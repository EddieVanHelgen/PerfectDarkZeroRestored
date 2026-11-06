// Native renderer (stage 1): HLSL text for the 7e3 blend pixel-shader variants (native_blend7e3.h). Standard library
// only, so the transform can be checked offline against the whole shader cache (dxc over every PS).
//
// What the variants compute is the ReXGlue SDK's ROV output merger for a k_2_10_10_10_FLOAT(_AS_16_16_16_16) colour
// target (the reference renders with render_target_path_d3d12 = "rov"):
//   * render_target/cache.h AddPSIColorFormatFlags: 7e3 has a fixed-point ALPHA but a floating-point colour, and
//     cache.cpp GetPSIColorFormatInfo clamps rgb to 0..31.875, alpha to 0..1.
//   * dxbc_translator_om.cpp:2380-2475: the shader colour is multiplied by 2^RB_COLOR_INFO.color_exp_bias (all four
//     components); blending off (RB_BLENDCONTROL = 0x00010001) clamps and packs; blending on clamps the source alpha
//     to 0..1 (fixed-point alpha) and leaves the source colour alone.
//   * dxbc_translator_om.cpp:2575-3105: the colour and alpha factor cases (ROV_HandleColorBlendFactorCases /
//     ROV_HandleAlphaBlendFactorCases 1534-1700), a zero factor drops its operand entirely (NaN-safe), subtract /
//     reverse subtract as signs, min / max applied to the FACTORED operands (Xenos, unlike D3D12), alpha factors and
//     parts clamped to 0..1, the blended colour clamped to 0..31.875 and the alpha to 0..1.
//   * dxbc_translator_om.cpp:3431-3485 PreClampedFloat32To7e3 (round to nearest even, denormals below 0.25), 3501
//     Float7e3To32, 1440-1460 the pack (alpha ftou(a * 3 + 0.5)), 1284-1310 the unpack (alpha n / 3).
//   * keep mask (cache.cpp GetPSIColorFormatInfo + om 3105-3125): channels outside RB_COLOR_MASK keep the stored value.
// The host surface stays R16G16B16A16_FLOAT (every 7e3 value and n / 3 within a float16 rounding of it are exact
// there): the variant reads the stored value through a rasterizer-ordered view, takes what the EDRAM would hold for it
// (clamp + round to 7e3, alpha to n / 3: the non-blended draws keep writing unrounded float16, the readers round),
// blends in float32 like the SDK and writes the rounded result back.
#pragma once

#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

namespace pdz::native::blend7e3
{
  // Helpers of the ROV variant (mode 2). The root signature maps u0 space0 (one-descriptor table) and b4 space0
  // (8 root constants) for the pixel shader (native_pipeline.h kRootUavEdram7e3 / kRootConstBlend7e3).
  inline const char *RovHelperSource()
  {
    return R"HLSL(
// ---- pdz native: 7e3 blend variant (native_blend7e3.cpp): the SDK's ROV output merger for a 7e3 colour target ----
RasterizerOrderedTexture2D<float4> g_PdzEdram7e3 : register(u0, space0);
cbuffer PdzBlend7e3Constants : register(b4, space0)
{
    uint g_PdzBlendControl;    // RB_BLENDCONTROL0 & 0x1FFF1FFF (0x00010001 = blending off)
    uint g_PdzWriteMask;       // RB_COLOR_MASK bits 0..3 (render target 0)
    float g_PdzColorExpScale;  // 2^RB_COLOR_INFO.color_exp_bias
    uint g_PdzBlend7e3Pad;
    float4 g_PdzBlendConstant; // RB_BLEND_RED / GREEN / BLUE / ALPHA
};

// dxbc_translator_om.cpp PreClampedFloat32To7e3: f in [0, 31.875] -> 10-bit 7e3, round to nearest even.
uint pdzFloat32To7e3(float f)
{
    uint u = asuint(f);
    uint biased = (u < 0x3E800000u) ? (((u & 0x7FFFFFu) | 0x800000u) >> min(125u - (u >> 23u), 24u))
                                    : (u + 0xC2000000u);
    biased += 0x7FFFu + ((biased >> 16u) & 1u);
    return (biased >> 16u) & 0x3FFu;
}

// dxbc_translator_om.cpp Float7e3To32 (exact).
float pdzFloat7e3To32(uint f10)
{
    uint e = (f10 >> 7u) & 7u;
    uint m = f10 & 0x7Fu;
    return (e != 0u) ? asfloat(((e + 124u) << 23u) | (m << 16u)) : float(m) * (1.0 / 512.0);
}

// What the EDRAM holds after a write of c: clamped to the 7e3 range (NaN -> 0), rounded to 7e3.
float3 pdzStored7e3(float3 c)
{
    c = clamp(c, 0.0, 31.875);
    return float3(pdzFloat7e3To32(pdzFloat32To7e3(c.r)), pdzFloat7e3To32(pdzFloat32To7e3(c.g)),
                  pdzFloat7e3To32(pdzFloat32To7e3(c.b)));
}

// The 2-bit alpha: pack ftou(a * 3 + 0.5) of the clamped alpha, unpack n * (1 / 3).
float pdzStored2(float a)
{
    return float(uint(saturate(a) * 3.0 + 0.5)) * (1.0 / 3.0);
}

// ROV_HandleColorBlendFactorCases (xenos::BlendFactor; 0 and anything unknown = zero).
float3 pdzColorFactor(uint f, float4 s, float4 d)
{
    switch (f)
    {
    case 1u: return float3(1.0, 1.0, 1.0);
    case 4u: return s.rgb;
    case 5u: return 1.0 - s.rgb;
    case 6u: return s.aaa;
    case 7u: return 1.0 - s.aaa;
    case 8u: return d.rgb;
    case 9u: return 1.0 - d.rgb;
    case 10u: return d.aaa;
    case 11u: return 1.0 - d.aaa;
    case 12u: return g_PdzBlendConstant.rgb;
    case 13u: return 1.0 - g_PdzBlendConstant.rgb;
    case 14u: return g_PdzBlendConstant.aaa;
    case 15u: return 1.0 - g_PdzBlendConstant.aaa;
    case 16u: return min(s.a, 1.0 - d.a).xxx;
    default: return float3(0.0, 0.0, 0.0);
    }
}

// ROV_HandleAlphaBlendFactorCases.
float pdzAlphaFactor(uint f, float4 s, float4 d)
{
    switch (f)
    {
    case 1u: case 16u: return 1.0;
    case 4u: case 6u: return s.a;
    case 5u: case 7u: return 1.0 - s.a;
    case 8u: case 10u: return d.a;
    case 9u: case 11u: return 1.0 - d.a;
    case 12u: case 14u: return g_PdzBlendConstant.a;
    case 13u: case 15u: return 1.0 - g_PdzBlendConstant.a;
    default: return 0.0;
    }
}

// s = the guest shader's colour output 0 (after its alpha test), d = the stored value as the EDRAM holds it.
float4 pdzBlend7e3(float4 s, float4 d)
{
    s *= g_PdzColorExpScale;
    uint bc = g_PdzBlendControl;
    if (bc == 0x00010001u)
    {
        return float4(clamp(s.rgb, 0.0, 31.875), saturate(s.a));
    }
    s.a = saturate(s.a); // fixed-point alpha: the blending source alpha is clamped (7e3 colour is not)
    uint cs = bc & 0x1Fu, cd = (bc >> 8u) & 0x1Fu;
    uint as = (bc >> 16u) & 0x1Fu, ad = (bc >> 24u) & 0x1Fu;
    float4 r;
    // Colour: zero factors drop the operand; min / max take the factored operands (Xenos).
    float3 sp = (cs != 0u) ? s.rgb * pdzColorFactor(cs, s, d) : float3(0.0, 0.0, 0.0);
    float3 dp = (cd != 0u) ? d.rgb * pdzColorFactor(cd, s, d) : float3(0.0, 0.0, 0.0);
    if ((bc & 0x40u) != 0u)
    {
        r.rgb = ((bc & 0x20u) != 0u) ? max(sp, dp) : min(sp, dp);
    }
    else
    {
        sp = ((bc & 0x80u) != 0u) ? -sp : sp;                    // reverse subtract: -src
        r.rgb = (cd != 0u) ? (dp * (((bc & 0x20u) != 0u) ? -1.0 : 1.0) + sp) : sp; // subtract: -dst
    }
    r.rgb = clamp(r.rgb, 0.0, 31.875);
    // Alpha: fixed point, factors and parts clamped to 0..1.
    float sa = (as != 0u) ? saturate(s.a * saturate(pdzAlphaFactor(as, s, d))) : 0.0;
    float da = (ad != 0u) ? saturate(d.a * saturate(pdzAlphaFactor(ad, s, d))) : 0.0;
    if ((bc & 0x400000u) != 0u)
    {
        r.a = ((bc & 0x200000u) != 0u) ? max(sa, da) : min(sa, da);
    }
    else
    {
        sa = ((bc & 0x800000u) != 0u) ? -sa : sa;
        r.a = saturate((ad != 0u) ? (da * (((bc & 0x200000u) != 0u) ? -1.0 : 1.0) + sa) : sa);
    }
    return r;
}

// The output merger for this pixel: read what is stored, blend, round to 7e3 / 2-bit alpha, keep masked channels.
void pdzRovBlend7e3(float4 pos, float4 color)
{
    uint2 xy = uint2(pos.xy);
    float4 old = g_PdzEdram7e3[xy];
    float4 d = float4(pdzStored7e3(old.rgb), pdzStored2(old.a));
    float4 r = pdzBlend7e3(color, d);
    float4 packed = float4(pdzStored7e3(r.rgb), pdzStored2(r.a));
    uint wm = g_PdzWriteMask;
    g_PdzEdram7e3[xy] = float4(((wm & 1u) != 0u) ? packed.r : old.r, ((wm & 2u) != 0u) ? packed.g : old.g,
                               ((wm & 4u) != 0u) ? packed.b : old.b, ((wm & 8u) != 0u) ? packed.a : old.a);
}
)HLSL";
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
    inline bool StartsWithWord(const std::string &s, const char *word)
    {
      size_t n = 0;
      while (word[n])
        ++n;
      return s.size() > n && s.compare(0, n, word) == 0 && std::isspace(static_cast<unsigned char>(s[n]));
    }
    struct Param
    {
      std::string text;     // as written, trimmed ("in float4 i0 : TEXCOORD0")
      std::string name;     // "i0"
      std::string semantic; // "TEXCOORD0"
      bool out = false;     // out / inout
      bool float4_type = false;
      bool extra = false;   // a colour output other than SV_Target0
    };
  } // namespace detail

  // The variant of one translated pixel shader (the fork's .hlsl text): the guest `main` is renamed to pdzGuestMain and
  // called from a new `main`.
  //   mode 1: same parameters; after the call the colour output's alpha is clamped to 0..1 (the 7e3 fixed-point
  //           alpha of the blending source; the fixed-function float16 blend does the rest).
  //   mode 2: [earlydepthstencil], no colour output; the colour goes through pdzRovBlend7e3 (RovHelperSource).
  // The shader must output `out float4 <name> : SV_Target0` (SV_Target), may output SV_Target1..7 (mode 1 keeps them,
  // mode 2 drops them: it is used with render target 0 alone), nothing else (no depth), and mode 2 needs an SV_Position
  // input. Empty result = the text does not have the expected shape (`why` says what).
  inline std::string BuildVariantSource(const std::string &hlsl, uint32_t mode, std::string &why)
  {
    if (mode != 1 && mode != 2)
    {
      why = "unknown mode";
      return {};
    }
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

    std::vector<detail::Param> list;
    size_t start = 0;
    while (start <= params.size())
    {
      size_t comma = params.find(',', start);
      if (comma == std::string::npos)
        comma = params.size();
      detail::Param p;
      p.text = detail::Trim(params.substr(start, comma - start));
      start = comma + 1;
      if (p.text.empty())
      {
        if (comma >= params.size())
          break;
        why = "empty parameter";
        return {};
      }
      const size_t colon = p.text.find(':');
      if (colon == std::string::npos)
      {
        why = "parameter without a semantic: " + p.text;
        return {};
      }
      const std::string decl = detail::Trim(p.text.substr(0, colon));
      p.semantic = detail::Trim(p.text.substr(colon + 1));
      const size_t sp = decl.find_last_of(" \t");
      if (sp == std::string::npos)
      {
        why = "parameter without a type: " + p.text;
        return {};
      }
      p.name = decl.substr(sp + 1);
      p.out = detail::StartsWithWord(decl, "out") || detail::StartsWithWord(decl, "inout");
      p.float4_type = decl.find("float4") != std::string::npos;
      list.push_back(p);
    }

    int color = -1;
    std::string pos_name;
    for (size_t i = 0; i < list.size(); ++i)
    {
      detail::Param &p = list[i];
      if (p.out)
      {
        if ((detail::IEquals(p.semantic, "SV_Target0") || detail::IEquals(p.semantic, "SV_Target")) && p.float4_type &&
            color < 0)
        {
          color = int(i);
          continue;
        }
        // Other colour outputs (SV_Target1..7): mode 1 keeps them, mode 2 (render target 0 only) drops them.
        if (p.semantic.size() == 10 && detail::IEquals(p.semantic.substr(0, 9), "SV_Target") && p.semantic[9] >= '1' &&
            p.semantic[9] <= '7')
        {
          p.extra = true;
          continue;
        }
        why = "output other than float4 SV_Target0..7: " + p.text;
        return {};
      }
      if (detail::IEquals(p.semantic, "SV_Position"))
      {
        pos_name = p.name;
      }
    }
    if (color < 0)
    {
      why = "no SV_Target0 output";
      return {};
    }
    if (mode == 2 && pos_name.empty())
    {
      why = "no SV_Position input";
      return {};
    }

    std::string out;
    out.reserve(hlsl.size() + 8192);
    out.append(hlsl, 0, attr);
    out += "void pdzGuestMain(";
    out.append(hlsl, open + 1, std::string::npos);
    const std::string &color_name = list[size_t(color)].name;
    if (mode == 1)
    {
      out += "\n// ---- pdz native: 7e3 blend variant 1 (native_blend7e3.cpp): the blending source alpha clamped to 0..1 ----\n";
      out += "[shader(\"pixel\")]\nvoid main(" + params + ")\n{\n\tpdzGuestMain(";
      for (size_t i = 0; i < list.size(); ++i)
      {
        out += (i ? ", " : "") + list[i].name;
      }
      out += ");\n\t" + color_name + ".a = saturate(" + color_name + ".a);\n}\n";
      return out;
    }
    out += RovHelperSource();
    out += "\n[earlydepthstencil]\n[shader(\"pixel\")]\nvoid main(";
    bool first = true;
    for (size_t i = 0; i < list.size(); ++i)
    {
      if (int(i) == color || list[i].extra)
      {
        continue;
      }
      out += (first ? "\n\t" : ",\n\t") + list[i].text;
      first = false;
    }
    out += "\n)\n{\n\tfloat4 pdzColor = float4(0.0, 0.0, 0.0, 0.0);\n";
    for (size_t i = 0; i < list.size(); ++i)
    {
      if (list[i].extra)
      {
        out += "\tfloat4 pdzUnused" + std::to_string(i) + " = float4(0.0, 0.0, 0.0, 0.0); // " + list[i].semantic +
               ": no render target bound\n";
      }
    }
    out += "\tpdzGuestMain(";
    for (size_t i = 0; i < list.size(); ++i)
    {
      out += (i ? ", " : "") + (int(i) == color ? std::string("pdzColor")
                                                : (list[i].extra ? "pdzUnused" + std::to_string(i) : list[i].name));
    }
    out += ");\n\tpdzRovBlend7e3(" + pos_name + ", pdzColor);\n}\n";
    return out;
  }
} // namespace pdz::native::blend7e3
