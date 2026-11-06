// Native renderer (stage 1): the per-draw path. The shadow hooks hand every draw record to OnDraw; it resolves
// the shaders (native_shaders.h), the PSO (native_pipeline.h), uploads constants / vertices / indices and
// issues the D3D12 draw on the device's frame list. PIPELINE_NOTES.md: "OnDraw" for the contract.
#pragma once

#include <cstdint>

#include <rex/cvar.h>

#include "../shadow_record.h"

// Stage 2 M5 drawcost3 (native_draw.cpp): skip D3D12 state calls that set what the list already has.
REXCVAR_DECLARE(bool, pdz_native_state_filter);

namespace pdz::native
{
  struct FetchDecoded; // native_textures.h
  struct HostTexture;  // native_textures.h

  // Why a draw was skipped for its geometry / viewport (DrawStats::geometry_reasons; first examples logged).
  enum class GeometrySkip : uint32_t
  {
    kDrawArguments,          // zero count, zero stride or an unreadable UP / BeginVertices data pointer
    kBeginVerticesData,      // BeginVertices whose ring pointer was not valid at EndVertices
    kIndexBufferObject,      // no index buffer object set
    kIndexMemory,            // index data unreadable or more indices than the draw limit
    kIndexUpload,            // the index upload failed (frame upload / geometry cache full)
    kIndexRange,             // index range wider than the vertex limit
    kExpandIndices,          // quad / fan index expansion failed
    kUpStreamNotZero,        // UP / BeginVertices draw whose VS reads a stream other than 0
    kStreamNoBuffer,         // a stream the VS reads has no vertex buffer set (NOT skipped: null view; counted only)
    kStreamOffsetPastEnd,    // SetStreamSource offset at or beyond the buffer size
    kStreamNoStrideOrMemory, // stride 0 (stream and sidecar) or memory unreadable
    kRangePastBuffer,        // first vertex of the draw beyond the buffer
    kVertexUpload,           // the vertex upload failed
    kEmptyViewportOrScissor, // empty host viewport or scissor (nothing can be covered)
    kPastPhysicalMemory,     // index / vertex bytes crossing the end of the 512 MB physical view (0x20000000)
    kCount
  };

  struct DrawStats
  {
    uint64_t issued = 0;
    uint64_t skipped_off = 0;        // native renderer off / no device / no frame
    uint64_t skipped_shader = 0;     // VS or PS not resolved (miss or pending, counted in the shader cache)
    uint64_t skipped_targets = 0;    // no host render target bound
    uint64_t skipped_pipeline = 0;   // PSO unavailable (creation failed) or unsupported primitive type
    uint64_t skipped_geometry = 0;   // unbound stream / index buffer, empty range, memory not readable
    uint64_t vertex_uploads = 0;     // geometry-cache fills (bytes in vertex_bytes)
    uint64_t vertex_bytes = 0;
    uint64_t vertex_cache_hits = 0;
    uint64_t index_uploads = 0;
    uint64_t index_cache_hits = 0;
    uint64_t index32_draws = 0;      // indexed draws with 32-bit indices (index buffer Common bit 31)
    uint64_t constant_reuse = 0;     // float constant buffers reused (no dirty bits since the last draw)
    uint64_t table_reuse = 0;        // descriptor tables reused within the frame
    uint64_t uncached_fallbacks = 0; // geometry that went through the frame upload (cache full)
    uint64_t pending_begin = 0;      // BeginVertices draws issued at EndVertices or the next entry point
    uint64_t up_inner_begin = 0;     // DrawVerticesUP inner BeginVertices not kept pending (pdz_native_up_begin_once)
    uint64_t texture_kind_mismatch = 0; // texture bound where the shader samples another kind (left null)
    // pdz_native_stacked_array: tfetch3D slots of patched pixel shaders given the Texture2DArray view of a 1D / 2D /
    // stacked / mirror texture, and those that had none (left to the Texture3D path: volume view or null).
    uint64_t stacked_array_views = 0;
    uint64_t stacked_array_fallbacks = 0;
    // Loop diagnostic (issued draws): a shader loop over a constant whose count (bits 7:0) is 0 never runs its body.
    uint64_t loop_draws = 0;         // the VS or PS has a loop (sidecar loop_constants_used)
    uint64_t loop_zero_draws = 0;    // ... and at least one of its loop constants has count 0
    uint64_t loop_zero_vs = 0;       // loop_zero_draws by stage (a draw may count in both)
    uint64_t loop_zero_ps = 0;
    // skipped_shader by reason (log 026: in-game frames all missed with nothing logged by the shader cache).
    uint64_t miss_ps_object = 0;     // PS object unknown and unreadable as a D3D pixel shader
    uint64_t miss_vs_object = 0;     // VS object unknown and unreadable (or no VS set)
    uint64_t miss_ps_cache = 0;      // PS entry known, shader not drawable (missing / translating)
    uint64_t miss_vs_cache = 0;      // VS entry known, binding pending / missing / translating
    uint64_t null_ps_draws = 0;      // depth-only draws (no pixel shader): drawn with no PS stage
    uint64_t geometry_reasons[uint32_t(GeometrySkip::kCount)] = {}; // skipped_geometry by reason
    uint64_t pipeline_unsupported_prim = 0; // skipped_pipeline: primitive type with no host topology
    uint64_t pipeline_pso_failed = 0;       // skipped_pipeline: CreateGraphicsPipelineState failed (logged per key)
    uint64_t skipped_descriptors = 0;       // a descriptor ring was full (NativeDevice warns once per frame)
    uint64_t rectlist_draws = 0;            // RECTLIST draws issued through the rectangle-list GS
    // Primitive reset (REVIEW.md 9): indexed strip / fan draws with PA_SU_SC_MODE_CNTL multi_prim_ib_ena on.
    uint64_t prim_reset_draws = 0;
    uint64_t prim_reset_rewritten = 0; // ... whose reset value is not all ones for the index size (rewritten on upload)
    uint64_t prim_reset_widened = 0;   // ... 16-bit ones uploaded as 32-bit (0xFFFF is also a real vertex index there)
    // Issued draws with RB_COLORCONTROL.alpha_to_mask_enable (done as the PS alpha test: native_pipeline.h
    // AlphaTestFor; counted whatever pdz_native_alpha_to_mask).
    uint64_t alpha_to_mask_draws = 0;
    uint64_t alpha_to_mask_dithered = 0;    // ... unequal quad offsets (one mean threshold, no 2x2 dither)
    uint64_t alpha_to_mask_inexact = 0;     // ... with an alpha test that does not combine into one comparison
    uint64_t alpha_to_mask_no_oc0 = 0;      // ... PS without colour output 0 (no alpha test runs, as the SDK)
    // Draws whose PSO was created by a draw of another primitive type with a different host topology or expansion
    // (TRIANGLESTRIP on a QUADLIST / TRIANGLEFAN PSO and the like); drawn with their own unless
    // pdz_native_draw_topology is off (native_draw.cpp DrawTopology).
    uint64_t topology_differs = 0;
    // D3DDevice_DrawIndexedVerticesUP (hooked since stage1/subagents/hud.md): issued, and not drawn because
    // pdz_native_indexed_up is off.
    uint64_t indexed_up_draws = 0;
    uint64_t indexed_up_off = 0;
  };

  // Entry point for the shadow hooks (the orchestrator wires it, HOOK_WIRING.md): one DrawRecord of kind
  // kDrawIndexedVertices / kDrawVertices / kDrawVerticesUP / kBeginVertices / kDrawIndexedVerticesUP (indices and
  // vertices at guest pointers, the UI's indexed shapes), `base` = guest memory base.
  // Reads the device struct (constants) and guest memory (vertices, indices, microcode) through `base`; the
  // record's own fields for everything else. Call it with the record the hook just filled, before or after
  // forwarding (both work: the state the draw needs is already in the device struct at hook entry; for a
  // vertex shader bound for the first time the bound-copy rehash may need a later draw either way).
  // kBeginVertices is deferred: the game writes the vertices after the call returns, so the draw is issued by
  // OnEndVertices(). Other kinds (Clear, Resolve, Swap) belong to the surface layer and return false here.
  // Returns true when a native draw was issued.
  bool OnDraw(uint8_t *base, const shadow::DrawRecord &r);

  // D3DDevice_EndVertices hook (sub_826E9A50), AFTER forwarding: issues the pending BeginVertices draw.
  void OnEndVertices(uint8_t *base);

  // Issues a BeginVertices draw whose batch was ended without EndVertices (the particle renderer, caller
  // 825F1260, inlines EndVertices' single store, so the hook never runs for it): called from every later native
  // entry point (draw, BeginVertices, clear, resolve, swap, SetRenderTarget, SetDepthStencilSurface) before it
  // does anything else. Caller holds the render lock. pdz_native_pending_begin_late off: does nothing (only
  // EndVertices issues the batch, as before 6929aff).
  void FlushPendingBegin(uint8_t *base);
  // The same without the cvar check (EndVertices). Caller holds the render lock.
  void FlushPendingBeginNow(uint8_t *base);

  // Forget the reusable float constant buffers (the next draw uploads its constants). Clear / Resolve call it: D3D
  // draws them with its own shaders and constants (review finding 3).
  void InvalidateConstantBuffers();

  // Split screen: several scene passes in one frame, each player's view resolving into the same textures again
  // (stage1/subagents/split.md). Surfaces::Resolve calls OnResolveInto after every copy into a resolve mirror
  // (render lock held; `source` = the guest surface resolved, for the log):
  // - a depth resolve into a mirror that already had one in this frame makes the frame multi-pass (single-view frames
  //   resolve each of their six depth textures once: Datacore, Jungle, Nightclub pass dumps), held for 120 frames;
  // - while pdz_native_split_tables / pdz_native_split_geometry act (multi-pass frames; _tables 2: every frame), a
  //   resolve into a mirror that a descriptor table of the current pass segment samples starts a new segment: later
  //   draws build their SRV tables again (Textures::Lookup transitions the mirror back to a shader resource and
  //   refreshes its alias views) and hash their geometry again (pdz_native_geometry_hash_once per segment).
  // Single-pass frames are unchanged.
  void OnResolveInto(HostTexture &mirror, bool depth, uint32_t source);
  // pdz_native_texture_lock_tables (stage2/polish/textflicker.md): the CPU locked `t` for writing (Textures::NoteCpuWrite,
  // in draw order). The SRV tables are keyed by the draw's shaders and raw fetch constants, so a draw after the write
  // with the same shaders and fetch constants as one before it found that table and never reached Textures::Lookup: it
  // sampled the version uploaded before the write. If a table of the current table epoch binds `t`, a new epoch starts
  // (the draws after the write build their tables again; the ones before keep theirs). Returns whether it started one.
  // Render lock held.
  bool OnTextureRewritten(const HostTexture &t);
  // True while split screen is detected (a multi-pass frame within the last 120 frames). Render lock held.
  bool SplitActive();

  const DrawStats &GetDrawStats();

  // A 2D or stacked texture (fetch dimension 1) under a shader's tfetch3D slot: true = bind its volume view
  // (Textures::LookupVolume), false = the slot stays null (a kind mismatch the SDK never has). Decided by
  // pdz_native_volume_view (all), pdz_native_volume_view_stacked (stacked only), pdz_native_volume_view_2d (plain
  // 2D only) and pdz_native_volume_view_dxn (k_DXN normal maps, stacked or not; default on:
  // stage1/subagents/water2.md). BindSrvTable and the draw dump use it.
  bool VolumeViewFor(const FetchDecoded &fetch);

  // The geometry cache (memory log, native_memlog.cpp): persistent upload-heap chunks of 32 MB, at most 8, recycled
  // whole (least recently used) once every chunk is full; pdz_native_geometry_shrink releases chunks idle for 3600
  // frames down to 2.
  struct GeometryCacheStats
  {
    uint32_t chunks = 0;      // live chunks
    uint64_t chunk_bytes = 0; // chunks x 32 MB
    uint64_t used_bytes = 0;  // filled part of the chunks (stale copies included until their chunk is recycled)
    uint64_t entries = 0;     // cached ranges
    uint64_t recycles = 0;    // chunks recycled (total)
    uint64_t released = 0;    // chunks released by pdz_native_geometry_shrink (total)
  };
  GeometryCacheStats GetGeometryCacheStats();

  // The content-cached sampler tables of the persistent sampler region (memory log): tables held now, tables the
  // region had no room for (each came from the per-frame ring instead; a ring refusal there skips the draw and is
  // counted by the device), tables freed by pdz_native_sampler_evict and its passes (at most one per frame).
  struct SamplerCacheStats
  {
    uint64_t tables = 0;
    uint64_t region_refused = 0;
    uint64_t evicted = 0;
    uint64_t passes = 0;
  };
  SamplerCacheStats GetSamplerCacheStats();
  // Frame trace: every traced draw is wrapped in a pipeline-statistics and an occlusion query; this resolves them
  // (one Flush) and logs one "pdz_trace: q" line per draw (vertices, clipper primitives, PS invocations, samples
  // that passed depth / stencil). Called when the traced frame ends, with the frame still open.
  void ResolveTraceQueries();

  void LogDrawStats(const char *why);

  // The guest physical memory write watches (pdz_native_geometry_watch's module in native_draw.cpp; record thread):
  // Ready() = the invalidation callback is registered and the self-test didn't fail; GenSum = the sum of the pages'
  // generations over [address, address + bytes) (raised by every invalidation); Arm = watch the range's pages.
  namespace guestwatch
  {
    bool Ready();
    uint64_t GenSum(uint32_t physical_address, uint32_t bytes);
    void Arm(uint32_t physical_address, uint32_t bytes);
  } // namespace guestwatch
} // namespace pdz::native
