// Auto-aim tweaks (aim_assist.cpp).
#pragma once

#include <cstdint>

namespace aim_assist
{
  // pdz_autoaim_log: one "pdz_autoaim: shot" line for a hitscan shot by a
  // local player (shooter = the player or a weapon whose +0x5C is the
  // player), with that player's last auto-aim decision. Cheap when the log
  // is off. Called from combat_log.cpp's sub_82238938 hook.
  void OnShot(const uint8_t *base, uint32_t shooter);

  // The same from TU3's hitscan (sub_8223FC00: r3 = shooter, r5 = &origin, r6 = &aim point; called from
  // ai_behaviour_debug.cpp's hook), plus where the shot goes: the origin and the aim point (cm).
  void OnShotTU3(const uint8_t *base, uint32_t shooter, uint32_t origin, uint32_t aim_point);
} // namespace aim_assist
