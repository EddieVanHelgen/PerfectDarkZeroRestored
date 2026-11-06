// Audio presence log (pdz_audio_presence_log, diagnostic, off by default;
// needs log_level "info"). tu3-base only; TU3 addresses. Read-only: it changes
// nothing in the game. For "this area is silent" reports
// (docs/BUG_AUDIT.md, "Missing sounds in levels").
//
// - Each sound cue's first start, first refusal and first stop in a session
//   (pdz_audio: lines): name, XACT category, position when the sound is
//   positioned. A DarkAudioSound keeps its name at +52, the resolved cue at
//   +308, the category (the cue's low 6 bits, written by Prime) at +312, its
//   flags at +336 (bit 3 = positioned), its position at +324 and its state at
//   +340 (6 = refused). Starts and refusals come from the DarkAudioSound::Prime
//   hook (sub_82205C68, in cue_fixes.cpp: 0 back = not started: no free cue,
//   or DarkAudioSoundManager::RequestToPlay declined it); stops from
//   sub_822055C0.
// - Every 5 s: the four ambience slots (82D3ECC8, 8 bytes each: sound, play
//   flag +4, Ambivol percent +5; the pdz.xgs RPC maps Ambivol 0 -> -96 dB,
//   1 -> 0 dB, and every ambience starts at 0 until the level fades its slot
//   up), the music manager state (82E682C8: type +176, current / wanted
//   interactive level +168 / +172, player state +148, layer cue handles +52 /
//   +100) and how many sounds of each category started in those 5 s.
//
// Wave channel counts aren't read at run time: out/audio/cue_channels.tsv
// lists them per cue.
#include "audio_presence_log.h"

#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>

#include <fmt/format.h>

#include <rex/hook.h>
#include <rex/logging.h>

#include "../game_cvars.h"
#include "hook_profile.h" // pdz_hook_profile (must follow <rex/hook.h>)

REX_EXTERN(__imp__sub_822055C0);

namespace
{
  using Clock = std::chrono::steady_clock;

  constexpr uint32_t kAmbienceSlots = 0x82D3ECC8;
  constexpr uint32_t kMusic = 0x82E682C8;

  // Categories in pdz.xgs order (the cue handle's low 6 bits).
  const char *const kCategories[] = {
      "Global", "Default", "Music", "footsteps", "gunshots", "richochets", "shelldrops",
      "speech_characters", "Tanoys", "speech_level", "effect", "Vehicle", "Ambience", "GunBits",
      "Radios", "LevelTunes", "frontend", "speech_radio", "speech_comms", "cutscenes",
      "FrontendTunes", "CutsceneTunes", "cutscenes_front", "cutscenes_rear", "speech_cutscenes"};

  std::mutex g_mutex;
  std::set<std::string> g_started, g_refused, g_stopped;
  std::map<std::string, int> g_window; // category -> starts since the last summary
  int g_window_refused = 0;
  Clock::time_point g_last;
  bool g_have_last = false;

  uint32_t Load32(const uint8_t *base, uint32_t address)
  {
    const uint8_t *p = base + address;
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
  }

  float LoadFloat(const uint8_t *base, uint32_t address)
  {
    const uint32_t bits = Load32(base, address);
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
  }

  std::string Name(const uint8_t *base, uint32_t sound)
  {
    if (!sound)
    {
      return "-";
    }
    const char *text = reinterpret_cast<const char *>(base + sound + 52);
    std::string name(text, strnlen(text, 255));
    if (name.rfind("XACT_SOUNDBANK_", 0) == 0)
    {
      name.erase(0, 15);
    }
    return name.empty() ? "?" : name;
  }

  const char *Category(uint32_t index)
  {
    return index < sizeof(kCategories) / sizeof(kCategories[0]) ? kCategories[index] : "?";
  }

  std::string Where(const uint8_t *base, uint32_t sound)
  {
    if (!((Load32(base, sound + 336) >> 3) & 1))
    {
      return "not positioned";
    }
    return fmt::format("at ({:.0f}, {:.0f}, {:.0f})", LoadFloat(base, sound + 324),
                       LoadFloat(base, sound + 328), LoadFloat(base, sound + 332));
  }
} // namespace

namespace audio_presence_log
{
  void OnPrime(uint8_t *base, uint32_t sound, uint32_t result)
  {
    if (!REXCVAR_GET(pdz_audio_presence_log) || !sound)
    {
      return;
    }
    const std::string name = Name(base, sound);
    const uint32_t category = Load32(base, sound + 312) & 0x3F;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (result)
    {
      g_window[Category(category)]++;
      if (g_started.insert(name).second)
      {
        REXLOG_INFO("pdz_audio: start {} [{}] {}", name, Category(category), Where(base, sound));
      }
    }
    else
    {
      g_window_refused++;
      if (g_refused.insert(name).second)
      {
        REXLOG_INFO("pdz_audio: refused {} [{}] state {} {}", name, Category(category),
                    Load32(base, sound + 340), Where(base, sound));
      }
    }
  }

  void Tick(uint8_t *base)
  {
    if (!REXCVAR_GET(pdz_audio_presence_log))
    {
      return;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    const Clock::time_point now = Clock::now();
    if (!g_have_last)
    {
      g_have_last = true;
      g_last = now;
      return;
    }
    if (now - g_last < std::chrono::seconds(5))
    {
      return;
    }
    g_last = now;
    std::string slots;
    for (uint32_t i = 0; i < 4; ++i)
    {
      const uint32_t slot = kAmbienceSlots + 8 * i;
      const uint32_t sound = Load32(base, slot);
      slots += fmt::format("{}{}: {} (play {}, Ambivol {}%)", i ? "; " : "", i + 1, Name(base, sound),
                           base[slot + 4], base[slot + 5]);
    }
    std::string started;
    for (const auto &[category, count] : g_window)
    {
      started += fmt::format("{}{} {}", started.empty() ? "" : ", ", category, count);
    }
    REXLOG_INFO("pdz_audio: 5 s: ambience {}", slots);
    REXLOG_INFO("pdz_audio: 5 s: music type {}, level {} -> {}, player state {}, layer cues {:08X} / {:08X}; "
                "started: {}; refused {}",
                Load32(base, kMusic + 176), Load32(base, kMusic + 168), Load32(base, kMusic + 172),
                Load32(base, kMusic + 148), Load32(base, kMusic + 52), Load32(base, kMusic + 100),
                started.empty() ? "none" : started, g_window_refused);
    g_window.clear();
    g_window_refused = 0;
  }
} // namespace audio_presence_log

// DarkAudioSound stop (sub_822055C0, r3 = the sound): first stop per name.
REX_HOOK_RAW(sub_822055C0)
{
  if (REXCVAR_GET(pdz_audio_presence_log) && ctx.r3.u32)
  {
    const std::string name = Name(base, ctx.r3.u32);
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_stopped.insert(name).second)
    {
      REXLOG_INFO("pdz_audio: stop {} [{}]", name, Category(Load32(base, ctx.r3.u32 + 312) & 0x3F));
    }
  }
  __imp__sub_822055C0(ctx, base);
}
