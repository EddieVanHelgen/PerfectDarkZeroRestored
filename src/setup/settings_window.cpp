// PDZ Restored: the settings launch window (see settings_window.h).
#include "settings_window.h"
#include "../pdz_version.h"

#include "map_packs.h"
#include "setup_window.h"
#include "theme.h"
#include "update_check.h"
#include "../mod/render/native/native_dxc.h" // DeveloperBuild
#include "../mod/minigame_input.h"            // the Locktopus: the shared pick step and Rare's pin rules

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commctrl.h>
#include <windowsx.h>
#include <shellapi.h>
#include <uxtheme.h>

#include <algorithm>
#include <cstdlib>
#include <cmath>
#include <fstream>
#include <functional>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <fmt/xchar.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/keybinds.h>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "uxtheme.lib")

REXCVAR_DEFINE_BOOL(pdz_settings_show, true, "Setup",
                    "The settings window before every start (PDZ Restored Settings); its \"Don't show this again\" "
                    "sets it false. Shift held at launch, or --pdz_settings_open, shows it anyway");
REXCVAR_DEFINE_BOOL(pdz_settings_open, false, "Setup",
                    "Show the settings window at this start (the \"PDZ Restored Settings\" launcher passes it)");
REXCVAR_DEFINE_STRING(pdz_map_packs_install, "", "Setup",
                      "Map pack package files to install at this start, separated by ';' (the same checks and repairs "
                      "as the settings window's Add; for scripts and tests). One line per file in the log");
REXCVAR_DEFINE_STRING(pdz_settings_test_capture, "", "Testing",
                      "TESTING TOOL: the settings window shows each tab in turn, writes it as tab<N>.bmp into this "
                      "folder and closes itself (the close box: the game doesn't start)");
REXCVAR_DEFINE_STRING(pdz_settings_test_rebind, "", "Testing",
                      "TESTING TOOL: the settings window rebinds on the Controls tab through its own capture path and "
                      "then presses Play. ';'-separated '<keybind cvar>[:2]=<input>', input one of LButton, RButton, "
                      "MButton, XButton1, XButton2, WheelUp, WheelDown, WheelLeft, WheelRight (posted as the Windows "
                      "messages), Key:<vk hex>:<scan hex>[:ext] (a WM_KEYDOWN), Raw:<button 6-32> (a Raw Input button "
                      "state), Esc. A clash is answered with pdz_settings_test_answer. Every step is logged");
REXCVAR_DEFINE_INT32(pdz_settings_test_dpi, 0, "Testing",
                     "TESTING TOOL: lay the settings window, the setup windows and the message boxes out for this "
                     "DPI (96 = 100 %, 144 = 150 %) instead of the screen's; 0 = the screen's");
REXCVAR_DEFINE_STRING(pdz_settings_test_answer, "swap", "Testing",
                      "TESTING TOOL: the answer pdz_settings_test_rebind gives when an input is already used: swap, "
                      "both or cancel");
REXCVAR_DEFINE_STRING(pdz_settings_test_dial, "", "Testing",
                      "TESTING TOOL: the settings window plays the Controls tab's practice Locktopus dial by itself, "
                      "';'-separated runs of keys, mouse or ignore (the inputs the Mouse / Keys choice leaves out must "
                      "not move the pick), each on a new lock from a fixed seed, through the dial's own step (the "
                      "game's steering code and Rare's pin rules) with simulated keys or mouse counts. Every step that "
                      "matters is logged; with pdz_settings_test_capture each run's end is captured as dial_<n>.bmp")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DECLARE(bool, pdz_setup_quiet);
REXCVAR_DECLARE(bool, pdz_fix_minigame_keyboard);
REXCVAR_DECLARE(std::string, pdz_minigame_inputs);
REXCVAR_DECLARE(double, pdz_minigame_mouse_counts);
REXCVAR_DECLARE(bool, pdz_update_check);
REXCVAR_DECLARE(bool, pdz_setup_restarted); // first_run.cpp: setup's own restart after adding TU3

namespace pdz::settings_ui
{
  namespace fs = std::filesystem;

  namespace
  {
    // ---- the settings files: only the lines the window owns ----

    std::vector<std::string> ReadLines(const fs::path &p)
    {
      std::vector<std::string> lines;
      std::ifstream f(p, std::ios::binary);
      std::string line;
      while (std::getline(f, line))
      {
        if (!line.empty() && line.back() == '\r')
        {
          line.pop_back();
        }
        lines.push_back(line);
      }
      return lines;
    }

    bool LineIsKey(const std::string &line, const std::string &key)
    {
      size_t i = line.find_first_not_of(" \t");
      if (i == std::string::npos || line.compare(i, key.size(), key) != 0)
      {
        return false;
      }
      i += key.size();
      while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
      {
        ++i;
      }
      return i < line.size() && line[i] == '=';
    }

    // `key = value` in place of `line`, keeping the line's trailing comment (at its column where it fits).
    std::string ReplaceValue(const std::string &line, const std::string &key, const std::string &value)
    {
      std::string out = key + " = " + value;
      const size_t eq = line.find('=');
      size_t v = eq == std::string::npos ? std::string::npos : line.find_first_not_of(" \t", eq + 1);
      size_t end = line.size();
      if (v != std::string::npos)
      {
        if (line[v] == '"')
        {
          end = v + 1;
          while (end < line.size() && line[end] != '"')
          {
            end += line[end] == '\\' ? 2 : 1;
          }
          end = std::min(end + 1, line.size());
        }
        else
        {
          end = line.find_first_of(" \t#", v);
          end = end == std::string::npos ? line.size() : end;
        }
      }
      const size_t hash = line.find('#', end);
      if (hash != std::string::npos)
      {
        out += std::string(out.size() + 1 < hash ? hash - out.size() : 1, ' ') + line.substr(hash);
      }
      return out;
    }

    // Sets `key = value` (value already TOML: "\"x\"", "true", "2.0") in place where the key is (its comment kept),
    // else after the window's section marker (or at the end); an empty value removes the line. Everything else stays
    // as it is.
    void WriteKeys(const fs::path &p, const std::vector<std::pair<std::string, std::string>> &keys)
    {
      std::vector<std::string> lines = ReadLines(p);
      const std::string marker = "# --- Settings window (PDZ Restored Settings) ---";
      for (const auto &[key, value] : keys)
      {
        bool done = false;
        for (size_t i = 0; i < lines.size(); ++i)
        {
          if (LineIsKey(lines[i], key))
          {
            if (value.empty())
            {
              lines.erase(lines.begin() + ptrdiff_t(i));
            }
            else
            {
              lines[i] = ReplaceValue(lines[i], key, value);
            }
            done = true;
            break;
          }
        }
        if (done || value.empty())
        {
          continue;
        }
        auto at = std::find(lines.begin(), lines.end(), marker);
        if (at == lines.end())
        {
          lines.push_back("");
          lines.push_back(marker);
          at = lines.end() - 1;
        }
        // After the marker and its comment lines.
        auto insert = at + 1;
        while (insert != lines.end() && !insert->empty() && (*insert)[0] == '#')
        {
          ++insert;
        }
        while (insert != lines.end() && !insert->empty())
        {
          ++insert;
        }
        lines.insert(insert, key + " = " + value);
      }
      std::ofstream f(p, std::ios::binary | std::ios::trunc);
      for (const std::string &line : lines)
      {
        f << line << "\r\n";
      }
    }

    // The value of `key` in the settings file (the line WriteKeys would replace; quotes and a trailing comment
    // dropped), or "" if the file has no such line.
    std::string FileValue(const fs::path &p, const std::string &key)
    {
      for (const std::string &line : ReadLines(p))
      {
        if (!LineIsKey(line, key))
        {
          continue;
        }
        const size_t v = line.find_first_not_of(" \t", line.find('=') + 1);
        if (v == std::string::npos)
        {
          return {};
        }
        if (line[v] == '"')
        {
          const size_t end = line.find('"', v + 1);
          return line.substr(v + 1, end == std::string::npos ? std::string::npos : end - v - 1);
        }
        const size_t end = line.find_first_of(" \t#", v);
        return line.substr(v, end == std::string::npos ? std::string::npos : end - v);
      }
      return {};
    }

    // A setting's saved value as the window shows it, or "" when it isn't set (the compiled default applies).
    // - Registered now (the exe's and the core's settings): its value, "" while it is still the compiled default.
    // - Not registered yet (the GPU plugin's, e.g. vsync: the window opens before rexgpu-xenos.dll loads, so the
    //   registry has no value for it; the SDK applies the file's value when the plugin registers it): the file's.
    std::string SavedValue(const fs::path &settings_file, const std::string &key)
    {
      if (rex::cvar::GetFlagInfo(key))
      {
        return rex::cvar::GetFlagSource(key) == rex::cvar::Source::kDefault ? std::string()
                                                                            : rex::cvar::GetFlagByName(key);
      }
      return FileValue(settings_file, key);
    }

    // Render scale's choices (100% = no line: the native renderer's own pick for the screen) as pdz_native_scale
    // for a screen of this height. 75% and 50% are never the same value where a smaller one exists (at 2160p both
    // used to give 2), so a saved choice reads back as itself.
    int ScaleForChoice(int choice, int screen_height)
    {
      const int base = std::clamp(int((screen_height + 360) / 720), 1, 3);
      const int v75 = std::max(1, int(std::lround(base * 0.75)));
      const int v50 = std::max(1, int(std::lround(base * 0.5)));
      return choice == 1 ? v75 : choice == 2 ? (v50 == v75 && v75 > 1 ? v75 - 1 : v50) : base;
    }

    std::wstring W(const std::string &s) { return std::wstring(s.begin(), s.end()); }

    // ---- the PC ----

    struct Monitor
    {
      std::wstring device, label;
      DWORD width = 0, height = 0, hz = 0;
      bool primary = false;
    };

    std::vector<Monitor> Monitors()
    {
      std::vector<Monitor> out;
      DISPLAY_DEVICEW dd{};
      dd.cb = sizeof(dd);
      for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dd, 0); ++i, dd = DISPLAY_DEVICEW{}, dd.cb = sizeof(dd))
      {
        if (!(dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP))
        {
          continue;
        }
        DEVMODEW m{};
        m.dmSize = sizeof(m);
        if (!EnumDisplaySettingsW(dd.DeviceName, ENUM_CURRENT_SETTINGS, &m))
        {
          continue;
        }
        Monitor mon;
        mon.device = dd.DeviceName;
        mon.width = m.dmPelsWidth;
        mon.height = m.dmPelsHeight;
        mon.hz = m.dmDisplayFrequency;
        mon.primary = (dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) != 0;
        mon.label = fmt::format(L"Display {}{} - {} x {}, {} Hz", out.size() + 1, mon.primary ? L" (main)" : L"",
                                mon.width, mon.height, mon.hz);
        out.push_back(mon);
      }
      return out;
    }

    std::vector<std::pair<DWORD, DWORD>> Modes(const std::wstring &device)
    {
      std::set<std::pair<DWORD, DWORD>> modes;
      DEVMODEW m{};
      m.dmSize = sizeof(m);
      for (DWORD i = 0; EnumDisplaySettingsW(device.c_str(), i, &m); ++i)
      {
        if (m.dmPelsWidth >= 1280 && m.dmPelsHeight >= 720)
        {
          modes.insert({m.dmPelsWidth, m.dmPelsHeight});
        }
      }
      std::vector<std::pair<DWORD, DWORD>> v(modes.rbegin(), modes.rend());
      return v;
    }

    // Field of view: pdz_fov_scale multiplies Rare's 58 degree 4:3 horizontal FOV (fov_tuning.cpp); across a 16:9
    // screen that is 2 atan(4/3 tan(29 deg x scale)): 72.9 degrees at 1.0 (Rare's), 103.4 at the cvar's 1.5.
    constexpr int kFovMinDegrees = 73;  // Rare's (scale 1.0, 72.9 across 16:9)
    constexpr int kFovMaxDegrees = 103; // scale 1.494
    constexpr double kPi = 3.14159265358979323846;

    double FovDegreesOfScale(double scale)
    {
      return 2.0 * std::atan(4.0 / 3.0 * std::tan(29.0 * scale * kPi / 180.0)) * 180.0 / kPi;
    }

    double FovScaleOfDegrees(double degrees)
    {
      return 2.0 * std::atan(0.75 * std::tan(degrees * 0.5 * kPi / 180.0)) * 180.0 / kPi / 58.0;
    }

    // The slider's position: whole degrees, Rare's at the left end.
    int FovPositionOfScale(double scale)
    {
      return scale <= 1.0005 ? kFovMinDegrees
                             : std::clamp(int(std::lround(FovDegreesOfScale(scale))), kFovMinDegrees, kFovMaxDegrees);
    }

    double FovScaleOfPosition(int position)
    {
      return position <= kFovMinDegrees ? 1.0 : FovScaleOfDegrees(double(std::min(position, kFovMaxDegrees)));
    }

    std::wstring FovText(int position)
    {
      return position <= kFovMinDegrees ? fmt::format(L"{}\u00B0 (Rare's)", kFovMinDegrees)
                                        : fmt::format(L"{}\u00B0", position);
    }

    std::wstring AutoNote(DWORD hz)
    {
      if (hz >= 239)
      {
        return fmt::format(L"Your screen runs at {} Hz, so the campaign plays at 120 fps, on every other refresh.", hz);
      }
      if (hz > 122)
      {
        // No display switch since VSync Auto went (2026-11-06): On plays at half the refresh.
        return fmt::format(L"Your screen runs at {} Hz. With VSync On the campaign plays at {:.0f} fps. Choose VRR on "
                           L"the Display tab if G-Sync or FreeSync is on in your graphics driver, or set your screen "
                           L"to 120 Hz in Windows for 120 fps.",
                           hz, hz / 2.0);
      }
      if (hz >= 119)
      {
        return fmt::format(L"Your screen runs at {} Hz, so the campaign plays at 120 fps.", hz);
      }
      return fmt::format(L"Your screen runs at {} Hz, so the campaign plays at 60 fps.", hz);
    }

    // ---- the window ----

    // The VSync / VRR choices' help lines: On (pdz_present_pacing fixed), VRR (vrr), Off (vsync off).
    constexpr const wchar_t *kVsyncHelp[3] = {
        L"Locked to your screen's refresh: smooth at 60 and 120 Hz. On a 144 / 165 Hz screen without VRR this runs at "
        L"72 fps; use VRR there if your screen and driver have it.",
        L"Only if G-Sync or FreeSync is switched on in your graphics driver for this display. Otherwise choose On.",
        L"Off can cause tearing and judder. Only turn it off for testing.",
    };

    enum Tab
    {
      kTabDisplay,
      kTabFrameRate,
      kTabPacks,
      kTabControls,
      kTabAdvanced,
      kTabCount,
    };

    enum Id
    {
      kTabs = 100,
      kTabButton0 = 180, // ..184: the side bar's tabs (clear of every other id)
      kMonitor = 101,
      kMode,
      kResolution,
      kScale,
      kVsync,
      kFov,
      kFovValue,
      kFovDefault,
      kWindowedNote,
      kCampaign,
      kCampaignNote,
      kMultiplayer,
      kMultiplayerNote,
      kSplitCap,     // Frame rate: Split screen Auto / 60 / 40 / 30 (pdz_splitscreen_cap)
      kSplitCapNote,
      kPackName0 = 200,
      kPackMaps0 = 210,
      kPackStatus0 = 220,
      kPackButton0 = 230,
      kPackAddFiles = 240,
      kPackCheck,
      kSensitivity = 300,
      kSensitivityLabel,
      kInvert,
      kZoomSteps,
      kBindList,
      kBindChange1,
      kBindChange2,
      kBindClear,
      kBindReset,
      kBindWarning, // Controls: the Start-has-no-menu-key warning line (empty when there is nothing to say)
      kLockHeading, // Controls: the Locktopus group
      kLockInputsLabel,
      kLockInputs,  // Mouse / Keys / Both (pdz_minigame_inputs)
      kLockSensLabel,
      kLockSens,    // the mouse sensitivity trackbar (pdz_minigame_mouse_counts)
      kLockLow,
      kLockHigh,
      kLockKeys,    // the keys, as text
      kLockDial,    // the practice dial (owner-drawn)
      kLockNewLock,
      kLockDifficulty, // the practice dial's Agent / Secret Agent / Perfect Agent (Rare's radius)
      kOpenSettings = 400,
      kOpenLogs,
      kRebuild,
      kReset,
      kNetwork,
      kUpdateCheck, // Advanced: "Check for updates" (pdz_update_check)
      kUpdateOpen,  // the side bar: "Open download page", shown when a newer release is out
      kHelp = 500,
      kDontShow,
      kSafe,
      kPlay,
    };

    struct Ui
    {
      Context ctx;
      HWND window = nullptr, tabs = nullptr, help = nullptr;
      HFONT font = nullptr, bold = nullptr;
      int dpi = 96;
      int tab = kTabDisplay;
      std::vector<HWND> page[kTabCount];
      std::map<HWND, std::wstring> help_of;
      std::wstring tab_help[kTabCount];
      std::vector<Monitor> monitors;
      std::vector<std::pair<DWORD, DWORD>> modes;
      Choice choice = Choice::kQuit;
      bool done = false;
      bool save_on_close = true; // the close box saves like Play (0.9.2); the capture test closes without saving
      // Controls: each row's bind tokens (keybind value split at ','), and the rebind in progress.
      HWND list = nullptr;
      std::vector<std::vector<std::string>> binds;
      int cap_row = -1, cap_slot = 0;
      bool cap_by_click = false; // started by a click on the row (its double-click's second click is ignored)
      DWORD cap_started = 0;
      unsigned long raw_prev = 0;
      bool raw_registered = false;
      bool testing = false; // pdz_settings_test_rebind: no message boxes, answers from pdz_settings_test_answer
      // The look (theme.h): fonts, segmented choices (group id -> its buttons and choice), switches, button styles.
      theme::Fonts tf;
      struct Segmented
      {
        std::vector<HWND> parts;
        std::vector<std::wstring> labels;
        int sel = 0;
      };
      std::map<int, Segmented> segs;
      std::map<int, int> seg_group; // a segment button's id -> its group's id
      std::map<int, bool> toggles;  // a switch's id -> on
      struct Drop
      {
        std::vector<std::wstring> items;
        int sel = 0;
      };
      std::map<int, Drop> drops; // our drop-down lists (Monitor, Resolution), by id
      HWND list_scroll = nullptr; // the key list's scroll bar (ours; the list's own is clipped off)
      int list_scroll_top = -1;   // the top row it last showed
      bool list_placed = false;   // PlaceKeyList has run once
      HWND tab_buttons[kTabCount] = {};
      // The Controls tab's Locktopus practice dial: the game's pick (minigame_input.h's shared step) on a lock of 2-3
      // pins with Rare's rules (the Secret Agent radius, a 1 s hold).
      struct Dial
      {
        HWND wnd = nullptr;
        pdz::minigame::Pick pick;
        struct Pin
        {
          double x = 0.0, y = 0.0; // stick position, x right, y up
          bool set = false;
        };
        std::vector<Pin> pins;
        int cur = 0;
        double hold = pdz::minigame::kPinHoldSeconds;
        double radius = pdz::minigame::kPinRadiusSecretAgent;
        bool done = false;
        bool captured = false;     // the mouse is held by the dial (a click; click or Esc lets go)
        bool hover = false;        // the pointer is over it
        double mdx = 0.0, mdy = 0.0; // raw mouse counts since the last step
        LARGE_INTEGER last{};
        std::mt19937 rng{12345u};
        int locks = 0;
        bool test = false;         // pdz_settings_test_dial: the inputs below instead of the keyboard and mouse
        double tkx = 0.0, tky = 0.0, tdx = 0.0, tdy = 0.0;
        bool tslow = false;
        double elapsed = 0.0;      // seconds in this lock
        int lock = -1;             // kRareLocks index of the lock on the dial
        int force_lock = -1;       // pdz_settings_test_dial: this lock for the next New lock
      } dial;
      // The update check (update_check.h): the newer release, once known, shown in the side bar.
      std::string update_version, update_page;
      int px(int v) const { return MulDiv(v, dpi, 96); }
    };
    // The layout (logical pixels at 96 dpi): side bar, content, help strip, footer. Page controls end above the help
    // strip: their y (from kContentDy) + height <= kWindowH - kFooterH - kHelpH - kContentDy = 460.
    constexpr int kWindowW = 880, kWindowH = 640, kSideW = 200, kFooterH = 72, kHelpH = 68;
    // Page controls are laid out from (28, 48) as before; they sit right of the side bar and under the heading.
    constexpr int kContentDx = kSideW + 4, kContentDy = 40;
    constexpr COLORREF kPanelAlt = RGB(0x12, 0x22, 0x4F); // every other row of the key list
    Ui *g_ui = nullptr;
    std::string g_summary; // logged once logging is up (LogSummary): the window runs before it

    HWND Add(Ui &ui, int tab, const wchar_t *cls, const std::wstring &text, DWORD style, int x, int y, int w, int h,
             int id, const std::wstring &help = L"")
    {
      if (tab >= 0)
      {
        x += kContentDx;
        y += kContentDy;
      }
      HWND c = CreateWindowExW(0, cls, text.c_str(), WS_CHILD | style, ui.px(x), ui.px(y), ui.px(w), ui.px(h), ui.window,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
      SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(ui.font), TRUE);
      if (tab >= 0)
      {
        ui.page[tab].push_back(c);
      }
      else
      {
        ShowWindow(c, SW_SHOW);
      }
      if (!help.empty())
      {
        ui.help_of[c] = help;
      }
      return c;
    }

    // The help strip's text (owner-drawn: up to two lines, centred beside the info icon).
    void SetHelp(Ui &ui, const std::wstring &text)
    {
      wchar_t shown[1024] = {};
      GetWindowTextW(ui.help, shown, 1024);
      if (text != shown)
      {
        SetWindowTextW(ui.help, text.c_str());
        InvalidateRect(ui.help, nullptr, FALSE);
      }
    }

    // The Locktopus group's helpers (defined with the dial, below).
    void RefreshLockText(Ui &ui);
    void DialRelease(Ui &ui);
    double LockCountsOfPos(int pos);
    int LockPosOfCounts(double counts);
    pdz::minigame::PickInputs LockInputsOfSel(int sel);

    HWND Label(Ui &ui, int tab, const std::wstring &text, int x, int y, int w, bool bold = false)
    {
      HWND c = Add(ui, tab, L"STATIC", text, SS_LEFT, x, y, w, 22, 0);
      if (bold)
      {
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(ui.bold), TRUE);
      }
      return c;
    }

    // A drop-down list drawn by us: a button showing the choice and a cyan chevron; clicking it opens OpenDrop's list.
    HWND Combo(Ui &ui, int tab, int x, int y, int w, int id, const std::vector<std::wstring> &items, int sel,
               const std::wstring &help)
    {
      HWND c = Add(ui, tab, L"BUTTON", L"", BS_OWNERDRAW | WS_TABSTOP, x, y - 4, w, 34, id, help);
      theme::EnableHover(c);
      Ui::Drop &d = ui.drops[id];
      d.items = items;
      d.sel = items.empty() ? 0 : std::clamp(sel, 0, int(items.size()) - 1);
      return c;
    }

    // ---- the open list of a drop-down ----
    struct DropList
    {
      Ui *ui = nullptr;
      const Ui::Drop *drop = nullptr;
      int hover = 0, top = 0, rows = 1, row_h = 30, result = -2; // result: -2 open, -1 cancelled, else the choice
    };

    void DropListScrollTo(DropList &dl, int index)
    {
      const int n = int(dl.drop->items.size());
      if (index < dl.top)
      {
        dl.top = index;
      }
      if (index >= dl.top + dl.rows)
      {
        dl.top = index - dl.rows + 1;
      }
      dl.top = std::clamp(dl.top, 0, std::max(0, n - dl.rows));
    }

    LRESULT CALLBACK DropListProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
      DropList *dl = reinterpret_cast<DropList *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
      if (!dl)
      {
        return DefWindowProcW(hwnd, msg, wp, lp);
      }
      const int n = int(dl->drop->items.size());
      switch (msg)
      {
      case WM_ERASEBKGND:
        return 1;
      case WM_PAINT:
      {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        theme::FillRect(dc, rc, theme::kPanel);
        for (int i = 0; i < dl->rows && dl->top + i < n; ++i)
        {
          const int item = dl->top + i;
          RECT r{1, 1 + i * dl->row_h, rc.right - 1, 1 + (i + 1) * dl->row_h};
          const bool on = item == dl->hover;
          if (on)
          {
            theme::FillRect(dc, r, theme::kBlue);
          }
          RECT t = r;
          t.left += dl->ui->px(10);
          theme::Text(dc, dl->drop->items[size_t(item)], t, item == dl->drop->sel ? dl->ui->tf.body_bold : dl->ui->tf.body,
                      on ? RGB(255, 255, 255) : theme::kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
        HPEN pen = CreatePen(PS_SOLID, 1, theme::kBorder);
        HGDIOBJ old_pen = SelectObject(dc, pen);
        HGDIOBJ old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
        Rectangle(dc, 0, 0, rc.right, rc.bottom);
        SelectObject(dc, old_brush);
        SelectObject(dc, old_pen);
        DeleteObject(pen);
        EndPaint(hwnd, &ps);
        return 0;
      }
      case WM_MOUSEMOVE:
      {
        const int y = GET_Y_LPARAM(lp);
        RECT rc;
        GetClientRect(hwnd, &rc);
        if (y >= 0 && y < rc.bottom && GET_X_LPARAM(lp) >= 0 && GET_X_LPARAM(lp) < rc.right)
        {
          const int item = std::clamp(dl->top + (y - 1) / dl->row_h, 0, n - 1);
          if (item != dl->hover)
          {
            dl->hover = item;
            InvalidateRect(hwnd, nullptr, FALSE);
          }
        }
        return 0;
      }
      case WM_LBUTTONDOWN:
      {
        RECT rc;
        GetClientRect(hwnd, &rc);
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        dl->result = PtInRect(&rc, pt) ? std::clamp<int>(dl->top + int(pt.y - 1) / dl->row_h, 0, n - 1) : -1;
        return 0;
      }
      case WM_MOUSEWHEEL:
        dl->top = std::clamp(dl->top - GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA, 0, std::max(0, n - dl->rows));
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      case WM_CAPTURECHANGED:
        if (dl->result == -2 && reinterpret_cast<HWND>(lp) != hwnd)
        {
          dl->result = -1;
        }
        return 0;
      default:
        return DefWindowProcW(hwnd, msg, wp, lp);
      }
    }

    // Opens the list under the drop-down `id` and waits for a choice. Returns the item or -1 (Esc, a click outside).
    int OpenDrop(Ui &ui, int id)
    {
      auto it = ui.drops.find(id);
      HWND button = GetDlgItem(ui.window, id);
      if (it == ui.drops.end() || it->second.items.empty() || !button)
      {
        return -1;
      }
      static bool registered = false;
      if (!registered)
      {
        WNDCLASSW wc{};
        wc.lpfnWndProc = DropListProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        wc.lpszClassName = L"PDZRestoredList";
        wc.style = CS_DROPSHADOW;
        registered = RegisterClassW(&wc) != 0;
      }
      DropList dl;
      dl.ui = &ui;
      dl.drop = &it->second;
      dl.row_h = ui.px(30);
      dl.rows = std::min<int>(int(it->second.items.size()), 10);
      dl.hover = it->second.sel;
      DropListScrollTo(dl, dl.hover);
      RECT br;
      GetWindowRect(button, &br);
      HWND list = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"PDZRestoredList", L"", WS_POPUP, br.left,
                                  br.bottom + ui.px(2), br.right - br.left, dl.rows * dl.row_h + 2, ui.window, nullptr,
                                  GetModuleHandleW(nullptr), nullptr);
      if (!list)
      {
        return -1;
      }
      SetWindowLongPtrW(list, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&dl));
      ShowWindow(list, SW_SHOWNA);
      SetCapture(list);
      SetTimer(list, 1, 100, nullptr);
      const int n = int(it->second.items.size());
      MSG msg;
      while (dl.result == -2 && GetMessageW(&msg, nullptr, 0, 0) > 0)
      {
        if (msg.message == WM_TIMER && msg.hwnd == list)
        {
          // A controller: up / down move, A chooses, B closes.
          const UINT vk = theme::PollController(ui.window);
          if (vk == VK_UP || vk == VK_DOWN)
          {
            SetFocus(button); // PollController moved the focus; it belongs to the drop-down while the list is open
            msg.message = WM_KEYDOWN;
            msg.wParam = vk;
          }
          else if (vk == VK_RETURN || vk == VK_ESCAPE)
          {
            dl.result = vk == VK_RETURN ? dl.hover : -1;
            break;
          }
          else
          {
            continue;
          }
        }
        if (msg.message == WM_KEYDOWN || msg.message == WM_SYSKEYDOWN)
        {
          switch (msg.wParam)
          {
          case VK_UP:
          case VK_DOWN:
          case VK_PRIOR:
          case VK_NEXT:
          case VK_HOME:
          case VK_END:
          {
            const int step = msg.wParam == VK_UP ? -1 : msg.wParam == VK_DOWN ? 1 : msg.wParam == VK_PRIOR ? -dl.rows
                             : msg.wParam == VK_NEXT                          ? dl.rows
                             : msg.wParam == VK_HOME                          ? -n
                                                                              : n;
            dl.hover = std::clamp(dl.hover + step, 0, n - 1);
            DropListScrollTo(dl, dl.hover);
            InvalidateRect(list, nullptr, FALSE);
            break;
          }
          case VK_RETURN:
          case VK_SPACE:
            dl.result = dl.hover;
            break;
          case VK_ESCAPE:
          case VK_TAB:
          case VK_F4:
            dl.result = -1;
            break;
          default:
            break;
          }
          continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
      }
      if (GetCapture() == list)
      {
        ReleaseCapture();
      }
      KillTimer(list, 1);
      SetWindowLongPtrW(list, GWLP_USERDATA, 0);
      DestroyWindow(list);
      SetFocus(button);
      return dl.result >= 0 ? dl.result : -1;
    }

    // Every button is drawn by us (WM_DRAWITEM); a check box becomes a switch (Ui::toggles).
    HWND Button(Ui &ui, int tab, const std::wstring &text, int x, int y, int w, int id, const std::wstring &help,
                DWORD extra = BS_PUSHBUTTON)
    {
      const bool toggle = (extra & 0xF) == BS_AUTOCHECKBOX;
      HWND b = Add(ui, tab, L"BUTTON", text, BS_OWNERDRAW | WS_TABSTOP, x, y - 2, w, 32, id, help);
      theme::EnableHover(b);
      if (toggle)
      {
        ui.toggles[id] = false;
      }
      return b;
    }

    // A choice of a few options as side-by-side buttons (replaces a drop-down list; same id for Sel / SetSel).
    void Segments(Ui &ui, int tab, int x, int y, int w_each, int id, const std::vector<std::wstring> &items, int sel,
                  const std::wstring &help, bool disabled = false, const std::vector<int> &widths = {})
    {
      Ui::Segmented &g = ui.segs[id];
      g.labels = items;
      g.sel = std::clamp(sel, 0, int(items.size()) - 1);
      int left = x;
      for (size_t i = 0; i < items.size(); ++i)
      {
        const int part = 2000 + id * 8 + int(i);
        const int w = i < widths.size() ? widths[i] : w_each; // widths: one per segment (a long label), else w_each
        HWND b = Add(ui, tab, L"BUTTON", items[i], BS_OWNERDRAW | WS_TABSTOP, left, y - 4, w + 1, 34, part, help);
        left += w;
        theme::EnableHover(b);
        EnableWindow(b, !disabled);
        g.parts.push_back(b);
        ui.seg_group[part] = id;
      }
    }

    int Sel(Ui &ui, int id)
    {
      if (auto d = ui.drops.find(id); d != ui.drops.end())
      {
        return d->second.sel;
      }
      auto it = ui.segs.find(id);
      return it != ui.segs.end() ? it->second.sel : int(SendMessageW(GetDlgItem(ui.window, id), CB_GETCURSEL, 0, 0));
    }

    void SetSel(Ui &ui, int id, int sel)
    {
      if (auto d = ui.drops.find(id); d != ui.drops.end())
      {
        d->second.sel = std::clamp(sel, 0, std::max(0, int(d->second.items.size()) - 1));
        InvalidateRect(GetDlgItem(ui.window, id), nullptr, FALSE);
        return;
      }
      auto it = ui.segs.find(id);
      if (it == ui.segs.end())
      {
        SendMessageW(GetDlgItem(ui.window, id), CB_SETCURSEL, WPARAM(sel), 0);
        return;
      }
      it->second.sel = sel;
      for (HWND b : it->second.parts)
      {
        InvalidateRect(b, nullptr, FALSE);
      }
    }

    bool Checked(Ui &ui, int id)
    {
      auto it = ui.toggles.find(id);
      return it != ui.toggles.end() ? it->second
                                    : SendMessageW(GetDlgItem(ui.window, id), BM_GETCHECK, 0, 0) == BST_CHECKED;
    }

    void SetChecked(Ui &ui, int id, bool on)
    {
      ui.toggles[id] = on;
      InvalidateRect(GetDlgItem(ui.window, id), nullptr, FALSE);
    }

    // Our message box with a Windows MessageBox's flags.
    int Box(HWND owner, const std::wstring &text, const std::wstring &caption, UINT flags)
    {
      return theme::ShowMessage(owner, text, caption, flags & MB_ICONMASK, flags & MB_TYPEMASK);
    }

    void ShowTab(Ui &ui, int tab)
    {
      DialRelease(ui);
      ui.tab = tab;
      for (HWND b : ui.tab_buttons)
      {
        InvalidateRect(b, nullptr, FALSE);
      }
      InvalidateRect(ui.window, nullptr, FALSE);
      for (int t = 0; t < kTabCount; ++t)
      {
        for (HWND c : ui.page[t])
        {
          ShowWindow(c, t == tab ? SW_SHOW : SW_HIDE);
        }
      }
      SetHelp(ui, ui.tab_help[tab]);
    }

    void RefreshPacks(Ui &ui, bool deep)
    {
      const fs::path root = map_packs::TitleRoot(ui.ctx.user_data_root);
      for (size_t i = 0; i < map_packs::Packs().size(); ++i)
      {
        const map_packs::State st = map_packs::StateOf(root, i, deep);
        const bool installed_copy = st.status == map_packs::Status::kInstalled ||
                                    st.status == map_packs::Status::kInstalledRepaired;
        if (ui.ctx.packs_on_disc)
        {
          // The US Platinum Hits: the pack is on the disc. A copy installed before (from another disc's setup) can
          // still be removed; nothing can be added.
          SetWindowTextW(GetDlgItem(ui.window, kPackStatus0 + int(i)), L"Included on your disc");
          // "Included" (not "Add...", whose accent style is drawn bright even when disabled): a greyed panel button.
          SetWindowTextW(GetDlgItem(ui.window, kPackButton0 + int(i)), installed_copy ? L"Remove" : L"Included");
          EnableWindow(GetDlgItem(ui.window, kPackButton0 + int(i)), installed_copy ? TRUE : FALSE);
          continue;
        }
        const std::wstring status = st.status == map_packs::Status::kInstalled          ? L"Installed"
                                    : st.status == map_packs::Status::kInstalledRepaired ? L"Installed (repaired)"
                                    : st.status == map_packs::Status::kDidNotVerify
                                        ? L"Didn't verify: " + W(st.reason.substr(0, 60))
                                        : L"Not installed";
        SetWindowTextW(GetDlgItem(ui.window, kPackStatus0 + int(i)), status.c_str());
        const bool installed = st.status == map_packs::Status::kInstalled ||
                               st.status == map_packs::Status::kInstalledRepaired;
        SetWindowTextW(GetDlgItem(ui.window, kPackButton0 + int(i)), installed ? L"Remove" : L"Add...");
      }
    }

    void AddPackFiles(Ui &ui)
    {
      if (ui.ctx.packs_on_disc)
      {
        // Installing them on top would only add a second copy of maps the disc already has.
        Box(ui.window, L"Your disc already includes both map packs, so there's nothing to add.",
            L"PDZ Restored Settings", MB_OK | MB_ICONINFORMATION);
        return;
      }
      fs::path file;
      if (!setup_ui::PickFile(L"Choose a map pack file",
                              {{L"Map pack files", L"*.*"}}, file))
      {
        return;
      }
      int index = -1;
      std::string log;
      const bool ok = map_packs::Install(file, map_packs::TitleRoot(ui.ctx.user_data_root), index, log);
      RefreshPacks(ui, false);
      const std::wstring name = index >= 0 ? W(map_packs::Packs()[size_t(index)].name) : L"This file";
      Box(ui.window, (name + L": " + W(log)).c_str(), L"PDZ Restored Settings",
                  MB_OK | (ok ? MB_ICONINFORMATION : MB_ICONWARNING));
    }

    // ---- Controls: one row per action, two bindings each ----

    struct BindRow
    {
      const char *cvar;
      const wchar_t *label; // the game's name, the pad button in brackets
      const char *defaults; // settings\mapping.toml's committed value
      const wchar_t *help;
    };

    const BindRow kBindRows[] = {
        {"keybind_right_trigger", L"Fire (RT)", "LMB", L"Fires your weapon. Hold for automatic fire."},
        {"keybind_left_trigger", L"Aim / zoom (LT)", "RMB",
         L"Aim or zoom while held. The mouse slows down to match the zoom."},
        {"keybind_x", L"Reload (X)", "R", L"Reloads your weapon."},
        {"keybind_a", L"Use / open / take cover (A)", "Space,E",
         L"Use switches, open doors, pick things up and take cover against walls. PDZ has no jump button."},
        {"keybind_lstick_press", L"Crouch (L3)", "Control,C", L"Crouch, or stand back up."},
        {"keybind_left_shoulder", L"Dodge (LB)", "Shift,Mouse5",
         L"Roll in the direction you're moving. Hold a movement key at the same time."},
        {"keybind_b", L"Melee (B)", "V,Mouse4", L"Hit with the weapon in your hand."},
        {"keybind_y", L"Switch weapon (Y)", "Q,WheelUp,WheelDown",
         L"Switches weapon. PDZ only has one switch button, so both wheel directions do the same."},
        {"keybind_right_shoulder", L"Secondary mode (RB)", "F,MMB",
         L"Switches your weapon to its secondary mode. Every gun has its own."},
        {"keybind_start", L"Pause (Start)", "Escape,P",
         L"Pauses the game. P is Start in menus too (Esc goes back there)."},
        {"keybind_back", L"Objectives / scores (Back)", "Tab",
         L"Shows your objectives, or the scores in multiplayer."},
        {"keybind_lstick_up", L"Move forward (left stick up)", "W", L"Walk forward."},
        {"keybind_lstick_down", L"Move back (left stick down)", "S", L"Walk back."},
        {"keybind_lstick_left", L"Strafe left (left stick left)", "A", L"Step to the left."},
        {"keybind_lstick_right", L"Strafe right (left stick right)", "D", L"Step to the right."},
        {"keybind_dpad_up", L"Gadget up (D-pad up)", "1,Up", L"Gadget and item slot. Also moves up in menus."},
        {"keybind_dpad_right", L"Gadget right (D-pad right)", "2,Right",
         L"Gadget and item slot. Also moves right in menus."},
        {"keybind_dpad_down", L"Gadget down (D-pad down)", "3,Down",
         L"Gadget and item slot. Also moves down in menus."},
        {"keybind_dpad_left", L"Gadget left (D-pad left)", "4,Left",
         L"Gadget slot, used for the Data Thief. Also moves left in menus."},
        {"keybind_rstick_up", L"Look up with keys (right stick up)", "Numpad8",
         L"For looking around with the keyboard instead of the mouse. The mouse still works too."},
        {"keybind_rstick_down", L"Look down with keys (right stick down)", "Numpad2",
         L"For looking around with the keyboard instead of the mouse. The mouse still works too."},
        {"keybind_rstick_left", L"Turn left with keys (right stick left)", "Numpad4",
         L"For looking around with the keyboard instead of the mouse. The mouse still works too."},
        {"keybind_rstick_right", L"Turn right with keys (right stick right)", "Numpad6",
         L"For looking around with the keyboard instead of the mouse. The mouse still works too."},
        {"keybind_rstick_press", L"Right stick click (R3)", "Z", L"Same as clicking in the controller's right stick."},
    };
    constexpr int kBindRowCount = int(sizeof(kBindRows) / sizeof(kBindRows[0]));
    const wchar_t *const kWaiting = L"Press a key or mouse button, or Esc to cancel";

    std::vector<std::string> SplitTokens(const std::string &value)
    {
      std::vector<std::string> out;
      std::stringstream ss(value);
      std::string t;
      while (std::getline(ss, t, ','))
      {
        const size_t a = t.find_first_not_of(' '), b = t.find_last_not_of(' ');
        if (a != std::string::npos)
        {
          out.push_back(t.substr(a, b - a + 1));
        }
      }
      return out;
    }

    std::string JoinTokens(const std::vector<std::string> &tokens)
    {
      std::string out;
      for (const auto &t : tokens)
      {
        out += (out.empty() ? "" : ",") + t;
      }
      return out;
    }

    // "Shift+X" -> modifiers "Shift+", key "X".
    std::pair<std::string, std::string> SplitModifiers(const std::string &token)
    {
      const size_t plus = token.rfind('+');
      if (plus == std::string::npos || plus == 0 || plus + 1 >= token.size())
      {
        return {std::string(), token};
      }
      return {token.substr(0, plus + 1), token.substr(plus + 1)};
    }

    std::wstring TokenName(const std::string &token)
    {
      const auto [mods, key] = SplitModifiers(token);
      const uint16_t code = rex::ui::ParseInputCode(key);
      return W(mods) + (code ? W(rex::ui::InputCodeDisplayName(code)) : W(key) + L" (?)");
    }

    // The same input, whatever its spelling ("Control" / "Ctrl"): bare keys by code, tokens with modifiers as text.
    bool SameInput(const std::string &a, const std::string &b)
    {
      const auto [ma, ka] = SplitModifiers(a);
      const auto [mb, kb] = SplitModifiers(b);
      if (!ma.empty() || !mb.empty())
      {
        return a == b;
      }
      const uint16_t ca = rex::ui::ParseInputCode(ka), cb = rex::ui::ParseInputCode(kb);
      if (!ca)
      {
        return a == b;
      }
      if (ca == cb)
      {
        return true;
      }
      // One key under both names: "Ctrl" (either side) is pressed by Right Ctrl too, Enter by Numpad Enter.
      auto overlaps = [](uint16_t generic, uint16_t sided)
      {
        const int sc = sided >= rex::ui::kInputCodeScancodeBase ? sided - rex::ui::kInputCodeScancodeBase : -1;
        switch (generic)
        {
        case 0x10: // Shift
          return sc == 225 || sc == 229; // SDL_SCANCODE_LSHIFT / RSHIFT
        case 0x11: // Control
          return sc == 224 || sc == 228; // LCTRL / RCTRL
        case 0x12: // Alt
          return sc == 226 || sc == 230; // LALT / RALT
        case 0x0D: // Enter
          return sc == 88;               // KP_ENTER
        default:
          return false;
        }
      };
      return overlaps(ca, cb) || overlaps(cb, ca);
    }

    std::wstring SlotText(const Ui &ui, int row, int slot)
    {
      if (row == ui.cap_row && slot == ui.cap_slot)
      {
        return L"Press a key or mouse button...";
      }
      const auto &t = ui.binds[size_t(row)];
      if (slot == 0)
      {
        return t.empty() ? L"" : TokenName(t[0]);
      }
      std::wstring out;
      for (size_t i = 1; i < t.size(); ++i)
      {
        out += (i > 1 ? L", " : L"") + TokenName(t[i]);
      }
      return out;
    }

    void SetCell(HWND list, int row, int col, const std::wstring &text)
    {
      LVITEMW it{};
      it.iSubItem = col;
      it.pszText = const_cast<wchar_t *>(text.c_str());
      SendMessageW(list, LVM_SETITEMTEXTW, WPARAM(row), reinterpret_cast<LPARAM>(&it));
    }

    // The menus' own keys (the SDK's mnk menu mode, mnk_input_driver.cpp kMenuKeys: left / right click, Enter, Esc,
    // the wheel; Numpad Enter is Enter to it). While a menu is up those keys do the menu's work and no bind sees them,
    // so a Start bound only to them gives no Start in menus (the multiplayer setup screen needs one). A modifier
    // doesn't help: the key itself is masked.
    bool MenuReservedInput(const std::string &token)
    {
      const auto [mods, key] = SplitModifiers(token);
      const uint16_t code = rex::ui::ParseInputCode(key);
      return code == 0x01 /* LMB */ || code == 0x02 /* RMB */ || code == 0x0D /* Enter */ || code == 0x1B /* Esc */ ||
             code == rex::ui::kInputCodeWheelUp || code == rex::ui::kInputCodeWheelDown ||
             code == rex::ui::kInputCodeScancodeBase + 88 /* SDL_SCANCODE_KP_ENTER */;
    }

    // The Controls tab's warning line: shown while Start (keybind_start) has no key the menus leave alone (or no key
    // at all); empty otherwise. Called with every change of the rows (RefreshBinds).
    // The key list and its buttons (logical units, tab coordinates): with the Start warning the list is 146 tall and
    // the warning sits under the buttons; without it the list takes the warning's room (186). The list's own scroll
    // bar is clipped off by a window region (ours sits over its place), and the columns fill its client width, so
    // there is never a horizontal scroll bar.
    void PlaceKeyList(Ui &ui, bool warning)
    {
      if (!ui.list)
      {
        return;
      }
      const int x = 28, y = 84, w = 604, h = warning ? 146 : 186, by = y + h + 8;
      auto at = [&](HWND c, int cx, int cy, int cw, int ch)
      {
        MoveWindow(c, ui.px(cx + kContentDx), ui.px(cy + kContentDy), ui.px(cw), ui.px(ch), TRUE);
      };
      at(ui.list, x, y, w, h);
      at(GetDlgItem(ui.window, kBindChange1), x, by - 2, 120, 32);
      at(GetDlgItem(ui.window, kBindChange2), x + 128, by - 2, 120, 32);
      at(GetDlgItem(ui.window, kBindClear), x + 256, by - 2, 80, 32);
      at(GetDlgItem(ui.window, kBindReset), x + 404, by - 2, 200, 32);
      // The warning's own place, or nothing (an empty static over the buttons would paint over them).
      if (HWND w_line = GetDlgItem(ui.window, kBindWarning))
      {
        at(w_line, x, 274, 604, warning ? 40 : 0);
      }
      RECT wr, cr;
      GetWindowRect(ui.list, &wr);
      MapWindowPoints(nullptr, ui.window, reinterpret_cast<POINT *>(&wr), 2);
      GetClientRect(ui.list, &cr);
      // The list's own vertical scroll bar (always there: more rows than fit), at the DPI the list itself uses; the
      // client width is taken from it, not from GetClientRect, which may not count the bar until the list repaints.
      const int border = 1;
      const int sb = GetSystemMetricsForDpi(SM_CXVSCROLL, GetDpiForWindow(ui.list));
      cr.right = std::max<LONG>(0, (wr.right - wr.left) - 2 * border - sb);
      SetWindowRgn(ui.list, CreateRectRgn(0, 0, (wr.right - wr.left) - sb - border, wr.bottom - wr.top), TRUE);
      if (ui.list_scroll)
      {
        MoveWindow(ui.list_scroll, wr.right - sb - border, wr.top, sb + border, wr.bottom - wr.top, TRUE);
      }
      const int client = int(cr.right) - 1;
      const int action = client * 45 / 100, key = (client - action) / 2;
      SendMessageW(ui.list, LVM_SETCOLUMNWIDTH, 0, action);
      SendMessageW(ui.list, LVM_SETCOLUMNWIDTH, 1, key);
      SendMessageW(ui.list, LVM_SETCOLUMNWIDTH, 2, client - action - key);
      ui.list_scroll_top = -1;
      if (ui.list_scroll)
      {
        InvalidateRect(ui.list_scroll, nullptr, FALSE);
      }
    }

    void UpdateStartWarning(Ui &ui)
    {
      HWND warning = GetDlgItem(ui.window, kBindWarning);
      if (!warning)
      {
        return;
      }
      std::wstring text;
      for (int r = 0; r < kBindRowCount && r < int(ui.binds.size()); ++r)
      {
        if (std::string(kBindRows[r].cvar) != "keybind_start")
        {
          continue;
        }
        const auto &tokens = ui.binds[size_t(r)];
        const bool usable = std::any_of(tokens.begin(), tokens.end(),
                                        [](const std::string &t) { return !MenuReservedInput(t); });
        if (!usable)
        {
          text = tokens.empty()
                     ? L"Start has no key, so the menus have no Start and a multiplayer match can't be started. Add a "
                       L"plain key such as P (P is Start in menus by default)."
                     : L"Start only has keys the menus use for other things (Enter, Esc, the mouse buttons), so the "
                       L"menus have no Start and a multiplayer match can't be started. Add a plain key such as P.";
        }
      }
      wchar_t shown[512] = {};
      GetWindowTextW(warning, shown, 512);
      if (text != shown || !ui.list_placed)
      {
        SetWindowTextW(warning, text.c_str());
        InvalidateRect(warning, nullptr, TRUE);
        ui.list_placed = true;
        PlaceKeyList(ui, !text.empty());
        InvalidateRect(ui.window, nullptr, FALSE);
      }
    }

    void RefreshBinds(Ui &ui)
    {
      for (int r = 0; r < kBindRowCount; ++r)
      {
        SetCell(ui.list, r, 1, SlotText(ui, r, 0));
        SetCell(ui.list, r, 2, SlotText(ui, r, 1));
      }
      UpdateStartWarning(ui);
      RefreshLockText(ui);
    }

    int SelectedRow(const Ui &ui)
    {
      return int(SendMessageW(ui.list, LVM_GETNEXTITEM, WPARAM(-1), LVNI_SELECTED));
    }

    void Note(const std::string &line)
    {
      g_summary += (g_summary.empty() ? "" : "\n") + line;
    }

    void RawMouse(Ui &ui, bool on)
    {
      if (on == ui.raw_registered)
      {
        return;
      }
      RAWINPUTDEVICE d{};
      d.usUsagePage = 0x01; // generic desktop
      d.usUsage = 0x02;     // mouse
      d.dwFlags = on ? 0 : RIDEV_REMOVE;
      d.hwndTarget = on ? ui.window : nullptr;
      ui.raw_registered = RegisterRawInputDevices(&d, 1, sizeof(d)) ? on : false;
      ui.raw_prev = 0;
    }

    void EndCapture(Ui &ui)
    {
      const int row = ui.cap_row;
      ui.cap_row = -1;
      if (GetCapture() == ui.window)
      {
        ReleaseCapture();
      }
      RawMouse(ui, false);
      if (row >= 0)
      {
        RefreshBinds(ui);
      }
      SetHelp(ui, ui.tab_help[kTabControls]);
    }

    void StartCapture(Ui &ui, int row, int slot, bool by_click = false)
    {
      if (row < 0 || row >= kBindRowCount)
      {
        return;
      }
      ui.cap_row = row;
      ui.cap_slot = slot;
      ui.cap_by_click = by_click;
      ui.cap_started = GetTickCount();
      SendMessageW(ui.list, LVM_ENSUREVISIBLE, WPARAM(row), FALSE);
      RefreshBinds(ui);
      SetHelp(ui, std::wstring(kWaiting));
      SetFocus(ui.list);
      SetCapture(ui.window);
      RawMouse(ui, true);
    }

    // The captured input: put it in the row's slot, after asking when another action already has it.
    void Captured(Ui &ui, uint16_t code)
    {
      const int row = ui.cap_row, slot = ui.cap_slot;
      if (row < 0)
      {
        return;
      }
      const std::string token = rex::ui::InputCodeToken(code);
      if (token.empty())
      {
        EndCapture(ui);
        Note("pdz_settings: rebind: that input has no name the game reads; nothing changed");
        if (!ui.testing)
        {
          Box(ui.window, L"That key can't be used. Try another one.", L"PDZ Restored Settings",
                      MB_OK | MB_ICONWARNING);
        }
        return;
      }
      EndCapture(ui);
      auto &mine = ui.binds[size_t(row)];
      const std::string old = slot == 0 ? (mine.empty() ? std::string() : mine[0])
                                        : (mine.size() > 1 ? mine[1] : std::string());
      // Already this action's: the other slot gives it up.
      for (size_t i = 0; i < mine.size(); ++i)
      {
        if (SameInput(mine[i], token) && int(std::min<size_t>(i, 1)) != slot)
        {
          mine.erase(mine.begin() + ptrdiff_t(i));
          break;
        }
      }
      // Another action's: swap, both, or cancel.
      for (int other = 0; other < kBindRowCount; ++other)
      {
        auto &theirs = ui.binds[size_t(other)];
        auto it = std::find_if(theirs.begin(), theirs.end(), [&](const std::string &t) { return SameInput(t, token); });
        if (other == row || it == theirs.end())
        {
          continue;
        }
        const std::wstring name = W(rex::ui::InputCodeDisplayName(code));
        const std::wstring q = name + L" is already used for " + kBindRows[other].label +
                               L".\n\nYes: swap them.\nNo: use it for both.\nCancel: leave it as it was.";
        int answer = IDYES;
        if (ui.testing)
        {
          const std::string a = REXCVAR_GET(pdz_settings_test_answer);
          answer = a == "both" ? IDNO : a == "cancel" ? IDCANCEL : IDYES;
          Note(fmt::format("pdz_settings: rebind: {} is already {}: answered {}", rex::ui::InputCodeDisplayName(code),
                           kBindRows[other].cvar, a));
        }
        else
        {
          answer = Box(ui.window, q.c_str(), L"PDZ Restored Settings", MB_YESNOCANCEL | MB_ICONQUESTION);
        }
        if (answer == IDCANCEL)
        {
          RefreshBinds(ui);
          return;
        }
        if (answer == IDYES)
        {
          if (old.empty())
          {
            theirs.erase(it);
          }
          else
          {
            *it = old;
          }
        }
        break;
      }
      if (slot == 0 || mine.empty())
      {
        if (mine.empty())
        {
          mine.push_back(token);
        }
        else
        {
          mine[0] = token;
        }
      }
      else
      {
        mine.resize(1); // the second slot holds one input; any further ones it showed go
        mine.push_back(token);
      }
      RefreshBinds(ui);
      Note(fmt::format("pdz_settings: rebind: {} key {} = {} [{}] -> \"{}\"", kBindRows[row].cvar, slot + 1,
                       rex::ui::InputCodeDisplayName(code), token, JoinTokens(mine)));
    }

    // While a rebind waits: every key, mouse button and wheel message goes to it (true = swallowed).
    bool CaptureMessage(Ui &ui, MSG &msg)
    {
      if (ui.cap_row < 0)
      {
        return false;
      }
      switch (msg.message)
      {
      case WM_KEYDOWN:
      case WM_SYSKEYDOWN:
      {
        if (msg.lParam & (1 << 30))
        {
          return true; // auto-repeat (the Enter that started this, held)
        }
        if (msg.wParam == VK_ESCAPE)
        {
          EndCapture(ui);
          Note("pdz_settings: rebind: cancelled (Esc)");
          return true;
        }
        const int sc = rex::ui::Win32KeyMessageToScancode(uintptr_t(msg.wParam), intptr_t(msg.lParam));
        Captured(ui, rex::ui::InputCodeForScancode(sc));
        return true;
      }
      case WM_KEYUP:
      case WM_SYSKEYUP:
        if (msg.wParam == VK_SNAPSHOT)
        {
          // Print Screen sends only its key-up.
          Captured(ui, rex::ui::InputCodeForScancode(
                           rex::ui::Win32KeyMessageToScancode(uintptr_t(msg.wParam), intptr_t(msg.lParam))));
        }
        return true;
      case WM_CHAR:
      case WM_SYSCHAR:
      case WM_DEADCHAR:
      case WM_SYSDEADCHAR:
        return true;
      case WM_LBUTTONDOWN:
      case WM_LBUTTONDBLCLK:
      case WM_NCLBUTTONDOWN:
        // The second click of a double-click on the row isn't the answer.
        if (ui.cap_by_click && GetTickCount() - ui.cap_started < GetDoubleClickTime())
        {
          return true;
        }
        Captured(ui, rex::ui::InputCodeForMouseButton(1));
        return true;
      case WM_RBUTTONDOWN:
      case WM_RBUTTONDBLCLK:
      case WM_NCRBUTTONDOWN:
        Captured(ui, rex::ui::InputCodeForMouseButton(2));
        return true;
      case WM_MBUTTONDOWN:
      case WM_MBUTTONDBLCLK:
      case WM_NCMBUTTONDOWN:
        Captured(ui, rex::ui::InputCodeForMouseButton(3));
        return true;
      case WM_XBUTTONDOWN:
      case WM_XBUTTONDBLCLK:
      case WM_NCXBUTTONDOWN:
        Captured(ui, rex::ui::InputCodeForMouseButton(HIWORD(msg.wParam) == XBUTTON2 ? 5 : 4));
        return true;
      case WM_MOUSEWHEEL:
        Captured(ui, short(HIWORD(msg.wParam)) > 0 ? rex::ui::kInputCodeWheelUp : rex::ui::kInputCodeWheelDown);
        return true;
      case WM_MOUSEHWHEEL:
        Captured(ui, short(HIWORD(msg.wParam)) > 0 ? rex::ui::kInputCodeWheelRight : rex::ui::kInputCodeWheelLeft);
        return true;
      case WM_LBUTTONUP:
      case WM_RBUTTONUP:
      case WM_MBUTTONUP:
      case WM_XBUTTONUP:
        return true;
      default:
        return false;
      }
    }

    // Raw Input's button state (bit n = button n + 1): a newly held button 6-32 is the capture.
    void RawButtons(Ui &ui, unsigned long buttons)
    {
      const unsigned long extra = buttons & ~0x1Ful;
      const unsigned long pressed = extra & ~ui.raw_prev;
      ui.raw_prev = extra;
      for (int bit = 5; bit < 32 && ui.cap_row >= 0; ++bit)
      {
        if (pressed & (1ul << bit))
        {
          Captured(ui, rex::ui::InputCodeForMouseButton(bit + 1));
        }
      }
    }

    void ResetBinds(Ui &ui)
    {
      for (int r = 0; r < kBindRowCount; ++r)
      {
        ui.binds[size_t(r)] = SplitTokens(kBindRows[r].defaults);
      }
      RefreshBinds(ui);
    }

    // The keybind lines that changed: (key, TOML value).
    std::vector<std::pair<std::string, std::string>> ChangedBinds(Ui &ui)
    {
      std::vector<std::pair<std::string, std::string>> out;
      for (int r = 0; r < kBindRowCount; ++r)
      {
        const std::string v = JoinTokens(ui.binds[size_t(r)]);
        if (v != rex::cvar::GetFlagByName(kBindRows[r].cvar))
        {
          out.push_back({kBindRows[r].cvar, "\"" + v + "\""});
        }
      }
      return out;
    }

    void Save(Ui &ui)
    {
      std::vector<std::pair<std::string, std::string>> keys;
      const int mon = Sel(ui, kMonitor);
      keys.push_back({"monitor", fmt::format("{}", mon <= 0 ? 0 : mon + 1)});
      const int mode = Sel(ui, kMode); // 0 borderless, 1 fullscreen, 2 windowed
      keys.push_back({"fullscreen", mode == 2 ? "false" : "true"});
      keys.push_back({"fullscreen_exclusive", mode == 1 ? "true" : "false"});
      const int res = Sel(ui, kResolution);
      if (mode == 1 && res > 0 && size_t(res - 1) < ui.modes.size())
      {
        keys.push_back({"resolution", fmt::format("\"{}x{}\"", ui.modes[size_t(res - 1)].first,
                                                  ui.modes[size_t(res - 1)].second)});
      }
      // Render scale: the native renderer's integer scale, from the desktop height as its auto profile (100% = auto).
      const Monitor &m = ui.monitors.empty() ? Monitor{} : ui.monitors[size_t(std::max(0, mon))];
      const int scale_sel = Sel(ui, kScale);
      keys.push_back({"pdz_native_scale",
                      scale_sel == 0 ? std::string() : fmt::format("{}", ScaleForChoice(scale_sel, int(m.height)))});
      if (scale_sel == 0 && rex::cvar::GetFlagSource("pdz_native_scale") == rex::cvar::Source::kConfig)
      {
        // 100%: the line is removed, and the value the file had is dropped for this start too, so the native
        // renderer's own pick applies now (not one start later). A command-line value is left alone.
        rex::cvar::ResetToDefault("pdz_native_scale");
      }
      const int vs = Sel(ui, kVsync); // on (fixed), vrr, off (vsync off; the pacing stays fixed)
      {
        // On keeps a hand-set "present" / "latch" (Steam Deck / Proton, A/B tests): the window only offers On and VRR.
        const std::string had = rex::cvar::GetFlagByName("pdz_present_pacing");
        const bool keep = vs != 1 && (had == "present" || had == "latch");
        keys.push_back({"pdz_present_pacing", vs == 1 ? "\"vrr\"" : keep ? "\"" + had + "\"" : "\"fixed\""});
      }
      keys.push_back({"vsync", vs == 2 ? "false" : "true"});
      const int fov_pos = int(SendMessageW(GetDlgItem(ui.window, kFov), TBM_GETPOS, 0, 0));
      keys.push_back({"pdz_fov_scale", fmt::format("{:.3f}", FovScaleOfPosition(fov_pos))});
      const int fps = Sel(ui, kCampaign); // auto, 120, 60
      keys.push_back({"pdz_sp_120", fps == 2 ? "false" : "true"});
      const int split = Sel(ui, kSplitCap); // auto, 60, 40, 30 (pdz_splitscreen_cap: 0 = auto)
      keys.push_back({"pdz_splitscreen_cap", split == 1 ? "60" : split == 2 ? "40" : split == 3 ? "30" : "0"});
      keys.push_back({"pdz_settings_show", Checked(ui, kDontShow) ? "false" : "true"});
      keys.push_back({"pdz_update_check", Checked(ui, kUpdateCheck) ? "true" : "false"});
      WriteKeys(ui.ctx.settings_file, keys);
      wchar_t sens[32] = {};
      GetWindowTextW(GetDlgItem(ui.window, kSensitivity), sens, 32);
      double s = _wtof(sens);
      s = std::clamp(s > 0.0 ? s : 2.0, 0.1, 20.0);
      const std::vector<std::pair<std::string, std::string>> mouse = {
          {"pdz_mouse_sensitivity", fmt::format("{:.2f}", s)},
          {"pdz_mouse_invert", Checked(ui, kInvert) ? "true" : "false"}};
      const auto binds = ChangedBinds(ui);
      std::vector<std::pair<std::string, std::string>> mapping = mouse;
      // Step zoom: written only when it changes (so a line in the settings file elsewhere isn't overridden).
      const std::string zoom_steps = Checked(ui, kZoomSteps) ? "true" : "false";
      if (zoom_steps != rex::cvar::GetFlagByName("pdz_zoom_steps"))
      {
        mapping.push_back({"pdz_zoom_steps", zoom_steps});
        rex::cvar::SetFlagByName("pdz_zoom_steps", zoom_steps);
      }
      // Locktopus: what steers the pick and the mouse sensitivity, written only when they change (like step zoom);
      // a choice made here also switches the keyboard / mouse Locktopus on (pdz_fix_minigame_keyboard).
      {
        const std::string inputs = pdz::minigame::PickInputsName(LockInputsOfSel(Sel(ui, kLockInputs)));
        const double counts =
            LockCountsOfPos(int(SendMessageW(GetDlgItem(ui.window, kLockSens), TBM_GETPOS, 0, 0)));
        const int now_pos = LockPosOfCounts(REXCVAR_GET(pdz_minigame_mouse_counts));
        const bool inputs_changed = inputs != REXCVAR_GET(pdz_minigame_inputs);
        const bool counts_changed =
            int(SendMessageW(GetDlgItem(ui.window, kLockSens), TBM_GETPOS, 0, 0)) != now_pos;
        if (inputs_changed)
        {
          mapping.push_back({"pdz_minigame_inputs", "\"" + inputs + "\""});
          rex::cvar::SetFlagByName("pdz_minigame_inputs", inputs);
        }
        if (counts_changed)
        {
          mapping.push_back({"pdz_minigame_mouse_counts", fmt::format("{:.0f}", counts)});
          rex::cvar::SetFlagByName("pdz_minigame_mouse_counts", fmt::format("{:.0f}", counts));
        }
        if ((inputs_changed || counts_changed) && !REXCVAR_GET(pdz_fix_minigame_keyboard))
        {
          mapping.push_back({"pdz_fix_minigame_keyboard", "true"});
          rex::cvar::SetFlagByName("pdz_fix_minigame_keyboard", "true");
        }
      }
      mapping.insert(mapping.end(), binds.begin(), binds.end());
      WriteKeys(ui.ctx.mapping_file, mapping);
      // And for this start.
      for (const auto &kv : keys)
      {
        if (!kv.second.empty())
        {
          std::string v = kv.second;
          if (v.size() >= 2 && v.front() == '"')
          {
            v = v.substr(1, v.size() - 2);
          }
          rex::cvar::SetFlagByName(kv.first, v);
        }
      }
      for (const auto &kv : mouse)
      {
        rex::cvar::SetFlagByName(kv.first, kv.second);
      }
      for (const auto &kv : binds)
      {
        rex::cvar::SetFlagByName(kv.first, kv.second.substr(1, kv.second.size() - 2));
      }
      Note(fmt::format("pdz_settings: the settings window saved {} lines in {} and {} in {} ({} keybind lines changed)",
                       keys.size(), ui.ctx.settings_file.string(), mapping.size(), ui.ctx.mapping_file.string(),
                       binds.size()));
      for (const auto &kv : binds)
      {
        Note(fmt::format("pdz_settings: {} = {}", kv.first, kv.second));
      }
    }

    void ResetDefaults(Ui &ui)
    {
      WriteKeys(ui.ctx.settings_file, {{"monitor", ""}, {"fullscreen_exclusive", ""}, {"resolution", ""},
                                       {"pdz_native_scale", ""}, {"pdz_present_pacing", ""}, {"vsync", ""},
                                       {"pdz_sp_120", ""}, {"pdz_splitscreen_cap", ""}, {"pdz_settings_show", ""},
                                       {"pdz_update_check", ""}});
      ResetBinds(ui);
      std::vector<std::pair<std::string, std::string>> mapping = {{"pdz_mouse_sensitivity", "2.0"},
                                                                  {"pdz_mouse_invert", "false"}};
      for (const auto &kv : ChangedBinds(ui))
      {
        mapping.push_back(kv);
        rex::cvar::SetFlagByName(kv.first, kv.second.substr(1, kv.second.size() - 2));
      }
      WriteKeys(ui.ctx.mapping_file, mapping);
      SetSel(ui, kMonitor, 0);
      SetSel(ui, kMode, 0);
      SetSel(ui, kResolution, 0);
      SetSel(ui, kScale, 0);
      SetSel(ui, kVsync, 0);
      SendMessageW(GetDlgItem(ui.window, kFov), TBM_SETPOS, TRUE, kFovMinDegrees);
      SetWindowTextW(GetDlgItem(ui.window, kFovValue), FovText(kFovMinDegrees).c_str());
      WriteKeys(ui.ctx.settings_file, {{"pdz_fov_scale", "1.000"}});
      rex::cvar::SetFlagByName("pdz_fov_scale", "1.0");
      SetSel(ui, kCampaign, 0);
      SetSel(ui, kSplitCap, 0);
      rex::cvar::SetFlagByName("pdz_splitscreen_cap", "0");
      SetWindowTextW(GetDlgItem(ui.window, kSensitivity), L"2.00");
      SetChecked(ui, kInvert, false);
      SetChecked(ui, kZoomSteps, false);
      SetSel(ui, kLockInputs, 2);
      SendMessageW(GetDlgItem(ui.window, kLockSens), TBM_SETPOS, TRUE, LockPosOfCounts(400.0));
      WriteKeys(ui.ctx.mapping_file, {{"pdz_minigame_inputs", ""}, {"pdz_minigame_mouse_counts", ""}});
      rex::cvar::SetFlagByName("pdz_minigame_inputs", "both");
      rex::cvar::SetFlagByName("pdz_minigame_mouse_counts", "400");
      SetChecked(ui, kUpdateCheck, true);
      rex::cvar::SetFlagByName("pdz_update_check", "true");
      if (rex::cvar::GetFlagByName("pdz_zoom_steps") == "true")
      {
        WriteKeys(ui.ctx.mapping_file, {{"pdz_zoom_steps", "false"}});
        rex::cvar::SetFlagByName("pdz_zoom_steps", "false");
      }
      Box(ui.window, L"Everything in this window is back to its default.", L"PDZ Restored Settings",
                  MB_OK | MB_ICONINFORMATION);
    }

    // ---- Controls: the Locktopus group and its practice dial ----

    // The mouse sensitivity trackbar: 0..20, Low (800 counts to full deflection) .. High (80); the default 400 is the
    // middle (two halves, each even on a log scale).
    constexpr int kLockSensMax = 20;
    double LockCountsOfPos(int pos)
    {
      pos = std::clamp(pos, 0, kLockSensMax);
      return pos <= 10 ? 800.0 * std::pow(0.5, pos / 10.0) : 400.0 * std::pow(0.2, (pos - 10) / 10.0);
    }
    int LockPosOfCounts(double counts)
    {
      counts = std::clamp(counts, 80.0, 800.0);
      const double pos = counts >= 400.0 ? 10.0 * std::log(counts / 800.0) / std::log(0.5)
                                         : 10.0 + 10.0 * std::log(counts / 400.0) / std::log(0.2);
      return std::clamp(int(std::lround(pos)), 0, kLockSensMax);
    }
    // The Mouse / Keys / Both choice <-> pdz_minigame_inputs.
    pdz::minigame::PickInputs LockInputsOfSel(int sel)
    {
      return sel == 0 ? pdz::minigame::PickInputs::kMouse
             : sel == 1 ? pdz::minigame::PickInputs::kKeys
                        : pdz::minigame::PickInputs::kBoth;
    }
    int LockSelOfInputs(pdz::minigame::PickInputs inputs)
    {
      return inputs == pdz::minigame::PickInputs::kMouse ? 0 : inputs == pdz::minigame::PickInputs::kKeys ? 1 : 2;
    }

    // The window's live choices for the pick: the game's own tuning with the Mouse / Keys / Both choice and the
    // sensitivity slider as they are now (not yet saved).
    pdz::minigame::SteerTuning LockTuning(Ui &ui)
    {
      pdz::minigame::SteerTuning t = pdz::minigame::LiveTuning();
      t.inputs = LockInputsOfSel(Sel(ui, kLockInputs));
      t.mouse_counts = LockCountsOfPos(int(SendMessageW(GetDlgItem(ui.window, kLockSens), TBM_GETPOS, 0, 0)));
      return t;
    }

    // The tokens bound now (the key list's rows, unsaved changes included) for a keybind cvar.
    const std::vector<std::string> &LockBinds(Ui &ui, const char *cvar)
    {
      static const std::vector<std::string> none;
      for (int r = 0; r < kBindRowCount && r < int(ui.binds.size()); ++r)
      {
        if (std::string(kBindRows[r].cvar) == cvar)
        {
          return ui.binds[size_t(r)];
        }
      }
      return none;
    }
    // A row's first key's name ("A"), or "-" if it has none.
    std::wstring LockKeyName(Ui &ui, const char *cvar)
    {
      const auto &t = LockBinds(ui, cvar);
      return t.empty() ? std::wstring(L"-") : TokenName(t[0]);
    }
    // The Locktopus line with the live keys (the default binds give the line exactly as written).
    std::wstring LockLine(Ui &ui)
    {
      return L"Locktopus (lock-pick gadget): move the mouse to steer the pick, or use " +
             LockKeyName(ui, "keybind_lstick_left") + L" / " + LockKeyName(ui, "keybind_lstick_right") +
             L" to turn it and " + LockKeyName(ui, "keybind_lstick_up") + L" / " +
             LockKeyName(ui, "keybind_lstick_down") + L" to push it in or out. Hold it on a pin until it sets.";
    }
    std::wstring LockKeysText(Ui &ui)
    {
      return L"Keys: " + LockKeyName(ui, "keybind_lstick_left") + L" / " + LockKeyName(ui, "keybind_lstick_right") +
             L" turn, " + LockKeyName(ui, "keybind_lstick_up") + L" / " + LockKeyName(ui, "keybind_lstick_down") +
             L" push in or out, Shift slows (your Move and Strafe keys; the Look keys too).";
    }
    void RefreshLockText(Ui &ui)
    {
      HWND keys = GetDlgItem(ui.window, kLockKeys);
      if (!keys)
      {
        return;
      }
      const std::wstring text = LockKeysText(ui);
      wchar_t shown[512] = {};
      GetWindowTextW(keys, shown, 512);
      if (text != shown)
      {
        SetWindowTextW(keys, text.c_str());
        InvalidateRect(keys, nullptr, TRUE);
      }
      const std::wstring line = LockLine(ui);
      for (int id : {kLockHeading, kLockKeys, kLockInputsLabel})
      {
        if (HWND c = GetDlgItem(ui.window, id))
        {
          ui.help_of[c] = line;
        }
      }
    }

    // Is any key bound to this cvar held (keyboard keys with a virtual key code; mouse buttons and modifiers ignored)?
    bool LockHeld(Ui &ui, const char *cvar)
    {
      for (const std::string &token : LockBinds(ui, cvar))
      {
        const auto [mods, key] = SplitModifiers(token);
        if (!mods.empty())
        {
          continue;
        }
        const uint16_t code = rex::ui::ParseInputCode(key);
        if (code > 0x07 && code < 0x100 && (GetAsyncKeyState(code) & 0x8000) != 0)
        {
          return true;
        }
      }
      return false;
    }

    // Rare's 12 Locktopus lock datas (docs/MOUSE_KEYBOARD.md, "The 12 locks"): their pins in Rare's order, as the
    // stick position each pin wants (x right, y up; deflection over 1 = a corner of the stick's square). Exact bytes
    // over 128 where the data's bytes are known (the Mansion intercom door, Subway's diversion side door), else from the
    // table's angle and deflection.
    struct RareLock
    {
      const char *name;
      int count;
      double pin[8][2]; // {x, y} for byte pins, {angle degrees, deflection} for table pins (polar = true)
      bool polar;
    };
    const RareLock kRareLocks[] = {
        {"club_officebackdoor", 2, {{90, 0.94}, {0, 0.94}}, true},
        {"club_autogunroom", 2, {{0, 0.86}, {180, 0.86}}, true},
        {"ware_factorysidedoor", 3, {{45, 1.10}, {0, 0.78}, {225, 1.10}}, true},
        {"ware_diversionsidedoor", 3, {{45 / 128.0, 100 / 128.0}, {-100 / 128.0, 30 / 128.0}, {-30 / 128.0, 100 / 128.0}},
         false},
        {"labo_generatorswitch", 6, {{309, 1.11}, {212, 1.02}, {252, 0.90}, {90, 0.94}, {138, 1.15}, {308, 1.08}}, true},
        {"labo_gasvalve", 6, {{45, 1.10}, {227, 1.18}, {350, 0.87}, {224, 1.19}, {38, 1.09}, {310, 1.13}}, true},
        {"mans_ladderlock1", 4, {{42, 1.11}, {221, 1.08}, {138, 1.06}, {306, 0.97}}, true},
        {"mans_intercomdoor", 4,
         {{47 / 128.0, 102 / 128.0}, {-10 / 128.0, -90 / 128.0}, {90 / 128.0, -40 / 128.0}, {-88 / 128.0, -60 / 128.0}},
         false},
        {"airp_main_gate", 6, {{45, 1.10}, {125, 1.00}, {240, 0.94}, {0, 0.86}, {217, 1.03}, {241, 0.94}}, true},
        {"cs2_water_valve", 8,
         {{217, 1.03}, {240, 0.91}, {0, 0.86}, {127, 0.97}, {240, 0.94}, {45, 1.10}, {217, 1.03}, {125, 1.00}}, true},
        {"ruin_nice_and_easy_for_chris", 1, {{270, 0.94}}, true},
        {"test", 3, {{318, 1.05}, {125, 0.86}, {37, 0.78}}, true},
    };
    constexpr int kRareLockCount = int(sizeof(kRareLocks) / sizeof(kRareLocks[0]));
    constexpr double kDialRadius[3] = {pdz::minigame::kPinRadiusAgent, pdz::minigame::kPinRadiusSecretAgent,
                                       pdz::minigame::kPinRadiusPerfectAgent};

    // A new lock: one of Rare's 12 locks at random (its pins, in Rare's order).
    void DialNewLock(Ui &ui)
    {
      Ui::Dial &d = ui.dial;
      int lock = d.force_lock;
      if (lock < 0 || lock >= kRareLockCount)
      {
        lock = std::uniform_int_distribution<int>(0, kRareLockCount - 1)(d.rng);
      }
      d.force_lock = -1;
      d.lock = lock;
      const RareLock &l = kRareLocks[lock];
      d.pins.clear();
      for (int i = 0; i < l.count; ++i)
      {
        if (l.polar)
        {
          const double a = l.pin[i][0] * 3.14159265358979323846 / 180.0;
          d.pins.push_back({l.pin[i][1] * std::cos(a), l.pin[i][1] * std::sin(a), false});
        }
        else
        {
          d.pins.push_back({l.pin[i][0], l.pin[i][1], false});
        }
      }
      d.cur = 0;
      d.hold = pdz::minigame::kPinHoldSeconds;
      d.done = false;
      d.pick = pdz::minigame::Pick{};
      d.elapsed = 0.0;
      ++d.locks;
      if (d.wnd)
      {
        InvalidateRect(d.wnd, nullptr, FALSE);
      }
    }

    void DialRelease(Ui &ui)
    {
      Ui::Dial &d = ui.dial;
      if (!d.captured)
      {
        return;
      }
      d.captured = false;
      ClipCursor(nullptr);
      ShowCursor(TRUE);
      if (GetCapture() == d.wnd)
      {
        ReleaseCapture();
      }
      if (d.wnd)
      {
        InvalidateRect(d.wnd, nullptr, FALSE);
      }
    }

    // One step of the dial, dt seconds: the inputs (keys while it has the focus, the mouse while it is pointed at or
    // holds the mouse; or the test's), the game's steering step, Rare's hold rule on the current pin.
    void DialStep(Ui &ui, double dt)
    {
      Ui::Dial &d = ui.dial;
      double kx = 0.0, ky = 0.0, mdx = 0.0, mdy = 0.0;
      bool slow = false;
      if (d.test)
      {
        kx = d.tkx;
        ky = d.tky;
        slow = d.tslow;
        mdx = d.tdx;
        mdy = d.tdy;
      }
      else
      {
        if (GetFocus() == d.wnd && GetForegroundWindow() == ui.window)
        {
          kx = (LockHeld(ui, "keybind_lstick_right") || LockHeld(ui, "keybind_rstick_right") ? 1.0 : 0.0) -
               (LockHeld(ui, "keybind_lstick_left") || LockHeld(ui, "keybind_rstick_left") ? 1.0 : 0.0);
          ky = (LockHeld(ui, "keybind_lstick_up") || LockHeld(ui, "keybind_rstick_up") ? 1.0 : 0.0) -
               (LockHeld(ui, "keybind_lstick_down") || LockHeld(ui, "keybind_rstick_down") ? 1.0 : 0.0);
          slow = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
        }
        mdx = d.mdx;
        mdy = d.mdy;
      }
      d.mdx = d.mdy = 0.0;
      d.radius = kDialRadius[std::clamp(Sel(ui, kLockDifficulty), 0, 2)]; // Agent / Secret Agent / Perfect Agent
      const bool moved = pdz::minigame::SteerPick(d.pick, LockTuning(ui), kx, ky, slow, mdx, mdy, dt);
      bool changed = moved;
      if (!d.done && d.cur < int(d.pins.size()))
      {
        d.elapsed += dt;
        const double x = d.pick.radius * std::cos(d.pick.angle), y = d.pick.radius * std::sin(d.pick.angle);
        const double before = d.hold;
        auto &pin = d.pins[size_t(d.cur)];
        if (pdz::minigame::StepPinHold(d.hold, x, y, pin.x, pin.y, d.radius, dt))
        {
          pin.set = true;
          if (d.test || ui.testing)
          {
            Note(fmt::format("pdz_settings: test dial: lock {} pin {} of {} SET at {:.2f} s (pin {:+.2f} {:+.2f}, "
                             "deflection {:.2f}; pick {:+.2f} {:+.2f})",
                             d.locks, d.cur + 1, d.pins.size(), d.elapsed, pin.x, pin.y, std::hypot(pin.x, pin.y), x,
                             y));
          }
          ++d.cur;
          d.hold = pdz::minigame::kPinHoldSeconds;
          if (d.cur >= int(d.pins.size()))
          {
            d.done = true;
            if (d.test || ui.testing)
            {
              Note(fmt::format("pdz_settings: test dial: lock {} Unlocked! after {:.2f} s", d.locks, d.elapsed));
            }
          }
        }
        changed = changed || d.hold != before;
      }
      if (changed && d.wnd)
      {
        InvalidateRect(d.wnd, nullptr, FALSE);
      }
    }

    void DialPaint(Ui &ui, HDC target, const RECT &rc)
    {
      Ui::Dial &d = ui.dial;
      HDC dc = CreateCompatibleDC(target);
      HBITMAP bmp = CreateCompatibleBitmap(target, rc.right, rc.bottom);
      HGDIOBJ old_bmp = SelectObject(dc, bmp);
      const bool focus = GetFocus() == d.wnd;
      theme::FillRect(dc, rc, theme::kBackground);
      theme::FillRound(dc, rc, ui.px(6), theme::kPanel, focus || d.captured ? theme::kCyan : theme::kBorder);
      const int size = std::min(rc.right, rc.bottom) - ui.px(12);
      // Rare's corner pins reach a deflection of 1.19 (the stick's square): the circle (1.0) leaves them room.
      const int cx = rc.right / 2, cy = rc.bottom / 2, R = int(size / 2 / 1.22);
      auto at = [&](double x, double y) { return POINT{cx + int(std::lround(x * R)), cy - int(std::lround(y * R))}; };
      auto circle = [&](POINT c, int r, COLORREF pen_colour, COLORREF fill, bool filled, int width)
      {
        HPEN pen = CreatePen(PS_SOLID, std::max(1, width), pen_colour);
        HBRUSH brush = filled ? CreateSolidBrush(fill) : nullptr;
        HGDIOBJ op = SelectObject(dc, pen);
        HGDIOBJ ob = SelectObject(dc, filled ? static_cast<HGDIOBJ>(brush) : GetStockObject(NULL_BRUSH));
        Ellipse(dc, c.x - r, c.y - r, c.x + r + 1, c.y + r + 1);
        SelectObject(dc, op);
        SelectObject(dc, ob);
        DeleteObject(pen);
        if (brush)
        {
          DeleteObject(brush);
        }
      };
      // The lock's face, the current pin's window (Rare's radius), the pins, the pick.
      circle(POINT{cx, cy}, R, theme::kBorder, RGB(0x0A, 0x16, 0x38), true, ui.px(2));
      if (!d.done && d.cur < int(d.pins.size()))
      {
        const auto &p = d.pins[size_t(d.cur)];
        circle(at(p.x, p.y), int(d.radius * R), RGB(0x2A, 0x4A, 0x8A), 0, false, 1);
      }
      const int pin_r = std::max(3, ui.px(5));
      for (size_t i = 0; i < d.pins.size(); ++i)
      {
        const auto &p = d.pins[i];
        const bool current = !d.done && int(i) == d.cur;
        const COLORREF c = p.set ? theme::kCyan : current ? theme::kOrange : theme::kTextDim;
        circle(at(p.x, p.y), pin_r, c, c, p.set || current, 1);
        if (current && d.hold < pdz::minigame::kPinHoldSeconds)
        {
          // The hold: a ring that closes as the pin sets.
          const double f = 1.0 - d.hold / pdz::minigame::kPinHoldSeconds;
          const POINT c0 = at(p.x, p.y);
          const int rr = pin_r + ui.px(4);
          HPEN pen = CreatePen(PS_SOLID, std::max(2, ui.px(2)), theme::kCyan);
          HGDIOBJ op = SelectObject(dc, pen);
          SetArcDirection(dc, AD_CLOCKWISE);
          const double a1 = 3.14159265358979323846 / 2 - f * 2 * 3.14159265358979323846;
          Arc(dc, c0.x - rr, c0.y - rr, c0.x + rr, c0.y + rr, c0.x, c0.y - rr,
              c0.x + int(std::lround(rr * std::cos(a1))), c0.y - int(std::lround(rr * std::sin(a1))));
          SelectObject(dc, op);
          DeleteObject(pen);
        }
      }
      {
        const POINT tip = at(d.pick.radius * std::cos(d.pick.angle), d.pick.radius * std::sin(d.pick.angle));
        HPEN pen = CreatePen(PS_SOLID, std::max(2, ui.px(2)), RGB(255, 255, 255));
        HGDIOBJ op = SelectObject(dc, pen);
        MoveToEx(dc, cx, cy, nullptr);
        LineTo(dc, tip.x, tip.y);
        SelectObject(dc, op);
        DeleteObject(pen);
        circle(tip, std::max(2, ui.px(3)), RGB(255, 255, 255), RGB(255, 255, 255), true, 1);
      }
      if (d.done)
      {
        RECT t{cx - ui.px(44), cy - ui.px(12), cx + ui.px(44), cy + ui.px(12)};
        theme::FillRound(dc, t, ui.px(5), theme::kBackground, theme::kCyan);
        theme::Text(dc, L"Unlocked!", t, ui.tf.body_bold, theme::kCyan, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
      }
      else if (d.captured)
      {
        RECT t = rc;
        t.top = rc.bottom - ui.px(20);
        theme::Text(dc, L"Esc lets go", t, ui.tf.note, theme::kTextDim, DT_CENTER | DT_SINGLELINE);
      }
      BitBlt(target, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
      SelectObject(dc, old_bmp);
      DeleteObject(bmp);
      DeleteDC(dc);
    }

    LRESULT CALLBACK DialProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
      Ui *ui = g_ui;
      switch (msg)
      {
      case WM_GETDLGCODE:
        // The arrows and letters while it has the focus (they steer); Tab, Enter and Esc stay the window's, except Esc
        // while it holds the mouse.
        return DLGC_WANTARROWS | DLGC_WANTCHARS | (ui && ui->dial.captured ? DLGC_WANTALLKEYS : 0);
      case WM_ERASEBKGND:
        return 1;
      case WM_PAINT:
      {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        if (ui)
        {
          RECT rc;
          GetClientRect(hwnd, &rc);
          DialPaint(*ui, dc, rc);
        }
        EndPaint(hwnd, &ps);
        return 0;
      }
      case WM_LBUTTONDOWN:
        if (ui)
        {
          SetFocus(hwnd);
          if (ui->dial.captured)
          {
            DialRelease(*ui);
          }
          else
          {
            // Holds the mouse: the pointer hides and stays on the dial; its motion steers (raw counts, as in the game).
            ui->dial.captured = true;
            SetCapture(hwnd);
            RECT r;
            GetWindowRect(hwnd, &r);
            ClipCursor(&r);
            ShowCursor(FALSE);
            InvalidateRect(hwnd, nullptr, FALSE);
          }
        }
        return 0;
      case WM_KEYDOWN:
        if (ui && wp == VK_ESCAPE && ui->dial.captured)
        {
          DialRelease(*ui);
          return 0;
        }
        break;
      case WM_CHAR:
        return 0; // the letters steer; no beep
      case WM_SETFOCUS:
      case WM_KILLFOCUS:
        if (ui && msg == WM_KILLFOCUS)
        {
          DialRelease(*ui);
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      case WM_CAPTURECHANGED:
        if (ui && ui->dial.captured && reinterpret_cast<HWND>(lp) != hwnd)
        {
          ui->dial.captured = false;
          ClipCursor(nullptr);
          ShowCursor(TRUE);
          InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
      case WM_TIMER:
        if (ui && IsWindowVisible(hwnd) && !ui->dial.test)
        {
          LARGE_INTEGER now, freq;
          QueryPerformanceCounter(&now);
          QueryPerformanceFrequency(&freq);
          const double dt = ui->dial.last.QuadPart
                                ? std::clamp(double(now.QuadPart - ui->dial.last.QuadPart) / double(freq.QuadPart), 0.0,
                                             0.05)
                                : 0.0;
          ui->dial.last = now;
          POINT pt;
          GetCursorPos(&pt);
          ui->dial.hover = WindowFromPoint(pt) == hwnd && GetForegroundWindow() == ui->window;
          // Raw mouse counts while it is pointed at or holds the mouse (the key list's capture shares the
          // registration).
          const bool want = ui->dial.hover || ui->dial.captured;
          if (want && !ui->raw_registered)
          {
            RawMouse(*ui, true);
          }
          else if (!want && ui->raw_registered && ui->cap_row < 0)
          {
            RawMouse(*ui, false);
          }
          if (!want)
          {
            ui->dial.mdx = ui->dial.mdy = 0.0;
          }
          DialStep(*ui, dt);
        }
        else if (ui)
        {
          ui->dial.last.QuadPart = 0;
        }
        return 0;
      default:
        break;
      }
      return DefWindowProcW(hwnd, msg, wp, lp);
    }

    // pdz_settings_test_dial: one run ("keys", "mouse" or "ignore"), steering like a player would, through DialStep.
    void DialTestRun(Ui &ui, const std::string &kind, int n, const std::string &capture,
                     const std::function<void(DWORD)> &pump)
    {
      Ui::Dial &d = ui.dial;
      d.rng.seed(1000u + unsigned(n));
      d.test = true;
      // The inputs: Both for the keys and mouse runs; for "ignore", Keys with the mouse moving, then Mouse with keys.
      // keys_pa / mouse_pa: Perfect Agent on the Mansion intercom door (its part-way pins); the others Secret Agent.
      SetSel(ui, kLockInputs, 2);
      const bool pa = kind == "keys_pa" || kind == "mouse_pa";
      SetSel(ui, kLockDifficulty, pa ? 2 : 1);
      if (pa)
      {
        for (int i = 0; i < kRareLockCount; ++i)
        {
          d.force_lock = std::string(kRareLocks[i].name) == "mans_intercomdoor" ? i : d.force_lock;
        }
      }
      DialNewLock(ui);
      d.radius = kDialRadius[std::clamp(Sel(ui, kLockDifficulty), 0, 2)];
      std::string pins;
      for (const auto &p : d.pins)
      {
        pins += fmt::format(" ({:+.2f} {:+.2f}: angle {:.0f}, deflection {:.2f})", p.x, p.y,
                            std::atan2(p.y, p.x) * 180.0 / 3.14159265358979323846, std::hypot(p.x, p.y));
      }
      const pdz::minigame::SteerTuning tuning = LockTuning(ui);
      Note(fmt::format("pdz_settings: test dial: run {} '{}': lock {} = Rare's '{}' with {} pins{}; radius {:.2f}, "
                       "hold {:.1f} s; mouse {:.0f} counts, key turn {:.2f} s, slow {:.2f}",
                       n, kind, d.locks, d.lock >= 0 ? kRareLocks[d.lock].name : "?", d.pins.size(), pins, d.radius,
                       pdz::minigame::kPinHoldSeconds, tuning.mouse_counts, tuning.key_turn_s, tuning.key_slow));
      const double dt = 1.0 / 60.0;
      bool mid_taken = false;
      if (kind == "ignore")
      {
        const auto before = d.pick;
        SetSel(ui, kLockInputs, 1); // Keys: the mouse must not move it
        for (int i = 0; i < 60; ++i)
        {
          d.tkx = d.tky = 0.0;
          d.tdx = 20.0;
          d.tdy = -20.0;
          DialStep(ui, dt);
        }
        const bool mouse_ignored = d.pick.radius == before.radius && d.pick.angle == before.angle;
        SetSel(ui, kLockInputs, 0); // Mouse: the keys must not move it
        for (int i = 0; i < 60; ++i)
        {
          d.tdx = d.tdy = 0.0;
          d.tkx = 1.0;
          d.tky = 1.0;
          DialStep(ui, dt);
        }
        const bool keys_ignored = d.pick.radius == before.radius && d.pick.angle == before.angle;
        Note(fmt::format("pdz_settings: test dial: run {} 'ignore': Keys chosen, mouse moved 1 s: pick {}; Mouse "
                         "chosen, keys held 1 s: pick {}",
                         n, mouse_ignored ? "unmoved (PASS)" : "MOVED (FAIL)",
                         keys_ignored ? "unmoved (PASS)" : "MOVED (FAIL)"));
        SetSel(ui, kLockInputs, 2);
      }
      else
      {
        for (int step = 0; step < 60 * 30 && !d.done; ++step)
        {
          const auto &pin = d.pins[size_t(d.cur)];
          const double x = d.pick.radius * std::cos(d.pick.angle), y = d.pick.radius * std::sin(d.pick.angle);
          d.tkx = d.tky = d.tdx = d.tdy = 0.0;
          d.tslow = false;
          if (kind == "keys" || kind == "keys_pa")
          {
            // Like a player: turn towards the pin's angle (left / right), then push in or out to its depth, then hold.
            const double target_a = std::atan2(pin.y, pin.x), target_r = std::hypot(pin.x, pin.y);
            double da = std::remainder(target_a - d.pick.angle, 2.0 * 3.14159265358979323846);
            if (std::fabs(da) > 0.04)
            {
              d.tkx = da > 0 ? -1.0 : 1.0; // right turns clockwise
              d.tslow = std::fabs(da) < 0.25;
            }
            else if (std::fabs(target_r - d.pick.radius) > 0.03)
            {
              d.tky = target_r > d.pick.radius ? 1.0 : -1.0;
              d.tslow = std::fabs(target_r - d.pick.radius) < 0.12;
            }
          }
          else
          {
            // The mouse: counts towards the pin (down = positive y), at most 12 counts a step (720 a second).
            const double ex = pin.x - x, ey = pin.y - y;
            if (std::hypot(ex, ey) > 0.04)
            {
              d.tdx = std::clamp(ex * tuning.mouse_counts, -12.0, 12.0);
              d.tdy = std::clamp(-ey * tuning.mouse_counts, -12.0, 12.0);
            }
          }
          DialStep(ui, dt);
          if (step % 6 == 0)
          {
            pump(1);
          }
          // A picture mid-lock: the first pin set, the next one half held.
          if (!mid_taken && d.cur == 1 && !d.done && d.hold < 0.5 && !capture.empty())
          {
            mid_taken = true;
            InvalidateRect(d.wnd, nullptr, FALSE);
            pump(200);
            theme::CaptureWindow(ui.window,
                                 (fs::u8path(capture) / fmt::format("dial_{}_{}_mid.bmp", n, kind)).wstring());
            Note(fmt::format("pdz_settings: test dial: run {} '{}': mid-lock picture (pin 1 set, pin 2 held {:.2f} s "
                             "of {:.1f})",
                             n, kind, pdz::minigame::kPinHoldSeconds - d.hold, pdz::minigame::kPinHoldSeconds));
          }
        }
        if (!d.done)
        {
          Note(fmt::format("pdz_settings: test dial: run {} '{}': NOT unlocked after 30 s ({} of {} pins set)", n,
                           kind, d.cur, d.pins.size()));
        }
      }
      d.tkx = d.tky = d.tdx = d.tdy = 0.0;
      InvalidateRect(d.wnd, nullptr, FALSE);
      pump(400);
      if (!capture.empty())
      {
        theme::CaptureWindow(ui.window, (fs::u8path(capture) / fmt::format("dial_{}_{}.bmp", n, kind)).wstring());
      }
      d.test = false;
      SetSel(ui, kLockDifficulty, 1);
    }

    void OnCommand(Ui &ui, int id, int code)
    {
      if (id >= kTabButton0 && id < kTabButton0 + kTabCount)
      {
        if (ui.cap_row >= 0)
        {
          EndCapture(ui);
        }
        ShowTab(ui, id - kTabButton0);
        return;
      }
      if (auto seg = ui.seg_group.find(id); seg != ui.seg_group.end() && code == BN_CLICKED)
      {
        const int group = seg->second;
        Ui::Segmented &g = ui.segs[group];
        const int pick = int(std::find(g.parts.begin(), g.parts.end(), GetDlgItem(ui.window, id)) - g.parts.begin());
        if (pick != g.sel)
        {
          SetSel(ui, group, pick);
          OnCommand(ui, group, CBN_SELCHANGE); // as the drop-down list it replaces
        }
        return;
      }
      if (ui.drops.count(id) && code == BN_CLICKED)
      {
        const int pick = OpenDrop(ui, id);
        if (pick >= 0 && pick != ui.drops[id].sel)
        {
          SetSel(ui, id, pick);
          OnCommand(ui, id, CBN_SELCHANGE); // as the Windows drop-down list did
        }
        return;
      }
      if (auto t = ui.toggles.find(id); t != ui.toggles.end() && code == BN_CLICKED)
      {
        SetChecked(ui, id, !t->second);
        if (id == kUpdateCheck && Checked(ui, kUpdateCheck))
        {
          update_check::Start(); // ticked now: ask now (once per start)
        }
        return;
      }
      if (id == kUpdateOpen && code == BN_CLICKED && !ui.update_page.empty())
      {
        // The release's page in the browser; nothing is downloaded by us.
        const std::wstring page = W(ui.update_page);
        const auto r = reinterpret_cast<INT_PTR>(ShellExecuteW(ui.window, L"open", page.c_str(), nullptr, nullptr,
                                                               SW_SHOWNORMAL));
        Note(fmt::format("pdz_update: Open download page: {} ({})", ui.update_page, r > 32 ? "opened" : "failed"));
        return;
      }
      if (id == kPlay || id == kSafe)
      {
        Save(ui);
        ui.choice = id == kPlay ? Choice::kPlay : Choice::kSafeMode;
        DestroyWindow(ui.window);
        return;
      }
      if (id == kVsync && code == CBN_SELCHANGE)
      {
        // The chosen one's help (Off: the tearing warning) goes on the help line at once.
        const int sel = std::clamp(Sel(ui, kVsync), 0, 2);
        SetHelp(ui, std::wstring(kVsyncHelp[sel]));
        return;
      }
      if (id == kMonitor && code == CBN_SELCHANGE)
      {
        const int mon = Sel(ui, kMonitor);
        if (mon >= 0 && size_t(mon) < ui.monitors.size())
        {
          SetWindowTextW(GetDlgItem(ui.window, kCampaignNote), AutoNote(ui.monitors[size_t(mon)].hz).c_str());
        }
        return;
      }
      if (id >= kPackButton0 && id < kPackButton0 + 10)
      {
        const size_t i = size_t(id - kPackButton0);
        const fs::path root = map_packs::TitleRoot(ui.ctx.user_data_root);
        const auto st = map_packs::StateOf(root, i, false);
        if (st.status == map_packs::Status::kInstalled || st.status == map_packs::Status::kInstalledRepaired)
        {
          if (Box(ui.window, (W(map_packs::Packs()[i].name) + L": remove it from PDZ Restored? Your own copy "
                                                               L"isn't touched.").c_str(),
                          L"PDZ Restored Settings", MB_OKCANCEL | MB_ICONQUESTION) == IDOK)
          {
            map_packs::Remove(root, i);
          }
          RefreshPacks(ui, false);
        }
        else
        {
          AddPackFiles(ui);
        }
        return;
      }
      switch (id)
      {
      case kPackAddFiles:
        AddPackFiles(ui);
        break;
      case kPackCheck:
        RefreshPacks(ui, true);
        break;
      case kOpenSettings:
        ShellExecuteW(ui.window, L"open", L"notepad.exe", ui.ctx.settings_file.wstring().c_str(), nullptr, SW_SHOW);
        break;
      case kFovDefault:
        SendMessageW(GetDlgItem(ui.window, kFov), TBM_SETPOS, TRUE, kFovMinDegrees);
        SetWindowTextW(GetDlgItem(ui.window, kFovValue), FovText(kFovMinDegrees).c_str());
        break;
      case kBindChange1:
      case kBindChange2:
        StartCapture(ui, std::max(0, SelectedRow(ui)), id == kBindChange2 ? 1 : 0);
        break;
      case kBindClear:
      {
        const int row = SelectedRow(ui);
        if (row >= 0)
        {
          ui.binds[size_t(row)].clear();
          RefreshBinds(ui);
        }
        break;
      }
      case kBindReset:
        ResetBinds(ui);
        SetWindowTextW(GetDlgItem(ui.window, kSensitivity), L"2.00");
        SetChecked(ui, kInvert, false);
        SetChecked(ui, kZoomSteps, false);
        SetSel(ui, kLockInputs, 2);
        SendMessageW(GetDlgItem(ui.window, kLockSens), TBM_SETPOS, TRUE, LockPosOfCounts(400.0));
        break;
      case kLockNewLock:
        DialNewLock(ui);
        SetFocus(ui.dial.wnd);
        break;
      case kLockDifficulty:
        if (code == CBN_SELCHANGE)
        {
          ui.dial.hold = pdz::minigame::kPinHoldSeconds;
          ui.dial.radius = kDialRadius[std::clamp(Sel(ui, kLockDifficulty), 0, 2)];
          InvalidateRect(ui.dial.wnd, nullptr, FALSE);
        }
        break;
      case kOpenLogs:
      {
        std::error_code ec;
        fs::create_directories(ui.ctx.logs_dir, ec);
        ShellExecuteW(ui.window, L"open", ui.ctx.logs_dir.wstring().c_str(), nullptr, nullptr, SW_SHOW);
        break;
      }
      case kRebuild:
      {
        std::error_code ec;
        fs::remove(ui.ctx.cache_root / "native_shaders" / "generated.json", ec);
        Box(ui.window, L"The game's graphics are rebuilt from your game files next time you start. It takes a few "
                       L"seconds.",
                    L"PDZ Restored Settings", MB_OK | MB_ICONINFORMATION);
        break;
      }
      case kReset:
        ResetDefaults(ui);
        break;
      default:
        break;
      }
    }

    // ---- the key list's header and scroll bar ----

    // The key list's scroll position: top row, rows on a page, the row height.
    struct ListMetrics
    {
      int top = 0, page = 1, count = 0, row_h = 1, header_h = 0;
    };

    ListMetrics MetricsOf(HWND list)
    {
      ListMetrics m;
      m.top = int(SendMessageW(list, LVM_GETTOPINDEX, 0, 0));
      m.page = std::max(1, int(SendMessageW(list, LVM_GETCOUNTPERPAGE, 0, 0)));
      m.count = int(SendMessageW(list, LVM_GETITEMCOUNT, 0, 0));
      RECT r{};
      r.left = LVIR_BOUNDS;
      if (SendMessageW(list, LVM_GETITEMRECT, 0, reinterpret_cast<LPARAM>(&r)))
      {
        m.row_h = std::max<int>(1, r.bottom - r.top);
      }
      RECT hr{};
      GetWindowRect(reinterpret_cast<HWND>(SendMessageW(list, LVM_GETHEADER, 0, 0)), &hr);
      m.header_h = hr.bottom - hr.top;
      return m;
    }

    // The thumb's rectangle in the scroll bar (client coordinates), below the header strip.
    RECT ThumbRect(const RECT &rc, const ListMetrics &m)
    {
      const int track_top = m.header_h, track_h = std::max<int>(1, rc.bottom - m.header_h - 1);
      if (m.count <= m.page)
      {
        return RECT{rc.left + 2, track_top + 1, rc.right - 2, rc.bottom - 2};
      }
      const int thumb_h = std::max(track_h * m.page / m.count, track_h / 8);
      const int y = track_top + (track_h - thumb_h) * m.top / std::max(1, m.count - m.page);
      return RECT{rc.left + 3, y + 1, rc.right - 3, y + thumb_h - 1};
    }

    void ScrollListTo(HWND list, int top)
    {
      const ListMetrics m = MetricsOf(list);
      top = std::clamp(top, 0, std::max(0, m.count - m.page));
      if (top != m.top)
      {
        SendMessageW(list, LVM_SCROLL, 0, (top - m.top) * m.row_h);
      }
    }

    LRESULT CALLBACK ScrollProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
      Ui *ui = reinterpret_cast<Ui *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
      static int drag_from_y = -1, drag_from_top = 0;
      if (!ui || !ui->list)
      {
        return DefWindowProcW(hwnd, msg, wp, lp);
      }
      switch (msg)
      {
      case WM_ERASEBKGND:
        return 1;
      case WM_PAINT:
      {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        const ListMetrics m = MetricsOf(ui->list);
        theme::FillRect(dc, rc, theme::kSideBar);
        theme::FillRect(dc, RECT{rc.left, m.header_h, rc.right, rc.bottom}, theme::kPanel);
        theme::FillRect(dc, RECT{rc.left, m.header_h - 1, rc.right, m.header_h}, theme::kBorder);
        theme::FillRect(dc, RECT{rc.right - 1, rc.top, rc.right, rc.bottom}, theme::kBorder);
        theme::FillRect(dc, RECT{rc.left, rc.top, rc.right, rc.top + 1}, theme::kBorder);
        theme::FillRect(dc, RECT{rc.left, rc.bottom - 1, rc.right, rc.bottom}, theme::kBorder);
        if (m.count > m.page)
        {
          const RECT t = ThumbRect(rc, m);
          theme::FillRound(dc, t, std::max(1, int(t.right - t.left) / 2),
                           drag_from_y >= 0 ? theme::kBlue : RGB(0x2E, 0x50, 0x96),
                           drag_from_y >= 0 ? theme::kBlue : RGB(0x2E, 0x50, 0x96));
        }
        ui->list_scroll_top = m.top;
        EndPaint(hwnd, &ps);
        return 0;
      }
      case WM_LBUTTONDOWN:
      {
        RECT rc;
        GetClientRect(hwnd, &rc);
        const ListMetrics m = MetricsOf(ui->list);
        const RECT t = ThumbRect(rc, m);
        const int y = GET_Y_LPARAM(lp);
        if (y >= t.top && y < t.bottom)
        {
          drag_from_y = y;
          drag_from_top = m.top;
          SetCapture(hwnd);
        }
        else if (y >= m.header_h)
        {
          ScrollListTo(ui->list, m.top + (y < t.top ? -m.page : m.page)); // a page up / down
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      }
      case WM_MOUSEMOVE:
        if (drag_from_y >= 0)
        {
          RECT rc;
          GetClientRect(hwnd, &rc);
          const ListMetrics m = MetricsOf(ui->list);
          const RECT t = ThumbRect(rc, m);
          const int track = std::max<int>(1, (rc.bottom - m.header_h) - (t.bottom - t.top));
          ScrollListTo(ui->list,
                       drag_from_top + (GET_Y_LPARAM(lp) - drag_from_y) * std::max(1, m.count - m.page) / track);
          InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
      case WM_LBUTTONUP:
      case WM_CAPTURECHANGED:
        if (drag_from_y >= 0)
        {
          drag_from_y = -1;
          if (msg == WM_LBUTTONUP)
          {
            ReleaseCapture();
          }
          InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
      case WM_MOUSEWHEEL:
        SendMessageW(ui->list, msg, wp, lp);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      default:
        return DefWindowProcW(hwnd, msg, wp, lp);
      }
    }

    // The key list's header: the side bar's colour, the column names in silver in the display font, border lines.
    LRESULT CALLBACK ListProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data)
    {
      Ui *ui = reinterpret_cast<Ui *>(data);
      if (msg == WM_NOTIFY && ui)
      {
        NMHDR *nm = reinterpret_cast<NMHDR *>(lp);
        HWND header = reinterpret_cast<HWND>(SendMessageW(hwnd, LVM_GETHEADER, 0, 0));
        if (nm->hwndFrom == header && nm->code == NM_CUSTOMDRAW)
        {
          NMCUSTOMDRAW *cd = reinterpret_cast<NMCUSTOMDRAW *>(lp);
          if (cd->dwDrawStage == CDDS_PREPAINT)
          {
            RECT rc;
            GetClientRect(header, &rc);
            theme::FillRect(cd->hdc, rc, theme::kSideBar);
            theme::FillRect(cd->hdc, RECT{rc.left, rc.bottom - 1, rc.right, rc.bottom}, theme::kBorder);
            return CDRF_NOTIFYITEMDRAW;
          }
          if (cd->dwDrawStage == CDDS_ITEMPREPAINT)
          {
            RECT r = cd->rc;
            theme::FillRect(cd->hdc, r, theme::kSideBar);
            theme::FillRect(cd->hdc, RECT{r.left, r.bottom - 1, r.right, r.bottom}, theme::kBorder);
            theme::FillRect(cd->hdc, RECT{r.right - 1, r.top, r.right, r.bottom}, theme::kBorder);
            wchar_t text[64] = {};
            HDITEMW item{};
            item.mask = HDI_TEXT;
            item.pszText = text;
            item.cchTextMax = 64;
            SendMessageW(header, HDM_GETITEMW, cd->dwItemSpec, reinterpret_cast<LPARAM>(&item));
            std::wstring upper = text;
            CharUpperBuffW(upper.data(), DWORD(upper.size()));
            r.left += ui->px(8);
            theme::Text(cd->hdc, upper, r, ui->tf.display_small, theme::kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE,
                        ui->px(1));
            return CDRF_SKIPDEFAULT;
          }
          return CDRF_DODEFAULT;
        }
      }
      if (msg == WM_NCDESTROY)
      {
        RemoveWindowSubclass(hwnd, ListProc, 1);
      }
      return DefSubclassProc(hwnd, msg, wp, lp);
    }

    LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
      Ui *ui = g_ui;
      switch (msg)
      {
      case WM_COMMAND:
        if (ui)
        {
          OnCommand(*ui, LOWORD(wp), HIWORD(wp));
        }
        return 0;
      case WM_ERASEBKGND:
        return 1;
      case WM_PAINT:
      {
        PAINTSTRUCT ps;
        HDC target = BeginPaint(hwnd, &ps);
        if (ui)
        {
          RECT rc;
          GetClientRect(hwnd, &rc);
          HDC dc = CreateCompatibleDC(target);
          HBITMAP bmp = CreateCompatibleBitmap(target, rc.right, rc.bottom);
          HGDIOBJ old = SelectObject(dc, bmp);
          const int side = ui->px(kSideW), footer = rc.bottom - ui->px(kFooterH), help = footer - ui->px(kHelpH);
          theme::FillRect(dc, rc, theme::kBackground);
          theme::FillRect(dc, RECT{0, 0, side, footer}, theme::kSideBar);
          theme::FillRect(dc, RECT{side - 1, 0, side, footer}, theme::kBorder);
          theme::DrawTitle(dc, RECT{ui->px(22), ui->px(24), side, ui->px(56)}, ui->tf.title_side, ui->dpi, 1);
          theme::Text(dc, W(std::string("Version ") + PDZ_RESTORED_VERSION), RECT{ui->px(22), ui->px(54), side,
                      ui->px(74)}, ui->tf.note, theme::kTextDim, DT_LEFT | DT_SINGLELINE);
          if (!ui->update_version.empty())
          {
            theme::Text(dc, W("Version " + ui->update_version + " is available"),
                        RECT{ui->px(14), ui->px(456), side - ui->px(8), ui->px(490)}, ui->tf.note, theme::kCyan,
                        DT_LEFT | DT_WORDBREAK);
          }
          const wchar_t *headings[kTabCount] = {L"DISPLAY", L"FRAME RATE", L"MAP PACKS", L"CONTROLS", L"ADVANCED"};
          theme::Text(dc, headings[ui->tab], RECT{side + ui->px(32), ui->px(22), rc.right, ui->px(60)}, ui->tf.display,
                      theme::kText, DT_LEFT | DT_SINGLELINE, ui->px(2));
          theme::FillRect(dc, RECT{side, help, rc.right, footer}, theme::kSideBar);
          theme::FillRect(dc, RECT{side, help, rc.right, help + 1}, theme::kBorder);
          theme::DrawInfoIcon(dc, side + ui->px(32), help + (ui->px(kHelpH) - ui->px(18)) / 2, ui->px(18),
                              theme::kCyan);
          theme::FillRect(dc, RECT{0, footer, rc.right, rc.bottom}, theme::kSideBar);
          theme::FillRect(dc, RECT{0, footer, rc.right, footer + 1}, theme::kBorder);
          BitBlt(target, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
          SelectObject(dc, old);
          DeleteObject(bmp);
          DeleteDC(dc);
        }
        EndPaint(hwnd, &ps);
        return 0;
      }
      case WM_DRAWITEM:
        if (ui)
        {
          const DRAWITEMSTRUCT *di = reinterpret_cast<const DRAWITEMSTRUCT *>(lp);
          const int id = int(di->CtlID);
          HWND b = di->hwndItem;
          if (di->CtlType == ODT_STATIC && id == kHelp)
          {
            // The help strip's text: up to two lines (longer text is cut at the second), centred on the info icon.
            wchar_t help_text[1024] = {};
            GetWindowTextW(b, help_text, 1024);
            theme::FillRect(di->hDC, di->rcItem, theme::kSideBar);
            RECT need = di->rcItem;
            HGDIOBJ of = SelectObject(di->hDC, ui->tf.note);
            DrawTextW(di->hDC, help_text, -1, &need, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);
            SelectObject(di->hDC, of);
            RECT t = di->rcItem;
            const int h = std::min<int>(need.bottom - need.top, t.bottom - t.top);
            t.top += ((t.bottom - t.top) - h) / 2;
            t.bottom = t.top + h;
            theme::Text(di->hDC, help_text, t, ui->tf.note, RGB(0xC9, 0xD3, 0xE6),
                        DT_LEFT | DT_WORDBREAK | DT_NOPREFIX | DT_END_ELLIPSIS);
            return TRUE;
          }
          wchar_t text[256] = {};
          GetWindowTextW(b, text, 256);
          const bool hot = theme::IsHot(b), pressed = (di->itemState & ODS_SELECTED) != 0,
                     focus = (di->itemState & ODS_FOCUS) != 0, disabled = !IsWindowEnabled(b);
          // What is behind the button: the side bar / footer, or the page.
          RECT wr;
          GetWindowRect(b, &wr);
          MapWindowPoints(nullptr, hwnd, reinterpret_cast<POINT *>(&wr), 2);
          RECT client;
          GetClientRect(hwnd, &client);
          const bool on_bar = wr.left < ui->px(kSideW) || wr.top >= client.bottom - ui->px(kFooterH);
          theme::FillRect(di->hDC, di->rcItem, on_bar ? theme::kSideBar : theme::kBackground);
          if (id >= kTabButton0 && id < kTabButton0 + kTabCount)
          {
            theme::DrawButton(di->hDC, di->rcItem, theme::ButtonStyle::kTab, text, ui->tf, ui->dpi,
                              id - kTabButton0 == ui->tab, hot, pressed, focus, false);
          }
          else if (auto seg = ui->seg_group.find(id); seg != ui->seg_group.end())
          {
            const Ui::Segmented &g = ui->segs[seg->second];
            const int i = int(std::find(g.parts.begin(), g.parts.end(), b) - g.parts.begin());
            theme::DrawSegment(di->hDC, di->rcItem, text, ui->tf, ui->dpi, i == g.sel, hot, focus, disabled, i == 0,
                               i == int(g.parts.size()) - 1);
          }
          else if (auto d = ui->drops.find(id); d != ui->drops.end())
          {
            // Panel, border (cyan with the focus), the choice in silver, a cyan chevron.
            const RECT r = di->rcItem;
            theme::FillRound(di->hDC, r, ui->px(6), hot ? theme::kPanelHot : theme::kPanel,
                             focus ? theme::kCyan : theme::kBorder);
            RECT t = r;
            t.left += ui->px(12);
            t.right -= ui->px(36);
            const std::wstring shown =
                d->second.items.empty() ? std::wstring() : d->second.items[size_t(d->second.sel)];
            theme::Text(di->hDC, shown, t, ui->tf.body, theme::kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            const int cx = r.right - ui->px(20), cy = (r.top + r.bottom) / 2, k = ui->px(5);
            HPEN pen = CreatePen(PS_SOLID, std::max(1, ui->px(2)), theme::kCyan);
            HGDIOBJ old = SelectObject(di->hDC, pen);
            MoveToEx(di->hDC, cx - k, cy - k / 2, nullptr);
            LineTo(di->hDC, cx, cy + k / 2);
            LineTo(di->hDC, cx + k + 1, cy - k / 2 - 1);
            SelectObject(di->hDC, old);
            DeleteObject(pen);
          }
          else if (auto t = ui->toggles.find(id); t != ui->toggles.end())
          {
            RECT r = di->rcItem;
            theme::FillRect(di->hDC, r, on_bar ? theme::kSideBar : theme::kBackground);
            theme::DrawToggle(di->hDC, r, text, ui->tf, ui->dpi, t->second, hot, focus);
          }
          else
          {
            const std::wstring label = text;
            const theme::ButtonStyle style =
                id == kPlay                                    ? theme::ButtonStyle::kPrimary
                : id == kSafe                                  ? theme::ButtonStyle::kSecondary
                : id == kReset || id == kBindReset || label == L"Remove" ? theme::ButtonStyle::kDanger
                : label == L"Add..."                           ? theme::ButtonStyle::kAccent
                                                               : theme::ButtonStyle::kPanel;
            theme::DrawButton(di->hDC, di->rcItem, style, label, ui->tf, ui->dpi, false, hot, pressed, focus, disabled);
          }
          return TRUE;
        }
        break;
      case WM_CTLCOLOREDIT:
      case WM_CTLCOLORLISTBOX:
      {
        HDC dc = reinterpret_cast<HDC>(wp);
        SetTextColor(dc, theme::kText);
        SetBkColor(dc, theme::kPanel);
        return reinterpret_cast<LRESULT>(theme::Brush(theme::kPanel));
      }
      case WM_NOTIFY:
      {
        const NMHDR *nm = reinterpret_cast<NMHDR *>(lp);
        if (ui && nm->code == NM_CUSTOMDRAW && (nm->idFrom == kFov || nm->idFrom == kLockSens))
        {
          // The field of view slider in the theme: a panel channel, a blue thumb with a cyan edge, no ticks.
          NMCUSTOMDRAW *cd = reinterpret_cast<NMCUSTOMDRAW *>(lp);
          if (cd->dwDrawStage == CDDS_PREPAINT)
          {
            return CDRF_NOTIFYITEMDRAW;
          }
          if (cd->dwDrawStage == CDDS_ITEMPREPAINT)
          {
            if (cd->dwItemSpec == TBCD_CHANNEL)
            {
              RECT r = cd->rc;
              InflateRect(&r, 0, ui->px(2));
              theme::FillRound(cd->hdc, r, ui->px(3), theme::kPanel, theme::kBorder);
            }
            else if (cd->dwItemSpec == TBCD_THUMB)
            {
              theme::FillRound(cd->hdc, cd->rc, ui->px(3), theme::kBlue, theme::kCyan);
            }
            return CDRF_SKIPDEFAULT;
          }
          return CDRF_DODEFAULT;
        }
        if (ui && nm->code == NM_CUSTOMDRAW && nm->idFrom == kBindList)
        {
          // The key list's rows: the selected row in blue, the row waiting for a key lighter.
          NMLVCUSTOMDRAW *cd = reinterpret_cast<NMLVCUSTOMDRAW *>(lp);
          if (cd->nmcd.dwDrawStage == CDDS_PREPAINT)
          {
            return CDRF_NOTIFYITEMDRAW;
          }
          if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT)
          {
            const int row = int(cd->nmcd.dwItemSpec);
            const bool selected = row == SelectedRow(*ui);
            cd->nmcd.uItemState &= ~(CDIS_SELECTED | CDIS_FOCUS);
            cd->clrText = selected || row == ui->cap_row ? RGB(255, 255, 255) : theme::kText;
            cd->clrTextBk = row == ui->cap_row ? theme::kPanelHot
                            : selected         ? theme::kBlue
                            : row % 2          ? kPanelAlt
                                               : theme::kPanel;
            return CDRF_NEWFONT | CDRF_NOTIFYPOSTPAINT;
          }
          if (cd->nmcd.dwDrawStage == CDDS_ITEMPOSTPAINT)
          {
            // The grid: a line under the row and between the columns, in the border colour.
            RECT r{};
            r.left = LVIR_BOUNDS;
            SendMessageW(ui->list, LVM_GETITEMRECT, cd->nmcd.dwItemSpec, reinterpret_cast<LPARAM>(&r));
            HPEN pen = CreatePen(PS_SOLID, 1, theme::kBorder);
            HGDIOBJ old = SelectObject(cd->nmcd.hdc, pen);
            MoveToEx(cd->nmcd.hdc, r.left, r.bottom - 1, nullptr);
            LineTo(cd->nmcd.hdc, r.right, r.bottom - 1);
            int x = r.left;
            for (int c = 0; c < 2; ++c)
            {
              x += int(SendMessageW(ui->list, LVM_GETCOLUMNWIDTH, WPARAM(c), 0));
              MoveToEx(cd->nmcd.hdc, x - 1, r.top, nullptr);
              LineTo(cd->nmcd.hdc, x - 1, r.bottom);
            }
            SelectObject(cd->nmcd.hdc, old);
            DeleteObject(pen);
            return CDRF_DODEFAULT;
          }
          return CDRF_DODEFAULT;
        }
        if (ui && nm->idFrom == kBindList && nm->code == NM_CLICK && ui->cap_row < 0)
        {
          // A click on the Key 2 column changes the second binding; anywhere else on the row, the first.
          const NMITEMACTIVATE *ia = reinterpret_cast<const NMITEMACTIVATE *>(lp);
          LVHITTESTINFO hit{};
          hit.pt = ia->ptAction;
          SendMessageW(ui->list, LVM_SUBITEMHITTEST, 0, reinterpret_cast<LPARAM>(&hit));
          if (hit.iItem >= 0)
          {
            StartCapture(*ui, hit.iItem, hit.iSubItem == 2 ? 1 : 0, true);
          }
        }
        return 0;
      }
      case WM_INPUT:
        if (ui && ui->cap_row < 0 && (ui->dial.hover || ui->dial.captured))
        {
          RAWINPUT raw{};
          UINT size = sizeof(raw);
          if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER)) !=
                  UINT(-1) &&
              raw.header.dwType == RIM_TYPEMOUSE && (raw.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) == 0)
          {
            ui->dial.mdx += raw.data.mouse.lLastX;
            ui->dial.mdy += raw.data.mouse.lLastY;
          }
        }
        if (ui && ui->cap_row >= 0)
        {
          RAWINPUT raw{};
          UINT size = sizeof(raw);
          if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER)) !=
                  UINT(-1) &&
              raw.header.dwType == RIM_TYPEMOUSE)
          {
            RawButtons(*ui, raw.data.mouse.ulRawButtons);
          }
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
      case WM_ACTIVATE:
        if (ui && LOWORD(wp) == WA_INACTIVE && ui->cap_row >= 0 && !ui->testing)
        {
          EndCapture(*ui);
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
      case WM_CAPTURECHANGED:
        if (ui && ui->cap_row >= 0 && reinterpret_cast<HWND>(lp) != hwnd && !ui->testing)
        {
          EndCapture(*ui);
        }
        return 0;
      case WM_TIMER:
        if (ui && ui->update_version.empty())
        {
          // The update check (a background thread): a newer release shows the notice and the button.
          const update_check::Result r = update_check::Get();
          if (r.state == update_check::State::kNewer)
          {
            ui->update_version = r.version;
            ui->update_page = r.page;
            ShowWindow(GetDlgItem(hwnd, kUpdateOpen), SW_SHOW);
            InvalidateRect(hwnd, nullptr, FALSE);
          }
        }
        if (ui)
        {
          // A controller: up / down move the focus, A presses, LB / RB change tabs, left / right change a choice.
          const UINT vk = theme::PollController(hwnd);
          if (vk == VK_PRIOR || vk == VK_NEXT)
          {
            OnCommand(*ui, kTabButton0 + (ui->tab + (vk == VK_NEXT ? 1 : kTabCount - 1)) % kTabCount, BN_CLICKED);
          }
          else if (vk == VK_LEFT || vk == VK_RIGHT)
          {
            const int focus_id = GetDlgCtrlID(GetFocus());
            if (auto seg = ui->seg_group.find(focus_id); seg != ui->seg_group.end())
            {
              const Ui::Segmented &g = ui->segs[seg->second];
              const int next = std::clamp(g.sel + (vk == VK_RIGHT ? 1 : -1), 0, int(g.parts.size()) - 1);
              SetFocus(g.parts[size_t(next)]);
              SendMessageW(g.parts[size_t(next)], BM_CLICK, 0, 0);
            }
          }
          else if (vk == VK_ESCAPE && ui->cap_row >= 0)
          {
            EndCapture(*ui);
          }
        }
        if (ui && ui->list && ui->list_scroll && IsWindowVisible(ui->list_scroll) &&
            int(SendMessageW(ui->list, LVM_GETTOPINDEX, 0, 0)) != ui->list_scroll_top)
        {
          InvalidateRect(ui->list_scroll, nullptr, FALSE);
        }
        if (ui && ui->cap_row < 0)
        {
          // The help line: the control under the mouse, else the one with the keyboard focus.
          POINT pt;
          GetCursorPos(&pt);
          HWND under = WindowFromPoint(pt);
          auto it = ui->help_of.find(under);
          if (it == ui->help_of.end() || !IsWindowVisible(under))
          {
            it = ui->help_of.find(GetFocus());
          }
          static HWND shown = nullptr;
          static int shown_row = -1;
          HWND key = it != ui->help_of.end() ? it->first : nullptr;
          // The key list: the row under the mouse, else the selected one.
          int row = -1;
          if (key && key == ui->list)
          {
            LVHITTESTINFO hit{};
            hit.pt = pt;
            ScreenToClient(ui->list, &hit.pt);
            row = under == ui->list ? int(SendMessageW(ui->list, LVM_HITTEST, 0, reinterpret_cast<LPARAM>(&hit))) : -1;
            row = row >= 0 ? row : SelectedRow(*ui);
          }
          if (key != shown || row != shown_row)
          {
            shown = key;
            shown_row = row;
            const std::wstring text =
                row >= 0 && row < kBindRowCount
                    ? std::wstring(kBindRows[row].help) + L" Click Key 1 or Key 2 (or press Enter) to change it."
                    : std::wstring(key ? it->second : ui->tab_help[ui->tab]);
            SetHelp(*ui, text);
          }
        }
        return 0;
      case WM_HSCROLL:
        if (ui && reinterpret_cast<HWND>(lp) == GetDlgItem(hwnd, kFov))
        {
          const int pos = int(SendMessageW(reinterpret_cast<HWND>(lp), TBM_GETPOS, 0, 0));
          SetWindowTextW(GetDlgItem(hwnd, kFovValue), FovText(pos).c_str());
        }
        return 0;
      case WM_CTLCOLORSTATIC:
      {
        // Labels on the page; the help line on its strip; notes in the dimmer or the info colour.
        HDC dc = reinterpret_cast<HDC>(wp);
        const int id = GetDlgCtrlID(reinterpret_cast<HWND>(lp));
        const bool strip = id == kHelp;
        COLORREF text = theme::kText;
        if (id >= kPackStatus0 && id < kPackStatus0 + 10)
        {
          // A pack's status: installed in the info colour, not installed dimmer, didn't verify as a warning.
          wchar_t status[16] = {};
          GetWindowTextW(reinterpret_cast<HWND>(lp), status, 16);
          text = wcsncmp(status, L"Installed", 9) == 0 || wcsncmp(status, L"Included", 8) == 0 ? theme::kCyan
                 : wcsncmp(status, L"Didn't", 6) == 0  ? theme::kOrange
                                                       : theme::kTextDim;
        }
        else if (id == kCampaignNote)
        {
          text = theme::kCyan;
        }
        else if (id == kMultiplayerNote || id == kSplitCapNote || (id >= kPackMaps0 && id < kPackMaps0 + 10) ||
                 id == kWindowedNote)
        {
          text = theme::kTextDim;
        }
        else if (id == kBindWarning)
        {
          text = theme::kOrange; // the Controls tab's warning (Start without a menu key)
        }
        else if (strip)
        {
          text = RGB(0xC9, 0xD3, 0xE6);
        }
        SetTextColor(dc, text);
        SetBkColor(dc, strip ? theme::kSideBar : theme::kBackground);
        SetBkMode(dc, OPAQUE);
        return reinterpret_cast<LRESULT>(theme::Brush(strip ? theme::kSideBar : theme::kBackground));
      }
      case WM_CLOSE:
        // The close box (or Alt+F4): the settings are saved as with Play, and the game doesn't start (kQuit).
        if (ui && ui->save_on_close && !ui->done)
        {
          Save(*ui);
          Note("pdz_settings: the settings window was closed without playing: settings saved, the game doesn't start");
        }
        DestroyWindow(hwnd);
        return 0;
      case WM_DESTROY:
        if (ui)
        {
          ui->done = true;
        }
        PostQuitMessage(0);
        return 0;
      default:
        return DefWindowProcW(hwnd, msg, wp, lp);
      }
    }

    // The capture test's layout check (every DPI it runs at): text that doesn't fit its control, controls that run into
    // the help strip, off the window or into each other, help lines longer than the strip's two lines. One line per
    // problem, then a count ("layout check: 0 problems" = nothing clipped or overlapping).
    void CheckLayout(Ui &ui)
    {
      int problems = 0;
      auto narrow = [](const std::wstring &w)
      {
        std::string out;
        for (wchar_t c : w)
        {
          out += c < 128 ? char(c) : '?';
        }
        return out.size() > 70 ? out.substr(0, 70) + "..." : out;
      };
      HDC dc = GetDC(ui.window);
      auto text_height = [&](const std::wstring &text, HFONT font, int width, UINT extra)
      {
        RECT r{0, 0, width, 0};
        HGDIOBJ of = SelectObject(dc, font);
        DrawTextW(dc, text.c_str(), -1, &r, DT_LEFT | DT_WORDBREAK | DT_CALCRECT | extra);
        SelectObject(dc, of);
        return int(r.bottom);
      };
      auto text_width = [&](const std::wstring &text, HFONT font)
      {
        RECT r{0, 0, 0, 0};
        HGDIOBJ of = SelectObject(dc, font);
        DrawTextW(dc, text.c_str(), -1, &r, DT_LEFT | DT_SINGLELINE | DT_CALCRECT | DT_NOPREFIX);
        SelectObject(dc, of);
        return int(r.right);
      };
      RECT client;
      GetClientRect(ui.window, &client);
      const int strip_top = client.bottom - ui.px(kFooterH) - ui.px(kHelpH);
      auto rect_of = [&](HWND c)
      {
        RECT r;
        GetWindowRect(c, &r);
        MapWindowPoints(nullptr, ui.window, reinterpret_cast<POINT *>(&r), 2);
        return r;
      };
      for (int t = 0; t < kTabCount; ++t)
      {
        std::vector<std::pair<HWND, RECT>> rects;
        for (HWND c : ui.page[t])
        {
          if (c == ui.list_scroll)
          {
            continue;
          }
          const RECT r = rect_of(c);
          wchar_t cls[32] = {}, text[512] = {};
          GetClassNameW(c, cls, 32);
          GetWindowTextW(c, text, 512);
          const int w = r.right - r.left, h = r.bottom - r.top;
          if (r.bottom > strip_top)
          {
            Note(fmt::format("pdz_settings: layout check: tab {} '{}' runs into the help strip ({} px)", t,
                             narrow(text), r.bottom - strip_top));
            ++problems;
          }
          if (r.right > client.right - ui.px(8))
          {
            Note(fmt::format("pdz_settings: layout check: tab {} '{}' runs off the right edge", t, narrow(text)));
            ++problems;
          }
          const HFONT font = reinterpret_cast<HFONT>(SendMessageW(c, WM_GETFONT, 0, 0));
          if (_wcsicmp(cls, L"Static") == 0 && text[0])
          {
            const int need = text_height(text, font ? font : ui.font, w,
                                         (GetWindowLongW(c, GWL_STYLE) & SS_NOPREFIX) ? DT_NOPREFIX : 0);
            if (need > h)
            {
              Note(fmt::format("pdz_settings: layout check: tab {} text '{}' needs {} px, has {}", t, narrow(text), need,
                               h));
              ++problems;
            }
          }
          if (_wcsicmp(cls, L"Button") == 0 && text[0])
          {
            const bool toggle = ui.toggles.count(GetDlgCtrlID(c)) != 0;
            const int room = toggle ? w - ui.px(2 + 40 + 12) : w - ui.px(16);
            const int need = text_width(text, toggle ? ui.tf.body : ui.tf.body_bold);
            if (need > room)
            {
              Note(fmt::format("pdz_settings: layout check: tab {} button '{}' needs {} px, has {}", t, narrow(text),
                               need, room));
              ++problems;
            }
          }
          rects.push_back({c, r});
        }
        for (size_t i = 0; i < rects.size(); ++i)
        {
          for (size_t j = i + 1; j < rects.size(); ++j)
          {
            RECT a = rects[i].second, b = rects[j].second, x{};
            InflateRect(&a, -ui.px(2), -ui.px(2));
            InflateRect(&b, -ui.px(2), -ui.px(2));
            if (IntersectRect(&x, &a, &b))
            {
              wchar_t ta[128] = {}, tb[128] = {};
              GetWindowTextW(rects[i].first, ta, 128);
              GetWindowTextW(rects[j].first, tb, 128);
              Note(fmt::format("pdz_settings: layout check: tab {} '{}' overlaps '{}' (ids {} / {})", t, narrow(ta),
                               narrow(tb), GetDlgCtrlID(rects[i].first), GetDlgCtrlID(rects[j].first)));
              ++problems;
            }
          }
        }
      }
      // The footer's controls inside the window.
      for (int id : {kDontShow, kSafe, kPlay})
      {
        HWND c = GetDlgItem(ui.window, id);
        wchar_t text[256] = {};
        GetWindowTextW(c, text, 256);
        const RECT r = rect_of(c);
        const bool toggle = ui.toggles.count(id) != 0;
        const int room = toggle ? (r.right - r.left) - ui.px(2 + 40 + 12) : (r.right - r.left) - ui.px(16);
        const int need = text_width(text, toggle ? ui.tf.body : ui.tf.button);
        if (r.left < 0 || r.right > client.right || need > room)
        {
          Note(fmt::format("pdz_settings: layout check: footer '{}' doesn't fit ({} px of text, {} of room, left {})",
                           narrow(text), need, room, r.left));
          ++problems;
        }
      }
      // Help lines: two at most on the strip.
      RECT hr;
      GetClientRect(ui.help, &hr);
      const int line = text_height(L"Ag", ui.tf.note, hr.right, DT_NOPREFIX);
      std::set<std::wstring> helps;
      for (const auto &kv : ui.help_of)
      {
        helps.insert(kv.second);
      }
      for (int t = 0; t < kTabCount; ++t)
      {
        helps.insert(ui.tab_help[t]);
      }
      for (int r = 0; r < kBindRowCount; ++r)
      {
        helps.insert(std::wstring(kBindRows[r].help) + L" Click Key 1 or Key 2 (or press Enter) to change it.");
      }
      for (const wchar_t *v : kVsyncHelp)
      {
        helps.insert(v);
      }
      helps.insert(kWaiting);
      for (const std::wstring &h : helps)
      {
        const int need = text_height(h, ui.tf.note, hr.right, DT_NOPREFIX);
        if (need > 2 * line || need > hr.bottom)
        {
          Note(fmt::format("pdz_settings: layout check: help line '{}' is {} lines (two at most)", narrow(h),
                           (need + line - 1) / line));
          ++problems;
        }
      }
      ReleaseDC(ui.window, dc);
      Note(fmt::format("pdz_settings: layout check at {} dpi: {} help lines, {} problem(s)", ui.dpi, helps.size(),
                       problems));
    }

    // The common controls' current look for this window only (a comctl32 v6 manifest in the exe's resources, id 3,
    // activated around the window: the rest of the process keeps its own).
    struct VisualStyles
    {
      HANDLE ctx = INVALID_HANDLE_VALUE;
      ULONG_PTR cookie = 0;
      VisualStyles()
      {
        ACTCTXW a{};
        a.cbSize = sizeof(a);
        a.dwFlags = ACTCTX_FLAG_RESOURCE_NAME_VALID | ACTCTX_FLAG_HMODULE_VALID;
        a.hModule = GetModuleHandleW(nullptr);
        a.lpResourceName = MAKEINTRESOURCEW(3);
        ctx = CreateActCtxW(&a);
        if (ctx != INVALID_HANDLE_VALUE)
        {
          ActivateActCtx(ctx, &cookie);
        }
      }
      ~VisualStyles()
      {
        if (ctx != INVALID_HANDLE_VALUE)
        {
          DeactivateActCtx(0, cookie);
          ReleaseActCtx(ctx);
        }
      }
    };

    Choice Run(const Context &ctx, bool packs_tab)
    {
      VisualStyles styles;
      INITCOMMONCONTROLSEX icc{sizeof(icc),
                               ICC_TAB_CLASSES | ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES};
      InitCommonControlsEx(&icc);
      Ui ui;
      ui.ctx = ctx;
      g_ui = &ui;
      HDC screen = GetDC(nullptr);
      ui.dpi = GetDeviceCaps(screen, LOGPIXELSY);
      if (REXCVAR_GET(pdz_settings_test_dpi) > 0)
      {
        ui.dpi = REXCVAR_GET(pdz_settings_test_dpi); // test aid: lay the window out as at another scaling
      }
      ReleaseDC(nullptr, screen);
      ui.tf = theme::MakeFonts(ui.dpi);
      ui.font = ui.tf.body;
      ui.bold = ui.tf.body_bold;

      WNDCLASSEXW wc{sizeof(wc)};
      wc.lpfnWndProc = WndProc;
      wc.hInstance = GetModuleHandleW(nullptr);
      wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
      // The class gets a brush of its own, never the cached theme::Brush: UnregisterClassW (below) deletes a class's
      // background brush, and with the cached one deleted every later kBackground fill failed, so the shader
      // progress window and message boxes after the settings window were left white (2026-11-06).
      wc.hbrBackground = CreateSolidBrush(theme::kBackground);
      wc.lpszClassName = L"PDZRestoredSettings";
      wc.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1));
      if (!RegisterClassExW(&wc))
      {
        DeleteObject(wc.hbrBackground); // already registered: that registration's brush stays in use
      }
      const int width = ui.px(kWindowW), height = ui.px(kWindowH);
      RECT r{0, 0, width, height};
      AdjustWindowRect(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN, FALSE);
      const std::wstring window_title = W(std::string("PDZ Restored ") + PDZ_RESTORED_VERSION + " Settings");
      ui.window = CreateWindowExW(0, wc.lpszClassName, window_title.c_str(),
                                  WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN,
                                  CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr,
                                  wc.hInstance, nullptr);
      if (!ui.window)
      {
        g_ui = nullptr;
        return Choice::kPlay;
      }
      // Centre on the primary display.
      RECT wr;
      GetWindowRect(ui.window, &wr);
      SetWindowPos(ui.window, nullptr, (GetSystemMetrics(SM_CXSCREEN) - (wr.right - wr.left)) / 2,
                   (GetSystemMetrics(SM_CYSCREEN) - (wr.bottom - wr.top)) / 2, 0, 0, SWP_NOSIZE | SWP_NOZORDER);

      theme::ThemeTitleBar(ui.window);
      // The side bar: the title (painted), then the tabs.
      const wchar_t *names[kTabCount] = {L"Display", L"Frame rate", L"Map packs", L"Controls", L"Advanced"};
      for (int t = 0; t < kTabCount; ++t)
      {
        ui.tab_buttons[t] = Add(ui, -1, L"BUTTON", names[t], BS_OWNERDRAW | WS_TABSTOP, 12, 96 + t * 48, kSideW - 24,
                                44, kTabButton0 + t);
        theme::EnableHover(ui.tab_buttons[t]);
      }
      const int x = 28, lw = 200, cx = 240, cw = 380;

      // Display.
      ui.monitors = Monitors();
      std::vector<std::wstring> mons;
      size_t primary = 0;
      for (size_t i = 0; i < ui.monitors.size(); ++i)
      {
        mons.push_back(ui.monitors[i].label);
        primary = ui.monitors[i].primary ? i : primary;
      }
      if (mons.empty())
      {
        mons.push_back(L"Display 1 (main)");
      }
      const int cur_mon = std::max(0, std::atoi(rex::cvar::GetFlagByName("monitor").c_str()) - 1);
      Label(ui, kTabDisplay, L"Monitor", x, 52, lw, true);
      Combo(ui, kTabDisplay, cx, 48, cw, kMonitor, mons, cur_mon < int(mons.size()) ? cur_mon : int(primary),
            L"The screen the game opens on.");
      Label(ui, kTabDisplay, L"Screen mode", x, 92, lw, true);
      const bool exclusive = rex::cvar::GetFlagByName("fullscreen_exclusive") == "true";
      const bool windowed = rex::cvar::GetFlagByName("fullscreen") == "false";
      Segments(ui, kTabDisplay, cx, 88, 126, kMode, {L"Borderless", L"Fullscreen", L"Windowed"},
               windowed ? 2 : exclusive ? 1 : 0,
               L"Borderless fills the screen and lets you Alt+Tab instantly. Fullscreen takes over the screen "
               L"completely. Windowed runs the game in a window.");
      Label(ui, kTabDisplay, L"Resolution", x, 158, lw, true);
      ui.modes = Modes(ui.monitors.empty() ? std::wstring() : ui.monitors[primary].device);
      std::vector<std::wstring> res = {ui.monitors.empty() ? L"Desktop resolution"
                                                           : fmt::format(L"Desktop resolution ({} x {})",
                                                                         ui.monitors[primary].width,
                                                                         ui.monitors[primary].height)};
      for (const auto &[w, h] : ui.modes)
      {
        res.push_back(fmt::format(L"{} x {}", w, h));
      }
      Combo(ui, kTabDisplay, cx, 154, cw, kResolution, res, 0,
            L"Leave this on your desktop resolution unless you have a reason. Other sizes only apply in Fullscreen.");
      Label(ui, kTabDisplay, L"Render scale", x, 198, lw, true);
      // The saved choice (no line = 100%), matched against the choices for the screen Play saves for.
      int scale_sel = 0;
      {
        const std::string saved = SavedValue(ui.ctx.settings_file, "pdz_native_scale");
        const int mon_index = cur_mon < int(ui.monitors.size()) ? cur_mon : int(primary);
        const int height = ui.monitors.empty() ? 0 : int(ui.monitors[size_t(mon_index)].height);
        const int value = saved.empty() ? 0 : std::atoi(saved.c_str());
        scale_sel = value <= 0 ? 0 : value == ScaleForChoice(1, height) ? 1 : value == ScaleForChoice(2, height) ? 2 : 0;
        if (value > 0 && scale_sel == 0)
        {
          Note(fmt::format("pdz_settings: pdz_native_scale = {} matches no Render scale choice for this screen: shown "
                           "as 100%",
                           value));
        }
      }
      Segments(ui, kTabDisplay, cx, 194, 70, kScale, {L"100%", L"75%", L"50%"}, scale_sel,
            L"Draws the game at a lower resolution and scales it up. Lower is faster on weaker graphics cards, but a "
            L"bit softer.");
      Label(ui, kTabDisplay, L"VSync / VRR", x, 238, lw, true);
      const std::string pacing = rex::cvar::GetFlagByName("pdz_present_pacing");
      // vsync is the GPU plugin's: not registered yet when the window opens, so it is read from the settings file.
      const bool vsync = SavedValue(ui.ctx.settings_file, "vsync") != "false";
      // On = fixed (the default; an old "auto" reads as fixed), VRR = vrr, Off = vsync off. No Auto (2026-11-06): the
      // EDID says whether a display can do VRR, not whether the driver has it on (a FreeSync EDID with VRR off in the
      // driver juddered at 117 fps Present(0) on a fixed 120 Hz display).
      Segments(ui, kTabDisplay, cx, 234, 70, kVsync, {L"On", L"VRR (G-Sync / FreeSync)", L"Off"},
               !vsync ? 2 : pacing == "vrr" ? 1 : 0, kVsyncHelp[0], false, {70, 200, 70});
      for (size_t i = 0; i < ui.segs[kVsync].parts.size(); ++i)
      {
        ui.help_of[ui.segs[kVsync].parts[i]] = kVsyncHelp[i]; // each choice explains itself
      }
      Label(ui, kTabDisplay, L"Field of view", x, 278, lw, true);
      {
        const std::wstring help = L"How much you can see around you. Rare's default is 73\u00B0. Scopes keep the same "
                                  L"zoom. Vehicles, turrets and cutscenes always use Rare's view.";
        HWND fov = Add(ui, kTabDisplay, TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_AUTOTICKS | WS_TABSTOP, cx, 272, 200, 30,
                       kFov, help);
        SendMessageW(fov, TBM_SETRANGE, FALSE, MAKELPARAM(kFovMinDegrees, kFovMaxDegrees));
        SendMessageW(fov, TBM_SETTICFREQ, 5, 0);
        SendMessageW(fov, TBM_SETPAGESIZE, 0, 5);
        // The capture test shows Rare's default unless its rebind script sets a field of view.
        const bool capture_default = !REXCVAR_GET(pdz_settings_test_capture).empty() &&
                                     REXCVAR_GET(pdz_settings_test_rebind).find("pdz_fov_degrees") == std::string::npos;
        const int pos = capture_default
                            ? kFovMinDegrees
                            : FovPositionOfScale(std::atof(rex::cvar::GetFlagByName("pdz_fov_scale").c_str()));
        SendMessageW(fov, TBM_SETPOS, TRUE, pos);
        Add(ui, kTabDisplay, L"STATIC", FovText(pos), SS_LEFT, cx + 206, 278, 130, 22, kFovValue, help);
        Button(ui, kTabDisplay, L"Default (Rare's)", cx + 240, 302, 140, kFovDefault,
               L"Back to Rare's 73\u00B0.");
      }
      ui.tab_help[kTabDisplay] = L"Point at a setting to see what it does.";

      // Frame rate.
      const bool sp120 = rex::cvar::GetFlagByName("pdz_sp_120") != "false";
      Label(ui, kTabFrameRate, L"Campaign and co-op", x, 52, lw, true);
      Segments(ui, kTabFrameRate, cx, 48, 90, kCampaign, {L"Auto", L"120 fps", L"60 fps"}, sp120 ? 0 : 2,
            L"Auto picks 120 fps on 120 Hz screens and 60 on others. Pick 60 if your PC struggles to hold 120.");
      Add(ui, kTabFrameRate, L"STATIC",
          AutoNote(ui.monitors.empty() ? 60 : ui.monitors[size_t(std::min<int>(cur_mon, int(ui.monitors.size()) - 1))].hz),
          SS_LEFT, cx, 80, cw, 48, kCampaignNote);
      Label(ui, kTabFrameRate, L"Multiplayer", x, 142, lw, true);
      Segments(ui, kTabFrameRate, cx, 138, 90, kMultiplayer, {L"60 fps"}, 0,
               L"Multiplayer always runs at 60 fps. The game's timing and bots were built for it.", true);
      Add(ui, kTabFrameRate, L"STATIC", L"Locked at 60.", SS_LEFT, cx + 104, 142, 220, 22,
          kMultiplayerNote);
      // Split screen (pdz_splitscreen_cap, 2026-11-08): the frame rate while 2 or more players share the screen.
      Label(ui, kTabFrameRate, L"Split screen", x, 200, lw, true);
      const std::string split_cap = rex::cvar::GetFlagByName("pdz_splitscreen_cap");
      const int split_sel = split_cap == "60" ? 1 : split_cap == "40" ? 2 : split_cap == "30" ? 3 : 0;
      Segments(ui, kTabFrameRate, cx, 196, 90, kSplitCap, {L"Auto", L"60 fps", L"40 fps", L"30 fps"}, split_sel,
               L"The frame rate while two or more of you share the screen, in co-op or multiplayer. With three or "
               L"four players a steady 40 or 30 is smoother than dropped frames.");
      Add(ui, kTabFrameRate, L"STATIC",
          L"Only while 2 or more players share the screen, in the level. Auto: 60 fps with 2 players, 40 fps with 3 "
          L"or 4 on a 120 Hz screen.",
          SS_LEFT, cx, 230, cw + 40, 44, kSplitCapNote);
      ui.tab_help[kTabFrameRate] = L"Point at a setting to see what it does.";

      // Map packs.
      Add(ui, kTabPacks, L"STATIC",
          ctx.packs_on_disc
              ? L"Your disc is the Platinum Hits edition: both map packs and their six extra maps are already on it."
              : L"Optional. Add the map packs from your own Xbox 360 to get their six extra maps.",
          SS_LEFT, x, 48, 600, 34, 0);
      for (size_t i = 0; i < map_packs::Packs().size(); ++i)
      {
        const int y = 94 + int(i) * 64;
        Label(ui, kTabPacks, W(map_packs::Packs()[i].name), x, y, 200, true);
        Add(ui, kTabPacks, L"STATIC", W(map_packs::Packs()[i].maps), SS_LEFT, x, y + 20, 300, 22, kPackMaps0 + int(i));
        Add(ui, kTabPacks, L"STATIC", L"", SS_LEFT, 340, y, 200, 36, kPackStatus0 + int(i),
            L"Installed (repaired): your copy had a known fault, and it's been fixed. Not installed: its maps won't "
            L"show up in multiplayer.");
        Button(ui, kTabPacks, L"Add...", 550, y, 90, kPackButton0 + int(i),
               L"Add: pick the pack's file. Remove: takes it out of PDZ Restored only. Your own copy isn't touched.");
      }
      Button(ui, kTabPacks, L"Add pack files...", x, 236, 160, kPackAddFiles,
             L"Pick the pack files you copied from your Xbox 360: Content\\0000000000000000\\4D5307D3\\00000002 "
             L"on its drive. They're checked, and fixed if needed.");
      Button(ui, kTabPacks, L"Check installed packs", x + 170, 236, 180, kPackCheck,
             L"Checks the installed packs again, for example after you've moved the game folder.");
      ui.tab_help[kTabPacks] =
          ctx.packs_on_disc ? L"Both map packs are included on your disc. There's nothing to add."
          : packs_tab       ? L"Got the map packs? Add them now, or any time later from Settings."
                            : L"Point at a pack or button to see what it does.";
      if (ctx.packs_on_disc)
      {
        EnableWindow(GetDlgItem(ui.window, kPackAddFiles), FALSE);
        EnableWindow(GetDlgItem(ui.window, kPackCheck), FALSE);
      }

      // Controls.
      Label(ui, kTabControls, L"Mouse sensitivity", x, 52, 130, true);
      Add(ui, kTabControls, WC_EDITW, fmt::format(L"{:.2f}", std::atof(rex::cvar::GetFlagByName("pdz_mouse_sensitivity").c_str())),
          ES_LEFT | WS_BORDER | WS_TABSTOP, 160, 48, 70, 24, kSensitivity,
          L"How fast the view turns when you move the mouse. 2.0 is the default. Aiming down a scope is slower to "
          L"match.");
      Button(ui, kTabControls, L"Invert up / down", 260, 46, 176, kInvert, L"Mouse up looks down.", BS_AUTOCHECKBOX);
      if (rex::cvar::GetFlagByName("pdz_mouse_invert") == "true")
      {
        SetChecked(ui, kInvert, true);
      }
      Button(ui, kTabControls, L"Step zoom", 452, 46, 180, kZoomSteps,
             L"Aiming zooms to your gun's first zoom level. While aiming, click R3 or roll the mouse wheel to zoom in "
             L"further. Guns with one zoom level work as normal. Off: zoom works the way Rare made it.",
             BS_AUTOCHECKBOX);
      if (rex::cvar::GetFlagByName("pdz_zoom_steps") == "true")
      {
        SetChecked(ui, kZoomSteps, true);
      }
      ui.list = Add(ui, kTabControls, WC_LISTVIEWW, L"",
                    LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER | WS_BORDER | WS_TABSTOP, x, 84,
                    604, 146, kBindList, L"");
      const wchar_t *const list_help = L"Click Key 1 or Key 2, then press the key or mouse button you want. In menus, "
                                       L"Enter or left click selects, and Esc or right click goes back.";
      ui.help_of[ui.list] = list_help;
      SendMessageW(ui.list, LVM_SETEXTENDEDLISTVIEWSTYLE, 0,
                   LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
      SetWindowTheme(ui.list, L"DarkMode_Explorer", nullptr);
      SetWindowTheme(reinterpret_cast<HWND>(SendMessageW(ui.list, LVM_GETHEADER, 0, 0)), L"DarkMode_ItemsView", nullptr);
      SendMessageW(ui.list, LVM_SETBKCOLOR, 0, theme::kPanel);
      SendMessageW(ui.list, LVM_SETTEXTBKCOLOR, 0, theme::kPanel);
      SendMessageW(ui.list, LVM_SETTEXTCOLOR, 0, theme::kText);
      SetWindowSubclass(ui.list, ListProc, 1, reinterpret_cast<DWORD_PTR>(&ui));
      {
        // Our scroll bar over the list's right edge; the list's own is clipped off (its window region).
        RECT lr;
        GetWindowRect(ui.list, &lr);
        MapWindowPoints(nullptr, ui.window, reinterpret_cast<POINT *>(&lr), 2);
        const int sb = GetSystemMetrics(SM_CXVSCROLL);
        const int w = lr.right - lr.left, h = lr.bottom - lr.top;
        SetWindowRgn(ui.list, CreateRectRgn(0, 0, w - sb - 1, h), FALSE);
        static bool registered = false;
        if (!registered)
        {
          WNDCLASSW wc{};
          wc.lpfnWndProc = ScrollProc;
          wc.hInstance = GetModuleHandleW(nullptr);
          wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
          wc.lpszClassName = L"PDZRestoredScroll";
          registered = RegisterClassW(&wc) != 0;
        }
        ui.list_scroll = CreateWindowExW(0, L"PDZRestoredScroll", L"", WS_CHILD, lr.right - sb - 1, lr.top, sb + 1, h,
                                         ui.window, nullptr, GetModuleHandleW(nullptr), nullptr);
        SetWindowLongPtrW(ui.list_scroll, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&ui));
        ui.page[kTabControls].push_back(ui.list_scroll);
      }
      {
        const wchar_t *cols[3] = {L"Action", L"Key 1", L"Key 2"};
        const int widths[3] = {262, 160, 160};
        for (int c = 0; c < 3; ++c)
        {
          LVCOLUMNW col{};
          col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
          col.pszText = const_cast<wchar_t *>(cols[c]);
          col.cx = ui.px(widths[c]);
          col.iSubItem = c;
          SendMessageW(ui.list, LVM_INSERTCOLUMNW, WPARAM(c), reinterpret_cast<LPARAM>(&col));
        }
        for (int r = 0; r < kBindRowCount; ++r)
        {
          ui.binds.push_back(SplitTokens(rex::cvar::GetFlagByName(kBindRows[r].cvar)));
          LVITEMW it{};
          it.mask = LVIF_TEXT;
          it.iItem = r;
          it.pszText = const_cast<wchar_t *>(kBindRows[r].label);
          SendMessageW(ui.list, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&it));
        }
        RefreshBinds(ui);
        LVITEMW sel{};
        sel.stateMask = sel.state = LVIS_SELECTED | LVIS_FOCUSED;
        SendMessageW(ui.list, LVM_SETITEMSTATE, 0, reinterpret_cast<LPARAM>(&sel));
      }
      Button(ui, kTabControls, L"Change key 1", x, 238, 120, kBindChange1,
             L"Change this action's first key.");
      Button(ui, kTabControls, L"Change key 2", x + 128, 238, 120, kBindChange2,
             L"Change this action's second key.");
      Button(ui, kTabControls, L"Clear", x + 256, 238, 80, kBindClear, L"Removes both keys from this action.");
      Button(ui, kTabControls, L"Reset controls to defaults", x + 404, 238, 200, kBindReset,
             L"Puts all keys, mouse sensitivity, invert, step zoom and the Locktopus settings back to their "
             L"defaults.");
      // The warning line under the buttons (orange, WM_CTLCOLORSTATIC): Start without a key the menus leave alone.
      // Empty when there is nothing to say; UpdateStartWarning sets it with every change of the rows. Two lines.
      Add(ui, kTabControls, L"STATIC", L"", SS_LEFT, x, 274, 604, 40, kBindWarning);
      ui.tab_help[kTabControls] = list_help;

      // Locktopus (the lock-pick gadget): its own group under the keys, with a practice dial on the right.
      {
        const int gy = 318, lx = 150;
        HWND heading = Add(ui, kTabControls, L"STATIC", L"LOCKTOPUS", SS_LEFT, x, gy, 200, 22, kLockHeading);
        SendMessageW(heading, WM_SETFONT, reinterpret_cast<WPARAM>(ui.bold), TRUE);
        Add(ui, kTabControls, L"STATIC", L"Steer with", SS_LEFT, x, gy + 28, 116, 22, kLockInputsLabel);
        Segments(ui, kTabControls, lx, gy + 24, 72, kLockInputs, {L"Mouse", L"Keys", L"Both"},
                 LockSelOfInputs(pdz::minigame::PickInputsFromName(REXCVAR_GET(pdz_minigame_inputs))),
                 L"What steers the pick: the mouse, the keys, or both together. A controller always works.");
        const std::wstring sens_help =
            L"How far the pick moves for a mouse movement. High: a small movement moves it further.";
        Add(ui, kTabControls, L"STATIC", L"Mouse sensitivity", SS_LEFT, x, gy + 68, 120, 22, kLockSensLabel,
            sens_help);
        Add(ui, kTabControls, L"STATIC", L"Low", SS_LEFT, lx, gy + 68, 32, 22, kLockLow, sens_help);
        HWND sens = Add(ui, kTabControls, TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, lx + 32, gy + 62,
                        180, 30, kLockSens, sens_help);
        SendMessageW(sens, TBM_SETRANGE, FALSE, MAKELPARAM(0, kLockSensMax));
        SendMessageW(sens, TBM_SETPAGESIZE, 0, 2);
        SendMessageW(sens, TBM_SETPOS, TRUE, LockPosOfCounts(REXCVAR_GET(pdz_minigame_mouse_counts)));
        Add(ui, kTabControls, L"STATIC", L"High", SS_LEFT, lx + 216, gy + 68, 40, 22, kLockHigh, sens_help);
        HWND keys = Add(ui, kTabControls, L"STATIC", L"", SS_LEFT | SS_NOPREFIX, x, gy + 98, 376, 40, kLockKeys);
        SendMessageW(keys, WM_SETFONT, reinterpret_cast<WPARAM>(ui.tf.note), TRUE);
        static bool dial_class = false;
        if (!dial_class)
        {
          WNDCLASSW wc{};
          wc.lpfnWndProc = DialProc;
          wc.hInstance = GetModuleHandleW(nullptr);
          wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
          wc.lpszClassName = L"PDZRestoredLockDial";
          dial_class = RegisterClassW(&wc) != 0;
        }
        ui.dial.wnd = Add(ui, kTabControls, L"PDZRestoredLockDial", L"", WS_TABSTOP, 412, gy - 2, 220, 92, kLockDial,
                          L"Practice lock: point at it to steer with the mouse, or click it to keep the mouse there "
                          L"(click or Esc lets go). Click it first to use the keys.");
        SetTimer(ui.dial.wnd, 1, 15, nullptr);
        Combo(ui, kTabControls, 412, gy + 98, 132, kLockDifficulty, {L"Agent", L"Secret Agent", L"Perfect Agent"}, 1,
              L"The practice lock's difficulty: how close the pick has to be to a pin (Rare's distances 0.45, 0.33 and "
              L"0.20; Dark Agent is as Perfect Agent).");
        Button(ui, kTabControls, L"New lock", 548, gy + 96, 84, kLockNewLock,
               L"A new practice lock: the pins of one of Rare's own locks, picked at random.");
        ui.dial.rng.seed(GetTickCount());
        DialNewLock(ui);
      }
      UpdateStartWarning(ui);
      RefreshLockText(ui);

      // Advanced.
      Button(ui, kTabAdvanced, L"Open settings file", x, 52, 200, kOpenSettings,
             L"Opens the settings file in Notepad. It has extra settings this window doesn't show.");
      Button(ui, kTabAdvanced, L"Open logs folder", x + 220, 52, 200, kOpenLogs,
             L"Opens the logs folder. Send the newest log with any bug report.");
      Button(ui, kTabAdvanced, L"Rebuild shaders", x, 92, 200, kRebuild,
             L"Rebuilds the game's graphics from your game files. It takes a few seconds. Only needed if the picture "
             L"looks wrong after an update.");
      Button(ui, kTabAdvanced, L"Reset to defaults", x + 220, 92, 200, kReset,
             L"Puts everything in this window back to its default. Your saves and map packs aren't touched.");
      // Network play isn't offered: system link isn't there yet (the SDK's networking is stubbed). net_lan stays off
      // by default (no firewall question) and the window leaves it alone.
      Button(ui, kTabAdvanced, L"Check for updates", x, 140, 420, kUpdateCheck,
             L"When this window opens, asks GitHub whether a newer PDZ Restored is out and shows it here, with a "
             L"button to its download page. Nothing about you or your PC is sent, and nothing is ever downloaded.",
             BS_AUTOCHECKBOX);
      SetChecked(ui, kUpdateCheck, REXCVAR_GET(pdz_update_check));
      ui.tab_help[kTabAdvanced] = L"Point at a button to see what it does.";

      // Bottom: help line, don't show, buttons.
      // The help strip along the bottom of the content area (its info icon is painted), then the footer.
      ui.help = Add(ui, -1, L"STATIC", L"", SS_OWNERDRAW, kSideW + 60, kWindowH - kFooterH - kHelpH + 4,
                    kWindowW - kSideW - 60 - 24, kHelpH - 8, kHelp);
      SendMessageW(ui.help, WM_SETFONT, reinterpret_cast<WPARAM>(ui.tf.note), TRUE);
      Button(ui, -1, L"Don't show this again (hold Shift when starting to bring it back)", 24,
             kWindowH - kFooterH + 22, 500,
             kDontShow, L"", BS_AUTOCHECKBOX);
      HWND safe = Button(ui, -1, L"Safe mode", kWindowW - 24 - 150 - 12 - 130, kWindowH - kFooterH + 16, 130, kSafe,
                         L"Starts with a slower, simpler renderer at 60 fps. Try it if the game won't start or looks "
                         L"wrong.");
      HWND play = Button(ui, -1, L"Play", kWindowW - 24 - 150, kWindowH - kFooterH + 16, 150, kPlay,
                         L"Start the game.");
      for (HWND big : {safe, play})
      {
        RECT br;
        GetWindowRect(big, &br);
        MapWindowPoints(nullptr, ui.window, reinterpret_cast<POINT *>(&br), 2);
        MoveWindow(big, br.left, br.top, br.right - br.left, ui.px(42), FALSE);
      }
      HWND update = Button(ui, -1, L"Open download page", 12, 494, kSideW - 24, kUpdateOpen,
                           L"Opens the new version's page on GitHub in your browser. Download it from there.");
      RefreshPacks(ui, false);
      // After setup it opens on Map packs (their optional last step), unless the disc already has them.
      ShowTab(ui, packs_tab && !ctx.packs_on_disc ? kTabPacks : kTabDisplay);
      ShowWindow(update, SW_HIDE); // until the update check finds a newer release
      if (REXCVAR_GET(pdz_update_check))
      {
        update_check::Start(); // in the background; the window and the game never wait for it
      }
      SetTimer(ui.window, 1, 100, nullptr);
      ShowWindow(ui.window, SW_SHOW);
      SetForegroundWindow(ui.window);
      SetFocus(play);
      // pdz_settings_test_rebind: the rebinds through the capture path, then Play.
      const std::string rebind = REXCVAR_GET(pdz_settings_test_rebind);
      const bool capture_too = !REXCVAR_GET(pdz_settings_test_capture).empty();
      if (!rebind.empty())
      {
        ui.testing = true;
        ShowTab(ui, kTabControls);
        auto pump = [&](DWORD ms)
        {
          for (const DWORD until = GetTickCount() + ms; GetTickCount() < until && !ui.done;)
          {
            MSG m;
            while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE))
            {
              if (CaptureMessage(ui, m))
              {
                continue;
              }
              TranslateMessage(&m);
              DispatchMessageW(&m);
            }
            Sleep(5);
          }
        };
        std::stringstream items(rebind);
        std::string item;
        while (std::getline(items, item, ';'))
        {
          const size_t eq = item.find('=');
          if (eq == std::string::npos)
          {
            continue;
          }
          std::string cvar = item.substr(0, eq), input = item.substr(eq + 1);
          int slot = 0;
          if (cvar.size() > 2 && cvar.compare(cvar.size() - 2, 2, ":2") == 0)
          {
            slot = 1;
            cvar.resize(cvar.size() - 2);
          }
          if (cvar == "click")
          {
            // click=<control id>: a mouse click on that control (a segment's own id is 2000 + group x 8 + index).
            const int id = std::atoi(input.c_str());
            SendMessageW(GetDlgItem(ui.window, id), BM_CLICK, 0, 0);
            Note(fmt::format("pdz_settings: test: clicked control {}: tab now {}, Screen mode {}, Render scale {}", id,
                             ui.tab, Sel(ui, kMode), Sel(ui, kScale)));
            continue;
          }
          if (cvar == "lock_inputs" || cvar == "lock_sens")
          {
            // The Locktopus group: lock_inputs=mouse|keys|both clicks that segment, lock_sens=<0..20> moves the slider.
            if (cvar == "lock_inputs")
            {
              const int sel = input == "mouse" ? 0 : input == "keys" ? 1 : 2;
              SendMessageW(GetDlgItem(ui.window, 2000 + kLockInputs * 8 + sel), BM_CLICK, 0, 0);
            }
            else
            {
              SendMessageW(GetDlgItem(ui.window, kLockSens), TBM_SETPOS, TRUE, std::atoi(input.c_str()));
            }
            Note(fmt::format("pdz_settings: test: Locktopus: steer with {}, sensitivity position {} ({:.0f} counts)",
                             pdz::minigame::PickInputsName(LockInputsOfSel(Sel(ui, kLockInputs))),
                             int(SendMessageW(GetDlgItem(ui.window, kLockSens), TBM_GETPOS, 0, 0)),
                             LockCountsOfPos(int(SendMessageW(GetDlgItem(ui.window, kLockSens), TBM_GETPOS, 0, 0)))));
            continue;
          }
          if (cvar == "pdz_fov_degrees")
          {
            // The slider moved the way a player moves it (its position, then the scroll notification).
            HWND fov = GetDlgItem(ui.window, kFov);
            SendMessageW(fov, TBM_SETPOS, TRUE, std::atoi(input.c_str()));
            SendMessageW(ui.window, WM_HSCROLL, MAKEWPARAM(TB_THUMBPOSITION, 0), reinterpret_cast<LPARAM>(fov));
            wchar_t shown[64] = {};
            GetWindowTextW(GetDlgItem(ui.window, kFovValue), shown, 64);
            Note(fmt::format("pdz_settings: test: field of view slider {} -> \"{}\", pdz_fov_scale {:.3f}",
                             int(SendMessageW(fov, TBM_GETPOS, 0, 0)), std::string(shown, shown + wcslen(shown)),
                             FovScaleOfPosition(int(SendMessageW(fov, TBM_GETPOS, 0, 0)))));
            continue;
          }
          if (cvar == "pdz_zoom_steps")
          {
            // The Step zoom check box, clicked the way a player clicks it.
            const bool want = input == "true";
            if (Checked(ui, kZoomSteps) != want)
            {
              SendMessageW(GetDlgItem(ui.window, kZoomSteps), BM_CLICK, 0, 0);
            }
            Note(fmt::format("pdz_settings: test: Step zoom check box {}", Checked(ui, kZoomSteps) ? "on" : "off"));
            continue;
          }
          int row = -1;
          for (int r = 0; r < kBindRowCount; ++r)
          {
            row = cvar == kBindRows[r].cvar ? r : row;
          }
          if (row < 0)
          {
            Note("pdz_settings: test rebind: no row " + cvar);
            continue;
          }
          LVITEMW sel{};
          sel.stateMask = sel.state = LVIS_SELECTED | LVIS_FOCUSED;
          SendMessageW(ui.list, LVM_SETITEMSTATE, WPARAM(row), reinterpret_cast<LPARAM>(&sel));
          // Started the way the Change key buttons start it.
          SendMessageW(ui.window, WM_COMMAND, MAKEWPARAM(slot ? kBindChange2 : kBindChange1, BN_CLICKED), 0);
          wchar_t help[128] = {};
          GetWindowTextW(ui.help, help, 128);
          Note(fmt::format("pdz_settings: test rebind: {} key {}: waiting (help line: \"{}\"), input {}", cvar,
                           slot + 1, std::string(help, help + wcslen(help)), input));
          auto hex = [](const std::string &h) { return unsigned(std::strtoul(h.c_str(), nullptr, 16)); };
          if (input == "LButton" || input == "RButton" || input == "MButton")
          {
            PostMessageW(ui.window, input == "LButton" ? WM_LBUTTONDOWN : input == "RButton" ? WM_RBUTTONDOWN
                                                                                              : WM_MBUTTONDOWN,
                         0, 0);
          }
          else if (input == "XButton1" || input == "XButton2")
          {
            PostMessageW(ui.window, WM_XBUTTONDOWN, MAKEWPARAM(0, input == "XButton1" ? XBUTTON1 : XBUTTON2), 0);
          }
          else if (input == "WheelUp" || input == "WheelDown")
          {
            PostMessageW(ui.list, WM_MOUSEWHEEL, MAKEWPARAM(0, input == "WheelUp" ? WHEEL_DELTA : -WHEEL_DELTA), 0);
          }
          else if (input == "WheelLeft" || input == "WheelRight")
          {
            PostMessageW(ui.list, WM_MOUSEHWHEEL, MAKEWPARAM(0, input == "WheelRight" ? WHEEL_DELTA : -WHEEL_DELTA),
                         0);
          }
          else if (input == "Esc")
          {
            PostMessageW(ui.list, WM_KEYDOWN, VK_ESCAPE, 0x00010001);
          }
          else if (input.rfind("Key:", 0) == 0)
          {
            // Key:<vk>:<scan>[:ext] -> WM_KEYDOWN with that scan code in lParam (bit 24 = extended).
            std::stringstream parts(input.substr(4));
            std::string vk, scan, ext;
            std::getline(parts, vk, ':');
            std::getline(parts, scan, ':');
            std::getline(parts, ext, ':');
            const LPARAM lparam = LPARAM(1 | (hex(scan) & 0xFF) << 16 | (ext == "ext" ? 1u << 24 : 0u));
            PostMessageW(ui.list, WM_KEYDOWN, WPARAM(hex(vk)), lparam);
          }
          else if (input.rfind("Raw:", 0) == 0)
          {
            // What WM_INPUT hands on: the device's raw button state with button n held.
            const int n = std::atoi(input.c_str() + 4);
            if (n >= 6 && n <= 32)
            {
              RawButtons(ui, 1ul << (n - 1));
              RawButtons(ui, 0);
            }
          }
          pump(300);
          if (ui.cap_row >= 0)
          {
            Note("pdz_settings: test rebind: still waiting after 300 ms: cancelled");
            EndCapture(ui);
          }
        }
        auto narrow = [](const std::wstring &w)
        {
          std::string out;
          for (wchar_t c : w)
          {
            out += c < 128 ? char(c) : '?';
          }
          return out;
        };
        for (int r = 0; r < kBindRowCount; ++r)
        {
          Note(fmt::format("pdz_settings: test rebind: row {}: {} = \"{}\" (shown: {} | {})", r, kBindRows[r].cvar,
                           JoinTokens(ui.binds[size_t(r)]), narrow(SlotText(ui, r, 0)), narrow(SlotText(ui, r, 1))));
        }
        // With a capture folder too, the tabs are captured next (the Controls tab with these binds) and the window
        // closes without saving; else Play saves and starts.
        if (!capture_too)
        {
          PostMessageW(ui.window, WM_COMMAND, MAKEWPARAM(kPlay, BN_CLICKED), 0);
        }
      }
      // pdz_settings_test_dial: the practice dial played by its own test steering (logged), each run captured.
      const std::string dial_test = REXCVAR_GET(pdz_settings_test_dial);
      if (!dial_test.empty())
      {
        ui.testing = true;
        ShowTab(ui, kTabControls);
        auto pump = [&](DWORD ms)
        {
          for (const DWORD until = GetTickCount() + ms; GetTickCount() < until && !ui.done;)
          {
            MSG m;
            while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE))
            {
              TranslateMessage(&m);
              DispatchMessageW(&m);
            }
            Sleep(5);
          }
        };
        pump(300);
        std::stringstream runs(dial_test);
        std::string run;
        int n = 0;
        while (std::getline(runs, run, ';'))
        {
          DialTestRun(ui, run, ++n, REXCVAR_GET(pdz_settings_test_capture), pump);
        }
        DialNewLock(ui);
        ui.testing = !REXCVAR_GET(pdz_settings_test_rebind).empty();
      }
      // pdz_settings_test_capture: every tab to a .bmp, then close.
      const std::string capture = REXCVAR_GET(pdz_settings_test_capture);
      if (!capture.empty())
      {
        std::error_code ec;
        fs::create_directories(fs::u8path(capture), ec);
        CheckLayout(ui);
        for (int t = 0; t < kTabCount; ++t)
        {
          ShowTab(ui, t);
          UpdateWindow(ui.window);
          // PrintWindow's full content is the last frame DWM composed: let it compose this tab first.
          for (const DWORD until = GetTickCount() + 300; GetTickCount() < until;)
          {
            MSG pump;
            while (PeekMessageW(&pump, nullptr, 0, 0, PM_REMOVE))
            {
              TranslateMessage(&pump);
              DispatchMessageW(&pump);
            }
            Sleep(10);
          }
          RECT wr2;
          GetWindowRect(ui.window, &wr2);
          const int cw = wr2.right - wr2.left, ch = wr2.bottom - wr2.top;
          HDC sdc = GetDC(nullptr), mdc = CreateCompatibleDC(sdc);
          BITMAPINFO bi{};
          bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
          bi.bmiHeader.biWidth = cw;
          bi.bmiHeader.biHeight = ch;
          bi.bmiHeader.biPlanes = 1;
          bi.bmiHeader.biBitCount = 32;
          void *bits = nullptr;
          HBITMAP bmp = CreateDIBSection(mdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
          HGDIOBJ old = SelectObject(mdc, bmp);
          PrintWindow(ui.window, mdc, 2 /* PW_RENDERFULLCONTENT */);
          BITMAPFILEHEADER fh{};
          fh.bfType = 0x4D42;
          fh.bfOffBits = sizeof(fh) + sizeof(bi.bmiHeader);
          fh.bfSize = fh.bfOffBits + DWORD(cw) * DWORD(ch) * 4;
          std::ofstream out(fs::u8path(capture) / fmt::format("tab{}.bmp", t), std::ios::binary);
          out.write(reinterpret_cast<const char *>(&fh), sizeof(fh));
          out.write(reinterpret_cast<const char *>(&bi.bmiHeader), sizeof(bi.bmiHeader));
          out.write(static_cast<const char *>(bits), std::streamsize(size_t(cw) * size_t(ch) * 4));
          SelectObject(mdc, old);
          DeleteObject(bmp);
          DeleteDC(mdc);
          ReleaseDC(nullptr, sdc);
        }
        ui.save_on_close = false; // a capture test changes nothing
        PostMessageW(ui.window, WM_CLOSE, 0, 0);
      }
      MSG msg;
      while (!ui.done && GetMessageW(&msg, nullptr, 0, 0) > 0)
      {
        if (CaptureMessage(ui, msg))
        {
          continue;
        }
        // The key list: Enter changes the selected action's first key, Delete clears it (before the dialog
        // manager, which would press Play on Enter).
        if (msg.hwnd == ui.list && msg.message == WM_KEYDOWN && (msg.wParam == VK_RETURN || msg.wParam == VK_DELETE))
        {
          if (msg.wParam == VK_RETURN)
          {
            StartCapture(ui, std::max(0, SelectedRow(ui)), 0);
          }
          else
          {
            SendMessageW(ui.window, WM_COMMAND, MAKEWPARAM(kBindClear, BN_CLICKED), 0);
          }
          continue;
        }
        // Enter on a control that isn't a button (a list, the sensitivity box): Play, the default button.
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN && msg.hwnd != ui.list)
        {
          wchar_t cls[32] = {};
          GetClassNameW(msg.hwnd, cls, 32);
          if (_wcsicmp(cls, L"Button") == 0)
          {
            SendMessageW(msg.hwnd, BM_CLICK, 0, 0); // a focused button: Enter presses it
            continue;
          }
          if (_wcsicmp(cls, L"ComboBox") != 0)
          {
            SendMessageW(ui.window, WM_COMMAND, MAKEWPARAM(kPlay, BN_CLICKED), 0);
            continue;
          }
        }
        if (!IsDialogMessageW(ui.window, &msg))
        {
          TranslateMessage(&msg);
          DispatchMessageW(&msg);
        }
      }
      RawMouse(ui, false);
      if (!capture.empty())
      {
        std::ofstream lines(fs::u8path(capture) / "settings_test.txt", std::ios::binary | std::ios::trunc);
        lines << g_summary << "\n";
      }
      theme::FreeFonts(ui.tf);
      g_ui = nullptr;
      UnregisterClassW(wc.lpszClassName, wc.hInstance);
      return ui.choice;
    }
  } // namespace

  void LogSummary()
  {
    std::stringstream lines(g_summary);
    std::string line;
    while (std::getline(lines, line))
    {
      REXLOG_INFO("{}", line);
    }
    update_check::LoggingReady(); // the update check's line, now or when it comes
  }

  Choice MaybeShow(const Context &context, bool after_first_setup)
  {
    // pdz_map_packs_install: packages given on the command line, before anything is shown.
    const std::string install = REXCVAR_GET(pdz_map_packs_install);
    for (size_t at = 0; at < install.size();)
    {
      const size_t semi = install.find(';', at);
      const std::string one = install.substr(at, semi == std::string::npos ? std::string::npos : semi - at);
      at = semi == std::string::npos ? install.size() : semi + 1;
      if (one.empty())
      {
        continue;
      }
      if (context.packs_on_disc)
      {
        g_summary += fmt::format("{}pdz_map_packs: not installed: the US Platinum Hits disc already has both map "
                                 "packs ({})",
                                 g_summary.empty() ? "" : "\n", one);
        continue;
      }
      int index = -1;
      std::string log;
      const bool ok = map_packs::Install(fs::u8path(one), map_packs::TitleRoot(context.user_data_root), index, log);
      g_summary += fmt::format("{}pdz_map_packs: {}: {} ({})", g_summary.empty() ? "" : "\n",
                               index >= 0 ? map_packs::Packs()[size_t(index)].name : "?", log, one);
      (void)ok;
    }
    const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool show = REXCVAR_GET(pdz_settings_open) || shift || after_first_setup || REXCVAR_GET(pdz_settings_show);
    // A rebind test shows it even in a quiet test run (it presses Play itself).
    const bool test = !REXCVAR_GET(pdz_settings_test_rebind).empty();
    if ((!show || REXCVAR_GET(pdz_setup_quiet) || REXCVAR_GET(pdz_setup_restarted)) && !test ||
        pdz::native::DeveloperBuild())
    {
      return Choice::kPlay;
    }
    const Choice c = Run(context, after_first_setup);
    g_summary += fmt::format("{}pdz_settings: settings window closed with {}", g_summary.empty() ? "" : "\n",
                             c == Choice::kPlay ? "Play" : c == Choice::kSafeMode ? "Safe mode" : "the close box (quit)");
    return c;
  }
} // namespace pdz::settings_ui
