// Unused AI behaviours, debug only (not a fix). tu3-base only; TU3 addresses,
// launch addresses in brackets. See docs/SP_INVENTORY.md, "Unused AI
// behaviours in depth".
//
// pdz_debug_ai_behaviour (off = "none") forces a combat behaviour on every
// eligible single-player enemy when its fight starts. A fight is a combat
// task: its Start (sub_821B5A68 [821B0D98]) reads the requested behaviour at
// task +108, and when that is 18 (BEHAVIOUR_MAX, "use the setup's") copies the
// setup's byte (template +92) before mapping it to a plan record
// (sub_821B5780 [821B0AB0]). We write the forced value into task +108 first,
// so the setup data is never changed and the next fight picks up a changed
// setting. Fights whose behaviour a level script chose (Datacore's scientists
// get POSITION_AND_SHOOT, 17) are left alone.
// - cover / dodge: behaviours 12 / 15, which use ALL_OUT_ATTACK's record;
// - cover_jump: 16, which uses CAUTIOUS_ATTACK's record;
// - position_and_shoot: 17 (close plan only: never cover, never charges);
// - cover_regardless: 16 (so the cover chooser always runs beyond 5.5 m) and
//   the planner's two cover actions swapped for their unused "regardless"
//   twins: move-to-cover sub_821B75C8 -> sub_821B7700 [821B28F8 -> 821B2A30]
//   (only on pads with cover flags 0xC, then the cover task with
//   regardless = 1) and cover-in-place sub_821B7840 -> sub_821B78D8
//   [821B2B70 -> 821B2C08] (a coin flip: move-to-cover regardless on a pad
//   without flag 0x4, otherwise in place). "Regardless" makes the cover task
//   skip its own tests (line of sight / arc timers, the low-cover angle); the
//   plan's per-tick re-check of the pad still runs;
// - all_out_attack / cautious_attack / hold_position: the shipped baselines.
// Eligible: single player or co-op only; not player 1 or anyone who counts
// player 1 as a friend (sub_8251EBD0 [825104D8]); the setup's behaviour isn't
// MAI_HEM, SPECIAL, SNIPER or SNIPER_MOVING; all three setup scripts (template +172 / +173 / +174:
// first, alerted, return) are ordinary ones (no boss, Jack Dark, civilian,
// club, scientist, follow or bot script).
//
// pdz_ai_behaviour_log: once a second per enemy in a fight (from the combat
// task's Update sub_821B5B20 [821B0E50]) one pdz_ai_behaviour: line with the
// behaviour, the current plan's goal types (plan at task +92, goals at plan
// +56.., count +76; 2 / 3 cover, 0 / 7 attack, 4 / 5 / 6 / 10 close), whether
// a cover task runs (character +6268 0x8000), the planner cover actions taken
// (and how many were "regardless"), animated dodges (AnimType 0x8C-0x8F,
// 0x92-0x9D picked by sub_821F00C8; the 2-in-3 dodges that relocate instead
// aren't counted), the hurt / shot-at latches (+6252 0x80 / 0x100), provoked
// (+6268 0x800), the level's no-cover flag (+6268 0x1000) and the distance to
// player 1, plus the move-into-cover tasks it has ended and its seconds settled
// in cover. Each move-into-cover task (vtable 82092950, hooks in
// ai_cover_fixes.cpp plus its Stop sub_821CE990 [821C78D0] here) also gets a
// line when it ends: regardless or not, total time, whether it reached its
// cover point and for how long, peeks, and why it ended (stopped by its owner,
// i.e. the plan's re-check or the plan ending; or its own cover tests).
#include "ai_behaviour_debug.h"

#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/ppc/func.h>
#include <rex/logging.h>

#include "../game_cvars.h"
#include "aim_assist.h"
#include "hook_profile.h" // pdz_hook_profile (must follow <rex/hook.h>)

REX_EXTERN(__imp__sub_821B5A68);
REX_EXTERN(__imp__sub_821B5B20);
REX_EXTERN(__imp__sub_821B75C8);
REX_EXTERN(__imp__sub_821B7840);
REX_EXTERN(sub_821B7700);
REX_EXTERN(sub_821B78D8);
REX_EXTERN(sub_8251EBD0);
REX_EXTERN(sub_82518280);
REX_EXTERN(__imp__sub_821B5DF8);
REX_EXTERN(__imp__sub_8223FC00);
REX_EXTERN(sub_821B7520);
REX_EXTERN(sub_821E0DC0);

namespace
{
  using Clock = std::chrono::steady_clock;

  constexpr uint32_t kGameInfo = 0x82D6BFD8; // -> +40 game type (0 single player, 1 co-op)
  constexpr uint32_t kPlayerOne = 0x82E3C3E4; // player 1's character [82E3B624]

  constexpr uint32_t kCharTemplate = 84;  // character: setup template
  constexpr uint32_t kCharLatches = 6252; // 0x80 hurt, 0x100 shot at
  constexpr uint32_t kCharFlags = 6268;   // 0x800 provoked, 0x1000 no cover, 0x8000 in a cover task
  constexpr uint32_t kTemplateBehaviour = 92;
  constexpr uint32_t kTemplateScripts = 172; // three bytes: first, alerted, return script
  constexpr uint32_t kTaskCharacter = 16;
  constexpr uint32_t kTaskPlan = 92;
  constexpr uint32_t kTaskBehaviour = 108;
  constexpr uint32_t kPlanGoals = 56;
  constexpr uint32_t kPlanGoalCount = 76;
  constexpr uint32_t kGoalType = 4;

  constexpr uint8_t kUseTemplate = 18;
  constexpr uint8_t kSpecial = 6;
  constexpr uint8_t kSniper = 7;       // placed to stay put (log 065: forcing them made
  constexpr uint8_t kSniperMoving = 8; // the bridge snipers cover-seekers 100+ m away)
  constexpr uint8_t kMaiHem = 10;
  constexpr uint8_t kCoverJump = 16;

  constexpr std::array<const char *, 19> kBehaviours = {
      "ALL_OUT_ATTACK", "CAUTIOUS_ATTACK", "HOLD_POSITION", "WITHDRAW", "RETREAT",
      "ANY", "SPECIAL", "SNIPER", "SNIPER_MOVING", "GOTO_TARGET_ATTACK",
      "MAI_HEM", "ZONAL_ATTACK", "COVER", "HOLD_ROOM_FROM_UNSEEN_ENEMY", "HOLD_ROOM_FROM_SEEN_ENEMY",
      "DODGE", "COVER_JUMP", "POSITION_AND_SHOOT", "BEHAVIOUR_MAX"};

  struct Mode
  {
    const char *name;
    int behaviour; // -1: off
    bool regardless;
  };

  constexpr std::array<Mode, 9> kModes = {{
      {"none", -1, false},
      {"cover", 12, false},
      {"cover_jump", 16, false},
      {"dodge", 15, false},
      {"position_and_shoot", 17, false},
      {"cover_regardless", kCoverJump, true},
      {"all_out_attack", 0, false},
      {"cautious_attack", 1, false},
      {"hold_position", 2, false},
  }};

  struct Enemy
  {
    uint32_t vtable = 0;
    bool forced = false;
    bool regardless = false;
    uint8_t setup_behaviour = kUseTemplate;
    uint32_t covers = 0;
    uint32_t regardless_covers = 0;
    uint32_t dodges = 0;
    uint32_t cover_tasks_ended = 0; // move-into-cover tasks
    uint32_t cover_tasks_settled = 0;
    double cover_seconds = 0.0; // settled at the cover point
    uint32_t samples = 0;       // per-second lines
    uint32_t planless = 0;      // ... with no plan
    uint32_t shots = 0;         // shots fired (sub_8223FC00)
    uint32_t shoot_from_here = 0; // do-nothing waits replaced (pdz_ai_shoot_from_here)
    Clock::time_point last_line{};
    Clock::time_point last_seen{};
  };

  struct CoverTask
  {
    Clock::time_point created{};
    Clock::time_point settled{};
    bool is_settled = false;
    bool regardless = false;
    bool stopped = false; // Stop (from its owner: plan re-check, plan end) before End
    uint32_t peeks = 0;
  };

  std::mutex g_mutex;
  std::unordered_map<uint32_t, Enemy> g_enemies; // by character
  std::unordered_map<uint32_t, CoverTask> g_cover_tasks; // by task
  std::unordered_map<uint32_t, bool> g_script_fights;      // combat task -> its behaviour was chosen by a script
  std::string g_announced_mode;

  constexpr uint32_t kCoverRegardless = 0x58; // move-into-cover task: "regardless" byte
  constexpr uint32_t kTaskStateField = 0x0C;  // 1 running, 2 done (its own tests failed / gave up)

  uint32_t Load32(const uint8_t *base, uint32_t address)
  {
    const uint8_t *p = base + address;
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
  }

  bool IsGuestPointer(uint32_t address)
  {
    return address >= 0x00010000u && address < 0xFFFF0000u;
  }

  const char *BehaviourName(int value)
  {
    return value >= 0 && value < int(kBehaviours.size()) ? kBehaviours[value] : "?";
  }

  Mode CurrentMode()
  {
    std::string text = REXCVAR_GET(pdz_debug_ai_behaviour);
    for (char &c : text)
    {
      if (c >= 'A' && c <= 'Z')
      {
        c = char(c - 'A' + 'a');
      }
    }
    for (const Mode &mode : kModes)
    {
      if (text == mode.name)
      {
        return mode;
      }
    }
    return kModes[0];
  }

  bool OrdinaryScript(uint8_t script)
  {
    switch (script)
    {
    case 0:  // UNSET
    case 1:  // DEFAULT
    case 2:  // SEARCH
    case 3:  // COMBAT
    case 4:  // IDLE
    case 6:  // RADIO
    case 13: // TIPTABLE
    case 16: // OBJECT_COVER
    case 17: // OBJECT_COVER_REGARDLESS
    case 18: // TIPTABLE_REGARDLESS
    case 19: // SQUAD_HOLD
    case 24: // COMBAT_SCRIPT_RETURN_TO_INITIAL_NO_HIT_ANIMS
    case 25: // COMBAT_SCRIPT_RETURN_TO_INITIAL
    case 39: // KICK_DOOR
      return true;
    default:
      return false;
    }
  }

  // Our own frame below the hook's stack pointer.
  void UseOwnStack(rex::CallFrame &frame, const PPCContext &ctx)
  {
    frame.ctx.r1.u64 = (ctx.r1.u32 - 0x200) & ~0xFu;
  }

  bool SinglePlayer(const uint8_t *base)
  {
    const uint32_t info = Load32(base, kGameInfo);
    return IsGuestPointer(info) && Load32(base, info + 40) <= 1;
  }

  bool EligibleEnemy(PPCContext &ctx, uint8_t *base, uint32_t character)
  {
    if (!IsGuestPointer(character) || !SinglePlayer(base))
    {
      return false;
    }
    const uint32_t setup = Load32(base, character + kCharTemplate);
    if (!IsGuestPointer(setup))
    {
      return false;
    }
    const uint8_t behaviour = base[setup + kTemplateBehaviour];
    if (behaviour == kMaiHem || behaviour == kSpecial || behaviour == kSniper || behaviour == kSniperMoving)
    {
      return false;
    }
    for (uint32_t i = 0; i < 3; ++i)
    {
      if (!OrdinaryScript(base[setup + kTemplateScripts + i]))
      {
        return false;
      }
    }
    const uint32_t player = Load32(base, kPlayerOne);
    if (!IsGuestPointer(player) || player == character)
    {
      return false;
    }
    rex::CallFrame frame(ctx);
    UseOwnStack(frame, ctx);
    frame.ctx.r3.u64 = character;
    frame.ctx.r4.u64 = player;
    sub_8251EBD0(frame.ctx, base); // does the character count player 1 as a friend?
    return (frame.ctx.r3.u32 & 0xFF) == 0;
  }

  // Under g_mutex. The entry for a live character (dropped if the slot now
  // holds another object).
  Enemy *Find(const uint8_t *base, uint32_t character)
  {
    auto it = g_enemies.find(character);
    if (it == g_enemies.end())
    {
      return nullptr;
    }
    if (it->second.vtable != Load32(base, character))
    {
      g_enemies.erase(it);
      return nullptr;
    }
    return &it->second;
  }

  bool Regardless(const uint8_t *base, uint32_t character)
  {
    if (!CurrentMode().regardless)
    {
      return false;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    const Enemy *enemy = Find(base, character);
    return enemy && enemy->regardless;
  }

  void CountCover(const uint8_t *base, uint32_t character, bool regardless)
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (Enemy *enemy = Find(base, character))
    {
      ++enemy->covers;
      enemy->regardless_covers += regardless ? 1 : 0;
    }
  }

  std::string PlanText(const uint8_t *base, uint32_t task)
  {
    const uint32_t plan = Load32(base, task + kTaskPlan);
    if (!IsGuestPointer(plan))
    {
      return "none";
    }
    const uint32_t count = Load32(base, plan + kPlanGoalCount);
    if (count == 0 || count > 4)
    {
      return "none";
    }
    std::string types;
    bool cover = false, attack = false, close = false;
    for (uint32_t i = 0; i < count; ++i)
    {
      const uint32_t goal = Load32(base, plan + kPlanGoals + 4 * i);
      const uint32_t type = IsGuestPointer(goal) ? Load32(base, goal + kGoalType) : 0xFFu;
      types += fmt::format("{}{}", i ? "," : "", type);
      cover |= type == 2 || type == 3;
      attack |= type == 0 || type == 7;
      close |= type == 4 || type == 5 || type == 6;
    }
    const char *kind = cover ? "cover" : attack ? "attack" : close ? "close" : "other";
    return fmt::format("{} [{}]", kind, types);
  }

  void LogLine(PPCContext &ctx, uint8_t *base, uint32_t task, uint32_t character)
  {
    const auto now = Clock::now();
    Enemy snapshot;
    {
      std::lock_guard<std::mutex> lock(g_mutex);
      Enemy *enemy = Find(base, character);
      if (!enemy)
      {
        Enemy fresh;
        fresh.vtable = Load32(base, character);
        enemy = &g_enemies.emplace(character, fresh).first->second;
      }
      enemy->last_seen = now;
      if (now - enemy->last_line < std::chrono::seconds(1))
      {
        return;
      }
      enemy->last_line = now;
      ++enemy->samples;
      enemy->planless += IsGuestPointer(Load32(base, task + kTaskPlan)) ? 0 : 1;
      snapshot = *enemy;
      // Forget characters not seen for a while (dead or out of combat).
      for (auto it = g_enemies.begin(); it != g_enemies.end();)
      {
        it = now - it->second.last_seen > std::chrono::seconds(30) ? g_enemies.erase(it) : std::next(it);
      }
    }
    const int behaviour = int8_t(base[task + kTaskBehaviour]);
    const uint32_t latches = Load32(base, character + kCharLatches);
    const uint32_t flags = Load32(base, character + kCharFlags);
    float metres = -1.0f;
    const uint32_t player = Load32(base, kPlayerOne);
    if (IsGuestPointer(player) && player != character)
    {
      rex::CallFrame frame(ctx);
      UseOwnStack(frame, ctx);
      frame.ctx.r3.u64 = character;
      frame.ctx.r4.u64 = player;
      sub_82518280(frame.ctx, base); // squared distance, cm^2
      const double d2 = frame.ctx.f1.f64;
      metres = std::isfinite(d2) && d2 >= 0.0 ? float(std::sqrt(d2) / 100.0) : -1.0f;
    }
    const std::string setup = snapshot.forced
                                  ? fmt::format(" (forced{}, setup {})", snapshot.regardless ? ", regardless" : "",
                                                BehaviourName(snapshot.setup_behaviour))
                                  : std::string();
    REXLOG_INFO("pdz_ai_behaviour: {:08X} {}{} plan {} in-cover {} covers {} (regardless {}) dodges {} "
                "hurt {} shot-at {} provoked {} no-cover {} {:.1f} m; cover tasks ended {} (settled {}), "
                "settled {:.1f} s; plan-less {}% of {} s, shots {}, shoot-from-here {}",
                character, BehaviourName(behaviour), setup, PlanText(base, task), (flags & 0x8000) ? "yes" : "no",
                snapshot.covers, snapshot.regardless_covers, snapshot.dodges, (latches & 0x80) ? "yes" : "no",
                (latches & 0x100) ? "yes" : "no", (flags & 0x800) ? "yes" : "no", (flags & 0x1000) ? "yes" : "no",
                metres, snapshot.cover_tasks_ended, snapshot.cover_tasks_settled, snapshot.cover_seconds,
                snapshot.samples ? 100 * snapshot.planless / snapshot.samples : 0, snapshot.samples,
                snapshot.shots, snapshot.shoot_from_here);
  }
} // namespace

namespace ai_behaviour_debug
{
  void OnAnimType(uint32_t character, uint32_t anim_type)
  {
    const bool dodge = (anim_type >= 0x8C && anim_type <= 0x8F) || (anim_type >= 0x92 && anim_type <= 0x9D);
    if (!dodge || !REXCVAR_GET(pdz_ai_behaviour_log))
    {
      return;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_enemies.find(character);
    if (it != g_enemies.end())
    {
      ++it->second.dodges;
    }
  }

  void OnCoverCreated(const uint8_t *base, uint32_t task)
  {
    if (!REXCVAR_GET(pdz_ai_behaviour_log))
    {
      return;
    }
    CoverTask info;
    info.created = Clock::now();
    info.regardless = base[task + kCoverRegardless] != 0;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_cover_tasks.size() > 512) // tasks whose End we never saw (level unloaded)
    {
      g_cover_tasks.clear();
    }
    g_cover_tasks[task] = info;
  }

  void OnCoverSettled(uint32_t task)
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_cover_tasks.find(task);
    if (it != g_cover_tasks.end() && !it->second.is_settled)
    {
      it->second.is_settled = true;
      it->second.settled = Clock::now();
    }
  }

  void OnCoverPeek(uint32_t task)
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_cover_tasks.find(task);
    if (it != g_cover_tasks.end())
    {
      ++it->second.peeks;
    }
  }

  void OnCoverEnd(const uint8_t *base, uint32_t task)
  {
    CoverTask info;
    {
      std::lock_guard<std::mutex> lock(g_mutex);
      auto it = g_cover_tasks.find(task);
      if (it == g_cover_tasks.end())
      {
        return;
      }
      info = it->second;
      g_cover_tasks.erase(it);
    }
    const auto now = Clock::now();
    const uint32_t character = Load32(base, task + kTaskCharacter);
    const uint32_t state = Load32(base, task + kTaskStateField);
    const double total = std::chrono::duration<double>(now - info.created).count();
    const double settled = info.is_settled ? std::chrono::duration<double>(now - info.settled).count() : 0.0;
    const char *reason = info.stopped       ? "stopped by its owner (plan re-check or plan end)"
                         : state == 2       ? "its own cover tests failed (or the snap-back fix gave up)"
                         : state == 1       ? "ended while running"
                                            : "ended";
    {
      std::lock_guard<std::mutex> lock(g_mutex);
      if (Enemy *enemy = IsGuestPointer(character) ? Find(base, character) : nullptr)
      {
        ++enemy->cover_tasks_ended;
        enemy->cover_tasks_settled += info.is_settled ? 1 : 0;
        enemy->cover_seconds += settled;
      }
    }
    REXLOG_INFO("pdz_ai_behaviour: {:08X} cover task ended{}: {:.1f} s ({}), settled {:.1f} s, peeks {}, {}", character,
                info.regardless ? " (regardless)" : "", total,
                info.is_settled ? "reached its cover point" : "never settled at its cover point", settled, info.peeks,
                reason);
  }
} // namespace ai_behaviour_debug

// sub_821CE990 [821C78D0](task): the move-into-cover task's Stop (its owner
// ends it: clears the in-cover flag, then End).
REX_EXTERN(__imp__sub_821CE990);
REX_HOOK_RAW(sub_821CE990)
{
  const uint32_t task = ctx.r3.u32;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_cover_tasks.find(task);
    if (it != g_cover_tasks.end())
    {
      it->second.stopped = true;
    }
  }
  __imp__sub_821CE990(ctx, base);
}

// --- pdz_ai_shoot_from_here (enhancement, off by default = Rare's behaviour) ---
// When a combat task's plan ends with "no pad for any goal" (result 2) and its
// behaviour has no fallback, sub_821B5DF8 [821B1128] starts Rare's do-nothing
// task (class 39, vtable 82090BB8, empty Update) in the combat task's holder
// +80 (pointer +84). Its end check sub_821B83C0 [821B36F0] ends it after 5 s
// (target seen in the last 20 s) or 10 s, then the planner tries again. With
// the option on, for an eligible enemy that can see its target within its
// maximum range (sub_821E0DC0 [821D9CF8] index 0), that wait is kept (same
// duration, same end) but the enemy attacks from where it stands while it
// lasts: Rare's close-plan attack step sub_821B7520 [821B2850] (the attack
// builder sub_821E1150 [821DA088], which starts nothing new while a shoot task
// is running), every 0.25 s while the target stays visible and in range.
// Nothing moves the enemy, so area-tied guards (character +6348) stay in their
// areas. Eligible as for the debug switch (single player / co-op enemies; not
// allies, snipers, bosses or special-script characters), and not fights a
// level script started with its own behaviour.
namespace
{
  constexpr uint32_t kIdleVtable = 0x82090BB8;
  constexpr uint32_t kTaskFallback = 84;       // combat task: holder +80 -> fallback / do-nothing task
  constexpr uint32_t kAiClock = 0x82E2F334;    // float seconds
  constexpr uint32_t kCharTargetSeen = 12536;  // double, AI clock: target last seen
  constexpr uint32_t kCharTargetInfo = 12652;  // target (else +12628)
  constexpr uint32_t kCharTargetInfoOld = 12628;
  constexpr uint32_t kCharArea = 6348;         // pad area the enemy is tied to (template +93), 0 = none
  constexpr double kVisibleWindow = 1.5;       // the sight check runs about once a second (AI12)
  constexpr auto kAttackGap = std::chrono::milliseconds(250);

  struct ShootFromHere
  {
    uint32_t idle = 0;
    uint32_t character = 0;
    Clock::time_point started{};
    Clock::time_point last_attack{};
    uint32_t shots_at_start = 0;
    uint32_t attack_steps = 0;
    float metres = 0.0f;
  };
  std::unordered_map<uint32_t, ShootFromHere> g_sfh; // by combat task
  std::unordered_map<uint32_t, uint32_t> g_shots;    // shots fired, by character

  float LoadFloat(const uint8_t *base, uint32_t address)
  {
    const uint32_t bits = Load32(base, address);
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
  }

  double LoadDouble(const uint8_t *base, uint32_t address)
  {
    const uint64_t bits = (uint64_t(Load32(base, address)) << 32) | Load32(base, address + 4);
    double value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
  }

  uint32_t TargetOf(const uint8_t *base, uint32_t character)
  {
    const uint32_t target = Load32(base, character + kCharTargetInfo);
    return IsGuestPointer(target) ? target : Load32(base, character + kCharTargetInfoOld);
  }

  bool TargetVisible(const uint8_t *base, uint32_t character)
  {
    const double seen = LoadDouble(base, character + kCharTargetSeen);
    const double now = LoadFloat(base, kAiClock);
    return std::isfinite(seen) && seen > 0.0 && now - seen >= 0.0 && now - seen < kVisibleWindow;
  }

  // Target within the enemy's maximum attack range; metres = its distance.
  bool TargetInRange(PPCContext &ctx, uint8_t *base, uint32_t character, float &metres)
  {
    metres = -1.0f;
    const uint32_t target = TargetOf(base, character);
    if (!IsGuestPointer(target))
    {
      return false;
    }
    rex::CallFrame frame(ctx);
    UseOwnStack(frame, ctx);
    frame.ctx.r3.u64 = character;
    frame.ctx.r4.u64 = target;
    sub_82518280(frame.ctx, base); // squared distance, cm^2
    const double d2 = frame.ctx.f1.f64;
    UseOwnStack(frame, ctx);
    frame.ctx.r3.u64 = character;
    frame.ctx.r4.u64 = 0;
    sub_821E0DC0(frame.ctx, base); // maximum attack range, squared
    const double range2 = frame.ctx.f1.f64;
    if (!std::isfinite(d2) || d2 < 0.0 || !std::isfinite(range2))
    {
      return false;
    }
    metres = float(std::sqrt(d2) / 100.0);
    return d2 < range2;
  }

  bool ScriptFight(uint32_t task)
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto it = g_script_fights.find(task);
    return it != g_script_fights.end() && it->second;
  }

  // Under g_mutex.
  uint32_t ShotsOf(uint32_t character)
  {
    const auto it = g_shots.find(character);
    return it == g_shots.end() ? 0 : it->second;
  }

  // From the combat task's Update: attack while a replaced wait runs; log it
  // when it ends.
  void ShootFromHereTick(PPCContext &ctx, uint8_t *base, uint32_t task)
  {
    ShootFromHere state;
    {
      std::lock_guard<std::mutex> lock(g_mutex);
      const auto it = g_sfh.find(task);
      if (it == g_sfh.end())
      {
        return;
      }
      state = it->second;
    }
    const uint32_t idle = Load32(base, task + kTaskFallback);
    if (idle != state.idle || !IsGuestPointer(idle) || Load32(base, idle) != kIdleVtable)
    {
      uint32_t shots = 0;
      {
        std::lock_guard<std::mutex> lock(g_mutex);
        shots = ShotsOf(state.character) - state.shots_at_start;
        g_sfh.erase(task);
      }
      if (REXCVAR_GET(pdz_ai_behaviour_log))
      {
        REXLOG_INFO("pdz_ai_behaviour: {:08X} shoot-from-here ended after {:.1f} s: {} attack steps, {} shots",
                    state.character, std::chrono::duration<double>(Clock::now() - state.started).count(),
                    state.attack_steps, shots);
      }
      return;
    }
    const auto now = Clock::now();
    if (!REXCVAR_GET(pdz_ai_shoot_from_here) || now - state.last_attack < kAttackGap)
    {
      return;
    }
    float metres = 0.0f;
    const bool attack = TargetVisible(base, state.character) && TargetInRange(ctx, base, state.character, metres);
    if (attack)
    {
      rex::CallFrame frame(ctx);
      UseOwnStack(frame, ctx);
      frame.ctx.r3.u64 = state.character;
      sub_821B7520(frame.ctx, base); // Rare's close-plan attack step
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto it = g_sfh.find(task);
    if (it != g_sfh.end())
    {
      it->second.last_attack = now;
      it->second.attack_steps += attack ? 1 : 0;
    }
  }
} // namespace

// sub_821B5DF8 [821B1128](combat task): the plan ended with "no pad for any
// goal": the behaviour's fallback, else Rare's do-nothing wait.
REX_HOOK_RAW(sub_821B5DF8)
{
  const uint32_t task = ctx.r3.u32;
  const uint32_t before = Load32(base, task + kTaskFallback);
  __imp__sub_821B5DF8(ctx, base);
  if (!REXCVAR_GET(pdz_ai_shoot_from_here))
  {
    return;
  }
  const uint32_t idle = Load32(base, task + kTaskFallback);
  if (!IsGuestPointer(idle) || idle == before || Load32(base, idle) != kIdleVtable || ScriptFight(task))
  {
    return;
  }
  const uint32_t character = Load32(base, task + kTaskCharacter);
  if (!EligibleEnemy(ctx, base, character))
  {
    return;
  }
  const bool log = REXCVAR_GET(pdz_ai_behaviour_log);
  float metres = -1.0f;
  const bool visible = TargetVisible(base, character);
  const bool in_range = TargetInRange(ctx, base, character, metres);
  if (!visible || !in_range)
  {
    if (log)
    {
      REXLOG_INFO("pdz_ai_behaviour: {:08X} no plan: Rare's wait kept (target visible {}, in range {}, {:.1f} m)",
                  character, visible ? "yes" : "no", in_range ? "yes" : "no", metres);
    }
    return;
  }
  ShootFromHere state;
  state.idle = idle;
  state.character = character;
  state.started = Clock::now();
  state.metres = metres;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    state.shots_at_start = ShotsOf(character);
    g_sfh[task] = state;
    if (Enemy *enemy = Find(base, character))
    {
      ++enemy->shoot_from_here;
    }
  }
  if (log)
  {
    REXLOG_INFO("pdz_ai_behaviour: {:08X} no plan: shoot-from-here instead of Rare's wait (target visible, "
                "{:.1f} m, area {})",
                character, metres, Load32(base, character + kCharArea));
  }
}

// sub_8223FC00 [82238938](shooter, weapon, &origin, &aim point, ...): one
// hitscan shot; counted per tracked enemy.
REX_HOOK_RAW(sub_8223FC00)
{
  const uint32_t shooter = ctx.r3.u32;
  aim_assist::OnShotTU3(base, shooter, ctx.r5.u32, ctx.r6.u32); // pdz_autoaim_log (cheap when off)
  if (REXCVAR_GET(pdz_ai_behaviour_log) || REXCVAR_GET(pdz_ai_shoot_from_here))
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    bool tracked = g_enemies.count(shooter) != 0;
    for (const auto &entry : g_sfh)
    {
      tracked = tracked || entry.second.character == shooter;
    }
    if (tracked)
    {
      ++g_shots[shooter];
      if (auto it = g_enemies.find(shooter); it != g_enemies.end())
      {
        ++it->second.shots;
      }
    }
  }
  __imp__sub_8223FC00(ctx, base);
}

// sub_821B5A68 [821B0D98](task): combat task Start; picks the behaviour.
REX_HOOK_RAW(sub_821B5A68)
{
  const uint32_t task = ctx.r3.u32;
  const uint32_t character = Load32(base, task + kTaskCharacter);
  const Mode mode = CurrentMode();
  const uint8_t requested = base[task + kTaskBehaviour];
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_script_fights.size() > 1024) // tasks long gone
    {
      g_script_fights.clear();
    }
    g_script_fights[task] = requested != kUseTemplate;
  }
  bool forced = false;
  uint8_t setup_behaviour = kUseTemplate;
  if (mode.behaviour >= 0 && requested == kUseTemplate && EligibleEnemy(ctx, base, character))
  {
    setup_behaviour = base[Load32(base, character + kCharTemplate) + kTemplateBehaviour];
    base[task + kTaskBehaviour] = uint8_t(mode.behaviour);
    forced = true;
  }
  __imp__sub_821B5A68(ctx, base);
  if (!IsGuestPointer(character))
  {
    return;
  }
  const bool log = REXCVAR_GET(pdz_ai_behaviour_log);
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    Enemy *enemy = Find(base, character);
    if (!enemy && (forced || log))
    {
      Enemy fresh;
      fresh.vtable = Load32(base, character);
      enemy = &g_enemies.emplace(character, fresh).first->second;
    }
    if (enemy)
    {
      enemy->forced = forced;
      enemy->regardless = forced && mode.regardless;
      enemy->setup_behaviour = setup_behaviour;
      enemy->last_seen = Clock::now();
    }
    if (forced && g_announced_mode != mode.name)
    {
      g_announced_mode = mode.name;
      REXLOG_INFO("pdz_debug_ai_behaviour: forcing {} (debug, not a fix)", mode.name);
    }
  }
  if (log)
  {
    std::string how;
    if (forced)
    {
      how = fmt::format(" (forced, setup {})", BehaviourName(setup_behaviour));
    }
    else if (requested != kUseTemplate)
    {
      how = " (chosen by a script)";
    }
    REXLOG_INFO("pdz_ai_behaviour: {:08X} fight starts: {}{}", character,
                BehaviourName(int8_t(base[task + kTaskBehaviour])), how);
  }
}

// sub_821B5B20 [821B0E50](task): combat task Update.
REX_HOOK_RAW(sub_821B5B20)
{
  const uint32_t task = ctx.r3.u32;
  __imp__sub_821B5B20(ctx, base);
  ShootFromHereTick(ctx, base, task);
  if (REXCVAR_GET(pdz_ai_behaviour_log))
  {
    const uint32_t character = Load32(base, task + kTaskCharacter);
    if (IsGuestPointer(character))
    {
      LogLine(ctx, base, task, character);
    }
  }
}

// sub_821B75C8 [821B28F8](character, pad): the planner's move-to-cover action.
REX_HOOK_RAW(sub_821B75C8)
{
  const uint32_t character = ctx.r3.u32;
  const bool regardless = Regardless(base, character);
  if (regardless)
  {
    sub_821B7700(ctx, base); // [821B2A30], unused in the shipped game
  }
  else
  {
    __imp__sub_821B75C8(ctx, base);
  }
  if (ctx.r3.u32 && REXCVAR_GET(pdz_ai_behaviour_log))
  {
    CountCover(base, character, regardless);
  }
}

// sub_821B7840 [821B2B70](character, pad): the planner's cover-in-place action.
REX_HOOK_RAW(sub_821B7840)
{
  const uint32_t character = ctx.r3.u32;
  const bool regardless = Regardless(base, character);
  if (regardless)
  {
    sub_821B78D8(ctx, base); // [821B2C08], unused in the shipped game
  }
  else
  {
    __imp__sub_821B7840(ctx, base);
  }
  if (ctx.r3.u32 && REXCVAR_GET(pdz_ai_behaviour_log))
  {
    CountCover(base, character, regardless);
  }
}
