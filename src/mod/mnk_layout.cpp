// mnk_layout.cpp - the keyboard and mouse follow the player's intent whatever in-game controller layout is chosen
// (pdz_mnk_follow_layout, on by default). Title Update 3 addresses. See mnk_layout.h and docs/MOUSE_KEYBOARD.md
// ("Controller layouts").
//
// Rare's controller object (read 2026-11-08): one per local player, at participant +1680 (player +6560 -> participant;
// so the "input block" fields +1680.. of the other mods are this object's +0..). Updated once per frame by
// sub_822748C8(controller r3, player r4, -, aim allowed r6), called from sub_82472E98 (r3 = participant + 1680).
//   +0  pad index (0-3; XInputGetState user)          +4  Controller Style: 0 = CLASSIC PD, 1 = STANDARD
//   +8  context: 0 on foot (set through player vtable +596, sub_825AC318), 2 hovercraft, 4 jetpack, 6 turret, 3 / 5
//       (not seen set; sub_82275278 / sub_82275328), 1 (jetpack exit path)
//   +16 byte: 0 = Look Inversion on (the look y is negated: 82274BB4)   +24 byte: 1 = sticks SWAPPED (82274A64)
//   +28 / +32 move x (strafe) / y (walk)   +36 / +40 look x (turn) / y (pitch): the outputs the player code reads
//   +60..+72 the two sticks as read (pad vtable +44 / +48: the pad object's +104 / +112), swapped first when +24
//   +76 the aim trigger (LT) while aiming may swap the sticks (Classic PD, sub_82275BD8), +100 holds it
// The profile copy (sub_82274790, profile sub_82508B40(user)): +4 = profile +8808, +16 = bit 29 of +8816, +24 = bit
// 31 of +8816. Rare's front end lists the values as "CLASSIC PD | STANDARD", "NORMAL | SWAPPED" (frontend .str).
//
// The stick -> move / look mapping (822748C8: mode 0 sub_82274CE0, mode 1 sub_82274E40, then per context):
//   Standard (any context but 5):      move = left stick,                  look = right stick
//   Classic PD, on foot, not aiming:   move = (right x, left y),           look = (left x, right y)
//   Classic PD, on foot, aiming (LT):  move = right stick,                 look = left stick
//   Classic PD, contexts 2 / 4 / 6:    move = (right x, left y),           look = (left x, right y)
//   Classic PD context 3, any style 5: move = right stick,                 look = left stick
//   Sticks swapped: the same with left and right exchanged (the swap comes first).
//
// pdz_mnk_follow_layout: the keyboard's stick binds mean, as their names say, strafe (left stick x), walk (left stick
// y), turn (right stick x) and look up / down (right stick y). In the game's XInputGetState (user 0, the SDK's
// keyboard device is merged there) each stick axis the keyboard drives (all its keys held this poll, by the binds,
// as the SDK computes them, and the merged value equal to the keyboard's: the keyboard won the SDK's merge) is moved
// to the axis that does that job in the active layout. A controller's own sticks are never moved; on an axis where
// keys are held too, the controller's weaker deflection is lost while the keys are held.
#include "mnk_layout.h"
#include "minigame_input.h" // the Locktopus on the keyboard / mouse: told after each controller update of pad 0

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/input/mnk/mnk_input_driver.h>
#include <rex/logging.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/virtual_key.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>

REXCVAR_DEFINE_BOOL(pdz_mnk_follow_layout, true, "Mouse",
                    "Keyboard and mouse follow the in-game controller layout: the movement keys (WASD) always walk and "
                    "strafe, the look keys always turn and look up / down, and the mouse steering the CamSpy / a "
                    "turret / a vehicle always turns, whatever Controller Style (Classic PD / Standard), Sticks "
                    "(Normal / Swapped) or aiming in Classic PD does to the sticks; the game's Look Inversion doesn't "
                    "flip the mouse (pdz_mouse_invert does). A controller is not changed. Off = the keys are plain "
                    "stick directions, which the layout reinterprets (in Classic PD A / D turn)");
REXCVAR_DEFINE_BOOL(pdz_mnk_layout_log, false, "Mouse",
                    "Diagnostic: log the layout of pad 0 (style, context, sticks, inversion, aiming) at each change and, "
                    "twice a second while a keyboard stick key is held, which stick axis each key drove and the game's "
                    "resulting move / look");
REXCVAR_DEFINE_STRING(pdz_mnk_test_layout, "", "Testing",
                      "TESTING TOOL: force pad 0's controller layout before each update; '' (or any value without "
                      "classic / standard, e.g. 'profile') = the profile's. Words joined by '_': classic or standard, "
                      "optional swapped, optional inverted (e.g. classic_swapped)");
REXCVAR_DEFINE_STRING(pdz_mnk_test_keys, "", "Testing",
                      "TESTING TOOL: keyboard keys held as if pressed, bind tokens joined by ',' (e.g. 'D' or 'W,D'); "
                      "'' or '-' = none. The keyboard driver's stick from them is merged into pad 0's state as the SDK "
                      "does (simulated: no OS or SDL event), then pdz_mnk_follow_layout applies. Logged");

REX_EXTERN(__imp__sub_822748C8);

namespace
{
  using Clock = std::chrono::steady_clock;

  uint32_t LoadBe32(const uint8_t *base, uint32_t address)
  {
    const uint8_t *p = base + address;
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
  }
  float LoadBeFloat(const uint8_t *base, uint32_t address)
  {
    const uint32_t bits = LoadBe32(base, address);
    float v;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
  }
  void StoreBe32(uint8_t *base, uint32_t address, uint32_t v)
  {
    uint8_t *p = base + address;
    p[0] = uint8_t(v >> 24);
    p[1] = uint8_t(v >> 16);
    p[2] = uint8_t(v >> 8);
    p[3] = uint8_t(v);
  }
  int16_t LoadBe16s(const uint8_t *base, uint32_t address)
  {
    const uint8_t *p = base + address;
    return int16_t(uint16_t((uint16_t(p[0]) << 8) | p[1]));
  }
  void StoreBe16(uint8_t *base, uint32_t address, int16_t v)
  {
    uint8_t *p = base + address;
    p[0] = uint8_t(uint16_t(v) >> 8);
    p[1] = uint8_t(uint16_t(v));
  }
  bool Guest(uint32_t address) { return address >= 0x10000u && address < 0xF0000000u; }

  // Controller object fields (participant +1680).
  constexpr uint32_t kCtrlPad = 0, kCtrlStyle = 4, kCtrlContext = 8, kCtrlNotInverted = 16, kCtrlSwapped = 24;
  constexpr uint32_t kCtrlMoveX = 28, kCtrlLookX = 36, kCtrlAimTrigger = 76, kCtrlAimHold = 100;
  // XINPUT_STATE: +6 LT, +8 lx, ly, rx, ry (big-endian).
  constexpr uint32_t kStateLeftTrigger = 6, kStateSticks = 8;

  // Stick axes, in XINPUT order; a key bind's intent has the same number (left stick x = strafe, ...).
  constexpr const char *kAxisName[4] = {"left stick x", "left stick y", "right stick x", "right stick y"};
  constexpr const char *kIntentName[4] = {"strafe", "walk", "turn", "look up / down"};
  constexpr const char *kBindCvar[8] = {"keybind_lstick_up", "keybind_lstick_down", "keybind_lstick_left",
                                        "keybind_lstick_right", "keybind_rstick_up", "keybind_rstick_down",
                                        "keybind_rstick_left", "keybind_rstick_right"};

  int64_t NowMs()
  {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
  }

  int16_t LargerMagnitude(int16_t a, int16_t b) { return std::abs(int(a)) >= std::abs(int(b)) ? a : b; }

  // ---- The layout of pad 0, as the last controller update saw it --------------------------------------------------

  struct Layout
  {
    bool valid = false;
    int32_t style = 0, context = 0;
    bool swapped = false, inverted = false;
    bool aim_allowed = false; // sub_822748C8's r6 (Classic PD on foot may swap the sticks while aiming)
    bool aim_hold = false;    // +100: +76 kept as it is
    float aim_trigger = 0.0f; // +76
    float move[2] = {}, look[2] = {};
    int64_t at_ms = 0;
  };
  std::mutex g_layout_mu;
  Layout g_layout;

  Layout CurrentLayout()
  {
    std::lock_guard lock(g_layout_mu);
    Layout l = g_layout;
    if (l.valid && NowMs() - l.at_ms > 300)
    {
      l.valid = false; // no on-foot / vehicle update lately (menus, loading)
    }
    return l;
  }

  // For each intent (strafe, walk, turn, look up / down), the stick axis that does it; false = an unknown layout.
  bool AxisForIntent(const Layout &l, bool aiming, std::array<int, 4> &m)
  {
    constexpr std::array<int, 4> kSame{0, 1, 2, 3}, kCross{2, 1, 0, 3}, kSwap{2, 3, 0, 1};
    if (l.style == 1)
    {
      m = l.context == 5 ? kSwap : kSame;
    }
    else if (l.style == 0)
    {
      switch (l.context)
      {
      case 0:
      case 1:
        m = aiming ? kSwap : kCross;
        break;
      case 2:
      case 4:
      case 6:
        m = kCross;
        break;
      case 3:
      case 5:
        m = kSwap;
        break;
      default:
        return false;
      }
    }
    else
    {
      return false;
    }
    if (l.swapped)
    {
      for (int &a : m)
      {
        a ^= 2; // the sticks are swapped before the mapping
      }
    }
    return true;
  }

  // Classic PD on foot swaps the sticks while the aim trigger is held (82274CE0: controller +76 > 0, set from the
  // pad's LT by sub_82275BD8 when the update's r6 allows it and +100 doesn't hold it).
  bool Aiming(const Layout &l, uint8_t left_trigger)
  {
    if (l.style != 0 || (l.context != 0 && l.context != 1))
    {
      return false;
    }
    return l.aim_hold ? l.aim_trigger > 0.0f : (l.aim_allowed && left_trigger > 0);
  }

  // Whether the update that took this snapshot used the aiming swap (82274CE0: Classic PD on foot, +76 > 0).
  bool AimedAtUpdate(const Layout &l)
  {
    return l.style == 0 && (l.context == 0 || l.context == 1) && l.aim_trigger > 0.0f;
  }

  std::string LayoutText(const Layout &l, bool aiming)
  {
    return fmt::format("{} (style {}), context {}, sticks {}, look inversion {}, {}",
                       l.style == 0 ? "CLASSIC PD" : l.style == 1 ? "STANDARD" : "unknown style", l.style, l.context,
                       l.swapped ? "SWAPPED" : "NORMAL", l.inverted ? "on" : "off",
                       aiming ? "aiming (LT)" : "not aiming");
  }

  // ---- Which keyboard keys are held ---------------------------------------------------------------------------------

  // The SDK keyboard driver's bind matching (rexglue-sdk mnk_input_driver.cpp IsBindPressed), so the keyboard's stick
  // is computed exactly as the driver computes it.
  constexpr uint8_t kModShift = 1, kModCtrl = 2, kModAlt = 4;
  struct BindAlternative
  {
    uint8_t mods = 0;
    uint16_t code = 0;
  };
  using Bind = std::vector<BindAlternative>;

  std::string_view TrimSpaces(std::string_view s)
  {
    while (!s.empty() && s.front() == ' ')
    {
      s.remove_prefix(1);
    }
    while (!s.empty() && s.back() == ' ')
    {
      s.remove_suffix(1);
    }
    return s;
  }

  Bind ParseBind(std::string_view text)
  {
    Bind bind;
    while (!text.empty())
    {
      const size_t comma = text.find(',');
      std::string_view token = TrimSpaces(text.substr(0, comma));
      text = comma == std::string_view::npos ? std::string_view() : text.substr(comma + 1);
      BindAlternative alt;
      for (;;)
      {
        const size_t plus = token.find('+');
        if (plus == std::string_view::npos || plus == 0)
        {
          break;
        }
        const std::string_view head = token.substr(0, plus);
        const uint8_t bit = head == "Shift" ? kModShift : (head == "Ctrl" || head == "Control") ? kModCtrl
                                                      : head == "Alt"                           ? kModAlt
                                                                                                : 0;
        if (!bit)
        {
          break;
        }
        alt.mods |= bit;
        token.remove_prefix(plus + 1);
      }
      if (!token.empty())
      {
        alt.code = rex::ui::ParseInputCode(token);
        if (alt.code != 0 && alt.code < rex::ui::kInputCodeCount)
        {
          bind.push_back(alt);
        }
      }
    }
    return bind;
  }

  class KeyTracker final : public rex::ui::WindowInputListener, public rex::ui::WindowListener
  {
  public:
    void OnKeyDown(rex::ui::KeyEvent &e) override
    {
      if (Focused())
      {
        SetKey(e, true);
      }
    }
    void OnKeyUp(rex::ui::KeyEvent &e) override { SetKey(e, false); }
    void OnMouseDown(rex::ui::MouseEvent &e) override
    {
      if (Focused())
      {
        SetCode(rex::ui::InputCodeForMouseButton(static_cast<int>(e.button())), true);
      }
    }
    void OnMouseUp(rex::ui::MouseEvent &e) override
    {
      SetCode(rex::ui::InputCodeForMouseButton(static_cast<int>(e.button())), false);
    }
    void OnLostFocus(rex::ui::UISetupEvent &) override
    {
      has_focus_.store(false, std::memory_order_relaxed);
      std::lock_guard lock(mu_);
      std::memset(down_, 0, sizeof(down_)); // releases landing on another window never arrive (as the driver does)
    }
    void OnGotFocus(rex::ui::UISetupEvent &) override { has_focus_.store(true, std::memory_order_relaxed); }

    // The keyboard driver's stick (lx, ly, rx, ry) from the held keys, plus the test keys. Also the tokens held.
    void KeyboardStick(const std::vector<uint16_t> &extra, int16_t out[4])
    {
      RefreshBinds();
      std::lock_guard lock(mu_);
      bool saved[16] = {};
      for (size_t i = 0; i < extra.size() && i < 16; ++i)
      {
        saved[i] = down_[extra[i]];
        down_[extra[i]] = true;
      }
      const uint8_t live = uint8_t((down_[uint16_t(rex::ui::VirtualKey::kShift)] ? kModShift : 0) |
                                   (down_[uint16_t(rex::ui::VirtualKey::kControl)] ? kModCtrl : 0) |
                                   (down_[uint16_t(rex::ui::VirtualKey::kMenu)] ? kModAlt : 0));
      shift_.store((live & kModShift) != 0, std::memory_order_relaxed); // the Locktopus' slow modifier
      bool on[8] = {};
      for (int i = 0; i < 8; ++i)
      {
        for (const BindAlternative &alt : binds_[i])
        {
          if ((live & alt.mods) == alt.mods && down_[alt.code])
          {
            on[i] = true;
            break;
          }
        }
      }
      for (size_t i = std::min<size_t>(extra.size(), 16); i-- > 0;)
      {
        down_[extra[i]] = saved[i]; // in reverse: a key named twice gets its own state back
      }
      // As the driver: up / down / left / right, INT16_MAX each way (mnk_input_driver.cpp GetDeviceState).
      int32_t v[4] = {};
      v[0] = (on[2] ? -32767 : 0) + (on[3] ? 32767 : 0);
      v[1] = (on[0] ? 32767 : 0) + (on[1] ? -32767 : 0);
      v[2] = (on[6] ? -32767 : 0) + (on[7] ? 32767 : 0);
      v[3] = (on[4] ? 32767 : 0) + (on[5] ? -32767 : 0);
      for (int i = 0; i < 4; ++i)
      {
        out[i] = int16_t(v[i]);
      }
    }

    // Shift was held at the last KeyboardStick (the test keys included).
    bool ShiftHeld() const { return shift_.load(std::memory_order_relaxed); }

    // The keyboard driver is on (mnk_mode; read by name: the cvar lives in the SDK's DLL).
    bool DriverOn()
    {
      RefreshBinds();
      return driver_on_.load(std::memory_order_relaxed);
    }

  private:
    bool Focused()
    {
      RefreshBinds();
      return has_focus_.load(std::memory_order_relaxed) || test_script_.load(std::memory_order_relaxed);
    }

    // The bind cvars (live-editable: the settings window's Controls tab), re-read twice a second.
    void RefreshBinds()
    {
      const int64_t now = NowMs();
      if (now - binds_at_.load(std::memory_order_relaxed) < 500)
      {
        return;
      }
      binds_at_.store(now, std::memory_order_relaxed);
      std::array<Bind, 8> fresh;
      for (int i = 0; i < 8; ++i)
      {
        fresh[i] = ParseBind(rex::cvar::GetFlagByName(kBindCvar[i]));
      }
      const std::string mode = rex::cvar::GetFlagByName("mnk_mode");
      driver_on_.store(mode == "true" || mode == "1", std::memory_order_relaxed);
      // While a keyboard test script runs the driver counts as focused (mnk_input_driver.cpp Focused).
      test_script_.store(!rex::cvar::GetFlagByName("mnk_test_script").empty(), std::memory_order_relaxed);
      std::lock_guard lock(mu_);
      binds_ = std::move(fresh);
    }

    void SetCode(uint16_t code, bool down)
    {
      if (code == 0 || code >= rex::ui::kInputCodeCount)
      {
        return;
      }
      std::lock_guard lock(mu_);
      down_[code] = down;
    }

    // Both names a key has, as the driver sets them (mnk_input_driver.cpp SetKeyEventLocked): its VirtualKey and
    // its scancode's own code.
    void SetKey(const rex::ui::KeyEvent &e, bool down)
    {
      const int sc = e.scancode();
      const bool has_sc = sc > 0 && sc < int(rex::ui::kInputCodeCount - rex::ui::kInputCodeScancodeBase);
      const uint16_t vk = static_cast<uint16_t>(e.virtual_key());
      std::lock_guard lock(mu_);
      if (vk != 0 && vk < 256)
      {
        down_[vk] = down;
      }
      if (has_sc)
      {
        down_[rex::ui::kInputCodeScancodeBase + sc] = down;
      }
    }

    std::mutex mu_;
    bool down_[rex::ui::kInputCodeCount] = {};
    std::array<Bind, 8> binds_;
    std::atomic<int64_t> binds_at_{-1000000};
    std::atomic<bool> driver_on_{true};
    std::atomic<bool> test_script_{false};
    std::atomic<bool> has_focus_{true};
    std::atomic<bool> shift_{false};
  };

  KeyTracker &Keys()
  {
    static KeyTracker tracker;
    return tracker;
  }

  // pdz_mnk_test_keys, parsed when it changes (game thread only).
  std::vector<uint16_t> TestKeys(std::string &text_out)
  {
    static std::string text;
    static std::vector<uint16_t> codes;
    const std::string &now = REXCVAR_GET(pdz_mnk_test_keys);
    if (now != text)
    {
      text = now;
      codes.clear();
      if (text != "-")
      {
        for (const BindAlternative &alt : ParseBind(text))
        {
          codes.push_back(alt.code);
        }
      }
      REXLOG_INFO("pdz_mnk_layout: test keys '{}' held ({} keys, simulated)", text, codes.size());
    }
    text_out = text;
    return codes;
  }

  // ---- The log ------------------------------------------------------------------------------------------------------

  struct LogState
  {
    std::mutex mu;
    std::string last_layout;
    int64_t last_keys_line = 0;
    // The last remap (OnPadState), for the line the controller update writes with the game's result.
    int16_t keys[4] = {};
    int16_t before[4] = {}, after[4] = {};
    std::array<int, 4> target{0, 1, 2, 3};
    bool moved[4] = {};
    bool follow = false;
    int64_t at_ms = 0;
  };
  LogState g_log;

  // LastKeyboardStick: the keyboard's stick at the last poll (OnPadState), any thread.
  std::mutex g_last_keys_mu;
  int16_t g_last_keys[4] = {};

  void LogAfterUpdate(const Layout &l)
  {
    std::lock_guard lock(g_log.mu);
    const int64_t now = NowMs();
    // Keys held, or a controller's stick (to show it keeps Rare's mapping), in the last poll.
    const bool recent = now - g_log.at_ms < 200;
    if (!recent || now - g_log.last_keys_line < 500)
    {
      return;
    }
    g_log.last_keys_line = now;
    std::string routes;
    for (int s = 0; s < 4; ++s)
    {
      if (g_log.keys[s] == 0)
      {
        continue;
      }
      routes += fmt::format("{}{} keys ({} {:+d}) -> {}{}", routes.empty() ? "keyboard: " : "; ", kIntentName[s],
                            kAxisName[s], g_log.keys[s], kAxisName[g_log.moved[s] ? g_log.target[s] : s],
                            g_log.moved[s] ? " (moved)" : g_log.target[s] == s ? "" : " (not moved)");
    }
    if (routes.empty())
    {
      routes = "no keyboard stick key (controller only: not changed)";
    }
    REXLOG_INFO("pdz_mnk_layout: follow {} | {} | pad 0 sticks {:+d} {:+d} {:+d} {:+d} -> {:+d} {:+d} {:+d} {:+d} | game "
                "move x {:+.2f} y {:+.2f}, look x {:+.2f} y {:+.2f}",
                g_log.follow ? "on" : "off", routes, g_log.before[0], g_log.before[1], g_log.before[2],
                g_log.before[3], g_log.after[0], g_log.after[1], g_log.after[2], g_log.after[3], l.move[0], l.move[1],
                l.look[0], l.look[1]);
  }

  // pdz_mnk_test_layout: style / swapped / inversion forced into pad 0's controller before its update.
  void ForceTestLayout(uint8_t *base, uint32_t ctrl)
  {
    const std::string &want = REXCVAR_GET(pdz_mnk_test_layout);
    const bool classic = want.find("classic") != std::string::npos;
    const bool standard = want.find("standard") != std::string::npos;
    if (!classic && !standard)
    {
      return; // '' (or anything else, e.g. 'profile' in an input script): the profile's own layout
    }
    StoreBe32(base, ctrl + kCtrlStyle, classic ? 0u : 1u);
    base[ctrl + kCtrlSwapped] = want.find("swapped") != std::string::npos ? 1 : 0;
    base[ctrl + kCtrlNotInverted] = want.find("inverted") != std::string::npos ? 0 : 1;
  }
} // namespace

// sub_822748C8(controller r3, player r4, -, aim allowed r6): the per-frame controller update (see the top).
void PdzUnarmedControllerUpdate(PPCContext &ctx, uint8_t *base, uint32_t ctrl, uint32_t participant); // unarmed_log.cpp

REX_HOOK_RAW(sub_822748C8)
{
  const uint32_t ctrl = ctx.r3.u32;
  const bool aim_allowed = (ctx.r6.u32 & 0xFF) != 0;
  const bool pad0 = Guest(ctrl) && LoadBe32(base, ctrl + kCtrlPad) == 0;
  if (pad0)
  {
    ForceTestLayout(base, ctrl);
  }
  const uint32_t participant = ctx.r4.u32; // r4 = the participant (824731E8), read before the call clobbers it
  __imp__sub_822748C8(ctx, base);
  PdzUnarmedControllerUpdate(ctx, base, ctrl, participant); // unarmed_log.cpp: pdz_unarmed_log's hands line
  if (!pad0)
  {
    return;
  }
  pdz::minigame::OnControllerUpdated(base, ctrl); // the Locktopus on the keyboard / mouse (after Rare's update)
  Layout l;
  l.valid = true;
  l.style = int32_t(LoadBe32(base, ctrl + kCtrlStyle));
  l.context = int32_t(LoadBe32(base, ctrl + kCtrlContext));
  l.inverted = base[ctrl + kCtrlNotInverted] == 0;
  l.swapped = base[ctrl + kCtrlSwapped] != 0;
  l.aim_allowed = aim_allowed;
  l.aim_hold = base[ctrl + kCtrlAimHold] != 0;
  l.aim_trigger = LoadBeFloat(base, ctrl + kCtrlAimTrigger);
  l.move[0] = LoadBeFloat(base, ctrl + kCtrlMoveX);
  l.move[1] = LoadBeFloat(base, ctrl + kCtrlMoveX + 4);
  l.look[0] = LoadBeFloat(base, ctrl + kCtrlLookX);
  l.look[1] = LoadBeFloat(base, ctrl + kCtrlLookX + 4);
  l.at_ms = NowMs();
  {
    std::lock_guard lock(g_layout_mu);
    g_layout = l;
  }
  if (REXCVAR_GET(pdz_mnk_layout_log))
  {
    const bool aiming = AimedAtUpdate(l);
    const bool follow = REXCVAR_GET(pdz_mnk_follow_layout);
    const std::string text =
        LayoutText(l, aiming) + (follow ? ", pdz_mnk_follow_layout on" : ", pdz_mnk_follow_layout off");
    bool changed = false;
    {
      std::lock_guard lock(g_log.mu);
      changed = text != g_log.last_layout;
      if (changed)
      {
        g_log.last_layout = text;
      }
    }
    if (changed)
    {
      std::array<int, 4> m{};
      const bool known = AxisForIntent(l, aiming, m);
      REXLOG_INFO("pdz_mnk_layout: pad 0 layout {}{} | strafe = {}, walk = {}, turn = {}, look up / down = {} | aim "
                  "allowed {} | the mouse as a stick (steering): turn -> {}, pitch -> {}{}",
                  text, known ? "" : " (not mapped: the keys are left as they are)", known ? kAxisName[m[0]] : "?",
                  known ? kAxisName[m[1]] : "?", known ? kAxisName[m[2]] : "?", known ? kAxisName[m[3]] : "?",
                  aim_allowed ? "yes" : "no", follow && known ? kAxisName[m[2]] : kAxisName[2],
                  follow && known ? kAxisName[m[3]] : kAxisName[3],
                  follow && known && l.inverted ? " (y pre-flipped: the game's Look Inversion doesn't flip the mouse)"
                                                : "");
    }
    LogAfterUpdate(l);
  }
}

void pdz::mnk_layout::Attach(rex::ui::Window *window)
{
  if (!window)
  {
    return;
  }
  window->AddInputListener(&Keys(), 0);
  window->AddListener(&Keys());
}

void pdz::mnk_layout::OnPadState(uint8_t *base, uint32_t user, uint32_t state)
{
  if (user != 0 || !Guest(state))
  {
    return;
  }
  std::string test_text;
  const std::vector<uint16_t> test_keys = TestKeys(test_text);
  int16_t keys[4];
  Keys().KeyboardStick(test_keys, keys);
  {
    std::lock_guard lock(g_last_keys_mu);
    std::memcpy(g_last_keys, keys, sizeof(g_last_keys));
  }
  const bool log = REXCVAR_GET(pdz_mnk_layout_log);
  if ((keys[0] | keys[1] | keys[2] | keys[3]) == 0 && !log)
  {
    return; // no keyboard stick key held: nothing to do (a controller alone is never touched)
  }
  int16_t s[4];
  for (uint32_t i = 0; i < 4; ++i)
  {
    s[i] = LoadBe16s(base, state + kStateSticks + 2 * i);
  }
  if ((keys[0] | keys[1] | keys[2] | keys[3] | s[0] | s[1] | s[2] | s[3]) == 0)
  {
    return; // nothing on the sticks (logged lines need something to show)
  }
  if (!test_keys.empty())
  {
    // pdz_mnk_test_keys: what the SDK would have merged in for those keys.
    for (int i = 0; i < 4; ++i)
    {
      s[i] = LargerMagnitude(s[i], keys[i]);
    }
  }
  int16_t before[4];
  std::memcpy(before, s, sizeof(before));
  const bool follow = REXCVAR_GET(pdz_mnk_follow_layout);
  const Layout l = CurrentLayout();
  std::array<int, 4> target{0, 1, 2, 3};
  bool moved[4] = {};
  const bool menu = rex::input::mnk::IsMenuMode();
  const bool aiming = l.valid && Aiming(l, base[state + kStateLeftTrigger]);
  if (l.valid && !menu && (Keys().DriverOn() || !test_keys.empty()) && AxisForIntent(l, aiming, target))
  {
    for (int a = 0; a < 4; ++a)
    {
      moved[a] = follow && keys[a] != 0 && target[a] != a && s[a] == keys[a];
    }
    for (int a = 0; a < 4; ++a)
    {
      if (moved[a])
      {
        s[a] = 0;
      }
    }
    for (int a = 0; a < 4; ++a)
    {
      if (moved[a])
      {
        s[target[a]] = LargerMagnitude(s[target[a]], keys[a]);
      }
    }
  }
  if (std::memcmp(before, s, sizeof(s)) != 0 || !test_keys.empty())
  {
    for (uint32_t i = 0; i < 4; ++i)
    {
      StoreBe16(base, state + kStateSticks + 2 * i, s[i]);
    }
  }
  if (log)
  {
    std::lock_guard lock(g_log.mu);
    std::memcpy(g_log.keys, keys, sizeof(keys));
    std::memcpy(g_log.before, before, sizeof(before));
    std::memcpy(g_log.after, s, sizeof(s));
    g_log.target = target;
    std::memcpy(g_log.moved, moved, sizeof(moved));
    g_log.follow = follow;
    g_log.at_ms = NowMs();
  }
}

pdz::mnk_layout::SteerRoute pdz::mnk_layout::RouteForSteering()
{
  SteerRoute r;
  if (!REXCVAR_GET(pdz_mnk_follow_layout))
  {
    return r;
  }
  const Layout l = CurrentLayout();
  std::array<int, 4> m{};
  // Aiming as the last update saw it (Classic PD on foot, +76 > 0: 82274CE0).
  if (!l.valid || !AxisForIntent(l, AimedAtUpdate(l), m))
  {
    return r;
  }
  r.x_left = m[2] == 0;
  r.y_left = m[3] == 1;
  r.undo_game_invert = l.inverted;
  return r;
}

void pdz::mnk_layout::LastKeyboardStick(int16_t out[4])
{
  std::lock_guard lock(g_last_keys_mu);
  std::memcpy(out, g_last_keys, sizeof(g_last_keys));
}

bool pdz::mnk_layout::LastKeyboardSlow() { return Keys().ShiftHeld(); }
