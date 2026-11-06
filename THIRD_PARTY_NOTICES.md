# PDZ Restored: third-party components shipped in the release

PDZ Restored contains no game files and no translated shaders. The player supplies their own Perfect Dark Zero disc
and Title Update 3; the native renderer's shaders are made from them on the player's PC at first start.

PDZ Restored's own code, and the PerfectDarkZeroRecomp project it is built from (Nikolai Gorb), are under the MIT
licence in `LICENSE`. Everything else in the release is listed below, with its licence text under `licenses\`.
The list was taken from the play build's link lines and linker map (`PDZRestored.exe`, `rexruntime.dll`,
`rexgpu-xenos.dll`), not from the source tree: libraries the SDK has in its source but does not build into these
three files are not listed (see the end).

## Components

Where: E = `PDZRestored.exe`, R = `rexruntime.dll`, G = `rexgpu-xenos.dll`.

| Component | Used for | Where | Licence | Copyright | Licence file |
|---|---|---|---|---|---|
| ReXGlue SDK | the runtime the recompiled game runs on | E, R, G | BSD-3-Clause | (c) 2026 Tom Clay; portions (c) 2022 Ben Vanik and Xenia project contributors | `licenses\rexglue-sdk\LICENSE` |
| Xenia | the SDK and parts of the native renderer are derived from it | E, R, G | BSD-3-Clause | (c) 2015 Ben Vanik | `licenses\xenia\LICENSE` |
| XenosRecomp, pdz branch (with the project's patches) | the shader translator built into the exe | E | MIT | (c) 2025 hedge-dev and contributors | `licenses\xenosrecomp\LICENSE.md` |
| {fmt} | text formatting | E, R, G | MIT (with the {fmt} binary exception) | (c) 2012 - present, Victor Zverovich and {fmt} contributors | `licenses\fmt\LICENSE` |
| spdlog | logging | E, R, G | MIT | (c) 2016 - present, Gabi Melman and spdlog contributors | `licenses\spdlog\LICENSE` |
| SDL 3.4.14 (one local change, see below) | window, input, audio output | R | zlib | (C) 1997-2026 Sam Lantinga | `licenses\sdl3\LICENSE.txt` |
| SDL_GameControllerDB | the controller list `gamecontrollerdb.txt` | file | zlib | (C) 1997-2025 Sam Lantinga | `licenses\sdl-gamecontrollerdb\LICENSE` |
| Rajdhani (Bold) | the display font of the settings and setup windows (embedded) | E | SIL Open Font License 1.1 | (c) 2014 Indian Type Foundry | `licenses\rajdhani\OFL.txt` |
| Dear ImGui 1.92.5 | the overlay and settings screens | R | MIT | (c) 2014-2025 Omar Cornut | `licenses\imgui\LICENSE.txt` |
| FFmpeg 4.4 (libavcodec, libavutil; Xenia's fork) | XMA / WMA / MP3 audio decoding | R | LGPL 2.1 or later (see below) | the FFmpeg developers | `licenses\ffmpeg\LICENSE.md`, `licenses\ffmpeg\COPYING.LGPLv2.1` |
| libmspack (LZX decoder only) | unpacking the game's compressed executable | R | LGPL 2.1 (see below) | (C) 2003-2023 Stuart Caie | `licenses\libmspack\COPYING.LIB.txt` |
| xxHash | hashing | E, R, G | BSD-2-Clause | (c) 2012-2021 Yann Collet | `licenses\xxhash\LICENSE` |
| o1heap | guest heap allocator | R | MIT | (c) 2020 Pavel Kirienko | `licenses\o1heap\LICENSE` |
| aes_128 | AES (kernel crypto calls) | R | MIT | (c) 2017 LuoPeng | `licenses\aes_128\LICENSE` |
| TinySHA1 | SHA-1 (kernel crypto calls) | R | ISC-style (notice in file) | (c) 2012-22 Saurav Mohapatra | `licenses\crypto\TinySHA1.txt` |
| DES (Faraz Fallahi) | DES (kernel crypto calls) | R | MIT | (c) 2014 Faraz Fallahi | `licenses\crypto\des-LICENSE` |
| SHA-256 (Stephan Brumme) | SHA-256 (kernel crypto calls) | R | zlib (notice in file) | (c) 2014, 2015 Stephan Brumme | `licenses\crypto\sha256.txt` |
| rijndael-alg-fst | AES for executable decryption | R | public domain | Rijmen, Bosselaers, Barreto | `licenses\crypto\rijndael-alg-fst.txt` |
| CLI11 | command-line options | R | BSD-3-Clause | (c) 2017-2025 University of Cincinnati, developed by Henry Schreiner under NSF AWARD 1414736 | `licenses\cli11\LICENSE` |
| toml++ | settings files | R | MIT | (c) Mark Gillard | `licenses\tomlplusplus\LICENSE` |
| utfcpp | UTF-8 conversion | R | Boost Software License 1.0 | (c) 2006 Nemanja Trifunovic | `licenses\utfcpp\LICENSE` |
| disruptorplus | timer queue ring buffer | R | MIT | (c) 2013 Lewis Baker | `licenses\disruptorplus\LICENSE.txt` |
| stb_image v2.29 (zlib decoder, image decoding) | PNG / zlib decoding | E, R | MIT or public domain (choose) | (c) 2017 Sean Barrett | `licenses\stb\LICENSE.txt` |
| SIMD Everywhere (SIMDe) | vector instructions in the recompiled code | E, R, G | MIT | (c) 2017 Evan Nemerson | `licenses\simde\COPYING` |
| SMAA (lookup textures, shaders) | anti-aliasing | E, G | MIT | (C) 2013 Jorge Jimenez, Jose I. Echevarria, Belen Masia, Fernando Navarro, Diego Gutierrez | `licenses\smaa\LICENSE.txt` |
| NVIDIA FXAA 3.11 (compiled shaders) | anti-aliasing | E, G | BSD-3-Clause | (c) 2014-2015, NVIDIA CORPORATION | `licenses\fxaa\LICENSE` |
| AMD FidelityFX CAS (compiled shaders) | sharpening / scaling on present | R | MIT | (c) 2020 Advanced Micro Devices, Inc. | `licenses\fidelityfx-cas\LICENSE.txt` |
| AMD FidelityFX FSR 1 (compiled shaders) | upscaling on present | R | MIT | (c) 2021 Advanced Micro Devices, Inc. | `licenses\fidelityfx-fsr\license.txt` |
| glslang (with its SPIR-V builder) | SPIR-V generation for the Vulkan path | G | BSD-3-Clause and others (all in the file) | (C) 2015-2018 Google, Inc. and the other holders named in the file | `licenses\glslang\LICENSE.txt` |
| Vulkan Memory Allocator | Vulkan memory | R, G | MIT | (c) 2017-2025 Advanced Micro Devices, Inc. | `licenses\vulkan-memory-allocator\LICENSE.txt` |
| Vulkan-Headers | Vulkan declarations | R, G | Apache-2.0 or MIT | (c) 2015-2026 The Khronos Group Inc. | `licenses\vulkan-headers\` |
| SPIRV-Tools header (`libspirv.h` only) | optional SPIR-V validation (uses the Vulkan SDK's library if installed; none shipped) | R | Apache-2.0 | (c) 2015-2020 The Khronos Group Inc. | `licenses\spirv-tools\LICENSE` |
| RenderDoc in-application API header | optional RenderDoc captures | R, G | MIT | (c) 2015-2016 Baldur Karlsson | `licenses\renderdoc\LICENSE.txt` |
| DXBC checksum (AMD, derived from RSA MD5) | checksums for translated D3D12 shaders | G | AMD notice and RSA MD5 licence (see below) | (c) 2008-2016 Advanced Micro Devices, Inc.; (C) 1990 RSA Data Security, Inc. | `licenses\dxbc-checksum\NOTICE.txt` |
| DirectX Shader Compiler `dxcompiler.dll` 1.8.2407.7 (and the DXC headers built into E, R, G) | shader compiling | file next to the exe | LLVM / University of Illinois NCSA | (c) 2003-2015 University of Illinois at Urbana-Champaign | `licenses\dxc\LICENSE-LLVM.txt` |
| DirectX Shader Compiler `dxil.dll` 101.8.2407.12 | shader signing | file next to the exe | Microsoft Software License Terms (distributable code) | Microsoft Corporation | `licenses\dxc\LICENSE-MS.txt` |

## DirectX Shader Compiler (`dxcompiler.dll`, `dxil.dll`)

`dxcompiler.dll` and `dxil.dll` are the only Microsoft files in the release. They are the redistributable DXC
binaries from the official release v1.8.2407 (`dxc_2024_07_31.zip`), unmodified and byte-identical to Microsoft's
(SHA-256 `1220478c...f492d1` and `e038a975...f8ac93`).

**Terms for `dxil.dll`.** `dxil.dll` is Microsoft software, distributed with PDZ Restored under the Microsoft
Software License Terms for the Microsoft DirectX Shader Compiler (`licenses\dxc\LICENSE-MS.txt`). By using or
redistributing PDZ Restored you agree to those terms for `dxil.dll`, including that it is licensed, not sold, and
that you will not reverse engineer, decompile or disassemble it, remove its notices, or use or share it other than
as part of PDZ Restored. Microsoft provides it "as is", without warranty, and is not responsible for PDZ Restored.
PDZ Restored is not made, endorsed or supported by Microsoft.

The release ships no Visual C++ runtime DLLs: install the Microsoft Visual C++ Redistributable (x64).

## FFmpeg and libmspack (LGPL)

`rexruntime.dll` contains FFmpeg's libavcodec and libavutil (FFmpeg 4.4, Xenia's fork, configured with
`--disable-everything` and only the mp3, wmav2, wmapro and xmaframes decoders; `CONFIG_GPL 0`, `CONFIG_NONFREE 0`,
`FFMPEG_LICENSE "LGPL version 2.1 or later"`) and the LZX decoder of libmspack (LGPL 2.1). Both are linked
statically into `rexruntime.dll`. You may modify these libraries and use your modified versions with PDZ Restored:
the complete source of FFmpeg and libmspack as built, and the source of the rest of `rexruntime.dll` so it can be
rebuilt and relinked with them, is at: (source link to be added before release).

## Local changes to SDL

SDL 3.4.14 is used with one change by the PDZ Restored project, marked in the source:
`src/video/windows/SDL_windowsevents.c` reads mouse buttons 6-32 from `RAWMOUSE.ulRawButtons` (SDL reads only
buttons 1-5). This is an altered version of SDL, not the original. The SDK file `src/ui/win32_scancodes.inc` holds a
copy of SDL's Windows scan code table (`src/events/scancodes_windows.h`, zlib licence, (C) 1997-2026 Sam Lantinga),
altered (table only) so the settings window names keys as SDL does.

## RSA MD5 notice

The DXBC checksum code in `rexgpu-xenos.dll` is derived from the RSA Data Security, Inc. MD5 Message Digest
Algorithm.

## Not in the release

In the SDK's source but not built into the three files: Tracy (not compiled in), the Vulkan loader (the system's
`vulkan-1.dll` is used if present), SPIRV-Tools' library, MoltenVK, Catch2, inja, tiny-AES-c, the PowerPC
disassembler (used only by the build-time code generator) and the SDK's code generator itself.
