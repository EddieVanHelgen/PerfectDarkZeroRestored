// GPU synchronisation log and the null GPU backend switch (gpu_sync.cpp; STAGE2_PLAN.md M1).
//
// graphics_backend null: the Xenos GPU plugin's null backend (SDK rex/graphics/null_gpu.h) consumes the PM4 ring and
// services every synchronisation but renders nothing, so the native renderer is the only output. NullBackend() is
// latched once by the app's OnPreSetup (OnBackendSelected) before the guest runs; with it the shadow recorder, the
// native renderer and its output are on for the session, F9 and the per-frame shader-miss fallback are off, and the
// plugin-only tools (auto-diff, pass dump, draw dump) refuse to start.
//
// pdz_sync_log (on by itself with the null backend): "pdz_sync:" lines that make a hang diagnosable from the log alone,
// in the order the stage-2 plan checks them: fences (the first D3D fence block, BlockUntilIdle returns, the fence the
// GPU wrote and its rate), the Swap / presentation-interval gate (first guest Swap in / out, Swap rate), interrupts
// (InsertCallback calls; the SDK's null_gpu lines have the CP side), vblank timing (D3D's vblank count and its rate) and
// the de-rez readback (the native part of InsertCallback). A heartbeat every second for 20 s then every 10 s, a STALL
// line when the Swap, a fence block or BlockUntilIdle stops, and a SUMMARY at test exit.
#pragma once
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

#include <rex/cvar.h>

REXCVAR_DECLARE(bool, pdz_sync_log);

namespace pdz::gpusync
{
  // Steady-clock microseconds for the hook timings below.
  inline int64_t NowUs()
  {
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
  }

  // Written once by OnBackendSelected (app setup, before any guest thread exists); read by every hook.
  inline bool g_null_backend = false;
  inline bool NullBackend() { return g_null_backend; }

  // perfectdarkzerorecomp_app.h OnPreSetup, after the plugin was asked for `backend`: latches NullBackend when the
  // plugin really created its null backend and logs the session policy (or why null is not in effect).
  void OnBackendSelected(std::string_view backend, bool plugin_configured, bool plugin_created);

  inline bool Enabled() { return g_null_backend || REXCVAR_GET(pdz_sync_log); }

  // perfectdarkzerorecomp_app.h OnShutdown: stops the native record thread (pdz_native_record_thread), if running.
  void OnAppShutdown();

  // Hook entry points (call only when Enabled()). base = the guest memory base the hooks get; device = r3.
  void OnSwapEnter(uint8_t *base, uint32_t device);
  void OnSwapLeave();
  void OnBlockUntilIdleEnter(uint8_t *base, uint32_t device, uint32_t lr);
  void OnBlockUntilIdleLeave(uint32_t lr, int64_t us);
  // sub_826DE258(device r3, pointer to the awaited fence value r4).
  void OnFenceBlockEnter(uint8_t *base, uint32_t device, uint32_t awaited_ptr, uint32_t lr);
  void OnFenceBlockLeave(int64_t us);
  void OnInsertCallbackEnter(uint8_t *base, uint32_t device, uint32_t lr, uint32_t type, uint32_t callback);
  // native_us: the native part before the guest body (the de-rez snapshot readback), total_us: the whole hook.
  void OnInsertCallbackLeave(int64_t native_us, int64_t total_us);

  // The D3D synchronisation state now (fence the GPU wrote / issued / awaited, scratch block, D3D vblank count, the
  // threads in a fence block or BlockUntilIdle), for hang reports (native_watchdog.cpp). Any thread; guest memory is
  // read only where committed. Empty before the first hook saw a device.
  std::string D3DState();

  // Test exit (pdz::test::LogSummary): the SUMMARY line (when Enabled()); stops the heartbeat.
  void LogSummary(const char *why);
} // namespace pdz::gpusync
