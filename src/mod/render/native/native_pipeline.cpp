// Native renderer (stage 1): root signature, input layouts, register decoding, PSO cache (native_pipeline.h).
// The register bit layouts come from the SDK's registers.h / xenos.h unions (read-only use); the mapping to
// D3D12 state mirrors the SDK's own D3D12 pipeline cache (src/graphics/d3d12/pipeline_cache.cpp) where the
// two agree, see PIPELINE_NOTES.md for the table.
#include "native_dxc.h" // NativeCacheDir
#include "native_pipeline.h"
#include "../cpu_placement.h"
#include "native_clip_gs_dxil.h"
#include "native_point_gs_dxil.h"
#include "native_rectlist_gs_dxil.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dxgi1_4.h> // pdz_native_pso_cache: the adapter and driver the pipeline library belongs to
#include <psapi.h>    // PROCESS_MEMORY_COUNTERS_EX (K32GetProcessMemoryInfo fetched at run time: no psapi.lib)

// CreatePipelineLibrary's results for a library from another driver / GPU (winerror.h of newer SDKs).
#ifndef D3D12_ERROR_ADAPTER_NOT_FOUND
#define D3D12_ERROR_ADAPTER_NOT_FOUND _HRESULT_TYPEDEF_(0x887E0001L)
#endif
#ifndef D3D12_ERROR_DRIVER_VERSION_MISMATCH
#define D3D12_ERROR_DRIVER_VERSION_MISMATCH _HRESULT_TYPEDEF_(0x887E0002L)
#endif

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>
#include <rex/hash.h>
#include <rex/logging.h>

#include "native.h"
#include "native_blend7e3.h"
#include "native_depth20e4.h"
#include "native_device.h"
#include "../frame_timing.h" // pdz_frame_timing_phases: pso.create

REXCVAR_DEFINE_BOOL(pdz_native_debug_ztest_off, false, "Render",
                    "Native renderer diagnostic: disable the depth test of draws that do not write depth.");
REXCVAR_DEFINE_BOOL(pdz_native_rectlist, true, "Render",
                    "Native renderer: draw D3DPT_RECTLIST (Xenos kRectangleList) through the rectangle-list geometry "
                    "shader (the SDK's expansion: fourth vertex mirrored across the longest edge). Off = such draws are "
                    "skipped and counted as unsupported primitives (PDZ issued none in 10.6M draws).");
REXCVAR_DEFINE_BOOL(pdz_native_clip_planes, true, "Render",
                    "Native renderer: honour Xenos user clip planes (PA_CL_CLIP_CNTL ucp_ena, D3DDevice_SetClipPlane) "
                    "as the SDK does, through a pass-through geometry shader that writes SV_ClipDistance "
                    "(native_clip_gs_dxil.h; triangle draws only). Off = the planes are ignored (geometry the "
                    "emulator clips, e.g. in the Nightclub planar reflection, is drawn).");
REXCVAR_DEFINE_BOOL(pdz_native_mask_unwritten_targets, true, "Render",
                    "Native renderer: a colour target the pixel shader does not write (sidecar color_outputs) gets a "
                    "zero write mask, as the SDK's draw_util::GetNormalizedColorMask (the guest writes nothing there). "
                    "Off: RB_COLOR_MASK as is, and D3D12 writes undefined values (the hair and foliage forward draws "
                    "of PS 89e06287 / 7c273fb3 export oC0 only with mask 0077: garbage in the velocity target RT1).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(pdz_native_alpha_to_mask, true, "Render",
                    "Native renderer: RB_COLORCONTROL alpha-to-mask (D3DRS_ALPHATOMASKENABLE) as the SDK computes it "
                    "with one sample, folded into the PS alpha test: oC0.w >= 1 - offset / 4 (the mean of the four "
                    "quad offsets when they differ). Off = alpha-to-mask ignored (cut-outs drawn as solid quads).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(pdz_native_pso_cap, 1536, "Render",
                     "Native renderer: most PSOs kept alive. The cache key holds the shaders, the state and the target "
                     "formats, so it is bounded by the game's content, but a session keeps every level's PSOs: one "
                     "mission makes 278-462 (regression 15:04), the 14 missions use about 3 times the bound vertex "
                     "shaders of one (413 against 103-173). Checked every 600 frames: over the cap, the least recently "
                     "used PSOs not drawn with for 1800 frames are released (through the frame fence) until three "
                     "quarters of the cap are left; one needed again is created again (a driver compile, usually from "
                     "its shader cache). Never reached within one level. 0 = never released (the old behaviour).");
REXCVAR_DEFINE_BOOL(pdz_native_pso_cache, true, "Render",
                    "Native renderer: keep the PSOs across sessions. Every PSO a draw makes goes into a D3D12 pipeline "
                    "library and its recipe (the cache key, the shaders' DXIL, the input layout) into a list; both are "
                    "saved to <exe dir>\\native_cache (pso.lib, pso_recipes.bin) on a background thread 5 s after the "
                    "last new PSO (written to a .tmp file and renamed). A PSO made again (a later session, or one "
                    "released by pdz_native_pso_cap) is then loaded from the library instead of compiled by the driver. "
                    "The library belongs to the GPU and driver: another one (or a damaged file) starts it empty, the "
                    "recipes stay. Recipes no draw used for 16 sessions are dropped. The first native draw of a session "
                    "starts it. Off = no files read or written, PSOs made at first use as before")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(pdz_native_pso_warmup, true, "Render",
                    "Native renderer, with pdz_native_pso_cache: at the session's first native draw, worker threads make "
                    "the PSOs of the saved recipes (most recently used first, at most pdz_native_pso_warmup_max) so the "
                    "draws find them ready instead of creating them at first use (the stutter of a new PSO in play: "
                    "462-970 a session in logs 001-005, every one at its first draw). A draw needing a PSO a worker is "
                    "making waits for that one; one no worker has started yet is made by the draw (counted). Logged as "
                    "'PSO warm-up' with its progress. Off = no warm-up (the library still serves PSOs at first use)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(pdz_native_pso_warmup_threads, 0, "Render",
                     "Native renderer: worker threads of the PSO warm-up (pdz_native_pso_warmup). 0 = a quarter of the "
                     "CPU's threads, 1 to 4")
    .range(0, 8)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(pdz_native_pso_warmup_max, 0, "Render",
                     "Native renderer: most PSOs the warm-up makes (pdz_native_pso_warmup), the most recently used "
                     "recipes first; the rest are loaded from the pipeline library at first use. 0 = three quarters of "
                     "pdz_native_pso_cap (every recipe with the cap at 0)")
    .range(0, 16384)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(pdz_native_pso_prewarm, true, "Render",
                    "Native renderer, with pdz_native_pso_cache: every pipeline ever made is kept in the recipe list "
                    "(merged across runs, never aged out) and the whole list is built at startup on the warm-up's "
                    "worker threads (pdz_native_pso_warmup implied, no pdz_native_pso_warmup_max limit) while the "
                    "intros and the front end run, so no level waits for its pipelines at first sight. The warm-up "
                    "keeps its PSOs for the session (pdz_native_pso_cap no longer frees them; a draw shares the one "
                    "made for its description); the draws never wait for the warm-up as a whole, only for the one "
                    "PSO a worker is making when a draw needs exactly that one (counted). A PSO not in the list yet "
                    "is made on demand as before and joins the list. A GPU / driver change rebuilds the list's PSOs "
                    "once (driver compiles on the workers). Logged: 'PSO warm-up done ... after process start' and "
                    "'PSO summary' (misses during play). Off = the per-session warm-up as before (recipes age out "
                    "after 16 sessions, at most pdz_native_pso_warmup_max)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(pdz_native_pso_level_wait_ms, 20000, "Render",
                     "Native renderer, with pdz_native_pso_prewarm: when a level starts while the startup warm-up is "
                     "still making pipelines (a first start, or a new GPU / driver), the level start waits for it, at "
                     "most this many ms, so the level's pipelines are ready before its first frame instead of being "
                     "compiled mid-play. Logged with the time waited. 0 = never wait")
    .range(0, 120000);
REXCVAR_DEFINE_BOOL(pdz_native_pso_warmup_wait, false, "Render",
                    "Native renderer, with pdz_native_pso_prewarm: the first native draw waits for the whole startup "
                    "warm-up (the game stays on its first frame meanwhile). Off = the warm-up runs on its worker "
                    "threads while the intros and the front end play (pdz_native_pso_level_wait_ms covers a level "
                    "started before it is done)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(pdz_native_first_use_log, true, "Render",
                    "Native renderer: log what is still made at a draw's first use, with its cost in ms: PSOs not ready "
                    "from the warm-up (driver compile, pipeline library load, or a wait for the warm-up), VS bindings "
                    "and pixel shaders resolved (file load, co-issue / variant patch, compile) and shader variants "
                    "compiled because the disk caches lacked them. The first 32 of each kind one line each, then counts "
                    "and total ms in the periodic draws / shader cache lines. Off = none of these lines (stutter "
                    "hunting, pdz_native_pso_cache)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace pdz::native
{
  namespace reg = rex::graphics::reg;
  namespace xenos = rex::graphics::xenos;
  using rex::graphics::Register;

  // ---------------------------------------------------------------------------
  // Register shadow access
  // ---------------------------------------------------------------------------
  namespace
  {
    struct Run
    {
      uint32_t first;  // first register
      uint32_t count;  // registers in the run
      uint32_t offset; // device offset of the first
    };
    // The seven runs the D3D flush emits (shadow_record.h dev::kRun*), device offsets.
    constexpr Run kRuns[] = {
        {0x2000, 16, shadow::dev::kRun2000}, {0x2100, 21, shadow::dev::kRun2100}, {0x2180, 5, shadow::dev::kRun2180},
        {0x2200, 12, shadow::dev::kRun2200}, {0x2280, 21, shadow::dev::kRun2280}, {0x2300, 38, shadow::dev::kRun2300},
        {0x2380, 8, shadow::dev::kRun2380},
    };

    bool RegIndex(uint32_t reg, uint32_t &index)
    {
      for (const Run &run : kRuns)
      {
        if (reg >= run.first && reg < run.first + run.count)
        {
          index = (run.offset - shadow::dev::kRegShadowBegin) / 4 + (reg - run.first);
          return index < shadow::dev::kRegShadowDwords;
        }
      }
      if (reg == rex::graphics::XE_GPU_REG_PA_SC_WINDOW_OFFSET)
      {
        index = (shadow::dev::kWindowOffset - shadow::dev::kRegShadowBegin) / 4;
        return true;
      }
      if (reg == rex::graphics::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL)
      {
        index = (shadow::dev::kWindowScissorTL - shadow::dev::kRegShadowBegin) / 4;
        return true;
      }
      if (reg == rex::graphics::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR)
      {
        index = (shadow::dev::kWindowScissorBR - shadow::dev::kRegShadowBegin) / 4;
        return true;
      }
      return false;
    }
  } // namespace

  uint32_t ReadReg(const shadow::DrawRecord &r, uint32_t reg)
  {
    uint32_t index = 0;
    if (!RegIndex(reg, index))
    {
      return 0;
    }
    return shadow::Swap32(r.regs_be[index]);
  }

  float ReadRegFloat(const shadow::DrawRecord &r, uint32_t reg) { return shadow::BitsToFloat(ReadReg(r, reg)); }

  void DecodeRenderState(const shadow::DrawRecord &r, RenderState &out)
  {
    using namespace rex::graphics;
    out.depthcontrol = ReadReg(r, XE_GPU_REG_RB_DEPTHCONTROL);
    out.blendcontrol[0] = ReadReg(r, XE_GPU_REG_RB_BLENDCONTROL0);
    out.blendcontrol[1] = ReadReg(r, XE_GPU_REG_RB_BLENDCONTROL1);
    out.blendcontrol[2] = ReadReg(r, XE_GPU_REG_RB_BLENDCONTROL2);
    out.blendcontrol[3] = ReadReg(r, XE_GPU_REG_RB_BLENDCONTROL3);
    out.color_mask = ReadReg(r, XE_GPU_REG_RB_COLOR_MASK);
    out.colorcontrol = ReadReg(r, XE_GPU_REG_RB_COLORCONTROL);
    out.su_sc_mode_cntl = ReadReg(r, XE_GPU_REG_PA_SU_SC_MODE_CNTL);
    out.stencilrefmask = ReadReg(r, XE_GPU_REG_RB_STENCILREFMASK);
    out.stencilrefmask_bf = ReadReg(r, XE_GPU_REG_RB_STENCILREFMASK_BF);
    out.point_size = ReadReg(r, XE_GPU_REG_PA_SU_POINT_SIZE);
    out.vtx_cntl = ReadReg(r, XE_GPU_REG_PA_SU_VTX_CNTL);
    out.clip_cntl = ReadReg(r, XE_GPU_REG_PA_CL_CLIP_CNTL);
    out.depth_info = ReadReg(r, XE_GPU_REG_RB_DEPTH_INFO);
    out.color_info = ReadReg(r, XE_GPU_REG_RB_COLOR_INFO);
    out.alpha_ref = ReadRegFloat(r, XE_GPU_REG_RB_ALPHA_REF);
    out.blend_factor[0] = ReadRegFloat(r, XE_GPU_REG_RB_BLEND_RED);
    out.blend_factor[1] = ReadRegFloat(r, XE_GPU_REG_RB_BLEND_GREEN);
    out.blend_factor[2] = ReadRegFloat(r, XE_GPU_REG_RB_BLEND_BLUE);
    out.blend_factor[3] = ReadRegFloat(r, XE_GPU_REG_RB_BLEND_ALPHA);
    out.poly_offset_front_scale = ReadRegFloat(r, XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_SCALE);
    out.poly_offset_front_offset = ReadRegFloat(r, XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_OFFSET);
    out.poly_offset_back_scale = ReadRegFloat(r, XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_SCALE);
    out.poly_offset_back_offset = ReadRegFloat(r, XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_OFFSET);
  }

  uint32_t UserClipPlaneMask(const RenderState &rs)
  {
    if (!REXCVAR_GET(pdz_native_clip_planes))
    {
      return 0;
    }
    reg::PA_CL_CLIP_CNTL cc;
    cc.value = rs.clip_cntl;
    // The SDK's pipeline_cache: no planes with clip_disable; ucp_cull_only_ena makes them cull distances (whole
    // primitives only), not reproduced here (left unclipped, as before).
    if (cc.clip_disable || cc.ucp_cull_only_ena)
    {
      return 0;
    }
    return cc.ucp_ena;
  }

  bool UserClipPlanesToHost(const shadow::DrawRecord &r, uint32_t mask, const HostViewport &hv, float out[6][4])
  {
    std::memset(out, 0, sizeof(float) * 6 * 4);
    for (uint32_t i = 0; i < 3; ++i)
    {
      if (!(hv.ndc_scale[i] != 0.0f) || !std::isfinite(hv.ndc_scale[i]) || !std::isfinite(hv.ndc_offset[i]))
      {
        return false;
      }
    }
    // Packed in ascending plane order like the SDK's user_clip_planes. guest.xyz = (host.xyz - o * w) / s,
    // guest.w = host.w, so dot(guest, P) = sum(P.i / s.i * host.i) + host.w * (P.w - sum(P.i * o.i / s.i)).
    uint32_t k = 0;
    for (uint32_t i = 0; i < shadow::dev::kClipPlaneCount; ++i)
    {
      if (!(mask & (1u << i)))
      {
        continue;
      }
      float p[4];
      for (uint32_t c = 0; c < 4; ++c)
      {
        uint32_t be = r.clip_planes_be[4 * i + c];
        uint32_t v = (be >> 24) | ((be >> 8) & 0xFF00u) | ((be << 8) & 0xFF0000u) | (be << 24);
        std::memcpy(&p[c], &v, sizeof(float));
      }
      float w = p[3];
      for (uint32_t c = 0; c < 3; ++c)
      {
        out[k][c] = p[c] / hv.ndc_scale[c];
        w -= p[c] * hv.ndc_offset[c] / hv.ndc_scale[c];
      }
      out[k][3] = w;
      ++k;
    }
    return true;
  }

  AlphaTest AlphaTestFor(const RenderState &rs)
  {
    reg::RB_COLORCONTROL cc;
    cc.value = rs.colorcontrol;
    AlphaTest a;
    a.func = cc.alpha_test_enable ? uint32_t(cc.alpha_func) + 1u : 0u; // D3DCMPFUNC = xenos + 1
    a.ref = rs.alpha_ref;
    if (!cc.alpha_to_mask_enable)
    {
      return a;
    }
    a.alpha_to_mask = true;
    // SDK DxbcShaderTranslator::CompletePixelShader_AlphaToMask (dxbc_translator_om.cpp), its "MSAA is
    // disabled" branch (the SDK takes the sample count from the guest's RB_SURFACE_INFO; PDZ never uses MSAA):
    // the one sample is covered when oC0.w >= 1 - offset / 4 (NaN fails), offset = alpha_to_mask_offset0..3
    // (RB_COLORCONTROL bits 31:24) of the pixel's place in its 2x2 quad ((x & 1) << 1 | (y & 1)), after the
    // alpha test. alphaTest() has one reference per draw, so one threshold: exact when the four offsets are
    // equal (D3D's undithered 2, 2, 2, 2: 0.5), else their mean (the dithered 3, 1, 0, 2: 0.625), which keeps
    // the covered fraction of evenly spread alpha but not the 2x2 dither pattern.
    const uint32_t o0 = cc.alpha_to_mask_offset0, o1 = cc.alpha_to_mask_offset1;
    const uint32_t o2 = cc.alpha_to_mask_offset2, o3 = cc.alpha_to_mask_offset3;
    a.dithered = o0 != o1 || o0 != o2 || o0 != o3;
    const float threshold = 1.0f - float(o0 + o1 + o2 + o3) * (1.0f / 16.0f);
    // Both must pass: the alpha test (func, ref) and alpha >= threshold, as one comparison.
    constexpr uint32_t kNever = 1, kLess = 2, kEqual = 3, kLessEqual = 4, kGreater = 5, kNotEqual = 6,
                       kGreaterEqual = 7;
    uint32_t func = a.func;
    float ref = a.ref;
    switch (func)
    {
    case kNever:
      break;
    case kEqual: // alpha == ref, passes only when ref >= threshold
      if (!(ref >= threshold))
      {
        func = kNever;
      }
      break;
    case kGreater: // alpha > ref implies alpha >= threshold when ref >= threshold
      if (!(ref >= threshold))
      {
        func = kGreaterEqual;
        ref = threshold;
      }
      break;
    case kGreaterEqual:
      ref = std::max(ref, threshold);
      break;
    case kLess:      // threshold <= alpha < ref: a band; never when empty, else the alpha test alone
    case kLessEqual: // threshold <= alpha <= ref
      if (func == kLess ? !(ref > threshold) : !(ref >= threshold))
      {
        func = kNever;
      }
      else
      {
        a.inexact = true;
      }
      break;
    case kNotEqual: // alpha >= threshold, and != ref: that one value is lost when ref >= threshold
      a.inexact = ref >= threshold;
      func = kGreaterEqual;
      ref = threshold;
      break;
    default: // 0 = alpha test off, 8 = always
      func = kGreaterEqual;
      ref = threshold;
      break;
    }
    if (REXCVAR_GET(pdz_native_alpha_to_mask))
    {
      a.func = func;
      a.ref = ref;
    }
    return a;
  }

  // ---------------------------------------------------------------------------
  // Viewport (the ReXGlue SDK's draw_util::GetHostViewportInfo / GetScissor, reimplemented: that code lives in
  // the GPU plugin DLL, which exports nothing else). Fixed arguments as the SDK's D3D12 command processor passes
  // them: origin bottom-left (D3D12 NDC +Y is up), x_max = y_max = D3D12_VIEWPORT_BOUNDS_MAX, no reverse Z, no
  // window offset in the EDRAM base (predicated tiling is never active in PDZ), half_pixel_offset on (the SDK
  // default), no MSAA sample-position adjustment and no float24 depth-range remap (native depth is D32).
  // ---------------------------------------------------------------------------
  void ComputeHostViewport(const shadow::DrawRecord &r, uint32_t scale, bool ps_writes_depth, HostViewport &out)
  {
    using namespace rex::graphics;
    scale = std::max(scale, 1u);
    reg::PA_CL_CLIP_CNTL clip;
    clip.value = ReadReg(r, XE_GPU_REG_PA_CL_CLIP_CNTL);
    reg::PA_CL_VTE_CNTL vte;
    vte.value = ReadReg(r, XE_GPU_REG_PA_CL_VTE_CNTL);
    reg::PA_SU_SC_MODE_CNTL sc;
    sc.value = ReadReg(r, XE_GPU_REG_PA_SU_SC_MODE_CNTL);
    reg::PA_SU_VTX_CNTL vc;
    vc.value = ReadReg(r, XE_GPU_REG_PA_SU_VTX_CNTL);

    const float scale_xy[2] = {vte.vport_x_scale_ena ? ReadRegFloat(r, XE_GPU_REG_PA_CL_VPORT_XSCALE) : 1.0f,
                               vte.vport_y_scale_ena ? ReadRegFloat(r, XE_GPU_REG_PA_CL_VPORT_YSCALE) : 1.0f};
    const float scale_z = vte.vport_z_scale_ena ? ReadRegFloat(r, XE_GPU_REG_PA_CL_VPORT_ZSCALE) : 1.0f;
    const float offset_base_xy[2] = {vte.vport_x_offset_ena ? ReadRegFloat(r, XE_GPU_REG_PA_CL_VPORT_XOFFSET) : 0.0f,
                                     vte.vport_y_offset_ena ? ReadRegFloat(r, XE_GPU_REG_PA_CL_VPORT_YOFFSET) : 0.0f};
    const float offset_z = vte.vport_z_offset_ena ? ReadRegFloat(r, XE_GPU_REG_PA_CL_VPORT_ZOFFSET) : 0.0f;
    float offset_add_xy[2] = {};
    reg::PA_SC_WINDOW_OFFSET wo;
    wo.value = ReadReg(r, XE_GPU_REG_PA_SC_WINDOW_OFFSET);
    if (sc.vtx_window_offset_enable)
    {
      offset_add_xy[0] += float(wo.window_x_offset);
      offset_add_xy[1] += float(wo.window_y_offset);
    }
    if (vc.pix_center == xenos::PixelCenter::kD3DZero)
    {
      offset_add_xy[0] += 0.5f;
      offset_add_xy[1] += 0.5f;
    }
    const uint32_t xy_max_unscaled = D3D12_VIEWPORT_BOUNDS_MAX / scale;
    int32_t xy_offset[2] = {};
    uint32_t xy_extent[2] = {};
    float z_min = 0.0f, z_max = 1.0f;
    float ndc_scale[3], ndc_offset[3];
    if (clip.clip_disable)
    {
      // Screen-space draws (clears, UI): a huge host viewport, all offsetting in the vertex shader.
      for (uint32_t i = 0; i < 2; ++i)
      {
        const uint32_t extent_unscaled = std::min<uint32_t>(xenos::kTexture2DCubeMaxWidthHeight, xy_max_unscaled);
        xy_extent[i] = extent_unscaled * scale;
        const float extent_f = float(extent_unscaled);
        const float pixels_to_ndc = 2.0f / extent_f;
        ndc_scale[i] = scale_xy[i] * pixels_to_ndc;
        ndc_offset[i] = (offset_base_xy[i] - extent_f * 0.5f + offset_add_xy[i]) * pixels_to_ndc;
      }
      ndc_scale[2] = scale_z;
      ndc_offset[2] = offset_z;
    }
    else
    {
      for (uint32_t i = 0; i < 2; ++i)
      {
        const float offset_axis = offset_base_xy[i] + offset_add_xy[i];
        const float scale_abs = std::abs(scale_xy[i]);
        const float max_f = float(xy_max_unscaled);
        const int32_t axis_1 = int32_t(std::floor(std::clamp(offset_axis + scale_abs, 0.0f, max_f)));
        const int32_t axis_0 =
            int32_t(std::floor(std::clamp(offset_axis - scale_abs, float(axis_1) - max_f, max_f)));
        const uint32_t extent = axis_1 > 0 ? uint32_t(axis_1 - axis_0) : 0;
        xy_offset[i] = axis_0 * int32_t(scale);
        xy_extent[i] = extent * scale;
        if (extent)
        {
          const float extent_f = float(extent);
          ndc_scale[i] = scale_xy[i] * 2.0f / extent_f;
          ndc_offset[i] = (offset_axis - (float(axis_0) + extent_f * 0.5f)) * 2.0f / extent_f;
        }
        else
        {
          ndc_scale[i] = 1.0f;
          ndc_offset[i] = 0.0f;
        }
      }
      float host_clip_offset_z, host_clip_scale_z;
      if (clip.dx_clip_space_def)
      {
        host_clip_offset_z = offset_z;
        host_clip_scale_z = scale_z;
        ndc_scale[2] = 1.0f;
        ndc_offset[2] = 0.0f;
      }
      else
      {
        // -W..W clip space remapped to 0..W.
        host_clip_offset_z = offset_z - scale_z;
        host_clip_scale_z = scale_z * 2.0f;
        ndc_scale[2] = 0.5f;
        ndc_offset[2] = 0.5f;
      }
      if (!ps_writes_depth)
      {
        z_min = std::clamp(host_clip_offset_z, 0.0f, 1.0f);
        z_max = std::clamp(host_clip_offset_z + host_clip_scale_z, 0.0f, 1.0f);
        if (z_min > z_max)
        {
          std::swap(z_min, z_max);
          ndc_scale[2] = -ndc_scale[2];
          ndc_offset[2] = 1.0f - ndc_offset[2];
        }
      }
    }
    ndc_scale[1] = -ndc_scale[1]; // origin bottom-left
    ndc_offset[1] = -ndc_offset[1];
    out.viewport.TopLeftX = float(xy_offset[0]);
    out.viewport.TopLeftY = float(xy_offset[1]);
    out.viewport.Width = float(xy_extent[0]);
    out.viewport.Height = float(xy_extent[1]);
    out.viewport.MinDepth = z_min;
    out.viewport.MaxDepth = z_max;
    for (uint32_t i = 0; i < 3; ++i)
    {
      out.ndc_scale[i] = ndc_scale[i];
      out.ndc_offset[i] = ndc_offset[i];
    }
    out.vte_flags = (vte.vtx_xy_fmt ? 1u : 0u) | (vte.vtx_z_fmt ? 2u : 0u) | (vte.vtx_w0_fmt ? 4u : 0u);
    out.depth_clip = !clip.clip_disable;
    out.empty_viewport = xy_extent[0] == 0 || xy_extent[1] == 0;

    // Scissor (GetScissor, clamp_to_surface_pitch = true), then the resolution scale.
    reg::PA_SC_WINDOW_SCISSOR_TL wtl;
    wtl.value = ReadReg(r, XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL);
    reg::PA_SC_WINDOW_SCISSOR_BR wbr;
    wbr.value = ReadReg(r, XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR);
    int32_t tl_x = int32_t(wtl.tl_x), tl_y = int32_t(wtl.tl_y);
    int32_t br_x = int32_t(wbr.br_x), br_y = int32_t(wbr.br_y);
    if (!wtl.window_offset_disable)
    {
      tl_x += wo.window_x_offset;
      tl_y += wo.window_y_offset;
      br_x += wo.window_x_offset;
      br_y += wo.window_y_offset;
    }
    reg::PA_SC_SCREEN_SCISSOR_TL stl;
    stl.value = ReadReg(r, XE_GPU_REG_PA_SC_SCREEN_SCISSOR_TL);
    reg::PA_SC_SCREEN_SCISSOR_BR sbr;
    sbr.value = ReadReg(r, XE_GPU_REG_PA_SC_SCREEN_SCISSOR_BR);
    tl_x = std::max(tl_x, int32_t(stl.tl_x));
    tl_y = std::max(tl_y, int32_t(stl.tl_y));
    br_x = std::min(br_x, int32_t(sbr.br_x));
    br_y = std::min(br_y, int32_t(sbr.br_y));
    reg::RB_SURFACE_INFO si;
    si.value = ReadReg(r, XE_GPU_REG_RB_SURFACE_INFO);
    tl_x = std::min(tl_x, int32_t(si.surface_pitch));
    br_x = std::min(br_x, int32_t(si.surface_pitch));
    tl_x = std::max(tl_x, 0);
    tl_y = std::max(tl_y, 0);
    br_x = std::max(br_x, tl_x);
    br_y = std::max(br_y, tl_y);
    out.scissor.left = LONG(tl_x) * LONG(scale);
    out.scissor.top = LONG(tl_y) * LONG(scale);
    out.scissor.right = LONG(br_x) * LONG(scale);
    out.scissor.bottom = LONG(br_y) * LONG(scale);
  }

  // ---------------------------------------------------------------------------
  // Topology
  // ---------------------------------------------------------------------------
  Topology TopologyFor(uint32_t prim)
  {
    Topology t;
    t.supported = true;
    switch (xenos::PrimitiveType(prim))
    {
    case xenos::PrimitiveType::kPointList:
      t.type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
      t.topology = D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
      t.points = true;
      break;
    case xenos::PrimitiveType::kLineList:
      t.type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
      t.topology = D3D_PRIMITIVE_TOPOLOGY_LINELIST;
      break;
    case xenos::PrimitiveType::kLineStrip:
      t.type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
      t.topology = D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
      break;
    case xenos::PrimitiveType::kTriangleList:
      break;
    case xenos::PrimitiveType::kTriangleStrip:
      t.topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
      break;
    case xenos::PrimitiveType::kTriangleFan:
    case xenos::PrimitiveType::kQuadList:
      t.expand = true; // rewritten to a triangle list in native_draw.cpp
      break;
    case xenos::PrimitiveType::kRectangleList:
      // Three vertices per rectangle as a triangle list; the rectangle-list GS emits the four-vertex strip (the
      // SDK's PipelineGeometryShader::kRectangleList). Not expected from PDZ (D3D's own Clear / Resolve path).
      t.rects = true;
      t.supported = REXCVAR_GET(pdz_native_rectlist);
      break;
    default:
      // kLineLoop, kQuadStrip, kPolygon: not expected from PDZ; counted and skipped.
      t.supported = false;
      break;
    }
    return t;
  }

  PrimitiveReset DecodePrimitiveReset(const shadow::DrawRecord &r, uint32_t prim, bool index32)
  {
    using namespace rex::graphics;
    PrimitiveReset out;
    reg::PA_SU_SC_MODE_CNTL sc;
    sc.value = ReadReg(r, XE_GPU_REG_PA_SU_SC_MODE_CNTL);
    if (!sc.multi_prim_ib_ena)
    {
      return out;
    }
    switch (xenos::PrimitiveType(prim))
    {
    case xenos::PrimitiveType::kLineStrip:
    case xenos::PrimitiveType::kTriangleStrip:
    case xenos::PrimitiveType::kTriangleFan:
      break;
    default:
      // Lists (QUADLIST and RECTLIST included) never reset, as in the SDK (Vulkan forbids it, D3D12 leaves it
      // implementation-defined); kLineLoop / kQuadStrip / kPolygon have no host topology here.
      return out;
    }
    reg::VGT_MULTI_PRIM_IB_RESET_INDX ri;
    ri.value = ReadReg(r, XE_GPU_REG_VGT_MULTI_PRIM_IB_RESET_INDX);
    out.index = ri.reset_indx;
    // A 16-bit index can never equal a reset value above 0xFFFF: no reset (the SDK's rule).
    out.enabled = index32 || out.index <= 0xFFFFu;
    return out;
  }

  // ---------------------------------------------------------------------------
  // Point expansion geometry shader (compiled once from this source with d3dcompiler_47 via the device's
  // D3DCompile pointer). One point -> a screen-aligned quad of PA_SU_POINT_SIZE; every VS output
  // (SV_Position + TEXCOORD0..15, the fork's VS always writes all 16) is copied to the four corners. Xenos
  // point-sprite texture coordinate generation (PsParamGen ZW for points) is NOT reproduced, so the corners get the
  // point's own interpolators. PsParamGen for polygons (the face in the sign of x, the only use PDZ makes of it) is
  // native_paramgen.h; a param-gen pixel shader drawn with points would see the expanded quad's face, where the
  // hardware always reports a front face (none seen in PDZ).
  // ---------------------------------------------------------------------------
  namespace
  {
    const char kPointGsSource[] = R"(
cbuffer SystemConstants : register(b2, space0)
{
    float2 g_HalfPixelOffset;
    float g_AlphaRef;
    uint g_AlphaFunc;
    float4 g_PointSize; // x, y = half size in guest pixels; z, w = 2 / viewport width, height
    uint g_Flags;
};
struct V
{
    float4 pos : SV_Position;
    float4 t0 : TEXCOORD0;  float4 t1 : TEXCOORD1;  float4 t2 : TEXCOORD2;  float4 t3 : TEXCOORD3;
    float4 t4 : TEXCOORD4;  float4 t5 : TEXCOORD5;  float4 t6 : TEXCOORD6;  float4 t7 : TEXCOORD7;
    float4 t8 : TEXCOORD8;  float4 t9 : TEXCOORD9;  float4 t10 : TEXCOORD10; float4 t11 : TEXCOORD11;
    float4 t12 : TEXCOORD12; float4 t13 : TEXCOORD13; float4 t14 : TEXCOORD14; float4 t15 : TEXCOORD15;
};
[maxvertexcount(4)]
void main(point V input[1], inout TriangleStream<V> stream)
{
    V v = input[0];
    float2 half_ndc = g_PointSize.xy * g_PointSize.zw * v.pos.w;
    const float2 corners[4] = { float2(-1.0, -1.0), float2(-1.0, 1.0), float2(1.0, -1.0), float2(1.0, 1.0) };
    [unroll] for (int i = 0; i < 4; ++i)
    {
        V o = v;
        o.pos.xy = v.pos.xy + corners[i] * half_ndc;
        stream.Append(o);
    }
    stream.RestartStrip();
}
)";

    // SDK draw_util::GetD3D10IntegerPolygonOffset (include/rex/graphics/util/draw.h), replicated to keep this
    // file's include set to the two register headers.
    int32_t IntegerPolygonOffset(bool is_float24, float polygon_offset)
    {
      constexpr float kFactorUnorm24 = float((UINT32_C(1) << 24) - 1);
      constexpr float kFactorFloat24 = float(UINT32_C(1) << (21 + 3));
      int32_t v = int32_t(std::ceil(std::abs(polygon_offset) *
                                    (is_float24 ? kFactorFloat24 * (1.0f / 8.0f) : kFactorUnorm24)));
      if (is_float24)
      {
        v <<= 3;
      }
      return polygon_offset < 0 ? -v : v;
    }

    D3D12_BLEND BlendFactor(uint32_t xenos_factor, bool alpha)
    {
      D3D12_BLEND b = D3D12_BLEND_ZERO;
      switch (xenos::BlendFactor(xenos_factor))
      {
      case xenos::BlendFactor::kZero: b = D3D12_BLEND_ZERO; break;
      case xenos::BlendFactor::kOne: b = D3D12_BLEND_ONE; break;
      case xenos::BlendFactor::kSrcColor: b = alpha ? D3D12_BLEND_SRC_ALPHA : D3D12_BLEND_SRC_COLOR; break;
      case xenos::BlendFactor::kOneMinusSrcColor: b = alpha ? D3D12_BLEND_INV_SRC_ALPHA : D3D12_BLEND_INV_SRC_COLOR; break;
      case xenos::BlendFactor::kSrcAlpha: b = D3D12_BLEND_SRC_ALPHA; break;
      case xenos::BlendFactor::kOneMinusSrcAlpha: b = D3D12_BLEND_INV_SRC_ALPHA; break;
      case xenos::BlendFactor::kDstColor: b = alpha ? D3D12_BLEND_DEST_ALPHA : D3D12_BLEND_DEST_COLOR; break;
      case xenos::BlendFactor::kOneMinusDstColor: b = alpha ? D3D12_BLEND_INV_DEST_ALPHA : D3D12_BLEND_INV_DEST_COLOR; break;
      case xenos::BlendFactor::kDstAlpha: b = D3D12_BLEND_DEST_ALPHA; break;
      case xenos::BlendFactor::kOneMinusDstAlpha: b = D3D12_BLEND_INV_DEST_ALPHA; break;
      case xenos::BlendFactor::kConstantColor: b = D3D12_BLEND_BLEND_FACTOR; break;
      case xenos::BlendFactor::kOneMinusConstantColor: b = D3D12_BLEND_INV_BLEND_FACTOR; break;
      case xenos::BlendFactor::kConstantAlpha: b = D3D12_BLEND_BLEND_FACTOR; break; // ALPHA_FACTOR needs a newer runtime; the SDK falls back the same way
      case xenos::BlendFactor::kOneMinusConstantAlpha: b = D3D12_BLEND_INV_BLEND_FACTOR; break;
      case xenos::BlendFactor::kSrcAlphaSaturate: b = D3D12_BLEND_SRC_ALPHA_SAT; break;
      default: b = D3D12_BLEND_ZERO; break;
      }
      return b;
    }

    bool IsConstantFactor(uint32_t xenos_factor) { return xenos_factor >= 12 && xenos_factor <= 15; }

    D3D12_BLEND_OP BlendOp(uint32_t xenos_op)
    {
      switch (xenos::BlendOp(xenos_op))
      {
      case xenos::BlendOp::kSubtract: return D3D12_BLEND_OP_SUBTRACT;
      case xenos::BlendOp::kMin: return D3D12_BLEND_OP_MIN;
      case xenos::BlendOp::kMax: return D3D12_BLEND_OP_MAX;
      case xenos::BlendOp::kRevSubtract: return D3D12_BLEND_OP_REV_SUBTRACT;
      default: return D3D12_BLEND_OP_ADD;
      }
    }

    D3D12_COMPARISON_FUNC Compare(uint32_t xenos_func)
    {
      return D3D12_COMPARISON_FUNC(uint32_t(D3D12_COMPARISON_FUNC_NEVER) + (xenos_func & 7));
    }
    D3D12_STENCIL_OP StencilOp(uint32_t xenos_op)
    {
      return D3D12_STENCIL_OP(uint32_t(D3D12_STENCIL_OP_KEEP) + (xenos_op & 7));
    }

    // Everything the PSO depends on, zero-padded and hashed as bytes.
    struct PsoKey
    {
      uint64_t vs_hash;
      uint64_t ps_hash;
      uint64_t layout_hash;
      uint32_t depthcontrol;
      uint32_t blendcontrol[4];
      uint32_t color_mask;
      uint32_t stencil_masks; // read | write << 8
      uint32_t raster;        // cull (2) | front_ccw (1) << 2 | wireframe (1) << 3 | points (1) << 4 | no depth clip << 5
                              // | rectangle-list GS << 6 | user-clip-plane GS << 7
      int32_t depth_bias;
      float slope_bias;
      uint32_t topology_type;
      uint32_t rt_count;
      uint32_t rt[4];
      uint32_t ds;
      uint32_t depth_variant; // 1 = SV_Depth 20e4 PS variant (native_depth20e4.h); the fixed-function bias is then 0;
                              // 2 / 3 = the same with compare edge 1 / 2 (pdz_native_depth20e4_edge)
      uint32_t strip_cut;     // D3D12_INDEX_BUFFER_STRIP_CUT_VALUE (primitive reset, strips only); 0 = disabled
      uint32_t blend7e3_variant; // 7e3 blend PS variant (native_blend7e3.h): 0 none, 1 alpha clamp, 2 ROV (no RTV)
    };

    const char *HResultName(HRESULT hr)
    {
      switch (hr)
      {
      case E_INVALIDARG: return "E_INVALIDARG";
      case E_OUTOFMEMORY: return "E_OUTOFMEMORY";
      case DXGI_ERROR_DEVICE_REMOVED: return "DXGI_ERROR_DEVICE_REMOVED";
      default: return "HRESULT";
      }
    }

    // ---------------------------------------------------------------------------
    // PSO description (CreatePso and the pdz_native_pso_warmup workers)
    // ---------------------------------------------------------------------------
    constexpr uint32_t kPsoCacheFormat = 1; // the saved files' layout and PsoDescIdentity's input; bump on a change

    // The bytecode and input layout of one PSO: from the draw's shaders (CreatePso) or from a saved recipe (warm-up).
    // The ids (XXH3 of the bytes) are filled only with pdz_native_pso_cache on.
    struct PsoInputs
    {
      const void *vs = nullptr;
      size_t vs_size = 0;
      const void *ps = nullptr; // the guest pixel shader; size 0 = none (ShaderCache::NullPixel: depth only)
      size_t ps_size = 0;
      const void *variant = nullptr; // the SV_Depth 20e4 or 7e3 blend variant replacing it (key.depth_variant /
      size_t variant_size = 0;       // key.blend7e3_variant say which; never both)
      uint64_t vs_id = 0, ps_id = 0, variant_id = 0;
      std::vector<D3D12_INPUT_ELEMENT_DESC> elements;
    };

    uint64_t BlobId(const void *data, size_t size) { return data && size ? XXH3_64bits(data, size) : 0; }

    // One vertex input of the VS sidecar as an input element: ATTR<attr>, the stream = input-assembler slot.
    D3D12_INPUT_ELEMENT_DESC InputElement(uint32_t attr, DXGI_FORMAT format, uint32_t stream, uint32_t offset)
    {
      D3D12_INPUT_ELEMENT_DESC e = {};
      e.SemanticName = "ATTR";
      e.SemanticIndex = attr;
      e.Format = format;
      e.InputSlot = stream;
      e.AlignedByteOffset = offset;
      e.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
      e.InstanceDataStepRate = 0;
      return e;
    }

    // The D3D12 description of a PSO (CreatePso; the warm-up from a saved recipe): everything from the key and the
    // bytecode / input layout in `in` (`d` points into it); the Pipeline flags the draw needs are set in `out`. False: a
    // point-list key without the point geometry shader.
    bool BuildPsoDesc(ID3D12RootSignature *root, const PsoKey &key, const PsoInputs &in, bool point_gs_available,
                      D3D12_GRAPHICS_PIPELINE_STATE_DESC &d, Pipeline &out)
    {
      std::memset(&d, 0, sizeof(d)); // zeroed padding too: PsoDescIdentity hashes it as bytes
      d.pRootSignature = root;
      d.VS.pShaderBytecode = in.vs;
      d.VS.BytecodeLength = in.vs_size;
      // An empty PS (ShaderCache::NullPixel, a draw with no pixel shader) leaves the PS stage out: depth only.
      d.PS.pShaderBytecode = in.ps_size ? in.ps : nullptr;
      d.PS.BytecodeLength = in.ps_size;
      // SV_Depth 20e4 variant (decided in Lookup, part of the key): replaces the PS (or adds the depth-only one).
      // Lookup sets key.depth_variant / key.blend7e3_variant only with the variant's DXIL (never both).
      const void *depth_dxil = key.depth_variant ? in.variant : nullptr;
      const size_t depth_dxil_size = key.depth_variant ? in.variant_size : 0;
      const void *blend_dxil = key.blend7e3_variant ? in.variant : nullptr;
      const size_t blend_dxil_size = key.blend7e3_variant ? in.variant_size : 0;
      const bool depth_only_variant = depth_dxil && in.ps_size == 0;
      if (depth_dxil)
      {
        d.PS.pShaderBytecode = depth_dxil;
        d.PS.BytecodeLength = depth_dxil_size;
        out.shader_depth_bias = true;
      }
      // 7e3 blend variant (decided in Lookup, part of the key; never together with the depth one): replaces the PS.
      // Mode 2 writes render target 0 through its rasterizer-ordered view: no colour target in the PSO.
      const bool rov_variant = blend_dxil && key.blend7e3_variant == 2;
      if (blend_dxil)
      {
        d.PS.pShaderBytecode = blend_dxil;
        d.PS.BytecodeLength = blend_dxil_size;
        out.blend7e3 = key.blend7e3_variant;
      }
      const bool points = (key.raster >> 4) & 1;
      const bool rects = (key.raster >> 6) & 1;
      if ((key.raster >> 7) & 1)
      {
        // User clip planes (native_clip_gs_dxil.h): pass-through GS adding SV_ClipDistance (triangles only).
        d.GS.pShaderBytecode = kClipGsDxil;
        d.GS.BytecodeLength = sizeof(kClipGsDxil);
      }
      if (rects)
      {
        // Rectangle-list expansion (native_rectlist_gs_dxil.h, the SDK's kRectangleList GS as DXIL).
        d.GS.pShaderBytecode = kRectListGsDxil;
        d.GS.BytecodeLength = sizeof(kRectListGsDxil);
      }
      if (points)
      {
        if (!point_gs_available)
        {
          return false;
        }
        // The DXIL build of the same source (native_point_gs_dxil.h): a PSO cannot mix the runtime-compiled DXBC
        // with the DXIL VS / PS (the D3DCompile blob only gates availability now).
        d.GS.pShaderBytecode = kPointGsDxil;
        d.GS.BytecodeLength = sizeof(kPointGsDxil);
      }

      // Input layout (the VS sidecar's inputs, InputElement).
      d.InputLayout.pInputElementDescs = in.elements.empty() ? nullptr : in.elements.data();
      d.InputLayout.NumElements = UINT(in.elements.size());

      // Blend state (RB_BLENDCONTROL0..3 + RB_COLOR_MASK). Alpha-to-mask is not PSO state: with one sample it is
      // the PS alpha test (AlphaTestFor, the SDK's single-sample threshold); host alpha-to-coverage on a 1-sample
      // target would be a vendor-defined test without the guest's quad offsets.
      d.BlendState.AlphaToCoverageEnable = FALSE;
      d.BlendState.IndependentBlendEnable = TRUE;
      for (uint32_t i = 0; i < 8; ++i)
      {
        D3D12_RENDER_TARGET_BLEND_DESC &rt = d.BlendState.RenderTarget[i];
        rt.BlendEnable = FALSE;
        rt.LogicOpEnable = FALSE;
        rt.SrcBlend = D3D12_BLEND_ONE;
        rt.DestBlend = D3D12_BLEND_ZERO;
        rt.BlendOp = D3D12_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D12_BLEND_ONE;
        rt.DestBlendAlpha = D3D12_BLEND_ZERO;
        rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        rt.LogicOp = D3D12_LOGIC_OP_NOOP;
        rt.RenderTargetWriteMask = 0;
        if (rov_variant || i >= key.rt_count || key.rt[i] == uint32_t(DXGI_FORMAT_UNKNOWN))
        {
          continue;
        }
        reg::RB_BLENDCONTROL bc;
        bc.value = key.blendcontrol[i];
        const uint32_t src = uint32_t(bc.color_srcblend), dst = uint32_t(bc.color_destblend);
        const uint32_t src_a = uint32_t(bc.alpha_srcblend), dst_a = uint32_t(bc.alpha_destblend);
        const uint32_t op = uint32_t(bc.color_comb_fcn), op_a = uint32_t(bc.alpha_comb_fcn);
        const bool enabled = !(src == uint32_t(xenos::BlendFactor::kOne) && dst == uint32_t(xenos::BlendFactor::kZero) &&
                               op == uint32_t(xenos::BlendOp::kAdd) && src_a == uint32_t(xenos::BlendFactor::kOne) &&
                               dst_a == uint32_t(xenos::BlendFactor::kZero) && op_a == uint32_t(xenos::BlendOp::kAdd));
        if (enabled)
        {
          rt.BlendEnable = TRUE;
          rt.SrcBlend = BlendFactor(src, false);
          rt.DestBlend = BlendFactor(dst, false);
          rt.BlendOp = BlendOp(op);
          rt.SrcBlendAlpha = BlendFactor(src_a, true);
          rt.DestBlendAlpha = BlendFactor(dst_a, true);
          rt.BlendOpAlpha = BlendOp(op_a);
          if (IsConstantFactor(src) || IsConstantFactor(dst) || IsConstantFactor(src_a) || IsConstantFactor(dst_a))
          {
            out.uses_blend_factor = true;
          }
        }
        // The depth-only variant stands in for "no pixel shader": no colour output, so no colour writes.
        rt.RenderTargetWriteMask = depth_only_variant ? UINT8(0) : UINT8((key.color_mask >> (4 * i)) & 0xF);
      }
      d.SampleMask = UINT_MAX;

      // Rasterizer (PA_SU_SC_MODE_CNTL, PA_SU_POLY_OFFSET_*).
      d.RasterizerState.FillMode = ((key.raster >> 3) & 1) ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
      switch (key.raster & 3)
      {
      case 1: d.RasterizerState.CullMode = D3D12_CULL_MODE_FRONT; break;
      case 2: d.RasterizerState.CullMode = D3D12_CULL_MODE_BACK; break;
      default: d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; break;
      }
      d.RasterizerState.FrontCounterClockwise = ((key.raster >> 2) & 1) ? TRUE : FALSE;
      d.RasterizerState.DepthBias = out.shader_depth_bias ? 0 : key.depth_bias;
      d.RasterizerState.DepthBiasClamp = 0.0f;
      d.RasterizerState.SlopeScaledDepthBias = out.shader_depth_bias ? 0.0f : key.slope_bias;
      d.RasterizerState.DepthClipEnable = ((key.raster >> 5) & 1) ? FALSE : TRUE;
      d.RasterizerState.MultisampleEnable = FALSE;
      d.RasterizerState.AntialiasedLineEnable = FALSE;
      d.RasterizerState.ForcedSampleCount = 0;
      d.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

      // Depth / stencil (RB_DEPTHCONTROL, RB_STENCILREFMASK) only with a depth-stencil target bound.
      reg::RB_DEPTHCONTROL dc;
      dc.value = key.depthcontrol;
      const bool has_ds = key.ds != uint32_t(DXGI_FORMAT_UNKNOWN);
      d.DepthStencilState.DepthEnable = (has_ds && dc.z_enable) ? TRUE : FALSE;
      d.DepthStencilState.DepthWriteMask = (has_ds && dc.z_enable && dc.z_write_enable) ? D3D12_DEPTH_WRITE_MASK_ALL
                                                                                       : D3D12_DEPTH_WRITE_MASK_ZERO;
      d.DepthStencilState.DepthFunc = Compare(uint32_t(dc.zfunc));
      d.DepthStencilState.StencilEnable = (has_ds && dc.stencil_enable) ? TRUE : FALSE;
      d.DepthStencilState.StencilReadMask = UINT8(key.stencil_masks & 0xFF);
      d.DepthStencilState.StencilWriteMask = UINT8((key.stencil_masks >> 8) & 0xFF);
      d.DepthStencilState.FrontFace.StencilFailOp = StencilOp(uint32_t(dc.stencilfail));
      d.DepthStencilState.FrontFace.StencilDepthFailOp = StencilOp(uint32_t(dc.stencilzfail));
      d.DepthStencilState.FrontFace.StencilPassOp = StencilOp(uint32_t(dc.stencilzpass));
      d.DepthStencilState.FrontFace.StencilFunc = Compare(uint32_t(dc.stencilfunc));
      if (dc.backface_enable)
      {
        d.DepthStencilState.BackFace.StencilFailOp = StencilOp(uint32_t(dc.stencilfail_bf));
        d.DepthStencilState.BackFace.StencilDepthFailOp = StencilOp(uint32_t(dc.stencilzfail_bf));
        d.DepthStencilState.BackFace.StencilPassOp = StencilOp(uint32_t(dc.stencilzpass_bf));
        d.DepthStencilState.BackFace.StencilFunc = Compare(uint32_t(dc.stencilfunc_bf));
      }
      else
      {
        d.DepthStencilState.BackFace = d.DepthStencilState.FrontFace;
      }
      out.stencil = d.DepthStencilState.StencilEnable == TRUE;

      d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE(key.topology_type);
      d.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE(key.strip_cut); // primitive reset (0 = disabled)
      // The 7e3 ROV variant has no colour target (render target 0 is its u0); the draw binds the depth target only.
      const uint32_t rt_count = rov_variant ? 0u : key.rt_count;
      d.NumRenderTargets = rt_count;
      for (uint32_t i = 0; i < 4; ++i)
      {
        d.RTVFormats[i] = i < rt_count ? DXGI_FORMAT(key.rt[i]) : DXGI_FORMAT_UNKNOWN;
      }
      d.DSVFormat = DXGI_FORMAT(key.ds);
      d.SampleDesc.Count = 1;
      d.SampleDesc.Quality = 0;
      d.NodeMask = 0;
      d.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
      return true;
    }

    // A PSO's identity across sessions (its name in the pipeline library, the warm-up's check against the draw that
    // takes it): the description without its pointers, the bytecode ids, the input layout and the root signature. A
    // change to how a key maps to D3D12 state changes it too, so a PSO saved by another build is never used.
    uint64_t PsoDescIdentity(const D3D12_GRAPHICS_PIPELINE_STATE_DESC &d, const PsoInputs &in, uint64_t root_id)
    {
      D3D12_GRAPHICS_PIPELINE_STATE_DESC c;
      std::memcpy(&c, &d, sizeof(c)); // BuildPsoDesc zeroed the padding
      std::memset(&c.pRootSignature, 0, sizeof(c.pRootSignature));
      std::memset(&c.VS, 0, sizeof(c.VS));
      std::memset(&c.PS, 0, sizeof(c.PS));
      std::memset(&c.DS, 0, sizeof(c.DS));
      std::memset(&c.HS, 0, sizeof(c.HS));
      std::memset(&c.GS, 0, sizeof(c.GS));
      std::memset(&c.StreamOutput, 0, sizeof(c.StreamOutput));
      std::memset(&c.InputLayout, 0, sizeof(c.InputLayout));
      std::memset(&c.CachedPSO, 0, sizeof(c.CachedPSO));
      const uint64_t ids[5] = {root_id, in.vs_id, in.ps_id, in.variant_id,
                               BlobId(d.GS.pShaderBytecode, d.GS.BytecodeLength)};
      XXH3_state_t *st = XXH3_createState();
      XXH3_64bits_reset(st);
      const uint32_t format = kPsoCacheFormat;
      XXH3_64bits_update(st, &format, sizeof(format));
      XXH3_64bits_update(st, ids, sizeof(ids));
      XXH3_64bits_update(st, &c, sizeof(c));
      for (const D3D12_INPUT_ELEMENT_DESC &e : in.elements)
      {
        const uint32_t v[6] = {e.SemanticIndex, uint32_t(e.Format), e.InputSlot, e.AlignedByteOffset,
                               uint32_t(e.InputSlotClass), e.InstanceDataStepRate};
        XXH3_64bits_update(st, v, sizeof(v));
      }
      const uint64_t h = XXH3_64bits_digest(st);
      XXH3_freeState(st);
      return h;
    }

    // ---------------------------------------------------------------------------
    // pdz_native_pso_cache / pdz_native_pso_warmup: the PSOs across sessions
    // ---------------------------------------------------------------------------
    constexpr char kRecipeMagic[8] = {'P', 'D', 'Z', 'P', 'S', 'O', 'R', 'C'};
    constexpr char kLibraryMagic[8] = {'P', 'D', 'Z', 'P', 'S', 'O', 'L', 'B'};
    constexpr uint16_t kRecipeMaxAge = 16;     // sessions a recipe is kept without a draw using it
    constexpr uint64_t kSaveQuietFrames = 300; // saved this long after the last new PSO (a level load has settled) ...
    constexpr uint32_t kSaveEagerCount = 256;  // ... or as soon as this many are new
    constexpr uint32_t kFirstUseLogLines = 32; // pdz_native_first_use_log: lines per kind, then the periodic counters

    // How the PSO of a draw's first Lookup of its key came to be.
    enum class PsoOrigin : uint32_t
    {
      kCompiled, // CreateGraphicsPipelineState (the driver compiles, or finds it in its own shader cache)
      kLibrary,  // loaded from the pipeline library (pdz_native_pso_cache)
      kWarm,     // made by the warm-up before the draw needed it
      kWaited,   // made by the warm-up while the draw waited for it
    };

    // What a recipe saves of an input element (SemanticName is always "ATTR", per-vertex data).
    struct RecipeElement
    {
      uint32_t attr = 0, format = 0, stream = 0, offset = 0;
    };

    // Everything CreateGraphicsPipelineState needs for one PSO, without the draw: the key and the bytecode ids (the
    // bytes are in the recipe file once per id) and the input layout.
    struct Recipe
    {
      Recipe() { std::memset(&key, 0, sizeof(key)); }
      PsoKey key;
      uint64_t vs_id = 0, ps_id = 0, variant_id = 0;
      std::vector<RecipeElement> elements;
      uint16_t age = 0;      // sessions since a draw last used it
      uint64_t identity = 0; // PsoDescIdentity, computed when loaded / made (not saved: it follows the code)
    };
    using Blob = std::shared_ptr<const std::vector<uint8_t>>;

    // The GPU and driver the pipeline library was saved with (its file header): another one starts it empty.
    struct AdapterId
    {
      uint32_t vendor = 0, device = 0, subsys = 0, revision = 0;
      uint64_t driver = 0; // user-mode driver version (IDXGIAdapter::CheckInterfaceSupport)
    };

    AdapterId QueryAdapter(ID3D12Device *device)
    {
      AdapterId id;
      HMODULE dxgi = GetModuleHandleW(L"dxgi.dll"); // loaded by the provider (the swap chain)
      using PFN_CreateFactory = HRESULT(WINAPI *)(REFIID, void **);
      const auto create = dxgi ? reinterpret_cast<PFN_CreateFactory>(
                                     reinterpret_cast<void *>(GetProcAddress(dxgi, "CreateDXGIFactory1")))
                               : nullptr;
      IDXGIFactory4 *factory = nullptr;
      if (!create || FAILED(create(IID_PPV_ARGS(&factory))) || !factory)
      {
        return id;
      }
      IDXGIAdapter1 *adapter = nullptr;
      if (SUCCEEDED(factory->EnumAdapterByLuid(device->GetAdapterLuid(), IID_PPV_ARGS(&adapter))) && adapter)
      {
        DXGI_ADAPTER_DESC1 desc = {};
        if (SUCCEEDED(adapter->GetDesc1(&desc)))
        {
          id.vendor = desc.VendorId;
          id.device = desc.DeviceId;
          id.subsys = desc.SubSysId;
          id.revision = desc.Revision;
        }
        LARGE_INTEGER umd = {};
        if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd)))
        {
          id.driver = uint64_t(umd.QuadPart);
        }
        adapter->Release();
      }
      factory->Release();
      return id;
    }

    // pso.lib: this header, then the ID3D12PipelineLibrary::Serialize bytes.
    struct LibraryHeader
    {
      char magic[8];
      uint32_t format;
      uint32_t vendor, device, subsys, revision;
      uint32_t entries;     // PSOs stored in it over all sessions (an overgrown library starts over)
      uint64_t driver;
      uint64_t payload_size;
      uint64_t payload_hash; // XXH3 of the payload (a damaged file is not handed to the driver)
    };
    static_assert(sizeof(LibraryHeader) == 56);

    struct ByteWriter
    {
      std::vector<uint8_t> bytes;
      template <typename T>
      void Put(const T &v)
      {
        const uint8_t *p = reinterpret_cast<const uint8_t *>(&v);
        bytes.insert(bytes.end(), p, p + sizeof(T));
      }
      void PutBytes(const void *data, size_t size)
      {
        const uint8_t *p = static_cast<const uint8_t *>(data);
        bytes.insert(bytes.end(), p, p + size);
      }
    };

    struct ByteReader
    {
      const uint8_t *p = nullptr;
      size_t left = 0;
      template <typename T>
      bool Get(T &v)
      {
        if (left < sizeof(T))
        {
          return false;
        }
        std::memcpy(&v, p, sizeof(T));
        p += sizeof(T);
        left -= sizeof(T);
        return true;
      }
    };

    bool ReadWholeFile(const std::string &path, std::vector<uint8_t> &out)
    {
      std::ifstream f(path, std::ios::binary | std::ios::ate);
      if (!f)
      {
        return false;
      }
      const std::streamoff size = f.tellg();
      if (size <= 0)
      {
        return false;
      }
      out.resize(size_t(size));
      f.seekg(0);
      return bool(f.read(reinterpret_cast<char *>(out.data()), std::streamsize(size)));
    }

    // Written to <path>.tmp and renamed over <path>: a crash or exit mid-write leaves the previous file.
    bool WriteFileAtomically(const std::string &path, const std::vector<uint8_t> &bytes)
    {
      const std::string tmp = path + ".tmp";
      {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f || !f.write(reinterpret_cast<const char *>(bytes.data()), std::streamsize(bytes.size())))
        {
          return false;
        }
      }
      return MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
    }

    double MsSince(std::chrono::steady_clock::time_point t0)
    {
      return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    }

    // Seconds since the process was created (the warm-up's end compared with the front end's arrival in the log).
    double SecondsSinceProcessStart()
    {
      FILETIME created = {}, exited = {}, kernel = {}, user = {}, now = {};
      if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
      {
        return -1.0;
      }
      GetSystemTimeAsFileTime(&now);
      const auto to64 = [](const FILETIME &f) { return (uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
      return double(int64_t(to64(now) - to64(created))) / 1e7;
    }

    // The process's private bytes and its use of the adapter's local (video) memory, for the warm-up's memory line.
    // 0 when unavailable. The difference across the warm-up is approximate: the game allocates meanwhile too.
    void QueryMemory(ID3D12Device *device, uint64_t &private_bytes, uint64_t &video_bytes)
    {
      private_bytes = 0;
      video_bytes = 0;
      using PFN_Info = BOOL(WINAPI *)(HANDLE, PROCESS_MEMORY_COUNTERS *, DWORD);
      if (HMODULE k32 = GetModuleHandleW(L"kernel32.dll"))
      {
        if (const auto info = reinterpret_cast<PFN_Info>(
                reinterpret_cast<void *>(GetProcAddress(k32, "K32GetProcessMemoryInfo"))))
        {
          PROCESS_MEMORY_COUNTERS_EX c = {};
          c.cb = sizeof(c);
          if (info(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&c), sizeof(c)))
          {
            private_bytes = c.PrivateUsage;
          }
        }
      }
      HMODULE dxgi = GetModuleHandleW(L"dxgi.dll");
      using PFN_CreateFactory = HRESULT(WINAPI *)(REFIID, void **);
      const auto create = dxgi ? reinterpret_cast<PFN_CreateFactory>(
                                     reinterpret_cast<void *>(GetProcAddress(dxgi, "CreateDXGIFactory1")))
                               : nullptr;
      IDXGIFactory4 *factory = nullptr;
      if (!create || FAILED(create(IID_PPV_ARGS(&factory))) || !factory)
      {
        return;
      }
      IDXGIAdapter3 *adapter = nullptr;
      if (SUCCEEDED(factory->EnumAdapterByLuid(device->GetAdapterLuid(), IID_PPV_ARGS(&adapter))) && adapter)
      {
        DXGI_QUERY_VIDEO_MEMORY_INFO mem = {};
        if (SUCCEEDED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &mem)))
        {
          video_bytes = mem.CurrentUsage;
        }
        adapter->Release();
      }
      factory->Release();
    }

    // The pipeline library, the recipes and the warm-up (pdz_native_pso_cache, pdz_native_pso_warmup), in
    // <exe dir>\native_cache: pso.lib (the library, tied to the GPU and driver) and pso_recipes.bin (the recipes with
    // their bytecode, independent of both). Threads: the draw thread (Lookup, under the render mutex: TakeWarm, Create,
    // Record, MarkUsed, MaybeSave), the loader (reads both files, then warms up with the other workers) and the saver.
    // The draw thread never waits for the library while it is serialized (try_lock: the driver compiles, the store is
    // deferred) and waits for the warm-up only for the one PSO it needs while a worker is making that PSO.
    class PsoStore
    {
    public:
      enum WarmState : uint32_t
      {
        kWarmOff = 0,     // pdz_native_pso_warmup off: nothing is queued
        kWarmLoading = 1, // the loader reads the files (the draws claim the keys they make meanwhile)
        kWarmRunning = 2,
        kWarmDone = 3,
      };
      enum LibraryState : uint32_t
      {
        kLibLoading = 0,
        kLibReady = 1,
        kLibOff = 2, // unsupported or failed: the driver's own shader cache only
      };
      // Why TakeWarm gave the draw no PSO (the misses of the summary line).
      enum class Miss : uint32_t
      {
        kNone = 0,     // taken
        kNotListed,    // no recipe for it (new content), or the warm-up is off
        kNotReady,     // in the list, but no worker had started it (claimed: the draw makes it)
        kStale,        // made for other bytecode / state (a changed shader or cvar)
        kFailed,       // the warm-up could not make it
      };

      void Start(ID3D12Device *dev, ID3D12RootSignature *root_signature, uint64_t root_signature_id,
                 bool point_gs_available)
      {
        device_ = dev;
        root_ = root_signature;
        root_id_ = root_signature_id;
        point_gs_ = point_gs_available;
        // Per user in a release, beside the executable in a developer's build (native_dxc.h NativeCacheDir).
        dir_ = NativeCacheDir();
        lib_path_ = dir_ + "\\pso.lib";
        recipe_path_ = dir_ + "\\pso_recipes.bin";
        seed_path_ = dir_ + "\\pso_seed.bin";
        prewarm_ = REXCVAR_GET(pdz_native_pso_prewarm);
        warm_state_.store(REXCVAR_GET(pdz_native_pso_warmup) || prewarm_ ? kWarmLoading : kWarmOff);
        std::thread([this]() { Load(); }).detach();
      }

      // The draw's first Lookup of `key_hash`: true with out.pso = the warmed PSO when one of the same identity is
      // ready (waits while a worker is making it; `waited_ms` > 0 then). False (`miss` says why): the draw makes it
      // itself; a queued entry is claimed so no worker makes it too, one made for other bytecode / state is released
      // (stale). pdz_native_pso_prewarm: entries are found by identity (keys mapping to the same description share
      // one PSO) and stay: the warm-up keeps its reference, the draw gets one of its own (AddRef).
      bool TakeWarm(uint64_t key_hash, uint64_t identity, Pipeline &out, double &waited_ms, Miss &miss)
      {
        waited_ms = 0.0;
        miss = Miss::kNotListed;
        const uint32_t state = warm_state_.load(std::memory_order_acquire);
        if (state == kWarmOff)
        {
          return false;
        }
        const uint64_t warm_key = prewarm_ ? identity : key_hash;
        std::unique_lock lock(warm_mutex_);
        auto it = warm_.find(warm_key);
        if (it == warm_.end())
        {
          if (warm_state_.load(std::memory_order_relaxed) == kWarmLoading)
          {
            claimed_.insert(warm_key); // the loader does not queue it
          }
          // In the list (loaded_by_identity_ is fixed once recipes_ready_) but not queued: the files were still being
          // read, it was claimed, past pdz_native_pso_warmup_max, or released; else new content.
          const bool listed = !recipes_ready_.load(std::memory_order_acquire) || loaded_by_identity_.count(identity);
          miss = listed ? Miss::kNotReady : Miss::kNotListed;
          return false;
        }
        if (it->second.state == WarmEntry::kBuilding)
        {
          const auto t0 = std::chrono::steady_clock::now();
          warm_cv_.wait(lock, [&]() { return it->second.state != WarmEntry::kBuilding; });
          waited_ms = std::max(MsSince(t0), 0.001);
          ++waits_;
        }
        WarmEntry &e = it->second;
        if (e.state == WarmEntry::kQueued)
        {
          e.state = WarmEntry::kClaimed;
          ++warm_claimed_;
          miss = Miss::kNotReady;
          return false;
        }
        if (e.state == WarmEntry::kReady)
        {
          if (e.identity == identity)
          {
            miss = Miss::kNone;
            ++taken_;
            if (prewarm_)
            {
              e.pso->AddRef(); // the draw's reference (pdz_native_pso_cap releases that one only)
              out.pso = e.pso;
              return true;
            }
            out.pso = e.pso;
            warm_.erase(it);
            warm_held_.fetch_sub(1);
            return true;
          }
          e.pso->Release(); // never bound
          warm_held_.fetch_sub(1);
          ++stale_;
          miss = Miss::kStale;
        }
        else
        {
          miss = e.state == WarmEntry::kFailed ? Miss::kFailed : Miss::kNotReady;
        }
        if (prewarm_ && e.state == WarmEntry::kClaimed)
        {
          return false; // kept: a later key of the same identity is also "not ready" (made on demand)
        }
        warm_.erase(it); // stale, failed or claimed earlier (its PSO released by pdz_native_pso_cap since)
        return false;
      }

      // The PSO of `d`: loaded from the pipeline library when it holds `identity`, else created by the driver and
      // stored. `wait`: a worker waits for the library; the draw thread uses it only when it is free.
      HRESULT Create(const D3D12_GRAPHICS_PIPELINE_STATE_DESC &d, uint64_t identity, bool wait,
                     ID3D12PipelineState **out, bool *from_library)
      {
        *out = nullptr;
        *from_library = false;
        wchar_t name[24];
        PsoName(identity, name);
        const uint32_t lib_state = lib_state_.load(std::memory_order_acquire);
        if (lib_state == kLibReady)
        {
          std::shared_lock lock(lib_mutex_, std::defer_lock);
          LockLibrary(lock, wait);
          if (lock.owns_lock() && library_)
          {
            // The library synchronizes itself, except for two loads of the same PSO at once: one stripe per name.
            std::lock_guard name_lock(name_mutex_[identity % kNameStripes]);
            if (SUCCEEDED(library_->LoadGraphicsPipeline(name, &d, IID_PPV_ARGS(out))) && *out)
            {
              *from_library = true;
              return S_OK;
            }
          }
          *out = nullptr;
        }
        const HRESULT hr = device_->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(out));
        if (FAILED(hr))
        {
          *out = nullptr;
          return hr;
        }
        if (lib_state == kLibReady)
        {
          Store(name, identity, *out, wait);
        }
        else if (lib_state == kLibLoading)
        {
          Defer(name, *out); // stored once the loader has the library
        }
        return hr;
      }

      // A PSO the draw thread made (not from the warm-up): its recipe for the next sessions. A recipe already saved is
      // only marked as used.
      void Record(const PsoKey &key, const PsoInputs &in, uint64_t identity, uint64_t frame)
      {
        last_new_frame_ = frame;
        std::lock_guard lock(recipes_mutex_);
        if (recipes_ready_.load(std::memory_order_acquire))
        {
          auto it = loaded_by_identity_.find(identity);
          if (it != loaded_by_identity_.end())
          {
            loaded_used_[it->second] = 1;
            return;
          }
        }
        if (!session_identities_.insert(identity).second)
        {
          return;
        }
        Recipe rc;
        rc.key = key;
        rc.vs_id = in.vs_id;
        rc.ps_id = in.ps_id;
        rc.variant_id = in.variant_id;
        rc.identity = identity;
        rc.elements.reserve(in.elements.size());
        for (const D3D12_INPUT_ELEMENT_DESC &e : in.elements)
        {
          RecipeElement re;
          re.attr = e.SemanticIndex;
          re.format = uint32_t(e.Format);
          re.stream = e.InputSlot;
          re.offset = e.AlignedByteOffset;
          rc.elements.push_back(re);
        }
        AddBlob(in.vs_id, in.vs, in.vs_size);
        AddBlob(in.ps_id, in.ps, in.ps_size);
        AddBlob(in.variant_id, in.variant, in.variant_size);
        session_.push_back(std::move(rc));
        session_count_.fetch_add(1);
        new_since_save_.fetch_add(1);
      }

      // A warmed PSO a draw took: its recipe stays young.
      void MarkUsed(uint64_t identity)
      {
        std::lock_guard lock(recipes_mutex_);
        if (!recipes_ready_.load(std::memory_order_acquire))
        {
          return;
        }
        auto it = loaded_by_identity_.find(identity);
        if (it != loaded_by_identity_.end())
        {
          loaded_used_[it->second] = 1;
        }
      }

      // Once per native frame (draw thread): saves on a background thread once the warm-up is over and there is
      // something new, kSaveQuietFrames after the last new PSO (or at once with kSaveEagerCount new ones).
      void MaybeSave(uint64_t frame)
      {
        const uint32_t state = warm_state_.load(std::memory_order_acquire);
        if (state == kWarmLoading || state == kWarmRunning || saving_.load(std::memory_order_acquire))
        {
          return;
        }
        if (!recipes_ready_.load(std::memory_order_acquire) || lib_state_.load(std::memory_order_acquire) == kLibLoading)
        {
          return; // the loader is still reading (warm-up off)
        }
        const uint32_t fresh = new_since_save_.load(std::memory_order_relaxed);
        const uint32_t stores = stores_unsaved_.load(std::memory_order_relaxed);
        if (fresh == 0 && stores == 0)
        {
          return;
        }
        if (fresh < kSaveEagerCount && frame - last_new_frame_ < kSaveQuietFrames)
        {
          return;
        }
        saving_.store(true, std::memory_order_release);
        std::thread([this]() {
          Save();
          saving_.store(false, std::memory_order_release);
        }).detach();
      }

      // The draws line's cache part (draw thread).
      std::string StatsText() const
      {
        const uint32_t lib = lib_state_.load(std::memory_order_acquire);
        return fmt::format("; cache: warm-up {} of {} ({} library, {} compiled, {} failed, {} claimed by draws), "
                           "taken {} (waited {}, stale {}), recipes {} + {} new, library {} (+{} stored), saves {}",
                           warm_done_.load(), warm_total_.load(), warm_library_.load(), warm_compiled_.load(),
                           warm_failed_.load(), warm_claimed_.load(), taken_.load(), waits_.load(), stale_.load(),
                           loaded_count_.load(), session_count_.load(),
                           lib == kLibReady ? "on" : (lib == kLibOff ? "off" : "loading"), library_stores_.load(),
                           saves_.load());
      }

      bool warm_running() const
      {
        const uint32_t s = warm_state_.load(std::memory_order_acquire);
        return s == kWarmLoading || s == kWarmRunning;
      }
      bool prewarm() const { return prewarm_; }
      // Waits until the warm-up is done (or off), at most `max_ms`; the ms waited. Any thread.
      double WaitDone(double max_ms) const
      {
        const auto t0 = std::chrono::steady_clock::now();
        while (warm_running() && MsSince(t0) < max_ms)
        {
          std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return MsSince(t0);
      }
      uint32_t warm_done() const { return warm_done_.load(std::memory_order_relaxed); }
      uint32_t warm_total() const { return warm_total_.load(std::memory_order_relaxed); }
      uint32_t warm_held() const { return warm_held_.load(std::memory_order_relaxed); }

      // The list / warm-up part of the summary line (draw thread or exit).
      std::string SummaryText() const
      {
        const uint32_t state = warm_state_.load(std::memory_order_acquire);
        std::string warm;
        if (state == kWarmOff)
        {
          warm = "off";
        }
        else if (state == kWarmDone)
        {
          warm = fmt::format("{} of {} ready in {:.0f} ms, {:.1f} s after process start ({} library, {} compiled, {} "
                             "failed, {} claimed by draws)",
                             warm_library_.load() + warm_compiled_.load(), warm_total_.load(), warm_ms_,
                             warm_done_after_s_, warm_library_.load(), warm_compiled_.load(), warm_failed_.load(),
                             warm_claimed_.load());
        }
        else
        {
          warm = fmt::format("running, {} of {} so far", warm_done_.load(), warm_total_.load());
        }
        return fmt::format("list {} recipes + {} new this session{}, warm-up {}", loaded_count_.load(),
                           session_count_.load(), prewarm_ ? " (pdz_native_pso_prewarm)" : "", warm);
      }

    private:
      struct WarmEntry
      {
        enum State : uint32_t
        {
          kQueued,
          kBuilding,
          kReady,
          kFailed,
          kClaimed, // a draw needed it before a worker started it: the draw made it
        };
        State state = kQueued;
        ID3D12PipelineState *pso = nullptr;
        uint64_t identity = 0;
      };

      static void PsoName(uint64_t identity, wchar_t (&name)[24])
      {
        swprintf(name, 24, L"pdz%016llx", static_cast<unsigned long long>(identity));
      }

      // The library's lock, shared by loads and stores (the library synchronizes those itself; log 084's first draw
      // waited 33.8 ms behind four warm-up workers on an exclusive lock), exclusive for Serialize. Workers wait for
      // it; the draw thread too unless the saver is serializing (only the draw thread starts a save, so saving_
      // false means no save runs), then it only tries.
      void LockLibrary(std::shared_lock<std::shared_mutex> &lock, bool wait)
      {
        if (wait || !saving_.load(std::memory_order_acquire))
        {
          lock.lock();
        }
        else
        {
          (void)lock.try_lock();
        }
      }

      // Under recipes_mutex_.
      void AddBlob(uint64_t id, const void *data, size_t size)
      {
        if (!id || blobs_.count(id))
        {
          return;
        }
        const uint8_t *p = static_cast<const uint8_t *>(data);
        blobs_.emplace(id, std::make_shared<const std::vector<uint8_t>>(p, p + size));
      }

      // The recipe's bytecode as PsoInputs (`hold` keeps the bytes alive). False when a blob is missing.
      static bool InputsFromRecipe(const Recipe &rc, const std::unordered_map<uint64_t, Blob> &blobs, Blob (&hold)[3],
                                   PsoInputs &in)
      {
        const uint64_t ids[3] = {rc.vs_id, rc.ps_id, rc.variant_id};
        for (uint32_t i = 0; i < 3; ++i)
        {
          hold[i].reset();
          if (!ids[i])
          {
            continue;
          }
          auto it = blobs.find(ids[i]);
          if (it == blobs.end() || !it->second)
          {
            return false;
          }
          hold[i] = it->second;
        }
        if (!hold[0])
        {
          return false; // no vertex shader
        }
        in.vs = hold[0]->data();
        in.vs_size = hold[0]->size();
        in.ps = hold[1] ? hold[1]->data() : nullptr;
        in.ps_size = hold[1] ? hold[1]->size() : 0;
        in.variant = hold[2] ? hold[2]->data() : nullptr;
        in.variant_size = hold[2] ? hold[2]->size() : 0;
        in.vs_id = rc.vs_id;
        in.ps_id = rc.ps_id;
        in.variant_id = rc.variant_id;
        in.elements.clear();
        in.elements.reserve(rc.elements.size());
        for (const RecipeElement &e : rc.elements)
        {
          in.elements.push_back(InputElement(e.attr, DXGI_FORMAT(e.format), e.stream, e.offset));
        }
        return true;
      }

      void Store(const wchar_t *name, uint64_t identity, ID3D12PipelineState *pso, bool wait)
      {
        std::shared_lock lock(lib_mutex_, std::defer_lock);
        LockLibrary(lock, wait);
        if (!lock.owns_lock())
        {
          Defer(name, pso);
          return;
        }
        std::lock_guard name_lock(name_mutex_[identity % kNameStripes]);
        if (library_ && SUCCEEDED(library_->StorePipeline(name, pso)))
        {
          library_stores_.fetch_add(1);
          stores_unsaved_.fetch_add(1);
        }
      }

      void Defer(const wchar_t *name, ID3D12PipelineState *pso)
      {
        if (lib_state_.load(std::memory_order_acquire) == kLibOff)
        {
          return;
        }
        pso->AddRef(); // pdz_native_pso_cap may release the cache's reference meanwhile
        std::lock_guard lock(pending_mutex_);
        pending_.emplace_back(std::wstring(name), pso);
        stores_unsaved_.fetch_add(1);
      }

      // Under lib_mutex_ (exclusive): the stores deferred while the library was loading or busy.
      void DrainPending()
      {
        std::vector<std::pair<std::wstring, ID3D12PipelineState *>> list;
        {
          std::lock_guard lock(pending_mutex_);
          list.swap(pending_);
        }
        for (auto &[name, pso] : list)
        {
          if (library_ && SUCCEEDED(library_->StorePipeline(name.c_str(), pso)))
          {
            library_stores_.fetch_add(1);
          }
          pso->Release();
        }
      }

      // Loader thread: both files, then the warm-up.
      void Load()
      {
        pdz::cpu::LowerBackgroundThread("PSO library load"); // pdz_cpu_placement, pdz_background_priority
        const auto t0 = std::chrono::steady_clock::now();
        std::error_code ec;
        std::filesystem::create_directories(dir_, ec);
        const std::string recipes_note = LoadRecipes(); // first: the count decides whether the library starts over
        const std::string library_note = OpenLibrary();
        REXLOG_INFO("pdz_native: PSO cache: {} recipes ({}); pipeline library {}; read in {:.0f} ms from {}",
                    loaded_.size(), recipes_note, library_note, MsSince(t0), dir_);
        if (warm_state_.load() == kWarmOff)
        {
          return;
        }
        Warmup();
      }

      // pso_recipes.bin: header (magic, format, sizeof(PsoKey), blob count, recipe count), the blobs (id, size, bytes),
      // the recipes (key, vs / ps / variant ids, age, element count, elements), XXH3 of all of it.
      std::string LoadRecipes()
      {
        std::unordered_map<uint64_t, Blob> blobs;
        std::string note = ReadRecipeFile(recipe_path_, blobs);
        // The shipped pipeline list's recipes (pipeline_seed, written at a first start): merged by identity, and
        // folded into pso_recipes.bin by the next save (then the file goes).
        std::error_code ec;
        if (std::filesystem::is_regular_file(seed_path_, ec))
        {
          const size_t before = loaded_.size();
          note += "; shipped list: " + ReadRecipeFile(seed_path_, blobs);
          seed_loaded_ = loaded_.size() > before;
          note += fmt::format(", {} new", loaded_.size() - before);
        }
        {
          std::lock_guard lock(recipes_mutex_);
          blobs_ = std::move(blobs);
          loaded_used_.assign(loaded_.size(), 0);
        }
        loaded_count_.store(uint32_t(loaded_.size()));
        recipes_ready_.store(true, std::memory_order_release);
        return note;
      }

      // One recipe file's recipes appended to loaded_ (new identities only) and its blobs to `blobs`; the note.
      std::string ReadRecipeFile(const std::string &path, std::unordered_map<uint64_t, Blob> &blobs)
      {
        std::vector<uint8_t> file;
        const bool read = ReadWholeFile(path, file);
        uint64_t trailer = 0;
        if (read && file.size() >= 40)
        {
          std::memcpy(&trailer, file.data() + file.size() - 8, 8);
        }
        if (!read)
        {
          return "none saved yet";
        }
        if (file.size() < 40 || XXH3_64bits(file.data(), file.size() - 8) != trailer)
        {
          return "file damaged: ignored";
        }
        ByteReader r{file.data(), file.size() - 8};
        char magic[8] = {};
        uint32_t format = 0, key_size = 0, blob_count = 0, recipe_count = 0;
        if (!r.Get(magic) || std::memcmp(magic, kRecipeMagic, 8) != 0 || !r.Get(format) || !r.Get(key_size) ||
            !r.Get(blob_count) || !r.Get(recipe_count) || format != kPsoCacheFormat || key_size != sizeof(PsoKey))
        {
          return "another format: ignored";
        }
        const size_t loaded_before = loaded_.size();
        std::vector<uint64_t> added_blobs;
        bool ok = true;
        size_t blob_bytes = 0;
        uint32_t file_blobs = 0;
        for (uint32_t i = 0; ok && i < blob_count; ++i)
        {
          uint64_t id = 0;
          uint32_t size = 0;
          ok = r.Get(id) && r.Get(size) && size <= r.left;
          if (ok)
          {
            if (blobs.emplace(id, std::make_shared<const std::vector<uint8_t>>(r.p, r.p + size)).second)
            {
              added_blobs.push_back(id);
            }
            r.p += size;
            r.left -= size;
            blob_bytes += size;
            ++file_blobs;
          }
        }
        uint32_t dropped = 0;
        for (uint32_t i = 0; ok && i < recipe_count; ++i)
        {
          Recipe rc;
          uint16_t element_count = 0;
          ok = r.Get(rc.key) && r.Get(rc.vs_id) && r.Get(rc.ps_id) && r.Get(rc.variant_id) && r.Get(rc.age) &&
               r.Get(element_count);
          for (uint32_t e = 0; ok && e < element_count; ++e)
          {
            RecipeElement re;
            ok = r.Get(re.attr) && r.Get(re.format) && r.Get(re.stream) && r.Get(re.offset);
            rc.elements.push_back(re);
          }
          if (!ok)
          {
            break;
          }
          // The identity with this build's mapping and root signature (a code change re-keys, never misuses).
          Blob hold[3];
          PsoInputs in;
          Pipeline flags;
          D3D12_GRAPHICS_PIPELINE_STATE_DESC d;
          if (!InputsFromRecipe(rc, blobs, hold, in) || !BuildPsoDesc(root_, rc.key, in, point_gs_, d, flags))
          {
            ++dropped;
            continue;
          }
          rc.identity = PsoDescIdentity(d, in, root_id_);
          if (loaded_by_identity_.emplace(rc.identity, uint32_t(loaded_.size())).second)
          {
            loaded_.push_back(std::move(rc));
          }
        }
        if (!ok)
        {
          for (size_t i = loaded_before; i < loaded_.size(); ++i)
          {
            loaded_by_identity_.erase(loaded_[i].identity);
          }
          loaded_.resize(loaded_before);
          for (const uint64_t id : added_blobs)
          {
            blobs.erase(id);
          }
          return "file truncated: ignored";
        }
        return fmt::format("{} recipes, {} shader blobs, {} KB{}", recipe_count, file_blobs, blob_bytes / 1024,
                           dropped ? fmt::format(", {} without their bytecode dropped", dropped) : std::string());
      }

      // A usable library file: "" (filled `library_file_`), else why not.
      std::string ReadLibraryFile(const AdapterId &adapter)
      {
        if (!ReadWholeFile(lib_path_, library_file_))
        {
          return "none saved yet";
        }
        LibraryHeader h;
        if (library_file_.size() < sizeof(h))
        {
          return "file damaged";
        }
        std::memcpy(&h, library_file_.data(), sizeof(h));
        if (std::memcmp(h.magic, kLibraryMagic, 8) != 0 || h.format != kPsoCacheFormat)
        {
          return "another format";
        }
        if (h.vendor != adapter.vendor || h.device != adapter.device || h.subsys != adapter.subsys ||
            h.revision != adapter.revision)
        {
          return fmt::format("saved on another GPU ({:04x}:{:04x})", h.vendor, h.device);
        }
        if (h.driver != adapter.driver)
        {
          return fmt::format("saved with another driver ({:016x}, now {:016x})", h.driver, adapter.driver);
        }
        if (h.payload_size != library_file_.size() - sizeof(h) ||
            XXH3_64bits(library_file_.data() + sizeof(h), size_t(h.payload_size)) != h.payload_hash)
        {
          return "file damaged";
        }
        // An outgrown library (entries of descriptions no build makes any more) starts over. With
        // pdz_native_pso_prewarm the list only grows, so the margin is wider: only dead entries count against it.
        if (h.entries > (prewarm_ ? 4 * uint32_t(loaded_.size()) + 2048 : 2 * uint32_t(loaded_.size()) + 512))
        {
          return fmt::format("outgrown ({} PSOs for {} recipes): rebuilt", h.entries, loaded_.size());
        }
        library_entries_loaded_ = h.entries;
        return std::string();
      }

      std::string OpenLibrary()
      {
        D3D12_FEATURE_DATA_SHADER_CACHE sc = {};
        if (FAILED(device_->CheckFeatureSupport(D3D12_FEATURE_SHADER_CACHE, &sc, sizeof(sc))) ||
            !(sc.SupportFlags & D3D12_SHADER_CACHE_SUPPORT_LIBRARY))
        {
          lib_state_.store(kLibOff, std::memory_order_release);
          return "not supported by the driver (the driver's own shader cache only)";
        }
        if (FAILED(device_->QueryInterface(IID_PPV_ARGS(&device1_))) || !device1_)
        {
          lib_state_.store(kLibOff, std::memory_order_release);
          return "unavailable (no ID3D12Device1)";
        }
        adapter_ = QueryAdapter(device_);
        std::string why = ReadLibraryFile(adapter_);
        ID3D12PipelineLibrary *lib = nullptr;
        std::string note;
        if (why.empty())
        {
          // The bytes must stay valid while the library lives: library_file_ is never freed.
          const HRESULT hr = device1_->CreatePipelineLibrary(library_file_.data() + sizeof(LibraryHeader),
                                                            library_file_.size() - sizeof(LibraryHeader),
                                                            IID_PPV_ARGS(&lib));
          if (FAILED(hr) || !lib)
          {
            lib = nullptr;
            why = hr == D3D12_ERROR_DRIVER_VERSION_MISMATCH ? std::string("the driver changed")
                  : hr == D3D12_ERROR_ADAPTER_NOT_FOUND     ? std::string("made on another GPU")
                                                             : fmt::format("rejected ({:#x})", uint32_t(hr));
          }
          else
          {
            note = fmt::format("loaded ({} PSOs, {} KB)", library_entries_loaded_,
                               (library_file_.size() - sizeof(LibraryHeader)) / 1024);
          }
        }
        if (!lib)
        {
          std::vector<uint8_t>().swap(library_file_);
          library_entries_loaded_ = 0;
          const HRESULT hr = device1_->CreatePipelineLibrary(nullptr, 0, IID_PPV_ARGS(&lib));
          if (FAILED(hr) || !lib)
          {
            lib_state_.store(kLibOff, std::memory_order_release);
            std::lock_guard lock(pending_mutex_);
            for (auto &[name, pso] : pending_)
            {
              pso->Release();
            }
            pending_.clear();
            return fmt::format("unavailable (CreatePipelineLibrary {:#x}; {})", uint32_t(hr), why);
          }
          note = "started empty: " + why;
        }
        {
          std::unique_lock lock(lib_mutex_);
          library_ = lib;
          DrainPending(); // what the draws made while the files were read
        }
        lib_state_.store(kLibReady, std::memory_order_release);
        return note;
      }

      void Warmup()
      {
        const auto t0 = std::chrono::steady_clock::now();
        // Most recently used first (age), then in the order they were first made.
        std::vector<uint32_t> order(loaded_.size());
        for (uint32_t i = 0; i < order.size(); ++i)
        {
          order[i] = i;
        }
        std::stable_sort(order.begin(), order.end(),
                         [this](uint32_t a, uint32_t b) { return loaded_[a].age < loaded_[b].age; });
        const int32_t max_cvar = REXCVAR_GET(pdz_native_pso_warmup_max);
        const int32_t cap = REXCVAR_GET(pdz_native_pso_cap);
        const size_t limit = prewarm_      ? order.size() // pdz_native_pso_prewarm: the whole list
                             : max_cvar > 0 ? size_t(max_cvar)
                                            : (cap > 0 ? size_t(cap) - size_t(cap) / 4 : order.size());
        {
          std::lock_guard lock(warm_mutex_);
          for (const uint32_t i : order)
          {
            if (queue_.size() >= limit)
            {
              break;
            }
            // By key (the per-session warm-up, as before) or by identity (prewarm: keys of one description share it).
            const uint64_t h = prewarm_ ? loaded_[i].identity : XXH3_64bits(&loaded_[i].key, sizeof(PsoKey));
            if (claimed_.count(h))
            {
              ++warm_claimed_; // a draw made it while the files were read
              continue;
            }
            if (warm_.emplace(h, WarmEntry{}).second)
            {
              queue_.emplace_back(h, i);
            }
          }
          claimed_.clear();
          warm_total_.store(uint32_t(queue_.size()));
          warm_state_.store(queue_.empty() ? kWarmDone : kWarmRunning, std::memory_order_release);
        }
        if (queue_.empty())
        {
          return;
        }
        int32_t threads = REXCVAR_GET(pdz_native_pso_warmup_threads);
        // A pipeline library holding under half the list (a first start, the shipped list seeded, or a new GPU /
        // driver): nearly all of it is driver compiles, so half the CPU's threads (2 to 8) instead of a quarter.
        const bool cold = library_entries_loaded_ * 2 < loaded_.size();
        if (threads <= 0)
        {
          const int32_t hw = int32_t(std::thread::hardware_concurrency());
          threads = cold ? std::clamp(hw / 2, 2, 8) : std::clamp(hw / 4, 1, 4);
        }
        threads = std::min<int32_t>(threads, int32_t(queue_.size()));
        warm_start_ = t0;
        warm_threads_ = uint32_t(threads);
        workers_left_.store(uint32_t(threads));
        QueryMemory(device_, warm_private_before_, warm_video_before_);
        REXLOG_INFO("pdz_native: PSO warm-up: {} of {} recipes on {} threads (pipeline library {}{}{}), {:.1f} s "
                    "after process start",
                    queue_.size(), loaded_.size(), threads, lib_state_.load() == kLibReady ? "on" : "off",
                    cold ? fmt::format(", {} PSOs in it: a cold start", library_entries_loaded_) : std::string(),
                    prewarm_ ? "; pdz_native_pso_prewarm: the whole list, kept for the session" : "",
                    SecondsSinceProcessStart());
        for (int32_t t = 1; t < threads; ++t)
        {
          std::thread([this]() { Work(); }).detach();
        }
        Work();
      }

      // A warm-up worker: takes queued recipes in order until none is left.
      void Work()
      {
        pdz::cpu::LowerBackgroundThread("PSO worker"); // pdz_cpu_placement, pdz_background_priority
        for (;;)
        {
          const uint32_t n = next_.fetch_add(1);
          if (n >= queue_.size())
          {
            break;
          }
          const auto [h, index] = queue_[n];
          {
            std::lock_guard lock(warm_mutex_);
            auto it = warm_.find(h);
            if (it == warm_.end() || it->second.state != WarmEntry::kQueued)
            {
              continue; // claimed by a draw
            }
            it->second.state = WarmEntry::kBuilding;
          }
          const Recipe &rc = loaded_[index];
          ID3D12PipelineState *pso = nullptr;
          bool from_library = false;
          Blob hold[3];
          PsoInputs in;
          bool have_inputs = false;
          {
            std::lock_guard lock(recipes_mutex_);
            have_inputs = InputsFromRecipe(rc, blobs_, hold, in);
          }
          Pipeline flags;
          D3D12_GRAPHICS_PIPELINE_STATE_DESC d;
          if (have_inputs && BuildPsoDesc(root_, rc.key, in, point_gs_, d, flags) &&
              FAILED(Create(d, rc.identity, true, &pso, &from_library)))
          {
            pso = nullptr;
          }
          {
            std::lock_guard lock(warm_mutex_);
            auto it = warm_.find(h);
            if (it != warm_.end())
            {
              it->second.state = pso ? WarmEntry::kReady : WarmEntry::kFailed;
              it->second.pso = pso;
              it->second.identity = rc.identity;
              if (pso)
              {
                warm_held_.fetch_add(1);
              }
            }
            else if (pso)
            {
              pso->Release();
            }
          }
          warm_cv_.notify_all();
          ++(pso ? (from_library ? warm_library_ : warm_compiled_) : warm_failed_);
          const uint32_t done = warm_done_.fetch_add(1) + 1;
          const uint32_t total = warm_total_.load();
          if (done < total && (done * 4 / total) != ((done - 1) * 4 / total))
          {
            REXLOG_INFO("pdz_native: PSO warm-up: {} of {} in {:.0f} ms ({} from the pipeline library, {} compiled, "
                        "{} failed)",
                        done, total, MsSince(warm_start_), warm_library_.load(), warm_compiled_.load(),
                        warm_failed_.load());
          }
        }
        if (workers_left_.fetch_sub(1) == 1)
        {
          warm_ms_ = MsSince(warm_start_);
          warm_done_after_s_ = SecondsSinceProcessStart();
          uint64_t private_after = 0, video_after = 0;
          QueryMemory(device_, private_after, video_after);
          const auto mb = [](uint64_t after, uint64_t before) {
            return (double(after) - double(before)) / (1024.0 * 1024.0);
          };
          REXLOG_INFO("pdz_native: PSO warm-up done: {} of {} PSOs ready in {:.0f} ms on {} threads ({} from the "
                      "pipeline library, {} compiled by the driver, {} failed, {} left to the draws that needed them "
                      "first), {:.1f} s after process start; memory over the warm-up (the game's included): private "
                      "{:+.1f} MB, video {:+.1f} MB",
                      warm_library_.load() + warm_compiled_.load(), warm_total_.load(), warm_ms_, warm_threads_,
                      warm_library_.load(), warm_compiled_.load(), warm_failed_.load(), warm_claimed_.load(),
                      warm_done_after_s_, mb(private_after, warm_private_before_),
                      mb(video_after, warm_video_before_));
          warm_state_.store(kWarmDone, std::memory_order_release);
        }
      }

      // Saver thread: pso_recipes.bin (this session's recipes first, then the older ones a session older; dropped
      // past kRecipeMaxAge) and pso.lib (Serialize, with the deferred stores first).
      void Save()
      {
        const auto t0 = std::chrono::steady_clock::now();
        // A snapshot under the lock (recipes and blob references); the file is put together without it, so a draw
        // recording a new PSO meanwhile does not wait for the copy.
        std::vector<std::pair<Recipe, uint16_t>> list;
        std::vector<std::pair<uint64_t, Blob>> blobs;
        {
          std::lock_guard lock(recipes_mutex_);
          new_since_save_.store(0);
          std::unordered_set<uint64_t> seen;
          for (const Recipe &rc : session_)
          {
            if (seen.insert(rc.identity).second)
            {
              list.emplace_back(rc, uint16_t(0));
            }
          }
          for (size_t i = 0; i < loaded_.size(); ++i)
          {
            const uint16_t age = loaded_used_[i] ? 0 : uint16_t(std::min<uint32_t>(loaded_[i].age + 1u, 0xFFFFu));
            // pdz_native_pso_prewarm: the list only grows (the age still orders the warm-up).
            if ((age <= kRecipeMaxAge || prewarm_) && seen.insert(loaded_[i].identity).second)
            {
              list.emplace_back(loaded_[i], age);
            }
          }
          std::unordered_set<uint64_t> id_seen;
          for (const auto &[rc, age] : list)
          {
            for (const uint64_t id : {rc.vs_id, rc.ps_id, rc.variant_id})
            {
              auto it = id ? blobs_.find(id) : blobs_.end();
              if (it != blobs_.end() && id_seen.insert(id).second)
              {
                blobs.emplace_back(id, it->second);
              }
            }
          }
        }
        ByteWriter w;
        const uint32_t recipe_count = uint32_t(list.size()), blob_count = uint32_t(blobs.size());
        size_t blob_bytes = 0;
        w.PutBytes(kRecipeMagic, 8);
        w.Put(kPsoCacheFormat);
        w.Put(uint32_t(sizeof(PsoKey)));
        w.Put(blob_count);
        w.Put(recipe_count);
        for (const auto &[id, blob] : blobs)
        {
          w.Put(id);
          w.Put(uint32_t(blob->size()));
          w.PutBytes(blob->data(), blob->size());
          blob_bytes += blob->size();
        }
        for (const auto &[rc, age] : list)
        {
          w.Put(rc.key);
          w.Put(rc.vs_id);
          w.Put(rc.ps_id);
          w.Put(rc.variant_id);
          w.Put(age);
          w.Put(uint16_t(rc.elements.size()));
          for (const RecipeElement &e : rc.elements)
          {
            w.Put(e.attr);
            w.Put(e.format);
            w.Put(e.stream);
            w.Put(e.offset);
          }
        }
        w.Put(uint64_t(XXH3_64bits(w.bytes.data(), w.bytes.size())));
        const bool recipes_ok = WriteFileAtomically(recipe_path_, w.bytes);
        if (recipes_ok && seed_loaded_)
        {
          // The shipped list's recipes are in pso_recipes.bin now (prewarm keeps them; without it they age out).
          std::error_code ec;
          std::filesystem::remove(seed_path_, ec);
          seed_loaded_ = false;
        }

        std::string library_note = "off";
        if (lib_state_.load(std::memory_order_acquire) == kLibReady)
        {
          std::vector<uint8_t> lib;
          uint32_t entries = 0;
          HRESULT hr = E_FAIL;
          {
            std::unique_lock lock(lib_mutex_); // exclusive: no load / store while it serializes
            DrainPending();
            stores_unsaved_.store(0);
            const SIZE_T size = library_ ? library_->GetSerializedSize() : 0;
            if (size)
            {
              lib.resize(sizeof(LibraryHeader) + size);
              hr = library_->Serialize(lib.data() + sizeof(LibraryHeader), size);
            }
            else
            {
              hr = S_FALSE; // nothing stored yet
            }
            entries = library_entries_loaded_ + library_stores_.load();
          }
          if (hr == S_FALSE)
          {
            library_note = "empty (not written)";
          }
          else if (SUCCEEDED(hr))
          {
            LibraryHeader h = {};
            std::memcpy(h.magic, kLibraryMagic, 8);
            h.format = kPsoCacheFormat;
            h.vendor = adapter_.vendor;
            h.device = adapter_.device;
            h.subsys = adapter_.subsys;
            h.revision = adapter_.revision;
            h.entries = entries;
            h.driver = adapter_.driver;
            h.payload_size = lib.size() - sizeof(LibraryHeader);
            h.payload_hash = XXH3_64bits(lib.data() + sizeof(LibraryHeader), size_t(h.payload_size));
            std::memcpy(lib.data(), &h, sizeof(h));
            library_note = WriteFileAtomically(lib_path_, lib)
                               ? fmt::format("{} KB ({} PSOs)", h.payload_size / 1024, entries)
                               : std::string("NOT written");
          }
          else
          {
            library_note = fmt::format("not serialized ({:#x})", uint32_t(hr));
          }
        }
        stores_unsaved_.store(0); // also with the library off: nothing more to store
        ++saves_;
        REXLOG_INFO("pdz_native: PSO cache saved: {} recipes ({} shader blobs, {} KB){}, pipeline library {}, in {:.0f} "
                    "ms (background) to {}",
                    recipe_count, blob_count, blob_bytes / 1024, recipes_ok ? "" : " NOT written", library_note,
                    MsSince(t0), dir_);
      }

      // Set by Start before any other thread runs.
      ID3D12Device *device_ = nullptr;
      ID3D12Device1 *device1_ = nullptr;
      ID3D12RootSignature *root_ = nullptr;
      uint64_t root_id_ = 0;
      bool point_gs_ = false;
      std::string dir_, lib_path_, recipe_path_;
      AdapterId adapter_; // OpenLibrary (loader), read by the saver afterwards

      // Pipeline library.
      std::shared_mutex lib_mutex_;
      ID3D12PipelineLibrary *library_ = nullptr; // set under lib_mutex_ (exclusive)
      static constexpr uint32_t kNameStripes = 64;
      std::mutex name_mutex_[kNameStripes]; // one PSO name is never loaded / stored by two threads at once
      std::atomic<uint32_t> lib_state_{kLibLoading};
      std::vector<uint8_t> library_file_; // the loaded file: must outlive the library (never freed)
      uint32_t library_entries_loaded_ = 0;
      std::atomic<uint32_t> library_stores_{0}; // stored this session
      std::atomic<uint32_t> stores_unsaved_{0};
      std::mutex pending_mutex_;
      std::vector<std::pair<std::wstring, ID3D12PipelineState *>> pending_; // deferred stores (referenced)

      // Recipes. loaded_ and loaded_by_identity_ are written by the loader only, before recipes_ready_.
      std::mutex recipes_mutex_;
      std::vector<Recipe> loaded_;
      std::unordered_map<uint64_t, uint32_t> loaded_by_identity_;
      std::vector<uint8_t> loaded_used_;               // under recipes_mutex_
      std::vector<Recipe> session_;                    // under recipes_mutex_
      std::unordered_set<uint64_t> session_identities_; // under recipes_mutex_
      std::unordered_map<uint64_t, Blob> blobs_;        // under recipes_mutex_
      std::atomic<bool> recipes_ready_{false};
      std::atomic<uint32_t> new_since_save_{0};
      std::string seed_path_;    // pso_seed.bin: the shipped pipeline list's recipes (pipeline_seed)
      bool seed_loaded_ = false; // it added recipes: removed after the next save
      uint64_t last_new_frame_ = 0; // draw thread
      std::atomic<bool> saving_{false};

      // Warm-up. queue_ is written by the loader before the workers start, read-only afterwards.
      std::mutex warm_mutex_;
      std::condition_variable warm_cv_;
      std::unordered_map<uint64_t, WarmEntry> warm_; // by key hash (PipelineCache's map key)
      std::unordered_set<uint64_t> claimed_;         // keys the draws made while the loader read the files
      std::vector<std::pair<uint64_t, uint32_t>> queue_; // (key hash, loaded_ index), in warm-up order
      std::atomic<uint32_t> next_{0};
      std::atomic<uint32_t> workers_left_{0};
      std::atomic<uint32_t> warm_state_{kWarmOff};
      std::chrono::steady_clock::time_point warm_start_;
      uint32_t warm_threads_ = 0;
      bool prewarm_ = false;                // pdz_native_pso_prewarm (Start)
      std::atomic<uint32_t> warm_held_{0};  // PSOs the warm map holds a reference to (memory log pso_warm)
      double warm_ms_ = 0.0, warm_done_after_s_ = 0.0; // written by the last worker before kWarmDone
      uint64_t warm_private_before_ = 0, warm_video_before_ = 0;

      // Counters (StatsText).
      std::atomic<uint32_t> warm_total_{0}, warm_done_{0}, warm_library_{0}, warm_compiled_{0}, warm_failed_{0};
      std::atomic<uint32_t> warm_claimed_{0}, taken_{0}, waits_{0}, stale_{0};
      std::atomic<uint32_t> loaded_count_{0}, session_count_{0}, saves_{0};
    };
  } // namespace

  // ---------------------------------------------------------------------------
  // PipelineCache
  // ---------------------------------------------------------------------------
  struct PipelineCache::Impl
  {
    std::unordered_map<uint64_t, Pipeline> pipelines;
    ID3DBlob *point_gs = nullptr;
    bool rect_pso_failed_logged = false;
    ID3D12DescriptorHeap *null_srv_heap = nullptr;
    ID3D12DescriptorHeap *null_sampler_heap = nullptr;
    bool point_gs_failed_logged = false;
    bool point_pso_failed_logged = false;
    bool clip_gs_failed = false; // a PSO with the user-clip-plane GS failed: planes ignored from then on (logged)
    bool clip_gs_logged = false;
    // pdz_native_pso_cache / pdz_native_pso_warmup (nullptr = off) and the identity of the root signature (XXH3 of its
    // serialized form, part of PsoDescIdentity).
    PsoStore *store = nullptr;
    uint64_t root_id = 0;
    // PSOs made at a draw's first use (not ready from the warm-up): NoteFirstUse, the draws line.
    uint32_t first_use = 0, first_use_library = 0, first_use_waited = 0, first_use_logged = 0;
    double first_use_ms = 0.0, first_use_max_ms = 0.0;
    // ... by why the warm-up had none (the summary line): count and ms.
    struct MissCount
    {
      uint32_t count = 0;
      double ms = 0.0;
      void Add(double v)
      {
        ++count;
        ms += v;
      }
    };
    MissCount miss_not_listed, miss_not_ready, miss_waited, miss_other;
    uint32_t summary_logged_first_use = 0; // first_use at the last periodic summary line

    bool CreateRootSignature(NativeDevice &dev, ID3D12RootSignature **out)
    {
      // space0 Texture2D, space1 Texture3D, space2 TextureCube (the fork's declarations), space3 Texture2DArray (the
      // stacked / 2D view of a tfetch3D slot in shaders patched by pdz_native_stacked_array, native_hlslfix.h; the
      // others never declare it).
      D3D12_DESCRIPTOR_RANGE srv_ranges[4] = {};
      for (uint32_t space = 0; space < 4; ++space)
      {
        srv_ranges[space].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srv_ranges[space].NumDescriptors = 32;
        srv_ranges[space].BaseShaderRegister = 0;
        srv_ranges[space].RegisterSpace = space;
        srv_ranges[space].OffsetInDescriptorsFromTableStart = space * 32;
      }
      D3D12_DESCRIPTOR_RANGE sampler_range = {};
      sampler_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
      sampler_range.NumDescriptors = 32;
      sampler_range.BaseShaderRegister = 0;
      sampler_range.RegisterSpace = 0;
      sampler_range.OffsetInDescriptorsFromTableStart = 0;

      D3D12_ROOT_PARAMETER params[kRootParamCount] = {};
      auto cbv = [&](uint32_t index, uint32_t reg, D3D12_SHADER_VISIBILITY vis) {
        params[index].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[index].Descriptor.ShaderRegister = reg;
        params[index].Descriptor.RegisterSpace = 0;
        params[index].ShaderVisibility = vis;
      };
      cbv(kRootCbvFloatVs, 0, D3D12_SHADER_VISIBILITY_VERTEX);
      cbv(kRootCbvFloatPs, 0, D3D12_SHADER_VISIBILITY_PIXEL);
      cbv(kRootCbvBoolLoop, 1, D3D12_SHADER_VISIBILITY_ALL);
      cbv(kRootCbvSystem, 2, D3D12_SHADER_VISIBILITY_ALL);
      params[kRootSrvTable].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
      params[kRootSrvTable].DescriptorTable.NumDescriptorRanges = 4;
      params[kRootSrvTable].DescriptorTable.pDescriptorRanges = srv_ranges;
      params[kRootSrvTable].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
      params[kRootSamplerVs].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
      params[kRootSamplerVs].DescriptorTable.NumDescriptorRanges = 1;
      params[kRootSamplerVs].DescriptorTable.pDescriptorRanges = &sampler_range;
      params[kRootSamplerVs].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
      params[kRootSamplerPs].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
      params[kRootSamplerPs].DescriptorTable.NumDescriptorRanges = 1;
      params[kRootSamplerPs].DescriptorTable.pDescriptorRanges = &sampler_range;
      params[kRootSamplerPs].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
      params[kRootConstDepth].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
      params[kRootConstDepth].Constants.ShaderRegister = 3;
      params[kRootConstDepth].Constants.RegisterSpace = 0;
      params[kRootConstDepth].Constants.Num32BitValues = kRootConstDepthCount;
      params[kRootConstDepth].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
      // The 7e3 blend variant's rasterizer-ordered view of render target 0 and its constants (native_blend7e3.h). Only
      // that variant declares u0 / b4; the other draws leave both unset.
      D3D12_DESCRIPTOR_RANGE uav_range = {};
      uav_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
      uav_range.NumDescriptors = 1;
      uav_range.BaseShaderRegister = 0;
      uav_range.RegisterSpace = 0;
      uav_range.OffsetInDescriptorsFromTableStart = 0;
      params[kRootUavEdram7e3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
      params[kRootUavEdram7e3].DescriptorTable.NumDescriptorRanges = 1;
      params[kRootUavEdram7e3].DescriptorTable.pDescriptorRanges = &uav_range;
      params[kRootUavEdram7e3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
      params[kRootConstBlend7e3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
      params[kRootConstBlend7e3].Constants.ShaderRegister = 4;
      params[kRootConstBlend7e3].Constants.RegisterSpace = 0;
      params[kRootConstBlend7e3].Constants.Num32BitValues = kRootConstBlend7e3Count;
      params[kRootConstBlend7e3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

      D3D12_ROOT_SIGNATURE_DESC desc = {};
      desc.NumParameters = kRootParamCount;
      desc.pParameters = params;
      desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
                   D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
                   D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS;

      NativeDevice::PFN_SerializeRootSignature serialize = dev.serialize_root_signature();
      if (!serialize)
      {
        REXLOG_ERROR("pdz_native: D3D12SerializeRootSignature unavailable");
        return false;
      }
      ID3DBlob *blob = nullptr;
      ID3DBlob *error = nullptr;
      HRESULT hr = serialize(&desc, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, &error);
      if (FAILED(hr))
      {
        REXLOG_ERROR("pdz_native: root signature serialisation failed ({:#x}): {}", uint32_t(hr),
                     error ? static_cast<const char *>(error->GetBufferPointer()) : "");
        if (error) error->Release();
        return false;
      }
      if (error) error->Release();
      hr = dev.device()->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                             IID_PPV_ARGS(out));
      root_id = XXH3_64bits(blob->GetBufferPointer(), blob->GetBufferSize());
      blob->Release();
      if (FAILED(hr))
      {
        REXLOG_ERROR("pdz_native: CreateRootSignature failed ({:#x})", uint32_t(hr));
        return false;
      }
      return true;
    }

    void CompilePointGs(NativeDevice &dev)
    {
      NativeDevice::PFN_Compile compile = dev.d3d_compile();
      if (!compile)
      {
        REXLOG_WARN("pdz_native: D3DCompile unavailable: POINTLIST draws will be skipped");
        return;
      }
      ID3DBlob *blob = nullptr;
      ID3DBlob *error = nullptr;
      const HRESULT hr = compile(kPointGsSource, sizeof(kPointGsSource) - 1, "pdz_point_gs", nullptr, nullptr,
                                 "main", "gs_5_1", 0, 0, &blob, &error);
      if (FAILED(hr) || !blob)
      {
        REXLOG_WARN("pdz_native: point geometry shader failed to compile ({:#x}): {}", uint32_t(hr),
                    error ? static_cast<const char *>(error->GetBufferPointer()) : "");
        if (error) error->Release();
        if (blob) blob->Release();
        return;
      }
      if (error) error->Release();
      point_gs = blob;
    }

    bool CreateNullBlocks(NativeDevice &dev, D3D12_CPU_DESCRIPTOR_HANDLE &srv_block,
                          D3D12_CPU_DESCRIPTOR_HANDLE &sampler_block)
    {
      ID3D12Device *device = dev.device();
      D3D12_DESCRIPTOR_HEAP_DESC hd = {};
      hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
      hd.NumDescriptors = kSrvTableSize;
      hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
      if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&null_srv_heap))))
      {
        return false;
      }
      hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
      hd.NumDescriptors = kSamplerTableSize;
      if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&null_sampler_heap))))
      {
        return false;
      }
      srv_block = null_srv_heap->GetCPUDescriptorHandleForHeapStart();
      sampler_block = null_sampler_heap->GetCPUDescriptorHandleForHeapStart();
      const uint32_t srv_size = dev.srv_descriptor_size();
      const uint32_t sampler_size = dev.sampler_descriptor_size();
      for (uint32_t i = 0; i < kSrvTableSize; ++i)
      {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        const uint32_t space = i / 32;
        if (space == 0)
        {
          sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
          sd.Texture2D.MipLevels = 1;
        }
        else if (space == 1)
        {
          sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
          sd.Texture3D.MipLevels = 1;
        }
        else if (space == 2)
        {
          sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
          sd.TextureCube.MipLevels = 1;
        }
        else
        {
          // A null 2D array reads 0 and its GetDimensions returns 0: the patched tfetch3D's "not stacked" test.
          sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
          sd.Texture2DArray.MipLevels = 1;
          sd.Texture2DArray.ArraySize = 1;
        }
        D3D12_CPU_DESCRIPTOR_HANDLE h = srv_block;
        h.ptr += SIZE_T(i) * srv_size;
        device->CreateShaderResourceView(nullptr, &sd, h);
      }
      D3D12_SAMPLER_DESC sampler = {};
      sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
      sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
      sampler.MaxLOD = D3D12_FLOAT32_MAX;
      for (uint32_t i = 0; i < kSamplerTableSize; ++i)
      {
        D3D12_CPU_DESCRIPTOR_HANDLE h = sampler_block;
        h.ptr += SIZE_T(i) * sampler_size;
        device->CreateSampler(&sampler, h);
      }
      return true;
    }

    // The PSO of a key at a draw's first Lookup of it. With pdz_native_pso_cache: taken from the warm-up when that
    // made the same one, else loaded from the pipeline library or created by the driver, and recorded for the next
    // sessions. Off: CreateGraphicsPipelineState, as before. `origin` / `waited_ms`: how (the first-use log).
    bool CreatePso(NativeDevice &dev, ID3D12RootSignature *rs, const PsoKey &key, uint64_t key_hash,
                   const NativeShader *vs, const NativeShader *ps, const void *depth_dxil, size_t depth_dxil_size,
                   const void *blend_dxil, size_t blend_dxil_size, uint64_t frame, Pipeline &out, PsoOrigin &origin,
                   double &waited_ms, PsoStore::Miss &miss)
    {
      miss = PsoStore::Miss::kNotListed;
      const pdz::timing::PhaseScope phase(pdz::timing::Phase::kPsoCreate); // pdz_frame_timing_phases
      origin = PsoOrigin::kCompiled;
      waited_ms = 0.0;
      // Input layout from the VS sidecar: one element per ATTR<n>, the stream = input-assembler slot.
      PsoInputs in;
      in.vs = vs->dxil.data();
      in.vs_size = vs->dxil.size();
      in.ps = ps->dxil.data();
      in.ps_size = ps->dxil.size();
      in.variant = depth_dxil ? depth_dxil : blend_dxil;
      in.variant_size = depth_dxil ? depth_dxil_size : blend_dxil_size;
      in.elements.reserve(vs->inputs.size());
      for (const ShaderVertexInput &v : vs->inputs)
      {
        in.elements.push_back(InputElement(v.attr, v.format, v.stream, v.offset));
      }
      D3D12_GRAPHICS_PIPELINE_STATE_DESC d;
      if (!BuildPsoDesc(rs, key, in, point_gs != nullptr, d, out))
      {
        return false;
      }
      HRESULT hr = E_FAIL;
      if (store)
      {
        in.vs_id = BlobId(in.vs, in.vs_size);
        in.ps_id = BlobId(in.ps, in.ps_size);
        in.variant_id = BlobId(in.variant, in.variant_size);
        const uint64_t identity = PsoDescIdentity(d, in, root_id);
        if (store->TakeWarm(key_hash, identity, out, waited_ms, miss))
        {
          origin = waited_ms > 0.0 ? PsoOrigin::kWaited : PsoOrigin::kWarm;
          store->MarkUsed(identity);
          return true;
        }
        pdz::timing::CountEvent(pdz::timing::Event::kPsoCreate); // pdz_frame_timing_spike_ms
        bool from_library = false;
        hr = store->Create(d, identity, false, &out.pso, &from_library);
        if (SUCCEEDED(hr))
        {
          origin = from_library ? PsoOrigin::kLibrary : PsoOrigin::kCompiled;
          store->Record(key, in, identity, frame);
        }
      }
      else
      {
        pdz::timing::CountEvent(pdz::timing::Event::kPsoCreate); // pdz_frame_timing_spike_ms
        hr = dev.device()->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&out.pso));
      }
      if (FAILED(hr))
      {
        const bool points = (key.raster >> 4) & 1;
        const bool rects = (key.raster >> 6) & 1;
        REXLOG_WARN("pdz_native: PSO creation failed {} ({:#x}): vs {:016x} ps {:016x} prim type {} rts {} "
                    "({:#x} {:#x} {:#x} {:#x}) ds {:#x} depthcontrol {:#x} blend0 {:#x} raster {:#x}{}{}{} strip cut {}",
                    HResultName(hr), uint32_t(hr), key.vs_hash, key.ps_hash, key.topology_type, key.rt_count,
                    key.rt[0], key.rt[1], key.rt[2], key.rt[3], key.ds, key.depthcontrol, key.blendcontrol[0],
                    key.raster, points ? " [point expansion GS]" : (rects ? " [rectangle-list GS]" : ""),
                    key.depth_variant ? " [SV_Depth 20e4 variant]" : "",
                    key.blend7e3_variant == 2 ? " [7e3 ROV blend variant]"
                                              : (key.blend7e3_variant == 1 ? " [7e3 alpha-clamp variant]" : ""),
                    key.strip_cut);
        out.pso = nullptr;
        return false;
      }
      return true;
    }

    // A PSO made at a draw's first Lookup of its key, i.e. not ready from the warm-up beforehand: counted for the
    // draws line, and with pdz_native_first_use_log the first kFirstUseLogLines logged with their cost.
    void NoteFirstUse(const PsoKey &key, PsoOrigin origin, double ms, PsoStore::Miss miss)
    {
      // The summary's split: waited for a worker, not ready yet (claimed / warm-up still loading), not in the list
      // (or no warm-up / cache), other (stale, failed).
      if (origin == PsoOrigin::kWaited)
      {
        miss_waited.Add(ms);
      }
      else if (miss == PsoStore::Miss::kNotReady)
      {
        miss_not_ready.Add(ms);
      }
      else if (miss == PsoStore::Miss::kNotListed)
      {
        miss_not_listed.Add(ms);
      }
      else
      {
        miss_other.Add(ms);
      }
      ++first_use;
      first_use_ms += ms;
      first_use_max_ms = std::max(first_use_max_ms, ms);
      if (origin == PsoOrigin::kLibrary)
      {
        ++first_use_library;
      }
      if (origin == PsoOrigin::kWaited)
      {
        ++first_use_waited;
      }
      // pdz_native_pso_prewarm: a miss is the exception once the list is filled, so up to 8 x as many lines.
      const uint32_t lines = store && store->prewarm() ? 8 * kFirstUseLogLines : kFirstUseLogLines;
      if (!REXCVAR_GET(pdz_native_first_use_log) || first_use_logged >= lines)
      {
        return;
      }
      ++first_use_logged;
      const char *how = origin == PsoOrigin::kLibrary  ? "loaded from the pipeline library"
                        : origin == PsoOrigin::kWaited ? "waited for the warm-up to make it"
                        : !store                       ? "compiled by the driver"
                        : miss == PsoStore::Miss::kNotReady ? "compiled by the driver, in the list but not made yet"
                        : store->warm_running()        ? "compiled by the driver, the warm-up still running"
                                                       : "compiled by the driver, not in the PSO cache";
      const bool points = (key.raster >> 4) & 1;
      const bool rects = (key.raster >> 6) & 1;
      REXLOG_INFO("pdz_native: PSO first use {}: {:.1f} ms, {}: vs {:016x} ps {:016x} topology type {} rts {} "
                  "({:#x}) ds {:#x}{}{}{}{}",
                  first_use_logged, ms, how, key.vs_hash, key.ps_hash, key.topology_type, key.rt_count, key.rt[0],
                  key.ds, points ? " [point expansion GS]" : (rects ? " [rectangle-list GS]" : ""),
                  key.depth_variant ? " [SV_Depth 20e4 variant]" : "",
                  key.blend7e3_variant == 2 ? " [7e3 ROV blend variant]"
                                            : (key.blend7e3_variant == 1 ? " [7e3 alpha-clamp variant]" : ""),
                  first_use_logged == lines ? "; further first-use PSOs only in the periodic draws line" : "");
    }

    // One line per session at exit (and periodically while it changes): the list, the warm-up and the misses during
    // play (PSOs a draw made on demand), with the live PSO counts for the memory question.
    void LogSummary(const char *why, uint32_t pso_live)
    {
      const auto part = [](const MissCount &m) { return fmt::format("{} ({:.0f} ms)", m.count, m.ms); };
      REXLOG_INFO("pdz_native: PSO summary ({}): {} | misses during play {} in {:.0f} ms (max {:.1f} ms): not in the "
                  "list {}, not made yet {}, waited for a worker {}, other {}; {} of them from the pipeline library | "
                  "PSOs: draw map {}, held by the warm-up {}",
                  why, store ? store->SummaryText() : std::string("PSO cache off"), first_use, first_use_ms,
                  first_use_max_ms, part(miss_not_listed), part(miss_not_ready), part(miss_waited), part(miss_other),
                  first_use_library, pso_live, store ? store->warm_held() : 0u);
      summary_logged_first_use = first_use;
    }
  };

  PipelineCache &PipelineCache::Get()
  {
    static PipelineCache instance;
    return instance;
  }

  bool PipelineCache::Init()
  {
    if (init_attempted_)
    {
      return ready_;
    }
    init_attempted_ = true;
    NativeDevice *dev = NativeDevice::Get();
    if (!dev || !dev->device())
    {
      REXLOG_ERROR("pdz_native: pipeline cache: no D3D12 device");
      return false;
    }
    impl_ = new Impl();
    if (!impl_->CreateRootSignature(*dev, &root_signature_))
    {
      return false;
    }
    if (!impl_->CreateNullBlocks(*dev, null_srv_block_, null_sampler_block_))
    {
      REXLOG_ERROR("pdz_native: pipeline cache: null descriptor heaps failed");
      return false;
    }
    impl_->CompilePointGs(*dev);
    impl_->pipelines.reserve(1024);
    ready_ = true;
    REXLOG_INFO("pdz_native: pipeline cache ready (root signature: 4 CBVs, 128-entry SRV table, 2 x 32 sampler "
                "tables, 4 depth constants, 7e3 blend UAV + 8 constants; point GS {}; rectangle-list GS {}; depth20e4 "
                "mode {}; 7e3 blend mode {})",
                impl_->point_gs ? "compiled" : "UNAVAILABLE", REXCVAR_GET(pdz_native_rectlist) ? "on" : "off",
                depth20e4::CurrentMode(), blend7e3::CurrentMode());
    if (REXCVAR_GET(pdz_native_pso_cache))
    {
      // Reads the native_cache files and warms up on its own threads; the draws start at once (PsoStore).
      impl_->store = new PsoStore();
      impl_->store->Start(dev->device(), root_signature_, impl_->root_id, impl_->point_gs != nullptr);
      if (REXCVAR_GET(pdz_native_pso_warmup_wait) && impl_->store->prewarm())
      {
        const double ms = impl_->store->WaitDone(120000.0);
        REXLOG_INFO("pdz_native: pdz_native_pso_warmup_wait: the first native draw waited {:.0f} ms for the PSO "
                    "warm-up ({} of {} made)",
                    ms, impl_->store->warm_done(), impl_->store->warm_total());
      }
      std::atexit([]() {
        PipelineCache &p = PipelineCache::Get();
        if (p.impl_ && p.impl_->store)
        {
          p.impl_->LogSummary("exit", p.pso_live());
        }
      });
    }
    depth20e4::PreloadCompiler(); // pdz_native_dxc_preload: the variants' compiler on a worker now, not mid-level
    return true;
  }

  void PipelineCache::WaitForWarmupAtLevelStart(const char *level)
  {
    const int32_t max_ms = REXCVAR_GET(pdz_native_pso_level_wait_ms);
    PsoStore *store = ready_ && impl_ ? impl_->store : nullptr;
    if (max_ms <= 0 || !store || !store->prewarm() || !store->warm_running())
    {
      return;
    }
    const uint32_t before = store->warm_done(), total = store->warm_total();
    const double ms = store->WaitDone(double(max_ms));
    REXLOG_INFO("pdz_native: level start '{}': waited {:.0f} ms for the PSO warm-up ({} of {} made before, {} after{}): "
                "pdz_native_pso_level_wait_ms",
                level ? level : "", ms, before, total, store->warm_done(),
                store->warm_running() ? "; still running, the rest made in play" : "");
  }

  void PipelineCache::FirstUseTotals(uint32_t &created, uint32_t &built_at_draw, double &built_ms) const
  {
    created = pso_count_;
    built_at_draw = impl_ ? impl_->first_use : 0;
    built_ms = impl_ ? impl_->first_use_ms : 0.0;
  }

  std::string PipelineCache::CacheStatsText() const
  {
    if (!impl_ || (!impl_->store && !REXCVAR_GET(pdz_native_first_use_log)))
    {
      return std::string();
    }
    std::string text = fmt::format("; first use {} in {:.0f} ms (max {:.1f} ms; {} from the library, {} waited for the "
                                   "warm-up)",
                                   impl_->first_use, impl_->first_use_ms, impl_->first_use_max_ms,
                                   impl_->first_use_library, impl_->first_use_waited);
    if (impl_->store)
    {
      text += impl_->store->StatsText();
    }
    return text;
  }

  uint32_t PipelineCache::pso_keys() const { return impl_ ? uint32_t(impl_->pipelines.size()) : 0u; }

  uint32_t PipelineCache::pso_warm_held() const
  {
    return impl_ && impl_->store ? impl_->store->warm_held() : 0u;
  }

  // The 7e3 blend variant of a draw (native_blend7e3.h): the mode it gets and the DXIL; 0 = none. Mode 2 without
  // its variant falls back to mode 1.
  static uint32_t BlendVariantFor(const NativeShader *ps, const RenderState &rs, const TargetFormats &tf,
                                  const std::vector<uint8_t> **dxil_out)
  {
    *dxil_out = nullptr;
    uint32_t mode = blend7e3::Wanted(ps, rs, tf);
    if (mode == blend7e3::kRov)
    {
      if (const std::vector<uint8_t> *v = blend7e3::Variant(ps, blend7e3::kRov); v && !v->empty())
      {
        *dxil_out = v;
        return mode;
      }
      blend7e3::CountVariantFallback();
      mode = blend7e3::kAlphaClamp;
    }
    if (mode == blend7e3::kAlphaClamp)
    {
      if (const std::vector<uint8_t> *v = blend7e3::Variant(ps, blend7e3::kAlphaClamp); v && !v->empty())
      {
        *dxil_out = v;
        return mode;
      }
    }
    return blend7e3::kOff;
  }

  void PipelineCache::OnFrame(uint64_t frame)
  {
    constexpr uint64_t kMaintainFrames = 600;
    constexpr uint64_t kMinIdleFrames = 1800; // also far past the frames in flight: a released PSO is bound by none
    frame_ = frame;
    if (impl_ && impl_->store)
    {
      impl_->store->MaybeSave(frame); // pdz_native_pso_cache: a few atomic loads unless a save is due
      // The summary line once a minute while misses keep coming (at 60 fps), and at exit (Init's atexit).
      if (frame % 3600 == 0 && frame != 0 && impl_->first_use != impl_->summary_logged_first_use)
      {
        impl_->LogSummary("periodic", pso_live());
      }
    }
    if (!impl_ || frame - last_maintenance_ < kMaintainFrames)
    {
      return;
    }
    last_maintenance_ = frame;
    const int32_t cap = REXCVAR_GET(pdz_native_pso_cap);
    if (cap <= 0 || pso_live() <= uint32_t(cap))
    {
      return;
    }
    NativeDevice *dev = NativeDevice::Existing();
    if (!dev)
    {
      return;
    }
    // Least recently used first; failures (pso == nullptr) stay: they keep a failing key from being retried.
    std::vector<std::pair<uint64_t, uint64_t>> idle; // (last used frame, key)
    for (const auto &[key, p] : impl_->pipelines)
    {
      if (p.pso && p.last_used_frame + kMinIdleFrames <= frame)
      {
        idle.emplace_back(p.last_used_frame, key);
      }
    }
    std::sort(idle.begin(), idle.end());
    const uint32_t target = uint32_t(cap) - uint32_t(cap) / 4;
    const uint32_t before = pso_live();
    uint64_t newest_released = 0;
    for (const auto &[last_used, key] : idle)
    {
      if (pso_live() <= target)
      {
        break;
      }
      auto it = impl_->pipelines.find(key);
      if (it == impl_->pipelines.end())
      {
        continue;
      }
      dev->ReleaseLater(it->second.pso); // a frame in flight may still hold it (none can: 1800 frames idle)
      impl_->pipelines.erase(it);
      ++pso_evicted_;
      newest_released = last_used;
    }
    if (before != pso_live() && evict_logged_ < 16)
    {
      ++evict_logged_;
      REXLOG_INFO("pdz_native: PSO cache over pdz_native_pso_cap ({} live, cap {}): released {} PSOs unused for {}+ "
                  "frames (the newest of them last used in frame {}, now {}); {} live, {} released in total",
                  before, cap, before - pso_live(), kMinIdleFrames, newest_released, frame, pso_live(), pso_evicted_);
    }
  }

  bool PipelineCache::ApplyShaderDepthBias(const NativeShader *ps, const RenderState &rs, const TargetFormats &tf,
                                           bool polygonal, const void **dxil_out, size_t *dxil_size_out,
                                           uint32_t *edge_out)
  {
    if (dxil_out) *dxil_out = nullptr;
    if (dxil_size_out) *dxil_size_out = 0;
    if (edge_out) *edge_out = 0;
    if (!depth20e4::Wanted(ps, rs, tf, polygonal))
    {
      return false;
    }
    const uint32_t edge = depth20e4::EdgeFor(rs, polygonal); // 0 unless pdz_native_depth20e4_edge
    // Built once per PS (and edge); nullptr = failed (counted).
    const std::vector<uint8_t> *variant = depth20e4::Variant(ps, edge);
    if (!variant || variant->empty())
    {
      return false;
    }
    if (dxil_out) *dxil_out = variant->data();
    if (dxil_size_out) *dxil_size_out = variant->size();
    if (edge_out) *edge_out = edge;
    return true;
  }

  const Pipeline *PipelineCache::Lookup(const NativeShader *vs, const NativeShader *ps, uint32_t prim,
                                        const RenderState &rs, const TargetFormats &tf,
                                        D3D12_INDEX_BUFFER_STRIP_CUT_VALUE strip_cut)
  {
    if (!ready_ || !vs || !ps)
    {
      return nullptr;
    }
    const Topology topo = TopologyFor(prim);
    if (!topo.supported)
    {
      return nullptr;
    }

    PsoKey key;
    std::memset(&key, 0, sizeof(key));
    key.vs_hash = vs->hash;
    key.ps_hash = ps->hash;
    key.layout_hash = vs->input_layout_hash;
    key.depthcontrol = rs.depthcontrol;
    if (REXCVAR_GET(pdz_native_debug_ztest_off) && !(rs.depthcontrol & 4u))
    {
      key.depthcontrol &= ~2u; // diagnostic: no depth test for draws that do not write depth
    }
    for (uint32_t i = 0; i < 4; ++i)
    {
      key.blendcontrol[i] = rs.blendcontrol[i];
      key.rt[i] = uint32_t(tf.rt[i]);
    }
    key.color_mask = rs.color_mask;
    if (REXCVAR_GET(pdz_native_mask_unwritten_targets) && !ps->dxil.empty())
    {
      // draw_util::GetNormalizedColorMask: "If the shader doesn't write to a render target, it shouldn't be written
      // to". D3D12 would store undefined values for an RTV whose SV_Target the PS does not declare.
      for (uint32_t i = 0; i < 4; ++i)
      {
        if (!((ps->color_outputs >> i) & 1u))
        {
          key.color_mask &= ~(0xFu << (4 * i));
        }
      }
    }
    reg::RB_STENCILREFMASK srm;
    srm.value = rs.stencilrefmask;
    key.stencil_masks = uint32_t(srm.stencilmask) | (uint32_t(srm.stencilwritemask) << 8);

    // Rasterizer: cull / face / fill as the SDK does (per-side fill and bias follow the surviving side).
    reg::PA_SU_SC_MODE_CNTL sc;
    sc.value = rs.su_sc_mode_cntl;
    const bool polygonal = topo.type == D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    uint32_t cull = 0;
    bool front_ccw = true;
    bool wireframe = false;
    float polygon_offset = 0.0f, polygon_scale = 0.0f;
    if (polygonal)
    {
      front_ccw = sc.face == 0;
      const bool cull_front = sc.cull_front != 0, cull_back = sc.cull_back != 0;
      cull = cull_front ? 1u : (cull_back ? 2u : 0u);
      if (sc.poly_mode == xenos::PolygonModeEnable::kDualMode)
      {
        if (!cull_front && sc.polymode_front_ptype != xenos::PolygonType::kTriangles) wireframe = true;
        if (!cull_back && sc.polymode_back_ptype != xenos::PolygonType::kTriangles) wireframe = true;
      }
      // Polygon offset of the side that is drawn (front unless front faces are culled).
      const bool use_back = cull_front && !cull_back;
      const bool enabled = use_back ? sc.poly_offset_back_enable != 0 : sc.poly_offset_front_enable != 0;
      if (enabled)
      {
        polygon_offset = use_back ? rs.poly_offset_back_offset : rs.poly_offset_front_offset;
        polygon_scale = use_back ? rs.poly_offset_back_scale : rs.poly_offset_front_scale;
      }
    }
    key.raster = cull | (front_ccw ? 4u : 0u) | (wireframe ? 8u : 0u) | (topo.points ? 16u : 0u) | (topo.rects ? 64u : 0u);
    // User clip planes: triangle topologies without another GS (points / rectangle lists / lines keep ignoring them).
    if (!impl_->clip_gs_failed && UserClipPlaneMask(rs) != 0 && !topo.points && !topo.rects &&
        topo.type == D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE)
    {
      key.raster |= 128u;
    }
    {
      // PA_CL_CLIP_CNTL clip_disable: no depth clip (the SDK's pipeline_cache depth_clip).
      rex::graphics::reg::PA_CL_CLIP_CNTL cc;
      cc.value = rs.clip_cntl;
      if (cc.clip_disable)
      {
        key.raster |= 32u;
      }
    }
    if (polygon_offset != 0.0f || polygon_scale != 0.0f)
    {
      key.depth_bias = IntegerPolygonOffset(tf.depth_float24, polygon_offset);
      // 1/16 subpixel unit (xenos::kPolygonOffsetScaleSubpixelUnit), scaled with the host resolution.
      key.slope_bias = polygon_scale * (1.0f / 16.0f) * float(tf.resolution_scale ? tf.resolution_scale : 1);
    }
    key.topology_type = uint32_t(topo.type);
    // Primitive reset (native_draw.cpp picks the value for the bound index format): host strips only; expanded
    // fans and lists carry no reset index on the host.
    if (topo.topology == D3D_PRIMITIVE_TOPOLOGY_LINESTRIP || topo.topology == D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP)
    {
      key.strip_cut = uint32_t(strip_cut);
    }
    key.rt_count = tf.rt_count;
    key.ds = uint32_t(tf.ds);
    // SV_Depth 20e4 variant (pdz_native_depth20e4): the bias moves into the shader (root constants per draw), so
    // the fixed-function bias leaves the key and draws with different offsets share the PSO.
    const void *depth_dxil = nullptr;
    size_t depth_dxil_size = 0;
    uint32_t depth_edge = 0;
    if (ApplyShaderDepthBias(ps, rs, tf, polygonal, &depth_dxil, &depth_dxil_size, &depth_edge))
    {
      key.depth_variant = 1 + depth_edge;
      key.depth_bias = 0;
      key.slope_bias = 0.0f;
    }
    // 7e3 blend variant (pdz_native_7e3_blend, native_blend7e3.h): never with the depth one (mode 2's early depth
    // excludes SV_Depth; the depth variant is off by default).
    const std::vector<uint8_t> *blend_dxil = nullptr;
    if (!key.depth_variant)
    {
      key.blend7e3_variant = BlendVariantFor(ps, rs, tf, &blend_dxil);
    }

    const uint64_t h = XXH3_64bits(&key, sizeof(key));
    auto it = impl_->pipelines.find(h);
    if (it != impl_->pipelines.end())
    {
      it->second.last_used_frame = frame_; // pdz_native_pso_cap LRU
      return it->second.pso ? &it->second : nullptr;
    }
    Pipeline &p = impl_->pipelines[h];
    p.topology = topo;
    p.last_used_frame = frame_;
    NativeDevice *dev = NativeDevice::Get();
    if (topo.points && !impl_->point_gs)
    {
      if (!impl_->point_gs_failed_logged)
      {
        impl_->point_gs_failed_logged = true;
        REXLOG_WARN("pdz_native: POINTLIST draw without the point geometry shader: skipped (and every later one)");
      }
      ++pso_failures_;
      return nullptr;
    }
    PsoOrigin origin = PsoOrigin::kCompiled;
    PsoStore::Miss miss = PsoStore::Miss::kNotListed;
    double waited_ms = 0.0;
    const auto created_at = std::chrono::steady_clock::now();
    if (!dev || !impl_->CreatePso(*dev, root_signature_, key, h, vs, ps, depth_dxil, depth_dxil_size,
                                  blend_dxil ? blend_dxil->data() : nullptr, blend_dxil ? blend_dxil->size() : 0, frame_,
                                  p, origin, waited_ms, miss))
    {
      ++pso_failures_;
      if (dev && key.blend7e3_variant)
      {
        // The 7e3 blend variant's PSO failed (logged above): retire it for this PS; the retry takes the next mode down
        // (2 -> 1 -> fixed function). The failed key stays cached as a failure.
        blend7e3::MarkFailed(ps, key.blend7e3_variant, "CreateGraphicsPipelineState failed with the variant");
        return Lookup(vs, ps, prim, rs, tf, strip_cut);
      }
      if (dev && key.depth_variant)
      {
        // The SV_Depth variant's PSO failed (logged above): retire the variant of this PS and draw with the
        // fixed-function bias instead. The failed key stays cached as a failure; the retry builds the plain key.
        depth20e4::MarkFailed(ps, "CreateGraphicsPipelineState failed with the variant", key.depth_variant - 1);
        return Lookup(vs, ps, prim, rs, tf, strip_cut);
      }
      if (dev && (key.raster & 128u))
      {
        // The user-clip-plane GS PSO failed (logged above): ignore the planes from now on, as before the GS.
        impl_->clip_gs_failed = true;
        REXLOG_WARN("pdz_native: user-clip-plane geometry shader PSO failed: clip planes ignored from now on");
        return Lookup(vs, ps, prim, rs, tf);
      }
      if (topo.rects && !impl_->rect_pso_failed_logged)
      {
        impl_->rect_pso_failed_logged = true;
        REXLOG_WARN("pdz_native: RECTLIST pipeline failed: rectangle-list draws are skipped (counted as unsupported)");
      }
      return nullptr;
    }
    ++pso_count_;
    p.creations = pso_count_;
    if (origin != PsoOrigin::kWarm)
    {
      impl_->NoteFirstUse(key, origin, MsSince(created_at), miss); // pdz_native_first_use_log
    }
    if ((key.raster & 128u) && !impl_->clip_gs_logged)
    {
      // Cold (PSO creation): confirms in the log that the guest enabled user clip planes for a draw.
      impl_->clip_gs_logged = true;
      REXLOG_INFO("pdz_native: user clip planes in use (PA_CL_CLIP_CNTL {:08X}): first clip-plane GS pipeline created",
                  rs.clip_cntl);
    }
    return &p;
  }

  // ---------------------------------------------------------------------------
  // The shipped pipeline list (native_pipeline.h pipeline_seed; 2026-11-03)
  // ---------------------------------------------------------------------------
  namespace pipeline_seed
  {
    namespace
    {
      // data/native_pipeline_list.txt, embedded by CMake (cmake/pdz_xenosrecomp.cmake). Hashes and state only.
      const unsigned char kList[] = {
#include "native_pipeline_list.inc"
          0};

      struct Row
      {
        PsoKey key;
        Query q;
        std::vector<RecipeElement> elements;
      };

      uint64_t Hex(std::string_view v)
      {
        uint64_t h = 0;
        for (char c : v)
        {
          h = h * 16 + uint64_t(c >= 'a' ? c - 'a' + 10 : c >= 'A' ? c - 'A' + 10 : c - '0');
        }
        return h;
      }

      std::vector<std::string_view> Split(std::string_view s, char sep)
      {
        std::vector<std::string_view> out;
        for (size_t at = 0; at <= s.size();)
        {
          const size_t next = s.find(sep, at);
          std::string_view part = s.substr(at, next == std::string_view::npos ? std::string_view::npos : next - at);
          while (!part.empty() && part.front() == ' ')
          {
            part.remove_prefix(1);
          }
          while (!part.empty() && (part.back() == ' ' || part.back() == '\r'))
          {
            part.remove_suffix(1);
          }
          if (!part.empty())
          {
            out.push_back(part);
          }
          if (next == std::string_view::npos)
          {
            break;
          }
          at = next + 1;
        }
        return out;
      }

      // One line: 23 key fields | 3 DXIL hashes | elements (make_pipeline_list.py).
      bool ParseRow(std::string_view line, Row &r)
      {
        const std::vector<std::string_view> parts = Split(line, '|');
        if (parts.size() < 2)
        {
          return false;
        }
        const std::vector<std::string_view> k = Split(parts[0], ' ');
        const std::vector<std::string_view> ids = Split(parts[1], ' ');
        if (k.size() != 23 || ids.size() != 3)
        {
          return false;
        }
        std::memset(&r.key, 0, sizeof(r.key));
        r.key.vs_hash = Hex(k[0]);
        r.key.ps_hash = Hex(k[1]);
        r.key.layout_hash = Hex(k[2]);
        r.key.depthcontrol = uint32_t(Hex(k[3]));
        for (int i = 0; i < 4; ++i)
        {
          r.key.blendcontrol[i] = uint32_t(Hex(k[4 + i]));
        }
        r.key.color_mask = uint32_t(Hex(k[8]));
        r.key.stencil_masks = uint32_t(Hex(k[9]));
        r.key.raster = uint32_t(Hex(k[10]));
        r.key.depth_bias = int32_t(std::strtol(std::string(k[11]).c_str(), nullptr, 10));
        const uint32_t slope_bits = uint32_t(Hex(k[12]));
        std::memcpy(&r.key.slope_bias, &slope_bits, 4);
        r.key.topology_type = uint32_t(Hex(k[13]));
        r.key.rt_count = uint32_t(Hex(k[14]));
        for (int i = 0; i < 4; ++i)
        {
          r.key.rt[i] = uint32_t(Hex(k[15 + i]));
        }
        r.key.ds = uint32_t(Hex(k[19]));
        r.key.depth_variant = uint32_t(Hex(k[20]));
        r.key.strip_cut = uint32_t(Hex(k[21]));
        r.key.blend7e3_variant = uint32_t(Hex(k[22]));
        r.q.vs = r.key.vs_hash;
        r.q.ps = r.key.ps_hash;
        r.q.depth_variant = r.key.depth_variant;
        r.q.blend7e3_variant = r.key.blend7e3_variant;
        r.q.vs_id = Hex(ids[0]);
        r.q.ps_id = Hex(ids[1]);
        r.q.variant_id = Hex(ids[2]);
        r.elements.clear();
        if (parts.size() > 2)
        {
          for (std::string_view e : Split(parts[2], ';'))
          {
            const std::vector<std::string_view> f = Split(e, ':');
            if (f.size() == 4)
            {
              RecipeElement re;
              re.attr = uint32_t(std::strtoul(std::string(f[0]).c_str(), nullptr, 10));
              re.format = uint32_t(std::strtoul(std::string(f[1]).c_str(), nullptr, 10));
              re.stream = uint32_t(std::strtoul(std::string(f[2]).c_str(), nullptr, 10));
              re.offset = uint32_t(std::strtoul(std::string(f[3]).c_str(), nullptr, 10));
              r.elements.push_back(re);
            }
          }
        }
        return true;
      }

      std::vector<Row> Rows()
      {
        std::vector<Row> rows;
        std::string_view text(reinterpret_cast<const char *>(kList));
        while (!text.empty())
        {
          const size_t eol = text.find('\n');
          const std::string_view line = text.substr(0, eol);
          text = eol == std::string_view::npos ? std::string_view() : text.substr(eol + 1);
          Row r;
          if (!line.empty() && line[0] != '#' && ParseRow(line, r))
          {
            rows.push_back(std::move(r));
          }
        }
        return rows;
      }
    } // namespace

    uint64_t ListHash() { return XXH3_64bits(kList, sizeof(kList) - 1); }

    std::vector<Query> ListQueries()
    {
      std::vector<Query> out;
      for (const Row &r : Rows())
      {
        out.push_back(r.q);
      }
      return out;
    }

    Report WriteRecipes(const std::string &path, const std::function<Blobs(const Query &)> &provide)
    {
      Report rep;
      const std::vector<Row> rows = Rows();
      rep.listed = uint32_t(rows.size());
      ByteWriter blobs_w, recipes_w;
      std::unordered_set<uint64_t> blob_seen;
      uint32_t blob_count = 0, recipe_count = 0;
      const auto add_blob = [&](const std::vector<uint8_t> *b) -> uint64_t {
        if (!b || b->empty())
        {
          return 0;
        }
        const uint64_t id = BlobId(b->data(), b->size());
        if (blob_seen.insert(id).second)
        {
          blobs_w.Put(id);
          blobs_w.Put(uint32_t(b->size()));
          blobs_w.PutBytes(b->data(), b->size());
          ++blob_count;
        }
        return id;
      };
      for (const Row &r : rows)
      {
        const Blobs b = provide(r.q);
        const bool needs_ps = r.q.ps != 0, needs_variant = r.q.depth_variant || r.q.blend7e3_variant;
        if (!b.vs || (needs_ps && !b.ps && !(r.q.depth_variant && b.variant)) || (needs_variant && !b.variant))
        {
          ++rep.no_shader; // a shader the player doesn't have (a DLC pack's), or a variant that didn't build
          continue;
        }
        const uint64_t vs_id = add_blob(b.vs), ps_id = add_blob(b.ps), var_id = add_blob(b.variant);
        if (vs_id != r.q.vs_id || ps_id != r.q.ps_id || var_id != r.q.variant_id)
        {
          ++rep.other_dxil; // built from the player's DXIL anyway: the draws use that one
        }
        recipes_w.Put(r.key);
        recipes_w.Put(vs_id);
        recipes_w.Put(ps_id);
        recipes_w.Put(var_id);
        recipes_w.Put(uint16_t(0));
        recipes_w.Put(uint16_t(r.elements.size()));
        for (const RecipeElement &e : r.elements)
        {
          recipes_w.Put(e.attr);
          recipes_w.Put(e.format);
          recipes_w.Put(e.stream);
          recipes_w.Put(e.offset);
        }
        ++recipe_count;
      }
      ByteWriter w;
      w.PutBytes(kRecipeMagic, 8);
      w.Put(kPsoCacheFormat);
      w.Put(uint32_t(sizeof(PsoKey)));
      w.Put(blob_count);
      w.Put(recipe_count);
      w.PutBytes(blobs_w.bytes.data(), blobs_w.bytes.size());
      w.PutBytes(recipes_w.bytes.data(), recipes_w.bytes.size());
      w.Put(uint64_t(XXH3_64bits(w.bytes.data(), w.bytes.size())));
      std::error_code ec;
      std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
      if (WriteFileAtomically(path, w.bytes))
      {
        rep.written = recipe_count;
      }
      return rep;
    }
  } // namespace pipeline_seed
} // namespace pdz::native
