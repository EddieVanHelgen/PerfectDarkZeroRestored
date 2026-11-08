// Native renderer (stage 1): the surface registry, Clear, Resolve and the de-rez snapshot readback
// (native_surfaces.h). Also hooks D3DDevice_InsertCallback (sub_826DE910, not hooked by the shadow files).
//
// Host formats (CORE_NOTES.md has the table): A8R8G8B8 -> R8G8B8A8_UNORM; A16B16G16R16_EDRAM and the 7e3
// A2B10G10R10F_EDRAM -> R16G16B16A16_FLOAT (the -32..32 / 7e3 ranges kept as real values, no shader-side
// scaling); R32F -> R32_FLOAT; D24FS8 -> D32_FLOAT_S8X24_UINT; D24S8 -> D24_UNORM_S8_UINT (the DSV formats: the
// depth resources are R32G8X24_TYPELESS / R24G8_TYPELESS so that the resolve SRV may read the depth plane).
#include "native_surfaces.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <iterator>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>

#include "native.h"
#include "native_blend7e3.h"
#include "native_passes.h"
#include "native_draw.h"
#include "native_passdump.h"
#include "native_recthread.h"
#include "native_textures.h"
#include "native_view.h"
#include "../frame_timing.h"
#include "../gpu_census.h"
#include "../gpu_sync.h"

REX_EXTERN(__imp__sub_826DE910); // D3DDevice_InsertCallback
REX_EXTERN(__imp__sub_825F66E0); // DeResThread's snapshot read (pdz_native_async_readback)

REXCVAR_DEFINE_BOOL(pdz_native_async_readback, false, "Render",
                    "Native renderer, graphics_backend null: the de-rez snapshot readback without a GPU wait. "
                    "InsertCallback records the copy and submits it; the bytes are written into guest memory when the "
                    "GPU is done (at a native frame end), and at the latest by DeResThread just before it reads them "
                    "(sub_825F66E0 waits for the copy, at most 2 s). Off = the synchronous readback (submit and wait "
                    "inside InsertCallback: the game thread, or the record thread, stalls until the GPU has caught "
                    "up). The plugin path and other callbacks always read back synchronously.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_native_fixed16_clamp, true, "Render",
                    "Native renderer: a resolve from a k_16_16 / k_16_16_16_16 colour target (16-bit fixed point, "
                    "-32..32 in the EDRAM: the light buffer) clamps to that range and reads NaN as 0, like the 7e3 "
                    "clamp. The host target is float16 and keeps values the guest saturates (light-buffer specular "
                    "alpha up to 323 natively against 32 emulated, pass dumps m2_run1 / run13). Off: copied as is.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(pdz_native_scale_fill_half_pixel, true, "Render",
                    "Native renderer, pdz_native_scale > 1 only: fill the half-pixel-offset gap of every resolve as "
                    "the SDK does (its resolve_resolution_scale_fill_half_pixel_offset, default on): with "
                    "PA_SU_VTX_CNTL.pix_center = kD3DZero the first scale / 2 host columns and rows of the resolve "
                    "area take the values of the next one. No effect at scale 1.");
REXCVAR_DEFINE_BOOL(pdz_native_depth_stencil_alias, true, "Render",
                    "Native renderer: a depth resolve whose memory the game also reads as a k_8_8_8_8 texture (same "
                    "base, size, pitch and tiling; noticed at the first such fetch, Textures::Lookup) is also packed "
                    "into a k_8_8_8_8 mirror of that key: the EDRAM word depth24 << 8 | stencil, bytes 0..3 in "
                    "components 0..3, what the hardware and the GPU plugin leave in memory. The vision modes read "
                    "the scene's stencil that way (an A8R8G8B8 header over the 1216x640 depth texture, stencil in "
                    ".z): the RCP-90 threat detector's red outline (post pass sub_825FA640, PS 3a499d914a59453a) and "
                    "the stencil masks of modes 1, 7-9, 11 and 12. Without it the fetch reads guest memory, which "
                    "native resolves never write: no outline with graphics_backend null "
                    "(stage2/polish/rcp90.md). Off: as before.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace pdz::native
{
  namespace
  {
    using pdz::shadow::dev::kDepthStencil;
    using pdz::shadow::dev::kRenderTargets;
    using pdz::shadow::dev::kViewport;
    // Device register shadow (shadow_record.h dev::kRun2000 = +11456, dev::kRun2300 = +11768; offsets = run base
    // + 4 * (register - first), register numbers from the SDK's register_table.inc).
    constexpr uint32_t kRB_COLOR_INFO = 11460;      // 0x2001 (RT0; RT1..3 = 0x2003..0x2005 at +11468..+11476)
    constexpr uint32_t kRB_DEPTH_INFO = 11464;      // 0x2002 (bit 16: 1 = D24FS8)
    constexpr uint32_t kRB_COLOR1_INFO = 11468;     // 0x2003
    constexpr uint32_t kPA_SU_VTX_CNTL = 11776;     // 0x2302: bit 0 pix_center (0 = kD3DZero, the D3D9 half pixel)
    static_assert(kPA_SU_VTX_CNTL == pdz::shadow::dev::kRun2300 + 4 * (0x2302 - 0x2300));
    constexpr uint32_t kRB_COPY_CONTROL = 11864;    // 0x2318: 0..2 source (4 = depth), 8 colour clear, 9 depth clear
    constexpr uint32_t kRB_COPY_DEST_INFO = 11876;  // 0x231B
    constexpr uint32_t kRB_DEPTH_CLEAR = 11884;     // 0x231D: depth << 8 | stencil
    constexpr uint32_t kRB_COLOR_CLEAR = 11888;     // 0x231E: the whole value (32bpp) or the upper 32 bits (64bpp)
    constexpr uint32_t kRB_COLOR_CLEAR_LO = 11892;  // 0x231F: the lower 32 bits (64bpp)

    // Resolve clear values (packed by D3D in the EDRAM format) to the host surfaces' values, after the SDK's
    // D3D12RenderTargetCache resolve clear (render_target_cache.cpp, BSD-3). The host surfaces keep real values
    // (the -32..32 fixed 16-bit and the 7e3 ranges included), so the 16-bit and 7e3 formats are decoded to them.
    float HalfToFloat(uint32_t h)
    {
      // Xenos 16-bit float: s1e5m10 with no infinity / NaN (exponent 31 is a normal number, up to 131008).
      const uint32_t e = (h >> 10) & 0x1F, m = h & 0x3FF;
      const float v = e ? std::ldexp(float(m | 0x400), int(e) - 25) : std::ldexp(float(m), -24);
      return (h & 0x8000) ? -v : v;
    }
    float Fixed16ToFloat(uint32_t h) // k_16_16 / k_16_16_16_16: signed, -32..32
    {
      return float(std::max<int32_t>(int16_t(uint16_t(h)), -32767)) * (32.0f / 32767.0f);
    }
    float Float7e3ToFloat(uint32_t f10)
    {
      const uint32_t e = (f10 >> 7) & 7, m = f10 & 0x7F;
      return e ? std::ldexp(float(m | 0x80), int(e) - 10) : std::ldexp(float(m), -9);
    }
    float Float20e4ToFloat(uint32_t f24)
    {
      const uint32_t e = (f24 >> 20) & 0xF, m = f24 & 0xFFFFF;
      return float(e ? std::ldexp(double(m | 0x100000), int(e) - 35) : std::ldexp(double(m), -34));
    }
    // color_format: xenos::ColorRenderTargetFormat of the cleared target (RB_COLOR_INFO bits 16..19).
    void DecodeColorClear(uint32_t color_format, uint32_t hi, uint32_t lo, float rgba[4])
    {
      rgba[0] = rgba[1] = rgba[2] = rgba[3] = 0.0f;
      switch (color_format)
      {
      case 2: case 10: // 2_10_10_10 (_AS_10_10_10_10)
        for (uint32_t j = 0; j < 3; ++j)
        {
          rgba[j] = float((hi >> (10 * j)) & 0x3FF) / 1023.0f;
        }
        rgba[3] = float(hi >> 30) / 3.0f;
        break;
      case 3: case 12: // 2_10_10_10_FLOAT (_AS_16_16_16_16): 7e3
        for (uint32_t j = 0; j < 3; ++j)
        {
          rgba[j] = Float7e3ToFloat((hi >> (10 * j)) & 0x3FF);
        }
        rgba[3] = float(hi >> 30) / 3.0f;
        break;
      case 4: // 16_16
        rgba[0] = Fixed16ToFloat(hi & 0xFFFF);
        rgba[1] = Fixed16ToFloat(hi >> 16);
        break;
      case 6: // 16_16_FLOAT
        rgba[0] = HalfToFloat(hi & 0xFFFF);
        rgba[1] = HalfToFloat(hi >> 16);
        break;
      case 5: // 16_16_16_16 (64bpp: components 0, 1 in the lower word)
        rgba[0] = Fixed16ToFloat(lo & 0xFFFF);
        rgba[1] = Fixed16ToFloat(lo >> 16);
        rgba[2] = Fixed16ToFloat(hi & 0xFFFF);
        rgba[3] = Fixed16ToFloat(hi >> 16);
        break;
      case 7: // 16_16_16_16_FLOAT
        rgba[0] = HalfToFloat(lo & 0xFFFF);
        rgba[1] = HalfToFloat(lo >> 16);
        rgba[2] = HalfToFloat(hi & 0xFFFF);
        rgba[3] = HalfToFloat(hi >> 16);
        break;
      case 14: // 32_FLOAT
        rgba[0] = BitsToFloat(hi);
        break;
      case 15: // 32_32_FLOAT
        rgba[0] = BitsToFloat(lo);
        rgba[1] = BitsToFloat(hi);
        break;
      default: // 0 8_8_8_8, 1 8_8_8_8_GAMMA (stored as is in the host R8G8B8A8, like Clear's D3DCOLOR)
        for (uint32_t j = 0; j < 4; ++j)
        {
          rgba[j] = float((hi >> (8 * j)) & 0xFF) / 255.0f;
        }
        break;
      }
    }

    struct FormatMap
    {
      DXGI_FORMAT resource, view, srv;
      bool depth;
    };
    FormatMap MapSurfaceFormat(uint32_t tex_format, bool &supported)
    {
      supported = true;
      switch (tex_format)
      {
      case 6: case 14: case 50: case 62: // 8_8_8_8 (+GAMMA EDRAM)
        return {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, false};
      case 7: case 54: // 2_10_10_10
        return {DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM, false};
      case 13: case 31: // 16_16 EDRAM / float
        return {DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R16G16_FLOAT, false};
      case 21: case 32: case 63: // 16_16_16_16 EDRAM, 16F x4, 7e3
        return {DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT, false};
      case 36: // 32_FLOAT
        return {DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT, false};
      // Depth: a TYPELESS resource, typed DSV, depth-plane SRV. A view format that differs from the resource format
      // is only valid on a TYPELESS resource (REVIEW2.md C4: an SRV on a D24_UNORM_S8_UINT resource is rejected by
      // the debug layer and undefined on strict drivers).
      case 22: // 24_8 (D24S8)
        return {DXGI_FORMAT_R24G8_TYPELESS, DXGI_FORMAT_D24_UNORM_S8_UINT, DXGI_FORMAT_R24_UNORM_X8_TYPELESS, true};
      case 23: // 24_8_FLOAT (D24FS8)
        return {DXGI_FORMAT_R32G8X24_TYPELESS, DXGI_FORMAT_D32_FLOAT_S8X24_UINT, DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS,
                true};
      default:
        supported = false;
        return {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, false};
      }
    }

    struct LastResolve
    {
      bool valid = false;
      uint64_t frame = 0;
      FetchDecoded dest;
      HostTexture *mirror = nullptr;
    };

    // pdz_native_scale > 1: the SDK's half-pixel offset fill of a resolve (draw_util::GetResolveInfo sets
    // fill_half_pixel_offset with a resolution scale, resolve_resolution_scale_fill_half_pixel_offset and
    // PA_SU_VTX_CNTL.pix_center == kD3DZero; the resolve shaders then read dst(x, y) = src(max(x, fx), max(y, fy))
    // relative to the resolve area, fx = fy = scale / 2). The half-pixel offset moves geometry that covers whole
    // guest pixels from the area's left / top edge by half a guest pixel, which at a scale of 2 or more leaves the
    // first host column / row uncovered; the fill copies the first surely covered one over it. `copy` is the main
    // resolve blit (host pixels, already recorded); the fill is extra copies through the same pass and conversion
    // (rows above fy, columns left of fx, the corner), so the main copy, its shader and scale 1 are unchanged.
    // fx / fy = 0 skip an axis (an area origin off the 8-pixel resolve grid: the SDK's area starts left / above
    // the copy, its fill lies outside it).
    void FillHalfPixelOffset(NativeDevice &dev, const passes::BlitDesc &copy, uint32_t fx, uint32_t fy)
    {
      if ((!fx && !fy) || copy.width <= fx || copy.height <= fy)
      {
        return;
      }
      for (uint32_t y = 0; y < fy; ++y) // rows above fy, from column fx on: source row fy
      {
        passes::BlitDesc b = copy;
        b.src_x = copy.src_x + int32_t(fx);
        b.src_y = copy.src_y + int32_t(fy);
        b.dst_x = copy.dst_x + int32_t(fx);
        b.dst_y = copy.dst_y + int32_t(y);
        b.width = copy.width - fx;
        b.height = 1;
        passes::Blit(dev, b);
      }
      for (uint32_t x = 0; x < fx; ++x) // columns left of fx, from row fy on: source column fx
      {
        passes::BlitDesc b = copy;
        b.src_x = copy.src_x + int32_t(fx);
        b.src_y = copy.src_y + int32_t(fy);
        b.dst_x = copy.dst_x + int32_t(x);
        b.dst_y = copy.dst_y + int32_t(fy);
        b.width = 1;
        b.height = copy.height - fy;
        passes::Blit(dev, b);
      }
      for (uint32_t y = 0; y < fy; ++y) // the corner: source (fx, fy)
      {
        for (uint32_t x = 0; x < fx; ++x)
        {
          passes::BlitDesc b = copy;
          b.src_x = copy.src_x + int32_t(fx);
          b.src_y = copy.src_y + int32_t(fy);
          b.dst_x = copy.dst_x + int32_t(x);
          b.dst_y = copy.dst_y + int32_t(y);
          b.width = 1;
          b.height = 1;
          passes::Blit(dev, b);
        }
      }
    }
  } // namespace

  struct Surfaces::Impl
  {
    std::unordered_map<uint32_t, HostSurface *> surfaces;
    LastResolve last_resolve;
    ID3D12Resource *readback = nullptr;
    uint64_t readback_size = 0;
    bool unsupported_logged = false;
    // EDRAM aliasing
    uint64_t stamp = 0;
    // Surfaces the game re-created at the same address (Lookup): their resource is queued with ReleaseLater at once;
    // the object and its descriptors are freed by EndFrame (bound sets of the current call may still point at it).
    std::vector<HostSurface *> retired;
    uint32_t recreated_logged = 0;
    HostSurface *last_bound[5] = {};
    uint64_t inherit_copies = 0, inherit_skipped = 0;
    uint32_t inherit_logged = 0;
    uint32_t split_skip_logged = 0; // split screen: stacked colour overlaps logged (InheritEdram), pairs below
    uint64_t split_skip_pairs[8] = {};
    // Resolve clears (review 2 C1): the first clear of each call site is logged (up to 16 sites), and the first
    // few disagreements between the call's flags and RB_COPY_CONTROL.
    uint32_t clear_sites[16] = {};
    uint32_t clear_site_count = 0;
    uint32_t clear_mismatch_logged = 0;
    uint32_t scale_mismatch_logged = 0; // pdz_shadow_map_scale: a resolve whose source and mirror scales differ
    uint32_t bound_scale_logged = 0;    // pdz_shadow_map_scale: bound targets of different scales
    // pdz_native_scale > 1: resolves given the half-pixel offset fill (FillHalfPixelOffset; the first is logged).
    uint64_t fill_resolves = 0;
    // Resolves of 7e3 targets rounded to 7e3 (pdz_native_7e3_resolve_round / pdz_native_7e3_blend 2; the first is
    // logged).
    uint64_t round_7e3_resolves = 0;
    // Depth resolves also packed into a k_8_8_8_8 mirror (pdz_native_depth_stencil_alias; the first is logged).
    uint64_t stencil_packs = 0;
    // Eviction (EndFrame, REVIEW2.md C3).
    uint64_t last_eviction_frame = 0;
    uint64_t evicted_total = 0, retired_freed = 0;
    uint64_t descriptor_failures = 0;
    uint32_t evict_logged = 0;
    // pdz_native_state_filter (stage 2 M5 drawcost3): the render targets BindTargets last set on the list (as of
    // om_epoch): views and their resources (a view slot freed and reused within the epoch belongs to a new resource;
    // a released resource's address cannot come back before its frame's fence, a later epoch).
    bool om_valid = false;
    uint64_t om_epoch = 0;
    uint32_t om_count = 0;
    SIZE_T om_rtv[4] = {};
    ID3D12Resource *om_rtv_resource[4] = {};
    SIZE_T om_dsv = 0;
    ID3D12Resource *om_dsv_resource = nullptr;
    uint64_t om_set = 0, om_skipped = 0;
  };

  namespace
  {
    // EDRAM tiles a surface covers: 80x16-sample tiles of 32bpp (40x16 for 64bpp colour formats), no MSAA.
    bool Is64bpp(const HostSurface &s)
    {
      return !s.depth && (s.color_format == 5 || s.color_format == 7 || s.color_format == 15);
    }
    uint32_t TilePitch(const HostSurface &s) { return (s.width * (Is64bpp(s) ? 2u : 1u) + 79) / 80; }
    uint32_t TileEnd(const HostSurface &s) { return s.edram_base + TilePitch(s) * ((s.height + 15) / 16); }
  } // namespace

  void Surfaces::InheritEdram(HostSurface &s)
  {
    Impl &im = *impl_;
    NativeDevice *dev = NativeDevice::Get();
    if (!dev || !dev->in_frame())
    {
      return;
    }
    // Newest other writer of overlapping tiles.
    HostSurface *newest = nullptr;
    const uint32_t begin = s.edram_base, end = TileEnd(s);
    for (auto &[guest, o] : im.surfaces)
    {
      if (o == &s || o->write_stamp <= s.write_stamp || o->edram_base >= end || TileEnd(*o) <= begin)
      {
        continue;
      }
      if (!newest || o->write_stamp > newest->write_stamp)
      {
        newest = o;
      }
    }
    s.write_stamp = ++im.stamp;
    if (!newest)
    {
      return;
    }
    if (s.depth || newest->depth || newest->edram_base != s.edram_base || TilePitch(*newest) != TilePitch(s) ||
        newest->resource_format != s.resource_format || newest->scale != s.scale)
    {
      ++im.inherit_skipped;
      // Split screen (stage1/subagents/split.md, pad2.md risk 4): views stacked in EDRAM (a colour target of the same
      // pitch and format at another base, i.e. rows further down) are not inherited: the surface keeps its own host
      // content there. If a half of the picture comes out black or stale, these lines say which overlap the game
      // relied on. Multi-pass frames only, each pair once, 8 at most.
      if (!s.depth && !newest->depth && newest->edram_base != s.edram_base && TilePitch(*newest) == TilePitch(s) &&
          newest->resource_format == s.resource_format && newest->scale == s.scale && im.split_skip_logged < 8 &&
          SplitActive())
      {
        const uint64_t pair = (uint64_t(s.guest) << 32) | newest->guest;
        if (std::find(im.split_skip_pairs, im.split_skip_pairs + im.split_skip_logged, pair) ==
            im.split_skip_pairs + im.split_skip_logged)
        {
          im.split_skip_pairs[im.split_skip_logged++] = pair;
          REXLOG_INFO("pdz_native: split screen: EDRAM overlap not inherited: {:08X} ({}x{} colour fmt {} base {}) over "
                      "{:08X} ({}x{} colour fmt {} base {}), written later, same pitch {} tiles: {} rows apart; only "
                      "same-base colour surfaces inherit (a view stacked in EDRAM keeps the surface's own content)",
                      s.guest, s.width, s.height, s.color_format, s.edram_base, newest->guest, newest->width,
                      newest->height, newest->color_format, newest->edram_base, TilePitch(s),
                      (int32_t(newest->edram_base) - int32_t(s.edram_base)) / int32_t(std::max(TilePitch(s), 1u)) * 16);
        }
      }
      return;
    }
    const UINT w = std::min(newest->host_width, s.host_width), h = std::min(newest->host_height, s.host_height);
    Transition(*newest, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Transition(s, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = s.resource;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = newest->resource;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    const D3D12_BOX box{0, 0, 0, w, h, 1};
    dev->list()->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
    ++im.inherit_copies;
    if (im.inherit_logged < 4)
    {
      ++im.inherit_logged;
      REXLOG_INFO("pdz_native: EDRAM alias: {:08X} ({}x{}) inherits {}x{} from {:08X} at EDRAM base {}", s.guest, s.width,
                  s.height, w / s.scale, h / s.scale, newest->guest, s.edram_base);
    }
    if (FrameTraceActive())
    {
      REXLOG_INFO("pdz_trace: edram inherit {:08X} <- {:08X} base {} {}x{}", s.guest, newest->guest, s.edram_base, w, h);
    }
  }

  Surfaces::Surfaces() : impl_(new Impl()) {}

  Surfaces &Surfaces::Get()
  {
    static Surfaces instance;
    return instance;
  }

  HostSurface *Surfaces::Lookup(uint8_t *base, uint32_t guest)
  {
    if (!IsGuestPointer(guest))
    {
      return nullptr;
    }
    Impl &im = *impl_;
    auto it = im.surfaces.find(guest);
    if (it != im.surfaces.end())
    {
      // The game can free a surface and create another at the same address with another size / format / EDRAM
      // base (review finding 5): the header is re-checked and a changed surface gets a new host resource.
      HostSurface *cached = it->second;
      // pdz_native_record_thread: the header as the hook copied it (native_view.h); guest memory otherwise.
      const uint32_t d1 = view::Load32(base, guest + 20), d2 = view::Load32(base, guest + 24),
                     info = view::Load32(base, guest + 52);
      // Only what defines the host resource: texture format, size and EDRAM base. D3D rewrites the format bits of
      // +52 when it rebinds a surface (the HDR target toggles 2_10_10_10_FLOAT / _AS_16_16_16_16, log 073: comparing
      // the whole word re-created it on every bind and lost its contents).
      if ((d1 & 0x3F) == cached->tex_format && (d2 & 0x1FFF) + 1 == cached->width &&
          ((d2 >> 13) & 0x1FFF) + 1 == cached->height && (info & 0xFFF) == (cached->info_word & 0xFFF))
      {
        // Every bind, clear, resolve and pass-dump lookup counts as a use (EndFrame's eviction age).
        if (NativeDevice *dev = NativeDevice::Get())
        {
          cached->last_used_frame = dev->frame_index();
        }
        return cached;
      }
      if (im.recreated_logged++ < 8)
      {
        REXLOG_INFO("pdz_native: surface {:08X} re-created by the game ({}x{} fmt {} info {:08X} -> {}x{} fmt {} info "
                    "{:08X}); new host surface",
                    guest, cached->width, cached->height, cached->tex_format, cached->info_word, (d2 & 0x1FFF) + 1,
                    ((d2 >> 13) & 0x1FFF) + 1, d1 & 0x3F, info);
      }
      if (NativeDevice *dev = NativeDevice::Get(); dev && cached->resource)
      {
        dev->ReleaseLater(cached->resource);
        cached->resource = nullptr;
      }
      im.retired.push_back(cached); // other holders (bound sets) may still point at it this frame
      for (HostSurface *&b : im.last_bound)
      {
        b = nullptr;
      }
      im.surfaces.erase(it);
    }
    NativeDevice *dev = NativeDevice::Get();
    if (!dev)
    {
      return nullptr;
    }
    // Header written by CreateSurface (recomp.57.cpp sub_826ECE78).
    const uint32_t dword1 = view::Load32(base, guest + 20);
    const uint32_t dword2 = view::Load32(base, guest + 24);
    const uint32_t info = view::Load32(base, guest + 52);
    HostSurface *s = new HostSurface();
    s->guest = guest;
    s->tex_format = dword1 & 0x3F;
    s->width = (dword2 & 0x1FFF) + 1;
    s->height = ((dword2 >> 13) & 0x1FFF) + 1;
    s->info_word = info;
    s->edram_base = info & 0xFFF;
    bool supported;
    const FormatMap fm = MapSurfaceFormat(s->tex_format, supported);
    s->depth = fm.depth;
    if (s->depth)
    {
      s->depth_format = (info >> 16) & 1;
    }
    else
    {
      s->color_format = (info >> 16) & 0xF;
    }
    if (!supported && !im.unsupported_logged)
    {
      im.unsupported_logged = true;
      REXLOG_WARN("pdz_native: surface {:08X} has texture format {} (unmapped); using R8G8B8A8", guest, s->tex_format);
    }
    s->scale = SurfaceScale(s->width, s->height); // Scale(); the shadow maps x pdz_shadow_map_scale
    s->host_width = s->width * s->scale;
    s->host_height = s->height * s->scale;
    s->resource_format = fm.resource;
    s->view_format = fm.view;
    s->srv_format = fm.srv;
    s->last_used_frame = dev->frame_index();

    // Descriptors first (REVIEW2.md C3): a full heap refuses the surface (callers treat it as unbound) before a
    // resource is created for it, so a refusal costs no allocation and no live slot is shared.
    s->view = s->depth ? dev->AllocDsv() : dev->AllocRtv();
    s->srv = s->view.ptr ? dev->AllocStagingSrv() : D3D12_CPU_DESCRIPTOR_HANDLE{};
    if (!s->view.ptr || !s->srv.ptr)
    {
      if (im.descriptor_failures++ < 8)
      {
        REXLOG_ERROR("pdz_native: surface {:08X} ({}x{} {}) skipped: no {} descriptor left ({} live surfaces)", guest,
                     s->width, s->height, s->depth ? "depth" : "colour",
                     !s->view.ptr ? (s->depth ? "DSV" : "RTV") : "staging SRV", im.surfaces.size());
      }
      if (s->depth)
      {
        dev->FreeDsv(s->view);
      }
      else
      {
        dev->FreeRtv(s->view);
      }
      delete s;
      return nullptr;
    }

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = s->host_width;
    desc.Height = s->host_height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = fm.resource;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = s->depth ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL : D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    // 7e3 colour surface (k_2_10_10_10_FLOAT / _AS_16_16_16_16) with pdz_native_7e3_blend 2: UAV-capable, so the 7e3
    // blend variant can bind it as a rasterizer-ordered view (native_blend7e3.h).
    if (!s->depth && (s->color_format == 3 || s->color_format == 12) && fm.resource == DXGI_FORMAT_R16G16B16A16_FLOAT &&
        blend7e3::WantUavSurfaces())
    {
      desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
      s->uav_7e3 = true;
    }
    D3D12_CLEAR_VALUE clear{};
    clear.Format = fm.view;
    if (s->depth)
    {
      clear.DepthStencil.Depth = 1.0f;
    }
    const D3D12_RESOURCE_STATES initial =
        s->depth ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_RENDER_TARGET;
    s->resource = dev->CreateTexture(desc, initial, &clear, s->depth ? "pdz_native depth surface" : "pdz_native colour surface");
    s->state = initial;
    if (!s->resource)
    {
      if (s->depth)
      {
        dev->FreeDsv(s->view);
      }
      else
      {
        dev->FreeRtv(s->view);
      }
      dev->FreeStagingSrv(s->srv);
      delete s;
      return nullptr;
    }
    if (s->depth)
    {
      D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
      dsv.Format = fm.view;
      dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
      dev->device()->CreateDepthStencilView(s->resource, &dsv, s->view);
    }
    else
    {
      D3D12_RENDER_TARGET_VIEW_DESC rtv{};
      rtv.Format = fm.view;
      rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
      dev->device()->CreateRenderTargetView(s->resource, &rtv, s->view);
      // pdz_debug_poison_new_resources: a new colour surface starts magenta instead of zero.
      if (PoisonNewResources() && dev->list())
      {
        const FLOAT magenta[4] = {1.0f, 0.0f, 1.0f, 1.0f};
        dev->list()->ClearRenderTargetView(s->view, magenta, 0, nullptr);
      }
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = fm.srv;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    dev->device()->CreateShaderResourceView(s->resource, &srv, s->srv);
    im.surfaces.emplace(guest, s);
    REXLOG_INFO("pdz_native: surface {:08X}: {}x{} texture format {} {} (format {}, EDRAM base {}) -> host {}x{} DXGI {}{}",
                guest, s->width, s->height, s->tex_format, s->depth ? "depth" : "colour",
                s->depth ? s->depth_format : s->color_format, s->edram_base, s->host_width, s->host_height,
                uint32_t(fm.view), // the typed format, as before the TYPELESS depth resources (same log line)
                s->uav_7e3 ? " (UAV-capable: 7e3 blend mode 2)" : "");
    return s;
  }

  Surfaces::Bound Surfaces::GetBound(uint8_t *base, uint32_t device)
  {
    Bound b;
    if (!IsGuestPointer(device))
    {
      return b;
    }
    for (uint32_t i = 0; i < 4; ++i)
    {
      b.rt[i] = Lookup(base, view::Load32(base, device + kRenderTargets + 4 * i)); // the record thread's mirror
    }
    b.ds = Lookup(base, view::Load32(base, device + kDepthStencil));
    return b;
  }

  void Surfaces::Transition(HostSurface &s, D3D12_RESOURCE_STATES state)
  {
    NativeDevice *dev = NativeDevice::Get();
    if (dev && dev->in_frame())
    {
      dev->Transition(s.resource, s.state, state);
    }
  }

  void Surfaces::SyncBoundSet(const Bound &b)
  {
    // EDRAM aliasing: only when the bound set changes (the draws of one pass keep owning their tiles). Shared by
    // BindTargets, Clear and Resolve (review 2 C2), so whichever of them first sees a new set runs the inherit.
    Impl &im = *impl_;
    HostSurface *const now[5] = {b.rt[0], b.rt[1], b.rt[2], b.rt[3], b.ds};
    if (std::equal(now, now + 5, im.last_bound))
    {
      return;
    }
    std::copy(now, now + 5, im.last_bound);
    for (HostSurface *s : now)
    {
      if (s)
      {
        InheritEdram(*s);
      }
    }
  }

  void Surfaces::ForgetBoundTargets()
  {
    if (impl_)
    {
      impl_->om_valid = false;
    }
  }

  void Surfaces::TargetFilterCounts(uint64_t &set, uint64_t &skipped) const
  {
    set = impl_ ? impl_->om_set : 0;
    skipped = impl_ ? impl_->om_skipped : 0;
  }

  namespace
  {
    // pdz_native_gpu_passes: the pass kind of the bound targets (the first bound surface decides: a 1344 x 1344
    // shadow map, a target at least the 1280-wide back buffer, or a smaller one).
    GpuPass PassKind(const Surfaces::Bound &b, GpuPass shadow, GpuPass main, GpuPass other)
    {
      const HostSurface *first = b.ds;
      for (uint32_t i = 0; i < 4 && !first; ++i)
      {
        first = b.rt[i];
      }
      for (uint32_t i = 0; i < 4 && b.ds; ++i)
      {
        if (b.rt[i])
        {
          first = b.rt[i]; // colour decides when both are bound (the shadow pass binds R32F colour + depth)
          break;
        }
      }
      if (!first)
      {
        return other;
      }
      if (first->width == 1344 && first->height == 1344)
      {
        return shadow;
      }
      return first->width >= 1280 ? main : other;
    }

    // pdz_native_gpu_passes: one frame's bind / clear / resolve sequence (about 20 s after the first shadow-map
    // pass), so the pass totals can be read against what the game does.
    uint64_t g_trace_frame = 0;
    uint32_t g_trace_lines = 0;
    std::string TraceTargets(const Surfaces::Bound &b)
    {
      std::string out;
      for (uint32_t i = 0; i < 4; ++i)
      {
        if (b.rt[i])
        {
          out += fmt::format("rt{} {:08X} {}x{} fmt {} base {} ", i, b.rt[i]->guest, b.rt[i]->width, b.rt[i]->height,
                             b.rt[i]->color_format, b.rt[i]->edram_base);
        }
      }
      if (b.ds)
      {
        out += fmt::format("ds {:08X} {}x{} base {}", b.ds->guest, b.ds->width, b.ds->height, b.ds->edram_base);
      }
      return out.empty() ? std::string("none") : out;
    }
    void TraceOp(NativeDevice &dev, GpuPass kind, const std::string &what)
    {
      if (kind == GpuPass::kShadowDraw && !g_trace_frame)
      {
        g_trace_frame = dev.frame_index() + 2400;
      }
      if (g_trace_frame && dev.frame_index() == g_trace_frame && g_trace_lines < 160)
      {
        ++g_trace_lines;
        REXLOG_INFO("pdz_native: gpu pass trace frame {}: {}", g_trace_frame, what);
      }
    }
  } // namespace

  uint32_t Surfaces::BindTargets(uint8_t *base, uint32_t device, Bound *bound_out)
  {
    NativeDevice *dev = NativeDevice::Get();
    if (!dev || !dev->in_frame())
    {
      return 0;
    }
    Bound b = GetBound(base, device);
    SyncBoundSet(b);
    D3D12_CPU_DESCRIPTOR_HANDLE rtvs[4]{};
    uint32_t count = 0;
    for (uint32_t i = 0; i < 4; ++i)
    {
      if (b.rt[i])
      {
        Transition(*b.rt[i], D3D12_RESOURCE_STATE_RENDER_TARGET);
        rtvs[i] = b.rt[i]->view;
        count = i + 1;
        b.rt[i]->last_used_frame = dev->frame_index();
      }
    }
    if (b.ds)
    {
      Transition(*b.ds, D3D12_RESOURCE_STATE_DEPTH_WRITE);
      b.ds->last_used_frame = dev->frame_index();
    }
    {
      const pdz::timing::PhaseScope phase(pdz::timing::Phase::kDrawOmSet, 2); // pdz_frame_timing_phases 2 (drawcost)
      // pdz_native_state_filter (drawcost3): the same views of the same resources on the same list are not set again.
      Impl &im = *impl_;
      bool same = REXCVAR_GET(pdz_native_state_filter) && im.om_valid && im.om_epoch == dev->list_epoch() &&
                  im.om_count == count && im.om_dsv == (b.ds ? b.ds->view.ptr : 0) &&
                  im.om_dsv_resource == (b.ds ? b.ds->resource : nullptr);
      for (uint32_t i = 0; same && i < count; ++i)
      {
        same = im.om_rtv[i] == rtvs[i].ptr && im.om_rtv_resource[i] == (b.rt[i] ? b.rt[i]->resource : nullptr);
      }
      if (same)
      {
        ++im.om_skipped;
      }
      else
      {
        dev->list()->OMSetRenderTargets(count, count ? rtvs : nullptr, FALSE, b.ds ? &b.ds->view : nullptr);
        ++im.om_set;
        if (dev->GpuPassesOn())
        {
          const GpuPass kind = PassKind(b, GpuPass::kShadowDraw, GpuPass::kMainDraw, GpuPass::kOtherDraw);
          dev->GpuMark(kind);
          TraceOp(*dev, kind, "bind " + TraceTargets(b));
        }
        im.om_valid = true;
        im.om_epoch = dev->list_epoch();
        im.om_count = count;
        for (uint32_t i = 0; i < 4; ++i)
        {
          im.om_rtv[i] = i < count ? rtvs[i].ptr : 0;
          im.om_rtv_resource[i] = i < count && b.rt[i] ? b.rt[i]->resource : nullptr;
        }
        im.om_dsv = b.ds ? b.ds->view.ptr : 0;
        im.om_dsv_resource = b.ds ? b.ds->resource : nullptr;
        // pdz_shadow_map_scale > 1 only (every surface has Scale() otherwise): a 1344 x 1344 shadow surface bound
        // with a surface of another scale would clip the draws to the smaller host size. Not expected (the shadow
        // pass binds its 1344 x 1344 depth and R32F colour surfaces together); logged.
        const HostSurface *first = b.ds;
        for (uint32_t i = 0; i < count && !first; ++i)
        {
          first = b.rt[i];
        }
        for (uint32_t i = 0; first && i < count && im.bound_scale_logged < 4; ++i)
        {
          if (b.rt[i] && b.rt[i]->scale != first->scale)
          {
            ++im.bound_scale_logged;
            REXLOG_WARN("pdz_native: bound targets of different scales (pdz_shadow_map_scale): rt{} {:08X} {}x{} scale "
                        "{}, {:08X} {}x{} scale {}",
                        i, b.rt[i]->guest, b.rt[i]->width, b.rt[i]->height, b.rt[i]->scale, first->guest, first->width,
                        first->height, first->scale);
          }
        }
      }
    }
    if (bound_out)
    {
      *bound_out = b;
    }
    return count;
  }

  // ---------------------------------------------------------------------------
  // Clear: D3DCLEAR_TARGET0..3 = 0x1..0x8, D3DCLEAR_ZBUFFER = 0x10, D3DCLEAR_STENCIL = 0x20 (game flags seen:
  // 0x3F, 0xF, 0x30, 0x11, 0x1). Rects: `count` D3DRECTs (x1, y1, x2, y2) or, with none, the viewport.
  // ---------------------------------------------------------------------------
  void Surfaces::Clear(uint8_t *base, uint32_t device, uint32_t count, uint32_t rects, uint32_t flags, uint32_t color,
                       uint32_t z_bits, uint32_t stencil)
  {
    NativeDevice *dev = NativeDevice::Get();
    if (!dev || !dev->in_frame() || !IsGuestPointer(device))
    {
      return;
    }
    Bound b = GetBound(base, device);
    SyncBoundSet(b); // a clear right after SetRenderTarget keeps the inherited content outside its rects
    if (FrameTraceActive())
    {
      auto surf = [](const HostSurface *h) {
        return h ? TraceSurface(h->guest, h->width, h->height, h->depth ? h->depth_format : h->color_format,
                                h->edram_base, h->depth)
                 : std::string("-");
      };
      REXLOG_INFO("pdz_trace: clear flags {:X} count {} colour {:08X} z {:.4f} stencil {} | rt0 {} ds {}", flags, count,
                  color, BitsToFloat(z_bits), stencil, surf(b.rt[0]), surf(b.ds));
    }
    passdump::BeforeClear(b, flags, color, BitsToFloat(z_bits), stencil);

    // The rects in guest pixels; each cleared surface scales them by its own scale (clamp_rects): the shadow maps can
    // have another one (pdz_shadow_map_scale, SurfaceScale), every other surface has Scale().
    std::vector<D3D12_RECT> guest_rects;
    if (count && IsGuestPointer(rects))
    {
      for (uint32_t i = 0; i < std::min(count, 16u); ++i)
      {
        const uint32_t r = rects + 16 * i;
        guest_rects.push_back({LONG(view::LoadS32(base, r)), LONG(view::LoadS32(base, r + 4)),
                               LONG(view::LoadS32(base, r + 8)), LONG(view::LoadS32(base, r + 12))});
      }
    }
    else
    {
      const int32_t vx = view::LoadS32(base, device + kViewport);
      const int32_t vy = view::LoadS32(base, device + kViewport + 4);
      const int32_t vw = view::LoadS32(base, device + kViewport + 8);
      const int32_t vh = view::LoadS32(base, device + kViewport + 12);
      if (vw > 0 && vh > 0)
      {
        guest_rects.push_back({LONG(vx), LONG(vy), LONG(vx + vw), LONG(vy + vh)});
      }
    }

    auto clamp_rects = [&](const HostSurface &s, std::vector<D3D12_RECT> &out) {
      out.clear();
      const LONG k = LONG(s.scale);
      for (const D3D12_RECT &g : guest_rects)
      {
        const D3D12_RECT r{g.left * k, g.top * k, g.right * k, g.bottom * k};
        D3D12_RECT c{std::max<LONG>(r.left, 0), std::max<LONG>(r.top, 0), std::min<LONG>(r.right, LONG(s.host_width)),
                     std::min<LONG>(r.bottom, LONG(s.host_height))};
        if (c.right > c.left && c.bottom > c.top)
        {
          out.push_back(c);
        }
      }
    };
    std::vector<D3D12_RECT> clamped;

    const float rgba[4] = {float((color >> 16) & 0xFF) / 255.0f, float((color >> 8) & 0xFF) / 255.0f,
                           float(color & 0xFF) / 255.0f, float((color >> 24) & 0xFF) / 255.0f};
    for (uint32_t i = 0; i < 4; ++i)
    {
      if ((flags & (1u << i)) && b.rt[i])
      {
        clamp_rects(*b.rt[i], clamped);
        if (clamped.empty() && !guest_rects.empty())
        {
          continue;
        }
        if (!clamped.empty())
        {
          InheritEdram(*b.rt[i]); // a partial clear keeps the rest of the inherited content
        }
        else
        {
          b.rt[i]->write_stamp = ++impl_->stamp;
        }
        Transition(*b.rt[i], D3D12_RESOURCE_STATE_RENDER_TARGET);
        dev->list()->ClearRenderTargetView(b.rt[i]->view, rgba, UINT(clamped.size()), clamped.empty() ? nullptr : clamped.data());
      }
    }
    const D3D12_CLEAR_FLAGS dflags = D3D12_CLEAR_FLAGS(((flags & 0x10) ? D3D12_CLEAR_FLAG_DEPTH : 0) |
                                                       ((flags & 0x20) ? D3D12_CLEAR_FLAG_STENCIL : 0));
    if (dflags && b.ds)
    {
      clamp_rects(*b.ds, clamped);
      if (!(clamped.empty() && !guest_rects.empty()))
      {
        b.ds->write_stamp = ++impl_->stamp;
        Transition(*b.ds, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        dev->list()->ClearDepthStencilView(b.ds->view, dflags, BitsToFloat(z_bits), UINT8(stencil & 0xFF),
                                           UINT(clamped.size()), clamped.empty() ? nullptr : clamped.data());
      }
    }
  }

  // ---------------------------------------------------------------------------
  // Resolve: flags 0..3 = RT index, 4 = depth-stencil, 0x10..0x70 fragments (no MSAA: nothing), 0x100 = clear
  // the colour source after the copy, 0x200 = clear the depth-stencil surface after the copy (D3DRESOLVE_*, the
  // RB_COPY_CONTROL layout). The clears follow RB_COPY_CONTROL and the clear-value registers D3D wrote inside
  // the call (review 2 C1); pClearColor (r10) and ClearZ are only logged.
  // ---------------------------------------------------------------------------
  void Surfaces::Resolve(uint8_t *base, uint32_t device, uint32_t flags, uint32_t src_rect, uint32_t dest_texture,
                         uint32_t dest_point, uint32_t level, uint32_t slice, uint32_t clear_color_ptr,
                         uint32_t z_bits, uint32_t call_site)
  {
    if (level != 0 || slice != 0)
    {
      // The mirror is one 2D level: such a resolve lands in level 0 (review finding 7). Logged to see whether PDZ
      // ever does it before changing the behaviour.
      static uint32_t logged = 0;
      if (logged++ < 8)
      {
        REXLOG_WARN("pdz_native: resolve into level {} slice {} (dest texture {:08X}): written to level 0 / slice 0",
                    level, slice, dest_texture);
      }
    }
    NativeDevice *dev = NativeDevice::Get();
    if (!dev || !dev->in_frame() || !IsGuestPointer(device) || !IsGuestPointer(dest_texture))
    {
      return;
    }
    Bound b = GetBound(base, device);
    SyncBoundSet(b); // a resolve right after SetRenderTarget reads the inherited EDRAM content (review 2 C2)
    if (FrameTraceActive())
    {
      const FetchDecoded dst = DecodeTextureObject(base, dest_texture);
      auto surf = [](const HostSurface *h) {
        return h ? TraceSurface(h->guest, h->width, h->height, h->depth ? h->depth_format : h->color_format,
                                h->edram_base, h->depth)
                 : std::string("-");
      };
      REXLOG_INFO("pdz_trace: resolve flags {:X} rt0 {} ds {} -> texture {:08X} at {:08X} fmt {} {}x{} rect {:08X} "
                  "point {:08X}",
                  flags, surf(b.rt[0]), surf(b.ds), dest_texture, dst.base_address, dst.format, dst.width, dst.height,
                  src_rect, dest_point);
    }
    const bool depth = (flags & 4) != 0;
    HostSurface *src = depth ? b.ds : b.rt[flags & 3];
    if (!src)
    {
      return;
    }
    // The resolve area in source pixels. The copy clips it to the destination; the clear does not.
    int32_t sx = 0, sy = 0, sw = int32_t(src->width), sh = int32_t(src->height);
    if (IsGuestPointer(src_rect))
    {
      const int32_t x1 = view::LoadS32(base, src_rect), y1 = view::LoadS32(base, src_rect + 4);
      const int32_t x2 = view::LoadS32(base, src_rect + 8), y2 = view::LoadS32(base, src_rect + 12);
      sx = x1;
      sy = y1;
      sw = x2 - x1;
      sh = y2 - y1;
    }
    Impl &im = *impl_;
    // The copy (a lambda only so that its early returns skip the copy and not the clear below).
    [&] {
    const FetchDecoded dst_fetch = DecodeTextureObject(base, dest_texture);
    if (!dst_fetch.base_address || dst_fetch.width == 0)
    {
      return;
    }
    HostTexture *dst = Textures::Get().GetMirror(dst_fetch);
    if (!dst)
    {
      return;
    }
    // Surface and mirror scales follow the guest size (SurfaceScale): they differ only with pdz_shadow_map_scale > 1
    // and a resolve between a 1344 x 1344 shadow map and something of another size, which PDZ is not known to do.
    if (dst->scale != src->scale && im.scale_mismatch_logged < 4)
    {
      ++im.scale_mismatch_logged;
      REXLOG_WARN("pdz_native: resolve {:08X} ({}x{}, scale {}) -> {:08X} ({}x{}, mirror scale {}): the scales differ "
                  "(pdz_shadow_map_scale); copied texel for texel",
                  src->guest, src->width, src->height, src->scale, dst_fetch.base_address, dst_fetch.width,
                  dst_fetch.height, dst->scale);
    }

    int32_t w = sw, h = sh;
    int32_t dx = 0, dy = 0;
    if (IsGuestPointer(dest_point))
    {
      dx = view::LoadS32(base, dest_point);
      dy = view::LoadS32(base, dest_point + 4);
    }
    w = std::min<int32_t>(w, std::min<int32_t>(int32_t(src->width) - sx, int32_t(dst_fetch.width) - dx));
    h = std::min<int32_t>(h, std::min<int32_t>(int32_t(src->height) - sy, int32_t(dst_fetch.height) - dy));
    if (w <= 0 || h <= 0 || sx < 0 || sy < 0 || dx < 0 || dy < 0)
    {
      return;
    }

    // RB_COPY_DEST_INFO as D3D wrote it inside the call (the hook calls us after forwarding).
    const uint32_t dest_info = view::Load32(base, device + kRB_COPY_DEST_INFO);
    const bool swap = ((dest_info >> 24) & 1) != 0;
    int32_t exp_bias = int32_t((dest_info >> 16) & 0x3F);
    if (exp_bias & 0x20)
    {
      exp_bias -= 0x40;
    }

    const uint32_t scale = src->scale;
    passes::BlitDesc blit;
    blit.dst = dst->resource;
    blit.dst_rtv = dst->rtv;
    blit.rtv_format = dst->resource_format;
    blit.src_srv = src->srv;
    blit.src_x = sx * int32_t(scale);
    blit.src_y = sy * int32_t(scale);
    blit.dst_x = dx * int32_t(dst->scale);
    blit.dst_y = dy * int32_t(dst->scale);
    blit.width = uint32_t(w) * scale;
    blit.height = uint32_t(h) * scale;
    if (depth)
    {
      // The resolve copies the EDRAM depth bits unconverted (the SDK resolves depth as the source's own k_24_8 /
      // k_24_8_FLOAT); the encoding follows the source surface (D24FS8 = 20e4, D24S8 = unorm24) and a k_24_8 /
      // k_24_8_FLOAT destination reads those bits back as unorm24 / 20e4 (review 15).
      const bool src_float24 = src->depth_format == 1;
      if (dst_fetch.format == 22 && src_float24)
      {
        blit.mode = passes::BlitMode::kDepth20e4AsUnorm24;
      }
      else if (dst_fetch.format == 23 && !src_float24)
      {
        blit.mode = passes::BlitMode::kDepthUnorm24As20e4;
      }
      else
      {
        blit.mode = src_float24 ? passes::BlitMode::kDepth20e4 : passes::BlitMode::kDepthUnorm24;
      }
    }
    else
    {
      blit.mode = passes::BlitMode::kCopy;
      // The swap puts the EDRAM red into data component 2 (A8R8G8B8-style destinations); only meaningful
      // for destinations with three or more components.
      const bool rgb_dest = !(dst_fetch.format == 2 || dst_fetch.format == 8 || dst_fetch.format == 9 ||
                              dst_fetch.format == 10 || dst_fetch.format == 24 || dst_fetch.format == 25 ||
                              dst_fetch.format == 27 || dst_fetch.format == 30 || dst_fetch.format == 36);
      blit.swap_rb = swap && rgb_dest;
      blit.exp_scale = std::ldexp(1.0f, exp_bias);
      // xenos::ColorRenderTargetFormat 3 = k_2_10_10_10_FLOAT, 12 = k_2_10_10_10_FLOAT_AS_16_16_16_16 (host float16:
      // values past 31.875 from additive passes are clamped like the 7e3 EDRAM storage would; Nightclub reached
      // 124.8 natively against 31.875 emulated, pass dump m2_run1).
      blit.source_7e3 = src->color_format == 3 || src->color_format == 12;
      // pdz_native_7e3_blend 2 or pdz_native_7e3_resolve_round: the copy also rounds to 7e3 (nearest even), what the
      // EDRAM holds after a non-blended write of the unrounded float16 value (in mode 2 the blended draws already store
      // rounded values). The exposure loop's state lives in these resolves (river.md).
      blit.round_7e3 = blit.source_7e3 && blend7e3::RoundResolves();
      if (blit.round_7e3 && im.round_7e3_resolves++ == 0)
      {
        REXLOG_INFO("pdz_native: resolves of 7e3 targets round to 7e3 (pdz_native_7e3_resolve_round {}, "
                    "pdz_native_7e3_blend {}); first: {:08X} -> {:08X} fmt {} area {},{} {}x{}",
                    REXCVAR_GET(pdz_native_7e3_resolve_round), blend7e3::CurrentMode(), src->guest,
                    dst_fetch.base_address, dst_fetch.format, sx, sy, w, h);
      }
      // 4 = k_16_16, 5 = k_16_16_16_16 (fixed point -32..32; host float16, pdz_native_fixed16_clamp).
      blit.source_fixed16 = REXCVAR_GET(pdz_native_fixed16_clamp) && (src->color_format == 4 || src->color_format == 5);
    }
    passdump::BeforeResolve(*src);
    Transition(*src, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Textures::Get().Transition(*dst, D3D12_RESOURCE_STATE_RENDER_TARGET);
    passes::Blit(*dev, blit);
    // pdz_native_scale > 1 only (the fill is scale / 2 = 0 at scale 1): the SDK's half-pixel offset fill, on the
    // resolve area's 8-pixel-aligned left / top edges, while PA_SU_VTX_CNTL.pix_center is kD3DZero (as D3D wrote it).
    uint32_t fill_x = 0, fill_y = 0; // also used by the depth-stencil pack below
    if (const uint32_t fill = scale >> 1; fill && REXCVAR_GET(pdz_native_scale_fill_half_pixel) &&
                                          (view::Load32(base, device + kPA_SU_VTX_CNTL) & 1) == 0)
    {
      const uint32_t fx = (sx & 7) == 0 ? fill : 0, fy = (sy & 7) == 0 ? fill : 0;
      fill_x = fx;
      fill_y = fy;
      FillHalfPixelOffset(*dev, blit, fx, fy);
      if (im.fill_resolves++ == 0)
      {
        REXLOG_INFO("pdz_native: resolution scale {}: resolves get the SDK's half-pixel offset fill (host columns / "
                    "rows 0..{} of each resolve area take the values of column / row {}; first: {:08X} -> {:08X} area "
                    "{},{} {}x{}, fill x {} y {}; pdz_native_scale_fill_half_pixel)",
                    scale, fill - 1, fill, src->guest, dst_fetch.base_address, sx, sy, w, h, fx, fy);
      }
    }
    passdump::AfterResolve(*src, *dst, dst_fetch, flags, dx, dy, w, h);
    ++dst->generation;
    dst->last_used_frame = dev->frame_index();
    // Split screen (native_draw.h): scene pass detection, and a new pass segment when a descriptor table of the current
    // one samples this mirror (the draws after this copy then transition it back and refresh its alias views).
    OnResolveInto(*dst, depth, src->guest);

    // pdz_native_depth_stencil_alias (stage2/polish/rcp90.md): the game also reads this depth texture's memory as
    // k_8_8_8_8 (Textures::Lookup marked the mirror): the same area as the EDRAM words, depth24 << 8 | stencil, into the
    // k_8_8_8_8 mirror of that key (bytes 0..3 = components 0..3, the stencil in component 0), while the source is
    // still a shader resource. The threat detector's outline reads that stencil (1 or 2 = the outlined classes).
    if (depth && dst->stencil_alias_wanted && REXCVAR_GET(pdz_native_depth_stencil_alias) &&
        (src->resource_format == DXGI_FORMAT_R32G8X24_TYPELESS || src->resource_format == DXGI_FORMAT_R24G8_TYPELESS))
    {
      FetchDecoded alias_fetch = dst_fetch;
      alias_fetch.format = 6; // k_8_8_8_8; the mirror key is (base, format, width, height)
      HostTexture *packed = Textures::Get().GetMirror(alias_fetch);
      if (packed && packed->resource_format == DXGI_FORMAT_R8G8B8A8_UNORM && packed->scale == dst->scale)
      {
        passes::BlitDesc pack = blit;
        pack.dst = packed->resource;
        pack.dst_rtv = packed->rtv;
        pack.rtv_format = packed->resource_format;
        pack.mode = passes::BlitMode::kDepthStencilAs8888;
        pack.stencil_resource = src->resource;
        pack.stencil_srv_format = src->resource_format == DXGI_FORMAT_R32G8X24_TYPELESS
                                      ? DXGI_FORMAT_X32_TYPELESS_G8X24_UINT
                                      : DXGI_FORMAT_X24_TYPELESS_G8_UINT;
        pack.depth_float24 = src->depth_format == 1; // the encoding of the main copy's mode
        Textures::Get().Transition(*packed, D3D12_RESOURCE_STATE_RENDER_TARGET);
        if (passes::Blit(*dev, pack))
        {
          if (fill_x || fill_y)
          {
            FillHalfPixelOffset(*dev, pack, fill_x, fill_y);
          }
          ++packed->generation;
          packed->last_used_frame = dev->frame_index();
          OnResolveInto(*packed, false, src->guest);
          if (im.stencil_packs++ == 0)
          {
            REXLOG_INFO("pdz_native: depth resolve {:08X} -> {:08X} {}x{} also packed into its k_8_8_8_8 mirror "
                        "(depth24 << 8 | stencil, {}; pdz_native_depth_stencil_alias)",
                        src->guest, dst_fetch.base_address, dst_fetch.width, dst_fetch.height,
                        pack.depth_float24 ? "20e4" : "unorm24");
          }
        }
      }
    }

    im.last_resolve.valid = true;
    im.last_resolve.frame = dev->frame_index();
    im.last_resolve.dest = dst_fetch;
    im.last_resolve.mirror = dst;
    }();

    // The clear after the copy (review 2 C1), as D3D programmed it inside the call: RB_COPY_CONTROL bit 8 =
    // colour (the colour source only, SDK IsClearingColor), bit 9 = depth and stencil of the bound depth surface
    // (SDK IsClearingDepth, whatever the source). D3D copies both bits from the flags ((flags & 0x377) - 0x10),
    // except that a 4x MSAA partial-sample resolve drops them (the clear comes later); a register left over from
    // an earlier call would disagree as well. So a clear needs both, and a disagreement is logged.
    const uint32_t ctl = view::Load32(base, device + kRB_COPY_CONTROL);
    if (((ctl ^ flags) & 0x300) && im.clear_mismatch_logged < 4)
    {
      ++im.clear_mismatch_logged;
      REXLOG_WARN("pdz_native: resolve flags {:X} but RB_COPY_CONTROL {:08X} (call site {:08X}): the clear bits "
                  "differ; only the clears both ask for are done",
                  flags, ctl, call_site);
    }
    const uint32_t clear_bits = ctl & flags & 0x300;
    if (!clear_bits)
    {
      return;
    }
    const uint32_t src_select = ctl & 7;
    HostSurface *const color_target = ((clear_bits & 0x100) && src_select < 4) ? b.rt[src_select] : nullptr;
    HostSurface *const depth_target = (clear_bits & 0x200) ? b.ds : nullptr;
    // The colour value is packed in the format of the source's RB_COLOR_INFO (what D3D's packer sub_826F1FD8 reads).
    const uint32_t color_format =
        src_select < 4
            ? (view::Load32(base, device + (src_select ? kRB_COLOR1_INFO + 4 * (src_select - 1) : kRB_COLOR_INFO)) >> 16) & 0xF
            : 0;
    const uint32_t color_hi = view::Load32(base, device + kRB_COLOR_CLEAR),
                   color_lo = view::Load32(base, device + kRB_COLOR_CLEAR_LO);
    float rgba[4];
    DecodeColorClear(color_format, color_hi, color_lo, rgba);
    const uint32_t depth_clear = view::Load32(base, device + kRB_DEPTH_CLEAR);
    const bool depth_float = ((view::Load32(base, device + kRB_DEPTH_INFO) >> 16) & 1) != 0; // as sub_826F1B78 packs it
    const float z = depth_float ? Float20e4ToFloat(depth_clear >> 8) : float(depth_clear >> 8) / 16777215.0f;
    const UINT8 stencil = UINT8(depth_clear & 0xFF);

    // The cleared area: the resolve area aligned out to the 8-pixel resolve granularity (the SDK clears the same),
    // clamped to each cleared surface.
    const int32_t ax0 = std::max<int32_t>(sx, 0) & ~7, ay0 = std::max<int32_t>(sy, 0) & ~7;
    const int32_t ax1 = (sx + sw + 7) & ~7, ay1 = (sy + sh + 7) & ~7;
    auto host_rect = [&](const HostSurface &s, D3D12_RECT &r) {
      const int32_t x1 = std::min<int32_t>(ax1, int32_t(s.width)), y1 = std::min<int32_t>(ay1, int32_t(s.height));
      const int32_t k = int32_t(s.scale);
      r = {LONG(ax0 * k), LONG(ay0 * k), LONG(x1 * k), LONG(y1 * k)};
      return x1 > ax0 && y1 > ay0;
    };
    D3D12_RECT r{};
    bool color_done = false, depth_done = false;
    if (color_target && host_rect(*color_target, r))
    {
      Transition(*color_target, D3D12_RESOURCE_STATE_RENDER_TARGET);
      dev->list()->ClearRenderTargetView(color_target->view, rgba, 1, &r);
      color_target->write_stamp = ++im.stamp;
      color_done = true;
    }
    if (depth_target && host_rect(*depth_target, r))
    {
      Transition(*depth_target, D3D12_RESOURCE_STATE_DEPTH_WRITE);
      dev->list()->ClearDepthStencilView(depth_target->view, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, z,
                                         stencil, 1, &r);
      depth_target->write_stamp = ++im.stamp;
      depth_done = true;
    }

    // Log: the first resolve clear of each call site (up to 16 sites), and every one while a frame trace runs.
    bool log_clear = FrameTraceActive();
    if (im.clear_site_count < 16 &&
        std::find(im.clear_sites, im.clear_sites + im.clear_site_count, call_site) == im.clear_sites + im.clear_site_count)
    {
      im.clear_sites[im.clear_site_count++] = call_site;
      log_clear = true;
    }
    if (log_clear)
    {
      float v[4] = {};
      if (IsGuestPointer(clear_color_ptr))
      {
        for (uint32_t i = 0; i < 4; ++i)
        {
          v[i] = view::LoadFloat(base, clear_color_ptr + 4 * i);
        }
      }
      REXLOG_INFO("pdz_native: resolve clear at call site {:08X}: flags {:X}, RB_COPY_CONTROL {:08X} (source {}, "
                  "samples {}, colour clear {}, depth clear {}, command {}); pClearColor {:08X} = ({:.4f} {:.4f} {:.4f} "
                  "{:.4f}), ClearZ {:.6f}",
                  call_site, flags, ctl, src_select, (ctl >> 4) & 7, (ctl >> 8) & 1, (ctl >> 9) & 1, (ctl >> 20) & 3,
                  clear_color_ptr, v[0], v[1], v[2], v[3], BitsToFloat(z_bits));
      REXLOG_INFO("pdz_native:   colour {:08X}: RB_COLOR_CLEAR {:08X} LO {:08X} as colour format {} = ({:.4f} {:.4f} "
                  "{:.4f} {:.4f}) {}; depth {:08X}: RB_DEPTH_CLEAR {:08X} = z {:.6f} ({}) stencil {} {}; area {},{} "
                  "{}x{} (resolve area {},{} {}x{})",
                  color_target ? color_target->guest : 0u, color_hi, color_lo, color_format, rgba[0], rgba[1], rgba[2],
                  rgba[3], color_done ? "cleared" : "not cleared", depth_target ? depth_target->guest : 0u, depth_clear,
                  z, depth_float ? "20e4" : "unorm24", uint32_t(stencil), depth_done ? "cleared" : "not cleared", ax0, ay0,
                  ax1 - ax0, ay1 - ay0, sx, sy, sw, sh);
    }
  }

  // ---------------------------------------------------------------------------
  // pdz_native_async_readback (stage 2 M5 asyncrb; PDZRenderResearch\native\stage2\m5\m5_asyncrb.md). The synchronous
  // readback below waits for the GPU inside the InsertCallback (Flush: everything queued before the copy, up to three
  // frames) on the thread that runs it: the game thread, or the record thread, whose stall the game thread's next
  // Swap then waits for. The guest does not read the bytes at the callback: the de-rez callback 825F6608 only sets
  // the event [82DA488C] (sub_82ADA6E8 = NtSetEvent); DeResThread (sub_825F7C10: wait on that event, reset it, call
  // sub_825F66E0) reads the snapshot texture [82DA4614] on the CPU in sub_825F66E0, and no guest thread waits for
  // DeResThread. So the async path records the copy into a readback slot, submits it without a CPU wait and returns.
  // The bytes reach guest memory, oldest snapshot first, at the first of:
  //   - a native frame end (Surfaces::EndFrame) or the next snapshot, once the GPU has passed the copy's fence (no
  //     wait: GetCompletedValue);
  //   - the hook on sub_825F66E0 (DeResThread, before its read), which waits for every pending copy (bounded by
  //     kConsumerWaitMs) and writes them;
  //   - a synchronous readback (its Flush waits for everything): the older async ones are written first.
  // The command processor is not held back: the INTERRUPT and the callback run as before, and the callback's only
  // effect (the event) wakes the thread whose read waits for the bytes. Holding the ring write pointer instead would
  // also hold every fence, read-pointer write-back and the Swap's progress behind the INTERRUPT, which the game thread
  // waits for within a frame (m5_asyncrb.md). Null GPU backend only (on the plugin path the plugin's resolve_readback
  // writes the reference bytes after ours; a late native write would replace them) and only for the de-rez callback
  // (another InsertCallback's reader is not known: synchronous).
  // ---------------------------------------------------------------------------
  namespace
  {
    namespace asyncrb
    {
      constexpr uint32_t kDeRezCallback = 0x825F6608; // sub_825F64E8's callback (sets DeResThread's event)
      constexpr uint32_t kSlots = 4;                  // async copies in flight at most; more go synchronously
      constexpr uint32_t kConsumerWaitMs = 2000;      // DeResThread's bound; then it reads what is there (logged)

      enum class SlotState : uint8_t
      {
        kFree,
        kRecording, // the render thread is recording its copy (only that thread touches it)
        kPending,   // submitted; written once the GPU has passed `fence`
      };
      struct Slot
      {
        SlotState state = SlotState::kFree;
        ID3D12Resource *buffer = nullptr; // READBACK heap, kept for the session (replaced when a snapshot is larger)
        uint64_t size = 0;
        uint64_t fence = 0; // the queue's value after the copy (NativeDevice::Submit)
        uint64_t seq = 0;   // start order: written oldest first
        FetchDecoded dest;
        uint64_t offset = 0; // footprint offset, row pitch and scale for WriteGuestLevel0
        uint32_t row_pitch = 0;
        uint32_t scale = 1;
        uint64_t total = 0;
        int64_t start_us = 0;
        uint64_t start_frame = 0;
      };

      // Who wrote an async snapshot (SUMMARY, first-completion line).
      enum Who : uint32_t
      {
        kFrameEnd = 0,
        kNextSnapshot,
        kConsumer,
        kSync,
        kWhoCount
      };
      const char *const kWhoNames[kWhoCount] = {"at a native frame end", "at the next snapshot",
                                                "by DeResThread before its read", "before a synchronous readback"};

      struct State
      {
        // The slots. Taken under RenderMutex (render thread) or alone (DeResThread); never held during a GPU wait.
        std::mutex mutex;
        Slot slots[kSlots];
        uint64_t next_seq = 0;
        bool first_logged = false;
        std::atomic<uint32_t> pending{0};
        std::atomic<uint64_t> frames{0}; // native frames ended (Surfaces::EndFrame: frame index + 1)
        // SUMMARY counters.
        std::atomic<uint64_t> started{0}, fallbacks{0}, map_failures{0}, sync_done{0};
        std::atomic<uint64_t> done[kWhoCount] = {};
        std::atomic<uint64_t> latency_us{0}, latency_max_us{0}, latency_frames_max{0};
        std::atomic<uint64_t> consumer_calls{0}, consumer_waits{0}, consumer_wait_us{0}, consumer_wait_max_us{0},
            consumer_timeouts{0};
        // Snapshots written into guest memory, sync and async (the '(#n)' of the log lines; SnapshotReadbacks).
        std::atomic<uint32_t> written{0};
      };
      State g;

      void Max(std::atomic<uint64_t> &target, uint64_t value)
      {
        uint64_t seen = target.load(std::memory_order_relaxed);
        while (value > seen && !target.compare_exchange_weak(seen, value, std::memory_order_relaxed))
        {
        }
      }

      // The '(#n)' line derez_check.py pairs with each logged InsertCallback (the first three).
      void LogWritten(const FetchDecoded &dest, const char *how)
      {
        const uint32_t n = g.written.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 3)
        {
          REXLOG_INFO("pdz_native: snapshot {}x{} read back into guest texture memory {:08X} (#{}){}", dest.width,
                      dest.height, dest.base_address, n, how);
        }
      }

      // Writes a pending slot whose fence has passed into guest memory and frees it (g.mutex held). WriteGuestLevel0
      // only reads the slot and writes guest memory (no texture-cache state), so any thread may run it.
      void Finish(Slot &s, Who who)
      {
        uint8_t *mapped = nullptr;
        D3D12_RANGE range{0, SIZE_T(s.total)};
        if (SUCCEEDED(s.buffer->Map(0, &range, reinterpret_cast<void **>(&mapped))) && mapped)
        {
          Textures::Get().WriteGuestLevel0(s.dest, mapped + s.offset, s.row_pitch, s.scale);
          D3D12_RANGE none{0, 0};
          s.buffer->Unmap(0, &none);
          LogWritten(s.dest, " asynchronously");
        }
        else
        {
          g.map_failures.fetch_add(1, std::memory_order_relaxed);
        }
        const int64_t now = pdz::gpusync::NowUs();
        const uint64_t us = now > s.start_us ? uint64_t(now - s.start_us) : 0;
        const uint64_t frames_now = g.frames.load(std::memory_order_relaxed);
        const uint64_t frames = frames_now > s.start_frame ? frames_now - s.start_frame : 0;
        g.done[who].fetch_add(1, std::memory_order_relaxed);
        g.latency_us.fetch_add(us, std::memory_order_relaxed);
        Max(g.latency_max_us, us);
        Max(g.latency_frames_max, frames);
        pdz::timing::CountEvent(pdz::timing::Event::kSnapshotAsyncDone); // pdz_frame_timing_spike_ms
        if (!g.first_logged)
        {
          g.first_logged = true;
          REXLOG_INFO("pdz_native: snapshot read back asynchronously (fence {}, completed after {} frames / {:.1f} ms, "
                      "written {})",
                      s.fence, frames, double(us) / 1000.0, kWhoNames[who]);
        }
        s.state = SlotState::kFree;
        g.pending.fetch_sub(1, std::memory_order_release);
      }

      // Writes, oldest first, every pending snapshot whose fence the GPU has passed. Any thread; never waits.
      uint32_t CompleteReady(Who who)
      {
        if (g.pending.load(std::memory_order_acquire) == 0)
        {
          return 0;
        }
        NativeDevice *dev = NativeDevice::Existing();
        if (!dev)
        {
          return 0;
        }
        std::lock_guard<std::mutex> lock(g.mutex);
        const uint64_t completed = dev->completed_fence();
        uint32_t done = 0;
        for (;;)
        {
          Slot *oldest = nullptr;
          for (Slot &s : g.slots)
          {
            if (s.state == SlotState::kPending && (!oldest || s.seq < oldest->seq))
            {
              oldest = &s;
            }
          }
          if (!oldest || oldest->fence > completed)
          {
            break;
          }
          Finish(*oldest, who);
          ++done;
        }
        return done;
      }

      // The render thread (RenderMutex held, in a frame): records the copy of the snapshot mirror into a free slot and
      // submits it without a CPU wait. False = no free slot or no buffer (the caller reads back synchronously).
      bool Start(NativeDevice &dev, HostTexture &m, const FetchDecoded &dest,
                 const D3D12_PLACED_SUBRESOURCE_FOOTPRINT &fp, uint64_t total)
      {
        CompleteReady(kNextSnapshot); // frees the slots the GPU is done with (and writes them, in order)
        Slot *slot = nullptr;
        {
          std::lock_guard<std::mutex> lock(g.mutex);
          for (Slot &s : g.slots)
          {
            if (s.state == SlotState::kFree)
            {
              slot = &s;
              break;
            }
          }
          if (!slot)
          {
            return false;
          }
          slot->state = SlotState::kRecording;
        }
        if (!slot->buffer || slot->size < total)
        {
          if (slot->buffer)
          {
            dev.ReleaseLater(slot->buffer); // a free slot's last copy has completed
          }
          slot->buffer = dev.CreateBuffer(total, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST,
                                          D3D12_RESOURCE_FLAG_NONE, "pdz_native snapshot readback (async)");
          slot->size = slot->buffer ? total : 0;
          if (!slot->buffer)
          {
            std::lock_guard<std::mutex> lock(g.mutex);
            slot->state = SlotState::kFree;
            return false;
          }
        }
        pdz::timing::CountEvent(pdz::timing::Event::kSnapshotAsync); // pdz_frame_timing_spike_ms
        Textures::Get().Transition(m, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = slot->buffer;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = fp;
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = m.resource;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        dev.list()->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        Textures::Get().Transition(m, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                                          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        // Submitted now, without a wait: the fence then completes on the GPU's own progress, whatever the guest
        // threads and the render thread do next.
        const uint64_t fence = dev.Submit();
        {
          std::lock_guard<std::mutex> lock(g.mutex);
          slot->fence = fence;
          slot->seq = g.next_seq++;
          slot->dest = dest;
          slot->offset = fp.Offset;
          slot->row_pitch = fp.Footprint.RowPitch;
          slot->scale = m.scale;
          slot->total = total;
          slot->start_us = pdz::gpusync::NowUs();
          slot->start_frame = dev.frame_index();
          slot->state = SlotState::kPending;
          g.pending.fetch_add(1, std::memory_order_release);
        }
        g.started.fetch_add(1, std::memory_order_relaxed);
        return true;
      }

      // DeResThread, before sub_825F66E0 reads the snapshot: waits (bounded) until the GPU has passed every pending
      // copy and writes them. Needs nothing from the render thread (the copies were submitted when recorded).
      void BeforeConsumerRead()
      {
        g.consumer_calls.fetch_add(1, std::memory_order_relaxed);
        if (g.pending.load(std::memory_order_acquire) == 0)
        {
          return;
        }
        NativeDevice *dev = NativeDevice::Existing();
        if (!dev)
        {
          return;
        }
        uint64_t target = 0;
        {
          std::lock_guard<std::mutex> lock(g.mutex);
          for (const Slot &s : g.slots)
          {
            if (s.state == SlotState::kPending)
            {
              target = std::max(target, s.fence);
            }
          }
        }
        if (target && dev->completed_fence() < target)
        {
          static thread_local HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
          const int64_t t0 = pdz::gpusync::NowUs();
          bool ok = false;
          {
            const pdz::timing::EventTimer wait_time(pdz::timing::Event::kSnapshotAsyncWaitCycles);
            ok = dev->WaitFence(target, event, kConsumerWaitMs);
          }
          const int64_t t1 = pdz::gpusync::NowUs();
          const uint64_t us = t1 > t0 ? uint64_t(t1 - t0) : 0;
          g.consumer_waits.fetch_add(1, std::memory_order_relaxed);
          g.consumer_wait_us.fetch_add(us, std::memory_order_relaxed);
          Max(g.consumer_wait_max_us, us);
          if (!ok && g.consumer_timeouts.fetch_add(1, std::memory_order_relaxed) < 3)
          {
            REXLOG_WARN("pdz_native: DeResThread waited {} ms for the async snapshot copy (fence {}, GPU at {}): it "
                        "reads the snapshot as it is (the copy is written when the GPU gets there)",
                        kConsumerWaitMs, target, dev->completed_fence());
          }
        }
        CompleteReady(kConsumer);
      }
    } // namespace asyncrb
  } // namespace

  // ---------------------------------------------------------------------------
  // De-rez snapshot readback. Ordering: the game does Resolve(0) then InsertCallback; the hook runs us
  // before the guest InsertCallback body, so the guest memory holds the native pixels before the callback
  // is even queued (the Xenia plugin's resolve_readback later writes the same bytes from the emulated
  // frame, so with both running the emulated picture wins; see CORE_NOTES.md). pdz_native_async_readback: see
  // asyncrb above (the copy is submitted here, the bytes are written later, before DeResThread reads them).
  // ---------------------------------------------------------------------------
  void Surfaces::OnInsertCallback(uint8_t *base, uint32_t callback)
  {
    (void)base;
    Impl &im = *impl_;
    NativeDevice *dev = NativeDevice::Get();
    if (!dev || !im.last_resolve.valid || !dev->in_frame())
    {
      return;
    }
    LastResolve lr = im.last_resolve;
    im.last_resolve.valid = false;
    if (lr.frame != dev->frame_index() || !lr.mirror || lr.mirror->resource_format != DXGI_FORMAT_R8G8B8A8_UNORM)
    {
      return;
    }
    HostTexture &m = *lr.mirror;
    D3D12_RESOURCE_DESC desc = m.resource->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT rows = 0;
    UINT64 row_size = 0, total = 0;
    dev->device()->GetCopyableFootprints(&desc, 0, 1, 0, &fp, &rows, &row_size, &total);
    if (total == 0)
    {
      return;
    }
    // pdz_native_async_readback: the de-rez snapshot on the null backend, when a slot is free.
    if (REXCVAR_GET(pdz_native_async_readback) && pdz::gpusync::NullBackend() && callback == asyncrb::kDeRezCallback)
    {
      if (asyncrb::Start(*dev, m, lr.dest, fp, total))
      {
        return;
      }
      asyncrb::g.fallbacks.fetch_add(1, std::memory_order_relaxed);
      pdz::timing::CountEvent(pdz::timing::Event::kSnapshotAsyncFallback); // pdz_frame_timing_spike_ms
    }
    if (!im.readback || im.readback_size < total)
    {
      if (im.readback)
      {
        dev->ReleaseLater(im.readback);
      }
      im.readback = dev->CreateBuffer(total, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST,
                                      D3D12_RESOURCE_FLAG_NONE, "pdz_native snapshot readback");
      im.readback_size = total;
      if (!im.readback)
      {
        return;
      }
    }
    // pdz_frame_timing_spike_ms: the readback's time and its GPU wait (the frame's GPU work so far plus the frames
    // still in flight before it: the queue runs in order).
    pdz::timing::CountEvent(pdz::timing::Event::kSnapshot);
    const pdz::timing::EventTimer snapshot_time(pdz::timing::Event::kSnapshotCycles);
    Textures::Get().Transition(m, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = im.readback;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = fp;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = m.resource;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    dev->list()->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    {
      const pdz::timing::EventTimer wait_time(pdz::timing::Event::kSnapshotWaitCycles);
      dev->Flush(); // submit + wait (one GPU round trip per de-rez event)
    }
    // Async snapshots still pending are older than this one and complete now (the Flush waited for them): they are
    // written first, so this one is what stays in guest memory.
    asyncrb::CompleteReady(asyncrb::kSync);
    uint8_t *mapped = nullptr;
    D3D12_RANGE range{0, SIZE_T(total)};
    if (SUCCEEDED(im.readback->Map(0, &range, reinterpret_cast<void **>(&mapped))) && mapped)
    {
      Textures::Get().WriteGuestLevel0(lr.dest, mapped + fp.Offset, fp.Footprint.RowPitch, m.scale);
      D3D12_RANGE none{0, 0};
      im.readback->Unmap(0, &none);
      asyncrb::g.sync_done.fetch_add(1, std::memory_order_relaxed);
      asyncrb::LogWritten(lr.dest, "");
    }
    Textures::Get().Transition(m, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  }

  uint32_t Surfaces::SnapshotReadbacks() const { return asyncrb::g.written.load(std::memory_order_relaxed); }

  uint32_t SnapshotReadbacks()
  {
    RenderLock lock(RenderMutex());
    return Surfaces::Get().SnapshotReadbacks();
  }

  std::string SnapshotReadbackSummary()
  {
    using asyncrb::g;
    const auto get = [](const std::atomic<uint64_t> &a) { return a.load(std::memory_order_relaxed); };
    uint64_t done = 0;
    for (const auto &d : g.done)
    {
      done += get(d);
    }
    return fmt::format(
        "snapshot readbacks (pdz_native_async_readback {}): written {}, sync {}, async {} (written {}: {} {}, {} {}, "
        "{} {}, {} {}; pending {}; start to write avg {:.1f} ms, max {:.1f} ms / {} frames; map failures {}), sync "
        "fallbacks {} (all {} async slots pending, or no buffer); DeResThread reads {}, GPU waits {} ({:.1f} ms in "
        "all, longest {:.1f} ms), timeouts {}",
        REXCVAR_GET(pdz_native_async_readback) ? "on" : "off", g.written.load(std::memory_order_relaxed),
        get(g.sync_done), get(g.started), done, asyncrb::kWhoNames[0], get(g.done[0]), asyncrb::kWhoNames[1],
        get(g.done[1]), asyncrb::kWhoNames[2], get(g.done[2]), asyncrb::kWhoNames[3], get(g.done[3]),
        g.pending.load(std::memory_order_relaxed), done ? double(get(g.latency_us)) / 1000.0 / double(done) : 0.0,
        double(get(g.latency_max_us)) / 1000.0, get(g.latency_frames_max), get(g.map_failures), get(g.fallbacks),
        asyncrb::kSlots, get(g.consumer_calls), get(g.consumer_waits), double(get(g.consumer_wait_us)) / 1000.0,
        double(get(g.consumer_wait_max_us)) / 1000.0, get(g.consumer_timeouts));
  }


  // ---------------------------------------------------------------------------
  // Frame maintenance (REVIEW2.md C3): retired and long-unused surfaces.
  // ---------------------------------------------------------------------------
  namespace
  {
    // Frees a surface nothing points at any more. The RTV / DSV and the staging SRV are read when a command is
    // recorded (OMSetRenderTargets, Clear*View, CopySrv into the frame's table), so their slots are free at once;
    // the resource is released after the current frame's fence (a retired surface's was queued at retirement).
    void DestroySurface(NativeDevice &dev, HostSurface *s)
    {
      if (s->depth)
      {
        dev.FreeDsv(s->view);
      }
      else
      {
        dev.FreeRtv(s->view);
      }
      dev.FreeStagingSrv(s->srv);
      if (s->resource)
      {
        dev.ReleaseLater(s->resource);
        s->resource = nullptr;
      }
      delete s;
    }
  } // namespace

  void Surfaces::EndFrame(uint64_t frame_index)
  {
    // pdz_native_async_readback: snapshot copies the GPU has finished are written into guest memory now (no wait).
    asyncrb::g.frames.store(frame_index + 1, std::memory_order_relaxed);
    asyncrb::CompleteReady(asyncrb::kFrameEnd);
    Impl &im = *impl_;
    NativeDevice *dev = NativeDevice::Get();
    // The pass dump keeps HostSurface pointers between its steps (pending targets, dumped sources): nothing is
    // freed while it records.
    if (!dev || passdump::Active())
    {
      return;
    }
    // Retired surfaces: the bound sets that could still point at them were locals of this frame's calls, and
    // last_bound was cleared at retirement.
    if (!im.retired.empty())
    {
      for (HostSurface *s : im.retired)
      {
        DestroySurface(*dev, s);
      }
      im.retired_freed += im.retired.size();
      im.retired.clear();
    }
    // Long-unused surfaces (a previous level's or match's targets), checked every 300 frames like the texture cache.
    if (frame_index < im.last_eviction_frame + 300)
    {
      return;
    }
    im.last_eviction_frame = frame_index;
    uint32_t evicted = 0;
    for (auto it = im.surfaces.begin(); it != im.surfaces.end();)
    {
      HostSurface *s = it->second;
      // Never the last bound set (BindTargets compares against these pointers), never one used within kEvictFrames
      // (every Lookup stamps the frame, so nothing bound, cleared or resolved this frame qualifies).
      const bool in_last_bound =
          std::find(std::begin(im.last_bound), std::end(im.last_bound), s) != std::end(im.last_bound);
      if (!in_last_bound && s->last_used_frame + kEvictFrames < frame_index)
      {
        if (im.evict_logged < 32)
        {
          ++im.evict_logged;
          REXLOG_INFO("pdz_native: surface {:08X} ({}x{} {}, EDRAM base {}) evicted: unused since frame {} (now {})",
                      s->guest, s->width, s->height, s->depth ? "depth" : "colour", s->edram_base, s->last_used_frame,
                      frame_index);
        }
        DestroySurface(*dev, s);
        it = im.surfaces.erase(it);
        ++evicted;
      }
      else
      {
        ++it;
      }
    }
    if (evicted)
    {
      im.evicted_total += evicted;
      const NativeDevice::PersistentHeapStats hs = dev->GetPersistentHeapStats();
      PDZ_LOG_DEFERRED("pdz_native: evicted {} surfaces unused for {} frames ({} kept, {} evicted and {} retired freed in "
                  "total) | RTV {}/{} DSV {}/{} staging SRV {}/{} live, refused {} / {} / {}",
                  evicted, kEvictFrames, im.surfaces.size(), im.evicted_total, im.retired_freed, hs.rtv_live,
                  hs.rtv_size, hs.dsv_live, hs.dsv_size, hs.staging_live, hs.staging_size, hs.rtv_failures,
                  hs.dsv_failures, hs.staging_failures);
    }
  }

  void Surfaces::ForgetMirror(const HostTexture *mirror)
  {
    Impl &im = *impl_;
    if (mirror && im.last_resolve.mirror == mirror)
    {
      im.last_resolve.valid = false;
      im.last_resolve.mirror = nullptr;
    }
  }

  Surfaces::MemoryStats Surfaces::GetMemoryStats()
  {
    Impl &im = *impl_;
    MemoryStats s;
    NativeDevice *dev = NativeDevice::Existing();
    for (auto &entry : im.surfaces)
    {
      HostSurface &h = *entry.second;
      ++(h.depth ? s.depth : s.colour);
      if (!h.host_bytes && h.resource && dev)
      {
        const D3D12_RESOURCE_DESC desc = h.resource->GetDesc();
        h.host_bytes = dev->device()->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
      }
      s.bytes += h.host_bytes;
    }
    s.retired = uint32_t(im.retired.size());
    s.evicted = im.evicted_total;
    s.retired_freed = im.retired_freed;
    s.readback_bytes = im.readback ? im.readback_size : 0;
    {
      std::lock_guard<std::mutex> lock(asyncrb::g.mutex); // pdz_native_async_readback slots
      for (const asyncrb::Slot &slot : asyncrb::g.slots)
      {
        s.readback_bytes += slot.buffer ? slot.size : 0;
      }
    }
    return s;
  }

  namespace
  {
    // Marks a clear / resolve of the bound targets and, after it, the bound targets' draw pass again.
    struct PassBracket
    {
      NativeDevice *dev = nullptr;
      GpuPass after = GpuPass::kOtherDraw;
      PassBracket(uint8_t *base, uint32_t device, bool resolve, uint32_t flags, uint32_t rect_count, uint32_t rects,
                  uint32_t dest_texture)
      {
        NativeDevice *d = NativeDevice::Existing();
        if (!d || !d->GpuPassesOn() || !IsGuestPointer(device))
        {
          return;
        }
        dev = d;
        const Surfaces::Bound b = Surfaces::Get().GetBound(base, device);
        after = PassKind(b, GpuPass::kShadowDraw, GpuPass::kMainDraw, GpuPass::kOtherDraw);
        const GpuPass kind = resolve
                                 ? PassKind(b, GpuPass::kShadowResolve, GpuPass::kMainResolve, GpuPass::kOtherResolve)
                                 : PassKind(b, GpuPass::kShadowClear, GpuPass::kMainClear, GpuPass::kOtherClear);
        dev->GpuMark(kind);
        std::string rect = "none";
        if (IsGuestPointer(rects))
        {
          rect = fmt::format("{},{}-{},{}", view::LoadS32(base, rects), view::LoadS32(base, rects + 4),
                             view::LoadS32(base, rects + 8), view::LoadS32(base, rects + 12));
        }
        TraceOp(*dev, kind,
                resolve ? fmt::format("resolve flags {:X} src rect {} -> texture {:08X} (fetch {:08X}) | {}", flags, rect,
                                      dest_texture, IsGuestPointer(dest_texture) ? view::Load32(base, dest_texture + 4) : 0u,
                                      TraceTargets(b))
                        : fmt::format("clear flags {:X} rects {} first {} | {}", flags, rect_count, rect, TraceTargets(b)));
      }
      ~PassBracket()
      {
        if (dev && dev->GpuPassesOn())
        {
          dev->GpuMark(after);
        }
      }
    };
  } // namespace

  // native.h entry points
  void OnClear(uint8_t *base, uint32_t device, uint32_t count, uint32_t rects, uint32_t flags, uint32_t color,
               uint32_t z_bits, uint32_t stencil)
  {
    const pdz::timing::Scope timing(pdz::timing::Bucket::kNative); // pdz_frame_timing
    RenderLock lock(RenderMutex());
    const pdz::timing::PhaseScope phase(pdz::timing::Phase::kClear); // pdz_frame_timing_phases
    NoteRenderThread("clear");
    FlushPendingBegin(base);
    InvalidateConstantBuffers();
    if (EnsureFrame())
    {
      const PassBracket pass(base, device, false, flags, count, rects, 0); // pdz_native_gpu_passes
      Surfaces::Get().Clear(base, device, count, rects, flags, color, z_bits, stencil);
    }
  }

  void OnResolve(uint8_t *base, uint32_t device, uint32_t flags, uint32_t src_rect, uint32_t dest_texture,
                 uint32_t dest_point, uint32_t level, uint32_t slice, uint32_t clear_color_ptr, uint32_t z_bits,
                 uint32_t call_site)
  {
    const pdz::timing::Scope timing(pdz::timing::Bucket::kNative); // pdz_frame_timing
    RenderLock lock(RenderMutex());
    const pdz::timing::PhaseScope phase(pdz::timing::Phase::kResolve); // pdz_frame_timing_phases
    pdz::timing::CountEvent(pdz::timing::Event::kResolve);             // pdz_frame_timing_spike_ms
    NoteRenderThread("resolve");
    FlushPendingBegin(base);
    InvalidateConstantBuffers();
    if (EnsureFrame())
    {
      const PassBracket pass(base, device, true, flags, 0, src_rect, dest_texture); // pdz_native_gpu_passes
      Surfaces::Get().Resolve(base, device, flags, src_rect, dest_texture, dest_point, level, slice, clear_color_ptr,
                              z_bits, call_site);
    }
  }
} // namespace pdz::native

// D3DDevice_InsertCallback(device, type, callback, context): r3 device, r4 type, r5 callback, r6 context.
// Not hooked by shadow_hooks.cpp. The readback runs before the guest body queues the callback.
REX_HOOK_RAW(sub_826DE910)
{
  pdz::timing::CountEvent(pdz::timing::Event::kInsertCallback); // pdz_frame_timing_spike_ms
  if (pdz::census::Enabled())
  {
    pdz::census::OnInsertCallback(uint32_t(ctx.lr), ctx.r4.u32, ctx.r5.u32); // gpu_census.h: by caller and callback
  }
  // gpu_sync.h (pdz_sync_log / the null GPU backend): the call and the time of the native de-rez readback.
  const bool sync = pdz::gpusync::Enabled();
  const int64_t sync_start = sync ? pdz::gpusync::NowUs() : 0;
  int64_t native_us = 0;
  if (sync)
  {
    pdz::gpusync::OnInsertCallbackEnter(base, ctx.r3.u32, uint32_t(ctx.lr), ctx.r4.u32, ctx.r5.u32);
  }
  if (pdz::native::recthread::Active())
  {
    // pdz_native_record_thread: queued; the guest's callback (CP thread, at the INTERRUPT) comes after the readback
    // because the kick that publishes the INTERRUPT is queued behind it (pdz_native_async_readback: after the copy
    // is submitted; DeResThread's read waits for the bytes, see asyncrb).
    pdz::native::recthread::OnInsertCallback(base, ctx.r5.u32);
    native_us = sync ? pdz::gpusync::NowUs() - sync_start : 0;
  }
  else if (pdz::native::Enabled())
  {
    const pdz::timing::Scope timing(pdz::timing::Bucket::kNative); // pdz_frame_timing
    pdz::native::RenderLock lock(pdz::native::RenderMutex());
    const pdz::timing::PhaseScope phase(pdz::timing::Phase::kCallback); // pdz_frame_timing_phases
    pdz::native::Surfaces::Get().OnInsertCallback(base, ctx.r5.u32);
    native_us = sync ? pdz::gpusync::NowUs() - sync_start : 0;
  }
  __imp__sub_826DE910(ctx, base);
  if (sync)
  {
    pdz::gpusync::OnInsertCallbackLeave(native_us, pdz::gpusync::NowUs() - sync_start);
  }
}

// sub_825F66E0 (TU3; launch 825E6CD0): DeResThread's step after the de-rez callback's event (sub_825F7C10: wait, reset,
// this). It reads the snapshot texture [82DA4614] on the CPU to colour the cubes. pdz_native_async_readback: the
// snapshot copies still in flight are waited for and written first (nothing pending: a counter and one load).
REX_HOOK_RAW(sub_825F66E0)
{
  pdz::native::asyncrb::BeforeConsumerRead();
  __imp__sub_825F66E0(ctx, base);
}
