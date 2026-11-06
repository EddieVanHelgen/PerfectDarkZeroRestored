# Native renderer core (stage 1): notes

Branch `render-stage1`, folder `src\mod\render\native`. Shadow mode (RENDER_FEASIBILITY.md 5.2 option C):
the Xenia GPU plugin keeps rendering and presenting; the core runs on the SDK's D3D12 device, queue and
presenter and only reads guest memory and the guest device struct (one documented exception: the de-rez
snapshot write-back). Syntax-checked with `check_syntax.bat` (clang++ -fsyntax-only -std=c++23, the game's
include set and defines, -Wall -Wextra): SYNTAX OK on 2026-10-26. Not built, not run (the orchestrator does).

## Files

| File | What |
|---|---|
| `native.h` | Cvar declarations, `Enabled()`, `Scale()`, guest-memory helpers (`Load32`/`Load16`/`LoadS32`/`LoadFloat` via the shadow's `GuestPtr`; `PhysPtr` = the runtime's physical view for fetch-constant addresses), the entry points the hooks call (`EnsureFrame`, `OnClear`, `OnResolve`, `OnSwap`, `RegisterBinds`, `NativeOutputSelected`, `ToggleNativeOutput`, `PresentPendingFrame`). Includes `../shadow_record.h` read-only for the device offsets. |
| `native_cvars.cpp` | `pdz_native_renderer` (bool, off), `pdz_native_scale` (1..4, latched), `pdz_native_present_mode` (0/1), `pdz_native_output` (bool). |
| `native_device.h/.cpp` | `NativeDevice`: device/queue from `D3D12Provider` (reached through `rex::Runtime::instance()->graphics_system()->provider()`, static_cast after checking `graphics_backend != "vulkan"` and `IsD3D12APIAvailable`, no RTTI in the build); 3 frames in flight (fence + event), per-frame allocator, one command list, per-frame 64 MB upload buffer growing by doubling (and shrinking back after 600 frames of the slot under a quarter used, `pdz_native_upload_shrink`, REVIEW2 M1), shader-visible CBV/SRV/UAV heap 65536 and sampler heap 2048 split into three per-frame linear regions (bound at `BeginFrame`), persistent staging SRV heap (16384, free list), RTV 512 / DSV 256 free-list heaps, deferred release by fence, `Flush` (mid-frame submit + wait), `WaitIdle`, `Transition`, `CreateTexture`/`CreateBuffer`, `D3D12SerializeRootSignature`/`D3DCompile` fetched with GetProcAddress. `EnsureFrame` and `PhysPtr` live here too. |
| `native_passes.h/.cpp` | Run-time HLSL (vs_5_0/ps_5_0/cs_5_0): full-screen blit pass (modes: copy with R/B swap + exponent scale, depth 20e4 encode, depth unorm24 encode; PSO per RTV format) and the gamma compute pass writing the presenter's UAV-only R10G10B10A2 output (or a post-effect intermediate: `GammaDesc::format`, and `fxaa_luma` for the FXAA source, the SDK's `apply_gamma_table_fxaa_luma`). One root signature: root constants b0, SRV table t0-t1, UAV table u0, static point sampler. |
| `native_postfx.h/.cpp` | The GPU plugin's swap post effect (`swap_post_effect`: fxaa / fxaa_extreme / smaa) on the native output chain: `RecordChain` = the gamma pass, then the effect with the SDK's own compiled compute shaders (included from `thirdparty\rexglue-sdk\src\graphics\shaders\bytecode\d3d12_5_1`), its root bindings, static samplers, intermediate formats and SMAA lookup textures (`thirdparty\rexglue-sdk\thirdparty\smaa`). `pdz_native_post_effect` (bool, on): the shown native output applies it. See "Swap post effect" below. |
| `native_textures.h/.cpp` | Fetch-constant decode (`DecodeFetch` from the 24 big-endian device bytes, `DecodeTextureObject` from a guest texture object +16; base / mip addresses by D3D's GPU-address rule, so the 0xE0000000 view is 4 KB up, REVIEW.md 8), 64-entry format table, Tiled2D/Tiled3D and the guest level layout (base pitch, pow2 mips, packed mip tails, 4 KB slice alignment) reimplemented from the SDK (BSD-3), CPU untile with the endian swap and format conversions, upload, cache keyed by (base, format, size, mips, endian, tiled, signed, srgb, XXH3 of the first 4 KB) with a 1024-entry direct-mapped fast path revalidated once per frame, resolve mirrors (render-target-backed guest textures, served without upload; a fetch that misses the exact key is served by a mirror at the same base under a format / size alias, through a region-copy view for another size; a mirror an upload overlaps goes stale, REVIEW.md 8), SRVs with the fetch swizzle composed onto the host format's default swizzle (up to 4 variants per texture), `WriteGuestLevel0` (tiling in reverse, for the snapshot), eviction after 900 unused frames, `GetSampler`. |
| `native_textures.h/.cpp` | Fetch-constant decode (`DecodeFetch` from the 24 big-endian device bytes, `DecodeTextureObject` from a guest texture object +16), 64-entry format table, Tiled2D/Tiled3D and the guest level layout (base pitch, pow2 mips, packed mip tails, 4 KB slice alignment) reimplemented from the SDK (BSD-3), CPU untile with the endian swap and format conversions, upload, cache keyed by (base, format, size, mips, endian, tiled, signed, srgb, XXH3 of the first 4 KB) with a 1024-entry direct-mapped fast path revalidated once per frame, resolve mirrors (render-target-backed guest textures, served without upload), SRVs with the fetch swizzle composed onto the host format's default swizzle (up to 4 variants per texture), `WriteGuestLevel0` (tiling in reverse, for the snapshot), eviction after 900 unused frames, `GetSampler`. Full-extent re-check of the data past the first 4 KB every 8 frames, every frame for volatile textures (`pdz_native_texture_volatile_check`, default on: linear textures and textures that changed past their first page; linear textures hashed over the rows they hold; the dead earlier contents of a volatile texture freed at once; `stage1\subagents\bink.md`). Linear textures without a format conversion are untiled a row at a time. `pdz_native_texture_untile_cached` (default off, stage 2 M5 texstall): the per-block loop builds each row in a cached scratch buffer and copies it into the upload heap once (in place, every block was read back from write-combined memory: ~120-240 ms per MB, the mid-match tex.upload stalls); with the spike log on, a `pdz_native: texture created` line per creation from the level on (reason, layout, hash / create / untile ms, page faults). |
| `native_surfaces.h/.cpp` | Host surface per guest `D3DSurface*` from the object header, `GetBound` (device +12536 / +12552), `BindTargets`, `Clear`, `Resolve`, the snapshot readback, and the `REX_HOOK_RAW(sub_826DE910)` InsertCallback hook. |
| `native_present.cpp` | `OnSwap`: F9 handling, present via `Presenter::RefreshGuestOutput` with the output chain (the gamma pass + the plugin's swap post effect, `native_postfx.cpp`), `Textures::EndFrame`, `EndFrame`; `RegisterBinds`; `REX_HOOK_RAW(sub_826F14A8)` SetGammaRamp -> 3 x 256 u16 ramp -> 256x1 R16G16B16A16_UNORM LUT. With the renderer off `OnSwap` never creates the device (`NativeDevice::Existing`) and only drains the deferred releases of one switched off at run time. |
| `native_memlog.h/.cpp` | `pdz_native_memlog_s` (int, 0 = off): a `pdz_memlog:` line every N seconds at the Swap (process private bytes / working set, `QueryVideoMemoryInfo` local / non-local, upload buffers, descriptor heaps, deferred releases, texture cache, surfaces, geometry cache, PSOs, shader cache, diagnostic buffers) and SUMMARY lines at test exit (first / peak / last, second-half growth). Keys: `PDZRenderResearch\native\stage1\subagents\memlog.md`. |
| `native_watchdog.h/.cpp` | `pdz_native_watchdog_ms` (int, 0 = off): hang watchdog. Marked waits (guest D3D fence block `sub_826DE258` in `gpu_census.cpp`, the three phases of the presenter refresh, `NativeDevice` BeginFrame / Flush / WaitIdle, the render mutex at an output toggle) and Swap gaps over the threshold are logged as `pdz_watchdog:` lines with the native GPU fence, the presenter owner, a pending plugin capture and the device-removed reason; `pdz_native_watchdog_dump_s` writes one minidump of every thread (`logs\hang_*.dmp`). `PDZRenderResearch\native\stage1\subagents\outrace.md`. |
| `check_syntax.bat` | The syntax check (defaults to this worktree). |
| `HOOK_WIRING.md` | The exact call sites for the orchestrator. |

## Guest object layouts used (TU3, read from the generated code)

* Texture object (`D3DBaseTexture`, 40 bytes): +0 Common, +4 RefCount, +8 Fence, +12 ReadFence, **+16..+39 the
  6-dword texture fetch constant** (SetTexture `sub_826ED300` copies +16/+20/+24/+28/+32/+36 into the device's
  fetch slot). The Resolve destination and the Swap front buffer are decoded from there.
* Surface object (`D3DSurface`, 64 bytes, CreateSurface `sub_826ECE78(width, height, format, multisample,
  params)`): +16..+36 a fetch-constant-shaped header: **+20 bits 0..5 = texture format**, **+24 = (width-1) |
  (height-1) << 13**; **+52 = RB_COLOR_INFO** (colour: `ColorRenderTargetFormat` at bits 16..19 from a lookup
  table at 0x820E09A0, EDRAM tile base bits 0..11) or **RB_DEPTH_INFO** (depth: format bit 16, base bits
  0..11); +56 HiZ info. SetRenderTarget copies +52 into device+11460+4*index, SetDepthStencilSurface into
  +11464.
* `RB_COPY_DEST_INFO` = device+11876 (0x231B in the 0x2300 run at +11768): bit 24 `copy_dest_swap`, bits
  16..21 `copy_dest_exp_bias` (signed), bits 7..12 dest format. Written by D3D inside Resolve, hence the
  "call OnResolve after forwarding" rule.

## Surface / format table implemented (native_surfaces.cpp `MapSurfaceFormat`)

| Guest surface (2.4) | Texture format in the header | Host resource | View / SRV |
|---|---|---|---|
| 1280x720 D24FS8 main depth, 1216x640 D24FS8 second depth | 23 `k_24_8_FLOAT` | `R32G8X24_TYPELESS` | DSV / clear value / PSO `D32_FLOAT_S8X24_UINT`; SRV `R32_FLOAT_X8X24_TYPELESS` |
| 1344x1344 D24S8 shadow depth | 22 `k_24_8` | `R24G8_TYPELESS` | DSV / clear value / PSO `D24_UNORM_S8_UINT`; SRV `R24_UNORM_X8_TYPELESS` |
| 1216x640 A8R8G8B8 x2, 256x256 snapshot RT | 6 `k_8_8_8_8` (also 14, 50, 62) | `R8G8B8A8_UNORM` | RTV/SRV same |
| 1216x640 A16B16G16R16_EDRAM HDR scratch | 21 | `R16G16B16A16_FLOAT` (deviation from the UNORM suggestion: the EDRAM 16-bit fixed format holds -32..32; a float host keeps the values without shader-side scaling) | same |
| 1280x640 A2B10G10R10F_EDRAM (7e3) scene | 63 | `R16G16B16A16_FLOAT` | same |
| 1344x1344 R32F shadow colour | 36 `k_32_FLOAT` | `R32_FLOAT` | same |
| (2_10_10_10, 16_16 float/EDRAM) | 7/54, 13/31 | `R10G10B10A2_UNORM`, `R16G16_FLOAT` | same |
| anything else | | `R8G8B8A8_UNORM` (logged once) | |

Host size = guest size x `pdz_native_scale`. Depth surfaces are created cleared to 1.0. EDRAM aliasing is not
modelled: each guest surface is its own host texture; contents cross only via Resolve.

**Depth encoding contract.** The host depth buffers keep float depth (D32) or unorm24 (D24S8) as written by
the pipeline. The 20e4 quantisation of D24FS8 is applied (a) by the pipeline agent in the shader for
polygon-offset draws (feasibility 1.5) and (b) by the depth resolve: `Resolve(4)` into a `k_24_8_FLOAT`
texture runs the blit in mode `kDepth20e4` (float -> 20e4 -> float, truncating like the SDK's default
`depth_float24_round = false`), into a `k_24_8` texture in mode `kDepthUnorm24`. The destination mirror is
`R32_FLOAT`, i.e. **shaders sample resolved depth textures as a float in .r**, exactly what the SDK's
`texture_load_depth_*` produce for `k_24_8(_FLOAT)` (host `R32_FLOAT`, swizzle RRRR). Stencil is never
resolved (PDZ does not sample it).

**Colour resolve contract.** `Resolve(0..3)` blits the bound RT into the destination mirror (host format by
destination texture format: 8-bit unorm formats -> `R8G8B8A8_UNORM`; 16F/32F -> the matching float format;
`2_10_10_10` -> `R10G10B10A2_UNORM`), applying `copy_dest_swap` (EDRAM red lands in data component 2 for
A8R8G8B8-style destinations, so the mirror holds **guest data-component order** like an uploaded texture)
and `2^copy_dest_exp_bias`. **The pipeline must apply `RB_COLOR_INFO.color_exp_bias` on write** (as the SDK
does) so the resolve's un-bias is consistent. Flags: `& 3` RT index, `4` depth, `0x10..0x70` fragments
(ignored, no MSAA), `0x100` clear the colour source, `0x200` clear depth and stencil (the RB_COPY_CONTROL
layout: D3D writes `copy command << 20 | (flags & 0x377) - 0x10`). The clears after the copy follow
RB_COPY_CONTROL (+11864) and the values D3D packed into RB_COLOR_CLEAR / _LO (+11888 / +11892, the source's
RB_COLOR_INFO format) and RB_DEPTH_CLEAR (+11884, depth << 8 | stencil), read after forwarding (review 2 C1;
r10 is `pClearColor`, a pointer, logged only). The cleared area is the resolve area aligned out to 8 pixels.
Rects: `pSourceRect` (RECT) and `pDestPoint` (POINT), big-endian ints; no rect = whole surface. Level/slice
arguments ignored (PDZ resolves level 0 only). A 7e3 source (colour format 3 / 12) is clamped to 0..31.875 with
a 2-bit alpha; with `pdz_native_7e3_resolve_round` (default off) or `pdz_native_7e3_blend 2` the colour is also
rounded to 7e3 (nearest even), what the EDRAM holds: the exposure loop's adapted luminance then stalls within half
a 7e3 step per update of the measured one, as in the emulator (`stage1/subagents/river.md`). The stall shows on
screen only while the tone map's exposure acts: EDCCE21F uses f = clamp(La, c0.x, c0.y) / La with c0 = (0.2, 2.0)
(written after SetPixelShader from 82D25858, static data; c1 / c3 stay the shader's literals), so only dark scenes
(La < 0.2) change. Temple is one: its native frames have the exposed mean luminance pinned at 0.200 (an exact La),
the emulator's 0.22-0.25 (its La stopped 0.03-0.04 under the target), the even 0.83-0.91 darkening
(`stage1/subagents/temple.md`); Datacore and Jungle sit at La 0.27-0.43, exposure off.

**Clear.** `0x1..0x8` RT0..3, `0x10` depth, `0x20` stencil; `count` D3DRECTs or the viewport rect
(device+12808), scaled and clamped; colour from D3DCOLOR ARGB.

**De-rez snapshot.** `Resolve` remembers the frame's last destination; the `sub_826DE910` InsertCallback hook,
before the guest body runs, copies the mirror to a READBACK buffer, `Flush`es (submit + wait), downsamples by
point (`scale`), tiles/endian-swaps into the guest texture's level-0 memory (`WriteGuestLevel0`), then
transitions the mirror back. So the guest RAM holds the native pixels before the callback is queued. In shadow
mode the plugin's `resolve_readback` (pdz_fix_derez_snapshot) later writes the same bytes from the emulated
frame and wins; this is the one place the core writes guest memory. With `pdz_native_async_readback` (null
backend, de-rez callback only; default off) the copy is submitted without the wait and the bytes are written later:
at a native frame end once the GPU has passed the copy, or by the `sub_825F66E0` hook on DeResThread (the CPU
reader) just before it reads them (PDZRenderResearch `stage2\m5\m5_asyncrb.md`).

## Resolution scale (`pdz_native_scale`, 2026-10-26)

`pdz_native_scale` (1..4, default 1, latched by `Scale()` at the first surface) is the SDK's draw resolution
scale for the native path; the emulator's `resolution_scale = 2` (the user's settings) presents 2560x1440, so
`--pdz_native_scale=2` compares like for like. At scale 1 every path below is unchanged (each scale-only step is
skipped by `scale > 1`, `scale >> 1 == 0` or an integer-ratio test). What follows the scale, and where:

| What | Where | Rule (SDK reference) |
|---|---|---|
| Render targets / depth buffers | `Surfaces::Lookup` | host = guest x N (`HostSurface::scale`, `host_width/height`) |
| Resolve mirrors and alias views | `Textures::GetMirror`, `GetMirrorView` | guest x N ("resolution-scaled textures"); sampled with normalized coordinates, so shaders see the same image |
| Viewport, NDC scale / offset, half pixel | `ComputeHostViewport` | `draw_util::GetHostViewportInfo`: snapping and the +0.5 offset in guest pixels, host viewport x N |
| Scissor | `ComputeHostViewport`, clamped in `IssueDraw` | `GetScissor` x N (D3D12 command processor) |
| Clears | `Surfaces::Clear` | rects / viewport rect x N |
| Resolve copy, clear area | `Surfaces::Resolve` | source rect, dest point, size x N; the clear area (8-pixel aligned) x N |
| Resolve half-pixel fill | `Surfaces::Resolve` -> `FillHalfPixelOffset` | `resolve_resolution_scale_fill_half_pixel_offset` (on): with `pix_center == kD3DZero` the first N/2 host columns / rows of the resolve area take the next one's values; `pdz_native_scale_fill_half_pixel` (on) |
| EDRAM inheritance | `Surfaces::InheritEdram` | host-size copy, same scale only |
| Point size | `IssueDraw` system constants | `g_PointSize.zw = 2 N / host viewport` (`point_screen_diameter_to_ndc_radius`) |
| Polygon-offset slope | PSO `SlopeScaledDepthBias`, SV_Depth 20e4 constants | x N (`pipeline_cache`, `edram_poly_offset_*_scale`); the constant offset is not scaled |
| De-rez snapshot | `OnInsertCallback` -> `WriteGuestLevel0` | top-left host sample of each N x N block (the SDK's readback downscale default) |
| Present | `PresentFrame` | the N x mirror; aspect from the guest front buffer |
| Auto-diff | `native_autodiff.cpp` | scored at the host size (2560x1440 against 2560x1440 at N = 2: pixel for pixel, CSV / files / first line); a second log line and the summary add both frames box-filtered to the guest size |
| Pass dump | `native_passdump.cpp`, `tools\passdump.py` | native dumps at host size with `scale`; the emulated side is the SDK's 1x readback (top-left sample per block), and `passdump.py` / `depth_compare.py` take the same top-left sample of the native |

Not scaled, as in the SDK: line width (1 host pixel), sampler LOD (derivatives per host pixel), the integer
depth bias. Approximate or not handled: a fetch with unnormalized coordinates (`unnormalize2D`) on a resolve
mirror divides by the host size (the SDK by the guest size); none in the cache (the 63 are VS lookups on slot
16), detected and logged (`NoteUnnormalizedScaled`). Texel offsets (`tfetch` offsets / `texSize2D`) on a mirror
are host texels, as the SDK's default `draw_resolution_scaled_texture_offsets`. `ps_param_gen` (vPos) is not
emitted by the fork today (no PS reads `SV_Position`); the system constants carry the scale in `g_Reserved.x`
(scale > 1 only) for the pending param-gen fork patch to divide `floor(iPos.xy)` by, as the SDK does. Details and
test runs: `C:\Games\PDZRenderResearch\native\stage1\subagents\scale.md`.

## Swap post effect (`pdz_native_post_effect`, `pdz_native_autodiff_post_effect`, 2026-10-26)

Where each frame is taken, per output stage:

| Stage | Emulated output (SDK `D3D12CommandProcessor::IssueSwap`) | Native output (`PresentFrame`) |
|---|---|---|
| Gamma | `apply_gamma_table` (DC_LUT table) into the presenter's guest output, or into the effect's input | `cs_gamma` (the same table via `gamma_mirror`, c10 for c10 in R10G10B10A2) |
| Swap post effect (`swap_post_effect`, latched at start) | FXAA / FXAA extreme / SMAA compute passes into the guest output | the same passes (`native_postfx.cpp`: the SDK's DXBC, bindings, samplers, formats, AreaTex / SearchTex) while `pdz_native_post_effect` is on (default); off = gamma only |
| Same-frame capture (`CaptureGuestOutput`, sdk-04) | the guest output right after the refresh: **after** the post effect | (the auto-diff reads its own frame, below) |
| Paint (`present_effect` FSR / CAS / bilinear, `present_dither`) | presenter paint, on whatever guest output is current | the same: nothing to mirror, and in neither capture |

The auto-diff pair (`pdz_native_autodiff_post_effect`, hot reload; CSV columns `post_effect_mode`,
`emulated_post_effect`, `native_post_effect` appended after `files`):

* `none` (default): the emulated capture after the plugin's effect against the native front buffer read back and
  gamma'd on the CPU (no effect; the 16-bit ramp truncated to 8 bits, `c10 >> 2`). The scores before 2026-10-26;
  the log line and SUMMARY are unchanged in this mode.
* `after`: both after the effect. The native frame goes through `postfx::RecordChain` with the plugin's effect into
  the auto-diff's own R10G10B10A2 texture, is read back and rounded to 8 bits like the capture
  (`Packed10bpcRGBTo8bpcBytes`): the frame the window shows.
* `before`: both before it. The plugin refreshes the capture swap without its effect
  (`Presenter::SetGuestOutputCaptureBeforePostEffect`, SDK patch `sdk-09`, detected at compile time with a requires
  expression, so the title builds with or without the patch); the native frame through the chain without the
  effect. Without the patch (and an effect configured) it falls back to `after`, logged once;
  `--swap_post_effect=none` gives the same pair with no patch.

Details, numbers and test runs: `C:\Games\PDZRenderResearch\native\stage1\subagents\postfx.md`.

The whole chain checked against IssueSwap (2026-10-26, `stage1\subagents\present.md`): no presenter step is missing.
The front buffer (a k_8_8_8_8 resolve mirror) is read as plain UNORM data like the plugin's `GetDXGIUnormFormat`
(`pdz_native_present_raw_front`, on: clears the fetch signs, a guard for an uploaded gamma-signed front buffer).
PDZ never uses the plugin's PWL ramp (2_10_10_10 front buffers only; logged if one appears). The once-log
`pdz_native: presented front buffer ...` names the front buffer, the view and the gamma table. A darker native
picture is the exposure (river.md, temple.md, mansion2.md). The paint only differs by the guest output size: CAS
downscales the emulator's 2560x1440 in a squared space (+0.5-1 % mean), and FSR upscales a 1280x720 native output.

## Untiling formats covered (CPU, `native_textures.cpp` `kFormats`)

Supported: `k_8`, `k_8_A/B`, `k_8_8`, `k_8_8_8_8` (+`_A`, `_AS_16_16_16_16`, `_GAMMA_EDRAM`), `k_2_10_10_10`
(+AS), `k_1_5_5_5`, `k_5_6_5`, `k_4_4_4_4` (R/B field swap on the CPU like the SDK's load shaders), `k_DXT1`,
`k_DXT2_3`, `k_DXT4_5` (+AS variants) -> BC1/2/3, `k_DXN` -> BC5, `k_DXT5A` -> BC4, `k_16`, `k_16_16`,
`k_16_16_16_16` (unorm, snorm when the read signs are all signed), `k_16(_16(_16_16))_FLOAT` and `_EXPAND`,
`k_32_FLOAT`, `k_32_32_FLOAT`, `k_32_32_32_32_FLOAT`, `k_24_8` and `k_24_8_FLOAT` (converted to `R32_FLOAT`
on upload: unorm24 / 20e4 -> float). Endian 8in16 / 8in32 / 16in32; tiled and linear; 2D, stacked (2D array),
3D (volume, 32x32x4 tiles), cube; mip chains from `mip_address` with pow2 storage sizes and packed mip tails
(SDK `GetPackedMipOffset`); a texture whose level 0 already is the packed tail (smaller side <= 16 texels) has
mip address 0 and its mips in the tail at the base address, which `DecodeFetchDwords` uses as the mip address
like the SDK's `GetSubresourcesFromFetchConstant` (`pdz_native_packed_tail_mips`, on; before, such textures had
no mips); `k_8_8_8_8` with sign `kGamma` uses the `_SRGB` view (an approximation of the
PWL curve). Not supported (logged once, draw gets a null view): `k_1*`, `k_6_5_5`, YUV `_REP`, `k_10_11_11`,
`k_11_11_10`, integer `k_32*`, MPEG/interlaced, `k_DXT3A`, `k_CTX1`, `k_DXT3A_AS_1_1_1_1`, the EDRAM-only
formats as textures. PDZ's package textures are DXT1/3/5, 8888, L8 (`k_8`), 16F/32F; all covered.

## Present strategy and its risk

* Swap is always forwarded (the guest D3D's `VdSwap`, ring kick and the present-interval wait must run; the
  game hangs without them). `pdz::native::OnSwap` then, with the native output selected (F9 /
  `pdz_native_output`), decodes the front buffer texture (`[0x82E1C318]`, LE_X8R8G8B8, r4 of Swap) through the
  texture cache (its resolve mirror, with the fetch swizzle like the plugin's `RequestSwapTexture`) and calls
  `Presenter::RefreshGuestOutput(w, h, guest_w, guest_h, refresher)`; the refresher transitions the
  presenter's UAV output, runs the gamma compute pass with the 256-entry LUT (identity until SetGammaRamp),
  then the plugin's swap post effect (`pdz_native_post_effect`, section "Swap post effect"), transitions back,
  `SetIs8bpc` (true without an effect, as the plugin), and `EndFrame`s (submits) before returning, as the presenter
  requires.
  The D3D12 presenter signals its own refresher completion timeline after the callback.
* **Coexistence / race.** The plugin refreshes the same presenter from its GPU worker thread when it processes
  the swap packet, typically a few ms after our refresh; `RefreshGuestOutput` is not written for two
  concurrent producers (the writable mailbox index is read without a lock) and the window paints whichever
  image was refreshed last. Expected symptom with both running: the picture alternates between native and
  emulated frames, or sticks to one depending on the vblank phase. `pdz_native_present_mode = 1` moves our
  refresh to the next frame's first native call (after the plugin's refresh of the previous frame), which makes
  the choice phase-stable within a session but not predictable. Not forwarding Swap was rejected (ring/vsync
  hang risk). **Recommended stage-2 fix:** one cvar in the SDK's `D3D12CommandProcessor::IssueSwap`
  (`src/graphics/d3d12/command_processor.cpp`, `presenter->RefreshGuestOutput(...)`) to skip the plugin's
  refresh; the user already carries local SDK submodule commits. Until then, side-by-side comparison is best
  done with captured frames (the presenter's `CaptureGuestOutput`, or the snapshot readback path).
* **One producer at a time** (2026-10-26, `stage1\subagents\outrace.md`). The external owner (SDK patches
  sdk-01 / sdk-02) leaves the presenter to the game thread, but the plugin still refreshes it on a same-frame
  capture swap (sdk-04 bypass) and when its refresh passed the owner check just before an owner hand-over (a
  shader-miss frame left to the plugin, F9). Runs 196 / 197: the auto-diff's one capture of the run fell on the
  Datacore level start and the plugin's 2560x1440 capture refresh ran beside our 1280x720 one on the same
  mailbox image: device lost, then a hang. `pdz_native_output_capture` (0 default: no plugin captures while the
  native output is shown; 1: captures, our refresh held while one is pending; 2: the old concurrent behaviour)
  plus SDK patch sdk-11 (a producer mutex inside `RefreshGuestOutput`) for the hand-overs.
* F9: `rex::ui::RegisterBind("bind_native_output", "F9", ...)` when the app calls `RegisterBinds()`; otherwise
  the Swap path polls `GetAsyncKeyState(VK_F9)` edge-triggered (one call per frame, not per draw).
* **Null GPU backend** (stage 2 M1, `--graphics_backend=null`, SDK patch sdk-12, `../gpu_sync.h`). The plugin's
  `D3D12NullGraphicsSystem` / `NullCommandProcessor` consume the ring and service every synchronisation (fences,
  scratch write-back, WAIT_REG_MEM, interrupts, read-pointer write-back, vblank, DC_LUT gamma published through
  `gamma_mirror`) but draw nothing and never refresh the presenter. `pdz::gpusync::NullBackend()` is latched by the
  app's `OnPreSetup` (only when the plugin really created the null backend, `rex/graphics/null_gpu.h`); with it the
  shadow recorder, the native renderer and the native output are on, F9 / `pdz_native_output_toggle_s` are ignored,
  a frame with a shader miss is shown as drawn (counted: SUMMARY "null GPU backend: frames shown with a shader
  miss"), and the auto-diff, pass dump and draw dump refuse to start (one line each at the first native swap). The
  present is unchanged (Swap hook, game thread). `pdz_sync_log` (on by itself here) and the `null_gpu:` SDK lines
  make a hang diagnosable from the log (`stage1\subagents\m1.md` in the research folder).
* **Present parity at the user's settings** (stage 2 M3, `stage1\subagents\m3.md`, SDK patch sdk-13). With the null
  backend the native renderer renders at the GPU plugin's `resolution_scale` (read by name, as the plugin reads it,
  with `draw_resolution_scale_x / _y`; clamped to 1..4, X = Y) unless `pdz_native_scale` is set explicitly:
  `pdz_native_scale_follow` (0 never, 1 null backend only = default, 2 also with the plugin) and
  `ChooseScaleAtStartup()` (app setup, `gpu_sync.cpp` `OnBackendSelected`) latch the scale before any guest thread;
  otherwise `Scale()` latches `pdz_native_scale` at the first surface as before. The swap post effect needs nothing
  new: `PresentFrame`'s chain reads `swap_post_effect` by name whatever the backend. `pdz_native_present_timing`
  (null backend only, default 0 = the Swap hook): 1 stages each frame (chain into one of 4 R10G10B10A2 staging
  textures) and hands it to the presenter when the null command processor reaches its XE_SWAP (the SDK's
  `null_gpu::SetSwapListener`, CP thread, presenter bypass for the owner check, our copy list on the same queue) or
  at its publish when the XE_SWAP came first. In mode 0 `frame_pacing::SetOutputPairing(kNewestVdSwap)` keeps the
  'L' / 'D' latency rows on the right frame. `pdz_frame_timing` has present delay / gap columns, and the test exit
  writes a `pdz_present: SUMMARY` line.

## Stubbed / simplified

* No EDRAM aliasing, no MSAA (none in PDZ), no predicated tiling, no 1D-specific path (1D treated as 2D x 1).
* Resolve: level/slice ignored; stencil never resolved; the resolve clears (0x100 colour, 0x200 depth and
  stencil) need the flag bit and the RB_COPY_CONTROL bit both (a 4x MSAA partial-sample resolve, where D3D
  drops the bits, is not cleared natively; logged).
* Depth clears write the raw float Z (no 20e4 quantisation of the clear value; 0.0 and 1.0 are exact anyway).
* `ClampToHalfway` / `MirrorClampToHalfway` / `MirrorClampToBorder` approximated by CLAMP / MIRROR_ONCE.
* Gamma sign uses sRGB views, not the PWL curve; the pipeline agent can instead linearise in the shader and
  pass an unsigned fetch.
* Signed textures: SNORM view when every component the fetch swizzle reads is signed (SDK SwizzleSigns rule;
  0x55 for any swizzle); mixed signed / unsigned reads, biased signs and `exp_adjust` are not applied (they need
  the fork's tfetch helpers, `stage1/subagents/r2signs.md`) and log one `REVIEW2 C5` warning per combination.
  PDZ's logged levels use only all-unsigned, and all-signed on float formats, with `exp_adjust` 0.
* `pdz_native_scale` is latched at the first surface; changing it needs a restart.
* Textures are untiled on the CPU (one-off per content); the GPU untiler is a later optimisation.
* The upload path trusts the fetch constant's extents against the 512 MB physical view (level skipped with a
  warning if it would exceed it); guest memory protection is not consulted.
* Unsupported texture formats yield a null `TextureView` (the draw agent should bind a null descriptor or
  skip the draw); unmapped surface formats fall back to `R8G8B8A8_UNORM`.
* `NativeDevice` is never destroyed (process lifetime); `WaitIdle` is available for an orderly shutdown.

## What the pipeline/draw agent must call

```cpp
#include "mod/render/native/native.h"
#include "mod/render/native/native_device.h"
#include "mod/render/native/native_surfaces.h"
#include "mod/render/native/native_textures.h"

// At the top of every draw entry point (also opens the frame; false = no device / switch off):
if (!pdz::native::EnsureFrame()) return;
pdz::native::NativeDevice &dev = *pdz::native::NativeDevice::Get();
ID3D12GraphicsCommandList *list = dev.list();            // open for the whole frame, heaps already set

// Render targets of this draw (transitions + OMSetRenderTargets; formats for the PSO key):
pdz::native::Surfaces::Bound bound;
uint32_t rt_count = pdz::native::Surfaces::Get().BindTargets(base, device, &bound);
//   bound.rt[i]->resource_format (DXGI), bound.ds->resource_format; bound.rt[i]->host_width/height, ->scale
//   (viewport/scissor from the device struct x scale); bound.rt[i]->color_format = ColorRenderTargetFormat
//   (apply RB_COLOR_INFO.color_exp_bias on write for the 7e3 target); bound.ds->depth_format (1 = D24FS8).

// Textures of this draw: for each sampler slot the shader uses (fetch constant = record.fetch_be + 24 * slot
// or device+1152+24*slot):
pdz::native::FetchDecoded fetch = pdz::native::DecodeFetch(fetch_be24);
pdz::native::TextureView view = pdz::native::Textures::Get().Lookup(fetch); // transitions to shader resource
pdz::native::Descriptor table = dev.AllocSrv(n);         // per-draw shader-visible table
dev.CopySrv(slot_handle, view.srv);                      // view.texture == nullptr -> unsupported
//   view.texture->dimension (1 = 2D/stacked, 2 = 3D, 3 = cube), ->array_size, ->mips

// Samplers: the sidecar's samplers[].code for the instruction, 0x1FFF = fetch constant only:
D3D12_SAMPLER_DESC sd = pdz::native::GetSampler(fetch, code);
pdz::native::Descriptor s = dev.AllocSampler(1);
dev.device()->CreateSampler(&sd, s.cpu);                 // or dedupe by hash and CopySampler

// Constants / vertex data: dev.Upload(size, 256) -> UploadSlice {cpu, gpu}; one list, no own submission:
// never call EndFrame yourself (OnSwap does; PresentFrame does when presenting).
// Resources created by the agent that must outlive the frame: dev.ReleaseLater(resource) to free them.
```

The DrawRecord's `render_targets[4]` / `depth_stencil` can be passed to `Surfaces::Get().Lookup(base, guest)`
directly when the agent works from the record instead of the device struct.
