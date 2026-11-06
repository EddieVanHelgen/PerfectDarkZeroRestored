// Native renderer: asynchronous texture uploads (pdz_native_async_upload, log 307).
//
// Log 307 (Nightclub at 120): a single hitch every 10-60 s, the record thread spending the frame creating textures
// (147-241 in one frame, 16-19 MB; 30-46 ms of it in CreateCommittedResource, the rest untiling), the game thread
// waiting for it. Here, a texture the record thread will need is prepared ahead of its draw on worker threads: the
// resource is created, the guest data untiled into an upload buffer of the job's own, and the copy executed on a
// dedicated COPY queue with its own fence. The record thread finds the prepared texture at its draw (waiting only for
// that one when it isn't done yet) and makes the graphics queue wait (GPU side) for that copy's fence value before the
// frame's command list runs. The content, format and mips are exactly those of the synchronous path (the same untiler,
// the same key and content checks); a texture nobody prepared still takes the synchronous path.
#pragma once

#include <atomic>
#include <cstdint>

#include <d3d12.h>

namespace pdz::native
{
  class NativeDevice;
}

namespace pdz::native::asyncup
{
  // Where a job's untiler writes: a slice of the job's own upload buffer, and the copy list to record into.
  struct CopyTarget
  {
    ID3D12Device *device = nullptr;
    ID3D12GraphicsCommandList *list = nullptr;
    // Upload memory for `size` bytes (512-byte aligned); null cpu = failed.
    void *(*alloc)(CopyTarget &self, uint64_t size, ID3D12Resource **resource, uint64_t *offset) = nullptr;
    void *opaque = nullptr; // the module's state for alloc
  };

  // One prepared upload. Run() executes on a worker; the module submits its copy list and publishes `fence`.
  struct Job
  {
    virtual ~Job() = default;
    // The CPU work: returns false when the job failed (nothing to copy).
    virtual bool Run(CopyTarget &target) = 0;
    // 0 queued, 1 running, 2 done (fence valid), 3 failed. Waited on with std::atomic::wait.
    std::atomic<uint32_t> state{0};
    uint64_t fence = 0;      // the copy queue's value once the copy is submitted (state 2)
    uint64_t bytes = 0;      // upload bytes (set by Run)
    double cpu_ms = 0.0;     // worker CPU time (Run + submit)
    uint64_t queued_frame = 0;
  };

  // pdz_native_async_upload and the module started (copy queue, workers). The first call starts it.
  bool Active(NativeDevice &dev);
  // Hands a job to the workers (record thread). The caller keeps ownership; Wait before touching or deleting it.
  void Submit(Job *job);
  // Waits until the job is done or failed (record thread). Returns the wait in ms (0 when it was ready).
  double Wait(Job &job);
  // The copy fence value known complete (cheap: the fence's GetCompletedValue).
  uint64_t CompletedFence();
  // The graphics queue must wait for this copy fence value before the open list executes.
  void NeedFence(uint64_t value);
  // Called right before each ExecuteCommandLists of the frame's list on the direct queue: inserts the queue Wait for
  // the copies this list depends on (no-op when none).
  void BeforeDirectSubmit(ID3D12CommandQueue *queue);
  // A texture resource whose copy may still be on the copy queue: released once its fence completes.
  void ReleaseAfterCopy(ID3D12Resource *resource, uint64_t fence);

  // Statistics (the 5 s line, written by the record thread at frame end).
  struct Counters
  {
    std::atomic<uint64_t> jobs_done{0}, jobs_failed{0}, bytes{0}, worker_us{0};
  };
  Counters &Stats();
  // Frame end (record thread): recycles finished lists / buffers, releases deferred resources.
  void Tick();
  uint32_t WorkerCount();
  uint64_t PendingJobs();
} // namespace pdz::native::asyncup
