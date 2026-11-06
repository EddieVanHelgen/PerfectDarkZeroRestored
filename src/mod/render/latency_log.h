// Input-to-screen latency of the game's reactions (pdz_latency_log, default off). latency_log.cpp.
//
// The chain of one input change, each step timed on the host clock:
//   1. the pad read that first sees the change (the game's XInputGetState, test_harness.cpp; with the SDK's
//      input_poll_on_read the host polls the device inside that read, so this is also the host poll that saw it):
//      the trigger pulled (fire) or the right stick pushed past half (turn), on pad 0;
//   2. the first frame whose game state reacts, sampled at the Swap hook after the guest Swap (shadow_hooks.cpp):
//      the player's view yaw / pitch (player +5744 / +5748) moving faster than before the change, or a hitscan shot by
//      the player (combat_log.cpp's sub_82238938 hook);
//   3. that frame's native submit (native_present.cpp, after the output chain's EndFrame);
//   4. its host present and its refresh on screen (DXGI frame statistics): the SDK's frame_pacing trace carries the
//      mark with the frame (rex::ui::frame_pacing::MarkReaction) and logs 'frame_pacing: reaction #N ...' lines.
// pdz_latency_log turns frame_pacing_log on (it needs the trace). One 'pdz_latency:' line per reaction and a summary
// every 10 s (p50 / p95 / max); the SDK's summary adds "input change -> present / screen".
#pragma once

#include <cstdint>

namespace pdz::latency
{
  bool Enabled();
  // test_harness.cpp, XInputGetState(0) after the script: the state block the game reads (XINPUT_STATE, big-endian).
  void OnPadRead(const uint8_t *base, uint32_t state);
  // look_tuning.cpp, the on-foot look update: the player whose pad is 0.
  void OnPlayerLook(const uint8_t *base, uint32_t player);
  // combat_log.cpp, a hitscan shot (sub_82238938 r3: the shooter or its weapon).
  void OnShot(const uint8_t *base, uint32_t shooter);
  // shadow_hooks.cpp, every Swap after the guest's Swap (VdSwap done), game thread.
  void OnSwap(const uint8_t *base);
  // native_present.cpp, after a frame's output chain was submitted; swap = the native Swap count of that frame.
  void OnNativeSubmit(uint64_t swap);
  // The SUMMARY line (test exit).
  void LogSummary(const char *why);
} // namespace pdz::latency
