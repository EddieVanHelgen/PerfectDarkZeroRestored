// Fixes for bugs in the original player controls. See README.md next to this
// file and docs/BUG_AUDIT.md (controls section) for the evidence.
//
// Every fix is its own option and only applies while the master switch
// pdz_controls_fixes is on (bind_controls_ab flips it); with either off the
// game runs its original code. All of them are scoped to the player: call
// sites are matched by return address, records are patched only for the
// player's own update and put back right after.
//
// - pdz_fix_fastlook_stick: the PD look model (sub_825A0888) reads the MOVE
//   stick (player vtable +0x1FC / +0x200 at 825A08DC / 825A08F4) where the
//   Halo model reads the look stick: fast look's "pushed fully" magnitude is
//   sqrt(look x^2 + move y^2) and the cross deadzones scale with the move
//   stick. The fix answers those two calls with the look stick.
// - pdz_fix_fastlook_decel: in fast look, once the turn speed is within 0.15
//   of the fast-look ceiling, the PD model changes it at the fast-look accel
//   (2-4/s) even when the target is lower (825A0EF4..0F34), so zooming or
//   easing the stick glides on. The normal path picks accel or decel by
//   comparing the two. The fix gives that branch the decel rate (field 9)
//   while it is slowing down: when zoomed or in cover (the target is then
//   always lower), or the update after the speed dropped.
// - pdz_fix_sensitivity_clamp: the look models multiply the horizontal
//   sensitivity by 1.2 when Fast Look is off (825A09E8 / 825A13A4), and both
//   getters by 1.75 with input +0x4F2 set, then blend the min / default / max
//   records by it with no clamp, so above the slider's end every field
//   extrapolates past the max record (mid-stick turns get slower). The fix
//   caps the value the look models see at the max record.
// - pdz_fix_fastlook_accel_data: playerpddefault.xml has fast-look accel
//   (field 10) 2, below both min (3) and max (4); every other PD field and
//   the Halo set (3 / 4 / 5) run min <= default <= max. The fix uses 3.
// - pdz_fix_spread_recovery_gate: a gun's spread recovers only once both of
//   the player's hands have been idle 0.2 s (min of the two at 825737BC).
//   sub_8253F5C0 answers FLT_MAX for a missing hand, which min() ignores, but
//   0 for a hand component without a weapon state (+0x500), which would hold
//   the spread at its peak. The fix answers FLT_MAX for that hand too.
#include "controls_fixes.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>

#include <rex/hook.h>
#include <rex/logging.h>

#include "../game_cvars.h"

REX_EXTERN(__imp__sub_8259C790);
REX_EXTERN(__imp__sub_8259C7C0);
REX_EXTERN(__imp__sub_8259D170);
REX_EXTERN(__imp__sub_8253F5C0);

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

  bool IsGuestPointer(uint32_t address)
  {
    return address >= 0x40000000u && address < 0xFFFF0000u;
  }

  bool Master()
  {
    return REXCVAR_GET(pdz_controls_fixes);
  }

  // Player and input fields.
  constexpr uint32_t kPlayerInput = 0x19A0;
  constexpr uint32_t kPlayerZoom = 0x1A9C;
  constexpr uint32_t kPlayerCoverState = 0x1CA4;
  constexpr uint32_t kInputPad = 0x480; // < 0: no pad
  constexpr uint32_t kInputStyle = 0x484; // 1 = Halo look model
  constexpr uint32_t kInputLookX = 0x4A4;
  constexpr uint32_t kInputLookY = 0x4A8;
  // Look state (player +0xF50).
  constexpr uint32_t kStateFastLook = 0x0;
  constexpr uint32_t kStateYawVelocity = 0x8;

  // PD look model call sites (return addresses).
  constexpr uint32_t kPdMoveXReturn = 0x825A08E8;  // vtable +0x1FC
  constexpr uint32_t kPdMoveYReturn = 0x825A0900;  // vtable +0x200
  constexpr uint32_t kPdHorizontalSensReturn = 0x825A0918;
  constexpr uint32_t kPdVerticalSensReturn = 0x825A0930;
  constexpr uint32_t kHaloHorizontalSensReturn = 0x825A12EC;
  constexpr uint32_t kHaloVerticalSensReturn = 0x825A1304;
  constexpr double kFastLookOffBonus = 1.2;
  constexpr double kSensitivityMax = 1.0; // blend parameter reaching the max record

  // Look records (13 floats): [max, default, min] x [pd, halo].
  constexpr uint32_t kAimParamTable = 0x82DA3760;
  constexpr uint32_t kFieldDecel = 9;
  constexpr uint32_t kFieldFastLookAccel = 10;
  constexpr uint32_t kPdRecordIndex[3] = {0, 2, 4}; // max, default, min
  constexpr uint32_t kPdDefault = 1;                // index into kPdRecordIndex
  constexpr float kPdDefaultFastLookAccelOriginal = 2.0f;
  constexpr float kPdDefaultFastLookAccelFixed = 3.0f;
  constexpr float kZoomedThreshold = 0.001f;

  // Fast look slowed down last update (|yaw speed| fell with the flag on).
  std::atomic<bool> g_fast_look_slowing{false};
  float g_yaw_before = 0.0f;

  // Spread recovery: hand time getter call sites in sub_825736B0.
  constexpr uint32_t kSpreadHand0Return = 0x825737B0;
  constexpr uint32_t kSpreadHand1Return = 0x825737BC;
  constexpr uint32_t kHandComponents = 0x3FB * 4; // player + (0x3FB + hand) * 4
  constexpr uint32_t kHandWeaponState = 0x500;
  std::atomic<uint32_t> g_empty_hand_seen{0};
  std::atomic<uint32_t> g_empty_hand_fixed{0};

  std::chrono::steady_clock::time_point g_last_log;
} // namespace

namespace controls_fixes
{
  bool FastLookStick()
  {
    return Master() && REXCVAR_GET(pdz_fix_fastlook_stick);
  }
  bool FastLookDecel()
  {
    return Master() && REXCVAR_GET(pdz_fix_fastlook_decel);
  }
  bool SensitivityClamp()
  {
    return Master() && REXCVAR_GET(pdz_fix_sensitivity_clamp);
  }
  bool FastLookAccelData()
  {
    return Master() && REXCVAR_GET(pdz_fix_fastlook_accel_data);
  }
  bool SpreadRecoveryGate()
  {
    return Master() && REXCVAR_GET(pdz_fix_spread_recovery_gate);
  }

  void BeforeLook(uint8_t *base, uint32_t player, uint32_t state, uint32_t input, LookPatch &patch)
  {
    g_yaw_before = LoadBeFloat(base, state + kStateYawVelocity);
    const bool pd_model = IsGuestPointer(input) && LoadBe32(base, input + kInputStyle) != 1;
    const bool data = FastLookAccelData();
    const bool fast_look = base[state + kStateFastLook] != 0;
    const bool slowing =
        FastLookDecel() && fast_look &&
        (LoadBeFloat(base, player + kPlayerZoom) > kZoomedThreshold || LoadBe32(base, player + kPlayerCoverState) != 0 ||
         g_fast_look_slowing.load(std::memory_order_relaxed));
    if (!pd_model || (!data && !slowing))
    {
      return;
    }
    for (uint32_t i = 0; i < 3; ++i)
    {
      const uint32_t record = LoadBe32(base, kAimParamTable + kPdRecordIndex[i] * 4);
      patch.record[i] = IsGuestPointer(record) ? record : 0;
      if (!patch.record[i])
      {
        continue;
      }
      const uint32_t field = record + kFieldFastLookAccel * 4;
      patch.fast_look_accel[i] = LoadBe32(base, field);
      if (data && i == kPdDefault && LoadBeFloat(base, field) == kPdDefaultFastLookAccelOriginal)
      {
        StoreBeFloat(base, field, kPdDefaultFastLookAccelFixed);
      }
      if (slowing)
      {
        StoreBe32(base, field, LoadBe32(base, record + kFieldDecel * 4));
      }
    }
    patch.active = true;
  }

  void AfterLook(uint8_t *base, uint32_t player, uint32_t state, const LookPatch &patch)
  {
    if (patch.active)
    {
      for (uint32_t i = 0; i < 3; ++i)
      {
        if (patch.record[i])
        {
          StoreBe32(base, patch.record[i] + kFieldFastLookAccel * 4, patch.fast_look_accel[i]);
        }
      }
    }
    const float after = LoadBeFloat(base, state + kStateYawVelocity);
    g_fast_look_slowing.store(base[state + kStateFastLook] && std::fabs(after) < std::fabs(g_yaw_before),
                              std::memory_order_relaxed);

    if (!(REXCVAR_GET(pdz_autoaim_log) || REXCVAR_GET(pdz_look_log)))
    {
      return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - g_last_log < std::chrono::seconds(1))
    {
      return;
    }
    g_last_log = now;
    REXLOG_INFO("pdz_fix: [{}] empty hand in spread check {} (fixed {})", ActiveSummary(),
                g_empty_hand_seen.exchange(0), g_empty_hand_fixed.exchange(0));
    (void)player;
  }

  double ClampHorizontalSensitivity(uint32_t lr, double value, bool game_applies_fast_look_off_bonus)
  {
    if (!SensitivityClamp() || (lr != kPdHorizontalSensReturn && lr != kHaloHorizontalSensReturn))
    {
      return value;
    }
    const double cap = game_applies_fast_look_off_bonus ? kSensitivityMax / kFastLookOffBonus : kSensitivityMax;
    return std::min(value, cap);
  }

  std::string ActiveSummary()
  {
    if (!Master())
    {
      return "off";
    }
    std::string text;
    auto add = [&](bool on, const char *name)
    {
      if (on)
      {
        text += text.empty() ? name : std::string(", ") + name;
      }
    };
    add(REXCVAR_GET(pdz_fix_fastlook_stick), "fast-look stick");
    add(REXCVAR_GET(pdz_fix_fastlook_decel), "fast-look decel");
    add(REXCVAR_GET(pdz_fix_sensitivity_clamp), "sensitivity clamp");
    add(REXCVAR_GET(pdz_fix_fastlook_accel_data), "fast-look accel 3");
    add(REXCVAR_GET(pdz_fix_spread_recovery_gate), "spread gate");
    return text.empty() ? "on, no fixes selected" : text;
  }
} // namespace controls_fixes

// Player vtable +0x1FC: move stick x. In the PD look model: look stick x.
REX_HOOK_RAW(sub_8259C790)
{
  const uint32_t lr = uint32_t(ctx.lr);
  const uint32_t player = ctx.r3.u32;
  __imp__sub_8259C790(ctx, base);
  if (lr == kPdMoveXReturn && controls_fixes::FastLookStick())
  {
    const uint32_t input = LoadBe32(base, player + kPlayerInput);
    if (IsGuestPointer(input) && int32_t(LoadBe32(base, input + kInputPad)) >= 0)
    {
      ctx.f1.f64 = LoadBeFloat(base, input + kInputLookX);
    }
  }
}

// Player vtable +0x200: move stick y. In the PD look model: look stick y.
REX_HOOK_RAW(sub_8259C7C0)
{
  const uint32_t lr = uint32_t(ctx.lr);
  const uint32_t player = ctx.r3.u32;
  __imp__sub_8259C7C0(ctx, base);
  if (lr == kPdMoveYReturn && controls_fixes::FastLookStick())
  {
    const uint32_t input = LoadBe32(base, player + kPlayerInput);
    if (IsGuestPointer(input) && int32_t(LoadBe32(base, input + kInputPad)) >= 0)
    {
      ctx.f1.f64 = LoadBeFloat(base, input + kInputLookY);
    }
  }
}

// Player vtable +0x208: vertical sensitivity (x1.75 with input +0x4F2).
REX_HOOK_RAW(sub_8259D170)
{
  const uint32_t lr = uint32_t(ctx.lr);
  __imp__sub_8259D170(ctx, base);
  if (controls_fixes::SensitivityClamp() && (lr == kPdVerticalSensReturn || lr == kHaloVerticalSensReturn))
  {
    ctx.f1.f64 = std::min(ctx.f1.f64, kSensitivityMax);
  }
}

// sub_8253F5C0(owner, hand): seconds since that hand last fired; FLT_MAX for
// a missing hand, 0 for a hand without a weapon state.
REX_HOOK_RAW(sub_8253F5C0)
{
  const uint32_t lr = uint32_t(ctx.lr);
  const uint32_t owner = ctx.r3.u32;
  const uint32_t hand = ctx.r4.u32;
  __imp__sub_8253F5C0(ctx, base);
  if (lr != kSpreadHand0Return && lr != kSpreadHand1Return)
  {
    return;
  }
  const uint32_t component = LoadBe32(base, owner + kHandComponents + hand * 4);
  if (!IsGuestPointer(component))
  {
    return;
  }
  // Same "component enabled" test as the getter (bit 0x10 of its type word).
  const uint32_t type = LoadBe32(base, component + 8);
  if (!IsGuestPointer(type) || !(LoadBe32(base, type) & 0x10) || LoadBe32(base, component + kHandWeaponState))
  {
    return;
  }
  g_empty_hand_seen.fetch_add(1, std::memory_order_relaxed);
  if (controls_fixes::SpreadRecoveryGate())
  {
    ctx.f1.f64 = std::numeric_limits<float>::max();
    g_empty_hand_fixed.fetch_add(1, std::memory_order_relaxed);
  }
}
