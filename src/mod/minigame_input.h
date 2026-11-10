// minigame_input.h - the Locktopus (Rare's "lockbot" subgame) on the keyboard and mouse (pdz_fix_minigame_keyboard;
// 2026-11-08, docs/MOUSE_KEYBOARD.md "The Locktopus"). Title Update 3 addresses.
//
// The subgame's update (sub_826492E8) reads its stick as two floats at controller +44 / +48 (participant +1724 /
// +1728): Rare's filtered LEFT stick (sub_82275D30 writes +44 / +48 from the raw left stick +60 / +64 and +52 / +56
// from the right one, after the Sticks Swapped swap). A pin is a stick position (x, y over 128) with a radius from
// the difficulty (0.45 / 0.33 / 0.20); the stick has to sit within it for 1 s. Several pins are off the eight key
// directions or short of full deflection, so keys that snap the stick can't set them (minigame_input.cpp's header,
// docs/MOUSE_KEYBOARD.md). With the fix on, while the Locktopus is active for pad 0, after Rare's controller update,
// the fix writes a VIRTUAL stick to +44 / +48:
// - the left / right keys of either stick (A / D, Numpad 4 / 6) turn it round the circle (pdz_minigame_key_turn_s per
//   turn, Shift = pdz_minigame_key_slow of that), the up / down keys (W / S, Numpad 8 / 2) push it out and pull it in;
// - the mouse's motion (raw window events, whoever owns the pointer) moves it as a point (pdz_minigame_mouse_counts
//   per full deflection), so the mouse and the keys work together;
// - it stays where it was left (a pin can be held), starts at the centre with each lock, and follows a controller's
//   left stick while that is pushed;
// - a controller is never touched: nothing is written while its left stick is pushed, nor before a key or the mouse
//   has been used in the lock.
// Other stick-reading minigames (the demo kit's tile puzzle) keep the snapped direction of the keys held.
#pragma once

#include <cstdint>
#include <string>

namespace pdz::minigame
{
  // ---- Shared with the settings window's practice dial (src/setup/settings_window.cpp) ----

  // Which inputs steer the pick (pdz_minigame_inputs: "both", "mouse", "keys").
  enum class PickInputs
  {
    kBoth,
    kMouse,
    kKeys,
  };
  PickInputs PickInputsFromName(const std::string &name);
  const char *PickInputsName(PickInputs inputs);

  // The virtual stick (x right, y up, 1 = full deflection) as an angle and a deflection.
  struct Pick
  {
    double angle = 1.5707963267948966; // radians, 0 = right, up = pi / 2
    double radius = 0.0;
    bool used = false; // a key or the mouse steered it since the last reset
  };

  struct SteerTuning
  {
    double key_turn_s = 2.5;   // pdz_minigame_key_turn_s: seconds for a full turn with the left / right keys
    double key_slow = 0.4;     // pdz_minigame_key_slow: the keys' speed with Shift held
    double mouse_counts = 400; // pdz_minigame_mouse_counts: mouse counts from the centre to full deflection
    PickInputs inputs = PickInputs::kBoth;
  };
  SteerTuning LiveTuning(); // from the cvars

  // One step of the pick (the game's Locktopus and the practice dial both call this): kx / ky are the keys' stick
  // (right / up positive; only the sign counts), slow = Shift held, mdx / mdy the mouse's motion in counts (right /
  // down positive), dt seconds. Inputs the tuning leaves out are ignored. Returns true if it moved.
  bool SteerPick(Pick &pick, const SteerTuning &tuning, double kx, double ky, bool slow, double mdx, double mdy,
                 double dt);

  // Rare's pin rules (sub_826492E8, TU3): a pin is a stick position (x, y over 128); while the stick is within the
  // radius of it (a distance) the hold timer counts DOWN from 1 s by dt, outside it counts back UP by dt to 1 s; at 0
  // the pin sets and the next pin is current. The radius comes from the difficulty.
  constexpr double kPinHoldSeconds = 1.0;
  constexpr double kPinRadiusAgent = 0.45, kPinRadiusSecretAgent = 0.33, kPinRadiusPerfectAgent = 0.20;
  // A step of the hold: returns true when the pin sets (the hold reached 0).
  bool StepPinHold(double &hold, double stick_x, double stick_y, double pin_x, double pin_y, double radius,
                   double dt);

  // mnk_layout.cpp's sub_822748C8 hook, pad 0, after Rare's update: ctrl is the controller object (participant
  // +1680). Writes +44 / +48 while the Locktopus is active and the keyboard / mouse gives a direction.
  void OnControllerUpdated(uint8_t *base, uint32_t ctrl);

  // minigame_log.cpp's sub_8264ADE0 hook: the demo kit's tile puzzle is updating (it reads the same stick fields as a
  // digital pad). Keys bound to either stick move its cursor; the mouse is not used there.
  void NoteDemoKit(uint32_t subgame);
} // namespace pdz::minigame
