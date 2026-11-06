// Game patches for the Title Update 3 build (tu3-base). Only the 60 fps
// unlock is ported; the launch branch's 16:9 patch, user_language fix and
// mouse look hold are not (docs/TU3_BASE.md, re-targeting checklist).
#include <cstdint>

#include <rex/cvar.h>
#include <rex/hook.h>

#include "game_cvars.h"
#include "game_constants.h"
#include "game_patches.h"
#include "mod/render/game_pace.h" // pdz_null_refresh_hz forces the 1-vblank interval

namespace game_patches
{
  bool IsEnabled(PatchId patch)
  {
    switch (patch)
    {
    case PatchId::Fps60Unlock:
      // Stage 2 M5: at a paced guest refresh above 60 Hz (pdz_null_refresh_hz) the interval must be 1 vblank too.
      return REXCVAR_GET(pdz_fps60_unlock) || pdz::pace::ForcesInterval1();
    }
    return false;
  }
} // namespace game_patches

// 60 fps: the call that sets the presentation interval (T sub_826DF7F0's
// call of sub_826DE408; launch sub_826CFAA0 -> sub_826CE6B8) gets interval 1.
REX_EXTERN(__imp__sub_826DE408);

REX_HOOK_RAW(sub_826DE408)
{
  const auto patch = GameConstants::PatchConstants::Fps60();
  if (ctx.lr == patch.call_site &&
      game_patches::IsEnabled(game_patches::PatchId::Fps60Unlock))
  {
    ctx.r7.u32 = (ctx.r7.u32 & patch.preserve_mask) | patch.set_bits;
  }

  __imp__sub_826DE408(ctx, base);
}
