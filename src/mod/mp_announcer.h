// Unused multiplayer announcer lines (pdz_mp_announcer_extras). tu3-base; see
// mp_announcer.cpp.
#pragma once

#include <cstdint>

#include <rex/ppc/func.h>

namespace mp_announcer
{
  // The cue name to look up instead of `name` (Sabotage's per-target lines,
  // Plant the Flag's enemy-planted line), or `name`. From the name lookup
  // hook (sub_82202D20, cut_scenarios.cpp).
  uint32_t CueName(PPCContext &ctx, uint8_t *base, uint32_t name);

  // Plant the Flag (cut_scenarios.cpp): a bot of `team` starts planting; a
  // plant scored for `participant`'s team; the mode's update ran (human plant
  // starts, the deferred enemy-planted line).
  void BotPlantStarted(PPCContext &ctx, uint8_t *base, int team);
  void PlantScored(PPCContext &ctx, uint8_t *base, uint32_t participant);
  void PlantTheFlagTick(PPCContext &ctx, uint8_t *base, uint32_t mode);

  // Queues one of the announcer's cues by its full name (Duel's round award,
  // cut_scenarios.cpp); `why` goes to the log when the cue isn't loaded.
  void Line(PPCContext &ctx, uint8_t *base, const char *cue, const char *why);
} // namespace mp_announcer
