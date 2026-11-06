// Animation log (pdz_anim_log) and the character tracking it shares with the
// animation viewer (pdz_debug_anim_viewer). tu3-base only.
#pragma once

#include <cstdint>
#include <string>
#include <unordered_set>

namespace anim_log
{
  // Either the log or the viewer is on (the hooks track characters then).
  bool Tracking();

  // The character has started a character animation (aid_anim_chars_*, not
  // the pigeon set) and still has the vtable it had then: a human skeleton.
  bool IsHuman(const uint8_t *base, uint32_t character);

  // Every animation name seen so far (lower case): logs/anim_seen.csv from
  // earlier sessions plus this one.
  std::unordered_set<std::string> SeenNames();
} // namespace anim_log
