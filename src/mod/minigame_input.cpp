// minigame_input.cpp - see minigame_input.h. Title Update 3 addresses.
//
// Rare's Locktopus ("lockbot" subgame; set-up sub_82648BF0(subgame r3, gadget r4, lock data r5) with the
// LOCKTOPUS_VIBRATE / _SERVO / _COMPLETE cues, update sub_826492E8(subgame r3, f1 = dt)).
//
// The lock data (an aid_xml_subgames_lockbot_* asset): +12 four floats (0.3 / 0.2 / 0.15 / 0.15 in every lock, read
// only as "greater than 0" flags per difficulty), +16 eight pins of 3 bytes: x, y (signed, over 128 = the stick the
// pin wants) and a non-zero marker for a pin that exists. The set-up copies the pins to subgame +36 (3 bytes each),
// +100 = the current pin (the first with a non-zero marker), and the radius at +140: 0.45 on Agent, 0.33 on Secret
// Agent, 0.20 on Perfect and Dark Agent (the difficulty is the global 8206BFD8's +4).
//
// The update, state (+32) 1 = picking: subgame +4 is the gadget, its +92 the holder (the player); the holder's vtable
// +664 gives his participant P (P +1680 = pad index, the controller object starts at P +1680). Every frame it copies
// P +1724 / +1728 (controller +44 / +48) straight to its +124 / +128 (no smoothing of its own), then (byte +144: real
// lock data) measures dist = |stick - pin / 128| (+132). Inside the radius the hold timer +116 counts DOWN from 1.0 s;
// at 0 the pin's marker is cleared and the state goes to 2 (next pin, or 3 = complete when all eight markers are 0).
// Outside it counts back up to 1.0 (leaving the pin undoes the hold gradually). The feedback (sub_8264A958, f1 = 1 -
// dist, or 1 inside) is the LOCKTOPUS_SERVO cue and pad vibration; the draw (sub_8264A6D8) shows the eight pins
// round a circle with the current pin's hold: Rare draws no pick or cursor for the stick itself. So a pin is "angle +
// deflection within the radius": a full-deflection stick can only set pins with 1 - |pin| / 128 < radius, and the
// eight key directions of a keyboard only pins within the radius of one of them (see docs/MOUSE_KEYBOARD.md).
//
// Controller +44 / +48 are the filtered LEFT stick as sub_82275D30 writes them from the raw sticks +60..+72: (+44,
// +48) = (left x, left y) re-polarised. The right stick (+52 / +56) is never read by the subgame.
#include "minigame_input.h"

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include <algorithm>
#include <cctype>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include <fmt/format.h>

#include "mnk_layout.h"
#include "mouse_look.h"

REXCVAR_DEFINE_BOOL(pdz_fix_minigame_keyboard, true, "Mouse",
                    "The Locktopus on the keyboard and mouse (default on): while the lock-picking runs, the keys bound "
                    "to the sticks steer a virtual stick instead of snapping it: left / right keys (A / D, Numpad 4 / "
                    "6) turn the pick round the circle, up / down keys (W / S, Numpad 8 / 2) push it out and pull it "
                    "back, Shift slows it, and the mouse moves it too. It stays where it was left (so a pin can be "
                    "held) and starts at the centre with each lock. A controller is unchanged: nothing is written "
                    "while its left stick is pushed or while no key or mouse has been used in the lock. Off = Rare's "
                    "read of the left stick only")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(pdz_minigame_key_turn_s, 2.5, "Mouse",
                      "Locktopus on the keyboard: seconds the left / right keys take to turn the pick a full circle "
                      "(pdz_fix_minigame_keyboard)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(pdz_minigame_key_slow, 0.4, "Mouse",
                      "Locktopus on the keyboard: the keys' speed while Shift is held, as a fraction of the full speed")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(pdz_minigame_mouse_counts, 400.0, "Mouse",
                      "Locktopus on the mouse: mouse counts that move the pick from the centre to full deflection")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(pdz_minigame_inputs, "both", "Mouse",
                      "Locktopus on the keyboard and mouse: what steers the pick, \"both\" (default), \"mouse\" or "
                      "\"keys\" (the settings window's Controls tab, Locktopus). Needs pdz_fix_minigame_keyboard")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(pdz_minigame_input_log, false, "Mouse",
                    "Diagnostic: at each lock's start its data (pins, radius), twice a second while the Locktopus runs "
                    "its state, pin, hold timer and the stick it read, the virtual stick, pad 0's raw sticks, the "
                    "keys, the mouse motion and what the fix wrote, and a line as each pin sets")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_minigame_test_aim, false, "Testing",
                    "TESTING TOOL: while the Locktopus runs, the fix writes the current pin's own target (read from the "
                    "subgame) instead of the keyboard's or the mouse's, so a scripted run completes a lock and proves "
                    "the written stick reaches Rare's code. Needs pdz_fix_minigame_keyboard")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(pdz_minigame_test_radius, 0.0, "Testing",
                      "TESTING TOOL: > 0 replaces the Locktopus pin radius Rare sets from the difficulty (0.45 / 0.33 "
                      "/ 0.20) at each lock's start, so a scripted run can test the hard radius on any difficulty")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(pdz_minigame_test_pins, "", "Testing",
                      "TESTING TOOL: 'x,y;x,y;...' (signed bytes over 128, up to 8) replaces a Locktopus lock's pins at "
                      "its start, so another lock's pins (e.g. the Mansion intercom door's 47,102;-10,-90;90,-40;"
                      "-88,-60) can be picked at a lock a scripted run can reach")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REX_EXTERN(__imp__sub_826492E8);
REX_EXTERN(__imp__sub_82648BF0);

namespace
{
  using Clock = std::chrono::steady_clock;

  // Controller object (participant +1680).
  constexpr uint32_t kCtrlPad = 0, kCtrlStickA = 44, kCtrlRawLeft = 60, kCtrlRawRight = 68;
  // The lockbot subgame.
  constexpr uint32_t kSubState = 32, kSubPins = 36, kSubPin = 100, kSubTimer = 116, kSubStickX = 124,
                     kSubStickY = 128, kSubDist = 132, kSubRadius = 140, kSubRealData = 144;
  // The lock data handed to the set-up.
  constexpr uint32_t kDataPins = 16;
  constexpr int64_t kActiveNs = 150'000'000;  // the subgame updated this recently = running
  constexpr double kControllerPush = 0.1;      // a raw left stick beyond this is a controller in use (Rare's threshold)
  constexpr double kRadiusPerSecond = 1.0;     // the up / down keys: centre to full deflection in 1 s at full speed
  constexpr double kPi = 3.14159265358979323846;

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
  void StoreBeFloat(uint8_t *base, uint32_t address, float v)
  {
    uint32_t bits;
    std::memcpy(&bits, &v, sizeof(bits));
    uint8_t *p = base + address;
    p[0] = uint8_t(bits >> 24);
    p[1] = uint8_t(bits >> 16);
    p[2] = uint8_t(bits >> 8);
    p[3] = uint8_t(bits);
  }
  bool Guest(uint32_t address) { return address >= 0x10000u && address < 0xF0000000u; }
  int64_t NowNs()
  {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
  }
  double Degrees(double radians)
  {
    double d = radians * 180.0 / kPi;
    d = std::fmod(d, 360.0);
    return d < 0.0 ? d + 360.0 : d;
  }

  std::atomic<int64_t> g_active_ns{INT64_MIN / 2};
  std::atomic<uint32_t> g_subgame{0};
  std::atomic<bool> g_lockbot{false}; // the active subgame is the Locktopus (the mouse and the test aim apply)

  // Game thread only (the controller update and the subgame update both run there).
  struct State
  {
    // The virtual stick (the SDK's convention: x right, y up, 1 = full deflection), steered by the keys and the mouse.
    // pick.used: a key or the mouse steered it in this lock (before that a controller alone stands).
    pdz::minigame::Pick pick;
    bool was_active = false;
    int64_t last_step_ns = 0;
    // The sign of the raw left stick's axes against the keyboard's (learned when only keys push: Rare's pad float
    // convention for y is not assumed).
    float sign_x = 1.0f, sign_y = 1.0f;
    bool sign_known = false;
    // For the log.
    int64_t last_log_ns = 0;
    std::string last_action;
    float wrote_x = 0.0f, wrote_y = 0.0f;
    int16_t keys[4] = {};
    bool slow = false;
    double mdx = 0.0, mdy = 0.0;
    float raw[4] = {}, filtered_before[2] = {};
    uint32_t ctrl = 0;
    bool fix_on = false;
    // The lock as the update hook last saw it (to log each pin as it sets).
    uint32_t seen_state = 0, seen_pin = 0;
    uint8_t seen_marker = 0;
  };
  State g;

  bool Active() { return NowNs() - g_active_ns.load(std::memory_order_relaxed) < kActiveNs; }

  std::string PinText(const uint8_t *base, uint32_t at)
  {
    const int8_t x = int8_t(base[at]), y = int8_t(base[at + 1]);
    const double tx = x / 128.0, ty = y / 128.0;
    return fmt::format("({:+d} {:+d}: angle {:.0f} deg, deflection {:.2f}, marker {})", x, y,
                       Degrees(std::atan2(ty, tx)), std::hypot(tx, ty), base[at + 2]);
  }

  void LogSubgame(const uint8_t *base, uint32_t sub, double dt)
  {
    const int64_t now = NowNs();
    if (now - g.last_log_ns < 500'000'000)
    {
      return;
    }
    g.last_log_ns = now;
    const uint32_t pin = LoadBe32(base, sub + kSubPin);
    const uint32_t pin_at = sub + kSubPins + 3 * std::min<uint32_t>(pin, 7);
    REXLOG_INFO("pdz_minigame_input: lockbot {:08X} dt {:.4f}: state {} pin {} {} radius {:.3f} hold {:.2f} s dist "
                "{:.3f} real data {} | stick read {:+.3f} {:+.3f} | virtual angle {:.1f} deg radius {:.2f}{}{} | pad "
                "0 ctrl {:08X}: raw left {:+.3f} {:+.3f} right {:+.3f} {:+.3f}, Rare's +44/+48 {:+.3f} {:+.3f} | keys "
                "{:+d} {:+d} {:+d} {:+d}{} | mouse {:+.0f} {:+.0f} | fix {}: {}{}",
                sub, dt, LoadBe32(base, sub + kSubState), pin, PinText(base, pin_at),
                LoadBeFloat(base, sub + kSubRadius), LoadBeFloat(base, sub + kSubTimer),
                LoadBeFloat(base, sub + kSubDist), base[sub + kSubRealData], LoadBeFloat(base, sub + kSubStickX),
                LoadBeFloat(base, sub + kSubStickY), Degrees(g.pick.angle), g.pick.radius, g.pick.used ? " (used)" : " (unused)",
                g.slow ? " slow" : "", g.ctrl, g.raw[0], g.raw[1], g.raw[2], g.raw[3], g.filtered_before[0],
                g.filtered_before[1], g.keys[0], g.keys[1], g.keys[2], g.keys[3], g.slow ? " +Shift" : "", g.mdx,
                g.mdy, g.fix_on ? "on" : "off", g.last_action,
                g.last_action.rfind("wrote", 0) == 0 ? fmt::format(" {:+.3f} {:+.3f}", g.wrote_x, g.wrote_y)
                                                     : std::string());
  }

  // A pin set, the state changed: one line each (the log cvar on).
  void LogProgress(const uint8_t *base, uint32_t sub)
  {
    const uint32_t state = LoadBe32(base, sub + kSubState);
    const uint32_t pin = LoadBe32(base, sub + kSubPin);
    const uint32_t pin_at = sub + kSubPins + 3 * std::min<uint32_t>(pin, 7);
    const uint8_t marker = base[pin_at + 2];
    if (state == g.seen_state && pin == g.seen_pin && marker == g.seen_marker)
    {
      return;
    }
    if (g.seen_state == 1 && (pin != g.seen_pin || (marker == 0 && g.seen_marker != 0) || state == 3))
    {
      const uint32_t old_at = sub + kSubPins + 3 * std::min<uint32_t>(g.seen_pin, 7);
      REXLOG_INFO("pdz_minigame_input: lockbot {:08X}: pin {} SET {} with the stick read {:+.3f} {:+.3f} (angle {:.1f} "
                  "deg, deflection {:.2f}; dist {:.3f} < radius {:.3f}); virtual stick angle {:.1f} deg radius {:.2f}; "
                  "now state {} pin {}",
                  sub, g.seen_pin, PinText(base, old_at), LoadBeFloat(base, sub + kSubStickX),
                  LoadBeFloat(base, sub + kSubStickY),
                  Degrees(std::atan2(LoadBeFloat(base, sub + kSubStickY), LoadBeFloat(base, sub + kSubStickX))),
                  std::hypot(LoadBeFloat(base, sub + kSubStickX), LoadBeFloat(base, sub + kSubStickY)),
                  LoadBeFloat(base, sub + kSubDist), LoadBeFloat(base, sub + kSubRadius), Degrees(g.pick.angle), g.pick.radius,
                  state, pin);
    }
    if (state == 3 && g.seen_state != 3)
    {
      REXLOG_INFO("pdz_minigame_input: lockbot {:08X}: COMPLETE (state 3)", sub);
    }
    g.seen_state = state;
    g.seen_pin = pin;
    g.seen_marker = marker;
  }

  void ResetVirtual() { g.pick = pdz::minigame::Pick{}; }
} // namespace

// The lockbot subgame's set-up (subgame r3, gadget r4, lock data r5): the lock's data and radius, the test radius.
REX_HOOK_RAW(sub_82648BF0)
{
  const uint32_t sub = ctx.r3.u32;
  const uint32_t data = ctx.r5.u32;
  __imp__sub_82648BF0(ctx, base);
  if (!Guest(sub))
  {
    return;
  }
  const double test_radius = REXCVAR_GET(pdz_minigame_test_radius);
  const float rare_radius = LoadBeFloat(base, sub + kSubRadius);
  if (test_radius > 0.0 && base[sub + kSubRealData] != 0)
  {
    StoreBeFloat(base, sub + kSubRadius, float(test_radius));
  }
  const std::string test_pins = REXCVAR_GET(pdz_minigame_test_pins);
  if (!test_pins.empty() && base[sub + kSubRealData] != 0)
  {
    // 'x,y;x,y;...': the pins replaced in data order, the rest cleared, the first pin current (markers 10, 20, ...
    // as Rare's data has them).
    for (uint32_t i = 0; i < 8; ++i)
    {
      base[sub + kSubPins + 3 * i] = base[sub + kSubPins + 3 * i + 1] = base[sub + kSubPins + 3 * i + 2] = 0;
    }
    uint32_t n = 0;
    size_t pos = 0;
    while (n < 8 && pos < test_pins.size())
    {
      size_t semi = test_pins.find(';', pos);
      if (semi == std::string::npos)
      {
        semi = test_pins.size();
      }
      const std::string pair = test_pins.substr(pos, semi - pos);
      const size_t comma = pair.find(',');
      if (comma != std::string::npos)
      {
        const int x = std::clamp(std::atoi(pair.substr(0, comma).c_str()), -128, 127);
        const int y = std::clamp(std::atoi(pair.substr(comma + 1).c_str()), -128, 127);
        base[sub + kSubPins + 3 * n] = uint8_t(int8_t(x));
        base[sub + kSubPins + 3 * n + 1] = uint8_t(int8_t(y));
        base[sub + kSubPins + 3 * n + 2] = uint8_t(10 * (n + 1));
        ++n;
      }
      pos = semi + 1;
    }
    base[sub + kSubPin] = base[sub + kSubPin + 1] = base[sub + kSubPin + 2] = base[sub + kSubPin + 3] = 0;
    REXLOG_INFO("pdz_minigame_input: lockbot {:08X}: {} pin(s) replaced by pdz_minigame_test_pins '{}'", sub, n,
                test_pins);
  }
  if (!REXCVAR_GET(pdz_minigame_input_log))
  {
    return;
  }
  // The data's name: the asset's path string ("...\lockbot\<level>\<lock>\default.xml") sits just before the data in
  // the package; look back for it (bounded, guest memory only).
  std::string name = "(no name found before the data)";
  if (Guest(data) && data > 0x400)
  {
    const char *p = reinterpret_cast<const char *>(base + data - 0x200);
    std::string window(p, 0x200);
    const size_t at = window.rfind("lockbot\\");
    if (at != std::string::npos)
    {
      const size_t end = window.find('\0', at);
      name = window.substr(at, end == std::string::npos ? std::string::npos : end - at);
    }
  }
  std::string pins;
  int count = 0;
  for (uint32_t i = 0; i < 8; ++i)
  {
    const uint32_t at = sub + kSubPins + 3 * i;
    if (base[at + 2] != 0)
    {
      pins += fmt::format(" [{}] {}", i, PinText(base, at));
      ++count;
    }
  }
  REXLOG_INFO("pdz_minigame_input: lockbot {:08X} set up: data {:08X} '{}', real data {}, {} pin(s):{} | radius {:.3f} "
              "from the difficulty{} | first pin {}",
              sub, data, name, base[sub + kSubRealData], count, pins, rare_radius,
              test_radius > 0.0 ? fmt::format(", replaced by the test radius {:.3f}", test_radius) : std::string(),
              LoadBe32(base, sub + kSubPin));
}

// The lockbot subgame's update (subgame r3, f1 = dt).
REX_HOOK_RAW(sub_826492E8)
{
  const uint32_t sub = ctx.r3.u32;
  const double dt = ctx.f1.f64;
  g_active_ns.store(NowNs(), std::memory_order_relaxed);
  g_subgame.store(sub, std::memory_order_relaxed);
  g_lockbot.store(true, std::memory_order_relaxed);
  __imp__sub_826492E8(ctx, base);
  if (REXCVAR_GET(pdz_minigame_input_log) && Guest(sub))
  {
    LogProgress(base, sub);
    LogSubgame(base, sub, dt);
  }
}

void pdz::minigame::NoteDemoKit(uint32_t subgame)
{
  g_active_ns.store(NowNs(), std::memory_order_relaxed);
  g_subgame.store(subgame, std::memory_order_relaxed);
  g_lockbot.store(false, std::memory_order_relaxed);
  if (REXCVAR_GET(pdz_minigame_input_log))
  {
    const int64_t now = NowNs();
    if (now - g.last_log_ns >= 500'000'000)
    {
      g.last_log_ns = now;
      REXLOG_INFO("pdz_minigame_input: demo kit puzzle {:08X} updating | pad 0 ctrl {:08X}: raw left {:+.3f} {:+.3f}, "
                  "Rare's +44/+48 {:+.3f} {:+.3f} | keys {:+d} {:+d} {:+d} {:+d} | fix {}: {}",
                  subgame, g.ctrl, g.raw[0], g.raw[1], g.filtered_before[0], g.filtered_before[1], g.keys[0],
                  g.keys[1], g.keys[2], g.keys[3], g.fix_on ? "on" : "off", g.last_action);
    }
  }
}

void pdz::minigame::OnControllerUpdated(uint8_t *base, uint32_t ctrl)
{
  if (!Guest(ctrl) || LoadBe32(base, ctrl + kCtrlPad) != 0)
  {
    return;
  }
  const int64_t now = NowNs();
  if (!Active())
  {
    if (g.was_active)
    {
      g.was_active = false; // the lock closed: the next one starts at the centre
      ResetVirtual();
      g.seen_state = g.seen_pin = 0;
      g.seen_marker = 0;
    }
    double dx, dy;
    pdz::mouse::TakeRawMouseMotion(dx, dy); // drained, so a lock never starts with a backlog
    g.last_step_ns = now;
    return;
  }
  if (!g.was_active)
  {
    g.was_active = true;
    ResetVirtual();
    g.last_step_ns = now;
  }
  const double dt = std::clamp(double(now - g.last_step_ns) * 1e-9, 0.0, 0.05);
  g.last_step_ns = now;
  g.ctrl = ctrl;
  g.fix_on = REXCVAR_GET(pdz_fix_minigame_keyboard);
  for (uint32_t i = 0; i < 4; ++i)
  {
    g.raw[i] = LoadBeFloat(base, ctrl + kCtrlRawLeft + 4 * i);
  }
  g.filtered_before[0] = LoadBeFloat(base, ctrl + kCtrlStickA);
  g.filtered_before[1] = LoadBeFloat(base, ctrl + kCtrlStickA + 4);
  pdz::mnk_layout::LastKeyboardStick(g.keys);
  g.slow = pdz::mnk_layout::LastKeyboardSlow();
  double dx = 0.0, dy = 0.0;
  pdz::mouse::TakeRawMouseMotion(dx, dy);
  g.mdx = dx;
  g.mdy = dy;
  // The keyboard: both sticks' keys, as the SDK's stick values (lx, ly, rx, ry; up and right positive).
  const double kx = double(g.keys[0]) + double(g.keys[2]);
  const double ky = double(g.keys[1]) + double(g.keys[3]);
  const double key_mag = std::hypot(kx, ky);
  const bool keys_held = key_mag > 0.0;
  // Rare's raw left stick against the keyboard's left stick: learn the sign of each axis once (only keys pushing).
  const double raw_left_mag = std::hypot(double(g.raw[0]), double(g.raw[1]));
  if (!g.sign_known && g.keys[0] != 0 && g.keys[1] != 0 && raw_left_mag > kControllerPush &&
      std::fabs(g.raw[0]) > 0.3f && std::fabs(g.raw[1]) > 0.3f)
  {
    g.sign_x = (g.raw[0] > 0) == (g.keys[0] > 0) ? 1.0f : -1.0f;
    g.sign_y = (g.raw[1] > 0) == (g.keys[1] > 0) ? 1.0f : -1.0f;
    g.sign_known = true;
    REXLOG_INFO("pdz_minigame_input: pad 0's raw left stick against the keyboard's: x sign {:+.0f}, y sign {:+.0f} "
                "(raw {:+.3f} {:+.3f} for keys {:+d} {:+d})",
                g.sign_x, g.sign_y, g.raw[0], g.raw[1], g.keys[0], g.keys[1]);
  }
  const bool lockbot = g_lockbot.load(std::memory_order_relaxed);
  const pdz::minigame::SteerTuning tuning = pdz::minigame::LiveTuning();
  // Keys held that the chosen inputs leave out (pdz_minigame_inputs "mouse"): they must not steer through Rare's own
  // read either, so the virtual stick is written while they are held.
  const bool ignored_keys = keys_held && tuning.inputs == pdz::minigame::PickInputs::kMouse;
  if (lockbot)
  {
    // The virtual stick follows the keys and the mouse whatever is written below (the shared step: the settings
    // window's practice dial runs the same one).
    pdz::minigame::SteerPick(g.pick, tuning, kx, ky, g.slow, dx, dy, dt);
  }
  if (!g.fix_on)
  {
    g.last_action = "off (Rare's read)";
    return;
  }
  // A controller's left stick pushed (beyond what the keyboard's own left-stick keys explain): Rare's read stands,
  // and the virtual stick follows the controller so that the keys carry on from where the pad left the pick.
  const bool keyboard_left = g.keys[0] != 0 || g.keys[1] != 0;
  if (raw_left_mag > kControllerPush && !keyboard_left)
  {
    if (lockbot)
    {
      g.pick.radius = std::min(raw_left_mag, 1.0);
      g.pick.angle = std::atan2(double(g.raw[1]) * g.sign_y, double(g.raw[0]) * g.sign_x);
    }
    g.last_action = "untouched (a controller's left stick is pushed)";
    return;
  }
  double x, y;
  const uint32_t sub = g_subgame.load(std::memory_order_relaxed);
  if (lockbot && REXCVAR_GET(pdz_minigame_test_aim) && Guest(sub))
  {
    // The pin's target as Rare stored it (bytes over 128), in Rare's own convention: no sign fix.
    const uint32_t pin_at = sub + kSubPins + 3 * std::min<uint32_t>(LoadBe32(base, sub + kSubPin), 7);
    g.wrote_x = float(int8_t(base[pin_at]) / 128.0);
    g.wrote_y = float(int8_t(base[pin_at + 1]) / 128.0);
    g.last_action = "wrote the pin's target (pdz_minigame_test_aim)";
    StoreBeFloat(base, ctrl + kCtrlStickA, g.wrote_x);
    StoreBeFloat(base, ctrl + kCtrlStickA + 4, g.wrote_y);
    return;
  }
  if (lockbot)
  {
    if (!g.pick.used && !ignored_keys)
    {
      g.last_action = "untouched (no key or mouse used in this lock yet)";
      return;
    }
    x = g.pick.radius * std::cos(g.pick.angle);
    y = g.pick.radius * std::sin(g.pick.angle);
    g.last_action = ignored_keys ? "wrote the virtual stick (keys ignored: pdz_minigame_inputs mouse)"
                    : keys_held  ? "wrote the virtual stick (keys steering)"
                    : (dx != 0.0 || dy != 0.0) ? "wrote the virtual stick (mouse steering)"
                                               : "wrote the virtual stick (held)";
  }
  else
  {
    // Another stick-reading minigame (the demo kit's tile puzzle reads the stick as a digital pad): the keys'
    // direction, snapped, as before.
    if (!keys_held)
    {
      g.last_action = "untouched (no key held)";
      return;
    }
    x = kx / key_mag;
    y = ky / key_mag;
    g.last_action = "wrote the keys' direction";
  }
  g.wrote_x = float(x) * g.sign_x;
  g.wrote_y = float(y) * g.sign_y;
  StoreBeFloat(base, ctrl + kCtrlStickA, g.wrote_x);
  StoreBeFloat(base, ctrl + kCtrlStickA + 4, g.wrote_y);
}

// ---- Shared with the settings window's practice dial ----

pdz::minigame::PickInputs pdz::minigame::PickInputsFromName(const std::string &name)
{
  std::string n;
  for (char c : name)
  {
    n += char(std::tolower(static_cast<unsigned char>(c)));
  }
  return n == "mouse" ? PickInputs::kMouse : n == "keys" ? PickInputs::kKeys : PickInputs::kBoth;
}

const char *pdz::minigame::PickInputsName(PickInputs inputs)
{
  return inputs == PickInputs::kMouse ? "mouse" : inputs == PickInputs::kKeys ? "keys" : "both";
}

pdz::minigame::SteerTuning pdz::minigame::LiveTuning()
{
  SteerTuning t;
  t.key_turn_s = REXCVAR_GET(pdz_minigame_key_turn_s);
  t.key_slow = REXCVAR_GET(pdz_minigame_key_slow);
  t.mouse_counts = REXCVAR_GET(pdz_minigame_mouse_counts);
  t.inputs = PickInputsFromName(REXCVAR_GET(pdz_minigame_inputs));
  return t;
}

// The virtual stick: a step of the keys (both sticks' keys summed) and the mouse, dt seconds.
bool pdz::minigame::SteerPick(Pick &pick, const SteerTuning &tuning, double kx, double ky, bool slow, double mdx,
                              double mdy, double dt)
{
  if (tuning.inputs == PickInputs::kMouse)
  {
    kx = ky = 0.0;
  }
  if (tuning.inputs == PickInputs::kKeys)
  {
    mdx = mdy = 0.0;
  }
  const double slow_k = slow ? std::clamp(tuning.key_slow, 0.05, 1.0) : 1.0;
  const double turn_s = std::max(0.2, tuning.key_turn_s);
  bool moved = false;
  if (kx != 0.0)
  {
    pick.angle -= (kx > 0 ? 1.0 : -1.0) * (2.0 * kPi / turn_s) * slow_k * dt; // right = clockwise
    moved = true;
  }
  if (ky != 0.0)
  {
    pick.radius = std::clamp(pick.radius + (ky > 0 ? 1.0 : -1.0) * kRadiusPerSecond * slow_k * dt, 0.0, 1.0);
    moved = true;
  }
  const double counts = std::max(20.0, tuning.mouse_counts);
  if (mdx != 0.0 || mdy != 0.0)
  {
    double x = pick.radius * std::cos(pick.angle) + mdx / counts;
    double y = pick.radius * std::sin(pick.angle) - mdy / counts; // the mouse up = the stick up
    const double r = std::hypot(x, y);
    if (r > 1.0)
    {
      x /= r;
      y /= r;
    }
    pick.radius = std::min(r, 1.0);
    if (r > 0.02)
    {
      pick.angle = std::atan2(y, x);
    }
    moved = true;
  }
  if (moved)
  {
    pick.angle = std::fmod(pick.angle, 2.0 * kPi);
    if (pick.angle < 0.0)
    {
      pick.angle += 2.0 * kPi;
    }
    pick.used = true;
  }
  return moved;
}

bool pdz::minigame::StepPinHold(double &hold, double stick_x, double stick_y, double pin_x, double pin_y,
                                double radius, double dt)
{
  if (std::hypot(stick_x - pin_x, stick_y - pin_y) < radius)
  {
    hold -= dt;
    if (hold <= 0.0)
    {
      hold = 0.0;
      return true;
    }
  }
  else
  {
    hold = std::min(kPinHoldSeconds, hold + dt);
  }
  return false;
}
