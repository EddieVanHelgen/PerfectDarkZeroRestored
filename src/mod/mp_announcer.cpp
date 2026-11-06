// Unused multiplayer announcer lines (pdz_mp_announcer_extras, a fix, on by
// default: Rare recorded them and their code never plays them). tu3-base only; TU3 addresses.
//
// The multiplayer speech bank has lines no code plays (docs/MP_INVENTORY.md,
// Announcer lines). These play now where their meaning is clear:
// - Sabotage: Rare's message 20 (Prop_Destroyed) handler (sub_82429520
//   [8241DAB0]) plays DO_SABOTAGE_SUCCESSFUL, at most every 5 s (+17664). In
//   its place, the destroyed property's own line: the property's attribute
//   record ([[prop +100] +4]; +404 team points, +8 a float that tells crates
//   (30) from drums (18)) gives field supplies (crate), fuel dump (drum),
//   ammo cache, comms (both comms kinds) or power node. Rare's spacing stays.
// - Plant the Flag: planting_flag / enemy_planting_flag when a plant starts
//   (human plants: the mode's per-slot start time, mode +(68 + slot) * 4,
//   goes from 0 to a time; our bots: their plant timer starts), relative to
//   the local player's team; enemy_flag_planted when the other team scores
//   (Rare's FLAG_PLANTED is swapped for it, or said 1.5 s later if Rare
//   didn't play it for this player).
// - Deathmatch match lines, from the host's per-frame match-over check
//   (sub_8243B668 [?]: 1 when over; limits in the match settings: kills
//   +4407 (Killcount, Team Killcount), flags +4409 (Capture / Plant the Flag),
//   +4453 Territorial Gains, +4454 Protection; team scores in the scoreboard
//   82D73590 + team * 12 + 8, kills per participant at info +1084):
//   victory / defeat / match_drawn at the end for the local player's side;
//   1kill2win (kill modes) or victory_imminent (others) once when the local
//   side is one point from the limit; teams_level when the two top teams
//   draw level (above 0). DarkOps (rounds) is left out. kill_stolen has no
//   clear trigger and stays unused.
// Every line is logged ("pdz_mp_announcer:").
#include "mp_announcer.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <rex/hook.h>
#include "guest_function.h" // our indirect calls: nullptr for no function (log 101)
#include <rex/logging.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>

#include "../game_cvars.h"
#include "hook_profile.h" // pdz_hook_profile (must follow <rex/hook.h>)

REX_EXTERN(sub_82202D20);
REX_EXTERN(sub_82202C78);
REX_EXTERN(sub_822014B8);
REX_EXTERN(__imp__sub_82429520);
REX_EXTERN(__imp__sub_8243B668);

namespace
{
  using Clock = std::chrono::steady_clock;

  constexpr uint32_t kMatchSettings = 0x82E2FB20;   // -> match settings
  constexpr uint32_t kGame = 0x82D6BFD8;            // -> game state (+40 game type: 2 / 3 multiplayer)
  constexpr uint32_t kParticipantList = 0x82E34224; // -> first participant (+4 next)
  constexpr uint32_t kScoreboard = 0x82D73590;      // 4 x 12 bytes, +8 team score
  constexpr uint32_t kCueSabotageSuccessful = 0x820B6A40;
  constexpr uint32_t kCueFlagPlanted = 0x820B72D4;
  constexpr int kTeams = 4;

  const char *const kVictory = "XACT_SOUNDBANK_MULTIPLAYER_VICTORY";
  const char *const kDefeat = "XACT_SOUNDBANK_MULTIPLAYER_DEFEAT";
  const char *const kDrawn = "XACT_SOUNDBANK_MULTIPLAYER_MATCH_DRAWN";
  const char *const kImminent = "XACT_SOUNDBANK_MULTIPLAYER_VICTORY_IMMINENT";
  const char *const kOneKill = "XACT_SOUNDBANK_MULTIPLAYER_1KILL2WIN";
  const char *const kLevel = "XACT_SOUNDBANK_MULTIPLAYER_TEAMS_LEVEL";
  const char *const kPlanting = "XACT_SOUNDBANK_MULTIPLAYER_PLANTING_FLAG";
  const char *const kEnemyPlanting = "XACT_SOUNDBANK_MULTIPLAYER_ENEMY_PLANTING_FLAG";
  const char *const kEnemyPlanted = "XACT_SOUNDBANK_MULTIPLAYER_ENEMY_FLAG_PLANTED";

  bool Enabled()
  {
    return REXCVAR_GET(pdz_mp_announcer_extras);
  }

  uint32_t Load32(const uint8_t *base, uint32_t address)
  {
    const uint8_t *p = base + address;
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
  }

  uint32_t Load16(const uint8_t *base, uint32_t address)
  {
    return (uint32_t(base[address]) << 8) | base[address + 1];
  }

  float LoadFloat(const uint8_t *base, uint32_t address)
  {
    const uint32_t v = Load32(base, address);
    float f;
    std::memcpy(&f, &v, 4);
    return f;
  }

  bool IsGuestPointer(uint32_t address)
  {
    return address >= 0x00010000u && address < 0xFFFF0000u;
  }

  // A virtual getter (vtable slot), refusing anything whose vtable isn't in
  // the image.
  uint32_t CallVirtual(PPCContext &ctx, uint8_t *base, uint32_t object, uint32_t slot)
  {
    if (!IsGuestPointer(object))
    {
      return 0;
    }
    const uint32_t vtable = Load32(base, object);
    if (vtable < 0x82000000u || vtable >= 0x84000000u)
    {
      return 0;
    }
    PPCFunc *fn = pdz::GuestFunction(Load32(base, vtable + slot));
    if (!fn)
    {
      return 0;
    }
    rex::CallFrame frame(ctx);
    frame.ctx.r3.u64 = object;
    fn(frame.ctx, base);
    return frame.ctx.r3.u32;
  }

  uint32_t Settings(const uint8_t *base)
  {
    const uint32_t s = Load32(base, kMatchSettings);
    return IsGuestPointer(s) ? s : 0;
  }

  bool Multiplayer(const uint8_t *base)
  {
    const uint32_t game = Load32(base, kGame);
    const uint32_t type = IsGuestPointer(game) ? Load32(base, game + 40) : 0;
    return type == 2 || type == 3; // the game treats 3 as multiplayer too
  }

  uint32_t Kind(const uint8_t *base, uint32_t p)
  {
    return Load32(base, p + 944); // 2 player, 3 bot
  }

  int Team(PPCContext &ctx, uint8_t *base, uint32_t p)
  {
    const uint32_t info = Load32(base, p + 80);
    return IsGuestPointer(info) ? int(int8_t(CallVirtual(ctx, base, info + 120, 24))) : -1;
  }

  int Kills(PPCContext &ctx, uint8_t *base, uint32_t p)
  {
    const uint32_t info = Load32(base, p + 80);
    return IsGuestPointer(info) ? int32_t(CallVirtual(ctx, base, info + 1084, 24)) : 0;
  }

  std::vector<uint32_t> Participants(const uint8_t *base)
  {
    std::vector<uint32_t> out;
    uint32_t p = Load32(base, kParticipantList);
    for (int guard = 0; IsGuestPointer(p) && guard < 64; ++guard, p = Load32(base, p + 4))
    {
      const uint32_t kind = Kind(base, p);
      if (kind == 2 || kind == 3)
      {
        out.push_back(p);
      }
    }
    return out;
  }

  // The first local human player (its info +12 byte is set).
  uint32_t LocalPlayer(const uint8_t *base)
  {
    for (uint32_t p : Participants(base))
    {
      const uint32_t info = Load32(base, p + 12);
      if (Kind(base, p) == 2 && IsGuestPointer(info) && base[info + 12])
      {
        return p;
      }
    }
    return 0;
  }

  // Guest copies of our cue names, and one cue reference buffer (a 256-byte
  // name copy, +256 the resolved cue; sub_82202D20 fills it).
  std::map<std::string, uint32_t> g_names;
  uint32_t g_ref = 0;
  Clock::time_point g_last_line{};

  uint32_t GuestName(uint8_t *base, const char *text)
  {
    auto found = g_names.find(text);
    if (found != g_names.end())
    {
      return found->second;
    }
    const size_t size = std::strlen(text) + 1;
    const uint32_t address = REX_KERNEL_MEMORY()->SystemHeapAlloc(uint32_t(size));
    if (address)
    {
      std::memcpy(base + address, text, size);
    }
    g_names.emplace(text, address);
    return address;
  }

  bool Resolves(PPCContext &ctx, uint8_t *base, uint32_t name)
  {
    rex::CallFrame frame(ctx);
    frame.ctx.r3.u64 = name;
    sub_82202C78(frame.ctx, base);
    return int32_t(frame.ctx.r3.u32) >= 0;
  }

  const char *Short(const char *cue)
  {
    return cue + std::strlen("XACT_SOUNDBANK_MULTIPLAYER_");
  }

  // Queues a line on the announcer (sub_822014B8 [821F9800](ref, f1 volume,
  // f2, r6 priority, r7, r8), as Rare's lead lines: volume 1, priority 1).
  void Say(PPCContext &ctx, uint8_t *base, const char *cue, const std::string &why)
  {
    const auto now = Clock::now();
    if (now - g_last_line < std::chrono::seconds(2))
    {
      return; // our lines never pile up
    }
    if (!g_ref)
    {
      g_ref = REX_KERNEL_MEMORY()->SystemHeapAlloc(272);
    }
    const uint32_t name = GuestName(base, cue);
    if (!g_ref || !name)
    {
      return;
    }
    {
      rex::CallFrame frame(ctx);
      frame.ctx.r3.u64 = g_ref;
      frame.ctx.r4.u64 = name;
      sub_82202D20(frame.ctx, base);
    }
    if (int32_t(Load32(base, g_ref + 256)) < 0)
    {
      REXLOG_INFO("pdz_mp_announcer: {} not in the loaded banks ({})", Short(cue), why);
      return;
    }
    {
      rex::CallFrame frame(ctx);
      frame.ctx.r3.u64 = g_ref;
      frame.ctx.f1.f64 = 1.0;
      frame.ctx.f2.f64 = 0.0;
      frame.ctx.r6.u64 = 1;
      frame.ctx.r7.u64 = 0;
      frame.ctx.r8.u64 = 0;
      sub_822014B8(frame.ctx, base);
    }
    g_last_line = now;
    REXLOG_INFO("pdz_mp_announcer: {} ({})", Short(cue), why);
  }

  // --- Sabotage ---

  thread_local const char *t_sabotage_line = nullptr;

  const char *SabotageLine(const uint8_t *base, uint32_t prop)
  {
    const uint32_t holder = IsGuestPointer(prop) ? Load32(base, prop + 100) : 0;
    const uint32_t record = IsGuestPointer(holder) ? Load32(base, holder + 4) : 0;
    if (!IsGuestPointer(record))
    {
      return nullptr;
    }
    switch (Load16(base, record + 404))
    {
    case 1:
      // Crate (30) or oil drum (18): the same values otherwise.
      return LoadFloat(base, record + 8) < 24.0f ? "XACT_SOUNDBANK_MULTIPLAYER_DO_FUEL_DUMP_DESTROYED"
                                                 : "XACT_SOUNDBANK_MULTIPLAYER_DO_FIELD_SUPPLIES_DESTROYED";
    case 2:
      return "XACT_SOUNDBANK_MULTIPLAYER_DO_AMMO_CACHE_DESTROYED";
    case 5:
    case 20:
      return "XACT_SOUNDBANK_MULTIPLAYER_DO_COMMS_DESTROYED";
    case 40:
      return "XACT_SOUNDBANK_MULTIPLAYER_DO_POWER_NODE_DESTOYED"; // Rare's spelling
    default:
      return nullptr;
    }
  }

  // --- Plant the Flag ---

  struct PlantState
  {
    uint32_t mode = 0;
    uint32_t start[4] = {};
    int scorer_team = -1;
    Clock::time_point scored{};
    bool scored_pending = false; // the enemy-planted line not said yet
  } g_plant;

  void PlantLine(PPCContext &ctx, uint8_t *base, int team, const std::string &who)
  {
    const uint32_t local = LocalPlayer(base);
    if (!local || team < 0)
    {
      return;
    }
    const bool ours = Team(ctx, base, local) == team;
    Say(ctx, base, ours ? kPlanting : kEnemyPlanting, who + ", team " + std::to_string(team) + " planting");
  }

  // --- Match lines ---

  struct MatchState
  {
    bool playing = false;
    bool one_away_said = false;
    bool level = false;
    Clock::time_point level_said{};
  } g_match;

  struct Sides
  {
    std::vector<int> scores; // per team, or per participant without teams
    int local = -1;          // the local player's side
    int limit = 0;
    bool kills = false;      // Killcount / Team Killcount
    const char *scenario = "";
  };

  bool ReadSides(PPCContext &ctx, uint8_t *base, Sides &out)
  {
    const uint32_t s = Settings(base);
    if (!s || base[s + 4404] != 0 || !Multiplayer(base))
    {
      return false; // Deathmatch only
    }
    const uint32_t local = LocalPlayer(base);
    if (!local)
    {
      return false;
    }
    const int scenario = base[s + 4414];
    const int teams = base[s + 4410];
    static const char *const kNames[] = {"Killcount", "Team Killcount", "Capture the Flag", "Plant the Flag",
                                         "Territorial Gains", "Protection"};
    if (scenario > 5)
    {
      return false;
    }
    out.scenario = kNames[scenario];
    out.kills = scenario <= 1;
    out.limit = scenario <= 1 ? base[s + 4407] : scenario <= 3 ? base[s + 4409] : scenario == 4 ? base[s + 4453]
                                                                                                 : base[s + 4454];
    const std::vector<uint32_t> all = Participants(base);
    if (teams >= 2)
    {
      out.scores.assign(std::min(teams, kTeams), 0);
      for (int t = 0; t < int(out.scores.size()); ++t)
      {
        if (!out.kills)
        {
          out.scores[t] = int32_t(Load32(base, kScoreboard + uint32_t(t) * 12 + 8));
        }
      }
      if (out.kills)
      {
        for (uint32_t p : all)
        {
          const int t = Team(ctx, base, p);
          if (t >= 0 && t < int(out.scores.size()))
          {
            out.scores[t] += Kills(ctx, base, p);
          }
        }
      }
      out.local = Team(ctx, base, local);
    }
    else
    {
      for (size_t i = 0; i < all.size(); ++i)
      {
        out.scores.push_back(Kills(ctx, base, all[i]));
        if (all[i] == local)
        {
          out.local = int(i);
        }
      }
    }
    return out.local >= 0 && out.local < int(out.scores.size());
  }

  std::string ScoreText(const Sides &s)
  {
    std::string text;
    for (size_t i = 0; i < s.scores.size(); ++i)
    {
      text += (i ? " / " : "") + std::to_string(s.scores[i]);
    }
    return std::string(s.scenario) + ", scores " + text + ", yours " + std::to_string(s.scores[size_t(s.local)]) +
           (s.limit ? ", limit " + std::to_string(s.limit) : "");
  }

  void MatchTick(PPCContext &ctx, uint8_t *base, bool over)
  {
    Sides s;
    if (!ReadSides(ctx, base, s))
    {
      return;
    }
    if (!over && !g_match.playing)
    {
      g_match = MatchState{};
      g_match.playing = true;
    }
    if (!g_match.playing)
    {
      return;
    }
    const int mine = s.scores[size_t(s.local)];
    int best_other = -1;
    for (size_t i = 0; i < s.scores.size(); ++i)
    {
      if (int(i) != s.local)
      {
        best_other = std::max(best_other, s.scores[i]);
      }
    }
    if (over)
    {
      g_match.playing = false;
      const char *line = mine > best_other ? kVictory : mine < best_other ? kDefeat : kDrawn;
      Say(ctx, base, line, "match over: " + ScoreText(s));
      return;
    }
    if (s.limit >= 2 && mine == s.limit - 1 && !g_match.one_away_said)
    {
      g_match.one_away_said = true;
      Say(ctx, base, s.kills ? kOneKill : kImminent, "one from the limit: " + ScoreText(s));
    }
    // Teams level: the two best teams draw level (above 0).
    if (s.scores.size() >= 2 && base[Settings(base) + 4410] >= 2)
    {
      std::vector<int> sorted = s.scores;
      std::sort(sorted.rbegin(), sorted.rend());
      const bool level = sorted[0] > 0 && sorted[0] == sorted[1];
      if (level && !g_match.level && Clock::now() - g_match.level_said > std::chrono::seconds(10))
      {
        g_match.level_said = Clock::now();
        Say(ctx, base, kLevel, "top teams level: " + ScoreText(s));
      }
      g_match.level = level;
    }
  }
} // namespace

namespace mp_announcer
{
  void Line(PPCContext &ctx, uint8_t *base, const char *cue, const char *why)
  {
    Say(ctx, base, cue, why);
  }

  uint32_t CueName(PPCContext &ctx, uint8_t *base, uint32_t name)
  {
    if (!Enabled())
    {
      return name;
    }
    if (name == kCueSabotageSuccessful && t_sabotage_line)
    {
      const uint32_t line = GuestName(base, t_sabotage_line);
      if (line && Resolves(ctx, base, line))
      {
        REXLOG_INFO("pdz_mp_announcer: Sabotage: {} (instead of DO_SABOTAGE_SUCCESSFUL)", Short(t_sabotage_line));
        return line;
      }
      return name;
    }
    if (name == kCueFlagPlanted && g_plant.scored_pending &&
        Clock::now() - g_plant.scored < std::chrono::seconds(3))
    {
      g_plant.scored_pending = false;
      const uint32_t local = LocalPlayer(base);
      if (local && Team(ctx, base, local) != g_plant.scorer_team)
      {
        const uint32_t line = GuestName(base, kEnemyPlanted);
        if (line && Resolves(ctx, base, line))
        {
          REXLOG_INFO("pdz_mp_announcer: Plant the Flag: ENEMY_FLAG_PLANTED (instead of FLAG_PLANTED; team {} "
                      "scored)",
                      g_plant.scorer_team);
          return line;
        }
      }
    }
    return name;
  }

  void BotPlantStarted(PPCContext &ctx, uint8_t *base, int team)
  {
    if (Enabled())
    {
      PlantLine(ctx, base, team, "a bot");
    }
  }

  void PlantScored(PPCContext &ctx, uint8_t *base, uint32_t participant)
  {
    if (!Enabled())
    {
      return;
    }
    g_plant.scorer_team = Team(ctx, base, participant);
    g_plant.scored = Clock::now();
    g_plant.scored_pending = true;
  }

  void PlantTheFlagTick(PPCContext &ctx, uint8_t *base, uint32_t mode)
  {
    if (!Enabled() || !IsGuestPointer(mode))
    {
      return;
    }
    if (g_plant.mode != mode)
    {
      g_plant = PlantState{};
      g_plant.mode = mode;
      for (uint32_t slot = 0; slot < 4; ++slot)
      {
        g_plant.start[slot] = Load32(base, mode + (68 + slot) * 4);
      }
    }
    // Human plants: a local slot's start time appears.
    for (uint32_t slot = 0; slot < 4; ++slot)
    {
      const uint32_t start = Load32(base, mode + (68 + slot) * 4);
      if (start && !g_plant.start[slot])
      {
        for (uint32_t p : Participants(base))
        {
          if (Kind(base, p) == 2 && Load32(base, p + 1652) == slot)
          {
            PlantLine(ctx, base, Team(ctx, base, p), "a player");
            break;
          }
        }
      }
      g_plant.start[slot] = start;
    }
    // Rare didn't ask for FLAG_PLANTED for us: say the enemy line ourselves.
    if (g_plant.scored_pending && Clock::now() - g_plant.scored > std::chrono::milliseconds(1500))
    {
      g_plant.scored_pending = false;
      const uint32_t local = LocalPlayer(base);
      if (local && Team(ctx, base, local) != g_plant.scorer_team)
      {
        Say(ctx, base, kEnemyPlanted, "team " + std::to_string(g_plant.scorer_team) + " planted");
      }
    }
  }
} // namespace mp_announcer

// The Sabotage CP3 plant log (cut_scenarios.cpp, pdz_scenario_log), and the
// end of the message for the announcer-line log (job 2d, pdz_sabotage_cue_log).
void PdzSabotageMessageLog(const PPCContext &ctx, uint8_t *base);
void PdzSabotageMessageDone();

// sub_82429520 [8241DAB0](sabotage, r4, prop, r6, r7, message): Sabotage's
// level messages; message 20 (Prop_Destroyed) may play
// DO_SABOTAGE_SUCCESSFUL, which CueName swaps for the target's line.
REX_HOOK_RAW(sub_82429520)
{
  PdzSabotageMessageLog(ctx, base);
  const uint32_t message = ctx.r8.u32;
  if (Enabled() && IsGuestPointer(message) && base[message + 1] == 20)
  {
    t_sabotage_line = SabotageLine(base, ctx.r5.u32);
  }
  __imp__sub_82429520(ctx, base);
  t_sabotage_line = nullptr;
  PdzSabotageMessageDone();
}

// sub_8243B668(): the host's per-frame "is the match over" check (1 = over),
// while a match is being played (game state 5, from sub_8243D1A8).
void PdzTestMpSnapshotTick(uint8_t *base, bool over); // test_mp_setup.cpp (pdz_test_mp_snapshot_s)

REX_HOOK_RAW(sub_8243B668)
{
  PPCContext saved = ctx;
  __imp__sub_8243B668(ctx, base);
  const bool over = (ctx.r3.u32 & 0xFF) != 0;
  if (Enabled())
  {
    MatchTick(saved, base, over);
  }
  PdzTestMpSnapshotTick(base, over);
}
