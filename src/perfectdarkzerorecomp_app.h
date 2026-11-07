// perfectdarkzerorecomp - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include "pdz_version.h"
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/rex_app.h>
#include <rex/runtime.h>
#include <rex/system.h> // ShowSimpleMessageBox (the title update check)
#include <rex/system/gpu_plugin.h>

#include "debug_tools.h"
#include "game_cvars.h"
#include "mod/render/gpu_sync.h"
#include "mod/render/game_thread_sampler.h" // pdz_guest_profile's total at close
#include "mod/render/guest_ip_profile.h"    // pdz_guest_ip_profile's file at close
#include "mod/render/native/native.h" // ApplyNativeDefault (native renderer as the default)
#ifndef PDZ_NO_MODS
#include "local_profiles.h"
#include "mod/live_toggles.h"
#include "mod/third_person.h"
#else
#include "mod/anim_viewer.h"
#include "mod/restorations.h"
#include "mod/mouse_look.h" // pdz_mouse_look
#include "setup/settings_window.h" // the settings launch window
#endif
#include "utils.h"
#include "setup/first_run.h"
#include "mod/render/native/native_dxc.h"
#include "mod/render/native/native_generate.h"
#include "setup/setup_window.h"

const char *PdzBuildCommit(); // build_info.cpp
const char *PdzBuildTime();

#ifdef REXGLUE_ENABLE_PERF_COUNTERS
#include <rex/perf/counter.h>
#endif

namespace pdz::native
{
  void RegisterMarkBind(); // mod/render/native/native_present.cpp: bind_mark_spot (F12), the mark-this-spot test aid
}
namespace pdz::transwater
{
  void Init(rex::ui::ImGuiDrawer *drawer); // mod/transparency_water.cpp: F1 and its label
}

#ifdef PDZ_PGO_GEN
namespace pdz::pgo
{
  void WriteProfiles(); // src/pgo_flush.cpp (PDZ_PGO=gen builds only)
}
#endif

REXCVAR_DECLARE(bool, pdz_safe_mode);        // native_generate.cpp
REXCVAR_DEFINE_STRING(pdz_test_dump_image, "", "Testing",
                      "TESTING TOOL: write the loaded (title-update-patched) executable image to this file once the XEX is loaded, then exit (comparing disc pressings).");
REXCVAR_DECLARE(bool, pdz_setup_quiet);      // setup/first_run.cpp
REXCVAR_DECLARE(bool, pdz_native_renderer);  // native_cvars.cpp

class PerfectdarkzerorecompApp : public rex::ReXApp
{
public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext &ctx)
  {
    // The release name (2026-11-03): PDZRestored.exe. The SDK takes this name for the window title, the log files
    // (PDZRestored_NNN.log) and <exe dir>\PDZRestored.toml; the user data folder keeps the old name (kUserDataFolder).
    return std::unique_ptr<PerfectdarkzerorecompApp>(new PerfectdarkzerorecompApp(ctx, "PDZRestored", PPCImageConfig));
  }
  // The user data folder (saves, DLC packs, the title update, the shader cache) under the user's Documents, as before
  // the rename: the SDK would otherwise derive it from the app name.
  static constexpr const char *kUserDataFolder = "perfectdarkzerorecomp-tu3";
  // The exe's name before the release name: <exe dir>\perfectdarkzerorecomp.toml (a folder set up for the old exe,
  // e.g. the Steam Deck test folder) and logs\perfectdarkzerorecomp_NNN.log, whose numbering the new logs continue.
  static constexpr const char *kOldName = "perfectdarkzerorecomp";

  void OnPreSetup(rex::RuntimeConfig &config) override
  {
    // Release blocker (Steam Deck test, 30 Oct): a settings file without a gpu_plugin line started with no GPU plugin
    // and the null backend: a black screen. The native renderer needs the xenos plugin (its null backend services the
    // ring), so an empty setting means xenos; an explicit gpu_plugin still wins.
    if (config.gpu_plugin.empty())
    {
      config.gpu_plugin = "xenos";
      REXLOG_INFO("pdz: gpu_plugin not set: using 'xenos' (the GPU plugin the native renderer needs)");
    }
    // Safe mode, and the native renderer's shaders made from the player's game files (2026-11-03,
    // docs/RELEASE_FIRST_RUN.md): when they can't be made, this start uses the emulated renderer (Safe mode), said
    // in a box and the log.
    {
      bool safe = REXCVAR_GET(pdz_safe_mode);
      const bool native_wanted = rex::cvar::GetFlagSource("pdz_native_renderer") == rex::cvar::Source::kDefault
                                     ? !(rex::cvar::GetFlagSource("graphics_backend") == rex::cvar::Source::kCommandLine &&
                                         REXCVAR_GET(graphics_backend) != "null")
                                     : REXCVAR_GET(pdz_native_renderer);
      std::string why;
      if (!safe && native_wanted &&
          !pdz::native::generate::PreparePackages({game_root_, update_root_, user_root_, PdzBuildCommit()}, why))
      {
        safe = true;
        REXLOG_WARN("pdz: the native renderer's shaders couldn't be made ({}): starting with the emulated renderer "
                    "(Safe mode)",
                    why);
        if (!REXCVAR_GET(pdz_setup_quiet))
        {
          pdz::setup_ui::Info(L"PDZ Restored",
                              L"PDZ Restored couldn't prepare its graphics this time, so it's starting in Safe "
                              L"mode. It's slower and runs at 60 fps. It'll try again next time, and the details are "
                              L"in the newest log.");
        }
      }
      if (safe)
      {
        rex::cvar::SetFlagByName("pdz_native_renderer", "false");
        rex::cvar::SetFlagByName("pdz_sp_120", "false");
        REXLOG_INFO("pdz: Safe mode: the emulated renderer, 60 fps ({})",
                    REXCVAR_GET(pdz_safe_mode) ? "pdz_safe_mode" : "automatic fallback");
      }
    }
    std::string backend = REXCVAR_GET(graphics_backend);
    // Native renderer as the default (2026-10-29): with pdz_native_renderer set nowhere, the null backend and
    // the native render profile (mod/render/native/native.h, ApplyNativeDefault).
    pdz::native::ApplyNativeDefault(backend);
    if (backend != "any" && !config.gpu_plugin.empty())
    {
      config.graphics = rex::system::LoadGpuPlugin(config.gpu_plugin, backend);
      if (!config.graphics)
      {
        REXLOG_WARN("graphics_backend '{}' unavailable, falling back to automatic selection",
                    backend);
      }
    }
    // graphics_backend null (stage 2 M1): the plugin's null backend services the ring without rendering; latched here,
    // before the guest runs, for the native renderer's policy (mod/render/gpu_sync.h).
    pdz::gpusync::OnBackendSelected(backend, !config.gpu_plugin.empty(), config.graphics != nullptr);
  }

  // void OnLoadXexImage(std::string& xex_image) override {}
  // tu3-base: no image patches (the launch 16:9 patch isn't ported).
  void OnPostLoadXexImage() override
  {
    // The exe image's own shaders (its containers and two raw arrays), from guest memory, when the packages' were just
    // made (native_generate.h).
    if (runtime() && !REXCVAR_GET(pdz_test_dump_image).empty())
    {
      // Testing tool (2026-11-03, docs/RELEASE_FIRST_RUN.md "pressings"): the loaded, patched image, then exit.
      const uint8_t *img = runtime()->virtual_membase() + PPCImageConfig.image_base;
      std::ofstream(REXCVAR_GET(pdz_test_dump_image), std::ios::binary)
          .write(reinterpret_cast<const char *>(img), std::streamsize(PPCImageConfig.image_size));
      REXLOG_INFO("pdz: image 0x{:08X}+0x{:X} written to {} (pdz_test_dump_image); exiting", PPCImageConfig.image_base,
                  PPCImageConfig.image_size, REXCVAR_GET(pdz_test_dump_image));
      rex::FlushLogging();
      std::exit(0);
    }
    if (runtime())
    {
      // Any supported disc pressing with its own Title Update 3 gives the image the code is recompiled from; anything
      // else stops here with a message (first_run.h).
      pdz::first_run::VerifyImage(runtime()->virtual_membase() + PPCImageConfig.image_base, PPCImageConfig.image_size,
                                  game_root_);
      pdz::native::generate::PrepareExe(runtime()->virtual_membase(), PPCImageConfig.image_base, PPCImageConfig.image_size);
      // The shipped pipeline list, once (native_pipeline.h pipeline_seed): built before play, not at first draws.
      pdz::native::pipeline_seed::PrepareAtFirstStart();
    }
  }

  void OnCreateDialogs(rex::ui::ImGuiDrawer *drawer) override
  {
    // Registered before OnPostSetup reloads the settings files, so the key
    // can be rebound from mapping.toml.
#ifndef PDZ_NO_MODS
    third_person::RegisterBinds();
    live_toggles::Init(drawer);
    local_profiles::Init(drawer);
#else
    // tu3-base: the TU3 build's only binds (live_toggles isn't ported).
    anim_viewer::Init(drawer);
    restorations::RegisterBinds(); // testing tool: pdz_test_lab_blackout_key
    pdz::transwater::Init(drawer); // F1: pdz_fix_transparency_water on / off, and its label
#endif
    // Mouse aim straight into the view angles, menu mode for mnk (src/mod/mouse_look.h).
    pdz::mouse::MouseLook::Get().Attach(window(), drawer);
    pdz::native::RegisterMarkBind(); // test aid: bind_mark_spot (F12), capture + pdz_mark line
    // The release version in the title (the SDK set "PDZRestored <SDK build stamp>").
    if (window())
    {
      window()->SetTitle(std::string("PDZRestored ") + PDZ_RESTORED_VERSION);
    }
  }

  void OnPostSetup() override
  {
    utils::LoadSettingsFiles();

#ifdef REXGLUE_ENABLE_PERF_COUNTERS
    // SDK exposes the perf_log_csv cvar but never wires it up; do it here so
    // --perf_log_csv=<path> actually produces per-frame CSV output.
    // Use GetFlagByName to avoid needing a cross-DLL declaration of the cvar.
    rex::perf::SetCsvLogPath(rex::cvar::GetFlagByName("perf_log_csv"));
#endif

    // Run debug tools only if enabled via hardware.toml (dev_debug_runtime = true)
    if (REXCVAR_GET(dev_debug_runtime))
    {
      debug_tools::PerformMissingFunctionScan();
      debug_tools::PerformStubSweep();
    }
  }

  void OnShutdown() override
  {
    pdz::gpusync::OnAppShutdown(); // pdz_native_record_thread: drain and stop the worker
#ifndef PDZ_NO_MODS
    local_profiles::Shutdown();
#endif
#ifdef REXGLUE_ENABLE_PERF_COUNTERS
    rex::perf::FlushCsv();
#endif
  }

  // The SDK ends the process with std::_Exit after the window closes (rex_app.cpp OnClosing), which skips the exe's
  // atexit handlers; the close button, Alt+F4, a quit request and --pdz_test_exit_s's WM_CLOSE all come through this
  // veto point. pdz_guest_profile's total is logged here; in the PGO training build (PDZ_PGO=gen,
  // cmake/pdz_compiler.cmake) the profile runtime writes the exe's profiles here too.
  bool OnWindowCloseRequested() override
  {
    pdz::gtsample::LogGuestProfileTotal("window closing");
    pdz::ipprof::WriteAtExit("window closing"); // pdz_guest_ip_profile
#ifdef PDZ_PGO_GEN
    pdz::pgo::WriteProfiles();
#endif
    return true;
  }

  void OnConfigurePaths(rex::PathConfig &paths) override
  {
    // The release name (2026-11-03): a folder set up for the old exe keeps working. The SDK loaded
    // <exe dir>\PDZRestored.toml if there is one; without it, the old perfectdarkzerorecomp.toml is loaded here and
    // the path cvars it sets are applied as the SDK would have.
    {
      std::error_code ec;
      const std::filesystem::path exe_dir = rex::filesystem::GetExecutableFolder();
      const std::filesystem::path legacy = exe_dir / (std::string(kOldName) + ".toml");
      if (!std::filesystem::exists(exe_dir / "PDZRestored.toml", ec) && std::filesystem::exists(legacy, ec))
      {
        rex::cvar::LoadConfig(legacy);
        if (!REXCVAR_GET(game_data_root).empty())
        {
          paths.game_data_root = REXCVAR_GET(game_data_root);
        }
        if (!REXCVAR_GET(user_data_root).empty())
        {
          paths.user_data_root = REXCVAR_GET(user_data_root);
        }
        if (!REXCVAR_GET(update_data_root).empty())
        {
          paths.update_data_root = REXCVAR_GET(update_data_root);
        }
        if (!REXCVAR_GET(cache_root).empty())
        {
          paths.cache_root = REXCVAR_GET(cache_root);
        }
        if (!REXCVAR_GET(metadata_root).empty())
        {
          paths.metadata_root = REXCVAR_GET(metadata_root);
        }
        legacy_config_ = legacy;
      }
    }
    // --game_data_root / REX_GAME_DATA_ROOT still win: this cvar is read
    // before any config file loads, so it's already non-empty here if set.
    if (paths.game_data_root.empty())
    {
      paths.game_data_root = utils::RepoRoot() / "assets";
    }

    // tu3-base: by default this build keeps its own user data (saves,
    // settings, shader cache) in <user folder>\perfectdarkzerorecomp-tu3 and
    // mounts the title update's files from its update\ folder, so it never
    // reads or writes the launch build's data (TU3's saved settings are
    // larger). --user_data_root / --update_data_root / --cache_root still win.
    if (REXCVAR_GET(user_data_root).empty())
    {
      // <user folder>\perfectdarkzerorecomp-tu3 whatever the exe is called (the SDK's default is <user folder>\<app
      // name>).
      const std::filesystem::path launch_root = paths.user_data_root;
      paths.user_data_root = launch_root.parent_path() / kUserDataFolder;
      if (REXCVAR_GET(cache_root).empty())
      {
        paths.cache_root = paths.user_data_root / "cache";
      }
    }
    if (paths.update_data_root.empty())
    {
      paths.update_data_root = paths.user_data_root / "update";
    }
    // First-run setup (2026-11-03, docs/RELEASE_FIRST_RUN.md): with no usable game files, the player's disc dump and
    // Title Update 3 are found (game\ next to the exe, else a file picker), checked and prepared, with a progress
    // window; a later start uses what setup prepared. --game_data_root / --update_data_root skip it.
    {
      const pdz::first_run::Paths found = pdz::first_run::Prepare(
          rex::filesystem::GetExecutableFolder(), paths.user_data_root, {paths.game_data_root, paths.update_data_root},
          !REXCVAR_GET(game_data_root).empty() || !REXCVAR_GET(update_data_root).empty());
      paths.game_data_root = found.game_root;
      paths.update_data_root = found.update_root;
    }
    update_root_ = paths.update_data_root;
    game_root_ = paths.game_data_root;
    user_root_ = paths.user_data_root;
    pdz::native::SetCacheRoot(paths.cache_root); // the native renderer's per-user shader cache (native_dxc.h)

    // Hardware/rendering settings become the SDK's primary config file, so
    // the in-game Settings overlay's "Save to config" also lands here.
    paths.config_path = utils::SettingsDir() / "hardware.toml";

    // Input mapping lives in its own file, loaded manually below (the SDK
    // only auto-loads the single path above).
    utils::LoadSettingsFiles();

    // The settings window (src/setup/settings_window.h): after the first setup, every start until "Don't show this
    // again", Shift held, or --pdz_settings_open; never in a developer's build or a quiet test run.
    {
      const pdz::settings_ui::Choice choice = pdz::settings_ui::MaybeShow(
          {utils::SettingsDir() / "hardware.toml", utils::SettingsDir() / "mapping.toml",
           rex::filesystem::GetExecutableFolder() / "logs", paths.user_data_root, paths.cache_root},
          pdz::first_run::SetupRanThisStart());
      if (choice == pdz::settings_ui::Choice::kQuit)
      {
        std::exit(0);
      }
      if (choice == pdz::settings_ui::Choice::kSafeMode)
      {
        rex::cvar::SetFlagByName("pdz_safe_mode", "true");
      }
    }

    // Each local profile keeps its saves in its own folder. The cache root
    // was derived before this point, so the shader cache stays shared.
#ifndef PDZ_NO_MODS
    paths.user_data_root = local_profiles::Load(paths.user_data_root);
#endif
  }

  // Log 299: the SDK flushed the log file after every info line, on the thread that logged it (the game thread's
  // periodic lines, the record thread's, and Rare's XLIVEBASE error about 100 times a second). Flush on warnings and
  // errors, and every second from spdlog's own flusher thread; the crash handler (crash_dump.cpp) and the exit
  // (rex_app.cpp OnClosing) flush explicitly. log_flush_interval in the settings still wins.
  void OnConfigureLogging(rex::LogConfig &config) override
  {
    config.flush_level = spdlog::level::warn;
    if (config.flush_interval.count() <= 0)
    {
      config.flush_interval = std::chrono::seconds(1);
    }
    // The release name (2026-11-03): logs\PDZRestored_NNN.log, numbered on from the highest PDZRestored_NNN or
    // perfectdarkzerorecomp_NNN log there (the SDK counts its own app name only, so it would start again at 001).
    // --log_file still wins (applied after this). With a log-folder budget set, the oldest runs under both names are
    // pruned first, as the SDK does for its own name.
    if (config.log_file.empty() && !config.log_dir.empty())
    {
      std::error_code ec;
      std::filesystem::create_directories(config.log_dir, ec);
      if (config.dir_budget_bytes > 0)
      {
        PruneOldLogs(config.log_dir, config.dir_budget_bytes);
      }
      int last = 0;
      for (const auto &entry : std::filesystem::directory_iterator(config.log_dir, ec))
      {
        last = std::max(last, std::max(LogNumber(entry.path(), config.app_name), LogNumber(entry.path(), kOldName)));
      }
      config.log_file = config.log_dir / fmt::format("{}_{:03d}.log", config.app_name, last + 1);
    }
  }

  // The run number of <name>_NNN.log (0: not one).
  static int LogNumber(const std::filesystem::path &file, const std::string &name)
  {
    const std::string fname = file.filename().string();
    const std::string prefix = name + "_";
    if (fname.size() <= prefix.size() + 4 || fname.compare(0, prefix.size(), prefix) != 0 ||
        fname.compare(fname.size() - 4, 4, ".log") != 0)
    {
      return 0;
    }
    const std::string digits = fname.substr(prefix.size(), fname.size() - prefix.size() - 4);
    if (digits.size() > 9 || digits.find_first_not_of("0123456789") != std::string::npos)
    {
      return 0;
    }
    return std::atoi(digits.c_str());
  }

  // As the SDK's PruneLogDirectory, over both names: the oldest runs go first until the folder is under the budget.
  static void PruneOldLogs(const std::filesystem::path &dir, uint64_t budget)
  {
    std::error_code ec;
    std::map<int, std::vector<std::filesystem::path>> runs;
    uint64_t total = 0;
    for (const auto &entry : std::filesystem::directory_iterator(dir, ec))
    {
      const int n = std::max(LogNumber(entry.path(), "PDZRestored"), LogNumber(entry.path(), kOldName));
      if (n <= 0 || !entry.is_regular_file(ec))
      {
        continue;
      }
      const uint64_t bytes = entry.file_size(ec);
      if (!ec)
      {
        runs[n].push_back(entry.path());
        total += bytes;
      }
    }
    for (const auto &[n, files] : runs)
    {
      if (total <= budget)
      {
        break;
      }
      for (const auto &file : files)
      {
        const uint64_t bytes = std::filesystem::file_size(file, ec);
        if (!ec && std::filesystem::remove(file, ec))
        {
          total -= bytes;
        }
      }
    }
  }

  // Override virtual hooks for customization:
  // Startup line (2026-10-28: one build): which build this is and which
  // settings file it read.
  void OnPostInitLogging() override
  {
    const std::filesystem::path settings = utils::SettingsDir() / "hardware.toml";
    REXLOG_INFO("pdz_build: PDZ Restored {}, built {} from commit {}; settings file {} ({})", PDZ_RESTORED_VERSION,
                PdzBuildTime(), PdzBuildCommit(), settings.string(),
                std::filesystem::exists(settings) ? "read" : "MISSING: defaults");
    pdz::first_run::LogSummary();
    pdz::settings_ui::LogSummary();
    if (!legacy_config_.empty())
    {
      REXLOG_INFO("pdz: no PDZRestored.toml beside the exe: read the old name's {} instead (rename it to "
                  "PDZRestored.toml)",
                  legacy_config_.string());
    }
    // Release blocker (Steam Deck test, 30 Oct): without the Title Update 3 data package (UPDATE:) TU3's code reads a
    // null asset at boot (sub_8227D0A0, after the DLC loc-assets file aid_xml_DLC_locassets fails to open) and crashes.
    // Its package file is checked here, before the game starts: missing = a clear message and a clean exit.
    const std::filesystem::path package = update_root_ / "Pdz" / "Assets" / "PackageData" / "1726605008.pkg";
    std::error_code ec;
    if (!update_root_.empty() && !std::filesystem::exists(package, ec))
    {
      REXLOG_ERROR("pdz: Title Update 3 data package not found ({} is missing; update data root {}): see README, "
                   "'What you need'. Exiting.",
                   package.string(), update_root_.string());
      rex::ShowSimpleMessageBox(rex::SimpleMessageBoxType::Error,
                                "Title Update 3 data package not found: see README, 'What you need'.\n\nExpected: " +
                                    package.string());
      std::exit(2);
    }
  }
  std::filesystem::path update_root_; // the title update's data folder (OnConfigurePaths), checked at startup
  std::filesystem::path game_root_, user_root_; // game data / user data roots (the shader generator's inputs)
  std::filesystem::path legacy_config_; // <exe dir>\perfectdarkzerorecomp.toml when it was read (the old exe name)
  // void OnPreSetup(rex::RuntimeConfig& config) override {}
  // void OnLoadXexImage(std::string& xex_image) override {}
  // void OnPostLoadXexImage() override {}
  // void OnPostSetup() override {}
  // void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {}
  // std::unique_ptr<rex::ui::ImGuiDialog> CreateAchievementsOverlay() override;
  // std::unique_ptr<rex::ui::ImGuiDialog> CreateAchievementNotificationDialog() override;
  // void OnShutdown() override {}
  // void OnConfigurePaths(rex::PathConfig& paths) override {}
};
