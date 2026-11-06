// Native renderer (stage 1): blending into 7e3 colour targets as the SDK's ROV path does (native_blend7e3.cpp).
//
// The guest's HDR scene target (403C16D0 in every level, k_2_10_10_10_FLOAT / _AS_16_16_16_16: 7e3 = 3-bit exponent,
// 7-bit mantissa per colour channel, 0..31.875, 2-bit alpha) is R16G16B16A16_FLOAT on the host and blended by the
// fixed-function D3D12 blender: float16 per blend, no clamping. The reference (the SDK's ROV output merger,
// render_target_path_d3d12 = "rov") clamps the blending source ALPHA to 0..1 (7e3 has a fixed-point alpha), blends in
// float32, clamps the result to 0..31.875 / 0..1 and rounds it to 7e3 (nearest even) and to the 2-bit alpha after every
// blend (stage1/subagents/hdr7e3.md has the code references and the Datacore numbers).
//
// pdz_native_7e3_blend (default 1 since a550399; 0 = off, the fixed-function blend), for the draws that BLEND into a 7e3
// render target 0 (non-blended writes need nothing: their readers round, see below):
//   1 = the pixel shader's alpha output is clamped to 0..1 before the fixed-function blend (a PS variant built like
//       the depth20e4 one: the guest main renamed, a new main calling it). The SDK's source-alpha clamp only.
//   2 = exact: a PS variant with [earlydepthstencil] and no colour output reads the stored value through a
//       rasterizer-ordered view of the surface (u0 space0), blends like the SDK and writes the rounded result back
//       (native_blend7e3_hlsl.h). Only for a single colour target without depth / stencil writes (early depth /
//       stencil then equals the late test) on a surface created UAV-capable (7e3 colour surfaces are, when this mode
//       is set at their creation: set it on the command line); other blended 7e3 draws fall back to mode 1 (counted).
//       Resolves of 7e3 surfaces also round to 7e3 (native_passes.cpp flag 8), so a non-blended write reads back as
//       the EDRAM would hold it. Needs ROVs and typed UAV loads of R16G16B16A16_FLOAT (checked; else mode 1).
// Read per draw (PSO key) and when a surface is created; the variants are built at a pixel shader's first use with
// the fork's dxcompiler (the depth20e4 compiler) and cached in <shader dir>\blend7e3.
//
// pdz_native_7e3_resolve_round (default off) is that resolve rounding alone, in every blend mode. It matters where the
// game keeps state in the 7e3 target from frame to frame: the exposure loop (PS 8156AB53, La' = La + (1 - 0.95^(30 dt))
// (L - La) once a frame, resolved to the k_16_FLOAT textures 1E089000 / 1E08B000) only moves when a frame's step
// reaches half a 7e3 step, so on the console and in the emulator the adapted luminance stops within +-0.039 of L while
// L < 0.5 (60 fps; 8..16 % of L between 0.25 and 0.5, more below), where float16 converges to L. stage1/subagents/river.md.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <rex/cvar.h>

#include "native_pipeline.h"

REXCVAR_DECLARE(int32_t, pdz_native_7e3_blend);
REXCVAR_DECLARE(bool, pdz_native_7e3_resolve_round);

namespace pdz::native
{
  struct NativeShader;
}

namespace pdz::native::blend7e3
{
  enum Mode : uint32_t
  {
    kOff = 0,
    kAlphaClamp = 1, // PS variant: oC0.a clamped to 0..1, fixed-function blend
    kRov = 2,        // PS variant: the SDK's output merger through a rasterizer-ordered view
  };
  uint32_t CurrentMode();

  // Mode 2's device requirements (ROVs, typed UAV load / store of R16G16B16A16_FLOAT), checked once, logged.
  bool RovSupported();
  // A 7e3 colour surface created now gets D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS (mode 2 and supported).
  bool WantUavSurfaces();
  // Resolves of 7e3 surfaces round to 7e3 (mode 2, or pdz_native_7e3_resolve_round).
  bool RoundResolves();

  // The variant mode a draw should use (0 = none): blending on render target 0, a 7e3 surface there, a pixel shader
  // writing exactly oC0 and no depth. Mode 2 needs one colour target, no depth / stencil writes and a UAV-capable
  // surface, else it falls back to mode 1 (counted by reason).
  uint32_t Wanted(const NativeShader *ps, const RenderState &rs, const TargetFormats &tf);

  // The variant DXIL of `ps` for `mode` (1 or 2), built at first use (synchronously on the render thread, ~15 ms, or
  // read from <shader dir>\blend7e3\<ps>_<mode>_<source hash>.dxil); nullptr when it cannot be built (logged, counted).
  const std::vector<uint8_t> *Variant(const NativeShader *ps, uint32_t mode);
  // The PSO with the variant failed: the variant is retired (nullptr from now on), logged once.
  void MarkFailed(const NativeShader *ps, uint32_t mode, const char *why);

  // Mode 2's root constants (native_pipeline.h kRootConstBlend7e3, b4 space0): RB_BLENDCONTROL0 & 0x1FFF1FFF,
  // RB_COLOR_MASK & 0xF, 2^RB_COLOR_INFO.color_exp_bias (float bits), 0, RB_BLEND_RED..ALPHA (float bits).
  constexpr uint32_t kConstantCount = 8;
  void Constants(const RenderState &rs, uint32_t out[kConstantCount]);

  void CountDraw(uint32_t mode);
  // Mode 2 was wanted but its variant could not be built: mode 1 used instead (pipeline).
  void CountVariantFallback();
  // After the pipeline list's collect pass (depth20e4::batch): forgets the variants it built and its counters.
  void ResetForSeed();
  // "7e3 blend mode M:variants B (C from disk, failed F, T ms) draws alpha-clamp A rov R, rov fallbacks: ..." for the
  // periodic draw line.
  std::string StatsText();
} // namespace pdz::native::blend7e3
