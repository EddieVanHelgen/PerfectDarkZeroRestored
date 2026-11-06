// Native renderer (stage 1): the shader cache. Loads the fork's translated shaders (<xxh3>.dxil + <xxh3>.json
// sidecar, optionally <xxh3>.hlsl for the literal values) lazily per hash from pdz_native_shader_dir and keeps
// them in memory; resolves the shader objects of a DrawRecord to NativeShader entries. PIPELINE_NOTES.md
// describes the lookup rules (the vertex-shader rehash over the bound microcode at object+12) and the miss path.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <d3d12.h>
#include <rex/cvar.h>

#include "../shadow_record.h"

REXCVAR_DECLARE(std::string, pdz_native_shader_dir);
REXCVAR_DECLARE(int32_t, pdz_native_log_frames);
REXCVAR_DECLARE(bool, pdz_native_literals);

namespace pdz::native
{
  enum class TexDim : uint8_t
  {
    k2D = 0,   // t<slot> space0
    k3D = 1,   // t<slot> space1
    kCube = 2, // t<slot> space2
  };

  // One vertex input of a bound vertex shader (sidecar "vertex_inputs"): ATTR<attr> read by the input
  // assembler from stream `stream` at byte `offset` with `format` (2_10_10_10 / 10_11_11 / 11_11_10 arrive as
  // R32_UINT and are decoded in the HLSL).
  struct ShaderVertexInput
  {
    uint32_t attr = 0;        // ATTR<n> semantic index
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    uint32_t offset = 0;      // bytes from the vertex start
    uint32_t stride = 0;      // bytes (sidecar stride_dwords * 4)
    uint32_t fetch_slot = 95; // vertex fetch constant slot (95 = stream 0, 94 = stream 1, ...)
    uint32_t stream = 0;      // 95 - fetch_slot (D3D stream index = input-assembler slot)
  };

  struct ShaderTexture
  {
    uint32_t slot = 0; // texture fetch constant 0..31 (PS 0..15, VS 16..31)
    TexDim dim = TexDim::k2D;
  };

  // Sidecar "samplers": g_Samplers[index] = the sampler of fetch constant `slot` with the per-instruction filter
  // overrides packed in `code` (0x1FFF = none; native_textures.h decodes it: GetSampler(fetch, code)).
  struct ShaderSampler
  {
    uint32_t index = 0;
    uint32_t slot = 0;
    TexDim dim = TexDim::k2D;
    uint32_t code = 0x1FFF;
  };

  struct FloatLiteral
  {
    uint32_t reg = 0; // c[reg] in the shader's own numbering (PS: hardware register - 256)
    uint32_t bits[4] = {0, 0, 0, 0};
  };

  struct NativeShader
  {
    uint64_t hash = 0;
    bool is_vertex = false;
    bool usable = false;           // DXIL + sidecar loaded and understood
    bool inputs_unbound = false;   // VS translated from an unbound container (inspection only): not drawable
    std::string source;            // the directory the files came from (log)
    std::vector<uint8_t> dxil;
    float load_ms = 0.0f;          // the first Load's file reads and parsing (pdz_native_first_use_log)

    // Vertex shaders.
    std::vector<ShaderVertexInput> inputs;
    // Sidecar "vfetch_address" of each input (parallel to `inputs`): the instruction of its first vfetch. A variant
    // whose microcode differs only in vfetch strides / offsets reuses this shader's DXIL with the inputs re-read
    // there (pdz_native_vs_sibling_variants).
    std::vector<uint32_t> input_fetch_address;
    uint32_t stream_mask = 0;      // bit s = input-assembler slot s is read
    uint32_t stream_stride[16] = {}; // per stream, from the sidecar (bytes)
    uint64_t input_layout_hash = 0;
    bool writes_point_size = false;
    bool vertex_texture_fetch = false;

    // Pixel shaders.
    uint32_t color_outputs = 1;    // bit n = writes SV_Target<n>
    bool writes_depth = false;
    bool uses_kill = false;
    // Co-issue fix and PsParamGen (native_coissue.h, native_paramgen.h): checked at the first draw; when patched, `dxil`
    // is the recompiled shader and `coissue_hlsl` its source (the depth20e4 variants start from it instead of the
    // fork's .hlsl).
    bool coissue_checked = false;
    std::string coissue_hlsl;
    // pdz_native_stacked_array (native_hlslfix.h): bit n = the patched shader's tfetch3D of slot n reads the space3
    // Texture2DArray t<n> when it holds a view (BindSrvTable puts the array view of a 2D / stacked / mirror texture
    // there), else its Texture3D t<n> space1. 0 = not patched.
    uint32_t array_slots = 0;

    // Both.
    // Sidecar "loop_constants_used": bit i = a loop reads loop constant i, the microcode's absolute index
    // (VS i0..15 = 0..15, PS i0..15 = 16..31; g_LoopConstants[i >> 2][i & 3] = device +10144 + 4i).
    uint32_t loop_constants_used = 0;
    std::vector<ShaderTexture> textures;
    std::vector<ShaderSampler> samplers;
    uint32_t unsupported = 0;      // sidecar "unsupported" entries (logged, shader still used)
    // Sidecar "texture_fetches" with "unnormalized": bit s = fetch slot s is read with unnormalized (texel)
    // coordinates, which the fork's unnormalize2D / 3D divide by the bound view's GetDimensions: right for an
    // upload (guest size), off by the scale for a resolve mirror at pdz_native_scale > 1 (only checked and logged
    // there, native_draw.cpp BindSrvTable). 63 VS in the cache use slot 16; no PS.
    uint32_t unnormalized_slots = 0;

    // Literal records (the container's embedded defaults) decoded from the sidecar / HLSL; applied on top of
    // the device's constant shadow only when pdz_native_literals is on (D3D itself copies them into the
    // shadow at SetShader time, PIPELINE_NOTES.md "literals"). For a bound VS the parent's literals are
    // merged in (manifest derived_from).
    std::vector<FloatLiteral> float_literals;
    uint32_t bool_literal_bits[8] = {};
    uint32_t bool_literal_mask[8] = {};
    uint32_t loop_literals[32] = {};
    uint32_t loop_literal_mask = 0;

    // pdz_native_cb_used_only (stage 2 M5 drawcost): the float constants the shader can read, as runs {first, count}
    // of c[] indices (VS c0..c255, PS c0..c223 = hardware 256..479; past the uploaded count the full upload has zeros):
    // the sidecar "float_constants_used", every c[<n>] read and "static const float4 cl<n>" literal declaration of the
    // HLSL (pdz_native_live_literals turns those into c[<n>] reads), and the literal records. float_constants_all: the
    // whole block (relative addressing, no sidecar list, no HLSL, or an access the scan does not recognise).
    bool float_constants_all = true;
    std::vector<std::pair<uint16_t, uint16_t>> float_constant_runs;
    uint16_t float_constants_count = 0; // registers in the runs
  };

  struct ShaderStats
  {
    uint64_t hits = 0;           // draws whose shader resolved
    uint64_t misses = 0;         // draws skipped for a missing shader
    uint64_t pending = 0;        // draws skipped while a VS binding waits for D3D's patch
    uint32_t distinct_loaded = 0;
    uint32_t distinct_failed = 0; // files present but unusable
    uint32_t distinct_missed = 0; // no files (dumped to misses\)
    uint32_t vs_bindings = 0;
    uint32_t binder_matches = 0;    // (object, declaration) pairs where the binder reproduced D3D's bound copy
    uint32_t binder_mismatches = 0; // ... and where it did not (logged with the first differing dword)
    uint32_t translations_requested = 0, translations_done = 0, translations_failed = 0;
    // Bound VS variants built from a cached sibling instead of the translator (pdz_native_vs_sibling_variants).
    uint32_t sibling_derived = 0;
    // pdz_native_translate_sync: draws that waited for a translation, how those waits ended, the waits the frame
    // budget refused, and the time waited.
    uint32_t sync_waits = 0, sync_done = 0, sync_failed = 0, sync_timeouts = 0, sync_over_budget = 0;
    uint64_t sync_ms_total = 0;
    uint32_t sync_ms_max = 0;
    // pdz_native_first_use_log: VS bindings resolved and pixel shaders first used, with the time their lookups took
    // until then (binder, file load, co-issue / PsParamGen patch, compile).
    uint32_t first_use_vs = 0, first_use_ps = 0;
    double first_use_vs_ms = 0.0, first_use_vs_max_ms = 0.0, first_use_ps_ms = 0.0, first_use_ps_max_ms = 0.0;
  };

  class ShaderCache
  {
  public:
    static ShaderCache &Get();

    // The directory the shaders are read from (resolved once: cvar, else <exe dir>\native_shaders); a file not there
    // is tried in pdz_native_shader_fallback_dir.
    const std::string &directory();
    // The fallback folder (pdz_native_shader_fallback_dir; the research cache in a developer build, else "").
    const std::string &fallback_directory();

    // Pixel shader by the create-time hash (PS microcode is never patched, so the hash is the .xsh key).
    // `entry` may be null (object not seen by the Create hook): counted as a miss.
    const NativeShader *FindPixel(const uint8_t *base, const shadow::ShaderEntry *entry);
    // The pixel shader of a draw with none set (D3D SetPixelShader(NULL): depth-only passes). Empty DXIL: the
    // pipeline is built without a PS stage, so colour targets are not written.
    const NativeShader *NullPixel();

    // Vertex shader for the (object, declaration) pair: hashes D3D's bound microcode copy (the patched vfetch
    // variant) and the binder's own reproduction of it (native_binder.h, checked against each other); a hash
    // the cache does not know is built from a cached sibling when only its vfetch strides / offsets differ
    // (pdz_native_vs_sibling_variants), else translated on first use with the XenosRecomp fork into the shader dir
    // (pdz_native_translator), the draw staying pending until the files exist; with pdz_native_translate_sync (on by
    // itself with graphics_backend null) the draw waits for the translation instead (bounded). FindPixel likewise.
    // Returns nullptr while pending or on a miss (both counted).
    const NativeShader *FindVertex(const uint8_t *base, uint32_t object, uint32_t declaration,
                                   const shadow::ShaderEntry *entry, const shadow::DrawRecord *record = nullptr);

    // Direct lookup / lazy load by hash (nullptr = not available; the failure is remembered).
    const NativeShader *Load(uint64_t hash, bool vertex);
    // The pipeline list's first-start pass (native_pipeline_seed.cpp): pixel shader `hash` loaded and its first-draw
    // patch check (coissue::Apply, as FindPixel runs it) done now, with the microcode and container the generator saved
    // beside it instead of guest memory's. `copy`: on a copy of the cached shader (the collect pass), which is returned.
    // nullptr: not loadable.
    const NativeShader *PreparePixel(uint64_t hash, NativeShader *copy = nullptr);

    // Once per frame (from the draw path's NewFrame, before the frame's first lookup): the frame stamp for the VS
    // bindings' LRU, pdz_native_vs_binding_cap every 600 frames, coverage log every pdz_native_log_frames frames.
    void OnFrame(uint64_t frame_index);
    void LogCoverage(const char *why);
    const ShaderStats &stats() const { return stats_; }
    // pdz_native_cb_used_only (drawcost): how the loaded shaders' float constant sets came out (the draws line).
    static std::string FloatConstantScanText();

    // Memory log (native_memlog.cpp): what the cache keeps. The loaded shaders are never evicted (bounded by the
    // shader cache on disk); the VS bindings over pdz_native_vs_binding_cap are. Cold: walks the maps; all zero (and
    // nothing created) before the first shader lookup.
    struct MemoryStats
    {
      uint32_t loaded = 0, usable = 0; // every hash ever asked for, and those drawable
      uint64_t dxil_bytes = 0;         // their DXIL (the co-issue fix's recompiled DXIL replaces the fork's)
      uint64_t source_bytes = 0;       // co-issue patched HLSL kept for the depth20e4 variants
      uint32_t bindings = 0;           // VS (object, declaration) pairs held (pairs of freed objects stay until dropped)
      uint64_t binder_bytes = 0;       // the binder's microcode copies they hold
      uint32_t missed = 0;             // hashes reported as misses
      uint64_t bindings_evicted = 0;   // pairs dropped by pdz_native_vs_binding_cap (total)
      uint64_t binder_bytes_released = 0; // binder copies released once checked (total)
    };
    MemoryStats GetMemoryStats() const;

  private:
    void MaintainBindings(uint64_t frame); // pdz_native_vs_binding_cap (OnFrame)
    using VsBindingRef = void *; // ShaderCache::Impl's VsBinding (private to native_shaders.cpp)
    void CompareWithD3D(VsBindingRef binding, const uint8_t *copy, const shadow::ShaderEntry *entry,
                        uint32_t declaration);
    // pdz_native_vs_sibling_variants: the bound variant `hash` (guest-order microcode `ucode`, `bytes`) of the VS
    // created as `parent`, built from a cached bound variant of the same parent whose microcode differs only in the
    // vfetch stride / offset fields (and prefetch counts): the fork's HLSL is then identical, only the input layout
    // differs. nullptr when no sibling qualifies (the caller translates as before).
    const NativeShader *DeriveVertexVariant(uint64_t hash, uint64_t parent, const uint8_t *ucode, uint32_t bytes,
                                            uint32_t declaration);
    ShaderCache() = default;
    struct Impl;
    Impl *impl_ = nullptr;
    ShaderStats stats_;
    Impl &impl();
  };
} // namespace pdz::native
