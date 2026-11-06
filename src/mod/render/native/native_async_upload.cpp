// Native renderer: asynchronous texture uploads (native_async_upload.h; pdz_native_async_upload, log 307).
#include "native_async_upload.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include <windows.h>

#include <rex/cvar.h>

#include "../cpu_placement.h" // pdz_background_priority
#include <rex/logging.h>

#include "native.h"
#include "native_device.h"

REXCVAR_DEFINE_BOOL(pdz_native_async_upload, true, "Render",
                    "Native renderer (log 307: a hitch every 10-60 s, the record thread creating 150-240 textures in one "
                    "frame): textures are prepared ahead of their draw on worker threads (resource created, guest data "
                    "untiled, copied on a dedicated copy queue with its own fence) as the record thread sees them coming "
                    "in its queue; at the draw the texture is taken from there (waiting only for that one if it isn't "
                    "ready) and the graphics queue waits on the GPU for that copy alone. Identical textures (same "
                    "untiler, key and content checks); one not prepared takes the old path. 'pdz_native: uploads' line "
                    "every 5 s. Read at startup (restart). Off = every texture created on the record thread at its draw, "
                    "as before");

namespace pdz::native::asyncup
{
  namespace
  {
    // A copy list and its allocator, reusable once the copy fence passes `fence`.
    struct ListSlot
    {
      ID3D12CommandAllocator *allocator = nullptr;
      ID3D12GraphicsCommandList *list = nullptr;
      uint64_t fence = 0;
    };
    // A persistently mapped upload buffer, reusable once the copy fence passes `fence`.
    struct Buffer
    {
      ID3D12Resource *resource = nullptr;
      uint8_t *cpu = nullptr;
      uint64_t size = 0;
      uint64_t used = 0;
      uint64_t fence = 0;
    };
    struct Deferred
    {
      ID3D12Resource *resource = nullptr;
      uint64_t fence = 0;
    };

    struct State
    {
      bool tried = false, ok = false;
      ID3D12Device *device = nullptr;
      ID3D12CommandQueue *queue = nullptr;
      ID3D12Fence *fence = nullptr;
      std::mutex submit_mutex; // ExecuteCommandLists + Signal in order; the pools
      uint64_t next_value = 1;
      std::vector<ListSlot> lists;   // free or in flight (fence)
      std::vector<Buffer> buffers;   // free (used == 0) or in flight (fence)
      uint64_t buffer_bytes = 0;
      std::vector<Deferred> deferred;
      // Workers
      std::mutex jobs_mutex;
      std::condition_variable jobs_cv;
      std::deque<Job *> jobs;
      std::vector<std::thread> workers;
      std::atomic<uint64_t> pending{0};
      // The graphics queue: the copy value the open list needs, and the last one waited for.
      uint64_t needed = 0, waited = 0;
      Counters counters;
      uint32_t list_waits_logged = 0;
    };
    State &g = *new State(); // never destroyed: the detached upload workers wait on it until the process ends

    constexpr uint64_t kBufferMin = 256ull << 10;   // upload buffers come in powers of two from here
    constexpr uint64_t kIdleBufferBytes = 256ull << 20; // free buffers kept above this are released

    uint32_t g_worker_count = 0;

    // Pools (submit_mutex held). Lists are referred to by index: the vector may grow under another worker.
    size_t TakeList(uint64_t completed)
    {
      for (size_t i = 0; i < g.lists.size(); ++i)
      {
        ListSlot &s = g.lists[i];
        if (s.fence != ~0ull && s.fence <= completed)
        {
          s.fence = ~0ull; // taken
          s.allocator->Reset();
          s.list->Reset(s.allocator, nullptr);
          return i;
        }
      }
      ListSlot s;
      if (FAILED(g.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&s.allocator))) ||
          FAILED(g.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, s.allocator, nullptr,
                                             IID_PPV_ARGS(&s.list))))
      {
        if (s.allocator)
        {
          s.allocator->Release();
        }
        return ~size_t(0);
      }
      s.fence = ~0ull;
      g.lists.push_back(s);
      return g.lists.size() - 1;
    }

    Buffer *TakeBuffer(uint64_t size, uint64_t completed)
    {
      uint64_t want = kBufferMin;
      while (want < size)
      {
        want <<= 1;
      }
      Buffer *best = nullptr;
      for (Buffer &b : g.buffers)
      {
        if (b.used == 0 && b.fence <= completed && b.size >= want && (!best || b.size < best->size))
        {
          best = &b;
        }
      }
      if (best)
      {
        best->used = 1; // taken
        return best;
      }
      Buffer b;
      D3D12_HEAP_PROPERTIES props{};
      props.Type = D3D12_HEAP_TYPE_UPLOAD;
      D3D12_RESOURCE_DESC desc{};
      desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
      desc.Width = want;
      desc.Height = 1;
      desc.DepthOrArraySize = 1;
      desc.MipLevels = 1;
      desc.SampleDesc.Count = 1;
      desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
      if (FAILED(g.device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                   nullptr, IID_PPV_ARGS(&b.resource))))
      {
        return nullptr;
      }
      D3D12_RANGE no_read{0, 0};
      if (FAILED(b.resource->Map(0, &no_read, reinterpret_cast<void **>(&b.cpu))))
      {
        b.resource->Release();
        return nullptr;
      }
      b.resource->SetName(L"pdz_native async upload");
      b.size = want;
      b.used = 1;
      g.buffer_bytes += want;
      g.buffers.push_back(b);
      return &g.buffers.back();
    }

    // The job's allocation: one buffer per job, sized at the first alloc (the untiler asks once, for the total).
    struct JobMemory
    {
      Buffer *buffer = nullptr;
      size_t index = 0;
    };

    void *AllocFor(CopyTarget &self, uint64_t size, ID3D12Resource **resource, uint64_t *offset)
    {
      JobMemory &m = *static_cast<JobMemory *>(self.opaque);
      std::lock_guard<std::mutex> lock(g.submit_mutex);
      if (m.buffer)
      {
        return nullptr; // one allocation per job
      }
      Buffer *b = TakeBuffer(size, g.fence->GetCompletedValue());
      if (!b)
      {
        return nullptr;
      }
      m.buffer = b;
      m.index = size_t(b - g.buffers.data());
      *resource = b->resource;
      *offset = 0;
      return b->cpu;
    }

    void RunJob(Job *job)
    {
      const auto t0 = std::chrono::steady_clock::now();
      job->state.store(1, std::memory_order_relaxed);
      size_t slot_index = ~size_t(0);
      {
        std::lock_guard<std::mutex> lock(g.submit_mutex);
        slot_index = TakeList(g.fence->GetCompletedValue());
      }
      bool ok = slot_index != ~size_t(0);
      JobMemory memory;
      if (ok)
      {
        CopyTarget target;
        target.device = g.device;
        {
          std::lock_guard<std::mutex> lock(g.submit_mutex);
          target.list = g.lists[slot_index].list;
        }
        target.alloc = &AllocFor;
        target.opaque = &memory;
        ok = job->Run(target);
        {
          std::lock_guard<std::mutex> lock(g.submit_mutex);
          ListSlot &s = g.lists[slot_index];
          s.list->Close();
          uint64_t value = 0;
          if (ok)
          {
            ID3D12CommandList *lists[1] = {s.list};
            g.queue->ExecuteCommandLists(1, lists);
            value = g.next_value++;
            g.queue->Signal(g.fence, value);
          }
          s.fence = ok ? value : 0; // a failed job's list was never executed: free now
          if (memory.buffer)
          {
            Buffer &b = g.buffers[memory.index];
            b.used = 0;
            b.fence = ok ? value : 0;
          }
          job->fence = value;
        }
      }
      const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
      job->cpu_ms = ms;
      g.counters.worker_us.fetch_add(uint64_t(ms * 1000.0), std::memory_order_relaxed);
      if (ok)
      {
        g.counters.jobs_done.fetch_add(1, std::memory_order_relaxed);
        g.counters.bytes.fetch_add(job->bytes, std::memory_order_relaxed);
      }
      else
      {
        g.counters.jobs_failed.fetch_add(1, std::memory_order_relaxed);
      }
      g.pending.fetch_sub(1, std::memory_order_relaxed);
      job->state.store(ok ? 2 : 3, std::memory_order_release);
      job->state.notify_all();
    }

    void WorkerMain(uint32_t index)
    {
      SetThreadDescription(GetCurrentThread(), (L"PDZ native upload " + std::to_wstring(index)).c_str());
      pdz::cpu::LowerBackgroundThread("upload worker"); // pdz_background_priority
      for (;;)
      {
        Job *job = nullptr;
        {
          std::unique_lock<std::mutex> lock(g.jobs_mutex);
          g.jobs_cv.wait(lock, [] { return !g.jobs.empty(); });
          job = g.jobs.front();
          g.jobs.pop_front();
        }
        RunJob(job);
      }
    }

    bool Start(NativeDevice &dev)
    {
      g.tried = true;
      if (!REXCVAR_GET(pdz_native_async_upload))
      {
        REXLOG_INFO("pdz_native: async uploads off (pdz_native_async_upload): textures are created on the record "
                    "thread at their draw");
        return false;
      }
      g.device = dev.device();
      D3D12_COMMAND_QUEUE_DESC qd{};
      qd.Type = D3D12_COMMAND_LIST_TYPE_COPY;
      if (!g.device || FAILED(g.device->CreateCommandQueue(&qd, IID_PPV_ARGS(&g.queue))) ||
          FAILED(g.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence))))
      {
        REXLOG_ERROR("pdz_native: async uploads: no copy queue / fence; textures are created on the record thread");
        return false;
      }
      g.queue->SetName(L"pdz_native copy queue");
      g.fence->SetName(L"pdz_native copy fence");
      // The worker pool: a quarter of the logical processors, 1 to 4 (the record thread, the game thread and the
      // GPU plugin's threads keep theirs).
      const uint32_t hw = std::max(1u, std::thread::hardware_concurrency());
      const uint32_t count = std::clamp(hw / 4u, 1u, 4u);
      g.lists.reserve(256);
      g.buffers.reserve(512);
      for (uint32_t i = 0; i < count; ++i)
      {
        g.workers.emplace_back(WorkerMain, i);
        g.workers.back().detach();
      }
      g.workers.clear(); // detached: they live for the process
      g.ok = true;
      g_worker_count = count;
      REXLOG_INFO("pdz_native: async uploads on (pdz_native_async_upload): {} upload worker thread(s) of {} logical "
                  "processors, a dedicated copy queue; textures prepared ahead of their draws",
                  count, hw);
      return true;
    }
  } // namespace

  bool Active(NativeDevice &dev)
  {
    if (!g.tried)
    {
      Start(dev);
    }
    return g.ok;
  }

  void Submit(Job *job)
  {
    job->state.store(0, std::memory_order_relaxed);
    g.pending.fetch_add(1, std::memory_order_relaxed);
    {
      std::lock_guard<std::mutex> lock(g.jobs_mutex);
      g.jobs.push_back(job);
    }
    g.jobs_cv.notify_one();
  }

  double Wait(Job &job)
  {
    uint32_t s = job.state.load(std::memory_order_acquire);
    if (s >= 2)
    {
      return 0.0;
    }
    const auto t0 = std::chrono::steady_clock::now();
    if (s == 0)
    {
      // Still queued: run it here rather than wait behind the others (it is the one this draw needs).
      bool mine = false;
      {
        std::lock_guard<std::mutex> lock(g.jobs_mutex);
        auto it = std::find(g.jobs.begin(), g.jobs.end(), &job);
        if (it != g.jobs.end())
        {
          g.jobs.erase(it);
          mine = true;
        }
      }
      if (mine)
      {
        RunJob(&job);
      }
    }
    while ((s = job.state.load(std::memory_order_acquire)) < 2)
    {
      job.state.wait(s, std::memory_order_acquire);
    }
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  }

  uint64_t CompletedFence() { return g.fence ? g.fence->GetCompletedValue() : 0; }

  void NeedFence(uint64_t value) { g.needed = std::max(g.needed, value); }

  void BeforeDirectSubmit(ID3D12CommandQueue *queue)
  {
    if (!g.ok || g.needed <= g.waited || !queue)
    {
      return;
    }
    if (g.fence->GetCompletedValue() < g.needed)
    {
      queue->Wait(g.fence, g.needed); // GPU side: the copies this list samples
    }
    g.waited = g.needed;
  }

  void ReleaseAfterCopy(ID3D12Resource *resource, uint64_t fence)
  {
    if (!resource)
    {
      return;
    }
    if (!g.ok || fence == 0 || g.fence->GetCompletedValue() >= fence)
    {
      resource->Release();
      return;
    }
    std::lock_guard<std::mutex> lock(g.submit_mutex);
    g.deferred.push_back({resource, fence});
  }

  Counters &Stats() { return g.counters; }

  void Tick()
  {
    if (!g.ok)
    {
      return;
    }
    std::lock_guard<std::mutex> lock(g.submit_mutex);
    const uint64_t completed = g.fence->GetCompletedValue();
    size_t kept = 0;
    for (size_t i = 0; i < g.deferred.size(); ++i)
    {
      if (g.deferred[i].fence <= completed)
      {
        g.deferred[i].resource->Release();
      }
      else
      {
        g.deferred[kept++] = g.deferred[i];
      }
    }
    g.deferred.resize(kept);
    // Free buffers beyond the idle budget go (largest first); in-flight and taken ones stay.
    if (g.buffer_bytes > kIdleBufferBytes)
    {
      uint64_t idle = 0;
      for (const Buffer &b : g.buffers)
      {
        idle += (b.used == 0 && b.fence <= completed) ? b.size : 0;
      }
      if (idle > kIdleBufferBytes && g.pending.load(std::memory_order_relaxed) == 0)
      {
        // Only with no job in flight: indices into the vector stay valid for running jobs otherwise.
        for (size_t i = g.buffers.size(); i-- > 0 && idle > kIdleBufferBytes;)
        {
          Buffer &b = g.buffers[i];
          if (b.used == 0 && b.fence <= completed)
          {
            idle -= b.size;
            g.buffer_bytes -= b.size;
            b.resource->Unmap(0, nullptr);
            b.resource->Release();
            g.buffers.erase(g.buffers.begin() + std::ptrdiff_t(i));
          }
        }
      }
    }
  }

  uint32_t WorkerCount() { return g_worker_count; }

  uint64_t PendingJobs() { return g.pending.load(std::memory_order_relaxed); }
} // namespace pdz::native::asyncup
