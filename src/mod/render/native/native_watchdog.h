// Native renderer (stage 1): hang watchdog (pdz_native_watchdog_ms, off by default). Threads mark the blocking waits
// they enter (the guest D3D fence block, the presenter refresh, the native frame fences, the render mutex at an
// output toggle); a background thread reports any wait older than the threshold, a Swap that stopped coming, and the
// state around it: the native GPU fence (completed vs signalled, how long without progress), the presenter owner,
// a pending same-frame plugin capture and the device-removed reason. With pdz_native_watchdog_dump_s it also writes
// one minidump of every thread (logs\hang_*.dmp) once a wait or the Swap gap passes that many seconds: the GPU
// thread and the UI thread live in the SDK and are only visible there (run 197 hung without a dump).
// Written for the native-output device loss / hang at the Datacore level start (stage1\subagents\outrace.md).
#pragma once
#include <cstdint>

namespace pdz::native
{
  class NativeDevice;
}

namespace pdz::native::watchdog
{
  // pdz_native_watchdog_ms > 0 (one relaxed cvar load).
  bool Enabled();

  // Marks the calling thread as blocked in `what` (a string literal) until the scope ends; `detail` is printed with
  // it (a caller address, a fence value). Scopes nest: the innermost is reported and the outer one is restored at
  // its end. While the watchdog is on: a thread-local slot lookup, a few relaxed stores and GetTickCount64 (no
  // system call); while it is off, nothing.
  class Wait
  {
  public:
    explicit Wait(const char *what, uint64_t detail = 0);
    ~Wait();
    Wait(const Wait &) = delete;
    Wait &operator=(const Wait &) = delete;
    // A later phase of the same wait: new label and detail, the start time is kept.
    void Phase(const char *what, uint64_t detail = 0);

  private:
    int slot_ = -1;
    const char *prev_what_ = nullptr;
    uint64_t prev_detail_ = 0;
    uint64_t prev_since_ = 0;
  };

  // Every native OnSwap (the render mutex held; also while the renderer is off, dev = nullptr then): records the
  // swap for the report and starts the watchdog thread the first time the switch is on.
  void OnSwap(NativeDevice *dev);
} // namespace pdz::native::watchdog
