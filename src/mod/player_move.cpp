// Player movement log (pdz_step_log, default off, diagnostics): one line per player-1 move frame, for the Subway ramp
// (and any step-up / slope report). Function hooks only (no mid-function hooks, so no codegen change).
//
// How a walking character moves (TU3; traced on branch a5-step-up, 7-8 Nov 2026):
// - the mover update sub_82697088(character, mover, f1 = dt) -> (on the ground) sub_82697B88 builds the target
//   position (mover +360) = the previous one + the stick / animation move + a gravity drop, then
// - sub_8269C580(character, type, &previous position) resolves it: the ground probe sub_8269CEF0(&position, character,
//   type, &previous) (a ray from max(previous y, target y) + the step allowance down to the target y; a hit puts the
//   feet on the ground), sub_82215ED8 sets the position, then the wall sweep sub_8269C8F0(character, type, &previous)
//   (a sphere above the feet, its height from sub_8269C688: 120 cm standing, less when crouched) slides the move
//   along what it hits; r3 bit 0 = ground found, bit 1 = wall hit.
//
// The line: dt; the sphere height (crouch shows as < 120); the intended move (target - previous at the resolve);
// the probe (target y in, ground y out, hit, ground normal y); the sweep (the move it removed, its wall bit); the
// achieved move; air / fall / vertical speed; the character position's y (the view's base: +48) beside the feet's;
// which stage took the move away ("stopped by"); and the two parts of pdz_fix_ramp_step (carried cm, start -Nf).
//
// pdz_fix_ramp_step (default off): the Subway end-train ramp (8-9 Nov 2026, the collision study).
// Jo's feet stay on the track bed there; only the sweep's sphere, sliding up a sloped, non-floor surface at the
// slab's foot, lifts her over a low edge, and every frame the probe puts the feet back on the bed first. So the sweep
// of each frame starts from the lifted previous position and moves (the stick's move, minus the lift) - and whether
// she gets over depends on how big that move is per sweep: Rare's 1/30 s at 30 fps, half or a quarter at 60 / 120.
// The fix gives the sweep Rare's 30 fps move in both of its parts: (a) the blocked part of a move is carried into the
// next frames' target (up to the 1/30 s move), and (b) after a frame whose sweep touched something, the sweep starts
// from where she was 1/30 s ago (sub_8269C580's own copy of the previous position is rewritten for the sweep only).
// Both parts only redistribute the stick's own distance: nothing is added to the move's size beyond one 1/30 s step.
//
// rc2 (9 Nov evening): in 0.9.4-rc1 both parts acted after ANY touch, walls included, and Jo bounced back a little when
// she ran into a wall above 30 fps (the sweep then started from where she was 1/30 s ago). Now both parts act only on
// a LOW EDGE: a touch whose push (the sweep's correction, target -> result, which for a slide is along the surface's
// normal) points up by at least pdz_fix_ramp_step_edge (its vertical share; 0 = a vertical wall). A sphere rising onto
// the ramp's foot or the top of a low edge is pushed up and back; a wall pushes only sideways. Against a wall the move
// is exactly Rare's: nothing carried, no earlier start. pdz_fix_ramp_step_walls = the rc1 rule, for A/B.
// The state is per character (rc1 kept one copy for all players: one player's carried move and position history could
// reach another's in split screen / co-op).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>
#include "hook_profile.h" // pdz_hook_profile (must follow <rex/hook.h>)

#include "render/game_pace.h" // EffectiveHz

REXCVAR_DEFINE_BOOL(pdz_step_log, false, "Diagnostics",
                    "Log player 1's movement every frame (intended move, ground probe, wall sweep, achieved move, "
                    "ground normal, crouch height): for step-up and slope reports")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_fix_ramp_step, true, "Gameplay",
                    "Above 30 fps, the player's wall check sees Rare's 30 fps move at low edges: (a) when a low edge "
                    "stops the move, the blocked part is kept and added to the next frames' move, up to the distance "
                    "one 1/30 s frame would move at that speed; (b) after a frame in which she touched a low edge, the "
                    "next check starts from where Jo was 1/30 s ago (pdz_fix_ramp_step_start). Walls get Rare's code "
                    "exactly (pdz_fix_ramp_step_edge). Low edges Jo crosses at 30 fps (the ramp into "
                    "Subway's end train) are crossed the same at 60 / 120; what blocks her at 30 (a crouch-walk there, "
                    "walls) still does. On by default since 9 Nov 2026 (after the round-3 runs; the one "
                    "accepted difference is that a walk into the slab's side at 60 / 120 can climb onto it where 30 fps "
                    "holds; it is the same slab at the very end of the level)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_INT32(pdz_fix_ramp_step_start, 1, "Gameplay",
                     "With pdz_fix_ramp_step, the wall check's start point after a frame in which it touched a low edge: "
                     "0 = the previous frame's position (Rare's code at this frame rate; only the blocked-move carry is "
                     "left), 1 = the position 1/30 s ago (Rare's 30 fps start, so the check sees a 1/30 s move), 2 = the "
                     "same, but only after a touch that lifted her (a sloped surface)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(pdz_fix_ramp_step_edge, 0.2, "Gameplay",
                     "With pdz_fix_ramp_step, which touches count as a low edge Jo can rise over (the only touches the fix "
                     "acts on): the wall check's push must point up by at least this much (its vertical part over its "
                     "length: 0 = a vertical wall, 1 = flat floor). Walls (below it) get Rare's code exactly: no carried "
                     "move, no earlier start")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_fix_ramp_step_walls, false, "Gameplay",
                    "Testing: with pdz_fix_ramp_step, act on walls too (the earlier rule; Jo bounces back a little "
                    "when she runs into a wall above 30 fps)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_INT32(pdz_step_marker, 0, "Diagnostics",
                     "Testing: a number written into the step log when it changes (input scripts label attempts)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REX_EXTERN(__imp__sub_82697088); // mover update (character, mover, f1 = dt)
REX_EXTERN(__imp__sub_8269C580); // move resolve (character, type, &previous) -> bit 0 ground, bit 1 wall
REX_EXTERN(__imp__sub_8269CEF0); // ground probe (&position in / out, character, type, &previous) -> hit
REX_EXTERN(__imp__sub_8269C8F0); // wall sweep (character, type, &previous) -> hit
REX_EXTERN(__imp__sub_8269C688); // the sweep sphere's height above the feet (character) -> f1

namespace
{
  constexpr uint32_t kPlayerVtable = 0x820C7DE0; // TU3's player class
  constexpr uint32_t kMoverPosition = 360;
  constexpr uint32_t kMoverVertical = 332;
  constexpr uint32_t kMoverAirborne = 601;
  constexpr uint32_t kMoverFalling = 606;
  constexpr uint32_t kCharacterPosition = 48;

  uint32_t Load32(const uint8_t *base, uint32_t a)
  {
    const uint8_t *p = base + a;
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
  }
  float LoadF(const uint8_t *base, uint32_t a)
  {
    const uint32_t v = Load32(base, a);
    float f;
    std::memcpy(&f, &v, 4);
    return f;
  }
  void StoreF(uint8_t *base, uint32_t a, float f)
  {
    uint32_t v;
    std::memcpy(&v, &f, 4);
    uint8_t *p = base + a;
    p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v);
  }
  bool IsGuestPointer(uint32_t a) { return a >= 0x00010000u && a < 0xFFFF0000u; }
  struct V3
  {
    float x = 0, y = 0, z = 0;
  };
  V3 LoadV(const uint8_t *base, uint32_t a) { return {LoadF(base, a), LoadF(base, a + 4), LoadF(base, a + 8)}; }
  float Horizontal(const V3 &a, const V3 &b) { return std::hypot(b.x - a.x, b.z - a.z); }

  struct Frame
  {
    bool in_update = false;
    uint32_t mover = 0;
    bool resolved = false;
    V3 previous, target;       // at the resolve's entry
    bool probed = false;
    float probe_in_y = 0, probe_out_y = 0, ny = 0;
    uint32_t probe_hit = 0;
    bool swept = false;
    V3 sweep_in, sweep_out;
    uint32_t sweep_hit = 0;
    float sphere = -1.0f;
    uint32_t resolve = ~0u;
    float dt = 0.0f;
    float carried = 0.0f;     // cm of an earlier blocked move added this frame (pdz_fix_ramp_step)
    int start_back = 0;       // pdz_fix_ramp_step_start: the sweep started from the position this many frames back
    float start_shift = 0.0f; // ... which was this far (cm) from the previous frame's position
    float touch_ny = 0.0f;    // the sweep push's vertical share (EdgeTouch)
    bool edge = false;        // ... a low edge (the fix may act on it)
    bool wall_skip = false;   // a wall took part of the move: Rare's code, nothing carried
    bool held = false;        // the fix's sweep ended behind her: she stayed where she was
  };
  // pdz_fix_ramp_step: the player's last resolved positions (the position after each frame's resolve), newest
  // last, so the wall sweep can start from where Jo was 1/30 s ago.
  struct Resolved
  {
    V3 pos;
    float dt = 0.0f;     // the frame's dt
    uint64_t update = 0; // the character's mover update count of the frame (consecutive = no frame missed)
    bool grounded = false, touched = false, lifted = false, edge = false;
  };
  constexpr int kRing = 16;

  // Everything the fix keeps between frames, per character (0.9.4-rc1 kept one copy for all players, so in split
  // screen / co-op one player's carried move and position history could reach another's).
  struct MoverState
  {
    uint32_t character = 0;
    uint64_t updates = 0, last_use = 0;
    float pending[2] = {0, 0}; // the blocked part of earlier moves (x, z), carried into the next resolve
    float speed_ref = 0.0f;    // cm/s of the last move that wasn't blocked (the speed Jo reached the obstacle with)
    Resolved ring[kRing];
    int ring_head = 0, ring_n = 0; // head = index of the newest entry
    const Resolved &Back(int back) const // back = 1: the previous frame
    {
      return ring[(ring_head - (back - 1) + kRing) % kRing];
    }
    void Push(const Resolved &r)
    {
      ring_head = (ring_head + 1) % kRing;
      ring[ring_head] = r;
      ring_n = std::min(ring_n + 1, kRing);
    }
  };
  constexpr int kMovers = 40;
  MoverState g_movers[kMovers];
  uint64_t g_mover_clock = 0;
  MoverState *MoverFor(uint32_t character)
  {
    ++g_mover_clock;
    MoverState *oldest = &g_movers[0];
    for (MoverState &m : g_movers)
    {
      if (m.character == character)
      {
        m.last_use = g_mover_clock;
        return &m;
      }
      if (m.last_use < oldest->last_use)
      {
        oldest = &m;
      }
    }
    *oldest = MoverState{};
    oldest->character = character;
    oldest->last_use = g_mover_clock;
    return oldest;
  }

  thread_local Frame t_frame;
  thread_local MoverState *t_mover = nullptr;
  uint64_t g_frames = 0, g_last_still = 0;

  uint64_t g_carry_frames = 0, g_carry_through = 0, g_wall_skips = 0, g_back_holds = 0;
  bool g_carry_logged = false;
  constexpr float kRareRate = 30.0f;
  uint64_t g_start_frames = 0;
  bool g_start_logged = false;

  // A touch of the wall sweep: its push (target -> result, 3D). For a slide along one surface the push is along that
  // surface's normal, so its vertical share says what she touched: about 0 = a wall (vertical), more = a slope or the
  // top of an edge that lifts her (the Subway ramp's foot). The fix acts only on the second kind.
  bool EdgeTouch(const V3 &in, const V3 &out, float *ny_out)
  {
    const float cx = out.x - in.x, cy = out.y - in.y, cz = out.z - in.z;
    const float len = std::sqrt(cx * cx + cy * cy + cz * cz);
    const float ny = len >= 0.02f ? cy / len : 0.0f;
    if (ny_out)
    {
      *ny_out = ny;
    }
    return len >= 0.02f && ny >= float(REXCVAR_GET(pdz_fix_ramp_step_edge));
  }

  // The frame the sweep should start from (frames back, 1 = the previous frame = Rare's code), or 0 for no change: the
  // oldest resolved position within 1/30 s of this frame's end, when the previous frame's sweep touched a low edge
  // (mode 1; with pdz_fix_ramp_step_walls any touch, the rc1 rule) or lifted her (mode 2), this frame has ground and
  // every frame in between was a consecutive, grounded move.
  int StaleStartBack(const MoverState &m, const Frame &f, int mode)
  {
    if (mode <= 0 || m.ring_n < 2 || !f.probed || !f.probe_hit || f.dt <= 0.0f)
    {
      return 0;
    }
    const Resolved &prev = m.Back(1);
    const bool walls = REXCVAR_GET(pdz_fix_ramp_step_walls);
    const bool trigger = mode == 2 ? prev.lifted && (walls || prev.edge) : walls ? prev.touched : prev.edge;
    if (!trigger || !prev.grounded || prev.update + 1 != m.updates)
    {
      return 0; // no touch, or the previous frame was not the previous mover update (an airborne frame between)
    }
    const float limit = 1.0f / kRareRate + 0.25f * f.dt;
    float age = f.dt; // from the end of the previous frame to the end of this one
    int back = 1;
    while (back < m.ring_n)
    {
      const Resolved &r = m.Back(back);         // the frame whose dt separates entry back+1 from entry back
      const Resolved &older = m.Back(back + 1); // the candidate start
      if (age + r.dt > limit || !older.grounded || older.update + 1 != r.update)
      {
        break;
      }
      age += r.dt;
      ++back;
    }
    if (back < 2)
    {
      return 0;
    }
    const V3 &a = m.Back(back).pos;
    const V3 &b = prev.pos;
    if (Horizontal(a, b) > 40.0f || std::fabs(a.y - b.y) > 30.0f)
    {
      return 0; // a warp or a jump in between: not a continuous move
    }
    return back;
  }
} // namespace

// sub_8269C688(r3 = &offset out, r4 = &centre out, r5 = character, r6 = type, r7) -> r3 = the offset of the sweep
// sphere's centre above the feet.
REX_HOOK_RAW(sub_8269C688)
{
  const bool mine = t_frame.in_update && IsGuestPointer(ctx.r5.u32) && Load32(base, ctx.r5.u32) == kPlayerVtable;
  __imp__sub_8269C688(ctx, base);
  if (mine && IsGuestPointer(ctx.r3.u32))
  {
    t_frame.sphere = LoadF(base, ctx.r3.u32 + 4);
  }
}

REX_HOOK_RAW(sub_8269CEF0)
{
  const uint32_t position = ctx.r3.u32, character = ctx.r4.u32;
  const bool mine = t_frame.in_update && IsGuestPointer(character) && Load32(base, character) == kPlayerVtable;
  const float in_y = mine ? LoadF(base, position + 4) : 0.0f;
  __imp__sub_8269CEF0(ctx, base);
  if (mine)
  {
    Frame &f = t_frame;
    f.probed = true;
    f.probe_in_y = in_y;
    f.probe_out_y = LoadF(base, position + 4);
    f.probe_hit = ctx.r3.u32 & 0xFF;
    const uint32_t controller = Load32(base, character + 228);
    if (IsGuestPointer(controller) && Load32(base, controller) == 0x820CB320u)
    {
      f.ny = LoadF(base, controller + 0x80 + 40); // the probe's ground normal y (a5-step-up)
    }
  }
}

REX_HOOK_RAW(sub_8269C8F0)
{
  const uint32_t character = ctx.r3.u32;
  MoverState *m = t_mover;
  const bool mine = t_frame.in_update && t_frame.mover && m && IsGuestPointer(character) &&
                    Load32(base, character) == kPlayerVtable;
  const uint32_t previous = ctx.r5.u32; // sub_8269C580's own copy of the previous position: the sweep's start
  V3 saved;
  int back = 0;
  if (mine)
  {
    Frame &f = t_frame;
    f.sweep_in = LoadV(base, f.mover + kMoverPosition);
    // pdz_fix_ramp_step_start: after a low-edge touch, sweep from where Jo was 1/30 s ago (Rare's 30 fps start), so the
    // sweep sees a 1/30 s move against the same geometry; the probe has already put this frame's feet on the ground.
    const uint32_t hz = pdz::pace::EffectiveHz();
    if (REXCVAR_GET(pdz_fix_ramp_step) && hz > 31 && hz <= 480 && f.dt < 1.0f / 31.0f && IsGuestPointer(previous))
    {
      back = StaleStartBack(*m, f, REXCVAR_GET(pdz_fix_ramp_step_start));
      if (back > 0)
      {
        saved = LoadV(base, previous);
        const V3 &start = m->Back(back).pos;
        StoreF(base, previous, start.x);
        StoreF(base, previous + 4, start.y);
        StoreF(base, previous + 8, start.z);
        f.start_back = back;
        f.start_shift = std::hypot(Horizontal(start, saved), start.y - saved.y);
        ++g_start_frames;
        if (!g_start_logged)
        {
          g_start_logged = true;
          REXLOG_INFO("pdz_fix_ramp_step: after a low-edge touch, the wall check starts from the position {} frames "
                      "back ({:.2f} cm from the previous frame's) at {} Hz, dt {:.4f}",
                      back, f.start_shift, hz, f.dt);
        }
      }
    }
  }
  __imp__sub_8269C8F0(ctx, base);
  if (mine)
  {
    if (back > 0)
    {
      StoreF(base, previous, saved.x);
      StoreF(base, previous + 4, saved.y);
      StoreF(base, previous + 8, saved.z);
    }
    Frame &f = t_frame;
    f.swept = true;
    f.sweep_out = LoadV(base, f.mover + kMoverPosition);
    f.sweep_hit = ctx.r3.u32 & 0xFF;
    f.edge = EdgeTouch(f.sweep_in, f.sweep_out, &f.touch_ny);
    // Never backward because of the fix: when this frame's sweep started early or tried a carried move, and it ended
    // behind where she was (against the stick's own move), she stays where she was instead. (Rare's own sweep can
    // still move her back a little on frames the fix leaves alone.)
    if ((back > 0 || f.carried > 0.0f) && f.resolved && !REXCVAR_GET(pdz_fix_ramp_step_walls))
    {
      const float sx = f.target.x - f.previous.x, sz = f.target.z - f.previous.z;
      const float rx = f.sweep_out.x - f.previous.x, rz = f.sweep_out.z - f.previous.z;
      if (sx * rx + sz * rz < 0.0f)
      {
        StoreF(base, f.mover + kMoverPosition, f.previous.x);
        StoreF(base, f.mover + kMoverPosition + 4, f.previous.y);
        StoreF(base, f.mover + kMoverPosition + 8, f.previous.z);
        f.held = true;
        ++g_back_holds;
      }
    }
  }
}

REX_HOOK_RAW(sub_8269C580)
{
  const uint32_t character = ctx.r3.u32, previous = ctx.r5.u32;
  MoverState *m = t_mover;
  const bool mine = t_frame.in_update && t_frame.mover && m && IsGuestPointer(character) &&
                    IsGuestPointer(previous) && Load32(base, character) == kPlayerVtable;
  if (!mine)
  {
    __imp__sub_8269C580(ctx, base);
    return;
  }
  Frame &f = t_frame;
  f.resolved = true;
  f.previous = LoadV(base, previous);
  f.target = LoadV(base, f.mover + kMoverPosition);
  // pdz_fix_ramp_step: above 30 fps, add the blocked part of the last frames' move, so that the move this resolve
  // tries is up to what one 1/30 s frame would try at the current speed (Rare's 30 fps).
  const float dt = f.dt;
  const uint32_t hz = pdz::pace::EffectiveHz();
  const bool fix = REXCVAR_GET(pdz_fix_ramp_step) && hz > 31 && hz <= 480 && dt > 0.0f && dt < 1.0f / 31.0f;
  float mx = f.target.x - f.previous.x, mz = f.target.z - f.previous.z;
  const float len = std::hypot(mx, mz);
  if (!fix || len < 0.01f || mx * m->pending[0] + mz * m->pending[1] < 0.0f)
  {
    m->pending[0] = m->pending[1] = 0.0f; // no stick, a turn of more than 90 degrees, or the switch off
    if (!fix || len < 0.01f)
    {
      m->speed_ref = 0.0f;
    }
  }
  else if (m->pending[0] != 0.0f || m->pending[1] != 0.0f)
  {
    // The 1/30 s move at the speed Jo had before the obstacle stopped her (being blocked slows her walk down within a
    // few frames; at 30 fps she reaches the obstacle with one or two whole 1/30 s moves before that).
    const float cap = std::max(len / dt, m->speed_ref) / kRareRate;
    float tx = mx + m->pending[0], tz = mz + m->pending[1];
    const float tl = std::hypot(tx, tz);
    if (tl > cap)
    {
      tx *= cap / tl, tz *= cap / tl;
    }
    f.carried = std::hypot(tx, tz) - len;
    StoreF(base, f.mover + kMoverPosition, f.previous.x + tx);
    StoreF(base, f.mover + kMoverPosition + 8, f.previous.z + tz);
    mx = tx, mz = tz;
    ++g_carry_frames;
    if (!g_carry_logged)
    {
      g_carry_logged = true;
      REXLOG_INFO("pdz_fix_ramp_step: a blocked move carried into the next frame ({:.2f} cm tried instead of {:.2f}; "
                  "at most the 1/30 s move, {:.2f} cm, at {} Hz)",
                  std::hypot(mx, mz), len, cap, hz);
    }
  }
  __imp__sub_8269C580(ctx, base);
  f.resolve = ctx.r3.u32 & 0xFF;
  {
    // The ring of resolved positions for pdz_fix_ramp_step_start (kept whether or not the fix is on, so a switch-on has
    // history). touched / lifted: the sweep moved the position (3D) / raised it; edge: a low-edge touch (EdgeTouch).
    Resolved r;
    r.pos = LoadV(base, f.mover + kMoverPosition);
    r.dt = dt;
    r.update = m->updates;
    r.grounded = f.probed && f.probe_hit != 0;
    if (f.swept)
    {
      const float moved = std::hypot(Horizontal(f.sweep_in, f.sweep_out), f.sweep_out.y - f.sweep_in.y);
      r.touched = moved >= 0.02f;
      r.lifted = r.touched && f.sweep_out.y - f.sweep_in.y >= 0.02f;
      r.edge = f.edge;
    }
    m->Push(r);
  }
  if (fix)
  {
    const V3 after = LoadV(base, f.mover + kMoverPosition);
    const float ax = after.x - f.previous.x, az = after.z - f.previous.z;
    const float tried = std::hypot(mx, mz);
    const float got = std::hypot(ax, az);
    const bool blocked =
        tried >= 0.01f && f.swept && Horizontal(f.sweep_in, f.sweep_out) >= 0.1f * tried && got < tried;
    if (blocked && (f.edge || REXCVAR_GET(pdz_fix_ramp_step_walls)))
    {
      m->pending[0] = mx - ax, m->pending[1] = mz - az; // a low edge took part of the move: keep that part
    }
    else
    {
      if (blocked)
      {
        f.wall_skip = true; // a wall took it: Rare's code, nothing kept
        ++g_wall_skips;
      }
      else if (f.carried <= 0.0f)
      {
        m->speed_ref = len / dt; // an unblocked frame: the speed she's moving at
      }
      if (f.carried > 0.0f && !blocked)
      {
        ++g_carry_through;
      }
      m->pending[0] = m->pending[1] = 0.0f;
    }
  }
}

REX_HOOK_RAW(sub_82697088)
{
  const uint32_t character = ctx.r3.u32, mover = ctx.r4.u32;
  if (!IsGuestPointer(character) || !IsGuestPointer(mover) || Load32(base, character) != kPlayerVtable)
  {
    __imp__sub_82697088(ctx, base);
    return;
  }
  MoverState *m = MoverFor(character);
  ++m->updates;
  if (!REXCVAR_GET(pdz_step_log))
  {
    t_frame = Frame{};
    t_frame.in_update = true;
    t_frame.mover = mover;
    t_frame.dt = float(ctx.f1.f64);
    t_mover = m;
    __imp__sub_82697088(ctx, base);
    t_frame = Frame{};
    t_mover = nullptr;
    return;
  }
  static int32_t marker = 0;
  if (REXCVAR_GET(pdz_step_marker) != marker)
  {
    marker = REXCVAR_GET(pdz_step_marker);
    REXLOG_INFO("pdz_step: ATTEMPT {} (pdz_step_marker)", marker);
  }
  const float dt = float(ctx.f1.f64);
  const V3 before = LoadV(base, mover + kMoverPosition);
  t_frame = Frame{};
  t_frame.in_update = true;
  t_frame.mover = mover;
  t_frame.dt = dt;
  t_mover = m;
  __imp__sub_82697088(ctx, base);
  const Frame f = t_frame;
  t_frame = Frame{};
  t_mover = nullptr;
  ++g_frames;
  const V3 after = LoadV(base, mover + kMoverPosition);
  const float achieved = Horizontal(before, after);
  const float intended = f.resolved ? Horizontal(f.previous, f.target) : 0.0f;
  const bool moving = achieved > 0.01f || intended > 0.01f || std::fabs(after.y - before.y) > 0.01f;
  if (!moving && g_frames - g_last_still < 120)
  {
    return;
  }
  if (!moving)
  {
    g_last_still = g_frames;
  }
  const float swept_off = f.swept ? Horizontal(f.sweep_in, f.sweep_out) : 0.0f;
  const float probe_off = f.resolved && f.swept ? Horizontal(f.target, f.sweep_in) : 0.0f;
  const char *stopped = "-";
  if (intended > 0.05f && achieved < 0.5f * intended)
  {
    stopped = swept_off >= probe_off && swept_off > 0.25f * intended ? "SWEEP"
              : probe_off > 0.25f * intended                       ? "PROBE"
                                                                    : "OTHER";
  }
  const float view_y = LoadF(base, character + kCharacterPosition + 4);
  // move / got: the stick's move this frame (before any carry) and the achieved move (x, z); BACKWARD: they point apart.
  const float ix = f.resolved ? f.target.x - f.previous.x : 0.0f, iz = f.resolved ? f.target.z - f.previous.z : 0.0f;
  const float gx = after.x - before.x, gz = after.z - before.z;
  const bool backward = intended > 0.05f && achieved > 0.02f && ix * gx + iz * gz < 0.0f;
  const bool pushed = f.swept && (f.sweep_in.x != f.sweep_out.x || f.sweep_in.y != f.sweep_out.y ||
                                  f.sweep_in.z != f.sweep_out.z);
  REXLOG_INFO("pdz_step: #{} dt {:.4f} sphere {:.0f} | pos ({:.2f}, {:.2f}, {:.2f}) view_y {:.2f} (+{:.1f}) | intended "
              "{:.2f} cm dy {:.2f} | probe in_y {:.2f} ground_y {:.2f} hit {} ny {:.3f} | sweep removed {:.2f} cm "
              "hit {} resolve {} | achieved {:.2f} cm ({:.0f} cm/s) dy {:.2f} | air {} fall {} vy {:.1f} | stopped by {} | "
              "carried {:.2f} cm (carries {}, through {}) | start -{}f {:.2f} cm (starts {}) | move ({:.2f}, {:.2f}) "
              "got ({:.2f}, {:.2f}){}{} | touch ny {:.2f} {} (wall skips {}, holds {}) | char {:08X}",
              g_frames, dt, f.sphere, after.x, after.y, after.z, view_y, view_y - after.y, intended,
              f.resolved ? f.target.y - f.previous.y : 0.0f, f.probe_in_y, f.probe_out_y, f.probe_hit, f.ny, swept_off,
              f.sweep_hit, f.resolve == ~0u ? -1 : int(f.resolve), achieved, dt > 0 ? achieved / dt : 0.0f,
              after.y - before.y, int(base[mover + kMoverAirborne]), int(base[mover + kMoverFalling]),
              LoadF(base, mover + kMoverVertical), stopped, f.carried, g_carry_frames, g_carry_through,
              f.start_back > 0 ? f.start_back : 1, f.start_shift, g_start_frames, ix, iz, gx, gz,
              backward ? " BACKWARD" : "", f.held ? " HELD" : "", f.touch_ny, !pushed ? "-" : f.edge ? "edge" : "wall",
              g_wall_skips, g_back_holds,
              character);
}
