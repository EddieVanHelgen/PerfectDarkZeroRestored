// Native renderer: where its files live, and the shader compiler (2026-11-03, docs/RELEASE_FIRST_RUN.md Stage 2a).
//
// Paths. A release (no repo around the exe) keeps everything the renderer makes from the player's game files in the
// per-user cache (<cache root>\native_shaders). A developer's build (the exe inside the repo: a
// perfectdarkzerorecomp_manifest.toml up the tree, as the play copy has too) keeps today's places: <exe dir>\
// native_shaders, and the research cache as the fallback.
//
// DXC. dxcompiler.dll + dxil.dll (1.8.2407.7 / 101.8.2407.12, shipped next to PDZRestored.exe) are loaded once, by full
// path, dxil.dll first (dxcompiler loads "dxil.dll" by name to sign, and the loaded module is what it gets). The
// in-process translator and the patched pixel-shader variants (co-issue, blend7e3, depth20e4) all use it.
#pragma once

#include <filesystem>
#include <string>

namespace pdz::native
{
  // From the app (OnConfigurePaths): the resolved cache root (<user data root>\cache unless --cache_root).
  void SetCacheRoot(const std::filesystem::path &cache_root);
  const std::string &CacheRoot();
  const std::string &ExeDir();
  // The exe runs inside the repo (or the play copy): research paths may be used as defaults.
  bool DeveloperBuild();
  // The research cache (C:\Games\PDZRenderResearch\native\cache\hlsl) in a developer build, else "".
  std::string ResearchCacheDefault();
  // The PSO cache / texture prewarm folder: <cache root>\native_cache in a release (game-derived, per user; the exe's
  // folder may be read-only), <exe dir>\native_cache in a developer's build.
  std::string NativeCacheDir();

  namespace dxc
  {
    // DxcCreateInstance of the loaded dxcompiler.dll, or nullptr (logged once with the folders tried). Thread-safe.
    void *CreateProc();
    // The folder it was loaded from ("" if not).
    std::string LoadedFrom();
  } // namespace dxc
} // namespace pdz::native
