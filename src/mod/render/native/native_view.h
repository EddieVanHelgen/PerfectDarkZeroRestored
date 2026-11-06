// Native renderer, stage 2 M5 (pdz_native_record_thread, native_recthread.h): the per-command view of guest memory.
//
// With the record thread on, the native entry points run on a worker after the guest's D3D call returned. Guest memory
// the real GPU never reads (the D3D device struct, CPU-side objects, caller memory D3D copies into the ring during the
// call) is copied by the game thread at the hook; the worker sets a View listing those copies before it calls the
// entry point, and the read sites below go through it:
//   device struct (dev_ptr, RT / DS, viewport, resolve registers, +12948)   native_draw.cpp, native_surfaces.cpp,
//   index buffer header, UP vertices / indices                              native_shaders.cpp
//   shader object fields                                                    shadow_record.cpp (FindOrReadShader)
//   surface headers                                                         native_surfaces.cpp (Lookup)
//   texture object fetch dwords                                             native_textures.cpp (DecodeTextureObject)
//   clear rects, resolve rect / point / clear colour                        native_surfaces.cpp
// With no view set (the thread off, or any other thread) every helper reads guest memory exactly as before.
// m5_record_thread.md section 2 lists every read and why the others are safe.
#pragma once
#include <cstdint>

#include "../shadow_record.h"

namespace pdz::native::view
{
  struct Range
  {
    uint32_t guest = 0;          // first guest (virtual) address the copy holds
    uint32_t bytes = 0;          // its length
    const uint8_t *host = nullptr; // the copy (guest byte order)
  };

  struct View
  {
    static constexpr uint32_t kMaxRanges = 32;
    Range ranges[kMaxRanges];
    uint32_t count = 0;
    void Add(uint32_t guest, uint32_t bytes, const uint8_t *host)
    {
      if (count < kMaxRanges && bytes)
      {
        ranges[count++] = {guest, bytes, host};
      }
    }
  };

  // Set by the record thread around each command (nullptr otherwise).
  inline thread_local const View *t_view = nullptr;

  // The copy holding [guest, guest + bytes), or nullptr. Later ranges win (a command's own copies are added after the
  // device mirror).
  inline const uint8_t *Find(uint32_t guest, uint32_t bytes)
  {
    const View *v = t_view;
    if (!v)
    {
      return nullptr;
    }
    for (uint32_t i = v->count; i-- > 0;)
    {
      const Range &r = v->ranges[i];
      if (guest >= r.guest && uint64_t(guest) + bytes <= uint64_t(r.guest) + r.bytes)
      {
        return r.host + (guest - r.guest);
      }
    }
    return nullptr;
  }

  // Big-endian load: the view's copy when it holds the address, else guest memory.
  inline uint32_t Load32(const uint8_t *base, uint32_t guest)
  {
    if (const uint8_t *p = Find(guest, 4))
    {
      return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
    }
    return pdz::shadow::Load32(base, guest);
  }
  inline int32_t LoadS32(const uint8_t *base, uint32_t guest) { return int32_t(Load32(base, guest)); }
  inline float LoadFloat(const uint8_t *base, uint32_t guest) { return pdz::shadow::BitsToFloat(Load32(base, guest)); }

  // Host pointer to `bytes` guest bytes at `guest`: the view's copy when it holds the whole range, else guest memory.
  inline const uint8_t *Ptr(const uint8_t *base, uint32_t guest, uint32_t bytes)
  {
    if (const uint8_t *p = Find(guest, bytes))
    {
      return p;
    }
    return pdz::shadow::GuestPtr(base, guest);
  }
} // namespace pdz::native::view
