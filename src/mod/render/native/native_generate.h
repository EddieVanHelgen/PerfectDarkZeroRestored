// Native renderer: the shader cache made on the player's machine from their own game files (2026-11-03,
// docs/RELEASE_FIRST_RUN.md Stage 2b). A release ships no translated shaders; the first start makes them, the same
// way the developer's cache was made (PDZRenderResearch\native\tools\harvest\harvest.py + translate_pdz.py):
//   1. scan, in harvest's order (first container per microcode hash wins): the exe image's containers and its two raw
//      microcode arrays, the retail packages (sorted by name), then the DLC and title update packages (by path);
//   2. translate every one with the XenosRecomp fork built into the exe: static shaders from their full container
//      (literals and interpolators), the raw arrays from their microcode;
//   3. write <hash>.hlsl / .json / .dxil, the microcode <hash>.bin and the container containers\<hash>.bin into the
//      per-user shader folder, and generated.json, which makes it run again only when the game files, the exe, the
//      translator or DXC change.
// Two phases: the packages before the renderer starts (OnPreSetup: a failure there starts the emulated renderer),
// the exe image once it is loaded in guest memory (OnPostLoadXexImage).
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace pdz::native::generate
{
  struct Inputs
  {
    std::filesystem::path game_root;      // game:\ (pdz\ASSETS\PackageData)
    std::filesystem::path update_root;    // update:\ (title update packages)
    std::filesystem::path user_data_root; // DLC: 0000000000000000\...
    std::string exe_build;                // PdzBuildCommit(): a new exe makes it run again
  };

  // Phase 1 (OnPreSetup). False: the shaders couldn't be made (`why` says why); the caller starts the emulated
  // renderer. True when nothing was needed (a developer's build, or generated.json matches) or it succeeded.
  bool PreparePackages(const Inputs &in, std::string &why);

  // Phase 2 (OnPostLoadXexImage): the exe image's containers and raw arrays, from guest memory.
  void PrepareExe(const uint8_t *membase, uint32_t image_base, uint32_t image_size);
} // namespace pdz::native::generate

namespace pdz::native::pipeline_seed
{
  // The shipped pipeline list prepared once after the shaders are made (native_pipeline.h, native_pipeline_seed.cpp).
  void PrepareAtFirstStart();
} // namespace pdz::native::pipeline_seed
