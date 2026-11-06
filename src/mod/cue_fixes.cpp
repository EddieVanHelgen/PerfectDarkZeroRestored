// Speech and sound names (pdz_fix_speech_names, pdz_fix_sound_names; game
// fixes, on by default, need pdz_game_fixes). tu3-base only; TU3 addresses.
//
// Every cue the game plays by name goes through sub_82202D20 [821FAC08]
// (ref, name), whose only resolver is sub_82202C78 [821FAB60] (the name less
// "XACT_SOUNDBANK_" in the loaded banks' name table; -1 if none). A few names
// the code or the level data ask for have no exact match, but Rare recorded
// the line or sound under another name (docs/SP_INVENTORY.md, sections 2 and
// 3; scratchpad sp/cue_missing.txt). These are looked up instead:
// - Datacore's level script (sub_822ECD00 and others, table 82D6C8B8) asks
//   for LV00_SPEECH_SCIENTIST / TROOPER_SHOOT_SPDRBOT_SOLO / _TEAM; the
//   lines were recorded in the speakers' voice banks as
//   lvl0_shooting_spiderbots_solo / _team (scientists SCI1 / SCI2, troopers
//   TROOPER1-3);
// - Nightclub's gangster script (sub_822AA518) asks for
//   GANGSTA1_FIRE_ALARMED_TRIGGERED; the line is fire_alarm_triggered;
// - Jungle's script (sub_82308F58) asks for TROOPERX_SEE_ENEMY_SQUAD_TEAM,
//   a placeholder voice; TROOPER1-3 recorded see_enemy_squad_team;
// - Urban's level data asks for MUSIC_CHN_CHINA_AMBI_SPAWN (its spawn-room
//   ambience); the bank is music_mlt_chn, as on every other map;
// - level data asks for BULLETHIT_WOOD (no bank name: GUNS_BULLETHIT_WOOD)
//   and GUNS_HITFLESH (GUNS_HIT_FLESH).
// Added 2026-10-22 (docs/TU3_BASE.md "Restoration build plan"):
// - G1: the metal table prop template asks for
//   XACT_SOXACT_SOUNDBANK_GUNS_BULLETHIT_METAL (prefix typed twice), so its
//   bullet hits were silent;
// - G2: Bridge's Institute squad (sub_82294680) picks INS_TROOPER_02 / _03,
//   never recorded; ins_trooper_04 is the arrival line;
// - G3: Trinity Escape's level data asks for STOCK_FLARE_BURNING_LP; the
//   bank has STOCK_FLARE_BURNING;
// - G4: the "they missed me" line for a male shooter: the builder asks for
//   <voice>_BEEN_SHOT_MISS_AWARE_TEAM_MALE, recorded as
//   <voice>_BEEN_SHOT_MISS_AWARE_MALE (every voice that has it);
// - G5: elite1's PLAYER_RETREATS_TEAM was saved with a DEL byte (0x7F) in
//   place of the second underscore;
// - G7: Mansion's voice check after the satellite hack (EndGuardQuestioning
//   node 11) plays JO_J / JO_I (never recorded) or JO_H (a Mai Hem match
//   line); Jo's own "My voice is my invite." is GUARD_S, as node 0 uses.
// Not changed: Sabotage's ExplosivesPlaced message (25, handler 82429520)
// asks for T_XACT_SOUNDBANK_MULTIPLAYER_DO_EXPLOSIVES_SET (820B6A74, a stray
// "T_"), so in retail it never plays; the plant start (825F05E0, state 3)
// plays the same line by its right name. A remap (2026-10-24) made every
// plant say it twice; reverted to retail (2026-10-24).
// Where several voices recorded a line, one that resolves in the loaded
// banks is picked at random. Each remap is logged once.
// Not changed (no clear counterpart): Laboratory's TANNOY_SECURITY07 (area
// unknown), Mansion's GUARD_L, Subway's CHAN_25, TRAN_RADIO_INTERFERENCE,
// Trinity Escape's INS_SQUAD_JONATHAN_JO_BLOW_DOOR, CRATE_WOOD_PUSH, the lab
// door sounds, <voice>_FRIEND_DIES_MALE (event 109 passes the killer's sex,
// so the target take isn't confirmed; plan item G6).
#include "cue_fixes.h"

#include <cstring>
#include <mutex>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>

#include "../game_cvars.h"
#include "audio_presence_log.h"
#include "fix_log.h"
#include "hook_profile.h" // pdz_hook_profile (must follow <rex/hook.h>)

REX_EXTERN(sub_82202C78);

namespace
{
  struct Remap
  {
    bool speech;                        // pdz_fix_speech_names, else pdz_fix_sound_names
    const char *what;                   // for the log
    std::vector<std::string> targets;   // recorded names (full XACT names)
    std::vector<uint32_t> guest;        // the same, as guest strings
    bool logged = false;
  };

  std::mutex g_mutex;
  std::unordered_map<std::string, Remap> g_remaps;
  std::mt19937 g_rng{std::random_device{}()};

  // G1's name, prefix typed twice. The resolver skips 15 characters, so it
  // looks up "NDBANK_GUNS_BULLETHIT_METAL" and finds nothing.
  constexpr const char *kMetalTableName = "XACT_SOXACT_SOUNDBANK_GUNS_BULLETHIT_METAL";

  void Add(const char *from, bool speech, const char *what, std::vector<std::string> to)
  {
    Remap r;
    r.speech = speech;
    r.what = what;
    r.targets = std::move(to);
    g_remaps.emplace(std::string("XACT_SOUNDBANK_") + from, std::move(r));
  }

  std::vector<std::string> Voices(std::initializer_list<const char *> banks, const char *line)
  {
    std::vector<std::string> out;
    for (const char *bank : banks)
    {
      out.push_back(std::string("XACT_SOUNDBANK_") + bank + "_" + line);
    }
    return out;
  }

  void Build()
  {
    if (!g_remaps.empty())
    {
      return;
    }
    const auto scientists = {"SCI1", "SCI2"};
    const auto troopers = {"TROOPER1", "TROOPER2", "TROOPER3"};
    Add("LV00_SPEECH_SCIENTIST_SHOOT_SPDRBOT_SOLO", true, "Datacore scientist spiderbot line",
        Voices(scientists, "LVL0_SHOOTING_SPIDERBOTS_SOLO"));
    Add("LV00_SPEECH_SCIENTIST_SHOOT_SPDRBOT_TEAM", true, "Datacore scientist spiderbot line",
        Voices(scientists, "LVL0_SHOOTING_SPIDERBOTS_TEAM"));
    Add("LV00_SPEECH_TROOPER_SHOOT_SPDRBOT_SOLO", true, "Datacore trooper spiderbot line",
        Voices(troopers, "LVL0_SHOOTING_SPIDERBOTS_SOLO"));
    Add("LV00_SPEECH_TROOPER_SHOOT_SPDRBOT_TEAM", true, "Datacore trooper spiderbot line",
        Voices(troopers, "LVL0_SHOOTING_SPIDERBOTS_TEAM"));
    // GANGSTA1 first; the other gangster voices recorded it too (log 062:
    // GANGSTA1's didn't resolve when the alarm went off).
    Add("GANGSTA1_FIRE_ALARMED_TRIGGERED", true, "Nightclub gangster fire alarm line",
        Voices({"GANGSTA1", "GANGSTA2", "GANGSTA3"}, "FIRE_ALARM_TRIGGERED"));
    Add("TROOPERX_SEE_ENEMY_SQUAD_TEAM", true, "Jungle trooper squad line",
        Voices(troopers, "SEE_ENEMY_SQUAD_TEAM"));
    Add("MUSIC_CHN_CHINA_AMBI_SPAWN", false, "Urban spawn-room ambience",
        {"XACT_SOUNDBANK_MUSIC_MLT_CHN_CHINA_AMBI_SPAWN"});
    Add("BULLETHIT_WOOD", false, "bullet hit on wood", {"XACT_SOUNDBANK_GUNS_BULLETHIT_WOOD"});
    Add("GUNS_HITFLESH", false, "bullet hit on flesh", {"XACT_SOUNDBANK_GUNS_HIT_FLESH"});
    // G1: the whole name is misspelt, so it's keyed in full (see CueName).
    {
      Remap r;
      r.speech = false;
      r.what = "metal table bullet hit (G1)";
      r.targets = {"XACT_SOUNDBANK_GUNS_BULLETHIT_METAL"};
      g_remaps.emplace(kMetalTableName, std::move(r));
    }
    Add("STOCK_FLARE_BURNING_LP", false, "Trinity Escape flare (G3)", {"XACT_SOUNDBANK_STOCK_FLARE_BURNING"});
    Add("LV13_SPEECH_INS_TROOPER_02", true, "Bridge Institute squad arrival (G2)",
        {"XACT_SOUNDBANK_LV13_SPEECH_INS_TROOPER_04"});
    Add("LV13_SPEECH_INS_TROOPER_03", true, "Bridge Institute squad arrival (G2)",
        {"XACT_SOUNDBANK_LV13_SPEECH_INS_TROOPER_04"});
    for (const char *voice : {"ELITE1", "ELITE2", "ELITE3", "GANGSTA1", "GANGSTA2", "GANGSTA3", "JON", "SCI1",
                              "SCI2", "TROOPER1", "TROOPER2", "TROOPER3", "WOMAN1", "WOMAN2"})
    {
      const std::string from = std::string(voice) + "_BEEN_SHOT_MISS_AWARE_TEAM_MALE";
      Add(from.c_str(), true, "\"they missed me\", male shooter (G4)",
          {std::string("XACT_SOUNDBANK_") + voice + "_BEEN_SHOT_MISS_AWARE_MALE"});
    }
    Add("ELITE1_PLAYER_RETREATS_TEAM", true, "elite1 \"she's falling back\" (G5)",
        {"XACT_SOUNDBANK_ELITE1_PLAYER_RETREATS\x7fTEAM"});
    for (const char *jo : {"LV04_SPEECH_JO_J", "LV04_SPEECH_JO_I", "LV04_SPEECH_JO_H"})
    {
      Add(jo, true, "Mansion voice check after the hack (G7)", {"XACT_SOUNDBANK_LV04_SPEECH_GUARD_S"});
    }
  }

  uint32_t GuestString(uint8_t *base, const std::string &text)
  {
    const uint32_t address = REX_KERNEL_MEMORY()->SystemHeapAlloc(uint32_t(text.size() + 1));
    if (address)
    {
      std::memcpy(base + address, text.c_str(), text.size() + 1);
    }
    return address;
  }

  int32_t Resolve(PPCContext &ctx, uint8_t *base, uint32_t name)
  {
    rex::CallFrame frame(ctx);
    frame.ctx.r3.u64 = name;
    sub_82202C78(frame.ctx, base);
    return int32_t(frame.ctx.r3.u32);
  }
} // namespace

namespace cue_fixes
{
  uint32_t CueName(PPCContext &ctx, uint8_t *base, uint32_t name)
  {
    if (!name || !REXCVAR_GET(pdz_game_fixes) ||
        (!REXCVAR_GET(pdz_fix_speech_names) && !REXCVAR_GET(pdz_fix_sound_names)))
    {
      return name;
    }
    // Cheap reject: every remapped name is an XACT name (G1's has its prefix
    // typed twice).
    const char *text = reinterpret_cast<const char *>(base + name);
    if (std::strncmp(text, "XACT_SOUNDBANK_", 15) != 0 && std::strncmp(text, "XACT_SOXACT_", 12) != 0)
    {
      return name;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    Build();
    const auto found = g_remaps.find(std::string(text, strnlen(text, 128)));
    if (found == g_remaps.end())
    {
      return name;
    }
    Remap &r = found->second;
    if (!REXCVAR_GET(pdz_fix_speech_names) && r.speech)
    {
      return name;
    }
    if (!REXCVAR_GET(pdz_fix_sound_names) && !r.speech)
    {
      return name;
    }
    if (r.guest.empty())
    {
      for (const std::string &t : r.targets)
      {
        r.guest.push_back(GuestString(base, t));
      }
    }
    // The recorded versions that the loaded banks have.
    std::vector<size_t> usable;
    for (size_t i = 0; i < r.guest.size(); ++i)
    {
      // -1 is "not found"; real cue handles have the top bit set (log 063:
      // GANGSTA1's resolved to 0x80C7xxxx), so don't test for >= 0.
      if (r.guest[i] && Resolve(ctx, base, r.guest[i]) != -1)
      {
        usable.push_back(i);
      }
    }
    if (usable.empty())
    {
      if (!r.logged)
      {
        r.logged = true;
        // Why: each candidate's resolve result, and the asked-for name's own
        // (-1 = not in the loaded banks' name table, or its bank not loaded).
        std::string tried;
        for (size_t i = 0; i < r.guest.size(); ++i)
        {
          tried += fmt::format("{}{} {}", i ? ", " : "", r.targets[i].substr(15),
                               r.guest[i] ? Resolve(ctx, base, r.guest[i]) : -2);
        }
        REXLOG_INFO("pdz_fix: {} ({}, itself {}): no recorded version in the loaded banks, left as is (tried {})",
                    r.what, found->first.substr(15), Resolve(ctx, base, name), tried);
      }
      return name;
    }
    const size_t pick = usable[std::uniform_int_distribution<size_t>(0, usable.size() - 1)(g_rng)];
    if (!r.logged)
    {
      r.logged = true;
      REXLOG_INFO("pdz_fix: {} {} -> {} ({} of {} recorded versions loaded)", r.what, found->first.substr(15),
                  r.targets[pick].substr(15), usable.size(), r.targets.size());
    }
    fix_log::Count(r.speech ? "speech name" : "sound name");
    return r.guest[pick];
  }
} // namespace cue_fixes

// --- pdz_sound_log (debug, off by default) ---
// sub_82205C68 (DarkAudioSound::Prime) is where every sound the sound manager
// starts is prepared (callers sub_822061C8 / sub_821FF428; it hands the sound
// to DarkAudioSoundManager::RequestToPlay, sub_82203328). The sound keeps its
// name at +52 (the reference sub_82202D20 fills: name, then the resolved cue
// at +308, -1 when no loaded bank has it). Each name is logged once per
// session: pdz_sound: <BANK>_<CUE>, from the XACT name.
REX_EXTERN(__imp__sub_82205C68);

REX_HOOK_RAW(sub_82205C68)
{
  if (REXCVAR_GET(pdz_sound_log) && ctx.r3.u32)
  {
    static std::mutex mutex;
    static std::unordered_map<std::string, bool> seen;
    const uint32_t sound = ctx.r3.u32;
    const char *text = reinterpret_cast<const char *>(base + sound + 52);
    std::string name(text, strnlen(text, 255));
    if (name.rfind("XACT_SOUNDBANK_", 0) == 0)
    {
      name.erase(0, 15);
    }
    const int32_t cue = int32_t(
        (uint32_t(base[sound + 308]) << 24) | (uint32_t(base[sound + 309]) << 16) |
        (uint32_t(base[sound + 310]) << 8) | uint32_t(base[sound + 311]));
    std::lock_guard<std::mutex> lock(mutex);
    if (!name.empty() && seen.emplace(name, true).second)
    {
      REXLOG_INFO("pdz_sound: {}{}", name, cue == -1 ? " (no loaded bank has it)" : "");
    }
  }
  const uint32_t sound = ctx.r3.u32;
  __imp__sub_82205C68(ctx, base);
  audio_presence_log::OnPrime(base, sound, ctx.r3.u32); // pdz_audio_presence_log
}
