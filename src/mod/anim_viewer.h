// Animation viewer (pdz_debug_anim_viewer). tu3-base only.
#pragma once

namespace rex::ui
{
  class ImGuiDrawer;
}

namespace anim_viewer
{
  // Registers bind_anim_viewer_next (PageDown), bind_anim_viewer_prev (PageUp)
  // and bind_anim_viewer_reset (End; F3 / F4 / F7 are the SDK's debug,
  // settings and achievements overlays) and the label showing the animation's name.
  void Init(rex::ui::ImGuiDrawer *drawer);
} // namespace anim_viewer
