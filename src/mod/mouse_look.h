// mouse_look.h - mouse aim straight into the player's view angles (Title Update 3).
//
// Ported from the launch branch's V1.7 "Pure PC mouse" (src/mouse/, itself from GoldenEye007ReloadedRecomp on the same
// SDK) with TU3 addresses (2026-11-04, docs/MOUSE_KEYBOARD.md). The SDK's keyboard / mouse driver (mnk) turns the mouse
// into the right stick, which the game runs through its dead zone, response curve, acceleration and turn cap. While
// the gameplay look update runs, MouseLook takes the mouse from mnk (rex::input::mnk::SetMouseLookActive(false)),
// locks the pointer itself and hands the raw motion to the look hook (mouse_look_hook.cpp) as an angle. When the look
// update stops running (menus, cutscenes) a watchdog gives the mouse back to mnk, whose emulated stick takes over
// there, and puts mnk in menu mode (left click / Enter = A, right click / Esc = B, wheel = D-pad). While the player
// steers something else (the CamSpy, a turret, the guided rocket, the hovercraft, the jetpack: NoteSteeredLook)
// MouseLook keeps the pointer and its motion becomes pad 0's right stick in the pad filter (TakeSteerStick), and mnk
// stays in play mode.
#pragma once

#include <rex/cvar.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

REXCVAR_DECLARE(bool, pdz_mouse_aim_assist_off);

namespace rex::ui
{
  class ImGuiDrawer;
  class WindowedAppContext;
} // namespace rex::ui

namespace pdz::mouse
{
  // Degrees, in pointer direction: the hook maps them onto the game's angles.
  struct Turn
  {
    double yaw;   // + = turn right
    double pitch; // + = look down
  };

  // The keyboard / mouse produced input more recently than any controller (auto-aim and the aim assist are off then:
  // pdz_mouse_aim_assist_off). Logged once at each switch. Any thread.
  bool MouseIsLastDevice();

  // The look update (sub_825B3AC0, look_tuning.cpp's hook) steered something other than the player's own view for
  // guest user 0's player: the CamSpy, a hardpoint turret, the guided rocket (he is the owner), the hovercraft or the
  // jetpack (his own view, the vehicle the owner). Each reads the player's right stick. While one is steered (and for
  // kSteerHold after) MouseLook keeps the pointer as on foot, and its motion becomes pad 0's right stick in the pad
  // filter (SteerStickForPad), after the SDK's own stick dead zone. 2026-11-06 (logs 663 / 665): handing the mouse to
  // mnk's emulated stick instead gave no stick at all (mapping.toml right_stick_deadzone_percentage 0.2 swallowed it),
  // so the CamSpy couldn't be steered. mnk_sensitivity sets the scale as before, mouse up = stick up (pdz_mouse_invert
  // flips it: SteeringInvertY), and mnk stays out of menu mode (the mouse buttons and the wheel stay what they are in
  // play). One line when it takes / gives back the mouse. caller = the look update's return address (which it is).
  void NoteSteeredLook(uint8_t *base, uint32_t owner, uint32_t object, uint32_t caller);
  // Something is being steered (above) and pdz_mouse_invert is on and the keyboard / mouse is the last device used:
  // the pad filter flips pad 0's right stick y (look_tuning.cpp).
  bool SteeringInvertY();

  // pdz_mouse_steer_log (diagnostic, off): around each look update that isn't the player's own view, once a second
  // per caller, one line with the owner / view / state and which view floats the update changed. LookProbeBegin
  // before the original, LookProbeEnd after it.
  struct LookProbe
  {
    static constexpr uint32_t kBytes = 0x700;
    bool armed = false;
    uint32_t owner = 0, view = 0, state = 0, caller = 0;
    uint8_t before[kBytes];
  };
  void LookProbeBegin(uint8_t *base, uint32_t owner, uint32_t view, uint32_t state, uint32_t caller, LookProbe &p);
  void LookProbeEnd(uint8_t *base, LookProbe &p);
  bool SteerLogOn();
  void LogAimHook(const uint8_t *base, uint32_t player, uint32_t view, bool our_view);
  // pdz_mouse_steer_log: pad 0's right stick into / out of the pad filter, once a second while either is nonzero.
  void LogPadStick(int32_t raw_x, int32_t raw_y, int32_t out_x, int32_t out_y);
  // The pad filter (look_tuning.cpp), once per poll of pad 0's right stick: the mouse as that stick while something
  // is steered (MouseLook::TakeSteerStick), x / y in -1..1, 0 otherwise.
  void SteerStickForPad(double &x, double &y);
  // pdz_mouse_test_motion (testing tool): synthetic mouse motion pushed as real SDL events at that many counts per
  // second (x; pdz_mouse_test_motion_y for y). Call often (each look update); it keeps its own clock.
  void PumpTestMotion();

  class MouseLook final : public rex::ui::WindowInputListener, public rex::ui::WindowListener
  {
  public:
    static MouseLook &Get();

    MouseLook(const MouseLook &) = delete;
    MouseLook &operator=(const MouseLook &) = delete;

    // Once, after the main window has opened. imgui is optional: when given, the mouse is handed back while an
    // overlay wants it.
    void Attach(rex::ui::Window *window, rex::ui::ImGuiDrawer *imgui = nullptr);

    // pdz_mouse_look is on.
    bool Enabled() const;

    // The gameplay look update ran (every frame of play, mouse look on or off): keeps mnk out of menu mode.
    void NoteGameplayLook();

    // Something else than the player's view is being steered (NoteSteeredLook): keeps (or takes) the pointer as on
    // foot, but its motion is kept for TakeSteerStick instead of turning the player's view. Guest thread.
    void ConsumeForSteering();

    // The mouse as a right stick for the steered thing: x / y in -1..1 (y + = up), from the motion since the last
    // call, smoothed; zero (and the smoothing reset) when steering is false. Called by the pad filter once per poll
    // of pad 0's right stick (look_tuning.cpp). counts_* = the raw motion taken (diagnostics).
    struct SteerStick
    {
      double x = 0.0, y = 0.0;
      double counts_x = 0.0, counts_y = 0.0;
      bool engaged = false;
    };
    SteerStick TakeSteerStick(bool steering);

    // The hand-over state, for the logs.
    const char *ClaimState() const;

    // Called by the look hook once per look update (guest thread). Drives the hand-over from mnk and returns the
    // mouse turn since the last call, or nullopt while the pointer is not ours yet. extra_scale multiplies the turn
    // (zoom).
    std::optional<Turn> Consume(double extra_scale);

    // rex::ui::WindowInputListener
    void OnMouseMove(rex::ui::MouseEvent &e) override;

    // rex::ui::WindowListener
    void OnLostFocus(rex::ui::UISetupEvent &) override;
    void OnGotFocus(rex::ui::UISetupEvent &) override;
    void OnClosing(rex::ui::UIEvent &) override;

  private:
    MouseLook() = default;
    ~MouseLook();

    enum class Phase
    {
      kIdle,      // mnk owns the mouse
      kHandoff,   // mnk told to let go, waiting for it to do so
      kEngaging,  // capture request posted to the UI thread
      kEngaged,   // pointer locked, deltas flowing
      kReleasing, // release request posted to the UI thread
    };

    using Clock = std::chrono::steady_clock;
    using UiTask = std::function<void()>;

    UiTask BeginReleaseLocked();
    UiTask BeginEngageLocked();
    // The claim's state machine (idle -> handoff -> engaging -> engaged); true when engaged (motion flowing).
    bool AdvanceClaimLocked(Clock::time_point now, UiTask &task);
    Turn TakeTurnLocked(double extra_scale);
    bool OverlayWantsMouse() const;

    // Call without mu_ held.
    void Post(UiTask task);

    void WatchdogLoop();

    rex::ui::Window *window_ = nullptr;
    rex::ui::WindowedAppContext *ui_ = nullptr;
    rex::ui::ImGuiDrawer *imgui_ = nullptr;

    mutable std::mutex mu_;
    Phase phase_ = Phase::kIdle;
    bool has_focus_ = true;
    bool captured_ = false; // set by the UI-thread engage task
    rex::ui::Window::CursorVisibility saved_cursor_ = rex::ui::Window::CursorVisibility::kVisible;
    Clock::time_point handoff_since_{};
    Clock::time_point last_consume_{};
    Clock::time_point last_gameplay_{};

    double pending_dx_ = 0.0;
    double pending_dy_ = 0.0;
    // ConsumeForSteering / TakeSteerStick: motion kept for the steered thing, and its smoothed speed (counts / s).
    double steer_dx_ = 0.0, steer_dy_ = 0.0, steer_vx_ = 0.0, steer_vy_ = 0.0;
    Clock::time_point last_stick_{};

    std::thread watchdog_;
    std::condition_variable watchdog_cv_;
    bool watchdog_stop_ = false;
  };
} // namespace pdz::mouse
