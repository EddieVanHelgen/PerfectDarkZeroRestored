# Translated shaders in a release

Plan only (night of 30-31 Oct 2026); no code changed.

**The problem.** The native renderer draws with host shaders (DXIL) that the XenosRecomp fork translated from
the game's own Xenos microcode. Those files are derived from Rare's copyrighted shaders, so a release can't ship
them. Today they come from two folders on the developer's machine. A fresh install has neither (found in
the Steam Deck test).

**Short answer.** Most of the game's shaders sit in the player's own game files, so the player's machine can make
the cache. Build a one-time generator into the game. Keep the existing translate-at-first-use path as the safety
net. Ship the translator and DXC next to the exe. See "Recommendation".

## How the cache is made today

All of it lives in `C:\Games\PDZRenderResearch\native` (its own git repo). The cache folders are gitignored.

### Step 1: harvest (find every shader)

Script: `tools\harvest\harvest.py` (doc `tools\harvest\README.md`, results `HARVEST_SUMMARY.md`). It reads, read-only:

| Source | Files | Shader containers | What it finds |
|---|---|---|---|
| retail packages `assets\pdz\ASSETS\PackageData\*.pkg` | 137 (69 with shaders) | 17,729 | zlib-packed CAFF packages; containers inside `ndShader2` model nodes, found by magic `10 2A 0E 0x` |
| executable image (`tu3_image.bin`, a dev dump of the loaded TU3 image, base 0x82000000) | 1 | 181 + 2 raw arrays | UI, post FX and video shaders; raw microcode at 0x820740F0 (PS) and 0x82074108 (VS) |
| DLC map packs (user data `0000000000000000\4D5307D3\...`) | 20 (8 with shaders) | 2,372 | 97 shaders found nowhere else |
| title update packages | 2 | 0 | the TU ships no shaders |
| emulator shader cache `4D5307D3.xsh` (from play sessions, written by the SDK GPU plugin) | 1 | 1,500 entries | what the emulated GPU ran: 960 PS byte-identical to a package or exe container, 534 bound VS, 4 video shaders, 2 raw |

Result: 2,177 distinct shaders, keyed by XXH3-64 of the guest-order microcode (the SDK's key):

| Origin | VS | PS | Total | Can be found in the game files? |
|---|---|---|---|---|
| `static` (XDK container in a package, the exe or a DLC pack) | 439 | 1,198 | 1,637 | yes |
| `exe-raw` (raw arrays in the exe) | 1 | 1 | 2 | yes (fixed addresses) |
| `xsh-bound` (a package VS after D3D bound it to a vertex declaration) | 534 | 0 | 534 | no: needs the declaration (see below) |
| `xsh-only` (compiled at run time by the game's video player) | 3 | 1 | 4 | no: only seen at run time |
| **Total** | **977** | **1,200** | **2,177** | 1,639 static, 538 run time only |

Why the bound VS need run-time data: every package vertex shader is stored unbound (vfetch format 0, stride 0).
The Xbox D3D runtime patches about ten vfetch bit-fields from the vertex declaration when the shader is first
drawn. One package VS gives up to 9 variants. The declarations are thought to sit in `ndVertexBuffer2` nodes next
to each `ndShader2` (`PDZRenderResearch\work\PART3_SHADERS.md` section 5), but nobody has decoded them yet
(**not verified**). The binder itself is reproduced exactly: `tools\binder\binder.py` matches all 534 pairs and
`src\mod\render\native\native_binder.cpp` (`binder::Bind`) is its C++ port, used in the game today.

The harvest takes 16 s with the scan cached (`cache\scan\static.json`) and 2-3 minutes with `--rescan`
(about 4 GB peak RAM, Python).

### Step 2: translate and compile

Translator: the XenosRecomp fork, branch `pdz` (MIT, upstream hedge-dev 990d03b plus `tools\patches\01..09`), nested
checkout `tools\XenosRecomp` (gitignored), build steps `tools\BUILD_XENOSRECOMP.md`, notes
`tools\XENOSRECOMP_NOTES.md`. One process per shader:

    XenosRecomp --pdz <container.bin | ucode.bin> out.hlsl [--type vs|ps] --json out.json --dxil out.dxil

`--dxil` compiles in-process with the bundled DXC (`dxcompiler.dll` + `dxil.dll` from `thirdparty\dxc-bin`,
`-T vs_6_0 / ps_6_0 -HV 2021 -all-resources-bound`). `dxil.dll` signs the DXIL; D3D12 rejects unsigned DXIL.
The batch driver is `tools\XenosRecomp-pdz\translate_pdz.py` (default jobs = CPU threads - 2). It writes
`cache\hlsl\<hash>.hlsl / .json / .dxil` and `report.json`. Result: 2,177 translated, 2,177 compiled, 0 failed.

Timing: the last full batch wrote all 2,177 triples in 5.4 s (file times 06:23:49.9 to 06:23:55.3 on 26 Oct, 24
thread machine). That is about 2 CPU-minutes, about 55 ms per shader per thread. The game logs give 31-110 ms per
translation, one 1,047 ms outlier (`pdz_native_translate_sync_ms` description; log 299: 78 ms).

### Step 3: top-ups from play

`tools\warm_shader_cache.py` reads the game's shader record files and logs, finds shaders the cache lacks, and
translates them with the same command into the build's `native_shaders`. `tools\make_nohole_cache.ps1` builds a
stand-alone copy of the cache with some shaders removed, to test the miss path
(`--pdz_native_shader_fallback_dir=none`).

### Sizes (measured 31 Oct)

| Folder | Files | Size |
|---|---|---|
| `PDZRenderResearch\native\cache\hlsl` | 2,177 x (.hlsl, .dxil, .json) + report.json = 6,532 | 47.9 MB of data (.hlsl 28.6, .dxil 14.1, .json 5.2); 64 MB on disk |
| `...\cache\ucode` (raw microcode, read by the sibling-variant path) | 2,177 | 5.2 MB |
| `...\cache\containers` | 1,637 | 7.8 MB |
| `...\cache\manifest.json` (variant map, read at run time) | 1 | 4.0 MB |
| `...\cache\hlsl_dxc`, `hlsl_pre08`, `hlsl_oracle*`, `oracle` | | research only, not read by the game |
| build `native_shaders` (top level: run-time translations not in the research cache) | 45 x (.bin, .hlsl, .json) + 44 .dxil | under 1 MB |
| build `native_shaders\coissue` / `blend7e3` / `depth20e4` (patched PS variants, made by the game) | 1,980 / 805 / 244 .dxil | 20 / 9.7 / 2.1 MB |
| build `native_shaders\record` (shader records + microcode copies) | 481 .txt + 11 .bin | 39 MB |
| build `native_cache` (`pso.lib`, `pso_recipes.bin`, `texture_prewarm\`) | 2 + 6 | 24 MB |

The startup file warm-up (log 342) reads all of it: 11,935 files, 73.7 MB in 0.5-1.2 s.

## How the game uses it

All in `src\mod\render\native\native_shaders.cpp` (class `ShaderCache`, `Impl::ResolveDir`, `Impl::Locate`,
`Translator`) unless noted.

| Cvar | Default | Role |
|---|---|---|
| `pdz_native_renderer` | true | native renderer is the only output (null GPU backend) unless set otherwise (`native_cvars.cpp`) |
| `pdz_native_shader_dir` | "" = `<exe dir>\native_shaders` | first folder for `<hash>.dxil` + `.json` (+ `.hlsl`, `.bin`); misses dumped to `misses\` |
| `pdz_native_shader_fallback_dir` | `C:\Games\PDZRenderResearch\native\cache\hlsl` | second folder; its `..\ucode` and `..\manifest.json` are read too |
| `pdz_native_translator` | `C:\Games\PDZRenderResearch\native\tools\XenosRecomp\build\XenosRecomp\XenosRecomp.exe` | child process for a miss |
| `pdz_native_translate_sync` | -1 (on with the null backend, so on by default) | a draw waits for its translation |
| `pdz_native_translate_sync_ms` / `_frame_ms` | 1500 / 1500 | longest wait per shader / per frame |
| `pdz_native_dxc_dir` (`native_depth20e4.cpp`) | "" = the translator's folder, then the exe folder | `dxcompiler.dll` + `dxil.dll` for patched PS variants |
| `pdz_native_dxc_preload` | true | loads DXC on a worker at startup |
| `pdz_native_shader_file_warm` | true | reads every shader file once at startup so first-use loads hit the OS cache |
| `pdz_native_shader_record` | true | writes `record\bound_*.txt` (and microcode of sibling-built VS) for `warm_shader_cache.py` |
| `pdz_native_vs_sibling_variants` | true | a new VS variant that differs from a cached one only in stride / offset reuses its DXIL (no translation) |

Hard-coded research paths besides the cvars: `kResearchCache` in `native_blend7e3.cpp:58` and
`native_depth20e4.cpp:70` (the `.hlsl` search for patched variants).

**Lookup.** Pixel shaders by hash (`ShaderCache::FindPixel`). Vertex shaders by (created VS, declaration): the
binder (`native_binder.cpp`) makes the bound microcode, its hash is looked up, then a sibling variant is tried.
A loaded pixel shader is checked once by `coissue::Apply` (`native_coissue.cpp`). If it needs a fix (co-issue,
param gen, rcp, stacked array, cube) it is rebuilt from its `.hlsl` with DXC (about 12 ms) and cached in
`coissue\`. The `blend7e3` and `depth20e4` variants work the same way. So the `.hlsl` files and DXC are needed at
run time even with a full cache.

**On a miss.** The microcode is taken from guest memory, written to `<shader dir>\<hash>.bin`, and queued on the
`Translator` worker (one thread, one `XenosRecomp.exe` process at a time, below-normal priority). With
`pdz_native_translate_sync` on, the draw waits (bounded). Otherwise, or after a timeout or a failure, **the draw is
skipped** (a hole in the picture) and the microcode goes to `misses\`. Without a translator the log says
`translator '...' missing (unseen shader variants stay misses)` and every miss stays a skipped draw.

**Is a translator linked in?** No. The game runs `XenosRecomp.exe` as a child process (`CreateProcessA` in
`Translator::Translate`). The only compiler loaded in-process is the fork's DXC (`depth20e4::Compiler`,
`DxcCreateInstance`). `native_device.cpp` also loads `d3dcompiler_47.dll` (ships with Windows) for FXC-era helper
shaders; it can't make DXIL. The SDK GPU plugin (`rexgpu-xenos.dll`) has its own run-time translator for the
emulator path, which the native renderer doesn't use.

**Where the build folders come from.** No CMake rule makes `native_shaders` or `native_cache` (grep of
`CMakeLists.txt` and `cmake\`: nothing). The game creates them at run time: translations, patched variants,
records and miss dumps in `native_shaders`; `pso.lib` / `pso_recipes.bin` (`pdz_native_pso_cache`,
`native_pipeline.cpp`) and `texture_prewarm\` (`pdz_native_texture_prewarm`, `native_textures.cpp`) in
`native_cache`. `tools\build_variant.bat` copies the default build's `native_shaders` into a new build folder.
`pso_recipes.bin` holds the shaders' DXIL and `pso.lib` the driver's compiled PSOs, so `native_cache` is game
derived too. Never ship either folder.

**Existing warm-up features.** `pdz_native_pso_cache` / `pdz_native_pso_warmup` / `pdz_native_pso_prewarm`
(build every PSO ever made at startup, on worker threads, during the intros), `pdz_native_shader_file_warm`,
`pdz_native_dxc_preload`, and `pdz_native_texture_prewarm` (learned per level, used on the second play). None of
them translates shaders ahead of time. They all assume the DXIL already exists.

**Today's run (log 342, 31 Oct):** 362 distinct shaders loaded, 771 VS bindings, 0 misses, 0 translations; first
use cost 84 ms for all VS bindings and 120 ms for 193 PS, all from files.

## Release options

### (a) Generate at first run from the player's own game files

The game already has everything it needs except the scan: the packages are on disk (`game_data_root`), the DLC
under the user data root, and the exe image is loaded in guest memory (the 181 exe containers and the two raw
arrays at fixed addresses). A host-side scan finds the 1,639 static shaders and translates them.

| | Estimate |
|---|---|
| shaders made | 1,639 static (all 1,201 PS, 24 already bound VS, 415 unbound VS as parents); plus the 534 bound VS only with a bind list (below) |
| translate time | 5.4 s on 24 threads here (about 2 CPU-minutes); about 15-30 s on an 8-thread CPU such as the Deck (**estimate**, not measured) |
| scan time | 2-3 min in Python with ~4 GB RAM; a C++ scan that inflates one package at a time should be far less (**not measured**) |
| disk | about 60 MB (.hlsl 29 + .dxil 14 + .json 5 + microcode 5 + manifest 4), plus the variant folders as play adds them (~30 MB) |
| needed at run time | the fork (`XenosRecomp.exe` or linked in), `shader_common_pdz.hlsli`, `dxcompiler.dll` (17.1 MB), `dxil.dll` (1.4 MB) |

Licences: the fork is MIT (hedge-dev, our patches). `dxcompiler.dll` is the open-source DirectX Shader Compiler
(LLVM / NCSA licence). `dxil.dll` is a Microsoft binary that ships with each DXC release under Microsoft's own
licence, which games and emulators commonly redistribute. **Not verified here:** `thirdparty\dxc-bin` carries no
licence text, so the exact release and its terms must be checked, and the licence files shipped with the DLLs.

The gap is the 534 bound VS. Two ways to fill it:

1. **Bind list.** Ship a small table of (parent VS hash, declaration elements, stream strides). It holds hashes and
   vertex formats, no microcode. The generator runs `binder::Bind` on the player's own parent VS and translates
   the result. Today's records (`record\bound_*.txt`) store the declaration's guest address, not its elements, so
   the recorder must be changed to collect the table. Whether such a table is fine to ship is the user's call.
2. **Decode the declarations statically** from the `ndVertexBuffer2` nodes. Research first; size unknown.

The 4 video-player shaders can only come from (c). The very first frame draws one (`c2bdc141db1b3bff`, the Deck
miss), so one stall of about 80 ms at the intro video is expected.

### (b) A separate generator the player runs once

The same work as (a), in a separate exe or a mode of the game exe (`--pdz_build_shaders` that exits when done).
It still needs the exe image: either the game's own XEX loading (in the game exe) or the SDK's XEX code (in a
separate tool). It adds a step players can skip or get wrong (wrong folders, the TU or DLC added later). It suits a
launcher or installer. It needs the same DLLs. As a mode of the game exe it costs little on top of (a).

### (c) Translate on demand at first use

This exists today (`Translator`, `pdz_native_translate_sync`). It needs no scan and no bind list. It covers every
shader, the video ones included. The cost is stutter on the first play of each level:

| Measure | Value |
|---|---|
| one translation | 31-110 ms, one 1,047 ms (logs), plus about 12 ms for a co-issue rebuild |
| distinct shaders in one session | 362 (log 342) |
| bound VS per mission | 103-173 (`pdz_native_pso_cap` description); one mission makes 278-462 PSOs |
| translator throughput | one process at a time, one worker thread |

A level with 200-400 new shaders at about 60 ms each is 12-24 s of serial translation at its first sight. With the
sync wait each one freezes the frame (up to 1.5 s per frame by the budget). Without it each one is a hole until
done. The PSO prewarm then helps only from the second play, once the DXIL exists.

Cheaper on-demand: several translator workers; the fork linked in-process (no process start, DXC loaded once); and
a per-level learned prewarm like `pdz_native_texture_prewarm`: record the (VS, declaration) pairs and PS hashes a
level used, and on its next load translate them during the loading screen. That still leaves the first play of
every level with stalls.

## Recommendation

Do (a), with (c) kept as the safety net, and offer (b) as a command-line mode of the same code. The scan and
translation are seconds of work, the inputs are files the player must have anyway, and the on-demand path already
covers what the scan can't see (video shaders, a declaration nobody listed). Fill the bound VS with a bind list, or
accept learned VS variants on first play if a bind list is not wanted.

Work, in order (rough sizes):

1. **Paths** (small, an evening): `pdz_native_shader_dir` default to `<cache_root>\native_shaders` (the exe folder
   may be read-only under Steam or Program Files); `pdz_native_shader_fallback_dir` default empty;
   `pdz_native_translator` and `pdz_native_dxc_dir` default to the exe folder; remove `kResearchCache` in
   `native_blend7e3.cpp` and `native_depth20e4.cpp`; `manifest.json` and `ucode\` looked up under the new folder;
   `pdz_native_shader_record` off in a release.
2. **Ship the translator** (medium, 1-2 days): build the fork from upstream 990d03b plus patches 01-09 inside the
   CMake build (public fork or vendored), install `XenosRecomp.exe`, `shader_common_pdz.hlsli`, `dxcompiler.dll`,
   `dxil.dll` and their licence files next to the game exe. Check it starts under Proton (child process).
3. **Generator scan** (medium-large, 2-4 days): port `harvest.py` / `scan_packages.py` to C++ (inflate, CAFF
   directory, container finder), one package at a time; scan `PackageData`, the DLC folders and the exe image in
   guest memory; write `<hash>.bin` and a `manifest.json` with the fields the game reads (`derived_from`).
4. **Batch translate** (small-medium, a day): a worker pool over the scan's list (exe and front-end shaders first),
   during the intros and front end; a stamp file (fork version + package list and sizes) so it reruns only when the
   translator or the game files change (a DLC pack added later). Optionally apply the deterministic co-issue / rcp /
   param-gen patches here too, so DXC is needed less at play time.
5. **Bound VS** (medium, 1-2 days): record the declaration elements and strides (`pdz_native_shader_record`),
   collect a bind list over the regression runs, ship it, and bind with `binder::Bind` in the generator. Or skip it
   and rely on step 6.
6. **Better on-demand** (medium, 1-2 days): more translator workers, maybe the fork in-process, and a per-level
   learned shader prewarm on the loading screen.
7. **Test** (an evening): a clean folder with no research paths (`make_nohole_cache.ps1` pattern, fallback none), a
   whole mission and an MP map with zero misses after generation, then the Deck folder.

## Surprises and unverified points

- **`XenosRecomp.exe` is missing.** `tools\XenosRecomp\build\XenosRecomp\` has the DLLs and the `.pdb` but no exe
  (folder changed 30 Oct 14:10). Log 299 (14:09) found it and translated one VS in 78 ms; log 317 (21:47) says
  `missing`. So the play build can't translate a miss right now. Cause not checked. Rebuild with `ninja` in
  `tools\XenosRecomp\build` (BUILD_XENOSRECOMP.md).
- The cache holds 2,177 shaders, not about 1,600: 1,637 static containers plus the 534 bound VS and 6 others.
- `native_cache\pso_recipes.bin` stores DXIL, so it is game-derived and must not ship either.
- The `.hlsl` files are needed at run time (patched variants are rebuilt from them), not only the `.dxil`.
- Not verified: the `ndVertexBuffer2` declaration layout; the DXC release and licence in `thirdparty\dxc-bin`;
  scan and translate times on a slower CPU; that child processes behave under Proton.

**Why XenosRecomp.exe is missing (checked 31 Oct 00:40).** Windows Defender quarantined it as a threat
(threat ID 2147731250), first on 26 Oct 17:34 and again on 30 Oct 14:10 after the rebuild. It is a
false positive on a self-built, unsigned tool, but a release that ships the translator will meet the same thing on
players' machines. Plan for it: sign the translator, or link it into the game instead of running a separate exe,
and report a submission to Microsoft. Tonight's runs had no shader misses, so nothing waited on it.
