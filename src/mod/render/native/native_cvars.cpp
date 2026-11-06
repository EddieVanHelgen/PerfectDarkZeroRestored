// Native renderer switches (pattern: src\game_cvars.cpp, REXCVAR_DEFINE_*). Declared in native.h.
#include "native.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <string>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dxgi1_2.h>

#include <rex/logging.h>
#include <rex/cvar.h>

#include <fmt/format.h>

REXCVAR_DEFINE_BOOL(pdz_native_renderer, true, "Render",
                    "Native D3D12 renderer. Default (2026-10-29: native is the default, the emulator the "
                    "fallback): when this is set nowhere (not in a settings file, the environment or the command "
                    "line), the native renderer is the only output, as the play launcher used to start it: the null "
                    "GPU backend and the native render profile (resolution scale from the desktop height: 3 at 2160p, "
                    "2 at 1440p / 1080p, 1 below; record thread; cached untiling; each only where it is not set; "
                    "shadows stay Rare's stock: the pdz_shadow_* extension is an optional enhancement), whatever graphics_backend "
                    "the settings file names (that one is the emulator's, for the fallback). A graphics_backend other "
                    "than null on the command line instead starts the emulator alone. false = the emulator renders, "
                    "no native work at all (the fallback: exactly the behaviour before the switch). true, given "
                    "explicitly = the stage-1 behaviour: the native path runs on the backend graphics_backend selects "
                    "(d3d12: alongside the GPU plugin, F9 / bind_native_output picks the frame shown; null: alone).");

REXCVAR_DEFINE_INT32(pdz_native_scale, 1, "Render",
                     "Native renderer: integer resolution scale (1..4), the SDK's draw resolution scale for the native "
                     "path: every render target, depth buffer and resolve mirror is guest size x N, viewports, "
                     "scissors, clears, resolves, point sizes and the polygon-offset slope follow, the frame is "
                     "presented at N x (the emulator's resolution_scale = 2 presents 2560x1440: use 2 to compare "
                     "like for like). Latched when the first host surface is created (a change needs a restart); "
                     "with pdz_native_scale_follow in effect (default: graphics_backend null) it is chosen at startup "
                     "instead, and the GPU plugin's resolution_scale is used unless this is set explicitly (config, "
                     "environment or command line, even to 1). CORE_NOTES.md 'Resolution scale' lists what scales "
                     "and what is approximate.")
    .range(1, 4);

REXCVAR_DEFINE_INT32(pdz_native_scale_follow, 1, "Render",
                     "Native renderer: take the resolution scale from the GPU plugin's resolution_scale (or "
                     "draw_resolution_scale_x / _y, as the plugin reads them) at startup, unless pdz_native_scale is "
                     "set explicitly. 0 = never (pdz_native_scale alone, latched at the first host surface), 1 = with "
                     "graphics_backend null only (the native output is the only output: render at the user's scale), "
                     "2 = with the GPU plugin too (like-for-like auto-diff at the user's scale). The native renderer "
                     "scales X and Y alike, 1..4: a larger or uneven plugin scale is clamped (logged).")
    .range(0, 2)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_INT32(pdz_shadow_map_scale, 1, "Render",
                     "Native renderer only, enhancement (polish 6, with pdz_shadow_distance_scale): the sun's shadow "
                     "maps (the game's 1344 x 1344 depth surface and colour target of the shadow pass and the four "
                     "1344 x 1344 k_24_8 resolve textures, both cascades) are rendered at this many times the "
                     "native renderer's scale, so the far cascade stretched by pdz_shadow_distance_scale keeps its "
                     "texel density (distance 3 needs 3; 2 = 2688 x 2688 at pdz_native_scale 1, 8064 x 8064 at 3). "
                     "The guest keeps its 1344 x 1344 EDRAM surface, viewport and shader constants (the receivers "
                     "sample in normalized coordinates; no texel-size constant). Integer, as the native resolution "
                     "scale; the product is capped at 12 (16128 x 16128, the D3D12 texture limit 16384). Memory: the "
                     "six surfaces / textures take 6 x (1344 x product)^2 x 4 bytes: product 3 = 390 MB, 6 = 1.6 GB, "
                     "9 = 3.5 GB, 12 = 6.2 GB. 1 = original. Latched with the first shadow surface (a change needs a "
                     "restart). The GPU plugin path ignores it (its resolution_scale scales every target)")
    .range(1, 4)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_INT32(pdz_native_present_mode, 0, "Render",
                     "Native renderer: when the native frame is handed to the presenter. 0 = in the Swap hook "
                     "right after the guest Swap (the plugin's own present of the same frame lands a few ms "
                     "later and may replace it), 1 = at the first native call of the next frame (lands after "
                     "the plugin's present of the previous frame; which of the two the window shows depends "
                     "on the vblank phase and is stable within a session)")
    .range(0, 1)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_INT32(pdz_native_output_toggle_s, 0, "Render",
                     "Native renderer test aid: toggle the shown output (native / emulated) every N seconds of wall-clock time, as F9 "
                     "does. 0 = off.");
REXCVAR_DEFINE_INT32(pdz_native_capture_s, 0, "Render",
                     "Native renderer test aid: write the presenter's guest output every N seconds (max 64 files). 0 = off.");
REXCVAR_DEFINE_STRING(pdz_native_capture_swaps, "", "Render",
                      "Native renderer test aid: capture points at exact guest swap numbers instead of the "
                      "pdz_native_capture_s ticks (it overrides them). Comma-separated items: N (swap N), A-B/S "
                      "(A, A+S, ... up to B) or A/S (A, A+S, ... to the end of the run), e.g. \"3000,3600,4200\" or "
                      "\"2400-6000/120\". Swaps count from 1 at the first guest Swap, as the auto-diff CSV's swap "
                      "column and the shadow assert 'frame N' lines do. One capture is in flight at a time: a point "
                      "less than about 5 swaps after the previous one may be skipped (logged); swap 1 cannot be "
                      "captured. Read at the first swap with the native renderer on. Empty = off.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(pdz_native_capture_dir, "", "Render",
                      "Native renderer test aid: folder for pdz_native_capture_s / pdz_native_capture_swaps files "
                      "(empty = working directory).");
REXCVAR_DEFINE_BOOL(pdz_native_output, false, "Render",
                    "Native renderer: start with the native frame selected for display (F9 toggles)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace pdz::native
{
  namespace
  {
    struct HardwareGuess
    {
      uint32_t desktop_width = 0, desktop_height = 0; // the primary display's current mode (0 = unknown)
      uint64_t vram_mb = 0;                           // the largest hardware adapter's dedicated memory (0 = unknown)
      std::string gpu_name = "unknown";
    };

    // Startup, before the GPU plugin loads: the primary display mode and the hardware adapter with the most dedicated
    // memory (the SDK's D3D12 provider picks by vendor preference, then the largest DedicatedVideoMemory). DXGI is
    // loaded by name so the exe does not need dxgi.lib.
    HardwareGuess GuessHardware()
    {
      HardwareGuess g;
      DEVMODEW mode{};
      mode.dmSize = sizeof(mode);
      if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode))
      {
        g.desktop_width = mode.dmPelsWidth;
        g.desktop_height = mode.dmPelsHeight;
      }
      using CreateFactoryFn = HRESULT(WINAPI *)(REFIID, void **);
      HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
      auto create = dxgi ? reinterpret_cast<CreateFactoryFn>(
                               reinterpret_cast<void *>(GetProcAddress(dxgi, "CreateDXGIFactory1")))
                         : nullptr;
      IDXGIFactory1 *factory = nullptr;
      if (create && SUCCEEDED(create(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory))) && factory)
      {
        IDXGIAdapter1 *adapter = nullptr;
        for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
        {
          DXGI_ADAPTER_DESC1 desc{};
          if (SUCCEEDED(adapter->GetDesc1(&desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
              desc.DedicatedVideoMemory / (1024 * 1024) > g.vram_mb)
          {
            g.vram_mb = desc.DedicatedVideoMemory / (1024 * 1024);
            std::wstring w(desc.Description);
            g.gpu_name.assign(w.begin(), w.end()); // ASCII adapter names; others lose their accents in the log
          }
          adapter->Release();
        }
        factory->Release();
      }
      return g;
    }
  } // namespace

  // The native default (2026-10-29), from the app's OnPreSetup (settings file, environment and command line
  // read; GPU plugin not loaded yet). PDZ_PLAY.bat's render set, applied only to cvars nothing has set.
  bool ApplyNativeDefault(std::string &backend)
  {
    using rex::cvar::GetFlagSource;
    using rex::cvar::Source;
    if (GetFlagSource("pdz_native_renderer") != Source::kDefault)
    {
      return false; // set somewhere: its meaning before the default changed
    }
    if (GetFlagSource("graphics_backend") == Source::kCommandLine && backend != "null")
    {
      rex::cvar::SetFlagByName("pdz_native_renderer", "false");
      REXLOG_INFO("pdz_native: native default: graphics_backend {} on the command line, so the emulator renders alone "
                  "(pdz_native_renderer off)",
                  backend);
      return false;
    }
    struct Setting
    {
      const char *name;
      const char *value;
    };
    // Release safety (performance pass): the resolution scale follows the desktop height (3 at 2160p, 2 at 1440p /
    // 1080p, 1 at 800p and below), capped on GPUs with little dedicated memory (2 under 6 GB, 1 under 3 GB). The
    // developer's PC (2160p, 32 GB) gets scale 3, as PDZ_PLAY.bat had. Shadows: Rare's stock (decision
    // 2026-10-29): the extension (pdz_shadow_distance_scale / pdz_shadow_map_scale / pdz_shadow_far_pcf) is not part
    // of the profile; its cvar defaults are the stock values.
    const HardwareGuess hw = GuessHardware();
    uint32_t scale = hw.desktop_height ? std::clamp<uint32_t>((hw.desktop_height + 360) / 720, 1, 3) : 3;
    if (hw.vram_mb && hw.vram_mb < 6 * 1024)
    {
      scale = std::min<uint32_t>(scale, hw.vram_mb < 3 * 1024 ? 1 : 2);
    }
    const std::string scale_text = fmt::format("{}", scale);
    const Setting kProfile[] = {
        {"pdz_native_scale", scale_text.c_str()},
        {"pdz_native_record_thread", "true"},
        {"pdz_native_texture_untile_cached", "true"},
    };
    REXLOG_INFO("pdz_native: native default for this PC: desktop {}x{}, GPU '{}' {} MB dedicated -> resolution scale "
                "{} (unless set); shadows: Rare's stock unless the pdz_shadow_* extension is set",
                hw.desktop_width, hw.desktop_height, hw.gpu_name, hw.vram_mb, scale);
    std::string applied, kept;
    for (const Setting &s : kProfile)
    {
      std::string &out = GetFlagSource(s.name) == Source::kDefault && rex::cvar::SetFlagByName(s.name, s.value)
                             ? applied
                             : kept;
      out += fmt::format("{}{} {}", out.empty() ? "" : ", ", s.name, rex::cvar::GetFlagByName(s.name));
    }
    const std::string settings_backend = backend;
    backend = "null";
    rex::cvar::SetFlagByName("graphics_backend", "null");
    REXLOG_INFO("pdz_native: native default: the native renderer is the only output (null GPU backend; the settings' "
                "graphics_backend {} is the emulator's, used with --pdz_native_renderer=false); profile: {}{}{}",
                settings_backend, applied.empty() ? "none" : applied, kept.empty() ? "" : "; already set: ", kept);
    return true;
  }

  namespace
  {
    // 0 = not latched yet. Written by ChooseScaleAtStartup (app setup, before any guest thread) or by the first
    // Scale() call (a game thread, the render mutex held).
    uint32_t g_scale = 0;

    void LogScaleDetail(uint32_t scale)
    {
      if (scale > 1)
      {
        REXLOG_INFO("pdz_native: resolution scale {}: host surfaces and resolve mirrors at {}x the guest size; "
                    "resolves with the SDK's half-pixel fill (pdz_native_scale_fill_half_pixel); the auto-diff scores "
                    "the {}x frame and, on a second line, both frames box-filtered to the guest size",
                    scale, scale, scale);
      }
    }

    // The shadow surfaces' scale / Scale(), for the sun shadow setup's bias factor (sun_shadows.cpp, a game thread):
    // 1 until the first shadow surface latches it.
    std::atomic<float> g_shadow_relative{1.0f};

    // A plugin cvar by name (it lives in the GPU plugin DLL); -1 when it is not registered or not a number.
    int32_t FlagInt(const char *name)
    {
      const std::string value = rex::cvar::GetFlagByName(name);
      int32_t v = -1;
      if (value.empty() || std::from_chars(value.data(), value.data() + value.size(), v).ec != std::errc())
      {
        return -1;
      }
      return v;
    }
  } // namespace

  uint32_t Scale()
  {
    if (!g_scale)
    {
      g_scale = uint32_t(std::clamp(REXCVAR_GET(pdz_native_scale), 1, 4));
      REXLOG_INFO("pdz_native: resolution scale {} latched", g_scale);
      LogScaleDetail(g_scale);
    }
    return g_scale;
  }

  uint32_t SurfaceScale(uint32_t width, uint32_t height)
  {
    const uint32_t scale = Scale();
    if (width != kShadowMapSize || height != kShadowMapSize)
    {
      return scale;
    }
    // 0 = not latched yet; the first shadow surface or mirror latches it (render mutex held, like Scale()). The product
    // is capped at kMaxShadowScale: 1344 x 12 = 16128, the largest multiple under D3D12's 16384 texture limit.
    constexpr uint32_t kMaxShadowScale = 16384 / kShadowMapSize; // 12
    static uint32_t shadow_scale = 0;
    if (!shadow_scale)
    {
      const uint32_t k = uint32_t(std::clamp(REXCVAR_GET(pdz_shadow_map_scale), 1, 4));
      shadow_scale = std::min(scale * k, kMaxShadowScale);
      g_shadow_relative.store(float(shadow_scale) / float(scale), std::memory_order_relaxed);
      if (k > 1)
      {
        const uint64_t side = uint64_t(kShadowMapSize) * shadow_scale;
        REXLOG_INFO("pdz_native: shadow map scale {} (pdz_shadow_map_scale): the {}x{} shadow surfaces and their "
                    "resolve mirrors at {}x the guest size ({}x{} host, about {} MB for the six; the native resolution "
                    "scale is {}{})",
                    k, kShadowMapSize, kShadowMapSize, shadow_scale, side, side, 6 * side * side * 4 / 1000000,
                    scale, scale * k > kMaxShadowScale ? ", the product capped at 12" : "");
      }
    }
    return shadow_scale;
  }

  float ShadowMapRelativeScale()
  {
    const float latched = g_shadow_relative.load(std::memory_order_relaxed);
    if (latched > 1.0f || !g_scale)
    {
      return latched;
    }
    // Not latched yet (the first shadow surface comes after the first shadow setup): with the resolution scale
    // already latched at startup (graphics_backend null), the shadow surfaces' scale is known, so answer it now
    // (log 130: the far bias was x8 for the first frames, then x4 once the surface latched).
    const uint32_t k = uint32_t(std::clamp(REXCVAR_GET(pdz_shadow_map_scale), 1, 4));
    constexpr uint32_t kMaxShadowScale = 16384 / kShadowMapSize; // 12, as SurfaceScale caps it
    return float(std::min(g_scale * k, kMaxShadowScale)) / float(g_scale);
  }

  void ChooseScaleAtStartup()
  {
    const int32_t follow = REXCVAR_GET(pdz_native_scale_follow);
    const bool null_backend = pdz::gpusync::NullBackend();
    if (follow == 0 || (follow == 1 && !null_backend))
    {
      return; // pdz_native_scale alone, latched at the first host surface (the stage-1 behaviour)
    }
    const char *why = null_backend ? "graphics_backend null" : "pdz_native_scale_follow 2";
    if (rex::cvar::GetFlagSource("pdz_native_scale") != rex::cvar::Source::kDefault)
    {
      g_scale = uint32_t(std::clamp(REXCVAR_GET(pdz_native_scale), 1, 4));
      REXLOG_INFO("pdz_native: resolution scale {} latched at startup ({}): pdz_native_scale is set explicitly, so the "
                  "GPU plugin's resolution_scale ({}) is not followed",
                  g_scale, why, rex::cvar::GetFlagByName("resolution_scale"));
      LogScaleDetail(g_scale);
      return;
    }
    // The plugin's rule (TextureCache::GetConfigDrawResolutionScale): resolution_scale when it is not the default,
    // unless draw_resolution_scale_x / _y are set; each clamped to the plugin's 1..7.
    const int32_t shared = FlagInt("resolution_scale");
    if (shared < 0)
    {
      g_scale = uint32_t(std::clamp(REXCVAR_GET(pdz_native_scale), 1, 4));
      REXLOG_WARN("pdz_native: resolution scale {} latched at startup ({}): the GPU plugin's resolution_scale is not "
                  "registered (no GPU plugin loaded?), pdz_native_scale used",
                  g_scale, why);
      LogScaleDetail(g_scale);
      return;
    }
    const bool use_shared = rex::cvar::HasNonDefaultValue("resolution_scale");
    const auto axis = [&](const char *name) {
      const int32_t own = FlagInt(name);
      const int32_t v = use_shared && !rex::cvar::HasNonDefaultValue(name) ? shared : own;
      return std::clamp(v, 1, 7);
    };
    const int32_t x = axis("draw_resolution_scale_x"), y = axis("draw_resolution_scale_y");
    const int32_t chosen = std::clamp(std::min(x, y), 1, 4);
    g_scale = uint32_t(chosen);
    REXLOG_INFO("pdz_native: resolution scale {} latched at startup ({}): the GPU plugin's scale {}x{} "
                "(resolution_scale {}, draw_resolution_scale_x {}, _y {}); pdz_native_scale not set explicitly",
                g_scale, why, x, y, shared, rex::cvar::GetFlagByName("draw_resolution_scale_x"),
                rex::cvar::GetFlagByName("draw_resolution_scale_y"));
    if (x != y || std::max(x, y) > 4)
    {
      REXLOG_WARN("pdz_native: the GPU plugin's scale {}x{} is {}: the native renderer uses {} on both axes", x, y,
                  x != y ? "uneven" : "above the native renderer's 4", g_scale);
    }
    LogScaleDetail(g_scale);
  }
} // namespace pdz::native
