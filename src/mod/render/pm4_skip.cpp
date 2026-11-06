// pdz_native_skip_pm4 (pm4_skip.h): the context / constant register runs are not written to the PM4 ring when the
// null GPU backend (which never consumes them) is the only consumer. Hooks on the three leaf run writers and on
// KickOff (the ring dwords per frame, what the command processor parses); counters per frame; 'pdz_pm4:' lines.
//
// Code (TU3, generated\default):
//   sub_826F9298 (recomp.90): mask r4 (64-bit, MSB first), first register r5, source r6 - 4. Per run of set bits:
//     header (count - 1) << 16 | register, then the dwords; stores through device+0 (stwu), the end back to device+0.
//     Ring end (device+4) -> sub_826F91E0, which splits the run and calls ring space sub_826DDE30.
//   sub_826F9450 (recomp.99): the same per 16 dwords (4 float4 constants) per bit; up to 3 type-2 fillers first so the
//     vector stores are 16-byte aligned.
//   sub_826F9358 (recomp.114): 6 dwords (one fetch constant) per bit; up to 1 filler for 8-byte alignment.
//   Callers: sub_826FA8B8 (the flush, recomp.178), sub_826FAAD8 (recomp.72), sub_826F8170 (recomp.182); every call
//   site reloads what it needs from the device after the call (no volatile register is read back), and the masks are
//   cleared by the callers (std r30,48(r31) ... in the flush), not by the leaves.
//   KickOff sub_826DDC30 (recomp.67; its only caller is ring space sub_826DDE30, the 116-site kick / make-space path):
//     the segment kicked is [device+14012, device+0 + 4), before the fence pair KickOff appends.
//
// Threads: every D3D call runs on the game's render thread (shadow_record.h: single producer), the Swap included, so
// the counters are plain relaxed load + store (no lock prefix) and OnSwap reads them on the same thread.
#include "pm4_skip.h"

#include <atomic>
#include <bit>
#include <cstdint>
#include <mutex>
#include <string>

#include <intrin.h>
#include <windows.h>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>

#include "shadow.h"        // pdz_render_shadow_assert
#include "shadow_record.h" // Load32, IsGuestPointer
#include "gpu_sync.h"      // NullBackend
#include "frame_timing.h"  // pdz::timing::Enabled

REXCVAR_DEFINE_INT32(pdz_native_skip_pm4, 0, "Render",
                     "Stage 2 M5, graphics_backend null only: the guest D3D library stops copying the context "
                     "registers and the shader / fetch constants into the PM4 ring (about 85% of its dwords). The "
                     "native renderer reads them from the D3D device, and the null command processor never uses them; "
                     "every synchronisation packet (fences, WAIT_REG_MEM, interrupts and callbacks, the presentation "
                     "gate, gamma, Swap) and the draw packets are still written and executed. 0 = off (the default again: "
                     "in play, logs 276-277, 1 gave regular ~32 ms hitches every 1-10 s, all in wait.guest_fence; log "
                     "278 with 0 had none; the saving was only 0.1-0.3 ms a frame), 1 = skip (logs 252-259; ignored, "
                     "with one line, with the GPU plugin or pdz_render_shadow_assert), 2 = count "
                     "only: write everything and count / time what 1 would skip. Modes 1 and 2 log 'pdz_pm4:' lines "
                     "(every 600 frames while pdz_frame_timing is on, and a SUMMARY at test exit over the Swaps after "
                     "pdz_frame_timing_skip): runs skipped or "
                     "written per frame, the guest's time writing them, "
                     "ring kicks and the dwords the command processor parses per frame. Taken at each guest Swap")
    .range(0, 2)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DECLARE(int32_t, pdz_frame_timing_skip); // frame_timing.cpp: the SUMMARY counts the same Swaps as pdz_timing's

REX_EXTERN(__imp__sub_826F9298); // register run writer (context registers, bool / loop constants)
REX_EXTERN(__imp__sub_826F9450); // ALU constant writer (0x4000 VS / 0x4400 PS)
REX_EXTERN(__imp__sub_826F9358); // fetch constant writer (0x4800)
REX_EXTERN(__imp__sub_826DDC30); // KickOff

namespace pdz::pm4skip
{
  namespace
  {
    enum Writer : uint32_t
    {
      kRuns = 0,
      kAlu = 1,
      kFetch = 2,
      kWriterCount = 3,
    };
    constexpr uint32_t kUnitDwords[kWriterCount] = {1, 16, 6};
    constexpr const char *kWriterName[kWriterCount] = {"register runs", "ALU constants", "fetch constants"};
    constexpr uint32_t kFirstContextRegister = 0x2000; // config registers below (scratch, COHER, DC_LUT, ...)
    constexpr uint32_t kSampleMask = 7;                // time one forwarded call in 8
    constexpr uint32_t kDeviceSegmentStart = 14012;    // KickOff: start of the segment not yet kicked
    constexpr uint64_t kWindowFrames = 600;

    // Cumulative counters (never reset; OnSwap takes the deltas). One writer: the render thread.
    struct Totals
    {
      std::atomic<uint64_t> calls[kWriterCount] = {};  // calls with a first register >= 0x2000 (mode 1: skipped)
      std::atomic<uint64_t> dwords[kWriterCount] = {}; // their payload + one header per run (fillers not counted)
      std::atomic<uint64_t> config_calls{0};           // calls below 0x2000 (always written; none expected in TU3)
      std::atomic<uint64_t> config_dwords{0};
      std::atomic<uint64_t> sampled_cycles{0}; // forwarded calls, 1 in 8 timed, x8
      std::atomic<uint64_t> kicks{0};
      std::atomic<uint64_t> kicked_dwords{0};
    };
    Totals g_t;

    // The mode of the frame in progress (0 off, 1 skip, 2 count), set by OnSwap; 0 before the first Swap.
    std::atomic<int32_t> g_mode{0};
    uint32_t g_sample = 0; // render thread only
    uint64_t g_swaps = 0;  // every guest Swap (render thread; the same numbering as the pdz_timing lines)

    inline void Bump(std::atomic<uint64_t> &a, uint64_t v)
    {
      a.store(a.load(std::memory_order_relaxed) + v, std::memory_order_relaxed);
    }

    inline uint64_t RunDwords(uint64_t mask, uint32_t unit)
    {
      // Payload, plus one type-0 header per run of set bits (a run starts where the next higher bit is clear).
      return uint64_t(std::popcount(mask)) * unit + uint64_t(std::popcount(mask & ~(mask >> 1)));
    }

    // Before a leaf writer: true = skip the call (mode 1, context / constant registers). `timed`: forward with the
    // sampled timer.
    inline bool Before(Writer w, const PPCContext &ctx, bool &timed)
    {
      const int32_t mode = g_mode.load(std::memory_order_relaxed);
      if (mode == 0)
      {
        return false;
      }
      const uint64_t dwords = RunDwords(ctx.r4.u64, kUnitDwords[w]);
      if (ctx.r5.u32 >= kFirstContextRegister)
      {
        Bump(g_t.calls[w], 1);
        Bump(g_t.dwords[w], dwords);
        if (mode == 1)
        {
          return true;
        }
      }
      else
      {
        Bump(g_t.config_calls, 1);
        Bump(g_t.config_dwords, dwords);
      }
      timed = ((++g_sample) & kSampleMask) == 0;
      return false;
    }

    inline void AfterTimed(uint64_t t0)
    {
      Bump(g_t.sampled_cycles, (__rdtsc() - t0) * (kSampleMask + 1));
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Per-frame accounting (OnSwap / LogSummary).
    // ---------------------------------------------------------------------------------------------------------------
    struct Snap
    {
      uint64_t calls[kWriterCount] = {};
      uint64_t dwords[kWriterCount] = {};
      uint64_t config_calls = 0, config_dwords = 0, cycles = 0, kicks = 0, kicked = 0;
    };

    Snap Take()
    {
      Snap s;
      for (uint32_t w = 0; w < kWriterCount; ++w)
      {
        s.calls[w] = g_t.calls[w].load(std::memory_order_relaxed);
        s.dwords[w] = g_t.dwords[w].load(std::memory_order_relaxed);
      }
      s.config_calls = g_t.config_calls.load(std::memory_order_relaxed);
      s.config_dwords = g_t.config_dwords.load(std::memory_order_relaxed);
      s.cycles = g_t.sampled_cycles.load(std::memory_order_relaxed);
      s.kicks = g_t.kicks.load(std::memory_order_relaxed);
      s.kicked = g_t.kicked_dwords.load(std::memory_order_relaxed);
      return s;
    }

    struct Acc
    {
      uint64_t frames = 0;
      uint64_t frames_mode[3] = {};
      uint64_t first_swap = 0, last_swap = 0;
      Snap sum;
      uint64_t max_dwords = 0; // context / constant dwords in one frame
      uint64_t max_kicked = 0;

      void Add(const Snap &d, int32_t mode, uint64_t swap)
      {
        if (!frames)
        {
          first_swap = swap;
        }
        last_swap = swap;
        ++frames;
        ++frames_mode[mode];
        uint64_t frame_dwords = 0;
        for (uint32_t w = 0; w < kWriterCount; ++w)
        {
          sum.calls[w] += d.calls[w];
          sum.dwords[w] += d.dwords[w];
          frame_dwords += d.dwords[w];
        }
        sum.config_calls += d.config_calls;
        sum.config_dwords += d.config_dwords;
        sum.cycles += d.cycles;
        sum.kicks += d.kicks;
        sum.kicked += d.kicked;
        max_dwords = frame_dwords > max_dwords ? frame_dwords : max_dwords;
        max_kicked = d.kicked > max_kicked ? d.kicked : max_kicked;
      }
    };

    struct State
    {
      std::mutex mutex; // OnSwap / LogSummary (once per frame), never on the per-call path
      Snap last;
      int32_t requested_logged = -1, effective_logged = -1;
      Acc window, session;
      uint64_t tsc0 = 0;
      int64_t qpc0 = 0;
      bool summary_done = false;
    };
    State g_s;

    int64_t Qpc()
    {
      LARGE_INTEGER v;
      QueryPerformanceCounter(&v);
      return v.QuadPart;
    }

    // TSC MHz from the session so far (calibrated against QPC since the first active frame).
    double TscMhz(const State &s)
    {
      LARGE_INTEGER f;
      QueryPerformanceFrequency(&f);
      const double seconds = double(Qpc() - s.qpc0) / double(f.QuadPart);
      return seconds > 0.5 ? double(__rdtsc() - s.tsc0) / seconds / 1e6 : 0.0;
    }

    void LogAcc(const Acc &a, const std::string &tag, double mhz)
    {
      if (!a.frames)
      {
        return;
      }
      const double f = double(a.frames);
      const char *mode = a.frames_mode[1] && a.frames_mode[2] ? "mixed 1 / 2"
                         : a.frames_mode[1]                    ? "1 skip"
                                                               : "2 count only";
      const char *verb = a.frames_mode[1] && !a.frames_mode[2] ? "skipped (not written)"
                         : a.frames_mode[1]                    ? "skipped in mode-1 frames, written in mode-2 frames"
                                                               : "written (mode 1 would skip them)";
      const uint64_t calls = a.sum.calls[kRuns] + a.sum.calls[kAlu] + a.sum.calls[kFetch];
      const uint64_t dwords = a.sum.dwords[kRuns] + a.sum.dwords[kAlu] + a.sum.dwords[kFetch];
      const double ms = mhz > 0.0 ? double(a.sum.cycles) / (mhz * 1000.0) / f : -1.0;
      REXLOG_INFO("pdz_pm4: {} swaps {}-{}, {} frames, mode {} | context / constant runs {}: calls {:.1f}/frame, "
                  "dwords {:.0f}/frame ({} {:.0f}, {} {:.0f}, {} {:.0f}), max {} in a frame | config runs (below "
                  "0x2000, always written) {:.2f}/frame | guest time in the run writers it forwarded {} ms/frame "
                  "(1 call in 8 timed, x8) | ring kicks {:.1f}/frame, dwords kicked {:.0f}/frame (max {} in a frame): "
                  "the ring the command processor parses (before KickOff's fence pair)",
                  tag, a.first_swap, a.last_swap, a.frames, mode, verb, double(calls) / f, double(dwords) / f,
                  kWriterName[kRuns], double(a.sum.dwords[kRuns]) / f, kWriterName[kAlu],
                  double(a.sum.dwords[kAlu]) / f, kWriterName[kFetch], double(a.sum.dwords[kFetch]) / f,
                  a.max_dwords, double(a.sum.config_calls) / f, ms >= 0.0 ? fmt::format("{:.3f}", ms) : "n/a",
                  double(a.sum.kicks) / f, double(a.sum.kicked) / f, a.max_kicked);
    }

    void LogMode(int32_t requested, int32_t effective, uint64_t swap)
    {
      if (requested == 1 && effective == 0)
      {
        if (!gpusync::NullBackend())
        {
          REXLOG_INFO("pdz_pm4: pdz_native_skip_pm4 1 not in effect: the GPU plugin renders from these registers "
                      "(the skip needs graphics_backend null)");
        }
        else
        {
          REXLOG_WARN("pdz_pm4: pdz_native_skip_pm4 1 ignored while pdz_render_shadow_assert is on (the assert "
                      "compares the ring the flush wrote with the shadow)");
        }
        return;
      }
      switch (effective)
      {
      case 1:
        REXLOG_INFO("pdz_pm4: pdz_native_skip_pm4 1 from swap {}: the context registers and the shader / fetch "
                    "constants (sub_826F9298 / sub_826F9450 / sub_826F9358, first register 0x2000 and up) are no "
                    "longer written to the PM4 ring; fences, scratch, WAIT_REG_MEM, INTERRUPT / InsertCallback, the "
                    "presentation gate, COHER, gamma, draw / shader-load packets and XE_SWAP still are. The null "
                    "command processor's resolve count (RB_MODECONTROL) and the census register totals now count "
                    "without these registers",
                    swap);
        break;
      case 2:
        REXLOG_INFO("pdz_pm4: pdz_native_skip_pm4 2 from swap {}: count only (everything is written; the lines "
                    "count and time what mode 1 would skip)",
                    swap);
        break;
      default:
        REXLOG_INFO("pdz_pm4: pdz_native_skip_pm4 off from swap {}: every register run is written", swap);
        break;
      }
    }
  } // namespace

  void OnSwap()
  {
    const uint64_t swap = ++g_swaps;
    const int32_t mode_now = g_mode.load(std::memory_order_relaxed);
    int32_t requested = REXCVAR_GET(pdz_native_skip_pm4);
    requested = requested < 0 ? 0 : (requested > 2 ? 2 : requested);
    if (mode_now == 0 && requested == 0)
    {
      return; // off and staying off: nothing counted, nothing to log (one cvar load per Swap)
    }
    State &s = g_s;
    std::lock_guard<std::mutex> lock(s.mutex);
    const Snap now = Take();
    if (mode_now != 0)
    {
      Snap d;
      for (uint32_t w = 0; w < kWriterCount; ++w)
      {
        d.calls[w] = now.calls[w] - s.last.calls[w];
        d.dwords[w] = now.dwords[w] - s.last.dwords[w];
      }
      d.config_calls = now.config_calls - s.last.config_calls;
      d.config_dwords = now.config_dwords - s.last.config_dwords;
      d.cycles = now.cycles - s.last.cycles;
      d.kicks = now.kicks - s.last.kicks;
      d.kicked = now.kicked - s.last.kicked;
      s.window.Add(d, mode_now, swap);
      if (swap > uint64_t(REXCVAR_GET(pdz_frame_timing_skip) > 0 ? REXCVAR_GET(pdz_frame_timing_skip) : 0))
      {
        s.session.Add(d, mode_now, swap); // the SUMMARY: Swaps after pdz_frame_timing_skip, as the pdz_timing SUMMARY
      }
      if (s.window.frames >= kWindowFrames)
      {
        if (pdz::timing::Enabled()) // the window lines go with the pdz_timing ones (mode 1 is the default)
        {
          LogAcc(s.window, "window", TscMhz(s));
        }
        s.window = Acc();
      }
    }
    s.last = now;

    int32_t effective = requested;
    if (requested == 1 && (!gpusync::NullBackend() || REXCVAR_GET(pdz_render_shadow_assert)))
    {
      effective = 0;
    }
    if (requested != s.requested_logged || effective != s.effective_logged)
    {
      s.requested_logged = requested;
      s.effective_logged = effective;
      LogMode(requested, effective, swap);
    }
    if (effective != 0 && !s.tsc0)
    {
      s.tsc0 = __rdtsc();
      s.qpc0 = Qpc();
    }
    g_mode.store(effective, std::memory_order_relaxed);
  }

  void LogSummary(const char *why)
  {
    State &s = g_s;
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.summary_done || !s.session.frames)
    {
      return;
    }
    s.summary_done = true;
    LogAcc(s.session, fmt::format("SUMMARY ({})", why ? why : ""), TscMhz(s));
  }
} // namespace pdz::pm4skip

// ---------------------------------------------------------------------------------------------------------------------
// Hooks. Off (mode 0): one relaxed load, then the original.
// ---------------------------------------------------------------------------------------------------------------------
REX_HOOK_RAW(sub_826F9298)
{
  bool timed = false;
  if (pdz::pm4skip::Before(pdz::pm4skip::kRuns, ctx, timed))
  {
    return;
  }
  if (!timed)
  {
    __imp__sub_826F9298(ctx, base);
    return;
  }
  const uint64_t t0 = __rdtsc();
  __imp__sub_826F9298(ctx, base);
  pdz::pm4skip::AfterTimed(t0);
}

REX_HOOK_RAW(sub_826F9450)
{
  bool timed = false;
  if (pdz::pm4skip::Before(pdz::pm4skip::kAlu, ctx, timed))
  {
    return;
  }
  if (!timed)
  {
    __imp__sub_826F9450(ctx, base);
    return;
  }
  const uint64_t t0 = __rdtsc();
  __imp__sub_826F9450(ctx, base);
  pdz::pm4skip::AfterTimed(t0);
}

REX_HOOK_RAW(sub_826F9358)
{
  bool timed = false;
  if (pdz::pm4skip::Before(pdz::pm4skip::kFetch, ctx, timed))
  {
    return;
  }
  if (!timed)
  {
    __imp__sub_826F9358(ctx, base);
    return;
  }
  const uint64_t t0 = __rdtsc();
  __imp__sub_826F9358(ctx, base);
  pdz::pm4skip::AfterTimed(t0);
}

// KickOff(device r3): the segment it kicks, counted before forwarding (13 / 25 / 48 per frame in Datacore / MP / the
// heavy MP of log 440: fence pairs per frame).
REX_HOOK_RAW(sub_826DDC30)
{
  if (pdz::pm4skip::g_mode.load(std::memory_order_relaxed) != 0)
  {
    const uint32_t device = ctx.r3.u32;
    if (pdz::shadow::IsGuestPointer(device))
    {
      const uint32_t next = pdz::shadow::Load32(base, device) + 4;
      const uint32_t start = pdz::shadow::Load32(base, device + pdz::pm4skip::kDeviceSegmentStart);
      pdz::pm4skip::Bump(pdz::pm4skip::g_t.kicks, 1);
      if (next > start && next - start < (64u << 20))
      {
        pdz::pm4skip::Bump(pdz::pm4skip::g_t.kicked_dwords, (next - start) >> 2);
      }
    }
  }
  __imp__sub_826DDC30(ctx, base);
}
