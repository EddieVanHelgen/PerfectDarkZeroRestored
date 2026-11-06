// Native renderer (stage 1): the memory log (native_memlog.h). One line every pdz_native_memlog_s seconds at the Swap,
// SUMMARY lines at test exit. Every value is a number under a unique key (key=value), so a log can be turned into a
// table with one regex; the keys are listed in kDefs below and described in stage1/subagents/memlog.md.
#include "native_memlog.h"

#include <windows.h>

#include <psapi.h> // PROCESS_MEMORY_COUNTERS_EX (K32GetProcessMemoryInfo is fetched at run time: no psapi.lib)

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/ui/d3d12/d3d12_provider.h>

#include "../shadow.h" // pdz_render_shadow_vs_ucode_erase
#include "native.h"
#include "native_autodiff.h"
#include "native_coissue.h"
#include "native_depth20e4.h"
#include "native_device.h"
#include "native_draw.h"
#include "native_drawdump.h"
#include "native_passdump.h"
#include "native_passes.h"
#include "native_pipeline.h"
#include "native_postfx.h"
#include "native_shaders.h"
#include "native_surfaces.h"
#include "native_textures.h"

REXCVAR_DECLARE(bool, pdz_native_upload_shrink);           // native_device.cpp
REXCVAR_DECLARE(int32_t, pdz_native_sampler_region_limit); // native_device.cpp
REXCVAR_DECLARE(bool, pdz_native_sampler_evict);           // native_draw.cpp
REXCVAR_DECLARE(bool, pdz_native_geometry_shrink);         // native_draw.cpp
REXCVAR_DECLARE(int32_t, pdz_native_pso_cap);              // native_pipeline.cpp
REXCVAR_DECLARE(int32_t, pdz_native_vs_binding_cap);       // native_shaders.cpp
REXCVAR_DECLARE(bool, pdz_native_texture_supersede);       // native_textures.cpp

REXCVAR_DEFINE_INT32(pdz_native_memlog_s, 0, "Render",
                     "Memory log for soak tests: every N seconds of wall-clock time (taken at the guest Swap) a "
                     "pdz_memlog line with the process private bytes and working set, the adapter's video memory in "
                     "use and budget (local / non-local) and what the native renderer holds (upload buffers, "
                     "descriptor heaps, deferred releases, texture cache, surfaces, geometry cache, PSOs, shader "
                     "cache); at test exit (--pdz_test_exit_s) SUMMARY lines with the first / peak / last value of "
                     "each and the values that still grew in the second half. Works with the native renderer off "
                     "(process and GPU totals). 0 = off.");

namespace pdz::native::memlog
{
  namespace
  {
    // How a value reads in the SUMMARY. kLevel: what is held now (the second-half growth list looks at these);
    // kGauge: a level that swings from frame to frame (first / peak / last only); kTotal: a running total or a
    // session maximum (grows by nature); kBudget: the OS figure the GPU levels are measured against.
    enum class Kind : uint8_t
    {
      kLevel,
      kGauge,
      kTotal,
      kBudget
    };

    enum Id : uint32_t
    {
      kPrivateMb, kWorkingSetMb,
      kGpuLocalMb, kGpuLocalBudgetMb, kGpuNonLocalMb, kGpuNonLocalBudgetMb,
      kUploadMb, kUploadUsedMb, kUploadFramePeakMb, kUploadGrows, kUploadShrinks,
      kSrvUsed, kSrvPeak, kSrvRefused, kSamplerUsed, kSamplerPeak, kSamplerRefused, kSamplerPersistent,
      kSamplerTables, kSamplerRegionRefused, kSamplerEvicted,
      kStagingLive, kStagingPeak, kStagingRefused, kRtvLive, kRtvPeak, kRtvRefused, kDsvLive, kDsvPeak, kDsvRefused,
      kReleasePending, kReleasePendingMb,
      kTexUploads, kTexUploadMb, kTexVolumes, kTexVolumeMb, kTexMirrors, kTexMirrorsStale, kTexMirrorMb, kTexViews,
      kTexViewMb, kTexSrvs, kTexCreated, kTexEvicted, kTexReplaced, kTexSuperseded, kTexMirrorsEvicted,
      kTexViewsEvicted, kTexLogSeen,
      kSurfColour, kSurfDepth, kSurfMb, kSurfRetired, kSurfEvicted, kSurfRetiredFreed,
      kGeoChunks, kGeoMb, kGeoUsedMb, kGeoEntries, kGeoRecycles, kGeoReleased,
      kPsos, kPsoKeys, kPsoFailed, kPsoEvicted, kPsoWarm, kPassPsos, kRootSignatures,
      kShaderLoaded, kShaderUsable, kShaderMb, kShaderSourceMb, kVsBindings, kBinderKb, kVsBindingsEvicted,
      kShaderMissed, kCoissuePatched, kDepthVariants, kVsUcode, kVsUcodeKb, kVsUcodeErased,
      kDiagMb,
      kCount
    };

    struct Def
    {
      Id id;
      const char *group; // non-null on the first value of a group: its name ("|" before it in the lines)
      uint8_t summary;   // which SUMMARY line (0..3) the group goes on
      const char *key;
      bool decimal;      // printed with one decimal (MB / KB), else as an integer
      Kind kind;
    };

    constexpr Def kDefs[] = {
        // Process: GetProcessMemoryInfo (private bytes = the commit charge; working set = resident).
        {kPrivateMb, "process", 0, "private_mb", true, Kind::kLevel},
        {kWorkingSetMb, nullptr, 0, "ws_mb", true, Kind::kLevel},
        // GPU: IDXGIAdapter3::QueryVideoMemoryInfo of the process (all its devices: the plugin's resources included).
        {kGpuLocalMb, "gpu", 0, "gpu_local_mb", true, Kind::kLevel},
        {kGpuLocalBudgetMb, nullptr, 0, "gpu_local_budget_mb", true, Kind::kBudget},
        {kGpuNonLocalMb, nullptr, 0, "gpu_nonlocal_mb", true, Kind::kLevel},
        {kGpuNonLocalBudgetMb, nullptr, 0, "gpu_nonlocal_budget_mb", true, Kind::kBudget},
        // Upload memory: the three frame slots' linear buffers (REVIEW2 M1; pdz_native_upload_shrink).
        {kUploadMb, "upload", 0, "upload_mb", true, Kind::kLevel},
        {kUploadUsedMb, nullptr, 0, "upload_used_mb", true, Kind::kGauge},
        {kUploadFramePeakMb, nullptr, 0, "upload_frame_peak_mb", true, Kind::kGauge},
        {kUploadGrows, nullptr, 0, "upload_grows", false, Kind::kTotal},
        {kUploadShrinks, nullptr, 0, "upload_shrinks", false, Kind::kTotal},
        // Descriptor heaps: shader-visible rings (the open frame's use, the most one frame used, refusals), the
        // persistent sampler tables (descriptors in tables, tables, tables the region had no room for and that came
        // from the ring instead, tables freed by pdz_native_sampler_evict), the free-list heaps (live, most live at
        // once, refusals; r2free).
        {kSrvUsed, "descriptors", 1, "srv_used", false, Kind::kGauge},
        {kSrvPeak, nullptr, 1, "srv_peak", false, Kind::kTotal},
        {kSrvRefused, nullptr, 1, "srv_refused", false, Kind::kTotal},
        {kSamplerUsed, nullptr, 1, "sampler_used", false, Kind::kGauge},
        {kSamplerPeak, nullptr, 1, "sampler_peak", false, Kind::kTotal},
        {kSamplerRefused, nullptr, 1, "sampler_refused", false, Kind::kTotal},
        {kSamplerPersistent, nullptr, 1, "sampler_persistent", false, Kind::kLevel},
        {kSamplerTables, nullptr, 1, "sampler_tables", false, Kind::kLevel},
        {kSamplerRegionRefused, nullptr, 1, "sampler_region_refused", false, Kind::kTotal},
        {kSamplerEvicted, nullptr, 1, "sampler_evicted", false, Kind::kTotal},
        {kStagingLive, nullptr, 1, "staging_live", false, Kind::kLevel},
        {kStagingPeak, nullptr, 1, "staging_peak", false, Kind::kTotal},
        {kStagingRefused, nullptr, 1, "staging_refused", false, Kind::kTotal},
        {kRtvLive, nullptr, 1, "rtv_live", false, Kind::kLevel},
        {kRtvPeak, nullptr, 1, "rtv_peak", false, Kind::kTotal},
        {kRtvRefused, nullptr, 1, "rtv_refused", false, Kind::kTotal},
        {kDsvLive, nullptr, 1, "dsv_live", false, Kind::kLevel},
        {kDsvPeak, nullptr, 1, "dsv_peak", false, Kind::kTotal},
        {kDsvRefused, nullptr, 1, "dsv_refused", false, Kind::kTotal},
        // Deferred releases (NativeDevice::ReleaseLater) waiting for their fence.
        {kReleasePending, "releases", 1, "release_pending", false, Kind::kGauge},
        {kReleasePendingMb, nullptr, 1, "release_pending_mb", true, Kind::kGauge},
        // Texture cache.
        {kTexUploads, "textures", 2, "tex_uploads", false, Kind::kLevel},
        {kTexUploadMb, nullptr, 2, "tex_upload_mb", true, Kind::kLevel},
        {kTexVolumes, nullptr, 2, "tex_volumes", false, Kind::kLevel},
        {kTexVolumeMb, nullptr, 2, "tex_volume_mb", true, Kind::kLevel},
        {kTexMirrors, nullptr, 2, "tex_mirrors", false, Kind::kLevel},
        {kTexMirrorsStale, nullptr, 2, "tex_mirrors_stale", false, Kind::kLevel},
        {kTexMirrorMb, nullptr, 2, "tex_mirror_mb", true, Kind::kLevel},
        {kTexViews, nullptr, 2, "tex_views", false, Kind::kLevel},
        {kTexViewMb, nullptr, 2, "tex_view_mb", true, Kind::kLevel},
        {kTexSrvs, nullptr, 2, "tex_srvs", false, Kind::kLevel},
        {kTexCreated, nullptr, 2, "tex_created", false, Kind::kTotal},
        {kTexEvicted, nullptr, 2, "tex_evicted", false, Kind::kTotal},
        {kTexReplaced, nullptr, 2, "tex_replaced", false, Kind::kTotal},
        {kTexSuperseded, nullptr, 2, "tex_superseded", false, Kind::kTotal},
        {kTexMirrorsEvicted, nullptr, 2, "tex_mirrors_evicted", false, Kind::kTotal},
        {kTexViewsEvicted, nullptr, 2, "tex_views_evicted", false, Kind::kTotal},
        {kTexLogSeen, nullptr, 2, "tex_log_seen", false, Kind::kLevel},
        // Surfaces (EDRAM render targets).
        {kSurfColour, "surfaces", 2, "surf_colour", false, Kind::kLevel},
        {kSurfDepth, nullptr, 2, "surf_depth", false, Kind::kLevel},
        {kSurfMb, nullptr, 2, "surf_mb", true, Kind::kLevel},
        {kSurfRetired, nullptr, 2, "surf_retired", false, Kind::kGauge},
        {kSurfEvicted, nullptr, 2, "surf_evicted", false, Kind::kTotal},
        {kSurfRetiredFreed, nullptr, 2, "surf_retired_freed", false, Kind::kTotal},
        // Geometry cache (upload-heap chunks of 32 MB; released = pdz_native_geometry_shrink).
        {kGeoChunks, "geometry", 3, "geo_chunks", false, Kind::kLevel},
        {kGeoMb, nullptr, 3, "geo_mb", true, Kind::kLevel},
        {kGeoUsedMb, nullptr, 3, "geo_used_mb", true, Kind::kGauge},
        {kGeoEntries, nullptr, 3, "geo_entries", false, Kind::kGauge},
        {kGeoRecycles, nullptr, 3, "geo_recycles", false, Kind::kTotal},
        {kGeoReleased, nullptr, 3, "geo_released", false, Kind::kTotal},
        // Pipelines: live PSOs, map keys (failures included), PSOs released by pdz_native_pso_cap.
        {kPsos, "pipelines", 3, "psos", false, Kind::kLevel},
        {kPsoKeys, nullptr, 3, "pso_keys", false, Kind::kLevel},
        {kPsoFailed, nullptr, 3, "pso_failed", false, Kind::kTotal},
        {kPsoEvicted, nullptr, 3, "pso_evicted", false, Kind::kTotal},
        // PSOs the warm-up holds (pdz_native_pso_cache / _prewarm): with prewarm all it made, shared with "psos".
        {kPsoWarm, nullptr, 3, "pso_warm", false, Kind::kLevel},
        {kPassPsos, nullptr, 3, "pass_psos", false, Kind::kLevel},
        {kRootSignatures, nullptr, 3, "root_signatures", false, Kind::kLevel},
        // Shader cache (loaded shaders never evicted; VS bindings dropped by pdz_native_vs_binding_cap) and the shadow
        // recorder's kept vertex microcode (dropped by pdz_render_shadow_vs_ucode_erase; read only while it is on).
        {kShaderLoaded, "shaders", 3, "shader_loaded", false, Kind::kLevel},
        {kShaderUsable, nullptr, 3, "shader_usable", false, Kind::kLevel},
        {kShaderMb, nullptr, 3, "shader_dxil_mb", true, Kind::kLevel},
        {kShaderSourceMb, nullptr, 3, "shader_source_mb", true, Kind::kLevel},
        {kVsBindings, nullptr, 3, "vs_bindings", false, Kind::kLevel},
        {kBinderKb, nullptr, 3, "binder_kb", true, Kind::kLevel},
        {kVsBindingsEvicted, nullptr, 3, "vs_bindings_evicted", false, Kind::kTotal},
        {kShaderMissed, nullptr, 3, "shader_missed", false, Kind::kLevel},
        {kCoissuePatched, nullptr, 3, "coissue_patched", false, Kind::kLevel},
        {kDepthVariants, nullptr, 3, "depth20e4_variants", false, Kind::kLevel},
        {kVsUcode, nullptr, 3, "vs_ucode", false, Kind::kLevel},
        {kVsUcodeKb, nullptr, 3, "vs_ucode_kb", true, Kind::kLevel},
        {kVsUcodeErased, nullptr, 3, "vs_ucode_erased", false, Kind::kTotal},
        // Diagnostic buffers kept between uses: auto-diff readback + image, pass / draw dump readbacks, de-rez snapshot.
        {kDiagMb, "diagnostics", 3, "diag_mb", true, Kind::kLevel},
    };
    static_assert(std::size(kDefs) == kCount, "one Def per Id");
    constexpr bool DefsInOrder()
    {
      for (uint32_t i = 0; i < kCount; ++i)
      {
        if (kDefs[i].id != i)
        {
          return false;
        }
      }
      return true;
    }
    static_assert(DefsInOrder(), "kDefs follows the Id order");
    constexpr const char *kSummaryNames[4] = {"process / gpu / upload", "descriptors / releases",
                                              "textures / surfaces", "geometry / pipelines / shaders / diagnostics"};

    constexpr double kMb = 1.0 / (1024.0 * 1024.0);
    constexpr size_t kMaxRows = 512; // samples kept for the mid value; past that every second one is dropped

    struct Row
    {
      uint64_t t_ms = 0; // since the log's first Swap
      uint64_t swap = 0; // Swap calls since then
      uint64_t frame = 0; // native frame index (0 without a device)
      double v[kCount] = {};
    };

    struct State
    {
      uint64_t start_ms = 0, next_ms = 0;
      uint64_t swaps = 0;
      uint64_t samples = 0;
      uint32_t stride = 1; // every stride-th sample is kept in rows
      Row first, last;
      double peak[kCount] = {};
      std::vector<Row> rows;
      bool header_logged = false;
      bool capacities_logged = false;
    };
    State g;

    // -----------------------------------------------------------------------------------------------------------
    // Process and adapter totals
    // -----------------------------------------------------------------------------------------------------------
    void QueryProcessMemory(double *v)
    {
      using Fn = BOOL(WINAPI *)(HANDLE, PROCESS_MEMORY_COUNTERS *, DWORD);
      static const Fn fn = [] {
        HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
        return kernel ? reinterpret_cast<Fn>(reinterpret_cast<void *>(GetProcAddress(kernel, "K32GetProcessMemoryInfo")))
                      : Fn(nullptr);
      }();
      PROCESS_MEMORY_COUNTERS_EX c{};
      c.cb = sizeof(c);
      if (fn && fn(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&c), sizeof(c)))
      {
        v[kPrivateMb] = double(c.PrivateUsage) * kMb;
        v[kWorkingSetMb] = double(c.WorkingSetSize) * kMb;
      }
    }

    struct Gpu
    {
      bool tried = false;
      IDXGIAdapter3 *adapter = nullptr; // kept for the session
      std::string name;
    };
    Gpu g_gpu;

    // The SDK's D3D12 provider without creating our device (NativeDevice::Init's checks: no RTTI in this build,
    // graphics_backend "vulkan" is the only non-D3D12 case).
    rex::ui::d3d12::D3D12Provider *Provider()
    {
      if (NativeDevice *dev = NativeDevice::Existing())
      {
        return dev->provider();
      }
      rex::Runtime *runtime = rex::Runtime::instance();
      rex::system::IGraphicsSystem *graphics = runtime ? runtime->graphics_system() : nullptr;
      rex::ui::GraphicsProvider *generic = graphics ? graphics->provider() : nullptr;
      if (!generic || rex::cvar::GetFlagByName("graphics_backend") == "vulkan" ||
          !rex::ui::d3d12::D3D12Provider::IsD3D12APIAvailable())
      {
        return nullptr;
      }
      return static_cast<rex::ui::d3d12::D3D12Provider *>(generic);
    }

    IDXGIAdapter3 *Adapter()
    {
      if (g_gpu.tried)
      {
        return g_gpu.adapter;
      }
      rex::ui::d3d12::D3D12Provider *provider = Provider();
      if (!provider || !provider->GetDevice() || !provider->GetDXGIFactory())
      {
        return nullptr; // not up yet (or not D3D12): asked again at the next sample
      }
      g_gpu.tried = true;
      g_gpu.name = provider->GetAdapterDescription();
      IDXGIFactory4 *factory = nullptr;
      if (SUCCEEDED(provider->GetDXGIFactory()->QueryInterface(IID_PPV_ARGS(&factory))) && factory)
      {
        const LUID luid = provider->GetDevice()->GetAdapterLuid();
        if (FAILED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&g_gpu.adapter))))
        {
          g_gpu.adapter = nullptr;
        }
        factory->Release();
      }
      if (!g_gpu.adapter)
      {
        REXLOG_WARN("pdz_memlog: no IDXGIAdapter3 for the device's adapter: the gpu_* values stay 0");
      }
      return g_gpu.adapter;
    }

    void QueryGpuMemory(double *v)
    {
      IDXGIAdapter3 *adapter = Adapter();
      if (!adapter)
      {
        return;
      }
      DXGI_QUERY_VIDEO_MEMORY_INFO info{};
      if (SUCCEEDED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)))
      {
        v[kGpuLocalMb] = double(info.CurrentUsage) * kMb;
        v[kGpuLocalBudgetMb] = double(info.Budget) * kMb;
      }
      info = {};
      if (SUCCEEDED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &info)))
      {
        v[kGpuNonLocalMb] = double(info.CurrentUsage) * kMb;
        v[kGpuNonLocalBudgetMb] = double(info.Budget) * kMb;
      }
    }

    // -----------------------------------------------------------------------------------------------------------
    // The native renderer (nothing without a device: no cache can hold anything then, and no singleton is touched)
    // -----------------------------------------------------------------------------------------------------------
    void QueryNative(NativeDevice &dev, double *v)
    {
      const NativeDevice::MemoryStats d = dev.GetMemoryStats();
      v[kUploadMb] = double(d.upload_bytes) * kMb;
      v[kUploadUsedMb] = double(d.upload_in_use) * kMb;
      v[kUploadFramePeakMb] = double(d.upload_frame_peak) * kMb;
      v[kUploadGrows] = double(d.upload_grows);
      v[kUploadShrinks] = double(d.upload_shrinks);
      v[kSrvUsed] = double(d.srv_in_use);
      v[kSrvPeak] = double(d.srv_peak);
      v[kSrvRefused] = double(d.srv_refused);
      v[kSamplerUsed] = double(d.sampler_in_use);
      v[kSamplerPeak] = double(d.sampler_peak);
      v[kSamplerRefused] = double(d.sampler_refused);
      v[kSamplerPersistent] = double(d.sampler_persistent_used);
      const SamplerCacheStats sc = GetSamplerCacheStats();
      v[kSamplerTables] = double(sc.tables);
      v[kSamplerRegionRefused] = double(sc.region_refused);
      v[kSamplerEvicted] = double(sc.evicted);
      v[kStagingLive] = double(d.heaps.staging_live);
      v[kStagingPeak] = double(d.heaps.staging_peak);
      v[kStagingRefused] = double(d.heaps.staging_failures);
      v[kRtvLive] = double(d.heaps.rtv_live);
      v[kRtvPeak] = double(d.heaps.rtv_peak);
      v[kRtvRefused] = double(d.heaps.rtv_failures);
      v[kDsvLive] = double(d.heaps.dsv_live);
      v[kDsvPeak] = double(d.heaps.dsv_peak);
      v[kDsvRefused] = double(d.heaps.dsv_failures);
      v[kReleasePending] = double(d.pending_releases);
      v[kReleasePendingMb] = double(d.pending_release_bytes) * kMb;
      if (!g.capacities_logged)
      {
        g.capacities_logged = true;
        REXLOG_INFO("pdz_memlog: native capacities: SRV ring {}, sampler ring {} + {} persistent, staging SRV {}, RTV {}, "
                    "DSV {}; upload buffers {:.0f} MB (3 frame slots); upload shrink {}; sampler evict {} (region "
                    "limit {}); PSO cap {}; VS binding cap {}; geometry shrink {}; texture supersede {}; VS ucode "
                    "erase {}",
                    d.srv_ring_size, d.sampler_ring_size, d.sampler_persistent_size, d.heaps.staging_size,
                    d.heaps.rtv_size, d.heaps.dsv_size, double(d.upload_bytes) * kMb,
                    REXCVAR_GET(pdz_native_upload_shrink) ? "on" : "off",
                    REXCVAR_GET(pdz_native_sampler_evict) ? "on" : "off", REXCVAR_GET(pdz_native_sampler_region_limit),
                    REXCVAR_GET(pdz_native_pso_cap), REXCVAR_GET(pdz_native_vs_binding_cap),
                    REXCVAR_GET(pdz_native_geometry_shrink) ? "on" : "off",
                    REXCVAR_GET(pdz_native_texture_supersede) ? "on" : "off",
                    REXCVAR_GET(pdz_render_shadow_vs_ucode_erase) ? "on" : "off");
      }

      const Textures::MemoryStats t = Textures::Get().GetMemoryStats();
      v[kTexUploads] = double(t.uploads);
      v[kTexUploadMb] = double(t.upload_bytes) * kMb;
      v[kTexVolumes] = double(t.volumes);
      v[kTexVolumeMb] = double(t.volume_bytes) * kMb;
      v[kTexMirrors] = double(t.mirrors);
      v[kTexMirrorsStale] = double(t.mirrors_stale);
      v[kTexMirrorMb] = double(t.mirror_bytes) * kMb;
      v[kTexViews] = double(t.views);
      v[kTexViewMb] = double(t.view_bytes) * kMb;
      v[kTexSrvs] = double(t.srvs);
      v[kTexCreated] = double(t.created);
      v[kTexEvicted] = double(t.evicted);
      v[kTexReplaced] = double(t.replaced);
      v[kTexSuperseded] = double(t.superseded);
      v[kTexMirrorsEvicted] = double(t.mirrors_evicted);
      v[kTexViewsEvicted] = double(t.views_evicted);
      v[kTexLogSeen] = double(t.log_seen);

      const Surfaces::MemoryStats s = Surfaces::Get().GetMemoryStats();
      v[kSurfColour] = double(s.colour);
      v[kSurfDepth] = double(s.depth);
      v[kSurfMb] = double(s.bytes) * kMb;
      v[kSurfRetired] = double(s.retired);
      v[kSurfEvicted] = double(s.evicted);
      v[kSurfRetiredFreed] = double(s.retired_freed);

      const GeometryCacheStats geo = GetGeometryCacheStats();
      v[kGeoChunks] = double(geo.chunks);
      v[kGeoMb] = double(geo.chunk_bytes) * kMb;
      v[kGeoUsedMb] = double(geo.used_bytes) * kMb;
      v[kGeoEntries] = double(geo.entries);
      v[kGeoRecycles] = double(geo.recycles);
      v[kGeoReleased] = double(geo.released);

      const PipelineCache &p = PipelineCache::Get();
      v[kPsos] = double(p.pso_live()); // created - released (the same as created with pdz_native_pso_cap 0)
      v[kPsoKeys] = double(p.pso_keys());
      v[kPsoFailed] = double(p.pso_failures());
      v[kPsoEvicted] = double(p.pso_evicted());
      v[kPsoWarm] = double(p.pso_warm_held());
      v[kPassPsos] = double(passes::PipelineCount() + postfx::PipelineCount()); // + the swap post-effect passes
      v[kRootSignatures] =
          double(p.root_signature_count() + passes::RootSignatureCount() + postfx::RootSignatureCount());

      const ShaderCache::MemoryStats sh = ShaderCache::Get().GetMemoryStats();
      v[kShaderLoaded] = double(sh.loaded);
      v[kShaderUsable] = double(sh.usable);
      v[kShaderMb] = double(sh.dxil_bytes) * kMb;
      v[kShaderSourceMb] = double(sh.source_bytes) * kMb;
      v[kVsBindings] = double(sh.bindings);
      v[kBinderKb] = double(sh.binder_bytes) / 1024.0;
      v[kVsBindingsEvicted] = double(sh.bindings_evicted);
      v[kShaderMissed] = double(sh.missed);
      v[kCoissuePatched] = double(coissue::GetStats().patched);
      v[kDepthVariants] = double(depth20e4::GetStats().built);

      v[kDiagMb] = double(autodiff::KeptBytes() + passdump::ReadbackBytes() + drawdump::ReadbackBytes() +
                          s.readback_bytes) *
                   kMb;
    }

    // -----------------------------------------------------------------------------------------------------------
    // Samples and lines
    // -----------------------------------------------------------------------------------------------------------
    std::string Value(uint32_t i, double v)
    {
      return kDefs[i].decimal ? fmt::format("{:.1f}", v) : fmt::format("{}", uint64_t(std::llround(std::max(v, 0.0))));
    }

    void Sample(uint64_t now_ms)
    {
      State &s = g;
      Row r;
      r.t_ms = now_ms - s.start_ms;
      r.swap = s.swaps;
      QueryProcessMemory(r.v);
      QueryGpuMemory(r.v);
      if (NativeDevice *dev = NativeDevice::Existing())
      {
        r.frame = dev->frame_index();
        QueryNative(*dev, r.v);
      }
      if (REXCVAR_GET(pdz_render_shadow_vs_ucode_erase))
      {
        // The shadow recorder's kept vertex microcode (renderer on or off). Read under the render lock (OnSwap and
        // LogSummary hold it), which the Create hooks also take while this cvar is on; while it is off they write
        // unlocked, so the values stay 0 then.
        const shadow::VertexUcodeStats vu = shadow::GetVertexUcodeStats();
        r.v[kVsUcode] = double(vu.entries);
        r.v[kVsUcodeKb] = double(vu.bytes) / 1024.0;
        r.v[kVsUcodeErased] = double(vu.erased);
      }
      for (uint32_t i = 0; i < kCount; ++i)
      {
        s.peak[i] = s.samples ? std::max(s.peak[i], r.v[i]) : r.v[i];
      }
      if (!s.samples)
      {
        s.first = r;
      }
      s.last = r;
      if (s.samples % s.stride == 0)
      {
        s.rows.push_back(r);
        if (s.rows.size() >= kMaxRows)
        {
          // Keep samples 0, 2, 4, ...: the rows stay evenly spread over the run, memory stays bounded.
          size_t kept = 0;
          for (size_t i = 0; i < s.rows.size(); i += 2)
          {
            s.rows[kept++] = s.rows[i];
          }
          s.rows.resize(kept);
          s.stride *= 2;
        }
      }
      ++s.samples;
    }

    void LogRow(const Row &r, const char *tag)
    {
      State &s = g;
      if (!s.header_logged)
      {
        s.header_logged = true;
        REXLOG_INFO("pdz_memlog: a sample every {} s at the Swap (native renderer {}); adapter '{}'; values are MB (_mb), "
                    "KB (_kb) or counts; SUMMARY at test exit",
                    REXCVAR_GET(pdz_native_memlog_s), Enabled() ? "on" : "off",
                    g_gpu.name.empty() ? std::string("unknown") : g_gpu.name);
      }
      std::string line = fmt::format("pdz_memlog: t {} s swap {} frame {}{}", r.t_ms / 1000, r.swap, r.frame, tag);
      for (uint32_t i = 0; i < kCount; ++i)
      {
        line += kDefs[i].group ? " | " : " ";
        line += kDefs[i].key;
        line += '=';
        line += Value(i, r.v[i]);
      }
      REXLOG_INFO("{}", line);
    }

    // The kept sample nearest to the middle of the run.
    const Row &MidRow()
    {
      State &s = g;
      const uint64_t mid = (s.first.t_ms + s.last.t_ms) / 2;
      const Row *best = &s.first;
      for (const Row &r : s.rows)
      {
        const uint64_t d = r.t_ms > mid ? r.t_ms - mid : mid - r.t_ms;
        const uint64_t b = best->t_ms > mid ? best->t_ms - mid : mid - best->t_ms;
        if (d < b)
        {
          best = &r;
        }
      }
      return *best;
    }
  } // namespace

  void OnSwap()
  {
    const int32_t every = REXCVAR_GET(pdz_native_memlog_s);
    if (every <= 0)
    {
      return;
    }
    State &s = g;
    const uint64_t now = GetTickCount64();
    const uint64_t interval = uint64_t(every) * 1000;
    if (!s.start_ms)
    {
      s.start_ms = now;
      s.next_ms = now + interval;
      return;
    }
    ++s.swaps;
    if (now < s.next_ms)
    {
      return;
    }
    s.next_ms += interval;
    if (s.next_ms <= now)
    {
      s.next_ms = now + interval; // after a long stall (a load): one sample, not a burst
    }
    Sample(now);
    LogRow(s.last, "");
  }

  void LogSummary(const char *why)
  {
    if (REXCVAR_GET(pdz_native_memlog_s) <= 0)
    {
      return;
    }
    RenderLock lock(RenderMutex());
    State &s = g;
    if (!s.start_ms)
    {
      return; // no Swap yet
    }
    Sample(GetTickCount64()); // the state at exit
    LogRow(s.last, " (final sample)");
    const Row &mid = MidRow();
    REXLOG_INFO("pdz_memlog: SUMMARY ({}): {} samples over {} s (swaps {}..{}, native frames {}..{}); each value as "
                "first/peak/last; mid-run sample at t {} s",
                why, s.samples, s.last.t_ms / 1000, s.first.swap, s.last.swap, s.first.frame, s.last.frame,
                mid.t_ms / 1000);
    for (uint32_t line = 0; line < 4; ++line)
    {
      std::string text;
      for (uint32_t i = 0; i < kCount; ++i)
      {
        if (kDefs[i].summary != line)
        {
          continue;
        }
        text += kDefs[i].group && !text.empty() ? " | " : " ";
        text += fmt::format("{}={}/{}/{}", kDefs[i].key, Value(i, s.first.v[i]), Value(i, s.peak[i]),
                            Value(i, s.last.v[i]));
      }
      REXLOG_INFO("pdz_memlog: SUMMARY {}:{}", kSummaryNames[line], text);
    }

    // Levels that grew from the mid-run sample to the last one: a cache that levelled off after the loads moved
    // in the first half only; a leak keeps going. Largest relative growth first, with the first-half growth and the
    // rate for comparison.
    struct Growth
    {
      uint32_t i;
      double pct;
    };
    std::vector<Growth> grew;
    for (uint32_t i = 0; i < kCount; ++i)
    {
      const double delta = s.last.v[i] - mid.v[i];
      const double epsilon = kDefs[i].decimal ? 0.05 : 0.5; // below the printed precision
      if (kDefs[i].kind != Kind::kLevel || delta <= epsilon)
      {
        continue;
      }
      grew.push_back({i, mid.v[i] > 0.0 ? 100.0 * delta / mid.v[i] : 100.0});
    }
    std::sort(grew.begin(), grew.end(), [](const Growth &a, const Growth &b) { return a.pct > b.pct; });
    std::string text;
    const double hours = double(s.last.t_ms - mid.t_ms) / 3600000.0;
    for (const Growth &gr : grew)
    {
      const uint32_t i = gr.i;
      const double delta = s.last.v[i] - mid.v[i];
      text += fmt::format("{}{} {}->{} (+{}, +{:.1f}%, {:.1f}/h; first half {}{})", text.empty() ? " " : "; ",
                          kDefs[i].key, Value(i, mid.v[i]), Value(i, s.last.v[i]), Value(i, delta), gr.pct,
                          hours > 0.0 ? delta / hours : 0.0, mid.v[i] >= s.first.v[i] ? "+" : "-",
                          Value(i, std::fabs(mid.v[i] - s.first.v[i])));
    }
    REXLOG_INFO("pdz_memlog: SUMMARY still growing in the second half (levels, t {} s -> {} s):{}", mid.t_ms / 1000,
                s.last.t_ms / 1000, text.empty() ? " none" : text);

    // Upload-cache bookkeeping: every entry created is evicted, replaced or still cached.
    if (NativeDevice::Existing())
    {
      const double created = s.last.v[kTexCreated], evicted = s.last.v[kTexEvicted], replaced = s.last.v[kTexReplaced];
      const double live = s.last.v[kTexUploads];
      REXLOG_INFO("pdz_memlog: SUMMARY texture cache: created {} - evicted {} - replaced {} = {}, cached {} ({})",
                  Value(kTexCreated, created), Value(kTexEvicted, evicted), Value(kTexReplaced, replaced),
                  Value(kTexUploads, created - evicted - replaced), Value(kTexUploads, live),
                  std::llround(created - evicted - replaced) == std::llround(live)
                      ? "consistent"
                      : "MISMATCH: an entry left the cache uncounted, or one is counted twice");
    }
  }
} // namespace pdz::native::memlog
