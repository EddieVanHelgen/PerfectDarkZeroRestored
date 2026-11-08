// Testing tools for repeatable multiplayer test matches (stage 2 M5: the heavy MP case). TU3 addresses. Both cvars
// default to no effect; nothing here changes a match without them.
//
// pdz_test_mp_setup = "bots=31,points=15,time=10,map=urban,scenario=dm" (any subset; "fresh" alone = only the first
// point below):
// - Rare's saved setup is ignored. The Combat Arena setup is saved per mode / scenario in the profile at match start
//   (pdzsave.dat; the "saving" notification) and loaded when the setup opens: sub_823BD140 asks sub_823BD1A8 (bit 7
//   of the slot's first byte at profile +8968 + (mode * 6 + scenario) * 28) and then loads options 2-32 except 12 /
//   13 (sub_823BD260 -> sub_82509920, LEVEL is not saved), else Rare's presets (823BAE80, 823BB598). A scripted
//   route that edits values with LEFT / RIGHT therefore starts from the previous run's values (the testdata user
//   folder is reused between runs): with the option on, sub_823BD1A8 answers "nothing saved", so every run starts
//   from Rare's presets as on a fresh profile.
// - The listed values are set through the game's own setter (sub_823B8AD8, the hooked one: player_limit.cpp's
//   max players 16 -> 32 applies) right after the setup loads (sub_823BD140) and again just before the match
//   start's pass over the options (sub_82432678, from cut_scenarios.cpp's hook). Option ids (TU3 name table, index
//   - 3): 1 LEVEL (a map position, sub_823BB108 of the level name), 2 POINTS_LIMIT, 3 TIME_LIMIT, 5 PLAYERS_MAX,
//   19 BOT_COUNT, 20 bot difficulty (key "skill"), 61 MODE, 62 SCENARIO, 63 DARKOPS_SCENARIO, 77 BOT_MAXCOUNT. The bot count setter clamps to max
//   players - humans and the bot limit, so max players (5) and the bot limit (77) are raised first when needed. The
//   mode itself is never changed (the script picks DEATHMATCH / DARK OPS): a scenario of the other mode is skipped
//   with a warning.
// - Logged: 'pdz_test_mp: <when>: ...' for each apply that changed something, 'pdz_test_mp: match start: in
//   effect ...' after the match start's pass (the option values the match starts with), and, once per match,
//   'pdz_test_mp: all N participants spawned: B bots, H humans' (characters counted at their spawn, so the number
//   of bots really spawned).
//
// pdz_test_spawn_pad = N (default -1 = off): the human player's spawn pad is the N-th free-for-all spawn pad (group
// 0, in the map's spawn list order; BUG_AUDIT.md "Spawn pads per map": Urban (Chinatown) has 93). Every spawn goes
// through sub_825ADF80 (character): Deathmatch picks the pad index with sub_8216A190 (participant; the free-for-all
// chooser: a random pad far from the others) and DarkOps through the mode's vtable +120, which in turn calls
// sub_8216A190 (DarkOps base 82421408, Duel 82425F80); the node is stored at character +4024, the position is the
// node's +16 and the facing comes from the node's +28 (Rare's own math, then sub_825AD148 move and sub_825AD3B0
// pitch / yaw for a human, type 2 at character +12). The option replaces sub_8216A190's answer during a human's
// spawn, so position and facing are the pad's, every run. Logged at each human spawn: 'pdz_test_mp: human spawn:
// pad ...' with the pad, position, the pad's +28 values and the yaw / pitch Rare applied, and whether the pin took
// (modes that never call sub_8216A190 for the human are reported as not pinned).
//
// pdz_test_mp_snapshot_s = N (default 0 = off; stage 2 M5 determinism check for compiler experiments, with
// pdz_random_seed, pdz_test_mp_setup and pdz_test_spawn_pad): every N seconds of match time one line
//   pdz_test_mp: snapshot #k t=<match s> swap <n> | H0: pos (x, y, z) kills K deaths D score S | B1: ...
//     | hash H khash C
// (every player / bot participant, humans first, then slot order), one more at match end ("end") and at test exit
// ("exit", only while the match still runs), and a 'pdz_test_mp: SUMMARY (<why>)' line at match end and at test
// exit (pdz::test::LogSummary): per participant kills / deaths / score / suicides and the kill matrix sums, the
// totals, the hash chain and every snapshot's hash. PDZRenderResearch native/tools/mpsnap_compare.py compares two
// logs. TU3 data read (no guest calls):
// - match clock 82E2C578 (ms; cut_scenarios.cpp kGameClock): the host's match-over check sub_8243B668 compares
//   (state +12548 == 5 ? now +0 : stop time +12560) - start +12556 with the time limit (settings +4408 minutes x
//   60000). Start / stop are set by sub_8243DB08 (match start) and sub_8243E108 (set elapsed). t = that elapsed ms
//   / 1000, so the snapshots follow the game's own match clock (which runs on the CPU timebase dt, real time).
// - swap: the game's frame counter 82D6BFD8 +136 (sub_82140500, one per main tick = one Swap) since snapshot #0.
// - participants: list 82E34224 (+4 next), +944 kind (2 player, 3 bot), +924 slot (0-63), +964 character (+48
//   position), +80 info. The info's netvars keep their value at +12: the kill bookkeeping sub_8243F498 (r3 killer,
//   r4 victim, r6 the one credited with the score) reads it there and writes through sub_824CCD80. +1084 score
//   (points: the end screen 823AEEE0, the points-limit check 82422188 and mp_announcer.cpp read it), +1188 kills
//   (+1 for the killer, also copied to the profile stats +6124), +1292 deaths (+1 for the victim; Duel's HUD
//   82425518 prints it as DEATHS), +1240 suicides (+1 with score -1; also DarkOps 82420790). Those writes are gated
//   on the participant's +12 object's +12 byte (inferred: the owning machine, so offline everyone). The participant
//   record's kill matrix, written unconditionally by sub_8243F498: killer +(32 + victim slot) * 4 += 1, victim
//   +(96 + killer slot) * 4 += 1; its sums are in the SUMMARY as a cross-check ("matrix kills/deaths").
// - hash: FNV-1a 32 over slot, kind, the position rounded to 0.1 units, kills, deaths and score of each participant
//   in line order; khash: the same without the position; chain / kchain: FNV-1a over the snapshot hashes in order.
// Read from the host's per-frame match-over check (mp_announcer.cpp's sub_8243B668 hook, game thread): a frame
// without a snapshot reads a few words and compares; nothing is allocated. The test exit runs in the Swap hook (game
// thread too).
#include <algorithm>
#include <array>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>

#include "../game_cvars.h"
#include "hook_profile.h" // pdz_hook_profile (must follow <rex/hook.h>)
#include "render/test_harness.h" // pdz::test::LogMpSnapshotSummary

REX_EXTERN(__imp__sub_823B8340); // option getter (id) -> value
REX_EXTERN(sub_823B8AD8);        // option setter (id, value): the hooked one (cut_scenarios.cpp)
REX_EXTERN(__imp__sub_823BB108); // level name -> LEVEL position
REX_EXTERN(sub_8248C658);        // local humans in the setup
REX_EXTERN(__imp__sub_823BD140);
REX_EXTERN(__imp__sub_823BD1A8);
REX_EXTERN(__imp__sub_825ADF80);
REX_EXTERN(__imp__sub_8216A190);
REX_EXTERN(__imp__sub_825AD3B0);

namespace
{
  constexpr uint32_t kOptionLevel = 1, kOptionPoints = 2, kOptionTime = 3, kOptionPlayersMax = 5;
  constexpr uint32_t kOptionBots = 19, kOptionMode = 61, kOptionScenario = 62, kOptionDarkOpsScenario = 63;
  constexpr uint32_t kOptionBotLimit = 77;
  constexpr int32_t kMaxPlayers = 32;

  constexpr uint32_t kPadManager = 0x82D6C068;      // -> pad manager
  constexpr uint32_t kPadSpawnList = 1100;          // pad manager: spawn list (count, entries)
  constexpr uint32_t kPadEntrySize = 820;           // entry: +0 node, +4 group (0 = free-for-all)
  constexpr uint32_t kNodePosition = 16;            // node: position (vec3)
  constexpr uint32_t kNodeAngles = 28;              // node: the facing data Rare turns into the yaw (vec3)
  constexpr uint32_t kParticipantList = 0x82E34224; // -> first participant (+4 next)
  constexpr uint32_t kParticipantCharacter = 964;   // participant: its character
  constexpr uint32_t kCharacterType = 12;           // character: 2 = human player
  constexpr uint32_t kCharacterPosition = 48;       // character: position (vec3)
  constexpr uint32_t kCharacterSpawnNode = 4024;    // character: the spawn node sub_825ADF80 used
  constexpr uint32_t kBotVtable = 0x820BF3B8;       // bot character class
  constexpr uint32_t kAnglesReturn = 0x825AE2B8;    // sub_825ADF80's call of sub_825AD3B0 (a human's spawn)

  uint32_t Load32(const uint8_t *base, uint32_t address)
  {
    const uint8_t *p = base + address;
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
  }

  float LoadFloat(const uint8_t *base, uint32_t address)
  {
    const uint32_t bits = Load32(base, address);
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
  }

  bool IsGuestPointer(uint32_t address)
  {
    return address >= 0x00010000u && address < 0xFFFF0000u;
  }

  int32_t Call(PPCContext &ctx, uint8_t *base, PPCFunc *fn, uint32_t r3, uint32_t r4 = 0)
  {
    rex::CallFrame frame(ctx);
    frame.ctx.r3.u64 = r3;
    frame.ctx.r4.u64 = r4;
    fn(frame.ctx, base);
    return int32_t(frame.ctx.r3.u32);
  }

  int32_t Get(PPCContext &ctx, uint8_t *base, uint32_t id)
  {
    return Call(ctx, base, __imp__sub_823B8340, id);
  }

  // --- pdz_test_mp_setup ---

  struct Level
  {
    const char *key;
    uint32_t name; // level name in the image (sub_823BB108)
    const char *menu;
  };
  constexpr Level kLevels[] = {
      {"subway", 0x82090EAC, "Subway"},   {"urban", 0x820A3DD4, "Urban"},       {"desert", 0x820A3D88, "Desert"},
      {"temple", 0x820CD47C, "Temple"},   {"oldtown", 0x820CD428, "Old Town"},  {"tower", 0x820CD498, "Tower"},
      {"facility", 0x820AD068, "Facility"}, {"ruin", 0x820AD088, "Ruin"},
  };

  struct Scenario
  {
    const char *key;
    int32_t mode; // option 61: 0 Deathmatch, 1 DarkOps
    int32_t value;
  };
  constexpr Scenario kScenarios[] = {
      {"dm", 0, 0},          {"killcount", 0, 0}, {"tkc", 0, 1},        {"teamkillcount", 0, 1},
      {"ctf", 0, 2},         {"tg", 0, 4},        {"territorial", 0, 4}, {"eradication", 1, 0},
      {"onslaught", 1, 1},   {"infection", 1, 2}, {"sabotage", 1, 3},  {"duel", 1, 4}, {"teamduel", 1, 4},
      {"ptf", 0, 3},         {"protection", 0, 5}, // the cut Deathmatch scenarios (pdz_mp_cut_scenarios)
  };

  struct Wanted
  {
    std::string text; // the cvar value this was parsed from
    bool active = false;
    int32_t bots = -1, points = -1, time = -1, teams = -1, skill = -1;
    const Level *level = nullptr; // map by name
    int32_t level_position = -1;  // map by number (a LEVEL position)
    const Scenario *scenario = nullptr;
  };

  std::mutex g_wanted_mutex;
  Wanted g_wanted;

  int32_t ParseInt(const std::string &text, bool &ok)
  {
    char *end = nullptr;
    const long v = std::strtol(text.c_str(), &end, 10);
    ok = !text.empty() && end && *end == '\0' && v >= 0 && v <= 1000;
    return int32_t(v);
  }

  // The parsed cvar (re-parsed when its text changes; errors logged once per text).
  Wanted CurrentWanted()
  {
    const std::string text = REXCVAR_GET(pdz_test_mp_setup);
    std::lock_guard<std::mutex> lock(g_wanted_mutex);
    if (text == g_wanted.text)
    {
      return g_wanted;
    }
    Wanted w;
    w.text = text;
    w.active = !text.empty();
    std::stringstream ss(text);
    std::string item;
    while (std::getline(ss, item, ','))
    {
      item.erase(std::remove_if(item.begin(), item.end(), [](unsigned char c) { return c == ' ' || c == '\t'; }),
                 item.end());
      std::transform(item.begin(), item.end(), item.begin(), [](unsigned char c) { return char(std::tolower(c)); });
      if (item.empty() || item == "fresh")
      {
        continue;
      }
      const size_t eq = item.find('=');
      const std::string key = item.substr(0, eq), value = eq == std::string::npos ? std::string() : item.substr(eq + 1);
      bool ok = false;
      if (key == "bots")
      {
        w.bots = std::min(ParseInt(value, ok), kMaxPlayers - 1);
      }
      else if (key == "points")
      {
        w.points = ParseInt(value, ok);
      }
      else if (key == "time")
      {
        w.time = ParseInt(value, ok);
      }
      else if (key == "teams")
      {
        w.teams = ParseInt(value, ok); // DARKVALUE_TEAM_COUNT (option 4)
      }
      else if (key == "skill")
      {
        w.skill = ParseInt(value, ok); // bot difficulty (option 20, as cut_scenarios.cpp's 'bot difficulty' line)
      }
      else if (key == "map")
      {
        for (const Level &l : kLevels)
        {
          if (value == l.key || (value == "chinatown" && std::strcmp(l.key, "urban") == 0) ||
              (value == "africa" && std::strcmp(l.key, "oldtown") == 0))
          {
            w.level = &l;
            ok = true;
          }
        }
        if (!ok)
        {
          w.level_position = ParseInt(value, ok); // a LEVEL position (map pack maps have no name string)
        }
      }
      else if (key == "scenario")
      {
        for (const Scenario &s : kScenarios)
        {
          if (value == s.key)
          {
            w.scenario = &s;
            ok = true;
          }
        }
      }
      if (!ok)
      {
        REXLOG_ERROR("pdz_test_mp: pdz_test_mp_setup item '{}' not understood (keys: bots, points, time, map, "
                     "scenario, teams, skill; e.g. bots=31,points=15,time=10,map=urban,scenario=dm)",
                     item);
      }
    }
    if (w.active)
    {
      REXLOG_INFO("pdz_test_mp: pdz_test_mp_setup '{}': the saved setup is ignored (Rare's presets); bots {}, points "
                  "{}, time {}, map {}, scenario {}, bot difficulty {}",
                  text, w.bots, w.points, w.time,
                  w.level ? w.level->menu : w.level_position >= 0 ? fmt::format("position {}", w.level_position) : "-",
                  w.scenario ? w.scenario->key : "-", w.skill);
    }
    g_wanted = w;
    return w;
  }

  thread_local bool t_applying = false;

  std::string MapName(PPCContext &ctx, uint8_t *base, int32_t position)
  {
    for (const Level &l : kLevels)
    {
      if (Call(ctx, base, __imp__sub_823BB108, l.name) == position)
      {
        return l.menu;
      }
    }
    return fmt::format("map {}", position);
  }

  // Sets the wanted values through the game's setter; one line with what changed.
  void Apply(PPCContext &ctx, uint8_t *base, const char *when)
  {
    if (t_applying)
    {
      return;
    }
    const Wanted w = CurrentWanted();
    if (!w.active)
    {
      return;
    }
    t_applying = true;
    std::string changes, refused;
    const auto set = [&](uint32_t id, const char *name, int32_t want) {
      const int32_t before = Get(ctx, base, id);
      if (before == want)
      {
        return;
      }
      Call(ctx, base, sub_823B8AD8, id, uint32_t(want));
      const int32_t after = Get(ctx, base, id);
      changes += fmt::format("{}{} {} -> {}", changes.empty() ? "" : ", ", name, before, after);
      if (after != want)
      {
        refused += fmt::format("{}{} asked {} kept {}", refused.empty() ? "" : ", ", name, want, after);
      }
    };
    if (w.scenario)
    {
      const int32_t mode = Get(ctx, base, kOptionMode);
      if (mode == w.scenario->mode)
      {
        set(mode == 0 ? kOptionScenario : kOptionDarkOpsScenario, "scenario", w.scenario->value);
      }
      else
      {
        refused += fmt::format("{}scenario {} skipped (mode {} chosen in the menu, {} wanted)",
                               refused.empty() ? "" : ", ", w.scenario->key, mode, w.scenario->mode);
      }
    }
    if (w.level || w.level_position >= 0)
    {
      const int32_t position = w.level ? Call(ctx, base, __imp__sub_823BB108, w.level->name) : w.level_position;
      if (position >= 0)
      {
        set(kOptionLevel, "level", position);
      }
    }
    if (w.bots >= 0)
    {
      const int32_t humans = std::max(1, Call(ctx, base, sub_8248C658, 0));
      const int32_t players = std::min(kMaxPlayers, w.bots + humans);
      if (Get(ctx, base, kOptionPlayersMax) < players)
      {
        set(kOptionPlayersMax, "max players", players);
      }
      if (Get(ctx, base, kOptionBotLimit) < w.bots)
      {
        set(kOptionBotLimit, "bot limit", w.bots);
      }
      set(kOptionBots, "bots", w.bots);
    }
    if (w.points >= 0)
    {
      set(kOptionPoints, "points", w.points);
    }
    if (w.time >= 0)
    {
      set(kOptionTime, "time", w.time);
    }
    if (w.teams >= 0)
    {
      set(4, "teams", w.teams);
    }
    if (w.skill >= 0)
    {
      set(20, "bot difficulty", w.skill);
    }
    t_applying = false;
    if (!changes.empty() || !refused.empty())
    {
      REXLOG_INFO("pdz_test_mp: {}: {}{}{}", when, changes.empty() ? std::string("nothing to change") : changes,
                  refused.empty() ? "" : " | NOT AS ASKED: ", refused);
    }
  }

  // --- spawn tracking (both cvars) and pdz_test_spawn_pad ---

  thread_local bool t_human_spawn = false; // inside sub_825ADF80 for a human character
  thread_local bool t_pinned = false;      // sub_8216A190's answer was replaced during it
  thread_local double t_pitch = 0.0, t_yaw = 0.0;
  thread_local bool t_angles = false;

  std::unordered_set<uint32_t> g_spawned; // characters spawned this match (game thread)
  bool g_all_logged = false;

  bool SpawnWatch()
  {
    return REXCVAR_GET(pdz_test_spawn_pad) >= 0 || !REXCVAR_GET(pdz_test_mp_setup).empty();
  }

  struct Pad
  {
    int32_t list_index = -1; // index in the spawn list (sub_8216A4B8's)
    uint32_t node = 0;
    uint32_t ffa_count = 0;  // group-0 pads on the map
  };

  // The n-th free-for-all (group 0) spawn pad; list_index -1 when the map has fewer.
  Pad FfaPad(const uint8_t *base, int32_t n)
  {
    Pad pad;
    const uint32_t manager = Load32(base, kPadManager);
    const uint32_t list = IsGuestPointer(manager) ? Load32(base, manager + kPadSpawnList) : 0;
    const uint32_t count = IsGuestPointer(list) ? Load32(base, list) : 0;
    const uint32_t entries = IsGuestPointer(list) ? Load32(base, list + 4) : 0;
    for (uint32_t i = 0; IsGuestPointer(entries) && i < count && i < 4096; ++i)
    {
      const uint32_t entry = entries + i * kPadEntrySize;
      if (base[entry + 4] != 0)
      {
        continue;
      }
      if (int32_t(pad.ffa_count) == n)
      {
        pad.list_index = int32_t(i);
        pad.node = Load32(base, entry);
      }
      ++pad.ffa_count;
    }
    return pad;
  }

  // The spawn list index and free-for-all number of a node (-1 if not a spawn pad).
  std::pair<int32_t, int32_t> PadOfNode(const uint8_t *base, uint32_t node)
  {
    const uint32_t manager = Load32(base, kPadManager);
    const uint32_t list = IsGuestPointer(manager) ? Load32(base, manager + kPadSpawnList) : 0;
    const uint32_t count = IsGuestPointer(list) ? Load32(base, list) : 0;
    const uint32_t entries = IsGuestPointer(list) ? Load32(base, list + 4) : 0;
    int32_t ffa = 0;
    for (uint32_t i = 0; IsGuestPointer(entries) && i < count && i < 4096; ++i)
    {
      const uint32_t entry = entries + i * kPadEntrySize;
      const bool free_for_all = base[entry + 4] == 0;
      if (Load32(base, entry) == node)
      {
        return {int32_t(i), free_for_all ? ffa : -1};
      }
      ffa += free_for_all ? 1 : 0;
    }
    return {-1, -1};
  }

  void OnSpawned(uint8_t *base, uint32_t character, bool human)
  {
    if (!IsGuestPointer(character))
    {
      return;
    }
    g_spawned.insert(character);
    if (g_all_logged && !human)
    {
      return;
    }
    // Participants and the characters spawned so far this match (counted at their spawn).
    uint32_t participants = 0, bots = 0, humans = 0, spawned = 0;
    for (uint32_t p = Load32(base, kParticipantList); IsGuestPointer(p) && participants < 130; p = Load32(base, p + 4))
    {
      ++participants;
      const uint32_t c = Load32(base, p + kParticipantCharacter);
      if (!IsGuestPointer(c) || !g_spawned.count(c))
      {
        continue;
      }
      ++spawned;
      if (Load32(base, c) == kBotVtable)
      {
        ++bots;
      }
      else if (Load32(base, c + kCharacterType) == 2)
      {
        ++humans;
      }
    }
    if (human)
    {
      const uint32_t node = Load32(base, character + kCharacterSpawnNode);
      const bool has_node = IsGuestPointer(node);
      const auto [list_index, ffa] = has_node ? PadOfNode(base, node) : std::pair<int32_t, int32_t>{-1, -1};
      const int32_t want = REXCVAR_GET(pdz_test_spawn_pad);
      const char *pin = want < 0 ? "no pin asked" : t_pinned && ffa == want ? "PINNED" : "NOT PINNED (Rare's pad)";
      const auto at = [&](uint32_t offset) { return has_node ? LoadFloat(base, node + offset) : 0.0f; };
      REXLOG_INFO("pdz_test_mp: human spawn: pad ffa {} (spawn list {}, node {:08X}) at ({:.1f}, {:.1f}, {:.1f}), pad "
                  "+28 ({:.4f}, {:.4f}, {:.4f}), yaw {} pitch {}; character at ({:.1f}, {:.1f}, {:.1f}); {} "
                  "(pdz_test_spawn_pad {}); spawned so far {} of {} participants ({} bots, {} humans)",
                  ffa, list_index, node, at(kNodePosition), at(kNodePosition + 4), at(kNodePosition + 8),
                  at(kNodeAngles), at(kNodeAngles + 4), at(kNodeAngles + 8),
                  t_angles ? fmt::format("{:.4f}", t_yaw) : std::string("n/a"),
                  t_angles ? fmt::format("{:.4f}", t_pitch) : std::string("n/a"),
                  LoadFloat(base, character + kCharacterPosition), LoadFloat(base, character + kCharacterPosition + 4),
                  LoadFloat(base, character + kCharacterPosition + 8), pin, want, spawned, participants, bots,
                  humans);
    }
    if (!g_all_logged && participants && spawned == participants)
    {
      g_all_logged = true;
      REXLOG_INFO("pdz_test_mp: all {} participants spawned: {} bots, {} humans{}", participants, bots, humans,
                  bots + humans == participants ? "" : fmt::format(", {} other", participants - bots - humans));
    }
  }
} // namespace

void PdzTestMpSnapshotReset(); // pdz_test_mp_snapshot_s (below)

// pdz_test_complete_mission (testing tool, off): the campaign level ends in success, the way Rare's level scripts end
// one: bit 0x01 (COMPLETED) of the level state byte 82E426F8 (0x02 = failed, 0x40 = checkpoint; Datacore's handler
// sets it at 822FFE1C). The game's own flow then scores the level, marks it complete in the profile and writes the
// save (sub_8250E090 -> sub_82508AF0 -> WriteSaveGame sub_82510E50). Set once each time the cvar turns on (an input
// script sets it once the level is in play); called from the game thread's music tick (music_restore.cpp).
REXCVAR_DEFINE_BOOL(pdz_test_complete_mission, false, "Testing",
                    "TESTING TOOL: when this turns on, the current campaign level ends in success as if every "
                    "objective were done (Rare's COMPLETED bit), so a scripted run can finish a mission and the game "
                    "saves its progress. For input scripts: 'cvar <t> pdz_test_complete_mission true' once in the level")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

void PdzTestCompleteMissionTick(uint8_t *base)
{
  static bool done = false;
  if (!REXCVAR_GET(pdz_test_complete_mission))
  {
    done = false;
    return;
  }
  if (done)
  {
    return;
  }
  done = true;
  constexpr uint32_t kLevelState = 0x82E426F8;
  const uint8_t before = base[kLevelState];
  base[kLevelState] = uint8_t(before | 0x01);
  REXLOG_INFO("pdz_test: complete mission (pdz_test_complete_mission): level state byte {:02X} -> {:02X} (COMPLETED)",
              before, base[kLevelState]);
}

// cut_scenarios.cpp's hook on sub_82432678 (the match start's pass over the setup options): before and after.
void PdzTestMpSetupMatchStart(PPCContext &ctx, uint8_t *base, bool after)
{
  if (!after)
  {
    PdzTestMpSnapshotReset();
    g_spawned.clear();
    g_all_logged = false;
    Apply(ctx, base, "match start (before the pass)");
    return;
  }
  if (!CurrentWanted().active)
  {
    return;
  }
  const int32_t level = Get(ctx, base, kOptionLevel);
  const int32_t mode = Get(ctx, base, kOptionMode);
  REXLOG_INFO("pdz_test_mp: match start: in effect: bots {}, points {}, time {}, max players {}, bot limit {}, humans "
              "{}, mode {} scenario {}, map {} (LEVEL {})",
              Get(ctx, base, kOptionBots), Get(ctx, base, kOptionPoints), Get(ctx, base, kOptionTime),
              Get(ctx, base, kOptionPlayersMax), Get(ctx, base, kOptionBotLimit), Call(ctx, base, sub_8248C658, 0),
              mode, Get(ctx, base, mode == 1 ? kOptionDarkOpsScenario : kOptionScenario), MapName(ctx, base, level),
              level);
  // The game's level list (TU3 82D6C2B4, what the DLC merge 8227A648 appends to: +0 count, +8 entries; an entry is
  // name +0, id +4, kind +5), its multiplayer levels by name and id: a map listed twice shows here.
  constexpr uint32_t kLevelList = 0x82D6C2B4;
  const uint32_t count = Load32(base, kLevelList), entries = Load32(base, kLevelList + 8);
  std::string mp;
  uint32_t mp_count = 0;
  for (uint32_t i = 0; IsGuestPointer(entries) && i < count && i < 256; ++i)
  {
    const uint32_t e = Load32(base, entries + i * 4);
    if (!IsGuestPointer(e) || base[e + 5] != 1)
    {
      continue;
    }
    const uint32_t name = Load32(base, e);
    std::string n;
    for (uint32_t k = 0; IsGuestPointer(name) && k < 64 && base[name + k]; ++k)
    {
      n += char(base[name + k]);
    }
    mp += fmt::format(" {}({:02X})", n.rfind("aid_xml_level_", 0) == 0 ? n.substr(14) : n, base[e + 4]);
    ++mp_count;
  }
  REXLOG_INFO("pdz_test_mp: level list: {} levels, {} multiplayer:{}", count, mp_count, mp);
}

// sub_823BD140(): the setup's load after Rare's defaults (sub_823BD8D8): the profile's saved setup for the mode /
// scenario, else Rare's presets. Called when a setup screen opens (823D3548, 823FF19C, 823FF5E4, 823BD104,
// 823BD12C). The wanted values are set after it.
REX_HOOK_RAW(sub_823BD140)
{
  PPCContext saved = ctx;
  __imp__sub_823BD140(ctx, base);
  Apply(saved, base, "setup loaded");
}

// sub_823BD1A8(): whether the profile holds a saved setup for the current mode / scenario (only caller
// sub_823BD140). pdz_test_mp_setup: never (Rare's presets, as on a fresh profile).
REX_HOOK_RAW(sub_823BD1A8)
{
  __imp__sub_823BD1A8(ctx, base);
  if (ctx.r3.u32 != 0 && CurrentWanted().active)
  {
    static bool logged = false;
    if (!logged)
    {
      logged = true;
      REXLOG_INFO("pdz_test_mp: the profile's saved setup for this mode / scenario is ignored (pdz_test_mp_setup)");
    }
    ctx.r3.u64 = 0;
  }
}

// sub_825ADF80(character): a spawn (initial or respawn), every mode.
REX_HOOK_RAW(sub_825ADF80)
{
  const uint32_t character = ctx.r3.u32;
  if (!SpawnWatch() || !IsGuestPointer(character))
  {
    __imp__sub_825ADF80(ctx, base);
    return;
  }
  const bool human = Load32(base, character + kCharacterType) == 2 && Load32(base, character) != kBotVtable;
  t_human_spawn = human;
  t_pinned = false;
  t_angles = false;
  __imp__sub_825ADF80(ctx, base);
  t_human_spawn = false;
  OnSpawned(base, character, human);
}

// sub_8216A190(participant) -> spawn list index: the free-for-all (or team group) pad chooser. During a human's
// spawn with pdz_test_spawn_pad: the pinned pad instead.
REX_HOOK_RAW(sub_8216A190)
{
  __imp__sub_8216A190(ctx, base);
  if (!t_human_spawn)
  {
    return;
  }
  const int32_t want = REXCVAR_GET(pdz_test_spawn_pad);
  if (want < 0)
  {
    return;
  }
  const Pad pad = FfaPad(base, want);
  if (pad.list_index < 0)
  {
    static bool warned = false;
    if (!warned)
    {
      warned = true;
      REXLOG_WARN("pdz_test_mp: pdz_test_spawn_pad {}: the map has {} free-for-all spawn pads (0-{}): Rare's pad used",
                  want, pad.ffa_count, int32_t(pad.ffa_count) - 1);
    }
    return;
  }
  ctx.r3.u64 = uint32_t(pad.list_index);
  t_pinned = true;
}

// sub_825AD3B0(character, f1 pitch, f2 yaw): a player's look angles; from a human's spawn (return 825AE2B8) the
// values are kept for the spawn line.
REX_HOOK_RAW(sub_825AD3B0)
{
  if (t_human_spawn && uint32_t(ctx.lr) == kAnglesReturn)
  {
    t_pitch = ctx.f1.f64;
    t_yaw = ctx.f2.f64;
    t_angles = true;
  }
  __imp__sub_825AD3B0(ctx, base);
}

// --- pdz_test_mp_snapshot_s (determinism check for compiler experiments) ---

namespace
{
  constexpr uint32_t kMatchClock = 0x82E2C578; // ms (cut_scenarios.cpp kGameClock)
  constexpr uint32_t kMatchState = 12548;      // match clock block: 5 = the match clock runs
  constexpr uint32_t kMatchStart = 12556;      // ms at the match start
  constexpr uint32_t kMatchEnd = 12560;        // ms the clock stopped at (state != 5)
  constexpr uint32_t kGameBlock = 0x82D6BFD8;  // +0 -> game state (+40 game type: 2 / 3 multiplayer)
  constexpr uint32_t kFrameCounter = kGameBlock + 136;
  constexpr uint32_t kParticipantSlot = 924; // 0-63
  constexpr uint32_t kParticipantKind = 944; // 2 player, 3 bot
  constexpr uint32_t kParticipantInfo = 80;
  constexpr uint32_t kInfoScore = 1084, kInfoKills = 1188, kInfoSuicides = 1240, kInfoDeaths = 1292;
  constexpr uint32_t kNetvarValue = 12;
  constexpr uint32_t kMatrixKills = 32 * 4, kMatrixDeaths = 96 * 4; // participant: int32[64] by slot, each
  constexpr size_t kMaxSnapParticipants = 64;
  constexpr uint32_t kFnvBasis = 2166136261u;

  struct SnapEntry
  {
    int32_t slot = 0, kind = 0;
    bool has_pos = false;
    float x = 0, y = 0, z = 0;
    int32_t kills = 0, deaths = 0, score = 0, suicides = 0, matrix_kills = 0, matrix_deaths = 0;
  };

  struct SnapState
  {
    bool running = false;  // snapshot #0 taken this match
    bool ended = false;    // the match-over snapshot taken
    uint32_t frames0 = 0;  // frame counter at snapshot #0
    uint32_t start_ms = 0; // the match start the series belongs to
    uint32_t next = 0;     // next periodic snapshot index
    double last_t = 0.0;
    std::array<SnapEntry, kMaxSnapParticipants> last{};
    size_t last_count = 0;
    uint32_t chain = kFnvBasis, kchain = kFnvBasis;
    std::vector<uint32_t> hashes; // per snapshot (grows on snapshot frames only)
    std::vector<std::string> labels;
  };
  std::mutex g_snap_mutex;
  SnapState g_snap;
  uint8_t *g_snap_base = nullptr; // guest memory, kept for the test exit

  void Fnv(uint32_t &h, uint32_t v)
  {
    for (int i = 0; i < 4; ++i)
    {
      h ^= (v >> (i * 8)) & 0xFFu;
      h *= 16777619u;
    }
  }

  int32_t Tenths(float v)
  {
    return std::isfinite(v) ? int32_t(std::lround(double(v) * 10.0)) : INT32_MIN;
  }

  int32_t Netvar(const uint8_t *base, uint32_t info, uint32_t offset)
  {
    return IsGuestPointer(info) ? int32_t(Load32(base, info + offset + kNetvarValue)) : 0;
  }

  bool MatchMultiplayer(const uint8_t *base)
  {
    const uint32_t game = Load32(base, kGameBlock);
    const uint32_t type = IsGuestPointer(game) ? Load32(base, game + 40) : 0;
    return type == 2 || type == 3;
  }

  // Elapsed match ms, as the match-over check computes it for the time limit.
  uint32_t MatchMs(const uint8_t *base)
  {
    const bool runs = Load32(base, kMatchClock + kMatchState) == 5;
    const uint32_t now = Load32(base, kMatchClock + (runs ? 0 : kMatchEnd));
    const uint32_t start = Load32(base, kMatchClock + kMatchStart);
    return now >= start ? now - start : 0;
  }

  // Every player / bot participant into s.last: humans first, then slot order.
  void ReadParticipants(const uint8_t *base, SnapState &s)
  {
    s.last_count = 0;
    uint32_t guard = 0;
    for (uint32_t p = Load32(base, kParticipantList); IsGuestPointer(p) && guard < 130 &&
                                                      s.last_count < kMaxSnapParticipants;
         p = Load32(base, p + 4), ++guard)
    {
      const int32_t kind = int32_t(Load32(base, p + kParticipantKind));
      if (kind != 2 && kind != 3)
      {
        continue;
      }
      SnapEntry &e = s.last[s.last_count++];
      e = SnapEntry{};
      e.kind = kind;
      e.slot = int32_t(Load32(base, p + kParticipantSlot));
      const uint32_t c = Load32(base, p + kParticipantCharacter);
      e.has_pos = IsGuestPointer(c);
      if (e.has_pos)
      {
        e.x = LoadFloat(base, c + kCharacterPosition);
        e.y = LoadFloat(base, c + kCharacterPosition + 4);
        e.z = LoadFloat(base, c + kCharacterPosition + 8);
      }
      const uint32_t info = Load32(base, p + kParticipantInfo);
      e.kills = Netvar(base, info, kInfoKills);
      e.deaths = Netvar(base, info, kInfoDeaths);
      e.score = Netvar(base, info, kInfoScore);
      e.suicides = Netvar(base, info, kInfoSuicides);
      for (uint32_t j = 0; j < 64; ++j)
      {
        e.matrix_kills += int32_t(Load32(base, p + kMatrixKills + j * 4));
        e.matrix_deaths += int32_t(Load32(base, p + kMatrixDeaths + j * 4));
      }
    }
    std::sort(s.last.begin(), s.last.begin() + std::ptrdiff_t(s.last_count),
              [](const SnapEntry &a, const SnapEntry &b) {
                return (a.kind == 2) != (b.kind == 2) ? a.kind == 2 : a.slot < b.slot;
              });
  }

  char KindLetter(const SnapEntry &e)
  {
    return e.kind == 2 ? 'H' : 'B';
  }

  void TakeSnapshot(const uint8_t *base, SnapState &s, const std::string &label, uint32_t ms)
  {
    ReadParticipants(base, s);
    uint32_t hash = kFnvBasis, khash = kFnvBasis;
    std::string line;
    line.reserve(96 + s.last_count * 96);
    s.last_t = double(ms) / 1000.0;
    fmt::format_to(std::back_inserter(line), "pdz_test_mp: snapshot {} t={:.3f} swap {}", label, s.last_t,
                   Load32(base, kFrameCounter) - s.frames0);
    for (size_t i = 0; i < s.last_count; ++i)
    {
      const SnapEntry &e = s.last[i];
      const int32_t qx = e.has_pos ? Tenths(e.x) : INT32_MIN;
      const int32_t qy = e.has_pos ? Tenths(e.y) : INT32_MIN;
      const int32_t qz = e.has_pos ? Tenths(e.z) : INT32_MIN;
      for (const int32_t v : {e.slot, e.kind, qx, qy, qz, e.kills, e.deaths, e.score})
      {
        Fnv(hash, uint32_t(v));
      }
      for (const int32_t v : {e.slot, e.kind, e.kills, e.deaths, e.score})
      {
        Fnv(khash, uint32_t(v));
      }
      if (e.has_pos)
      {
        fmt::format_to(std::back_inserter(line), " | {}{}: pos ({:.1f}, {:.1f}, {:.1f}) kills {} deaths {} score {}",
                       KindLetter(e), e.slot, e.x, e.y, e.z, e.kills, e.deaths, e.score);
      }
      else
      {
        fmt::format_to(std::back_inserter(line), " | {}{}: pos - kills {} deaths {} score {}", KindLetter(e), e.slot,
                       e.kills, e.deaths, e.score);
      }
    }
    Fnv(s.chain, hash);
    Fnv(s.kchain, khash);
    s.hashes.push_back(hash);
    s.labels.push_back(label);
    fmt::format_to(std::back_inserter(line), " | hash {:08x} khash {:08x}", hash, khash);
    REXLOG_INFO("{}", line);
  }

  void LogSnapshotSummary(const SnapState &s, const char *why)
  {
    const int32_t every_s = REXCVAR_GET(pdz_test_mp_snapshot_s);
    if (s.hashes.empty())
    {
      REXLOG_INFO("pdz_test_mp: SUMMARY ({}): no snapshot taken (no multiplayer match clock ran; "
                  "pdz_test_mp_snapshot_s {})",
                  why, every_s);
      return;
    }
    std::string line = fmt::format("pdz_test_mp: SUMMARY ({}): {} snapshots every {} s, last t={:.3f}{}, chain "
                                   "{:08x} kchain {:08x}",
                                   why, s.hashes.size(), every_s, s.last_t, s.ended ? " (match over)" : "", s.chain,
                                   s.kchain);
    int32_t kills = 0, deaths = 0, suicides = 0, mk = 0, md = 0;
    for (size_t i = 0; i < s.last_count; ++i)
    {
      const SnapEntry &e = s.last[i];
      fmt::format_to(std::back_inserter(line), " | {}{}: kills {} deaths {} score {} suicides {} matrix {}/{}",
                     KindLetter(e), e.slot, e.kills, e.deaths, e.score, e.suicides, e.matrix_kills,
                     e.matrix_deaths);
      kills += e.kills;
      deaths += e.deaths;
      suicides += e.suicides;
      mk += e.matrix_kills;
      md += e.matrix_deaths;
    }
    fmt::format_to(std::back_inserter(line), " | total: {} participants, kills {} deaths {} suicides {} matrix {}/{}",
                   s.last_count, kills, deaths, suicides, mk, md);
    line += " | hashes";
    for (size_t i = 0; i < s.hashes.size(); ++i)
    {
      fmt::format_to(std::back_inserter(line), " {}={:08x}", s.labels[i], s.hashes[i]);
    }
    REXLOG_INFO("{}", line);
  }
} // namespace

// mp_announcer.cpp's sub_8243B668 hook (the host's per-frame match-over check, game thread), after Rare's code:
// over = its answer (1 when the match is over).
void PdzTestMpSnapshotTick(uint8_t *base, bool over)
{
  const int32_t every_s = REXCVAR_GET(pdz_test_mp_snapshot_s);
  if (every_s <= 0)
  {
    return;
  }
  std::lock_guard<std::mutex> lock(g_snap_mutex);
  SnapState &s = g_snap;
  g_snap_base = base;
  if (s.ended || !MatchMultiplayer(base))
  {
    return;
  }
  const uint32_t start = Load32(base, kMatchClock + kMatchStart);
  if (s.running && start != s.start_ms)
  {
    // The match clock restarted without a match start pass in between: a new series.
    REXLOG_INFO("pdz_test_mp: snapshot: the match clock restarted (start {} -> {} ms): new snapshot series",
                s.start_ms, start);
    s = SnapState{};
  }
  if (!s.running)
  {
    if (Load32(base, kMatchClock + kMatchState) != 5 && !over)
    {
      return; // the match clock is not running yet
    }
    s.running = true;
    s.start_ms = start;
    s.frames0 = Load32(base, kFrameCounter);
    s.hashes.reserve(256);
    s.labels.reserve(256);
  }
  const uint32_t ms = MatchMs(base);
  if (over)
  {
    s.ended = true;
    TakeSnapshot(base, s, "end", ms);
    LogSnapshotSummary(s, "match end");
    return;
  }
  const uint32_t period_ms = uint32_t(every_s) * 1000u;
  if (ms < s.next * period_ms)
  {
    return;
  }
  TakeSnapshot(base, s, fmt::format("#{}", s.next), ms);
  s.next = ms / period_ms + 1; // a long frame skips a due time instead of doubling up
}

// The match start pass (PdzTestMpSetupMatchStart, before): a new series for the coming match.
void PdzTestMpSnapshotReset()
{
  std::lock_guard<std::mutex> lock(g_snap_mutex);
  g_snap = SnapState{};
}

namespace pdz::test
{
  // pdz::test::LogSummary (test exit; the Swap hook, game thread): a last snapshot ("exit") while the match still
  // runs, then the SUMMARY line.
  void LogMpSnapshotSummary(const char *why)
  {
    if (REXCVAR_GET(pdz_test_mp_snapshot_s) <= 0)
    {
      return;
    }
    std::lock_guard<std::mutex> lock(g_snap_mutex);
    SnapState &s = g_snap;
    const uint8_t *base = g_snap_base;
    if (base && s.running && !s.ended && MatchMultiplayer(base) &&
        Load32(base, kMatchClock + kMatchStart) == s.start_ms)
    {
      TakeSnapshot(base, s, "exit", MatchMs(base));
    }
    LogSnapshotSummary(s, why);
  }
} // namespace pdz::test
