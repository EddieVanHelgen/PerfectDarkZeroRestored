// Unattended test harness for the native renderer (stage 1). Everything here is off unless its cvar is set.
//
//   --pdz_input_script=<file>  feeds a timed sequence of controller states into the game's own XInputGetState
//                              (sub_82AD4A18 -> XamInputGetState) by rewriting the state the game reads: user 0,
//                              and user 1 when the script has 'p2' lines (a second pad, see below). Nothing is
//                              injected into the OS, so no other window receives input.
//   --pdz_test_exit_s=<n>      after n seconds (wall clock from the first swap) logs one summary line and closes
//                              the window the normal way (WM_CLOSE -> "Window closing, shutting down").
//   --pdz_test_log_xui=true    logs, with the script time, every scene the frontend creates (sub_823A14F0 ->
//                              XuiSceneCreate), every keystroke the XUI input pump hands to XuiProcessInput
//                              (sub_8278DCA8), the MISSION SELECT cursor / choice (sub_8227AD78) and the game's
//                              message boxes (sub_82400EE0 / sub_82400DB0): routing checks. Local multiplayer joins
//                              (sub_82472858) are logged whenever an input script runs.
//
// Keystrokes: the menus read XInputGetKeystroke, so each scripted press also becomes a key-down / key-up pair, handed
// out in order to the next poll that accepts gamepad keystrokes (flags XINPUT_FLAG_GAMEPAD or ANYDEVICE, i.e. the
// frontend's input pump sub_823B4220 -> XuiProcessInput). The game's KeyboardUpdate task (sub_82274370) polls with
// XINPUT_FLAG_KEYBOARD only and gets none, as on a console without a keyboard: until 2026-10-26 it was fed too and,
// running on the game thread, ate the DOWN / A presses meant for MISSION SELECT and the MISSION BRIEFING (logs
// 085, 092-096, 107, 109: every lost press went to that thread). LT / RT presses also make VK_PAD_LTRIGGER /
// VK_PAD_RTRIGGER (0x5806 / 0x5807), as a real pad does: the front end's PLAYERS footer button is LT
// (DarkXuiSceneLoader, sub_823D35F0: LeftScene / LeftAction).
//
// Script format (one command per line, '#' comments, times in seconds from the first input poll):
//   press  <t> <buttons> [hold]              buttons held from t for hold seconds (default 0.15), plus keystrokes
//   repeat <from> <to> <period> <buttons> [hold]   a press every period seconds in [from, to)
//   pad    <t> <buttons> [hold]              like press, pad state only: no keystrokes (for game code that reads
//                                            the controller, e.g. a second pad's START in the multiplayer lobby; a
//                                            pad-only press never waits in the keystroke queue through a loading
//                                            screen)
//   stick  <t> <duration> <lx> <ly> [rx ry]  thumbsticks (-32768..32767) for duration seconds
//   p2 <any of the above>                    the same for a second pad, user index 1
//   p2 signin <t> [name]                     from t on, user 1 is a signed-in local profile (default "Player 2")
//   cvar   <t> <name> <value>                at t, sets a hot-reload cvar by name (rex::cvar::SetFlagByName), logged
//                                            'pdz_test: t=... cvar <name> = <value>': A/B of a setting at one view in
//                                            one run (shadows 2: bias / PCF / stable cascades side by side)
// Buttons: A B X Y START BACK LB RB LT RT UP DOWN LEFT RIGHT LS RS, joined with '+' (e.g. A+START).
//
// Second pad: with any 'p2' line, XInputGetState(1) reports a connected pad for the whole run (ERROR_SUCCESS, as if
// plugged in before boot) carrying the p2 events, and XInputGetCapabilities(1) a standard gamepad (the TU3 code has
// no caller of sub_82AD4A10; kept for completeness). The game reads all four pads every frame (sub_82723F40), so the
// lobby's join (sub_82472E98) and ready (SplitscreenHandlerButton, sub_823E78F8) see a second pad's START. Its
// keystrokes carry user index 1 and go to polls for user 1 or for any user (the XUI pump polls user 255); when both
// pads have a keystroke due, the earlier one goes first.
//
// p2 signin: a local multiplayer player needs a signed-in profile. sub_82472858 (the join) answers a pad without one
// with the sign-in message box 0x2043B instead of a participant, and the SDK signs in user 0 only. From t on the
// harness answers for user 1: XamUserGetSigninState = 1 (signed in locally; thunk sub_82AD9FF8), XamUserGetName =
// the name (thunk sub_82AD9FF0), and an offline XUID distinct from user 0's (sub_82ADA4A8, the game's
// XamUserGetXUID wrapper: an equal XUID would make the profile task drop player 1). With t > 0 it broadcasts
// XN_SYS_SIGNINCHANGED at t, so the profile task (sub_8250A340, scans the four users only on that notification)
// attaches a profile to pad 2; while a multiplayer lobby is up it also marks it to join (+9948), and the profile
// update (sub_8250AD20) then joins it (sub_82472858), as on a console when a second player signs in at the lobby.
// t = 0 signs user 1 in from boot (the SDK's start-up notifications trigger the scan); a START on pad 2 in the lobby
// then joins (sub_82472E98). Profile settings reads / writes for user 1 still fail in the SDK (NO_SUCH_USER).
//
// Saves: the harness does not touch the save path itself; scripted runs are launched with --user_data_root on a
// throwaway copy (PDZRenderResearch\native\testdata\userdata), so anything the game saves lands there. The SDK's
// content calls ignore the user index (user 0's XUID for everyone), so a signed-in second profile reads and writes
// user 0's save container: 'p2 signin' is honoured only when the user data root path contains "testdata".
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include <windows.h>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>

#include "shadow_record.h"

// Log 101 (split screen: player 2's pad connected, but he never became a participant): the join, its sign-in
// message boxes and the profile task's per-user attach also log with pdz_scenario_log (game_cvars.cpp), not only
// in scripted runs.
REXCVAR_DECLARE(bool, pdz_scenario_log);
#include "test_harness.h"
#include "../zoom_steps.h"
#include "latency_log.h"

REXCVAR_DEFINE_STRING(pdz_input_script, "", "Render",
                      "Test harness: a controller script fed into the game's XInputGetState for user 0, and user 1 "
                      "with 'p2' lines (see src\\mod\\render\\test_harness.cpp for the format). Empty = off.");
REXCVAR_DEFINE_INT32(pdz_test_exit_s, 0, "Render",
                     "Test harness: close the game cleanly this many seconds after the first swap, with a summary "
                     "line. 0 = off.");
REXCVAR_DEFINE_BOOL(pdz_test_log_xui, false, "Render",
                    "Test harness: log the frontend's scene creations (sub_823A14F0), the keystrokes the XUI receives "
                    "(sub_8278DCA8), the MISSION SELECT cursor / choice (sub_8227AD78) and the game's message boxes "
                    "(sub_82400EE0 / sub_82400DB0), with the script time ('pdz_test: t=..s scene / xui keystroke / "
                    "mission select / notification'). Off = no work beyond the cvar read.");

REX_EXTERN(__imp__sub_82AD4A18); // XInputGetState(user, state) -> XamInputGetState(user, 0, state)
REX_EXTERN(__imp__sub_82AD4A10); // XInputGetCapabilities(user, flags, caps) -> XamInputGetCapabilities (no TU3 caller)
REX_EXTERN(__imp__sub_82AD4A78); // XInputGetKeystroke(user, flags, keystroke) -> XamInputGetKeystrokeEx(&user, ...)
REX_EXTERN(__imp__sub_82AD4A58); // XInputGetKeystrokeEx(&user, flags, keystroke)
REX_EXTERN(__imp__sub_82AD9FF8); // XamUserGetSigninState(user) thunk
REX_EXTERN(__imp__sub_82AD9FF0); // XamUserGetName(user, buffer, length) thunk
REX_EXTERN(__imp__sub_82ADA4A8); // XamUserGetXUID(user, 7, &xuid) wrapper (0 or a Win32 error)
REX_EXTERN(__imp__sub_82472858); // local multiplayer join (controller, flag) -> participant or 0
REX_EXTERN(__imp__sub_82400EE0); // message box (user, caption string id, text, buttons)
REX_EXTERN(__imp__sub_82400DB0); // message box (user, caption string id, text string id, buttons)
REX_EXTERN(__imp__sub_8278DCA8); // XuiProcessInput(keystroke): the XUI input pump's delivery (from sub_823B4220)
REX_EXTERN(__imp__sub_823A14F0); // the game's scene creation (base path, UTF-16 scene path, init data, &scene)
REX_EXTERN(__imp__sub_8227AD78); // MISSION SELECT list index -> level-list index (cursor moves and the choice)

namespace pdz::test
{
  namespace
  {
    struct Event
    {
      double t0 = 0.0, t1 = 0.0;
      uint16_t buttons = 0;
      uint8_t lt = 0, rt = 0;
      bool sticks = false;
      bool keys = true; // press / repeat: also keystrokes; pad: pad state only
      int16_t lx = 0, ly = 0, rx = 0, ry = 0;
    };

    // Keystroke events derived from a pad's presses (menus such as "press start" read XInputGetKeystroke, not the
    // pad state): a key-down at each press start and a key-up at its end, per button, in time order.
    struct KeyEvent
    {
      double t = 0.0;
      uint16_t vk = 0;
      bool down = true;
    };

    // One scripted controller: index 0 = user 0 ('press ...'), 1 = user 1 ('p2 press ...').
    struct Pad
    {
      std::vector<Event> events;
      std::vector<KeyEvent> keys;
      size_t next_key = 0;        // advanced by the keystroke polls (the XUI pump's thread)
      uint32_t packet = 0;        // our own packet numbers (changes whenever the scripted state changes)
      uint64_t last_key = ~0ull;
      uint32_t presses_applied = 0;
      double last_logged_t = -1.0;
    };

    struct Script
    {
      bool failed = false;
      bool has_p2 = false; // any 'p2' line: user 1 is a connected pad
      Pad pads[2];
      uint64_t start_ms = 0;
      // 'p2 signin <t> [name]'
      bool signin = false, signin_allowed = false;
      double signin_t = 0.0;
      std::string signin_name = "Player 2";
      std::atomic<bool> signin_broadcast{false}, signin_reported{false};
      // 'cvar <t> <name> <value>', in time order; applied by user 0's input poll (the game thread).
      struct CvarSet
      {
        double t = 0.0;
        std::string name, value;
      };
      std::vector<CvarSet> cvars;
      size_t next_cvar = 0;
    };
    Script g_script;
    std::once_flag g_load_once;

    // Offline XUIDs start with 0xE. It must differ from user 0's (the SDK profile): when a profile signs in, the
    // profile task drops a participant with that profile's XUID on another pad, or on its pad with another XUID
    // (sub_8250A340, loc_8250ABBC), so an equal XUID would drop player 1.
    constexpr uint64_t kSecondUserXuid = 0xE000000000000002ull;

    uint16_t VirtualKeyFor(uint16_t bit)
    {
      switch (bit)
      {
      case 0x1000: return 0x5800; // VK_PAD_A
      case 0x2000: return 0x5801; // B
      case 0x4000: return 0x5802; // X
      case 0x8000: return 0x5803; // Y
      case 0x0200: return 0x5804; // RSHOULDER
      case 0x0100: return 0x5805; // LSHOULDER
      case 0x0001: return 0x5810; // DPAD_UP
      case 0x0002: return 0x5811; // DPAD_DOWN
      case 0x0004: return 0x5812; // DPAD_LEFT
      case 0x0008: return 0x5813; // DPAD_RIGHT
      case 0x0010: return 0x5814; // START
      case 0x0020: return 0x5815; // BACK
      case 0x0040: return 0x5816; // LTHUMB_PRESS
      case 0x0080: return 0x5817; // RTHUMB_PRESS
      default: return 0;
      }
    }

    void BuildKeyEvents(Pad &pad)
    {
      for (const Event &e : pad.events)
      {
        if (!e.keys)
        {
          continue;
        }
        auto add = [&](uint16_t vk) {
          pad.keys.push_back({e.t0, vk, true});
          pad.keys.push_back({e.t1, vk, false});
        };
        for (uint32_t b = 0; b < 16; ++b)
        {
          const uint16_t vk = VirtualKeyFor(uint16_t(1u << b));
          if ((e.buttons >> b) & 1 && vk)
          {
            add(vk);
          }
        }
        if (e.lt)
        {
          add(0x5806); // VK_PAD_LTRIGGER
        }
        if (e.rt)
        {
          add(0x5807); // VK_PAD_RTRIGGER
        }
      }
      std::stable_sort(pad.keys.begin(), pad.keys.end(),
                       [](const KeyEvent &a, const KeyEvent &b) { return a.t < b.t; });
    }

    bool ParseButtons(const std::string &text, uint16_t &buttons, uint8_t &lt, uint8_t &rt)
    {
      std::stringstream ss(text);
      std::string b;
      while (std::getline(ss, b, '+'))
      {
        if (b == "A") buttons |= 0x1000;
        else if (b == "B") buttons |= 0x2000;
        else if (b == "X") buttons |= 0x4000;
        else if (b == "Y") buttons |= 0x8000;
        else if (b == "START") buttons |= 0x0010;
        else if (b == "BACK") buttons |= 0x0020;
        else if (b == "LS") buttons |= 0x0040;
        else if (b == "RS") buttons |= 0x0080;
        else if (b == "LB") buttons |= 0x0100;
        else if (b == "RB") buttons |= 0x0200;
        else if (b == "UP") buttons |= 0x0001;
        else if (b == "DOWN") buttons |= 0x0002;
        else if (b == "LEFT") buttons |= 0x0004;
        else if (b == "RIGHT") buttons |= 0x0008;
        else if (b == "LT") lt = 255;
        else if (b == "RT") rt = 255;
        else if (b != "NONE") return false;
      }
      return true;
    }

    // 'p2 signin' gives user 1 user 0's save container (the SDK's content calls take user 0's XUID for everyone), so
    // it needs the throwaway user data root of the scripted runs (PDZRenderResearch\native\testdata\userdata).
    bool UserDataRootIsTestCopy(std::string &root)
    {
      auto *rt = rex::Runtime::instance();
      root = rt ? rt->user_data_root().string() : std::string();
      std::string lower = root;
      std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return char(std::tolower(c)); });
      return lower.find("testdata") != std::string::npos;
    }

    void Load()
    {
      Script &s = g_script;
      const std::string path = REXCVAR_GET(pdz_input_script);
      std::ifstream in(path);
      if (!in)
      {
        s.failed = true;
        REXLOG_ERROR("pdz_test: input script {} could not be opened", path);
        return;
      }
      s.pads[0].packet = 0x50445A00;
      s.pads[1].packet = 0x50445B00;
      std::string line;
      uint32_t n = 0;
      while (std::getline(in, line))
      {
        ++n;
        const size_t hash = line.find('#');
        if (hash != std::string::npos) line.resize(hash);
        std::stringstream ls(line);
        std::string cmd;
        if (!(ls >> cmd)) continue;
        uint32_t p = 0;
        if (cmd == "p2")
        {
          p = 1;
          s.has_p2 = true;
          cmd.clear();
          ls >> cmd;
        }
        Pad &pad = s.pads[p];
        Event e;
        bool ok = true;
        if (cmd == "press" || cmd == "pad")
        {
          std::string b;
          double hold = 0.15;
          ok = bool(ls >> e.t0 >> b);
          ls >> hold;
          e.t1 = e.t0 + hold;
          e.keys = cmd == "press";
          ok = ok && ParseButtons(b, e.buttons, e.lt, e.rt);
          if (ok) pad.events.push_back(e);
        }
        else if (cmd == "repeat")
        {
          double from = 0, to = 0, period = 0, hold = 0.15;
          std::string b;
          ok = bool(ls >> from >> to >> period >> b) && period > 0.05;
          ls >> hold;
          ok = ok && ParseButtons(b, e.buttons, e.lt, e.rt);
          for (double t = from; ok && t < to; t += period)
          {
            e.t0 = t;
            e.t1 = t + hold;
            pad.events.push_back(e);
          }
        }
        else if (cmd == "stick")
        {
          double dur = 0;
          int lx = 0, ly = 0, rx = 0, ry = 0;
          ok = bool(ls >> e.t0 >> dur >> lx >> ly);
          ls >> rx >> ry;
          e.t1 = e.t0 + dur;
          e.sticks = true;
          e.lx = int16_t(lx);
          e.ly = int16_t(ly);
          e.rx = int16_t(rx);
          e.ry = int16_t(ry);
          if (ok) pad.events.push_back(e);
        }
        else if (cmd == "cvar" && p == 0)
        {
          Script::CvarSet c;
          ok = bool(ls >> c.t >> c.name >> c.value);
          if (ok) s.cvars.push_back(c);
        }
        else if (cmd == "signin" && p == 1)
        {
          ok = bool(ls >> s.signin_t);
          std::string name;
          std::getline(ls, name);
          const size_t first = name.find_first_not_of(" \t\r");
          const size_t last = name.find_last_not_of(" \t\r");
          name = first == std::string::npos ? std::string() : name.substr(first, last - first + 1);
          if (!name.empty())
          {
            s.signin_name = name.substr(0, 15); // XamUserGetName: at most 15 characters and the terminator
          }
          s.signin = ok;
        }
        else
        {
          ok = false;
        }
        if (!ok)
        {
          REXLOG_ERROR("pdz_test: input script line {} not understood: {}", n, line);
        }
      }
      for (Pad &pad : s.pads)
      {
        BuildKeyEvents(pad);
      }
      std::stable_sort(s.cvars.begin(), s.cvars.end(),
                       [](const Script::CvarSet &a, const Script::CvarSet &b) { return a.t < b.t; });
      REXLOG_INFO("pdz_test: input script {} loaded ({} timed events, {} keystrokes){}", path, s.pads[0].events.size(),
                  s.pads[0].keys.size(),
                  s.has_p2 ? fmt::format("; pad 2 (user 1, connected): {} timed events, {} keystrokes",
                                         s.pads[1].events.size(), s.pads[1].keys.size())
                           : std::string());
      if (s.signin)
      {
        std::string root;
        s.signin_allowed = UserDataRootIsTestCopy(root);
        if (s.signin_allowed)
        {
          REXLOG_INFO("pdz_test: user 1 signs in as '{}' (offline XUID {:016X}) at t={:.1f}s; saves under {}",
                      s.signin_name, kSecondUserXuid, s.signin_t, root);
        }
        else
        {
          REXLOG_ERROR("pdz_test: 'p2 signin' ignored: the user data root {} is not a testdata copy (the SDK gives a "
                       "second profile user 0's save container)",
                       root);
        }
      }
    }

    void EnsureLoaded() { std::call_once(g_load_once, Load); }

    // Seconds since the script started: the first input poll (pad state or keystroke) of the run.
    double ScriptTime()
    {
      Script &s = g_script;
      const uint64_t now = GetTickCount64();
      if (!s.start_ms)
      {
        s.start_ms = now;
      }
      return double(now - s.start_ms) / 1000.0;
    }

    // 'p2 signin <t>' with t > 0: XN_SYS_SIGNINCHANGED once at t, so the profile task rescans the users.
    void MaybeBroadcastSignin(double t)
    {
      Script &s = g_script;
      if (!s.signin || !s.signin_allowed || s.signin_t <= 0.0 || t < s.signin_t ||
          s.signin_broadcast.load(std::memory_order_relaxed) || s.signin_broadcast.exchange(true))
      {
        return;
      }
      if (rex::system::KernelState *ks = REX_KERNEL_STATE())
      {
        ks->BroadcastNotification(0x0000000A, 0x2); // XN_SYS_SIGNINCHANGED, user 1's bit
      }
      REXLOG_INFO("pdz_test: t={:.1f}s user 1 signed in as '{}': XN_SYS_SIGNINCHANGED broadcast", t, s.signin_name);
    }

    inline uint16_t Load16BE(uint8_t *base, uint32_t address)
    {
      const uint8_t *p = pdz::shadow::GuestPtr(base, address);
      return uint16_t((uint16_t(p[0]) << 8) | p[1]);
    }
    inline uint8_t *Mutable(uint8_t *base, uint32_t address)
    {
      return const_cast<uint8_t *>(pdz::shadow::GuestPtr(base, address));
    }
    inline void Store16(uint8_t *base, uint32_t address, uint16_t v)
    {
      uint8_t *p = Mutable(base, address);
      p[0] = uint8_t(v >> 8);
      p[1] = uint8_t(v);
    }
    inline void Store32(uint8_t *base, uint32_t address, uint32_t v)
    {
      uint8_t *p = Mutable(base, address);
      p[0] = uint8_t(v >> 24);
      p[1] = uint8_t(v >> 16);
      p[2] = uint8_t(v >> 8);
      p[3] = uint8_t(v);
    }
    inline void Store64(uint8_t *base, uint32_t address, uint64_t v)
    {
      Store32(base, address, uint32_t(v >> 32));
      Store32(base, address + 4, uint32_t(v));
    }

    // User 1 is a signed-in local profile right now ('p2 signin': from its time on; with t > 0 once the broadcast
    // went out, so the profile task's rescan is what first sees it).
    bool SecondUserSignedIn()
    {
      if (!InputScriptActive())
      {
        return false;
      }
      EnsureLoaded();
      Script &s = g_script;
      if (s.failed || !s.signin || !s.signin_allowed)
      {
        return false;
      }
      if (s.signin_t > 0.0 && !s.signin_broadcast.load(std::memory_order_relaxed))
      {
        return false;
      }
      if (!s.signin_reported.exchange(true))
      {
        REXLOG_INFO("pdz_test: user 1 reported signed in locally as '{}'", s.signin_name);
      }
      return true;
    }
  } // namespace

  bool InputScriptActive() { return !REXCVAR_GET(pdz_input_script).empty(); }

  // Swap hook (shadow_hooks.cpp): the exit timer.
  void OnSwap()
  {
    const int32_t exit_s = REXCVAR_GET(pdz_test_exit_s);
    if (exit_s <= 0)
    {
      return;
    }
    static uint64_t start = 0;
    static bool closing = false;
    const uint64_t now = GetTickCount64();
    if (!start)
    {
      start = now;
    }
    if (closing || now - start < uint64_t(exit_s) * 1000)
    {
      return;
    }
    closing = true;
    LogSummary("test exit");
    // Close our own top-level windows the way the user does (the app's normal shutdown path).
    EnumWindows(
        [](HWND hwnd, LPARAM) -> BOOL {
          DWORD pid = 0;
          GetWindowThreadProcessId(hwnd, &pid);
          if (pid == GetCurrentProcessId() && IsWindowVisible(hwnd))
          {
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
          }
          return TRUE;
        },
        0);
  }

  uint32_t ScriptedPresses() { return g_script.pads[0].presses_applied + g_script.pads[1].presses_applied; }
} // namespace pdz::test

// XInputGetState(user r3, state r4): the original fills the state; for user 0, and for user 1 when the script has
// 'p2' lines, the script's buttons / triggers / sticks are merged in and the result forced to ERROR_SUCCESS (a script
// may run with no controller attached; pad 2 is connected for the whole run).
namespace
{
  // The scripted pad state over the one XInputGetState returned (r3 = its result).
  void ApplyPadScript(PPCContext &ctx, uint8_t *base, uint32_t user, uint32_t state)
  {
    if (user > 1 || !pdz::test::InputScriptActive() || !pdz::shadow::IsGuestPointer(state))
    {
      return;
    }
    using namespace pdz::test;
    EnsureLoaded();
    Script &s = g_script;
    if (s.failed || (user == 1 && !s.has_p2))
    {
      return;
    }
    const double t = ScriptTime();
    MaybeBroadcastSignin(t);
    if (user == 0)
    {
      while (s.next_cvar < s.cvars.size() && t >= s.cvars[s.next_cvar].t)
      {
        const Script::CvarSet &c = s.cvars[s.next_cvar++];
        rex::cvar::SetFlagByName(c.name, c.value);
        REXLOG_INFO("pdz_test: t={:.2f} cvar {} = {} (script; now {})", t, c.name, c.value,
                    rex::cvar::GetFlagByName(c.name));
      }
    }
    Pad &pad = s.pads[user];
    const bool connected = ctx.r3.u32 == 0;
    uint16_t buttons = connected ? Load16BE(base, state + 4) : 0;
    uint8_t lt = connected ? pdz::shadow::GuestPtr(base, state + 6)[0] : 0;
    uint8_t rt = connected ? pdz::shadow::GuestPtr(base, state + 7)[0] : 0;
    int16_t sticks[4] = {};
    for (uint32_t i = 0; i < 4; ++i)
    {
      sticks[i] = connected ? int16_t(Load16BE(base, state + 8 + 2 * i)) : 0;
    }
    uint16_t scripted = 0;
    for (const Event &e : pad.events)
    {
      if (t >= e.t0 && t < e.t1)
      {
        buttons |= e.buttons;
        scripted |= e.buttons;
        lt = std::max(lt, e.lt);
        rt = std::max(rt, e.rt);
        if (e.sticks)
        {
          sticks[0] = e.lx;
          sticks[1] = e.ly;
          sticks[2] = e.rx;
          sticks[3] = e.ry;
        }
      }
    }
    const uint64_t key = (uint64_t(buttons) << 48) ^ (uint64_t(lt) << 40) ^ (uint64_t(rt) << 32) ^
                         (uint64_t(uint16_t(sticks[0])) << 16) ^ uint64_t(uint16_t(sticks[1])) ^
                         (uint64_t(uint16_t(sticks[2])) << 24) ^ (uint64_t(uint16_t(sticks[3])) << 8);
    if (key != pad.last_key)
    {
      pad.last_key = key;
      ++pad.packet;
      if (scripted)
      {
        ++pad.presses_applied;
        if (t - pad.last_logged_t > 0.5)
        {
          pad.last_logged_t = t;
          REXLOG_INFO("pdz_test: t={:.1f}s scripted buttons {:04X}{}{}", t, scripted, user ? " pad 2" : "",
                      connected ? "" : " (no controller attached: state synthesised)");
        }
      }
    }
    Store32(base, state + 0, pad.packet);
    Store16(base, state + 4, buttons);
    Mutable(base, state + 6)[0] = lt;
    Mutable(base, state + 7)[0] = rt;
    for (uint32_t i = 0; i < 4; ++i)
    {
      Store16(base, state + 8 + 2 * i, uint16_t(sticks[i]));
    }
    ctx.r3.u64 = 0; // ERROR_SUCCESS
  }
} // namespace

REX_HOOK_RAW(sub_82AD4A18)
{
  const uint32_t user = ctx.r3.u32, state = ctx.r4.u32;
  __imp__sub_82AD4A18(ctx, base);
  ApplyPadScript(ctx, base, user, state);
  if (ctx.r3.u32 == 0 && pdz::shadow::IsGuestPointer(state))
  {
    pdz::zoom::OnPadButtons(user, pdz::test::Load16BE(base, state + 4)); // zoom_steps.cpp: R3 for step zoom
  }
  if (user == 0 && ctx.r3.u32 == 0 && pdz::shadow::IsGuestPointer(state))
  {
    pdz::latency::OnPadRead(base, state); // latency_log.h: input changes (pdz_latency_log)
  }
}

// XInputGetCapabilities(user r3, flags r4, caps r5): pad 2 is a standard gamepad when the script has 'p2' lines and
// no real pad answered. XINPUT_CAPABILITIES: Type, SubType, Flags (16), Gamepad (12), Vibration (4), big-endian.
REX_HOOK_RAW(sub_82AD4A10)
{
  const uint32_t user = ctx.r3.u32, caps = ctx.r5.u32;
  __imp__sub_82AD4A10(ctx, base);
  if (user != 1 || ctx.r3.u32 == 0 || !pdz::test::InputScriptActive() || !pdz::shadow::IsGuestPointer(caps))
  {
    return;
  }
  using namespace pdz::test;
  EnsureLoaded();
  if (g_script.failed || !g_script.has_p2)
  {
    return;
  }
  std::memset(Mutable(base, caps), 0, 20);
  Mutable(base, caps)[0] = 1; // XINPUT_DEVTYPE_GAMEPAD
  Mutable(base, caps)[1] = 1; // XINPUT_DEVSUBTYPE_GAMEPAD
  Store16(base, caps + 4, 0xFFFF); // every button
  Mutable(base, caps + 6)[0] = 0xFF;
  Mutable(base, caps + 7)[0] = 0xFF;
  for (uint32_t i = 0; i < 4; ++i)
  {
    Store16(base, caps + 8 + 2 * i, 0x7FFF);
  }
  Store16(base, caps + 16, 0xFFFF); // vibration motors
  Store16(base, caps + 18, 0xFFFF);
  ctx.r3.u64 = 0; // ERROR_SUCCESS
}

namespace
{
  // Shared body of the two keystroke hooks: when the real call returned no keystroke (ERROR_EMPTY or not
  // connected), hand the game the next due scripted keystroke of the pad the poll asks for (user 0, user 1, or 255 =
  // any pad: the earlier due of the two). Only a poll that accepts gamepad keystrokes gets one (XINPUT_FLAG_GAMEPAD 1,
  // XINPUT_FLAG_ANYDEVICE 0xFF, or 0): the KeyboardUpdate task's XINPUT_FLAG_KEYBOARD (2) polls get nothing, as with
  // a real pad and no keyboard. The keystroke carries the pad's user index, written back to *user_ptr for the Ex form.
  void InjectKeystroke(PPCContext &ctx, uint8_t *base, uint32_t user, uint32_t flags, uint32_t user_ptr,
                       uint32_t keystroke)
  {
    using namespace pdz::test;
    if (!InputScriptActive() || !pdz::shadow::IsGuestPointer(keystroke) || (user > 1 && user != 0xFF) ||
        (flags != 0 && (flags & 0x1) == 0))
    {
      return;
    }
    EnsureLoaded();
    Script &s = g_script;
    if (s.failed || ctx.r3.u32 == 0)
    {
      return; // a real keystroke was returned
    }
    const double t = ScriptTime();
    Pad *pick = nullptr;
    uint32_t pick_user = 0;
    for (uint32_t p = 0; p < 2; ++p)
    {
      Pad &pad = s.pads[p];
      if ((user != 0xFF && user != p) || pad.next_key >= pad.keys.size() || pad.keys[pad.next_key].t > t)
      {
        continue;
      }
      if (!pick || pad.keys[pad.next_key].t < pick->keys[pick->next_key].t)
      {
        pick = &pad;
        pick_user = p;
      }
    }
    if (!pick)
    {
      return; // nothing due (or nothing scripted is left)
    }
    const KeyEvent k = pick->keys[pick->next_key++];
    Store16(base, keystroke + 0, k.vk);
    Store16(base, keystroke + 2, 0);
    Store16(base, keystroke + 4, k.down ? 0x0001 : 0x0002); // XINPUT_KEYSTROKE_KEYDOWN / KEYUP
    Mutable(base, keystroke + 6)[0] = uint8_t(pick_user); // user index
    Mutable(base, keystroke + 7)[0] = 0;                  // HID code
    if (user_ptr)
    {
      Store32(base, user_ptr, pick_user);
    }
    if (k.down)
    {
      REXLOG_INFO("pdz_test: t={:.1f}s scripted keystroke {:04X} down{} (poll flags {:02X})", t, k.vk,
                  pick_user ? " pad 2" : "", flags);
    }
    ctx.r3.u64 = 0; // ERROR_SUCCESS
  }

  // Seconds since the script started (the first input poll), or since the first call when no script runs.
  double HarnessTime()
  {
    using namespace pdz::test;
    static uint64_t first = 0;
    const uint64_t now = GetTickCount64();
    if (g_script.start_ms)
    {
      return double(now - g_script.start_ms) / 1000.0;
    }
    if (!first)
    {
      first = now;
    }
    return double(now - first) / 1000.0;
  }

  // A guest UTF-16 (big-endian) string as ASCII, for the log.
  std::string ReadGuestWide(uint8_t *base, uint32_t address, size_t max = 160)
  {
    using namespace pdz::test;
    std::string text;
    if (!pdz::shadow::IsGuestPointer(address))
    {
      return text;
    }
    for (size_t i = 0; i < max; ++i)
    {
      const uint16_t c = Load16BE(base, address + uint32_t(2 * i));
      if (!c)
      {
        break;
      }
      text += (c >= 0x20 && c < 0x7F) ? char(c) : '?';
    }
    return text;
  }
} // namespace

// XInputGetKeystroke(user r3, flags r4, keystroke r5).
REX_HOOK_RAW(sub_82AD4A78)
{
  const uint32_t user = ctx.r3.u32, flags = ctx.r4.u32, keystroke = ctx.r5.u32;
  __imp__sub_82AD4A78(ctx, base);
  InjectKeystroke(ctx, base, user, flags, 0, keystroke);
}

// XInputGetKeystrokeEx(user pointer r3, flags r4, keystroke r5).
REX_HOOK_RAW(sub_82AD4A58)
{
  const uint32_t user_ptr = ctx.r3.u32, flags = ctx.r4.u32, keystroke = ctx.r5.u32;
  const uint32_t user = pdz::shadow::IsGuestPointer(user_ptr) ? pdz::shadow::Load32(base, user_ptr) : 0xFFu;
  __imp__sub_82AD4A58(ctx, base);
  InjectKeystroke(ctx, base, user, flags, pdz::shadow::IsGuestPointer(user_ptr) ? user_ptr : 0, keystroke);
}

// 'p2 signin' (see the top of the file): user 1 as a signed-in local profile. The three are called by the profile
// task (sub_8250A340 -> sub_82509EB0) on XN_SYS_SIGNINCHANGED and by the network library's local-user scan; nothing
// changes for user 0 or without the script line.

// XamUserGetSigninState(user r3) thunk: 0 not signed in, 1 signed in locally, 2 signed in to Live.
REX_HOOK_RAW(sub_82AD9FF8)
{
  const uint32_t user = ctx.r3.u32;
  __imp__sub_82AD9FF8(ctx, base);
  if (user == 1 && ctx.r3.u32 == 0 && pdz::test::SecondUserSignedIn())
  {
    ctx.r3.u64 = 1; // eXUserSigninState_SignedInLocally
  }
}

// XamUserGetName(user r3, buffer r4, length r5) thunk: ASCII, at most 16 bytes with the terminator (as the SDK).
REX_HOOK_RAW(sub_82AD9FF0)
{
  const uint32_t user = ctx.r3.u32, buffer = ctx.r4.u32, length = ctx.r5.u32;
  __imp__sub_82AD9FF0(ctx, base);
  if (user != 1 || !length || !pdz::shadow::IsGuestPointer(buffer) || !pdz::test::SecondUserSignedIn())
  {
    return;
  }
  using namespace pdz::test;
  const std::string &name = g_script.signin_name;
  const size_t count = std::min<size_t>(name.size(), std::min<uint32_t>(length, 16u) - 1);
  for (size_t i = 0; i < count; ++i)
  {
    Mutable(base, buffer + uint32_t(i))[0] = uint8_t(name[i]);
  }
  Mutable(base, buffer + uint32_t(count))[0] = 0;
  ctx.r3.u64 = 0; // ERROR_SUCCESS
}

// sub_82ADA4A8(user r3, &xuid r4): XamUserGetXUID(user, 7, &xuid), 0 or a Win32 error (callers: sub_82509EB0,
// sub_8250A178 and the network library).
REX_HOOK_RAW(sub_82ADA4A8)
{
  const uint32_t user = ctx.r3.u32, out = ctx.r4.u32;
  __imp__sub_82ADA4A8(ctx, base);
  if (user != 1 || !pdz::shadow::IsGuestPointer(out) || !pdz::test::SecondUserSignedIn())
  {
    return;
  }
  using namespace pdz::test;
  Store64(base, out, kSecondUserXuid);
  ctx.r3.u64 = 0;
}

// sub_82472858(controller r3, flag r4): a local multiplayer join. Called on START from a connected pad without a
// participant while a session is up (sub_82472E98, game task sub_82455488) and for a profile that signed in at the
// lobby (sub_8250AD20). Needs game mode 2 and a signed-in profile on that pad: without one it opens the sign-in
// message box 0x2043B (buttons: XamShowSigninUI) and returns 0. Returns the new participant.
REX_HOOK_RAW(sub_82472858)
{
  const uint32_t controller = ctx.r3.u32;
  __imp__sub_82472858(ctx, base);
  if (pdz::test::InputScriptActive() || REXCVAR_GET(pdz_test_log_xui) || REXCVAR_GET(pdz_scenario_log))
  {
    REXLOG_INFO("pdz_test: t={:.1f}s local join controller {} -> participant {:08X}{}", HarnessTime(), controller,
                ctx.r3.u32, ctx.r3.u32 ? "" : " (refused: no signed-in profile on that pad, or not in a session)");
  }
}

// sub_82509EB0(user r3): the profile task attaches a signed-in user's profile to its pad (from the four-user scan
// sub_8250A340 on XN_SYS_SIGNINCHANGED); true when the profile object is new / changed. Log 101.
REX_EXTERN(__imp__sub_82509EB0);
REX_HOOK_RAW(sub_82509EB0)
{
  const uint32_t user = ctx.r3.u32;
  __imp__sub_82509EB0(ctx, base);
  if (REXCVAR_GET(pdz_scenario_log) || REXCVAR_GET(pdz_test_log_xui))
  {
    REXLOG_INFO("pdz_split: profile task: user {} signed in, profile attached to its pad (result {})", user,
                ctx.r3.u32 & 0xFF);
  }
}

// Routing checks (--pdz_test_log_xui): what the frontend really received and showed, on the script's clock.

// sub_82400EE0(user r3, caption string id r4, text r5, buttons r6): a message box for one user (or 255).
REX_HOOK_RAW(sub_82400EE0)
{
  const uint32_t user = ctx.r3.u32, caption = ctx.r4.u32, buttons = ctx.r6.u32;
  __imp__sub_82400EE0(ctx, base);
  if (REXCVAR_GET(pdz_test_log_xui) || (REXCVAR_GET(pdz_scenario_log) && user >= 1 && user < 4))
  {
    REXLOG_INFO("pdz_test: t={:.1f}s notification for user {}: caption {:05X} buttons {} -> {:08X}", HarnessTime(),
                user, caption, buttons, ctx.r3.u32);
  }
}

// sub_82400DB0(user r3, caption string id r4, text string id r5, buttons r6): the same with a text string id.
REX_HOOK_RAW(sub_82400DB0)
{
  const uint32_t user = ctx.r3.u32, caption = ctx.r4.u32, text = ctx.r5.u32, buttons = ctx.r6.u32;
  __imp__sub_82400DB0(ctx, base);
  if (REXCVAR_GET(pdz_test_log_xui))
  {
    REXLOG_INFO("pdz_test: t={:.1f}s notification for user {}: caption {:05X} text {:05X} buttons {} -> {:08X}",
                HarnessTime(), user, caption, text, buttons, ctx.r3.u32);
  }
}

// XuiProcessInput(keystroke r3): the keystroke the XUI input pump (sub_823B4220, flags XINPUT_FLAG_ANYDEVICE) hands
// to the scenes, scripted or real; key-down becomes XM_KEYDOWN (36), key-up XM_KEYUP (37).
REX_HOOK_RAW(sub_8278DCA8)
{
  // Log 104 (split screen: player 2's face buttons never reached the menus): with pdz_scenario_log, the XUI
  // keystrokes of users 1-3 (once per key and user a second at most), to see them arrive.
  if (REXCVAR_GET(pdz_scenario_log) && !REXCVAR_GET(pdz_test_log_xui) && pdz::shadow::IsGuestPointer(ctx.r3.u32))
  {
    const uint32_t ks = ctx.r3.u32;
    const uint8_t user = pdz::shadow::Load8(base, ks + 6);
    const uint16_t vk = pdz::test::Load16BE(base, ks), flags = pdz::test::Load16BE(base, ks + 4);
    static std::chrono::steady_clock::time_point last[4][64]{};
    const auto now = std::chrono::steady_clock::now();
    if (user >= 1 && user < 4 && (flags & 0x1) && now - last[user][vk & 63] >= std::chrono::seconds(1))
    {
      last[user][vk & 63] = now;
      REXLOG_INFO("pdz_split: XUI keystroke {:04X} down from user {} (log 104)", vk, user);
    }
  }
  if (REXCVAR_GET(pdz_test_log_xui) && pdz::shadow::IsGuestPointer(ctx.r3.u32))
  {
    using namespace pdz::test;
    const uint32_t ks = ctx.r3.u32;
    const uint16_t vk = Load16BE(base, ks), flags = Load16BE(base, ks + 4);
    REXLOG_INFO("pdz_test: t={:.1f}s xui keystroke {:04X} {} user {}", HarnessTime(), vk,
                (flags & 0x1) ? "down" : (flags & 0x2) ? "up" : "repeat", pdz::shadow::Load8(base, ks + 6));
  }
  __imp__sub_8278DCA8(ctx, base);
}

// sub_823A14F0(base path r3, scene path r4, init data r5, &scene r6): every frontend scene creation goes through
// this wrapper of XuiSceneCreate (35 callers; mp_music.cpp already hooks XuiSceneCreate itself).
REX_HOOK_RAW(sub_823A14F0)
{
  const bool log = REXCVAR_GET(pdz_test_log_xui);
  const std::string path = log ? ReadGuestWide(base, ctx.r4.u32) : std::string();
  const uint32_t out = ctx.r6.u32;
  __imp__sub_823A14F0(ctx, base);
  if (log)
  {
    const uint32_t scene = pdz::shadow::IsGuestPointer(out) ? pdz::shadow::Load32(base, out) : 0;
    REXLOG_INFO("pdz_test: t={:.1f}s scene {} -> {:08X} (hr {:08X})", HarnessTime(), path, scene, ctx.r3.u32);
  }
}

// sub_8227AD78(list index r3): the level-list index of the n-th campaign level. MISSION SELECT's list (class
// MissionList, handler sub_82402318) calls it when the cursor moves (XN notification 7 -> sub_824026F8, return
// 8240272C) and when an entry is chosen (notification 2 -> sub_82402790, return 8240280C).
REX_HOOK_RAW(sub_8227AD78)
{
  const uint32_t index = ctx.r3.u32, lr = uint32_t(ctx.lr);
  __imp__sub_8227AD78(ctx, base);
  if (REXCVAR_GET(pdz_test_log_xui) && (lr == 0x8240272Cu || lr == 0x8240280Cu))
  {
    REXLOG_INFO("pdz_test: t={:.1f}s mission select {} index {} -> level {}", HarnessTime(),
                lr == 0x8240272Cu ? "cursor" : "chosen", index, int32_t(ctx.r3.u32));
  }
}
