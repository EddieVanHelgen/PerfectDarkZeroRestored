// Auto-aim tweaks for the Title Update 3 build (tu3-base). Only what the
// launch build keeps on is ported: the pdz_autoaim_tuning master switch with
// pdz_aim_still_lock, pdz_aim_respect_headshots and pdz_aim_assist_zoomed
// (docs/TU3_BASE.md, re-targeting checklist). The launch branch's cone,
// strength, enable, ignore-difficulty, friction and log options aren't here.
// Addresses are TU3's; launch addresses in brackets.
//
// The player's auto-aim (player +0x1F60, updated by sub_824EF078
// [824E2028]) never turns the camera. Each frame its target search picks a
// visible character near the reticle and, if the target's aim point is within
// the lock radius, moves the aim (+0x38/+0x3C) from where the player aims
// (+0x30/+0x34) onto the target. The caller (sub_825B2D08 [825A34F0]) hands
// it only to the weapons' aim: the shot bends to the target, the drawn
// crosshair stays put. The object's layout is the same in TU3.
#include <cstdint>
#include <cstring>

#include <rex/hook.h>

#include "../game_cvars.h"
#include "mouse_look.h" // pdz_mouse_aim_assist_off: no auto-aim while the mouse is the last device
#include "hook_profile.h" // pdz_hook_profile (must follow <rex/hook.h>)

REX_EXTERN(__imp__sub_824EF078);
REX_EXTERN(__imp__sub_825B2D08);
REX_EXTERN(__imp__sub_824F0BB8);
REX_EXTERN(__imp__sub_825AC080);

namespace
{
  uint32_t LoadBe32(const uint8_t *base, uint32_t address)
  {
    const uint8_t *p = base + address;
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
  }

  void StoreBe32(uint8_t *base, uint32_t address, uint32_t value)
  {
    uint8_t *p = base + address;
    p[0] = uint8_t(value >> 24);
    p[1] = uint8_t(value >> 16);
    p[2] = uint8_t(value >> 8);
    p[3] = uint8_t(value);
  }

  float LoadBeFloat(const uint8_t *base, uint32_t address)
  {
    const uint32_t bits = LoadBe32(base, address);
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
  }

  void StoreBeFloat(uint8_t *base, uint32_t address, float value)
  {
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    StoreBe32(base, address, bits);
  }

  // Heap and physical allocations both sit above the 1 GB mark.
  bool IsGuestPointer(uint32_t address)
  {
    return address >= 0x40000000u && address < 0xFFFF0000u;
  }

  // Auto-aim object fields.
  constexpr uint32_t kPlayerAutoAim = 0x1F60;
  constexpr uint32_t kTarget = 0x14;
  // Where the reticle is on the target's screen box: 0 at its centre, 1 at
  // its edge (the box the search projects, 10 px bigger all round).
  constexpr uint32_t kTargetOverlap = 0x20;
  constexpr uint32_t kAimReticle = 0x30;    // x, y: where the player aims
  constexpr uint32_t kReticle = 0x38;       // x, y: after auto-aim

  // The player whose auto-aim sub_825B2D08 is updating (it passes
  // player + kPlayerAutoAim), and the update in progress.
  thread_local uint32_t t_player = 0;
  thread_local uint32_t t_autoaim = 0;

  bool Tuning()
  {
    return REXCVAR_GET(pdz_autoaim_tuning);
  }

  // pdz_aim_still_lock: the game tests the lock with radius
  // (r14 + r1C * t) * t and refuses when t <= 0, t = how fast the view turns
  // (0..1), so shots aren't bent while you hold still on a target. Dividing
  // both record fields by t for that test turns it into r14 + r1C * t, the
  // form target selection uses; at t = 0 a tiny t gets past the refusal (the
  // only other use left, the alternate radius r08 * t, stays as good as 0).
  // sub_824F0BB8 [824E36D8] is called right before the lock test and returns
  // to 824EF448 [824E23F8] with the record in r10 and t in f31.
  constexpr uint32_t kLockTestReturn = 0x824EF448;
  constexpr uint32_t kLockBase = 0x14;
  constexpr uint32_t kLockTurnExtra = 0x1C;
  constexpr double kLockMinTurn = 1e-6;
  thread_local uint32_t t_lock_record = 0;
  thread_local uint32_t t_lock_saved[2] = {};

  bool StillLock()
  {
    return Tuning() && REXCVAR_GET(pdz_aim_still_lock);
  }

  void ApplyStillLock(uint8_t *base, PPCContext &ctx)
  {
    const uint32_t record = ctx.r10.u32;
    if (!StillLock() || !IsGuestPointer(record))
    {
      return;
    }
    double t = ctx.f31.f64;
    if (!(t > 0.0))
    {
      t = kLockMinTurn;
      ctx.f31.f64 = t;
    }
    t_lock_record = record;
    t_lock_saved[0] = LoadBe32(base, record + kLockBase);
    t_lock_saved[1] = LoadBe32(base, record + kLockTurnExtra);
    StoreBeFloat(base, record + kLockBase, float(LoadBeFloat(base, record + kLockBase) / t));
    StoreBeFloat(base, record + kLockTurnExtra, float(LoadBeFloat(base, record + kLockTurnExtra) / t));
  }

  void RestoreStillLock(uint8_t *base)
  {
    if (!t_lock_record)
    {
      return;
    }
    StoreBe32(base, t_lock_record + kLockBase, t_lock_saved[0]);
    StoreBe32(base, t_lock_record + kLockTurnExtra, t_lock_saved[1]);
    t_lock_record = 0;
  }
} // namespace

// sub_825B2D08 [825A34F0](player, f1 = dt): the player's aiming update, which
// runs the player's auto-aim (player + 0x1F60) through sub_824EF078.
REX_HOOK_RAW(sub_825B2D08)
{
  const uint32_t previous = t_player;
  t_player = ctx.r3.u32;
  __imp__sub_825B2D08(ctx, base);
  t_player = previous;
}

// pdz_aim_still_lock rewrites the lock radius for the lock test, player only.
REX_HOOK_RAW(sub_824F0BB8)
{
  const bool lock_test = t_autoaim && uint32_t(ctx.lr) == kLockTestReturn;
  __imp__sub_824F0BB8(ctx, base);
  if (lock_test)
  {
    ApplyStillLock(base, ctx);
  }
}

// sub_825AC080 [8259C878](player, 0): zoomed? (player vtable +540 [+536]).
// The game's rotational aim assist (sub_825AF590 [8259FD70]) asks at
// 825AF6C8 [8259FEA0] and switches itself off when the answer is yes;
// pdz_aim_assist_zoomed answers no there, so it also runs while zoomed. Every
// other caller gets the real answer.
REX_HOOK_RAW(sub_825AC080)
{
  constexpr uint32_t kAssistZoomCheckReturn = 0x825AF6CC;
  const bool assist_check = uint32_t(ctx.lr) == kAssistZoomCheckReturn && Tuning() &&
                            REXCVAR_GET(pdz_aim_assist_zoomed);
  __imp__sub_825AC080(ctx, base);
  if (assist_check)
  {
    ctx.r3.u64 = 0;
  }
}

// sub_824EF078 [824E2028](autoaim, f1 = dt, f2 = range, &aim_reticle, &aim_origin).
REX_HOOK_RAW(sub_824EF078)
{
  const uint32_t autoaim = ctx.r3.u32;
  const uint32_t owner = t_player;
  if (!owner || autoaim != owner + kPlayerAutoAim)
  {
    __imp__sub_824EF078(ctx, base);
    return;
  }

  t_autoaim = autoaim;
  __imp__sub_824EF078(ctx, base);
  t_autoaim = 0;
  RestoreStillLock(base);

  // pdz_mouse_aim_assist_off (mouse_look.cpp): with the mouse the shot goes where the player aims, unbent.
  if (REXCVAR_GET(pdz_mouse_aim_assist_off) && pdz::mouse::MouseIsLastDevice())
  {
    StoreBeFloat(base, autoaim + kReticle, LoadBeFloat(base, autoaim + kAimReticle));
    StoreBeFloat(base, autoaim + kReticle + 4, LoadBeFloat(base, autoaim + kAimReticle + 4));
    return;
  }

  // pdz_aim_respect_headshots: with the reticle over the target's screen box
  // (overlap <= 1), keep the player's own height and bend only sideways, so
  // a head aim stays on the head instead of moving to the model's aim point.
  const uint32_t target = LoadBe32(base, autoaim + kTarget);
  const float aim_x = LoadBeFloat(base, autoaim + kAimReticle);
  const float aim_y = LoadBeFloat(base, autoaim + kAimReticle + 4);
  const float reticle_x = LoadBeFloat(base, autoaim + kReticle);
  const float reticle_y = LoadBeFloat(base, autoaim + kReticle + 4);
  const bool locked = target && (reticle_x != aim_x || reticle_y != aim_y);
  if (Tuning() && locked && REXCVAR_GET(pdz_aim_respect_headshots) &&
      LoadBeFloat(base, autoaim + kTargetOverlap) <= 1.0f)
  {
    StoreBeFloat(base, autoaim + kReticle + 4, aim_y);
  }
}
