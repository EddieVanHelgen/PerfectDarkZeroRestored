// Per-shot diagnostic (pdz_shot_log). See README.md next to this file.
#pragma once

#include <cstdint>

struct PPCContext;

namespace shot_log
{
  // Around sub_822397C8 (the shot's aim, game_fixes.cpp's hook), after any
  // fix has adjusted its spread argument.
  void BeforeAim(PPCContext &ctx, uint8_t *base);
  void AfterAim(PPCContext &ctx, uint8_t *base);

  // Around sub_82238938 (one hitscan shot, combat_log.cpp's hook).
  void BeforeShot(PPCContext &ctx, uint8_t *base);
  void AfterShot(PPCContext &ctx, uint8_t *base);

  // A character's damage handler is about to run (combat_log.cpp's hooks of
  // sub_8251EBB0 and sub_8259B738: f1 = damage).
  void OnDamage(uint32_t victim, double damage);
} // namespace shot_log
