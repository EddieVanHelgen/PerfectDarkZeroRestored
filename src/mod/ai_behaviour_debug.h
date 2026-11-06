// Unused AI behaviours: the debug switch pdz_debug_ai_behaviour and the log
// pdz_ai_behaviour_log. tu3-base only. See docs/SP_INVENTORY.md, "Unused AI
// behaviours in depth".
#pragma once

#include <cstdint>

namespace ai_behaviour_debug
{
  // sub_821F00C8 picked an animation of this AnimType for the character
  // (called from anim_log's hook): counts the enemy's animated dodges.
  void OnAnimType(uint32_t character, uint32_t anim_type);

  // The move-into-cover task (vtable 82092950), from ai_cover_fixes' hooks:
  // created, settled at its cover point (walk and settle-in finished), a new
  // peek-and-shoot, ended. Each ended task is one pdz_ai_behaviour_log line.
  void OnCoverCreated(const uint8_t *base, uint32_t task);
  void OnCoverSettled(uint32_t task);
  void OnCoverPeek(uint32_t task);
  void OnCoverEnd(const uint8_t *base, uint32_t task);
} // namespace ai_behaviour_debug
