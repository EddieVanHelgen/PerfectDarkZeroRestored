// Music diagnostic (pdz_music_log). See README.md next to this file.
//
// Read-only: logs the music manager's state changes and music cue starts so
// the dynamic music can be checked in a level.
//
// - Music type (None, Interactive, Tune1-4, Death; names at 82D228A8): the
//   manager (82E67500) keeps the current type at +0xB0 and the requested one
//   at +0xB4; every change goes through sub_821F8B28 (from SetMusicType
//   sub_821F85B8 and level scripts that store +0xB4 themselves), which
//   leaves the tune / death sound at +0x00.
// - Interactive music (levels Silence, Stealth, Speedup; names at 82D22898):
//   the player at 82E67504 has state +0x90 (0 = stopped), pending action
//   +0x94 (1 pause, 2 start, 3 change level), level +0xA4 and requested
//   level +0xA8 (82E67594 / 98 / A8 / AC); polled after the music tick
//   sub_821F86C0.
// - Cues: sub_821F70D0(ref) creates a sound from {char name[256]; id
//   +0x100} and returns it (0 = failed). Logged for XACT_SOUNDBANK_MUSIC_*
//   names and the interactive layers (called from 82200454, r31 = layer,
//   level = (r31 - 82E67504) / 0x30).
// - Pause / resume / stop: sub_821F8A78 / sub_821F8AC0 / sub_821F8570.
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>

#include <rex/hook.h>
#include <rex/logging.h>

#include "../game_cvars.h"
#include "mp_music.h"

REX_EXTERN(__imp__sub_821F8B28);
REX_EXTERN(__imp__sub_821F86C0);
REX_EXTERN(__imp__sub_821F70D0);
REX_EXTERN(__imp__sub_821F8A78);
REX_EXTERN(__imp__sub_821F8AC0);
REX_EXTERN(__imp__sub_821F8570);

namespace
{
  uint32_t LoadBe32(const uint8_t *base, uint32_t address)
  {
    const uint8_t *p = base + address;
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
  }

  bool Enabled()
  {
    return REXCVAR_GET(pdz_music_log);
  }

  constexpr uint32_t kMusicSound = 0x82E67500;
  constexpr uint32_t kMusicType = 0x82E675B0;
  constexpr uint32_t kMusicTypeRequested = 0x82E675B4;
  constexpr uint32_t kInteractive = 0x82E67504;
  constexpr uint32_t kInteractiveState = 0x82E67594;
  constexpr uint32_t kInteractiveAction = 0x82E67598;
  constexpr uint32_t kInteractiveLevel = 0x82E675A8;
  constexpr uint32_t kInteractiveLevelRequested = 0x82E675AC;
  constexpr uint32_t kInteractiveLayerSize = 0x30;
  constexpr uint32_t kLayerCueReturn = 0x82200454;
  constexpr uint32_t kSoundRefId = 0x100;
  constexpr uint32_t kMusicTypeNames = 0x82D228A8;
  constexpr uint32_t kMusicTypeCount = 7;
  constexpr uint32_t kIaLevelNames = 0x82D22898;
  constexpr uint32_t kIaLevelCount = 4;

  std::string GuestString(const uint8_t *base, uint32_t address, size_t limit)
  {
    if (address < 0x40000000u && (address < 0x82000000u || address >= 0x833D0000u))
    {
      // Neither heap nor image: don't read it.
      if (address < 0x10000u)
      {
        return {};
      }
    }
    const char *p = reinterpret_cast<const char *>(base + address);
    size_t n = 0;
    while (n < limit && p[n])
    {
      ++n;
    }
    return std::string(p, n);
  }

  std::string EnumName(const uint8_t *base, uint32_t table, uint32_t count, uint32_t value, const char *prefix)
  {
    if (value >= count)
    {
      return std::to_string(int32_t(value));
    }
    std::string name = GuestString(base, LoadBe32(base, table + value * 4), 64);
    const size_t n = std::strlen(prefix);
    return name.compare(0, n, prefix) == 0 ? name.substr(n) : name;
  }

  std::string TypeName(const uint8_t *base, uint32_t value)
  {
    return EnumName(base, kMusicTypeNames, kMusicTypeCount, value, "MusicType_");
  }

  std::string LevelName(const uint8_t *base, uint32_t value)
  {
    return EnumName(base, kIaLevelNames, kIaLevelCount, value, "IaLevel_");
  }

  double Seconds()
  {
    static const auto start = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  }

  struct Interactive
  {
    uint32_t state = ~0u, action = ~0u, level = ~0u, requested = ~0u;
  } g_interactive;
} // namespace

// sub_821F8B28(): applies a music type change (requested -> current).
REX_HOOK_RAW(sub_821F8B28)
{
  if (!Enabled())
  {
    __imp__sub_821F8B28(ctx, base);
    return;
  }
  const uint32_t lr = uint32_t(ctx.lr);
  const uint32_t from = LoadBe32(base, kMusicType);
  const uint32_t to = LoadBe32(base, kMusicTypeRequested);
  __imp__sub_821F8B28(ctx, base);
  if (from != to)
  {
    REXLOG_INFO("pdz_music: [{:8.2f}] type {} -> {} (now {}), sound {:08X}, from {:08X}", Seconds(),
                TypeName(base, from), TypeName(base, to), TypeName(base, LoadBe32(base, kMusicType)),
                LoadBe32(base, kMusicSound), lr);
  }
}

// sub_821F86C0(): the music tick; interactive music state changes. Also
// drives the multiplayer match music (mp_music.cpp).
REX_HOOK_RAW(sub_821F86C0)
{
  __imp__sub_821F86C0(ctx, base);
  mp_music::Tick(ctx, base);
  if (!Enabled())
  {
    return;
  }
  const Interactive now{LoadBe32(base, kInteractiveState), LoadBe32(base, kInteractiveAction),
                        LoadBe32(base, kInteractiveLevel), LoadBe32(base, kInteractiveLevelRequested)};
  if (now.state != g_interactive.state || now.level != g_interactive.level ||
      now.requested != g_interactive.requested || (now.action != g_interactive.action && now.action))
  {
    REXLOG_INFO("pdz_music: [{:8.2f}] interactive level {} -> {} (requested {}), state {}, action {}", Seconds(),
                LevelName(base, g_interactive.level), LevelName(base, now.level), LevelName(base, now.requested),
                now.state, now.action);
  }
  g_interactive = now;
}

// sub_821F70D0(ref, ...): creates a sound from a cue name / id.
REX_HOOK_RAW(sub_821F70D0)
{
  if (!Enabled())
  {
    __imp__sub_821F70D0(ctx, base);
    return;
  }
  const uint32_t lr = uint32_t(ctx.lr);
  const uint32_t ref = ctx.r3.u32;
  const std::string name = GuestString(base, ref, 255);
  const bool music = name.rfind("XACT_SOUNDBANK_MUSIC_", 0) == 0;
  const bool layer = lr == kLayerCueReturn;
  const uint32_t id = LoadBe32(base, ref + kSoundRefId);
  const uint32_t layer_address = ctx.r31.u32;
  __imp__sub_821F70D0(ctx, base);
  if (music)
  {
    REXLOG_INFO("pdz_music: [{:8.2f}] cue \"{}\" -> sound {:08X}{}", Seconds(), name, ctx.r3.u32,
                ctx.r3.u32 ? "" : " (FAILED)");
  }
  else if (layer)
  {
    const uint32_t level = (layer_address - kInteractive) / kInteractiveLayerSize;
    REXLOG_INFO("pdz_music: [{:8.2f}] interactive layer {} id {} -> sound {:08X}{}", Seconds(),
                LevelName(base, level), id, ctx.r3.u32, ctx.r3.u32 ? "" : " (FAILED)");
  }
}

REX_HOOK_RAW(sub_821F8A78)
{
  if (Enabled())
  {
    REXLOG_INFO("pdz_music: [{:8.2f}] pause (from {:08X})", Seconds(), uint32_t(ctx.lr));
  }
  __imp__sub_821F8A78(ctx, base);
  mp_music::Pause(ctx, base);
}

REX_HOOK_RAW(sub_821F8AC0)
{
  if (Enabled())
  {
    REXLOG_INFO("pdz_music: [{:8.2f}] resume (from {:08X})", Seconds(), uint32_t(ctx.lr));
  }
  __imp__sub_821F8AC0(ctx, base);
  mp_music::Resume(ctx, base);
}

REX_HOOK_RAW(sub_821F8570)
{
  if (Enabled())
  {
    REXLOG_INFO("pdz_music: [{:8.2f}] stop (from {:08X})", Seconds(), uint32_t(ctx.lr));
  }
  __imp__sub_821F8570(ctx, base);
}
