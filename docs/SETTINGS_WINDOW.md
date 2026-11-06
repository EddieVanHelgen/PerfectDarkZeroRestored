# Settings window, map packs and the release settings file (2026-11-04)

## The window (`src/setup/settings_window.cpp`)

A standard Windows window: native controls with the system's look (comctl32 v6 activated around this window only,
`src/pdz_comctl6.manifest`, resource 3).

**When it shows:**
- after the first-run setup, on the Map packs tab (the packs are setup's optional last step);
- on every start until "Don't show this again" is ticked (`pdz_settings_show`);
- when Shift is held at launch;
- with `--pdz_settings_open` (`PDZ Restored Settings.bat`).

Never in a developer's build or with `pdz_setup_quiet`. The close box quits; Play and Safe mode save and start.

**Tabs:**

| Tab | Settings | Writes |
|---|---|---|
| Display | Monitor, Borderless / Fullscreen, Resolution (Fullscreen only; Desktop by default), Render scale 100 / 75 / 50 %, VSync / VRR On / VRR (G-Sync / FreeSync) / Off (each with its own help line; Off warns: tearing and judder), Field of view 73-103 degrees across 16:9 with Default (Rare's) | `monitor`, `fullscreen`, `fullscreen_exclusive`, `resolution` (Fullscreen with a chosen size only), `pdz_native_scale` (75 / 50 % of the desktop's auto scale, nearest integer; 100 % = no line), `pdz_present_pacing` (fixed / vrr; a hand-set present / latch is kept by On), `vsync`, `pdz_fov_scale` |
| Frame rate | Campaign Auto / 120 / 60 (Auto: what this screen gets, in a line); Multiplayer 60, greyed out | `pdz_sp_120` |
| Map packs | each pack, its maps and status; Add / Remove; Add pack files...; Check installed packs | the packs (below) |
| Controls | mouse sensitivity, invert, Step zoom (off), one row per action with two keys each (rebind by pressing), Reset controls to defaults | `pdz_mouse_sensitivity`, `pdz_mouse_invert`, `pdz_zoom_steps` (only when changed) and the changed `keybind_*` lines in `mapping.toml` (comments kept) |
| Advanced | Open settings file, Open logs folder, Rebuild shaders (removes `generated.json`), Reset to defaults, Network play | `net_lan` |

- **Help:** the line at the bottom explains whatever control has the mouse over it or the keyboard focus (polled
  every 100 ms; no hover tooltips).
- **Its lines:** the window writes only the lines it owns: in place where they are, else under the release file's
  "Settings window" marker. It sets them for the current start too.
- **Windowed:** back in the Screen mode choice (2026-11-04: `--fullscreen=false` ran 150 s through the front end into Datacore with no crash, log PDZ_Job8 008; the NVIDIA crash noted in docs/MAP_PACKS.md was an older build's). It writes `fullscreen = false`.

**Test:** `pdz_settings_test_capture=<folder>` shows each tab, writes `tab<N>.bmp` and closes. All five tabs were
captured in the test folder.

## Controls tab (2026-11-04, evening)

- **Rows:** one per action, the game's name with the pad button in brackets (Fire (RT), Aim / zoom (LT), Reload (X),
  Use / open / take cover (A), Crouch / stand (L3), Dodge roll (LB), Melee (B), Select weapon (Y), Secondary fire
  mode (RB), Pause (Start), Objectives / scores (Back), the four moves, the four D-pad gadget slots, the four keyboard
  look directions, Right stick click (R3)). PDZ has no jump and one weapon button (no next / previous).
- **Rebind:** click a row's Key 1 or Key 2 cell (or press Enter on the row: Key 1; or the Change key 1 / 2 buttons),
  then press anything: every key (by its scan code, as SDL will see it: `rex::ui::Win32KeyMessageToScancode`), mouse
  buttons 1-5, buttons 6-32 (Raw Input's raw button state, registered only while waiting), the wheel up / down and
  tilt left / right. Esc cancels; so does switching away. Delete or Clear empties the row.
- **Names:** `rex::ui::InputCodeDisplayName` ("Left Ctrl", "Mouse 4 (back)", "Wheel up"); the file gets the bind token
  (`InputCodeToken`: "LControl", "Mouse4", "WheelUp").
- **Clashes:** an input another action has asks: Yes swap (the other action gets the old key), No use it for both,
  Cancel.
- **Two keys:** a value with more than two (the default Select weapon `Q,WheelUp,WheelDown`) shows the rest in Key 2;
  setting Key 2 replaces them with one.
- **Saved** with Play / Safe mode: only the keybind lines that changed, in place with their comments; set for this
  start too. Reset controls to defaults restores job 2's keys, sensitivity 2.0 and invert off (saved with Play); the
  Advanced tab's Reset also resets them, at once.
- **Test:** `pdz_settings_test_rebind="keybind_a:2=XButton2;keybind_x=Key:11:1D:ext;keybind_back:2=Raw:6;..."` runs
  the rebinds through the capture path (posted Windows messages; Raw:<n> feeds the raw button state), answers clashes
  with `pdz_settings_test_answer` (swap / both / cancel), logs every step and the final rows, then presses Play. It
  shows the window even with `pdz_setup_quiet`.

## Frame rate on the display (`game_pace.cpp`, `pdz_display_switch_120`)

**VSync / VRR (2026-11-06): no Auto.** Auto chose VRR whenever the display's EDID advertised it (HDMI VRR,
FreeSync, Adaptive-Sync), but the EDID can't tell whether VRR is switched on in the graphics driver: in the
new-player test a FreeSync EDID with VRR off in the NVIDIA driver gave 117 fps Present(0) on a fixed 120 Hz display,
which juddered; `fixed` was smooth. So:
- **On** = `pdz_present_pacing = "fixed"` (the default and the release file's value): locked to the display's refresh.
- **VRR (G-Sync / FreeSync)** = `"vrr"`: only if VRR is on in the driver for that display.
- **Off** = `vsync = false` (the pacing stays fixed); its help line is the tearing warning.
- An older settings file's `"auto"` is read as `"fixed"`, with one `pdz_pace:` line saying so.

With pdz_sp_120:

| Display | On (fixed) | VRR |
|---|---|---|
| 60 Hz | 60 | (needs 119 Hz or more) |
| 120 Hz | 120 | 117 (capped under the refresh) |
| 144 / 165 Hz | the even division at most 120: 72 / 82.5 | 120 (capped) |
| 240 Hz | 120 (240 / 2) | 120 (capped) |

The switch of a 144 / 165 Hz display to 120 Hz (`pdz_display_switch_120`) only ran under Auto, so it no longer
happens; set the display to 120 Hz in Windows for 120 fps with On.

The choice is logged (`pdz_pace: display N Hz: ...`), and so is a start-up summary:
`pdz: this PC: GPU ... | display ... Hz, VRR ... | pacing ... | CPU N cores, M threads | second record thread ...`.

## Map packs in the exe (`src/setup/map_packs.cpp`)

**The port:** `tools/install_map_packs.py` + `tools/stfs.py`.
- **Steps:** STFS read-only; the known Map Pack One repairs (Plaza 1 byte, Trench 2 bytes); every file against the
  package's SHA-1 block hashes; every `.pkg` inflated with its Adler-32 trailer checked.
- **Where:** a pack that verifies completely goes to `<user data>\0000000000000000\4D5307D3\00000002\<package file
  name>\`, where TU3 lists it (`XamContentCreateEnumerator` includes shared content); `pdz_install.txt` holds each
  file's SHA-1 for "Check installed packs".
- **A failure:** a pack that fails is left as `<name>.failed` with the reason.
- **From a script:** `pdz_map_packs_install = "<file>;<file>"` installs at start-up.

**Tests:**
- **Install from your copies (read only):** Map Pack One from `<your Map Pack One folder>`, installed
  repaired. Map Pack Two from its own folder, every file verified. Your files were unchanged.
- **Against the old script:** all 78 + 58 installed files are byte-identical to `install_map_packs.py`'s install.
- **Front end:** with the packs, 0 shader misses. Without them, the original maps; every other test ran without
  packs.

## The release settings file (`settings/release/hardware.toml`)

Shipped as `settings\hardware.toml` by `stage_release.py`. It is the committed template plus:
- `log_level = "info"`;
- frame timing off;
- `pdz_cutscene_smooth = false`;
- `net_lan = false`;
- the developer's multiplayer play set: Plant the Flag, Protection and Sabotage with bots and the Sabotage stages;
- `pdz_infection_32`;
- `pdz_mp_announcer_extras` (now a fix, on in code too);
- the window's section.

The aim enhancements are unchanged (on). `net_lan` (SDK) off binds every socket to 127.0.0.1.

`stage_release.py` refuses:
- launchers other than `PDZ Restored Settings.bat` and `PDZRestored (Safe mode).bat`;
- any audio file;
- anything from `MyMusic`.

## Field of view (2026-11-04, `src/mod/fov_tuning.cpp`)

- **What it scales:** `pdz_fov_scale` multiplies the zoom factor the gameplay camera asks for (`sub_825AC8E0`, its
  call returning to `822156D4`), so the camera's FOV is 58 degrees (Rare's 4:3 horizontal) x zoom factor x scale.
  Across a 16:9 screen that is 2 atan(4/3 tan(29 deg x scale)): **72.9 degrees** at 1.0 (Rare's), 103.4 at 1.5.
- **The slider:** whole degrees from 73 (Rare's, written as 1.000) to 103 (scale 1.494); "Default (Rare's)" moves it
  back. Saved as `pdz_fov_scale` in the settings file; the Advanced tab's Reset puts 1.0 back.
- **Zoom:** a scope's or step zoom level's FOV is widened by the same factor, so every zoom keeps its magnification
  relative to the unzoomed view (not Rare's absolute zoomed FOV). The mouse's zoomed turn rate (`mouse_look.cpp`)
  uses the same widened angles, so it stays in proportion.
- **Not scaled:** every other caller of the zoom factor and every other camera (vehicles, turrets, cutscenes,
  menus): `pdz_fov_log` lists the callers with counts every 10 s.
- **Test hook:** `pdz_settings_test_rebind="pdz_fov_degrees=90"` moves the slider through its own notification.

## The look (2026-11-04, `src/setup/theme.cpp`)

- **Palette:** navy window #050B1E, panels #0E1C44, side bar / help strip / footer #081230, title bar #030714 (DWM,
  Windows 11), borders #23407F, text #E4EAF4 / #A2B1CF, selected choices and switches #1F5FE0, the selected tab's bar
  and info notes #5FD3FF, warnings #FF8A50.
- **Buttons:** Play (and a question's main answer) orange gradient with navy upper-case Rajdhani; Safe mode and Cancel
  brushed silver; Remove / Reset orange outline; Add blue; the rest panel buttons. All owner-drawn real buttons, so Tab,
  Space and Enter work as before; a cyan ring shows the focus.
- **Controls:** side bar tabs, segmented choices (Screen mode, Render scale, VSync, Campaign, Multiplayer), switches
  instead of check boxes, a themed slider.
- **Drop-down lists** (Monitor, Resolution, 2026-11-04 polish): drawn by us, a panel with a cyan chevron; the open list
  (`OpenDrop`) is a drawn popup in the same colours, the item under the mouse / the keyboard in blue. Mouse, wheel,
  arrows / Page Up / Down / Home / End, Enter or Space to choose, Esc / Tab / a click outside to close; a controller's
  up / down / A / B too.
- **The key list:** still a Windows list view (rebinding, hit tests), drawn in the theme: the header in the side bar's
  colour with the column names in silver Rajdhani capitals (15:1 contrast), a line under each row and between the
  columns, every other row slightly lighter, the selected row blue with white text, and our own scroll bar (the list's
  own is clipped off by its window region; ours scrolls it: drag, click above / below the thumb, the wheel).
- **Map packs:** the status keeps "Installed (repaired)" across starts: the install record (`pdz_install.txt`, first
  line `repaired 1`) says so. Packs installed by the old `tools/install_map_packs.py` have no record and show
  "Installed"; Remove and Add again writes one. Colours: installed cyan, not installed dimmer, didn't verify orange.
- **Field of view in tests:** `pdz_settings_test_capture` shows Rare's 73 degrees unless its rebind script sets
  `pdz_fov_degrees`; a fresh settings file has `pdz_fov_scale = 1.0`.
- **Ids:** the side bar tabs use 180-184 (in the restyle they were 120-124 and pushed Monitor to 121, so the Monitor /
  Resolution lists drew as empty tabs and Screen mode / Render scale choices switched tabs; fixed). `click=<id>` in
  `pdz_settings_test_rebind` clicks a control (a segment's id: 2000 + group x 8 + index).
- **Fonts:** Segoe UI for text, Rajdhani Bold (SIL OFL, `licenses/rajdhani`) for the title, headings and Play, embedded
  in the exe (resource 10).
- **Our message box** (`theme::ShowMessage`) replaces every Windows message box of the settings window and setup, same
  text and buttons; the setup progress window is redrawn the same way. The Windows file picker stays Windows'.
- **Controller:** up / down move the focus, A presses, B cancels a rebind or a question, LB / RB change tabs, left /
  right change a segmented choice.
- **Screenshots:** `docs/settings_window/` (`pdz_settings_test_capture`, `pdz_setup_test_capture`; `_100pc` /
  `_150pc` with `pdz_settings_test_dpi` 96 / 144: the window's own layout at that scale, but Windows still draws
  title bars and scroll bars at the screen's scale, so the key list's scroll bar there is too wide).
