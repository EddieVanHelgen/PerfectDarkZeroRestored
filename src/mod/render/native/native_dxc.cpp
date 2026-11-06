// Native renderer: paths and the shader compiler (native_dxc.h).
#include "native_dxc.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <chrono>
#include <mutex>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_STRING(pdz_native_dxc_dir, "", "Render",
                      "Native renderer: folder with dxcompiler.dll + dxil.dll (DXC 1.8.2407, signed DXIL) for the "
                      "in-process shader translator and the patched pixel-shader variants. Empty = the executable's "
                      "folder (where the release ships them), then, in a developer's build, the XenosRecomp fork's "
                      "dxc-bin folder.");

namespace pdz::native
{
  namespace
  {
    std::mutex g_mutex;
    std::string g_cache_root;
    constexpr const char *kResearchCache = "C:\\Games\\PDZRenderResearch\\native\\cache\\hlsl";
    constexpr const char *kResearchDxc = "C:\\Games\\PDZRenderResearch\\native\\tools\\XenosRecomp\\thirdparty\\dxc-bin\\bin\\x64";

    std::wstring Widen(const std::string &s) { return std::filesystem::path(s).wstring(); }
  } // namespace

  void SetCacheRoot(const std::filesystem::path &cache_root)
  {
    std::lock_guard lock(g_mutex);
    g_cache_root = cache_root.string();
  }

  const std::string &CacheRoot()
  {
    std::lock_guard lock(g_mutex);
    return g_cache_root;
  }

  const std::string &ExeDir()
  {
    static const std::string dir = [] {
      wchar_t exe[MAX_PATH] = {};
      const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
      return n && n < MAX_PATH ? std::filesystem::path(exe).parent_path().string() : std::string(".");
    }();
    return dir;
  }

  bool DeveloperBuild()
  {
    static const bool dev = [] {
      std::error_code ec;
      std::filesystem::path dir = ExeDir();
      for (int i = 0; i < 6 && !dir.empty(); ++i)
      {
        if (std::filesystem::exists(dir / "perfectdarkzerorecomp_manifest.toml", ec))
        {
          return true;
        }
        if (!dir.has_parent_path() || dir.parent_path() == dir)
        {
          break;
        }
        dir = dir.parent_path();
      }
      return false;
    }();
    return dev;
  }

  std::string ResearchCacheDefault() { return DeveloperBuild() ? std::string(kResearchCache) : std::string(); }

  std::string NativeCacheDir()
  {
    return (DeveloperBuild() || CacheRoot().empty() ? ExeDir() : CacheRoot()) + "\\native_cache";
  }

  namespace dxc
  {
    namespace
    {
      bool g_tried = false;
      void *g_create = nullptr;
      std::string g_from;
    } // namespace

    void *CreateProc()
    {
      std::lock_guard lock(g_mutex);
      if (g_tried)
      {
        return g_create;
      }
      g_tried = true;
      const auto t0 = std::chrono::steady_clock::now();
      std::vector<std::string> dirs;
      if (!REXCVAR_GET(pdz_native_dxc_dir).empty())
      {
        dirs.push_back(REXCVAR_GET(pdz_native_dxc_dir));
      }
      else
      {
        dirs.push_back(ExeDir());
        if (DeveloperBuild())
        {
          dirs.push_back(kResearchDxc);
        }
      }
      std::string tried;
      for (const std::string &d : dirs)
      {
        tried += (tried.empty() ? "" : "; ") + d;
        const std::filesystem::path dxcompiler = std::filesystem::path(d) / "dxcompiler.dll";
        const std::filesystem::path dxil = std::filesystem::path(d) / "dxil.dll";
        std::error_code ec;
        if (!std::filesystem::exists(dxcompiler, ec) || !std::filesystem::exists(dxil, ec))
        {
          continue;
        }
        if (!LoadLibraryW(dxil.wstring().c_str()))
        {
          REXLOG_WARN("pdz_native: can't load {} ({}): DXIL would be unsigned", dxil.string(), uint32_t(GetLastError()));
          continue;
        }
        HMODULE module = LoadLibraryW(dxcompiler.wstring().c_str());
        void *create = module ? reinterpret_cast<void *>(GetProcAddress(module, "DxcCreateInstance")) : nullptr;
        if (!create)
        {
          REXLOG_WARN("pdz_native: can't load DxcCreateInstance from {} ({})", dxcompiler.string(), uint32_t(GetLastError()));
          continue;
        }
        g_create = create;
        g_from = d;
        REXLOG_INFO("pdz_native: shader compiler (DXC) loaded from {} in {} ms", d,
                    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
        return g_create;
      }
      REXLOG_WARN("pdz_native: no dxcompiler.dll + dxil.dll found (tried: {}): shaders can't be translated or patched",
                  tried.empty() ? std::string("-") : tried);
      return nullptr;
    }

    std::string LoadedFrom()
    {
      std::lock_guard lock(g_mutex);
      return g_from;
    }
  } // namespace dxc
} // namespace pdz::native
