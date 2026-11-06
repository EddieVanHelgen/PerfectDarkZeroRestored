// Game-specific patches (tu3-base: the 60 fps unlock only).
#pragma once

namespace game_patches
{
    enum class PatchId
    {
        Fps60Unlock
    };

    bool IsEnabled(PatchId patch);
} // namespace game_patches
