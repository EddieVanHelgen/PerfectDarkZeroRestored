# Map packs and Title Update 3 on the launch build

Goal: play the official multiplayer map packs (Map Pack One: Plaza, Rooftops,
Trench, GasPlant; Map Pack Two: Facility, Ruin) on the launch (day-one)
executable, keep the original music, and know what the title update fixed.
Category: official content and official fixes count as restoration. Status
(2026-10-19): option B implemented behind `pdz_dlc_maps` (on by default),
play-tested: all six maps load, play and exit, with their map pictures.
Without the packs installed the game shows the original maps.

## Our executable

`assets\default.xex`: media ID **6A78E206**, version **0.0.0.4**, title
4D5307D3. The README's media ID (`...-750FF1D9`) is a different disc: the
title update for 750FF1D9 is a delta from version 0.0.0.10 and does not apply
to ours. The matching update is Title Update 3 for media ID 6A78E206
(`TU_16L61UJ_000000G000000.00000000000O4`, delta 0.0.0.4 -> 0.0.3.4,
source digest = our executable). Both packages carry identical data files.

## Findings: SDK and launch game

- `XamContentCreateEnumerator` lists **folders** (not STFS files) under
  `<root>/<XUID>/<title>/<type>/`; `XamContentCreate(Ex)` mounts such a folder
  as `<root name>:`. Marketplace content (type 2) uses XUID 0. Content root
  for this app: `%USERPROFILE%\Documents\perfectdarkzerorecomp`. An STFS
  reader and `ContentManager::InstallContent` exist but nothing calls them.
  `XamContentGetLicenseMask` returns the `license_mask` cvar (0); install,
  resolve and mount-package calls are stubs.
- **The launch game has no DLC code path.** Its content enumerators are only
  for saved games; maps come from the fixed level list
  `aid_xml_level_levellist` (`sub_82272A28`): 14 single-player levels and 7
  multiplayer maps. Packages load by name hash only from
  `game:\ASSETS\PackageData\<hash>.pkg` (`sub_821EE918` / `sub_821EE9E0`).

## Findings: the map packs (extracted read-only to the scratchpad)

- Laid out like the game's own assets: `Pdz/dlc_contents/default.bin` (the
  pack's level list, in the title update's longer record layout),
  `Pdz/Assets/PackageData/*.pkg`, `Pdz/Assets/audio` (cue list
  `cuelist_mappackN.txt`, new ambience banks `music_mlt_gas`, `_plz`, `_alc`),
  `Pdz/Assets/loc/<9 languages>/`.
- Level ids: trench 0x0C, rooftops 0x0D, plaza 0x0F, gasplant 0x10 (pack 1);
  alcatraz = Facility 0x21, temple64 = Ruin 0x23 (pack 2). No clash with the
  launch multiplayer ids. Pack 2 also carries unlisted villa / skedar data
  (cut maps; leave out). No package name collides with the launch packages.
- The maps also use launch ambience banks; the cue list format matches the
  launch `cuelist2.txt`. The packs carry their own copy of the title update's
  remote mine settings (`guncontrolmultimine.xml`). Two pack-one packages
  were damaged in this copy (repaired, see Install).

## Findings: Title Update 3

Method: the SDK's codegen applied the delta patch to a copy in the
scratchpad (never under the repo: the runtime applies any `default.xexp`
next to `default.xex`), then every function was matched against launch.

| | Count |
|---|---|
| identical (most moved: the update inserts code early in the image) | 65,659 |
| offsets / constants only (many player fields moved by +4) | 2,734 |
| changed code | 856 |
| new / removed | 906 / 525 |

### Data files the update adds

- `PackageData/1726605008.pkg`: multiplayer collision and prop fixes
  (chinatown, town, urban, jetpack, desert, africa, subway, temple,
  towersmall) **plus single-player `town` props**, bot UI and personalities,
  tournament / leaderboard / lobby screens, ammo types, remote mine.
- `PackageData/393969562.pkg` (266 bytes), per-language `968010180.pkg`
  (title textures), `frontendTU1` / `generalTU1` strings (bot personalities,
  Map Pack One map names, "purchase the map pack", version-mismatch
  messages).

### Changes by system

| System | What changed | Confidence |
|---|---|---|
| **Music** | The interactive music player (Silence / Stealth / Speedup layers, launch `82200060`-`822010B0`, ~50 functions) is **gone**. The music type change (`821F8B28` -> TU `82200C68`) no longer stops or starts it, the level start (`821F1470` -> `821F8E40`) starts only Tune1-4 cues, the music tick and pause / resume were removed. Levels and scripts that ask for Interactive music get silence. | read, both versions compared instruction by instruction; not heard in game |
| Dub lines | The wavebank loader (`821F1E48` -> `821F9888`) drops its own "try the default language directory" fallback and calls a new resolver (`821FA380`) that searches mounted content first | lead only |
| **DLC** | New: file resolver `821FA380` (mounted packages, then `game:`), package path builder `821F6040`, level-start pack list load `8251A1C8` / `821F8810`, mount / registration `82510300` (-> `822027C8`, `8227A648`); level list `82272A28` changed | read (structure), not traced in detail |
| **Cover** | New player timer (+7520): 1.0 s, set on cover exits (normal `824F11C0`, forced `824F1610`, and in `824F2E20`), counted down each update; the cover scan and prompt (`sub_824F4AB8`) don't run until it reaches 0. A cooldown before cover can be entered again after leaving it: most likely the "cover wall clip" fix (knocked out of cover, re-entering at once) | read; effect not tested |
| Turret / weapons | Launch's player update has two re-equip blocks: flag +7664 equips GunType_Turret_Shell (59), set by the vehicle seat handler `825AE258` for occupant type 2; flag +7665 equips type 16, set by multiplayer mode code. TU3 keeps the type-16 block (flag now +7668) and moves the Turret_Shell block into the base character update (`8253CEE0` -> T `8254C5A0`, flag +6171), set for occupant type 2 **or 42** and by new AI-side setters. So it extends the turret weapon swap to type-42 occupants (likely bots); players behave the same. The hardpoint turret update (`8257D618`) is unchanged. Not the Plasma Rifle 10x fix | read (earlier "59 -> 16" was a diff artefact) |
| Player | Damage (`8259B738`) takes health from a virtual call; spawn resets new fields | read |
| AI / bots | AI update (`8251DB70`) gains a range / team test setting a flag (probably bot update throttling); bot personality UI | read |
| Multiplayer | Game modes (infection, sabotage, onslaught, CTF, data thief, eradication), lobby, tournaments, networking, save / profile | read (biggest areas), not detailed |
| Single-player scripts | Mostly register reallocation; the `82265DE8` block. The LV12 Jonathan script (`823645C0`) has no behaviour change (corrected 2026-10-20: TU3's compiler duplicated a shared tail; BUG_AUDIT U10) | read |
| Unchanged | Spread (`822397C8`), rotational assist (`8259FD70`), AI cover task (`821C6AC8`): constant-pool / scheduling only | read |

Cross-check with `docs/BUG_AUDIT.md`: CV9 (cover wall clip, patched later)
matches the cover cooldown; WP14 (Plasma Rifle / turret, TU1, cumulative in
TU3) does not match the re-equip change (player behaviour is the same); the
music / dub breakage
(Known bugs) is explained for music. None of our game fixes (H5, spread cap
order, CMP noise) were fixed by Rare: those functions are unchanged.

## Implementation (option B): `pdz_dlc_maps`

Approved plan B: stay on the launch executable and port what the maps need
from Title Update 3 as hooks (`src/mod/dlc_maps.cpp`). `pdz_dlc_maps`
(default `true`, restart to apply) is in the "Official content restoration"
section of `hardware.toml`; `pdz_dlc_log` writes `pdz_dlc:` lines. With
`pdz_dlc_maps` off, or on with no pack installed (no folder with
`Pdz\dlc_contents\default.bin` under `...\4D5307D3\00000002\`), every hook
runs the original code: the launch map list, nothing mounted or moved.

| TU3 (T address) | What it does | Launch port |
|---|---|---|
| UpdateDLCState `8250FF08` | Enumerates marketplace content, mounts each pack with `\pdz\dlc_contents` as `CONTENT<user><n>:`, loads its manifest | Lists the pack folders on the host and mounts them read-only as `CONTENT0<n>:` (as the SDK's XamContentCreate does). Title Update data goes on `UPDATE:` |
| File lookup `821F6040` (replaces `821EEE60`) | Each pack, then `UPDATE:`, then the game; plain, language and default-language paths | Hook `sub_821EEE60`, same order |
| Path rule `82225280` (replaces `8221DFF0`) | `UPDATE:\` and `CONTENT...` paths are absolute | Hook `sub_8221DFF0` |
| Manifest `aid_dlc_info` (new asset kind `dlc`) | Compiled level list `\pdz\dlc_contents\default.bin` | Read on the host (launch has no `dlc` kind): per record level name, id, kind; first record: general / frontend string tables and slots, package base, cue list |
| `82510300` / `82510468` | Loads `<base>System` (heap 3, category 3) and `<base>Frontend` / `Frontend_loc` (heap 8, category 4) | Same names, heaps and memory categories |
| Merge `8227A648` | Appends levels to the level list | After `sub_82272A28`: append entry, leveldata, extra record; per-kind id tables. Lockoffs (`823377B8`) and the menus read the list afterwards |
| Strings `8239BA70`, `8239B6D8` | Pack tables in slots 18-21, TU tables (`aid_xml_dlc_locassets`: generalTU1 8, frontendTU1 9); 38 slots | After `sub_82393540`; slots 7-37 in our own array, lookups `sub_82393AD0` / `sub_82393B30`. A table no pack has is skipped (the launch loader crashes on a missing table; TU3 added a check) |
| Cue lists `822027C8` | Pack cue lists through the lookup | Right after the game's own lists (`sub_821F22D0`, inside the audio init), with the launch loader `821FA770` |
| Sound bank table (audio init `821F8810` clears 1788 bytes, launch 1020) | Room for ~149 banks (launch: 84, the game uses 82) | Before the audio init (`sub_821F0E58`) the manager moves to a zeroed 4 KB block (1084 bytes copied; checked for self-pointers), 335 slots; the packs' banks start at 86 (85 would overlap the fields after the table). The game's banks keep their numbers |
| Soundbank loader `821F9A00`, unloader `821F9B20`, idle check `821F9290` | Search / clear / stop the larger table | The launch versions stop at entry 84 (`sub_821F2050` cmplwi 85, `sub_821F21A8` li 85, `sub_821F1888` cmpwi 1020); hooks do the same for entries 86+. Without the idle check one, a pack's ambience never stops and leaving the map hangs (the state machine `sub_822261E0` waits for silence before the level teardown) |
| Prop template master list (`1726605008.pkg`) | Launch's list + 9 entries | The 8 for the packs' props (4 Facility doors -> `door_facupdown`, 4 teleporters -> `electrical_teleport`), inserted after the list (`sub_8251ADC0`, node pool grown in `sub_82623748`). Without them: no door air hiss, no teleporting. The 9th, Desert's hit-fix prop `thing -> example`, is added by `pdz_fix_tu_mp_hitfix` (U18) |
| Per-level strings `8239B830` / release `8239C110` | Multiplayer: `aid_strings_text_multi_<map>` in slot 7 (area names under the map) | After `sub_82393780` (heap 8, only tables a pack has) and `sub_82393F08` (release). Without it the label reads `_no string!_` |
| Map pictures (T `823FFE58`, new DarkXUI control) | Shows a level's info scene (leveldata +0x9C, `aid_darkxui_frontend_info_level_<map>` in the pack's Frontend package: the map's image and name) | The launch map picture is an XUI tab scene with one child per map, `Tab<level id + 1>`; the tab switch (`sub_82792BB8`) finds nothing for a pack id. Hook on the child lookup (`sub_8277ADB8` from `82792C8C`): for a pack level only, the info scene is created (`sub_82399048`, base path `sub_823A2C00`) and added (`sub_8277A658`) as that tab, one per tab scene (the previous one destroyed with `sub_827768C8`), as TU3's control does. The launch maps' tabs are untouched |
| Wavebanks `821F9888` | Lookup replaces the language fallback | **Not replaced**: the launch loader keeps its fallback; underneath, a file under `ASSETS\audio\` that CreateFileA (`sub_82AC3DA8`) can't find in the game is taken from a pack |

Kept as launch: the interactive music system (TU3 removes it), the wavebank
language fallback, everything else in the update.

Boot test (2026-10-19, fullscreen, `pdz_dlc_maps` on): 2 packs mounted, 6
maps added (43 levels), System and Frontend packages, leveldata and extra
records, 6 string tables and 2 cue lists loaded, no errors.
`Frontend_loc` isn't in the packs for English (expected).

Play test (2026-10-19, logs 100-105): title menu, single-player interactive
music (Trinity, Mansion), Plaza, GasPlant, Facility, Ruin, Rooftops load and
play with their ambience; Facility door hiss as on Xbox; area names under the
map; leaving Facility, GasPlant and Plaza returns to the menu (pack banks
stopped and unloaded). Trench hung on its loading bar until its damaged
package was repaired (see Install).

Note (2026-11-04: no longer seen; the restyle job's windowed run, log PDZ_Job8 008, was clean): windowed mode (`--fullscreen=false`) crashed in the NVIDIA driver
(`nvwgf2umx.dll` +0x140256, during present) with `pdz_dlc_maps` on or off;
unrelated to the packs.

## Install

You need your own copies of the two map packs (STFS package files, e.g.
copied from a console's `Content\0000000000000000\4D5307D3\00000002\`) and
Title Update 3, copied from your own Xbox 360 (any disc's: the data files are
the same; file names start with `TU_16L61UJ_`). One command installs, repairs
and verifies everything; the packages are only read:

```
python -I tools/install_map_packs.py <Map Pack One package> <Map Pack Two package> ^
    --update <Title Update 3 package>
```

For example, with copies from a console:

```
python -I tools/install_map_packs.py ^
  "<your Map Pack One folder>\4D5307D3\00000002\28A3533063A3643DD1AF810EE03BFEA893A7C9D74D" ^
  "<your Map Pack Two folder>\4D5307D3\00000002\7841AEFC8C33B6D0BECAF57E4DD4DBAECD104C734D" ^
  --update "<your TU3 folder>\<your Title Update 3 file>"
```

Per pack it extracts every file to the folder below (folder name = package
file name), applies the known repairs (see Repaired packages) when a file
has the damaged bytes, checks every file against the package's SHA-1 block
hashes and inflates every `.pkg` (Adler-32 trailer). Only a pack that
verifies completely is installed; otherwise it is left as `<name>.failed`
and the script exits with 1. An installed folder is only replaced with
`--replace`; `--root` installs elsewhere. From the title update it copies
only the `generalTU1` / `frontendTU1` string tables and its data package
`PackageData\1726605008.pkg` (used by `pdz_fix_tu_township` /
`pdz_fix_tu_mp_hitfix`, see Title update data), never its `default.xexp`
(the runtime applies any `default.xexp` next to `default.xex`, so it must
never go under the repo). Tested 2026-10-19: the result is identical to the
play-tested install (154 files); 2026-10-20 the update folder was
reinstalled with the data package (19 files, all verified).

By hand: `python -I tools/stfs.py <package> <folder>` extracts a package,
`python -I tools/stfs_verify.py <package> .pkg` checks a package's files
against its SHA-1 hash table.

### Repaired packages

| File | Map | STFS block | File offset | Damaged | Original |
|---|---|---|---|---|---|
| `1994253005.pkg` | Plaza | 8608 | 6618007 (block offset 2967) | `88` | `AB` |
| `454830383.pkg` | Trench | 44745 | 48589206-48589207 (block offsets 2454-2455) | `00 00` | `3A 3E` |

Both were found by brute force against the block's SHA-1 stored in the
package (one byte for Plaza; a 2-byte burst for Trench, which a bit-flip
search misses). With them the blocks match their hashes, each file's own
Adler-32 trailer verifies and the installed files are Rare's.

### Layout

The SDK lists marketplace content as extracted folders under the user data
root. The packs are installed once for every local profile (the content
folder of the base root, not the per-profile one):

```
Documents\perfectdarkzerorecomp\0000000000000000\4D5307D3\
  00000002\28A3533063A3643DD1AF810EE03BFEA893A7C9D74D\Pdz\...   Map Pack One
  00000002\7841AEFC8C33B6D0BECAF57E4DD4DBAECD104C734D\Pdz\...   Map Pack Two
  000B0000\TU_16L61UJ_000000G000000.00000000000O4\Pdz\Assets\loc\<9 languages>\
      aid\strings\text\generalTU1\default.str
      aid\strings\text\frontendTU1\default.str
  000B0000\TU_16L61UJ_000000G000000.00000000000O4\Pdz\Assets\PackageData\
      1726605008.pkg
```

The pack folders are the STFS packages extracted as they are (folder name =
package file name). From the title update only its two string tables (pack
one's map names are in `frontendTU1`) and its data package are installed.

### Title update data (pdz_fix_tu_township, pdz_fix_tu_mp_hitfix)

`1726605008.pkg` is `aid_darkpackage_titleupdate`. TU3 loads it at boot
before every level package (T `821411A8`), so its records win over the level
packages' (registration `sub_821F0410` skips a name that is already
registered; unloading only removes a package's own records). We load it
from `UPDATE:` right after the level list (the `sub_82272A28` hook; `UPDATE:`
is mounted and looked up even with no pack installed), but only these
records are registered (a mid-function hook before `821F04D4` makes every
other one count as already registered, so the game skips it):

| Fix | Records |
|---|---|
| `pdz_fix_tu_township` (U12) | `aid_xml_proplists_single_town` (launch's list + `static1142002086`), that static, `..._props_hitfix_town` (`TownDoubleHitFix`), and the 7 `aid_texture_replacement_township*` textures (Rare spelled the second `tonwship2`) |
| `pdz_fix_tu_mp_hitfix` (U18) | `aid_xml_proplists_multi_chinatown` (+ `static1141219760`, `static1143197731`: `..._hitfix_urban`, `..._hitfix_chinatown`) and `..._multi_desert` (+ `static1139304392`: `..._props_thing` at (-1358, 2284, -3907)), the models, and the master-list entry `thing -> aid_xml_proptemplate_example` (TU3's 9th entry) |

Left to the game (76 records in the 2026-10-20 boot log): the bot,
tournament, lobby and stats screens, `aid_xml_xui_options` / `_names`, the
remote mine (`aid_xml_guns_multimine`, U20), explosion / OBB volumes, the
Temple sandbag static and two Chinatown route points (U27). The Township
textures are written into the background's two instances (background
manager `[82D6B2BC]` +20 / +24, instance +4, fields +1052, +1056, +1060,
+1028, +920, +916, +1032, as T `82276D78`) after the background setup
(`sub_8260B9B0`) on level load and restart when the level's background is
`aid_darkbackground_single_town`. Boot log line: `pdz_fix_tu: title update
package: 19 record(s) taken ...; Township textures 7/7`. The records are
taken at boot, so turning the two options (or F11) off and on needs a
restart for the props; the textures follow F11 at the next load / restart.
No `.header` files are needed: the packs are mounted directly, and the launch
game never checks a licence. Originals stay where they are.

### The damage and why the 360 runs it

**The Map Pack One copy used here is damaged** in two 4 KB blocks (no block
in the file matches their stored hashes), one in Plaza's level package
`1994253005.pkg` (block 8608) and one in Trench's `454830383.pkg` (block
44745). Their data still inflated, but wrong (hence the bad Adler-32
trailers), and Plaza crashed while loading its package: the package walker
(`sub_821ECDA8`) read more entries than the damaged directory had and
string-compared a null type name in the asset kind lookup (`sub_821EE850`).
Plaza's block was repaired in the installed copy by one byte (offset 2967,
0x88 -> 0xAB, found by searching against the block's SHA-1; with it the
file's original Adler-32 trailer verifies), so it is now Rare's file.
Trench's block had two adjacent bytes zeroed (block offsets 2454-2455, file
offset 48589206: 00 00 -> 3A 3E, found the same way); repaired likewise, the
installed copy is Rare's file. All other package files verify.

The user's console copy of Map Pack One is byte-identical to this one, and
Trench loads on a modded 360 with Title Update 3. TU3 has no tolerant path:
its inflate, zlib reader and package walker are the launch code (only
storage-removal handling changed). In the damaged Trench the inflate still
runs to the end but yields 2 extra bytes (inside DXT texture data in
`.gpu`), so the rest of `.gpu` and all of `.stream` are 2 bytes late. The
model record walker (`sub_826FE2D0`, from model setup `sub_826FA2F0`) then
reads a `.stream` record block from the wrong bytes, walks off the
section's end into zeroed memory and spins on a size-0 record: the loading
hang. On the 360 the same walk most likely ends by chance in leftover
non-zero memory, with shifted textures and model data.

## How to test

`pdz_dlc_maps` is on by default (for diagnosis also set `pdz_dlc_log = true`,
`log_level = "info"`); start the game.

1. The log lists `pdz_dlc: ... 6 level(s) added (43 in the list)` and the
   string tables / cue lists.
2. Multiplayer setup (Combat / DarkOps), map list: the new maps follow
   Towersmall in id order: Trench, Rooftops, Plaza, GasPlant, Facility, Ruin,
   then Random. Check each name and picture (see Map pictures).
3. Play each map for a minute (bots on): loads, spawns, ambience, bots move and
   fight, vehicles where the map has them.
4. Split-screen on one DLC map.
5. Single-player: one launch level with `pdz_music_log = true`; the
   `pdz_music:` lines should show the interactive levels (Silence / Stealth /
   Speedup) changing as before.
6. On each pack map: the area name under the on-screen map (not
   `_no string!_`), Facility's doors hiss, teleporters work, and leaving the
   map returns to the menu (with `pdz_dlc_log`: `soundbank in bank <n>
   stopped`, then `unloaded`).

## Ambience banks

The packs' new sound banks are ambience loops, not music. Exported with
`tools/xwb_export.py` (WAV per cue in `music_export\mappacks\`, gitignored;
`index.txt` lists them):

| File | Cue | Length | Format | Character |
|---|---|---|---|---|
| `music_mlt_gas_gasplant_ambi.wav` | `gasplant_ambi` | 90.0 s | 4 ch, 22050 Hz | ambience loop (no steady beat, loop region set) |
| `music_mlt_gas_gasplant_ambi_cave.wav` | `gasplant_ambi_cave` | 66.5 s | 2 ch, 22050 Hz | ambience loop, quiet (mean -36 dB) |
| `music_mlt_plz_plaza_ambi.wav` | `plaza_ambi` | 68.5 s | 2 ch, 22050 Hz | ambience loop (most rhythmic of the three, still below music) |
| `music_mlt_alc_alcatraz_ambi.wav` | `alcatraz_ambi` | 66.5 s | 2 ch, 22050 Hz | identical to `gasplant_ambi_cave` |
| `music_mlt_alc_wave0.wav` | (no cue) | 90.0 s | 4 ch, 22050 Hz | identical to `gasplant_ambi`, unused |

`music_mlt_alc.xwb` holds exactly the same audio as `music_mlt_gas.xwb`
(renamed bank).

What each map plays (its music setup, confirmed with `pdz_music_log`):

| Map | Cues |
|---|---|
| Plaza | `PLZ_PLAZA_AMBI` + launch `SUB_SUBWAY_AMBI_SPAWN` |
| GasPlant | `GAS_GASPLANT_AMBI`, `GAS_GASPLANT_AMBI_CAVE` + launch `TWR_TOWER_AMBI_SPAWN` / `_EXT` (the Tower cues are in GasPlant's own setup, so intended) |
| Facility | `ALC_ALCATRAZ_AMBI` twice (wolves, dripping taps) + launch `TWR_TOWER_AMBI_SPAWN` / `_EXT` |
| Ruin | launch `TMP_TEMPLE_AMBI_INT` / `_EXT` / `_SPAWN` |
| Rooftops | launch `LV03_ROOFTOPS_AMBI_WIND` / `_CITY` + `SUB_SUBWAY_AMBI_SPAWN` |

The character comes from the export's beat measure,
calibrated on launch banks: single-player score (`music_lv00`, 44.1 kHz
stereo) 0.39-0.50, launch multiplayer ambience (`music_mlt_twr`, 22 kHz)
0.06-0.23, pack cues 0.06-0.27. Same formats as the launch ambience banks.

## Facility / Ruin rules (pdz_fix_tu_pack_map_rules, U22)

Done 2026-10-20 (was the open item "a different menu path (screens 19 / 34
instead of 18 / 33) ... options 61-63": those were value lists, and the
options are MODE 60 / SCENARIO 61 / DARKOPS_SCENARIO 62 in launch's
numbering, TU3's +1). TU3's option router (T `823BA378`) gives these lists
when the chosen map is Facility or Ruin (decoded from launch's and TU3's
`options.xml`):

| Option | Launch / other maps | Facility / Ruin (TU3) |
|---|---|---|
| SCENARIO (61) | [0, 1, 2, 4] | [0, 1] |
| DARKOPS_SCENARIO (62) | [0, 1, 2, 3] | [0, 2] |
| PLAYERS_MAX (5) | 2-16 (by connection type) | 2-8 |

Launch's list table is full, so the four list readers (`sub_823B2C28`
count, `823B2D08` first, `823B2D90` last, `823B31A0` value at index) answer
these three options from host lists while Facility or Ruin is chosen; the
game's validator then clamps a value that isn't in the list, as in TU3.

Random: launch's picker (`sub_823B5288`) only takes positions 0-8
(`823B52BC cmpwi r4,8`), which with the packs' maps (positions 0 subway,
1-5 launch maps, 6 Trench, 7 Rooftops, 8 Plaza, 9 GasPlant, 10 Facility, 11
Ruin, 12 Random) means GasPlant, Facility and Ruin were never picked. Like
TU3 (T `823BE050`) it now picks from every position except Random's, and
leaves out Facility / Ruin unless the scenario is one they allow (MODE 0
with SCENARIO 0 / 1, or DarkOps with scenario 0 / 2; -1 counts as allowed);
launch's tail (settings +652) is kept. TU3's ownership check isn't needed.
Left out: TU3's unrelated FRIENDLYFIRE rule in the same area (T `82432678`:
on for scenario 2 or ranked team games). `pdz_dlc_log` prints each pick.

## Not ported / open
- **Remote mine record** (`guncontrolmultimine.xml`): the mine's ammo
  amounts (clip 5 vs launch's 1, with TU3's ammo row dropping the 4 free
  spare mines). Pack maps get TU3's record but launch's ammo row, so probably
  9 mines there; proposal `pdz_fix_tu_mines` in `docs/BUG_AUDIT.md`
  ("Multiplayer bots and TU features"). Not applied yet.
- Ownership per signed-in user (TU3 bit array): not needed on PC.

## Options considered

A, rebuilding from the TU3 executable, was rejected: it removes the
interactive music, every hook would need re-addressing, and the multiplayer
rewrite comes along. B (above) was approved.
