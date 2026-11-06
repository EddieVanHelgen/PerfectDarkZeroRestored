// Native renderer (stage 1): 7e3 blend pixel-shader variants (native_blend7e3.h). Cold path except Wanted() (a few
// register bit tests per draw, only with the cvar on) and the variant map lookup of the draws it selects.
#include "native_blend7e3.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <unordered_map>

#include <fmt/format.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>
#include <rex/hash.h> // XXH3_64bits: the on-disk variant cache key
#include <rex/logging.h>

#include "native_blend7e3_hlsl.h"
#include "native_depth20e4.h" // CompilePixelShader / IsSignedDxil: the fork's dxcompiler, the variants' arguments
#include "native_device.h"
#include "native_shaders.h"

REXCVAR_DEFINE_INT32(pdz_native_7e3_blend, 1, "Render",
                     "Native renderer: blending into a 7e3 (k_2_10_10_10_FLOAT) render target like the SDK's ROV path "
                     "(the HDR scene target; the host surface is float16, blended by the fixed-function blender with "
                     "float16 rounding and no clamping). 0 = off (as before). 1 = the pixel shader's alpha output is "
                     "clamped to 0..1 before the blend (7e3 has a fixed-point alpha: the SDK clamps the blending "
                     "source alpha). 2 = exact: the blend runs in the pixel shader through a rasterizer-ordered view "
                     "(float32 blend, clamp to 0..31.875 / 0..1, round to nearest even 7e3 and 2-bit alpha after "
                     "every blend) for single-target draws without depth / stencil writes, mode 1 for the others; "
                     "resolves of 7e3 surfaces round to 7e3. Mode 2 applies to surfaces created after it is set (set "
                     "it on the command line). stage1/subagents/hdr7e3.md.")
    .range(0, 2)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(pdz_native_7e3_resolve_round, true, "Render",
                    "Native renderer: resolves of 7e3 (k_2_10_10_10_FLOAT) colour targets round the colour to 7e3 "
                    "(nearest even, the SDK's pack), what the EDRAM holds after a non-blended write, in every "
                    "pdz_native_7e3_blend mode (mode 2 always does). The game's exposure loop runs through the 7e3 HDR "
                    "target: with the rounding the adapted luminance stops within half a 7e3 step per frame's update "
                    "of the measured one, as in the emulator (+-0.039 below 0.5 at 60 fps), instead of converging "
                    "to it. The picture changes only while the tone map's exposure acts (adapted luminance under "
                    "0.2, EDCCE21F c0 = 0.2 / 2.0: dark scenes such as Temple, where the native is otherwise an even "
                    "0.83-0.91 of the emulator). stage1/subagents/river.md, temple.md. MP Old Town (batch "
                    "20261026_1640, mp03): after the first hit the emulator's La stopped above the composite's "
                    "clamp (EDCCE21F c0.y), exposure 0.71-0.77 against the native's 1 for the rest of the match "
                    "(MAE ~10, 4-8 % over 16); stage1/subagents/oldtown.md.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace pdz::native::blend7e3
{
  namespace
  {
    namespace reg = rex::graphics::reg;
    namespace xenos = rex::graphics::xenos;

    constexpr uint32_t kLogBuilds = 8; // variants logged individually (then only the periodic counters)
    constexpr uint32_t kFirstUseLogLines = 32; // pdz_native_first_use_log: compiles past kLogBuilds logged individually
    uint32_t g_first_use_logged = 0;

    struct Entry
    {
      std::vector<uint8_t> dxil;
      bool failed = false;
    };
    std::unordered_map<uint64_t, Entry> g_variants; // by PS hash ^ mode; render thread only

    struct Stats
    {
      uint32_t built = 0, from_disk = 0, failed = 0, compile_ms = 0;
      uint64_t draws[3] = {};
      // Mode 2 wanted, mode 1 used: depth write, stencil write, more than one colour target, surface without UAV,
      // no ROV support on the device, no mode-2 variant.
      uint64_t fallback_depth = 0, fallback_stencil = 0, fallback_mrt = 0, fallback_surface = 0, fallback_device = 0,
               fallback_variant = 0;
    };
    Stats g_stats;

    int g_rov_supported = -1; // -1 = not checked yet

    bool FileExists(const std::string &path)
    {
      std::error_code ec;
      return std::filesystem::is_regular_file(path, ec);
    }

    bool ReadText(const std::string &path, std::string &out)
    {
      std::ifstream f(path, std::ios::binary);
      if (!f)
      {
        return false;
      }
      out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
      return true;
    }

    bool ReadBinary(const std::string &path, std::vector<uint8_t> &out)
    {
      std::ifstream f(path, std::ios::binary);
      if (!f)
      {
        return false;
      }
      out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
      return true;
    }

    std::string FindHlsl(const NativeShader *ps)
    {
      const std::string name = fmt::format("{:016x}.hlsl", ps->hash);
      for (const std::string &d : {ps->source, ShaderCache::Get().directory(), ShaderCache::Get().fallback_directory()})
      {
        if (!d.empty() && FileExists(d + "\\" + name))
        {
          return d + "\\" + name;
        }
      }
      return {};
    }

    uint64_t Key(const NativeShader *ps, uint32_t mode) { return ps->hash ^ (uint64_t(mode) << 60); }

    // RB_BLENDCONTROL0 blends (anything but ONE / ZERO / ADD for colour and alpha, the PSO's test) and writes something.
    bool Blends(const RenderState &rs)
    {
      return (rs.blendcontrol[0] & 0x1FFF1FFFu) != 0x00010001u && (rs.color_mask & 0xFu) != 0;
    }
  } // namespace

  uint32_t CurrentMode() { return uint32_t(std::clamp(REXCVAR_GET(pdz_native_7e3_blend), 0, 2)); }

  bool RovSupported()
  {
    if (g_rov_supported >= 0)
    {
      return g_rov_supported == 1;
    }
    NativeDevice *dev = NativeDevice::Get();
    if (!dev || !dev->device())
    {
      return false; // not latched: checked again once the device exists
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS options = {};
    const bool options_ok =
        SUCCEEDED(dev->device()->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)));
    D3D12_FEATURE_DATA_FORMAT_SUPPORT format = {};
    format.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    const bool format_ok =
        SUCCEEDED(dev->device()->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &format, sizeof(format)));
    const bool rov = options_ok && options.ROVsSupported;
    const bool typed_load = options_ok && options.TypedUAVLoadAdditionalFormats && format_ok &&
                            (format.Support1 & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW) &&
                            (format.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD) &&
                            (format.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE);
    g_rov_supported = (rov && typed_load) ? 1 : 0;
    REXLOG_INFO("pdz_native: 7e3 blend: rasterizer-ordered views {}, typed UAV load / store of R16G16B16A16_FLOAT {}: "
                "mode 2 {}",
                rov ? "yes" : "NO", typed_load ? "yes" : "NO", g_rov_supported ? "available" : "falls back to mode 1");
    return g_rov_supported == 1;
  }

  bool WantUavSurfaces() { return CurrentMode() == kRov && RovSupported(); }

  bool RoundResolves() { return CurrentMode() == kRov || REXCVAR_GET(pdz_native_7e3_resolve_round); }

  uint32_t Wanted(const NativeShader *ps, const RenderState &rs, const TargetFormats &tf)
  {
    const uint32_t mode = CurrentMode();
    if (mode == kOff || !ps || ps->dxil.empty() || !ps->usable || !(ps->color_outputs & 1u) || ps->writes_depth ||
        tf.rt_count == 0 || !tf.rt0_7e3 || !Blends(rs))
    {
      return kOff;
    }
    if (mode == kAlphaClamp)
    {
      return kAlphaClamp;
    }
    // Mode 2: the output merger moves into the shader with early depth / stencil, so the draw must not write depth or
    // stencil (a kill after the early test would still have written them), and RT0 must be the only colour target.
    const bool has_ds = tf.ds != DXGI_FORMAT_UNKNOWN;
    reg::RB_DEPTHCONTROL dc;
    dc.value = rs.depthcontrol;
    if (has_ds && dc.z_enable && dc.z_write_enable)
    {
      ++g_stats.fallback_depth;
      return kAlphaClamp;
    }
    if (has_ds && dc.stencil_enable)
    {
      reg::RB_STENCILREFMASK srm;
      srm.value = rs.stencilrefmask;
      const auto keep = [](xenos::StencilOp op) { return op == xenos::StencilOp::kKeep; };
      bool modifies = !keep(dc.stencilfail) || !keep(dc.stencilzfail) || !keep(dc.stencilzpass);
      if (dc.backface_enable)
      {
        modifies = modifies || !keep(dc.stencilfail_bf) || !keep(dc.stencilzfail_bf) || !keep(dc.stencilzpass_bf);
      }
      if (modifies && srm.stencilwritemask != 0)
      {
        ++g_stats.fallback_stencil;
        return kAlphaClamp;
      }
    }
    if (tf.rt_count != 1)
    {
      ++g_stats.fallback_mrt;
      return kAlphaClamp;
    }
    if (!RovSupported())
    {
      ++g_stats.fallback_device;
      return kAlphaClamp;
    }
    if (!tf.rt0_uav)
    {
      ++g_stats.fallback_surface;
      return kAlphaClamp;
    }
    return kRov;
  }

  const std::vector<uint8_t> *Variant(const NativeShader *ps, uint32_t mode)
  {
    if (!ps || (mode != kAlphaClamp && mode != kRov))
    {
      return nullptr;
    }
    const uint64_t key = Key(ps, mode);
    auto it = g_variants.find(key);
    if (it != g_variants.end())
    {
      return it->second.failed ? nullptr : &it->second.dxil;
    }
    Entry &e = g_variants[key];
    e.failed = true;
    std::string why, source;
    if (!ps->coissue_hlsl.empty())
    {
      // The co-issue fix recompiled this PS (native_coissue.h): the variant starts from the patched source.
      source = BuildVariantSource(ps->coissue_hlsl, mode, why);
    }
    else
    {
      const std::string path = FindHlsl(ps);
      std::string hlsl;
      if (path.empty() || !ReadText(path, hlsl))
      {
        why = "no .hlsl next to the shader";
      }
      else
      {
        source = BuildVariantSource(hlsl, mode, why);
      }
    }
    // On-disk cache keyed by the whole variant source: a changed .hlsl or helper never reuses a stale file.
    std::string cache_path;
    bool from_disk = false;
    if (!source.empty())
    {
      cache_path = fmt::format("{}\\blend7e3\\{:016x}_{}_{:016x}.dxil", ShaderCache::Get().directory(), ps->hash, mode,
                               XXH3_64bits(source.data(), source.size()));
      from_disk = ReadBinary(cache_path, e.dxil) && depth20e4::IsSignedDxil(e.dxil);
      if (!from_disk)
      {
        e.dxil.clear();
      }
    }
    const auto t0 = std::chrono::steady_clock::now();
    if (!source.empty() && !from_disk)
    {
      std::string errors;
      if (!depth20e4::CompilePixelShader(source, e.dxil, errors))
      {
        why = "compile: " + errors.substr(0, 600);
      }
      else if (!depth20e4::batch::Collecting()) // the collect pass's placeholder never goes to disk
      {
        depth20e4::SaveCacheFile(cache_path, e.dxil);
      }
    }
    const uint32_t ms =
        uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
    g_stats.compile_ms += ms;
    if (source.empty() || !why.empty())
    {
      ++g_stats.failed;
      e.dxil.clear();
      if (g_stats.failed <= kLogBuilds && !depth20e4::batch::Collecting())
      {
        REXLOG_WARN("pdz_native: 7e3 blend: no mode {} variant for ps {:016x}: {}{}", mode, ps->hash, why,
                    g_stats.failed == kLogBuilds ? "; further failures only in the periodic counters" : "");
      }
      return nullptr;
    }
    e.failed = false;
    ++g_stats.built;
    if (from_disk)
    {
      ++g_stats.from_disk;
    }
    if (!from_disk && g_stats.built > kLogBuilds && REXCVAR_GET(pdz_native_first_use_log) &&
        g_first_use_logged < kFirstUseLogLines && !depth20e4::batch::Collecting())
    {
      // Not in the disk cache: compiled at the draw's first use (the cost a stutter hunt looks for).
      ++g_first_use_logged;
      REXLOG_INFO("pdz_native: 7e3 blend: mode {} variant for ps {:016x} compiled at first use in {} ms (not in the "
                  "disk cache){}",
                  mode, ps->hash, ms,
                  g_first_use_logged == kFirstUseLogLines ? "; further ones only in the periodic counters" : "");
    }
    if (g_stats.built <= kLogBuilds && !depth20e4::batch::Collecting())
    {
      REXLOG_INFO("pdz_native: 7e3 blend: mode {} variant {} for ps {:016x} ({} bytes DXIL, {} in {} ms){}", mode,
                  g_stats.built, ps->hash, e.dxil.size(), from_disk ? "read from the disk cache" : "compiled", ms,
                  g_stats.built == kLogBuilds ? "; further builds only in the periodic counters" : "");
    }
    return &e.dxil;
  }

  void MarkFailed(const NativeShader *ps, uint32_t mode, const char *why)
  {
    if (!ps)
    {
      return;
    }
    Entry &e = g_variants[Key(ps, mode)];
    if (e.failed)
    {
      return;
    }
    e.failed = true;
    e.dxil.clear();
    if (g_stats.built)
    {
      --g_stats.built;
    }
    ++g_stats.failed;
    REXLOG_WARN("pdz_native: 7e3 blend: mode {} variant for ps {:016x} retired: {}", mode, ps->hash, why);
  }

  void Constants(const RenderState &rs, uint32_t out[kConstantCount])
  {
    reg::RB_COLOR_INFO ci;
    ci.value = rs.color_info;
    // The SDK's color_exp_bias scale: float bits 0x3F800000 + (bias << 23) = 2^bias (bias -32..31).
    const float exp_scale = std::ldexp(1.0f, int(ci.color_exp_bias));
    out[0] = rs.blendcontrol[0] & 0x1FFF1FFFu;
    out[1] = rs.color_mask & 0xFu;
    std::memcpy(&out[2], &exp_scale, 4);
    out[3] = 0;
    std::memcpy(&out[4], rs.blend_factor, 16);
  }

  void CountDraw(uint32_t mode)
  {
    if (mode < 3)
    {
      ++g_stats.draws[mode];
    }
  }

  std::string StatsText()
  {
    return fmt::format("7e3 blend mode {}: variants {} ({} from disk, failed {}, {} ms) draws alpha-clamp {} rov {}, "
                       "rov fallbacks: depth write {} stencil write {} targets {} surface {} device {} variant {}",
                       CurrentMode(), g_stats.built, g_stats.from_disk, g_stats.failed, g_stats.compile_ms,
                       g_stats.draws[kAlphaClamp], g_stats.draws[kRov], g_stats.fallback_depth,
                       g_stats.fallback_stencil, g_stats.fallback_mrt, g_stats.fallback_surface,
                       g_stats.fallback_device, g_stats.fallback_variant);
  }

  void CountVariantFallback() { ++g_stats.fallback_variant; }

  void ResetForSeed()
  {
    g_variants.clear();
    g_stats = Stats();
    g_first_use_logged = 0;
  }
} // namespace pdz::native::blend7e3
