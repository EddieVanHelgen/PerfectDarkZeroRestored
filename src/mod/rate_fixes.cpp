// Frame-rate fixes (0.9.2.1, overnight 7-8 Nov, items A2-A4; the 7 Nov sweep, findings F1 / F2 and
// S2_per_frame_effects candidate 3): per-frame steps Rare tuned for the shipped 30 fps, put back on Rare's 30 fps
// rate at 60 / 120 fps the way AI55's taunt roll (pdz_fix_ai_taunt_rate, ai_game_fixes.cpp) was: from the frame's
// dt. A per-frame share k becomes k' = 1 - (1 - k)^(dt x 30), a per-frame chance p becomes p' = 1 - (1 - p)^(dt x 30):
// at dt = 1/30 both are Rare's values, and two frames at 60 (four at 120) do what one frame did at 30. Each fix
// needs pdz_game_fixes (F11 flips it) and logs one "pdz_fix:" line the first time it changes something.
//
// - pdz_fix_turret_rate (A2, F1): Bridge's ship turret (prop class DP_Turret, type 49, vtable 820CA748, update
//   sub_825D0B28 = vtable slot 1, dt in f1) turns yaw +0x3BC by 0.1 (820DFC98) and pitch +0x3C0 by 0.2 (82073E54) of
//   the remaining angle per call, with no dt: state 2 yaw only (825D1338, then |gap| < 1 deg (820DFD34) -> state 3,
//   wait 0.2 s), state 3 both (825D1298 / 825D12A0, both gaps < 0.1 deg -> state 4 = fire after 0.2 s, or state 1),
//   state 0 back to centre (825D150C / 825D1514). Its waits (+0x3CC, +0x3FC) use dt. So at 120 fps it lines up and
//   fires about 4x sooner. The angle steps are the last thing the update does (the barrel matrix is built from the
//   angles at its start), so the fix wraps the update: when a step ran (the angle changed), the angle is redone from
//   the value before the call with k', and a state change the step made is undone (state and wait back) when the
//   redone angle is not yet inside the threshold, so the turret moves on exactly when Rare's 30 fps one would.
//   sub_825D0AF0 (the same step to an f2 / f3 target) has no caller or table entry: dead. Only above 30 fps
//   (dt < 1/30); slower frames keep Rare's per-frame step (as the 360 did when it dropped frames).
// - pdz_fix_idle_fidget_rate (A3, F2): the held-gun animation controller (sub_824FA578, from the character update via
//   sub_824F76B0) rolls rand01 (sub_82702860, called at 824FAA80) < 0.005 (820EB250) every frame while the character
//   stands (+0x300 < 0.1) and no gun animation plays, and starts idle slot 10-13: a fidget every 6.7 s on average at
//   30 fps, 3.3 s at 60, 1.7 s at 120. The roll's value is only compared, so the fix scales it on its return
//   (lr 824FAA84) by p / p': it comes in under 0.005 with chance p'. Every character's controller, the player's
//   included.
// - pdz_fix_screen_flicker_rate (A4, S2 candidate 3): frameBufferFxUpdate (sub_825F5290, a main task, dt in f10)
//   decays the flicker 82DA4884 by 4/s (dt), and while it is 0 rolls rand01 (call at 825F5498) < 0.01 (8208BBF4)
//   every frame to start a new one (0.2 + 0.3 x rand01, so 0.05-0.125 s): ~0.3 flickers/s at 30 fps, 0.6 at 60, 1.2
//   at 120. Only the effect pass sub_825F9C68 reads it (825FA08C, a pixel shader constant), for framebuffer-effect
//   modes 10 and 13 (per view, 82E3C5C0[view], set only by the camera from its PDZ_FramebufferFx channel, 8220E740).
//   Same fix as A3 on the roll's return (lr 825F549C). pdz_rate_fix_log also logs view 0's mode when it changes, so a
//   run can show which scenes use 10 / 13.
//
// Diagnostic pdz_rate_fix_log (off): turret state changes and, per shot, the measured turn time (state 1 -> 4) next
// to the 30 fps prediction for the same angles; for each roll fix a line per hit and a summary per second of rolling
// time: hits per rolling second against Rare's 30 fps rate (-30 ln(1 - p)) and the unfixed rate at this frame rate.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>
#include "hook_profile.h" // pdz_hook_profile (must follow <rex/hook.h>)

#include "../game_cvars.h" // pdz_game_fixes

REXCVAR_DEFINE_BOOL(pdz_fix_turret_rate, true, "Gameplay",
                    "Bridge's ship turret turns onto its target at Rare's 30 fps rate at 60 / 120 fps (it turned a "
                    "fixed share of the gap per frame, so 2x / 4x faster and fired sooner). On by default. Needs "
                    "pdz_game_fixes")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_rate_fix_log, false, "Debug",
                    "Diagnostic for the frame-rate fixes (pdz_fix_turret_rate, _idle_fidget_rate, _screen_flicker_rate): "
                    "the turret's state changes and turn time per shot against the 30 fps prediction; each idle fidget "
                    "/ flicker start and a per-second rate summary against Rare's 30 fps rate; view 0's framebuffer fx "
                    "mode when it changes")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_fix_idle_fidget_rate, true, "Gameplay",
                    "Gun idle fidgets (standing still) come at Rare's 30 fps rate at 60 / 120 fps (the 1-in-200 "
                    "chance was rolled per frame, so 2x / 4x as often). On by default. Needs pdz_game_fixes")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_fix_screen_flicker_rate, true, "Gameplay",
                    "The random screen flicker of framebuffer-effect modes 10 / 13 starts at Rare's 30 fps rate at "
                    "60 / 120 fps (the 1-in-100 chance was rolled per frame, so 2x / 4x as often). On by default. "
                    "Needs pdz_game_fixes")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_test_turret_wake, false, "Testing",
                    "TESTING TOOL, not a game option (pdz_fix_turret_rate's scripted test): an idle DP_Turret "
                    "(Bridge's ship turret, state 0 with no target) is given the local player as its target and "
                    "switched on, as Rare's multiplayer branch of its update does, so it turns and fires at him "
                    "wherever he stands (use with pdz_test_god_mode). Logs one 'pdz_test: turret woken' line. Off by "
                    "default")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REX_EXTERN(__imp__sub_825D0B28); // DP_Turret update (vtable 820CA748 slot 1): r3 turret, f1 dt
REX_EXTERN(sub_821433D8);        // Rare's reference setter (ref, object)
REX_EXTERN(__imp__sub_82702860); // rand01: f1 in [0, 1)

namespace
{
  uint32_t Load32(const uint8_t *base, uint32_t a)
  {
    const uint8_t *p = base + a;
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
  }

  void Store32(uint8_t *base, uint32_t a, uint32_t v)
  {
    uint8_t *p = base + a;
    p[0] = uint8_t(v >> 24);
    p[1] = uint8_t(v >> 16);
    p[2] = uint8_t(v >> 8);
    p[3] = uint8_t(v);
  }

  float LoadF(const uint8_t *base, uint32_t a)
  {
    const uint32_t bits = Load32(base, a);
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
  }

  void StoreF(uint8_t *base, uint32_t a, float f)
  {
    uint32_t bits;
    std::memcpy(&bits, &f, 4);
    Store32(base, a, bits);
  }

  bool IsGuestPointer(uint32_t a)
  {
    return a >= 0x00010000u && a < 0xFFFF0000u;
  }

  bool Fix(bool option)
  {
    return REXCVAR_GET(pdz_game_fixes) && option;
  }

  // k' = 1 - (1 - k)^(dt x 30): Rare's per-frame share at 30 fps, for a frame of dt seconds.
  float RateShare(float k, double steps)
  {
    return float(1.0 - std::pow(1.0 - double(k), steps));
  }

  constexpr uint32_t kFrameDt = 0x82D6BFD8 + 120; // the game's frame time (float s, sub_82140500, capped at 0.1)

  // ---- per-frame chances (rand01 < p) ----
  struct RollFix
  {
    const char *name;
    uint32_t roll_return; // the rand01 call's return address
    float chance;         // Rare's per-frame p (the constant the roll is compared with)
    std::atomic<bool> logged{false};
    // pdz_rate_fix_log (game thread only)
    double window_s = 0.0, total_s = 0.0, unfixed = 0.0;
    uint32_t window_rolls = 0, window_hits = 0;
    uint64_t total_hits = 0;
  };
  RollFix g_idle{"idle fidget", 0x824FAA84, 0.005f};
  RollFix g_flicker{"screen flicker", 0x825F549C, 0.01f};
  constexpr uint32_t kFbFxMode = 0x82E3C5C0; // u32 per view
  uint32_t g_fbfx_mode_seen = ~0u;

  void RollLog(RollFix &f, const PPCContext &ctx, float dt, double p_now, bool on)
  {
    const bool hit = ctx.f1.f64 < double(f.chance);
    const double d = std::clamp(double(dt), 0.0, 0.1);
    f.window_s += d;
    f.total_s += d;
    f.unfixed += -std::log(1.0 - double(f.chance)); // Rare's p on every frame: the unfixed expectation
    ++f.window_rolls;
    if (hit)
    {
      ++f.window_hits;
      ++f.total_hits;
      REXLOG_INFO("rate_fix: {} hit (r31 {:08X}; p this frame {:.5f}; dt {:.5f}; fix {})", f.name, ctx.r31.u32,
                  p_now, dt, on ? "on" : "off");
    }
    if (f.window_s >= 1.0)
    {
      const double rare = -30.0 * std::log(1.0 - double(f.chance));
      REXLOG_INFO("rate_fix: {}: last {:.2f} s of rolling {} rolls ({:.1f}/s) {} hits | total {:.1f} s {} hits = "
                  "{:.3f}/s (Rare's 30 fps {:.3f}/s; unfixed here {:.3f}/s; fix {})",
                  f.name, f.window_s, f.window_rolls, f.window_rolls / f.window_s, f.window_hits, f.total_s,
                  f.total_hits, f.total_s > 0.0 ? double(f.total_hits) / f.total_s : 0.0, rare,
                  f.total_s > 0.0 ? f.unfixed / f.total_s : 0.0, on ? "on" : "off");
      f.window_s = 0.0;
      f.window_rolls = 0;
      f.window_hits = 0;
    }
  }

  // After rand01 returned to the roll: scale its value so that it is under p with chance p' = 1 - (1 - p)^(dt x 30).
  void RateRoll(RollFix &f, PPCContext &ctx, uint8_t *base, bool on)
  {
    const float dt = LoadF(base, kFrameDt);
    const double steps = std::clamp(double(dt), 0.0, 0.1) * 30.0;
    double p_now = f.chance;
    if (on && std::fabs(steps - 1.0) > 1e-3)
    {
      p_now = 1.0 - std::pow(1.0 - double(f.chance), steps);
      const double r = ctx.f1.f64;
      ctx.f1.f64 = p_now > 0.0 ? double(float(r * (double(f.chance) / p_now))) : 1.0;
      if (!f.logged.exchange(true, std::memory_order_relaxed))
      {
        REXLOG_INFO("pdz_fix: {} rate: the per-frame chance {} runs at Rare's 30 fps rate (dt {:.5f} s: {:.5f} per "
                    "frame)",
                    f.name, f.chance, dt, p_now);
      }
    }
    if (REXCVAR_GET(pdz_rate_fix_log))
    {
      RollLog(f, ctx, dt, p_now, on);
    }
  }

  // ---- A2: the DP_Turret ----
  constexpr uint32_t kTurretState = 0x3B0; // u32: 0 centre, 1 pick target, 2 yaw, 3 yaw + pitch, 4 fire
  constexpr uint32_t kTurretYaw = 0x3BC;   // degrees
  constexpr uint32_t kTurretPitch = 0x3C0;
  constexpr uint32_t kTurretYawTarget = 0x3C4;
  constexpr uint32_t kTurretPitchTarget = 0x3C8;
  constexpr uint32_t kTurretWait = 0x3CC; // s, counts down by dt
  constexpr float kYawShare = 0.1f;       // 820DFC98
  constexpr float kPitchShare = 0.2f;     // 82073E54
  constexpr float kYawDone = 1.0f;        // 820DFD34, state 2 -> 3
  constexpr float kAimDone = 0.1f;        // 820DFC98, state 3 -> 4 (both gaps)
  constexpr uint32_t kTurretHasTarget = 0x3D0; // u8: aim at the referenced object (else at the point +0x3DC)
  constexpr uint32_t kTurretTargetRef = 0x3D4;  // reference (+4 = the object)
  constexpr uint32_t kPlayers = 0x82E34210;      // -> +0x14 -> +0x3C4: the object the MP branch targets (825D0CD4)
  std::atomic<bool> g_turret_logged{false};
  uint32_t g_turret_woken = 0;

  // pdz_rate_fix_log: one turret's clock and the angles it set off with (Bridge places one).
  struct TurretDiag
  {
    uint32_t turret = 0;
    double clock = 0.0;      // sum of its dt
    double shot_start = -1.0; // clock at state 1 -> 2
    float yaw = 0, pitch = 0, yaw_target = 0, pitch_target = 0;
    uint32_t shots = 0;
  };
  TurretDiag g_turret_diag[4];

  TurretDiag &DiagFor(uint32_t turret)
  {
    for (TurretDiag &d : g_turret_diag)
    {
      if (d.turret == turret)
      {
        return d;
      }
    }
    for (TurretDiag &d : g_turret_diag)
    {
      if (d.turret == 0)
      {
        d.turret = turret;
        REXLOG_INFO("rate_fix: turret {:08X} first update", turret);
        return d;
      }
    }
    g_turret_diag[0] = TurretDiag{};
    g_turret_diag[0].turret = turret;
    return g_turret_diag[0];
  }

  // Rare's turn at 30 fps from state 2 entry (0.5 s wait, yaw to within 1 deg, 0.2 s wait, both to within 0.1),
  // in seconds, to set against the measured time.
  double PredictTurn30(float yaw, float pitch, float yaw_target, float pitch_target)
  {
    const float dt = 1.0f / 30.0f;
    double t = 0.0;
    float wait = 0.5f;
    for (int i = 0; i < 2000; ++i)
    {
      t += dt;
      if (wait > 0.0f)
      {
        wait -= dt;
        continue;
      }
      yaw = (yaw_target - yaw) * kYawShare + yaw;
      if (std::fabs(yaw_target - yaw) < kYawDone)
      {
        break;
      }
    }
    wait = 0.2f;
    for (int i = 0; i < 2000; ++i)
    {
      t += dt;
      if (wait > 0.0f)
      {
        wait -= dt;
        continue;
      }
      yaw = (yaw_target - yaw) * kYawShare + yaw;
      pitch = (pitch_target - pitch) * kPitchShare + pitch;
      if (std::fabs(yaw_target - yaw) < kAimDone && std::fabs(pitch_target - pitch) < kAimDone)
      {
        break;
      }
    }
    return t;
  }

  void TurretLog(uint8_t *base, uint32_t turret, double dt, uint32_t s0, uint32_t s1, bool on)
  {
    TurretDiag &d = DiagFor(turret);
    d.clock += dt;
    if (s1 == s0)
    {
      return;
    }
    const float yaw = LoadF(base, turret + kTurretYaw), pitch = LoadF(base, turret + kTurretPitch);
    const float ty = LoadF(base, turret + kTurretYawTarget), tp = LoadF(base, turret + kTurretPitchTarget);
    REXLOG_INFO("rate_fix: turret {:08X} state {} -> {} at {:.3f} s (dt {:.5f}; yaw {:.2f} -> {:.2f}, pitch {:.2f} "
                "-> {:.2f}; fix {})",
                turret, s0, s1, d.clock, dt, yaw, ty, pitch, tp, on ? "on" : "off");
    if (s0 == 1 && s1 == 2)
    {
      d.shot_start = d.clock;
      d.yaw = yaw;
      d.pitch = pitch;
      d.yaw_target = ty;
      d.pitch_target = tp;
    }
    else if (s0 == 3 && d.shot_start >= 0.0)
    {
      ++d.shots;
      const double took = d.clock - d.shot_start;
      const double rare = PredictTurn30(d.yaw, d.pitch, d.yaw_target, d.pitch_target);
      REXLOG_INFO("rate_fix: turret {:08X} shot {}: turned {:.2f} deg / {:.2f} deg in {:.3f} s; Rare's 30 fps: "
                  "{:.3f} s (ratio {:.2f}; {:.1f} fps)",
                  turret, d.shots, std::fabs(d.yaw_target - d.yaw), std::fabs(d.pitch_target - d.pitch), took, rare,
                  rare > 0.0 ? took / rare : 0.0, dt > 0.0 ? 1.0 / dt : 0.0);
      d.shot_start = -1.0;
    }
  }
} // namespace

// DP_Turret update: r3 the turret, f1 dt.
REX_HOOK_RAW(sub_825D0B28)
{
  const uint32_t turret = ctx.r3.u32;
  const double dt = ctx.f1.f64;
  if (!IsGuestPointer(turret))
  {
    __imp__sub_825D0B28(ctx, base);
    return;
  }
  if (REXCVAR_GET(pdz_test_turret_wake) && g_turret_woken != turret && Load32(base, turret + kTurretState) == 0 &&
      base[turret + kTurretHasTarget] == 0)
  {
    const uint32_t players = Load32(base, kPlayers);
    const uint32_t entry = IsGuestPointer(players) ? Load32(base, players + 0x14) : 0;
    const uint32_t target = IsGuestPointer(entry) ? Load32(base, entry + 0x3C4) : 0;
    if (IsGuestPointer(target))
    {
      rex::CallFrame frame(ctx);
      frame.ctx.r1.u64 = (ctx.r1.u32 - 0x400) & ~0xFu;
      frame.ctx.r3.u64 = turret + kTurretTargetRef;
      frame.ctx.r4.u64 = target;
      sub_821433D8(frame.ctx, base); // as sub_825D1580 (its SetTarget) does
      base[turret + kTurretHasTarget] = 1;
      Store32(base, turret + kTurretState, 1);
      g_turret_woken = turret;
      REXLOG_INFO("pdz_test: turret woken: turret {:08X} targets {:08X} (pdz_test_turret_wake)", turret, target);
    }
  }
  const uint32_t s0 = Load32(base, turret + kTurretState);
  const float y0 = LoadF(base, turret + kTurretYaw);
  const float p0 = LoadF(base, turret + kTurretPitch);
  const float w0 = LoadF(base, turret + kTurretWait);
  __imp__sub_825D0B28(ctx, base);

  const bool on = Fix(REXCVAR_GET(pdz_fix_turret_rate));
  const double steps = dt * 30.0;
  bool fixed = false;
  if (on && dt > 0.0 && steps < 0.999)
  {
    const uint32_t s1 = Load32(base, turret + kTurretState);
    const float y1 = LoadF(base, turret + kTurretYaw);
    const float p1 = LoadF(base, turret + kTurretPitch);
    const float ky = RateShare(kYawShare, steps), kp = RateShare(kPitchShare, steps);
    const float ty = LoadF(base, turret + kTurretYawTarget), tp = LoadF(base, turret + kTurretPitchTarget);
    float y = y1, p = p1;
    if (s0 == 0 && (y1 != y0 || p1 != p0))
    {
      // Back to centre (825D150C / 825D1514).
      y = (0.0f - y0) * ky + y0;
      p = (0.0f - p0) * kp + p0;
      fixed = true;
    }
    else if (s0 == 2 && y1 != y0)
    {
      // Yaw only (825D1338); pitch is stored back unchanged.
      y = (ty - y0) * ky + y0;
      fixed = true;
      if (s1 != 2 && !(std::fabs(ty - y) < kYawDone))
      {
        Store32(base, turret + kTurretState, 2);
        StoreF(base, turret + kTurretWait, w0);
      }
    }
    else if (s0 == 3 && (y1 != y0 || p1 != p0))
    {
      // Both (825D1298 / 825D12A0).
      y = (ty - y0) * ky + y0;
      p = (tp - p0) * kp + p0;
      fixed = true;
      if (s1 != 3 && !(std::fabs(ty - y) < kAimDone && std::fabs(tp - p) < kAimDone))
      {
        Store32(base, turret + kTurretState, 3);
        StoreF(base, turret + kTurretWait, w0);
      }
    }
    if (fixed)
    {
      StoreF(base, turret + kTurretYaw, y);
      StoreF(base, turret + kTurretPitch, p);
      if (!g_turret_logged.exchange(true, std::memory_order_relaxed))
      {
        REXLOG_INFO("pdz_fix: turret rate: turret {:08X} turns at Rare's 30 fps rate (dt {:.5f} s: yaw share {} -> "
                    "{:.4f}, pitch {} -> {:.4f} per frame)",
                    turret, dt, kYawShare, ky, kPitchShare, kp);
      }
    }
  }
  if (REXCVAR_GET(pdz_rate_fix_log))
  {
    TurretLog(base, turret, dt, s0, Load32(base, turret + kTurretState), on);
  }
}

// rand01 (sub_82702860): the per-frame chances above, by return address.
REX_HOOK_RAW(sub_82702860)
{
  const uint32_t lr = uint32_t(ctx.lr);
  __imp__sub_82702860(ctx, base);
  if (lr == g_idle.roll_return)
  {
    RateRoll(g_idle, ctx, base, Fix(REXCVAR_GET(pdz_fix_idle_fidget_rate)));
  }
  else if (lr == g_flicker.roll_return)
  {
    RateRoll(g_flicker, ctx, base, Fix(REXCVAR_GET(pdz_fix_screen_flicker_rate)));
    if (REXCVAR_GET(pdz_rate_fix_log))
    {
      const uint32_t mode = Load32(base, kFbFxMode);
      if (mode != g_fbfx_mode_seen)
      {
        REXLOG_INFO("rate_fix: framebuffer fx mode (view 0) {} -> {}{}", int32_t(g_fbfx_mode_seen), mode,
                    mode == 10 || mode == 13 ? " (a flicker mode)" : "");
        g_fbfx_mode_seen = mode;
      }
    }
  }
}
