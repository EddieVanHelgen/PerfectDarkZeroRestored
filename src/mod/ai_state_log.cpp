// pdz_ai_state_log: once a second per enemy (aware, or within 50 m of
// player 1), one "pdz_ai:" line with its setup, flags, alert, target, tasks
// and what its combat planner did since the last line, to see why enemies
// stand around or don't take cover. Read-only: nothing here changes the
// game. See docs/BUG_AUDIT.md AI34-AI45 for the code these counters follow.
//
// - Per enemy tick: sub_8251DF70 (r3 = character), NPC vtable +0x334.
// - Plans: the default planner sub_821B2658 and the cautious one
//   sub_821B1AC0 (r3 = combat task, r4 = character) return a plan or 0; the
//   cover chooser sub_821B20D8 (same) gives up without a target or with
//   fewer than 4 free plan nodes, and builds the close plan within 5.5 m.
//   A plan's kind is read from its first goal's type (plan +0x38 -> +4):
//   7 attack, 2 cover, anything else counted as "other" (close plan).
// - Cover pads: sub_82155EB8 (r3 = pad, r4 = search context, character at
//   +0x24, target position pointer at +0x70) is the pick-time filter (hooked
//   in ai_game_fixes.cpp, which reports here); passing pads less than 10 m
//   from the target fail the per-tick re-check sub_821B3030 (r3 = plan)
//   right after (AI35) unless pdz_fix_ai_cover_min_distance rejects them.
// - Plan ends: sub_821B10C0 (r3 = combat task, r4 = plan) with result +0x20
//   (0 failed, 1 pad reached / re-check failed, 2 no pad for any goal); the
//   fallback sub_821B1128 (r3 = combat task) from 821B0FB4 is the 6 s rule,
//   from 821B110C "no pad".
// - Re-check failures are broken down by the first of sub_82156C50's tests
//   that fails now: no target, target within 5.5 m (sub_821B3030's own
//   test), no player pad, no cover flags, player outside the pad's arc
//   (sub_82166EF8, 30 / 90 deg), under 10 m / over 65 m, else visibility
//   (pad-to-pad, peek pads, side angle); and whether the player's pad has
//   changed since the pick.
// - Route failures are broken down by where sub_8218A990 (and its two
//   copies) gave up: no own pad, destination in another area (+60) for a
//   follower that can't cross areas (+156 == 0), no zone path (+152 = 2),
//   no pad route (+84 null), or the route was rejected (sub_8218AD80).
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iterator>
#include <string>
#include <unordered_map>

#include <fmt/format.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "../game_cvars.h"
#include "ai_state_log.h"

REX_EXTERN(__imp__sub_8251DF70);
REX_EXTERN(__imp__sub_821B2658);
REX_EXTERN(__imp__sub_821B1AC0);
REX_EXTERN(__imp__sub_821B20D8);
REX_EXTERN(__imp__sub_821B3030);
REX_EXTERN(__imp__sub_821B10C0);
REX_EXTERN(__imp__sub_821B1128);
REX_EXTERN(__imp__sub_821CB8E0);
REX_EXTERN(__imp__sub_8218A990);
REX_EXTERN(__imp__sub_8218D088);
REX_EXTERN(__imp__sub_8218F780);
REX_EXTERN(sub_8214C7E8);
REX_EXTERN(sub_825104D8);
REX_EXTERN(sub_8253E380);
REX_EXTERN(sub_82166EF8);

namespace
{
  uint32_t LoadBe32(const uint8_t *base, uint32_t address)
  {
    const uint8_t *p = base + address;
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
  }

  int16_t LoadBe16(const uint8_t *base, uint32_t address)
  {
    return int16_t((base[address] << 8) | base[address + 1]);
  }

  float LoadBeFloat(const uint8_t *base, uint32_t address)
  {
    const uint32_t bits = LoadBe32(base, address);
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
  }

  // Guest heap objects can sit below 0x40000000 too (4 KB page range; player
  // 1 seems to), so only rule out null-ish and the top page.
  bool IsGuestPointer(uint32_t address)
  {
    return address >= 0x00010000u && address < 0xFFFF0000u;
  }

  bool Enabled()
  {
    return REXCVAR_GET(pdz_ai_state_log);
  }

  using Clock = std::chrono::steady_clock;

  constexpr uint32_t kAiClock = 0x82E2E478;
  constexpr uint32_t kPlayerOne = 0x82E3B624;
  constexpr uint32_t kPlayerVtable = 0x820C93A0;
  constexpr uint32_t kPerceptionSlots = 0x82E5C7C8;
  constexpr uint32_t kPerceptionSlotSize = 80;
  constexpr int32_t kPerceptionSlotCount = 170;
  constexpr uint32_t kPlanPool = 0x82E2E4E8;
  constexpr uint32_t kGameSettings = 0x82D6B2B0; // -> +4 difficulty
  constexpr uint64_t kAbilityDodge = 1ull << 41;
  constexpr uint32_t kDodgeDelayReturn = 0x821CB294;

  // Character fields.
  constexpr uint32_t kCharPad = 0xB4;
  constexpr uint32_t kCharTemplate = 0x54;
  constexpr uint32_t kCharPerceptionSlot = 0x354;
  constexpr uint32_t kCharFlags = 0x186C;    // 0x2 aware, 0x80 hurt, 0x100 shot at
  constexpr uint32_t kCharFlagsC = 0x187C;   // 0x800 provoked, 0x1000 no cover
  constexpr uint32_t kCharTaskManager = 0x18D4;
  // vtable +0xE8 on the base NPC class (sub_8251E528): the byte at +0x3D4 of
  // the object at +0x18D0, else +0x186C bit 0x1. When true the combat task
  // skips its fallback (821B10F4) and the alert counter doesn't move.
  constexpr uint32_t kCharController = 0x18D0;
  constexpr uint32_t kControllerFlag = 0x3D4;
  constexpr uint32_t kCharTargetA = 0x3150;  // target handles: pointer at +4,
  constexpr uint32_t kCharTargetB = 0x3168;  // B preferred when set
  constexpr uint32_t kCharLastSeen = 0x30F8; // double, AI clock
  constexpr uint32_t kTemplateBehaviour = 0x5C;

  // Task manager: top layer at +0x528 (minus 0x84), its nodes +0x78..+0x7C.
  constexpr uint32_t kManagerLayer = 0x528;
  constexpr uint32_t kLayerBias = 0x84;
  constexpr uint32_t kLayerFirst = 0x78;
  constexpr uint32_t kLayerEnd = 0x7C;
  constexpr uint32_t kNodeNext = 8;
  constexpr int kTasksMax = 12;

  // Combat task, plans.
  constexpr uint32_t kTaskCharacter = 0x10;
  constexpr uint32_t kPlanCharacter = 0x10;
  constexpr uint32_t kPlanResult = 0x20;
  constexpr uint32_t kPlanFirstGoal = 0x38;
  constexpr uint32_t kGoalType = 4;
  constexpr uint32_t kGoalAttack = 7;
  constexpr uint32_t kGoalCover = 2;

  // Cover pad search context and pads.
  constexpr uint32_t kSearchCharacter = 0x24;
  constexpr uint32_t kSearchTargetEye = 0x70;
  constexpr uint32_t kPadPosition = 0x10;
  constexpr float kRecheckMin2 = 1.0e6f; // 10 m, sub_821B3030

  constexpr uint32_t kFallbackSixSeconds = 0x821B0FB4;
  constexpr uint32_t kFallbackNoPad = 0x821B110C;
  constexpr float kNearPlayer = 5000.0f; // cm

  const char *const kBehaviours[] = {
      "ALL_OUT",  "CAUTIOUS", "HOLD_POS", "WITHDRAW", "RETREAT",   "ANY",
      "SPECIAL",  "SNIPER",   "SNIPER_MOVING", "GOTO_TARGET", "MAI_HEM", "ZONAL",
      "COVER",    "HOLD_ROOM_UNSEEN", "HOLD_ROOM_SEEN", "DODGE", "COVER_JUMP", "POS_AND_SHOOT"};

  // Re-check failure reasons (see the header) and route failure reasons.
  constexpr int kRecheckReasons = 8;
  const char *const kRecheckNames[kRecheckReasons] = {"no-target", "close", "no-player-pad", "flags",
                                                      "arc",       "under-10m", "over-65m", "visibility"};
  constexpr int kRouteReasons = 5;
  const char *const kRouteNames[kRouteReasons] = {"no-own-pad", "other-area", "no-zone-path", "no-pad-route",
                                                  "rejected"};
  constexpr uint32_t kPadFlags = 0x34;
  constexpr float kArcLowCover = 0.523599f; // 30 deg, flag 0x8
  constexpr float kArcSideCover = 1.5708f;  // 90 deg
  constexpr float kRecheckClose2 = 302500.0f;
  constexpr float kRecheckMax2 = 4.225e7f;
  constexpr uint32_t kFollowerOwner = 56;
  constexpr uint32_t kFollowerRoute = 84;
  constexpr uint32_t kFollowerZoneFail = 152;
  constexpr uint32_t kFollowerCrossAreas = 156;
  constexpr uint32_t kPadArea = 60; // s16
  constexpr uint32_t kSearchTargetPad = 0;

  struct Counters
  {
    uint32_t plans_default = 0, plans_cautious = 0;
    uint32_t built_attack = 0, built_cover = 0, built_other = 0, built_none = 0;
    uint32_t chooser_calls = 0, chooser_no_target = 0, chooser_close = 0, chooser_pool = 0;
    uint32_t cover_candidates = 0, cover_passed = 0, cover_passed_inside_10m = 0, cover_rejected_inside_10m = 0;
    uint32_t rechecks_failed = 0;
    uint32_t ended[3] = {};
    uint32_t fallback_six_seconds = 0, fallback_no_pad = 0;
    uint32_t dodge_ready = 0, dodge_relocate = 0, dodge_animate = 0, dodge_clear = 0;
    uint32_t recheck_reason[kRecheckReasons] = {};
    uint32_t recheck_pad_changed = 0;
    uint32_t route_reason[kRouteReasons] = {};
  };

  struct Enemy
  {
    Clock::time_point next_log{};
    Counters counters;
  };

  std::unordered_map<uint32_t, Enemy> g_enemies;

  // Route plans (sub_8218A990 / 8218D088 / 8218F780, one per route follower
  // type) that failed (PathfindError / RouteError, which the game doesn't
  // print), counted for a once-a-second summary line.
  uint32_t g_routes_planned = 0;
  uint32_t g_routes_failed = 0;
  uint32_t g_route_reasons[kRouteReasons] = {};
  Clock::time_point g_next_summary{};

  // The player's pad each enemy's last cover pick used.
  std::unordered_map<uint32_t, uint32_t> g_pick_player_pad;

  Counters *CountersFor(uint32_t character)
  {
    if (!character)
    {
      return nullptr;
    }
    if (g_enemies.size() > 1024)
    {
      g_enemies.clear();
    }
    return &g_enemies[character].counters;
  }

  // A plan's kind from its first goal: 7 attack, 2 cover, else other.
  int PlanKind(const uint8_t *base, uint32_t plan)
  {
    if (!IsGuestPointer(plan))
    {
      return -1;
    }
    const uint32_t goal = LoadBe32(base, plan + kPlanFirstGoal);
    if (!IsGuestPointer(goal))
    {
      return 0;
    }
    const uint32_t type = LoadBe32(base, goal + kGoalType);
    return type == kGoalAttack ? 1 : type == kGoalCover ? 2 : 0;
  }

  // One route plan (sub_8218A990 and copies, r3 = route follower, r4 =
  // destination pad): count it and, when it fails, where it gave up.
  void RoutePlan(PPCContext &ctx, uint8_t *base, void (*plan)(PPCContext &, uint8_t *))
  {
    const uint32_t follower = ctx.r3.u32;
    const uint32_t destination = ctx.r4.u32;
    const bool log = Enabled() && IsGuestPointer(follower);
    const uint32_t owner = log ? LoadBe32(base, follower + kFollowerOwner) : 0;
    const uint32_t own_pad = IsGuestPointer(owner) ? LoadBe32(base, owner + kCharPad) : 0;
    plan(ctx, base);
    if (!log)
    {
      return;
    }
    ++g_routes_planned;
    if ((ctx.r3.u32 & 0xFF) != 0)
    {
      return;
    }
    ++g_routes_failed;
    int reason = 4;
    if (!IsGuestPointer(own_pad))
    {
      reason = 0;
    }
    else if (IsGuestPointer(destination) && LoadBe16(base, destination + kPadArea) != LoadBe16(base, own_pad + kPadArea) &&
             LoadBe32(base, follower + kFollowerCrossAreas) == 0)
    {
      reason = 1;
    }
    else if (LoadBe32(base, follower + kFollowerZoneFail) == 2)
    {
      reason = 2;
    }
    else if (LoadBe32(base, follower + kFollowerRoute) == 0)
    {
      reason = 3;
    }
    ++g_route_reasons[reason];
    if (Counters *c = IsGuestPointer(owner) ? CountersFor(owner) : nullptr)
    {
      ++c->route_reason[reason];
    }
  }

  void CountBuilt(Counters &c, int kind)
  {
    switch (kind)
    {
    case -1: ++c.built_none; break;
    case 1: ++c.built_attack; break;
    case 2: ++c.built_cover; break;
    default: ++c.built_other; break;
    }
  }

  // A task's id: its vtable +4 is `li r3,N; blr`.
  int TaskId(const uint8_t *base, uint32_t task)
  {
    const uint32_t vtable = LoadBe32(base, task);
    if (!IsGuestPointer(vtable))
    {
      return -1;
    }
    const uint32_t function = LoadBe32(base, vtable + 4);
    if (!IsGuestPointer(function))
    {
      return -1;
    }
    const uint32_t first = LoadBe32(base, function);
    return (first >> 16) == 0x3860 && LoadBe32(base, function + 4) == 0x4E800020 ? int(int16_t(first & 0xFFFF)) : -1;
  }

  std::string TaskName(int id)
  {
    switch (id)
    {
    case 4: return "hunt";
    case 6: return "combat";
    case 9: return "dodge";
    case 28: return "engage";
    case 39: return "idle";
    case 49: return "nocover";
    case 59: return "shoot";
    default: return fmt::format("{}", id);
    }
  }

  std::string Tasks(const uint8_t *base, uint32_t character)
  {
    const uint32_t layer_pointer = LoadBe32(base, character + kCharTaskManager + kManagerLayer);
    if (!IsGuestPointer(layer_pointer))
    {
      return "-";
    }
    const uint32_t layer = layer_pointer - kLayerBias;
    const uint32_t end = LoadBe32(base, layer + kLayerEnd);
    uint32_t node = LoadBe32(base, layer + kLayerFirst);
    std::string text;
    for (int i = 0; i < kTasksMax && IsGuestPointer(node) && node != end; ++i)
    {
      const uint32_t task = LoadBe32(base, node);
      if (IsGuestPointer(task))
      {
        text += (text.empty() ? "" : ",") + TaskName(TaskId(base, task));
      }
      node = LoadBe32(base, node + kNodeNext);
    }
    return text.empty() ? "-" : text;
  }

  // The game's position getter: the mover's position, or +0x30 without one.
  uint32_t PositionAddress(PPCContext &ctx, uint8_t *base, uint32_t character)
  {
    rex::CallFrame frame(ctx);
    frame.ctx.r3.u64 = character;
    sub_8214C7E8(frame.ctx, base);
    return frame.ctx.r3.u32;
  }

  float Distance2(const uint8_t *base, uint32_t a, uint32_t b)
  {
    float d2 = 0.0f;
    for (int i = 0; i < 3; ++i)
    {
      const float d = LoadBeFloat(base, a + 4 * i) - LoadBeFloat(base, b + 4 * i);
      d2 += d * d;
    }
    return d2;
  }

  bool Position(PPCContext &ctx, uint8_t *base, uint32_t character, float out[3])
  {
    const uint32_t address = PositionAddress(ctx, base, character);
    if (!IsGuestPointer(address))
    {
      return false;
    }
    for (int i = 0; i < 3; ++i)
    {
      out[i] = LoadBeFloat(base, address + 4 * i);
    }
    return true;
  }

  int Held(const uint8_t *base, uint32_t character, uint32_t flags)
  {
    const uint32_t controller = LoadBe32(base, character + kCharController);
    return IsGuestPointer(controller) ? base[controller + kControllerFlag] : int(flags & 1);
  }

  // sub_825104D8(a, b): non-zero when b is on a's side (the crowd rule
  // counts the characters it returns 0 for as hostile).
  bool Friendly(PPCContext &ctx, uint8_t *base, uint32_t player, uint32_t character)
  {
    rex::CallFrame frame(ctx);
    frame.ctx.r3.u64 = player;
    frame.ctx.r4.u64 = character;
    sub_825104D8(frame.ctx, base);
    return (frame.ctx.r3.u32 & 0xFF) != 0;
  }

  // sub_8253E380(character, mask): the ability isn't locked.
  bool CanDodge(PPCContext &ctx, uint8_t *base, uint32_t character)
  {
    rex::CallFrame frame(ctx);
    frame.ctx.r3.u64 = character;
    frame.ctx.r4.u64 = kAbilityDodge;
    sub_8253E380(frame.ctx, base);
    return (frame.ctx.r3.u32 & 0xFF) != 0;
  }

  uint32_t Target(const uint8_t *base, uint32_t character)
  {
    const uint32_t b = LoadBe32(base, character + kCharTargetB + 4);
    return b ? b : LoadBe32(base, character + kCharTargetA + 4);
  }

  // Which of the re-check's tests (sub_821B3030 -> sub_82156C50) fails now
  // for this enemy and cover pad; see kRecheckNames.
  int RecheckReason(PPCContext &ctx, uint8_t *base, uint32_t character, uint32_t pad)
  {
    const uint32_t target = Target(base, character);
    if (!IsGuestPointer(target))
    {
      return 0;
    }
    const uint32_t here = PositionAddress(ctx, base, character);
    const uint32_t there = PositionAddress(ctx, base, target);
    if (!IsGuestPointer(here) || !IsGuestPointer(there) || Distance2(base, here, there) <= kRecheckClose2)
    {
      return 1;
    }
    if (!IsGuestPointer(LoadBe32(base, target + kCharPad)))
    {
      return 2;
    }
    if (!IsGuestPointer(pad))
    {
      return 3;
    }
    const uint32_t flags = LoadBe32(base, pad + kPadFlags);
    if ((flags & 0xC) == 0)
    {
      return 3;
    }
    {
      rex::CallFrame frame(ctx);
      frame.ctx.r3.u64 = pad;
      frame.ctx.r4.u64 = there;
      frame.ctx.f1.f64 = (flags & 0x8) ? kArcLowCover : kArcSideCover;
      sub_82166EF8(frame.ctx, base);
      if ((frame.ctx.r3.u32 & 0xFF) == 0)
      {
        return 4;
      }
    }
    const float d2 = Distance2(base, pad + kPadPosition, there);
    if (d2 <= kRecheckMin2)
    {
      return 5;
    }
    if (d2 >= kRecheckMax2)
    {
      return 6;
    }
    return 7;
  }

  std::string Breakdown(const uint32_t *counts, const char *const *names, int n)
  {
    std::string text;
    for (int i = 0; i < n; ++i)
    {
      if (counts[i])
      {
        text += fmt::format("{}{} {}", text.empty() ? "" : ", ", names[i], counts[i]);
      }
    }
    return text.empty() ? "-" : text;
  }

  void LogEnemy(PPCContext &ctx, uint8_t *base, uint32_t character, Enemy &enemy)
  {
    const uint32_t player = LoadBe32(base, kPlayerOne);
    const uint32_t flags = LoadBe32(base, character + kCharFlags);
    const uint32_t flags_c = LoadBe32(base, character + kCharFlagsC);
    const bool aware = (flags & 0x2) != 0;
    float here[3], there[3];
    float distance = -1.0f;
    if (IsGuestPointer(player) && Position(ctx, base, character, here) && Position(ctx, base, player, there))
    {
      distance = std::sqrt((here[0] - there[0]) * (here[0] - there[0]) + (here[1] - there[1]) * (here[1] - there[1]) +
                           (here[2] - there[2]) * (here[2] - there[2]));
    }
    Counters &c = enemy.counters;
    if (!aware && (distance < 0.0f || distance > kNearPlayer))
    {
      c = Counters{};
      return;
    }
    const uint32_t setup = LoadBe32(base, character + kCharTemplate);
    const int behaviour = IsGuestPointer(setup) ? int(int8_t(base[setup + kTemplateBehaviour])) : -1;
    const char *behaviour_name =
        behaviour >= 0 && behaviour < int(std::size(kBehaviours)) ? kBehaviours[behaviour] : "?";
    const int32_t slot = int32_t(LoadBe32(base, character + kCharPerceptionSlot));
    const int alert = slot >= 0 && slot < kPerceptionSlotCount
                          ? LoadBe16(base, kPerceptionSlots + uint32_t(slot) * kPerceptionSlotSize + 12)
                          : -1;
    const uint32_t target = Target(base, character);
    const float now = LoadBeFloat(base, kAiClock);
    double last_seen;
    {
      const uint64_t bits = (uint64_t(LoadBe32(base, character + kCharLastSeen)) << 32) |
                            LoadBe32(base, character + kCharLastSeen + 4);
      std::memcpy(&last_seen, &bits, sizeof(last_seen));
    }
    const uint32_t pool = LoadBe32(base, kPlanPool);
    const int free_nodes = IsGuestPointer(pool) ? int(LoadBe32(base, pool + 0xC) - LoadBe32(base, pool + 0x10)) : -1;
    REXLOG_INFO(
        "pdz_ai: {:08X} {} {} (class {:08X}, held {}, difficulty {}) | aware {} hurt {} shot-at {} provoked {} no-cover {} | alert {} | target {} {:.1f} m, "
        "seen {:.1f} s ago | pads own {} player {} ({:08X}) | tasks {} | plans default {} cautious {} -> attack {} cover {} "
        "other {} none {} | chooser {} (no target {}, close {}, pool {}; free nodes {}) | cover pads {} passed {} "
        "(< 10 m {}, pick under 10 m rejected {}) re-check fails {} [{}; player pad changed {}] | route fails [{}] | ended fail {} reached {} no-pad {} | fallback 6s {} no-pad {} | dodge "
        "ability {} ready {} relocate {} animate {} clear {}",
        character, IsGuestPointer(player) && Friendly(ctx, base, player, character) ? "ally" : "enemy",
        behaviour_name, LoadBe32(base, character), Held(base, character, flags),
        IsGuestPointer(LoadBe32(base, kGameSettings)) ? int32_t(LoadBe32(base, LoadBe32(base, kGameSettings) + 4)) : -1,
        aware ? 1 : 0, (flags & 0x80) ? 1 : 0, (flags & 0x100) ? 1 : 0,
        (flags_c & 0x800) ? 1 : 0, (flags_c & 0x1000) ? 1 : 0, alert,
        target == 0 ? "none" : target == player ? "player" : fmt::format("{:08X}", target), distance / 100.0f,
        now - last_seen, LoadBe32(base, character + kCharPad) ? "ok" : "null",
        IsGuestPointer(player) && LoadBe32(base, player + kCharPad) ? "ok" : "null", player, Tasks(base, character),
        c.plans_default, c.plans_cautious, c.built_attack, c.built_cover, c.built_other, c.built_none,
        c.chooser_calls, c.chooser_no_target, c.chooser_close, c.chooser_pool, free_nodes, c.cover_candidates,
        c.cover_passed, c.cover_passed_inside_10m, c.cover_rejected_inside_10m, c.rechecks_failed,
        Breakdown(c.recheck_reason, kRecheckNames, kRecheckReasons), c.recheck_pad_changed,
        Breakdown(c.route_reason, kRouteNames, kRouteReasons), c.ended[0], c.ended[1], c.ended[2],
        c.fallback_six_seconds, c.fallback_no_pad, CanDodge(ctx, base, character) ? 1 : 0, c.dodge_ready,
        c.dodge_relocate, c.dodge_animate, c.dodge_clear);
    c = Counters{};
  }
} // namespace

namespace ai_state_log
{
  void OnDodge(uint32_t character, Dodge what)
  {
    if (Counters *c = Enabled() ? CountersFor(character) : nullptr)
    {
      switch (what)
      {
      case Dodge::Relocate: ++c->dodge_relocate; break;
      case Dodge::Animate: ++c->dodge_animate; break;
      case Dodge::AnimateClear: ++c->dodge_clear; break;
      }
    }
  }
} // namespace ai_state_log

// sub_821CB8E0(dodge task): the dodge delay for this difficulty, asked once
// the dodge task's ability and idle checks have passed (from 821CB290).
REX_HOOK_RAW(sub_821CB8E0)
{
  const uint32_t task = ctx.r3.u32;
  __imp__sub_821CB8E0(ctx, base);
  if (Enabled() && uint32_t(ctx.lr) == kDodgeDelayReturn && IsGuestPointer(task))
  {
    if (Counters *c = CountersFor(LoadBe32(base, task + kTaskCharacter)))
    {
      ++c->dodge_ready;
    }
  }
}

// sub_8251DF70(character, f1 = dt): an NPC's update.
REX_HOOK_RAW(sub_8251DF70)
{
  const uint32_t character = ctx.r3.u32;
  __imp__sub_8251DF70(ctx, base);
  if (!Enabled() || !IsGuestPointer(character) || LoadBe32(base, character) == kPlayerVtable)
  {
    return;
  }
  Enemy &enemy = g_enemies[character];
  const auto now = Clock::now();
  if (now >= g_next_summary)
  {
    g_next_summary = now + std::chrono::seconds(1);
    REXLOG_INFO("pdz_ai: routes planned {} failed {} [{}] this second", g_routes_planned, g_routes_failed,
                Breakdown(g_route_reasons, kRouteNames, kRouteReasons));
    g_routes_planned = 0;
    g_routes_failed = 0;
    std::fill(std::begin(g_route_reasons), std::end(g_route_reasons), 0u);
  }
  if (now < enemy.next_log)
  {
    return;
  }
  enemy.next_log = now + std::chrono::seconds(1);
  LogEnemy(ctx, base, character, enemy);
}

// sub_821B2658(combat task, character, flags): the default plan.
REX_HOOK_RAW(sub_821B2658)
{
  const uint32_t character = ctx.r4.u32;
  __imp__sub_821B2658(ctx, base);
  if (Counters *c = Enabled() ? CountersFor(character) : nullptr)
  {
    ++c->plans_default;
    CountBuilt(*c, ctx.r3.u32 ? PlanKind(base, ctx.r3.u32) : -1);
  }
}

// sub_821B1AC0(combat task, character, flags): the cautious plan.
REX_HOOK_RAW(sub_821B1AC0)
{
  const uint32_t character = ctx.r4.u32;
  __imp__sub_821B1AC0(ctx, base);
  if (Counters *c = Enabled() ? CountersFor(character) : nullptr)
  {
    ++c->plans_cautious;
    CountBuilt(*c, ctx.r3.u32 ? PlanKind(base, ctx.r3.u32) : -1);
  }
}

// sub_821B20D8(combat task, character, flags, 2): the cover chooser.
REX_HOOK_RAW(sub_821B20D8)
{
  const uint32_t character = ctx.r4.u32;
  Counters *c = Enabled() ? CountersFor(character) : nullptr;
  const bool has_target = c && Target(base, character) != 0;
  const uint32_t pool = LoadBe32(base, kPlanPool);
  const bool pool_short = IsGuestPointer(pool) && int(LoadBe32(base, pool + 0xC) - LoadBe32(base, pool + 0x10)) < 4;
  __imp__sub_821B20D8(ctx, base);
  if (!c)
  {
    return;
  }
  ++c->chooser_calls;
  if (!has_target)
  {
    ++c->chooser_no_target;
  }
  else if (ctx.r3.u32 && PlanKind(base, ctx.r3.u32) != 2)
  {
    ++c->chooser_close;
  }
  else if (!ctx.r3.u32 && pool_short)
  {
    ++c->chooser_pool;
  }
}

namespace ai_state_log
{
  // Called from the cover pad filter hook in ai_game_fixes.cpp.
  void OnCoverPick(const uint8_t *base, uint32_t search, uint32_t pad, bool passed, bool rejected)
  {
    if (!Enabled() || !IsGuestPointer(search))
    {
      return;
    }
    Counters *c = CountersFor(LoadBe32(base, search + kSearchCharacter));
    if (!c)
    {
      return;
    }
    ++c->cover_candidates;
    c->cover_rejected_inside_10m += rejected;
    if (!passed)
    {
      return;
    }
    ++c->cover_passed;
    g_pick_player_pad[LoadBe32(base, search + kSearchCharacter)] = LoadBe32(base, search + kSearchTargetPad);
    const uint32_t eye = LoadBe32(base, search + kSearchTargetEye);
    if (IsGuestPointer(pad) && IsGuestPointer(eye) && Distance2(base, pad + kPadPosition, eye) <= kRecheckMin2)
    {
      ++c->cover_passed_inside_10m;
    }
  }
} // namespace ai_state_log

// sub_821B3030(plan, goal pad info): the cover goal's per-tick re-check.
REX_HOOK_RAW(sub_821B3030)
{
  const uint32_t plan = ctx.r3.u32;
  const uint32_t goal_pad = ctx.r4.u32;
  __imp__sub_821B3030(ctx, base);
  if (!Enabled() || !IsGuestPointer(plan) || (ctx.r3.u32 & 0xFF) != 0)
  {
    return;
  }
  const uint32_t character = LoadBe32(base, plan + kPlanCharacter);
  Counters *c = CountersFor(character);
  if (!c)
  {
    return;
  }
  ++c->rechecks_failed;
  const uint32_t pad = IsGuestPointer(goal_pad) ? LoadBe32(base, goal_pad) : 0;
  ++c->recheck_reason[RecheckReason(ctx, base, character, pad)];
  const uint32_t target = Target(base, character);
  const auto picked = g_pick_player_pad.find(character);
  if (IsGuestPointer(target) && picked != g_pick_player_pad.end() &&
      LoadBe32(base, target + kCharPad) != picked->second)
  {
    ++c->recheck_pad_changed;
  }
}

// sub_821B10C0(combat task, plan): a plan has ended.
REX_HOOK_RAW(sub_821B10C0)
{
  const uint32_t task = ctx.r3.u32;
  const uint32_t plan = ctx.r4.u32;
  if (Enabled() && IsGuestPointer(task) && IsGuestPointer(plan))
  {
    if (Counters *c = CountersFor(LoadBe32(base, task + kTaskCharacter)))
    {
      const uint32_t result = LoadBe32(base, plan + kPlanResult);
      ++c->ended[result < 3 ? result : 0];
    }
  }
  __imp__sub_821B10C0(ctx, base);
}

// sub_821B1128(combat task): the behaviour's fallback when a plan gives up.
REX_HOOK_RAW(sub_821B1128)
{
  const uint32_t task = ctx.r3.u32;
  const uint32_t lr = uint32_t(ctx.lr);
  if (Enabled() && IsGuestPointer(task))
  {
    if (Counters *c = CountersFor(LoadBe32(base, task + kTaskCharacter)))
    {
      c->fallback_six_seconds += lr == kFallbackSixSeconds;
      c->fallback_no_pad += lr == kFallbackNoPad;
    }
  }
  __imp__sub_821B1128(ctx, base);
}

// Route planning for the three route follower types: false = no route.
REX_HOOK_RAW(sub_8218A990)
{
  RoutePlan(ctx, base, __imp__sub_8218A990);
}

REX_HOOK_RAW(sub_8218D088)
{
  RoutePlan(ctx, base, __imp__sub_8218D088);
}

REX_HOOK_RAW(sub_8218F780)
{
  RoutePlan(ctx, base, __imp__sub_8218F780);
}
