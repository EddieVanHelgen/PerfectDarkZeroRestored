// Distant window lights (Rare's bug, seen on a real Xbox 360 too; pdz_fix_distant_lights, on by default).
//
// What players see: in Subway's opening streets the lit windows of the houses in front of the blue towers go dark
// when looked at head-on and light up at the edge of the view; Nightclub's distant housing lights change with distance
// and with turning.
//
// What the lights are: the lit windows are Rare's deferred point lights (warm 1.46/1.5/0.87 and cyan 0.51/1.5/1.47,
// radius 300-700), drawn per light by sub_826048F0 (stencil sphere 826051E0 / 82605324, additive light 82605450),
// and the lamp halos (sub_82603AC0 -> 82608510) come from the same list.
//
// Rare's rule: each frame the list of lights to draw is built by sub_82606268 (called from sub_82602C88 with a cap of
// 200 lights): for every light in the level it asks sub_826064D0 for a score; a light with a score above 0 is drawn,
// the list is sorted by score (highest first; the top ones get the specular term, then it fades out by rank) and cut
// at the cap. For a point or spot light the score is
//     score = (z + radius) / range + 1        (clamped)
// where z is the light's position transformed by the camera (sub_827D15A8 into light +1232 + 12 * (2 * pass + buffer),
// so z = minus the depth along the view direction) and range = the caller's f2. So a light is drawn when its depth
// along the view direction, minus its radius, is under the range. Depth along the view direction is the distance
// times the cosine of the angle off the view centre: a light straight ahead counts at its full distance, the same light
// at the edge of the view counts nearer (at a 37 degree half angle, 0.8 of its distance). A light just past the range
// is therefore dropped when looked at and drawn when it is at the edge of the view, which is exactly the switching.
//
// The fix: the same score with the straight distance |v| from the camera instead of the depth (the range, the radius,
// the clamp, the cap and the sort are Rare's). A light's state then depends only on how far away it is, not on where
// it is in the view. pdz_distant_lights_reach scales the distance: 1 = Rare's range measured straight; below 1 the
// lights reach further (0.8 keeps every light that Rare drew at the edge of a 37 degree half angle view lit head-on).
//
// Diagnostics: pdz_distant_lights_log = N logs the next N list builds (one per pass and frame): every light the score
// function sees, with its world position, view vector, radius, gates, Rare's score and the fixed score.
#include <cmath>
#include <cstdint>
#include <cstring>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>
#include "hook_profile.h" // pdz_hook_profile (must follow <rex/hook.h>)

#include "../game_cvars.h" // pdz_game_fixes

REXCVAR_DEFINE_BOOL(pdz_fix_distant_lights, false, "Gameplay",
                    "Game fix (Rare's bug, on a real Xbox 360 too): lights (lit windows, lamps) are kept or dropped by "
                    "their straight distance from the camera instead of their depth along the view direction, so "
                    "distant window lights no longer switch off when looked at head-on and on at the edge of the view "
                    "(Subway's houses, Nightclub's housing). Off by default in 0.9.4 (the overnight runs showed no change at the "
                    "scripted spots: unproven). Needs pdz_game_fixes")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(pdz_distant_lights_reach, 1.0, "Gameplay",
                      "pdz_fix_distant_lights: the light's straight distance is multiplied by this before Rare's range "
                      "test. 1 = Rare's range; 0.8 = lights reach as far head-on as Rare's reached at the view's edge "
                      "(more lights drawn)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(pdz_distant_lights_log, 0, "Debug",
                     "Logs the next N light list builds (sub_82606268): every light's position, view vector, radius, "
                     "gates and score, Rare's and fixed. 0 = off")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REX_EXTERN(__imp__sub_826064D0); // a light's score: (r3 light, r10 pass, f2 range) -> f1
REX_EXTERN(__imp__sub_82606268); // the light list build: (r3 cap, r10 pass, f1..f4)

namespace
{
  constexpr uint32_t kBuffer = 0x82E1BA20;     // render buffer index (0 / 1)
  constexpr uint32_t kListCount = 0x82E0A0BC;  // + 4 * (2 * pass + buffer): lights in the list
  constexpr uint32_t kLevelLights = 0x82E3C5F4; // lights in the level
  // Rare's float constants (the clamp of the score).
  constexpr uint32_t kOne = 0x820DFD34, kZero = 0x820DFCA4;
  constexpr uint32_t kMax = 0x820EB1A0, kOverMax = 0x820EB198, kMin = 0x820EAC94, kUnderMin = 0x820EB190;
  // The light (offsets).
  constexpr uint32_t kType = 680, kRadius = 684, kWorldPos = 720, kViewPos = 1232, kRgb = 828, kActive = 1448;

  uint32_t Load32(const uint8_t *base, uint32_t a)
  {
    return _byteswap_ulong(*reinterpret_cast<const uint32_t *>(base + a));
  }
  float LoadF(const uint8_t *base, uint32_t a)
  {
    const uint32_t u = Load32(base, a);
    float f;
    std::memcpy(&f, &u, 4);
    return f;
  }
  double LoadD(const uint8_t *base, uint32_t a)
  {
    const uint64_t u = _byteswap_uint64(*reinterpret_cast<const uint64_t *>(base + a));
    double d;
    std::memcpy(&d, &u, 8);
    return d;
  }

  // Rare's clamp at the end of sub_826064D0 (two fsel: over the max -> kOverMax, under the min -> kUnderMin).
  double Clamp(const uint8_t *base, double s)
  {
    const double max = LoadF(base, kMax), min = LoadF(base, kMin);
    const double f0 = (max - s) >= 0.0 ? s : LoadD(base, kOverMax);
    return (s - min) >= 0.0 ? f0 : LoadD(base, kUnderMin);
  }

  int g_log_builds = 0;        // list builds still to log (set from the cvar)
  bool g_logging = false;      // inside a logged build
  uint32_t g_build_seq = 0;
  int g_light_seq = 0;
  int g_changed = 0;           // lights whose drawn / dropped state the fix changed in this build
} // namespace

REX_HOOK_RAW(sub_82606268)
{
  const int want = REXCVAR_GET(pdz_distant_lights_log);
  if (want > 0)
  {
    g_log_builds = want;
    REXCVAR_SET(pdz_distant_lights_log, 0);
  }
  const uint32_t pass = ctx.r10.u32 & 0xFF, cap = ctx.r3.u32;
  g_logging = g_log_builds > 0;
  if (g_logging)
  {
    --g_log_builds;
    ++g_build_seq;
    g_light_seq = 0;
    g_changed = 0;
    REXLOG_INFO("distant_lights: build {} pass {} cap {} range f2 {:.1f} (f1 {:.3f} f3 {:.3f} f4 {:.3f}) level lights "
                "{} fix {} reach {:.3f}",
                g_build_seq, pass, cap, ctx.f2.f64, ctx.f1.f64, ctx.f3.f64, ctx.f4.f64,
                Load32(base, kLevelLights), REXCVAR_GET(pdz_game_fixes) && REXCVAR_GET(pdz_fix_distant_lights),
                REXCVAR_GET(pdz_distant_lights_reach));
  }
  __imp__sub_82606268(ctx, base);
  if (g_logging)
  {
    const uint32_t b = Load32(base, kBuffer) & 1u;
    REXLOG_INFO("distant_lights: build {} done: {} lights in the list, {} changed by the fix", g_build_seq,
                Load32(base, kListCount + 4 * (2 * pass + b)), g_changed);
    g_logging = false;
  }
}

REX_HOOK_RAW(sub_826064D0)
{
  const uint32_t light = ctx.r3.u32, pass = ctx.r10.u32 & 0xFF;
  const double range = ctx.f2.f64;
  __imp__sub_826064D0(ctx, base);
  const bool on = REXCVAR_GET(pdz_game_fixes) && REXCVAR_GET(pdz_fix_distant_lights);
  if (!on && !g_logging)
  {
    return;
  }
  // Rare's gates (as sub_826064D0): enabled in this buffer, active, not a directional light, visible in this pass,
  // and not black. Where any fails Rare's score stands.
  const uint32_t b = Load32(base, kBuffer);
  if (b > 1)
  {
    return;
  }
  const uint32_t type = Load32(base, light + kType);
  bool point = Load32(base, light + (b + 356) * 4) != 0 && int32_t(Load32(base, light + kActive)) >= 1 && type != 1;
  const uint32_t mask = point ? Load32(base, light + b * 340) : 0;
  const bool in_pass = pass < 32 && (mask >> pass) & 1u;
  const uint32_t rgb = light + b * 16 + kRgb;
  const bool lit = LoadF(base, rgb) > 0.0f || LoadF(base, rgb + 4) > 0.0f || LoadF(base, rgb + 8) > 0.0f;
  const double rare = ctx.f1.f64;
  double fixed = rare;
  const uint32_t v = light + kViewPos + 12 * (2 * pass + b);
  const float vx = point ? LoadF(base, v) : 0.0f, vy = point ? LoadF(base, v + 4) : 0.0f,
              vz = point ? LoadF(base, v + 8) : 0.0f;
  const double dist = std::sqrt(double(vx) * vx + double(vy) * vy + double(vz) * vz);
  const double radius = point ? LoadF(base, light + kRadius) : 0.0;
  if (point && in_pass && lit && range > 0.0 && std::isfinite(dist))
  {
    double reach = REXCVAR_GET(pdz_distant_lights_reach);
    if (!(reach > 0.05 && reach <= 4.0))
    {
      reach = 1.0;
    }
    fixed = Clamp(base, (-dist * reach + radius) / range + double(LoadF(base, kOne)));
    if (on)
    {
      ctx.f1.f64 = fixed;
    }
  }
  if (!g_logging)
  {
    return;
  }
  const double zero = LoadF(base, kZero);
  const bool changed = (rare > zero) != (fixed > zero);
  g_changed += changed;
  if (point && in_pass && lit)
  {
    REXLOG_INFO("distant_lights:   #{} light {:08X} type {} world ({:.0f} {:.0f} {:.0f}) r {:.0f} view ({:.0f} {:.0f} "
                "{:.0f}) dist {:.0f} depth {:.0f} off-axis {:.1f} deg | score Rare {:.4f} fixed {:.4f}{}",
                g_light_seq, light, type, LoadF(base, light + kWorldPos), LoadF(base, light + kWorldPos + 4),
                LoadF(base, light + kWorldPos + 8), radius, vx, vy, vz, dist, -vz,
                dist > 0.0 ? std::acos(std::fmin(1.0, std::fabs(vz) / dist)) * 57.29578 : 0.0, rare, fixed,
                changed ? (fixed > zero ? "  [fix draws it]" : "  [fix drops it]") : "");
  }
  else if (point)
  {
    REXLOG_INFO("distant_lights:   #{} light {:08X} type {} gated: pass bit {} lit {} | score {:.4f}", g_light_seq, light,
                type, in_pass, lit, rare);
  }
  ++g_light_seq;
}
