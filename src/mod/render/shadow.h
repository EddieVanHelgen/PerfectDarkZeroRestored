// Shadow-mode recorder (stage 0 of the native renderer plan): public switch
// and the cold entry points. See README.md in this folder.
//
// In the real build the two REXCVAR_DECLARE lines below would move into
// src\game_cvars.h and the definitions in shadow_cvars.cpp into
// src\game_cvars.cpp, following the game's pattern (pdz_tp_enable etc.).
#pragma once

#include <cstdint>

#include <rex/cvar.h>

#include "gpu_sync.h"

REXCVAR_DECLARE(bool, pdz_render_shadow);
REXCVAR_DECLARE(int32_t, pdz_render_shadow_dump_frames);
REXCVAR_DECLARE(bool, pdz_render_shadow_assert);
REXCVAR_DECLARE(bool, pdz_render_shadow_vs_ucode_erase);
REXCVAR_DECLARE(bool, pdz_native_renderer);

namespace pdz::shadow
{
  // Two or three relaxed loads; the only cost of a hook while the recorder is off. The native renderer is driven by
  // the recorded draws, so pdz_native_renderer turns the recorder on by itself (launched without pdz_render_shadow
  // the native output had no draws: playtest 2026-10-26), and with graphics_backend null (gpu_sync.h) it is always
  // on: the native renderer is the only output.
  inline bool Enabled()
  {
    return REXCVAR_GET(pdz_render_shadow) || REXCVAR_GET(pdz_native_renderer) || pdz::gpusync::NullBackend();
  }

  // Cold path, called from the Swap hook after the original Swap returned:
  // closes the frame's counters, advances the frame number and applies the
  // dump policy (shadow_dump.cpp).
  void OnSwap(uint8_t *base);

  // Cold path: writes every ring record of `frame` to the log (REXLOG_INFO),
  // decoding the key registers. First call also lists the shader and vertex
  // declaration tables.
  void DumpFrame(uint8_t *base, uint32_t frame);
} // namespace pdz::shadow
