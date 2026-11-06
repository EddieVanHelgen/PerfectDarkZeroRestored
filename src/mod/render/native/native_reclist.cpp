// Native renderer, parallel recording (3 Nov 2026): the deferred command list and its replay thread
// (native_reclist.h; design in docs/PERF_FOR_EVERYONE.md, "Part B").
//
// Stream: one byte ring (kRingBytes), one producer at a time (RenderMutex() holders), one consumer (the replay thread).
// A record is a 16-byte header {op, payload bytes, sequence, checksum} and its payload padded to 16 bytes; a kWrap
// header sends the reader back to offset 0. Positions are monotonic byte counts. The producer publishes its head
// every kPublishEvery records, at Publish() and at Drain(); the reader publishes its tail every 64 records and when
// it has caught up. The producer waits for room (ring full) and at Drain(); the reader spins briefly and then sleeps
// on an event with a 1 ms bound.
#include "native_reclist.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

#include <windows.h>
#include <intrin.h>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "../cpu_placement.h"
#include "../frame_timing.h"

REXCVAR_DEFINE_INT32(pdz_native_record_workers, 0, "Render",
                     "Native renderer (3 Nov 2026, docs/PERF_FOR_EVERYONE.md): threads that record the frame's D3D12 "
                     "command list. 0 = auto: 2 when the game may use at least 6 physical cores, else 1 (measured: "
                     "faster on 6 cores, slower on 4). 1 = the record thread makes every D3D12 call itself (as "
                     "before). 2 = the record "
                     "thread does all of the renderer's own work and writes each D3D12 call with its arguments into a "
                     "stream; a second thread makes the same calls, in the same order, on the same command list (the "
                     "driver's recording time moves off the record thread). The list is still closed, executed and "
                     "signalled on the record thread after the second thread has made every call, so fences, "
                     "ordering and the picture are unchanged. Read at startup")
    .range(0, 2)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(pdz_native_record_workers_verify, false, "Render",
                    "pdz_native_record_workers 2 diagnostic: every recorded call carries a sequence number and a "
                    "checksum of its arguments; the replay thread checks both before making the call, and the draw "
                    "calls made are compared with the draw calls recorded at every submit. Mismatches are counted in "
                    "the 'pdz_native: replay' line and the first 8 logged (expected 0)");

REXCVAR_DEFINE_BOOL(pdz_native_record_workers_direct, false, "Render",
                    "pdz_native_record_workers 2 test aid: the replay thread is started and RTV / DSV frees are held "
                    "until each submit as with 2, but every call is still made directly by the recording thread");
REXCVAR_DEFINE_BOOL(pdz_native_record_workers_sync, false, "Render",
                    "pdz_native_record_workers 2 test aid: the record thread waits after every recorded call until the "
                    "replay thread has made it (the stream and the second thread as with 2, without the overlap)");

namespace pdz::native
{
  namespace
  {
    constexpr uint64_t kRingBytes = 16ull << 20; // about 5 frames of Outpost (3 MB a frame)
    constexpr uint32_t kPublishEvery = 32;
    constexpr uint32_t kHeader = 16;

    struct Header
    {
      uint16_t op;
      uint16_t pad;
      uint32_t bytes;
      uint32_t sequence;
      uint32_t checksum; // pdz_native_record_workers_verify
    };
    static_assert(sizeof(Header) == kHeader);

    constexpr uint32_t Align16(uint32_t v) { return (v + 15u) & ~15u; }

    uint32_t Checksum(const uint8_t *p, uint32_t bytes)
    {
      uint32_t h = 2166136261u; // FNV-1a
      for (uint32_t i = 0; i < bytes; ++i)
      {
        h = (h ^ p[i]) * 16777619u;
      }
      return h;
    }

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

    struct Replay
    {
      std::unique_ptr<uint8_t[]> ring;
      ID3D12GraphicsCommandList *list = nullptr;
      bool verify = false;
      bool sync = false; // pdz_native_record_workers_sync

      // Producer (RenderMutex holders).
      uint64_t head_local = 0;
      uint32_t sequence = 0;
      // Producer -> reader.
      alignas(64) std::atomic<uint64_t> head{0};
      // Reader -> producer.
      alignas(64) std::atomic<uint64_t> tail{0};
      std::atomic<uint64_t> draws_replayed{0};
      alignas(64) std::atomic<bool> reader_waiting{false};
      std::atomic<bool> running{false};
      HANDLE work_event = nullptr;
      std::thread thread;

      // Reader-side counters (one writer, relaxed).
      alignas(64) std::atomic<uint64_t> busy_ns{0}, calls{0}, bytes{0}, verify_bad{0};
      uint32_t expect_sequence = 0;
      uint32_t verify_logged = 0;

      // Producer-side counters.
      uint64_t drain_ns = 0, drains = 0, room_waits = 0, room_ns = 0, frames = 0, draw_mismatch = 0;
      uint64_t draws_at_drain = 0;
      std::chrono::steady_clock::time_point log_at{};
      uint64_t busy_at = 0, calls_at = 0, bytes_at = 0, drain_at = 0, drains_at = 0, room_at = 0, room_ns_at = 0,
               frames_at = 0;
    };
    Replay *g_replay = nullptr;

    void Execute(Replay &r, uint16_t op, const uint8_t *d)
    {
      ID3D12GraphicsCommandList *l = r.list;
      using Op = RecList::Op;
      switch (Op(op))
      {
      case Op::kDrawInstanced:
      {
        const auto &a = *reinterpret_cast<const reclist::Draw *>(d);
        l->DrawInstanced(a.a, a.b, a.c, a.d);
        r.draws_replayed.store(r.draws_replayed.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        break;
      }
      case Op::kDrawIndexedInstanced:
      {
        const auto &a = *reinterpret_cast<const reclist::DrawIndexed *>(d);
        l->DrawIndexedInstanced(a.a, a.b, a.c, a.d, a.e);
        r.draws_replayed.store(r.draws_replayed.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        break;
      }
      case Op::kDispatch:
      {
        const auto &a = *reinterpret_cast<const reclist::Dispatch3 *>(d);
        l->Dispatch(a.x, a.y, a.z);
        break;
      }
      case Op::kCopyResource:
      {
        const auto &a = *reinterpret_cast<const reclist::Copy *>(d);
        l->CopyResource(a.dst, a.src);
        break;
      }
      case Op::kCopyTextureRegion:
      {
        const auto &a = *reinterpret_cast<const reclist::CopyRegion *>(d);
        l->CopyTextureRegion(&a.dst, a.x, a.y, a.z, &a.src, a.has_box ? &a.box : nullptr);
        break;
      }
      case Op::kIASetPrimitiveTopology:
        l->IASetPrimitiveTopology(*reinterpret_cast<const D3D12_PRIMITIVE_TOPOLOGY *>(d));
        break;
      case Op::kRSSetViewports:
      {
        const auto &h = *reinterpret_cast<const reclist::ItemsHead *>(d);
        l->RSSetViewports(h.count, reinterpret_cast<const D3D12_VIEWPORT *>(d + sizeof(h)));
        break;
      }
      case Op::kRSSetScissorRects:
      {
        const auto &h = *reinterpret_cast<const reclist::ItemsHead *>(d);
        l->RSSetScissorRects(h.count, reinterpret_cast<const D3D12_RECT *>(d + sizeof(h)));
        break;
      }
      case Op::kOMSetBlendFactor:
      {
        const auto &a = *reinterpret_cast<const reclist::BlendFactor *>(d);
        l->OMSetBlendFactor(a.null ? nullptr : a.f);
        break;
      }
      case Op::kOMSetStencilRef:
        l->OMSetStencilRef(*reinterpret_cast<const UINT *>(d));
        break;
      case Op::kSetPipelineState:
        l->SetPipelineState(*reinterpret_cast<ID3D12PipelineState *const *>(d));
        break;
      case Op::kResourceBarrier:
      {
        const auto &h = *reinterpret_cast<const reclist::ItemsHead *>(d);
        l->ResourceBarrier(h.count, reinterpret_cast<const D3D12_RESOURCE_BARRIER *>(d + sizeof(h)));
        break;
      }
      case Op::kSetDescriptorHeaps:
      {
        const auto &h = *reinterpret_cast<const reclist::ItemsHead *>(d);
        l->SetDescriptorHeaps(h.count, reinterpret_cast<ID3D12DescriptorHeap *const *>(d + sizeof(h)));
        break;
      }
      case Op::kSetComputeRootSignature:
        l->SetComputeRootSignature(*reinterpret_cast<ID3D12RootSignature *const *>(d));
        break;
      case Op::kSetGraphicsRootSignature:
        l->SetGraphicsRootSignature(*reinterpret_cast<ID3D12RootSignature *const *>(d));
        break;
      case Op::kSetComputeRootDescriptorTable:
      {
        const auto &a = *reinterpret_cast<const reclist::Table *>(d);
        l->SetComputeRootDescriptorTable(a.index, D3D12_GPU_DESCRIPTOR_HANDLE{a.h});
        break;
      }
      case Op::kSetGraphicsRootDescriptorTable:
      {
        const auto &a = *reinterpret_cast<const reclist::Table *>(d);
        l->SetGraphicsRootDescriptorTable(a.index, D3D12_GPU_DESCRIPTOR_HANDLE{a.h});
        break;
      }
      case Op::kSetComputeRoot32BitConstants:
      {
        const auto &h = *reinterpret_cast<const reclist::ConstHead *>(d);
        l->SetComputeRoot32BitConstants(h.index, h.count, d + sizeof(h), h.offset);
        break;
      }
      case Op::kSetGraphicsRoot32BitConstants:
      {
        const auto &h = *reinterpret_cast<const reclist::ConstHead *>(d);
        l->SetGraphicsRoot32BitConstants(h.index, h.count, d + sizeof(h), h.offset);
        break;
      }
      case Op::kSetGraphicsRootConstantBufferView:
      {
        const auto &a = *reinterpret_cast<const reclist::Cbv *>(d);
        l->SetGraphicsRootConstantBufferView(a.index, a.address);
        break;
      }
      case Op::kIASetIndexBuffer:
      {
        const auto &a = *reinterpret_cast<const reclist::IndexBuffer *>(d);
        l->IASetIndexBuffer(a.null ? nullptr : &a.v);
        break;
      }
      case Op::kIASetVertexBuffers:
      {
        const auto &h = *reinterpret_cast<const reclist::VbHead *>(d);
        l->IASetVertexBuffers(h.start, h.count,
                              h.null ? nullptr : reinterpret_cast<const D3D12_VERTEX_BUFFER_VIEW *>(d + sizeof(h)));
        break;
      }
      case Op::kOMSetRenderTargets:
      {
        const auto &h = *reinterpret_cast<const reclist::RtHead *>(d);
        const auto *rts = reinterpret_cast<const D3D12_CPU_DESCRIPTOR_HANDLE *>(d + sizeof(h));
        l->OMSetRenderTargets(h.count, h.count ? rts : nullptr, BOOL(h.single_range), h.has_ds ? &h.ds : nullptr);
        break;
      }
      case Op::kClearDepthStencilView:
      {
        const auto &h = *reinterpret_cast<const reclist::ClearDepthHead *>(d);
        l->ClearDepthStencilView(h.view, h.flags, h.depth, UINT8(h.stencil), h.rects,
                                 h.rects ? reinterpret_cast<const D3D12_RECT *>(d + sizeof(h)) : nullptr);
        break;
      }
      case Op::kClearRenderTargetView:
      {
        const auto &h = *reinterpret_cast<const reclist::ClearColorHead *>(d);
        l->ClearRenderTargetView(h.view, h.rgba, h.rects,
                                 h.rects ? reinterpret_cast<const D3D12_RECT *>(d + sizeof(h)) : nullptr);
        break;
      }
      case Op::kBeginQuery:
      {
        const auto &a = *reinterpret_cast<const reclist::Query *>(d);
        l->BeginQuery(a.heap, a.type, a.index);
        break;
      }
      case Op::kEndQuery:
      {
        const auto &a = *reinterpret_cast<const reclist::Query *>(d);
        l->EndQuery(a.heap, a.type, a.index);
        break;
      }
      case Op::kResolveQueryData:
      {
        const auto &a = *reinterpret_cast<const reclist::Resolve *>(d);
        l->ResolveQueryData(a.heap, a.type, a.start, a.count, a.dst, a.offset);
        break;
      }
      default:
        break;
      }
    }

    void ReaderMain()
    {
      Replay &r = *g_replay;
      SetThreadDescription(GetCurrentThread(), L"PDZ native replay thread");
      SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
      pdz::cpu::PlaceCriticalThread("native replay thread"); // pdz_cpu_placement: as the record thread
      uint64_t tail = 0;
      uint8_t *ring = r.ring.get();
      for (;;)
      {
        uint64_t head = r.head.load(std::memory_order_acquire);
        if (tail == head)
        {
          if (!r.running.load(std::memory_order_acquire))
          {
            return;
          }
          bool got = false;
          for (uint32_t spin = 0; spin < 4000; ++spin)
          {
            _mm_pause();
            if (r.head.load(std::memory_order_acquire) != tail)
            {
              got = true;
              break;
            }
          }
          if (!got)
          {
            r.reader_waiting.store(true, std::memory_order_seq_cst);
            if (r.head.load(std::memory_order_seq_cst) == tail && r.running.load(std::memory_order_seq_cst))
            {
              WaitForSingleObject(r.work_event, 1);
            }
            r.reader_waiting.store(false, std::memory_order_relaxed);
          }
          continue;
        }
        const double t0 = QpcMs();
        uint64_t calls = 0, bytes = 0;
        while (tail != head)
        {
          const uint64_t pos = tail & (kRingBytes - 1);
          Header h;
          std::memcpy(&h, ring + pos, sizeof(h));
          if (h.op == uint16_t(RecList::Op::kWrap))
          {
            tail += kRingBytes - pos;
            continue;
          }
          const uint8_t *data = ring + pos + kHeader;
          if (r.verify)
          {
            const bool bad_seq = h.sequence != r.expect_sequence;
            const bool bad_sum = Checksum(data, h.bytes) != h.checksum;
            r.expect_sequence = h.sequence + 1;
            if (bad_seq || bad_sum || h.op >= uint16_t(RecList::Op::kCount))
            {
              r.verify_bad.fetch_add(1, std::memory_order_relaxed);
              if (r.verify_logged < 8)
              {
                ++r.verify_logged;
                REXLOG_ERROR("pdz_native: replay verify: record {} (op {}, {} bytes) {}{}", h.sequence, h.op, h.bytes,
                             bad_seq ? "out of sequence " : "", bad_sum ? "checksum differs" : "");
              }
            }
          }
          Execute(r, h.op, data);
          const uint64_t size = kHeader + Align16(h.bytes);
          tail += size;
          bytes += size;
          if ((++calls & 63) == 0)
          {
            _mm_sfence(); // see below
            r.tail.store(tail, std::memory_order_release);
            head = r.head.load(std::memory_order_acquire);
          }
        }
        // The driver writes the command list's memory as this thread records, and that memory is write-combined:
        // those stores can sit in this core's write-combining buffers after the release store below, which orders
        // only ordinary stores. The record thread closes and executes the list as soon as it sees the tail, so
        // without this fence the GPU could read commands this thread had not yet made visible (3 Nov: a GPU hang,
        // DEVICE_HUNG, at the Datacore gate's first capture: a copy recorded here, closed, executed and waited
        // for at once; gone with this fence, and with the calls made on the record thread itself).
        _mm_sfence();
        r.tail.store(tail, std::memory_order_release);
        r.calls.store(r.calls.load(std::memory_order_relaxed) + calls, std::memory_order_relaxed);
        r.bytes.store(r.bytes.load(std::memory_order_relaxed) + bytes, std::memory_order_relaxed);
        r.busy_ns.store(r.busy_ns.load(std::memory_order_relaxed) + uint64_t((QpcMs() - t0) * 1.0e6),
                        std::memory_order_relaxed);
      }
    }

    // Physical cores with at least one logical processor in the process's affinity (processor group 0, where the
    // game runs). 0 if unknown.
    uint32_t PhysicalCoresAvailable()
    {
      DWORD_PTR process_mask = 0, system_mask = 0;
      if (!GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask))
      {
        return 0;
      }
      DWORD length = 0;
      GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &length);
      if (!length)
      {
        return 0;
      }
      std::unique_ptr<uint8_t[]> buffer(new uint8_t[length]);
      auto *info = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *>(buffer.get());
      if (!GetLogicalProcessorInformationEx(RelationProcessorCore, info, &length))
      {
        return 0;
      }
      uint32_t cores = 0;
      for (DWORD at = 0; at < length;)
      {
        auto *e = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *>(buffer.get() + at);
        if (e->Relationship == RelationProcessorCore)
        {
          for (WORD g = 0; g < e->Processor.GroupCount; ++g)
          {
            const GROUP_AFFINITY &ga = e->Processor.GroupMask[g];
            if (ga.Group == 0 && (ga.Mask & process_mask))
            {
              ++cores;
              break;
            }
          }
        }
        if (!e->Size)
        {
          break;
        }
        at += e->Size;
      }
      return cores;
    }

    void Wake(Replay &r)
    {
      std::atomic_thread_fence(std::memory_order_seq_cst);
      if (r.reader_waiting.load(std::memory_order_relaxed))
      {
        SetEvent(r.work_event);
      }
    }
  } // namespace

  void RecList::StartIfWanted(uint64_t frame)
  {
    if (start_checked_)
    {
      return;
    }
    int32_t workers = REXCVAR_GET(pdz_native_record_workers);
    // Auto decides at frame 600 (a few seconds in, the front end): the core count is read from the process
    // affinity, which a test harness (tools/perf/cpu_modes.ps1) applies after the launch; the frames before use 1.
    if (workers == 0 && frame < 600)
    {
      return;
    }
    start_checked_ = true;
    if (!real_)
    {
      return;
    }
    if (workers == 0)
    {
      // Auto (3 Nov, Outpost): 6 E-cores 10.1 -> 9.5 ms, 4 E-cores 10.1 -> 11.5 ms (the extra thread competes with
      // the game's own on 4 cores).
      const uint32_t cores = PhysicalCoresAvailable();
      workers = cores >= 6 ? 2 : 1;
      REXLOG_INFO("pdz_native: pdz_native_record_workers auto: {} physical cores available -> {} recording thread{}",
                  cores, workers, workers > 1 ? "s" : "");
    }
    if (workers < 2)
    {
      return;
    }
    g_replay = new Replay();
    Replay &r = *g_replay;
    r.ring.reset(new uint8_t[kRingBytes]);
    r.list = real_;
    r.verify = REXCVAR_GET(pdz_native_record_workers_verify);
    r.sync = REXCVAR_GET(pdz_native_record_workers_sync);
    r.work_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    r.running.store(true, std::memory_order_release);
    r.thread = std::thread(ReaderMain);
    deferred_ = true;
    direct_test_ = REXCVAR_GET(pdz_native_record_workers_direct);
    defer_ = open_ && !direct_test_;
    REXLOG_INFO("pdz_native: replay thread on (pdz_native_record_workers 2): the record thread writes each D3D12 call "
                "into a {} MB stream and a second thread makes the calls on the frame's command list, in order; the "
                "list is closed and executed after the second thread caught up; verify {}",
                kRingBytes >> 20, r.verify ? "on (sequence + checksum per call, draw counts per submit)" : "off");
  }

  uint8_t *RecList::Begin(Op op, uint32_t bytes)
  {
    Replay &r = *g_replay;
    const uint64_t size = kHeader + Align16(bytes);
    const uint64_t pos = r.head_local & (kRingBytes - 1);
    const uint64_t skip = pos + size > kRingBytes ? kRingBytes - pos : 0;
    const uint64_t need_end = r.head_local + skip + size;
    if (need_end - r.tail.load(std::memory_order_acquire) > kRingBytes)
    {
      // Ring full: let the reader see everything and wait for room.
      const double t0 = QpcMs();
      r.head.store(r.head_local, std::memory_order_release);
      unpublished_ = 0;
      SetEvent(r.work_event);
      while (need_end - r.tail.load(std::memory_order_acquire) > kRingBytes)
      {
        _mm_pause();
      }
      ++r.room_waits;
      r.room_ns += uint64_t((QpcMs() - t0) * 1.0e6);
    }
    uint8_t *ring = r.ring.get();
    if (skip)
    {
      Header w{uint16_t(Op::kWrap), 0, 0, 0, 0};
      std::memcpy(ring + pos, &w, sizeof(w));
      r.head_local += skip;
    }
    const uint64_t at = r.head_local & (kRingBytes - 1);
    Header h{uint16_t(op), 0, bytes, r.sequence++, 0};
    std::memcpy(ring + at, &h, sizeof(h));
    pending_end_ = r.head_local + size;
    return ring + at + kHeader;
  }

  void RecList::End()
  {
    Replay &r = *g_replay;
    if (r.verify)
    {
      uint8_t *p = r.ring.get() + (r.head_local & (kRingBytes - 1));
      Header h;
      std::memcpy(&h, p, sizeof(h));
      h.checksum = Checksum(p + kHeader, h.bytes);
      std::memcpy(p, &h, sizeof(h));
    }
    r.head_local = pending_end_;
    if (r.sync)
    {
      r.head.store(r.head_local, std::memory_order_release);
      Wake(r);
      while (r.tail.load(std::memory_order_acquire) != r.head_local)
      {
        _mm_pause();
      }
      return;
    }
    if (++unpublished_ >= kPublishEvery)
    {
      unpublished_ = 0;
      r.head.store(r.head_local, std::memory_order_release);
      Wake(r);
    }
  }

  void RecList::PutArray(Op op, UINT count, const void *items, uint32_t item_bytes)
  {
    const uint32_t n = items ? count : 0;
    uint8_t *p = Begin(op, uint32_t(sizeof(reclist::ItemsHead)) + n * item_bytes);
    reclist::ItemsHead h{n, 0};
    std::memcpy(p, &h, sizeof(h));
    if (n)
    {
      std::memcpy(p + sizeof(h), items, size_t(n) * item_bytes);
    }
    End();
  }

  void RecList::PutConstants(Op op, UINT index, UINT count, const void *data, UINT offset)
  {
    uint8_t *p = Begin(op, uint32_t(sizeof(reclist::ConstHead)) + count * 4u);
    reclist::ConstHead h{index, count, offset, 0};
    std::memcpy(p, &h, sizeof(h));
    std::memcpy(p + sizeof(h), data, size_t(count) * 4u);
    End();
  }

  void RecList::PutVertexBuffers(UINT start, UINT count, const D3D12_VERTEX_BUFFER_VIEW *views)
  {
    const UINT n = views ? count : 0;
    uint8_t *p = Begin(Op::kIASetVertexBuffers,
                       uint32_t(sizeof(reclist::VbHead)) + n * uint32_t(sizeof(D3D12_VERTEX_BUFFER_VIEW)));
    // A null array with a count unbinds `count` slots: the count is kept with no items (the replay passes nullptr).
    reclist::VbHead h{start, count, views ? 0u : 1u, 0};
    std::memcpy(p, &h, sizeof(h));
    if (n)
    {
      std::memcpy(p + sizeof(h), views, size_t(n) * sizeof(D3D12_VERTEX_BUFFER_VIEW));
    }
    End();
  }

  void RecList::PutRenderTargets(UINT count, const D3D12_CPU_DESCRIPTOR_HANDLE *rts, BOOL single_range,
                                 const D3D12_CPU_DESCRIPTOR_HANDLE *ds)
  {
    // single_range: one handle names `count` consecutive descriptors.
    const UINT items = rts ? (single_range ? (count ? 1u : 0u) : count) : 0u;
    uint8_t *p = Begin(Op::kOMSetRenderTargets,
                       uint32_t(sizeof(reclist::RtHead)) + items * uint32_t(sizeof(D3D12_CPU_DESCRIPTOR_HANDLE)));
    reclist::RtHead h{rts ? count : 0u, single_range ? 1u : 0u, ds ? 1u : 0u, 0, ds ? *ds : D3D12_CPU_DESCRIPTOR_HANDLE{}};
    std::memcpy(p, &h, sizeof(h));
    if (items)
    {
      std::memcpy(p + sizeof(h), rts, size_t(items) * sizeof(D3D12_CPU_DESCRIPTOR_HANDLE));
    }
    End();
  }

  void RecList::PutClearDepth(D3D12_CPU_DESCRIPTOR_HANDLE view, D3D12_CLEAR_FLAGS flags, FLOAT depth, UINT8 stencil,
                              UINT rect_count, const D3D12_RECT *rects)
  {
    const UINT n = rects ? rect_count : 0;
    uint8_t *p = Begin(Op::kClearDepthStencilView,
                       uint32_t(sizeof(reclist::ClearDepthHead)) + n * uint32_t(sizeof(D3D12_RECT)));
    reclist::ClearDepthHead h{view, flags, depth, stencil, n};
    std::memcpy(p, &h, sizeof(h));
    if (n)
    {
      std::memcpy(p + sizeof(h), rects, size_t(n) * sizeof(D3D12_RECT));
    }
    End();
  }

  void RecList::PutClearColor(D3D12_CPU_DESCRIPTOR_HANDLE view, const FLOAT rgba[4], UINT rect_count,
                              const D3D12_RECT *rects)
  {
    const UINT n = rects ? rect_count : 0;
    uint8_t *p = Begin(Op::kClearRenderTargetView,
                       uint32_t(sizeof(reclist::ClearColorHead)) + n * uint32_t(sizeof(D3D12_RECT)));
    reclist::ClearColorHead h{};
    h.view = view;
    std::memcpy(h.rgba, rgba, sizeof(h.rgba));
    h.rects = n;
    std::memcpy(p, &h, sizeof(h));
    if (n)
    {
      std::memcpy(p + sizeof(h), rects, size_t(n) * sizeof(D3D12_RECT));
    }
    End();
  }

  void RecList::Publish()
  {
    if (!deferred_ || !unpublished_)
    {
      return;
    }
    Replay &r = *g_replay;
    unpublished_ = 0;
    r.head.store(r.head_local, std::memory_order_release);
    Wake(r);
  }

  void RecList::Drain()
  {
    if (!deferred_)
    {
      return;
    }
    Replay &r = *g_replay;
    unpublished_ = 0;
    r.head.store(r.head_local, std::memory_order_release);
    ++r.drains;
    if (r.tail.load(std::memory_order_acquire) != r.head_local)
    {
      const double t0 = QpcMs();
      SetEvent(r.work_event);
      for (uint32_t spin = 0; r.tail.load(std::memory_order_acquire) != r.head_local; ++spin)
      {
        _mm_pause();
        if (spin > 200000)
        {
          SwitchToThread();
        }
      }
      r.drain_ns += uint64_t((QpcMs() - t0) * 1.0e6);
    }
    // The reader made every call: its draw count is final for what was recorded.
    const uint64_t replayed = r.draws_replayed.load(std::memory_order_acquire);
    if (r.verify && replayed != draws_recorded_)
    {
      if (r.draw_mismatch++ < 8)
      {
        REXLOG_ERROR("pdz_native: replay verify: {} draw calls recorded, {} made", draws_recorded_, replayed);
      }
    }
    ++r.frames;
    // One line every 10 s: the replay thread's busy time and the record thread's waits, per drain.
    const auto now = std::chrono::steady_clock::now();
    if (r.log_at == std::chrono::steady_clock::time_point{})
    {
      r.log_at = now;
    }
    else if (now - r.log_at >= std::chrono::seconds(10))
    {
      const double secs = std::chrono::duration<double>(now - r.log_at).count();
      const uint64_t busy = r.busy_ns.load(std::memory_order_relaxed), calls = r.calls.load(std::memory_order_relaxed),
                     bytes = r.bytes.load(std::memory_order_relaxed);
      const double n = double(std::max<uint64_t>(1, r.drains - r.drains_at));
      const double per_s = 1.0 / secs;
      REXLOG_INFO("pdz_native: replay (pdz_native_record_workers 2, {:.1f} s, {:.0f} submits/s): replay thread busy "
                  "{:.3f} ms per submit, {:.0f} calls and {:.0f} KB per submit; record thread waited {:.3f} ms per "
                  "submit for it to catch up, ring full {} times ({:.2f} ms); verify {} bad records, {} draw count "
                  "mismatches; calls on the closed list (direct, as before) {} so far",
                  secs, n * per_s, double(busy - r.busy_at) / 1.0e6 / n, double(calls - r.calls_at) / n,
                  double(bytes - r.bytes_at) / 1024.0 / n, double(r.drain_ns - r.drain_at) / 1.0e6 / n,
                  r.room_waits - r.room_at, double(r.room_ns - r.room_ns_at) / 1.0e6,
                  r.verify_bad.load(std::memory_order_relaxed), r.draw_mismatch, closed_calls_);
      r.log_at = now;
      r.busy_at = busy;
      r.calls_at = calls;
      r.bytes_at = bytes;
      r.drain_at = r.drain_ns;
      r.drains_at = r.drains;
      r.room_at = r.room_waits;
      r.room_ns_at = r.room_ns;
    }
  }

  void RecList::Stop()
  {
    if (!deferred_)
    {
      return;
    }
    Drain();
    Replay &r = *g_replay;
    r.running.store(false, std::memory_order_seq_cst);
    SetEvent(r.work_event);
    if (r.thread.joinable())
    {
      r.thread.join();
    }
    deferred_ = false;
    defer_ = false;
  }
} // namespace pdz::native
