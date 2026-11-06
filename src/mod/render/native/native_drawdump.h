// Native renderer (stage 1): per-draw dump of chosen draws (native_drawdump.cpp), the diagnostic for one draw's inputs
// and effect: --pdz_native_draw_dump=<PS hash prefixes> picks the draws, --pdz_native_draw_dump_frames=N how many
// frames (stage1/subagents/bink.md has the exact run).
//
// For every draw whose pixel shader hash starts with one of the prefixes, in each dump frame, one block of
// "pdz_drawdump:" lines: the state the native drew it with (blend, colour mask, viewport / NDC, alpha test), the
// float constants the GPU read (the uploaded constant buffers, compared with the device store), every texture slot
// (fetch constant decoded, host view, frames since its upload and whether the guest memory still matches the
// upload), the samplers and, for DrawVerticesUP / BeginVertices, the first vertices. For one picked draw per frame
// (the pdz_native_draw_dump_pick-th draw matching the FIRST prefix) the files: its textures read back from the GPU
// (exactly what the shader samples, before the SRV swizzle), colour target 0 before it and after it (and after the
// next matching draws, pdz_native_draw_dump_rt), and a .json with every constant. At the swap, the texture memory of
// the logged draws is hashed again: a change means the CPU rewrote the texture after the draw within the frame (the
// emulator samples at GPU time, the native at draw time).
//
// Off (empty prefix list, or every frame dumped) it costs one bool test per draw. The readbacks flush the command
// list (a GPU round trip each): diagnostic runs only.
#pragma once

#include <cstdint>

#include <d3d12.h>

#include "../shadow_record.h"
#include "native_pipeline.h"
#include "native_shaders.h"

namespace pdz::native
{
  struct HostSurface;
}

namespace pdz::native::drawdump
{
  extern bool g_armed;
  // True while a prefix list is configured and frames remain to dump (refreshed by NewFrame; one load per draw).
  inline bool Armed() { return g_armed; }

  // Once per native frame (native_draw.cpp NewFrame): re-reads the cvars.
  void NewFrame();

  // IssueDraw, after the shaders resolved and before anything is recorded for the draw (the same point as the pass
  // dump's BeforeDraw): true when the draw is dumped, then AfterDraw follows once it is recorded. For the picked
  // draw the textures and colour target 0 are read back here (each readback flushes the list).
  bool BeforeDraw(uint8_t *base, const shadow::DrawRecord &r, const NativeShader *vs, const NativeShader *ps);

  // What IssueDraw drew the draw with.
  struct DrawInfo
  {
    const NativeShader *vs = nullptr;
    const NativeShader *ps = nullptr;
    const uint8_t *device = nullptr; // the constant source: device struct or the BeginVertices snapshot
    const uint8_t *vs_cb = nullptr;  // the float constant buffers the GPU reads (upload heap, host order, 4 KB)
    const uint8_t *ps_cb = nullptr;
    bool vs_cb_reused = false;       // no upload for this draw: the buffer of an earlier draw (no dirty bits)
    bool ps_cb_reused = false;
    bool literals = false;           // pdz_native_literals (the buffers carry the shaders' literals)
    uint32_t prim = 0;
    uint32_t guest_count = 0;        // index count (indexed) or vertex count
    uint32_t host_count = 0;         // what the host draw call drew (expanded fans / quads)
    bool user_data = false;          // DrawVerticesUP / BeginVertices: `data` is the guest vertex pointer
    uint32_t data = 0, stride = 0, vertex_count = 0;
    const RenderState *rs = nullptr;
    const HostViewport *hv = nullptr;
    D3D12_RECT scissor{};
    uint32_t alpha_func = 0;         // SystemConstants alpha test (0 = off)
    float alpha_ref = 0.0f;
    const HostSurface *rt0 = nullptr;
    const void *pso = nullptr;
  };
  void AfterDraw(uint8_t *base, const shadow::DrawRecord &r, const DrawInfo &info);

  // native OnSwap (outside any draw): ends a dump frame (the end-of-frame texture re-check and the summary line).
  void OnSwap(uint8_t *base);

  // Memory log: the readback buffer's size while one is held (released once every frame was dumped).
  uint64_t ReadbackBytes();
} // namespace pdz::native::drawdump
