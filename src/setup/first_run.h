// PDZ Restored: first-run setup (2026-11-03, docs/RELEASE_FIRST_RUN.md Stage 1).
//
// A new player supplies their own Perfect Dark Zero disc dump (an ISO, or the extracted files) and Title Update 3
// (the package as copied from an Xbox 360, or extracted). On a start with no usable game data, the exe finds them (the game\ folder
// next to the exe, else a file picker), checks they are the versions this build was made from, extracts what the game
// needs into <data dir>\game (the disc, with TU3's default.xexp) and <data dir>\update (TU3's data package), and
// writes <data dir>\setup.json. That stamp keeps it from running again unless the files change.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace pdz::first_run
{
  struct Paths
  {
    std::filesystem::path game_root;   // game:\ (default.xex, default.xexp, pdz\)
    std::filesystem::path update_root; // update:\ (TU3's data package)
  };

  // From the app's OnConfigurePaths, after the default paths are known. `current` holds them (the repo's assets and
  // <user data root>\update on a developer's machine). `explicit_paths`: --game_data_root / --update_data_root was
  // given, so nothing is changed. Returns the paths to use. On a problem the player can't get past here (wrong files,
  // setup cancelled, no space), a message has been shown and the process exits with code 2.
  Paths Prepare(const std::filesystem::path &exe_dir, const std::filesystem::path &user_data_root, const Paths &current,
                bool explicit_paths);

  // At load (OnPostLoadXexImage): the patched executable image must be the one the build is recompiled from (any
  // supported pressing with its own TU3 gives it). Otherwise a message, the prepared stamp set aside, exit 2.
  void VerifyImage(const uint8_t *image, size_t size, const std::filesystem::path &game_root);

  // After logging starts: what setup found or did (one or two lines).
  void LogSummary();

  // The first-run setup prepared the game files at this start (the settings window then opens on Map packs).
  bool SetupRanThisStart();
} // namespace pdz::first_run
