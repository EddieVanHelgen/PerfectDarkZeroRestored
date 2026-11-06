// Frame timing (pdz_frame_timing, default off; frame_timing.cpp): the wall-clock time between consecutive guest
// Swap calls, and per frame the CPU time spent inside the native renderer's entry points and inside the shadow
// recorder (Record), plus the GPU side in both GPU backends (plugin and null, stage 2 M1): the native renderer's GPU
// time from timestamps around its own command lists (native_device.cpp), the GPU plugin's host GPU time and command
// processor busy time from the SDK's frame_pacing 'T' / 'K' rows (only while frame_pacing_log is on; the null backend
// has no 'T' rows), the GPU command processor thread's CPU time and the process's 3D-engine busy share from the
// Windows GPU counters (PDH), both sampled once a second on a low-priority thread. One line every 600 frames and a
// SUMMARY line at test exit (pdz::test::LogSummary):
//   pdz_timing: frames N | frame ms avg A p50 B p99 C max D | native cpu ms avg E p99 F | shadow ms avg G | native gpu
//   ms avg H p99 I | plugin gpu ms avg J p99 K | cp busy ms avg L | cp thread cpu ms avg M | process gpu busy avg P% =
//   Q ms/frame | present delay ms avg R p99 S, gap p50 T p99 U max V (<where>) | gpu backend B | <window / SUMMARY>
//   (<why>) swaps X-Y
// The present columns (stage 2 M3, native output only): per native frame handed to the presenter, the time from its
// native OnSwap (the guest Swap has returned) to the presenter refresh ("delay": about 0 when presenting from the Swap
// hook, the wait for the command processor's XE_SWAP with pdz_native_present_timing 1) and the time since the
// previous refresh ("gap": 16.7 ms at an even 60 Hz; the max of the gaps in a Swap interval).
// Each pdz_timing line is followed by a scenes line (stage 2 M5, front-end pacing):
//   pdz_timing: scenes (<window / SUMMARY>) swaps X-Y | front end (present step outside state 21): frames N, frame ms
//   avg A p50 B p99 C max D, over 20 ms E (e%), over 40 ms F (f%), over 16.7 ms G (g%) | level (state 21): ... | other
//   Swaps (not from the present step): ...
//
// Cost while off: one cvar load per scope. While on: two QueryPerformanceCounter reads and one relaxed atomic add
// per outermost scope; nothing allocates per draw; the Swap reads a few atomics and the SDK's frame_pacing totals.
//
// Phases (pdz_frame_timing_phases 1 / 2, with pdz_frame_timing on; stage 2 M5): the native CPU time split into
// exclusive phases (PhaseScope / Lap below; a nested scope's time belongs to it, not to the enclosing one), plus two
// game-thread waits outside the native entry points (the render mutex, the guest's own Swap call). A second line
// follows each pdz_timing line:
//   pdz_timing: phases ms per frame avg/p99 (<window / SUMMARY>, level L) swaps X-Y | native cpu A = <phase a/p> ...
//   + other U | outside native: wait.mutex M, guest.swap G | tsc F MHz
// Level 1: one rdtsc pair per draw and per clear / resolve / present part, plus the rare scopes (texture hash /
// upload, PSO creation, shader translation wait, fence waits). Level 2 adds the draw stages (the pdz_native_draw_timing
// lap points: about ten rdtsc reads per draw) and the geometry hash. rdtsc, not QPC (cheaper, no call); calibrated
// against QPC over the session. Charges are plain relaxed load + store on cumulative counters: every phase scope runs
// with the native render mutex held (one writer at a time); the guest Swap (outside the mutex) uses a fetch_add.
// Stage 2 M5 drawcost: level 2 splits the draw further (d.omset, d.decode, d.cbfloat, d.texlookup, d.samplers); level
// 1 adds outside phases (shadow.record, shadow.assert, guest.d3d: the guest's own D3D draw / clear / resolve calls), the
// draw calls per frame with the native draw cost per draw, and a third line per window:
//   pdz_timing: game thread ms per frame avg/p99 (<window / SUMMARY>) swaps X-Y | frame F = native N + outside O +
//   rest R | thread cpu C, blocked B (frame - cpu) | rest = guest code (R - B) + waits | draws D/frame, native
//   draw ns/draw E
// (the swap thread's CPU time from QueryThreadCycleTime once per Swap), and every 600 frames the busiest threads of
// the process over the last second (sampler thread): 'pdz_timing: threads (last 1 s) | <name> <tid> P% ...'.
// Stage 2 M5 cpusplit (level 1, Swap thread only, exclusive of nested scopes): guest.gate (sub_826DF7F0), guest.d3d
// also for the census-hooked D3D calls (ClearF, SetShaderGPRAllocation, HiZ flush, texture lock, the other draw
// entries), and the game thread's waits: wait.ring (command-ring space), wait.guest_fence (sub_826DE258),
// wait.block_idle, wait.not_busy, wait.adaptive / wait.fe_spin (the present step's NtYieldExecution loops),
// wait.fe_pace (pdz_fix_frontend_pacing) and wait.yield (other NtYieldExecution calls). guest.swap and guest.d3d
// leave out the scopes nested in them (a ring wait inside a draw is wait.ring, not guest.d3d). A fourth line per
// window, the table of the game thread:
//   pdz_timing: game thread split ms per frame avg/p99 (<window / SUMMARY>) swaps X-Y | frame F | our hooks H |
//   guest D3D D | spin waits S | blocking waits B | rest R | thread cpu C
// and, with pdz_game_thread_profile, the guest sampler line that splits R by guest function (game_thread_sampler.h).
//
// Spike log (pdz_frame_timing_spike_ms N > 0, with pdz_frame_timing; stage 2 M5 spikes, m5_spikes.md): every kept
// Swap interval longer than N ms gets one line with what that frame did (at most 200 lines per run):
//   pdz_timing: spike swap S scene X frame F ms | game thread: cpu C, blocked B (frame - cpu); <outside phases> |
//   native cpu (<thread>) N: <inside phases> | record thread: ... | events: ... | cause <tag> (<ms>)
// and a 'pdz_timing: spikes SUMMARY' line after the SUMMARY lines: spikes by scene and a histogram of the cause tags
// (the largest component of each spike). Cost while on: the event counters below (rare sites) and a few atomic loads
// per Swap; the line is formatted only for a spike.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

#include <intrin.h>

#include <rex/cvar.h>

REXCVAR_DECLARE(bool, pdz_frame_timing);
REXCVAR_DECLARE(int32_t, pdz_frame_timing_phases);

namespace pdz::timing
{
  enum class Bucket : uint32_t
  {
    kNative = 0, // pdz::native entry points (OnDraw, OnEndVertices, FlushPendingBegin, OnClear, OnResolve,
                 // OnSwap, the InsertCallback / SetGammaRamp hook bodies)
    kShadow = 1, // pdz::shadow::Record
    kCount = 2,
  };

  inline bool Enabled() { return REXCVAR_GET(pdz_frame_timing); }

  int64_t Now();                      // QueryPerformanceCounter ticks
  void AddCycles(Bucket b, uint64_t cycles); // adds rdtsc cycles to the current frame's accumulator (relaxed atomic)

  // Nesting depth per bucket and thread: only the outermost scope of a bucket counts (OnResolve calls
  // FlushPendingBegin, OnDraw runs inside the draw hook's lock, ...).
  inline thread_local uint32_t t_depth[uint32_t(Bucket::kCount)] = {};
  // Interception speed-up (31 Oct, docs/INTERCEPTION_SPEEDUP.md): a Scope measures with rdtsc (not QPC, about 20 ns
  // a call) and adds into this thread's own counters; every kScopeBatch scopes they go to the shared accumulator (one
  // atomic add per bucket instead of one per scope, so the render and record threads no longer bounce its cache line
  // every draw). The Swap converts the frame's cycles to ms. A batch can straddle a Swap: at most kScopeBatch - 1
  // scopes (a few microseconds) land in the next frame's split.
  constexpr uint32_t kScopeBatch = 32;
  inline thread_local uint64_t t_bucket_cycles[uint32_t(Bucket::kCount)] = {};
  inline thread_local uint32_t t_bucket_pending = 0;
  inline void FlushScopes()
  {
    for (uint32_t i = 0; i < uint32_t(Bucket::kCount); ++i)
    {
      if (t_bucket_cycles[i])
      {
        AddCycles(Bucket(i), t_bucket_cycles[i]);
        t_bucket_cycles[i] = 0;
      }
    }
    t_bucket_pending = 0;
  }

  class Scope
  {
  public:
    explicit Scope(Bucket b) : bucket_(uint32_t(b))
    {
      if (!Enabled())
      {
        return;
      }
      active_ = true;
      if (t_depth[bucket_]++ == 0)
      {
        outer_ = true;
        start_ = __rdtsc();
      }
    }
    ~Scope()
    {
      if (!active_)
      {
        return;
      }
      --t_depth[bucket_];
      if (outer_)
      {
        t_bucket_cycles[bucket_] += __rdtsc() - start_;
        if (++t_bucket_pending >= kScopeBatch)
        {
          FlushScopes();
        }
      }
    }
    Scope(const Scope &) = delete;
    Scope &operator=(const Scope &) = delete;

  private:
    uint32_t bucket_;
    bool active_ = false;
    bool outer_ = false;
    uint64_t start_ = 0;
  };

  // -----------------------------------------------------------------------------------------------------------------
  // Phases (pdz_frame_timing_phases). Order = print order. "Inside" phases run within the native entry points (their
  // sum plus "other" is the native cpu column); the ones after kInsideCount are game-thread time outside them (the
  // mutex wait, the guest Swap, the shadow recorder / assert, the guest's D3D calls).
  // -----------------------------------------------------------------------------------------------------------------
  enum class Phase : uint32_t
  {
    kDraw = 0,       // draw entry points (OnDraw, OnEndVertices, the pending BeginVertices batch): level 1 the whole
                     // draw, level 2 what the stages below leave (ArgsFor, the hook prologue, ...)
    kDrawShaders,    // level 2: IssueDraw entry + the shader lookups
    kDrawTargets,    // level 2: BindTargets without its OMSetRenderTargets call (surface lookups, transitions)
    kDrawOmSet,      // level 2: BindTargets' OMSetRenderTargets call (stage 2 M5 drawcost)
    kDrawDecode,     // level 2: DecodeRenderState + ResolveIndexReset (stage 2 M5 drawcost)
    kDrawPipeline,   // level 2: PipelineCache::Lookup (key build, hash, PSO map) + DrawTopology
    kDrawViewport,   // level 2: host viewport + scissor
    kDrawGeometry,   // level 2: index / vertex uploads (without the hash below)
    kGeometryHash,   // level 2: the geometry content hash (SampleHash)
    kDrawCbFloat,    // level 2: the float constant buffers (reuse check, 4 KB byte-swapped uploads; drawcost)
    kDrawConstants,  // level 2: bool / loop and system constants
    kDrawTables,     // level 2: the SRV descriptor table (key, frame map, copies) without the texture lookups below
    kDrawTexLookup,  // level 2: Textures::Lookup / LookupArray / LookupVolume inside the SRV table (without hash /
                     // upload below; drawcost)
    kDrawSamplers,   // level 2: the two sampler tables (decode, key hash, maps) + the 7e3 ROV table (drawcost)
    kDrawRecord,     // level 2: the D3D12 calls up to the draw
    kDrawTail,       // level 2: stats, diagnostics after the draw
    kDrawSkipped,    // level 2: the rest of a draw that returned early
    kTextureHash,    // texture content / full-extent hashes on the lookup's slow path
    kTextureUpload,  // texture creation + untiling into the upload buffer + copies
    kPsoCreate,      // CreateGraphicsPipelineState on a pipeline cache miss
    kShaderWait,     // the synchronous shader translation wait (pdz_native_translate_sync)
    kClear,          // OnClear
    kResolve,        // OnResolve
    kCallback,       // OnInsertCallback (the de-rez snapshot readback; its GPU wait is wait.flush)
    kFrameBegin,     // NativeDevice::BeginFrame without its fence wait (deferred releases, allocator / list reset)
    kFenceWait,      // BeginFrame: the frame slot's fence (our GPU work of three frames ago)
    kFlushWait,      // NativeDevice::Flush / WaitIdle: the GPU wait
    kSubmit,         // EndFrame / Flush: Close + ExecuteCommandLists + Signal
    kPresentChain,   // PresentFrame: front texture lookup, LUT, gamma + swap post effect recording (and the XE_SWAP
                     // staging)
    kPresentSdkPre,  // Presenter::RefreshGuestOutput before our refresher (owner check, guest output image)
    kPresentSdkPost, // RefreshGuestOutput after our refresher: host vblank latch wait (up to 8 ms), mailbox, paint
                     // request; the XE_SWAP mode's publish on the game thread
    kSwapOther,      // the rest of the native OnSwap (texture cache EndFrame, bookkeeping)
    kInsideCount,    // ---- outside the native entry points:
    kMutexWait = kInsideCount, // a contended render mutex (RenderLock), any hook
    kGuestSwap,      // the guest's own D3D Swap call (its ring / vblank / presentation-interval waits)
    kShadowRecord,   // pdz::shadow::Record (the ~1.4 KB device snapshot per draw / clear / resolve; drawcost)
    kShadowAssert,   // the shadow assert (pdz_render_shadow_assert, a test aid the runners turn on): the flush hook's
                     // own work and the after-draw packet check (drawcost)
    kGuestD3D,       // the guest's D3D draw / clear / resolve / BeginVertices calls (the recompiled D3D runtime: state
                     // flush, PM4 packets), without the shadow assert's work inside them (drawcost)
    // Stage 2 M5 cpusplit (level 1, the Swap thread only, exclusive: nested scopes are subtracted from the enclosing
    // guest call / guest Swap). Guest D3D work outside the hooked draw entries, then the game thread's waits:
    kGuestGate,      // the presentation gate sub_826DF7F0 (ring packets: SCRATCH_REG1, the gate's InsertCallback)
    kWaitRing,       // always 0 since 3 Nov 2026: its hook (sub_826E3968, a pre-TU3 address) was dead and removed
    kWaitGuestFence, // the guest's fence block sub_826DE258 (polls a fence the CP writes), any caller
    kWaitBlockIdle,  // D3DDevice_BlockUntilIdle sub_826DE6A0
    kWaitNotBusy,    // D3DResource_BlockUntilNotBusy sub_826DE880
    kWaitAdaptive,   // the present step's adaptive extra wait in a level (NtYieldExecution loop, lr 821418C4)
    kWaitFeSpin,     // Rare's front-end spin in the present step (NtYieldExecution loop, lr 821418FC)
    kWaitFePace,     // pdz_fix_frontend_pacing's wait for the vblank (game_pace.cpp PdzFrontendPace)
    kWaitYield,      // NtYieldExecution (sub_82AD31F0) from any other call site on the Swap thread
    // Stage 2 M5 step 4 (pdz_native_record_thread, native/native_recthread.h), outside, the producing thread: the
    // hook's copies and queueing after Record, and its waits for the record thread (a full queue / arena / shadow-ring
    // slot, the Swap bound, a drain).
    kRecEnqueue,     // rt.enqueue
    kRecWait,        // wait.rt
    kCount,
  };

  // Cached once per Swap (OnSwap): 0 = off, 1, 2. One relaxed load per scope.
  inline std::atomic<int32_t> g_phase_level{0};
  inline int32_t PhaseLevel() { return g_phase_level.load(std::memory_order_relaxed); }

  // Cumulative TSC cycles per phase (never reset; OnSwap takes the deltas).
  inline std::atomic<uint64_t> g_phase_cycles[uint32_t(Phase::kCount)] = {};
  constexpr uint32_t kNoPhase = ~0u;

  // Render mutex held (one writer at a time): a plain load + store, no lock prefix.
  inline void ChargeLocked(uint32_t phase, uint64_t cycles)
  {
    if (phase < uint32_t(Phase::kCount))
    {
      std::atomic<uint64_t> &a = g_phase_cycles[phase];
      a.store(a.load(std::memory_order_relaxed) + cycles, std::memory_order_relaxed);
    }
  }
  // Any thread (the guest Swap): a fetch_add. A phase is charged by one of the two only.
  inline void ChargeAtomic(Phase phase, uint64_t cycles)
  {
    g_phase_cycles[uint32_t(phase)].fetch_add(cycles, std::memory_order_relaxed);
  }

  struct PhaseThread
  {
    uint32_t current = kNoPhase; // the phase the time since `mark` belongs to (kNoPhase: outside every scope)
    uint64_t mark = 0;
  };
  inline thread_local PhaseThread t_phase;

  // Exclusive phase scope (render mutex held). Entry charges the time so far to the enclosing phase; exit charges the
  // time since the last mark to the current phase and resumes the enclosing one (or `ResumeAs`).
  class PhaseScope
  {
  public:
    explicit PhaseScope(Phase p, int32_t min_level = 1)
    {
      if (PhaseLevel() < min_level)
      {
        return;
      }
      active_ = true;
      PhaseThread &t = t_phase;
      const uint64_t now = __rdtsc();
      if (t.current != kNoPhase)
      {
        ChargeLocked(t.current, now - t.mark);
      }
      resume_ = t.current;
      t.current = uint32_t(p);
      t.mark = now;
    }
    ~PhaseScope()
    {
      if (!active_)
      {
        return;
      }
      PhaseThread &t = t_phase;
      const uint64_t now = __rdtsc();
      ChargeLocked(t.current, now - t.mark);
      t.current = resume_;
      t.mark = now;
    }
    // The enclosing scope continues as `p` after this one (the presenter: before / after our refresher).
    void ResumeAs(Phase p)
    {
      if (active_ && resume_ != kNoPhase)
      {
        resume_ = uint32_t(p);
      }
    }
    PhaseScope(const PhaseScope &) = delete;
    PhaseScope &operator=(const PhaseScope &) = delete;

  private:
    bool active_ = false;
    uint32_t resume_ = kNoPhase;
  };

  // Level 2 draw stages: the time since the last mark goes to `ended`; the thread continues as `next`.
  inline void Lap(Phase ended, Phase next)
  {
    PhaseThread &t = t_phase;
    const uint64_t now = __rdtsc();
    ChargeLocked(uint32_t(ended), now - t.mark);
    t.current = uint32_t(next);
    t.mark = now;
  }

  // -----------------------------------------------------------------------------------------------------------------
  // Outside the native entry points (stage 2 M5 drawcost; level 1): the shadow assert and the guest's own D3D calls.
  // Not under the render mutex: charged with ChargeAtomic. An outside scope nested inside a GuestCall (the assert's
  // flush hook runs inside the guest draw) is subtracted from the GuestCall through t_nested_outside.
  // -----------------------------------------------------------------------------------------------------------------
  inline thread_local uint64_t t_nested_outside = 0;

  // Cycles of an outside phase measured by hand (an rdtsc pair taken while PhaseLevel() > 0).
  inline void ChargeOutside(Phase phase, uint64_t cycles)
  {
    ChargeAtomic(phase, cycles);
    t_nested_outside += cycles;
  }

  class OutsideScope
  {
  public:
    explicit OutsideScope(Phase p) : phase_(p)
    {
      if (PhaseLevel() > 0)
      {
        t0_ = __rdtsc();
      }
    }
    ~OutsideScope()
    {
      if (t0_)
      {
        ChargeOutside(phase_, __rdtsc() - t0_);
      }
    }
    OutsideScope(const OutsideScope &) = delete;
    OutsideScope &operator=(const OutsideScope &) = delete;

  private:
    Phase phase_;
    uint64_t t0_ = 0;
  };

  // A guest D3D call (__imp__ of a hooked draw / clear / resolve / BeginVertices): guest.d3d, minus the outside
  // scopes nested in it.
  class GuestCall
  {
  public:
    GuestCall()
    {
      if (PhaseLevel() > 0)
      {
        nested0_ = t_nested_outside;
        t0_ = __rdtsc();
      }
    }
    ~GuestCall()
    {
      if (t0_)
      {
        const uint64_t dt = __rdtsc() - t0_;
        const uint64_t nested = t_nested_outside - nested0_;
        const uint64_t net = dt > nested ? dt - nested : 0;
        ChargeAtomic(Phase::kGuestD3D, net);
        t_nested_outside += net; // cpusplit: an enclosing exclusive scope (guest Swap, gate) leaves it out too
      }
    }
    GuestCall(const GuestCall &) = delete;
    GuestCall &operator=(const GuestCall &) = delete;

  private:
    uint64_t t0_ = 0;
    uint64_t nested0_ = 0;
  };

  // Stage 2 M5 cpusplit: true on the thread that calls the guest Swap (set by OnSwap while pdz_frame_timing is on).
  // The cpusplit scopes below charge only there: the loader / worker threads' fence blocks and yields are not part of
  // the game thread's frame.
  inline thread_local bool t_swap_thread = false;

  // An exclusive outside scope on the Swap thread (cpusplit): charges its time minus the outside scopes nested in it
  // (a fence block inside the gate, a ring wait inside a guest draw) and adds its own net time to t_nested_outside so
  // that an enclosing GuestCall / guest Swap / ExclusiveScope leaves it out. One rdtsc pair; the waits are rare per
  // frame. min_level 2: the hooked D3D state setters (tens of
  // thousands of calls per frame in heavy MP), timed only at pdz_frame_timing_phases 2.
  class ExclusiveScope
  {
  public:
    explicit ExclusiveScope(Phase p, int32_t min_level = 1) : phase_(p)
    {
      if (t_swap_thread && PhaseLevel() >= min_level)
      {
        nested0_ = t_nested_outside;
        t0_ = __rdtsc();
      }
    }
    ~ExclusiveScope()
    {
      if (t0_)
      {
        const uint64_t dt = __rdtsc() - t0_;
        const uint64_t nested = t_nested_outside - nested0_;
        const uint64_t net = dt > nested ? dt - nested : 0;
        ChargeAtomic(phase_, net);
        t_nested_outside += net;
      }
    }
    void Retarget(Phase p) { phase_ = p; }
    ExclusiveScope(const ExclusiveScope &) = delete;
    ExclusiveScope &operator=(const ExclusiveScope &) = delete;

  private:
    Phase phase_;
    uint64_t t0_ = 0;
    uint64_t nested0_ = 0;
  };

  // -----------------------------------------------------------------------------------------------------------------
  // Spike log (pdz_frame_timing_spike_ms, stage 2 M5 spikes; frame_timing.cpp): event counters for the per-frame
  // 'pdz_timing: spike' line. Cumulative (never reset; OnSwap takes the deltas). Counted only while the spike log is
  // on (one relaxed load); every site is rare per frame (a texture or PSO creation, a GPU wait, a hook that runs a few
  // times per frame), so a fetch_add from whichever thread runs it (game thread or record thread).
  // -----------------------------------------------------------------------------------------------------------------
  enum class Event : uint32_t
  {
    kInsertCallback = 0, // D3DDevice_InsertCallback hook calls (game thread; queued with the record thread)
    kSnapshot,           // de-rez snapshot readbacks, synchronous (copy, Flush = submit + GPU wait, map, guest write)
    kSnapshotCycles,     //   TSC cycles of those, from the copy to the guest write
    kSnapshotWaitCycles, //   of which the Flush (the frame's GPU work so far, and the frames still in flight)
    // pdz_native_async_readback (native_surfaces.cpp asyncrb): the copy is submitted without a wait; the guest write
    // comes later (a native frame end, the next snapshot, or DeResThread before its read). Started - done = pending.
    kSnapshotAsync,           // async snapshot copies started (copy + submit on the thread that ran InsertCallback)
    kSnapshotAsyncDone,       //   their guest writes (any thread)
    kSnapshotAsyncFallback,   //   asked for but run synchronously (every async slot still pending, or no buffer)
    kSnapshotAsyncWaitCycles, //   TSC cycles DeResThread waited for the GPU before reading the snapshot
    kFlushWait,          // NativeDevice::Flush / WaitIdle calls that blocked on the GPU (any caller)
    kFenceWait,          // BeginFrame waits for the frame slot's fence (our GPU work of three frames ago)
    kPsoCreate,          // CreateGraphicsPipelineState (pipeline cache misses)
    kTexCreate,          // guest textures created (CreateUploadTexture: resource + untile + copy)
    kTexUploadBytes,     //   their upload bytes (GetCopyableFootprints total)
    kTexHash,            // texture content / full-extent hashes (the lookup's slow path)
    kTexHashBytes,       //   bytes hashed
    kResourceCreate,     // CreateCommittedResource calls (textures, surfaces, mirrors, buffers)
    kResourceCycles,     //   TSC cycles inside them
    kUploadGrow,         // upload buffer growths mid-frame (a new, larger upload heap)
    kDescriptorFull,     // descriptor ring full (the draw / pass is skipped)
    kResolve,            // native resolves (OnResolve; no CPU readback: the native resolve never writes guest memory)
    kCount,
  };
  // pdz_frame_timing_spike_ms as set at the previous Swap (0 = off), cached once per Swap like the phase level.
  inline std::atomic<int32_t> g_spike_ms{0};
  inline bool SpikeOn() { return g_spike_ms.load(std::memory_order_relaxed) > 0; }
  inline std::atomic<uint64_t> g_events[uint32_t(Event::kCount)] = {};
  inline void CountEvent(Event e, uint64_t n = 1)
  {
    if (SpikeOn())
    {
      g_events[uint32_t(e)].fetch_add(n, std::memory_order_relaxed);
    }
  }
  // An rdtsc pair charged to a cycles event (no rdtsc while the spike log is off).
  class EventTimer
  {
  public:
    explicit EventTimer(Event cycles) : event_(cycles), t0_(SpikeOn() ? __rdtsc() : 0) {}
    ~EventTimer()
    {
      if (t0_)
      {
        g_events[uint32_t(event_)].fetch_add(__rdtsc() - t0_, std::memory_order_relaxed);
      }
    }
    EventTimer(const EventTimer &) = delete;
    EventTimer &operator=(const EventTimer &) = delete;

  private:
    Event event_;
    uint64_t t0_;
  };

  // Stage 2 M5 step 4: the native renderer runs on the record thread (set once at startup by
  // native_recthread.cpp). The game thread lines then leave the native cpu time out of the game thread's frame
  // (it is the worker's) and the record thread line follows them.
  void SetNativeOffThread(bool off_thread);

  // Native draw calls (IssueDraw, issued or skipped) since the session start, for the per-draw cost in the phases
  // line. Render mutex held: a plain load + store.
  inline std::atomic<uint64_t> g_draw_calls{0};
  inline void CountDrawLocked()
  {
    g_draw_calls.store(g_draw_calls.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
  }

  // The native renderer's GPU time of one finished native frame (native_device.cpp: timestamps around each of its
  // command lists, read back when the frame's slot comes round again), added to the frame the Swap closes next.
  void AddNativeGpuUs(uint64_t us);

  // A native frame handed to the presenter (native_present.cpp; game thread or the GPU command processor thread):
  // delay_us from the frame's native OnSwap to the refresh, gap_us since the previous refresh (0 = the first).
  // `where`: a string literal naming the present timing, shown in the line.
  void AddPresent(uint64_t delay_us, uint64_t gap_us, const char *where);

  // Swap hook (shadow_hooks.cpp), first thing, every Swap whatever the shadow / native switches: closes the
  // frame that ends here (wall-clock interval since the previous Swap plus the accumulated native / shadow time).
  // base / device (r3 of the guest Swap): the D3D vblank count for the spike line (0 = not read).
  void OnSwap(const uint8_t *base = nullptr, uint32_t device = 0);
  // The session line (frames after pdz_frame_timing_skip); called by pdz::test::LogSummary at test exit.
  void LogSummary(const char *why);

  // The background log thread (log 299: about one 25 ms frame every 600 in Jungle, where the record thread had no
  // headroom). Periodic lines are never written on the game (Swap) thread or the record thread: `fn` runs on a low
  // priority thread, in order (at most 1024 waiting; more are dropped and counted). Lost at a hard exit, by design:
  // only periodic statistics go this way; summaries at exit stay synchronous.
  void Defer(std::function<void()> fn);
  // A line formatted by the caller (whose counters are not safe to read elsewhere), written on that thread.
  void DeferLine(std::string line);
} // namespace pdz::timing

// A periodic line: formatted here, written on the background log thread (pdz::timing::DeferLine).
#define PDZ_LOG_DEFERRED(...) ::pdz::timing::DeferLine(fmt::format(__VA_ARGS__))
// The same line written in place (a summary, e.g. at exit) or deferred (periodic).
#define PDZ_LOG_PERIODIC(periodic, ...)                                                                              \
  do                                                                                                                 \
  {                                                                                                                  \
    if (periodic)                                                                                                    \
    {                                                                                                                \
      PDZ_LOG_DEFERRED(__VA_ARGS__);                                                                                 \
    }                                                                                                                \
    else                                                                                                             \
    {                                                                                                                \
      REXLOG_INFO(__VA_ARGS__);                                                                                      \
    }                                                                                                                \
  } while (0)
