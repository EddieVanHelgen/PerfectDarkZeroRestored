// Native renderer (stage 1): the texture cache (native_textures.cpp).
//
// Input is the 24-byte texture fetch constant the game wrote into the device struct (+1152 + 24 * sampler,
// big-endian dwords; the shadow DrawRecord carries them as fetch_be). It is decoded with the Xenos layout
// (SDK include\rex\graphics\xenos.h xe_gpu_texture_fetch_t) and served either from the resolve-mirror
// registry (render-target-backed textures: no upload) or from an upload cache keyed by (base address,
// format, size, mips, XXH3 of the first 4 KB) that untiles the guest data on the CPU with the SDK's tiled
// address math (BSD-3, reimplemented) and the fetch constant's endian swap.
#pragma once

#include <cstdint>
#include <d3d12.h>

#include "native_device.h"

namespace pdz::native
{
  // Decoded texture fetch constant (host-order dwords of the guest's 6 big-endian dwords).
  struct FetchDecoded
  {
    uint32_t dwords[6] = {};
    uint32_t type = 0;          // 2 = texture fetch
    uint32_t format = 0;        // xenos::TextureFormat value
    uint32_t endian = 0;        // 0 none, 1 8in16, 2 8in32, 3 16in32
    uint32_t base_address = 0;  // physical, 4 KB aligned (0 = none)
    uint32_t mip_address = 0;   // physical, 4 KB aligned (0 = no mips stored)
    uint32_t width = 1, height = 1, depth = 1; // texels (depth = slices for stacked, 6 for cube)
    uint32_t dimension = 1;     // 0 1D, 1 2D / stacked, 2 3D, 3 cube
    bool stacked = false;
    bool tiled = false;
    bool packed_mips = false;
    uint32_t pitch = 0;         // base level row pitch in texels (multiple of 32)
    uint32_t mip_min_level = 0;
    uint32_t mip_max_level = 0;
    uint32_t swizzle = 0;       // 12 bits: x 0-2, y 3-5, z 6-8, w 9-11 (0..3 = component, 4 = 0, 5 = 1)
    uint32_t signs = 0;         // 4 x 2 bits (0 unsigned, 1 signed, 2 biased, 3 gamma)
    uint32_t num_format = 0;
    int32_t exp_adjust = 0;
    uint32_t clamp_x = 0, clamp_y = 0, clamp_z = 0;
    uint32_t mag_filter = 0, min_filter = 0, mip_filter = 0, aniso_filter = 0;
    uint32_t vol_mag_filter = 0, vol_min_filter = 0;
    int32_t lod_bias = 0;       // 5 fractional bits
    uint32_t border_color = 0;
  };
  FetchDecoded DecodeFetch(const uint8_t *fetch_be24);          // the raw 24 device bytes (big-endian)
  FetchDecoded DecodeFetchDwords(const uint32_t host_dwords[6]); // host-order dwords
  // The 6 dwords of the fetch constant stored in a guest D3DBaseTexture object (object + 16 .. + 39).
  FetchDecoded DecodeTextureObject(const uint8_t *base, uint32_t texture_object);
  // The guest's texture lock (sub_826EC238, gpu_census.cpp's hook; r3 the D3DBaseTexture, r6 the lock flags) returned:
  // the CPU writes the texture next. Routes Textures::NoteCpuWrite inline or through the record thread
  // (pdz_native_texture_lock_check; stage2/polish/textflicker.md). Read-only locks (flags 0x10) are ignored.
  void OnTextureLock(uint8_t *base, uint32_t texture_object, uint32_t flags);
  // pdz_native_texture_prewarm: the game entered a level (`key` names it: Rare's level asset name) or left it (empty).
  // Game thread (game_pace.cpp's level detection); picked up by the record thread's PrewarmStep.
  void OnLevelChange(const char *key);
  // pdz_native_texture_discover: the level's first frame (state 21) after OnLevelChange named it at the start of its
  // loading. Game thread.
  void OnLevelPlaying();

  struct HostTexture
  {
    ID3D12Resource *resource = nullptr;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    DXGI_FORMAT resource_format = DXGI_FORMAT_UNKNOWN;
    uint32_t width = 0, height = 0, depth = 0; // host texels (mirrors: guest * scale)
    uint32_t mips = 1, array_size = 1;
    uint32_t dimension = 1;                    // as FetchDecoded::dimension
    bool mirror = false;                        // resolve destination: render-target-backed, never uploaded
    bool volume_of_array = false;               // Texture3D copy of a 2D / stacked texture (tfetch3D on it)
    uint32_t volume_layers = 1;                 // array layers of the source (depth = layers * replicas)
    D3D12_CPU_DESCRIPTOR_HANDLE rtv{};          // mirrors only
    uint32_t scale = 1;                         // mirrors only
    uint64_t generation = 0;                    // mirrors: bumped by every resolve into it
    uint64_t last_used_frame = 0;
    // pdz_native_texture_watch (overnight 30-31 Oct): armed = the base level's pages are write-watched and nothing
    // wrote them since (generation sum watch_gen); watch_content = the first-4-KB hash it was found under.
    bool watch_armed = false;
    uint8_t watch_stable = 0;
    uint32_t watch_bytes = 0;
    uint64_t watch_gen = 0, watch_content = 0;
    // Split screen (native_draw.h OnResolveInto, stage1/subagents/split.md): the draw path's pass segment whose SRV
    // table last bound this texture (a mirror is stamped too when one of its alias views is bound), and the depth
    // resolves into this mirror within depth_resolve_frame (2 or more = another scene pass resolved the same depth
    // texture: a multi-pass frame). Written by native_draw.cpp only.
    uint64_t table_segment = 0;
    uint64_t depth_resolve_frame = ~0ull;
    uint32_t depth_resolves = 0;
    // Resolve mirrors beyond the exact key (REVIEW.md finding 8). The destination's guest layout (row pitch in
    // texels, tiling, base level bytes) for format / size aliases and address-range overlap; the frame and the
    // first-4-KB guest memory hash of the last resolve into it (GetMirror); stale = an ordinary texture was
    // uploaded over its range since, so lookups ignore it until the next resolve into it (or until the mirror
    // eviction frees it, Textures::EndFrame: a stale mirror unused for 1800 frames goes).
    uint32_t key_pitch = 0, range_bytes = 0;
    bool key_tiled = false;
    bool stale = false;
    uint64_t last_resolve_frame = 0;
    uint64_t resolve_content_hash = 0;
    // Depth mirrors (k_24_8 / k_24_8_FLOAT): a k_8_8_8_8 fetch of the same memory (base, size, pitch, tiling) found no
    // mirror, i.e. the game reads the resolved depth-stencil words as colour (the vision modes' stencil, stage2/polish/
    // rcp90.md). Set by Textures::Lookup; with pdz_native_depth_stencil_alias every later resolve into this mirror also
    // fills the k_8_8_8_8 mirror of that key with the packed words (Surfaces::Resolve).
    bool stencil_alias_wanted = false;
    // Mirror views: a copy of a mirror's top-left region for an alias fetch of another size (same pitch and
    // tiling, so the same texels), re-copied when the source's generation moves on. Freed with their source
    // mirror, or when unused for 1800 frames (Textures::EndFrame); sampling one stamps the source's last_used_frame.
    bool mirror_view = false;
    HostTexture *view_source = nullptr;
    uint64_t view_generation = ~0ull;
    uint64_t upload_frame = 0;                  // uploads: NativeDevice::frame_index() of the upload (draw dump)
    uint64_t upload_bytes = 0;                  // uploads: the bytes untiled (the keep budget, pdz_native_texture_keep_mb)
    // pdz_native_async_upload: the copy queue's fence value of this texture's copy while it may still run (0 = done or
    // uploaded on the graphics list); Lookup makes the graphics queue wait for it.
    uint64_t copy_fence = 0;
    uint64_t host_bytes = 0;                    // the resource's allocation size; filled by the memory log (0 = not yet)
    // Key
    uint32_t base_address = 0, format = 0, key_width = 0, key_height = 0, key_depth = 0, key_mips = 0;
    bool key_signed = false, key_srgb = false;
    uint64_t content_hash = 0;
    // Uploads: the key without the content hash (every fetch field of the key, the view kind, 2D or volume copy): two
    // entries with the same identity are two contents of one guest texture, and only the newest can match guest memory
    // (pdz_native_texture_supersede).
    uint64_t identity = 0;
    uint64_t validated_frame = ~0ull;
    // Whole base-level extent (the key only hashes the first 4 KB): re-checked every kFullCheckFrames frames under
    // a per-frame byte budget, so data written past the first page (a glyph atlas filled after its first use) is
    // re-uploaded instead of sampled stale.
    uint64_t full_hash = 0;
    uint64_t full_check_frame = 0;
    bool full_phase_set = false; // pdz_native_texture_check_stagger: the first cadence re-check moved its phase
    uint32_t full_bytes = 0;
    // pdz_native_texture_volatile_check: re-checked every frame instead of every kFullCheckFrames (linear textures, and
    // textures whose data already changed past the first 4 KB once: video planes rewritten in place by the CPU).
    bool volatile_content = false;
    // pdz_native_texture_lock_check: the game locked guest memory this texture covers (texture lock sub_826EC238, a CPU
    // write: the font atlas gets new glyphs that way) since its content was last compared with guest memory. The next
    // Lookup re-checks the whole base level at once, whatever the cadence and budget above.
    bool lock_dirty = false;
    // The frame of the last such lock: a texture re-checked in that same frame is re-checked once more at its first use
    // in the next one (writes the CPU made after a draw between its lock and the end of its writes).
    uint64_t lock_frame = ~0ull;
    // The spike log's stale-content probe with the check off (Lookup): the frame of the last probe and its result.
    uint64_t stale_probe_frame = ~0ull;
    bool stale_probe = false;
    // Up to four SRV variants (swizzle / signs differ between fetch constants of the same data).
    struct SrvVariant
    {
      uint32_t swizzle = 0;
      D3D12_CPU_DESCRIPTOR_HANDLE srv{};
    };
    SrvVariant srvs[4];
    uint32_t srv_count = 0;
  };

  struct TextureView
  {
    HostTexture *texture = nullptr;        // nullptr = unsupported / no device
    D3D12_CPU_DESCRIPTOR_HANDLE srv{};      // staging SRV with the fetch swizzle applied (CopySrv it)
  };

  class Textures
  {
  public:
    static Textures &Get();

    // The texture a bound sampler's fetch constant refers to, ready for sampling (transitioned to
    // PIXEL|NON_PIXEL shader resource on the open command list). Null view when unsupported (logged once
    // per format), when the fetch constant is not a texture fetch, or when there is no device.
    TextureView Lookup(const FetchDecoded &fetch);

    // The same 2D or stacked texture as a Texture3D, for shaders that sample it with tfetch3D: on Xenos that reads
    // the array slice; a Texture2D(Array) cannot be bound to the translated shader's Texture3D slot (pass dump run7:
    // the Datacore glow particles sample DXN 128x128x16/32 stacked textures this way and stayed invisible). With
    // pdz_native_volume_sdk_layers (default) the copy samples like the SDK's 2D array: one slice per layer for a
    // linear volume filter (lerp between adjacent layers), 8 replicas per layer for a point one, the guest mip chain
    // (a volume mip halves the layer count: one layer of every 2^m); without it, 8 replicas and the base level only.
    // Upload-backed textures only (not resolve mirrors). Details: stage1/subagents/stacked.md.
    TextureView LookupVolume(const FetchDecoded &fetch);

    // The texture Lookup serves (an upload or a resolve mirror) as a Texture2DArray view with every layer and mip: the
    // space3 binding of a tfetch3D slot in pixel shaders patched by pdz_native_stacked_array (native_hlslfix.h; the
    // SDK's k2D binding of a tfetch3D). A stacked texture's own view is one already; a 1D / 2D texture or a mirror gets a
    // one-slice array view, kept with its SRV variants. Null view for a 3D texture, a cube map, or when Lookup has none.
    TextureView LookupArray(const FetchDecoded &fetch);

    // The host mirror of a guest texture used as a resolve destination; created RTV-capable at the host
    // scale on first use (the format follows the guest texture format: 8-bit unorm formats -> R8G8B8A8,
    // float formats -> R16G16B16A16_FLOAT / R16G16_FLOAT / R16_FLOAT / R32_FLOAT, depth formats ->
    // R32_FLOAT). Subsequent Lookups of the same guest texture serve this mirror without any upload.
    HostTexture *GetMirror(const FetchDecoded &fetch);
    // The mirror previously created for the guest texture object (by its fetch constant: exact key base, format,
    // width, height), or nullptr. Stale mirrors (an ordinary texture uploaded over the range since the last
    // resolve) are not returned; GetMirror revives them.
    HostTexture *FindMirror(const FetchDecoded &fetch);

    // Records the barrier to `state` on the open list.
    void Transition(HostTexture &texture, D3D12_RESOURCE_STATES state);

    // Writes a host RGBA8 image (host scale `scale`, point-sampled down) into the guest texture's level-0
    // memory in its own layout (tiling, endian): the de-rez snapshot readback. 8_8_8_8 textures only.
    void WriteGuestLevel0(const FetchDecoded &fetch, const uint8_t *rgba, uint32_t row_pitch_bytes,
                          uint32_t scale);

    // Frame maintenance (called from OnSwap): evicts upload-cache textures unused for a while.
    void EndFrame(uint64_t frame_index);

    // pdz_native_async_upload (record thread, render mutex held): a fetch constant of a draw still in the record
    // thread's queue. When it names an upload texture that isn't cached yet, its upload is prepared on the upload
    // workers; the draw's Lookup then takes it from there. A hint only: nothing changes when it isn't used.
    void Prefetch(const uint8_t *fetch_be24);

    // The game locked a texture for writing (OnTextureLock): every upload-cache entry over [begin, end) or
    // [mip_begin, mip_end) leaves the per-frame fast path and is marked lock_dirty. Called in order with the draws
    // (inline under the render mutex, or as a record-thread command).
    void NoteCpuWrite(uint32_t begin, uint32_t end, uint32_t mip_begin, uint32_t mip_end);

    // pdz_native_texture_prewarm (log 315): the record thread's step (render mutex held, once per command batch):
    // notices a level change, loads that level's texture set from the last run and queues it on the upload workers
    // (entries verified against guest memory; the ones not loaded yet are retried while the level loads), saves the
    // set learned in this level, and writes the level-start summary line 30 s into the level.
    void PrewarmStep();
    // The level change (OnLevelChange's target; any thread).
    void NoteLevelChange(const char *key);
    void NoteLevelPlaying();

    // Memory log (native_memlog.cpp, pdz_native_memlog_s). Cold: walks the caches; a texture's allocation size is
    // asked from the device once (GetResourceAllocationInfo) and kept in HostTexture::host_bytes.
    struct MemoryStats
    {
      uint32_t uploads = 0;       // upload-cache entries, volume copies included
      uint64_t upload_bytes = 0;
      uint32_t volumes = 0;       // ... of which Texture3D copies of 2D / stacked textures (LookupVolume)
      uint64_t volume_bytes = 0;
      uint32_t mirrors = 0, mirrors_stale = 0; // resolve mirrors (render-target-backed)
      uint64_t mirror_bytes = 0;
      uint32_t views = 0;         // alias views of mirrors (region copies)
      uint64_t view_bytes = 0;
      uint32_t srvs = 0;          // staging SRVs the three kinds hold (up to 4 variants each)
      // Upload-cache entries created, evicted (unused for 900 frames) and replaced (changed past their first 4 KB, or
      // an older content of a volatile texture, or of any texture with pdz_native_texture_supersede): the live entries
      // are created - evicted - replaced. `superseded` = the replaced entries only pdz_native_texture_supersede freed.
      uint64_t created = 0, evicted = 0, replaced = 0, superseded = 0;
      uint64_t mirrors_evicted = 0, views_evicted = 0;
      uint64_t log_seen = 0;      // keys in the once-per-texture set of pdz_native_texture_log / _dump_dir
    };
    MemoryStats GetMemoryStats();

    struct Impl; // implementation detail (public for the file-local helpers in native_textures.cpp)

  private:
    Impl *impl_ = nullptr;
    Textures();
  };

  // Sampler state from the fetch constant plus the per-instruction override code of the shader sidecar
  // (XenosRecomp fork, pdzSamplerCode: mag 1:0, min 3:2, mip 5:4 (0 point, 1 linear, 2 base map, 3 fetch),
  // aniso 8:6 (7 fetch), vol mag 10:9, vol min 12:11; 0x1FFF = no override).
  D3D12_SAMPLER_DESC GetSampler(const FetchDecoded &fetch, uint32_t override_code);
  D3D12_SAMPLER_DESC GetSampler(const uint8_t *fetch_be24, uint32_t override_code);

  // The xenos::TextureFormat name ("k_8", "k_8_8_8_8", ...) for logs.
  const char *TextureFormatName(uint32_t format);
} // namespace pdz::native
