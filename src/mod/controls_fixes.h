// Fixes for bugs in the original player controls (pdz_fix_*, master switch
// pdz_controls_fixes). See README.md and docs/BUG_AUDIT.md.
#pragma once

#include <cstdint>
#include <string>

struct PPCContext;

namespace controls_fixes
{
  // Option checks (each also needs the master switch).
  bool FastLookStick();
  bool FastLookDecel();
  bool SensitivityClamp();
  bool FastLookAccelData();
  bool SpreadRecoveryGate();

  // Around the player's on-foot look update (look_tuning.cpp, sub_825A42B0):
  // patches the PD look records for the decel and accel-data fixes, then
  // puts them back.
  struct LookPatch
  {
    uint32_t record[3] = {};
    uint32_t fast_look_accel[3] = {};
    bool active = false;
  };
  void BeforeLook(uint8_t *base, uint32_t player, uint32_t state, uint32_t input, LookPatch &patch);
  void AfterLook(uint8_t *base, uint32_t player, uint32_t state, const LookPatch &patch);

  // Horizontal sensitivity as returned by sub_8259D140 to the look models:
  // clamped so the x1.2 (Fast Look off) and x1.75 boosts can't push the blend
  // past the max record. lr = the caller's return address.
  double ClampHorizontalSensitivity(uint32_t lr, double value, bool game_applies_fast_look_off_bonus);

  // "fast-look stick, decel, sens clamp, accel data, spread gate" or "off".
  std::string ActiveSummary();
} // namespace controls_fixes
