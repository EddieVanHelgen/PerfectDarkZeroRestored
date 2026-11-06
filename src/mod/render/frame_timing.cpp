// Frame timing (frame_timing.h): the pdz_frame_timing switches, the per-frame samples and the pdz_timing lines.
//
// A sample is one Swap-to-Swap interval: its wall-clock length and the native / shadow CPU time accumulated by
// the Scope timers since the previous Swap. The first pdz_frame_timing_skip intervals (boot, menus, loading)
// are not kept. Samples live for the session (reserved for 20,000 frames; the vector grows past that).
//
// GPU and command-processor columns (stage 2 M1: the same numbers with the GPU plugin and with the null backend):
//   native gpu ms     the native renderer's own GPU time: timestamps around each of its command lists, summed per
//                     native frame and read back when the frame's slot comes round again (native_device.cpp), so a
//                     sample is the native frame that finished about three frames earlier; n/a while the renderer is
//                     off or before its first readback.
//   plugin gpu ms     the GPU plugin's host GPU time ('T' rows of the SDK's frame_pacing_log: timestamps around its
//                     submissions) since the previous Swap; needs --frame_pacing_log=true; "none" with the null
//                     backend (it submits nothing).
//   cp busy ms        the command processor's busy time ('K' rows: wall - idle - WAIT_REG_MEM - waits on the host GPU)
//                     since the previous Swap; needs --frame_pacing_log=true; both backends.
//   cp thread cpu ms  the CPU time of the GPU command processor thread ("GPU Commands", QueryThreadCycleTime, TSC
//                     calibrated) per Swap over the last second; both backends; no frame_pacing_log needed.
//   process gpu busy  the process's share of the 3D engine over the last second (PDH "\GPU Engine(pid_<pid>_*
//                     engtype_3D)\Utilization Percentage", summed over its 3D engine instances: the plugin, the native
//                     renderer and the presenter together) and that share of each frame's wall time in ms.
// The last two come from a sampler thread (lowest priority, once a second, started at the first Swap with the switch
// on); the Swap only reads atomics and the SDK's frame_pacing totals.
//
// Scene split (stage 2 M5, m5_frontend.md): each sample carries the scene of the frame its Swap closes, as the
// present step's hook saw it (game_pace.h pdz::pace::TakeScene: front end = the present step outside state 21, level
// = in state 21, other = a Swap that did not come from the present step). A 'pdz_timing: scenes' line follows each
// pdz_timing line: per scene the frame count and the Swap interval avg / p50 / p99 / max, and how many intervals were
// over 20 ms and over 40 ms (the front end's every-third-frame 50-64 ms stall shows there).
//
// Spike log (pdz_frame_timing_spike_ms N > 0; stage 2 M5 spikes, PDZRenderResearch\native\stage2\m5\m5_spikes.md):
// every kept Swap interval over N ms gets one 'pdz_timing: spike' line (at most kSpikeLines = 600 per run) with that frame's
// phases (pdz_frame_timing_phases), the Swap thread's CPU time, the record thread's counters for the interval
// (native_recthread.h TakeSpikeTotals), the event counters (frame_timing.h Event) and the D3D vblank count, and a cause
// tag: the largest component of the frame (a wait for the record thread is tagged with the worker's largest group,
// "rt.worker/<group>"). LogSummary adds a 'pdz_timing: spikes SUMMARY' line: spikes by scene and the tag histogram.
#include "frame_timing.h"
#include "cpu_placement.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <thread>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <windows.h>
#include <intrin.h>
#include <pdh.h>
#include <psapi.h>
#include <pdhmsg.h>
#include <tlhelp32.h>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/frame_pacing.h>

#include "game_pace.h"
#include "game_thread_sampler.h"
#include "gpu_sync.h"
#include "native/native_recthread.h"
#include "shadow_record.h"

REXCVAR_DEFINE_BOOL(pdz_frame_timing, false, "Render",
                    "Frame timing: wall-clock time between guest Swap calls and the CPU time spent in the native "
                    "renderer and the shadow recorder per frame, plus the native renderer's GPU time (timestamps), the "
                    "GPU plugin's GPU time and command processor busy time (with frame_pacing_log), the GPU command "
                    "processor thread's CPU time and the process's GPU busy share (Windows GPU counters). Logs a "
                    "pdz_timing line every 600 frames and a SUMMARY line at test exit (--pdz_test_exit_s).");
REXCVAR_DEFINE_INT32(pdz_frame_timing_skip, 300, "Render",
                     "Frame timing: Swap intervals left out of the statistics at the start (boot, menus, loading).");
REXCVAR_DEFINE_INT32(pdz_frame_timing_phases, 0, "Render",
                     "Frame timing (needs pdz_frame_timing): split the native CPU time per frame into exclusive phases "
                     "(draws, clears, resolves, texture / PSO / shader work, submit, present chain, the presenter "
                     "before / after our refresher, fence and flush waits) plus the render mutex wait and the guest "
                     "Swap call, logged as a 'pdz_timing: phases' line after each pdz_timing line. 0 = off, 1 = one "
                     "rdtsc pair per draw and per other scope, 2 = also the draw stages (about ten rdtsc per draw) "
                     "and the geometry hash (and, cpusplit, the guest D3D state setters' own time in guest.d3d).")
    .range(0, 2);
REXCVAR_DEFINE_INT32(pdz_frame_timing_spike_ms, 0, "Render",
                     "Frame timing (needs pdz_frame_timing; best with pdz_frame_timing_phases 1): one 'pdz_timing: "
                     "spike' line for every kept Swap interval longer than this many ms (level and front end) with "
                     "what that frame did: the game thread split (phases, thread cpu vs blocked), the record thread's "
                     "busy time, commands and the game thread's waits for it, and the frame's events (InsertCallback "
                     "and the de-rez snapshot readback, GPU waits, PSO / texture / resource creation, descriptor ring, "
                     "present, guest vblanks), with a cause tag (the largest component). At most 600 lines per run; a "
                     "'pdz_timing: spikes SUMMARY' line at test exit (spikes by scene, cause histogram). 0 = off.")
    .range(0, 100000);
REXCVAR_DEFINE_BOOL(pdz_frame_timing_spike_level_only, false, "Render",
                    "Frame timing spike log (pdz_frame_timing_spike_ms): only frames of the level scene (state 21) "
                    "count and get lines; the front end's paced 60 fps frames are not spikes at 120 Hz.");

namespace pdz::timing
{
  namespace
  {
    constexpr size_t kWindow = 600;     // frames per periodic line
    constexpr size_t kReserve = 20000;  // samples reserved up front (about 5.5 minutes at 60 fps)
    constexpr float kNone = std::numeric_limits<float>::quiet_NaN(); // no measurement for this frame
    constexpr uint32_t kPhases = uint32_t(Phase::kCount);
    constexpr uint32_t kInside = uint32_t(Phase::kInsideCount);
    const char *const kPhaseNames[kPhases] = {
        "draw",        "d.shaders",   "d.targets",  "d.omset",       "d.decode",        "d.pipeline",
        "d.viewport",  "d.geometry",  "d.geomhash", "d.cbfloat",     "d.constants",     "d.tables",
        "d.texlookup", "d.samplers",  "d.record",   "d.tail",        "d.skipped",
        "tex.hash",    "tex.upload",  "pso.create", "shader.wait",   "clear",           "resolve",
        "callback",    "frame.begin", "wait.fence", "wait.flush",    "submit",          "present.chain",
        "present.sdk_pre", "present.sdk_post", "swap.other", "wait.mutex", "guest.swap", "shadow.record",
        "shadow.assert", "guest.d3d", "guest.gate", "wait.ring", "wait.guest_fence", "wait.block_idle",
        "wait.not_busy", "wait.adaptive", "wait.fe_spin", "wait.fe_pace", "wait.yield", "rt.enqueue", "wait.rt"};
    static_assert(sizeof(kPhaseNames) / sizeof(kPhaseNames[0]) == kPhases, "one name per phase");
    // The draw phases (level 1: draw; level 2: draw + d.*): their sum per draw call is the 'native draw ns/draw'.
    constexpr uint32_t kDrawPhaseFirst = uint32_t(Phase::kDraw);
    constexpr uint32_t kDrawPhaseLast = uint32_t(Phase::kDrawSkipped);
    // Phases that are waits rather than work (subtracted from the native / outside time to estimate the guest's own
    // code from the thread's CPU time). Blocking waits only: the cpusplit spin waits (wait.ring, wait.adaptive,
    // wait.fe_spin, wait.fe_pace, wait.yield: SwitchToThread loops, on the CPU while nothing else is runnable) count
    // as CPU time like work, so they stay out of this list (still outside native, so out of 'rest').
    constexpr Phase kWaitPhases[] = {Phase::kFenceWait,      Phase::kFlushWait,      Phase::kShaderWait,
                                     Phase::kPresentSdkPost, Phase::kMutexWait,      Phase::kGuestSwap,
                                     Phase::kWaitGuestFence, Phase::kWaitBlockIdle,  Phase::kWaitNotBusy,
                                     Phase::kRecWait};

    // Spike log (pdz_frame_timing_spike_ms).
    constexpr uint64_t kSpikeLines = 600;          // spike lines per run; the SUMMARY counts the rest
    constexpr uint32_t kEvents = uint32_t(Event::kCount);
    constexpr uint32_t kDeviceVblankCount = 15600; // D3D device: its interrupt handler's vblank count (gpu_sync.cpp)
    // Cause groups: phases whose sum is one component of a spike. Inside: the native entry points (the record thread's
    // with pdz_native_record_thread, else the game thread's); draw (draw + d.*) and the unattributed native rest are
    // added by hand. Outside: the game thread's time outside them.
    struct Group
    {
      const char *tag;
      Phase phases[4];
      uint32_t count;
    };
    constexpr Group kInsideGroups[] = {
        {"readback", {Phase::kCallback, Phase::kFlushWait}, 2}, // the de-rez snapshot: copy, Flush (GPU wait), write
        {"pso", {Phase::kPsoCreate}, 1},
        {"tex.upload", {Phase::kTextureUpload}, 1},
        {"tex.hash", {Phase::kTextureHash}, 1},
        {"shader.wait", {Phase::kShaderWait}, 1},
        {"fence", {Phase::kFenceWait}, 1},
        {"present", {Phase::kPresentChain, Phase::kPresentSdkPre, Phase::kPresentSdkPost}, 3},
        {"clear", {Phase::kClear}, 1},
        {"resolve", {Phase::kResolve}, 1},
        {"submit", {Phase::kSubmit, Phase::kFrameBegin}, 2},
        {"swap.other", {Phase::kSwapOther}, 1},
    };
    constexpr Group kOutsideGroups[] = {
        {"wait.rt", {Phase::kRecWait}, 1}, // retagged "rt.worker/<the worker's largest group>"
        {"rt.enqueue", {Phase::kRecEnqueue}, 1},
        {"wait.mutex", {Phase::kMutexWait}, 1},
        {"guest.swap", {Phase::kGuestSwap}, 1},
        {"guest.d3d", {Phase::kGuestD3D, Phase::kGuestGate}, 2},
        {"shadow", {Phase::kShadowRecord, Phase::kShadowAssert}, 2},
        {"wait.ring", {Phase::kWaitRing}, 1},
        {"guest.wait", {Phase::kWaitGuestFence, Phase::kWaitBlockIdle, Phase::kWaitNotBusy}, 3},
        {"spin", {Phase::kWaitAdaptive, Phase::kWaitFeSpin, Phase::kWaitFePace, Phase::kWaitYield}, 4},
    };

    // Per frame phase ms, kept beside the samples (same index) while pdz_frame_timing_phases is on; NaN = off.
    struct PhaseSample
    {
      float ms[kPhases];
      float other_ms; // native cpu - the inside phases (unattributed native time; can be slightly negative)
      float thread_cpu_ms; // the Swap thread's CPU time in the interval (QueryThreadCycleTime); NaN = n/a
      float draws;         // native draw calls (IssueDraw) in the interval
      int32_t level;
      // Phases on (MANSION_HITCH.md): the Swap thread's logical processor at the previous and this Swap (-1 = n/a) and
      // the process's page faults in the interval (soft + hard, GetProcessMemoryInfo; -1 = n/a).
      int16_t core_from, core_to;
      int32_t page_faults;
    };

    // Spike log: the counters' deltas over one Swap interval (TakeSpikeDeltas).
    struct SpikeFrame
    {
      uint64_t events[kEvents] = {};
      bool rt = false; // the record thread is on
      pdz::native::recthread::SpikeTotals rt_delta; // counters: the interval's deltas; maxima: as taken
      bool vblank = false; // the D3D vblank count was read at both Swaps
      uint32_t vblanks = 0;
    };

    struct Sample
    {
      float frame_ms;
      float native_ms;
      float shadow_ms;
      float native_gpu_ms; // NaN: none
      float plugin_gpu_ms; // NaN: frame_pacing_log off
      float cp_busy_ms;    // NaN: frame_pacing_log off
      float cp_thread_ms;  // NaN: not sampled (yet)
      float gpu_busy_pct;  // NaN: not sampled (yet) / PDH unavailable
      float present_delay_ms = kNone; // NaN: no native frame presented in the interval (mean of those that were)
      float present_gap_ms = kNone;   // NaN: none (the longest gap between presenter refreshes in the interval)
      pdz::pace::Scene scene = pdz::pace::Scene::kOther; // the frame this Swap closes (scene split)
    };

    struct Stats
    {
      size_t count = 0; // samples with a value
      double avg = 0.0, p50 = 0.0, p99 = 0.0, max = 0.0;
    };

    struct State
    {
      std::mutex mutex; // OnSwap / LogSummary only (once per frame), never on the per-draw path
      int64_t last = 0; // QPC ticks at the previous Swap (0 = none yet)
      uint64_t swaps = 0; // Swap calls seen with the switch on
      uint64_t first_swap = 0; // the Swap that closed samples[0]
      double ms_per_tick = 0.0;
      bool level_seen = false; // the first level frame was logged (load time, compiler experiments)
      std::vector<Sample> samples;
      std::vector<PhaseSample> phases; // same index as samples
      std::vector<float> scratch;
      // Phases: the cumulative cycle counters at the previous Swap, and the TSC calibration against QPC.
      uint64_t phase_prev[kPhases] = {};
      int64_t calib_qpc = 0;
      uint64_t calib_tsc = 0;
      double tsc_per_ms = 0.0;
      int32_t phase_level = 0; // pdz_frame_timing_phases as set at the previous Swap
      // The Swap thread's CPU cycles (QueryThreadCycleTime) and draw calls at the previous Swap (phases on).
      DWORD thread_id = 0;
      ULONG64 thread_cycles = 0;
      uint64_t draw_calls = 0;
      int16_t core_prev = -1;      // the Swap thread's logical processor at the previous Swap (phases on)
      int64_t page_faults_prev = -1; // the process's page fault count at the previous Swap (phases on)
      // The SDK's frame_pacing totals at the previous Swap.
      rex::ui::frame_pacing::Totals pacing{};
      bool have_pacing = false;
      // Spike log: the counters at the previous Swap (spike_have: taken there with the log on), the spike counts and
      // the cause histogram (only touched when a spike happens).
      bool spike_have = false;
      uint64_t event_prev[kEvents] = {};
      pdz::native::recthread::SpikeTotals rt_prev;
      bool vblank_have = false;
      uint32_t vblank_prev = 0;
      int32_t spike_ms = 0;  // the threshold of the last spike (for the SUMMARY)
      uint64_t spike_frames = 0; // kept frames checked against the threshold
      uint64_t spikes = 0, spike_lines = 0;
      uint64_t spike_scene[3] = {}; // by pdz::pace::Scene
      double spike_max_ms = 0.0;
      uint64_t spike_max_swap = 0;
      std::vector<std::pair<std::string, uint64_t>> spike_causes;
    };

    State g_state;
    std::atomic<int64_t> g_acc[uint32_t(Bucket::kCount)] = {};
    std::atomic<uint64_t> g_native_gpu_us{0};
    std::atomic<uint32_t> g_native_gpu_frames{0};
    // AddPresent (any thread): sums since the last Swap, the longest gap, and the label of the last caller.
    std::atomic<uint64_t> g_present_delay_us{0};
    std::atomic<uint32_t> g_presents{0};
    std::atomic<uint64_t> g_present_gap_max_us{0};
    std::atomic<const char *> g_present_where{nullptr};
    // Stage 2 M5 step 4: native cpu runs on the record thread (SetNativeOffThread), not inside the game thread's frame.
    std::atomic<bool> g_native_off_thread{false};

    // -------------------------------------------------------------------------------------------------------------
    // Sampler thread: the GPU command processor thread's CPU time and the process's GPU busy share, once a second.
    // -------------------------------------------------------------------------------------------------------------
    namespace sampler
    {
      std::atomic<bool> g_started{false};
      std::atomic<bool> g_stop{false};
      std::atomic<bool> g_exited{false};
      std::atomic<uint64_t> g_swaps{0};            // Swap calls with the switch on (OnSwap)
      std::atomic<int64_t> g_cp_ns_per_swap{-1};   // -1 = n/a
      std::atomic<int64_t> g_gpu_pct_milli{-1};    // percent x 1000, -1 = n/a
      // pdz_frame_timing_phases (stage 2 M5 drawcost): the busiest threads of the process over the last second.
      std::atomic<DWORD> g_swap_tid{0};            // the thread of the latest guest Swap (OnSwap)
      std::mutex g_threads_mutex;
      std::string g_threads_line;                  // "name tid P% ..." (empty: not sampled yet)

      using GetThreadDescriptionFn = HRESULT(WINAPI *)(HANDLE, PWSTR *);
      GetThreadDescriptionFn ThreadDescriptionFn()
      {
        HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
        return kernel ? reinterpret_cast<GetThreadDescriptionFn>(
                            reinterpret_cast<void *>(GetProcAddress(kernel, "GetThreadDescription")))
                      : nullptr;
      }

      struct ThreadCpu
      {
        HANDLE handle = nullptr;
        ULONG64 last = 0;
        bool have = false;
        bool seen = false;
        std::string name; // GetThreadDescription (ASCII, at most 40 characters), empty = none
      };

      // Every thread of the process: its CPU cycles (QueryThreadCycleTime, TSC ticks) since the previous call; the
      // eight busiest go to g_threads_line as "name tid P%" (P = percent of one core over `tsc_delta`), the Swap
      // thread marked [swap]. Sampler thread only (lowest priority, once a second, phases on).
      void SampleThreads(std::unordered_map<DWORD, ThreadCpu> &threads, uint64_t tsc_delta)
      {
        static const GetThreadDescriptionFn get_description = ThreadDescriptionFn();
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot == INVALID_HANDLE_VALUE)
        {
          return;
        }
        const DWORD pid = GetCurrentProcessId();
        for (auto &kv : threads)
        {
          kv.second.seen = false;
        }
        struct Busy
        {
          uint64_t cycles;
          DWORD tid;
          const std::string *name;
        };
        std::vector<Busy> busy;
        uint64_t total = 0;
        THREADENTRY32 entry{};
        entry.dwSize = sizeof(entry);
        for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry))
        {
          if (entry.th32OwnerProcessID != pid)
          {
            continue;
          }
          ThreadCpu &t = threads[entry.th32ThreadID]; // references stay valid across rehashing
          t.seen = true;
          if (!t.handle)
          {
            t.handle = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ThreadID);
            if (!t.handle)
            {
              continue;
            }
            PWSTR description = nullptr;
            if (get_description && SUCCEEDED(get_description(t.handle, &description)) && description)
            {
              for (const wchar_t *p = description; *p && t.name.size() < 40; ++p)
              {
                t.name += (*p >= 32 && *p < 127) ? char(*p) : '?';
              }
              LocalFree(description);
            }
          }
          ULONG64 cycles = 0;
          if (!QueryThreadCycleTime(t.handle, &cycles))
          {
            continue;
          }
          if (t.have && cycles >= t.last)
          {
            busy.push_back({cycles - t.last, entry.th32ThreadID, &t.name});
            total += cycles - t.last;
          }
          t.last = cycles;
          t.have = true;
        }
        CloseHandle(snapshot);
        std::sort(busy.begin(), busy.end(), [](const Busy &a, const Busy &b) { return a.cycles > b.cycles; });
        const DWORD swap_tid = g_swap_tid.load(std::memory_order_relaxed);
        std::string line;
        if (!busy.empty() && tsc_delta)
        {
          line = fmt::format("process {:.0f}% over {} threads |", 100.0 * double(total) / double(tsc_delta),
                             busy.size());
          for (size_t i = 0; i < busy.size() && i < 8; ++i)
          {
            const Busy &b = busy[i];
            line += fmt::format(" {} {} {:.0f}%{}", b.name->empty() ? std::string("(unnamed)") : *b.name, b.tid,
                                100.0 * double(b.cycles) / double(tsc_delta), b.tid == swap_tid ? " [swap]" : "");
            line += i + 1 < busy.size() && i + 1 < 8 ? "," : "";
          }
        }
        for (auto it = threads.begin(); it != threads.end();)
        {
          if (!it->second.seen)
          {
            if (it->second.handle)
            {
              CloseHandle(it->second.handle);
            }
            it = threads.erase(it);
          }
          else
          {
            ++it;
          }
        }
        std::lock_guard<std::mutex> lock(g_threads_mutex);
        g_threads_line = std::move(line);
      }

      void CloseThreads(std::unordered_map<DWORD, ThreadCpu> &threads)
      {
        for (auto &kv : threads)
        {
          if (kv.second.handle)
          {
            CloseHandle(kv.second.handle);
          }
        }
        threads.clear();
      }

      // pdh.dll, loaded at run time (the executable does not link pdh.lib).
      struct Pdh
      {
        using OpenQueryFn = PDH_STATUS(WINAPI *)(LPCWSTR, DWORD_PTR, PDH_HQUERY *);
        using AddCounterFn = PDH_STATUS(WINAPI *)(PDH_HQUERY, LPCWSTR, DWORD_PTR, PDH_HCOUNTER *);
        using CollectFn = PDH_STATUS(WINAPI *)(PDH_HQUERY);
        using ArrayFn = PDH_STATUS(WINAPI *)(PDH_HCOUNTER, DWORD, LPDWORD, LPDWORD, PPDH_FMT_COUNTERVALUE_ITEM_W);
        using CloseFn = PDH_STATUS(WINAPI *)(PDH_HQUERY);
        OpenQueryFn open = nullptr;
        AddCounterFn add = nullptr;
        CollectFn collect = nullptr;
        ArrayFn array = nullptr;
        CloseFn close = nullptr;
        PDH_HQUERY query = nullptr;
        PDH_HCOUNTER counter = nullptr;
        std::vector<uint8_t> buffer;
        std::string path;

        bool Open(DWORD pid, std::string &why)
        {
          HMODULE dll = LoadLibraryW(L"pdh.dll");
          if (!dll)
          {
            why = "pdh.dll not available";
            return false;
          }
          open = reinterpret_cast<OpenQueryFn>(reinterpret_cast<void *>(GetProcAddress(dll, "PdhOpenQueryW")));
          add = reinterpret_cast<AddCounterFn>(reinterpret_cast<void *>(GetProcAddress(dll, "PdhAddEnglishCounterW")));
          collect = reinterpret_cast<CollectFn>(reinterpret_cast<void *>(GetProcAddress(dll, "PdhCollectQueryData")));
          array = reinterpret_cast<ArrayFn>(
              reinterpret_cast<void *>(GetProcAddress(dll, "PdhGetFormattedCounterArrayW")));
          close = reinterpret_cast<CloseFn>(reinterpret_cast<void *>(GetProcAddress(dll, "PdhCloseQuery")));
          if (!open || !add || !collect || !array || !close)
          {
            why = "pdh.dll lacks the query functions";
            return false;
          }
          PDH_STATUS status = open(nullptr, 0, &query);
          if (status != ERROR_SUCCESS)
          {
            why = fmt::format("PdhOpenQueryW failed (0x{:08X})", uint32_t(status));
            return false;
          }
          path = fmt::format("\\GPU Engine(pid_{}_*engtype_3D)\\Utilization Percentage", pid);
          const std::wstring wide(path.begin(), path.end());
          status = add(query, wide.c_str(), 0, &counter);
          if (status != ERROR_SUCCESS)
          {
            why = fmt::format("PdhAddEnglishCounterW('{}') failed (0x{:08X})", path, uint32_t(status));
            close(query);
            query = nullptr;
            return false;
          }
          collect(query); // the first sample: utilisation needs two
          return true;
        }

        // Sum of the matching 3D engine instances over the interval since the previous call; instances = how many.
        bool Sample(double &percent, uint32_t &instances)
        {
          percent = 0.0;
          instances = 0;
          if (collect(query) != ERROR_SUCCESS)
          {
            return false;
          }
          DWORD size = 0, count = 0;
          constexpr DWORD kFormat = PDH_FMT_DOUBLE | PDH_FMT_NOCAP100;
          PDH_STATUS status = array(counter, kFormat, &size, &count, nullptr);
          if (status == PDH_STATUS(PDH_MORE_DATA))
          {
            buffer.resize(size);
            status = array(counter, kFormat, &size, &count, reinterpret_cast<PPDH_FMT_COUNTERVALUE_ITEM_W>(buffer.data()));
          }
          if (status != ERROR_SUCCESS)
          {
            return status == PDH_STATUS(PDH_CSTATUS_NO_INSTANCE); // no instance yet: 0 instances, no value
          }
          const auto *items = reinterpret_cast<const PDH_FMT_COUNTERVALUE_ITEM_W *>(buffer.data());
          for (DWORD i = 0; i < count; ++i)
          {
            if (items[i].FmtValue.CStatus == PDH_CSTATUS_VALID_DATA || items[i].FmtValue.CStatus == PDH_CSTATUS_NEW_DATA)
            {
              percent += items[i].FmtValue.doubleValue;
              ++instances;
            }
          }
          return true;
        }

        void Close()
        {
          if (query)
          {
            close(query);
            query = nullptr;
          }
        }
      };

      // The SDK names the command processor thread "GPU Commands (<handle>)" (SetThreadDescription).
      HANDLE FindThread(const wchar_t *prefix, DWORD &tid)
      {
        using GetThreadDescriptionFn = HRESULT(WINAPI *)(HANDLE, PWSTR *);
        HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
        auto get_description = kernel ? reinterpret_cast<GetThreadDescriptionFn>(
                                            reinterpret_cast<void *>(GetProcAddress(kernel, "GetThreadDescription")))
                                      : nullptr;
        if (!get_description)
        {
          return nullptr;
        }
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot == INVALID_HANDLE_VALUE)
        {
          return nullptr;
        }
        const DWORD pid = GetCurrentProcessId();
        const size_t prefix_length = wcslen(prefix);
        HANDLE found = nullptr;
        THREADENTRY32 entry{};
        entry.dwSize = sizeof(entry);
        for (BOOL more = Thread32First(snapshot, &entry); more && !found; more = Thread32Next(snapshot, &entry))
        {
          if (entry.th32OwnerProcessID != pid)
          {
            continue;
          }
          HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ThreadID);
          if (!thread)
          {
            continue;
          }
          PWSTR description = nullptr;
          if (SUCCEEDED(get_description(thread, &description)) && description)
          {
            if (wcsncmp(description, prefix, prefix_length) == 0)
            {
              found = thread;
              tid = entry.th32ThreadID;
            }
            LocalFree(description);
          }
          if (found != thread)
          {
            CloseHandle(thread);
          }
        }
        CloseHandle(snapshot);
        return found;
      }

      DWORD WINAPI Main(void *)
      {
        pdz::cpu::PlaceHelperThread("timing sampler"); // pdz_cpu_placement
        using SetThreadDescriptionFn = HRESULT(WINAPI *)(HANDLE, PCWSTR);
        if (HMODULE kernel = GetModuleHandleW(L"kernel32.dll"))
        {
          if (auto set_name = reinterpret_cast<SetThreadDescriptionFn>(
                  reinterpret_cast<void *>(GetProcAddress(kernel, "SetThreadDescription"))))
          {
            set_name(GetCurrentThread(), L"pdz timing sampler");
          }
        }
        Pdh pdh;
        std::string why;
        const bool pdh_ok = pdh.Open(GetCurrentProcessId(), why);
        if (pdh_ok)
        {
          REXLOG_INFO("pdz_timing: process GPU busy from PDH '{}' (sampled once a second)", pdh.path);
        }
        else
        {
          REXLOG_WARN("pdz_timing: process GPU busy unavailable: {}", why);
        }
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        LARGE_INTEGER last_qpc;
        QueryPerformanceCounter(&last_qpc);
        uint64_t last_tsc = __rdtsc();
        uint64_t last_swaps = g_swaps.load(std::memory_order_relaxed);
        HANDLE cp = nullptr;
        DWORD cp_tid = 0;
        uint32_t cp_lookups = 0;
        ULONG64 last_cycles = 0;
        bool have_cycles = false;
        uint32_t pdh_samples = 0;
        bool pdh_instances_logged = false;
        std::unordered_map<DWORD, ThreadCpu> threads; // pdz_frame_timing_phases: per-thread CPU (SampleThreads)
        while (!g_stop.load(std::memory_order_acquire))
        {
          for (int i = 0; i < 4 && !g_stop.load(std::memory_order_acquire); ++i)
          {
            Sleep(250);
          }
          if (g_stop.load(std::memory_order_acquire))
          {
            break;
          }
          LARGE_INTEGER qpc;
          QueryPerformanceCounter(&qpc);
          const uint64_t tsc = __rdtsc();
          const uint64_t swaps = g_swaps.load(std::memory_order_relaxed);
          const double seconds = double(qpc.QuadPart - last_qpc.QuadPart) / double(frequency.QuadPart);

          if (pdh_ok)
          {
            double percent = 0.0;
            uint32_t instances = 0;
            if (pdh.Sample(percent, instances))
            {
              ++pdh_samples;
              if (instances)
              {
                g_gpu_pct_milli.store(int64_t(percent * 1000.0), std::memory_order_relaxed);
                if (!pdh_instances_logged)
                {
                  pdh_instances_logged = true;
                  REXLOG_INFO("pdz_timing: PDH: {} 3D engine instance(s) of this process", instances);
                }
              }
              else if (pdh_samples == 5)
              {
                REXLOG_WARN("pdz_timing: PDH: no 3D engine instance of this process after 5 s (process GPU busy n/a)");
              }
            }
          }

          if (!cp && cp_lookups < 120)
          {
            ++cp_lookups;
            cp = FindThread(L"GPU Commands", cp_tid);
            if (cp)
            {
              REXLOG_INFO("pdz_timing: GPU command processor thread {} found (CPU time sampled once a second)", cp_tid);
            }
          }
          if (cp)
          {
            ULONG64 cycles = 0;
            if (QueryThreadCycleTime(cp, &cycles))
            {
              const uint64_t swaps_done = swaps - last_swaps;
              if (have_cycles && seconds > 0.0 && tsc > last_tsc && swaps_done)
              {
                // QueryThreadCycleTime counts TSC ticks: calibrate them against QPC over the same second.
                const double tsc_per_ns = double(tsc - last_tsc) / (seconds * 1e9);
                const double cp_ns = double(cycles - last_cycles) / tsc_per_ns;
                g_cp_ns_per_swap.store(int64_t(cp_ns / double(swaps_done)), std::memory_order_relaxed);
              }
              last_cycles = cycles;
              have_cycles = true;
            }
            else
            {
              CloseHandle(cp); // the thread is gone (shutdown)
              cp = nullptr;
              have_cycles = false;
            }
          }
          if (g_phase_level.load(std::memory_order_relaxed) > 0 && tsc > last_tsc)
          {
            SampleThreads(threads, tsc - last_tsc); // the busiest threads (drawcost)
          }
          else if (!threads.empty())
          {
            CloseThreads(threads);
          }
          last_qpc = qpc;
          last_tsc = tsc;
          last_swaps = swaps;
        }
        if (cp)
        {
          CloseHandle(cp);
        }
        CloseThreads(threads);
        if (pdh_ok)
        {
          pdh.Close();
        }
        g_exited.store(true, std::memory_order_release);
        return 0;
      }

      // Before static destruction at exit (the thread may be inside PDH).
      void StopAtExit()
      {
        g_stop.store(true, std::memory_order_release);
        for (int i = 0; i < 150 && !g_exited.load(std::memory_order_acquire); ++i)
        {
          Sleep(10);
        }
      }

      void Start()
      {
        bool expected = false;
        if (!g_started.compare_exchange_strong(expected, true))
        {
          return;
        }
        HANDLE thread = CreateThread(nullptr, 256 * 1024, Main, nullptr, 0, nullptr);
        if (!thread)
        {
          g_exited.store(true, std::memory_order_release);
          REXLOG_WARN("pdz_timing: the sampler thread could not be created (no cp thread cpu / process gpu busy)");
          return;
        }
        SetThreadPriority(thread, THREAD_PRIORITY_LOWEST);
        CloseHandle(thread);
        std::atexit(StopAtExit);
      }
    } // namespace sampler

    // Nearest-rank percentile; reorders v.
    double Percentile(std::vector<float> &v, double p)
    {
      const size_t rank = size_t(std::ceil(p * double(v.size())));
      const size_t i = rank == 0 ? 0 : std::min(rank, v.size()) - 1;
      std::nth_element(v.begin(), v.begin() + ptrdiff_t(i), v.end());
      return v[i];
    }

    // Over the samples with a value (NaN = none); get(i) = the value of sample i.
    template <class Get>
    Stats ComputeBy(State &s, size_t begin, size_t end, Get get)
    {
      Stats st;
      s.scratch.clear();
      double sum = 0.0;
      for (size_t i = begin; i < end; ++i)
      {
        const float x = get(i);
        if (std::isnan(x))
        {
          continue;
        }
        s.scratch.push_back(x);
        sum += x;
        st.max = std::max(st.max, double(x));
      }
      st.count = s.scratch.size();
      if (s.scratch.empty())
      {
        return st;
      }
      st.avg = sum / double(s.scratch.size());
      st.p50 = Percentile(s.scratch, 0.50);
      st.p99 = Percentile(s.scratch, 0.99);
      return st;
    }

    Stats Compute(State &s, size_t begin, size_t end, float Sample::*field)
    {
      return ComputeBy(s, begin, end, [&](size_t i) { return s.samples[i].*field; });
    }

    // The phases line (call with s.mutex held): avg/p99 ms per frame of each phase charged in the range.
    void LogPhases(State &s, size_t begin, size_t end, const char *kind)
    {
      if (s.phases.size() < end)
      {
        return;
      }
      int32_t level = 0;
      for (size_t i = begin; i < end; ++i)
      {
        level = std::max(level, s.phases[i].level);
      }
      if (level == 0)
      {
        return; // phases off over the whole range
      }
      const Stats native = Compute(s, begin, end, &Sample::native_ms);
      std::string inside, outside;
      double inside_sum = 0.0;
      for (uint32_t p = 0; p < kPhases; ++p)
      {
        const Stats st = ComputeBy(s, begin, end, [&](size_t i) { return s.phases[i].ms[p]; });
        if (!st.count || st.max <= 0.0)
        {
          continue; // never charged in the range (a level-2 phase at level 1, a wait that never happened)
        }
        std::string &out = p < kInside ? inside : outside;
        out += fmt::format("{}{} {:.3f}/{:.3f}", out.empty() ? "" : ", ", kPhaseNames[p], st.avg, st.p99);
        if (p < kInside)
        {
          inside_sum += st.avg;
        }
      }
      const Stats other = ComputeBy(s, begin, end, [&](size_t i) { return s.phases[i].other_ms; });
      // Per draw call (drawcost): the draw phases' time over the draw calls of the range.
      double draw_ms_sum = 0.0, draws_sum = 0.0;
      size_t draw_frames = 0;
      for (size_t i = begin; i < end; ++i)
      {
        const PhaseSample &ph = s.phases[i];
        if (!ph.level)
        {
          continue;
        }
        for (uint32_t p = kDrawPhaseFirst; p <= kDrawPhaseLast; ++p)
        {
          draw_ms_sum += std::isnan(ph.ms[p]) ? 0.0 : double(ph.ms[p]);
        }
        draws_sum += double(ph.draws);
        ++draw_frames;
      }
      const std::string per_draw =
          draws_sum > 0.0 ? fmt::format("draws {:.0f}/frame, native draw {:.0f} ns/draw", draws_sum / double(draw_frames),
                                        draw_ms_sum * 1.0e6 / draws_sum)
                          : std::string("draws none");
      REXLOG_INFO("pdz_timing: phases ms per frame avg/p99 ({}, level {}) swaps {}-{} | native cpu {:.3f}/{:.3f} = {} "
                  "(sum {:.3f}) + other {:.3f}/{:.3f} | outside native: {} | {} | tsc {:.0f} MHz",
                  kind, level, s.first_swap + begin, s.first_swap + end - 1, native.avg, native.p99,
                  inside.empty() ? std::string("none") : inside, inside_sum, other.avg, other.p99,
                  outside.empty() ? std::string("none") : outside, per_draw, s.tsc_per_ms / 1000.0);

      // The game thread (drawcost): frame = native + outside phases + rest (the guest's own code and its waits outside
      // the hooked calls); the thread's CPU time splits the rest into guest code and waits.
      const auto outside_ms = [&](size_t i) {
        double sum = 0.0;
        for (uint32_t p = kInside; p < kPhases; ++p)
        {
          sum += std::isnan(s.phases[i].ms[p]) ? 0.0 : double(s.phases[i].ms[p]);
        }
        return sum;
      };
      const auto waits_ms = [&](size_t i) {
        double sum = 0.0;
        for (const Phase p : kWaitPhases)
        {
          const float v = s.phases[i].ms[uint32_t(p)];
          sum += std::isnan(v) ? 0.0 : double(v);
        }
        return sum;
      };
      const auto valid = [&](size_t i) { return s.phases[i].level != 0; };
      // The native cpu time the game thread itself spent (none with the record thread: the worker's).
      const bool off_thread = g_native_off_thread.load(std::memory_order_relaxed);
      const auto native_game = [&](size_t i) { return off_thread ? 0.0 : double(s.samples[i].native_ms); };
      const Stats frame = ComputeBy(s, begin, end, [&](size_t i) { return valid(i) ? s.samples[i].frame_ms : kNone; });
      const Stats outside_st = ComputeBy(s, begin, end, [&](size_t i) { return valid(i) ? float(outside_ms(i)) : kNone; });
      const Stats rest = ComputeBy(s, begin, end, [&](size_t i) {
        return valid(i) ? float(double(s.samples[i].frame_ms) - native_game(i) - outside_ms(i)) : kNone;
      });
      const Stats cpu = ComputeBy(s, begin, end, [&](size_t i) { return valid(i) ? s.phases[i].thread_cpu_ms : kNone; });
      const Stats blocked = ComputeBy(s, begin, end, [&](size_t i) {
        return valid(i) && !std::isnan(s.phases[i].thread_cpu_ms) ? s.samples[i].frame_ms - s.phases[i].thread_cpu_ms
                                                                   : kNone;
      });
      // Guest code ~ thread cpu - (native + outside - their waits); rest waits ~ rest - guest code.
      const auto guest_code = [&](size_t i) {
        return double(s.phases[i].thread_cpu_ms) - (native_game(i) + outside_ms(i) - waits_ms(i));
      };
      const Stats code = ComputeBy(s, begin, end, [&](size_t i) {
        return valid(i) && !std::isnan(s.phases[i].thread_cpu_ms) ? float(guest_code(i)) : kNone;
      });
      const Stats rest_wait = ComputeBy(s, begin, end, [&](size_t i) {
        return valid(i) && !std::isnan(s.phases[i].thread_cpu_ms)
                   ? float(double(s.samples[i].frame_ms) - native_game(i) - outside_ms(i) - guest_code(i))
                   : kNone;
      });
      const std::string cpu_part =
          cpu.count ? fmt::format("thread cpu {:.2f}/{:.2f}, blocked {:.2f}/{:.2f} (frame - cpu) | rest = guest code "
                                  "~{:.2f}/{:.2f} + waits ~{:.2f}/{:.2f}",
                                  cpu.avg, cpu.p99, blocked.avg, blocked.p99, code.avg, code.p99, rest_wait.avg,
                                  rest_wait.p99)
                    : std::string("thread cpu n/a");
      REXLOG_INFO("pdz_timing: game thread ms per frame avg/p99 ({}) swaps {}-{} | frame {:.2f}/{:.2f} = native "
                  "{:.2f}/{:.2f}{} + outside {:.2f}/{:.2f} + rest {:.2f}/{:.2f} | {}",
                  kind, s.first_swap + begin, s.first_swap + end - 1, frame.avg, frame.p99,
                  off_thread ? 0.0 : native.avg, off_thread ? 0.0 : native.p99,
                  off_thread ? fmt::format(" (record thread: native {:.2f}/{:.2f} runs there)", native.avg, native.p99)
                             : std::string(),
                  outside_st.avg, outside_st.p99, rest.avg, rest.p99, cpu_part);

      // The game thread split (stage 2 M5 cpusplit): one table-like line, ms per frame avg/p99 of each group over the
      // frames of the range; the groups add up to the frame (avg). rest = the guest's own code outside every timed
      // scope (game logic, the unhooked D3D calls and the D3D work the game inlines, Rare's other waits, the mods'
      // hooks); the guest sampler line below splits it by guest function.
      const auto group = [&](std::initializer_list<Phase> phases, bool with_native) {
        return ComputeBy(s, begin, end, [&, phases](size_t i) {
          if (!valid(i))
          {
            return kNone;
          }
          double sum = with_native ? native_game(i) : 0.0;
          for (const Phase p : phases)
          {
            const float v = s.phases[i].ms[uint32_t(p)];
            sum += std::isnan(v) ? 0.0 : double(v);
          }
          return float(sum);
        });
      };
      const Stats hooks = group({Phase::kShadowRecord, Phase::kShadowAssert, Phase::kMutexWait, Phase::kRecEnqueue}, true);
      const Stats d3d = group({Phase::kGuestD3D, Phase::kGuestGate, Phase::kGuestSwap}, false);
      const Stats spin = group({Phase::kWaitRing, Phase::kWaitAdaptive, Phase::kWaitFeSpin, Phase::kWaitFePace,
                                Phase::kWaitYield},
                               false);
      const Stats block = group({Phase::kWaitGuestFence, Phase::kWaitBlockIdle, Phase::kWaitNotBusy, Phase::kRecWait},
                                false);
      const auto ap = [](const Stats &st) { return fmt::format("{:.2f}/{:.2f}", st.avg, st.p99); };
      REXLOG_INFO("pdz_timing: game thread split ms per frame avg/p99 ({}) swaps {}-{} | frame {} | our hooks {} "
                  "({} + shadow.record + shadow.assert + wait.mutex + rt.enqueue) | guest D3D {} (guest.d3d + "
                  "guest.gate + guest.swap) | spin waits {} (wait.ring + wait.adaptive + wait.fe_spin + wait.fe_pace + "
                  "wait.yield) | blocking waits {} (wait.guest_fence + wait.block_idle + wait.not_busy + wait.rt) | rest "
                  "{} (guest code + untimed waits) | thread cpu {}",
                  kind, s.first_swap + begin, s.first_swap + end - 1, ap(frame), ap(hooks),
                  off_thread ? "native cpu on the record thread: not here" : "native cpu", ap(d3d), ap(spin),
                  ap(block), ap(rest), cpu.count ? ap(cpu) : std::string("n/a"));
      // The guest sampler (pdz_game_thread_profile): the rest by guest function, D3D library vs game code.
      pdz::gtsample::LogRange(kind, s.first_swap + begin, s.first_swap + end - 1, frame.avg, rest.avg,
                              std::string_view(kind) == "window");
    }

    // The busiest threads over the sampler's last second (drawcost; phases on, periodic windows only).
    void LogThreads()
    {
      std::string line;
      {
        std::lock_guard<std::mutex> lock(sampler::g_threads_mutex);
        line = sampler::g_threads_line;
      }
      if (!line.empty())
      {
        REXLOG_INFO("pdz_timing: threads (last 1 s, % of one core) | {}", line);
      }
    }

    // The scenes line (call with s.mutex held): the Swap interval per scene of the closed frame.
    void LogScenes(State &s, size_t begin, size_t end, const char *kind)
    {
      struct Row
      {
        pdz::pace::Scene scene;
        const char *name;
      };
      static constexpr Row kRows[] = {{pdz::pace::Scene::kFrontEnd, "front end (present step outside state 21)"},
                                      {pdz::pace::Scene::kLevel, "level (state 21)"},
                                      {pdz::pace::Scene::kOther, "other Swaps (not from the present step)"}};
      std::string out;
      for (const Row &row : kRows)
      {
        size_t over17 = 0, over20 = 0, over40 = 0, over_budget = 0, over_miss = 0;
        // Paced above 60 (pdz_null_refresh_hz / pdz_sp_120): the frame budget at that rate, and 1.5x it (a frame
        // shown for two refreshes on a fixed grid).
        const uint32_t hz = pdz::pace::EffectiveHz();
        const float budget = hz >= 90 && row.scene == pdz::pace::Scene::kLevel ? 1000.0f / float(hz) : 0.0f;
        const Stats st = ComputeBy(s, begin, end, [&](size_t i) {
          const Sample &x = s.samples[i];
          if (x.scene != row.scene)
          {
            return kNone;
          }
          over17 += x.frame_ms > 16.7f;   // the refocus pass mark (% over 16.7 ms at 60), no slack
          over20 += x.frame_ms > 20.0f;
          over40 += x.frame_ms > 40.0f;
          over_budget += budget > 0.0f && x.frame_ms > budget;
          over_miss += budget > 0.0f && x.frame_ms > budget * 1.5f;
          return x.frame_ms;
        });
        if (!st.count)
        {
          continue;
        }
        out += fmt::format(" | {}: frames {}, frame ms avg {:.2f} p50 {:.2f} p99 {:.2f} max {:.2f}, over 20 ms {} "
                           "({:.1f}%), over 40 ms {} ({:.1f}%), over 16.7 ms {} ({:.1f}%)",
                           row.name, st.count, st.avg, st.p50, st.p99, st.max, over20,
                           100.0 * double(over20) / double(st.count), over40,
                           100.0 * double(over40) / double(st.count), over17,
                           100.0 * double(over17) / double(st.count));
        if (budget > 0.0f)
        {
          out += fmt::format(", over {:.2f} ms {} ({:.1f}%), over {:.2f} ms {} ({:.1f}%)", budget, over_budget,
                             100.0 * double(over_budget) / double(st.count), budget * 1.5f, over_miss,
                             100.0 * double(over_miss) / double(st.count));
        }
      }
      REXLOG_INFO("pdz_timing: scenes ({}) swaps {}-{}{}", kind, s.first_swap + begin, s.first_swap + end - 1,
                  out.empty() ? std::string(" | none") : out);
    }

    std::string AvgP99(const char *name, const Stats &st, const char *none)
    {
      return st.count ? fmt::format("{} avg {:.3f} p99 {:.3f}", name, st.avg, st.p99) : fmt::format("{} {}", name, none);
    }

    // Call with s.mutex held.
    void LogRange(State &s, size_t begin, size_t end, const char *kind, const char *why)
    {
      const Stats f = Compute(s, begin, end, &Sample::frame_ms);
      const Stats n = Compute(s, begin, end, &Sample::native_ms);
      const Stats h = Compute(s, begin, end, &Sample::shadow_ms);
      const Stats ng = Compute(s, begin, end, &Sample::native_gpu_ms);
      const Stats pg = Compute(s, begin, end, &Sample::plugin_gpu_ms);
      const Stats cb = Compute(s, begin, end, &Sample::cp_busy_ms);
      const Stats ct = Compute(s, begin, end, &Sample::cp_thread_ms);
      const Stats pd = Compute(s, begin, end, &Sample::present_delay_ms);
      const Stats pgap = Compute(s, begin, end, &Sample::present_gap_ms);
      // The process's GPU busy share, and that share of each frame's wall time.
      double pct_sum = 0.0, busy_ms_sum = 0.0;
      size_t pct_count = 0;
      for (size_t i = begin; i < end; ++i)
      {
        const Sample &x = s.samples[i];
        if (!std::isnan(x.gpu_busy_pct))
        {
          pct_sum += x.gpu_busy_pct;
          busy_ms_sum += double(x.gpu_busy_pct) / 100.0 * double(x.frame_ms);
          ++pct_count;
        }
      }
      const bool null_backend = pdz::gpusync::NullBackend();
      const std::string native_gpu = AvgP99("native gpu ms", ng, "n/a (native renderer off or no readback yet)");
      const std::string plugin_gpu =
          null_backend ? std::string("plugin gpu ms none (null backend)")
                       : AvgP99("plugin gpu ms", pg, "n/a (needs --frame_pacing_log=true)");
      const std::string cp_busy = AvgP99("cp busy ms", cb, "n/a (needs --frame_pacing_log=true)");
      const std::string cp_thread =
          ct.count ? fmt::format("cp thread cpu ms avg {:.3f}", ct.avg) : std::string("cp thread cpu ms n/a");
      const std::string gpu_busy =
          pct_count ? fmt::format("process gpu busy avg {:.1f}% = {:.2f} ms/frame", pct_sum / double(pct_count),
                                  busy_ms_sum / double(pct_count))
                    : std::string("process gpu busy n/a");
      const char *where = g_present_where.load(std::memory_order_relaxed);
      const std::string present =
          pd.count ? fmt::format("present delay ms avg {:.3f} p99 {:.3f}, gap p50 {:.2f} p99 {:.2f} max {:.2f} ({})",
                                 pd.avg, pd.p99, pgap.p50, pgap.p99, pgap.max, where ? where : "?")
                   : std::string("present n/a (native output not shown)");
      REXLOG_INFO("pdz_timing: frames {} | frame ms avg {:.2f} p50 {:.2f} p99 {:.2f} max {:.2f} | native cpu ms avg "
                  "{:.3f} p99 {:.3f} | shadow ms avg {:.3f} | {} | {} | {} | {} | {} | {} | gpu backend {} | {} ({}) "
                  "swaps {}-{}",
                  end - begin, f.avg, f.p50, f.p99, f.max, n.avg, n.p99, h.avg, native_gpu, plugin_gpu, cp_busy,
                  cp_thread, gpu_busy, present, null_backend ? "null" : "plugin", kind, why, s.first_swap + begin,
                  s.first_swap + end - 1);
      LogScenes(s, begin, end, kind); // front end / level split
      LogPhases(s, begin, end, kind); // pdz_frame_timing_phases
      // pdz_native_record_thread: the worker's busy time, queue depth, the game thread's waits for it.
      pdz::native::recthread::LogRange(kind, s.first_swap + begin, s.first_swap + end - 1, f.avg);
      if (s.phase_level > 0 && std::string_view(kind) == "window")
      {
        LogThreads();
      }
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Spike log (pdz_frame_timing_spike_ms)
    // ---------------------------------------------------------------------------------------------------------------

    // The counters' deltas over the interval this Swap closes (call with s.mutex held, every Swap). True when the
    // previous Swap took them too (the interval ran with the log on). base / device: the D3D vblank count.
    bool TakeSpikeDeltas(State &s, int32_t spike_ms, const uint8_t *base, uint32_t device, SpikeFrame &f)
    {
      if (spike_ms <= 0)
      {
        s.spike_have = false;
        s.vblank_have = false;
        return false;
      }
      const bool valid = s.spike_have;
      for (uint32_t i = 0; i < kEvents; ++i)
      {
        const uint64_t now = g_events[i].load(std::memory_order_relaxed);
        f.events[i] = valid ? now - s.event_prev[i] : 0;
        s.event_prev[i] = now;
      }
      pdz::native::recthread::SpikeTotals rt;
      f.rt = pdz::native::recthread::TakeSpikeTotals(rt);
      if (f.rt)
      {
        const pdz::native::recthread::SpikeTotals &p = s.rt_prev;
        auto &d = f.rt_delta;
        d = rt; // the maxima as taken
        if (valid)
        {
          d.busy_ns = rt.busy_ns - p.busy_ns;
          d.commands = rt.commands - p.commands;
          for (uint32_t k = 0; k < 3; ++k)
          {
            d.wait_us[k] = rt.wait_us[k] - p.wait_us[k];
            d.waits[k] = rt.waits[k] - p.waits[k];
          }
          d.arena_waits = rt.arena_waits - p.arena_waits;
          d.arena_wraps = rt.arena_wraps - p.arena_wraps;
          d.inline_runs = rt.inline_runs - p.inline_runs;
          d.payload_bytes = rt.payload_bytes - p.payload_bytes;
        }
        s.rt_prev = rt;
      }
      // The D3D vblank count (big-endian u32 in the device the guest Swap got; the device struct is 0x5080 bytes).
      if (base && pdz::shadow::IsGuestPointer(device) && pdz::shadow::IsGuestPointer(device + kDeviceVblankCount + 4))
      {
        const uint32_t vblank = pdz::shadow::Load32(base, device + kDeviceVblankCount);
        f.vblank = valid && s.vblank_have;
        f.vblanks = f.vblank ? vblank - s.vblank_prev : 0;
        s.vblank_prev = vblank;
        s.vblank_have = true;
      }
      else
      {
        s.vblank_have = false;
      }
      s.spike_have = true;
      return valid;
    }

    double PhaseMs(const PhaseSample &ph, Phase p)
    {
      const float v = ph.ms[uint32_t(p)];
      return std::isnan(v) ? 0.0 : double(v);
    }

    double GroupMs(const PhaseSample &ph, const Group &g)
    {
      double sum = 0.0;
      for (uint32_t i = 0; i < g.count; ++i)
      {
        sum += PhaseMs(ph, g.phases[i]);
      }
      return sum;
    }

    struct Cause
    {
      std::string tag;
      double ms = -1.0;
      void Consider(const char *t, double v)
      {
        if (v > ms)
        {
          tag = t;
          ms = v;
        }
      }
    };

    // The largest group of the native entry points' phases in the frame (draw, the inside groups, the native rest).
    Cause LargestNative(const PhaseSample &ph)
    {
      Cause best;
      double draw = 0.0;
      for (uint32_t p = kDrawPhaseFirst; p <= kDrawPhaseLast; ++p)
      {
        draw += PhaseMs(ph, Phase(p));
      }
      best.Consider("draw", draw);
      for (const Group &g : kInsideGroups)
      {
        best.Consider(g.tag, GroupMs(ph, g));
      }
      best.Consider("native other", std::isnan(ph.other_ms) ? 0.0 : double(ph.other_ms));
      return best;
    }

    // The cause tag of a spike: the largest component of the game thread's frame (the frame_timing.h game thread
    // split: outside groups, native groups when the native work runs on the game thread, guest code, blocked = the
    // untimed time off the CPU). A wait for the record thread is retagged with the worker's largest group.
    Cause SpikeCause(const Sample &x, const PhaseSample &ph, const SpikeFrame &f, bool off_thread, std::string &detail)
    {
      Cause best;
      if (!ph.level)
      {
        // Phases off: only the record thread's own wait counters say anything.
        const double rt_wait =
            f.rt ? double(f.rt_delta.wait_us[0] + f.rt_delta.wait_us[1] + f.rt_delta.wait_us[2]) / 1000.0 : 0.0;
        if (rt_wait > 0.5 * double(x.frame_ms))
        {
          best.Consider("rt.worker", rt_wait);
        }
        else
        {
          best.Consider("unknown (phases off)", double(x.frame_ms));
        }
        return best;
      }
      for (const Group &g : kOutsideGroups)
      {
        best.Consider(g.tag, GroupMs(ph, g));
      }
      double outside = 0.0, waits = 0.0;
      for (uint32_t p = kInside; p < kPhases; ++p)
      {
        outside += PhaseMs(ph, Phase(p));
      }
      for (const Phase p : kWaitPhases)
      {
        waits += PhaseMs(ph, p);
      }
      const double native_game = off_thread ? 0.0 : double(x.native_ms);
      if (!off_thread)
      {
        const Cause n = LargestNative(ph);
        best.Consider(n.tag.c_str(), n.ms);
      }
      if (!std::isnan(ph.thread_cpu_ms))
      {
        const double code = double(ph.thread_cpu_ms) - (native_game + outside - waits);
        best.Consider("guest code", code);
        best.Consider("blocked", double(x.frame_ms) - native_game - outside - code);
      }
      else
      {
        best.Consider("rest", double(x.frame_ms) - native_game - outside);
      }
      if (best.tag == "wait.rt" && off_thread)
      {
        const Cause w = LargestNative(ph);
        detail = fmt::format("; the game thread waited {:.2f} ms for the record thread, whose largest group "
                             "was {} {:.2f} ms",
                             best.ms, w.tag, w.ms);
        best.tag = "rt.worker/" + w.tag;
      }
      return best;
    }

    // "name ms, ..." of the phases [first, last) charged at least 0.05 ms in the frame, largest first.
    std::string PhaseList(const PhaseSample &ph, uint32_t first, uint32_t last)
    {
      std::pair<float, uint32_t> items[kPhases];
      uint32_t n = 0;
      for (uint32_t p = first; p < last; ++p)
      {
        const float v = ph.ms[p];
        if (!std::isnan(v) && v >= 0.05f)
        {
          items[n++] = {v, p};
        }
      }
      std::sort(items, items + n, [](const auto &a, const auto &b) { return a.first > b.first; });
      std::string out;
      for (uint32_t i = 0; i < n; ++i)
      {
        out += fmt::format("{}{} {:.2f}", i ? ", " : "", kPhaseNames[items[i].second], items[i].first);
      }
      return out.empty() ? std::string("none over 0.05 ms") : out;
    }

    // " P" / " E" for a logical processor of group 0 on a CPU with more than one efficiency class (the highest class
    // = P), "" otherwise (one GetSystemCpuSetInformation at the first call).
    const char *CoreClassTag(int16_t core)
    {
      static const std::vector<int8_t> classes = [] {
        std::vector<int8_t> out(256, -1);
        ULONG len = 0;
        GetSystemCpuSetInformation(nullptr, 0, &len, GetCurrentProcess(), 0);
        std::vector<uint8_t> buf(len);
        if (!len || !GetSystemCpuSetInformation(reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(buf.data()), len, &len,
                                                GetCurrentProcess(), 0))
        {
          return std::vector<int8_t>();
        }
        int8_t lo = 127, hi = -1;
        for (ULONG off = 0; off + sizeof(DWORD) * 2 <= len;)
        {
          const auto *e = reinterpret_cast<const SYSTEM_CPU_SET_INFORMATION *>(buf.data() + off);
          if (!e->Size)
          {
            break;
          }
          if (e->Type == CpuSetInformation && e->CpuSet.Group == 0)
          {
            const int8_t c = int8_t(e->CpuSet.EfficiencyClass);
            out[e->CpuSet.LogicalProcessorIndex] = c;
            lo = std::min(lo, c);
            hi = std::max(hi, c);
          }
          off += e->Size;
        }
        return lo < hi ? out : std::vector<int8_t>();
      }();
      if (core < 0 || size_t(core) >= classes.size() || classes[size_t(core)] < 0)
      {
        return "";
      }
      int8_t hi = -1;
      for (const int8_t c : classes)
      {
        hi = std::max(hi, c);
      }
      return classes[size_t(core)] == hi ? " P" : " E";
    }

    const char *SceneName(pdz::pace::Scene scene)
    {
      switch (scene)
      {
      case pdz::pace::Scene::kFrontEnd: return "front end";
      case pdz::pace::Scene::kLevel: return "level";
      default: return "other";
      }
    }

    // One spike (call with s.mutex held): counts it and, under the line limit, logs it.
    // One spike line from copies (written on the background log thread). note: appended (the rolling cap's count).
    void SpikeLine(uint64_t swap, const Sample &x, const PhaseSample &ph, const SpikeFrame &f, const Cause &cause,
                   const std::string &detail, double tsc_per_ms, bool off_thread, uint64_t async_started,
                   uint64_t async_done, const std::string &note)
    {
      const double cyc_ms = tsc_per_ms > 0.0 ? 1.0 / tsc_per_ms : 0.0;
      const auto ev = [&](Event e) { return f.events[uint32_t(e)]; };
      const auto ev_ms = [&](Event e) { return double(f.events[uint32_t(e)]) * cyc_ms; };
      const auto opt_ms = [](float v) { return std::isnan(v) ? std::string("n/a") : fmt::format("{:.2f}", v); };

      // The game thread: CPU time vs off the CPU, the outside phases (and the native ones without the record thread).
      std::string game;
      if (ph.level)
      {
        double outside = 0.0, waits = 0.0;
        for (uint32_t p = kInside; p < kPhases; ++p)
        {
          outside += PhaseMs(ph, Phase(p));
        }
        for (const Phase p : kWaitPhases)
        {
          waits += PhaseMs(ph, p);
        }
        const double native_game = off_thread ? 0.0 : double(x.native_ms);
        const std::string cpu =
            std::isnan(ph.thread_cpu_ms)
                ? fmt::format("cpu n/a; rest {:.2f}", double(x.frame_ms) - native_game - outside)
                : fmt::format("cpu {:.2f}, blocked {:.2f} (frame - cpu); guest code ~{:.2f}, untimed waits ~{:.2f}",
                              ph.thread_cpu_ms, x.frame_ms - ph.thread_cpu_ms,
                              double(ph.thread_cpu_ms) - (native_game + outside - waits),
                              double(x.frame_ms) - double(ph.thread_cpu_ms) - waits);
        const auto core_name = [](int16_t c) {
          return c < 0 ? std::string("?") : fmt::format("{}{}", c, CoreClassTag(c));
        };
        game = fmt::format("game thread: {}; core {} -> {}, page faults {}; outside native {:.2f}: {}", cpu,
                           core_name(ph.core_from), core_name(ph.core_to),
                           ph.page_faults < 0 ? std::string("n/a") : fmt::format("{}", ph.page_faults), outside,
                           PhaseList(ph, kInside, kPhases));
      }
      else
      {
        game = "game thread: phases off (pdz_frame_timing_phases 0)";
      }
      const std::string native =
          ph.level ? fmt::format("native cpu {:.2f} ({}): {}, other {}", x.native_ms,
                                 off_thread ? "record thread" : "game thread", PhaseList(ph, 0, kInside),
                                 opt_ms(ph.other_ms))
                   : fmt::format("native cpu {:.2f} ({})", x.native_ms, off_thread ? "record thread" : "game thread");
      std::string rt = "record thread off";
      if (f.rt)
      {
        const auto &d = f.rt_delta;
        rt = fmt::format("record thread: busy {:.2f} ms, commands {}, longest {} {:.2f} ms, queue depth max {}, game "
                         "thread waits room {:.2f} ms ({}, arena {}) / swap bound {:.2f} ({}) / drain {:.2f} ({}), "
                         "inline {}, arena wraps {}, payload {:.2f} MB",
                         double(d.busy_ns) / 1.0e6, d.commands, d.longest_type, double(d.longest_cycles) * cyc_ms,
                         d.depth_max, double(d.wait_us[0]) / 1000.0, d.waits[0], d.arena_waits,
                         double(d.wait_us[1]) / 1000.0, d.waits[1], double(d.wait_us[2]) / 1000.0, d.waits[2],
                         d.inline_runs, d.arena_wraps, double(d.payload_bytes) / 1048576.0);
      }
      // pdz_native_async_readback: async snapshot copies in this interval and those still pending now (cumulative
      // started - written, both counted while the spike log is on).
      const std::string readback_async = fmt::format(
          "readback async {} (pending {}, written {}, sync fallbacks {}, DeResThread wait {:.2f} ms)",
          ev(Event::kSnapshotAsync), async_started > async_done ? async_started - async_done : 0,
          ev(Event::kSnapshotAsyncDone), ev(Event::kSnapshotAsyncFallback), ev_ms(Event::kSnapshotAsyncWaitCycles));
      const std::string events = fmt::format(
          "events: InsertCallback {}, snapshot readbacks {} sync ({:.2f} ms, GPU wait {:.2f}), {}, flush waits {} "
          "(wait.flush {:.2f}), fence waits {} (wait.fence {:.2f}), pso {} ({:.2f}), textures created {} (upload "
          "{:.2f} MB, tex.upload {:.2f}), hashed {} ({:.2f} MB, tex.hash {:.2f}), resources created {} ({:.2f}), "
          "upload grows {}, descriptor ring full {}, resolves {} | present delay {} gap {} (present.sdk_post {:.2f}), "
          "guest vblanks {}, native gpu {} (read back ~3 frames late)",
          ev(Event::kInsertCallback), ev(Event::kSnapshot), ev_ms(Event::kSnapshotCycles),
          ev_ms(Event::kSnapshotWaitCycles), readback_async, ev(Event::kFlushWait), PhaseMs(ph, Phase::kFlushWait),
          ev(Event::kFenceWait), PhaseMs(ph, Phase::kFenceWait), ev(Event::kPsoCreate), PhaseMs(ph, Phase::kPsoCreate),
          ev(Event::kTexCreate), double(ev(Event::kTexUploadBytes)) / 1048576.0, PhaseMs(ph, Phase::kTextureUpload),
          ev(Event::kTexHash), double(ev(Event::kTexHashBytes)) / 1048576.0, PhaseMs(ph, Phase::kTextureHash),
          ev(Event::kResourceCreate), ev_ms(Event::kResourceCycles), ev(Event::kUploadGrow),
          ev(Event::kDescriptorFull), ev(Event::kResolve), opt_ms(x.present_delay_ms), opt_ms(x.present_gap_ms),
          PhaseMs(ph, Phase::kPresentSdkPost), f.vblank ? fmt::format("{}", f.vblanks) : std::string("n/a"),
          opt_ms(x.native_gpu_ms));
      REXLOG_INFO("pdz_timing: spike swap {} scene {} frame {:.2f} ms | {} | {} | {} | {} | cause {} ({:.2f} ms){}{}",
                  swap, SceneName(x.scene), x.frame_ms, game, native, rt, events, cause.tag, cause.ms, detail, note);
    }

    // The rolling spike cap (after kSpikeLines): the slowest spike of the current 5 s (s.mutex).
    struct RollSpike
    {
      uint64_t swap;
      Sample x;
      PhaseSample ph;
      SpikeFrame f;
      Cause cause;
      std::string detail;
      double tsc_per_ms;
      bool off_thread;
      uint64_t async_started, async_done;
    };
    std::shared_ptr<RollSpike> g_roll;
    int64_t g_roll_start = 0; // QPC of the period's first spike
    uint64_t g_roll_count = 0;

    // Every Swap (s.mutex held): the period's slowest spike goes out once its 5 s are over.
    void RollSpikes(const State &s)
    {
      if (!g_roll || double(s.last - g_roll_start) * s.ms_per_tick < 5000.0)
      {
        return;
      }
      const std::shared_ptr<RollSpike> r = std::move(g_roll);
      const uint64_t count = std::exchange(g_roll_count, 0);
      Defer([r, count]() {
        SpikeLine(r->swap, r->x, r->ph, r->f, r->cause, r->detail, r->tsc_per_ms, r->off_thread, r->async_started,
                  r->async_done, fmt::format(" | the slowest of {} spike(s) in 5 s", count));
      });
    }

    void OnSpike(State &s, uint64_t swap, const Sample &x, const PhaseSample &ph, const SpikeFrame &f, int32_t spike_ms)
    {
      ++s.spikes;
      s.spike_ms = spike_ms;
      s.spike_scene[std::min<uint32_t>(uint32_t(x.scene), 2)] += 1;
      if (double(x.frame_ms) > s.spike_max_ms)
      {
        s.spike_max_ms = x.frame_ms;
        s.spike_max_swap = swap;
      }
      const bool off_thread = g_native_off_thread.load(std::memory_order_relaxed);
      std::string detail;
      const Cause cause = SpikeCause(x, ph, f, off_thread, detail);
      auto it = std::find_if(s.spike_causes.begin(), s.spike_causes.end(),
                             [&](const auto &c) { return c.first == cause.tag; });
      if (it == s.spike_causes.end())
      {
        s.spike_causes.emplace_back(cause.tag, 1);
      }
      else
      {
        ++it->second;
      }
      if (s.spike_lines >= kSpikeLines)
      {
        // Rolling (log 299: the lines stopped at 600 before Rooftops' periodic hitch): from here the slowest spike of
        // every 5 s is kept and logged when the 5 s are over (RollSpikes, every Swap).
        if (s.spike_lines++ == kSpikeLines)
        {
          Defer([]() {
            REXLOG_INFO("pdz_timing: spike lines: {} logged; from here the slowest spike of every 5 s (the spikes "
                        "SUMMARY counts them all)",
                        kSpikeLines);
          });
        }
        if (!g_roll || x.frame_ms > g_roll->x.frame_ms)
        {
          if (!g_roll)
          {
            g_roll_start = s.last;
          }
          g_roll = std::make_shared<RollSpike>(RollSpike{
              swap, x, ph, f, cause, detail, s.tsc_per_ms, off_thread,
              g_events[uint32_t(Event::kSnapshotAsync)].load(std::memory_order_relaxed),
              g_events[uint32_t(Event::kSnapshotAsyncDone)].load(std::memory_order_relaxed)});
        }
        ++g_roll_count;
        return;
      }
      ++s.spike_lines;
      const double tsc_per_ms = s.tsc_per_ms;
      const uint64_t async_started = g_events[uint32_t(Event::kSnapshotAsync)].load(std::memory_order_relaxed);
      const uint64_t async_done = g_events[uint32_t(Event::kSnapshotAsyncDone)].load(std::memory_order_relaxed);
      // Formatted and written on the background log thread (log 299), from copies of this frame's data.
      Defer([=]() {
        SpikeLine(swap, x, ph, f, cause, detail, tsc_per_ms, off_thread, async_started, async_done, std::string());
      });
    }

    // The spikes SUMMARY line (call with s.mutex held).
    void LogSpikeSummary(State &s, const char *why)
    {
      const int32_t spike_ms = REXCVAR_GET(pdz_frame_timing_spike_ms);
      if (spike_ms <= 0 && !s.spikes)
      {
        return;
      }
      std::vector<std::pair<std::string, uint64_t>> causes = s.spike_causes;
      std::sort(causes.begin(), causes.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
      std::string hist;
      for (const auto &c : causes)
      {
        hist += fmt::format("{}{} {}", hist.empty() ? "" : ", ", c.first, c.second);
      }
      REXLOG_INFO("pdz_timing: spikes SUMMARY ({}) over {} ms: {} of {} kept frames ({:.2f}%), max {:.1f} ms (swap {}) "
                  "| by scene: level {}, front end {}, other {} | causes: {} | lines logged {}, not logged {} "
                  "(limit {})",
                  why, s.spike_ms ? s.spike_ms : spike_ms, s.spikes, s.spike_frames,
                  s.spike_frames ? 100.0 * double(s.spikes) / double(s.spike_frames) : 0.0, s.spike_max_ms,
                  s.spike_max_swap, s.spike_scene[uint32_t(pdz::pace::Scene::kLevel)],
                  s.spike_scene[uint32_t(pdz::pace::Scene::kFrontEnd)],
                  s.spike_scene[uint32_t(pdz::pace::Scene::kOther)], hist.empty() ? std::string("none") : hist,
                  std::min(s.spike_lines, kSpikeLines),
                  s.spike_lines > kSpikeLines ? s.spike_lines - kSpikeLines : 0, kSpikeLines);
    }
  } // namespace

  int64_t Now()
  {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return int64_t(t.QuadPart);
  }

  void AddCycles(Bucket b, uint64_t cycles)
  {
    g_acc[uint32_t(b)].fetch_add(int64_t(cycles), std::memory_order_relaxed);
  }

  void SetNativeOffThread(bool off_thread) { g_native_off_thread.store(off_thread, std::memory_order_relaxed); }

  void AddNativeGpuUs(uint64_t us)
  {
    g_native_gpu_us.fetch_add(us, std::memory_order_relaxed);
    g_native_gpu_frames.fetch_add(1, std::memory_order_relaxed);
  }

  void AddPresent(uint64_t delay_us, uint64_t gap_us, const char *where)
  {
    if (!Enabled())
    {
      return;
    }
    g_present_where.store(where, std::memory_order_relaxed);
    g_present_delay_us.fetch_add(delay_us, std::memory_order_relaxed);
    g_presents.fetch_add(1, std::memory_order_relaxed);
    uint64_t prev = g_present_gap_max_us.load(std::memory_order_relaxed);
    while (gap_us > prev && !g_present_gap_max_us.compare_exchange_weak(prev, gap_us, std::memory_order_relaxed))
    {
    }
  }

  void OnSwap(const uint8_t *base, uint32_t device)
  {
    State &s = g_state;
    if (!Enabled())
    {
      s.last = 0; // a later switch-on starts with a fresh interval
      g_phase_level.store(0, std::memory_order_relaxed);
      g_spike_ms.store(0, std::memory_order_relaxed);
      return;
    }
    const int32_t phase_level = std::clamp(REXCVAR_GET(pdz_frame_timing_phases), 0, 2);
    g_phase_level.store(phase_level, std::memory_order_relaxed); // the scopes of the next frame
    const int32_t spike_ms = std::max(0, REXCVAR_GET(pdz_frame_timing_spike_ms));
    g_spike_ms.store(spike_ms, std::memory_order_relaxed); // the event counters of the next frame
    t_swap_thread = true; // cpusplit: the ExclusiveScope waits charge on this thread only
    const uint64_t now_tsc = __rdtsc();
    uint64_t phase_now[kPhases];
    for (uint32_t i = 0; i < kPhases; ++i)
    {
      phase_now[i] = g_phase_cycles[i].load(std::memory_order_relaxed);
    }
    const int64_t now = Now();
    int64_t acc[uint32_t(Bucket::kCount)];
    for (uint32_t i = 0; i < uint32_t(Bucket::kCount); ++i)
    {
      acc[i] = g_acc[i].exchange(0, std::memory_order_relaxed);
    }
    const uint64_t native_gpu_us = g_native_gpu_us.exchange(0, std::memory_order_relaxed);
    const uint32_t native_gpu_frames = g_native_gpu_frames.exchange(0, std::memory_order_relaxed);
    const uint64_t present_delay_us = g_present_delay_us.exchange(0, std::memory_order_relaxed);
    const uint32_t presents = g_presents.exchange(0, std::memory_order_relaxed);
    const uint64_t present_gap_us = g_present_gap_max_us.exchange(0, std::memory_order_relaxed);
    const pdz::pace::Scene scene = pdz::pace::TakeScene(); // set by the present step just before this Swap
    sampler::g_swaps.fetch_add(1, std::memory_order_relaxed);
    const bool pacing = rex::ui::frame_pacing::Enabled();
    rex::ui::frame_pacing::Totals totals{};
    if (pacing)
    {
      rex::ui::frame_pacing::GetTotals(totals);
    }
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.ms_per_tick == 0.0)
    {
      LARGE_INTEGER freq;
      QueryPerformanceFrequency(&freq);
      s.ms_per_tick = 1000.0 / double(freq.QuadPart);
      s.samples.reserve(kReserve);
      s.scratch.reserve(kReserve);
      REXLOG_INFO("pdz_timing: on (QPC {} Hz); the first {} Swap intervals are left out of the statistics; gpu backend "
                  "{}; frame_pacing_log {} (plugin gpu ms / cp busy ms need it)",
                  int64_t(freq.QuadPart), REXCVAR_GET(pdz_frame_timing_skip),
                  pdz::gpusync::NullBackend() ? "null" : "plugin", pacing ? "on" : "off");
      sampler::Start();
    }
    // Load time (m5_compiler.md): wall time from process start to the first Swap of a level frame, once per run.
    if (scene == pdz::pace::Scene::kLevel && !s.level_seen)
    {
      s.level_seen = true;
      FILETIME created{}, exited{}, kernel{}, user{}, wall{};
      GetSystemTimePreciseAsFileTime(&wall);
      if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
      {
        const auto ticks = [](const FILETIME &f) { return (uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
        REXLOG_INFO("pdz_timing: first level frame at Swap {}, {:.2f} s after process start", s.swaps + 1,
                    double(ticks(wall) - ticks(created)) / 1.0e7);
      }
    }
    // Phases: rdtsc calibrated against QPC since the first Swap (stable after a fraction of a second; the first
    // pdz_frame_timing_skip intervals are not kept anyway), and the cycles charged since the previous Swap.
    if (s.calib_qpc == 0)
    {
      s.calib_qpc = now;
      s.calib_tsc = now_tsc;
    }
    else if (const double ms = double(now - s.calib_qpc) * s.ms_per_tick; ms >= 200.0 && now_tsc > s.calib_tsc)
    {
      s.tsc_per_ms = double(now_tsc - s.calib_tsc) / ms;
    }
    // The buckets' rdtsc cycles in ms (0 before the calibration: those first intervals are not kept).
    double acc_ms[uint32_t(Bucket::kCount)];
    for (uint32_t i = 0; i < uint32_t(Bucket::kCount); ++i)
    {
      acc_ms[i] = s.tsc_per_ms > 0.0 ? double(acc[i]) / s.tsc_per_ms : 0.0;
    }
    PhaseSample phase_sample;
    phase_sample.level = s.tsc_per_ms > 0.0 ? s.phase_level : 0; // the level the interval's scopes ran at
    phase_sample.other_ms = kNone;
    phase_sample.thread_cpu_ms = kNone;
    phase_sample.draws = 0.0f;
    phase_sample.core_from = phase_sample.core_to = -1;
    phase_sample.page_faults = -1;
    // The Swap thread's CPU time (one QueryThreadCycleTime per Swap, phases on) and the draw calls since the previous
    // Swap (drawcost). A Swap from another thread than the previous one restarts the thread clock.
    {
      const uint64_t draws_now = g_draw_calls.load(std::memory_order_relaxed);
      phase_sample.draws = float(draws_now - s.draw_calls);
      s.draw_calls = draws_now;
      const DWORD tid = GetCurrentThreadId();
      sampler::g_swap_tid.store(tid, std::memory_order_relaxed);
      ULONG64 cycles = 0;
      if (phase_level > 0 && QueryThreadCycleTime(GetCurrentThread(), &cycles))
      {
        if (s.phase_level > 0 && s.thread_id == tid && s.thread_cycles && cycles >= s.thread_cycles &&
            s.tsc_per_ms > 0.0)
        {
          phase_sample.thread_cpu_ms = float(double(cycles - s.thread_cycles) / s.tsc_per_ms);
        }
        s.thread_id = tid;
        s.thread_cycles = cycles;
        // Where the game thread ran (P / E core) and the page faults of the interval: one processor read and one
        // GetProcessMemoryInfo per Swap, phases on only.
        const int16_t core = int16_t(GetCurrentProcessorNumber());
        phase_sample.core_from = s.core_prev;
        phase_sample.core_to = core;
        s.core_prev = core;
        PROCESS_MEMORY_COUNTERS pmc{};
        if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
        {
          const int64_t faults = int64_t(pmc.PageFaultCount);
          if (s.page_faults_prev >= 0 && faults >= s.page_faults_prev)
          {
            phase_sample.page_faults = int32_t(std::min<int64_t>(faults - s.page_faults_prev, INT32_MAX));
          }
          s.page_faults_prev = faults;
        }
      }
      else
      {
        s.thread_cycles = 0;
        s.core_prev = -1;
        s.page_faults_prev = -1;
      }
    }
    double inside_ms = 0.0;
    for (uint32_t i = 0; i < kPhases; ++i)
    {
      const uint64_t delta = phase_now[i] - s.phase_prev[i];
      s.phase_prev[i] = phase_now[i];
      phase_sample.ms[i] = phase_sample.level ? float(double(delta) / s.tsc_per_ms) : kNone;
      if (phase_sample.level && i < kInside)
      {
        inside_ms += double(delta) / s.tsc_per_ms;
      }
    }
    s.phase_level = phase_level;
    // Spike log: the event / record thread / vblank deltas of this interval (taken every Swap while the log is on).
    SpikeFrame spike_frame;
    const bool spike_valid = TakeSpikeDeltas(s, spike_ms, base, device, spike_frame);
    // The SDK's per-frame totals since the previous Swap ('T': the plugin's GPU busy time, 'K': the CP's busy time).
    // Rows come at the CP's XE_SWAP ('K') or when GPU timestamps are read back ('T', frames later), so one Swap interval
    // holds 0, 1 or 2 rows: a sample is the mean per row (per CP / GPU frame), none when the interval has no row.
    float plugin_gpu_ms = kNone, cp_busy_ms = kNone;
    if (pacing && s.have_pacing)
    {
      const rex::ui::frame_pacing::Totals &p = s.pacing;
      if (const uint64_t rows = totals.gpu_frames - p.gpu_frames)
      {
        plugin_gpu_ms = float(double(totals.gpu_busy_us - p.gpu_busy_us) / 1000.0 / double(rows));
      }
      if (const uint64_t rows = totals.cp_frames - p.cp_frames)
      {
        const int64_t busy_us = int64_t(totals.cp_wall_us - p.cp_wall_us) - int64_t(totals.cp_idle_us - p.cp_idle_us) -
                                int64_t(totals.cp_wait_reg_us - p.cp_wait_reg_us) -
                                int64_t(totals.cp_gpu_await_us - p.cp_gpu_await_us);
        cp_busy_ms = float(double(std::max<int64_t>(busy_us, 0)) / 1000.0 / double(rows));
      }
    }
    s.pacing = totals;
    s.have_pacing = pacing;
    ++s.swaps;
    const int64_t last = s.last;
    s.last = now;
    if (last == 0)
    {
      return; // the first Swap only starts the clock (the time before it is not a frame)
    }
    const uint64_t interval = s.swaps - 1;
    if (interval <= uint64_t(std::max(0, REXCVAR_GET(pdz_frame_timing_skip))))
    {
      return;
    }
    if (s.samples.empty())
    {
      s.first_swap = s.swaps;
      pdz::gtsample::StartSession(); // the guest sampler's SUMMARY counts from the first kept frame (cpusplit)
    }
    const int64_t cp_ns = sampler::g_cp_ns_per_swap.load(std::memory_order_relaxed);
    const int64_t gpu_pct_milli = sampler::g_gpu_pct_milli.load(std::memory_order_relaxed);
    if (phase_sample.level)
    {
      phase_sample.other_ms = float(acc_ms[0] - inside_ms);
    }
    if (s.phases.empty())
    {
      s.phases.reserve(kReserve);
    }
    s.phases.push_back(phase_sample);
    s.samples.push_back({float(double(now - last) * s.ms_per_tick), float(acc_ms[0]),
                         float(acc_ms[1]),
                         // the mean per native frame read back in this interval (usually one; none before the first)
                         native_gpu_frames ? float(double(native_gpu_us) / 1000.0 / double(native_gpu_frames)) : kNone,
                         plugin_gpu_ms, cp_busy_ms,
                         cp_ns >= 0 ? float(double(cp_ns) / 1e6) : kNone,
                         gpu_pct_milli >= 0 ? float(double(gpu_pct_milli) / 1000.0) : kNone,
                         presents ? float(double(present_delay_us) / 1000.0 / double(presents)) : kNone,
                         presents && present_gap_us ? float(double(present_gap_us) / 1000.0) : kNone});
    s.samples.back().scene = scene;
    if (spike_valid)
    {
      ++s.spike_frames;
      if (s.samples.back().frame_ms > float(spike_ms) &&
          (!REXCVAR_GET(pdz_frame_timing_spike_level_only) || scene == pdz::pace::Scene::kLevel))
      {
        OnSpike(s, s.swaps, s.samples.back(), s.phases.back(), spike_frame, spike_ms);
      }
    }
    RollSpikes(s);
    if (s.samples.size() % kWindow == 0)
    {
      // The window's percentiles and lines on the background log thread (log 299), from a copy of its 600 frames.
      const size_t begin = s.samples.size() - kWindow, end = s.samples.size();
      auto snap = std::make_shared<State>();
      snap->samples.assign(s.samples.begin() + ptrdiff_t(begin), s.samples.end());
      if (s.phases.size() >= end)
      {
        snap->phases.assign(s.phases.begin() + ptrdiff_t(begin), s.phases.begin() + ptrdiff_t(end));
      }
      snap->first_swap = s.first_swap + begin;
      snap->ms_per_tick = s.ms_per_tick;
      snap->tsc_per_ms = s.tsc_per_ms;
      snap->phase_level = s.phase_level;
      Defer([snap]() { LogRange(*snap, 0, snap->samples.size(), "window", "periodic"); });
    }
  }

  void LogSummary(const char *why)
  {
    if (!Enabled())
    {
      return;
    }
    State &s = g_state;
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.samples.empty())
    {
      REXLOG_INFO("pdz_timing: SUMMARY ({}): no frames kept ({} Swap calls seen, the first {} intervals skipped)",
                  why, s.swaps, REXCVAR_GET(pdz_frame_timing_skip));
      return;
    }
    LogRange(s, 0, s.samples.size(), "SUMMARY", why);
    LogSpikeSummary(s, why); // pdz_frame_timing_spike_ms
  }
} // namespace pdz::timing

namespace pdz::timing
{
  namespace
  {
    // Heap-allocated and never freed: a detached thread may still wait on it while statics are destroyed at exit.
    struct DeferQueue
    {
      std::mutex mutex;
      std::condition_variable wake;
      std::deque<std::function<void()>> items;
      bool started = false;
      uint64_t dropped = 0;
    };
    DeferQueue &Queue()
    {
      static DeferQueue *q = new DeferQueue;
      return *q;
    }

    void DeferMain()
    {
      pdz::cpu::PlaceHelperThread("log writer"); // pdz_cpu_placement
      SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
      DeferQueue &q = Queue();
      for (;;)
      {
        std::function<void()> fn;
        uint64_t dropped = 0;
        {
          std::unique_lock<std::mutex> lock(q.mutex);
          q.wake.wait(lock, [&] { return !q.items.empty(); });
          fn = std::move(q.items.front());
          q.items.pop_front();
          dropped = std::exchange(q.dropped, 0);
        }
        if (dropped)
        {
          REXLOG_WARN("pdz_timing: {} deferred log line(s) dropped (the background log thread fell behind)", dropped);
        }
        fn();
      }
    }
  } // namespace

  void Defer(std::function<void()> fn)
  {
    DeferQueue &q = Queue();
    {
      std::lock_guard<std::mutex> lock(q.mutex);
      if (!q.started)
      {
        q.started = true;
        std::thread(DeferMain).detach();
      }
      if (q.items.size() >= 1024)
      {
        ++q.dropped;
        return;
      }
      q.items.push_back(std::move(fn));
    }
    q.wake.notify_one();
  }

  void DeferLine(std::string line)
  {
    Defer([line = std::move(line)]() { REXLOG_INFO("{}", line); });
  }
} // namespace pdz::timing
