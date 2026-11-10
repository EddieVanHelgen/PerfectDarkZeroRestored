// Laboratory | Rescue restorations (pdz_lab_*): see lab_restorations.cpp.
#pragma once

#include <cstdint>

struct PPCContext;

namespace lab_restorations
{
  // The Laboratory handler's five 64-bit message words (r5..r9 of
  // sub_823272E0: the XOR of the level's 320-bit flag set against last
  // frame's copy; flag n = word n/32, bit n%32; words 0, 2, 4, 6, 8 in the
  // high halves).
  struct Message
  {
    uint64_t r5 = 0, r6 = 0, r7 = 0, r8 = 0, r9 = 0;
  };

  // Did flag n change in this message?
  bool Changed(const Message &m, unsigned flag);

  // Called by restorations.cpp's hook on sub_823272E0 before and after
  // Rare's handler runs (script = r3).
  void BeforeLabMessage(PPCContext &ctx, uint8_t *base, uint32_t script, const Message &m);
  void AfterLabMessage(PPCContext &ctx, uint8_t *base, uint32_t script, const Message &m);

  // pdz_lab_blackout_lines: is this position (cm) inside one of the four
  // light-group boxes the blackout darkens?
  bool InDarkenedBox(const float pos[3]);

  // pdz_lab_blackout_coverage as this run of the Laboratory uses it: the
  // level latched at the blackout, else the cvar (1..3). At 3 every room is
  // dark and the box filter above no longer applies.
  int BlackoutCoverage();

  // The Laboratory script object as Rare's own setup / tick hand it to us (r3), or 0 when the Laboratory's tick has
  // not run in the last half second (another level, a menu): for code that runs outside the script's own calls.
  uint32_t CurrentScript();
} // namespace lab_restorations
