// Shadow-mode hooks on the Title Update 3 D3D entry points (RENDER_FEASIBILITY
// 2.2). Every hook forwards to the original (__imp__) and, with
// pdz_render_shadow on, snapshots the guest device struct into the ring
// (shadow_record.cpp). The emulator keeps rendering; nothing here writes guest
// memory or changes a register.
//
// Pattern: src\mod\cut_scenarios.cpp (REX_HOOK_RAW + REX_EXTERN(__imp__)).
// Not compiled into the game: FILES ONLY (stage 0).
//
// Argument registers (checked in generated\default\*.cpp, see README.md):
//   r3 = device for every D3DDevice_* function except the three Create*
//   functions, which take the data pointer in r3 and return the new object in
//   r3; floats arrive in f1 and also consume the next GPR slot (Clear's
//   Stencil is r9, not r8).
#include <cstdint>

#include <rex/hook.h>
#include <rex/ppc/func.h>

#include "shadow.h"
#include "shadow_record.h"
#include "shadow_assert.h"
#include "test_harness.h"
#include "cpu_placement.h"
#include "frame_timing.h"
#include "game_thread_sampler.h"
#include "guest_ip_profile.h"
#include "gpu_census.h"
#include "gpu_sync.h"
#include "pm4_skip.h"
#include "native/native.h"
#include "native/native_draw.h"
#include "native/native_recthread.h"
#include "cutscene_dump.h"

// Draw path
REX_EXTERN(__imp__sub_826EA128); // D3DDevice_DrawIndexedVertices
REX_EXTERN(__imp__sub_826E9EE8); // D3DDevice_DrawVertices
REX_EXTERN(__imp__sub_826E9A60); // D3DDevice_DrawVerticesUP
REX_EXTERN(__imp__sub_826E9E18); // D3DDevice_DrawIndexedVerticesUP (BeginIndexedVertices sub_826E9AD8 + 2 memcpy)
REX_EXTERN(__imp__sub_826E97B0); // D3DDevice_BeginVertices (emits the draw, returns the ring data pointer)
REX_EXTERN(__imp__sub_826E9A50); // D3DDevice_EndVertices (commits the deferred ring write pointer)
REX_EXTERN(__imp__sub_826E0A48); // D3DDevice_Clear
REX_EXTERN(__imp__sub_826F21D0); // D3DDevice_Resolve
REX_EXTERN(__imp__sub_826DF960); // D3DDevice_Swap
// State setters
REX_EXTERN(__imp__sub_826ED300); // D3DDevice_SetTexture
REX_EXTERN(__imp__sub_826F0E90); // D3DDevice_SetRenderTarget
REX_EXTERN(__imp__sub_826F11F8); // D3DDevice_SetDepthStencilSurface
REX_EXTERN(__imp__sub_826E4378); // D3DDevice_SetVertexShader
REX_EXTERN(__imp__sub_826E3E90); // D3DDevice_SetPixelShader
REX_EXTERN(__imp__sub_826E4448); // D3DDevice_SetVertexDeclaration
REX_EXTERN(__imp__sub_826F0A70); // D3DDevice_SetViewport
REX_EXTERN(__imp__sub_826F0980); // D3DDevice_SetScissorRect
REX_EXTERN(__imp__sub_826F0CE0); // D3DDevice_SetStreamSource
REX_EXTERN(__imp__sub_826F0DD8); // D3DDevice_SetIndices
// Object creation
REX_EXTERN(__imp__sub_826E44E8); // D3DDevice_CreateVertexDeclaration
REX_EXTERN(__imp__sub_826E3D60); // D3DDevice_CreatePixelShader
REX_EXTERN(__imp__sub_826E4108); // D3DDevice_CreateVertexShader

namespace
{
  using namespace pdz::shadow;

  inline uint32_t FloatArgBits(const PPCContext &ctx)
  {
    // Guest floats travel as doubles in f1.
    const float f = float(ctx.f1.f64);
    uint32_t bits;
    std::memcpy(&bits, &f, sizeof(bits));
    return bits;
  }

  // pdz_render_shadow_vs_ucode_erase: the Create hooks update the shader / declaration tables and the kept vertex
  // microcode map, which the native draw path reads under the render lock (a loading thread may create a level's
  // shaders while the render thread draws the loading screen). Taken after the guest call returned, as every hook does:
  // never held across a guest call. Off = unlocked, as before.
  std::unique_lock<std::recursive_mutex> CreateHookLock()
  {
    std::unique_lock<std::recursive_mutex> lock(pdz::native::RenderMutex(), std::defer_lock);
    if (REXCVAR_GET(pdz_render_shadow_vs_ucode_erase))
    {
      lock.lock();
    }
    return lock;
  }
} // namespace

// -----------------------------------------------------------------------------
// Draws: snapshot BEFORE forwarding. At entry the game has finished its inline
// register stores and the five dirty masks still say what the flush
// (sub_826FA8B8, called inside the draw) is about to emit; after the call the
// masks are zero. Two fields change inside the draw and are therefore recorded
// from the arguments instead: VGT_INDX_OFFSET (+11540, set to the base vertex)
// and the stream fetch constants rebuilt by sub_826F9578 (see shadow_assert.md).
// -----------------------------------------------------------------------------

// DrawIndexedVertices(device, PrimitiveType, BaseVertexIndex, StartIndex, IndexCount)
// r4 prim (-> r17), r5 base vertex (compared with/stored to +11540), r6 start
// index (stack 284), r7 index count (-> r16).
REX_HOOK_RAW(sub_826EA128)
{
  const pdz::gtsample::Mark sampler_mark(pdz::gtsample::Where::kD3dHook); // pdz_game_thread_profile
  if (Enabled() && pdz::native::recthread::Active())
  {
    // pdz_native_record_thread: the record and the draw's copies queued; the worker draws (native_recthread.h).
    pdz::native::recthread::OnDraw(base, Hook::kDrawIndexedVertices, Kind::kDrawIndexedVertices, ctx.r3.u32,
                                   uint32_t(ctx.lr), ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, 0, 0, 0, 0);
  }
  else if (Enabled())
  {
    pdz::native::RenderLock lock(pdz::native::RenderMutex()); // ring record + native call on one thread
    Count(Hook::kDrawIndexedVertices);
    DrawRecord &r = Record(base, ctx.r3.u32, Kind::kDrawIndexedVertices, uint32_t(ctx.lr),
                           ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, 0, 0);
    if (pdz::native::Enabled())
    {
      pdz::native::OnDraw(base, r); // native/native_draw.h: the native draw from the same record
    }
  }
  {
    const pdz::timing::GuestCall guest_call; // pdz_frame_timing_phases: guest.d3d (drawcost)
    __imp__sub_826EA128(ctx, base);
  }
  if (AssertEnabled())
  {
    const pdz::timing::OutsideScope assert_time(pdz::timing::Phase::kShadowAssert); // pdz_frame_timing_phases
    AssertOnDrawReturn(base); // shadow_assert.h: VGT_DRAW_INITIATOR vs the record
  }
}

// DrawVertices(device, PrimitiveType, StartVertex, VertexCount)
// r4 prim (-> r24), r5 start vertex (-> r23, stored to +11540), r6 count (-> r21).
REX_HOOK_RAW(sub_826E9EE8)
{
  const pdz::gtsample::Mark sampler_mark(pdz::gtsample::Where::kD3dHook); // pdz_game_thread_profile
  if (Enabled() && pdz::native::recthread::Active())
  {
    pdz::native::recthread::OnDraw(base, Hook::kDrawVertices, Kind::kDrawVertices, ctx.r3.u32, uint32_t(ctx.lr),
                                   ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, 0, 0, 0, 0, 0);
  }
  else if (Enabled())
  {
    pdz::native::RenderLock lock(pdz::native::RenderMutex()); // ring record + native call on one thread
    Count(Hook::kDrawVertices);
    DrawRecord &r = Record(base, ctx.r3.u32, Kind::kDrawVertices, uint32_t(ctx.lr),
                           ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, 0, 0, 0);
    if (pdz::native::Enabled())
    {
      pdz::native::OnDraw(base, r); // native/native_draw.h: the native draw from the same record
    }
  }
  {
    const pdz::timing::GuestCall guest_call; // pdz_frame_timing_phases: guest.d3d (drawcost)
    __imp__sub_826E9EE8(ctx, base);
  }
  if (AssertEnabled())
  {
    const pdz::timing::OutsideScope assert_time(pdz::timing::Phase::kShadowAssert); // pdz_frame_timing_phases
    AssertOnDrawReturn(base); // shadow_assert.h: VGT_DRAW_INITIATOR vs the record
  }
}

// DrawVerticesUP(device, PrimitiveType, VertexCount, pVertexStreamZeroData, VertexStreamZeroStride)
// r4 prim (-> r28), r5 count (-> r30), r6 data (-> r27), r7 stride (-> r29).
REX_HOOK_RAW(sub_826E9A60)
{
  const pdz::gtsample::Mark sampler_mark(pdz::gtsample::Where::kD3dHook); // pdz_game_thread_profile
  if (Enabled() && pdz::native::recthread::Active())
  {
    // The vertices (game memory) are copied at the hook.
    pdz::native::recthread::OnDraw(base, Hook::kDrawVerticesUP, Kind::kDrawVerticesUP, ctx.r3.u32, uint32_t(ctx.lr),
                                   ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, 0, 0, 0, 0);
  }
  else if (Enabled())
  {
    pdz::native::RenderLock lock(pdz::native::RenderMutex()); // ring record + native call on one thread
    Count(Hook::kDrawVerticesUP);
    DrawRecord &r = Record(base, ctx.r3.u32, Kind::kDrawVerticesUP, uint32_t(ctx.lr),
                           ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, 0, 0);
    if (pdz::native::Enabled())
    {
      pdz::native::OnDraw(base, r); // native/native_draw.h: the native draw from the same record
    }
  }
  {
    const pdz::timing::GuestCall guest_call; // pdz_frame_timing_phases: guest.d3d (drawcost)
    __imp__sub_826E9A60(ctx, base);
  }
  if (AssertEnabled())
  {
    const pdz::timing::OutsideScope assert_time(pdz::timing::Phase::kShadowAssert); // pdz_frame_timing_phases
    AssertOnDrawReturn(base); // shadow_assert.h: VGT_DRAW_INITIATOR vs the record
  }
}

// DrawIndexedVerticesUP(device, PrimitiveType, MinVertexIndex, NumVertices, IndexCount, pIndexData,
//                       IndexDataFormat, pVertexStreamZeroData, VertexStreamZeroStride)
// r4 prim (-> r25), r5 min vertex index (-> r29), r6 vertex count (-> r28), r7 index count (-> r27), r8 index data
// (-> r24), r9 index format (-> r26; 1 = 32-bit, else 16-bit), r10 vertex data (-> r23); the stride is the ninth
// argument, on the caller's stack at entry r1 + 84 (the body reads 276(r1) after stwu r1,-192). The body calls
// BeginIndexedVertices (sub_826E9AD8: flush, ring space for the vertices [min, min + count) and the indices, PM4
// DRAW_INDX with VGT_INDX_OFFSET = -min), copies both and ends with the inlined EndVertices store. Callers: the UI
// shape drawer sub_827B5890 (shapes with an index array: TRILIST, 16-bit, stride 8) and sub_827B3368. Not hooked
// before: the Subway / Bridge / Arena bar frame and the front-end rings were missing natively (the shadow
// assert's only unpaired flush, lr 826E9B84, is the one inside BeginIndexedVertices).
REX_HOOK_RAW(sub_826E9E18)
{
  const pdz::gtsample::Mark sampler_mark(pdz::gtsample::Where::kD3dHook); // pdz_game_thread_profile
  if (Enabled() && pdz::native::recthread::Active())
  {
    // The vertices and indices (game memory) are copied at the hook.
    pdz::native::recthread::OnDraw(base, Hook::kDrawIndexedVerticesUP, Kind::kDrawIndexedVerticesUP, ctx.r3.u32,
                                   uint32_t(ctx.lr), ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32,
                                   ctx.r9.u32, ctx.r10.u32,
                                   pdz::shadow::IsGuestPointer(ctx.r1.u32) ? pdz::shadow::Load32(base, ctx.r1.u32 + 84)
                                                                            : 0);
  }
  else if (Enabled())
  {
    pdz::native::RenderLock lock(pdz::native::RenderMutex()); // ring record + native call on one thread
    Count(Hook::kDrawIndexedVerticesUP);
    DrawRecord &r = Record(base, ctx.r3.u32, Kind::kDrawIndexedVerticesUP, uint32_t(ctx.lr),
                           ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, 0);
    r.a[5] = ctx.r9.u32;
    r.a[6] = ctx.r10.u32;
    r.a[7] = pdz::shadow::IsGuestPointer(ctx.r1.u32) ? pdz::shadow::Load32(base, ctx.r1.u32 + 84) : 0;
    if (pdz::native::Enabled())
    {
      pdz::native::OnDraw(base, r); // native/native_draw.h (pdz_native_indexed_up)
    }
  }
  {
    const pdz::timing::GuestCall guest_call; // pdz_frame_timing_phases: guest.d3d (drawcost)
    __imp__sub_826E9E18(ctx, base);
  }
  if (AssertEnabled())
  {
    const pdz::timing::OutsideScope assert_time(pdz::timing::Phase::kShadowAssert); // pdz_frame_timing_phases
    AssertOnDrawReturn(base); // shadow_assert.h: VGT_DRAW_INITIATOR vs the record
  }
}

// BeginVertices(device, PrimitiveType, VertexCount, VertexStreamZeroStride) -> void* data
// r4 prim (-> r26), r5 count (-> r27), r6 stride (bytes = r5 * r6). The
// function flushes state, allocates count*stride bytes in the ring, emits the
// draw packet and returns the data pointer in r3 (stored at device+13640; the
// deferred ring write pointer at +13636 is committed by EndVertices). The
// record is patched with the returned pointer.
REX_HOOK_RAW(sub_826E97B0)
{
  const pdz::gtsample::Mark sampler_mark(pdz::gtsample::Where::kD3dHook); // pdz_game_thread_profile
  if (Enabled() && pdz::native::recthread::Active())
  {
    // pdz_native_record_thread: the previous batch's flush point and the record before the call, the deferred draw
    // (with the returned data pointer) after it.
    const uint32_t sequence = pdz::native::recthread::OnBeginVerticesEnter(base, ctx.r3.u32, uint32_t(ctx.lr),
                                                                           ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
    {
      const pdz::timing::GuestCall guest_call; // pdz_frame_timing_phases: guest.d3d (drawcost)
      __imp__sub_826E97B0(ctx, base);
    }
    pdz::native::recthread::OnBeginVerticesLeave(base, sequence, ctx.r3.u32);
    if (AssertEnabled())
    {
      const pdz::timing::OutsideScope assert_time(pdz::timing::Phase::kShadowAssert); // pdz_frame_timing_phases
      AssertOnDrawReturn(base);
    }
    return;
  }
  if (Enabled())
  {
    DrawRecord *rp = nullptr;
    {
      // The mutex is never held across a guest call (the guest may block on the GPU ring).
      pdz::native::RenderLock lock(pdz::native::RenderMutex());
      if (pdz::native::Enabled())
      {
        // The previous batch (ended without EndVertices) is complete; draw it before this call reserves ring space
        // that could wrap over its vertices (review finding 13).
        pdz::native::FlushPendingBegin(base);
      }
      Count(Hook::kBeginVertices);
      rp = &Record(base, ctx.r3.u32, Kind::kBeginVertices, uint32_t(ctx.lr),
                   ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, 0, 0, 0);
    }
    {
      const pdz::timing::GuestCall guest_call; // pdz_frame_timing_phases: guest.d3d (drawcost)
      __imp__sub_826E97B0(ctx, base);
    }
    {
      pdz::native::RenderLock lock(pdz::native::RenderMutex());
      rp->a[3] = ctx.r3.u32;
      if (pdz::native::Enabled())
      {
        pdz::native::OnDraw(base, *rp); // deferred: the data pointer is in r.a[3]; drawn at EndVertices
      }
    }
    if (AssertEnabled())
    {
      const pdz::timing::OutsideScope assert_time(pdz::timing::Phase::kShadowAssert); // pdz_frame_timing_phases
      AssertOnDrawReturn(base);
    }
    return;
  }
  __imp__sub_826E97B0(ctx, base);
  if (AssertEnabled())
  {
    AssertOnDrawReturn(base);
  }
}

// EndVertices(device): device[+0] = device[+13636]. Only caller: sub_827BA8E8
// (XUI / video side). Counted, nothing to record.
REX_HOOK_RAW(sub_826E9A50)
{
  const pdz::gtsample::Mark sampler_mark(pdz::gtsample::Where::kD3dHook); // pdz_game_thread_profile
  if (pdz::native::recthread::Active())
  {
    const uint32_t device = ctx.r3.u32;
    __imp__sub_826E9A50(ctx, base);
    pdz::native::recthread::OnEndVertices(base, device); // the counter and the queued EndVertices
    return;
  }
  if (Enabled())
  {
    Count(Hook::kEndVertices);
  }
  __imp__sub_826E9A50(ctx, base);
  if (pdz::native::Enabled())
  {
    pdz::native::OnEndVertices(base);
  }
}

// Clear(device, Count, pRects, Flags, Color, Z, Stencil)
// r4 count, r5 rects, r6 flags (-> r28), r7 colour (stored at 212(r1) and loaded
// as a vector), f1 Z (fmr f31,f1; its GPR slot r8 is skipped), r9 stencil (-> r27).
REX_HOOK_RAW(sub_826E0A48)
{
  const pdz::gtsample::Mark sampler_mark(pdz::gtsample::Where::kD3dHook); // pdz_game_thread_profile
  if (pdz::native::recthread::Active())
  {
    // pdz_native_record_thread: the record, the rects (caller memory) and the device's targets / viewport queued.
    pdz::native::recthread::OnClear(base, ctx.r3.u32, uint32_t(ctx.lr), ctx.r4.u32, ctx.r5.u32, ctx.r6.u32,
                                    ctx.r7.u32, FloatArgBits(ctx), ctx.r9.u32);
    const pdz::timing::GuestCall guest_call; // pdz_frame_timing_phases: guest.d3d (drawcost)
    __imp__sub_826E0A48(ctx, base);
    return;
  }
  if (Enabled())
  {
    pdz::native::RenderLock lock(pdz::native::RenderMutex()); // ring record + native call on one thread
    Count(Hook::kClear);
    Record(base, ctx.r3.u32, Kind::kClear, uint32_t(ctx.lr),
           ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r9.u32, FloatArgBits(ctx));
  }
  if (pdz::native::Enabled())
  {
    pdz::native::OnClear(base, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, FloatArgBits(ctx), ctx.r9.u32);
  }
  const pdz::timing::GuestCall guest_call; // pdz_frame_timing_phases: guest.d3d (drawcost)
  __imp__sub_826E0A48(ctx, base);
}

// Resolve(device, Flags, pSourceRect, pDestTexture, pDestPoint, DestLevel,
//         DestSliceOrFace, pClearColor, ClearZ, ClearStencil, pParameters)
// r4 flags (-> r24), r5 source rect (stack 404), r6 dest texture (-> r17),
// r7 dest point, r8 level, r9 slice, r10 pClearColor (-> r20: a pointer to a
// D3DVECTOR4, NULL = a constant vector; not a packed colour), f1 clear Z
// (fmr f30,f1); ClearStencil / pParameters are stack arguments (the function
// reads one caller-frame word at 468(r1) = frame 368 + 0x64; which of the two
// it is was not resolved, see README.md). D3D writes RB_COPY_CONTROL =
// copy command << 20 | (flags & 0x377) - 0x10 (bit 8 colour clear, bit 9 depth
// clear) and, for 0x200 / 0x100, packs ClearZ / *pClearColor into RB_DEPTH_CLEAR /
// RB_COLOR_CLEAR(_LO) (sub_826F1B78 / sub_826F1FD8): the native resolve reads
// those registers after forwarding (review 2 C1). The caller's LR goes along for
// the per-call-site clear log.
REX_HOOK_RAW(sub_826F21D0)
{
  const pdz::gtsample::Mark sampler_mark(pdz::gtsample::Where::kD3dHook); // pdz_game_thread_profile
  const uint32_t device = ctx.r3.u32, flags = ctx.r4.u32, src_rect = ctx.r5.u32, dest_texture = ctx.r6.u32,
                 dest_point = ctx.r7.u32, level = ctx.r8.u32, slice = ctx.r9.u32, clear_color_ptr = ctx.r10.u32;
  const uint32_t z_bits = FloatArgBits(ctx);
  const uint32_t call_site = uint32_t(ctx.lr); // read before forwarding
  if (pdz::native::recthread::Active())
  {
    // pdz_native_record_thread: the record before the call; after it the registers D3D wrote and the caller's rect,
    // point and colour are copied and the resolve queued.
    if (Enabled())
    {
      pdz::native::recthread::OnResolveEnter(base, device, call_site, flags, src_rect, dest_texture, dest_point,
                                             level, z_bits);
    }
    {
      const pdz::timing::GuestCall guest_call; // pdz_frame_timing_phases: guest.d3d (drawcost)
      __imp__sub_826F21D0(ctx, base);
    }
    pdz::native::recthread::OnResolveLeave(base, device, flags, src_rect, dest_texture, dest_point, level, slice,
                                           clear_color_ptr, z_bits, call_site);
    return;
  }
  if (Enabled())
  {
    pdz::native::RenderLock lock(pdz::native::RenderMutex()); // ring record + native call on one thread
    Count(Hook::kResolve);
    Record(base, ctx.r3.u32, Kind::kResolve, uint32_t(ctx.lr),
           ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, FloatArgBits(ctx));
  }
  {
    const pdz::timing::GuestCall guest_call; // pdz_frame_timing_phases: guest.d3d (drawcost)
    __imp__sub_826F21D0(ctx, base);
  }
  if (pdz::native::Enabled())
  {
    pdz::native::OnResolve(base, device, flags, src_rect, dest_texture, dest_point, level, slice, clear_color_ptr,
                           z_bits, call_site);
  }
}

// Swap(device, pFrontBuffer, pParameters): r4 front buffer texture (the game's
// own LE_X8R8G8B8 texture at [0x82E1C318]), r5 parameters (-> r27). The frame
// marker is recorded before the swap so it belongs to the frame it closes; the
// cold dump policy runs after the original returned (OnSwap).
REX_HOOK_RAW(sub_826DF960)
{
  pdz::gtsample::OnSwapHook(ctx, base); // game_thread_sampler.h: the game thread's context (pdz_game_thread_profile)
  pdz::ipprof::OnSwap(ctx, base);       // guest_ip_profile.h (pdz_guest_ip_profile, off: one cvar read)
  const pdz::gtsample::Mark sampler_mark(pdz::gtsample::Where::kSwapHook);
  // frame_timing.h: closes the frame (pdz_frame_timing; independent of the switches below); the device for the spike
  // line's vblank count.
  pdz::timing::OnSwap(base, ctx.r3.u32);
  const uint32_t device = ctx.r3.u32, front_buffer = ctx.r4.u32;
  const bool sync = pdz::gpusync::Enabled(); // gpu_sync.h: Swap in / out for the sync log (pdz_sync_log, null GPU)
  if (sync)
  {
    pdz::gpusync::OnSwapEnter(base, device);
  }
  pdz::csdump::BeforeSwap(base); // cutscene_dump.h: a cutscene Swap without the cutscene's update (before the record)
  const bool record_thread = pdz::native::recthread::Active(); // pdz_native_record_thread
  if (Enabled() && record_thread)
  {
    pdz::native::recthread::OnSwapEnter(base, ctx.r3.u32, uint32_t(ctx.lr), ctx.r4.u32, ctx.r5.u32);
  }
  else if (Enabled())
  {
    pdz::native::RenderLock lock(pdz::native::RenderMutex()); // ring record + native call on one thread
    Count(Hook::kSwap);
    Record(base, ctx.r3.u32, Kind::kSwap, uint32_t(ctx.lr), ctx.r4.u32, ctx.r5.u32, 0, 0, 0, 0);
  }
  // pdz_frame_timing_phases: the guest's own Swap (ring space, vblank / presentation-interval waits), outside the
  // render mutex: charged with an atomic add. Exclusive since cpusplit: a ring wait, fence block or hooked guest call
  // inside it is charged to its own phase and left out of guest.swap.
  {
    const pdz::timing::ExclusiveScope guest_swap(pdz::timing::Phase::kGuestSwap);
    __imp__sub_826DF960(ctx, base);
  }
  if (sync)
  {
    pdz::gpusync::OnSwapLeave();
  }
  pdz::cpu::OnSwapThread();   // cpu_placement.h: the game thread's CPU-set hint (once; pdz_cpu_placement)
  pdz::latency::OnSwap(base); // latency_log.h: the frame reacting to an input change (pdz_latency_log)
  pdz::csdump::OnSwap(base);  // cutscene_dump.h: Swap count, the frame's row (pdz_cutscene_frame_dump; read-only)
  OnSwap(base);
  pdz::pm4skip::OnSwap(); // pm4_skip.h: pdz_native_skip_pm4 frame counters and mode (one cvar load while off)
  pdz::census::OnSwap(base, device); // gpu_census.h: hooked call totals, periodic line (checks its own switch)
  {
    const pdz::timing::OutsideScope assert_time(pdz::timing::Phase::kShadowAssert); // pdz_frame_timing_phases
    AssertOnSwap(); // shadow_assert.h: per-frame summary
  }
  if (record_thread)
  {
    // Queued after the frame's work; waits for the worker's previous Swap (run-ahead at most one frame).
    pdz::native::recthread::OnSwapLeave(base, device, front_buffer);
  }
  else
  {
    pdz::native::OnSwap(base, device, front_buffer); // native/native.h: present (checks its own switch)
  }
  pdz::test::OnSwap(); // test_harness.h: --pdz_test_exit_s timer
}

// -----------------------------------------------------------------------------
// State setters: counted only. Their effect is in the device struct and the
// draw snapshot carries it (SetTexture -> +12704 slot and the fetch constant at
// +1152+24*sampler; SetRenderTarget -> +12536+4*index; SetDepthStencilSurface
// -> +12552; shaders -> +20456 / +12944; declaration -> +11408; viewport ->
// +12808 and PA_CL_VPORT_* at +11592; scissor -> +12832 and +11524/+11528;
// indices -> +12532; streams -> +12556+8*stream). Forward first or last makes
// no difference for a counter; forwarding last keeps the hooks uniform.
// -----------------------------------------------------------------------------

// SetTexture(device, Sampler, pTexture): r4 sampler, r5 texture.
REX_HOOK_RAW(sub_826ED300)
{
  const pdz::gtsample::Mark sampler_mark(pdz::gtsample::Where::kD3dHook); // pdz_game_thread_profile
  if (Enabled())
  {
    Count(Hook::kSetTexture);
  }
  {
    // cpusplit: the guest setter's own work, guest.d3d at pdz_frame_timing_phases 2 (per call: level 1 leaves it
    // in rest, the sampler's 'd3d hooks' share counts it).
    const pdz::timing::ExclusiveScope guest_d3d(pdz::timing::Phase::kGuestD3D, 2);
    __imp__sub_826ED300(ctx, base);
  }
}

// SetRenderTarget(device, RenderTargetIndex, pRenderTarget): r4 index, r5 surface.
REX_HOOK_RAW(sub_826F0E90)
{
  const pdz::gtsample::Mark sampler_mark(pdz::gtsample::Where::kD3dHook); // pdz_game_thread_profile
  if (Enabled())
  {
    Count(Hook::kSetRenderTarget);
  }
  if (pdz::native::recthread::Active())
  {
    pdz::native::recthread::OnFlushPoint(base, ctx.r3.u32); // the pending batch, in order (pdz_native_record_thread)
  }
  else if (pdz::native::Enabled())
  {
    // A pending BeginVertices batch draws into the targets bound now (native_draw.h FlushPendingBegin).
    pdz::native::RenderLock lock(pdz::native::RenderMutex());
    pdz::native::FlushPendingBegin(base);
  }
  {
    // cpusplit: the guest setter's own work, guest.d3d at pdz_frame_timing_phases 2 (per call: level 1 leaves it
    // in rest, the sampler's 'd3d hooks' share counts it).
    const pdz::timing::ExclusiveScope guest_d3d(pdz::timing::Phase::kGuestD3D, 2);
    __imp__sub_826F0E90(ctx, base);
  }
}

// SetDepthStencilSurface(device, pZStencilSurface): r4 surface.
REX_HOOK_RAW(sub_826F11F8)
{
  const pdz::gtsample::Mark sampler_mark(pdz::gtsample::Where::kD3dHook); // pdz_game_thread_profile
  if (Enabled())
  {
    Count(Hook::kSetDepthStencilSurface);
  }
  if (pdz::native::recthread::Active())
  {
    pdz::native::recthread::OnFlushPoint(base, ctx.r3.u32); // the pending batch, in order (pdz_native_record_thread)
  }
  else if (pdz::native::Enabled())
  {
    pdz::native::RenderLock lock(pdz::native::RenderMutex());
    pdz::native::FlushPendingBegin(base);
  }
  {
    // cpusplit: the guest setter's own work, guest.d3d at pdz_frame_timing_phases 2 (per call: level 1 leaves it
    // in rest, the sampler's 'd3d hooks' share counts it).
    const pdz::timing::ExclusiveScope guest_d3d(pdz::timing::Phase::kGuestD3D, 2);
    __imp__sub_826F11F8(ctx, base);
  }
}

// SetVertexShader(device, pShader): r4 shader (-> r31, refcount at +4 bumped).
REX_HOOK_RAW(sub_826E4378)
{
  const pdz::gtsample::Mark sampler_mark(pdz::gtsample::Where::kD3dHook); // pdz_game_thread_profile
  if (Enabled())
  {
    Count(Hook::kSetVertexShader);
  }
  {
    // cpusplit: the guest setter's own work, guest.d3d at pdz_frame_timing_phases 2 (per call: level 1 leaves it
    // in rest, the sampler's 'd3d hooks' share counts it).
    const pdz::timing::ExclusiveScope guest_d3d(pdz::timing::Phase::kGuestD3D, 2);
    __imp__sub_826E4378(ctx, base);
  }
}

// SetPixelShader(device, pShader): r4 shader (-> r31); also ORs the shader's
// default-constant masks into the PS ALU / bool dirty masks (+24, +32) and
// sets bits 8 and 5 of mask +48 (program reload).
REX_HOOK_RAW(sub_826E3E90)
{
  const pdz::gtsample::Mark sampler_mark(pdz::gtsample::Where::kD3dHook); // pdz_game_thread_profile
  if (Enabled())
  {
    Count(Hook::kSetPixelShader);
  }
  {
    // cpusplit: the guest setter's own work, guest.d3d at pdz_frame_timing_phases 2 (per call: level 1 leaves it
    // in rest, the sampler's 'd3d hooks' share counts it).
    const pdz::timing::ExclusiveScope guest_d3d(pdz::timing::Phase::kGuestD3D, 2);
    __imp__sub_826E3E90(ctx, base);
  }
}

// SetVertexDeclaration(device, pDecl): r4 declaration (stored at +11408).
REX_HOOK_RAW(sub_826E4448)
{
  const pdz::gtsample::Mark sampler_mark(pdz::gtsample::Where::kD3dHook); // pdz_game_thread_profile
  if (Enabled())
  {
    Count(Hook::kSetVertexDeclaration);
  }
  {
    // cpusplit: the guest setter's own work, guest.d3d at pdz_frame_timing_phases 2 (per call: level 1 leaves it
    // in rest, the sampler's 'd3d hooks' share counts it).
    const pdz::timing::ExclusiveScope guest_d3d(pdz::timing::Phase::kGuestD3D, 2);
    __imp__sub_826E4448(ctx, base);
  }
}

// SetViewport(device, const D3DVIEWPORT9*): r4 -> {X, Y, Width, Height, MinZ, MaxZ}.
REX_HOOK_RAW(sub_826F0A70)
{
  const pdz::gtsample::Mark sampler_mark(pdz::gtsample::Where::kD3dHook); // pdz_game_thread_profile
  if (Enabled())
  {
    Count(Hook::kSetViewport);
  }
  {
    // cpusplit: the guest setter's own work, guest.d3d at pdz_frame_timing_phases 2 (per call: level 1 leaves it
    // in rest, the sampler's 'd3d hooks' share counts it).
    const pdz::timing::ExclusiveScope guest_d3d(pdz::timing::Phase::kGuestD3D, 2);
    __imp__sub_826F0A70(ctx, base);
  }
}

// SetScissorRect(device, const RECT*): r4 -> {x1, y1, x2, y2}.
REX_HOOK_RAW(sub_826F0980)
{
  const pdz::gtsample::Mark sampler_mark(pdz::gtsample::Where::kD3dHook); // pdz_game_thread_profile
  if (Enabled())
  {
    Count(Hook::kSetScissorRect);
  }
  {
    // cpusplit: the guest setter's own work, guest.d3d at pdz_frame_timing_phases 2 (per call: level 1 leaves it
    // in rest, the sampler's 'd3d hooks' share counts it).
    const pdz::timing::ExclusiveScope guest_d3d(pdz::timing::Phase::kGuestD3D, 2);
    __imp__sub_826F0980(ctx, base);
  }
}

// SetStreamSource(device, StreamNumber, pStreamData, OffsetInBytes, Stride):
// r4 stream (-> r28), r5 vertex buffer (-> r29), r6 offset (-> r27), r7 stride
// (-> r26). The device slot keeps only {offset, vb}; the stride goes into the
// vfetch instructions when the vertex shader is bound, so it is mirrored here
// (always, one store, so a recorder switched on later still knows it).
REX_HOOK_RAW(sub_826F0CE0)
{
  const pdz::gtsample::Mark sampler_mark(pdz::gtsample::Where::kD3dHook); // pdz_game_thread_profile
  const uint32_t stream = ctx.r4.u32;
  if (stream < dev::kStreamCount)
  {
    Get().stream_stride[stream] = ctx.r7.u32;
  }
  if (Enabled())
  {
    Count(Hook::kSetStreamSource);
  }
  {
    // cpusplit: the guest setter's own work, guest.d3d at pdz_frame_timing_phases 2 (per call: level 1 leaves it
    // in rest, the sampler's 'd3d hooks' share counts it).
    const pdz::timing::ExclusiveScope guest_d3d(pdz::timing::Phase::kGuestD3D, 2);
    __imp__sub_826F0CE0(ctx, base);
  }
}

// SetIndices(device, pIndexData): r4 index buffer (-> r30, stored at +12532).
REX_HOOK_RAW(sub_826F0DD8)
{
  const pdz::gtsample::Mark sampler_mark(pdz::gtsample::Where::kD3dHook); // pdz_game_thread_profile
  if (Enabled())
  {
    Count(Hook::kSetIndices);
  }
  {
    // cpusplit: the guest setter's own work, guest.d3d at pdz_frame_timing_phases 2 (per call: level 1 leaves it
    // in rest, the sampler's 'd3d hooks' share counts it).
    const pdz::timing::ExclusiveScope guest_d3d(pdz::timing::Phase::kGuestD3D, 2);
    __imp__sub_826F0DD8(ctx, base);
  }
}

// -----------------------------------------------------------------------------
// Object creation: ALWAYS recorded (cold, init / level load), so the tables are
// complete even when pdz_render_shadow is switched on later. These three take
// no device pointer.
// -----------------------------------------------------------------------------

// CreateVertexDeclaration(const D3DVERTEXELEMENT9* pElements) -> D3DVertexDeclaration*
// r3 = element table (walked in 12-byte steps to Stream == 0xFF); returns the
// object in r3 (NULL on allocation failure). The game stores r3 straight into
// a global (sub_8261DFC8), so no ppDecl out-parameter exists in this build.
REX_HOOK_RAW(sub_826E44E8)
{
  const uint32_t elements = ctx.r3.u32;
  __imp__sub_826E44E8(ctx, base);
  const auto lock = CreateHookLock();
  Count(Hook::kCreateVertexDeclaration);
  NoteDeclaration(base, ctx.r3.u32, elements);
}

// CreatePixelShader(const DWORD* pFunction) -> D3DPixelShader*
// r3 = shader container (magic 0x102A0E00, header size at +4, microcode size
// at +8); returns the object in r3. r4 is overwritten before any read.
REX_HOOK_RAW(sub_826E3D60)
{
  const uint32_t function = ctx.r3.u32;
  __imp__sub_826E3D60(ctx, base);
  const auto lock = CreateHookLock();
  Count(Hook::kCreatePixelShader);
  NoteShader(base, ctx.r3.u32, function, 0);
}

// CreateVertexShader(const DWORD* pFunction) -> D3DVertexShader*
// Same shape as CreatePixelShader (magic 0x102A0E01).
REX_HOOK_RAW(sub_826E4108)
{
  const uint32_t function = ctx.r3.u32;
  __imp__sub_826E4108(ctx, base);
  const auto lock = CreateHookLock();
  Count(Hook::kCreateVertexShader);
  NoteShader(base, ctx.r3.u32, function, 1);
}
