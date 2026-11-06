// Shared objective-bot toolkit: see objective_bots.h and docs/TU3_BASE.md
// ("Objective bots"). tu3-base only; TU3 addresses, launch addresses in
// brackets.
#include "objective_bots.h"
#include "bot_watch.h"

#include <unordered_map>

#include <fmt/format.h>

#include <rex/hook.h>
#include "guest_function.h" // our indirect calls: nullptr for no function (log 101)
#include <rex/ppc/func.h>
#include "hook_profile.h" // pdz_hook_profile (must follow <rex/hook.h>)

REX_EXTERN(sub_82539AA0);
REX_EXTERN(__imp__sub_821C2158);

namespace
{
  constexpr uint32_t kBotGoal = 12976;       // bot: goal
  constexpr uint32_t kBotGoalTarget = 14068; // bot: goal target (object)

  constexpr uint32_t kBrainCharacter = 64; // brain: its bot character

  uint32_t Load32(const uint8_t *base, uint32_t address)
  {
    const uint8_t *p = base + address;
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
  }

  bool IsGuestPointer(uint32_t address)
  {
    return address >= 0x00010000u && address < 0xFFFF0000u;
  }

  std::unordered_map<uint32_t, uint32_t> g_brains; // character -> brain (from its update)
  bool g_setting_goal = false;                     // SetGoal is in Rare's setter
} // namespace

namespace objective_bots
{
  const char *RoleName(Role role)
  {
    switch (role)
    {
    case Role::Carrier:
      return "carrier";
    case Role::Escort:
      return "escort";
    case Role::Hunt:
      return "hunt";
    case Role::Guard:
      return "guard";
    default:
      return "none";
    }
  }

  Role HelperRole(size_t index, bool enemy_carrier)
  {
    switch (index % 3)
    {
    case 0:
      return Role::Escort;
    case 1:
      return enemy_carrier ? Role::Hunt : Role::Escort;
    default:
      return Role::Guard;
    }
  }

  // sub_82539AA0 [8252A600](bot, target, goal, replan): the CTF team brain's
  // own way of steering a bot (r6 = 1 as it passes).
  bool SetGoal(PPCContext &ctx, uint8_t *base, uint32_t bot, uint32_t goal, uint32_t target)
  {
    if (Load32(base, bot + kBotGoal) == goal && Load32(base, bot + kBotGoalTarget) == target)
    {
      return false;
    }
    rex::CallFrame frame(ctx);
    frame.ctx.r3.u64 = bot;
    frame.ctx.r4.u64 = target;
    frame.ctx.r5.u64 = goal;
    frame.ctx.r6.u64 = 1;
    g_setting_goal = true;
    sub_82539AA0(frame.ctx, base);
    g_setting_goal = false;
    return true;
  }

  bool SettingGoal()
  {
    return g_setting_goal;
  }

  Action::Event Action::Update(const ActionRules &rules, float distance, double dt)
  {
    if (distance < rules.enter_cm)
    {
      outside_ = 0.0;
      const bool started = !running_;
      const bool resumed = running_ && paused_;
      running_ = true;
      paused_ = false;
      progress_ += dt;
      if (progress_ >= rules.seconds)
      {
        Clear();
        return Event::Completed;
      }
      return started ? Event::Started : resumed ? Event::Resumed : Event::None;
    }
    if (!running_)
    {
      return Event::None;
    }
    outside_ += dt;
    if (distance >= rules.leave_cm || outside_ >= rules.grace_s)
    {
      Clear();
      return Event::Reset;
    }
    if (!paused_)
    {
      paused_ = true;
      return Event::Paused;
    }
    return Event::None;
  }

  void Action::Clear()
  {
    progress_ = 0.0;
    outside_ = 0.0;
    running_ = false;
    paused_ = false;
  }

  const char *EventName(Action::Event event)
  {
    switch (event)
    {
    case Action::Event::Started:
      return "started";
    case Action::Event::Paused:
      return "paused (stepped out)";
    case Action::Event::Resumed:
      return "resumed";
    case Action::Event::Reset:
      return "interrupted (left)";
    case Action::Event::Completed:
      return "completed";
    default:
      return "";
    }
  }
} // namespace objective_bots

// --- A bot's brain ---

namespace objective_bots
{
  uint32_t BrainOf(const uint8_t *base, uint32_t character)
  {
    const auto it = g_brains.find(character);
    if (it == g_brains.end() || Load32(base, it->second + kBrainCharacter) != character)
    {
      return 0;
    }
    return it->second;
  }

  bool CharacterDown(PPCContext &ctx, uint8_t *base, uint32_t character)
  {
    if (!IsGuestPointer(character))
    {
      return true;
    }
    const uint32_t vtable = Load32(base, character);
    if (vtable < 0x82000000u || vtable >= 0x84000000u)
    {
      return true;
    }
    PPCFunc *fn = pdz::GuestFunction(Load32(base, vtable + 232));
    if (!fn)
    {
      return false;
    }
    rex::CallFrame frame(ctx);
    frame.ctx.r1.u64 = (ctx.r1.u32 - 0x200) & ~0xFu;
    frame.ctx.r3.u64 = character;
    fn(frame.ctx, base);
    return (frame.ctx.r3.u32 & 0xFF) != 0;
  }
} // namespace objective_bots

void PdzSabotageBotFrame(PPCContext &ctx, uint8_t *base, uint32_t bot, double dt);
void PdzPadLinkDiagFrame(PPCContext &ctx, uint8_t *base);
void PdzTestIgnorePlayerFrame(PPCContext &ctx, uint8_t *base, uint32_t bot);
bool PdzBotHoldStill(uint32_t character);

// sub_821C2158 [821BCB80](brain, f1 dt): a bot brain's per-frame update;
// learns the brain of each bot character. Sabotage attackers act after it
// (cut_scenarios.cpp: the update clears the bot's aim point first thing, and
// Rare's activities set it during the update, as our aim does after).
REX_HOOK_RAW(sub_821C2158)
{
  const uint32_t brain = ctx.r3.u32;
  const uint32_t character = Load32(base, brain + kBrainCharacter);
  if (IsGuestPointer(character))
  {
    if (g_brains.size() > 256)
    {
      g_brains.clear();
    }
    g_brains[character] = brain;
  }
  PPCContext saved = ctx;
  const double dt = ctx.f1.f64;
  // A bot planting (Sabotage) or hacking (Protection) stands still: Rare's
  // brain (its route to the goal pad) is skipped for the timed action
  // (cut_scenarios.cpp, PdzBotHoldStill).
  const bool held = PdzBotHoldStill(character);
  if (!held)
  {
    __imp__sub_821C2158(ctx, base);
  }
  if (IsGuestPointer(character))
  {
    bot_watch::Frame(saved, base, character, held);
    PdzTestIgnorePlayerFrame(saved, base, character);
    PdzSabotageBotFrame(saved, base, character, dt);
    PdzPadLinkDiagFrame(saved, base);
  }
}
