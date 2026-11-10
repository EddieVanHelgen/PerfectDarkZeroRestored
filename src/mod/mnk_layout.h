// mnk_layout.h - the keyboard and mouse follow the player's intent whatever in-game controller layout is chosen
// (pdz_mnk_follow_layout, on by default; 2026-11-08, docs/MOUSE_KEYBOARD.md "Controller layouts").
//
// The SDK's keyboard driver (mnk) presses stick directions: W / S / A / D are the left stick, the look keys (numpad
// 8 / 2 / 4 / 6) the right stick. Rare maps the two sticks onto move and look per player in the controller object at
// participant +1680 (sub_822748C8, once per frame): Controller Style Standard moves with the left stick and looks
// with the right one; Classic PD turns with the left stick's x and strafes with the right stick's x; Classic PD while
// aiming (LT) moves with the right stick and aims with the left one; Sticks Swapped swaps the two sticks first. So in
// Classic PD, A / D turned instead of strafing. With pdz_mnk_follow_layout the keyboard's stick directions are moved,
// in the game's XInputGetState, to the stick axis that does what the key says in the active layout: the keys strafe,
// walk, turn and look whatever the layout. A controller's own sticks are never changed. The mouse on foot turns the
// view directly (mouse_look.cpp) and so never depended on the layout; the mouse as a stick (CamSpy, turrets,
// vehicles) is routed the same way (SteerRoute), and the game's Look Inversion doesn't flip it (mouse up = look up,
// as on foot; pdz_mouse_invert still does).
#pragma once

#include <cstdint>

namespace rex::ui
{
  class Window;
}

namespace pdz::mnk_layout
{
  // Once, after the main window has opened: the key tracker (which keys are held, to tell the keyboard's stick
  // directions apart from a controller's in the merged pad state).
  void Attach(rex::ui::Window *window);

  // test_harness.cpp's XInputGetState hook, after the original (and before the input script's own pad): user 0's
  // merged state (XINPUT_STATE, big-endian, at guest address state). Moves the keyboard's stick directions.
  void OnPadState(uint8_t *base, uint32_t user, uint32_t state);

  // Where the mouse goes while it is a stick (look_tuning.cpp's pad filter, pad 0): for the look's x (turn) and y
  // (pitch) whether it is the left stick (slot 0) instead of the right one (slot 4), and whether its y is to be
  // flipped to undo the game's Look Inversion. Off / no layout known: both right, no flip (as before).
  struct SteerRoute
  {
    bool x_left = false;
    bool y_left = false;
    bool undo_game_invert = false;
  };
  SteerRoute RouteForSteering();

  // The keyboard driver's stick from the keys held at the last XInputGetState poll (lx, ly, rx, ry, INT16_MAX per
  // direction, as the SDK computes it; pdz_mnk_test_keys included). For the Locktopus on the keyboard
  // (minigame_input.cpp). Any thread.
  void LastKeyboardStick(int16_t out[4]);

  // Shift was held at that poll (pdz_mnk_test_keys included): the Locktopus' slow modifier. Any thread.
  bool LastKeyboardSlow();
} // namespace pdz::mnk_layout
