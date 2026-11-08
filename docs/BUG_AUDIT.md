# Bug audit

Bugs found in the original game's code and data (Perfect Dark Zero retail,
4D5307D3), what each looks like it was meant to do, and the evidence.
Addresses are guest addresses in the recompiled image; data is from the
compiled XML records in `assets\pdz\ASSETS\PackageData`.

Verdicts: **bug** = confirmed in code/data and clearly wrong; **likely bug**
= confirmed in code/data, intent inferred; **deliberate** = intended or no
evidence otherwise; **unknown** = needs an in-game check; **fine** =
correct. Impact = how much a player would notice (high / medium / low).
"Confirmed" says how: *read* = the decisive instructions or bytes were read
and are quoted; *2nd read* = re-checked independently by a second pass;
*test* = seen in game.

Baseline for the audit: every enhancement off (look, aim, FOV, ragdoll), the
recomp fixes (vblank pacing, latency) and the enemy cover fixes on,
`pdz_fps60_unlock` on (the game shipped at 30 fps; differences are flagged).

## Fix defaults (2026-10-19)

Every confirmed fix is on by default; enhancements are off.

| Fix | Setting | Default | A/B key |
|---|---|---|---|
| Even frame pacing, late latch 3 ms, controller read on demand (recomp) | `host_vblank_pacing` (needs `vsync`), `host_vblank_late_latch_ms`, `input_poll_on_read` | on (code and `hardware.toml`) | F8 (latch / poll) |
| Enemy cover: grace, peek roll, snap-back | `pdz_ai_cover_fixes` + `pdz_ai_cover_fix_*` | on | F5 |
| TU3 cover cooldown (Rare's fix) | `pdz_fix_tu_cover_cooldown` | on | F11 |
| TU3 null checks: Datacore script (U7), bot tasks (U8, a guard), base table (U9) | `pdz_fix_tu_datacore_null`, `pdz_fix_tu_bot_task_null`, `pdz_fix_tu_base_slot_null` | on | F11 |
| TU3 LV10 entry 14 health (U11) | `pdz_fix_tu_lv10_health` | on | F11 |
| TU3 Township textures and hit-fix prop (U12; needs the title update's data package) | `pdz_fix_tu_township` | on | F11 (textures at the next load / restart; the prop at boot) |
| TU3 flag drop only from the carrier (U13 + U17) | `pdz_fix_tu_flag_drop` | on | F11 |
| TU3 teleport re-insert + attached objects (U16) | `pdz_fix_tu_teleport` | on | F11 |
| TU3 Chinatown / Desert hit-fix props (U18; needs the data package) | `pdz_fix_tu_mp_hitfix` | on | restart (boot) |
| TU3 Facility / Ruin HUD markers (U21) and option / Random rules (U22) | `pdz_fix_tu_pack_markers`, `pdz_fix_tu_pack_map_rules` | on | F11 |
| H5 Standard zoom curve | `pdz_fix_halo_zoom_curve` | on | F11 |
| WP4 spread cap order | `pdz_fix_spread_cap_order` | on (inactive until 2026-10-20: a pointer check skipped the player's weapon) | F11 |
| WP16 zoomed auto-aim bend overshoot | `pdz_fix_autoaim_zoom_bend` | on | F11 |
| WP9 / TD3 CMP noise | `pdz_fix_cmp_noise` | on | F11 |
| De-rez death effect snapshot (emulation fix, VR8-VR10) | `pdz_fix_derez_snapshot` | on | F11 |
| AI35 enemy cover pick minimum distance | `pdz_fix_ai_cover_min_distance` | on | F11 |
| AI14 enemy semi-auto rhythm at 60 fps | `pdz_fix_ai_semiauto_rhythm` | on | F11 |
| AI18 enemy grenade height test | `pdz_fix_ai_grenade_height` | on | F11 |
| AI20 enemy dodge facing (% 4) | `pdz_fix_ai_dodge_facing` | on | F11 |
| AI21 enemy backwards dodge clearance | `pdz_fix_ai_dodge_clearance` | on | F11 |
| AI23 enemy corner speed curve | `pdz_fix_ai_corner_speed` | on | F11 |
| AI10 enemy alert calm flag | `pdz_fix_ai_calm_flag` | on | F11 |
| Still-lock (aim enhancement, kept on by request) | `pdz_aim_still_lock` (needs `pdz_autoaim_tuning`) | on | F10 |
| Headshots kept (aim enhancement, on by request for testing) | `pdz_aim_respect_headshots` (needs `pdz_autoaim_tuning`) | on | F10 |
| Rotational assist while zoomed (aim enhancement, on by request) | `pdz_aim_assist_zoomed` (needs `pdz_autoaim_tuning`) | on | F10 |
| Controls fixes (parked) | `pdz_controls_fixes` + options | off | F2 |

Off by default: look tuning, the other aim tweaks (close-range radius,
friction, difficulty override), FOV and ragdoll tuning. Diagnostics are off
and `log_level` is `critical`. `host_vblank_pacing` only phase-locks on a
display that is a multiple of 60 Hz (60 / 120 / 240); other rates fall back
to a steady timer, and on G-Sync / FreeSync the display follows the presents.

## Summary (game audit 2026-10-18)

Most systems are sound and timed by real time, so 60 fps changes very little.
No outright high-impact code bug was found outside what's already fixed or
parked. The notable findings:

- **Enemy cover is rare by design, not by bug**: a cover spot must be 10-65 m
  from the player and in its facing arc, and below 5.5 m there is no cover
  plan at all. Most fights happen closer.
- **The cover prompt is unreliable while running**, worse at the shipped
  30 fps (frame-counted scan that restarts after 1.5 m).
- **Data outliers**: the CMP's gunshot noise minimum is 10x every other gun's
  (5000 vs 500), the PD default fast-look accel, a test motion table with two
  values that look like typos, and two secondary-fire records with the wrong
  gun type.
- **One latent crash path** in a level script (spawn result used without a
  null check).
- **No LOD system exists**: distant enemies look like blobs because of fog,
  the narrow original FOV (45 deg vertical, ~73 deg across) and host
  resolution / filtering, not distance LOD.
- **The parked controls findings may be Classic PD behaviour**, not bugs: see
  the Controls section.

## Player cover

| # | What the code does | Seems meant to | Evidence | Confirmed | Verdict | Impact | Smallest fix |
|---|---|---|---|---|---|---|---|
| CV1 | Cover prompt scan: one probe per frame (9-15 frames for a full scan), result fresh for 20 frames, scan restarts when you move > 1.5 m from its start, prompt only within 1.3 m / 40 deg of the scan start. Running (~7 m/s top) restarts most scans before they finish; at 30 fps a scan takes 0.3-0.5 s (2-3.6 m of running) | a prompt that appears as you approach cover | `sub_824F4630` (one `sub_824F4DB8` per call, restart `824F4728`), freshness `824F4AFC`-`4B3C` (frame counter 82D6B338 + 20), gates `824F4CE4`/`4D24` | read, 2nd read | likely bug (frame-counted, tuned for a faster frame rate or slower movement) | medium | `pdz_fix_cover_scan`: run as many probes per update as 1/60 s steps have elapsed and count freshness in seconds (0.33 s) |
| CV2 | Lean-out / lean-back end on an animation event (mover +0x25A set by `sub_82685B70` from the event dispatcher), at ~0.8x the clip: measured 200 ms out, 430-635 ms back | as coded | `824F3C58`-`3C60`, `821E6D60`; log 065 | read, test | deliberate | - | none |
| CV3 | Cover camera FOV: 58 x zoom factor, same sequence as the gameplay camera; split-screen 45/58 scaling applies to whichever camera is active | as coded | `824F3398`-`33B0` vs `8220E448`-`E484`; `82205478`-`5490` | read | fine (only our `pdz_fov_scale` misses it) | - | (enhancement: extend `pdz_fov_scale` to cover) |
| CV4 | Both cameras compare a mode value with 2 and never use the result | a removed 2-player FOV branch | `8220E440`, `8220E46C` | read | dead code, harmless | - | none |
| CV5 | Entry 0.5 s with turning locked and exit ignored; firing blocked until fully leaned out; instant exit from states 2 / 4 | as coded | partA notes | read | deliberate | - | none |
| CV6 | Toggle Cover Mode: the press that enters can't also exit (exit test skipped during entry). Hold mode: a tap gives 0.5 s of cover | as coded | `824F2F04` | read | fine / deliberate | - | none |
| CV7 | Hold mode side switch uses player action 0xF instead of the stick | unknown button | `824F33C8`-`340C` | read | unknown | low | log action 0xF in cover |
| CV8 | Cover pitch limit +-42.5 deg (85 x 0.5), not +-29 as earlier notes said | - | `824F0EC0`, `824F115C`-`11A8` | read | fine (correction) | - | none |
| CV9 | Community: wall clip / out of bounds by entering cover at corners or being knocked back while in cover (patched in later title updates) | cover moves checked against collision | speedrun guides (B grade) | web | bug (patched by Rare in a later exe; ours is the launch exe, unpatched) | low | **fixed: `pdz_fix_tu_cover_cooldown`** (TU3's 1 s cooldown after leaving cover, on by default) |
| CV10 | **Lean-out drops the look stick for ~0.3 s.**<br>- **Order:** the look controller turns the view first (`825981D8`); the cover tick runs later in the same update (`82598368` -> `sub_824F2E20`).<br>- **Hold condition:** state 2, or the first tick after it, or the 0.3 s camera blend timer +0x1CA0 running (set at lean-out by `sub_824F7838`, `824F292C`), with the blend byte +0x1C78 clear (`824F381C`-`3844`, `3898`-`38B4`).<br>- **Effect:** the cover code then recomputes yaw / pitch from the camera position (+0x1C88) to a look-at point held 500 m out (`824F390C`-`3A3C`), clamps them, and writes them over +0x1670 / +0x1674 (`824F3B9C` / `3BA4`), so that tick's stick turn is overwritten | keep the crosshair on the same world point while the camera swings to the pop-out pose | as quoted | read, 2nd read | deliberate mechanism; losing the stick is a side effect (the edge step, CV12, keeps the stick during the same kind of swing) | high (feel): the crosshair freezes for ~0.3 s on every pop-out | `pdz_fix_cover_look_hold` (proposal, not built): before `sub_824F2E20`, under the same condition, set +0x1C78 = 1 so the game takes its own edge-step blend (eases over the rest of the 0.3 s but keeps the stick); and save / restore +0x1670 / +0x1674 around `sub_824F26C8` (CV16) |
| CV11 | **Lean-back holds the view for the whole lean-back**: 430-635 ms measured (0.8 s nominal), +1 tick; `sub_824F29A0` sets the timer (`824F2B24` / `2B6C`) and state 2 lasts the whole animation. Firing again restarts the lean-out and the hold | as CV10 | `824F2B24`, `824F2B6C`, `824F381C` | read | as CV10 | high (longest hold) | same fix |
| CV12 | **Edge step / side switch does not hold:** `824F35F0` sets +0x1C78 = 1, so `824F38B8`-`3908` blends the look-at point from the saved one to the stick-driven one over 0.3 s (weight = timer x 3.333) and +0x1670 keeps the stick | - | `824F35EC`-`35F0`, `38AC`-`3908`, `45A8` (flag cleared when the timer ends) | read | fine (the model for the fix) | - | none |
| CV13 | Crouch / stand toggle at an edge starts the blend (`824F427C`) without the flag, so it also holds the view for 0.3 s | as CV12 | `824F427C`-`4290` | read | likely oversight (same family as CV10) | low-medium | covered by the CV10 fix |
| CV14 | **The input is discarded, not delayed.** The look scales are zeroed only in state 1 (entry) and at the wall limits, and no look-disable ability bit is set, so velocity V keeps building and is applied each tick, then overwritten. When the hold ends the view moves at the current speed from where it was held: no jump and no ramp from zero. The turn made during the hold is lost (e.g. 30 deg/s x 0.3 s = 9 deg per pop-out); the rotational assist's turns too | - | `824F2ED0`-`2EF8`, `sub_824F0CF0`, `825A43B8`-`4514`, `825A1994` / `1B2C` | read | (explains the feel) | - | - |
| CV15 | The cover camera is not driven by the animation: orientation = the held point; position = the body-relative pose blended over the 0.3 s timer (`824F0310`-`0370`) | - | as quoted | read | - | - | - |
| CV16 | `sub_824F26C8` zeroes yaw / pitch on a shoulder-swap pop-out (`824F2980` / `2984`); the CV10 hold overwrites it next tick, so it never shows | re-centre (superseded) | `824F2930`-`2984` | read | dead in effect; would show as a snap if CV10 were fixed alone | none now | part of the CV10 fix |
| CV17 | The hold is time-based (0.3 s timer by dt, fixed animation lengths); only the one-tick tail is per frame, so 60 fps doesn't change it | - | `824F4578`-`459C` | read | fine | - | none |

## Player movement

| # | What the code does | Seems meant to | Evidence | Confirmed | Verdict | Impact | Smallest fix |
|---|---|---|---|---|---|---|---|
| MV1 | Speeds 12 / 10 / 10 cm per 1/60 s (forward / back / strafe) x 60 dt; acceleration pow(1 - k (1 + stick) ..., 60 dt), time constant ~0.27 s | as coded | `8259EF2C`-`EF6C`, `8259F010`-`F02C`, `8259EF8C`-`EFA8` | read | fine (same at 30 / 60) | - | none |
| MV2 | The only movement record comes from `test\movementparamsjo` | - | data | read | naming leftover, values sane | - | none |
| MV3 | Male "forwards" anim speed bands differ from female (e.g. stance 0.4-1.1 vs 0.9-1.1) while male backwards / strafe are byte copies of the female records | maybe all retuned together | anims bipedanimcontrol records | read | unknown (male NPCs only) | low | none until seen |
| MV4 | Roll: dt-based; frame counter only for a profile statistic | - | `824EEF08`, `824EF1E0` | read | fine | - | none |
| MV5 | Community: rolling into some cliffs / between buildings goes out of bounds | - | speedrun guides | web | collision gaps (level data) | low | none |
| - | Not traced: crouch hold/toggle, step-up and slope snagging, Classic PD one-stick turning | | | | | | |

## Weapons and combat

| # | What the code does | Seems meant to | Evidence | Confirmed | Verdict | Impact | Smallest fix |
|---|---|---|---|---|---|---|---|
| WP1 | Fire rate driven by shoot / rapid animation events, advanced by dt in sub-steps (max 100 events per update); gun timers dt-based; high-rate class fires 240 x dt per update | as coded | `822332AC`-`82233404`, `82233098`/`822330AC`, `8224E468`-`E4CC` | read | fine (same at 30 / 60) | - | none |
| WP2 | Charge meter in 30 fps "frame" units, converted x1/30 and drained by dt | - | `8224DDA4` | read | N64-era units, consistent | - | none |
| WP3 | Spread bloom per shot = byte +0xBC x base x stance / 300 (base 0.125; higher for hand-1 guns when +0xFF4 == 2); stance 0.5 / 0.75 / 1.0 / 1.5 | as coded | `82578044`-`8257819x` | read | deliberate | - | none |
| WP4 | The shot is aimed with the current spread before the stance cap is applied: a shot right after a bloom can exceed the cap by one step | cap, then aim | `8257881C` load, `8257885C` aim call, cap `82578860`-`8257894C` | read, 2nd read | likely bug | low | **fixed: `pdz_fix_spread_cap_order`** |
| WP5 | Spread recovery: min(hand idle times) > 0.2 s, dt-based; only for owner type 2 | as coded | `825737BC`-`37C4`, `82573790` | read, test (0 empty-hand cases) | fine | - | drop `pdz_fix_spread_recovery_gate` |
| WP6 | Shotgun range 20 m (others 500 m) | as coded | `82238A64` | read | deliberate | - | none |
| WP7 | Damage categories (`gunparams\gundamagecategories\gun.xml`, 15 rows x 3 groups of 3): some rows contradict their names (Laptop "armour stripper" has 0s in group 2, Plastigun "GOOD_VS_ARMOUR" 0s in group 3, RCP "GOOD_VS_ARMOUR" equals the P99) | rows matching their names | gun record +0x98 selects the row (`8255C694` ...) | read (data); group meanings not traced | unknown | medium if real | trace the groups / log damage per hit |
| WP8 | Visible-touch misses: which shape the hitscan ray tests and whether the target pose is this update's is not traced | - | ray `sub_82252798`, sweep `sub_8225A860` | not traced | unknown | medium | `pdz_weapon_log` first |
| WP9 | CMP gunshot noise: every gun adds +0xF0 per shot to a per-shooter noise slot clamped to [+0xF4, +0xF8]; all guns have +0xF4 equal to +0xF0 (or half for melee / throwables) except the CMP: 500 / 5000 (MP5 1500 / 1500) | 500 (or 250) like the others | `sub_8257FB90`, table `82E5C7C8` read by AI code; gun records | read (data verified; meaning confirmed: noise slot = clamp(old + F0, F4, F8) at `825800D8`-`80EC`, heard while distance < value x listener hearing factor in `sub_82185EF8`) | likely data bug (a stray zero) | medium-low (first CMP shot heard ~50 m away vs ~5 m) | **fixed: `pdz_fix_cmp_noise`** (minimum = per-shot, 500, the pattern every firearm follows) |
| WP10 | Secondary-fire records: `ak47secondary` gun type 0 (Null), `superdragon_secondary` 13 while the enum has SuperDragon_Secondary (52); code checks 52 for hit-reaction strength (x2) | type 52 | `sub_8255C740`; records | read | unknown (projectiles may take another type) | low | log the type on hit |
| WP11 | MP5 tracer speed 250; every other SMG / pistol 500 | 500 | records | read | unknown (possible typo) | low | none until seen |
| WP12 | M16 record carries the Walther silencer sound cue; its grenade launcher uses silencer-style 40 / 100 / 300 noise parameters (the MagSec's grenade uses 2500 / 2500 / 13500) | grenade-like values | records | read | likely data copy-paste | low (quiet M16 grenades) | none until seen |
| WP13 | Four gun classes pass (0, +9.81, 0) to Havok (`sub_8223D8A0` -> `sub_829B2440`) while world gravity is (0, -9.8, 0) | - | | read | unknown (may not be gravity) | low | none |
| WP14 | Title update (Rare, May 2006): switching rapidly between the Plasma Rifle and a hovercraft turret made it fire 10x faster; rocket launcher alt-fire zoomed other players' screens in split screen | fixed by the TU | IGN quoting Rare | web | **not confirmed in our build**: hardpoint turrets have their own gun object and a per-kind cooldown (plasma 0.0 s, `8257DB3C`-`DB68`), but no path applying turret state to the player's Plasma Rifle was found; XEX version 0.0.0.4 (launch). TU3 diff: no turret fire-rate change; its re-equip change only extends the turret weapon swap to type-42 occupants (see Title Update 3 changes) | - | measure first: Plasma Rifle shots per second (`pdz_combat_log`, gun = Plasmarifle) before and after mounting a hovercraft turret |
| - | Not covered: per-gun record outlier scan of all 0x4D4 bytes, damage application, headshots, health / armour / shields, explosion falloff, melee | | | | | | |

### Parked (play-test reports, 2026-10-20)

Both are logged by `pdz_shot_log` (diagnostic, off by default, needs
`log_level = "info"`; one `pdz_shot:` line per shot, see `src/mod/README.md`).
No behaviour has been changed.

How a player shot is aimed (read for the log; launch addresses):

- The aiming update (`sub_825A34F0`) runs auto-aim (`sub_824E2028`), then
  per hand the weapon aim (`sub_825787F0`), which calls `sub_822397C8` with
  the screen point at player +0x1F58: auto-aim's bent point when it bends,
  otherwise the crosshair. `sub_822397C8` moves that point at random within
  the spread cone (gun spread + (record byte +0xB2 - 1) / 300), casts the aim
  ray (layer 32) from the camera (player vtable +652 object +948) through it,
  and stores the first thing it hits, 2 cm past it (or 500 m out), as the
  hand's aim point (player +0x1FF0 + 12 x hand).
- The shot (`sub_82238938`, e.g. from `sub_82236E40`, which passes the same
  camera position and that aim point) casts the bullets' ray (layer 1) from
  its origin towards the aim point, then sweeps for characters along it;
  each object hit goes through `sub_82512D48`, and a character reads the hit
  box from the hit entry (+0x34, HitBox enum: -1 none, 0 Head, 1 Neck, 2
  UpperBody, ...).
- Auto-aim's target point (auto-aim +0x24) is `sub_82535E00`: a fixed point
  from the character's skeleton data (+324 block) moved by the model's world
  matrix. It is **not** the head bone. When the shot is bent, it goes to
  that point, wherever the crosshair was on the target.
- Target selection needs line of sight from the eye to that point on the aim
  ray's layer (32), every update (`sub_824E3A10` at `824E2A94`).

| # | Report | What we know | Leads to check in the log | Status |
|---|---|---|---|---|
| WP15 | **Auto-aim shot bend ignores walls** (play-test report, 2026-10-20). Firing at an enemy beside a wall on the left while holding the aim still (`pdz_aim_still_lock` bending the shots). The enemy stepped left behind the wall; the crosshair stayed on open space, but many more bullets hit the wall: the bend kept pulling shots towards a target the player could no longer see. (The report text was cut off after "towards a target the"; the ending is our reading.) | The bend goes to the target's single aim point (above), not to where the crosshair is. Selection checks line of sight to that point only, on layer 32; the bullets use layer 1. So the bend stays on while that one point is visible from the eye, even with most of the body behind the wall, and the spread cone around a point at the wall's edge puts part of the shots into the wall. With `pdz_aim_still_lock` the bend also applies while holding still (the original only bends while turning fast), so our enhancement makes this case more common | per shot: `bent`, `path to aim point: bullets ... / aim ray ...` and `hit world at N m` vs the target's distance. Bullets blocked while the aim ray is clear = a layer mismatch (bug in our use of the bend or in the game's test); both clear but hits on the wall = the spread cone at the edge; both blocked while still bent = the line-of-sight test isn't doing what it should | parked (decision); diagnostic built |
| WP16 | **Zoomed shots bent by auto-aim overshoot by 1 / (1 - zoom)** (play-test report, 2026-10-20: from cover, zoomed P99, crosshair on the head of an enemy ~106 m away, every shot hit the cliff below him) | **Cause found (read + test, logs 129-131).** The weapon aim divides its screen point by the zoom factor (`sub_822397C8` `82239950`-`8223998C`: x, y / vtable +656 = `sub_8259D0D8` = 1 - player +0x1A9C), so it expects a point in unzoomed units, the units auto-aim's lock test uses (reference FOV set at `824E218C`). But auto-aim restores the zoomed FOV (`824E24A0`-`824E24AC`) **before** projecting the target to get the bent point (`sub_8260C210` at `824E24C8`), so the bent point is in zoomed units and the shot lands 1 / (1 - zoom) times as far from the crosshair as the target's aim point (5x at the P99's 0.8). Measured: 52/52 bent zoomed shots from cover 0.5-1.2 deg low, each matching 5x the aim point's offset (e.g. screen y -0.061 -> target 0.27 deg below the crosshair, shot 1.36 deg); zoomed outside cover the same 5x (smaller, the aim point was near the crosshair); unbent shots exactly on the crosshair; with F10 (no still lock) no bends and no misses. Same camera object and FOV for both sides (log). In the original it only shows while zoomed and bending: Agent / Secret Agent while turning fast, and zoomed sniper / Shockwave on every difficulty; `pdz_aim_still_lock` makes it every still zoomed shot | - | **bug** (Rare's). Fix proposal `pdz_fix_autoaim_zoom_bend`: scale the bent point (player +0x1F58) by (1 - zoom) for the weapon aim call (825787F0 -> `sub_822397C8`), restore after. **Built 2026-10-20: `pdz_fix_autoaim_zoom_bend`, on (F11).** Separately, at the user's request: `pdz_aim_respect_headshots` (enhancement, F10): with the crosshair over the target's screen box the bend keeps the player's aim height and only corrects sideways, since the game bends to a fixed body point, not the head |

## Enemy AI

| # | What the code does | Seems meant to | Evidence | Confirmed | Verdict | Impact | Smallest fix |
|---|---|---|---|---|---|---|---|
| AI1 | Cover goals need a pad with cover flags 0x4 / 0x8, the player inside its facing arc (30 deg for 0x8, 90 otherwise), **10-65 m from the player** (squared 1e6 / 4.225e7 cm^2), and line of sight from the pad and its peek pads; candidates within 35 m of the enemy; no cover plan at all with the player under 5.5 m | as coded | `sub_821B3030` passes 1e6 / 4.225e7 at `821B30A8`/`30B0`, compared at `82156CF8`/`6D00`; `821B2128` | read, 2nd read | deliberate (explains why cautious enemies rarely use cover in close fights) | high (feel), not a bug | none as a fix; a redesign option would lower the 10 m minimum |
| AI2 | Plans are rebuilt only when none is running; cautious plans never time out; other plans are abandoned after 6 s unless the enemy was hurt and fired in the last 6 s (**corrected in AI36:** +0x30F8 is "target last seen", so it is "unless it still sees the target or has fired in the last 6 s") | as coded | `821B0FF4`-`1040`, `821B0F30`-`0FB0` | read | deliberate (frequent replanning may add to "streaming") | medium | none |
| AI3 | Every cover test returns 0 when the player's current pad (+0xB4) is null | as coded | `82156C78`, `821C7E18` | read | unknown how often | possibly high | log how often player +0xB4 is null |
| AI4 | Plan-node pool of 350 (`82E2E4E8`); a plan needs >= 4 free nodes or none is built | as coded | `sub_82141DE8` | read | unknown whether it runs low | possibly medium | log free nodes (`sub_821B0A78`) |
| AI5 | Cover-in-place task lasts 10 s and claims up to 5 pads (earlier notes said there was no occupancy at all: wrong for this task) | as coded | `821C67D4` | read | deliberate (correction) | - | none |
| AI6 | **Corrected 2026-10-19:** [`82D6B2CC`]+0x2F9 is not an attack limiter. It counts running shoot tasks: Start `sub_821DA940` adds 1, Stop `sub_821DA860` / Finish `sub_821DB5C0` take 1 away, all guarded by a latch at +0x60. Its only reader is a global combat-intensity state, `sub_821F8708` (`821F8820`). Every removal path calls Stop before the destructor (`8216AFE0`, `8216BA40`), so there is no leak, and it has no effect on who attacks | - | `821DA974`, `821DA8A4`, `821DB600`; four accesses to +761 in total | read, 2nd read | deliberate | none | none |
| AI7 | No frame-counted logic or per-frame random rolls in the planner or any of the 84 task updates | - | scan | read | fine for the planner and the cover tasks (same at 30 / 60). Exceptions found later: the shoot action's per-frame semi-auto trigger roll (AI14, FR9) and per-frame re-rolls after a failed dodge (AI22) | - | none |
| AI8 | Enemy cover task bugs (grace timers, peek roll, snap-back) | | see Enemy cover below | | fixed (`pdz_ai_cover_fix_*`) | | |

### Perception, accuracy, grenades, dodge, navigation, squads, companions, difficulty (audit 2026-10-19)

This is a static read of the launch image; nothing was run. "Read" means I read the quoted instructions; "2nd read" means I read them again independently before calling something a bug. Background:
- **Units and clock:** distances are in cm. Most AI timers run on the AI clock (`82E2E478`, a sum of dt), so they are unaffected by the 60 fps unlock. The exceptions are listed in the frame-rate table (FR9-FR12).
- **Perception data:** perception keeps one 80-byte slot per character in table `82E5C7C8` (170 slots; the index is at char +0x354).
- **Difficulty:** the difficulty value is `*(82D6B2B0)+4` (0 Agent, 1 Secret Agent, 2 Perfect Agent, 3 Dark Agent).

| # | What the code does | Seems meant to | Evidence | Confirmed | Verdict | Impact | Smallest fix |
|---|---|---|---|---|---|---|---|
| AI9 | **Footsteps never alert anyone on their own.** The hearing check `sub_82185EF8` loops over every noise maker. When a listener is inside another character's footstep radius (slot +0x28: +400, or +200 when quiet, capped at 400 = 4 m; written by `821F79AC` via `sub_82186580`), it sets "heard noise" 0x40 (and 0x80) in the listener's slot flags. After the loop it asks the sound-event list (`sub_82185DB0`, which returns true only for its own events), and when that answers false it **clears 0x40** (`82186154 rlwinm r11,r11,0,26,24`). So the footstep result survives only when an unrelated sound event (impact, body fall, explosion) is in range too. 0x40 is the bit the NPC reads (`82571E7C`) to set "heard a noise" (char +0x1874 \|= 4). No reader of 0x80 was found. | heard = footstep heard **or** event heard | `821860D8`-`821860F8`, `82186130`-`82186158`, `82185DB0` (r26 set only by its own list), `82571E7C` | read, 2nd read | **likely bug** (footstep hearing is dead) | medium: walking or running up behind an unaware guard never alerts them, so stealth is easier than coded. Caveat: the footstep branch has no team filter, so restoring it as is would make guards react to their own squad's footsteps. That may be why it went unnoticed or got "tuned" this way | left as shipped (design decision: stealth) |
| AI10 | Alert counter `sub_82184618` adds or subtracts 1 from slot +0xC and sets "calm" 0x10 from the **unclamped** result before clamping to 0..100. Subtracting at 0 gives -1, which clears "calm" while the stored count stays 0 | calm == (count == 0) | `82184660`-`821846E4` | read, 2nd read | likely bug | low (no reader of 0x10 found; many tasks reset it by hand) | **fixed: `pdz_fix_ai_calm_flag`** (on) |
| AI11 | Heard-source handle slot +0x4C is written only when something is heard and never cleared; an event without an owner doesn't overwrite it. The investigate planner (`sub_821C9688`) reads it | the nearest current source | `82186100`, `82185ED4`, `821C9B04` | read | unknown (possibly stale) | low | clear +0x4C before the loop |
| AI12 | **Sight:**<br>- **Cone:** set at registration (`82184D58`), with the FOV used as a **half-angle**. A half-angle of 90 deg or more, or 0, means all-round vision (`82184EC4`).<br>- **Range:** taken from the character data (`[char+0x54]+0x40/+0x44`). Alerted NPCs see all round and to at least 130 m, but still need the cone beyond 65 m (flag 0x800, on by default).<br>- **Crouch:** an unseen, crouching player scales range^2 by 0.5 (`sub_82599BA8`), so detection distance is x0.71, not x0.5.<br>- **Light:** there is **no light / darkness term**.<br>- **Raycasts:** line-of-sight raycasts are budgeted at 5 per frame (`82E2E4AA`, `821856E0`).<br>- **Throttle:** the NPC's sight evaluation (`sub_82570C68`) runs at most once per AI-clock interval (1.0 s default), so reaction is up to ~1 s; all its random rolls sit behind that throttle | half-angle cone, crouch halves range | `82184D58`, `82185358`, `82185640`, `82599BA8`, `82570C68` | read, 2nd read (cone, crouch, budget) | deliberate; half vs full angle in the data unknown | low | none (if half range was meant for crouch, the factor would be 0.25) |
| AI13 | **Hearing and alert spread:**<br>- **Gunfire:** noise per shooter (`sub_8257FB90`) is heard while distance < noise x the listener's hearing factor. It decays at 1500 cm/s x dt and is cut 2 s after the last shot.<br>- **Area sounds:** sound events (impacts 500, body fall 1000, ...) hold 1.5 s, then shrink at 1500 cm/s. They are team-filtered and don't use the hearing factor.<br>- **"Spread":** an aware NPC copies its target into its squad record (`sub_82177980`, no delay or radius), and guards hear each other's gunfire.<br>- **\*_TEAM / \*_SOLO:** the HEAR_GUNFIRE_TEAM / _SOLO strings and the like are **speech** events, not alert spreading | - | `821861E8`, `82186350`-`821863B4`, `82185DB0`, `82571E50`, table `82D2A3C0` | read | deliberate | - | none |
| AI14 | **Semi-auto enemy guns fire faster at 60 fps.**<br>- **Flutter:** while a burst runs, the shoot action (`821A29B8` -> `821A2AA0`) calls `sub_821A5CB8` every frame. For guns with spec +0xE1 == 0 (semi-auto) or the Falcon, it rolls `rand(0, rand(1,2))`: zero (5/12) presses the trigger, anything else releases it (`821A5D70`-`821A5DF0`).<br>- **Shots:** a semi-auto shot needs a new press (gun latch +0x2D, `822329E8` / `82232AD0`). New presses come at ~0.243 per **frame**: ~7.3/s at 30 fps, ~14.6/s at 60.<br>- **Result:** shots per burst don't change, but the wait per shot after refire falls from ~137 ms to ~69 ms (unless the gun's own refire time is longer) | a human-like tap rhythm, set for 30 fps | `821A5CB8`, `821A2AA0`, `822329E8`, `82232AD0` | read, 2nd read | **60 fps side effect** (per-frame roll) | medium: semi-auto enemies (data-dependent: pistols, the Falcon) finish bursts faster, about +10-25% damage per second | **fixed: `pdz_fix_ai_semiauto_rhythm`** (on): the inner roll is held per action for 1/30 s of AI clock; the AI pad (vtable `8209C088`) only stores triggers in its setters, so a held value stays |
| AI15 | **Enemy aim:**<br>- **Aim error:** a per-shot random 3-D offset **in cm** at the aim point (`sub_8252A1F8`). Its size blends a difficulty profile with the character's accuracy byte (params +0xB9). It grows by 80 cm when the target holds a +0x165C object, and while the "aim spoiled" timer char +0x36E4 runs (dt-decayed; set when the target dodges, `821BA130`, or the shooter flinches, at least 5 s, `8252D5F8`). Because the error is in cm, not degrees, it shrinks in angle with distance. Player crouch / cover is not an input.<br>- **Head shots:** close and well-aimed shots go to the head point 20% of the time.<br>- **Hit accumulator:** hits are also counted by `sub_821483B8` (range factor / char +0x1820, a hit at >= 1). At close range under 10 m it doubles on every difficulty; Secret Agent adds a 1-in-5 forced hit and PA / DA always force a hit | data-driven accuracy | `8252A23C`-`8252A5F4`, `82529CA0`, `82148528`-`821485EC` | read, 2nd read | deliberate | - | none |
| AI16 | Crowd rule `sub_821487B0` (feeds AI15): "if frame counter - [`82E2E47C`] > 15, count hostile characters and force hits x2 above a per-level count". `82E2E47C` is **never written** (one lis-resolved reference, the read; no base +4 store), so after the first 16 frames of a session the gate is always open and the rule runs on every call | throttle the recount to once per 15 frames | `821487F4`-`82148804`; xref `82E2E470`-`82E2E480` | read, 2nd read | likely bug (dead timestamp), harmless | none (same at 30 / 60; see FR3) | none needed |
| AI17 | **Fire rhythm:**<br>- **Gap between bursts:** 2.2 / 2.9 / 4.0 s by archetype (params +0x71), +0.5 s if moving, doubled for a grenade hand while the other hand is armed (`821D9F00`, gun idle timer += dt).<br>- **Shoot action:** waits 0.4 s, gives up after 1 s if it can't shoot, and finishes 0.5 s after the last shot.<br>- **Burst size:** depends on clip size and style (`82189538`).<br>- **Latent:** burst style >= 4 would read uninitialised stack and hit a divide trap (`8218969C`), and gap style >= 4 gives 0 s. Shipped data presumably never uses it | - | `821D9AB0`, `821D9F00`, `821A29B8`, `82189538` | read, 2nd read | deliberate (latent crash only with out-of-range data) | - | optional: clamp style to 3 |
| AI18 | **Grenade target scale: the 1.3 branch can never run.** `821ABA00` scales the throw target by 0.5-0.85. If the height gap \|dy\| > 100 cm it uses 1.0; the "else if \|dy\| > 300 -> 1.3" test sits on the \|dy\| <= 100 path | test 300 first (1.3), then 100 (1.0) | `821ABB3C`-`821ABB68` (`fcmpu` 100, `ble`; then `fcmpu` 300, `ble`; 1.3 at `82D34F58`) | read, 2nd read | **genuine bug** (never-true) | low: it only feeds the "can throw" checks (10 m minimum, arc trace), not the release angle. Throws at targets more than 3 m above or below need >= 10 m instead of ~7.7 m | **fixed: `pdz_fix_ai_grenade_height`** (on) |
| AI19 | **Grenade gating:**<br>- **Who throws:** the attack builder (`821DA088`) throws when the hand holds a thrown weapon (types 30-34).<br>- **Start conditions:** the target must be the current one, visible, within 15 deg of facing, with ammo, more than 10 m away after the AI18 scaling (~12-20 m in practice), and pass an arc trace.<br>- **Cooldown:** the only one is the per-enemy burst gap (AI17); there is **no team or global grenade cooldown** | - | `821AB1D0`, `821ABBD8`, `82188B48`, `82189960` | read | deliberate / unknown | possibly medium (several grenadiers can throw at once) | none |
| AI20 | **Dodge animation: FacingRight never plays.** The dodge task Update (`sub_821CB0C0`, task 9) picks with `rand() % 3` (`821CB628 li r11,3`, `821CB630 mulli r11,r11,3`) and then switches on 4 cases. Case 3, FacingRight_Dodge_Forwards / Backwards (151 / 152, `821CB698`), is unreachable | `% 4` | `821CB624`-`821CB6E0` | read, 2nd read | **genuine bug** (unreachable case) | low, cosmetic: the dodge still happens; one of the four animation pairs never plays | **fixed: `pdz_fix_ai_dodge_facing`** (on) |
| AI21 | **Dodge details:**<br>- **Facing:** chosen at random; only the side comes from geometry (`sub_8251E9F8`).<br>- **Clearance:** the 250 cm ray (`sub_821CAC10`) is cast forwards for both FacingLeft Forwards and **Backwards** (both r5 = 0), so a backwards dodge isn't checked behind.<br>- **Unused animations:** FacingFront / FacingBack_Dodge_Forwards and both Moving_Dodge animations are never chosen | - | `821CAC44`-`821CAC90`, `821CB6E4`-`821CB71C` | read | likely bug (backwards clearance), minor; the unused animations are deliberate or unknown | low (the dodge can run into a wall; collision stops it) | **fixed: `pdz_fix_ai_dodge_clearance`** (on) |
| AI22 | **Dodge timing:**<br>- **Trigger:** the target must be within 30 m and the condition must have held for 4 / 3 / 2 / 1 s by difficulty (`sub_821CB8E0`).<br>- **Cooldown and choice:** 3.5 s between dodges. Then 2/3 relocate (goal types 8 / 9), 1/3 an animated dodge.<br>- **Clock:** all on the AI clock. After a failed attempt the roll repeats every frame until one succeeds (about a frame of latency) | - | `821CB158`-`821CB2E0`, `821CB8E0` | read | deliberate | none | none |
| AI23 | **Path corner-speed curve off by one.**<br>- **Where:** three copies (`sub_8218A6E0` / `8218CDD8` / `8218F4D0`) loop `i < n-2` over the curve points, so the last segment is never tested and returns 0.<br>- **Curve (type A):** 0, 30, 45, 50, 55, 65, 70, 85, 90 deg -> 1 ... 0.05.<br>- **Result:** corners of 85-90 deg get speed 0 instead of 5-7% | `i < n-1` | `8218A70C`-`8218A74C` (and copies), data `sub_82169250` | read, 2nd read | **genuine bug** (off by one) | negligible: AI pauses briefly at near right-angle corners instead of crawling | **fixed: `pdz_fix_ai_corner_speed`** (on) |
| AI24 | **Route following:**<br>- **Repath:** every 2 s of AI clock the follower probes 70 cm ahead and replans if blocked (limit 70 m). A failed plan logs PathfindError and stops the task (state 3); there is no retry loop.<br>- **Stuck check:** a start position and time are stored (+0xA0 / +0xB0, `sub_8218C6A0`), but **nothing reads the time**, so there is no time-based give-up: a pinned character can push indefinitely unless the 70 cm probe sees the block | perhaps an unfinished stuck timer | `8218B904`-`8218BABC`, `8218C934`-`8218C960`, `821A0D64` | read | deliberate / unknown | low-medium (rare stuck AI) | none (diagnostic first) |
| AI25 | Goto-prop action `sub_821A0DC0` rolls `rand() % 10` **every frame**, has no timeout and can read through a null pointer, but its constructor (`sub_821A0B78`) is never called | - | `821A0E44`, `821A0ED4`; no callers | read | dead code | none | none |
| AI26 | **Squads:**<br>- **Coordination:** there is no flanking or squad-order system in code. OUTFLANK_TEAM, PROVIDING_COVER_TEAM and HOLD_DOOR_* are speech events. Coordination is squad target sharing (AI13) plus each enemy's own planner.<br>- **Squad record:** found at `(squad - 1) * 560` (`sub_82178548`). Squad 0 (the player's) would point before the array, but it was only seen called for NPCs | - | `82177980`, `82178548` | read | deliberate; squad 0 unknown | - | none |
| AI27 | **Jonathan stuck seated (Outpost Rescue, LV12): probable cause.**<br>- **Seated:** the level script creates Jonathan (and two other hostages) in state 1, sitting (`sub_823686C0`). Nothing but level message (8, 8) moves them on (handler `8236F678`, one-shot).<br>- **Release:** (8, 8) is sent from one place (`8236CA08`-`8236CA68`): when flag +1028+12 bit 0x20 is set and player 1's current pad is in zone 3.<br>- **Flag:** bit 0x20 has one setter, the handler for message (8, 12) (`8236F5C4`-`8236F670`). That handler counts the arrivals of message 12 and the living members of spawn group s04a-e standing in zone 3, and sets the flag once 5 messages have arrived or none of s04 is left there.<br>- **Gap:** no code sends (8, 12), so it comes from level data, and there is **no timer or other fallback**. If the squad skip bypasses whatever sends it, Jonathan never stands up.<br>- **TU3:** Rare's Title Update 3 did not touch this chain. Player 2's position is never checked | release once the hostage room is clear | `823686C0`, `8236CA08`-`8236CA68`, `8236F5C4`-`8236F674`, `8236F678` | read, 2nd read | **probable cause (design gap)**: the code is consistent; what's missing is a fallback when the data message doesn't come | high for speedrunners (softlock), none in normal play | Option, off by default: in the LV12 update (`sub_8236C3E0`), evaluate message 12's own test each frame (not released yet, and no living s04 member in zone 3 -> set bit 0x20). First a logged normal run and a skip run (type-8 messages, the +1028 words, s04 state) to confirm |
| AI28 | **Datacore scientists (Level 0 script):**<br>- **Survivors:** required 2 / 3 / 4 by difficulty, so 2 / 1 / 0 losses are allowed (`822F1F70`-`822F1FC4`).<br>- **Failure:** per-scientist events 80-83 count a loss only while the objective is active, and it fails (`sub_8214CC08`) once losses exceed the allowance (`822F9C74`). There is no timer, distance or alert failure.<br>- **Immortality:** scientists can lose their immortal flag only on PA / DA, and only scientists 0-2 (`822F7B08`). One loop moves scientists 2..0 using 3 of a 4-entry pad table (pad 55 never used); scientist 3 has his own trigger (19).<br>- **Tank scientist:** has Rare's own anti-stuck watchdog (`sub_822F3218`) | - | as quoted | read, 2nd read | deliberate; the unused pad is unknown | community "objective fails" reports fit the PA / DA zero allowance | none until the senders of events 80-83 are known |
| AI29 | **Companion follow:** Jonathan follows from ~3.9 m (`sub_821CE868`) and re-plans every 0.35 s when the leader changes zone or moves 8 m. A long route uses the long goto. There is **no teleport or catch-up** | - | `821CEA6C`-`821CEEF8` | read | deliberate | a lost companion waits and re-plans | none |
| AI30 | A Level 0 scientist talk animation is picked with `rand(0,0)` (`822E0070`), so table entries 1-3 (`talk_say_no` 1-3) never play | `rand(0,3)` or similar | `822E0048`, table `82D6BB68`, names via `82D287F8` | read, 2nd read | deliberate (rechecked when building the AI fixes) | none | left as shipped: the table is tankidle, three pistol talk_say_no animations, bang glass, typing...; this routine idles the scientist sealed in the tank, so `rand(0,0)` (always tankidle) looks like a deliberate narrowing |
| AI31 | **What difficulty changes for enemies:**<br>- **Damage:** the player takes x0.1 / x0.3 / x1 / x1 and enemies take x2.5 / x1.34 / x1 / x1 (`sub_8222F458`).<br>- **Hits:** AI15's forced close hits.<br>- **Timers:** dodge delay 4 / 3 / 2 / 1 s and melee chance 5 / 20 / 30 / 50% (`8208E4D4`).<br>- **Auto-aim:** the record is shared by PA and DA (`sub_824E1DA0`).<br>- **Setups:** random on PA / DA, rotating on Agent / SA.<br>- **Scripts:** level-script gates (527 reads, consistent min / max patterns).<br>- **Tables:** every difficulty table read has 4 entries and correct indexing, with no swapped compares. Two level-script tests `difficulty < 0` are never true (`822ACDBC`, `823701FC`), i.e. degenerate "Agent and up" gates | harder = harder | as quoted | read | deliberate | - | none |
| AI32 | **Boss tuning out of order on Secret Agent:**<br>- **Mai Hem:** health 1000 / **1500** / 1200 / 1200; accuracy divisors SA 1.0 / 1.2 / 1.2 vs PA 1.0 / 2.2 / 3.5 (`821B83CC`, `821B8A70`).<br>- **Brothers:** the tables are not monotonic (`821B6B00`: 0.5 / 0.1 / 0.3 / 0.1).<br>- **Net effect:** after the SA damage scaling (AI31) both bosses stay easier on SA overall | per-difficulty tuning | as quoted | read, 2nd read (Mai Hem) | probably deliberate (compensates SA's damage scaling) | SA bosses hit harder per shot than you'd expect | none |
| AI33 | On Dark Agent about 100 in 101 characters get ability bit 39 locked at spawn (`82570548`, `rand(0,100) > 0`), which sends their attack through the standalone shoot task. What ability 39 means is not traced | - | `82570548`, `821DA1CC` | read | deliberate | DA-only behaviour change | none |
| AI34 | **Hurt / shot-at bits do get set and never clear.**<br>- **0x80, damage:** set at `8251EBE8` (`sub_8251EBB0`, NPC vtable +0x32C), called unconditionally from TakeDamage `sub_82537548` (`825376C8`), and from the damage message handler (`824A9134`).<br>- **0x100, near miss:** set at `8251E244` (`sub_8251E170`) for every character within 150 (cm) of each bullet segment (`sub_82175658` <- `822391AC`), skipping the shooter and its allies.<br>- **Clears:** none, apart from the constructors (`8251D16C`, `8251D3B4`).<br>- **Planner:** tests the right word and bits (`821B2674`-`268C`: 0x1000 no-cover at +0x187C, then 0x80 / 0x100 at +0x186C). After the first hit or near miss, every later default-plan build goes through the cover chooser | hurt / shot-at latches | as quoted; every `6252(r` store in the recompiled code | read, 2nd read | works as coded | - | none |
| AI35 | **Cover pad pick and re-check disagree on the minimum distance.**<br>- **Pick:** the search (`sub_82155EB8` -> `sub_82156C50`) accepts pads from max(weapon range #1^2, 700) (`82157AE0`-`82157B40`, read at `82155EFC`).<br>- **Re-check:** the goal's per-tick test (`sub_821B3030`) needs 10 m (1e6, `821B30B0`).<br>- **Loop:** a pad inside 10 m of the player is chosen, fails the next tick, ends the plan with result 1 (no fallback), and the plan is rebuilt on the same pad | one distance band for both | `82157B2C`, `82155EFC`, `821B30A8`-`30B8`, `821B4B38`-`4B60` | read, 2nd read, logged (185 of 2,545 picked pads under 10 m in a close fight, each dropped by the re-check) | **genuine bug** | high in close fights: wounded enemies turn for cover and come back | **fixed: `pdz_fix_ai_cover_min_distance`** (on): while the cover goal A / B pickers (`sub_821B2E88` / `sub_821B2DC8`) run, passing pads at or under 10 m from the target are rejected, so the pick uses the re-check's band; the re-check and other users of the filter are unchanged |
| AI36 | **Correction to AI2.**<br>- **+0x30F8:** char +0x30F8 is the time the target was **last seen**, not last hit (writers: the sight evaluation `8257146C` and the constructors).<br>- **6 s rule:** a non-cautious plan is dropped once its goal's **action** has run > 6 s **and** the target has been unseen > 6 s, or neither gun has fired for 6 s (`821B0F30`-`0FB0`). Being hurt plays no part | - | as quoted | read, 2nd read | doc correction | - | - |
| AI37 | A running plan rarely keeps the cover chooser out: the default attack plan ends one tick after the enemy reaches its pad (`821AFE08`), and the planner runs again next frame | - | `821AFBD8`, `821B0FF4`-`1040` | read | (hypothesis rejected) | low | none |
| AI38 | **What a cover plan needs.**<br>- **Goals:** A / B (cover, types 2 / 3) need a pad from the 35 m search with cover flags 0x4 / 0x8, on the enemy's side, the player inside its 30 / 90 deg arc, inside the AI35 band (up to 65 m), pad-to-player-pad visibility, and a non-null player pad.<br>- **Gates:** no cover within 5.5 m (close plan); fewer than 4 free plan nodes gives no plan.<br>- **Order:** both cover actions (move to cover / cover in place) walk to the pad first.<br>- **Fall-through:** when A and B find nothing, goal D (attack position) runs, which looks like ordinary combat | cover when hurt | `821B20D8`, `82157970`, `82156C50`, `821B3250` | read | deliberate, strict: the main reason wounded enemies rarely *visibly* take cover (with AI35 and AI39) | high (feel) | design option only (loosen the band / arc for hurt enemies) |
| AI39 | Every plan goal (attack, cover, attack position, re-check) needs the **player's** current pad (+0xB4) to be non-null; all use pad-to-pad visibility to it. AI3 therefore applies to all plans, not just cover | - | `82156158`, `82156C78`, `821B32FC`, `821B309C` | read | unknown how often | possibly high in open or sparse-pad areas (a bridge deck) | log it (`pdz_ai_state_log`: pads player null) |
| AI40 | Enemy's own pad null: the combat update drops its plan and returns (`821B0EAC`-`0EB8`) | - | as quoted | read | deliberate (no nav position) | "frozen" until it's back on the pad graph | log (pads own null) |
| AI41 | SNIPER (behaviour 7) has no plan function: it shoots from where it stands | - | record `82D2C3FC` | read | deliberate | 10 setups on the bridge level | none |
| AI42 | **HOLD_POSITION.**<br>- **Not provoked:** while unprovoked (+0x187C 0x800 clear) it holds. Its advance task needs: aware, target unseen > 4 s, 8 s since the last, the target's pad in another zone within its hop limit (`sub_821B18D8`).<br>- **Fallback:** does nothing unless provoked | hold until provoked | `sub_821B2828`, `sub_821B18D8`, `821B36C8` | read | deliberate | 47 setups on the bridge level | none |
| AI43 | "No cover" flag +0x187C 0x1000 is set by level scripts (e.g. the bridge level, 3 times) and script task 49, and never cleared | - | `821B2674`, `821B1AD0` | read | deliberate / unknown | per enemy | none |
| AI44 | The bridge script checks s44k (+0x978) and s44l (+0x980) but writes the no-cover flag to s35a (+0x830) both times (`82291CE4`-`82291D30`) | no-cover on s44k / s44l | as quoted | read, 2nd read | likely bug (copy-paste) | low; arguably better play (they may use cover) | none |
| AI45 | **Plan failure handling.**<br>- **No pad for any goal (result 2):** runs the behaviour's fallback, often the do-nothing task 39, ended after up to 5 s (`821B36F0`).<br>- **Results 0 / 1:** just leave the slot empty, and the plan is rebuilt next frame with no backoff (`821B0FF4`-`1040`).<br>- **Path failures (AI24):** end with result 1 and rebuild toward the same pad | - | `sub_821B10C0`, `sub_821B1128`, `821B35A0` | read | deliberate / unknown | the "standing around" signature: idle up to 5 s, or rebuilding every frame | log first (`pdz_ai_state_log`) |
| AI46 | **Cover plan churn: pick vs re-check, condition by condition.**<br>- **Same test, same inputs:** goals A / B (types 2 / 3) pick with `821B2E88` / `821B2DC8` (35 m search `82157970`, filter `82155EB8`, then scorers `8215A0F8` / `8215A2D8`) and re-check every tick with `821B3030` (member-function table `82D2C650`-`6D8`). Both end in the same `sub_82156C50` on the same cover pad, the player's current pad (+0xB4) and the player's position (`sub_8214C7E8`). Its tests: cover flags 0x4 / 0x8, arc (30 / 90 deg), distance band, pad-to-pad visibility (`82195208` / `821952D8`), and peek pads plus side angle for side cover.<br>- **Only fixed difference:** the minimum distance, which is AI35.<br>- **Logged on the bridge (2 runs):**<br>&nbsp;&nbsp;- **Run with the player close:** 185 of 2,545 picked pads were under 10 m, and failed plans tracked re-check failures (correlation 0.96).<br>&nbsp;&nbsp;- **Run at range:** 41 re-check failures (visibility 34, arc 4, under 10 m 3), 40 of them after the player had moved to another pad since the pick.<br>&nbsp;&nbsp;- **No pad at all:** 1,040 of 1,160 cover plans were built when no pad passed (enemies 26-66 m away), and ended "no pad for any goal" | the pick and the re-check use one band | as quoted; `pdz_ai_state_log` | read, 2nd read, logged | **AI35: genuine bug** (same test, looser minimum at pick time). The visibility / arc drops after the player moves: **design** (the pad is only kept while it still covers the player's current position; there is no hysteresis, so moving targets cause churn). No pad found: **design** (strict pad rules) | high in close fights (AI35); medium otherwise | AI35: **fixed: `pdz_fix_ai_cover_min_distance`** (on). The rest is **design, left as shipped** (decision): honest "no pad" results plus re-checks failing after the player moves. A short grace before a re-check failure ends a cover goal would be a design change, not a fix |
| AI47 | **Route failures.**<br>- **Bridge run:** 87 of 320 route plans failed, **all by allied squad members**, all at "no pad route" (`8218C6A0` built nothing within the zone). Enemies had none in that run.<br>- **Failure points in `sub_8218A990`** (same in `8218D088` / `8218F780`): no own pad; other area (+60) with no cross-area routing (+156 == 0); no zone path (+152 = 2); no pad route (+84 null); rejected (`8218AD80`) | - | `8218A9E0`-`8218AB24` | read, logged | unknown (ally follow targets the player's position; probably pads it can't reach within the zone). Not proven to be a bug | low for enemies | none (log first if the squad visibly gets stuck) |
| AI48 | **Friendly fire (corrected 2026-10-20 after the user killed Jonathan and the squad turned on them).**<br>- **Zero-damage path:** the hit reaction `sub_82539980` (r26 = victim, r20 = attacker) zeroes the hit's values (`8253A4B8`) only when the victim counts the attacker as a friend (`sub_825104D8` at `8253A3F4`; team masks AND-ed in mode 3) **and** the attacker's type has flag bit 0x4 (`8253A400`-`A414`), most likely NPC attackers. So it doesn't stop the player's shots.<br>- **Betrayal switch:** if the **attacker** character (r31) has +0x1884 flag 0x400 and not yet 0x800, the hit sets 0x800 and **clears the attacker's team mask** (+128 = 0, `8253A428`-`A44C`). A shooter with that flag stops being anyone's ally after its first hit.<br>- **Also:** the level script gets every hit (`sub_82509228`), and the bridge level has a "Jo shoots multiple friends" line.<br>- **Not traced:** who sets 0x400 on the player (written through a +0x187C base +8 pattern), and whether it's the switch the bridge uses or a script count | allies turn on a player who shoots them | as quoted | read, 2nd read (branch conditions) | the original game's rule (engine switch and / or level script), not a bug. **The earlier "the engine zeroes friendly fire" was wrong** | - | none |
| AI49 | **Nightclub guards ask for a Mansion line their voices don't have.**<br>- **The remap:** the Nightclub script (`sub_822AB130`) gives 12 guards (s02c-e, s09a-e, s10a-d) two one-shot speech remaps at char +12852: SEE_PLAYER_TEAM (2) -> LVL4_SEES_PLAYER_WITH_GUN (160) and SURPRISED_SOLO (141) -> 160 (`822AB588`-`822AB598` and copies). The voice code applies them in `sub_82152450` after the solo-to-team step (`sub_82152FB0`).<br>- **The gap:** Nightclub loads only gangster voices, and no gangster bank records `lvl4_sees_player_with_gun` (troopers do, for Mansion). So each guard's first squad sighting and first surprise ask for `GANGSTAn_LVL4_SEES_PLAYER_WITH_GUN`, find nothing, and stay silent; the normal `see_player_team` / `surprised_solo` takes, which exist, never play for them | the club guards' own sighting line | `822AB56C`-`822AB598`, `82152450`, gangsta1-3 cue lists | read, 2nd read | **likely bug** (a Mansion remap reused in Nightclub) | low: 12 guards lose one or two barks each | remap 160 to the speaker's own event (`SEE_PLAYER_TEAM` / `SURPRISED_SOLO`) when the voice lacks it, or drop the pair for gangster voices. *Built 2026-10-22 (untested): `pdz_fix_ai_speech_remap_fallback` (on) keeps the original event when the remapped line isn't recorded for the voice.* |
| AI50 | **A second queued speech event is always dropped; its priority check is computed and thrown away.** The voice keeps one pending event (+216, `sub_8262B488`). When one is already pending, it maps both the new and the pending event to their speech groups (`sub_82153038` twice) and then returns without comparing them, so the new event is lost whatever its group | keep the more important of the two | `8262B4B8`-`8262B4C4` | read, 2nd read | likely bug (a dead comparison), or deliberate "first one wins" | low: an urgent line can be lost behind a minor one | compare the two groups and keep the more important one, if wanted (which way Rare ranked groups is not known) |
| AI51 | **Speech groups below 40 play once per character life.** The may-speak check (`sub_8262B378`) blocks a group whose bit is set in +196. The bit is set when a line in that group plays (`82152F50`, `8262B9C8`) and cleared only by the constructors (`8262A1A4`, `8262A2C4`). So each guard says each first-reaction line (seeing a body, the alarm, a camspy, gunfire, damage...) at most once. Groups 40-64 instead use a three-step rolling mask (+208 / +210 / +212, shifted in `sub_8262A4E0`), and every line needs a global gap since the voice last spoke (+144 vs +100) | as coded | `8262B378`-`8262B470` | read | deliberate (anti-repetition) | explains guards that react silently the second time | none |
| AI52 | **Speech events with no speech group.** The level sound manager's map (`sub_82153038`, byte table `8208D7C0`) has no group for LVL4_PLAYER_SPOTTED_RADIO_IN (158, sent at `821D0984`), PLAYER_IGNORES_WARNING (163, sent at `822B0CC0`), LIGHT_SHOT_OUT (147), DOOR_KICKED_IN (106), LV4_LET_JO_IN (169), the two upstairs events (165 / 166) and the pain sounds. Those fall to the "Category %d does not have a group! (Is it a new level-specific category?)" print (compiled out) and group 57, so the two sent ones are throttled as if they were hurt lines. Group numbers 17-19 and 30 have no events at all | a group per sent event | `8215325C`-`82153270`, string `82090528` | read | likely oversight (level-specific events added after the map) | low | none needed |
| AI53 | **LV4_LET_JO_IN (169) is special-cased but never raised (TU3).** The name builder (`sub_82204718`, `8220472C`) turns event 169 into `LV04_SPEECH_GUARD_N`, but no TU3 code raises 169 (a scan of every say entry point and every 169 immediate). So Mansion's `guard_n`, counted as used because the exe names it, never plays in TU3. *Corrected 2026-10-22 (follow-up, subtitles re-aligned and recognizer-checked):* `guard_n` is **not** the ID request. Its takes are "Enjoy the party." / "Miss." / "The entertainment's about to begin.": the door guard waving Jo through, which matches the event's name. The ID request ("Identification?" / "Your voice signature please.") is `guard_o`; it plays from the Perfect / Dark Agent voice-check list, as retail footage shows. | the main-door guards letting Jo in (Agent / Secret Agent at once; Perfect / Dark after the voice check) | `8220472C`-`82204764` | read | the guards wave Jo through in silence | none | a genuine fix if launch raises 169, otherwise a restoration. *Built 2026-10-22 (untested): restoration `pdz_restore_mansion_door_greeting` (off).* |
| AI54 | **Default task: only PATROL is real.** DEFAULT's setup (`sub_821D0358`) reads the setup's default task (template +175): 2 (PATROL) builds the patrol task (`sub_821DE520`); any other value with a guard target (+168) builds the guard-at-pad task (`sub_821D97E0`). TASK_GUARD (3), TASK_DEFAULT_TASK_SCRIPT (1) and TASK_PLAY_ANIM (4) are not told apart, and PLAY_ANIM has no code. Census of 1,013 single-player setups: UNSET 861, PATROL 136, GUARD 10, DEFAULT_TASK_SCRIPT 5, PLAY_ANIM 1 (Laboratory `s04labone3`, which just guards) | play an animation for PLAY_ANIM | `821D0400`-`821D052C` | read, census | deliberate / unknown (PLAY_ANIM unfinished) | negligible | none |
| AI55 | **Subway's pre-fight taunt rolls every frame.** The taunt task's update (`sub_8235FE60`, vtable slot 7) rolls `rand(0,100) < 5` each update while idle (`823601E8`-`8236020C`), so taunts start about twice as often at 60 fps as at 30. It only plays for voice 17 (TROOPER1) / 7 (GANGSTA1) | a per-second chance | `823601E8`-`82360268` | read | frame-rate dependent (like AI14 / FR9) | low: more chatter at 60 fps | scale the chance by dt / (1/30), if the 60 fps unlock is on. *Built 2026-10-22 (untested): `pdz_fix_ai_taunt_rate` (on, with the 60 fps unlock).* |

**Logged on the bridge level (pdz_ai_state_log, 3 runs, Agent, 2026-10-19/20):**

- **Ruled out:**
  - the player's pad is valid 100% of the time (AI3 / AI39 aren't the cause here);
  - "held" (vtable +0xE8) is only true on a character's last lines, i.e. dying;
  - the "do nothing" CAUTIOUS characters are the player's allied squad: they never target the player.
- **Wounded enemies going for cover and turning back (the user's report) is the AI35 / re-check churn:**
  - enemies built 1,300 cover plans in 6 minutes and only one reached its pad;
  - per second, failed plan ends track re-check failures (correlation 0.96, 250 of 326);
  - 185 of the 2,545 pads the pick accepted were under 10 m from the target, which the re-check rejects;
  - the rest end with "no pad for any goal" (958).
- **Routes:** 34% of route plans fail (323 of 939), a secondary cause (correlation 0.27).
- **Dodges:** every enemy has the dodge ability (bit 41), and the dodge task passed its ability / idle checks 66,779 times. Even so, the timed roll never ran: only 2 animated dodges in 6 minutes. The "sees its target within 30 m" condition (`sub_82508D88(target, 5)`) must hold unbroken for 4 s on Agent, and it almost never does. Next: measure how long that condition stays true.

Not traced:
- door handling (KICK_DOOR, locked doors);
- leader / follower formation;
- the live move-to tasks' updates;
- the generic grenade-avoidance reaction (SEE_GRENADE is only set by a level script);
- how grenades get into an enemy's hand;
- the profile and character data values (FOV, ranges, hearing factor, accuracy);
- who sends LV12 message 12 and Datacore events 80-83;
- ability 39;
- whether a checkpoint reload after Jonathan's release can re-seat him.

Diagnostics that would settle them:
- log LV12 type-8 messages;
- log the semi-auto trigger presses per second at 30 vs 60;
- log footstep hearing hits with and without the AI9 clear.

### Enemy AI: final state (2026-10-20)

**Fixes on by default** (all in the game fixes group, `pdz_game_fixes`, **F11**):

| Fix | Setting | What changes |
|---|---|---|
| AI8 enemy cover task (grace timers, peek roll, snap-back) | `pdz_ai_cover_fixes` + `pdz_ai_cover_fix_*` (own group, **F5**) | grace timers start on arrival; real 1-in-3 peek variant; give up after 3 snap-backs |
| AI10 alert calm flag | `pdz_fix_ai_calm_flag` | calm flag follows the clamped alert count |
| AI14 semi-auto rhythm at 60 fps | `pdz_fix_ai_semiauto_rhythm` | trigger tap rolled every 1/30 s of AI clock, as at 30 fps |
| AI18 grenade height test | `pdz_fix_ai_grenade_height` | targets more than 3 m above / below use the 1.3 scale |
| AI20 dodge facing | `pdz_fix_ai_dodge_facing` | all four facings, FacingRight plays |
| AI21 backwards dodge clearance | `pdz_fix_ai_dodge_clearance` | room checked behind for backwards dodges |
| AI23 corner speed curve | `pdz_fix_ai_corner_speed` | 85-90 deg path corners slow instead of stopping |
| AI35 cover pick minimum distance | `pdz_fix_ai_cover_min_distance` | cover pads picked at least 10 m from the player, matching the re-check |

**Left as shipped, and why:**
- **AI9 footsteps alone don't alert guards:** decision (stealth design).
- **AI30 the tank scientist's `rand(0,0)` animation:** keeps him on his tank idle; the other entries are pistol talking animations.
- **AI1 / AI38 strict cover rules:** design. A pad must be 10-65 m from the player, face them within 30 / 90 deg and see their pad; wounded enemies often find none ("no pad" endings, logged).
- **AI46 cover dropped after the player moves:** design (no hysteresis).
- **AI2 / AI36 the 6 s plan rule, AI41 snipers that never move, AI42 HOLD_POSITION waiting until provoked, AI43 / AI44 level-script no-cover flags, AI45 fallbacks:** design.
- **AI22 dodges:** rare by design on Agent. Every enemy has the ability, but "sees its target within 30 m" must hold unbroken for 4 s (3 / 2 / 1 s on harder levels), then a 3.5 s cooldown, then 2 in 3 rolls relocate. Logged: 2 animated dodges in 6 minutes.
- **AI48 friendly fire:** original rule. Shooting allies can turn them on the player (a one-hit team-clear switch on flagged shooters, and the level script).
- **AI32 boss tables and AI33 Dark Agent ability lock:** probably deliberate.

**Remaining unknowns:**
- why the dodge condition so rarely holds for 4 s (rarity vs perception flicker; not measured);
- the allied squad's "no pad route" failures (AI47);
- the Jonathan squad-skip softlock (AI27, needs a logged skip run);
- Datacore events 80-83;
- ability 39;
- door handling and squad formations;
- grenade avoidance and how grenades get into an enemy's hand;
- the actual sight, hearing and accuracy values in the level data.

## Level scripting and spawns

| # | What the code does | Seems meant to | Evidence | Confirmed | Verdict | Impact | Smallest fix |
|---|---|---|---|---|---|---|---|
| LS1 | Spawning: up to 20 spawn templates per level (`sub_82272340`), used by level scripts (27 call sites); route traffic from bounded pools; the alarm only plays a siren; the one alarm-reinforcement script (ware) spawns 3 guards, 1 per second, then stops | as coded | `sub_82142828`, `sub_8214A120`, `sub_8235A908` | read | deliberate (scripts don't cause enemy streaming) | - | none |
| LS2 | No script, spawn, objective or trigger timer uses the frame counter; scripts use the AI clock (dt) | - | xrefs | read | fine (same at 30 / 60) | - | none |
| LS3 | `sub_82376FD0` uses a spawn result without a null check (`lwz r9,0x54(r11)` right after `bl sub_82142828` at `82377050`); other callers check | check for 0 like the others | `82377050`-`82377080`, `823770BC`-`70F0` | read, 2nd read | likely bug (latent: crash if a spawn fails) | low | **not fixed**: the null result is dereferenced inline and linked into a reference node that `sub_823772E0` then uses, so a function-level hook can't skip it safely; never seen in play |
| LS4 | Co-op host path logs with "Client" labels | | `sub_82142250` | read | cosmetic | none | none |
| LS5 | Debug tasks compiled to empty stubs; a QA "SYSTEM INFO" popup on a large button chord (CheatTick) | | | read | fine | none | none |
| LS6 | **Laboratory blackout is never reversed.** The lights message (`8232A264`) switches light groups 1-4 off and the ambient colour to black; the darkness message gives the 29 marked guards 10 m sight (profile range 1e6) and x0.25 aim build-up (flag 0x2). Nothing turns the lights back on, puts the range back (only written at spawn, `sub_82189918`) or clears the flag, on any route, and `chan_07` "Generator is back online." is never played | the switch route as a timed blackout: `chan_06` "Manual shutdown...", tannoy "temporary power failure... Normal service will be resumed shortly", "We apologize for the delay in reconnecting...", `chan_07`; the lockbot hint says "permanently" | `823272E0` (lights `8232A264`, darkness `823282D4`), `sub_82601D88`, `sub_82189918` | read, every write to +6276 checked | cut feature (restore never built), not a code slip | medium on the switch route (permanent cover where a timed one was meant) | none (a restore would change the darkness mechanic) |
| LS7 | **`Lights_Toggle` (level command 111) only ever switches lights off.** The per-light handler computes the new state as "not the state array", testing the array's address (`addi r11,r3,1452` / `cntlzw` at `825F3BC8`) instead of an element, so the result is always "off" (`sub_82602648`, r4 = 0) | toggle: off when on, on when off | `sub_825F3B68`, `sub_825F3708`, `sub_8214E520` | read | bug (Rare: array decays to a pointer) | none seen: Laboratory only uses it to switch lights off; other levels' uses not checked | none needed now (a fix would read the element for the current buffer, `82E1BA20`) |
| - | Not traced: checkpoint restore, objectives and trigger volumes exhaustively | | | | | | |

## Frame rate (pdz_fps60_unlock vs the shipped 30 fps)

Everything in movement, weapons, AI, scripts, cameras and HUD is timed by dt
or the AI clock. Only these are counted in frames:

| # | What | 30 fps | 60 fps | Impact | Verdict |
|---|---|---|---|---|---|
| FR1 | Cover prompt scan (CV1) | 0.3-0.5 s | 0.15-0.25 s | medium (better at 60) | likely bug (see CV1) |
| FR2 | Job rotation every 10th frame (`sub_82500FA8`, under the profile tick; jobs not identified) | 333 ms | 167 ms | unknown | unknown |
| FR3 | Startup grace before an entity-count limiter (`sub_821487B0`) | 533 ms | 267 ms | low | fine |
| FR4 | Sound / message debounce (`sub_82398568`) | 100 ms | 50 ms | low | fine |
| FR5 | Cutscenes advance per vblank (60 Hz either way) | same | same | - | fine at 60 (a 120 Hz refresh would speed them up) |
| FR6 | Character +0x1308 counts down per update, but its setter (`sub_82536000`, 300 frames) is never called | - | - | none | dead feature |
| FR7 | Right-stick centre learning (`sub_8226BE48`): mean of up to 3600 near-centre samples | 120 s | 60 s | low (small held pushes move the centre twice as fast) | 60 fps side effect, minor |
| FR8 | Right-stick jitter estimate (smallest per-sample change, < 450 = quiet 15.3% deadzone) | - | settles sooner | none | fine |
| FR9 | Enemy semi-auto trigger flutter (AI14): press/release rolled every frame | ~7.3 presses/s | ~14.6/s | medium (semi-auto bursts finish faster) | 60 fps side effect; fix proposed in AI14 |
| FR10 | Enemy sight raycasts: 5 per frame (AI12) | 150/s | 300/s | low (a guard can register "visible" a few frames sooner; reactions are throttled to the AI clock) | fine |
| FR11 | Hearing: 2 listeners checked per frame; a new noise is held at full strength for 2 rounds before it decays | longer hold | half as long | none (every listener still gets one full-strength check) | fine |
| FR12 | Per-frame re-rolls after a failed dodge (AI22); a discarded rand() in task `821B5FD0` | - | twice as many draws | none | fine |
| FR13 | Bridge's ship turret (DP_Turret `sub_825D0B28`): yaw / pitch += gap x 0.1 / 0.2 per frame (`825D1298` / `825D12A0` / `825D1338`, centre `825D150C` / `825D1514`); its waits use dt | ~1.9 s turning per shot from 45 deg | ~1.0 s (0.5 s at 120) | medium (fires sooner, aims better) | bug. *Built 2026-11-08 (untested): `pdz_fix_turret_rate` (on), `rate_fixes.cpp`* |
| FR14 | Gun idle fidgets (`sub_824FA578`): `rand01() < 0.005` per frame while standing with no gun animation (`824FAA80`-`824FAA90`), slots 10-13 | one every ~6.7 s | ~3.3 s (1.7 s at 120) | medium (seen whenever you stand still) | bug. *Built 2026-11-08 (untested): `pdz_fix_idle_fidget_rate` (on)* |
| FR15 | Screen flicker of framebuffer-effect modes 10 / 13 (`sub_825F5290`): `rand01() < 0.01` per frame while no flicker runs (`825F5498`-`825F54B4`); its decay uses dt. Modes come from the camera's `PDZ_FramebufferFx` channel (`8220E740`); which scenes use 10 / 13 not yet found | ~0.3 flickers/s | ~0.6/s (1.2 at 120) | low (scenes unknown) | bug. *Built 2026-11-08 (untested): `pdz_fix_screen_flicker_rate` (on)* |

## Tuning data

| # | Record | Value | Looks meant to | Confirmed | Verdict | Impact | Fix |
|---|---|---|---|---|---|---|---|
| TD1 | `playerpddefault.xml` fast-look accel | 2 (min 3, max 4) | 3-4 | read | likely typo | Classic PD only (see Controls) | `pdz_fix_fastlook_accel_data` (parked) |
| TD2 | `test\motionparamtable` (loaded live via `test\global`) | block A has 30.13 and 9 where block B repeats 3.13 and 3 | 3.13 / 3 | read; field meanings unknown | unknown | unknown | log the fields' users first |
| TD3 | CMP noise (WP9) | 5000 | 500 | read (data and code) | likely data bug | medium-low | fixed: `pdz_fix_cmp_noise` |
| TD4 | Secondary gun types (WP10), MP5 tracer (WP11), M16 cues (WP12) | | | read | unknown / likely copy-paste | low | |
| TD5 | Halo pitch deadzone 0.5 / 0.455 / 0.5 (min / default / max) | dip at default | read | unknown | low | none |
| TD6 | Spot crosshair identical to the 4-way expanding one | | read | unknown | low | none |
| TD7 | AI templates: 8 roof guards with a 10 cm radius (others 15-50 m), one jung guard with 1 | | read | unknown | low | none |
| - | Secret Agent and MP auto-aim records identical; jetpack records are MP5 clones; tracer, bloom, cap, recovery and movement values consistent | | read | fine | | |

## Visual readability (LOD)

| # | Finding | Confirmed | Verdict | Suggestion |
|---|---|---|---|---|
| VR1 | No character LOD system exists: the only `_Lod` model in the packages is the combat shield, and no LOD strings or distance tables are in the code | read (all packages, all strings) | fine | - |
| VR2 | Distant enemies look like blobs because of per-level linear fog (e.g. cs1 / cs2 / holo fade 0.6-150 m), the original FOV (45 deg vertical, ~73 across at 16:9) and host resolution / filtering | read | fine | try `anisotropic_override = 5` and `present_effect` other than FSR before any game change; `pdz_fov_scale` widens the view (makes them smaller) |
| VR3 | Particle culling by draw-count budget, not GPU time | read | fine | - |
| VR4 | **Dynamic shadows: two cascaded shadow maps from one directional light.** The first light in the view's list that is directional (+680 == 1) and casts shadows (+820) gets them (`sub_825F32A8` 825F3408..3440). Casters are drawn by `sub_825FCF40` (from `sub_825F3120` in the frame render `sub_82611D60`), twice per cascade, into 1344 x 1344 D24S8 maps (`825FCFBC li r11,1344`; maps 82E1B364 / 82E1B368 and 82E09150 / 82E09154, two of them shared with post-FX targets). Per view mode at view +796 (82E3B93C): 0 both cascades, 1 the far one only, 2 off; a global flag 82E3B944 skips the pass. No blob or stencil shadows; the engine's older 512 x 512 single-map module (`sub_82751EA8` / `sub_8275ADF0`) is never called. Materials named `*_noshadow_` / `*_notshadowed_` don't receive. World lighting is probably baked (not checked) | read | fine | - |
| VR5 | **The pop-in distance: shadows stop at 20 m and fade out from 16 m.** `sub_825FC758` (per light, per view; identical in TU3) sets the near cascade to 300 units (`825FCA44 lfs f27` = 300, stored as the split), the far cascade, which is also the cut-off, to 2000 (`825FCA54 lfs f26` = 2000, stored at `825FCAA0` into light[432+view], used to size the far cascade's box at `825FCAEC` and the ortho fit `sub_825FDBA0`), and the fade start to 1600 (`825FCA24 lfs f13` = 1600, stored at `825FCA4C` into light[424+view]). Receivers read both back (`sub_825F7AA8` 825F8114, `sub_825F8440` 825F873C, `sub_825FDA48` 825FDB4C) and fade = 1 - (d - 1600) / 400, clamped. Units are cm. No per-level or per-object data feeds these; no caster count limit was found in `sub_825FCF40` (culling / LOD inside its draw pass, `sub_82613D38` pass 3, was not audited) | read (shader use of the fade inferred) | deliberate (performance / texel density) | `pdz_shadow_distance_scale` (VR7) |
| VR6 | **What raising the distance costs.** (1) The far cascade's map stays 1344 x 1344 (EDRAM: 7.2 of 10 MB, and aliased with post-FX targets, so the guest size can't safely grow; `resolution_scale` 2 already renders it at 2688 on the host), so its texels grow with the distance: about 2 cm / texel at 20 m (1 at scale 2), 2x / 3x coarser at k = 2 / 3: softer, shimmering far shadows (no texel snapping was seen in the box fit), thin casters may drop out, acne may change (the depth bias wasn't found). The near cascade (to 3 m) is untouched. (2) The light's depth range is fixed at 0..20000 with the eye 10000 back (`825FCA48`, `825FDC5C`, `825FC794`, shared constants), so the slice fits up to about k = 4; past that far shadows clip. (3) Casters inside the far box grow roughly with k^2: more guest draws (twice per cascade) and host draw calls; on the ROV path depth-only draws still run the interlocked pixel shader, so overdraw rises; the 4 resolves per frame stay the same size. (4) Fog hides shadows past the fog distance anyway, so in foggy levels a large k only costs. (5) Casters the main view has culled or swapped for LOD (not audited) can still pop in | read / inferred as marked | - | test k = 2 and 3 first |
| VR7 | **Built, untested in game (render-stage2-shadows): `pdz_shadow_distance_scale`** (enhancement, float, default 1 = original, clamped 1-4; `src/mod/render/sun_shadows.cpp`). The three constants are shared (2000.0 at 820A955C has 33 readers, 1600.0 at 820EC470 6, some in AI code; TU3 820A7794 / 820EB180), so they are never written, and swapping them around a call is unsafe (other threads read them). TU3 has the setup at `sub_8260C0D8` (launch + 0xF980). Two mid-function hooks in `midasm_hooks_tu3.toml`, no jumps: `PdzShadowFadeStart` before `8260C3CC` (launch 825FCA4C) scales f13 (fade start), `PdzShadowFarEnd` before `8260C3D8` (launch 825FCA58) scales f26 (far cascade = cut-off = fade end; loaded once per call, every receiver reads it back from the light: TU3 `sub_82607470` / `sub_82607E08` / `sub_8260D3C0` build the fade constant c.x = 1 / (end - start), c.y = start c.x + 1 from it). At 1 the hooks return without touching a register. The near split (f27, 300) stays, so the near cascade keeps its texel density. k = 2 gives shadows to 40 m fading from 32 m; k = 4 to 80 m. Native renderer only: `pdz_shadow_map_scale` (int 1-2) renders the 1344 x 1344 shadow surfaces (depth and the R32F colour target) and their four resolve mirrors at that multiple of the native scale; the receivers sample in normalized coordinates (point sampled, soft compare saturate(60 dz) over the fixed 0..20000 depth range) with no texel-size constant, so the guest constants stay. A larger guest map would still need EDRAM / alias work and is not proposed. **Polish 6b (render-play-shadowbias, untested):** the bias is the mask shader 34d98195's world-unit slope bias (literal c13: receiver pulled 4-31 cm toward the sun; no render-state depth bias), so `pdz_shadow_bias_scale` (float 1, 0.25-4, hot) scales the far cascade's bias by t = clamp(distance / native map scale x it, 1, 8) through a third hook, `PdzShadowFarDepthRange` before T `8260C568` (far cascade's ortho z column / t = light depth range 0..20000 t; casters and receivers agree; near cascade untouched); `pdz_shadow_map_scale` now 1-4 with the product capped at 12 (16128, D3D12 limit); `pdz_shadow_far_pcf` (float 0, native only) patches the mask shader with 9 bilinear compare taps in the far cascade, radius via the unused c13.z (hook `PdzShadowMaskConstants` before T `826079AC`). **Polish 6c (render-play-shcascade, untested):** `pdz_shadow_split_follow` (bool, on, hot; only with the distance scale above 1) scales the near split f27 (300) by k through `PdzShadowSplit` before T `8260C3D0`, so the cascades keep Rare's 300 / 2000 proportion and the ring moves out to 3k m; `pdz_shadow_split_blend` (float 0.25, 0-0.75, native only, only with k above 1 at the shader's first use) patches 34d98195 to blend the far cascade in over the near cascade's last 25% (band via the unused c13.w, written by `PdzShadowMaskConstants`); `pdz_shadow_stable` (bool, off, hot, both paths): the light view is built in view space with the camera's axes and each box is the slice's bounding rectangle (the original shimmer on camera turns); on, `PdzShadowStableBasis` before T `8260C380` turns the light view so a world axis maps to light +y, and `PdzShadowStableBox` before T `8260C56C` makes each box a square around the slice's bounding sphere snapped to whole 1344-grid texels from the world origin. A third cascade was judged not small (design only). Notes: PDZRenderResearch `native\stage2\polish\shadows.md`, `shadowbias.md`, `shadows2_cascade.md` | read (addresses, receivers, shaders), built | enhancement | test in a sunlit outdoor level: 16-20 m band and beyond at k = 1, 2, 3; watch shimmer, acne, far casters, frame time |
| VR8 | **The death "de-rez" effect ("DeRes") takes its colours from a snapshot the CPU reads back from a GPU resolve.** Request `sub_82510640(prop, mode)` (mode 3 = de-rez on death, 1 = rez-in on spawn): multiplayer players from `sub_82466258` (82466C10, 3-4 s death timer, mode 2 game and not DarkOps), AI characters from `sub_82529B18` (82529D90), the holo level's two holomatch participants (Mai Hem) from its script `sub_8232D868` (82331664 / 82331698). `sub_825106D8` aims a camera at the body (FOV 90, aspect 1), takes one of 64 effect slots (`sub_825E69A0`, 6384 bytes at 82DA43C0), hides the body and plays `XACT_SOUNDBANK_STOCK_REZOUT` (82510A50 / 82510A64). Next frame the renderer draws only that body (pass 9, 256 x 256) and `sub_825E6AD8` resolves it into the A8R8G8B8 texture at [82DA3958] (`825E6B20 bl 826E2480`) and queues a GPU interrupt callback (`sub_825E6BF8`, sets an event). The game's DeResThread (`sub_825E6CD0`) then reads the texture's memory **on the CPU** (`825E6D44` base = [tex+20] & ~0xFFF; `825E7008 lwzx r10,r11,r20`), keeps a pixel only if its RGB isn't 0 (`825E700C clrlwi. r10,r10,8`), tries up to 200 random pixels inside the body's projected square per particle, and stores each hit's colour (`sub_825E8238`): 32-256 particles by projected size, 24 bytes each. If none is found the slot goes to state 3 and the effect ends at once (`825E7098`). The particles are CPU-built camera-facing quads (quad list, `aid_texture_fx_circle`, each drawn 10 times as a short trail, `sub_825E70C8`), alpha = t, spread 1 + 20 (1 - t), life 1.0 s (`sub_825E8370`). No memexport, point sprites, instancing, or CPU reads of the model's own textures / vertices | read (agent + 2nd read of 825E6B20, 825E6D44, 825E7008 / 700C, 82510A50) | fine (original design) | - |
| VR9 | **Why it's missing here: the CPU never sees the resolved snapshot.** Our GPU emulation keeps resolves on the GPU: with `readback_resolve = "none"` (the SDK default) a resolve is a compute write into the GPU's copy of guest memory (or, with `resolution_scale` > 1 on the ROV path, only into the scaled resolve buffer), and guest RAM keeps whatever the CPU last wrote there (zero for fresh memory). Nothing makes CPU reads of GPU-written memory coherent (read trapping isn't implemented, `xmemory.cpp:2070`). So DeResThread finds no non-black pixel, the slot ends without particles, and what is left is exactly the symptom: REZOUT sound plays, the body vanishes, nothing appears. The GPU callback path itself works (one slot per thread cycle, gate 82E3B79C, and the sound plays on every death). `readback_resolve = "fast"` / `"some"` would hand back the previous resolve to that address (the previous victim's snapshot) or only the first one; `"full"` waits for the GPU and copies every resolve in the game (a stall per resolve; at scale 2 through a downscale step). No SDK warning is logged for this (none in 147 logs) | read (SDK, `d3d12/command_processor.cpp` `IssueCopy`, readback paths 3247-3507); **confirmed in game** (2026-10-20, Combat Simulator bot match, log 140): 60 de-rez requests, 27 slots granted (the REZOUT sound), 27 snapshot resolves reached the GPU callback, and all 27 DeResThread reads found **0 of 65536** pixels non-black in guest memory -> 0 particles, slot state 1 -> 3. `frame_pacing_log`: 0 memexport draws, no skipped draws, 44-46 resolves per gameplay frame | emulation gap | **fixed**: `pdz_fix_derez_snapshot` (VR10) |
| VR10 | **Fix (built 2026-10-20): `pdz_fix_derez_snapshot`** (game fixes, on by default, F11). The SDK gained resolve readback ranges (`rex/graphics/resolve_readback.h`, in rexruntime; SDK commit with this change): a resolve into a registered guest physical range takes the existing synchronous readback path (GPU wait, downscale at `resolution_scale` > 1, copy to guest RAM) whatever `readback_resolve` says, before the command processor reaches the game's GPU callback; every other resolve is unchanged. Game side (`game_fixes.cpp`, called from `fx_log.cpp`'s `sub_825E6AD8` hook) registers the snapshot texture's memory ([[82DA3958]+20], 256 KB); turning the fix off removes the range and clears that memory so the original behaviour (no cubes) returns instead of the last snapshot. **Tested 2026-10-20 (Combat Simulator, log 141): 52 snapshot reads, 0 empty, 2445-44154 non-black pixels each, 256 particles per death; the user saw the cubes.** (Before the clear-on-off change, F11 off still showed cubes: every read returned the last snapshot read back, 11214 pixels.) The earlier options, for the record: (a) Settings only: `readback_resolve = "full"` would fix it at the cost of a GPU wait on every resolve: 44-46 per gameplay frame (measured), so not usable. (b) Targeted, recommended: read back only this snapshot. A small SDK entry point "read back guest range [a, a + 256 KB) now" (wait for the submission that resolved it, copy from the shared memory buffer or the scaled resolve buffer through the existing downscale, write guest RAM through the unprotected view), called game-side from a pre-hook on `sub_825E6CD0` with the texture base [[82DA3958]+20] (or by marking that range "read back synchronously" from a hook on `sub_825E6AD8`). The wait happens on DeResThread, not the game thread, once per death. (c) A pure game-side workaround without GPU data (e.g. colours from the model's diffuse texture) would differ from the original and isn't proposed | emulation fix | - | done; also confirmed by the user in mission 4's holodeck fight (2026-10-20): Mai Hem de-rezzes into cubes, and her respawn plays the rez-in (mode 1, the same snapshot path), which was missing too. Every user of the effect is covered: 25 call sites of the request `sub_82510640` (players / bots, AI deaths, the holo script's 15 calls, including the support weapon drops (user confirmed 2026-10-20: they rez in with the cubes), attached items via its own recursion at 825106B0, and the mission 13 Zhang Li fight, where he summons soldiers' bodies as a shield: rez-in from `sub_8238D1D8` (8238D4F0) beside his summon / absorb / impact-pillar effects; not yet seen in game) all go through the one snapshot texture, resolve (`sub_825E6AD8`, only from 82407324) and reader. In two logged Combat Simulator matches the effect ran only for characters (rez-in at spawn from 8244A690, de-rez at death, their held items); weapon / ammo pickup respawns never requested it, and can't: no pointer to `sub_82510640` exists, none of its 25 callers is pickup code, and the prop update's call (8250D348) only passes mode 0 (cancel) |
| VR11 | **Series X backwards compatibility** (sound plays, no cubes) fits the same gap: an emulator that keeps EDRAM resolves in GPU memory (PDZ is an Xbox One X Enhanced title, so it also renders upscaled there, like our `resolution_scale` 2) and doesn't copy that 256 x 256 resolve back before the CPU reads it gives the CPU black pixels and the same early end. Inferred: Microsoft's emulator wasn't examined | inferred | - | - |
| VR12 | **Transparency vs water (Rare's bug; fix built 2026-11-07: `pdz_fix_transparency_water`, game fixes, on by default, F1 toggles it).** Nightclub's harbour: looking at the sea through a street-lamp glow, the water covers the glow. Rare's transparent stage `sub_826216D0` draws a bucket with the water's first layer (`sub_82622510` -> 82621ABC), then the additive glows (`sub_82603AC0` -> 82621AE0; draws from 8260AC80, PS d5ce1e70), then resolves the scene to 1D700000 and draws the same water meshes again as the second layer (the refraction fog, PS a2917857; bucket -> 82621B68), which composites that copy over the water area. The glows write no depth, so the second layer paints over every glow in front of the sea. The same calls in the same order in the emulator path. **Fix:** the glow call is held and made right after the second-layer bucket (before the next resolve); a fallback at the stage's end. Confirmed in play testing (7 Nov): the lamp glows over water and the glass are right with it on. Picture gate unchanged (1.54 / 1.4%). Not covered, parked (one area in one level): the Combat Shield in Trinity 2, where the water stops at the shield's edge. Rare's see-through stage first writes every see-through surface's depth (bucket -> 82621A30), so only the nearest one is drawn at a pixel; the shield's clear panel (its own draw, PS 13bffa03, 4164 indices) hides both water layers behind it. A fix would leave its mesh out of that depth pass and draw its two layers after the water's. Test aid for such captures: `pdz_trace_on_mark` (off; F12 then traces two frames; it needs the GPU plugin beside the native renderer and no record thread). Diagnostic: `pdz_transparency_trace_frame` (`pdz_transwater:` lines), test aid `pdz_transparency_test_alternate`. `src/mod/transparency_water.cpp`. | by hand (Nightclub glows, glass) | fixed (glows); the Combat Shield parked | - |
| VR13 | **Water animations snap back (fix built 2026-11-07: `pdz_fix_water_loop`, game fixes, on by default, Insert toggles it).** Rare's water animations are stacks of frames played as loops: Nightclub's sea ripples (DXN 128x128x16, layer coordinate VS c35.w, 0.25 a second, wrapped by 1.0 every 4 s), the caustics on walls near water (PS 858ecd68, DXT1 256x256x32, layer coordinate PS c12.w, 0.154 a second, wrapped every 6.5 s), a 50-layer stack (PS 89e948e7, VS c19.w, every 2.6 s), 19-layer stacks and others. Each material has its own clock and rate (no shared timer); each wraps its coordinate by 1.0. Their fetch constants say repeat on the layer axis (clamp_z 0), but a stacked texture's layer was clamped to the first / last layer (as the SDK models it), so at each wrap the animation froze, then jumped back. Found with draw dumps of the Nightclub harbour (7 Nov: each layer coordinate is the only constant of its draws that jumps; the periods match the ones seen in play testing, about 5.5 s on the sea and 7 s for the caustics). **Fix:** native_draw.cpp `WaterLoopMask` puts the pixel shader's stacked slots of 8 or more layers whose fetch says repeat on the layer axis in SystemConstants g_Reserved.y; `pdzTfetch3DS` (native_hlslfix.h) wraps those layers (the last blends into the first). 2-layer stacks and clamp fetches are unchanged; Rare's data and the emulator are untouched. Earlier tries, dropped: the SinWaves phase scale 0.98 -> 1.0 (no visible change), a +64 mark on the sea's texture matrix only (replaced). Datacore gate 1.56 / 1.4% (fix off, same build: 1.54 / 1.4%; today's spread 1.50-1.56). `src/mod/render/native/native_draw.cpp`, `native_hlslfix.h`. | by hand (Nightclub sea, pool, running water, caustics, River) | fixed | - |

## Graphics effects in the recomp (2026-10-22, batch task 2, read-only)

Which screen and special effects could draw wrong here, as the de-rez
effect did (VR8-VR11). It lists how each one draws, as far as the exe and
data show it, against what our GPU emulation does differently:
- **Resolves are not read back to the CPU** (`readback_resolve = "none"`;
  only ranges registered through `resolve_readback` are).
- **Occlusion queries are answered from a cache** (`occlusion_query =
  "fast"`).
- **Resolves at `resolution_scale` 2 go to a scaled buffer.**
- **Memexport**: none is used (measured 0 memexport draws in gameplay).

The render code has few names, so several "how it draws" entries are
inferred from the data and marked as such.

| # | Effect | How it draws | Risky pattern | Risk |
|---|---|---|---|---|
| GF1 | **De-rez / rez-in** cubes | body snapshot resolved, then **read on the CPU** | CPU readback | fixed (VR8-VR10) |
| GF2 | **Depth of field** (cutscenes: camera channels `.PDZ_DepthOfField`, `PDZ_FocusInner` / `Outer`, focal point) | the scene and its **depth** resolved to textures, blurred by focus distance (inferred from the channels; the D3D depth-resolve path is linked in) | depth resolve read as a texture, at scale 2 | medium; **seen working by the user (2026-10-22)** |
| GF3 | **Refraction / heat haze** (`BlendMode_Refractive`: rocket, car, jetpack and hovercraft explosions, dropship boosters, revive kit, Zhang Li's sword, `cut_13...refract`) | particles sample a resolved copy of the scene behind them | texture offsets of a scaled resolve (the SDK's `draw_resolution_scaled_texture_offsets` handles this) | medium; **seen working by the user (2026-10-22)** |
| GF4 | **Cutscene "framebuffer FX" and white transitions** (`.PDZ_FramebufferFx`, `PdzTransition_BlurToWhite` / `BlurFromWhite`) | full-screen pass over a resolved frame | resolve + full-screen blur | medium; **seen working by the user (2026-10-22)** |
| GF5 | **Motion blur** (Options > Video, `DARKVALUE_OPTION_MOTIONBLUR`; cutscene channel `.PDZ_MotionBlur`) | previous-frame blend (inferred) | resolve history across frames | low-medium |
| GF6 | **Water reflections** (`ShaderType_Water`, `_ReflectionPlane_`) | mirrored scene rendered to a texture | render-to-texture | low-medium |
| GF7 | **Shadows** (VR4-VR7) | two cascaded depth maps; the far map shares EDRAM with post-FX targets | EDRAM aliasing (the SDK's render-target cache transfers ownership) | low-medium |
| GF8 | **Cloak** (`WeaponVisualEffectType_Cloaked`, `CutsceneCloak` at `8251D77C`; enemies' "player cloaked" lines) | refraction-style shimmer (inferred) | as GF3 | medium; **seen working by the user (2026-10-22)** |
| GF9 | **Night vision, X-ray, threat detector, audioscope** (`WeaponVisualEffectType_*`; X-ray swaps in `xray_skeleton` heads / hands; scope menu scenes `small` / `large\scopes\*`) | full-screen colour pass + model swaps + menu overlays | ordinary passes | low |
| GF10 | **Flashbang** (whiteout, `flashbang_tinitus` unused) | white transition as GF4 | as GF4 | low-medium |
| GF11 | **Damage and hit overlays** (`hud_hitflash`), HUD, scopes | menu (XUI) overlays | none | low |
| GF12 | **Fog** (per-level linear fog, height fog) | built by the SSM shader generator | none (shader translation) | low |
| GF13 | **Brightness, gamma and colour tint** options | gamma ramp at scan-out (inferred) | the SDK implements gamma ramps | low |
| GF14 | **Particle culling** | draw-count budget (VR3). A "render load" value (`82E3C638`) above 0.4 culls non-critical emitters (`sub_82611C98`); no writer was found in this pass, so it may stay 0 | if a GPU-time metric fed it, our timing could differ | low (inferred) |
| GF15 | **Occlusion queries** | the D3D query code is linked in; no lens-flare or sun-glare effect was found | `occlusion_query` "fast" | low |

**What to check in game, ranked.** Compare with the Series X if possible.
1. **GF2 depth of field.** *Seen working in play testing (2026-10-22).* Watch any cutscene with a focus change (Datacore
   intro, Mansion mid when Jo meets Mai Hem). Correct looks like the
   background going soft while the speaker is sharp, with no halo of hard
   pixels around the sharp edges. Look for a whole frame that's blurred,
   or no blur at all.
2. **GF3 heat haze.** *Seen working in play testing (2026-10-22).*
   - Fire a rocket at a wall (River or Outpost), or watch a dropship's
     boosters (Trinity | Escape).
   - Correct looks like a wobbling distortion of what's behind the
     explosion.
   - Look for a shimmer that is offset or doubled, or a black or
     stretched patch.
3. **GF8 cloak.** *Seen working in play testing (2026-10-22).* Watch any cutscene where a character cloaks. Correct
   looks like a see-through shimmer. Look for an invisible-with-no-shimmer
   character, or a solid black silhouette.
4. **GF4 / GF10 white transitions.** *Seen working in play testing (2026-10-22).*
   - Throw a flashbang near yourself (Subway or Nightclub).
   - Watch the Arena intro and outro (Chandra's energy, Zhang Li's end).
   - Correct looks like a soft bloom to white and back. Look for a hard
     cut or a frozen frame.
5. **GF5 motion blur.** Toggle it in Options > Video and spin quickly.
   The difference should be visible, with no ghost of an old frame
   lingering.
6. **GF6 water.** Look at the Nightclub harbour water and River's river.
   Correct looks like reflections that move with the camera.
7. **GF7 shadows.** On a long view in Jungle or Temple, look for flickering
   or garbage blocks at the shadow edge.

Logging would help: `pdz_sound_log` is unrelated; `frame_pacing_log` counts
resolves. No new switch was built for this (the batch's only new switch is
task 5's).

## Missing sounds in levels (2026-10-22, batch task 5, read-only + one log switch)

Single-player areas often sound silent, not just quiet. This traces how
level sound is made, what could silence it, and how to compare with the
Series X. TU3 addresses.

**How level sound works.**

| Layer | What plays it | Data | In the recomp |
|---|---|---|---|
| **Ambience** (4-channel loops, 10-180 s) | the ambience manager, 4 slots (`AmbienceID_1`-`4`, table `82D3ECC8`). At level start (`sub_82204228`) each ambience the level's music setup names is created and, if its play flag is set, **started at Ambivol 0**. Region props and scripts then fade a slot up or down or switch its cue (`PropMsg_Audio_AudioFadeUp` / `FadeDown` / `SwitchAmbienceTunes`; `sub_82204508` sets Ambivol = the slot's percentage) | `aid_xml_musicsetup_single_<lvl>` (ambience names), level regions | all guest code, same as the 360 |
| **Ambivol** | pdz.xgs RPC curve 1: variable **Ambivol** (per cue, **initial 0.0**) -> volume, **-96 dB at 0** rising to 0 dB at 1 | `Audio/pdz.xgs` | guest XACT evaluates it, same as the 360 |
| **Distance** | pdz.xgs RPC curve 2: Distance -> volume, 0 dB near, -96 dB at about 14,000 units | pdz.xgs | same |
| **Sound emitters** | `soundemitter` setups, looped `stock` sounds (fountains, waterfalls, streams, fans, fires) | per level, see below | same; no emitter has a difficulty or mode filter |
| **Music** | music type (None, Interactive, Tune1-4, Death) and, for Interactive, the Stealth / Speedup layers at level 0 / 1 / 2 (game event 44 sets the level from the AI's alert state) | music setup | launch build: native. **tu3-base: restored from launch** (TU3 removed it, U5) |
| **Output** | the guest's XAudio renders 5.1, and the SDK folds it to stereo | - | **SDK**: front 1.0, centre and rears 0.707, LFE dropped, all x 0.586 (-4.6 dB), not configurable |

**What each level asks for** (all resolve to real cues in banks the level
loads; scratchpad `amb\levels.py`):

| Level | Ambiences in the music setup | Sound emitters |
|---|---|---|
| Datacore | Trinity ambi main, deck | - |
| Nightclub | ambi city (+ club, panic switched by script) | 11 small fountains, 6 big water laps |
| Subway | carpark, city, subway | - |
| Rooftops | boss, city, wind | 1 water lapping |
| Mansion | ambi main | 16 streams, 4 water lapping, 2 falls, 1 large waterfall |
| Laboratory | room tone, lab, air duct, security | **26 extract fans**, a generator, 1 water lapping |
| River | ext, river, room tone, security | **15 large waterfalls**, 1 water lapping |
| Trinity \| Infiltration | ambi main, deck | - |
| Trinity \| Escape | **Infiltration's** ambi main / deck (it loads both banks; its own `trinityescape_ambi_*` are never named) | - |
| Jungle | ambi main | 10 small fires, 5 waterfalls, 5 water lapping, 3 streams, 1 fountain, 1 big laps |
| Temple | ext, int | 3 waterfalls, 3 water lapping |
| Outpost | ambi main | 2 small fountains |
| Bridge | exp (script switches to noexp) | - |
| Arena | final ambi main | - |

**Likely causes of a "silent" area, ranked.**

| # | Cause | Recomp-specific? | Evidence | Check |
|---|---|---|---|---|
| MS1 | **The area has no ambience faded up.** Ambiences start at Ambivol 0 (-96 dB) and only become audible when a region or script fades their slot up. Areas outside every ambience region stay silent by design. | no (same data and code on the 360) | `sub_82204228`, `sub_82204508`, pdz.xgs | `pdz_audio_presence_log` 5 s lines: "ambience 1: <cue> (play 1, Ambivol 0%)" in the silent spot means Rare left it silent; compare on the Series X |
| MS2 | **Very quiet ambience recordings** | no | Measured RMS per channel: Temple interior **-41 to -47 dBFS**, Bridge "noexp" -42, Trinity deck -32 to -34; most others -16 to -30 | listen on the Series X in Temple's interior and Trinity's deck |
| MS3 | **The SDK's stereo fold lowers everything by 4.6 dB** (0.586 scale, to avoid clipping), so quiet ambiences sink closer to the noise floor than on a 360 at the same volume | **yes** (a level difference, not a balance one) | `rex/audio/downmix.h`; no title or cvar sets `SetStereoFold` / `SetOutputGain` | the same spot at equal amplifier volume vs the Series X. Enhancement candidate: an output gain or fold option (+3 to +4.6 dB with a limiter). *Built 2026-10-22 (untested): SDK cvar `audio_stereo_fold_gain_db` (default 0; +4.6 to match), soft-limited.* |
| MS4 | **Rear channels at -3 dB in stereo** | yes, but minor | the 29 four-channel ambiences are balanced (rear minus front -1.5 to +1.3 dB, except Nightclub's club -11.7 and Temple interior +3.9) | - |
| MS5 | **tu3-base only: the restored interactive music after a mid-level cutscene** (the end-of-cutscene resume hook `821F9814` is untested) | yes (our port) | `src/music/music_restore.cpp` | in tu3-base, a level with a mid-level cutscene (Mansion mid, Outpost mid): music should resume afterwards. The 5 s lines show "player state 2" when it plays |
| MS6 | **Known broken sound names** | no | sound audit S1 (metal table hits, `XACT_SOXACT_`), S2-S4; Urban's spawn ambience (fixed) | - |
| MS7 | **The game refuses sounds when busy** (Prime returns 0: no free cue, or RequestToPlay declines; state 6) | no (guest logic) | `sub_82205C68` | `pdz_audio: refused` lines |
| MS8 | **XMA looping or decoding** of long 4-channel loops | possible | the SDK's XMA decoder implements hardware loop points (`xma_context.cpp` `UpdateLoopStatus`, loop subframes) | a long stand in one area (> 3 min): the ambience should not stop. A stop with no `pdz_audio: stop` line would be a decoder issue |
| - | **stock_rpc** (task 5.5) | no | Its four engine loops reference RPC curves pdz.xgs doesn't define (pdz.xgs has only Ambivol and Distance, no speed variable), and its wave bank isn't on the disc, so nothing loads it. The `stock` twins play the same loops without RPC (flat, Vehicle category) | engines lack speed-driven pitch or volume on the 360 too; not a silence cause |

**Starting and culling (5.4).**
- Looping cues, play-on-load ambience, instance limits (the XACT
  variable NumCueInstances), 3D distance (RPC 2) and voice priority all
  run in the guest's XACT / XAudio code, exactly as on the 360.
- The SDK supplies the XMA decoder and the output stage (8 queued frames
  by default, `audio_maxqframes`; underruns stutter rather than silence).
- Nothing in the SDK culls sounds by distance or count. So a loop never
  starts, or is culled for good, only if the game decides so; the log's
  start / refused / stop lines show it.

**Interactive music (5.2).**
- `MusicType_None` / `Interactive` / `Tune1-4` / `Death`. For Interactive,
  the levels are Silence (0), Stealth (1) and Speedup (2), changed by game
  event 44 from the AI alert state, with cue markers 5 / 6 for the layer
  loops.
- **"Stuck in Silence" is possible only if no alert event comes.** Same
  as the 360 on the launch build; on tu3-base it depends on the restored
  hooks:
  - level start, pause / resume and event 44 are tested;
  - the end-of-cutscene resume isn't (MS5).
- The 5 s summary shows the music type, the current and wanted level, and
  the player state.

**`pdz_audio_presence_log`** (Debug, off by default; built at the end of
this batch):
- `--pdz_audio_presence_log=true` logs `pdz_audio:` lines:
  - each cue's first start, refusal and stop, with its XACT category
    (pdz.xgs order) and position if positioned;
  - every 5 s: the four ambience slots (cue, play flag, **Ambivol %**),
    the music state, and the sounds started by category.
- Hooks: DarkAudioSound::Prime (shared with `pdz_sound_log`), stop
  `sub_822055C0`, and the audio tick via the restored music code.
- Wave channel counts aren't read live: `out/audio/cue_channels.tsv`
  lists every cue's waves (bank, wave, channels, rate).

**Comparison checklist (Series X vs recomp, same speakers and volume).**

| # | Mission and spot | What the 360 should have | Look in the log |
|---|---|---|---|
| 1 | **Nightclub**, the start on the harbour front | city ambience; fountains and water laps near the quay | ambience slot with `NIGHTCLUB_AMBI_CITY` > 0%; `STOCK_FOUNTAIN_SMALL_LP` started |
| 2 | **Mansion**, the gardens by the waterfall | streams and waterfall, stealth music | `STOCK_WATER_STREAM`, `WATERFALL_LARGE`; music type 1, level 1 |
| 3 | **Laboratory**, inside the air ducts, then a lab | air-duct ambience, fan hum | `LAB_AMBI_AIRDUCT` %, `STOCK_EXTRACT_BLOW_LP` starts |
| 4 | **River**, the riverbank | river ambience, large waterfalls | `AIRPORT_AMBI_RIVER`, `WATERFALL_LARGE` |
| 5 | **Jungle**, the village with the fires | jungle ambience, crackling fires | `JUNGLE_AMBI_MAIN`, `STOCK_SMALL_FIRE_LOOP` |
| 6 | **Temple**, the interior | very quiet interior ambience (-45 dBFS), the best test of MS2 / MS3 | `TEMPLE_AMBI_INT` and its Ambivol % |

## Controls (parked pending the control scheme question)

Audit 2026-10-18; fixes implemented as `pdz_controls_fixes` + `pdz_fix_*`
(all default off, F2 A/B) but parked: no controls fixes are in use.

**Important caveat from the game audit** (and the Standard model audit
above: the Halo model deliberately glides out of fast look when zooming,
H10, so controls #4 is probably intended too): the look controller picks its model
by input +0x484 (`825A43E0`: 1 = the "Halo" records, otherwise the "PD"
records), and the Controller Style option has exactly two values, "CLASSIC
PD | STANDARD". The "PD" model is therefore most likely **Classic PD** and
Standard most likely the "Halo" model. If so, findings #1 and #2 below (move
stick read by the look model) are intended N64-style one-stick behaviour, not
a leak, and the PD-only fixes (`pdz_fix_fastlook_stick`, `_decel`,
`_accel_data`) don't affect Standard at all. Settle it in game: `pdz_look_log`
prints `style N`; play a few seconds on each scheme.

| # | Area | What the code does | Evidence | Verdict | Fix (parked) |
|---|---|---|---|---|---|
| 1 | Fast look (PD model) | "Stick fully pushed" = sqrt(look x^2 + move y^2) | `825A08DC`/`08F4` (move stick getters), `825A0C5C`-`0D04` | likely Classic PD design (was "likely bug") | `pdz_fix_fastlook_stick` |
| 2 | Cross deadzones (PD) | scale with the move stick | `825A0D08`-`0DA4` | as #1 | `pdz_fix_fastlook_stick` |
| 3 | Auto-aim lock radius | (r14 + r1C t) t, none when t = 0 | `824E2408`/`2410`, `824E2440`; r08 = 0 path needs t > 0.1 (`824E2328`); lead x t^2 | leans deliberate ("assist while sweeping") | now an enhancement, `pdz_aim_still_lock` (on by default): A/B in Combat Arena, Falcon only: hits 35% -> 59% of tracked shots |
| 4 | Fast-look slow-down (PD) | changes speed at the fast-look accel also when slowing | `825A0EF4`-`0F34` | likely bug (Classic PD only) | `pdz_fix_fastlook_decel` |
| 5 | Sensitivity | x1.2 / x1.75 blend past the max record | `825A09E8`, `825A13A4`, `sub_8259D140`/`D170` | likely bug (both schemes) | `pdz_fix_sensitivity_clamp` |
| 6 | Max sensitivity curve | exponent 2.84: slower below ~61% stick | data | deliberate | none |
| 7 | PD default fast-look accel | 2 between 3 and 4 | data | likely typo (Classic PD only) | `pdz_fix_fastlook_accel_data` |
| 8 | Turn integration | last frame's speed; 0.5 a dt^2 without x60 | `825A0FB4`-`0FD0` | bug, small | not implemented |
| 9 | Zero target sign | asymmetric stop | `825A0E9C`-`0EEC` | minor | not implemented |
| 10 | Pad deadzone | per-axis 15.3%: slow diagonals | `sub_8226BE48` | deliberate | none |
| 12 | Spread recovery gate | min() of hand idle times; empty-hand case never seen in game | `825737BC` | fine | drop `pdz_fix_spread_recovery_gate` |
| 13 | Rotational aim assist (`sub_8259FD70`) | strength 0.8 / 0.75 / 0.5, +-70 deg/s (10-30 on close targets), not zoomed; seen active in game. It turns the view and counter-turns your own slow stick input (H16) | read, test (Auto Aim off removed the heavy feel) | deliberate; the cause of the "wrestling" feel | considered `pdz_fix_rot_assist_self_turn` (subtract own view turn, reset on target switch); not kept: the original was preferred in play testing |
| 14 | Sniper / Shockwave and PA/DA zoom rule | gun types 0x25/0x26 | `825A35E8`-`3638` | deliberate | none |
| 17 | FOV | 58 deg is the 4:3 horizontal FOV (45.2 vertical, ~73 across at 16:9) | `82205900` -> `sub_82644AF0` | fine (docs corrected) | none |
| 18 | Zoom steps (enhancement; parked 2026-10-20, built 2026-11-04) | Any aim / zoom press zooms to the first level; R3 (pad) or the wheel (mouse) steps through the gun's own levels. Rare's zoom: the LT depth picks the level (`sub_825AC408`, docs/MOUSE_KEYBOARD.md "Step zoom") | `825AC408`, `825AC3B8` | read, tested by script | `pdz_zoom_steps` (off) |
| 19 | Reload / secondary presses "sometimes not registering" (play-test report, 2026-10-20). **Our input path:** the game polls the pad once per frame on its main thread (`sub_82714230` in task "JoyThreadEndFrame" `8226CC70`), keeps its own copy of the previous buttons (82E2A868) and computes edges once per poll; the pads commit them for exactly one frame (`sub_8226CB88` via `sub_8226C2E8`); it never reads `dwPacketNumber`. Two other GetState calls per poll (stick centre learning `sub_8226BE48`, "connected" `sub_8226C718`) discard the state. So `input_poll_on_read`, late latch, vblank pacing and the 60 fps patch can't consume an edge; a press shorter than one frame (down and up between two polls) is lost on every input path (no latch, also on the 360). FR4's debounce (`sub_82398568`) reads no input. **The game:** the hand (`sub_825781E0`) asks for a reload only on the frame X goes down (`82578500`) and the gun refuses it unless its action (`gun+4`) is 1, 5, 6 or 12 (`sub_822322A0` 822322BC..E0), the clip has room and there is reserve ammo (`sub_82232570`); B asks for a fire mode switch only on the press frame (`82578640`; held only while already switching) and the gun refuses it unless the action is 1, 10, 12 or 18 (or 14 with +24) (`sub_822336D0`). So a press during a shot (2), automatic fire (3), the post-shot window (5 / 6, B only), raising / lowering (8 / 9) or a switch is dropped with no buffer. A reload queue exists in the gun update (`gun+8 == 10` -> reload at 82233268) but nothing writes 10 there | `pdz_button_log` | design (no input buffer; matches "first press ignored, second works"); dormant queue, not a discarded one | not built; possible enhancement: buffer a refused X / B for ~250 ms and replay it (OR the bit into the pressed word around `sub_825781E0`) |

## Standard look model ("Halo", `sub_825A1280`) (audit 2026-10-19)

The model for input +0x484 == 1, most likely the **Standard** scheme (to be
confirmed with `pdz_look_log`, which now prints the model and scheme). It is
cleaner than the PD model. Confirmed by reading the quoted instructions;
numbers at 60 fps, stick percentages physical (including the pad's 15.3%
square deadzone).

| # | What the code does | Evidence | Confirmed | Verdict | Impact |
|---|---|---|---|---|---|
| H1 | Reads only the look stick (no move-stick leak) | `825A15C4` / `15E0` (vtable +0x1F0 / +0x1EC) | read | fine | - |
| H2 | Radial, direction-keeping curve: magnitude^e, e = field 2 (field 3 while zoomed or in cover) | `825A1610` / `1618`, `sub_826286E8` | read | fine | - |
| H3 | Cross deadzones after the curve, raised to the exponent, each scaled by the other axis; none while zoomed. Pitch deadzone 13.7% with no horizontal input, 45.5% with the stick fully sideways; yaw 3% | `825A1678`-`16B4`, `1708`, `16A8` | read | deliberate | medium (diagonal / vertical fine aim) |
| H4 | The deadzones (fields 6 / 7) come only from the default record's stack copy, never blended: the 0.455 pitch-deadzone "dip" is just the default's value; min / max 0.5 are never used | 13 words copied from `82DA376C` at `825A1374`-`1390` | read | deliberate (the dip has no effect) | none |
| **H5** | Below default horizontal sensitivity (blend parameter < 0.5; exactly 0.5 uses the defaults, `825A1474`), field 3 (zoomed / cover curve exponent) blends min -> **max** instead of min -> default; every other field in that branch uses the default record | `825A1484 lfs f9,0xc(r9)` with r9 = max record (`82DA3764`), neighbours `lfs fN,..(r11)` = default | read, 2nd read, 3rd read | **bug** (wrong register) - **fixed: `pdz_fix_halo_zoom_curve`** | low (X below default and zoomed / in cover; none at default X) |
| H6 | x1.2 with Fast Look off and x1.75 with input +0x4F2, never clamped: the blend runs past the max record | `825A13A4`, `13DC`-`146C` | read | likely bug (as PD #5) | low-medium |
| H7 | Higher sensitivity steepens the curve (1.964 -> 2.634): at max, stick below ~41% turns slower than at default | data | read | deliberate | medium for high-sensitivity players |
| H8 | Vertical sensitivity only blends the pitch rate (field 1); curve, deadzones, accel follow horizontal | `825A1524`-`1564` | read | deliberate | - |
| H9 | Fast look: on when not zoomed, x after curve > 0.9 and magnitude > 0.99 with the option on; off below 0.97; speed blends up to field 11. Full diagonals (0.925 after the pad filter) can't trigger it | `1638` / `1648` / `1654` / `1660`, `18BC` / `18C0` | read | deliberate | medium |
| H10 | Zooming or entering cover while turning faster than the zoomed maximum explicitly turns fast look on, so the view glides down at the fast-look accel (~0.7 s from 180 deg/s) instead of braking; with Fast Look off it brakes | `825A1804`-`181C` (`stb r26,0(r28)`), `1888`-`18C4` | read | deliberate - and it suggests the PD "fast-look decel bug" (controls #4) is the same intended glide | medium (feel) |
| H11 | Decel x2 above the max target; accel / decel x (0.25 + 0.75 stick); stops in ~36-50 ms | `18DC`, `190C` | read | deliberate | - |
| H12 | Zero target handled (no asymmetric stop, unlike PD #9) | `1830` / `1844` | read | fine | - |
| H13 | One-frame turn lag, as PD #8 (0.5 a dt^2 without x60) | `1960`-`1980`, `1AF8`-`1B18` | read | bug, small | low (17 ms at 60 fps) |
| H14 | Zoom: x (1 - zoom) while holding an item, x aim sensitivity x field 4 when zoomed or in cover | `17A0`-`17FC`, `19FC`-`1A30` | read | deliberate | - |
| H16 | The rotational assist runs after the turn (both models) and **turns the view** (vtable +40 / +44) by the target's drift across the view x per-difficulty strength, which includes the drift from your own turning, so it counter-turns slow stick input by 50-80% while a target is acquired (corrected 2026-10-19; it also writes auto-aim +0x5C / +0x60 for shot bending). | `825A1BA0`, `825A02B8` / `02DC`, `825A03D4` inside `sub_8259FD70` | read | deliberate (likely shortcut); stale error on a target switch is a possible bug | high (feel) |
| H17 | Field 5 (0.6) passed to the assist and never read there | `8259FD88` | read | dead parameter | none |

Numbers (Standard, Fast Look on unless noted):

| Setting | Yaw max | Fast-look max | Fast-look ramp 65% -> 90% | Yaw at 25 / 50 / 75% stick |
|---|---|---|---|---|
| default | 64.8 deg/s | 180 deg/s | ~0.41 s (+~30 ms to 65 deg/s) | 0.9 / 11.2 / 32.6 deg/s |
| default, Fast Look off (x1.2) | 75.2 | - | - | 0.8 / 11.6 / 36.1 |
| max | 117 | 396 | ~0.80 s | 0.4 / 11.2 / 46.6 |

| Vertical setting | Pitch max | Pitch at 25 / 50 / 75% stick |
|---|---|---|
| default (0.5) | 64.8 deg/s | 0 / 10.1 / 31.9 deg/s |
| -3 notches if a notch is 0.05 (0.35) | 54.0 | 0 / 8.4 / 26.6 |
| -3 notches if a notch is 0.1 (0.2) | 43.2 | 0 / 6.8 / 21.3 |

The slider's notch size is not known yet: `pdz_look_log` prints the
sensitivities (input +0x4D4 / +0x4D8) so a few steps of the slider settle it.
Accel from rest ~36 ms; zoomed (2x, aim sensitivity 0.5) ~13 deg/s.

## Enemy cover AI

See `src/mod/README.md` (Enemy cover, Enemy cover fixes): grace timers
started at task creation, the never-true 1-in-3 peek roll and the unlimited
snap-back are fixed (`pdz_ai_cover_fix_*`, on by default); points with no or
too many peek points and the COVER behaviour mapping are deliberate / unused.

## Known bugs from the community (for cross-checking)

Rare's May 2006 title update fixed: the Plasma Rifle / hovercraft turret 10x
fire rate, split-screen rocket zoom affecting other screens, one player
controlling both screens in split-screen co-op, a crash when signing out while
a level loads, and several multiplayer issues. Later updates reportedly patched
the cover wall clip and broke some mission music scripts and dub lines (on
real hardware). Speedrun guides document out-of-bounds rolls, a squad-skip
softlock (Jonathan stuck seated) and a Datacore scientist objective that can
fail. Emulator-only (Xenia): a softlock when rolling caused by an inexact VMX
dot product (`vdot`) - worth confirming our recompiler's is exact - plus
sound looping and shader issues. Sources and grades in the audit notes.

## Title Update 3 changes (2026-10-19, for decision)

### Every gameplay-relevant change, and its status (2026-10-20)

Title updates are cumulative, so launch vs TU3 (0.0.0.4 -> 0.0.3.4) covers
TU1-TU3. Public notes exist for TU1 (May 2006) and TU3 (Nov 2006, "Cover
Move" exploit); none were found for TU2.

**Coverage.** Every function in both exes was matched (69,790 launch /
70,171 TU3), so nothing in the code is missed outright. All 856 changed
functions are classified: the 463 in the gameplay ranges were triaged on
2026-10-19, the other 393 (frontend, multiplayer modes, profile, network,
engine libraries) on 2026-10-20. The decisive instructions of the flagged
ones were read in both images, but not every changed function was read line
by line: the large multiplayer refactor (per-player arrays -> participant
records for bots) was spot-checked. Added / removed functions are classified
by region. Every float constant loaded by the matched functions (whole exe)
was compared: **none was retuned**; the changed immediates in the 2,734
"offsets / constants only" functions are all struct offsets, sizes and
counts. Gameplay tables in the exe (auto-aim, look, noise) are filled at run
time from package XML and don't differ. All 25 records in the TU's data
package (`1726605008.pkg`) were compared with launch's. Nothing here was
tested in game. Gaps: eight public TU1 fixes were not found in the code
(listed under the table); they may sit in the multiplayer refactor.

Kind (for "not ported yet"): **bug** = genuine bug fix (on by default under
the fix rules), **design** = balance / design change or new feature (your
decision), **online** = Live / network only (irrelevant offline). Split
screen and bot matches count as offline. Effort: small = one hook / record,
medium = several hooks or TU data, large = new subsystem.

| # | Change (launch -> TU3) | What it changes / fixed | Area | Status in our build | Kind | Effort |
|---|---|---|---|---|---|---|
| U1 | Cover cooldown, player +7520 (`824F11C0`, `824F1610`, `824F2E20`) | 1 s before cover can be entered again: the "Cover Move" exploit (hiding in geometry), CV9 | exploit (MP, also SP) | **ported**: `pdz_fix_tu_cover_cooldown`, on | - | - |
| U2 | DLC support: mounting, file lookup, path rule, manifest, level list merge, pack string tables (38 slots), cue lists, larger sound bank table + loader / unloader / idle check, per-level MP strings, map pictures | Map Packs One and Two | DLC | **covered differently**: `pdz_dlc_maps` (on), own hooks on the launch exe (`docs/MAP_PACKS.md`) | - | - |
| U3 | String table loader checks for a missing table | Crash on a missing table | DLC / crash | **covered**: `pdz_dlc_maps` skips tables no pack has | - | - |
| U4 | Prop template master list + 9 entries | 8: pack doors / teleporters; 1: Desert's hit-fix prop `thing -> example` (corrected: not a Township prop) | DLC, collision | **ported**: the 8 with `pdz_dlc_maps`, the 9th with `pdz_fix_tu_mp_hitfix` (U18) | - | - |
| U5 | Interactive music player removed (`82200060`-`822010B0`, ~50 functions) | Levels asking for Interactive music go silent | music | **deliberately not ported**: we keep the original music | - | - |
| U6 | Wavebank loader: the language fallback moved into a content resolver (`821F1E48` -> `821F9888`; *corrected 2026-10-22: the default-language fallback is kept*) | Finds audio in mounted packs and the update folder first | audio / DLC | **deliberately not ported**: the launch fallback stays; pack audio comes through our own lookup | - | - |
| U7 | T1: Datacore script null check on +2592 (`822F8000` -> `822FFDC8`) | Crash in Datacore when the tracked character is gone | crash (SP) | **ported**: `pdz_fix_tu_datacore_null`, on (mid-function hooks before `822F89A0` / `822F8D64`, jumping where TU3 does) | bug | - |
| U8 | T3 / T4: bot task null checks on +76 (`821BC4D0`, `821BC7B0`, `821C08A8`, `821BF4B0`; T4 is also a +76 check, not +296 as first written) | Bot scripts (Deathmatch, Team Deathmatch, flag, King of the Hill, Protection) without their mode object | crash (bots) | **ported**: `pdz_fix_tu_bot_task_null`, on. Unreachable in launch (read): +76 is always set where launch has bots, and launch DarkOps deletes the bot manager (`82335398`); TU3 needed it for its DarkOps bots. A pure guard | bug (guard) | - |
| U9 | T5: null check in the 4-slot base table scan (`821766E8` -> `8217B2A0`, table `82D3D118`) | Crash on a multiplayer map with fewer than 4 bases when a base-assigned prop is used | crash (MP) | **ported**: `pdz_fix_tu_base_slot_null`, on (mid-function hook before `821766FC`). Whether a shipped map has 1-3 bases isn't known: the fix logs `pdz_fix_tu: base table: empty slot skipped` if it ever fires | bug | - |
| U10 | T6: LV12 Jonathan companion script (`823645C0` -> `8236C4C0`) | **No change** (corrected 2026-10-20): the "+312 == 1 cover animation" block is already in launch (`82366468`-`823665A8`); TU3's compiler gave the two branches their own copy of a shared tail. +312 is Jonathan's cover side; the queued task is an untimed cover-entry animation. Not the stuck-seated fix (AI27 is in the LV12 level script, `823686C0` / `8236CA08` / `8236F5C4`, unchanged in TU3) | SP script | nothing to port | - | - |
| U11 | T7: LV10 (jungle) script gives list entry 14 the 15000 health entry 13 gets (`82303A20` -> `8230B818`; corrected: health and max health via `sub_825C5A78`, not a 150 m range) | One character with normal health where its partner gets 15000 | SP script | **ported**: `pdz_fix_tu_lv10_health`, on (mid-function hook before `823067BC` re-runs the call at `823067B8` for entry 14). Who entries 13 / 14 are isn't identified: the fix logs the character once | bug (likely data slip) | - |
| U12 | T2 + Township hit-fix: 7 replacement background textures (T `82276D78` on load / restart) and a `hitfix_town` collision prop (Township = the Outpost mission, LV12) | Township textures and collision | SP visuals / collision | **ported**: `pdz_fix_tu_township`, on. Needs the title update's `1726605008.pkg` (installed by `tools/install_map_packs.py --update`); only its hit-fix records are registered (`docs/MAP_PACKS.md`, Title update data). Props at boot (restart to change), textures at the next load / restart | bug | - |
| U13 | T10: the flag weapon's destructor passes its holder to "flag dropped" (`82569708` -> `82578F00`; corrected: not a looping sound) | With U17: a stale drop from a non-carrier reset the carrier's flag | MP (offline) | **ported with U17**: `pdz_fix_tu_flag_drop`, on | bug | - |
| U14 | R7: XACT cue stop / release skip cues flagged 0x20 (`8274E1E8` / `8274E2D8`) | Traced 2026-10-20: 0x20 is set only by new TU3 code (`82204660`, "wave bank being unloaded", from its wavebank destroy and DLC shutdown); nothing in launch sets it, and launch already guards double stops (bit 0x2 and the STOPPING / STOPPED states, `8274E21C`-`8274E254`) | audio engine | **not ported**: can't fire in launch | not a launch fix | - |
| U15 | T11: rocket launcher guided mode needs ability bit 24 (0x1000000) unlocked (`8224B7C8` -> `82252B00`), and TU3 adds bit 24 to the hovercraft and JetPac occupant locks (`82664FD4` / `82664890`, `8267502C` / `82675B24`) | Traced 2026-10-20: 0x1000000 is the "weapon alt function" ability, not an owner flag; together TU3 blocks guided rockets while riding a hovercraft or JetPac. Plausibly TU1's "rocket alt-fire zooms other split screens", but launch misbehaving wasn't shown (the guided camera code is unchanged). `822480A8` / `82248628` were a mapping mix-up (weapon type 34) | split screen | **not ported**: not clearly a fix. Test first: split screen, player 1 fires a guided rocket from a JetPac and as hovercraft driver; watch both screens and whether player 1 can still move and use alt fire. Port design ready (lock masks via lr-filtered hooks on `sub_8253E1B8` / `2B8` / `230` / `318`, gate in `sub_8224B7C8`) Exact split-screen test steps: "Multiplayer bots and TU features", Guided rocket in split screen. | unclear | small-medium |
| U16 | R1: teleport re-inserts the character in the world's spatial lists and moves attached objects (`8242AB48` -> `82436938`) | Character left in its old world cell, attached objects left behind after a teleporter pad / out-of-bounds return | MP (offline) | **ported**: `pdz_fix_tu_teleport`, on (hooks around the move calls, lr `8242ADC8` / `ADD8` / `ADE8`: `sub_82508F78` / `82508EA8`, vtable +16 / +68 as TU3). TU3's extra `825CC480` call after the non-player warp is a separate change, left out | bug | - |
| U17 | R2: flag drop ignored unless the dropper is the carrier (`8240FFA0`, `82440190`) | Flag reset by a non-carrier's stale drop | MP (offline) | **ported with U13**: `pdz_fix_tu_flag_drop`, on. Launch passes no holder (r5 is left over), so the holder comes from the strip (`sub_8253CB78`) and only the destructor's call is checked; other destruction paths keep launch behaviour | bug | - |
| U18 | Chinatown and Desert hit-fix collision props (TU package; Desert's `thing` and its template entry) | MP out-of-bounds / shoot-through spots (TU1: Desert out of bounds) | MP exploit (offline) | **ported**: `pdz_fix_tu_mp_hitfix`, on. Needs the title update's data package; at boot (restart to change) | bug | - |
| U19 | Float-after-respawn leads: input +0x3AC zeroed on a player reset (`825413F0` -> `82550B08`); hovercraft drops an ability-disable step (`82665B70` -> `82675970`) | Traced 2026-10-20: (a) +940 is a timer only TU3's controller has (its class grew 12 bytes; Dark Ops code reads it); in launch that offset is another member, so it must not be ported; (b) the hovercraft hunks rebalance the driver's bit-29 lock and depend on other TU3 changes (alone they leave the flag set after the driver exits); neither explains floating | MP exploit | **not ported**: (a) not applicable, (b) not clearly a fix | not a fix / unclear | - |
| U20 | Remote mine: record `aid_xml_guns_multimine` (7 values) and ammo row 12 4 -> 0 in every column | **Corrected 2026-10-20:** the 7 values are the mine's ammo amounts (clip 1 -> 5, multiplayer pickup 2 -> 5, Agent pickup 6 -> 5; not animation states), ammo row 12 the free spare mines every character starts with (4 -> 0). Launch 1 + 4 spares, TU3 5 in the clip. Pack maps load TU3's record with launch's ammo row, so probably 9 mines there today | weapons (MP) | not ported; proposal `pdz_fix_tu_mines` (both halves together); see "Multiplayer bots and TU features" | **design** (consistency), not an exploit fix | small |
| U21 | R4: out-of-bounds HUD markers skipped on Ruin / Facility (`82429978` -> `82435750`, map ids 35 / 33 = our pack ids) | Return / teleport point icons drawn on the Pack Two maps while out of bounds (no crash path: every pointer is checked) | DLC (cosmetic) | **ported**: `pdz_fix_tu_pack_markers`, on (mid-function hook before `82429DF0`) | DLC support | - |
| U22 | R5: Facility / Ruin option lists and Random map (T `823BA378`, `823BE050`) | With Facility / Ruin: scenarios [0, 1], DarkOps [0, 2], 2-8 players; Random picks every map (launch: positions 0-8 only, so never GasPlant, Facility, Ruin) except Facility / Ruin for other scenarios | DLC | **ported**: `pdz_fix_tu_pack_map_rules`, on (`docs/MAP_PACKS.md`). TU3's FRIENDLYFIRE rule in the same area (T `82432678`) left out | DLC support | - |
| U23 | Turret re-equip moved to the base character update, also for occupant type 42 (`8253CEE0` -> T `8254C5A0`) | TU bots get the turret weapon; players unchanged (WP14) | bots | not ported yet (decision 2026-10-20: not ported, TU bot feature) Port plan: "Multiplayer bots and TU features", TU bots (part of `pdz_tu_bots`). | **design** (TU bot feature) | small-medium |
| U24 | TU1 bots: DarkOps bots, 8 personalities, bot UI, bot reactions to objectives (R3), AI update range / team flag (`8251DB70`), bot target filter (`825291A8`: accepts flag-0x4 targets with probability (1 - d / 40 m)^2) | New bot features | bots (offline) | not ported (decision 2026-10-20: not ported, new feature) Port plan: "Multiplayer bots and TU features", TU bots (`pdz_tu_bots`, off until tested). Correction: the "AI update range / team flag" change (`8251DB70`) is the Ghost personality's invisibility, and the target filter (`825291A8`) its counterpart. | **design** (new feature) | large |
| U25 | T8: player damage reads health through a virtual getter (`8259B738` -> `825AAF50`) | Refactor (MP / bots); no fix found | - | not ported (decision 2026-10-20: nothing to port) | nothing to port | - |
| U26 | T9: zipline camera (`825AD3C8` -> `825BCF28`) | Traced 2026-10-20: a refactor. Launch already uses camera mode 6 (`825AD4D4 li r4,6`); TU3 moved the placement into a new virtual with the same formula, for a new TU3 subclass | SP camera | **not ported**: no behaviour change for launch's zipline | not a fix | - |
| U27 | Small MP data: Temple sandbag flag, 8 explosion volumes' level ids, 2 Chinatown route point orders | Sync ids (online); bot routes | MP | not ported (decision 2026-10-20: not ported, online / design) The 2 Chinatown route points may matter to bots (test on Chinatown); the volume ids are most likely online sync. Plan: 11 names on the title update allowlist (`pdz_tu_bots`). | online / **design** | small |
| U28 | R6: sign-out during a cutscene / load stops the cutscene (`8247EE38` -> `8248B918`) | Best candidate for TU1's "sign out during a level load" crash | profile | not ported (decision 2026-10-20: not ported, online / profile only) | online / profile only: local profiles never sign out | - |
| U29 | Live and network: XTS transport, lobby, tournaments, leaderboards, presence, ranked balance, network replication, host-gated round end, ownership bits, version-mismatch strings, zlib 1.1.3 -> 1.2.3 | Online play | online | not ported (decision 2026-10-20: not ported, online) | **online** | - |

Public TU1 fixes **not found** in the code: one player controlling both co-op
screens, the Plasma Rifle / hovercraft turret 10x fire rate (WP14),
achievements not unlocking, flag capture every 4 s, unarmed melee kills not
counted, team-kill points near a territory tower, a gender change keeping
the old voice, and ranked team auto-balance (online). The first two matter
offline; both would need an in-game repro first.

### Earlier notes (2026-10-19)

Superseded by the table above where they differ: on 2026-10-20 T4 turned out to be a +76 check, T6 no change at all, T7 health (15000) on list entry 14, T10 the flag weapon's "flag dropped" call (with R2 / U17), T11 an ability-bit gate (U15).

From the TU3 vs launch code diff (`docs/MAP_PACKS.md`). Ported so far: the
cover cooldown (`pdz_fix_tu_cover_cooldown`, on) and, behind `pdz_dlc_maps`,
the map pack plumbing, including the TU3 changes the pack maps turned out to
need in play: the larger sound bank table (and the soundbank loader, unloader
and idle check that walk it; without the idle check leaving a pack map hung),
the 8 prop template entries for the packs' doors and teleporters (Facility
door hiss), and the per-map multiplayer strings in slot 7 (area names under
the map). Not ported: the interactive music removal and the wavebank language
fallback change (deliberately kept as launch).

**Turret re-equip (WP14): not the Plasma Rifle 10x fix.** Launch's player
update re-equips GunType_Turret_Shell (59) when flag +7664 is set (by the
vehicle seat handler `825AE258`, occupant type 2 only) and gun type 16 when
+7665 is set (multiplayer mode code). TU3 keeps the type-16 block and moves
the Turret_Shell block into the base character update (`8253CEE0` -> T
`8254C5A0`, flag +6171), set for occupant type 2 or 42 and by new AI-side
setters. Players behave the same; type-42 occupants (likely bots) now get the
turret weapon too. The hardpoint turret update (`8257D618`) is unchanged.
Left out; candidate only if bots misbehave in hovercraft turrets.

**Correction (2026-10-20, tu3-base): U23 is not about turrets.** Item 59
is the revive kit (its constructor `825BDA88` loads
`aid_xml_gadgetparams_revivekit` for type 59; the "GunType_Turret_Shell"
name came from a reading of the type-name table that doesn't hold). The setter (`825BDC78`) is the revive
kit's use-complete handler: after a revive, the reviver (character type 2,
or 42 in TU3) gets +6171, and the character update then removes the used kit
unless it is in hand. So TU3 lets type-42 characters spend revive kits; it
has nothing to do with turrets or WP14. Turrets and bots: Rare's hardpoint turret fires only for a human gunner;
`src/mod/bot_turrets.cpp` lets bots fire it.

**Remote mine controller (`xml\guns\multimine\guncontrolmultimine.xml`).**
Every pack level package carries Title Update 3's copy (identical to the one
in TU3's own `1726605008.pkg`); launch has its own. They differ in 7 16-bit
entries at file offset +0x114 (record +154..: launch
`1 9 9 6 3 6 6 4 2 2 2`, TU3 `5 9 5 5 3 5 5 4 2 5 5`). Corrected 2026-10-20:
these are ammo amounts (clip size and pickup amounts by difficulty), not
animation states; see "Multiplayer bots and TU features", Remote mines. The packs don't need it: it is
there because each level package bundles the shared gun records its level
uses, and launch level packages do the same with launch's copy, so each map
uses its own package's record: pack maps get TU3's values, launch maps
launch's. Not applied globally; awaiting a decision.

Other candidate fixes (read only, not tested):

| # | Launch -> TU3 | Change | Likely fixes | Confidence | Needs TU data |
|---|---|---|---|---|---|
| T1 | `822F8000` -> `822FFDC8` (Datacore level script) | Null check on script field +2592 before `lwz r11,92(r11)` (launch reads it unchecked at `822F89A0` and near `822F8D64`) | A crash in Datacore | high (that it's a null check) | no |
| T2 | `82270030` / `822711A0` + new `82141260`, `82276D78` | On level load and restart, if the background is `aid_darkbackground_single_town`, swaps in 7 `aid_texture_replacement_township*` textures for both co-op views | Township texture bug (co-op) | high | yes (`1726605008.pkg`) |
| T3 | `821BC4D0`, `821BC7B0`, `821C08A8` -> `821C1A88`, `821C1D78`, `821C67A0` (AI tasks) | Null check on task field +76 before the multiplayer objective code (`8242C0C0` / `8242C1A8`) | A bot crash | medium | no |
| T4 | `821BF4B0` -> `821C5160` (AI) | Null check before using +296 | Same family as T3 | medium-low | no |
| T5 | `821766E8` -> `8217B2A0` | Null check while scanning a 4-entry (per-player?) table at `82D3D118` | Crash with an empty slot (fewer than 4 players) | medium | no |
| T6 | `823645C0` -> `8236C4C0` (Jonathan companion script, LV09 / LV12) | When state +312 == 1, queues a timed cover animation | Possibly the "Jonathan stuck seated" squad softlock | low-medium | no |
| T7 | `82303A20` -> `8230B818` (spawn groups s10 / s11) | One more spawned character gets the 15000 (150 m) setting its group mates get | One enemy with a short range / not engaging | low-medium | no |
| T8 | `8259B738` -> `825AAF50` (player damage) | Health read through a virtual getter (field moved) | Refactor or field fix, unclear | low | no |
| T9 | `825AD3C8` -> `825BCF28` (zipline) | Custom camera placement replaced by camera mode 6 | Zipline camera glitch | low | no |
| T10 | `82569708` -> `82578F00` (weapon destructor) | Stops the looping weapon sound for the right owner | Stuck looping gun sound | low | no |
| T11 | `8224B7C8` -> `82252B00` (gun state machine, rocket area) | Alt mode requires an owner flag check (0x1000000) | Weak candidate for the split-screen rocket zoom bug | low | no |

Not found: one player controlling both co-op screens, the sign-out-during-load
crash (possibly in the profile functions `824FCF20` / `824FD888` / `824FEB88`,
mixed with the DLC calls), the multiplayer float / out-of-bounds fixes (TU
data: collision props in `1726605008.pkg`). T1, T3, T4 and T5 would be small
hooks that only differ from launch when the pointer is 0. Full notes:
scratchpad `tu3_fixes_survey.md` (session bc33c665).

## Multiplayer bots and TU features (enhancement; read-only audit 2026-10-20)

Plans only: nothing here is built. Addresses are launch unless marked T
(Title Update 3). Read = instructions / data read; inferred = reasoned.

### Bot limit (MB)

| # | Finding | Evidence | Confirmed |
|---|---|---|---|
| MB1 | **The 15 is settings policy, not a table size.** BOT_COUNT (option 19) uses list 96 = [0..63]. Its setter clamps bots to min(bots, PLAYERS_MAX - humans), then min(bots, BOT_MAXCOUNT). Offline PLAYERS_MAX uses list 15 = [2..16] (lists 13 = [2..32] / 16 = [32] / 14 = [2..64] are for other connection types), and BOT_MAXCOUNT (option 76, settings +3676) is set from `sub_8246CBD0`, which returns 16 except on Live. 16 players - 1 human = 15 bots | router `823B1E94 li r3,96`, PLAYERS_MAX router `823B1F54`-`1F9C`; setter `823B0D10`-`823B0DA4` (2nd read); `8246CBD8 li r3,16` (2nd read); BASE_CONFIG 16 forces PLAYERS_MAX back to 16 (`823B0AD0`-`0AF0`) | read |
| MB2 | **Match start checks the same two fields.** A bot is added only while bots + humans < PLAYERS_MAX mirror (settings +4301) and bots < BOT_MAXCOUNT mirror (+4364), so the fields themselves must hold 32 / 31; a menu-only change isn't enough | `sub_82144A28` / `sub_82144A88`; mirrors bound in `82457FE0` | read |
| MB3 | **Player tables are already >= 32**: setup slots 64 (`823B5248 cmpwi r29,64`), bot manager records 64 (`82144174`), participant records 128 x 4528 bytes (`82446F58 li r11,128`, 2nd read), participant ids 128, the MP mode object's arrays 128 plus growable vectors, bot registration a growable vector. Team counters and local-player tables are 4 (as needed). No separate offline cap of 16 exists beyond MB1. Not located (to check at run time): scoreboard rows, kill feed, radar | listed addresses | read (partial scan of 16 / 32 compares) |
| MB4 | **Pools hold 32 characters**: character list 150, AI plan nodes 350 (~124 needed for 31 bots, inferred), de-rez slots 64, particles 200 x 64 (purge / drop when full), bullet holes 50 (round-robin), audio clusters 180 (logs and drops a sound when full). Ragdolls, AI tasks, animation and projectiles have no fixed pool (per character / heap). Heaps are the same in SP and MP (LevelDynamicCPU 46 MB, LevelCPU 58 MB). TU3 changes none of these limits (its lists, clamp, BOT_MAXCOUNT 16 and checks are the same) | `sub_82CA11A8`, `sub_82141DE8 li r4,350`, `sub_825E69A0`, `sub_8260A910`, `sub_8222C5C0`, `sub_821F5188 li r4,180`, heap table `82D27CC0` | read; fill levels need a run-time check |
| MB5 | **Latent bug: the perception / noise slot table has no overflow check.** 170 slots of 80 bytes (`82E5C80C`..`82E5FD2C`), shared by characters, explosions (3 each), cameras, spiderbots and the player HUD. When none is free `sub_82184D58` uses index -1 (`82184DF4 mr r10,r28`, r28 = -1) and writes flags 80 bytes before the table. Never seen with 16 players; more characters and explosions make it likelier | `82184DCC`-`82184E48` (2nd read) | bug (latent) |
| MB6 | **Spawn points are AI pads** (`aid_aipads_multi_<map>`, type 7; group 0 free-for-all, 1-4 teams, 6 a 4-pad spawn room). Free-for-all picks a random pad with nobody within 20 m (`sub_82165638`), else any pad; teams pick any pad of the group (`sub_82165510`), else the whole list. No occupancy check: with too few pads characters spawn near enemies or stack; it never fails. No per-map player count exists in the data. TU3's choosers are the same | pad pool 7000 (`8215D918`) | read |
| MB7 | **Cost, measured with you + 15 bots** (Combat Simulator, 119 s of play, `frame_pacing_log`): 60 fps held (20 frames over 18 ms); host GPU 6.0 ms median (9.3 p99) of 16.7; GPU-command thread busy 3.7 ms (5.6 p99), ~13 ms of slack per frame in the game's own pre-swap wait; 3,500 draws per frame (5,700 p99), 44-46 resolves, no memexport, no shader compile stalls. 31 bots (inferred): draws and GPU time grow with the characters on screen (roughly 1.5-2x in a crowded view), which still fits at `resolution_scale` 2; the game thread's own AI cost isn't measured by any row yet | frame_pacing.csv from log 140 | measured / inferred |
| MB8 | **De-rez readback under mass deaths.** The game grants one effect slot per DeResThread cycle (gate 82E3B79C; slots ~33 ms apart in the logs), so at most one snapshot readback happens every ~2 frames however many bots die together. Each one waits on the GPU for at most the frame's queued work (6-9 ms measured) on the GPU-command thread, inside its ~13 ms slack. No limit or batching needed; a mass death spreads its effects over n x ~33 ms (whether a denied request retries is inferred: check with `pdz_fx_log`) | `sub_825E69A0`, logs 140 / 141 | read / inferred |

Spawn pads per map (free-for-all / teams 1-4):

| Map | Id | Pads | FFA | Team 1 / 2 / 3 / 4 |
|---|---|---|---|---|
| Subway | 0x00 | 197 | 99 | 15 / 15 / 32 / 32 |
| Chinatown | 0x04 | 225 | 93 | 32 / 32 / 32 / 32 |
| Desert | 0x05 | 228 | 98 | 32 / 30 / 32 / 32 |
| Temple | 0x06 | 116 | 52 | 15 / 15 / 15 / 15 |
| Africa | 0x07 | 181 | 67 | 15 / 31 / 32 / 32 |
| Tower (small) | 0x08 | 174 | 92 | 16 / 24 / 24 / 14 |
| Trench | 0x0C | 142 | 67 | 32 / 39 / 0 / 0 |
| Rooftops | 0x0D | 172 | 74 | 32 / 28 / 0 / 0 |
| Plaza | 0x0F | 149 | 48 | 32 / 32 / 0 / 0 |
| GasPlant | 0x10 | 162 | 74 | 26 / 25 / 1 / 0 |
| Facility | 0x21 | 27 | 23 | 0 / 0 / 0 / 0 |
| Ruin | 0x23 | 16 | 12 | 0 / 0 / 0 / 0 |

Facility and Ruin (and teams 3-4 on the pack maps) are small for 32: spawns
there will often land near enemies.

**Proposal: `pdz_mp_max_bots`** (enhancement, default 15 = original, up to 31; offline only):
1. PLAYERS_MAX: offline, answer [2..(N + 1)] (up to 32) from the four list readers (`sub_823B2C28` / `2D08` / `2D90` / `31A0`, already hooked in `dlc_maps.cpp` for U22), so the validator and next / previous keep values above 16.
2. BASE_CONFIG: skip its force-to-16 (mid-function hook before `823B0AE8`, jump to `823B0AF4`) while N > 15.
3. BOT_MAXCOUNT: `sub_8246CBD0` returns N on the non-Live branch; set option 76 through the setter when setup opens so the mirror (+4364) follows.
4. Keep bots + players <= 32 (the existing clamp does it); split screen gets 32 - players.
5. MB5 guard: in `sub_82184D58`, when no slot is free, skip the registration (TU-style midasm hook before `82184DF4`) instead of writing before the table (a fix in its own right, on by default under game fixes).
6. Test: 31 bots on a large map (Desert / Chinatown) and Ruin, 4-player split screen with 28 bots, `frame_pacing_log` + heap and perception slot counts (a small log), scoreboard / kill feed / radar with 32.

### TU bots (U23, U24, U27) port plan (TB)

| Feature | What TU3 does | Port route | Effort |
|---|---|---|---|
| Personalities | One value per bot (0 Vanilla, 1 Ghost, 2 Judge, 3 Bully, 4 Venge, 5 Feud, 6 Prey, 7 Random, 8 empty slot) in a new bot field (T +14188); Random = rand() % 7 in the bot constructor (`82529240` -> T `82537AC8`) | host tables (the bot class grew 48 bytes in TU3) + hooks on launch functions | medium-large |
| Ghost | The U24 "AI update range / team flag" change is really this (BUG_AUDIT correction): T `8252C464`-`8252C578` sets character flag +64 0x4 while a Ghost bot hasn't fired for 1 s (checks +14188 == 1, 2nd read); the target filter (`825291A8` -> T `825379B0`) accepts a flagged target only with probability (1 - d / 40 m)^2 | midasm hook before `8251DCFC`; `REX_HOOK_RAW(sub_825291A8)` (reached as a callback) | small-medium |
| Judge / Bully / Venge / Feud / Prey | One new function, T `82538728` (726 instructions, called from the bot update T `82538490`); everything it calls exists unchanged in launch | hand port as host code from TU3's recompiled C++; hook before `82529B64` | medium-large |
| Bot UI | TU `options.xml` / `names.xml` can't be loaded on launch (its option ids above 50 are launch + 1, its list-type enum gained 4 values); two TU scenes need TU-only XUI classes. Launch already has the lobby "TYPE" button (`DarkXuiBotPersonality`); personality names are TU strings already loaded by `pdz_dlc_maps` (slot 9) | a default-personality row like the MP Music row; hook the lobby label / cycle (`823BF6D0` / `823BF650`); the 12 icons through the title update allowlist | medium |
| Objective reactions (R3) | T `82146F60` (24 instructions, re-plan) and T `821C1420` (22, clear task targets) | host rewrites called from `8244AC68`, `8244AE60`, `8240FAE8`, `821BC230`; test first whether launch bots ignore a taken / dropped flag | small-medium |
| DarkOps bots | Keeping the bot manager is one hook (`82335398`-`823353BC`), but the rest is new: script id 38, 4 per-team classes, ~72 new functions (~4.8k instructions) in the bot manager, 36 new and 32 changed bot-task functions, and the participant refactor (189 changed, 57 new MP functions) | not portable piecemeal | very large |
| U23 turret re-equip | the turret-shell re-equip moved into the base character update, also for occupant type 42 | host flag, seat handler hook (`825AE258`), post-hook on `8253CEE0` | small-medium (only if bots mount turrets) |
| U27 data | two Chinatown route points (+0x38: 2 -> 3, 3 -> 4), Temple sandbag (+0x84 0 -> 1), eight explosion volumes (+4: ids 143 / 144) | 11 names on the title update allowlist; the volume ids are most likely online sync only | small |

Dependencies: none on the online code (U29) for Deathmatch personalities;
the participant refactor is what DarkOps bots need. U25 shifted character
vtable slots from +336 up by 4 (TU = launch + 4) and the settings block grew
112 bytes (launch's per-slot "no bot" value is 1, TU3's 8), so TU fields go
in host tables. Not verified: that flag 0x4 hides a bot from human players
too, the lobby clone / kick handlers, how Venge learns its last killer.

**Proposal: `pdz_tu_bots`** (Rare's addition, default off until tested).
Order: (1) personality value + host tables, Ghost; (2) Judge / Bully /
Venge / Feud / Prey; (3) lobby TYPE button + icons; (4) U27 route points;
(5) R3 only if a test shows bots need it; (6) U23; DarkOps bots last, if
ever.

### Remote mines (U20, corrected)

The 7 changed values are the remote mine's **ammo amounts**, not animation
states (the earlier "+0x114" was a file offset; record +154 is the clip
size, +166..+172 the pickup amount by difficulty, multiplayer last; readers
`822327F0` / `sub_82575F60`, unchanged in TU3). Ammo row 12 (`824E80B0`:
type 12 -> offset 72) is the mine's: column 0 the most spare mines carried,
columns 1-5 the spares every character starts with. Launch: clip 1, 4 free
spares at spawn on every difficulty (1 + 4 = 5). TU3: clip 5, no spares (all
5 in the clip, like the grenade and flashbang rows, which are already 0);
pickups give 5 (multiplayer was 2, Agent 6). **Verdict: a consistency /
design change, not an exploit fix.** **Our build is probably inconsistent
today**: pack maps load TU3's mine record but launch's ammo row, so likely 5
+ 4 = 9 mines there (HUD check on Plaza: 5|4 = mixed; a launch map reads
1|4). Proposal `pdz_fix_tu_mines`: TU3's record (exact-name allowlist entry,
`aid_xml_guns_multimine`) **and** TU3's ammo row (zero bytes +72..+77 of the
startup ammo record `[[82D6B2E0]+36]` when they are `04 x6`; that record is
registered before our title update load). Both halves together (one alone
gives 9 or 1). Default: off as a design change, unless the HUD check shows
the mixed 9 on pack maps, in which case on (TU3 everywhere) or forcing
launch's record on pack maps are the two consistent choices.

### Guided rocket in split screen (U15): test steps

Current build, no fix (it isn't built). Two controllers.
1. Multiplayer, split screen, 2 players. Combat (Killcount). A map with
   vehicles (Vehicles option on) where JetPacs and hovercraft appear, and a
   weapon set with the Rocket Launcher (Customize weapons if needed).
2. Control run: player 1 on foot, Rocket Launcher, switch to guided mode
   (B), fire. Note what both screens do (player 1 sees the rocket camera,
   player 2's view should not change) and that player 1 regains control
   when the rocket ends.
3. JetPac run: player 1 on a JetPac, guided mode selected, fire. Watch
   player 2's screen (does it zoom / switch / show the rocket?) and player
   1's controls during and after (move, jump, fire, switch modes).
4. Hovercraft run: player 1 drives a hovercraft, then the same as 3. Also
   try it as the gunner if there is a second seat.
5. Repeat 3 once with player 2 as the shooter.
Report: anything that happens to the other player's screen, any lost
control or stuck state for the shooter. If 3 / 4 misbehave and 2 doesn't,
the TU3 fix (no guided mode while riding: ability bit 24 in the vehicle
lock masks + the gate in `sub_8224B7C8`) fits and can be built.

### Cut multiplayer features (quick look)

| Feature | What exists | Verdict |
|---|---|---|
| Plant the Flag (Deathmatch scenario 3) | announcer case and FLAG_PLANTED cue (`sub_82335C90`, 2nd read), gameplay `sub_8243EE20` (986 instructions), points limit options, stats, preset XML | **cut, substantial code**; hidden only because the scenario list is [0, 1, 2, 4] |
| Protection (Deathmatch scenario 5) | manager at mode object +1016 ticked every frame (`sub_82440500`), terminal-hack logic and announcer lines (`sub_824414A8`, `824416A8`, `82441BB0`), preset, bot script name, stats | **cut, substantial code**; needs terminal props tagged for it in the maps (not checked) |
| DataDyne KillTV (spectating) | a GAME MODE menu button (`DarkXuiTV`, "DATADYNE KILLTV") whose press handler returns 0; TV scenes exist but their classes aren't in the exe; enabled only when signed in to Live | **frontend shell only** |
| Replays / recording / upload | nothing | **absent** |
| Tournaments (Live) | a button and six scenes; no code behind the controls | **stub** |
| Other | "Challenges" level type and prop masks, placeholder scenes, unused demo quick-launch button classes, debug tick names | **stubs** |

The cut scenarios' presets are loaded at startup with the others
(`sub_823B3BC8`). Trying one would mean adding its value to the scenario
list (the same reader hooks) and probably giving it a display name; Plant
the Flag and Protection likely need map props for their objectives.

## Ragdolls and death physics (audit 2026-10-19)

Question: are the floaty, "Thunderbirds puppet" ragdolls (also on the 360)
Rare's intent or a bug? Read only; ragdoll settings at their originals
(`pdz_ragdoll_inertia_scale` 1, damping -1, blend cap 0).

| # | Finding | Evidence | Verdict | Confirmed | Impact |
|---|---|---|---|---|---|
| RD1 | **Units are consistent: the Havok world is in metres, the game in centimetres.** Gravity (0, -9.8, 0) is therefore 9.8 m/s², not 100x too weak | World setup `sub_824DB530`: broadphase = level bounds widened by 20 / 100 (metre margins); trigger phantoms built from game positions x 0.01 (`824DB95C`-`824DB9BC`). Ragdoll: death hand-over puts the bodies at the character's position x 0.01 (`825DA610`-`825DA648`); body positions go back to game space x 100 for the limb line-of-sight check (`825DADCC`-`825DAE0C`, `sub_82252798`). Ragdoll asset bodies are placed in metres (feet y 0.061, knees 0.35, hips 0.82, thorax 1.45, head 1.70). The animation drive (`sub_82A4D660`) uses Havok's keyframe-controller defaults unchanged, all in metres (`sub_825D82F8`: hierarchy gain 0.17, velocity gain 0.6, position gain 0.05, max 1.4 m/s / 1.8 rad/s, snap 0.1 / 0.3 / 0.3 / 0.03 / 0.1) | fine | read | none |
| RD2 | **Inertia: every ragdoll body gets an isotropic inertia of 10 x its largest principal moment** (raised for stability). The bodies use Havok sphere motions (vtables `820399E0` / `82039A58`), which keep one inverse inertia at motion +196. The builder computes the body's inertia from its capsule and mass (`sub_825D6E70`, mass from the asset +88) and creates the body with it (setInertia, vtable +48 `sub_829CDB48`: inverse = 1 / the largest moment), reads the **inverse** back (vtable +56 `sub_829CDBB8`), takes the smallest diagonal entry (`fsel` pair at `825D8EAC`-`825D8EE0`), multiplies by 0.1 (`825D8EE4`) and sets it as the **inverse** (`sub_829B0790` -> vtable +52 `sub_829CDB90`, setInertiaInvLocal: the smallest diagonal entry): inertia = 10 x the largest moment on every axis | Thigh (r 8.4 cm, core 28.5 cm, 3 kg): computed ~0.011 (axial) / ~0.056 (across) kg m², game inertia ~0.56 on all axes. Log 120 (`pdz_ragdoll_log`, per body): computed 0.03039 / 0.00670 / 0.03039 -> inverse 3.29 = 1 / (10 x 0.03039); hand 0.00229 / 0.00118 / 0.00229 -> 43.66 = 1 / (10 x 0.00229). Raising inertia (here 10x the largest, spherical) is the usual Havok ragdoll stabilisation. **The audit first read the matrix as the inertia itself (so as 0.1 x the smallest moment) and called it a likely bug; a fix built on that (`pdz_fix_ragdoll_inertia`, 2026-10-19) wrote natural moments into the inverse setter, giving inertia ~150 (~490x the game's) and visibly worse, floatier ragdolls; it was removed** | deliberate | read + test | none |
| RD3 | **Death blend: a character that dies during a death animation is handed to physics over 2.5 x that animation's length** (`sub_825DA158`: anim ids 40-73 -> `sub_821E2978` = clip duration / rate, x 2.5 = float `0x40200000` at `8208E1BC`; otherwise 0.5 s). In mode 1 the body goes fully physical at weight 0.7, i.e. after 1.75 x the animation's length | Every other switch to ragdoll uses 0.1, 0.5 or 1 s (`8251F754`, `825AB444`, `825D99E8`, `825DAF08`, `825DA3CC`); `sub_825DC560` stores rate = 1 / seconds. The 2.5 is a literal, not a data value or a frames-to-seconds conversion, and the path deliberately checks for death animations and scales by their length: designed so the authored death plays out and physics takes over gently. The length (often 7-16 s) is what lets a falling body drift down half-animated | deliberate (generous tuning) | read | medium |
| RD4 | Masses: 15 bodies, 43 kg total (thorax / belly 5, chest / pelvis 4, thighs / upper arms / head 3, shins / forearms 2, feet 1); friction 1.0 (feet 0.44 / 0.48), restitution 0.3 | Asset records (`1262566553.pkg`, `aid_scenegraph_ragdolls_jack`, 96 bytes each, mass at +88). Light for a 1.7 m body but in proportion; total mass doesn't change how fast a body falls | deliberate | read | none |
| RD5 | Damping: Havok defaults (linear 0, angular 0.05) from `sub_829B2218`, not set by the game | Builder never writes the cinfo damping fields | deliberate (defaults) | read | low |
| RD6 | Joints: knees and elbows are limited hinges, the rest Havok ragdoll (cone / twist) constraints, from the asset's packfiles; their limits and joint friction weren't decoded | `hkLimitedHingeConstraintData` / `hkRagdollConstraintData` in the asset | unknown | - | - |
| RD7 | Timestep: real frame time in 1/60 s substeps (`sub_824DBB78` -> `sub_829B61D0`), so the 60 FPS patch doesn't change the physics; single player and multiplayer use the same builder and asset (spiderbot has its own) | Earlier read; no mode check in the builder | fine | read | none |

Conclusion: **the ragdolls are as Rare designed them.** Units and gravity
are correct (RD1: no 100x fall bug), the inertia is deliberately raised for
stability (RD2: 10x the largest moment, not reduced), the long death blend is
deliberate tuning (RD3), and masses and damping are sensible defaults
(RD4, RD5). The floaty look is the death blend keeping a falling body partly
animation-driven, plus heavy, uniform limb inertia. The enhancements
(`pdz_ragdoll_*`, off by default) stay available for taste:
`pdz_ragdoll_death_blend_max` shortens the blend; `pdz_ragdoll_inertia_scale`
scales the inverse inertia, so it divides the inertia (10 = each part's
largest natural moment, lighter).

### Fix test (2026-10-19, withdrawn)

`pdz_fix_ragdoll_inertia` (game fixes, default off) kept each body's computed
tensor, clipped Havok-style, and passed it to `sub_829B0790`. Because that
setter takes the **inverse** inertia, the bodies got inertia ~1 / (smallest
moment), about 490x the game's: in game the ragdolls were much worse, floating
as if gravity were lower. The per-body log exposed the mistake (the game's
value is 1 / (10 x the largest computed moment)). The fix, its setting and
its F11 entry were removed; `pdz_ragdoll_log` keeps the per-body inertia
(computed, and the inverse / inertia set) and the pelvis fall trace (height
and vertical speed every 50 ms for a second after each switch to ragdoll, and
the time of the first 1 m drop). In that test no trace reached a 1 m drop:
deaths from standing leave the pelvis about 1 m above the floor, and while the
death blend still drives the body its vertical speed stayed under 1.3 m/s, so
the trace doesn't measure free fall; a fall off a high ledge would.

## Suggested diagnostics (all default off; not implemented yet)

| Log | Settles |
|---|---|
| `pdz_look_log` `style N` on each control scheme (exists) | whether Standard is the "Halo" model (Controls) |
| cover scan log: scan start, restart reason, completion, duration, player speed, prompt shown / dropped | CV1 / FR1 |
| `pdz_weapon_log`: per shot gun type, spread used vs cap, pellets, hit object / part / distance, damage (partly built as `pdz_shot_log`: gun, spread, origin, bend, hit box, path; not damage or pellets) | WP4, WP7, WP8, WP10, WP15, WP16 |
| AI cover preconditions: player +0xB4 null rate, free plan nodes, attack tokens | AI3, AI4, AI6 |
| noise slot per shot and the AI that hears it | WP9 |
| spawn log: `sub_82142828` template, result, AI clock, spawns per minute | LS1, LS3 |
| `test\motionparamtable` field reads | TD2 |
