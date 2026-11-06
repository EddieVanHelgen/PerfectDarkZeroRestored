// mouse_look.cpp - see mouse_look.h. Title Update 3 addresses; launch addresses in brackets.
//
// The hook: sub_825AF590 [8259FD70](player, view, look state, f1, f2 = dt) is the rotational aim assist, the last
// step of the gameplay look update (both look models, after the pitch clamp, only with look control). Mouse counts are
// an angle, not a rate: they go straight into the view's yaw (+5744) and pitch (+5748), read by its vtable +24 / +28
// getters sub_8254E0E0 / sub_8254E0E8 [8253E9A8 / 8253E9B0]; the stick's curve, acceleration, turn cap and the in-game
// sensitivity are skipped. Turning right lowers the yaw; the pitch grows downward. The pitch is clamped to the
// game's own limit (float +64 of the look tuning block *(*(82D6BFDC [82D6B2B4]) + 12), as the look update does).
// Only the player on guest user 0 (input block +6560 [same], its user index +1680 [+1152]; the SDK merges keyboard
// and mouse into user 0).
//
// Zoom: the camera FOV is 58 deg x player vtable +660 [+656] = sub_825AC8E0 [8259D0D8] = 1 - player +6812. The turn
// is scaled by tan(fov / 2) / tan(29 deg), so the picture moves under the mouse at the same rate at the screen centre
// whatever the zoom (slower when zoomed, in proportion to the FOV).
//
// Aim help: while the keyboard / mouse is the last device used (pdz_mouse_aim_assist_off, on) the rotational aim
// assist is skipped here and auto-aim's bend is undone (aim_assist.cpp); a controller's input brings both back.
#include "mouse_look.h"
#include "../game_cvars.h"

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/input/input_system.h>
#include <rex/input/mnk/mnk_input_driver.h>
#include <rex/logging.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/windowed_app_context.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include <fmt/format.h>
#ifdef _WIN32
#include <windows.h>
#endif

#include "../game_constants.h"
#include "hook_profile.h" // pdz_hook_profile (must follow <rex/hook.h>)

REXCVAR_DEFINE_BOOL(pdz_mouse_look, true, "Mouse",
                    "Mouse aim: on foot the mouse turns the view directly (1:1, no dead zone, response curve, "
                    "acceleration or turn cap) instead of emulating the right stick; slower when zoomed, in proportion "
                    "to the field of view; the game's pitch limit applies. Where the gameplay look doesn't run "
                    "(menus, cutscenes) the emulated stick (mnk_sensitivity) is in charge. While something else is "
                    "steered (the CamSpy, a turret, the guided rocket, the hovercraft, the jetpack) the mouse becomes "
                    "the right stick it reads (mnk_sensitivity's scale). Needs mnk_mode and mnk_mouse")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(pdz_mouse_sensitivity, 2.0, "Mouse",
                      "Mouse aim sensitivity: the view turns 0.022 deg x this per mouse count (the Source games' scale; "
                      "2.0 = 0.044 deg per count)")
    .range(0.01, 100.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(pdz_mouse_invert, false, "Mouse", "Mouse aim: invert up / down")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(pdz_mouse_aim_assist_off, true, "Mouse",
                    "While the keyboard / mouse is the last device used, the game's auto-aim (the shot bend) and its "
                    "rotational aim assist are off; any controller input brings them back. Each switch is logged once")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(pdz_mouse_test_turn, 0.0, "Mouse",
                      "TESTING TOOL: a synthetic mouse turn in degrees per second (yaw) fed into the mouse aim path every "
                      "look update, pointer or not, with a 'pdz_mouse: test' line every 2 s (yaw / pitch before and "
                      "after). 0 = off")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(pdz_mouse_test_pitch, 0.0, "Mouse",
                      "TESTING TOOL: as pdz_mouse_test_turn, for the pitch (degrees per second, + = down)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_mouse_steer_log, false, "Mouse",
                    "Diagnostic: once a second per caller, a line for each look update that steers something other "
                    "than the player's own view (owner, view, which view fields it changed, the mouse hand-over state), "
                    "and pad 0's right stick into / out of the pad filter")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(pdz_mouse_test_motion, 0.0, "Mouse",
                      "TESTING TOOL: synthetic mouse motion, counts per second to the right, sent as relative mouse "
                      "moves through the OS (Windows SendInput) while the game window is in front: the whole path of a "
                      "real mouse. A 'pdz_mouse: test motion' line every 2 s. 0 = off")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(pdz_mouse_test_motion_y, 0.0, "Mouse",
                      "TESTING TOOL: as pdz_mouse_test_motion, counts per second downward")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(pdz_mouse_test_face_yaw, 999.0, "Mouse",
                      "TESTING TOOL: sets the on-foot view's yaw to this (degrees) once each time the value changes "
                      "(scripted routes: face a direction, then walk). 999 = off")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(pdz_mouse_test_face_pitch, 999.0, "Mouse",
                      "TESTING TOOL: as pdz_mouse_test_face_yaw, for the pitch (degrees, + = down). 999 = off")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REX_EXTERN(__imp__sub_825AF590);

namespace pdz::mouse
{
  namespace
  {
    // If Consume() stops being called (the look update doesn't run in this game state) the pointer must not stay
    // hidden and locked with nothing consuming its motion.
    constexpr std::chrono::milliseconds kConsumerTimeout{300};
    constexpr std::chrono::milliseconds kWatchdogPeriod{100};
    // After asking mnk to let go: never engage before the short one, stop waiting after the long one.
    constexpr std::chrono::milliseconds kHandoffMin{50};
    constexpr std::chrono::milliseconds kHandoffMax{300};
    constexpr double kDegreesPerCount = 0.022;
    // TakeSteerStick: the mouse speed's smoothing time constant (seconds).
    constexpr double kSteerSmoothing = 0.04;

    // mnk_sensitivity (the SDK's cvar, read by name: it lives in the SDK DLL), re-read twice a second.
    double SteerSensitivity()
    {
      static std::atomic<double> value{5.0};
      static std::atomic<int64_t> read_at{0};
      const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now().time_since_epoch())
                              .count();
      if (now - read_at.load(std::memory_order_relaxed) >= 500)
      {
        read_at.store(now, std::memory_order_relaxed);
        const double v = std::atof(rex::cvar::GetFlagByName("mnk_sensitivity").c_str());
        if (std::isfinite(v) && v > 0.0)
        {
          value.store(std::min(v, 100.0), std::memory_order_relaxed);
        }
      }
      return value.load(std::memory_order_relaxed);
    }
    // A stalled frame or a pointer teleport must not become one huge snap.
    constexpr double kMaxDegreesPerCall = 85.0;

    // pdz_mouse_steer_log: every mouse motion event the window sees (whoever owns the pointer), for the stick line.
    std::atomic<int64_t> g_motion_events{0}, g_motion_abs_x{0}, g_motion_abs_y{0};
    // pdz_mouse_steer_log: the synthesized stick (SteerStickForPad) over the current second.
    std::atomic<int64_t> g_syn_polls{0}, g_syn_nonzero{0}, g_syn_counts_x{0}, g_syn_counts_y{0};
    std::atomic<double> g_syn_sum{0.0}, g_syn_last_x{0.0}, g_syn_last_y{0.0};
    std::atomic<int> g_last_device{-1}; // 1 = keyboard / mouse, 0 = a controller, -1 = not decided yet

    // NoteSteeredLook: the last time something other than the player's view was steered, and what.
    constexpr std::chrono::milliseconds kSteerHold{250};
    std::atomic<int64_t> g_steered_ns{INT64_MIN / 2};
    std::atomic<uint32_t> g_steered_caller{0};
    std::atomic<uint32_t> g_steered_object{0};
    std::atomic<bool> g_steering{false};
    // The player whose own view the mouse aims (ApplyMouseLook: guest user 0, our view class): only his steering
    // counts (a bot gunner on a turret runs the same look update).
    std::atomic<uint32_t> g_human_player{0};
    std::atomic<int64_t> g_human_ns{INT64_MIN / 2}; // when it last looked on foot

    int64_t NowNs()
    {
      return std::chrono::duration_cast<std::chrono::nanoseconds>(
                 std::chrono::steady_clock::now().time_since_epoch())
          .count();
    }

    bool SteeringActive()
    {
      return NowNs() - g_steered_ns.load(std::memory_order_relaxed) <
             std::chrono::duration_cast<std::chrono::nanoseconds>(kSteerHold).count();
    }

    // The callers of sub_825B3AC0 (return addresses), by what they steer.
    const char *SteeredName(uint32_t caller)
    {
      switch (caller)
      {
      case 0x8253FA98: return "CamSpy";            // sub_8253F7D8 (the CamSpy, vtable 820BFA40)
      case 0x8258DF48: return "hardpoint turret";  // sub_8258CEA0
      case 0x82253588: return "guided rocket";     // sub_822533E8 (the rocket launcher's guided rocket)
      case 0x82675ECC: return "hovercraft";        // sub_82675B20
      case 0x8268659C: return "jetpack";           // sub_82685F80
      default: return "steered object";
      }
    }
  } // namespace

  namespace
  {
    bool LogDue(uint32_t key);
    uint32_t Be32(const uint8_t *base, uint32_t address);
  } // namespace

  void NoteSteeredLook(uint8_t *base, uint32_t owner, uint32_t object, uint32_t caller)
  {
    // Guest user 0's player only (the SDK merges keyboard and mouse into user 0). He is the owner (the CamSpy, a
    // turret, the guided rocket) or the steered view itself (the hovercraft and the jetpack turn his own view with
    // the vehicle as the owner).
    const auto user_of = [base](uint32_t object_address) -> int64_t {
      if (object_address < 0x10000u)
      {
        return -1;
      }
      const uint32_t input = Be32(base, object_address + 6560);
      return input >= 0x10000u ? int64_t(int32_t(Be32(base, input + 1680))) : -1;
    };
    const int64_t owner_user = user_of(owner), view_user = user_of(object);
    const uint32_t player = owner_user == 0 ? owner : view_user == 0 ? object : 0;
    // While he looks on foot (within the last second) only his own steering counts; a level that starts in a
    // vehicle has no such player yet, and then user 0 is enough.
    const bool human_known = NowNs() - g_human_ns.load(std::memory_order_relaxed) < 1000000000LL;
    if (!player || (human_known && player != g_human_player.load(std::memory_order_relaxed)))
    {
      if (SteerLogOn() && LogDue(caller ^ 1u))
      {
        REXLOG_INFO("pdz_mouse: steer log: caller {:08X} ({}) not taken: owner {:08X} user {} view {:08X} user {} "
                    "(human player {:08X}, known {})",
                    caller, SteeredName(caller), owner, owner_user, object, view_user,
                    g_human_player.load(std::memory_order_relaxed), human_known ? "yes" : "no");
      }
      return;
    }
    g_steered_ns.store(NowNs(), std::memory_order_relaxed);
    g_steered_caller.store(caller, std::memory_order_relaxed);
    g_steered_object.store(object, std::memory_order_relaxed);
    MouseLook &look = MouseLook::Get();
    look.NoteGameplayLook();    // play, not a menu: the mouse buttons and the wheel keep their binds
    look.ConsumeForSteering(); // the pointer stays ours; its motion becomes the right stick (TakeSteerStick)
    if (!g_steering.exchange(true, std::memory_order_relaxed))
    {
      REXLOG_INFO("pdz_mouse: the {} takes the mouse (object {:08X}): the mouse drives the right stick "
                  "(mnk_sensitivity {:.2f}, mouse up = {})",
                  SteeredName(caller), object, SteerSensitivity(),
                  REXCVAR_GET(pdz_mouse_invert) ? "look down (pdz_mouse_invert)" : "look up");
    }
  }


  namespace
  {
    uint32_t Be32(const uint8_t *base, uint32_t address)
    {
      const uint8_t *p = base + address;
      return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
    }

    // Once a second per key (a caller address).
    bool LogDue(uint32_t key)
    {
      struct Slot
      {
        std::atomic<uint32_t> key{0};
        std::atomic<int64_t> last{INT64_MIN / 2};
      };
      static Slot slots[16];
      const int64_t now = NowNs();
      for (Slot &s : slots)
      {
        uint32_t k = s.key.load(std::memory_order_relaxed);
        if (k == 0)
        {
          s.key.compare_exchange_strong(k, key);
          k = s.key.load(std::memory_order_relaxed);
        }
        if (k == key)
        {
          int64_t last = s.last.load(std::memory_order_relaxed);
          return now - last >= 1000000000LL && s.last.compare_exchange_strong(last, now);
        }
      }
      return false;
    }

    // What an object is: its vtable, and its input block's user index if it has one (-1 none).
    std::string Describe(const uint8_t *base, uint32_t object)
    {
      if (object < 0x10000u)
      {
        return fmt::format("{:08X}", object);
      }
      const uint32_t vtbl = Be32(base, object);
      const bool player_class = vtbl >= GameConstants::kImageBase &&
                                vtbl < GameConstants::kImageBase + GameConstants::kImageSize &&
                                Be32(base, vtbl + 744) == 0x8259B6D8; // third_person.cpp IsPlayer
      int32_t user = -1;
      if (player_class)
      {
        const uint32_t input = Be32(base, object + 6560);
        user = input >= 0x10000u ? int32_t(Be32(base, input + 1680)) : -2;
      }
      return fmt::format("{:08X} (vtbl {:08X}{})", object, vtbl,
                         player_class ? fmt::format(", player class, user {}", user) : std::string(", not a player"));
    }
  } // namespace

  void LookProbeBegin(uint8_t *base, uint32_t owner, uint32_t view, uint32_t state, uint32_t caller, LookProbe &p)
  {
    p.armed = false;
    if (!REXCVAR_GET(pdz_mouse_steer_log) || view < 0x10000u || !LogDue(caller))
    {
      return;
    }
    p.armed = true;
    p.owner = owner;
    p.view = view;
    p.state = state;
    p.caller = caller;
    std::memcpy(p.before, base + view, LookProbe::kBytes);
  }

  void LookProbeEnd(uint8_t *base, LookProbe &p)
  {
    if (!p.armed)
    {
      return;
    }
    std::string changed;
    int n = 0;
    for (uint32_t off = 0; off + 4 <= LookProbe::kBytes && n < 24; off += 4)
    {
      if (std::memcmp(p.before + off, base + p.view + off, 4) != 0)
      {
        const uint32_t a = (uint32_t(p.before[off]) << 24) | (uint32_t(p.before[off + 1]) << 16) |
                           (uint32_t(p.before[off + 2]) << 8) | p.before[off + 3];
        const uint32_t b = Be32(base, p.view + off);
        float fa, fb;
        std::memcpy(&fa, &a, 4);
        std::memcpy(&fb, &b, 4);
        changed += fmt::format(" +{}: {:.4g}->{:.4g}", off, fa, fb);
        ++n;
      }
    }
    // The view's yaw / pitch through its own getters (vtable +24 / +28, as OurView's): each is normally
    // 'lfs f1,D(r3); blr', so D is read from the guest code and the value from the view.
    std::string angles;
    {
      static std::atomic<float> last_yaw{0.0f}, last_pitch{0.0f};
      const uint32_t vtbl = Be32(base, p.view);
      float v[2] = {0.0f, 0.0f};
      bool ok[2] = {false, false};
      std::string how;
      for (int i = 0; i < 2; ++i)
      {
        const uint32_t fn = vtbl >= GameConstants::kImageBase &&
                                    vtbl < GameConstants::kImageBase + GameConstants::kImageSize
                                ? Be32(base, vtbl + 24 + 4 * i)
                                : 0;
        const uint32_t insn = fn >= GameConstants::kImageBase &&
                                      fn < GameConstants::kImageBase + GameConstants::kImageSize
                                  ? Be32(base, fn)
                                  : 0;
        // lfs (opcode 48) f1 from r3 + D
        if ((insn >> 26) == 48 && ((insn >> 21) & 31) == 1 && ((insn >> 16) & 31) == 3)
        {
          const int32_t d = int16_t(insn & 0xFFFF);
          const uint32_t b = Be32(base, p.view + uint32_t(d));
          std::memcpy(&v[i], &b, 4);
          ok[i] = true;
          how += fmt::format("{}{:08X} +{}", i ? ", " : "", fn, d);
        }
        else
        {
          how += fmt::format("{}{:08X} insn {:08X}", i ? ", " : "", fn, insn);
        }
      }
      if (ok[0] || ok[1])
      {
        angles = fmt::format(" | view yaw {:.2f} (moved {:+.2f} since the last line) pitch {:.2f} ({:+.2f}) [getters {}]",
                             v[0], v[0] - last_yaw.load(), v[1], v[1] - last_pitch.load(), how);
        last_yaw.store(v[0]);
        last_pitch.store(v[1]);
      }
      else
      {
        angles = fmt::format(" | view yaw / pitch getters not simple loads [{}]", how);
      }
    }
    REXLOG_INFO("pdz_mouse: steer log: caller {:08X} ({}) owner {} view {} state {:08X} (view +{}){} | CamSpy +1571 {} "
                "| steering {} | mnk mouse {} menu {} | changed:{}",
                p.caller, SteeredName(p.caller), Describe(base, p.owner), Describe(base, p.view), p.state,
                int64_t(p.state) - int64_t(p.view), angles, p.caller == 0x8253FA98 ? int(base[p.view + 1571]) : -1,
                SteeringActive() ? "yes" : "no", rex::input::mnk::IsMouseLookActive() ? "on" : "off (claimed)",
                rex::input::mnk::IsMenuMode() ? "on" : "off", changed.empty() ? " nothing" : changed);
  }

  bool SteerLogOn() { return REXCVAR_GET(pdz_mouse_steer_log); }

  void LogAimHook(const uint8_t *base, uint32_t player, uint32_t view, bool our_view)
  {
    if (!SteerLogOn() || !LogDue(0xFFFF0001u))
    {
      return;
    }
    float f[5];
    for (int i = 0; i < 3; ++i)
    {
      const uint32_t b = Be32(base, player + 48 + 4 * i);
      std::memcpy(&f[i], &b, 4);
    }
    for (int i = 0; i < 2; ++i)
    {
      const uint32_t b = Be32(base, view + 5744 + 4 * i);
      std::memcpy(&f[3 + i], &b, 4);
    }
    REXLOG_INFO("pdz_mouse: steer log: aim hook player {} view {} | pos {:.0f},{:.0f},{:.0f} yaw {:.1f} pitch {:.1f} | "
                "our view {} | steering {} | mnk mouse {} menu {}",
                Describe(base, player), Describe(base, view), f[0], f[1], f[2], f[3], f[4], our_view ? "yes" : "no",
                SteeringActive() ? "yes" : "no", rex::input::mnk::IsMouseLookActive() ? "on" : "off (claimed)",
                rex::input::mnk::IsMenuMode() ? "on" : "off");
  }

  void SteerStickForPad(double &x, double &y)
  {
    const MouseLook::SteerStick st = MouseLook::Get().TakeSteerStick(SteeringActive());
    x = st.x;
    y = st.y;
    if (SteerLogOn())
    {
      g_syn_polls.fetch_add(1, std::memory_order_relaxed);
      g_syn_nonzero.fetch_add(st.x != 0.0 || st.y != 0.0, std::memory_order_relaxed);
      g_syn_counts_x.fetch_add(int64_t(std::fabs(st.counts_x)), std::memory_order_relaxed);
      g_syn_counts_y.fetch_add(int64_t(std::fabs(st.counts_y)), std::memory_order_relaxed);
      g_syn_sum.store(g_syn_sum.load(std::memory_order_relaxed) + std::max(std::fabs(st.x), std::fabs(st.y)),
                      std::memory_order_relaxed);
      g_syn_last_x.store(st.x, std::memory_order_relaxed);
      g_syn_last_y.store(st.y, std::memory_order_relaxed);
    }
  }

  void LogPadStick(int32_t raw_x, int32_t raw_y, int32_t out_x, int32_t out_y)
  {
    if (!SteerLogOn())
    {
      return;
    }
    // Per-second statistics: how many polls carried a deflection (a stick that flickers between 0 and a value
    // reads very differently from a held one).
    static std::mutex mu;
    static int64_t start = 0;
    static int polls = 0, raw_nonzero = 0, out_nonzero = 0, max_raw = 0;
    static double sum_raw = 0.0, sum_out = 0.0;
    std::lock_guard lock(mu);
    const int64_t now = NowNs();
    if (start == 0)
    {
      start = now;
    }
    ++polls;
    const int raw = std::max(std::abs(raw_x), std::abs(raw_y)), out = std::max(std::abs(out_x), std::abs(out_y));
    raw_nonzero += raw != 0;
    out_nonzero += out != 0;
    max_raw = std::max(max_raw, raw);
    sum_raw += raw;
    sum_out += out;
    if (now - start < 1000000000LL)
    {
      return;
    }
    if (raw_nonzero || out_nonzero || g_motion_events.load() > 0 || g_syn_nonzero.load() > 0)
    {
      const int64_t sp = std::max<int64_t>(1, g_syn_polls.load());
      REXLOG_INFO("pdz_mouse: steer log: mouse as the right stick: {} of {} polls with a deflection (mean {:.3f}, last "
                  "{:+.3f},{:+.3f}), motion taken |dx| {} |dy| {} counts | claim {}",
                  g_syn_nonzero.load(), g_syn_polls.load(), g_syn_sum.load() / double(sp), g_syn_last_x.load(),
                  g_syn_last_y.load(), g_syn_counts_x.load(), g_syn_counts_y.load(), MouseLook::Get().ClaimState());
      REXLOG_INFO("pdz_mouse: steer log: pad 0 right stick over {} polls: raw nonzero in {} (mean |v| {:.0f}, max {}), "
                  "filtered nonzero in {} (mean |v| {:.0f}); last raw {:+d},{:+d} -> {:+d},{:+d} | mouse motion this "
                  "second: {} events, |dx| {} |dy| {} counts | steering {} | mnk mouse {}",
                  polls, raw_nonzero, sum_raw / polls, max_raw, out_nonzero, sum_out / polls, raw_x, raw_y, out_x,
                  out_y, g_motion_events.load(), g_motion_abs_x.load(), g_motion_abs_y.load(),
                  SteeringActive() ? "yes" : "no", rex::input::mnk::IsMouseLookActive() ? "on" : "off (claimed)");
    }
    g_motion_events.store(0);
    g_motion_abs_x.store(0);
    g_motion_abs_y.store(0);
    g_syn_polls.store(0);
    g_syn_nonzero.store(0);
    g_syn_counts_x.store(0);
    g_syn_counts_y.store(0);
    g_syn_sum.store(0.0);
    start = now;
    polls = raw_nonzero = out_nonzero = max_raw = 0;
    sum_raw = sum_out = 0.0;
  }

  void PumpTestMotion()
  {
#ifdef _WIN32
    const double rate_x = REXCVAR_GET(pdz_mouse_test_motion), rate_y = REXCVAR_GET(pdz_mouse_test_motion_y);
    if (rate_x == 0.0 && rate_y == 0.0)
    {
      return;
    }
    static std::mutex mu;
    static int64_t last = 0;
    static double carry_x = 0.0, carry_y = 0.0;
    static int64_t pushed = 0, last_log = 0;
    std::lock_guard lock(mu);
    const int64_t now = NowNs();
    if (last == 0)
    {
      last = now;
      return;
    }
    const double dt = std::min(double(now - last) / 1e9, 0.1);
    last = now;
    carry_x += rate_x * dt;
    carry_y += rate_y * dt;
    const double dx = std::trunc(carry_x), dy = std::trunc(carry_y);
    if (dx == 0.0 && dy == 0.0)
    {
      return;
    }
    carry_x -= dx;
    carry_y -= dy;
    // A relative move through the OS, as a physical mouse would send it (raw input and the pointer alike): only
    // while one of our windows is in front, so nothing else on the desktop gets it.
    DWORD fg_pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &fg_pid);
    const bool front = fg_pid == GetCurrentProcessId();
    bool ok = false;
    if (front)
    {
      INPUT in{};
      in.type = INPUT_MOUSE;
      in.mi.dx = LONG(dx);
      in.mi.dy = LONG(dy);
      in.mi.dwFlags = MOUSEEVENTF_MOVE;
      ok = SendInput(1, &in, sizeof(in)) == 1;
    }
    if (ok)
    {
      pushed += int64_t(std::fabs(dx) + std::fabs(dy));
    }
    if (now - last_log >= 2000000000LL)
    {
      last_log = now;
      REXLOG_INFO("pdz_mouse: test motion: {:.0f},{:.0f} counts/s sent as relative mouse moves (SendInput; {} counts "
                  "so far{}) | steering {} | mnk mouse {}",
                  rate_x, rate_y, pushed,
                  !front ? ", NOT SENT: the game window isn't in front"
                  : ok   ? ""
                         : ", SendInput refused",
                  SteeringActive() ? "yes" : "no", rex::input::mnk::IsMouseLookActive() ? "on" : "off (claimed)");
    }
#endif
  }

  bool SteeringInvertY()
  {
    return REXCVAR_GET(pdz_mouse_invert) && SteeringActive() && MouseIsLastDevice();
  }

  bool MouseIsLastDevice()
  {
    const int64_t mnk = rex::input::mnk::LastActivityNs();
    const int64_t pad = rex::input::LastPadActivityNs();
    const int now = mnk != 0 && mnk > pad ? 1 : 0;
    if (g_last_device.exchange(now, std::memory_order_relaxed) != now)
    {
      REXLOG_INFO("pdz_mouse: last device used: {} ({})", now ? "keyboard / mouse" : "controller",
                  !REXCVAR_GET(pdz_mouse_aim_assist_off) ? "aim help unchanged: pdz_mouse_aim_assist_off is off"
                  : now ? "auto-aim and aim assist off"
                        : "auto-aim and aim assist as the game has them");
    }
    return now == 1;
  }

  MouseLook &MouseLook::Get()
  {
    static MouseLook instance;
    return instance;
  }

  MouseLook::~MouseLook()
  {
    {
      std::lock_guard lock(mu_);
      watchdog_stop_ = true;
    }
    watchdog_cv_.notify_all();
    if (watchdog_.joinable())
    {
      watchdog_.join();
    }
  }

  void MouseLook::Attach(rex::ui::Window *window, rex::ui::ImGuiDrawer *imgui)
  {
    if (!window)
    {
      return;
    }
    {
      std::lock_guard lock(mu_);
      if (window_)
      {
        return;
      }
      window_ = window;
      ui_ = &window->app_context();
      imgui_ = imgui;
      has_focus_ = true;
    }
    rex::input::mnk::SetMenuMode(true); // the front end first
    window->AddInputListener(this, 0);
    window->AddListener(this);
    watchdog_ = std::thread([this] { WatchdogLoop(); });
  }

  bool MouseLook::Enabled() const { return REXCVAR_GET(pdz_mouse_look); }

  void MouseLook::NoteGameplayLook()
  {
    {
      std::lock_guard lock(mu_);
      last_gameplay_ = Clock::now();
    }
    rex::input::mnk::SetMenuMode(false);
  }

  void MouseLook::ConsumeForSteering()
  {
    UiTask task;
    {
      std::lock_guard lock(mu_);
      const auto now = Clock::now();
      // Not gated on pdz_mouse_look: the steered things always get the mouse this way.
      const bool want = window_ && has_focus_ && !OverlayWantsMouse();
      if (!want)
      {
        last_consume_ = now;
        task = BeginReleaseLocked();
      }
      else if (AdvanceClaimLocked(now, task))
      {
        steer_dx_ += pending_dx_;
        steer_dy_ += pending_dy_;
        pending_dx_ = pending_dy_ = 0.0;
      }
    }
    Post(std::move(task));
  }

  MouseLook::SteerStick MouseLook::TakeSteerStick(bool steering)
  {
    std::lock_guard lock(mu_);
    const auto now = Clock::now();
    SteerStick out;
    const double dt = std::clamp(std::chrono::duration<double>(now - last_stick_).count(), 1e-4, 0.1);
    last_stick_ = now;
    out.counts_x = steer_dx_;
    out.counts_y = steer_dy_;
    steer_dx_ = steer_dy_ = 0.0;
    if (!steering)
    {
      steer_vx_ = steer_vy_ = 0.0;
      return out;
    }
    // The mouse's speed (counts per second), smoothed over kSteerSmoothing so a poll without a motion event in it
    // doesn't drop the stick to zero (a stick that flickers between 0 and a value turns far less: the look's
    // acceleration starts again each time).
    const double alpha = 1.0 - std::exp(-dt / kSteerSmoothing);
    steer_vx_ += (out.counts_x / dt - steer_vx_) * alpha;
    steer_vy_ += (out.counts_y / dt - steer_vy_) * alpha;
    // mnk's own scale (mnk_sensitivity x 200 per count per 120 Hz poll), as a rate: mnk_sensitivity 5 = a full
    // stick at about 3900 counts per second.
    const double per_count_per_s = SteerSensitivity() * 200.0 / 120.0 / 32767.0;
    double fx = std::clamp(steer_vx_ * per_count_per_s, -1.0, 1.0);
    double fy = std::clamp(-steer_vy_ * per_count_per_s, -1.0, 1.0); // pointer y grows downward: mouse up = stick up
    if (std::fabs(fx) < 0.003)
    {
      fx = 0.0;
    }
    if (std::fabs(fy) < 0.003)
    {
      fy = 0.0;
    }
    out.x = fx;
    out.y = fy;
    out.engaged = phase_ == Phase::kEngaged;
    return out;
  }

  const char *MouseLook::ClaimState() const
  {
    std::lock_guard lock(mu_);
    switch (phase_)
    {
    case Phase::kIdle: return "idle (mnk has the mouse)";
    case Phase::kHandoff: return "handoff (mnk letting go)";
    case Phase::kEngaging: return "engaging";
    case Phase::kEngaged: return captured_ ? "engaged (pointer locked, relative mode, ours)" : "engaged (not captured)";
    case Phase::kReleasing: return "releasing";
    }
    return "?";
  }

  void MouseLook::OnMouseMove(rex::ui::MouseEvent &e)
  {
    if (SteerLogOn())
    {
      const int64_t px = std::llabs(int64_t(e.dx())), py = std::llabs(int64_t(e.dy()));
      g_motion_events.fetch_add(1, std::memory_order_relaxed);
      g_motion_abs_x.fetch_add(px, std::memory_order_relaxed);
      g_motion_abs_y.fetch_add(py, std::memory_order_relaxed);
    }
    std::lock_guard lock(mu_);
    if (phase_ != Phase::kEngaged)
    {
      return;
    }
    pending_dx_ += e.dx();
    pending_dy_ += e.dy();
  }

  void MouseLook::OnLostFocus(rex::ui::UISetupEvent &)
  {
    UiTask task;
    {
      std::lock_guard lock(mu_);
      has_focus_ = false;
      task = BeginReleaseLocked();
    }
    Post(std::move(task));
  }

  void MouseLook::OnGotFocus(rex::ui::UISetupEvent &)
  {
    std::lock_guard lock(mu_);
    has_focus_ = true;
  }

  void MouseLook::OnClosing(rex::ui::UIEvent &)
  {
    bool give_back = false;
    {
      std::lock_guard lock(mu_);
      give_back = phase_ != Phase::kIdle;
      phase_ = Phase::kIdle;
      captured_ = false;
      window_ = nullptr;
      ui_ = nullptr;
      watchdog_stop_ = true;
    }
    watchdog_cv_.notify_all();
    if (give_back)
    {
      rex::input::mnk::SetMouseLookActive(true);
    }
  }

  bool MouseLook::OverlayWantsMouse() const { return imgui_ && imgui_->GetIO().WantCaptureMouse; }

  MouseLook::UiTask MouseLook::BeginEngageLocked()
  {
    phase_ = Phase::kEngaging;
    return [this] {
      rex::ui::Window *window = nullptr;
      {
        std::lock_guard lock(mu_);
        if (phase_ != Phase::kEngaging)
        {
          return; // released again before this ran
        }
        window = window_;
      }
      if (!window)
      {
        return;
      }
      // Window state is touched on the UI thread only, and outside the lock.
      const auto visibility = window->GetCursorVisibility();
      const auto restore_to = visibility == rex::ui::Window::CursorVisibility::kHidden
                                  ? rex::ui::Window::CursorVisibility::kVisible
                                  : visibility;
      window->SetCursorVisibility(rex::ui::Window::CursorVisibility::kHidden);
      window->CaptureMouse();
      if (!window->SetRelativeMouseMode(true))
      {
        REXLOG_WARN("pdz_mouse: this window backend has no pointer lock; mouse motion is taken from raw pointer "
                    "deltas without re-centring");
      }
      std::lock_guard lock(mu_);
      captured_ = true;
      saved_cursor_ = restore_to;
      pending_dx_ = pending_dy_ = 0.0;
      if (phase_ == Phase::kEngaging)
      {
        phase_ = Phase::kEngaged;
      }
    };
  }

  MouseLook::UiTask MouseLook::BeginReleaseLocked()
  {
    if (phase_ == Phase::kIdle || phase_ == Phase::kReleasing)
    {
      return {};
    }
    phase_ = Phase::kReleasing;
    return [this] {
      rex::ui::Window *window = nullptr;
      bool captured = false;
      auto restore_to = rex::ui::Window::CursorVisibility::kVisible;
      {
        std::lock_guard lock(mu_);
        window = window_;
        captured = captured_;
        restore_to = saved_cursor_;
      }
      if (captured && window)
      {
        window->SetRelativeMouseMode(false);
        window->ReleaseMouse();
        window->SetCursorVisibility(restore_to);
      }
      {
        std::lock_guard lock(mu_);
        captured_ = false;
        pending_dx_ = pending_dy_ = 0.0;
        phase_ = Phase::kIdle;
      }
      // Only now: mnk re-captures on its next poll, after the pointer is free.
      rex::input::mnk::SetMouseLookActive(true);
    };
  }

  void MouseLook::Post(UiTask task)
  {
    if (!task)
    {
      return;
    }
    rex::ui::WindowedAppContext *ui;
    {
      std::lock_guard lock(mu_);
      ui = ui_;
    }
    if (ui && ui->CallInUIThreadDeferred(std::move(task)))
    {
      return;
    }
    {
      std::lock_guard lock(mu_);
      phase_ = Phase::kIdle;
      captured_ = false;
    }
    rex::input::mnk::SetMouseLookActive(true);
  }

  void MouseLook::WatchdogLoop()
  {
    std::unique_lock lock(mu_);
    while (!watchdog_stop_)
    {
      watchdog_cv_.wait_for(lock, kWatchdogPeriod);
      if (watchdog_stop_)
      {
        break;
      }
      const auto now = Clock::now();
      if (now - last_gameplay_ > kConsumerTimeout)
      {
        rex::input::mnk::SetMenuMode(true); // no gameplay look: menus, cutscenes, loading
      }
      if (phase_ != Phase::kIdle && phase_ != Phase::kReleasing && now - last_consume_ > kConsumerTimeout)
      {
        UiTask task = BeginReleaseLocked();
        lock.unlock();
        Post(std::move(task));
        lock.lock();
      }
    }
  }

  bool MouseLook::AdvanceClaimLocked(Clock::time_point now, UiTask &task)
  {
    last_consume_ = now;
    switch (phase_)
    {
    case Phase::kIdle:
      // Claim: mnk stops mapping the mouse to the stick and releases its own capture on its next poll.
      rex::input::mnk::SetMouseLookActive(false);
      phase_ = Phase::kHandoff;
      handoff_since_ = now;
      break;
    case Phase::kHandoff:
    {
      const auto waited = now - handoff_since_;
      const bool mnk_let_go = window_->GetCursorVisibility() != rex::ui::Window::CursorVisibility::kHidden;
      if (waited >= kHandoffMin && (mnk_let_go || waited >= kHandoffMax))
      {
        task = BeginEngageLocked();
      }
      break;
    }
    case Phase::kEngaging:
    case Phase::kReleasing:
      break;
    case Phase::kEngaged:
      return true;
    }
    return false;
  }

  std::optional<Turn> MouseLook::Consume(double extra_scale)
  {
    UiTask task;
    std::optional<Turn> result;
    {
      std::lock_guard lock(mu_);
      const auto now = Clock::now();
      const bool want = REXCVAR_GET(pdz_mouse_look) && window_ && has_focus_ && !OverlayWantsMouse();
      if (!want)
      {
        last_consume_ = now;
        task = BeginReleaseLocked();
      }
      else if (AdvanceClaimLocked(now, task))
      {
        result = TakeTurnLocked(extra_scale);
      }
    }
    Post(std::move(task));
    return result;
  }

  Turn MouseLook::TakeTurnLocked(double extra_scale)
  {
    const double dx = pending_dx_;
    const double dy = pending_dy_;
    pending_dx_ = pending_dy_ = 0.0;
    const double degrees_per_count = REXCVAR_GET(pdz_mouse_sensitivity) * kDegreesPerCount * extra_scale;
    // Pointer y grows downward: + = look down.
    const double y_sign = REXCVAR_GET(pdz_mouse_invert) ? -1.0 : 1.0;
    const double yaw = dx * degrees_per_count;
    const double pitch = dy * degrees_per_count * y_sign;
    if (!std::isfinite(yaw) || !std::isfinite(pitch))
    {
      return Turn{0.0, 0.0};
    }
    return Turn{std::clamp(yaw, -kMaxDegreesPerCall, kMaxDegreesPerCall),
                std::clamp(pitch, -kMaxDegreesPerCall, kMaxDegreesPerCall)};
  }
} // namespace pdz::mouse

namespace
{
  constexpr uint32_t kViewYaw = 5744;
  constexpr uint32_t kViewPitch = 5748;
  constexpr uint32_t kVtblGetYaw = 24;
  constexpr uint32_t kVtblGetPitch = 28;
  constexpr uint32_t kGetYawFunc = 0x8254E0E0;   // [8253E9A8]
  constexpr uint32_t kGetPitchFunc = 0x8254E0E8; // [8253E9B0]
  constexpr uint32_t kPlayerInput = 6560;
  constexpr uint32_t kInputUserIndex = 1680; // [1152]
  constexpr uint32_t kVtblZoom = 660;        // [656]
  constexpr uint32_t kZoomFunc = 0x825AC8E0; // [8259D0D8]
  constexpr uint32_t kPlayerZoomOut = 6812;
  constexpr double kBaseFovDegrees = 58.0;
  constexpr uint32_t kGameGlobals = 0x82D6BFDC; // [82D6B2B4]
  constexpr uint32_t kGlobalsLookTuning = 12;
  constexpr uint32_t kTuningPitchLimit = 64;
  constexpr float kFallbackPitchLimit = 85.0f;
  constexpr double kRadiansPerDegree = 3.14159265358979323846 / 180.0;

  uint32_t LoadBe32(const uint8_t *base, uint32_t address)
  {
    const uint8_t *p = base + address;
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
  }

  float LoadBeFloat(const uint8_t *base, uint32_t address)
  {
    const uint32_t bits = LoadBe32(base, address);
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
  }

  void StoreBeFloat(uint8_t *base, uint32_t address, float value)
  {
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    uint8_t *p = base + address;
    p[0] = uint8_t(bits >> 24);
    p[1] = uint8_t(bits >> 16);
    p[2] = uint8_t(bits >> 8);
    p[3] = uint8_t(bits);
  }

  bool InImage(uint32_t address)
  {
    return address >= GameConstants::kImageBase && address < GameConstants::kImageBase + GameConstants::kImageSize;
  }

  bool HasVirtual(const uint8_t *base, uint32_t object, uint32_t slot, uint32_t func)
  {
    if (object < 0x10000u)
    {
      return false;
    }
    const uint32_t vtbl = LoadBe32(base, object);
    return InImage(vtbl) && LoadBe32(base, vtbl + slot) == func;
  }

  double ZoomScale(const uint8_t *base, uint32_t player)
  {
    if (!HasVirtual(base, player, kVtblZoom, kZoomFunc))
    {
      return 1.0;
    }
    // pdz_fov_scale (fov_tuning.cpp) widens the unzoomed and the zoomed view alike: the ratio of the two is the zoom.
    const double unzoomed = kBaseFovDegrees * std::clamp(REXCVAR_GET(pdz_fov_scale), 0.5, 2.0);
    const double fov = unzoomed * (1.0 - LoadBeFloat(base, player + kPlayerZoomOut));
    if (!std::isfinite(fov) || fov < 0.5 || fov > 170.0)
    {
      return 1.0;
    }
    const double scale = std::tan(fov * 0.5 * kRadiansPerDegree) / std::tan(unzoomed * 0.5 * kRadiansPerDegree);
    return scale >= 0.999 ? 1.0 : std::clamp(scale, 0.02, 1.0);
  }

  float PitchLimit(const uint8_t *base)
  {
    const uint32_t globals = LoadBe32(base, kGameGlobals);
    const uint32_t tuning = globals >= 0x10000u ? LoadBe32(base, globals + kGlobalsLookTuning) : 0;
    const float limit = tuning >= 0x10000u ? LoadBeFloat(base, tuning + kTuningPitchLimit) : 0.0f;
    return limit > 1.0f && limit < 90.0f ? limit : kFallbackPitchLimit;
  }

  float WrapDegrees(float angle)
  {
    angle = std::fmod(angle, 360.0f);
    if (angle >= 180.0f)
    {
      angle -= 360.0f;
    }
    else if (angle < -180.0f)
    {
      angle += 360.0f;
    }
    return angle;
  }

  // The player on guest user 0 looking through a view whose angles we know (other view classes keep them elsewhere
  // and are left to the emulated stick).
  bool OurView(const uint8_t *base, uint32_t player, uint32_t view)
  {
    if (!HasVirtual(base, view, kVtblGetYaw, kGetYawFunc) || !HasVirtual(base, view, kVtblGetPitch, kGetPitchFunc))
    {
      return false;
    }
    const uint32_t input = player >= 0x10000u ? LoadBe32(base, player + kPlayerInput) : 0;
    return input >= 0x10000u && static_cast<int32_t>(LoadBe32(base, input + kInputUserIndex)) == 0;
  }

  void Turn(uint8_t *base, uint32_t view, double yaw_deg, double pitch_deg, bool log_test)
  {
    const float yaw_in = LoadBeFloat(base, view + kViewYaw);
    const float pitch_in = LoadBeFloat(base, view + kViewPitch);
    const float yaw_out = WrapDegrees(yaw_in - static_cast<float>(yaw_deg));
    // Clamped only in the direction of the motion: recoil may have pushed the pitch past the limit already, the mouse
    // must not snap it back.
    const float limit = PitchLimit(base);
    const float delta = static_cast<float>(pitch_deg);
    float pitch_out = pitch_in + delta;
    if (delta > 0.0f && pitch_out > limit)
    {
      pitch_out = std::max(pitch_in, limit);
    }
    else if (delta < 0.0f && pitch_out < -limit)
    {
      pitch_out = std::min(pitch_in, -limit);
    }
    StoreBeFloat(base, view + kViewYaw, yaw_out);
    StoreBeFloat(base, view + kViewPitch, pitch_out);
    if (log_test)
    {
      REXLOG_INFO("pdz_mouse: test: view {:08X} yaw {:.2f} -> {:.2f}, pitch {:.2f} -> {:.2f} (limit {:.1f})", view,
                  yaw_in, yaw_out, pitch_in, pitch_out, limit);
    }
  }

  void ApplyMouseLook(uint8_t *base, uint32_t player, uint32_t view, double dt)
  {
    auto &look = pdz::mouse::MouseLook::Get();
    if (!base)
    {
      return;
    }
    const bool our_view = OurView(base, player, view);
    pdz::mouse::LogAimHook(base, player, view, our_view);
    if (!our_view)
    {
      return;
    }
    look.NoteGameplayLook();
    pdz::mouse::g_human_player.store(player, std::memory_order_relaxed);
    pdz::mouse::g_human_ns.store(pdz::mouse::NowNs(), std::memory_order_relaxed);
    // The CamSpy, a turret, the guided rocket, the hovercraft or the jetpack is being steered (NoteSteeredLook): the
    // pointer stays ours and its motion becomes the right stick that reads (TakeSteerStick); the player's own view
    // isn't turned by it meanwhile.
    if (pdz::mouse::SteeringActive())
    {
      look.ConsumeForSteering();
      return;
    }
    if (pdz::mouse::g_steering.exchange(false, std::memory_order_relaxed))
    {
      REXLOG_INFO("pdz_mouse: the {} gives the mouse back: mouse aim on foot again",
                  pdz::mouse::SteeredName(pdz::mouse::g_steered_caller.load(std::memory_order_relaxed)));
    }
    {
      static double faced_yaw = 999.0, faced_pitch = 999.0;
      const double fy = REXCVAR_GET(pdz_mouse_test_face_yaw), fp = REXCVAR_GET(pdz_mouse_test_face_pitch);
      if (fy != faced_yaw && fy != 999.0)
      {
        StoreBeFloat(base, view + kViewYaw, WrapDegrees(float(fy)));
        REXLOG_INFO("pdz_mouse: test: view {:08X} yaw set to {:.1f}", view, fy);
      }
      if (fp != faced_pitch && fp != 999.0)
      {
        StoreBeFloat(base, view + kViewPitch, float(std::clamp(fp, -85.0, 85.0)));
        REXLOG_INFO("pdz_mouse: test: view {:08X} pitch set to {:.1f}", view, fp);
      }
      faced_yaw = fy;
      faced_pitch = fp;
    }
    const double test_yaw = REXCVAR_GET(pdz_mouse_test_turn), test_pitch = REXCVAR_GET(pdz_mouse_test_pitch);
    if (test_yaw != 0.0 || test_pitch != 0.0)
    {
      static auto last_log = std::chrono::steady_clock::now();
      const auto now = std::chrono::steady_clock::now();
      const bool log = now - last_log >= std::chrono::seconds(2);
      if (log)
      {
        last_log = now;
      }
      const double step = std::isfinite(dt) && dt > 0.0 && dt < 0.25 ? dt : 1.0 / 60.0;
      const double zoom = ZoomScale(base, player);
      Turn(base, view, test_yaw * step * zoom, test_pitch * step * zoom, log);
      return;
    }
    if (!look.Enabled())
    {
      return;
    }
    const auto turn = look.Consume(ZoomScale(base, player));
    if (turn && (turn->yaw != 0.0 || turn->pitch != 0.0))
    {
      Turn(base, view, turn->yaw, turn->pitch, false);
    }
  }
} // namespace

// sub_825AF590 [8259FD70](player, view, look state, f1, f2 = dt): the rotational aim assist, the last step of the
// look update.
REX_HOOK_RAW(sub_825AF590)
{
  ApplyMouseLook(base, ctx.r3.u32, ctx.r4.u32, ctx.f2.f64);
  if (REXCVAR_GET(pdz_mouse_aim_assist_off) && pdz::mouse::MouseIsLastDevice())
  {
    return; // no rotational aim assist with the mouse
  }
  __imp__sub_825AF590(ctx, base);
}

// pdz_mouse_steer_log (diagnostic): the CamSpy's own update and its factory.
REX_EXTERN(__imp__sub_8253F7D8);
REX_EXTERN(__imp__sub_82514F88);
REX_HOOK_RAW(sub_8253F7D8)
{
  const uint32_t camspy = ctx.r3.u32;
  if (pdz::mouse::SteerLogOn() && camspy >= 0x10000u)
  {
    static int64_t last = 0;
    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
    if (now - last >= 1000)
    {
      last = now;
      std::string bytes;
      for (uint32_t off = 1560; off < 1584; ++off)
      {
        bytes += fmt::format("{:02X}", base[camspy + off]);
      }
      REXLOG_INFO("pdz_mouse: steer log: CamSpy update {:08X} vtbl {:08X} owner(+1536) {:08X} +1520 {} +1571 {} | "
                  "+1560..1583 {} | f1 {:.4f}",
                  camspy, LoadBe32(base, camspy), LoadBe32(base, camspy + 1536), int(base[camspy + 1520]),
                  int(base[camspy + 1571]), bytes, ctx.f1.f64);
    }
  }
  __imp__sub_8253F7D8(ctx, base);
}

REX_HOOK_RAW(sub_82514F88)
{
  const uint32_t arg = ctx.r3.u32;
  __imp__sub_82514F88(ctx, base);
  if (pdz::mouse::SteerLogOn())
  {
    REXLOG_INFO("pdz_mouse: steer log: CamSpy created: {:08X} (factory arg {:08X})", ctx.r3.u32, arg);
  }
}
