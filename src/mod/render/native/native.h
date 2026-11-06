// Native renderer (stage 1): switches, the entry points the shadow hooks call, and the guest-memory helpers
// shared by every native_* file. See CORE_NOTES.md (what each file does) and HOOK_WIRING.md (the exact call
// sites in src\mod\render\shadow_hooks.cpp the orchestrator wires).
//
// Everything here runs inside the D3D hooks with RenderMutex() held: the game hands its D3D device between
// threads (log 022: main thread, frontend thread, render thread), so the entry points take the mutex. The Xenia GPU plugin keeps rendering
// (shadow mode, RENDER_FEASIBILITY.md 5.2 option C): the native path only READS guest memory and the guest
// device struct, with one documented exception (the de-rez snapshot readback writes the snapshot texture's
// guest memory, exactly what the plugin's resolve_readback does for the same bytes).
#pragma once
#include <mutex>
#include <string>

#include <cstdint>
#include <cstring>

#include <rex/cvar.h>

#include "../gpu_sync.h"      // graphics_backend null (NullBackend)
#include "../shadow_record.h" // pdz::shadow::dev offsets, GuestPtr / Load32 helpers (read-only use)

REXCVAR_DECLARE(bool, pdz_native_renderer);
REXCVAR_DECLARE(int32_t, pdz_native_scale);
REXCVAR_DECLARE(int32_t, pdz_native_present_mode);
REXCVAR_DECLARE(bool, pdz_native_output);
REXCVAR_DECLARE(int32_t, pdz_native_output_toggle_s);
REXCVAR_DECLARE(int32_t, pdz_native_capture_s);
REXCVAR_DECLARE(std::string, pdz_native_capture_swaps);
REXCVAR_DECLARE(std::string, pdz_native_capture_dir);
REXCVAR_DECLARE(bool, pdz_native_async_readback);

namespace pdz::native
{
  // One relaxed load (and one bool): the only cost of a hook call site while the native renderer is off. With
  // graphics_backend null (gpu_sync.h: no emulated frame exists) the native renderer is always on.
  inline bool Enabled() { return REXCVAR_GET(pdz_native_renderer) || pdz::gpusync::NullBackend(); }

  // Integer resolution scale (pdz_native_scale clamped to 1..4), latched the first time a host surface is
  // created so every surface of a session shares one scale (a change needs a restart; logged). With
  // pdz_native_scale_follow in effect (default: graphics_backend null) ChooseScaleAtStartup latches it first.
  uint32_t Scale();

  // The sun's shadow maps (BUG_AUDIT VR4): the shadow pass draws into a 1344 x 1344 D24S8 depth surface and a
  // 1344 x 1344 R32F colour target (both created in TU3 sub_8261DFC8) and resolves the depth into four 1344 x 1344
  // k_24_8 textures (the mirrors 1D700000 / 1C580000 / 1B818000 / 1CC64000). No other surface or resolve texture of
  // the game has that size.
  constexpr uint32_t kShadowMapSize = 1344;

  // The host scale of a surface or resolve mirror of this guest size: Scale(), or for the shadow maps (1344 x 1344)
  // Scale() x pdz_shadow_map_scale (1-4, the product capped at 12 = 16128 x 16128, under D3D12's 16384 limit; latched
  // with the first one). Every per-surface path (viewport, scissor, clears, resolve copies, point sizes, polygon-offset
  // slope) follows the surface's own scale, so the shadow pass renders at that scale while the guest keeps its
  // 1344 x 1344 viewport. With pdz_shadow_map_scale 1 this is Scale() for every size.
  uint32_t SurfaceScale(uint32_t width, uint32_t height);

  // The shadow maps' extra resolution over the rest of the frame: SurfaceScale(1344, 1344) / Scale() once the first
  // shadow surface latched it, else 1. Lock-free (any thread): the sun shadow setup hook (sun_shadows.cpp) divides the
  // far cascade's bias factor by it when the native renderer is the only output.
  float ShadowMapRelativeScale();

  // App setup (gpu_sync.cpp OnBackendSelected, after the GPU plugin is loaded and the null backend latched, before any
  // guest thread): with pdz_native_scale_follow 1 and the null backend, or 2, latches the scale now: pdz_native_scale
  // when it is set explicitly, else the GPU plugin's resolution_scale (draw_resolution_scale_x / _y as the plugin reads
  // them), clamped to 1..4. Otherwise nothing (Scale() latches pdz_native_scale at the first host surface).
  void ChooseScaleAtStartup();

  // App setup (the app's OnPreSetup, before the GPU plugin is loaded): the native default (2026-10-29). With
  // pdz_native_renderer set nowhere: `backend` becomes null and PDZ_PLAY.bat's render profile is applied to the cvars
  // nothing has set (logged), returns true. A non-null graphics_backend on the command line turns the native renderer
  // off instead (the emulator alone). pdz_native_renderer set anywhere: nothing changes (false).
  bool ApplyNativeDefault(std::string &backend);

  // App setup, after ChooseScaleAtStartup (native_present.cpp): latches pdz_native_present_timing, registers the null
  // command processor's XE_SWAP listener when it is 1 with the null backend, and sets the frame_pacing latency pairing
  // for a native output presented from the Swap hook.
  void ChoosePresentTimingAtStartup();

  // ---------------------------------------------------------------------------
  // Guest memory (big-endian). Virtual addresses go through `base` exactly like the shadow recorder; physical
  // addresses (fetch constants, vertex buffers) go through the runtime's physical view.
  // ---------------------------------------------------------------------------
  using pdz::shadow::BitsToFloat;
  using pdz::shadow::GuestPtr;
  using pdz::shadow::IsGuestPointer;
  using pdz::shadow::Load32;
  using pdz::shadow::Load8;
  using pdz::shadow::Swap32;

  inline uint16_t Load16(const uint8_t *base, uint32_t address)
  {
    const uint8_t *p = GuestPtr(base, address);
    return uint16_t((uint16_t(p[0]) << 8) | p[1]);
  }
  inline int32_t LoadS32(const uint8_t *base, uint32_t address) { return int32_t(Load32(base, address)); }
  inline float LoadFloat(const uint8_t *base, uint32_t address) { return BitsToFloat(Load32(base, address)); }
  inline uint16_t Swap16(uint16_t v) { return uint16_t((v >> 8) | (v << 8)); }

  // Host pointer to guest physical memory (the 512 MB physical view of the runtime's memory mapping).
  // Returns nullptr before the runtime exists. The whole physical range is mapped, so reads inside
  // [0, 0x1FFFFFFF] never fault.
  uint8_t *PhysPtr(uint32_t physical_address);

  // ---------------------------------------------------------------------------
  // Entry points for the shadow hooks (HOOK_WIRING.md). Every one checks Enabled() itself except where noted.
  // Arguments are the raw guest registers of the hooked call.
  // ---------------------------------------------------------------------------

  // One recursive mutex around every use of our command list and resources. Every entry point below takes it
  // (recursive, so nested entry points are fine); the shadow hooks hold it across Record + OnDraw so the ring
  // record and the native draw belong to the same thread's call. Lock order: this mutex, then the SDK
  // presenter's own lock (RefreshGuestOutput / CaptureGuestOutput); nothing under the presenter's lock takes it.
  // RenderLock: a lock_guard that tries first; only a contended acquisition goes through LockContended, which charges
  // the wait to the frame timing phase wait.mutex (pdz_frame_timing_phases; frame_timing.h). Same cost as lock_guard
  // otherwise.
  class RenderLock
  {
  public:
    explicit RenderLock(std::recursive_mutex &m) : m_(m)
    {
      if (!m_.try_lock())
      {
        LockContended(m_);
      }
    }
    ~RenderLock() { m_.unlock(); }
    RenderLock(const RenderLock &) = delete;
    RenderLock &operator=(const RenderLock &) = delete;

  private:
    static void LockContended(std::recursive_mutex &m); // native_device.cpp
    std::recursive_mutex &m_;
  };
  std::recursive_mutex &RenderMutex();
  // Logs when the guest D3D calls move to another OS thread (cold; call with the mutex held).
  void NoteRenderThread(const char *where);

  // Opens the native frame if none is open (BeginFrame on the device). Called by every other entry point;
  // the pipeline/draw agent calls it at the top of its draw path too. Returns false when there is no device.
  bool EnsureFrame();

  // D3DDevice_Clear (sub_826E0A48): r4 count, r5 rects, r6 flags, r7 colour, f1 Z (bits), r9 stencil.
  // Call BEFORE forwarding (the bound targets and the viewport are read from the device struct).
  void OnClear(uint8_t *base, uint32_t device, uint32_t count, uint32_t rects, uint32_t flags, uint32_t color,
               uint32_t z_bits, uint32_t stencil);

  // D3DDevice_Resolve (sub_826F21D0): r4 flags, r5 source rect, r6 dest texture, r7 dest point, r8 level,
  // r9 slice, r10 pClearColor (a guest pointer to a D3DVECTOR4, logged only), f1 ClearZ (bits, logged only),
  // call_site = the caller's LR (log). Call AFTER forwarding: the copy registers D3D writes inside the call are
  // read from the shadow (RB_COPY_DEST_INFO at device+11876: component swap, exponent bias; RB_COPY_CONTROL
  // +11864: bit 8 colour clear, bit 9 depth / stencil clear; RB_DEPTH_CLEAR +11884, RB_COLOR_CLEAR +11888,
  // RB_COLOR_CLEAR_LO +11892: the clear values D3D packed in the target's EDRAM format).
  void OnResolve(uint8_t *base, uint32_t device, uint32_t flags, uint32_t src_rect, uint32_t dest_texture,
                 uint32_t dest_point, uint32_t level, uint32_t slice, uint32_t clear_color_ptr, uint32_t z_bits,
                 uint32_t call_site);

  // D3DDevice_Swap (sub_826DF960): r4 front buffer texture. Call AFTER forwarding (and after the shadow's
  // OnSwap). Always call it (it closes an open native frame even when the switch was just turned off).
  void OnSwap(uint8_t *base, uint32_t device, uint32_t front_buffer);

  // D3DDevice_SetGammaRamp: the 3 x 256 big-endian u16 ramp at `ramp` becomes the gamma pass's ramp (native_present.cpp;
  // the hook, or the record thread with the hook's copy). Takes RenderMutex().
  void ApplyGammaRamp(const uint8_t *base, uint32_t ramp);

  // The de-rez snapshot readbacks done so far (Surfaces::OnInsertCallback, one GPU round trip each), for the GPU
  // census (gpu_census.h). Takes RenderMutex(). With pdz_native_async_readback: the snapshots written so far.
  uint32_t SnapshotReadbacks();
  // pdz_native_async_readback (native_surfaces.cpp): sync / async snapshot readbacks, who wrote the async ones, the
  // sync fallbacks and DeResThread's GPU waits, for the 'pdz_sync: SUMMARY' line. Any thread (atomics).
  std::string SnapshotReadbackSummary();

  // Registers bind_native_output (F9) with the SDK's keybinds; call from the app's OnCreateDialogs. When it
  // was never called, OnSwap polls GetAsyncKeyState(VK_F9) instead (edge-triggered).
  void RegisterBinds();
  // Registers bind_mark_spot (F12; native_present.cpp, mark-this-spot test aid) with the SDK's keybinds; call from the
  // app's OnCreateDialogs. When it was never called, OnSwap polls GetAsyncKeyState(VK_F12) instead (edge-triggered).
  void RegisterMarkBind();

  // The output selector (F9): true = the presenter shows the native frame, false = the emulated one.
  bool NativeOutputSelected();
  // True when a draw of `frame` was skipped for a missing shader (native_draw.cpp): that frame is shown
  // from the emulator instead of the native renderer.
  bool FrameHadShaderMiss(uint64_t frame);
  // Arms the presenter's external-owner flag from the configured output selection; called at the first
  // native call of the process (EnsureFrame) so the GPU plugin never presents concurrently.
  void ArmPresenterOwner();
  void ToggleNativeOutput();

  // pdz_native_present_mode 1: hands the frame kept at the last Swap to the presenter; called by EnsureFrame
  // when it opens a new frame (native_present.cpp).
  void PresentPendingFrame();

  // One-frame trace (--pdz_native_frame_trace=<auto-diff capture number>): every clear, resolve and draw of the
  // frame the auto-diff captures is logged with "pdz_trace:" (targets, viewport, depth / blend state, shaders).
  bool FrameTraceActive();
  void SetFrameTrace(bool active);
  // Short description of a surface for the trace: "guest(WxH fmt N, edram B)" or "-".
  std::string TraceSurface(uint32_t guest, uint32_t width, uint32_t height, uint32_t format, uint32_t edram, bool depth);
} // namespace pdz::native
