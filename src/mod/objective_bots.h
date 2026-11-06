// Shared objective-bot toolkit (docs/TU3_BASE.md, "Objective bots"): the
// pieces every objective mode's bots use. Protection uses it first
// (cut_scenarios.cpp); Plant the Flag keeps its own working code for now and
// Sabotage comes later.
#pragma once

#include <cstdint>
#include <string>

#include <rex/ppc/func.h>

namespace objective_bots
{
  // A team member's job in an objective mode.
  enum class Role
  {
    None,
    Carrier, // carries the mode's item and does the objective action
    Escort,  // stays with our carrier
    Hunt,    // goes after the enemy carrier
    Guard,   // holds the objective
  };

  const char *RoleName(Role role);

  // The role for the index-th helper (a team member who isn't the carrier):
  // escort, hunt, guard, in turn; hunt becomes escort with no enemy carrier.
  Role HelperRole(size_t index, bool enemy_carrier);

  // Bot goals the bot scripts understand (the CTF team brain's numbers).
  constexpr uint32_t kGoalRoam = 0;
  // Goals 3 and 8 take a CHARACTER target only: the script reads the target's
  // character fields (+180, sub_82149F60 / sub_82149D80), and a prop target
  // (a Data Thief, a terminal) crashed the game on a null read. Walking a bot
  // to a prop needs Rare's route tasks (as its teleporter hack 821C1A88).
  constexpr uint32_t kGoalAttackCarrier = 3; // target: a character to hunt
  constexpr uint32_t kGoalEscort = 8;        // target: a character to stay with

  // Sets a bot's goal and target (sub_82539AA0, which re-plans the bot) only
  // when they differ from what the bot has (bot +12976 goal, +14068 target).
  // Returns true if it set them.
  bool SetGoal(PPCContext &ctx, uint8_t *base, uint32_t bot, uint32_t goal, uint32_t target);

  // True while SetGoal is calling Rare's setter (the goal log names our code).
  bool SettingGoal();

  // A mode's objective action: how long it takes and where it counts.
  struct ActionRules
  {
    double seconds;  // time at the objective to complete it
    float enter_cm;  // the action runs within this distance
    float leave_cm;  // and keeps running (paused) until beyond this
    double grace_s;  // or outside enter_cm for this long (combat strafing)
  };

  // One actor's progress on an objective action (resumable, like Plant the
  // Flag's plant: short exits pause it, a long one or leaving resets it).
  class Action
  {
  public:
    enum class Event
    {
      None,
      Started,
      Paused,
      Resumed,
      Reset,
      Completed,
    };

    // distance: the actor's distance to the objective; dt: seconds.
    Event Update(const ActionRules &rules, float distance, double dt);
    void Clear();
    double Progress() const { return progress_; }
    bool Running() const { return running_; }

  private:
    double progress_ = 0.0;
    double outside_ = 0.0;
    bool running_ = false;
    bool paused_ = false;
  };

  const char *EventName(Action::Event event);

  // --- A bot's brain ---
  // A bot's brain is its bot script object (updated every frame by
  // sub_821C2158 [821BCB80]: character +64, activity +56). Bots move on
  // Rare's own goals (SetGoal); Protection's bot hacker uses Rare's goal 4
  // (cut_scenarios.cpp). Our own route layer (Trip) was removed after it
  // fought Rare's planner (logs 071-082).

  // The bot's brain (0 if not seen yet: brains are learned from their
  // per-frame update).
  uint32_t BrainOf(const uint8_t *base, uint32_t character);

  // The character is down (dead or dying): its vtable +232, the check the bot
  // brain's own update and chooser make before doing anything. For a bot it
  // is sub_821C6E60: brain activity 13 (dying, set by the chooser on its
  // on-death call) or a type 9 task on the route holder.
  bool CharacterDown(PPCContext &ctx, uint8_t *base, uint32_t character);
} // namespace objective_bots
