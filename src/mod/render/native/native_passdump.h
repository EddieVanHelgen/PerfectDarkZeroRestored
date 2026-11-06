// Native renderer (stage 1): per-pass dump of one frame (native_passdump.cpp), for finding the first pass where
// the native and emulated pictures diverge. --pdz_native_pass_dump=<auto-diff capture #> picks the frame (the
// auto-diff's requested swap); --pdz_native_pass_dump_dir is the output folder. tools\passdump.py turns the raw
// files into PNGs and pairs each native resolve with the emulator's resolve of the same frame.
#pragma once
#include <cstdint>

#include "native_surfaces.h"
#include "native_textures.h"

namespace pdz::native::passdump
{
  // True while the chosen frame is being recorded (cheap).
  bool Active();
  // autodiff::Request: arm for the frame that ends with guest swap `swap` when `capture` is the chosen one. Also
  // arms the SDK's resolve readback dump for the same swap (rex/graphics/resolve_readback.h).
  void MaybeBegin(uint64_t swap, uint32_t capture);
  // At that swap (frame still open): dumps the targets drawn since the last step and closes the index. The readback
  // buffer goes too (it used to be kept for the session after the dump).
  void End();
  // Memory log: the readback buffer's size while one is held (only during a dump since End releases it).
  uint64_t ReadbackBytes();
  // IssueDraw, before anything is recorded for the draw: dumps the previous targets when the bound set changes.
  void BeforeDraw(uint8_t *base, uint32_t device);
  void CountDraw();
  // IssueDraw, for every issued draw of the dump frame: the guest primitive, count and the VS float constants
  // c0..c63 the draw uploads (device shadow), for comparison with the emulator's (SDK SetDrawObserver).
  void RecordDraw(uint32_t sequence, uint32_t prim, uint32_t count, uint64_t vs_hash, const uint8_t *device_vs_constants_be);
  // Surfaces::Clear, before clearing (dumps targets with pending draws; logs the clear).
  void BeforeClear(const Surfaces::Bound &bound, uint32_t flags, uint32_t color, float z, uint32_t stencil);
  // Surfaces::Resolve, before the blit (dumps the source) and after it (dumps the destination mirror and records
  // the destination layout for the emulated side).
  void BeforeResolve(HostSurface &source);
  void AfterResolve(HostSurface &source, HostTexture &dest, const FetchDecoded &dest_fetch, uint32_t flags,
                    int32_t dst_x, int32_t dst_y, int32_t width, int32_t height);
} // namespace pdz::native::passdump
