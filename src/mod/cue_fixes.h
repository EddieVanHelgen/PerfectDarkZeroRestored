// Speech and sound names (pdz_fix_speech_names, pdz_fix_sound_names): cues the
// game asks for under a name its banks don't have, mapped to Rare's recorded
// ones. tu3-base; see cue_fixes.cpp.
#pragma once

#include <cstdint>

#include <rex/ppc/func.h>

namespace cue_fixes
{
  // The cue name to look up instead of `name` (a guest string), or `name`.
  // Called from the name lookup hook (sub_82202D20, cut_scenarios.cpp).
  uint32_t CueName(PPCContext &ctx, uint8_t *base, uint32_t name);
} // namespace cue_fixes
