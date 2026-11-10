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
- **Mounted turrets: the mouse aims directly (`pdz_mouse_direct_steer`, on; 2026-11-08).** As the right stick the
  mouse turned a turret no faster than the stick's top turn rate (Bridge's opening turret felt sluggish). Now a
  mounted turret's own aim takes the mouse counts as an angle, the same way the view does on foot:
  `pdz_mouse_sensitivity` x 0.022 deg per count (0.044 by default), at any frame rate, no stick curve or turn cap.
  - The turret (`sub_8258CEA0`, vtable `820C5BE0`) keeps its aim at +928 (yaw) and +924 (pitch) in radians. Just
    before its look update (8258DEC0..8258DF20) it rewrites both in degrees through its vtable +24 / +32 and +28 / +36
    (getters `8258BA10` / `8258BA18`, setters `8258BA20` / `8258BA28`, adders `8258BA30` / `8258BA40`, none with a
    limit). The look update (`sub_825B3AC0`: the turret's user as the owner, the turret as the view, the look state at
    +936) turns them from the stick and clamps the pitch to the look tuning's limit (float +64, as on foot). After it
    (8258DF54..8258DFEC) the turret takes both back to radians, wraps them (`sub_82703490`) and clamps the pitch to
    its own limits (the floats at `82D35BE4` / `82D35BE8`; past them the doubles at `820EC708` / `820EC710`). The yaw
    has no limit.
  - The mouse turn goes in between the two (`mouse_look.h` `AfterSteeredLook`, from `look_tuning.cpp`'s look hook
    right after Rare's look update returns): the look update's pitch limit as on foot, and Rare's own turret clamp
    right after it. The fields are found through the view's own vtable (+24 / +28 must be `lfs f1,D(r3); blr` and
    +32 / +36 `stfs f1,D(r3); blr` with the same D), otherwise the mouse stays the right stick.
  - Meanwhile the mouse gives the pad filter no stick (`SteerStickForPad`) and `pdz_mouse_invert` no longer flips
    pad 0's right stick there, so a controller is the same with the switch on or off.
  - 0.9.3's check before the "+6560" read (`NoteSteeredLook`) is unchanged. On foot, the mouse aim's own player check
    (`OurView`) no longer reads the owner's +6560 when the owner isn't the view (the hovercraft and the jetpack pass
    the vehicle as the owner).
  - The CamSpy, the guided rocket, the hovercraft and the jetpack keep the mouse as the right stick (not script
    testable; the vehicles turn the player's own view with the vehicle as the owner, a different mechanism).
  - `pdz_mouse_direct_steer_log` (diagnostic, off): a `pdz_mouse: turret aim` line once a second on a turret (its
    yaw / pitch, this second's counts, the mouse turn and how far the aim moved, turret updates per second), switch on
    or off. `pdz_mouse_test_steer` / `_y` feed the direct path too (counts per second of wall clock).
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

## Controller layouts (`pdz_mnk_follow_layout`, on; `src/mod/mnk_layout.cpp`, 2026-11-08)

The keys above are stick directions, and Rare maps the sticks onto move and look by the player's controller
options. In **Classic PD** the left stick's x turns and the right stick's x strafes, so without this A / D turned and
the turn keys strafed. With `pdz_mnk_follow_layout` (default on) each key does what its name says in every layout:
W / S walk, A / D strafe, the look keys turn and look up / down. A controller is never changed.

**Where the layout lives** (read from the code, TU3): one controller object per local player at participant +1680
(player +6560 is the participant; the "input block" fields +1680.. of the other mods are this object). Its update
`sub_822748C8(controller, player, -, aim allowed)` runs once per frame (from `sub_82472E98`):

| Field | Meaning |
|---|---|
| +0 | pad index (XInputGetState user) |
| +4 | Controller Style: 0 = CLASSIC PD, 1 = STANDARD (profile +8808; also picks the look model, "PD" / "Halo") |
| +8 | context: 0 on foot (player vtable +596 `sub_825AC318`), 2 hovercraft, 4 jetpack, 6 turret; 1, 3, 5 rare |
| +16 | 0 = Look Inversion on: the look y is negated (`82274BB4`; profile +8816 bit 29) |
| +24 | 1 = sticks SWAPPED: left and right exchanged before anything else (`82274A64`; profile +8816 bit 31) |
| +28 / +32, +36 / +40 | outputs: move x (strafe) / y (walk), look x (turn) / y (pitch) |
| +76 | the aim trigger (LT) for Classic PD's aiming swap (`sub_82275BD8`), when the update's r6 allows it |

The front end's values are "CLASSIC PD | STANDARD" and "NORMAL | SWAPPED" (frontend `default.str`).

| Layout | Strafe | Walk | Turn | Look up / down |
|---|---|---|---|---|
| Standard (any context but 5) | left x | left y | right x | right y |
| Classic PD, on foot, not aiming; hovercraft / jetpack / turret | **right x** | left y | **left x** | right y |
| Classic PD, on foot, aiming (LT held) | right x | right y | left x | left y |
| Classic PD context 3; context 5 in either style | right x | right y | left x | left y |
| Sticks swapped | the same with left and right exchanged | | | |

**How:** in the game's XInputGetState (user 0, where the SDK merges the keyboard), each stick axis the keyboard
drives is moved to the axis that does the key's job in the active layout. The keyboard's share is found by tracking the
held keys and evaluating the stick binds exactly as the SDK's driver does (same tokens, modifiers): an axis is moved
only when the merged value is the keyboard's own (the keyboard won the SDK's merge). The layout comes from the last
controller update (under 300 ms old; menus, cutscenes and mnk's menu mode are left alone). Aiming in Classic PD is the
current poll's LT when the update allows the swap.

**The mouse:** on foot it turns the view directly (`mouse_look.cpp`) and never depended on the layout. As a stick
(CamSpy, turrets, guided rocket, vehicles) it goes, at the pad filter, to the axes that turn and pitch in the layout
(`RouteForSteering`: the left stick's x in Classic PD), and the game's Look Inversion no longer flips it (mouse up =
look up, as on foot; `pdz_mouse_invert` still flips it).

**Not handled:** with `pdz_mouse_look` off the SDK's own mouse stick (mnk) is not rerouted. While a key and a
controller push the same axis, the controller's weaker deflection on that axis is lost until the key is let go.

**Testing tools:** `pdz_mnk_test_layout` (classic / standard, `_swapped`, `_inverted`: forced into pad 0's controller
before each update), `pdz_mnk_test_keys` (keys held, simulated: their stick merged into pad 0 as the SDK does), and the
diagnostic `pdz_mnk_layout_log` (the layout and its axes at each change; twice a second while a stick is pushed, which
axis each key drove and the game's move / look).

## The Locktopus (`pdz_fix_minigame_keyboard`, on; `src/mod/minigame_input.cpp`, 2026-11-09)

A player on the keyboard and mouse could not do the Locktopus: Rare's lock-picking reads **only the left stick**
(controller +44 / +48 = participant +1724 / +1728), so the look keys and the mouse never reached it, and pins that
sit at odd angles or part-way out can't be set by keys that snap a stick to eight full-deflection directions.

**Rare's lock (TU3, read from the code; `sub_82648BF0` set-up, `sub_826492E8` update):**

| | |
|---|---|
| Lock data | an `aid_xml_subgames_lockbot_<level>_<lock>` asset: four floats (0.3 / 0.2 / 0.15 / 0.15 in every lock, read only as "> 0" flags) and 8 pins of 3 bytes: x, y (signed, over 128 = the stick the pin wants) and a non-zero marker for a pin that exists |
| Radius | one per lock from the difficulty: Agent 0.45, Secret Agent 0.33, Perfect / Dark Agent 0.20 (subgame +140) |
| Check | every frame the subgame copies the pad's left stick (no smoothing of its own) and takes dist = \|stick - pin / 128\|; dist < radius = inside |
| Hold | inside, the timer (+116) counts down from 1.0 s; at 0 the pin sets and the next pin (in data order) becomes current. Outside, it counts back up to 1.0 (leaving the pin undoes the hold gradually) |
| Feedback | LOCKTOPUS_SERVO cue and pad vibration, strength 1 - dist (full inside); the draw shows the eight pins round a circle with the current pin's hold. **Rare draws no pick or cursor** for the stick itself |

So a pin is "angle and deflection within the radius". A full-deflection stick can only set pins with 1 - deflection
< radius, and the eight key directions only pins within the radius of one of them. The 12 locks in the game data
(by the level package that holds them; the `test` lock and `ruin_nice_and_easy_for_chris`, one pin straight down,
are in most packages and the multiplayer vehicle lock is the `test` lock):

| Lock data | Level package | Pins (angle deg, deflection) | Pins the 8 key directions can't set at 0.45 / 0.33 / 0.20 |
|---|---|---|---|
| club_officebackdoor | Nightclub | 90 0.94; 0 0.94 | - / - / - |
| club_autogunroom | Nightclub | 0 0.86; 180 0.86 | - / - / - |
| ware_factorysidedoor | Subway | 45 1.10; 0 0.78; 225 1.10 | - / - / 1 (0 deg needs a part-way stick) |
| ware_diversionsidedoor | Subway | 66 0.86; 163 0.82; 107 0.82 | - / 1 / 3 |
| labo_generatorswitch | Laboratory | 309 1.11; 212 1.02; 252 0.90; 90 0.94; 138 1.15; 308 1.08 | - / - / 2 |
| labo_gasvalve | Laboratory | 45 1.10; 227 1.18; 350 0.87; 224 1.19; 38 1.09; 310 1.13 | - / - / 1 |
| mans_ladderlock1 | Mansion (also Bridge, Trinity) | 42 1.11; 221 1.08; 138 1.06; 306 0.97 | - / - / - |
| mans_intercomdoor | Mansion | 65 0.88; 264 0.71; 336 0.77; 214 0.83 | - / 2 / 4 (two need a part-way stick) |
| airp_main_gate | River | 45 1.10; 125 1.00; 240 0.94; 0 0.86; 217 1.03; 241 0.94 | - / - / 2 |
| cs2_water_valve | Trinity Escape | 217 1.03; 240 0.91; 0 0.86; 127 0.97; 240 0.94; 45 1.10; 217 1.03; 125 1.00 | - / - / 2 |
| ruin_nice_and_easy_for_chris | most levels | 270 0.94 | - / - / - |
| test (and multi_vehiclelockdata) | several | 318 1.05; 125 0.86; 37 0.78 | - / - / 2 (one part-way) |

In all: of 47 pins, 0 are out of reach on Agent, 3 on Secret Agent and 17 on Perfect / Dark Agent, 4 of which need
a part-way stick whatever the angle. (Which door uses which lock data is not in the propsetups as a name; the
set-up log line names the data of each lock opened.)

**The fix:** while the Locktopus runs for pad 0, after Rare's controller update, a **virtual stick** is written to
+44 / +48 instead of the snapped keys:

- the left / right keys of either stick (A / D, Numpad 4 / 6) turn the pick round the circle: a full turn in
  `pdz_minigame_key_turn_s` (2.5 s); with **Shift** held `pdz_minigame_key_slow` (0.4) of that speed for fine control;
- the up / down keys (W / S, Numpad 8 / 2) push it out and pull it in (centre to full deflection in 1 s, Shift slows
  that too);
- the mouse moves the same point (`pdz_minigame_mouse_counts`, 400 counts from the centre to full deflection), so
  mouse and keys work together;
- it stays where it was left (so a pin can be held for its second), starts at the centre with each lock, and follows
  a controller's left stick while that is pushed;
- a controller is untouched: nothing is written while its left stick is pushed, nor before a key or the mouse has
  been used in the lock. Off = Rare's read of the left stick only.
- `pdz_minigame_inputs` (0.9.4: "both", "mouse" or "keys"; the settings window's Controls tab, Locktopus) leaves the
  mouse or the keys out; keys held in "mouse" mode write the unmoved virtual stick, so they don't steer through Rare's
  own read either.

**The settings window's practice dial** (Controls tab, Locktopus group, 0.9.4): the same step (`pdz::minigame::
SteerPick`, shared with the game) and Rare's pin rule (`StepPinHold`: within the radius the 1 s hold counts down by dt,
outside it counts back up, as `sub_826492E8` does) on the pins of one of the 12 locks above, at random (New lock), with Rare's radius for the Agent / Secret Agent / Perfect Agent choice under it (0.45 / 0.33 / 0.20, default Secret Agent). It uses the
window's unsaved Mouse / Keys / Both choice and sensitivity and the key list's current Move / Strafe / Look keys.
`pdz_settings_test_dial` plays it with simulated keys and mouse counts (`keys_pa` / `mouse_pa`: the Mansion intercom door on Perfect Agent) and captures it mid-lock and finished.

Other minigames: the demo kit's tile puzzle (`sub_8264ADE0`) reads the same fields as a digital pad; with the fix the
keys of either stick move its cursor (snapped, as a pad press). The Data Thief reads no stick.

**Testing tools:** `pdz_minigame_input_log` (each lock's data, radius and pins at its start; twice a second the
subgame's state, pin, hold, the stick it read, the virtual stick, the pad's raw sticks, the keys, the mouse, what was
written; a line as each pin sets), `pdz_minigame_test_aim` (writes the current pin's own target), and
`pdz_minigame_test_radius` (replaces the difficulty's radius at a lock's start, e.g. 0.2 to test the hard radius on
any difficulty).

## Menus

Whenever no on-foot look ran for 300 ms (front end, pause, briefings, cutscenes, loading), mnk is in menu mode:
- arrows / WASD move;
- Enter or left click = A, Esc or right click = B, the wheel = up / down;
- fire, aim, Start and weapon select don't see those keys meanwhile;
- so Start is P there (since 2026-11-06 the default Start bind is `Escape,P`: with Esc alone no menu could get a
  Start, and the multiplayer setup screen needs it). P is no menu key, so it is Start in menus and in play.
- The settings window's Controls tab (2026-11-08) shows an orange warning while Start has only menu keys (Enter, Esc,
  left / right click, the wheel; Numpad Enter counts as Enter) or no key: add a plain key, P is Start in menus by
  default. It updates with every rebind, Clear and Reset.

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
