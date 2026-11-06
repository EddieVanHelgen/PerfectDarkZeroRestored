// Native renderer (stage 1): the SV_Depth 20e4 pixel-shader variant (PIPELINE_NOTES.md section 6, "ApplyShaderDepthBias").
// Draws into a guest D24FS8 depth buffer (host D32_FLOAT_S8X24_UINT) get a variant of their pixel shader that writes
// SV_Depth = the guest's 20e4 depth: the polygon offset applied in the shader the way the SDK's ROV path does it
// (the reference renders with render_target_path_d3d12 = "rov"), then rounded to the nearest even 20e4 value and
// converted back to float32 exactly. The variant is built at run time from the fork's .hlsl
// (native_depth20e4_hlsl.h) with the XenosRecomp fork's dxcompiler.dll / dxil.dll (signed DXIL), cached per pixel
// shader. Off by default (pdz_native_depth20e4 = 0): the fixed-function bias path is unchanged.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <rex/cvar.h>

#include "native_pipeline.h"

REXCVAR_DECLARE(int32_t, pdz_native_depth20e4);
REXCVAR_DECLARE(std::string, pdz_native_dxc_dir);
REXCVAR_DECLARE(bool, pdz_native_depth20e4_edge);

namespace pdz::native::depth20e4
{
  enum Mode : uint32_t
  {
    kOff = 0,           // fixed-function D3D12 depth bias (GetD3D10IntegerPolygonOffset units), float32 depth
    kPolygonOffset = 1, // only draws with a polygon offset on a D24FS8 target use the variant
    kAll = 2,           // every depth-tested draw on a D24FS8 target (all stored depth is 20e4, as the ROV's)
  };
  uint32_t CurrentMode();

  // The ROV's edram_poly_offset_front / back for this draw (command_processor.cpp:4545-4581): x, y = front face
  // (slope scale per host pixel = register scale / 16 * resolution scale, offset in guest depth units), z, w =
  // back face. Lines / points use the front registers for both faces when poly_offset_para_enable is set.
  void Constants(const RenderState &rs, bool polygonal, uint32_t resolution_scale, float out[4]);

  // Whether the draw should be drawn with the variant (mode, D24FS8 depth bound, depth test on; mode 1 also needs
  // a non-zero offset on some face).
  bool Wanted(const NativeShader *ps, const RenderState &rs, const TargetFormats &tf, bool polygonal);

  // pdz_native_depth20e4_edge (default off): the compare edge for a variant draw (native_depth20e4_hlsl.h
  // EdgeHelperSource): 1 = the upper boundary of the 20e4 rounding interval (depth function GREATER_EQUAL / LESS),
  // 2 = the lower one (LESS_EQUAL / GREATER), 0 = the 20e4 value itself (the cvar off, the draw writes depth, no
  // polygon offset, or another depth function). Only meaningful when Wanted() is true.
  uint32_t EdgeFor(const RenderState &rs, bool polygonal);

  // The variant DXIL of `ps` (the null pixel shader, empty DXIL, gets the depth-only shader). Built on first use
  // (synchronously, on the render thread: median 14 ms, max ~40 ms per PS measured offline) and cached by PS hash
  // in memory and on disk (<shader dir>\depth20e4\<ps>_<source hash>.dxil, reused by later runs); nullptr when it
  // cannot be built (logged once per shader, counted; the draw then keeps the fixed-function bias).
  // `edge` (EdgeFor) selects a separate variant per compare edge; 0 = the plain variant (unchanged source).
  const std::vector<uint8_t> *Variant(const NativeShader *ps, uint32_t edge = 0);

  // Retires the variant of `ps` (its PSO could not be created): Variant() returns nullptr from now on, the draws
  // fall back to the fixed-function bias. Logged once, counted as failed.
  void MarkFailed(const NativeShader *ps, const char *why, uint32_t edge = 0);

  // Compiles a pixel shader source with the variants' compiler and arguments (the fork's dxcompiler.dll / dxil.dll,
  // `-T ps_6_0 -E main -HV 2021 -all-resources-bound ...` as XenosRecomp/dxc_compiler.cpp): signed DXIL in `dxil`.
  // Shared with native_coissue.cpp (render thread). False with `errors` when there is no compiler or it fails.
  bool CompilePixelShader(const std::string &source, std::vector<uint8_t> &dxil, std::string &errors);
  // A DXBC container whose hash is set (signed by dxil.dll; CreateGraphicsPipelineState rejects unsigned DXIL).
  bool IsSignedDxil(const std::vector<uint8_t> &dxil);
  // Compiles run so far by CompilePixelShader and the variants (the first-use log tells a compile from a disk read).
  uint32_t CompileCount();
  // A patch / variant cache file (<shader dir>\coissue | depth20e4 | blend7e3): written now, or during the pipeline
  // list's pass (batch results pending) queued and written on several threads by batch::Clear.
  void SaveCacheFile(const std::string &path, const std::vector<uint8_t> &bytes);
  // The pipeline list's first-start pass (native_pipeline_seed.cpp, 2026-11-03), on the draw thread or before the
  // draws begin. BeginCollect: from now every compile of CompilePixelShader and Variant() (so of the co-issue fix and
  // both variant kinds) records its source and returns a placeholder, kept off disk (false: no compiler). EndCollect
  // forgets what the pass built (the variants and their counters; the co-issue fix's are coissue::ResetForSeed's).
  // CompileCollected compiles the recorded sources on `threads` workers, each with its own compiler; the compiles
  // after it take those results by source (no DXC call) until Clear.
  namespace batch
  {
    struct Result
    {
      uint32_t sources = 0, compiled = 0, failed = 0, threads = 0;
      double ms = 0.0;
    };
    bool BeginCollect();
    void EndCollect();
    bool Collecting();
    Result CompileCollected(uint32_t threads);
    void Clear();
  } // namespace batch

  // pdz_native_dxc_preload (default on): loads the compiler on a detached worker thread now (PipelineCache::Init), so
  // the first compile of a session (a pixel shader not in the disk caches) does not also pay the DLL load. Once.
  void PreloadCompiler();

  struct Stats
  {
    uint32_t built = 0;      // variants available (compiled, or read from the disk cache)
    uint32_t from_disk = 0;  // ... of which read from <shader dir>\depth20e4
    uint32_t failed = 0;     // variants that could not be built (no .hlsl, unexpected shape, compile error)
    uint32_t compile_ms = 0; // total compile time
    uint64_t draws = 0;      // draws issued with a variant
    uint32_t edge_built = 0; // ... variants of them with a compare edge (pdz_native_depth20e4_edge)
    bool compiler_failed = false;
  };
  const Stats &GetStats();
  void CountDraw();
  // "depth20e4 mode M: variants B (C from disk, failed F, T ms) draws D" for the periodic draw line.
  std::string StatsText();
} // namespace pdz::native::depth20e4
