// GPU census (native renderer stage 2, milestone M0; STAGE2_PLAN.md section 4 "M0"): with pdz_gpu_census on, logs
// per run what the game uses of the emulator's GPU path that a native-only renderer would have to provide, with the
// GPU plugin still rendering and nothing else changed.
//
//   SDK side (rex/graphics/gpu_census.h, local SDK commit on pdz-m0): every PM4 packet by type / opcode / register
//   range / source (primary ring or indirect buffer), predication, the registers written below the constants, the
//   memory the command processor writes for the CPU (fences, scratch and read-pointer write-back, extents, ZPD,
//   MEM_WRITE / COND_WRITE / REG_TO_MEM) by address, WAIT_REG_MEM by poll target with the time waited, CP and vblank
//   interrupts, the CPU's MMIO accesses, draws by primitive / EDRAM mode, resolves and resolve readbacks, memexport.
//   The switch is set from here (a change callback on pdz_gpu_census, and every Swap as a fallback).
//   Title side (this file): D3D entry points that reach the ring without the shadow hooks: the D3D state flush
//   sub_826FA8B8 paired with the shadow ring like the assert does (an unpaired flush is counted by return
//   address), the unhooked draw entries (sub_826EA688, sub_826EA3C8, sub_82700A68), BeginIndexedVertices,
//   ClearF called directly, SetShaderGPRAllocation (D3D's 24 one-vertex point draws), the HiZ flush event,
//   InsertCallback (by caller and callback), the texture lock and BlockUntilNotBusy (CPU access to GPU memory, by
//   caller), the CPU's fence waits and BlockUntilIdle (by caller, with the time blocked), the hooked draw / clear /
//   resolve totals from the shadow recorder (pdz_render_shadow), the D3D vblank counter and vblank callback, and
//   the native de-rez snapshot readbacks.
//
// Output: a "pdz_census:" line every pdz_gpu_census_every swaps (per-frame averages over the window) and, at test
// exit (pdz::test::LogSummary), one "pdz_census: SUMMARY" line followed by "pdz_census: total ..." detail lines.
//
// Cost while off: one relaxed cvar load per hooked call (the new counter hooks forward unchanged), one per Swap,
// and in the GPU plugin one relaxed load per ring burst and per packet. While on: counting and logging only.
#pragma once
#include <cstdint>

#include <rex/cvar.h>

REXCVAR_DECLARE(bool, pdz_gpu_census);

namespace pdz::census
{
  // One relaxed load.
  inline bool Enabled() { return REXCVAR_GET(pdz_gpu_census); }

  // D3D state flush sub_826FA8B8 (the hook in shadow_assert.cpp), before forwarding: r3 device, lr = the flush's
  // return address (identifies the D3D function that flushed).
  void OnFlush(uint32_t device, uint32_t lr);

  // D3DDevice_InsertCallback sub_826DE910 (the hook in native_surfaces.cpp), before forwarding: the caller's LR,
  // r4 type, r5 callback.
  void OnInsertCallback(uint32_t lr, uint32_t type, uint32_t callback);

  // Swap hook (shadow_hooks.cpp), after the shadow recorder's OnSwap: syncs the SDK switch, adds the frame's hooked
  // call counts, samples the D3D vblank fields and writes the periodic line. Checks its own switch.
  void OnSwap(uint8_t *base, uint32_t device);

  // The SUMMARY line and the detail lines (pdz::test::LogSummary); prints nothing while the census is off.
  void LogSummary(const char *why);
} // namespace pdz::census
