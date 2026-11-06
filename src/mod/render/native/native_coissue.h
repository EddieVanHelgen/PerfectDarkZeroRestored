// Native renderer (stage 1): co-issued ALU operations read their sources before either writes (native_coissue.cpp).
//
// A Xenos ALU instruction pairs a vector and a scalar operation. The hardware reads both operations' sources before
// either result is written; the SDK's DxbcShaderTranslator::ProcessAluInstruction computes the vector result into a
// temporary, runs the scalar operation, then stores both ("Both operations must be executed before any result is
// stored if vector and scalar operations are paired", shader.h), and the Xbox 360 shader compiler relies on it (the
// shadow-mask blur 26eaa1f1 sums the previous tap's weight in the scalar slot while the vector slot computes the next
// one). The XenosRecomp fork emits the vector write first ("r1.z = r1.z + c[0].z;" then "p0 = r1.z > 0.0;"), so a
// scalar operation reading a register component the vector operation writes sees the new value. 215 of the 1,637
// cached PDZ shaders with a container have such an instruction (all pixel shaders; 252 of 2,428 containers), among
// them the sun / ambient light pass a95c4b61c177a282 of every level: the rim term becomes exp2(N.L) instead of
// rim ^ (4 RT0.z), so the light buffer gains (1..2) x sun colour x G-buffer RT0 alpha (Joanna's hair in Bridge and
// Arena about 5x too bright), and p0 = depth + c0.z > 0 instead of depth > 0 lights the pixels without geometry.
//
// Fix, at a pixel shader's first use (pdz_native_coissue_fix, on): the microcode (guest memory, the create hook's
// pointer, checked against the shader's XXH3) is scanned for such instructions; the fork's .hlsl next to the .dxil is
// patched by moving the scalar line(s) ("p0 = ..." for setp, "ps = ...") of each one above its vector line (the n-th
// "ps = " line of the HLSL is the n-th instruction with a scalar operation: checked over all 1,637 shaders),
// compiled with the fork's dxcompiler (the depth20e4 compiler, the fork's arguments) and the shader's DXIL replaced.
// Compiled DXIL is cached in <shader dir>\coissue\<ps>_<XXH3 of the patched source>.dxil for later runs. Any
// mismatch leaves the shader as it was (logged, counted). stage1\subagents\bridge.md has the analysis.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <rex/cvar.h>

#include "native_coissue_scan.h" // Hazard, ScanResult, Scan(), Patch()

REXCVAR_DECLARE(bool, pdz_native_coissue_fix);
REXCVAR_DECLARE(bool, pdz_native_param_gen);
REXCVAR_DECLARE(bool, pdz_native_cube_fix);
REXCVAR_DECLARE(bool, pdz_native_rcpf_zero);
REXCVAR_DECLARE(bool, pdz_native_stacked_array);
REXCVAR_DECLARE(bool, pdz_native_live_literals);

namespace pdz::native
{
  struct NativeShader;
}

namespace pdz::native::coissue
{
  // The fixes for one pixel shader at its first use (render thread): scan, patch the fork's .hlsl found next to the
  // DXIL, compile it or read the cached DXIL, and replace `s.dxil` (the patched source is kept in `s.coissue_hlsl` for
  // the depth20e4 variants). Three patches share the compile: the co-issue one (pdz_native_coissue_fix), the
  // PsParamGen one (pdz_native_param_gen, native_paramgen.h: the register from the container's program-info block,
  // `container_be` / `container_bytes` = the container in guest memory, header + microcode, or null; else the research
  // cache's copy), the cube one (pdz_native_cube_fix, native_cubefix.h: the cube instruction and tfetchCube as the
  // SDK computes them), rcpf / rsqf +-0 (pdz_native_rcpf_zero) and the stacked-array tfetch3D (pdz_native_stacked_array:
  // sets `s.array_slots`), both native_hlslfix.h, and the literal registers read from the uploaded constants
  // (pdz_native_live_literals, native_hlslfix.h). No-op when the cvars are off, the shader needs none, or it was
  // already checked.
  void Apply(NativeShader &s, const uint8_t *ucode_be, uint32_t bytes, const uint8_t *container_be,
             uint32_t container_bytes);

  struct Stats
  {
    uint32_t checked = 0;    // pixel shaders scanned
    uint32_t hazardous = 0;  // ... with at least one hazard
    uint32_t hazards = 0;    // hazardous instructions in them
    uint32_t patched = 0;    // shaders now drawn with the patched DXIL
    uint32_t from_disk = 0;  // ... of which read from <shader dir>\coissue
    uint32_t failed = 0;     // shaders left as they were (microcode mismatch, no .hlsl, shape mismatch, compile error)
    uint32_t compile_ms = 0; // total compile time
  };
  const Stats &GetStats();

  struct ParamGenStats
  {
    uint32_t checked = 0;   // pixel shaders looked at
    uint32_t from_file = 0; // ... whose container came from the research cache (none in guest memory)
    uint32_t enabled = 0;   // ... whose container enables PsParamGen
    uint32_t patched = 0;   // shaders now drawn with the register initialised
    uint32_t already = 0;   // the translation computes it already (a fork with PsParamGen)
    uint32_t failed = 0;    // no .hlsl, shape mismatch, compile error
  };
  const ParamGenStats &GetParamGenStats();

  struct CubeFixStats
  {
    uint32_t checked = 0; // pixel shaders whose .hlsl was looked at
    uint32_t used = 0;    // ... whose main() runs the cube instruction or fetches from a cube map
    uint32_t patched = 0; // shaders now drawn with the SDK's cube / tfetchCube
    uint32_t failed = 0;  // shape mismatch, compile error
  };
  const CubeFixStats &GetCubeFixStats();

  // pdz_native_rcpf_zero / pdz_native_stacked_array / pdz_native_live_literals (native_hlslfix.h; for the live literals
  // with_sites = shaders declaring literals, sites = registers read from c[]).
  struct HlslFixStats
  {
    uint32_t checked = 0;    // pixel shaders looked at
    uint32_t with_sites = 0; // ... with rcpf / rsqf instructions (resp. a tfetch3D slot)
    uint32_t sites = 0;      // those instructions (resp. the tfetch3D slots rewritten)
    uint32_t patched = 0;    // shaders now drawn with the patch
    uint32_t from_disk = 0;  // ... of which read from <shader dir>\coissue
    uint32_t already = 0;    // nothing to rewrite (the translation has it already / no tfetch3D call in main())
    uint32_t failed = 0;     // no .hlsl, shape mismatch, compile error
  };
  const HlslFixStats &GetRcpfStats();
  const HlslFixStats &GetArrayStats();
  const HlslFixStats &GetLiveLiteralStats();
  // "co-issue fix on: checked C, hazardous H (I instructions), patched P (D from disk, T ms), failed F | param gen on:
  // checked C (F from the cache's containers), enabled E, patched P, already A, failed F | cube fix on: checked C,
  // using U, patched P, failed F | rcpf zero on: ... | stacked array on: ... | live literals on: ...".
  std::string StatsText();
  // After the pipeline list's collect pass (depth20e4::batch), which ran Apply on copies: its counts forgotten.
  void ResetForSeed();
} // namespace pdz::native::coissue
