// Step zoom (zoom_steps.cpp).
#pragma once
#include <cstdint>

namespace pdz::zoom
{
  // The buttons the game's XInputGetState returned for a user (test_harness.cpp's hook, after the input script).
  void OnPadButtons(uint32_t user, uint16_t buttons);
} // namespace pdz::zoom
