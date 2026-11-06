// PDZ Restored: the first start's pipeline pass (2026-11-03, docs/RELEASE_FIRST_RUN.md "First-play pipelines").
//
// A release ships no PSO cache: each pipeline used to be compiled by the driver at its first draw, a stall of up to
// ~70 ms each in a player's first visit to a level. The exe carries a pipeline list (data/native_pipeline_list.txt,
// tools/shaders/make_pipeline_list.py: hashes and pipeline state only, no game code). Once, after the shaders are
// made from the player's game files and before any draw (OnPostLoadXexImage), this pass:
//   1. loads the list's shaders from the player's shader folder;
//   2. runs each pixel shader's first-draw patch check (coissue::Apply: co-issue, PsParamGen, live literals ...) and
//      builds the depth20e4 / blend7e3 variants the list names, first as a collect pass on copies (the sources only),
//      then with every source compiled on all cores at once, then for real (taking the compiled DXIL, saving the
//      patch caches on disk as the draws would);
//   3. writes the list's recipes with that DXIL to <native cache>\pso_seed.bin.
// The PSO store merges pso_seed.bin into its recipes, and the startup warm-up (pdz_native_pso_prewarm) builds every
// pipeline while the intros and the front end run; a level started before it is done waits for it at its start
// (pdz_native_pso_level_wait_ms) instead of compiling mid-play.
#include "native_pipeline.h"
#include "native_blend7e3.h"
#include "native_coissue.h"
#include "native_depth20e4.h"
#include "native_dxc.h"
#include "native_shaders.h"
#include "../../../setup/setup_window.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <thread>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>
#include <fmt/xchar.h>
#include <rex/cvar.h>
#include <rex/hash.h>
#include <rex/logging.h>

REXCVAR_DEFINE_STRING(pdz_native_pso_seed, "auto", "Render",
                      "Native renderer: the shipped pipeline list (data/native_pipeline_list.txt, in the exe) prepared "
                      "once, before the first draw: its shaders' patches and variants compiled on all cores and its "
                      "pipelines handed to the startup warm-up, so a first visit to a level compiles no pipeline in "
                      "play. auto = in a release, when the list, the shader folder or its generated shaders changed "
                      "since it last ran (pso_seed.stamp in the native cache); force = every start; off = never");
REXCVAR_DEFINE_BOOL(pdz_native_shader_preload, true, "Render",
                    "Native renderer: the pipeline list's shaders (its 508 VS and 998 PS, their patch check and depth20e4 "
                    "/ blend7e3 variants, from the disk caches the first start made) loaded a little each frame (about "
                    "1 ms) from the session's first frames through the front end and the loading, instead of at a "
                    "level's first draws (555 VS bindings and 138 pixel shaders in Datacore's first seconds, 106 ms). "
                    "One 'pdz_native: shader preload' line when done. Off = at first use, as before");
REXCVAR_DECLARE(bool, pdz_native_renderer);
REXCVAR_DECLARE(bool, pdz_native_pso_cache);
REXCVAR_DECLARE(bool, pdz_setup_quiet);

namespace pdz::native::pipeline_seed
{
  namespace
  {
    namespace fs = std::filesystem;

    bool ReadAll(const fs::path &p, std::string &out)
    {
      std::ifstream f(p, std::ios::binary);
      if (!f)
      {
        return false;
      }
      out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
      return true;
    }

    // What the pass's output depends on: the list, the shader folder and the shaders made there (generated.json).
    std::string StampText()
    {
      std::string generated;
      const std::string dir = ShaderCache::Get().directory();
      ReadAll(fs::path(dir) / "generated.json", generated);
      return fmt::format("list {:016x}\nshaders {}\ngenerated {:016x}\n", ListHash(), dir,
                         XXH3_64bits(generated.data(), generated.size()));
    }

    struct PsWork
    {
      bool edge[3] = {};  // depth20e4 variants (key.depth_variant 1-3 = edge 0-2)
      bool mode[3] = {};  // blend7e3 variants (mode 1, 2)
    };

    double MsSince(std::chrono::steady_clock::time_point t0)
    {
      return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    }
  } // namespace

  void PreloadStep(double budget_ms)
  {
    struct Item
    {
      uint64_t hash = 0;
      bool vertex = false;
      PsWork w;
    };
    static std::vector<Item> items;
    static size_t cursor = 0;
    static bool built = false, done = false;
    static double spent_ms = 0.0;
    static uint32_t calls = 0, vs_loaded = 0, ps_loaded = 0, variants = 0;
    if (done || !REXCVAR_GET(pdz_native_shader_preload))
    {
      return;
    }
    const auto t0 = std::chrono::steady_clock::now();
    if (!built)
    {
      built = true;
      std::set<uint64_t> vs_set;
      std::map<uint64_t, PsWork> ps_work;
      for (const Query &q : ListQueries())
      {
        vs_set.insert(q.vs);
        PsWork &w = ps_work[q.ps];
        if (q.depth_variant >= 1 && q.depth_variant <= 3)
        {
          w.edge[q.depth_variant - 1] = true;
        }
        if (q.blend7e3_variant == blend7e3::kAlphaClamp || q.blend7e3_variant == blend7e3::kRov)
        {
          w.mode[q.blend7e3_variant] = true;
        }
      }
      for (const uint64_t vs : vs_set)
      {
        items.push_back({vs, true, {}});
      }
      for (const auto &[ps, w] : ps_work)
      {
        items.push_back({ps, false, w});
      }
    }
    ShaderCache &sc = ShaderCache::Get();
    ++calls;
    while (cursor < items.size() && MsSince(t0) < budget_ms)
    {
      const Item &it = items[cursor++];
      if (it.vertex)
      {
        const NativeShader *v = sc.Load(it.hash, true);
        vs_loaded += v && v->usable;
        continue;
      }
      const NativeShader *p = it.hash ? sc.PreparePixel(it.hash) : sc.NullPixel();
      if (!p)
      {
        continue;
      }
      ++ps_loaded;
      if (it.hash && !p->coissue_checked)
      {
        continue; // its patch check waits for the first draw (no microcode file): so do its variants
      }
      for (uint32_t e = 0; e < 3; ++e)
      {
        if (it.w.edge[e])
        {
          variants += depth20e4::Variant(p, e) != nullptr;
        }
      }
      for (uint32_t m = 1; m < 3; ++m)
      {
        if (it.w.mode[m] && it.hash)
        {
          variants += blend7e3::Variant(p, m) != nullptr;
        }
      }
    }
    spent_ms += MsSince(t0);
    if (cursor >= items.size())
    {
      done = true;
      std::vector<Item>().swap(items);
      REXLOG_INFO("pdz_native: shader preload: {} VS, {} PS and {} variants of the pipeline list loaded in {:.0f} ms over "
                  "{} steps (pdz_native_shader_preload)",
                  vs_loaded, ps_loaded, variants, spent_ms, calls);
    }
  }

  void PrepareAtFirstStart()
  {
    const std::string mode = REXCVAR_GET(pdz_native_pso_seed);
    if (mode == "off" || !REXCVAR_GET(pdz_native_renderer) || !REXCVAR_GET(pdz_native_pso_cache))
    {
      return;
    }
    if (mode != "force" && DeveloperBuild())
    {
      return; // a developer's build has its own recipes from play
    }
    const fs::path cache = NativeCacheDir();
    const fs::path stamp_path = cache / "pso_seed.stamp";
    const std::string stamp = StampText();
    std::string old;
    if (mode != "force" && ReadAll(stamp_path, old) && old == stamp)
    {
      return;
    }
    const auto t0 = std::chrono::steady_clock::now();
    const std::vector<Query> queries = ListQueries();
    std::set<uint64_t> vs_set;
    std::map<uint64_t, PsWork> ps_work;
    for (const Query &q : queries)
    {
      vs_set.insert(q.vs);
      PsWork &w = ps_work[q.ps];
      if (q.depth_variant >= 1 && q.depth_variant <= 3)
      {
        w.edge[q.depth_variant - 1] = true;
      }
      if (q.blend7e3_variant == blend7e3::kAlphaClamp || q.blend7e3_variant == blend7e3::kRov)
      {
        w.mode[q.blend7e3_variant] = true;
      }
    }
    ShaderCache &sc = ShaderCache::Get();
    const uint32_t threads = std::clamp(std::thread::hardware_concurrency(), 1u, 16u);
    depth20e4::batch::Result batch;
    bool collected = false;
    uint32_t vs_missing = 0, ps_missing = 0;
    double load_ms = 0.0, collect_ms = 0.0, final_ms = 0.0;
    std::unordered_map<uint64_t, const NativeShader *> ps_ready;
    std::map<std::pair<uint64_t, uint32_t>, const std::vector<uint8_t> *> depth_ready, blend_ready;
    Report rep;

    setup_ui::State state;
    const auto work = [&](setup_ui::State &s) {
      const auto say = [&](const std::wstring &line, double progress) {
        std::lock_guard lock(s.mutex);
        s.line1 = line;
        s.progress = progress;
      };
      {
        std::lock_guard lock(s.mutex);
        s.line2 = L"First start only: the game's pipelines are built now, so play doesn't stutter.";
      }
      // 1. The shaders.
      say(L"Preparing graphics: loading the shaders", 0.02);
      auto t = std::chrono::steady_clock::now();
      for (const uint64_t vs : vs_set)
      {
        const NativeShader *v = sc.Load(vs, true);
        vs_missing += !v || !v->usable;
      }
      load_ms = MsSince(t);
      // 2a. Collect: the patch and variant sources, on copies (nothing kept, nothing logged).
      say(L"Preparing graphics: the shader patches", 0.08);
      t = std::chrono::steady_clock::now();
      if (depth20e4::batch::BeginCollect())
      {
        collected = true;
        for (const auto &[ps, w] : ps_work)
        {
          NativeShader copy;
          const NativeShader *p = ps ? sc.PreparePixel(ps, &copy) : sc.NullPixel();
          if (!p)
          {
            continue;
          }
          for (uint32_t e = 0; e < 3; ++e)
          {
            if (w.edge[e])
            {
              depth20e4::Variant(p, e);
            }
          }
          for (uint32_t m = 1; m < 3; ++m)
          {
            if (w.mode[m] && ps)
            {
              blend7e3::Variant(p, m);
            }
          }
        }
        depth20e4::batch::EndCollect();
        coissue::ResetForSeed();
        blend7e3::ResetForSeed();
        collect_ms = MsSince(t);
        // 2b. Every source at once, on all cores.
        say(L"Preparing graphics: compiling the shader patches", 0.15);
        batch = depth20e4::batch::CompileCollected(threads);
      }
      // 2c. For real: the cached shaders patched (the compiles above taken), the variants built and kept.
      say(L"Preparing graphics: the pipelines", 0.85);
      t = std::chrono::steady_clock::now();
      for (const auto &[ps, w] : ps_work)
      {
        const NativeShader *p = ps ? sc.PreparePixel(ps) : sc.NullPixel();
        if (!p)
        {
          ++ps_missing;
          continue;
        }
        ps_ready[ps] = p;
        for (uint32_t e = 0; e < 3; ++e)
        {
          if (w.edge[e])
          {
            depth_ready[{ps, e + 1}] = depth20e4::Variant(p, e);
          }
        }
        for (uint32_t m = 1; m < 3; ++m)
        {
          if (w.mode[m] && ps)
          {
            blend_ready[{ps, m}] = blend7e3::Variant(p, m);
          }
        }
      }
      depth20e4::batch::Clear();
      // 3. The recipes.
      rep = WriteRecipes((cache / "pso_seed.bin").string(), [&](const Query &q) {
        Blobs b;
        const NativeShader *v = sc.Load(q.vs, true);
        if (v && v->usable && !v->dxil.empty())
        {
          b.vs = &v->dxil;
        }
        auto p = ps_ready.find(q.ps);
        if (p != ps_ready.end() && p->second)
        {
          b.ps = &p->second->dxil; // empty for the null pixel shader (depth only): no blob, as at the draw
        }
        if (q.depth_variant)
        {
          auto it = depth_ready.find({q.ps, q.depth_variant});
          b.variant = it != depth_ready.end() ? it->second : nullptr;
        }
        else if (q.blend7e3_variant)
        {
          auto it = blend_ready.find({q.ps, q.blend7e3_variant});
          b.variant = it != blend_ready.end() ? it->second : nullptr;
        }
        return b;
      });
      final_ms = MsSince(t);
      say(L"Preparing graphics: done", 1.0);
    };
    // Behind the setup's progress window (a quiet test run: the same work, no window). Without a window it still runs.
    if (REXCVAR_GET(pdz_setup_quiet))
    {
      work(state);
    }
    else
    {
      setup_ui::RunWithProgress(L"PDZ Restored: preparing the renderer", state, work);
    }
    if (rep.written)
    {
      std::error_code ec;
      fs::create_directories(cache, ec);
      std::ofstream(stamp_path, std::ios::binary).write(stamp.data(), std::streamsize(stamp.size()));
    }
    REXLOG_INFO("pdz_native: pipeline list: {} pipelines ({} VS, {} PS; {} VS / {} PS not loadable): {} written to "
                "pso_seed.bin for the warm-up ({} left out: a shader or variant missing; {} built from other DXIL than "
                "the developer's); patches and variants: {} sources compiled on {} threads in {:.0f} ms ({} failed, "
                "compiled again at their first draw){}; shaders loaded in {:.0f} ms, collect {:.0f} ms, final pass "
                "{:.0f} ms; {:.1f} s in all",
                rep.listed, vs_set.size(), ps_work.size(), vs_missing, ps_missing, rep.written, rep.no_shader,
                rep.other_dxil, batch.compiled, batch.threads, batch.ms, batch.failed,
                collected ? "" : " (no shader compiler: none prepared)", load_ms, collect_ms, final_ms,
                MsSince(t0) / 1000.0);
  }
} // namespace pdz::native::pipeline_seed
