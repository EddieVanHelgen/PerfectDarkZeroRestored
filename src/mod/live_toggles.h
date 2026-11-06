// In-game A/B switches for the gameplay and latency tweaks.
#pragma once

namespace rex::ui
{
  class ImGuiDrawer;
}

namespace live_toggles
{
  // Registers bind_look_ab (F6), bind_aim_ab (F10), bind_ai_cover_ab (F5),
  // bind_controls_ab (F2), bind_game_fixes_ab (F11) and bind_latency_ab (F8)
  // and the on-screen label they show.
  void Init(rex::ui::ImGuiDrawer *drawer);
} // namespace live_toggles
