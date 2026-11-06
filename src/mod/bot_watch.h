// "watch:" bot diagnostic (pdz_scenario_log; 2026-10-25: spectated bots
// twitch). The bot the player spectates (Rare's spectate target), else the
// nearest bot within 40 m, is sampled each brain frame; a burst of per-frame
// lines (5 s) starts when its facing turns more than 30 degrees within
// 0.25 s, or its move target / route / goal / activity / gun state changes
// more than twice in a second. At most 2 bursts per bot per minute.
#pragma once

#include <cstdint>
#include <string>

#include <rex/ppc/func.h>

namespace bot_watch
{
  // The watched bot character (0: none). Cheap: hooks call it every time.
  uint32_t Watched();

  // Something wrote to the watched bot (our hooks by name, Rare's code by
  // caller); shown on its next sampled frame. Ignored for other characters.
  void Note(uint32_t character, const std::string &what);

  // Each bot brain frame (objective_bots.cpp, after Rare's update or its
  // skip): picks the watched bot and samples it.
  void Frame(PPCContext &ctx, uint8_t *base, uint32_t character, bool brain_skipped);
} // namespace bot_watch
