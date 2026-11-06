// Fixes for the enemies' cover task (pdz_ai_cover_fix_*). See README.md.
#pragma once

#include <cstdint>
#include <string>

namespace ai_cover_fixes
{
  // Called from the cover task hooks in ai_cover_fixes.cpp (tu3-base).
  // After the constructor sub_821CDB88: re-rolls the peek variant.
  // Returns true if it changed the task.
  bool AfterCreate(uint8_t *base, uint32_t task, uint32_t point);

  // Before Update sub_821CE328: restarts the grace timers once the enemy has
  // settled at the point. Returns true when it did.
  bool BeforeUpdate(uint8_t *base, uint32_t task);

  // After Update: counts snap-backs and ends the task after too many in a row.
  // Returns true when it ended the task.
  bool AfterUpdate(uint8_t *base, uint32_t task);

  // End sub_821CE270: forget the task.
  void OnEnd(uint32_t task);

  // "grace, peek roll, snap-back" or "off", for log lines and the A/B label.
  std::string ActiveSummary();
} // namespace ai_cover_fixes
