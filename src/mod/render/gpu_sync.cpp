// GPU synchronisation log and the null GPU backend switch (gpu_sync.h).
//
// Device fields read (TU3 D3D, generated\default; gpu_census.cpp reads the same): +10384 pointer to the fence the GPU
// writes (EVENT_WRITE_SHD; the CPU polls it in sub_826DE258), +10388 pointer to the scratch block (+0 the CPU mask of a
// pending callback, cleared by the interrupt handler; +4 the presentation-interval gate, cleared by the vblank
// handler), +10396 the last fence issued, +15600 the vblank count of D3D's interrupt handler (sub_826DE528 source 0).
// sub_826DE258(device, fence pointer r4) waits while [r4] is ahead of the fence the GPU wrote.
//
// Threads: the hooks run on the game's D3D threads and only store atomics (and log the first events). The heartbeat
// thread (low priority, started by the first hook or by the null backend at setup) reads guest memory only after
// VirtualQuery says it is committed (a device freed at the boot / game device switch must not fault). It stops at the
// test exit (LogSummary) and at process exit (atexit, before the logger is torn down).
#include "gpu_sync.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

#include <windows.h>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/graphics/null_gpu.h>
#include <rex/logging.h>

#include "shadow_record.h"
#include "game_pace.h"     // pdz_null_refresh_hz (stage 2 M5)
#include "native/native.h" // ChooseScaleAtStartup / ChoosePresentTimingAtStartup (stage 2 M3)
#include "native/native_recthread.h" // pdz_native_record_thread (stage 2 M5)

REXCVAR_DEFINE_BOOL(pdz_sync_log, false, "Render",
                    "GPU synchronisation log ('pdz_sync:' lines): the D3D sync milestones (first guest Swap in and out, "
                    "the first fence block, the BlockUntilIdle returns, InsertCallback calls with the native de-rez "
                    "readback time), a heartbeat with the fences the GPU passed, the D3D vblank count and the Swap rate "
                    "(every second for 20 s, then every 10 s), a STALL line when the Swap, a fence block or "
                    "BlockUntilIdle stops for 2-3 s, and a SUMMARY at test exit. On by itself with "
                    "graphics_backend null.");

namespace pdz::gpusync
{
  namespace
  {
    constexpr uint64_t kTickMs = 250;
    constexpr uint64_t kFastBeatMs = 1000;
    constexpr uint64_t kFastBeatUntilMs = 20000;
    constexpr uint64_t kSlowBeatMs = 10000;
    constexpr uint64_t kSwapStallMs = 3000; // a level load holds the Swap for up to ~5 s (Datacore: 4.5 s)
    constexpr uint64_t kBlockStallMs = 2000;
    constexpr uint64_t kRepeatMs = 5000;
    constexpr uint32_t kLoggedReturns = 3;  // BlockUntilIdle returns and InsertCallback calls logged one by one

    // D3D device fields (see the top of this file).
    constexpr uint32_t kDeviceFencePointer = 10384;
    constexpr uint32_t kDeviceScratchBlock = 10388;
    constexpr uint32_t kDeviceFenceIssued = 10396;
    constexpr uint32_t kDeviceVblankCount = 15600;

    struct State
    {
      std::atomic<uint8_t *> base{nullptr};
      std::atomic<uint32_t> device{0};
      std::atomic<uint64_t> start_ms{0}; // the heartbeat's t = 0
      // Swap (sub_826DF960).
      std::atomic<uint64_t> swaps_in{0}, swaps_out{0};
      std::atomic<uint64_t> first_swap_ms{0}, last_swap_ms{0}, swap_since_ms{0};
      // BlockUntilIdle (sub_826DE6A0).
      std::atomic<uint64_t> idle_in{0}, idle_out{0}, idle_since_ms{0}, idle_us_total{0}, idle_us_max{0};
      std::atomic<uint32_t> idle_lr{0}, idle_tid{0};
      // The fence block (sub_826DE258).
      std::atomic<uint64_t> fence_in{0}, fence_out{0}, fence_since_ms{0}, fence_us_total{0}, fence_us_max{0};
      std::atomic<uint32_t> fence_lr{0}, fence_awaited{0}, fence_tid{0};
      // InsertCallback (sub_826DE910).
      std::atomic<uint64_t> icb{0}, icb_since_ms{0}, icb_native_us{0}, icb_native_us_max{0};
      std::atomic<uint32_t> icb_lr{0}, icb_callback{0}, icb_type{0};
      // The heartbeat thread.
      std::atomic<bool> thread_started{false};
      std::atomic<bool> stop{false};
      std::atomic<bool> exited{false};
      std::atomic<bool> summary_done{false};
    };
    State g;

    uint64_t NowMs() { return GetTickCount64(); }

    // How long ago `since` was, 0 when unset or newer than `now` (a hook can store a newer tick between the reader's
    // clock read and its load: a raw difference would wrap to ~1.8e19 ms).
    uint64_t Age(uint64_t now, uint64_t since) { return since && now > since ? now - since : 0; }

    void Max(std::atomic<uint64_t> &target, uint64_t value)
    {
      uint64_t seen = target.load(std::memory_order_relaxed);
      while (value > seen && !target.compare_exchange_weak(seen, value, std::memory_order_relaxed))
      {
      }
    }

    // A big-endian guest word, read only from committed, readable memory (off-thread readers).
    bool SafeLoad32(uint8_t *base, uint32_t address, uint32_t &out)
    {
      if (!base || !pdz::shadow::IsGuestPointer(address) || (address & 3))
      {
        return false;
      }
      const uint8_t *p = pdz::shadow::GuestPtr(base, address); // the physical window's host offset included
      MEMORY_BASIC_INFORMATION mbi{};
      if (!VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) ||
          !(mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                           PAGE_EXECUTE_WRITECOPY)))
      {
        return false;
      }
      out = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
      return true;
    }

    struct D3DSnap
    {
      uint32_t device = 0;
      bool fence = false, scratch = false, vblank = false;
      uint32_t fence_ptr = 0, fence_gpu = 0, fence_issued = 0;
      uint32_t scratch_ptr = 0, scratch0 = 0, scratch4 = 0;
      uint32_t vblank_count = 0;
    };

    D3DSnap ReadD3D()
    {
      D3DSnap d;
      uint8_t *base = g.base.load(std::memory_order_acquire);
      d.device = g.device.load(std::memory_order_relaxed);
      if (!base || !d.device)
      {
        return d;
      }
      d.fence = SafeLoad32(base, d.device + kDeviceFencePointer, d.fence_ptr) &&
                SafeLoad32(base, d.fence_ptr, d.fence_gpu) &&
                SafeLoad32(base, d.device + kDeviceFenceIssued, d.fence_issued);
      d.scratch = SafeLoad32(base, d.device + kDeviceScratchBlock, d.scratch_ptr) &&
                  SafeLoad32(base, d.scratch_ptr, d.scratch0) && SafeLoad32(base, d.scratch_ptr + 4, d.scratch4);
      d.vblank = SafeLoad32(base, d.device + kDeviceVblankCount, d.vblank_count);
      return d;
    }

    // Callers of the hooked functions (TU3; gpu_census.cpp has the full tables).
    const char *IdleSite(uint32_t lr)
    {
      switch (lr)
      {
      case 0x826DF1EC:
      case 0x826DEDD4:
        return ": D3D ring init";
      case 0x827459E8:
        return ": game sub_82745908";
      case 0x8239D57C:
        return ": game";
      default:
        return "";
      }
    }
    const char *FenceSite(uint32_t lr)
    {
      switch (lr)
      {
      case 0x826DD5E4:
      case 0x826DD610:
        return ": KickOff ring segment reuse (Swap path)";
      case 0x826DDF0C:
        return ": ring space";
      case 0x826DE710:
        return ": BlockUntilIdle";
      case 0x826DE8EC:
        return ": BlockUntilNotBusy";
      case 0x826E1B48:
        return ": resource lock";
      case 0x826E1934:
        return ": sub_826E17F0";
      default:
        return "";
      }
    }
    const char *CallbackSite(uint32_t lr)
    {
      switch (lr)
      {
      case 0x825F654C:
        return ": de-rez snapshot sub_825F64E8";
      case 0x82AE0590:
      case 0x82AE3C00:
        return ": callback 82AE0308 site";
      default:
        return "";
      }
    }

    std::string Describe(const D3DSnap &d)
    {
      if (!d.device)
      {
        return "no D3D device seen yet";
      }
      std::string s = fmt::format("D3D device {:08X}: ", d.device);
      s += d.fence ? fmt::format("fences passed (GPU wrote) {}, issued {} (fence memory {:08X})", d.fence_gpu,
                                 d.fence_issued, d.fence_ptr)
                   : std::string("fence not readable");
      s += d.scratch ? fmt::format("; scratch block {:08X}: +0 (callback CPU mask) {:08X}, +4 (presentation gate) {:08X}",
                                   d.scratch_ptr, d.scratch0, d.scratch4)
                     : std::string("; scratch block not readable");
      s += d.vblank ? fmt::format("; D3D vblank count {}", d.vblank_count) : std::string("; D3D vblank count not readable");
      return s;
    }

    // The threads in a fence block / BlockUntilIdle / InsertCallback / Swap now.
    std::string Waits(uint64_t now)
    {
      std::string s;
      const uint64_t fence_since = g.fence_since_ms.load(std::memory_order_relaxed);
      if (g.fence_in.load(std::memory_order_relaxed) != g.fence_out.load(std::memory_order_relaxed) && fence_since)
      {
        const uint32_t lr = g.fence_lr.load(std::memory_order_relaxed);
        s += fmt::format(" | thread {} in the D3D fence block for {} ms awaiting fence {} (lr {:08X}{})",
                         g.fence_tid.load(std::memory_order_relaxed), Age(now, fence_since),
                         g.fence_awaited.load(std::memory_order_relaxed), lr, FenceSite(lr));
      }
      const uint64_t idle_since = g.idle_since_ms.load(std::memory_order_relaxed);
      if (g.idle_in.load(std::memory_order_relaxed) != g.idle_out.load(std::memory_order_relaxed) && idle_since)
      {
        const uint32_t lr = g.idle_lr.load(std::memory_order_relaxed);
        s += fmt::format(" | thread {} in BlockUntilIdle for {} ms (lr {:08X}{})",
                         g.idle_tid.load(std::memory_order_relaxed), Age(now, idle_since), lr, IdleSite(lr));
      }
      const uint64_t icb_since = g.icb_since_ms.load(std::memory_order_relaxed);
      if (icb_since)
      {
        s += fmt::format(" | InsertCallback in progress for {} ms (callback {:08X}; the native de-rez readback runs first)",
                         Age(now, icb_since), g.icb_callback.load(std::memory_order_relaxed));
      }
      const uint64_t swap_since = g.swap_since_ms.load(std::memory_order_relaxed);
      if (swap_since)
      {
        s += fmt::format(" | a guest Swap call has been in progress for {} ms", Age(now, swap_since));
      }
      return s;
    }

    std::string CountsLine()
    {
      return fmt::format("Swap {} in / {} out | fence block {} in / {} out | BlockUntilIdle {} in / {} out | "
                         "InsertCallback {}",
                         g.swaps_in.load(std::memory_order_relaxed), g.swaps_out.load(std::memory_order_relaxed),
                         g.fence_in.load(std::memory_order_relaxed), g.fence_out.load(std::memory_order_relaxed),
                         g.idle_in.load(std::memory_order_relaxed), g.idle_out.load(std::memory_order_relaxed),
                         g.icb.load(std::memory_order_relaxed));
    }

    DWORD WINAPI HeartbeatMain(void *)
    {
      using SetThreadDescriptionFn = HRESULT(WINAPI *)(HANDLE, PCWSTR);
      if (HMODULE kernel = GetModuleHandleW(L"kernel32.dll"))
      {
        if (auto set_name = reinterpret_cast<SetThreadDescriptionFn>(GetProcAddress(kernel, "SetThreadDescription")))
        {
          set_name(GetCurrentThread(), L"pdz sync heartbeat");
        }
      }
      const uint64_t start = g.start_ms.load(std::memory_order_relaxed);
      uint64_t next_beat = kFastBeatMs, last_beat_t = 0;
      uint64_t last_swaps = 0;
      D3DSnap last = ReadD3D();
      uint64_t swap_reported = 0, swap_stall_from = 0;
      uint64_t fence_reported = 0, fence_reported_since = 0;
      uint64_t idle_reported = 0, idle_reported_since = 0;
      while (!g.stop.load(std::memory_order_acquire))
      {
        Sleep(DWORD(kTickMs));
        if (g.stop.load(std::memory_order_acquire))
        {
          break;
        }
        const uint64_t now = NowMs();
        const uint64_t t = now - start;

        // The Swap: after the first one, a gap of kSwapStallMs.
        const uint64_t last_swap = g.last_swap_ms.load(std::memory_order_relaxed);
        if (swap_reported && last_swap != swap_stall_from)
        {
          REXLOG_INFO("pdz_sync: guest Swap resumed (the gap lasted at least {} ms)", swap_reported - swap_stall_from);
          swap_reported = 0;
        }
        if (last_swap && Age(now, last_swap) >= kSwapStallMs && (!swap_reported || now - swap_reported >= kRepeatMs))
        {
          swap_reported = now;
          swap_stall_from = last_swap;
          REXLOG_WARN("pdz_sync: STALL: no guest Swap for {} ms (a level load can take ~5 s){} | {} | {}",
                      Age(now, last_swap), Waits(now), Describe(ReadD3D()), CountsLine());
        }
        // A fence block / BlockUntilIdle that does not return.
        const uint64_t fence_since = g.fence_since_ms.load(std::memory_order_relaxed);
        const bool in_fence = g.fence_in.load(std::memory_order_relaxed) != g.fence_out.load(std::memory_order_relaxed);
        if (fence_reported && (!in_fence || fence_since != fence_reported_since))
        {
          REXLOG_INFO("pdz_sync: the D3D fence block reported stuck returned (after at least {} ms)",
                      fence_reported - fence_reported_since);
          fence_reported = 0;
        }
        if (in_fence && fence_since && Age(now, fence_since) >= kBlockStallMs &&
            (!fence_reported || now - fence_reported >= kRepeatMs))
        {
          fence_reported = now;
          fence_reported_since = fence_since;
          REXLOG_WARN("pdz_sync: STALL: D3D fence block for {} ms: the CPU waits for a fence the GPU thread has not "
                      "written{} | {} | {}",
                      Age(now, fence_since), Waits(now), Describe(ReadD3D()), CountsLine());
        }
        const uint64_t idle_since = g.idle_since_ms.load(std::memory_order_relaxed);
        const bool in_idle = g.idle_in.load(std::memory_order_relaxed) != g.idle_out.load(std::memory_order_relaxed);
        if (idle_reported && (!in_idle || idle_since != idle_reported_since))
        {
          REXLOG_INFO("pdz_sync: the BlockUntilIdle reported stuck returned (after at least {} ms)",
                      idle_reported - idle_reported_since);
          idle_reported = 0;
        }
        if (in_idle && idle_since && Age(now, idle_since) >= kBlockStallMs &&
            (!idle_reported || now - idle_reported >= kRepeatMs))
        {
          idle_reported = now;
          idle_reported_since = idle_since;
          REXLOG_WARN("pdz_sync: STALL: BlockUntilIdle for {} ms: the GPU thread has not drained the ring{} | {} | {}",
                      Age(now, idle_since), Waits(now), Describe(ReadD3D()), CountsLine());
        }

        // Heartbeat.
        if (t >= next_beat)
        {
          const double seconds = double(std::max<uint64_t>(t - last_beat_t, 1)) / 1000.0;
          const D3DSnap d = ReadD3D();
          const uint64_t swaps = g.swaps_in.load(std::memory_order_relaxed);
          std::string fences = d.fence ? fmt::format("fences passed (GPU wrote) {} ({:+.1f}/s), issued {}", d.fence_gpu,
                                                     last.fence && last.device == d.device
                                                         ? double(int32_t(d.fence_gpu - last.fence_gpu)) / seconds
                                                         : 0.0,
                                                     d.fence_issued)
                                       : std::string("fences not readable");
          std::string vblanks = d.vblank ? fmt::format("D3D vblank count {} ({:+.1f}/s)", d.vblank_count,
                                                       last.vblank && last.device == d.device
                                                           ? double(int32_t(d.vblank_count - last.vblank_count)) / seconds
                                                           : 0.0)
                                         : std::string("D3D vblank count not readable");
          std::string scratch = d.scratch ? fmt::format("scratch block +0 {:08X} +4 {:08X}", d.scratch0, d.scratch4)
                                          : std::string("scratch block not readable");
          REXLOG_INFO("pdz_sync: t={}s | Swap {:.1f}/s | {} | {} | {} | {} | device {:08X}{}", t / 1000,
                      double(swaps - last_swaps) / seconds, fences, vblanks, scratch, CountsLine(), d.device, Waits(now));
          last = d;
          last_swaps = swaps;
          last_beat_t = t;
          next_beat = t + (t < kFastBeatUntilMs ? kFastBeatMs : kSlowBeatMs);
        }
      }
      g.exited.store(true, std::memory_order_release);
      return 0;
    }

    // Before the logger goes away at exit: stop the heartbeat (it may be logging).
    void StopAtExit()
    {
      g.stop.store(true, std::memory_order_release);
      for (int i = 0; i < 100 && !g.exited.load(std::memory_order_acquire); ++i)
      {
        Sleep(10);
      }
    }

    void StartThread()
    {
      bool expected = false;
      if (!g.thread_started.compare_exchange_strong(expected, true))
      {
        return;
      }
      g.start_ms.store(NowMs(), std::memory_order_relaxed);
      HANDLE thread = CreateThread(nullptr, 256 * 1024, HeartbeatMain, nullptr, 0, nullptr);
      if (!thread)
      {
        g.exited.store(true, std::memory_order_release);
        REXLOG_WARN("pdz_sync: the heartbeat thread could not be created (milestone lines only)");
        return;
      }
      SetThreadPriority(thread, THREAD_PRIORITY_BELOW_NORMAL);
      CloseHandle(thread);
      std::atexit(StopAtExit);
      REXLOG_INFO("pdz_sync: on ({}): heartbeat every second for 20 s, then every 10 s; a STALL line when the Swap stops "
                  "for 3 s or a fence block / BlockUntilIdle for 2 s",
                  g_null_backend ? "graphics_backend null" : "pdz_sync_log");
    }

    // Every hook: the guest memory base and the device the hang reports read.
    void Note(uint8_t *base, uint32_t device)
    {
      if (!g.base.load(std::memory_order_relaxed))
      {
        g.base.store(base, std::memory_order_release);
      }
      if (device && pdz::shadow::IsGuestPointer(device))
      {
        g.device.store(device, std::memory_order_relaxed);
      }
      if (!g.thread_started.load(std::memory_order_relaxed))
      {
        StartThread();
      }
    }
  } // namespace

  namespace
  {
    void SelectBackend(std::string_view backend, bool plugin_configured, bool plugin_created)
    {
      if (backend != "null")
      {
        return;
      }
      if (plugin_created && rex::graphics::null_gpu::Active())
      {
        g_null_backend = true;
        REXLOG_INFO("pdz_native: graphics_backend null: null GPU backend in effect: the GPU plugin services the ring and "
                    "its synchronisation without rendering. For this session the shadow recorder, the native renderer "
                    "and the native output are on, F9 and the per-frame shader-miss fallback are off (a frame with a "
                    "missing shader is shown as drawn), the auto-diff, pass dump and draw dump refuse to start, and "
                    "pdz_sync_log is on");
        StartThread();
        return;
      }
      if (!plugin_configured)
      {
        REXLOG_ERROR("pdz_native: graphics_backend null requested, but no GPU plugin is configured (gpu_plugin is "
                     "empty): no graphics system services the GPU ring at all; NO null GPU backend in effect");
      }
      else if (!plugin_created)
      {
        REXLOG_ERROR("pdz_native: graphics_backend null requested, but the GPU plugin could not create it (a "
                     "rexgpu-xenos.dll without the null backend, SDK patch sdk-12?): the automatic backend is loaded "
                     "instead and the emulator renders as usual; NO null GPU backend in effect");
      }
      else
      {
        REXLOG_ERROR("pdz_native: graphics_backend null requested and the plugin created a graphics system, but it did "
                     "not report the null backend active (rex/graphics/null_gpu.h: an older rexruntime.dll?); NO null "
                     "GPU backend in effect");
      }
    }
  } // namespace

  void OnBackendSelected(std::string_view backend, bool plugin_configured, bool plugin_created)
  {
    SelectBackend(backend, plugin_configured, plugin_created);
    // Stage 2 M3, once the backend (and with it the plugin's cvars) is known: the native resolution scale and the
    // present timing (pdz_native_scale_follow, pdz_native_present_timing).
    pdz::native::ChooseScaleAtStartup();
    pdz::native::ChoosePresentTimingAtStartup();
    // Stage 2 M5 step 4: pdz_native_record_thread (the worker and the ring write pointer filter, before the guest runs).
    pdz::native::recthread::ChooseAtStartup();
    // Stage 2 M5: guest vblank pacing (pdz_null_refresh_hz, null backend only) and the game clock log; before the GPU's
    // presentation setup starts the vsync worker and the presenter's vblank thread.
    pdz::pace::ChooseAtStartup(g_null_backend);
  }

  void OnAppShutdown() { pdz::native::recthread::Stop(); }

  void OnSwapEnter(uint8_t *base, uint32_t device)
  {
    Note(base, device);
    const uint64_t now = NowMs();
    const uint64_t n = g.swaps_in.fetch_add(1, std::memory_order_relaxed) + 1;
    g.last_swap_ms.store(now, std::memory_order_relaxed);
    g.swap_since_ms.store(now, std::memory_order_relaxed);
    if (n == 1)
    {
      g.first_swap_ms.store(now, std::memory_order_relaxed);
      REXLOG_INFO("pdz_sync: first guest Swap entered (thread {}, {}); backend: {}", GetCurrentThreadId(),
                  Describe(ReadD3D()), g_null_backend ? "null GPU backend" : "the GPU plugin renders");
    }
  }

  void OnSwapLeave()
  {
    g.swap_since_ms.store(0, std::memory_order_relaxed);
    const uint64_t n = g.swaps_out.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n == 1)
    {
      REXLOG_INFO("pdz_sync: first guest Swap returned after {} ms",
                  NowMs() - g.first_swap_ms.load(std::memory_order_relaxed));
    }
  }

  void OnBlockUntilIdleEnter(uint8_t *base, uint32_t device, uint32_t lr)
  {
    Note(base, device);
    g.idle_lr.store(lr, std::memory_order_relaxed);
    g.idle_tid.store(GetCurrentThreadId(), std::memory_order_relaxed);
    g.idle_since_ms.store(NowMs(), std::memory_order_relaxed);
    if (g.idle_in.fetch_add(1, std::memory_order_relaxed) == 0)
    {
      REXLOG_INFO("pdz_sync: first BlockUntilIdle entered (lr {:08X}{}, thread {}): the CPU waits until the GPU "
                  "thread has executed the ring",
                  lr, IdleSite(lr), GetCurrentThreadId());
    }
  }

  void OnBlockUntilIdleLeave(uint32_t lr, int64_t us)
  {
    g.idle_since_ms.store(0, std::memory_order_relaxed);
    const uint64_t n = g.idle_out.fetch_add(1, std::memory_order_relaxed) + 1;
    const uint64_t u = uint64_t(std::max<int64_t>(us, 0));
    g.idle_us_total.fetch_add(u, std::memory_order_relaxed);
    Max(g.idle_us_max, u);
    if (n <= kLoggedReturns)
    {
      REXLOG_INFO("pdz_sync: BlockUntilIdle #{} returned after {:.1f} ms (lr {:08X}{})", n, double(u) / 1000.0, lr,
                  IdleSite(lr));
    }
    else if (u >= 1000000)
    {
      REXLOG_WARN("pdz_sync: BlockUntilIdle #{} took {:.1f} ms (lr {:08X}{})", n, double(u) / 1000.0, lr, IdleSite(lr));
    }
  }

  void OnFenceBlockEnter(uint8_t *base, uint32_t device, uint32_t awaited_ptr, uint32_t lr)
  {
    Note(base, device);
    // The awaited value: the game thread reads it right after (D3D's own pointer), so a plain load is safe here.
    const uint32_t awaited = pdz::shadow::IsGuestPointer(awaited_ptr) ? pdz::shadow::Load32(base, awaited_ptr) : 0;
    g.fence_awaited.store(awaited, std::memory_order_relaxed);
    g.fence_lr.store(lr, std::memory_order_relaxed);
    g.fence_tid.store(GetCurrentThreadId(), std::memory_order_relaxed);
    g.fence_since_ms.store(NowMs(), std::memory_order_relaxed);
    if (g.fence_in.fetch_add(1, std::memory_order_relaxed) == 0)
    {
      REXLOG_INFO("pdz_sync: first D3D fence block (lr {:08X}{}, thread {}): awaiting fence {}; {}", lr, FenceSite(lr),
                  GetCurrentThreadId(), awaited, Describe(ReadD3D()));
    }
  }

  void OnFenceBlockLeave(int64_t us)
  {
    g.fence_since_ms.store(0, std::memory_order_relaxed);
    const uint64_t n = g.fence_out.fetch_add(1, std::memory_order_relaxed) + 1;
    const uint64_t u = uint64_t(std::max<int64_t>(us, 0));
    g.fence_us_total.fetch_add(u, std::memory_order_relaxed);
    Max(g.fence_us_max, u);
    if (n == 1)
    {
      REXLOG_INFO("pdz_sync: first D3D fence block returned after {:.1f} ms: the GPU thread wrote the fence",
                  double(u) / 1000.0);
    }
  }

  void OnInsertCallbackEnter(uint8_t *base, uint32_t device, uint32_t lr, uint32_t type, uint32_t callback)
  {
    Note(base, device);
    g.icb_lr.store(lr, std::memory_order_relaxed);
    g.icb_type.store(type, std::memory_order_relaxed);
    g.icb_callback.store(callback, std::memory_order_relaxed);
    g.icb_since_ms.store(NowMs(), std::memory_order_relaxed);
    const uint64_t n = g.icb.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= kLoggedReturns)
    {
      REXLOG_INFO("pdz_sync: InsertCallback #{} (lr {:08X}{}, callback {:08X}, type {}, thread {}): the GPU thread "
                  "will raise an interrupt that runs the callback",
                  n, lr, CallbackSite(lr), callback, type, GetCurrentThreadId());
    }
  }

  void OnInsertCallbackLeave(int64_t native_us, int64_t total_us)
  {
    g.icb_since_ms.store(0, std::memory_order_relaxed);
    const uint64_t u = uint64_t(std::max<int64_t>(native_us, 0));
    g.icb_native_us.fetch_add(u, std::memory_order_relaxed);
    Max(g.icb_native_us_max, u);
    const uint64_t n = g.icb.load(std::memory_order_relaxed);
    if (n <= kLoggedReturns)
    {
      REXLOG_INFO("pdz_sync: InsertCallback #{} left after {:.1f} ms ({:.1f} ms in the native de-rez readback)", n,
                  double(std::max<int64_t>(total_us, 0)) / 1000.0, double(u) / 1000.0);
    }
  }

  std::string D3DState()
  {
    if (!g.device.load(std::memory_order_relaxed))
    {
      return std::string();
    }
    return Describe(ReadD3D()) + Waits(NowMs()) + " | " + CountsLine();
  }

  void LogSummary(const char *why)
  {
    g.stop.store(true, std::memory_order_release);
    if (!Enabled() || g.summary_done.exchange(true))
    {
      return;
    }
    const uint64_t idle_out = g.idle_out.load(std::memory_order_relaxed);
    REXLOG_INFO("pdz_sync: SUMMARY ({}): backend {} | {} | fence block {:.1f} ms in all (longest {:.1f} ms) | "
                "BlockUntilIdle {} returned, {:.1f} ms in all (longest {:.1f} ms) | InsertCallback {} ({:.1f} ms in the "
                "native de-rez readback, longest {:.1f} ms) | {}{}",
                why, g_null_backend ? "null" : "GPU plugin", CountsLine(),
                double(g.fence_us_total.load(std::memory_order_relaxed)) / 1000.0,
                double(g.fence_us_max.load(std::memory_order_relaxed)) / 1000.0, idle_out,
                double(g.idle_us_total.load(std::memory_order_relaxed)) / 1000.0,
                double(g.idle_us_max.load(std::memory_order_relaxed)) / 1000.0, g.icb.load(std::memory_order_relaxed),
                double(g.icb_native_us.load(std::memory_order_relaxed)) / 1000.0,
                double(g.icb_native_us_max.load(std::memory_order_relaxed)) / 1000.0, Describe(ReadD3D()),
                // pdz_native_async_readback: sync / async snapshot readbacks and DeResThread's waits.
                pdz::native::Enabled() ? " | " + pdz::native::SnapshotReadbackSummary() : std::string());
  }
} // namespace pdz::gpusync
