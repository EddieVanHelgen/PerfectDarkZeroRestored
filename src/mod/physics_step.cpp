// Physics at the paced frame rate (pdz_physics_frame_step, default on): Havok's solver step follows the game's paced
// frame step instead of Rare's fixed 1/60 s.
//
// Found by Perfect Dark Zero: Reinstated: at 120 fps their log reads "PDZ physics
// timing: frame 0.007790s, solver 0.016667s -> 0.007790s": the Havok world is stepped once per game frame with a
// fixed 1/60 s solver step, so above 60 fps physics advances 1/60 s every frame: at 120 fps, double speed (dropped
// weapons and armour fly twice as far). Their source for the fix isn't public; this is our own, from TU3's code.
//
// How the world is stepped (TU3): the main tick (sub_82140BC0) -> the world objects' update (sub_8251B718) -> the
// physics manager's update sub_824E8BC8(f1 = the game's frame dt, the measured one at 82D6BFD8 +120) ->
// sub_829C5F30(world, f1 = frame dt, f2 = solver step), the Havok world wrapper, which hands both floats untouched
// to the world's virtual step ([world +8] vtable +8). f2 comes from the only 1/60 float in the image (820EAEC0,
// 0x3C888889), or frame dt / 6 when a frame took more than 0.1 s (a hitch: six sub-steps' worth). Nothing sets a
// solver step at world creation; the step is passed on every call. The same constant also feeds sub_824E0A50
// (a second step entry: f2 = 1/60) and, outside Havok, the game's own fixed-step systems (see the report).
//
// The fix: in sub_829C5F30, when f2 is exactly Rare's 1/60 and the guest is paced (pdz::pace::EffectiveHz: 120 for
// a single player level under pdz_sp_120, 60 in co-op / multiplayer / pdz_sp_60_levels levels, the Deck's present
// pacing rate, 0 when nothing paces the guest), f2 = 1 / that rate, capped at Rare's 1/60 below 60 fps (9 Nov: our
// 30 fps runs stepped 1/30 where Rare and PDZ Reinstated step 1/60). At 60 that is 1/60: identical to today. The
// frame-over-0.1-s branch (f2 = frame / 6) is left alone. Without pacing (EffectiveHz 0: the game runs at the
// display's 60) nothing changes either. A per-frame measured dt is never used: the paced step is exact.
// One line per level and per rate change: "physics: solver step X s (paced Y Hz; Rare's 1/60; frame dt Z s)".
#include <cmath>
#include <cstdint>
#include <cstring>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>
#include "hook_profile.h" // pdz_hook_profile (must follow <rex/hook.h>)

#include "render/game_pace.h" // EffectiveHz

REXCVAR_DEFINE_BOOL(pdz_physics_frame_step, true, "Gameplay",
                    "Havok's solver step follows the paced frame rate (1/120 s at 120 fps, 1/60 at 60, the Deck's "
                    "present pacing) instead of Rare's fixed 1/60 s, so physics runs at real speed above 60 fps "
                    "(found by Perfect Dark Zero: Reinstated: dropped weapons and armour flew twice as far at 120). "
                    "At 60 fps nothing changes. Off = Rare's 1/60 at any rate")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_fix_level_start_physics, false, "Gameplay",
                    "Above 30 fps, the first seconds of a level (pdz_level_start_physics_s) step Havok in Rare's 1/30 s "
                    "steps, as the Xbox did, so props the level start leaves overlapping (Laboratory: the scientist "
                    "sitting down at the keycard desk) are pushed apart as gently as there. Default off until tested")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(pdz_level_start_physics_s, 5.0, "Gameplay",
                      "How long after a level starts pdz_fix_level_start_physics steps Havok at 1/30 s")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REX_EXTERN(__imp__sub_829C5F30); // the Havok world wrapper's step: (world, f1 frame dt, f2 solver step)

namespace
{
  constexpr uint32_t kGame = 0x82D6BFD8;      // -> +0 game state (+8 = 21 in a level)
  constexpr uint32_t kRareStepBits = 0x3C888889u; // 1/60 as a float (820EAEC0)
  constexpr uint32_t kMinHz = 20, kMaxHz = 480;

  uint32_t Load32(const uint8_t *base, uint32_t a)
  {
    const uint8_t *p = base + a;
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
  }

  // The last line's key: the level (its state object), the rate and whether the step was replaced.
  uint32_t g_logged_state = 0, g_logged_hz = ~0u;
  bool g_logged_on = false;
  uint64_t g_calls = 0, g_replaced = 0;
  uint32_t g_settle_state = 0;
  double g_settle_time = 0.0;
  bool g_settle_logged = false;
} // namespace

REX_HOOK_RAW(sub_829C5F30)
{
  ++g_calls;
  const bool on = REXCVAR_GET(pdz_physics_frame_step);
  const float solver = float(ctx.f2.f64);
  uint32_t bits = 0;
  std::memcpy(&bits, &solver, 4);
  const uint32_t hz = pdz::pace::EffectiveHz();
  const bool paced = hz >= kMinHz && hz <= kMaxHz;
  const bool replace = on && paced && bits == kRareStepBits;
  float step = solver;
  if (replace)
  {
    // Never longer than Rare's 1/60 (as PDZ Reinstated's rule, MIT licence): below 60 fps (a 30 Hz display)
    // Havok keeps Rare's exact step instead of 1/30. 60 and 120 are unchanged (1/60, 1/120).
    step = 1.0f / float(hz > 60u ? hz : 60u);
    ctx.f2.f64 = double(step);
    ++g_replaced;
  }
  // One line per level and per change of the rate or the switch (the first call of a level, or a pace change).
  const uint32_t so = Load32(base, kGame);
  const uint32_t state = so >= 0x10000u && Load32(base, so + 8) == 21 ? so : 0;
  // pdz_fix_level_start_physics: Rare's 30 fps steps for the first seconds of a level (Havok's world takes whole
  // solver steps of f2 until it has caught up with the frame time, so 1/30 s steps keep the right speed).
  if (state != g_settle_state)
  {
    g_settle_state = state;
    g_settle_time = 0.0;
    g_settle_logged = false;
  }
  if (state && ctx.f1.f64 > 0.0 && ctx.f1.f64 < 0.1)
  {
    g_settle_time += ctx.f1.f64;
  }
  if (state && REXCVAR_GET(pdz_fix_level_start_physics) && paced && hz > 31 && bits == kRareStepBits &&
      g_settle_time < REXCVAR_GET(pdz_level_start_physics_s))
  {
    step = 1.0f / 30.0f;
    ctx.f2.f64 = double(step);
    if (!g_settle_logged)
    {
      g_settle_logged = true;
      REXLOG_INFO("physics: level start: Havok steps of 1/30 s (Rare's 30 fps) for the first {:.1f} s "
                  "(pdz_fix_level_start_physics)",
                  REXCVAR_GET(pdz_level_start_physics_s));
    }
  }
  if (bits == kRareStepBits && (state != g_logged_state || hz != g_logged_hz || on != g_logged_on))
  {
    g_logged_state = state;
    g_logged_hz = hz;
    g_logged_on = on;
    REXLOG_INFO("physics: solver step {:.6f} s (paced {} Hz; Rare's {:.6f}; frame dt {:.5f} s; {}; step calls {}, "
                "replaced {})",
                step, hz, solver, ctx.f1.f64, !on ? "pdz_physics_frame_step off: Rare's step"
                                                : !paced ? "no pacing: Rare's step"
                                                         : "the paced step (found by PDZ Reinstated)",
                g_calls, g_replaced);
  }
  __imp__sub_829C5F30(ctx, base);
}
