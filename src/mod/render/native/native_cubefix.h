// Native renderer (stage 1): the Xenos `cube` ALU instruction and tfetchCube for the fork's pixel shaders, standard
// library only (checked offline against the whole shader cache: stage1/subagents/mpmaps.md). Applied together with the
// co-issue and PsParamGen patches at a pixel shader's first draw (native_coissue.cpp, pdz_native_cube_fix).
//
// The XenosRecomp fork's `cube()` helper returns float4(0, 0, 0, index) and keeps the direction in a two-entry side
// structure (CubeMapData) that its tfetchCube reads back by that index. That serves one cube instruction feeding the
// next tfetchCube and nothing else. 4 of the 2,177 cached shaders use the instruction, all from the exe:
//   * 858ECD686F0B70C5, a light-buffer pass: an omni light whose animated cookie (a 256x256x32 DXT1 stack) is projected
//     with the cube face coordinates: `cube r1, r0.zzxy, r0.yxzz`, `rcpf r1.z`, `mad r1.xy, r1, r1.z, 1.5`,
//     `tfetch3D r1.yxw` with w = the animation phase c12.w. Drawn in Nightclub, Mansion (21 draws a frame), MP Urban,
//     Temple, Gasplant and Sabotage Urban. 8C54972038AE1685 does the same lookup. With (0, 0, 0) every pixel samples
//     texel (1.5, 1.5), i.e. the wrapped centre of the cookie's current layer: a uniform light whose brightness
//     follows that one texel (Gasplant's ceiling: 31 % of the map's MAE).
//   * 24D8A196ED9FC7B5 runs five cube instructions before its five tfetchCube: the side structure keeps the last two
//     directions, so three of the taps sample another tap's direction. 3B5B9DFCF976E5B2 (one tap) is right either way.
// The SDK computes the hardware result (dxbc_translator_alu.cpp, kCube): x = T coordinate, y = S coordinate,
// z = 2 x the signed major axis, w = the face id (0 +X, 1 -X, 2 +Y, 3 -Y, 4 +Z, 5 -Z); its tfetchCube turns (S, T) in
// 1..2 and the face id back into a direction (dxbc_translator_fetch.cpp, kCube). The patch replaces both helpers with
// those formulas and the tfetchCubeLevel call sites' inline side-structure read with the same reconstruction, so the
// standard pattern (xy / |z| + 1.5, fetch .yxw) and every other use of the face coordinates see what the SDK sees.
#pragma once

#include <cstddef>
#include <string>
#include <utility>

namespace pdz::native::cubefix
{
  // The fork's helpers, as every .hlsl it writes has them (shader_common_pdz.hlsli; LF line ends).
  inline const char *ForkCube()
  {
    return "float4 cube(float4 value, inout CubeMapData data)\n"
           "{\n"
           "    uint index = data.index;\n"
           "    data.directions[index] = value.xyz;\n"
           "    data.index = (index + 1u) & 1u;\n"
           "    return float4(0.0, 0.0, 0.0, index);\n"
           "}\n";
  }

  inline const char *ForkFetch()
  {
    return "float4 tfetchCubeData(TextureCube<float4> t, SamplerState s, float3 coord, CubeMapData data)\n"
           "{\n"
           "    return t.Sample(s, data.directions[uint(coord.z) & 1u]);\n"
           "}\n";
  }

  // The SDK's cube instruction. The side structure is still filled (nothing patched reads it).
  inline const char *SdkCube()
  {
    return "// pdz native (native_cubefix.h): the Xenos cube instruction as the SDK computes it (dxbc_translator_alu.cpp\n"
           "// kCube): x = T coordinate, y = S coordinate, z = 2 * major axis (signed), w = face id.\n"
           "float4 cube(float4 value, inout CubeMapData data)\n"
           "{\n"
           "    uint index = data.index;\n"
           "    data.directions[index] = value.xyz;\n"
           "    data.index = (index + 1u) & 1u;\n"
           "    float x = value.x;\n"
           "    float y = value.y;\n"
           "    float z = value.z;\n"
           "    if (abs(z) >= abs(x) && abs(z) >= abs(y))\n"
           "    {\n"
           "        return float4(-y, (z < 0.0) ? -x : x, 2.0 * z, (z < 0.0) ? 5.0 : 4.0);\n"
           "    }\n"
           "    if (abs(y) >= abs(x))\n"
           "    {\n"
           "        return float4((y < 0.0) ? -z : z, x, 2.0 * y, (y < 0.0) ? 3.0 : 2.0);\n"
           "    }\n"
           "    return float4(-y, (x < 0.0) ? z : -z, 2.0 * x, (x < 0.0) ? 1.0 : 0.0);\n"
           "}\n";
  }

  // tfetchCube's coordinate (S and T in 1..2, the face id) back to a direction, as the SDK's fetch does it.
  inline const char *SdkFetch()
  {
    return "// pdz native (native_cubefix.h): tfetchCube's (S, T, face id) back to a direction (dxbc_translator_fetch.cpp\n"
           "// kCube: S / T from 1..2 to -1..1, face = min(ftou(z), 5), axis = face >> 1, negative = face & 1).\n"
           "float3 pdzCubeDirection(float3 coord)\n"
           "{\n"
           "    float sc = coord.x * 2.0 - 3.0;\n"
           "    float tc = coord.y * 2.0 - 3.0;\n"
           "    uint face = uint(clamp(coord.z, 0.0, 5.0));\n"
           "    bool neg = (face & 1u) != 0u;\n"
           "    uint axis = face >> 1u;\n"
           "    if (axis == 0u)\n"
           "    {\n"
           "        return float3(neg ? -1.0 : 1.0, -tc, neg ? sc : -sc);\n"
           "    }\n"
           "    if (axis == 1u)\n"
           "    {\n"
           "        return float3(sc, neg ? -1.0 : 1.0, neg ? -tc : tc);\n"
           "    }\n"
           "    return float3(neg ? -sc : sc, -tc, neg ? -1.0 : 1.0);\n"
           "}\n"
           "\n"
           "float4 tfetchCubeData(TextureCube<float4> t, SamplerState s, float3 coord, CubeMapData data)\n"
           "{\n"
           "    return t.Sample(s, pdzCubeDirection(coord));\n"
           "}\n";
  }

  // The fork's text with CR LF line ends, for a .hlsl saved that way.
  inline std::string WithCrLf(const std::string &s)
  {
    std::string out;
    out.reserve(s.size() + 32);
    for (char c : s)
    {
      if (c == '\n')
      {
        out += '\r';
      }
      out += c;
    }
    return out;
  }

  // Replaces the one occurrence of `from` (LF, or CR LF when the file uses it) by `to` (same line ends). False when it
  // is missing or not unique.
  inline bool ReplaceOnce(std::string &text, const std::string &from_lf, const std::string &to_lf)
  {
    for (const bool crlf : {false, true})
    {
      const std::string from = crlf ? WithCrLf(from_lf) : from_lf;
      const size_t at = text.find(from);
      if (at == std::string::npos)
      {
        continue;
      }
      if (text.find(from, at + 1) != std::string::npos)
      {
        return false;
      }
      text.replace(at, from.size(), crlf ? WithCrLf(to_lf) : to_lf);
      return true;
    }
    return false;
  }

  // main() runs the cube instruction or fetches from a cube map.
  inline bool Uses(const std::string &hlsl)
  {
    const size_t main_at = hlsl.find("void main(");
    if (main_at == std::string::npos)
    {
      return false;
    }
    return hlsl.find(" cube(", main_at) != std::string::npos ||
           hlsl.find("tfetchCubeData(", main_at) != std::string::npos ||
           hlsl.find("cubeMapData.directions[", main_at) != std::string::npos;
  }

  // The patched source in `out`. false with `why` empty: main() uses neither (or the source is patched already);
  // false with `why` set: the source is not shaped like the fork's.
  inline bool Patch(const std::string &hlsl, std::string &out, std::string &why)
  {
    why.clear();
    if (!Uses(hlsl) || hlsl.find("float3 pdzCubeDirection(float3 coord)") != std::string::npos)
    {
      return false;
    }
    std::string text = hlsl;
    if (!ReplaceOnce(text, ForkCube(), SdkCube()))
    {
      why = "the fork's cube() helper not found (or not once)";
      return false;
    }
    if (!ReplaceOnce(text, ForkFetch(), SdkFetch()))
    {
      why = "the fork's tfetchCubeData() helper not found (or not once)";
      return false;
    }
    // tfetchCubeLevel call sites: "cubeMapData.directions[uint(<coord>.z) & 1u]" -> "pdzCubeDirection(<coord>)".
    const std::string head = "cubeMapData.directions[uint(", tail = ".z) & 1u]";
    const size_t main_at = text.find("void main(");
    for (size_t at = text.find(head, main_at); at != std::string::npos; at = text.find(head, at))
    {
      const size_t expr = at + head.size();
      const size_t end = text.find(tail, expr);
      const size_t line_end = text.find('\n', expr);
      if (end == std::string::npos || (line_end != std::string::npos && end > line_end))
      {
        why = "a tfetchCubeLevel call site not shaped like the fork's";
        return false;
      }
      const std::string replacement = "pdzCubeDirection(" + text.substr(expr, end - expr) + ")";
      text.replace(at, end + tail.size() - at, replacement);
      at += replacement.size();
    }
    if (text.find("cubeMapData.directions[", main_at) != std::string::npos)
    {
      why = "main() reads the side structure in a way the patch does not know";
      return false;
    }
    out = std::move(text);
    return true;
  }
} // namespace pdz::native::cubefix
