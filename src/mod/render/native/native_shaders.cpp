// Native renderer (stage 1): shader cache (native_shaders.h). Cold path: file loads, sidecar parsing, miss
// dumps, coverage log. Hot path: two hash-map lookups per draw (pixel by hash, vertex by (object, decl)).
#include "native_shaders.h"
#include "../cpu_placement.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <fmt/format.h>
#include <rex/hash.h> // XXH3_64bits (xxhash.h, XXH_INLINE_ALL) - the SDK's shader key
#include <rex/logging.h>

#include "native.h" // PhysPtr, Enabled, guest helpers
#include "native_json.h"
#include "native_binder.h"
#include "native_coissue.h"
#include "native_depth20e4.h" // CompileCount: the first-use log tells a compile from a disk read
#include "native_view.h" // pdz_native_record_thread: BoundCopy reads the draw's device / program copies
#include "../frame_timing.h" // pdz_frame_timing_phases: shader.wait
#include "native_watchdog.h" // the synchronous translation's wait is a marked wait (pdz_native_translate_sync)
#include "native_dxc.h"      // paths (per-user cache / developer build) and the shared DXC
#include "xenosrecomp_pdz.h" // the XenosRecomp fork, in-process (thirdparty/XenosRecomp-pdz)

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <deque>
#include <mutex>
#include <thread>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

REXCVAR_DEFINE_STRING(pdz_native_shader_dir, "", "Render",
                      "Native renderer: directory of the translated shaders (<xxh3>.dxil + <xxh3>.json, "
                      "optional <xxh3>.hlsl and manifest.json). Empty = <cache root>\\native_shaders, the per-user cache "
                      "(a developer's build: <exe dir>\\native_shaders); a file missing there is also looked for in "
                      "pdz_native_shader_fallback_dir. Misses are dumped to <dir>\\misses.");
REXCVAR_DEFINE_STRING(pdz_native_shader_fallback_dir, "", "Render",
                      "Native renderer: the second shader directory, tried for a file pdz_native_shader_dir lacks (its "
                      "..\\ucode holds the microcode the sibling variants read). Empty = none in a release; in a "
                      "developer's build (the exe inside the repo) the research cache "
                      "C:\\Games\\PDZRenderResearch\\native\\cache\\hlsl. 'none' = no fallback (a cold or partial "
                      "cache test, stage1\\subagents\\m2_nohole.md).");
REXCVAR_DEFINE_BOOL(pdz_native_shader_file_warm, true, "Render",
                    "Native renderer: read every file of the shader folders once at startup on a background thread "
                    "(background I/O priority), so a level's first shader loads on the record thread hit the OS file "
                    "cache instead of the disk (log 318: 10-11 ms per cold file, a 1.4 s first-play freeze). Reads only. "
                    "Off = no warm-up")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_INT32(pdz_native_log_frames, 600, "Render",
                     "Native renderer: log the shader cache coverage (hits / misses / distinct) and the draw "
                     "counters every N frames (0 = only at exit)")
    .range(0, 1000000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_native_translate, true, "Render",
                    "Native renderer: translate a shader variant the cache does not hold, in-process with the XenosRecomp "
                    "fork built into the exe (2026-11-03; was XenosRecomp.exe as a child process): <hash>.bin -> "
                    ".hlsl / .json / .dxil in the shader dir, on worker threads, saved for every later start. Needs "
                    "dxcompiler.dll + dxil.dll (pdz_native_dxc_dir). Off = a missing shader stays a skipped draw.");
REXCVAR_DEFINE_INT32(pdz_native_translate_workers, 0, "Render",
                     "Native renderer: worker threads translating shaders in-process. 0 = automatic (a quarter of the "
                     "logical processors, 1-4).")
    .range(0, 16);
REXCVAR_DEFINE_INT32(pdz_native_translate_sync, -1, "Render",
                     "Native renderer: a draw whose shader variant is not in the cache waits for its translation "
                     "(bounded by pdz_native_translate_sync_ms and _frame_ms) instead of being skipped while the "
                     "translator runs. -1 = only with graphics_backend null (no emulator frame behind the native one, so "
                     "a skipped draw is a hole in the picture: one stall per new variant instead), 0 = never (the draw "
                     "is skipped until the files exist, as with the plugin), 1 = also with the GPU plugin (a test of "
                     "the stall under the auto-diff). With it, a new VS variant whose binder output exists is translated "
                     "at its first draw instead of waiting up to 8 draws for D3D's patched copy (the binder's output is "
                     "what a cached lookup takes then too). A wait that times out is not repeated for that shader: its "
                     "draws are skipped until the translation completes, as before.")
    .range(-1, 1)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(pdz_native_translate_sync_ms, 1500, "Render",
                     "Native renderer: longest wait for one shader translation with pdz_native_translate_sync (ms). "
                     "Translations took 31-110 ms in the regressions, one 1047 ms (MP Facility VS 871da7bd4e5bd131, log 352); "
                     "1500 stays under the -NullGpu runners' watchdog (2000 ms), which names the wait if it is longer.")
    .range(0, 60000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(pdz_native_translate_sync_frame_ms, 1500, "Render",
                     "Native renderer: most time one native frame spends waiting for synchronous translations (ms; a "
                     "level that brings many new variants at once). Over it, the frame's further misses are skipped as "
                     "before and waited for in a later frame.")
    .range(0, 60000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(pdz_native_shader_record, true, "Render",
                    "Native renderer: record every shader the draws bind, for the warm-cache tool "
                    "(PDZRenderResearch\\native\\tools\\warm_shader_cache.py): <shader dir>\\record\\bound_<time>_<pid>.txt, "
                    "one line per (created VS, declaration) pair as it resolves (bound variant hash and how: cache, "
                    "sibling, translated, created, missed) and per pixel shader at its first lookup; the microcode of a "
                    "VS variant built from a sibling (no files of its own) goes beside it as <hash>.bin. Cold path: "
                    "once per pair / shader, the file flushed at the next frame.");
REXCVAR_DEFINE_BOOL(pdz_native_vs_blank_copy_unbound, true, "Render",
                    "Native renderer: a D3D bound VS copy whose first control-flow dword is zero (memory D3D has "
                    "not written yet) is treated like a copy with unbound vfetch: the binder's output is used. "
                    "Off: the blank copy's hash wins, misses the cache, and the pair falls back to the unpatched "
                    "container (VS 5cc84ad7348d21c2 decl 40D5AA40: 32_FLOAT stride 0 at fetch slots 95/94/93 = "
                    "streams 0-2, the stream-no-buffer draws of caller 825F1260).");
REXCVAR_DEFINE_BOOL(pdz_native_vs_sibling_variants, true, "Render",
                    "Native renderer: a bound VS variant the cache does not hold is built from a cached bound variant of "
                    "the same created VS when the two microcodes differ only in the vfetch stride / offset fields (and "
                    "prefetch counts) and group the fetches into the same inputs: the fork's HLSL / DXIL are then "
                    "identical and only the input layout changes, read from the new microcode. No translator run, so "
                    "no draws skipped while it runs (Facility, log 190: stride-24 variants of VS 77862c6adeea4cb3 / "
                    "48eef7d3421cd5db, 23 draws in 8 frames). Checked offline on the research cache: 624 sibling pairs "
                    "accepted, all with identical HLSL and the sidecar's own offsets / strides. Off: translate as before.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(pdz_native_vs_binding_cap, 4096, "Render",
                     "Native renderer: most VS (object, declaration) bindings kept. They are keyed by guest addresses, "
                     "so every level adds its own (490-897 per mission, regression 15:04) and the pairs of freed objects "
                     "stay. Checked every 600 frames: over the cap, the least recently used resolved / missed pairs not "
                     "drawn with for 1800 frames are dropped until three quarters of the cap are left (a pair drawn again "
                     "is resolved again: binder, cache lookup). A resolved pair also drops the binder's microcode copy "
                     "once it was checked against D3D's (about 300 bytes a pair). Never reached within one level. "
                     "0 = nothing dropped (the old behaviour).");
REXCVAR_DEFINE_BOOL(pdz_native_literals, false, "Render",
                    "Native renderer: apply the shader containers' literal constant records (sidecar / HLSL "
                    "values, plus the parent's for a bound VS) on top of the device's constant shadow at "
                    "bind. Off: the shadow is uploaded as is (D3D copies the literals into it at SetShader).");

namespace pdz::native
{
  namespace
  {
    constexpr uint32_t kMaxBindRetries = 8;       // draws a VS binding may stay pending before it is a miss
    constexpr uint32_t kMaxUcodeBytes = 64 * 1024; // sanity bound for the bound-copy rehash (largest PDZ shader: 741 dwords)
    constexpr uint32_t kFirstUseLogLines = 32;     // pdz_native_first_use_log: lines per kind, then the periodic line

    struct VsBinding
    {
      enum State : uint8_t
      {
        kPending = 0,
        kResolved = 1,
        kMissed = 2,
        kTranslating = 3,
      };
      uint64_t hash = 0;
      const NativeShader *shader = nullptr;
      uint32_t retries = 0;
      State state = kPending;
      // Binder (native_binder.h): our reproduction of the bound microcode for this pair.
      bool binder_done = false;
      bool binder_checked = false; // compared against D3D's copy
      uint32_t check_attempts = 0;
      uint64_t binder_hash = 0;
      std::vector<uint8_t> binder_bytes;
      bool logged_unpatched = false;
      bool logged_foreign = false; // D3D's copy was not a variant of this VS (logged once per pair)
      bool logged_blank_copy = false;
      uint64_t created_hash = 0; // the VS object's create-time hash this pair was resolved for (review finding 1)
      uint64_t last_used_frame = 0; // native frame of the last FindVertex of the pair (pdz_native_vs_binding_cap LRU)
      double first_use_ms = 0.0;    // time in this pair's lookups until it resolved (pdz_native_first_use_log)
    };

    // Translates shader variants the cache does not hold with the XenosRecomp fork built into the exe
    // (xenosrecomp_pdz.h; 2026-11-03, was a child process), on a small pool of worker threads (cold path; the draw
    // stays pending meanwhile). The files it writes are the CLI's: <hash>.hlsl / .json / .dxil next to <hash>.bin.
    struct Translator
    {
      std::mutex mutex;
      std::deque<std::pair<uint64_t, bool>> queue; // hash, vertex
      std::unordered_map<uint64_t, int> status;    // 0 running / queued, 1 done, -1 failed
      std::condition_variable finished;            // a job's status left 0 (WaitFor)
      std::atomic<bool> stop{false};
      std::string dir;
      std::vector<std::string> container_dirs; // <dir>\\containers, <fallback>\\..\\containers (ResolveDir)
      int running = 0, max_workers = 1;
      bool checked = false, ok = false;

      ~Translator()
      {
        stop = true; // the (detached) workers leave after their current job
      }
      bool Available()
      {
        std::lock_guard lock(mutex);
        if (!checked)
        {
          checked = true;
          void *create = REXCVAR_GET(pdz_native_translate) ? dxc::CreateProc() : nullptr;
          ok = create != nullptr;
          if (ok)
          {
            xenosrecomp_pdz::SetDxcCreate(create);
          }
          const int cvar = REXCVAR_GET(pdz_native_translate_workers);
          max_workers = cvar > 0 ? cvar : std::clamp(int(std::thread::hardware_concurrency()) / 4, 1, 4);
          REXLOG_INFO("pdz_native: shader translator {}",
                      !REXCVAR_GET(pdz_native_translate) ? std::string("off (pdz_native_translate): unseen shader variants stay misses")
                      : ok ? fmt::format("in-process (XenosRecomp pdz, {} worker{})", max_workers, max_workers == 1 ? "" : "s")
                           : std::string("unavailable: no DXC (unseen shader variants stay misses)"));
        }
        return ok;
      }
      // Writes <dir>\<hash>.bin and queues the job. Returns false when already queued / done. `urgent` (a draw waits
      // for it, pdz_native_translate_sync): the job goes to the front of the queue, or moves there if still queued.
      bool Request(const std::string &shader_dir, uint64_t hash, bool vertex, const uint8_t *ucode, uint32_t bytes,
                   bool urgent = false)
      {
        std::lock_guard lock(mutex);
        if (auto known = status.find(hash); known != status.end())
        {
          if (urgent && known->second == 0)
          {
            auto job = std::find_if(queue.begin(), queue.end(), [hash](const auto &j) { return j.first == hash; });
            if (job != queue.end() && job != queue.begin())
            {
              const auto moved = *job;
              queue.erase(job);
              queue.push_front(moved);
            }
          }
          return false;
        }
        if (XXH3_64bits(ucode, bytes) != hash)
        {
          // Never translate bytes that aren't the shader asked for (memory reused since it was hashed).
          REXLOG_WARN("pdz_native: {} {:016x}: its microcode in memory has changed since it was created: not translated",
                      vertex ? "vs" : "ps", hash);
          status[hash] = -1;
          return true;
        }
        dir = shader_dir;
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        const std::string bin = dir + "\\" + fmt::format("{:016x}", hash) + ".bin";
        {
          std::ofstream f(bin, std::ios::binary);
          if (!f || !f.write(reinterpret_cast<const char *>(ucode), bytes))
          {
            REXLOG_WARN("pdz_native: cannot write {}", bin);
            status[hash] = -1;
            return true;
          }
        }
        status[hash] = 0;
        if (urgent)
        {
          queue.push_front({hash, vertex});
        }
        else
        {
          queue.push_back({hash, vertex});
        }
        if (running < max_workers && running < int(queue.size()))
        {
          ++running;
          std::thread([this] { Run(); }).detach();
        }
        return true;
      }
      // 0 = still running, 1 = done, -1 = failed, -2 = unknown hash.
      int Status(uint64_t hash)
      {
        std::lock_guard lock(mutex);
        auto it = status.find(hash);
        return it == status.end() ? -2 : it->second;
      }
      // Status(hash) once it leaves 0 (running / queued) or after `ms`, whichever comes first.
      int WaitFor(uint64_t hash, uint32_t ms)
      {
        std::unique_lock lock(mutex);
        const auto settled = [this, hash] {
          const auto it = status.find(hash);
          return it == status.end() || it->second != 0;
        };
        finished.wait_for(lock, std::chrono::milliseconds(ms), settled);
        const auto it = status.find(hash);
        return it == status.end() ? -2 : it->second;
      }
      void Run()
      {
        SetThreadDescription(GetCurrentThread(), L"PDZ shader translator");
        pdz::cpu::LowerBackgroundThread("shader worker"); // pdz_cpu_placement, pdz_background_priority
        for (;;)
        {
          std::pair<uint64_t, bool> job;
          std::string job_dir;
          {
            std::lock_guard lock(mutex);
            if (stop || queue.empty())
            {
              --running; // decided under the lock: a Request after this starts a new worker
              return;
            }
            job = queue.front();
            queue.pop_front();
            job_dir = dir;
          }
          const int result = Translate(job.first, job.second, job_dir);
          std::lock_guard lock(mutex);
          status[job.first] = result;
          finished.notify_all();
        }
      }
      static bool WriteFileBytes(const std::string &path, const void *data, size_t size)
      {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        return f && f.write(static_cast<const char *>(data), std::streamsize(size)) && f.flush();
      }
      // As the CLI: the HLSL and JSON are written, then DXC compiles; no .dxil when it fails. 1 = ok, -1 = failed.
      int Translate(uint64_t hash, bool vertex, const std::string &dir)
      {
        const std::string name = fmt::format("{:016x}", hash);
        const std::string base = dir + "\\" + name;
        const auto t0 = std::chrono::steady_clock::now();
        // The shader's full container when the scan knows it (2026-11-03: a static shader translated from raw microcode
        // loses its literals and interpolators), else the microcode.
        std::vector<uint8_t> ucode;
        bool from_container = false;
        for (const std::string &cd : container_dirs)
        {
          std::ifstream f(cd + "\\" + name + ".bin", std::ios::binary);
          if (!cd.empty() && f)
          {
            ucode.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
            from_container = !ucode.empty();
            if (from_container)
            {
              break;
            }
          }
        }
        if (!from_container)
        {
          std::ifstream f(base + ".bin", std::ios::binary);
          ucode.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        }
        const xenosrecomp_pdz::Result r = xenosrecomp_pdz::Translate(
            ucode.data(), ucode.size(), vertex ? xenosrecomp_pdz::Type::kVertex : xenosrecomp_pdz::Type::kPixel, true);
        bool ok = r.code == 0 && !r.dxil.empty();
        if (r.code != 1)
        {
          ok = WriteFileBytes(base + ".hlsl", r.hlsl.data(), r.hlsl.size()) &&
               WriteFileBytes(base + ".json", r.json.data(), r.json.size()) && ok;
          if (!r.dxil.empty())
          {
            ok = WriteFileBytes(base + ".dxil", r.dxil.data(), r.dxil.size()) && ok;
          }
        }
        REXLOG_INFO("pdz_native: translated {} {} ({}) in {} ms: {}{}", vertex ? "vs" : "ps", name,
                    from_container ? "its container" : "microcode",
                    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count(),
                    ok ? "ok" : "FAILED", ok ? "" : fmt::format(" (code {}: {})", r.code, r.message));
        return ok ? 1 : -1;
      }
    };

    bool ReadFile(const std::string &path, std::vector<uint8_t> &out)
    {
      std::ifstream f(path, std::ios::binary);
      if (!f)
      {
        return false;
      }
      f.seekg(0, std::ios::end);
      const std::streamoff size = f.tellg();
      if (size < 0)
      {
        return false;
      }
      out.resize(size_t(size));
      f.seekg(0, std::ios::beg);
      if (size > 0 && !f.read(reinterpret_cast<char *>(out.data()), size))
      {
        return false;
      }
      return true;
    }

    bool ReadText(const std::string &path, std::string &out)
    {
      std::vector<uint8_t> bytes;
      if (!ReadFile(path, bytes))
      {
        return false;
      }
      out.assign(reinterpret_cast<const char *>(bytes.data()), bytes.size());
      return true;
    }

    std::string HashName(uint64_t hash) { return fmt::format("{:016x}", hash); }
    // True when a vertex microcode copy still has an unbound vfetch (format 0 in dword 1 of a fetch
    // instruction that the exec clauses mark as a fetch): D3D has not bound it to a declaration yet.
    bool HasUnboundVfetch(const uint8_t *ucode, uint32_t bytes)
    {
      if (!ucode || bytes < 12) return false;
      const uint32_t count = bytes / 4;
      auto be = [&](uint32_t i) { const uint8_t *p = ucode + 4 * i; return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; };
      constexpr uint32_t kExecOps = (1u << 1) | (1u << 2) | (1u << 3) | (1u << 4) | (1u << 5) | (1u << 6) | (1u << 13) | (1u << 14);
      uint32_t bound = count / 3;
      for (uint32_t i = 0; i < bound; ++i)
      {
        const uint32_t d0 = be(3 * i), d1 = be(3 * i + 1), d2 = be(3 * i + 2);
        const uint32_t w0s[2] = {d0, ((d1 >> 16) | (d2 << 16))}, w1s[2] = {d1 & 0xFFFFu, d2 >> 16};
        for (int k = 0; k < 2; ++k)
        {
          if ((kExecOps >> ((w1s[k] >> 12) & 0xF)) & 1u)
          {
            bound = std::min(bound, w0s[k] & 0xFFFu);
          }
        }
      }
      for (uint32_t i = 0; i < bound; ++i)
      {
        const uint32_t d0 = be(3 * i), d1 = be(3 * i + 1), d2 = be(3 * i + 2);
        const uint32_t w0s[2] = {d0, ((d1 >> 16) | (d2 << 16))}, w1s[2] = {d1 & 0xFFFFu, d2 >> 16};
        for (int k = 0; k < 2; ++k)
        {
          if (!((kExecOps >> ((w1s[k] >> 12) & 0xF)) & 1u)) continue;
          const uint32_t addr = w0s[k] & 0xFFFu, n = (w0s[k] >> 12) & 7u, seq = (w0s[k] >> 16) & 0xFFFu;
          for (uint32_t j = 0; j < n; ++j)
          {
            const uint32_t idx = addr + j;
            if (3 * idx + 3 > count) continue;
            if (((seq >> (2 * j)) & 1u) && (be(3 * idx) & 0x1Fu) == 0 && ((be(3 * idx + 1) >> 16) & 0x3Fu) == 0)
            {
              return true;
            }
          }
        }
      }
      return false;
    }

    // pdz_native_vs_sibling_variants. A vertex fetch as the fork's pass over the microcode sees it (exec clauses in
    // control-flow order, the fetch bit of each sequence pair, vfetch opcode 0): a mini fetch has no slot / stride of
    // its own and takes those of the last full fetch; `key` is the fork's input key without the offset (slot, format,
    // signed, number format, exponent adjust), so fetches with equal key and offset share one HLSL input.
    struct VertexFetchField
    {
      uint32_t addr = 0;   // instruction index
      bool mini = false;
      uint32_t stride = 0; // dwords (the governing full fetch's)
      int32_t offset = 0;  // dwords (dword 2 bits 8..30, signed)
      uint32_t format = 0;
      uint32_t key = 0;
    };

    inline uint32_t LoadBe32(const uint8_t *p)
    {
      return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
    }

    void WalkVertexFetches(const std::vector<uint32_t> &dw, std::vector<VertexFetchField> &out)
    {
      out.clear();
      constexpr uint32_t kExecOps = (1u << 1) | (1u << 2) | (1u << 3) | (1u << 4) | (1u << 5) | (1u << 6) | (1u << 13) | (1u << 14);
      const uint32_t count = uint32_t(dw.size());
      uint32_t bound = count / 3; // the control flow ends at the lowest exec address (the fork's instrSize)
      uint32_t last_slot = 0, last_stride = 0;
      for (uint32_t i = 0; i < bound; ++i)
      {
        const uint32_t d0 = dw[3 * i], d1 = dw[3 * i + 1], d2 = dw[3 * i + 2];
        const uint32_t w0s[2] = {d0, (d1 >> 16) | (d2 << 16)}, w1s[2] = {d1 & 0xFFFFu, d2 >> 16};
        for (int k = 0; k < 2; ++k)
        {
          if (!((kExecOps >> ((w1s[k] >> 12) & 0xFu)) & 1u))
          {
            continue;
          }
          const uint32_t addr = w0s[k] & 0xFFFu, n = (w0s[k] >> 12) & 7u, seq = (w0s[k] >> 16) & 0xFFFu;
          bound = std::min(bound, addr);
          for (uint32_t j = 0; j < n; ++j)
          {
            const uint32_t idx = addr + j;
            if (3 * idx + 3 > count || !((seq >> (2 * j)) & 1u) || (dw[3 * idx] & 0x1Fu) != 0)
            {
              continue;
            }
            const uint32_t i0 = dw[3 * idx], i1 = dw[3 * idx + 1], i2 = dw[3 * idx + 2];
            VertexFetchField f;
            f.addr = idx;
            f.mini = ((i1 >> 30) & 1u) != 0;
            if (!f.mini)
            {
              last_slot = ((i0 >> 20) & 0x1Fu) * 3 + ((i0 >> 25) & 3u);
              last_stride = i2 & 0xFFu;
            }
            f.stride = last_stride;
            f.offset = int32_t(i2 << 1) >> 9;
            f.format = (i1 >> 16) & 0x3Fu;
            f.key = last_slot | (f.format << 8) | (((i1 >> 12) & 3u) << 14) | (((i1 >> 24) & 0x3Fu) << 16);
            out.push_back(f);
          }
        }
      }
    }

    // True when the guest-order microcodes `cand` and `sib` (same size) differ only in vfetch dword 2 bits 0..30
    // (stride, offset) and dword 0 bits 27..29 (prefetch count), and their fetches group into the fork's inputs the
    // same way (among fetches of one key, equal offsets stay equal and distinct ones distinct). The fork's HLSL depends
    // on neither field otherwise (they only reach the sidecar's offset_dwords / stride_dwords), so `sib`'s DXIL serves
    // `cand`. Checked offline on the research cache (every pair of bound variants of one parent): 624 accepted, each
    // with identical HLSL and the sidecar's own offsets / strides; 378 refused.
    bool DiffersOnlyInLayout(const uint8_t *cand, const uint8_t *sib, uint32_t bytes,
                             std::vector<VertexFetchField> &cand_fetches, std::vector<VertexFetchField> &sib_fetches,
                             const char **why)
    {
      const uint32_t count = bytes / 4;
      std::vector<uint32_t> a(count), b(count);
      for (uint32_t i = 0; i < count; ++i)
      {
        a[i] = LoadBe32(cand + 4 * i);
        b[i] = LoadBe32(sib + 4 * i);
      }
      WalkVertexFetches(a, cand_fetches);
      WalkVertexFetches(b, sib_fetches);
      if (cand_fetches.empty() || cand_fetches.size() != sib_fetches.size())
      {
        *why = "the vertex fetches differ";
        return false;
      }
      std::vector<uint8_t> role(count, 0); // 1 = a vfetch's dword 0, 2 = its dword 2
      for (size_t i = 0; i < cand_fetches.size(); ++i)
      {
        if (cand_fetches[i].addr != sib_fetches[i].addr || cand_fetches[i].mini != sib_fetches[i].mini)
        {
          *why = "the vertex fetches differ";
          return false;
        }
        role[3 * cand_fetches[i].addr] = 1;
        role[3 * cand_fetches[i].addr + 2] = 2;
      }
      for (uint32_t i = 0; i < count; ++i)
      {
        const uint32_t x = a[i] ^ b[i];
        if (x == 0 || (role[i] == 2 && !(x & 0x80000000u)) || (role[i] == 1 && !(x & ~0x38000000u)))
        {
          continue;
        }
        *why = "the microcode differs outside the vfetch strides / offsets";
        return false;
      }
      for (size_t i = 0; i < cand_fetches.size(); ++i)
      {
        for (size_t j = i + 1; j < cand_fetches.size(); ++j)
        {
          if (cand_fetches[i].key != cand_fetches[j].key || cand_fetches[i].format == 0)
          {
            continue;
          }
          if ((cand_fetches[i].offset == cand_fetches[j].offset) != (sib_fetches[i].offset == sib_fetches[j].offset))
          {
            *why = "the fetches would group into other inputs";
            return false;
          }
        }
      }
      return true;
    }

    // Input layout identity: the IA description (formats / offsets / strides / slots), not the shader.
    uint64_t InputLayoutHash(const std::vector<ShaderVertexInput> &inputs)
    {
      XXH3_state_t *st = XXH3_createState();
      XXH3_64bits_reset(st);
      for (const ShaderVertexInput &v : inputs)
      {
        XXH3_64bits_update(st, &v, sizeof(v));
      }
      const uint64_t h = XXH3_64bits_digest(st);
      XXH3_freeState(st);
      return h;
    }

    uint64_t ParseHash(std::string_view s)
    {
      if (s.size() != 16)
      {
        return 0;
      }
      char buf[17];
      s.copy(buf, 16);
      buf[16] = 0;
      char *end = nullptr;
      const unsigned long long v = std::strtoull(buf, &end, 16);
      return end == buf + 16 ? uint64_t(v) : 0;
    }

    // Hex string ("0x21") or number.
    uint32_t ParseHexOrNumber(const json::Value *v, uint32_t def)
    {
      if (!v)
      {
        return def;
      }
      if (v->IsNumber())
      {
        return v->AsU32(def);
      }
      if (v->IsString())
      {
        const std::string s(v->AsString());
        char *end = nullptr;
        const unsigned long long x = std::strtoull(s.c_str(), &end, 0);
        return (end && end != s.c_str()) ? uint32_t(x) : def;
      }
      return def;
    }

    DXGI_FORMAT DxgiFromName(std::string_view name)
    {
      struct Row
      {
        const char *name;
        DXGI_FORMAT format;
      };
      static const Row kRows[] = {
          {"DXGI_FORMAT_R32G32B32A32_FLOAT", DXGI_FORMAT_R32G32B32A32_FLOAT},
          {"DXGI_FORMAT_R32G32B32A32_UINT", DXGI_FORMAT_R32G32B32A32_UINT},
          {"DXGI_FORMAT_R32G32B32A32_SINT", DXGI_FORMAT_R32G32B32A32_SINT},
          {"DXGI_FORMAT_R32G32B32_FLOAT", DXGI_FORMAT_R32G32B32_FLOAT},
          {"DXGI_FORMAT_R32G32B32_UINT", DXGI_FORMAT_R32G32B32_UINT},
          {"DXGI_FORMAT_R32G32B32_SINT", DXGI_FORMAT_R32G32B32_SINT},
          {"DXGI_FORMAT_R32G32_FLOAT", DXGI_FORMAT_R32G32_FLOAT},
          {"DXGI_FORMAT_R32G32_UINT", DXGI_FORMAT_R32G32_UINT},
          {"DXGI_FORMAT_R32G32_SINT", DXGI_FORMAT_R32G32_SINT},
          {"DXGI_FORMAT_R32_FLOAT", DXGI_FORMAT_R32_FLOAT},
          {"DXGI_FORMAT_R32_UINT", DXGI_FORMAT_R32_UINT},
          {"DXGI_FORMAT_R32_SINT", DXGI_FORMAT_R32_SINT},
          {"DXGI_FORMAT_R16G16B16A16_FLOAT", DXGI_FORMAT_R16G16B16A16_FLOAT},
          {"DXGI_FORMAT_R16G16B16A16_UNORM", DXGI_FORMAT_R16G16B16A16_UNORM},
          {"DXGI_FORMAT_R16G16B16A16_UINT", DXGI_FORMAT_R16G16B16A16_UINT},
          {"DXGI_FORMAT_R16G16B16A16_SNORM", DXGI_FORMAT_R16G16B16A16_SNORM},
          {"DXGI_FORMAT_R16G16B16A16_SINT", DXGI_FORMAT_R16G16B16A16_SINT},
          {"DXGI_FORMAT_R16G16_FLOAT", DXGI_FORMAT_R16G16_FLOAT},
          {"DXGI_FORMAT_R16G16_UNORM", DXGI_FORMAT_R16G16_UNORM},
          {"DXGI_FORMAT_R16G16_UINT", DXGI_FORMAT_R16G16_UINT},
          {"DXGI_FORMAT_R16G16_SNORM", DXGI_FORMAT_R16G16_SNORM},
          {"DXGI_FORMAT_R16G16_SINT", DXGI_FORMAT_R16G16_SINT},
          {"DXGI_FORMAT_R16_FLOAT", DXGI_FORMAT_R16_FLOAT},
          {"DXGI_FORMAT_R16_UNORM", DXGI_FORMAT_R16_UNORM},
          {"DXGI_FORMAT_R16_UINT", DXGI_FORMAT_R16_UINT},
          {"DXGI_FORMAT_R16_SNORM", DXGI_FORMAT_R16_SNORM},
          {"DXGI_FORMAT_R16_SINT", DXGI_FORMAT_R16_SINT},
          {"DXGI_FORMAT_R8G8B8A8_UNORM", DXGI_FORMAT_R8G8B8A8_UNORM},
          {"DXGI_FORMAT_R8G8B8A8_UINT", DXGI_FORMAT_R8G8B8A8_UINT},
          {"DXGI_FORMAT_R8G8B8A8_SNORM", DXGI_FORMAT_R8G8B8A8_SNORM},
          {"DXGI_FORMAT_R8G8B8A8_SINT", DXGI_FORMAT_R8G8B8A8_SINT},
          {"DXGI_FORMAT_R8G8_UNORM", DXGI_FORMAT_R8G8_UNORM},
          {"DXGI_FORMAT_R8G8_UINT", DXGI_FORMAT_R8G8_UINT},
          {"DXGI_FORMAT_R8G8_SNORM", DXGI_FORMAT_R8G8_SNORM},
          {"DXGI_FORMAT_R8G8_SINT", DXGI_FORMAT_R8G8_SINT},
          {"DXGI_FORMAT_R8_UNORM", DXGI_FORMAT_R8_UNORM},
          {"DXGI_FORMAT_R8_UINT", DXGI_FORMAT_R8_UINT},
          {"DXGI_FORMAT_R8_SNORM", DXGI_FORMAT_R8_SNORM},
          {"DXGI_FORMAT_R8_SINT", DXGI_FORMAT_R8_SINT},
          {"DXGI_FORMAT_R10G10B10A2_UNORM", DXGI_FORMAT_R10G10B10A2_UNORM},
          {"DXGI_FORMAT_R10G10B10A2_UINT", DXGI_FORMAT_R10G10B10A2_UINT},
          {"DXGI_FORMAT_R11G11B10_FLOAT", DXGI_FORMAT_R11G11B10_FLOAT},
      };
      for (const Row &row : kRows)
      {
        if (name == row.name)
        {
          return row.format;
        }
      }
      return DXGI_FORMAT_UNKNOWN;
    }

    TexDim DimFromName(std::string_view s)
    {
      if (s == "3D")
      {
        return TexDim::k3D;
      }
      if (s == "Cube")
      {
        return TexDim::kCube;
      }
      return TexDim::k2D;
    }

    // Literal record index -> where it lands. The container records are byte offsets into the GPU constant
    // file (float register n at 0x300 + 16 n, PS n + 256; the 8 bool dwords at 0x2300 = "register" 512..513;
    // the 32 loop dwords at 0x2320 = 514..521). The fork lists PS float literals PS-relative (0..223), so a
    // PS bool / loop record may arrive either as 512.. or as 256.. (one such record exists in PDZ, in a VS).
    enum class LiteralKind
    {
      kFloat,
      kBool,
      kLoop,
      kUnknown
    };
    LiteralKind ClassifyLiteral(bool vertex, uint32_t index, uint32_t &slot)
    {
      uint32_t n = index;
      if (!vertex && n >= 256 && n < 512)
      {
        n += 256; // PS-relative bool / loop record
      }
      if (n >= 512 && n <= 513)
      {
        slot = (n - 512) * 4; // first bool dword of the record
        return LiteralKind::kBool;
      }
      if (n >= 514 && n <= 521)
      {
        slot = (n - 514) * 4; // first loop dword
        return LiteralKind::kLoop;
      }
      if (index < 256)
      {
        slot = index;
        return LiteralKind::kFloat;
      }
      return LiteralKind::kUnknown;
    }

    void ApplyLiteral(NativeShader &s, uint32_t index, const uint32_t bits[4])
    {
      uint32_t slot = 0;
      switch (ClassifyLiteral(s.is_vertex, index, slot))
      {
      case LiteralKind::kFloat:
      {
        FloatLiteral l;
        l.reg = slot;
        std::memcpy(l.bits, bits, sizeof(l.bits));
        s.float_literals.push_back(l);
        break;
      }
      case LiteralKind::kBool:
        for (uint32_t i = 0; i < 4 && slot + i < 8; ++i)
        {
          s.bool_literal_bits[slot + i] = bits[i];
          s.bool_literal_mask[slot + i] = 0xFFFFFFFFu;
        }
        break;
      case LiteralKind::kLoop:
        // A zero loop constant (count 0) is never a meaningful default: treat the record's zero padding as
        // "not present" (the record's dword count is not in the sidecar).
        for (uint32_t i = 0; i < 4 && slot + i < 32; ++i)
        {
          if (bits[i] != 0)
          {
            s.loop_literals[slot + i] = bits[i];
            s.loop_literal_mask |= 1u << (slot + i);
          }
        }
        break;
      default:
        break;
      }
    }

    // "static const float4 cl<n> = asfloat(uint4(0x..u, 0x..u, 0x..u, 0x..u));" lines of the emitted HLSL.
    void ParseHlslLiterals(const std::string &hlsl, NativeShader &s, uint32_t &count)
    {
      size_t pos = 0;
      const std::string_view key = "static const float4 cl";
      while ((pos = hlsl.find(key, pos)) != std::string::npos)
      {
        pos += key.size();
        char *end = nullptr;
        const unsigned long n = std::strtoul(hlsl.c_str() + pos, &end, 10);
        if (!end || end == hlsl.c_str() + pos)
        {
          continue;
        }
        const size_t open = hlsl.find("uint4(", size_t(end - hlsl.c_str()));
        if (open == std::string::npos)
        {
          break;
        }
        uint32_t bits[4] = {};
        const char *p = hlsl.c_str() + open + 6;
        bool ok = true;
        for (int i = 0; i < 4; ++i)
        {
          while (*p == ' ' || *p == ',')
          {
            ++p;
          }
          char *e = nullptr;
          const unsigned long long v = std::strtoull(p, &e, 0);
          if (!e || e == p)
          {
            ok = false;
            break;
          }
          bits[i] = uint32_t(v);
          p = e;
          if (*p == 'u')
          {
            ++p;
          }
        }
        if (ok)
        {
          ApplyLiteral(s, uint32_t(n), bits);
          ++count;
        }
        pos = open;
      }
    }

    // Sidecar "bool_literals": [{"dword": d, "value": v}] and "loop_literals": [{"loop": i, "value": v, ...}]: the
    // literal records past the float file (bool dwords at 0x2300, loop constants at 0x2320), exact per dword (fork
    // pdz-oracle, patch 13). Absent from older caches, whose HLSL carried the one PDZ record as cl514 instead
    // (ParseHlslLiterals still classifies that), so either cache gives the same defaults.
    uint32_t LoadFlowLiterals(const json::Value &doc, NativeShader &s)
    {
      uint32_t count = 0;
      if (const json::Value *bools = doc.Get("bool_literals"); bools && bools->IsArray())
      {
        for (size_t i = 0; i < bools->size(); ++i)
        {
          const json::Value &rec = (*bools)[i];
          const json::Value *dword = rec.Get("dword");
          const json::Value *value = rec.Get("value");
          if (!dword || !value || dword->AsU32(8) >= 8)
          {
            continue;
          }
          s.bool_literal_bits[dword->AsU32()] = value->AsU32();
          s.bool_literal_mask[dword->AsU32()] = 0xFFFFFFFFu;
          ++count;
        }
      }
      if (const json::Value *loops = doc.Get("loop_literals"); loops && loops->IsArray())
      {
        for (size_t i = 0; i < loops->size(); ++i)
        {
          const json::Value &rec = (*loops)[i];
          const json::Value *loop = rec.Get("loop");
          const json::Value *value = rec.Get("value");
          if (!loop || !value || loop->AsU32(32) >= 32)
          {
            continue;
          }
          s.loop_literals[loop->AsU32()] = value->AsU32();
          s.loop_literal_mask |= 1u << loop->AsU32();
          ++count;
        }
      }
      return count;
    }

    // -----------------------------------------------------------------------------------------------------------------
    // pdz_native_cb_used_only (stage 2 M5 drawcost): which float constants a shader can read.
    // -----------------------------------------------------------------------------------------------------------------
    struct FloatScanStats
    {
      uint32_t trimmed = 0;  // shaders with runs (the draw path uploads only those registers)
      uint32_t relative = 0; // whole block: relative addressing (sidecar uses_relative_constants)
      uint32_t no_list = 0;  // whole block: no sidecar float_constants_used / uses_relative_constants
      uint32_t no_hlsl = 0;  // whole block: no HLSL to check the list against
      uint32_t scan = 0;     // whole block: the HLSL indexes c[] or calls cRel( in a form the scan does not accept
      uint32_t hlsl_extra = 0; // trimmed shaders whose HLSL read registers the sidecar list lacks (added; logged)
      uint32_t logged = 0;
    };
    FloatScanStats g_float_scan;

    // The float constant registers an HLSL file reads, into `used` (c[] indices 0..255). Comments are skipped; the source
    // is read as maximal [A-Za-z0-9_] tokens. Accepted: c[<digits>] (a static read; the cbuffer declaration "float4
    // c[256];" is index 256, outside the block), cl<digits> tokens (the fork's literal registers: declarations and uses
    // alike mark the register, since pdz_native_live_literals turns them into c[<n>] reads), the relative helper's
    // definition ("float4 cRel(") and its body (c[uint(index) & 255u]). Any other indexing of c[] or call of cRel( gives
    // false with `why` (the caller then uploads the whole block).
    bool ScanHlslConstants(const std::string &hlsl, bool (&used)[256], uint32_t &extra, std::string &why)
    {
      static const std::string kHelperIndex = "[uint(index) & 255u]";
      const auto token_char = [](char ch) { return std::isalnum(static_cast<unsigned char>(ch)) != 0 || ch == '_'; };
      const size_t n = hlsl.size();
      size_t i = 0;
      while (i < n)
      {
        const char ch = hlsl[i];
        if (ch == '/' && i + 1 < n && hlsl[i + 1] == '/')
        {
          i = hlsl.find('\n', i);
          if (i == std::string::npos)
          {
            break;
          }
          continue;
        }
        if (ch == '/' && i + 1 < n && hlsl[i + 1] == '*')
        {
          i = hlsl.find("*/", i + 2);
          if (i == std::string::npos)
          {
            break;
          }
          i += 2;
          continue;
        }
        if (!token_char(ch))
        {
          ++i;
          continue;
        }
        size_t j = i;
        while (j < n && token_char(hlsl[j]))
        {
          ++j;
        }
        const std::string_view token(hlsl.data() + i, j - i);
        if (token == "c")
        {
          size_t k = j;
          while (k < n && (hlsl[k] == ' ' || hlsl[k] == '\t'))
          {
            ++k;
          }
          if (k < n && hlsl[k] == '[')
          {
            size_t d = k + 1;
            uint32_t v = 0, digits = 0;
            while (d < n && std::isdigit(static_cast<unsigned char>(hlsl[d])) && digits < 6)
            {
              v = v * 10 + uint32_t(hlsl[d] - '0');
              ++d;
              ++digits;
            }
            if (digits && d < n && hlsl[d] == ']')
            {
              if (v < 256)
              {
                extra += used[v] ? 0u : 1u;
                used[v] = true;
              }
              i = d + 1;
              continue;
            }
            if (hlsl.compare(k, kHelperIndex.size(), kHelperIndex) == 0)
            {
              i = k + kHelperIndex.size();
              continue;
            }
            why = "c[] with a computed index: '" + hlsl.substr(i, std::min<size_t>(40, n - i)) + "'";
            return false;
          }
        }
        else if (token == "cRel")
        {
          // The definition "float4 cRel(int index)" is fine; any other occurrence is a relative read.
          size_t b = i;
          while (b > 0 && (hlsl[b - 1] == ' ' || hlsl[b - 1] == '\t'))
          {
            --b;
          }
          if (!(b >= 6 && hlsl.compare(b - 6, 6, "float4") == 0 && (b == 6 || !token_char(hlsl[b - 7]))))
          {
            why = "relative read (cRel)";
            return false;
          }
        }
        else if (token.size() > 2 && token[0] == 'c' && token[1] == 'l' &&
                 std::all_of(token.begin() + 2, token.end(), [](char d) { return d >= '0' && d <= '9'; }) &&
                 token.size() <= 8)
        {
          uint32_t v = 0;
          for (size_t d = 2; d < token.size(); ++d)
          {
            v = v * 10 + uint32_t(token[d] - '0');
          }
          if (v < 256)
          {
            used[v] = true; // a literal register (not counted as extra: the literal records hold it too)
          }
        }
        i = j;
      }
      return true;
    }

    // Fills s.float_constants_all / float_constant_runs (after the literals are loaded and merged).
    void FinalizeFloatConstants(const json::Value &doc, const std::string &hlsl_path, uint64_t hash, NativeShader &s)
    {
      FloatScanStats &st = g_float_scan;
      s.float_constants_all = true;
      s.float_constant_runs.clear();
      s.float_constants_count = 0;
      const json::Value *list = doc.Get("float_constants_used");
      const json::Value *relative = doc.Get("uses_relative_constants");
      if (!list || !list->IsArray() || !relative)
      {
        ++st.no_list;
        return;
      }
      if (relative->AsBool(true))
      {
        ++st.relative;
        return;
      }
      bool used[256] = {};
      for (size_t i = 0; i < list->size(); ++i)
      {
        const uint32_t v = (*list)[i].AsU32(256);
        if (v >= 256)
        {
          ++st.no_list; // not a register index this code understands
          return;
        }
        used[v] = true;
      }
      std::string hlsl;
      if (!ReadText(hlsl_path, hlsl))
      {
        ++st.no_hlsl;
        return;
      }
      uint32_t extra = 0;
      std::string why;
      if (!ScanHlslConstants(hlsl, used, extra, why))
      {
        ++st.scan;
        if (st.logged < 8)
        {
          ++st.logged;
          REXLOG_INFO("pdz_native: shader {} uploads its whole float constant block (pdz_native_cb_used_only): {}",
                      HashName(hash), why);
        }
        return;
      }
      if (extra)
      {
        ++st.hlsl_extra;
        if (st.logged < 8)
        {
          ++st.logged;
          REXLOG_INFO("pdz_native: shader {}: the HLSL reads {} float constant(s) the sidecar list lacks (added; "
                      "pdz_native_cb_used_only)",
                      HashName(hash), extra);
        }
      }
      for (const FloatLiteral &l : s.float_literals)
      {
        if (l.reg < 256)
        {
          used[l.reg] = true;
        }
      }
      for (uint32_t r = 0; r < 256;)
      {
        if (!used[r])
        {
          ++r;
          continue;
        }
        uint32_t e = r;
        while (e < 256 && used[e])
        {
          ++e;
        }
        s.float_constant_runs.emplace_back(uint16_t(r), uint16_t(e - r));
        s.float_constants_count = uint16_t(s.float_constants_count + (e - r));
        r = e;
      }
      s.float_constants_all = false;
      ++st.trimmed;
    }

    // Sidecar "literal_values": [{"register": n, "value": [u32 x4]}] (future fork output); else the HLSL. Plus the
    // bool / loop literal lists when the sidecar has them.
    uint32_t LoadLiteralValues(const json::Value &doc, const std::string &hlsl_path, NativeShader &s)
    {
      uint32_t count = LoadFlowLiterals(doc, s);
      const json::Value *values = doc.Get("literal_values");
      if (values && values->IsArray())
      {
        for (size_t i = 0; i < values->size(); ++i)
        {
          const json::Value &rec = (*values)[i];
          const json::Value *reg = rec.Get("register");
          if (!reg)
          {
            reg = rec.Get("index");
          }
          const json::Value *val = rec.Get("value");
          if (!reg || !val || !val->IsArray() || val->size() < 4)
          {
            continue;
          }
          uint32_t bits[4];
          for (int k = 0; k < 4; ++k)
          {
            bits[k] = (*val)[size_t(k)].AsU32();
          }
          ApplyLiteral(s, reg->AsU32(), bits);
          ++count;
        }
        return count;
      }
      std::string hlsl;
      if (ReadText(hlsl_path, hlsl))
      {
        ParseHlslLiterals(hlsl, s, count);
      }
      return count;
    }
  } // namespace

  struct ShaderCache::Impl
  {
    std::string dir;
    std::string fallback; // pdz_native_shader_fallback_dir ('' = none)
    bool dir_resolved = false;
    bool fallback_used_logged = false;
    bool misses_dir_ready = false;
    std::unordered_map<uint64_t, NativeShader *> loaded; // every hash ever asked for (usable or not)
    std::unordered_map<uint64_t, VsBinding> bindings;    // (object << 32 | declaration) -> binding
    std::unordered_set<uint64_t> missed;                 // hashes dumped / logged
    std::unordered_map<uint64_t, uint64_t> parents;      // bound VS hash -> parent (manifest derived_from)
    std::unordered_set<uint64_t> derive_failed;           // variants no sibling could serve (tried once)
    Translator translator;
    std::unordered_set<uint64_t> ps_translating;          // pixel shaders handed to the translator
    // Drops a remembered failure so a freshly translated shader can be loaded.
    void ForgetUnusable(uint64_t hash)
    {
      auto it = loaded.find(hash);
      if (it != loaded.end() && it->second && !it->second->usable)
      {
        delete it->second;
        loaded.erase(it);
      }
    }
    bool manifest_loaded = false;
    uint64_t last_log_frame = 0;
    uint64_t last_logged_hits = 0, last_logged_misses = 0;
    // pdz_native_vs_binding_cap: OnFrame's frame (FindVertex's LRU stamp), the maintenance cadence and the counters.
    uint64_t frame = 0;
    uint64_t last_binding_maintenance = 0;
    uint64_t bindings_evicted = 0;
    uint64_t binder_bytes_released = 0;
    uint32_t binding_evict_logged = 0;
    // pdz_native_translate_sync: the frame the budget belongs to, its waited ms, the shaders whose wait timed out
    // (never waited for again), log lines written.
    uint64_t sync_frame = ~0ull;
    uint32_t sync_frame_ms = 0;
    std::unordered_set<uint64_t> sync_gave_up;
    uint32_t sync_logged = 0;
    // pdz_native_shader_record: the session's record file (opened at the first line), flushed by OnFrame.
    bool record_checked = false;
    std::string record_dir, record_path;
    std::ofstream record;
    bool record_dirty = false;
    // pdz_native_first_use_log: lines written per kind.
    uint32_t first_use_vs_logged = 0, first_use_ps_logged = 0;

    void ResolveDir()
    {
      if (dir_resolved)
      {
        return;
      }
      dir_resolved = true;
      dir = REXCVAR_GET(pdz_native_shader_dir);
      if (dir.empty())
      {
        // 2026-11-03: a release keeps them in the per-user cache (made there from the player's own game files); a
        // developer's build (or the play copy) next to the exe, as before.
        dir = DeveloperBuild() || CacheRoot().empty() ? ExeDir() + "\\native_shaders" : CacheRoot() + "\\native_shaders";
      }
      while (!dir.empty() && (dir.back() == '\\' || dir.back() == '/'))
      {
        dir.pop_back();
      }
      fallback = REXCVAR_GET(pdz_native_shader_fallback_dir);
      if (fallback.empty())
      {
        fallback = ResearchCacheDefault(); // "" in a release
      }
      if (fallback == "none")
      {
        fallback.clear();
      }
      translator.container_dirs = {dir + "\\containers",
                                   fallback.empty() ? std::string()
                                                    : std::filesystem::path(fallback).parent_path().string() + "\\containers"};
      while (!fallback.empty() && (fallback.back() == '\\' || fallback.back() == '/'))
      {
        fallback.pop_back();
      }
      std::error_code ec;
      const bool primary = std::filesystem::is_directory(dir, ec);
      const bool fallback_ok = !fallback.empty() && std::filesystem::is_directory(fallback, ec);
      REXLOG_INFO("pdz_native: shader dir '{}'{}; fallback {}{}", dir, primary ? "" : " (missing)",
                  fallback.empty() ? std::string("none (pdz_native_shader_fallback_dir)") : "'" + fallback + "'",
                  fallback.empty() || fallback_ok ? "" : " (missing)");
      // pdz_native_shader_file_warm (overnight 30-31 Oct, log 318: Trinity Escape's first frames loaded hundreds of
      // shaders on the record thread, cold file opens 10-11 ms each, one 1.4 s frame): every file of the shader
      // folders is read once at startup on a background thread at background I/O priority, so the record thread's
      // first-use loads hit the OS file cache. Reads only: nothing loaded or changed.
      if (REXCVAR_GET(pdz_native_shader_file_warm))
      {
        std::vector<std::string> roots;
        if (primary)
        {
          roots.push_back(dir);
        }
        if (fallback_ok)
        {
          roots.push_back(fallback);
          const std::filesystem::path up = std::filesystem::path(fallback).parent_path();
          if (std::filesystem::is_directory(up / "ucode", ec))
          {
            roots.push_back((up / "ucode").string());
          }
        }
        std::thread([roots]() {
          SetThreadDescription(GetCurrentThread(), L"PDZ shader file warm");
          pdz::cpu::LowerBackgroundThread("shader file warm"); // pdz_background_priority
          SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN); // low I/O priority as well
          const auto t0 = std::chrono::steady_clock::now();
          std::vector<char> buf(1 << 20);
          uint64_t files = 0, bytes = 0;
          for (const std::string &root : roots)
          {
            std::error_code e;
            for (auto it = std::filesystem::recursive_directory_iterator(root, e);
                 !e && it != std::filesystem::recursive_directory_iterator(); it.increment(e))
            {
              if (!it->is_regular_file(e))
              {
                continue;
              }
              const std::string ext = it->path().extension().string();
              if (ext != ".dxil" && ext != ".json" && ext != ".hlsl" && ext != ".bin")
              {
                continue;
              }
              HANDLE h = CreateFileW(it->path().c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                     nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
              if (h == INVALID_HANDLE_VALUE)
              {
                continue;
              }
              DWORD got = 0;
              while (::ReadFile(h, buf.data(), DWORD(buf.size()), &got, nullptr) && got)
              {
                bytes += got;
              }
              CloseHandle(h);
              ++files;
            }
          }
          SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
          REXLOG_INFO("pdz_native: shader file warm: {} files, {:.1f} MB read in {:.1f} s (background; first-use loads "
                      "now hit the OS file cache)",
                      files, double(bytes) / 1048576.0,
                      std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
        }).detach();
      }
      const int32_t sync = REXCVAR_GET(pdz_native_translate_sync);
      if (SyncTranslateOn())
      {
        REXLOG_INFO("pdz_native: synchronous translation on a shader miss on (pdz_native_translate_sync {}{}): a draw "
                    "waits up to {} ms for its variant's translation, at most {} ms per frame",
                    sync, sync < 0 ? ", graphics_backend null" : "", REXCVAR_GET(pdz_native_translate_sync_ms),
                    REXCVAR_GET(pdz_native_translate_sync_frame_ms));
      }
    }

    // pdz_native_translate_sync: -1 = with the null GPU backend only (gpu_sync.h, latched before the guest runs).
    static bool SyncTranslateOn()
    {
      const int32_t mode = REXCVAR_GET(pdz_native_translate_sync);
      return mode > 0 || (mode < 0 && pdz::gpusync::NullBackend());
    }

    // Guest-order microcode of a shader: <dir>\<hash>.bin (what the translator was given) or the research cache's
    // ucode folder beside its hlsl folder (<dir>\..\ucode\<hash>.bin), primary dir first. Empty when neither exists.
    std::string LocateUcode(uint64_t hash)
    {
      ResolveDir();
      const std::string name = HashName(hash) + ".bin";
      std::error_code ec;
      for (const std::string &root : {dir, fallback})
      {
        if (root.empty())
        {
          continue;
        }
        for (const std::string &path : {root + "\\" + name, root + "\\..\\ucode\\" + name})
        {
          if (std::filesystem::exists(path, ec))
          {
            return path;
          }
        }
      }
      return std::string();
    }

    // Path of <hash>.<ext> in the primary dir, else the fallback dir (nullptr-equivalent: empty string).
    std::string Locate(uint64_t hash, const char *ext)
    {
      ResolveDir();
      const std::string name = HashName(hash) + ext;
      std::error_code ec;
      std::string path = dir + "\\" + name;
      if (std::filesystem::exists(path, ec))
      {
        return path;
      }
      if (fallback.empty())
      {
        return std::string();
      }
      path = fallback + "\\" + name;
      if (std::filesystem::exists(path, ec))
      {
        if (!fallback_used_logged)
        {
          fallback_used_logged = true;
          REXLOG_INFO("pdz_native: shader files served from the fallback dir {}", fallback);
        }
        return path;
      }
      return std::string();
    }

    // manifest.json rows: "hash": "<h>" ... "derived_from": "<p>" (bound VS -> parent container VS).
    void LoadManifest()
    {
      if (manifest_loaded)
      {
        return;
      }
      manifest_loaded = true;
      ResolveDir();
      const char *candidates[] = {"\\manifest.json", "\\..\\manifest.json"};
      std::string text;
      for (const std::string &root : {dir, fallback})
      {
        if (root.empty())
        {
          continue;
        }
        for (const char *c : candidates)
        {
          if (ReadText(root + c, text))
          {
            break;
          }
          text.clear();
        }
        if (!text.empty())
        {
          break;
        }
      }
      if (text.empty())
      {
        return;
      }
      const std::string_view hash_key = "\"hash\": \"";
      const std::string_view parent_key = "\"derived_from\": \"";
      size_t pos = 0;
      uint32_t rows = 0;
      while ((pos = text.find(hash_key, pos)) != std::string::npos)
      {
        pos += hash_key.size();
        const uint64_t h = ParseHash(std::string_view(text).substr(pos, 16));
        const size_t next = text.find(hash_key, pos);
        const size_t p = text.find(parent_key, pos);
        if (p != std::string::npos && (next == std::string::npos || p < next))
        {
          const uint64_t parent = ParseHash(std::string_view(text).substr(p + parent_key.size(), 16));
          if (h && parent)
          {
            parents[h] = parent;
          }
        }
        ++rows;
      }
      REXLOG_INFO("pdz_native: manifest read, {} rows, {} bound VS with a parent", rows, parents.size());
    }

    // Literal values of another shader (the parent of a bound VS) merged into `s`.
    void MergeParentLiterals(NativeShader &s, uint64_t parent)
    {
      const std::string json_path = Locate(parent, ".json");
      if (json_path.empty())
      {
        return;
      }
      std::string text;
      json::Value doc;
      if (!ReadText(json_path, text) || !json::Parse(text, doc))
      {
        return;
      }
      NativeShader tmp;
      tmp.is_vertex = s.is_vertex;
      const std::string hlsl_path = json_path.substr(0, json_path.size() - 5) + ".hlsl";
      LoadLiteralValues(doc, hlsl_path, tmp);
      // Own records win over the parent's.
      for (const FloatLiteral &pl : tmp.float_literals)
      {
        bool own = false;
        for (const FloatLiteral &l : s.float_literals)
        {
          if (l.reg == pl.reg)
          {
            own = true;
            break;
          }
        }
        if (!own)
        {
          s.float_literals.push_back(pl);
        }
      }
      for (uint32_t i = 0; i < 8; ++i)
      {
        if (!s.bool_literal_mask[i] && tmp.bool_literal_mask[i])
        {
          s.bool_literal_bits[i] = tmp.bool_literal_bits[i];
          s.bool_literal_mask[i] = tmp.bool_literal_mask[i];
        }
      }
      for (uint32_t i = 0; i < 32; ++i)
      {
        if (!(s.loop_literal_mask & (1u << i)) && (tmp.loop_literal_mask & (1u << i)))
        {
          s.loop_literals[i] = tmp.loop_literals[i];
          s.loop_literal_mask |= 1u << i;
        }
      }
    }

    bool ParseSidecar(const json::Value &doc, NativeShader &s, std::string &why)
    {
      const std::string_view type = doc.GetString("type");
      if (type == "vs")
      {
        s.is_vertex = true;
      }
      else if (type == "ps")
      {
        s.is_vertex = false;
      }
      else
      {
        why = "sidecar type is neither vs nor ps";
        return false;
      }

      if (const json::Value *inputs = doc.Get("vertex_inputs"); inputs && inputs->IsArray())
      {
        for (size_t i = 0; i < inputs->size(); ++i)
        {
          const json::Value &in = (*inputs)[i];
          ShaderVertexInput v;
          const std::string_view sem = in.GetString("semantic");
          if (sem.size() < 5 || sem.substr(0, 4) != "ATTR")
          {
            s.inputs_unbound = true; // declaration-usage semantics: an unbound container VS
            continue;
          }
          v.attr = uint32_t(std::atoi(std::string(sem.substr(4)).c_str()));
          if (!in.GetBool("bound", true))
          {
            s.inputs_unbound = true;
          }
          v.format = DxgiFromName(in.GetString("dxgi_format"));
          if (v.format == DXGI_FORMAT_UNKNOWN)
          {
            s.inputs_unbound = true;
          }
          v.offset = uint32_t(in.GetInt("offset_dwords")) * 4;
          v.stride = uint32_t(in.GetInt("stride_dwords")) * 4;
          v.fetch_slot = uint32_t(in.GetInt("slot", 95));
          v.stream = v.fetch_slot <= 95 && v.fetch_slot >= 80 ? 95 - v.fetch_slot : 0;
          if (v.stream < 16)
          {
            s.stream_mask |= 1u << v.stream;
            if (v.stride)
            {
              s.stream_stride[v.stream] = v.stride;
            }
          }
          s.inputs.push_back(v);
          s.input_fetch_address.push_back(uint32_t(in.GetInt("vfetch_address", -1)));
        }
      }
      s.writes_point_size = doc.GetBool("writes_point_size");
      s.vertex_texture_fetch = doc.GetBool("vertex_texture_fetch");

      s.color_outputs = ParseHexOrNumber(doc.Get("color_outputs"), 1);
      s.writes_depth = doc.GetBool("writes_depth");
      s.uses_kill = doc.GetBool("uses_kill");

      if (const json::Value *tex = doc.Get("textures"); tex && tex->IsArray())
      {
        for (size_t i = 0; i < tex->size(); ++i)
        {
          const json::Value &t = (*tex)[i];
          ShaderTexture st;
          st.slot = uint32_t(t.GetInt("slot"));
          st.dim = DimFromName(t.GetString("dimension"));
          if (st.slot < 32)
          {
            s.textures.push_back(st);
          }
        }
      }
      if (const json::Value *samp = doc.Get("samplers"); samp && samp->IsArray())
      {
        for (size_t i = 0; i < samp->size(); ++i)
        {
          const json::Value &t = (*samp)[i];
          ShaderSampler ss;
          ss.index = uint32_t(t.GetInt("index"));
          ss.slot = uint32_t(t.GetInt("slot"));
          ss.dim = DimFromName(t.GetString("dimension"));
          ss.code = uint32_t(t.GetInt("code", 0x1FFF));
          if (ss.index < 32 && ss.slot < 32)
          {
            s.samplers.push_back(ss);
          }
        }
      }
      if (const json::Value *un = doc.Get("unsupported"); un && un->IsArray())
      {
        s.unsupported = uint32_t(un->size());
      }
      // Fetches with unnormalized coordinates (unnormalize2D / 3D divide by the bound view's size): checked against
      // resolve mirrors at pdz_native_scale > 1 only (native_draw.cpp BindSrvTable).
      if (const json::Value *fetches = doc.Get("texture_fetches"); fetches && fetches->IsArray())
      {
        for (size_t i = 0; i < fetches->size(); ++i)
        {
          const json::Value &f = (*fetches)[i];
          const int64_t slot = f.GetInt("slot", -1);
          if (slot >= 0 && slot < 32 && f.GetBool("unnormalized"))
          {
            s.unnormalized_slots |= 1u << uint32_t(slot);
          }
        }
      }
      // Loop constant indices as the microcode has them (the fork's cf.loopStart.loopId, no stage offset): the
      // draw path counts draws whose loops would be skipped (count 0) for the stats line.
      if (const json::Value *loops = doc.Get("loop_constants_used"); loops && loops->IsArray())
      {
        for (size_t i = 0; i < loops->size(); ++i)
        {
          const uint32_t n = (*loops)[i].AsU32(32);
          if (n < 32)
          {
            s.loop_constants_used |= 1u << n;
          }
        }
      }

      // Input layout identity: the IA description (formats / offsets / strides / slots), not the shader.
      if (s.is_vertex)
      {
        s.input_layout_hash = InputLayoutHash(s.inputs);
      }
      return true;
    }

    NativeShader *LoadNew(uint64_t hash, bool vertex, ShaderStats &stats)
    {
      NativeShader *s = new NativeShader();
      s->hash = hash;
      s->is_vertex = vertex;
      const std::string dxil_path = Locate(hash, ".dxil");
      const std::string json_path = Locate(hash, ".json");
      if (dxil_path.empty() || json_path.empty())
      {
        return s; // usable = false; the caller reports the miss (it has the microcode to dump)
      }
      s->source = dxil_path.substr(0, dxil_path.find_last_of("\\/"));
      std::string why;
      std::string text;
      json::Value doc;
      std::string err;
      if (!ReadFile(dxil_path, s->dxil) || s->dxil.size() < 4)
      {
        why = "cannot read " + dxil_path;
      }
      else if (!ReadText(json_path, text) || !json::Parse(text, doc, &err))
      {
        why = "sidecar " + json_path + ": " + err;
      }
      else if (!ParseSidecar(doc, *s, why))
      {
      }
      else if (s->is_vertex != vertex)
      {
        why = "sidecar type does not match the bound stage";
      }
      if (!why.empty())
      {
        REXLOG_WARN("pdz_native: shader {} ({}) unusable: {}", HashName(hash), vertex ? "vs" : "ps", why);
        ++stats.distinct_failed;
        s->dxil.clear();
        return s;
      }
      const std::string hlsl_path = json_path.substr(0, json_path.size() - 5) + ".hlsl";
      const uint32_t literal_count = LoadLiteralValues(doc, hlsl_path, *s);
      if (vertex)
      {
        LoadManifest();
        auto p = parents.find(hash);
        if (p != parents.end())
        {
          MergeParentLiterals(*s, p->second);
        }
      }
      FinalizeFloatConstants(doc, hlsl_path, hash, *s); // pdz_native_cb_used_only (drawcost)
      s->usable = true;
      ++stats.distinct_loaded;
      if (s->unsupported)
      {
        REXLOG_WARN("pdz_native: shader {} has {} unsupported sidecar entries (used anyway)", HashName(hash),
                    s->unsupported);
      }
      REXLOG_DEBUG("pdz_native: loaded {} {} ({} bytes DXIL, {} inputs, {} textures, {} samplers, {} literals{})",
                   vertex ? "vs" : "ps", HashName(hash), s->dxil.size(), s->inputs.size(), s->textures.size(),
                   s->samplers.size(), literal_count, s->inputs_unbound ? ", UNBOUND inputs" : "");
      return s;
    }

    void NoteMiss(uint64_t hash, bool vertex, const uint8_t *ucode, uint32_t bytes, ShaderStats &stats)
    {
      if (!hash || !missed.insert(hash).second)
      {
        return;
      }
      ++stats.distinct_missed;
      ResolveDir();
      std::string dumped;
      if (ucode && bytes)
      {
        std::error_code ec;
        const std::string misses_dir = dir + "\\misses";
        if (!misses_dir_ready)
        {
          std::filesystem::create_directories(misses_dir, ec);
          misses_dir_ready = true;
        }
        const std::string path = misses_dir + "\\" + HashName(hash) + ".bin";
        std::ofstream f(path, std::ios::binary);
        if (f && f.write(reinterpret_cast<const char *>(ucode), bytes))
        {
          dumped = path;
        }
      }
      REXLOG_WARN("pdz_native: shader MISS {} ({}, {} bytes microcode){}{}", HashName(hash), vertex ? "vs" : "ps",
                  bytes, dumped.empty() ? "" : " dumped to ", dumped);
    }

    // pdz_native_translate_sync: waits (bounded) for the translation of `hash`, already requested. Returns the
    // translator status afterwards (1 done, -1 failed, 0 still running: the caller skips the draw as before). No wait
    // when the policy is off, after an earlier timeout for this shader, or when the frame's budget is spent.
    int WaitForTranslation(uint64_t hash, bool vertex, uint64_t parent, uint32_t declaration, ShaderStats &stats)
    {
      int status = translator.Status(hash);
      if (status != 0 || !SyncTranslateOn() || sync_gave_up.count(hash))
      {
        return status;
      }
      if (sync_frame != frame)
      {
        sync_frame = frame;
        sync_frame_ms = 0;
      }
      const uint32_t per_shader = uint32_t(std::max(0, REXCVAR_GET(pdz_native_translate_sync_ms)));
      const uint32_t per_frame = uint32_t(std::max(0, REXCVAR_GET(pdz_native_translate_sync_frame_ms)));
      const uint32_t bound = std::min(per_shader, per_frame > sync_frame_ms ? per_frame - sync_frame_ms : 0u);
      if (bound == 0)
      {
        ++stats.sync_over_budget;
        return status;
      }
      ++stats.sync_waits;
      const auto t0 = std::chrono::steady_clock::now();
      {
        const pdz::timing::PhaseScope phase(pdz::timing::Phase::kShaderWait); // pdz_frame_timing_phases
        watchdog::Wait marked("native shader translation (pdz_native_translate_sync)", hash);
        status = translator.WaitFor(hash, bound);
      }
      const uint32_t ms = uint32_t(
          std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
      sync_frame_ms += ms;
      stats.sync_ms_total += ms;
      stats.sync_ms_max = std::max(stats.sync_ms_max, ms);
      const std::string what = vertex && parent ? fmt::format("VS {} decl {:08X}: bound variant {}", HashName(parent),
                                                              declaration, HashName(hash))
                                                : fmt::format("{} {}", vertex ? "VS" : "PS", HashName(hash));
      if (status == 1)
      {
        ++stats.sync_done;
        if (++sync_logged <= 64)
        {
          REXLOG_INFO("pdz_native: {}: translated synchronously, the draw waited {} ms (pdz_native_translate_sync; "
                      "frame {}: {} ms waited){}",
                      what, ms, frame, sync_frame_ms, sync_logged == 64 ? "; further ones only in the counters" : "");
        }
      }
      else if (status == 0)
      {
        ++stats.sync_timeouts;
        sync_gave_up.insert(hash);
        REXLOG_WARN("pdz_native: {}: translation not done after {} ms (pdz_native_translate_sync_ms {}, frame budget "
                    "{} ms): its draws are skipped until it is, no further waits for it",
                    what, ms, per_shader, per_frame);
      }
      else
      {
        ++stats.sync_failed;
        REXLOG_WARN("pdz_native: {}: translation failed after a {} ms wait (pdz_native_translate_sync)", what, ms);
      }
      return status;
    }

    // pdz_native_shader_record: one line of the session's record file (opened on the first line).
    void RecordLine(const std::string &line)
    {
      if (!record_checked)
      {
        record_checked = true;
        if (!REXCVAR_GET(pdz_native_shader_record))
        {
          return;
        }
        ResolveDir();
        record_dir = dir + "\\record";
        std::error_code ec;
        std::filesystem::create_directories(record_dir, ec);
        const std::time_t now = std::time(nullptr);
        std::tm tm{};
        localtime_s(&tm, &now);
        char stamp[32] = {};
        std::strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &tm);
        record_path = fmt::format("{}\\bound_{}_{}.txt", record_dir, stamp, GetCurrentProcessId());
        record.open(record_path, std::ios::out | std::ios::trunc);
        if (!record)
        {
          REXLOG_WARN("pdz_native: pdz_native_shader_record: cannot write {}", record_path);
          return;
        }
        record << "# pdz_native_shader_record " << stamp << " | backend " << (pdz::gpusync::NullBackend() ? "null" : "plugin")
               << " | shader dir " << dir << " | fallback " << (fallback.empty() ? std::string("none") : fallback) << "\n"
               << "# vs <created VS> <declaration> <bound variant> <how> <source dir> | ps <hash> <how> <source dir>\n";
        REXLOG_INFO("pdz_native: recording the bound shaders to {} (pdz_native_shader_record)", record_path);
      }
      if (record)
      {
        record << line << "\n";
        record_dirty = true;
      }
    }

    // A (created VS, declaration) pair settled: `how` = cache, sibling, translated, created, missed. A sibling-built
    // variant has no files of its own: its microcode goes beside the record for the warm-cache tool.
    void RecordVertex(uint64_t created, uint32_t declaration, uint64_t bound, const char *how, const NativeShader *s,
                      const uint8_t *ucode = nullptr, uint32_t bytes = 0)
    {
      if (record_checked && !record)
      {
        return;
      }
      RecordLine(fmt::format("vs {} {:08X} {} {} {}", HashName(created), declaration, HashName(bound), how,
                             s && !s->source.empty() ? s->source : std::string("-")));
      if (record && ucode && bytes)
      {
        const std::string path = record_dir + "\\" + HashName(bound) + ".bin";
        std::error_code ec;
        if (!std::filesystem::exists(path, ec))
        {
          std::ofstream f(path, std::ios::binary);
          f.write(reinterpret_cast<const char *>(ucode), bytes);
        }
      }
    }
  };

  ShaderCache &ShaderCache::Get()
  {
    static ShaderCache instance;
    return instance;
  }

  ShaderCache::Impl &ShaderCache::impl()
  {
    if (!impl_)
    {
      impl_ = new Impl();
      std::atexit([]() { ShaderCache::Get().LogCoverage("exit"); });
    }
    return *impl_;
  }

  const std::string &ShaderCache::fallback_directory()
  {
    Impl &im = impl();
    im.ResolveDir();
    return im.fallback;
  }

  const std::string &ShaderCache::directory()
  {
    Impl &im = impl();
    im.ResolveDir();
    return im.dir;
  }

  const NativeShader *ShaderCache::Load(uint64_t hash, bool vertex)
  {
    Impl &im = impl();
    auto it = im.loaded.find(hash);
    if (it != im.loaded.end())
    {
      return it->second;
    }
    const auto t0 = std::chrono::steady_clock::now();
    NativeShader *s = im.LoadNew(hash, vertex, stats_);
    if (s)
    {
      s->load_ms = float(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    }
    im.loaded.emplace(hash, s);
    if (!vertex)
    {
      // pdz_native_shader_record: a pixel shader at its first lookup (again after a translation made it loadable).
      im.RecordLine(fmt::format("ps {} {} {}", HashName(hash), s && s->usable ? "cache" : "missing",
                                s && s->usable && !s->source.empty() ? s->source : std::string("-")));
    }
    return s;
  }

  std::string ShaderCache::FloatConstantScanText()
  {
    const FloatScanStats &st = g_float_scan;
    return fmt::format("shaders trimmed {} (sidecar lists completed from the HLSL {}), whole block {} (relative {}, no "
                       "list {}, no HLSL {}, scan {})",
                       st.trimmed, st.hlsl_extra, st.relative + st.no_list + st.no_hlsl + st.scan, st.relative,
                       st.no_list, st.no_hlsl, st.scan);
  }

  const NativeShader *ShaderCache::PreparePixel(uint64_t hash, NativeShader *copy)
  {
    const NativeShader *s = Load(hash, false);
    Impl &im = impl();
    auto it = im.loaded.find(hash);
    if (!s || !s->usable || it == im.loaded.end() || it->second != s)
    {
      return nullptr;
    }
    NativeShader *target = it->second;
    if (copy)
    {
      *copy = *s;
      target = copy;
    }
    if (target->coissue_checked)
    {
      return target;
    }
    const auto read = [](const std::string &path, std::vector<uint8_t> &out) {
      std::ifstream f(path, std::ios::binary);
      if (f)
      {
        out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
      }
    };
    std::vector<uint8_t> ucode, container;
    read(s->source + "\\" + HashName(hash) + ".bin", ucode);
    read(s->source + "\\containers\\" + HashName(hash) + ".bin", container);
    const uint32_t bytes = ucode.size() <= kMaxUcodeBytes ? uint32_t(ucode.size()) : 0;
    if (!bytes || XXH3_64bits(ucode.data(), bytes) != hash)
    {
      return target; // no microcode file beside it (a research-cache shader): the first draw checks it, as before
    }
    coissue::Apply(*target, ucode.data(), bytes, container.empty() ? nullptr : container.data(),
                   uint32_t(container.size()));
    return target;
  }

  const NativeShader *ShaderCache::NullPixel()
  {
    static const NativeShader null_ps = [] {
      NativeShader s;
      s.hash = 0;
      s.usable = true;
      s.color_outputs = 0;
      s.source = "(no pixel shader)";
      s.float_constants_all = false; // no pixel stage: nothing reads the PS float constants (pdz_native_cb_used_only)
      return s;
    }();
    return &null_ps;
  }

  const NativeShader *ShaderCache::FindPixel(const uint8_t *base, const shadow::ShaderEntry *entry)
  {
    if (!entry || !entry->hash)
    {
      ++stats_.misses;
      return nullptr;
    }
    const NativeShader *s = Load(entry->hash, false);
    if (s && s->usable)
    {
      if (!s->coissue_checked)
      {
        // Once per pixel shader (native_coissue.h): co-issued scalar operations must read the registers as they were
        // before the instruction, and a PsParamGen register must hold the pixel parameters; a shader the fork
        // translated otherwise is recompiled here before its first PSO. The container (header right before the
        // microcode: its program-info block says whether PsParamGen is on) is looked up only while unchecked.
        const auto t0 = std::chrono::steady_clock::now();
        const uint32_t compiles = depth20e4::CompileCount();
        Impl &im = impl();
        auto it = im.loaded.find(entry->hash);
        const uint8_t *ucode =
            shadow::IsGuestPointer(entry->ucode_src) ? shadow::GuestPtr(base, entry->ucode_src) : nullptr;
        if (it != im.loaded.end() && it->second == s)
        {
          const uint32_t bytes = entry->ucode_bytes <= kMaxUcodeBytes ? entry->ucode_bytes : 0;
          const uint32_t container = ucode && bytes ? binder::FindContainer(base, *entry) : 0;
          coissue::Apply(*it->second, ucode, bytes, container ? shadow::GuestPtr(base, container) : nullptr,
                         container ? entry->ucode_src - container + bytes : 0);
        }
        // pdz_native_first_use_log: the shader's first draw (its file load was timed by Load).
        const double patch_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        const double total_ms = patch_ms + double(s->load_ms);
        ++stats_.first_use_ps;
        stats_.first_use_ps_ms += total_ms;
        stats_.first_use_ps_max_ms = std::max(stats_.first_use_ps_max_ms, total_ms);
        if (REXCVAR_GET(pdz_native_first_use_log) && im.first_use_ps_logged < kFirstUseLogLines)
        {
          ++im.first_use_ps_logged;
          const uint32_t compiled = depth20e4::CompileCount() - compiles;
          REXLOG_INFO("pdz_native: PS first use {}: {} in {:.1f} ms (file load {:.1f} ms, patch check {:.1f} ms{}){}",
                      im.first_use_ps_logged, HashName(entry->hash), total_ms, s->load_ms, patch_ms,
                      compiled ? fmt::format(", {} compile(s): not in the disk cache", compiled) : std::string(),
                      im.first_use_ps_logged == kFirstUseLogLines
                          ? "; further ones only in the periodic shader cache line"
                          : "");
        }
      }
      ++stats_.hits;
      return s;
    }
    Impl &im = impl();
    const uint8_t *ucode = shadow::IsGuestPointer(entry->ucode_src) ? shadow::GuestPtr(base, entry->ucode_src) : nullptr;
    // The microcode as it was at create time (2026-11-03, log 597: the video player's PS c2bdc141db1b3bff had other
    // bytes in its buffer by the time of the miss; translating those made garbage).
    if (const std::vector<uint8_t> *kept = shadow::FindPixelUcode(entry->object);
        kept && kept->size() == entry->ucode_bytes && XXH3_64bits(kept->data(), kept->size()) == entry->hash)
    {
      ucode = kept->data();
    }
    const bool translatable =
        ucode && entry->ucode_bytes && entry->ucode_bytes <= kMaxUcodeBytes && im.translator.Available();
    if (translatable && Impl::SyncTranslateOn() && !im.sync_gave_up.count(entry->hash))
    {
      // pdz_native_translate_sync (graphics_backend null): no emulator frame behind this one, so the draw waits for
      // the translation (bounded) instead of leaving a hole while it runs. Status 0 afterwards (timeout, budget) or -1
      // (failed): the path below as before.
      im.ResolveDir();
      if (im.ps_translating.insert(entry->hash).second)
      {
        im.translator.Request(im.dir, entry->hash, false, ucode, entry->ucode_bytes, true);
        ++stats_.translations_requested;
      }
      if (im.WaitForTranslation(entry->hash, false, 0, 0, stats_) == 1)
      {
        im.ForgetUnusable(entry->hash);
        const NativeShader *t = Load(entry->hash, false);
        if (t && t->usable)
        {
          im.ps_translating.erase(entry->hash);
          ++stats_.translations_done;
          return FindPixel(base, entry); // loaded now: the co-issue check and the hit count above, no recursion beyond
        }
        im.sync_gave_up.insert(entry->hash); // translated but not loadable: the path below, no more waits
      }
    }
    ++stats_.misses;
    if (translatable)
    {
      im.ResolveDir();
      if (im.ps_translating.insert(entry->hash).second)
      {
        im.translator.Request(im.dir, entry->hash, false, ucode, entry->ucode_bytes);
        ++stats_.translations_requested;
      }
      else if (im.translator.Status(entry->hash) == 1)
      {
        im.ForgetUnusable(entry->hash);
        im.ps_translating.erase(entry->hash);
        ++stats_.translations_done;
        return nullptr; // the next draw loads it
      }
      else if (im.translator.Status(entry->hash) == -1)
      {
        im.ps_translating.erase(entry->hash);
        ++stats_.translations_failed;
        im.NoteMiss(entry->hash, false, ucode, entry->ucode_bytes, stats_);
      }
      return nullptr;
    }
    im.NoteMiss(entry->hash, false, ucode, entry->ucode_bytes, stats_);
    return nullptr;
  }

  namespace
  {
    // The microcode copy D3D keeps per shader object ([object+12]: a pointer to the physical-memory copy that
    // receives the vfetch binding patch). Returns nullptr when the field is not a plausible pointer.
    const uint8_t *BoundCopy(const uint8_t *base, uint32_t object, uint32_t bytes)
    {
      if (!shadow::IsGuestPointer(object) || bytes == 0 || bytes > kMaxUcodeBytes)
      {
        return nullptr;
      }
      // Log 005 (stage 1, frontend): [object+12] gave 84 bytes of non-microcode for the boot VS. The copy the
      // state flush IM_LOADs is the device's current vertex program object (device+12948: +40 ucode pointer,
      // +600 byte count; shadow_assert.cpp, sub_826FA2E8). Prefer it when its size matches this shader.
      {
        // pdz_native_record_thread: +12948 from the worker's device mirror and the program's fields from the hook's
        // copies (native_view.h), as they were at the draw.
        const uint32_t device = shadow::Load32(base, shadow::dev::kDevicePointerGlobal);
        const uint32_t program = shadow::IsGuestPointer(device) ? view::Load32(base, device + 12948) : 0;
        if (shadow::IsGuestPointer(program) && view::Load32(base, program + 600) == bytes)
        {
          const uint32_t pptr = view::Load32(base, program + 40);
          static bool logged = false;
          if (!logged)
          {
            logged = true;
            REXLOG_INFO("pdz_native: bound VS microcode read from the device's vertex program object {:08X} (+40 = {:08X}, "
                        "+600 = {} bytes)",
                        program, pptr, bytes);
          }
          if (pptr && pptr < 0x20000000u)
          {
            return PhysPtr(pptr & ~3u);
          }
          if (shadow::IsGuestPointer(pptr))
          {
            return shadow::GuestPtr(base, pptr);
          }
        }
      }
      const uint32_t ptr = view::Load32(base, object + shadow::dev::kShaderUcode);
      if (ptr == 0)
      {
        return nullptr;
      }
      if (ptr < 0x20000000u)
      {
        return PhysPtr(ptr & ~3u); // physical address (low bits: GPU type bits if any)
      }
      if (shadow::IsGuestPointer(ptr))
      {
        return shadow::GuestPtr(base, ptr);
      }
      return nullptr;
    }

    // The created (unpatched) microcode of a VS object: the hook's kept copy, else the create-time source.
    const uint8_t *ParentUcode(const uint8_t *base, uint32_t object, const shadow::ShaderEntry *entry)
    {
      const std::vector<uint8_t> *kept = shadow::FindVertexUcode(object);
      if (kept && kept->size() == entry->ucode_bytes)
      {
        return kept->data();
      }
      return shadow::IsGuestPointer(entry->ucode_src) ? shadow::GuestPtr(base, entry->ucode_src) : nullptr;
    }

    // A bound copy is a variant of this VS only if its first control-flow triple (dwords 0-2) equals the created
    // microcode's: D3D's vfetch binding (and the binder) patch fetch instructions only, never control flow, and the
    // first exec's instructions start at triple 1 at the earliest. BoundCopy prefers the device's current vertex
    // program object whenever its size matches, and that can be another shader's buffer or freed memory: Subway
    // (log 087) read 192 zero bytes (one stray dword 7F100000) for VS 8cbc4754... decl 40D5AB00, took them as
    // D3D's copy (no unbound vfetch, not the created hash), and their translation failed ("loop not closed"), so
    // the pair missed for 38 frames although the binder's output (475c5932..., an xsh-bound harvest) was cached.
    bool CopyIsVariantOf(const uint8_t *copy, const uint8_t *parent, uint32_t bytes)
    {
      if (!copy || !parent)
      {
        return copy != nullptr; // nothing to check against
      }
      return std::memcmp(copy, parent, bytes < 12 ? bytes : 12) == 0;
    }
  } // namespace

  void ShaderCache::CompareWithD3D(VsBindingRef bref, const uint8_t *copy, const shadow::ShaderEntry *entry,
                                   uint32_t declaration)
  {
    VsBinding &b = *static_cast<VsBinding *>(bref);
    b.binder_checked = true;
    const uint64_t bound_hash = XXH3_64bits(copy, entry->ucode_bytes);
    if (b.binder_hash == bound_hash)
    {
      ++stats_.binder_matches;
      REXLOG_INFO("pdz_native: binder reproduced D3D's bound copy for VS {} decl {:08X}: {}", HashName(entry->hash),
                  declaration, HashName(bound_hash));
      return;
    }
    ++stats_.binder_mismatches;
    uint32_t first = 0;
    for (; first < entry->ucode_bytes / 4; ++first)
    {
      if (std::memcmp(copy + 4 * first, b.binder_bytes.data() + 4 * first, 4) != 0)
      {
        break;
      }
    }
    const auto be = [](const uint8_t *p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; };
    REXLOG_WARN("pdz_native: binder MISMATCH for VS {} decl {:08X}: ours {} vs D3D {}; first differing dword {} "
                "(ours {:08X}, D3D {:08X})",
                HashName(entry->hash), declaration, HashName(b.binder_hash), HashName(bound_hash), first,
                first < entry->ucode_bytes / 4 ? be(b.binder_bytes.data() + 4 * first) : 0,
                first < entry->ucode_bytes / 4 ? be(copy + 4 * first) : 0);
  }

  const NativeShader *ShaderCache::DeriveVertexVariant(uint64_t hash, uint64_t parent, const uint8_t *ucode,
                                                       uint32_t bytes, uint32_t declaration)
  {
    constexpr uint32_t kMaxSiblingReads = 32; // microcode files read per variant (cold path, once per hash)
    Impl &im = impl();
    if (!hash || !parent || !ucode || bytes < 12 || (bytes % 12) != 0 || bytes > kMaxUcodeBytes ||
        im.derive_failed.count(hash))
    {
      return nullptr;
    }
    im.LoadManifest();
    // The other bound variants of the same created VS: the manifest's (derived_from) and those the translator or this
    // function produced this session; the ones already loaded first.
    std::vector<uint64_t> siblings;
    for (const auto &[h, p] : im.parents)
    {
      if (p == parent && h != hash)
      {
        siblings.push_back(h);
      }
    }
    std::stable_partition(siblings.begin(), siblings.end(), [&im](uint64_t h) {
      const auto it = im.loaded.find(h);
      return it != im.loaded.end() && it->second && it->second->usable;
    });
    std::vector<uint8_t> sib_ucode;
    std::vector<VertexFetchField> cand_fetches, sib_fetches;
    const char *why = "no cached bound variant of the same VS";
    uint32_t reads = 0;
    for (const uint64_t h : siblings)
    {
      if (reads >= kMaxSiblingReads)
      {
        break;
      }
      const std::string path = im.LocateUcode(h);
      if (path.empty() || !ReadFile(path, sib_ucode))
      {
        continue;
      }
      ++reads;
      if (sib_ucode.size() != bytes)
      {
        why = "no sibling of the same size";
        continue;
      }
      if (!DiffersOnlyInLayout(ucode, sib_ucode.data(), bytes, cand_fetches, sib_fetches, &why))
      {
        continue;
      }
      const NativeShader *s = Load(h, true);
      if (!s || !s->usable || s->inputs_unbound || !s->is_vertex || s->input_fetch_address.size() != s->inputs.size())
      {
        why = "the sibling's shader is not usable";
        continue;
      }
      // Each input at its first fetch: the sibling's sidecar must say what our walk of its microcode says (else the
      // walk is not the fork's and nothing is derived); the variant's offset / stride come from its own fetch.
      NativeShader *d = new NativeShader(*s);
      bool ok = true;
      for (size_t k = 0; k < d->inputs.size() && ok; ++k)
      {
        ok = false;
        for (size_t f = 0; f < sib_fetches.size(); ++f)
        {
          if (sib_fetches[f].addr != s->input_fetch_address[k])
          {
            continue;
          }
          ok = sib_fetches[f].offset >= 0 && cand_fetches[f].offset >= 0 &&
               uint32_t(sib_fetches[f].offset) * 4 == s->inputs[k].offset && sib_fetches[f].stride * 4 == s->inputs[k].stride;
          d->inputs[k].offset = uint32_t(cand_fetches[f].offset) * 4;
          d->inputs[k].stride = cand_fetches[f].stride * 4;
          break;
        }
      }
      if (!ok)
      {
        delete d;
        why = "the sibling's sidecar does not match its fetches";
        continue;
      }
      d->hash = hash;
      d->stream_mask = 0;
      std::memset(d->stream_stride, 0, sizeof(d->stream_stride));
      for (const ShaderVertexInput &v : d->inputs)
      {
        if (v.stream < 16)
        {
          d->stream_mask |= 1u << v.stream;
          if (v.stride)
          {
            d->stream_stride[v.stream] = v.stride;
          }
        }
      }
      d->input_layout_hash = InputLayoutHash(d->inputs);
      im.ForgetUnusable(hash); // the failed lookup of `hash` (no files) is remembered as unusable
      auto [it, inserted] = im.loaded.emplace(hash, d);
      if (!inserted)
      {
        delete d; // a usable entry appeared meanwhile (cannot happen on the draw thread); keep it
        return it->second && it->second->usable ? it->second : nullptr;
      }
      im.parents[hash] = parent;
      ++stats_.sibling_derived;
      ++stats_.distinct_loaded;
      if (stats_.sibling_derived <= 32)
      {
        std::string layout;
        for (size_t k = 0; k < d->inputs.size(); ++k)
        {
          layout += fmt::format("{}ATTR{} +{} /{}", k ? ", " : "", d->inputs[k].attr, d->inputs[k].offset, d->inputs[k].stride);
        }
        REXLOG_INFO("pdz_native: VS {} decl {:08X}: bound variant {} built from the cached {} (only vfetch strides / "
                    "offsets differ; {}): no translation{}",
                    HashName(parent), declaration, HashName(hash), HashName(h), layout,
                    stats_.sibling_derived == 32 ? "; further ones only in the counters" : "");
      }
      return d;
    }
    im.derive_failed.insert(hash);
    REXLOG_INFO("pdz_native: VS {} decl {:08X}: bound variant {} not derivable from a cached sibling ({}; {} of {} "
                "read)",
                HashName(parent), declaration, HashName(hash), why, reads, siblings.size());
    return nullptr;
  }

  const NativeShader *ShaderCache::FindVertex(const uint8_t *base, uint32_t object, uint32_t declaration,
                                              const shadow::ShaderEntry *entry, const shadow::DrawRecord *record)
  {
    Impl &im = impl();
    const uint64_t key = (uint64_t(object) << 32) | declaration;
    auto it = im.bindings.find(key);
    // A later level can create another VS at a freed object's address (review finding 1): a binding made for a
    // different create-time hash starts over.
    if (it != im.bindings.end() && entry && entry->hash && it->second.created_hash != entry->hash)
    {
      it->second = VsBinding();
      it->second.created_hash = entry->hash;
    }
    if (it != im.bindings.end() && it->second.state == VsBinding::kResolved)
    {
      VsBinding &rb = it->second;
      rb.last_used_frame = im.frame;
      // Deferred cross-check: D3D binds the copy inside the draw's state flush, after this hook, so the
      // comparison happens on a later draw of the same pair (bounded attempts).
      if (!rb.binder_checked && rb.binder_hash && entry && rb.check_attempts++ < 64)
      {
        const uint8_t *later = BoundCopy(base, object, entry->ucode_bytes);
        const bool later_blank = later && REXCVAR_GET(pdz_native_vs_blank_copy_unbound) && later[0] == 0 &&
                                 later[1] == 0 && later[2] == 0 && later[3] == 0;
        if (later && !later_blank && !HasUnboundVfetch(later, entry->ucode_bytes) &&
            CopyIsVariantOf(later, ParentUcode(base, object, entry), entry->ucode_bytes))
        {
          CompareWithD3D(&rb, later, entry, declaration);
        }
      }
      // pdz_native_vs_binding_cap: the binder's copy of a resolved pair is read only by that cross-check (and, before
      // resolution, by a translation request, which writes it to disk at once); once the check ran or gave up it goes.
      if (!rb.binder_bytes.empty() && REXCVAR_GET(pdz_native_vs_binding_cap) > 0 &&
          (rb.binder_checked || !rb.binder_hash || rb.check_attempts >= 64))
      {
        im.binder_bytes_released += rb.binder_bytes.size();
        std::vector<uint8_t>().swap(rb.binder_bytes);
      }
      ++stats_.hits;
      return rb.shader;
    }
    if (!entry || !entry->hash)
    {
      ++stats_.misses;
      return nullptr;
    }
    if (it == im.bindings.end())
    {
      it = im.bindings.emplace(key, VsBinding()).first;
      it->second.created_hash = entry->hash;
      ++stats_.vs_bindings;
    }
    VsBinding &b = it->second;
    b.last_used_frame = im.frame;
    // pdz_native_first_use_log: the time of this unresolved pair's lookups (binder, file load, sibling build, the
    // pending ones too), counted and logged when it resolves. `b` stays valid: nothing below inserts or erases a pair.
    struct FirstUseScope
    {
      ShaderCache &cache;
      VsBinding &binding;
      uint64_t created;
      uint32_t declaration;
      uint32_t loaded_before;
      std::chrono::steady_clock::time_point t0;
      ~FirstUseScope()
      {
        binding.first_use_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (binding.state != VsBinding::kResolved)
        {
          return;
        }
        ShaderStats &st = cache.stats_;
        ++st.first_use_vs;
        st.first_use_vs_ms += binding.first_use_ms;
        st.first_use_vs_max_ms = std::max(st.first_use_vs_max_ms, binding.first_use_ms);
        Impl &im = cache.impl();
        if (!REXCVAR_GET(pdz_native_first_use_log) || im.first_use_vs_logged >= kFirstUseLogLines)
        {
          return;
        }
        ++im.first_use_vs_logged;
        REXLOG_INFO("pdz_native: VS binding first use {}: VS {} decl {:08X} -> {} in {:.1f} ms{}{}",
                    im.first_use_vs_logged, HashName(created), declaration, HashName(binding.hash),
                    binding.first_use_ms, st.distinct_loaded != loaded_before ? " (shader file loaded)" : "",
                    im.first_use_vs_logged == kFirstUseLogLines
                        ? "; further ones only in the periodic shader cache line"
                        : "");
      }
    } first_use{*this, b, entry->hash, declaration, stats_.distinct_loaded, std::chrono::steady_clock::now()};
    if (b.state == VsBinding::kMissed)
    {
      ++stats_.misses;
      return nullptr;
    }

    // D3D's bound copy (the device's current vertex program object) and its hash: the .xsh key.
    const uint8_t *copy = BoundCopy(base, object, entry->ucode_bytes);
    if (copy && HasUnboundVfetch(copy, entry->ucode_bytes))
    {
      // D3D binds during the draw's state flush, after this hook ran: the copy is not the bound code yet
      // (frontend video VS, log 009). Only the binder's output can be translated for such a pair.
      if (!b.binder_checked)
      {
        REXLOG_DEBUG("pdz_native: VS {} decl {:08X}: D3D's copy still has unbound vfetch", HashName(entry->hash), declaration);
      }
      copy = nullptr;
    }
    if (copy && REXCVAR_GET(pdz_native_vs_blank_copy_unbound) && copy[0] == 0 && copy[1] == 0 && copy[2] == 0 &&
        copy[3] == 0)
    {
      // A blank first control-flow dword is not a shader (an exec at address 0 would overlap the CF itself): D3D
      // has not written its bound copy yet. Logs 069-090: VS 5cc84ad7348d21c2 decl 40D5AA40 compared against such
      // a copy (D3D hash different every run, dword 0 = 00000000), the hash missed the cache and the pair
      // resolved to the UNPATCHED container (32_FLOAT stride 0 at fetch slots 95/94/93), so every BeginVertices
      // draw of caller 825F1260 read streams 1 and 2 with no buffer (stream-no-buffer) and a 1-float position.
      // The binder's output (78943e1e1f5f18ae: stream 0 stride 36 = BeginVertices' stride) is the bound code.
      if (!b.logged_blank_copy)
      {
        b.logged_blank_copy = true;
        REXLOG_INFO("pdz_native: VS {} decl {:08X}: D3D's copy is blank (first CF dword 0); treated as not bound "
                    "yet, the binder's output is used",
                    HashName(entry->hash), declaration);
      }
      copy = nullptr;
    }
    if (copy && !CopyIsVariantOf(copy, ParentUcode(base, object, entry), entry->ucode_bytes))
    {
      // Not this shader's code (another VS of the same size, or freed memory): treat it like a copy D3D has not
      // patched yet, so the binder's output is looked up instead of translating garbage.
      if (!b.logged_foreign)
      {
        b.logged_foreign = true;
        const auto be = [](const uint8_t *p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; };
        REXLOG_INFO("pdz_native: VS {} decl {:08X}: D3D's copy is not a variant of this shader (dword 0 {:08X}); "
                    "ignoring it",
                    HashName(entry->hash), declaration, be(copy));
      }
      copy = nullptr;
    }
    const uint64_t bound_hash = copy ? XXH3_64bits(copy, entry->ucode_bytes) : 0;

    // The binder's reproduction from the ring's declaration and stream strides (once per pair).
    if (!b.binder_done && record && entry->ucode_bytes && entry->ucode_bytes <= kMaxUcodeBytes)
    {
      b.binder_done = true;
      const shadow::DeclEntry *decl = shadow::FindDeclaration(declaration);
      if (!decl && shadow::IsGuestPointer(declaration))
      {
        // Declarations built through the FVF path (sub_826E2A90, video / UI) never pass the hooked
        // CreateVertexDeclaration: read the D3D object's own element table now (same layout the hook copies).
        shadow::NoteDeclaration(const_cast<uint8_t *>(base), declaration, declaration + shadow::dev::kDeclElements);
        decl = shadow::FindDeclaration(declaration);
      }
      const uint8_t *parent = ParentUcode(base, object, entry);
      const uint32_t container = entry->vs_table_count ? 0 : binder::FindContainer(base, *entry);
      binder::TableEntry table[32];
      binder::Element elements[16];
      uint32_t table_count = 0;
      for (uint32_t e = 0; e < entry->vs_table_count && e < 32; ++e)
      {
        const uint32_t raw = entry->vs_table[e];
        table[table_count++] = {raw & 0xFFFu, (raw >> 12) & 0xFu, (raw >> 16) & 0xFu, (raw >> 20) & 0xFu};
      }
      if (table_count == 0 && container)
      {
        table_count = binder::ReadTable(base, container, table, 32);
      }
      const uint32_t decl_count = decl ? binder::ParseDeclaration(*decl, elements, 16) : 0;
      uint32_t strides[shadow::dev::kStreamCount];
      for (uint32_t s = 0; s < shadow::dev::kStreamCount; ++s)
      {
        strides[s] = record->streams[s].stride;
      }
      // DrawVerticesUP (a3), BeginVertices (a2) and DrawIndexedVerticesUP (a7) pass the stream-0 stride as an
      // argument; no stream is set.
      if (record->kind == shadow::Kind::kDrawVerticesUP)
      {
        strides[0] = record->a[3];
      }
      else if (record->kind == shadow::Kind::kBeginVertices)
      {
        strides[0] = record->a[2];
      }
      else if (record->kind == shadow::Kind::kDrawIndexedVerticesUP)
      {
        strides[0] = record->a[7];
      }
      std::string why;
      if (parent && table_count && decl_count &&
          binder::Bind(parent, entry->ucode_bytes, table, table_count, elements, decl_count, strides,
                       shadow::dev::kStreamCount, b.binder_bytes, &why))
      {
        b.binder_hash = XXH3_64bits(b.binder_bytes.data(), b.binder_bytes.size());
        if (!why.empty())
        {
          REXLOG_WARN("pdz_native: binder VS {} decl {:08X}: {}", HashName(entry->hash), declaration, why);
        }
      }
      else
      {
        std::string before;
        for (uint32_t e = 0; e < table_count; ++e)
        {
          before += fmt::format("[table @{} u{}.{} hi{}] ", table[e].addr, table[e].usage, table[e].index, table[e].hi);
        }
        for (uint32_t e = 0; e < decl_count; ++e)
        {
          before += fmt::format("[decl s{} +{} type {:08X} u{}.{}] ", elements[e].stream, elements[e].offset_bytes,
                                elements[e].type, elements[e].usage, elements[e].index);
        }
        if (decl)
        {
          for (uint32_t e = 0; e < decl->count && e < 4; ++e)
          {
            before += fmt::format("[raw {:08X} {:08X} {:08X}] ", _byteswap_ulong(decl->elements_be[e][0]),
                                  _byteswap_ulong(decl->elements_be[e][1]), _byteswap_ulong(decl->elements_be[e][2]));
          }
        }
        if (parent)
        {
          for (int32_t off = -48; off < 0; off += 4)
          {
            before += fmt::format("{:08X} ", shadow::Load32(base, entry->ucode_src + uint32_t(off)));
          }
        }
        REXLOG_WARN("pdz_native: binder could not bind VS {} (object {:08X}) to decl {:08X}: {} (container {:08X}, "
                    "table {} entries, declaration {} elements{}; entry magic {:08X} ucode_src {:08X} bytes {}; the 12 "
                    "dwords before the microcode: {})",
                    HashName(entry->hash), object, declaration, why.empty() ? "inputs missing" : why, container,
                    table_count, decl_count, decl ? "" : " - declaration object unreadable", entry->magic,
                    entry->ucode_src, entry->ucode_bytes, before);
      }
    }
    if (!b.binder_checked && copy && b.binder_hash)
    {
      CompareWithD3D(&b, copy, entry, declaration);
    }

    // The end of this pair's translation (status 1 done, else failed): resolved to it, or missed.
    const auto finish_translation = [&](int status) -> const NativeShader * {
      if (status == 1)
      {
        ++stats_.translations_done;
        im.ForgetUnusable(b.hash);
        im.parents[b.hash] = entry->hash; // the parent's literals apply to the bound variant
        const NativeShader *s = Load(b.hash, true);
        if (s && s->usable && !s->inputs_unbound)
        {
          b.shader = s;
          b.state = VsBinding::kResolved;
          im.RecordVertex(entry->hash, declaration, b.hash, "translated", s);
          ++stats_.hits;
          return s;
        }
        REXLOG_WARN("pdz_native: translated VS {} is not usable", HashName(b.hash));
      }
      else
      {
        ++stats_.translations_failed;
      }
      b.state = VsBinding::kMissed;
      im.RecordVertex(entry->hash, declaration, b.hash, "missed", nullptr);
      ++stats_.misses;
      return nullptr;
    };

    // Translation in flight for this pair? With pdz_native_translate_sync the draw waits for it (bounded; a second
    // pair of the same variant, or a later frame after the budget ran out).
    if (b.state == VsBinding::kTranslating)
    {
      const int status = im.WaitForTranslation(b.hash, true, entry->hash, declaration, stats_);
      if (status == 0)
      {
        ++stats_.pending;
        return nullptr;
      }
      return finish_translation(status);
    }

    // Cache lookup by the bound hash (D3D's copy first, else the binder's). A copy identical to the created
    // microcode is not a bound copy: D3D patches during the draw's state flush, after this hook, and the static exe
    // post-process VS (aa03f226..., decl 403C11D0: POSITION FLOAT3 + TEXCOORD5 FLOAT2) still read 32_FLOAT at
    // stride 0 there, so every full-screen quad was degenerate (pass dump run2). The binder's output wins then.
    const bool copy_is_unpatched = bound_hash && bound_hash == entry->hash;
    const uint64_t candidate_hash =
        (copy_is_unpatched && b.binder_hash && b.binder_hash != bound_hash) ? b.binder_hash
                                                                             : (bound_hash ? bound_hash : b.binder_hash);
    if (copy_is_unpatched && candidate_hash != bound_hash && !b.logged_unpatched)
    {
      b.logged_unpatched = true;
      REXLOG_INFO("pdz_native: VS {} decl {:08X}: D3D's copy is the unpatched microcode; using the binder's {}",
                  HashName(entry->hash), declaration, HashName(candidate_hash));
    }
    const NativeShader *candidate = nullptr;
    if (candidate_hash)
    {
      candidate = Load(candidate_hash, true);
      if (candidate && candidate->usable && !candidate->inputs_unbound)
      {
        b.hash = candidate_hash;
        b.shader = candidate;
        b.state = VsBinding::kResolved;
        im.RecordVertex(entry->hash, declaration, candidate_hash, "cache", candidate);
        ++stats_.hits;
        return candidate;
      }
      // Not cached, but a cached sibling may differ from it in the vfetch strides / offsets only: that sibling's
      // shader with this variant's input layout, at once (pdz_native_vs_sibling_variants). Before the wait for D3D's
      // copy below and the translator: those left the draws pending (Facility, log 190: 23 draws in 8 frames for two
      // stride-24 variants the research cache had at strides 28-52 bytes only).
      const uint8_t *candidate_ucode = nullptr;
      if (candidate_hash == b.binder_hash && b.binder_bytes.size() == entry->ucode_bytes)
      {
        candidate_ucode = b.binder_bytes.data();
      }
      else if (copy && candidate_hash == bound_hash)
      {
        candidate_ucode = copy;
      }
      if (candidate_ucode && REXCVAR_GET(pdz_native_vs_sibling_variants))
      {
        if (const NativeShader *derived =
                DeriveVertexVariant(candidate_hash, entry->hash, candidate_ucode, entry->ucode_bytes, declaration))
        {
          b.hash = candidate_hash;
          b.shader = derived;
          b.state = VsBinding::kResolved;
          im.RecordVertex(entry->hash, declaration, candidate_hash, "sibling", derived, candidate_ucode,
                          entry->ucode_bytes);
          ++stats_.hits;
          return derived;
        }
      }
    }
    // pdz_native_translate_sync: the binder's output is translated at once (a cached binder hash is taken without
    // D3D's copy above too; the deferred cross-check still logs a mismatch), no draws skipped waiting for the copy.
    // Not when the created VS is drawable by itself (an exe VS): the old wait, so a cached D3D variant still wins.
    const bool binder_ready = b.binder_hash && b.binder_bytes.size() == entry->ucode_bytes;
    const auto created_drawable = [&]() {
      const NativeShader *c = Load(entry->hash, true);
      return c && c->usable && !c->inputs_unbound;
    };
    if (!copy && !(binder_ready && Impl::SyncTranslateOn() && !created_drawable()) && b.retries++ < kMaxBindRetries)
    {
      // D3D has not patched this pair yet: wait a few draws so the binder's output can be checked against it.
      ++stats_.pending;
      return nullptr;
    }
    // The create-time hash (a container VS whose translation is drawable, e.g. an exe VS).
    const NativeShader *created = Load(entry->hash, true);
    if (created && created->usable && !created->inputs_unbound)
    {
      b.hash = entry->hash;
      b.shader = created;
      b.state = VsBinding::kResolved;
      im.RecordVertex(entry->hash, declaration, entry->hash, "created", created);
      ++stats_.hits;
      return created;
    }
    // Unseen variant: translate the bound microcode on first use.
    const bool use_binder = !b.binder_bytes.empty() && (!copy || candidate_hash == b.binder_hash);
    const uint8_t *ucode = use_binder ? b.binder_bytes.data() : copy;
    const uint64_t miss_hash = candidate_hash ? candidate_hash : entry->hash;
    if (ucode && candidate_hash && im.translator.Available())
    {
      im.ResolveDir();
      const bool sync = Impl::SyncTranslateOn();
      im.translator.Request(im.dir, candidate_hash, true, ucode, entry->ucode_bytes, sync);
      ++stats_.translations_requested;
      b.hash = candidate_hash;
      b.state = VsBinding::kTranslating;
      REXLOG_INFO("pdz_native: VS {} decl {:08X}: bound variant {} not in the cache, translating ({}{})",
                  HashName(entry->hash), declaration, HashName(candidate_hash), use_binder ? "binder output" : "D3D's copy",
                  sync ? "; the draw waits for it" : "");
      if (sync)
      {
        const int status = im.WaitForTranslation(candidate_hash, true, entry->hash, declaration, stats_);
        if (status != 0)
        {
          return finish_translation(status);
        }
      }
      ++stats_.pending;
      return nullptr;
    }
    b.state = VsBinding::kMissed;
    im.RecordVertex(entry->hash, declaration, miss_hash, "missed", nullptr);
    ++stats_.misses;
    const uint8_t *dump = ucode ? ucode
                                : (shadow::IsGuestPointer(entry->ucode_src) ? shadow::GuestPtr(base, entry->ucode_src)
                                                                            : nullptr);
    im.NoteMiss(miss_hash, true, dump, entry->ucode_bytes, stats_);
    if (candidate && candidate->inputs_unbound)
    {
      REXLOG_WARN("pdz_native: VS object {:08X} decl {:08X}: bound copy hashes to the UNBOUND container {} "
                  "(D3D did not patch it, or [object+12] is not the patched copy)",
                  object, declaration, HashName(bound_hash));
    }
    return nullptr;
  }

  void ShaderCache::OnFrame(uint64_t frame_index)
  {
    Impl &im = impl();
    im.frame = frame_index;
    if (im.record_dirty)
    {
      im.record_dirty = false;
      im.record.flush(); // pdz_native_shader_record: the lines of the frames since the last new shader
    }
    MaintainBindings(frame_index);
    const int32_t every = REXCVAR_GET(pdz_native_log_frames);
    if (every <= 0)
    {
      return;
    }
    if (frame_index - im.last_log_frame >= uint64_t(every))
    {
      im.last_log_frame = frame_index;
      if (stats_.hits != im.last_logged_hits || stats_.misses != im.last_logged_misses)
      {
        LogCoverage("periodic");
      }
    }
  }

  void ShaderCache::MaintainBindings(uint64_t frame)
  {
    constexpr uint64_t kMaintainFrames = 600;
    constexpr uint64_t kMinIdleFrames = 1800;
    Impl &im = impl();
    if (frame - im.last_binding_maintenance < kMaintainFrames)
    {
      return;
    }
    im.last_binding_maintenance = frame;
    const int32_t cap = REXCVAR_GET(pdz_native_vs_binding_cap);
    if (cap <= 0 || im.bindings.size() <= size_t(cap))
    {
      return;
    }
    // Least recently used first. A pending or translating pair stays (a draw is waiting on it); a resolved one points
    // into `loaded` only (never freed), so dropping it frees its own memory and nothing else.
    std::vector<std::pair<uint64_t, uint64_t>> idle; // (last used frame, key)
    for (const auto &[key, b] : im.bindings)
    {
      if ((b.state == VsBinding::kResolved || b.state == VsBinding::kMissed) && b.last_used_frame + kMinIdleFrames <= frame)
      {
        idle.emplace_back(b.last_used_frame, key);
      }
    }
    std::sort(idle.begin(), idle.end());
    const size_t target = size_t(cap) - size_t(cap) / 4;
    const size_t before = im.bindings.size();
    for (const auto &entry : idle)
    {
      if (im.bindings.size() <= target)
      {
        break;
      }
      im.bindings.erase(entry.second);
      ++im.bindings_evicted;
    }
    if (before != im.bindings.size() && im.binding_evict_logged < 16)
    {
      ++im.binding_evict_logged;
      REXLOG_INFO("pdz_native: VS bindings over pdz_native_vs_binding_cap ({} pairs, cap {}): dropped {} unused for {}+ "
                  "frames (now {}, {} dropped in total; binder copies released {} KB in total)",
                  before, cap, before - im.bindings.size(), kMinIdleFrames, im.bindings.size(), im.bindings_evicted,
                  im.binder_bytes_released / 1024);
    }
  }

  ShaderCache::MemoryStats ShaderCache::GetMemoryStats() const
  {
    MemoryStats s;
    if (!impl_)
    {
      return s; // impl() would create it (and register the exit coverage line): not from the memory log
    }
    const Impl &im = *impl_;
    for (const auto &entry : im.loaded)
    {
      const NativeShader *shader = entry.second;
      ++s.loaded;
      if (!shader)
      {
        continue;
      }
      s.usable += shader->usable ? 1 : 0;
      s.dxil_bytes += shader->dxil.size();
      s.source_bytes += shader->coissue_hlsl.size();
    }
    for (const auto &entry : im.bindings)
    {
      ++s.bindings;
      s.binder_bytes += entry.second.binder_bytes.size();
    }
    s.missed = uint32_t(im.missed.size());
    s.bindings_evicted = im.bindings_evicted;
    s.binder_bytes_released = im.binder_bytes_released;
    return s;
  }

  void ShaderCache::LogCoverage(const char *why)
  {
    Impl &im = impl();
    im.last_logged_hits = stats_.hits;
    im.last_logged_misses = stats_.misses;
    if (im.record)
    {
      im.record.flush(); // the exit line comes from atexit: nothing flushes the record later
    }
    // pdz_native_translate_sync: shown when it is on or waited (else the line is as before).
    const std::string sync =
        Impl::SyncTranslateOn() || stats_.sync_waits || stats_.sync_over_budget
            ? fmt::format(" | synchronous translations: waits {} (done {}, failed {}, timed out {}), refused by the frame "
                          "budget {}, waited {} ms (longest {} ms)",
                          stats_.sync_waits, stats_.sync_done, stats_.sync_failed, stats_.sync_timeouts,
                          stats_.sync_over_budget, stats_.sync_ms_total, stats_.sync_ms_max)
            : std::string();
    // pdz_native_first_use_log: what the draws' first uses cost (else the line is as before).
    const std::string first_use =
        REXCVAR_GET(pdz_native_first_use_log)
            ? fmt::format(" | first use: VS bindings {} in {:.0f} ms (max {:.1f} ms), pixel shaders {} in {:.0f} ms "
                          "(max {:.1f} ms)",
                          stats_.first_use_vs, stats_.first_use_vs_ms, stats_.first_use_vs_max_ms, stats_.first_use_ps,
                          stats_.first_use_ps_ms, stats_.first_use_ps_max_ms)
            : std::string();
    PDZ_LOG_PERIODIC(std::string_view(why) == "periodic", "pdz_native: shader cache ({}): hits {} misses {} pending {} | distinct loaded {} failed {} "
                "missed {} | VS bindings {} (binder match {} mismatch {}){} | translations {} requested {} done {} "
                "failed | VS variants built from a cached sibling {} (not derivable {}){} | {} | dir {}{}",
                why, stats_.hits, stats_.misses, stats_.pending, stats_.distinct_loaded, stats_.distinct_failed,
                stats_.distinct_missed, stats_.vs_bindings, stats_.binder_matches, stats_.binder_mismatches,
                first_use, stats_.translations_requested, stats_.translations_done, stats_.translations_failed,
                stats_.sibling_derived, im.derive_failed.size(), sync, coissue::StatsText(), im.dir,
                im.record_path.empty() ? std::string() : " | record " + im.record_path);
  }
} // namespace pdz::native
