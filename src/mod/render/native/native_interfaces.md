# What the pipeline / draw files consume from the other native layers

The renderer-core headers existed by the time the draw path was written, so `native_draw.cpp` codes against them
directly (no placeholder header is left behind). This is the exact surface used, for reconciliation if those
headers change.

## native.h
* `bool Enabled()` - every entry point returns early when off.
* `uint32_t Scale()` - fallback resolution scale when no bound surface reports one.
* `uint8_t *PhysPtr(uint32_t physical_address)` - vertex / index data and the VS microcode copy when the pointer
  is physical (< 0x20000000); `shadow::GuestPtr` for virtual pointers (UP data, ring data, device struct).
* `bool EnsureFrame()` - called at the top of every draw; the draw then requires `NativeDevice::in_frame()` and a
  non-null `list()`.

## native_device.h (`NativeDevice::Get()`)
* `device()`, `list()`, `frame_index()`, `in_frame()`.
* `UploadSlice Upload(size, align)` - constant buffers (256-aligned), UP vertex data and expansion index buffers
  (16-aligned), geometry fallback when the geometry cache is full. Must stay valid until the frame's fence.
* `Descriptor AllocSrv(count)` / `AllocSampler(count)` - per-draw tables (96 SRVs; up to 32 samplers per shader);
  the handles must be in the heaps bound for the frame. The sampler heap is the pressure point (2048 shader-visible
  descriptors): tables are reused by key within a frame, so the per-frame demand is the number of distinct
  (shader, sampler fetch constants) combinations, not the draw count.
* `srv_descriptor_size()`, `sampler_descriptor_size()`.
* `CopySrv(dst, src)` - staging SRV (texture cache) into the per-draw table.
* `CreateBuffer(size, D3D12_HEAP_TYPE_UPLOAD, GENERIC_READ, NONE, name)` - the 32 MB geometry chunks (mapped once,
  never unmapped).
* `serialize_root_signature()` - `D3D12SerializeRootSignature` (version 1.0 desc).
* `d3d_compile()` - `D3DCompile` for the point-expansion GS (`gs_5_0`); null = point draws skipped.

## native_surfaces.h
* `Surfaces::Get().BindTargets(base, device, &bound)` - called once per draw before the PSO lookup: transitions
  the bound surfaces, issues `OMSetRenderTargets`, returns the number of colour slots set; `Bound::rt[i]` /
  `Bound::ds` give `HostSurface*` (null = hole / none).
* `HostSurface::resource_format` -> PSO `RTVFormats` / `DSVFormat`; `depth_format` (1 = D24FS8) -> depth bias
  units; `scale`, `host_width`, `host_height` -> viewport / scissor scaling and clamping.
* Expectation: the surfaces are left in `RENDER_TARGET` / `DEPTH_WRITE` for the draw; a draw skipped after
  `BindTargets` leaves only the barriers behind (harmless).

## native_textures.h
* `FetchDecoded DecodeFetch(const uint8_t *fetch_be24)` on `DrawRecord::fetch_be + 24 * slot` (slots 0..31; VS
  texture fetches use 16..31).
* `Textures::Get().Lookup(fetch)` -> `TextureView { texture, srv }`; `srv` is a staging CPU handle copied with
  `CopySrv`; `texture->dimension` (0 1D, 1 2D / stacked, 2 3D, 3 cube) must match the shader's fetch kind (2D /
  3D / cube -> 1 / 2 / 3) or the slot keeps the null SRV (`DrawStats::texture_kind_mismatch`).
* `D3D12_SAMPLER_DESC GetSampler(const uint8_t *fetch_be24, uint32_t override_code)` - created in place into the
  per-draw sampler table (`ID3D12Device::CreateSampler`), `override_code` = sidecar `samplers[].code`.

## shadow_record.h (read-only)
* `DrawRecord`: `kind`, `a[]`, `device`, `vertex_shader`, `pixel_shader`, `vertex_decl`, `index_buffer`,
  `streams[]` (`vb`, `offset`, `stride`, `base`, `size`), `viewport[6]`, `window_scissor[2]`, `dirty[0..1]`
  (VS / PS ALU masks for the constant-buffer reuse), `regs_be[]`, `fetch_be[]`, `bools_be[]`, `tiling_flag`.
* `FindShader(object)` -> `ShaderEntry` (`hash`, `ucode_src`, `ucode_bytes`).
* Device offsets `dev::kVsAluConstants` (+1920), `dev::kPsAluConstants` (+6016), loop constants at +10144 (own
  constant `kDevLoopConstants`, from PART2_D3D_USAGE: `SetVertexShaderConstantI` 10144 / `SetPixelShaderConstantI`
  10208), `dev::kShaderUcode` (+12), `dev::kVbBase` (+12, also the index buffer's base).

## What the hooks must call (HOOK_WIRING.md owner: orchestrator)
* `pdz::native::OnDraw(base, record)` in the `DrawIndexedVertices`, `DrawVertices`, `DrawVerticesUP`,
  `BeginVertices` hooks (before or after `__imp__`; `BeginVertices` only remembers the record).
* `pdz::native::OnEndVertices(base)` in the `EndVertices` hook (`sub_826E9A50`) after `__imp__`.
* `pdz::native::LogDrawStats("exit")` / `ShaderCache::Get().LogCoverage("exit")` at shutdown if the `atexit`
  hook is not reached (the shader cache registers its own `atexit`).
