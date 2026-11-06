// Native renderer (stage 1): root signature (the fork's binding model), input layouts from the VS sidecar,
// the register-shadow -> D3D12 state decoding and the PSO cache. PIPELINE_NOTES.md has the decoding table.
#pragma once
#include <vector>
#include <functional>

#include <cstdint>
#include <string>

#include <d3d12.h>
#include <rex/cvar.h>

#include "../shadow_record.h"
#include "native_shaders.h"

// pdz_native_first_use_log (native_pipeline.cpp): also read by the shader cache and the shader variants.
REXCVAR_DECLARE(bool, pdz_native_first_use_log);

namespace pdz::native
{
  // Root parameters, in order (native_pipeline.cpp builds them; native_draw.cpp binds them).
  enum RootParam : uint32_t
  {
    kRootCbvFloatVs = 0, // b0 space0, VS visibility: float4 c[256] = VS c0..c255
    kRootCbvFloatPs = 1, // b0 space0, PS visibility: float4 c[256] = PS c0..c223 (hardware 256..479)
    kRootCbvBoolLoop = 2, // b1 space0, all stages: uint4 g_BoolBits[2], uint4 g_LoopConstants[8]
    kRootCbvSystem = 3,  // b2 space0, all stages: SystemConstants
    kRootSrvTable = 4,   // t0..t31 space0 (2D), space1 (3D), space2 (cube), space3 (2D array: the stacked view of a
                         // tfetch3D slot, native_hlslfix.h): 128 descriptors, all stages
    kRootSamplerVs = 5,  // s0..s31 space0, VS visibility (the VS's sampler variants)
    kRootSamplerPs = 6,  // s0..s31 space0, PS visibility (the PS's sampler variants)
    kRootConstDepth = 7, // b3 space0, PS visibility: 4 root constants, the SV_Depth 20e4 variant's polygon offset
                         // (native_depth20e4.h: front scale / offset, back scale / offset); set only for its draws
    kRootUavEdram7e3 = 8,   // u0 space0, PS visibility: one-descriptor table, the 7e3 surface as a rasterizer-ordered
                            // view for the mode-2 7e3 blend variant (native_blend7e3.h); set only for its draws
    kRootConstBlend7e3 = 9, // b4 space0, PS visibility: 8 root constants of that variant (blendcontrol, write mask,
                            // exponent bias scale, blend constant colour; blend7e3::Constants)
    kRootParamCount = 10
  };
  constexpr uint32_t kRootConstDepthCount = 4;
  constexpr uint32_t kRootConstBlend7e3Count = 8;
  constexpr uint32_t kSrvTableSize = 128;
  constexpr uint32_t kSrvArraySpace = 3; // table entry kSrvArraySpace * 32 + slot = Texture2DArray t<slot> space3
  constexpr uint32_t kSamplerTableSize = 32;

  // b2 SystemConstants, as shader_common_pdz.hlsli declares it (48 bytes, 256-byte aligned upload).
  struct SystemConstants
  {
    float half_pixel_offset[2]; // added to oPos.xy * oPos.w; 0 here (the half pixel is in ndc_offset)
    float alpha_ref;            // RB_ALPHA_REF
    uint32_t alpha_func;        // 0 = off, else D3DCMPFUNC (xenos CompareFunction + 1)
    float point_size[4];        // x, y = half width / height in guest pixels (PA_SU_POINT_SIZE 12.4), z, w = 2 / viewport size
    uint32_t flags;             // HostViewport::vte_flags (fork patch 08: pdzHostPosition)
    uint32_t pad[3];            // HLSL g_Reserved; [0] = the resolution scale at pdz_native_scale > 1, else 0 (for a
                                // translated PsParamGen position: floor(iPos.xy) / scale, as the SDK); [1], [2] = 0
    float ndc_scale[4];         // xyz: guest -> host clip space scale
    float ndc_offset[4];        // xyz: offset multiplied by W
    // Read only by the user-clip-plane GS (native_clip_gs_dxil.h, pdz_native_clip_planes): the enabled
    // PA_CL_UCP_n converted to host clip space (UserClipPlanesToHost), packed like the SDK's user_clip_planes;
    // unused entries 0 (distance 0 = kept). The fork's shaders declare only the fields above.
    float clip_planes[6][4];
  };
  static_assert(sizeof(SystemConstants) == 176);

  struct BoolLoopConstants
  {
    uint32_t bool_bits[8];      // registers 0x4900..0x4907 = device +10112: VS b0..b127 in dwords 0..3 (+10112),
                                // PS b0..b127 = b128..b255 in 4..7 (+10128); bool n = bit (n & 31) of dword n >> 5
    uint32_t loop_constants[32]; // registers 0x4908..0x4927 = device +10144 + 4i: VS i0..i15 = 0..15 (+10144),
                                 // PS i0..i15 = 16..31 (+10208); count 7:0, start 15:8, step 23:16 (signed)
  };
  static_assert(sizeof(BoolLoopConstants) == 160);

  // Register-shadow access: host-order value of register `reg` (0x2000..0x2387 runs, window scissor) from a
  // DrawRecord's raw copy; 0 when the register is not in the shadow.
  uint32_t ReadReg(const shadow::DrawRecord &r, uint32_t reg);
  float ReadRegFloat(const shadow::DrawRecord &r, uint32_t reg);

  // The pipeline-relevant state of one draw, decoded from the register shadow.
  struct RenderState
  {
    uint32_t depthcontrol = 0;      // RB_DEPTHCONTROL
    uint32_t blendcontrol[4] = {};  // RB_BLENDCONTROL0..3
    uint32_t color_mask = 0;        // RB_COLOR_MASK
    uint32_t colorcontrol = 0;      // RB_COLORCONTROL (alpha test and alpha-to-mask -> b2, not PSO: AlphaTestFor)
    uint32_t su_sc_mode_cntl = 0;   // PA_SU_SC_MODE_CNTL
    uint32_t stencilrefmask = 0;    // RB_STENCILREFMASK
    uint32_t stencilrefmask_bf = 0; // RB_STENCILREFMASK_BF
    uint32_t point_size = 0;        // PA_SU_POINT_SIZE
    uint32_t vtx_cntl = 0;          // PA_SU_VTX_CNTL (pixel centre)
    uint32_t clip_cntl = 0;         // PA_CL_CLIP_CNTL (clip_disable -> no depth clip, as the SDK)
    uint32_t depth_info = 0;        // RB_DEPTH_INFO (depth format for the bias units)
    uint32_t color_info = 0;        // RB_COLOR_INFO (colour exponent bias of RT0, read by the 7e3 blend variant only)
    float alpha_ref = 0.0f;         // RB_ALPHA_REF
    float blend_factor[4] = {};     // RB_BLEND_RED..ALPHA
    float poly_offset_front_scale = 0.0f, poly_offset_front_offset = 0.0f;
    float poly_offset_back_scale = 0.0f, poly_offset_back_offset = 0.0f;
  };
  void DecodeRenderState(const shadow::DrawRecord &r, RenderState &out);

  // The PS alpha test of a draw (SystemConstants alpha_func / alpha_ref, the fork's alphaTest() on oC0.w): the
  // RB_COLORCONTROL alpha test with alpha-to-mask folded in as the SDK computes it for one sample (no MSAA in
  // PDZ), native_pipeline.cpp. Alpha-to-mask disabled (or cvar pdz_native_alpha_to_mask off) = the alpha test
  // alone, exactly as before. The flags describe the register state whatever the cvar (draw-stats counters).
  struct AlphaTest
  {
    uint32_t func = 0;          // SystemConstants::alpha_func: 0 = off, else D3DCMPFUNC (xenos CompareFunction + 1)
    float ref = 0.0f;           // SystemConstants::alpha_ref
    bool alpha_to_mask = false; // RB_COLORCONTROL.alpha_to_mask_enable
    bool dithered = false;      // ... with unequal quad offsets: one threshold (their mean) for every pixel
    bool inexact = false;       // ... with an alpha test that does not combine into one comparison (approximated)
  };
  AlphaTest AlphaTestFor(const RenderState &rs);

  // Guest viewport / clip / scissor registers -> host viewport, scissor and the VS position constants (the SDK's
  // draw_util::GetHostViewportInfo + GetScissor, native_pipeline.cpp). The scissor is in host pixels, not yet
  // clamped to the bound surface.
  struct HostViewport
  {
    D3D12_VIEWPORT viewport{};
    D3D12_RECT scissor{};
    float ndc_scale[3] = {1.0f, 1.0f, 1.0f};
    float ndc_offset[3] = {};
    uint32_t vte_flags = 0; // bit 0 VTX_XY_FMT, bit 1 VTX_Z_FMT, bit 2 VTX_W0_FMT (SystemConstants::flags)
    bool depth_clip = true;
    bool empty_viewport = false;
  };
  void ComputeHostViewport(const shadow::DrawRecord &r, uint32_t resolution_scale, bool ps_writes_depth,
                           HostViewport &out);

  // User clip planes (PA_CL_CLIP_CNTL ucp_ena, honoured as in the SDK's pipeline_cache only without clip_disable):
  // the mask of enabled planes when the clip-plane geometry shader applies (cvar pdz_native_clip_planes on, not
  // ucp_cull_only_ena), else 0.
  uint32_t UserClipPlaneMask(const RenderState &rs);
  // The enabled planes (DrawRecord::clip_planes_be, guest clip space, SDK order = ascending plane index) converted
  // for the GS: dot(host position, out[k]) == dot(guest position, plane), with host.xyz = guest.xyz * ndc_scale +
  // ndc_offset * w (pdzHostPosition, half-pixel offset 0). False (out zeroed = nothing clipped) when an ndc scale
  // component is 0 or not finite.
  bool UserClipPlanesToHost(const shadow::DrawRecord &r, uint32_t mask, const HostViewport &hv, float out[6][4]);

  // What the bound surfaces contribute to the PSO (filled by native_draw.cpp from the surface layer).
  struct TargetFormats
  {
    uint32_t rt_count = 0;
    DXGI_FORMAT rt[4] = {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN};
    DXGI_FORMAT ds = DXGI_FORMAT_UNKNOWN;
    bool depth_float24 = false; // guest depth format kD24FS8 (bias units)
    uint32_t resolution_scale = 1;
    // Render target 0 is a 7e3 surface (k_2_10_10_10_FLOAT / _AS_16_16_16_16), and it was created UAV-capable
    // (native_blend7e3.h: the blend variants).
    bool rt0_7e3 = false;
    bool rt0_uav = false;
  };

  // Guest D3DPRIMITIVETYPE (= xenos::PrimitiveType) -> host topology. `expand` = the index buffer has to be
  // rewritten (QUADLIST, TRIANGLEFAN); `points` = geometry-shader quad expansion; `rects` = RECTLIST drawn as a
  // triangle list through the rectangle-list geometry shader (native_rectlist_gs_dxil.h, cvar pdz_native_rectlist).
  struct Topology
  {
    D3D12_PRIMITIVE_TOPOLOGY_TYPE type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    D3D_PRIMITIVE_TOPOLOGY topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    bool expand = false;
    bool points = false;
    bool rects = false;
    bool supported = false;
  };
  Topology TopologyFor(uint32_t guest_primitive_type);

  // Primitive reset (strip cut) of an indexed draw, by the SDK primitive processor's rules: PA_SU_SC_MODE_CNTL
  // multi_prim_ib_ena on, a strip-like guest primitive (line strip, triangle strip, triangle fan; lists, QUADLIST
  // included, never reset) and, for 16-bit indices, VGT_MULTI_PRIM_IB_RESET_INDX <= 0xFFFF. A 16-bit index is
  // compared whole, a 32-bit one by its low 24 bits (the register is 24 bits wide).
  struct PrimitiveReset
  {
    bool enabled = false;
    uint32_t index = 0; // VGT_MULTI_PRIM_IB_RESET_INDX.reset_indx
  };
  PrimitiveReset DecodePrimitiveReset(const shadow::DrawRecord &r, uint32_t guest_primitive_type, bool index32);

  struct Pipeline
  {
    ID3D12PipelineState *pso = nullptr;
    Topology topology;
    bool uses_blend_factor = false; // OMSetBlendFactor needed
    bool stencil = false;           // OMSetStencilRef needed
    bool shader_depth_bias = false; // SV_Depth 20e4 PS variant (ApplyShaderDepthBias): set kRootConstDepth per draw
    // 7e3 blend PS variant (native_blend7e3.h): 1 = alpha clamp (fixed-function blend), 2 = rasterizer-ordered output
    // merger (no RTV: the draw binds render target 0 as kRootUavEdram7e3 and sets kRootConstBlend7e3).
    uint32_t blend7e3 = 0;
    uint32_t creations = 0;
    uint64_t last_used_frame = 0;   // the native frame of the last Lookup that returned it (pdz_native_pso_cap LRU)
  };

  class PipelineCache
  {
  public:
    static PipelineCache &Get();

    // Creates the root signature, the point-expansion geometry shader and the null descriptor blocks.
    // False (logged once) = the draw path stays off.
    bool Init();
    bool ready() const { return ready_; }
    ID3D12RootSignature *root_signature() const { return root_signature_; }

    // PSO for (shaders, primitive type, decoded register state, target formats, strip cut). Cached by a hash of
    // all of it; nullptr when creation failed (remembered, logged once per key). `strip_cut` = IBStripCutValue for
    // a primitive-reset draw (it must match the bound index format); honoured for line / triangle strips only.
    const Pipeline *Lookup(const NativeShader *vs, const NativeShader *ps, uint32_t guest_primitive_type,
                           const RenderState &rs, const TargetFormats &tf,
                           D3D12_INDEX_BUFFER_STRIP_CUT_VALUE strip_cut = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED);

    // Null descriptors to pre-fill the per-draw tables (non-shader-visible source handles, contiguous):
    // 128 null SRVs laid out like the table (2D x32, 3D x32, cube x32, 2D array x32) and 32 default samplers.
    D3D12_CPU_DESCRIPTOR_HANDLE null_srv_block() const { return null_srv_block_; }
    D3D12_CPU_DESCRIPTOR_HANDLE null_sampler_block() const { return null_sampler_block_; }

    uint32_t pso_count() const { return pso_count_; } // PSOs created (total; the draws line)
    uint32_t pso_failures() const { return pso_failures_; }
    // Keys in the PSO map (memory log): live PSOs plus the remembered failures (failures are never evicted).
    uint32_t pso_keys() const;
    // PSOs alive now (created - released by pdz_native_pso_cap) and released in total.
    uint32_t pso_live() const { return pso_count_ - uint32_t(pso_evicted_); }
    uint64_t pso_evicted() const { return pso_evicted_; }
    // PSOs the warm-up holds (pdz_native_pso_cache): not yet taken by a draw, or with pdz_native_pso_prewarm every one
    // it made (kept for the session, shared with the draw map; pdz_native_pso_cap does not free them). Memory log
    // pso_warm; distinct live PSOs are at most pso_live() + pso_warm_held().
    uint32_t pso_warm_held() const;
    uint32_t root_signature_count() const { return root_signature_ ? 1u : 0u; }
    // The draws line's addition to "PSOs N (failed F": the first-use PSOs (count, ms) and, with pdz_native_pso_cache,
    // the warm-up / library / recipes; empty with pdz_native_pso_cache and pdz_native_first_use_log both off.
    std::string CacheStatsText() const;
    // The level-start summary (native_textures.cpp, record thread): PSOs created so far (pso_count) and, of those,
    // the ones made at a draw's first use (not ready from the warm-up) with their total cost in ms.
    void FirstUseTotals(uint32_t &created, uint32_t &built_at_draw, double &built_ms) const;
    // A level starting (native_textures.cpp OnLevelChange, game thread): with the startup warm-up still running, waits
    // for it (pdz_native_pso_level_wait_ms), so the level's pipelines are made before its first frame. Logged.
    void WaitForWarmupAtLevelStart(const char *level);

    // Once per native frame, from the draw path's NewFrame (before the frame's first Lookup, so no Pipeline pointer is
    // held): the frame stamp Lookup gives each PSO it returns, and every 600 frames pdz_native_pso_cap: with more live
    // PSOs than the cap, the least recently used ones not returned for 1800 frames are released (ReleaseLater) until
    // three quarters of the cap are left.
    void OnFrame(uint64_t frame);

    // The SV_Depth 20e4 polygon-offset variant (RENDER_FEASIBILITY 6.4 step 5, native_depth20e4.h): true with
    // the variant's DXIL for `ps` when the draw should write the guest's 20e4 depth from the shader
    // (pdz_native_depth20e4 on, D24FS8 depth bound, depth test on; mode 1 also needs a polygon offset). The PSO
    // then has no fixed-function bias and Pipeline::shader_depth_bias is set (the draw supplies kRootConstDepth).
    // False = the fixed-function D3D12 depth bias (GetD3D10IntegerPolygonOffset units), as with the cvar off.
    // `edge_out`: the variant's compare edge (depth20e4::EdgeFor; 0 unless pdz_native_depth20e4_edge).
    bool ApplyShaderDepthBias(const NativeShader *ps, const RenderState &rs, const TargetFormats &tf, bool polygonal,
                              const void **dxil_out, size_t *dxil_size_out, uint32_t *edge_out = nullptr);

  private:
    PipelineCache() = default;
    struct Impl;
    Impl *impl_ = nullptr;
    bool ready_ = false;
    bool init_attempted_ = false;
    ID3D12RootSignature *root_signature_ = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE null_srv_block_{};
    D3D12_CPU_DESCRIPTOR_HANDLE null_sampler_block_{};
    uint32_t pso_count_ = 0;
    uint32_t pso_failures_ = 0;
    uint64_t frame_ = 0;            // OnFrame's frame: Lookup's LRU stamp
    uint64_t last_maintenance_ = 0;
    uint64_t pso_evicted_ = 0;
    uint32_t evict_logged_ = 0;
  };

  // The shipped pipeline list (data/native_pipeline_list.txt, tools/shaders/make_pipeline_list.py; 2026-11-03): per
  // pipeline its key and input layout and the hashes of the DXIL it was built with; no DXIL. A fresh install turns it
  // into pso_recipes.bin with the player's own DXIL (native_generate.cpp), so the warm-up compiles every pipeline at
  // the first start, before play.
  namespace pipeline_seed
  {
    struct Query
    {
      uint64_t vs = 0, ps = 0;                       // shader hashes (ps 0: depth only)
      uint32_t depth_variant = 0, blend7e3_variant = 0; // 0 none; depth 1-3 = edge 0-2; blend 1 / 2 = the mode
      uint64_t vs_id = 0, ps_id = 0, variant_id = 0; // the developer's DXIL hashes (a check only)
    };
    struct Blobs
    {
      const std::vector<uint8_t> *vs = nullptr, *ps = nullptr, *variant = nullptr;
    };
    struct Report
    {
      uint32_t listed = 0, written = 0, no_shader = 0, other_dxil = 0;
    };
    // The list's pipelines, in its order.
    std::vector<Query> ListQueries();
    uint64_t ListHash(); // XXH3 of the embedded list (the first start's stamp)
    // The list's pipelines written to `path` as a recipe file (pso_recipes.bin's format), with the DXIL `provide`
    // returns for each (nullptr: the player lacks that shader, the pipeline is left out). The warm-up then makes them.
    // Writes `path` (pso_recipes.bin's format) from the list, the blobs from `provide` (a null vs: the pipeline is left
    // out; ps / variant null where the key has none).
    Report WriteRecipes(const std::string &path, const std::function<Blobs(const Query &)> &provide);
    // The first start's pass (native_pipeline_seed.cpp; OnPostLoadXexImage, after the shaders are made, before any draw):
    // with no recipe file yet (pdz_native_pso_seed), the list's shaders loaded, their first-draw patches and variants
    // compiled on all cores behind the progress window, and the recipe file written, so the startup warm-up builds
    // every listed pipeline before play.
    void PrepareAtFirstStart();
    // pdz_native_shader_preload: the list's shaders and variants loaded a little per call (about `budget_ms`) until all
    // are; the record thread's PrewarmStep (the draw thread's: ShaderCache and the variants are its own).
    void PreloadStep(double budget_ms);
  } // namespace pipeline_seed
} // namespace pdz::native
