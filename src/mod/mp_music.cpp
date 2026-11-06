// Multiplayer match music (pdz_mp_music, enhancement; the in-game Music value
// defaults to Off).
//
// tu3-base: Title Update 3 addresses throughout (every function below maps to
// a launch one; the launch addresses are in git history). Differences from
// the launch branch: TU3's sound bank table has 149 entries (launch 85), the
// game mode is game state +40 (launch +36), the value is saved in the user
// data root (no local profiles here), the stop on leaving a level hooks TU3's
// audio idle check here (launch: dlc_maps.cpp), the tick is called from
// src/music/music_restore.cpp (the restored player's tick), and there is no
// pause / resume: multiplayer matches never pause the music (as on launch).
//
// The original multiplayer only plays each map's ambience; this plays one of the game's own soundtrack pieces over it: Off, Random or a
// chosen track. With the value Off (or pdz_mp_music false) nothing here runs.
//
// - Tracks: every soundtrack cue is one stereo wave in a level's music bank
//   (music_lvNN.xsb / streaming .xwb) or a menu bank, authored to loop for
//   ever (intro once, then a loop region), except the title piece. The
//   interactive levels keep each layer (Stealth, Speedup) as its own full mix,
//   so a layer's cue is a fixed full mix. Several cues stay silent unless their
//   Ambivol variable is 1 (the interactive player fades with it), so it is set
//   on every track.
// - Banks: a match loads the global lists (stock, guns, music_loading,
//   music_pause), single_all (music_backend, music_death, multiplayer) and the
//   map's ambience (sub_821F8D20, level audio load). A track's banks are loaded
//   right after, the way the lists do it (wavebank sub_821F9888, soundbank
//   sub_821F9A00 into its entry of the sound bank table), unless its soundbank
//   is already loaded; they are unloaded when the level's audio is torn down
//   (sub_821F9098), after the track has been stopped.
// - Playback: as the music manager plays a tune (sub_82200C68): cue reference
//   sub_82202D20, sound sub_821FF428, play sub_822061C8; stop sub_822055C0
//   and release sub_82205BD0 when the level is left (the audio idle check
//   sub_821F9290 waits for silence first).
// - Random: a random track at the start of the match, another once it has
//   played through (pdz_mp_music_random_loops times; timed from the first tick
//   the sound plays). A track with Rare's Ambivol volume curve fades out over
//   3 s first, the way the interactive player fades its layers; the others
//   change at the loop seam (Rare has no other fade for a sound). A chosen
//   track loops. Random draws from the 31 level
//   tracks only: MAIN MENU, MISSION SELECT, MULTIPLAYER MENU, PAUSE, RESULTS,
//   GAME OVER and CREDITS can still be chosen by name.
// - Multiplayer only: game state +40 mode 2 / 3 (as the per-level strings);
//   single player and co-op are left alone.
// - Menu: the setup screens' rows are DarkXuiValueButtons in compiled XUI
//   scenes, bound to a DARKVALUE by name (GameValue property); the game's
//   values are served by a set of per-id functions (name -> id 823B8288, get
//   823B8340, set 823B9D90, previous / next 823BBEE8 / 823BC2C0, count
//   823BA378, locked 823BA298, text 823BB968; ids 0-94 in TU3). TU3 added its
//   bot personality row as new scenes plus a new value id. Here DARKVALUE_MUSIC is a new id
//   served by hooks on those functions, and when a create-game scene is made
//   (XuiSceneCreate sub_8278F550) a MUSIC row is added under each Points
//   Limit row: a DarkXuiValueButton made with XuiCreateObject (sub_827865B8)
//   and the Points Limit row's own properties (visual, size, ...), rows below
//   moved down one row, navigation relinked, then initialized like a loaded
//   element (sub_8278E870). The value is kept in a file (the game's saved
//   match settings have no room for a new one).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <string>
#include <vector>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/stack.h>
#include <rex/system/kernel_state.h>

#include <rex/runtime.h>

#include "../game_cvars.h"
#include "cut_scenarios.h"
#include "mp_music.h"
#include "mp_music_custom.h"
#include "hook_profile.h" // pdz_hook_profile (must follow <rex/hook.h>)

REX_EXTERN(__imp__sub_821F8D20);
REX_EXTERN(__imp__sub_821F8E40);
REX_EXTERN(__imp__sub_821F9098);
REX_EXTERN(sub_821F9888);
REX_EXTERN(sub_821F9990);
REX_EXTERN(sub_821F9A00);
REX_EXTERN(sub_821F9B20);
REX_EXTERN(sub_82202D20);
REX_EXTERN(sub_821FF428);
REX_EXTERN(sub_822064E8);
REX_EXTERN(sub_822061C8);
REX_EXTERN(sub_822055C0);
REX_EXTERN(sub_82205BD0);
REX_EXTERN(__imp__sub_82205BD0);
REX_EXTERN(__imp__sub_821F9290);

namespace
{
  constexpr uint32_t kGameState = 0x82D6BFD8;   // -> game state (+40 mode)
  constexpr uint32_t kGameMode = 40;
  constexpr uint32_t kAudioManager = 0x82D6BFF8; // -> audio manager (+56 sound bank table)
  constexpr uint32_t kGameBankSlots = 148;       // entries 1..148 (149 in TU3's table)
  constexpr uint32_t kAmbivol = 0x8209431C;      // "Ambivol"
  constexpr uint32_t kSoundRefSize = 260;        // {char name[256]; cue id}
  // Sound state: 1 / 2 / 9 preparing (a streamed track starts once ready,
  // +336 bit 0x20 = play when ready), 3 fading in, 4 playing, 5 paused; 0, 6, 7
  // and 10 stopped or finished (sub_822061C8, sub_821FD788).
  constexpr uint32_t kSoundState = 340;
  constexpr uint32_t kSoundPlaying = 4;

  struct Track
  {
    const char *name;      // shown in the menu
    const char *soundbank; // music_lvNN
    const char *wavebank;  // usually the same
    const char *cue;
    double seconds;        // one play-through (intro + loop body)
    double intro;          // the part before the loop (a second play-through is seconds - intro)
    bool ambivol;          // its sound has Rare's Ambivol volume curve (RPC 0x17A), so it can fade
    bool random;           // in the Random rotation (the menu pieces aren't)
  };

  // Level order, then the menu pieces. Lengths from the wave banks (frames /
  // rate; Trinity Escape corrected 2026-10-22: 220.5 -> 208.5 s, a 15.3 s
  // intro and a 193.3 s loop). Rechecked 2026-10-29 against the wave each cue
  // plays (the soundbank's play-wave event) and its loop region (byte offsets
  // / (channels x 2)): every length is intro + loop body to 0.05 s, none
  // changed. Ambivol: the 7 interactive cues whose sound carries the Ambivol
  // -> volume curve (pdz.xgs variable 4); the others ignore the variable.
  // Every track can be chosen; Random skips the menu pieces (Nightclub's club
  // music stays in).
  constexpr Track kTracks[] = {
      {"DATACORE STEALTH", "music_lv00", "music_lv00", "XACT_SOUNDBANK_MUSIC_LV00_TRINITY_STEALTH", 152.5, 0.0, false, true},
      {"DATACORE ACTION", "music_lv00", "music_lv00", "XACT_SOUNDBANK_MUSIC_LV00_TRINITY_ACTION", 192.5, 4.3, false, true},
      {"DATACORE CAMSPY", "music_lv00", "music_lv00", "XACT_SOUNDBANK_MUSIC_LV00_TRINITY_CAMSPY", 140.2, 0.0, false, true},
      {"NIGHTCLUB", "music_lv01", "music_lv01", "XACT_SOUNDBANK_MUSIC_LV01_NIGHTCLUB_AMBI_CLUB", 523.0, 7.2, true, true},
      {"SUBWAY STEALTH", "music_lv02", "music_lv02", "XACT_SOUNDBANK_MUSIC_LV02_STREETS_INTERACTIVE_STEALTH", 163.9, 21.1, true, true},
      {"SUBWAY ACTION", "music_lv02", "music_lv02", "XACT_SOUNDBANK_MUSIC_LV02_STREETS_INTERACTIVE_SPEEDUP", 142.6, 2.4, true, true},
      {"SUBWAY BIKE ATTACK", "music_lv02", "music_lv02", "XACT_SOUNDBANK_MUSIC_LV02_STREETS_BIKEATTACK", 112.0, 0.1, false, true},
      {"SUBWAY DESCENT", "music_lv02", "music_lv02", "XACT_SOUNDBANK_MUSIC_LV02_STREETS_SUBWAYDESCEND", 112.2, 38.2, false, true},
      {"ROOFTOPS STEALTH", "music_lv03", "music_lv03", "XACT_SOUNDBANK_MUSIC_LV03_ROOFTOPS_STEALTH", 234.7, 0.0, false, true},
      {"ROOFTOPS DROPSHIP", "music_lv03", "music_lv03", "XACT_SOUNDBANK_MUSIC_LV03_DROPSHIPBOSS", 113.4, 1.8, false, true},
      {"MANSION STEALTH", "music_lv04", "music_lv04", "XACT_SOUNDBANK_MUSIC_LV04_MANSION_INTERACTIVE_STEALTH", 135.2, 0.0, true, true},
      {"MANSION ACTION", "music_lv04", "music_lv04", "XACT_SOUNDBANK_MUSIC_LV04_MANSION_INTERACTIVE_SPEEDUP", 135.2, 0.0, true, true},
      {"MANSION MAI HEM", "music_lv04", "music_lv04", "XACT_SOUNDBANK_MUSIC_LV04_MAIHEM_DEATHMATCH", 82.7, 7.5, false, true},
      {"LABORATORY STEALTH", "music_lv05", "music_lv05", "XACT_SOUNDBANK_MUSIC_LV05_LAB_INTERACTIVE_STEALTH", 180.0, 0.0, false, true},
      {"LABORATORY ACTION", "music_lv05", "music_lv05", "XACT_SOUNDBANK_MUSIC_LV05_LAB_INTERACTIVE_SPEEDUP", 149.9, 0.4, false, true},
      {"LABORATORY CONTAINMENT", "music_lv05", "music_lv05", "XACT_SOUNDBANK_MUSIC_LV05_LAB_CONTAINMENT", 107.5, 0.0, false, true},
      {"RIVER LAB ESCAPE", "music_lv06", "music_lv06", "XACT_SOUNDBANK_MUSIC_LV06_LABESCAPE", 203.1, 1.8, false, true},
      {"RIVER CHASE", "music_lv06", "music_lv06", "XACT_SOUNDBANK_MUSIC_LV06_RIVERCHASE", 170.5, 1.7, false, true},
      {"TRINITY STEALTH", "music_lv08", "music_lv08", "XACT_SOUNDBANK_MUSIC_LV08_TRINITYINFIL_STEALTH", 158.2, 5.7, true, true},
      {"TRINITY ACTION", "music_lv08", "music_lv08", "XACT_SOUNDBANK_MUSIC_LV08_TRINITY_SPEEDUP", 128.2, 8.2, true, true},
      {"TRINITY BROTHERS", "music_lv08", "music_lv08", "XACT_SOUNDBANK_MUSIC_LV08_TRINITY_BROTHERS", 108.8, 0.0, false, true},
      {"TRINITY ESCAPE", "music_lv09", "music_lv09", "XACT_SOUNDBANK_MUSIC_LV09_TRINITYESCAPE_MAIN", 208.5, 15.3, false, true},
      {"JUNGLE FIGHT", "music_lv10", "music_lv10", "XACT_SOUNDBANK_MUSIC_LV10_JUNGLEFIGHT", 102.4, 1.6, false, true},
      {"JUNGLE VILLAGE", "music_lv10", "music_lv10", "XACT_SOUNDBANK_MUSIC_LV10_VILLAGEFIGHT", 93.1, 11.6, false, true},
      {"TEMPLE I", "music_lv11", "music_lv11", "XACT_SOUNDBANK_MUSIC_LV11_TEMPLE_MAIN", 157.1, 15.9, false, true},
      {"TEMPLE CAMSPY", "music_lv11", "music_lv11", "XACT_SOUNDBANK_MUSIC_LV11_TEMPLE_CAMSPY", 80.0, 8.0, false, true},
      {"OUTPOST I", "music_lv12", "music_lv12", "XACT_SOUNDBANK_MUSIC_LV12_OUTPOST_MAIN", 103.1, 3.6, false, true},
      {"OUTPOST II", "music_lv12", "music_lv12", "XACT_SOUNDBANK_MUSIC_LV12_OUTPOST_END", 84.7, 10.4, false, true},
      {"BRIDGE I", "music_lv13", "music_lv13", "XACT_SOUNDBANK_MUSIC_LV13_BRIDGE_MAIN", 152.8, 12.4, false, true},
      {"BRIDGE ASSAULT", "music_lv13", "music_lv13", "XACT_SOUNDBANK_MUSIC_LV13_BRIDGE_ASSAULT", 117.0, 6.0, false, true},
      {"ARENA", "music_lv14", "music_lv14", "XACT_SOUNDBANK_MUSIC_LV14_FINAL_MAIN", 128.3, 17.0, false, true},
      {"MAIN MENU", "music_frontend", "music_frontend", "XACT_SOUNDBANK_MUSIC_FRONTEND_FRONTEND_MAIN", 49.7, 0.0, false, false},
      {"MISSION SELECT", "music_select", "music_select", "XACT_SOUNDBANK_MUSIC_SELECT_SELECT_MAIN", 59.9, 0.0, false, false},
      {"MULTIPLAYER MENU", "music_select", "music_select", "XACT_SOUNDBANK_MUSIC_SELECT_MULTIMENU_MAIN", 78.9, 5.0, false, false},
      {"PAUSE", "music_pause", "music_pause", "XACT_SOUNDBANK_MUSIC_PAUSE_PAUSE_MAIN", 94.1, 16.1, false, false},
      {"RESULTS", "music_backend", "music_backend", "XACT_SOUNDBANK_MUSIC_BACKEND_BACKEND_MAIN", 71.3, 0.0, false, false},
      {"GAME OVER", "music_death", "music_death", "XACT_SOUNDBANK_MUSIC_DEATH_DEATH_MAIN", 139.5, 13.5, false, false},
      {"CREDITS", "music_pause", "credits", "XACT_SOUNDBANK_MUSIC_PAUSE_CREDITS", 567.0, 0.0, false, false},
  };
  constexpr int kTrackCount = int(std::size(kTracks));
  // Values: 0 Off, 1 Random, 2.. the tracks.
  constexpr int kValueOff = 0;
  constexpr int kValueRandom = 1;
  // Then the player's own files (pdz_mp_music_custom_dir, mp_music_custom.cpp): value 2 + kTrackCount + n, track
  // index kTrackCount + n (they have no banks or sound object: the host plays them).
  int ValueCount()
  {
    return 2 + kTrackCount + mp_music_custom::Count();
  }
  bool IsCustom(int track)
  {
    return track >= kTrackCount;
  }
  const char *TrackName(int track)
  {
    return track < 0 ? "?" : IsCustom(track) ? mp_music_custom::MenuName(track - kTrackCount).c_str()
                                             : kTracks[track].name;
  }

  struct Bank
  {
    std::string name;
    uint32_t soundbank = 0; // handle we loaded (0 = not ours)
    uint32_t wavebank = 0;
  };

  std::recursive_mutex g_mutex;
  int g_value = -1; // -1 = not read yet
  std::vector<Bank> g_banks; // banks we loaded for this level
  int g_track = -1;          // playing (index into kTracks)
  int g_pending = -1;        // chosen at level load, started at level start
  uint32_t g_sound = 0;
  // The sound we play, watched by the release hook (sub_82205BD0, any
  // thread, no lock): if the engine frees it, the tick drops it instead of
  // stopping and releasing it again (log 079: a second release corrupted the
  // sound pool and the next sound start crashed in the audio engine).
  std::atomic<uint32_t> g_watch{0};
  std::atomic<bool> g_taken{false};
  std::atomic<uint32_t> g_taken_by{0};
  bool g_paused = false;
  bool g_started = false;    // the sound has been seen playing
  bool g_warned = false;
  double g_played = 0.0;     // seconds played since the sound was first seen playing, without pauses
  double g_waited = 0.0;     // seconds before that (a streamed track preparing)
  double g_fade = -1.0;      // Random's fade-out: seconds into it (-1 = not fading)
  std::chrono::steady_clock::time_point g_last_tick;
  std::mt19937 g_random{std::random_device{}()};

  bool Log()
  {
    return REXCVAR_GET(pdz_music_log);
  }

  // Random's fade-out, as the interactive music player fades a layer out (launch 821F8708's layers, T 822007F0
  // shape): Ambivol from 1 to 0 over this time, one step per audio tick. Only the cues with Rare's Ambivol volume
  // curve hear it (Track::ambivol); the rest change track at the end of the play-through, the loop seam.
  constexpr double kFadeSeconds = 3.0;

  // How long Random lets a track play: pdz_mp_music_random_loops play-throughs (the first with its intro).
  double PlayThrough(int track)
  {
    const Track &t = kTracks[track];
    const int loops = std::clamp(REXCVAR_GET(pdz_mp_music_random_loops), 1, 8);
    return t.seconds + double(loops - 1) * (t.seconds - t.intro);
  }

  void SetAmbivol(PPCContext &ctx, uint8_t *base, uint32_t sound, double value)
  {
    rex::CallFrame frame(ctx);
    frame.ctx.r3.u64 = sound;
    frame.ctx.r4.u64 = kAmbivol;
    frame.ctx.f1.f64 = value;
    sub_822064E8(frame.ctx, base);
  }

  uint32_t LoadBe32(const uint8_t *base, uint32_t address)
  {
    const uint8_t *p = base + address;
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
  }

  std::string ReadString(const uint8_t *base, uint32_t address, size_t max = 260)
  {
    std::string text;
    for (size_t i = 0; address && i < max && base[address + i]; ++i)
    {
      text += char(base[address + i]);
    }
    return text;
  }

  std::string Lower(std::string text)
  {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return text;
  }

  // The saved value: a file in the user data root, next to the saves.
  std::filesystem::path ValueFile()
  {
    auto *rt = rex::Runtime::instance();
    return (rt ? rt->user_data_root() : std::filesystem::path()) / "mp_music.txt";
  }

  int Value()
  {
    if (g_value < 0)
    {
      g_value = kValueOff;
      std::ifstream file(ValueFile());
      std::string name;
      if (std::getline(file, name))
      {
        if (name == "RANDOM")
        {
          g_value = kValueRandom;
        }
        for (int i = 0; i < kTrackCount; ++i)
        {
          if (name == kTracks[i].name)
          {
            g_value = 2 + i;
          }
        }
        if (const int custom = mp_music_custom::FromSaveName(name); custom >= 0)
        {
          g_value = 2 + kTrackCount + custom;
        }
      }
    }
    return g_value;
  }

  void SetValue(int value)
  {
    g_value = std::clamp(value, 0, ValueCount() - 1);
    std::error_code ec;
    std::filesystem::create_directories(ValueFile().parent_path(), ec);
    std::ofstream file(ValueFile(), std::ios::trunc);
    file << (g_value == kValueOff      ? std::string("OFF")
             : g_value == kValueRandom ? std::string("RANDOM")
             : IsCustom(g_value - 2)   ? mp_music_custom::SaveName(g_value - 2 - kTrackCount)
                                       : std::string(kTracks[g_value - 2].name))
         << "\n";
  }

  bool Multiplayer(const uint8_t *base)
  {
    const uint32_t game = LoadBe32(base, kGameState);
    const uint32_t mode = game ? LoadBe32(base, game + kGameMode) : 0;
    return mode == 2 || mode == 3;
  }

  bool Active(const uint8_t *base)
  {
    return REXCVAR_GET(pdz_mp_music) && Value() != kValueOff && Multiplayer(base);
  }

  int PickTrack(int avoid)
  {
    const int value = Value();
    if (value >= 2)
    {
      return value - 2;
    }
    // Random: the level tracks and the player's own files, not the one that just played (nor a file that failed).
    std::vector<int> pool;
    for (int i = 0; i < kTrackCount; ++i)
    {
      if (kTracks[i].random && i != avoid)
      {
        pool.push_back(i);
      }
    }
    for (int i = 0; i < mp_music_custom::Count(); ++i)
    {
      if (kTrackCount + i != avoid && mp_music_custom::Playable(i))
      {
        pool.push_back(kTrackCount + i);
      }
    }
    if (pool.empty())
    {
      return avoid >= 0 ? avoid : 0;
    }
    std::uniform_int_distribution<size_t> pick(0, pool.size() - 1);
    return pool[pick(g_random)];
  }

  // The sound bank table entry (manager +56, 12 bytes, numbered from 1) of a
  // soundbank, among the game's own entries.
  uint32_t BankEntry(const uint8_t *base, const std::string &name)
  {
    const uint32_t manager = LoadBe32(base, kAudioManager);
    for (uint32_t i = 1; manager && i <= kGameBankSlots; ++i)
    {
      const uint32_t entry = manager + 56 + i * 12;
      if (Lower(ReadString(base, LoadBe32(base, entry))) == name)
      {
        return entry;
      }
    }
    return 0;
  }

  uint32_t LoadFile(PPCContext &ctx, uint8_t *base, bool wavebank, const std::string &name)
  {
    rex::CallFrame frame(ctx);
    const std::string path = "D:\\audio\\" + name + (wavebank ? ".xwb" : ".xsb");
    frame.ctx.r3.u64 = rex::ppc::stack_push_string(frame.ctx, base, path.c_str());
    if (wavebank)
    {
      sub_821F9888(frame.ctx, base);
    }
    else
    {
      sub_821F9A00(frame.ctx, base);
    }
    return frame.ctx.r3.u32;
  }

  // Loads what a track needs that isn't loaded yet. False if it can't play.
  bool LoadTrack(PPCContext &ctx, uint8_t *base, int track)
  {
    if (IsCustom(track))
    {
      return true; // the host plays it
    }
    const Track &t = kTracks[track];
    const uint32_t entry = BankEntry(base, t.soundbank);
    if (!entry)
    {
      REXLOG_WARN("pdz_mp_music: no sound bank entry for {}", t.soundbank);
      return false;
    }
    const bool own_wavebank = std::string(t.wavebank) != t.soundbank;
    for (const Bank &bank : g_banks)
    {
      if (bank.name == t.soundbank && (!own_wavebank || bank.wavebank))
      {
        return true; // loaded earlier this level
      }
    }
    Bank bank;
    bank.name = t.soundbank;
    if (!LoadBe32(base, entry + 4))
    {
      bank.wavebank = LoadFile(ctx, base, true, t.wavebank);
      bank.soundbank = LoadFile(ctx, base, false, t.soundbank);
    }
    else if (own_wavebank)
    {
      bank.wavebank = LoadFile(ctx, base, true, t.wavebank); // credits.xwb (cue in music_pause)
    }
    if (Log())
    {
      REXLOG_INFO("pdz_mp_music: {} banks for {}: soundbank {:08X}, wavebank {:08X}{}", t.soundbank, t.name,
                  bank.soundbank, bank.wavebank, bank.soundbank || bank.wavebank ? "" : " (already loaded)");
    }
    if (bank.soundbank || bank.wavebank)
    {
      g_banks.push_back(bank);
    }
    return LoadBe32(base, entry + 4) != 0;
  }

  void UnloadBanks(PPCContext &ctx, uint8_t *base)
  {
    for (const Bank &bank : g_banks)
    {
      rex::CallFrame frame(ctx);
      if (bank.soundbank)
      {
        frame.ctx.r3.u64 = rex::ppc::stack_push<uint32_t>(frame.ctx, base, bank.soundbank);
        sub_821F9B20(frame.ctx, base);
      }
      if (bank.wavebank)
      {
        frame.ctx.r3.u64 = rex::ppc::stack_push<uint32_t>(frame.ctx, base, bank.wavebank);
        sub_821F9990(frame.ctx, base);
      }
      if (Log())
      {
        REXLOG_INFO("pdz_mp_music: unloaded {} (soundbank {:08X}, wavebank {:08X})", bank.name, bank.soundbank,
                    bank.wavebank);
      }
    }
    g_banks.clear();
  }

  void StopSound(PPCContext &ctx, uint8_t *base)
  {
    if (IsCustom(g_track))
    {
      mp_music_custom::Stop(); // the host stream stopped and freed
      g_track = -1;
    }
    if (!g_sound)
    {
      return;
    }
    g_watch = 0; // our own release below
    {
      rex::CallFrame frame(ctx);
      frame.ctx.r3.u64 = g_sound;
      sub_822055C0(frame.ctx, base);
    }
    {
      rex::CallFrame frame(ctx);
      frame.ctx.r3.u64 = g_sound;
      sub_82205BD0(frame.ctx, base);
    }
    if (Log())
    {
      REXLOG_INFO("pdz_mp_music: stopped {}", TrackName(g_track));
    }
    g_sound = 0;
    g_track = -1;
    g_paused = false;
  }

  // Starts a track (its banks must be loaded): as the music manager plays a
  // tune, with Ambivol at 1.
  void StartSound(PPCContext &ctx, uint8_t *base, int track)
  {
    StopSound(ctx, base);
    if (IsCustom(track))
    {
      // A chosen file loops (fading out at its end and back in); Random plays it once.
      mp_music_custom::Start(track - kTrackCount, Value() != kValueRandom);
      g_track = track;
      return;
    }
    uint32_t sound = 0;
    {
      rex::CallFrame frame(ctx);
      const uint32_t ref = rex::ppc::stack_push(frame.ctx, base, std::string(kSoundRefSize, '\0').data(),
                                                kSoundRefSize);
      frame.ctx.r3.u64 = ref;
      frame.ctx.r4.u64 = rex::ppc::stack_push_string(frame.ctx, base, kTracks[track].cue);
      sub_82202D20(frame.ctx, base);
      frame.ctx.r3.u64 = ref;
      frame.ctx.r4.u64 = 0;
      frame.ctx.r5.u64 = 0;
      frame.ctx.r6.u64 = 0;
      sub_821FF428(frame.ctx, base);
      sound = frame.ctx.r3.u32;
    }
    if (!sound)
    {
      REXLOG_WARN("pdz_mp_music: couldn't start {} ({})", kTracks[track].name, kTracks[track].cue);
      return;
    }
    SetAmbivol(ctx, base, sound, 1.0);
    {
      rex::CallFrame frame(ctx);
      frame.ctx.r3.u64 = sound;
      frame.ctx.f1.f64 = 0.0;
      sub_822061C8(frame.ctx, base);
    }
    g_sound = sound;
    g_taken = false;
    g_watch = sound;
    g_track = track;
    g_paused = false;
    g_started = false;
    g_warned = false;
    g_played = 0.0;
    g_waited = 0.0;
    g_fade = -1.0;
    g_last_tick = std::chrono::steady_clock::now();
    if (Log())
    {
      REXLOG_INFO("pdz_mp_music: playing {} ({}), sound {:08X}", kTracks[track].name, kTracks[track].cue, sound);
    }
  }
} // namespace

namespace mp_music
{
  void Tick(PPCContext &ctx, uint8_t *base)
  {
    std::lock_guard lock(g_mutex);
    if (IsCustom(g_track))
    {
      // The player's own file: when it has played through (Random) or can't be played, the next track.
      const mp_music_custom::State state = mp_music_custom::Tick();
      if (state != mp_music_custom::State::Finished && state != mp_music_custom::State::Failed)
      {
        return;
      }
      const bool random = Value() == kValueRandom;
      const int track = g_track;
      if (state == mp_music_custom::State::Failed && !random)
      {
        StopSound(ctx, base); // a chosen file that can't play: no music (logged)
        return;
      }
      const int next = random ? PickTrack(track) : track;
      if (Log())
      {
        REXLOG_INFO("pdz_mp_music: {} {} -> {}", TrackName(track),
                    state == mp_music_custom::State::Failed ? "skipped" : "played through", TrackName(next));
      }
      StopSound(ctx, base);
      if (LoadTrack(ctx, base, next))
      {
        StartSound(ctx, base, next);
      }
      return;
    }
    if (!g_sound)
    {
      return;
    }
    if (g_taken.exchange(false))
    {
      // The engine stopped and freed our sound (not ours to release any more):
      // forget it and start the track again (Random: another one).
      const int track = g_track;
      REXLOG_WARN("pdz_mp_music: {} was freed by the sound engine (from {:08X}) after {:.1f} s; starting again",
                  TrackName(track), g_taken_by.load(), g_played);
      g_sound = 0;
      g_track = -1;
      g_paused = false;
      const int next = track < 0 ? PickTrack(-1) : Value() == kValueRandom ? PickTrack(track) : track;
      if (LoadTrack(ctx, base, next))
      {
        StartSound(ctx, base, next);
      }
      return;
    }
    const auto now = std::chrono::steady_clock::now();
    const double dt = std::chrono::duration<double>(now - g_last_tick).count();
    g_last_tick = now;
    const uint32_t state = LoadBe32(base, g_sound + kSoundState);
    const bool random = Value() == kValueRandom;
    if (state == kSoundPlaying)
    {
      g_started = true;
    }
    // The clock runs from the first tick the sound is playing: a streamed track spends its first moments
    // preparing (states 1 / 2 / 9), which used to count and cut the track that much early. The sound's own play
    // position can't be read: Rare's sound object keeps only its state (+340) and XACT's cue gives no position.
    if (!g_paused)
    {
      (g_started ? g_played : g_waited) += dt;
    }
    if (!g_started && !g_warned && g_waited > 10.0)
    {
      REXLOG_WARN("pdz_mp_music: {} hasn't started after 10 s (sound state {})", kTracks[g_track].name, state);
      g_warned = true;
    }
    const bool ended = g_started && !g_paused && (state == 0 || state == 6 || state == 7 || state == 10);
    if (g_fade >= 0.0 && !ended)
    {
      g_fade += dt;
      SetAmbivol(ctx, base, g_sound, std::max(0.0, 1.0 - g_fade / kFadeSeconds));
      if (g_fade < kFadeSeconds)
      {
        return;
      }
    }
    else if (!ended)
    {
      if (!random || g_played < PlayThrough(g_track))
      {
        return;
      }
      if (kTracks[g_track].ambivol)
      {
        g_fade = 0.0;
        if (Log())
        {
          REXLOG_INFO("pdz_mp_music: {} played through after {:.1f} s (+{:.1f} s preparing): fading out over {:g} s",
                      kTracks[g_track].name, g_played, g_waited, kFadeSeconds);
        }
        return;
      }
    }
    // A chosen track that stopped (the title piece doesn't loop) starts again; Random moves on to another track.
    const int next = random ? PickTrack(g_track) : g_track;
    if (Log())
    {
      REXLOG_INFO("pdz_mp_music: {} {} after {:.1f} s (+{:.1f} s preparing) -> {}", kTracks[g_track].name,
                  ended ? "ended" : g_fade >= 0.0 ? "faded out" : "played through (no Ambivol curve: at the loop seam)",
                  g_played, g_waited, TrackName(next));
    }
    StopSound(ctx, base);
    if (LoadTrack(ctx, base, next))
    {
      StartSound(ctx, base, next);
    }
  }

  void LeaveLevel(PPCContext &ctx, uint8_t *base)
  {
    std::lock_guard lock(g_mutex);
    StopSound(ctx, base);
  }
} // namespace mp_music

// sub_82205BD0(sound): a sound object back to its pool (every sound's
// release, from anywhere). Notes when it is ours and we didn't release it.
REX_HOOK_RAW(sub_82205BD0)
{
  uint32_t watched = g_watch.load();
  if (watched && ctx.r3.u32 == watched && g_watch.compare_exchange_strong(watched, 0))
  {
    g_taken_by = uint32_t(ctx.lr);
    g_taken = true;
  }
  __imp__sub_82205BD0(ctx, base);
}

// sub_821F8D20(name): the level's audio (wavebank and soundbank lists).
REX_HOOK_RAW(sub_821F8D20)
{
  __imp__sub_821F8D20(ctx, base);
  std::lock_guard lock(g_mutex);
  g_pending = -1;
  if (!Active(base))
  {
    if (Log() && REXCVAR_GET(pdz_mp_music))
    {
      const uint32_t game = LoadBe32(base, kGameState);
      REXLOG_INFO("pdz_mp_music: level audio: game mode {}, value {} -> no match music",
                  game ? LoadBe32(base, game + kGameMode) : 0, Value());
    }
    return;
  }
  const int track = PickTrack(-1);
  if (LoadTrack(ctx, base, track))
  {
    g_pending = track;
  }
}

// sub_821F8E40(): starts the level's audio (ambience, music setup).
REX_HOOK_RAW(sub_821F8E40)
{
  __imp__sub_821F8E40(ctx, base);
  std::lock_guard lock(g_mutex);
  if (g_pending >= 0 && !g_sound && !IsCustom(g_track) && Active(base))
  {
    StartSound(ctx, base, g_pending);
  }
}

// sub_821F9098(): the level's audio teardown.
REX_HOOK_RAW(sub_821F9098)
{
  {
    std::lock_guard lock(g_mutex);
    StopSound(ctx, base);
    g_pending = -1;
    UnloadBanks(ctx, base);
  }
  __imp__sub_821F9098(ctx, base);
}

// sub_821F9290(): the audio idle check that leaving a level waits on; the
// match music stops here (it would keep the wait going).
REX_HOOK_RAW(sub_821F9290)
{
  mp_music::LeaveLevel(ctx, base);
  __imp__sub_821F9290(ctx, base);
}

// --- Menu: the DARKVALUE_MUSIC value and its row ---

REX_EXTERN(__imp__sub_823B8288);
REX_EXTERN(__imp__sub_823B8340);
REX_EXTERN(__imp__sub_823B9D90);
REX_EXTERN(__imp__sub_823BBEE8);
REX_EXTERN(__imp__sub_823BC2C0);
REX_EXTERN(__imp__sub_823BA378);
REX_EXTERN(__imp__sub_823BA298);
REX_EXTERN(__imp__sub_823BB968);
REX_EXTERN(__imp__sub_8278F550);
REX_EXTERN(__imp__sub_823FFB00);
REX_EXTERN(sub_827865B8);
REX_EXTERN(sub_8278A3B8);
REX_EXTERN(sub_8278A4C8);
REX_EXTERN(sub_8278A5B8);
REX_EXTERN(sub_8278E870);
REX_EXTERN(sub_8277B7F8);
REX_EXTERN(sub_8277F928);
REX_EXTERN(sub_8277FA28);
REX_EXTERN(sub_827950E0);
REX_EXTERN(sub_82794EC0);

namespace
{
  // A value id past the game's (0-93), served only by the hooks below.
  constexpr uint32_t kMusicValue = 120;
  constexpr const char *kMusicValueName = "DARKVALUE_MUSIC";
  constexpr const char *kAnchorRow = "optionPointsLimit";
  constexpr const char *kAdvancedRow = "navAdvanced";
  constexpr const char *kMusicRow = "optionMusic";
  constexpr const char *kMusicLabel = "MUSIC";
  constexpr float kDefaultRowSpacing = 45.0f;
  constexpr uint32_t kPropValSize = 20; // XUIElementPropVal: type, 16 bytes of value
  constexpr uint32_t kPropString = 5;
  constexpr uint32_t kPropVector = 7;

  // Properties copied from the Points Limit row to the new one. PressKey:
  // XuiButton's default press key is "accept" (0x5840: A or Start, see
  // sub_8278DA28), so a row without the scene's own PressKey takes Start as
  // its own press; the game's rows leave Start to the scene's launch button
  // (buttonCreate, the next setup screen).
  constexpr const char *kCopiedProps[] = {"Visual", "Width",     "Height",          "Anchor", "Pivot",
                                          "Scale",  "Opacity",   "Show",            "ClipChildren",
                                          "BlendMode", "ColorWriteFlags", "Tab", "PressKey"};

  std::vector<uint32_t> g_value_text; // guest UTF-16 strings, per value

  std::string ReadWide(const uint8_t *base, uint32_t address, size_t max = 128)
  {
    std::string text;
    for (size_t i = 0; address && i < max; ++i)
    {
      const uint16_t c = uint16_t(base[address + i * 2] << 8 | base[address + i * 2 + 1]);
      if (!c)
      {
        break;
      }
      text += c < 0x80 ? char(c) : '?';
    }
    return text;
  }

  uint32_t PushWide(PPCContext &frame, uint8_t *base, const std::string &text)
  {
    std::string bytes;
    for (char c : text)
    {
      bytes += '\0';
      bytes += c;
    }
    bytes.append(2, '\0');
    return rex::ppc::stack_push(frame, base, bytes.data(), uint32_t(bytes.size()));
  }

  std::string ValueName(int value)
  {
    return value == kValueOff      ? "OFF"
           : value == kValueRandom ? "RANDOM"
                                   : TrackName(value - 2);
  }

  uint32_t ValueText(uint8_t *base, int value)
  {
    if (g_value_text.empty())
    {
      for (int v = 0; v < ValueCount(); ++v)
      {
        const std::string name = ValueName(v);
        const uint32_t address = REX_KERNEL_MEMORY()->SystemHeapAlloc(uint32_t(name.size() + 1) * 2);
        for (size_t i = 0; address && i <= name.size(); ++i)
        {
          const char c = i < name.size() ? name[i] : '\0';
          base[address + i * 2] = 0;
          base[address + i * 2 + 1] = uint8_t(c);
        }
        g_value_text.push_back(address);
      }
    }
    return g_value_text[std::clamp(value, 0, int(g_value_text.size()) - 1)];
  }

  bool MenuActive()
  {
    return REXCVAR_GET(pdz_mp_music);
  }

  // XUI calls on one frame (its stack holds the scratch buffers).
  struct Xui
  {
    rex::CallFrame frame;
    uint8_t *base;
    uint32_t val;  // XUIElementPropVal
    uint32_t out;  // handle / id out
    Xui(PPCContext &ctx, uint8_t *b) : frame(ctx), base(b)
    {
      val = rex::ppc::stack_push(frame.ctx, base, std::string(kPropValSize, '\0').data(), kPropValSize);
      out = rex::ppc::stack_push<uint32_t>(frame.ctx, base, 0);
    }

    uint32_t PropId(uint32_t h, const std::string &name)
    {
      const uint32_t saved = frame.ctx.r1.u32;
      StoreOut(0xFFFFFFFFu);
      frame.ctx.r3.u64 = h;
      frame.ctx.r4.u64 = PushWide(frame.ctx, base, name);
      frame.ctx.r5.u64 = out;
      sub_8277B7F8(frame.ctx, base);
      frame.ctx.r1.u32 = saved;
      return int32_t(frame.ctx.r3.u32) < 0 ? 0xFFFFFFFFu : LoadBe32(base, out);
    }

    void StoreOut(uint32_t v)
    {
      base[out] = uint8_t(v >> 24);
      base[out + 1] = uint8_t(v >> 16);
      base[out + 2] = uint8_t(v >> 8);
      base[out + 3] = uint8_t(v);
    }

    void Clear()
    {
      frame.ctx.r3.u64 = val;
      sub_82794EC0(frame.ctx, base);
    }

    // Reads a property into val (type at +0); false if there is none.
    bool Get(uint32_t h, const std::string &name)
    {
      std::memset(base + val, 0, kPropValSize);
      const uint32_t id = PropId(h, name);
      if (id == 0xFFFFFFFFu)
      {
        return false;
      }
      frame.ctx.r3.u64 = h;
      frame.ctx.r4.u64 = id;
      frame.ctx.r5.u64 = 0;
      frame.ctx.r6.u64 = val;
      sub_8277F928(frame.ctx, base);
      return int32_t(frame.ctx.r3.u32) >= 0;
    }

    // Writes val to a property.
    bool Set(uint32_t h, const std::string &name)
    {
      const uint32_t id = PropId(h, name);
      if (id == 0xFFFFFFFFu)
      {
        return false;
      }
      frame.ctx.r3.u64 = h;
      frame.ctx.r4.u64 = id;
      frame.ctx.r5.u64 = 0;
      frame.ctx.r6.u64 = val;
      sub_8277FA28(frame.ctx, base);
      return int32_t(frame.ctx.r3.u32) >= 0;
    }

    std::string GetString(uint32_t h, const std::string &name)
    {
      std::string text;
      if (Get(h, name) && LoadBe32(base, val) == kPropString)
      {
        text = ReadWide(base, LoadBe32(base, val + 4));
      }
      Clear();
      return text;
    }

    bool SetString(uint32_t h, const std::string &name, const std::string &text)
    {
      const uint32_t saved = frame.ctx.r1.u32;
      frame.ctx.r3.u64 = val;
      frame.ctx.r4.u64 = PushWide(frame.ctx, base, text);
      sub_827950E0(frame.ctx, base);
      frame.ctx.r1.u32 = saved;
      const bool ok = Set(h, name);
      Clear();
      return ok;
    }

    bool GetPosition(uint32_t h, float pos[3])
    {
      const bool ok = Get(h, "Position") && LoadBe32(base, val) == kPropVector;
      for (int i = 0; ok && i < 3; ++i)
      {
        const uint32_t bits = LoadBe32(base, val + 4 + i * 4);
        std::memcpy(&pos[i], &bits, 4);
      }
      Clear();
      return ok;
    }

    bool SetPosition(uint32_t h, const float pos[3])
    {
      std::memset(base + val, 0, kPropValSize);
      const uint32_t type = kPropVector;
      for (int i = 0; i < 4; ++i)
      {
        uint32_t bits = type;
        if (i)
        {
          std::memcpy(&bits, &pos[i - 1], 4);
        }
        base[val + i * 4] = uint8_t(bits >> 24);
        base[val + i * 4 + 1] = uint8_t(bits >> 16);
        base[val + i * 4 + 2] = uint8_t(bits >> 8);
        base[val + i * 4 + 3] = uint8_t(bits);
      }
      return Set(h, "Position");
    }

    // Copies a property from one element to another.
    void Copy(uint32_t from, uint32_t to, const std::string &name)
    {
      if (Get(from, name))
      {
        Set(to, name);
      }
      Clear();
    }

    uint32_t Child(uint32_t h, bool first)
    {
      StoreOut(0);
      frame.ctx.r3.u64 = h;
      frame.ctx.r4.u64 = out;
      if (first)
      {
        sub_8278A4C8(frame.ctx, base);
      }
      else
      {
        sub_8278A5B8(frame.ctx, base);
      }
      return int32_t(frame.ctx.r3.u32) < 0 ? 0 : LoadBe32(base, out);
    }

    std::vector<uint32_t> Children(uint32_t h)
    {
      std::vector<uint32_t> list;
      for (uint32_t c = Child(h, true); c && list.size() < 256; c = Child(c, false))
      {
        list.push_back(c);
      }
      return list;
    }

    uint32_t Create(const std::string &cls)
    {
      const uint32_t saved = frame.ctx.r1.u32;
      StoreOut(0);
      frame.ctx.r3.u64 = PushWide(frame.ctx, base, cls);
      frame.ctx.r4.u64 = out;
      sub_827865B8(frame.ctx, base);
      frame.ctx.r1.u32 = saved;
      return int32_t(frame.ctx.r3.u32) < 0 ? 0 : LoadBe32(base, out);
    }
  };

  // Adds the MUSIC row under the Points Limit row `anchor` in `parent`.
  void AddRow(Xui &x, uint32_t parent, uint32_t anchor)
  {
    for (uint32_t c : x.Children(parent))
    {
      if (x.GetString(c, "Id") == kMusicRow)
      {
        return; // already there
      }
    }
    float at[3];
    if (!x.GetPosition(anchor, at))
    {
      return;
    }
    // Row spacing: the nearest row below.
    float spacing = 0.0f;
    std::vector<std::pair<uint32_t, float>> below;
    for (uint32_t c : x.Children(parent))
    {
      float p[3];
      if (c != anchor && x.GetPosition(c, p) && p[1] > at[1] + 0.5f)
      {
        below.push_back({c, p[1]});
        if (spacing == 0.0f || p[1] - at[1] < spacing)
        {
          spacing = p[1] - at[1];
        }
      }
    }
    if (spacing <= 0.0f || spacing > 2 * kDefaultRowSpacing)
    {
      spacing = kDefaultRowSpacing;
    }
    if (Log())
    {
      std::string rows;
      for (uint32_t c : x.Children(parent))
      {
        float p[3];
        if (x.GetPosition(c, p))
        {
          rows += " " + x.GetString(c, "Id") + "@" + std::to_string(int(p[1]));
        }
      }
      REXLOG_INFO("pdz_mp_music: rows before the Music row:{}", rows);
    }
    const uint32_t row = x.Create("DarkXuiValueButton");
    if (!row)
    {
      REXLOG_WARN("pdz_mp_music: couldn't create the Music row");
      return;
    }
    for (const char *name : kCopiedProps)
    {
      x.Copy(anchor, row, name);
    }
    x.SetString(row, "Id", kMusicRow);
    x.SetString(row, "Text", kMusicLabel);
    x.SetString(row, "GameValue", kMusicValueName);
    x.SetString(row, "InfoScene", "");
    const float pos[3] = {at[0], at[1] + spacing, at[2]};
    x.SetPosition(row, pos);
    // Move the rows below down one row.
    for (const auto &[c, y] : below)
    {
      float p[3];
      if (x.GetPosition(c, p))
      {
        p[1] += spacing;
        x.SetPosition(c, p);
      }
    }
    // Navigation: Points Limit <-> Music <-> what followed Points Limit.
    const std::string anchor_id = x.GetString(anchor, "Id");
    const std::string next_id = x.GetString(anchor, "NavDown");
    x.SetString(row, "NavUp", anchor_id);
    x.SetString(row, "NavDown", next_id);
    x.SetString(anchor, "NavDown", kMusicRow);
    for (uint32_t c : x.Children(parent))
    {
      if (!next_id.empty() && x.GetString(c, "Id") == next_id && x.GetString(c, "NavUp") == anchor_id)
      {
        x.SetString(c, "NavUp", kMusicRow);
      }
    }
    x.frame.ctx.r3.u64 = parent;
    x.frame.ctx.r4.u64 = row;
    sub_8278A3B8(x.frame.ctx, x.base);
    x.frame.ctx.r3.u64 = row;
    x.frame.ctx.r4.u64 = 0;
    sub_8278E870(x.frame.ctx, x.base);
    if (Log())
    {
      uint32_t press_key[2] = {};
      if (x.Get(anchor, "PressKey"))
      {
        press_key[0] = LoadBe32(x.base, x.val);
        press_key[1] = LoadBe32(x.base, x.val + 4);
      }
      x.Clear();
      REXLOG_INFO("pdz_mp_music: Music row added under {} at y {:.1f} (spacing {:.1f}, {} row(s) moved, next {}, "
                  "press key type {} value {:04X})",
                  anchor_id, pos[1], spacing, below.size(), next_id.empty() ? "-" : next_id, press_key[0],
                  press_key[1]);
    }
  }

  // Every Points Limit row in a scene tree; returns how many.
  int AddRows(Xui &x, uint32_t h, int depth)
  {
    if (depth > 12)
    {
      return 0;
    }
    int found = 0;
    for (uint32_t c : x.Children(h))
    {
      if (x.GetString(c, "Id") == kAnchorRow)
      {
        AddRow(x, h, c);
        ++found;
      }
      else
      {
        found += AddRows(x, c, depth + 1);
      }
    }
    return found;
  }

  // Setup screens without a Points Limit row (TU3's DarkOps setup): the row
  // goes under the row above Advanced Options (the one whose NavDown is
  // navAdvanced), where it sits on the Deathmatch screens. Returns how many.
  int AddRowsAboveAdvanced(Xui &x, uint32_t h, int depth)
  {
    if (depth > 12)
    {
      return 0;
    }
    int found = 0;
    const std::vector<uint32_t> children = x.Children(h);
    for (uint32_t c : children)
    {
      if (x.GetString(c, "Id") != kAdvancedRow)
      {
        found += AddRowsAboveAdvanced(x, c, depth + 1);
        continue;
      }
      for (uint32_t above : children)
      {
        if (above != c && x.GetString(above, "NavDown") == kAdvancedRow)
        {
          AddRow(x, h, above);
          ++found;
          break;
        }
      }
    }
    return found;
  }

  // Every element's Id and y in a scene tree, for the log.
  void ListRows(Xui &x, uint32_t h, int depth, std::string &text)
  {
    if (depth > 12)
    {
      return;
    }
    for (uint32_t c : x.Children(h))
    {
      float p[3];
      text += " " + x.GetString(c, "Id");
      if (x.GetPosition(c, p))
      {
        text += "@" + std::to_string(int(p[1]));
      }
      ListRows(x, c, depth + 1, text);
    }
  }
} // namespace

// sub_8278F550(base path, path, init data, &scene): XuiSceneCreate. A
// create-game scene gets its Music rows.
REX_HOOK_RAW(sub_8278F550)
{
  const std::string path = MenuActive() ? Lower(ReadWide(base, ctx.r4.u32, 260)) : std::string();
  const uint32_t out = ctx.r6.u32;
  __imp__sub_8278F550(ctx, base);
  if (path.find("creategame") == std::string::npos || int32_t(ctx.r3.u32) < 0 || !out)
  {
    return;
  }
  const uint32_t scene = LoadBe32(base, out);
  if (Log())
  {
    REXLOG_INFO("pdz_mp_music: scene {} -> {:08X}", path, scene);
  }
  if (scene)
  {
    const uint32_t result = ctx.r3.u32;
    Xui x(ctx, base);
    if (!AddRows(x, scene, 0) && !AddRowsAboveAdvanced(x, scene, 0) && Log())
    {
      std::string rows;
      ListRows(x, scene, 0, rows);
      REXLOG_INFO("pdz_mp_music: no Points Limit or Advanced Options row in {}:{}", path, rows);
    }
    ctx.r3.u64 = result;
  }
}

// sub_823FFB00(element): the DarkXUI panel switch's update: it shows the
// child panel numbered by its game value (Tab1 for 0, ...). With
// pdz_mp_cut_scenarios the scenario switch reads Capture the Flag for Plant
// the Flag and Protection (cut_scenarios::ValueRead), so the info scene
// aid_darkxui_frontend_info_deathmatchscenario shows CTF's Tab3; its text
// element gets our description then, and CTF's own line back for CTF. The
// setup scene's switch has the same option: its Tab3 holds option rows
// (GameValue), so it is left alone.
namespace
{
  std::unordered_map<std::string, std::string> g_info_original; // Tab3 element Id -> Rare's (CTF's) text
  std::atomic<int> g_info_logged{0};

  // Every text-bearing descendant of a tab: handle, Id, Text.
  void InfoTexts(Xui &x, uint32_t tab, int depth, std::vector<std::pair<uint32_t, std::string>> &out, bool &rows)
  {
    if (depth > 4)
    {
      return;
    }
    for (uint32_t c : x.Children(tab))
    {
      if (x.Get(c, "GameValue"))
      {
        rows = true; // an option row: the setup scene's panel, not the info scene's
      }
      x.Clear();
      const std::string text = x.GetString(c, "Text");
      if (!text.empty())
      {
        out.push_back({c, text});
      }
      InfoTexts(x, c, depth + 1, out, rows);
    }
  }
} // namespace

REX_HOOK_RAW(sub_823FFB00)
{
  const uint32_t element = ctx.r3.u32;
  __imp__sub_823FFB00(ctx, base);
  if (!cut_scenarios::MenuTextOn() || !element)
  {
    return;
  }
  const bool diag = cut_scenarios::ScenarioLogOn();
  if (LoadBe32(base, element + 8) != cut_scenarios::ScenarioOption())
  {
    if (diag)
    {
      static std::unordered_map<uint32_t, uint32_t> seen; // element -> option
      if (seen.size() < 64 && seen.emplace(element, LoadBe32(base, element + 8)).second)
      {
        REXLOG_INFO("pdz_scenario: panel switch {:08X} for option {} (not the scenario)", element,
                    LoadBe32(base, element + 8));
      }
    }
    return;
  }
  // The scenario as chosen (the unhooked getter: ValueRead would say CTF).
  int32_t value;
  {
    rex::CallFrame frame(ctx);
    frame.ctx.r3.u64 = cut_scenarios::ScenarioOption();
    __imp__sub_823B8340(frame.ctx, base);
    value = int32_t(frame.ctx.r3.u32);
  }
  const char *ours = cut_scenarios::ScenarioDescription(value);
  // The DarkXUI control object: +4 = its XUI handle (a XuiTabScene: the
  // switch calls its tab navigate at 823FFD14 with the value as the index;
  // +0xC = the tab shown), +8 = the option id. XUI handles are not plain
  // pointers (log 204), so the word is used as it is.
  const uint32_t handle = LoadBe32(base, element + 4);
  // Only when the scenario or the scene changes (the switch updates every
  // frame). Play test (2026-10-24): Protection showed CTF's text again on a
  // later visit: the menu rebuilds the scene (new handle) at the same control
  // address, and the cache was keyed by the control alone.
  static std::unordered_map<uint64_t, int32_t> last_value; // control + handle -> value applied
  const uint64_t key = (uint64_t(element) << 32) | handle;
  if (const auto it = last_value.find(key); it != last_value.end() && it->second == value)
  {
    return;
  }
  if (last_value.size() > 256)
  {
    last_value.clear();
  }
  Xui x(ctx, base);
  std::string probe;
  for (uint32_t c : x.Children(handle))
  {
    probe += " " + x.GetString(c, "Id");
  }
  if (diag && g_info_logged.fetch_add(1) < 4)
  {
    REXLOG_INFO("pdz_scenario: panel switch {:08X} (option {}, value {}) handle {:08X} children:{}", element,
                cut_scenarios::ScenarioOption(), value, handle, probe.empty() ? " none" : probe.c_str());
  }
  if (!handle)
  {
    return;
  }
  last_value[key] = value;
  uint32_t tab3 = 0;
  std::string ids;
  for (uint32_t c : x.Children(handle))
  {
    const std::string id = x.GetString(c, "Id");
    ids += " " + id;
    if (id == "Tab3")
    {
      tab3 = c;
    }
  }
  // Log 205: Tab3's first text was the "SCENARIO" label; the panel holds
  // a title ("CAPTURE THE FLAG") and the description (the longest text).
  std::vector<std::pair<uint32_t, std::string>> texts;
  bool rows = false;
  if (tab3)
  {
    InfoTexts(x, tab3, 0, texts, rows);
  }
  if (diag && g_info_logged.load() < 8)
  {
    std::string list;
    for (const auto &[h, t] : texts)
    {
      list += fmt::format(" {:08X}:{}=\"{}\"", h, x.GetString(h, "Id"), t.substr(0, 40));
    }
    REXLOG_INFO("pdz_scenario: panel switch handle {:08X} children:{}; Tab3 {:08X}{}; texts:{}", handle, ids, tab3,
                rows ? " (option rows: left alone)" : "", list);
  }
  if (!tab3 || rows || texts.empty())
  {
    return;
  }
  // Log 206: the panel's elements are "level name" (the SCENARIO label),
  // "location text" (the description) and "button name1" (the two-line
  // title, CAPTURE / THE FLAG).
  uint32_t title = 0, description = 0;
  for (const auto &[h, t] : texts)
  {
    const std::string id = x.GetString(h, "Id");
    if (id == "button name1")
    {
      title = h;
    }
    else if (id == "location text")
    {
      description = h;
    }
  }
  if (!description)
  {
    return;
  }
  // Rare's text is kept by element Id (handles are reused for other
  // elements once a scene goes away).
  const auto apply = [&](uint32_t h, const char *wanted_ours)
  {
    const std::string id = x.GetString(h, "Id");
    auto original = g_info_original.find(id);
    if (original == g_info_original.end())
    {
      const std::string current = x.GetString(h, "Text");
      bool is_ours = false;
      for (int32_t v : {3, 5})
      {
        is_ours = is_ours || (cut_scenarios::ScenarioDescription(v) && current == cut_scenarios::ScenarioDescription(v)) ||
                  (cut_scenarios::ScenarioTitle(v) && current == cut_scenarios::ScenarioTitle(v));
      }
      if (is_ours)
      {
        return; // no original seen yet (the scene came up on one of ours)
      }
      original = g_info_original.emplace(id, current).first;
    }
    const std::string wanted = wanted_ours ? std::string(wanted_ours) : original->second;
    if (x.GetString(h, "Text") != wanted)
    {
      const bool ok = x.SetString(h, "Text", wanted);
      if (diag)
      {
        REXLOG_INFO("pdz_scenario: info scene {:08X} text for scenario {}: \"{}\" ({})", h, value, wanted.substr(0, 60),
                    ok ? "ok" : "FAILED");
      }
    }
  };
  apply(description, ours);
  if (title)
  {
    apply(title, cut_scenarios::ScenarioTitle(value));
  }
}

// sub_823B8288(name): DARKVALUE name -> id.
REX_HOOK_RAW(sub_823B8288)
{
  if (MenuActive() && ReadWide(base, ctx.r3.u32, 32) == kMusicValueName)
  {
    ctx.r3.u64 = kMusicValue;
    return;
  }
  __imp__sub_823B8288(ctx, base);
}

// sub_823B8340(id): a value.
REX_HOOK_RAW(sub_823B8340)
{
  if (ctx.r3.u32 == kMusicValue)
  {
    std::lock_guard lock(g_mutex);
    ctx.r3.u64 = uint32_t(Value());
    return;
  }
  const uint32_t id = ctx.r3.u32;
  const uint32_t lr = uint32_t(ctx.lr);
  __imp__sub_823B8340(ctx, base);
  // The cut scenarios (cut_scenarios.cpp) share this hook.
  ctx.r3.u64 = cut_scenarios::ValueRead(id, lr, ctx.r3.u32);
}

// sub_823B9D90(id, value, player): sets a value.
REX_HOOK_RAW(sub_823B9D90)
{
  if (ctx.r3.u32 == kMusicValue)
  {
    std::lock_guard lock(g_mutex);
    SetValue(int(ctx.r4.u32));
    if (Log())
    {
      REXLOG_INFO("pdz_mp_music: Music = {}", ValueName(g_value));
    }
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_823B9D90(ctx, base);
}

// sub_823BBEE8(id, player) / sub_823BC2C0(id, player): the previous / next value.
REX_HOOK_RAW(sub_823BBEE8)
{
  if (ctx.r3.u32 == kMusicValue)
  {
    std::lock_guard lock(g_mutex);
    ctx.r3.u64 = uint32_t((Value() + ValueCount() - 1) % ValueCount());
    return;
  }
  if (uint32_t bots = 0; cut_scenarios::BotCountStep(ctx, base, ctx.r3.u32, __imp__sub_823BBEE8, bots))
  {
    ctx.r3.u64 = bots;
    return;
  }
  if (uint32_t level = 0; cut_scenarios::LevelStep(ctx, base, ctx.r3.u32, true, __imp__sub_823BBEE8, level))
  {
    ctx.r3.u64 = level;
    return;
  }
  __imp__sub_823BBEE8(ctx, base);
}

REX_HOOK_RAW(sub_823BC2C0)
{
  if (ctx.r3.u32 == kMusicValue)
  {
    std::lock_guard lock(g_mutex);
    ctx.r3.u64 = uint32_t((Value() + 1) % ValueCount());
    return;
  }
  if (uint32_t bots = 0; cut_scenarios::BotCountStep(ctx, base, ctx.r3.u32, __imp__sub_823BC2C0, bots))
  {
    ctx.r3.u64 = bots;
    return;
  }
  if (uint32_t level = 0; cut_scenarios::LevelStep(ctx, base, ctx.r3.u32, false, __imp__sub_823BC2C0, level))
  {
    ctx.r3.u64 = level;
    return;
  }
  __imp__sub_823BC2C0(ctx, base);
}

// sub_823BA378(id): how many values.
REX_HOOK_RAW(sub_823BA378)
{
  if (ctx.r3.u32 == kMusicValue)
  {
    ctx.r3.u64 = uint32_t(ValueCount());
    return;
  }
  __imp__sub_823BA378(ctx, base);
}

// sub_823BA298(id): locked (not changeable)?
REX_HOOK_RAW(sub_823BA298)
{
  if (ctx.r3.u32 == kMusicValue)
  {
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_823BA298(ctx, base);
}

// sub_823BB968(id, value, flag): a value's text.
REX_HOOK_RAW(sub_823BB968)
{
  if (ctx.r3.u32 == kMusicValue)
  {
    ctx.r3.u64 = ValueText(base, int(ctx.r4.u32));
    return;
  }
  // The cut scenarios' names (cut_scenarios.cpp) share this hook.
  if (const uint32_t text = cut_scenarios::ValueText(base, ctx.r3.u32, ctx.r4.u32))
  {
    ctx.r3.u64 = text;
    return;
  }
  __imp__sub_823BB968(ctx, base);
}
