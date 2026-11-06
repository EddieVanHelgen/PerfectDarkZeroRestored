// Look tweaks for the Title Update 3 build (tu3-base). Addresses are TU3's;
// launch addresses in brackets. Ported from the launch branch: the fast-look
// zoom brake (pdz_fastlook_zoom_brake, on) with the part of pdz_look_tuning
// it needs (the master switch). Added 2026-10-24 for the user's recordings,
// each its own labelled enhancement, off by default, A/B one at a time:
//
// - pdz_look_radial: the right stick as one direction (diagonals turn as
//   fast as straight), one round deadzone (pdz_look_radial_deadzone, 0.08 of
//   full deflection) instead of the pad filter's square one and the look
//   records' per-axis deadzones, rescaled so there is no jump at its edge.
//   Axial snapping (pdz_look_axial_snap_deg, 10): within that angle of an axis
//   the stick counts as straight, fading back to its own direction by twice
//   the angle, so a straight push doesn't drift into a diagonal.
// - pdz_look_snappy: the look records' acceleration / deceleration (fields
//   8 / 9) divided by pdz_look_inertia_scale (0.4; 1 = original), so the view
//   starts and stops with the stick. Rates (fields 0 / 1 / 11) and the fast-look
//   ramp (field 10) stay as they are.
// - pdz_look_fixed_curve: the curve exponent (field 2) of the max and min
//   records set to the default record's, so sensitivity only scales speed.
//
// sub_825B3AC0 [825A42B0](owner, rotated, state, f1 = dt) updates one look
// controller; on foot it is the player with its look state at player + 0xF50.
// Its sensitivity blend reads the look records at 82DA45FC [82DA3760]:
// [max, default, min] x [pd, halo], 13 floats each (docs/LOOK_PIPELINE.md).
// sub_822733D0 [8226BE48](slot, &x, &y) is the pad filter (one stick, in
// place; slots 4-7 are the right sticks), with its per-slot state at
// 82D6C0D0 [82D6B3A8], 60 bytes each.
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <string>

#include <fmt/format.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "../game_cvars.h"
#include "game_fixes.h"
#include "mouse_look.h"
#include "render/latency_log.h"
#include "hook_profile.h" // pdz_hook_profile (must follow <rex/hook.h>)

REX_EXTERN(__imp__sub_825B3AC0);
REX_EXTERN(__imp__sub_825AC948);
REX_EXTERN(__imp__sub_825AC978);
REX_EXTERN(__imp__sub_825AC9B8);
REX_EXTERN(__imp__sub_822733D0);

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
    return address >= 0x10000u && address < 0x90000000u;
  }

  // pdz_look_tuning off makes the look options behave as original.
  bool LookTuning()
  {
    return REXCVAR_GET(pdz_look_tuning);
  }

  // Player fields (same in TU3).
  constexpr uint32_t kPlayerLookState = 0xF50;
  constexpr uint32_t kPlayerInput = 0x19A0;
  constexpr uint32_t kPlayerZoom = 0x1A9C;  // scope zoom amount, 0 = not zoomed
  // Input fields (TU3: launch + 0x210).
  constexpr uint32_t kInputPad = 0x690;              // [0x480] the player's pad, < 0 none
  constexpr uint32_t kInputStyle = 0x694;            // [0x484] 1 = "Halo" model (Standard)
  constexpr uint32_t kInputSensitivity = 0x6E4;      // [0x4D4] horizontal, vertical
  constexpr uint32_t kInputSensitivityBoost = 0x702; // [0x4F2]
  // Look state fields.
  constexpr uint32_t kStateFastLook = 0x0;
  // The game's own turn of the view (ladder mounts and other set pieces):
  // while +0x49 is set the look update (825B3AE4) eases yaw / pitch from
  // +0x4C / +0x50 to +0x5C / +0x60 over +0x58 s (+0x54 elapsed, sub_826386C8)
  // and writes them through the owner's vtable +0x20 / +0x24. It doesn't use
  // the look records, the stick or fast look.
  constexpr uint32_t kStateGameTurn = 0x49;
  constexpr uint32_t kStateGameTurnFrom = 0x4C;   // yaw, pitch
  constexpr uint32_t kStateGameTurnTo = 0x5C;     // yaw, pitch
  constexpr uint32_t kStateGameTurnTime = 0x58;

  // Look records: [max, default, min] x [pd, halo], 13 floats each.
  constexpr uint32_t kAimParamTable = 0x82DA45FC; // [82DA3760]
  constexpr uint32_t kAimParamRecords = 6;
  constexpr uint32_t kAimParamFloats = 13;
  constexpr uint32_t kCurveExponent = 2; // |stick| ^ this
  constexpr uint32_t kYawDeadzone = 6;   // * (0.3 + 0.7 * |vertical|)
  constexpr uint32_t kPitchDeadzone = 7; // * 0.5 * (1 + |horizontal|) (PD model)
  constexpr uint32_t kAccel = 8;         // per second, * (0.25 + 0.75 * |stick|)
  constexpr uint32_t kDecel = 9;

  // Pad filter state, 60 bytes per slot: +0 centre x/y, +8 deadzone in use
  // x/y, +0x10 deadzone for a quiet stick (5000 of 32767), +0x18 connected,
  // +0x24 jitter x/y (0x7FFF = not measured yet).
  constexpr uint32_t kPadFilterState = 0x82D6C0D0; // [82D6B3A8]
  constexpr uint32_t kPadFilterStride = 60;
  constexpr uint32_t kPadDeadzoneInUse = 0x8;
  constexpr uint32_t kPadQuietDeadzone = 0x10;
  constexpr uint32_t kPadConnected = 0x18;
  constexpr uint32_t kPadJitter = 0x24;
  constexpr float kPadQuietDeadzoneDefault = 5000.0f;
  constexpr uint32_t kPadJitterUnknown = 0x7FFF;
  std::array<bool, 4> g_pad_deadzone_applied{};

  // pdz_look_slow_detect (overnight 30-31 Oct). Offsets from the launch branch's per-frame look log (a6d3021,
  // docs/LOOK_PIPELINE.md), input fields + 0x210 in TU3.
  constexpr uint32_t kPadCentreSamples = 0x34;          // per axis: samples in the learned centre's average
  constexpr uint32_t kInputStickRaw = 0x6AC;            // [0x49C] x, y
  constexpr uint32_t kInputStickLook = 0x6B4;           // [0x4A4] x, y: the look model's stick input
  constexpr uint32_t kInputAutoAimOption = 0x6A4;       // [0x494] the assist runs while 1
  constexpr uint32_t kStatePitchVelocity = 0x4, kStateYawVelocity = 0x8; // per 1/60 s, degrees
  constexpr uint32_t kStateYawScale = 0xC, kStatePitchScale = 0x10;    // aim friction multiplies these
  constexpr uint32_t kStateAssistRate = 0x30, kStateAssistStrength = 0x38; // the rotational assist's turn
  constexpr uint32_t kPlayerViewYaw = 0x1670;           // degrees
  struct PadSample
  {
    int32_t raw[2] = {}, out[2] = {}, jitter[2] = {};
    float centre[2] = {}, deadzone[2] = {};
    uint32_t samples[2] = {};
    float rim_neg[2] = {}, rim_pos[2] = {}; // RimRange per side (LOOK_SLOW_TURN.md, 2 Nov)
    float radial_magnitude = -1.0f;         // pdz_look_radial's output magnitude, < 0 = radial off
    bool valid = false;
  };
  std::array<PadSample, 4> g_pad_sample{};

  // The stick's range from the filter's learned centre to its rim, one side of one axis, in the filter's units
  // (full deflection = 32767): 32767 - centre on the positive side, 32767 + centre on the negative side (log 414,
  // 2 Nov; docs/LOOK_SLOW_TURN.md). pdz_look_radial and the fast-look rim measure the stick from the learned centre,
  // and used to divide by 32767 on both sides: with the centre at -330, a full push left (raw -32768) came to
  // 32438 / 32767 and, after the deadzone, look 0.989, just short of Rare's fast look (|look| > 0.99), while a push
  // right clamped early. Each side is now scaled to its own range, so the rim is full on both. The negative side
  // uses 32767 (not 32768) + centre so that a centred stick is scaled by exactly 1.0f: bit-identical to before (raw
  // -32768 then still comes to 32768, clamped to full as before). Rare's own filter (sub_822733D0, below) already
  // reaches full on both sides: it divides by the shorter side's range, 32767 - |centre| - deadzone, and clamps.
  float RimRange(float centre, bool negative)
  {
    const float c = std::isfinite(centre) ? std::clamp(centre, -16384.0f, 16384.0f) : 0.0f;
    return negative ? 32767.0f + c : 32767.0f - c;
  }

  // One axis's deflection from the learned centre (raw - centre), rescaled so its side's rim is at 32767.
  float ToRimUnits(float delta, float centre)
  {
    return delta * (32767.0f / RimRange(centre, delta < 0.0f));
  }

  struct SlowDetect
  {
    float peak = 0.0f;           // deg/s: the recent full-push turn rate (decays: halves in 120 s)
    int push_dir = 0;            // -1 left, +1 right, 0 none (raw beyond 0.8 after the deadzone)
    double push_s = 0.0;         // how long this push has lasted
    double since_line = 1e9;     // since the last line
    int compare_dir = 0;         // log the next full push this way for comparison
    uint32_t fired = 0;
  };
  std::array<SlowDetect, 4> g_slow{};

  // pdz_look_fastlook_threshold (31 Oct, docs/LOOK_SLOW_TURN.md): when fast look may engage. Rare decides in the
  // two look models (TU3; registers as the midasm hooks see them):
  // - Standard (Halo, sub_825B00A8 from 825B0480): f13 = |look input| (x f27, -y f28); on when f13 >= A
  //   [820EB50C] = 0.98 and Tx [8208BC00] = -1.0 < f27 < 1.0, the flag (state +0) = the Fast Look option;
  //   otherwise released when f13 < OFF [8208998C] = 0.95 or |f27| >= 1.0 (else kept as it is).
  // - Classic (PD, sub_825B0AA0 from 825B0E40): f28 = |look input|^2, f27 = the curved x; on when |f27| > X
  //   [8209A148] = 0.9 and f28 > M2 [820EBC8C] = 0.9801 (r29 == 0); released when f28 < OFF2 [820EBC88] = 0.9409.
  //   Log 319 matches this one (look 0.990: off; 0.993: on). Values as logged by the first TU3 run.
  // Setting the flag alone doesn't give the turbo (log b0_fl_new, 31 Oct: a 0.89 push with the flag forced on
  // still turned at 46 deg/s): the Classic model scales its turn target to the fast-look speed only once the turn
  // velocity is near the normal maximum (loc_825B10A8: |V| > |max| - margin), which a push short of full never
  // reaches. So at or past the threshold the look update is given the same direction at full magnitude (Rare's
  // own tests then engage fast look as for a full push); below it nothing changes. The rule uses the stick's
  // round magnitude after the inner deadzone (the pad filter hook below, read only).
  constexpr uint32_t kHaloOnMag = 0x820F0000u - 19188u;  // lfs at 825B0494
  constexpr uint32_t kHaloOffMag = 0x82090000u - 26228u; // r30 + 0, lfs at 825B0504
  constexpr uint32_t kHaloOnX = kHaloOffMag + 8820u;     // r30 + 8820, lfs at 825B04A4
  constexpr uint32_t kPdOnX = 0x820A0000u - 24260u + 12u;  // lfs at 825B0E58
  constexpr uint32_t kPdOnMag2 = 0x820F0000u - 17268u;   // lfs at 825B0E68
  constexpr uint32_t kPdOffMag2 = 0x820F0000u - 17272u;  // lfs at 825B0E80
  struct StickRim
  {
    float magnitude = 0.0f;  // round, after the inner deadzone, 0-1
    float horizontal = 0.0f; // |cos| of the angle from horizontal
    bool valid = false;
  };
  std::array<StickRim, 4> g_rim{};
  std::atomic<uint64_t> g_fast_look_engaged{0}; // look updates given a full push

  double FastLookThreshold()
  {
    return std::clamp(REXCVAR_GET(pdz_look_fastlook_threshold), 0.5, 1.0);
  }

  // Rare's constants (read once; logged with the threshold so a log shows them).
  struct RareFastLook
  {
    float halo_on = 0.98f, halo_x = -1.0f, halo_off = 0.95f, pd_x = 0.9f, pd_on2 = 0.9801f, pd_off2 = 0.9409f;
    bool read = false;
  } g_rare_fast_look;

  float SaneFraction(float v, float fallback)
  {
    return std::isfinite(v) && v > 0.05f && v <= 1.0f ? v : fallback;
  }

  const RareFastLook &RareFastLookConstants(const uint8_t *base)
  {
    RareFastLook &r = g_rare_fast_look;
    if (!r.read)
    {
      r.read = true;
      r.halo_on = SaneFraction(LoadBeFloat(base, kHaloOnMag), 0.98f);
      // TU3's value is -1.0 (log b0_fl_rare, 31 Oct): x only has to be inside (-1, 1), no angle limit.
      const float halo_x = LoadBeFloat(base, kHaloOnX);
      r.halo_x = std::isfinite(halo_x) && halo_x > -1.0f && halo_x < 1.0f ? halo_x : -1.0f;
      r.halo_off = SaneFraction(LoadBeFloat(base, kHaloOffMag), 0.95f);
      r.pd_x = SaneFraction(LoadBeFloat(base, kPdOnX), 0.9f);
      r.pd_on2 = SaneFraction(LoadBeFloat(base, kPdOnMag2), 0.9801f);
      r.pd_off2 = SaneFraction(LoadBeFloat(base, kPdOffMag2), 0.9409f);
      REXLOG_INFO("pdz_look: fast look threshold {:.2f} (pdz_look_fastlook_threshold; 1 = Rare's). Rare's own: Standard "
                  "model on at |look| >= {:.4f} with x > {:.4f}, off below {:.4f}; Classic model on at |x| > {:.4f} and "
                  "|look|^2 > {:.4f}, off below {:.4f} (raw {:.4f} / {:.4f} / {:.4f} / {:.4f} / {:.4f} / {:.4f})",
                  FastLookThreshold(), r.halo_on, r.halo_x, r.halo_off, r.pd_x, r.pd_on2, r.pd_off2,
                  LoadBeFloat(base, kHaloOnMag), LoadBeFloat(base, kHaloOnX), LoadBeFloat(base, kHaloOffMag),
                  LoadBeFloat(base, kPdOnX), LoadBeFloat(base, kPdOnMag2), LoadBeFloat(base, kPdOffMag2));
    }
    return r;
  }

  // Promotion to full push: per pad, on at the threshold, off below threshold - kFastLookHysteresis (no flicker at
  // the line). Only for the player's on-foot look update, not zoomed, with his Fast Look option on and
  // pdz_look_tuning; 1.0 = never.
  constexpr double kFastLookHysteresis = 0.02;
  std::array<bool, 4> g_full_push{};

  bool FullPush(int32_t pad, bool zoomed, bool fast_look_option)
  {
    const double threshold = FastLookThreshold();
    if (pad < 0 || pad >= 4)
    {
      return false;
    }
    bool &on = g_full_push[size_t(pad)];
    const StickRim &rim = g_rim[size_t(pad)];
    if (!LookTuning() || threshold >= 1.0 || zoomed || !fast_look_option || !rim.valid)
    {
      on = false;
      return false;
    }
    on = rim.magnitude >= (on ? threshold - kFastLookHysteresis : threshold);
    return on;
  }

  // With Fast Look off (player vtable +0x214 [+0x210] = sub_825AC9B8 false)
  // the look code multiplies the horizontal sensitivity (+0x208 [+0x204] =
  // sub_825AC948) by 1.2.
  constexpr double kFastLookOffSensitivity = 1.2;
  constexpr float kZoomedThreshold = 0.001f;
  // Set during the player's on-foot look update: report Fast Look as off
  // (and undo its sensitivity bonus) so zooming brakes at the normal rate.
  thread_local bool t_suppress_fast_look = false;
  // The player's Fast Look setting as the game last reported it. The look
  // code asks for the sensitivity before the setting, so the bonus is only
  // undone when the setting was really on.
  std::atomic<bool> g_fast_look_setting{true};

  // Curve exponent the look code will use (for pdz_look_radial's pre-shaping):
  // the records' field 2 blended by sensitivity as the look code does, or the
  // default record's with pdz_look_fixed_curve.
  std::atomic<float> g_curve_exponent{1.5f};

  // Split screen (log 099: player 2's turn sometimes slow; logs 098-099): the
  // two values above were one for every local player, so player 2's zoomed
  // sensitivity was corrected by player 1's Fast Look option, and his stick
  // pre-shaped with player 1's curve. Each player's own now: the Fast Look
  // option per player (the game thread asks it in each player's look update),
  // the exponent per pad (the pad filter has only the slot). With one player
  // they hold what the single values held.
  struct FastLookSeen
  {
    uint32_t player = 0;
    bool on = true;
  };
  std::array<FastLookSeen, 4> g_fast_look_players{}; // game thread only
  std::atomic<float> g_pad_curve_exponent[4] = {-1.0f, -1.0f, -1.0f, -1.0f}; // < 0: no look update seen

  void RememberFastLook(uint32_t player, bool on)
  {
    FastLookSeen *slot = nullptr;
    for (FastLookSeen &seen : g_fast_look_players)
    {
      if (seen.player == player)
      {
        slot = &seen;
        break;
      }
      if (!slot && !seen.player)
      {
        slot = &seen;
      }
    }
    if (!slot)
    {
      // A fifth player object (players come and go between matches): take the first slot.
      slot = &g_fast_look_players[0];
    }
    slot->player = player;
    slot->on = on;
  }

  bool FastLookOf(uint32_t player)
  {
    for (const FastLookSeen &seen : g_fast_look_players)
    {
      if (player && seen.player == player)
      {
        return seen.on;
      }
    }
    return g_fast_look_setting.load(std::memory_order_relaxed);
  }

  float CurveExponentForSlot(uint32_t slot)
  {
    const float own = slot >= 4 && slot < 8 ? g_pad_curve_exponent[slot - 4].load(std::memory_order_relaxed) : -1.0f;
    return own > 0.0f ? own : g_curve_exponent.load(std::memory_order_relaxed);
  }

  uint32_t Record(const uint8_t *base, uint32_t level, uint32_t halo)
  {
    const uint32_t record = LoadBe32(base, kAimParamTable + (level * 2 + halo) * 4);
    return IsGuestPointer(record) ? record : 0;
  }

  float CurveExponent(const uint8_t *base, uint32_t input)
  {
    const uint32_t halo = LoadBe32(base, input + kInputStyle) == 1 ? 1 : 0;
    const auto exponent = [&](uint32_t level)
    {
      const uint32_t record = Record(base, level, halo);
      return record ? LoadBeFloat(base, record + kCurveExponent * 4) : 1.5f;
    };
    const float max = exponent(0), def = exponent(1), min = exponent(2);
    if (LookTuning() && REXCVAR_GET(pdz_look_fixed_curve))
    {
      return def;
    }
    float s = LoadBeFloat(base, input + kInputSensitivity);
    if (base[input + kInputSensitivityBoost])
    {
      s *= 1.75f;
    }
    if (s > 0.5f)
    {
      return def + (max - def) * (s - 0.5f) * 2.0f;
    }
    if (s < 0.5f)
    {
      return min + (def - min) * s * 2.0f;
    }
    return def;
  }

  bool Radial()
  {
    return LookTuning() && REXCVAR_GET(pdz_look_radial);
  }

  bool TuningActive()
  {
    return LookTuning() &&
           (REXCVAR_GET(pdz_look_radial) || REXCVAR_GET(pdz_look_snappy) || REXCVAR_GET(pdz_look_fixed_curve));
  }

  struct SavedRecords
  {
    std::array<uint32_t, kAimParamRecords> address{};
    std::array<std::array<uint32_t, kAimParamFloats>, kAimParamRecords> bits{};
  };

  // Patches the six look records for one look update (restored after).
  void ApplyTuning(uint8_t *base, SavedRecords &saved)
  {
    const bool snappy = REXCVAR_GET(pdz_look_snappy);
    const float inertia = float(std::clamp(REXCVAR_GET(pdz_look_inertia_scale), 0.1, 1.0));
    const bool fixed_curve = REXCVAR_GET(pdz_look_fixed_curve);
    const bool radial = REXCVAR_GET(pdz_look_radial);
    for (uint32_t i = 0; i < kAimParamRecords; ++i)
    {
      const uint32_t record = LoadBe32(base, kAimParamTable + i * 4);
      saved.address[i] = IsGuestPointer(record) ? record : 0;
      if (!saved.address[i])
      {
        continue;
      }
      for (uint32_t f = 0; f < kAimParamFloats; ++f)
      {
        saved.bits[i][f] = LoadBe32(base, record + f * 4);
      }
    }
    for (uint32_t i = 0; i < kAimParamRecords; ++i)
    {
      const uint32_t record = saved.address[i];
      if (!record)
      {
        continue;
      }
      const auto scale = [&](uint32_t field, float factor)
      {
        StoreBeFloat(base, record + field * 4, LoadBeFloat(base, record + field * 4) * factor);
      };
      if (snappy)
      {
        // Faster response, not a faster turn: the targets (fields 0 / 1 / 11)
        // and the fast-look ramp (10) are untouched.
        scale(kAccel, 1.0f / inertia);
        scale(kDecel, 1.0f / inertia);
      }
      if (radial)
      {
        // One round deadzone in the pad filter instead; the records' per-axis
        // ones would cut into it again.
        StoreBeFloat(base, record + kYawDeadzone * 4, 0.0f);
        StoreBeFloat(base, record + kPitchDeadzone * 4, 0.0f);
      }
      if (fixed_curve)
      {
        // Records i: level = i / 2 (0 max, 1 default, 2 min), style = i % 2.
        const uint32_t def = saved.address[2 + (i % 2)];
        if (def)
        {
          StoreBe32(base, record + kCurveExponent * 4, saved.bits[2 + (i % 2)][kCurveExponent]);
        }
      }
    }
  }

  void RestoreTuning(uint8_t *base, const SavedRecords &saved)
  {
    for (uint32_t i = 0; i < kAimParamRecords; ++i)
    {
      if (!saved.address[i])
      {
        continue;
      }
      for (uint32_t f = 0; f < kAimParamFloats; ++f)
      {
        StoreBe32(base, saved.address[i] + f * 4, saved.bits[i][f]);
      }
    }
  }
} // namespace

namespace
{
  // pdz_look_slow_detect: one look update of the on-foot player (after Rare's update).
  void SlowTurnDetect(uint8_t *base, uint32_t owner, uint32_t state, uint32_t input, double dt, float yaw_before,
                      bool zoom_brake)
  {
    if (!(dt > 0.0005 && dt < 0.1))
    {
      return;
    }
    const int32_t pad = int32_t(LoadBe32(base, input + kInputPad));
    if (pad < 0 || pad >= 4 || !g_pad_sample[pad].valid)
    {
      return;
    }
    const PadSample &p = g_pad_sample[pad];
    SlowDetect &d = g_slow[pad];
    d.since_line += dt;
    // Raw sideways deflection after the quiet-stick deadzone (5000), from the physical centre.
    const float dz = 5000.0f;
    const float ax = float(std::abs(p.raw[0]));
    const float defl = ax > dz ? std::min(1.0f, (ax - dz) / (32767.0f - dz)) : 0.0f;
    const int dir = defl >= 0.8f ? (p.raw[0] < 0 ? -1 : 1) : 0;
    float turn = LoadBeFloat(base, owner + kPlayerViewYaw) - yaw_before;
    while (turn > 180.0f)
    {
      turn -= 360.0f;
    }
    while (turn < -180.0f)
    {
      turn += 360.0f;
    }
    const float rate = std::fabs(turn) / float(dt);
    const float zoom = LoadBeFloat(base, owner + kPlayerZoom);
    const bool zoomed = zoom > kZoomedThreshold;
    d.peak *= float(std::exp2(-dt / 120.0));
    if (dir != d.push_dir)
    {
      d.push_dir = dir;
      d.push_s = 0.0;
    }
    else if (dir)
    {
      d.push_s += dt;
    }
    if (!dir || zoomed || base[state + kStateGameTurn] || d.push_s < 0.4)
    {
      return;
    }
    // A normal full push raises the peak (only pushes held long enough to be past the ramp count).
    const bool slow = d.peak >= 30.0f && rate < 0.6f * d.peak;
    if (!slow)
    {
      d.peak = std::max(d.peak, rate);
    }
    const bool compare = d.compare_dir == dir;
    if ((!slow && !compare) || d.since_line < 0.5)
    {
      return;
    }
    if (slow)
    {
      ++d.fired;
      d.compare_dir = -dir; // the other way next, for comparison
    }
    else
    {
      d.compare_dir = 0;
    }
    d.since_line = 0.0;
    const float v_yaw = LoadBeFloat(base, state + kStateYawVelocity) * 60.0f;
    const float strength = LoadBeFloat(base, state + kStateAssistStrength);
    const float assist_yaw = -strength * LoadBeFloat(base, state + kStateAssistRate);
    REXLOG_INFO("pdz_look: {} pad {} {} push {:.2f} s | turn {:.1f} deg/s vs expected {:.1f} (recent full-push peak; "
                "{:.0f}%) | pad raw {:+d},{:+d} -> filtered {:+d},{:+d} (deadzone {:.0f}/{:.0f}, learned centre "
                "{:+.0f},{:+.0f}, jitter {}/{}, samples {}/{}) | input raw {:+.3f},{:+.3f} look {:+.3f},{:+.3f} | stick "
                "rate (V x 60) {:+.1f}, assist strength {:.2f} turn {:+.1f} deg/s (auto aim option {}) | fast look {} "
                "| sensitivity {:.3f}/{:.3f} scales {:.2f}/{:.2f} | zoom {:.3f} zoom brake {} | look tuning {} "
                "(radial {}, snappy {}, fixed curve {}) | style {} | dt {:.2f} ms | slow lines {} | fast look rule: "
                "threshold {:.2f}, stick {:.3f} at {:.1f} deg from horizontal, {} (full-push frames so far {}) | rim "
                "ranges from the learned centre x -{:.0f}/+{:.0f} y -{:.0f}/+{:.0f} (scaled to 32767 each), radial "
                "magnitude {} | Rare's filter range x {:.0f} y {:.0f}",
                slow ? "SLOW TURN" : "comparison (the other way)", pad, dir < 0 ? "LEFT" : "RIGHT", d.push_s, rate,
                d.peak, d.peak > 0.0f ? 100.0f * rate / d.peak : 0.0f, p.raw[0], p.raw[1], p.out[0], p.out[1],
                p.deadzone[0], p.deadzone[1], p.centre[0], p.centre[1], p.jitter[0], p.jitter[1], p.samples[0],
                p.samples[1], LoadBeFloat(base, input + kInputStickRaw), LoadBeFloat(base, input + kInputStickRaw + 4),
                LoadBeFloat(base, input + kInputStickLook), LoadBeFloat(base, input + kInputStickLook + 4), v_yaw,
                strength, assist_yaw, LoadBe32(base, input + kInputAutoAimOption), base[state + kStateFastLook],
                LoadBeFloat(base, input + kInputSensitivity), LoadBeFloat(base, input + kInputSensitivity + 4),
                LoadBeFloat(base, state + kStateYawScale), LoadBeFloat(base, state + kStatePitchScale), zoom,
                zoom_brake ? "on" : "off", LookTuning() ? "on" : "off", REXCVAR_GET(pdz_look_radial) ? "on" : "off",
                REXCVAR_GET(pdz_look_snappy) ? "on" : "off", REXCVAR_GET(pdz_look_fixed_curve) ? "on" : "off",
                LoadBe32(base, input + kInputStyle), dt * 1000.0, d.fired, FastLookThreshold(),
                g_rim[size_t(pad)].magnitude,
                std::acos(std::clamp(double(g_rim[size_t(pad)].horizontal), 0.0, 1.0)) * 180.0 / 3.14159265,
                FastLookThreshold() >= 1.0 ? "off (Rare's)"
                : !LookTuning()            ? "off (pdz_look_tuning)"
                : zoomed                   ? "not while zoomed"
                : !FastLookOf(owner)       ? "off (Fast Look option off)"
                : g_full_push[size_t(pad)] ? "full push: fast look allowed"
                                           : "below the threshold",
                g_fast_look_engaged.load(), p.rim_neg[0], p.rim_pos[0], p.rim_neg[1], p.rim_pos[1],
                p.radial_magnitude < 0.0f ? std::string("off") : fmt::format("{:.4f}", p.radial_magnitude),
                32767.0f - std::fabs(p.centre[0]) - p.deadzone[0], 32767.0f - std::fabs(p.centre[1]) - p.deadzone[1]);
  }
} // namespace

REX_HOOK_RAW(sub_825B3AC0)
{
  const uint32_t owner = ctx.r3.u32;
  const uint32_t state = ctx.r5.u32;
  const bool on_foot = owner && ctx.r4.u32 == owner && state == owner + kPlayerLookState;
  pdz::mouse::PumpTestMotion(); // pdz_mouse_test_motion (testing tool)
  if (owner && ctx.r4.u32 != owner)
  {
    // The CamSpy, a turret, the guided rocket, the hovercraft or the jetpack (mouse_look.h NoteSteeredLook).
    pdz::mouse::NoteSteeredLook(base, owner, ctx.r4.u32, uint32_t(ctx.lr));
  }
  if (!on_foot)
  {
    pdz::mouse::LookProbe probe;
    pdz::mouse::LookProbeBegin(base, owner, ctx.r4.u32, state, uint32_t(ctx.lr), probe);
    __imp__sub_825B3AC0(ctx, base);
    pdz::mouse::LookProbeEnd(base, probe);
    return;
  }
  pdz::latency::OnPlayerLook(base, owner); // render/latency_log.h: pad 0's player (pdz_latency_log)

  // While the game turns the view itself (ladder mounts etc.), our look
  // options pass the update through untouched (2026-10-24: a ~90 degree
  // swing on mounting a ladder). pdz_look_log logs each such turn.
  if (base[state + kStateGameTurn])
  {
    static bool logged_turn = false;
    if (!logged_turn && REXCVAR_GET(pdz_look_log))
    {
      REXLOG_INFO("pdz_look: game-controlled turn: yaw {:.3f} -> {:.3f}, pitch {:.3f} -> {:.3f}, over {:.2f} s",
                  LoadBeFloat(base, state + kStateGameTurnFrom), LoadBeFloat(base, state + kStateGameTurnTo),
                  LoadBeFloat(base, state + kStateGameTurnFrom + 4), LoadBeFloat(base, state + kStateGameTurnTo + 4),
                  LoadBeFloat(base, state + kStateGameTurnTime));
    }
    logged_turn = true;
    __imp__sub_825B3AC0(ctx, base);
    if (!base[state + kStateGameTurn])
    {
      logged_turn = false;
    }
    return;
  }
  const uint32_t input = LoadBe32(base, owner + kPlayerInput);
  if (IsGuestPointer(input))
  {
    const float exponent = CurveExponent(base, input);
    g_curve_exponent.store(exponent, std::memory_order_relaxed);
    const int32_t pad = int32_t(LoadBe32(base, input + kInputPad));
    if (pad >= 0 && pad < 4)
    {
      g_pad_curve_exponent[pad].store(exponent, std::memory_order_relaxed); // log 099: his own pad's
    }
  }
  const bool zoom_brake = LookTuning() && REXCVAR_GET(pdz_fastlook_zoom_brake) &&
                          LoadBeFloat(base, owner + kPlayerZoom) > kZoomedThreshold;
  if (zoom_brake)
  {
    base[state + kStateFastLook] = 0;
  }
  // pdz_fix_halo_zoom_curve patches the Halo record for this update; the
  // controls options scale what it left.
  game_fixes::LookPatch game_patch;
  game_fixes::BeforeLook(base, owner, input, game_patch);
  SavedRecords saved;
  const bool tuned = TuningActive();
  if (tuned)
  {
    ApplyTuning(base, saved);
  }
  t_suppress_fast_look = zoom_brake;
  const double dt = ctx.f1.f64;
  const float yaw_before = LoadBeFloat(base, owner + kPlayerViewYaw);
  // pdz_look_fastlook_threshold: at or past the threshold the update reads the stick's direction at full
  // magnitude (look / magnitude: the radial pre-shape kept), restored after it; below it, untouched.
  RareFastLookConstants(base);
  uint32_t promoted_bits[2] = {};
  bool promoted = false;
  if (IsGuestPointer(input))
  {
    const int32_t pad = int32_t(LoadBe32(base, input + kInputPad));
    const bool zoomed = LoadBeFloat(base, owner + kPlayerZoom) > kZoomedThreshold;
    if (FullPush(pad, zoomed, FastLookOf(owner)) && g_rim[size_t(pad)].magnitude > 0.05f)
    {
      const float m = g_rim[size_t(pad)].magnitude;
      for (uint32_t axis = 0; axis < 2; ++axis)
      {
        const uint32_t at = input + kInputStickLook + 4 * axis;
        promoted_bits[axis] = LoadBe32(base, at);
        StoreBeFloat(base, at, std::clamp(LoadBeFloat(base, at) / m, -1.0f, 1.0f));
      }
      promoted = true;
      g_fast_look_engaged.fetch_add(1, std::memory_order_relaxed);
    }
  }
  __imp__sub_825B3AC0(ctx, base);
  if (promoted)
  {
    StoreBe32(base, input + kInputStickLook, promoted_bits[0]);
    StoreBe32(base, input + kInputStickLook + 4, promoted_bits[1]);
  }
  t_suppress_fast_look = false;
  if (REXCVAR_GET(pdz_look_slow_detect) && IsGuestPointer(input))
  {
    SlowTurnDetect(base, owner, state, input, dt, yaw_before, zoom_brake);
  }
  if (tuned)
  {
    RestoreTuning(base, saved);
  }
  game_fixes::AfterLook(base, game_patch);
}

namespace look_tuning
{
  // The player's Fast Look option as the game last reported it.
  bool FastLookSetting()
  {
    return g_fast_look_setting.load(std::memory_order_relaxed);
  }

  // This player's own Fast Look option (log 099, split screen); the last
  // reported one before the game has asked his.
  bool FastLookSetting(uint32_t player)
  {
    return FastLookOf(player);
  }
} // namespace look_tuning

// Player vtable +0x214 [+0x210]: Fast Look enabled.
REX_HOOK_RAW(sub_825AC9B8)
{
  const uint32_t player = ctx.r3.u32;
  __imp__sub_825AC9B8(ctx, base);
  g_fast_look_setting.store((ctx.r3.u32 & 0xFF) != 0, std::memory_order_relaxed);
  RememberFastLook(player, (ctx.r3.u32 & 0xFF) != 0);
  if (t_suppress_fast_look)
  {
    ctx.r3.u64 = 0;
  }
}

// pdz_fix_sensitivity_clamp (with pdz_controls_fixes; the launch fix in controls_fixes.cpp, ported 2026-11-04):
// the look models blend the min / default / max look records by the sensitivity with no clamp, and the getters
// give up to x1.75 (input +0x702) and the horizontal one x1.2 more with Fast Look off, so past the slider's end
// every field extrapolates beyond the max record (mid-stick turns get slower). Capped at the max record (1.0) for
// the look models' own calls only: the PD model (sub_825B00A8 [825A0888]) and the Halo model (sub_825B0AA0
// [825A1280]).
namespace
{
  constexpr uint32_t kPdHorizontalSensReturn = 0x825B0138;   // [825A0918]
  constexpr uint32_t kPdVerticalSensReturn = 0x825B0150;     // [825A0930]
  constexpr uint32_t kHaloHorizontalSensReturn = 0x825B0B0C; // [825A12EC]
  constexpr uint32_t kHaloVerticalSensReturn = 0x825B0B24;   // [825A1304]
  constexpr double kSensitivityMax = 1.0;                    // the blend parameter reaching the max record

  bool SensitivityClamp() { return REXCVAR_GET(pdz_controls_fixes) && REXCVAR_GET(pdz_fix_sensitivity_clamp); }
} // namespace

// Player vtable +0x208 [+0x204]: horizontal sensitivity. The look models
// then apply the x1.2 bonus when they see Fast Look off; undo it.
REX_HOOK_RAW(sub_825AC948)
{
  const uint32_t player = ctx.r3.u32;
  const uint32_t lr = uint32_t(ctx.lr);
  __imp__sub_825AC948(ctx, base);
  if (t_suppress_fast_look && FastLookOf(player)) // log 099: his own option, not the last player's
  {
    ctx.f1.f64 /= kFastLookOffSensitivity;
  }
  if (SensitivityClamp() && (lr == kPdHorizontalSensReturn || lr == kHaloHorizontalSensReturn))
  {
    // The model multiplies by 1.2 when it sees Fast Look off (the player's option, or the zoom brake's report).
    const bool bonus = t_suppress_fast_look || !FastLookOf(player);
    ctx.f1.f64 = std::min(ctx.f1.f64, bonus ? kSensitivityMax / kFastLookOffSensitivity : kSensitivityMax);
  }
}

// Player vtable +0x20C [+0x208]: vertical sensitivity (x1.75 with input +0x702).
REX_HOOK_RAW(sub_825AC978)
{
  const uint32_t lr = uint32_t(ctx.lr);
  __imp__sub_825AC978(ctx, base);
  if (SensitivityClamp() && (lr == kPdVerticalSensReturn || lr == kHaloVerticalSensReturn))
  {
    ctx.f1.f64 = std::min(ctx.f1.f64, kSensitivityMax);
  }
}

// sub_822733D0 [8226BE48](slot, &x, &y): one stick through the pad filter, in
// place (-32767..32767 per axis). pdz_look_radial, right sticks (slots 4-7):
// the filter's quiet-stick deadzone becomes the round one (jitter zeroed so
// the filter always uses it), then the filtered axes are replaced by the raw
// stick around the filter's learned centre: a round deadzone, the rim as
// full deflection in every direction, and each axis pre-shaped by 1 / e so
// the look code's per-axis |v| ^ e curve comes out as magnitude ^ e along the
// stick's own direction (diagonals as fast as straight).
REX_HOOK_RAW(sub_822733D0)
{
  const uint32_t slot = ctx.r3.u32;
  const uint32_t x_address = ctx.r4.u32;
  const uint32_t y_address = ctx.r5.u32;
  const bool right = slot >= 4 && slot < 8;
  // pdz_mouse_steer_log: pad 0's right stick in and out (declared first: logs after everything below, on every way out).
  struct StickLog
  {
    uint8_t *base;
    uint32_t x_address, y_address;
    int32_t in_x, in_y;
    bool on;
    ~StickLog()
    {
      if (on)
      {
        pdz::mouse::LogPadStick(in_x, in_y, int32_t(LoadBe32(base, x_address)), int32_t(LoadBe32(base, y_address)));
      }
    }
  } stick_log{base, x_address, y_address, slot == 4 ? int32_t(LoadBe32(base, x_address)) : 0,
              slot == 4 ? int32_t(LoadBe32(base, y_address)) : 0, slot == 4 && pdz::mouse::SteerLogOn()};
  // pdz_mouse_invert while the mouse steers the CamSpy / a turret / a vehicle as the right stick
  // (mouse_look.h SteeringInvertY): pad 0's right stick y is flipped after everything below, on every way out.
  struct InvertY
  {
    uint8_t *base;
    uint32_t address;
    bool on;
    ~InvertY()
    {
      if (on)
      {
        const int32_t y = int32_t(LoadBe32(base, address));
        StoreBe32(base, address, uint32_t(std::clamp(-y, -32767, 32767)));
      }
    }
  } invert_y{base, y_address, slot == 4 && pdz::mouse::SteeringInvertY()};
  if (slot == 4)
  {
    // The mouse as pad 0's right stick while the CamSpy / a turret / the guided rocket / a vehicle is steered
    // (mouse_look.h SteerStickForPad). Written here, after the SDK's own stick dead zone (mapping.toml
    // right_stick_deadzone_percentage, 0.2, would swallow slow mouse motion), as the raw stick the filter reads: from
    // its learned centre, just past the dead zone it uses, out to the rim. A controller pushed further wins.
    double mouse_x = 0.0, mouse_y = 0.0;
    pdz::mouse::SteerStickForPad(mouse_x, mouse_y);
    if (mouse_x != 0.0 || mouse_y != 0.0)
    {
      const uint32_t pad = kPadFilterState + slot * kPadFilterStride;
      const uint32_t addresses[2] = {x_address, y_address};
      const double values[2] = {mouse_x, mouse_y};
      for (uint32_t axis = 0; axis < 2; ++axis)
      {
        if (values[axis] == 0.0)
        {
          continue;
        }
        const float centre = std::clamp(LoadBeFloat(base, pad + axis * 4), -8000.0f, 8000.0f);
        const float deadzone = std::clamp(LoadBeFloat(base, pad + kPadDeadzoneInUse + axis * 4), 0.0f, 30000.0f);
        const double room = std::max(0.0, 32767.0 - std::fabs(double(centre)) - double(deadzone) - 2.0);
        const double offset = (double(deadzone) + 1.0 + std::fabs(values[axis]) * room) * (values[axis] < 0 ? -1 : 1);
        const int32_t current = int32_t(LoadBe32(base, addresses[axis]));
        if (std::fabs(double(current) - centre) < std::fabs(offset))
        {
          StoreBe32(base, addresses[axis], uint32_t(int32_t(std::clamp(double(centre) + offset, -32767.0, 32767.0))));
        }
      }
    }
  }
  const bool radial = right && Radial();
  const bool capture = right && REXCVAR_GET(pdz_look_slow_detect);
  const bool rim = right && LookTuning() && FastLookThreshold() < 1.0; // pdz_look_fastlook_threshold
  const int32_t raw_x = radial || capture || rim ? int32_t(LoadBe32(base, x_address)) : 0;
  const int32_t raw_y = radial || capture || rim ? int32_t(LoadBe32(base, y_address)) : 0;
  if (right)
  {
    const uint32_t pad = kPadFilterState + slot * kPadFilterStride;
    bool &applied = g_pad_deadzone_applied[slot - 4];
    if (radial)
    {
      const float radius = float(std::clamp(REXCVAR_GET(pdz_look_radial_deadzone), 0.0, 0.9) * 32767.0);
      for (uint32_t axis = 0; axis < 8; axis += 4)
      {
        StoreBeFloat(base, pad + kPadQuietDeadzone + axis, radius);
        StoreBe32(base, pad + kPadJitter + axis, 0);
      }
      applied = true;
    }
    else if (applied)
    {
      // Back to the game's own deadzone; let it measure the jitter again.
      for (uint32_t axis = 0; axis < 8; axis += 4)
      {
        StoreBeFloat(base, pad + kPadQuietDeadzone + axis, kPadQuietDeadzoneDefault);
        StoreBe32(base, pad + kPadJitter + axis, kPadJitterUnknown);
      }
      applied = false;
    }
  }
  __imp__sub_822733D0(ctx, base);
  if (capture)
  {
    PadSample &p = g_pad_sample[slot - 4];
    p.raw[0] = raw_x;
    p.raw[1] = raw_y;
    p.out[0] = int32_t(LoadBe32(base, x_address));
    p.out[1] = int32_t(LoadBe32(base, y_address));
    const uint32_t pad = kPadFilterState + slot * kPadFilterStride;
    for (uint32_t axis = 0; axis < 2; ++axis)
    {
      p.centre[axis] = LoadBeFloat(base, pad + axis * 4);
      p.deadzone[axis] = LoadBeFloat(base, pad + kPadDeadzoneInUse + axis * 4);
      p.jitter[axis] = int32_t(LoadBe32(base, pad + kPadJitter + axis * 4));
      p.samples[axis] = LoadBe32(base, pad + kPadCentreSamples + axis * 4);
      p.rim_neg[axis] = RimRange(p.centre[axis], true);
      p.rim_pos[axis] = RimRange(p.centre[axis], false);
    }
    p.radial_magnitude = -1.0f; // set below when pdz_look_radial reshapes this sample
    p.valid = true;
  }
  if (right)
  {
    // pdz_look_fastlook_threshold: the stick's round magnitude after the inner deadzone (the radial one, or the
    // filter's own x deadzone without pdz_look_radial) and its angle, from the filter's learned centre, each side
    // scaled to its own rim (ToRimUnits). Read only.
    StickRim &r = g_rim[slot - 4];
    r.valid = false;
    const uint32_t pad = kPadFilterState + slot * kPadFilterStride;
    if (rim && LoadBe32(base, pad + kPadConnected))
    {
      const float cx = LoadBeFloat(base, pad), cy = LoadBeFloat(base, pad + 4);
      const float dx = ToRimUnits(float(raw_x) - cx, cx);
      const float dy = ToRimUnits(float(raw_y) - cy, cy);
      const float deadzone = std::clamp(LoadBeFloat(base, pad + kPadDeadzoneInUse), 0.0f, 30000.0f);
      const float length = std::hypot(dx, dy);
      r.magnitude = length > deadzone ? std::min(1.0f, (length - deadzone) / (32767.0f - deadzone)) : 0.0f;
      r.horizontal = length > 0.0f ? std::fabs(dx) / length : 0.0f;
      r.valid = true;
    }
  }
  if (!radial)
  {
    return;
  }
  const uint32_t pad = kPadFilterState + slot * kPadFilterStride;
  if (!LoadBe32(base, pad + kPadConnected))
  {
    return;
  }
  // From the learned centre, each side of each axis scaled to its own rim (RimRange: log 414, a full push left
  // stopped at 0.989 with the centre at -330); with the centre at 0 the scale is exactly 1, as before.
  const float cx = LoadBeFloat(base, pad), cy = LoadBeFloat(base, pad + 4);
  const float dx = ToRimUnits(float(raw_x) - cx, cx);
  const float dy = ToRimUnits(float(raw_y) - cy, cy);
  const float deadzone = std::clamp(LoadBeFloat(base, pad + kPadDeadzoneInUse), 0.0f, 30000.0f);
  const float length = std::hypot(dx, dy);
  float out_x = 0.0f, out_y = 0.0f;
  if (capture)
  {
    g_pad_sample[slot - 4].radial_magnitude = 0.0f;
  }
  if (length > deadzone)
  {
    const float magnitude = std::min(1.0f, (length - deadzone) / (32767.0f - deadzone));
    if (capture)
    {
      g_pad_sample[slot - 4].radial_magnitude = magnitude;
    }
    const float inv_e = 1.0f / std::max(0.1f, CurveExponentForSlot(slot)); // log 099: this pad's player
    // The stick's direction, snapped to the nearest axis within the snap
    // angle and faded back to itself by twice the angle (continuous).
    float ux = dx / length, uy = dy / length;
    const float snap = float(std::clamp(REXCVAR_GET(pdz_look_axial_snap_deg), 0.0, 20.0)) * 3.14159265f / 180.0f;
    if (snap > 0.0f)
    {
      const float theta = std::atan2(std::fabs(uy), std::fabs(ux)); // 0 = horizontal, pi/2 = vertical
      const bool near_x = theta < 3.14159265f / 4.0f;
      const float off = near_x ? theta : 3.14159265f / 2.0f - theta; // angle from the nearest axis
      const float kept = off <= snap ? 0.0f : off < 2.0f * snap ? 2.0f * (off - snap) : off;
      const float t = near_x ? kept : 3.14159265f / 2.0f - kept;
      ux = std::copysign(std::cos(t), dx);
      uy = std::copysign(std::sin(t), dy);
    }
    const auto shape = [&](float u)
    {
      return std::copysign(magnitude * std::pow(std::fabs(u), inv_e), u);
    };
    out_x = shape(ux);
    out_y = shape(uy);
  }
  StoreBe32(base, x_address, uint32_t(int32_t(std::lround(out_x * 32767.0f))));
  StoreBe32(base, y_address, uint32_t(int32_t(std::lround(out_y * 32767.0f))));
}
