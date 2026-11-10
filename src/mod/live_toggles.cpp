// In-game A/B switches for the gameplay and latency tweaks, so a change can be
// felt straight away without the settings overlay or a restart.
//
// bind_look_ab flips pdz_look_tuning: off, every look option behaves like the
// original game, but keeps its value for when it's switched back on.
// bind_aim_ab does the same with pdz_autoaim_tuning for the auto-aim options.
// bind_latency_ab swaps the latency options that apply at runtime between the
// current values and the originals. bind_ai_cover_ab flips pdz_ai_cover_fixes
// (the enemy cover fixes), bind_controls_ab flips pdz_controls_fixes (the
// player controls fixes), bind_game_fixes_ab flips pdz_game_fixes (the
// original-game bug fixes). Each press shows a label for a moment.
#include <array>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>

#include <fmt/format.h>
#include <imgui.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/keybinds.h>

#include "../game_cvars.h"
#include "live_toggles.h"
#include "ai_cover_fixes.h"
#include "controls_fixes.h"
#include "game_fixes.h"

namespace
{
  constexpr std::chrono::milliseconds kLabelTime{2500};
  // Held keys auto-repeat; only a press after a quiet gap counts.
  constexpr std::chrono::milliseconds kRepeatGap{300};

  std::mutex g_label_mutex;
  std::string g_label;
  std::chrono::steady_clock::time_point g_label_until;

  void ShowLabel(std::string text)
  {
    REXLOG_INFO("{}", text);
    std::lock_guard<std::mutex> lock(g_label_mutex);
    g_label = std::move(text);
    g_label_until = std::chrono::steady_clock::now() + kLabelTime;
  }

  // Always present, draws only while a label is showing. Not one of the SDK
  // overlays, so it never takes the controller away from the game.
  class LabelDialog final : public rex::ui::ImGuiDialog
  {
  public:
    explicit LabelDialog(rex::ui::ImGuiDrawer *drawer) : ImGuiDialog(drawer) {}

  protected:
    void OnDraw(ImGuiIO &io) override
    {
      std::string text;
      {
        std::lock_guard<std::mutex> lock(g_label_mutex);
        if (std::chrono::steady_clock::now() >= g_label_until)
        {
          return;
        }
        text = g_label;
      }
      ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.06f),
                              ImGuiCond_Always, ImVec2(0.5f, 0.0f));
      ImGui::SetNextWindowBgAlpha(0.75f);
      constexpr ImGuiWindowFlags kFlags =
          ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
          ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoFocusOnAppearing |
          ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings;
      if (ImGui::Begin("##pdz_live_toggle", nullptr, kFlags))
      {
        ImGui::TextUnformatted(text.c_str());
      }
      ImGui::End();
    }
  };

  std::unique_ptr<LabelDialog> g_dialog;

  bool FreshPress(std::chrono::steady_clock::time_point &last_event)
  {
    const auto now = std::chrono::steady_clock::now();
    const bool fresh = now - last_event > kRepeatGap;
    last_event = now;
    return fresh;
  }

  std::string LookSummary()
  {
    std::string text = fmt::format(
        "speed {:g}, pitch {:g}, accel {:g}", REXCVAR_GET(pdz_turn_speed_scale),
        REXCVAR_GET(pdz_pitch_speed_scale), REXCVAR_GET(pdz_turn_accel_scale));
    if (REXCVAR_GET(pdz_stick_curve) > 0.0)
    {
      text += fmt::format(", curve {:g}", REXCVAR_GET(pdz_stick_curve));
    }
    if (REXCVAR_GET(pdz_stick_deadzone) >= 0.0)
    {
      text += fmt::format(", deadzone {:g}", REXCVAR_GET(pdz_stick_deadzone));
    }
    if (REXCVAR_GET(pdz_look_deadzone_scale) != 1.0)
    {
      text += fmt::format(", look deadzone {:g}", REXCVAR_GET(pdz_look_deadzone_scale));
    }
    if (REXCVAR_GET(pdz_stick_radial))
    {
      text += ", radial";
    }
    if (REXCVAR_GET(pdz_fastlook_zoom_brake))
    {
      text += ", zoom brake";
    }
    if (REXCVAR_GET(pdz_fastlook_exit) > 0.95)
    {
      text += fmt::format(", fast-look exit {:g}", REXCVAR_GET(pdz_fastlook_exit));
    }
    return text;
  }

  std::string AimSummary()
  {
    if (!REXCVAR_GET(pdz_autoaim_enable))
    {
      return "auto-aim off";
    }
    std::string text = fmt::format("cone {:g}", REXCVAR_GET(pdz_autoaim_cone_scale));
    if (REXCVAR_GET(pdz_autoaim_min_radius_cm) > 0.0)
    {
      text += fmt::format(", min radius {:g} cm", REXCVAR_GET(pdz_autoaim_min_radius_cm));
    }
    if (REXCVAR_GET(pdz_autoaim_strength_scale) != 1.0)
    {
      text += fmt::format(", strength {:g}", REXCVAR_GET(pdz_autoaim_strength_scale));
    }
    if (REXCVAR_GET(pdz_aim_friction) > 0.0)
    {
      text += fmt::format(", friction {:g}", REXCVAR_GET(pdz_aim_friction));
    }
    if (REXCVAR_GET(pdz_aim_still_lock))
    {
      text += ", still lock";
    }
    if (REXCVAR_GET(pdz_aim_respect_headshots))
    {
      text += ", headshots kept";
    }
    if (REXCVAR_GET(pdz_aim_assist_zoomed))
    {
      text += ", zoomed assist";
    }
    if (REXCVAR_GET(pdz_autoaim_zoom_falloff))
    {
      text += fmt::format(", zoom falloff {:g}", REXCVAR_GET(pdz_autoaim_zoom_radius));
    }
    return text;
  }

  // Latency options that apply at runtime (SDK cvars, set by name), and their
  // original values (the first two are on by default now, so not the defaults).
  constexpr std::array<const char *, 3> kLatencyCvars = {
      "host_vblank_late_latch_ms", "input_poll_on_read", "gpu_max_frames_in_flight"};
  constexpr std::array<const char *, 3> kLatencyOriginals = {"0", "false", "3"};
  bool g_latency_original = false;
  std::array<std::string, kLatencyCvars.size()> g_latency_saved;

  std::string LatencySummary()
  {
    std::string text;
    for (const char *name : kLatencyCvars)
    {
      text += fmt::format("{}{} {}", text.empty() ? "" : ", ", name, rex::cvar::GetFlagByName(name));
    }
    return text;
  }
} // namespace

namespace live_toggles
{
  void Init(rex::ui::ImGuiDrawer *drawer)
  {
    g_dialog = std::make_unique<LabelDialog>(drawer);

    rex::ui::RegisterBind("bind_look_ab", "F6", "Switch the look tweaks on/off (A/B)", []
                          {
      static auto last_event = std::chrono::steady_clock::time_point{};
      if (!FreshPress(last_event))
      {
        return;
      }
      const bool on = !REXCVAR_GET(pdz_look_tuning);
      REXCVAR_SET(pdz_look_tuning, on);
      ShowLabel(on ? "Look tweaks ON: " + LookSummary() : std::string("Look tweaks OFF (original)")); });

    rex::ui::RegisterBind("bind_aim_ab", "F10", "Switch the auto-aim tweaks on/off (A/B)", []
                          {
      static auto last_event = std::chrono::steady_clock::time_point{};
      if (!FreshPress(last_event))
      {
        return;
      }
      const bool on = !REXCVAR_GET(pdz_autoaim_tuning);
      REXCVAR_SET(pdz_autoaim_tuning, on);
      ShowLabel(on ? "Aim assist tweaks ON: " + AimSummary() : std::string("Aim assist tweaks OFF (original)")); });

    rex::ui::RegisterBind("bind_ai_cover_ab", "F5", "Switch the enemy cover fixes on/off (A/B)", []
                          {
      static auto last_event = std::chrono::steady_clock::time_point{};
      if (!FreshPress(last_event))
      {
        return;
      }
      const bool on = !REXCVAR_GET(pdz_ai_cover_fixes);
      REXCVAR_SET(pdz_ai_cover_fixes, on);
      ShowLabel(on ? "Enemy cover fixes ON: " + ai_cover_fixes::ActiveSummary()
                   : std::string("Enemy cover fixes OFF (original)")); });

    rex::ui::RegisterBind("bind_controls_ab", "F2", "Switch the controls fixes on/off (A/B)", []
                          {
      static auto last_event = std::chrono::steady_clock::time_point{};
      if (!FreshPress(last_event))
      {
        return;
      }
      const bool on = !REXCVAR_GET(pdz_controls_fixes);
      REXCVAR_SET(pdz_controls_fixes, on);
      ShowLabel(on ? "Controls fixes ON: " + controls_fixes::ActiveSummary()
                   : std::string("Controls fixes OFF (original)")); });

    rex::ui::RegisterBind("bind_game_fixes_ab", "F11", "Switch the game bug fixes on/off (A/B)", []
                          {
      static auto last_event = std::chrono::steady_clock::time_point{};
      if (!FreshPress(last_event))
      {
        return;
      }
      const bool on = !REXCVAR_GET(pdz_game_fixes);
      REXCVAR_SET(pdz_game_fixes, on);
      ShowLabel(on ? "Game fixes ON: " + game_fixes::ActiveSummary() : std::string("Game fixes OFF (original)")); });

    rex::ui::RegisterBind("bind_latency_ab", "F8", "Switch the latency tweaks on/off (A/B)", []
                          {
      static auto last_event = std::chrono::steady_clock::time_point{};
      if (!FreshPress(last_event))
      {
        return;
      }
      g_latency_original = !g_latency_original;
      for (size_t i = 0; i < kLatencyCvars.size(); ++i)
      {
        if (g_latency_original)
        {
          g_latency_saved[i] = rex::cvar::GetFlagByName(kLatencyCvars[i]);
          rex::cvar::SetFlagByName(kLatencyCvars[i], kLatencyOriginals[i]);
        }
        else
        {
          rex::cvar::SetFlagByName(kLatencyCvars[i], g_latency_saved[i]);
        }
      }
      ShowLabel(g_latency_original ? std::string("Latency tweaks OFF (original)")
                                   : "Latency tweaks ON: " + LatencySummary()); });
  }
} // namespace live_toggles
