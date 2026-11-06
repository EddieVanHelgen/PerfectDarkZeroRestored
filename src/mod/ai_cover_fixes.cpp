// Fixes for the enemies' cover task. See README.md next to this file.
//
// tu3-base: Title Update 3 addresses (launch addresses in brackets). TU3's
// cover task code only moved its constants (constructor) or offsets, so the
// three issues below are still there (docs/TU3_BASE.md). On the launch branch
// the hooks live in cover_log.cpp; here they are at the end of this file.
//
// The cover task (vtable 82092950 [82094758]) walks an enemy to a cover point (a level
// pad), settles it there with an aligned animation and then peeks out from
// linked peek points. Each fix is its own option and only applies while the
// master switch pdz_ai_cover_fixes is on (bind_ai_cover_ab flips it); with it
// off the task runs exactly as in the original game. Only cover task objects
// are touched; no shared constants.
//
// - Grace timers. The cover tests (sub_821C7DC8) end the task when no peek
//   point has seen the target for 5 s (+0x70) or the target has been out of
//   their arc for 6 s (+0x78). Both timestamps are set when the task is
//   created (821C6C0C/6C14), but the tests only start once the walk and
//   settle-in sequence (+0x3C) has finished, so a walk of more than 5-6 s
//   fails the tests on arrival. pdz_ai_cover_fix_grace restarts both when
//   the enemy has settled.
// - Peek roll. The constructor picks the alternate peek animations (+0x59)
//   with rand() * 3.0f == 0 (821C6BB0), where rand() is a float in [0, 1),
//   so it never happens on points without flag 0x8 (those always use the
//   alternates). pdz_ai_cover_fix_peek_roll makes it a real 1-in-3 chance.
// - Snap-back. While settled, an enemy more than 15 cm from its point
//   (821C7464, 3D) replays the aligned idle to pull it back, with no limit.
//   During that sequence the task does nothing else, so a point that can't be
//   reached within 15 cm keeps the enemy re-aligning without shooting.
//   pdz_ai_cover_fix_snapback ends the task (state 2, as the game does when
//   its own tests fail) after kSnapBackLimit snap-backs in a row with no peek
//   or look-out in between, so the planner picks something else.
#include "ai_cover_fixes.h"

#include <chrono>
#include <cstring>
#include <random>
#include <unordered_map>

#include <rex/hook.h>

#include "../game_cvars.h"
#include "ai_behaviour_debug.h"
#include "fix_log.h"
#include "hook_profile.h" // pdz_hook_profile (must follow <rex/hook.h>)

REX_EXTERN(__imp__sub_821CDB88);
REX_EXTERN(__imp__sub_821CE328);
REX_EXTERN(__imp__sub_821CE270);

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

  void StoreBeDouble(uint8_t *base, uint32_t address, double value)
  {
    uint64_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    StoreBe32(base, address, uint32_t(bits >> 32));
    StoreBe32(base, address + 4, uint32_t(bits));
  }

  // Task fields.
  constexpr uint32_t kTaskState = 0x0C; // 1 running, 2 done
  constexpr uint32_t kTaskSequence = 0x3C;
  constexpr uint32_t kTaskAttack = 0x44;
  constexpr uint32_t kTaskLookOut = 0x4C;
  constexpr uint32_t kTaskPeekVariant = 0x59;
  constexpr uint32_t kTaskLastSight = 0x70;  // double, AI clock
  constexpr uint32_t kTaskLastInArc = 0x78;  // double, AI clock
  constexpr uint32_t kStateRunning = 1;
  constexpr uint32_t kStateDone = 2;
  // Cover point flags (+0x34): 0x8 = points the constructor always gives the
  // alternate peeks (and that use the facing test instead of the timers).
  constexpr uint32_t kPointFlags = 0x34;
  constexpr uint32_t kPointFlagAlternate = 0x8;
  // AI clock: float seconds since the level started.
  constexpr uint32_t kAiClock = 0x82E2F334; // [82E2E478]

  constexpr int kSnapBackLimit = 3;

  struct TaskState
  {
    bool settled = false;
    uint32_t sequence = 0;
    uint32_t attack = 0;
    uint32_t look_out = 0;
    int snap_backs = 0; // in a row, without a peek or look-out between
  };
  std::unordered_map<uint32_t, TaskState> g_tasks;

  std::minstd_rand g_rng{uint32_t(std::chrono::steady_clock::now().time_since_epoch().count())};

  bool Enabled()
  {
    return REXCVAR_GET(pdz_ai_cover_fixes);
  }
} // namespace

namespace ai_cover_fixes
{
  bool AfterCreate(uint8_t *base, uint32_t task, uint32_t point)
  {
    g_tasks.erase(task);
    fix_log::Count("cover tasks created");
    if (!Enabled() || !REXCVAR_GET(pdz_ai_cover_fix_peek_roll) || !point ||
        (LoadBe32(base, point + kPointFlags) & kPointFlagAlternate))
    {
      return false;
    }
    base[task + kTaskPeekVariant] = std::uniform_int_distribution<int>(0, 2)(g_rng) == 0 ? 1 : 0;
    fix_log::Count(base[task + kTaskPeekVariant] ? "cover peek roll: alternate peeks (fix)" : "cover peek roll: normal peeks");
    return true;
  }

  bool BeforeUpdate(uint8_t *base, uint32_t task)
  {
    TaskState &state = g_tasks[task];
    if (state.settled || LoadBe32(base, task + kTaskState) != kStateRunning ||
        LoadBe32(base, task + kTaskSequence) != 0)
    {
      return false;
    }
    // First update with the walk / settle-in sequence finished: the cover
    // tests run in this update for the first time.
    state.settled = true;
    ai_behaviour_debug::OnCoverSettled(task);
    if (!Enabled() || !REXCVAR_GET(pdz_ai_cover_fix_grace))
    {
      return false;
    }
    const double now = LoadBeFloat(base, kAiClock);
    StoreBeDouble(base, task + kTaskLastSight, now);
    StoreBeDouble(base, task + kTaskLastInArc, now);
    fix_log::Count("cover grace reset on arrival");
    return true;
  }

  bool AfterUpdate(uint8_t *base, uint32_t task)
  {
    TaskState &state = g_tasks[task];
    const uint32_t sequence = LoadBe32(base, task + kTaskSequence);
    const uint32_t attack = LoadBe32(base, task + kTaskAttack);
    const uint32_t look_out = LoadBe32(base, task + kTaskLookOut);
    if ((attack && !state.attack) || (look_out && !state.look_out))
    {
      state.snap_backs = 0;
    }
    if (attack && !state.attack)
    {
      ai_behaviour_debug::OnCoverPeek(task);
    }
    bool gave_up = false;
    if (state.settled && sequence && sequence != state.sequence)
    {
      ++state.snap_backs;
      fix_log::Count("cover snap-backs");
      if (Enabled() && REXCVAR_GET(pdz_ai_cover_fix_snapback) && state.snap_backs >= kSnapBackLimit &&
          LoadBe32(base, task + kTaskState) == kStateRunning)
      {
        StoreBe32(base, task + kTaskState, kStateDone);
        gave_up = true;
        fix_log::Count("cover snap-back give-up");
      }
    }
    state.sequence = sequence;
    state.attack = attack;
    state.look_out = look_out;
    return gave_up;
  }

  void OnEnd(uint32_t task)
  {
    g_tasks.erase(task);
  }

  std::string ActiveSummary()
  {
    if (!Enabled())
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
    add(REXCVAR_GET(pdz_ai_cover_fix_grace), "grace");
    add(REXCVAR_GET(pdz_ai_cover_fix_peek_roll), "peek roll");
    add(REXCVAR_GET(pdz_ai_cover_fix_snapback), "snap-back");
    return text.empty() ? "on, no fixes selected" : text;
  }
} // namespace ai_cover_fixes

// sub_821CDB88 [821C6AC8](task, point, regardless): an enemy's cover task is
// created.
REX_HOOK_RAW(sub_821CDB88)
{
  const uint32_t task = ctx.r3.u32;
  const uint32_t point = ctx.r4.u32;
  __imp__sub_821CDB88(ctx, base);
  ai_cover_fixes::AfterCreate(base, task, point);
  ai_behaviour_debug::OnCoverCreated(base, task);
}

// sub_821CE328 [821C7268](task, ...): per-frame update.
REX_HOOK_RAW(sub_821CE328)
{
  const uint32_t task = ctx.r3.u32;
  ai_cover_fixes::BeforeUpdate(base, task);
  __imp__sub_821CE328(ctx, base);
  ai_cover_fixes::AfterUpdate(base, task);
}

// sub_821CE270 [821C71B0](task): End.
REX_HOOK_RAW(sub_821CE270)
{
  ai_cover_fixes::OnEnd(ctx.r3.u32);
  ai_behaviour_debug::OnCoverEnd(base, ctx.r3.u32);
  __imp__sub_821CE270(ctx, base);
}
