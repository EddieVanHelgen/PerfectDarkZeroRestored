# Settings

Two hand-authored, checked-in config files that the `perfectdarkzerorecomp`
binary loads automatically at startup, plus a downloaded controller mapping
database and this README.

| File | Covers |
|---|---|
| [`hardware.toml`](hardware.toml) | Renderer/backend, window, vsync, resolution, debug output, experimental gameplay patches |
| [`mapping.toml`](mapping.toml) | Input backend selection (gamepad/keyboard+mouse) |
| [`gamecontrollerdb.txt`](gamecontrollerdb.txt) | SDL gamepad mappings for controllers it doesn't already recognize - not hand-authored, see below |

Split in two on purpose: rendering/perf tweaks and input tweaks are separate
concerns you'll want to touch independently, and both stay tiny because
they only list the cvars this project actually cares about, not the full
~80-flag surface the SDK exposes.

**Format: flat `key = value` only, no `[Table]` headers.** Confirmed via
`rex::cvar::SerializeToTOML()`, which itself dumps a flat list - the loader
reads root-level keys, it does not recurse into nested tables. A key put
under a `[Section]` header is silently ignored (found the hard way: values
under `[Display]`/`[GPU]` headers never took effect until the headers were
removed).

## How the binary consumes them

`ReXApp` (the SDK base class) already auto-loads a single `<exe_dir>/<name>.toml`
config file via `rex::cvar::LoadConfig()`. `PerfectdarkzerorecompApp::
OnConfigurePaths()` in [`src/perfectdarkzerorecomp_app.h`](../src/perfectdarkzerorecomp_app.h)
redirects that single slot to `settings/hardware.toml`, then loads both files
a **second time** in `OnPostSetup()`. Both paths are resolved by walking up
from the exe's folder until an `assets/` directory is found, so it works from
any build preset's output directory.

The second pass exists because not every cvar is registered when
`OnConfigurePaths` runs: window/fullscreen/resolution/monitor (`rex::ui`)
are available immediately, but GPU-backend cvars like `vsync`,
`resolution_scale`, `d3d12_debug` (`rex::graphics`) only register once the
runtime/GPU backend spins up, which happens later. `LoadConfig()` is a
one-shot read, not a live watch, so without the second pass those keys would
be silently dropped (confirmed via `rex::cvar::GetFlagSource()` returning
`kDefault` for them after the first pass, `kConfig` after the second).
Re-applying an already-set value on the second pass is a harmless no-op.

## Why `game_data_root` isn't a key in `hardware.toml`

Unlike every other cvar here, `game_data_root` is read by the SDK (`ReXApp::
SetupEnvironment()`) *before* any config file loads at all - so a
`game_data_root = "..."` line in `hardware.toml` would silently have no
effect, the same one-shot-too-early problem as the two-pass cvars above but
with no later pass to catch it. Instead, `OnConfigurePaths()` defaults
`paths.game_data_root` directly to `<repo_root>/assets` in code whenever
`--game_data_root`/`REX_GAME_DATA_ROOT` didn't already set it, so
`PDZRestored.exe` with no flags at all just works from this repo
layout. `--game_data_root <path>` still overrides it for a game dump kept
elsewhere.

Precedence (highest wins), same as any other cvar:

```
--flag=value (CLI)  >  REX_FLAG=value (env var)  >  settings/*.toml  >  compiled-in default
```

So a `--vsync=false` on the command line, or `REX_VSYNC=false` in the
environment, always overrides whatever is in `hardware.toml`.

## Caveat: the in-game Settings overlay writes to `hardware.toml`

The SDK's built-in Settings overlay's "Save to config" button writes *every*
modified cvar into the one path `OnConfigurePaths` redirected
(`hardware.toml`), including anything you changed at runtime through the
overlay. If you experiment there, expect `git diff` to show more than the
hand-picked keys below - `git checkout -- settings/hardware.toml` reverts it
to the committed defaults.

## Caveat: engine hotkeys aren't in `mapping.toml`

`mapping.toml` only covers static-flag input cvars (table below). The
console/debug/achievements overlay toggle keys ("Keybinds" category) are
registered by the SDK *after* these files load (once the window and
overlays exist), so a key added to `mapping.toml` for one of those would
silently have no effect. Rebind those from the in-game Settings overlay
instead - that persists through the same save mechanism described above.

## Keyboard, mouse, and gamepad all work at once

The SDK always merges every input device assigned to a guest player rather
than picking one active source (`rex::input::MergeInto` - buttons OR'd,
sticks/triggers take whichever device pushed further), and the keyboard as a
gamepad ("MnK") driver marks itself a synthetic device that's always folded
into player one alongside any physical pad, never competing with it for a
slot. So there's no "backend" to choose between keyboard and gamepad the way
`input_backend` chooses between `sdl`/`xinput` - it's `mnk_mode`/`mnk_mouse`
turning the keyboard/mouse side on or off, independent of whatever pad is
also plugged in.

Per-action keybinds (`keybind_a`, `keybind_lstick_up`, ...) already ship with
sensible WASD-style defaults and aren't repeated in `mapping.toml` -
add any of them the same way as everything else here if you want to rebind
one. The full list isn't in the headers vendored under `rexglue/win-amd64`
(source-only, defined in the SDK's `mnk_input_driver.cpp`) - the easiest way
to see every current default is `rex::cvar::SerializeToTOML()`'s dump, or the
in-game Settings overlay's control rebinding screen.

## `hardware.toml` reference

The "Default" column shows the value committed in `hardware.toml` (what the
build actually ships with), not the SDK's compiled-in default.

| Key | Type | Default | Effect |
|---|---|---|---|
| `gpu_plugin` | string | `"xenos"` | GPU emulation plugin to load. Set here rather than on the command line now - see root [README](../README.md#run). A `--gpu_plugin` flag would still override this file if you ever needed a different plugin. |
| `graphics_backend` | string | `"any"` | Graphics API backend: `"any"` (D3D12 first when both are compiled in), `"d3d12"`, or `"vulkan"`. Project-defined cvar (not from the SDK), wired up in `OnPreSetup()` in [`src/perfectdarkzerorecomp_app.h`](../src/perfectdarkzerorecomp_app.h) - it loads the plugin itself with the requested backend before the SDK's own auto-load (which only ever requests `"any"`) runs. Falls back to automatic selection with a warning if the requested backend isn't compiled into `rexgpu-xenos`. |
| `vsync` | bool | `true` | Vertical sync. Also paces the guest vblank (60 Hz) - without it the game runs as fast as it can. |
| `host_vblank_pacing` | bool | `true` | **Recomp fix, on by default.** Even frame pacing: every game frame is presented once, at the guest vblank after it was finished, so each one stays on screen equally long. On a fixed refresh rate that's a multiple of 60 Hz (60/120/240) the guest vblank is phase-locked to the display's; on G-Sync/FreeSync it stays evenly timed and the display follows. Other refresh rates (e.g. 144 Hz) fall back to a steady timer. Needs `vsync = true`. Restart after changing it. |
| `frame_pacing_log` | bool | `false` | Diagnostic: writes guest vblank/swap and host present/vblank timestamps to `logs/frame_pacing.csv` next to the exe, plus per-frame input latency rows (`L` at present, `D` once on screen, from DXGI frame statistics) and an `input->screen` summary in the main log about every 10 s. Per guest frame it also writes where the time went: `K` (command processor wall / idle / WAIT_REG_MEM / waiting for the host GPU, draws, resolves), `k` (host submissions, draws skipped while a shader compiles, memexport draws) and `T` (host GPU busy time, from D3D12 timestamp queries around each submission). `python tools/frame_cost.py` summarizes them per second. |
| `bind_latency_ab` | key | `F8` | In game: swaps `host_vblank_late_latch_ms`, `input_poll_on_read` and `gpu_max_frames_in_flight` between their current values and the originals (`0`, `false`, `3`) (A/B). Don't press "Save to config" while it's on the originals, or those get saved. |
| `host_vblank_late_latch_ms` | float | `3` (on by default) | With `host_vblank_pacing`: a frame that arrives up to this many ms after a guest vblank is presented for that vblank instead of the next one. PDZ swaps on the vblank, so its frames arrive about 0.5 ms after it and would otherwise wait a whole frame. `3` saves 16.7 ms at 60 fps. `0` = off (original). |
| `input_poll_on_read` | bool | `true` (on by default; `false` = original) | SDL input: read the controller (`SDL_UpdateJoysticks`) every time the game asks for its state, on the game's thread, instead of returning the state from the last UI-thread event pump, which is up to a frame old. |
| `gpu_max_frames_in_flight` | int | `3` | Guest frames the host GPU may be behind the command processor (1-3). Lower stops the game running ahead of the screen when the host GPU is the bottleneck, at some throughput cost. No effect while the GPU keeps up. |
| `present_max_frame_latency` | int | `0` | D3D12: presents that may be queued ahead of the display (1-3), using a waitable swap chain. `0` leaves DXGI's default (3). Restart after changing it. |
| `resolution_scale` | int | `2` | Internal render-target supersampling, independent of window/guest resolution. |
| `async_shader_compilation` | bool | `true` | Compile shaders on a background thread instead of blocking the render thread - reduces hitches when new shaders are first seen. |
| `native_2x_msaa` | bool | `true` | Native 2x MSAA on the emulated render targets. |
| `anisotropic_override` | int | `1` | Forces anisotropic texture filtering to this level (e.g. `16`); `0` leaves the game's own setting alone. |
| `window_width` / `window_height` | int | `1920` / `1080` | Host window size. |
| `fullscreen` | bool | `true` | Host window fullscreen. |
| `monitor` | int | `0` | Host monitor index for fullscreen. |
| `resolution` | string | `"1920x1080"` | Guest ("TV") video mode reported to the game - affects the game's own UI scale/aspect logic, separate from the host window size above. |
| `present_letterbox` | bool | `true` | Letterbox instead of stretch when window and guest aspect ratios differ. |
| `d3d12_debug` | bool | `false` | D3D12 debug layer. Leave off - noticeably slower. |
| `pdz_fps60_unlock` | bool | `true` | Port of Xenia-canary's Perfect Dark Zero "60 FPS" patch. Applied through a C++ function override and can be toggled at runtime. |
| `pdz_aspect_ratio_16_9` | bool | `false` | Port of Xenia-canary's "Aspect Ratio" patch. Despite the name it's for ultrawide screens: the game is already 16:9, and this replaces its widescreen aspect constant (1.778 at `820EC158`) with 2.389, so the view is built for a 2.39:1 screen. On a 16:9 display it squashes the picture horizontally. Applied while loading the game image; restart after changing it. |
| `pdz_fov_scale` | float | `1` | Enhancement, off by default (`1` = original). On-foot field of view (first and third person) as a multiple of the original: 58 degrees 4:3 horizontal (45 vertical), about 73 across at 16:9. `1.1` is about 79 across, `1.2` about 86. Scopes keep their magnification relative to it; vehicles, turrets and cutscenes are unchanged. `1` = original. |
| `pdz_look_tuning` | bool | `true` | Master switch for the look options below. Off = original look behaviour, values kept. On by default for `pdz_fastlook_zoom_brake`; the other look options are at their originals. **F6** (`bind_look_ab`) flips it in game and shows what's active. |
| `pdz_turn_speed_scale` | float | `1` | Right-stick turn speed on foot (yaw, pitch and the fast-look boost). See [Look tuning](../src/mod/README.md#look-tuning). |
| `pdz_pitch_speed_scale` | float | `1` | Vertical look speed only, on foot, multiplied on top of `pdz_turn_speed_scale`. Pitch otherwise shares yaw's base speed, curve and acceleration (but no fast look). |
| `pdz_turn_accel_scale` | float | `1` | How quickly turning speeds up and slows down on foot, including the ~1 s fast-look ramp. Higher = shorter ramp. |
| `pdz_stick_curve` | float | `0` | Right-stick response exponent. `0` keeps the game's (1.5 in PD style, rising to 2.84 at max sensitivity), `1` is linear. |
| `pdz_stick_deadzone` | float | `-1` | Right-stick deadzone per axis as a fraction of travel. `-1` keeps the game's (0.153, or 0.214 for a jittery stick). |
| `pdz_look_deadzone_scale` | float | `1` | Scales the look code's second, cross-shaped deadzone (yaw 3-10%, pitch 12.5-25% of the remaining travel). `0` turns it off. |
| `pdz_stick_radial` | bool | `false` | Right stick: round deadzone, and the response curve applied to the stick's overall deflection, so diagonals turn as fast as straight pushes. The original works per axis: a 45° push at the rim turns ~35% slower and can't fast-look. Pair with `pdz_look_deadzone_scale = 0`. |
| `pdz_fastlook_zoom_brake` | bool | `true` | **Look enhancement, on by default** (`false` = original; needs `pdz_look_tuning`). Zooming (left trigger) ends fast look, so the turn stops coasting past the target; works with Standard and Classic PD. The original glide is deliberate (Halo audit H10). Originally, zooming with the stick still at the rim keeps fast look on, and the turn slows at only the fast-look rate (~120°/s per second), overshooting for up to a second. |
| `pdz_fastlook_exit` | float | `0.95` | Stick deflection below which fast look ends (it starts at 0.98). Up to `0.98`: easing off the rim then brakes at the normal rate straight away. |
| `pdz_autoaim_tuning` | bool | `true` | Master switch for the auto-aim options below. Off = original auto-aim, values kept. On by default for `pdz_aim_still_lock`; the other options are at their originals. **F10** (`bind_aim_ab`) flips it in game. |
| `pdz_autoaim_enable` | bool | `true` | `false` turns the player's auto-aim off whatever the game's Auto Aim option says. `true` = original. |
| `pdz_autoaim_cone_scale` | float | `1` | Auto-aim lock radius (~3.8° on Agent/Secret Agent, ~3° on Perfect/Dark Agent) and target-selection margin. See [Auto-aim](../src/mod/README.md#auto-aim). |
| `pdz_autoaim_min_radius_cm` | float | `0` | Enhancement, off by default (e.g. `35`). Up close, widen the lock radius to cover at least this much of the target, in cm. The original covers ~13 cm at 2 m, ~33 cm at 5 m, ~66 cm at 10 m, so 35 only changes anything closer than ~5 m. `0` = original. |
| `pdz_autoaim_strength_scale` | float | `1` | How far a shot is bent towards a locked target's centre (the crosshair itself never moves). `1` = all the way (original), `0.5` = half way. |
| `pdz_aim_friction` | float | `0` | Enhancement, off by default (e.g. `0.35`; `0` = original). New, console-style slowdown: slows turning by this fraction while the crosshair is within `pdz_aim_friction_zone_deg` of a target's centre (or on its body, if that's bigger), fading out by twice that. The original game has no friction. |
| `pdz_aim_friction_zone_deg` | float | `3` | Size of the friction sticky zone in degrees, at least the target's body. Measured as the true angle between your aim and the target, so tiny distant targets still get a full-size zone. |
| `pdz_autoaim_ignore_difficulty` | bool | `false` | Enhancement, off by default (`false` = original). Auto-aim works the same on every difficulty, using the Agent / Secret Agent rules. The original gives Perfect and Dark Agent a weaker auto-aim record (~3° instead of ~3.8°) and switches auto-aim off while zoomed. |
| `pdz_aim_friction_toward` | float | `0.9` | How much of the friction applies while you turn toward the target, e.g. following it up a ladder or across the screen. The default eases it a little so you can keep up with moving targets; `0` = friction only slows you when you overshoot or turn away; `1` = the same both ways. |
| `pdz_aim_friction_near_m` | float | `2` | No friction on a target closer than this (metres); it fades back in up to `pdz_aim_friction_full_m`. Up close a body fills a wide angle and crosses the view fast, so friction there makes it hard to turn onto someone right next to you. `0` = friction at every distance. |
| `pdz_aim_friction_full_m` | float | `6` | Friction is at full strength from this distance (metres) out. |
| `pdz_aim_friction_sweep_start_dps` | float | `10` | Friction eases off once the target sweeps across your view faster than this (degrees per second, whether you or they are moving). Circle strafing at mid range is about 30; standing and aiming at someone is near 0. |
| `pdz_aim_friction_sweep_end_dps` | float | `30` | No friction while the target sweeps across your view this fast or faster. `1000` = never eases. |
| `pdz_aim_still_lock` | bool | `true` | **Aim enhancement, on by default** (`false` = original; needs `pdz_autoaim_tuning`). Auto-aim also bends shots while you hold still or track slowly; the original only bends them while you turn fast. Combat Arena A/B: tracked-shot hits 35% -> 59%. |
| `pdz_autoaim_log` | bool | `false` | Diagnostic: `pdz_aim:` lines once a second (target, lock, distance, target angular speed) and percentiles every 10 s. |
| `pdz_game_fixes` | bool | `true` | **Game fixes, on by default.** Master switch for Rare's Title Update 3 fixes (`pdz_fix_tu_cover_cooldown`, `_datacore_null`, `_bot_task_null`, `_base_slot_null`, `_lv10_health`, `_township`, `_flag_drop`, `_teleport`, `_mp_hitfix`, `_pack_markers`, `_pack_map_rules`; and `pdz_fix_derez_snapshot`, an emulation fix that brings back the death de-rez effect's cubes; `_township` and `_mp_hitfix` need the title update's data package and take their props at boot, so a restart applies them), `pdz_fix_halo_zoom_curve`, `pdz_fix_spread_cap_order`, `pdz_fix_cmp_noise` and the enemy AI fixes `pdz_fix_ai_semiauto_rhythm`, `_grenade_height`, `_dodge_facing`, `_dodge_clearance`, `_corner_speed`, `_calm_flag` and `_cover_min_distance` (confirmed bugs), all `true`. **F11** (`bind_game_fixes_ab`) flips it in game. See [Game fixes](../src/mod/README.md#game-fixes) and [`docs/BUG_AUDIT.md`](../docs/BUG_AUDIT.md). |
| `pdz_ai_cover_fixes` | bool | `true` | **Enemy cover fixes, on by default.** Master switch for `pdz_ai_cover_fix_grace`, `_peek_roll` and `_snapback`, all `true`. **F5** (`bind_ai_cover_ab`) flips it in game. |
| `pdz_controls_fixes` | bool | `false` | Parked, off by default: the player controls fixes (`pdz_fix_fastlook_*`, `pdz_fix_sensitivity_clamp`, `pdz_fix_spread_recovery_gate`), all `false`. **F2** (`bind_controls_ab`) flips it. |
| `pdz_mp_music` | bool | `true` | **Enhancement; its in-game value is Off by default.** Adds a MUSIC row under POINTS LIMIT on the multiplayer setup screens: Off / Random / one of 38 soundtrack pieces (the missions' music, menus, credits), played over the map's ambience in multiplayer matches only. A chosen track loops; Random picks another after each play-through. The choice is kept per local profile (`mp_music.txt` next to its saves). `false` = no Music row (original). Restart after changing it. See [Multiplayer music](../src/mod/README.md#multiplayer-music). |
| `pdz_ragdoll_inertia_scale` | float | `1` | Enhancement, off by default (`1` = original; e.g. `10`). Divides ragdoll limbs' resistance to spinning: it scales the inverse inertia the game sets. The game gives every body part 10x its largest natural moment on all axes (deliberate, for stability); `10` = the largest natural moment (lighter, freer limbs), `0.5` = twice the game's (stiffer). Applies to characters created after the change. See [Ragdolls](../src/mod/README.md#ragdolls). |
| `pdz_ragdoll_angular_damping` | float | `-1` | Enhancement, off by default (e.g. `0.4`). Ragdoll body parts' angular damping (original: Havok's 0.05). `-1` = original. |
| `pdz_ragdoll_linear_damping` | float | `-1` | Ragdoll body parts' linear damping (original 0; more = floatier). `-1` = original. |
| `pdz_ragdoll_death_blend_max` | float | `0` | Enhancement, off by default (`0` = original; e.g. `1.5`). Longest blend, in seconds, from a death animation into the ragdoll. The game uses 2.5x the animation's length (often 7-16 s) and only makes the body fully physical at 70% of the blend, so a body that dies mid-animation (feet swept out) floats down half-animated and spins when shot. Applies instantly. |
| `pdz_fx_log` | bool | `false` | Diagnostic (needs `log_level = "info"`): `pdz_fx:` lines for the de-rez death effect: each request, the effect slot and REZOUT sound, the body snapshot's resolve and GPU callback, how many non-black pixels the CPU found in the snapshot and how many particles that gave. Read-only. See [De-rez effect log](../src/mod/README.md#de-rez-effect-log). |
| `pdz_button_log` | bool | `false` | Diagnostic (needs `log_level = "info"`): `pdz_button:` lines for every press and release of A / B / X / Y, LB / RB and the triggers as the game read them (time in ms, frame, how long it was held), whether the game registered the press that frame, and for X (reload) and B (secondary fire) whether it started or why the gun refused it (gun busy and with what, clip full, no reserve, locked). Also flags a gap of more than 25 ms between pad polls. Read-only. See [Button log](../src/mod/README.md#button-log). |
| `pdz_ragdoll_log` | bool | `false` | Diagnostic: `pdz_ragdoll:` lines for every animation/ragdoll switch (mode, blend time, caller) and, while a ragdoll is still partly animation-driven, its blend weight ten times a second; each ragdoll body's computed inertia and the inertia set; and for a second after each switch to ragdoll the pelvis' height and vertical speed every 50 ms and when its first 1 m drop completed (free fall ~0.45 s). |
| `pdz_ai_state_log` | bool | `false` | Diagnostic: `pdz_ai:` lines once a second per enemy that is aware or within 50 m: setup type, aware / hurt / shot-at / provoked / no-cover flags, alert, target and distance, own and player pads, running tasks, and what its combat planner did since the last line (plans by kind, cover chooser give-ups, cover pads tried / passed / passed under 10 m / re-check failures, plan ends, fallbacks). Needs `log_level = "info"`. See [Enemy AI state log](../src/mod/README.md#enemy-ai-state-log). |
| `pdz_look_log` | bool | `false` | Diagnostic: logs the right-stick chain every frame (pad raw -> filtered, deadzone, centre, yaw / pitch deg/s, fast look, rotational assist turn and net rate, active assists) and sensitivity, peak turn rate and ramp time once a second (`pdz_look:` lines; needs `log_level = "info"`). Large log. |
| `user_language` | int | `1` | Game language: `1` English, `3` German, `4` French. SDK cvar, applied by a hook in [`src/game_patches.cpp`](../src/game_patches.cpp) because the SDK's `XGetLanguage` ignores it. Languages missing from `loc/` fall back to English. Restart after changing it. |

The source Xenia patch file also contains a "Bottomless Clip" patch. It is
not ported yet, so there is no corresponding cvar in this project.

## `mapping.toml` reference

| Key | Type | Default | Effect |
|---|---|---|---|
| `input_backend` | string | `"sdl"` | `""` is rejected by validation (logs a warning, falls back to default) - must be an explicit `"sdl"`, `"xinput"`, or `"nop"` (input disabled). |
| `hid_mappings_file` | string | `"gamecontrollerdb.txt"` | Extra SDL gamepad mappings loaded for controllers SDL doesn't already recognize, or `""` to skip. `gamecontrollerdb.txt` next to the exe is staged by `CMakeLists.txt` from [`gamecontrollerdb.txt`](gamecontrollerdb.txt) in this folder, sourced from [mdqinc/SDL_GameControllerDB](https://github.com/mdqinc/SDL_GameControllerDB) - re-copy that file from upstream any time to refresh it. |
| `guide_button` | bool | `true` | Whether the controller Guide/PS button is routed to the guest. |
| `mnk_mode` | bool | `true` | Enables keyboard-as-controller input, merged in alongside any physical gamepad (see below). |
| `mnk_mouse` | bool | `true` | Routes mouse movement to the right stick when `mnk_mode` is on. Off means the right stick only comes from the `keybind_rstick_*` keys. |
| `mnk_sensitivity` | double | `1.0` | Mouse sensitivity for the right stick, range `0.01`-`10.0`. |

Gamepad button layout itself (DualShock 4, DualSense, Xbox, Switch Pro, ...)
isn't a setting here - SDL auto-detects the controller and maps it to the
Xbox 360 layout the game expects.

## Adding more cvars

Any cvar declared with `REXCVAR_DECLARE` under
`rexglue/win-amd64/include/rex/**/flags.h` can be added the same way: pick
the file matching its concern (hardware vs. input), add `key = value`. If it
needs to apply before the window/overlays exist (true for everything
currently listed), it belongs in one of these two files loaded from
`OnConfigurePaths`; if it's a Keybinds-category hotkey, see the caveat above
instead.

`graphics_backend` isn't an SDK cvar - it's defined with
`REXCVAR_DEFINE_STRING` in [`src/game_cvars.cpp`](../src/game_cvars.cpp),
with its declaration in [`src/game_cvars.h`](../src/game_cvars.h). Project
cvars register at static-init time, so they load with the first config pass.
Define future project-specific toggles there, read them with
`REXCVAR_GET(name)`, and document them in this table.
