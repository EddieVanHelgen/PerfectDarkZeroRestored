// Auto-aim tweaks for the Title Update 3 build (tu3-base). Only what the
// launch build keeps on is ported: the pdz_autoaim_tuning master switch with
// pdz_aim_still_lock, pdz_aim_respect_headshots and pdz_aim_assist_zoomed
// (docs/TU3_BASE.md, re-targeting checklist), plus pdz_autoaim_zoom_falloff
// and the pdz_autoaim_log lines. The launch branch's cone, strength, enable,
// ignore-difficulty and friction options aren't here.
// Addresses are TU3's; launch addresses in brackets.
//
// The player's auto-aim (player +0x1F60, updated by sub_824EF078
// [824E2028]) never turns the camera. Each frame its target search picks a
// visible character near the reticle and, if the target's aim point is within
// the lock radius, moves the aim (+0x38/+0x3C) from where the player aims
// (+0x30/+0x34) onto the target. The caller (sub_825B2D08 [825A34F0]) hands
// it only to the weapons' aim: the shot bends to the target, the drawn
// crosshair stays put. The object's layout is the same in TU3.
//
// Units: the bent point is the target's aim point projected on the screen
// with the zoomed field of view (BUG_AUDIT WP16), so while zoomed it is in
// zoomed screen units; x (1 - zoom) puts it in the unzoomed units the lock
// test and the weapon aim use (1 = the edge of the unzoomed view, so a fixed
// number of units is a fixed angle whatever the zoom). The zoom is player
// +0x1A9C: the fraction the view shrinks by (the view's tangent is
// x (1 - zoom), magnification 1 / (1 - zoom)); 0 = not zoomed, 0.8 = the
// P99's scope (5x).
#include "aim_assist.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

#include <fmt/format.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "../game_cvars.h"
#include "fov_tuning.h"
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

  // Player and auto-aim object fields.
  constexpr uint32_t kPlayerAutoAim = 0x1F60;
  constexpr uint32_t kPlayerZoom = 0x1A9C; // 0 = not zoomed, 0.8 = P99 scope
  constexpr uint32_t kTarget = 0x14;
  // Where the reticle is on the target's screen box: 0 at its centre, 1 at
  // its edge (the box the search projects, 10 px bigger all round).
  constexpr uint32_t kTargetOverlap = 0x20;
  constexpr uint32_t kTargetPoint = 0x24;    // x, y, z: the target's aim point (cm)
  constexpr uint32_t kAimReticle = 0x30;     // x, y: where the player aims
  constexpr uint32_t kReticle = 0x38;        // x, y: after auto-aim
  constexpr uint32_t kEye = 0x40;            // x, y, z (cm)
  constexpr uint32_t kCrosshairPoint = 0x4C; // x, y, z: the crosshair's world point (25000 cm out)

  // With pdz_autoaim_zoom_falloff on, pdz_aim_assist_zoomed keeps the sticky
  // turn only up to this zoom (2x); a stronger zoom gets the game's own rule
  // (off while zoomed).
  constexpr float kStickyZoomLimit = 0.5f;

  // The player whose auto-aim sub_825B2D08 is updating (it passes
  // player + kPlayerAutoAim), and the update in progress.
  thread_local uint32_t t_player = 0;
  thread_local uint32_t t_autoaim = 0;

  bool Tuning()
  {
    return REXCVAR_GET(pdz_autoaim_tuning);
  }

  bool ZoomFalloff()
  {
    return Tuning() && REXCVAR_GET(pdz_autoaim_zoom_falloff);
  }

  // pdz_aim_still_lock: the game tests the lock with radius
  // (r14 + r1C * t) * t and refuses when t <= 0, t = how fast the view turns
  // (0..1), so shots aren't bent while you hold still on a target. Dividing
  // both record fields by t for that test turns it into r14 + r1C * t, the
  // form target selection uses; at t = 0 a tiny t gets past the refusal (the
  // only other use left, the alternate radius r08 * t, stays as good as 0).
  // sub_824F0BB8 [824E36D8] is called right before the lock test and returns
  // to 824EF448 [824E23F8] with the record in r10 and t in f31.
  // The widening only decides whether the game locks; the zoom falloff judges
  // the bend that comes out of the update, so a wider lock can't get past it.
  constexpr uint32_t kLockTestReturn = 0x824EF448;
  constexpr uint32_t kLockBase = 0x14;
  constexpr uint32_t kLockTurnExtra = 0x1C;
  constexpr double kLockMinTurn = 1e-6;
  thread_local uint32_t t_lock_record = 0;
  thread_local uint32_t t_lock_saved[2] = {};
  thread_local double t_lock_widen = 0.0; // 1 / t of this update's widening, 0 = none

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
    t_lock_widen = 1.0 / t;
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

  // What happened to the shot bend in an update (pdz_autoaim_log).
  enum class Result : uint8_t
  {
    kNotZoomed,  // not zoomed: Rare's bend (or none), untouched
    kNoBend,     // zoomed, auto-aim did not bend
    kBent,       // zoomed, the bend was within the allowed radius
    kRefused,    // zoomed, the bend was too far: the shot goes where the reticle points
    kFalloffOff, // zoomed and bent, pdz_autoaim_zoom_falloff off
    kMouse,      // pdz_mouse_aim_assist_off
  };

  const char *ResultText(Result result)
  {
    switch (result)
    {
    case Result::kNotZoomed:
      return "not zoomed";
    case Result::kNoBend:
      return "no bend";
    case Result::kBent:
      return "bent";
    case Result::kRefused:
      return "refused";
    case Result::kFalloffOff:
      return "bent (falloff off)";
    case Result::kMouse:
      return "mouse: no assist";
    }
    return "?";
  }

  struct Decision
  {
    uint32_t player = 0;
    uint32_t target = 0;
    float zoom = 0.0f;
    float bend = 0.0f;     // unzoomed screen units: how far auto-aim moves the shot
    float allowed = -1.0f; // unzoomed screen units, < 0 = no limit
    double widen = 0.0;    // still lock: 1 / t, 0 = not widened
    Result result = Result::kNotZoomed;
    float eye[3] = {};          // cm
    float crosshair[3] = {};    // cm
    float target_point[3] = {}; // cm
    uint32_t updates_since_line = 0; // rate limit for the "decision" lines
    bool logged = false;
  };

  // Local players (split screen has up to 4). Game thread only.
  constexpr int kMaxPlayers = 4;
  Decision g_decisions[kMaxPlayers];
  constexpr uint32_t kDecisionLineGap = 6; // updates between "decision" lines (10 a second at 60 fps)

  Decision *DecisionFor(uint32_t player, bool create)
  {
    for (Decision &d : g_decisions)
    {
      if (d.player == player)
      {
        return &d;
      }
    }
    if (!create)
    {
      return nullptr;
    }
    for (Decision &d : g_decisions)
    {
      if (!d.player)
      {
        d.player = player;
        return &d;
      }
    }
    g_decisions[0] = {};
    g_decisions[0].player = player;
    return &g_decisions[0];
  }

  void LoadVec(const uint8_t *base, uint32_t address, float out[3])
  {
    for (uint32_t i = 0; i < 3; ++i)
    {
      out[i] = LoadBeFloat(base, address + i * 4);
    }
  }

  void LogDecision(const char *kind, const Decision &d)
  {
    const std::string widen = d.widen > 0.0 ? fmt::format("widened x{:.0f}", d.widen) : std::string("no");
    if (!d.target)
    {
      REXLOG_INFO("pdz_autoaim: {} player {:08X} | zoom {:.2f} | target none | still lock {} | {}", kind, d.player,
                  d.zoom, widen, ResultText(d.result));
      return;
    }
    const double a[3] = {d.crosshair[0] - d.eye[0], d.crosshair[1] - d.eye[1], d.crosshair[2] - d.eye[2]};
    const double b[3] = {d.target_point[0] - d.eye[0], d.target_point[1] - d.eye[1], d.target_point[2] - d.eye[2]};
    const double la = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
    const double lb = std::sqrt(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]);
    double angle = 0.0;
    if (la > 0.0 && lb > 0.0)
    {
      const double c = (a[0] * b[0] + a[1] * b[1] + a[2] * b[2]) / (la * lb);
      angle = std::acos(c > 1.0 ? 1.0 : (c < -1.0 ? -1.0 : c));
    }
    const std::string allowed = d.allowed < 0.0f ? std::string("-") : fmt::format("{:.4f}", d.allowed);
    REXLOG_INFO("pdz_autoaim: {} player {:08X} | zoom {:.2f} | target {:08X} at {:.2f} m | bend {:.4f} allowed {} | "
                "off-axis {:.2f} deg ({:.0f} cm at the target) | still lock {} | {}",
                kind, d.player, d.zoom, d.target, lb / 100.0, d.bend, allowed, angle * 57.29577951308232,
                std::sin(angle) * lb, widen, ResultText(d.result));
  }

  // pdz_autoaim_log: keeps this update's decision for the shot lines and logs
  // a "decision" line when the target or the result changes (at most one per
  // kDecisionLineGap updates). Nothing when the log is off.
  void Record(const uint8_t *base, uint32_t player, uint32_t autoaim, uint32_t target, float zoom, float bend,
              float allowed, Result result)
  {
    if (!REXCVAR_GET(pdz_autoaim_log))
    {
      return;
    }
    Decision &d = *DecisionFor(player, true);
    const bool changed = !d.logged || d.target != target || d.result != result;
    d.target = target;
    d.zoom = zoom;
    d.bend = bend;
    d.allowed = allowed;
    d.widen = t_lock_widen;
    d.result = result;
    if (target)
    {
      LoadVec(base, autoaim + kEye, d.eye);
      LoadVec(base, autoaim + kCrosshairPoint, d.crosshair);
      LoadVec(base, autoaim + kTargetPoint, d.target_point);
    }
    ++d.updates_since_line;
    if (changed && d.updates_since_line >= kDecisionLineGap)
    {
      d.updates_since_line = 0;
      d.logged = true;
      LogDecision("decision", d);
    }
  }
} // namespace

namespace aim_assist
{
  void OnShot(const uint8_t *base, uint32_t shooter)
  {
    if (!REXCVAR_GET(pdz_autoaim_log))
    {
      return;
    }
    // The shooter is the player (below the 1 GB mark) or a weapon whose
    // +0x5C is the player.
    const Decision *d = DecisionFor(shooter, false);
    if (!d && shooter >= 0x10000u && shooter < 0xFFFF0000u)
    {
      d = DecisionFor(LoadBe32(base, shooter + 0x5C), false);
    }
    if (d && d->player)
    {
      LogDecision("shot", *d);
    }
  }

  void OnShotTU3(const uint8_t *base, uint32_t shooter, uint32_t origin, uint32_t aim_point)
  {
    if (!REXCVAR_GET(pdz_autoaim_log))
    {
      return;
    }
    const Decision *d = DecisionFor(shooter, false);
    if (!d && shooter >= 0x10000u && shooter < 0xFFFF0000u)
    {
      d = DecisionFor(LoadBe32(base, shooter + 0x5C), false);
    }
    if (!d || !d->player)
    {
      return;
    }
    LogDecision("shot", *d);
    if (origin >= 0x10000u && origin < 0xFFFF0000u && aim_point >= 0x10000u && aim_point < 0xFFFF0000u)
    {
      float o[3], a[3];
      LoadVec(base, origin, o);
      LoadVec(base, aim_point, a);
      REXLOG_INFO("pdz_autoaim: shot path player {:08X} from ({:.1f}, {:.1f}, {:.1f}) to ({:.1f}, {:.1f}, {:.1f})",
                  d->player, o[0], o[1], o[2], a[0], a[1], a[2]);
    }
  }
} // namespace aim_assist

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
// pdz_aim_assist_zoomed answers no there, so it also runs while zoomed (with
// pdz_autoaim_zoom_falloff only up to kStickyZoomLimit). Every other caller
// gets the real answer.
REX_HOOK_RAW(sub_825AC080)
{
  constexpr uint32_t kAssistZoomCheckReturn = 0x825AF6CC;
  const uint32_t player = ctx.r3.u32;
  bool assist_check = uint32_t(ctx.lr) == kAssistZoomCheckReturn && Tuning() && REXCVAR_GET(pdz_aim_assist_zoomed);
  if (assist_check && ZoomFalloff() && player && LoadBeFloat(base, player + kPlayerZoom) > kStickyZoomLimit)
  {
    assist_check = false;
  }
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
  t_lock_widen = 0.0;
  __imp__sub_824EF078(ctx, base);
  t_autoaim = 0;
  RestoreStillLock(base);

  const uint32_t target = LoadBe32(base, autoaim + kTarget);
  const float zoom = LoadBeFloat(base, owner + kPlayerZoom);

  // pdz_mouse_aim_assist_off (mouse_look.cpp): with the mouse the shot goes where the player aims, unbent.
  if (REXCVAR_GET(pdz_mouse_aim_assist_off) && pdz::mouse::MouseIsLastDevice())
  {
    StoreBeFloat(base, autoaim + kReticle, LoadBeFloat(base, autoaim + kAimReticle));
    StoreBeFloat(base, autoaim + kReticle + 4, LoadBeFloat(base, autoaim + kAimReticle + 4));
    Record(base, owner, autoaim, target, zoom, 0.0f, -1.0f, Result::kMouse);
    return;
  }

  // pdz_aim_respect_headshots: with the reticle over the target's screen box
  // (overlap <= 1), keep the player's own height and bend only sideways, so
  // a head aim stays on the head instead of moving to the model's aim point.
  const float aim_x = LoadBeFloat(base, autoaim + kAimReticle);
  const float aim_y = LoadBeFloat(base, autoaim + kAimReticle + 4);
  const float reticle_x = LoadBeFloat(base, autoaim + kReticle);
  float reticle_y = LoadBeFloat(base, autoaim + kReticle + 4);
  const bool locked = target && (reticle_x != aim_x || reticle_y != aim_y);
  if (Tuning() && locked && REXCVAR_GET(pdz_aim_respect_headshots) &&
      LoadBeFloat(base, autoaim + kTargetOverlap) <= 1.0f)
  {
    StoreBeFloat(base, autoaim + kReticle + 4, aim_y);
    reticle_y = aim_y;
  }

  // pdz_autoaim_zoom_falloff: while zoomed a bend is allowed only if it moves
  // the shot no further than pdz_autoaim_zoom_radius x (1 - zoom)^2 unzoomed
  // screen units (the bend left after the headshot rule above); otherwise the
  // shot goes exactly where the reticle points. Not zoomed: nothing changes.
  // An allowed bend is still corrected by pdz_fix_autoaim_zoom_bend.
  if (!(zoom > 0.0f))
  {
    Record(base, owner, autoaim, target, zoom, 0.0f, -1.0f, Result::kNotZoomed);
    return;
  }
  const float keep = zoom < 1.0f ? 1.0f - zoom : 0.0f;
  const float dx = reticle_x - aim_x;
  const float dy = reticle_y - aim_y;
  const float bend = std::sqrt(dx * dx + dy * dy) * keep * float(pdz::fov::ScaleFor(owner));
  if (!locked || (dx == 0.0f && dy == 0.0f))
  {
    Record(base, owner, autoaim, target, zoom, bend, -1.0f, Result::kNoBend);
    return;
  }
  if (!ZoomFalloff())
  {
    Record(base, owner, autoaim, target, zoom, bend, -1.0f, Result::kFalloffOff);
    return;
  }
  const float allowed = float(REXCVAR_GET(pdz_autoaim_zoom_radius)) * keep * keep;
  if (bend <= allowed)
  {
    Record(base, owner, autoaim, target, zoom, bend, allowed, Result::kBent);
    return;
  }
  StoreBeFloat(base, autoaim + kReticle, aim_x);
  StoreBeFloat(base, autoaim + kReticle + 4, aim_y);
  Record(base, owner, autoaim, target, zoom, bend, allowed, Result::kRefused);
}
