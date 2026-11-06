# Mouse and keyboard (2026-11-04)

A basic, solid layer over a game designed for the controller: the keyboard still acts as a pad (the SDK's mnk
driver), the mouse aims directly. Settings: `settings/mapping.toml`.

## Mouse aim (`src/mod/mouse_look.cpp`)

- **Where it hooks:** the rotational aim assist, the last step of the TU3 look update (`sub_825AF590`, launch
  `8259FD70`). The mouse turn goes straight into the view's yaw / pitch (+5744 / +5748), skipping the stick's dead
  zone, curve, acceleration, fast look and turn cap.
- **Ported from:** the launch branch's V1.7 "Pure PC mouse" (src/mouse/), re-targeted to TU3:
  - the getters `sub_8254E0E0` / `sub_8254E0E8`;
  - the input block +6560, with the user index at +1680 (launch +1152);
  - zoom at vtable +660, `sub_825AC8E0`;
  - the pitch limit at `*(*(82D6BFDC) + 12) + 64`, the one the look model clamps with.
- **Sensitivity and invert:** `pdz_mouse_sensitivity` (0.022 deg x this per count; default 2.0), `pdz_mouse_invert`
  (up / down).
- **Zoom:** the turn is scaled by tan(fov / 2) / tan(29 deg), so zoomed aiming is slower in proportion to the field
  of view. The game's pitch limit (85 deg) applies, in the direction of the motion only (recoil past it isn't
  undone).
- **Where it applies:** only the player on guest user 0, on foot. Where the look update doesn't run (menus,
  cutscenes) the pointer is handed back and the mouse drives the emulated right stick (`mnk_sensitivity`), as before.
- **Steering something else (2026-11-06):** the look update `sub_825B3AC0(owner, view, state, dt)` also steers other
  objects with the player's right stick. Its six callers (return address -> what):
  - `825A7A6C` (`sub_825A79B8`): on foot, owner = view = the player, state at player +0xF50;
  - `8253FA98` (`sub_8253F7D8`): the **CamSpy** (vtable `820BFA40`, its constructor `sub_8253E508` loads
    `aid_xml_gadgetparams_CamSpy`; look state at +1384);
  - `8258DF48` (`sub_8258CEA0`): a hardpoint turret;
  - `82253588` (`sub_822533E8`): the rocket launcher's guided rocket;
  - `82675ECC` (`sub_82675B20`): the hovercraft;
  - `8268659C` (`sub_82685F80`): the jetpack.

  Owner and view: the CamSpy, a turret and the guided rocket have the player as the owner; the hovercraft and the
  jetpack turn the player's own view with the vehicle as the owner (state at player +3676).

  **The fix (2026-11-06, tested by hand in log 665):** while any of the five steers for user 0's player (and for
  250 ms after; `mouse_look.h` `NoteSteeredLook`, called from `look_tuning.cpp`'s look hook), the mouse claim stays
  on, as on foot, and the mouse motion becomes pad 0's right stick in the pad filter `sub_822733D0`
  (`SteerStickForPad`). The deflection follows the mouse speed, smoothed over 40 ms (a poll without a motion event
  doesn't drop it to zero). It is scaled like mnk's emulated stick (`mnk_sensitivity` 5: a full stick at about 3,900
  counts per second), and it starts just past the dead zone the pad filter uses, so slow motion turns too. A
  controller pushed further wins. Mouse up = look up (`pdz_mouse_invert` flips pad 0's right stick y there). mnk stays
  out of menu mode (left click still fires, Esc / P still pause, the wheel still switches weapons). One
  `pdz_mouse: the CamSpy takes the mouse` / `gives the mouse back` line each way. Only the player whose own view the
  mouse aims counts (a bot gunner on a turret runs the same update).

  **Why the first fix (3a123e6) failed:** it handed the mouse back to mnk while something was steered. mnk did get
  the motion (about 120 events a second in log 664), but the stick stayed at 0. The SDK applies
  `right_stick_deadzone_percentage` (0.2 in `settings/mapping.toml`, 6,553) after merging mnk's stick, and mnk's
  per-poll stick (count x `mnk_sensitivity` x 200, drained at every poll) rarely got past it. The hand test's log 663 showed
  the same: mouse moved, no stick input, the CamSpy at yaw 60.00. Only the numpad look keys (a full stick) turned it.
  Now the stick is written after that dead zone, by the game side; no SDK change.

  **Evidence (log 665, a hand test in Datacore, mouse only):** the window got 700-1,000 motion events a second. The claim
  stayed engaged (pointer locked, relative mode). The mouse stick had a deflection in up to 121 of 121 polls (pad
  filter output up to +-32767; the SDK's own stick 0 all along). The CamSpy's yaw moved +105.9, -332.8, +85.2 and
  +265.2 deg in one-second steps, and its pitch up to +-16. No numpad key was pressed. After the CamSpy, mouse aim on
  foot came back (yaw 57.7 -> 144.3 in two seconds).
- **Diagnostics (off by default):** `pdz_mouse_steer_log`. Once a second:
  - each steered look update (owner, view, the view's yaw / pitch through its getters: the CamSpy's at +1380 / +1376);
  - the CamSpy's own update (`sub_8253F7D8`, its control flag +1571);
  - the mouse motion the window received and the claim state;
  - the mouse stick and pad 0's right stick into / out of the pad filter;
  - the aim hook (the player's position, yaw and pitch).
- **Testing tools:**
  - `pdz_mouse_test_motion` / `_y`: counts per second of synthetic relative mouse moves through Windows SendInput,
    while the game window is in front;
  - `pdz_mouse_test_face_yaw` / `_pitch`: set the on-foot view once (999 = off).
- **The launch branch's look-hold fix** (last delta kept 80 ms between polls) is not ported as such. Direct aim adds
  every mouse count into the angle at the look update, so on foot nothing is lost between polls. For the steered
  things, the 40 ms smoothing above does that job.
- **Test:** `pdz_mouse_test_turn` / `pdz_mouse_test_pitch` feed a synthetic turn, logged every 2 s. In Datacore at
  30 / 40 deg/s the yaw turned exactly 0.25 deg per 120 Hz update and the pitch stopped at 85.0.

## Aim help with the mouse (`pdz_mouse_aim_assist_off`, on)

- **When:** while the keyboard / mouse produced input more recently than any controller (SDK:
  `rex::input::mnk::LastActivityNs()` against `rex::input::LastPadActivityNs()`, a controller counting when it leaves
  its rest position).
- **What it does:** the rotational aim assist is skipped, and auto-aim's bend is undone (the shot goes where the
  crosshair is: `aim_assist.cpp`).
- **Back to the game's behaviour:** at the first controller input.
- **Log:** each switch is logged once: `pdz_mouse: last device used: ...`.

## Keys (defaults, `settings/mapping.toml`)

| Pad | PDZ (the training's own hints) | Keys |
|---|---|---|
| RT | fire | left click |
| LT | aim / zoom | right click |
| X | reload | R |
| A | use, doors, take cover (no jump in PDZ) | Space, E |
| L3 | crouch / stand | Ctrl, C |
| Y | select a weapon | Q, wheel |
| RB | secondary fire mode | F, middle click |
| B | melee | V, side button 1 |
| LB | dodge (+ a direction) | Shift, side button 2 |
| R3 | - | Z |
| Start | pause | Esc, P (P is Start in menus too) |
| Back | objectives / scores | Tab |
| D-pad | gadgets (left = datathief) | 1-4, arrows |
| Left stick | move | WASD |
| Right stick (no mouse) | look | numpad 8 / 2 / 4 / 6 |

PDZ has no quick weapon slots: the number keys are the D-pad's gadget slots.

## Menus

Whenever no on-foot look ran for 300 ms (front end, pause, briefings, cutscenes, loading), mnk is in menu mode:
- arrows / WASD move;
- Enter or left click = A, Esc or right click = B, the wheel = up / down;
- fire, aim, Start and weapon select don't see those keys meanwhile;
- so Start is P there (since 2026-11-06 the default Start bind is `Escape,P`: with Esc alone no menu could get a
  Start, and the multiplayer setup screen needs it). P is no menu key, so it is Start in menus and in play.

There is no cursor (point-and-click is a later job). Button prompts stay Xbox.

## SDK changes (rexglue-sdk, local commit)

- Wheel (`WheelUp` / `WheelDown`, an 80 ms press per notch, released after a poll saw it) and side buttons
  (`Mouse4` / `Mouse5`) as bindable keys.
- A bind without modifiers works whatever is held, so Ctrl / Shift can crouch / dodge while moving; a bind with
  modifiers needs them held.
- Menu mode (`SetMenuMode`), and the last-activity times above.

### Any input bindable (2026-11-04, evening; settings window Controls tab)

- **Input codes** (`rex/ui/keybinds.h`): one number per input: VirtualKeys (0x01-0xFF, with the classic mouse buttons
  and the wheel's 0x0E / 0x0F), mouse buttons 6-32 (0x106-0x120), wheel tilt left / right (0x121 / 0x122), and
  0x200 + SDL scancode for keys a VirtualKey can't tell apart (left / right Shift, Ctrl, Alt; Numpad Enter) or doesn't
  have (`Scancode<N>`). `ParseInputCode` / `InputCodeToken` round-trip; `InputCodeDisplayName` is the readable name.
- **Keys:** `KeyEvent::scancode()`; `window_sdl` now passes keys without a VirtualKey too (VirtualKey kNone, scancode
  set). mnk sets both a key's VirtualKey and its scancode code, so "Control" (either) and "RControl" both work.
- **Mouse buttons 6-32:** SDL 3.4.14's Windows raw input only reads buttons 1-5. A local SDL change
  (`SDL_windowsevents.c`, WIN_HandleRawMouseInput) sends bits 5-31 of `RAWMOUSE.ulRawButtons` as SDL buttons 6-32;
  window_sdl keeps the number (`MouseEvent::Button(n)`). SDL reads raw input only while the pointer is locked (on foot),
  so buttons 6+ work there; mnk lets go of them when the lock ends. Most mice report extra buttons through their own
  software as keys instead, which work everywhere.
- **Wheel tilt:** `WheelLeft` / `WheelRight` (SDL's horizontal wheel), the same 80 ms pulse as up / down.
- **Logs:** `mnk_log_inputs` logs each input with its token and each pad button the binds change
  (`mnk: input Mouse 4 (back) [Mouse4] down`, `mnk: pad B pressed (bind "V,Mouse4", menu mode)`).
- **Test:** `mnk_test_script="20 Mouse5; 22 WheelRight; 24 RControl 0.3"` pushes those inputs as real SDL events (time
  from the first pad poll; optional hold); logged as above.

## Step zoom (`pdz_zoom_steps`, off by default; `src/mod/zoom_steps.cpp`, 2026-11-04)

An enhancement (a design change, not a fix). Off = Rare's zoom exactly.

### Rare's zoom (read from the code, TU3; logs PDZ_Job6 001-007)

| | |
|---|---|
| Update | `sub_825AC408(player, dt)` every frame. Zoom amount = player +6812 (field of view x (1 - amount)); level = +6824 |
| Range and levels | the gun in hand's record (gun +860): +32 most zoom, +36 least (0 for every gun seen), +40 levels (Rare's tables support 1-3: `sub_825AC3B8`, 82D35CB8 / CC0 / CCC) |
| Level from the input | zoom mode = input block (player +6560) +1692. **Mode 2** (pad and mouse alike, the only one seen): the aim / zoom input's depth (+1756, 0..1, the analog LT) picks the level: depth x (levels - 1) + 1, so a light pull = level 1, a full pull = the last level, 0 = no zoom. The right click is a full pull: always the last level. **Mode 3**: each press of the zoom action steps 0, 1, .. levels, 0 (not seen in use) |
| Level change | blends from the old amount over Rare's time (about 0.2 s, +6808 start / +6804 time left) |
| Amounts (3 levels, most 0.8) | level 1 0.576 (x2.36), 2 0.736 (x3.79), 3 0.800 (x5.00): Rare's fractions 0.72 / 0.92 / 1.0 of the gun's range |
| R3 | sets no action in the input block (+1800 held, +1804 pressed, +1808 released): on foot it does nothing, zoomed or not (single player, default controls; the multiplayer input path is the same code, not run) |
| Wheel with the mouse | bound to Y (weapon switch): while aiming it switches the weapon too |

Gun records loaded in Datacore (`pdz_zoom_log` scan):

| Levels | Guns (most zoom) |
|---|---|
| 3 | P99 x5.00, MP5 x5.00, M16 x3.70, RCP-120 x4.55, Rocket launcher x4.00, Magsec x10, SuperDragon x10, Sniper rifle x50, Shockwave x50 |
| 2, no zoom (amount 0) | Plasma rifle, Uzi, Shotgun, Combat shield, Vi-Blade, grenade, flashbang, the flags |
| 1, no zoom | Falcon, Magnum, Plastigun, CMP, Boomerang |
| not loaded in Datacore (not read) | AK47, Phoenix, Flechette, Laptop gun, M60, Reaper, Gauss, Devastator, Slayer, Farsight, Tranquilizer, Capacirod, the other grenades / mines, Bombbot |

### Step zoom (mode 2 only)

- **Pad:** holding LT (any depth) gives level 1. While it is held, R3 steps 1 -> 2 -> 3 -> 1 (two-level guns 1 -> 2 -> 1).
  Releasing LT ends the zoom as Rare does. R3 without LT: unchanged (nothing).
- **Mouse:** holding the aim input (right click, or what is bound to LT) gives level 1; wheel up = next level, wheel
  down = previous, within the gun's levels (no wrap). The SDK's mnk zoom wheel (`SetZoomWheel` /
  `TakeZoomWheelSteps`) keeps the wheel off its binds only while the LT bind is held outside menus; released, the
  wheel switches weapons again.
- **How:** for that one update, the depth Rare reads (+1756) is replaced by the depth of the chosen level and put back
  afterwards. Rare's levels, amounts and blend, and everything that reads the zoom (pad sensitivity, the mouse's
  field-of-view scaling, auto-aim, the aim enhancements), are unchanged.
- **Unchanged:** one-level guns, guns whose levels give no zoom, zoom mode 3, players other than local ones.
- **R3 source:** the buttons the game's XInputGetState returned (`test_harness.cpp`'s hook records them per user, the
  input script included).
- **Settings:** the Controls tab's "Step zoom" check box writes `pdz_zoom_steps` into `mapping.toml` when it changes;
  by hand `pdz_zoom_steps = true` in either settings file.
- **Log:** `pdz_zoom_log`: each level change and its cause (aim pressed / R3 / wheel up / down), the amount it settles
  at, Rare's own level changes when step zoom is off, the held gun's zoom data, and once a scan of the gun records.

## Try by hand

Your own play folder; nothing in `hardware.toml` is needed:
- the feel of the sensitivity;
- zoomed aiming;
- a controller taking over (auto-aim back; the log line);
- menus with the mouse and keys;
- a multiplayer match;
- a vehicle (the mouse as the right stick);
- the CamSpy: Datacore (fly it down the vent to the CPU and detonate it) and Temple (pilot it to the power source and
  plant the tracker): the mouse turns it left / right and up / down, slow moves too, the log says
  `the CamSpy takes the mouse` (Datacore: tested 2026-11-06, log 665);
- a hardpoint turret, the guided rocket (rocket launcher secondary), the hovercraft and the jetpack: the same.
