// Native renderer (stage 1): two places where the XenosRecomp fork's HLSL computes something else than Xenos and the
// SDK's translator, fixed by a text patch of the fork's .hlsl at a pixel shader's first draw, together with the co-issue
// fix, PsParamGen and the cube fix (native_coissue.cpp: one recompile, cached in <shader dir>\coissue). Standard
// library only: the offline check (stage1/subagents/nightclub3.md) compiles it on its own against the shader cache.
//
// 1. rcpf / rsqf (pdz_native_rcpf_zero). The fork translates rcpc, rcpf and rcp alike (and rsqc, rsqf, rsq) as
//    "ps = clamp(rcp(x), FLT_MIN, FLT_MAX);" (FLT_MIN is -FLT_MAX in shader_common_pdz.hlsli): an infinite result
//    becomes +-FLT_MAX. The SDK (dxbc_translator_alu.cpp kRcpf / kRsqf) keeps the sign and returns +-0 for the "f"
//    (fixed-function, D3D9) variants, +-FLT_MAX only for the "c" (clamped) ones, the plain ones unclamped. PDZ uses rcpf
//    for depth linearisation, e.g. the harbour refraction 1 / (G-buffer depth + c32.x) of PS a2917857: where nothing was
//    drawn (depth 0) the emulator gets 0 (no water fog) and the fork FLT_MAX (fog saturated), stage1/subagents/water2.md.
//    The patch rewrites the n-th "ps = " line of every rcpf / rsqf instruction (the same ordinal mapping as the co-issue
//    patch, from the microcode scan) to pdzRcpF / pdzRsqF: 854 cached shaders (830 PS, 24 VS) have such instructions,
//    the rewritten lines are exactly those of the fork-oracle translation with patch 11 (cache\hlsl_oracle_clamp), for
//    all 854, operand for operand.
//
// 2. Stacked textures under tfetch3D (pdz_native_stacked_array). The SDK gives every tfetch3D two bindings, a Texture3D
//    and a Texture2DArray, and picks by the fetch constant's dimension at run time: a 2D or stacked texture is sampled
//    as an array (z * layers clamped to the layer centres, the two layers around it lerped for the linear volume filter
//    every PDZ stack has, the LOD from xy only, every layer at every mip, the sampler's anisotropy). The native volume
//    view (Textures::LookupVolume) is a Texture3D copy: its LOD includes the layer axis, its mips halve the layer count
//    (a minified stack shows other animation frames), 3D filtering gives no anisotropic footprint on current GPUs (the
//    water normals seen at grazing angles blur), and resolve mirrors get none at all. The patch turns every
//    tfetch3D(g_Texture3D[n], ...) into pdzTfetch3DS(g_Texture3D[n], g_Texture2DArray[n], ...): space3 t<n> holds the
//    array view of a 2D / stacked / mirror texture (BindSrvTable) or stays null for a true 3D texture, and the helper
//    takes the Texture3D path when GetDimensions reads 0 layers. The Nightclub harbour normals (DXN 128x128x16 / x32,
//    seen at grazing angles), the cookie / caustics projector of PS 858ecd68 (DXT1 256x256x32; its coordinates come from
//    the cube instruction, native_cubefix.h), the Datacore flare stacks and the Mansion ice are such textures
//    (stage1/subagents/stacked.md 5b, done in the renderer instead of the translator).
//
// 3. Literal registers (pdz_native_live_literals). The fork bakes the literal records of the one container the cache
//    was built from; PDZ ships the same microcode with other literals (per-material parallax scales). The patch reads
//    them from the uploaded constants, which D3D fills from the bound object (PatchLiveLiterals,
//    stage1/subagents/temple2.md).
//
// 4. Far-cascade PCF (pdz_shadow_far_pcf, an enhancement, off by default). The sun shadow mask shader 34d98195
//    point-samples its maps; the patch replaces the far cascade's single comparison by nine bilinear ones
//    (PatchShadowFarPcf, PDZRenderResearch native/stage2/polish/shadowbias.md).
//
// 5. Near / far split blend (pdz_shadow_split_blend, an enhancement with pdz_shadow_distance_scale above 1). The same
//    shader switches cascades at one distance; the patch blends the far cascade in over the near cascade's last part
//    (PatchShadowSplitBlend, PDZRenderResearch native/stage2/polish/shadows2_cascade.md).
#pragma once

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

#include "native_coissue_scan.h"

namespace pdz::native::hlslfix
{
  namespace detail
  {
    struct Lines
    {
      std::vector<std::string> lines;
      bool crlf = false;
    };

    inline Lines Split(const std::string &text)
    {
      Lines l;
      l.crlf = text.find("\r\n") != std::string::npos;
      for (size_t b = 0;;)
      {
        const size_t e = text.find('\n', b);
        if (e == std::string::npos)
        {
          l.lines.push_back(text.substr(b));
          break;
        }
        size_t end = e;
        if (l.crlf && end > b && text[end - 1] == '\r')
        {
          --end;
        }
        l.lines.push_back(text.substr(b, end - b));
        b = e + 1;
      }
      return l;
    }

    inline std::string Join(const Lines &l)
    {
      std::string out;
      size_t size = 0;
      for (const std::string &s : l.lines)
      {
        size += s.size() + 2;
      }
      out.reserve(size);
      for (size_t i = 0; i < l.lines.size(); ++i)
      {
        out += l.lines[i];
        if (i + 1 < l.lines.size())
        {
          out += l.crlf ? "\r\n" : "\n";
        }
      }
      return out;
    }

    inline bool EndsWith(const std::string &s, const std::string &suffix)
    {
      return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
    }

    // Index of the "[shader(...)]" line, or lines.size().
    inline size_t EntryLine(const Lines &l)
    {
      for (size_t i = 0; i < l.lines.size(); ++i)
      {
        if (coissue::scan_detail::StartsWith(coissue::scan_detail::Trim(l.lines[i]), "[shader("))
        {
          return i;
        }
      }
      return l.lines.size();
    }
  } // namespace detail

  // The helpers the rcpf / rsqf lines call: +-inf -> +-0, the sign kept (the SDK's and-mask with the sign bit). Integer
  // compares, so no fast-math assumption about infinities can fold them away.
  constexpr const char *kRcpfHelpers[] = {
      "float pdzRcpF(float x) { float r = rcp(x); uint u = asuint(r); return (u & 0x7FFFFFFFu) == 0x7F800000u ? "
      "asfloat(u & 0x80000000u) : r; } // native_hlslfix.h (SDK kRcpf)",
      "float pdzRsqF(float x) { float r = rsqrt(x); uint u = asuint(r); return (u & 0x7FFFFFFFu) == 0x7F800000u ? "
      "asfloat(u & 0x80000000u) : r; } // native_hlslfix.h (SDK kRsqf)",
  };

  // Rewrites the "ps = " line of every rcpf / rsqf instruction of `scan` (pdz_native_rcpf_zero). `sites` = lines
  // rewritten. False with `why` empty: nothing to do (no such instruction, or every line already calls the helpers);
  // false with `why` set: the text is not shaped like the fork's (left as it was).
  inline bool PatchRcpF(const std::string &hlsl, const coissue::ScanResult &scan, std::string &out, std::string &why,
                        uint32_t &sites)
  {
    using namespace coissue::scan_detail;
    why.clear();
    sites = 0;
    if (!scan.ok)
    {
      why = "microcode not parsed";
      return false;
    }
    if (scan.rcpf_rsqf.empty())
    {
      return false;
    }
    detail::Lines l = detail::Split(hlsl);
    const size_t start = detail::EntryLine(l);
    if (start == l.lines.size())
    {
      why = "no [shader(...)] entry point";
      return false;
    }
    std::vector<size_t> ps_lines;
    for (size_t i = start; i < l.lines.size(); ++i)
    {
      if (StartsWith(Trim(l.lines[i]), "ps = "))
      {
        ps_lines.push_back(i);
      }
    }
    if (ps_lines.size() != scan.scalar_ops)
    {
      why = std::to_string(ps_lines.size()) + " 'ps = ' lines for " + std::to_string(scan.scalar_ops) +
            " scalar operations";
      return false;
    }
    for (const coissue::ScalarSite &site : scan.rcpf_rsqf)
    {
      if (site.ordinal >= ps_lines.size())
      {
        why = At(site.address) + "scalar ordinal out of range";
        return false;
      }
      std::string &line = l.lines[ps_lines[site.ordinal]];
      const std::string trimmed = Trim(line);
      const bool rcp = site.opcode == kScalarRcpf;
      const std::string helper = rcp ? "pdzRcpF(" : "pdzRsqF(";
      if (StartsWith(trimmed, "ps = " + helper))
      {
        continue; // a translation that has it already
      }
      const std::string prefix = rcp ? "ps = clamp(rcp(" : "ps = clamp(rsqrt(";
      const std::string suffix = "), FLT_MIN, FLT_MAX);";
      if (!StartsWith(trimmed, prefix) || !detail::EndsWith(trimmed, suffix) ||
          trimmed.size() <= prefix.size() + suffix.size())
      {
        why = At(site.address) + (rcp ? "rcpf" : "rsqf") + " line is '" + trimmed.substr(0, 80) + "'";
        return false;
      }
      const std::string operand = trimmed.substr(prefix.size(), trimmed.size() - prefix.size() - suffix.size());
      const size_t indent_end = line.find_first_not_of(" \t");
      line = line.substr(0, indent_end == std::string::npos ? 0 : indent_end) + "ps = " + helper + operand + ");";
      ++sites;
    }
    if (sites == 0)
    {
      return false;
    }
    std::vector<std::string> insert;
    for (const char *h : kRcpfHelpers)
    {
      const std::string text(h);
      if (hlsl.find(text.substr(0, text.find('{'))) == std::string::npos) // "float pdzRcpF(float x) "
      {
        insert.push_back(text);
      }
    }
    insert.push_back("");
    l.lines.insert(l.lines.begin() + std::ptrdiff_t(start), insert.begin(), insert.end());
    out = detail::Join(l);
    return true;
  }

  constexpr const char *kArrayMarker = "// native_hlslfix.h: stacked textures under tfetch3D";

  // Rewrites every "tfetch3D(g_Texture3D[n], " call of the entry point into "pdzTfetch3DS(g_Texture3D[n],
  // g_Texture2DArray[n], " and declares the space3 array and the helper (pdz_native_stacked_array). `slots` = bit n for
  // every rewritten slot. False with `why` empty: no such call, or already patched; with `why` set: another use of
  // g_Texture3D (a Bias / Level fetch, a size query) the patch does not cover, or a text not shaped like the fork's.
  inline bool PatchStackedArray(const std::string &hlsl, std::string &out, std::string &why, uint32_t &slots)
  {
    why.clear();
    slots = 0;
    if (hlsl.find(kArrayMarker) != std::string::npos)
    {
      return false;
    }
    const size_t main_at = hlsl.find("[shader(");
    if (main_at == std::string::npos)
    {
      why = "no [shader(...)] entry point";
      return false;
    }
    const std::string decl = "Texture3D<float4> g_Texture3D[32] : register(t0, space1);";
    const std::string helper = "float4 tfetch3D(Texture3D<float4> t, SamplerState s, float3 uvw, float3 offset)";
    if (hlsl.find(decl) == std::string::npos || hlsl.find(helper) == std::string::npos)
    {
      why = "no fork g_Texture3D declaration / tfetch3D helper";
      return false;
    }
    if (hlsl.find("g_Texture2DArray") != std::string::npos)
    {
      why = "g_Texture2DArray already declared";
      return false;
    }
    const std::string call = "tfetch3D(g_Texture3D[";
    uint32_t calls = 0, uses = 0;
    for (size_t p = hlsl.find("g_Texture3D[", main_at); p != std::string::npos; p = hlsl.find("g_Texture3D[", p + 1))
    {
      ++uses;
    }
    std::string body;
    body.reserve(hlsl.size() - main_at + 4096);
    size_t copied = main_at;
    for (size_t p = hlsl.find(call, main_at); p != std::string::npos; p = hlsl.find(call, p + call.size()))
    {
      if (p > 0 && (hlsl[p - 1] == '_' || std::isalnum(static_cast<unsigned char>(hlsl[p - 1]))))
      {
        continue; // another helper's name ending in tfetch3D (none in the fork)
      }
      const size_t index_begin = p + call.size();
      const size_t index_end = hlsl.find(']', index_begin);
      if (index_end == std::string::npos || index_end == index_begin || index_end - index_begin > 2 ||
          hlsl.compare(index_end, 3, "], ") != 0)
      {
        why = "a tfetch3D call not shaped like the fork's";
        return false;
      }
      uint32_t slot = 0;
      for (size_t i = index_begin; i < index_end; ++i)
      {
        if (!std::isdigit(static_cast<unsigned char>(hlsl[i])))
        {
          why = "a tfetch3D call with a computed slot";
          return false;
        }
        slot = slot * 10 + uint32_t(hlsl[i] - '0');
      }
      if (slot >= 32)
      {
        why = "slot out of range";
        return false;
      }
      const std::string index = hlsl.substr(index_begin, index_end - index_begin);
      body.append(hlsl, copied, p - copied);
      body += "pdzTfetch3DS(g_Texture3D[" + index + "], g_Texture2DArray[" + index + "], ";
      copied = index_end + 3;
      slots |= 1u << slot;
      ++calls;
    }
    if (calls == 0)
    {
      return false;
    }
    if (calls != uses)
    {
      why = std::to_string(uses - calls) + " other use(s) of g_Texture3D (Bias / Level fetch or size query)";
      slots = 0;
      return false;
    }
    body.append(hlsl, copied, std::string::npos);
    const char *nl = hlsl.find("\r\n") != std::string::npos ? "\r\n" : "\n";
    // Inserted right before the entry point's "[shader(" line, after every fork helper (tfetch3D included).
    const size_t line_start = hlsl.rfind('\n', main_at) == std::string::npos ? 0 : hlsl.rfind('\n', main_at) + 1;
    std::string helpers;
    for (const char *line : {
             kArrayMarker,
             "// (pdz_native_stacked_array): space3 t<n> = the Texture2DArray view of a 2D / stacked texture (the SDK's",
             "// k2D binding of a tfetch3D), null for a true 3D texture (GetDimensions 0: the Texture3D path).",
             "Texture2DArray<float4> g_Texture2DArray[32] : register(t0, space3);",
             "",
             "float4 pdzTfetch3DS(Texture3D<float4> t, Texture2DArray<float4> ta, SamplerState s, float3 uvw, float3 offset)",
             "{",
             "    uint w, h, layers;",
             "    ta.GetDimensions(w, h, layers);",
             "    [branch] if (layers == 0u)",
             "        return tfetch3D(t, s, uvw, offset);",
             "    // SDK k3DOrStacked with the linear volume filter: z * layers clamped to the layer centres, the two layers",
             "    // around it lerped, the LOD from xy only (Texture2DArray.Sample), the sampler's anisotropy.",
             "    float2 uv = uvw.xy + offset.xy / float2(w, h);",
             "    float z = clamp(uvw.z * float(layers) + offset.z, 0.5, float(layers) - 0.5) - 0.5;",
             "    float l = floor(z);",
             "    float f = z - l;",
             "    float4 a = ta.Sample(s, float3(uv, l));",
             "    float4 b = ta.Sample(s, float3(uv, min(l + 1.0, float(layers) - 1.0)));",
             "    return f > 0.0 ? lerp(a, b, f) : a;",
             "}",
             "",
         })
    {
      helpers += line;
      helpers += nl;
    }
    out.clear();
    out.reserve(hlsl.size() + helpers.size() + 64 * calls);
    out.append(hlsl, 0, line_start);
    out += helpers;
    out.append(hlsl, line_start, main_at - line_start);
    out += body;
    return true;
  }

  constexpr const char *kLiveLiteralMarker = "// native_hlslfix.h: live literal register";

  // Literal registers read from the constant buffer (pdz_native_live_literals). The fork bakes a container's literal
  // records into "static const float4 cl<n> = asfloat(uint4(...));" lines (PS-relative n), but the shader cache is keyed
  // by the microcode alone, and PDZ ships one microcode in many containers whose literals differ (61 pixel shaders, no
  // vertex shader, in the retail + DLC packages): the material compiler puts per-material values there, e.g. the
  // parallax scale c1.w of the G-buffer / material shaders (a9204a6c: 13 values 0.01-0.1 in Temple's package, the HLSL
  // bakes 0.01; 35e06528: Mansion draws 0.02 / 0.025 / 0.05 / 0.1 in one frame, the HLSL bakes 0.1). D3D copies the
  // bound object's literals into the device's constant shadow at SetPixelShader (PIPELINE_NOTES.md "literals"), the
  // emulator reads the register file, and the native uploads that shadow already; this rewrites each declaration with
  // n < `limit` (the uploaded PS registers) into "#define cl<n> c[<n>]", so the shader reads what the GPU would.
  // `registers` = declarations rewritten. False with `why` empty: no such declaration (or already patched); with `why`
  // set: a declaration not shaped like the fork's (left as it was).
  inline bool PatchLiveLiterals(const std::string &hlsl, uint32_t limit, std::string &out, std::string &why,
                                uint32_t &registers)
  {
    using namespace coissue::scan_detail;
    why.clear();
    registers = 0;
    if (hlsl.find(kLiveLiteralMarker) != std::string::npos)
    {
      return false;
    }
    detail::Lines l = detail::Split(hlsl);
    const size_t start = detail::EntryLine(l);
    if (start == l.lines.size())
    {
      why = "no [shader(...)] entry point";
      return false;
    }
    const std::string key = "static const float4 cl";
    const std::string tail = " = asfloat(uint4(";
    for (size_t i = 0; i < start; ++i)
    {
      std::string &line = l.lines[i];
      if (!StartsWith(line, key))
      {
        continue;
      }
      size_t p = key.size();
      uint32_t n = 0;
      while (p < line.size() && std::isdigit(static_cast<unsigned char>(line[p])) && n < 100000)
      {
        n = n * 10 + uint32_t(line[p] - '0');
        ++p;
      }
      if (p == key.size() || line.compare(p, tail.size(), tail) != 0 || !detail::EndsWith(line, "));"))
      {
        why = "literal line '" + line.substr(0, 80) + "'";
        return false;
      }
      if (n >= limit)
      {
        continue; // not uploaded (PS c224..c255): the container's value stays
      }
      const std::string name = line.substr(key.size() - 2, p - (key.size() - 2)); // "cl<n>"
      const std::string value = line.substr(p + 3, line.size() - (p + 3) - 1);  // "asfloat(uint4(...))"
      line = "#define " + name + " c[" + std::to_string(n) + "] " + kLiveLiteralMarker + " (the cached container's " +
             value + ")";
      ++registers;
    }
    if (registers == 0)
    {
      return false;
    }
    out = detail::Join(l);
    return true;
  }

  // The sun shadow mask shader (bound by TU3 sub_82607470, src/mod/render/sun_shadows.cpp): per pixel the scene
  // position in the cascade's light space, one point sample of the cascade's map (far: t2 with g_Samplers[3], whose
  // fetch instruction overrides the filter to point), the bias z' = z + NdotL * cl13.x + cl13.y, then
  // r3.x = (cl10.z >= z' - map) (lit) and r3.z = saturate(1 - cl10.w (z' - map)) (soft); r3.xz go out faded.
  constexpr uint64_t kShadowMaskShader = 0x34d981950e6baf85ull;
  constexpr const char *kShadowPcfMarker = "// native_hlslfix.h: far-cascade PCF";

  // Far-cascade PCF (pdz_shadow_far_pcf). Records the far cascade's map coordinate where the fork fetches it, and right
  // after the comparison (the line "r0.z = r4.y * cl17.x;", where r2.z is the biased receiver depth and p0 the
  // comparison's predicate) replaces r3.x / r3.z with the tent-weighted mean of nine bilinear comparisons (GatherRed:
  // 2 x 2 texels, each compared as the original compares one, then lerped by the sub-texel position) at offsets of
  // -1 / 0 / +1 times c[13].z texels: the radius sun_shadows.cpp writes into the literal's unused z (0 = the original
  // single sample; the block is skipped). Only when the far cascade was sampled: the near cascade keeps one sample.
  // False with `why` set: the text is not shaped like the fork's 34d98195 (left as it was); with `why` empty: already
  // patched.
  inline bool PatchShadowFarPcf(const std::string &hlsl, std::string &out, std::string &why)
  {
    using namespace coissue::scan_detail;
    why.clear();
    if (hlsl.find(kShadowPcfMarker) != std::string::npos)
    {
      return false;
    }
    detail::Lines l = detail::Split(hlsl);
    const size_t start = detail::EntryLine(l);
    if (start == l.lines.size())
    {
      why = "no [shader(...)] entry point";
      return false;
    }
    // Each anchor exactly once in main(), and the comparison as described above.
    const auto find_one = [&](const std::string &text, size_t &at) {
      size_t n = 0;
      for (size_t i = start; i < l.lines.size(); ++i)
      {
        if (Trim(l.lines[i]) == text)
        {
          at = i;
          ++n;
        }
      }
      if (n != 1)
      {
        why = "'" + text + "' found " + std::to_string(n) + " times";
        return false;
      }
      return true;
    };
    size_t decl = 0, fetch = 0, apply = 0, unused = 0;
    if (!find_one("CubeMapData cubeMapData = (CubeMapData)0;", decl) ||
        !find_one("r3.w = tfetch2D(g_Texture2D[2], g_Samplers[3], r2.xy, float2(0, 0)).x;", fetch) ||
        !find_one("r2.w = r0.x * cl13.x + cl13.y;", unused) || !find_one("r3.x = cl10.z >= r3.z;", unused) ||
        !find_one("r3.z = saturate(-r3.z + cl10.y);", unused) || !find_one("r0.z = r4.y * cl17.x;", apply) ||
        !(decl < fetch && fetch < apply))
    {
      if (why.empty())
      {
        why = "anchors out of order";
      }
      return false;
    }
    const auto indent = [&](size_t i) {
      const std::string &line = l.lines[i];
      return line.substr(0, line.find_first_not_of(" \t"));
    };
    const std::string fi = indent(fetch), ai = indent(apply);
    const std::vector<std::string> block = {
        ai + kShadowPcfMarker + " (pdz_shadow_far_pcf): c[13].z = radius in texels, 0 = the original sample",
        ai + "[branch] if (pdzFar && p0 && c[13].z > 0.0)",
        ai + "{",
        ai + "\tfloat2 pdzSize = texSize2D(g_Texture2D[2]);",
        ai + "\tfloat pdzHard = 0.0;",
        ai + "\tfloat pdzSoft = 0.0;",
        ai + "\t[unroll] for (int pdzY = -1; pdzY <= 1; ++pdzY)",
        ai + "\t{",
        ai + "\t\t[unroll] for (int pdzX = -1; pdzX <= 1; ++pdzX)",
        ai + "\t\t{",
        ai + "\t\t\tfloat2 pdzTc = pdzFarUv * pdzSize + float2(pdzX, pdzY) * c[13].z - 0.5;",
        ai + "\t\t\tfloat2 pdzBase = floor(pdzTc);",
        ai + "\t\t\tfloat2 pdzF = pdzTc - pdzBase;",
        ai + "\t\t\tfloat4 pdzD = r2.z - g_Texture2D[2].GatherRed(g_Samplers[3], (pdzBase + 1.0) / pdzSize);",
        ai + "\t\t\tfloat4 pdzH = float4(cl10.zzzz >= pdzD);",
        ai + "\t\t\tfloat4 pdzS = saturate(-(cl10.wwww * pdzD) + cl10.yyyy);",
        ai + "\t\t\tfloat pdzW = (pdzX == 0 ? 2.0 : 1.0) * (pdzY == 0 ? 2.0 : 1.0);",
        ai + "\t\t\tpdzHard += pdzW * lerp(lerp(pdzH.w, pdzH.z, pdzF.x), lerp(pdzH.x, pdzH.y, pdzF.x), pdzF.y);",
        ai + "\t\t\tpdzSoft += pdzW * lerp(lerp(pdzS.w, pdzS.z, pdzF.x), lerp(pdzS.x, pdzS.y, pdzF.x), pdzF.y);",
        ai + "\t\t}",
        ai + "\t}",
        ai + "\tr3.x = pdzHard * 0.0625;",
        ai + "\tr3.z = pdzSoft * 0.0625;",
        ai + "}",
    };
    // Bottom up, so the earlier indices stay valid.
    l.lines.insert(l.lines.begin() + std::ptrdiff_t(apply), block.begin(), block.end());
    l.lines.insert(l.lines.begin() + std::ptrdiff_t(fetch + 1),
                   {fi + "pdzFar = true; " + kShadowPcfMarker, fi + "pdzFarUv = r2.xy;"});
    l.lines.insert(l.lines.begin() + std::ptrdiff_t(decl + 1),
                   {indent(decl) + "bool pdzFar = false; " + kShadowPcfMarker, indent(decl) + "float2 pdzFarUv = 0.0;"});
    out = detail::Join(l);
    return true;
  }
  constexpr const char *kShadowBlendMarker = "// native_hlslfix.h: split blend";

  // Near / far split blend (pdz_shadow_split_blend, polish 6c). The mask shader switches from the near to the far
  // cascade where the view depth -r1.z passes the split c[8].w (a visible ring when the far cascade is much coarser).
  // This records that the near cascade was sampled (after its map fetch, t1 with g_Samplers[5]) and, right before the
  // line "r0.z = r4.y * cl17.x;" (as PatchShadowFarPcf: r2.z / r2.w = the near receiver depth and its bias, r0.x = the
  // fade x NdotL x (1 - r4.x) term, r0.w = the map-A term, r3.x / r3.z = hard / soft, p0 = the comparison's
  // predicate), for a near pixel within the last c[13].w x c[8].w of the near cascade (c[13].w: the literal's unused w,
  // written by sun_shadows.cpp; 0 = the original switch, the block is skipped) evaluates the far cascade the way the
  // original does (matrix c4-c6, map A t4 / g_Samplers[2], map B t2 / g_Samplers[3], the same bias r2.w and
  // compares) and lerps r3.x, r3.z and r0.w toward it, from 0 at the band's start to 1 at the split, where the far
  // cascade takes over: no step at either end. When the text already has PatchShadowFarPcf's block, the far map B
  // compare in the band uses the same nine taps (radius c[13].z, while above 0), so the far side matches at the split;
  // applied before the PCF patch it keeps one far sample in the band. Both patches only insert lines, none equal to
  // the other's anchors: they apply in either order. False with `why` set: the text is not shaped like the fork's
  // 34d98195 (left as it was); with `why` empty: already patched.
  inline bool PatchShadowSplitBlend(const std::string &hlsl, std::string &out, std::string &why)
  {
    using namespace coissue::scan_detail;
    why.clear();
    if (hlsl.find(kShadowBlendMarker) != std::string::npos)
    {
      return false;
    }
    const bool with_pcf = hlsl.find(kShadowPcfMarker) != std::string::npos;
    detail::Lines l = detail::Split(hlsl);
    const size_t start = detail::EntryLine(l);
    if (start == l.lines.size())
    {
      why = "no [shader(...)] entry point";
      return false;
    }
    // Each anchor exactly once in main(): the near fetch, the far cascade's matrix rows and fetches, the split test,
    // the map-A term, the bias and the comparison, in the shapes the block relies on.
    const auto find_one = [&](const std::string &text, size_t &at) {
      size_t n = 0;
      for (size_t i = start; i < l.lines.size(); ++i)
      {
        if (Trim(l.lines[i]) == text)
        {
          at = i;
          ++n;
        }
      }
      if (n != 1)
      {
        why = "'" + text + "' found " + std::to_string(n) + " times";
        return false;
      }
      return true;
    };
    size_t decl = 0, fetch = 0, apply = 0, unused = 0;
    for (const char *text : {"ps = -c[8].w + -r1.z;", "r2.x = dot(r1.xyzw, c[4].xyzw);", "r2.y = dot(r1.xyzw, c[5].xyzw);",
                             "r2.z = dot(r1.xyzw, c[6].xyzw);",
                             "r2.w = tfetch2D(g_Texture2D[4], g_Samplers[2], r2.xy, float2(0, 0)).x;",
                             "r3.w = tfetch2D(g_Texture2D[2], g_Samplers[3], r2.xy, float2(0, 0)).x;",
                             "r0.w = select(r0.w >= 0.0, r0.w, cl15.x);", "ps = saturate(c[18].x * r0.w);",
                             "r0.x = r3.y * r0.x;", "r0.x = -r0.x * r4.x + r0.x;", "r0.w = r0.x * r0.w + -r0.x;",
                             "r2.w = r0.x * cl13.x + cl13.y;", "r3.x = cl10.z >= r3.z;",
                             "r3.z = saturate(-r3.z + cl10.y);"})
    {
      if (!find_one(text, unused))
      {
        return false;
      }
    }
    if (!find_one("CubeMapData cubeMapData = (CubeMapData)0;", decl) ||
        !find_one("r3.w = tfetch2D(g_Texture2D[1], g_Samplers[5], r2.xy, float2(0, 0)).x;", fetch) ||
        !find_one("r0.z = r4.y * cl17.x;", apply) || !(decl < fetch && fetch < apply))
    {
      if (why.empty())
      {
        why = "anchors out of order";
      }
      return false;
    }
    const auto indent = [&](size_t i) {
      const std::string &line = l.lines[i];
      return line.substr(0, line.find_first_not_of(" \t"));
    };
    const std::string fi = indent(fetch), ai = indent(apply);
    std::vector<std::string> block = {
        ai + kShadowBlendMarker + " (pdz_shadow_split_blend): c[13].w = band, a fraction of the split c[8].w; 0 = off",
        ai + "[branch] if (pdzNear && p0 && c[13].w > 0.0)",
        ai + "{",
        ai + "\tfloat pdzBlend = saturate((-r1.z - c[8].w) / (c[8].w * c[13].w) + 1.0);",
        ai + "\t[branch] if (pdzBlend > 0.0)",
        ai + "\t{",
        ai + "\t\tfloat3 pdzLp = float3(dot(r1.xyzw, c[4].xyzw), dot(r1.xyzw, c[5].xyzw), dot(r1.xyzw, c[6].xyzw));",
        ai + "\t\tfloat2 pdzLuv = pdzLp.xy * cl14.xy + cl14.xx;",
        ai + "\t\tfloat pdzA = pdzLp.z - g_Texture2D[4].SampleLevel(g_Samplers[2], pdzLuv, 0.0).x;",
        ai + "\t\tpdzA = saturate(c[18].x * (pdzA >= 0.0 ? pdzA : cl15.x));",
        ai + "\t\tpdzA = r0.x * pdzA + -r0.x + cl15.x;",
        ai + "\t\tfloat pdzZ = pdzLp.z + r2.w;",
        ai + "\t\tfloat pdzDb = pdzZ - g_Texture2D[2].SampleLevel(g_Samplers[3], pdzLuv, 0.0).x;",
        ai + "\t\tfloat pdzLit = float(cl10.z >= pdzDb);",
        ai + "\t\tfloat pdzSoftLit = saturate(-(cl10.w * pdzDb) + cl10.y);",
    };
    if (with_pcf)
    {
      const std::vector<std::string> pcf = {
          ai + "\t\t[branch] if (c[13].z > 0.0)",
          ai + "\t\t{",
          ai + "\t\t\tfloat2 pdzMs = texSize2D(g_Texture2D[2]);",
          ai + "\t\t\tfloat pdzTh = 0.0;",
          ai + "\t\t\tfloat pdzTs = 0.0;",
          ai + "\t\t\t[unroll] for (int pdzJ = -1; pdzJ <= 1; ++pdzJ)",
          ai + "\t\t\t{",
          ai + "\t\t\t\t[unroll] for (int pdzI = -1; pdzI <= 1; ++pdzI)",
          ai + "\t\t\t\t{",
          ai + "\t\t\t\t\tfloat2 pdzT = pdzLuv * pdzMs + float2(pdzI, pdzJ) * c[13].z - 0.5;",
          ai + "\t\t\t\t\tfloat2 pdzT0 = floor(pdzT);",
          ai + "\t\t\t\t\tfloat2 pdzTf = pdzT - pdzT0;",
          ai + "\t\t\t\t\tfloat4 pdzG = pdzZ - g_Texture2D[2].GatherRed(g_Samplers[3], (pdzT0 + 1.0) / pdzMs);",
          ai + "\t\t\t\t\tfloat4 pdzGh = float4(cl10.zzzz >= pdzG);",
          ai + "\t\t\t\t\tfloat4 pdzGs = saturate(-(cl10.wwww * pdzG) + cl10.yyyy);",
          ai + "\t\t\t\t\tfloat pdzTw = (pdzI == 0 ? 2.0 : 1.0) * (pdzJ == 0 ? 2.0 : 1.0);",
          ai + "\t\t\t\t\tpdzTh += pdzTw * lerp(lerp(pdzGh.w, pdzGh.z, pdzTf.x), lerp(pdzGh.x, pdzGh.y, pdzTf.x), pdzTf.y);",
          ai + "\t\t\t\t\tpdzTs += pdzTw * lerp(lerp(pdzGs.w, pdzGs.z, pdzTf.x), lerp(pdzGs.x, pdzGs.y, pdzTf.x), pdzTf.y);",
          ai + "\t\t\t\t}",
          ai + "\t\t\t}",
          ai + "\t\t\tpdzLit = pdzTh * 0.0625;",
          ai + "\t\t\tpdzSoftLit = pdzTs * 0.0625;",
          ai + "\t\t}",
      };
      block.insert(block.end(), pcf.begin(), pcf.end());
    }
    const std::vector<std::string> tail = {
        ai + "\t\tr3.x = lerp(r3.x, pdzLit, pdzBlend);",
        ai + "\t\tr3.z = lerp(r3.z, pdzSoftLit, pdzBlend);",
        ai + "\t\tr0.w = lerp(r0.w, pdzA * pdzA, pdzBlend);",
        ai + "\t}",
        ai + "}",
    };
    block.insert(block.end(), tail.begin(), tail.end());
    // Bottom up, so the earlier indices stay valid.
    l.lines.insert(l.lines.begin() + std::ptrdiff_t(apply), block.begin(), block.end());
    l.lines.insert(l.lines.begin() + std::ptrdiff_t(fetch + 1), fi + "pdzNear = true; " + kShadowBlendMarker);
    l.lines.insert(l.lines.begin() + std::ptrdiff_t(decl + 1), indent(decl) + "bool pdzNear = false; " + kShadowBlendMarker);
    out = detail::Join(l);
    return true;
  }
} // namespace pdz::native::hlslfix
