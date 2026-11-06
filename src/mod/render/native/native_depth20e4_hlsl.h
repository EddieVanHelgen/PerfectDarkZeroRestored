// Native renderer (stage 1): HLSL text for the SV_Depth 20e4 pixel-shader variant (native_depth20e4.h). Standard
// library only, so the transform can be checked offline against the whole shader cache (dxc over every PS).
//
// What the variant computes is the ReXGlue SDK's ROV depth path (the reference renders with
// render_target_path_d3d12 = "rov"), see stage1\subagents\stubs.md:
//   * dxbc_translator.cpp:599-625 / dxbc_translator_om.cpp:491-574 (ROV_DepthStencilTest): the coarse
//     derivatives of SV_Position.z are taken at the start of the shader, before any guest control flow;
//     bias = max(|ddx z|, |ddy z|) * poly_offset_scale + poly_offset_offset of the face (SV_IsFrontFace),
//     depth = saturate(bias + z) (no MSAA: the centre sample).
//   * dxbc_translator_om.cpp:3573-3638 (PreClampedDepthTo20e4, round to nearest even, no remap) = SDK
//     xenos.cpp:129 Float32To20e4(f, true); the stored value is that 24-bit number. Here it is converted back to
//     float32 (xenos.cpp:154 Float20e4To32, exact), so the host D32 buffer holds exactly the guest's 20e4 values
//     and the host float compare gives the same result as the ROV's 24-bit integer compare (20e4 -> float32 is
//     exact and monotonic).
//   * A pixel shader that writes oDepth gets no polygon offset (ROV: shader_writes_depth branch), only the
//     20e4 rounding of its saturated depth.
// The bias constants come from root constants b3 space0 (native_pipeline.h kRootConstDepth):
//   x, y = front face (slope scale per host pixel, offset), z, w = back face.
#pragma once

#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

namespace pdz::native::depth20e4
{
  // Helpers appended to every variant (and the depth-only shader). The 20e4 conversion is the SDK's C++
  // xenos::Float32To20e4(f, true) / Float20e4To32 for a saturated input, written like native_passes.cpp.
  inline const char *HelperSource()
  {
    return R"HLSL(
// ---- pdz native: SV_Depth 20e4 variant (native_depth20e4.cpp) ----
cbuffer PdzDepth20e4Constants : register(b3, space0)
{
    float4 g_PdzPolyOffset; // x, y = front face slope scale (per host pixel), offset; z, w = back face
};

// xenos::Float32To20e4(f, true) for f in [0, 1]: round to the nearest even 20e4 value (24-bit result).
uint pdzFloat32To20e4(float f)
{
    uint u = (f > 0.0) ? min(asuint(f), 0x3F800000u) : 0u;
    uint shift = min(113u - min(u >> 23u, 113u), 24u);
    uint b = (u < 0x38800000u) ? (((u & 0x7FFFFFu) | 0x800000u) >> shift) : (u + 0xC8000000u);
    b += 3u + ((b >> 3u) & 1u);
    return (b >> 3u) & 0xFFFFFFu;
}

// xenos::Float20e4To32: exact.
float pdzFloat20e4To32(uint f24)
{
    uint mantissa = f24 & 0xFFFFFu;
    uint exponent = f24 >> 20u;
    bool is_denormalized = exponent == 0u;
    uint mantissa_lzcnt = 20u - firstbithigh(mantissa);
    exponent = is_denormalized ? (1u - mantissa_lzcnt) : exponent;
    mantissa = is_denormalized ? ((mantissa << mantissa_lzcnt) & 0xFFFFFu) : mantissa;
    return asfloat((f24 != 0u) ? (((exponent + 112u) << 23u) | (mantissa << 3u)) : 0u);
}

float pdzDepth20e4(float depth)
{
    return pdzFloat20e4To32(pdzFloat32To20e4(saturate(depth)));
}

// The ROV polygon offset (ROV_DepthStencilTest): slope from the coarse derivatives taken before the guest code.
float pdzBiasedDepth(float z, float slope, bool front)
{
    float2 offset = front ? g_PdzPolyOffset.xy : g_PdzPolyOffset.zw;
    return mad(slope, offset.x, offset.y) + z;
}
)HLSL";
  }

  // Edge variants (pdz_native_depth20e4_edge, native_depth20e4.h EdgeFor): appended after HelperSource() only when
  // edge != 0, so the edge-0 source (and its on-disk cache key) is byte for byte the plain variant's.
  //
  // In mode 1 only the polygon-offset draw is rounded to 20e4; the surface under it was drawn without the variant
  // and holds plain float32 depth zb. The ROV compares the two 20e4 numbers: q = 20e4(decal) against 20e4(zb).
  // Writing q itself compares q against the UNrounded zb, which differs whenever both round to the same q (or
  // neighbours) - i.e. for offsets under about one 20e4 step, the decal then loses or wins by where zb sits inside
  // its rounding interval: banded / speckled z-fighting that the ROV does not have. Writing the boundary of q's
  // rounding interval on the side the compare looks at gives the ROV's answer against an unrounded (or rounded) zb:
  //   edge 1 (GREATER_EQUAL, LESS): the upper boundary, mid(q, q + 1), one float32 ulp lower when q is odd
  //          (a zb exactly on the midpoint rounds to the even neighbour);
  //   edge 2 (LESS_EQUAL, GREATER): the lower boundary, mid(q - 1, q), one float32 ulp higher when q is odd;
  //          0 for q = 0.
  // The midpoints are exact in float32 (20e4 has 20 mantissa bits). Exact for zb in the normal 20e4 range
  // (>= 2^-14); below it xenos::Float32To20e4 truncates before rounding and the boundary can be a few float32 ulps
  // off (offline check: stage2\polish\zfight\edge_check.cpp). Only for draws that do not write depth (the stored
  // value would no longer be the 20e4 number).
  inline const char *EdgeHelperSource()
  {
    return R"HLSL(
// ---- pdz native: 20e4 compare-edge depth (pdz_native_depth20e4_edge) ----
float pdzDepth20e4Edge(float depth, uint edge)
{
    uint q = pdzFloat32To20e4(saturate(depth));
    float v = pdzFloat20e4To32(q);
    float d = 0.0;
    if (edge == 1u)
    {
        precise float m = (v + pdzFloat20e4To32(q + 1u)) * 0.5;
        d = asfloat(asuint(m) - (q & 1u));
    }
    else if (q != 0u)
    {
        precise float m = (pdzFloat20e4To32(q - 1u) + v) * 0.5;
        d = asfloat(asuint(m) + (q & 1u));
    }
    return saturate(d);
}
)HLSL";
  }

  // The depth expression of the epilogue: the 20e4 value, or the compare edge of it (edge 1 / 2).
  inline std::string DepthExpression(const std::string &depth, uint32_t edge)
  {
    if (edge == 0)
    {
      return "pdzDepth20e4(" + depth + ")";
    }
    return "pdzDepth20e4Edge(" + depth + ", " + std::to_string(edge) + "u)";
  }

  // The pixel shader for draws with no guest pixel shader (depth only): the fork's PS input signature
  // (SV_Position, TEXCOORD0..15, SV_IsFrontFace) so it links with every translated VS / the point GS, no colour
  // output (the PSO masks the colour writes).
  inline std::string DepthOnlySource(uint32_t edge = 0)
  {
    std::string s = HelperSource();
    if (edge != 0)
    {
      s += EdgeHelperSource();
    }
    s += "\n[shader(\"pixel\")]\nvoid main(\n\tin float4 iPos : SV_Position,\n";
    for (int i = 0; i < 16; ++i)
    {
      s += "\tin float4 i" + std::to_string(i) + " : TEXCOORD" + std::to_string(i) + ",\n";
    }
    s += "\tin uint iFace : SV_IsFrontFace,\n\tout float pdzDepth : SV_Depth\n)\n{\n"
         "\tfloat pdzZ = iPos.z;\n"
         "\tfloat pdzSlope = max(abs(ddx_coarse(pdzZ)), abs(ddy_coarse(pdzZ)));\n"
         "\tpdzDepth = " + DepthExpression("pdzBiasedDepth(pdzZ, pdzSlope, iFace != 0u)", edge) + ";\n}\n";
    return s;
  }

  namespace detail
  {
    inline std::string Trim(const std::string &s)
    {
      size_t a = 0, b = s.size();
      while (a < b && std::isspace(static_cast<unsigned char>(s[a])))
        ++a;
      while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1])))
        --b;
      return s.substr(a, b - a);
    }
    inline bool IEquals(const std::string &a, const char *b)
    {
      size_t n = 0;
      for (; b[n]; ++n)
      {
        if (n >= a.size() || std::tolower(static_cast<unsigned char>(a[n])) != std::tolower(static_cast<unsigned char>(b[n])))
          return false;
      }
      return n == a.size();
    }
  } // namespace detail

  // The variant of one translated pixel shader (the fork's .hlsl text): the guest `main` is renamed to
  // pdzGuestMain and called from a new `main` with the same parameters (+ SV_Depth when the guest writes none)
  // that adds the depth epilogue. Empty result = the text does not have the expected shape (`why` says what).
  inline std::string BuildVariantSource(const std::string &hlsl, std::string &why, uint32_t edge = 0)
  {
    const std::string kAttr = "[shader(\"pixel\")]";
    const size_t attr = hlsl.rfind(kAttr);
    if (attr == std::string::npos)
    {
      why = "no [shader(\"pixel\")] entry";
      return {};
    }
    // Expect `void main (` after the attribute, whitespace between the tokens.
    size_t open = std::string::npos;
    {
      size_t p = hlsl.find_first_not_of(" \t\r\n", attr + kAttr.size());
      if (p != std::string::npos && hlsl.compare(p, 4, "void") == 0)
      {
        p = hlsl.find_first_not_of(" \t\r\n", p + 4);
        if (p != std::string::npos && hlsl.compare(p, 4, "main") == 0)
        {
          p = hlsl.find_first_not_of(" \t\r\n", p + 4);
          if (p != std::string::npos && hlsl[p] == '(')
          {
            open = p;
          }
        }
      }
    }
    if (open == std::string::npos)
    {
      why = "entry is not `void main(`";
      return {};
    }
    const size_t close = hlsl.find(')', open);
    if (close == std::string::npos)
    {
      why = "unterminated parameter list";
      return {};
    }
    const std::string params = hlsl.substr(open + 1, close - open - 1);
    if (params.find('(') != std::string::npos)
    {
      why = "unexpected parentheses in the parameter list";
      return {};
    }

    // Parameters: "[in|out|inout] <type> <name> : <semantic>".
    std::vector<std::string> names;
    std::string pos_name, face_name, depth_name;
    size_t start = 0;
    while (start <= params.size())
    {
      size_t comma = params.find(',', start);
      if (comma == std::string::npos)
        comma = params.size();
      const std::string p = detail::Trim(params.substr(start, comma - start));
      start = comma + 1;
      if (p.empty())
      {
        if (comma >= params.size())
          break;
        why = "empty parameter";
        return {};
      }
      const size_t colon = p.find(':');
      if (colon == std::string::npos)
      {
        why = "parameter without a semantic: " + p;
        return {};
      }
      const std::string decl = detail::Trim(p.substr(0, colon));
      const std::string semantic = detail::Trim(p.substr(colon + 1));
      const size_t sp = decl.find_last_of(" \t");
      if (sp == std::string::npos)
      {
        why = "parameter without a type: " + p;
        return {};
      }
      const std::string name = decl.substr(sp + 1);
      names.push_back(name);
      if (detail::IEquals(semantic, "SV_Position"))
        pos_name = name;
      else if (detail::IEquals(semantic, "SV_IsFrontFace"))
        face_name = name;
      else if (detail::IEquals(semantic, "SV_Depth"))
        depth_name = name;
      else if (semantic.size() >= 8 && detail::IEquals(semantic.substr(0, 8), "SV_Depth"))
      {
        why = "conservative depth output " + semantic;
        return {};
      }
    }
    if (depth_name.empty() && (pos_name.empty() || face_name.empty()))
    {
      why = "no SV_Position / SV_IsFrontFace input";
      return {};
    }

    std::string out;
    out.reserve(hlsl.size() + 4096);
    out.append(hlsl, 0, attr);
    out += "void pdzGuestMain(";
    out.append(hlsl, open + 1, std::string::npos);
    out += HelperSource();
    if (edge != 0)
    {
      out += EdgeHelperSource();
    }
    out += "\n[shader(\"pixel\")]\nvoid main(";
    out += params;
    if (depth_name.empty())
    {
      out += ",\n\tout float pdzDepth : SV_Depth";
    }
    out += "\n)\n{\n";
    std::string call = "\tpdzGuestMain(";
    for (size_t i = 0; i < names.size(); ++i)
    {
      call += (i ? ", " : "") + names[i];
    }
    call += ");\n";
    if (depth_name.empty())
    {
      out += "\tfloat pdzZ = " + pos_name + ".z;\n";
      out += "\tfloat pdzSlope = max(abs(ddx_coarse(pdzZ)), abs(ddy_coarse(pdzZ)));\n";
      out += call;
      out += "\tpdzDepth = " + DepthExpression("pdzBiasedDepth(pdzZ, pdzSlope, " + face_name + " != 0u)", edge) + ";\n";
    }
    else
    {
      out += call;
      out += "\t" + depth_name + " = " + DepthExpression(depth_name, edge) + ";\n";
    }
    out += "}\n";
    return out;
  }
} // namespace pdz::native::depth20e4
