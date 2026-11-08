// Native renderer (stage 1): the texture cache (native_textures.h).
//
// Guest texture addressing follows the SDK (BSD-3): include\rex\graphics\pipeline\texture\address.h
// (Tiled2D / Tiled3D) and src\graphics\pipeline\texture\util.cpp (GetGuestTextureLayout, GetPackedMipOffset),
// reimplemented here for the formats PDZ uses. Untiling is done on the CPU (stage 1), once per texture
// content (key = base, format, size, mips, XXH3 of the first 4 KB); the GPU-side untiler can replace the
// inner loop later without changing the interface.
#include "native_dxc.h" // NativeCacheDir
#include "native_textures.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <windows.h>

#include <psapi.h> // PROCESS_MEMORY_COUNTERS (K32GetProcessMemoryInfo fetched at run time, as native_memlog.cpp)

#include <fmt/format.h>
#include <rex/cvar.h>

#include <rex/logging.h>
#include <rex/runtime.h> // pdz_native_texture_discover: the guest heap scan
#include <rex/hash.h> // xxhash.h with XXH_INLINE_ALL

#include "native.h"
#include "native_async_upload.h" // pdz_native_async_upload: textures prepared on the upload workers
#include "native_shaders.h" // pdz_native_texture_prewarm: the level-start summary's shader first-use counters
#include "native_draw.h" // SplitActive (split screen diagnostics)
#include "native_pipeline.h" // the level-start summary's PSO counts (prepared by the warm-up / built at the draw)
#include "native_recthread.h" // OnTextureWrite: texture locks in draw order
#include "native_surfaces.h"
#include "native_view.h" // DecodeTextureObject: the record thread's copy of the object
#include "../frame_timing.h" // pdz_frame_timing_phases: tex.hash / tex.upload
#include "../cpu_placement.h" // pdz_native_texture_discover: the scan thread at background priority

REXCVAR_DEFINE_BOOL(pdz_native_texture_log, false, "Render",
                    "Native renderer: log every new guest texture's fetch constant (format, endian, tiling, swizzle, "
                    "size, pitch, mips; first 400).");
REXCVAR_DEFINE_STRING(pdz_native_texture_dump_dir, "", "Render",
                      "Native renderer: dump each new guest texture's mip 0 (raw guest bytes, the native decode as "
                      "uploaded, layout .json) to this folder for tools\\glyphs\\compare_texture.py. Empty = off.");
REXCVAR_DEFINE_INT32(pdz_native_texture_dump_max, 400, "Render",
                     "Native renderer: at most this many textures are dumped per run (pdz_native_texture_dump_dir).");
REXCVAR_DEFINE_BOOL(pdz_native_stacked_layer_clamp, true, "Render",
                    "Native renderer: a 2D / stacked fetch constant gets AddressW = CLAMP (the SDK clamps a stacked "
                    "tfetch3D layer to [0.5, depth - 0.5] whatever clamp_z says). Only reaches draws through the "
                    "pdz_native_volume_view Texture3D copy (W is unused by 2D views). Mansion ice / water normals.");
REXCVAR_DEFINE_BOOL(pdz_native_texture_check_stagger, true, "Render",
                    "Native renderer (performance pass, genuine fix): the every-8-frames full re-check of a cached "
                    "texture (its data past the first 4 KB) runs on a phase of its own: at its first re-check the "
                    "next one is moved 0-7 frames earlier (from its address), so the hundreds of textures a level "
                    "creates in one frame are not all re-hashed on the same frame every 8 frames (Rooftops: up to "
                    "26 MB / 2.9 ms in one frame, logs 231). No re-check comes later than before. Off = the old "
                    "cadence.");
REXCVAR_DEFINE_BOOL(pdz_native_texture_volatile_check, true, "Render",
                    "Native renderer: linear textures (CPU-written: video planes, UI ramps) and textures whose data once "
                    "changed past their first 4 KB are compared with guest memory every frame instead of every 8 "
                    "(the first-4 KB key missed video frames whose top rows do not change: the intro / briefing videos "
                    "were sampled up to 8 frames stale), and a linear texture is hashed over the rows it holds, not its "
                    "32-row aligned slice (a 128x1 ramp no longer re-uploads when its neighbours change). Off = the "
                    "previous behaviour (stage1/subagents/bink.md).");
REXCVAR_DEFINE_BOOL(pdz_native_volume_sdk_layers, true, "Render",
                    "Native renderer: the pdz_native_volume_view Texture3D copy of a 2D / stacked texture follows the "
                    "SDK's stacked sampling (dxbc_translator_fetch.cpp): one slice per layer when the fetch constant's "
                    "volume filter is linear (the SDK lerps the two layers around z * layers - 0.5; a linear Texture3D "
                    "does the same; every PDZ stacked fetch constant seen says linear), 8 replicas per layer assigned "
                    "by floor(z * layers) when it says point, and the texture's own mip chain (volume mip m holds one "
                    "layer of every 2^m). Off = the pre-stacked copy: 8 replicas assigned by round(z * layers), which "
                    "puts a 50 / 50 blend at every layer centre, and the base level only (stage1/subagents/stacked.md).");
REXCVAR_DEFINE_BOOL(pdz_native_aniso_override, true, "Render",
                    "Native renderer: the samplers follow the SDK's anisotropic_override GPU cvar (default 3 = 4x, also "
                    "in settings\\hardware.toml) like D3D12TextureCache::GetSamplerParameters: a texture with mips, "
                    "min and mag filter linear and a point or linear mip filter is sampled with that anisotropy "
                    "whatever its fetch constant says. PDZ's fetch constants leave anisotropy off, so without it the "
                    "native frame is trilinear where the emulator is 4x anisotropic (oblique ground, leaves, the "
                    "G-buffer normals and the specular they drive). Off: the fetch constant's anisotropy only "
                    "(stage1/subagents/jungle.md).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(pdz_native_packed_tail_mips, true, "Render",
                    "Native renderer: a texture whose level 0 already is the packed mip tail (smaller side 16 texels "
                    "or less, packed mips) keeps the mips its fetch constant asks for, read from the base address: D3D "
                    "leaves the mip address at 0 for such a texture and stores the rest of the tail at the base "
                    "address too (SDK texture_util::GetSubresourcesFromFetchConstant, mip_page = base_page). PDZ's "
                    "small DXT textures are like that (8x8 / 16x16 / 8x64 / 2x2, mips 0..1 to 0..4, mip address 0: the "
                    "texture log of run 034). Off = the old decode: no mip address, no mips (sampled at level 0 when "
                    "minified, where the emulator filters the tail's levels) (stage1/subagents/subway2.md).");
REXCVAR_DEFINE_BOOL(pdz_native_texture_supersede, true, "Render",
                    "Native renderer: when the upload cache creates a texture, the older contents of the same guest "
                    "texture (same key except the first-4 KB content hash: address, format, size, layout, view kind) not "
                    "used in this frame are freed at once, as the volatile path already did for linear textures. A tiled "
                    "texture whose first 4 KB change every frame got a new entry every frame, kept until the 900-frame "
                    "eviction: Jungle evicted 300 every 300 frames with about 1190 cached (log 162; memlog.md N5). Only "
                    "an older content goes, which guest memory no longer holds; if the same bytes come back they are "
                    "uploaded again. Off = only volatile textures replace their older contents (the old behaviour).");
// AMD garbage fixes (on by default; each is a pure correctness fix: the same picture on every GPU vendor). New
// textures are created without zeroing (CREATE_NOT_ZEROED: AMD hands back recycled memory, often an old picture;
// NVIDIA usually zero), so anything sampled that was never written must be written first.
REXCVAR_DEFINE_BOOL(pdz_fix_texture_unwritten, true, "Render",
                    "Native renderer fix (on): a texture mip level or array slice the upload never writes (a level "
                    "past the end of guest memory or without an address) is written as zero instead of keeping what "
                    "the new memory held (garbage on AMD)");
REXCVAR_DEFINE_BOOL(pdz_fix_mirror_view_zero, true, "Render",
                    "Native renderer fix (on): a resolve mirror's alias view for a fetch of another size (its "
                    "texels outside the resolved region) starts as zero, as the code always assumed, instead of the "
                    "new memory's old content (garbage on AMD)");
REXCVAR_DEFINE_BOOL(pdz_native_texture_untile_cached, false, "Render",
                    "Native renderer: the per-block untiler (tiled textures, and linear ones with a format conversion) "
                    "builds each row in a cached scratch buffer and copies the finished row into the upload heap with "
                    "one memcpy, as the linear row path already did. Off = the old loop, which byte-swaps and converts "
                    "every block in place in the upload heap: that memory is write-combined (uncached), so each block "
                    "is written, read back (a WC flush plus uncached reads) and written again, about 120-240 ms per MB "
                    "uploaded: a 0.12 MB texture created mid-match stalled the record thread ~29 ms, 20 at once 384 ms "
                    "(stage2/m5/m5_texstall.md). The bytes uploaded are the same either way.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(pdz_native_texture_lock_check, true, "Render",
                    "Native renderer: a texture the game locks for writing (the D3D texture lock; the font atlas gets "
                    "new glyphs that way while the menus scroll) is compared with guest memory at its next use and "
                    "re-uploaded if it changed, also later in the same frame. Without it a draw after the write could "
                    "sample the old texture: for the rest of the frame (the per-frame fast path), or up to 8 frames "
                    "when the change lies past the first 4 KB (the full re-check cadence): jumbled text for a split "
                    "second (stage2/polish/textflicker.md). Off = the old behaviour; with pdz_frame_timing_spike_ms on, "
                    "the draws that sampled stale content are then counted.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(pdz_native_texture_lock_tables, true, "Render",
                    "Native renderer: with pdz_native_texture_lock_check, a texture the game locks for writing after "
                    "draws of the same frame sampled it (the font atlas rewritten several times in one frame while the "
                    "level list scrolls fast) starts a new SRV table epoch: the draws after the write build their "
                    "descriptor tables again and get the new upload, the draws before it keep the version they were "
                    "recorded with. Without it a draw after the write with the same shaders and fetch constants reused "
                    "the table built before the write (jumbled text, stage2/polish/textflicker.md section 5). A texture "
                    "that gets two or more versions in one frame logs 'pdz_native: texture ... versions in frame'. Off "
                    "= tables reused across the write (the first fix alone).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(pdz_native_texture_keep_seconds, 60, "Render",
                     "Upload-cache textures unused this long are freed (log 307: at 120 fps the old 900-frame rule freed "
                     "textures after 7.5 s, 30-126 every 2.5 s in Nightclub, and walking back to them re-created them all "
                     "in one frame). At least 900 frames, as before; 0 = the 900-frame rule only. While the cache holds "
                     "more than pdz_native_texture_keep_mb, the 900-frame rule applies")
    .range(0, 3600);
REXCVAR_DEFINE_INT32(pdz_native_texture_keep_mb, 1024, "Render",
                     "pdz_native_texture_keep_seconds: above this many MB of upload-cache textures, unused textures go "
                     "after 900 frames again")
    .range(64, 16384);
REXCVAR_DEFINE_BOOL(pdz_native_texture_prewarm, true, "Rendering",
                    "Native renderer (log 315): the textures a level used in its first 90 s are remembered (native_cache\\"
                    "texture_prewarm\\<level>.bin, written every 30 s and when the level ends) and, the next time that "
                    "level starts, prepared on the upload workers during the loading screen and the opening cutscene "
                    "(each entry checked against guest memory first; entries whose data isn't loaded yet are retried for "
                    "60 s), so the first frames of play don't wait for their uploads. Prepared textures are kept pending "
                    "up to 3 minutes (the usual 5 s) until a draw takes them. One 'pdz_native: prewarm' line per level "
                    "and a 'level start' summary 30 s in. Off = as before (the set is still recorded)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(pdz_native_texture_discover, true, "Rendering",
                    "Native renderer (2026-11-03, docs/RELEASE_FIRST_RUN.md Stage 5): during a level's loading screen a "
                    "worker thread scans the guest heap every 0.4 s for the D3D texture objects the loading creates "
                    "(type 3 in the object's first dword; their fetch constant is what the draws will use) and, on a "
                    "later play, for objects holding the content the level used last time (pdz_native_texture_prewarm's "
                    "set, matched by content: the textures load at other addresses each run). Each one is prepared on "
                    "the upload workers as a prefetch would be, so the level's first frames take them ready instead of "
                    "creating 170-300 textures at once. A draw takes one only with its exact key and after its whole "
                    "base level is compared again. One 'pdz_native: texture discovery' line per level. Off = as before")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(pdz_native_texture_discover_mb, 256, "Rendering",
                     "pdz_native_texture_discover: at most this many MB of textures prepared ahead per level (a first "
                     "play: Datacore's loading makes about 1,300, 97 MB). A later play prepares only the level's own "
                     "set from last time")
    .range(0, 4096)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(pdz_native_texture_prewarm_mb, 512, "Rendering",
                     "pdz_native_texture_prewarm: at most this many MB of prepared-ahead textures pending at once (the "
                     "rest follow as draws take them)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(pdz_native_prefetch_skip_recent, true, "Rendering",
                    "Native renderer (overnight 30-31 Oct): the async upload prefetch (a hint) skips a fetch constant "
                    "whose texture was validated this or the previous frame, instead of hashing its first 4 KB a second "
                    "time per frame. The draw's own lookup checks it as before. Off = every queued texture hashed")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(pdz_native_texture_watch, true, "Rendering",
                    "Native renderer (overnight 30-31 Oct, with pdz_native_geometry_watch's write watches): a texture "
                    "whose full re-check found the same bytes twice in a row gets a write watch on its base level; while "
                    "nothing wrote it, its first-4-KB hash and its full re-check are skipped. A write, a texture lock, a "
                    "protect / free invalidates it and the checks run as before. Volatile textures are never watched. "
                    "Off = every texture hashed as before")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(pdz_native_texture_watch_verify, false, "Rendering",
                    "Test aid for pdz_native_texture_watch: a texture used without its hashes is hashed anyway and "
                    "compared; mismatches are counted in the 5 s uploads line and the first 8 logged (expected 0)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DECLARE(int32_t, pdz_frame_timing_skip); // frame_timing.cpp: the texture creation log starts after it
REXCVAR_DECLARE(int32_t, pdz_frame_timing_spike_ms); // frame_timing.cpp: the lock counters' periodic line
REXCVAR_DECLARE(bool, pdz_native_depth_stencil_alias); // native_surfaces.cpp: the log line of NoteStencilAliasFetch

namespace pdz::native
{
  // ---------------------------------------------------------------------------
  // Fetch constant decoding (xenos.h xe_gpu_texture_fetch_t bit layout)
  // ---------------------------------------------------------------------------
  namespace
  {
    inline uint32_t Bits(uint32_t v, uint32_t shift, uint32_t count) { return (v >> shift) & ((1u << count) - 1u); }
    inline int32_t SignedBits(uint32_t v, uint32_t shift, uint32_t count)
    {
      const uint32_t raw = Bits(v, shift, count);
      return (raw & (1u << (count - 1))) ? int32_t(raw) - int32_t(1u << count) : int32_t(raw);
    }
    inline uint32_t Log2Ceil(uint32_t v)
    {
      uint32_t r = 0;
      while ((1u << r) < v)
      {
        ++r;
      }
      return r;
    }
    inline uint32_t Log2Floor(uint32_t v)
    {
      uint32_t r = 0;
      while (v > 1)
      {
        v >>= 1;
        ++r;
      }
      return r;
    }
    inline uint32_t NextPow2(uint32_t v) { return 1u << Log2Ceil(v); }
    inline uint32_t Align(uint32_t v, uint32_t a) { return (v + a - 1) / a * a; }
    // GPU (physical) address of a guest address, the way the game's D3D converts a CPU address before it goes to
    // the GPU: (a & 0x1FFFFFFF) + (((a >> 20) + 0x200) & 0x1000), i.e. the 0xE0000000 4 KB-page view is 4 KB
    // above its mask (rlwinm 12,20,31 / addi 512 / rlwinm 0,19,19 / add in the D3D command writers, e.g. generated
    // perfectdarkzerorecomp_recomp.112.cpp:17439; shadow_assert.cpp GuestToPacketAddress, sub_826F9578 /
    // sub_826FA2E8). The SDK maps it the same way (xmemory.cpp map_info 0xE0000000 -> 0x100001000,
    // PhysicalHeap::GetPhysicalAddress +0x1000). Physical addresses (< 0x20000000, what the device fetch constants
    // hold) and the 0xA0000000 / 0xC0000000 views are unchanged by it.
    inline uint32_t GpuAddress(uint32_t a) { return (a & 0x1FFFFFFFu) + (((a >> 20) + 0x200u) & 0x1000u); }
  } // namespace

  FetchDecoded DecodeFetchDwords(const uint32_t d[6])
  {
    FetchDecoded f;
    std::memcpy(f.dwords, d, sizeof(f.dwords));
    f.type = Bits(d[0], 0, 2);
    f.signs = Bits(d[0], 2, 8);
    f.clamp_x = Bits(d[0], 10, 3);
    f.clamp_y = Bits(d[0], 13, 3);
    f.clamp_z = Bits(d[0], 16, 3);
    f.pitch = Bits(d[0], 22, 9) << 5;
    f.tiled = Bits(d[0], 31, 1) != 0;
    f.format = Bits(d[1], 0, 6);
    f.endian = Bits(d[1], 6, 2);
    f.stacked = Bits(d[1], 10, 1) != 0;
    // Physical addresses (29 bits). A D3D texture object can hold the address in the 0xA0000000+ physical
    // view (BB520000 for the fetch constant's 1B520000): unmasked, resolve mirrors (keyed by the object's
    // address) never matched the draws' fetch constants and every resolved texture was sampled from guest
    // memory (the native world was black, pass dump run1). An object in the 0xE0000000 view needs the +4 KB of
    // GpuAddress, or its mirror is keyed 4 KB below the address D3D puts in the fetch constant (finding 8).
    f.base_address = GpuAddress(Bits(d[1], 12, 20) << 12);
    f.num_format = Bits(d[3], 0, 1);
    f.swizzle = Bits(d[3], 1, 12);
    f.exp_adjust = SignedBits(d[3], 13, 6);
    f.mag_filter = Bits(d[3], 19, 2);
    f.min_filter = Bits(d[3], 21, 2);
    f.mip_filter = Bits(d[3], 23, 2);
    f.aniso_filter = Bits(d[3], 25, 3);
    f.vol_mag_filter = Bits(d[4], 0, 1);
    f.vol_min_filter = Bits(d[4], 1, 1);
    f.mip_min_level = Bits(d[4], 2, 4);
    f.mip_max_level = Bits(d[4], 6, 4);
    f.lod_bias = SignedBits(d[4], 12, 10);
    f.border_color = Bits(d[5], 0, 2);
    f.dimension = Bits(d[5], 9, 2);
    f.packed_mips = Bits(d[5], 11, 1) != 0;
    f.mip_address = GpuAddress(Bits(d[5], 12, 20) << 12);
    switch (f.dimension)
    {
    case 0: // 1D
      f.width = Bits(d[2], 0, 24) + 1;
      f.height = 1;
      f.depth = 1;
      break;
    case 2: // 3D
      f.width = Bits(d[2], 0, 11) + 1;
      f.height = Bits(d[2], 11, 11) + 1;
      f.depth = Bits(d[2], 22, 10) + 1;
      break;
    case 3: // cube
      f.width = Bits(d[2], 0, 13) + 1;
      f.height = Bits(d[2], 13, 13) + 1;
      f.depth = 6;
      break;
    default: // 2D or stacked
      f.width = Bits(d[2], 0, 13) + 1;
      f.height = Bits(d[2], 13, 13) + 1;
      f.depth = f.stacked ? Bits(d[2], 26, 6) + 1 : 1;
      break;
    }
    // Level 0 already the packed tail (GetPackedMipLevel(width, height) == 0: the smaller side is 16 texels or
    // less): D3D leaves the mip address at 0 and keeps the rest of the tail at the base address, so the mips are read
    // from there (GetSubresourcesFromFetchConstant: mip_page = base_page). ComputeLevel then finds every level inside
    // the base level's storage (storage level = packed level 0) at its packed offset, as the SDK does.
    if (f.mip_address == 0 && f.base_address != 0 && f.packed_mips && f.dimension != 0 &&
        Log2Ceil(std::min(f.width, f.height)) <= 4 && REXCVAR_GET(pdz_native_packed_tail_mips))
    {
      f.mip_address = f.base_address;
    }
    // Mip range the same way the SDK cleans it up (GetSubresourcesFromFetchConstant): no mip address means
    // no mips; the base level is always level 0 here.
    const uint32_t longest = std::max(f.width, std::max(f.height, f.dimension == 2 ? f.depth : 1u));
    const uint32_t size_max_level = Log2Floor(longest);
    if (f.mip_address == 0)
    {
      f.mip_min_level = 0;
      f.mip_max_level = 0;
    }
    else
    {
      f.mip_min_level = std::min(f.mip_min_level, size_max_level);
      f.mip_max_level = std::max(std::min(f.mip_max_level, size_max_level), f.mip_min_level);
    }
    return f;
  }

  FetchDecoded DecodeFetch(const uint8_t *fetch_be24)
  {
    uint32_t d[6];
    for (uint32_t i = 0; i < 6; ++i)
    {
      const uint8_t *p = fetch_be24 + i * 4;
      d[i] = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
    }
    return DecodeFetchDwords(d);
  }

  FetchDecoded DecodeTextureObject(const uint8_t *base, uint32_t texture_object)
  {
    uint32_t d[6] = {};
    if (IsGuestPointer(texture_object))
    {
      for (uint32_t i = 0; i < 6; ++i)
      {
        d[i] = view::Load32(base, texture_object + 16 + 4 * i); // pdz_native_record_thread: the hook's copy
      }
    }
    return DecodeFetchDwords(d);
  }

  // ---------------------------------------------------------------------------
  // Formats
  // ---------------------------------------------------------------------------
  namespace
  {
    enum Swz : uint16_t
    {
      kSwzRGBA = 0 | (1 << 3) | (2 << 6) | (3 << 9),
      kSwzRRRR = 0,
      kSwzRGGG = 0 | (1 << 3) | (1 << 6) | (1 << 9),
      kSwzRGBB = 0 | (1 << 3) | (2 << 6) | (2 << 9),
    };
    enum class Convert : uint8_t
    {
      kNone,
      kSwapRB16_1555, // B5G5R5A1 host: swap the 5-bit R and B fields
      kSwapRB16_565,
      kSwapRB16_4444,
      kDepthUnorm24,  // k_24_8: (dword >> 8) / 2^24 - 1 -> float
      kDepthFloat24,  // k_24_8_FLOAT: 20e4 -> float
    };
    struct FormatInfo
    {
      uint8_t block_w, block_h, bytes_per_block;
      DXGI_FORMAT unorm, snorm, srgb;
      uint16_t host_swizzle;
      Convert convert;
      bool supported;
      const char *name;
    };
    // Indexed by xenos::TextureFormat.
    const FormatInfo kFormats[64] = {
        {1, 1, 1, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kSwzRRRR, Convert::kNone, false, "k_1_REVERSE"},
        {1, 1, 1, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kSwzRRRR, Convert::kNone, false, "k_1"},
        {1, 1, 1, DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8_SNORM, DXGI_FORMAT_UNKNOWN, kSwzRRRR, Convert::kNone, true, "k_8"},
        {1, 1, 2, DXGI_FORMAT_B5G5R5A1_UNORM, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kSwzRGBA, Convert::kSwapRB16_1555, true, "k_1_5_5_5"},
        {1, 1, 2, DXGI_FORMAT_B5G6R5_UNORM, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kSwzRGBB, Convert::kSwapRB16_565, true, "k_5_6_5"},
        {1, 1, 2, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kSwzRGBB, Convert::kNone, false, "k_6_5_5"},
        {1, 1, 4, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_SNORM, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, kSwzRGBA, Convert::kNone, true, "k_8_8_8_8"},
        {1, 1, 4, DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kSwzRGBA, Convert::kNone, true, "k_2_10_10_10"},
        {1, 1, 1, DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8_SNORM, DXGI_FORMAT_UNKNOWN, kSwzRRRR, Convert::kNone, true, "k_8_A"},
        {1, 1, 1, DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8_SNORM, DXGI_FORMAT_UNKNOWN, kSwzRRRR, Convert::kNone, true, "k_8_B"},
        {1, 1, 2, DXGI_FORMAT_R8G8_UNORM, DXGI_FORMAT_R8G8_SNORM, DXGI_FORMAT_UNKNOWN, kSwzRGGG, Convert::kNone, true, "k_8_8"},
        {2, 1, 4, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kSwzRGBB, Convert::kNone, false, "k_Cr_Y1_Cb_Y0_REP"},
        {2, 1, 4, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kSwzRGBB, Convert::kNone, false, "k_Y1_Cr_Y0_Cb_REP"},
        {1, 1, 4, DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kSwzRGGG, Convert::kNone, false, "k_16_16_EDRAM"},
        {1, 1, 4, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_SNORM, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, kSwzRGBA, Convert::kNone, true, "k_8_8_8_8_A"},
        {1, 1, 2, DXGI_FORMAT_B4G4R4A4_UNORM, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kSwzRGBA, Convert::kSwapRB16_4444, true, "k_4_4_4_4"},
        {1, 1, 4, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kSwzRGBB, Convert::kNone, false, "k_10_11_11"},
        {1, 1, 4, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kSwzRGBB, Convert::kNone, false, "k_11_11_10"},
        {4, 4, 8, DXGI_FORMAT_BC1_UNORM, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_BC1_UNORM_SRGB, kSwzRGBA, Convert::kNone, true, "k_DXT1"},
        {4, 4, 16, DXGI_FORMAT_BC2_UNORM, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_BC2_UNORM_SRGB, kSwzRGBA, Convert::kNone, true, "k_DXT2_3"},
        {4, 4, 16, DXGI_FORMAT_BC3_UNORM, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_BC3_UNORM_SRGB, kSwzRGBA, Convert::kNone, true, "k_DXT4_5"},
        {1, 1, 8, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kSwzRGBA, Convert::kNone, false, "k_16_16_16_16_EDRAM"},
        {1, 1, 4, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_UNKNOWN, kSwzRRRR, Convert::kDepthUnorm24, true, "k_24_8"},
        {1, 1, 4, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_UNKNOWN, kSwzRRRR, Convert::kDepthFloat24, true, "k_24_8_FLOAT"},
        {1, 1, 2, DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16_SNORM, DXGI_FORMAT_UNKNOWN, kSwzRRRR, Convert::kNone, true, "k_16"},
        {1, 1, 4, DXGI_FORMAT_R16G16_UNORM, DXGI_FORMAT_R16G16_SNORM, DXGI_FORMAT_UNKNOWN, kSwzRGGG, Convert::kNone, true, "k_16_16"},
        {1, 1, 8, DXGI_FORMAT_R16G16B16A16_UNORM, DXGI_FORMAT_R16G16B16A16_SNORM, DXGI_FORMAT_UNKNOWN, kSwzRGBA, Convert::kNone, true, "k_16_16_16_16"},
        {1, 1, 2, DXGI_FORMAT_R16_FLOAT, DXGI_FORMAT_R16_FLOAT, DXGI_FORMAT_UNKNOWN, kSwzRRRR, Convert::kNone, true, "k_16_EXPAND"},
        {1, 1, 4, DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_UNKNOWN, kSwzRGGG, Convert::kNone, true, "k_16_16_EXPAND"},
        {1, 1, 8, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_UNKNOWN, kSwzRGBA, Convert::kNone, true, "k_16_16_16_16_EXPAND"},
        {1, 1, 2, DXGI_FORMAT_R16_FLOAT, DXGI_FORMAT_R16_FLOAT, DXGI_FORMAT_UNKNOWN, kSwzRRRR, Convert::kNone, true, "k_16_FLOAT"},
        {1, 1, 4, DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_UNKNOWN, kSwzRGGG, Convert::kNone, true, "k_16_16_FLOAT"},
        {1, 1, 8, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_UNKNOWN, kSwzRGBA, Convert::kNone, true, "k_16_16_16_16_FLOAT"},
        {1, 1, 4, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kSwzRRRR, Convert::kNone, false, "k_32"},
        {1, 1, 8, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kSwzRGGG, Convert::kNone, false, "k_32_32"},
        {1, 1, 16, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kSwzRGBA, Convert::kNone, false, "k_32_32_32_32"},
        {1, 1, 4, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_UNKNOWN, kSwzRRRR, Convert::kNone, true, "k_32_FLOAT"},
        {1, 1, 8, DXGI_FORMAT_R32G32_FLOAT, DXGI_FORMAT_R32G32_FLOAT, DXGI_FORMAT_UNKNOWN, kSwzRGGG, Convert::kNone, true, "k_32_32_FLOAT"},
        {1, 1, 16, DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_UNKNOWN, kSwzRGBA, Convert::kNone, true, "k_32_32_32_32_FLOAT"},
        {1, 1, 4, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kSwzRRRR, Convert::kNone, false, "k_32_AS_8"},
        {1, 1, 4, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kSwzRGGG, Convert::kNone, false, "k_32_AS_8_8"},
        {1, 1, 2, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kSwzRRRR, Convert::kNone, false, "k_16_MPEG"},
        {1, 1, 4, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kSwzRGGG, Convert::kNone, false, "k_16_16_MPEG"},
        {1, 1, 1, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kSwzRRRR, Convert::kNone, false, "k_8_INTERLACED"},
        {1, 1, 4, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kSwzRRRR, Convert::kNone, false, "k_32_AS_8_INTERLACED"},
        {1, 1, 4, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kSwzRGGG, Convert::kNone, false, "k_32_AS_8_8_INTERLACED"},
        {1, 1, 2, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kSwzRRRR, Convert::kNone, false, "k_16_INTERLACED"},
        {1, 1, 2, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kSwzRRRR, Convert::kNone, false, "k_16_MPEG_INTERLACED"},
        {1, 1, 4, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kSwzRGGG, Convert::kNone, false, "k_16_16_MPEG_INTERLACED"},
        {4, 4, 16, DXGI_FORMAT_BC5_UNORM, DXGI_FORMAT_BC5_SNORM, DXGI_FORMAT_UNKNOWN, kSwzRGGG, Convert::kNone, true, "k_DXN"},
        {1, 1, 4, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_SNORM, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, kSwzRGBA, Convert::kNone, true, "k_8_8_8_8_AS_16_16_16_16"},
        {4, 4, 8, DXGI_FORMAT_BC1_UNORM, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_BC1_UNORM_SRGB, kSwzRGBA, Convert::kNone, true, "k_DXT1_AS_16_16_16_16"},
        {4, 4, 16, DXGI_FORMAT_BC2_UNORM, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_BC2_UNORM_SRGB, kSwzRGBA, Convert::kNone, true, "k_DXT2_3_AS_16_16_16_16"},
        {4, 4, 16, DXGI_FORMAT_BC3_UNORM, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_BC3_UNORM_SRGB, kSwzRGBA, Convert::kNone, true, "k_DXT4_5_AS_16_16_16_16"},
        {1, 1, 4, DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kSwzRGBA, Convert::kNone, true, "k_2_10_10_10_AS_16_16_16_16"},
        {1, 1, 4, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kSwzRGBB, Convert::kNone, false, "k_10_11_11_AS_16_16_16_16"},
        {1, 1, 4, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kSwzRGBB, Convert::kNone, false, "k_11_11_10_AS_16_16_16_16"},
        {1, 1, 12, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kSwzRGBB, Convert::kNone, false, "k_32_32_32_FLOAT"},
        {4, 4, 8, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kSwzRRRR, Convert::kNone, false, "k_DXT3A"},
        {4, 4, 8, DXGI_FORMAT_BC4_UNORM, DXGI_FORMAT_BC4_SNORM, DXGI_FORMAT_UNKNOWN, kSwzRRRR, Convert::kNone, true, "k_DXT5A"},
        {4, 4, 8, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kSwzRGGG, Convert::kNone, false, "k_CTX1"},
        {4, 4, 8, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kSwzRGBA, Convert::kNone, false, "k_DXT3A_AS_1_1_1_1"},
        {1, 1, 4, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_SNORM, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, kSwzRGBA, Convert::kNone, true, "k_8_8_8_8_GAMMA_EDRAM"},
        {1, 1, 4, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kSwzRGBA, Convert::kNone, false, "k_2_10_10_10_FLOAT_EDRAM"},
    };

    // Host format of a resolve mirror (RTV-capable, keeps the resolved values; no reinterpretation).
    DXGI_FORMAT MirrorFormat(uint32_t format)
    {
      switch (format)
      {
      case 7: case 54: return DXGI_FORMAT_R10G10B10A2_UNORM;
      case 22: case 23: case 36: return DXGI_FORMAT_R32_FLOAT;
      case 37: return DXGI_FORMAT_R32G32_FLOAT;
      case 38: return DXGI_FORMAT_R32G32B32A32_FLOAT;
      case 24: return DXGI_FORMAT_R16_UNORM;
      case 25: return DXGI_FORMAT_R16G16_UNORM;
      case 26: return DXGI_FORMAT_R16G16B16A16_UNORM;
      case 27: case 30: return DXGI_FORMAT_R16_FLOAT;
      case 13: case 28: case 31: return DXGI_FORMAT_R16G16_FLOAT;
      case 21: case 29: case 32: case 63: return DXGI_FORMAT_R16G16B16A16_FLOAT;
      case 2: case 8: case 9: return DXGI_FORMAT_R8_UNORM;
      case 10: return DXGI_FORMAT_R8G8_UNORM;
      default: return DXGI_FORMAT_R8G8B8A8_UNORM; // 8888 family, 4444, 1555, 565: 8-bit unorm host
      }
    }

    // ---------------------------------------------------------------------------
    // Tiled addressing (SDK texture_address::Tiled2D / Tiled3D)
    // ---------------------------------------------------------------------------
    inline int32_t TiledCombine(int32_t outer_inner_bytes, uint32_t bank, uint32_t pipe, uint32_t y_lsb)
    {
      return int32_t((y_lsb << 4) | (pipe << 6) | (bank << 11)) | (outer_inner_bytes & 0b1111) |
             (((outer_inner_bytes >> 4) & 0b1) << 5) | (((outer_inner_bytes >> 5) & 0b111) << 8) |
             (outer_inner_bytes >> 8 << 12);
    }
    inline int32_t Tiled2D(int32_t x, int32_t y, uint32_t pitch_aligned, uint32_t bpb_log2)
    {
      const int32_t outer_blocks = ((y >> 5) * int32_t(pitch_aligned >> 5) + (x >> 5)) << 6;
      const int32_t inner_blocks = (((y >> 1) & 0b111) << 3) | (x & 0b111);
      const int32_t outer_inner_bytes = (outer_blocks | inner_blocks) << bpb_log2;
      const uint32_t bank = (y >> 4) & 0b1;
      const uint32_t pipe = ((x >> 3) & 0b11) ^ (((y >> 3) & 0b1) << 1);
      return TiledCombine(outer_inner_bytes, bank, pipe, y & 1);
    }
    inline int64_t Tiled3D(int32_t x, int32_t y, int32_t z, uint32_t pitch_aligned, uint32_t height_aligned,
                           uint32_t bpb_log2)
    {
      const int32_t outer_blocks =
          ((((z >> 2) * int32_t(height_aligned >> 4) + (y >> 4)) * int32_t(pitch_aligned >> 5)) + (x >> 5)) << 7;
      const int32_t inner_blocks = ((z & 0b11) << 5) | (((y >> 1) & 0b11) << 3) | (x & 0b111);
      const int64_t outer_inner_bytes = int64_t(outer_blocks | inner_blocks) << bpb_log2;
      const uint32_t bank = ((y >> 3) ^ (z >> 2)) & 0b1;
      const uint32_t pipe = ((x >> 3) & 0b11) ^ (bank << 1);
      const int64_t low = int64_t((y & 1) << 4) | int64_t(pipe << 6) | int64_t(bank << 11);
      return low | (outer_inner_bytes & 0b1111) | (((outer_inner_bytes >> 4) & 0b1) << 5) |
             (((outer_inner_bytes >> 5) & 0b111) << 8) | (outer_inner_bytes >> 8 << 12);
    }

    // ---------------------------------------------------------------------------
    // Guest layout (SDK texture_util::GetGuestTextureLayout, trimmed to what the untiler needs)
    // ---------------------------------------------------------------------------
    inline uint32_t PackedMipLevel(uint32_t width, uint32_t height)
    {
      const uint32_t log2_size = Log2Ceil(std::min(width, height));
      return log2_size > 4 ? log2_size - 4 : 0;
    }

    // Offset of a packed level inside the packed tail, in texels; false when the level is not packed.
    bool PackedMipOffset(uint32_t width, uint32_t height, uint32_t depth, uint32_t mip, uint32_t &x_texels,
                         uint32_t &y_texels, uint32_t &z)
    {
      const uint32_t log2_width = Log2Ceil(width);
      const uint32_t log2_height = Log2Ceil(height);
      const uint32_t log2_size = std::min(log2_width, log2_height);
      x_texels = y_texels = z = 0;
      if (log2_size > 4 + mip)
      {
        return false;
      }
      const uint32_t packed_mip_base = log2_size > 4 ? log2_size - 4 : 0;
      const uint32_t packed_mip = mip - packed_mip_base;
      if (packed_mip < 3)
      {
        if (log2_width > log2_height)
        {
          y_texels = 16 >> packed_mip;
        }
        else
        {
          x_texels = 16 >> packed_mip;
        }
      }
      else
      {
        uint32_t offset;
        if (log2_width > log2_height)
        {
          offset = (1u << (log2_width - packed_mip_base)) >> (packed_mip - 2);
          x_texels = offset;
        }
        else
        {
          offset = (1u << (log2_height - packed_mip_base)) >> (packed_mip - 2);
          y_texels = offset;
        }
        if (offset < 4)
        {
          const uint32_t log2_depth = Log2Ceil(depth);
          z = (log2_depth > 1 + mip) ? (log2_depth - mip) * 4 : 4;
        }
      }
      return true;
    }

    struct LevelLayout
    {
      bool valid = false;
      uint32_t width_blocks = 0, height_blocks = 0, depth = 0; // extents of the level itself
      uint32_t pitch_blocks = 0;     // storage row pitch in blocks (tile aligned)
      uint32_t rows_aligned = 0;     // storage block rows per z slice (32 aligned)
      uint32_t z_stride_bytes = 0;   // between z slices
      uint32_t slice_stride_bytes = 0; // between array slices (4 KB aligned)
      uint32_t base_offset = 0;      // from base_address (level 0) or mip_address (mips)
      bool from_mips = false;
      uint32_t packed_x_blocks = 0, packed_y_blocks = 0, packed_z = 0;
    };

    // Storage of the level whose data holds `level` (the packed tail for packed levels).
    LevelLayout ComputeLevel(const FetchDecoded &f, const FormatInfo &fi, uint32_t level)
    {
      LevelLayout l;
      const bool is_3d = f.dimension == 2;
      const uint32_t depth_texels = is_3d ? f.depth : 1;
      const uint32_t packed_level = f.packed_mips ? PackedMipLevel(f.width, f.height) : ~0u;
      const bool packed = level >= packed_level;
      const uint32_t storage_level = packed ? packed_level : level;
      l.width_blocks = Align(std::max(f.width >> level, 1u), fi.block_w) / fi.block_w;
      l.height_blocks = Align(std::max(f.height >> level, 1u), fi.block_h) / fi.block_h;
      l.depth = std::max(depth_texels >> level, 1u);
      l.from_mips = level != 0;

      uint32_t pitch_texels, rows_texels;
      if (storage_level == 0)
      {
        pitch_texels = f.pitch ? f.pitch : Align(f.width, 32);
        rows_texels = packed_level == 0 ? NextPow2(f.height) : f.height;
      }
      else
      {
        pitch_texels = std::max(NextPow2(f.width) >> storage_level, 1u);
        rows_texels = std::max(NextPow2(f.height) >> storage_level, 1u);
      }
      uint32_t pitch_alignment = 32;
      if (!f.tiled && storage_level != 0)
      {
        pitch_alignment = std::max(256u / fi.bytes_per_block, 32u);
      }
      l.pitch_blocks = Align(Align(pitch_texels, fi.block_w) / fi.block_w, pitch_alignment);
      l.rows_aligned = f.dimension == 0 ? 1 : Align(Align(rows_texels, fi.block_h) / fi.block_h, 32);
      l.z_stride_bytes = l.pitch_blocks * fi.bytes_per_block * l.rows_aligned;
      uint32_t depth_storage = 1;
      if (is_3d)
      {
        depth_storage = Align(storage_level == 0 && packed_level != 0 ? depth_texels
                                                                      : std::max(NextPow2(depth_texels) >> storage_level, 1u),
                              4);
      }
      l.slice_stride_bytes = Align(l.z_stride_bytes * depth_storage, 4096);

      // Offset of the storage level inside the mip chain (levels 1.. are consecutive from mip_address).
      l.base_offset = 0;
      if (l.from_mips && storage_level >= 1)
      {
        uint32_t array = f.dimension == 3 ? 6 : (f.stacked ? f.depth : 1);
        const uint32_t mip_array = (f.dimension == 1 && array > 1) ? Align(array, 4) : array;
        for (uint32_t s = 1; s < storage_level; ++s)
        {
          const uint32_t pt = std::max(NextPow2(f.width) >> s, 1u);
          const uint32_t rt = std::max(NextPow2(f.height) >> s, 1u);
          uint32_t pa = 32;
          if (!f.tiled)
          {
            pa = std::max(256u / fi.bytes_per_block, 32u);
          }
          const uint32_t pb = Align(Align(pt, fi.block_w) / fi.block_w, pa);
          const uint32_t ra = f.dimension == 0 ? 1 : Align(Align(rt, fi.block_h) / fi.block_h, 32);
          uint32_t ds = 1;
          if (is_3d)
          {
            ds = Align(std::max(NextPow2(depth_texels) >> s, 1u), 4);
          }
          l.base_offset += Align(pb * fi.bytes_per_block * ra * ds, 4096) * mip_array;
        }
      }
      if (packed)
      {
        uint32_t px, py, pz;
        PackedMipOffset(f.width, f.height, depth_texels, level, px, py, pz);
        l.packed_x_blocks = px / fi.block_w;
        l.packed_y_blocks = py / fi.block_h;
        l.packed_z = pz;
      }
      l.valid = true;
      return l;
    }

    inline void EndianSwap(uint8_t *p, uint32_t bytes, uint32_t endian)
    {
      switch (endian)
      {
      case 1: // 8in16
        for (uint32_t i = 0; i + 1 < bytes; i += 2)
        {
          std::swap(p[i], p[i + 1]);
        }
        break;
      case 2: // 8in32
        for (uint32_t i = 0; i + 3 < bytes; i += 4)
        {
          std::swap(p[i], p[i + 3]);
          std::swap(p[i + 1], p[i + 2]);
        }
        break;
      case 3: // 16in32
        for (uint32_t i = 0; i + 3 < bytes; i += 4)
        {
          std::swap(p[i], p[i + 2]);
          std::swap(p[i + 1], p[i + 3]);
        }
        break;
      default:
        break;
      }
    }

    inline uint32_t Float20e4To32Bits(uint32_t f24)
    {
      uint32_t mantissa = f24 & 0xFFFFFu;
      uint32_t exponent = f24 >> 20;
      if (f24 == 0)
      {
        return 0;
      }
      if (exponent == 0)
      {
        uint32_t lz = 0;
        while (!(mantissa & 0x100000u))
        {
          mantissa <<= 1;
          ++lz;
        }
        mantissa &= 0xFFFFFu;
        exponent = 1u - lz;
      }
      return ((exponent + 112u) << 23) | (mantissa << 3);
    }

    inline void ConvertBlock(uint8_t *p, Convert convert)
    {
      switch (convert)
      {
      case Convert::kSwapRB16_1555:
      {
        uint16_t v;
        std::memcpy(&v, p, 2);
        const uint16_t r = v & 0x1F, b = (v >> 10) & 0x1F;
        v = uint16_t((v & 0x83E0) | (r << 10) | b);
        std::memcpy(p, &v, 2);
        break;
      }
      case Convert::kSwapRB16_565:
      {
        uint16_t v;
        std::memcpy(&v, p, 2);
        const uint16_t r = v & 0x1F, b = (v >> 11) & 0x1F;
        v = uint16_t((v & 0x07E0) | (r << 11) | b);
        std::memcpy(p, &v, 2);
        break;
      }
      case Convert::kSwapRB16_4444:
      {
        uint16_t v;
        std::memcpy(&v, p, 2);
        const uint16_t r = v & 0xF, b = (v >> 8) & 0xF;
        v = uint16_t((v & 0xF0F0) | (r << 8) | b);
        std::memcpy(p, &v, 2);
        break;
      }
      case Convert::kDepthUnorm24:
      {
        uint32_t v;
        std::memcpy(&v, p, 4);
        const float d = float(v >> 8) / 16777215.0f;
        std::memcpy(p, &d, 4);
        break;
      }
      case Convert::kDepthFloat24:
      {
        uint32_t v;
        std::memcpy(&v, p, 4);
        const uint32_t bits = Float20e4To32Bits(v >> 8);
        std::memcpy(p, &bits, 4);
        break;
      }
      default:
        break;
      }
    }

    // D3D12 component mapping from the host default swizzle composed with the fetch swizzle.
    UINT ComponentMapping(uint32_t fetch_swizzle, uint16_t host_swizzle)
    {
      UINT comps[4];
      for (uint32_t i = 0; i < 4; ++i)
      {
        const uint32_t sel = (fetch_swizzle >> (3 * i)) & 7;
        if (sel == 4)
        {
          comps[i] = D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0;
        }
        else if (sel == 5)
        {
          comps[i] = D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1;
        }
        else
        {
          const uint32_t host = (host_swizzle >> (3 * (sel & 3))) & 7;
          comps[i] = D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0 + host;
        }
      }
      return D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(comps[0], comps[1], comps[2], comps[3]);
    }
  } // namespace

  // ---------------------------------------------------------------------------
  // Cache
  // ---------------------------------------------------------------------------
  struct TextureJob; // pdz_native_async_upload (below)

  struct Textures::Impl
  {
    std::unordered_map<uint64_t, HostTexture *> uploads; // by key hash
    // pdz_native_async_upload: uploads being prepared on the workers, by the key Lookup will ask for; the per-frame
    // set of fetch constants already looked at (raw 24 bytes); the 5 s window's counters.
    std::unordered_map<uint64_t, TextureJob *> pending;
    struct PrefetchSeen
    {
      uint8_t bytes[24] = {};
      uint64_t frame = ~0ull;
    };
    static constexpr uint32_t kPrefetchSeenSize = 4096;
    PrefetchSeen prefetch_seen[kPrefetchSeenSize];
    static constexpr size_t kMaxPending = 1024;
    static constexpr double kPrewarmKeepSeconds = 180.0;   // a prepared-ahead texture waits this long for its draw
    static constexpr double kPrewarmRetrySeconds = 60.0;   // entries not matching memory are retried this long
    static constexpr double kPrewarmPassSeconds = 2.0;     // between retry passes
    static constexpr double kLevelRecordSeconds = 90.0;    // uploads this long after a level start join its set
    static constexpr double kLevelSummarySeconds = 30.0;
    static constexpr size_t kPrewarmPerBatch = 48;         // submissions per record-thread batch
    static constexpr size_t kLevelSetMax = 16384;
    struct UploadWindow
    {
      uint64_t prefetched = 0, taken = 0, taken_bytes = 0, waits = 0, wait_frames = 0, discarded = 0;
      uint64_t prefetch_skipped = 0; // pdz_native_prefetch_skip_recent
      uint64_t invalidated = 0, unused = 0, sync = 0, sync_bytes = 0, evicted = 0;
      double wait_ms = 0.0, wait_max_ms = 0.0, sync_ms = 0.0;
      uint64_t last_wait_frame = ~0ull;
    };
    UploadWindow win;
    std::chrono::steady_clock::time_point win_start{};
    // pdz_native_texture_prewarm (log 315). The game thread's level change (OnLevelChange) is handed over under
    // prewarm_mutex; everything else is the record thread's (render mutex).
    std::mutex prewarm_mutex;
    std::string prewarm_level_pending;
    bool prewarm_level_changed = false;
    struct PrewarmEntry
    {
      uint32_t dwords[6] = {}; // the fetch constant, host order (FetchDecoded::dwords)
      uint64_t content = 0;    // ContentHash of its base level when it was uploaded
    };
    std::string level_key;                                   // the level being played ("" = none)
    std::chrono::steady_clock::time_point level_start_time{};
    uint64_t level_start_frame = 0;
    bool level_summary_done = true;
    std::unordered_map<uint64_t, PrewarmEntry> level_set;    // this level's textures (by the fetch constant's hash)
    bool level_set_dirty = false;
    std::chrono::steady_clock::time_point level_set_saved{};
    std::vector<PrewarmEntry> prewarm_queue;                 // the last run's set, submitted in passes
    std::vector<uint8_t> prewarm_done;                       // per entry: 1 submitted / cached, 2 given up
    size_t prewarm_cursor = 0;
    uint32_t prewarm_pass = 0;
    std::chrono::steady_clock::time_point prewarm_pass_time{};
    bool prewarm_active = false, prewarm_reported = false;
    uint64_t prewarm_from_file = 0, prewarm_submitted = 0, prewarm_submitted_bytes = 0, prewarm_cached = 0,
             prewarm_stale = 0, prewarm_unsupported = 0;
    // pdz_native_texture_discover: the record thread's state (the scan's result is handed over under prewarm_mutex).
    struct Discovered
    {
      uint32_t dwords[6] = {};
      uint64_t content = 0;
    };
    bool level_playing_pending = false; // under prewarm_mutex
    bool level_playing = false;
    std::chrono::steady_clock::time_point level_playing_time{};
    std::shared_ptr<std::atomic<bool>> scan_busy = std::make_shared<std::atomic<bool>>(false);
    std::vector<Discovered> scan_result; // under prewarm_mutex
    bool scan_result_ready = false;      // under prewarm_mutex
    double scan_result_ms = 0.0;         // under prewarm_mutex
    std::chrono::steady_clock::time_point scan_last{};
    uint32_t disc_scans = 0;
    double disc_scan_ms = 0.0, disc_scan_ms_max = 0.0;
    std::unordered_set<uint64_t> disc_baseline;    // fetch constants of the loading's first scan (already there)
    std::unordered_set<uint64_t> disc_seen;        // fetch constants queued (once each)
    std::unordered_map<uint64_t, uint64_t> disc_prev; // the previous scan: fetch constant -> content
    std::unordered_set<uint64_t> disc_learned;     // contents the level used last time (its prewarm set)
    std::unordered_set<uint64_t> disc_keys;        // upload keys submitted
    std::unordered_set<uint64_t> disc_first_keys;  // upload keys the first 10 s of play created or took
    std::vector<Discovered> disc_queue;
    size_t disc_cursor = 0;
    uint64_t disc_new = 0, disc_by_learned = 0, disc_submitted = 0, disc_cached = 0, disc_other = 0,
             disc_submitted_bytes = 0, disc_taken = 0, disc_rejected = 0;
    bool disc_reported = true;
    // The level-start summary: upload waits and creations since the level started; the shader counters then.
    uint64_t lvl_waits = 0, lvl_wait_frames = 0, lvl_sync = 0, lvl_taken = 0;
    double lvl_wait_ms = 0.0, lvl_wait_max_ms = 0.0, lvl_sync_ms = 0.0;
    uint32_t lvl_vs0 = 0, lvl_ps0 = 0;
    double lvl_vs_ms0 = 0.0, lvl_ps_ms0 = 0.0;
    uint32_t lvl_pso0 = 0, lvl_pso_built0 = 0;
    double lvl_pso_built_ms0 = 0.0;
    uint64_t async_done_seen = 0, async_bytes_seen = 0, async_us_seen = 0, async_failed_seen = 0;
    // pdz_native_texture_keep_seconds: the frame rate between eviction passes (frames per second).
    std::chrono::steady_clock::time_point eviction_time{};
    double eviction_fps = 60.0;
    std::unordered_map<uint64_t, HostTexture *> mirrors; // by (base_address, format, w, h)
    struct FastEntry
    {
      uint32_t dwords[6] = {};
      HostTexture *texture = nullptr;
      uint64_t frame = ~0ull;
    };
    static constexpr uint32_t kFastSize = 1024;
    FastEntry fast[kFastSize];
    bool unsupported_logged[64] = {};
    uint64_t frame = 0;
    uint64_t last_eviction_frame = 0;
    // Resolve mirrors unused this long are evicted (REVIEW2.md C3: a previous level's resolve targets otherwise keep
    // their RT resource and RTV for the whole session): after kMirrorEvictFrames (the surfaces' age) when another
    // mirror over the same guest memory was used since, otherwise after kMirrorStaleFrames (10 minutes at 60 Hz).
    static constexpr uint64_t kMirrorEvictFrames = 1800;
    static constexpr uint64_t kMirrorStaleFrames = 36000;
    uint64_t mirrors_evicted = 0;
    uint32_t mirror_evict_logged = 0;
    uint32_t mirror_refused_logged = 0;
    std::vector<uint8_t> scratch;
    // Full-extent change detection (HostTexture::full_hash).
    static constexpr uint64_t kFullCheckFrames = 8;
    static constexpr uint64_t kFullCheckBudget = 64ull << 20; // bytes hashed per frame
    // Of which the every-frame checks of volatile textures (pdz_native_texture_volatile_check) may use this much; past
    // it a volatile texture waits for the kFullCheckFrames cadence (a 1280x720 video's three planes are 1.4 MB).
    static constexpr uint64_t kVolatileCheckBudget = 8ull << 20;
    uint64_t full_budget_frame = ~0ull;
    uint64_t full_budget = 0;
    uint64_t volatile_budget = 0;
    uint64_t full_changes = 0;
    // Resolve mirrors beyond the exact key (finding 8): mirrors by base address (alias search), alias views (copies
    // of a mirror region for an alias fetch of another size, by the fetch's MirrorKey), and counters for the
    // periodic line in EndFrame.
    std::unordered_map<uint32_t, std::vector<HostTexture *>> mirrors_by_base;
    std::unordered_map<uint64_t, HostTexture *> mirror_views;
    uint64_t alias_hits = 0, alias_view_copies = 0, alias_content_misses = 0, stale_marks = 0, stale_revivals = 0;
    uint64_t logged_alias_hits = 0, logged_stale_marks = 0, logged_stale_revivals = 0;
    uint32_t alias_logged = 0, stale_logged = 0;
    // Alias views freed by the mirror eviction (REVIEW2.md C3): with their source mirror, or unused themselves.
    uint64_t alias_views_evicted = 0, logged_alias_views_evicted = 0;
    // Upload-cache churn (memory log): entries created, evicted unused, replaced by a newer content; superseded = the
    // replaced ones only pdz_native_texture_supersede freed (not volatile).
    uint64_t uploads_created = 0, uploads_evicted = 0, uploads_replaced = 0, uploads_superseded = 0;
    uint32_t supersede_logged = 0;
    // Texture creation log (pdz_frame_timing_spike_ms on; stage 2 M5 texstall): why each upload texture was created,
    // by its identity (UploadKey without the content hash): 1 = created and not evicted since, 2 = evicted by the
    // 900-frame eviction (EndFrame). Filled only while the spike log is on.
    std::unordered_map<uint64_t, uint8_t> created_identities;
    uint32_t creation_logged = 0;
    uint64_t creations_timed = 0;
    // Texture locks (pdz_native_texture_lock_check): locks noted, cache entries they marked, lock-driven re-checks and
    // how many found new content; with the check off, draws that sampled a marked texture and (spike log on) those whose
    // texture really was older than guest memory. Logged every 600 frames with the spike log on (EndFrame).
    uint64_t locks = 0, lock_marks = 0, lock_rechecks = 0, lock_changes = 0, lock_dirty_draws = 0, stale_draws = 0;
    uint64_t locks_logged = 0, last_lock_log_frame = 0;
    uint32_t lock_change_logged = 0;
    // pdz_native_texture_lock_tables: textures the CPU wrote after draws of the same frame sampled them (a small ring
    // of the frame's bases), how many contents the frame's draws got of each, and the new table epochs the writes
    // started. multi_versions = each version past the first of a texture in one frame (logged, first 16).
    struct FrameVersions
    {
      uint32_t base = 0;
      uint64_t frame = ~0ull;
      uint32_t versions = 1;
      bool pending = false; // written after it was sampled: the next upload of this base is a new version
    };
    FrameVersions frame_versions[16];
    uint32_t frame_versions_next = 0;
    uint64_t lock_epochs = 0, multi_versions = 0;
    uint32_t max_versions = 1, multi_version_logged = 0;
  };

  namespace
  {
    uint64_t MirrorKey(const FetchDecoded &f)
    {
      const uint64_t parts[4] = {f.base_address, f.format, f.width, f.height};
      return XXH3_64bits(parts, sizeof(parts));
    }
    uint64_t UploadKey(const FetchDecoded &f, bool is_signed, bool srgb, uint64_t content_hash)
    {
      const uint64_t parts[8] = {f.base_address,
                                 f.format | (uint64_t(f.endian) << 8) | (uint64_t(f.tiled) << 10) |
                                     (uint64_t(f.dimension) << 11) | (uint64_t(is_signed) << 13) |
                                     (uint64_t(srgb) << 14) | (uint64_t(f.packed_mips) << 15),
                                 f.width, f.height, f.depth, f.mip_max_level, f.pitch | (uint64_t(f.mip_address) << 32),
                                 content_hash};
      return XXH3_64bits(parts, sizeof(parts));
    }
    // ContentHash of an address whose page may not be committed (a texture object found by
    // pdz_native_texture_discover, its data perhaps not loaded): copied out first, 0 when unreadable. Any thread.
    uint64_t SafeContentHash(uint32_t base_address)
    {
      const uint8_t *p = PhysPtr(base_address);
      uint8_t page[4096];
      SIZE_T got = 0;
      if (!p || !ReadProcessMemory(GetCurrentProcess(), p, page, sizeof(page), &got) || got != sizeof(page))
      {
        return 0;
      }
      return XXH3_64bits(page, sizeof(page));
    }
    uint64_t ContentHash(uint32_t base_address)
    {
      const pdz::timing::PhaseScope phase(pdz::timing::Phase::kTextureHash); // pdz_frame_timing_phases
      pdz::timing::CountEvent(pdz::timing::Event::kTexHash);                // pdz_frame_timing_spike_ms
      pdz::timing::CountEvent(pdz::timing::Event::kTexHashBytes, 4096);
      const uint8_t *p = PhysPtr(base_address);
      return p ? XXH3_64bits(p, 4096) : 0;
    }
    // Base level (all array slices) as stored in guest memory, capped at 64 MB; `tight_linear`: a linear base level up
    // to the end of its last row (pdz_native_texture_volatile_check).
    uint32_t FullExtentBytes(const FetchDecoded &f, const FormatInfo &fi, bool tight_linear);
    uint64_t FullHash(uint32_t base_address, uint32_t bytes)
    {
      if (!bytes || uint64_t(base_address) + bytes > 0x20000000ull)
      {
        return 0;
      }
      const pdz::timing::PhaseScope phase(pdz::timing::Phase::kTextureHash); // pdz_frame_timing_phases
      pdz::timing::CountEvent(pdz::timing::Event::kTexHash);                // pdz_frame_timing_spike_ms
      pdz::timing::CountEvent(pdz::timing::Event::kTexHashBytes, bytes);
      const uint8_t *p = PhysPtr(base_address);
      return p ? XXH3_64bits(p, bytes) : 0;
    }
    // Whether the SNORM view reproduces the fetch's signs exactly: every component the fetch swizzle reads from
    // memory has the signed field (the SDK's SwizzleSigns rule, texture/util.cpp: such a fetch reads only the
    // signed host view, its 0 / 1 constants included; a fetch of constants only is signed when all four fields
    // are). The swizzle selects which fields count, so XY-signed normal maps read through an X/Y-only swizzle
    // (signs 0x05) now get SNORM instead of UNORM. All-signed (0x55) is signed for any swizzle and all-unsigned
    // (0x00) is not, as before. Mixed signed / unsigned reads, biased (2x-1) and exp_adjust need the shader side
    // (REVIEW2 C5; stage1/subagents/r2signs.md) and are reported by LogSignsGap.
    bool ViewSigned(const FetchDecoded &f)
    {
      bool any_data = false;
      for (uint32_t i = 0; i < 4; ++i)
      {
        const uint32_t sel = (f.swizzle >> (3 * i)) & 7;
        if (sel & 4)
        {
          continue; // constant 0 / 1 (the SDK treats 4..7 as constants here)
        }
        any_data = true;
        if (((f.signs >> (2 * sel)) & 3) != 1)
        {
          return false;
        }
      }
      return any_data || f.signs == 0x55;
    }
    bool AnyGamma(uint32_t signs)
    {
      for (uint32_t i = 0; i < 4; ++i)
      {
        if (((signs >> (2 * i)) & 3) == 3)
        {
          return true;
        }
      }
      return false;
    }

    // Why the host view (uploads: sRGB if any field is gamma, else SNORM per ViewSigned, else UNORM; resolve
    // mirrors: always the plain MirrorFormat view, REVIEW2 J1) does not reproduce the fetch constant's
    // per-component signs and exp_adjust, or nullptr when it does. Float formats (one host format for both)
    // ignore signed / unsigned. sRGB stays the approximation of the guest's piecewise-linear gamma it has always
    // been; only gamma it cannot cover is reported.
    const char *SignsGap(const FetchDecoded &f, const FormatInfo &fi, bool mirror)
    {
      if (f.exp_adjust != 0)
      {
        return "exp_adjust (2^n scale) not applied";
      }
      const bool srgb_view = !mirror && AnyGamma(f.signs) && fi.srgb != DXGI_FORMAT_UNKNOWN;
      const bool snorm_view = !mirror && !srgb_view && ViewSigned(f) && fi.snorm != DXGI_FORMAT_UNKNOWN;
      const bool sign_agnostic = fi.snorm == fi.unorm;
      for (uint32_t i = 0; i < 4; ++i)
      {
        const uint32_t sel = (f.swizzle >> (3 * i)) & 7;
        if (sel & 4)
        {
          continue;
        }
        const uint32_t s = (f.signs >> (2 * sel)) & 3;
        const bool converted = srgb_view && sel < 3; // _SRGB views convert R, G, B only (host swizzle RGBA)
        if (s == 3 && !converted)
        {
          return "gamma component without sRGB conversion";
        }
        if (s != 3 && converted)
        {
          return "sRGB view converts a non-gamma component";
        }
        if (s == 2)
        {
          return "biased (2x-1) not applied";
        }
        if (s == 1 && !snorm_view && !sign_agnostic)
        {
          return "signed component read through a UNORM view";
        }
      }
      return nullptr;
    }

    // One warning per distinct (format, signs, exp_adjust, swizzle) the views above get wrong, at most 32. PDZ's
    // logged levels use only all-unsigned and all-signed float fetches with exp_adjust 0 (r2signs.md), so this
    // is expected to stay silent; a line here names a fetch that needs the shader-side fix.
    void LogSignsGap(const FetchDecoded &f, const FormatInfo &fi, bool mirror)
    {
      if (f.signs == 0 && f.exp_adjust == 0)
      {
        return;
      }
      const char *gap = SignsGap(f, fi, mirror);
      if (!gap)
      {
        return;
      }
      static uint64_t seen[32] = {};
      static uint32_t seen_count = 0;
      const uint64_t key = uint64_t(f.format & 63) | (uint64_t(f.signs & 0xFF) << 6) |
                           (uint64_t(uint32_t(f.exp_adjust) & 63) << 14) | (uint64_t(f.swizzle & 0xFFF) << 20) |
                           (uint64_t(mirror ? 1 : 0) << 32);
      for (uint32_t i = 0; i < seen_count; ++i)
      {
        if (seen[i] == key)
        {
          return;
        }
      }
      if (seen_count >= 32)
      {
        return;
      }
      seen[seen_count++] = key;
      REXLOG_WARN("pdz_native: texture {:08X} {} {}x{} {} signs {:02X} swizzle {:04o} exp_adjust {}: {} (REVIEW2 C5)",
                  f.base_address, fi.name, f.width, f.height, mirror ? "mirror" : "upload", f.signs, f.swizzle,
                  f.exp_adjust, gap);
    }

    // `array_view`: a 1D / 2D texture as a TEXTURE2DARRAY of its slices (one for a plain texture), LookupArray.
    D3D12_CPU_DESCRIPTOR_HANDLE CreateSrv(NativeDevice &dev, HostTexture &t, DXGI_FORMAT view_format,
                                          uint32_t fetch_swizzle, uint16_t host_swizzle, bool array_view = false)
    {
      D3D12_SHADER_RESOURCE_VIEW_DESC d{};
      d.Format = view_format;
      d.Shader4ComponentMapping = ComponentMapping(fetch_swizzle, host_swizzle);
      switch (t.dimension)
      {
      case 2:
        d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
        d.Texture3D.MipLevels = t.mips;
        break;
      case 3:
        d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        d.TextureCube.MipLevels = t.mips;
        break;
      default:
        if (t.array_size > 1 || array_view)
        {
          d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
          d.Texture2DArray.MipLevels = t.mips;
          d.Texture2DArray.ArraySize = t.array_size;
        }
        else
        {
          d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
          d.Texture2D.MipLevels = t.mips;
        }
        break;
      }
      D3D12_CPU_DESCRIPTOR_HANDLE h = dev.AllocStagingSrv();
      if (!h.ptr)
      {
        return h; // staging heap full (logged by the device): the caller skips the texture
      }
      dev.device()->CreateShaderResourceView(t.resource, &d, h);
      return h;
    }

    // SRV variants are keyed by the fetch swizzle (12 bits); kArrayVariant marks LookupArray's TEXTURE2DARRAY ones.
    constexpr uint32_t kArrayVariant = 0x1000u;

    D3D12_CPU_DESCRIPTOR_HANDLE SrvFor(NativeDevice &dev, HostTexture &t, uint32_t fetch_swizzle,
                                       uint16_t host_swizzle)
    {
      for (uint32_t i = 0; i < t.srv_count; ++i)
      {
        if (t.srvs[i].swizzle == fetch_swizzle)
        {
          return t.srvs[i].srv;
        }
      }
      D3D12_CPU_DESCRIPTOR_HANDLE h = CreateSrv(dev, t, t.resource_format, fetch_swizzle & ~kArrayVariant, host_swizzle,
                                                (fetch_swizzle & kArrayVariant) != 0);
      if (!h.ptr)
      {
        return h; // not cached: the next lookup tries again
      }
      if (t.srv_count < 4)
      {
        t.srvs[t.srv_count++] = {fetch_swizzle, h};
      }
      else
      {
        dev.FreeStagingSrv(t.srvs[3].srv);
        t.srvs[3] = {fetch_swizzle, h};
      }
      return h;
    }

    // pdz_native_texture_watch: the armed textures by identity (UploadKey without the content); a destroyed texture
    // leaves the map.
    std::unordered_map<uint64_t, HostTexture *> g_tex_watch;
    struct TexWatchStats
    {
      uint64_t clean = 0, invalidated = 0, armed = 0, refused = 0, verified = 0, mismatches = 0;
    } g_tex_watch_stats;

    void DestroyTexture(NativeDevice &dev, HostTexture *t)
    {
      if (t->watch_armed)
      {
        auto w = g_tex_watch.find(t->identity);
        if (w != g_tex_watch.end() && w->second == t)
        {
          g_tex_watch.erase(w);
        }
      }
      for (uint32_t i = 0; i < t->srv_count; ++i)
      {
        dev.FreeStagingSrv(t->srvs[i].srv);
      }
      if (t->mirror)
      {
        dev.FreeRtv(t->rtv);
      }
      dev.ReleaseLater(t->resource);
      delete t;
    }

    // LogAndDumpTexture's once-per-(address, format, size) set: video frames re-upload the same planes every frame.
    std::unordered_map<uint64_t, bool> g_log_seen;

    // Test aid (cvars above): one line per new texture and an optional dump of mip 0 / slice 0.
    void LogAndDumpTexture(const FetchDecoded &f, const FormatInfo &fi, const LevelLayout &l, const uint8_t *guest,
                           const uint8_t *native, uint32_t host_row_pitch, uint32_t host_rows, uint64_t host_row_bytes)
    {
      static uint32_t logged = 0, dumped = 0;
      // The switches first: the set used to take a key for every texture uploaded with both off (the default), so it
      // grew for the whole session (memory log review); it now only holds what is logged or dumped.
      const bool log = REXCVAR_GET(pdz_native_texture_log) && logged < 400;
      const bool dump_wanted = dumped < uint32_t(std::max(0, REXCVAR_GET(pdz_native_texture_dump_max)));
      if (!log && !(dump_wanted && !REXCVAR_GET(pdz_native_texture_dump_dir).empty()))
      {
        return;
      }
      const uint64_t key = (uint64_t(f.base_address) << 32) ^ (uint64_t(f.format) << 26) ^ (uint64_t(f.width) << 13) ^
                           uint64_t(f.height);
      if (!g_log_seen.emplace(key, true).second)
      {
        return;
      }
      const std::string dir = REXCVAR_GET(pdz_native_texture_dump_dir);
      const bool dump = !dir.empty() && dump_wanted;
      const std::string name = fmt::format("{:08X}_{}_{}x{}", f.base_address, fi.name, f.width, f.height);
      if (log)
      {
        ++logged;
        REXLOG_INFO("pdz_native: texture {} dim {} depth {} endian {} tiled {} pitch {} swizzle {:03o} signs {:02X} "
                    "num_format {} mips {}..{} packed {} mip_address {:08X} fetch {:08X} {:08X} {:08X} {:08X} {:08X} "
                    "{:08X}",
                    name, f.dimension, f.depth, f.endian, f.tiled ? 1 : 0, f.pitch, f.swizzle, f.signs, f.num_format,
                    f.mip_min_level, f.mip_max_level, f.packed_mips ? 1 : 0, f.mip_address, f.dwords[0], f.dwords[1],
                    f.dwords[2], f.dwords[3], f.dwords[4], f.dwords[5]);
      }
      if (!dump || !guest || !native)
      {
        return;
      }
      ++dumped;
      std::error_code ec;
      std::filesystem::create_directories(dir, ec);
      const std::string base = dir + "\\" + name;
      const uint64_t guest_bytes = std::min<uint64_t>(uint64_t(l.z_stride_bytes) + 0x4000, 16ull << 20);
      if (FILE *fp = std::fopen((base + ".guest").c_str(), "wb"))
      {
        std::fwrite(guest, 1, size_t(guest_bytes), fp);
        std::fclose(fp);
      }
      if (FILE *fp = std::fopen((base + ".native").c_str(), "wb"))
      {
        for (uint32_t y = 0; y < host_rows; ++y)
        {
          std::fwrite(native + size_t(y) * host_row_pitch, 1, size_t(host_row_bytes), fp);
        }
        std::fclose(fp);
      }
      if (FILE *fp = std::fopen((base + ".json").c_str(), "wb"))
      {
        const std::string json = fmt::format(
            "{{\"format\": {}, \"format_name\": \"{}\", \"width\": {}, \"height\": {}, \"depth\": {}, "
            "\"dimension\": {}, \"endian\": {}, \"tiled\": {}, \"pitch\": {}, \"swizzle\": {}, \"signs\": {}, "
            "\"num_format\": {}, \"packed_mips\": {}, \"mip_max_level\": {}, \"base_address\": {}, "
            "\"mip_address\": {}, \"block_w\": {}, \"block_h\": {}, \"bytes_per_block\": {}, "
            "\"pitch_blocks\": {}, \"rows_aligned\": {}, \"width_blocks\": {}, \"height_blocks\": {}, "
            "\"packed_x_blocks\": {}, \"packed_y_blocks\": {}, \"base_offset\": {}, \"guest_bytes\": {}, "
            "\"native_row_bytes\": {}, \"native_rows\": {}, \"fetch\": [{}, {}, {}, {}, {}, {}]}}\n",
            f.format, fi.name, f.width, f.height, f.depth, f.dimension, f.endian, f.tiled ? 1 : 0, f.pitch, f.swizzle,
            f.signs, f.num_format, f.packed_mips ? 1 : 0, f.mip_max_level, f.base_address, f.mip_address, fi.block_w,
            fi.block_h, fi.bytes_per_block, l.pitch_blocks, l.rows_aligned, l.width_blocks, l.height_blocks,
            l.packed_x_blocks, l.packed_y_blocks, l.base_offset, guest_bytes, host_row_bytes, host_rows, f.dwords[0],
            f.dwords[1], f.dwords[2], f.dwords[3], f.dwords[4], f.dwords[5]);
        std::fwrite(json.data(), 1, json.size(), fp);
        std::fclose(fp);
      }
    }

    // LogAndDumpTexture's state: the upload workers (pdz_native_async_upload) call it too.
    std::mutex g_log_mutex;

    // Untiles every level / slice of the guest texture into the upload buffer and records the copies. `async`
    // (pdz_native_async_upload, an upload worker): the job's own upload buffer and copy list instead of the frame's;
    // the bytes and copies are the same.
    bool UploadGuestTexture(NativeDevice &dev, std::vector<uint8_t> &scratch, HostTexture &t, const FetchDecoded &f,
                            const FormatInfo &fi, uint64_t *upload_bytes = nullptr,
                            asyncup::CopyTarget *async = nullptr)
    {
      // A volume made from an array (CreateUploadTexture): layers * replicas slices, one subresource per mip. The SDK
      // samples a stacked texture under tfetch3D from a 2D array at layer floor(clamp(z * layers, 0.5, layers - 0.5))
      // (point volume filter) or lerps the two layers around z * layers - 0.5 (linear). With
      // pdz_native_volume_sdk_layers, slice j of mip m (slices_m = depth >> m) holds the layer at the centre of its z
      // range, floor((j + 0.5) / slices_m * layers): slice j = layer j at mip 0 for a linear-filter fetch (one
      // replica), a run of 8 slices per layer for a point one, and one layer of every 2^m at mip m (the Texture3D's
      // depth halves per mip where the SDK's array keeps every layer: a minified stacked texture shows the middle
      // layer of each group). Without it (the pre-stacked copy): the base level only, 8 replicas, slice j holding
      // layer round((j + 0.5) / 8), i.e. a 50 / 50 blend at every layer centre instead of at the layer boundaries.
      const bool volume = t.volume_of_array;
      const bool sdk_layers = volume && REXCVAR_GET(pdz_native_volume_sdk_layers);
      const uint32_t replicas = volume ? std::max(1u, t.depth / std::max(1u, t.volume_layers)) : 1u;
      const uint32_t subresources = volume ? t.mips : t.mips * t.array_size;
      std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(subresources);
      std::vector<UINT> rows(subresources);
      std::vector<UINT64> row_sizes(subresources);
      UINT64 total = 0;
      D3D12_RESOURCE_DESC desc = t.resource->GetDesc();
      dev.device()->GetCopyableFootprints(&desc, 0, subresources, 0, footprints.data(), rows.data(), row_sizes.data(),
                                          &total);
      if (total == 0 || total > (1ull << 30))
      {
        return false;
      }
      UploadSlice slice;
      if (async)
      {
        ID3D12Resource *resource = nullptr;
        uint64_t offset = 0;
        slice.cpu = async->alloc(*async, total, &resource, &offset);
        slice.resource = resource;
        slice.offset = offset;
      }
      else
      {
        slice = dev.Upload(uint32_t(total), D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
      }
      if (!slice.cpu)
      {
        return false;
      }
      if (!async)
      {
        pdz::timing::CountEvent(pdz::timing::Event::kTexUploadBytes, total); // pdz_frame_timing_spike_ms
      }
      if (upload_bytes)
      {
        *upload_bytes = total;
      }
      uint8_t *upload = static_cast<uint8_t *>(slice.cpu);
      // pdz_debug_poison_new_resources: every byte of every subresource starts magenta; the loop below overwrites what
      // the guest data covers, and a subresource it never copies still gets the magenta one (after the loop).
      const bool poison = PoisonNewResources();
      if (poison)
      {
        PoisonFill(upload, total, t.resource->GetDesc().Format);
      }
      std::vector<uint8_t> copied(subresources, 0);
      const char *skip_reason = nullptr;
      uint32_t skip_level = 0;
      const uint32_t bpb = fi.bytes_per_block;
      const uint32_t bpb_log2 = Log2Floor(bpb);
      const bool is_3d = f.dimension == 2;
      // Linear textures without a format conversion go a row at a time (a 1280x720 video plane: 720 copies instead of
      // 921,600). The block loop's per-block endian swap is a no-op for blocks smaller than the swap unit and equals
      // the row-wide swap when the block size is a multiple of it, so the bytes are the same.
      const bool row_copy = !f.tiled && fi.convert == Convert::kNone;
      const bool row_swap = f.endian != 0 && bpb % (f.endian == 1 ? 2u : 4u) == 0;
      // pdz_native_texture_untile_cached: the block loop's row is built in the scratch buffer (cached) and copied into
      // the upload heap once, so the heap (write-combined) is only written: swapping / converting a block in place
      // there reads it back through uncached memory (stage2/m5/m5_texstall.md).
      const bool cached_rows = !row_copy && REXCVAR_GET(pdz_native_texture_untile_cached);

      for (uint32_t level = 0; level < t.mips; ++level)
      {
        const LevelLayout l = ComputeLevel(f, fi, level);
        const uint32_t address = l.from_mips ? f.mip_address : f.base_address;
        if (!address)
        {
          if (!skip_reason)
          {
            skip_reason = "no address";
            skip_level = level;
          }
          continue;
        }
        const uint32_t slices = volume ? std::max(1u, t.depth >> level) : t.array_size;
        for (uint32_t array = 0; array < slices; ++array)
        {
          const uint32_t sub = volume ? level : level + array * t.mips; // D3D12 order: mip-major within a slice
          const D3D12_PLACED_SUBRESOURCE_FOOTPRINT &fp = footprints[sub];
          const uint32_t host_row_pitch = fp.Footprint.RowPitch;
          const uint32_t host_slice_pitch = host_row_pitch * rows[sub];
          // SDK layers: floor((j + 0.5) / slices * layers) in integers; pre-stacked copy: round((j + 0.5) / replicas).
          const uint32_t source_layer =
              !volume      ? array
              : sdk_layers ? std::min(t.volume_layers - 1, ((2u * array + 1u) * t.volume_layers) / (2u * slices))
                           : std::min(t.volume_layers - 1, uint32_t((float(array) + 0.5f) / float(replicas) + 0.5f));
          const uint32_t guest_level_offset = l.base_offset + source_layer * l.slice_stride_bytes;
          // Bound check: the furthest byte this level can touch (tiled: the 32x32 tile span, generous).
          const uint64_t extent = uint64_t(l.slice_stride_bytes) + 0x4000;
          if (uint64_t(address) + guest_level_offset + extent > 0x20000000ull)
          {
            REXLOG_WARN("pdz_native: texture {:08X} level {} exceeds physical memory; skipped", f.base_address,
                        level);
            if (!skip_reason)
            {
              skip_reason = "past the end of physical memory";
              skip_level = level;
            }
            continue;
          }
          const uint8_t *guest = PhysPtr(address + guest_level_offset);
          if (!guest)
          {
            return false;
          }
          uint8_t *dst_base = upload + fp.Offset + (volume ? size_t(array) * host_slice_pitch : 0);
          const uint32_t block_row_bytes = l.width_blocks * bpb;
          if (cached_rows && scratch.size() < block_row_bytes)
          {
            scratch.resize(block_row_bytes);
          }
          for (uint32_t z = 0; z < l.depth; ++z)
          {
            const uint32_t gz = z + l.packed_z;
            for (uint32_t y = 0; y < l.height_blocks; ++y)
            {
              const uint32_t gy = y + l.packed_y_blocks;
              uint8_t *dst_row = dst_base + z * host_slice_pitch + y * host_row_pitch;
              if (row_copy)
              {
                // The row's blocks are contiguous in guest memory. A swapped row is prepared in the scratch buffer so
                // the upload heap (write-combined) is only written.
                const uint32_t row_bytes = l.width_blocks * bpb;
                const uint8_t *src_row = guest + uint64_t(gz) * l.z_stride_bytes + uint64_t(gy) * l.pitch_blocks * bpb +
                                         uint64_t(l.packed_x_blocks) * bpb;
                if (row_swap)
                {
                  if (scratch.size() < row_bytes)
                  {
                    scratch.resize(row_bytes);
                  }
                  std::memcpy(scratch.data(), src_row, row_bytes);
                  EndianSwap(scratch.data(), row_bytes, f.endian);
                  src_row = scratch.data();
                }
                std::memcpy(dst_row, src_row, row_bytes);
                continue;
              }
              uint8_t *row_out = cached_rows ? scratch.data() : dst_row;
              for (uint32_t x = 0; x < l.width_blocks; ++x)
              {
                const uint32_t gx = x + l.packed_x_blocks;
                uint64_t src_offset;
                if (f.tiled)
                {
                  src_offset = is_3d ? uint64_t(Tiled3D(int32_t(gx), int32_t(gy), int32_t(gz), l.pitch_blocks,
                                                        l.rows_aligned, bpb_log2))
                                     : uint64_t(uint32_t(Tiled2D(int32_t(gx), int32_t(gy), l.pitch_blocks, bpb_log2)));
                }
                else
                {
                  src_offset = uint64_t(gz) * l.z_stride_bytes + uint64_t(gy) * l.pitch_blocks * bpb + uint64_t(gx) * bpb;
                }
                uint8_t *dst = row_out + x * bpb;
                std::memcpy(dst, guest + src_offset, bpb);
                EndianSwap(dst, bpb, f.endian);
                if (fi.convert != Convert::kNone)
                {
                  ConvertBlock(dst, fi.convert);
                }
              }
              if (cached_rows)
              {
                std::memcpy(dst_row, row_out, block_row_bytes);
              }
            }
          }
          if (level == 0 && array == 0)
          {
            std::lock_guard<std::mutex> log_lock(g_log_mutex);
            LogAndDumpTexture(f, fi, l, guest, dst_base, host_row_pitch, rows[sub], row_sizes[sub]);
          }
          if (volume && array + 1 < slices)
          {
            continue; // one copy per volume mip, after its last slice
          }
          D3D12_TEXTURE_COPY_LOCATION dst_loc{};
          dst_loc.pResource = t.resource;
          dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
          dst_loc.SubresourceIndex = sub;
          D3D12_TEXTURE_COPY_LOCATION src_loc{};
          src_loc.pResource = slice.resource;
          src_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
          src_loc.PlacedFootprint = fp;
          src_loc.PlacedFootprint.Offset += slice.offset;
          if (async)
          {
            async->list->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, nullptr);
          }
          else
          {
            dev.list()->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, nullptr);
          }
          copied[sub] = 1;
        }
      }
      // Subresources the loop never wrote (a level skipped above) hold whatever the new memory held. Logged (the first
      // 16); with pdz_fix_texture_unwritten (on) they are written as zero, with the poison switch as magenta.
      uint32_t missing = 0;
      for (uint32_t sub = 0; sub < subresources; ++sub)
      {
        missing += copied[sub] ? 0u : 1u;
      }
      if (missing)
      {
        static std::atomic<uint32_t> missing_logged{0};
        const bool fill = poison || REXCVAR_GET(pdz_fix_texture_unwritten);
        if (missing_logged.fetch_add(1) < 16)
        {
          REXLOG_WARN("pdz_native: texture {:08X} {} {}x{} mips {} slices {}: {} of {} subresources not uploaded "
                      "(first: level {}, {}); {}",
                      f.base_address, fi.name, f.width, f.height, t.mips, volume ? 1u : t.array_size, missing,
                      subresources, skip_level, skip_reason ? skip_reason : "?",
                      poison ? "filled magenta (pdz_debug_poison_new_resources)"
                      : fill ? "filled with zero (pdz_fix_texture_unwritten)"
                             : "left as the new memory held them");
        }
        for (uint32_t sub = 0; fill && sub < subresources; ++sub)
        {
          if (copied[sub])
          {
            continue;
          }
          const D3D12_PLACED_SUBRESOURCE_FOOTPRINT &fp = footprints[sub];
          if (!poison)
          {
            std::memset(upload + fp.Offset, 0,
                        size_t(fp.Footprint.RowPitch) * rows[sub] * std::max(1u, fp.Footprint.Depth));
          }
          D3D12_TEXTURE_COPY_LOCATION dst_loc{};
          dst_loc.pResource = t.resource;
          dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
          dst_loc.SubresourceIndex = sub;
          D3D12_TEXTURE_COPY_LOCATION src_loc{};
          src_loc.pResource = slice.resource;
          src_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
          src_loc.PlacedFootprint = fp;
          src_loc.PlacedFootprint.Offset += slice.offset;
          if (async)
          {
            async->list->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, nullptr);
          }
          else
          {
            dev.list()->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, nullptr);
          }
        }
      }
      return true;
    }

    uint32_t FullExtentBytes(const FetchDecoded &f, const FormatInfo &fi, bool tight_linear)
    {
      const LevelLayout l = ComputeLevel(f, fi, 0);
      const uint32_t array = f.dimension == 3 ? 6 : (f.stacked ? f.depth : 1);
      uint64_t bytes = uint64_t(l.slice_stride_bytes) * array;
      if (tight_linear && !f.tiled && l.width_blocks && l.height_blocks && !l.packed_x_blocks && !l.packed_y_blocks &&
          !l.packed_z)
      {
        // Linear base level (not a packed tail): the rows it holds, up to the end of the last row of the last slice,
        // instead of the 32-row aligned slices (a 128x1 ramp spans 16 KB of them: its neighbours' pages).
        const uint64_t rows = uint64_t(l.depth - 1) * l.z_stride_bytes +
                              uint64_t(l.height_blocks - 1) * l.pitch_blocks * fi.bytes_per_block +
                              uint64_t(l.width_blocks) * fi.bytes_per_block;
        bytes = uint64_t(array - 1) * l.slice_stride_bytes + rows;
      }
      return uint32_t(std::min<uint64_t>(bytes, 64ull << 20));
    }

    // Texture creation log (pdz_frame_timing_spike_ms on): the process page-fault count (soft and hard), to tell a
    // slow untile caused by cold guest pages from one caused by the upload heap's uncached read-back.
    uint64_t ProcessPageFaults()
    {
      using Fn = BOOL(WINAPI *)(HANDLE, PROCESS_MEMORY_COUNTERS *, DWORD);
      static const Fn fn = [] {
        HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
        return kernel ? reinterpret_cast<Fn>(reinterpret_cast<void *>(GetProcAddress(kernel, "K32GetProcessMemoryInfo")))
                      : Fn(nullptr);
      }();
      PROCESS_MEMORY_COUNTERS c{};
      c.cb = sizeof(c);
      return fn && fn(GetCurrentProcess(), &c, sizeof(c)) ? uint64_t(c.PageFaultCount) : 0;
    }

    // Why an upload texture is created (the creation log; spike log on only, else nullptr): first time this identity
    // (the key without the content hash) is seen, again after the 900-frame eviction freed it, or a new content of a
    // texture already cached (its first 4 KB changed, or `why` from the caller: changed past them).
    const char *CreationReason(Textures::Impl &im, uint64_t identity, const char *why)
    {
      if (!pdz::timing::SpikeOn())
      {
        return nullptr;
      }
      auto [it, inserted] = im.created_identities.try_emplace(identity, uint8_t(1));
      const char *reason = why                ? why
                           : inserted         ? "first seen"
                           : it->second == 2 ? "re-created after the 900-frame eviction"
                                              : "new content (first 4 KB changed)";
      it->second = 1;
      return reason;
    }

    // FullHash without the phase / event accounting (an upload worker: those are the record thread's).
    uint64_t FullHashQuiet(uint32_t base_address, uint32_t bytes)
    {
      if (!bytes || uint64_t(base_address) + bytes > 0x20000000ull)
      {
        return 0;
      }
      const uint8_t *p = PhysPtr(base_address);
      return p ? XXH3_64bits(p, bytes) : 0;
    }

    // A new upload texture's fields (no resource yet): CreateUploadTexture on the record thread, TextureJob on an
    // upload worker (record_thread false: no phase accounting). The caller sets upload_frame and full_check_frame.
    HostTexture *NewUploadTexture(const FetchDecoded &f, const FormatInfo &fi, bool is_signed, bool srgb,
                                  uint64_t content_hash, bool volume, bool record_thread)
    {
      HostTexture *t = new HostTexture();
      t->volume_of_array = volume;
      t->dimension = volume ? 2 : f.dimension;
      t->width = f.width;
      t->height = f.height;
      t->volume_layers = volume && f.stacked ? f.depth : 1;
      // Volume copy (LookupVolume): layers * replicas slices. With pdz_native_volume_sdk_layers a fetch constant whose
      // volume filter (dword 4 bit 0) is linear gets one slice per layer: the Texture3D's own linear filtering along W
      // is then the SDK's lerp between the two layers around z * layers - 0.5, clamped to the layer centres by
      // AddressW = CLAMP (GetSampler). A point one gets 8 replicas (D3D12 cannot point-sample W alone; the replica
      // runs leave a 1/8-layer blend band at each layer boundary). The mip chain is the guest's: a Texture3D's mip m
      // has depth / 2^m slices, each holding one layer of its group (UploadGuestTexture). Without the cvar: 8
      // replicas and the base level only (the pre-stacked copy).
      const bool sdk_layers = volume && REXCVAR_GET(pdz_native_volume_sdk_layers);
      const uint32_t volume_replicas =
          !volume || t->volume_layers == 1 ? 1u : (sdk_layers && f.vol_mag_filter == 1 ? 1u : 8u);
      t->depth = volume ? t->volume_layers * volume_replicas : (f.dimension == 2 ? f.depth : 1);
      t->array_size = volume ? 1 : (f.dimension == 3 ? 6 : (f.stacked ? f.depth : 1));
      t->mips = volume && !sdk_layers ? 1 : (f.mip_address ? f.mip_max_level + 1 : 1);
      t->resource_format = srgb && fi.srgb != DXGI_FORMAT_UNKNOWN ? fi.srgb
                           : (is_signed && fi.snorm != DXGI_FORMAT_UNKNOWN ? fi.snorm : fi.unorm);
      t->base_address = f.base_address;
      t->format = f.format;
      t->key_width = f.width;
      t->key_height = f.height;
      t->key_depth = f.depth;
      t->key_mips = t->mips;
      t->key_signed = is_signed;
      t->key_srgb = srgb;
      t->content_hash = content_hash;
      // pdz_native_texture_volatile_check: a linear texture is CPU-written (video planes, UI ramps) and is compared
      // with guest memory every frame (Lookup also marks a texture whose data changed past its first 4 KB).
      const bool volatile_check = REXCVAR_GET(pdz_native_texture_volatile_check);
      t->volatile_content = volatile_check && !f.tiled;
      t->full_bytes = FullExtentBytes(f, fi, volatile_check);
      t->full_hash = record_thread ? FullHash(f.base_address, t->full_bytes)
                                   : FullHashQuiet(f.base_address, t->full_bytes);
      return t;
    }

    D3D12_RESOURCE_DESC UploadDesc(const HostTexture &t, const FetchDecoded &f)
    {
      D3D12_RESOURCE_DESC desc{};
      desc.Dimension = t.dimension == 2 ? D3D12_RESOURCE_DIMENSION_TEXTURE3D : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
      desc.Width = f.width;
      desc.Height = f.height;
      desc.DepthOrArraySize = UINT16(t.dimension == 2 ? t.depth : t.array_size);
      desc.MipLevels = UINT16(t.mips);
      desc.Format = t.resource_format;
      desc.SampleDesc.Count = 1;
      desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
      return desc;
    }

    HostTexture *CreateUploadTexture(NativeDevice &dev, Textures::Impl &im, const FetchDecoded &f, const FormatInfo &fi,
                                     bool is_signed, bool srgb, uint64_t content_hash, bool volume = false,
                                     const char *why = nullptr)
    {
      const pdz::timing::PhaseScope phase(pdz::timing::Phase::kTextureUpload); // pdz_frame_timing_phases
      pdz::timing::CountEvent(pdz::timing::Event::kTexCreate);                // pdz_frame_timing_spike_ms
      // Creation log (spike log on): wall-clock times of the hash, the resource creation and the untile + copies.
      const bool timed = pdz::timing::SpikeOn();
      using Clock = std::chrono::steady_clock;
      const Clock::time_point t_start = Clock::now(); // also the 5 s uploads line (record thread time)
      const uint64_t faults_start = timed ? ProcessPageFaults() : 0;
      Clock::time_point t_hashed{}, t_created{};
      HostTexture *t = NewUploadTexture(f, fi, is_signed, srgb, content_hash, volume, true);
      if (timed)
      {
        t_hashed = Clock::now();
      }

      t->upload_frame = dev.frame_index();
      t->full_check_frame = im.frame;
      const D3D12_RESOURCE_DESC desc = UploadDesc(*t, f);
      t->resource = dev.CreateTexture(desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, "pdz_native texture");
      t->state = D3D12_RESOURCE_STATE_COPY_DEST;
      if (!t->resource)
      {
        delete t;
        return nullptr;
      }
      if (timed)
      {
        t_created = Clock::now();
      }
      uint64_t upload_bytes = 0;
      if (!UploadGuestTexture(dev, im.scratch, *t, f, fi, &upload_bytes))
      {
        dev.ReleaseLater(t->resource);
        delete t;
        return nullptr;
      }
      t->upload_bytes = upload_bytes;
      ++im.win.sync;
      im.win.sync_bytes += upload_bytes;
      im.win.sync_ms += std::chrono::duration<double, std::milli>(Clock::now() - t_start).count();
      ++im.lvl_sync;
      im.lvl_sync_ms += std::chrono::duration<double, std::milli>(Clock::now() - t_start).count();
      if (timed)
      {
        // One line per creation from the level on (native frame >= pdz_frame_timing_skip, the swap the statistics
        // start at), plus any slow one (5 ms or more) before it; at most 200 per run.
        const Clock::time_point t_end = Clock::now();
        const uint64_t faults = ProcessPageFaults() - faults_start;
        auto ms = [](Clock::time_point a, Clock::time_point b) {
          return std::chrono::duration<double, std::milli>(b - a).count();
        };
        const double total_ms = ms(t_start, t_end);
        ++im.creations_timed;
        const bool level = dev.frame_index() >= uint64_t(std::max(0, REXCVAR_GET(pdz_frame_timing_skip)));
        if (im.creation_logged < 200 && (level || total_ms >= 5.0))
        {
          ++im.creation_logged;
          const bool row_copy = !f.tiled && fi.convert == Convert::kNone;
          REXLOG_INFO("pdz_native: texture created (#{} of {}, frame {}, {}): {:08X} {} {}x{}x{} dim {} mips {} {} endian "
                      "{} convert {} | upload {:.3f} MB, hashed {:.3f} MB | hash {:.2f} ms, create {:.2f} ms, untile + "
                      "copy {:.2f} ms ({}), total {:.2f} ms, page faults {}",
                      im.creation_logged, im.creations_timed, dev.frame_index(), why ? why : (volume ? "volume copy" : "?"),
                      f.base_address, fi.name, f.width, f.height, f.depth, f.dimension, t->mips,
                      f.tiled ? "tiled" : "linear", f.endian, fi.convert != Convert::kNone ? 1 : 0,
                      double(upload_bytes) / 1048576.0, double(t->full_bytes) / 1048576.0, ms(t_start, t_hashed),
                      ms(t_hashed, t_created), ms(t_created, t_end),
                      row_copy                                          ? "row copy"
                      : REXCVAR_GET(pdz_native_texture_untile_cached) ? "block loop, cached rows"
                                                                        : "block loop in the upload heap",
                      total_ms, faults);
        }
      }
      // pdz_native_packed_tail_mips (DecodeFetchDwords): the first few textures whose mips come from the packed tail
      // at the base address, so a run shows the rule fired.
      static uint32_t packed_tail_logged = 0;
      if (t->mips > 1 && f.packed_mips && f.mip_address == f.base_address && packed_tail_logged < 8)
      {
        ++packed_tail_logged;
        REXLOG_INFO("pdz_native: texture {:08X} {} {}x{}: level 0 is the packed mip tail, mips {}..{} read from the "
                    "base address like the SDK (pdz_native_packed_tail_mips)",
                    f.base_address, fi.name, f.width, f.height, f.mip_min_level, f.mip_max_level);
      }
      ++im.uploads_created;
      return t;
    }

  } // namespace

  // pdz_native_async_upload: one texture prepared on an upload worker. Run: the same fields, resource description,
  // untiler and copies as CreateUploadTexture; the resource starts in COMMON (the copy queue promotes it to COPY_DEST
  // and it decays back to COMMON when the copy list completes), so the graphics list transitions it from COMMON.
  struct TextureJob final : asyncup::Job
  {
    FetchDecoded f;
    const FormatInfo *fi = nullptr;
    bool is_signed = false, srgb = false;
    uint64_t content_hash = 0;
    uint64_t key = 0;
    uint32_t range_begin = 0, range_end = 0; // guest bytes the untiler reads (base level; the mips from mip_address)
    HostTexture *t = nullptr;                // set by Run (owned by the job until taken)
    bool invalidated = false;                // record thread: a CPU write over its range since it was queued
    bool prewarm = false;                    // pdz_native_texture_prewarm: kept pending up to kPrewarmKeepSeconds
    bool discovered = false;                 // pdz_native_texture_discover: whole base level compared when taken
    uint32_t extent = 0;                     // guest bytes of the base level (the pending-memory cap's estimate)
    std::chrono::steady_clock::time_point queued_time{};

    bool Run(asyncup::CopyTarget &target) override
    {
      NativeDevice *dev = NativeDevice::Get();
      if (!dev)
      {
        return false;
      }
      HostTexture *nt = NewUploadTexture(f, *fi, is_signed, srgb, content_hash, false, false);
      const D3D12_RESOURCE_DESC desc = UploadDesc(*nt, f);
      nt->resource = dev->CreateTexture(desc, D3D12_RESOURCE_STATE_COMMON, nullptr, "pdz_native texture (async)", false);
      nt->state = D3D12_RESOURCE_STATE_COMMON;
      if (!nt->resource)
      {
        delete nt;
        return false;
      }
      thread_local std::vector<uint8_t> scratch;
      uint64_t upload = 0;
      if (!UploadGuestTexture(*dev, scratch, *nt, f, *fi, &upload, &target))
      {
        nt->resource->Release(); // never executed on any queue (a failed job's list is not submitted)
        delete nt;
        return false;
      }
      nt->upload_bytes = upload;
      bytes = upload;
      t = nt;
      return true;
    }
  };

  namespace
  {
    void NoteLevelUpload(Textures::Impl &im, const FetchDecoded &f, uint64_t content); // pdz_native_texture_prewarm

    // A job nobody will take (record thread; done or failed): its texture goes once its copy is off the copy queue.
    void DiscardJob(TextureJob *job)
    {
      asyncup::Wait(*job);
      if (job->t)
      {
        asyncup::ReleaseAfterCopy(job->t->resource, job->fence);
        delete job->t;
      }
      delete job;
    }

    // Lookup's creation step with pdz_native_async_upload: the prepared texture for this key, waiting for it if a
    // worker is still on it (only this one), or nullptr (none prepared, failed, or its guest memory changed since).
    HostTexture *TakePrefetched(NativeDevice &dev, Textures::Impl &im, uint64_t key)
    {
      auto it = im.pending.find(key);
      if (it == im.pending.end())
      {
        return nullptr;
      }
      TextureJob *job = it->second;
      im.pending.erase(it);
      double waited = 0.0;
      {
        const pdz::timing::PhaseScope phase(pdz::timing::Phase::kTextureUpload); // the wait is this draw's upload
        waited = asyncup::Wait(*job);
      }
      if (waited > 0.0)
      {
        ++im.win.waits;
        im.win.wait_ms += waited;
        im.win.wait_max_ms = std::max(im.win.wait_max_ms, waited);
        if (im.win.last_wait_frame != im.frame)
        {
          im.win.last_wait_frame = im.frame;
          ++im.win.wait_frames;
          ++im.lvl_wait_frames;
        }
        ++im.lvl_waits;
        im.lvl_wait_ms += waited;
        im.lvl_wait_max_ms = std::max(im.lvl_wait_max_ms, waited);
      }
      HostTexture *t = job->t;
      bool ok = job->state.load(std::memory_order_acquire) == 2 && t && !job->invalidated;
      if (ok && job->discovered)
      {
        // pdz_native_texture_discover: prepared during the loading, perhaps before all of its data was in: the whole
        // base level must still be what was uploaded.
        ok = FullHash(t->base_address, t->full_bytes) == t->full_hash;
        ++(ok ? im.disc_taken : im.disc_rejected);
      }
      // A linear texture (CPU-written: video planes, UI ramps) is compared whole, as its every-frame check would.
      if (ok && t->volatile_content && FullHash(t->base_address, t->full_bytes) != t->full_hash)
      {
        ok = false;
      }
      if (!ok)
      {
        ++im.win.discarded;
        im.win.invalidated += job->invalidated ? 1 : 0;
        DiscardJob(job);
        return nullptr;
      }
      job->t = nullptr;
      t->upload_frame = dev.frame_index();
      t->full_check_frame = im.frame;
      t->copy_fence = job->fence;
      pdz::timing::CountEvent(pdz::timing::Event::kTexCreate);                    // pdz_frame_timing_spike_ms
      pdz::timing::CountEvent(pdz::timing::Event::kTexUploadBytes, job->bytes);
      ++im.uploads_created;
      ++im.win.taken;
      ++im.lvl_taken;
      im.win.taken_bytes += job->bytes;
      delete job;
      return t;
    }

    // -------------------------------------------------------------------------
    // Resolve mirrors beyond the exact key (REVIEW.md finding 8)
    // -------------------------------------------------------------------------

    // Base format as the SDK texture cache keys textures (include\rex\graphics\pipeline\texture\info.h
    // GetBaseFormat): the _EXPAND / _AS_16_16_16_16 / GAMMA_EDRAM formats are the same bytes read another way.
    uint32_t BaseFormat(uint32_t format)
    {
      switch (format & 63)
      {
      case 27: return 30; // k_16_EXPAND -> k_16_FLOAT
      case 28: return 31; // k_16_16_EXPAND -> k_16_16_FLOAT
      case 29: return 32; // k_16_16_16_16_EXPAND -> k_16_16_16_16_FLOAT
      case 50: return 6;  // k_8_8_8_8_AS_16_16_16_16 -> k_8_8_8_8
      case 51: return 18; // k_DXT1_AS_16_16_16_16 -> k_DXT1
      case 52: return 19; // k_DXT2_3_AS_16_16_16_16 -> k_DXT2_3
      case 53: return 20; // k_DXT4_5_AS_16_16_16_16 -> k_DXT4_5
      case 54: return 7;  // k_2_10_10_10_AS_16_16_16_16 -> k_2_10_10_10
      case 55: return 16; // k_10_11_11_AS_16_16_16_16 -> k_10_11_11
      case 56: return 17; // k_11_11_10_AS_16_16_16_16 -> k_11_11_10
      case 62: return 6;  // k_8_8_8_8_GAMMA_EDRAM -> k_8_8_8_8
      default: return format & 63;
      }
    }

    // Row pitch (texels) and base level bytes of a texture in guest memory, as ComputeLevel lays level 0 out.
    uint32_t StoragePitch(const FetchDecoded &f) { return f.pitch ? f.pitch : Align(f.width, 32); }
    uint32_t BaseLevelBytes(const FetchDecoded &f) { return ComputeLevel(f, kFormats[f.format & 63], 0).slice_stride_bytes; }

    void ForgetFast(Textures::Impl &im, const HostTexture *m)
    {
      for (auto &fe : im.fast)
      {
        if (fe.texture && (fe.texture == m || fe.texture->view_source == m))
        {
          fe.texture = nullptr;
        }
      }
    }

    // A mirror holding the data a fetch reads without being its exact key: the same base address, the same base
    // format and host format, a plain 2D fetch with the mirror's tiling and row pitch (so texel (x, y) is at the
    // same guest offset in both) whose base level lies inside the mirror's. Only mirrors resolved into at least
    // once, not stale, and whose guest memory is unchanged since their last resolve (a native resolve never
    // writes guest memory, so a change means the CPU put other data there). The most recently resolved wins.
    HostTexture *FindAliasMirror(Textures::Impl &im, const FetchDecoded &f, bool count_content_miss)
    {
      if (f.dimension != 1 || f.stacked)
      {
        return nullptr;
      }
      auto it = im.mirrors_by_base.find(f.base_address);
      if (it == im.mirrors_by_base.end())
      {
        return nullptr;
      }
      const uint32_t pitch = StoragePitch(f);
      const uint32_t bytes = BaseLevelBytes(f);
      HostTexture *best = nullptr;
      for (HostTexture *m : it->second)
      {
        if (m->stale || m->generation == 0 || BaseFormat(m->format) != BaseFormat(f.format) ||
            MirrorFormat(f.format) != m->resource_format || f.tiled != m->key_tiled || pitch != m->key_pitch ||
            bytes > m->range_bytes)
        {
          continue;
        }
        if (!best || m->last_resolve_frame > best->last_resolve_frame)
        {
          best = m;
        }
      }
      if (best && ContentHash(best->base_address) != best->resolve_content_hash)
      {
        if (count_content_miss)
        {
          ++im.alias_content_misses;
        }
        return nullptr;
      }
      return best;
    }

    // A k_8_8_8_8 fetch that no mirror serves, of the memory a depth resolve wrote (same base, size, row pitch and
    // tiling): PDZ's vision modes read the scene's stencil that way (an A8R8G8B8 header over the 1216x640 k_24_8_FLOAT
    // depth texture, the stencil in its .z; the RCP-90 threat outline, stage2/polish/rcp90.md). The native depth
    // resolve keeps the depth only and never writes guest memory, so the fetch would read whatever the memory held.
    // Marks the depth mirror; Surfaces::Resolve then packs its next resolves into a k_8_8_8_8 mirror of this key
    // (pdz_native_depth_stencil_alias). Logged once per mirror, 8 at most.
    void NoteStencilAliasFetch(Textures::Impl &im, const FetchDecoded &f)
    {
      if (f.dimension != 1 || f.stacked)
      {
        return;
      }
      auto it = im.mirrors_by_base.find(f.base_address);
      if (it == im.mirrors_by_base.end())
      {
        return;
      }
      const uint32_t pitch = StoragePitch(f);
      for (HostTexture *m : it->second)
      {
        const uint32_t mf = m->format & 63;
        if ((mf != 22 && mf != 23) || m->stencil_alias_wanted || m->generation == 0 || m->key_width != f.width ||
            m->key_height != f.height || m->key_pitch != pitch || m->key_tiled != f.tiled)
        {
          continue;
        }
        m->stencil_alias_wanted = true;
        static uint32_t logged = 0;
        if (logged < 8)
        {
          ++logged;
          REXLOG_INFO("pdz_native: texture {:08X} k_8_8_8_8 {}x{} reads the memory of depth resolve mirror {}: its "
                      "resolves {} also packed into a k_8_8_8_8 mirror (depth24 << 8 | stencil; "
                      "pdz_native_depth_stencil_alias {})",
                      f.base_address, f.width, f.height, kFormats[mf].name,
                      REXCVAR_GET(pdz_native_depth_stencil_alias) ? "are" : "would be",
                      REXCVAR_GET(pdz_native_depth_stencil_alias));
        }
      }
    }

    // Copies the source mirror's top-left region into an alias view when the source was resolved into since.
    void RefreshMirrorView(NativeDevice &dev, Textures::Impl &im, HostTexture &v, HostTexture &src)
    {
      if (v.view_source == &src && v.view_generation == src.generation)
      {
        return;
      }
      D3D12_BOX box{};
      box.right = std::min(v.width, src.width);
      box.bottom = std::min(v.height, src.height);
      box.back = 1;
      dev.Transition(src.resource, src.state, D3D12_RESOURCE_STATE_COPY_SOURCE);
      dev.Transition(v.resource, v.state, D3D12_RESOURCE_STATE_COPY_DEST);
      D3D12_TEXTURE_COPY_LOCATION dst_loc{};
      dst_loc.pResource = v.resource;
      dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      dst_loc.SubresourceIndex = 0;
      D3D12_TEXTURE_COPY_LOCATION src_loc{};
      src_loc.pResource = src.resource;
      src_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      src_loc.SubresourceIndex = 0;
      dev.list()->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, &box);
      v.view_source = &src;
      v.view_generation = src.generation;
      ++im.alias_view_copies;
    }

    // A new non-target texture's every subresource written once with zero (or magenta with the poison switch): the
    // resource is created without zeroing, so what a later copy does not cover would otherwise be the old memory.
    void FillNewTexture(NativeDevice &dev, ID3D12Resource *resource, bool magenta)
    {
      D3D12_RESOURCE_DESC desc = resource->GetDesc();
      const uint32_t subresources =
          desc.MipLevels * (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1u : desc.DepthOrArraySize);
      std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(subresources);
      UINT64 total = 0;
      dev.device()->GetCopyableFootprints(&desc, 0, subresources, 0, footprints.data(), nullptr, nullptr, &total);
      if (total == 0 || total > (1ull << 30) || !dev.list())
      {
        return;
      }
      const UploadSlice slice = dev.Upload(uint32_t(total), D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
      if (!slice.cpu)
      {
        return;
      }
      if (magenta)
      {
        PoisonFill(static_cast<uint8_t *>(slice.cpu), total, desc.Format);
      }
      else
      {
        std::memset(slice.cpu, 0, size_t(total));
      }
      for (uint32_t sub = 0; sub < subresources; ++sub)
      {
        D3D12_TEXTURE_COPY_LOCATION dst_loc{};
        dst_loc.pResource = resource;
        dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst_loc.SubresourceIndex = sub;
        D3D12_TEXTURE_COPY_LOCATION src_loc{};
        src_loc.pResource = slice.resource;
        src_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src_loc.PlacedFootprint = footprints[sub];
        src_loc.PlacedFootprint.Offset += slice.offset;
        dev.list()->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, nullptr);
      }
    }

    // The alias view of `src` for a fetch of another size. Texels outside the source's region: zero (written once at
    // creation, pdz_fix_mirror_view_zero; before, the resource was not zeroed and they held the old memory), magenta
    // with pdz_debug_poison_new_resources.
    HostTexture *GetMirrorView(NativeDevice &dev, Textures::Impl &im, const FetchDecoded &f, HostTexture &src)
    {
      const uint64_t key = MirrorKey(f);
      HostTexture *v = nullptr;
      if (auto it = im.mirror_views.find(key); it != im.mirror_views.end())
      {
        v = it->second;
        if (v->scale != src.scale || v->resource_format != src.resource_format)
        {
          ForgetFast(im, v);
          DestroyTexture(dev, v);
          im.mirror_views.erase(it);
          v = nullptr;
        }
      }
      if (!v)
      {
        v = new HostTexture();
        v->mirror_view = true;
        v->scale = src.scale;
        v->dimension = 1;
        v->width = f.width * src.scale;
        v->height = f.height * src.scale;
        v->depth = 1;
        v->array_size = 1;
        v->mips = 1;
        v->resource_format = src.resource_format;
        v->base_address = f.base_address;
        v->format = f.format;
        v->key_width = f.width;
        v->key_height = f.height;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = v->width;
        desc.Height = v->height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = v->resource_format;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        v->resource = dev.CreateTexture(desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, "pdz_native resolve mirror view");
        v->state = D3D12_RESOURCE_STATE_COPY_DEST;
        if (!v->resource)
        {
          delete v;
          return nullptr;
        }
        if (PoisonNewResources() || REXCVAR_GET(pdz_fix_mirror_view_zero))
        {
          FillNewTexture(dev, v->resource, PoisonNewResources());
          v->view_source = nullptr; // the refresh below copies the source over it
        }
        im.mirror_views.emplace(key, v);
      }
      RefreshMirrorView(dev, im, *v, src);
      return v;
    }

    // Split screen (stage1/subagents/split.md, pad2.md risk 3): a fetch whose base address lies inside a resolve mirror's
    // range but not at its base (e.g. one view's half of a texture both views resolve into, read through a texture
    // object of its own) matches no mirror and is served from guest memory, which a native resolve never writes. Logged
    // in multi-pass frames only (each address once, 8 at most), so the first split-screen run shows whether PDZ does it.
    void NoteFetchInsideMirror(Textures::Impl &im, const FetchDecoded &f)
    {
      static uint32_t logged_bases[8] = {};
      static uint32_t logged = 0;
      if (logged >= 8 || std::find(logged_bases, logged_bases + logged, f.base_address) != logged_bases + logged)
      {
        return;
      }
      for (const auto &entry : im.mirrors)
      {
        const HostTexture *m = entry.second;
        if (m->stale || m->generation == 0 || f.base_address <= m->base_address ||
            uint64_t(f.base_address) >= uint64_t(m->base_address) + m->range_bytes)
        {
          continue;
        }
        logged_bases[logged++] = f.base_address;
        REXLOG_INFO("pdz_native: split screen: texture {:08X} {} {}x{} pitch {} {} starts {} bytes into resolve mirror "
                    "{:08X} {} {}x{} (pitch {}, {} bytes); served from guest memory, not from the mirror (not modelled)",
                    f.base_address, kFormats[f.format & 63].name, f.width, f.height, StoragePitch(f),
                    f.tiled ? "tiled" : "linear", f.base_address - m->base_address, m->base_address,
                    kFormats[m->format & 63].name, m->key_width, m->key_height, m->key_pitch, m->range_bytes);
        return;
      }
    }

    // An ordinary texture was just uploaded over [base, base + bytes): every mirror overlapping that range and not
    // resolved into during the last two frames is marked stale (another level's texture in a reused heap must not
    // be shadowed by an old render). Kept: a mirror resolved into during the last two frames (the upload is an
    // alias the mirror path did not serve; it gets one upload, cached, so this runs once, and the next resolve
    // would revive the mirror anyway), and a mirror at the upload's own base address whose guest memory is
    // unchanged since its last resolve (an incompatible read of the resolved memory, not new CPU data).
    void MarkMirrorsStale(Textures::Impl &im, const FetchDecoded &f, uint32_t bytes)
    {
      const uint64_t begin = f.base_address, end = begin + std::max(bytes, 1u);
      for (auto &entry : im.mirrors)
      {
        HostTexture *m = entry.second;
        const uint64_t m_begin = m->base_address, m_end = m_begin + std::max(m->range_bytes, 1u);
        if (m->stale || begin >= m_end || m_begin >= end || m->last_resolve_frame + 2 > im.frame)
        {
          continue;
        }
        if (m->base_address == f.base_address && ContentHash(m->base_address) == m->resolve_content_hash)
        {
          continue;
        }
        m->stale = true;
        ++im.stale_marks;
        ForgetFast(im, m);
        if (im.stale_logged < 12)
        {
          ++im.stale_logged;
          REXLOG_INFO("pdz_native: resolve mirror {:08X} {} {}x{} ({} bytes, last resolve frame {}) is stale: texture "
                      "{:08X} {} {}x{} ({} bytes) uploaded over it at frame {}",
                      m->base_address, kFormats[m->format & 63].name, m->key_width, m->key_height, m->range_bytes,
                      m->last_resolve_frame, f.base_address, kFormats[f.format & 63].name, f.width, f.height, bytes,
                      im.frame);
        }
      }
    }
  } // namespace

  Textures::Textures() : impl_(new Impl()) {}

  Textures &Textures::Get()
  {
    static Textures instance;
    return instance;
  }

  void Textures::Transition(HostTexture &t, D3D12_RESOURCE_STATES state)
  {
    NativeDevice *dev = NativeDevice::Get();
    if (dev && dev->in_frame())
    {
      dev->Transition(t.resource, t.state, state);
    }
  }

  HostTexture *Textures::FindMirror(const FetchDecoded &f)
  {
    auto it = impl_->mirrors.find(MirrorKey(f));
    return it == impl_->mirrors.end() || it->second->stale ? nullptr : it->second;
  }

  HostTexture *Textures::GetMirror(const FetchDecoded &f)
  {
    Impl &im = *impl_;
    // Called by Surfaces::Resolve right before every resolve into the mirror: note when, and what guest memory
    // held then (alias lookups only serve the mirror while that is unchanged).
    if (auto it = im.mirrors.find(MirrorKey(f)); it != im.mirrors.end())
    {
      HostTexture *existing = it->second;
      if (existing->stale)
      {
        existing->stale = false; // resolved into again: the render is current
        ++im.stale_revivals;
        for (auto &fe : im.fast)
        {
          fe.texture = nullptr; // this frame's entries for its fetch constants point at the upload
        }
      }
      existing->last_resolve_frame = im.frame;
      existing->resolve_content_hash = ContentHash(existing->base_address);
      return existing;
    }
    NativeDevice *dev = NativeDevice::Get();
    if (!dev || !f.base_address || f.width == 0 || f.height == 0)
    {
      return nullptr;
    }
    // The RTV first (REVIEW2.md C3): a full RTV heap refuses the mirror (the resolve is skipped and the texture
    // falls back to the upload path) before a render target is created for it.
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle = dev->AllocRtv();
    if (!rtv_handle.ptr)
    {
      if (impl_->mirror_refused_logged++ < 8)
      {
        REXLOG_ERROR("pdz_native: resolve mirror for guest {:08X} ({}x{}) skipped: no RTV left ({} mirrors live)",
                     f.base_address, f.width, f.height, impl_->mirrors.size());
      }
      return nullptr;
    }
    // Scale(); the 1344 x 1344 shadow-map textures x pdz_shadow_map_scale, as their source surface (SurfaceScale).
    const uint32_t scale = SurfaceScale(f.width, f.height);
    HostTexture *t = new HostTexture();
    t->mirror = true;
    t->scale = scale;
    t->dimension = 1;
    t->width = f.width * scale;
    t->height = f.height * scale;
    t->depth = 1;
    t->array_size = 1;
    t->mips = 1;
    t->resource_format = MirrorFormat(f.format);
    t->base_address = f.base_address;
    t->format = f.format;
    t->key_width = f.width;
    t->key_height = f.height;
    t->key_pitch = StoragePitch(f);
    t->key_tiled = f.tiled;
    t->range_bytes = BaseLevelBytes(f);
    t->last_resolve_frame = im.frame;
    t->resolve_content_hash = ContentHash(f.base_address);

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = t->width;
    desc.Height = t->height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = t->resource_format;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE clear{};
    clear.Format = t->resource_format;
    // The guest address and format in the debug name (debug layer / GPU-based validation messages say which mirror).
    const std::string mirror_name = fmt::format("pdz_native resolve mirror {:08X} {} {}x{}", f.base_address,
                                                kFormats[f.format & 63].name, f.width, f.height);
    t->resource = dev->CreateTexture(desc, D3D12_RESOURCE_STATE_RENDER_TARGET, &clear, mirror_name.c_str());
    t->state = D3D12_RESOURCE_STATE_RENDER_TARGET;
    if (!t->resource)
    {
      dev->FreeRtv(rtv_handle);
      delete t;
      return nullptr;
    }
    t->rtv = rtv_handle;
    t->last_used_frame = impl_->frame;
    D3D12_RENDER_TARGET_VIEW_DESC rtv{};
    rtv.Format = t->resource_format;
    rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    dev->device()->CreateRenderTargetView(t->resource, &rtv, t->rtv);
    // pdz_debug_poison_new_resources: a new resolve mirror starts magenta instead of zero (what a resolve does not
    // cover then shows).
    if (PoisonNewResources() && dev->list())
    {
      const FLOAT magenta[4] = {1.0f, 0.0f, 1.0f, 1.0f};
      dev->list()->ClearRenderTargetView(t->rtv, magenta, 0, nullptr);
    }
    im.mirrors.emplace(MirrorKey(f), t);
    im.mirrors_by_base[f.base_address].push_back(t);
    REXLOG_INFO("pdz_native: resolve mirror {}x{} (guest {:08X} {} {}x{} pitch {} {} {} bytes) host format {}",
                t->width, t->height, f.base_address, kFormats[f.format & 63].name, f.width, f.height, t->key_pitch,
                f.tiled ? "tiled" : "linear", t->range_bytes, uint32_t(t->resource_format));
    return t;
  }

  TextureView Textures::Lookup(const FetchDecoded &f)
  {
    TextureView view;
    NativeDevice *dev = NativeDevice::Get();
    if (!dev || !dev->in_frame() || f.type != 2 || !f.base_address || f.width == 0 || f.height == 0)
    {
      return view;
    }
    Impl &im = *impl_;
    const FormatInfo &fi = kFormats[f.format & 63];

    // Fast path: the same 24 bytes, validated this frame.
    const uint32_t fast_index = uint32_t(XXH3_64bits(f.dwords, sizeof(f.dwords))) & (Impl::kFastSize - 1);
    Impl::FastEntry &fe = im.fast[fast_index];
    HostTexture *t = nullptr;
    if (fe.texture && std::memcmp(fe.dwords, f.dwords, sizeof(f.dwords)) == 0 && fe.frame == im.frame)
    {
      t = fe.texture;
    }
    if (!t)
    {
      // Resolve mirrors first (render-target-backed, no upload, never hashed).
      t = FindMirror(f);
      if (!t)
      {
        // Not a mirror's exact key: the same resolved data under a format alias or another size (finding 8).
        if (HostTexture *m = FindAliasMirror(im, f, true))
        {
          const bool same_size = f.width == m->key_width && f.height == m->key_height;
          t = same_size ? m : GetMirrorView(*dev, im, f, *m);
          if (t)
          {
            ++im.alias_hits;
            if (im.alias_logged < 12)
            {
              ++im.alias_logged;
              REXLOG_INFO("pdz_native: texture {:08X} {} {}x{} pitch {} served by resolve mirror {} {}x{}{} (alias "
                          "#{})",
                          f.base_address, fi.name, f.width, f.height, StoragePitch(f), kFormats[m->format & 63].name,
                          m->key_width, m->key_height, same_size ? "" : " through a region copy", im.alias_hits);
            }
          }
        }
      }
      LogSignsGap(f, fi, t != nullptr); // a mirror or an alias view of one: the plain MirrorFormat view
      if (!t && SplitActive())
      {
        NoteFetchInsideMirror(im, f); // log only
      }
      if (!t && (f.format & 63) == 6)
      {
        NoteStencilAliasFetch(im, f); // a k_8_8_8_8 view of a depth resolve: packed from the next resolve on
      }
      if (!t)
      {
        if (!fi.supported)
        {
          if (!im.unsupported_logged[f.format & 63])
          {
            im.unsupported_logged[f.format & 63] = true;
            REXLOG_WARN("pdz_native: texture format {} ({}) not supported by the stage-1 untiler ({}x{}x{} at {:08X})",
                        f.format, fi.name, f.width, f.height, f.depth, f.base_address);
          }
          return view;
        }
        const bool is_signed = ViewSigned(f);
        const bool srgb = AnyGamma(f.signs);
        // pdz_native_texture_watch: an armed texture whose pages nobody wrote since: its first-4-KB hash without
        // reading guest memory, and no full-extent re-check (below).
        HostTexture *armed = nullptr;
        const bool tex_watch = REXCVAR_GET(pdz_native_texture_watch) && guestwatch::Ready();
        if (tex_watch && !g_tex_watch.empty())
        {
          auto w = g_tex_watch.find(UploadKey(f, is_signed, srgb, 0));
          if (w != g_tex_watch.end())
          {
            HostTexture *wt = w->second;
            if (wt->watch_armed && !wt->lock_dirty && wt->base_address == f.base_address &&
                guestwatch::GenSum(wt->base_address, wt->watch_bytes) == wt->watch_gen)
            {
              armed = wt;
              ++g_tex_watch_stats.clean;
              if (REXCVAR_GET(pdz_native_texture_watch_verify))
              {
                ++g_tex_watch_stats.verified;
                if ((ContentHash(f.base_address) != wt->watch_content ||
                     FullHash(wt->base_address, wt->full_bytes) != wt->full_hash) &&
                    g_tex_watch_stats.mismatches++ < 8)
                {
                  REXLOG_WARN("pdz_native: texture watch verify: {:08X} {}x{} changed without a watch callback "
                              "(mismatch #{})",
                              f.base_address, f.width, f.height, g_tex_watch_stats.mismatches);
                }
              }
            }
            else
            {
              wt->watch_armed = false;
              wt->watch_stable = 0;
              g_tex_watch.erase(w);
              ++g_tex_watch_stats.invalidated;
            }
          }
        }
        const uint64_t content = armed ? armed->watch_content : ContentHash(f.base_address);
        const uint64_t key = UploadKey(f, is_signed, srgb, content);
        auto it = im.uploads.find(key);
        bool changed_past_first_page = false;
        bool replaced_by_lock = false; // pdz_native_texture_lock_check: the new entry keeps the lock's frame
        if (it != im.uploads.end())
        {
          t = it->second;
          // Full-extent re-check (budgeted): the key covers only the first 4 KB. Every frame for a volatile texture
          // (pdz_native_texture_volatile_check), else every kFullCheckFrames.
          if (im.full_budget_frame != im.frame)
          {
            im.full_budget_frame = im.frame;
            im.full_budget = Impl::kFullCheckBudget;
            im.volatile_budget = Impl::kVolatileCheckBudget;
          }
          const bool every_frame = t->volatile_content && REXCVAR_GET(pdz_native_texture_volatile_check) &&
                                   t->full_bytes <= im.volatile_budget;
          const uint64_t interval = every_frame ? 1u : Impl::kFullCheckFrames;
          // pdz_native_texture_lock_check: locked for writing since the last comparison: compared now.
          const bool lock_recheck =
              REXCVAR_GET(pdz_native_texture_lock_check) &&
              (t->lock_dirty || (t->full_check_frame == t->lock_frame && im.frame == t->lock_frame + 1));
          if (armed != t &&
              (lock_recheck || (im.frame - t->full_check_frame >= interval && t->full_bytes <= im.full_budget)))
          {
            if (!lock_recheck)
            {
              im.full_budget -= t->full_bytes;
              if (every_frame)
              {
                im.volatile_budget -= t->full_bytes;
              }
            }
            t->full_check_frame = im.frame;
            if (!lock_recheck && !every_frame && !t->full_phase_set && REXCVAR_GET(pdz_native_texture_check_stagger))
            {
              // pdz_native_texture_check_stagger: textures created together leave the shared phase (the next
              // re-check 0-7 frames sooner, once).
              t->full_phase_set = true;
              const uint32_t a = f.base_address;
              t->full_check_frame -= ((a >> 12) ^ (a >> 15) ^ (a >> 18)) & (Impl::kFullCheckFrames - 1);
            }
            const uint64_t h = FullHash(f.base_address, t->full_bytes);
            // pdz_native_texture_watch: two full checks in a row found the same bytes: arm (watch first, both hashes
            // again, accepted only if no callback touched the pages meanwhile).
            if (tex_watch && h == t->full_hash && !lock_recheck && !t->volatile_content && !t->watch_armed &&
                ++t->watch_stable >= 2 && uint64_t(f.base_address) + std::max(t->full_bytes, 4096u) <= 0x20000000ull)
            {
              const uint32_t bytes = std::max(t->full_bytes, 4096u);
              const uint64_t before = guestwatch::GenSum(f.base_address, bytes);
              guestwatch::Arm(f.base_address, bytes);
              const bool same = FullHash(f.base_address, t->full_bytes) == t->full_hash &&
                                ContentHash(f.base_address) == content;
              const uint64_t after = guestwatch::GenSum(f.base_address, bytes);
              if (same && before == after)
              {
                t->watch_armed = true;
                t->watch_gen = after;
                t->watch_content = content;
                t->watch_bytes = bytes;
                g_tex_watch[t->identity] = t;
                ++g_tex_watch_stats.armed;
              }
              else
              {
                t->watch_stable = 0;
                ++g_tex_watch_stats.refused;
              }
            }
            im.lock_rechecks += lock_recheck;
            t->lock_dirty = false; // compared with guest memory: re-uploaded below if it changed
            if (h != t->full_hash)
            {
              changed_past_first_page = true;
              if (lock_recheck)
              {
                replaced_by_lock = true;
                ++im.lock_changes;
                if (im.lock_change_logged < 8)
                {
                  ++im.lock_change_logged;
                  REXLOG_INFO("pdz_native: texture {:08X} {} {}x{} written by the CPU (texture lock) since its upload: "
                              "re-uploaded before the next draw (#{}; pdz_native_texture_lock_check)",
                              f.base_address, fi.name, f.width, f.height, im.lock_changes);
                }
              }
              if (im.full_changes++ < 20)
              {
                REXLOG_INFO("pdz_native: texture {:08X} {} {}x{} changed past its first 4 KB ({} bytes checked); "
                            "re-uploaded (#{})",
                            f.base_address, fi.name, f.width, f.height, t->full_bytes, im.full_changes);
              }
              for (auto &other : im.fast)
              {
                if (other.texture == t)
                {
                  other.texture = nullptr;
                }
              }
              DestroyTexture(*dev, t);
              im.uploads.erase(it);
              ++im.uploads_replaced;
              it = im.uploads.end();
              t = nullptr;
            }
          }
        }
        if (!t)
        {
          const char *why = CreationReason(im, UploadKey(f, is_signed, srgb, 0),
                                           changed_past_first_page ? "changed past its first 4 KB" : nullptr);
          t = TakePrefetched(*dev, im, key); // pdz_native_async_upload
          if (!t)
          {
            t = CreateUploadTexture(*dev, im, f, fi, is_signed, srgb, content, false, why);
          }
          if (!t)
          {
            return view;
          }
          if (changed_past_first_page && REXCVAR_GET(pdz_native_texture_volatile_check))
          {
            t->volatile_content = true; // rewritten in place past its first page: checked every frame from now on
          }
          if (replaced_by_lock)
          {
            t->lock_frame = im.frame; // re-checked once more next frame, as the entry it replaces would have been
          }
          for (auto &v : im.frame_versions)
          {
            if (v.pending && v.frame == im.frame && v.base == f.base_address)
            {
              // pdz_native_texture_lock_tables: a new content of a texture this frame already sampled; the earlier
              // draws keep their tables (and the old upload, released after the frame), the later ones get this one.
              v.pending = false;
              ++v.versions;
              ++im.multi_versions;
              im.max_versions = std::max(im.max_versions, v.versions);
              if (im.multi_version_logged < 16)
              {
                ++im.multi_version_logged;
                REXLOG_INFO("pdz_native: texture {:08X} {} {}x{} has {} versions in frame {}: the CPU wrote it (texture "
                            "lock) after draws of this frame sampled it; those keep the version they were recorded "
                            "with, the draws after the write get the new one (#{}; pdz_native_texture_lock_tables)",
                            f.base_address, fi.name, f.width, f.height, v.versions, im.frame, im.multi_versions);
              }
              break;
            }
          }
          t->identity = UploadKey(f, is_signed, srgb, 0);
          const bool supersede = REXCVAR_GET(pdz_native_texture_supersede);
          if (t->volatile_content || supersede)
          {
            // Earlier contents of the same guest texture (a video plane whose first rows changed gets a new key every
            // frame) can never match guest memory again: freed now instead of after 900 unused frames. Not one used
            // this frame (its SRV may already be in a table of this frame). A volatile texture matches them by its key
            // fields, as always; pdz_native_texture_supersede does the same for every texture by the whole key minus
            // the content hash (identity), so two layouts of the same memory never replace each other.
            for (auto other = im.uploads.begin(); other != im.uploads.end();)
            {
              HostTexture *o = other->second;
              const bool same_fields =
                  t->volatile_content && o->base_address == t->base_address && o->format == t->format &&
                  o->key_width == t->key_width && o->key_height == t->key_height && o->key_depth == t->key_depth &&
                  o->key_mips == t->key_mips && o->key_signed == t->key_signed && o->key_srgb == t->key_srgb &&
                  o->volume_of_array == t->volume_of_array;
              const bool same_identity = supersede && !t->volatile_content && o->identity == t->identity &&
                                         o->volume_of_array == t->volume_of_array;
              if ((same_fields || same_identity) && o->last_used_frame != im.frame)
              {
                ForgetFast(im, o);
                if (same_identity)
                {
                  ++im.uploads_superseded;
                  if (im.supersede_logged < 16)
                  {
                    ++im.supersede_logged;
                    REXLOG_INFO("pdz_native: texture {:08X} {} {}x{}x{} {} pitch {}: an older content (uploaded in frame "
                                "{}, last used in frame {}) superseded by the new one (#{}; pdz_native_texture_supersede)",
                                f.base_address, fi.name, f.width, f.height, f.depth, f.tiled ? "tiled" : "linear",
                                f.pitch, o->upload_frame, o->last_used_frame, im.uploads_superseded);
                  }
                }
                DestroyTexture(*dev, o);
                other = im.uploads.erase(other);
                ++im.uploads_replaced;
              }
              else
              {
                ++other;
              }
            }
          }
          im.uploads.emplace(key, t);
          MarkMirrorsStale(im, f, t->full_bytes);
          NoteLevelUpload(im, f, content); // pdz_native_texture_prewarm: this level's set
        }
      }
      std::memcpy(fe.dwords, f.dwords, sizeof(f.dwords));
      fe.texture = t;
      fe.frame = im.frame;
    }
    if (t->lock_dirty)
    {
      // Only reachable with pdz_native_texture_lock_check off: this draw samples a texture the game locked for writing
      // since its upload. With the spike log on, whether its guest memory really differs (once per texture and frame).
      ++im.lock_dirty_draws;
      if (REXCVAR_GET(pdz_frame_timing_spike_ms) > 0 && t->stale_probe_frame != im.frame)
      {
        t->stale_probe_frame = im.frame;
        t->stale_probe = FullHash(t->base_address, t->full_bytes) != t->full_hash;
      }
      im.stale_draws += t->stale_probe_frame == im.frame && t->stale_probe;
    }
    if (t->mirror_view && t->view_source)
    {
      RefreshMirrorView(*dev, im, *t, *t->view_source); // a resolve into the source since the last copy
      // Sampling through an alias view uses the source mirror too: its last_used_frame keeps the mirror eviction
      // (REVIEW2.md C3) from freeing a mirror that is only ever sampled through its views.
      t->view_source->last_used_frame = im.frame;
    }
    if (t->copy_fence)
    {
      // Prepared on the copy queue (pdz_native_async_upload): this frame's list waits for that copy on the GPU.
      if (asyncup::CompletedFence() >= t->copy_fence)
      {
        t->copy_fence = 0;
      }
      else
      {
        asyncup::NeedFence(t->copy_fence);
      }
    }
    t->last_used_frame = im.frame;
    dev->Transition(t->resource, t->state,
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    view.srv = SrvFor(*dev, *t, f.swizzle,
                      t->mirror || t->mirror_view ? (fi.supported ? fi.host_swizzle : kSwzRGBA) : fi.host_swizzle);
    if (!view.srv.ptr)
    {
      return TextureView{}; // staging heap full: unsupported for this draw (the draw / present skips it)
    }
    view.texture = t;
    return view;
  }

  TextureView Textures::LookupArray(const FetchDecoded &f)
  {
    if (f.dimension >= 2)
    {
      return TextureView{};
    }
    TextureView view = Lookup(f);
    if (!view.texture || !view.srv.ptr)
    {
      return TextureView{};
    }
    HostTexture &t = *view.texture;
    if (t.dimension >= 2 || t.volume_of_array)
    {
      return TextureView{};
    }
    if (t.array_size > 1)
    {
      return view; // CreateSrv made its view a TEXTURE2DARRAY of every layer already
    }
    NativeDevice *dev = NativeDevice::Get();
    const FormatInfo &fi = kFormats[f.format & 63];
    view.srv = SrvFor(*dev, t, f.swizzle | kArrayVariant,
                      t.mirror || t.mirror_view ? (fi.supported ? fi.host_swizzle : kSwzRGBA) : fi.host_swizzle);
    if (!view.srv.ptr)
    {
      return TextureView{};
    }
    return view;
  }

  TextureView Textures::LookupVolume(const FetchDecoded &f)
  {
    TextureView view;
    NativeDevice *dev = NativeDevice::Get();
    const FormatInfo &fi = kFormats[f.format & 63];
    if (!dev || !dev->in_frame() || f.type != 2 || !f.base_address || f.width == 0 || f.height == 0 ||
        f.dimension != 1 || !fi.supported || FindMirror(f) || FindAliasMirror(*impl_, f, false))
    {
      return view;
    }
    Impl &im = *impl_;
    LogSignsGap(f, fi, false);
    const bool is_signed = ViewSigned(f);
    const bool srgb = AnyGamma(f.signs);
    const uint64_t content = ContentHash(f.base_address);
    const uint64_t key = UploadKey(f, is_signed, srgb, content) ^ 0x9E3779B97F4A7C15ull; // the volume variant
    HostTexture *t = nullptr;
    if (auto it = im.uploads.find(key); it != im.uploads.end())
    {
      t = it->second;
    }
    else
    {
      t = CreateUploadTexture(*dev, im, f, fi, is_signed, srgb, content, true);
      if (!t)
      {
        return view;
      }
      // pdz_native_texture_supersede: the older contents of the same volume copy (a stack rewritten in place gets a new
      // key per content, and LookupVolume has no full-extent check that would replace it) go now, as in Lookup.
      t->identity = UploadKey(f, is_signed, srgb, 0) ^ 0x9E3779B97F4A7C15ull;
      if (REXCVAR_GET(pdz_native_texture_supersede))
      {
        for (auto other = im.uploads.begin(); other != im.uploads.end();)
        {
          HostTexture *o = other->second;
          if (o->volume_of_array && o->identity == t->identity && o->last_used_frame != im.frame)
          {
            ForgetFast(im, o);
            DestroyTexture(*dev, o);
            other = im.uploads.erase(other);
            ++im.uploads_replaced;
            ++im.uploads_superseded;
          }
          else
          {
            ++other;
          }
        }
      }
      im.uploads.emplace(key, t);
      MarkMirrorsStale(im, f, t->full_bytes);
      static uint32_t logged = 0;
      if (logged++ < 8)
      {
        REXLOG_INFO("pdz_native: volume view of {} texture {:08X} {}x{}x{} for a tfetch3D ({} layers x {} replicas, {} "
                    "mips, volume filter {}, sdk layers {})",
                    fi.name, f.base_address, f.width, f.height, t->depth, t->volume_layers,
                    t->depth / std::max(1u, t->volume_layers), t->mips, f.vol_mag_filter ? "linear" : "point",
                    REXCVAR_GET(pdz_native_volume_sdk_layers) ? "on" : "off");
      }
    }
    t->last_used_frame = im.frame;
    dev->Transition(t->resource, t->state,
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    view.srv = SrvFor(*dev, *t, f.swizzle, fi.host_swizzle);
    if (!view.srv.ptr)
    {
      return TextureView{};
    }
    view.texture = t;
    return view;
  }

  namespace
  {
    // The prepare-ahead of one decoded fetch constant (Prefetch and the prewarm). Returns what happened: 0 submitted,
    // 1 already cached or pending, 2 not an upload texture (unsupported, a mirror, out of range), 3 the pending
    // limit, 4 the guest memory does not hold `expect_content` (prewarm only: the data isn't loaded, or changed).
    int PrepareAhead(Textures &self, Textures::Impl &im, const FetchDecoded &f, bool prewarm, uint64_t expect_content,
                     bool discovered = false)
    {
      if (f.type != 2 || !f.base_address || f.width == 0 || f.height == 0)
      {
        return 2;
      }
      const FormatInfo &fi = kFormats[f.format & 63];
      if (!fi.supported || self.FindMirror(f) || FindAliasMirror(im, f, false))
      {
        return 2; // a resolve mirror serves it: no upload
      }
      const uint32_t extent = FullExtentBytes(f, fi, false);
      if (uint64_t(f.base_address) + extent > 0x20000000ull)
      {
        return 2;
      }
      const bool is_signed = ViewSigned(f);
      const bool srgb = AnyGamma(f.signs);
      const uint64_t content = discovered ? SafeContentHash(f.base_address) : ContentHash(f.base_address);
      if (prewarm && content != expect_content)
      {
        return 4;
      }
      const uint64_t key = UploadKey(f, is_signed, srgb, content);
      if (im.uploads.count(key) || im.pending.count(key))
      {
        return 1;
      }
      if (im.pending.size() >= Textures::Impl::kMaxPending)
      {
        return 3;
      }
      TextureJob *job = new TextureJob();
      job->f = f;
      job->fi = &fi;
      job->is_signed = is_signed;
      job->srgb = srgb;
      job->content_hash = content;
      job->key = key;
      job->range_begin = f.base_address;
      job->range_end = f.base_address + std::max(extent, 4096u);
      job->queued_frame = im.frame;
      job->queued_time = std::chrono::steady_clock::now();
      job->prewarm = prewarm;
      job->discovered = discovered;
      job->extent = extent;
      im.pending.emplace(key, job);
      ++im.win.prefetched;
      asyncup::Submit(job);
      return 0;
    }
  } // namespace

  void Textures::Prefetch(const uint8_t *fetch_be24)
  {
    NativeDevice *dev = NativeDevice::Get();
    if (!dev || !asyncup::Active(*dev))
    {
      return;
    }
    Impl &im = *impl_;
    // Once per fetch constant per frame (Lookup's fast path works the same way).
    Impl::PrefetchSeen &seen =
        im.prefetch_seen[uint32_t(XXH3_64bits(fetch_be24, 24)) & (Impl::kPrefetchSeenSize - 1)];
    if (seen.frame == im.frame && std::memcmp(seen.bytes, fetch_be24, 24) == 0)
    {
      return;
    }
    std::memcpy(seen.bytes, fetch_be24, 24);
    seen.frame = im.frame;
    if (im.pending.size() >= Impl::kMaxPending)
    {
      return;
    }
    const FetchDecoded f = DecodeFetch(fetch_be24);
    // pdz_native_prefetch_skip_recent (overnight 30-31 Oct): a fetch constant whose texture Lookup validated this or
    // the previous frame is cached: the prepare-ahead (a hint) would only hash its first 4 KB a second time. Lookup
    // still checks it as before (a changed texture is created there, as without the async path).
    if (REXCVAR_GET(pdz_native_prefetch_skip_recent))
    {
      const Impl::FastEntry &fe = im.fast[uint32_t(XXH3_64bits(f.dwords, sizeof(f.dwords))) & (Impl::kFastSize - 1)];
      if (fe.texture && fe.frame + 1 >= im.frame && std::memcmp(fe.dwords, f.dwords, sizeof(f.dwords)) == 0)
      {
        ++im.win.prefetch_skipped;
        return;
      }
    }
    PrepareAhead(*this, im, f, false, 0);
  }

  // ---- pdz_native_texture_prewarm (log 315) -----------------------------------------------------------------------
  namespace
  {
    std::string PrewarmDir()
    {
      return NativeCacheDir() + "\\texture_prewarm"; // per user in a release (native_dxc.h)
    }

    std::string PrewarmPath(const std::string &key)
    {
      std::string name;
      for (const char c : key)
      {
        name += (std::isalnum(uint8_t(c)) || c == '_' || c == '-') ? c : '_';
      }
      if (name.empty())
      {
        name = "level";
      }
      return PrewarmDir() + "\\" + name + ".bin";
    }

    constexpr char kPrewarmMagic[8] = {'P', 'D', 'Z', 'T', 'X', 'P', 'W', '1'};

    // The level's set from the last run (empty when none).
    std::vector<Textures::Impl::PrewarmEntry> LoadPrewarm(const std::string &key)
    {
      std::vector<Textures::Impl::PrewarmEntry> out;
      FILE *f = std::fopen(PrewarmPath(key).c_str(), "rb");
      if (!f)
      {
        return out;
      }
      char magic[8] = {};
      uint32_t count = 0;
      if (std::fread(magic, 1, 8, f) == 8 && std::memcmp(magic, kPrewarmMagic, 8) == 0 &&
          std::fread(&count, 4, 1, f) == 1 && count <= Textures::Impl::kLevelSetMax)
      {
        out.resize(count);
        if (std::fread(out.data(), sizeof(Textures::Impl::PrewarmEntry), count, f) != count)
        {
          out.clear();
        }
      }
      std::fclose(f);
      return out;
    }

    // Written on its own thread (a copy of the set): never file I/O under the render mutex.
    void SavePrewarm(const std::string &key, std::vector<Textures::Impl::PrewarmEntry> entries)
    {
      std::thread([key, entries = std::move(entries)]() {
        std::error_code ec;
        std::filesystem::create_directories(PrewarmDir(), ec);
        const std::string path = PrewarmPath(key), tmp = path + ".tmp";
        FILE *f = std::fopen(tmp.c_str(), "wb");
        if (!f)
        {
          return;
        }
        const uint32_t count = uint32_t(entries.size());
        bool ok = std::fwrite(kPrewarmMagic, 1, 8, f) == 8 && std::fwrite(&count, 4, 1, f) == 1 &&
                  std::fwrite(entries.data(), sizeof(Textures::Impl::PrewarmEntry), count, f) == count;
        ok = std::fclose(f) == 0 && ok;
        if (ok)
        {
          std::filesystem::rename(tmp, path, ec);
          if (ec)
          {
            std::filesystem::remove(path, ec);
            std::filesystem::rename(tmp, path, ec);
          }
        }
        else
        {
          std::filesystem::remove(tmp, ec);
        }
      }).detach();
    }

    void NoteLevelUpload(Textures::Impl &im, const FetchDecoded &f, uint64_t content)
    {
      if (im.level_playing && !im.disc_reported)
      {
        im.disc_first_keys.insert(UploadKey(f, ViewSigned(f), AnyGamma(f.signs), content));
      }
      if (im.level_key.empty() || im.level_set.size() >= Textures::Impl::kLevelSetMax ||
          std::chrono::duration<double>(std::chrono::steady_clock::now() - im.level_start_time).count() >
              Textures::Impl::kLevelRecordSeconds)
      {
        return;
      }
      const uint64_t h = XXH3_64bits(f.dwords, sizeof(f.dwords));
      Textures::Impl::PrewarmEntry &e = im.level_set[h];
      if (e.content != content || std::memcmp(e.dwords, f.dwords, sizeof(f.dwords)) != 0)
      {
        std::memcpy(e.dwords, f.dwords, sizeof(f.dwords));
        e.content = content;
        im.level_set_dirty = true;
      }
    }

    void FlushLevelSet(Textures::Impl &im, bool force)
    {
      if (im.level_key.empty() || !im.level_set_dirty)
      {
        return;
      }
      const auto now = std::chrono::steady_clock::now();
      if (!force && std::chrono::duration<double>(now - im.level_set_saved).count() < 30.0)
      {
        return;
      }
      std::vector<Textures::Impl::PrewarmEntry> entries;
      entries.reserve(im.level_set.size());
      for (const auto &kv : im.level_set)
      {
        entries.push_back(kv.second);
      }
      SavePrewarm(im.level_key, std::move(entries));
      im.level_set_dirty = false;
      im.level_set_saved = now;
    }

    uint64_t PendingPrewarmBytes(const Textures::Impl &im)
    {
      uint64_t bytes = 0;
      for (const auto &kv : im.pending)
      {
        bytes += kv.second->prewarm ? kv.second->extent : 0;
      }
      return bytes;
    }

    void ReportPrewarm(Textures::Impl &im, const char *why)
    {
      if (im.prewarm_reported)
      {
        return;
      }
      im.prewarm_reported = true;
      const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - im.level_start_time).count();
      PDZ_LOG_DEFERRED("pdz_native: prewarm: level '{}': {} textures from the last run: {} prepared ahead ({:.2f} MB), {} "
                       "already cached, {} not matching guest memory after {} pass(es) (data not loaded, or changed), {} "
                       "not upload textures; {} ({:.1f} s after the level started)",
                       im.level_key, im.prewarm_from_file, im.prewarm_submitted,
                       double(im.prewarm_submitted_bytes) / 1048576.0, im.prewarm_cached, im.prewarm_stale, im.prewarm_pass,
                       im.prewarm_unsupported, why, secs);
    }

    void LevelSummary(Textures::Impl &im)
    {
      const ShaderStats &sh = ShaderCache::Get().stats();
      uint32_t pso = 0, pso_built = 0;
      double pso_built_ms = 0.0;
      PipelineCache::Get().FirstUseTotals(pso, pso_built, pso_built_ms);
      const uint32_t new_psos = pso - im.lvl_pso0, built = pso_built - im.lvl_pso_built0;
      // Item f of the overnight brief: "N pipelines prepared, M built during play" (prepared = taken ready from the
      // warm-up / prewarm; built = made at the draw: driver compile, library load or a wait for a worker).
      PDZ_LOG_DEFERRED("pdz_native: level start '{}': {} pipelines prepared, {} built during play ({:.1f} ms) in the "
                       "first {:.0f} s",
                       im.level_key, new_psos >= built ? new_psos - built : 0u, built, pso_built_ms - im.lvl_pso_built_ms0,
                       Textures::Impl::kLevelSummarySeconds);
      PDZ_LOG_DEFERRED("pdz_native: level start '{}': first {:.0f} s: {} textures waited for at the draw in {} frame(s) "
                       "({:.1f} ms, longest {:.2f} ms), {} taken prepared, {} created at the draw on the record thread "
                       "({:.1f} ms); prewarm: {} from the last run, {} prepared ahead, {} stale; shaders first used: {} "
                       "VS bindings ({:.1f} ms), {} pixel shaders ({:.1f} ms)",
                       im.level_key, Textures::Impl::kLevelSummarySeconds, im.lvl_waits, im.lvl_wait_frames, im.lvl_wait_ms,
                       im.lvl_wait_max_ms, im.lvl_taken, im.lvl_sync, im.lvl_sync_ms, im.prewarm_from_file,
                       im.prewarm_submitted, im.prewarm_stale, sh.first_use_vs - im.lvl_vs0,
                       sh.first_use_vs_ms - im.lvl_vs_ms0, sh.first_use_ps - im.lvl_ps0, sh.first_use_ps_ms - im.lvl_ps_ms0);
    }
  } // namespace

  void OnLevelChange(const char *key)
  {
    if (key && *key)
    {
      PipelineCache::Get().WaitForWarmupAtLevelStart(key); // pdz_native_pso_level_wait_ms (game thread)
    }
    Textures::Get().NoteLevelChange(key);
  }

  void OnLevelPlaying() { Textures::Get().NoteLevelPlaying(); }

  void Textures::NoteLevelPlaying()
  {
    Impl &im = *impl_;
    std::lock_guard<std::mutex> lock(im.prewarm_mutex);
    im.level_playing_pending = true;
  }

  namespace
  {
    // pdz_native_texture_discover: the D3D texture objects in the guest heap (0x40000000-0x7FFFFFFF; where the
    // loading puts them, 0x40-0x43xxxxxx in Datacore / Mansion / Jungle): the object's first dword has type 3 in
    // bits 16-23 (D3DCOMMON_TYPE_TEXTURE), its fetch constant is the 6 dwords at +16 (DecodeTextureObject), checked
    // like a draw's. With the first 4 KB of each one's data (0 = not readable: skipped). Worker thread: the memory is
    // copied out with ReadProcessMemory, as the guest commits and protects pages meanwhile.
    // Every page of guest physical [address, address + bytes) committed and readable (the loading creates a texture
    // object before all of its data is in: an upload job reading it then faults). Worker thread.
    bool PhysicalRangeReadable(uint32_t address, uint32_t bytes)
    {
      const uint8_t *p = PhysPtr(address);
      if (!p || !bytes || uint64_t(address) + bytes > 0x20000000ull)
      {
        return false;
      }
      // One copy per 4 MB: it fails when any page of it is not committed or not readable. (VirtualQuery would walk
      // the write watches' per-page protections: thousands of regions per texture, 11 s per scan.)
      constexpr uint32_t kStep = 4u << 20;
      thread_local std::vector<uint8_t> scratch(kStep);
      for (uint32_t at = 0; at < bytes; at += kStep)
      {
        const uint32_t n = std::min(kStep, bytes - at);
        SIZE_T got = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), p + at, scratch.data(), n, &got) || got != n)
        {
          return false;
        }
      }
      return true;
    }

    std::vector<Textures::Impl::Discovered> ScanTextureObjects()
    {
      std::vector<Textures::Impl::Discovered> out;
      rex::Runtime *runtime = rex::Runtime::instance();
      rex::memory::Memory *memory = runtime ? runtime->memory() : nullptr;
      if (!memory)
      {
        return out;
      }
      std::unordered_set<uint64_t> seen;
      constexpr size_t kChunk = 1u << 20;
      std::vector<uint8_t> buf(kChunk + 64);
      uint8_t *const begin = memory->virtual_membase() + 0x40000000u, *const end = memory->virtual_membase() + 0x80000000u;
      for (uint8_t *at = begin; at < end;)
      {
        MEMORY_BASIC_INFORMATION mi{};
        if (!VirtualQuery(at, &mi, sizeof(mi)))
        {
          break;
        }
        uint8_t *region_end = std::min(static_cast<uint8_t *>(mi.BaseAddress) + mi.RegionSize, end);
        const DWORD prot = mi.Protect & 0xFF;
        if (mi.State == MEM_COMMIT && !(mi.Protect & PAGE_GUARD) && prot != PAGE_NOACCESS && prot != 0)
        {
          for (uint8_t *c = at; c < region_end; c += kChunk)
          {
            const size_t want = std::min<size_t>(kChunk + 40, size_t(region_end - c));
            SIZE_T got = 0;
            if (want < 40 || !ReadProcessMemory(GetCurrentProcess(), c, buf.data(), want, &got) || got != want)
            {
              continue; // a page of it went away: the next scan sees it again
            }
            for (size_t o = 0; o + 40 <= want; o += 4)
            {
              const uint8_t *p = buf.data() + o;
              const uint32_t common = _byteswap_ulong(*reinterpret_cast<const uint32_t *>(p));
              if (((common >> 16) & 0xFFu) != 3u)
              {
                continue;
              }
              uint32_t d[6];
              for (int i = 0; i < 6; ++i)
              {
                d[i] = _byteswap_ulong(*reinterpret_cast<const uint32_t *>(p + 16 + 4 * i));
              }
              if ((d[0] & 3u) != 2u || !kFormats[d[1] & 63u].supported || (d[1] >> 12) == 0)
              {
                continue;
              }
              const FetchDecoded f = DecodeFetchDwords(d);
              // A real object's pitch covers its width (rounded up to 32, at least 256 for linear rows); the stale or
              // foreign "objects" the scan also meets have pitch 0 or sizes like 8119 x 8040.
              const uint32_t max_dim = std::max(f.width, f.height);
              if (f.width == 0 || f.height == 0 || f.width > 4096 || f.height > 4096 || f.base_address >= 0x20000000u ||
                  (f.mip_address && f.mip_address <= f.base_address) || f.pitch < f.width ||
                  f.pitch > std::max(Align(f.width, 32), 256u) || (1u << f.mip_max_level) > max_dim)
              {
                continue;
              }
              if (!seen.insert(XXH3_64bits(d, sizeof(d))).second)
              {
                continue;
              }
              // The base level and (generously) the mip chain must be in memory already.
              const uint32_t extent = FullExtentBytes(f, kFormats[f.format & 63], false);
              if (extent > (16u << 20) || !PhysicalRangeReadable(f.base_address, std::max(extent, 4096u)) ||
                  (f.mip_address && !PhysicalRangeReadable(f.mip_address, std::max(extent / 2, 4096u))))
              {
                continue;
              }
              Textures::Impl::Discovered e;
              std::memcpy(e.dwords, d, sizeof(d));
              e.content = SafeContentHash(f.base_address);
              if (e.content)
              {
                out.push_back(e);
              }
            }
          }
        }
        at = region_end;
      }
      return out;
    }

    // One step of pdz_native_texture_discover (the record thread's PrewarmStep, a level named).
    void DiscoveryStep(Textures &self, Textures::Impl &im, std::chrono::steady_clock::time_point now)
    {
      std::vector<Textures::Impl::Discovered> scanned;
      bool ready = false, playing_now = false;
      double scan_ms = 0.0;
      {
        std::lock_guard<std::mutex> lock(im.prewarm_mutex);
        if (im.scan_result_ready)
        {
          ready = true;
          scanned.swap(im.scan_result);
          scan_ms = im.scan_result_ms;
          im.scan_result_ready = false;
        }
        if (im.level_playing_pending)
        {
          playing_now = true;
          im.level_playing_pending = false;
        }
      }
      if (playing_now && !im.level_playing)
      {
        im.level_playing = true;
        im.level_playing_time = now;
      }
      if (ready)
      {
        const bool first = im.disc_scans == 0;
        ++im.disc_scans;
        im.disc_scan_ms += scan_ms;
        im.disc_scan_ms_max = std::max(im.disc_scan_ms_max, scan_ms);
        std::unordered_map<uint64_t, uint64_t> current;
        current.reserve(scanned.size());
        for (const Textures::Impl::Discovered &e : scanned)
        {
          const uint64_t fetch = XXH3_64bits(e.dwords, sizeof(e.dwords));
          current[fetch] = e.content;
          if (first)
          {
            im.disc_baseline.insert(fetch); // there before the loading: the front end's, or the level's from before
          }
          const bool learned = im.disc_learned.count(e.content) != 0;
          if ((first || im.disc_baseline.count(fetch)) && !learned)
          {
            continue;
          }
          if (!learned && !im.disc_learned.empty())
          {
            continue; // a later play: the level's own set from last time, nothing more
          }
          // Only once the same object shows the same first 4 KB in two scans in a row: the loading is still writing
          // the others (and a stale object's bytes keep changing).
          auto prev = im.disc_prev.find(fetch);
          if (!learned && (prev == im.disc_prev.end() || prev->second != e.content))
          {
            continue;
          }
          if (!im.disc_seen.insert(fetch).second)
          {
            continue;
          }
          ++(learned ? im.disc_by_learned : im.disc_new);
          im.disc_queue.push_back(e);
        }
        im.disc_prev.swap(current);
      }
      // Scans: from the start of the loading until 2 s into the level (at most 60 s), one at a time, 0.4 s apart.
      const double since_start = std::chrono::duration<double>(now - im.level_start_time).count();
      const bool scanning = since_start < 60.0 &&
                            (!im.level_playing || std::chrono::duration<double>(now - im.level_playing_time).count() < 2.0);
      if (scanning && !im.scan_busy->load() &&
          (im.disc_scans == 0 || std::chrono::duration<double>(now - im.scan_last).count() >= 0.4))
      {
        im.scan_last = now;
        im.scan_busy->store(true);
        std::shared_ptr<std::atomic<bool>> busy = im.scan_busy;
        Textures::Impl *imp = &im;
        std::thread([busy, imp]() {
          pdz::cpu::LowerBackgroundThread("texture discovery"); // pdz_cpu_placement
          const auto t0 = std::chrono::steady_clock::now();
          std::vector<Textures::Impl::Discovered> found = ScanTextureObjects();
          const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
          {
            std::lock_guard<std::mutex> lock(imp->prewarm_mutex);
            imp->scan_result = std::move(found);
            imp->scan_result_ready = true;
            imp->scan_result_ms = ms;
          }
          busy->store(false);
        }).detach();
      }
      // Submissions: a batch per step, within the prewarm's pending cap and pdz_native_texture_discover_mb in all.
      const uint64_t cap = uint64_t(std::max(0, REXCVAR_GET(pdz_native_texture_prewarm_mb))) << 20;
      const uint64_t budget = uint64_t(std::max(0, REXCVAR_GET(pdz_native_texture_discover_mb))) << 20;
      for (size_t n = 0; im.disc_cursor < im.disc_queue.size() && n < Textures::Impl::kPrewarmPerBatch; ++n)
      {
        if ((cap && (n & 7) == 0 && PendingPrewarmBytes(im) >= cap) || im.disc_submitted_bytes >= budget)
        {
          break;
        }
        const Textures::Impl::Discovered &e = im.disc_queue[im.disc_cursor];
        const FetchDecoded f = DecodeFetchDwords(e.dwords);
        const int r = PrepareAhead(self, im, f, true, e.content, true);
        if (r == 3)
        {
          break; // the pending limit: the same entry next step
        }
        ++im.disc_cursor;
        if (r == 0)
        {
          ++im.disc_submitted;
          im.disc_submitted_bytes += FullExtentBytes(f, kFormats[f.format & 63], false);
          im.disc_keys.insert(UploadKey(f, ViewSigned(f), AnyGamma(f.signs), e.content));
        }
        else if (r == 1)
        {
          ++im.disc_cached;
        }
        else
        {
          ++im.disc_other; // not an upload texture, or its data changed since the scan (a later scan queues it again)
        }
      }
      if (im.level_playing && !im.disc_reported &&
          std::chrono::duration<double>(now - im.level_playing_time).count() >= 10.0)
      {
        im.disc_reported = true;
        uint32_t prepared = 0;
        for (const uint64_t k : im.disc_first_keys)
        {
          prepared += im.disc_keys.count(k) ? 1 : 0;
        }
        PDZ_LOG_DEFERRED("pdz_native: texture discovery: level '{}': {} scans ({:.0f} ms each on average, longest {:.0f} "
                         "ms) found {} texture objects of the loading and {} by last time's content; {} prepared ahead "
                         "({:.1f} MB), {} already cached, {} skipped; the first 10 s of play drew {} new textures, {} of "
                         "them prepared ahead; {} taken prepared, {} refused (data changed since)",
                         im.level_key, im.disc_scans, im.disc_scans ? im.disc_scan_ms / im.disc_scans : 0.0,
                         im.disc_scan_ms_max, im.disc_new, im.disc_by_learned, im.disc_submitted,
                         double(im.disc_submitted_bytes) / 1048576.0, im.disc_cached, im.disc_other,
                         im.disc_first_keys.size(), prepared, im.disc_taken, im.disc_rejected);
      }
    }
  } // namespace

  void Textures::NoteLevelChange(const char *key)
  {
    Impl &im = *impl_;
    std::lock_guard<std::mutex> lock(im.prewarm_mutex);
    im.prewarm_level_pending = key ? key : "";
    im.prewarm_level_changed = true;
  }

  void Textures::PrewarmStep()
  {
    Impl &im = *impl_;
    pipeline_seed::PreloadStep(0.75); // pdz_native_shader_preload: front end and loading, before a level's first draws
    const auto now = std::chrono::steady_clock::now();
    // A level change from the game thread.
    std::string new_key;
    bool changed = false;
    {
      std::lock_guard<std::mutex> lock(im.prewarm_mutex);
      if (im.prewarm_level_changed)
      {
        changed = true;
        new_key = im.prewarm_level_pending;
        im.prewarm_level_changed = false;
      }
    }
    if (changed)
    {
      if (!im.level_key.empty())
      {
        FlushLevelSet(im, true);
        if (im.prewarm_active)
        {
          ReportPrewarm(im, "the level ended");
        }
      }
      // Prepared-ahead textures of the old level are not wanted any more.
      if (!im.level_key.empty() && im.level_key != new_key)
      {
        for (auto it = im.pending.begin(); it != im.pending.end();)
        {
          if (it->second->prewarm && it->second->state.load(std::memory_order_acquire) >= 2)
          {
            ++im.win.discarded;
            ++im.win.unused;
            DiscardJob(it->second);
            it = im.pending.erase(it);
          }
          else
          {
            ++it;
          }
        }
      }
      im.level_key = new_key;
      // pdz_native_texture_discover: a new level's discovery (the scan in flight, if any, is taken as its first).
      im.level_playing = false;
      im.disc_scans = 0;
      im.disc_scan_ms = im.disc_scan_ms_max = 0.0;
      im.disc_baseline.clear();
      im.disc_seen.clear();
      im.disc_prev.clear();
      im.disc_learned.clear();
      im.disc_keys.clear();
      im.disc_first_keys.clear();
      im.disc_queue.clear();
      im.disc_cursor = 0;
      im.disc_new = im.disc_by_learned = im.disc_submitted = im.disc_cached = im.disc_other = im.disc_submitted_bytes =
          im.disc_taken = im.disc_rejected = 0;
      im.disc_reported = new_key.empty() || !REXCVAR_GET(pdz_native_texture_discover);
      {
        std::lock_guard<std::mutex> lock(im.prewarm_mutex);
        im.level_playing_pending = false;
        im.scan_result_ready = false;
      }
      im.level_set.clear();
      im.level_set_dirty = false;
      im.level_set_saved = now;
      im.prewarm_queue.clear();
      im.prewarm_done.clear();
      im.prewarm_cursor = 0;
      im.prewarm_pass = 0;
      im.prewarm_active = false;
      im.prewarm_reported = true;
      im.prewarm_from_file = im.prewarm_submitted = im.prewarm_submitted_bytes = im.prewarm_cached = im.prewarm_stale =
          im.prewarm_unsupported = 0;
      im.lvl_waits = im.lvl_wait_frames = im.lvl_sync = im.lvl_taken = 0;
      im.lvl_wait_ms = im.lvl_wait_max_ms = im.lvl_sync_ms = 0.0;
      im.level_summary_done = new_key.empty();
      im.level_start_time = now;
      im.level_start_frame = im.frame;
      {
        const ShaderStats &sh = ShaderCache::Get().stats();
        im.lvl_vs0 = sh.first_use_vs;
        im.lvl_ps0 = sh.first_use_ps;
        im.lvl_vs_ms0 = sh.first_use_vs_ms;
        im.lvl_ps_ms0 = sh.first_use_ps_ms;
        PipelineCache::Get().FirstUseTotals(im.lvl_pso0, im.lvl_pso_built0, im.lvl_pso_built_ms0);
      }
      if (!new_key.empty())
      {
        // The last run's set (also the base of this run's: entries stay unless the data changed).
        im.prewarm_queue = LoadPrewarm(new_key);
        for (const Impl::PrewarmEntry &e : im.prewarm_queue)
        {
          im.level_set[XXH3_64bits(e.dwords, sizeof(e.dwords))] = e;
        }
        im.prewarm_from_file = im.prewarm_queue.size();
        im.prewarm_done.assign(im.prewarm_queue.size(), 0);
        NativeDevice *dev = NativeDevice::Get();
        const bool discover = REXCVAR_GET(pdz_native_texture_discover) && dev && asyncup::Active(*dev);
        if (discover)
        {
          // Found by content during the loading (the textures sit at other addresses each run: log 2026-11-03, 359 of
          // 367 entries never matched their address).
          for (const Impl::PrewarmEntry &e : im.prewarm_queue)
          {
            im.disc_learned.insert(e.content);
          }
        }
        im.prewarm_active = !discover && REXCVAR_GET(pdz_native_texture_prewarm) && dev && asyncup::Active(*dev) &&
                            !im.prewarm_queue.empty();
        im.prewarm_reported = !im.prewarm_active;
        im.prewarm_pass = 1;
        im.prewarm_pass_time = now;
        PDZ_LOG_DEFERRED("pdz_native: prewarm: level '{}' started: {} textures remembered from the last run{}",
                         new_key, im.prewarm_from_file,
                         im.prewarm_active ? ": preparing them ahead on the upload workers"
                         : im.prewarm_queue.empty() ? " (none: this run records the set for the next)"
                                                    : " (pdz_native_texture_prewarm off or no upload workers: not prepared)");
      }
    }
    if (im.level_key.empty())
    {
      return;
    }
    if (!im.disc_reported || im.disc_cursor < im.disc_queue.size())
    {
      DiscoveryStep(*this, im, now);
    }
    // The prewarm passes: this pass's entries in order, a few per batch; entries whose memory doesn't match yet are
    // retried in later passes (every 2 s for 60 s), the rest are done.
    if (im.prewarm_active)
    {
      const uint64_t cap = uint64_t(std::max(0, REXCVAR_GET(pdz_native_texture_prewarm_mb))) << 20;
      size_t n = 0;
      bool over_cap = false;
      while (im.prewarm_cursor < im.prewarm_queue.size() && n < Impl::kPrewarmPerBatch)
      {
        const size_t i = im.prewarm_cursor;
        if (im.prewarm_done[i])
        {
          ++im.prewarm_cursor;
          continue;
        }
        if (cap && (n == 0 || (n & 7) == 0) && PendingPrewarmBytes(im) >= cap)
        {
          over_cap = true;
          break;
        }
        const Impl::PrewarmEntry &e = im.prewarm_queue[i];
        const int r = PrepareAhead(*this, im, DecodeFetchDwords(e.dwords), true, e.content);
        if (r == 3)
        {
          break; // the pending limit: the same entry next batch
        }
        ++n;
        ++im.prewarm_cursor;
        if (r == 0)
        {
          im.prewarm_done[i] = 1;
          ++im.prewarm_submitted;
          im.prewarm_submitted_bytes += std::max(FullExtentBytes(DecodeFetchDwords(e.dwords),
                                                                 kFormats[DecodeFetchDwords(e.dwords).format & 63], false),
                                                 4096u);
        }
        else if (r == 1)
        {
          im.prewarm_done[i] = 1;
          ++im.prewarm_cached;
        }
        else if (r == 2)
        {
          im.prewarm_done[i] = 2;
          ++im.prewarm_unsupported;
        }
        // r == 4: left for the next pass
      }
      if (!over_cap && im.prewarm_cursor >= im.prewarm_queue.size())
      {
        // A pass is over: anything left is a retry.
        size_t left = 0;
        for (const uint8_t d : im.prewarm_done)
        {
          left += d == 0;
        }
        const double since_start = std::chrono::duration<double>(now - im.level_start_time).count();
        if (left == 0)
        {
          im.prewarm_active = false;
          ReportPrewarm(im, "done");
        }
        else if (since_start >= Impl::kPrewarmRetrySeconds)
        {
          im.prewarm_stale = left;
          im.prewarm_active = false;
          ReportPrewarm(im, "gave up on the rest");
        }
        else if (std::chrono::duration<double>(now - im.prewarm_pass_time).count() >= Impl::kPrewarmPassSeconds)
        {
          im.prewarm_cursor = 0;
          ++im.prewarm_pass;
          im.prewarm_pass_time = now;
        }
      }
    }
    FlushLevelSet(im, false);
    if (!im.level_summary_done &&
        std::chrono::duration<double>(now - im.level_start_time).count() >= Impl::kLevelSummarySeconds)
    {
      im.level_summary_done = true;
      LevelSummary(im);
    }
  }

  void Textures::NoteCpuWrite(uint32_t begin, uint32_t end, uint32_t mip_begin, uint32_t mip_end)
  {
    Impl &im = *impl_;
    ++im.locks;
    // pdz_native_async_upload: a prepared upload over the written range may hold the old bytes: not taken.
    for (auto &[key, job] : im.pending)
    {
      const bool over_base = job->range_begin < end && begin < job->range_end;
      const uint32_t job_mips = job->f.mip_address;
      const bool over_mips = (mip_end > mip_begin && job->range_begin < mip_end && mip_begin < job->range_end) ||
                             (job_mips && job_mips < end && begin < job_mips + (job->range_end - job->range_begin));
      if (over_base || over_mips)
      {
        job->invalidated = true;
      }
    }
    for (auto &[key, t] : im.uploads)
    {
      const uint32_t t_end = t->base_address + std::max(t->full_bytes, 1u);
      const bool over_base = t->base_address < end && begin < t_end;
      const bool over_mips = mip_end > mip_begin && t->base_address < mip_end && mip_begin < t_end;
      // Not the Texture3D copies of LookupVolume: they have no full-extent check (a stack rewritten past its first 4 KB
      // stays as uploaded, as before this switch).
      if (!t->mirror && !t->volume_of_array && (over_base || over_mips))
      {
        // Draws later in this frame must not take the fast path's entry (validated before the write).
        ForgetFast(im, t);
        im.lock_marks += !t->lock_dirty;
        t->lock_dirty = true;
        t->lock_frame = im.frame;
        if (t->last_used_frame == im.frame && REXCVAR_GET(pdz_native_texture_lock_check))
        {
          // Sampled earlier in this frame: the draws after the write must not reuse the tables built before it.
          im.lock_epochs += OnTextureRewritten(*t);
          Impl::FrameVersions *v = nullptr;
          for (auto &e : im.frame_versions)
          {
            if (e.frame == im.frame && e.base == t->base_address)
            {
              v = &e;
              break;
            }
          }
          if (!v)
          {
            v = &im.frame_versions[im.frame_versions_next++ % std::size(im.frame_versions)];
            *v = Impl::FrameVersions{t->base_address, im.frame, 1, false};
          }
          v->pending = true;
        }
      }
    }
  }

  void OnTextureLock(uint8_t *base, uint32_t texture_object, uint32_t flags)
  {
    if (!Enabled() || (flags & 0x10) || !IsGuestPointer(texture_object))
    {
      return; // D3DLOCK_READONLY: nothing is written
    }
    const FetchDecoded f = DecodeTextureObject(base, texture_object);
    if (f.type != 2 || !f.base_address)
    {
      return;
    }
    const FormatInfo &fi = kFormats[f.format & 63];
    const uint32_t bytes = std::max(FullExtentBytes(f, fi, false), 4096u);
    // The mips: their guest extent is at most the base level's (each level is a quarter of the one above, packed
    // tails included); an over-wide range only costs a re-check.
    const uint32_t mip_begin = f.mip_address;
    const uint32_t mip_end = f.mip_address ? f.mip_address + bytes : 0;
    if (recthread::Active())
    {
      recthread::OnTextureWrite(base, f.base_address, f.base_address + bytes, mip_begin, mip_end); // in draw order
    }
    else
    {
      RenderLock lock(RenderMutex());
      Textures::Get().NoteCpuWrite(f.base_address, f.base_address + bytes, mip_begin, mip_end);
    }
  }

  void Textures::WriteGuestLevel0(const FetchDecoded &f, const uint8_t *rgba, uint32_t row_pitch_bytes, uint32_t scale)
  {
    const FormatInfo &fi = kFormats[f.format & 63];
    if (fi.bytes_per_block != 4 || fi.block_w != 1 || !f.base_address || f.dimension == 2)
    {
      REXLOG_WARN("pdz_native: snapshot write-back needs a 32-bit-per-texel 2D texture (format {})", fi.name);
      return;
    }
    const LevelLayout l = ComputeLevel(f, fi, 0);
    if (uint64_t(f.base_address) + l.slice_stride_bytes + 0x4000 > 0x20000000ull)
    {
      return;
    }
    uint8_t *guest = PhysPtr(f.base_address);
    if (!guest)
    {
      return;
    }
    for (uint32_t y = 0; y < f.height; ++y)
    {
      const uint8_t *src_row = rgba + uint64_t(y) * scale * row_pitch_bytes;
      for (uint32_t x = 0; x < f.width; ++x)
      {
        uint8_t texel[4];
        std::memcpy(texel, src_row + uint64_t(x) * scale * 4, 4);
        EndianSwap(texel, 4, f.endian);
        const uint64_t offset = f.tiled ? uint64_t(uint32_t(Tiled2D(int32_t(x), int32_t(y), l.pitch_blocks, 2)))
                                        : uint64_t(y) * l.pitch_blocks * 4 + uint64_t(x) * 4;
        std::memcpy(guest + offset, texel, 4);
      }
    }
  }

  void Textures::EndFrame(uint64_t frame_index)
  {
    Impl &im = *impl_;
    im.frame = frame_index + 1;
    if (REXCVAR_GET(pdz_frame_timing_spike_ms) > 0 && im.locks != im.locks_logged &&
        im.frame - im.last_lock_log_frame >= 600)
    {
      im.last_lock_log_frame = im.frame;
      im.locks_logged = im.locks;
      PDZ_LOG_DEFERRED("pdz_native: texture locks (CPU writes) so far: {} locks marked {} cached texture(s); {} | draws "
                  "sampling a marked texture {}, of which with content older than guest memory {} | written after a "
                  "draw of the same frame sampled them: new table epochs {}, extra versions in a frame {} (most in one "
                  "frame {})",
                  im.locks, im.lock_marks,
                  REXCVAR_GET(pdz_native_texture_lock_check)
                      ? fmt::format("re-checked at the next use {} (new content {})", im.lock_rechecks, im.lock_changes)
                      : std::string("pdz_native_texture_lock_check off: not re-checked"),
                  im.lock_dirty_draws, im.stale_draws, im.lock_epochs, im.multi_versions, im.max_versions);
    }
    // pdz_native_async_upload: prepared uploads no draw took within 600 frames (a stale fetch constant, a texture a
    // mirror serves after all) or invalidated ones go; finished copy lists / buffers are recycled; the 5 s line.
    if (!im.pending.empty() && (im.frame & 63) == 0)
    {
      for (auto it = im.pending.begin(); it != im.pending.end();)
      {
        TextureJob *job = it->second;
        const bool expired =
            job->prewarm ? std::chrono::duration<double>(std::chrono::steady_clock::now() - job->queued_time).count() >
                               Impl::kPrewarmKeepSeconds
                         : im.frame - job->queued_frame > 600;
        if (job->state.load(std::memory_order_acquire) >= 2 && (job->invalidated || expired))
        {
          ++im.win.discarded;
          im.win.invalidated += job->invalidated ? 1 : 0;
          im.win.unused += job->invalidated ? 0 : 1;
          DiscardJob(job);
          it = im.pending.erase(it);
        }
        else
        {
          ++it;
        }
      }
    }
    asyncup::Tick();
    {
      const auto now = std::chrono::steady_clock::now();
      if (im.win_start == std::chrono::steady_clock::time_point{})
      {
        im.win_start = now;
      }
      const double secs = std::chrono::duration<double>(now - im.win_start).count();
      if (secs >= 5.0)
      {
        asyncup::Counters &c = asyncup::Stats();
        const uint64_t done = c.jobs_done.load(std::memory_order_relaxed), bytes = c.bytes.load(std::memory_order_relaxed),
                       us = c.worker_us.load(std::memory_order_relaxed),
                       failed = c.jobs_failed.load(std::memory_order_relaxed);
        const uint64_t d_done = done - im.async_done_seen, d_bytes = bytes - im.async_bytes_seen,
                       d_us = us - im.async_us_seen, d_failed = failed - im.async_failed_seen;
        im.async_done_seen = done;
        im.async_bytes_seen = bytes;
        im.async_us_seen = us;
        im.async_failed_seen = failed;
        const Impl::UploadWindow &w = im.win;
        if (d_done || d_failed || w.sync || w.taken || w.discarded || w.evicted || w.prefetch_skipped)
        {
          PDZ_LOG_DEFERRED(
              "pdz_native: uploads ({:.1f} s): prepared on {} worker(s) {} ({:.2f} MB, worker cpu {:.1f} ms, failed {}) | "
              "taken at the draw {} ({:.2f} MB), of which waited for {} in {} frame(s) ({:.2f} ms total, longest "
              "{:.2f} ms) | discarded {} (CPU-written since {}, unused {}) | created at the draw on the record thread "
              "{} ({:.2f} MB, {:.1f} ms) | pending {} | evicted {} (pdz_native_texture_keep_seconds {}, {:.0f} fps) | "
              "prefetch hints skipped (validated last frame) {} | texture watch: clean {} invalidated {} armed {} "
              "refused {} (armed now {}; verify checked {} mismatches {})",
              secs, asyncup::WorkerCount(), d_done, double(d_bytes) / 1048576.0, double(d_us) / 1000.0, d_failed,
              w.taken, double(w.taken_bytes) / 1048576.0, w.waits, w.wait_frames, w.wait_ms, w.wait_max_ms,
              w.discarded, w.invalidated, w.unused, w.sync, double(w.sync_bytes) / 1048576.0, w.sync_ms,
              im.pending.size(), w.evicted, REXCVAR_GET(pdz_native_texture_keep_seconds), im.eviction_fps,
              w.prefetch_skipped, g_tex_watch_stats.clean, g_tex_watch_stats.invalidated, g_tex_watch_stats.armed,
              g_tex_watch_stats.refused, g_tex_watch.size(), g_tex_watch_stats.verified, g_tex_watch_stats.mismatches);
          g_tex_watch_stats.clean = g_tex_watch_stats.invalidated = g_tex_watch_stats.armed = g_tex_watch_stats.refused = 0;
        }
        im.win = Impl::UploadWindow{};
        im.win_start = now;
      }
    }
    // The surface registry's maintenance runs at the same safe point (the swap, outside any draw / clear / resolve):
    // retired surfaces every frame, long-unused surfaces on its own 300-frame cadence (REVIEW2.md C3).
    Surfaces::Get().EndFrame(frame_index);
    if (im.frame - im.last_eviction_frame < 300)
    {
      return;
    }
    // pdz_native_texture_keep_seconds (log 307): unused for that long (at the measured frame rate), at least 900
    // frames; while the cache is over pdz_native_texture_keep_mb, 900 frames as before.
    {
      const auto now = std::chrono::steady_clock::now();
      if (im.eviction_time != std::chrono::steady_clock::time_point{})
      {
        const double secs = std::chrono::duration<double>(now - im.eviction_time).count();
        if (secs > 0.0)
        {
          im.eviction_fps = std::clamp(double(im.frame - im.last_eviction_frame) / secs, 10.0, 1000.0);
        }
      }
      im.eviction_time = now;
    }
    im.last_eviction_frame = im.frame;
    NativeDevice *dev = NativeDevice::Get();
    if (!dev)
    {
      return;
    }
    uint64_t cached_bytes = 0;
    for (const auto &entry : im.uploads)
    {
      cached_bytes += entry.second->upload_bytes;
    }
    const int32_t keep_s = REXCVAR_GET(pdz_native_texture_keep_seconds);
    const bool over_budget = cached_bytes > uint64_t(REXCVAR_GET(pdz_native_texture_keep_mb)) << 20;
    const uint64_t keep_frames =
        keep_s <= 0 || over_budget ? 900u : std::max<uint64_t>(900u, uint64_t(double(keep_s) * im.eviction_fps));
    uint32_t evicted = 0;
    for (auto it = im.uploads.begin(); it != im.uploads.end();)
    {
      HostTexture *t = it->second;
      if (im.frame - t->last_used_frame > keep_frames)
      {
        for (auto &fe : im.fast)
        {
          if (fe.texture == t)
          {
            fe.texture = nullptr;
          }
        }
        if (!im.created_identities.empty())
        {
          // The creation log's reason (spike log on): this identity's next creation is a re-creation after eviction.
          if (auto c = im.created_identities.find(t->identity); c != im.created_identities.end())
          {
            c->second = 2;
          }
        }
        DestroyTexture(*dev, t);
        it = im.uploads.erase(it);
        ++evicted;
      }
      else
      {
        ++it;
      }
    }
    if (evicted)
    {
      im.uploads_evicted += evicted;
      im.win.evicted += evicted;
      PDZ_LOG_DEFERRED("pdz_native: evicted {} textures unused for {} frames ({} cached, {:.0f} MB before{})", evicted,
                       keep_frames, im.uploads.size(), double(cached_bytes) / 1048576.0,
                       over_budget ? ", over pdz_native_texture_keep_mb: the 900-frame rule" : "");
    }

    // Resolve mirrors (REVIEW2.md C3): each holds a render target at the host scale and an RTV; a level's or a
    // match's resolve destinations are otherwise kept for the session. Lookup (sampling, present, auto-diff) and
    // Resolve stamp last_used_frame, so a mirror used this frame never qualifies. Unlike a surface (EDRAM content
    // does not outlive the next passes), a mirror's content exists nowhere else on the native path (a native resolve
    // writes only the mirror, not guest memory, except the de-rez snapshot), so a texture resolved once and sampled
    // rarely must survive a pause: a mirror unused for kMirrorEvictFrames goes only when another mirror over the same
    // guest memory was used after it (the game reused the memory: a later level or match), otherwise after
    // kMirrorStaleFrames. The pointers held elsewhere are dropped first: the fast cache entries and the de-rez
    // LastResolve (Surfaces::ForgetMirror).
    // Finding 8 (resolve mirrors beyond the exact key): a stale mirror (an ordinary texture was uploaded over its
    // range since its last resolve; no lookup serves it) also goes after kMirrorEvictFrames. An evicted mirror
    // leaves mirrors_by_base and takes its alias views with it (they point at it; sampling a view stamps the
    // mirror too, see Lookup). An alias view unused for kMirrorEvictFrames goes on its own: it is a copy, made
    // again from its mirror on the next alias hit.
    auto guest_end = [](const HostTexture &m) -> uint64_t {
      // Approximate guest extent of the key: base level, 32x32-texel tile padding.
      const FormatInfo &fi = kFormats[m.format & 63];
      const uint64_t bw = std::max<uint32_t>(fi.block_w, 1u), bh = std::max<uint32_t>(fi.block_h, 1u);
      const uint64_t w = (uint64_t(m.key_width) + 31) / 32 * 32, h = (uint64_t(m.key_height) + 31) / 32 * 32;
      return uint64_t(m.base_address) + ((w + bw - 1) / bw) * ((h + bh - 1) / bh) * std::max<uint32_t>(fi.bytes_per_block, 1u);
    };
    auto superseded = [&](const HostTexture &m) {
      const uint64_t begin = m.base_address, end = guest_end(m);
      for (const auto &[key, other] : im.mirrors)
      {
        if (other != &m && other->last_used_frame > m.last_used_frame && other->base_address < end &&
            guest_end(*other) > begin)
        {
          return true;
        }
      }
      return false;
    };
    uint32_t mirrors_evicted = 0;
    for (auto it = im.mirrors.begin(); it != im.mirrors.end();)
    {
      HostTexture *t = it->second;
      if (t->last_used_frame + Impl::kMirrorEvictFrames < im.frame &&
          (t->last_used_frame + Impl::kMirrorStaleFrames < im.frame || t->stale || superseded(*t)))
      {
        if (im.mirror_evict_logged < 32)
        {
          ++im.mirror_evict_logged;
          REXLOG_INFO("pdz_native: resolve mirror {}x{} (guest {:08X} {} {}x{}) evicted: unused since frame {} (now {}){}",
                      t->width, t->height, t->base_address, kFormats[t->format & 63].name, t->key_width,
                      t->key_height, t->last_used_frame, im.frame, t->stale ? ", stale" : "");
        }
        ForgetFast(im, t); // its fast cache entries and those of its alias views
        Surfaces::Get().ForgetMirror(t);
        for (auto v = im.mirror_views.begin(); v != im.mirror_views.end();)
        {
          if (v->second->view_source == t)
          {
            DestroyTexture(*dev, v->second);
            v = im.mirror_views.erase(v);
            ++im.alias_views_evicted;
          }
          else
          {
            ++v;
          }
        }
        if (auto b = im.mirrors_by_base.find(t->base_address); b != im.mirrors_by_base.end())
        {
          std::vector<HostTexture *> &same_base = b->second;
          same_base.erase(std::remove(same_base.begin(), same_base.end(), t), same_base.end());
          if (same_base.empty())
          {
            im.mirrors_by_base.erase(b);
          }
        }
        DestroyTexture(*dev, t);
        it = im.mirrors.erase(it);
        ++mirrors_evicted;
      }
      else
      {
        ++it;
      }
    }
    for (auto v = im.mirror_views.begin(); v != im.mirror_views.end();)
    {
      HostTexture *t = v->second;
      if (t->last_used_frame + Impl::kMirrorEvictFrames < im.frame)
      {
        ForgetFast(im, t);
        DestroyTexture(*dev, t);
        v = im.mirror_views.erase(v);
        ++im.alias_views_evicted;
      }
      else
      {
        ++v;
      }
    }
    if (mirrors_evicted)
    {
      im.mirrors_evicted += mirrors_evicted;
      const NativeDevice::PersistentHeapStats hs = dev->GetPersistentHeapStats();
      PDZ_LOG_DEFERRED("pdz_native: evicted {} resolve mirrors unused for {} frames ({} kept, {} evicted in total) | RTV "
                  "{}/{} staging SRV {}/{} live",
                  mirrors_evicted, Impl::kMirrorEvictFrames, im.mirrors.size(), im.mirrors_evicted, hs.rtv_live,
                  hs.rtv_size, hs.staging_live, hs.staging_size);
    }

    // Resolve mirrors beyond the exact key (finding 8), every 300 frames while anything moved. Alias hits count
    // slow-path lookups (once per fetch constant per frame), view copies count region copies of alias views.
    if (im.alias_hits != im.logged_alias_hits || im.stale_marks != im.logged_stale_marks ||
        im.stale_revivals != im.logged_stale_revivals || im.alias_views_evicted != im.logged_alias_views_evicted)
    {
      im.logged_alias_hits = im.alias_hits;
      im.logged_stale_marks = im.stale_marks;
      im.logged_stale_revivals = im.stale_revivals;
      im.logged_alias_views_evicted = im.alias_views_evicted;
      uint32_t stale = 0;
      for (const auto &entry : im.mirrors)
      {
        stale += entry.second->stale ? 1 : 0;
      }
      REXLOG_INFO("pdz_native: resolve mirrors {} ({} stale, {} alias views): alias hits {}, view copies {}, alias "
                  "refused for changed guest memory {}, stale marks {}, revived by a resolve {}, alias views evicted {}",
                  im.mirrors.size(), stale, im.mirror_views.size(), im.alias_hits, im.alias_view_copies,
                  im.alias_content_misses, im.stale_marks, im.stale_revivals, im.alias_views_evicted);
    }
  }

  Textures::MemoryStats Textures::GetMemoryStats()
  {
    Impl &im = *impl_;
    MemoryStats s;
    NativeDevice *dev = NativeDevice::Existing();
    // The allocation size (64 KB pages for a committed resource), asked once per texture.
    auto bytes = [dev](HostTexture &t) -> uint64_t {
      if (!t.host_bytes && t.resource && dev)
      {
        const D3D12_RESOURCE_DESC desc = t.resource->GetDesc();
        t.host_bytes = dev->device()->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
      }
      return t.host_bytes;
    };
    for (auto &entry : im.uploads)
    {
      HostTexture &t = *entry.second;
      const uint64_t b = bytes(t);
      ++s.uploads;
      s.upload_bytes += b;
      s.srvs += t.srv_count;
      if (t.volume_of_array)
      {
        ++s.volumes;
        s.volume_bytes += b;
      }
    }
    for (auto &entry : im.mirrors)
    {
      HostTexture &t = *entry.second;
      ++s.mirrors;
      s.mirrors_stale += t.stale ? 1 : 0;
      s.mirror_bytes += bytes(t);
      s.srvs += t.srv_count;
    }
    for (auto &entry : im.mirror_views)
    {
      HostTexture &t = *entry.second;
      ++s.views;
      s.view_bytes += bytes(t);
      s.srvs += t.srv_count;
    }
    s.created = im.uploads_created;
    s.evicted = im.uploads_evicted;
    s.replaced = im.uploads_replaced;
    s.superseded = im.uploads_superseded;
    s.mirrors_evicted = im.mirrors_evicted;
    s.views_evicted = im.alias_views_evicted;
    s.log_seen = g_log_seen.size();
    return s;
  }

  // ---------------------------------------------------------------------------
  // Samplers
  // ---------------------------------------------------------------------------
  namespace
  {
    D3D12_TEXTURE_ADDRESS_MODE AddressMode(uint32_t clamp)
    {
      switch (clamp)
      {
      case 0: return D3D12_TEXTURE_ADDRESS_MODE_WRAP;          // repeat
      case 1: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;        // mirrored repeat
      case 2: return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;         // clamp to edge
      case 3: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE;   // mirror clamp to edge
      case 4: return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;         // clamp to halfway (approximated)
      case 5: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE;   // mirror clamp to halfway (approximated)
      case 6: return D3D12_TEXTURE_ADDRESS_MODE_BORDER;        // clamp to border
      default: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE;  // mirror clamp to border (approximated)
      }
    }
  } // namespace

  namespace
  {
    // The SDK's anisotropic_override (GPU cvar, defined in the SDK DLL: read through the registry by name, which parses
    // a string, so refreshed every 4096 samplers built; a frame builds a few hundred). -1 = no override.
    int32_t SdkAnisotropicOverride()
    {
      static int32_t value = -1;
      static uint32_t calls = 0;
      if ((calls++ & 4095u) == 0)
      {
        const std::string s = rex::cvar::GetFlagByName("anisotropic_override");
        value = s.empty() ? -1 : int32_t(std::strtol(s.c_str(), nullptr, 10));
      }
      return value;
    }
  } // namespace

  D3D12_SAMPLER_DESC GetSampler(const FetchDecoded &f, uint32_t override_code)
  {
    uint32_t mag = (override_code >> 0) & 3;
    uint32_t min = (override_code >> 2) & 3;
    uint32_t mip = (override_code >> 4) & 3;
    uint32_t aniso = (override_code >> 6) & 7;
    if (mag == 3)
    {
      mag = f.mag_filter;
    }
    if (min == 3)
    {
      min = f.min_filter;
    }
    if (mip == 3)
    {
      mip = f.mip_filter;
    }
    if (aniso == 7)
    {
      aniso = f.aniso_filter;
    }
    if (REXCVAR_GET(pdz_native_aniso_override))
    {
      // D3D12TextureCache::GetSamplerParameters: anisotropic_override replaces the anisotropy of every texture with
      // mips (max level above min level, mip filter not base map) whose min and mag filters are linear and whose mip
      // filter is point or linear; the result is capped at 16:1.
      const int32_t forced = SdkAnisotropicOverride();
      if (forced > -1 && forced < 6 && f.mip_max_level > f.mip_min_level && mag == 1 && min == 1 && mip <= 1)
      {
        aniso = uint32_t(forced);
      }
      aniso = std::min(aniso, 5u);
    }
    // Filter values: 0 point, 1 linear, 2 base map (mip: no mip filtering), 3 fetch (resolved above).
    const bool mag_linear = mag == 1;
    const bool min_linear = min == 1;
    const bool mip_linear = mip == 1;
    const bool base_map = mip == 2;

    D3D12_SAMPLER_DESC d{};
    if (aniso != 0 && aniso <= 5)
    {
      d.Filter = D3D12_FILTER_ANISOTROPIC;
      d.MaxAnisotropy = 1u << (aniso - 1);
    }
    else
    {
      d.Filter = D3D12_ENCODE_BASIC_FILTER(min_linear ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT,
                                           mag_linear ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT,
                                           mip_linear ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT,
                                           D3D12_FILTER_REDUCTION_TYPE_STANDARD);
      d.MaxAnisotropy = 1;
    }
    d.AddressU = AddressMode(f.clamp_x);
    d.AddressV = AddressMode(f.clamp_y);
    // A stacked texture's layer coordinate is clamped to the layer centres by the SDK (dxbc_translator_fetch.cpp,
    // k3DOrStacked), never wrapped: with WRAP, layer 0 at w = 0 would blend with the last layer in the volume view.
    d.AddressW = (f.dimension == 1 && REXCVAR_GET(pdz_native_stacked_layer_clamp)) ? D3D12_TEXTURE_ADDRESS_MODE_CLAMP
                                                                                   : AddressMode(f.clamp_z);
    d.MipLODBias = float(f.lod_bias) / 32.0f;
    d.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    const float border = (f.border_color == 1) ? 1.0f : 0.0f;
    d.BorderColor[0] = d.BorderColor[1] = d.BorderColor[2] = border;
    d.BorderColor[3] = border;
    d.MinLOD = float(f.mip_min_level);
    d.MaxLOD = base_map ? float(f.mip_min_level) : float(f.mip_max_level);
    if (d.MaxLOD < d.MinLOD)
    {
      d.MaxLOD = d.MinLOD;
    }
    return d;
  }

  D3D12_SAMPLER_DESC GetSampler(const uint8_t *fetch_be24, uint32_t override_code)
  {
    return GetSampler(DecodeFetch(fetch_be24), override_code);
  }

  const char *TextureFormatName(uint32_t format) { return kFormats[format & 63].name; }
} // namespace pdz::native
