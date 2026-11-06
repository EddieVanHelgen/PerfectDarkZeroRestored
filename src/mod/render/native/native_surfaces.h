// Native renderer (stage 1): one host texture per guest render-target / depth-stencil surface
// (native_surfaces.cpp), keyed by the guest D3DSurface object pointer and created lazily from the object's
// own header (CreateSurface sub_826ECE78 writes a fetch-constant-shaped header: +20 bits 0..5 = texture
// format, +24 = (width-1) | (height-1) << 13; +52 = RB_COLOR_INFO (colour) or RB_DEPTH_INFO (depth):
// format in bits 16.., EDRAM tile base in bits 0..11). EDRAM aliasing is not modelled: contents cross
// surfaces only through Resolve (RENDER_FEASIBILITY.md 2.6).
#pragma once

#include <cstdint>
#include <d3d12.h>

#include "native_device.h"

namespace pdz::native
{
  struct HostTexture;

  struct HostSurface
  {
    uint32_t guest = 0;            // D3DSurface*
    uint32_t width = 0, height = 0; // guest pixels
    uint32_t scale = 1;
    uint32_t host_width = 0, host_height = 0;
    uint32_t tex_format = 0;       // xenos::TextureFormat of the header (+20)
    bool depth = false;
    uint32_t depth_format = 0;     // 0 D24S8, 1 D24FS8 (RB_DEPTH_INFO bit 16)
    uint32_t color_format = 0;     // RB_COLOR_INFO bits 16..19 (xenos::ColorRenderTargetFormat)
    uint32_t edram_base = 0;       // tiles
    uint32_t info_word = 0;        // raw +52
    // Colour: the three formats are the same. Depth: the resource is TYPELESS (R24G8 / R32G8X24) because D3D12 allows
    // a view format other than the resource format only on a TYPELESS resource (REVIEW2.md C4); the DSV, the clear
    // value and the PSO DSVFormat use view_format (D24_UNORM_S8_UINT / D32_FLOAT_S8X24_UINT), the SRV srv_format.
    DXGI_FORMAT resource_format = DXGI_FORMAT_UNKNOWN; // the resource itself (copy compatibility)
    DXGI_FORMAT view_format = DXGI_FORMAT_UNKNOWN;     // RTV (colour) / DSV (depth); PSO RTVFormats / DSVFormat
    DXGI_FORMAT srv_format = DXGI_FORMAT_UNKNOWN;
    ID3D12Resource *resource = nullptr;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    D3D12_CPU_DESCRIPTOR_HANDLE view{}; // RTV (colour) or DSV (depth)
    D3D12_CPU_DESCRIPTOR_HANDLE srv{};  // staging SRV (depth: the depth plane, R32_FLOAT_X8X24 / R24_UNORM_X8)
    uint64_t last_used_frame = 0;       // NativeDevice::frame_index() of the last Lookup / bind (eviction)
    // EDRAM aliasing (Surfaces::InheritEdram): when this surface last became the owner of its EDRAM tiles.
    uint64_t write_stamp = 0;
    uint64_t host_bytes = 0;            // the resource's allocation size; filled by the memory log (0 = not yet)
    // A 7e3 colour surface created with D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS (pdz_native_7e3_blend 2 at its
    // creation): the 7e3 blend variant may bind it as a rasterizer-ordered view (native_blend7e3.h).
    bool uav_7e3 = false;
  };

  class Surfaces
  {
  public:
    static Surfaces &Get();

    // The host surface for a guest D3DSurface*, created on first sight. Null for 0 / non-pointers / failures.
    HostSurface *Lookup(uint8_t *base, uint32_t guest_surface);

    // The targets bound in the device struct (RT[0..3] at +12536, depth-stencil at +12552).
    struct Bound
    {
      HostSurface *rt[4] = {};
      HostSurface *ds = nullptr;
    };
    Bound GetBound(uint8_t *base, uint32_t device);

    // Records the barrier to `state` on the open list (RENDER_TARGET / DEPTH_WRITE for drawing,
    // PIXEL_SHADER_RESOURCE for sampling a resolve source).
    void Transition(HostSurface &surface, D3D12_RESOURCE_STATES state);

    // For the draw code: transitions the bound targets to RENDER_TARGET / DEPTH_WRITE and sets them on the
    // list (OMSetRenderTargets with the RT slots that are bound; a hole leaves a null RTV). Returns the
    // number of colour targets set. Formats for the PSO: HostSurface::view_format of each.
    uint32_t BindTargets(uint8_t *base, uint32_t device, Bound *bound_out);
    // pdz_native_state_filter (stage 2 M5 drawcost3): BindTargets skips OMSetRenderTargets when the list already has the
    // same views of the same resources (same list epoch). Code that sets render targets on the list itself without
    // bumping the list epoch (the draw path's 7e3 ROV binding) calls this. Counters for the draws line.
    void ForgetBoundTargets();
    void TargetFilterCounts(uint64_t &set, uint64_t &skipped) const;

    // EDRAM aliasing: before `surface` is drawn into or cleared, if another surface wrote overlapping EDRAM tiles
    // more recently, the newer content is copied over when both are colour surfaces with the same EDRAM base, the
    // same tile pitch and the same host format (the game's final 1280x720 target shares base 720 with the
    // 1216x640 scene target and expects the scene in it). Other overlaps only move the ownership (counted).
    void InheritEdram(HostSurface &surface);

    // EDRAM aliasing for every user of the bound set (BindTargets, Clear, Resolve; review 2 C2): when the bound
    // set differs from the last one seen, each bound surface runs InheritEdram. A clear or a resolve right after
    // SetRenderTarget, with no draw in between, so sees the content the aliasing surface left in EDRAM.
    void SyncBoundSet(const Bound &bound);

    // native.h entry points (documented there).
    void Clear(uint8_t *base, uint32_t device, uint32_t count, uint32_t rects, uint32_t flags, uint32_t color,
               uint32_t z_bits, uint32_t stencil);
    void Resolve(uint8_t *base, uint32_t device, uint32_t flags, uint32_t src_rect, uint32_t dest_texture,
                 uint32_t dest_point, uint32_t level, uint32_t slice, uint32_t clear_color_ptr, uint32_t z_bits,
                 uint32_t call_site);

    // D3DDevice_InsertCallback (sub_826DE910, hooked in native_surfaces.cpp; callback = r5): if the frame's last
    // resolve wrote a CPU-readable 8_8_8_8 texture (the de-rez snapshot), read the host mirror back and write the
    // guest texture memory before the callback is queued. With pdz_native_async_readback (null backend, the de-rez
    // callback 825F6608) the copy is only submitted here and written later, before DeResThread reads it.
    void OnInsertCallback(uint8_t *base, uint32_t callback);
    // Snapshots written into guest memory so far, sync and async (any thread).
    uint32_t SnapshotReadbacks() const;

    // Frame maintenance at the swap (called from Textures::EndFrame, outside any draw / clear / resolve): deletes
    // the surfaces the game re-created at the same address (their resource was already queued with ReleaseLater)
    // and evicts surfaces no Lookup has touched for kEvictFrames frames (REVIEW2.md C3): their RTV / DSV and staging
    // SRV are freed and the resource is released after the frame's fence. Never the last bound set, and nothing
    // while the pass dump is recording (it keeps surface pointers between steps).
    static constexpr uint64_t kEvictFrames = 1800;
    void EndFrame(uint64_t frame_index);
    // Textures is evicting this resolve mirror: drops the de-rez LastResolve that points at it.
    void ForgetMirror(const HostTexture *mirror);

    // Memory log (native_memlog.cpp, pdz_native_memlog_s). Cold: walks the registry (allocation size asked once per
    // surface and kept in HostSurface::host_bytes).
    struct MemoryStats
    {
      uint32_t colour = 0, depth = 0; // live host surfaces
      uint64_t bytes = 0;
      uint32_t retired = 0;           // re-created this frame, freed at the swap (EndFrame)
      uint64_t evicted = 0;           // totals: evicted unused, retired ones freed
      uint64_t retired_freed = 0;
      uint64_t readback_bytes = 0;    // the de-rez snapshot readback buffers (sync + async; kept once made)
    };
    MemoryStats GetMemoryStats();

  private:
    struct Impl;
    Impl *impl_ = nullptr;
    Surfaces();
  };
} // namespace pdz::native
