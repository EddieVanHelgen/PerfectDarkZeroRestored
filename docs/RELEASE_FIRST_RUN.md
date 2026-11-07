# Release: first-run setup and the native renderer's shaders for new players

Goal: a new player supplies only their own Perfect Dark Zero disc dump and Title Update 3, and gets the native renderer
with the same picture and performance as the developer's build. The release ships stock graphics settings. The
emulated renderer stays only as Safe mode (emulated, locked 60) and as the automatic fallback when the native
renderer can't start. The existing plan is docs/RELEASE_SHADERS.md; this file records what was checked on
2026-11-03 (Stage 0) and each stage after it.

## Stage 0 (2026-11-03): findings

### 1. Game files: what the build expects today

| Item | Today |
|---|---|
| Format | **Extracted files only.** `game_data_root` (default `<repo root>\assets`, found by walking up from the exe for `perfectdarkzerorecomp_manifest.toml` or an `assets` folder) must hold `default.xex`, TU3's `default.xexp` next to it, and `pdz\` (4.9 GB). The SDK mounts it as `game:` / `d:` (`Runtime::SetupVfs`, a host-folder device). At load, `UserModule::LoadFromFile` finds `default.xexp` next to `default.xex` and applies the patch. |
| TU3 data | The 29 files of the TU3 package (`TU_16L61UJ_000000G000000.00000000000O4`, an STFS `LIVE` package, 1,703,936 bytes) extracted to `<user data root>\update` (default `Documents\perfectdarkzerorecomp-tu3\update`), mounted as `update:`. |
| Checked | Only that `update\Pdz\Assets\PackageData\1726605008.pkg` exists (message box + exit 2, 17a4468) and that `default.xex` exists ("Entrypoint XEX not found"). Nothing checks versions at run time: the README asks for `default.xex` CRC32 `48079969`, media ID `...-6A78E206`; the build's code is recompiled from exactly that XEX + TU3. |
| Original README (nikolaygorb, 6756d2c..d9d7549) | The launch build: files extracted with ABGX360 / extract-xiso into `assets\`. No TU, no ISO support. |
| What players have | A Redump ISO (here `Perfect Dark Zero (USA) (En,Fr,Es).iso`, 7,834,892,288 bytes, XGD2) and the TU3 package as copied from their own Xbox 360. There are five TU3 packages, one per media ID; at this stage only `...O4` (media ID 6A78E206) matched this build (since 2026-11-04 any disc's Title Update 3 works: "Any Title Update 3" below). |
| Reference hashes (SHA-1) | `default.xex` `c24ae39fb18a22de1f18e61dc498393d25eadcaf` (5,476,352 bytes); TU3 `default.xexp` `b148ab898498a4dbdd494ddaf0e7d826612a6126` (935,936 bytes). |
| Reusable code | The SDK already has Xenia's `DiscImageDevice` (XDVDFS / ISO) and `StfsContainerDevice` (STFS packages), unused by the runtime. `rexruntime.dll` exports all its symbols, so setup can read the ISO and the TU3 package as they are. |

### 2. The shader numbers, against today's cache and code

`PDZRenderResearch\native\cache\manifest.json`: 2,177 shaders, all translated (`hlsl\`: 2,177 `.hlsl` / `.dxil` / `.json`).

| Origin | VS | PS | From the player's game files? |
|---|---|---|---|
| `static` (containers in packages, the exe, DLC) | 439 | 1,198 | yes |
| `exe-raw` (raw arrays in the exe) | 1 | 1 | yes |
| `xsh-bound` (a package VS bound to a vertex declaration) | 534 | 0 | **needs the declaration**: 534 of 534 bound VS translate to different HLSL from their parent (the vertex input types follow the declaration: e.g. `int4 iAttr0 : ATTR0` instead of `float4 iPosition0 : POSITION0`). 317 parents. |
| `xsh-only` | 3 | 1 | **no** |
| **Total** | 977 | 1,200 | 1,639 static + 538 run time |

- **The 4 the binder can't rebuild** (`c2bdc141db1b3bff` ps, `6d8bba1c194df9b9`, `c5b0cb6859943348`, `fd8157c0fec17889` vs) differ from their nearest container outside the vfetch fields (ALU and tfetch dwords). The game's video player creates their sources at run time. The shader records show the created VS `b2e19d06be503178` (in no package or exe container; it sits in the build's `misses\`) bound to `6d8bba1c194df9b9`. So they exist only at run time.
- **Beyond the cache:** the build's `native_shaders` holds 45 more bound VS translated at first use (`.bin` + `.hlsl` + `.json`, 44 `.dxil`), plus 10 microcode files in `misses\`. The set of bound VS keeps growing with play (MP maps, DLC), so the run-time path is needed whatever is generated ahead.
- **DXC is deterministic:** 109 cached shaders (every 20th) recompiled with the fork's bundled `dxc.exe` (1.8.2407.7, `dxil.dll` 101.8.2407.12) and the fork's flags (`-T vs_6_0|ps_6_0 -HV 2021 -all-resources-bound -Wno-ignored-attributes -Qstrip_reflect -Qstrip_debug`) came out **109 of 109 byte-identical**. Median 64 ms per shader as a process (start included).

### 3. A native shader miss today (null GPU backend), measured

- Code (`native_present.cpp`): with the null backend there is no emulated frame. A frame with a draw skipped for a missing shader is shown as drawn, without that draw. Before that, `pdz_native_translate_sync` (on with the null backend) lets the draw wait up to 1,500 ms for a translation, but only when the translator exists.
- **Today the translator is missing** (`XenosRecomp.exe` quarantined by Defender on 26 and 30 Oct; only its DLLs and `.pdb` remain). Every miss is a skipped draw that stays skipped.
- **Fresh-install run** (log 596, Datacore, 60 s, empty shader folder, no research fallback, no PSO cache, no translator): 0 draws issued, 5,873,086 skipped for a missing shader, 4,952 of 4,974 frames shown with draws missing: a **black picture**. The note that a fresh install "falls back to the emulated renderer (17a4468)" doesn't hold: 17a4468 only made `gpu_plugin` default to xenos and added the TU3 message. Nothing checks for shaders before choosing the native default.
- **Safety net design** (Stage 2):
  1. Translate on the fly in-process, on a pool of workers. The draw waits, bounded as today. A translation is about 64 ms as a process, less in-process (to measure in Stage 2).
  2. Once the budget is used, skip that draw for the frames until it is ready, and log it once.
  3. Save every translation to the per-user cache, so it happens once per player.

### 4. The translator

- **Where it runs today:** as a child process (`Translator::Translate`, `CreateProcessA`), at `pdz_native_translator` = `C:\Games\PDZRenderResearch\native\tools\XenosRecomp\build\XenosRecomp\XenosRecomp.exe` (missing). The research batch (`translate_pdz.py`) runs the same exe.
- **The fork:** hedge-dev/XenosRecomp (MIT) at upstream 990d03b plus 9 local `pdz` commits (b668b30 the last), published as the `pdz` branch of the XenosRecomp fork (docs/CREDITS.md). About 5,100 lines, plus `fmt`, `xxHash`, `zstd`, `smol-v` (the last two are only for the Unleashed path).
- **Linking it in:** yes. `pdzTranslate` (`pdz_main.cpp`) is a function over `ShaderRecompiler::recompilePdz` and `DxcCompiler::compile`. The game already loads DXC in-process (`native_depth20e4.cpp`). Built into `PDZRestored.exe`, no separate translator exe exists to be quarantined.

### 5. Licences

| Component | Licence | Ship? |
|---|---|---|
| XenosRecomp fork | MIT (hedge-dev and contributors; our patches) | yes, as code inside the exe, with the licence text |
| fmt / xxHash / zstd / smol-v | MIT / BSD-2 / BSD (dual GPLv2) / MIT | yes (zstd / smol-v not needed) |
| SDK disc-image and STFS devices | Xenia, BSD | already in `rexruntime.dll` |
| stb (zlib inflate for the package scan) | MIT / public domain | yes (already in the SDK) |
| `dxcompiler.dll` 1.8.2407.7 | open source (LLVM / NCSA), a Microsoft-built binary | **your rule: no Microsoft files**, see below |
| `dxil.dll` 101.8.2407.12 | Microsoft, closed source; the DXC release licence lets apps redistribute it | **your rule: no Microsoft files**, see below |
| VC++ runtime DLLs (the Deck folder had them) | Microsoft | not in the zip; the player installs the VC++ redistributable |

**DXC is a decision for you.** DXIL needs `dxcompiler.dll`, and D3D12 needs it signed by `dxil.dll`. Windows has neither (checked: only `d3dcompiler_47.dll`, which makes DXBC and can't read HLSL 2021). The game also needs DXC at play time even with a full cache: the co-issue / blend7e3 / depth20e4 pixel variants are rebuilt from `.hlsl` at first use. The options:
- (a) ship the two DLLs under the DXC release licence, as other PC ports and emulators do;
- (b) first-run setup downloads the exact DXC release (v1.8.2407) from Microsoft's GitHub, checked by SHA-256, so the zip carries no Microsoft file;
- (c) build the open-source `dxcompiler` ourselves and leave the DXIL unsigned (D3D12's bypass hash). Not proven on Windows 10 / 11 D3D12; to measure first.

The byte-identical match needs the same DXC version, which (a) and (b) give.

### 6. Stock settings

- The launchers in `C:\Games`, the play copy's and the committed `settings\hardware.toml` pass nothing non-stock. `PDZ_MIXED.bat`'s `--resolution_scale=3` is the emulator's, for picture comparison.
- **The Blades dashboard entry** (`C:\Games\Dashboard360Research\recomps.toml` line 10) still passes `--pdz_shadow_distance_scale=3 --pdz_shadow_map_scale=3 --pdz_shadow_far_pcf=1`. Drop those three if you test from it.

## Plan

| Stage | Work | Estimate |
|---|---|---|
| 1. First-run setup | A small Win32 window inside the exe (reusable for the settings launcher). Finds the disc dump: an ISO or an extracted folder in `game\` next to the exe, else a file picker. Finds TU3 (the package as copied from an Xbox 360, or extracted). Checks the `default.xex` and `default.xexp` hashes, with clear messages for a wrong dump, media ID or TU. Extracts the ISO and the TU3 package with the SDK's devices into the prepared folders, shows progress, and writes a stamp so it never runs again unless the files change. | 2-3 days |
| 2a. Translator in the exe | Vendor the fork (MIT), build its core into the game, and replace the child process with in-process workers. Paths default to the per-user cache (RELEASE_SHADERS.md "Paths"). DXC as decided. | 1-2 days |
| 2b. Scan + batch translate | A C++ package scan (CAFF, zlib via stb, one package at a time; the exe image's containers and raw arrays; DLC), translated on worker threads into `<cache root>\native_shaders`. A stamp (exe build, translator version, package list) makes it rebuild itself when anything changes. On failure, the emulated renderer starts, with a message and a log line. | 2-3 days |
| 2c. Bound VS | A bind list (parent hash + declaration formats / strides; no microcode) from the cache and the build's records, run through `binder::Bind`. Plus the run-time safety net above for anything new. | 1-2 days |
| 3. Proof | `C:\Games\PDZ_FreshTest` with only the release zip's files plus copies of the game files. First-run setup, byte-compare every generated shader with the cache, the auto-diff on Datacore + 2 missions against the play build, timing, first-play freezes. | 1-2 days |

Two decisions for you, needed by Stage 2: **DXC** (a / b / c above), and **the bind list** (a table of hashes and vertex formats, no Rare code, is fine to ship? Without it the 534+ bound VS are translated on first use: about 100-170 per mission on first play).

## Stage 1 (2026-11-03): first-run setup in the exe

Code: `src/setup/first_run.cpp` (the logic), `src/setup/setup_window.cpp` (a small Win32 window: two text lines, a
progress bar, Cancel; message boxes; a file picker; reusable for the settings launcher). Called from
`OnConfigurePaths` before the runtime starts.

**When it runs.** Only when the game files aren't usable where the build looks (`default.xex` + `default.xexp` in
`game_data_root` and `Pdz\Assets\PackageData\1726605008.pkg` in `update_data_root`). A developer's machine (the
repo's `assets` + `Documents\perfectdarkzerorecomp-tu3\update`) and the play copy never see it.
`--game_data_root` / `--update_data_root` skip it.

**What it does.**
1. Looks in `game\` next to the exe (two levels deep) for an ISO (`.iso` / `.xiso`, the largest), else a folder with
   `default.xex`, and for Title Update 3: a Title Update 3 package as copied from an Xbox 360 (file names starting
   with `TU_16L61UJ_`), any `TU_*` package, or an extracted `default.xexp`. Nothing found: a box explains, then a file picker asks for each.
2. Checks: `default.xex` SHA-1 `c24ae39f...` (else: "this disc isn't the version...", with the disc's own media ID
   and version from its XEX header), the TU package's title ID, and `default.xexp` SHA-1 `b148ab89...` (else: "this
   isn't Title Update 3 for your disc", naming the right file). It also checks the free space.
3. Extracts with the SDK's own readers (Xenia's `DiscImageDevice` for the ISO, `StfsContainerDevice` for the TU
   package) or copies the folder:
   - the disc files (without `$SystemUpdate`) and TU3's `default.xexp` go to `<data dir>\game`;
   - TU3's 29 data files go to `<data dir>\update`.

   `<data dir>` is `data\` next to the exe, or `<user data root>\data` when the exe's folder can't be written.
4. Writes `<data dir>\setup.json` last (the two hashes, sources, counts, time). Each start re-hashes `default.xex` /
   `default.xexp` (about 6 MB) against it, so setup runs again only if those change or files are missing.
5. `<data dir>\setup.log` records each step and every message shown (for bug reports); the game log gets one
   `pdz_setup:` line.

Cvars: `pdz_setup` (auto / force / off), `pdz_setup_data_dir`, `pdz_setup_source`, `pdz_setup_quiet` (testing: no
boxes, the problem goes to `setup.log` and the game exits with code 2).

**Tested** (`C:\Games\PDZ_FreshTest`: only `PDZRestored.exe`, the two DLLs, `gamecontrollerdb.txt` and the committed
`settings\`, plus copies of the game files; `--user_data_root` of its own, so no real saves are touched):

| Test | Result |
|---|---|
| Extracted disc folder (a copy of the 6A78E206 files) + the TU3 package as copied from the console | 1,381 files, 5.23 GB in 14.0 s (warm OS cache; 139 s before a fix of the progress thread); the game ran from `data\`: 898 frames, 0 shader misses |
| Second start | the stamp checked, no setup, the game ran |
| ISO (an XISO made from the same files with `extract-xiso -c`) | 15.7 s; the result is byte-identical to the folder path's (`diff -rq`), and the TU3 data is byte-identical to `Documents\perfectdarkzerorecomp-tu3\update` |
| Wrong disc: the second ISO | stopped before copying: "media ID 41EEAFCB, version 0.0.0.1" |
| Wrong TU: the 41EEAFCB title update | stopped in 0.1 s, naming the right file |
| File picker, Cancel, the window's look | not tested (needs a person) |

**Found on the way.**
- **Your dumps are three different pressings.** `assets\default.xex` (as the daily build's, 17 Oct) is media ID
  6A78E206, version 0.0.0.4. `<another backup of the disc>` is 750FF1D9, version
  0.0.0.10: it differs from 6A78E206's in only 314 bytes, all in the XEX header (0xC8-0x217C; the code after it is
  identical). The second ISO is 41EEAFCB, version 0.0.0.1. Setup accepts only 6A78E206 + its TU3 (the build's
  code is recompiled from exactly that pair). Whether another pressing plus its own TU3 gives the same final image
  (each TU3 brings its disc to 0.0.3.4) decides how many players can use a release. To check next (Stage 2 side
  item): apply each TU3 to its disc's XEX with the SDK's patcher and compare the images.
- The committed settings template has `log_level = "critical"`, so a release folder writes empty logs. Tests pass
  `--log_level=info`; a release probably wants `info` (bug reports).

## Stage 2a (2026-11-03): the translator inside the exe

Decided (2026-11-03): DXC ships next to the exe (`dxcompiler.dll` 1.8.2407.7, `dxil.dll` 101.8.2407.12, the
release the cache was compiled with), under the DXC release licence: the only Microsoft files in the release. A bind
list (hashes and vertex formats only) may ship.

- `thirdparty/XenosRecomp-pdz`: the fork's sources at b668b30 (MIT; provenance and the changes in `README_PDZ.md`),
  built as a static library (`cmake/pdz_xenosrecomp.cmake`). `xenosrecomp_pdz::Translate` is the CLI's
  `pdzTranslate` on memory. `shader_common_pdz.hlsli` is embedded byte for byte (`.gitattributes`: `-text`).
- The build copies `dxcompiler.dll` + `dxil.dll` next to `PDZRestored.exe` from `PDZ_DXC_BIN_DIR` (default: the
  fork's `dxc-bin`). They are never committed.
- `native_dxc.cpp`: one DXC loader for the translator and the patched pixel variants (co-issue, blend7e3, depth20e4).
  It looks in `pdz_native_dxc_dir`, else the exe's folder, else (developer build only) the fork's `dxc-bin`.
- `native_shaders.cpp`: the child process is gone (`pdz_native_translator` removed; `pdz_native_translate`
  on/off, `pdz_native_translate_workers`, 0 = a quarter of the logical processors, 1-4). The files it writes are the
  CLI's.
- **Paths.** A release (no `perfectdarkzerorecomp_manifest.toml` up the tree) keeps shaders in
  `<cache root>\native_shaders` (per user) with no fallback. A developer's build and the play copy keep
  `<exe dir>\native_shaders` + the research cache. The research path in `native_blend7e3.cpp` / `native_depth20e4.cpp`
  is now the cache's fallback folder.

**Measured** (log 597, Datacore 90 s, empty shader folder, no fallback, no PSO cache):
- **233 translations, median 13 ms, max 44 ms**, on 4 workers. The exe as a process took 64 ms median.
- 640 draws skipped in 375 of 7,579 frames, while the first translations ran.
- **Byte comparison with the research cache:** the bound VS (89) and the run-time-only VS (1) are identical (.hlsl,
  .json, .dxil). The 142 static pixel shaders differ, because their input differs. The cache translated them from
  their containers (literal constants, interpolators); a run-time miss only has raw microcode, which gives a worse
  shader (`r0 = 0.0` for `r0 = i0`, `c[0]` for the literals).
- So Stage 2b must translate every static shader from its container, and a run-time miss of a static shader must
  use its container from the scan, not the raw microcode.
- The video-player PS `c2bdc141db1b3bff` failed. Its microcode read from guest memory differs from the cache's copy
  and parses as unsupported. To look at in 2b.

## Stage 2b (2026-11-03): the shaders made from the player's game files

`src/mod/render/native/native_generate.cpp` does on the player's PC what `harvest.py` + `translate_pdz.py` did for
the developer's cache:
- **Order and rules as the harvest's**:
  - the exe image's containers, then its two raw microcode arrays (0x820740F0 PS, 0x82074108 VS), then the retail
    packages by name, then the DLC and title update packages by path;
  - magic `10 2A 0E 00/01` with the same size checks;
  - the first container of each microcode hash (XXH3-64) wins, and a raw array counts only when no container holds
    it;
  - packages are inflated with stb's zlib decoder (all 137 retail and 47 DLC / TU packages are one zlib stream each,
    checked).
- **Translated** in-process on all cores but one, as the batch did: static shaders from their full container, the
  raw arrays from their microcode.
- **Written** into the per-user folder `<cache root>\native_shaders`:
  - `<hash>.hlsl/.json/.dxil` and the microcode `<hash>.bin`;
  - `containers\<hash>.bin`, which the run-time translator now prefers over raw microcode;
  - `generated.json`, a fingerprint of the package list (names, sizes, times), the exe build, the translator (with
    its include) and DXC. A change makes it run again; the old files are cleared first.
- **Two phases:**
  - the packages in `OnPreSetup`, with a progress window, before the renderer is chosen;
  - the exe image in `OnPostLoadXexImage`, from guest memory.
- **Safe mode** (`pdz_safe_mode`): the emulated renderer at 60 fps. It is also the automatic fallback when the
  shaders can't be made (no DXC, no packages, over 0.5 % failed), with a box (none under `pdz_setup_quiet`) and a
  log line. It tries again next start.
- `cvar pdz_native_generate`: auto (a release only) / force (a developer's build needs `pdz_native_shader_dir`) /
  off.

**Fixed on the way:** a pixel shader's microcode is kept as it was at create time (`shadow::FindPixelUcode`). The
video player rebuilds its shaders in a reused buffer, so the miss path read other bytes. In log 597 the intro video's
PS `c2bdc141db1b3bff` became garbage; on a fresh install the intro would have stayed black. The translator now also
refuses input whose bytes don't hash to the shader asked for.

**Measured** (`C:\Games\PDZ_FreshTest`, logs 005-007):

| | |
|---|---|
| Generation | **5.5 s** on this 24-thread PC: 148 packages (137 retail + 11 TU) scanned and 1,376 shaders translated in 5.2 s, the exe image's 164 containers + 2 raw arrays in 0.3 s, 0 failed |
| Shaders made | 1,542: the research cache's 1,639 static ones minus the 97 found only in the DLC packs (the test folder has none) |
| **Byte comparison** | **1,544 of 1,544 identical** to the research cache (.hlsl, .json, .dxil, microcode): the 1,542 generated plus the two video shaders translated at first use. **1,540 of 1,540 containers identical.** |
| Next start | the fingerprint matched: no generation |
| Intro and front end | 1,349 frames, 0 with a shader miss, 0 draws skipped (log 006) |
| Fallback | DXC missing: "couldn't be made", Safe mode, the game ran on the emulator (log 007) |

## Stage 2c (2026-11-03): the bound vertex shaders

Decided: the bind list ships, holding only hashes and vertex declaration formats / strides.

- **`data/native_bind_list.txt`** (84 KB, 588 rows; a note at the top says what it is and that it holds no game
  code). Per bound VS: its hash, its parent's hash, the stream strides, and the declaration elements (usage, index,
  stream, offset, vertex format, signed, integer, type swizzle).
  - Made by `tools/shaders/make_bind_list.py` from the research cache's 534 .xsh bound VS plus 63 more pairs from the
    build's run-time shader records.
  - The research binder rebuilds each declaration and must bind the parent back to the bound microcode byte for
    byte. 9 pairs were left out: their parents are the video player's run-time shaders, with no container.
  - Embedded in the exe by CMake.
- **The exe phase of the generator** binds each row's parent (the player's own container, the exe's first) with
  `binder::Bind`, checks the XXH3 hash, translates the result with the others, and writes `manifest.json` (bound VS
  → parent) for the run-time sibling path.

**Measured** (log 009, forced regeneration in `C:\Games\PDZ_FreshTest`):
- 581 bound VS made, 0 the C++ binder didn't reproduce; the other 7 have DLC parents the test folder doesn't have.
- The whole generation is now **6.7 s** (packages 5.6 s, exe + bound VS 1.1 s): 2,123 shaders, 0 failed.
- **Byte comparison: 2,113 of 2,113 identical** (.hlsl / .json / .dxil) to the research cache (2,071) or the build's
  run-time translations (42).
- 12 more are bound variants the developer's build never translated (it reused a sibling's DXIL), so there is
  nothing to compare them with.
- Front end: 598 frames, 0 shader misses.

## Stage 3 (2026-11-03): proof in a clean folder

**The release folder.** `tools/release/stage_release.py` builds it from the play build and the committed repo files,
writes `RELEASE_CONTENTS.txt` (sizes, SHA-256) and fails on any game file, translated shader or Microsoft runtime
file.
- 17 files, 120.6 MB: `PDZRestored.exe`, `rexruntime.dll`, `rexgpu-xenos.dll`, `dxcompiler.dll`, `dxil.dll`,
  `gamecontrollerdb.txt`, `settings\` (the committed `hardware.toml`, `mapping.toml`, `gamecontrollerdb.txt`),
  `PDZRestored (Safe mode).bat`, `game\README.txt`, `README.md`, `LICENSE`, `THIRD_PARTY_NOTICES.md`, `licenses\`.
- The only Microsoft files are `dxcompiler.dll` and `dxil.dll`. Both are byte-identical to Microsoft's official DXC
  v1.8.2407 release (`dxc_2024_07_31.zip`; licences `LICENSE-LLVM.txt` and `LICENSE-MS.txt` from it).
- No VC++ runtime DLLs: the README tells players to install the redistributable.
- `C:\Games\PDZ_FreshTest` is that folder plus copies of the game files in `game\` (the extracted 6A78E206 disc and
  the TU3 package as copied from the console). Its own `--user_data_root` (`userdata\`) keeps it off the developer's saves.

**First start** (log 001 there):
- setup 7.0 s (OS cache warm; 14-18 s from a cold disc or the ISO);
- shader generation 6.9 s;
- **about 14 s from start to a running game** on this 24-thread PC;
- the intro and front end: 1,798 frames, 0 shader misses.

**Shaders against the developer's cache:** 2,113 of 2,113 with a reference byte-identical (Stage 2c); 1,540 of
1,540 containers identical.

**Auto-diff** (native vs emulated, same frames; same exe; the developer's build with the research cache against the
fresh folder with its generated cache; every run 0 shader misses, 0 skipped draws):

| Run | Developer's build | Fresh folder |
|---|---|---|
| Datacore picture gate (`run_datacore_test.ps1`, 41 captures, swaps 2400-7200) | MAE 1.53, 1.4 % over (sd 0.05) | **MAE 1.54, 1.4 % over** (sd 0.04) |
| Datacore (mission script, in-level captures) | 1.53-1.56 | 1.54-1.58 |
| Mansion, run 1 / run 2 | 7.51 / 6.89 | 6.62 / 7.18 (the order flips: run-to-run spread about 0.5) |
| Jungle | 1.9-3.2 per capture | 1.6-3.1 per capture |

The developer's first Datacore mission run had a 6.2 s stall, so its first four captures fell before the level
(MAE 65). The settings differ in one way: the fresh folder uses the committed template, the developer's build
your own `hardware.toml`.

**First-play freezes** (for the warm-up item). The pipelines are compiled at their first use:

| Mission | PSOs compiled at first use | Total time | Longest | Patched pixel variants compiled |
|---|---|---|---|---|
| Datacore | 306 | 496 ms | 65 ms | 28 |
| Mansion | 236 | 1,213 ms | 68 ms | 42 |
| Jungle | 138 | 821 ms | 71 ms | 49 |

- On the second play of a level: 1 PSO in 1 ms (the PSO cache, now per user in `<cache root>\native_cache`).
- 0 shaders were translated at run time on any first play.
- Texture first-use waits weren't measured: the template doesn't turn on the frame timing.

**Pressings** (the side item):
- Each disc's `default.xex` differs from 6A78E206's only in its XEX header: 314 bytes (signature, media ID field,
  header digest, execution info). The AES key and everything after the header are identical.
- Loading each disc with its own TU3 and writing the image (`pdz_test_dump_image`) gives **one byte-identical image**
  for 6A78E206 + O4, 750FF1D9 + OA and 41EEAFCB + O1: SHA-1 `4e57d589...`, 20,840,448 bytes.
- The discs differ in their languages. 6A78E206 has Italian audio; 41EEAFCB (USA) has French and Latin-American
  Spanish; 750FF1D9 has French and German audio. Every file they share is identical (685 of 685 on the US disc,
  English included).
- **Setup now accepts all five pressings**, each with its own TU3:
  - by the `default.xex` hash for the three seen;
  - by the XEX header for 509DBB6B and 2B35C136, which are known only from their TU3 files.

  When several TU files are in `game\`, the one named for the disc is taken. **Every start checks the loaded image**
  against `4e57d589...` (`first_run::VerifyImage`, about 25 ms). A mismatch shows a message and sets the stamp
  aside.
- Tested with the US ISO plus both the O1 and O4 packages in one folder: O1 was chosen, setup took 18.4 s from the
  ISO, the image check passed, the 2,125 generated `.dxil` are byte-identical to the 6A78E206 folder's, and the front
  end ran with 0 misses.

**For a release** (not done here):
- the SDK's bundled libraries (SDL3, FFmpeg, ImGui, ...) need their notices;
- `dxil.dll`'s terms ask that players agree to terms protecting Microsoft at least as much (a line in the release's
  terms);
- the settings template's `log_level = "critical"` writes empty logs.

## Stage 4 (2026-11-03): first-play pipelines

Stage 3 measured the first-play freezes: a fresh install compiled every pipeline (PSO) and every patched pixel
variant at its first draw, while the developer's build had them all from its warm-up library. Now a new player's
first play is as smooth as their second.

**The pipeline list** (`data/native_pipeline_list.txt`, made by `tools/shaders/make_pipeline_list.py`):
- what it holds: one line per pipeline with the shader hashes, the vertex layout, the depth / blend / stencil /
  raster state, topology, target formats, the depth20e4 / blend7e3 variant flags and the hashes of the developer's
  DXIL (a check only);
- what it doesn't hold: no microcode, no translated shader code, no DXIL. A note at the top says so;
- where it comes from: the play build's recipes, the play copy's and Stage 3's fresh folder's, merged: 2,404
  pipelines, 549 KB;
- how it ships: embedded in the exe (`cmake/pdz_xenosrecomp.cmake`, like the bind list). `stage_release.py` checks
  that the exe holds the committed list, that every line is hex / decimal fields only, and lists it in
  `RELEASE_CONTENTS.txt`.

**At first start** (`native_pipeline_seed.cpp`, `pdz_native_pso_seed` auto): once, after the shaders are made and
before any draw, behind the setup's progress window ("Preparing graphics"):
1. the list's 508 VS and 998 PS are loaded from the player's shader folder;
2. each pixel shader's first-draw patch check (co-issue, PsParamGen, live literals, ... as `FindPixel` runs it) and
   the listed depth20e4 / blend7e3 variants:
   - first as a collect pass on copies, which gathers only the sources;
   - then all sources compiled at once on every core (`depth20e4::batch`, one DXC compiler per worker);
   - then for real, taking the compiled DXIL and saving the patch caches as the draws would. The microcode and
     container come from the files the generator saved instead of guest memory.
3. the recipes are written to `<native cache>\pso_seed.bin`.

The PSO store merges `pso_seed.bin` into its recipes, and the existing startup warm-up (`pdz_native_pso_prewarm`)
builds them all. A stamp (`pso_seed.stamp`: list, shader folder, `generated.json`) runs the pass again when any of
them changes. After the next save the seed recipes live in `pso_recipes.bin` and the seed file goes. A developer's
build skips the pass (its own recipes).

**Warm-up:** with a pipeline library holding under half the list (a first start, or a new GPU / driver), it uses half
the CPU's threads (2-8) instead of a quarter.

**Safety net:**
- `pdz_native_pso_level_wait_ms` (20 s): a level starting while the warm-up is still running waits for it before
  its first frame (logged with the time waited). In every run here the warm-up was done 1-2 s after the process
  started, so the wait never triggered.
- Pipelines not in the list can't be known before their first draw. They are still compiled there, each logged once
  with its time ("PSO first use"), and they join the player's recipes, so later starts build them in the warm-up.

**Blocking mode, measured:** `pdz_native_pso_warmup_wait` makes the first native draw wait for the whole warm-up. It
gained nothing here: the warm-up takes 0.1 s with the driver's cache warm, 2.4 s cold (the first start of a new
exe). So it stays off: the warm-up runs on its workers during the intros.

**Proof** (this PC, 24 threads; fresh copies of the release folder, own `--user_data_root`):
- **First start:** two runs.
  - Totals: about 38 s and 41 s to a running game.
  - Setup took 19.5 s and 11.6 s, shader generation 11.5 s and 21 s. Both vary with the disk cache: Stage 3 had
    7.0 s and 6.9 s.
  - The new pipeline pass took **4.6 s and 6.0 s**: 1,283 patch / variant sources compiled on 16 threads in 2.0 s,
    2,352 of the 2,404 pipelines written.
  - The other 52 use shaders that no generated folder has (they came from the developer's research cache; Stage 3's
    folder lacks them too).
  - Every patched shader Stage 3's draws made is reproduced byte for byte: 278 co-issue, 24 depth20e4, 62 blend7e3.
  - The warm-up then built all 2,337 pipelines in 2.4 s on 8 threads (driver cache cold).
- **First plays** with the bare exe's profile (scale 3, record thread, cached untiling), each from a reset cache and
  a cold pipeline library, frame timing on (`--pdz_frame_timing=true --pdz_frame_timing_spike_ms=12`). Before =
  the same build with `pdz_native_pso_seed=off` and empty patch caches (Stage 3's conditions):

| Mission | PSOs compiled in play: Stage 3 / before / **after** | Stall (frames over 40 ms): before / **after** | Longest frame: before / **after** |
|---|---|---|---|
| Datacore | 306 / 306 / **0** | 2,520 ms / **182 ms** | 2,022 ms / **97 ms** |
| Mansion | 236 / 404 / **0** | 3,067 ms / **224 ms** | 1,622 ms / **131 ms** |
| Jungle | 138 / 416 / **0** | 3,617 ms / **192 ms** | 1,924 ms / **143 ms** |

- About the PSO counts:
  - "before" counts the whole session (front end included); Stage 3 counted from the auto-diff runs.
  - "After" leaves one PSO: the video player's pixel shader at boot, 0.6 ms, before any level.
- The driver's own shader cache was warm in these runs, so each PSO compile cost under 2 ms. Stage 3's (cold, first
  runs of that exe) took up to 71 ms each. The before stalls here are mostly the variant / patch compiles and pixel
  shader loads at the level's first frames.
- Datacore's second play: longest frame 99 ms, 0 PSOs.
- **Texture first-use waits** (item 5), first play, frames over 10 ms:
  - At each level start, 1-2 frames of 50-143 ms (Datacore 97 ms, Mansion 131 ms, Jungle 143 ms). In those frames
    the record thread creates 170-300 textures (20-24 MB) while the game thread waits for it (cause `guest.wait` /
    `blocked`).
  - After that only single frames of 12-42 ms. Mansion's are its known heavy scenes (cause `blocked`), not
    textures.
  - Individual waits at the draw stay under 1.4 ms.
  - The second play is the same: the learned texture set had 367 entries but prepared only 5 ahead.
  - With the record thread off (the mission runner's settings), the same level starts take 2.5-3.9 s on the game
    thread (`tex.upload`).
- **Datacore picture gate** (fresh folder, seeded): MAE 1.54, 1.4 % over (Stage 3: 1.54 / 1.4 %).

## Stage 5 (2026-11-03): level-start textures, shaders, first-start time

**Cause (why the learned texture set prepared 5 of 367):**
- The set (`texture_prewarm\<level>.bin`) keyed each texture by its guest address.
- The level loads its textures at **other addresses every run**: of the unmatched entries, 319 had nothing at the
  learned address and 42 had another texture there, even between two identical runs.
- The level also wasn't known until state 21, the end of the frame that draws the burst, so those textures were
  never learned.

**What changed:**
- **Level known at loading start:** `game_pace.cpp` names the level from loading start (level id set, game state
  past 4: Datacore loads through states 5, 30, 6, 29, 19, 20, 7 for about 4 s), then `OnLevelPlaying` at state 21.
  `pdz_pace_state_log` (off) logs the state changes.
- **Texture discovery** (`pdz_native_texture_discover`, on):
  - What it finds: during the loading a worker scans the guest heap (0x40000000-0x7FFFFFFF, 36 ms a scan, every
    0.4 s) for the D3D texture objects the loading creates. Those are type 3 in the object's first dword, with the
    fetch constant at +16.
  - Filters: a sane pitch / size and committed data (checked by copying it); queued once two scans in a row show the
    same content.
  - First play: everything new since the loading began, at most `pdz_native_texture_discover_mb` (256).
  - Later plays: only the objects whose content matches last time's set (the learned set is now matched by content).
  - Each one is prepared on the upload workers like a prefetch. A draw takes it only with its exact key, and after the
    whole base level is hashed again.
  - Datacore: 1,321 found, 1,285 prepared (75 MB), 340 of the first 10 s textures taken ready, 0 refused.
- **Shader preload** (`pdz_native_shader_preload`, on): the pipeline list's 504 VS, 950 PS and 352 variants are
  loaded from the first start's disk caches, about 0.75 ms per record batch, from the session's first frames (812 ms
  in all, done in the front end). A level's first frames had spent about 106 ms on them (555 VS bindings, 138 PS).
- **Setup:** `pdz_setup_link`, on: an extracted disc on the same drive is hard-linked into `data\`, not copied
  (1,351 of 1,381 files; an ISO is still copied).
- **Pipeline pass:** writes its ~1,300 cache files on several threads (final pass 0.8 s, was 1.8-3.2 s).

**Proof** (fresh release copies, own user data root, bare-exe profile, frame timing on; longest level frame):

| Mission | Discovery off (first play) | First play | Second play |
|---|---|---|---|
| Datacore | 108 ms | 59 ms | 62 ms (2 s in: the game thread waits, not textures; in the off run too) |
| Mansion | 116 ms | 47 ms | 43 ms (first frame: resolves 20 ms, draws 13 ms) |
| Jungle | 112 ms | 52 ms | 56 ms (first frame: draws 17 ms, resolves 15 ms) |

- p99 stays 8.7-9.0 ms. One frame over 40 ms per mission is left.
- Datacore gate: MAE 1.54, 1.4 % over (unchanged).
- Memory (Datacore peak, first play): private bytes +128 MB, GPU local +94 MB. That is the prepared textures
  waiting for their first draw: kept up to 3 minutes, then freed if unused.

**First start** (`C:\Games\PDZ_FreshTest4`, game files copied with unbuffered I/O, so not in the file cache):
- 12.9 s cold and 12.3 s warm to a running game:
  - setup 1.2 s;
  - shader generation 5.3-5.6 s, plus the exe image 1.2-1.3 s;
  - pipeline pass 3.6 s;
  - warm-up 0.1 s.
- Stage 4's 38-41 s came from copying 4.9 GB in setup (11.6-19.5 s), with Defender scanning the new files meanwhile.
  It slowed generation to 10-21 s; Defender was still at about 85 % of a core on average during these runs.

## Any Title Update 3 (2026-11-04)

**The 9 pairings.** Each disc's `default.xex` was loaded with each TU3 package's `default.xexp` through the SDK's
patcher (`XexModule::ApplyPatch`), and the image was written with `pdz_test_dump_image`:

| Disc \ TU3 | O4 (made for 6A78E206) | OA (750FF1D9) | O1 (41EEAFCB) |
|---|---|---|---|
| 6A78E206 (v0.0.0.4) | `4e57d589...` | `4e57d589...` | `4e57d589...` |
| 750FF1D9 (v0.0.0.10) | `4e57d589...` | `4e57d589...` | `4e57d589...` |
| 41EEAFCB (v0.0.0.1) | `4e57d589...` | `4e57d589...` | `4e57d589...` |

- The patcher accepts every pairing as-is, and every image is the verified one (`first_run::VerifyImage`).
- Its only check against the disc is a warning: "XEX patch signature hash doesn't match base XEX signature hash"
  (the patch's `digest_source` is the SHA-1 of its own disc's RSA signature). The header range checks pass for all.
- The patched header takes the TU's version (0.0.3.4 / 0.0.3.10 / 0.0.3.1); the image is identical.

**The data files.** The 29 files of `Pdz\` are byte-identical across all five TU3 packages (O4, OA, O1, O3, O2). Only
`default.xexp` differs, in its header (media ID, version).

**Setup now:**
- It accepts any of the five TU3 `default.xexp` files with any of the five discs.
- When several TU3 files are in `game\`, it takes the disc's own TU3, then another disc's.
- `setup.json` records the TU actually used.
- Anything else is refused, and the message says Title Update 3 is needed: TU1 / TU2 ("It is Title Update N"),
  another game's update, a file that isn't a title update. Any disc's TU3 is named as fine.
- The image check still runs on every start.

**Tests:**
- **Mismatched pairing:** the US disc 41EEAFCB (the second ISO, copied) with O4 (`C:\Games\PDZ_TU3Mix`).
  - Setup took 18.4 s from the ISO; the patch warned as above and was applied (0.0.3.4); the image check passed.
  - Shader generation: 1,376 + 747 translated, 0 failed; the 2,125 `.dxil` are byte-identical to the 6A78E206
    folder's.
  - Then 2,695 front-end frames with 0 shader misses.
  - Datacore first play: 0 pipelines built during play, longest level frame 58 ms. Gate: MAE 1.51, 1.4 % over.
- **Refusal:** a disc `default.xex` offered as the title update stops setup before copying anything: "PDZ Restored
  needs Perfect Dark Zero Title Update 3 ... It is version 0.0.0.1 for media ID 41EEAFCB, not a known Title
  Update 3". No TU1 / TU2 file was at hand to try.

## Discs setup doesn't know (0.9.2.1)

A player's genuine US "Platinum Hits" reprint (media ID 40C97944, version 0.0.0.15, `default.xex` SHA-1
29470e58...; 1,415 files, said to carry the title update and both map packs) was refused as "not a disc PDZ Restored
knows". Since 0.9.2.1:

- **Any `default.xex` with a valid XEX2 header and title ID 4D5307D3 is accepted.** One that isn't in `kPressings`,
  or is listed without a TU3 of its own, is logged as
  `UNKNOWN DISC, accepted by its header` with its media ID, version and SHA-1.
- **Its own code is checked first, by the real loader.** Setup copies the disc and Title Update 3's data files, but
  holds TU3's `default.xexp` back in `data\pending\`. At the first load `VerifyImage` hashes the disc's own code:
  - the same as `kImageSha1`: no title update is needed and none is used (`setup.json`: `image_check` "the disc's
    own code matches");
  - different: the `default.xexp` is moved into `data\game\`, PDZ Restored starts itself again once
    (`--pdz_setup_restarted`, the settings window isn't shown again), and the final code is checked;
  - a full-size image like TU3's (0x13E0000 bytes mapped) that isn't `kImageSha1`: a different build of the game, not
    the original code TU3's patch is made for. No title update is added; it stops at once with the message below;
  - the final code different: "This version of the game is not supported.", with the path of the setup log on its
    own line (the same message for every disc whose code doesn't match).
  - A `setup.json` still at `image_check` "with Title Update 3 (added at the first load)" at a normal start means the
    restart never reached its image check (it crashed or stopped): `Prepare` sets it aside as `setup.json.rejected` and
    setup runs again. The restart's passing check records "... (added at the first load, checked)".
  - Setup doesn't compute the image itself: the loaded image is the loader's result (imports included), and a disc's
    own code can be a smaller image (the original US disc's ends at 833D0000, TU3's at 833E0000; the hash covers only
    the mapped part).
- **Known discs** go as before (their own code needs TU3: checked for 6A78E206, 750FF1D9 and 41EEAFCB on 3 Nov).
- **The title update:** a TU3 the disc carries itself (an STFS package of content type 000B0000 for this title, or a
  `TU_` package) is extracted to `data\disc_tu\` and preferred; then the game folder's, the one named for the disc
  first. With none, a disc checked at load that has TU3's data package (`1726605008.pkg`) itself goes on without
  one; otherwise setup asks for Title Update 3 as before.
- **What a disc has that the known discs don't** is logged: every top-level name with its file count and size (and a
  `disc extra` line for names the known discs don't have), and every content package (CON / LIVE / PIRS) with its
  content type and title.
- **Map packs on a disc:** downloadable-content packages (content type 00000002) for this title are installed after
  the copy with `map_packs::Install`, the same as packs copied from a console (a pack in a `$` folder is copied to
  `data\disc_content\` first). Packs merged into the game's own packages instead would show only as the top-level /
  size lines; that needs the real disc (not tested: no such disc here).
- **setup.log** is copied to `logs\setup.log` next to `PDZRestored.exe` whenever setup ends or stops, and every
  setup message ends with both paths.
- **Test tools (off):** `pdz_setup_test_media_id` (a known disc goes the unknown way, not matched by its SHA-1) and
  `pdz_setup_test_image_mismatch` (the check at load reports a mismatch).

**Tested** (a test folder, the US ISO media ID 41EEAFCB with the 6A78E206 TU3 package, `--pdz_setup_quiet`):
the known disc as before (24.7 s, image matched, 1,342 frames); `pdz_setup_test_media_id=40C97944`: accepted by its
header, its own code (0x13D0000 bytes mapped, SHA-1 82b910c3...) didn't match, TU3 added, the restart's final image
matched, 1,193 frames; `pdz_setup_test_image_mismatch`: the new message with the log path, `setup.json` set aside;
a fake `default.xex`: refused with both log paths. Not tested (no such disc here): a disc that carries its own title
update or map packs.

## The US Platinum Hits (0.9.2.1, 7 Nov 2026): known, not supported

A dump of a real US Platinum Hits disc (media ID 40C97944, version 0.0.0.15, `default.xex` SHA-1 29470e58...) was
tested in its own folder:

- **Its own code** is a full-size image like TU3's (0x13E0000 bytes, SHA-1 e3048eb7...), not `kImageSha1`. It's a
  different build: the PE timestamp is 12 Sep 2006 (TU3's 19 Sep 2006), and from 8214030E on the code is moved (mostly
  by 48 bytes; 71 % of the code's words differ). The recompiled code can't run it.
- **TU3 can't be applied to it:** with a TU3 in `game\`, the first 0.9.2.1 setup added it and restarted, and the SDK's
  patcher crashed ("XEX patch signature hash doesn't match base XEX signature hash"), at every later start too.
- **Since then:** 40C97944 is in `kPressings` with `not_supported`, refused before anything is copied: "This version of
  the game is not supported." plus the setup log path. Any other disc whose own code is full-size but not
  `kImageSha1` stops at its first load with the same message, and no title update is added. A `setup.json` left by
  the crashing restart is set aside at the next start.
- **Its map packs** are merged into its game files (65 files more than the original US disc under `pdzssets\`:
  59 byte-identical to the official packs and TU3's language files; its own `1726605008.pkg` and `393969562.pkg`; the
  Rooftops / Trench multiplayer music `music_mlt_roof` / `music_mlt_trn`). No content packages.

**Tested** (`--pdz_setup_quiet`, TU3 copy in `game\` throughout): the crashed folder: set aside, refused, no crash; the
second start: refused (2 s each). `pdz_setup_test_media_id=12345678` on the same disc: unknown, own code full-size and
wrong, no TU3 added, the message, no crash, both starts. The US disc (41EEAFCB): known, image matched, played 60 s;
as unknown (`12345678`): own code 0x13D0000 mapped, TU3 added, restart matched, stamp "... checked"; a plain start
after it used the prepared files and played 60 s.
