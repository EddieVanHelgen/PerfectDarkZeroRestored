// Native renderer (stage 1): PsParamGen for the fork's pixel shaders, standard library only (checked offline against
// the shader cache, stage1/subagents/jungle.md). Applied together with the co-issue fix at a pixel shader's first draw
// (native_coissue.cpp, pdz_native_param_gen).
//
// With SQ_PROGRAM_CNTL.param_gen the hardware writes the pixel parameters into register SQ_CONTEXT_MISC.param_gen_pos
// instead of an interpolator (SDK Shader::GetInterpolatorInputMask and DxbcShaderTranslator::StartPixelShader; for a
// polygon: x = the pixel x (.0) with the sign bit set on a back face, even for 0; y = the pixel y (.0), sign bit = point;
// z = sign bit "line"; w = 0). D3D's program loader writes both registers from the container's program-info block:
// word 0 is the shader's SQ_PROGRAM_CNTL (param_gen = bit 18), word 1 its SQ_CONTEXT_MISC (param_gen_pos = bits 15:8;
// 4 = no param gen, 0x406 / 0x506 / 0x606 = registers 4 / 5 / 6). The XenosRecomp fork ignores both: it copies the
// register from TEXCOORD<pos>, which the VS never writes (0.0), so every face reads as a front face.
//
// 425 of the PDZ pixel-shader containers enable it, all to test the face: `rcpc rT.w, rP.x` then `cndge` on the sign
// (321), or `cndge` on rP.x itself (4). Two-sided foliage and materials flip their normal on back faces, and the
// G-buffer foliage shaders write the back-face flag (`cndge r3.w, rcpc(r5.x), 0, 0.5` -> RT1.w = 0.5) that the sun pass
// a95c4b61 turns into a normal flip (`cndgt r3.xy, RT1.w, -r3.xy, r3.xy`). Jungle capture 10: RT1.w is 0.5 on 19 % of
// the pixels emulated and 0 everywhere natively; the light-buffer specular (alpha) of those back faces is 1.41 natively
// against 0.11 emulated.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace pdz::native::paramgen
{
  constexpr uint32_t kNone = ~0u;

  inline uint32_t ReadBe32(const uint8_t *p)
  {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
  }

  // The PsParamGen register of a pixel shader from its 0x102A0E00 container (guest byte order; `size` readable bytes
  // from the container start, at least its header). kNone: not a pixel-shader container, no program-info block,
  // param_gen off, or a register past the 16 interpolator registers (the SDK's bound).
  inline uint32_t RegisterFromContainer(const uint8_t *c, size_t size)
  {
    if (!c || size < 0x18 || ReadBe32(c) != 0x102A0E00u)
    {
      return kNone;
    }
    const uint32_t header = ReadBe32(c + 4);
    const uint32_t program = ReadBe32(c + 0x14);
    if (header > size || program < 0x18 || program > header || header - program < 8)
    {
      return kNone;
    }
    const uint32_t program_cntl = ReadBe32(c + program);
    const uint32_t context_misc = ReadBe32(c + program + 4);
    if (((program_cntl >> 18) & 1u) == 0)
    {
      return kNone;
    }
    const uint32_t reg = (context_misc >> 8) & 0xFFu;
    return reg < 16 ? reg : kNone;
  }

  // The fork's initialisation of that register at the top of main() becomes the PsParamGen value (the window offset the
  // SDK adds back is left out: PDZ only reads the sign of x). The fork emits "\tfloat4 r<n> = i<n>;" for a register it
  // found live-in and "\tfloat4 r<n> = 0.0;" otherwise; its live-in scan is per register, not per component, so 15
  // shaders that read the face only in a co-issued scalar slot (`max r5.x, r2.w, r2.w + rcpc r5.w, r5.x` after r5.yz
  // were written; the co-issue fix makes the rcpc read the old r5.x) get the second form: both are replaced. false
  // with `why` empty: the source already computes it (a fork with PsParamGen, stage1/subagents/jungle_paramgen_fork.patch,
  // emits the same floor(iPos.xy) line); false with `why` set: the source is not shaped like the fork's.
  inline bool Patch(const std::string &hlsl, uint32_t reg, std::string &out, std::string &why)
  {
    why.clear();
    if (reg >= 16)
    {
      why = "register out of range";
      return false;
    }
    if (hlsl.find("= float4(floor(iPos.xy), 0.0, 0.0);") != std::string::npos)
    {
      return false;
    }
    const std::string r = "r" + std::to_string(reg);
    const size_t main_at = hlsl.find("void main(");
    if (main_at == std::string::npos)
    {
      why = "no main()";
      return false;
    }
    size_t at = std::string::npos, end = std::string::npos;
    for (const std::string &line : {"\tfloat4 " + r + " = i" + std::to_string(reg) + ";", "\tfloat4 " + r + " = 0.0;"})
    {
      for (size_t p = hlsl.find(line, main_at); p != std::string::npos; p = hlsl.find(line, p + 1))
      {
        const size_t e = p + line.size();
        const bool starts_line = p == 0 || hlsl[p - 1] == '\n';
        const bool ends_line = e == hlsl.size() || hlsl[e] == '\n' || hlsl[e] == '\r';
        if (starts_line && ends_line && (at == std::string::npos || p < at))
        {
          at = p;
          end = e;
          break;
        }
      }
    }
    if (at == std::string::npos)
    {
      why = "no initialisation of " + r + " at the top of main()";
      return false;
    }
    if (hlsl.find("iPos : SV_Position") == std::string::npos || hlsl.find("iFace : SV_IsFrontFace") == std::string::npos)
    {
      why = "no iPos / iFace input in the entry point";
      return false;
    }
    const char *newline = (end < hlsl.size() && hlsl[end] == '\r') ? "\r\n" : "\n";
    out.clear();
    out.reserve(hlsl.size() + 160);
    out.append(hlsl, 0, at);
    out += "\tfloat4 " + r + " = float4(floor(iPos.xy), 0.0, 0.0); // PsParamGen (native_paramgen.h)";
    out += newline;
    out += "\t" + r + ".x = iFace != 0u ? " + r + ".x : asfloat(asuint(" + r + ".x) | 0x80000000u);";
    out.append(hlsl, end, std::string::npos); // the original line ending and the rest
    return true;
  }
} // namespace pdz::native::paramgen
