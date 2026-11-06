// The per-frame cutscene dump (pdz_cutscene_frame_dump, default off; read-only diagnostics). Implemented in
// cutscene_smooth.cpp; these are its taps elsewhere. Each is one relaxed load when the dump isn't collecting.
#pragma once

#include <cstdint>

namespace pdz::csdump
{
  // shadow_hooks.cpp, every guest Swap (game thread), after the guest's Swap returned: counts the Swaps (the same
  // count as the native renderer's OnSwap, native_present.cpp g_present.swaps) and closes the frame's row.
  void OnSwap(uint8_t *base);
  // native_present.cpp, after a frame's output was handed to the presenter (RefreshGuestOutput returned; present
  // timing 0): the native Swap number of that frame and the time.
  void OnPresent(uint64_t swap);
  // sun_shadows.cpp PdzShadowCasterBox (the shadow setup sub_8260C0D8 at 8260C204, game thread, in the frame's render):
  // r28 = the camera matrix the frame's view is rendered with (4x4 floats).
  void OnShadowCamera(uint32_t matrix);
  // shadow_hooks.cpp, every guest Swap (game thread), BEFORE the Swap is recorded / forwarded: while a cutscene plays,
  // a Swap without a cutscene update since the previous Swap is a frame the game didn't draw (log 292: about one per
  // block, black); it is counted for the per-cutscene line and, with pdz_cutscene_repeat_frame, marked so the
  // presenter shows the previous frame again for it (never two in a row). One load outside cutscenes.
  void BeforeSwap(uint8_t *base);
  // native_present.cpp PresentFrame (record thread): true for the Swap number BeforeSwap marked (one atomic load).
  bool SkipPresent(uint64_t swap);
  // pdz_cutscene_repeat_frame, night of 31 Oct (native_present.cpp, record thread): true when this Swap was a cutscene
  // Swap with pdz_cutscene_smooth on (BeforeSwap marks them); the presenter shows the previous frame for such a Swap
  // with no draws (retime's empty frames, dump native_draws 0) and counts it here.
  bool CutsceneSwap(uint64_t swap);
  void NoteEmptyRepeat();
  // pdz_cutscene_black_detect (overnight 30-31 Oct, off by default): native_present.cpp asks before each present
  // whether this Swap's front buffer should be sampled (the dump is collecting and the detector is on), and hands the
  // brightness of an 8 x 8 grid of its pixels back a few frames later (0-255 luma: average and brightest).
  bool WantBrightness(uint64_t swap);
  // draws: the native renderer's draws issued since the previous sampled frame (this frame's; night of 31 Oct).
  void OnBrightness(uint64_t swap, float average, float brightest, uint32_t draws);
} // namespace pdz::csdump
