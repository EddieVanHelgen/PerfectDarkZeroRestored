// PM4 register runs the null GPU backend never consumes (pm4_skip.cpp; stage 2 M5, pdz_native_skip_pm4; default 1 in
// the performance pass, logs 252-259, back to 0 on 2026-10-30: in play it gave regular ~32 ms hitches every 1-10 s, all
// in wait.guest_fence (logs 276-277), gone with 0 (log 278), for a 0.1-0.3 ms saving; the code stays behind the cvar;
// design and decision: PDZRenderResearch\native\stage2\m5\m5_skip_pm4.md).
//
// With graphics_backend null the native renderer is the only renderer: it reads the guest D3D device struct (the
// shadow registers, constants and dirty masks) from its hooks, never the PM4 ring. The null command processor parses
// the ring for its synchronisation only (fences, scratch write-back, WAIT_REG_MEM, INTERRUPT / InsertCallback, the
// presentation gate, COHER, DC_LUT gamma, XE_SWAP), and every one of those packets is written inline by its own D3D
// function. The context registers and the shader / fetch constants (about 85% of the ring's dwords: STAGE2_PLAN.md
// M0) reach the ring only through three leaf writers that copy shadow state into it, and nothing reads them back:
//   sub_826F9298 register run (r4 = 64-bit mask, 1 dword per bit, r5 = first register, r6 = shadow source - 4),
//   sub_826F9450 ALU constants (16 dwords per bit, 0x4000 VS / 0x4400 PS),
//   sub_826F9358 fetch constants (6 dwords per bit, 0x4800).
// Callers (all register bases 0x2000 and up): the state flush sub_826FA8B8 (every draw, Clear, Resolve), the internal
// draw state sub_826FAAD8 (Resolve, ClearF, SetShaderGPRAllocation's clear) and the full-state writer sub_826F8170
// (no direct caller). The leaves only advance device+0 (the ring write pointer) and write ring memory (overflow:
// sub_826F91E0 -> ring space sub_826DDE30); the callers clear the dirty masks themselves, so skipping a leaf leaves the
// device struct exactly as forwarding would, minus the ring copy.
//
// pdz_native_skip_pm4: 0 = off (the default; every hook forwards, one cvar load per call). 1 = skip: with the null GPU
// backend, a leaf call whose first register is 0x2000 or above returns without writing (config registers below
// 0x2000 would still be written: none in TU3); ignored with one line under the GPU plugin (it renders from those
// registers) and while pdz_render_shadow_assert is on (it compares the ring with the shadow). 2 = count only (any
// backend): forward everything and count / time what mode 1 would skip, for the A/B. Read at each guest Swap (a
// frame never mixes modes). Modes 1 / 2 log 'pdz_pm4:' lines every 600 Swaps and a SUMMARY at test exit.
#pragma once
#include <cstdint>

#include <rex/cvar.h>

REXCVAR_DECLARE(int32_t, pdz_native_skip_pm4);

namespace pdz::pm4skip
{
  // Swap hook (shadow_hooks.cpp), after the guest Swap returned: closes the frame's counters, latches the mode for the
  // next frame, logs the window line.
  void OnSwap();
  // Test exit (pdz::test::LogSummary): the SUMMARY line; prints nothing while the mode was never on.
  void LogSummary(const char *why);
} // namespace pdz::pm4skip
