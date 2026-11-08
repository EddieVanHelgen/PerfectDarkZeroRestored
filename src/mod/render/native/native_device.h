// Native renderer (stage 1): the shared D3D12 device layer every other native_* file codes against.
// Owned by native_device.cpp (the "renderer core" work); consumed by the shader cache / pipeline / draw code.
// Everything runs on the game thread inside the D3D hooks (src/mod/render/shadow_hooks.cpp records the
// state; the native path consumes the same DrawRecord). The ReXGlue SDK's D3D12 device, queue and presenter are
// reused (rex::ui::Window::presenter(), rex::ui::d3d12::D3D12Provider: GetDevice() / GetDirectQueue()).
//
// The original public API (Get, BeginFrame/EndFrame, Upload, AllocSrv/AllocSampler, AllocRtv/AllocDsv,
// ReleaseLater, WaitIdle) is unchanged; the members after the "additions" marker were added by the core
// work and are documented in CORE_NOTES.md.
#pragma once
#include <cstdint>
#include <d3d12.h>
#include <dxgi1_4.h>

#include "native_reclist.h"

namespace rex::ui::d3d12
{
  class D3D12Provider;
}

namespace pdz::native
{
  // A CPU-writable upload allocation valid for the current frame.
  struct UploadSlice
  {
    void *cpu = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS gpu = 0;
    ID3D12Resource *resource = nullptr;
    uint64_t offset = 0;
    uint32_t size = 0;
  };

  // A shader-visible descriptor (CBV/SRV/UAV or sampler) allocated for the current frame.
  struct Descriptor
  {
    D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE gpu{};
    uint32_t index = 0; // index in the heap (bindless use)
  };

  // pdz_native_gpu_passes (performance pass): the kind of GPU work a pass boundary starts (NativeDevice::GpuMark).
  enum class GpuPass : uint8_t
  {
    kShadowDraw, kShadowClear, kShadowResolve, // the 1344 x 1344 sun shadow maps
    kMainDraw, kMainClear, kMainResolve,       // targets at least the guest back buffer's size
    kOtherDraw, kOtherClear, kOtherResolve,    // smaller targets (post effects, reflections, UI)
    kPresent,                                  // the native present / post-effect chain
    kCount
  };

  class NativeDevice
  {
  public:
    // Lazily created on first use from the SDK's provider; returns nullptr when the SDK has no D3D12 device
    // (another backend) or the creation failed (logged once).
    static NativeDevice *Get();
    // The device if Get() has created it, else nullptr; never creates it. For the paths that run whether or not the
    // native renderer is on (native OnSwap, the draw-stats line, the memory log): creating the device costs its
    // three 64 MB upload buffers and the descriptor heaps.
    static NativeDevice *Existing();

    ID3D12Device *device() const { return device_; }
    ID3D12CommandQueue *queue() const { return queue_; }

    // Frame bracket: BeginFrame opens the frame's command list (waiting on the fence of the frame that used the
    // same slot); EndFrame closes, executes and signals. Three frames in flight.
    void BeginFrame();
    void EndFrame();
    bool in_frame() const { return in_frame_; }
    // Command-list state epoch: changes whenever the list's bound pipeline state / root signature may no longer
    // be the draw path's (BeginFrame, Flush, and any pass that binds its own PSO or root signature calls
    // ListStateChanged). The draw path re-binds its root signature and PSO when the epoch differs.
    uint64_t list_epoch() const { return list_epoch_; }
    void ListStateChanged() { ++list_epoch_; }
    uint64_t frame_index() const { return frame_index_; }
    // The frame's command list as the draw path records into it (native_reclist.h: direct D3D12 calls, or with
    // pdz_native_record_workers 2 a stream another thread replays in order). Null before the device is ready.
    RecList *list() const { return list_ ? &rec_ : nullptr; }
    // pdz_native_record_workers 2: hands the calls recorded so far to the replay thread (the record thread calls it
    // at the end of each batch of commands, RenderMutex held). No-op otherwise.
    void PublishRecording() { rec_.Publish(); }

    // Per-frame linear upload memory (reset at BeginFrame). Alignment defaults to 256 (constant buffers). A frame
    // slot's buffer grows (doubling) when a frame needs more; with pdz_native_upload_shrink it is replaced by a
    // smaller one at BeginFrame once the slot's frames used under a quarter of it for a whole window (REVIEW2.md M1).
    UploadSlice Upload(uint32_t size, uint32_t align = 256);

    // Per-frame shader-visible descriptors: one CBV_SRV_UAV heap and one sampler heap bound for the whole frame
    // (SetDescriptorHeaps is called in BeginFrame). The allocators are linear and reset at BeginFrame.
    Descriptor AllocSrv(uint32_t count = 1);
    Descriptor AllocSampler(uint32_t count = 1);
    // Sampler descriptors that stay valid until freed (the draw path caches sampler tables by content and reuses
    // them across frames). First fit over the region's free ranges; with nothing freed that is the old bump
    // allocation. cpu.ptr == 0 when no free range holds `count` (counted; logged once). pdz_native_sampler_region_limit
    // (test aid) restricts it to the first N descriptors.
    Descriptor AllocPersistentSampler(uint32_t count);
    // Returns a table (Descriptor::index of AllocPersistentSampler, same count) to the region. The descriptors are
    // rewritten by the next table placed there, at once: the caller frees only tables no frame in flight can read
    // (pdz_native_sampler_evict: unused for at least 4 frames; frame F - 3 is complete when frame F begins).
    void FreePersistentSampler(uint32_t heap_index, uint32_t count);
    ID3D12DescriptorHeap *srv_heap() const { return srv_heap_; }
    ID3D12DescriptorHeap *sampler_heap() const { return sampler_heap_; }

    // Non-shader-visible RTV / DSV descriptors (persistent, freed with the surface). ptr == 0 when the heap is full
    // (counted and logged, REVIEW2.md C3): the caller skips its surface / mirror instead of sharing a live slot.
    D3D12_CPU_DESCRIPTOR_HANDLE AllocRtv();
    D3D12_CPU_DESCRIPTOR_HANDLE AllocDsv();

    // Deferred release: the resource is released once the current frame's fence has passed.
    void ReleaseLater(IUnknown *resource);
    // Releases the deferred objects whose fence has passed. BeginFrame does it; native OnSwap also does it while the
    // renderer is off, so a renderer switched off at run time does not hold them (REVIEW2.md M2).
    void ReleaseCompleted();

    // Blocks until every submitted frame has completed (shutdown, readbacks).
    void WaitIdle();

    // pdz_native_gpu_passes: a GPU timestamp in the open list starting a pass of this kind (no-op when off).
    void GpuMark(GpuPass pass);
    bool GpuPassesOn() const;

    // ----- additions (renderer core) ------------------------------------------------------------------

    rex::ui::d3d12::D3D12Provider *provider() const { return provider_; }
    uint32_t srv_descriptor_size() const { return srv_size_; }
    uint32_t sampler_descriptor_size() const { return sampler_size_; }
    uint32_t rtv_descriptor_size() const { return rtv_size_; }

    // Persistent, non-shader-visible CBV/SRV/UAV descriptors ("staging"): the texture cache and the surface
    // registry create their views here once; a draw copies them into its per-frame table with CopySrv.
    // ptr == 0 when the heap is full (counted and logged), like AllocRtv. The Free functions ignore ptr == 0. RTV,
    // DSV and staging descriptors are read when a command is recorded (OMSetRenderTargets, Clear*View, CopySrv),
    // so a slot can be freed at once; the resource itself goes through ReleaseLater.
    D3D12_CPU_DESCRIPTOR_HANDLE AllocStagingSrv();
    void FreeStagingSrv(D3D12_CPU_DESCRIPTOR_HANDLE handle);
    void FreeRtv(D3D12_CPU_DESCRIPTOR_HANDLE handle);
    void FreeDsv(D3D12_CPU_DESCRIPTOR_HANDLE handle);
    // Persistent heap use (for the eviction log lines): live entries and refused allocations of each heap.
    struct PersistentHeapStats
    {
      uint32_t rtv_live = 0, dsv_live = 0, staging_live = 0;
      uint32_t rtv_size = 0, dsv_size = 0, staging_size = 0;
      uint64_t rtv_failures = 0, dsv_failures = 0, staging_failures = 0;
      // Most entries live at once: the bump index (a freed slot is reused before the index moves on).
      uint32_t rtv_peak = 0, dsv_peak = 0, staging_peak = 0;
    };
    PersistentHeapStats GetPersistentHeapStats() const;

    // Memory log (native_memlog.cpp, pdz_native_memlog_s): upload memory, descriptor use and deferred releases.
    struct MemoryStats
    {
      uint64_t upload_bytes = 0;      // the frame slots' upload buffers together (allocated)
      uint64_t upload_in_use = 0;     // bytes the open frame has taken so far (0 between frames)
      uint64_t upload_frame_peak = 0; // the largest frame since the previous GetMemoryStats (which resets it)
      uint64_t upload_grows = 0;      // growth events (a slot's buffer replaced by a larger one)
      uint64_t upload_shrinks = 0;    // pdz_native_upload_shrink: a slot's buffer replaced by a smaller one
      uint32_t srv_ring_size = 0, sampler_ring_size = 0;
      uint64_t srv_in_use = 0, sampler_in_use = 0;    // entries the open frame has taken from each ring
      uint64_t srv_peak = 0, srv_refused = 0;         // shader-visible SRV ring: most one frame used, refusals
      uint64_t sampler_peak = 0, sampler_refused = 0; // sampler ring
      // Content-cached sampler tables: descriptors in tables now (freed by pdz_native_sampler_evict), the region's
      // size, the highest offset ever used, allocations refused (no free range: the table came from the ring) and
      // the free ranges (fragmentation).
      uint32_t sampler_persistent_used = 0, sampler_persistent_size = 0, sampler_persistent_high = 0;
      uint64_t sampler_persistent_refused = 0;
      uint32_t sampler_persistent_ranges = 0;
      PersistentHeapStats heaps;                      // staging SRV / RTV / DSV
      uint64_t pending_releases = 0;                  // ReleaseLater objects waiting for their fence
      uint64_t pending_release_bytes = 0;             // ... their resources' allocation size
    };
    // Resets the frame peak. Cold: walks the pending releases (one GetResourceAllocationInfo each).
    MemoryStats GetMemoryStats();
    // CopyDescriptorsSimple(1, dst, src, CBV_SRV_UAV): dst is a per-frame shader-visible slot (AllocSrv),
    // src a staging descriptor. src.ptr == 0 (a refused staging allocation) writes a null 2D SRV instead.
    void CopySrv(D3D12_CPU_DESCRIPTOR_HANDLE dst, D3D12_CPU_DESCRIPTOR_HANDLE src);
    // Same for sampler descriptors (src: any CPU sampler handle, e.g. from a non-shader-visible heap).
    void CopySampler(D3D12_CPU_DESCRIPTOR_HANDLE dst, D3D12_CPU_DESCRIPTOR_HANDLE src);

    // Mid-frame submit: closes and executes the open list, waits for it, and reopens the list in the same
    // frame slot (used by the de-rez snapshot readback). Costs a GPU round trip; one call per de-rez event.
    void Flush();
    // The same without the CPU wait (pdz_native_async_readback): closes and executes the open list, signals the
    // fence and reopens the list in the same frame slot. Returns the signalled value (0 outside a frame).
    uint64_t Submit();
    // Any thread: waits (on the caller's own event, at most timeout_ms) until the GPU has completed `value`. True
    // when it has. The fence is the queue's one, so `value` also means everything submitted before it is done.
    bool WaitFence(uint64_t value, HANDLE event, uint32_t timeout_ms) const;

    // Fence bookkeeping: the value signalled by the last EndFrame / Flush and the last value the GPU completed.
    uint64_t last_signalled_fence() const { return last_signalled_; }
    uint64_t completed_fence() const;

    // Records a transition barrier when `tracked` differs from `to` and updates `tracked`.
    void Transition(ID3D12Resource *resource, D3D12_RESOURCE_STATES &tracked, D3D12_RESOURCE_STATES to);
    void UavBarrier(ID3D12Resource *resource);

    // Descriptor ring statistics (periodic draw line): peak entries one frame used, and allocations refused.
    void DescriptorStats(uint64_t &srv_peak, uint64_t &srv_failures, uint64_t &sampler_peak,
                         uint64_t &sampler_failures) const;

    // Committed-resource helpers (DEFAULT heap, not zeroed). Return nullptr on failure (logged).
    ID3D12Resource *CreateTexture(const D3D12_RESOURCE_DESC &desc, D3D12_RESOURCE_STATES initial_state,
                                  const D3D12_CLEAR_VALUE *clear_value, const char *debug_name, bool count = true);
    ID3D12Resource *CreateBuffer(uint64_t size, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES initial_state,
                                 D3D12_RESOURCE_FLAGS flags, const char *debug_name);

    // d3d12.dll / d3dcompiler_47.dll entry points fetched at run time (the game executable does not link the
    // import libraries). Null when unavailable.
    using PFN_SerializeRootSignature = HRESULT(WINAPI *)(const D3D12_ROOT_SIGNATURE_DESC *,
                                                         D3D_ROOT_SIGNATURE_VERSION, ID3DBlob **, ID3DBlob **);
    using PFN_Compile = HRESULT(WINAPI *)(LPCVOID, SIZE_T, LPCSTR, const void *, void *, LPCSTR, LPCSTR, UINT,
                                          UINT, ID3DBlob **, ID3DBlob **);
    PFN_SerializeRootSignature serialize_root_signature() const { return serialize_root_signature_; }
    PFN_Compile d3d_compile() const { return d3d_compile_; }

  private:
    NativeDevice() = default;
    bool Init();
    uint64_t SubmitOpenList(bool wait); // Flush (wait) / Submit
    void DrainRecording();              // pdz_native_record_workers 2: replay caught up, held RTV / DSV slots freed
    ID3D12Device *device_ = nullptr;
    ID3D12CommandQueue *queue_ = nullptr;
    ID3D12GraphicsCommandList *list_ = nullptr;
    mutable RecList rec_; // list_ as the draw path sees it
    ID3D12DescriptorHeap *srv_heap_ = nullptr;
    ID3D12DescriptorHeap *sampler_heap_ = nullptr;
    bool in_frame_ = false;
    uint64_t frame_index_ = 0;
    uint64_t list_epoch_ = 0;
    // The rest (allocators, fences, per-frame slots) lives in native_device.cpp's implementation struct.
    struct Impl;
    Impl *impl_ = nullptr;

    rex::ui::d3d12::D3D12Provider *provider_ = nullptr;
    uint32_t srv_size_ = 0, sampler_size_ = 0, rtv_size_ = 0, dsv_size_ = 0;
    uint64_t last_signalled_ = 0;
    PFN_SerializeRootSignature serialize_root_signature_ = nullptr;
    PFN_Compile d3d_compile_ = nullptr;
  };
  // pdz_debug_poison_new_resources (AMD garbage hunt, off): fill new textures / targets with magenta before use.
  bool PoisonNewResources();
  // pdz_debug_gpu_validation (off): the debug layer with GPU-based validation, enabled before the device exists.
  void EnableGpuValidationEarly();
  // One block of opaque magenta in `format` (bytes returned; 0 = format not known) and a fill of `bytes` with it
  // (unknown format: 0xFF bytes).
  uint32_t PoisonPattern(DXGI_FORMAT format, uint8_t out[16]);
  void PoisonFill(uint8_t *dst, uint64_t bytes, DXGI_FORMAT format);

} // namespace pdz::native
