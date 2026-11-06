// Game thread sampler (game_thread_sampler.h; stage 2 M5 cpusplit): the sampler thread, the guest function table
// and the 'pdz_timing: guest sampler' line.
#include "game_thread_sampler.h"
#include "cpu_placement.h"

#include "frame_timing.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <windows.h>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

REXCVAR_DEFINE_INT32(pdz_game_thread_profile, 0, "Render",
                     "Frame timing (needs pdz_frame_timing and pdz_frame_timing_phases >= 1): sample the game (Swap) "
                     "thread every N microseconds from a separate thread (0 = off; 1000 = 1 kHz is the intended "
                     "setting; 100-100000) and split its untimed time ('rest': the guest's own code) by guest "
                     "function: game code vs the D3D library, the busiest functions by self and inclusive samples "
                     "(guest link register and stack back chain). Logs a 'pdz_timing: guest sampler' line per "
                     "pdz_timing window and at SUMMARY. The game thread is never stopped (racy reads only).")
    .range(0, 100000);

REXCVAR_DEFINE_BOOL(pdz_guest_profile, false, "Render",
                    "Slow-frame profile of the game thread (log 299: Rooftops had a slow frame about twice a second "
                    "in Rare's own code). The sampler above wakes every 1 ms (or every pdz_game_thread_profile us when "
                    "that is set) and reads the game thread's guest link register and stack without stopping it; the "
                    "samples of a frame (Swap to Swap) count only when that frame took over pdz_guest_profile_ms. One "
                    "'pdz_guest_profile:' line every 5 s (slow frames, where the time went, the top 10 guest functions "
                    "by self and inclusive time) and a total when the window is asked to close. Needs nothing else on. "
                    "Off by default")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(pdz_guest_profile_ms, 12, "Render",
                     "pdz_guest_profile: a frame (Swap to Swap) counts as slow over this many ms (4-1000)")
    .range(4, 1000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace pdz::gtsample
{
  namespace
  {
    // The D3D library: the closure of the hooked D3D entry points over the generated call graph within
    // 0x826C0000-0x82720000 is 0x826DCB48-0x82700E38; the start is moved down to the block of small vtable functions
    // at 0x826DC530 (no direct callers) and the end to the first function of the next block (0x82700F30).
    constexpr uint32_t kD3dBegin = 0x826DC500;
    constexpr uint32_t kD3dEnd = 0x82700F30;

    // A few guest functions known from the stage 2 research (m5_framerate_research.md, m5_frontend.md).
    struct Known
    {
      uint32_t address;
      const char *name;
    };
    constexpr Known kKnown[] = {
        {0x82140AD0, "main tick"},       {0x82140958, "update task tables"}, {0x82140820, "update list"},
        {0x82140500, "game clock"},      {0x82141788, "present step"},       {0x8221ED68, "cutscene update"},
        {0x826DF960, "D3D Swap"},        {0x826DF7F0, "D3D gate"},           {0x826FA8B8, "D3D flush"},
        {0x82AD31F0, "NtYieldExecution"}};

    constexpr uint32_t kMaxDepth = 64;

    struct Counts
    {
      uint64_t total = 0;
      uint64_t where[uint32_t(Where::kCount)] = {};
      uint64_t no_context = 0;   // before the first Swap with the sampler on
      uint64_t rest_unknown = 0; // rest, link register outside the function table
      uint64_t rest_d3d_self = 0;
      uint64_t rest_d3d_stack = 0; // a D3D library function anywhere on the guest stack (or the link register)
      uint64_t stack_frames = 0;   // guest frames kept over the rest samples (walk depth)
      std::vector<uint32_t> self;  // per function index
      std::vector<uint32_t> incl;

      void Reset(size_t functions)
      {
        total = 0;
        std::fill(std::begin(where), std::end(where), 0);
        no_context = rest_unknown = rest_d3d_self = rest_d3d_stack = stack_frames = 0;
        self.assign(functions, 0);
        incl.assign(functions, 0);
      }
    };

    std::atomic<PPCContext *> g_ctx{nullptr};
    std::atomic<uint8_t *> g_base{nullptr};
    std::atomic<bool> g_started{false};
    std::atomic<bool> g_stop{false};
    std::atomic<bool> g_exited{false};
    std::atomic<uint32_t> g_period_us{1000};

    std::mutex g_mutex; // the counts (sampler thread per sample; the logger once per window)
    std::vector<uint32_t> g_starts; // sorted guest function starts (set before the thread starts, then read-only)
    Counts g_window;
    Counts g_session;
    bool g_session_on = false;
    uint64_t g_window_ms_start = 0; // GetTickCount64 at the window's start (the achieved period)
    uint64_t g_session_ms_start = 0;

    // pdz_guest_profile: the frame in progress (the sampler appends [where, self, count, functions...] per sample;
    // the Swap closes the frame) and the slow frames' counts (g_mutex).
    struct Slow
    {
      uint64_t frames = 0, all_frames = 0, samples = 0;
      double ms_sum = 0.0, ms_max = 0.0;
      uint64_t where[uint32_t(Where::kCount)] = {};
      std::vector<uint32_t> self, incl;
      void Reset(size_t functions)
      {
        frames = all_frames = samples = 0;
        ms_sum = ms_max = 0.0;
        std::fill(std::begin(where), std::end(where), 0);
        self.assign(functions, 0);
        incl.assign(functions, 0);
      }
    };
    std::atomic<bool> g_profile{false};
    std::vector<int32_t> g_frame;
    Slow g_slow_window, g_slow_total;
    int64_t g_prev_swap_qpc = 0; // the Swap thread's previous Swap (QPC)
    double g_qpc_ms = 0.0;
    std::atomic<bool> g_total_logged{false};
    void LogSlow(bool total, const char *what); // below

    // Committed, readable guest ranges (guest addresses) of the image's code, for the call-site check.
    std::vector<std::pair<uint32_t, uint32_t>> g_code_ranges;

    int64_t FunctionIndex(uint32_t address)
    {
      if (g_starts.empty() || address < g_starts.front() || address >= g_starts.back() + 0x10000u)
      {
        return -1;
      }
      const auto it = std::upper_bound(g_starts.begin(), g_starts.end(), address);
      return int64_t(it - g_starts.begin()) - 1;
    }

    bool InD3d(int64_t index)
    {
      return index >= 0 && g_starts[size_t(index)] >= kD3dBegin && g_starts[size_t(index)] < kD3dEnd;
    }

    std::string FunctionName(size_t index)
    {
      const uint32_t a = g_starts[index];
      for (const Known &k : kKnown)
      {
        if (k.address == a)
        {
          return fmt::format("{:08X} {}", a, k.name);
        }
      }
      return fmt::format("{:08X}{}", a, a >= kD3dBegin && a < kD3dEnd ? " d3d" : "");
    }

    void BuildTable()
    {
      for (const PPCFuncMapping *m = PPCFuncMappings; m->guest || m->host; ++m)
      {
        if (m->guest >= 0x80000000u && m->guest < 0x100000000ull)
        {
          g_starts.push_back(uint32_t(m->guest));
        }
      }
      std::sort(g_starts.begin(), g_starts.end());
      g_starts.erase(std::unique(g_starts.begin(), g_starts.end()), g_starts.end());
    }

    // The committed readable regions over the code (one VirtualQuery walk at the sampler's start).
    void MapCode(uint8_t *base)
    {
      if (g_starts.empty())
      {
        return;
      }
      uint64_t a = g_starts.front();
      const uint64_t end = uint64_t(g_starts.back()) + 0x1000;
      while (a < end)
      {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(base + a, &mbi, sizeof(mbi)))
        {
          break;
        }
        const uint64_t region_begin = uint64_t(static_cast<uint8_t *>(mbi.BaseAddress) - base);
        const uint64_t region_end = region_begin + mbi.RegionSize;
        const DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                               PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY;
        if (mbi.State == MEM_COMMIT && (mbi.Protect & readable) && !(mbi.Protect & PAGE_GUARD))
        {
          g_code_ranges.push_back({uint32_t(std::max(region_begin, a)), uint32_t(std::min(region_end, end))});
        }
        if (region_end <= a)
        {
          break;
        }
        a = region_end;
      }
    }

    inline uint32_t LoadBe32(const uint8_t *base, uint32_t address)
    {
      return _byteswap_ulong(*reinterpret_cast<const volatile uint32_t *>(base + address));
    }

    bool CodeReadable(uint32_t address)
    {
      for (const auto &r : g_code_ranges)
      {
        if (address >= r.first && address + 4 <= r.second)
        {
          return true;
        }
      }
      return false;
    }

    // A return address: 4-aligned, in a function, and the word before it is a call (bl / bla, bcctrl, bclrl).
    bool IsReturnAddress(const uint8_t *base, uint32_t ret)
    {
      if ((ret & 3) || ret < 4 || FunctionIndex(ret) < 0 || !CodeReadable(ret - 4))
      {
        return false;
      }
      const uint32_t insn = LoadBe32(base, ret - 4);
      const uint32_t op = insn >> 26;
      return (insn & 1) && (op == 18 || op == 19 || op == 16);
    }

    // The committed region holding the guest stack pointer (cached; re-queried when r1 leaves it).
    struct StackRegion
    {
      uint32_t lo = 0, hi = 0; // guest addresses, [lo, hi)
      bool Contains(uint32_t a, uint32_t bytes) const { return a >= lo && uint64_t(a) + bytes <= hi; }
      bool Update(uint8_t *base, uint32_t r1)
      {
        if (Contains(r1, 4))
        {
          return true;
        }
        lo = hi = 0;
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(base + r1, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT ||
            !(mbi.Protect & (PAGE_READWRITE | PAGE_READONLY | PAGE_EXECUTE_READWRITE)) || (mbi.Protect & PAGE_GUARD))
        {
          return false;
        }
        const uint64_t b = uint64_t(static_cast<uint8_t *>(mbi.BaseAddress) - base);
        const uint64_t e = std::min<uint64_t>(b + mbi.RegionSize, 0xFFFFFFFFull);
        lo = uint32_t(b);
        hi = uint32_t(e);
        return Contains(r1, 4);
      }
    };

    void Sample(StackRegion &stack)
    {
      PPCContext *const c = g_ctx.load(std::memory_order_acquire);
      uint8_t *const base = g_base.load(std::memory_order_relaxed);
      const uint8_t where = g_where.load(std::memory_order_relaxed);
      int64_t functions[kMaxDepth + 1];
      uint32_t count = 0;
      bool d3d_self = false, d3d_stack = false, unknown = false;
      uint32_t frames = 0;
      if (c && base && where == uint8_t(Where::kRest))
      {
        const uint32_t lr = uint32_t(*reinterpret_cast<const volatile uint64_t *>(&c->lr));
        const uint32_t r1 = *reinterpret_cast<const volatile uint32_t *>(&c->r1); // the low word (little endian)
        const int64_t self = FunctionIndex(lr);
        if (self < 0)
        {
          unknown = true;
        }
        else
        {
          functions[count++] = self;
          d3d_self = d3d_stack = InD3d(self);
        }
        // The guest back chain: frame at sp, its caller's sp at [sp], the return address at [caller sp - 8].
        uint32_t sp = r1;
        if (stack.Update(base, r1))
        {
          for (uint32_t depth = 0; depth < kMaxDepth && stack.Contains(sp, 4); ++depth)
          {
            const uint32_t next = LoadBe32(base, sp);
            if (next <= sp || next - sp > 0x20000u || !stack.Contains(next - 8, 4))
            {
              break;
            }
            const uint32_t ret = LoadBe32(base, next - 8);
            if (IsReturnAddress(base, ret))
            {
              const int64_t f = FunctionIndex(ret);
              bool seen = false;
              for (uint32_t i = 0; i < count && !seen; ++i)
              {
                seen = functions[i] == f;
              }
              if (!seen && count <= kMaxDepth)
              {
                functions[count++] = f;
                d3d_stack = d3d_stack || InD3d(f);
              }
              ++frames;
            }
            sp = next;
          }
        }
      }
      std::lock_guard<std::mutex> lock(g_mutex);
      if (g_profile.load(std::memory_order_relaxed) && c && base && g_frame.size() < 65536)
      {
        // pdz_guest_profile: [where, self (-1: unknown / not rest), count, the functions on the stack incl. self]
        const bool rest = where == uint8_t(Where::kRest);
        g_frame.push_back(int32_t(where < uint8_t(Where::kCount) ? where : 0));
        g_frame.push_back(rest && !unknown && count ? int32_t(functions[0]) : -1);
        g_frame.push_back(rest ? int32_t(count) : 0);
        for (uint32_t i = 0; rest && i < count; ++i)
        {
          g_frame.push_back(int32_t(functions[i]));
        }
      }
      Counts *targets[2] = {&g_window, g_session_on ? &g_session : nullptr};
      for (Counts *t : targets)
      {
        if (!t)
        {
          continue;
        }
        ++t->total;
        if (!c || !base)
        {
          ++t->no_context;
          continue;
        }
        ++t->where[where < uint8_t(Where::kCount) ? where : 0];
        if (where != uint8_t(Where::kRest))
        {
          continue;
        }
        t->rest_unknown += unknown;
        t->rest_d3d_self += d3d_self;
        t->rest_d3d_stack += d3d_stack;
        t->stack_frames += frames;
        if (!unknown && count)
        {
          ++t->self[size_t(functions[0])]; // functions[0] = the link register's function when known
        }
        for (uint32_t i = 0; i < count; ++i)
        {
          ++t->incl[size_t(functions[i])];
        }
      }
    }

    DWORD WINAPI Main(void *)
    {
      pdz::cpu::PlaceHelperThread("game thread sampler"); // pdz_cpu_placement
      using SetThreadDescriptionFn = HRESULT(WINAPI *)(HANDLE, PCWSTR);
      if (HMODULE kernel = GetModuleHandleW(L"kernel32.dll"))
      {
        if (auto set_name = reinterpret_cast<SetThreadDescriptionFn>(
                reinterpret_cast<void *>(GetProcAddress(kernel, "SetThreadDescription"))))
        {
          set_name(GetCurrentThread(), L"pdz game thread sampler");
        }
      }
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
      HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
      const bool high_resolution = timer != nullptr;
      if (!timer)
      {
        timer = CreateWaitableTimerW(nullptr, TRUE, nullptr);
      }
      if (uint8_t *base = g_base.load(std::memory_order_relaxed))
      {
        MapCode(base);
      }
      REXLOG_INFO("pdz_gtsample: on, every {} us ({} timer), {} guest functions, D3D library {:08X}-{:08X}, {} "
                  "readable code range(s) for the call-site check",
                  g_period_us.load(), high_resolution ? "high-resolution" : "standard", g_starts.size(), kD3dBegin,
                  kD3dEnd, g_code_ranges.size());
      StackRegion stack;
      while (!g_stop.load(std::memory_order_acquire))
      {
        const uint32_t period = g_period_us.load(std::memory_order_relaxed);
        if (timer)
        {
          LARGE_INTEGER due;
          due.QuadPart = -int64_t(period) * 10; // 100 ns units, relative
          if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE))
          {
            WaitForSingleObject(timer, 1000);
          }
          else
          {
            Sleep(1);
          }
        }
        else
        {
          Sleep(1);
        }
        if (g_stop.load(std::memory_order_acquire))
        {
          break;
        }
        Sample(stack);
        if (g_profile.load(std::memory_order_relaxed))
        {
          static uint64_t last_line = 0;
          const uint64_t now = GetTickCount64();
          if (!last_line)
          {
            last_line = now;
          }
          else if (now - last_line >= 5000)
          {
            last_line = now;
            LogSlow(false, "last 5 s");
          }
        }
      }
      if (timer)
      {
        CloseHandle(timer);
      }
      g_exited.store(true, std::memory_order_release);
      return 0;
    }

    void StopAtExit()
    {
      LogGuestProfileTotal("exit");
      g_stop.store(true, std::memory_order_release);
      for (int i = 0; i < 100 && !g_exited.load(std::memory_order_acquire); ++i)
      {
        Sleep(5);
      }
    }

    void Start()
    {
      bool expected = false;
      if (!g_started.compare_exchange_strong(expected, true))
      {
        return;
      }
      BuildTable();
      {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_window.Reset(g_starts.size());
        g_session.Reset(g_starts.size());
        g_slow_window.Reset(g_starts.size());
        g_slow_total.Reset(g_starts.size());
        g_window_ms_start = g_session_ms_start = GetTickCount64();
      }
      HANDLE thread = CreateThread(nullptr, 256 * 1024, Main, nullptr, 0, nullptr);
      if (!thread)
      {
        g_exited.store(true, std::memory_order_release);
        REXLOG_WARN("pdz_gtsample: the sampler thread could not be created");
        return;
      }
      SetThreadPriority(thread, THREAD_PRIORITY_ABOVE_NORMAL); // wakes on time; a few microseconds per wake
      CloseHandle(thread);
      std::atexit(StopAtExit);
    }

    struct Ranked
    {
      uint32_t count;
      size_t index;
    };

    std::vector<Ranked> Top(const std::vector<uint32_t> &v, size_t n)
    {
      std::vector<Ranked> out;
      for (size_t i = 0; i < v.size(); ++i)
      {
        if (v[i])
        {
          out.push_back({v[i], i});
        }
      }
      const size_t k = std::min(n, out.size());
      std::partial_sort(out.begin(), out.begin() + ptrdiff_t(k), out.end(),
                        [](const Ranked &a, const Ranked &b) { return a.count > b.count; });
      out.resize(k);
      return out;
    }

    // pdz_guest_profile (the Swap thread, every Swap): the frame since the previous Swap ends here; its samples count
    // when it took over the threshold.
    void CloseFrame()
    {
      LARGE_INTEGER now;
      QueryPerformanceCounter(&now);
      if (g_qpc_ms == 0.0)
      {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        g_qpc_ms = 1000.0 / double(f.QuadPart);
      }
      const double ms = g_prev_swap_qpc ? double(now.QuadPart - g_prev_swap_qpc) * g_qpc_ms : 0.0;
      const bool timed = g_prev_swap_qpc != 0;
      g_prev_swap_qpc = now.QuadPart;
      const double threshold = double(std::clamp(REXCVAR_GET(pdz_guest_profile_ms), 4, 1000));
      std::lock_guard<std::mutex> lock(g_mutex);
      Slow *targets[2] = {&g_slow_window, &g_slow_total};
      for (Slow *t : targets)
      {
        t->all_frames += timed;
      }
      if (timed && ms > threshold && !g_slow_window.self.empty())
      {
        for (Slow *t : targets)
        {
          ++t->frames;
          t->ms_sum += ms;
          t->ms_max = std::max(t->ms_max, ms);
        }
        const size_t n = g_slow_window.self.size();
        for (size_t i = 0; i + 3 <= g_frame.size();)
        {
          const int32_t where = g_frame[i], self = g_frame[i + 1], count = g_frame[i + 2];
          i += 3;
          for (Slow *t : targets)
          {
            ++t->samples;
            ++t->where[size_t(where) < size_t(Where::kCount) ? size_t(where) : 0];
            if (self >= 0 && size_t(self) < n)
            {
              ++t->self[size_t(self)];
            }
          }
          for (int32_t k = 0; k < count && i < g_frame.size(); ++k, ++i)
          {
            const int32_t f = g_frame[i];
            for (Slow *t : targets)
            {
              if (f >= 0 && size_t(f) < n)
              {
                ++t->incl[size_t(f)];
              }
            }
          }
        }
      }
      g_frame.clear();
    }

    // One pdz_guest_profile line: the window (5 s, then reset) or the total.
    void LogSlow(bool total, const char *what)
    {
      Slow c;
      {
        std::lock_guard<std::mutex> lock(g_mutex);
        Slow &src = total ? g_slow_total : g_slow_window;
        c = src;
        if (!total)
        {
          g_slow_window.Reset(g_starts.size());
        }
      }
      const int32_t threshold = std::clamp(REXCVAR_GET(pdz_guest_profile_ms), 4, 1000);
      const double sample_ms = double(g_period_us.load(std::memory_order_relaxed)) / 1000.0;
      std::string line;
      if (!c.frames || !c.samples)
      {
        line = fmt::format("pdz_guest_profile: {}: no slow frames (over {} ms) of {} frames", what, threshold,
                           c.all_frames);
      }
      else
      {
        static constexpr const char *kNames[uint32_t(Where::kCount)] = {
            "rest (guest code)", "d3d hooks", "swap hook", "gate",    "ring wait", "guest fence",
            "block idle",        "not busy",  "adaptive",  "fe spin", "fe pace",   "yield"};
        const double per = sample_ms / double(c.frames); // ms per slow frame for one sample count
        std::string where;
        for (uint32_t w = 0; w < uint32_t(Where::kCount); ++w)
        {
          if (c.where[w])
          {
            where += fmt::format("{}{} {:.1f}%", where.empty() ? "" : ", ", kNames[w],
                                 100.0 * double(c.where[w]) / double(c.samples));
          }
        }
        const size_t n = total ? 20 : 10;
        std::string self, incl;
        for (const Ranked &x : Top(c.self, n))
        {
          self += fmt::format("{}{} {:.2f} ms ({:.1f}%)", self.empty() ? "" : ", ", FunctionName(x.index),
                              double(x.count) * per, 100.0 * double(x.count) / double(c.samples));
        }
        for (const Ranked &x : Top(c.incl, n))
        {
          incl += fmt::format("{}{} {:.2f} ms ({:.1f}%)", incl.empty() ? "" : ", ", FunctionName(x.index),
                              double(x.count) * per, 100.0 * double(x.count) / double(c.samples));
        }
        line = fmt::format("pdz_guest_profile: {}: slow frames {} of {} (over {} ms; avg {:.2f} ms, max {:.2f} ms), "
                           "{} samples ({:.1f} per slow frame, one per {} us) | where: {} | self top {} (ms per slow "
                           "frame, % of the samples): {} | inclusive top {}: {}",
                           what, c.frames, c.all_frames, threshold, c.ms_sum / double(c.frames), c.ms_max, c.samples,
                           double(c.samples) / double(c.frames), g_period_us.load(std::memory_order_relaxed), where, n,
                           self.empty() ? std::string("none") : self, n, incl.empty() ? std::string("none") : incl);
      }
      if (total)
      {
        REXLOG_INFO("{}", line); // at close: written now
      }
      else
      {
        pdz::timing::DeferLine(std::move(line));
      }
    }
  } // namespace

  void OnSwapHook(PPCContext &ctx, uint8_t *base)
  {
    const bool profile = REXCVAR_GET(pdz_guest_profile);
    int32_t period = REXCVAR_GET(pdz_game_thread_profile);
    if (period <= 0 && profile)
    {
      period = 1000; // pdz_guest_profile alone: 1 kHz
    }
    t_mark = period > 0;
    if (period <= 0)
    {
      g_profile.store(false, std::memory_order_relaxed);
      return;
    }
    if (profile && g_started.load(std::memory_order_relaxed))
    {
      CloseFrame();
    }
    g_profile.store(profile, std::memory_order_relaxed);
    // The Swap is called from the game's own code, outside every marked section: start the frame from "rest" so a
    // section whose exit was never seen (a stale value, logs 216-222: no rest samples at all) cannot stick.
    g_where.store(uint8_t(Where::kRest), std::memory_order_relaxed);
    g_period_us.store(uint32_t(std::clamp(period, 100, 100000)), std::memory_order_relaxed);
    g_base.store(base, std::memory_order_relaxed);
    g_ctx.store(&ctx, std::memory_order_release); // the game thread's context (one per guest thread)
    if (!g_started.load(std::memory_order_relaxed))
    {
      Start();
    }
  }

  void StartSession()
  {
    if (!g_started.load(std::memory_order_relaxed))
    {
      return;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    g_session.Reset(g_starts.size());
    g_window.Reset(g_starts.size());
    g_session_on = true;
    g_window_ms_start = g_session_ms_start = GetTickCount64();
  }

  void LogRange(const char *kind, uint64_t first_swap, uint64_t last_swap, double frame_ms, double rest_ms,
                bool window)
  {
    if (!g_started.load(std::memory_order_relaxed))
    {
      return;
    }
    Counts c;
    uint64_t ms_start = 0;
    {
      std::lock_guard<std::mutex> lock(g_mutex);
      Counts &src = window ? g_window : g_session;
      c = src; // a few hundred KB, once per window
      ms_start = window ? g_window_ms_start : g_session_ms_start;
      if (window)
      {
        g_window.Reset(g_starts.size());
        g_window_ms_start = GetTickCount64();
      }
    }
    const uint64_t frames = last_swap >= first_swap ? last_swap - first_swap + 1 : 0;
    const uint64_t sampled = c.total - c.no_context;
    if (!sampled || !frames)
    {
      REXLOG_INFO("pdz_timing: guest sampler ({}) swaps {}-{} | no samples ({} before the first Swap)", kind,
                  first_swap, last_swap, c.no_context);
      return;
    }
    const double elapsed_ms = double(GetTickCount64() - ms_start);
    static constexpr const char *kWhereNames[uint32_t(Where::kCount)] = {
        "rest",          "d3d hooks",        "swap hook", "gate",      "ring wait", "guest fence",
        "block idle",    "not busy",         "adaptive",  "fe spin",   "fe pace",   "yield"};
    std::string where;
    for (uint32_t w = 0; w < uint32_t(Where::kCount); ++w)
    {
      if (!c.where[w])
      {
        continue;
      }
      const double share = double(c.where[w]) / double(sampled);
      where += fmt::format("{}{} {:.1f}% ~{:.2f} ms", where.empty() ? "" : ", ", kWhereNames[w], 100.0 * share,
                           share * frame_ms);
    }
    const uint64_t rest = c.where[uint32_t(Where::kRest)];
    std::string by_code = "no rest samples";
    std::string self_top, incl_top;
    if (rest)
    {
      const double r = double(rest);
      const uint64_t game = rest - std::min(rest, c.rest_d3d_self + c.rest_unknown);
      by_code = fmt::format("game code {:.2f} ms ({:.1f}%), D3D library self {:.2f} ms ({:.1f}%), D3D on the guest "
                            "stack {:.2f} ms ({:.1f}%), link register outside the table {:.1f}%, guest frames per "
                            "sample {:.1f}",
                            rest_ms * double(game) / r, 100.0 * double(game) / r, rest_ms * double(c.rest_d3d_self) / r,
                            100.0 * double(c.rest_d3d_self) / r, rest_ms * double(c.rest_d3d_stack) / r,
                            100.0 * double(c.rest_d3d_stack) / r, 100.0 * double(c.rest_unknown) / r,
                            double(c.stack_frames) / r);
      const size_t n = window ? 8 : 20;
      for (const Ranked &x : Top(c.self, n))
      {
        self_top += fmt::format("{}{} {:.2f} ms ({:.1f}%)", self_top.empty() ? "" : ", ", FunctionName(x.index),
                                rest_ms * double(x.count) / r, 100.0 * double(x.count) / r);
      }
      for (const Ranked &x : Top(c.incl, window ? 10 : 30))
      {
        incl_top += fmt::format("{}{} {:.2f} ms ({:.1f}%)", incl_top.empty() ? "" : ", ", FunctionName(x.index),
                                rest_ms * double(x.count) / r, 100.0 * double(x.count) / r);
      }
    }
    REXLOG_INFO("pdz_timing: guest sampler ({}) swaps {}-{} | samples {} ({:.1f} per frame, every {} us, achieved "
                "{:.0f}/s) | where (share x frame {:.2f} ms): {} | rest (exact {:.2f} ms/frame) by code: {} | self "
                "top (% of rest): {} | inclusive top (% of rest): {}",
                kind, first_swap, last_swap, sampled, double(sampled) / double(frames), g_period_us.load(),
                elapsed_ms > 0.0 ? double(c.total) * 1000.0 / elapsed_ms : 0.0, frame_ms, where, rest_ms, by_code,
                self_top.empty() ? std::string("none") : self_top, incl_top.empty() ? std::string("none") : incl_top);
  }

  void LogGuestProfileTotal(const char *why)
  {
    if (!g_started.load(std::memory_order_relaxed) || !REXCVAR_GET(pdz_guest_profile) ||
        g_total_logged.exchange(true))
    {
      return;
    }
    LogSlow(true, fmt::format("total ({})", why).c_str());
  }
} // namespace pdz::gtsample

// -----------------------------------------------------------------------------------------------------------------
// cpusplit hooks (forward unchanged; timers / sections only, on the Swap thread with pdz_frame_timing_phases >= 1 or
// pdz_game_thread_profile set).
// -----------------------------------------------------------------------------------------------------------------
REX_EXTERN(__imp__sub_826DF7F0); // the presentation gate (present step, every frame)
REX_EXTERN(__imp__sub_82AD31F0); // NtYieldExecution wrapper (the present step's adaptive wait / front-end spin, ...)

// sub_826DF7F0(device): the presentation gate's ring packets (SCRATCH_REG1, InsertCallback of sub_826DF720); the CPU
// does not wait here unless the ring is full (then a ring wait inside it, charged to wait.ring).
REX_HOOK_RAW(sub_826DF7F0)
{
  const pdz::timing::ExclusiveScope gate_time(pdz::timing::Phase::kGuestGate);
  const pdz::gtsample::Mark sampler_mark(pdz::gtsample::Where::kGate);
  __imp__sub_826DF7F0(ctx, base);
}

// sub_82AD31F0(): NtYieldExecution (SwitchToThread). From the present step sub_82141788: lr 821418C4 = the adaptive
// extra wait in a level (spin while 82D3DB00+104 < +144), lr 821418FC = the front-end spin (until +104 >= 2).
REX_HOOK_RAW(sub_82AD31F0)
{
  const uint32_t lr = uint32_t(ctx.lr);
  const pdz::timing::Phase phase = lr == 0x821418C4u   ? pdz::timing::Phase::kWaitAdaptive
                                   : lr == 0x821418FCu ? pdz::timing::Phase::kWaitFeSpin
                                                       : pdz::timing::Phase::kWaitYield;
  const pdz::gtsample::Where where = lr == 0x821418C4u   ? pdz::gtsample::Where::kAdaptive
                                     : lr == 0x821418FCu ? pdz::gtsample::Where::kFeSpin
                                                         : pdz::gtsample::Where::kYield;
  const pdz::timing::ExclusiveScope yield_time(phase);
  const pdz::gtsample::Mark sampler_mark(where);
  __imp__sub_82AD31F0(ctx, base);
}
