// Native renderer (stage 1): auto-diff of the native and emulated frames of one guest swap (native_autodiff.cpp).
#pragma once
#include <cstdint>
#include <string>

#include "native_textures.h"

namespace pdz::native
{
  class NativeDevice;
}

namespace pdz::native::autodiff
{
  bool Enabled();
  // Every Swap (before anything else), so the index stays equal to the GPU plugin's IssueSwap count.
  void CountSwap();
  // pdz_native_capture_swaps (native_cvars.cpp): capture points at exact swap numbers. SwapPointsSet: the cvar names
  // at least one point (parsed and logged at the first call). DueSwapPoint, once per Swap after CountSwap: the swap
  // number when the next swap is a point (a Request now captures exactly that swap), else 0; points that went by
  // without a call (the renderer was off) are logged as skipped.
  bool SwapPointsSet();
  uint64_t DueSwapPoint();
  // A requested capture is not scored (or dropped) yet: a Request now would do nothing (one at a time).
  bool CaptureInFlight();
  // A capture tick: ask the plugin for the next swap's output and read our own frame at that swap.
  void Request();
  // Drops the pending request, the plugin's and ours (native_present.cpp: the native output gave up holding for it).
  void CancelRequest();
  // The gamma LUT's staging SRV (native_present.cpp), made or refreshed on the open command list; ptr 0 when it could
  // not be made. Called only at the readback of the native output chain (pdz_native_autodiff_post_effect after /
  // before).
  using LutSource = D3D12_CPU_DESCRIPTOR_HANDLE (*)(NativeDevice &dev);
  // Every Swap with the native frame still open (front = the swap's front buffer; ramp = the active gamma ramp for the
  // CPU readback of mode none).
  void OnSwap(NativeDevice &dev, const FetchDecoded &front, bool front_valid, bool frame_complete,
              const uint16_t (*ramp)[256], LutSource lut);
  std::string Summary();
  // Memory log: what the auto-diff keeps between captures (its readback buffer, the native image and the output
  // chain's texture, sized for the largest front buffer read so far).
  uint64_t KeptBytes();

  // Swaps seen (CountSwap): a request made now captures swap SwapCount() + 1.
  uint64_t SwapCount();

  // Mark-this-spot (bind_mark_spot, native_present.cpp): one capture of swap SwapCount() + 1 at full resolution, the
  // native frame (the host front buffer through the output chain with the native output's swap post effect: what the
  // window shows for native) and / or the emulated one (the GPU plugin's guest output of the same swap, taken like the
  // auto-diff's: after the plugin's swap post effect), written as PNG on a writer thread; `line` is logged there with
  // the files appended. One at a time, never beside an auto-diff capture (MarkBusy; the auto-diff skips its ticks
  // while a mark is in flight).
  struct MarkRequest
  {
    uint32_t number = 0;
    bool native = false;   // read the native frame at the swap (the native renderer is on)
    bool emulated = false; // ask the GPU plugin for its frame of the swap
    std::string native_path, emulated_path;
    std::string line;             // "pdz_mark: #n ..." up to the files
    std::string emulated_skipped; // why the emulated frame is not asked for (appended to the files), or empty
  };
  // A mark or an auto-diff capture is in flight, or another plugin capture is pending: a new mark waits.
  bool MarkBusy();
  void StartMark(MarkRequest request);
  // Every Swap after CountSwap, renderer on or off: takes the plugin's capture, gives up on a side that cannot come,
  // and hands a complete mark to its writer thread.
  void MarkOnSwap();
  // The native frame of the pending mark is due at this swap (call MarkReadNative with the frame still open).
  bool MarkNativeDue();
  void MarkReadNative(NativeDevice &dev, const FetchDecoded &front, bool front_valid, const uint16_t (*ramp)[256],
                      LutSource lut);
} // namespace pdz::native::autodiff
