// Interactive music for the Title Update 3 build (tu3-base).
//
// Title Update 3 removed the launch exe's interactive music player: the
// stealth / action layers a level's music record can ask for (music type 1,
// MusicType_Interactive). Every level script still asks for it, TU3 just plays
// nothing (docs/BUG_AUDIT.md U5, docs/TU3_BASE.md). The player's storage is
// still there in TU3 (the music manager state at T 82E682C8, player object at
// +4, same layout as launch 82E67500), only the code is gone.
//
// The code comes back from the launch exe: music_lifted.inc holds the launch
// build's generated C++ for the player and the manager functions that drive
// it, retargeted to TU3 by tools/lift_music.py (mus_sub_<launch address>).
// Here they are wired into TU3:
//
// - TU3's versions of the manager functions that only lost the player are
//   replaced by the launch versions: level music setup (T 82200738 = launch
//   821F8450), stop (T 822008B8 = 821F8600), intensity (T 82200928 =
//   821F8708), play tune (T 82200C68 = 821F8B28) and the player constructor
//   (T 822082E8 = 82200920, which also numbers the three layers).
// - The audio tick (T 821F8A38 = launch 821F1080) ticked the player
//   (821F86C0) right before T 82201038; done around that call, followed by
//   the multiplayer match music's tick (src/mod/mp_music.cpp).
// - TU3 inlined the small manager functions it kept into their callers and
//   dropped their interactive half. Mid-function hooks (midasm_hooks.toml)
//   put it back at those spots:
//   - resume / start (launch 821F8AC0 / 821F8570 for type 1: state 2 =
//     start the layers) in "resume all" (T 821F8BF4), the pause menu's
//     resume (T 821F9E58), level start (T 821F8F48), the end of a cutscene
//     (T 821F9814) and the cue marker event 0 (T 82207220);
//   - pause (launch 821F8A78: state 1) in "pause all" (T 821FBD58);
//   - cue markers 5 / 6 (launch 821F8CB0 / 821F8D10: a layer's cue reached
//     its loop / end point) in the sound cue tick (T 822071B0);
//   - game event 44 (launch 821FC548: the stealth / action level the AI
//     reports) in the event handler (T 82517B40).
// music_lifted.inc is recompiled launch-exe code, so it is not in the public repository: generate it from your own
// launch build with tools/lift_music.py. Without it this file builds TU3's own (non-interactive) music unchanged and
// keeps only the audio tick for the multiplayer music, the audio presence log and the speech restorations.
#if __has_include("music_lifted.inc")
#include "music_lifted.inc"
#define PDZ_MUSIC_LIFTED 1
#else
#include <rex/ppc/func.h>
#define PDZ_MUSIC_LIFTED 0
#endif

#include "../mod/audio_presence_log.h"
#include "../mod/mp_music.h"
#include "../mod/restorations.h"

#include <string>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/runtime.h>

REX_EXTERN(__imp__sub_82201038);

namespace
{
  // Music manager state (T 82E682C8; launch 82E67500).
  constexpr uint32_t kMusic = 0x82E682C8;
  constexpr uint32_t kLayer1Cue = kMusic + 52;    // player +48: layer 1 cue
  constexpr uint32_t kLayer1Flags = kMusic + 96;  // byte: wants loop / end markers
  constexpr uint32_t kLayer1Hits = kMusic + 97;   // byte: markers reached
  constexpr uint32_t kLayer2Cue = kMusic + 100;   // player +96: layer 2 cue
  constexpr uint32_t kLayer2Flags = kMusic + 144;
  constexpr uint32_t kLayer2Hits = kMusic + 145;
  constexpr uint32_t kPlayerState = kMusic + 148; // player +144: 0 stopped, 2 playing
  constexpr uint32_t kRequest = kMusic + 152;     // player +148: 1 pause, 2 start, 3 change level
  constexpr uint32_t kCurrentLevel = kMusic + 168;
  constexpr uint32_t kWantedLevel = kMusic + 172;
  constexpr uint32_t kType = kMusic + 176;        // music type playing (1 = interactive)
  constexpr uint32_t kSuppress = kMusic + 184;    // byte

  constexpr uint32_t kTickReturn = 0x821F8ABC;    // T 821F8A38's call of 82201038

  uint8_t *Base()
  {
    auto *rt = rex::Runtime::instance();
    return rt ? rt->virtual_membase() : nullptr;
  }

  uint32_t Load32(const uint8_t *base, uint32_t address)
  {
    const uint8_t *p = base + address;
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
  }

  void Store32(uint8_t *base, uint32_t address, uint32_t value)
  {
    uint8_t *p = base + address;
    p[0] = uint8_t(value >> 24);
    p[1] = uint8_t(value >> 16);
    p[2] = uint8_t(value >> 8);
    p[3] = uint8_t(value);
  }

  // Launch 821F8CB0 / 821F8D10: a layer's cue hit marker `bit`.
  void MarkerHit(uint8_t *base, uint32_t cue, uint8_t bit)
  {
    if (!cue)
    {
      return;
    }
    if (Load32(base, kLayer1Cue) == cue)
    {
      if (base[kLayer1Flags] & bit)
      {
        base[kLayer1Hits] |= bit;
      }
    }
    else if (Load32(base, kLayer2Cue) == cue)
    {
      if (base[kLayer2Flags] & bit)
      {
        base[kLayer2Hits] |= bit;
      }
    }
  }

  // Reads a C string from guest memory for the log.
  std::string GuestString(const uint8_t *base, uint32_t address)
  {
    std::string s;
    if (address < 0x10000)
    {
      return s;
    }
    for (const char *p = reinterpret_cast<const char *>(base + address); *p && s.size() < 96; ++p)
    {
      s += *p;
    }
    return s;
  }
} // namespace

#if PDZ_MUSIC_LIFTED
// Level music setup: TU3 dropped starting the layers.
REX_HOOK_RAW(sub_82200738)
{
  // game state block (T 82D6BFD8): +32 level object, its +36 = the music record
  const uint32_t holder = Load32(base, 0x82D6BFD8 + 32);
  const uint32_t record = holder ? Load32(base, holder + 36) : 0;
  if (record)
  {
    REXLOG_INFO("pdz music: level music type {}, layers '{}' / '{}'", Load32(base, record + 12),
                GuestString(base, Load32(base, record + 24)), GuestString(base, Load32(base, record + 48)));
  }
  mus_sub_821F8450(ctx, base);
}

REX_HOOK_RAW(sub_822008B8)
{
  mus_sub_821F8600(ctx, base);
}

REX_HOOK_RAW(sub_82200928)
{
  mus_sub_821F8708(ctx, base);
}

REX_HOOK_RAW(sub_82200C68)
{
  mus_sub_821F8B28(ctx, base);
}

REX_HOOK_RAW(sub_822082E8)
{
  mus_sub_82200920(ctx, base);
}
#endif // PDZ_MUSIC_LIFTED

void PdzTestCompleteMissionTick(uint8_t *base); // test_mp_setup.cpp

REX_HOOK_RAW(sub_82201038)
{
  if (ctx.lr == kTickReturn)
  {
    const double f1 = ctx.f1.f64; // frame time, the caller passes it to both
#if PDZ_MUSIC_LIFTED
    mus_sub_821F86C0(ctx, base);
#else
    static bool noted = false;
    if (!noted)
    {
      noted = true;
      REXLOG_INFO("pdz music: interactive music not built (src/music/music_lifted.inc missing: tools/lift_music.py)");
    }
#endif
    mp_music::Tick(ctx, base); // multiplayer match music (pdz_mp_music)
    audio_presence_log::Tick(base); // 5 s summary (pdz_audio_presence_log)
    restorations::Tick(ctx, base);  // delayed restoration lines (pdz_restore_light_speech)
    PdzTestCompleteMissionTick(base); // pdz_test_complete_mission (testing tool, test_mp_setup.cpp)
    ctx.f1.f64 = f1;
  }
  __imp__sub_82201038(ctx, base);
}

// Mid-function hooks (midasm_hooks.toml).
void PdzMusicResume()
{
  if (!PDZ_MUSIC_LIFTED)
  {
    return;
  }
  uint8_t *base = Base();
  if (base && Load32(base, kType) == 1)
  {
    Store32(base, kRequest, 2);
  }
}

void PdzMusicPause()
{
  if (!PDZ_MUSIC_LIFTED)
  {
    return;
  }
  if (uint8_t *base = Base())
  {
    Store32(base, kRequest, 1);
  }
}

void PdzMusicMarker(PPCRegister &event, PPCRegister &cue)
{
  uint8_t *base = Base();
  if (!PDZ_MUSIC_LIFTED || !base)
  {
    return;
  }
  if (event.u32 == 5)
  {
    MarkerHit(base, cue.u32, 1);
  }
  else if (event.u32 == 6)
  {
    MarkerHit(base, cue.u32, 2);
  }
}

// Launch 821FC548 (event 44, r28 = the event: +2 level, +3 flags). TU3 kept
// only the flag half; true skips it (to T 82517B64).
bool PdzMusicIntensityEvent(PPCRegister &event)
{
  uint8_t *base = Base();
  if (!PDZ_MUSIC_LIFTED || !base)
  {
    return false;
  }
  const uint32_t e = event.u32;
  const uint8_t flags = base[e + 3];
  if (flags && (flags & 2))
  {
    base[kSuppress] = 0;
  }
  const uint32_t level = base[e + 2];
  const uint32_t state = Load32(base, kPlayerState);
  if (state == 0)
  {
    Store32(base, kWantedLevel, level);
  }
  else if (state == 2)
  {
    Store32(base, kWantedLevel, level);
    if (Load32(base, kCurrentLevel) != level)
    {
      Store32(base, kRequest, 3);
    }
  }
  return true;
}
