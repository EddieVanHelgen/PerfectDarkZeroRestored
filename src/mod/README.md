# Third-person camera mod

Perfect Dark Zero is a first-person game, but it already has a third-person
camera: you get it for a moment when you roll. This mod keeps that camera on.
The code is in `third_person.cpp`.

## Usage

Press **V** to switch between first and third person. To use another key,
change `bind_third_person` in `settings/mapping.toml`.

Settings in `settings/hardware.toml` (all can be changed live from the F4 overlay):

| Setting | Default | What it does |
|---|---|---|
| `pdz_tp_enable` | `false` | Start the game in third person |
| `pdz_tp_offset_x/y/z` | `0` | Camera offset in camera space, in roughly centimetres. Negative Z is behind the player. `-80, 10, -320` works well. |
| `pdz_tp_zoom_mode` | `2` | What happens when zoomed: `0` stays behind the player, `1` goes first person, `2` slides forward |
| `pdz_tp_binoculars_fp` | `false` | Use first person while looking through the binoculars |
| `pdz_tp_auto_fp_gadgets` | `true` | Use first person while hacking with the Data Thief |
## How it works

- **The gameplay camera.** It is camera mode 0, updated by `sub_8220D9C8`. It
  stores an offset vector at `+160`. When the offset isn't zero, the camera
  moves by it and a collision ray stops it from going through walls. Rolls fill
  this vector in for about a second. The mod writes its own offset into it every
  frame.
- **Where the camera looks.** It doesn't look straight ahead. It looks at a
  point 10 m in front of the eyes (`+16`), so the crosshair lines up with where
  the bullets go.
- **Only the player's camera.** The CamSpy uses the same camera mode. The mod
  checks the camera's target and only changes the player's camera.


## What didn't work

- **Firing our own ray along the player's view to find the distance.** The
  binoculars' ray passes through characters, so the distance kept jumping
  between the target and the background, and the camera shook.
- **Always using the real distance instead of 10 m.** With the camera 80 cm to
  the side, every jump in distance, say from a nearby wall to the far
  background, made the camera turn on its own by up to about 5°.
- **Smooth transitions.** We switch views instantly on purpose: it felt better.

## How we found all this

1. Using memory dump.
2. Searched it for strings and followed them to code. Task names like
   `CameraControlUpdate` sit in a table next to their functions.
3. Read the generated C++, which keeps the original PowerPC instructions as
   comments.
4. Hooked functions with `REX_HOOK_RAW`, logged what they did while playing,
   and compared the logs with what we did in game. Most of the real answers
   came from these logs, not from reading code: for example the lock flag
   (`+1384` in the binoculars object) and where the target's position is kept.

# Ragdolls

`ragdoll_tuning.cpp` changes how the death ragdolls are built. Enhancement,
off by default (the defaults are the originals: inertia scale 1, angular
damping -1, blend cap 0); e.g. 10 / 0.4 / 1.5 give lighter, freer limbs
(their largest natural moment instead of 10x it), more angular damping and a
shorter death blend. Ragdolls are built when a character is created, so a
change shows on characters spawned afterwards (next level, or respawns).
The ragdolls themselves are as Rare designed them (`docs/BUG_AUDIT.md`,
RD1-RD7).

| Setting | Default | What it does |
|---|---|---|
| `pdz_ragdoll_inertia_scale` | `1` | Divides limbs' resistance to spinning (it scales the inverse inertia the game sets): `10` = 10x lighter = each part's largest natural moment, `0.5` = twice the game's (stiffer), `1` = original |
| `pdz_ragdoll_angular_damping` | `-1` | Angular damping; `-1` = original (0.05) |
| `pdz_ragdoll_linear_damping` | `-1` | Linear damping (original 0) |
| `pdz_ragdoll_death_blend_max` | `0` | Cap on the death-animation-to-ragdoll blend (`sub_825DA158` → `sub_825DC560` mode 1; original 2.5x the animation length; `0` = original) |
| `pdz_ragdoll_log` | `false` | `pdz_ragdoll:` log lines (switches `sub_825DC560`, blend state from `sub_825DA950`, each body's computed inertia and the inertia set, and the pelvis' fall for a second after each switch to ragdoll: height / vertical speed every 50 ms and the time of its first 1 m drop) |

## How the game builds them

- `sub_825D8B30` builds a ragdoll from its asset (humans share
  `aid_darkcharacter_ragdolls_jack`, the spiderbot has its own). Per body
  part it fills a Havok rigid body cinfo (`sub_829B2218`: linear damping 0,
  angular damping 0.05), takes friction, restitution and mass from the
  asset, creates the body (`sub_829B1A98`) with the inertia computed from
  its capsule and mass (`sub_825D6E70`), then raises it for stability. The
  bodies use Havok sphere motions, which keep a single inverse inertia
  (motion +196): creation sets it to 1 / the largest principal moment; the
  builder reads that inverse back (motion vtable +56), multiplies it by 0.1
  and sets it again (`sub_829B0790` -> vtable +52, setInertiaInvLocal), so
  every axis gets 10x the largest moment. Deliberate (BUG_AUDIT RD2).
- Physics itself is sound: the Havok world is set up with gravity
  (0, -9.8, 0) and a 4-iteration medium solver (`sub_824DB530`), and is
  stepped every frame with the real frame time in 1/60 s substeps
  (`sub_824DBB78` -> `sub_829B61D0`), so the 60 FPS patch doesn't change it.
- The hooks only touch bodies created while the builder runs.

# Auto-aim

`aim_assist.cpp` tunes the player's auto-aim. Defaults are the original game
except `pdz_aim_still_lock`, which is on (the master switch stays on for it);
the close-range radius (e.g. 35 cm), friction (e.g. 0.35) and difficulty
override are enhancements, off by default. **F10** (`bind_aim_ab`) switches
the tweaks off and on.

| Setting | Default | What it does |
|---|---|---|
| `pdz_autoaim_tuning` | `true` | Master switch (F10) |
| `pdz_autoaim_enable` | `true` | `false` = no auto-aim |
| `pdz_autoaim_cone_scale` | `1` | Lock radius and selection margin |
| `pdz_autoaim_min_radius_cm` | `0` | Minimum lock radius at the target, cm (widens it up close; e.g. `35`, `0` = original) |
| `pdz_autoaim_strength_scale` | `1` | How far the shot bends to the target |
| `pdz_aim_friction` | `0` | Slower turning near a target (new; e.g. `0.35`, `0` = off) |
| `pdz_aim_friction_zone_deg` | `3` | Friction zone around a target's centre, degrees (at least its body) |
| `pdz_aim_friction_near_m` / `pdz_aim_friction_full_m` | `2` / `6` | Friction fades out on targets closer than `full_m`, none within `near_m` |
| `pdz_aim_friction_sweep_start_dps` / `_end_dps` | `10` / `30` | Friction fades out as the target's line of sight rotates faster (from either one's movement, smoothed over 0.25 s): circle strafing |
| `pdz_aim_friction_toward` | `0.9` | Share of the friction while turning toward the target (from the aim direction's change since the last update; `1` = always) |
| `pdz_autoaim_ignore_difficulty` | `false` | Same auto-aim on every difficulty (`false` = original) |
| `pdz_aim_still_lock` | `true` | Bend shots while holding still or tracking slowly: lock radius `+0x14 + +0x1C * t` instead of `(+0x14 + +0x1C * t) * t` (after `sub_824E36D8` returns to `824E23F8`, both fields divided by t for the lock test; t = 1e-6 when 0). `false` = original |
| `pdz_aim_assist_zoomed` | `true` (on by request) | Keep the game's rotational assist (`sub_8259FD70`) on while zoomed: its zoom check (player vtable +536, `sub_8259C878`, called at `8259FEA0`) answers "not zoomed" for that call only. `false` = original (assist off while zoomed) |
| `pdz_aim_respect_headshots` | `true` | While the reticle is over the target's screen box (auto-aim +0x20 <= 1), a bent shot keeps the player's aim height (+0x3C = +0x34) and is only bent sideways: the game bends to a fixed point on the body (`sub_82535E00`), so a head aim would hit the body. On by request for testing; `false` = original |
| `pdz_autoaim_log` | `false` | `pdz_aim:` log lines |

## How the game's auto-aim works

- **It bends the shot, invisibly.** The player's auto-aim object is at
  `player +0x1F60`, updated by `sub_824E2028` from the player's aiming update
  `sub_825A34F0` (hooked to know which player it is: the object's `+0xC` is
  not the player). It starts from where you aim
  (`+0x30/+0x34`, screen units) and, if it locks, moves the aim
  (`+0x38/+0x3C`) onto the target's aim point. The caller (`sub_825A34F0`)
  copies that to `player +0x1F58` and hands it only to the weapons' aim
  (`sub_825787F0` per hand), so the shot goes there while the drawn
  crosshair stays centred. (Separately, the game also has a rotational
  assist that turns the view: see below. Earlier versions of this file said
  there was none; that was wrong.)
- **Picking a target** (`sub_824E2528`, the object's vtable +0): characters
  in sight (ray test), within 250 m, whose screen box padded by
  `(record +0x4 + record +0x18) * min(1, weapon factor * 1.75)` pixels
  contains the crosshair. The weapon factor (`+0x64`) comes from the weapon,
  so some weapons have little or none; that is left alone.
- **Locking**: the target's aim point must be within
  `(record +0x14 + record +0x1C * t) * t` of the crosshair, in normalised
  screen units of a reference projection (45.2 deg vertical, set by
  `sub_82644AF0` from 58 deg at 4:3, the same as the real view at
  `pdz_fov_scale` 1). `t` is how fast the view turns: the look code writes
  the yaw / pitch speeds to `+0x5C/+0x60` (`825A03D4`), and
  t = clamp(speed^2 / (90 deg/s * zoom factor)^2, 0, 1). So the radius is 0
  while you hold still (no bending), and the full 0.16 (~3.8 deg; 0.125 on
  Perfect / Dark Agent) only at fast-look speeds. A record with `+0x08 == 0`
  would even require t > 0.1 (`824E2328`), and the lead is scaled by t^2, so
  this "assist while sweeping" may be deliberate. `pdz_aim_still_lock`
  (aim enhancement, on by default) changes it to `+0x14 + +0x1C * t`; in a
  Combat Arena A/B with one gun it raised hits from 35% to 59% of tracked
  shots (6-20 m: ~31% -> ~67%).
- **Rotational assist** (`sub_8259FD70`, end of both look models): while the
  crosshair is over a selected target's box (full weight in its inner half),
  you're turning faster than 4.5 deg/s or the target moves, and you're not
  zoomed, it turns the view by strength (record `+0x00`: 0.8 / 0.75 / 0.5,
  MP 0.75) times the change of the target's bearing (clamped to 70 deg/s,
  10-30 deg/s within 3 m): it resists sweeping across a target and follows a
  moving one. Pitch only while the look stick has a vertical input. It
  doesn't depend on the lock radius. `pdz_autoaim_log` shows it
  (`rotational assist ...` lines). `pdz_aim_friction` stacks with it.
- **Records**: `playerparams/*autoaim.xml`, 8 floats, pointers at
  `82E3B558` (agent, secret agent, perfect agent - also dark agent - and
  multiplayer), picked by `sub_824E1DA0`. Dual wielding in some modes uses
  the perfect agent record. Zooming turns auto-aim off on Perfect and Dark
  Agent: `sub_825A34F0` asks `sub_8259AA60` ("aiming down the sight?") at
  `825A3544` and skips auto-aim on difficulties 2+ if so, except in player
  states 0x25/0x26 where every difficulty inverts the rule.
  `pdz_autoaim_ignore_difficulty` points the record table at the agent record
  during the player's update and answers that one call (matched by its return
  address) the way the Agent rule ends up. (0x25/0x26 are the held gun's
  type, Sniper Rifle and Shockwave, not player states.)
- **The tuning** patches the four records only during the player's update,
  scales the jump afterwards, and applies friction through the look state's
  turn scales, and ends fast look while it applies (otherwise the game would
  slow the turn at only the fast-look rate and the friction wouldn't be
  felt with the stick at the rim). Friction uses the true angle between the
  aim and the target:
  the update first projects the aim reticle out to 250 m (`sub_824E38B0`,
  hooked to keep that point), and the target's aim point is at `+0x24`.
  Friction is full within `pdz_aim_friction_zone_deg` (or the target's body,
  30 cm either side, if bigger) and fades out by twice that. While zoomed the
  zone shrinks by `1 - zoom` (`player +0x1A9C`), the factor the game slows
  turning by, so it covers the same part of the screen. While it's on,
  the search margin is doubled so small distant targets are picked up; the
  lock (shot-bending) radius isn't affected.

# Look tuning

`look_tuning.cpp` tunes the right stick on foot. Enhancements: the master
switch `pdz_look_tuning` is on by default for the one look enhancement that
is on, `pdz_fastlook_zoom_brake`; every other option defaults to its
original value, so the zoom brake is the only change. They all apply instantly, also from the F4 overlay (the controller
keeps working while it's open, as long as the mouse isn't over it). **F6**
(`bind_look_ab`, in `live_toggles.cpp`) switches them all off and on again
for an A/B comparison and shows what's active; **F8** (`bind_latency_ab`)
does the same for the runtime latency options.

| Setting | Default | What it does |
|---|---|---|
| `pdz_turn_speed_scale` | `1` | Turn speed (yaw, pitch and the fast-look boost) |
| `pdz_pitch_speed_scale` | `1` | Vertical look speed only, on top of `pdz_turn_speed_scale` |
| `pdz_turn_accel_scale` | `1` | How quickly turning speeds up and slows down, including the fast-look ramp |
| `pdz_stick_curve` | `0` | Response exponent; `0` keeps the game's (1.5 PD style), `1` is linear |
| `pdz_stick_deadzone` | `-1` | Right-stick deadzone per axis, as a fraction of travel; `-1` keeps the game's 0.153 |
| `pdz_look_deadzone_scale` | `1` | The look code's second, cross-shaped deadzone; `0` turns it off |
| `pdz_stick_radial` | `false` | Round deadzone and curve on the stick's overall deflection (diagonals at full speed) |
| `pdz_fastlook_zoom_brake` | `true` | **Look enhancement, on by default.** Zooming ends fast look instead of coasting past the target (with `pdz_look_tuning` on; `false` = original). The original glide is deliberate (Halo audit H10, `docs/BUG_AUDIT.md`) |
| `pdz_fastlook_exit` | `0.95` | Stick deflection below which fast look ends (it starts at 0.98) |
| `pdz_look_log` | `false` | Logs the look controller: one `pdz_look: frame` line per update (pad raw -> filtered, deadzone, learned centre, look stick, yaw / pitch deg/s, fast look, scales, rotational assist turn and net rate, mod assists) plus a summary once a second |

## How the game turns

- **Two models.** `sub_825A42B0` updates a look controller; on foot that's the
  player, with its look state at `+0xF50`. It runs `sub_825A0888` ("PD") or,
  when the player input's `+0x484` is 1, `sub_825A1280` ("Halo").
- **Parameters.** Each model blends a 13-float record from
  `playerparams/{min,default,max}aim{pd,halo}` by the sensitivity setting
  (min at 0, default at 0.5, max at 1). The record pointers are at `82DA3760`.
  Fields: 0/1 yaw/pitch rate, 2 curve exponent, 6/7 deadzones, 8/9
  acceleration/deceleration, 10/11 fast-look acceleration/rate.
- **Velocity, not position.** A yaw velocity V moves towards
  `6 (movementparamsjo +8) * rate * |stick|^exponent` at the acceleration
  (scaled by 0.25 + 0.75 * |stick|), and the view turns `V * 60 * dt`
  degrees. Reversing the stick sets V to 0 for a frame. Holding the stick at
  the rim, once V is near its maximum, raises the target to the fast-look
  rate (field 11) at field 10's slow acceleration. Pitch works the same way
  with its own rate (field 1, blended by the vertical sensitivity) and the
  same curve and acceleration, but has no fast look.
- **Deadzones.** The stick filter (`sub_8226BE48`, state at `82D6B3A8`,
  right-stick slots 4-7) learns each axis's centre and applies a square
  deadzone of 5000/32767 (7000 for a jittery stick), rescaling the rest. The
  look code then adds a cross-shaped one: yaw `0.1 * (0.3 + 0.7 * |y|)`,
  pitch `0.25 * 0.5 * (1 + |x|)` (PD).
- **Diagonals.** The stick deadzone, the cross deadzone and the curve all
  work per axis, so a 45 degree push at the rim reaches only ~0.65 per axis
  and turns ~35% slower than a straight one, and never reaches the 0.98 that
  fast look needs. `pdz_stick_radial` replaces the filter's output with a
  round deadzone around its learned centre, then pre-shapes each axis
  (`magnitude * |direction| ^ (1/e)`) so the game's per-axis curve acts on
  the stick's deflection along its own direction.
- **Fast look and zoom.** Fast look turns on at 0.98 and off below 0.95.
  While it's on, the velocity moves at field 10's slow rate both ways, so
  zooming with the stick at the rim coasts on at up to 180 deg/s, dropping
  by only ~120 deg/s per second. `pdz_fastlook_zoom_brake` reports Fast Look
  as off (`sub_8259D1B0`) during a zoomed update, and divides the
  horizontal sensitivity (`sub_8259D140`) by the 1.2 the game adds when
  Fast Look is off, so the normal deceleration applies at once. Both look
  models read these two (Standard = Halo model `sub_825A1280`: +0x210 gates
  fast-look entry at `825A1654`, x1.2 at `825A13A0`; Classic PD
  `sub_825A0888`: +0x210 at `825A0990`, entry at `825A0CA4`, x1.2 at
  `825A09E4`), so it works the same with either control scheme.
- **What is left alone.** Zoom slows turning by `1 - player +0x1A9C` (scope
  zoom, set per weapon); the player state scales the turn through the look
  state's `+0xC/+0x10` (0, 0.2 or 0.5 while in cover, rolling and so on).
  The tuning only patches the records during the player's on-foot update and
  restores them right after, so vehicles and turrets keep theirs.

# Field of view

`fov_tuning.cpp` widens (or narrows) the on-foot view with `pdz_fov_scale`
(enhancement; default `1` = original, about 73 degrees across at 16:9; `1.1`
is about 79).

The game's 58 degrees is a **4:3 horizontal** FOV, not a vertical one: the
camera manager converts it to 45.2 degrees vertical before the projection
(`82205900` -> `sub_82644AF0`: 2 * atan(0.75 * tan(h / 2))), and widescreen
widens it sideways from there. (Earlier versions of this file said "58
vertical, ~89 across"; that was wrong.)

| `pdz_fov_scale` | 4:3 horizontal | Vertical | Across at 16:9 |
|---|---|---|---|
| `1.0` (original, default) | 58 | 45.2 | ~73 |
| `1.1` | 63.8 | 50.0 | ~79 |
| `1.2` | 69.6 | 55.1 | ~86 |

- The gameplay camera (mode id 0, `sub_8220D9C8`) sets its FOV at `+0x70` to
  58 (`820EC228`) times the player's zoom factor from player vtable `+0x290`
  (`sub_8259D0D8`: `1 - player +0x1A9C`). The hook scales that factor for the
  call at `8220E454` only, so scopes keep their magnification relative to the
  new view, and other camera modes (vehicles, turrets, cutscenes) keep theirs.
- It is a vertical FOV: `sub_82512430` builds the projection as
  `1 / tan(fov / 2)` and divides the width by the aspect ratio
  (`sub_82644178`: the viewport's, or the 16:9 constant at `820EC158`).
- The third-person camera's zoom detection compares against `58 * scale`.

# Cover (diagnostic only)

`cover_log.cpp` logs the player's cover state changes with timestamps
(`pdz_cover_log`, default `false`; needs `log_level = "info"`). It only
reads: cover behaviour is the original game's.

| Setting | Default | What it does |
|---|---|---|
| `pdz_cover_log` | `false` | `pdz_cover:` log lines: cover found/lost, enter requested (with Cover Mode and A state), snapped in, lean out/back, exit, time in each step |
|  |  | `pdz_cover_ai:` lines for enemies: cover task created / in cover / peek position / look-out / snap-back / cover tests failed / ended, and totals once a minute |

## How the game's cover works

- **Player only.** The module (`sub_824EFCF8`..`sub_824F7F40`, player fields
  `+0x1C34`..`+0x1D6C`) runs from the player update `sub_82598128` through
  `sub_824F11C0(player, dt)`. Enemies use a separate AI cover task and
  never call it. Most of its constants are shared with other code, so
  changes must be player-scoped hooks, not constant patches.
- **Finding cover** (state 0): `sub_824F4AB8` → `sub_824F4630` casts one
  probe a frame: straight ahead and ±16.7°, up to 5 steps each (9-15
  frames for a full scan; restarts after moving 1.5 m). Rays are 1.7 m long
  at 0.8 m (low) and 1.55 m (high); the wall must be within 45° of vertical;
  side probes 70 cm out find the edges; it needs 1.6 m headroom and enough
  width. Surfaces can be tagged NoCover, BlockCover or InvisCover in the
  level collision. A result kept while you've turned less than 40° and moved
  less than 1.3 m raises the A prompt (context action 0x7A, priority 2, so
  doors and pickups win).
- **Entering**: A calls the player's action callback `sub_8259B2B8`
  (vtable +148) → `sub_824F03A8`: state 1, the entry animation is time-scaled
  to 0.5 s and the third-person cover camera blends in over the same time.
- **States** (`+0x1CA4`; in state 2 the target is `+0x1CAC`): 0 out, 1
  entering, 2 transition, 3 in cover, 4 popped out (the only state that can
  fire), 5 exit animation. Holding fire leans out (0.25 s, no firing until it
  finishes); releasing it waits 0.35 s, then leans back over 0.8 s. No blind
  fire, no roll out of cover.
- **Leaving**: Cover Mode (`[player +0x19A0] +0x4E6`): 0 = hold A (release
  to leave), else toggle (press A again). From state 3 it plays the exit
  animation (state 5); from 2 or 4 it is instant. `sub_824F1610` forces an
  exit (special-move cancel).
- **Camera**: its own mode with a fixed 58° FOV (ignores `pdz_fov_scale`),
  turning clamped to the wall (±90° on low cover, 67.5° toward an open
  edge, ±29° pitch).
- **60 FPS**: the timers use real frame time; only the scan is counted in
  frames, so at 60 FPS the prompt appears twice as fast (150-250 ms instead
  of 300-500 ms).

## Enemy cover

- Enemies use their own cover task, not the player module. Cover points are
  hand-placed level pads (flags at `+0x34`: low cover, sides, peek points).
- The combat planner (`sub_821B0AB0`, `sub_821B20D8`) only adds a cover step
  when the target is over 5.5 m away and the behaviour allows it (always for
  CAUTIOUS_ATTACK and COVER_JUMP, with template flags for most others, never
  for snipers or POSITION_AND_SHOOT), then picks 50/50 between moving to a
  cover point within 35 m (task vtable `82094758`, `sub_821C6AC8`) and taking
  cover in place on the pad it stands on (vtable `820946F4`, `sub_821C6190`,
  refused if that pad has no peek points).
- In cover the enemy is pinned to the point (snaps back if more than 15 cm
  away), peeks after 2-4 s from a linked peek point that sees the target, or
  does a look-out every 4-8 s when not hit for 25 s. The cover tests end the
  task after 5 s with no sight from a peek point or 6 s with the target out of
  its arc. Points the player aims near (80 cm, 15 m, every 0.5 s) are
  "suppressed" (`+0x5E`): enemies won't pick them or peek from them.
- Suspected bugs (from code, unconfirmed in game): the 5 s / 6 s grace timers
  start when the task is created, not on arrival, so a long walk can fail the
  tests on arrival; the 1-in-3 alternate peek roll (`821C6BB0`) compares
  `rand * 3.0f == 0`, so it never happens; the COVER behaviour gets the same
  plan as ALL_OUT_ATTACK; the snap-back has no limit.

## Enemy cover fixes

`ai_cover_fixes.cpp`, applied from the cover task hooks in `cover_log.cpp`.
All on by default (`false` = original AI). **F5** (`bind_ai_cover_ab`)
flips the master switch in game and shows which fixes are active.

| Setting | Default | What it does |
|---|---|---|
| `pdz_ai_cover_fixes` | `true` | Master switch; the fixes below only apply while it's on |
| `pdz_ai_cover_fix_grace` | `true` | Restart the 5 s (`+0x70`) / 6 s (`+0x78`) cover test timers on the first update after the walk and settle-in sequence (`+0x3C`) ends, instead of at task creation |
| `pdz_ai_cover_fix_peek_roll` | `true` | Points without flag 0x8 get the alternate peek animations (`+0x59`) 1 time in 3, as the constructor's roll intends |
| `pdz_ai_cover_fix_snapback` | `true` | After 3 snap-backs in a row with no peek or look-out between, set the task to state 2 (the game's own give-up) so the planner picks something else |

Not changed, because they look deliberate: points with no peek points end
after 5 s (the tests ask "can I fight from here?", and from such a point the
enemy can't), and Start rejects points with more than 2 peek points (the task
has 3 slots: 2 peek points plus the point itself for over-the-top cover).

The COVER behaviour (12) falling through to ALL_OUT_ATTACK's plan is left
alone: `sub_821B0AB0` only has cases for 1, 2, 7, 8, 11, 16 and 17 and sends
the other eleven values to the default plan, and no enemy in the
single-player levels uses COVER or COVER_JUMP (setup records, template +0x5C
= record +0x68: of ~1000 setups, 358 CAUTIOUS_ATTACK, 353 HOLD_POSITION, 230
ALL_OUT_ATTACK, the rest snipers / special / zonal). CAUTIOUS always uses
cover; most other behaviours only once hurt or shot at (character +0x186C
0x80 / 0x100, set by the damage and bullet code). Two unreferenced plan
actions (`sub_821B2A30`, `sub_821B2C08`) are a "stay in cover regardless"
pair (cover tests skipped).

`pdz_cover_ai:` lines show `[fixes: ...]` on each new task and in the totals,
plus `(re-rolled 1 in 3)`, `grace reset on arrival` and `snap-back timeout`
where a fix acted.

# Enemy AI state log (diagnostic only)

`ai_state_log.cpp` (`pdz_ai_state_log`, default `false`; needs
`log_level = "info"`) writes one `pdz_ai:` line a second for each enemy
that is aware of you or within 50 m. It only reads.

| Field | Meaning |
|---|---|
| setup | behaviour from the AI setup (template +0x5C): CAUTIOUS, HOLD_POS, ALL_OUT, SNIPER... |
| aware / hurt / shot-at | char +0x186C bits 0x2 / 0x80 / 0x100 (hurt and shot-at are never cleared; either makes the default plan try cover) |
| provoked / no-cover | +0x187C 0x800 / 0x1000 (no-cover is set by level scripts and never cleared) |
| alert | perception slot alert count (0..100) |
| target, seen | target (player or another character), distance to player 1, time since the target was last seen (+0x30F8) |
| pads | the enemy's and the player's current pad (+0xB4); every plan goal fails while the player's is null |
| tasks | running task ids: combat, engage, shoot, dodge, hunt, idle (39, the do-nothing fallback, up to 5 s), nocover |
| plans | plans built since the last line by the default / cautious planner and their kind (attack, cover, other = close plan, none) |
| chooser | cover chooser calls and give-ups (no target, close = within 5.5 m, pool = fewer than 4 free plan nodes) |
| cover pads | pads the cover search tried, passed, and passed while under 10 m from the target (the per-tick re-check rejects those: AI35), re-check failures |
| ended / fallback | plan ends by result (fail, reached / re-check failed, no pad for any goal) and fallbacks (6 s rule, no pad) |

# Controls fixes

`controls_fixes.cpp` (with hooks in `look_tuning.cpp` and `aim_assist.cpp`)
fixes bugs found by the original-controls audit (`docs/BUG_AUDIT.md`). Parked:
all are off by default; each needs the master switch, and **F2**
(`bind_controls_ab`) flips the master switch in game with a label listing the
active fixes. With them off the original code runs unchanged.

| Setting | Default | What it does |
|---|---|---|
| `pdz_controls_fixes` | `false` | Master switch |
| `pdz_fix_fastlook_stick` | `false` | PD look model: the two move-stick reads (`825A08DC` / `08F4`) answer with the look stick, so fast look and the cross deadzones use the look stick like the Halo model |
| `pdz_fix_fastlook_decel` | `false` | PD fast look slows down at the decel rate (field 9) instead of the fast-look accel (field 10) when zoomed, in cover, or after its speed dropped |
| `pdz_fix_sensitivity_clamp` | `false` | Horizontal / vertical sensitivity seen by the look models capped so x1.2 (Fast Look off) and x1.75 can't blend past the max record |
| `pdz_fix_fastlook_accel_data` | `false` | PD default fast-look accel 3 (data has 2, below min 3 and max 4) |
| `pdz_fix_spread_recovery_gate` | `false` | Spread recovery's "both hands idle 0.2 s" test: a hand component without a weapon state counts as idle (the game answers 0 = just fired) |

`pdz_autoaim_log` also prints `pdz_fix:` lines once a second (active fixes,
lock rewrites, empty-hand cases in the spread check) and `rotational assist`
lines.

# Combat aiming log

`combat_log.cpp` (`pdz_combat_log`, default `false`, needs `log_level =
"info"`) measures aiming in real fights, player only, read-only. While the
auto-aim has a target, or an enemy that is targeting you is within 40 m and
in view, it samples at 20 Hz and writes `logs/combat_<time>.csv`:

- distance and band (0-3, 3-6, 6-10, 10-20, 20-40 m);
- the yaw / pitch rate needed to stay on the target (line-of-sight rotation),
  your actual rate (unbent aim direction) and the look model's maximum this
  frame (records blended by your X / Y sensitivity: yaw 6 x field 0, fast
  look 6 x field 11, pitch 6 x field 1, x60 deg/s, x (1 - zoom));
- look stick, fast look and its ramp, zoom, aim error (deg and cm at the
  target), the rotational assist's weight and rate, shot bending;
- the enemy's speed, speed across your line of sight and whether it is in a
  cover task; the active look model (style +0x484: 1 = "Halo", else "PD")
  and sensitivities.

Every hitscan shot by your weapon adds a `shot` row: distance, error, bent
or not, hit (AI damage handler called during the shot) and an estimate of
whether bending was needed (unbent aim > 25 cm off the target). Projectile
weapons aren't counted. A summary per band (time, % of time the needed turn
beat the normal and fast-look maximum, mean / p90 error, hit rate, bent
shots, fast-look use, reacquire time after the error passed 5 deg until it
is back under 2 deg) is logged every minute and kept in
`logs/combat_summary.txt`. The first lines say whose hitscan shots were
seen, to confirm your shots are recognised.

`pdz_look_log` now also prints the look model and the scheme it most likely
belongs to (`style 1 = Halo model (Standard?)`).

# Shot log

`shot_log.cpp` (`pdz_shot_log`, default `false`, needs `log_level = "info"`)
writes one `pdz_shot:` line per hitscan shot by a player. Read-only: it
changes nothing in the game. It hooks the shot's aim (`sub_822397C8` from
the weapon aim, via `game_fixes.cpp`), the hitscan (`sub_82238938`, via
`combat_log.cpp`), the hitscan's segments (`sub_82175658`) and hits on
objects (`sub_82512D48`). Each line has:

- `t`, the player, the weapon (gun type name) and the hand;
- `zoom` (0 = not zoomed);
- `cover` state (0 out, 1 entering, 2 leaning out / back, 3 in cover, 4
  popped out, 5 exiting), the time in that state, which shot this is in that
  state (`shot 1 in state` = the first shot since popping out), the cover
  camera blend timer (0.3 s at lean-out) and `edge` when the edge-step blend
  is running;
- `origin`: where the shot starts. `eye` = the camera the crosshair belongs
  to (auto-aim +0x40), `cover camera` = the cover camera (player +0x1C88),
  otherwise the offset from the eye in cm (x y z, y = up). In cover the cover
  camera's offset from the eye is printed too;
- what the crosshair is on (`world at N m` or `open space`), from the aim
  ray through the crosshair;
- `target`: auto-aim's target, its distance, its aim point's height above
  the target's feet (the game aims at a fixed point of the model, not the
  head) and whether the shot was `bent` (and by how much from the
  crosshair, plus the screen points before / after);
- `path to aim point`: the straight line from the shot's origin to the
  target's aim point (the bent aim point), tested with the bullets' ray
  (layer 1) and the aim ray (layer 32): `clear` or `blocked at N m`;
- `spread` (the gun's current spread), `cone` (spread plus the gun's base
  inaccuracy, screen units) and the spread angle the game wrote;
- `shot vs crosshair`: the shot's direction (origin -> the aim point the
  game fired at) against the crosshair, in degrees, and how many cm up / down
  that is at the aim point's distance; `vs bent point` when bent;
- `hit`: a character (`(target)` if it was auto-aim's target) and its hit
  box (`Head`, `Neck`, `UpperBody`, ...), another object (with its vtable;
  separate head models may show up this way), `world at N m`, or `nothing`;
- `damage`: the first character whose damage handler ran during the shot
  (`sub_8251EBB0` / `sub_8259B738`, f1) and how much.

# Button log

`button_log.cpp` (`pdz_button_log`, default `false`, needs `log_level =
"info"`) is for "the first press of reload / secondary fire sometimes doesn't
register". Read-only. It hooks the pad poll (`sub_82714230`), the per-frame
commit of every pad's edges (`sub_8226C2E8`) and the hand's button handling
(`sub_825781E0`) and writes `pdz_button:` lines:

- `user 0 X down, game registered the press` / `did NOT register the press`
  (time in ms since start, the game's frame counter `82D6B338`, poll count),
  and `X up after 140 ms`, for A / B / X / Y, LB / RB, LT / RT (> 30 of 255);
- `X (reload), right hand, clip 3/12 reserve 48: reload started` or
  `refused: gun busy, after a shot (action 5)` / `clip full` / `no reserve
  ammo` / `weapon locked` / `reload locked`, with the gun action before and
  after;
- `B (secondary), right hand: fire mode switch started` or `refused: gun
  busy, ...`;
- `N ms since the last pad poll` when polls are more than 25 ms apart.

What it showed in the code (`docs/BUG_AUDIT.md`, Controls #19): the game
polls the pad once per frame and works out its own edges (it never reads
`dwPacketNumber`), so `input_poll_on_read` can't eat a press; the gun takes a
reload only in actions 1 (idle), 5 / 6 (after a shot) and 12 (idle fidget)
and a fire mode switch only in 1, 10 (reloading), 12, 18 or while already
switching, only on the frame the button goes down, and drops it otherwise.

# De-rez effect log

`fx_log.cpp` (`pdz_fx_log`, default `false`, needs `log_level = "info"`)
follows the death "de-rez" effect (`docs/BUG_AUDIT.md` VR8-VR11): a
defeated multiplayer or holo character replaced by a cloud of cubes in its
colours. Read-only. `pdz_fx:` lines: the request (`sub_82510640`: mode 3
death / 1 spawn, caller, position), the slot grant and REZOUT sound
(`sub_825E69A0` from `sub_825106D8`), the snapshot resolve
(`sub_825E6AD8`, texture [82DA3958] and its memory), the GPU callback
(`sub_825E6BF8`), and the DeResThread's work (`sub_825E6CD0`): how many
of the 256 x 256 snapshot's pixels were non-black in guest memory when the
CPU read them, and per slot the particles spawned (state 2) or none (state
3, the effect ends). Slots ending their 1 s life are logged from
`sub_825E8370`. `frame_pacing_log`'s `K` / `k` rows show the frame's draws,
resolves and memexport draws around it.

# Game fixes

`game_fixes.cpp` fixes bugs in the original game: Rare's own fixes from
Title Update 3 (`pdz_fix_tu_*`: official fixes count as restoration) and
confirmed bugs found by the game audit (`docs/BUG_AUDIT.md`): the player's in
`game_fixes.cpp`, the enemy AI's in `ai_game_fixes.cpp`, the other Title Update 3 ports in
`tu_fixes.cpp` and (title update data, Facility / Ruin rules) `dlc_maps.cpp`. All on by default. Each needs the master switch (on by default), and
**F11** (`bind_game_fixes_ab`) flips it in game with a label listing the
active fixes.

| Setting | Default | What it does |
|---|---|---|
| `pdz_game_fixes` | `true` | Master switch |
| `pdz_fix_tu_cover_cooldown` | `true` | Title Update 3: after leaving cover (exit finished, exit started from cover, forced exit) the cover scan / prompt (`sub_824F4AB8` from the cover update) is skipped for 1.0 s of cover-update time (TU3 player +7520); a spawn clears it. Most likely Rare's fix for the cover wall clip |
| `pdz_fix_derez_snapshot` | `true` | Emulation fix, not a game bug: the death "de-rez" effect's 256 x 256 body snapshot (resolved into [82DA3958] by `sub_825E6AD8`, read on the CPU by the DeResThread `sub_825E6CD0`) is registered with the SDK's resolve readback ranges, so it reaches guest memory and the cubes appear. Off clears the snapshot memory (original: no cubes). BUG_AUDIT VR8-VR10 |
| `pdz_fix_tu_datacore_null` | `true` | TU3 (U7): the Datacore level script (`sub_822F8000`) skips its two tracked-character blocks when +2592 is 0 (mid-function hooks before `822F89A0` / `822F8D64`, jumping to `822F8CDC` / `822F8F98` like TU3) |
| `pdz_fix_tu_bot_task_null` | `true` | TU3 (U8): the four multiplayer bot task functions leave when the task's mode object (+76) is 0 (before `821BC518`, `821BC7F8`, `821C08E8`, `821BF590`). Never 0 in launch; a guard |
| `pdz_fix_tu_base_slot_null` | `true` | TU3 (U9): the base table lookup `sub_821766E8` skips empty slots (before `821766FC`); logs if it ever fires |
| `pdz_fix_tu_lv10_health` | `true` | TU3 (U11): the LV10 script also gives its list entry 14 the 15000 health entry 13 gets (hook before `823067BC` re-runs the call at `823067B8`); logs the character once |
| `pdz_fix_tu_township` | `true` | TU3 (U12): Township's hit-fix prop (its proplist, static and model taken from the title update package at boot) and 7 replacement background textures (set after the background setup on load / restart). Needs the title update's `1726605008.pkg` (`docs/MAP_PACKS.md`, Title update data) |
| `pdz_fix_tu_flag_drop` | `true` | TU3 (U13 + U17): the flag weapon's destructor's "flag dropped" (`sub_8240FFA0` / `sub_82440190` from `82569760`) is ignored when the holder being stripped (`sub_8253CB78`) isn't the carrier |
| `pdz_fix_tu_teleport` | `true` | TU3 (U16): after the multiplayer teleport's move (`sub_8242AB48`) the character is removed from and re-inserted in the world's spatial lists (`sub_82508F78` / `sub_82508EA8`) and its attached objects are moved with it (vtable +16 / +68) |
| `pdz_fix_tu_mp_hitfix` | `true` | TU3 (U18): Chinatown's and Desert's hit-fix props (proplists, statics, models, Desert's template entry) from the title update package, at boot |
| `pdz_fix_tu_pack_markers` | `true` | TU3 (U21): no out-of-bounds HUD markers on Facility / Ruin (hook before `82429DF0`) |
| `pdz_fix_tu_pack_map_rules` | `true` | TU3 (U22): Facility / Ruin option lists (scenarios 0 / 1, DarkOps 0 / 2, 2-8 players) and the Random map over every map (`docs/MAP_PACKS.md`) |
| `pdz_fix_halo_zoom_curve` | `true` | Standard ("Halo") look model below default horizontal sensitivity: field 3 (zoomed / cover curve exponent) blended min -> max (`825A1484` loads the max record); the max record gets the default's field 3 during the player's look update while the blend parameter is under 0.5 |
| `pdz_fix_autoaim_zoom_bend` | `true` | Zoomed shots bent by auto-aim land on the target: the weapon aim (`sub_822397C8`) divides its screen point by 1 - zoom (unzoomed units), but auto-aim projects the bent point after restoring the zoomed FOV (`824E24A0` before `sub_8260C210` at `824E24C8`), so the original overshoots by 1 / (1 - zoom) (5x with the P99). The bend (player +0x1F58 minus the reticle) is scaled by 1 - zoom for the weapon aim's call from `8257885C`, then restored. BUG_AUDIT WP16 |
| `pdz_fix_spread_cap_order` | `true` | The stance spread cap (byte +0xBD / 300 x 0.5 / 0.75 / 1.0 / 1.5) is applied to the spread passed to the shot direction (`sub_822397C8` called from `8257885C`), not just after it |
| `pdz_fix_cmp_noise` | `true` | The player's CMP150 (gun type 0x11) uses its per-shot noise (500) as the minimum instead of 5000 during `sub_8257FB90`: its first shot is heard 5 m away, not 50 m (every other firearm has minimum = per-shot) |
| `pdz_fix_ai_semiauto_rhythm` | `true` | AI14: the shoot action (`821A29B8`) calls `sub_821A5CB8` every frame, which for semi-auto guns rolls `rand(0, rand(1,2))` (0 = press, else release; a shot needs a fresh press), so at 60 fps semi-auto enemies get ~14.6 presses/s instead of ~7.3. The inner roll (`sub_826F28C0` returning to `821A5D88`, action in r31) is kept per action for 1/30 s of AI clock. The AI pad (vtable `8209C088`) only stores triggers in its setters (`8226C638` / `8226C640`), so a held value stays |
| `pdz_fix_ai_grenade_height` | `true` | AI18: `821ABA00` uses scale 1.0 for targets more than 100 cm above / below and its "more than 300 cm -> 1.3" test sits behind that one. The launch point (`sub_825762D0` from `821ABB28`) is recorded and, over 300 cm, the point passed to `sub_82188D48` (from `821ABBB4`) is redone with 1.3 |
| `pdz_fix_ai_dodge_facing` | `true` | AI20: the dodge task (`sub_821CB0C0`) takes `rand() % 3` (`821CB628`) for four facings, so FacingRight (151 / 152) never plays. The roll (`sub_826F2808`) is redrawn % 4; the fourth runs the FacingFront branch and gets 151 / 152 and a forward clearance ray when `sub_821CAC10` is called (it saves the caller's r31, the animation type) |
| `pdz_fix_ai_dodge_clearance` | `true` | AI21: the clearance ray for backwards dodges (150 / 152) runs along the enemy's yaw (in front); the yaw `sub_821CAC10` reads (`sub_8250B3E8` from `821CAC2C`) is turned 180 degrees for them |
| `pdz_fix_ai_corner_speed` | `true` | AI23: the corner speed curve lookups (`sub_8218A6E0` / `8218CDD8` / `8218F4D0`) loop i < n-2, so the last segment (85-90 degrees) returns 0 (a stop). When one returns 0 for an angle in that segment, the segment is interpolated (the curve is left in r3) |
| `pdz_fix_ai_cover_min_distance` | `true` | AI35: the planner's cover goals pick pads (`sub_821B2E88` / `sub_821B2DC8` -> filter `sub_82155EB8` -> `sub_82156C50`) with a minimum distance from the target of max(weapon range #1^2, 700) but re-check them every tick with a fixed 10 m (`sub_821B3030`), so a pad picked closer is dropped the next tick and picked again (wounded enemies turn for cover and come back). While those two pickers run, passing pads at or under 10 m from the target position the filter used are rejected. The re-check is unchanged; other users of the filter are untouched |
| `pdz_fix_ai_calm_flag` | `true` | AI10: `sub_82184618` sets the perception slot's calm flag (+68 0x10) from the count before clamping; after a call that changed the slot the flag is set from the stored count |

Checked and left as shipped: AI9, footsteps not alerting guards on their own
(design decision: stealth), and AI30, the tank scientist's `rand(0,0)`
animation pick (it keeps him on his tank idle; the other entries are
pistol talking animations, so a wider roll isn't clearly intended).

Gun noise: each shot sets the shooter's noise slot (table `82E5C7C8`,
channel 1) to clamp(old + record +0xF0, +0xF4, +0xF8); it holds 2 updates,
then decays 1500 cm/s and clears 2 s after the last shot (`sub_821861E8`).
An AI hears it while its distance is below value x its hearing factor
(+0x38, `sub_82185EF8`).

# Music log

`music_log.cpp` (`pdz_music_log`, default `false`, needs `log_level =
"info"`) logs the music manager, read-only, so the dynamic music can be
checked in a level:

- `type A -> B`: music type changes (None, Interactive, Tune1-4, Death) from
  `sub_821F8B28`, with the sound it started (0 = failed) and the caller
  (level scripts call it directly);
- `interactive level A -> B`: the interactive music's level (Silence,
  Stealth, Speedup), state and pending action, polled after the music tick
  `sub_821F86C0`;
- `cue "XACT_SOUNDBANK_MUSIC_..."` and `interactive layer ...`: music sounds
  created by `sub_821F70D0` (`FAILED` when the cue doesn't exist);
- pause / resume / stop.

With `pdz_music_log` on, `mp_music.cpp` also writes `pdz_mp_music:` lines
(scenes, the Music row, value changes, banks, tracks).

# Multiplayer music

`mp_music.cpp` (`pdz_mp_music`, default `true`; enhancement) adds a MUSIC
row to the multiplayer setup screens. Its value starts at **Off**, which is
the original game: multiplayer only plays each map's ambience. The other
values are Random and 38 of the game's own soundtrack pieces, named after
their mission ("SUBWAY STEALTH", "BRIDGE ASSAULT", ...; menus and credits by
use: "MAIN MENU", "CREDITS"). The choice is kept per local profile
(`mp_music.txt` next to its saves).

- **Row.** The setup screens are compiled XUI scenes whose rows are
  `DarkXuiValueButton`s bound to a `DARKVALUE` by name (`GameValue`). When a
  create-game scene is made (XuiSceneCreate `sub_8277F7F0`), a row is created
  under each POINTS LIMIT row (every scenario page) with XuiCreateObject
  (`sub_82776858`) and the POINTS LIMIT row's own visual, size and anchor (the
  game's own widget and font), labelled MUSIC; the rows below move down one
  row and up / down navigation is relinked; then it is initialized like a
  loaded element (`sub_8277EB10`). TU3 added its bot personality row the
  same way in data (new scenes plus a new value id).
- **Start.** The row also copies POINTS LIMIT's `PressKey`. XuiButton's
  default press key is "accept" (A or Start, `sub_8278DA28` from its key
  handler `sub_8278DBD8`), so without it the row took Start as its own press
  and did nothing; the game's rows leave Start to the scene's launch button
  (`buttonCreate`, a `DarkXuiLaunchButton`), which moves on to the next setup
  screen. A / left / right / B are handled by the value button itself
  (`sub_823F4940`) and are unchanged.
- **Value.** `DARKVALUE_MUSIC` is a new value id (120), served by hooks on
  the game's per-value functions: name -> id `823AFB48`, get `823AFC00`, set
  `823B14D0`, previous / next `823B3350` / `823B3648`, count `823B1950`, locked
  `823B1898`, text `823B2E48`. The game's saved match settings (ids 0-32) have
  no room for another one, hence the file.
- **Tracks.** Every soundtrack cue is one stereo wave in a level's music bank
  (`music_lvNN`) or a menu bank, authored to loop for ever (intro once, then a
  loop region), except the title piece, which plays once (restarted). The
  interactive levels keep each layer (Stealth, Speedup) as its own full mix,
  so a layer's cue is one fixed full mix ("... STEALTH" / "... ACTION").
  Several cues are silent unless their `Ambivol` variable is 1, so it is set
  on every track (`sub_821FE378`).
- **Playback.** After the level's audio is loaded (`sub_821F1330`), the
  track's banks are loaded the way the bank lists do it (wavebank
  `sub_821F1E48`, soundbank `sub_821F2050` into its entry of the sound bank
  table) unless already loaded (PAUSE, RESULTS, GAME OVER are). When the
  level's audio starts (`sub_821F1470`) the track plays over the ambience the
  way the music manager plays a tune (`sub_821FAC08`, `sub_821F70D0`,
  `sub_821FE058`), at the music volume. It pauses / resumes with the game's
  music pause (`sub_821F8A78` / `sub_821F8AC0`; multiplayer matches don't
  call them, like their ambience), stops in the audio idle check
  (`sub_821F1888`) that leaving a level waits on, and its banks are unloaded
  in the level's audio teardown (`sub_821F1690`). Random picks a track per
  match and another once it has played through (music tick `sub_821F86C0`).
  Multiplayer only (game mode 2 / 3): single player and co-op are left alone.
