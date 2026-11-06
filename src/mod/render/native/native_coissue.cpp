// Native renderer (stage 1): co-issue read-before-write fix for the fork's pixel shaders (native_coissue.h), the
// PsParamGen register the fork does not write (native_paramgen.h), the cube instruction / tfetchCube the fork
// reduces to a side structure (native_cubefix.h), and the rcpf / rsqf +-0 semantics and the stacked-array tfetch3D
// (native_hlslfix.h). Cold path: once per pixel shader at its first draw (a microcode scan of a few hundred dwords, a
// look at the container's program-info block and, with pdz_native_cube_fix, a read of the .hlsl; for the shaders that
// need it a text patch and a DXC compile, ~15 ms, or a read of the cached DXIL). The scan / patches are in
// native_coissue_scan.h, native_paramgen.h, native_cubefix.h and native_hlslfix.h.
#include "native_coissue.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>

#include <fmt/format.h>
#include <rex/hash.h> // XXH3_64bits
#include <rex/logging.h>

#include "native_cubefix.h"   // the SDK's cube instruction / tfetchCube for the fork's helpers
#include "native_depth20e4.h" // CompilePixelShader / IsSignedDxil: the fork's dxcompiler, the variants' arguments
#include "native_hlslfix.h"   // rcpf / rsqf +-0, the stacked-array tfetch3D
#include "native_paramgen.h"  // the PsParamGen register of a container, its HLSL patch
#include "native_shaders.h"

// src/mod/render/sun_shadows.cpp (polish 6b): the far-cascade PCF radius; above 0 at the mask shader's first use, it is
// patched (native_hlslfix.h PatchShadowFarPcf).
REXCVAR_DECLARE(double, pdz_shadow_far_pcf);
// Polish 6c: the near / far split blend band; above 0 with pdz_shadow_distance_scale above 1 at the mask shader's first
// use, it is patched (PatchShadowSplitBlend).
REXCVAR_DECLARE(double, pdz_shadow_split_blend);
REXCVAR_DECLARE(double, pdz_shadow_distance_scale);

REXCVAR_DEFINE_BOOL(pdz_native_coissue_fix, true, "Render",
                    "Native renderer: co-issued Xenos ALU operations read their sources before either writes (the "
                    "hardware and the SDK's translator); the XenosRecomp fork lets the scalar operation see the "
                    "vector result. On: a pixel shader with such an instruction (215 in the cache, among them the "
                    "light pass a95c4b61 and the shadow blur 26eaa1f1 of every level) is recompiled at its first use "
                    "from its .hlsl with the scalar line(s) moved above the vector line (cached in "
                    "<shader dir>\\coissue). Read at a shader's first use. Off: the fork's DXIL as is "
                    "(stage1/subagents/bridge.md).");
REXCVAR_DEFINE_BOOL(pdz_native_param_gen, true, "Render",
                    "Native renderer: PsParamGen for the pixel shaders whose container enables it (SQ_PROGRAM_CNTL "
                    "param_gen, 425 in PDZ): register SQ_CONTEXT_MISC.param_gen_pos holds the pixel position with the "
                    "sign bit of x set on a back face, as the hardware and the SDK write it; the XenosRecomp fork reads "
                    "it as an interpolator no VS writes (0: every face a front face). PDZ uses it only for the face: "
                    "two-sided foliage / materials flip their normal on back faces, and the G-buffer foliage shaders "
                    "write the back-face flag RT1.w the sun pass flips the normal with (Jungle: specular on leaves seen "
                    "from below). On: such a shader is recompiled at its first use from its .hlsl with the register "
                    "initialised from SV_Position / SV_IsFrontFace (with the co-issue patch when both apply; cached in "
                    "<shader dir>\\coissue). Read at a shader's first use (stage1/subagents/jungle.md).");
REXCVAR_DEFINE_BOOL(pdz_native_cube_fix, true, "Render",
                    "Native renderer: the Xenos cube instruction and tfetchCube as the SDK computes them "
                    "(native_cubefix.h). The XenosRecomp fork's cube() returns (0, 0, 0, index) and keeps the "
                    "direction in a two-entry side structure for its tfetchCube: the omni-light cookie pass 858ecd68 "
                    "(Nightclub, Mansion, MP Urban / Temple / Gasplant, Sabotage Urban) and 8c549720 project their "
                    "cookie with the face coordinates and so sample one texel (a uniform light: Gasplant's ceiling), "
                    "and 24d8a196 (five cube taps) samples three taps with another tap's direction. On: a pixel shader "
                    "whose main() uses either is recompiled at its first use from its .hlsl with cube() = (T, S, "
                    "2 * major axis, face id) and tfetchCube = that coordinate back to a direction (with the co-issue / "
                    "PsParamGen patches when they apply; cached in <shader dir>\\coissue). Read at a shader's first "
                    "use (stage1/subagents/mpmaps.md).");
REXCVAR_DEFINE_BOOL(pdz_native_rcpf_zero, true, "Render",
                    "Native renderer: rcpf / rsqf (the D3D9-style reciprocal and reciprocal square root) return +-0 "
                    "for an infinite result, as the SDK's translator (kRcpf / kRsqf) and Xenos; the XenosRecomp fork "
                    "clamps them to +-FLT_MAX like rcpc / rsqc. PDZ linearises depth with rcpf: where nothing was drawn "
                    "(depth 0) the emulator gets 0 and the fork FLT_MAX (the harbour refraction fog of PS a2917857 "
                    "saturates there). On: a pixel shader with such an instruction (830 in the cache) is recompiled at "
                    "its first use from its .hlsl with those lines calling pdzRcpF / pdzRsqF (together with the other "
                    "first-use patches; cached in <shader dir>\\coissue). Read at a shader's first use "
                    "(native_hlslfix.h, stage1/subagents/nightclub3.md).");
REXCVAR_DEFINE_BOOL(pdz_native_stacked_array, true, "Render",
                    "Native renderer: a 2D or stacked texture under a pixel shader's tfetch3D is sampled as a "
                    "Texture2DArray, like the SDK (its k2D binding of a tfetch3D): z * layers clamped to the layer "
                    "centres, the two layers around it lerped, the LOD from xy only, every layer at every mip, the "
                    "sampler's anisotropy (the Texture3D volume view's LOD includes the layer axis, its mips halve the "
                    "layers, its filter has no anisotropic footprint), resolve mirrors included. On: a pixel shader "
                    "with tfetch3D (400 in the cache) is recompiled at its first use with each tfetch3D reading space3 "
                    "t<n> when it holds an array (cached in <shader dir>\\coissue); a true 3D texture keeps the "
                    "Texture3D path. Read at a shader's first use; the root signature always has the space3 range "
                    "(native_hlslfix.h, stage1/subagents/nightclub3.md).");
REXCVAR_DEFINE_BOOL(pdz_native_live_literals, true, "Render",
                    "Native renderer: a pixel shader's literal registers (the fork's baked 'static const float4 cl<n>') "
                    "are read from the uploaded constants, i.e. the device shadow D3D fills with the bound object's "
                    "literals at SetPixelShader, as the emulator reads the register file. The shader cache is keyed "
                    "by the microcode, but PDZ ships 61 pixel shaders in containers whose literals differ: the "
                    "per-material parallax scale c1.w of the G-buffer / material shaders (a9204a6c bakes 0.01, Temple "
                    "draws 0.03-0.07; 35e06528 bakes 0.1, Mansion draws 0.02-0.1), so natively most parallax walls / "
                    "floors had another depth than the emulator's. On: every pixel shader with literals below PS c224 "
                    "is recompiled at its first use with those registers read from c[] (together with the other "
                    "first-use patches; cached in <shader dir>\\coissue). Read at a shader's first use. Off: the "
                    "fork's baked values (native_hlslfix.h, stage1/subagents/temple2.md).");

// The pipeline list's collect pass (depth20e4::batch) runs Apply on copies of the shaders: nothing of it is logged.
#define COISSUE_LOG_INFO(...)                                                                                            do                                                                                                                     {                                                                                                                        if (!::pdz::native::depth20e4::batch::Collecting())                                                                    {                                                                                                                        REXLOG_INFO(__VA_ARGS__);                                                                                            }                                                                                                                    } while (0)
#define COISSUE_LOG_WARN(...)                                                                                            do                                                                                                                     {                                                                                                                        if (!::pdz::native::depth20e4::batch::Collecting())                                                                    {                                                                                                                        REXLOG_WARN(__VA_ARGS__);                                                                                            }                                                                                                                    } while (0)

namespace pdz::native::coissue
{
  namespace
  {
    constexpr uint32_t kLogShaders = 16; // shaders logged individually (then only the counters)
    constexpr uint32_t kLivePsRegisters = 224; // PS c0..c223 are uploaded (native_draw.cpp kPsFloatCount)

    std::string MaskName(uint32_t mask)
    {
      std::string s;
      for (uint32_t i = 0; i < 4; ++i)
      {
        if ((mask >> i) & 1u)
        {
          s += "xyzw"[i];
        }
      }
      return s.empty() ? "_" : s;
    }

    bool FileExists(const std::string &path)
    {
      std::error_code ec;
      return std::filesystem::is_regular_file(path, ec);
    }

    bool ReadText(const std::string &path, std::string &out)
    {
      std::ifstream f(path, std::ios::binary);
      if (!f)
      {
        return false;
      }
      out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
      return true;
    }

    bool ReadBinary(const std::string &path, std::vector<uint8_t> &out)
    {
      std::ifstream f(path, std::ios::binary);
      if (!f)
      {
        return false;
      }
      out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
      return true;
    }

    Stats g_stats;
    ParamGenStats g_param_gen;
    CubeFixStats g_cube;
    HlslFixStats g_rcpf;  // pdz_native_rcpf_zero (sites = rcpf / rsqf instructions)
    HlslFixStats g_array; // pdz_native_stacked_array (sites = tfetch3D slots rewritten)
    HlslFixStats g_live;  // pdz_native_live_literals (sites = literal registers read from c[])
    bool g_pcf_logged = false;   // pdz_shadow_far_pcf: the one shader's outcome is logged once
    bool g_blend_logged = false; // pdz_shadow_split_blend: likewise

    void CountFixFailure(HlslFixStats &st, const char *name, uint64_t hash, const std::string &what)
    {
      ++st.failed;
      if (st.failed <= kLogShaders)
      {
        COISSUE_LOG_WARN("pdz_native: {}: ps {:016x}: {}{}", name, hash, what,
                    st.failed == kLogShaders ? "; further failures only in the counters" : "");
      }
    }

    void CountFailure(uint64_t hash, const std::string &what)
    {
      ++g_stats.failed;
      if (g_stats.failed <= kLogShaders)
      {
        COISSUE_LOG_WARN("pdz_native: co-issue fix: ps {:016x}: {}{}", hash, what,
                    g_stats.failed == kLogShaders ? "; further failures only in the counters" : "");
      }
    }

    void CountParamGenFailure(uint64_t hash, const std::string &what)
    {
      ++g_param_gen.failed;
      if (g_param_gen.failed <= kLogShaders)
      {
        COISSUE_LOG_WARN("pdz_native: param gen: ps {:016x}: {}{}", hash, what,
                    g_param_gen.failed == kLogShaders ? "; further failures only in the counters" : "");
      }
    }

    void CountCubeFailure(uint64_t hash, const std::string &what)
    {
      ++g_cube.failed;
      if (g_cube.failed <= kLogShaders)
      {
        COISSUE_LOG_WARN("pdz_native: cube fix: ps {:016x}: {}{}", hash, what,
                    g_cube.failed == kLogShaders ? "; further failures only in the counters" : "");
      }
    }

    // The PsParamGen register from the research cache's copy of the container (<shader dir's parent>\containers\
    // <hash>.bin, the sibling of cache\hlsl) when its microcode is the shader's; `found` = such a file was read.
    uint32_t ParamGenFromCacheContainer(const NativeShader &s, bool &found)
    {
      found = false;
      if (s.source.empty())
      {
        return paramgen::kNone;
      }
      std::error_code ec;
      const std::filesystem::path p =
          std::filesystem::path(s.source).parent_path() / "containers" / fmt::format("{:016x}.bin", s.hash);
      std::vector<uint8_t> b;
      if (!std::filesystem::is_regular_file(p, ec) || !ReadBinary(p.string(), b) || b.size() < 0x18)
      {
        return paramgen::kNone;
      }
      const uint32_t header = paramgen::ReadBe32(b.data() + 4);
      const uint32_t ucode = paramgen::ReadBe32(b.data() + 8);
      if (header > b.size() || ucode > b.size() - header || XXH3_64bits(b.data() + header, ucode) != s.hash)
      {
        return paramgen::kNone;
      }
      found = true;
      return paramgen::RegisterFromContainer(b.data(), b.size());
    }
  } // namespace

  void Apply(NativeShader &s, const uint8_t *ucode_be, uint32_t bytes, const uint8_t *container_be,
             uint32_t container_bytes)
  {
    if (s.coissue_checked)
    {
      return;
    }
    s.coissue_checked = true;
    const bool coissue_on = REXCVAR_GET(pdz_native_coissue_fix);
    const bool param_gen_on = REXCVAR_GET(pdz_native_param_gen);
    const bool cube_on = REXCVAR_GET(pdz_native_cube_fix);
    const bool rcpf_on = REXCVAR_GET(pdz_native_rcpf_zero);
    const bool array_on = REXCVAR_GET(pdz_native_stacked_array);
    const bool live_on = REXCVAR_GET(pdz_native_live_literals);
    // pdz_shadow_far_pcf (an enhancement, off by default): only the sun shadow mask shader, only when on at its first use.
    const bool pcf_wanted = REXCVAR_GET(pdz_shadow_far_pcf) > 0.0 && s.hash == hlslfix::kShadowMaskShader;
    // pdz_shadow_split_blend (part of the longer shadows): the same shader, only with the band above 0 and
    // pdz_shadow_distance_scale above 1 at its first use (so the stock settings never patch it).
    const bool blend_wanted = REXCVAR_GET(pdz_shadow_split_blend) > 0.0 && REXCVAR_GET(pdz_shadow_distance_scale) > 1.0 &&
                              s.hash == hlslfix::kShadowMaskShader;
    if ((!coissue_on && !param_gen_on && !cube_on && !rcpf_on && !array_on && !live_on && !pcf_wanted && !blend_wanted) ||
        s.is_vertex || s.dxil.empty() || !s.usable)
    {
      return;
    }
    // The microcode must be the one the shader was translated from: its XXH3 is the cache key.
    const bool ucode_matches = ucode_be && bytes && XXH3_64bits(ucode_be, bytes) == s.hash;

    ScanResult scan;
    std::string list;
    if ((coissue_on || rcpf_on) && ucode_matches)
    {
      scan = Scan(ucode_be, bytes);
    }
    // rcpf / rsqf (native_hlslfix.h): the scan lists their "ps = " ordinals.
    if (rcpf_on)
    {
      ++g_rcpf.checked;
      if (scan.ok && !scan.rcpf_rsqf.empty())
      {
        ++g_rcpf.with_sites;
        g_rcpf.sites += uint32_t(scan.rcpf_rsqf.size());
      }
    }
    const bool rcpf_wanted = rcpf_on && scan.ok && !scan.rcpf_rsqf.empty();
    // The stacked-array patch needs no scan: the sidecar says whether the shader has a tfetch3D slot.
    bool array_wanted = false;
    if (array_on)
    {
      ++g_array.checked;
      for (const ShaderTexture &t : s.textures)
      {
        array_wanted |= t.dim == TexDim::k3D;
      }
      if (array_wanted)
      {
        ++g_array.with_sites;
      }
    }
    if (coissue_on)
    {
      ++g_stats.checked;
      if (!ucode_matches)
      {
        CountFailure(s.hash, fmt::format("the microcode in guest memory ({} bytes) does not hash to the shader: not "
                                         "checked",
                                         bytes));
      }
      else
      {
        if (!scan.ok)
        {
          CountFailure(s.hash, "microcode not parsed: " + scan.why);
        }
        else if (!scan.hazards.empty())
        {
          ++g_stats.hazardous;
          g_stats.hazards += uint32_t(scan.hazards.size());
          for (const Hazard &h : scan.hazards)
          {
            list += fmt::format("{}@{} {} r{}.{} | {}", list.empty() ? "" : "; ", h.address,
                                scan_detail::VectorName(h.vector_opcode), h.vector_dest, MaskName(h.vector_mask),
                                scan_detail::ScalarName(h.scalar_opcode));
          }
        }
      }
    }
    const bool coissue_wanted = coissue_on && scan.ok && !scan.hazards.empty();

    // PsParamGen: the program-info block of the shader's container, in guest memory right before the microcode
    // (trusted when that microcode is the shader's), else the research cache's copy of the container.
    uint32_t param_gen = paramgen::kNone;
    if (param_gen_on)
    {
      ++g_param_gen.checked;
      if (ucode_matches && container_be && container_bytes)
      {
        param_gen = paramgen::RegisterFromContainer(container_be, container_bytes);
      }
      else
      {
        bool from_file = false;
        param_gen = ParamGenFromCacheContainer(s, from_file);
        if (from_file)
        {
          ++g_param_gen.from_file;
        }
      }
      if (param_gen != paramgen::kNone)
      {
        ++g_param_gen.enabled;
      }
    }
    // The cube fix and the live literals cannot tell from the microcode scan whether they apply: with either on, every
    // pixel shader's .hlsl is read once (only those whose main() uses the cube instruction or a cube fetch, resp. that
    // declare literal registers, are recompiled).
    if (!coissue_wanted && param_gen == paramgen::kNone && !cube_on && !rcpf_wanted && !array_wanted && !live_on &&
        !pcf_wanted && !blend_wanted)
    {
      return;
    }

    // The fork's .hlsl of this DXIL: next to it, else in the primary shader dir.
    const std::string name = fmt::format("{:016x}.hlsl", s.hash);
    std::string path;
    for (const std::string &d : {s.source, ShaderCache::Get().directory()})
    {
      if (!d.empty() && FileExists(d + "\\" + name))
      {
        path = d + "\\" + name;
        break;
      }
    }
    std::string hlsl;
    if (path.empty() || !ReadText(path, hlsl))
    {
      if (coissue_wanted)
      {
        CountFailure(s.hash, fmt::format("{}: no .hlsl next to the shader, drawn as translated", list));
      }
      if (param_gen != paramgen::kNone)
      {
        CountParamGenFailure(s.hash, "no .hlsl next to the shader, drawn as translated");
      }
      if (rcpf_wanted)
      {
        CountFixFailure(g_rcpf, "rcpf zero", s.hash, "no .hlsl next to the shader, drawn as translated");
      }
      if (array_wanted)
      {
        CountFixFailure(g_array, "stacked array", s.hash, "no .hlsl next to the shader, drawn as translated");
      }
      if (live_on && !s.float_literals.empty())
      {
        CountFixFailure(g_live, "live literals", s.hash, "no .hlsl next to the shader, drawn as translated");
      }
      if (pcf_wanted)
      {
        COISSUE_LOG_WARN("pdz_native: far shadow PCF: ps {:016x}: no .hlsl next to the shader, drawn without PCF", s.hash);
      }
      if (blend_wanted)
      {
        COISSUE_LOG_WARN("pdz_native: shadow split blend: ps {:016x}: no .hlsl next to the shader, drawn without the blend",
                    s.hash);
      }
      return;
    }
    const bool cube_wanted = cube_on && cubefix::Uses(hlsl);
    if (cube_on)
    {
      ++g_cube.checked;
      if (cube_wanted)
      {
        ++g_cube.used;
      }
    }
    const bool live_wanted = live_on && hlsl.find("static const float4 cl") != std::string::npos;
    if (live_on)
    {
      ++g_live.checked;
      if (live_wanted)
      {
        ++g_live.with_sites;
      }
    }
    if (!coissue_wanted && param_gen == paramgen::kNone && !cube_wanted && !rcpf_wanted && !array_wanted && !live_wanted &&
        !pcf_wanted && !blend_wanted)
    {
      return;
    }
    // The rcpf / rsqf lines first (they keep the "ps = " order the co-issue patch counts), then the co-issue moves, the
    // PsParamGen initialisation at the top of main(), the cube helpers (the prelude only, plus tfetchCubeLevel call
    // sites), and last the tfetch3D calls of main() with their prelude before the entry point.
    std::string source = hlsl, why;
    bool coissue_done = false, param_gen_done = false, cube_done = false, rcpf_done = false, array_done = false;
    bool live_done = false, pcf_done = false, blend_done = false;
    uint32_t rcpf_lines = 0, array_slots = 0, live_registers = 0;
    if (rcpf_wanted)
    {
      std::string patched;
      if (hlslfix::PatchRcpF(source, scan, patched, why, rcpf_lines))
      {
        source = std::move(patched);
        rcpf_done = true;
      }
      else if (why.empty())
      {
        ++g_rcpf.already; // every such line calls the helper already
      }
      else
      {
        CountFixFailure(g_rcpf, "rcpf zero", s.hash, fmt::format("{} not patched ({}), drawn without this fix", path, why));
      }
    }
    if (coissue_wanted)
    {
      std::string patched;
      if (Patch(source, scan, patched, why))
      {
        source = std::move(patched);
        coissue_done = true;
      }
      else
      {
        CountFailure(s.hash, fmt::format("{}: {} not patched ({}), drawn without this fix", list, path, why));
      }
    }
    if (param_gen != paramgen::kNone)
    {
      std::string patched;
      if (paramgen::Patch(source, param_gen, patched, why))
      {
        source = std::move(patched);
        param_gen_done = true;
      }
      else if (why.empty())
      {
        ++g_param_gen.already; // the translation computes it already
      }
      else
      {
        CountParamGenFailure(s.hash, fmt::format("r{}: {} not patched ({}), drawn without this fix", param_gen, path, why));
      }
    }
    if (cube_wanted)
    {
      std::string patched;
      if (cubefix::Patch(source, patched, why))
      {
        source = std::move(patched);
        cube_done = true;
      }
      else if (!why.empty())
      {
        CountCubeFailure(s.hash, fmt::format("{} not patched ({}), drawn without this fix", path, why));
      }
    }
    if (array_wanted)
    {
      std::string patched;
      if (hlslfix::PatchStackedArray(source, patched, why, array_slots))
      {
        source = std::move(patched);
        array_done = true;
      }
      else if (why.empty())
      {
        ++g_array.already; // no tfetch3D call in the entry point, or already patched
      }
      else
      {
        CountFixFailure(g_array, "stacked array", s.hash,
                        fmt::format("{} not patched ({}), drawn without this fix", path, why));
      }
    }
    // Last, the literal declarations (above the entry point; one line for one, so nothing above counts lines).
    if (live_wanted)
    {
      std::string patched;
      if (hlslfix::PatchLiveLiterals(source, kLivePsRegisters, patched, why, live_registers))
      {
        source = std::move(patched);
        live_done = true;
      }
      else if (why.empty())
      {
        ++g_live.already; // only registers past PS c223, or already patched
      }
      else
      {
        CountFixFailure(g_live, "live literals", s.hash, fmt::format("{} not patched ({}), drawn without this fix", path, why));
      }
    }
    // The far-cascade PCF (pdz_shadow_far_pcf) after everything else: it inserts lines (none a "ps = " line) and reads
    // cl10 as declared or as the live literal's #define.
    if (pcf_wanted)
    {
      std::string patched;
      if (hlslfix::PatchShadowFarPcf(source, patched, why))
      {
        source = std::move(patched);
        pcf_done = true;
      }
      else if (!why.empty())
      {
        COISSUE_LOG_WARN("pdz_native: far shadow PCF: ps {:016x}: {} not patched ({}), drawn without PCF", s.hash, path, why);
      }
    }
    // The split blend (pdz_shadow_split_blend) after the PCF patch, so its band compares the far map with the same
    // taps; it only inserts lines too.
    if (blend_wanted)
    {
      std::string patched;
      if (hlslfix::PatchShadowSplitBlend(source, patched, why))
      {
        source = std::move(patched);
        blend_done = true;
      }
      else if (!why.empty())
      {
        COISSUE_LOG_WARN("pdz_native: shadow split blend: ps {:016x}: {} not patched ({}), drawn without the blend", s.hash,
                    path, why);
      }
    }
    if (!coissue_done && !param_gen_done && !cube_done && !rcpf_done && !array_done && !live_done && !pcf_done &&
        !blend_done)
    {
      return;
    }

    // <shader dir>\coissue\<ps>_<XXH3 of the patched source>.dxil: a changed .hlsl (or patch set) never reuses a stale
    // file.
    const std::string cache_path = fmt::format("{}\\coissue\\{:016x}_{:016x}.dxil", ShaderCache::Get().directory(),
                                               s.hash, XXH3_64bits(source.data(), source.size()));
    std::vector<uint8_t> dxil;
    const bool from_disk = ReadBinary(cache_path, dxil) && depth20e4::IsSignedDxil(dxil);
    uint32_t ms = 0;
    if (!from_disk)
    {
      std::string errors;
      const auto t0 = std::chrono::steady_clock::now();
      const bool ok = depth20e4::CompilePixelShader(source, dxil, errors);
      ms = uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
      g_stats.compile_ms += ms;
      if (!ok)
      {
        const std::string what = fmt::format("{}{}compile failed ({}), drawn as translated",
                                             param_gen_done ? fmt::format("with PsParamGen r{}: ", param_gen) : "",
                                             cube_done ? "with the cube fix: " : "", errors.substr(0, 600));
        if (coissue_done)
        {
          CountFailure(s.hash, list + ": " + what);
        }
        if (param_gen_done)
        {
          CountParamGenFailure(s.hash, what);
        }
        if (cube_done)
        {
          CountCubeFailure(s.hash, what);
        }
        if (rcpf_done)
        {
          CountFixFailure(g_rcpf, "rcpf zero", s.hash, what);
        }
        if (array_done)
        {
          CountFixFailure(g_array, "stacked array", s.hash, what);
        }
        if (live_done)
        {
          CountFixFailure(g_live, "live literals", s.hash, what);
        }
        if (pcf_done)
        {
          COISSUE_LOG_WARN("pdz_native: far shadow PCF: ps {:016x}: {}", s.hash, what);
        }
        if (blend_done)
        {
          COISSUE_LOG_WARN("pdz_native: shadow split blend: ps {:016x}: {}", s.hash, what);
        }
        return;
      }
      if (!depth20e4::batch::Collecting()) // the collect pass's placeholder never goes to disk
      {
        depth20e4::SaveCacheFile(cache_path, dxil);
      }
    }
    s.dxil = std::move(dxil);
    s.coissue_hlsl = std::move(source);
    if (array_done)
    {
      s.array_slots = array_slots; // BindSrvTable fills space3 t<n> for these slots from now on
      g_array.sites += uint32_t(__builtin_popcount(array_slots));
    }
    if (live_done)
    {
      g_live.sites += live_registers;
    }
    if (coissue_done)
    {
      ++g_stats.patched;
      if (from_disk)
      {
        ++g_stats.from_disk;
      }
      if (g_stats.patched <= kLogShaders)
      {
        COISSUE_LOG_INFO("pdz_native: co-issue fix: ps {:016x}: {} instruction(s) whose scalar operation read the vector "
                    "result ({}): patched ({} bytes DXIL, {}){}",
                    s.hash, scan.hazards.size(), list, s.dxil.size(),
                    from_disk ? std::string("from the disk cache") : fmt::format("compiled in {} ms", ms),
                    g_stats.patched == kLogShaders ? "; further shaders only in the counters" : "");
      }
    }
    if (param_gen_done)
    {
      ++g_param_gen.patched;
      if (g_param_gen.patched <= kLogShaders)
      {
        COISSUE_LOG_INFO("pdz_native: param gen: ps {:016x}: PsParamGen in r{} (pixel position, sign of x = back face){}: "
                    "patched ({} bytes DXIL, {}){}",
                    s.hash, param_gen, coissue_done ? " with the co-issue patch" : "", s.dxil.size(),
                    from_disk ? std::string("from the disk cache") : fmt::format("compiled in {} ms", ms),
                    g_param_gen.patched == kLogShaders ? "; further shaders only in the counters" : "");
      }
    }
    if (cube_done)
    {
      ++g_cube.patched;
      if (g_cube.patched <= kLogShaders)
      {
        COISSUE_LOG_INFO("pdz_native: cube fix: ps {:016x}: the cube instruction / tfetchCube as the SDK computes them{}{}: "
                    "patched ({} bytes DXIL, {}){}",
                    s.hash, coissue_done ? ", with the co-issue patch" : "",
                    param_gen_done ? ", with PsParamGen" : "", s.dxil.size(),
                    from_disk ? std::string("from the disk cache") : fmt::format("compiled in {} ms", ms),
                    g_cube.patched == kLogShaders ? "; further shaders only in the counters" : "");
      }
    }
    if (pcf_done && !g_pcf_logged)
    {
      g_pcf_logged = true;
      COISSUE_LOG_INFO("pdz_native: far shadow PCF: ps {:016x} (the sun shadow mask): the far cascade compares 9 bilinear "
                  "taps, radius c13.z = pdz_shadow_far_pcf {:g} texels (read every frame): patched ({} bytes DXIL, {})",
                  s.hash, REXCVAR_GET(pdz_shadow_far_pcf), s.dxil.size(),
                  from_disk ? std::string("from the disk cache") : fmt::format("compiled in {} ms", ms));
    }
    if (blend_done && !g_blend_logged)
    {
      g_blend_logged = true;
      COISSUE_LOG_INFO("pdz_native: shadow split blend: ps {:016x} (the sun shadow mask): the near cascade's last c13.w = "
                  "pdz_shadow_split_blend {:g} of its range mixes in the far cascade{} (read every frame): patched ({} "
                  "bytes DXIL, {})",
                  s.hash, REXCVAR_GET(pdz_shadow_split_blend), pcf_done ? " with the PCF taps" : "", s.dxil.size(),
                  from_disk ? std::string("from the disk cache") : fmt::format("compiled in {} ms", ms));
    }
    for (HlslFixStats *st : {rcpf_done ? &g_rcpf : nullptr, array_done ? &g_array : nullptr, live_done ? &g_live : nullptr})
    {
      if (!st)
      {
        continue;
      }
      ++st->patched;
      if (from_disk)
      {
        ++st->from_disk;
      }
      if (st->patched <= kLogShaders)
      {
        const std::string what =
            st == &g_rcpf
                ? fmt::format("rcpf zero: ps {:016x}: {} rcpf / rsqf instruction(s) give +-0 for an infinite result",
                              s.hash, rcpf_lines)
            : st == &g_array
                ? fmt::format("stacked array: ps {:016x}: tfetch3D of slot mask {:08X} reads a Texture2DArray when a 2D "
                              "/ stacked texture is bound",
                              s.hash, array_slots)
                : fmt::format("live literals: ps {:016x}: {} literal register(s) read from the uploaded constants (the "
                              "bound object's, D3D's SetPixelShader copy)",
                              s.hash, live_registers);
        COISSUE_LOG_INFO("pdz_native: {}: patched ({} bytes DXIL, {}){}", what, s.dxil.size(),
                    from_disk ? std::string("from the disk cache") : fmt::format("compiled in {} ms", ms),
                    st->patched == kLogShaders ? "; further shaders only in the counters" : "");
      }
    }
  }

  const Stats &GetStats() { return g_stats; }

  void ResetForSeed()
  {
    g_stats = Stats();
    g_param_gen = ParamGenStats();
    g_cube = CubeFixStats();
    g_rcpf = HlslFixStats();
    g_array = HlslFixStats();
    g_live = HlslFixStats();
    g_pcf_logged = false;
    g_blend_logged = false;
  }
  const ParamGenStats &GetParamGenStats() { return g_param_gen; }
  const CubeFixStats &GetCubeFixStats() { return g_cube; }
  const HlslFixStats &GetRcpfStats() { return g_rcpf; }
  const HlslFixStats &GetArrayStats() { return g_array; }
  const HlslFixStats &GetLiveLiteralStats() { return g_live; }

  std::string StatsText()
  {
    return fmt::format("co-issue fix {}: checked {}, hazardous {} ({} instructions), patched {} ({} from disk, {} ms), "
                       "failed {} | param gen {}: checked {} ({} from the cache's containers), enabled {}, patched {}, "
                       "already {}, failed {} | cube fix {}: checked {}, using {}, patched {}, failed {} | rcpf zero "
                       "{}: checked {}, with rcpf / rsqf {} ({} instructions), patched {} ({} from disk), already {}, "
                       "failed {} | stacked array {}: checked {}, with tfetch3D {}, patched {} ({} slots, {} from "
                       "disk), already {}, failed {} | live literals {}: checked {}, with literals {}, patched {} ({} "
                       "registers, {} from disk), already {}, failed {}",
                       REXCVAR_GET(pdz_native_coissue_fix) ? "on" : "off", g_stats.checked, g_stats.hazardous,
                       g_stats.hazards, g_stats.patched, g_stats.from_disk, g_stats.compile_ms, g_stats.failed,
                       REXCVAR_GET(pdz_native_param_gen) ? "on" : "off", g_param_gen.checked, g_param_gen.from_file,
                       g_param_gen.enabled, g_param_gen.patched, g_param_gen.already, g_param_gen.failed,
                       REXCVAR_GET(pdz_native_cube_fix) ? "on" : "off", g_cube.checked, g_cube.used, g_cube.patched,
                       g_cube.failed, REXCVAR_GET(pdz_native_rcpf_zero) ? "on" : "off", g_rcpf.checked,
                       g_rcpf.with_sites, g_rcpf.sites, g_rcpf.patched, g_rcpf.from_disk, g_rcpf.already, g_rcpf.failed,
                       REXCVAR_GET(pdz_native_stacked_array) ? "on" : "off", g_array.checked, g_array.with_sites,
                       g_array.patched, g_array.sites, g_array.from_disk, g_array.already, g_array.failed,
                       REXCVAR_GET(pdz_native_live_literals) ? "on" : "off", g_live.checked, g_live.with_sites,
                       g_live.patched, g_live.sites, g_live.from_disk, g_live.already, g_live.failed);
  }
} // namespace pdz::native::coissue
