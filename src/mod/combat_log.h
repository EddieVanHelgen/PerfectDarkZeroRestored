// Combat aiming diagnostic (pdz_combat_log). See README.md next to this file.
#pragma once

#include <cstdint>

namespace combat_log
{
  // One auto-aim update of the player (from aim_assist.cpp's sub_824E2028
  // hook): where the player aims (before auto-aim), the selected target and
  // whether the shot is being bent onto it.
  struct AimFrame
  {
    uint32_t player = 0;
    uint32_t target = 0;       // autoaim +0x14 (0 = none)
    bool aim_valid = false;    // aim_dir is set
    float origin[3] = {};      // autoaim +0x40, cm
    float aim_dir[3] = {};     // unit vector of the unbent aim
    float target_point[3] = {}; // autoaim +0x24, when target != 0
    bool bending = false;      // the shot is bent onto the target
  };
  void OnAimUpdate(uint8_t *base, const AimFrame &frame);
} // namespace combat_log
