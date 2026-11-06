// Shadow-mode assertion against D3D's own state flush (shadow_assert.cpp,
// plan in shadow_assert.md). The sub_826FA8B8 hook lives in shadow_assert.cpp
// and needs no wiring; the two entry points below are optional call sites for
// shadow_hooks.cpp (see ASSERT_WIRING.md). Switch: pdz_render_shadow_assert
// (bool, off; requires pdz_render_shadow).
#pragma once

#include <cstdint>

#include "shadow.h"

namespace pdz::shadow
{
  // One relaxed load; the only cost of the assertion while it is off.
  inline bool AssertEnabled() { return REXCVAR_GET(pdz_render_shadow_assert) && Enabled(); }

  // Cold path, Swap hook after OnSwap(base): writes the per-frame summary line
  // "shadow assert: N draws, M registers compared, K mismatches (top: ...)" and
  // resets the frame counters. Without this call the summary goes out at the
  // next frame's first flush instead.
  void AssertOnSwap();

  // Cold path: per-register match / mismatch totals and the unpaired flush
  // callers (REXLOG_INFO). AssertOnSwap calls it every
  // pdz_render_shadow_dump_frames swaps.
  void AssertDumpTotals();

  // Optional, draw hooks after the original returned (DrawIndexedVertices,
  // DrawVertices, DrawVerticesUP, BeginVertices): checks the draw packet's
  // VGT_DRAW_INITIATOR (primitive type, count) against the record's arguments.
  // Only the flush hook's state is used, so the hook need not keep r3.
  void AssertOnDrawReturn(uint8_t *base);
} // namespace pdz::shadow
