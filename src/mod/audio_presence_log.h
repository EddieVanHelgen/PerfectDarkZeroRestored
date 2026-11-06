// pdz_audio_presence_log (tu3-base, diagnostic, off by default).
#pragma once

#include <cstdint>

namespace audio_presence_log
{
  // From the DarkAudioSound::Prime hook (cue_fixes.cpp): `sound` is the
  // DarkAudioSound, `result` Prime's return (0 = not started).
  void OnPrime(uint8_t *base, uint32_t sound, uint32_t result);

  // From the audio tick (music_restore.cpp): the 5 s summary.
  void Tick(uint8_t *base);
} // namespace audio_presence_log
