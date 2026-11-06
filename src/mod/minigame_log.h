// Minigame calibration log (pdz_minigame_log): see minigame_log.cpp.
#pragma once

#include <cstdint>

#include <rex/ppc/func.h>

namespace minigame_log
{
  // A cue looked up by name (sub_82202D20, hooked in cut_scenarios.cpp):
  // demo kit presses and success come from cues.
  void OnCue(PPCContext &ctx, uint8_t *base, uint32_t name);
} // namespace minigame_log
