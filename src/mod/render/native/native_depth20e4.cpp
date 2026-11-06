// Native renderer (stage 1): SV_Depth 20e4 pixel-shader variants (native_depth20e4.h). Cold path except Wanted() /
// Constants() (a few register bit tests per draw, only when the cvar is on).
#include "native_depth20e4.h"
#include "native_dxc.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <unordered_map>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <objidl.h> // IUnknown / IStream for dxcapi.h, whatever was included before
#include <dxcapi.h>
#include <fmt/format.h>
#include <rex/graphics/registers.h>
#include <rex/hash.h> // XXH3_64bits: the on-disk variant cache key
#include <rex/graphics/xenos.h>
#include <rex/logging.h>

#include "native_depth20e4_hlsl.h"

REXCVAR_DEFINE_INT32(pdz_native_depth20e4, 1, "Render",
                     "Native renderer: guest float24 (20e4) depth in the pixel shader for draws into a D24FS8 depth "
                     "buffer, as the SDK's ROV path (the reference): a PS variant writes SV_Depth = 20e4 round to "
                     "nearest even of (z + polygon offset applied in the shader), instead of the fixed-function D3D12 "
                     "bias on float32 depth. 0 = off (fixed-function bias), 1 = draws with a polygon offset only, "
                     "2 = every depth-tested draw on D24FS8 (all stored depth is 20e4; costs early-Z on all of them)")
    .range(0, 2)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(pdz_native_depth20e4_edge, false, "Render",
                    "Native renderer, with pdz_native_depth20e4 on: polygon-offset draws that test depth without "
                    "writing it (decals) output the edge of their 20e4 rounding interval on the side the depth "
                    "function looks at (upper for GREATER_EQUAL / LESS, lower for LESS_EQUAL / GREATER) instead of "
                    "the 20e4 value, so the host compare against the float32 depth of the surface under them gives "
                    "the ROV's 24-bit answer also for offsets under one 20e4 step (mode 1 rounds only the decal). "
                    "Off = the 20e4 value (unchanged). Separate PS variants; z-fighting check, stage2 polish 4")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_native_dxc_preload, true, "Render",
                    "Native renderer: load the shader variants' compiler (dxcompiler.dll + dxil.dll, see "
                    "pdz_native_dxc_dir) on a worker thread when the pipeline cache is created, instead of at the "
                    "first compile. A compile is only needed for a pixel shader whose co-issue / PsParamGen patch, 7e3 "
                    "blend or depth20e4 variant is not in the disk caches yet (one never drawn before): with this on "
                    "it costs the compile alone (about 12 ms), not also the compiler's load mid-level (log 004 20:20:33, "
                    "a co-issue patch). The variants in the disk caches never load it. Off = loaded at the first "
                    "compile, as before")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);


namespace pdz::native::depth20e4
{
  namespace
  {
    namespace reg = rex::graphics::reg;
    namespace xenos = rex::graphics::xenos;

    constexpr uint32_t kLogBuilds = 8; // variants logged individually (then only the periodic counters)
    constexpr uint32_t kFirstUseLogLines = 32; // pdz_native_first_use_log: compiles past kLogBuilds logged individually
    uint32_t g_first_use_logged = 0;

    Stats g_stats;

    struct Entry
    {
      std::vector<uint8_t> dxil;
      bool failed = false;
    };
    // By PS hash (0 = the depth-only shader), one map per compare edge (EdgeFor: 0 plain, 1 upper, 2 lower);
    // render thread only.
    std::unordered_map<uint64_t, Entry> g_variants[3];

    std::wstring Widen(const std::string &s) { return std::wstring(s.begin(), s.end()); }

    bool FileExists(const std::string &path)
    {
      const DWORD a = GetFileAttributesA(path.c_str());
      return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
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

    // A DXBC container whose hash is set: signed by dxil.dll (CreateGraphicsPipelineState rejects unsigned DXIL).
    bool IsSignedContainer(const std::vector<uint8_t> &c)
    {
      if (c.size() < 32 || std::memcmp(c.data(), "DXBC", 4) != 0)
      {
        return false;
      }
      bool any = false;
      for (uint32_t i = 4; i < 20; ++i)
      {
        any |= c[i] != 0;
      }
      return any;
    }

    std::string DirOf(const std::string &path)
    {
      const size_t slash = path.find_last_of("\\/");
      return slash == std::string::npos ? std::string() : path.substr(0, slash);
    }

    // The pipeline list's first-start pass (batch::, native_pipeline_seed.cpp, 2026-11-03): a collect pass records
    // every source the co-issue fix and the variants would compile, workers compile them all at once, and the real
    // pass's compiles take the results by source. Draw thread (or the start-up thread before the draws begin).
    struct Batch
    {
      bool collecting = false;
      std::unordered_map<uint64_t, std::string> sources;       // by XXH3 of the source
      std::unordered_map<uint64_t, std::vector<uint8_t>> done; // compiled, not yet taken
      std::vector<std::pair<std::string, std::vector<uint8_t>>> writes; // the real pass's cache files, written at Clear
    };
    Batch g_batch;
    // What a compile returns while collecting: not DXIL (the callers keep it off disk: batch::Collecting).
    constexpr uint8_t kCollectPlaceholder[4] = {'P', 'D', 'Z', 0};

    // Same arguments as the fork's DxcCompiler::compile for a pixel shader (XenosRecomp/dxc_compiler.cpp), so the
    // guest part of the variant compiles exactly like the cached DXIL. Any thread, with its own compiler.
    bool CompileWith(IDxcCompiler3 *compiler, const std::string &source, std::vector<uint8_t> &out, std::string &errors)
    {
      DxcBuffer buffer = {};
      buffer.Ptr = source.data();
      buffer.Size = source.size();
      buffer.Encoding = 0;
      const wchar_t *args[] = {L"-T", L"ps_6_0", L"-E", L"main", L"-HV", L"2021", L"-all-resources-bound",
                               L"-Wno-ignored-attributes", L"-Qstrip_reflect", L"-Qstrip_debug"};
      IDxcResult *result = nullptr;
      HRESULT hr = compiler->Compile(&buffer, args, UINT32(sizeof(args) / sizeof(args[0])), nullptr,
                                     __uuidof(IDxcResult), reinterpret_cast<void **>(&result));
      if (FAILED(hr) || !result)
      {
        errors = fmt::format("Compile() failed ({:#x})", uint32_t(hr));
        return false;
      }
      HRESULT status = E_FAIL;
      result->GetStatus(&status);
      if (FAILED(status))
      {
        IDxcBlobUtf8 *text = nullptr;
        if (SUCCEEDED(result->GetOutput(DXC_OUT_ERRORS, __uuidof(IDxcBlobUtf8), reinterpret_cast<void **>(&text), nullptr)) &&
            text)
        {
          errors.assign(text->GetStringPointer(), text->GetStringLength());
          text->Release();
        }
        result->Release();
        if (errors.empty())
        {
          errors = fmt::format("status {:#x}", uint32_t(status));
        }
        return false;
      }
      IDxcBlob *object = nullptr;
      if (FAILED(result->GetOutput(DXC_OUT_OBJECT, __uuidof(IDxcBlob), reinterpret_cast<void **>(&object), nullptr)) ||
          !object || object->GetBufferSize() < 20)
      {
        if (object) object->Release();
        result->Release();
        errors = "no object";
        return false;
      }
      const uint8_t *bytes = static_cast<const uint8_t *>(object->GetBufferPointer());
      out.assign(bytes, bytes + object->GetBufferSize());
      object->Release();
      result->Release();
      if (!IsSignedContainer(out))
      {
        errors = "the container is not signed (dxil.dll not used by dxcompiler)";
        out.clear();
        return false;
      }
      return true;
    }

    // dxcompiler.dll from the fork's build. dxil.dll is loaded first by full path: dxcompiler loads "dxil.dll" by
    // name to validate and sign, and an already loaded module of that name is what the loader returns.
    struct Compiler
    {
      std::mutex mutex; // Init: the draw thread, or the pdz_native_dxc_preload worker (Compile: the draw thread only)
      bool tried = false;
      IDxcCompiler3 *compiler = nullptr;
      std::string dir;
      uint32_t compiles = 0; // Compile() calls (CompileCount)

      bool Init(bool preload = false)
      {
        std::lock_guard lock(mutex);
        if (tried)
        {
          return compiler != nullptr;
        }
        tried = true;
        const auto t0 = std::chrono::steady_clock::now();
        // The shared DXC (native_dxc.h, 2026-11-03): next to the exe, or the fork's dxc-bin in a developer's build.
        auto create = reinterpret_cast<DxcCreateInstanceProc>(dxc::CreateProc());
        if (create && SUCCEEDED(create(CLSID_DxcCompiler, __uuidof(IDxcCompiler3), reinterpret_cast<void **>(&compiler))) &&
            compiler)
        {
          dir = dxc::LoadedFrom();
          REXLOG_INFO("pdz_native: depth20e4: shader compiler ready ({}) in {} ms ({})", dir,
                      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count(),
                      preload ? "preloaded on a worker, pdz_native_dxc_preload" : "at the first compile");
          return true;
        }
        compiler = nullptr;
        g_stats.compiler_failed = true;
        REXLOG_WARN("pdz_native: depth20e4: no shader compiler (see the DXC line above); the SV_Depth variants are off, "
                    "draws keep the fixed-function depth bias");
        return false;
      }

      bool Compile(const std::string &source, std::vector<uint8_t> &out, std::string &errors)
      {
        if (g_batch.collecting)
        {
          g_batch.sources.emplace(XXH3_64bits(source.data(), source.size()), source);
          out.assign(kCollectPlaceholder, kCollectPlaceholder + sizeof(kCollectPlaceholder));
          return true;
        }
        if (!g_batch.done.empty())
        {
          auto it = g_batch.done.find(XXH3_64bits(source.data(), source.size()));
          if (it != g_batch.done.end())
          {
            out = it->second; // kept until batch::Clear (two shaders may share a source)
            return true;
          }
        }
        ++compiles;
        return CompileWith(compiler, source, out, errors);
      }
    };
    Compiler g_compiler;

    std::string FindHlsl(const NativeShader *ps)
    {
      const std::string name = fmt::format("{:016x}.hlsl", ps->hash);
      for (const std::string &d : {ps->source, ShaderCache::Get().directory(), ShaderCache::Get().fallback_directory()})
      {
        if (!d.empty() && FileExists(d + "\\" + name))
        {
          return d + "\\" + name;
        }
      }
      return {};
    }
  } // namespace

  uint32_t CurrentMode() { return uint32_t(std::clamp(REXCVAR_GET(pdz_native_depth20e4), 0, 2)); }

  void Constants(const RenderState &rs, bool polygonal, uint32_t resolution_scale, float out[4])
  {
    reg::PA_SU_SC_MODE_CNTL sc;
    sc.value = rs.su_sc_mode_cntl;
    float front_scale = 0.0f, front_offset = 0.0f, back_scale = 0.0f, back_offset = 0.0f;
    if (polygonal)
    {
      if (sc.poly_offset_front_enable)
      {
        front_scale = rs.poly_offset_front_scale;
        front_offset = rs.poly_offset_front_offset;
      }
      if (sc.poly_offset_back_enable)
      {
        back_scale = rs.poly_offset_back_scale;
        back_offset = rs.poly_offset_back_offset;
      }
    }
    else if (sc.poly_offset_para_enable)
    {
      front_scale = back_scale = rs.poly_offset_front_scale;
      front_offset = back_offset = rs.poly_offset_front_offset;
    }
    // xenos::kPolygonOffsetScaleSubpixelUnit (1/16) times the resolution scale: the derivatives are per host pixel.
    const float scale_factor = (1.0f / 16.0f) * float(resolution_scale ? resolution_scale : 1);
    out[0] = front_scale * scale_factor;
    out[1] = front_offset;
    out[2] = back_scale * scale_factor;
    out[3] = back_offset;
  }

  uint32_t EdgeFor(const RenderState &rs, bool polygonal)
  {
    if (!REXCVAR_GET(pdz_native_depth20e4_edge) || CurrentMode() == kOff)
    {
      return 0;
    }
    reg::RB_DEPTHCONTROL dc;
    dc.value = rs.depthcontrol;
    if (!dc.z_enable || dc.z_write_enable)
    {
      return 0; // a depth write would store the edge instead of the 20e4 number
    }
    float c[4];
    Constants(rs, polygonal, 1, c);
    if (c[0] == 0.0f && c[1] == 0.0f && c[2] == 0.0f && c[3] == 0.0f)
    {
      return 0; // no polygon offset: in mode 2 both sides are 20e4 already, in mode 1 there is no variant
    }
    switch (dc.zfunc)
    {
    case xenos::CompareFunction::kGreaterEqual:
    case xenos::CompareFunction::kLess:
      return 1;
    case xenos::CompareFunction::kLessEqual:
    case xenos::CompareFunction::kGreater:
      return 2;
    default:
      return 0; // EQUAL / NOT_EQUAL need both edges; NEVER / ALWAYS do not compare
    }
  }

  bool Wanted(const NativeShader *ps, const RenderState &rs, const TargetFormats &tf, bool polygonal)
  {
    const uint32_t mode = CurrentMode();
    if (mode == kOff || !ps || tf.ds == DXGI_FORMAT_UNKNOWN || !tf.depth_float24)
    {
      return false;
    }
    reg::RB_DEPTHCONTROL dc;
    dc.value = rs.depthcontrol;
    // draw_util::GetNormalizedDepthControl: an always-passing test without writes is no depth at all.
    if (!dc.z_enable || (!dc.z_write_enable && dc.zfunc == xenos::CompareFunction::kAlways))
    {
      return false;
    }
    if (mode == kAll)
    {
      return true;
    }
    float c[4];
    Constants(rs, polygonal, 1, c);
    return c[0] != 0.0f || c[1] != 0.0f || c[2] != 0.0f || c[3] != 0.0f;
  }

  const std::vector<uint8_t> *Variant(const NativeShader *ps, uint32_t edge)
  {
    if (!ps || edge > 2)
    {
      return nullptr;
    }
    const bool depth_only = ps->dxil.empty();
    const uint64_t key = depth_only ? 0 : ps->hash;
    std::unordered_map<uint64_t, Entry> &variants = g_variants[edge];
    auto it = variants.find(key);
    if (it != variants.end())
    {
      return it->second.failed ? nullptr : &it->second.dxil;
    }
    Entry &e = variants[key];
    e.failed = true;
    std::string why, source;
    if (depth_only)
    {
      source = DepthOnlySource(edge);
    }
    else if (!ps->coissue_hlsl.empty())
    {
      // The co-issue fix recompiled this PS (native_coissue.h): the variant starts from the patched source.
      source = BuildVariantSource(ps->coissue_hlsl, why, edge);
    }
    else
    {
      const std::string path = FindHlsl(ps);
      std::string hlsl;
      if (path.empty() || !ReadText(path, hlsl))
      {
        why = "no .hlsl next to the shader";
      }
      else
      {
        source = BuildVariantSource(hlsl, why, edge);
      }
    }
    // On-disk cache: <shader dir>\depth20e4\<ps>_<hash of the whole variant source>.dxil. Keyed by the source text,
    // so a changed .hlsl or a changed epilogue never reuses a stale file. Saves the compile (and loading DXC) on
    // later runs.
    std::string cache_path;
    bool from_disk = false;
    if (!source.empty())
    {
      cache_path = fmt::format("{}\\depth20e4\\{:016x}_{:016x}.dxil", ShaderCache::Get().directory(), key,
                               XXH3_64bits(source.data(), source.size()));
      from_disk = ReadBinary(cache_path, e.dxil) && IsSignedContainer(e.dxil);
      if (!from_disk)
      {
        e.dxil.clear();
      }
    }
    std::string errors;
    const auto t0 = std::chrono::steady_clock::now();
    if (!source.empty() && !from_disk)
    {
      if (g_stats.compiler_failed || !g_compiler.Init())
      {
        why = "no shader compiler (see the earlier warning)";
      }
      else if (!g_compiler.Compile(source, e.dxil, errors))
      {
        why = "compile: " + errors.substr(0, 600);
      }
      else if (!g_batch.collecting) // the collect pass's placeholder never goes to disk
      {
        SaveCacheFile(cache_path, e.dxil);
      }
    }
    const uint32_t ms =
        uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
    g_stats.compile_ms += ms;
    if (source.empty() || !why.empty())
    {
      ++g_stats.failed;
      e.dxil.clear();
      if (g_stats.failed <= kLogBuilds && !g_batch.collecting)
      {
        REXLOG_WARN("pdz_native: depth20e4: no variant for ps {:016x}: {} (the draws keep the fixed-function bias){}",
                    key, why, g_stats.failed == kLogBuilds ? "; further failures only in the periodic counters" : "");
      }
      return nullptr;
    }
    e.failed = false;
    ++g_stats.built;
    if (edge)
    {
      ++g_stats.edge_built;
    }
    if (from_disk)
    {
      ++g_stats.from_disk;
    }
    if (!from_disk && g_stats.built > kLogBuilds && REXCVAR_GET(pdz_native_first_use_log) &&
        g_first_use_logged < kFirstUseLogLines && !g_batch.collecting)
    {
      // Not in the disk cache: compiled at the draw's first use (the cost a stutter hunt looks for).
      ++g_first_use_logged;
      REXLOG_INFO("pdz_native: depth20e4: variant for ps {:016x}{} compiled at first use in {} ms (not in the disk "
                  "cache){}",
                  key, edge == 1 ? " (compare edge: upper)" : (edge == 2 ? " (compare edge: lower)" : ""), ms,
                  g_first_use_logged == kFirstUseLogLines ? "; further ones only in the periodic counters" : "");
    }
    if (g_stats.built <= kLogBuilds && !g_batch.collecting)
    {
      REXLOG_INFO("pdz_native: depth20e4: variant {} for ps {:016x}{}{} ({} bytes DXIL, {} in {} ms){}",
                  g_stats.built, key, depth_only ? " (depth-only)" : (ps->writes_depth ? " (writes depth)" : ""),
                  edge == 1 ? " (compare edge: upper)" : (edge == 2 ? " (compare edge: lower)" : ""),
                  e.dxil.size(), from_disk ? "read from the disk cache" : "compiled", ms,
                  g_stats.built == kLogBuilds ? "; further builds only in the periodic counters" : "");
    }
    return &e.dxil;
  }

  void MarkFailed(const NativeShader *ps, const char *why, uint32_t edge)
  {
    if (!ps || edge > 2)
    {
      return;
    }
    const uint64_t key = ps->dxil.empty() ? 0 : ps->hash;
    Entry &e = g_variants[edge][key];
    if (e.failed)
    {
      return;
    }
    e.failed = true;
    e.dxil.clear();
    if (g_stats.built)
    {
      --g_stats.built;
    }
    if (edge && g_stats.edge_built)
    {
      --g_stats.edge_built;
    }
    ++g_stats.failed;
    REXLOG_WARN("pdz_native: depth20e4: variant for ps {:016x} retired: {} (the draws keep the fixed-function bias)",
                key, why);
  }

  bool CompilePixelShader(const std::string &source, std::vector<uint8_t> &dxil, std::string &errors)
  {
    dxil.clear();
    if (g_stats.compiler_failed || !g_compiler.Init())
    {
      errors = "no shader compiler (dxcompiler.dll + dxil.dll, see pdz_native_dxc_dir)";
      return false;
    }
    return g_compiler.Compile(source, dxil, errors);
  }

  bool IsSignedDxil(const std::vector<uint8_t> &dxil) { return IsSignedContainer(dxil); }

  uint32_t CompileCount() { return g_compiler.compiles; }

  void SaveCacheFile(const std::string &path, const std::vector<uint8_t> &bytes)
  {
    if (!g_batch.done.empty())
    {
      g_batch.writes.emplace_back(path, bytes); // the pipeline list's pass: written on several threads at Clear
      return;
    }
    std::error_code ec;
    std::filesystem::create_directories(DirOf(path), ec);
    std::ofstream f(path, std::ios::binary);
    if (f)
    {
      f.write(reinterpret_cast<const char *>(bytes.data()), std::streamsize(bytes.size()));
    }
  }

  namespace batch
  {
    bool BeginCollect()
    {
      if (g_stats.compiler_failed || !g_compiler.Init())
      {
        return false;
      }
      g_batch.collecting = true;
      return true;
    }

    void EndCollect()
    {
      g_batch.collecting = false;
      // What the collect pass built holds placeholders: forgotten, with its counters.
      for (auto &m : g_variants)
      {
        m.clear();
      }
      const bool compiler_failed = g_stats.compiler_failed;
      g_stats = Stats();
      g_stats.compiler_failed = compiler_failed;
      g_first_use_logged = 0;
    }

    bool Collecting() { return g_batch.collecting; }

    Result CompileCollected(uint32_t threads)
    {
      Result r;
      const auto t0 = std::chrono::steady_clock::now();
      std::vector<std::pair<uint64_t, const std::string *>> work;
      work.reserve(g_batch.sources.size());
      for (const auto &[h, src] : g_batch.sources)
      {
        if (!g_batch.done.count(h))
        {
          work.emplace_back(h, &src);
        }
      }
      r.sources = uint32_t(work.size());
      // The longest first, so the last worker doesn't finish alone on a big one.
      std::sort(work.begin(), work.end(), [](const auto &a, const auto &b) { return a.second->size() > b.second->size(); });
      std::vector<std::vector<uint8_t>> out(work.size());
      std::vector<uint8_t> ok(work.size(), 0);
      threads = std::clamp<uint32_t>(threads, 1u, 32u);
      threads = std::min<uint32_t>(threads, std::max<uint32_t>(1u, uint32_t(work.size())));
      r.threads = threads;
      std::atomic<size_t> next{0};
      auto create = reinterpret_cast<DxcCreateInstanceProc>(dxc::CreateProc());
      std::vector<std::thread> pool;
      for (uint32_t t = 0; t < threads; ++t)
      {
        pool.emplace_back([&] {
          IDxcCompiler3 *c = nullptr;
          if (!create || FAILED(create(CLSID_DxcCompiler, __uuidof(IDxcCompiler3), reinterpret_cast<void **>(&c))) || !c)
          {
            return; // the others take its share
          }
          for (size_t i; (i = next++) < work.size();)
          {
            std::string errors;
            ok[i] = CompileWith(c, *work[i].second, out[i], errors) ? 1 : 0;
          }
          c->Release();
        });
      }
      for (std::thread &t : pool)
      {
        t.join();
      }
      for (size_t i = 0; i < work.size(); ++i)
      {
        if (ok[i])
        {
          g_batch.done.emplace(work[i].first, std::move(out[i]));
          ++r.compiled;
        }
        else
        {
          ++r.failed; // the real pass compiles it again and logs why
        }
      }
      g_batch.sources.clear();
      r.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
      return r;
    }

    void Clear()
    {
      g_batch.sources.clear();
      g_batch.done.clear();
      // The cache files the real pass made, on a few threads (each new file is a create + write the OS and the virus
      // scanner see one at a time otherwise: about 1 ms each, 1,300 of them at a first start).
      std::vector<std::pair<std::string, std::vector<uint8_t>>> writes;
      writes.swap(g_batch.writes);
      std::atomic<size_t> next{0};
      std::vector<std::thread> pool;
      const unsigned threads = std::clamp(std::thread::hardware_concurrency() / 2, 1u, 8u);
      for (unsigned t = 0; t < threads && t < writes.size(); ++t)
      {
        pool.emplace_back([&] {
          for (size_t i; (i = next++) < writes.size();)
          {
            std::error_code ec;
            std::filesystem::create_directories(DirOf(writes[i].first), ec);
            std::ofstream f(writes[i].first, std::ios::binary);
            if (f)
            {
              f.write(reinterpret_cast<const char *>(writes[i].second.data()), std::streamsize(writes[i].second.size()));
            }
          }
        });
      }
      for (std::thread &t : pool)
      {
        t.join();
      }
    }
  } // namespace batch

  void PreloadCompiler()
  {
    static bool started = false;
    if (started || !REXCVAR_GET(pdz_native_dxc_preload))
    {
      return;
    }
    started = true;
    // Detached: Init holds the compiler's mutex, so a first compile meanwhile waits for it instead of loading twice.
    std::thread([]() { g_compiler.Init(true); }).detach();
  }

  const Stats &GetStats() { return g_stats; }

  void CountDraw() { ++g_stats.draws; }

  std::string StatsText()
  {
    std::string text = fmt::format("depth20e4 mode {}: variants {} ({} from disk, failed {}, {} ms) draws {}",
                                   CurrentMode(), g_stats.built, g_stats.from_disk, g_stats.failed, g_stats.compile_ms,
                                   g_stats.draws);
    if (REXCVAR_GET(pdz_native_depth20e4_edge) || g_stats.edge_built)
    {
      text += fmt::format(" (compare-edge variants {})", g_stats.edge_built);
    }
    return text;
  }
} // namespace pdz::native::depth20e4
