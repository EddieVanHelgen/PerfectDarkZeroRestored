// Native renderer: the shader cache made from the player's own game files (native_generate.h).
#include "native_generate.h"

#include "native_binder.h"
#include "native_dxc.h"
#include "xenosrecomp_pdz.h"
#include "../../../setup/setup_window.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>
#include <fmt/xchar.h>
#include <rex/cvar.h>
#include <rex/hash.h>
#include <rex/logging.h>

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_ONLY_PNG // only its zlib decoder is used
#include <stb_image.h>

REXCVAR_DEFINE_STRING(pdz_native_generate, "auto", "Render",
                      "Native renderer: make the translated shaders from the player's own game files (first start; "
                      "docs/RELEASE_FIRST_RUN.md). auto = in a release when generated.json in the shader folder doesn't "
                      "match the game files / exe / translator / DXC; force = now (a developer's build needs "
                      "pdz_native_shader_dir set: it never writes into the build's own folder); off = never.");

REXCVAR_DEFINE_BOOL(pdz_safe_mode, false, "Render",
                    "Safe mode: the emulated renderer (the GPU plugin, d3d12), locked at 60 fps, none of the native renderer. Also what the game falls back to when the native renderer can't start (its shaders couldn't be made).");
REXCVAR_DECLARE(std::string, pdz_native_shader_dir); // native_shaders.cpp

namespace pdz::native::generate
{
  namespace fs = std::filesystem;

  namespace
  {
    constexpr int kGeneratorVersion = 1;
    constexpr const char *kTranslatorId = "XenosRecomp-pdz b668b30";
    // The exe's raw microcode arrays (PDZRenderResearch\native\work\PART3_SHADERS.md section 5; harvest.py RAW_ARRAYS).
    struct RawArray
    {
      uint32_t address, dwords;
      bool pixel;
    };
    constexpr RawArray kRawArrays[] = {{0x820740F0, 6, true}, {0x82074108, 24, false}};

    struct Found
    {
      std::vector<uint8_t> input; // the container (static) or the microcode (raw)
      std::vector<uint8_t> ucode;
      bool pixel = false;
      bool raw = false;
      std::string source;
    };

    std::mutex g_mutex;
    std::unordered_map<uint64_t, Found> g_found; // phase 1's containers, by microcode hash (phase 2 checks against them)
    bool g_phase2 = false;                      // phase 1 ran (or generated.json lacks the exe phase)
    fs::path g_dir;
    std::string g_fingerprint;
    double g_phase1_seconds = 0.0;
    uint32_t g_phase1_shaders = 0, g_phase1_failed = 0;

    uint32_t Be32(const uint8_t *p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }

    std::string HashName(uint64_t h) { return fmt::format("{:016x}", h); }

    bool ReadAll(const fs::path &p, std::vector<uint8_t> &out)
    {
      std::ifstream f(p, std::ios::binary);
      if (!f)
      {
        return false;
      }
      out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
      return true;
    }

    bool WriteAll(const fs::path &p, const void *data, size_t size)
    {
      std::ofstream f(p, std::ios::binary | std::ios::trunc);
      return f && f.write(static_cast<const char *>(data), std::streamsize(size)) && f.flush();
    }

    // scan_packages.py inflate(): a CAFF blob as it is, else the zlib stream (every PDZ package is one).
    std::vector<uint8_t> Inflate(const std::vector<uint8_t> &raw)
    {
      if (raw.size() >= 4 && std::memcmp(raw.data(), "CAFF", 4) == 0)
      {
        return raw;
      }
      int out_len = 0;
      char *out = stbi_zlib_decode_malloc_guesssize_headerflag(reinterpret_cast<const char *>(raw.data()), int(raw.size()),
                                                               int(std::min<size_t>(raw.size() * 4, 256u << 20)), &out_len, 1);
      std::vector<uint8_t> d;
      if (out)
      {
        d.assign(reinterpret_cast<uint8_t *>(out), reinterpret_cast<uint8_t *>(out) + out_len);
        STBI_FREE(out);
      }
      return d;
    }

    // scan_packages.py find_shaders(): containers by magic 10 2A 0E 00/01, with its size checks. Calls
    // `found(offset, header_size, ucode_size, pixel)` for each, in offset order.
    template <typename F>
    void FindContainers(const uint8_t *d, size_t n, F &&found)
    {
      for (size_t o = 0; o + 4 <= n; ++o)
      {
        if (d[o] != 0x10 || d[o + 1] != 0x2A || d[o + 2] != 0x0E || d[o + 3] > 1)
        {
          continue;
        }
        if (o + 0x18 > n)
        {
          continue;
        }
        const uint32_t hdr = Be32(d + o + 4), us = Be32(d + o + 8);
        if (hdr < 0x18 || hdr > 0x20000 || us == 0 || us % 12 || us > 0x40000 || o + hdr + us > n)
        {
          continue;
        }
        found(o, hdr, us, d[o + 3] == 0);
      }
    }

    // harvest.py scan_blob(): the first container of each microcode hash wins.
    void ScanBlob(const uint8_t *d, size_t n, const std::string &ctx, std::unordered_map<uint64_t, Found> &out,
                  std::vector<uint64_t> &order)
    {
      FindContainers(d, n, [&](size_t o, uint32_t hdr, uint32_t us, bool pixel) {
        const uint8_t *uc = d + o + hdr;
        const uint64_t h = XXH3_64bits(uc, us);
        if (out.count(h))
        {
          return;
        }
        Found f;
        f.input.assign(d + o, d + o + hdr + us);
        f.ucode.assign(uc, uc + us);
        f.pixel = pixel;
        f.source = fmt::format("{}@0x{:X}", ctx, o);
        out.emplace(h, std::move(f));
        order.push_back(h);
      });
    }

    std::vector<fs::path> Packages(const fs::path &dir, bool recursive)
    {
      std::vector<fs::path> out;
      std::error_code ec;
      if (!fs::is_directory(dir, ec))
      {
        return out;
      }
      const auto take = [&](const fs::directory_entry &e) {
        if (e.is_regular_file(ec) && _wcsicmp(e.path().extension().c_str(), L".pkg") == 0)
        {
          out.push_back(e.path());
        }
      };
      if (recursive)
      {
        for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator();
             it.increment(ec))
        {
          take(*it);
        }
      }
      else
      {
        for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
        {
          take(*it);
        }
      }
      return out;
    }

    // The scan's inputs in harvest's order: retail packages by name; DLC then title update packages by path.
    std::vector<fs::path> AllPackages(const Inputs &in, size_t &retail)
    {
      std::vector<fs::path> pkgs = Packages(in.game_root / "pdz" / "ASSETS" / "PackageData", false);
      std::sort(pkgs.begin(), pkgs.end(), [](const fs::path &a, const fs::path &b) {
        return a.filename().string() < b.filename().string();
      });
      retail = pkgs.size();
      std::vector<fs::path> dlc = Packages(in.user_data_root / "0000000000000000", true);
      std::sort(dlc.begin(), dlc.end());
      std::vector<fs::path> upd = Packages(in.update_root, true);
      std::sort(upd.begin(), upd.end());
      pkgs.insert(pkgs.end(), dlc.begin(), dlc.end());
      pkgs.insert(pkgs.end(), upd.begin(), upd.end());
      return pkgs;
    }

    std::string FileStamp(const fs::path &p)
    {
      std::error_code ec;
      const auto size = fs::file_size(p, ec);
      const auto time = ec ? fs::file_time_type{} : fs::last_write_time(p, ec);
      return fmt::format("{}:{}", ec ? 0 : size, uint64_t(time.time_since_epoch().count()));
    }

    // What the shaders are made from: the package list (names, sizes, times), the exe build, the translator (with its
    // embedded include) and DXC.
    std::string Fingerprint(const Inputs &in, const std::vector<fs::path> &pkgs)
    {
      std::string all = fmt::format("gen {} | {} include {:016x} | exe {} | dxc {} {}", kGeneratorVersion, kTranslatorId,
                                    XXH3_64bits(xenosrecomp_pdz::CommonInclude().data(), xenosrecomp_pdz::CommonInclude().size()),
                                    in.exe_build, FileStamp(fs::path(dxc::LoadedFrom()) / "dxcompiler.dll"),
                                    FileStamp(fs::path(dxc::LoadedFrom()) / "dxil.dll"));
      for (const fs::path &p : pkgs)
      {
        all += "|" + p.filename().string() + ":" + FileStamp(p);
      }
      return fmt::format("{:016x}", XXH3_64bits(all.data(), all.size()));
    }

    std::string StampField(const std::string &json, const std::string &key)
    {
      const std::string k = "\"" + key + "\": \"";
      const size_t at = json.find(k);
      const size_t end = at == std::string::npos ? at : json.find('"', at + k.size());
      return end == std::string::npos ? std::string() : json.substr(at + k.size(), end - at - k.size());
    }

    void WriteStamp(bool exe_done, uint32_t shaders, uint32_t failed, double seconds)
    {
      const std::string text = fmt::format(
          "{{\n  \"note\": \"PDZ Restored: shaders translated on this PC from the player's own game files. Made again "
          "when the fingerprint changes.\",\n  \"generator\": \"{}\",\n  \"fingerprint\": \"{}\",\n  \"exe_phase\": \"{}\",\n"
          "  \"shaders\": \"{}\",\n  \"failed\": \"{}\",\n  \"seconds\": \"{:.1f}\"\n}}\n",
          kGeneratorVersion, g_fingerprint, exe_done ? 1 : 0, shaders, failed, seconds);
      WriteAll(g_dir / "generated.json", text.data(), text.size());
    }

    // Removes what an older generation (or play) left in our own shader folder: translations, microcode, containers,
    // the patched variants made from them. The run-time records stay.
    void ClearFolder(const fs::path &dir)
    {
      std::error_code ec;
      for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
      {
        const fs::path p = it->path();
        const std::wstring ext = p.extension().wstring();
        if (it->is_regular_file(ec) && (ext == L".hlsl" || ext == L".json" || ext == L".dxil" || ext == L".bin"))
        {
          fs::remove(p, ec);
        }
        else if (it->is_directory(ec))
        {
          const std::wstring name = p.filename().wstring();
          if (name == L"containers" || name == L"coissue" || name == L"blend7e3" || name == L"depth20e4" || name == L"misses")
          {
            fs::remove_all(p, ec);
          }
        }
      }
    }

    // data/native_bind_list.txt (tools/shaders/make_bind_list.py), embedded by CMake: per bound VS its parent and the
    // vertex declaration it was bound with. Hashes and formats only.
    const unsigned char kBindList[] = {
#include "native_bind_list.inc"
        0};

    struct BindRow
    {
      uint64_t bound = 0, parent = 0;
      uint32_t strides_bytes[16] = {};
      std::vector<binder::Element> elements;
    };

    uint64_t ParseHex(std::string_view v)
    {
      uint64_t h = 0;
      for (char c : v.substr(0, 16))
      {
        h = h * 16 + uint64_t(c >= 'a' ? c - 'a' + 10 : c >= 'A' ? c - 'A' + 10 : c - '0');
      }
      return h;
    }

    std::vector<uint32_t> Numbers(std::string_view v)
    {
      std::vector<uint32_t> out;
      uint32_t n = 0;
      bool any = false;
      for (char c : v)
      {
        if (c >= '0' && c <= '9')
        {
          n = n * 10 + uint32_t(c - '0');
          any = true;
        }
        else
        {
          if (any)
          {
            out.push_back(n);
          }
          n = 0;
          any = false;
        }
      }
      if (any)
      {
        out.push_back(n);
      }
      return out;
    }

    std::vector<BindRow> BindList()
    {
      std::vector<BindRow> rows;
      std::string_view text(reinterpret_cast<const char *>(kBindList));
      while (!text.empty())
      {
        const size_t eol = text.find('\n');
        std::string_view line = text.substr(0, eol);
        text = eol == std::string_view::npos ? std::string_view() : text.substr(eol + 1);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
        {
          line.remove_suffix(1);
        }
        if (line.empty() || line[0] == '#')
        {
          continue;
        }
        std::vector<std::string_view> f;
        for (size_t at = 0; at < line.size();)
        {
          const size_t sp = line.find(' ', at);
          f.push_back(line.substr(at, sp == std::string_view::npos ? std::string_view::npos : sp - at));
          at = sp == std::string_view::npos ? line.size() : sp + 1;
        }
        if (f.size() != 4)
        {
          continue;
        }
        BindRow r;
        r.bound = ParseHex(f[0]);
        r.parent = ParseHex(f[1]);
        const std::vector<uint32_t> st = Numbers(f[2]);
        for (size_t i = 0; i + 1 < st.size(); i += 2)
        {
          if (st[i] < 16)
          {
            r.strides_bytes[st[i]] = st[i + 1] * 4;
          }
        }
        const std::vector<uint32_t> el = Numbers(f[3]);
        for (size_t i = 0; i + 7 < el.size(); i += 8)
        {
          binder::Element e;
          e.usage = el[i];
          e.index = el[i + 1];
          e.stream = el[i + 2];
          e.offset_bytes = el[i + 3];
          e.fmt = el[i + 4];
          e.is_signed = el[i + 5];
          e.integer = el[i + 6];
          e.type_swz = el[i + 7];
          e.type = e.fmt | (e.is_signed << 8) | (e.integer << 9) | (e.type_swz << 10);
          r.elements.push_back(e);
        }
        rows.push_back(std::move(r));
      }
      return rows;
    }

    // binder::ReadTable on container bytes (the vertex element table: program info word 7 = count, words 11..).
    uint32_t TableOf(const std::vector<uint8_t> &c, binder::TableEntry *out, uint32_t max_out)
    {
      if (c.size() < 0x18)
      {
        return 0;
      }
      const uint32_t header = Be32(c.data() + 4), program = Be32(c.data() + 0x14);
      if (program == 0 || program + 12 * 4 > header || header > c.size())
      {
        return 0;
      }
      const uint32_t count = Be32(c.data() + program + 7 * 4);
      uint32_t n = 0;
      for (uint32_t i = 0; i < count && n < max_out; ++i)
      {
        const uint32_t at = program + (11 + i) * 4;
        if (at + 4 > header)
        {
          break;
        }
        const uint32_t raw = Be32(c.data() + at);
        out[n++] = {raw & 0xFFFu, (raw >> 12) & 0xFu, (raw >> 16) & 0xFu, (raw >> 20) & 0xFu};
      }
      return n;
    }

    struct Job
    {
      uint64_t hash;
      const Found *found;
    };

    // translate_pdz.py's one(): the container for a static shader, else the microcode with its type; the CLI's files.
    bool TranslateOne(const fs::path &dir, uint64_t hash, const Found &f)
    {
      const std::string base = (dir / HashName(hash)).string();
      const xenosrecomp_pdz::Result r =
          xenosrecomp_pdz::Translate(f.input.data(), f.input.size(),
                                     f.pixel ? xenosrecomp_pdz::Type::kPixel : xenosrecomp_pdz::Type::kVertex, true);
      bool ok = WriteAll(base + ".bin", f.ucode.data(), f.ucode.size());
      if (!f.raw)
      {
        ok = WriteAll(dir / "containers" / (HashName(hash) + ".bin"), f.input.data(), f.input.size()) && ok;
      }
      if (r.code == 1)
      {
        return false;
      }
      ok = WriteAll(base + ".hlsl", r.hlsl.data(), r.hlsl.size()) && WriteAll(base + ".json", r.json.data(), r.json.size()) && ok;
      if (r.dxil.empty())
      {
        return false;
      }
      return WriteAll(base + ".dxil", r.dxil.data(), r.dxil.size()) && ok;
    }

    // All jobs on a pool of workers; progress into `state` (if any). Returns the failures.
    uint32_t TranslateAll(const fs::path &dir, const std::vector<Job> &jobs, setup_ui::State *state, const std::wstring &what)
    {
      std::atomic<size_t> next{0}, done{0};
      std::atomic<uint32_t> failed{0};
      const unsigned workers = std::max(1u, std::thread::hardware_concurrency() > 2 ? std::thread::hardware_concurrency() - 1 : 1u);
      std::vector<std::thread> pool;
      for (unsigned w = 0; w < workers; ++w)
      {
        pool.emplace_back([&] {
          SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
          for (size_t i; (i = next++) < jobs.size();)
          {
            if (state && state->cancel)
            {
              return;
            }
            if (!TranslateOne(dir, jobs[i].hash, *jobs[i].found))
            {
              ++failed;
              REXLOG_WARN("pdz_native: generate: {} {} ({}) failed to translate", jobs[i].found->pixel ? "ps" : "vs",
                          HashName(jobs[i].hash), jobs[i].found->source);
            }
            ++done;
          }
        });
      }
      while (state && done < jobs.size() && !state->cancel)
      {
        {
          std::lock_guard lock(state->mutex);
          state->line1 = fmt::format(L"{}: {} of {} shaders", what, done.load(), jobs.size());
          state->progress = jobs.empty() ? 1.0 : 0.5 + 0.5 * double(done) / double(jobs.size());
        }
        Sleep(50);
      }
      for (std::thread &t : pool)
      {
        t.join();
      }
      return failed;
    }
  } // namespace

  bool PreparePackages(const Inputs &in, std::string &why)
  {
    const std::string mode = REXCVAR_GET(pdz_native_generate);
    if (mode == "off" || (mode != "force" && DeveloperBuild()))
    {
      return true; // a developer's build reads the research cache and its own folder
    }
    const std::string cvar_dir = REXCVAR_GET(pdz_native_shader_dir);
    if (DeveloperBuild() && cvar_dir.empty())
    {
      REXLOG_WARN("pdz_native: generate: force in a developer's build needs pdz_native_shader_dir (never the build's "
                  "own folder): skipped");
      return true;
    }
    g_dir = !cvar_dir.empty() ? fs::path(cvar_dir) : fs::path(CacheRoot()) / "native_shaders";
    if (!dxc::CreateProc())
    {
      why = "the shader compiler (dxcompiler.dll + dxil.dll next to PDZRestored.exe) couldn't be loaded";
      return false;
    }
    xenosrecomp_pdz::SetDxcCreate(dxc::CreateProc());
    size_t retail = 0;
    const std::vector<fs::path> pkgs = AllPackages(in, retail);
    g_fingerprint = Fingerprint(in, pkgs);
    std::string old;
    {
      std::vector<uint8_t> bytes;
      if (ReadAll(g_dir / "generated.json", bytes))
      {
        old.assign(bytes.begin(), bytes.end());
      }
    }
    if (mode != "force" && StampField(old, "fingerprint") == g_fingerprint)
    {
      g_phase2 = StampField(old, "exe_phase") != "1";
      REXLOG_INFO("pdz_native: generate: the shaders in {} match the game files ({} shaders{})", g_dir.string(),
                  StampField(old, "shaders"), g_phase2 ? "; the exe's are made again" : "");
      return true;
    }
    if (retail == 0)
    {
      why = "no game packages found in " + (in.game_root / "pdz" / "ASSETS" / "PackageData").string();
      return false;
    }
    const auto t0 = std::chrono::steady_clock::now();
    std::error_code ec;
    fs::create_directories(g_dir / "containers", ec);
    ClearFolder(g_dir);
    fs::create_directories(g_dir / "containers", ec);
    REXLOG_INFO("pdz_native: generate: making the shaders from the game files ({}; {} packages: {} retail, {} DLC / title "
                "update) into {}",
                old.empty() ? "first start" : "the game files, exe, translator or DXC changed", pkgs.size(), retail,
                pkgs.size() - retail, g_dir.string());

    // 1. Scan (packages in parallel; the first-wins order is kept by merging in package order).
    std::vector<std::unordered_map<uint64_t, Found>> per_pkg(pkgs.size());
    std::vector<std::vector<uint64_t>> per_order(pkgs.size());
    setup_ui::State state;
    uint32_t failed = 0;
    std::vector<Job> jobs;
    setup_ui::RunWithProgress(L"PDZ Restored: preparing the renderer", state, [&](setup_ui::State &s) {
      {
        std::lock_guard lock(s.mutex);
        s.line2 = L"First start only: the shaders are made from your own game files.";
      }
      std::atomic<size_t> next{0}, done{0};
      std::vector<std::thread> pool;
      const unsigned workers = std::clamp(std::thread::hardware_concurrency() / 2, 1u, 8u); // memory: one package each
      for (unsigned w = 0; w < workers; ++w)
      {
        pool.emplace_back([&] {
          for (size_t i; (i = next++) < pkgs.size();)
          {
            if (s.cancel)
            {
              return;
            }
            std::vector<uint8_t> raw;
            if (ReadAll(pkgs[i], raw))
            {
              const std::vector<uint8_t> d = Inflate(raw);
              ScanBlob(d.data(), d.size(), pkgs[i].stem().string(), per_pkg[i], per_order[i]);
            }
            ++done;
          }
        });
      }
      while (done < pkgs.size() && !s.cancel)
      {
        {
          std::lock_guard lock(s.mutex);
          s.line1 = fmt::format(L"Reading the game files: {} of {} packages", done.load(), pkgs.size());
          s.progress = 0.5 * double(done) / double(pkgs.size());
        }
        Sleep(50);
      }
      for (std::thread &t : pool)
      {
        t.join();
      }
      if (s.cancel)
      {
        return;
      }
      std::lock_guard lock(g_mutex);
      g_found.clear();
      for (size_t i = 0; i < pkgs.size(); ++i)
      {
        for (uint64_t h : per_order[i])
        {
          if (!g_found.count(h))
          {
            g_found.emplace(h, std::move(per_pkg[i][h]));
            jobs.push_back({h, nullptr});
          }
        }
        per_pkg[i].clear();
      }
      for (Job &j : jobs)
      {
        j.found = &g_found.at(j.hash);
      }
      failed = TranslateAll(g_dir, jobs, &s, L"Translating the shaders");
    });
    if (state.cancel)
    {
      why = "cancelled";
      return false;
    }
    g_phase1_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    g_phase1_shaders = uint32_t(jobs.size());
    g_phase1_failed = failed;
    WriteStamp(false, g_phase1_shaders, failed, g_phase1_seconds);
    REXLOG_INFO("pdz_native: generate: packages: {} shaders ({} ps, {} vs) translated in {:.1f} s, {} failed",
                jobs.size(), std::count_if(jobs.begin(), jobs.end(), [](const Job &j) { return j.found->pixel; }),
                std::count_if(jobs.begin(), jobs.end(), [](const Job &j) { return !j.found->pixel; }), g_phase1_seconds,
                failed);
    if (jobs.empty() || failed * 200 > jobs.size()) // none, or over 0.5 % failed
    {
      why = fmt::format("{} of {} shaders failed to translate", failed, jobs.size());
      return false;
    }
    g_phase2 = true;
    return true;
  }

  void PrepareExe(const uint8_t *membase, uint32_t image_base, uint32_t image_size)
  {
    if (!g_phase2 || !membase || !image_size)
    {
      return;
    }
    g_phase2 = false;
    const auto t0 = std::chrono::steady_clock::now();
    const uint8_t *img = membase + image_base;
    // harvest.py: the exe's containers come first in its order, so one of them wins over a package's copy of the same
    // microcode; a raw array only counts when no container holds it.
    std::unordered_map<uint64_t, Found> exe;
    std::vector<uint64_t> order;
    ScanBlob(img, image_size, "exe", exe, order);
    std::vector<Job> jobs;
    std::lock_guard lock(g_mutex);
    uint32_t replaced = 0, same = 0;
    for (uint64_t h : order)
    {
      Found &f = exe.at(h);
      if (const auto pkg = g_found.find(h); pkg != g_found.end())
      {
        if (pkg->second.input == f.input)
        {
          ++same;
          continue;
        }
        ++replaced;
      }
      jobs.push_back({h, &f});
    }
    std::unordered_map<uint64_t, Found> raw;
    for (const RawArray &a : kRawArrays)
    {
      if (a.address < image_base || a.address + a.dwords * 4 > image_base + image_size)
      {
        continue;
      }
      const uint8_t *uc = membase + a.address;
      const uint64_t h = XXH3_64bits(uc, a.dwords * 4);
      if (exe.count(h) || g_found.count(h))
      {
        continue;
      }
      Found f;
      f.input.assign(uc, uc + a.dwords * 4);
      f.ucode = f.input;
      f.pixel = a.pixel;
      f.raw = true;
      f.source = fmt::format("exe-raw@0x{:08X}", a.address);
      raw.emplace(h, std::move(f));
    }
    for (auto &[h, f] : raw)
    {
      jobs.push_back({h, &f});
    }
    // The bound VS of the bind list, from the player's own parent containers (the exe's win, as in the harvest).
    std::unordered_map<uint64_t, Found> bound;
    std::string manifest = "{\n  \"note\": \"PDZ Restored: bound vertex shaders made on this PC (bind list + binder::Bind).\",\n"
                           "  \"shaders\": [\n";
    uint32_t no_parent = 0, mismatch = 0;
    const std::vector<BindRow> rows = BindList();
    for (const BindRow &r : rows)
    {
      const Found *parent = nullptr;
      if (const auto it = exe.find(r.parent); it != exe.end())
      {
        parent = &it->second;
      }
      else if (const auto it2 = g_found.find(r.parent); it2 != g_found.end())
      {
        parent = &it2->second;
      }
      if (!parent || parent->raw || parent->pixel)
      {
        ++no_parent; // a DLC pack's shader the player doesn't have, for instance
        continue;
      }
      binder::TableEntry table[64];
      const uint32_t n = TableOf(parent->input, table, 64);
      std::vector<uint8_t> out;
      std::string why;
      if (!binder::Bind(parent->ucode.data(), uint32_t(parent->ucode.size()), table, n, r.elements.data(),
                        uint32_t(r.elements.size()), r.strides_bytes, 16, out, &why) ||
          XXH3_64bits(out.data(), out.size()) != r.bound)
      {
        ++mismatch;
        REXLOG_WARN("pdz_native: generate: bound VS {} from {}: the binder doesn't make it ({})", HashName(r.bound),
                    HashName(r.parent), why.empty() ? std::string("other bytes") : why);
        continue;
      }
      Found f;
      f.input = out;
      f.ucode = std::move(out);
      f.raw = true;
      f.source = "bound from " + HashName(r.parent);
      bound.emplace(r.bound, std::move(f));
      manifest += fmt::format("{}    {{\"hash\": \"{}\", \"derived_from\": \"{}\"}}", bound.size() > 1 ? ",\n" : "",
                              HashName(r.bound), HashName(r.parent));
    }
    manifest += "\n  ]\n}\n";
    for (auto &[h, f] : bound)
    {
      jobs.push_back({h, &f});
    }
    WriteAll(g_dir / "manifest.json", manifest.data(), manifest.size());
    REXLOG_INFO("pdz_native: generate: bind list: {} rows, {} bound VS made, {} without their parent here, {} the binder "
                "didn't reproduce",
                rows.size(), bound.size(), no_parent, mismatch);
    const uint32_t failed = TranslateAll(g_dir, jobs, nullptr, L"");
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    REXLOG_INFO("pdz_native: generate: exe image: {} containers ({} also in a package: {} identical, {} replaced by "
                "the exe's), {} raw arrays, {} bound VS: {} translated in {:.1f} s, {} failed",
                order.size(), same + replaced, same, replaced, raw.size(), bound.size(), jobs.size(), secs, failed);
    WriteStamp(true, g_phase1_shaders + uint32_t(jobs.size()) - replaced, g_phase1_failed + failed, g_phase1_seconds + secs);
    g_found.clear();
  }
} // namespace pdz::native::generate
