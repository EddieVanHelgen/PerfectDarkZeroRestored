// Fixes for bugs in the original game (pdz_fix_*, master switch
// pdz_game_fixes). See README.md and docs/BUG_AUDIT.md. tu3-base: only the
// fixes the launch build keeps and TU3 still needs.
#pragma once

#include <cstdint>
#include <string>

namespace game_fixes
{
  // Around the player's on-foot look update (look_tuning.cpp, sub_825B3AC0):
  // patches the Halo look record for pdz_fix_halo_zoom_curve and puts it back.
  struct LookPatch
  {
    uint32_t field = 0; // address patched, 0 = none
    uint32_t saved = 0;
  };
  void BeforeLook(uint8_t *base, uint32_t player, uint32_t input, LookPatch &patch);
  void AfterLook(uint8_t *base, const LookPatch &patch);

  // "halo zoom curve, spread cap order" or "off", for the A/B label.
  std::string ActiveSummary();
} // namespace game_fixes
