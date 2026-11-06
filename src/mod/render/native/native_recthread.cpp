// Native renderer, stage 2 M5 step 4: the record thread (native_recthread.h; design and the guest-memory read list in
// PDZRenderResearch\native\stage2\m5\m5_record_thread.md).
//
// Queue: a ring of fixed commands (g_cmd_count) plus a byte arena for their payloads (g_arena_bytes), one consumer (the
// worker), producers serialised by one mutex (the hooks of whichever thread owns the D3D device, and the CP_RB_WPTR
// filter). A payload is a list of chunks:
//   kMirror   bytes at a device offset, copied into the worker's device mirror (loops, RT / DS, viewport, +12948, the
//             resolve registers, the whole float constant blocks at the first draw on a device and at every Swap)
//   kVsGroups / kPsGroups  a dirty mask (u64, bit 63 = c0..c3) and the 64-byte groups it names, into the mirror
//   kCopy     a guest range copied at the hook (object headers, caller memory, UP data), added to the command's view
// The worker applies the chunks, sets the view (native_view.h: the mirror as the device struct, then the copies) and
// calls the unchanged native entry point under the render mutex (commands in batches of up to kBatch per lock).
//
// Producer waits (wait.rt): a full command ring / arena, a shadow-ring slot the worker still reads (DrawRecords are
// read in place from the shadow ring: 4096 slots), the Swap bound (Swap N+1 waits for the worker's Swap N), the drain.
// A payload larger than a quarter of the arena drains the queue and runs on the producer thread itself (same order).
#include "native_recthread.h"
#include "../cpu_placement.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>
#include <intrin.h>
#include <chrono>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/graphics/null_gpu.h>
#include <rex/logging.h>

#include "../frame_timing.h"
#include "../gpu_sync.h"
#include "../shadow.h"
#include "native.h"
#include "native_async_upload.h" // pdz_native_async_upload: the queue look-ahead below
#include "native_draw.h"
#include "native_surfaces.h"
#include "native_textures.h" // kTextureWrite: Textures::NoteCpuWrite
#include "native_view.h"

REXCVAR_DEFINE_BOOL(pdz_native_record_thread, false, "Render",
                    "Stage 2 M5: the native renderer's work (draws, clears, resolves, the de-rez readback, the present) "
                    "on one worker thread instead of inside the hooked D3D calls on the game thread. The game thread "
                    "copies what the draw needs (the shadow record, dirty constant groups, object headers, caller "
                    "memory) and queues a command; the guest's ring kicks are queued too, so the GPU progress the guest "
                    "sees never passes the worker (SDK sdk-16). Read at startup (restart). Off = everything as before.");
REXCVAR_DEFINE_BOOL(pdz_native_record_delta, true, "Render",
                    "pdz_native_record_thread: each draw's device state for the worker's mirror (loop constants 128 "
                    "bytes, render targets 20, viewport 24, vertex program 4, each with a 16-byte chunk header) is sent "
                    "only when it differs from what the mirror already holds (log 299: 8,000+ draws a frame in "
                    "Rooftops' heavy views, about 240 of each draw's ~800 queued bytes). Identical result: the mirror is "
                    "only written by these chunks; any other write to it and a device change resend everything. Off = "
                    "sent with every draw, as before");
REXCVAR_DEFINE_INT32(pdz_native_record_arena_mb, 8, "Render",
                     "pdz_native_record_thread (31 Oct, docs/INTERCEPTION_SPEEDUP.md): the payload arena in MB, rounded "
                     "up to a power of two from 4 to 64 (64 = the size before 31 Oct). At most ~1,500 commands (about "
                     "1 MB) are queued at once, so 8 MB stays in cache where 64 MB went to memory. Payloads over a "
                     "quarter of it run inline after a drain (none at Outpost). Read at startup")
    .range(4, 64)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(pdz_native_record_queue, 8192, "Render",
                     "pdz_native_record_thread (31 Oct, docs/INTERCEPTION_SPEEDUP.md): command slots, rounded up to a "
                     "power of two from 1024 to 65536 (65536 = the size before 31 Oct; 8192 = 850 KB). A full queue "
                     "makes the render thread wait for room. Read at startup")
    .range(1024, 65536)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(pdz_native_record_thread_verify, false, "Render",
                    "pdz_native_record_thread diagnostic: at every draw, compare the worker's float constant mirror "
                    "(built from the dirty groups) with the guest device's 8 KB and count the 64-byte groups that "
                    "differ (constants the game changed without their dirty bit). Costs an 8 KB compare per draw.");

namespace pdz::native::recthread
{
  namespace
  {
    using pdz::shadow::DrawRecord;
    using pdz::shadow::Hook;
    using pdz::shadow::Kind;
    namespace dev = pdz::shadow::dev;

    // The queue's sizes (31 Oct, docs/INTERCEPTION_SPEEDUP.md): powers of two latched at startup from
    // pdz_native_record_queue / pdz_native_record_arena_mb (the maxima are the sizes before 31 Oct).
    constexpr uint32_t kCmdCountMax = 1u << 16;
    constexpr uint64_t kArenaBytesMax = 64ull << 20;
    constexpr uint64_t kCopyMax = kArenaBytesMax / 2; // CopyableRange's plausibility limit (unchanged)
    uint32_t g_cmd_count = kCmdCountMax;      // commands in flight at most
    uint64_t g_arena_bytes = kArenaBytesMax;  // payload ring
    uint64_t g_oversize = kArenaBytesMax / 4; // larger payloads run inline after a drain
    uint64_t PowerOfTwoAtLeast(uint64_t want, uint64_t lo, uint64_t hi)
    {
      uint64_t v = lo;
      while (v < want && v < hi)
      {
        v <<= 1;
      }
      return v;
    }
    constexpr uint32_t kBatch = 64;                   // commands per render-mutex hold
    constexpr uint32_t kPublishEvery = 8;             // worker: done marks published every that many commands
    constexpr uint32_t kShadowMargin = 32;            // shadow-ring slots kept free
    constexpr uint32_t kFloatBytes = 8192;            // VS (+1920) and PS (+6016) float constants, contiguous
    constexpr uint32_t kLoopBytes = 128;              // +10144
    constexpr uint32_t kTargetsOffset = dev::kRenderTargets; // RT0..3 + DS: 20 bytes
    constexpr uint32_t kTargetsBytes = 20;
    constexpr uint32_t kViewportBytes = 24;
    constexpr uint32_t kProgramOffset = 12948;        // the device's current vertex program object (BoundCopy)
    constexpr uint32_t kSurfaceHeader = 20;           // surface header copy: +20 .. +56 (dword1, dword2, .. info +52)
    constexpr uint32_t kSurfaceHeaderBytes = 36;

    enum class Type : uint8_t
    {
      kDraw,
      kFlushPending,
      kEndVertices,
      kClear,
      kResolve,
      kCallback,
      kGamma,
      kSwap,
      kKick,
      kTextureWrite,
      kNop, // Stop: wakes the worker so it sees `running` false
      kCount
    };
    const char *const kTypeNames[] = {"draw", "flush-pending", "end-vertices", "clear", "resolve", "callback",
                                      "gamma",  "swap",          "kick",         "texture-write", "nop"};

    struct Cmd
    {
      Type type = Type::kDraw;
      uint32_t device = 0;
      uint32_t slot = 0;      // kDraw: the record's sequence (shadow ring slot)
      uint32_t ring_head = 0; // shadow ring head when queued (slots below it are free once this command is done)
      uint32_t a[10] = {};
      uint32_t payload_bytes = 0;
      uint64_t payload_pos = 0; // arena offset
      uint64_t arena_end = 0;   // arena head after this payload
      int64_t time_us = 0;      // kSwap: the game thread's Swap time (gpusync::NowUs)
      uint64_t swap_index = 0;  // kSwap
      bool inline_payload = false;
    };

    enum class ChunkKind : uint16_t
    {
      kMirror,
      kVsGroups,
      kPsGroups,
      kCopy,
    };
    struct ChunkHeader
    {
      uint16_t kind;
      uint16_t pad;
      uint32_t guest; // kMirror: device offset; kCopy: guest address
      uint32_t bytes; // data bytes (padded to 16 in the payload)
      uint32_t reserved;
    };
    static_assert(sizeof(ChunkHeader) == 16);
    constexpr uint32_t Align16(uint32_t v) { return (v + 15u) & ~15u; }
    constexpr uint32_t ChunkSize(uint32_t bytes) { return 16u + Align16(bytes); }

    // Wait kinds (game thread), for the counters.
    enum WaitKind : uint32_t
    {
      kWaitRoom = 0, // command ring / arena / shadow-ring slot
      kWaitSwap,     // Swap bound
      kWaitDrain,    // drain (oversize inline, test exit, shutdown)
      kWaitKinds
    };
    const char *const kWaitNames[] = {"room", "swap bound", "drain"};

    // Per-frame producer counters (producer lock), pushed as a sample at each Swap.
    struct ProducerFrame
    {
      double wait_ms[kWaitKinds] = {};
      uint64_t commands = 0;
      uint64_t const_bytes = 0, up_bytes = 0, other_bytes = 0;
      uint64_t full_resyncs = 0;
      uint64_t kicks_deferred = 0, kicks_direct = 0;
      uint64_t depth_sum = 0, depth_max = 0, depth_samples = 0;
      uint64_t inline_runs = 0;
      uint64_t verify_draws = 0, verify_vs_groups = 0, verify_ps_groups = 0;
    };
    struct ProducerSample
    {
      uint64_t swap = 0;
      ProducerFrame f;
    };
    struct WorkerSample
    {
      uint64_t swap = 0;
      double busy_ms = 0.0;
      uint64_t commands = 0;
    };

    struct State
    {
      // Producer side (produce_mutex).
      std::mutex produce_mutex;
      uint64_t cmd_head_local = 0;   // commands queued (producer's copy of cmd_head)
      uint64_t arena_head = 0;       // bytes reserved (monotonic)
      uint32_t last_device = 0;      // device of the last command with a device mirror
      bool need_full = true;         // next draw sends the whole float blocks
      uint64_t swaps_queued = 0;     // Swap commands queued (= Swap hooks)
      uint32_t pending_vs = 0, pending_ps = 0; // shaders of a queued BeginVertices batch not yet flushed
      ProducerFrame frame;
      std::vector<uint8_t> oversize;  // payload of an inline command
      alignas(16) uint8_t sent[kFloatBytes] = {}; // verify: the float constants as the worker's mirror will hold them
      // pdz_native_record_delta: the device state WriteDeviceBits last sent (loop, targets, viewport, program), valid
      // while the worker's mirror holds exactly that (cleared by any other mirror write and by a device change).
      alignas(16) uint8_t bits_sent[176] = {};
      bool bits_valid = false;
      uint32_t verify_logged = 0;

      // Shared. Interception speed-up (31 Oct, docs/INTERCEPTION_SPEEDUP.md): the atomics each side writes for every
      // command are on cache lines of their own, apart from each other and from the producer's fields above (they
      // were all on one or two lines, so each command moved those lines between the render thread's core and the
      // record thread's several times).
      // Written by the producer for every command, read by the worker.
      alignas(64) std::atomic<uint64_t> cmd_head{0}; // published commands
      // Written by the worker for every command, read by the producer.
      alignas(64) std::atomic<uint64_t> completed{0}; // commands done by the worker
      std::atomic<uint64_t> arena_tail{0};   // arena bytes freed
      std::atomic<uint32_t> ring_done{0};    // shadow ring head of the last command done
      std::atomic<uint64_t> swaps_done{0};   // Swap commands done
      // Written only when a side goes to sleep, read by the other side for every command.
      alignas(64) std::atomic<bool> worker_waiting{false};
      std::atomic<bool> producer_waiting{false};
      // Written by the worker twice per command, read only by the producer's wait log.
      alignas(64) std::atomic<int32_t> executing{-1}; // command type the worker runs now (-1: idle), for the wait log
      // Producer counters (one writer, read by the logs).
      alignas(64) std::atomic<uint64_t> bits_sent_total{0}, bits_skipped_total{0}; // pdz_native_record_delta: device
                                                                                    // state parts
      // Read-mostly.
      alignas(64) std::atomic<bool> running{false};
      // Auto-reset events: set when the other side announced it waits (a stale signal costs one extra loop).
      HANDLE work_event = nullptr; // worker: a command was published
      HANDLE done_event = nullptr; // producer: a command was completed
      std::atomic<uint8_t *> base{nullptr};
      std::unique_ptr<Cmd[]> cmds;
      std::unique_ptr<uint8_t[]> arena;
      std::thread worker;
      bool started = false;

      // Worker side.
      alignas(64) uint8_t mirror[dev::kDeviceSize] = {};
      uint32_t mirror_device = 0;
      double busy_ms_frame = 0.0;
      uint64_t commands_frame = 0;
      uint64_t lost_records = 0; // a draw whose shadow-ring slot was overwritten (a bug: logged)

      // Spike log (pdz_frame_timing_spike_ms; TakeSpikeTotals): cumulative counters the game thread's Swap takes deltas
      // of, and per-interval maxima it takes with an exchange. busy / payload: one writer each (worker / producer
      // lock), a relaxed load + store; the waits are rare (a fetch_add).
      // The producer's (every command) and the worker's (every batch) on separate lines.
      alignas(64) std::atomic<uint64_t> spike_payload_bytes{0}; // producer: payload bytes queued
      std::atomic<uint64_t> spike_depth_max{0};     // queue depth at enqueue, since the last take
      std::atomic<uint64_t> spike_wait_us[kWaitKinds] = {}, spike_waits[kWaitKinds] = {};
      std::atomic<uint64_t> spike_arena_waits{0}, spike_arena_wraps{0};
      alignas(64) std::atomic<uint64_t> spike_busy_ns{0}; // worker: batch wall time
      std::atomic<uint64_t> spike_longest{0};       // the longest worker command since the last take (cycles << 8 |
                                                    // its type)

      // Totals (any thread, relaxed) and samples (samples_mutex).
      std::atomic<uint64_t> total_commands{0}, total_kicks_deferred{0}, total_kicks_direct{0}, total_inline{0},
          total_lost{0};
      std::mutex samples_mutex;
      std::vector<ProducerSample> psamples;
      std::vector<WorkerSample> wsamples;
    };
    State *g_state = nullptr; // allocated by ChooseAtStartup (the mirror and the verify copy are large)
    inline State &S() { return *g_state; }

    thread_local int64_t t_swap_us = 0;      // the executing Swap command's game-thread time
    thread_local uint64_t t_wait_cycles = 0; // waits inside the current hook (rdtsc, phases on)

    double QpcMs()
    {
      static const double ms_per_tick = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return 1000.0 / double(f.QuadPart);
      }();
      LARGE_INTEGER t;
      QueryPerformanceCounter(&t);
      return double(t.QuadPart) * ms_per_tick;
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Producer waits (produce_mutex held). The worker updates ring_done / arena_tail / swaps_done before `completed`,
    // so waiting on `completed` covers every condition.
    // ---------------------------------------------------------------------------------------------------------------
    // Returns whether it had to wait.
    template <typename Done>
    bool WaitFor(WaitKind kind, Done done)
    {
      State &s = S();
      if (done())
      {
        return false;
      }
      const bool phases = pdz::timing::PhaseLevel() > 0;
      const uint64_t c0 = phases ? __rdtsc() : 0;
      const double t0 = QpcMs();
      double next_log = t0 + 5000.0;
      for (uint32_t spin = 0; !done(); ++spin)
      {
        if (spin < 256)
        {
          _mm_pause();
          continue;
        }
        s.producer_waiting.store(true, std::memory_order_seq_cst);
        if (!done())
        {
          // The worker sets the event at its next publish while we wait; bounded so the log below can run.
          WaitForSingleObject(s.done_event, 100);
        }
        s.producer_waiting.store(false, std::memory_order_relaxed);
        const double now = QpcMs();
        if (now > next_log)
        {
          next_log = now + 5000.0;
          const int32_t ex = s.executing.load(std::memory_order_relaxed);
          REXLOG_WARN("pdz_recthread: the game thread has waited {:.1f} s for the record thread ({}): commands done {} "
                      "of {}, worker {}",
                      (now - t0) / 1000.0, kWaitNames[kind], s.completed.load(std::memory_order_relaxed),
                      s.cmd_head.load(std::memory_order_relaxed),
                      ex >= 0 && ex < int32_t(Type::kCount) ? kTypeNames[ex] : "idle");
        }
      }
      const double waited_ms = QpcMs() - t0;
      s.frame.wait_ms[kind] += waited_ms;
      s.spike_wait_us[kind].fetch_add(uint64_t(waited_ms * 1000.0), std::memory_order_relaxed);
      s.spike_waits[kind].fetch_add(1, std::memory_order_relaxed);
      if (phases)
      {
        t_wait_cycles += __rdtsc() - c0;
      }
      return true;
    }

    void WaitDrained()
    {
      State &s = S();
      WaitFor(kWaitDrain, [&] { return s.completed.load(std::memory_order_acquire) == s.cmd_head_local; });
    }

    // A shadow-ring slot for the next Record: the worker reads DrawRecords in place. With nothing in flight it reads
    // none (a pending BeginVertices batch is a copy), whatever ring_done says (records with no command: native off).
    void WaitShadowRoom()
    {
      State &s = S();
      const uint32_t head = pdz::shadow::Get().head;
      WaitFor(kWaitRoom, [&] {
        return head - s.ring_done.load(std::memory_order_acquire) < pdz::shadow::RingSlots() - kShadowMargin ||
               s.completed.load(std::memory_order_acquire) == s.cmd_head_local;
      });
    }

    // rt.enqueue / wait.rt for one hook (phases on): constructed after Record (shadow.record is its own phase).
    class EnqueueScope
    {
    public:
      EnqueueScope()
      {
        if (pdz::timing::PhaseLevel() > 0)
        {
          t0_ = __rdtsc();
          waits0_ = t_wait_cycles;
        }
      }
      ~EnqueueScope()
      {
        if (t0_)
        {
          const uint64_t dt = __rdtsc() - t0_;
          const uint64_t waits = t_wait_cycles - waits0_;
          pdz::timing::ChargeOutside(pdz::timing::Phase::kRecWait, waits);
          pdz::timing::ChargeOutside(pdz::timing::Phase::kRecEnqueue, dt > waits ? dt - waits : 0);
        }
      }
      EnqueueScope(const EnqueueScope &) = delete;
      EnqueueScope &operator=(const EnqueueScope &) = delete;

    private:
      uint64_t t0_ = 0;
      uint64_t waits0_ = 0;
    };

    // ---------------------------------------------------------------------------------------------------------------
    // Payload writer
    // ---------------------------------------------------------------------------------------------------------------
    struct Writer
    {
      uint8_t *p = nullptr;
      uint8_t *end = nullptr;

      uint8_t *Chunk(ChunkKind kind, uint32_t guest, uint32_t bytes)
      {
        const uint32_t size = ChunkSize(bytes);
        if (!p || p + size > end)
        {
          return nullptr; // sizes are computed exactly; never reached
        }
        ChunkHeader h{uint16_t(kind), 0, guest, bytes, 0};
        std::memcpy(p, &h, sizeof(h));
        uint8_t *data = p + 16;
        p += size;
        return data;
      }
      // Any mirror write but WriteDeviceBits' own clears pdz_native_record_delta's record of what the mirror holds.
      void Mirror(const uint8_t *dev_ptr, uint32_t offset, uint32_t bytes)
      {
        S().bits_valid = false;
        MirrorKeep(dev_ptr, offset, bytes);
      }
      void MirrorKeep(const uint8_t *dev_ptr, uint32_t offset, uint32_t bytes)
      {
        if (uint8_t *d = Chunk(ChunkKind::kMirror, offset, bytes))
        {
          std::memcpy(d, dev_ptr + offset, bytes);
        }
      }
      void Copy(const uint8_t *base, uint32_t guest, uint32_t bytes)
      {
        if (uint8_t *d = Chunk(ChunkKind::kCopy, guest, bytes))
        {
          std::memcpy(d, pdz::shadow::GuestPtr(base, guest), bytes);
        }
      }
    };

    // A guest range worth copying: a plausible pointer, the whole range inside the user address space.
    bool CopyableRange(uint32_t guest, uint64_t bytes)
    {
      return bytes > 0 && bytes <= kCopyMax && pdz::shadow::IsGuestPointer(guest) &&
             uint64_t(guest) + bytes <= 0xFFFF0000ull;
    }

    // Object field copies for the native read sites (native_view.h): shader objects (FindOrReadShader: VS +40 / +600,
    // PS +12 / +60; BoundCopy's fallback VS +12), the current vertex program's +40 / +600 (BoundCopy), surface headers
    // (Surfaces::Lookup), a texture object's fetch dwords (DecodeTextureObject), the index buffer header.
    struct Plan
    {
      uint32_t guest[24];
      uint32_t bytes[24];
      uint32_t count = 0;
      uint32_t size = 0;
      void Add(uint32_t g, uint32_t b)
      {
        if (count < 24 && CopyableRange(g, b))
        {
          // Each range once (two RTs may share a surface; the pending batch may use the command's shaders).
          for (uint32_t i = 0; i < count; ++i)
          {
            if (guest[i] == g && bytes[i] == b)
            {
              return;
            }
          }
          guest[count] = g;
          bytes[count] = b;
          ++count;
          size += ChunkSize(b);
        }
      }
      void Shaders(uint32_t vs, uint32_t ps)
      {
        if (vs)
        {
          Add(vs + 12, 4);
          Add(vs + 40, 4);
          Add(vs + 600, 4);
        }
        if (ps)
        {
          Add(ps + 12, 4);
          Add(ps + 60, 4);
        }
      }
      void Surface(uint32_t surface)
      {
        if (surface)
        {
          Add(surface + kSurfaceHeader, kSurfaceHeaderBytes);
        }
      }
      void Write(Writer &w, const uint8_t *base) const
      {
        for (uint32_t i = 0; i < count; ++i)
        {
          w.Copy(base, guest[i], bytes[i]);
        }
      }
    };

    // The RT / DS pointers of the live device (resolve, flush points).
    void PlanLiveSurfaces(Plan &plan, const uint8_t *base, uint32_t device)
    {
      for (uint32_t i = 0; i < 5; ++i)
      {
        plan.Surface(pdz::shadow::Load32(base, device + kTargetsOffset + 4 * i));
      }
    }
    void PlanPending(Plan &plan)
    {
      State &s = S();
      plan.Shaders(s.pending_vs, s.pending_ps);
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Queue
    // ---------------------------------------------------------------------------------------------------------------
    struct Reservation
    {
      uint8_t *ptr = nullptr;
      uint64_t pos = 0;
      uint64_t end = 0;
      bool inline_payload = false;
    };

    Reservation Reserve(uint32_t bytes)
    {
      State &s = S();
      Reservation r;
      const uint64_t size = (uint64_t(bytes) + 63u) & ~63ull;
      if (size > g_oversize || !s.running.load(std::memory_order_relaxed))
      {
        s.oversize.resize(std::max<size_t>(bytes, 16));
        r.ptr = s.oversize.data();
        r.inline_payload = true;
        r.end = s.arena_head;
        return r;
      }
      if (size == 0)
      {
        r.end = s.arena_head;
        return r;
      }
      uint64_t head = s.arena_head;
      const uint64_t pos = head & (g_arena_bytes - 1);
      if (pos + size > g_arena_bytes)
      {
        head += g_arena_bytes - pos; // skip the tail: payloads are contiguous
        s.spike_arena_wraps.fetch_add(1, std::memory_order_relaxed);
      }
      const uint64_t need_end = head + size;
      if (WaitFor(kWaitRoom, [&] { return need_end - s.arena_tail.load(std::memory_order_acquire) <= g_arena_bytes; }))
      {
        s.spike_arena_waits.fetch_add(1, std::memory_order_relaxed);
      }
      r.pos = head & (g_arena_bytes - 1);
      r.ptr = s.arena.get() + r.pos;
      r.end = need_end;
      s.arena_head = need_end;
      return r;
    }

    void Execute(const Cmd &c, const uint8_t *payload);

    // Queues `c` with the payload written into `r` (or runs it here: oversize payload or the worker stopped).
    void Publish(Cmd &c, const Reservation &r)
    {
      State &s = S();
      s.frame.commands += 1;
      if (r.inline_payload)
      {
        WaitDrained();
        s.frame.inline_runs += 1;
        s.total_inline.fetch_add(1, std::memory_order_relaxed);
        RenderLock lock(RenderMutex());
        Execute(c, r.ptr);
        return;
      }
      WaitFor(kWaitRoom, [&] { return s.cmd_head_local - s.completed.load(std::memory_order_acquire) < g_cmd_count; });
      const uint64_t depth = s.cmd_head_local - s.completed.load(std::memory_order_relaxed);
      s.frame.depth_sum += depth;
      s.frame.depth_max = std::max(s.frame.depth_max, depth);
      s.frame.depth_samples += 1;
      if (depth > s.spike_depth_max.load(std::memory_order_relaxed))
      {
        s.spike_depth_max.store(depth, std::memory_order_relaxed); // races the Swap's exchange: a max may be lost
      }
      s.spike_payload_bytes.store(s.spike_payload_bytes.load(std::memory_order_relaxed) + c.payload_bytes,
                                  std::memory_order_relaxed);
      c.payload_pos = r.pos;
      c.arena_end = r.end;
      c.ring_head = pdz::shadow::Get().head;
      s.cmds[s.cmd_head_local & (g_cmd_count - 1)] = c;
      ++s.cmd_head_local;
      s.cmd_head.store(s.cmd_head_local, std::memory_order_seq_cst);
      if (s.worker_waiting.load(std::memory_order_seq_cst))
      {
        SetEvent(s.work_event);
      }
    }

    void NoteBase(uint8_t *base)
    {
      State &s = S();
      if (s.base.load(std::memory_order_relaxed) != base)
      {
        s.base.store(base, std::memory_order_release);
      }
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Device state of a command: the float constant groups (or the whole blocks), loops, RT / DS, viewport, +12948.
    // ---------------------------------------------------------------------------------------------------------------
    uint32_t ConstSize(const DrawRecord *r, bool full)
    {
      if (full || !r)
      {
        return ChunkSize(kFloatBytes);
      }
      const uint32_t vs = uint32_t(std::popcount(r->dirty[0])), ps = uint32_t(std::popcount(r->dirty[1]));
      return (vs ? ChunkSize(8 + 64 * vs) : 0) + (ps ? ChunkSize(8 + 64 * ps) : 0);
    }

    void WriteGroups(Writer &w, ChunkKind kind, uint64_t mask, const uint8_t *src)
    {
      if (!mask)
      {
        return;
      }
      uint8_t *d = w.Chunk(kind, 0, 8 + 64 * uint32_t(std::popcount(mask)));
      if (!d)
      {
        return;
      }
      std::memcpy(d, &mask, 8);
      d += 8;
      for (uint64_t m = mask; m;)
      {
        const uint32_t g = uint32_t(std::countl_zero(m)); // bit 63 = group 0 (c0..c3)
        std::memcpy(d, src + 64 * g, 64);
        d += 64;
        m &= ~(1ull << (63 - g));
      }
    }

    // The draw's constants into the payload; keeps the verify copy (pdz_native_record_thread_verify).
    void WriteConsts(Writer &w, const uint8_t *dev_ptr, const DrawRecord *r, bool full)
    {
      State &s = S();
      const uint8_t *floats = dev_ptr + dev::kVsAluConstants;
      if (full || !r)
      {
        w.Mirror(dev_ptr, dev::kVsAluConstants, kFloatBytes);
        s.frame.const_bytes += kFloatBytes;
        s.frame.full_resyncs += 1;
        if (REXCVAR_GET(pdz_native_record_thread_verify))
        {
          std::memcpy(s.sent, floats, kFloatBytes);
        }
        return;
      }
      WriteGroups(w, ChunkKind::kVsGroups, r->dirty[0], floats);
      WriteGroups(w, ChunkKind::kPsGroups, r->dirty[1], dev_ptr + dev::kPsAluConstants);
      s.frame.const_bytes += 64ull * (std::popcount(r->dirty[0]) + std::popcount(r->dirty[1]));
      if (REXCVAR_GET(pdz_native_record_thread_verify))
      {
        // What the worker's mirror holds after this draw's groups, against the device now: a group that differs was
        // changed without its dirty bit (the synchronous path would have read the new value at its next upload).
        for (uint32_t block = 0; block < 2; ++block)
        {
          const uint64_t mask = r->dirty[block];
          uint8_t *sent = s.sent + 4096 * block;
          const uint8_t *live = floats + 4096 * block;
          for (uint32_t g = 0; g < 64; ++g)
          {
            if (mask & (1ull << (63 - g)))
            {
              std::memcpy(sent + 64 * g, live + 64 * g, 64);
            }
            else if (std::memcmp(sent + 64 * g, live + 64 * g, 64) != 0)
            {
              ++(block ? s.frame.verify_ps_groups : s.frame.verify_vs_groups);
              if (s.verify_logged < 8)
              {
                ++s.verify_logged;
                REXLOG_INFO("pdz_recthread: verify: {} constants c{}..c{} differ from the device at draw #{} (caller "
                            "{:08X}) without their dirty bit",
                            block ? "PS" : "VS", 4 * g, 4 * g + 3, r->sequence, r->caller);
              }
              std::memcpy(sent + 64 * g, live + 64 * g, 64); // count each change once
            }
          }
        }
        s.frame.verify_draws += 1;
      }
    }

    constexpr uint32_t kDeviceBitsSize =
        ChunkSize(kLoopBytes) + ChunkSize(kTargetsBytes) + ChunkSize(kViewportBytes) + ChunkSize(4);
    void WriteDeviceBits(Writer &w, const uint8_t *dev_ptr)
    {
      struct Part
      {
        uint32_t offset, bytes, at;
      };
      static constexpr Part kParts[] = {{dev::kLoopConstants, kLoopBytes, 0},
                                        {kTargetsOffset, kTargetsBytes, kLoopBytes},
                                        {dev::kViewport, kViewportBytes, kLoopBytes + kTargetsBytes},
                                        {kProgramOffset, 4, kLoopBytes + kTargetsBytes + kViewportBytes}};
      static_assert(kLoopBytes + kTargetsBytes + kViewportBytes + 4 <= sizeof(State::bits_sent));
      State &s = S();
      const bool delta = s.bits_valid && REXCVAR_GET(pdz_native_record_delta);
      uint64_t skipped = 0;
      for (const Part &p : kParts)
      {
        const uint8_t *now = dev_ptr + p.offset;
        if (delta && std::memcmp(now, s.bits_sent + p.at, p.bytes) == 0)
        {
          ++skipped; // the worker's mirror already holds these bytes
          continue;
        }
        w.MirrorKeep(dev_ptr, p.offset, p.bytes);
        std::memcpy(s.bits_sent + p.at, now, p.bytes);
      }
      s.bits_valid = true;
      // One writer (produce_mutex): a load and a store, not a locked add per draw.
      s.bits_sent_total.store(s.bits_sent_total.load(std::memory_order_relaxed) + 4, std::memory_order_relaxed);
      s.bits_skipped_total.store(s.bits_skipped_total.load(std::memory_order_relaxed) + skipped,
                                 std::memory_order_relaxed);
    }

    // The device changed (boot device -> game device): the worker clears its mirror at the first command with another
    // device, so every producer path that queues a command with a device calls this first and the next draw sends the
    // whole blocks.
    void NoteDevice(uint32_t device)
    {
      State &s = S();
      if (device != s.last_device)
      {
        s.last_device = device;
        s.need_full = true;
        s.bits_valid = false; // the worker clears its mirror for the new device
      }
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Worker
    // ---------------------------------------------------------------------------------------------------------------
    void ApplyPayload(const uint8_t *p, uint32_t bytes, view::View &v)
    {
      State &s = S();
      const uint8_t *end = p + bytes;
      while (p + 16 <= end)
      {
        ChunkHeader h;
        std::memcpy(&h, p, sizeof(h));
        const uint8_t *data = p + 16;
        p += ChunkSize(h.bytes);
        if (p > end)
        {
          break;
        }
        switch (ChunkKind(h.kind))
        {
        case ChunkKind::kMirror:
          if (uint64_t(h.guest) + h.bytes <= dev::kDeviceSize)
          {
            std::memcpy(s.mirror + h.guest, data, h.bytes);
          }
          break;
        case ChunkKind::kVsGroups:
        case ChunkKind::kPsGroups:
        {
          uint64_t mask;
          std::memcpy(&mask, data, 8);
          const uint8_t *src = data + 8;
          uint8_t *dst = s.mirror + (ChunkKind(h.kind) == ChunkKind::kVsGroups ? dev::kVsAluConstants
                                                                                : dev::kPsAluConstants);
          for (uint64_t m = mask; m;)
          {
            const uint32_t g = uint32_t(std::countl_zero(m));
            std::memcpy(dst + 64 * g, src, 64);
            src += 64;
            m &= ~(1ull << (63 - g));
          }
          break;
        }
        case ChunkKind::kCopy:
          v.Add(h.guest, h.bytes, data);
          break;
        }
      }
    }

    void Execute(const Cmd &c, const uint8_t *payload)
    {
      State &s = S();
      uint8_t *base = s.base.load(std::memory_order_acquire);
      view::View v;
      if (c.device && c.device != s.mirror_device)
      {
        std::memset(s.mirror, 0, sizeof(s.mirror));
        s.mirror_device = c.device;
      }
      if (s.mirror_device)
      {
        v.Add(s.mirror_device, dev::kDeviceSize, s.mirror);
      }
      if (payload && c.payload_bytes)
      {
        ApplyPayload(payload, c.payload_bytes, v);
      }
      if (!base && c.type != Type::kKick)
      {
        return;
      }
      s.executing.store(int32_t(c.type), std::memory_order_relaxed);
      const view::View *previous = view::t_view;
      view::t_view = &v;
      switch (c.type)
      {
      case Type::kDraw:
      {
        const DrawRecord &r = pdz::shadow::Get().ring[c.slot & pdz::shadow::RingMask()];
        if (r.sequence != c.slot)
        {
          s.total_lost.fetch_add(1, std::memory_order_relaxed);
          if (s.lost_records++ < 4)
          {
            REXLOG_ERROR("pdz_recthread: draw record {} was overwritten before the record thread read it (slot holds "
                         "{}); draw skipped",
                         c.slot, r.sequence);
          }
          break;
        }
        pdz::native::OnDraw(base, r);
        break;
      }
      case Type::kFlushPending:
        if (pdz::native::Enabled())
        {
          pdz::native::FlushPendingBegin(base);
        }
        break;
      case Type::kEndVertices:
        pdz::native::OnEndVertices(base);
        break;
      case Type::kClear:
        pdz::native::OnClear(base, c.device, c.a[0], c.a[1], c.a[2], c.a[3], c.a[4], c.a[5]);
        break;
      case Type::kResolve:
        pdz::native::OnResolve(base, c.device, c.a[0], c.a[1], c.a[2], c.a[3], c.a[4], c.a[5], c.a[6], c.a[7],
                               c.a[8]);
        break;
      case Type::kCallback:
        if (pdz::native::Enabled())
        {
          const pdz::timing::Scope timing(pdz::timing::Bucket::kNative);
          const pdz::timing::PhaseScope phase(pdz::timing::Phase::kCallback);
          pdz::native::Surfaces::Get().OnInsertCallback(base, c.a[0]);
        }
        break;
      case Type::kGamma:
        pdz::native::ApplyGammaRamp(base, c.a[0]);
        break;
      case Type::kTextureWrite:
        pdz::native::Textures::Get().NoteCpuWrite(c.a[0], c.a[1], c.a[2], c.a[3]);
        break;
      case Type::kSwap:
        t_swap_us = c.time_us;
        pdz::native::OnSwap(base, c.device, c.a[0]);
        t_swap_us = 0;
        break;
      case Type::kKick:
        if (!rex::graphics::null_gpu::DeliverWritePointer(c.a[0]))
        {
          static bool logged = false;
          if (!logged)
          {
            logged = true;
            REXLOG_WARN("pdz_recthread: a kick ({:08X}) found no command processor to deliver to", c.a[0]);
          }
        }
        break;
      default:
        break;
      }
      view::t_view = previous;
      s.executing.store(-1, std::memory_order_relaxed);
    }

    // pdz_native_async_upload (log 307): the texture fetch constants of the draws still queued (up to kPrefetchAhead
    // commands past the one about to run) go to Textures::Prefetch, so their uploads are prepared on the upload
    // workers before the record thread reaches those draws. Only bound samplers (the record's texture object
    // non-null), and only constants that differ from the previous queued draw's (consecutive draws mostly share
    // them); Prefetch itself skips anything already cached, prepared or seen this frame. Render mutex held.
    constexpr uint64_t kPrefetchAhead = 8192;
    void PrefetchAhead(State &s, uint64_t tail, uint64_t head)
    {
      static uint64_t scanned = 0;
      static uint8_t last[dev::kTextureCount][dev::kFetchConstantBytes] = {};
      pdz::native::NativeDevice *device = pdz::native::NativeDevice::Get();
      if (!device || !pdz::native::Enabled() || !pdz::native::asyncup::Active(*device))
      {
        return;
      }
      const uint64_t from = std::max(scanned, tail);
      const uint64_t to = std::min(head, tail + kPrefetchAhead);
      if (from >= to)
      {
        return;
      }
      const auto &ring = pdz::shadow::Get().ring;
      pdz::native::Textures &textures = pdz::native::Textures::Get();
      for (uint64_t i = from; i < to; ++i)
      {
        const Cmd &c = s.cmds[i & (g_cmd_count - 1)];
        if (c.type != Type::kDraw)
        {
          continue;
        }
        const DrawRecord &r = ring[c.slot & pdz::shadow::RingMask()];
        if (r.sequence != c.slot)
        {
          continue;
        }
        for (uint32_t slot = 0; slot < dev::kTextureCount && slot < dev::kFetchConstantCount; ++slot)
        {
          if (!r.textures[slot])
          {
            continue;
          }
          const uint8_t *fetch = r.fetch_be + slot * dev::kFetchConstantBytes;
          if (std::memcmp(last[slot], fetch, dev::kFetchConstantBytes) == 0)
          {
            continue;
          }
          std::memcpy(last[slot], fetch, dev::kFetchConstantBytes);
          textures.Prefetch(fetch);
        }
      }
      scanned = to;
    }

    // Overnight 30-31 Oct: where the worker's batch time goes (pdz_frame_timing on): one line every 5 s.
    struct WorkerSplit
    {
      uint64_t lock = 0, prefetch = 0, prewarm = 0, exec = 0, batch = 0;
      uint64_t exec_by_type[16] = {};
      uint64_t swaps_at = 0;
      uint64_t tsc_at = 0;
      std::chrono::steady_clock::time_point time_at{};
    } g_split;

    void SplitLog()
    {
      const auto now = std::chrono::steady_clock::now();
      if (g_split.time_at == std::chrono::steady_clock::time_point{})
      {
        g_split.time_at = now;
        g_split.tsc_at = __rdtsc();
        g_split.swaps_at = S().swaps_done.load(std::memory_order_relaxed);
        return;
      }
      const double secs = std::chrono::duration<double>(now - g_split.time_at).count();
      if (secs < 5.0)
      {
        return;
      }
      const uint64_t tsc = __rdtsc();
      const double tsc_per_ms = double(tsc - g_split.tsc_at) / (secs * 1000.0);
      const uint64_t swaps = S().swaps_done.load(std::memory_order_relaxed);
      const double frames = double(std::max<uint64_t>(1, swaps - g_split.swaps_at));
      const auto ms = [&](uint64_t c) { return tsc_per_ms > 0.0 ? double(c) / tsc_per_ms / frames : 0.0; };
      std::string types;
      for (uint32_t t = 0; t < uint32_t(Type::kCount) && t < 16; ++t)
      {
        if (g_split.exec_by_type[t])
        {
          types += fmt::format(" {} {:.3f}", kTypeNames[t], ms(g_split.exec_by_type[t]));
        }
      }
      REXLOG_INFO("pdz_recthread: worker split ({:.1f} s, ms per frame): batches {:.3f} = render lock wait {:.3f} + "
                       "prefetch scan {:.3f} + prewarm {:.3f} + commands {:.3f} (by type:{}) + loop rest {:.3f}",
                       secs, ms(g_split.batch), ms(g_split.lock), ms(g_split.prefetch), ms(g_split.prewarm),
                       ms(g_split.exec), types,
                       ms(g_split.batch) - ms(g_split.lock) - ms(g_split.prefetch) - ms(g_split.prewarm) - ms(g_split.exec));
      const auto keep = g_split.time_at;
      (void)keep;
      g_split = WorkerSplit{};
      g_split.time_at = now;
      g_split.tsc_at = tsc;
      g_split.swaps_at = swaps;
    }

    void WorkerMain()
    {
      State &s = S();
      SetThreadDescription(GetCurrentThread(), L"PDZ native record thread");
      SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
      pdz::cpu::PlaceCriticalThread("native record thread"); // pdz_cpu_placement
      uint64_t tail = 0;
      for (;;)
      {
        uint64_t head = s.cmd_head.load(std::memory_order_acquire);
        if (tail == head)
        {
          if (!s.running.load(std::memory_order_acquire))
          {
            return;
          }
          // Spin a little (the next command usually follows within microseconds), then sleep on the head.
          bool got = false;
          for (uint32_t spin = 0; spin < 2000; ++spin)
          {
            _mm_pause();
            if (s.cmd_head.load(std::memory_order_acquire) != tail)
            {
              got = true;
              break;
            }
          }
          if (!got)
          {
            s.worker_waiting.store(true, std::memory_order_seq_cst);
            if (s.cmd_head.load(std::memory_order_seq_cst) == tail && s.running.load(std::memory_order_seq_cst))
            {
              WaitForSingleObject(s.work_event, 100); // bounded: `running` is re-read at least every 100 ms
            }
            s.worker_waiting.store(false, std::memory_order_relaxed);
          }
          continue;
        }
        const double t0 = QpcMs();
        // pdz_frame_timing_spike_ms: the longest command of the batch (one rdtsc per command while the log is on).
        const bool spike = pdz::timing::SpikeOn();
        uint64_t longest = 0;
        // Overnight 30-31 Oct: the worker's own split (pdz_frame_timing): lock, prefetch scan, prewarm, commands.
        const bool split = pdz::timing::Enabled();
        const uint64_t s0 = split ? __rdtsc() : 0;
        uint64_t s_exec = 0;
        {
          RenderLock lock(RenderMutex());
          const uint64_t s1 = split ? __rdtsc() : 0;
          PrefetchAhead(s, tail, head); // pdz_native_async_upload
          const uint64_t s2 = split ? __rdtsc() : 0;
          if (pdz::native::NativeDevice::Get() && pdz::native::Enabled())
          {
            pdz::native::Textures::Get().PrewarmStep(); // pdz_native_texture_prewarm (log 315)
          }
          const uint64_t s3 = split ? __rdtsc() : 0;
          if (split)
          {
            g_split.lock += s1 - s0;
            g_split.prefetch += s2 - s1;
            g_split.prewarm += s3 - s2;
          }
          for (uint32_t n = 0; tail != head && n < kBatch; ++n)
          {
            const Cmd &c = s.cmds[tail & (g_cmd_count - 1)];
            const uint8_t *payload = c.payload_bytes ? s.arena.get() + c.payload_pos : nullptr;
            const uint64_t c0 = spike || split ? __rdtsc() : 0;
            Execute(c, payload);
            if (split)
            {
              const uint64_t c1 = __rdtsc();
              s_exec += c1 - c0;
              g_split.exec_by_type[uint32_t(c.type) & 15] += c1 - c0;
            }
            if (spike)
            {
              longest = std::max(longest, ((__rdtsc() - c0) << 8) | uint64_t(c.type));
            }
            s.commands_frame += 1;
            if (c.type == Type::kSwap)
            {
              // The frame this Swap closes: the busy time since the previous Swap command, up to here.
              const double now = QpcMs();
              WorkerSample w;
              w.swap = c.swap_index;
              w.busy_ms = s.busy_ms_frame + (now - t0);
              w.commands = s.commands_frame;
              s.busy_ms_frame = -(now - t0); // the rest of this batch counts for the next frame
              s.commands_frame = 0;
              {
                std::lock_guard<std::mutex> lk(s.samples_mutex);
                s.wsamples.push_back(w);
              }
              s.swaps_done.store(c.swap_index, std::memory_order_release);
            }
            ++tail;
            // Interception speed-up (31 Oct, docs/INTERCEPTION_SPEEDUP.md): the done marks go out every kPublishEvery
            // commands, not after each one (each publish moves their cache line to the producer's core): at once
            // after a Swap (the producer's run-ahead waits on it), when the producer waits, and at the end of the
            // commands seen (a caught-up worker shows completed == cmd_head, as before). The producer then sees
            // up to kPublishEvery - 1 finished commands as still queued for a few microseconds: its room checks
            // (shadow ring, arena, command ring) keep that much more margin; nothing it reads from them is freed
            // early.
            if ((n % kPublishEvery) == kPublishEvery - 1 || tail == head || c.type == Type::kSwap ||
                s.producer_waiting.load(std::memory_order_relaxed))
            {
              s.ring_done.store(c.ring_head, std::memory_order_release);
              s.arena_tail.store(c.arena_end, std::memory_order_release);
              s.completed.store(tail, std::memory_order_seq_cst);
              // One SetEvent per wait (it was one per command while the producer slept): the flag is taken here; a
              // producer still short of room sets it again before it sleeps again.
              if (s.producer_waiting.load(std::memory_order_seq_cst) &&
                  s.producer_waiting.exchange(false, std::memory_order_seq_cst))
              {
                SetEvent(s.done_event);
              }
            }
            if ((n & 15) == 15)
            {
              head = s.cmd_head.load(std::memory_order_acquire);
            }
          }
          // pdz_native_record_workers 2: the batch's D3D12 calls to the replay thread now (the stream publishes
          // itself every few calls; this hands over the rest before the worker may go idle).
          if (pdz::native::NativeDevice *d = pdz::native::NativeDevice::Existing())
          {
            d->PublishRecording();
          }
        }
        const double batch_ms = QpcMs() - t0;
        s.busy_ms_frame += batch_ms;
        if (split)
        {
          g_split.exec += s_exec;
          g_split.batch += __rdtsc() - s0;
          SplitLog();
        }
        s.total_commands.store(tail, std::memory_order_relaxed);
        s.spike_busy_ns.store(s.spike_busy_ns.load(std::memory_order_relaxed) + uint64_t(batch_ms * 1.0e6),
                              std::memory_order_relaxed);
        for (uint64_t cur = s.spike_longest.load(std::memory_order_relaxed);
             longest > cur && !s.spike_longest.compare_exchange_weak(cur, longest, std::memory_order_relaxed);)
        {
        }
      }
    }

    // The CP_RB_WPTR filter (SDK sdk-16): any thread writing the guest's kick, inside the guest's MMIO store.
    bool KickFilter(uint32_t value)
    {
      if (!g_state || !S().running.load(std::memory_order_acquire))
      {
        return false;
      }
      State &s = S();
      std::lock_guard<std::mutex> lock(s.produce_mutex);
      const EnqueueScope enqueue_time;
      if (!s.running.load(std::memory_order_relaxed))
      {
        return false;
      }
      if (s.completed.load(std::memory_order_acquire) == s.cmd_head_local)
      {
        // Nothing queued: the worker is past every earlier command (and every earlier kick), deliver now.
        if (!rex::graphics::null_gpu::DeliverWritePointer(value))
        {
          return false; // no sink (yet): the SDK delivers it itself
        }
        s.frame.kicks_direct += 1;
        s.total_kicks_direct.fetch_add(1, std::memory_order_relaxed);
        return true;
      }
      Cmd c;
      c.type = Type::kKick;
      c.a[0] = value;
      const Reservation r = Reserve(0);
      Publish(c, r);
      s.frame.kicks_deferred += 1;
      s.total_kicks_deferred.fetch_add(1, std::memory_order_relaxed);
      return true;
    }

    struct Stats
    {
      size_t count = 0;
      double avg = 0.0, p99 = 0.0, max = 0.0;
    };
    Stats Compute(std::vector<double> &v)
    {
      Stats st;
      if (v.empty())
      {
        return st;
      }
      st.count = v.size();
      double sum = 0.0;
      for (const double x : v)
      {
        sum += x;
        st.max = std::max(st.max, x);
      }
      st.avg = sum / double(v.size());
      const size_t k = std::min(v.size() - 1, size_t(std::ceil(0.99 * double(v.size()))) - 1);
      std::nth_element(v.begin(), v.begin() + k, v.end());
      st.p99 = v[k];
      return st;
    }
  } // namespace

  // =================================================================================================================
  // Public entry points
  // =================================================================================================================
  void ChooseAtStartup()
  {
    // Only with the native renderer on: with it off the thread would run for nothing (and a default of true could not
    // be rolled back with one switch: native_default_proposal.md).
    if (!REXCVAR_GET(pdz_native_record_thread) || !Enabled() || g_state)
    {
      return;
    }
    pdz::shadow::LatchRingSize(); // pdz_native_record_ring, before the first record
    g_cmd_count = uint32_t(PowerOfTwoAtLeast(uint64_t(std::max(0, REXCVAR_GET(pdz_native_record_queue))), 1024,
                                           kCmdCountMax));
    g_arena_bytes = PowerOfTwoAtLeast(uint64_t(std::max(0, REXCVAR_GET(pdz_native_record_arena_mb))), 4, 64) << 20;
    g_oversize = g_arena_bytes / 4;
    g_state = new State();
    State &s = S();
    s.cmds.reset(new Cmd[g_cmd_count]);
    s.arena.reset(new uint8_t[g_arena_bytes]);
    s.psamples.reserve(20000);
    s.wsamples.reserve(20000);
    s.work_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    s.done_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    s.running.store(true, std::memory_order_release);
    s.worker = std::thread(WorkerMain);
    s.started = true;
    rex::graphics::null_gpu::SetWritePointerFilter(&KickFilter);
    pdz::timing::SetNativeOffThread(true);
    REXLOG_INFO("pdz_recthread: on (pdz_native_record_thread): the native renderer runs on its own thread; the game "
                "thread queues each hooked D3D call ({} commands, {} MB payload arena, DrawRecords read in place from "
                "the shadow ring's {} slots) and every CP_RB_WPTR kick, which reaches the command processor only "
                "after the work queued before it (sdk-16 write pointer filter installed); run-ahead at most one "
                "frame; pass dump and draw dump refused; verify {}",
                g_cmd_count, g_arena_bytes >> 20, pdz::shadow::RingSlots(),
                REXCVAR_GET(pdz_native_record_thread_verify) ? "on (8 KB constant compare per draw)" : "off");
  }

  bool Active() { return g_state && g_state->running.load(std::memory_order_relaxed); }

  int64_t CurrentSwapUs() { return t_swap_us; }

  void OnDraw(uint8_t *base, Hook hook, Kind kind, uint32_t device, uint32_t caller, uint32_t a0, uint32_t a1,
              uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5, uint32_t a6, uint32_t a7)
  {
    State &s = S();
    std::lock_guard<std::mutex> lock(s.produce_mutex);
    NoteBase(base);
    WaitShadowRoom();
    pdz::shadow::Count(hook);
    DrawRecord &r = pdz::shadow::Record(base, device, kind, caller, a0, a1, a2, a3, a4, 0);
    if (kind == Kind::kDrawIndexedVerticesUP)
    {
      r.a[5] = a5;
      r.a[6] = a6;
      r.a[7] = a7;
    }
    if (!pdz::native::Enabled() || !pdz::shadow::IsGuestPointer(device))
    {
      return;
    }
    const EnqueueScope enqueue_time;
    NoteDevice(device);
    const uint8_t *dev_ptr = pdz::shadow::GuestPtr(base, device);
    // Object copies: shaders, the vertex program, render targets, the index buffer, UP data.
    Plan plan;
    plan.Shaders(r.vertex_shader, r.pixel_shader);
    PlanPending(plan);
    const uint32_t program = pdz::shadow::Load32(base, device + kProgramOffset);
    if (pdz::shadow::IsGuestPointer(program))
    {
      plan.Add(program + 40, 4);
      plan.Add(program + 600, 4);
    }
    for (uint32_t i = 0; i < 4; ++i)
    {
      plan.Surface(r.render_targets[i]);
    }
    plan.Surface(r.depth_stencil);
    if (kind == Kind::kDrawIndexedVertices && r.index_buffer)
    {
      plan.Add(r.index_buffer, 20);
    }
    const uint32_t other_bytes = plan.size;
    uint32_t up_bytes = 0;
    if (kind == Kind::kDrawVerticesUP)
    {
      const uint64_t n = uint64_t(a1) * a3;
      const uint32_t before = plan.size;
      plan.Add(a2, uint32_t(std::min<uint64_t>(n, 0xFFFFFFFFull)));
      up_bytes = plan.size - before;
    }
    else if (kind == Kind::kDrawIndexedVerticesUP)
    {
      const uint32_t before = plan.size;
      // The vertices D3D copies, [min, min + count) (an index i reads vertex i of a6); the indices.
      plan.Add(uint32_t(uint64_t(a6) + uint64_t(a1) * a7), uint32_t(std::min<uint64_t>(uint64_t(a2) * a7, 0xFFFFFFFFull)));
      plan.Add(a4, uint32_t(std::min<uint64_t>(uint64_t(a3) * (a5 == 1 ? 4u : 2u), 0xFFFFFFFFull)));
      up_bytes = plan.size - before;
    }
    const bool full = s.need_full;
    const uint32_t size = ConstSize(&r, full) + kDeviceBitsSize + plan.size;
    const Reservation res = Reserve(size);
    Writer w{res.ptr, res.ptr + size};
    WriteConsts(w, dev_ptr, &r, full);
    s.need_full = false;
    WriteDeviceBits(w, dev_ptr);
    plan.Write(w, base);
    s.frame.up_bytes += up_bytes;
    s.frame.other_bytes += other_bytes + kDeviceBitsSize;
    s.pending_vs = s.pending_ps = 0; // the worker flushes a pending batch at the top of this draw
    Cmd c;
    c.type = Type::kDraw;
    c.device = device;
    c.slot = r.sequence;
    c.payload_bytes = uint32_t(w.p - res.ptr);
    Publish(c, res);
  }

  uint32_t OnBeginVerticesEnter(uint8_t *base, uint32_t device, uint32_t caller, uint32_t a0, uint32_t a1,
                                uint32_t a2)
  {
    State &s = S();
    std::lock_guard<std::mutex> lock(s.produce_mutex);
    NoteBase(base);
    if (pdz::native::Enabled() && pdz::shadow::IsGuestPointer(device))
    {
      // The previous batch (ended without EndVertices) is complete: drawn before this call reserves ring space.
      const EnqueueScope enqueue_time;
      NoteDevice(device);
      Plan plan;
      PlanPending(plan);
      PlanLiveSurfaces(plan, base, device);
      const uint32_t size = ChunkSize(kTargetsBytes) + ChunkSize(kViewportBytes) + plan.size;
      const Reservation res = Reserve(size);
      Writer w{res.ptr, res.ptr + size};
      const uint8_t *dev_ptr = pdz::shadow::GuestPtr(base, device);
      w.Mirror(dev_ptr, kTargetsOffset, kTargetsBytes);
      w.Mirror(dev_ptr, dev::kViewport, kViewportBytes);
      plan.Write(w, base);
      s.frame.other_bytes += size;
      s.pending_vs = s.pending_ps = 0;
      Cmd c;
      c.type = Type::kFlushPending;
      c.device = device;
      c.payload_bytes = uint32_t(w.p - res.ptr);
      Publish(c, res);
    }
    WaitShadowRoom();
    pdz::shadow::Count(Hook::kBeginVertices);
    const DrawRecord &r = pdz::shadow::Record(base, device, Kind::kBeginVertices, caller, a0, a1, a2, 0, 0, 0);
    return r.sequence;
  }

  void OnBeginVerticesLeave(uint8_t *base, uint32_t sequence, uint32_t data)
  {
    State &s = S();
    std::lock_guard<std::mutex> lock(s.produce_mutex);
    DrawRecord &r = pdz::shadow::Get().ring[sequence & pdz::shadow::RingMask()];
    if (r.sequence != sequence)
    {
      return; // cannot happen: the slot is held until the worker is past it
    }
    r.a[3] = data;
    const uint32_t device = r.device;
    if (!pdz::native::Enabled() || !pdz::shadow::IsGuestPointer(device))
    {
      return;
    }
    const EnqueueScope enqueue_time;
    NoteDevice(device);
    const uint8_t *dev_ptr = pdz::shadow::GuestPtr(base, device);
    Plan plan;
    plan.Shaders(r.vertex_shader, r.pixel_shader);
    const uint32_t program = pdz::shadow::Load32(base, device + kProgramOffset);
    if (pdz::shadow::IsGuestPointer(program))
    {
      plan.Add(program + 40, 4);
      plan.Add(program + 600, 4);
    }
    for (uint32_t i = 0; i < 4; ++i)
    {
      plan.Surface(r.render_targets[i]);
    }
    plan.Surface(r.depth_stencil);
    const bool full = s.need_full;
    const uint32_t size = ConstSize(&r, full) + kDeviceBitsSize + plan.size;
    const Reservation res = Reserve(size);
    Writer w{res.ptr, res.ptr + size};
    WriteConsts(w, dev_ptr, &r, full); // the values the call's flush emitted (unchanged since its entry)
    s.need_full = false;
    WriteDeviceBits(w, dev_ptr);
    plan.Write(w, base);
    s.frame.other_bytes += plan.size + kDeviceBitsSize;
    // A batch the worker keeps pending until the next entry point (unless it is DrawVerticesUP's inner one).
    s.pending_vs = r.vertex_shader;
    s.pending_ps = r.pixel_shader;
    Cmd c;
    c.type = Type::kDraw;
    c.device = device;
    c.slot = r.sequence;
    c.payload_bytes = uint32_t(w.p - res.ptr);
    Publish(c, res);
  }

  namespace
  {
    // FlushPending / EndVertices: the live RT / DS, viewport and surface headers, and the pending batch's shaders.
    void QueueFlushPoint(uint8_t *base, uint32_t device, Type type)
    {
      State &s = S();
      const EnqueueScope enqueue_time;
      Plan plan;
      PlanPending(plan);
      const bool device_ok = pdz::shadow::IsGuestPointer(device);
      if (device_ok)
      {
        NoteDevice(device);
        PlanLiveSurfaces(plan, base, device);
      }
      const uint32_t size = (device_ok ? ChunkSize(kTargetsBytes) + ChunkSize(kViewportBytes) : 0) + plan.size;
      const Reservation res = Reserve(size);
      Writer w{res.ptr, res.ptr + size};
      if (device_ok)
      {
        const uint8_t *dev_ptr = pdz::shadow::GuestPtr(base, device);
        w.Mirror(dev_ptr, kTargetsOffset, kTargetsBytes);
        w.Mirror(dev_ptr, dev::kViewport, kViewportBytes);
      }
      plan.Write(w, base);
      s.frame.other_bytes += size;
      s.pending_vs = s.pending_ps = 0;
      Cmd c;
      c.type = type;
      c.device = device_ok ? device : 0;
      c.payload_bytes = uint32_t(w.p - res.ptr);
      Publish(c, res);
    }
  } // namespace

  void OnEndVertices(uint8_t *base, uint32_t device)
  {
    State &s = S();
    std::lock_guard<std::mutex> lock(s.produce_mutex);
    NoteBase(base);
    if (pdz::shadow::Enabled())
    {
      pdz::shadow::Count(Hook::kEndVertices);
    }
    if (pdz::native::Enabled())
    {
      QueueFlushPoint(base, device, Type::kEndVertices);
    }
  }

  void OnFlushPoint(uint8_t *base, uint32_t device)
  {
    State &s = S();
    if (!pdz::native::Enabled())
    {
      return;
    }
    std::lock_guard<std::mutex> lock(s.produce_mutex);
    NoteBase(base);
    QueueFlushPoint(base, device, Type::kFlushPending);
  }

  void OnClear(uint8_t *base, uint32_t device, uint32_t caller, uint32_t count, uint32_t rects, uint32_t flags,
               uint32_t color, uint32_t z_bits, uint32_t stencil)
  {
    State &s = S();
    std::lock_guard<std::mutex> lock(s.produce_mutex);
    NoteBase(base);
    if (pdz::shadow::Enabled())
    {
      WaitShadowRoom();
      pdz::shadow::Count(Hook::kClear);
      pdz::shadow::Record(base, device, Kind::kClear, caller, count, rects, flags, color, stencil, z_bits);
    }
    if (!pdz::native::Enabled())
    {
      return;
    }
    const EnqueueScope enqueue_time;
    const bool device_ok = pdz::shadow::IsGuestPointer(device);
    Plan plan;
    PlanPending(plan);
    if (device_ok)
    {
      NoteDevice(device);
      PlanLiveSurfaces(plan, base, device);
    }
    if (count && pdz::shadow::IsGuestPointer(rects))
    {
      plan.Add(rects, 16 * std::min(count, 16u));
    }
    const uint32_t size = (device_ok ? ChunkSize(kTargetsBytes) + ChunkSize(kViewportBytes) : 0) + plan.size;
    const Reservation res = Reserve(size);
    Writer w{res.ptr, res.ptr + size};
    if (device_ok)
    {
      const uint8_t *dev_ptr = pdz::shadow::GuestPtr(base, device);
      w.Mirror(dev_ptr, kTargetsOffset, kTargetsBytes);
      w.Mirror(dev_ptr, dev::kViewport, kViewportBytes);
    }
    plan.Write(w, base);
    s.frame.other_bytes += size;
    s.pending_vs = s.pending_ps = 0;
    Cmd c;
    c.type = Type::kClear;
    c.device = device_ok ? device : 0;
    c.a[0] = count;
    c.a[1] = rects;
    c.a[2] = flags;
    c.a[3] = color;
    c.a[4] = z_bits;
    c.a[5] = stencil;
    c.payload_bytes = uint32_t(w.p - res.ptr);
    Publish(c, res);
  }

  void OnResolveEnter(uint8_t *base, uint32_t device, uint32_t caller, uint32_t flags, uint32_t src_rect,
                      uint32_t dest_texture, uint32_t dest_point, uint32_t level, uint32_t z_bits)
  {
    State &s = S();
    std::lock_guard<std::mutex> lock(s.produce_mutex);
    NoteBase(base);
    WaitShadowRoom();
    pdz::shadow::Count(Hook::kResolve);
    pdz::shadow::Record(base, device, Kind::kResolve, caller, flags, src_rect, dest_texture, dest_point, level, z_bits);
  }

  void OnResolveLeave(uint8_t *base, uint32_t device, uint32_t flags, uint32_t src_rect, uint32_t dest_texture,
                      uint32_t dest_point, uint32_t level, uint32_t slice, uint32_t clear_color_ptr, uint32_t z_bits,
                      uint32_t call_site)
  {
    State &s = S();
    if (!pdz::native::Enabled())
    {
      return;
    }
    std::lock_guard<std::mutex> lock(s.produce_mutex);
    NoteBase(base);
    const EnqueueScope enqueue_time;
    const bool device_ok = pdz::shadow::IsGuestPointer(device);
    Plan plan;
    PlanPending(plan);
    if (device_ok)
    {
      NoteDevice(device);
      PlanLiveSurfaces(plan, base, device);
    }
    plan.Add(src_rect, 16);
    plan.Add(dest_point, 8);
    plan.Add(clear_color_ptr, 16);
    if (dest_texture)
    {
      plan.Add(dest_texture + 16, 24);
    }
    const uint32_t size =
        (device_ok ? ChunkSize(dev::kRegShadowBytes) + ChunkSize(kTargetsBytes) + ChunkSize(kViewportBytes) : 0) +
        plan.size;
    const Reservation res = Reserve(size);
    Writer w{res.ptr, res.ptr + size};
    if (device_ok)
    {
      // The copy / clear registers D3D wrote inside the call (and the surface info registers the resolve reads).
      const uint8_t *dev_ptr = pdz::shadow::GuestPtr(base, device);
      w.Mirror(dev_ptr, dev::kRegShadowBegin, dev::kRegShadowBytes);
      w.Mirror(dev_ptr, kTargetsOffset, kTargetsBytes);
      w.Mirror(dev_ptr, dev::kViewport, kViewportBytes);
    }
    plan.Write(w, base);
    s.frame.other_bytes += size;
    s.pending_vs = s.pending_ps = 0;
    Cmd c;
    c.type = Type::kResolve;
    c.device = device_ok ? device : 0;
    c.a[0] = flags;
    c.a[1] = src_rect;
    c.a[2] = dest_texture;
    c.a[3] = dest_point;
    c.a[4] = level;
    c.a[5] = slice;
    c.a[6] = clear_color_ptr;
    c.a[7] = z_bits;
    c.a[8] = call_site;
    c.payload_bytes = uint32_t(w.p - res.ptr);
    Publish(c, res);
  }

  void OnSwapEnter(uint8_t *base, uint32_t device, uint32_t caller, uint32_t front_buffer, uint32_t parameters)
  {
    State &s = S();
    std::lock_guard<std::mutex> lock(s.produce_mutex);
    NoteBase(base);
    WaitShadowRoom();
    pdz::shadow::Count(Hook::kSwap);
    pdz::shadow::Record(base, device, Kind::kSwap, caller, front_buffer, parameters, 0, 0, 0, 0);
  }

  void OnSwapLeave(uint8_t *base, uint32_t device, uint32_t front_buffer)
  {
    State &s = S();
    const int64_t now_us = pdz::gpusync::NowUs();
    std::lock_guard<std::mutex> lock(s.produce_mutex);
    NoteBase(base);
    const EnqueueScope enqueue_time;
    const uint64_t index = ++s.swaps_queued;
    // Run-ahead at most one frame: Swap N waits for the worker's Swap N - 1 (its present).
    if (index > 1)
    {
      WaitFor(kWaitSwap, [&] { return s.swaps_done.load(std::memory_order_acquire) >= index - 1; });
    }
    const bool device_ok = pdz::shadow::IsGuestPointer(device);
    Plan plan;
    PlanPending(plan);
    if (front_buffer)
    {
      plan.Add(front_buffer + 16, 24);
    }
    // The whole float blocks once per frame: the mirror starts the next frame from the device as it is now.
    const uint32_t size = (device_ok ? ChunkSize(kFloatBytes) + kDeviceBitsSize : 0) + plan.size;
    const Reservation res = Reserve(size);
    Writer w{res.ptr, res.ptr + size};
    if (device_ok)
    {
      NoteDevice(device);
      const uint8_t *dev_ptr = pdz::shadow::GuestPtr(base, device);
      WriteConsts(w, dev_ptr, nullptr, true);
      s.need_full = false;
      WriteDeviceBits(w, dev_ptr);
    }
    plan.Write(w, base);
    s.frame.other_bytes += plan.size + (device_ok ? kDeviceBitsSize : 0);
    s.pending_vs = s.pending_ps = 0;
    Cmd c;
    c.type = Type::kSwap;
    c.device = device_ok ? device : 0;
    c.a[0] = front_buffer;
    c.time_us = now_us;
    c.swap_index = index;
    c.payload_bytes = uint32_t(w.p - res.ptr);
    Publish(c, res);
    // The frame's producer sample (the counters since the previous Swap).
    {
      std::lock_guard<std::mutex> lk(s.samples_mutex);
      s.psamples.push_back({index, s.frame});
    }
    s.frame = ProducerFrame();
  }

  void OnInsertCallback(uint8_t *base, uint32_t callback)
  {
    State &s = S();
    if (!pdz::native::Enabled())
    {
      return;
    }
    std::lock_guard<std::mutex> lock(s.produce_mutex);
    NoteBase(base);
    const EnqueueScope enqueue_time;
    Cmd c;
    c.type = Type::kCallback;
    c.a[0] = callback;
    Publish(c, Reserve(0));
  }

  void OnGammaRamp(uint8_t *base, uint32_t ramp)
  {
    State &s = S();
    std::lock_guard<std::mutex> lock(s.produce_mutex);
    NoteBase(base);
    const EnqueueScope enqueue_time;
    Plan plan;
    plan.Add(ramp, 3 * 256 * 2);
    const Reservation res = Reserve(plan.size);
    Writer w{res.ptr, res.ptr + plan.size};
    plan.Write(w, base);
    Cmd c;
    c.type = Type::kGamma;
    c.a[0] = ramp;
    c.payload_bytes = uint32_t(w.p - res.ptr);
    Publish(c, res);
  }

  void OnTextureWrite(uint8_t *base, uint32_t begin, uint32_t end, uint32_t mip_begin, uint32_t mip_end)
  {
    State &s = S();
    std::lock_guard<std::mutex> lock(s.produce_mutex);
    NoteBase(base);
    const EnqueueScope enqueue_time;
    Cmd c;
    c.type = Type::kTextureWrite;
    c.a[0] = begin;
    c.a[1] = end;
    c.a[2] = mip_begin;
    c.a[3] = mip_end;
    Publish(c, Reserve(0));
  }

  void Drain(const char *why)
  {
    if (!Active())
    {
      return;
    }
    State &s = S();
    const double t0 = QpcMs();
    {
      std::lock_guard<std::mutex> lock(s.produce_mutex);
      const uint64_t target = s.cmd_head_local;
      const double deadline = t0 + 5000.0;
      while (s.completed.load(std::memory_order_acquire) < target && QpcMs() < deadline)
      {
        Sleep(1);
      }
    }
    REXLOG_INFO("pdz_recthread: drained for {} in {:.1f} ms ({} commands queued, {} done)", why, QpcMs() - t0,
                s.cmd_head.load(std::memory_order_relaxed), s.completed.load(std::memory_order_relaxed));
  }

  void Stop()
  {
    if (!g_state || !S().started)
    {
      return;
    }
    State &s = S();
    {
      std::lock_guard<std::mutex> lock(s.produce_mutex);
      const double deadline = QpcMs() + 2000.0;
      while (s.completed.load(std::memory_order_acquire) < s.cmd_head_local && QpcMs() < deadline)
      {
        Sleep(1);
      }
      // From here every hook runs its command on its own thread (Reserve sees running false) and kicks go straight
      // to the command processor.
      s.running.store(false, std::memory_order_seq_cst);
      rex::graphics::null_gpu::SetWritePointerFilter(nullptr);
      // A no-op command changes cmd_head after `running` is false: a worker about to sleep on the head wakes, does
      // it, finds the queue empty and exits (a bare notify could come before its wait).
      if (s.cmd_head_local - s.completed.load(std::memory_order_acquire) < g_cmd_count)
      {
        Cmd c;
        c.type = Type::kNop;
        c.ring_head = pdz::shadow::Get().head;
        c.arena_end = s.arena_head;
        s.cmds[s.cmd_head_local & (g_cmd_count - 1)] = c;
        ++s.cmd_head_local;
        s.cmd_head.store(s.cmd_head_local, std::memory_order_seq_cst);
      }
      SetEvent(s.work_event);
    }
    if (s.worker.joinable())
    {
      // The worker exits once the queue is empty; do not hang the shutdown on a stuck present.
      const double deadline = QpcMs() + 2000.0;
      while (s.completed.load(std::memory_order_acquire) < s.cmd_head.load(std::memory_order_acquire) &&
             QpcMs() < deadline)
      {
        Sleep(1);
      }
      if (s.completed.load(std::memory_order_acquire) >= s.cmd_head.load(std::memory_order_acquire))
      {
        SetEvent(s.work_event);
        s.worker.join();
      }
      else
      {
        s.worker.detach();
      }
    }
    REXLOG_INFO("pdz_recthread: stopped ({} commands, kicks deferred {} / delivered at once {}, inline {}, lost {})",
                s.completed.load(std::memory_order_relaxed), s.total_kicks_deferred.load(std::memory_order_relaxed),
                s.total_kicks_direct.load(std::memory_order_relaxed), s.total_inline.load(std::memory_order_relaxed),
                s.total_lost.load(std::memory_order_relaxed));
  }

  bool TakeSpikeTotals(SpikeTotals &out)
  {
    if (!g_state)
    {
      return false;
    }
    State &s = S();
    out.busy_ns = s.spike_busy_ns.load(std::memory_order_relaxed);
    out.commands = s.completed.load(std::memory_order_relaxed);
    for (uint32_t k = 0; k < kWaitKinds; ++k)
    {
      out.wait_us[k] = s.spike_wait_us[k].load(std::memory_order_relaxed);
      out.waits[k] = s.spike_waits[k].load(std::memory_order_relaxed);
    }
    out.arena_waits = s.spike_arena_waits.load(std::memory_order_relaxed);
    out.arena_wraps = s.spike_arena_wraps.load(std::memory_order_relaxed);
    out.inline_runs = s.total_inline.load(std::memory_order_relaxed);
    out.payload_bytes = s.spike_payload_bytes.load(std::memory_order_relaxed);
    out.depth_max = s.spike_depth_max.exchange(0, std::memory_order_relaxed);
    const uint64_t longest = s.spike_longest.exchange(0, std::memory_order_relaxed);
    const uint32_t type = uint32_t(longest & 0xFF);
    out.longest_cycles = longest >> 8;
    out.longest_type = longest && type < uint32_t(Type::kCount) ? kTypeNames[type] : "-";
    return true;
  }

  void LogRange(const char *kind, uint64_t first_swap, uint64_t last_swap, double frame_ms_avg)
  {
    if (!g_state)
    {
      return;
    }
    State &s = S();
    std::vector<double> busy, wait_kind[kWaitKinds], wait_all;
    double commands = 0.0, depth_sum = 0.0, depth_n = 0.0, depth_max = 0.0, const_kb = 0.0, up_kb = 0.0,
           other_kb = 0.0, resyncs = 0.0, kd = 0.0, kn = 0.0, inl = 0.0, vd = 0.0, vvs = 0.0, vps = 0.0,
           worker_commands = 0.0;
    size_t pframes = 0;
    {
      // Both lists are appended in Swap order: the window is at their end. Log 299 scanned all of them (55,000+ each
      // by Jungle) under this mutex, which the worker and the producer take every frame.
      std::lock_guard<std::mutex> lk(s.samples_mutex);
      size_t wfrom = s.wsamples.size();
      while (wfrom > 0 && s.wsamples[wfrom - 1].swap >= first_swap)
      {
        --wfrom;
      }
      for (size_t i = wfrom; i < s.wsamples.size(); ++i)
      {
        const WorkerSample &w = s.wsamples[i];
        if (w.swap >= first_swap && w.swap <= last_swap)
        {
          busy.push_back(w.busy_ms);
          worker_commands += double(w.commands);
        }
      }
      size_t pfrom = s.psamples.size();
      while (pfrom > 0 && s.psamples[pfrom - 1].swap >= first_swap)
      {
        --pfrom;
      }
      for (size_t i = pfrom; i < s.psamples.size(); ++i)
      {
        const ProducerSample &p = s.psamples[i];
        if (p.swap < first_swap || p.swap > last_swap)
        {
          continue;
        }
        ++pframes;
        const ProducerFrame &f = p.f;
        double all = 0.0;
        for (uint32_t k = 0; k < kWaitKinds; ++k)
        {
          wait_kind[k].push_back(f.wait_ms[k]);
          all += f.wait_ms[k];
        }
        wait_all.push_back(all);
        commands += double(f.commands);
        depth_sum += double(f.depth_sum);
        depth_n += double(f.depth_samples);
        depth_max = std::max(depth_max, double(f.depth_max));
        const_kb += double(f.const_bytes) / 1024.0;
        up_kb += double(f.up_bytes) / 1024.0;
        other_kb += double(f.other_bytes) / 1024.0;
        resyncs += double(f.full_resyncs);
        kd += double(f.kicks_deferred);
        kn += double(f.kicks_direct);
        inl += double(f.inline_runs);
        vd += double(f.verify_draws);
        vvs += double(f.verify_vs_groups);
        vps += double(f.verify_ps_groups);
      }
    }
    if (!pframes && busy.empty())
    {
      return;
    }
    const double pf = pframes ? double(pframes) : 1.0;
    const Stats b = Compute(busy);
    std::string waits;
    for (uint32_t k = 0; k < kWaitKinds; ++k)
    {
      const Stats st = Compute(wait_kind[k]);
      waits += fmt::format("{}{} {:.3f}/{:.3f}", k ? ", " : "", kWaitNames[k], st.avg, st.p99);
    }
    const Stats wa = Compute(wait_all);
    const std::string verify =
        REXCVAR_GET(pdz_native_record_thread_verify)
            ? fmt::format(" | verify: {:.0f} draws/frame, constant groups differing from the device VS {:.0f} PS {:.0f} "
                          "(run)",
                          vd / pf, vvs, vps)
            : std::string();
    REXLOG_INFO("pdz_timing: record thread ({}) swaps {}-{} | worker busy ms per frame avg {:.3f} p99 {:.3f} max {:.3f} "
                "({:.0f}% of the frame), commands {:.0f}/frame | queue depth at enqueue avg {:.1f} max {:.0f} | game "
                "thread waits ms per frame avg/p99: all {:.3f}/{:.3f} = {} | payload KB per frame: constants {:.1f} "
                "(full blocks {:.2f}/frame), UP data {:.1f}, other {:.1f} | commands queued {:.0f}/frame, inline {:.0f} "
                "(run) | kicks per frame: deferred {:.1f}, at once {:.1f}{} | lost records {} | device state parts not resent "
                "{:.1f}% (pdz_native_record_delta {}, run total)",
                kind, first_swap, last_swap, b.avg, b.p99, b.max,
                frame_ms_avg > 0.0 ? 100.0 * b.avg / frame_ms_avg : 0.0,
                busy.empty() ? 0.0 : worker_commands / double(busy.size()), depth_n > 0.0 ? depth_sum / depth_n : 0.0,
                depth_max, wa.avg, wa.p99, waits, const_kb / pf, resyncs / pf, up_kb / pf, other_kb / pf,
                commands / pf, inl, kd / pf, kn / pf, verify, s.total_lost.load(std::memory_order_relaxed),
                s.bits_sent_total.load(std::memory_order_relaxed)
                    ? 100.0 * double(s.bits_skipped_total.load(std::memory_order_relaxed)) /
                          double(s.bits_sent_total.load(std::memory_order_relaxed))
                    : 0.0,
                REXCVAR_GET(pdz_native_record_delta) ? "on" : "off");
  }
} // namespace pdz::native::recthread
