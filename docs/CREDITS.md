# Credits

This project builds on other people's work. Perfect Dark Zero itself, its code and all of its assets belong to
**Rare** and **Microsoft**; nothing of the game is in this repository (you supply your own copy, see README.md).

## The recompilation

| Who | What | Licence |
|---|---|---|
| **nikolaygorb** (Nikolai Gorb) | [PerfectDarkZeroRecomp](https://github.com/nikolaygorb/PerfectDarkZeroRecomp), the recompilation this fork is built on: the project, its hooks, the third-person camera, mouse support, language selection, profile menu | MIT ([LICENSE](../LICENSE)) |
| **2023PerfectDark** | The local profile manager with isolated saves and its persistence tests (`e37081c`, merged upstream as pull request #8) | MIT (contributed upstream) |
| **The ReXGlue SDK authors** (Tom Clay and contributors) | [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk), the recompilation toolkit and runtime (`thirdparty/rexglue-sdk`, via nikolaygorb's fork plus this project's commits) | BSD-3-Clause |
| **The Xenia project** (Ben Vanik and contributors) | The emulator code the ReXGlue runtime is derived from. Some native renderer files reimplement its formulas (texture addressing, 20e4 depth, the resolve clear, microcode layouts); they say so in their headers | BSD-3-Clause |
| **Rare** | Perfect Dark Zero | proprietary (not included) |

## Findings and ideas from other projects

Nothing from the projects below is copied: each entry says what was taken (an observation, an idea) and where our
own implementation is.

| Who | What | Where in this repo |
|---|---|---|
| **Perfect Dark Zero: Reinstated** (2023PerfectDark / PerfectDark023) | Found that Rare's Havok world is stepped once per frame with a fixed 1/60 s solver step, so physics runs at double speed at 120 fps ("PDZ physics timing: frame 0.007790s, solver 0.016667s -> 0.007790s"). Their source isn't public; our fix is our own, from TU3's code | `src/mod/physics_step.cpp` (`pdz_physics_frame_step`) |
| xenia-canary/game-patches | The 60 FPS and 16:9 patches' origin | README.md Credits |

## Third-party code and data in this repository

| What | Where | Licence |
|---|---|---|
| SDL_GameControllerDB | `settings/gamecontrollerdb.txt` | zlib |
| [XenosRecomp](https://github.com/hedge-dev/XenosRecomp) (hedge-dev and contributors), the shader translator built into the exe, with this project's `pdz` commits ([EddieVanHelgen/XenosRecomp](https://github.com/EddieVanHelgen/XenosRecomp), branch `pdz`) | `thirdparty/XenosRecomp-pdz` (vendored, see its `README_PDZ.md`; docs/RELEASE_SHADERS.md) | MIT |

## Third-party libraries used through the SDK

Built from `thirdparty/rexglue-sdk/thirdparty` (git submodules from their own repositories, or vendored with their
licence files): SDL3, fmt, spdlog, toml++, xxHash, SIMDe, glslang, SPIRV-Tools / SPIRV-Headers, Vulkan headers /
loader / MoltenVK, Vulkan Memory Allocator, Dear ImGui, CLI11, Catch2, utfcpp, inja, o1heap, Tracy, libmspack,
FFmpeg (wmarti fork), FidelityFX CAS / FSR, SMAA, FXAA, stb, tiny-AES-c, the DirectX Shader Compiler headers
(`dxcapi.h`), disruptorplus, RenderDoc's API header. Each keeps its own licence; see the SDK's `thirdparty/` folder.

## Tools

- [ABGX360](https://github.com/BakasuraRCE/abgx360) and [extract-xiso](https://github.com/XboxDev/extract-xiso) for
  dumping your own copy of the game.
- Development was assisted by Claude (Anthropic); those commits carry a `Co-Authored-By: Claude` line.
