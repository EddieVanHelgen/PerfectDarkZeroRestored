// Native renderer (stage 1): hang watchdog (native_watchdog.h). Off unless pdz_native_watchdog_ms > 0.
//
// Each thread that enters a marked wait gets a slot (thread-local index into a fixed table) holding its OS thread
// id, the wait's label, a detail value and the start time; the watchdog thread (started at the first Swap or the first
// marked wait with the switch on) scans the table every quarter threshold and logs:
//   pdz_watchdog: thread <id> in '<wait>' (<detail>) for <ms> ms | <state>     (again every 5 s while it lasts)
//   pdz_watchdog: thread <id> left '<wait>' after about <ms> ms
//   pdz_watchdog: no Swap for <ms> ms (<marked waits or none>) | <state>        (again every 5 s)
//   pdz_watchdog: Swap resumed (the gap lasted at least <ms> ms)
// <state> = the last swap and its age, the native GPU fence (completed now / signalled at the last swap, and how
// long the completed value has not moved while work was outstanding), the presenter (whether the title owns the
// output, a pending same-frame plugin capture and since when) and the device-removed reason; with pdz_sync_log or
// the null GPU backend also the guest D3D's sync state (gpu_sync.h D3DState: the fence the GPU wrote / issued /
// awaited, the scratch block, the D3D vblank count, the threads in a fence block or BlockUntilIdle).
// pdz_native_watchdog_dump_s > 0: one minidump (every thread's stack, logs\hang_<time>_<pid>.dmp) when a wait or the
// Swap gap first passes that many seconds.
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

#include <windows.h>
#include <dbghelp.h>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/presenter.h>

#include "native_device.h"
#include "native_watchdog.h"
#include "../cpu_placement.h"
#include "../gpu_sync.h"

#pragma comment(lib, "dbghelp.lib") // MiniDumpWriteDump (the target links it too: crash_dump.cpp)

REXCVAR_DEFINE_INT32(pdz_native_watchdog_ms, 0, "Render",
                     "Native renderer hang watchdog: log a marked wait (guest D3D fence block, presenter refresh, "
                     "native frame fence, render mutex at an output toggle) or a Swap gap older than this many ms, "
                     "with the native GPU fence, the presenter owner, a pending plugin capture and the device state "
                     "(0 = off; e.g. 2000).")
    .range(0, 600000);
REXCVAR_DEFINE_INT32(pdz_native_watchdog_dump_s, 0, "Render",
                     "With pdz_native_watchdog_ms on: write one minidump of every thread (logs\\hang_<time>_<pid>.dmp) "
                     "when a marked wait or the Swap gap first passes this many seconds (0 = none; e.g. 15).")
    .range(0, 3600);

namespace pdz::native::watchdog
{
  namespace
  {
    constexpr int kSlots = 32;
    constexpr uint64_t kRepeatMs = 5000;

    struct Slot
    {
      std::atomic<uint32_t> tid{0};
      std::atomic<const char *> what{nullptr};
      std::atomic<uint64_t> detail{0};
      std::atomic<uint64_t> since{0}; // GetTickCount64 at the start of the innermost wait; 0 = not waiting
    };
    Slot g_slots[kSlots];
    std::atomic<int> g_used{0};
    thread_local int t_slot = -1;

    int ThreadSlot()
    {
      if (t_slot < 0)
      {
        const int s = g_used.fetch_add(1, std::memory_order_relaxed);
        t_slot = s < kSlots ? s : kSlots - 1; // more threads than slots: the last one is shared
        g_slots[t_slot].tid.store(GetCurrentThreadId(), std::memory_order_relaxed);
      }
      return t_slot;
    }

    // Written by OnSwap (render mutex held), read by the watchdog thread.
    std::atomic<uint64_t> g_swaps{0};
    std::atomic<uint64_t> g_last_swap_ms{0};
    std::atomic<uint64_t> g_native_frame{0};
    std::atomic<uint64_t> g_signalled{0};
    std::atomic<NativeDevice *> g_dev{nullptr};
    // The watchdog thread: started by the first Swap or the first marked wait (a hang before the first Swap, e.g. the
    // ring init's BlockUntilIdle with the null GPU backend, is reported too).
    std::atomic<bool> g_started{false};

    // Watchdog thread only.
    struct Reported
    {
      uint64_t since = 0; // the wait reported (its start time), 0 = none
      uint64_t at = 0;    // when it was last logged
      const char *what = nullptr;
      uint64_t detail = 0;
      uint32_t tid = 0;
    };
    Reported g_reported[kSlots];
    uint64_t g_completed_seen = 0, g_completed_change_ms = 0;
    uint64_t g_capture_seen = 0, g_capture_since_ms = 0;
    uint64_t g_stale_at = 0; // last "no Swap" line, 0 = swaps are coming
    uint64_t g_stale_since_swap_ms = 0;
    bool g_dumped = false;

    std::string Detail(const char *what, uint64_t detail)
    {
      if (!detail)
      {
        return std::string();
      }
      // The guest fence block's detail is its caller (LR); the D3D sites behind the common ones (gpu_census.cpp).
      if (what && std::string(what).find("sub_826DE258") != std::string::npos)
      {
        const char *site = detail == 0x826DD5E4 || detail == 0x826DD610 ? ": sub_826DD4F8 <- KickOff (ring segment reuse)"
                           : detail == 0x826DDF0C                       ? ": sub_826DDE30 ring space"
                           : detail == 0x826DE710                       ? ": BlockUntilIdle"
                           : detail == 0x826DE8EC                       ? ": BlockUntilNotBusy"
                           : detail == 0x826E1B48                       ? ": resource lock"
                                                                        : "";
        return fmt::format(" (caller {:08X}{})", uint32_t(detail), site);
      }
      return fmt::format(" ({})", detail);
    }

    std::string State(uint64_t now)
    {
      std::string s;
      const uint64_t swap_ms = g_last_swap_ms.load(std::memory_order_relaxed);
      if (swap_ms)
      {
        s = fmt::format("swap {} {} ms ago (native frame {})", g_swaps.load(std::memory_order_relaxed),
                        now - swap_ms, g_native_frame.load(std::memory_order_relaxed));
      }
      else
      {
        s = "no swap yet";
      }
      NativeDevice *dev = g_dev.load(std::memory_order_acquire);
      if (dev)
      {
        const uint64_t completed = dev->completed_fence();
        const uint64_t signalled = g_signalled.load(std::memory_order_relaxed);
        if (completed == UINT64_MAX)
        {
          s += " | native GPU fence: device removed";
        }
        else
        {
          s += fmt::format(" | native GPU fence completed {} / signalled {} at the last swap", completed, signalled);
          if (completed < signalled && g_completed_change_ms && now - g_completed_change_ms >= 1000)
          {
            s += fmt::format(", no GPU progress for {} ms", now - g_completed_change_ms);
          }
        }
      }
      const bool external = rex::ui::Presenter::IsGuestOutputExternal();
      const uint64_t capture = rex::ui::Presenter::RequestedGuestOutputCapture();
      s += fmt::format(" | presenter: output {}", external ? "owned by the title (native output)" : "refreshed by the GPU plugin");
      if (capture)
      {
        s += fmt::format(", plugin capture of swap {} pending for {} ms", capture,
                         g_capture_since_ms ? now - g_capture_since_ms : 0);
      }
      if (dev && dev->device())
      {
        const HRESULT removed = dev->device()->GetDeviceRemovedReason();
        s += FAILED(removed) ? fmt::format(" | device REMOVED (reason 0x{:08X})", uint32_t(removed)) : " | device OK";
      }
      // The guest D3D's side (gpu_sync.h, with pdz_sync_log or the null GPU backend): the fence the GPU wrote against
      // the one issued / awaited, the scratch block (callback CPU mask, presentation gate), the D3D vblank count.
      const std::string d3d = pdz::gpusync::D3DState();
      if (!d3d.empty())
      {
        s += fmt::format(" | {}{}", pdz::gpusync::NullBackend() ? "null GPU backend, " : "", d3d);
      }
      return s;
    }

    std::string WaitsSummary(uint64_t now)
    {
      std::string s;
      const int used = std::min(g_used.load(std::memory_order_relaxed), kSlots);
      for (int i = 0; i < used; ++i)
      {
        const uint64_t since = g_slots[i].since.load(std::memory_order_acquire);
        if (!since)
        {
          continue;
        }
        const char *what = g_slots[i].what.load(std::memory_order_relaxed);
        s += fmt::format("{}thread {} in '{}'{} for {} ms", s.empty() ? "" : "; ",
                         g_slots[i].tid.load(std::memory_order_relaxed), what ? what : "?",
                         Detail(what, g_slots[i].detail.load(std::memory_order_relaxed)), now - since);
      }
      return s.empty() ? std::string("no thread in a marked wait (the game thread runs guest code, e.g. loading)") : s;
    }

    void WriteDump(const std::string &reason)
    {
      g_dumped = true;
      char exe[MAX_PATH] = {};
      GetModuleFileNameA(nullptr, exe, MAX_PATH);
      std::string dir(exe);
      const size_t slash = dir.find_last_of('\\');
      dir = (slash == std::string::npos ? std::string(".") : dir.substr(0, slash)) + "\\logs";
      CreateDirectoryA(dir.c_str(), nullptr);
      SYSTEMTIME st;
      GetLocalTime(&st);
      const std::string path = fmt::format("{}\\hang_{:04}{:02}{:02}_{:02}{:02}{:02}_{}.dmp", dir, st.wYear, st.wMonth,
                                           st.wDay, st.wHour, st.wMinute, st.wSecond, GetCurrentProcessId());
      HANDLE file = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (file == INVALID_HANDLE_VALUE)
      {
        REXLOG_WARN("pdz_watchdog: hang minidump could not be created at {} (error {})", path, GetLastError());
        return;
      }
      const MINIDUMP_TYPE type = MINIDUMP_TYPE(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithDataSegs |
                                               MiniDumpWithThreadInfo | MiniDumpWithHandleData);
      const BOOL ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, type, nullptr, nullptr, nullptr);
      CloseHandle(file);
      REXLOG_WARN("pdz_watchdog: hang minidump ({}) {} {}", reason, ok ? "written to" : "FAILED at", path);
    }

    void Check(uint64_t threshold_ms)
    {
      const uint64_t now = GetTickCount64();
      // Tracked between reports: GPU progress of the native fence and the age of a pending plugin capture.
      if (NativeDevice *dev = g_dev.load(std::memory_order_acquire))
      {
        const uint64_t completed = dev->completed_fence();
        if (completed != g_completed_seen || !g_completed_change_ms)
        {
          g_completed_seen = completed;
          g_completed_change_ms = now;
        }
      }
      const uint64_t capture = rex::ui::Presenter::RequestedGuestOutputCapture();
      if (capture != g_capture_seen)
      {
        g_capture_seen = capture;
        g_capture_since_ms = capture ? now : 0;
      }

      const int32_t dump_s = REXCVAR_GET(pdz_native_watchdog_dump_s);
      const uint64_t dump_ms = dump_s > 0 ? uint64_t(dump_s) * 1000 : 0;
      uint64_t oldest = 0;
      std::string oldest_what;
      const int used = std::min(g_used.load(std::memory_order_relaxed), kSlots);
      for (int i = 0; i < used; ++i)
      {
        Slot &slot = g_slots[i];
        Reported &r = g_reported[i];
        const uint64_t since = slot.since.load(std::memory_order_acquire);
        if (r.since && since != r.since)
        {
          REXLOG_INFO("pdz_watchdog: thread {} left '{}'{} after about {} ms", r.tid, r.what ? r.what : "?",
                      Detail(r.what, r.detail), now - r.since);
          r.since = 0;
        }
        if (!since || now < since || now - since < threshold_ms)
        {
          continue;
        }
        const char *what = slot.what.load(std::memory_order_relaxed);
        const uint64_t detail = slot.detail.load(std::memory_order_relaxed);
        if (now - since > oldest)
        {
          oldest = now - since;
          oldest_what = what ? what : "?";
        }
        if (r.since != since || now - r.at >= kRepeatMs)
        {
          const bool again = r.since == since;
          r.since = since;
          r.at = now;
          r.what = what;
          r.detail = detail;
          r.tid = slot.tid.load(std::memory_order_relaxed);
          REXLOG_WARN("pdz_watchdog: thread {} {}in '{}'{} for {} ms | {}", r.tid, again ? "still " : "",
                      what ? what : "?", Detail(what, detail), now - since, State(now));
        }
      }

      const uint64_t swap_ms = g_last_swap_ms.load(std::memory_order_relaxed);
      if (g_stale_at && swap_ms != g_stale_since_swap_ms)
      {
        // A Swap came after the reported gap (the gap lasted at least until the last report).
        REXLOG_INFO("pdz_watchdog: Swap resumed (the gap lasted at least {} ms)", g_stale_at - g_stale_since_swap_ms);
        g_stale_at = 0;
      }
      if (swap_ms && now > swap_ms && now - swap_ms >= threshold_ms)
      {
        if (!g_stale_at || now - g_stale_at >= kRepeatMs)
        {
          g_stale_at = now;
          g_stale_since_swap_ms = swap_ms;
          REXLOG_WARN("pdz_watchdog: no Swap for {} ms ({}) | {}", now - swap_ms, WaitsSummary(now), State(now));
        }
        if (now - swap_ms > oldest)
        {
          oldest = now - swap_ms;
          oldest_what = "no Swap";
        }
      }

      if (dump_ms && !g_dumped && oldest >= dump_ms)
      {
        WriteDump(fmt::format("{} for {} ms", oldest_what, oldest));
      }
    }

    void Run()
    {
      pdz::cpu::PlaceHelperThread("watchdog"); // pdz_cpu_placement
      // Named for the hang dump (SetThreadDescription looked up at run time, as the SDK's threading code does).
      using SetThreadDescriptionFn = HRESULT(WINAPI *)(HANDLE, PCWSTR);
      if (HMODULE kernel = GetModuleHandleW(L"kernel32.dll"))
      {
        if (auto set_name = reinterpret_cast<SetThreadDescriptionFn>(GetProcAddress(kernel, "SetThreadDescription")))
        {
          set_name(GetCurrentThread(), L"pdz native watchdog");
        }
      }
      for (;;)
      {
        const int32_t ms = REXCVAR_GET(pdz_native_watchdog_ms);
        Sleep(DWORD(ms > 0 ? std::clamp(ms / 4, 50, 500) : 500));
        if (ms > 0)
        {
          Check(uint64_t(ms));
        }
      }
    }

    // Once: the first Swap or the first marked wait with the switch on.
    void StartThread()
    {
      bool expected = false;
      if (g_started.load(std::memory_order_relaxed) || !g_started.compare_exchange_strong(expected, true))
      {
        return;
      }
      std::thread(Run).detach();
      REXLOG_INFO("pdz_watchdog: on (report waits and Swap gaps over {} ms{})", REXCVAR_GET(pdz_native_watchdog_ms),
                  REXCVAR_GET(pdz_native_watchdog_dump_s) > 0
                      ? fmt::format("; one hang minidump after {} s", REXCVAR_GET(pdz_native_watchdog_dump_s))
                      : std::string());
    }
  } // namespace

  bool Enabled() { return REXCVAR_GET(pdz_native_watchdog_ms) > 0; }

  Wait::Wait(const char *what, uint64_t detail)
  {
    if (!Enabled())
    {
      return;
    }
    StartThread(); // a hang before the first Swap (e.g. the ring init's BlockUntilIdle) is reported too
    slot_ = ThreadSlot();
    Slot &s = g_slots[slot_];
    prev_what_ = s.what.load(std::memory_order_relaxed);
    prev_detail_ = s.detail.load(std::memory_order_relaxed);
    prev_since_ = s.since.load(std::memory_order_relaxed);
    s.what.store(what, std::memory_order_relaxed);
    s.detail.store(detail, std::memory_order_relaxed);
    s.since.store(GetTickCount64(), std::memory_order_release);
  }

  Wait::~Wait()
  {
    if (slot_ < 0)
    {
      return;
    }
    Slot &s = g_slots[slot_];
    s.since.store(prev_since_, std::memory_order_release);
    s.what.store(prev_what_, std::memory_order_relaxed);
    s.detail.store(prev_detail_, std::memory_order_relaxed);
  }

  void Wait::Phase(const char *what, uint64_t detail)
  {
    if (slot_ < 0)
    {
      return;
    }
    Slot &s = g_slots[slot_];
    s.what.store(what, std::memory_order_relaxed);
    s.detail.store(detail, std::memory_order_relaxed);
  }

  void OnSwap(NativeDevice *dev)
  {
    if (!Enabled())
    {
      return;
    }
    g_swaps.fetch_add(1, std::memory_order_relaxed);
    g_last_swap_ms.store(GetTickCount64(), std::memory_order_relaxed);
    if (dev)
    {
      g_native_frame.store(dev->frame_index(), std::memory_order_relaxed);
      g_signalled.store(dev->last_signalled_fence(), std::memory_order_relaxed);
      g_dev.store(dev, std::memory_order_release);
    }
    StartThread();
  }
} // namespace pdz::native::watchdog
