// Native renderer, stage 2 M5 step 4 (pdz_native_record_thread, default off; native_recthread.cpp): the native work
// of the D3D hooks on one worker thread ("record thread"). The game thread records each hooked call (the shadow
// recorder's DrawRecord plus the device fields and caller memory the record lacks: native_view.h) and queues one
// command; the worker runs the unchanged native entry points in the same order. Every CPU write of CP_RB_WPTR (the
// guest's kick) is queued too and reaches the command processor only after the work queued before it (SDK sdk-16,
// rex/graphics/null_gpu.h SetWritePointerFilter): the GPU progress the guest observes (fences, read pointer,
// interrupts) never runs ahead of the worker, so memory the guest reuses on that progress is read in time.
// Design, the list of guest-memory reads and how each is made safe: PDZRenderResearch\native\stage2\m5\
// m5_record_thread.md.
//
// Every hook entry below is called from shadow_hooks.cpp (and the InsertCallback / SetGammaRamp hooks) only while
// Active(); each one does what the hook did under the render mutex before (Count, Record, the native call), with
// the native call replaced by a queued command.
#pragma once
#include <cstdint>

#include <rex/cvar.h>

#include "../shadow_record.h"

REXCVAR_DECLARE(bool, pdz_native_record_thread);
REXCVAR_DECLARE(bool, pdz_native_record_thread_verify);

namespace pdz::native::recthread
{
  // App setup (gpu_sync.cpp OnBackendSelected, before any guest thread): with pdz_native_record_thread on, allocates
  // the queue, starts the worker and installs the write pointer filter. Logs one 'pdz_recthread: on' line.
  void ChooseAtStartup();

  // True while the worker takes the commands (one relaxed load).
  bool Active();

  // Game-thread hooks (shadow_hooks.cpp). `hook` is the counter Count() increments.
  // Draws other than BeginVertices: before forwarding. a5..a7 only for DrawIndexedVerticesUP (r9, r10, the stride).
  void OnDraw(uint8_t *base, pdz::shadow::Hook hook, pdz::shadow::Kind kind, uint32_t device, uint32_t caller,
              uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5, uint32_t a6, uint32_t a7);
  // BeginVertices: Enter before forwarding (the previous batch is complete: FlushPending; the record), Leave after it
  // with the returned data pointer (r3): the draw command. Enter returns the record's sequence for Leave.
  uint32_t OnBeginVerticesEnter(uint8_t *base, uint32_t device, uint32_t caller, uint32_t a0, uint32_t a1,
                                uint32_t a2);
  void OnBeginVerticesLeave(uint8_t *base, uint32_t sequence, uint32_t data);
  // EndVertices (after forwarding), SetRenderTarget / SetDepthStencilSurface (before forwarding): the pending batch.
  void OnEndVertices(uint8_t *base, uint32_t device);
  void OnFlushPoint(uint8_t *base, uint32_t device);
  // Clear: before forwarding (count / rects / flags / colour / Z bits / stencil as the hook passes them).
  void OnClear(uint8_t *base, uint32_t device, uint32_t caller, uint32_t count, uint32_t rects, uint32_t flags,
               uint32_t color, uint32_t z_bits, uint32_t stencil);
  // Resolve: Enter (the record) before forwarding, Leave (the native resolve) after it.
  void OnResolveEnter(uint8_t *base, uint32_t device, uint32_t caller, uint32_t flags, uint32_t src_rect,
                      uint32_t dest_texture, uint32_t dest_point, uint32_t level, uint32_t z_bits);
  void OnResolveLeave(uint8_t *base, uint32_t device, uint32_t flags, uint32_t src_rect, uint32_t dest_texture,
                      uint32_t dest_point, uint32_t level, uint32_t slice, uint32_t clear_color_ptr, uint32_t z_bits,
                      uint32_t call_site);
  // Swap: Enter = the record (before forwarding), Leave = the native OnSwap (present), after the guest Swap and the
  // shadow's own OnSwap; waits until the worker has finished the previous Swap (run-ahead at most one frame).
  void OnSwapEnter(uint8_t *base, uint32_t device, uint32_t caller, uint32_t front_buffer, uint32_t parameters);
  void OnSwapLeave(uint8_t *base, uint32_t device, uint32_t front_buffer);
  // InsertCallback (native_surfaces.cpp hook, before forwarding): the de-rez snapshot readback. callback = r5
  // (pdz_native_async_readback: only the de-rez callback's readback is asynchronous).
  void OnInsertCallback(uint8_t *base, uint32_t callback);
  // SetGammaRamp (native_present.cpp hook): the 1536-byte ramp at `ramp`.
  void OnGammaRamp(uint8_t *base, uint32_t ramp);
  // The guest locked a texture for writing (native_textures.h OnTextureLock): Textures::NoteCpuWrite on the worker, in
  // order with the draws (pdz_native_texture_lock_check).
  void OnTextureWrite(uint8_t *base, uint32_t begin, uint32_t end, uint32_t mip_begin, uint32_t mip_end);

  // The executing command's game-thread Swap time (gpusync::NowUs), 0 outside a Swap command: native OnSwap measures
  // its present delay from it.
  int64_t CurrentSwapUs();

  // Waits until the worker has done everything queued (test exit: before the SUMMARY lines; bounded at 5 s).
  void Drain(const char *why);
  // App shutdown: drains, stops the worker and removes the write pointer filter; later hooks run synchronously.
  void Stop();

  // frame_timing.cpp spike log (pdz_frame_timing_spike_ms): the record thread's cumulative counters (the Swap takes
  // deltas per interval) and the maxima since the previous call (reset by it). False while the record thread is off.
  struct SpikeTotals
  {
    uint64_t busy_ns = 0;           // worker batches' wall time (the commands' work, render mutex held)
    uint64_t commands = 0;          // commands done by the worker
    uint64_t wait_us[3] = {};       // game-thread waits for the worker: room, swap bound, drain (WaitKind order)
    uint64_t waits[3] = {};         //   how many blocked
    uint64_t arena_waits = 0;       // room waits for payload arena bytes (in wait_us[0])
    uint64_t arena_wraps = 0;       // the arena head skipped the arena's tail (payloads are contiguous)
    uint64_t inline_runs = 0;       // oversize payloads run on the producer thread after a drain
    uint64_t payload_bytes = 0;     // payload bytes queued
    // Since the previous call:
    uint64_t depth_max = 0;         // queue depth at enqueue (commands in flight)
    uint64_t longest_cycles = 0;    // the worker's longest single command (TSC cycles; while the spike log is on)
    const char *longest_type = "-"; // its command type
  };
  bool TakeSpikeTotals(SpikeTotals &out);

  // frame_timing.cpp: the 'pdz_timing: record thread' line for the swaps [first, last] (window / SUMMARY).
  void LogRange(const char *kind, uint64_t first_swap, uint64_t last_swap, double frame_ms_avg);
} // namespace pdz::native::recthread
