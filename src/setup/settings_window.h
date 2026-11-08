// PDZ Restored: the settings launch window (2026-11-04). A standard Windows window (native controls, the system's
// look) before the game starts: Display, Frame rate, Map packs, Controls, Advanced; Play and Safe mode. It writes only
// the lines it owns (settings\hardware.toml: display, frame rate, network, the window itself; settings\mapping.toml:
// mouse sensitivity, invert and the Controls tab's changed keybind lines, comments kept) and sets them for this start too.
//
// When it shows: after the first-run setup (on the Map packs tab: the packs are setup's optional last step), on every
// start until "Don't show this again" is ticked (pdz_settings_show), when Shift is held at launch, and with
// --pdz_settings_open (the "PDZ Restored Settings" launcher). Never in a developer's build or with pdz_setup_quiet.
#pragma once

#include <filesystem>

namespace pdz::settings_ui
{
  struct Context
  {
    std::filesystem::path settings_file; // settings\hardware.toml
    std::filesystem::path mapping_file;  // settings\mapping.toml
    std::filesystem::path logs_dir;      // logs\ next to the exe
    std::filesystem::path user_data_root;
    std::filesystem::path cache_root;
    // The game files are the US Platinum Hits disc's (first_run::PlatinumDisc): both map packs are on the disc, so
    // the Map packs tab shows them as included and no pack package is installed on top.
    bool packs_on_disc = false;
  };

  enum class Choice
  {
    kPlay,
    kSafeMode,
    kQuit,
  };

  // Shows the window when it should (see above) and returns what the player chose; kPlay when it didn't show.
  Choice MaybeShow(const Context &context, bool after_first_setup);
  // After logging starts: what the window did (it runs before the log exists).
  void LogSummary();
} // namespace pdz::settings_ui
