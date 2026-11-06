// Restorations (pdz_restore_*): see restorations.cpp.
#pragma once

#include <cstdint>

struct PPCContext;

namespace restorations
{
  // Called every audio tick from src/music/music_restore.cpp: delayed lines
  // (pdz_restore_light_speech).
  void Tick(PPCContext &ctx, uint8_t *base);

  // pdz_test_lab_blackout_key (testing tool): registers the Home key.
  void RegisterBinds();
} // namespace restorations
