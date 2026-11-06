// Cover diagnostic (pdz_cover_log). See README.md next to this file.
//
// Logs the player's cover state machine with timestamps, so transition times
// can be measured in game, and the enemies' cover tasks (how often they take
// cover and how it ends). It only reads; cover behaviour is unchanged.
//
// sub_824F11C0(player, f1 = dt) runs the cover module once per player update
// (from sub_82598128). The state is at player +0x1CA4 (0 = not in cover); a
// state change is queued in the global 82E3B588 and applied at the end of the
// update (sub_824F03A8 applies its own). Out of cover it runs the detection (sub_824F4AB8): one probe a frame
// until it has a candidate at +0x1D4C/+0x1D50, which raises the "take cover"
// prompt (context action 0x7A). Pressing A calls the player's action callback
// sub_8259B2B8 (vtable +148), which enters cover through sub_824F03A8.
// sub_824F1610 forces the player out of cover.
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/hook.h>
#include <rex/logging.h>

#include "../game_cvars.h"
#include "ai_cover_fixes.h"
#include "game_fixes.h"

REX_EXTERN(__imp__sub_824F11C0);
REX_EXTERN(__imp__sub_8259B2B8);
REX_EXTERN(__imp__sub_824F03A8);
REX_EXTERN(__imp__sub_824F1610);

namespace
{
  uint32_t LoadBe32(const uint8_t *base, uint32_t address)
  {
    const uint8_t *p = base + address;
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
  }

  float LoadBeFloat(const uint8_t *base, uint32_t address)
  {
    const uint32_t bits = LoadBe32(base, address);
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
  }

  constexpr float kRadToDeg = 57.2957795f;

  // Player fields (world units are cm, angles radians).
  constexpr uint32_t kPlayerPosition = 0x30; // x, y, z
  constexpr uint32_t kCoverState = 0x1CA4;
  constexpr uint32_t kCoverSubstate = 0x1CAC; // target state while in state 2
  constexpr uint32_t kCoverSide = 0x1CB0;
  constexpr uint32_t kCoverMode = 0x1CB4;
  constexpr uint32_t kCoverFlagsE0 = 0x1CE0;
  constexpr uint32_t kCoverFlagsE4 = 0x1CE4;
  constexpr uint32_t kCoverWallYaw = 0x1CE8;
  constexpr uint32_t kCoverBusy = 0x1D2D;
  constexpr uint32_t kCoverLocked = 0x1D6C;
  // Detection result: cover type flags, cover point and its angle.
  constexpr uint32_t kCandidateType = 0x1D4C;
  constexpr uint32_t kCandidatePoint = 0x1D50;
  constexpr uint32_t kCandidateAngle = 0x1D5C;
  // Cover states (+0x1CA4). In state 2 the target state is at +0x1CAC.
  constexpr uint32_t kNotInCover = 0;
  constexpr uint32_t kEntering = 1;   // entry animation, forced to 0.5 s
  constexpr uint32_t kTransition = 2; // lean out (0.25 s) or back in (0.8 s)
  constexpr uint32_t kInCover = 3;
  constexpr uint32_t kPoppedOut = 4; // the only state that can fire
  constexpr uint32_t kExiting = 5;   // exit animation and camera blend
  // Input struct at [player +0x19A0]: pointers to the held and pressed-this-
  // frame buttons, and the Cover Mode option (0 = hold A, else toggle).
  constexpr uint32_t kPlayerInput = 0x19A0;
  constexpr uint32_t kInputHeld = 0x508;
  constexpr uint32_t kInputPressed = 0x510;
  constexpr uint32_t kInputCoverMode = 0x4E6;
  constexpr uint32_t kButtonA = 0x40000;

  using Clock = std::chrono::steady_clock;

  struct Snapshot
  {
    uint32_t state = 0;
    uint32_t substate = 0;
    uint32_t side = 0;
    uint32_t mode = 0;
    uint32_t flags_e0 = 0;
    uint32_t flags_e4 = 0;
    uint8_t busy = 0;
    uint8_t locked = 0;
    uint32_t candidate = 0;

    bool operator==(const Snapshot &other) const
    {
      return std::memcmp(this, &other, sizeof(*this)) == 0;
    }
  };

  struct PlayerLog
  {
    bool valid = false;
    Snapshot last;
    Clock::time_point state_since;  // last change of +0x1CA4
    Clock::time_point change_since; // last change of anything logged
    Clock::time_point enter_request;
    Clock::time_point exit_request;
    Clock::time_point cover_since;
    bool enter_pending = false;
    bool exit_pending = false;
  };
  std::unordered_map<uint32_t, PlayerLog> g_logs;

  bool Enabled()
  {
    return REXCVAR_GET(pdz_cover_log);
  }

  double Ms(Clock::time_point from, Clock::time_point to)
  {
    return std::chrono::duration<double, std::milli>(to - from).count();
  }

  // Seconds since the first logged event, for lining lines up with video.
  double Stamp(Clock::time_point now)
  {
    static const Clock::time_point start = now;
    return std::chrono::duration<double>(now - start).count();
  }

  Snapshot Read(const uint8_t *base, uint32_t player)
  {
    Snapshot s;
    s.state = LoadBe32(base, player + kCoverState);
    s.substate = LoadBe32(base, player + kCoverSubstate);
    s.side = LoadBe32(base, player + kCoverSide);
    s.mode = LoadBe32(base, player + kCoverMode);
    s.flags_e0 = LoadBe32(base, player + kCoverFlagsE0);
    s.flags_e4 = LoadBe32(base, player + kCoverFlagsE4);
    s.busy = base[player + kCoverBusy];
    s.locked = base[player + kCoverLocked];
    s.candidate = LoadBe32(base, player + kCandidateType);
    return s;
  }

  // Cover Mode ("hold" / "toggle") and whether A is held / pressed this frame.
  void ReadInput(const uint8_t *base, uint32_t player, const char *&mode, bool &held, bool &pressed)
  {
    mode = "?";
    held = pressed = false;
    const uint32_t input = LoadBe32(base, player + kPlayerInput);
    if (!input)
    {
      return;
    }
    mode = base[input + kInputCoverMode] ? "toggle" : "hold";
    const uint32_t held_ptr = LoadBe32(base, input + kInputHeld);
    const uint32_t pressed_ptr = LoadBe32(base, input + kInputPressed);
    held = held_ptr && (LoadBe32(base, held_ptr) & kButtonA);
    pressed = pressed_ptr && (LoadBe32(base, pressed_ptr) & kButtonA);
  }

  float DistanceToCandidate(const uint8_t *base, uint32_t player)
  {
    const float dx = LoadBeFloat(base, player + kCandidatePoint) - LoadBeFloat(base, player + kPlayerPosition);
    const float dz = LoadBeFloat(base, player + kCandidatePoint + 8) - LoadBeFloat(base, player + kPlayerPosition + 8);
    return std::sqrt(dx * dx + dz * dz);
  }
} // namespace

// sub_8259B2B8(player, target): context action callback; with target ==
// player and a cover candidate it enters cover (tail call to sub_824F03A8).
REX_HOOK_RAW(sub_8259B2B8)
{
  const uint32_t player = ctx.r3.u32;
  if (Enabled() && ctx.r4.u32 == player && LoadBe32(base, player + kCandidateType))
  {
    const auto now = Clock::now();
    PlayerLog &log = g_logs[player];
    log.enter_request = now;
    log.enter_pending = true;
    const char *mode;
    bool held, pressed;
    ReadInput(base, player, mode, held, pressed);
    REXLOG_INFO("pdz_cover: [{:9.3f}] {:08X} enter requested: cover type {:#x}, point {:.0f} cm away, "
                "angle {:.0f} deg, cover mode {}, A held {} pressed {}",
                Stamp(now), player, LoadBe32(base, player + kCandidateType), DistanceToCandidate(base, player),
                LoadBeFloat(base, player + kCandidateAngle) * kRadToDeg, mode, held, pressed);
  }
  __imp__sub_8259B2B8(ctx, base);
}

// sub_824F03A8(player, &point, f1, r6 = cover type): starts entering cover.
REX_HOOK_RAW(sub_824F03A8)
{
  const uint32_t player = ctx.r3.u32;
  __imp__sub_824F03A8(ctx, base);
  if (Enabled())
  {
    // It applies its queued state itself: 1 = entering.
    const uint32_t state = LoadBe32(base, player + kCoverState);
    REXLOG_INFO("pdz_cover: [{:9.3f}] {:08X} enter {}: state {}, target {}, side {:#x}",
                Stamp(Clock::now()), player, state == kEntering ? "started" : "refused", state,
                LoadBe32(base, player + kCoverSubstate), LoadBe32(base, player + kCoverSide));
  }
}

// sub_824F1610(player, ...): takes the player out of cover.
REX_HOOK_RAW(sub_824F1610)
{
  const uint32_t player = ctx.r3.u32;
  game_fixes::OnForcedCoverExit(base, player);
  if (Enabled())
  {
    const auto now = Clock::now();
    PlayerLog &log = g_logs[player];
    log.exit_request = now;
    log.exit_pending = true;
    REXLOG_INFO("pdz_cover: [{:9.3f}] {:08X} forced exit (from {:08X}, instant {}), state {}", Stamp(now),
                player, uint32_t(ctx.lr), ctx.r4.u32, LoadBe32(base, player + kCoverState));
  }
  __imp__sub_824F1610(ctx, base);
}

// sub_824F11C0(player, f1 = dt): the cover module's per-frame update.
REX_HOOK_RAW(sub_824F11C0)
{
  const uint32_t player = ctx.r3.u32;
  const double dt = ctx.f1.f64;
  __imp__sub_824F11C0(ctx, base);
  if (!Enabled())
  {
    return;
  }

  const auto now = Clock::now();
  PlayerLog &log = g_logs[player];
  const Snapshot s = Read(base, player);
  if (log.valid && s == log.last)
  {
    return;
  }

  const Snapshot old = log.valid ? log.last : Snapshot{};
  if (!log.valid)
  {
    log.state_since = now;
    log.change_since = now;
  }
  if (s.candidate != old.candidate)
  {
    if (s.candidate)
    {
      REXLOG_INFO("pdz_cover: [{:9.3f}] {:08X} cover found: type {:#x}, {:.0f} cm away", Stamp(now), player,
                  s.candidate, DistanceToCandidate(base, player));
    }
    else
    {
      REXLOG_INFO("pdz_cover: [{:9.3f}] {:08X} cover lost", Stamp(now), player);
    }
  }

  if (s.state != old.state)
  {
    const char *event = "state change";
    double since = Ms(log.state_since, now);
    const char *since_what = "in the previous state";
    if (old.state == kEntering && s.state == kInCover)
    {
      event = "snapped in";
      if (log.enter_pending)
      {
        since = Ms(log.enter_request, now);
        since_what = "since enter requested";
      }
      log.enter_pending = false;
      log.cover_since = now;
    }
    else if (old.state == kInCover && s.state == kTransition && s.substate == kPoppedOut)
    {
      event = "lean out start";
    }
    else if (old.state == kTransition && s.state == kPoppedOut)
    {
      event = "leaned out (can fire)";
      since_what = "since lean out start";
    }
    else if (old.state == kPoppedOut && s.state == kTransition && s.substate == kInCover)
    {
      event = "lean back start";
      since_what = "popped out";
    }
    else if (old.state == kTransition && s.state == kInCover)
    {
      event = "lean end (back in cover)";
      since_what = "since lean back start";
    }
    else if (s.state == kExiting)
    {
      event = "exit requested";
      log.exit_request = now;
      log.exit_pending = true;
    }
    else if (s.state == kNotInCover)
    {
      event = old.state == kExiting ? "exited" : "exited instantly";
      if (log.exit_pending)
      {
        since = Ms(log.exit_request, now);
        since_what = "since exit requested";
      }
      log.exit_pending = false;
      log.enter_pending = false;
    }
    REXLOG_INFO("pdz_cover: [{:9.3f}] {:08X} {}: state {}->{} target {} side {:#x} mode {} | {:.0f} ms {}, "
                "dt {:.1f} ms",
                Stamp(now), player, event, old.state, s.state, s.substate, s.side, s.mode, since, since_what,
                dt * 1000.0);
    if (s.state == kNotInCover && old.state != kEntering)
    {
      REXLOG_INFO("pdz_cover: [{:9.3f}] {:08X} out of cover after {:.0f} ms in cover", Stamp(now), player,
                  Ms(log.cover_since, now));
    }
    log.state_since = now;
    log.change_since = now;
  }
  // +0x1CE0 (stay-out timer) and +0x1CE4 (an angle) are floats that change
  // continuously, so they are not logged as changes.
  else if (s.substate != old.substate || s.side != old.side || s.mode != old.mode || s.busy != old.busy ||
           s.locked != old.locked)
  {
    REXLOG_INFO("pdz_cover: [{:9.3f}] {:08X} detail: state {} target {}->{} side {:#x}->{:#x} mode {}->{} "
                "busy {}->{} locked {}->{} | {:.0f} ms since last change, wall {:.0f} deg",
                Stamp(now), player, s.state, old.substate, s.substate, old.side, s.side, old.mode, s.mode,
                old.busy, s.busy, old.locked, s.locked, Ms(log.change_since, now),
                LoadBeFloat(base, player + kCoverWallYaw) * kRadToDeg);
    log.change_since = now;
  }
  log.last = s;
  log.valid = true;
}

// ---------------------------------------------------------------------------
// Enemy AI cover (lines start with "pdz_cover_ai:"). The AI's cover task is a
// separate system: vtable 82094758, constructor sub_821C6AC8(task, point,
// regardless), Start sub_821C6D08, Update sub_821C7268, cover tests
// sub_821C7D70, peek position sub_821C7908, End sub_821C71B0. Cover points are
// level pads: position +0x10, flags +0x34 (0x8 low cover, 0x100 / 0x200
// sides), suppression count +0x5E. Timers run on the AI clock (float seconds
// at 82E2E478). The same hooks apply the pdz_ai_cover_fix_* options
// (ai_cover_fixes.cpp) whether or not the log is on.
REX_EXTERN(__imp__sub_821C6AC8);
REX_EXTERN(__imp__sub_821C6D08);
REX_EXTERN(__imp__sub_821C7268);
REX_EXTERN(__imp__sub_821C7D70);
REX_EXTERN(__imp__sub_821C7908);
REX_EXTERN(__imp__sub_821C71B0);
REX_EXTERN(__imp__sub_821C62F0);
REX_EXTERN(__imp__sub_821C6828);

namespace
{
  double LoadBeDouble(const uint8_t *base, uint32_t address)
  {
    const uint64_t bits = (uint64_t(LoadBe32(base, address)) << 32) | LoadBe32(base, address + 4);
    double value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
  }

  // Task fields.
  constexpr uint32_t kTaskState = 0x0C; // 0 new, 1 running, 2 done, 3 stopped
  constexpr uint32_t kTaskCharacter = 0x10;
  constexpr uint32_t kTaskSequence = 0x3C; // walk / into position / snap back
  constexpr uint32_t kTaskAttack = 0x44;
  constexpr uint32_t kTaskLookOut = 0x4C;
  constexpr uint32_t kTaskPeekVariant = 0x59;
  constexpr uint32_t kTaskLinkCount = 0x6C;
  constexpr uint32_t kTaskLastSight = 0x70;  // double, AI clock
  constexpr uint32_t kTaskLastInArc = 0x78;  // double, AI clock
  constexpr uint32_t kTaskPeekWait = 0x90;   // float seconds
  constexpr uint32_t kTaskLookOutGap = 0x94; // float seconds
  // Cover point (pad) fields.
  constexpr uint32_t kPointPosition = 0x10;
  constexpr uint32_t kPointFlags = 0x34;
  constexpr uint32_t kPointSuppression = 0x5E;
  // Character fields: the pad the character is on (read by the in-place task
  // as its cover point).
  constexpr uint32_t kCharacterPad = 0xB4;
  constexpr uint32_t kCharacterPosition = 0x30; // x, y, z
  constexpr uint32_t kAiClock = 0x82E2E478;

  struct AiTaskLog
  {
    uint32_t character = 0;
    uint32_t point = 0;
    Clock::time_point created;
    Clock::time_point arrived;
    bool has_arrived = false;
    uint32_t sequence = 0;
    uint32_t attack = 0;
    uint32_t look_out = 0;
    uint32_t state = 0;
    int snap_backs = 0;
    int attacks = 0;
    int look_outs = 0;
    float start_position[3] = {};
  };
  std::unordered_map<uint32_t, AiTaskLog> g_ai_tasks;

  // Cover attempts each enemy gave up before reaching cover, for spotting
  // enemies that keep changing their mind (walking one way, then back).
  struct AbandonedLog
  {
    std::vector<Clock::time_point> times;
  };
  std::unordered_map<uint32_t, AbandonedLog> g_abandoned;
  constexpr auto kHesitationWindow = std::chrono::seconds(6);
  constexpr size_t kHesitationCount = 3;

  void ReadPosition(const uint8_t *base, uint32_t object, float out[3])
  {
    for (int i = 0; i < 3; ++i)
    {
      out[i] = object ? LoadBeFloat(base, object + kCharacterPosition + 4 * i) : 0.0f;
    }
  }

  float Distance2d(const float a[3], const float b[3])
  {
    const float dx = a[0] - b[0];
    const float dz = a[2] - b[2];
    return std::sqrt(dx * dx + dz * dz);
  }

  // Totals since the game started, printed at most once a minute.
  struct AiTotals
  {
    int created = 0;
    int in_place_started = 0;
    int in_place_refused = 0;
    int aborted_at_start = 0;
    int arrived = 0;
    int tests_failed = 0;
    int tests_failed_on_arrival = 0;
    int ended = 0;
    int snap_backs = 0;
    int attacks = 0;
    int look_outs = 0;
    int peek_ids[16] = {};
    std::unordered_map<uint32_t, int> characters;
    Clock::time_point last_print;
    bool dirty = false;
  } g_ai;

  void PrintAiTotals(Clock::time_point now)
  {
    if (!g_ai.dirty || now - g_ai.last_print < std::chrono::seconds(60))
    {
      return;
    }
    std::string peeks;
    for (int i = 0; i < 16; ++i)
    {
      if (g_ai.peek_ids[i])
      {
        peeks += fmt::format(" {}:{}", i, g_ai.peek_ids[i]);
      }
    }
    REXLOG_INFO("pdz_cover_ai: [{:9.3f}] totals: in place {} started / {} refused; move to cover: {} tasks, {} "
                "enemies reached cover, {} aborted at start, {} reached cover, {} failed cover tests ({} on "
                "arrival), {} ended, {} snap-backs, {} attacks from cover, {} look-outs, peek ids{} [fixes: {}]",
                Stamp(now), g_ai.in_place_started, g_ai.in_place_refused, g_ai.created, g_ai.characters.size(),
                g_ai.aborted_at_start, g_ai.arrived,
                g_ai.tests_failed, g_ai.tests_failed_on_arrival, g_ai.ended, g_ai.snap_backs, g_ai.attacks,
                g_ai.look_outs, peeks.empty() ? std::string(" none") : peeks, ai_cover_fixes::ActiveSummary());
    g_ai.last_print = now;
    g_ai.dirty = false;
  }

  std::string DescribePoint(const uint8_t *base, uint32_t point)
  {
    if (!point)
    {
      return "no point";
    }
    return fmt::format("point {:08X} at ({:.0f}, {:.0f}, {:.0f}) flags {:#x} suppressed {}", point,
                       LoadBeFloat(base, point + kPointPosition), LoadBeFloat(base, point + kPointPosition + 4),
                       LoadBeFloat(base, point + kPointPosition + 8), LoadBe32(base, point + kPointFlags),
                       base[point + kPointSuppression]);
  }
} // namespace

// sub_821C6AC8(task, point, regardless): an enemy's cover task is created.
REX_HOOK_RAW(sub_821C6AC8)
{
  const uint32_t task = ctx.r3.u32;
  const uint32_t point = ctx.r4.u32;
  const uint32_t regardless = ctx.r5.u32 & 0xFF;
  __imp__sub_821C6AC8(ctx, base);
  const bool rerolled = ai_cover_fixes::AfterCreate(base, task, point);
  if (!Enabled())
  {
    return;
  }
  const auto now = Clock::now();
  AiTaskLog &log = g_ai_tasks[task];
  log = AiTaskLog{};
  log.character = LoadBe32(base, task + kTaskCharacter);
  log.point = point;
  log.created = now;
  ++g_ai.created;
  g_ai.dirty = true;
  // The owner (+0x10) is only set when the task is started; see Start.
  REXLOG_INFO("pdz_cover_ai: [{:9.3f}] task {:08X} created: {}, regardless {}, peek variant {}{}, peek wait "
              "{:.1f} s, look-out gap {:.1f} s [fixes: {}]",
              Stamp(now), task, DescribePoint(base, point), regardless, base[task + kTaskPeekVariant],
              rerolled ? " (re-rolled 1 in 3)" : "", LoadBeFloat(base, task + kTaskPeekWait),
              LoadBeFloat(base, task + kTaskLookOutGap), ai_cover_fixes::ActiveSummary());
}

// sub_821C6D08(task): Start - walk to the point, then settle in.
REX_HOOK_RAW(sub_821C6D08)
{
  const uint32_t task = ctx.r3.u32;
  __imp__sub_821C6D08(ctx, base);
  if (!Enabled())
  {
    return;
  }
  AiTaskLog &log = g_ai_tasks[task];
  log.character = LoadBe32(base, task + kTaskCharacter);
  log.state = LoadBe32(base, task + kTaskState);
  log.sequence = LoadBe32(base, task + kTaskSequence);
  ReadPosition(base, log.character, log.start_position);
  float pad[3] = {};
  ReadPosition(base, log.point ? log.point + kPointPosition - kCharacterPosition : 0, pad);
  REXLOG_INFO("pdz_cover_ai: [{:9.3f}] task {:08X} started by {:08X} at ({:.0f}, {:.0f}, {:.0f}), {:.0f} cm from "
              "the point",
              Stamp(Clock::now()), task, log.character, log.start_position[0], log.start_position[1],
              log.start_position[2], log.point ? Distance2d(log.start_position, pad) : 0.0f);
  if (log.state == 2)
  {
    ++g_ai.aborted_at_start;
    g_ai.dirty = true;
    REXLOG_INFO("pdz_cover_ai: [{:9.3f}] task {:08X} ({:08X}) aborted at start: {} peek points, pad {:08X}",
                Stamp(Clock::now()), task, log.character, LoadBe32(base, task + kTaskLinkCount),
                log.character ? LoadBe32(base, log.character + kCharacterPad) : 0);
  }
}

// sub_821C7D70(task): cover tests; 0 = failed, the task ends.
REX_HOOK_RAW(sub_821C7D70)
{
  const uint32_t task = ctx.r3.u32;
  __imp__sub_821C7D70(ctx, base);
  if (!Enabled() || (ctx.r3.u32 & 0xFF))
  {
    return;
  }
  const auto now = Clock::now();
  AiTaskLog &log = g_ai_tasks[task];
  const double clock = LoadBeFloat(base, kAiClock);
  const bool on_arrival = log.has_arrived && now - log.arrived < std::chrono::milliseconds(100);
  ++g_ai.tests_failed;
  g_ai.tests_failed_on_arrival += on_arrival ? 1 : 0;
  g_ai.dirty = true;
  REXLOG_INFO("pdz_cover_ai: [{:9.3f}] task {:08X} ({:08X}) cover tests failed{}: no sight from peek points for "
              "{:.1f} s (limit 5), target out of arc for {:.1f} s (limit 6), {:.0f} ms since created, {:.0f} ms "
              "in cover",
              Stamp(now), task, log.character, on_arrival ? " ON ARRIVAL" : "",
              clock - LoadBeDouble(base, task + kTaskLastSight), clock - LoadBeDouble(base, task + kTaskLastInArc),
              Ms(log.created, now), log.has_arrived ? Ms(log.arrived, now) : 0.0);
}

// sub_821C7908(task, ...): peek position id for an attack from cover.
REX_HOOK_RAW(sub_821C7908)
{
  const uint32_t task = ctx.r3.u32;
  __imp__sub_821C7908(ctx, base);
  if (!Enabled())
  {
    return;
  }
  const uint32_t id = ctx.r3.u32;
  ++g_ai.peek_ids[id < 16 ? id : 15];
  g_ai.dirty = true;
  const AiTaskLog &log = g_ai_tasks[task];
  REXLOG_INFO("pdz_cover_ai: [{:9.3f}] task {:08X} ({:08X}) peek position {} (point flags {:#x}, variant {})",
              Stamp(Clock::now()), task, log.character, id, log.point ? LoadBe32(base, log.point + kPointFlags) : 0,
              base[task + kTaskPeekVariant]);
}

// sub_821C7268(task, ...): per-frame update.
REX_HOOK_RAW(sub_821C7268)
{
  const uint32_t task = ctx.r3.u32;
  const bool grace_reset = ai_cover_fixes::BeforeUpdate(base, task);
  __imp__sub_821C7268(ctx, base);
  const bool gave_up = ai_cover_fixes::AfterUpdate(base, task);
  if (!Enabled())
  {
    return;
  }
  const auto now = Clock::now();
  AiTaskLog &log = g_ai_tasks[task];
  if (grace_reset)
  {
    REXLOG_INFO("pdz_cover_ai: [{:9.3f}] task {:08X} ({:08X}) grace reset on arrival ({:.0f} ms after created)",
                Stamp(now), task, log.character, Ms(log.created, now));
  }
  if (gave_up)
  {
    REXLOG_INFO("pdz_cover_ai: [{:9.3f}] task {:08X} ({:08X}) snap-back timeout: gave up the point after 3 "
                "snap-backs in a row without peeking",
                Stamp(now), task, log.character);
  }
  const uint32_t sequence = LoadBe32(base, task + kTaskSequence);
  const uint32_t attack = LoadBe32(base, task + kTaskAttack);
  const uint32_t look_out = LoadBe32(base, task + kTaskLookOut);
  const uint32_t state = LoadBe32(base, task + kTaskState);
  if (sequence != log.sequence)
  {
    if (!sequence && !log.has_arrived)
    {
      log.has_arrived = true;
      log.arrived = now;
      ++g_ai.arrived;
      g_ai.characters[log.character]++;
      g_ai.dirty = true;
      REXLOG_INFO("pdz_cover_ai: [{:9.3f}] task {:08X} ({:08X}) in cover {:.0f} ms after created", Stamp(now),
                  task, log.character, Ms(log.created, now));
    }
    else if (sequence && log.has_arrived)
    {
      ++log.snap_backs;
      ++g_ai.snap_backs;
      g_ai.dirty = true;
      REXLOG_INFO("pdz_cover_ai: [{:9.3f}] task {:08X} ({:08X}) snap back #{} ({:.0f} ms in cover)", Stamp(now),
                  task, log.character, log.snap_backs, Ms(log.arrived, now));
    }
  }
  if (attack && !log.attack)
  {
    ++log.attacks;
    ++g_ai.attacks;
    g_ai.dirty = true;
  }
  if (look_out && !log.look_out)
  {
    ++log.look_outs;
    ++g_ai.look_outs;
    g_ai.dirty = true;
    REXLOG_INFO("pdz_cover_ai: [{:9.3f}] task {:08X} ({:08X}) look-out", Stamp(now), task, log.character);
  }
  if (state != log.state)
  {
    REXLOG_INFO("pdz_cover_ai: [{:9.3f}] task {:08X} ({:08X}) state {}->{}", Stamp(now), task, log.character,
                log.state, state);
  }
  log.sequence = sequence;
  log.attack = attack;
  log.look_out = look_out;
  log.state = state;
  PrintAiTotals(now);
}

// sub_821C71B0(task): End.
REX_HOOK_RAW(sub_821C71B0)
{
  const uint32_t task = ctx.r3.u32;
  ai_cover_fixes::OnEnd(task);
  if (Enabled())
  {
    const auto now = Clock::now();
    auto it = g_ai_tasks.find(task);
    if (it != g_ai_tasks.end())
    {
      const AiTaskLog &log = it->second;
      ++g_ai.ended;
      g_ai.dirty = true;
      float position[3];
      ReadPosition(base, log.character, position);
      REXLOG_INFO("pdz_cover_ai: [{:9.3f}] task {:08X} ({:08X}) ended, state {}: {:.0f} ms since created, in "
                  "cover {}, {} attacks, {} look-outs, {} snap-backs, moved {:.0f} cm, point suppressed {}",
                  Stamp(now), task, log.character, LoadBe32(base, task + kTaskState), Ms(log.created, now),
                  log.has_arrived ? fmt::format("{:.0f} ms", Ms(log.arrived, now)) : std::string("never"),
                  log.attacks, log.look_outs, log.snap_backs, Distance2d(log.start_position, position),
                  log.point ? base[log.point + kPointSuppression] : 0);
      if (!log.has_arrived)
      {
        auto &times = g_abandoned[log.character].times;
        times.push_back(now);
        while (!times.empty() && now - times.front() > kHesitationWindow)
        {
          times.erase(times.begin());
        }
        if (times.size() >= kHesitationCount)
        {
          REXLOG_INFO("pdz_cover_ai: [{:9.3f}] {:08X} HESITATING: gave up {} cover moves in {:.1f} s without "
                      "reaching cover",
                      Stamp(now), log.character, times.size(),
                      std::chrono::duration<double>(now - times.front()).count());
        }
      }
      g_ai_tasks.erase(it);
    }
    PrintAiTotals(now);
  }
  __imp__sub_821C71B0(ctx, base);
}

// The other cover task (vtable 820946F4): take cover in place, on the pad the
// character is standing on (character +0xB4 -> task +0x5C); it collects the
// pad's peek points (count +0x74) and refuses (state 2) without any.
// Start sub_821C62F0. It has two ways out, End sub_821C67F0 (sets state 2)
// and Stop sub_821C68D0 (interrupted, e.g. by a new plan); both run the
// cleanup sub_821C6828, which is hooked here.
namespace
{
  std::unordered_map<uint32_t, Clock::time_point> g_in_place_tasks;
  constexpr uint32_t kInPlaceEndReturn = 0x821C6808;
  std::unordered_map<uint32_t, AbandonedLog> g_refusals;
}

REX_HOOK_RAW(sub_821C62F0)
{
  const uint32_t task = ctx.r3.u32;
  __imp__sub_821C62F0(ctx, base);
  if (!Enabled())
  {
    return;
  }
  const auto now = Clock::now();
  const uint32_t state = LoadBe32(base, task + kTaskState);
  const uint32_t pad = LoadBe32(base, task + 0x5C);
  const bool started = state == 1;
  (started ? g_ai.in_place_started : g_ai.in_place_refused)++;
  g_ai.dirty = true;
  if (started)
  {
    g_in_place_tasks[task] = now;
  }
  else
  {
    // The 50/50 "cover in place" choice is refused at once when the enemy
    // isn't on a pad with peek points; repeated refusals = repeated replans.
    auto &times = g_refusals[LoadBe32(base, task + kTaskCharacter)].times;
    times.push_back(now);
    while (!times.empty() && now - times.front() > kHesitationWindow)
    {
      times.erase(times.begin());
    }
    if (times.size() >= kHesitationCount)
    {
      REXLOG_INFO("pdz_cover_ai: [{:9.3f}] {:08X} REPLANNING: {} in-place covers refused in {:.1f} s", Stamp(now),
                  LoadBe32(base, task + kTaskCharacter), times.size(),
                  std::chrono::duration<double>(now - times.front()).count());
    }
  }
  REXLOG_INFO("pdz_cover_ai: [{:9.3f}] in-place task {:08X} ({:08X}) {}: {}, {} peek points", Stamp(now), task,
              LoadBe32(base, task + kTaskCharacter), started ? "started" : "refused", DescribePoint(base, pad),
              LoadBe32(base, task + 0x74));
  PrintAiTotals(now);
}

REX_HOOK_RAW(sub_821C6828)
{
  const uint32_t task = ctx.r3.u32;
  // Called from End at 821C6804 (returns to 821C6808); Stop jumps here.
  const bool from_end = uint32_t(ctx.lr) == kInPlaceEndReturn;
  if (Enabled())
  {
    auto it = g_in_place_tasks.find(task);
    if (it != g_in_place_tasks.end())
    {
      const auto now = Clock::now();
      REXLOG_INFO("pdz_cover_ai: [{:9.3f}] in-place task {:08X} ({:08X}) {}, state {}, after {:.0f} ms",
                  Stamp(now), task, LoadBe32(base, task + kTaskCharacter), from_end ? "ended" : "stopped",
                  LoadBe32(base, task + kTaskState), Ms(it->second, now));
      g_in_place_tasks.erase(it);
    }
  }
  __imp__sub_821C6828(ctx, base);
}
