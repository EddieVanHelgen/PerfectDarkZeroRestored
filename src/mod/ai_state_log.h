// pdz_ai_state_log counters fed from hooks in other files.
#pragma once

#include <cstdint>

namespace ai_state_log
{
  // The dodge task (sub_821CB0C0) for this character: its 2-in-3 roll chose
  // a relocation, or it tried an animated dodge, and whether the clearance
  // ray (sub_821CAC10) found room.
  enum class Dodge
  {
    Relocate,
    Animate,
    AnimateClear,
  };
  void OnDodge(uint32_t character, Dodge what);

  // The cover pad filter (sub_82155EB8, r3 = pad, r4 = search context) has
  // run: whether the pad passed in the end, and whether
  // pdz_fix_ai_cover_min_distance rejected it for being 10 m or less from
  // the target.
  void OnCoverPick(const uint8_t *base, uint32_t search, uint32_t pad, bool passed, bool rejected);
} // namespace ai_state_log
