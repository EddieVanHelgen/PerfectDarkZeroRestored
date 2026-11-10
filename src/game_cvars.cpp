#include "game_cvars.h"

REXCVAR_DEFINE_STRING(graphics_backend, "any", "GPU",
                      "Graphics API backend: any, d3d12, vulkan, null (D3D12 without GPU emulation: the GPU plugin "
                      "services the command ring and its synchronisation but draws nothing; the native renderer, "
                      "which this forces on, is the only output; needs a rexgpu-xenos.dll with the null backend)")
    .allowed({"any", "d3d12", "vulkan", "null"});

REXCVAR_DEFINE_BOOL(pdz_fps60_unlock, false, "Gameplay",
                    "Unlock 60 FPS");

REXCVAR_DEFINE_BOOL(pdz_aspect_ratio_16_9, false, "Gameplay",
                    "Enable 16:9 aspect ratio");

REXCVAR_DEFINE_BOOL(dev_debug_runtime, false, "Debug",
                    "Enable runtime debug tools (stub sweep, missing function scan)");

REXCVAR_DEFINE_BOOL(pdz_tp_enable, false, "Gameplay",
                    "Third-person camera (toggle with bind_third_person)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(pdz_tp_offset_x, 0.0, "Gameplay",
                      "Third-person camera offset, camera-local X")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(pdz_tp_offset_y, 0.0, "Gameplay",
                      "Third-person camera offset, camera-local Y")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(pdz_tp_offset_z, 0.0, "Gameplay",
                      "Third-person camera offset, camera-local Z")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_tp_auto_fp_gadgets, true, "Gameplay",
                    "Third person: switch to first person while the Data Thief is in use")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_INT32(pdz_tp_zoom_mode, 2, "Gameplay",
                     "Third-person camera while zoomed (scopes, binoculars): "
                     "0 = stay behind, 1 = first person, 2 = slide forward along the view line")
    .range(0, 2)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_tp_binoculars_fp, false, "Gameplay",
                    "Third person: look through the binoculars in first person "
                    "instead of like scopes (pdz_tp_zoom_mode)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_look_tuning, true, "Gameplay",
                    "Master switch for the pdz_turn_*, pdz_pitch_*, pdz_stick_*, "
                    "pdz_look_deadzone_scale and pdz_fastlook_* options; off = original look "
                    "behaviour without touching their values. On by default for "
                    "pdz_fastlook_zoom_brake; the other options are at their originals. "
                    "bind_look_ab (F6) flips it")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(pdz_turn_speed_scale, 1.0, "Gameplay",
                      "Right-stick turn speed (yaw, pitch and the fast-look boost), on foot. "
                      "1 = original")
    .range(0.25, 4.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(pdz_pitch_speed_scale, 1.0, "Gameplay",
                      "Vertical look speed on foot, on top of pdz_turn_speed_scale. "
                      "1 = original")
    .range(0.25, 4.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(pdz_turn_accel_scale, 1.0, "Gameplay",
                      "How quickly turning speeds up and slows down, including the fast-look "
                      "ramp to full speed, on foot. 1 = original, higher = shorter ramp")
    .range(0.25, 50.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(pdz_stick_curve, 0.0, "Gameplay",
                      "Right-stick response exponent (1 = linear, higher = more precision near "
                      "the centre). 0 = original (1.5 PD style, ~2 Halo style)")
    .range(0.0, 4.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(pdz_stick_deadzone, -1.0, "Gameplay",
                      "Right-stick deadzone as a fraction of stick travel (0.153 is the original "
                      "for a quiet stick, 0.214 for a jittery one). -1 = original")
    .range(-1.0, 0.9)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(pdz_look_deadzone_scale, 1.0, "Gameplay",
                      "Scales the look code's second, cross-shaped deadzone (applied after the "
                      "stick deadzone). 1 = original, 0 = off")
    .range(0.0, 2.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_stick_radial, false, "Gameplay",
                    "Right stick: round deadzone and the response curve applied to the stick's "
                    "overall deflection, so diagonals turn as fast as straight pushes. "
                    "Pair with pdz_look_deadzone_scale = 0. false = original (per axis)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_fastlook_zoom_brake, true, "Gameplay",
                    "Look enhancement, on by default (needs pdz_look_tuning; F6 switches it): "
                    "zooming (left trigger) ends fast look, so the turn slows to the zoomed "
                    "speed at once instead of coasting past the target for up to a second. "
                    "The original glide is deliberate (Halo audit H10); false = original")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(pdz_look_fastlook_threshold, 1.0, "Gameplay",
                      "Fast look (the full-push turbo turn) engages from this right-stick deflection: the round "
                      "magnitude after the inner deadzone, 0.5-1. 1 = Rare's original, the default since 2 Nov: "
                      "the real cause of slow full-push turns was our centre-drift correction stopping a full "
                      "push at 0.989 (log 414), now fixed, so the rim reaches 1.0 on both sides. Below 1, a push "
                      "at or past this deflection is given to Rare's look code at full deflection (same "
                      "direction), so a stick that physically stops short still gets the turbo; nothing changes "
                      "while zoomed (needs pdz_look_tuning; docs/LOOK_SLOW_TURN.md)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_hook_profile, false, "Debug",
                    "Every 5 s, log the mod hooks that took the most host time (calls and ms per "
                    "second, inclusive of the game code they call). Diagnostic for frame drops")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_look_radial, false, "Gameplay",
                    "Look enhancement (ours, off by default; needs pdz_look_tuning): the right "
                    "stick is read as one direction, so diagonals turn as fast as straight left / "
                    "right, with one round deadzone (pdz_look_radial_deadzone) in place of the pad "
                    "filter's square one and the look records' per-axis ones, rescaled so there is "
                    "no jump at its edge. false = original")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(pdz_look_radial_deadzone, 0.08, "Gameplay",
                      "pdz_look_radial's round deadzone as a fraction of full deflection (0-0.9)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(pdz_look_axial_snap_deg, 10.0, "Gameplay",
                      "pdz_look_radial: within this many degrees of straight left / right / up / down "
                      "the stick counts as straight; from there to twice this angle it fades back to "
                      "the stick's own direction (0 = no snapping, max 20)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_look_snappy, false, "Gameplay",
                    "Look enhancement (ours, off by default; needs pdz_look_tuning): the view "
                    "starts and stops with the stick: the look records' acceleration and "
                    "deceleration are divided by pdz_look_inertia_scale. Top turn speeds and the "
                    "Fast Look ramp are unchanged. false = original")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(pdz_look_inertia_scale, 0.4, "Gameplay",
                      "pdz_look_snappy's inertia (0.1-1; 1 = original, 0.4 = 2.5 times quicker to "
                      "start and stop)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_look_fixed_curve, false, "Gameplay",
                    "Look enhancement (ours, off by default; needs pdz_look_tuning): the stick "
                    "curve exponent stays at the default sensitivity's value at every sensitivity "
                    "(the original steepens it with higher X sensitivity, LOOK_PIPELINE H7), so "
                    "sensitivity only scales speed. false = original")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(pdz_fastlook_exit, 0.95, "Gameplay",
                      "Stick deflection below which fast look ends (it starts at 0.98). Higher "
                      "= easing off the rim brakes sooner. 0.95 = original")
    .range(0.95, 0.98)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_autoaim_tuning, true, "Gameplay",
                    "Master switch for the pdz_autoaim_* and pdz_aim_friction options; off = "
                    "original auto-aim without touching their values. bind_aim_ab (F10) flips it")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_autoaim_enable, true, "Gameplay",
                    "false turns the player's auto-aim off regardless of the game's Auto Aim "
                    "option. true = original (the game's option decides)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(pdz_autoaim_cone_scale, 1.0, "Gameplay",
                      "Auto-aim lock radius and target-selection margin (~3.8 deg on Agent and "
                      "Secret Agent, ~3 deg on Perfect and Dark Agent). 1 = original")
    .range(0.25, 4.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(pdz_autoaim_min_radius_cm, 0.0, "Gameplay",
                      "Up close, widen the auto-aim lock radius to cover at least this much of "
                      "the target (cm; the original covers ~13 cm at 2 m, ~66 cm at 10 m). "
                      "Enhancement, e.g. 35; 0 = original (default)")
    .range(0.0, 200.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(pdz_autoaim_strength_scale, 1.0, "Gameplay",
                      "How far a shot is bent towards a locked target's centre (the "
                      "crosshair itself never moves): 1 = all the way (original), 0.5 = half "
                      "way, 0 = not at all")
    .range(0.0, 1.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(pdz_aim_friction, 0.0, "Gameplay",
                      "Console-style slowdown: slow turning by this fraction while the "
                      "crosshair is within pdz_aim_friction_zone_deg of a target (or on its "
                      "body, if that's bigger), fading out by twice that (the original game "
                      "has none). Enhancement, e.g. 0.35; 0 = original (default)")
    .range(0.0, 0.9)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(pdz_aim_friction_zone_deg, 3.0, "Gameplay",
                      "Size of the pdz_aim_friction sticky zone around a target's centre, in "
                      "degrees unzoomed (shrinks with scope zoom; at least the target's body). "
                      "Bigger = distant targets are easier to settle on")
    .range(0.5, 15.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_autoaim_ignore_difficulty, false, "Gameplay",
                    "Auto-aim works the same on every difficulty (the Agent / Secret Agent "
                    "rules): the original gives Perfect and Dark Agent a weaker auto-aim and "
                    "turns it off while zoomed. Enhancement; false = original (default)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(pdz_aim_friction_near_m, 2.0, "Gameplay",
                      "No aim friction on a target closer than this (metres), so you can turn "
                      "fast enough at close range; it fades back in up to "
                      "pdz_aim_friction_full_m. 0 = friction at every distance")
    .range(0.0, 50.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(pdz_aim_friction_full_m, 6.0, "Gameplay",
                      "Aim friction is at full strength on targets this far away (metres) or "
                      "further, fading out to none at pdz_aim_friction_near_m")
    .range(0.0, 100.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(pdz_aim_friction_sweep_start_dps, 10.0, "Gameplay",
                      "Aim friction eases off once the target sweeps across your view faster "
                      "than this (deg/s, from your movement or theirs, e.g. circle strafing), "
                      "and is gone by pdz_aim_friction_sweep_end_dps")
    .range(0.0, 360.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(pdz_aim_friction_sweep_end_dps, 30.0, "Gameplay",
                      "No aim friction while the target sweeps across your view this fast "
                      "(deg/s) or faster. Set very high (360) to turn the easing off")
    .range(1.0, 1000.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(pdz_aim_friction_toward, 0.9, "Gameplay",
                      "How much of pdz_aim_friction applies while you turn toward the target "
                      "(following it up a ladder or across the screen). Default 0.9, a little "
                      "less than when overshooting; 0 = only slows overshoots; 1 = always")
    .range(0.0, 1.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_aim_still_lock, true, "Gameplay",
                    "Auto-aim also bends shots while you hold still or track slowly: lock radius "
                    "base + turn extra (~1.7 deg on Agent), like target selection, instead of the "
                    "original (base + extra) x turn ratio, which is ~0 unless you whip the stick. "
                    "Enhancement, on by default (F10 switches the aim tweaks); false = original")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_aim_respect_headshots, true, "Gameplay",
                    "When auto-aim bends a shot while your crosshair is already over the "
                    "target, keep your own aim height and bend only sideways, so a head aim "
                    "hits the head (the game bends to a fixed point on the body). Enhancement, "
                    "on by request for testing (F10 switches the aim tweaks); false = original")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_aim_assist_zoomed, true, "Gameplay",
                    "Keep the game's rotational aim assist (the sticky turn with a target) on "
                    "while zoomed; the original switches it off when you zoom. With "
                    "pdz_autoaim_zoom_falloff on it stays on only up to 2x zoom (a scope gets the "
                    "original rule). Enhancement, on by request (F10 switches the aim tweaks); "
                    "false = original")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_autoaim_zoom_falloff, true, "Gameplay",
                    "The more you zoom, the less auto-aim bends your shots: zoomed, a shot is "
                    "bent onto a target only if that moves it less than pdz_autoaim_zoom_radius x "
                    "(1 - zoom)^2, else it goes exactly where the crosshair points (about none at "
                    "full scope zoom). It also limits the zoomed sticky turn of "
                    "pdz_aim_assist_zoomed to 2x zoom. Not zoomed: unchanged. Enhancement, on by "
                    "default (F10 switches the aim tweaks); false = the bend at every zoom")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(pdz_autoaim_zoom_radius, 0.05, "Gameplay",
                      "How far auto-aim may move a zoomed shot before pdz_autoaim_zoom_falloff "
                      "refuses the bend, at the start of the zoom, in the game's unzoomed screen "
                      "units (1 = the edge of the view; 0.05 is about 1.6 degrees). It shrinks "
                      "with (1 - zoom)^2: 2x zoom allows about 7 cm at 10 m, the P99's 5x scope "
                      "about 1 cm. 0 = no bending while zoomed")
    .range(0.0, 0.5)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_autoaim_log, false, "Debug",
                    "Log auto-aim: one 'pdz_autoaim: shot' line per shot by a local player and a "
                    "'pdz_autoaim: decision' line when the target or the result changes (at most "
                    "10 a second): zoom, target and distance, how far auto-aim moves the shot, the "
                    "zoom falloff's limit, the target's angle from the crosshair, whether still "
                    "lock widened the lock, and bent / refused")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(pdz_fov_scale, 1.0, "Gameplay",
                      "On-foot field of view (first and third person), as a multiple of the "
                      "original 58 degrees (a 4:3 horizontal FOV: 45 vertical, about 73 across "
                      "at 16:9). 1.1 = ~79 across, 1.2 = ~86. Scopes keep their "
                      "magnification relative to it. 1 = original (default)")
    .range(0.7, 1.5)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(pdz_ragdoll_inertia_scale, 1.0, "Gameplay",
                      "Divides ragdoll limbs' resistance to spinning (it scales the inverse "
                      "inertia the game sets). The game gives every body part 10x its largest "
                      "natural moment on all axes, for stability; 10 = the largest natural moment "
                      "(lighter, freer limbs), 0.5 = twice the game's (stiffer). 1 = original "
                      "(default). Applies to characters created after the change")
    .range(0.1, 20.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(pdz_ragdoll_angular_damping, -1.0, "Gameplay",
                      "Ragdoll body parts' angular damping (original: Havok's 0.05, limbs keep "
                      "swinging). E.g. 0.4; -1 = original (default). Applies to characters created "
                      "after the change")
    .range(-1.0, 5.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(pdz_ragdoll_linear_damping, -1.0, "Gameplay",
                      "Ragdoll body parts' linear damping (original 0; more = slower, floatier "
                      "falls). -1 = original. Applies to characters created after the change")
    .range(-1.0, 2.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(pdz_ragdoll_death_blend_max, 0.0, "Gameplay",
                      "Longest blend, in seconds, from a death animation into the ragdoll. "
                      "The game uses 2.5x the animation's length (often 7-16 s), so bodies "
                      "float down half-animated. E.g. 1.5; 0 = original (default)")
    .range(0.0, 20.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_ragdoll_log, false, "Debug",
                    "Log ragdoll animation/physics switches, while a ragdoll is still being "
                    "driven by an animation its blend state ten times a second, each body's "
                    "computed and set inertia, and for the first second after a switch to "
                    "ragdoll the pelvis' height and speed (and how long its first 1 m drop took)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_game_fixes, true, "Gameplay",
                    "Master switch for the game bug fixes (pdz_fix_*: Rare's title update fixes and "
                    "confirmed bugs, all on by default). Off = the original game. "
                    "bind_game_fixes_ab (F11) flips it in game")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_fix_tu_cover_cooldown, true, "Gameplay",
                    "Rare's fix from the title update: after leaving cover, the game waits 1 s "
                    "before looking for cover again (the cover prompt). Official fix, on by "
                    "default. Needs pdz_game_fixes")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_fix_tu_datacore_null, true, "Gameplay",
                    "Rare's fix from the title update (U7): the Datacore level script skips its "
                    "tracked-character block once that character is gone (the original reads it "
                    "with no null check, a crash). On by default. Needs pdz_game_fixes")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_fix_tu_bot_task_null, true, "Gameplay",
                    "Rare's fix from the title update (U8): the multiplayer bot scripts stop when "
                    "their mode object is missing (a crash in the original; in this version it never "
                    "happens offline, a guard). On by default. Needs pdz_game_fixes")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_fix_tu_base_slot_null, true, "Gameplay",
                    "Rare's fix from the title update (U9): the multiplayer base lookup skips empty "
                    "slots of its 4-entry table (a crash on a map with fewer than 4 bases). On by "
                    "default. Needs pdz_game_fixes")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_fix_tu_lv10_health, true, "Gameplay",
                    "Rare's fix from the title update (U11): in the jungle level (LV10) the script "
                    "gives one more character (its list entry 14) the 15000 health its list entry 13 "
                    "gets. On by default. Needs pdz_game_fixes")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_fix_tu_township, true, "Gameplay",
                    "Rare's fix from the title update (U12): Township (the Outpost mission) gets the "
                    "title update's 7 replacement background textures (on level load / restart) and "
                    "its hit-fix collision prop (at boot: restart to change it). Needs the title "
                    "update's data package installed (tools/install_map_packs.py --update). On by "
                    "default. Needs pdz_game_fixes")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_fix_tu_flag_drop, true, "Gameplay",
                    "Rare's fix from the title update (U13 / U17): in the flag modes, destroying the "
                    "flag weapon of someone who isn't the carrier no longer resets the carrier's flag "
                    "to dropped. On by default. Needs pdz_game_fixes")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_fix_tu_teleport, true, "Gameplay",
                    "Rare's fix from the title update (U16): a multiplayer teleport (teleporter pads, "
                    "the out-of-bounds return) re-inserts the character in the world's spatial lists "
                    "and moves its attached objects with it. On by default. Needs pdz_game_fixes")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_fix_tu_mp_hitfix, true, "Gameplay",
                    "Rare's fix from the title update (U18): Chinatown and Desert get the title "
                    "update's hit-fix collision props (out-of-bounds / shoot-through spots; at boot: "
                    "restart to change it). Needs the title update's data package installed. On by "
                    "default. Needs pdz_game_fixes")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_fix_tu_pack_markers, true, "Gameplay",
                    "Rare's fix from the title update (U21): no out-of-bounds HUD markers on Facility "
                    "and Ruin (Map Pack Two). On by default. Needs pdz_game_fixes")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_fix_tu_pack_map_rules, true, "Gameplay",
                    "Rare's rules from the title update (U22): with Facility or Ruin chosen, the "
                    "setup offers only their scenarios (Combat: the first two; DarkOps: the first and "
                    "third) and up to 8 players, and the Random map picks from every map (the "
                    "original never picked GasPlant, Facility or Ruin), leaving out Facility / Ruin "
                    "for other scenarios. Needs pdz_dlc_maps. On by default. Needs pdz_game_fixes")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_fix_halo_zoom_curve, true, "Gameplay",
                    "Standard controls below default horizontal sensitivity: the zoomed / in-cover "
                    "stick curve blends toward the default setting like every other value, not "
                    "toward the max-sensitivity one (wrong record in the original). On by default. "
                    "Needs pdz_game_fixes")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_fix_spread_cap_order, true, "Gameplay",
                    "Apply the stance spread cap before the shot direction is worked out (the "
                    "original caps it just after, so a shot right after a bloom can exceed it). "
                    "On by default. Needs pdz_game_fixes")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_fix_autoaim_zoom_bend, true, "Gameplay",
                    "Zoomed shots bent by auto-aim land on the target, not 1 / (1 - zoom) times "
                    "as far from the crosshair (5x with the P99's zoom: the original works the "
                    "bend out in zoomed screen units, the weapon expects unzoomed ones). On by "
                    "default. Needs pdz_game_fixes")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_fix_cmp_noise, true, "Gameplay",
                    "Your CMP150's first shot is heard as far as its later ones start (5 m), like "
                    "every other gun, not 10x further (50 m: its minimum noise is 5000 vs 500 per "
                    "shot in the data). On by default. Needs pdz_game_fixes")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_fix_transparency_water, true, "Gameplay",
                    "Game fix (0.9.2, Rare's bug): see-through surfaces drawn in the wrong order against the water "
                    "(street-light glows covered by the water's second layer). "
                    "On by default; F1 toggles it. docs/BUG_AUDIT.md \"Transparency vs water\"")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(pdz_fix_water_loop, true, "Gameplay",
                    "Game fix (0.9.2): Rare's looping water animations (the sea's ripples, caustics, running water: "
                    "stacks of 16 / 32 frames) loop smoothly instead of freezing and jumping back every few seconds. "
                    "A stacked texture whose fetch constant says repeat on its layer axis wraps its layers (the last "
                    "blends into the first) in the native renderer instead of clamping them. On by default; Insert "
                    "toggles it. Needs pdz_game_fixes")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(pdz_fix_short_stacks, true, "Gameplay",
                    "Game fix (0.9.3): the short looping texture animations (stacks of 2-7 frames) that Rare animates "
                    "like the water loop play their frames in turn instead of holding the first and snapping back. Only "
                    "the stacks a draw dump proved animated (a list in native_draw.cpp ShortStackMask); static 2-frame "
                    "blends stay as they were. Native renderer. On by default. Needs pdz_game_fixes")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(pdz_transparency_test_alternate, false, "Testing",
                    "Test aid for pdz_fix_transparency_water: the fix only on odd game frames, so two consecutive "
                    "captures (a still camera) show its change alone.");
REXCVAR_DEFINE_INT32(pdz_transparency_trace_frame, 0, "Testing",
                     "Diagnostic for pdz_fix_transparency_water: log Rare's water / alpha mesh draws (sub_82710CC0) "
                     "and glow pass (sub_826216D0) with their guest call chains for the one game frame whose counter "
                     "first reaches this value (0 = off). Lines start with pdz_transwater:.");
REXCVAR_DEFINE_BOOL(pdz_fix_derez_snapshot, true, "Gameplay",
                    "Emulation fix: the death \"de-rez\" effect (multiplayer and holo deaths: the body "
                    "breaks into coloured cubes) gets its snapshot of the body back from the GPU, so the "
                    "cubes appear (without it the CPU reads a black snapshot and the effect ends at once). "
                    "On by default. Needs pdz_game_fixes")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_fix_random_bytes, true, "Gameplay",
                    "tu3-base, emulation fix: random bytes the game asks the system for (XNetRandom) "
                    "are random, as on the console, instead of the SDK's constant 0xBB. The game's main "
                    "random seed then differs each boot, and e.g. Protection's live terminal and hacker "
                    "turns vary. On by default. Needs pdz_game_fixes")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_UINT64(pdz_random_seed, 0, "Testing",
                      "TESTING TOOL, not a game option (stage 2 M5 cpusplit: repeatable test matches): with "
                      "pdz_fix_random_bytes on, the random bytes come from a generator seeded with this number "
                      "(the same bytes in the same order every boot, so the game's main random seed and the other "
                      "XNetRandom users repeat) instead of a new random seed each boot. 0 (default) = a random "
                      "seed, as before. Read at the first random-bytes call (set it on the command line)");

REXCVAR_DEFINE_BOOL(pdz_fix_ai_semiauto_rhythm, true, "Gameplay",
                    "Enemies with semi-auto guns tap the trigger at the original 30 fps rhythm (the "
                    "tap roll is redone every 1/30 s instead of every frame, so at 60 fps they no "
                    "longer fire about twice as many shots per second). On by default. Needs "
                    "pdz_game_fixes")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_fix_ai_grenade_height, true, "Gameplay",
                    "Enemy grenade check: targets more than 3 m above or below are measured with the "
                    "1.3 scale the code has for them (its test sat behind the 1 m one and never ran). "
                    "On by default. Needs pdz_game_fixes")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_fix_ai_dodge_facing, true, "Gameplay",
                    "Enemy dodges pick from all four facings (rand() % 3 never chose the fourth, so "
                    "the FacingRight dodge animations never played). On by default. Needs "
                    "pdz_game_fixes")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_fix_ai_dodge_clearance, true, "Gameplay",
                    "Enemy backwards dodges check for room behind them, not in front. On by default. "
                    "Needs pdz_game_fixes")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_fix_ai_corner_speed, true, "Gameplay",
                    "Enemies slow to 5-7% of top speed on 85-90 degree path corners instead of "
                    "stopping (off-by-one in the corner speed curve). On by default. Needs "
                    "pdz_game_fixes")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_fix_ai_calm_flag, true, "Gameplay",
                    "Enemy alert count: the 'calm' flag follows the stored count (lowering a count of "
                    "0 cleared it). On by default. Needs pdz_game_fixes")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_fix_ai_cover_min_distance, true, "Gameplay",
                    "Enemy cover: when picking a cover pad, skip pads 10 m or less from you, the "
                    "minimum the game re-checks every tick (it picked them, dropped them the next "
                    "tick and picked them again, so wounded enemies turned for cover and came "
                    "back). On by default. Needs pdz_game_fixes")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_fix_speech_names, true, "Gameplay",
                    "Rare's recorded lines the code asks for under the wrong name now play: "
                    "Datacore's spiderbot lines (scientists and troopers), the Nightclub gangster's "
                    "fire alarm line, Jungle's squad line (a placeholder trooper voice), Bridge's "
                    "Institute squad arrival line, enemies' \"they missed me\" line when a man shot "
                    "at them, elite1's \"she's falling back\", and Jo's \"My voice is my invite.\" at "
                    "the Mansion main door after the satellite hack. On by default. Needs "
                    "pdz_game_fixes")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_fix_sound_names, true, "Gameplay",
                    "Sounds asked for under the wrong name now play: Urban's spawn-room ambience "
                    "(wrong bank name), bullet hits on wood and on flesh (a missing bank name and a "
                    "missing underscore), bullet hits on metal tables (prefix typed twice), Trinity "
                    "Escape's burning flares (no _LP version). On by default. Needs pdz_game_fixes")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_fix_ai_speech_remap_fallback, true, "Gameplay",
                    "When a level remaps an enemy's line to one his voice never recorded (Nightclub "
                    "guards asking for the Mansion troopers' \"she's got a gun\" line), he says his "
                    "normal line instead of nothing. On by default. Needs pdz_game_fixes")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_fix_ai_taunt_rate, true, "Gameplay",
                    "Subway's squaring-up taunts start at the 30 fps rate when the 60 fps unlock is on "
                    "(the chance was rolled per frame, so twice as often). On by default. Needs "
                    "pdz_game_fixes")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_restore_mansion_door_greeting, false, "Gameplay",
                    "Restoration: the Mansion main-door guard says his cut \"let Jo in\" line (\"Enjoy "
                    "the party.\" / \"Miss.\" / \"The entertainment's about to begin.\") when he lets "
                    "Jo through. Off by default")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_restore_light_speech, false, "Gameplay",
                    "Restoration: the Laboratory staff's cut lights-out lines. When the generator "
                    "blackout starts, the nearest scientist (or two, as question and answer) says his "
                    "\"lights out again\" line and the other staff nearby say their \"lights shot out\" "
                    "line. Speech only: the darkness itself is unchanged. Off by default")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// Laboratory | Rescue restorations (src/mod/lab_restorations.cpp; research in
// the Laboratory research notes). Rare's lines and Rare's logic where
// they exist; each off by default.
REXCVAR_DEFINE_BOOL(pdz_lab_blackout_lines, false, "Gameplay",
                    "Restoration: the Laboratory staff's cut lights-out lines on the generator blackout "
                    "(pdz_restore_light_speech's lines), spoken only by staff standing inside one of Rare's "
                    "four darkened light-group boxes (main hall, generator room, cutscene room, security / "
                    "tower), so nobody in the still-lit Lab One / Lab Two says \"lights out again\". Speech "
                    "only. Off by default")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_lab_power_return, false, "Gameplay",
                    "Restoration: the Laboratory's power comes back on the generator SWITCH route only "
                    "(the quiet lockbot route and the explosives stay permanent, as Chandra's lines say). "
                    "pdz_lab_power_return_s after the switch: the lights Rare switched off fade back in, the "
                    "ambient light returns, the darkened guards get their sight and aim back (Rare's "
                    "uncalled reset), the tannoy's \"reconnecting the power\" promises stop (Rare's unused "
                    "flag 40) and Chandra says her recorded, never-played \"Generator is back online\". "
                    "Off by default")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_INT32(pdz_lab_power_return_s, 90, "Gameplay",
                     "pdz_lab_power_return: seconds of game time between the generator switch and the power "
                     "returning. 90 = after the tannoy's \"temporary power failure\" (10 s) and its first "
                     "\"apologize for the delay\" (40-70 s), before the second (70-130 s). Rare left no "
                     "number: the tannoy promises every 30-60 s")
    .range(5, 600)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_lab_darkness_list, true, "Gameplay",
                    "Fix (Rare's data slips): the Laboratory's darkness list reaches every generator-room "
                    "guard. Rare's list names s3g6, but that guard's record is tagged s3g4 by mistake (a "
                    "duplicate, so only one of the two is found), and the two variant walkway guards "
                    "(s3gb / s3gc) were left off, so on two of three level variants the walkway guard keeps "
                    "full sight in the dark. On by default")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_INT32(pdz_lab_blackout_coverage, 1, "Gameplay",
                     "OUR addition (not Rare's): how far the Laboratory generator blackout reaches. 1 = Rare's "
                     "four light-group boxes exactly as shipped (560 lights off; Lab One, Lab Two, the prison, the "
                     "vents and the strips down both sides of the main hall stay lit; the 25 listed guards "
                     "darkened). 2 = also the gaps inside the dark areas: the lit strips down both sides of the "
                     "main hall (45 lights) and the far end of security (26), 71 more. 3 = every switchable light "
                     "in the level except the red cell lights, the amber security lights and the sun (201 more "
                     "than 1), and the darkness (10 m sight, slow aim) for everyone Rare's list leaves out: the "
                     "prison guards (Rare's full darkness with his search, also when they wake after the "
                     "blackout) and the ten scientists (sight only, so a calm scientist no longer spots Jo across "
                     "a lit lab and runs for the alarm; his behaviour is otherwise unchanged). Same instant "
                     "switch-off as Rare's, in the same frame, on every route; the level is fixed at the "
                     "blackout. pdz_lab_power_return restores all of it; pdz_lab_blackout_lines drops its box "
                     "filter at 3. Default 1")
    .range(1, 3)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_fix_lab_tannoy_repeat, true, "Gameplay",
                    "Fix (Rare bug): after the Laboratory generator blackout the tannoy's \"we apologize for the "
                    "delay in reconnecting the power\" (TANNOY_TRIGGER03) repeats every 30-60 s for the rest of "
                    "the mission, even over the alarm, because the \"power restored\" flag that stops it (40) is "
                    "never set. With this on it plays at most 3 times, then the power announcements stop (flag 40 "
                    "is left alone, so the joke announcements don't come back in the dark). pdz_lab_power_return "
                    "still sets flag 40 when the power returns. On by default")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_lab_cut_lines, false, "Gameplay",
                    "Restoration: small Laboratory lines Rare recorded and never wired. Chandra's gas payoff "
                    "(\"just as well you locked the valve down\" / \"destroyed the vent fans\") when the "
                    "Perfect / Dark Agent gas attack would have started but the defences were already "
                    "neutralised; the gas programmer's \"OK, OK. Gimme a chance.\" after Jo's threat; "
                    "Chandra's level hints 02 (gold keycard, after a gold door refuses Jo), 07 (where the "
                    "tracking system is, after she announces it), 08 (the datathief virus, after \"controls "
                    "are locked out\") and 10 (where the gas is controlled, after the gas starts). Off by "
                    "default")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// Testing tools (not game options): for testing in the recomp only.
REXCVAR_DEFINE_BOOL(pdz_test_god_mode, false, "Testing",
                    "TESTING TOOL, not a game option: the player takes no damage (the player's damage "
                    "handler runs with 0 damage); enemies see, shoot and react as normal. Off by default")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_test_ai_ignores_player, false, "Testing",
                    "TESTING TOOL, not a game option: bots never take a human player as their enemy "
                    "target (Rare's reference setter sub_821433D8 on a bot's target slot, +12648, is "
                    "given nothing for a player), so they play the match round him and he can watch "
                    "them. Off by default")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_STRING(pdz_test_mp_setup, "", "Testing",
                      "TESTING TOOL, not a game option (stage 2 M5: repeatable MP test matches): Combat Arena "
                      "match options set through the game's own setter, e.g. "
                      "\"bots=31,points=15,time=10,map=urban,scenario=dm\" (any subset). Any value, \"fresh\" "
                      "alone included, also makes the setup ignore the profile's saved setup (Rare's "
                      "presets instead, as on a fresh profile: a scripted route no longer starts from the previous "
                      "run's values). Set when the setup loads and again at match start; logs 'pdz_test_mp:' lines "
                      "(the values in effect at match start, the bots really spawned). Empty (default) = off. "
                      "See src/mod/test_mp_setup.cpp");

REXCVAR_DEFINE_INT32(pdz_test_spawn_pad, -1, "Testing",
                     "TESTING TOOL, not a game option (stage 2 M5): the human player's spawn pad is this "
                     "free-for-all spawn pad (group 0, 0-based in the map's spawn list order; Urban has 93), so "
                     "his position and facing (Rare's, from the pad) are the same every run; logs a "
                     "'pdz_test_mp: human spawn' line. Modes that never use the free-for-all chooser for the "
                     "human are left as Rare's (the line says NOT PINNED). -1 (default) = off")
    .range(-1, 4095);

REXCVAR_DEFINE_INT32(pdz_test_mp_snapshot_s, 0, "Testing",
                     "TESTING TOOL, not a game option (stage 2 M5: determinism check for compiler experiments): "
                     "every N seconds of match time (the game's own match clock), at match end and at test exit, one "
                     "'pdz_test_mp: snapshot' line with every participant's position, kills, deaths and score plus "
                     "a short hash; a 'pdz_test_mp: SUMMARY' line (totals and the hash chain) at match end and at "
                     "test exit (--pdz_test_exit_s). Compare two logs with PDZRenderResearch "
                     "native/tools/mpsnap_compare.py. 0 (default) = off. See src/mod/test_mp_setup.cpp")
    .range(0, 3600)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_test_lab_light_census, false, "Testing",
                    "TESTING TOOL, not a game option: at the Laboratory blackout and at pdz_lab_power_return's power "
                    "return, log how many scene lights are on / off in total and per area (main hall, generator room, "
                    "cutscene room, security, Lab One, Lab Two, prison, vents, spawner end), to check "
                    "pdz_lab_blackout_coverage by log. Off by default")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_test_lab_blackout_key, false, "Testing",
                    "TESTING TOOL, not a game option: in Laboratory, the Home key sends the generator "
                    "blackout messages to the level script as the generator switch did in log 114 "
                    "(lights out, Chandra's lights-out line, the darkness; the tannoy 10 s and 52 s later), "
                    "and every Laboratory script message is logged. Off by default")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_INT32(pdz_test_lab_blackout_now, 0, "Testing",
                     "TESTING TOOL, not a game option: in Laboratory, changing this to a non-zero value sends the "
                     "generator blackout to the level script as the Home key of pdz_test_lab_blackout_key does "
                     "(input scripts: 'cvar <t> pdz_test_lab_blackout_now 1'). 1 = the lockbot-style blackout "
                     "(Chandra's CHAN_06); 2 = the switch route for pdz_lab_power_return (Chandra's CHAN_04 + "
                     "TANNOY_TRIGGER01 and the switch-route marker, flag 37; the real switch's alarm 14 s later "
                     "is not sent). Set it back to 0 before sending another. 0 (default) = nothing")
    .range(0, 2)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_controls_fixes, false, "Gameplay",
                    "Master switch for the player controls fixes (pdz_fix_*). Off = the original "
                    "controls. In the TU3 build only pdz_fix_sensitivity_clamp is ported (look_tuning.cpp); "
                    "the release settings turn it on with that one fix")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_fix_fastlook_stick, false, "Gameplay",
                    "Default (PD) look model: fast look and the cross deadzones use the look stick, as "
                    "in the Halo model, instead of the move stick's (running forward blocks fast look). "
                    "Kept off for good (decision 2026-11-04: Rare's PD-style design, not a bug); launch build "
                    "only. Needs pdz_controls_fixes")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_fix_fastlook_decel, false, "Gameplay",
                    "Fast look slows down at the normal decel rate (when zooming, in cover or easing "
                    "the stick) instead of gliding on at the fast-look accel. Off: PD-style look only "
                    "(decision 2026-11-04); launch build only. Needs pdz_controls_fixes")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_fix_sensitivity_clamp, false, "Gameplay",
                    "Cap the look sensitivity at the max record: the x1.2 (Fast Look off) and x1.75 "
                    "boosts no longer extrapolate past it, which made mid-stick turns slower. On in the "
                    "release settings (the one controls fix kept; ported to TU3 in look_tuning.cpp). Needs "
                    "pdz_controls_fixes")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_fix_fastlook_accel_data, false, "Gameplay",
                    "Default sensitivity's fast-look accel 3 instead of 2 (below both min 3 and max 4 "
                    "in the PD data). Off: PD-style look only (decision 2026-11-04); launch build only. "
                    "Needs pdz_controls_fixes")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_fix_spread_recovery_gate, false, "Gameplay",
                    "Gun spread recovery: a hand with no weapon no longer counts as 'just fired', "
                    "which would stop the spread recovering. Dropped (decision 2026-11-04: the empty-hand "
                    "case was never seen in game); launch build only. Needs pdz_controls_fixes")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_ai_cover_fixes, true, "Gameplay",
                    "Master switch for the enemy cover fixes (pdz_ai_cover_fix_*), on by default. "
                    "Off = the original enemy cover AI. bind_ai_cover_ab (F5) flips it in game")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_ai_cover_fix_grace, true, "Gameplay",
                    "Enemy cover: start the 5 s / 6 s 'can I still fight from here' timers when the "
                    "enemy reaches its cover point, not when it decides to go there, so a long run "
                    "doesn't end in leaving cover on arrival. Needs pdz_ai_cover_fixes")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_ai_cover_fix_peek_roll, true, "Gameplay",
                    "Enemy cover: give the alternate peek animations their intended 1-in-3 "
                    "chance (the original roll never succeeds). Needs pdz_ai_cover_fixes")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_ai_cover_fix_snapback, true, "Gameplay",
                    "Enemy cover: give up a cover point after 3 snap-backs in a row (it can't be "
                    "reached within 15 cm) instead of re-aligning forever without shooting. Needs "
                    "pdz_ai_cover_fixes")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_dlc_maps, true, "Content",
                    "Official content restoration: the two official map packs (Trench, Rooftops, "
                    "Plaza, GasPlant; Facility, Ruin) in the multiplayer map list, using what "
                    "Title Update 3 adds for them on the launch game (install: docs/MAP_PACKS.md). "
                    "Without the packs installed the game is unchanged (the original maps). "
                    "Restart after changing it")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(pdz_mp_music, true, "Gameplay",
                    "Enhancement: a Music option on the multiplayer setup screen (Off / Random / "
                    "a soundtrack track) that plays the game's own music over the map's ambience. "
                    "The Music value itself defaults to Off (original). false removes the option. "
                    "Restart after changing it")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(pdz_mp_music_random_loops, 1, "Gameplay",
                     "Multiplayer match music, Music = Random: how many times a track plays through (intro + loop, then "
                     "the loop again) before Random moves on, 1-8. 1 = once, as before. A chosen track always loops")
    .range(1, 8);
REXCVAR_DEFINE_STRING(pdz_mp_music_custom_dir, "", "Gameplay",
                      "Multiplayer match music: a folder of your own MP3s (e.g. C:\\Music\\PDZ). Each file "
                      "is added to the Music row after the game's tracks as \"REMIX: <title>\" (its ID3 title, else "
                      "its file name) and to Random; played by the host (FFmpeg) over the map's ambience, its "
                      "loudness matched to the game's tracks (measured once on a worker thread, kept in "
                      "mp_music_custom.txt in the user data folder), fading in over 2 s and out over 3 s. Empty = "
                      "off (default). Restart after changing it")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_DOUBLE(pdz_mp_music_custom_volume_db, 0.0, "Gameplay",
                      "Multiplayer match music: your own MP3s (pdz_mp_music_custom_dir), extra volume in dB on top "
                      "of the loudness match, followed live; -30 to +12")
    .range(-30.0, 12.0);
REXCVAR_DEFINE_BOOL(pdz_dlc_log, false, "Debug",
                    "Log the map packs: mounts, manifests, registered maps, packages, string "
                    "tables, cue lists and files taken from a pack (pdz_dlc: lines). Diagnostic "
                    "only")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_ai_state_log, false, "Debug",
                    "Log once a second per enemy that is aware or within 50 m (pdz_ai: lines): "
                    "setup (CAUTIOUS / HOLD_POS / ALL_OUT ...), aware / hurt / shot-at / "
                    "provoked / no-cover flags, alert, target and distance, own and player pads, "
                    "running tasks, and what its combat planner did since the last line (plans "
                    "built by kind, cover chooser give-ups, cover pads tried / passed / re-check "
                    "failures, plan ends, fallbacks). Diagnostic only; needs log_level = info")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_cover_log, false, "Debug",
                    "Log the player's cover state changes with timestamps (cover found/lost, "
                    "enter requested, in cover, state changes, exit, out of cover) to measure "
                    "transition times, plus enemies' cover tasks (pdz_cover_ai: lines: taking "
                    "cover, peeking, failed cover tests, totals once a minute). Diagnostic only; "
                    "cover behaviour is unchanged")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_combat_log, false, "Debug",
                    "Combat aiming measurement (player only): while a target is selected or an "
                    "enemy targeting you is within 40 m and in view, 20 Hz samples of distance, "
                    "needed vs actual vs maximum turn rate, stick, fast look, zoom, aim error, "
                    "assist and bending, plus every shot and whether it hit. Writes "
                    "logs/combat_<time>.csv and logs/combat_summary.txt; summary by distance band "
                    "every minute. Diagnostic only")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_fx_log, false, "Debug",
                    "Log the de-rez death effect (multiplayer / holo deaths): each request, the slot "
                    "and REZOUT sound, the body snapshot's resolve and GPU callback, how many non-black "
                    "pixels the CPU found in the snapshot and how many particles that gave. Needs "
                    "log_level info")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_button_log, false, "Debug",
                    "Log every press and release of A / B / X / Y, the bumpers and the triggers as "
                    "the game read them (time, frame), whether the game registered the press, and "
                    "for X (reload) and B (secondary) whether it started or why the gun refused it. "
                    "Needs log_level info")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_shot_log, false, "Debug",
                    "One pdz_shot: line per hitscan shot by a player (needs log_level = info): "
                    "weapon, zoom, cover state and blend timer, shot origin vs the eye / cover "
                    "camera, what the crosshair is on, auto-aim target, its aim point and the "
                    "bend, spread, where the shot went vs the crosshair, whether the straight "
                    "path to the target's aim point is blocked, and what was hit (character and "
                    "hit box, or world). Diagnostic only")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_music_log, false, "Debug",
                    "Log music changes: music type (Interactive, Tune1-4, Death), interactive music "
                    "level (Silence / Stealth / Speedup), music cue starts (and failures), pause / "
                    "resume / stop. Diagnostic only")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_look_log, false, "Debug",
                    "Log the look controller's game-controlled turns (ladder mounts and other set pieces: one "
                    "'pdz_look: game-controlled turn' line each) and, with pdz_autoaim_log, the controls fixes' "
                    "auto-aim lines. The per-frame right-stick log of the launch branch (docs/LOOK_PIPELINE.md) was "
                    "not ported to TU3; pdz_look_slow_detect logs the same chain when a full push turns slowly. "
                    "Diagnostic only")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_look_slow_detect, true, "Debug",
                    "Slow-turn detector (overnight 30-31 Oct; in play testing on two pads, turning left on the right "
                    "stick was sometimes suddenly slow, on foot): while the right stick is pushed beyond 0.8 sideways (raw, after "
                    "the deadzone) for 0.4 s, not zoomed, and the view turns at under 60% of what a full push "
                    "normally gives (the recent peak), one 'pdz_look: SLOW TURN' line with everything that shapes the "
                    "turn (raw / filtered stick, deadzone, Rare's learned centre and its sample count, look-stick "
                    "values, the turn rate vs expected, the stick-driven rate and the aim assist's counter-turn, fast "
                    "look, sensitivity, scales, zoom, our look options); the next full push the other way logs the "
                    "same line for comparison. At most one line per 0.5 s; reads only, logs only when it fires")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_mp_cut_scenarios, false, "Gameplay",
                    "tu3-base, enhancement (off = original): offer Rare's cut scenarios, whose "
                    "code is complete in the game: Plant the Flag (Deathmatch 3, after Capture the "
                    "Flag; our bots), Protection (Deathmatch 5, on the six launch maps, which have "
                    "its terminals; humans hack). Duel is parked: see pdz_mp_cut_scenarios_duel")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_mp_cut_scenarios_duel, false, "Gameplay",
                    "tu3-base, with pdz_mp_cut_scenarios: also offer Duel / Team Duel (DarkOps 4, on "
                    "Subway, Tower, Temple and Old Town, which have its arenas). Rare's round award, empty in "
                    "both builds, is filled in (rounds end and rotate, Team Duel scores round wins; "
                    "2026-10-28, untested in play; docs/TU3_BASE.md)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_duel_bracket, true, "Gameplay",
                    "tu3-base, part of pdz_mp_cut_scenarios_duel (Rare's cut mode, off by default): Duel / Team Duel as "
                    "a knockout bracket (the user's design, docs/DUEL_DESIGN.md): 2 / 4 / 8 players (Team Duel 2 teams "
                    "of 2 or 4), each pair in its own laser-sealed zone, a winner holstered until the lasers on the "
                    "border to a neighbouring decided zone open for him (touch to drop, per laser), byes through empty "
                    "zones, bots walk to their gate, a stall hunt after 60 s and every laser down in the round's last "
                    "60 s. false = Rare's Duel as it is (any count, Rare's placement and group drops)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_test_duel_key, false, "Testing",
                    "TESTING TOOL, not a game option: in Duel, every human player is given key 1 at each round start "
                    "(Rare's key setter), to check the gates' green colour; logs the laser colour decision ('Duel "
                    "gate colour') once per laser and viewer each round. Off by default")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_mp_announcer_extras, true, "Gameplay",
                    "tu3-base, fix (on): Rare recorded these multiplayer announcer lines and their code never "
                    "plays them. Sabotage names the destroyed target (field supplies, fuel dump, ammo cache, "
                    "comms, power node) instead of 'sabotage successful'; Plant the Flag says planting / "
                    "enemy planting / enemy flag planted; Deathmatch matches end with victory / defeat / match "
                    "drawn and call victory imminent, one kill to win and teams level. Only the announcer "
                    "(src/mod/mp_announcer.cpp) reads it. Off = the original announcer")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_STRING(pdz_ptf_spawn, "random", "Gameplay",
                      "tu3-base, with pdz_mp_cut_scenarios: Plant the Flag spawns. Every life starts "
                      "in the team's armory as Rare's. random = the match's first teleporter trip goes "
                      "to the team's base, later trips to a free-for-all spawn pad with nobody within "
                      "20 m, never within 15 m of a flag base (base where too few pads qualify); "
                      "base = Rare's (always the team's pads at its base)")
    .allowed({"random", "base"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_INT32(pdz_protection_dt_icon, 9, "Gameplay",
                     "tu3-base, with pdz_mp_cut_scenarios: map icon for a Protection Data Thief "
                     "lying on the ground (Rare drew none): an icon of the HUD sheet the map draws "
                     "from (1-33; 9 = round glowing orb, unused by the game, chosen by the user; "
                     "20 = the live terminal's splat); 0 = no marker")
    .range(0, 33)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_protection_holster, true, "Gameplay",
                    "tu3-base, with pdz_mp_cut_scenarios: a Protection hacker carries the Data Thief "
                    "put away (empty hands, so unarmed running and ladder animations) instead of Rare's "
                    "always in hand (no third-person model or animations exist for that); it lifts "
                    "itself at the live terminal, as in single player. Weapons stay locked either way")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_ptf_base_marker, true, "Gameplay",
                    "tu3-base, with pdz_mp_cut_scenarios (Plant the Flag only; untested in play): each ENEMY "
                    "team's base (where to plant) is marked on the viewer's map with Protection's live-terminal "
                    "icon, drawn exactly as Protection draws it; own base not marked; map only. false = Rare's "
                    "map (no marker). docs/TU3_BASE.md \"Plant the Flag: where to plant\"")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_protection_fair_hunt, true, "Gameplay",
                    "tu3-base, with pdz_mp_cut_scenarios and pdz_protection_objective_focus (untested in play): "
                    "Protection hunters (group 3, and group 1 with no hacker of its own) hunt the enemy hacker on "
                    "Rare's goal 3 only while one of them perceives him (seen or heard) and for 5 s after; "
                    "otherwise they patrol the live terminal on goal 7. false = they always hunt (Rare's goal 3 "
                    "steers to him wherever he is). docs/TU3_BASE.md \"Protection: fair hunt\"")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(pdz_protection_spread, true, "Gameplay",
                    "tu3-base, with pdz_protection_bots and pdz_protection_objective_focus (0.9.2; release log 001: "
                    "bots bunched in one open area): per team at most 3 escorts, 4 hunters and 4 terminal patrollers; "
                    "the patrollers on their own pads of a ring 9 m round the live terminal (each team on its own "
                    "sectors), every other helper on his own pad of an outer ring 18-34 m out on the approaches, a "
                    "new one every 40 s. false = everyone on the terminal's one pad as before (the test runs' before "
                    "case). docs/TU3_BASE.md \"Protection: the spread\"")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_protection_hack_under_fire, true, "Gameplay",
                    "tu3-base, with pdz_mp_cut_scenarios (untested in play): a Protection hacker's hack at the "
                    "live terminal is no longer cancelled when he takes damage (the damage path's put-away of the "
                    "Data Thief is skipped for him); death or leaving the terminal still ends it. false = Rare's "
                    "cancel. docs/TU3_BASE.md \"Protection: hack under fire\"")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_bot_turrets, true, "Gameplay",
                    "tu3-base, our fix (on by default; off = original): bots fire the turrets they "
                    "mount (fixed M60 / plasma / rocket hardpoints and the hovercraft gunner seat). "
                    "Rare's turret code only lets a human gunner fire, so a bot sat there without "
                    "shooting; the bot's own fire request now goes through Rare's fire step")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_bot_turret_log, false, "Debug",
                    "tu3-base: log bot turret gunners (mount, state, trigger, shots)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_bot_count_default, false, "Gameplay",
                    "tu3-base (off): a DEFAULT choice before 0 on the MP bot count row. DEFAULT fills the "
                    "match to Rare's own size for the map (8 players on levels with flag 33 / 35, else 16) "
                    "minus the humans, and follows scenario and map changes; Sabotage without "
                    "pdz_sabotage_bots stays at Rare's 0. docs/TU3_BASE.md, \"Bot count DEFAULT\"")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_sabotage_bots, false, "Gameplay",
                    "tu3-base, with pdz_mp_cut_scenarios, (on in the release settings (played in the developer's matches since 2026-10-27)): bots in DarkOps Sabotage. "
                    "The setup screen offers the bot rows for Sabotage (Onslaught's panel, as Rare "
                    "does for Duel); bots fight as Rare assigns attackers and defenders. Objectives "
                    "come in later checkpoints (docs/TU3_BASE.md, \"Sabotage bots\")")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_sabotage_experiments, false, "Gameplay",
                    "tu3-base, with pdz_sabotage_bots, EXPERIMENTAL research (off): human firing spots "
                    "(a piece the player destroys records where he stood, sabotage_spots/<map>.txt next to "
                    "the exe, and attackers then shoot from there), the stall probe and route scans (a bot "
                    "that hasn't moved 50 cm in 4 s: collision rays and pad / link fields, logged) and the "
                    "real-bullet log (\"bullet:\" lines). docs/TU3_BASE.md, Logs 152-159")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_sabotage_objective_focus, false, "Gameplay",
                    "tu3-base, with pdz_sabotage_bots, (on in the release settings (played in the developer's matches since 2026-10-27)): the whole attacking team on "
                    "Rare's goal 4 for the round (no roam groups, no 2 s reset to goal 0; Rare's chooser "
                    "keeps objective goals out of fights, they shoot while walking), Rare's activity as the "
                    "fight test instead of the enemy target, and the defenders' guard groups 2 and 3 on "
                    "goal 7 at the two most valuable pieces (group 1 roams). docs/TU3_BASE.md, \"Rare's "
                    "bot AI, the untraced parts\"")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_sabotage_people_stage1, false, "Gameplay",
                    "tu3-base, with pdz_sabotage_bots, (on in the release settings (played in the developer's matches since 2026-10-27)): Sabotage bots play like people, "
                    "stage 1: targets scored against the round clock (a piece out of reach this round scores "
                    "nothing), gunners shoot any live piece they see within 35 m on the way (field supplies "
                    "first; a short detour within 20 m when no enemy is near it; 10 s cap, 8 s cooldown), melee "
                    "only on field supplies, and attackers leave the last defender alive unless he is close or "
                    "hitting them. docs/TU3_BASE.md, \"Sabotage: playing like people\"")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_sabotage_nav_fix, false, "Gameplay",
                    "tu3-base, with pdz_sabotage_bots, PARKED (off; research only, docs/TU3_BASE.md 'Rare's bot "
                    "navigation'): registers the Sabotage sandbag walls as Rare obstacles (Rare's own link closing, "
                    "differences logged), refuses a bot's current pad across a sandbag wall, asks Rare to route "
                    "again on no progress, and logs Rare's route lists at wall pushes. It sealed the ammo dump on "
                    "Desert in every test (logs 286-292). The choke point and the route-failure fallback it "
                    "brought stay on with pdz_sabotage_people_stage1.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_sabotage_people_stage2, false, "Gameplay",
                    "tu3-base, with pdz_sabotage_bots, (on in the release settings (played in the developer's matches since 2026-10-27)): Sabotage defenders play like people, "
                    "stage 2: Rare's guard groups loiter at the top pieces, a few defenders roam, the rest hold "
                    "near the property and respond to what a human defender gets (the announcer's explosives "
                    "set / piece destroyed, by the piece's position) on goal 7 for 20 s; a lost piece is "
                    "re-guarded at the nearest good one. docs/TU3_BASE.md, \"Sabotage: playing like people\"")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(pdz_sabotage_desert_dump_wall_removed, false, "Gameplay",
                    "tu3-base, MAP TWEAK (off): in Sabotage on Desert the ammo dump's back sandbag wall "
                    "(propsetup Static1110564230 at (-6724, 905, -4663)) is not spawned: no model, no collision, "
                    "no nav registration, no wall line of ours. Ten bot builds on this one wall and bots still "
                    "got caught on it (log 298); the same map for both sides. docs/TU3_BASE.md, \"Map tweak: Desert's "
                    "dump back wall\"")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(pdz_sabotage_desert_nav, false, "Gameplay",
                    "tu3-base, Sabotage bots on Desert only (off; on in the release settings (played in the developer's matches since 2026-10-27)): the Desert pad table "
                    "(the dump's cache floor, the power node catwalk and the comms pen inside are never spots, "
                    "stand-ins or walk-set pads; the pads behind the dump's back wall never spots), the dump "
                    "caches shot from the tested entrance pad, a current pad across a sandbag wall refused, and "
                    "revives only from a pad within Rare's revive reach (else written off for 20 s). No geometry "
                    "change. docs/TU3_BASE.md \"Desert pathfinding\"")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_sabotage_play_no_idle, false, "Gameplay",
                    "tu3-base, with pdz_sabotage_bots, (on in the release settings (played in the developer's matches since 2026-10-27)): no idle or "
                    "roaming attackers: the whole attacking team attacks; an attacker the plan leaves without a "
                    "piece (crowded out, avoided, just revived) takes the best piece with room at once; a gunless "
                    "attacker punches the nearest soft piece instead of roaming on Rare's goal 0; escorts and kit "
                    "carriers shoot soft pieces in a clear line on the way without changing their goal. "
                    "docs/TU3_BASE.md \"Sabotage Job 2\"")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_sabotage_play_snap, false, "Gameplay",
                    "tu3-base, with pdz_sabotage_people_stage1, (on in the release settings (played in the developer's matches since 2026-10-27)): an "
                    "attacker's snap shot only when he was shot in the last 3 s, the enemy is within 8 m, or he "
                    "already stands at his piece; 4 s between snaps; no freeze: on the move he keeps Rare's route "
                    "and brain and only aims and fires (at his piece he fires from where he stands). "
                    "docs/TU3_BASE.md \"Sabotage Job 2\"")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_sabotage_play_quiet_melee, false, "Gameplay",
                    "tu3-base, with pdz_sabotage_people_stage1, (on in the release settings (played in the developer's matches since 2026-10-27)): an attacker whose piece (or a piece on his way) is soft (crate, drum) punches it, a 5 m "
                    "noise instead of gunfire, only when he is within 4 m or its open pad within 6 m of his route, "
                    "no enemy he knows of is within 15 m of it or sees its open pad, and over 45 s are left; else he "
                    "shoots it. Never a cache, comms or the node; one puncher per piece. docs/TU3_BASE.md "
                    "\"Sabotage Job 2\"")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_sabotage_cue_log, false, "Gameplay",
                    "tu3-base, DIAGNOSTIC (off; Job 2d), with pdz_scenario_log: every multiplayer announcer line "
                    "played in Sabotage, with the round time, the local player's team (attacking / defending) and "
                    "what asked for it (Sabotage message, round-over check); at each round end, the voice line "
                    "Rare's check plays for that end. docs/TU3_BASE.md \"Sabotage Job 2\"")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_sabotage_play_weapons, false, "Gameplay",
                    "tu3-base, with pdz_sabotage_people_stage1, (on in the release settings (played in the developer's matches since 2026-10-27)): "
                    "weapons for the property: piece damage logged with each shooter's GunType; splash safety "
                    "(no rocket / grenade-launcher shot with a teammate within 6 m of the aim point or the aim "
                    "point within 7 m of himself, friendly fire or not); the best gun he carries for the piece "
                    "(launchers for hard pieces and clusters, rapid fire / shotgun for crates; 5 s between "
                    "switches, none while firing); a launcher holder goes for a cluster of pieces. "
                    "docs/TU3_BASE.md \"Sabotage stage 3\"")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(pdz_sabotage_reload_guard, true, "Gameplay",
                    "The 0.9.2 name of pdz_bot_reload_guard (0.9.2.1: every multiplayer match with bots), still accepted: "
                    "the one set by the higher source wins. The reload guard: a bot whose "
                    "gun reloads for 5 s without finishing, or starts a second reload animation without a shot (the gun cycling "
                    "10 -> 14 -> 10: release log 002, a bot locked reloading at a Subway fuel dump), gets one step per "
                    "trigger: the clip refilled Rare's AI way (when empty) and Rare's instant raise; then another gun "
                    "he carries with ammo; then his attacker job's fallback (melee at a soft piece, another spot at a "
                    "hard one). false = off (the test runs' before case). docs/TU3_BASE.md \"Sabotage: the reload guard\"")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_sabotage_play_shop, false, "Gameplay",
                    "tu3-base, with pdz_sabotage_bots, (on in the release settings (played in the developer's matches since 2026-10-27)): Sabotage bots "
                    "shop by role through Rare's own shop (82147390): its gun roll is answered with the role's pick "
                    "from the list Rare built (price, weapon set and owned guns already filtered); Rare gives and "
                    "pays. Attackers: a third demolition (rocket launcher, Devastator, SuperDragon, M16), the rest "
                    "crate breakers (rapid fire, shotgun); a demo-kit holder a cheap sidearm; defenders Rare's "
                    "random pick. docs/TU3_BASE.md \"Sabotage stage 4\"")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_sabotage_play_team, false, "Gameplay",
                    "tu3-base, with pdz_sabotage_people_stage1, (on in the release settings (played in the developer's matches since 2026-10-27)): "
                    "Sabotage team play: a kit carrier stops 12-30 m short of the power node and waits (8 s at most) "
                    "until 2-3 teammates are within 10 m (the two nearest gunners are called over), then pushes; "
                    "each attacker's first long leg goes through a via pad to one side (approach variety; the rule "
                    "is checked offline with sabotage_navcheck.py --via); escorts face outward while the carrier "
                    "plants. docs/TU3_BASE.md \"Sabotage stage 5\"")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_sabotage_play_squads, false, "Gameplay",
                    "tu3-base, with pdz_sabotage_people_stage1, (on in the release settings (played in the developer's matches since 2026-10-27)): Sabotage "
                    "attackers in 2-3 squads of 2-4 (new each round), each on a target area (a cluster of pieces or "
                    "the power node) picked by weighted chance; at most one squad on the node, with the stage 5 "
                    "gather; members keep loosely together on the way and take one piece each inside the area; "
                    "sometimes a lone wolf; squads' first legs from different sides. docs/TU3_BASE.md \"Sabotage "
                    "squads\"")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_sabotage_node_last_push, true, "Gameplay",
                    "tu3-base, with pdz_sabotage_people_stage1 (on; 2026-11-03, untested in play): Sabotage, when "
                    "only the power node stands for a team's attackers: every attacker with no piece of his own and "
                    "no carrier to escort joins the push at the node (Rare's guard goal at a pad by it: he fights "
                    "there, Rare's combat as ever) instead of roaming the map; the kit holders wait there and plant "
                    "in turn (a node takes three charges). false = before (they roam). docs/TU3_BASE.md \"Sabotage "
                    "node-last push\"")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_infection_32, false, "Gameplay",
                    "tu3-base, (on in the release settings (played in the developer's matches since 2026-10-27)): DarkOps Infection with up to 32 players offline (else it "
                    "keeps Rare's 16: 31 bots crashed it, logs 301-303). Rare's team brain bot list (16 records) "
                    "is filled with the first 16 bots of a team only, the first-infected pool (64) never takes "
                    "a 65th entry. docs/TU3_BASE.md \"Infection at 32 players\"")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_protection_objective_focus, false, "Gameplay",
                    "tu3-base, with pdz_protection_bots, (on in the release settings (played in the developer's matches since 2026-10-27)): Rare's CTF groups over each "
                    "team's helper bots: group 1 escorts its hacker (goal 8), group 2 patrols the live "
                    "terminal (Rare's guard goal 7), group 3 hunts the enemy hacker (goal 3) or roams. "
                    "docs/TU3_BASE.md, \"Plant the Flag and Protection: objective focus\"")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_ptf_objective_focus, false, "Gameplay",
                    "tu3-base, with pdz_mp_cut_scenarios, (on in the release settings (played in the developer's matches since 2026-10-27)): a Plant the Flag bot carrying "
                    "its flag keeps Rare's carry goal 5 (no other goal but death replaces it; Rare's chooser "
                    "then keeps it out of fights, it shoots while walking), stands still while it plants, "
                    "and Rare's guard goal 7 patrols the team's own base. docs/TU3_BASE.md, \"Plant the Flag "
                    "and Protection: objective focus\"")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_protection_bots, false, "Gameplay",
                    "tu3-base, with pdz_mp_cut_scenarios, (on in the release settings (played in the developer's matches since 2026-10-27)): Protection bots. "
                    "Bots can be picked as hacker (Rare's turn rule over every team member); a bot "
                    "hacker fetches its Data Thief and stays at the live terminal for Rare's bot hack "
                    "time (10 s), then scores through Rare's hack path; teammates escort it, hunt the "
                    "enemy hacker and guard the terminal (the Capture the Flag bot script's goals)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_minigame_log, false, "Debug",
                    "tu3-base: record every Data Thief and demo kit minigame (single player and "
                    "multiplayer) to logs/minigame_events.csv and logs/minigame_sessions.csv: "
                    "presses (hit / miss / wrong block), time per ring, result, damage taken")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_anim_log, false, "Debug",
                    "tu3-base: append every animation the game starts, once, to logs/anim_seen.csv "
                    "(kept across sessions; tools/anim_report.py lists the ones never seen). Records "
                    "character animations picked by type, looked up by name or played by scripts; "
                    "not weapon or prop animations")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_debug_anim_viewer, false, "Debug",
                    "tu3-base, debug: animation viewer. PageDown plays the next animation of the list on "
                    "the nearest non-player character (replayed until the next key), PageUp the previous "
                    "one, End resets (back to the start, the character returns to normal). Only character "
                    "animations, only on characters seen using them; nothing is saved")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_STRING(pdz_debug_anim_viewer_list, "unseen", "Debug",
                      "tu3-base: pdz_debug_anim_viewer's list: unseen (character animations not in "
                      "logs/anim_seen.csv yet, the likely-unused ones first), suspects (only the "
                      "likely-unused ones, docs/SP_INVENTORY.md), all, or any other text to list "
                      "the names containing it (e.g. idle_male_mp5)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_STRING(pdz_debug_ai_behaviour, "none", "Debug",
                      "tu3-base, DEBUG, not a fix: force every single-player enemy (not allies, "
                      "snipers, bosses, special-script or level-script characters) to one combat behaviour "
                      "when its fight starts: none, cover, cover_jump, dodge, position_and_shoot, "
                      "cover_regardless (cautious plan with the planner's unused 'stay in cover "
                      "regardless' actions), or the baselines all_out_attack, cautious_attack, "
                      "hold_position. See docs/SP_INVENTORY.md, 'Unused AI behaviours in depth'")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_ai_behaviour_log, false, "Debug",
                    "tu3-base: once a second per enemy in a fight, log its behaviour, plan goals, "
                    "cover use, dodges and hurt / shot-at state, and one line per ended move-into-"
                    "cover task (time settled, peeks, why it ended) (pdz_ai_behaviour: lines). "
                    "Diagnostic only")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_ai_shoot_from_here, false, "Gameplay",
                    "tu3-base, enhancement (off = Rare's behaviour): when an alerted single-player "
                    "enemy's planner finds no pad for any goal and it would stand in Rare's "
                    "do-nothing wait (5 / 10 s), let it attack from where it stands (Rare's own "
                    "close-plan attack step) while it can see its target within its weapon's "
                    "maximum range; it never moves, so area-tied guards stay in their areas. Not "
                    "allies, snipers, bosses, special-script, level-script or multiplayer "
                    "characters. See docs/SP_INVENTORY.md (why forced enemies had no plan)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_scenario_log, false, "Debug",
                    "tu3-base: log the cut scenarios (pdz_scenario: lines): Plant the Flag mode "
                    "created / first update / destroyed, flag pick-ups and drops, team scores "
                    "from the plant check, multiplayer announcer cues. Diagnostic only")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_sound_log, false, "Debug",
                    "tu3-base: log each sound cue the first time it starts playing in a session "
                    "(pdz_sound: lines, bank and cue name; cues no loaded bank has are marked). "
                    "Diagnostic only, to confirm which sounds really play")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_audio_presence_log, false, "Debug",
                    "tu3-base: for silent-area reports (pdz_audio: lines): each sound cue's first "
                    "start, refusal and stop (category, position), and every 5 s the four ambience "
                    "slots with their Ambivol volume, the music state and the sounds started. "
                    "Diagnostic only")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_fix_log, false, "Debug",
                    "tu3-base: every 10 s while a fix acts, log how often each ported game / AI "
                    "fix changed something since the last line (pdz_fix_log: lines). Diagnostic "
                    "only, to confirm the fixes fire in the TU3 build")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
