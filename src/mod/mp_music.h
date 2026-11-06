// Multiplayer match music (pdz_mp_music): see mp_music.cpp.
#pragma once

#include <cstdint>

struct PPCContext;

namespace mp_music
{
  // Called every music tick from src/music/music_restore.cpp (tu3-base).
  void Tick(PPCContext &ctx, uint8_t *base);
  // Stops the track (the audio idle check before a level is left).
  void LeaveLevel(PPCContext &ctx, uint8_t *base);
} // namespace mp_music
