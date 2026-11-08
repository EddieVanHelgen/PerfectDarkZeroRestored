// Native renderer (stage 1): per-draw path (native_draw.h). Everything here is O(1) per draw except the
// vertex / index uploads, which are cached across frames by (guest address, length, content sample hash) in
// persistent upload-heap chunks (GeometryCache). No heap allocation in steady state: the maps are reserved,
// the per-frame tables are reused by key, constant buffers come from the device's frame upload ring.
#include "native_draw.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <intrin.h> // __rdtsc (pdz_native_draw_timing)
#include <mutex>
#include <cstdlib>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <fmt/format.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>
#include <rex/hash.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h> // pdz_native_geometry_watch: the guest memory's write watches

#include "native.h"
#include "native_blend7e3.h"
#include "native_depth20e4.h"
#include "native_device.h"
#include "native_drawdump.h"
#include "native_pipeline.h"
#include "native_shaders.h"
#include "native_surfaces.h"
#include "native_passdump.h"
#include "native_recthread.h"
#include "native_textures.h"
#include "native_view.h"
#include "../frame_timing.h"

REXCVAR_DEFINE_BOOL(pdz_native_debug_no_cb_reuse, false, "Render",
                    "Native renderer diagnostic: upload the float constants for every draw (no reuse by dirty bits).");
REXCVAR_DEFINE_BOOL(pdz_native_state_filter, true, "Render",
                    "Native renderer (stage 2 M5 drawcost3): the draw path does not repeat a D3D12 state call whose "
                    "value the command list already has from the draw path in the same list epoch (topology, "
                    "viewport, scissor, stencil ref, blend factor, the four root CBVs, the three root descriptor "
                    "tables, vertex / index buffers, OMSetRenderTargets); a pass with its own pipeline, a Flush or a "
                    "new frame starts a new epoch. Nothing drawn changes. Off = every call on every draw, as before.");
REXCVAR_DEFINE_BOOL(pdz_native_cb_dedupe, true, "Render",
                    "Native renderer (stage 2 M5 drawcost4): a draw whose bool / loop or system constants are byte for "
                    "byte those of the previous upload in the same device frame binds that upload again instead of a "
                    "new copy (the same GPU address also lets pdz_native_state_filter skip the root CBV call). "
                    "Nothing drawn changes. Off = a new upload of both for every draw, as before.");
REXCVAR_DEFINE_BOOL(pdz_native_cb_used_only, true, "Render",
                    "Native renderer (stage 2 M5 drawcost): a float constant upload writes only the registers the "
                    "shader can read (sidecar float_constants_used, checked against the HLSL, plus the literal "
                    "registers) instead of the whole byte-swapped 4 KB block; the shader reads the same values. "
                    "Shaders with relative constant addressing, or whose HLSL the check does not understand, upload "
                    "the whole block as before. Off = the whole block for every shader.");
REXCVAR_DEFINE_BOOL(pdz_native_up_begin_once, true, "Render",
                    "Native renderer: the BeginVertices that D3DDevice_DrawVerticesUP (sub_826E9A60) calls inside itself "
                    "(return address 826E9AB0, ended by DrawVerticesUP's inlined EndVertices store) is not kept as a "
                    "pending batch: the DrawVerticesUP hook already drew it. Off = every DrawVerticesUP is drawn twice "
                    "(6929aff), which over-blends the frontend UI (mission select arrow / stars / tint).");
REXCVAR_DEFINE_BOOL(pdz_native_pending_begin_late, true, "Render",
                    "Native renderer: issue a BeginVertices batch the game ends without the EndVertices hook (inlined "
                    "store, e.g. the particle / render-list replay 825F1260) at the next native entry point (6929aff). "
                    "Off = the pre-6929aff behaviour (only EndVertices issues; such a batch is replaced by the next "
                    "BeginVertices): A/B for the mission select regression (stage1/subagents/uiselect.md).");
REXCVAR_DEFINE_BOOL(pdz_native_volume_view, true, "Render",
                    "Native renderer: bind a Texture3D copy of a 2D / stacked texture sampled with tfetch3D (else the "
                    "slot stays null). Off by default: Datacore level auto-diff 3.82 off vs 3.99-4.04 on (logs 049-053); "
                    "2.65 / 3.9% off vs 2.82 / 4.5% on (logs 106 / 112, the flare region above the door, "
                    "stage1/subagents/stacked.md). The copy's layer / mip layout: pdz_native_volume_sdk_layers.");
REXCVAR_DEFINE_INT32(pdz_native_log_up_fans, 0, "Render",
                     "Native renderer diagnostic: log the vertices (as uploaded, little-endian floats) and VS c0..c3 of "
                     "the first N DrawVerticesUP / BeginVertices TRIANGLEFAN / TRIANGLESTRIP draws with more than 8 "
                     "vertices (the Rooftops radar disc is a 62-vertex UP fan; 0 = off).");
REXCVAR_DEFINE_BOOL(pdz_native_volume_view_2d, true, "Render",
                    "Native renderer: the volume view (pdz_native_volume_view) for plain 2D textures only (one slice, "
                    "not stacked): a one-slice Texture3D samples exactly like the 2D texture, nothing is replicated. "
                    "The particle glow puffs (BeginVertices caller 825F1260, VS aee0d65f, PS 89234b6d / 041b3bef) "
                    "sample their 128x128 DXT4_5 atlas with tfetch3D; with the slot null they are invisible = the "
                    "Datacore light flare's white core (stage1/subagents/flarehook.md). Stacked textures stay under "
                    "pdz_native_volume_view. Off by default since 14:20: Datacore 2.80 / 4.4% with it on (gate 2.7 / 4.0): the puffs come out too strong. "
                    "7e3 rounding alone cannot explain it (< 0.01 HDR around the glow, stage1/subagents/hdr7e3.md); "
                    "A/B with pdz_native_7e3_blend 1 / 2 (the SDK's source-alpha clamp / its whole output merger).");
REXCVAR_DEFINE_BOOL(pdz_native_volume_view_dxn, true, "Render",
                    "Native renderer: the volume view (pdz_native_volume_view) for k_DXN textures only, stacked or plain "
                    "2D, under a tfetch3D slot. PDZ's water / ice / glass surfaces read their animated two-channel "
                    "normal stacks this way (Nightclub harbour PS ccf49f44 / a2917857 and pieces a271efa0, 18cdab4c, "
                    "89e948e7, 53aa4b67: DXN 128x128 stacks 0F2AD000 / 0F3AD000; Mansion ice d69ac83c / fdd88a8e; "
                    "Datacore glass 7131cd8b / 73db1019). With the slot null the normal decodes to (-1, -1, 0) = -T - B, "
                    "a flat normal lying in the surface plane: the harbour loses its ripples, fresnel and reflection "
                    "and turns flat dark (stage1/subagents/water2.md). A normal map only feeds lighting, never the "
                    "blended colour of particles (the Datacore glow puffs that keep pdz_native_volume_view_2d off are "
                    "DXT4_5). Off: such slots stay null unless pdz_native_volume_view / _2d.");
REXCVAR_DEFINE_BOOL(pdz_native_volume_view_stacked, false, "Render",
                    "Native renderer: the volume view (pdz_native_volume_view) for stacked textures of any format under a "
                    "tfetch3D slot, plain 2D ones excluded (those stay under pdz_native_volume_view_2d: the Datacore "
                    "glow puffs). A/B switch for the projected-texture lights of the light pass (PS 858ecd68 samples "
                    "its DXT1 256x256 projector 0DE2F000 with tfetch3D, layer = animation frame: null natively, the "
                    "likely reason resolve #12 lacks the Nightclub dock-wall caustics; if that texture is plain 2D it "
                    "needs _2d instead, the draw dump's tex line says which) and the stacked particle flipbooks "
                    "(Datacore 512x32x2): off until its Datacore gate run (stage1/subagents/water2.md).");
REXCVAR_DEFINE_BOOL(pdz_native_draw_topology, true, "Render",
                    "Native renderer: the host primitive topology and the QUADLIST / TRIANGLEFAN index expansion follow "
                    "each draw's own primitive type, as the SDK. Off = the topology of the draw that created the PSO "
                    "(TRIANGLELIST / STRIP / FAN and QUADLIST draws with the same shaders and state share one PSO): the "
                    "pre-fix behaviour that drew the Rooftops radar ring strips as fans of spikes "
                    "(stage1/subagents/radar.md).");
REXCVAR_DEFINE_BOOL(pdz_native_sampler_evict, true, "Render",
                    "Native renderer: when the persistent sampler region (1504 descriptors, content-cached sampler "
                    "tables shared by every frame) has no room for a new table, the tables no frame in flight can still "
                    "read are freed at once (first those unused for 600 frames, then, if that freed under 256 "
                    "descriptors, every table unused for 4 frames) and the new table goes there; at most one such pass "
                    "per frame. One level holds about 600 (Datacore, log 173), so without it a session of a few levels "
                    "fills the region and every later table comes from the 512-entry per-frame ring, where a busy frame "
                    "runs out and skips draws (memlog.md N3). Never triggers while the region has room: within a level "
                    "nothing changes. Off = tables are never freed (the old behaviour).");
REXCVAR_DEFINE_BOOL(pdz_native_geometry_shrink, true, "Render",
                    "Native renderer: the geometry cache (upload-heap chunks of 32 MB, at most 8) releases a chunk no "
                    "draw has used for 7200 frames (about 2 min at 60 Hz: a previous level's static geometry), keeping "
                    "at least 2; its cached ranges are dropped first. Checked every 600 frames. A range of such a chunk "
                    "drawn again is copied again, as on a cache miss (same bytes). Off = chunks are kept for the session "
                    "(up to 256 MB, recycled whole only once all 8 exist).");
REXCVAR_DEFINE_BOOL(pdz_native_indexed_up, true, "Render",
                    "Native renderer: draw D3DDevice_DrawIndexedVerticesUP (sub_826E9E18, hooked since stage1/subagents/"
                    "hud.md): the UI's indexed shapes (sub_827B5890: TRILIST, 16-bit indices), i.e. the framed top bar "
                    "of Subway / Bridge / Arena and the rings and glow over the front end (20 a frame). Off = not "
                    "drawn natively, as before the hook (A/B).");
REXCVAR_DECLARE(bool, pdz_native_texture_lock_tables); // native_textures.cpp: OnTextureRewritten
REXCVAR_DECLARE(bool, pdz_fix_water_loop); // game_cvars.cpp: the ripple stack's layers wrap (MarkWaterLoop)
REXCVAR_DECLARE(bool, pdz_game_fixes);
REXCVAR_DECLARE(bool, pdz_fix_short_stacks); // game_cvars.cpp: proven short animated stacks wrap too (ShortStackMask)
REXCVAR_DEFINE_STRING(pdz_fix_short_stacks_extra, "", "Testing",
                      "Test aid for pdz_fix_short_stacks: more stacks to wrap besides the built-in list, as "
                      "'<pixel shader hash, 16 hex>:<slot>:<v|p><register>' items separated by commas (e.g. "
                      "'1991475BAD6C860C:0:v19'): that slot's layers wrap while the named constant's .w (the layer "
                      "coordinate Rare animates) is not exactly 0. Read once a frame.");
REXCVAR_DEFINE_INT32(pdz_stack_layer_log, 0, "Render",
                     "Diagnostic (stacked-texture loops, overnight 1108 A6): every N device frames (0 = off) one "
                     "'pdz_stack_layer:' line per stacked texture drawn in that frame (pixel shader + slot + texture "
                     "address, first draw only): layers, the fetch's clamp_z, the vertex shader and every non-zero .w "
                     "of VS c0..c63 and PS c0..c31 (a row (0, 0, z, w) is marked '*z'), to see which layer coordinates "
                     "Rare animates and whether they leave the stack's range. Works with the null GPU backend and the "
                     "record thread (reads the device constant store, not the upload heap).");
REXCVAR_DEFINE_INT32(pdz_stack_layer_log_from, 0, "Render",
                     "Diagnostic: the first device frame pdz_stack_layer_log may log.");
REXCVAR_DEFINE_INT32(pdz_stack_layer_log_lines, 20000, "Render",
                     "Diagnostic: at most this many pdz_stack_layer lines in a session.");
REXCVAR_DEFINE_INT32(pdz_native_split_tables, 1, "Render",
                     "Native renderer, split screen (stage1/subagents/split.md): the per-frame SRV descriptor tables are "
                     "keyed by the draw's shaders and raw fetch constants, so a draw of player 2's pass found player "
                     "1's table and skipped Textures::Lookup: a texture the pass had resolved into again stayed in the "
                     "render-target state of the resolve (sampled with no barrier after the copy: stale texels) and its "
                     "alias views kept the old copy. 1 = in multi-pass frames (a depth surface resolved again into the "
                     "same texture within the frame; held 120 frames), a resolve into a texture that a table of the "
                     "current pass segment samples starts a new segment whose draws build their tables again. Frames "
                     "with one scene pass are unchanged. 2 = in every frame (single-player too: the bloom chain resolves "
                     "1E0BD000 / 1E095000 / 1E08D000 again between draws that may reuse a table; A/B for the Datacore "
                     "gate). 0 = tables reused for the whole frame (the old behaviour).");
REXCVAR_DEFINE_BOOL(pdz_fix_resolve_table_reuse, true, "Render",
                    "Native renderer fix (on): the game resolves into the same small textures several times a frame "
                    "(bloom blur, glow and luminance chains). A draw after such a resolve must not reuse a texture "
                    "table built before it: the reused table skips the barrier back from the resolve's render-target "
                    "state, and sampling a render target is undefined (NVIDIA usually shows the new picture, AMD and "
                    "the Steam Deck show compressed garbage: pink or blocky squares in the bloom). On = "
                    "pdz_native_split_tables 2 in every frame; off = the old rule (split screen only)");
REXCVAR_DEFINE_BOOL(pdz_debug_unbarriered_magenta, false, "Render",
                    "Debug (off): show on any GPU what AMD shows. A draw that would reuse a texture table while one of "
                    "its textures is still a render target (the hazard pdz_fix_resolve_table_reuse removes; turn the "
                    "fix off to see it) samples bright magenta for that texture instead of the undefined read, and the "
                    "draw is logged ('pdz_native: unbarriered read', each pixel shader and texture once).");
REXCVAR_DEFINE_STRING(pdz_debug_unbarriered_magenta_only, "", "Render",
                      "Debug: with pdz_debug_unbarriered_magenta, only these guest texture addresses (hex, comma "
                      "separated, e.g. 1C4EC000) are shown magenta; empty = all.");
REXCVAR_DEFINE_BOOL(pdz_native_split_geometry, true, "Render",
                    "Native renderer, split screen (stage1/subagents/split.md): in multi-pass frames (see "
                    "pdz_native_split_tables) pdz_native_geometry_hash_once holds for one pass segment instead of the "
                    "whole frame, so a vertex / index range player 1's pass checked is hashed again in player 2's and "
                    "new bytes in the same buffer are drawn, not player 1's copy (one more hash per range and pass). "
                    "Counted in 'pdz_native: split screen (periodic)'. Frames with one scene pass are unchanged.");

namespace pdz::native
{
  namespace reg = rex::graphics::reg;
  namespace xenos = rex::graphics::xenos;

  bool VolumeViewFor(const FetchDecoded &fetch)
  {
    constexpr uint32_t kFormatDXN = 49; // xenos::TextureFormat::k_DXN
    if (REXCVAR_GET(pdz_native_volume_view))
    {
      return true;
    }
    if (fetch.stacked ? REXCVAR_GET(pdz_native_volume_view_stacked) : REXCVAR_GET(pdz_native_volume_view_2d))
    {
      return true;
    }
    return (fetch.format & 63) == kFormatDXN && REXCVAR_GET(pdz_native_volume_view_dxn);
  }

  namespace
  {
    // 32 dwords = registers 0x4908..0x4927 = g_LoopConstants[i >> 2][i & 3]: loop index i = device +10144 + 4i, VS
    // i0..i15 = index 0..15 (+10144), PS i0..i15 = index 16..31 (+10208); the shaders' loop_id is that absolute
    // index (fork loopConst(16u) in 183 pixel shaders). Settled 2026-10-26 (shadow_assert.md 1.3): the flush emits
    // 0x4900..0x4927 as one run from +10112, SetVertexShaderConstantI / SetPixelShaderConstantI (sub_826E3788 /
    // sub_826E37D8) write +10144 + 4n / +10208 + 4n, and the Datacore shadow assert (logs 045 / 053) found every
    // emitted 0x4908+i equal to device+10144+4i (4,342,624 dwords, 134,665 non-zero).
    constexpr uint32_t kDevLoopConstants = shadow::dev::kLoopConstants;
    constexpr uint32_t kFloatCbBytes = 4096;      // float4 c[256]
    constexpr uint32_t kPsFloatCount = 224;       // PS c0..c223 = hardware 256..479

    constexpr uint32_t kGeometryChunkBytes = 32u << 20;
    constexpr uint32_t kGeometryMaxChunks = 8;    // 256 MB of static geometry at most
    constexpr uint32_t kGeometryKeepChunks = 2;   // pdz_native_geometry_shrink never goes below this many
    constexpr uint64_t kGeometryIdleFrames = 7200; // ... and releases chunks unused this long (2 min at 60 Hz)
    constexpr uint64_t kGeometryMaintainFrames = 600;
    constexpr uint32_t kFramesInFlight = 3;
    // pdz_native_sampler_evict (EvictSamplerTables): pass 1 frees tables unused this long; if it freed fewer descriptors
    // than kSamplerEvictTarget, pass 2 frees every table unused for kSamplerMinAge frames (BeginFrame of frame F waited
    // for frame F - 3, so a table last bound in frame F - 4 or earlier is read by no frame in flight).
    constexpr uint64_t kSamplerIdleFrames = 600;
    constexpr uint64_t kSamplerMinAge = kFramesInFlight + 1;
    constexpr uint32_t kSamplerEvictTarget = 256;
    constexpr uint32_t kMaxDrawVertices = 1u << 22; // sanity bound on a single draw's vertex range

    DrawStats g_stats;

    // -----------------------------------------------------------------------
    // Byte swapping as the Xenos vertex fetch does it: the fetch constant's endian field is applied to the
    // 32-bit words of the stream, then the format is unpacked from the little-endian dword (x in the low bits
    // for 16_16, 8_8_8_8, 2_10_10_10 ... exactly the DXGI layout the fork's input layout reads). So the swap
    // does not depend on the element formats, only on the stream's endian (PIPELINE_NOTES.md "vertex upload").
    // -----------------------------------------------------------------------
    inline uint32_t Load32Unaligned(const uint8_t *p)
    {
      uint32_t v;
      std::memcpy(&v, p, 4);
      return v;
    }
    inline void Store32Unaligned(uint8_t *p, uint32_t v) { std::memcpy(p, &v, 4); }

    void SwapCopy(uint8_t *dst, const uint8_t *src, uint32_t bytes, uint32_t endian)
    {
      const uint32_t words = bytes / 4;
      switch (xenos::Endian(endian & 3))
      {
      case xenos::Endian::k8in32:
        for (uint32_t i = 0; i < words; ++i)
        {
          Store32Unaligned(dst + 4 * i, __builtin_bswap32(Load32Unaligned(src + 4 * i)));
        }
        break;
      case xenos::Endian::k8in16:
        for (uint32_t i = 0; i < words; ++i)
        {
          const uint32_t v = Load32Unaligned(src + 4 * i);
          Store32Unaligned(dst + 4 * i, ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu));
        }
        break;
      case xenos::Endian::k16in32:
        for (uint32_t i = 0; i < words; ++i)
        {
          const uint32_t v = Load32Unaligned(src + 4 * i);
          Store32Unaligned(dst + 4 * i, (v << 16) | (v >> 16));
        }
        break;
      default:
        std::memcpy(dst, src, words * 4);
        break;
      }
      if (bytes & 3)
      {
        std::memcpy(dst + words * 4, src + words * 4, bytes & 3);
      }
    }

    // Big-endian float4 constants -> host dwords.
    void SwapConstants(uint8_t *dst, const uint8_t *src, uint32_t bytes) { SwapCopy(dst, src, bytes, 2); }

    // Content fingerprint of a guest range for the geometry cache: whole range up to 64 KB, else the head,
    // the tail and a 256-byte sample every 64 KB (cheap on multi-MB static buffers; a dynamic buffer rewritten
    // in place between frames with the same sampled bytes would be missed - see PIPELINE_NOTES.md).
    uint64_t SampleHash(const uint8_t *p, uint32_t bytes)
    {
      if (bytes <= 65536)
      {
        return XXH3_64bits(p, bytes);
      }
      XXH3_state_t st; // complete type with XXH_INLINE_ALL (rex/hash.h): no heap allocation
      XXH3_64bits_reset(&st);
      XXH3_64bits_update(&st, &bytes, sizeof(bytes));
      XXH3_64bits_update(&st, p, 4096);
      XXH3_64bits_update(&st, p + bytes - 4096, 4096);
      for (uint32_t off = 65536; off + 256 <= bytes; off += 65536)
      {
        XXH3_64bits_update(&st, p + off, 256);
      }
      return XXH3_64bits_digest(&st);
    }

    // -----------------------------------------------------------------------
    // Per-draw CPU cost (PDZRenderResearch native\stage1\subagents\perfopt.md). The cvars are defined here, inside
    // the namespace, to keep this change in one block. pdz_native_draw_timing splits IssueDraw into stages with
    // rdtsc (off by default; about ten rdtsc reads per draw when on). The geometry hash counters are always kept.
    // -----------------------------------------------------------------------
    REXCVAR_DEFINE_BOOL(pdz_native_geometry_hash_once, true, "Render",
                        "Native renderer: hash a cached vertex / index range at most once per frame; later draws of "
                        "the same range in the same frame reuse that check (REVIEW.md 12). Off = hash on every draw.");
    REXCVAR_DEFINE_BOOL(pdz_native_debug_geometry_verify, false, "Render",
                        "Native renderer diagnostic for pdz_native_geometry_hash_once: hash every draw's ranges anyway "
                        "and count the ranges whose content changed after their first check in the frame (those draws "
                        "then take the fresh copy, exactly as with the switch off). Logged in 'pdz_native: draw cpu'.");
    REXCVAR_DEFINE_BOOL(pdz_native_geometry_page_hash, false, "Render",
                        "Native renderer (overnight 30-31 Oct, experiment, off: no gain measured): a vertex / index range "
                        "is fingerprinted from per-page hashes kept for the frame, so draws whose ranges share pages hash "
                        "each page once. Outpost: 30 MB of ranges per frame cover 21 MB of pages, and the hash time was "
                        "the same (3.48 vs 3.50 ms). Off = every range hashed in full, as before");
    REXCVAR_DEFINE_BOOL(pdz_native_geometry_watch, true, "Render",
                        "Native renderer (overnight 30-31 Oct): a cached vertex / index range that stayed the same for "
                        "8 checks gets a write watch on its guest pages (the SDK's Xenia memory watches: a write by the "
                        "game, a file read, a protect / free all invalidate it); while nothing wrote it, its copy is used "
                        "without hashing guest memory again. A written range is hashed as before. Off = every range "
                        "hashed every frame");
    REXCVAR_DEFINE_BOOL(pdz_native_geometry_watch_verify, false, "Render",
                        "Test aid for pdz_native_geometry_watch: every range used without a hash is hashed anyway and "
                        "compared; mismatches are counted in the draw state line and the first 8 logged (expected 0)");
    REXCVAR_DEFINE_BOOL(pdz_native_draw_timing, false, "Render",
                        "Native renderer: CPU time of each draw stage (rdtsc), logged every pdz_native_log_frames "
                        "frames as 'pdz_native: draw cpu (periodic)'.");

    enum DrawStage : uint32_t
    {
      kStageShaders,   // IssueDraw entry (device, frame start, PSO cache ready) + the shader lookups
      kStageTargets,   // passdump check + BindTargets (Surfaces::GetBound lookups, transitions, OMSetRenderTargets)
      kStageDecode,    // DecodeRenderState + ResolveIndexReset (stage 2 M5 drawcost: split from pipeline)
      kStagePipeline,  // PipelineCache::Lookup (key build, XXH3, map) + DrawTopology
      kStageViewport,  // ComputeHostViewport + scissor clamp
      kStageGeometry,  // index + vertex uploads (the geometry hash time is also reported on its own)
      kStageCbFloat,   // float constant buffers (reuse check, swap copies; drawcost: split from constants)
      kStageConstants, // bool / loop and system constants
      kStageTables,    // the SRV descriptor table (with its texture lookups)
      kStageSamplers,  // the sampler descriptor tables + the 7e3 ROV table (drawcost: split from tables)
      kStageRecord,    // the D3D12 calls up to the draw
      kStageTail,      // stats, passdump, loop diagnostic, frame trace
      kStageSkipped,   // the rest of a draw that returned early (skipped)
      kStageCount
    };
    const char *const kStageNames[kStageCount] = {"shaders", "targets",   "decode", "pipeline", "viewport",
                                                  "geometry", "cbfloat",  "constants", "tables", "samplers",
                                                  "record",  "tail",      "skipped"};

    struct DrawPerf
    {
      uint64_t hash_calls = 0;     // SampleHash calls in UploadRange
      uint64_t hash_bytes = 0;     // bytes they read
      uint64_t hash_skips = 0;     // UploadRange calls answered by an earlier check of the same frame
      uint64_t verify_changes = 0; // pdz_native_debug_geometry_verify: ranges changed after that check
      bool on = false;             // pdz_native_draw_timing: the current draw is timed
      bool phases = false;         // pdz_frame_timing_phases 2: the current draw's stages go to the frame timing
      uint64_t last = 0;           // rdtsc at the end of the previous stage
      uint64_t draws = 0;          // timed IssueDraw calls
      uint64_t cycles[kStageCount] = {};
      uint64_t hash_cycles = 0;
      uint64_t union_bytes = 0;    // overnight 30-31 Oct diagnostic: distinct 4 KB pages those reads touched per frame
    };
    DrawPerf g_perf;

    // pdz_frame_timing_phases 2: each stage's phase, and the phase the draw continues as after it (the next stage;
    // the draw itself after the tail or an early return).
    constexpr pdz::timing::Phase kStagePhase[kStageCount] = {
        pdz::timing::Phase::kDrawShaders,  pdz::timing::Phase::kDrawTargets,   pdz::timing::Phase::kDrawDecode,
        pdz::timing::Phase::kDrawPipeline, pdz::timing::Phase::kDrawViewport,  pdz::timing::Phase::kDrawGeometry,
        pdz::timing::Phase::kDrawCbFloat,  pdz::timing::Phase::kDrawConstants, pdz::timing::Phase::kDrawTables,
        pdz::timing::Phase::kDrawSamplers, pdz::timing::Phase::kDrawRecord,    pdz::timing::Phase::kDrawTail,
        pdz::timing::Phase::kDrawSkipped};
    constexpr pdz::timing::Phase kStageNextPhase[kStageCount] = {
        pdz::timing::Phase::kDrawTargets,   pdz::timing::Phase::kDrawDecode,    pdz::timing::Phase::kDrawPipeline,
        pdz::timing::Phase::kDrawViewport,  pdz::timing::Phase::kDrawGeometry,  pdz::timing::Phase::kDrawCbFloat,
        pdz::timing::Phase::kDrawConstants, pdz::timing::Phase::kDrawTables,    pdz::timing::Phase::kDrawSamplers,
        pdz::timing::Phase::kDrawRecord,    pdz::timing::Phase::kDrawTail,      pdz::timing::Phase::kDraw,
        pdz::timing::Phase::kDraw};

    // Ends the current stage of a timed draw.
    inline void Lap(DrawStage stage)
    {
      if (g_perf.on)
      {
        const uint64_t now = __rdtsc();
        g_perf.cycles[stage] += now - g_perf.last;
        g_perf.last = now;
      }
      if (g_perf.phases)
      {
        pdz::timing::Lap(kStagePhase[stage], kStageNextPhase[stage]);
      }
    }

    // pdz_native_geometry_page_hash (overnight 30-31 Oct, log 319's Outpost: the geometry hash was 3.7 of the record
    // thread's 11.3 ms): a range up to 64 KB is fingerprinted as its partial head bytes, the hash of every whole 4 KB page
    // it covers and its partial tail bytes, and each page's hash is kept for the current check window (the frame, or one
    // pass segment in split screen: g_split.geometry_stamp, the window pdz_native_geometry_hash_once already trusts a
    // range for). Ranges of the same window that share pages (the many draws of one big vertex buffer) then read each
    // page once. Only bytes inside the range are read, as before; a fingerprint of other bytes never results.
    struct PageMemoEntry
    {
      uintptr_t page = 0;
      uint64_t stamp = ~0ull;
      uint64_t hash = 0;
    };
    PageMemoEntry g_page_memo[1u << 16];

    uint64_t PageHash(uintptr_t page, uint64_t stamp)
    {
      PageMemoEntry &e = g_page_memo[uint32_t((uint64_t(page) * 0x9E3779B97F4A7C15ull) >> 48)];
      if (e.page != page || e.stamp != stamp)
      {
        e.page = page;
        e.stamp = stamp;
        e.hash = XXH3_64bits(reinterpret_cast<const void *>(page << 12), 4096);
      }
      return e.hash;
    }

    uint64_t PagedHash(const uint8_t *p, uint32_t bytes, uint64_t stamp)
    {
      const uintptr_t a = reinterpret_cast<uintptr_t>(p), e = a + bytes;
      const uintptr_t first_full = (a + 4095) & ~uintptr_t(4095), last_full = e & ~uintptr_t(4095);
      if (first_full >= last_full)
      {
        return XXH3_64bits(p, bytes); // no whole page: as before
      }
      XXH3_state_t st;
      XXH3_64bits_reset(&st);
      XXH3_64bits_update(&st, &bytes, sizeof(bytes));
      XXH3_64bits_update(&st, p, size_t(first_full - a));
      for (uintptr_t page = first_full >> 12; page < (last_full >> 12); ++page)
      {
        const uint64_t h = PageHash(page, stamp);
        XXH3_64bits_update(&st, &h, sizeof(h));
      }
      XXH3_64bits_update(&st, reinterpret_cast<const void *>(last_full), size_t(e - last_full));
      return XXH3_64bits_digest(&st);
    }

    uint64_t RangeHash(const uint8_t *p, uint32_t bytes, uint64_t stamp)
    {
      return bytes <= 65536 && bytes >= 4096 && REXCVAR_GET(pdz_native_geometry_page_hash) ? PagedHash(p, bytes, stamp)
                                                                                            : SampleHash(p, bytes);
    }

    // SampleHash plus its counters (bytes read: the whole range up to 64 KB, else the length, head, tail, samples).
    uint64_t CountedSampleHash(const uint8_t *p, uint32_t bytes, uint64_t stamp)
    {
      ++g_perf.hash_calls;
      g_perf.hash_bytes += bytes <= 65536 ? bytes
                                          : 8196u + (bytes >= 65536u + 256u ? ((bytes - 65536u - 256u) / 65536u + 1u) * 256u : 0u);
      const pdz::timing::PhaseScope phase(pdz::timing::Phase::kGeometryHash, 2); // pdz_frame_timing_phases 2
      if (g_perf.on && bytes <= 65536)
      {
        // How much the frame's ranges overlap: the distinct pages of the fully hashed ranges (diagnostic only).
        static std::unordered_set<uintptr_t> pages;
        static uint64_t pages_frame = ~0ull;
        if (pages_frame != stamp)
        {
          pages_frame = stamp;
          pages.clear();
        }
        const uintptr_t first = reinterpret_cast<uintptr_t>(p) >> 12, last = (reinterpret_cast<uintptr_t>(p) + bytes - 1) >> 12;
        for (uintptr_t page = first; bytes && page <= last; ++page)
        {
          if (pages.insert(page).second)
          {
            g_perf.union_bytes += 4096;
          }
        }
      }
      if (!g_perf.on)
      {
        return RangeHash(p, bytes, stamp);
      }
      const uint64_t t0 = __rdtsc();
      const uint64_t h = RangeHash(p, bytes, stamp);
      g_perf.hash_cycles += __rdtsc() - t0;
      return h;
    }

    // 'pdz_native: draw cpu (periodic)': the window since the previous line (pdz_native_log_frames frames): ms per
    // frame and, in brackets, ns per timed draw for each stage. The rdtsc rate is measured against steady_clock over
    // the window. Printed only with pdz_native_draw_timing or pdz_native_debug_geometry_verify.
    void LogDrawPerf(uint64_t frame)
    {
      const bool timing = REXCVAR_GET(pdz_native_draw_timing);
      if (!timing && !REXCVAR_GET(pdz_native_debug_geometry_verify))
      {
        return;
      }
      static DrawPerf prev;
      static uint64_t prev_frame = 0, prev_tsc = 0;
      static std::chrono::steady_clock::time_point prev_time;
      const uint64_t tsc = __rdtsc();
      const auto now = std::chrono::steady_clock::now();
      if (prev_tsc != 0 && frame > prev_frame)
      {
        const double seconds = std::chrono::duration<double>(now - prev_time).count();
        const double tsc_per_ms = seconds > 0.0 ? double(tsc - prev_tsc) / (seconds * 1000.0) : 0.0;
        const double frames = double(frame - prev_frame);
        const uint64_t draws = g_perf.draws - prev.draws;
        const auto ms_per_frame = [&](uint64_t c) { return tsc_per_ms > 0.0 ? double(c) / tsc_per_ms / frames : 0.0; };
        const auto ns_per_draw = [&](uint64_t c) {
          return tsc_per_ms > 0.0 && draws ? double(c) / tsc_per_ms * 1.0e6 / double(draws) : 0.0;
        };
        std::string stages;
        uint64_t total = 0;
        for (uint32_t i = 0; i < kStageCount; ++i)
        {
          const uint64_t c = g_perf.cycles[i] - prev.cycles[i];
          total += c;
          stages += fmt::format(" {} {:.3f} ({:.0f})", kStageNames[i], ms_per_frame(c), ns_per_draw(c));
        }
        const uint64_t hash_c = g_perf.hash_cycles - prev.hash_cycles;
        PDZ_LOG_DEFERRED("pdz_native: draw cpu (periodic): frames {}-{} | timed draws {} ({:.0f}/frame) | ms per frame (ns per "
                    "draw):{} | total {:.3f} ({:.0f}) | geometry hash {:.3f} ({:.0f}): {:.0f} calls {:.0f} KB per frame, "
                    "{:.0f} checks skipped per frame (hash once {}), in-frame changes {} (verify {}) | distinct pages of the "
                    "fully hashed ranges {:.0f} KB per frame | rdtsc {:.0f} MHz",
                    prev_frame, frame, draws, double(draws) / frames, stages, ms_per_frame(total), ns_per_draw(total),
                    ms_per_frame(hash_c), ns_per_draw(hash_c), double(g_perf.hash_calls - prev.hash_calls) / frames,
                    double(g_perf.hash_bytes - prev.hash_bytes) / 1024.0 / frames,
                    double(g_perf.hash_skips - prev.hash_skips) / frames, REXCVAR_GET(pdz_native_geometry_hash_once),
                    g_perf.verify_changes - prev.verify_changes, REXCVAR_GET(pdz_native_debug_geometry_verify),
                    double(g_perf.union_bytes - prev.union_bytes) / 1024.0 / frames, tsc_per_ms / 1000.0);
      }
      prev = g_perf;
      prev_frame = frame;
      prev_tsc = tsc;
      prev_time = now;
    }

    // -----------------------------------------------------------------------
    // Split screen (native_draw.h OnResolveInto; stage1/subagents/split.md). Each player's view is a whole scene pass
    // (shadow cascades, G-buffer, light, HDR, bloom) that resolves into the same textures again, with its own camera
    // constants. Per-frame caches that assumed one pass:
    // - the SRV tables (FrameTable srv_tables, keyed by shaders + fetch constants): a draw of the second pass found the
    //   first pass's table and skipped Textures::Lookup, so the transition back from the resolve's render-target state
    //   and the alias view refresh never happened (pdz_native_split_tables: the key is salted with the table epoch);
    // - pdz_native_geometry_hash_once: a range checked in the first pass was not hashed again in the second
    //   (pdz_native_split_geometry: the check stamp is per pass segment).
    // A pass segment ends at a resolve into a mirror that a descriptor table of the segment samples (HostTexture::
    // table_segment == segment): nothing built before it may be reused after it. A new frame starts a new one too.
    // -----------------------------------------------------------------------
    constexpr uint64_t kSplitHoldFrames = 120; // a multi-pass frame keeps split screen on this long (menus, pauses)

    struct SplitState
    {
      uint64_t segment = 1;        // current pass segment (never 0: HostTexture::table_segment starts at 0)
      uint64_t table_epoch = 1;    // salt of the SRV table keys (the segment that started the table epoch)
      uint64_t geometry_stamp = 1; // pdz_native_geometry_hash_once window (the segment that started it)
      // Scene passes of the device frame `frame`: the most depth resolves into one mirror (OnResolveInto).
      uint64_t frame = ~0ull;
      uint32_t scene_passes = 1;
      bool multi_seen = false;     // a multi-pass frame was seen (last_multi_frame valid)
      uint64_t last_multi_frame = 0;
      bool announced = false;      // the "split screen" line was logged and the "over" line not yet
      uint32_t announce_logged = 0;
      // Counters for 'pdz_native: split screen (periodic)' (never reset).
      uint64_t multi_frames = 0;     // frames with 2+ scene passes
      uint64_t boundaries = 0;       // segments started by a resolve (or by a detection)
      uint64_t table_epochs = 0;     // ... that started a table epoch (pdz_native_split_tables)
      uint64_t geometry_windows = 0; // ... that started a geometry check window (pdz_native_split_geometry)
      uint32_t max_passes_window = 0;
    };
    SplitState g_split;

    bool SplitActiveAt(uint64_t frame)
    {
      return g_split.multi_seen && frame <= g_split.last_multi_frame + kSplitHoldFrames;
    }

    // Whether a resolve into a sampled mirror ends the pass segment in `frame` (some per-pass cache acts).
    // pdz_native_split_tables, raised to 2 by pdz_fix_resolve_table_reuse (0 = explicitly off stays off).
    int32_t TablesMode()
    {
      const int32_t tables = REXCVAR_GET(pdz_native_split_tables);
      return tables != 0 && REXCVAR_GET(pdz_fix_resolve_table_reuse) ? std::max(tables, 2) : tables;
    }

    bool SegmentsActive(uint64_t frame)
    {
      const int32_t tables = TablesMode();
      if (tables >= 2)
      {
        return true;
      }
      return SplitActiveAt(frame) && (tables == 1 || REXCVAR_GET(pdz_native_split_geometry));
    }

    void StartSegment(uint64_t frame)
    {
      ++g_split.segment;
      ++g_split.boundaries;
      const int32_t tables = TablesMode();
      const bool split = SplitActiveAt(frame);
      if (tables >= 2 || (tables == 1 && split))
      {
        g_split.table_epoch = g_split.segment;
        ++g_split.table_epochs;
      }
      if (split && REXCVAR_GET(pdz_native_split_geometry))
      {
        g_split.geometry_stamp = g_split.segment;
        ++g_split.geometry_windows;
      }
    }

    // A texture an SRV table of the current segment binds (BindSrvTable, table built): a later resolve into it ends the
    // segment. An alias view stamps its source mirror, the texture resolves write.
    inline void StampTable(HostTexture &t)
    {
      t.table_segment = g_split.segment;
      if (t.view_source)
      {
        t.view_source->table_segment = g_split.segment;
      }
    }

    // -----------------------------------------------------------------------
    // Geometry cache: persistent upload-heap chunks, linear suballocation, whole-chunk recycling once every
    // frame that used it has retired.
    // -----------------------------------------------------------------------
    struct GeometryEntry
    {
      D3D12_GPU_VIRTUAL_ADDRESS gpu = 0;
      uint32_t chunk = 0;
      uint32_t bytes = 0;
      uint64_t content_hash = 0;
      uint64_t last_frame = 0;
      uint32_t min_index = 0; // index buffers: the swapped indices' range
      uint32_t max_index = 0;
      // Check window whose draw last hashed this range (pdz_native_geometry_hash_once): SplitState::geometry_stamp, one
      // per frame, one per pass segment in split screen (pdz_native_split_geometry).
      uint64_t checked_stamp = ~uint64_t(0);
      // pdz_native_geometry_watch: armed = the range's pages are write-watched and nothing has written them since
      // (their generation sum is still armed_gen); stable = consecutive checks that found the same bytes.
      bool armed = false;
      uint8_t invalidations = 0;
      uint16_t stable = 0;
      uint64_t armed_gen = 0;
    };

    class GeometryCache
    {
    public:
      // `frame` stamps the entry and its chunk (recycling), `stamp` the check window (FindChecked).
      GeometryEntry *Find(uint64_t key, uint64_t content_hash, uint64_t frame, uint64_t stamp, uint32_t &chunk_touched)
      {
        auto it = entries_.find(key);
        if (it == entries_.end())
        {
          return nullptr;
        }
        GeometryEntry &e = it->second;
        if (e.content_hash != content_hash)
        {
          // Stale copy: the space stays allocated until the chunk is recycled; a new copy is made. Drawn earlier in this
          // frame with other bytes: rewritten between two passes (split screen) or two draws (hash once off / verify).
          in_frame_changes_ += e.last_frame == frame ? 1 : 0;
          entries_.erase(it);
          return nullptr;
        }
        e.last_frame = frame;
        e.checked_stamp = stamp;
        chunks_[e.chunk].last_frame = frame;
        chunk_touched = e.chunk;
        return &e;
      }

      // pdz_native_geometry_watch: the entry for `key` (any content), and a hit's bookkeeping without a hash.
      GeometryEntry *FindKey(uint64_t key)
      {
        auto it = entries_.find(key);
        return it != entries_.end() ? &it->second : nullptr;
      }
      void Touch(GeometryEntry &e, uint64_t frame, uint64_t stamp)
      {
        e.last_frame = frame;
        e.checked_stamp = stamp;
        chunks_[e.chunk].last_frame = frame;
      }

      // The entry for `key` if a draw of check window `stamp` already hashed it (Find matched or Insert copied it): its
      // last_frame and its chunk's are this frame already (a window never spans frames), so nothing else changes.
      GeometryEntry *FindChecked(uint64_t key, uint64_t stamp)
      {
        auto it = entries_.find(key);
        return it != entries_.end() && it->second.checked_stamp == stamp ? &it->second : nullptr;
      }

      // Ranges found with other bytes than their copy made earlier in the same frame (Find).
      uint64_t in_frame_changes() const { return in_frame_changes_; }

      // Allocates `bytes` for `key`; returns the CPU pointer to fill, or nullptr when every chunk is still in
      // flight (the caller falls back to the frame upload).
      uint8_t *Insert(uint64_t key, uint32_t bytes, uint64_t content_hash, uint64_t frame, uint64_t stamp,
                      GeometryEntry **out)
      {
        if (bytes > kGeometryChunkBytes)
        {
          return nullptr;
        }
        const uint32_t aligned = (bytes + 15) & ~15u;
        int chunk = -1;
        for (size_t i = 0; i < chunks_.size(); ++i)
        {
          // A released slot (pdz_native_geometry_shrink) has size 0 and never fits.
          if (chunks_[i].used + aligned <= chunks_[i].size)
          {
            chunk = int(i);
            break;
          }
        }
        if (chunk < 0 && chunk_count() < kGeometryMaxChunks)
        {
          chunk = AddChunk();
        }
        if (chunk < 0)
        {
          chunk = Recycle(frame);
        }
        if (chunk < 0)
        {
          return nullptr;
        }
        Chunk &c = chunks_[size_t(chunk)];
        GeometryEntry e;
        e.gpu = c.gpu + c.used;
        e.chunk = uint32_t(chunk);
        e.bytes = bytes;
        e.content_hash = content_hash;
        e.last_frame = frame;
        e.checked_stamp = stamp;
        uint8_t *cpu = c.cpu + c.used;
        c.used += aligned;
        c.last_frame = frame;
        auto res = entries_.insert_or_assign(key, e);
        *out = &res.first->second;
        return cpu;
      }

      size_t entry_count() const { return entries_.size(); }
      // Live chunks (a slot released by pdz_native_geometry_shrink stays in the vector, empty, and is reused first).
      size_t chunk_count() const
      {
        size_t live = 0;
        for (const Chunk &c : chunks_)
        {
          live += c.resource ? 1 : 0;
        }
        return live;
      }
      uint64_t chunk_bytes() const
      {
        uint64_t total = 0;
        for (const Chunk &c : chunks_)
        {
          total += c.size;
        }
        return total;
      }
      uint64_t used_bytes() const
      {
        uint64_t total = 0;
        for (const Chunk &c : chunks_)
        {
          total += c.used;
        }
        return total;
      }
      uint32_t recycles() const { return recycles_; }
      uint64_t released() const { return released_; }

      // pdz_native_geometry_shrink, from NewFrame (no draw is in progress, so no GeometryEntry pointer is held): every
      // kGeometryMaintainFrames, releases the least recently used chunks no draw used for kGeometryIdleFrames while more
      // than kGeometryKeepChunks are live. Their entries go first, so no cached range points into a released chunk; the
      // chunk was not read by any frame in flight for thousands of frames, and the resource still goes through
      // ReleaseLater.
      void Maintain(uint64_t frame)
      {
        if (frame - last_maintenance_ < kGeometryMaintainFrames || !REXCVAR_GET(pdz_native_geometry_shrink))
        {
          return;
        }
        last_maintenance_ = frame;
        NativeDevice *dev = NativeDevice::Existing();
        if (!dev)
        {
          return;
        }
        size_t live = chunk_count();
        uint32_t released = 0;
        size_t entries_before = entries_.size();
        while (live > kGeometryKeepChunks)
        {
          int oldest = -1;
          for (size_t i = 0; i < chunks_.size(); ++i)
          {
            const Chunk &c = chunks_[i];
            if (c.resource && c.last_frame + kGeometryIdleFrames <= frame &&
                (oldest < 0 || c.last_frame < chunks_[size_t(oldest)].last_frame))
            {
              oldest = int(i);
            }
          }
          if (oldest < 0)
          {
            break;
          }
          DropEntries(uint32_t(oldest));
          Chunk &c = chunks_[size_t(oldest)];
          c.resource->Unmap(0, nullptr);
          dev->ReleaseLater(c.resource);
          c = Chunk(); // an empty slot: never fits, reused by the next AddChunk
          --live;
          ++released;
        }
        if (released)
        {
          released_ += released;
          if (released_logged_ < 16)
          {
            ++released_logged_;
            REXLOG_INFO("pdz_native: geometry cache released {} chunk(s) of 32 MB unused for {}+ frames ({} cached ranges "
                        "dropped; {} chunks live, {} released in total; pdz_native_geometry_shrink)",
                        released, kGeometryIdleFrames, entries_before - entries_.size(), live, released_);
          }
        }
      }

    private:
      struct Chunk
      {
        ID3D12Resource *resource = nullptr; // nullptr = a released slot (pdz_native_geometry_shrink)
        uint8_t *cpu = nullptr;
        D3D12_GPU_VIRTUAL_ADDRESS gpu = 0;
        uint32_t size = 0;
        uint32_t used = 0;
        uint64_t last_frame = 0;
      };

      // A new chunk in the first released slot, else at the end; returns its index or -1.
      int AddChunk()
      {
        NativeDevice *dev = NativeDevice::Get();
        if (!dev)
        {
          return -1;
        }
        Chunk c;
        c.resource = dev->CreateBuffer(kGeometryChunkBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ,
                                       D3D12_RESOURCE_FLAG_NONE, "pdz_native geometry chunk");
        if (!c.resource)
        {
          return -1;
        }
        void *mapped = nullptr;
        D3D12_RANGE no_read = {0, 0};
        if (FAILED(c.resource->Map(0, &no_read, &mapped)))
        {
          c.resource->Release();
          return -1;
        }
        c.cpu = static_cast<uint8_t *>(mapped);
        c.gpu = c.resource->GetGPUVirtualAddress();
        c.size = kGeometryChunkBytes;
        size_t slot = chunks_.size();
        for (size_t i = 0; i < chunks_.size(); ++i)
        {
          if (!chunks_[i].resource)
          {
            slot = i;
            break;
          }
        }
        if (slot == chunks_.size())
        {
          chunks_.push_back(c);
        }
        else
        {
          chunks_[slot] = c;
        }
        if (entries_.bucket_count() < 4096)
        {
          entries_.reserve(4096);
        }
        return int(slot);
      }

      // Forgets every cached range in `chunk` (before its space is reused or released).
      void DropEntries(uint32_t chunk)
      {
        for (auto it = entries_.begin(); it != entries_.end();)
        {
          if (it->second.chunk == chunk)
          {
            it = entries_.erase(it);
          }
          else
          {
            ++it;
          }
        }
      }

      // The least recently used chunk, if the GPU is done with it.
      int Recycle(uint64_t frame)
      {
        int best = -1;
        for (size_t i = 0; i < chunks_.size(); ++i)
        {
          if (!chunks_[i].resource || chunks_[i].last_frame + kFramesInFlight + 1 > frame)
          {
            continue;
          }
          if (best < 0 || chunks_[i].last_frame < chunks_[size_t(best)].last_frame)
          {
            best = int(i);
          }
        }
        if (best < 0)
        {
          return -1;
        }
        DropEntries(uint32_t(best));
        chunks_[size_t(best)].used = 0;
        ++recycles_;
        return best;
      }

      std::vector<Chunk> chunks_;
      std::unordered_map<uint64_t, GeometryEntry> entries_;
      uint32_t recycles_ = 0;
      uint64_t released_ = 0;
      uint32_t released_logged_ = 0;
      uint64_t last_maintenance_ = 0;
      uint64_t in_frame_changes_ = 0;
    };

    GeometryCache g_geometry;

    // -----------------------------------------------------------------------
    // Per-frame state
    // -----------------------------------------------------------------------
    // Per-frame key -> GPU descriptor handle table (open addressing, linear probing, cleared per frame):
    // no allocation per new key, unlike a node-based map. Key 0 is the empty marker. At 7/8 load the table is
    // emptied and refilled (review 15: it used to stop inserting at 3,584 keys, so every later draw of a heavy
    // frame built a fresh table and could run the SRV ring dry). The tables it pointed to stay valid for the frame.
    template <uint32_t kSlots>
    struct FrameTable
    {
      static constexpr uint32_t kSize = kSlots; // power of two, > distinct (shader, textures) combinations per frame
      static_assert((kSize & (kSize - 1)) == 0, "FrameTable size must be a power of two");
      uint64_t keys[kSize];
      D3D12_GPU_DESCRIPTOR_HANDLE values[kSize];
      uint32_t count = 0;
      uint64_t recycles = 0; // times the table was emptied when nearly full (never reset; first one logged)
      void Clear()
      {
        std::memset(keys, 0, sizeof(keys));
        count = 0;
      }
      bool Find(uint64_t key, D3D12_GPU_DESCRIPTOR_HANDLE &out) const
      {
        uint32_t i = uint32_t(key ^ (key >> 29)) & (kSize - 1);
        for (uint32_t probe = 0; probe < kSize; ++probe)
        {
          if (keys[i] == key)
          {
            out = values[i];
            return true;
          }
          if (keys[i] == 0)
          {
            return false;
          }
          i = (i + 1) & (kSize - 1);
        }
        return false;
      }
      void Insert(uint64_t key, D3D12_GPU_DESCRIPTOR_HANDLE value)
      {
        if (count >= kSize - kSize / 8)
        {
          // Nearly full: forget every key and start over (only the reuse of earlier tables is lost).
          if (recycles++ == 0)
          {
            REXLOG_INFO("pdz_native: per-frame descriptor table map ({} slots) full at {} keys; recycled from now on",
                        kSize, count);
          }
          Clear();
        }
        uint32_t i = uint32_t(key ^ (key >> 29)) & (kSize - 1);
        while (keys[i] != 0 && keys[i] != key)
        {
          i = (i + 1) & (kSize - 1);
        }
        if (keys[i] == 0)
        {
          ++count;
        }
        keys[i] = key;
        values[i] = value;
      }
    };

    struct FrameState
    {
      uint64_t frame = ~uint64_t(0);
      // root_signature_set / last_pso describe the command list as of list_epoch; a pass binding its own
      // pipeline (resolve blit, gamma) or a Flush changes the epoch and both are re-bound (log 022 crash: a
      // draw after a resolve in the same frame bound its tables against the blit's 3-parameter root signature).
      uint64_t list_epoch = ~uint64_t(0);
      bool root_signature_set = false;
      ID3D12PipelineState *last_pso = nullptr;
      // pdz_native_state_filter (stage 2 M5 drawcost3): the rest of what the draw path last set on the list, valid for
      // list_epoch like the two above (cleared with them: a pass with its own pipeline, a Flush or a new frame bumps the
      // epoch). Always kept up to date; the filter only decides whether an equal value is set again.
      struct ListState
      {
        bool topology_valid = false, viewport_valid = false, scissor_valid = false, stencil_valid = false,
             blend_valid = false, vb_valid = false, ib_valid = false;
        D3D_PRIMITIVE_TOPOLOGY topology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
        D3D12_VIEWPORT viewport{};
        D3D12_RECT scissor{};
        UINT stencil_ref = 0;
        float blend_factor[4] = {};
        D3D12_GPU_VIRTUAL_ADDRESS cbv[4] = {}; // kRootCbvFloatVs .. kRootCbvSystem (0 = unknown)
        UINT64 table[3] = {};                  // kRootSrvTable, kRootSamplerVs, kRootSamplerPs (0 = unknown)
        uint32_t vb_slots = 0;
        D3D12_VERTEX_BUFFER_VIEW vbv[16] = {};
        D3D12_INDEX_BUFFER_VIEW ibv{};
      } ls;
      // Float constant buffer reuse (valid while no ALU dirty bit was set since the upload).
      D3D12_GPU_VIRTUAL_ADDRESS vs_cb = 0, ps_cb = 0;
      const NativeShader *vs_cb_shader = nullptr, *ps_cb_shader = nullptr;
      // CPU view of the same uploads (frame upload heap; read only by the draw dump, pdz_native_draw_dump).
      const uint8_t *vs_cb_cpu = nullptr, *ps_cb_cpu = nullptr;
      // pdz_native_cb_dedupe (drawcost4): the last bool / loop and system constants uploaded in this device frame and
      // where (0 = none yet this frame).
      BoolLoopConstants last_bool_loop{};
      SystemConstants last_system{};
      D3D12_GPU_VIRTUAL_ADDRESS last_bool_loop_gpu = 0, last_system_gpu = 0;
      // Descriptor tables reused within the frame.
      FrameTable<16384> srv_tables; // 14,336 keys before a recycle (256 KB)
      FrameTable<4096> sampler_tables;
      D3D12_GPU_DESCRIPTOR_HANDLE null_samplers{};
      // Deferred BeginVertices draw: the ring record (static storage in shadow_record.cpp; its a[3] is
      // patched with the returned data pointer after the hook forwards the call).
      const shadow::DrawRecord *begin_record = nullptr;
      // Set when a draw of this frame was skipped for a missing shader: the presenter then shows the
      // emulated frame instead of a partial native one (native_present.cpp).
      bool shader_miss = false;
      // One-time diagnostics.
      bool logged_stream_type = false;
      bool logged_ib_flag = false;
      bool logged_endian[4] = {};
    };
    FrameState g_frame;

    // Device bytes 0 .. 10271 (VS / PS float constants at +1920 / +6016, bool + loop constants at +10112..+10271)
    // as they were when the pending BeginVertices returned: D3D emitted the batch's constants in that call's
    // flush, and the draw is issued later (FlushPendingBegin), when the game may already have set the next ones.
    constexpr uint32_t kBeginDeviceBytes = 10272;
    // Return address of the BeginVertices call inside D3DDevice_DrawVerticesUP (sub_826E9A60: bl 826E97B0 at
    // 826E9AAC, then memcpy 82AA8CC0 and the inlined EndVertices store device[+0] = device[+13636]).
    constexpr uint32_t kDrawVerticesUpBeginReturn = 0x826E9AB0u;
    alignas(16) uint8_t g_begin_device[kBeginDeviceBytes];
    // pdz_native_record_thread: the pending BeginVertices record (its shadow ring slot is reused after the command).
    shadow::DrawRecord g_begin_copy;

    // Frame-trace queries (ResolveTraceQueries).
    struct TraceQueries
    {
      static constexpr uint32_t kMax = 4096;
      ID3D12QueryHeap *stats = nullptr;
      ID3D12QueryHeap *occlusion = nullptr;
      ID3D12Resource *readback = nullptr;
      bool init_failed = false;
      uint32_t count = 0;
      uint64_t frame = ~0ull;
      uint32_t sequence[kMax] = {};
    };
    TraceQueries g_tq;

    bool InitTraceQueries(NativeDevice &dev)
    {
      if (g_tq.stats || g_tq.init_failed)
      {
        return g_tq.stats != nullptr;
      }
      D3D12_QUERY_HEAP_DESC desc{};
      desc.Count = TraceQueries::kMax;
      desc.Type = D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS;
      HRESULT hr = dev.device()->CreateQueryHeap(&desc, IID_PPV_ARGS(&g_tq.stats));
      if (SUCCEEDED(hr))
      {
        desc.Type = D3D12_QUERY_HEAP_TYPE_OCCLUSION;
        hr = dev.device()->CreateQueryHeap(&desc, IID_PPV_ARGS(&g_tq.occlusion));
      }
      const uint64_t bytes =
          uint64_t(TraceQueries::kMax) * (sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS) + sizeof(uint64_t));
      if (SUCCEEDED(hr))
      {
        g_tq.readback = dev.CreateBuffer(bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST,
                                         D3D12_RESOURCE_FLAG_NONE, "pdz_native trace query readback");
      }
      if (FAILED(hr) || !g_tq.readback)
      {
        g_tq.init_failed = true;
        REXLOG_WARN("pdz_native: trace queries unavailable (0x{:08X})", uint32_t(hr));
        return false;
      }
      return true;
    }

    const char *const kGeometrySkipNames[uint32_t(GeometrySkip::kCount)] = {
        "draw-args", "begin-vertices-data", "no-index-buffer", "index-memory", "index-upload", "index-range",
        "expand-indices", "up-stream-not-0", "stream-no-buffer", "stream-offset-past-end", "stream-stride-or-memory",
        "range-past-buffer", "vertex-upload", "empty-viewport-or-scissor", "past-physical-memory"};

    const char *DrawKindName(shadow::Kind k)
    {
      switch (k)
      {
      case shadow::Kind::kDrawIndexedVertices: return "DrawIndexedVertices";
      case shadow::Kind::kDrawVertices: return "DrawVertices";
      case shadow::Kind::kDrawVerticesUP: return "DrawVerticesUP";
      case shadow::Kind::kBeginVertices: return "BeginVertices";
      case shadow::Kind::kDrawIndexedVerticesUP: return "DrawIndexedVerticesUP";
      default: return "other";
      }
    }

    // Counts a geometry skip; the first three of each reason are logged with the draw and a detail line.
    void SkipGeometry(GeometrySkip reason, const shadow::DrawRecord &r, const char *detail_format, ...)
    {
      ++g_stats.skipped_geometry;
      const uint64_t n = g_stats.geometry_reasons[uint32_t(reason)]++;
      if (n < 3)
      {
        char detail[256];
        va_list args;
        va_start(args, detail_format);
        std::vsnprintf(detail, sizeof(detail), detail_format, args);
        va_end(args);
        REXLOG_INFO("pdz_native: geometry skip {} (example {}): {} draw #{} f{} caller {:08X} prim {} args {:08X} "
                    "{:08X} {:08X} {:08X}: {}",
                    kGeometrySkipNames[uint32_t(reason)], n + 1, DrawKindName(r.kind), r.sequence, r.frame,
                    r.caller, r.a[0], r.a[0], r.a[1], r.a[2], r.a[3], detail);
      }
    }

    // 'pdz_native: split screen (periodic)': the window since the previous line, printed only when it had a multi-pass
    // frame (single-view logs are unchanged).
    void LogSplit(uint64_t frame)
    {
      static SplitState prev;
      static uint64_t prev_frame = 0, prev_changes = 0;
      const uint64_t changes = g_geometry.in_frame_changes();
      if (g_split.multi_frames != prev.multi_frames)
      {
        const double frames = frame > prev_frame ? double(frame - prev_frame) : 1.0;
        PDZ_LOG_DEFERRED("pdz_native: split screen (periodic): frames {}-{}: multi-pass frames {} (up to {} scene passes) | "
                    "pass segments started by a resolve {:.1f} per frame (table epochs {:.1f}, geometry windows {:.1f}) | "
                    "geometry ranges drawn earlier in the frame with other bytes {} | pdz_native_split_tables {} "
                    "pdz_native_split_geometry {}",
                    prev_frame, frame, g_split.multi_frames - prev.multi_frames, g_split.max_passes_window,
                    double(g_split.boundaries - prev.boundaries) / frames,
                    double(g_split.table_epochs - prev.table_epochs) / frames,
                    double(g_split.geometry_windows - prev.geometry_windows) / frames, changes - prev_changes,
                    REXCVAR_GET(pdz_native_split_tables), REXCVAR_GET(pdz_native_split_geometry));
      }
      prev = g_split;
      prev_frame = frame;
      prev_changes = changes;
      g_split.max_passes_window = 0;
    }

    // Overnight 30-31 Oct (docs/EXECUTE_INDIRECT.md): what changes between consecutive draws on the list, and the runs
    // an ExecuteIndirect path could batch. A run breaks (a) today: pipeline, topology, viewport / scissor, stencil ref,
    // blend factor, root constants of the 20e4 / 7e3 variants, any descriptor table; (b) with bindless textures and
    // samplers (tables as indices in root constants): the same minus the tables. Root CBVs, vertex / index buffer views
    // and the draw arguments can change inside a run (a command signature holds them).
    struct EiStats
    {
      uint64_t draws = 0, pso = 0, topology = 0, viewport = 0, stencil = 0, blend = 0, root_constants = 0;
      uint64_t srv_table = 0, sampler_table = 0, cbv_vs = 0, cbv_ps = 0, cbv_other = 0, vb = 0, ib = 0;
      uint64_t runs_now = 0, runs_bindless = 0, frames = 0;
    } g_ei;

    void LogWatchStats(uint64_t frames); // pdz_native_geometry_watch (defined after the watch code)
    void ShortStacksNewFrame(uint64_t frame); // pdz_fix_short_stacks / pdz_stack_layer_log (after WaterLoopMask)

    void LogEiStats()
    {
      EiStats &e = g_ei;
      if (!e.draws || !e.frames)
      {
        return;
      }
      const double f = double(e.frames), d = double(e.draws);
      REXLOG_INFO("pdz_native: draw state changes per frame ({} frames): draws {:.0f} | pipeline {:.0f}, topology {:.0f}, "
                  "viewport / scissor {:.0f}, stencil ref {:.0f}, blend factor {:.0f}, variant root constants {:.0f}, SRV "
                  "table {:.0f}, sampler tables {:.0f}, VS float cb {:.0f}, PS float cb {:.0f}, other cbs {:.0f}, vertex "
                  "buffers {:.0f}, index buffer {:.0f} | ExecuteIndirect runs: {:.0f} now ({:.1f} draws each), {:.0f} with "
                  "bindless tables ({:.1f} draws each)",
                  e.frames, d / f, e.pso / f, e.topology / f, e.viewport / f, e.stencil / f, e.blend / f,
                  e.root_constants / f, e.srv_table / f, e.sampler_table / f, e.cbv_vs / f, e.cbv_ps / f,
                  e.cbv_other / f, e.vb / f, e.ib / f, e.runs_now / f, e.runs_now ? d / double(e.runs_now) : 0.0,
                  e.runs_bindless / f, e.runs_bindless ? d / double(e.runs_bindless) : 0.0);
      LogWatchStats(e.frames);
      e = EiStats{};
    }

    void NewFrame(uint64_t frame)
    {
      ++g_ei.frames;
      if (g_ei.frames >= 600)
      {
        LogEiStats();
      }
      // A new pass segment: the tables are cleared below and the geometry check window is per frame (as before split
      // screen) unless pdz_native_split_geometry starts more within the frame.
      ++g_split.segment;
      g_split.table_epoch = g_split.segment;
      g_split.geometry_stamp = g_split.segment;
      if (g_split.announced && !SplitActiveAt(frame))
      {
        g_split.announced = false;
        REXLOG_INFO("pdz_native: split screen over: no depth texture resolved twice in a frame since frame {} (now {}); "
                    "single-pass caches again",
                    g_split.last_multi_frame, frame);
      }
      g_frame.frame = frame;
      g_frame.root_signature_set = false;
      g_frame.last_pso = nullptr;
      g_frame.ls = {}; // pdz_native_state_filter: nothing known about the list (with the root signature above)
      g_frame.vs_cb = g_frame.ps_cb = 0;
      g_frame.vs_cb_shader = g_frame.ps_cb_shader = nullptr;
      g_frame.vs_cb_cpu = g_frame.ps_cb_cpu = nullptr;
      g_frame.last_bool_loop_gpu = g_frame.last_system_gpu = 0; // pdz_native_cb_dedupe: a new frame's upload heap
      g_frame.shader_miss = false;
      g_frame.srv_tables.Clear();
      g_frame.sampler_tables.Clear();
      g_frame.null_samplers.ptr = 0;
      // Maintenance at the frame's first draw, before anything of this frame is looked up (no Pipeline, VS binding or
      // geometry entry pointer is held): the frame stamp for the LRU of PSOs and VS bindings and, on their own cadence,
      // pdz_native_pso_cap, pdz_native_vs_binding_cap and pdz_native_geometry_shrink.
      PipelineCache::Get().OnFrame(frame);
      ShaderCache::Get().OnFrame(frame);
      g_geometry.Maintain(frame);
      drawdump::NewFrame(); // pdz_native_draw_dump (re-reads its cvars; a bool test per draw while off)
      ShortStacksNewFrame(frame); // pdz_fix_short_stacks / pdz_stack_layer_log (re-reads their cvars)
      const int32_t every = REXCVAR_GET(pdz_native_log_frames);
      if (every > 0 && frame % uint64_t(every) == 0 && frame != 0)
      {
        LogDrawStats("periodic");
        LogDrawPerf(frame);
        LogSplit(frame);
      }
    }

    // -----------------------------------------------------------------------
    // Draw arguments per record kind
    // -----------------------------------------------------------------------
    struct DrawArgs
    {
      uint32_t prim = 0;
      bool indexed = false;
      bool user_data = false; // DrawVerticesUP / BeginVertices: vertices at a guest pointer, one stream
      uint32_t base_vertex = 0;
      uint32_t start_index = 0;
      uint32_t index_count = 0;
      uint32_t start_vertex = 0;
      uint32_t vertex_count = 0;
      uint32_t data = 0;   // user_data: guest pointer
      uint32_t stride = 0; // user_data: bytes
      // DrawIndexedVerticesUP: the indices are at a guest pointer too (no index buffer object), base_vertex 0,
      // start_index 0, vertex_count = min vertex index + vertex count (the vertices readable from `data`).
      bool user_indices = false;
      bool index32 = false;    // user_indices: 32-bit (index format argument 1), else 16-bit
      uint32_t index_data = 0; // user_indices: guest pointer
    };

    bool ArgsFor(const shadow::DrawRecord &r, DrawArgs &a)
    {
      switch (r.kind)
      {
      case shadow::Kind::kDrawIndexedVertices:
        a.prim = r.a[0];
        a.indexed = true;
        a.base_vertex = r.a[1];
        a.start_index = r.a[2];
        a.index_count = r.a[3];
        return a.index_count != 0;
      case shadow::Kind::kDrawVertices:
        a.prim = r.a[0];
        a.start_vertex = r.a[1];
        a.vertex_count = r.a[2];
        return a.vertex_count != 0;
      case shadow::Kind::kDrawVerticesUP:
        a.prim = r.a[0];
        a.user_data = true;
        a.vertex_count = r.a[1];
        a.data = r.a[2];
        a.stride = r.a[3];
        return a.vertex_count != 0 && a.stride != 0 && shadow::IsGuestPointer(a.data);
      case shadow::Kind::kBeginVertices:
        a.prim = r.a[0];
        a.user_data = true;
        a.vertex_count = r.a[1];
        a.stride = r.a[2];
        a.data = r.a[3]; // 0 until the hook patches it after forwarding; checked again in OnEndVertices
        return a.vertex_count != 0 && a.stride != 0 && (a.data == 0 || shadow::IsGuestPointer(a.data));
      case shadow::Kind::kDrawIndexedVerticesUP:
        // D3D copies the vertices [min, min + count) and draws with VGT_INDX_OFFSET = -min: an index i reads vertex
        // i of `data`. The upload below takes the vertices the indices actually reach, so only the end matters.
        a.prim = r.a[0];
        a.indexed = true;
        a.user_data = true;
        a.user_indices = true;
        a.index_count = r.a[3];
        a.index_data = r.a[4];
        a.index32 = r.a[5] == 1;
        a.data = r.a[6];
        a.stride = r.a[7];
        a.vertex_count = r.a[1] + r.a[2];
        return a.index_count != 0 && r.a[2] != 0 && a.stride != 0 && a.vertex_count >= r.a[2] &&
               a.vertex_count <= kMaxDrawVertices && shadow::IsGuestPointer(a.data) &&
               shadow::IsGuestPointer(a.index_data);
      default:
        return false;
      }
    }

    // -----------------------------------------------------------------------
    // Constant buffers
    // -----------------------------------------------------------------------
    void ApplyFloatLiterals(uint8_t *cb, const NativeShader *s)
    {
      for (const FloatLiteral &l : s->float_literals)
      {
        if (l.reg < 256)
        {
          std::memcpy(cb + 16 * l.reg, l.bits, 16);
        }
      }
    }

    // pdz_native_cb_used_only (stage 2 M5 drawcost): float constant uploads, whole 4 KB blocks or only the registers
    // the shader reads, and the bytes written into the upload heap (the draws line).
    struct FloatCbStats
    {
      uint64_t whole = 0, trimmed = 0;
      uint64_t bytes = 0;
    };
    FloatCbStats g_float_cb;

    std::string FloatCbText()
    {
      const uint64_t uploads = g_float_cb.whole + g_float_cb.trimmed;
      return fmt::format("float cb uploads {} (whole {}, used registers only {}), {} KB written ({:.0f} B per upload; "
                         "pdz_native_cb_used_only {}; {})",
                         uploads, g_float_cb.whole, g_float_cb.trimmed, g_float_cb.bytes / 1024,
                         uploads ? double(g_float_cb.bytes) / double(uploads) : 0.0,
                         REXCVAR_GET(pdz_native_cb_used_only) ? "on" : "off", ShaderCache::FloatConstantScanText());
    }

    // pdz_native_state_filter (stage 2 M5 drawcost3): the draw path's filterable state calls (topology, viewport,
    // scissor, stencil ref, blend factor, 4 root CBVs, 3 root tables, vertex / index buffers) and those skipped.
    struct StateFilterStats
    {
      uint64_t considered = 0, skipped = 0;
    };
    StateFilterStats g_state_filter;

    std::string StateFilterText()
    {
      uint64_t om_set = 0, om_skipped = 0;
      Surfaces::Get().TargetFilterCounts(om_set, om_skipped);
      const auto pct = [](uint64_t part, uint64_t whole) { return whole ? 100.0 * double(part) / double(whole) : 0.0; };
      return fmt::format("state filter {}: state calls skipped {} of {} ({:.0f}%), OMSetRenderTargets skipped {} of {} "
                         "({:.0f}%)",
                         REXCVAR_GET(pdz_native_state_filter) ? "on" : "off", g_state_filter.skipped,
                         g_state_filter.considered, pct(g_state_filter.skipped, g_state_filter.considered), om_skipped,
                         om_set + om_skipped, pct(om_skipped, om_set + om_skipped));
    }

    // pdz_native_cb_dedupe (stage 2 M5 drawcost4): the bool / loop and system constant buffers, built in cached memory,
    // reuse the previous upload of the device frame when their bytes are equal (the same GPU address then also lets
    // pdz_native_state_filter skip the root CBV call). Counts per buffer kind.
    struct SmallCbCounts
    {
      uint64_t uploads = 0, reused = 0;
    };
    struct SmallCbStats
    {
      SmallCbCounts bool_loop, system;
    };
    SmallCbStats g_small_cb;

    template <typename T>
    D3D12_GPU_VIRTUAL_ADDRESS UploadSmallCb(NativeDevice &dev, const T &value, T &last, D3D12_GPU_VIRTUAL_ADDRESS &last_gpu,
                                            SmallCbCounts &counts)
    {
      // Compared byte for byte (BoolLoopConstants / SystemConstants: 32-bit fields only, sizes asserted in
      // native_pipeline.h, so no padding): equal bytes = the same upload.
      static_assert(std::is_trivially_copyable_v<T> && sizeof(T) % 4 == 0);
      if (last_gpu && REXCVAR_GET(pdz_native_cb_dedupe) && std::memcmp(&value, &last, sizeof(T)) == 0)
      {
        ++counts.reused;
        return last_gpu; // an upload of this device frame (NewFrame clears last_gpu): still in the frame's upload heap
      }
      UploadSlice slice = dev.Upload(sizeof(T), 256);
      if (!slice.cpu)
      {
        return 0;
      }
      std::memcpy(slice.cpu, &value, sizeof(T));
      ++counts.uploads;
      last = value;
      last_gpu = slice.gpu;
      return slice.gpu;
    }

    std::string SmallCbText()
    {
      return fmt::format("bool/loop cb reused {} of {}, system cb reused {} of {} (pdz_native_cb_dedupe {})",
                         g_small_cb.bool_loop.reused, g_small_cb.bool_loop.reused + g_small_cb.bool_loop.uploads,
                         g_small_cb.system.reused, g_small_cb.system.reused + g_small_cb.system.uploads,
                         REXCVAR_GET(pdz_native_cb_dedupe) ? "on" : "off");
    }

    // pdz_fix_water_loop (0.9.2; transparency_water.cpp has the key and the story): the pixel shader's stacked
    // tfetch3D slots (pdz_native_stacked_array) whose fetch constant says clamp_z = repeat. pdzTfetch3DS
    // (native_hlslfix.h) wraps their layers (the last blends into the first) instead of clamping them to the first /
    // last layer as the SDK does: Rare's water animations (ripple stacks, caustics, running water) are looping stacks
    // whose layer coordinate Rare wraps by 1.0 every few seconds, which the clamp turned into a freeze and a jump.
    std::atomic<uint64_t> g_water_loop_draws{0};
    constexpr uint32_t kWaterLoopMinLayers = 8;

    // pdz_fix_short_stacks (overnight 1108 A6, Sweep T1 / S1 candidate 1): stacks of 2-7 layers keep the clamp
    // (a static 2-layer blend sampled at w = 0 must stay on layer 0), EXCEPT the ones a pdz_stack_layer_log dump proved
    // Rare animates as a blended loop (TexAnim, w wrapped into [-1/N, 1 - 1/N) by 82760FC4): with the clamp their
    // first frame was held for 3/4 of the period (N = 2), the second never shown on its own, then a snap back. Each
    // entry: the pixel shader, its slot, and the constant whose .w is that slot's layer coordinate (from the dump);
    // a draw whose .w is exactly 0 (a static material sharing the shader) is left clamped.
    struct ShortStack
    {
      uint64_t ps_hash;
      uint32_t slot;
      bool ps_reg; // the register is a PS constant (else VS)
      uint32_t reg;
      const char *what;
    };
    constexpr ShortStack kShortStacks[] = {
        // Datacore (and River, S1): a 2-frame alpha-tested quad (VS 61BBF23A). Overnight 1108 dumps a6_dc_dump2 /
        // a6_te_dump2: VS c19 = (0, 0, 1, w), w climbing and wrapped from about 0.5 to -0.5 (Rare's [-1/N, 1 - 1/N)
        // for N = 2) 28 and 38 times in the two runs; with the clamp its second frame never showed on its own.
        {0xDC005D2B09D0303Full, 0, false, 19, "Datacore / River 2-frame quad, VS c19.w loops -0.5..0.5"},
        // Trinity Escape (dump a6_te_dump3, 120 fps): 2-frame repeat stacks whose VS c19 = (0, 0, 1, w) loops in
        // Rare's -0.5..0.5 (every sample moving, none exactly 0), many on the same 256x256x2 texture as DC005D2B
        // (lighting variants of one material), and the clamp_z 0 uses of the cyan ring's shaders (CAFD0BC8 /
        // 131B14E8; their clamp_z 1 512x32 uses keep w in 0.23..0.85 and are left alone: the rule needs repeat).
        // c19 is the texcoord .z each of these vertex shaders writes (translated HLSL: o0/o3/o4.z = dot(.., c[19])).
        {0x034A4C53233F2826ull, 0, false, 19, "Trinity Escape 2-frame stack, VS c19.w loops -0.5..0.5"},
        {0x131B14E81BD0F8CAull, 0, false, 19, "Trinity Escape 2-frame stack (t0), VS c19.w loops -0.5..0.5"},
        {0x131B14E81BD0F8CAull, 4, false, 19, "Trinity Escape 2-frame stack (t4), VS c19.w loops -0.5..0.5"},
        {0x254FD7CACB28CAA9ull, 0, false, 19, "Trinity Escape 2-frame stack, VS c19.w loops -0.5..0.5"},
        {0x3F4CABBA957622CFull, 0, false, 19, "Trinity Escape / River 2-frame stack (t0), VS c19.w loops -0.5..0.5"},
        {0x3F4CABBA957622CFull, 1, false, 19, "Trinity Escape / River 2-frame stack (t1), VS c19.w loops -0.5..0.5"},
        {0x4844C283BC5A7F27ull, 2, false, 19, "Trinity Escape / River 2-frame stack (t2), VS c19.w loops -0.5..0.5"},
        {0x4844C283BC5A7F27ull, 3, false, 19, "Trinity Escape / River 2-frame stack (t3), VS c19.w loops -0.5..0.5"},
        {0xB26D03C5BB02A137ull, 0, false, 19, "Trinity Escape / Datacore 2-frame stack, VS c19.w loops -0.5..0.5"},
        {0xC06C4F4590D601F2ull, 0, false, 19, "Trinity Escape 2-frame stack, VS c19.w loops -0.5..0.5"},
        {0xCAFD0BC8EFA10DB7ull, 0, false, 19, "Trinity Escape 2-frame stack (t0), VS c19.w loops -0.5..0.5"},
        {0xCAFD0BC8EFA10DB7ull, 5, false, 19, "Trinity Escape 2-frame stack (t5), VS c19.w loops -0.5..0.5"},
        {0xE5D38E6AECDE8FFFull, 5, false, 19, "Trinity Escape / Datacore 2-frame stack, VS c19.w loops -0.5..0.5"},
    };

    struct ShortStackState
    {
      bool on = false;          // pdz_fix_short_stacks && pdz_game_fixes, read once a frame
      std::string extra_text;   // pdz_fix_short_stacks_extra as last parsed
      std::vector<ShortStack> extra;
      int32_t log_every = 0;    // pdz_stack_layer_log
      bool log_frame = false;   // this frame is a pdz_stack_layer_log frame
      uint64_t frame = 0;
      uint64_t last_log_frame = 0;
      uint32_t log_lines = 0;
      std::vector<uint64_t> logged_keys; // this log frame: (ps hash ^ slot ^ texture address) already logged
      std::mutex wrapped_mutex;
      std::vector<std::pair<uint64_t, uint32_t>> wrapped; // (ps hash, slot) whose first wrap was logged
    } g_short;

    void ShortStacksNewFrame(uint64_t frame)
    {
      ShortStackState &s = g_short;
      s.on = REXCVAR_GET(pdz_fix_short_stacks) && REXCVAR_GET(pdz_game_fixes);
      s.frame = frame;
      if (s.on)
      {
        const std::string text = REXCVAR_GET(pdz_fix_short_stacks_extra);
        if (text != s.extra_text)
        {
          s.extra_text = text;
          s.extra.clear();
          size_t at = 0;
          while (at < text.size())
          {
            size_t end = text.find(',', at);
            if (end == std::string::npos)
            {
              end = text.size();
            }
            const std::string item = text.substr(at, end - at);
            at = end + 1;
            uint64_t hash = 0;
            unsigned slot = 0, reg = 0;
            char stage = 0;
            if (std::sscanf(item.c_str(), " %llx:%u:%c%u", reinterpret_cast<unsigned long long *>(&hash), &slot,
                            &stage, &reg) == 4 &&
                slot < 32 && (stage == 'v' || stage == 'p') && reg < 256)
            {
              s.extra.push_back({hash, slot, stage == 'p', reg, "pdz_fix_short_stacks_extra"});
            }
            else if (!item.empty())
            {
              REXLOG_WARN("pdz_fix_short_stacks_extra: item '{}' not understood (want PSHASH:slot:v19)", item);
            }
          }
          REXLOG_INFO("pdz_fix_short_stacks_extra: {} extra stack(s)", s.extra.size());
        }
      }
      s.log_every = REXCVAR_GET(pdz_stack_layer_log);
      s.log_frame = false;
      if (s.log_every > 0 && frame >= uint64_t(std::max(0, REXCVAR_GET(pdz_stack_layer_log_from))) &&
          s.log_lines < uint32_t(std::max(0, REXCVAR_GET(pdz_stack_layer_log_lines))) &&
          (s.last_log_frame == 0 || frame >= s.last_log_frame + uint64_t(s.log_every)))
      {
        s.log_frame = true;
        s.last_log_frame = frame;
        s.logged_keys.clear();
      }
    }

    // A float4 register from the device constant store (big-endian guest layout; dev_ptr = the device struct or the
    // record thread's mirror of it).
    void DeviceConst(const uint8_t *dev_ptr, uint32_t offset, uint32_t reg, float out[4])
    {
      for (uint32_t i = 0; i < 4; ++i)
      {
        const uint8_t *p = dev_ptr + offset + 16 * reg + 4 * i;
        const uint32_t v = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
        std::memcpy(&out[i], &v, 4);
      }
    }

    // pdz_stack_layer_log: one line per (pixel shader, slot, texture) in a log frame.
    void LogStackLayers(const shadow::DrawRecord &r, const NativeShader *ps, const NativeShader *vs,
                        const uint8_t *dev_ptr)
    {
      ShortStackState &s = g_short;
      if (!dev_ptr || s.log_lines >= uint32_t(std::max(0, REXCVAR_GET(pdz_stack_layer_log_lines))))
      {
        return;
      }
      std::string slots;
      uint64_t key = ps->hash;
      for (const ShaderTexture &t : ps->textures)
      {
        if (t.slot < 32 && ((ps->array_slots >> t.slot) & 1u))
        {
          const FetchDecoded f = DecodeFetch(r.fetch_be + t.slot * shadow::dev::kFetchConstantBytes);
          if (f.dimension == 1 && f.depth > 1)
          {
            slots += fmt::format(" t{} {}L z{} {}x{} @{:08X}", t.slot, f.depth, f.clamp_z, f.width, f.height,
                                 f.base_address);
            key = key * 1000003ull ^ (uint64_t(f.base_address) << 5 | t.slot);
          }
        }
      }
      if (slots.empty())
      {
        return;
      }
      for (uint64_t k : s.logged_keys)
      {
        if (k == key)
        {
          return;
        }
      }
      if (s.logged_keys.size() >= 512)
      {
        return;
      }
      s.logged_keys.push_back(key);
      std::string consts;
      auto add = [&](char stage, uint32_t offset, uint32_t count) {
        for (uint32_t reg = 0; reg < count; ++reg)
        {
          float c[4];
          DeviceConst(dev_ptr, offset, reg, c);
          if (c[0] == 0.0f && c[1] == 0.0f && c[2] != 0.0f)
          {
            consts += fmt::format(" {}{}*{:.4g}={:.5g}", stage, reg, c[2], c[3]);
          }
          else if (c[3] != 0.0f)
          {
            consts += fmt::format(" {}{}={:.5g}", stage, reg, c[3]);
          }
        }
      };
      add('v', shadow::dev::kVsAluConstants, 64);
      add('p', shadow::dev::kPsAluConstants, 32);
      ++s.log_lines;
      REXLOG_INFO("pdz_stack_layer: f{} ps {:016X} vs {:016X}{} |{}", s.frame, ps->hash, vs ? vs->hash : 0ull, slots,
                  consts);
    }

    // The slots of the short-stack list (and pdz_fix_short_stacks_extra) that wrap for this draw.
    uint32_t ShortStackMask(const shadow::DrawRecord &r, const NativeShader *ps, const uint8_t *dev_ptr)
    {
      ShortStackState &s = g_short;
      uint32_t mask = 0;
      auto check = [&](const ShortStack &e) {
        if (e.ps_hash != ps->hash || !dev_ptr || !((ps->array_slots >> e.slot) & 1u))
        {
          return;
        }
        const FetchDecoded f = DecodeFetch(r.fetch_be + e.slot * shadow::dev::kFetchConstantBytes);
        if (f.dimension != 1 || f.depth < 2 || f.depth >= kWaterLoopMinLayers || f.clamp_z != 0)
        {
          return; // not a short repeat stack in this draw (8+ layers: WaterLoopMask's rule already wraps them)
        }
        float c[4];
        DeviceConst(dev_ptr, e.ps_reg ? shadow::dev::kPsAluConstants : shadow::dev::kVsAluConstants, e.reg, c);
        if (c[3] == 0.0f)
        {
          return; // w exactly 0: a static material on the same shader stays on layer 0
        }
        mask |= 1u << e.slot;
        std::lock_guard<std::mutex> lock(s.wrapped_mutex);
        for (const auto &w : s.wrapped)
        {
          if (w.first == e.ps_hash && w.second == e.slot)
          {
            return;
          }
        }
        if (s.wrapped.size() < 64)
        {
          s.wrapped.emplace_back(e.ps_hash, e.slot);
          REXLOG_INFO("pdz_fix_short_stacks: ps {:016X} t{} ({} layers, {}x{}) wraps its layers (layer coordinate "
                      "{}c{}.w = {:.4g}; {})",
                      e.ps_hash, e.slot, f.depth, f.width, f.height, e.ps_reg ? "PS " : "VS ", e.reg, c[3],
                      e.what ? e.what : "");
        }
      };
      for (const ShortStack &e : kShortStacks)
      {
        check(e);
      }
      for (const ShortStack &e : s.extra)
      {
        check(e);
      }
      return mask;
    }

    uint32_t WaterLoopMask(const shadow::DrawRecord &r, const NativeShader *ps, const NativeShader *vs,
                           const uint8_t *dev_ptr)
    {
      if (!ps || ps->array_slots == 0)
      {
        return 0;
      }
      if (g_short.log_frame)
      {
        LogStackLayers(r, ps, vs, dev_ptr);
      }
      const uint32_t short_mask = g_short.on ? ShortStackMask(r, ps, dev_ptr) : 0;
      if (!REXCVAR_GET(pdz_fix_water_loop) || !REXCVAR_GET(pdz_game_fixes))
      {
        return short_mask;
      }
      uint32_t mask = short_mask, clamped = 0;
      uint32_t depth_of[32] = {}, clamp_z_of[32] = {};
      for (const ShaderTexture &t : ps->textures)
      {
        if (t.slot < 32 && ((ps->array_slots >> t.slot) & 1u))
        {
          const FetchDecoded f = DecodeFetch(r.fetch_be + t.slot * shadow::dev::kFetchConstantBytes);
          // A stack of 8 or more layers (an animation: the sea's ripples 16, the caustics 32) whose fetch says
          // repeat on its layer axis (xenos ClampMode kRepeat). Smaller stacks (2-layer blends) keep the clamp.
          if (f.dimension == 1 && f.depth >= kWaterLoopMinLayers && f.clamp_z == 0)
          {
            mask |= 1u << t.slot;
          }
          else if (f.dimension == 1 && f.depth > 1 && !((short_mask >> t.slot) & 1u))
          {
            clamped |= 1u << t.slot; // a stack left as it was (fewer layers, or its fetch says clamp)
          }
          depth_of[t.slot] = f.depth;
          clamp_z_of[t.slot] = f.clamp_z;
        }
      }
      if (mask != 0)
      {
        ++g_water_loop_draws;
      }
      if (mask != 0 || clamped != 0)
      {
        // One line per pixel shader (the first 64): which of its stacked slots wrap.
        static std::mutex seen_mutex;
        static uint64_t seen[64] = {};
        static uint32_t seen_count = 0;
        std::lock_guard<std::mutex> lock(seen_mutex);
        bool known = false;
        for (uint32_t i = 0; i < seen_count && !known; ++i)
        {
          known = seen[i] == ps->hash;
        }
        if (!known && seen_count < 64)
        {
          seen[seen_count++] = ps->hash;
          std::string slots;
          for (uint32_t n = 0; n < 32; ++n)
          {
            if (((mask | clamped) >> n) & 1u)
            {
              slots += fmt::format(" t{} {} ({} layers, clamp_z {})", n, (mask >> n) & 1u ? "wraps" : "clamped",
                                   depth_of[n], clamp_z_of[n]);
            }
          }
          REXLOG_INFO("pdz_fix_water_loop: ps {:016X} stacked slots:{} ({} draws wrapped so far)", ps->hash, slots,
                      g_water_loop_draws.load());
        }
      }
      return mask;
    }

    D3D12_GPU_VIRTUAL_ADDRESS UploadFloatConstants(NativeDevice &dev, const uint8_t *dev_ptr, uint32_t dev_offset,
                                                   uint32_t float4_count, const NativeShader *s, bool literals,
                                                   const uint8_t **cpu_out)
    {
      UploadSlice slice = dev.Upload(kFloatCbBytes, 256);
      if (!slice.cpu)
      {
        *cpu_out = nullptr;
        return 0;
      }
      uint8_t *cb = static_cast<uint8_t *>(slice.cpu);
      *cpu_out = cb;
      if (!s->float_constants_all && REXCVAR_GET(pdz_native_cb_used_only) && !drawdump::Armed())
      {
        // Only the registers the shader can read (NativeShader::float_constant_runs), with the values the whole block
        // would give them: swapped from the device below float4_count, zero above (PS c224..c255). The other registers
        // of the slice are left unwritten; no shader stage reads them (relative addressing always takes the branch
        // below). The draw dump (armed) gets the whole block, as it reads the slice back.
        for (const std::pair<uint16_t, uint16_t> &run : s->float_constant_runs)
        {
          const uint32_t first = run.first, end = uint32_t(run.first) + run.second;
          const uint32_t swap_end = std::min(end, float4_count);
          if (first < swap_end)
          {
            SwapConstants(cb + first * 16, dev_ptr + dev_offset + first * 16, (swap_end - first) * 16);
          }
          const uint32_t zero_first = std::max(first, float4_count);
          if (zero_first < end)
          {
            std::memset(cb + zero_first * 16, 0, (end - zero_first) * 16);
          }
        }
        ++g_float_cb.trimmed;
        g_float_cb.bytes += uint64_t(s->float_constants_count) * 16;
      }
      else
      {
        SwapConstants(cb, dev_ptr + dev_offset, float4_count * 16);
        if (float4_count < 256)
        {
          std::memset(cb + float4_count * 16, 0, (256 - float4_count) * 16);
        }
        ++g_float_cb.whole;
        g_float_cb.bytes += kFloatCbBytes;
      }
      if (literals)
      {
        ApplyFloatLiterals(cb, s);
      }
      return slice.gpu;
    }

    // Loop constant `i` (0..31) as UploadBoolLoop uploads it: the device store (big-endian), or a shader's literal
    // when pdz_native_literals is on (the PS literal wins, as in UploadBoolLoop). Read from guest memory, not
    // from the write-combined upload slice.
    uint32_t LoopConstantValue(const uint8_t *dev_ptr, uint32_t i, const NativeShader *vs, const NativeShader *ps,
                               bool literals)
    {
      if (literals)
      {
        if (ps->loop_literal_mask & (1u << i))
        {
          return ps->loop_literals[i];
        }
        if (vs->loop_literal_mask & (1u << i))
        {
          return vs->loop_literals[i];
        }
      }
      return __builtin_bswap32(Load32Unaligned(dev_ptr + kDevLoopConstants + 4 * i));
    }

    // The loop constants `s` loops over whose count (bits 7:0) is 0, as a bit mask: such a loop body never runs
    // (SDK DxbcShaderTranslator::ProcessLoopStartInstruction, the fork's loopCount() != 0 test). A few bit
    // operations per draw, only for shaders with loops (1 VS, 183 PS in the research cache).
    uint32_t ZeroCountLoops(const uint8_t *dev_ptr, const NativeShader *s, const NativeShader *vs,
                            const NativeShader *ps, bool literals)
    {
      uint32_t zero = 0;
      for (uint32_t m = s->loop_constants_used; m != 0; m &= m - 1)
      {
        const uint32_t i = uint32_t(__builtin_ctz(m));
        if ((LoopConstantValue(dev_ptr, i, vs, ps, literals) & 0xFFu) == 0)
        {
          zero |= 1u << i;
        }
      }
      return zero;
    }

    D3D12_GPU_VIRTUAL_ADDRESS UploadBoolLoop(NativeDevice &dev, const shadow::DrawRecord &r, const uint8_t *dev_ptr,
                                             const NativeShader *vs, const NativeShader *ps, bool literals)
    {
      // Built in cached memory (the literal merge below reads it back: no read of the write-combined upload heap),
      // then copied up whole or the previous identical upload reused (UploadSmallCb, pdz_native_cb_dedupe).
      BoolLoopConstants local{};
      BoolLoopConstants *bl = &local;
      for (uint32_t i = 0; i < 8; ++i)
      {
        bl->bool_bits[i] = shadow::Swap32(r.bools_be[i]);
      }
      SwapConstants(reinterpret_cast<uint8_t *>(bl->loop_constants), dev_ptr + kDevLoopConstants, 32 * 4);
      if (literals)
      {
        for (const NativeShader *s : {vs, ps})
        {
          for (uint32_t i = 0; i < 8; ++i)
          {
            if (s->bool_literal_mask[i])
            {
              bl->bool_bits[i] = (bl->bool_bits[i] & ~s->bool_literal_mask[i]) |
                                 (s->bool_literal_bits[i] & s->bool_literal_mask[i]);
            }
          }
          for (uint32_t i = 0; i < 32; ++i)
          {
            if (s->loop_literal_mask & (1u << i))
            {
              bl->loop_constants[i] = s->loop_literals[i];
            }
          }
        }
      }
      return UploadSmallCb(dev, local, g_frame.last_bool_loop, g_frame.last_bool_loop_gpu, g_small_cb.bool_loop);
    }

    // -----------------------------------------------------------------------
    // Descriptor tables
    // -----------------------------------------------------------------------
    // pdz_native_scale > 1: a fetch with unnormalized (texel) coordinates on a resolve mirror. The fork's
    // unnormalize2D / 3D divide by the bound view's GetDimensions, the host size, where the SDK divides by the guest
    // size: the coordinates come out divided by the scale. Not corrected (no shader in the cache does it: the 63
    // unnormalized fetches are VS lookups on slot 16); logged once per shader and slot, counted.
    void NoteUnnormalizedScaled(const NativeShader &s, uint32_t slot, const FetchDecoded &fetch, const HostTexture &t)
    {
      static uint64_t seen[16] = {};
      static uint32_t seen_count = 0;
      static uint64_t total = 0;
      ++total;
      const uint64_t key = s.hash ^ (uint64_t(slot) << 58);
      if (std::find(seen, seen + seen_count, key) != seen + seen_count || seen_count >= 16)
      {
        return;
      }
      seen[seen_count++] = key;
      REXLOG_WARN("pdz_native: resolution scale {}: {} {:016X} fetches slot {} with unnormalized coordinates from "
                  "resolve mirror {:08X} ({}x{} guest, {}x{} host); its texel coordinates are divided by the host size, "
                  "so it reads the wrong texels (not corrected, {} such bindings so far)",
                  t.scale, s.is_vertex ? "VS" : "PS", s.hash, slot, fetch.base_address, fetch.width, fetch.height,
                  t.width, t.height, total);
    }

    // Keys of the per-frame descriptor tables: the shader identities plus the raw fetch constants of the
    // slots they reference (at most 2 x 32 x 24 bytes), hashed from a stack buffer.
    struct KeyBuffer
    {
      uint8_t bytes[16 + 64 * shadow::dev::kFetchConstantBytes];
      uint32_t used = 0;
      void Add(const void *p, uint32_t n)
      {
        if (used + n <= sizeof(bytes))
        {
          std::memcpy(bytes + used, p, n);
          used += n;
        }
      }
      uint64_t Hash() const { return XXH3_64bits(bytes, used); }
    };

    uint64_t TableKey(const shadow::DrawRecord &r, const NativeShader *a, const NativeShader *b)
    {
      KeyBuffer k;
      for (const NativeShader *s : {a, b})
      {
        if (!s)
        {
          continue;
        }
        k.Add(&s->hash, sizeof(s->hash));
        for (const ShaderTexture &t : s->textures)
        {
          k.Add(r.fetch_be + t.slot * shadow::dev::kFetchConstantBytes, shadow::dev::kFetchConstantBytes);
        }
      }
      return k.Hash();
    }

    uint64_t SamplerKey(const shadow::DrawRecord &r, const NativeShader *s)
    {
      KeyBuffer k;
      k.Add(&s->hash, sizeof(s->hash));
      for (const ShaderSampler &sm : s->samplers)
      {
        k.Add(r.fetch_be + sm.slot * shadow::dev::kFetchConstantBytes, shadow::dev::kFetchConstantBytes);
      }
      return k.Hash();
    }

    // pdz_debug_unbarriered_magenta: the textures of each table built this frame, and one magenta texture.
    struct UnbarrieredSim
    {
      uint64_t frame = ~0ull;
      std::unordered_map<uint64_t, std::vector<HostTexture *>> tables; // table key -> textures it binds
      std::vector<const HostTexture *> substitute;                    // the rebuild in progress: these get magenta
      ID3D12Resource *magenta = nullptr;
      D3D12_RESOURCE_STATES magenta_state = D3D12_RESOURCE_STATE_COPY_DEST;
      D3D12_CPU_DESCRIPTOR_HANDLE magenta_srv{};
      uint64_t draws = 0;
      uint32_t logged = 0;
    };
    UnbarrieredSim g_unbarriered;

    // pdz_debug_unbarriered_magenta_only: the address filter (parsed when the string changes).
    bool UnbarrieredWanted(uint32_t base_address)
    {
      static std::string spec;
      static std::vector<uint32_t> only;
      const std::string now = REXCVAR_GET(pdz_debug_unbarriered_magenta_only);
      if (now != spec)
      {
        spec = now;
        only.clear();
        size_t i = 0;
        while (i < spec.size())
        {
          const size_t j = spec.find(',', i);
          const std::string item = spec.substr(i, j == std::string::npos ? std::string::npos : j - i);
          if (!item.empty())
          {
            only.push_back(uint32_t(std::strtoul(item.c_str(), nullptr, 16)));
          }
          i = j == std::string::npos ? spec.size() : j + 1;
        }
      }
      return only.empty() || std::find(only.begin(), only.end(), base_address) != only.end();
    }

    // 4x4 R16G16B16A16_FLOAT (4, 0, 4, 1): magenta, bright enough to survive tone mapping.
    D3D12_CPU_DESCRIPTOR_HANDLE UnbarrieredMagentaSrv(NativeDevice &dev)
    {
      UnbarrieredSim &u = g_unbarriered;
      if (!u.magenta)
      {
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = 4;
        desc.Height = 4;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        desc.SampleDesc.Count = 1;
        u.magenta = dev.CreateTexture(desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, "pdz_debug magenta");
        if (!u.magenta)
        {
          return {};
        }
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        UINT64 total = 0;
        dev.device()->GetCopyableFootprints(&desc, 0, 1, 0, &fp, nullptr, nullptr, &total);
        const UploadSlice slice = dev.Upload(uint32_t(total), D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
        if (!slice.cpu)
        {
          return {};
        }
        const uint16_t texel[4] = {0x4400, 0, 0x4400, 0x3C00}; // 4.0, 0, 4.0, 1.0
        for (uint32_t y = 0; y < 4; ++y)
        {
          for (uint32_t x = 0; x < 4; ++x)
          {
            std::memcpy(static_cast<uint8_t *>(slice.cpu) + fp.Offset + y * fp.Footprint.RowPitch + x * 8, texel, 8);
          }
        }
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = u.magenta;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = slice.resource;
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = fp;
        src.PlacedFootprint.Offset += slice.offset;
        dev.list()->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        dev.Transition(u.magenta, u.magenta_state,
                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        u.magenta_srv = dev.AllocStagingSrv();
        if (u.magenta_srv.ptr)
        {
          D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
          srv.Format = desc.Format;
          srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
          srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
          srv.Texture2D.MipLevels = 1;
          dev.device()->CreateShaderResourceView(u.magenta, &srv, u.magenta_srv);
        }
      }
      return u.magenta_srv;
    }

    bool BindSrvTable(NativeDevice &dev, const shadow::DrawRecord &r, const NativeShader *vs,
                      const NativeShader *ps, D3D12_GPU_DESCRIPTOR_HANDLE &out)
    {
      const bool sim = REXCVAR_GET(pdz_debug_unbarriered_magenta);
      if (sim && g_unbarriered.frame != dev.frame_index())
      {
        g_unbarriered.frame = dev.frame_index();
        g_unbarriered.tables.clear();
      }
      g_unbarriered.substitute.clear();
      if (vs->textures.empty() && ps->textures.empty())
      {
        // Nothing sampled: one shared null table per frame.
        constexpr uint64_t kNullKey = 1;
        if (g_frame.srv_tables.Find(kNullKey, out))
        {
          return true;
        }
        // One descriptor: the shader samples nothing (the heap padding keeps the declared range in bounds).
        Descriptor d = dev.AllocSrv(1);
        if (!d.cpu.ptr)
        {
          return false;
        }
        dev.device()->CopyDescriptorsSimple(1, d.cpu, PipelineCache::Get().null_srv_block(),
                                            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        g_frame.srv_tables.Insert(kNullKey, d.gpu);
        out = d.gpu;
        return true;
      }
      // Never 0 / 1 (the empty and null-table markers). Salted with the table epoch (pdz_native_split_tables): constant
      // through a single-pass frame, so a table of another pass segment is never found.
      const uint64_t key = (TableKey(r, vs, ps) ^ (g_split.table_epoch * 0x9E3779B97F4A7C15ull)) | 2u;
      if (g_frame.srv_tables.Find(key, out))
      {
        bool reuse = true;
        if (sim)
        {
          // A texture of the reused table still in a render-target state (resolved into since the table was built,
          // no barrier back): the undefined read. Rebuilt below with magenta for it.
          if (auto it = g_unbarriered.tables.find(key); it != g_unbarriered.tables.end())
          {
            for (HostTexture *t : it->second)
            {
              if (!(t->state & D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) && UnbarrieredWanted(t->base_address))
              {
                g_unbarriered.substitute.push_back(t);
                // Each distinct (pixel shader, texture) once, 64 at most.
                static std::vector<std::pair<uint64_t, uint32_t>> seen;
                const std::pair<uint64_t, uint32_t> id{ps->hash, t->base_address};
                if (g_unbarriered.logged < 64 && std::find(seen.begin(), seen.end(), id) == seen.end())
                {
                  seen.push_back(id);
                  ++g_unbarriered.logged;
                  REXLOG_INFO("pdz_native: unbarriered read (pdz_debug_unbarriered_magenta): draw #{} caller {:08X} vs "
                              "{:016x} ps {:016x} reuses a table whose texture {:08X} {}x{} is still a render target "
                              "(state {:#x}); shown magenta",
                              r.sequence, r.caller, vs->hash, ps->hash, t->base_address, t->key_width, t->key_height,
                              uint32_t(t->state));
                }
              }
            }
          }
          reuse = g_unbarriered.substitute.empty();
          g_unbarriered.draws += reuse ? 0 : 1;
        }
        if (reuse)
        {
          ++g_stats.table_reuse;
          return true;
        }
      }
      std::vector<HostTexture *> bound_textures;
      // Compact table: only up to the highest entry the two shaders sample (space * 32 + slot; most draws use a
      // handful of 2D slots, a cube map reaches 64+, a stacked-array tfetch3D 96+). The root signature's 128-entry
      // range stays inside the heap through its padding, and entries past `count` are never read.
      uint32_t count = 1;
      for (const NativeShader *s : {vs, ps})
      {
        for (const ShaderTexture &t : s->textures)
        {
          count = std::max(count, uint32_t(t.dim) * 32 + t.slot + 1);
        }
        if (s->array_slots)
        {
          count = std::max(count, kSrvArraySpace * 32 + 32u - uint32_t(__builtin_clz(s->array_slots)));
        }
      }
      Descriptor d = dev.AllocSrv(count);
      if (!d.cpu.ptr)
      {
        return false;
      }
      dev.device()->CopyDescriptorsSimple(count, d.cpu, PipelineCache::Get().null_srv_block(),
                                          D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
      const uint32_t size = dev.srv_descriptor_size();
      for (const NativeShader *s : {vs, ps})
      {
        for (const ShaderTexture &t : s->textures)
        {
          const FetchDecoded fetch = DecodeFetch(r.fetch_be + t.slot * shadow::dev::kFetchConstantBytes);
          // pdz_native_stacked_array (native_hlslfix.h): a patched tfetch3D slot bound to a 1D / 2D / stacked texture or
          // a resolve mirror reads its Texture2DArray view at space3 t<slot>, as the SDK samples it; its Texture3D slot
          // stays null. A true 3D texture (fetch dimension 2) keeps the Texture3D path, space3 null.
          if (t.dim == TexDim::k3D && ((s->array_slots >> t.slot) & 1u) && fetch.dimension < 2)
          {
            TextureView array;
            {
              const pdz::timing::PhaseScope phase(pdz::timing::Phase::kDrawTexLookup, 2); // pdz_frame_timing_phases 2
              array = Textures::Get().LookupArray(fetch);
            }
            if (array.texture && array.srv.ptr)
            {
              D3D12_CPU_DESCRIPTOR_HANDLE dst = d.cpu;
              dst.ptr += SIZE_T(kSrvArraySpace * 32 + t.slot) * size;
              dev.CopySrv(dst, array.srv);
              ++g_stats.stacked_array_views;
              continue;
            }
            if (fetch.type == 2 && fetch.base_address)
            {
              ++g_stats.stacked_array_fallbacks; // the shader takes the Texture3D path below (volume view or null)
            }
          }
          TextureView view;
          {
            const pdz::timing::PhaseScope phase(pdz::timing::Phase::kDrawTexLookup, 2); // pdz_frame_timing_phases 2
            view = Textures::Get().Lookup(fetch);
          }
          if (!view.texture || !view.srv.ptr)
          {
            continue; // the table keeps its null SRV
          }
          StampTable(*view.texture); // a resolve into it ends the pass segment (OnResolveInto)
          // The SRV type must match the shader's tfetch kind (2D / 3D / cube); FetchDecoded::dimension is
          // 0 1D, 1 2D-or-stacked, 2 3D, 3 cube. A 2D / stacked texture under a tfetch3D gets its volume view
          // when VolumeViewFor says so (pdz_native_volume_view, _stacked, _2d, _dxn), else the slot stays null.
          static const uint32_t kExpectedDimension[3] = {1, 2, 3};
          if (view.texture->dimension == 1 && kExpectedDimension[uint32_t(t.dim)] == 2 && VolumeViewFor(fetch))
          {
            TextureView volume;
            {
              const pdz::timing::PhaseScope phase(pdz::timing::Phase::kDrawTexLookup, 2); // pdz_frame_timing_phases 2
              volume = Textures::Get().LookupVolume(fetch);
            }
            if (volume.texture && volume.srv.ptr)
            {
              view = volume;
            }
          }
          // A 1D texture (dimension 0) is a TEXTURE2D resource with a 2D view: it fits a 2D fetch (review finding 6).
          const uint32_t bound_dimension = view.texture->dimension == 0 ? 1u : view.texture->dimension;
          if (bound_dimension != kExpectedDimension[uint32_t(t.dim)])
          {
            ++g_stats.texture_kind_mismatch;
            continue;
          }
          // pdz_native_scale > 1 only (every texture has scale 1 at scale 1): unnormalized coordinates on a mirror.
          if (view.texture->scale > 1 && ((s->unnormalized_slots >> t.slot) & 1u))
          {
            NoteUnnormalizedScaled(*s, t.slot, fetch, *view.texture);
          }
          D3D12_CPU_DESCRIPTOR_HANDLE dst = d.cpu;
          dst.ptr += SIZE_T(uint32_t(t.dim) * 32 + t.slot) * size;
          D3D12_CPU_DESCRIPTOR_HANDLE src = view.srv;
          if (sim)
          {
            bound_textures.push_back(view.texture);
            const auto &sub = g_unbarriered.substitute;
            if (std::find(sub.begin(), sub.end(), view.texture) != sub.end())
            {
              if (const D3D12_CPU_DESCRIPTOR_HANDLE m = UnbarrieredMagentaSrv(dev); m.ptr)
              {
                src = m;
              }
            }
          }
          dev.CopySrv(dst, src);
        }
      }
      g_frame.srv_tables.Insert(key, d.gpu);
      if (sim)
      {
        g_unbarriered.tables[key] = std::move(bound_textures);
      }
      out = d.gpu;
      return true;
    }

    // Persistent sampler tables (BindSamplerTable): key = the table's sampler descriptors, value = where the table sits
    // in the device's persistent sampler region and the last frame that bound it.
    struct SamplerTable
    {
      D3D12_GPU_DESCRIPTOR_HANDLE gpu{};
      uint32_t index = 0; // heap index of its first descriptor (Descriptor::index)
      uint32_t count = 0;
      uint64_t last_used = 0;
    };
    struct SamplerTableCache
    {
      std::unordered_map<uint64_t, SamplerTable> tables;
      uint64_t region_refused = 0; // tables the region had no room for (served from the per-frame ring)
      uint64_t evicted = 0;        // tables freed by pdz_native_sampler_evict
      uint64_t passes = 0;         // eviction passes (at most one per frame)
      uint64_t last_pass_frame = ~uint64_t(0);
      uint32_t logged = 0;
    };
    SamplerTableCache g_samplers;

    // pdz_native_sampler_evict: called when the region refused a table, at most once per frame. Frees the tables that
    // no frame in flight can read (BeginFrame of frame F waited for frame F - 3): pass 1 those unused for
    // kSamplerIdleFrames (a previous level's), pass 2, when pass 1 freed under kSamplerEvictTarget descriptors, every
    // one unused for kSamplerMinAge frames. A table bound in this frame is never freed, so the handles in this frame's
    // table cache (g_frame.sampler_tables) stay valid, and the descriptors of a freed table are rewritten only by the
    // next table placed there. True when anything was freed.
    bool EvictSamplerTables(NativeDevice &dev, uint64_t frame)
    {
      SamplerTableCache &c = g_samplers;
      if (!REXCVAR_GET(pdz_native_sampler_evict) || c.last_pass_frame == frame)
      {
        return false;
      }
      c.last_pass_frame = frame;
      uint32_t tables = 0, descriptors = 0;
      uint64_t min_age_used = 0;
      for (const uint64_t min_age : {kSamplerIdleFrames, kSamplerMinAge})
      {
        if (min_age == kSamplerMinAge && descriptors >= kSamplerEvictTarget)
        {
          break;
        }
        min_age_used = min_age;
        for (auto it = c.tables.begin(); it != c.tables.end();)
        {
          const SamplerTable &t = it->second;
          if (t.last_used + min_age <= frame)
          {
            dev.FreePersistentSampler(t.index, t.count);
            descriptors += t.count;
            ++tables;
            it = c.tables.erase(it);
          }
          else
          {
            ++it;
          }
        }
      }
      ++c.passes;
      c.evicted += tables;
      if (c.logged < 8)
      {
        ++c.logged;
        REXLOG_INFO("pdz_native: persistent sampler region full in frame {}: freed {} tables ({} descriptors) unused for "
                    "{}+ frames; {} tables kept (pass #{}, {} tables freed in total; pdz_native_sampler_evict)",
                    frame, tables, descriptors, min_age_used, c.tables.size(), c.passes, c.evicted);
      }
      return tables != 0;
    }

    bool BindSamplerTable(NativeDevice &dev, const shadow::DrawRecord &r, const NativeShader *s,
                          D3D12_GPU_DESCRIPTOR_HANDLE &out)
    {
      if (s->samplers.empty())
      {
        if (!g_frame.null_samplers.ptr)
        {
          Descriptor d = dev.AllocSampler(1);
          if (!d.cpu.ptr)
          {
            return false;
          }
          dev.device()->CopyDescriptorsSimple(1, d.cpu, PipelineCache::Get().null_sampler_block(),
                                              D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
          g_frame.null_samplers = d.gpu;
        }
        out = g_frame.null_samplers;
        return true;
      }
      // Key = the sampler descriptors themselves (not the fetch constants: those include the texture address, so
      // identical samplers on different textures never shared a table; log 034: 796 sampler entries a frame and
      // 380 draws refused). Tables live in the device's persistent sampler region, shared across frames.
      uint32_t count = 0;
      for (const ShaderSampler &sm : s->samplers)
      {
        count = std::max(count, sm.index + 1);
      }
      D3D12_SAMPLER_DESC descs[kSamplerTableSize] = {};
      bool used[kSamplerTableSize] = {};
      for (const ShaderSampler &sm : s->samplers)
      {
        if (sm.index < kSamplerTableSize)
        {
          descs[sm.index] = GetSampler(r.fetch_be + sm.slot * shadow::dev::kFetchConstantBytes, sm.code);
          used[sm.index] = true;
        }
      }
      count = std::min(count, kSamplerTableSize);
      const uint64_t key = (XXH3_64bits_withSeed(descs, sizeof(D3D12_SAMPLER_DESC) * count, count) ^
                            (XXH3_64bits(used, count) << 1)) | 2u;
      if (g_frame.sampler_tables.Find(key, out))
      {
        ++g_stats.table_reuse;
        return true;
      }
      SamplerTableCache &cache = g_samplers;
      if (auto it = cache.tables.find(key); it != cache.tables.end())
      {
        it->second.last_used = g_frame.frame;
        out = it->second.gpu;
        g_frame.sampler_tables.Insert(key, out);
        ++g_stats.table_reuse;
        return true;
      }
      Descriptor d = dev.AllocPersistentSampler(count);
      if (!d.cpu.ptr && EvictSamplerTables(dev, g_frame.frame))
      {
        d = dev.AllocPersistentSampler(count); // room freed by pdz_native_sampler_evict
      }
      const bool is_persistent = d.cpu.ptr != 0;
      if (!is_persistent)
      {
        ++cache.region_refused;
        d = dev.AllocSampler(count); // persistent region full: per-frame ring
      }
      if (!d.cpu.ptr)
      {
        return false;
      }
      dev.device()->CopyDescriptorsSimple(count, d.cpu, PipelineCache::Get().null_sampler_block(),
                                          D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
      const uint32_t size = dev.sampler_descriptor_size();
      for (const ShaderSampler &sm : s->samplers)
      {
        if (sm.index >= kSamplerTableSize)
        {
          continue;
        }
        D3D12_CPU_DESCRIPTOR_HANDLE dst = d.cpu;
        dst.ptr += SIZE_T(sm.index) * size;
        dev.device()->CreateSampler(&descs[sm.index], dst);
      }
      if (is_persistent)
      {
        SamplerTable t;
        t.gpu = d.gpu;
        t.index = d.index;
        t.count = std::max(count, 1u); // as AllocPersistentSampler counted it
        t.last_used = g_frame.frame;
        cache.tables.emplace(key, t);
      }
      g_frame.sampler_tables.Insert(key, d.gpu);
      out = d.gpu;
      return true;
    }

    // -----------------------------------------------------------------------
    // Geometry upload
    // -----------------------------------------------------------------------
    // pdz_native_geometry_watch (overnight 30-31 Oct): the geometry hash read 30 MB of guest memory per frame at Outpost
    // (3.5 of the record thread's ~11 ms) to find that ~93% of the ranges had not changed. The SDK keeps Xenia's write
    // watches on guest physical memory (Memory::EnablePhysicalMemoryAccessCallbacks: the pages are made read-only in the
    // guest's views; a write by guest code, the kernel's file reads, a Protect / Decommit / Release all run the
    // invalidation callbacks and unprotect the pages). Here each physical page has a generation, raised by our
    // callback for every page it is told about (the callback returns exactly that range, so exactly those pages lose
    // their watch). A cached range found unchanged by its hash for 8 checks in a row (more after each invalidation) is
    // armed: watch enabled, then hashed again, accepted only if no callback touched its pages meanwhile. While its pages'
    // generation sum is the armed one, nothing wrote them: the cached copy is used without reading guest memory.
    // A changed range takes the hash path as before. pdz_native_geometry_watch_verify hashes every skipped range anyway
    // and counts mismatches (expected 0).
    namespace geowatch
    {
      constexpr uint32_t kPhysicalBytes = 0x20000000u, kPages = kPhysicalBytes >> 12;
      std::atomic<uint32_t> g_gen[kPages];
      void *g_handle = nullptr;
      bool g_tried = false;
      // The self-test (once, at the first arm): a write of a byte's own value through the game's view of an armed
      // page must raise that page's generation. If it doesn't, the watch is off for the session (hashing as before).
      bool g_selftest_done = false, g_broken = false;
      std::atomic<uint64_t> g_callbacks{0}, g_pages_invalidated{0};
      uint64_t g_armed_total = 0;
      struct Stats
      {
        uint64_t clean = 0, invalidated = 0, armed = 0, arm_refused = 0, verified = 0, mismatches = 0;
        uint64_t clean_bytes = 0;
      } g_stats;

      std::pair<uint32_t, uint32_t> Callback(void *, uint32_t start, uint32_t length, bool)
      {
        if (length && start < kPhysicalBytes)
        {
          const uint32_t first = start >> 12;
          const uint32_t last = std::min<uint32_t>((uint64_t(start) + length - 1) >> 12, kPages - 1);
          for (uint32_t p = first; p <= last; ++p)
          {
            g_gen[p].fetch_add(1, std::memory_order_acq_rel);
          }
          g_pages_invalidated.fetch_add(last - first + 1, std::memory_order_relaxed);
        }
        g_callbacks.fetch_add(1, std::memory_order_relaxed);
        return {start, length}; // unwatch exactly the pages whose generation was raised
      }

      bool Ready()
      {
        if (!g_tried)
        {
          g_tried = true;
          if (auto *memory = REX_KERNEL_MEMORY())
          {
            g_handle = memory->RegisterPhysicalMemoryInvalidationCallback(Callback, nullptr);
          }
          REXLOG_INFO("pdz_native: geometry watch {}", g_handle ? "on (write watches on the cached vertex / index ranges)"
                                                                 : "unavailable (no guest memory object): hashing as before");
        }
        return g_handle != nullptr && !g_broken;
      }

      // After the first arm: write a byte of the armed range with its own value through the 0xA0000000 view (the one the
      // watch protects, read-only); the access violation must go through the SDK's handler to our callback.
      void SelfTest(uint32_t address, uint32_t bytes)
      {
        g_selftest_done = true;
        auto *memory = REX_KERNEL_MEMORY();
        if (!memory || !bytes)
        {
          return;
        }
        uint8_t *view = memory->virtual_membase() + 0xA0000000u + address;
        MEMORY_BASIC_INFORMATION info{};
        VirtualQuery(view, &info, sizeof(info));
        const uint32_t page = address >> 12;
        const uint32_t before = g_gen[page].load(std::memory_order_acquire);
        volatile uint8_t *p = view;
        const uint8_t value = *p;
        *p = value; // faults on a watched page: the handler runs the callbacks, unprotects, and the write completes
        const uint32_t after = g_gen[page].load(std::memory_order_acquire);
        if (after != before)
        {
          REXLOG_INFO("pdz_native: geometry watch self-test passed: a write to an armed page (physical {:08X}, view "
                      "protection {:#x}) raised its generation {} -> {}",
                      address, uint32_t(info.Protect), before, after);
        }
        else
        {
          g_broken = true;
          REXLOG_WARN("pdz_native: geometry watch self-test FAILED: a write to an armed page (physical {:08X}, view "
                      "protection {:#x}) raised no callback: the watch is off for this session, hashing as before",
                      address, uint32_t(info.Protect));
        }
      }

      uint64_t GenSum(uint32_t address, uint32_t bytes)
      {
        const uint32_t first = address >> 12, last = (address + bytes - 1) >> 12;
        uint64_t sum = 0;
        for (uint32_t p = first; p <= last; ++p)
        {
          sum += g_gen[p].load(std::memory_order_acquire);
        }
        return sum;
      }

      void Arm(uint32_t address, uint32_t bytes)
      {
        if (auto *memory = REX_KERNEL_MEMORY())
        {
          memory->EnablePhysicalMemoryAccessCallbacks(address, bytes, true, false);
        }
      }

      uint32_t StableNeeded(const GeometryEntry &e) { return 8u << std::min<uint32_t>(e.invalidations, 6); }
    } // namespace geowatch

    void LogWatchStats(uint64_t frames)
    {
      geowatch::Stats &w = geowatch::g_stats;
      const double f = double(std::max<uint64_t>(frames, 1));
      if (REXCVAR_GET(pdz_native_geometry_watch))
      {
        REXLOG_INFO("pdz_native: geometry watch ({} frames): used without a hash {:.0f} ranges / {:.0f} KB per frame, "
                    "invalidated {:.1f}, armed {:.1f}, arm refused {:.1f} per frame; run totals: armed {}, callbacks {}, "
                    "pages invalidated {}; verify {}: checked {} mismatches {}",
                    frames, w.clean / f, double(w.clean_bytes) / 1024.0 / f, w.invalidated / f, w.armed / f,
                    w.arm_refused / f, geowatch::g_armed_total, geowatch::g_callbacks.load(),
                    geowatch::g_pages_invalidated.load(),
                    REXCVAR_GET(pdz_native_geometry_watch_verify) ? "on" : "off", w.verified, w.mismatches);
      }
      w = geowatch::Stats{};
    }

    // Primitive reset of an indexed draw (REVIEW.md 9, the SDK primitive processor's handling; native_pipeline.h
    // DecodePrimitiveReset has the rules). Strips keep their index format with the reset value rewritten to all ones
    // (0xFFFF / 0xFFFFFFFF) and IBStripCutValue set in the PSO; a 16-bit strip whose reset value is not 0xFFFF while
    // 0xFFFF is also a real vertex index goes up as 32-bit indices (`widen`); fans split into separate fans in the
    // expansion. Reset indices never count towards the uploaded vertex range.
    struct IndexReset
    {
      bool enabled = false;
      bool index32 = false;
      bool widen = false;
      uint32_t index = 0; // the guest reset value: whole 16-bit index, or the low 24 bits of a 32-bit one
      D3D12_INDEX_BUFFER_STRIP_CUT_VALUE cut = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;

      bool Is(uint32_t v) const { return enabled && (index32 ? (v & 0x00FFFFFFu) : v) == index; }
    };
    constexpr uint32_t kResetMarker = 0xFFFFFFFFu; // reset indices in the swapped scratch (fans, widened strips)

    // Cached copy of `bytes` guest bytes at host pointer `src` (keyed by the guest address), swapped per
    // `endian`. For index buffers (`indices16`) the swap also scans the index range into the entry. `reset`
    // (strips with primitive reset on): reset indices become all ones and stay out of the range.
    bool UploadRange(NativeDevice &dev, uint32_t guest_address, const uint8_t *src, uint32_t bytes, uint32_t endian,
                     uint32_t index_size, D3D12_GPU_VIRTUAL_ADDRESS &gpu, uint32_t *min_index, uint32_t *max_index,
                     bool is_vertex, const IndexReset *reset = nullptr)
    {
      // A reset copy has its own key (bit 29: bytes stay below 2^26) and the reset value salts the content hash.
      const bool cut = reset && reset->enabled && index_size != 0;
      const uint64_t key = (uint64_t(guest_address) << 32) | bytes | (index_size == 2 ? 0x80000000u : 0u) |
                           (index_size == 4 ? 0x40000000u : 0u) | (cut ? 0x20000000u : 0u);
      // pdz_native_geometry_hash_once: a range an earlier draw of this frame already hashed is not hashed again
      // (the same mesh in the shadow, depth and G-buffer passes). pdz_native_debug_geometry_verify hashes it anyway
      // and, if it changed since, goes on below exactly as with the switch off. A reset copy (cut) is always
      // hashed: its reset value salts the content hash and is not part of the key. The window is the frame, or one
      // pass segment in split screen (pdz_native_split_geometry: player 2's pass hashes again what player 1's checked).
      const uint64_t stamp = g_split.geometry_stamp;
      if (REXCVAR_GET(pdz_native_geometry_hash_once) && !cut)
      {
        if (GeometryEntry *e = g_geometry.FindChecked(key, stamp))
        {
          if (!REXCVAR_GET(pdz_native_debug_geometry_verify) || CountedSampleHash(src, bytes, stamp) == e->content_hash)
          {
            ++g_perf.hash_skips;
            gpu = e->gpu;
            if (min_index) *min_index = e->min_index;
            if (max_index) *max_index = e->max_index;
            if (is_vertex) ++g_stats.vertex_cache_hits; else ++g_stats.index_cache_hits;
            return true;
          }
          ++g_perf.verify_changes;
        }
      }
      // pdz_native_geometry_watch: an armed range whose pages nobody wrote since: its copy, no hash.
      const bool watch = REXCVAR_GET(pdz_native_geometry_watch) && !cut && bytes &&
                         uint64_t(guest_address) + bytes <= geowatch::kPhysicalBytes && geowatch::Ready();
      if (watch)
      {
        if (GeometryEntry *e = g_geometry.FindKey(key))
        {
          if (e->armed)
          {
            if (geowatch::GenSum(guest_address, bytes) == e->armed_gen)
            {
              if (REXCVAR_GET(pdz_native_geometry_watch_verify))
              {
                ++geowatch::g_stats.verified;
                if (CountedSampleHash(src, bytes, stamp) != e->content_hash && geowatch::g_stats.mismatches++ < 8)
                {
                  REXLOG_WARN("pdz_native: geometry watch verify: range {:08X} ({} bytes) changed without a watch "
                              "callback (mismatch #{})",
                              guest_address, bytes, geowatch::g_stats.mismatches);
                }
              }
              g_geometry.Touch(*e, g_frame.frame, stamp);
              ++geowatch::g_stats.clean;
              geowatch::g_stats.clean_bytes += bytes;
              gpu = e->gpu;
              if (min_index) *min_index = e->min_index;
              if (max_index) *max_index = e->max_index;
              if (is_vertex) ++g_stats.vertex_cache_hits; else ++g_stats.index_cache_hits;
              return true;
            }
            e->armed = false; // written since (its pages are unwatched now): hashed below
            e->stable = 0;
            e->invalidations = uint8_t(std::min(255, e->invalidations + 1));
            ++geowatch::g_stats.invalidated;
          }
        }
      }
      const uint64_t content =
          CountedSampleHash(src, bytes, stamp) ^ (cut ? (uint64_t(reset->index) + 1) * 0x9E3779B97F4A7C15ull : 0ull);
      uint32_t chunk = 0;
      if (GeometryEntry *e = g_geometry.Find(key, content, g_frame.frame, stamp, chunk))
      {
        if (watch && ++e->stable >= geowatch::StableNeeded(*e))
        {
          // Arm: watch first, then the bytes again; armed only if no callback touched these pages meanwhile (a write
          // after the watch was enabled always raises a generation).
          const uint64_t before = geowatch::GenSum(guest_address, bytes);
          geowatch::Arm(guest_address, bytes);
          const uint64_t again = CountedSampleHash(src, bytes, stamp);
          const uint64_t after = geowatch::GenSum(guest_address, bytes);
          if (before == after && again == content)
          {
            e->armed = true;
            e->armed_gen = after;
            ++geowatch::g_stats.armed;
            ++geowatch::g_armed_total;
            if (!geowatch::g_selftest_done)
            {
              geowatch::SelfTest(guest_address, bytes); // invalidates this entry's arm on success (hashed next time)
            }
          }
          else
          {
            e->stable = 0;
            ++geowatch::g_stats.arm_refused;
          }
        }
        gpu = e->gpu;
        if (min_index) *min_index = e->min_index;
        if (max_index) *max_index = e->max_index;
        if (is_vertex) ++g_stats.vertex_cache_hits; else ++g_stats.index_cache_hits;
        return true;
      }
      GeometryEntry *entry = nullptr;
      uint8_t *dst = g_geometry.Insert(key, bytes, content, g_frame.frame, stamp, &entry);
      D3D12_GPU_VIRTUAL_ADDRESS dst_gpu = 0;
      if (dst)
      {
        dst_gpu = entry->gpu;
      }
      else
      {
        UploadSlice slice = dev.Upload(bytes, 16);
        if (!slice.cpu)
        {
          return false;
        }
        dst = static_cast<uint8_t *>(slice.cpu);
        dst_gpu = slice.gpu;
        ++g_stats.uncached_fallbacks;
      }
      uint32_t lo = 0xFFFFFFFFu, hi = 0;
      if (index_size == 4)
      {
        const uint32_t count = bytes / 4;
        for (uint32_t i = 0; i < count; ++i)
        {
          const uint32_t v = (uint32_t(src[4 * i]) << 24) | (uint32_t(src[4 * i + 1]) << 16) |
                             (uint32_t(src[4 * i + 2]) << 8) | src[4 * i + 3];
          if (cut && reset->Is(v))
          {
            std::memcpy(dst + 4 * i, &kResetMarker, 4); // low 24 bits matched: exactly 0xFFFFFFFF for the host
            continue;
          }
          std::memcpy(dst + 4 * i, &v, 4);
          lo = std::min<uint32_t>(lo, v);
          hi = std::max<uint32_t>(hi, v);
        }
        if (count == 0)
        {
          lo = 0;
        }
      }
      else if (index_size == 2)
      {
        const uint32_t count = bytes / 2;
        for (uint32_t i = 0; i < count; ++i)
        {
          const uint16_t v = uint16_t((uint16_t(src[2 * i]) << 8) | src[2 * i + 1]);
          if (cut && reset->Is(v))
          {
            const uint16_t all_ones = 0xFFFFu; // the caller made sure 0xFFFF is no real index here
            std::memcpy(dst + 2 * i, &all_ones, 2);
            continue;
          }
          std::memcpy(dst + 2 * i, &v, 2);
          lo = std::min<uint32_t>(lo, v);
          hi = std::max<uint32_t>(hi, v);
        }
        if (count == 0)
        {
          lo = 0;
        }
      }
      else
      {
        SwapCopy(dst, src, bytes, endian);
      }
      if (cut && lo > hi)
      {
        lo = hi = 0; // reset indices only: nothing is drawn (every primitive is cut)
      }
      if (entry)
      {
        entry->min_index = lo;
        entry->max_index = hi;
      }
      if (min_index) *min_index = lo;
      if (max_index) *max_index = hi;
      gpu = dst_gpu;
      if (is_vertex)
      {
        ++g_stats.vertex_uploads;
        g_stats.vertex_bytes += bytes;
      }
      else
      {
        ++g_stats.index_uploads;
      }
      return true;
    }

    // Rewrites QUADLIST / TRIANGLEFAN into a 32-bit triangle-list index buffer in frame upload memory.
    // `src16` = the swapped 16-bit source indices (nullptr = implicit 0..n-1); `count` = source indices.
    // `reset` (TRIANGLEFAN with primitive reset on): kResetMarker entries end a fan and the next index starts a new
    // one, as the SDK's GetMultiPrimitiveHostIndexCountAndRanges + TriangleFanToList (QUADLIST never resets).
    bool ExpandIndices(NativeDevice &dev, uint32_t prim, const uint32_t *src16, uint32_t count, uint32_t vertex_offset,
                       D3D12_GPU_VIRTUAL_ADDRESS &gpu, uint32_t &out_count, bool reset = false)
    {
      const bool quads = prim == uint32_t(xenos::PrimitiveType::kQuadList);
      const bool split = reset && !quads && src16;
      out_count = quads ? (count / 4) * 6 : (count >= 3 ? (count - 2) * 3 : 0);
      if (split)
      {
        out_count = 0;
        for (uint32_t s = 0; s < count;)
        {
          uint32_t e = s;
          while (e < count && src16[e] != kResetMarker) ++e;
          out_count += e - s >= 3 ? (e - s - 2) * 3 : 0;
          s = e + 1;
        }
      }
      if (!out_count)
      {
        return false;
      }
      UploadSlice slice = dev.Upload(out_count * 4, 16);
      if (!slice.cpu)
      {
        return false;
      }
      uint32_t *dst = static_cast<uint32_t *>(slice.cpu);
      auto at = [&](uint32_t i) -> uint32_t { return (src16 ? uint32_t(src16[i]) : i) + vertex_offset; };
      uint32_t o = 0;
      if (quads)
      {
        for (uint32_t q = 0; q + 4 <= count; q += 4)
        {
          dst[o++] = at(q);
          dst[o++] = at(q + 1);
          dst[o++] = at(q + 2);
          dst[o++] = at(q);
          dst[o++] = at(q + 2);
          dst[o++] = at(q + 3);
        }
      }
      else if (split)
      {
        for (uint32_t s = 0; s < count;)
        {
          uint32_t e = s;
          while (e < count && src16[e] != kResetMarker) ++e;
          for (uint32_t i = s + 1; i + 1 < e; ++i)
          {
            dst[o++] = at(s);
            dst[o++] = at(i);
            dst[o++] = at(i + 1);
          }
          s = e + 1;
        }
      }
      else
      {
        for (uint32_t i = 1; i + 1 < count; ++i)
        {
          dst[o++] = at(0);
          dst[o++] = at(i);
          dst[o++] = at(i + 1);
        }
      }
      gpu = slice.gpu;
      return true;
    }

    // The primitive reset of an indexed draw (IndexReset above), decided before the PSO lookup because the strip
    // cut value is part of the PSO. Disabled = the draw goes exactly as without primitive reset handling.
    IndexReset ResolveIndexReset(uint8_t *base, const shadow::DrawRecord &r, const DrawArgs &args)
    {
      IndexReset out;
      if (args.indexed && args.user_indices)
      {
        // DrawIndexedVerticesUP: format from the argument; the indices always go up as 32-bit ones (IssueDraw), so
        // a strip's reset index becomes 0xFFFFFFFF (kResetMarker) with that cut value; fans split in the expansion.
        const PrimitiveReset pr = DecodePrimitiveReset(r, args.prim, args.index32);
        if (pr.enabled)
        {
          out.enabled = true;
          out.index32 = args.index32;
          out.index = pr.index;
          if (!TopologyFor(args.prim).expand)
          {
            out.cut = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF;
          }
        }
        return out;
      }
      if (!args.indexed || !shadow::IsGuestPointer(r.index_buffer))
      {
        return out; // non-indexed draws have no reset index; a bad index buffer object is skipped later
      }
      const bool index32 = (view::Load32(base, r.index_buffer + 0) & 0x80000000u) != 0; // as IssueDraw
      const PrimitiveReset pr = DecodePrimitiveReset(r, args.prim, index32);
      if (!pr.enabled)
      {
        return out;
      }
      out.enabled = true;
      out.index32 = index32;
      out.index = pr.index;
      if (TopologyFor(args.prim).expand)
      {
        return out; // TRIANGLEFAN: split into separate fans by the expansion, no host strip cut
      }
      if (index32)
      {
        out.cut = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF;
        return out;
      }
      out.cut = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF;
      if (pr.index != 0xFFFFu)
      {
        // The reset value is rewritten to 0xFFFF, unless 0xFFFF is also a real vertex index of this draw: then the
        // indices go up as 32-bit ones with 0xFFFFFFFF for the reset (the SDK's Get16BitResetIndexUsage /
        // ReplaceResetIndex16To24).
        const uint32_t ib_base = view::Load32(base, r.index_buffer + shadow::dev::kVbBase) & 0x1FFFFFFEu;
        const uint8_t *src = PhysPtr(ib_base + args.start_index * 2u);
        if (src && args.index_count <= kMaxDrawVertices * 3)
        {
          for (uint32_t i = 0; i < args.index_count; ++i)
          {
            if (src[2 * i] == 0xFF && src[2 * i + 1] == 0xFF)
            {
              out.widen = true;
              out.cut = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF;
              break;
            }
          }
        }
      }
      return out;
    }

    // -----------------------------------------------------------------------
    // The draw
    // -----------------------------------------------------------------------
    // pdz_native_log_up_fans: the uploaded (swapped) vertices 0, 1, 2, n/2 and n-1 of a long UP fan / strip, up to 9
    // floats each, and the VS c0..c3 it is transformed by (radar disc drawn as spikes in Rooftops, logs 088 / 089).
    void LogUpVertices(const shadow::DrawRecord &r, const DrawArgs &args, const uint8_t *data, uint32_t bytes,
                       const uint8_t *dev_ptr)
    {
      static uint32_t logged = 0;
      const int32_t limit = REXCVAR_GET(pdz_native_log_up_fans);
      const bool long_prim = args.prim == uint32_t(xenos::PrimitiveType::kTriangleFan) ||
                             args.prim == uint32_t(xenos::PrimitiveType::kTriangleStrip);
      if (limit <= 0 || logged >= uint32_t(limit) || !long_prim || args.vertex_count <= 8 || !args.stride)
      {
        return;
      }
      ++logged;
      std::string s;
      const uint32_t picks[5] = {0, 1, 2, args.vertex_count / 2, args.vertex_count - 1};
      for (uint32_t v : picks)
      {
        if (uint64_t(v + 1) * args.stride > bytes)
        {
          continue;
        }
        s += " | v" + std::to_string(v) + ":";
        const uint32_t floats = std::min<uint32_t>(args.stride / 4, 9);
        for (uint32_t i = 0; i < floats; ++i)
        {
          float f = 0.0f;
          std::memcpy(&f, data + v * args.stride + 4 * i, 4);
          char buf[24];
          std::snprintf(buf, sizeof(buf), " %.4g", f);
          s += buf;
        }
      }
      std::string cs;
      if (dev_ptr)
      {
        for (uint32_t i = 0; i < 16; ++i)
        {
          const uint8_t *p = dev_ptr + shadow::dev::kVsAluConstants + 4 * i;
          const uint32_t bits = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
          float f = 0.0f;
          std::memcpy(&f, &bits, 4);
          char buf[24];
          std::snprintf(buf, sizeof(buf), "%s%.4g", (i % 4) ? " " : (i ? " / " : ""), f);
          cs += buf;
        }
      }
      REXLOG_INFO("pdz_native: UP {} #{} caller {:08X} prim {} count {} stride {} data {:08X} | VS c0..c3 {}{}",
                  DrawKindName(r.kind), r.sequence, r.caller, args.prim, args.vertex_count, args.stride, args.data, cs,
                  s);
    }

    const char *HostTopologyName(D3D_PRIMITIVE_TOPOLOGY t)
    {
      switch (t)
      {
      case D3D_PRIMITIVE_TOPOLOGY_POINTLIST: return "POINTLIST";
      case D3D_PRIMITIVE_TOPOLOGY_LINELIST: return "LINELIST";
      case D3D_PRIMITIVE_TOPOLOGY_LINESTRIP: return "LINESTRIP";
      case D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST: return "TRIANGLELIST";
      case D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP: return "TRIANGLESTRIP";
      default: return "other";
      }
    }

    // The host topology and the QUADLIST / TRIANGLEFAN expansion of this draw. The PSO key holds the topology type
    // (triangle / line / point), the point and rectangle-list GS bits and the strip cut, not the primitive type
    // (as the SDK's PipelinePrimitiveTopologyType), so TRIANGLELIST, TRIANGLESTRIP (no strip cut), TRIANGLEFAN and
    // QUADLIST draws with the same shaders and state share one PSO, and Pipeline::topology is that of the draw
    // that created it. Read from there (before pdz_native_draw_topology), a TRIANGLESTRIP on a QUADLIST / FAN PSO
    // was expanded as a fan around its vertex 0 (the Rooftops radar rings: 122-vertex UP strips drawn as 360-index
    // fans of spikes, log 116 frame trace), a FAN / QUADLIST on a strip PSO went unexpanded, a list on a strip PSO
    // was drawn as a strip. The SDK sets the host topology per draw from the primitive processor's
    // host_primitive_type (D3D12CommandProcessor::IssueDraw, SetPrimitiveTopology).
    Topology DrawTopology(const shadow::DrawRecord &r, const DrawArgs &args, const Pipeline &pipe)
    {
      const Topology own = TopologyFor(args.prim);
      if (own.topology == pipe.topology.topology && own.expand == pipe.topology.expand)
      {
        return own;
      }
      const bool use_own = REXCVAR_GET(pdz_native_draw_topology);
      ++g_stats.topology_differs;
      // The first draw of each (caller, primitive type) is logged, 16 at most: the call sites this changes.
      static uint32_t seen_caller[16] = {};
      static uint32_t seen_prim[16] = {};
      static uint32_t seen_count = 0;
      bool fresh = seen_count < 16;
      for (uint32_t i = 0; fresh && i < seen_count; ++i)
      {
        fresh = seen_caller[i] != r.caller || seen_prim[i] != args.prim;
      }
      if (fresh)
      {
        seen_caller[seen_count] = r.caller;
        seen_prim[seen_count] = args.prim;
        ++seen_count;
        REXLOG_INFO("pdz_native: draw topology: {} draw #{} f{} caller {:08X} prim {} count {} shares a PSO created "
                    "for {}{}; drawn as {}{} (pdz_native_draw_topology {})",
                    DrawKindName(r.kind), r.sequence, r.frame, r.caller, args.prim,
                    args.indexed ? args.index_count : args.vertex_count, HostTopologyName(pipe.topology.topology),
                    pipe.topology.expand ? " (expanded)" : "",
                    HostTopologyName(use_own ? own.topology : pipe.topology.topology),
                    (use_own ? own.expand : pipe.topology.expand) ? " (expanded)" : "", use_own ? "on" : "off");
      }
      return use_own ? own : pipe.topology;
    }

    bool IssueDraw(uint8_t *base, const shadow::DrawRecord &r, const DrawArgs &args,
                   const uint8_t *device_override = nullptr)
    {
      NativeDevice *devp = NativeDevice::Get();
      if (!devp || !EnsureFrame() || !devp->in_frame() || !devp->list())
      {
        ++g_stats.skipped_off;
        return false;
      }
      NativeDevice &dev = *devp;
      if (dev.frame_index() != g_frame.frame)
      {
        NewFrame(dev.frame_index());
      }
      // D3D flushed this draw's constants whether or not it is issued below: a draw skipped before the upload must
      // not leave a reusable buffer that predates its changes (review finding 3).
      if (r.dirty[0] != 0)
      {
        g_frame.vs_cb = 0;
      }
      if (r.dirty[1] != 0)
      {
        g_frame.ps_cb = 0;
      }
      PipelineCache &pipelines = PipelineCache::Get();
      if (!pipelines.ready() && !pipelines.Init())
      {
        ++g_stats.skipped_off;
        return false;
      }
      if (r.tiling_flag == 0xFF || !shadow::IsGuestPointer(r.device))
      {
        ++g_stats.skipped_off;
        return false;
      }

      // Shaders.
      ShaderCache &shaders = ShaderCache::Get();
      // No pixel shader set = a depth-only draw (z prepass, shadow maps): drawn without a PS stage. Shader objects
      // the Create hooks never saw are read from the D3D object (shadow::FindOrReadShader).
      const shadow::ShaderEntry *ps_entry = r.pixel_shader ? shadow::FindOrReadShader(base, r.pixel_shader, false) : nullptr;
      const shadow::ShaderEntry *vs_entry = r.vertex_shader ? shadow::FindOrReadShader(base, r.vertex_shader, true) : nullptr;
      const NativeShader *ps = r.pixel_shader ? shaders.FindPixel(base, ps_entry) : shaders.NullPixel();
      const NativeShader *vs = shaders.FindVertex(base, r.vertex_shader, r.vertex_decl, vs_entry, &r);
      if (!r.pixel_shader)
      {
        ++g_stats.null_ps_draws;
      }
      if (!vs || !ps)
      {
        if (!ps)
        {
          ++(ps_entry ? g_stats.miss_ps_cache : g_stats.miss_ps_object);
        }
        if (!vs)
        {
          ++(vs_entry ? g_stats.miss_vs_cache : g_stats.miss_vs_object);
        }
        ++g_stats.skipped_shader;
        g_frame.shader_miss = true;
        return false;
      }
      Lap(kStageShaders);

      // Pass dump (one chosen frame): the previous targets are read back when the bound set changes. Before
      // anything is recorded for this draw (the readback flushes the list).
      passdump::BeforeDraw(base, r.device);
      // Draw dump (pdz_native_draw_dump): the same point; the picked draw's textures and colour target 0 are read back
      // here (flushes). AfterDraw below completes it once the draw is recorded.
      const bool dumped = drawdump::Armed() && drawdump::BeforeDraw(base, r, vs, ps);

      // Targets: the surface layer transitions them and sets them on the list (OMSetRenderTargets).
      Surfaces::Bound bound;
      const uint32_t rt_count = Surfaces::Get().BindTargets(base, r.device, &bound);
      if (rt_count == 0 && !bound.ds)
      {
        ++g_stats.skipped_targets;
        return false;
      }
      TargetFormats tf;
      tf.rt_count = std::min<uint32_t>(rt_count, 4);
      const HostSurface *any_surface = bound.ds;
      for (uint32_t i = 0; i < tf.rt_count; ++i)
      {
        tf.rt[i] = bound.rt[i] ? bound.rt[i]->view_format : DXGI_FORMAT_UNKNOWN; // the RTV format
        if (!any_surface && bound.rt[i])
        {
          any_surface = bound.rt[i];
        }
      }
      // The DSV format (D24_UNORM_S8_UINT / D32_FLOAT_S8X24_UINT); the depth resource itself is TYPELESS.
      tf.ds = bound.ds ? bound.ds->view_format : DXGI_FORMAT_UNKNOWN;
      tf.depth_float24 = bound.ds && bound.ds->depth_format == 1;
      // 7e3 render target 0 (k_2_10_10_10_FLOAT / _AS_16_16_16_16) for the blend variants (native_blend7e3.h).
      if (tf.rt_count && bound.rt[0])
      {
        tf.rt0_7e3 = bound.rt[0]->color_format == 3 || bound.rt[0]->color_format == 12;
        tf.rt0_uav = bound.rt[0]->uav_7e3;
      }
      tf.resolution_scale = any_surface && any_surface->scale ? any_surface->scale : Scale();
      const uint32_t host_width = any_surface ? any_surface->host_width : 0;
      const uint32_t host_height = any_surface ? any_surface->host_height : 0;
      // MSAA is not modelled (single-sampled surfaces, 80x16-pixel tiles in the EDRAM aliasing maths): the first
      // multisampled draw is logged so a run shows whether PDZ ever uses it (review 15).
      reg::RB_SURFACE_INFO surface_info;
      surface_info.value = ReadReg(r, uint32_t(reg::RB_SURFACE_INFO::register_index));
      if (surface_info.msaa_samples != xenos::MsaaSamples::k1X)
      {
        static bool logged_msaa = false;
        if (!logged_msaa)
        {
          logged_msaa = true;
          REXLOG_WARN("pdz_native: MSAA draw (not modelled; drawn single-sampled): RB_SURFACE_INFO {:08X} = {} samples; "
                      "surface {:08X} {}x{} ({} colour targets, depth {:08X}); {} draw #{} f{} caller {:08X}",
                      surface_info.value, 1u << uint32_t(surface_info.msaa_samples),
                      any_surface ? any_surface->guest : 0u, any_surface ? any_surface->width : 0u,
                      any_surface ? any_surface->height : 0u, rt_count, bound.ds ? bound.ds->guest : 0u,
                      DrawKindName(r.kind), r.sequence, r.frame, r.caller);
        }
      }
      Lap(kStageTargets);

      // Pipeline.
      RenderState rs;
      DecodeRenderState(r, rs);
      if (!TopologyFor(args.prim).supported)
      {
        ++g_stats.skipped_pipeline;
        if (g_stats.pipeline_unsupported_prim++ < 3)
        {
          REXLOG_INFO("pdz_native: pipeline skip: primitive type {} has no host topology ({} draw #{} caller {:08X})",
                      args.prim, DrawKindName(r.kind), r.sequence, r.caller);
        }
        return false;
      }
      const IndexReset reset = ResolveIndexReset(base, r, args); // the strip cut value is part of the PSO
      Lap(kStageDecode);
      const Pipeline *pipe = pipelines.Lookup(vs, ps, args.prim, rs, tf, reset.cut);
      if (!pipe)
      {
        // A RECTLIST whose pipeline (rectangle-list GS) failed stays an unsupported primitive, as before the GS.
        ++(TopologyFor(args.prim).rects ? g_stats.pipeline_unsupported_prim : g_stats.pipeline_pso_failed);
        ++g_stats.skipped_pipeline;
        return false;
      }
      const Topology topo = DrawTopology(r, args, *pipe); // this draw's own, not the PSO creator's
      Lap(kStagePipeline);

      // Viewport, scissor and the VS position constants from the guest PA registers (PA_CL_VTE_CNTL, VPORT
      // scale / offset, CLIP_CNTL, window offset, pixel centre, window / screen scissor), the SDK's
      // GetHostViewportInfo / GetScissor maths (native_pipeline.cpp ComputeHostViewport). The vertex shader
      // applies ndc_scale / ndc_offset in pdzHostPosition (fork patch 08).
      HostViewport hv;
      ComputeHostViewport(r, tf.resolution_scale, ps->writes_depth, hv);
      const D3D12_VIEWPORT &vp = hv.viewport;
      D3D12_RECT scissor = hv.scissor;
      if (host_width)
      {
        scissor.right = std::min<LONG>(scissor.right, LONG(host_width));
        scissor.bottom = std::min<LONG>(scissor.bottom, LONG(host_height));
        scissor.left = std::min<LONG>(scissor.left, scissor.right);
        scissor.top = std::min<LONG>(scissor.top, scissor.bottom);
      }
      if (hv.empty_viewport || scissor.right <= scissor.left || scissor.bottom <= scissor.top)
      {
        // Nothing can be covered (the SDK draws with an empty scissor; skipping is the same result).
        SkipGeometry(GeometrySkip::kEmptyViewportOrScissor, r, "viewport %ux%u scissor %d,%d..%d,%d", uint32_t(vp.Width),
                     uint32_t(vp.Height), int(scissor.left), int(scissor.top), int(scissor.right), int(scissor.bottom));
        return false;
      }
      Lap(kStageViewport);

      // Geometry: index buffer first (its range bounds the vertex upload).
      RecList *list = dev.list();
      D3D12_INDEX_BUFFER_VIEW ibv = {};
      bool use_index = false;
      uint32_t draw_count = 0;
      uint32_t min_index = 0, max_index = 0;
      int32_t base_vertex_location = 0;
      const uint32_t *swapped_indices = nullptr; // for the expansion path only
      static std::vector<uint32_t> expansion_scratch; // capacity kept across draws (render thread only)

      if (args.indexed && args.user_indices)
      {
        // DrawIndexedVerticesUP: the indices are at a guest (virtual) pointer, big-endian as in an index buffer
        // (BeginIndexedVertices' DRAW_INDX reads its ring copy with endian 8in16 / 8in32). Dynamic data: swapped
        // into the scratch on every draw and, unless a fan / quad expansion follows, uploaded to the frame ring as
        // 32-bit indices (reset indices = kResetMarker, the strip cut ResolveIndexReset chose); never cached.
        const uint32_t index_size = args.index32 ? 4u : 2u;
        const uint64_t index_end = uint64_t(args.index_data) + uint64_t(args.index_count) * index_size;
        if (args.index_count > kMaxDrawVertices * 3 || index_end > 0xFFFF0000ull)
        {
          SkipGeometry(GeometrySkip::kIndexMemory, r, "UP indices at %08X count %u (%u-byte)", args.index_data,
                       args.index_count, index_size);
          return false;
        }
        // pdz_native_record_thread: the hook's copy (the game's own memory, rewritten after the call).
        const uint8_t *src = view::Ptr(base, args.index_data, args.index_count * index_size);
        if (args.index32)
        {
          ++g_stats.index32_draws;
        }
        if (reset.enabled)
        {
          ++g_stats.prim_reset_draws;
          g_stats.prim_reset_rewritten += reset.index != (args.index32 ? 0x00FFFFFFu : 0xFFFFu) ? 1 : 0;
        }
        expansion_scratch.resize(args.index_count);
        min_index = 0xFFFFFFFFu;
        for (uint32_t i = 0; i < args.index_count; ++i)
        {
          const uint32_t v = args.index32 ? (uint32_t(src[4 * i]) << 24) | (uint32_t(src[4 * i + 1]) << 16) |
                                                (uint32_t(src[4 * i + 2]) << 8) | src[4 * i + 3]
                                          : uint32_t((uint16_t(src[2 * i]) << 8) | src[2 * i + 1]);
          if (reset.Is(v))
          {
            expansion_scratch[i] = kResetMarker;
            continue;
          }
          expansion_scratch[i] = v;
          min_index = std::min<uint32_t>(min_index, v);
          max_index = std::max<uint32_t>(max_index, v);
        }
        if (min_index > max_index)
        {
          min_index = max_index = 0; // reset indices only: nothing is drawn
        }
        swapped_indices = expansion_scratch.data();
        if (!topo.expand)
        {
          UploadSlice slice = dev.Upload(args.index_count * 4, 16);
          if (!slice.cpu)
          {
            SkipGeometry(GeometrySkip::kIndexUpload, r, "%u UP index bytes", args.index_count * 4);
            return false;
          }
          std::memcpy(slice.cpu, expansion_scratch.data(), size_t(args.index_count) * 4);
          ibv.BufferLocation = slice.gpu;
          ibv.SizeInBytes = args.index_count * 4;
          ibv.Format = DXGI_FORMAT_R32_UINT;
          use_index = true;
          draw_count = args.index_count;
          ++g_stats.index_uploads;
        }
        if (max_index - min_index + 1 > kMaxDrawVertices)
        {
          SkipGeometry(GeometrySkip::kIndexRange, r, "UP indices %u..%u (limit %u vertices)", min_index, max_index,
                       kMaxDrawVertices);
          return false;
        }
        if (FrameTraceActive())
        {
          REXLOG_INFO("pdz_trace:   UP indices {:08X} {}-bit count {} | indices {}..{} | vertices {:08X} stride {} "
                      "readable {}",
                      args.index_data, args.index32 ? 32 : 16, args.index_count, min_index, max_index, args.data,
                      args.stride, args.vertex_count);
        }
      }
      else if (args.indexed)
      {
        if (!shadow::IsGuestPointer(r.index_buffer))
        {
          SkipGeometry(GeometrySkip::kIndexBufferObject, r, "index buffer object %08X", r.index_buffer);
          return false;
        }
        // The index buffer object's header (view: the hook's copy with pdz_native_record_thread).
        const uint32_t ib_flags = view::Load32(base, r.index_buffer + 0);
        const uint32_t ib_base = view::Load32(base, r.index_buffer + shadow::dev::kVbBase) & 0x1FFFFFFEu;
        // Bit 31 of the index buffer's Common dword = 32-bit indices (INDEX32; the Datacore door / arch meshes use
        // them: read as 16-bit they exploded into screen-filling triangles, pass dump run4).
        const bool index32 = (ib_flags & 0x80000000u) != 0;
        const uint32_t index_size = index32 ? 4u : 2u;
        if (index32)
        {
          ++g_stats.index32_draws;
          if (!g_frame.logged_ib_flag)
          {
            g_frame.logged_ib_flag = true;
            static uint32_t logged = 0;
            if (logged++ < 3)
            {
              REXLOG_INFO("pdz_native: 32-bit indices: index buffer {:08X} Common {:08X} ({} indices)", r.index_buffer,
                          ib_flags, args.index_count);
            }
          }
        }
        const uint32_t ib_bytes = args.index_count * index_size;
        const uint8_t *src = PhysPtr(ib_base + args.start_index * index_size);
        if (!src || args.index_count > kMaxDrawVertices * 3)
        {
          SkipGeometry(GeometrySkip::kIndexMemory, r, "index base %08X start %u count %u%s", ib_base, args.start_index,
                       args.index_count, src ? "" : " (unreadable)");
          return false;
        }
        // PhysPtr masks only the first address: every byte read must lie inside the 512 MB physical view.
        if (uint64_t(ib_base) + (uint64_t(args.start_index) + args.index_count) * index_size > 0x20000000ull)
        {
          SkipGeometry(GeometrySkip::kPastPhysicalMemory, r, "index base %08X start %u count %u (%u-byte indices)",
                       ib_base, args.start_index, args.index_count, index_size);
          return false;
        }
        if (reset.enabled)
        {
          // Primitive reset (ResolveIndexReset): counted, the first few logged.
          ++g_stats.prim_reset_draws;
          const bool rewritten = reset.index != (index32 ? 0x00FFFFFFu : 0xFFFFu);
          g_stats.prim_reset_rewritten += rewritten ? 1 : 0;
          g_stats.prim_reset_widened += reset.widen ? 1 : 0;
          static uint32_t reset_logged = 0;
          if (reset_logged < 3 || (rewritten && reset_logged < 6))
          {
            ++reset_logged;
            REXLOG_INFO("pdz_native: primitive reset: {} draw #{} caller {:08X}: prim {} {}-bit indices x{} reset index "
                        "{:#x} -> {}",
                        DrawKindName(r.kind), r.sequence, r.caller, args.prim, index32 ? 32 : 16, args.index_count,
                        reset.index,
                        topo.expand ? "fans split in the expansion"
                                    : (reset.widen ? "widened to 32-bit, strip cut 0xFFFFFFFF"
                                                   : (index32 ? "strip cut 0xFFFFFFFF" : "strip cut 0xFFFF")));
          }
        }
        if (topo.expand || reset.widen)
        {
          // Swap into scratch, expand to 32-bit triangle indices (the quad / fan path is rare: UI, front end). A
          // widened primitive-reset strip goes up from here as 32-bit indices. Reset indices become kResetMarker
          // and stay out of the range.
          expansion_scratch.resize(args.index_count);
          min_index = 0xFFFFFFFFu;
          for (uint32_t i = 0; i < args.index_count; ++i)
          {
            const uint32_t v = index32 ? (uint32_t(src[4 * i]) << 24) | (uint32_t(src[4 * i + 1]) << 16) |
                                             (uint32_t(src[4 * i + 2]) << 8) | src[4 * i + 3]
                                       : uint32_t((uint16_t(src[2 * i]) << 8) | src[2 * i + 1]);
            if (reset.Is(v))
            {
              expansion_scratch[i] = kResetMarker;
              continue;
            }
            expansion_scratch[i] = v;
            min_index = std::min<uint32_t>(min_index, v);
            max_index = std::max<uint32_t>(max_index, v);
          }
          if (reset.enabled && min_index > max_index)
          {
            min_index = max_index = 0; // reset indices only: nothing is drawn
          }
          swapped_indices = expansion_scratch.data();
          if (reset.widen)
          {
            UploadSlice slice = dev.Upload(args.index_count * 4, 16);
            if (!slice.cpu)
            {
              SkipGeometry(GeometrySkip::kIndexUpload, r, "%u index bytes (widened)", args.index_count * 4);
              return false;
            }
            std::memcpy(slice.cpu, expansion_scratch.data(), size_t(args.index_count) * 4);
            ibv.BufferLocation = slice.gpu;
            ibv.SizeInBytes = args.index_count * 4;
            ibv.Format = DXGI_FORMAT_R32_UINT;
            use_index = true;
            draw_count = args.index_count;
          }
        }
        else
        {
          D3D12_GPU_VIRTUAL_ADDRESS gpu = 0;
          if (!UploadRange(dev, ib_base + args.start_index * index_size, src, ib_bytes, index32 ? 2 : 1, index_size, gpu,
                           &min_index, &max_index,
                           false, &reset))
          {
            SkipGeometry(GeometrySkip::kIndexUpload, r, "%u index bytes", ib_bytes);
            return false;
          }
          ibv.BufferLocation = gpu;
          ibv.SizeInBytes = ib_bytes;
          ibv.Format = index32 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
          use_index = true;
          draw_count = args.index_count;
        }
        if (max_index < min_index || max_index - min_index + 1 > kMaxDrawVertices)
        {
          SkipGeometry(GeometrySkip::kIndexRange, r, "indices %u..%u (limit %u vertices)", min_index, max_index,
                       kMaxDrawVertices);
          return false;
        }
      }
      else
      {
        min_index = 0;
        max_index = args.vertex_count - 1;
        draw_count = args.vertex_count;
      }

      if (topo.expand)
      {
        D3D12_GPU_VIRTUAL_ADDRESS gpu = 0;
        uint32_t count = 0;
        const uint32_t src_count = args.indexed ? args.index_count : args.vertex_count;
        // Indices relative to the uploaded vertex range (first uploaded vertex = min_index).
        if (!ExpandIndices(dev, args.prim, swapped_indices, src_count, 0, gpu, count, reset.enabled))
        {
          SkipGeometry(GeometrySkip::kExpandIndices, r, "prim %u count %u", args.prim, src_count);
          return false;
        }
        ibv.BufferLocation = gpu;
        ibv.SizeInBytes = count * 4;
        ibv.Format = DXGI_FORMAT_R32_UINT;
        use_index = true;
        draw_count = count;
      }

      if (FrameTraceActive() && args.indexed && !args.user_indices && shadow::IsGuestPointer(r.index_buffer))
      {
        const uint32_t ib = r.index_buffer;
        REXLOG_INFO("pdz_trace:   ib {:08X} dwords {:08X} {:08X} {:08X} {:08X} {:08X} | start {} count {} base_vertex {} | "
                    "indices {}..{} | stream0 stride {}",
                    ib, view::Load32(base, ib), view::Load32(base, ib + 4), view::Load32(base, ib + 8),
                    view::Load32(base, ib + 12), view::Load32(base, ib + 16), args.start_index, args.index_count,
                    args.base_vertex, min_index, max_index, r.streams[0].stride);
      }

      // Vertex streams: the range [first, last] of vertices this draw can touch, per stream the VS reads.
      const uint32_t first_vertex = args.indexed ? args.base_vertex + min_index : args.start_vertex;
      const uint32_t vertex_span = max_index - min_index + 1;
      base_vertex_location = args.indexed ? -int32_t(min_index) : 0;
      D3D12_VERTEX_BUFFER_VIEW vbv[16] = {};
      uint32_t vb_slots = 0;
      for (uint32_t s = 0; s < 16; ++s)
      {
        if (!(vs->stream_mask & (1u << s)))
        {
          continue;
        }
        uint32_t stride = 0;
        const uint8_t *src = nullptr;
        uint32_t guest_address = 0;
        uint32_t endian = uint32_t(xenos::Endian::k8in32);
        uint32_t available = 0;
        // UP / BeginVertices data replaces stream 0 only; any other stream the VS reads comes from the vertex
        // buffer set with SetStreamSource (log 034/038: 40 such draws a frame in Datacore were skipped).
        const bool up_stream = args.user_data && s == 0;
        if (up_stream)
        {
          stride = args.stride;
          src = shadow::GuestPtr(base, args.data);
          guest_address = args.data;
          available = args.vertex_count * stride;
        }
        else
        {
          const shadow::Stream &st = r.streams[s];
          if (!shadow::IsGuestPointer(st.vb))
          {
            // No vertex buffer on a stream the VS reads (log 039: the second stream of 40 UP draws a frame): a
            // null view, so the fetch reads zeros, instead of skipping the draw. Counted under its reason.
            const uint64_t n = g_stats.geometry_reasons[uint32_t(GeometrySkip::kStreamNoBuffer)]++;
            if (n < 8)
            {
              // Names the VS variant (hash, object, declaration) and the stream's recorded VB / fetch dwords, so
              // the next run says which pair it is (logs 069-090: VS 5cc84ad7 decl 40D5AA40 resolved to its
              // unpatched container, see pdz_native_vs_blank_copy_unbound).
              REXLOG_INFO("pdz_native: stream {} has no vertex buffer (VS {:016x} object {:08X} decl {:08X} stream mask "
                          "{:08X}, {} prim {} draw #{} caller {:08X}; VB {:08X} fetch {:08X} {:08X} stride {} "
                          "offset {}); bound as a null view (reads zero)",
                          s, vs->hash, r.vertex_shader, r.vertex_decl, vs->stream_mask, DrawKindName(r.kind),
                          uint32_t(args.prim), r.sequence, r.caller, r.streams[s].vb, r.streams[s].base, r.streams[s].size,
                          r.streams[s].stride, r.streams[s].offset);
            }
            vbv[s] = {};
            vb_slots = s + 1;
            continue;
          }
          // VB object +12 = GPU fetch dword 0 (byte address | type bits), +16 = fetch dword 1 (byte size |
          // endian bits); SetStreamSource's offset is added to both (sub_826F9578).
          const uint32_t type = st.base & 3u;
          const uint32_t phys = (st.base & 0x1FFFFFFCu) + st.offset;
          endian = st.size & 3u;
          const uint32_t size_bytes = st.size & 0x03FFFFFCu;
          if (type != 3u && !g_frame.logged_stream_type)
          {
            g_frame.logged_stream_type = true;
            REXLOG_WARN("pdz_native: stream {} VB {:08X}: fetch dword0 type bits = {} (expected 3 = vertex), "
                        "dword1 = {:08X}; using the address anyway",
                        s, st.vb, type, st.size);
          }
          if (!g_frame.logged_endian[endian])
          {
            g_frame.logged_endian[endian] = true;
            REXLOG_INFO("pdz_native: vertex stream endian {} first seen (VB {:08X}, size {:08X})", endian, st.vb, st.size);
          }
          stride = st.stride ? st.stride : vs->stream_stride[s];
          if (size_bytes <= st.offset)
          {
            SkipGeometry(GeometrySkip::kStreamOffsetPastEnd, r, "stream %u VB %08X size %u offset %u", s, st.vb,
                         size_bytes, st.offset);
            return false;
          }
          available = size_bytes - st.offset;
          src = PhysPtr(phys);
          guest_address = phys;
        }
        if (!stride || !src)
        {
          SkipGeometry(GeometrySkip::kStreamNoStrideOrMemory, r, "stream %u stride %u memory %s", s, stride,
                       src ? "ok" : "unreadable");
          return false;
        }
        const uint64_t range_start = uint64_t(first_vertex) * stride;
        uint64_t range_end = uint64_t(first_vertex + vertex_span) * stride;
        if (range_start >= available)
        {
          SkipGeometry(GeometrySkip::kRangePastBuffer, r, "stream %u first vertex %u stride %u -> byte %llu, %u available",
                       s, first_vertex, stride, (unsigned long long)range_start, available);
          return false;
        }
        range_end = std::min<uint64_t>(range_end, available);
        if (!up_stream && uint64_t(guest_address) + range_end > 0x20000000ull)
        {
          // The read would cross the end of the 512 MB physical view (PhysPtr masks only the first address).
          SkipGeometry(GeometrySkip::kPastPhysicalMemory, r, "stream %u address %08X bytes %llu..%llu", s,
                       guest_address, (unsigned long long)range_start, (unsigned long long)range_end);
          return false;
        }
        const uint32_t bytes = uint32_t(range_end - range_start);
        D3D12_GPU_VIRTUAL_ADDRESS gpu = 0;
        if (up_stream)
        {
          // Dynamic data: straight into the frame upload, never cached.
          UploadSlice slice = dev.Upload(bytes, 16);
          if (!slice.cpu)
          {
            SkipGeometry(GeometrySkip::kVertexUpload, r, "UP data %u bytes", bytes);
            return false;
          }
          // pdz_native_record_thread: the hook's copy of the range (the game's memory for DrawVerticesUP /
          // DrawIndexedVerticesUP, rewritten after the call; BeginVertices data lives in the ring: guest memory).
          const uint8_t *up_src = view::Ptr(base, args.data + uint32_t(range_start), bytes);
          SwapCopy(static_cast<uint8_t *>(slice.cpu), up_src, bytes, endian);
          LogUpVertices(r, args, static_cast<const uint8_t *>(slice.cpu), bytes,
                        device_override ? device_override : view::Ptr(base, r.device, shadow::dev::kDeviceSize));
          gpu = slice.gpu;
          ++g_stats.vertex_uploads;
          g_stats.vertex_bytes += bytes;
        }
        else if (!UploadRange(dev, uint32_t(guest_address + range_start), src + range_start, bytes, endian, 0, gpu,
                              nullptr, nullptr, true))
        {
          SkipGeometry(GeometrySkip::kVertexUpload, r, "stream %u %u bytes", s, bytes);
          return false;
        }
        vbv[s].BufferLocation = gpu;
        vbv[s].SizeInBytes = bytes;
        vbv[s].StrideInBytes = stride;
        vb_slots = s + 1;
      }
      Lap(kStageGeometry);

      // Constants.
      // The device struct: guest memory, or the record thread's mirror (native_view.h).
      const uint8_t *dev_ptr = device_override ? device_override : view::Ptr(base, r.device, shadow::dev::kDeviceSize);
      const bool literals = REXCVAR_GET(pdz_native_literals);
      const bool no_cb_reuse = REXCVAR_GET(pdz_native_debug_no_cb_reuse);
      bool vs_cb_reused = false, ps_cb_reused = false; // draw dump
      if (!g_frame.vs_cb || r.dirty[0] != 0 || g_frame.vs_cb_shader != vs || no_cb_reuse)
      {
        g_frame.vs_cb = UploadFloatConstants(dev, dev_ptr, shadow::dev::kVsAluConstants, 256, vs, literals,
                                             &g_frame.vs_cb_cpu);
        g_frame.vs_cb_shader = vs;
      }
      else
      {
        ++g_stats.constant_reuse;
        vs_cb_reused = true;
      }
      if (!g_frame.ps_cb || r.dirty[1] != 0 || g_frame.ps_cb_shader != ps || no_cb_reuse)
      {
        g_frame.ps_cb = UploadFloatConstants(dev, dev_ptr, shadow::dev::kPsAluConstants, kPsFloatCount, ps, literals,
                                             &g_frame.ps_cb_cpu);
        g_frame.ps_cb_shader = ps;
      }
      else
      {
        ++g_stats.constant_reuse;
        ps_cb_reused = true;
      }
      Lap(kStageCbFloat);
      const D3D12_GPU_VIRTUAL_ADDRESS bool_loop_cb = UploadBoolLoop(dev, r, dev_ptr, vs, ps, literals);
      D3D12_GPU_VIRTUAL_ADDRESS system_cb = 0;
      const AlphaTest alpha_test = AlphaTestFor(rs); // RB_COLORCONTROL alpha test + alpha-to-mask (native_pipeline.h)
      {
        // Built in cached memory, then copied up whole (or the previous identical upload reused: UploadSmallCb).
        SystemConstants system_local{};
        {
          SystemConstants *sc = &system_local;
          sc->alpha_func = alpha_test.func; // 0 = off, else D3DCMPFUNC = xenos + 1
          sc->alpha_ref = alpha_test.ref;
          reg::PA_SU_POINT_SIZE psz;
          psz.value = rs.point_size;
          sc->point_size[0] = float(psz.width) / 16.0f;  // 12.4 fixed point, half size in guest pixels
          sc->point_size[1] = float(psz.height) / 16.0f;
          // Guest pixels -> host NDC for the point GS (the host viewport is in host pixels, scale x guest).
          sc->point_size[2] = vp.Width > 0.0f ? 2.0f * float(tf.resolution_scale) / vp.Width : 0.0f;
          sc->point_size[3] = vp.Height > 0.0f ? 2.0f * float(tf.resolution_scale) / vp.Height : 0.0f;
          sc->flags = hv.vte_flags;
          // pdz_native_scale > 1 only (stays 0 at scale 1): the resolution scale in g_Reserved.x, for a translated
          // pixel position (PsParamGen) to be divided by it after flooring, as the SDK does; no shader reads it yet.
          if (tf.resolution_scale > 1)
          {
            sc->pad[0] = tf.resolution_scale;
          }
          sc->pad[1] = WaterLoopMask(r, ps, vs, dev_ptr); // g_Reserved.y: stacked slots whose layers wrap (pdz_fix_water_loop)
          for (uint32_t i = 0; i < 3; ++i)
          {
            sc->ndc_scale[i] = hv.ndc_scale[i];
            sc->ndc_offset[i] = hv.ndc_offset[i];
          }
          // User clip planes for the clip-plane GS (pdz_native_clip_planes; read only when the PSO has it).
          if (const uint32_t ucp = UserClipPlaneMask(rs))
          {
            UserClipPlanesToHost(r, ucp, hv, sc->clip_planes);
          }
        }
        system_cb = UploadSmallCb(dev, system_local, g_frame.last_system, g_frame.last_system_gpu, g_small_cb.system);
      }
      if (!g_frame.vs_cb || !g_frame.ps_cb || !bool_loop_cb || !system_cb)
      {
        ++g_stats.skipped_off;
        return false;
      }

      // Descriptor tables.
      Lap(kStageConstants);
      D3D12_GPU_DESCRIPTOR_HANDLE srv_table{}, vs_samplers{}, ps_samplers{};
      const bool srv_bound = BindSrvTable(dev, r, vs, ps, srv_table);
      Lap(kStageTables);
      if (!srv_bound || !BindSamplerTable(dev, r, vs, vs_samplers) || !BindSamplerTable(dev, r, ps, ps_samplers))
      {
        ++g_stats.skipped_descriptors; // a descriptor ring is full (warned once per frame by the device)
        return false;
      }
      // 7e3 blend mode 2 (native_blend7e3.h): render target 0 leaves the output merger and becomes the pixel shader's
      // rasterizer-ordered view (u0); the depth target stays bound. The next draw's BindTargets transitions it back.
      D3D12_GPU_DESCRIPTOR_HANDLE rov_table{};
      if (pipe->blend7e3 == blend7e3::kRov)
      {
        HostSurface *rov_surface = bound.rt[0];
        const Descriptor u = (rov_surface && rov_surface->uav_7e3) ? dev.AllocSrv(1) : Descriptor{};
        if (!u.cpu.ptr)
        {
          ++g_stats.skipped_descriptors;
          return false;
        }
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc{};
        uav_desc.Format = rov_surface->view_format; // R16G16B16A16_FLOAT
        uav_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        dev.device()->CreateUnorderedAccessView(rov_surface->resource, nullptr, &uav_desc, u.cpu);
        Surfaces::Get().Transition(*rov_surface, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list->OMSetRenderTargets(0, nullptr, FALSE, bound.ds ? &bound.ds->view : nullptr);
        Surfaces::Get().ForgetBoundTargets(); // pdz_native_state_filter: the next BindTargets sets its targets again
        rov_table = u.gpu;
      }

      // Record.
      Lap(kStageSamplers);
      if (g_frame.list_epoch != dev.list_epoch())
      {
        g_frame.list_epoch = dev.list_epoch();
        g_frame.root_signature_set = false;
        g_frame.last_pso = nullptr;
        g_frame.ls = {}; // pdz_native_state_filter: a new / reset list or a pass's own pipeline: nothing known
      }
      if (!g_frame.root_signature_set)
      {
        list->SetGraphicsRootSignature(pipelines.root_signature());
        g_frame.root_signature_set = true;
        g_frame.ls = {}; // the root arguments are bound anew after a root signature is set
      }
      bool ei_break = false, ei_break_bindless = false; // docs/EXECUTE_INDIRECT.md counters
      if (g_frame.last_pso != pipe->pso)
      {
        list->SetPipelineState(pipe->pso);
        g_frame.last_pso = pipe->pso;
        ++g_ei.pso;
        ei_break = ei_break_bindless = true;
      }
      // pdz_native_state_filter (stage 2 M5 drawcost3): a state call whose value equals what the list has (this list
      // epoch, set by the draw path) is not made again. The list state (ls) is kept either way.
      FrameState::ListState &ls = g_frame.ls;
      const bool filter = REXCVAR_GET(pdz_native_state_filter);
      uint32_t calls_skipped = 0;
      if (!filter || !ls.topology_valid || ls.topology != topo.topology)
      {
        list->IASetPrimitiveTopology(topo.topology);
        ls.topology = topo.topology;
        ls.topology_valid = true;
        ++g_ei.topology;
        ei_break = ei_break_bindless = true;
      }
      else
      {
        ++calls_skipped;
      }
      if (!filter || !ls.viewport_valid || std::memcmp(&ls.viewport, &vp, sizeof(vp)) != 0)
      {
        list->RSSetViewports(1, &vp);
        ls.viewport = vp;
        ls.viewport_valid = true;
        ++g_ei.viewport;
        ei_break = ei_break_bindless = true;
      }
      else
      {
        ++calls_skipped;
      }
      if (!filter || !ls.scissor_valid || std::memcmp(&ls.scissor, &scissor, sizeof(scissor)) != 0)
      {
        list->RSSetScissorRects(1, &scissor);
        ls.scissor = scissor;
        ls.scissor_valid = true;
        ei_break = ei_break_bindless = true;
      }
      else
      {
        ++calls_skipped;
      }
      if (pipe->stencil)
      {
        reg::RB_STENCILREFMASK srm;
        srm.value = rs.stencilrefmask;
        const UINT ref = srm.stencilref;
        if (!filter || !ls.stencil_valid || ls.stencil_ref != ref)
        {
          list->OMSetStencilRef(ref);
          ls.stencil_ref = ref;
          ls.stencil_valid = true;
          ++g_ei.stencil;
          ei_break = ei_break_bindless = true;
        }
        else
        {
          ++calls_skipped;
        }
      }
      if (pipe->uses_blend_factor)
      {
        if (!filter || !ls.blend_valid || std::memcmp(ls.blend_factor, rs.blend_factor, sizeof(ls.blend_factor)) != 0)
        {
          list->OMSetBlendFactor(rs.blend_factor);
          std::memcpy(ls.blend_factor, rs.blend_factor, sizeof(ls.blend_factor));
          ls.blend_valid = true;
          ++g_ei.blend;
          ei_break = ei_break_bindless = true;
        }
        else
        {
          ++calls_skipped;
        }
      }
      if (pipe->shader_depth_bias)
      {
        // SV_Depth 20e4 variant: this draw's polygon offset (front / back) for the shader (native_depth20e4.h).
        float depth_constants[kRootConstDepthCount];
        depth20e4::Constants(rs, topo.type == D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE, tf.resolution_scale,
                             depth_constants);
        list->SetGraphicsRoot32BitConstants(kRootConstDepth, kRootConstDepthCount, depth_constants, 0);
        depth20e4::CountDraw();
        ++g_ei.root_constants;
        ei_break = ei_break_bindless = true;
      }
      if (rov_table.ptr)
      {
        // 7e3 blend mode 2: the surface view and this draw's blend state for the shader's output merger.
        static_assert(blend7e3::kConstantCount == kRootConstBlend7e3Count);
        uint32_t blend_constants[blend7e3::kConstantCount];
        blend7e3::Constants(rs, blend_constants);
        list->SetGraphicsRootDescriptorTable(kRootUavEdram7e3, rov_table);
        list->SetGraphicsRoot32BitConstants(kRootConstBlend7e3, kRootConstBlend7e3Count, blend_constants, 0);
        ++g_ei.root_constants;
        ei_break = ei_break_bindless = true;
      }
      if (pipe->blend7e3)
      {
        blend7e3::CountDraw(pipe->blend7e3);
      }
      {
        static_assert(kRootCbvFloatVs == 0 && kRootCbvFloatPs == 1 && kRootCbvBoolLoop == 2 && kRootCbvSystem == 3,
                      "ListState::cbv is indexed by the root parameter");
        const D3D12_GPU_VIRTUAL_ADDRESS cbvs[4] = {g_frame.vs_cb, g_frame.ps_cb, bool_loop_cb, system_cb};
        for (uint32_t i = 0; i < 4; ++i)
        {
          if (!filter || ls.cbv[i] != cbvs[i])
          {
            list->SetGraphicsRootConstantBufferView(i, cbvs[i]);
            ls.cbv[i] = cbvs[i];
            ++(i == 0 ? g_ei.cbv_vs : i == 1 ? g_ei.cbv_ps : g_ei.cbv_other);
          }
          else
          {
            ++calls_skipped;
          }
        }
        const uint32_t table_params[3] = {kRootSrvTable, kRootSamplerVs, kRootSamplerPs};
        const D3D12_GPU_DESCRIPTOR_HANDLE tables[3] = {srv_table, vs_samplers, ps_samplers};
        for (uint32_t i = 0; i < 3; ++i)
        {
          if (!filter || ls.table[i] != tables[i].ptr)
          {
            list->SetGraphicsRootDescriptorTable(table_params[i], tables[i]);
            ls.table[i] = tables[i].ptr;
            ++(i == 0 ? g_ei.srv_table : g_ei.sampler_table);
            ei_break = true;
          }
          else
          {
            ++calls_skipped;
          }
        }
      }
      if (vb_slots)
      {
        if (!filter || !ls.vb_valid || ls.vb_slots != vb_slots ||
            std::memcmp(ls.vbv, vbv, sizeof(D3D12_VERTEX_BUFFER_VIEW) * vb_slots) != 0)
        {
          list->IASetVertexBuffers(0, vb_slots, vbv);
          ++g_ei.vb;
          ls.vb_slots = vb_slots;
          std::memcpy(ls.vbv, vbv, sizeof(D3D12_VERTEX_BUFFER_VIEW) * vb_slots);
          ls.vb_valid = true;
        }
        else
        {
          ++calls_skipped;
        }
      }
      // Frame trace: pipeline statistics + occlusion per draw (resolved and logged at the end of the frame).
      int32_t trace_query = -1;
      if (FrameTraceActive() && InitTraceQueries(dev))
      {
        if (g_tq.frame != dev.frame_index())
        {
          g_tq.frame = dev.frame_index();
          g_tq.count = 0;
        }
        if (g_tq.count < TraceQueries::kMax)
        {
          trace_query = int32_t(g_tq.count++);
          g_tq.sequence[trace_query] = r.sequence;
          list->BeginQuery(g_tq.stats, D3D12_QUERY_TYPE_PIPELINE_STATISTICS, UINT(trace_query));
          list->BeginQuery(g_tq.occlusion, D3D12_QUERY_TYPE_OCCLUSION, UINT(trace_query));
        }
      }
      if (use_index)
      {
        if (!filter || !ls.ib_valid || std::memcmp(&ls.ibv, &ibv, sizeof(ibv)) != 0)
        {
          list->IASetIndexBuffer(&ibv);
          ++g_ei.ib;
          ls.ibv = ibv;
          ls.ib_valid = true;
        }
        else
        {
          ++calls_skipped;
        }
        list->DrawIndexedInstanced(draw_count, 1, 0, base_vertex_location, 0);
      }
      else
      {
        list->DrawInstanced(draw_count, 1, 0, 0);
      }
      ++g_ei.draws;
      g_ei.runs_now += ei_break ? 1 : 0;
      g_ei.runs_bindless += ei_break_bindless ? 1 : 0;
      if (trace_query >= 0)
      {
        list->EndQuery(g_tq.occlusion, D3D12_QUERY_TYPE_OCCLUSION, UINT(trace_query));
        list->EndQuery(g_tq.stats, D3D12_QUERY_TYPE_PIPELINE_STATISTICS, UINT(trace_query));
      }
      g_state_filter.considered += 3u + (pipe->stencil ? 1u : 0u) + (pipe->uses_blend_factor ? 1u : 0u) + 7u +
                                   (vb_slots ? 1u : 0u) + (use_index ? 1u : 0u);
      g_state_filter.skipped += calls_skipped;
      Lap(kStageRecord);
      ++g_stats.issued;
      g_stats.indexed_up_draws += args.user_indices ? 1 : 0;
      if (passdump::Active())
      {
        passdump::RecordDraw(r.sequence, args.prim, args.indexed ? args.index_count : args.vertex_count, vs->hash,
                             dev_ptr + shadow::dev::kVsAluConstants);
      }
      passdump::CountDraw();
      if (dumped)
      {
        // Draw dump: what this draw was drawn with (the constant buffers the GPU reads, state, vertices); colour
        // target 0 after it for the picked draw (flushes; the next draw re-binds everything).
        drawdump::DrawInfo di;
        di.vs = vs;
        di.ps = ps;
        di.device = dev_ptr;
        di.vs_cb = g_frame.vs_cb_cpu;
        di.ps_cb = g_frame.ps_cb_cpu;
        di.vs_cb_reused = vs_cb_reused;
        di.ps_cb_reused = ps_cb_reused;
        di.literals = literals;
        di.prim = args.prim;
        di.guest_count = args.indexed ? args.index_count : args.vertex_count;
        di.host_count = draw_count;
        di.user_data = args.user_data;
        di.data = args.data;
        di.stride = args.stride;
        di.vertex_count = args.vertex_count;
        di.rs = &rs;
        di.hv = &hv;
        di.scissor = scissor;
        di.alpha_func = alpha_test.func;
        di.alpha_ref = alpha_test.ref;
        di.rt0 = bound.rt[0];
        di.pso = static_cast<const void *>(pipe->pso);
        drawdump::AfterDraw(base, r, di);
      }
      // Loop diagnostic (draw-stats line): issued draws whose VS / PS loops over a constant with count 0.
      if ((vs->loop_constants_used | ps->loop_constants_used) != 0)
      {
        ++g_stats.loop_draws;
        const bool vs_zero = ZeroCountLoops(dev_ptr, vs, vs, ps, literals) != 0;
        const bool ps_zero = ZeroCountLoops(dev_ptr, ps, vs, ps, literals) != 0;
        g_stats.loop_zero_draws += vs_zero || ps_zero;
        g_stats.loop_zero_vs += vs_zero;
        g_stats.loop_zero_ps += ps_zero;
      }
      if (topo.rects && g_stats.rectlist_draws++ < 3)
      {
        REXLOG_INFO("pdz_native: RECTLIST draw expanded by the rectangle-list GS ({} draw #{} caller {:08X}, {} vertices)",
                    DrawKindName(r.kind), r.sequence, r.caller, draw_count);
      }
      // Alpha-to-mask (draw-stats line; the first 8 distinct RB_COLORCONTROL values are logged with what the PS
      // alpha test became). Without colour output 0 the PS runs no alpha test, as the SDK skips alpha-to-mask.
      if (alpha_test.alpha_to_mask)
      {
        ++g_stats.alpha_to_mask_draws;
        g_stats.alpha_to_mask_dithered += alpha_test.dithered;
        g_stats.alpha_to_mask_inexact += alpha_test.inexact;
        g_stats.alpha_to_mask_no_oc0 += (ps->color_outputs & 1u) == 0;
        static uint32_t a2m_seen[8] = {};
        static uint32_t a2m_seen_count = 0;
        bool a2m_new = a2m_seen_count < 8;
        for (uint32_t i = 0; a2m_new && i < a2m_seen_count; ++i)
        {
          a2m_new = a2m_seen[i] != rs.colorcontrol;
        }
        if (a2m_new)
        {
          a2m_seen[a2m_seen_count++] = rs.colorcontrol;
          REXLOG_INFO("pdz_native: alpha-to-mask RB_COLORCONTROL {:08X} (offsets {} {} {} {}, alpha test {}, ref "
                      "{:.4f}) -> PS alpha test func {} ref {:.4f}{}{}{} ({} draw #{} caller {:08X})",
                      rs.colorcontrol, (rs.colorcontrol >> 24) & 3u, (rs.colorcontrol >> 26) & 3u,
                      (rs.colorcontrol >> 28) & 3u, (rs.colorcontrol >> 30) & 3u,
                      (rs.colorcontrol & 8u) ? (rs.colorcontrol & 7u) + 1u : 0u, rs.alpha_ref, alpha_test.func,
                      alpha_test.ref, alpha_test.dithered ? " [dithered: mean threshold]" : "",
                      alpha_test.inexact ? " [inexact]" : "", (ps->color_outputs & 1u) ? "" : " [no oC0]",
                      DrawKindName(r.kind), r.sequence, r.caller);
        }
      }
      if (FrameTraceActive())
      {
        for (const NativeShader *s : {vs, ps})
        {
          for (uint32_t m = s->loop_constants_used; m != 0; m &= m - 1)
          {
            const uint32_t i = uint32_t(__builtin_ctz(m));
            const uint32_t v = LoopConstantValue(dev_ptr, i, vs, ps, literals);
            REXLOG_INFO("pdz_trace:   {} loop i{} (reg 0x{:04X}, device +{}) = {:08X}: count {} start {} step {}{}",
                        s == vs ? "vs" : "ps", i, 0x4908u + i, kDevLoopConstants + 4 * i, v, v & 0xFFu,
                        (v >> 8) & 0xFFu, int32_t(v << 8) >> 24, (v & 0xFFu) ? "" : " COUNT-0 (loop skipped)");
          }
          for (const ShaderTexture &t : s->textures)
          {
            const FetchDecoded f = DecodeFetch(r.fetch_be + t.slot * shadow::dev::kFetchConstantBytes);
            static const uint32_t kExpected[3] = {1, 2, 3};
            REXLOG_INFO("pdz_trace:   {} tex slot {} dim {} -> {:08X} fmt {} {}x{}x{} fetch-dim {}{} pitch {} {} endian {} "
                        "swizzle {:o} signs {} {}{}",
                        s == vs ? "vs" : "ps", t.slot, uint32_t(t.dim), f.base_address, f.format, f.width, f.height,
                        f.depth, f.dimension, f.stacked ? " stacked" : "", f.pitch, f.tiled ? "tiled" : "linear",
                        f.endian, f.swizzle, f.signs, Textures::Get().FindMirror(f) ? "MIRROR" : "upload",
                        f.dimension != kExpected[uint32_t(t.dim)] ? " KIND-MISMATCH" : "");
          }
        }
        auto surf = [](const HostSurface *h) {
          return h ? TraceSurface(h->guest, h->width, h->height, h->depth ? h->depth_format : h->color_format,
                                  h->edram_base, h->depth)
                   : std::string("-");
        };
        REXLOG_INFO("pdz_trace: draw #{} {} prim {} count {} | rt0 {} rt1 {} ds {} | vp {:.0f},{:.0f} {:.0f}x{:.0f} z "
                    "{:.3f}..{:.3f} scissor {},{}..{},{} | ndc scale {:.4f} {:.4f} {:.4f} off {:.4f} {:.4f} {:.4f} vte "
                    "{} clip {:08X} | depthcontrol {:08X} blend0 {:08X} colormask {:08X} | vs {:016x} ps {:016x} tex "
                    "{}+{} | pso {}",
                    r.sequence, DrawKindName(r.kind), args.prim, draw_count, surf(bound.rt[0]), surf(bound.rt[1]),
                    surf(bound.ds), vp.TopLeftX, vp.TopLeftY, vp.Width, vp.Height, vp.MinDepth, vp.MaxDepth,
                    scissor.left, scissor.top, scissor.right, scissor.bottom, hv.ndc_scale[0], hv.ndc_scale[1],
                    hv.ndc_scale[2], hv.ndc_offset[0], hv.ndc_offset[1], hv.ndc_offset[2], hv.vte_flags, rs.clip_cntl,
                    rs.depthcontrol, rs.blendcontrol[0], rs.color_mask, vs->hash, ps->hash, vs->textures.size(),
                    ps->textures.size(), static_cast<const void *>(pipe->pso));
      }
      return true;
    }

    // IssueDraw with the pdz_native_draw_timing stages: what follows the last Lap counts as the tail, or as
    // "skipped" for a draw that returned early.
    bool TimedIssueDraw(uint8_t *base, const shadow::DrawRecord &r, const DrawArgs &args,
                        const uint8_t *device_override = nullptr)
    {
      g_perf.on = REXCVAR_GET(pdz_native_draw_timing);
      if (pdz::timing::PhaseLevel() > 0)
      {
        pdz::timing::CountDrawLocked(); // the per-draw cost in the phases line (drawcost)
      }
      // pdz_frame_timing_phases 2: the stages as frame timing phases (inside the caller's draw phase scope).
      // Only inside an active draw scope (the level can change at a Swap between that scope's start and here).
      g_perf.phases = pdz::timing::PhaseLevel() >= 2 &&
                      pdz::timing::t_phase.current == uint32_t(pdz::timing::Phase::kDraw);
      if (g_perf.phases)
      {
        pdz::timing::Lap(pdz::timing::Phase::kDraw, pdz::timing::Phase::kDrawShaders); // the prologue so far: draw
      }
      if (!g_perf.on && !g_perf.phases)
      {
        return IssueDraw(base, r, args, device_override);
      }
      if (!g_perf.on)
      {
        const bool issued = IssueDraw(base, r, args, device_override);
        Lap(issued ? kStageTail : kStageSkipped);
        g_perf.phases = false;
        return issued;
      }
      ++g_perf.draws;
      g_perf.last = __rdtsc();
      const bool issued = IssueDraw(base, r, args, device_override);
      Lap(issued ? kStageTail : kStageSkipped);
      g_perf.on = false;
      g_perf.phases = false;
      return issued;
    }
  } // namespace

  bool OnDraw(uint8_t *base, const shadow::DrawRecord &r)
  {
    const pdz::timing::Scope timing(pdz::timing::Bucket::kNative); // pdz_frame_timing
    if (!Enabled())
    {
      return false;
    }
    RenderLock lock(RenderMutex());
    const pdz::timing::PhaseScope phase(pdz::timing::Phase::kDraw); // pdz_frame_timing_phases
    NoteRenderThread("draw");
    if (r.kind == shadow::Kind::kDrawIndexedVerticesUP && !REXCVAR_GET(pdz_native_indexed_up))
    {
      ++g_stats.indexed_up_off; // A/B: not drawn, as before the hook existed
      return false;
    }
    FlushPendingBegin(base); // a batch ended without EndVertices is complete once the game calls D3D again
    DrawArgs args;
    if (!ArgsFor(r, args))
    {
      if (r.kind == shadow::Kind::kDrawIndexedVertices || r.kind == shadow::Kind::kDrawVertices ||
          r.kind == shadow::Kind::kDrawVerticesUP || r.kind == shadow::Kind::kDrawIndexedVerticesUP)
      {
        SkipGeometry(GeometrySkip::kDrawArguments, r, "count / stride / data pointer");
      }
      return false;
    }
    if (r.kind == shadow::Kind::kBeginVertices)
    {
      // D3DDevice_DrawVerticesUP is BeginVertices + memcpy + an inlined EndVertices store, so its inner
      // BeginVertices reaches this hook too; the DrawVerticesUP hook has already issued that draw from its own
      // record. Keeping the inner batch pending would draw every UP batch a second time at the next entry point.
      if (r.caller == kDrawVerticesUpBeginReturn && REXCVAR_GET(pdz_native_up_begin_once))
      {
        ++g_stats.up_inner_begin;
        return false;
      }
      // The game fills the returned ring pointer after BeginVertices returns; draw at EndVertices. The hook
      // patches r.a[3] after forwarding, so keep the ring record itself (static storage), not a copy.
      // pdz_native_record_thread: the command comes after the patch, and the shadow ring slot is reused once the
      // command is done, so the batch keeps a copy; the device bytes come from the worker's mirror.
      std::memcpy(g_begin_device, view::Ptr(base, r.device, kBeginDeviceBytes), kBeginDeviceBytes);
      if (recthread::Active())
      {
        g_begin_copy = r;
        g_frame.begin_record = &g_begin_copy;
      }
      else
      {
        g_frame.begin_record = &r;
      }
      return false;
    }
    return TimedIssueDraw(base, r, args);
  }

  void OnEndVertices(uint8_t *base)
  {
    const pdz::timing::Scope timing(pdz::timing::Bucket::kNative); // pdz_frame_timing
    RenderLock lock(RenderMutex());
    if (Enabled())
    {
      const pdz::timing::PhaseScope phase(pdz::timing::Phase::kDraw); // pdz_frame_timing_phases
      FlushPendingBeginNow(base);
    }
  }

  void InvalidateConstantBuffers()
  {
    g_frame.vs_cb = 0;
    g_frame.ps_cb = 0;
  }

  void OnResolveInto(HostTexture &mirror, bool depth, uint32_t source)
  {
    NativeDevice *dev = NativeDevice::Existing();
    if (!dev)
    {
      return;
    }
    const uint64_t frame = dev->frame_index();
    if (g_split.frame != frame)
    {
      g_split.frame = frame;
      g_split.scene_passes = 1;
    }
    if (depth)
    {
      // Scene passes: every single-view frame resolves its depth surfaces into six different textures (four shadow
      // cascades, two of the scene depth); another view's pass resolves into the same ones again.
      if (mirror.depth_resolve_frame != frame)
      {
        mirror.depth_resolve_frame = frame;
        mirror.depth_resolves = 0;
      }
      if (++mirror.depth_resolves > g_split.scene_passes)
      {
        g_split.scene_passes = mirror.depth_resolves;
        g_split.max_passes_window = std::max(g_split.max_passes_window, g_split.scene_passes);
        g_split.multi_frames += g_split.scene_passes == 2 ? 1 : 0;
        const bool was_active = SplitActiveAt(frame);
        g_split.multi_seen = true;
        g_split.last_multi_frame = frame;
        if (!was_active && g_split.announce_logged < 16)
        {
          ++g_split.announce_logged;
          g_split.announced = true;
          REXLOG_INFO("pdz_native: split screen: frame {} has a scene pass {} (depth surface {:08X} resolved again into "
                      "{:08X} {}x{}); per-pass caches from here: pdz_native_split_tables {}, pdz_native_split_geometry {} "
                      "(held {} frames after the last such frame)",
                      frame, g_split.scene_passes, source, mirror.base_address, mirror.key_width, mirror.key_height,
                      REXCVAR_GET(pdz_native_split_tables), REXCVAR_GET(pdz_native_split_geometry), kSplitHoldFrames);
        }
        if (SegmentsActive(frame))
        {
          StartSegment(frame); // the new pass reuses nothing the previous one built
        }
        return;
      }
    }
    // A table of this segment samples the texture just written: the draws after the resolve must not reuse it.
    if (mirror.table_segment == g_split.segment)
    {
      const bool fixed = SegmentsActive(frame);
      // Each such texture once (the first 32): which of the game's re-resolved textures this concerns.
      static uint32_t logged_bases[32] = {};
      static uint32_t logged = 0;
      if (logged < 32 &&
          std::find(logged_bases, logged_bases + logged, mirror.base_address) == logged_bases + logged)
      {
        logged_bases[logged++] = mirror.base_address;
        REXLOG_INFO("pdz_native: resolve into {:08X} {}x{} (frame {}), which a texture table built earlier in this pass "
                    "samples: {}",
                    mirror.base_address, mirror.key_width, mirror.key_height, frame,
                    fixed ? "the draws after it build their tables again (barrier back to shader read; "
                            "pdz_fix_resolve_table_reuse)"
                          : "the draws after it may reuse that table and sample it in the render-target state "
                            "(undefined: garbage on AMD)");
      }
      if (fixed)
      {
        StartSegment(frame);
      }
    }
  }

  bool OnTextureRewritten(const HostTexture &t)
  {
    // table_segment is the segment of the last table that bound it; every segment of the current epoch is at least
    // table_epoch (segments only grow), and a new frame starts a new epoch.
    if (!REXCVAR_GET(pdz_native_texture_lock_tables) || t.table_segment < g_split.table_epoch)
    {
      return false; // no table built since the epoch began binds it: nothing reusable holds the old version
    }
    ++g_split.segment;
    g_split.table_epoch = g_split.segment;
    return true;
  }

  bool SplitActive()
  {
    const NativeDevice *dev = NativeDevice::Existing();
    return dev && SplitActiveAt(dev->frame_index());
  }

  void FlushPendingBegin(uint8_t *base)
  {
    // Every entry point but EndVertices: the batch was ended by an inlined EndVertices store (or not at all).
    if (!REXCVAR_GET(pdz_native_pending_begin_late))
    {
      return;
    }
    FlushPendingBeginNow(base);
  }

  void FlushPendingBeginNow(uint8_t *base)
  {
    const pdz::timing::Scope timing(pdz::timing::Bucket::kNative); // pdz_frame_timing
    const shadow::DrawRecord *rec = g_frame.begin_record;
    if (!rec)
    {
      return;
    }
    const pdz::timing::PhaseScope phase(pdz::timing::Phase::kDraw); // pdz_frame_timing_phases (callers hold the mutex)
    g_frame.begin_record = nullptr;
    DrawArgs args;
    if (!ArgsFor(*rec, args) || !shadow::IsGuestPointer(args.data))
    {
      SkipGeometry(GeometrySkip::kBeginVerticesData, *rec, "data %08X", args.data);
      return;
    }
    if (TimedIssueDraw(base, *rec, args, g_begin_device))
    {
      ++g_stats.pending_begin;
    }
  }

  const DrawStats &GetDrawStats() { return g_stats; }

  GeometryCacheStats GetGeometryCacheStats()
  {
    GeometryCacheStats s;
    s.chunks = uint32_t(g_geometry.chunk_count());
    s.chunk_bytes = g_geometry.chunk_bytes();
    s.used_bytes = g_geometry.used_bytes();
    s.entries = g_geometry.entry_count();
    s.recycles = g_geometry.recycles();
    s.released = g_geometry.released();
    return s;
  }

  SamplerCacheStats GetSamplerCacheStats()
  {
    SamplerCacheStats s;
    s.tables = g_samplers.tables.size();
    s.region_refused = g_samplers.region_refused;
    s.evicted = g_samplers.evicted;
    s.passes = g_samplers.passes;
    return s;
  }

  bool FrameHadShaderMiss(uint64_t frame) { return g_frame.frame == frame && g_frame.shader_miss; }

  std::string GeometryReasons()
  {
    std::string out;
    for (uint32_t i = 0; i < uint32_t(GeometrySkip::kCount); ++i)
    {
      if (g_stats.geometry_reasons[i])
      {
        out += fmt::format(" {}={}", kGeometrySkipNames[i], g_stats.geometry_reasons[i]);
      }
    }
    return out.empty() ? std::string(" -") : out;
  }

  void ResolveTraceQueries()
  {
    NativeDevice *devp = NativeDevice::Get();
    if (!devp || !devp->in_frame() || !g_tq.stats || g_tq.frame != devp->frame_index() || g_tq.count == 0)
    {
      return;
    }
    NativeDevice &dev = *devp;
    const uint32_t n = g_tq.count;
    const uint64_t occlusion_offset = uint64_t(TraceQueries::kMax) * sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS);
    dev.list()->ResolveQueryData(g_tq.stats, D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0, n, g_tq.readback, 0);
    dev.list()->ResolveQueryData(g_tq.occlusion, D3D12_QUERY_TYPE_OCCLUSION, 0, n, g_tq.readback, occlusion_offset);
    dev.Flush();
    uint8_t *mapped = nullptr;
    const D3D12_RANGE range{0, SIZE_T(occlusion_offset + uint64_t(TraceQueries::kMax) * sizeof(uint64_t))};
    if (FAILED(g_tq.readback->Map(0, &range, reinterpret_cast<void **>(&mapped))) || !mapped)
    {
      return;
    }
    const auto *stats = reinterpret_cast<const D3D12_QUERY_DATA_PIPELINE_STATISTICS *>(mapped);
    const auto *samples = reinterpret_cast<const uint64_t *>(mapped + occlusion_offset);
    for (uint32_t i = 0; i < n; ++i)
    {
      const D3D12_QUERY_DATA_PIPELINE_STATISTICS &q = stats[i];
      REXLOG_INFO("pdz_trace: q draw #{} ia_prims {} vs {} gs_prims {} clip_in {} clip_out {} ps {} samples {}",
                  g_tq.sequence[i], q.IAPrimitives, q.VSInvocations, q.GSPrimitives, q.CInvocations, q.CPrimitives,
                  q.PSInvocations, samples[i]);
    }
    const D3D12_RANGE none{0, 0};
    g_tq.readback->Unmap(0, &none);
    g_tq.count = 0;
  }

  void LogDrawStats(const char *why)
  {
    uint64_t srv_peak = 0, srv_failures = 0, sampler_peak = 0, sampler_failures = 0;
    // Existing, not Get: the test-exit summary calls this with the renderer off too, and must not create the device.
    if (NativeDevice *dev = NativeDevice::Existing())
    {
      dev->DescriptorStats(srv_peak, srv_failures, sampler_peak, sampler_failures);
    }
    const PipelineCache &p = PipelineCache::Get();
    PDZ_LOG_PERIODIC(std::string_view(why) == "periodic", "pdz_native: draws ({}): issued {} | skipped: off {} shader {} targets {} pipeline {} geometry {} "
                "| PSOs {} (failed {}{}) | vertex uploads {} ({} KB, {} cache hits) index uploads {} ({} hits) "
                "uncached {} | 32-bit index draws {} | texture kind mismatches {} | stacked-array views {} (none {}) | "
                "loop draws {}, count-0 loop {} (VS {} "
                "PS {}) | cb reuse {} table reuse {} | geometry cache {} entries in {} chunks | shader skips by "
                "reason: PS object {} VS object {} PS cache {} VS cache {} | no-PS draws {} | pipeline skips: "
                "unsupported primitive {} PSO failed {} | geometry skips by reason:{} | descriptor rings: SRV peak "
                "{}/frame refused {}, sampler peak {}/frame refused {}, draws skipped {} | {} | {} | rectlist draws {} | "
                "primitive reset draws {} (rewritten {}, widened {}) | alpha-to-mask draws {} (dithered {}, inexact {}, "
                "no oC0 {}) | pending Begin draws {}, DrawVerticesUP inner Begin skipped {} | draws on a PSO of "
                "another topology {} ({}) | DrawIndexedVerticesUP draws {} (not drawn, cvar off: {}) | {} | {} | {}",
                why, g_stats.issued, g_stats.skipped_off, g_stats.skipped_shader, g_stats.skipped_targets,
                g_stats.skipped_pipeline, g_stats.skipped_geometry, p.pso_count(), p.pso_failures(), p.CacheStatsText(),
                g_stats.vertex_uploads, g_stats.vertex_bytes / 1024, g_stats.vertex_cache_hits, g_stats.index_uploads,
                g_stats.index_cache_hits, g_stats.uncached_fallbacks, g_stats.index32_draws,
                g_stats.texture_kind_mismatch, g_stats.stacked_array_views, g_stats.stacked_array_fallbacks,
                g_stats.loop_draws, g_stats.loop_zero_draws, g_stats.loop_zero_vs,
                g_stats.loop_zero_ps, g_stats.constant_reuse, g_stats.table_reuse,
                g_geometry.entry_count(), g_geometry.chunk_count(), g_stats.miss_ps_object, g_stats.miss_vs_object,
                g_stats.miss_ps_cache, g_stats.miss_vs_cache, g_stats.null_ps_draws,
                g_stats.pipeline_unsupported_prim, g_stats.pipeline_pso_failed, GeometryReasons(), srv_peak,
                srv_failures, sampler_peak, sampler_failures, g_stats.skipped_descriptors, depth20e4::StatsText(),
                blend7e3::StatsText(), g_stats.rectlist_draws, g_stats.prim_reset_draws, g_stats.prim_reset_rewritten,
                g_stats.prim_reset_widened, g_stats.alpha_to_mask_draws, g_stats.alpha_to_mask_dithered,
                g_stats.alpha_to_mask_inexact, g_stats.alpha_to_mask_no_oc0, g_stats.pending_begin,
                g_stats.up_inner_begin, g_stats.topology_differs,
                REXCVAR_GET(pdz_native_draw_topology) ? "drawn with their own" : "drawn with the PSO's",
                g_stats.indexed_up_draws, g_stats.indexed_up_off, FloatCbText(), StateFilterText(), SmallCbText());
  }
  namespace guestwatch
  {
    bool Ready() { return geowatch::Ready(); }
    uint64_t GenSum(uint32_t physical_address, uint32_t bytes) { return geowatch::GenSum(physical_address, bytes); }
    void Arm(uint32_t physical_address, uint32_t bytes) { geowatch::Arm(physical_address, bytes); }
  } // namespace guestwatch
} // namespace pdz::native
