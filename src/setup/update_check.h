// PDZ Restored: the update check of the settings window (0.9.2).
//
// When the settings window opens (and "Check for updates" is ticked: pdz_update_check), a background thread asks
// GitHub for the latest release (GET https://api.github.com/repos/EddieVanHelgen/PerfectDarkZeroRestored/releases/
// latest, 3 s timeouts, User-Agent "PDZRestored/<version>", no other header and no data of any kind). A newer tag
// than this exe's version shows "Version X is available" and an "Open download page" button in the window, which
// opens that release's page in the browser. Nothing is ever downloaded or installed. Offline, no answer or a tag that
// can't be read: nothing is shown, one log line. The game's start never waits for it.
#pragma once

#include <string>

namespace pdz::update_check
{
  enum class State
  {
    kIdle,    // not started
    kPending, // asking
    kCurrent, // the latest release is this version or older
    kNewer,   // a newer release: tag, version and page are set
    kFailed,  // offline, no answer, or a tag that can't be read (one log line)
  };

  struct Result
  {
    State state = State::kIdle;
    std::string version; // the newer release's version, without the 'v' ("0.9.3")
    std::string page;    // its release page (github.com, this project only)
  };

  // Starts the check once (later calls do nothing). The caller has already checked pdz_update_check.
  void Start();
  Result Get();
  // The start-up log is up: the check's line is logged now, or when it comes.
  void LoggingReady();
} // namespace pdz::update_check
