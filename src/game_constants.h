// game_constants.h - Game-specific constants (tu3-base: Title Update 3 addresses)
#pragma once

#include <cstdint>

namespace GameConstants
{
  constexpr uint32_t kCodeBase = 0x82140000;
  constexpr uint32_t kCodeEnd = 0x82CEE27C;
  constexpr uint32_t kImageBase = 0x82000000;
  constexpr uint32_t kImageSize = 0x013E0000;
}

namespace GameConstants::PatchConstants
{
  struct RegisterPatch
  {
    std::uintptr_t call_site;
    std::uint32_t preserve_mask;
    std::uint32_t set_bits;
  };

  // Return address of T sub_826DF7F0's call of sub_826DE408 (launch 826CFBB0).
  constexpr RegisterPatch Fps60()
  {
    return RegisterPatch{
        0x826DF900,
        0x0000FFFF,
        0x00010000};
  }
}
