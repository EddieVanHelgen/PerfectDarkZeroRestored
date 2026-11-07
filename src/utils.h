// utils.h - Utility functions for path handling and config loading
#pragma once

#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include <rex/cvar.h>
#include <rex/filesystem.h>

namespace utils
{
  // Find the repository root by walking up from the executable location
  static std::filesystem::path RepoRoot()
  {
    namespace fs = std::filesystem;
    fs::path dir = rex::filesystem::GetExecutableFolder();
    for (int i = 0; i < 6 && !dir.empty() && dir.has_parent_path(); ++i)
    {
      // A release folder (2026-11-03): no repo, but settings\hardware.toml next to the exe; since 0.9.2 the zip
      // ships settings\defaults\ instead (EnsureSettingsFiles makes the player's files from it).
      if (fs::exists(dir / "perfectdarkzerorecomp_manifest.toml") || fs::exists(dir / "assets") ||
          fs::exists(dir / "settings" / "hardware.toml") || fs::exists(dir / "settings" / "defaults" / "hardware.toml"))
      {
        break;
      }
      dir = dir.parent_path();
    }
    return dir;
  }

  // Get the settings directory path
  static std::filesystem::path SettingsDir() { return RepoRoot() / "settings"; }

  // 0.9.2: the release zip ships the default settings as settings\defaults\hardware.toml and mapping.toml, so a new
  // version unzipped over an old folder never replaces the player's own settings and key bindings. Called before
  // anything reads the settings: a missing settings\<file> is created as a copy of settings\defaults\<file>; one that
  // exists is never touched. Returns one log line per file (the log isn't up yet: OnPostInitLogging writes them).
  static std::vector<std::string> EnsureSettingsFiles()
  {
    namespace fs = std::filesystem;
    std::vector<std::string> lines;
    const fs::path dir = SettingsDir();
    for (const char *file : {"hardware.toml", "mapping.toml"})
    {
      std::error_code ec;
      const fs::path own = dir / file, defaults = dir / "defaults" / file;
      if (fs::exists(own, ec))
      {
        lines.push_back("pdz_settings: " + own.string() + ": the player's own file, kept as it is");
      }
      else if (!fs::exists(defaults, ec))
      {
        lines.push_back("pdz_settings: " + own.string() + " is missing and there is no " + defaults.string() +
                        ": the built-in defaults apply");
      }
      else if (fs::copy_file(defaults, own, fs::copy_options::none, ec))
      {
        lines.push_back("pdz_settings: " + own.string() + " was missing: created from " + defaults.string());
      }
      else
      {
        lines.push_back("pdz_settings: " + own.string() + " was missing and couldn't be created from " +
                        defaults.string() + " (" + ec.message() + "): the built-in defaults apply");
      }
    }
    return lines;
  }

  // Load all settings files from the settings directory
  static void LoadSettingsFiles()
  {
    namespace fs = std::filesystem;
    fs::path dir = SettingsDir();
    for (const char *file : {"hardware.toml", "mapping.toml"})
    {
      if (fs::path p = dir / file; fs::exists(p))
      {
        rex::cvar::LoadConfig(p);
      }
    }
  }
} // namespace utils
