// Shadow-mode recorder switches. Pattern: src\game_cvars.cpp
// (REXCVAR_DEFINE_BOOL(name, default, category, description)); in the real
// build these two definitions join that file and the declarations join
// src\game_cvars.h.
#include "shadow.h"

REXCVAR_DEFINE_BOOL(pdz_render_shadow, false, "Render",
                    "Shadow-mode recorder (native renderer stage 0): record every D3D draw's "
                    "state from the guest device struct into a ring buffer; the emulator keeps "
                    "rendering as usual. Costs one struct copy per draw while on.");

REXCVAR_DEFINE_INT32(pdz_render_shadow_dump_frames, 0, "Render",
                     "With pdz_render_shadow on: write the ring records of one frame to the log "
                     "every N swaps (0 = only the first full frame after the recorder is switched on). Was 600: "
                     "each dump is ~4,100 log lines on the game thread at the Swap, a 25-30 ms frame every 10 s in "
                     "heavy MP with the native renderer (native/stage2/m5/m5_texstall.md); pass 600 for the old periodic dumps")
    .range(0, 1000000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_render_shadow_vs_ucode_erase, true, "Render",
                    "Shadow recorder: the created microcode kept per vertex shader object (for the native binder) is "
                    "dropped when the object's shader-table entry is overwritten by another object (the oldest entry of "
                    "a full probe window) or its address is re-created as a pixel shader; until now such copies stayed "
                    "for the session (memlog.md N6). The Create hooks then take the native render lock while they "
                    "update the tables, which the native draw path reads under it (a loading thread used to write the "
                    "kept-microcode map while the render thread searched it). Off = never dropped, Create hooks "
                    "unlocked (the old behaviour).");

REXCVAR_DEFINE_BOOL(pdz_render_shadow_assert, false, "Render",
                    "With pdz_render_shadow on: compare every draw's record with the PM4 register runs "
                    "D3D's own state flush (sub_826FA8B8) writes into the command ring; first 50 "
                    "mismatches and a per-frame summary line go to the log (shadow_assert.cpp). "
                    "Debugging only: parses the flush output of every draw.");
