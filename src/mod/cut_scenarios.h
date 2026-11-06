// Rare's cut multiplayer scenarios (pdz_mp_cut_scenarios): see cut_scenarios.cpp.
#pragma once

#include <cstdint>

#include <rex/ppc/func.h>

namespace cut_scenarios
{
  // A value's menu text for option `id` (guest UTF-16 string), or 0 for the
  // game's own. Called from mp_music.cpp's hook on the value text function.
  uint32_t ValueText(uint8_t *base, uint32_t id, uint32_t value);

  // A value read through the game's getter (sub_823B8340) from `lr`, as the
  // caller should see it. Called from mp_music.cpp's hook on the getter.
  uint32_t ValueRead(uint32_t id, uint32_t lr, uint32_t value);

  // The LEVEL row's next (forward) / previous value with the option on: Rare's
  // own step (`rare`, the unhooked function), then on over maps the chosen
  // scenario doesn't support (Random stays). False: the game's own. Called
  // from mp_music.cpp's next / previous hooks.
  bool LevelStep(PPCContext &ctx, uint8_t *base, uint32_t id, bool forward, PPCFunc *rare, uint32_t &value);

  // The bot count row's step with pdz_bot_count_default: DEFAULT sits
  // between the most bots allowed and 0; the direction is read from Rare's
  // own step (`rare`, the unhooked function: +1 / -1, or a wrap). False: the
  // game's own. Called from mp_music.cpp's step hooks (823BBEE8 / 823BC2C0).
  bool BotCountStep(PPCContext &ctx, uint8_t *base, uint32_t id, PPCFunc *rare, uint32_t &value);

  // The scenario info scene's description text (mp_music.cpp's panel-switch
  // hook): pdz_mp_cut_scenarios on, the Deathmatch scenario option's id, and
  // our description for a scenario value (nullptr for the game's own).
  bool MenuTextOn();
  bool ScenarioLogOn(); // pdz_scenario_log
  uint32_t ScenarioOption();
  const char *ScenarioDescription(int32_t value);
  const char *ScenarioName(int32_t value); // menu name (nullptr for the game's own)
  const char *ScenarioTitle(int32_t value); // the info panel's two-line title (as CTF's CAPTURE / THE FLAG)
} // namespace cut_scenarios
