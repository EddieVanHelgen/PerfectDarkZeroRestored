// Fixes for bugs in the enemy AI (pdz_fix_ai_*), part of the game fixes
// group: each applies only while pdz_game_fixes is on (bind_game_fixes_ab,
// F11). See README.md next to this file and docs/BUG_AUDIT.md (AI10-AI35)
// for the evidence. Every fix is a hook on a call made from the one place
// that needs it (checked by return address), so no shared code or constant
// changes for anything else.
//
// tu3-base: Title Update 3 addresses (launch addresses in brackets). Every
// function involved is identical in TU3 or differs only in offsets that
// don't touch the bug, so each bug is still there (docs/TU3_BASE.md,
// re-targeting checklist). The launch branch's pdz_ai_state_log calls are
// left out (logs aren't ported).
//
// - pdz_fix_ai_semiauto_rhythm (AI14): while a burst runs, the shoot action
//   calls sub_821AA998 [821A5CB8] every frame, and for semi-auto guns that
//   rolls rand(0, rand(1,2)) each time: 0 presses the trigger, anything else
//   releases it, and a semi-auto shot needs a fresh press. Rolled per frame,
//   that's twice the presses per second at 60 fps. The fix keeps each
//   action's last roll (the inner rand returning to 821AAA68 [821A5D88],
//   action in r31) for 1/30 s of AI clock, the original rhythm.
// - pdz_fix_ai_grenade_height (AI18): the grenade throw check
//   (sub_821B06E0 [821ABA00]) scales the target point by 0.5-0.85, uses 1.0
//   when the target is more than 100 cm above or below, and only then tests
//   "more than 300 cm -> 1.3", which can't be true any more. The fix records
//   the launch point (sub_82585B58 from 821B0808 [825762D0 / 821ABB28]) and,
//   when the gap is over 300 cm, redoes the point with 1.3 before the throw
//   tests (sub_8218D8F0 from 821B0894 [82188D48 / 821ABBB4]).
// - pdz_fix_ai_dodge_facing (AI20): the dodge task (sub_821D2180 [821CB0C0])
//   picks one of four facings with rand() % 3 (821D26E8 [821CB628]), so the
//   fourth, FacingRight (animations 151 / 152), never plays. The fix draws
//   % 4; for the fourth it lets the game take the FacingFront branch
//   (147 / 148, same side test) and swaps in 151 / 152 and its forward
//   clearance ray (r5 = 0) when the clearance check (sub_821D1CD0 [821CAC10])
//   is called, as case 3 would.
// - pdz_fix_ai_dodge_clearance (AI21): that check casts its 250 cm ray along
//   the enemy's yaw (+-90 degrees for sideways dodges), so the backwards
//   dodges (150 / 152) are checked in front. The fix turns the yaw it reads
//   (sub_82519A90 from 821D1CEC [8250B3E8 / 821CAC2C]) round by 180 degrees
//   for them.
// - pdz_fix_ai_corner_speed (AI23): the route follower's corner speed curve
//   lookup (sub_8218F280 / 82191988 / 82194090 [8218A6E0 / 8218CDD8 /
//   8218F4D0]) loops i < n-2 over n points, so the last segment (85-90
//   degrees: 7% -> 5% of top speed) returns 0. The lookup leaves the curve in
//   r3; when it returned 0 for an angle in the last segment, the fix
//   interpolates that segment the same way.
// - pdz_fix_ai_cover_min_distance (AI35): the combat planner's cover goals
//   (types 2 / 3, pickers sub_821B7B58 / sub_821B7A98 [821B2E88 / 821B2DC8])
//   pick pads through the filter sub_8215A888 [82155EB8] with a minimum
//   distance from the target of max(weapon range #1^2, 700), but re-check
//   the chosen pad every tick with the same test and a fixed 10 m (1e6,
//   sub_821B7D00 [821B3030]). A pad picked at 10 m or less fails on the next
//   tick and the plan is rebuilt on it again. While one of those two pickers
//   runs, the fix rejects passing pads at or under 10 m from the target
//   position the filter used (search context +0x70), so the pick uses the
//   re-check's band. Other users of the filter are untouched.
// - pdz_fix_ai_calm_flag (AI10): the alert counter (sub_821891D8
//   [82184618]) sets the perception slot's "calm" flag (0x10) from the count
//   before clamping it to 0..100, so lowering a count of 0 clears "calm" and
//   stores 0. The fix sets the flag from the stored count whenever the call
//   changed the slot.
// - pdz_fix_ai_speech_remap_fallback (AI49, plan G8): the per-character
//   one-shot speech remaps (char +12852, applied by sub_82152450 after the
//   solo-to-team step) can point at a line the speaker's voice never
//   recorded. Nightclub's script remaps 12 gangsters' SEE_PLAYER_TEAM /
//   SURPRISED_SOLO to LVL4_SEES_PLAYER_WITH_GUN (160), which only the troopers
//   recorded, so those first reactions were silent. When the remapped name
//   (plain or _MALE) doesn't resolve for the speaker's voice, the fix keeps
//   the original event.
// - pdz_fix_ai_taunt_rate (AI55, plan G9): see the sub_827025F8 hook.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

#include <rex/hook.h>
#include <rex/system/kernel_state.h>

#include "../game_cvars.h"
#include "fix_log.h"
#include "hook_profile.h" // pdz_hook_profile (must follow <rex/hook.h>)
#include "render/game_pace.h" // pdz_null_refresh_hz (the taunt roll)

REX_EXTERN(__imp__sub_82152450);
REX_EXTERN(sub_82202C78);

REX_EXTERN(__imp__sub_827025F8);
REX_EXTERN(__imp__sub_82702540);
REX_EXTERN(__imp__sub_821D1CD0);
REX_EXTERN(__imp__sub_82519A90);
REX_EXTERN(__imp__sub_82585B58);
REX_EXTERN(__imp__sub_8218D8F0);
REX_EXTERN(__imp__sub_8218F280);
REX_EXTERN(__imp__sub_82191988);
REX_EXTERN(__imp__sub_82194090);
REX_EXTERN(__imp__sub_821891D8);
REX_EXTERN(__imp__sub_821B7B58);
REX_EXTERN(__imp__sub_821B7A98);
REX_EXTERN(__imp__sub_8215A888);

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

  uint16_t LoadBe16(const uint8_t *base, uint32_t address)
  {
    return uint16_t((base[address] << 8) | base[address + 1]);
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

  // Guest heap and stack objects can sit below 0x40000000 too (4 KB page
  // range), so only rule out null-ish and the top page.
  bool IsGuestPointer(uint32_t address)
  {
    return address >= 0x00010000u && address < 0xFFFF0000u;
  }

  bool Fix(bool option)
  {
    return REXCVAR_GET(pdz_game_fixes) && option;
  }

  constexpr uint32_t kAiClock = 0x82E2F334; // [82E2E478] float seconds, per level

  // AI14: the inner trigger roll in sub_821AA998, and how long a roll holds.
  constexpr uint32_t kTriggerRollReturn = 0x821AAA68;
  // G9 / AI55: the taunt roll's return address in sub_8235FE60.
  constexpr uint32_t kTauntRollReturn = 0x823601F4;
  // The game's frame time (float s, 82D6BFD8 + 120, sub_82140500; render/game_pace.cpp).
  constexpr uint32_t kFrameDt = 0x82D6BFD8 + 120;
  constexpr float kTriggerRollInterval = 1.0f / 30.0f;
  constexpr float kTriggerRollSlack = 0.001f; // frame time jitter
  constexpr size_t kTriggerActionsMax = 512;
  struct TriggerRoll
  {
    float time = 0.0f;
    uint32_t value = 0;
  };
  std::unordered_map<uint32_t, TriggerRoll> g_trigger_rolls;

  // AI20 / AI21: the dodge task's facing roll and clearance check.
  constexpr uint32_t kDodgeFacingRollReturn = 0x821D26E8;
  constexpr uint32_t kDodgeClearanceReturn = 0x821D2728;
  constexpr uint32_t kDodgeYawReturn = 0x821D1CF0;
  constexpr uint32_t kDodgeFrontLeft = 147;
  constexpr uint32_t kDodgeFrontRight = 148;
  constexpr uint32_t kDodgeLeftBack = 150;
  constexpr uint32_t kDodgeRightForwards = 151;
  constexpr uint32_t kDodgeRightBack = 152;
  constexpr uint32_t kFacingsShipped = 3;
  constexpr uint32_t kFacings = 4;
  constexpr double kHalfTurn = 3.14159265358979323846;
  thread_local bool t_dodge_facing_right = false;
  thread_local bool t_dodge_check_behind = false;

  // AI18: the grenade check's launch point and target scaling.
  constexpr uint32_t kGrenadeLaunchReturn = 0x821B080C;
  constexpr uint32_t kGrenadeTestReturn = 0x821B0898;
  constexpr uint32_t kGrenadeTargetOffset = 88; // target point in its frame
  constexpr float kGrenadeHighGap = 300.0f;
  constexpr float kGrenadeHighScale = 1.3f;
  thread_local bool t_grenade_launch_valid = false;
  thread_local float t_grenade_launch[3] = {};

  // AI23: the curve the lookup returns in r3: (angle, factor) pairs from the
  // start, end pointer at +160.
  constexpr uint32_t kCurveEnd = 160;
  constexpr uint32_t kCurvePoint = 8;

  // AI35: cover pad picks of the planner's cover goals A / B.
  constexpr uint32_t kSearchTargetPosition = 0x70; // pointer
  constexpr uint32_t kPadPosition = 0x10;
  constexpr float kRecheckMin2 = 1.0e6f; // sub_821B7D00's 10 m
  thread_local int t_cover_goal_pick = 0;

  // AI10: perception slots.
  constexpr uint32_t kPerceptionSlots = 0x82E5D590; // [82E5C7C8]
  constexpr uint32_t kPerceptionSlotSize = 80;
  constexpr uint32_t kSlotAlertCount = 12; // u16, 0..100
  constexpr uint32_t kSlotFlags = 68;
  constexpr uint32_t kSlotCalm = 0x10;
  constexpr int32_t kPerceptionSlotCount = 170;

  void FixCornerSpeed(PPCContext &ctx, uint8_t *base, void (*lookup)(PPCContext &, uint8_t *))
  {
    const float angle = float(ctx.f1.f64);
    lookup(ctx, base);
    if (!Fix(REXCVAR_GET(pdz_fix_ai_corner_speed)) || ctx.f1.f64 != 0.0)
    {
      return;
    }
    const uint32_t curve = ctx.r3.u32;
    if (!IsGuestPointer(curve))
    {
      return;
    }
    const int32_t points = int32_t(LoadBe32(base, curve + kCurveEnd) - curve) / int32_t(kCurvePoint);
    if (points < 2)
    {
      return;
    }
    // The segment the original loop leaves out, interpolated as it does
    // the others.
    const uint32_t first = curve + uint32_t(points - 2) * kCurvePoint;
    const uint32_t last = first + kCurvePoint;
    const float x0 = LoadBeFloat(base, first);
    const float x1 = LoadBeFloat(base, last);
    if (!(angle >= x0 && angle < x1))
    {
      return;
    }
    const float y0 = LoadBeFloat(base, first + 4);
    const float y1 = LoadBeFloat(base, last + 4);
    const float t = (x1 - angle) / (x1 - x0);
    ctx.f1.f64 = double(t * (y0 - y1) + y1);
    fix_log::Count("AI corner last segment (fix)");
  }
} // namespace

// sub_827025F8 [826F28C0](lo, hi): random integer in [lo, hi].
REX_HOOK_RAW(sub_827025F8)
{
  // G9 / AI55: Subway's pre-fight taunt task (sub_8235FE60) rolls
  // rand(0, 100) < 5 every update, so at 60 fps taunts start twice as often.
  // With the 60 fps unlock on, roll over [0, 200]: the same chance per second.
  // With pdz_null_refresh_hz in effect (stage 2 M5: up to 120 fps or more),
  // roll over [0, 100 x (1/30 s) / this frame's dt]: the same chance per
  // second at any frame rate.
  if (uint32_t(ctx.lr) == kTauntRollReturn)
  {
    if (Fix(REXCVAR_GET(pdz_fix_ai_taunt_rate)) && ctx.r4.u32 == 100)
    {
      if (pdz::pace::Active())
      {
        const float dt = LoadBeFloat(base, kFrameDt);
        const double scale = dt > 0.0f ? (1.0 / 30.0) / double(dt) : 1.0;
        ctx.r4.u64 = uint64_t(std::clamp<long>(std::lround(100.0 * scale), 100, 1000));
        fix_log::Count("AI taunt roll at the 30 fps rate, frame-time scaled (fix)");
      }
      else if (REXCVAR_GET(pdz_fps60_unlock))
      {
        ctx.r4.u64 = 200;
        fix_log::Count("AI taunt roll at the 30 fps rate (fix)");
      }
    }
    __imp__sub_827025F8(ctx, base);
    return;
  }
  if (uint32_t(ctx.lr) != kTriggerRollReturn || !Fix(REXCVAR_GET(pdz_fix_ai_semiauto_rhythm)))
  {
    __imp__sub_827025F8(ctx, base);
    return;
  }
  // sub_821AA998 keeps its shoot action in r31.
  const uint32_t action = ctx.r31.u32;
  const float now = LoadBeFloat(base, kAiClock);
  const auto found = g_trigger_rolls.find(action);
  if (found != g_trigger_rolls.end() && now >= found->second.time &&
      now - found->second.time < kTriggerRollInterval - kTriggerRollSlack)
  {
    ctx.r3.u64 = found->second.value;
    fix_log::Count("AI semi-auto roll held");
    return;
  }
  __imp__sub_827025F8(ctx, base);
  if (g_trigger_rolls.size() >= kTriggerActionsMax)
  {
    g_trigger_rolls.clear();
  }
  g_trigger_rolls[action] = TriggerRoll{now, ctx.r3.u32};
  fix_log::Count("AI semi-auto rolls");
}

// Sabotage stage 4 (cut_scenarios.cpp, pdz_sabotage_play_shop): the bot
// shop's gun roll (sub_82147390 at 821477B0: r3 % r28 over its list).
uint32_t PdzSabotageShopRoll(PPCContext &ctx, uint8_t *base, uint32_t roll);
constexpr uint32_t kShopRollReturn = 0x821477B4;

// sub_82702540 [826F2808](): random non-negative integer.
REX_HOOK_RAW(sub_82702540)
{
  __imp__sub_82702540(ctx, base);
  if (uint32_t(ctx.lr) == kShopRollReturn)
  {
    ctx.r3.u64 = PdzSabotageShopRoll(ctx, base, ctx.r3.u32);
    return;
  }
  if (uint32_t(ctx.lr) != kDodgeFacingRollReturn)
  {
    return;
  }
  t_dodge_facing_right = false;
  if (!Fix(REXCVAR_GET(pdz_fix_ai_dodge_facing)))
  {
    return;
  }
  // The caller takes r3 % 3; hand it the facing it should get from % 4,
  // with the fourth going down the first branch to be swapped later.
  const uint32_t facing = ctx.r3.u32 % kFacings;
  t_dodge_facing_right = facing == kFacingsShipped;
  fix_log::Count(t_dodge_facing_right ? "AI dodge facing: right (fix)" : "AI dodge facing: other");
  ctx.r3.u64 = t_dodge_facing_right ? 0 : facing;
}

// sub_821D1CD0 [821CAC10](character, r5 = direction, f1 = distance): the
// dodge task's clearance ray. The dodge animation type is in the caller's
// r31, which this function saves and restores, so changing it here changes
// the caller's.
REX_HOOK_RAW(sub_821D1CD0)
{
  if (uint32_t(ctx.lr) != kDodgeClearanceReturn)
  {
    __imp__sub_821D1CD0(ctx, base);
    return;
  }
  uint32_t type = ctx.r31.u32;
  if (t_dodge_facing_right && (type == kDodgeFrontLeft || type == kDodgeFrontRight))
  {
    type = kDodgeRightForwards + (type - kDodgeFrontLeft);
    ctx.r31.u64 = type;
    ctx.r5.u64 = 0; // forwards / backwards dodges check along the yaw
  }
  t_dodge_facing_right = false;
  t_dodge_check_behind =
      Fix(REXCVAR_GET(pdz_fix_ai_dodge_clearance)) && (type == kDodgeLeftBack || type == kDodgeRightBack);
  if (t_dodge_check_behind)
  {
    fix_log::Count("AI dodge back: ray behind (fix)");
  }
  __imp__sub_821D1CD0(ctx, base);
  t_dodge_check_behind = false;
}

// sub_82519A90 [8250B3E8](character): its yaw in radians.
REX_HOOK_RAW(sub_82519A90)
{
  __imp__sub_82519A90(ctx, base);
  if (t_dodge_check_behind && uint32_t(ctx.lr) == kDodgeYawReturn)
  {
    ctx.f1.f64 += kHalfTurn;
  }
}

// sub_82585B58 [825762D0](gun, r4 = out point): the grenade's launch point.
REX_HOOK_RAW(sub_82585B58)
{
  const bool grenade = uint32_t(ctx.lr) == kGrenadeLaunchReturn;
  const uint32_t out = ctx.r4.u32;
  __imp__sub_82585B58(ctx, base);
  if (grenade && IsGuestPointer(out))
  {
    for (int i = 0; i < 3; ++i)
    {
      t_grenade_launch[i] = LoadBeFloat(base, out + 4 * i);
    }
    t_grenade_launch_valid = true;
  }
}

// sub_8218D8F0 [82188D48](..., r7 = aim point, ...): can this attack reach
// the point.
REX_HOOK_RAW(sub_8218D8F0)
{
  if (uint32_t(ctx.lr) == kGrenadeTestReturn)
  {
    const bool valid = t_grenade_launch_valid;
    t_grenade_launch_valid = false;
    const uint32_t point = ctx.r7.u32;
    const uint32_t target = ctx.r1.u32 + kGrenadeTargetOffset;
    if (valid && Fix(REXCVAR_GET(pdz_fix_ai_grenade_height)) && IsGuestPointer(point))
    {
      fix_log::Count("AI grenade checks");
      const float gap = LoadBeFloat(base, target + 4) - t_grenade_launch[1];
      if (std::fabs(gap) > kGrenadeHighGap)
      {
        fix_log::Count("AI grenade high target x1.3 (fix)");
        for (int i = 0; i < 3; ++i)
        {
          const float from = t_grenade_launch[i];
          StoreBeFloat(base, point + 4 * i, from + (LoadBeFloat(base, target + 4 * i) - from) * kGrenadeHighScale);
        }
      }
    }
  }
  __imp__sub_8218D8F0(ctx, base);
}

// Corner speed curve lookups (route follower types A / B / C, vtable +0x1C).
REX_HOOK_RAW(sub_8218F280)
{
  FixCornerSpeed(ctx, base, __imp__sub_8218F280);
}

REX_HOOK_RAW(sub_82191988)
{
  FixCornerSpeed(ctx, base, __imp__sub_82191988);
}

REX_HOOK_RAW(sub_82194090)
{
  FixCornerSpeed(ctx, base, __imp__sub_82194090);
}

// sub_821891D8 [82184618](slot, r4 = raise): one step of a perception slot's
// alert count.
REX_HOOK_RAW(sub_821891D8)
{
  const int32_t slot = int32_t(ctx.r3.u32);
  if (slot < 0 || slot >= kPerceptionSlotCount || !Fix(REXCVAR_GET(pdz_fix_ai_calm_flag)))
  {
    __imp__sub_821891D8(ctx, base);
    return;
  }
  const uint32_t entry = kPerceptionSlots + uint32_t(slot) * kPerceptionSlotSize;
  const uint16_t count = LoadBe16(base, entry + kSlotAlertCount);
  const uint32_t flags = LoadBe32(base, entry + kSlotFlags);
  __imp__sub_821891D8(ctx, base);
  const uint16_t new_count = LoadBe16(base, entry + kSlotAlertCount);
  const uint32_t new_flags = LoadBe32(base, entry + kSlotFlags);
  if (new_count == count && new_flags == flags)
  {
    return; // the character is dead: the game changed nothing
  }
  const uint32_t fixed = new_count == 0 ? (new_flags | kSlotCalm) : (new_flags & ~kSlotCalm);
  if (fixed != new_flags)
  {
    fix_log::Count("AI calm flag corrected (fix)");
  }
  StoreBe32(base, entry + kSlotFlags, fixed);
}

// sub_821B7B58 / sub_821B7A98 [821B2E88 / 821B2DC8](character, goal): the
// planner's cover goal A / B pad pickers (AI35).
REX_HOOK_RAW(sub_821B7B58)
{
  ++t_cover_goal_pick;
  __imp__sub_821B7B58(ctx, base);
  --t_cover_goal_pick;
}

REX_HOOK_RAW(sub_821B7A98)
{
  ++t_cover_goal_pick;
  __imp__sub_821B7A98(ctx, base);
  --t_cover_goal_pick;
}

// sub_8215A888 [82155EB8](pad, search context): may this pad be a cover goal.
REX_HOOK_RAW(sub_8215A888)
{
  const uint32_t pad = ctx.r3.u32;
  const uint32_t search = ctx.r4.u32;
  __imp__sub_8215A888(ctx, base);
  const bool passed = (ctx.r3.u32 & 0xFF) != 0;
  if (passed && t_cover_goal_pick > 0 && Fix(REXCVAR_GET(pdz_fix_ai_cover_min_distance)) && IsGuestPointer(pad) &&
      IsGuestPointer(search))
  {
    const uint32_t target = LoadBe32(base, search + kSearchTargetPosition);
    if (IsGuestPointer(target))
    {
      float d2 = 0.0f;
      for (int i = 0; i < 3; ++i)
      {
        const float d = LoadBeFloat(base, pad + kPadPosition + 4 * i) - LoadBeFloat(base, target + 4 * i);
        d2 += d * d;
      }
      // The re-check fails at d^2 <= min.
      if (d2 <= kRecheckMin2)
      {
        ctx.r3.u64 = 0;
        fix_log::Count("AI cover pad within 10 m rejected (fix)");
      }
      else
      {
        fix_log::Count("AI cover pads passed");
      }
    }
  }
}

// --- pdz_fix_ai_speech_remap_fallback (AI49, plan G8) ---
namespace
{
  constexpr uint32_t kVoiceTable = 0x82D2ADE8;      // event name pointers, 4 bytes each
  constexpr uint32_t kVoiceNames = kVoiceTable + 692; // voice names, 36 bytes each
  constexpr uint32_t kVoiceCount = 26;
  constexpr uint32_t kEventCount = 171;
  constexpr uint32_t kCharVoice = 12704;
  constexpr uint32_t kNameScratchSize = 128;

  std::mutex g_remap_mutex;
  uint32_t g_name_scratch = 0;

  std::string GuestCString(const uint8_t *base, uint32_t address, size_t limit)
  {
    std::string s;
    if (!IsGuestPointer(address))
    {
      return s;
    }
    for (const char *p = reinterpret_cast<const char *>(base + address); *p && s.size() < limit; ++p)
    {
      s += *p;
    }
    return s;
  }

  // Does XACT_SOUNDBANK_<voice>_<event> (or its _MALE take) resolve in the
  // loaded banks? sub_82202C78 [821FAB60] returns -1 when it doesn't.
  bool VoiceHasEvent(PPCContext &ctx, uint8_t *base, uint32_t voice, uint32_t event)
  {
    const std::string voice_name = GuestCString(base, kVoiceNames + voice * 36, 35);
    const std::string event_name = GuestCString(base, LoadBe32(base, kVoiceTable + event * 4), 80);
    if (voice_name.empty() || event_name.empty())
    {
      return true; // can't tell: leave the game's choice alone
    }
    if (!g_name_scratch)
    {
      g_name_scratch = REX_KERNEL_MEMORY()->SystemHeapAlloc(kNameScratchSize);
      if (!g_name_scratch)
      {
        return true;
      }
    }
    for (const char *suffix : {"", "_MALE"})
    {
      const std::string name = "XACT_SOUNDBANK_" + voice_name + "_" + event_name + suffix;
      if (name.size() + 1 > kNameScratchSize)
      {
        return true;
      }
      std::memcpy(base + g_name_scratch, name.c_str(), name.size() + 1);
      rex::CallFrame frame(ctx);
      frame.ctx.r3.u64 = g_name_scratch;
      sub_82202C78(frame.ctx, base);
      if (int32_t(frame.ctx.r3.u32) != -1)
      {
        return true;
      }
    }
    return false;
  }
} // namespace

// sub_82152450 [8214DA40](character, event): applies the character's one-shot
// speech remaps (+12852) and returns the event to say.
REX_HOOK_RAW(sub_82152450)
{
  const uint32_t character = ctx.r3.u32;
  const uint32_t event = ctx.r4.u32;
  __imp__sub_82152450(ctx, base);
  const uint32_t remapped = ctx.r3.u32;
  if (remapped == event || !Fix(REXCVAR_GET(pdz_fix_ai_speech_remap_fallback)) || !IsGuestPointer(character) ||
      remapped >= kEventCount)
  {
    return;
  }
  const uint32_t voice = LoadBe32(base, character + kCharVoice);
  if (voice >= kVoiceCount)
  {
    return;
  }
  std::lock_guard<std::mutex> lock(g_remap_mutex);
  if (!VoiceHasEvent(ctx, base, voice, remapped))
  {
    ctx.r3.u64 = event;
    fix_log::Count("AI speech remap to an unrecorded line: original kept (fix)");
  }
}
