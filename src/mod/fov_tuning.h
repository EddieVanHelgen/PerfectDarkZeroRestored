// Field of view (fov_tuning.cpp).
#pragma once
#include <cstdint>

namespace pdz::fov
{
  // pdz_fov_scale for a player whose gameplay camera it widened (local players), else 1.
  double ScaleFor(uint32_t player);
} // namespace pdz::fov
