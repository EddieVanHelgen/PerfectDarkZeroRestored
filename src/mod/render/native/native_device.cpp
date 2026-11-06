// Native renderer (stage 1): the D3D12 device layer (native_device.h). The device and the direct queue are
// the SDK's (rex::ui::d3d12::D3D12Provider reached through the runtime's graphics system); everything else
// (frames in flight, fences, upload memory, descriptor heaps, deferred release) is ours.
//
// Thread model: every method is called from inside the D3D hooks with RenderMutex() held (native.h). The game
// hands its D3D device between threads (main thread at boot and around loads, a frontend render thread, the
// in-game render thread), so the hooks are not always on one thread; the mutex makes every use of our command
// list and resources serial. The SDK's presenter and the Xenia plugin submit to the same queue from other
// threads; D3D12 queues are thread-safe, and our fences only track our own submissions.
#include "native_device.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <iterator>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/ui/d3d12/d3d12_provider.h>
#include <rex/ui/d3d12/d3d12_util.h>

#include "native.h"
#include "native_async_upload.h"
#include "native_watchdog.h"
#include "../frame_timing.h"
#include <rex/ui/flags.h> // d3d12_debug

REXCVAR_DEFINE_BOOL(pdz_native_upload_shrink, true, "Render",
                    "Native renderer: a frame slot's upload buffer that grew for a burst (level load, video) goes back "
                    "down once the slot's frames used under a quarter of it for 600 frames (about 30 s at 60 Hz): it "
                    "is replaced at BeginFrame by the initial 64 MB, or the smallest doubling of it that holds twice "
                    "that window's largest frame (REVIEW2.md M1). Off = a grown buffer is kept for the session.");
REXCVAR_DEFINE_INT32(pdz_native_sampler_region_limit, 0, "Render",
                     "Native renderer test aid: use only the first N descriptors of the persistent sampler region "
                     "(1504) for the content-cached sampler tables, so a short run fills it and exercises "
                     "pdz_native_sampler_evict (one level holds about 600: Datacore, log 173). 0 = the whole region.");
REXCVAR_DEFINE_BOOL(pdz_native_gpu_passes, false, "Render",
                    "Native renderer measurement (performance pass): a GPU timestamp at each pass boundary of the frame "
                    "(new render targets, a clear, a resolve, the present chain), the time to the next boundary "
                    "charged to that pass's kind (shadow-map draws / clears / resolves, main-scene draws, other "
                    "targets, other clears / resolves, present). Logs a 'pdz_native: gpu passes' line every 600 "
                    "frames: GPU ms per frame and passes per frame of each kind. Off = no extra timestamps.");

namespace pdz::native
{
  struct NativeDevice::Impl
  {
    static constexpr uint32_t kFrames = 3;
    // Shader-visible heaps: a ring each, shared by the frames in flight (a frame may use more than a third when the
    // others use less). The heap is one full root-signature table larger than the ring so a compact table that
    // starts near the end still has its whole declared range (128 SRVs = kSrvTableSize / 32 samplers) inside the heap.
    static constexpr uint32_t kSrvRingSize = 3 * 65536;   // 196,608 (log 023/028: 21,845 per frame ran out)
    static constexpr uint32_t kSrvHeapSize = kSrvRingSize + 128;
    static constexpr uint32_t kSamplerHeapSize = 2048;     // the D3D12 limit for a shader-visible sampler heap
    // Sampler heap: [0, ring) per-frame ring, [ring, ring + persistent) content-cached tables shared by all frames,
    // then one table of padding.
    static constexpr uint32_t kSamplerRingSize = 512;
    static constexpr uint32_t kSamplerPersistentSize = kSamplerHeapSize - kSamplerRingSize - 32;
    // The persistent region's free ranges (offsets into the region, sorted by start, adjacent ranges merged), first
    // fit. Until a table is freed this is one range [high, size): the bump allocation the region always had. The
    // draw path's sampler-table cache frees the tables it evicts (pdz_native_sampler_evict, native_draw.cpp).
    struct SamplerRange
    {
      uint32_t start = 0, count = 0;
    };
    std::vector<SamplerRange> sampler_free{SamplerRange{0, kSamplerPersistentSize}};
    uint32_t sampler_persistent_live = 0;    // descriptors in allocated tables
    uint32_t sampler_persistent_high = 0;    // highest offset ever allocated (the old bump index)
    uint64_t sampler_persistent_refused = 0; // allocations that found no free range (the caller uses the ring)
    uint32_t sampler_bad_frees = 0;
    bool sampler_persistent_full_logged = false;
    static constexpr uint32_t kStagingHeapSize = 16384;
    static constexpr uint32_t kRtvHeapSize = 512;
    static constexpr uint32_t kDsvHeapSize = 256;
    static constexpr uint64_t kUploadInitialBytes = 64ull << 20;
    // pdz_native_upload_shrink: frames of one slot (every third frame) per shrink decision.
    static constexpr uint32_t kShrinkWindowFrames = 600;

    struct Frame
    {
      ID3D12CommandAllocator *allocator = nullptr;
      uint64_t fence_value = 0; // the value signalled when this slot's last submission completes
      ID3D12Resource *upload = nullptr;
      uint8_t *upload_cpu = nullptr;
      uint64_t upload_size = 0;
      uint64_t upload_offset = 0;
      // Upload use of the open frame in buffers a growth replaced (the frame's use = carry + upload_offset), and the
      // shrink window: the slot's largest frame and its frame count since the last decision.
      uint64_t upload_carry = 0;
      uint64_t window_peak = 0;
      uint32_t window_frames = 0;
    };
    Frame frames[kFrames];
    uint32_t slot = 0;
    // Memory log counters (GetMemoryStats).
    uint64_t upload_grows = 0, upload_shrinks = 0;
    uint64_t upload_frame_peak = 0; // since the last GetMemoryStats
    uint32_t shrink_logged = 0;

    ID3D12Fence *fence = nullptr;
    HANDLE fence_event = nullptr;
    uint64_t next_fence_value = 1;

    // pdz_frame_timing (frame_timing.h "native gpu ms"): GPU timestamps around each command list of a frame (BeginFrame
    // and Flush open one, Flush and EndFrame close one), resolved into a readback buffer and summed when the frame's
    // slot comes round again (its fence has passed by then). Made at the first frame with the timing on. A frame
    // measures at most kTimestampLists lists (each de-rez / auto-diff readback Flush splits a frame).
    static constexpr uint32_t kTimestampLists = 8;
    ID3D12QueryHeap *ts_heap = nullptr;
    ID3D12Resource *ts_readback = nullptr;
    uint64_t ts_frequency = 0;
    bool ts_failed = false;
    bool ts_open = false;            // a begin timestamp is in the open list
    uint32_t ts_lists[kFrames] = {}; // lists of the slot's current frame closed with timestamps

    bool EnsureTimestamps(ID3D12Device *device, ID3D12CommandQueue *queue)
    {
      if (ts_heap)
      {
        return true;
      }
      if (ts_failed)
      {
        return false;
      }
      ts_failed = true;
      if (FAILED(queue->GetTimestampFrequency(&ts_frequency)) || !ts_frequency)
      {
        REXLOG_WARN("pdz_native: the direct queue has no GPU timestamp frequency: no native gpu ms (pdz_frame_timing)");
        return false;
      }
      D3D12_QUERY_HEAP_DESC heap_desc{};
      heap_desc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
      heap_desc.Count = kFrames * kTimestampLists * 2;
      if (FAILED(device->CreateQueryHeap(&heap_desc, IID_PPV_ARGS(&ts_heap))))
      {
        ts_heap = nullptr;
        REXLOG_WARN("pdz_native: no GPU timestamp query heap: no native gpu ms (pdz_frame_timing)");
        return false;
      }
      D3D12_HEAP_PROPERTIES heap{};
      heap.Type = D3D12_HEAP_TYPE_READBACK;
      D3D12_RESOURCE_DESC desc{};
      desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
      desc.Width = uint64_t(heap_desc.Count) * sizeof(uint64_t);
      desc.Height = 1;
      desc.DepthOrArraySize = 1;
      desc.MipLevels = 1;
      desc.SampleDesc.Count = 1;
      desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
      if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                                 nullptr, IID_PPV_ARGS(&ts_readback))))
      {
        ts_readback = nullptr;
        ts_heap->Release();
        ts_heap = nullptr;
        REXLOG_WARN("pdz_native: no GPU timestamp readback buffer: no native gpu ms (pdz_frame_timing)");
        return false;
      }
      ts_failed = false;
      REXLOG_INFO("pdz_native: GPU timestamps around the native command lists for pdz_frame_timing ({} Hz)",
                  ts_frequency);
      return true;
    }

    // After the list was (re)opened.
    void TimestampBegin(RecList *list)
    {
      ts_open = false;
      if (!ts_heap || ts_lists[slot] >= kTimestampLists)
      {
        return;
      }
      list->EndQuery(ts_heap, D3D12_QUERY_TYPE_TIMESTAMP, (slot * kTimestampLists + ts_lists[slot]) * 2);
      ts_open = true;
    }

    // Before the list is closed.
    void TimestampEnd(RecList *list)
    {
      if (!ts_open)
      {
        return;
      }
      ts_open = false;
      const uint32_t index = (slot * kTimestampLists + ts_lists[slot]) * 2;
      list->EndQuery(ts_heap, D3D12_QUERY_TYPE_TIMESTAMP, index + 1);
      list->ResolveQueryData(ts_heap, D3D12_QUERY_TYPE_TIMESTAMP, index, 2, ts_readback, uint64_t(index) * 8);
      ++ts_lists[slot];
    }

    // BeginFrame, after the slot's fence wait: the GPU time of the slot's previous frame (the sum over its lists).
    void TimestampCollect()
    {
      const uint32_t lists = ts_lists[slot];
      ts_lists[slot] = 0;
      if (!lists || !ts_readback)
      {
        return;
      }
      const size_t first = size_t(slot) * kTimestampLists * 2;
      D3D12_RANGE range{first * sizeof(uint64_t), (first + size_t(lists) * 2) * sizeof(uint64_t)};
      void *mapped = nullptr;
      if (FAILED(ts_readback->Map(0, &range, &mapped)) || !mapped)
      {
        return;
      }
      const uint64_t *t = static_cast<const uint64_t *>(mapped) + first;
      uint64_t ticks = 0;
      for (uint32_t i = 0; i < lists; ++i)
      {
        ticks += t[2 * i + 1] > t[2 * i] ? t[2 * i + 1] - t[2 * i] : 0;
      }
      D3D12_RANGE none{0, 0};
      ts_readback->Unmap(0, &none);
      pdz::timing::AddNativeGpuUs(uint64_t(double(ticks) * 1000000.0 / double(ts_frequency)));
    }

    // pdz_native_gpu_passes: per-slot marks (a timestamp and the kind of the pass it starts); a list's close writes
    // an end mark, so no interval crosses lists. Resolved into the readback at each list close, summed at the slot's
    // next BeginFrame (after its fence).
    static constexpr uint32_t kPassMarks = 4096;
    static constexpr uint8_t kPassEnd = 0xFF;
    static constexpr uint32_t kPassKinds = uint32_t(GpuPass::kCount);
    ID3D12QueryHeap *pass_heap = nullptr;
    ID3D12Resource *pass_readback = nullptr;
    uint64_t pass_frequency = 0;
    bool pass_failed = false;
    uint32_t pass_count[kFrames] = {};      // marks written in the slot's current frame
    uint32_t pass_list_first[kFrames] = {}; // the open list's first mark
    uint8_t pass_kind[kFrames][kPassMarks] = {};
    uint64_t pass_overflow = 0;
    // Window totals (kWindow collected frames).
    double pass_ms[kPassKinds] = {};
    uint64_t pass_marks[kPassKinds] = {};
    uint32_t pass_frames = 0;

    bool EnsurePasses(ID3D12Device *device, ID3D12CommandQueue *queue)
    {
      if (pass_heap)
      {
        return true;
      }
      if (pass_failed)
      {
        return false;
      }
      pass_failed = true;
      if (FAILED(queue->GetTimestampFrequency(&pass_frequency)) || !pass_frequency)
      {
        return false;
      }
      D3D12_QUERY_HEAP_DESC heap_desc{};
      heap_desc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
      heap_desc.Count = kFrames * kPassMarks;
      if (FAILED(device->CreateQueryHeap(&heap_desc, IID_PPV_ARGS(&pass_heap))))
      {
        pass_heap = nullptr;
        return false;
      }
      D3D12_HEAP_PROPERTIES heap{};
      heap.Type = D3D12_HEAP_TYPE_READBACK;
      D3D12_RESOURCE_DESC desc{};
      desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
      desc.Width = uint64_t(heap_desc.Count) * sizeof(uint64_t);
      desc.Height = 1;
      desc.DepthOrArraySize = 1;
      desc.MipLevels = 1;
      desc.SampleDesc.Count = 1;
      desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
      if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                                 nullptr, IID_PPV_ARGS(&pass_readback))))
      {
        pass_readback = nullptr;
        pass_heap->Release();
        pass_heap = nullptr;
        return false;
      }
      pass_failed = false;
      REXLOG_INFO("pdz_native: GPU pass timestamps on (pdz_native_gpu_passes, {} Hz)", pass_frequency);
      return true;
    }

    void PassMark(RecList *list, uint8_t kind)
    {
      uint32_t &n = pass_count[slot];
      if (n >= kPassMarks - 1) // keep one for the list's end mark
      {
        ++pass_overflow;
        return;
      }
      list->EndQuery(pass_heap, D3D12_QUERY_TYPE_TIMESTAMP, slot * kPassMarks + n);
      pass_kind[slot][n++] = kind;
    }

    // After the list was (re)opened: the list's first pass is "other targets" until a bind says otherwise.
    void PassListBegin(RecList *list)
    {
      pass_list_first[slot] = pass_count[slot];
      if (pass_heap)
      {
        PassMark(list, uint8_t(GpuPass::kOtherDraw));
      }
    }

    // Before the list is closed.
    void PassListEnd(RecList *list)
    {
      if (!pass_heap)
      {
        return;
      }
      uint32_t &n = pass_count[slot];
      const uint32_t first = pass_list_first[slot];
      if (n <= first || n >= kPassMarks)
      {
        return;
      }
      list->EndQuery(pass_heap, D3D12_QUERY_TYPE_TIMESTAMP, slot * kPassMarks + n);
      pass_kind[slot][n++] = kPassEnd;
      list->ResolveQueryData(pass_heap, D3D12_QUERY_TYPE_TIMESTAMP, slot * kPassMarks + first, n - first,
                             pass_readback, uint64_t(slot * kPassMarks + first) * 8);
      pass_list_first[slot] = n;
    }

    // BeginFrame, after the slot's fence wait.
    void PassCollect()
    {
      const uint32_t n = pass_count[slot];
      pass_count[slot] = 0;
      pass_list_first[slot] = 0;
      if (!n || !pass_readback)
      {
        return;
      }
      const size_t first = size_t(slot) * kPassMarks;
      D3D12_RANGE range{first * sizeof(uint64_t), (first + n) * sizeof(uint64_t)};
      void *mapped = nullptr;
      if (FAILED(pass_readback->Map(0, &range, &mapped)) || !mapped)
      {
        return;
      }
      const uint64_t *t = static_cast<const uint64_t *>(mapped) + first;
      const double ms_per_tick = 1000.0 / double(pass_frequency);
      for (uint32_t i = 0; i + 1 < n; ++i)
      {
        const uint8_t kind = pass_kind[slot][i];
        if (kind == kPassEnd)
        {
          continue;
        }
        pass_ms[kind] += t[i + 1] > t[i] ? double(t[i + 1] - t[i]) * ms_per_tick : 0.0;
        ++pass_marks[kind];
      }
      D3D12_RANGE none{0, 0};
      pass_readback->Unmap(0, &none);
      if (++pass_frames < 600)
      {
        return;
      }
      static const char *const kNames[kPassKinds] = {"shadow draws", "shadow clears", "shadow resolves",
                                                     "main draws",   "main clears",   "main resolves",
                                                     "other draws",  "other clears",  "other resolves",
                                                     "present"};
      double total = 0.0;
      std::string parts;
      for (uint32_t k = 0; k < kPassKinds; ++k)
      {
        total += pass_ms[k];
        parts += fmt::format("{}{} {:.3f} ({:.1f})", k ? ", " : "", kNames[k], pass_ms[k] / pass_frames,
                             double(pass_marks[k]) / pass_frames);
      }
      REXLOG_INFO("pdz_native: gpu passes ({} frames): total {:.3f} ms/frame | ms/frame (passes/frame): {} | marks "
                  "dropped {}",
                  pass_frames, total / pass_frames, parts, pass_overflow);
      std::fill(std::begin(pass_ms), std::end(pass_ms), 0.0);
      std::fill(std::begin(pass_marks), std::end(pass_marks), 0);
      pass_frames = 0;
    }

    // Descriptor rings: positions are monotonic (index = position % size). frame_start[slot] = the ring position
    // when that slot's frame began; a frame may allocate up to the start of the oldest frame still in flight.
    struct Ring
    {
      const char *name = "";
      uint32_t size = 0;
      uint64_t head = 0;
      uint64_t frame_start[kFrames] = {};
      uint64_t used_this_frame = 0, peak_frame = 0;
      uint64_t failures = 0;
      uint64_t warned_frame = ~uint64_t(0);
      uint32_t warnings = 0;
    };
    Ring srv_ring, sampler_ring;
    bool upload_grow_logged = false;

    // The oldest ring position still owned by a frame in flight other than the current one.
    uint64_t RingTail(const Ring &ring, uint32_t current, uint64_t completed) const
    {
      uint64_t tail = ring.frame_start[current];
      for (uint32_t s = 0; s < kFrames; ++s)
      {
        if (s != current && frames[s].fence_value > completed)
        {
          tail = std::min(tail, ring.frame_start[s]);
        }
      }
      return tail;
    }

    // Returns the ring index of `count` contiguous entries, or ~0u when the ring is full (logged once per frame).
    uint32_t RingAlloc(Ring &ring, uint32_t count, uint64_t frame_index)
    {
      uint64_t pos = ring.head;
      if ((pos % ring.size) + count > ring.size)
      {
        pos += ring.size - (pos % ring.size); // no wrap inside one table
      }
      uint64_t limit = RingTail(ring, slot, 0) + ring.size; // assume nothing completed (cheap, conservative)
      if (pos + count > limit)
      {
        limit = RingTail(ring, slot, fence->GetCompletedValue()) + ring.size;
      }
      if (pos + count > limit)
      {
        ++ring.failures;
        pdz::timing::CountEvent(pdz::timing::Event::kDescriptorFull); // pdz_frame_timing_spike_ms
        if (ring.warned_frame != frame_index && ring.warnings < 50)
        {
          ring.warned_frame = frame_index;
          ++ring.warnings;
          REXLOG_WARN("pdz_native: {} descriptor ring full in frame {} ({} requested, {} used this frame, ring {}); "
                      "the draw is skipped",
                      ring.name, frame_index, count, ring.used_this_frame, ring.size);
        }
        return ~0u;
      }
      ring.used_this_frame += (pos + count) - ring.head;
      ring.peak_frame = std::max(ring.peak_frame, ring.used_this_frame);
      ring.head = pos + count;
      return uint32_t(pos % ring.size);
    }

    // Persistent non-shader-visible descriptors: a bump index plus a free list of returned slots. A full heap
    // refuses the allocation (ptr == 0, counted, logged a few times) instead of handing out slot 0 again: slot 0
    // belongs to a live surface (usually the first colour target), and a second view written over it would send
    // that surface's draws into another resource (REVIEW2.md C3).
    struct FreeListHeap
    {
      ID3D12DescriptorHeap *heap = nullptr;
      uint32_t size = 0, next = 0, descriptor_size = 0;
      uint32_t live = 0;
      std::vector<uint32_t> free_list;
      D3D12_CPU_DESCRIPTOR_HANDLE start{};
      uint64_t failures = 0;
      uint32_t warnings = 0;
      uint32_t bad_frees = 0;

      D3D12_CPU_DESCRIPTOR_HANDLE Alloc(const char *name)
      {
        uint32_t index;
        if (!free_list.empty())
        {
          index = free_list.back();
          free_list.pop_back();
        }
        else if (next < size)
        {
          index = next++;
        }
        else
        {
          ++failures;
          if (warnings < 8)
          {
            ++warnings;
            REXLOG_ERROR("pdz_native: {} descriptor heap exhausted ({} entries, {} live); allocation refused "
                         "(#{}; the surface / texture is skipped)",
                         name, size, live, failures);
          }
          return D3D12_CPU_DESCRIPTOR_HANDLE{};
        }
        ++live;
        D3D12_CPU_DESCRIPTOR_HANDLE h = start;
        h.ptr += SIZE_T(index) * descriptor_size;
        return h;
      }
      void Free(D3D12_CPU_DESCRIPTOR_HANDLE h, const char *name)
      {
        if (!heap || !h.ptr || h.ptr < start.ptr)
        {
          return; // ptr == 0: a refused allocation
        }
        const uint32_t index = uint32_t((h.ptr - start.ptr) / descriptor_size);
        if (index >= next || live == 0)
        {
          if (bad_frees++ < 4)
          {
            REXLOG_ERROR("pdz_native: {} descriptor free of an unallocated slot {} ignored", name, index);
          }
          return;
        }
        free_list.push_back(index);
        --live;
      }
    };
    FreeListHeap staging, rtv, dsv;
    // pdz_native_record_workers 2: RTV / DSV slots freed since the last drain (FreeRtv / FreeDsv).
    std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> held_rtv, held_dsv;

    std::vector<std::pair<uint64_t, IUnknown *>> pending_releases; // fence value to wait for, object
  };

  namespace
  {
    NativeDevice *g_device = nullptr;
    bool g_device_tried = false;

    void SetName(ID3D12Object *object, const char *name)
    {
      if (object && name)
      {
        wchar_t wide[128];
        size_t i = 0;
        for (; name[i] && i < 127; ++i)
        {
          wide[i] = wchar_t(name[i]);
        }
        wide[i] = 0;
        object->SetName(wide);
      }
    }

    bool CreateHeap(ID3D12Device *device, D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t count, bool shader_visible,
                    ID3D12DescriptorHeap **out, const char *name)
    {
      D3D12_DESCRIPTOR_HEAP_DESC desc{};
      desc.Type = type;
      desc.NumDescriptors = count;
      desc.Flags = shader_visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
      if (FAILED(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(out))))
      {
        REXLOG_ERROR("pdz_native: CreateDescriptorHeap({}, {}) failed", name, count);
        return false;
      }
      SetName(*out, name);
      return true;
    }
  } // namespace

  NativeDevice *NativeDevice::Get()
  {
    if (!g_device_tried)
    {
      g_device_tried = true;
      NativeDevice *d = new NativeDevice();
      if (d->Init())
      {
        g_device = d;
      }
      else
      {
        REXLOG_ERROR("pdz_native: no D3D12 device; the native renderer stays off for this session");
        delete d;
      }
    }
    return g_device;
  }

  NativeDevice *NativeDevice::Existing() { return g_device; }

  bool NativeDevice::Init()
  {
    // The SDK's device: the Xenia GPU plugin's graphics system owns the provider (and the presenter the
    // window paints from). gpu_plugin = "" (no emulation) means no provider: stage 1 needs the plugin.
    rex::Runtime *runtime = rex::Runtime::instance();
    rex::system::IGraphicsSystem *graphics = runtime ? runtime->graphics_system() : nullptr;
    rex::ui::GraphicsProvider *generic = graphics ? graphics->provider() : nullptr;
    if (!generic)
    {
      REXLOG_ERROR("pdz_native: the runtime has no graphics provider (gpu_plugin not loaded?)");
      return false;
    }
    // No RTTI in this build: the backend is decided the way the plugin decides it (plugin_main.cpp: "any" or
    // "d3d12" -> D3D12 when compiled in), so graphics_backend == "vulkan" is the only non-D3D12 case.
    const std::string backend = rex::cvar::GetFlagByName("graphics_backend");
    if (backend == "vulkan" || !rex::ui::d3d12::D3D12Provider::IsD3D12APIAvailable())
    {
      REXLOG_ERROR("pdz_native: graphics backend '{}' is not D3D12", backend);
      return false;
    }
    provider_ = static_cast<rex::ui::d3d12::D3D12Provider *>(generic);
    device_ = provider_->GetDevice();
    queue_ = provider_->GetDirectQueue();
    if (!device_ || !queue_)
    {
      REXLOG_ERROR("pdz_native: the D3D12 provider has no device/queue");
      return false;
    }
    srv_size_ = provider_->GetViewDescriptorSize();
    sampler_size_ = provider_->GetSamplerDescriptorSize();
    rtv_size_ = provider_->GetRTVDescriptorSize();
    dsv_size_ = provider_->GetDSVDescriptorSize();

    impl_ = new Impl();
    Impl &im = *impl_;

    if (FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&im.fence))))
    {
      REXLOG_ERROR("pdz_native: CreateFence failed");
      return false;
    }
    SetName(im.fence, "pdz_native fence");
    im.fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!im.fence_event)
    {
      return false;
    }

    for (uint32_t i = 0; i < Impl::kFrames; ++i)
    {
      Impl::Frame &f = im.frames[i];
      if (FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&f.allocator))))
      {
        REXLOG_ERROR("pdz_native: CreateCommandAllocator failed");
        return false;
      }
      SetName(f.allocator, "pdz_native allocator");
      f.upload = CreateBuffer(Impl::kUploadInitialBytes, D3D12_HEAP_TYPE_UPLOAD,
                              D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE, "pdz_native upload");
      if (!f.upload)
      {
        return false;
      }
      f.upload_size = Impl::kUploadInitialBytes;
      D3D12_RANGE no_read{0, 0};
      if (FAILED(f.upload->Map(0, &no_read, reinterpret_cast<void **>(&f.upload_cpu))))
      {
        REXLOG_ERROR("pdz_native: upload buffer Map failed");
        return false;
      }
    }
    if (FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, im.frames[0].allocator, nullptr,
                                          IID_PPV_ARGS(&list_))))
    {
      REXLOG_ERROR("pdz_native: CreateCommandList failed");
      return false;
    }
    SetName(list_, "pdz_native list");
    list_->Close();
    rec_.Bind(list_);

    if (!CreateHeap(device_, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, Impl::kSrvHeapSize, true, &srv_heap_,
                    "pdz_native srv heap") ||
        !CreateHeap(device_, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, Impl::kSamplerHeapSize, true, &sampler_heap_,
                    "pdz_native sampler heap") ||
        !CreateHeap(device_, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, Impl::kStagingHeapSize, false,
                    &im.staging.heap, "pdz_native staging heap") ||
        !CreateHeap(device_, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, Impl::kRtvHeapSize, false, &im.rtv.heap,
                    "pdz_native rtv heap") ||
        !CreateHeap(device_, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, Impl::kDsvHeapSize, false, &im.dsv.heap,
                    "pdz_native dsv heap"))
    {
      return false;
    }
    im.srv_ring.name = "SRV";
    im.srv_ring.size = Impl::kSrvRingSize;
    im.sampler_ring.name = "sampler";
    im.sampler_ring.size = Impl::kSamplerRingSize;
    im.staging.size = Impl::kStagingHeapSize;
    im.staging.descriptor_size = srv_size_;
    im.staging.start = im.staging.heap->GetCPUDescriptorHandleForHeapStart();
    im.rtv.size = Impl::kRtvHeapSize;
    im.rtv.descriptor_size = rtv_size_;
    im.rtv.start = im.rtv.heap->GetCPUDescriptorHandleForHeapStart();
    im.dsv.size = Impl::kDsvHeapSize;
    im.dsv.descriptor_size = dsv_size_;
    im.dsv.start = im.dsv.heap->GetCPUDescriptorHandleForHeapStart();

    // Entry points the executable does not import. d3d12.dll is loaded (the provider created the device);
    // d3dcompiler_47.dll ships with Windows 10+.
    if (HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll"))
    {
      serialize_root_signature_ = reinterpret_cast<PFN_SerializeRootSignature>(
          reinterpret_cast<void *>(GetProcAddress(d3d12, "D3D12SerializeRootSignature")));
    }
    if (HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll"))
    {
      d3d_compile_ = reinterpret_cast<PFN_Compile>(reinterpret_cast<void *>(GetProcAddress(compiler, "D3DCompile")));
    }
    if (!serialize_root_signature_ || !d3d_compile_)
    {
      REXLOG_ERROR("pdz_native: D3D12SerializeRootSignature / D3DCompile unavailable ({}, {})",
                   serialize_root_signature_ != nullptr, d3d_compile_ != nullptr);
      return false;
    }

    REXLOG_INFO("pdz_native: device ready on the SDK's D3D12 device ({} frames in flight, {} MB upload per "
                "frame, {} SRV + {} sampler descriptors in rings shared by the frames)",
                Impl::kFrames, Impl::kUploadInitialBytes >> 20, im.srv_ring.size, im.sampler_ring.size);
    return true;
  }

  // ---------------------------------------------------------------------------
  // Frames
  // ---------------------------------------------------------------------------
  void NativeDevice::BeginFrame()
  {
    if (in_frame_)
    {
      return;
    }
    const pdz::timing::PhaseScope phase(pdz::timing::Phase::kFrameBegin); // pdz_frame_timing_phases
    Impl &im = *impl_;
    im.slot = uint32_t(frame_index_ % Impl::kFrames);
    Impl::Frame &f = im.frames[im.slot];
    if (f.fence_value && im.fence->GetCompletedValue() < f.fence_value)
    {
      const pdz::timing::PhaseScope fence_wait(pdz::timing::Phase::kFenceWait);
      pdz::timing::CountEvent(pdz::timing::Event::kFenceWait); // pdz_frame_timing_spike_ms
      watchdog::Wait wait("native BeginFrame: the frame slot's fence (our GPU work of 3 frames ago)", f.fence_value);
      im.fence->SetEventOnCompletion(f.fence_value, im.fence_event);
      WaitForSingleObject(im.fence_event, INFINITE);
    }
    ReleaseCompleted(); // deferred releases whose fence has passed
    im.TimestampCollect(); // pdz_frame_timing: the GPU time of this slot's previous frame (complete now)
    im.PassCollect();      // pdz_native_gpu_passes

    // pdz_native_upload_shrink (REVIEW2.md M1): a buffer that grew for a burst is otherwise kept for the session, in
    // each slot that saw one. The slot's earlier frames are complete (the wait above), and this frame has not
    // uploaded anything yet, so the buffer can be swapped here; the old one goes through ReleaseLater like a grown one.
    if (f.window_frames >= Impl::kShrinkWindowFrames)
    {
      if (REXCVAR_GET(pdz_native_upload_shrink) && f.upload_size > Impl::kUploadInitialBytes &&
          f.window_peak <= f.upload_size / 4)
      {
        uint64_t target = Impl::kUploadInitialBytes;
        while (target < 2 * f.window_peak)
        {
          target *= 2;
        }
        ID3D12Resource *smaller = target < f.upload_size
                                      ? CreateBuffer(target, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                     D3D12_RESOURCE_FLAG_NONE, "pdz_native upload")
                                      : nullptr;
        uint8_t *cpu = nullptr;
        D3D12_RANGE no_read{0, 0};
        if (smaller && SUCCEEDED(smaller->Map(0, &no_read, reinterpret_cast<void **>(&cpu))))
        {
          if (im.shrink_logged < 8)
          {
            ++im.shrink_logged;
            REXLOG_INFO("pdz_native: upload buffer of frame slot {} shrunk from {} MB to {} MB (the slot's largest frame "
                        "of its last {} used {:.1f} MB; #{})",
                        im.slot, f.upload_size >> 20, target >> 20, f.window_frames, double(f.window_peak) / 1048576.0,
                        im.upload_shrinks + 1);
          }
          f.upload->Unmap(0, nullptr);
          ReleaseLater(f.upload);
          f.upload = smaller;
          f.upload_cpu = cpu;
          f.upload_size = target;
          ++im.upload_shrinks;
        }
        else if (smaller)
        {
          smaller->Release(); // keep the large one
        }
      }
      f.window_peak = 0;
      f.window_frames = 0;
    }

    f.allocator->Reset();
    rec_.StartIfWanted(frame_index_); // pdz_native_record_workers (once; the list is closed, the replay thread idle)
    list_->Reset(f.allocator, nullptr);
    rec_.SetOpen(true);
    if (pdz::timing::Enabled() && im.EnsureTimestamps(device_, queue_))
    {
      im.TimestampBegin(&rec_);
    }
    if (REXCVAR_GET(pdz_native_gpu_passes))
    {
      im.EnsurePasses(device_, queue_);
    }
    im.PassListBegin(&rec_);
    ID3D12DescriptorHeap *heaps[2] = {srv_heap_, sampler_heap_};
    rec_.SetDescriptorHeaps(2, heaps);
    ++list_epoch_; // fresh list: no root signature / PSO bound
    f.upload_offset = 0;
    f.upload_carry = 0;
    for (Impl::Ring *ring : {&im.srv_ring, &im.sampler_ring})
    {
      ring->frame_start[im.slot] = ring->head;
      ring->used_this_frame = 0;
    }
    in_frame_ = true;
  }

  void NativeDevice::EndFrame()
  {
    if (!in_frame_)
    {
      return;
    }
    const pdz::timing::PhaseScope phase(pdz::timing::Phase::kSubmit); // pdz_frame_timing_phases
    Impl &im = *impl_;
    Impl::Frame &f = im.frames[im.slot];
    im.TimestampEnd(&rec_);
    im.PassListEnd(&rec_);
    DrainRecording(); // pdz_native_record_workers 2: every recorded call made on the list
    rec_.SetOpen(false);
    list_->Close();
    ID3D12CommandList *lists[1] = {list_};
    asyncup::BeforeDirectSubmit(queue_); // pdz_native_async_upload: the copies this list samples (GPU-side wait)
    queue_->ExecuteCommandLists(1, lists);
    const uint64_t value = im.next_fence_value++;
    queue_->Signal(im.fence, value);
    f.fence_value = value;
    last_signalled_ = value;
    in_frame_ = false;
    ++frame_index_;
    // Upload use of the frame: the shrink window (BeginFrame) and the memory log's frame peak.
    const uint64_t used = f.upload_carry + f.upload_offset;
    f.window_peak = std::max(f.window_peak, used);
    ++f.window_frames;
    im.upload_frame_peak = std::max(im.upload_frame_peak, used);
  }

  void NativeDevice::GpuMark(GpuPass pass)
  {
    Impl &im = *impl_;
    if (in_frame_ && im.pass_heap && REXCVAR_GET(pdz_native_gpu_passes))
    {
      im.PassMark(&rec_, uint8_t(pass));
    }
  }

  bool NativeDevice::GpuPassesOn() const
  {
    return in_frame_ && impl_->pass_heap && REXCVAR_GET(pdz_native_gpu_passes);
  }

  void NativeDevice::Flush() { SubmitOpenList(true); }

  uint64_t NativeDevice::Submit() { return SubmitOpenList(false); }

  uint64_t NativeDevice::SubmitOpenList(bool wait)
  {
    if (!in_frame_)
    {
      return 0;
    }
    const pdz::timing::PhaseScope phase(pdz::timing::Phase::kSubmit); // pdz_frame_timing_phases
    Impl &im = *impl_;
    Impl::Frame &f = im.frames[im.slot];
    im.TimestampEnd(&rec_);
    im.PassListEnd(&rec_);
    DrainRecording(); // pdz_native_record_workers 2: every recorded call made on the list
    rec_.SetOpen(false);
    list_->Close();
    ID3D12CommandList *lists[1] = {list_};
    asyncup::BeforeDirectSubmit(queue_); // pdz_native_async_upload: the copies this list samples (GPU-side wait)
    queue_->ExecuteCommandLists(1, lists);
    const uint64_t value = im.next_fence_value++;
    queue_->Signal(im.fence, value);
    f.fence_value = value;
    last_signalled_ = value;
    if (wait && im.fence->GetCompletedValue() < value)
    {
      const pdz::timing::PhaseScope flush_wait(pdz::timing::Phase::kFlushWait);
      pdz::timing::CountEvent(pdz::timing::Event::kFlushWait); // pdz_frame_timing_spike_ms
      watchdog::Wait wait_note("native Flush: submit and wait (de-rez snapshot / auto-diff readback)", value);
      im.fence->SetEventOnCompletion(value, im.fence_event);
      WaitForSingleObject(im.fence_event, INFINITE);
    }
    // The allocator keeps growing until the slot is reset at the next BeginFrame; fine for one flush per
    // de-rez event. Without the wait (Submit) the list is reset while the GPU still runs the part just executed:
    // allowed (no other list records into the allocator), only the allocator's own Reset needs the GPU done.
    list_->Reset(f.allocator, nullptr);
    rec_.SetOpen(true);
    if (pdz::timing::Enabled() && im.EnsureTimestamps(device_, queue_))
    {
      im.TimestampBegin(&rec_); // the rest of the frame: another timed list
    }
    im.PassListBegin(&rec_);
    ID3D12DescriptorHeap *heaps[2] = {srv_heap_, sampler_heap_};
    rec_.SetDescriptorHeaps(2, heaps);
    ++list_epoch_; // the reset list lost the draw path's root signature / PSO (same frame index)
    return value;
  }

  bool NativeDevice::WaitFence(uint64_t value, HANDLE event, uint32_t timeout_ms) const
  {
    Impl &im = *impl_;
    if (im.fence->GetCompletedValue() >= value)
    {
      return true;
    }
    if (!event || FAILED(im.fence->SetEventOnCompletion(value, event)))
    {
      return false;
    }
    WaitForSingleObject(event, timeout_ms);
    return im.fence->GetCompletedValue() >= value;
  }

  Descriptor NativeDevice::AllocPersistentSampler(uint32_t count)
  {
    Impl &im = *impl_;
    Descriptor d;
    count = std::max(count, 1u);
    // pdz_native_sampler_region_limit (test aid): only the first N descriptors of the region.
    const int32_t limit_cvar = REXCVAR_GET(pdz_native_sampler_region_limit);
    const uint32_t limit = limit_cvar > 0 ? std::min<uint32_t>(uint32_t(limit_cvar), Impl::kSamplerPersistentSize)
                                          : Impl::kSamplerPersistentSize;
    uint32_t offset = ~0u;
    for (size_t i = 0; i < im.sampler_free.size(); ++i)
    {
      Impl::SamplerRange &r = im.sampler_free[i];
      if (r.count >= count && r.start + count <= limit)
      {
        offset = r.start;
        r.start += count;
        r.count -= count;
        if (r.count == 0)
        {
          im.sampler_free.erase(im.sampler_free.begin() + std::ptrdiff_t(i));
        }
        break;
      }
    }
    if (offset == ~0u)
    {
      ++im.sampler_persistent_refused;
      if (!im.sampler_persistent_full_logged)
      {
        im.sampler_persistent_full_logged = true;
        REXLOG_WARN("pdz_native: persistent sampler region full ({} entries, {} in tables); new tables use the per-frame "
                    "ring until pdz_native_sampler_evict frees room (refusals are counted: memory log "
                    "sampler_region_refused)",
                    limit, im.sampler_persistent_live);
      }
      return d;
    }
    im.sampler_persistent_live += count;
    im.sampler_persistent_high = std::max(im.sampler_persistent_high, offset + count);
    const uint32_t index = Impl::kSamplerRingSize + offset;
    d.index = index;
    d.cpu = sampler_heap_->GetCPUDescriptorHandleForHeapStart();
    d.cpu.ptr += SIZE_T(index) * sampler_size_;
    d.gpu = sampler_heap_->GetGPUDescriptorHandleForHeapStart();
    d.gpu.ptr += UINT64(index) * sampler_size_;
    return d;
  }

  void NativeDevice::FreePersistentSampler(uint32_t heap_index, uint32_t count)
  {
    Impl &im = *impl_;
    count = std::max(count, 1u);
    const uint32_t start = heap_index - Impl::kSamplerRingSize;
    auto bad = [&](const char *why) {
      if (im.sampler_bad_frees++ < 4)
      {
        REXLOG_ERROR("pdz_native: persistent sampler free of [{}, +{}) ignored: {}", heap_index, count, why);
      }
    };
    if (heap_index < Impl::kSamplerRingSize || start + count > Impl::kSamplerPersistentSize ||
        count > im.sampler_persistent_live)
    {
      bad("not an allocated persistent table");
      return;
    }
    // Sorted insert; a range overlapping a free one is a double free.
    auto next = std::lower_bound(im.sampler_free.begin(), im.sampler_free.end(), start,
                                 [](const Impl::SamplerRange &r, uint32_t s) { return r.start < s; });
    if ((next != im.sampler_free.end() && next->start < start + count) ||
        (next != im.sampler_free.begin() && std::prev(next)->start + std::prev(next)->count > start))
    {
      bad("overlaps a free range");
      return;
    }
    auto it = im.sampler_free.insert(next, Impl::SamplerRange{start, count});
    if (std::next(it) != im.sampler_free.end() && it->start + it->count == std::next(it)->start)
    {
      it->count += std::next(it)->count;
      im.sampler_free.erase(std::next(it));
    }
    if (it != im.sampler_free.begin() && std::prev(it)->start + std::prev(it)->count == it->start)
    {
      std::prev(it)->count += it->count;
      im.sampler_free.erase(it);
    }
    im.sampler_persistent_live -= count;
  }

  void NativeDevice::DescriptorStats(uint64_t &srv_peak, uint64_t &srv_failures, uint64_t &sampler_peak,
                                     uint64_t &sampler_failures) const
  {
    srv_peak = impl_->srv_ring.peak_frame;
    srv_failures = impl_->srv_ring.failures;
    sampler_peak = impl_->sampler_ring.peak_frame;
    sampler_failures = impl_->sampler_ring.failures;
  }

  uint64_t NativeDevice::completed_fence() const { return impl_->fence->GetCompletedValue(); }

  void NativeDevice::WaitIdle()
  {
    Impl &im = *impl_;
    if (in_frame_)
    {
      EndFrame();
    }
    const uint64_t value = im.next_fence_value++;
    queue_->Signal(im.fence, value);
    if (im.fence->GetCompletedValue() < value)
    {
      const pdz::timing::PhaseScope flush_wait(pdz::timing::Phase::kFlushWait); // pdz_frame_timing_phases
      pdz::timing::CountEvent(pdz::timing::Event::kFlushWait);                   // pdz_frame_timing_spike_ms
      watchdog::Wait wait("native WaitIdle", value);
      im.fence->SetEventOnCompletion(value, im.fence_event);
      WaitForSingleObject(im.fence_event, INFINITE);
    }
    for (auto &p : im.pending_releases)
    {
      p.second->Release();
    }
    im.pending_releases.clear();
  }

  void NativeDevice::ReleaseLater(IUnknown *resource)
  {
    if (!resource)
    {
      return;
    }
    // Waits for the value the current (or next) frame will signal.
    impl_->pending_releases.emplace_back(impl_->next_fence_value, resource);
  }

  void NativeDevice::ReleaseCompleted()
  {
    Impl &im = *impl_;
    if (im.pending_releases.empty())
    {
      return;
    }
    const uint64_t completed = im.fence->GetCompletedValue();
    size_t kept = 0;
    for (size_t i = 0; i < im.pending_releases.size(); ++i)
    {
      if (im.pending_releases[i].first <= completed)
      {
        im.pending_releases[i].second->Release();
      }
      else
      {
        im.pending_releases[kept++] = im.pending_releases[i];
      }
    }
    im.pending_releases.resize(kept);
  }

  // ---------------------------------------------------------------------------
  // Upload memory
  // ---------------------------------------------------------------------------
  UploadSlice NativeDevice::Upload(uint32_t size, uint32_t align)
  {
    UploadSlice slice;
    if (!in_frame_)
    {
      BeginFrame();
    }
    Impl &im = *impl_;
    Impl::Frame &f = im.frames[im.slot];
    align = std::max<uint32_t>(align, 1);
    uint64_t offset = (f.upload_offset + align - 1) / align * align;
    if (offset + size > f.upload_size)
    {
      // Grow by doubling (the old buffer is still referenced by this frame's commands: deferred release).
      uint64_t new_size = f.upload_size;
      while (new_size < uint64_t(size) + align)
      {
        new_size *= 2;
      }
      new_size = std::max(new_size * 2, uint64_t(size) + align);
      ID3D12Resource *grown = CreateBuffer(new_size, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ,
                                           D3D12_RESOURCE_FLAG_NONE, "pdz_native upload (grown)");
      uint8_t *cpu = nullptr;
      D3D12_RANGE no_read{0, 0};
      if (!grown || FAILED(grown->Map(0, &no_read, reinterpret_cast<void **>(&cpu))))
      {
        REXLOG_ERROR("pdz_native: upload buffer growth to {} MB failed", new_size >> 20);
        if (grown)
        {
          grown->Release();
        }
        return slice;
      }
      if (!im.upload_grow_logged)
      {
        im.upload_grow_logged = true;
        REXLOG_WARN("pdz_native: upload buffer grown to {} MB (frame slot {})", new_size >> 20, im.slot);
      }
      ++im.upload_grows;
      pdz::timing::CountEvent(pdz::timing::Event::kUploadGrow); // pdz_frame_timing_spike_ms
      f.upload_carry += f.upload_offset; // this frame's use of the replaced buffer
      f.upload->Unmap(0, nullptr);
      ReleaseLater(f.upload);
      f.upload = grown;
      f.upload_cpu = cpu;
      f.upload_size = new_size;
      offset = 0;
    }
    slice.cpu = f.upload_cpu + offset;
    slice.gpu = f.upload->GetGPUVirtualAddress() + offset;
    slice.resource = f.upload;
    slice.offset = offset;
    slice.size = size;
    f.upload_offset = offset + size;
    return slice;
  }

  // ---------------------------------------------------------------------------
  // Descriptors
  // ---------------------------------------------------------------------------
  Descriptor NativeDevice::AllocSrv(uint32_t count)
  {
    Impl &im = *impl_;
    Descriptor d;
    const uint32_t index = im.RingAlloc(im.srv_ring, std::max(count, 1u), frame_index_);
    if (index == ~0u)
    {
      return d; // cpu.ptr == 0: the caller skips its draw / pass
    }
    d.index = index;
    d.cpu = srv_heap_->GetCPUDescriptorHandleForHeapStart();
    d.cpu.ptr += SIZE_T(index) * srv_size_;
    d.gpu = srv_heap_->GetGPUDescriptorHandleForHeapStart();
    d.gpu.ptr += UINT64(index) * srv_size_;
    return d;
  }

  Descriptor NativeDevice::AllocSampler(uint32_t count)
  {
    Impl &im = *impl_;
    Descriptor d;
    const uint32_t index = im.RingAlloc(im.sampler_ring, std::max(count, 1u), frame_index_);
    if (index == ~0u)
    {
      return d;
    }
    d.index = index;
    d.cpu = sampler_heap_->GetCPUDescriptorHandleForHeapStart();
    d.cpu.ptr += SIZE_T(index) * sampler_size_;
    d.gpu = sampler_heap_->GetGPUDescriptorHandleForHeapStart();
    d.gpu.ptr += UINT64(index) * sampler_size_;
    return d;
  }

  D3D12_CPU_DESCRIPTOR_HANDLE NativeDevice::AllocRtv() { return impl_->rtv.Alloc("RTV"); }
  D3D12_CPU_DESCRIPTOR_HANDLE NativeDevice::AllocDsv() { return impl_->dsv.Alloc("DSV"); }
  D3D12_CPU_DESCRIPTOR_HANDLE NativeDevice::AllocStagingSrv() { return impl_->staging.Alloc("staging SRV"); }
  // pdz_native_record_workers 2: D3D12 reads RTV / DSV descriptors when OMSetRenderTargets / Clear*View is recorded,
  // which is now on the replay thread, later. A freed slot is therefore held until the next drain (every recorded
  // call made), so no slot a pending call names is reused and rewritten before the call is made.
  void NativeDevice::FreeRtv(D3D12_CPU_DESCRIPTOR_HANDLE h)
  {
    if (rec_.deferred())
    {
      impl_->held_rtv.push_back(h);
      return;
    }
    impl_->rtv.Free(h, "RTV");
  }
  void NativeDevice::FreeDsv(D3D12_CPU_DESCRIPTOR_HANDLE h)
  {
    if (rec_.deferred())
    {
      impl_->held_dsv.push_back(h);
      return;
    }
    impl_->dsv.Free(h, "DSV");
  }
  void NativeDevice::DrainRecording()
  {
    rec_.Drain();
    // d3d12_debug (the SDK's debug layer switch): the debug layer's stored messages at each submit of ours, so an
    // error is logged next to the frame that caused it (the SDK itself only logs them when a pipeline fails).
    if (REXCVAR_GET(d3d12_debug) && provider_)
    {
      provider_->LogD3D12DebugMessages();
    }
    Impl &im = *impl_;
    for (const D3D12_CPU_DESCRIPTOR_HANDLE h : im.held_rtv)
    {
      im.rtv.Free(h, "RTV");
    }
    for (const D3D12_CPU_DESCRIPTOR_HANDLE h : im.held_dsv)
    {
      im.dsv.Free(h, "DSV");
    }
    im.held_rtv.clear();
    im.held_dsv.clear();
  }
  void NativeDevice::FreeStagingSrv(D3D12_CPU_DESCRIPTOR_HANDLE h) { impl_->staging.Free(h, "staging SRV"); }

  NativeDevice::PersistentHeapStats NativeDevice::GetPersistentHeapStats() const
  {
    const Impl &im = *impl_;
    PersistentHeapStats s;
    s.rtv_live = im.rtv.live;
    s.dsv_live = im.dsv.live;
    s.staging_live = im.staging.live;
    s.rtv_size = im.rtv.size;
    s.dsv_size = im.dsv.size;
    s.staging_size = im.staging.size;
    s.rtv_failures = im.rtv.failures;
    s.dsv_failures = im.dsv.failures;
    s.staging_failures = im.staging.failures;
    s.rtv_peak = im.rtv.next;
    s.dsv_peak = im.dsv.next;
    s.staging_peak = im.staging.next;
    return s;
  }

  NativeDevice::MemoryStats NativeDevice::GetMemoryStats()
  {
    Impl &im = *impl_;
    MemoryStats s;
    for (const Impl::Frame &f : im.frames)
    {
      s.upload_bytes += f.upload_size;
    }
    if (in_frame_)
    {
      const Impl::Frame &f = im.frames[im.slot];
      s.upload_in_use = f.upload_carry + f.upload_offset;
      s.srv_in_use = im.srv_ring.used_this_frame;
      s.sampler_in_use = im.sampler_ring.used_this_frame;
    }
    s.upload_frame_peak = std::max(im.upload_frame_peak, s.upload_in_use);
    im.upload_frame_peak = 0;
    s.upload_grows = im.upload_grows;
    s.upload_shrinks = im.upload_shrinks;
    s.srv_ring_size = im.srv_ring.size;
    s.srv_peak = im.srv_ring.peak_frame;
    s.srv_refused = im.srv_ring.failures;
    s.sampler_ring_size = im.sampler_ring.size;
    s.sampler_peak = im.sampler_ring.peak_frame;
    s.sampler_refused = im.sampler_ring.failures;
    s.sampler_persistent_used = im.sampler_persistent_live;
    s.sampler_persistent_size = Impl::kSamplerPersistentSize;
    s.sampler_persistent_high = im.sampler_persistent_high;
    s.sampler_persistent_refused = im.sampler_persistent_refused;
    s.sampler_persistent_ranges = uint32_t(im.sampler_free.size());
    s.heaps = GetPersistentHeapStats();
    s.pending_releases = im.pending_releases.size();
    for (const auto &p : im.pending_releases)
    {
      ID3D12Resource *resource = nullptr;
      if (p.second && SUCCEEDED(p.second->QueryInterface(IID_PPV_ARGS(&resource))) && resource)
      {
        const D3D12_RESOURCE_DESC desc = resource->GetDesc();
        s.pending_release_bytes += device_->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
        resource->Release();
      }
    }
    return s;
  }

  void NativeDevice::CopySrv(D3D12_CPU_DESCRIPTOR_HANDLE dst, D3D12_CPU_DESCRIPTOR_HANDLE src)
  {
    if (!src.ptr)
    {
      // A refused staging allocation (the texture and surface callers skip before this; defensive): a null view
      // reads zero instead of whatever an earlier frame left in the ring slot.
      D3D12_SHADER_RESOURCE_VIEW_DESC null_desc{};
      null_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
      null_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
      null_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
      null_desc.Texture2D.MipLevels = 1;
      device_->CreateShaderResourceView(nullptr, &null_desc, dst);
      return;
    }
    device_->CopyDescriptorsSimple(1, dst, src, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  }

  void NativeDevice::CopySampler(D3D12_CPU_DESCRIPTOR_HANDLE dst, D3D12_CPU_DESCRIPTOR_HANDLE src)
  {
    device_->CopyDescriptorsSimple(1, dst, src, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
  }

  // ---------------------------------------------------------------------------
  // Barriers and resources
  // ---------------------------------------------------------------------------
  void NativeDevice::Transition(ID3D12Resource *resource, D3D12_RESOURCE_STATES &tracked,
                                D3D12_RESOURCE_STATES to)
  {
    if (!resource || tracked == to)
    {
      return;
    }
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = resource;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = tracked;
    b.Transition.StateAfter = to;
    rec_.ResourceBarrier(1, &b);
    tracked = to;
  }

  void NativeDevice::UavBarrier(ID3D12Resource *resource)
  {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    b.UAV.pResource = resource;
    rec_.ResourceBarrier(1, &b);
  }

  ID3D12Resource *NativeDevice::CreateTexture(const D3D12_RESOURCE_DESC &desc, D3D12_RESOURCE_STATES initial_state,
                                              const D3D12_CLEAR_VALUE *clear_value, const char *debug_name, bool count)
  {
    ID3D12Resource *resource = nullptr;
    // Render targets and depth buffers are created zeroed: a not-zeroed one must see a Clear / Discard / Copy
    // before any other use (debug layer error 1422), and guest surfaces are often drawn to before any clear.
    const bool target = (desc.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) != 0;
    // count = false: an upload worker (pdz_native_async_upload): not the record thread's frame (spike line).
    pdz::timing::CountEvent(pdz::timing::Event::kResourceCreate, count ? 1 : 0); // pdz_frame_timing_spike_ms
    std::optional<pdz::timing::EventTimer> create_time;
    if (count)
    {
      create_time.emplace(pdz::timing::Event::kResourceCycles);
    }
    const HRESULT hr = device_->CreateCommittedResource(&rex::ui::d3d12::util::kHeapPropertiesDefault,
                                                        target ? D3D12_HEAP_FLAG_NONE : provider_->GetHeapFlagCreateNotZeroed(),
                                                        &desc, initial_state, clear_value, IID_PPV_ARGS(&resource));
    if (FAILED(hr))
    {
      REXLOG_ERROR("pdz_native: CreateCommittedResource({}, {}x{}x{}, format {}) failed: 0x{:08X}", debug_name,
                   desc.Width, desc.Height, desc.DepthOrArraySize, uint32_t(desc.Format), uint32_t(hr));
      return nullptr;
    }
    SetName(resource, debug_name);
    return resource;
  }

  ID3D12Resource *NativeDevice::CreateBuffer(uint64_t size, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES initial_state,
                                             D3D12_RESOURCE_FLAGS flags, const char *debug_name)
  {
    D3D12_HEAP_PROPERTIES props{};
    props.Type = heap;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = std::max<uint64_t>(size, 256);
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = flags;
    ID3D12Resource *resource = nullptr;
    pdz::timing::CountEvent(pdz::timing::Event::kResourceCreate); // pdz_frame_timing_spike_ms
    const pdz::timing::EventTimer create_time(pdz::timing::Event::kResourceCycles);
    const HRESULT hr = device_->CreateCommittedResource(&props, provider_->GetHeapFlagCreateNotZeroed(), &desc,
                                                        initial_state, nullptr, IID_PPV_ARGS(&resource));
    if (FAILED(hr))
    {
      REXLOG_ERROR("pdz_native: CreateCommittedResource(buffer {}, {} bytes) failed: 0x{:08X}", debug_name, size,
                   uint32_t(hr));
      return nullptr;
    }
    SetName(resource, debug_name);
    return resource;
  }

  // ---------------------------------------------------------------------------
  // native.h: frame bracket and guest physical memory
  // ---------------------------------------------------------------------------
  std::recursive_mutex &RenderMutex()
  {
    static std::recursive_mutex mutex;
    return mutex;
  }

  void RenderLock::LockContended(std::recursive_mutex &m)
  {
    // pdz_frame_timing_phases: the wait is charged once the mutex is held (the phase counters' single-writer rule).
    const bool timed = pdz::timing::PhaseLevel() > 0;
    const uint64_t t0 = timed ? __rdtsc() : 0;
    m.lock();
    if (timed)
    {
      pdz::timing::ChargeLocked(uint32_t(pdz::timing::Phase::kMutexWait), __rdtsc() - t0);
    }
  }

  void NoteRenderThread(const char *where)
  {
    // Cold except on a change: GetCurrentThreadId reads the TEB (no system call).
    static DWORD last = 0;
    static uint32_t changes = 0;
    const DWORD tid = GetCurrentThreadId();
    if (tid != last)
    {
      if (last != 0 && changes < 64)
      {
        REXLOG_INFO("pdz_native: guest D3D calls moved from thread {} to thread {} (at {}; handover #{})", last, tid,
                    where, changes + 1);
      }
      ++changes;
      last = tid;
    }
  }

  bool EnsureFrame()
  {
    RenderLock lock(RenderMutex());
    ArmPresenterOwner(); // before any native work touches the presenter (stage 1 report 2.2)
    if (!Enabled())
    {
      return false;
    }
    NativeDevice *d = NativeDevice::Get();
    if (!d)
    {
      return false;
    }
    if (!d->in_frame())
    {
      d->BeginFrame();
      PresentPendingFrame(); // pdz_native_present_mode 1 (may end this frame and begin another on the next call)
      if (!d->in_frame())
      {
        d->BeginFrame();
      }
    }
    return true;
  }

  uint8_t *PhysPtr(uint32_t physical_address)
  {
    static uint8_t *physical_base = nullptr;
    if (!physical_base)
    {
      rex::Runtime *runtime = rex::Runtime::instance();
      rex::memory::Memory *memory = runtime ? runtime->memory() : nullptr;
      physical_base = memory ? memory->physical_membase() : nullptr;
      if (!physical_base)
      {
        return nullptr;
      }
    }
    return physical_base + (physical_address & 0x1FFFFFFFu);
  }
} // namespace pdz::native
