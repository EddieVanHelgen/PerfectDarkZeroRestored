# Native renderer stage 1: shader cache, pipelines, draws

Files (all under `src\mod\render\native\`, not yet listed in `CMakeLists.txt`; the orchestrator adds them):

| File | What |
|---|---|
| `native_json.h` | 300-line JSON reader (DOM of numbers / strings / arrays / objects) for the sidecars and the manifest. No new dependency: the SDK's nlohmann copy lives under `thirdparty\inja`, which is not on the game's include set. |
| `native_shaders.h/.cpp` | `ShaderCache`: lazy per-hash load of `<xxh3>.dxil` + `<xxh3>.json` (+ `<xxh3>.hlsl` for literal values), VS resolution by (object, declaration) with the bound-copy rehash, miss dump, coverage counters. Cvars `pdz_native_shader_dir`, `pdz_native_log_frames`, `pdz_native_literals`. |
| `native_pipeline.h/.cpp` | Root signature (the fork's binding model), input layout from the VS sidecar, `DecodeRenderState` (register shadow -> `RenderState`), `PipelineCache::Lookup` (PSO by hash of everything it depends on), point-expansion geometry shader, `ApplyShaderDepthBias()` stub. |
| `native_draw.h/.cpp` | `OnDraw(base, DrawRecord)` / `OnEndVertices(base)`: shader + PSO resolution, constants from the device shadow, vertex / index upload with the Xenos endian swap and a cross-frame geometry cache, descriptor tables, the D3D12 draw. `DrawStats` + `LogDrawStats`. |
| `native_depth20e4.h/.cpp`, `native_depth20e4_hlsl.h` | The SV_Depth 20e4 pixel-shader variant (§6): decision per draw, bias constants, run-time build from the fork's `.hlsl` with the fork's `dxcompiler.dll` / `dxil.dll`, cache per PS, counters. Cvars `pdz_native_depth20e4` (0 off / 1 polygon-offset draws / 2 all depth-tested D24FS8 draws, default 0), `pdz_native_dxc_dir`. |
| `native_coissue.h/.cpp`, `native_coissue_scan.h` | Co-issue fix (§6): at a pixel shader's first draw, scan of its microcode for ALU instructions whose scalar operation reads the vector operation's destination (the fork's HLSL gives it the new value, the hardware and the SDK the old one), patch of the `.hlsl` (scalar line(s) moved above the vector line), compile with the depth20e4 compiler, `<shader dir>\coissue\` cache, counters in the shader coverage line. `native_coissue_scan.h` is standard-library only (offline check against the cache). Cvar `pdz_native_coissue_fix` (default on). |
| `native_cubefix.h` | Cube fix (§6): the Xenos `cube` instruction and tfetchCube as the SDK computes them, a text patch of the fork's `cube()` / `tfetchCubeData()` helpers (and the tfetchCubeLevel call sites), applied with the co-issue / PsParamGen patches at a pixel shader's first draw. Standard-library only (offline check against the cache: 4 shaders patched, all compile). Cvar `pdz_native_cube_fix` (default on). |
| `native_rectlist_gs_dxil.h` | The rectangle-list geometry shader (SDK `kRectangleList` GS rewritten in HLSL, source in the header comment) as signed DXIL. Cvar `pdz_native_rectlist` (default on). |
| `native_drawdump.h/.cpp` | Diagnostic, off by default: `pdz_native_draw_dump=<PS hash prefixes>` logs every matching draw of `pdz_native_draw_dump_frames` frames ("pdz_drawdump:" lines: state, the constant buffers the GPU read against the device store, fetch constants and host views with upload age / staleness, samplers, UP vertices, raw viewport registers) and, for the picked draw, reads back its textures and colour target 0 before / after it (plus after the next matching draws) to `pdz_native_draw_dump_dir`; texture memory re-hashed at the swap (CPU writes after the draw). Hooks: `IssueDraw` (beside the pass dump), `NewFrame`, native `OnSwap`. Run and cvars: `stage1\subagents\bink.md`. |
| `check_syntax_pipeline.bat` | `clang++ -fsyntax-only -std=c++23 -Wall` over the three `.cpp` with the game's include set and defines (from `out\build\win-amd64-release\build.ninja`). Passes. |

Consumed from the renderer-core work: `native.h` (`Enabled`, `Scale`, `PhysPtr`, `EnsureFrame`), `native_device.h`
(`NativeDevice::Get`, `list`, `Upload`, `AllocSrv/AllocSampler`, `CopySrv`, `CreateBuffer`, `serialize_root_signature`,
`d3d_compile`, descriptor sizes), `native_surfaces.h` (`Surfaces::BindTargets`, `HostSurface::resource_format /
depth_format / scale / host_width / host_height`), `native_textures.h` (`DecodeFetch`, `Textures::Lookup` ->
`TextureView::srv` + `HostTexture::dimension`, `GetSampler(fetch_be24, code)` -> `D3D12_SAMPLER_DESC`).
`native_interfaces.md` lists the exact expectations.

## 1. Entry point

```cpp
// native_draw.h
bool pdz::native::OnDraw(uint8_t *base, const pdz::shadow::DrawRecord &r);   // kDrawIndexedVertices, kDrawVertices,
                                                                             // kDrawVerticesUP, kBeginVertices
void pdz::native::OnEndVertices(uint8_t *base);                              // sub_826E9A50 hook, after forwarding
```

* Call `OnDraw` from the four draw hooks with the record `Record()` just filled (`base` = guest memory base);
  before or after forwarding both work: everything the draw needs is in the device struct at hook entry (the
  flush inside the draw only clears dirty masks and rewrites `VGT_INDX_OFFSET` / the stream fetch constants,
  which the record carries from the arguments). It returns true when a native draw was recorded on the device's
  frame list. `Clear` / `Resolve` / `Swap` records return false (surface layer).
* `kBeginVertices`: the game writes the vertices into the returned ring pointer *after* the call, so `OnDraw`
  only remembers the ring record (`r.a[3]` is patched by the hook after forwarding) and `OnEndVertices` issues
  it. Wire `OnEndVertices` into the `sub_826E9A50` hook after `__imp__`.
* `kDrawIndexedVerticesUP` (`sub_826E9E18`, hooked 2026-10-26, stage1/subagents/hud.md): DrawIndexedPrimitiveUP.
  It calls `BeginIndexedVertices` (`sub_826E9AD8`, its only caller: flush, ring space for the vertices
  `[min, min + count)` and the indices, PM4 DRAW_INDX with `VGT_INDX_OFFSET = -min`) and copies both. Record:
  `a0` prim, `a1` min vertex index, `a2` vertex count, `a3` index count, `a4` index pointer, `a5` format (1 =
  32-bit), `a6` vertex pointer, `a7` stride (ninth argument, `[r1 + 84]` at entry). Drawn like a UP draw with
  user indices: big-endian indices swapped into the scratch every draw, uploaded as 32-bit to the frame ring (or
  expanded), vertices `[min index, max index]` from the vertex pointer, base vertex `-min index`. Users: the UI
  shape drawer `sub_827B5890` (shapes with an index array; TRILIST, 16-bit, stride 8, VS B112C5F1) and
  `sub_827B3368`. Before the hook these were the shadow assert's only unpaired flushes (lr 826E9B84): 20 a frame
  in the front end (rings / glow), about 2 a frame in Subway / Arena and 3-4 in Bridge (the framed top bar), none
  in Datacore / Rooftops. `pdz_native_indexed_up` (default on) gates the native draw. They share PSO keys with the
  UI fans and lists, so they rely on the per-draw topology (`pdz_native_draw_topology`, section 4).
* What it reads through `base`: the device struct at `r.device` (`+1920` VS floats, `+6016` PS floats, `+10144`
  the 32 loop constants = registers 0x4908..0x4927; bools come from `r.bools_be`), the VS object's bound microcode
  copy (`[object+12]`), the index buffer object (`+0` flags, `+12` base), vertex data through `PhysPtr` (VB) or
  `GuestPtr` (UP / ring data). All constant offsets settled 2026-10-26 (`shadow_assert.md` 1.3: flush, D3D
  setters, state block capture and the Datacore assert agree).
* Per draw cost: two hash-map lookups (shaders), one PSO lookup (hash of a 100-byte key), `Surfaces::BindTargets`,
  two 4 KB constant swaps (skipped when the ALU dirty masks are clear), 160 + 48 bytes of b1 / b2, descriptor
  table lookups by key (built only on a new (shader, fetch constants) combination per frame), the vertex / index
  range hash (full XXH3 up to 64 KB, sampled above), uploads only on a cache miss. No heap allocation in steady
  state (fixed open-addressing frame tables, stack hash buffers, geometry cache maps reserved and filled only
  on a miss; the quad / fan scratch vector keeps its capacity).
* Counters: `LogDrawStats` every `pdz_native_log_frames` frames (default 600) and the shader cache coverage line
  (`hits / misses / pending | distinct loaded / failed / missed | VS bindings`) on the same cadence and at exit
  (`atexit`).
* Loop diagnostic: for every issued draw whose VS or PS has a loop (sidecar `loop_constants_used`), the loop
  constants it reads are checked as uploaded (device `+10144 + 4i`, or the literal with `pdz_native_literals`);
  a count byte (bits 7:0) of 0 means the loop body never runs. The stats line shows `loop draws N, count-0
  loop Z (VS v PS p)` (a draw may count in both stages); the frame trace prints `pdz_trace:   vs|ps loop iN
  (reg 0x49xx, device +nnnnn) = XXXXXXXX: count c start s step t` per loop, marked `COUNT-0 (loop skipped)`.
  Bit operations only, no allocation.

## 2. Shader cache

Directory: `pdz_native_shader_dir`, else `<exe dir>\native_shaders`; a file not found there is also looked for in
`C:\Games\PDZRenderResearch\native\cache\hlsl` (the research cache: all 2,177 shaders; logged once when used).
Misses are dumped to `<dir>\misses\<hash>.bin` (the microcode as bound: the patched copy for a VS) and logged once
per hash; the draw is skipped and counted.

* **Pixel shaders**: key = `ShaderEntry::hash` from the Create hook (PS microcode is never patched; matches the
  `.xsh` keys).
* **Vertex shaders**: D3D patches the vfetch instructions of the copy at `[object+12]` for the bound declaration
  (STAGE0 §5.4), so the create-time hash is the *unbound* container's. `FindVertex(object, declaration)` keeps a
  binding per (object, decl) pair: it hashes the copy (`PhysPtr` for a physical pointer, `GuestPtr` otherwise;
  `ucode_bytes` from the entry) and loads that hash. While the result is unknown / is the unbound container
  (`vertex_inputs` without `ATTR` semantics or `bound: false`) the binding stays *pending* for up to 8 draws (the
  patch may happen inside the first draw, after the hook ran), then the create-time hash is tried, then it is a
  miss. Resolved bindings are O(1) afterwards.
* **Sibling variants** (cvar `pdz_native_vs_sibling_variants`, on): a bound hash the cache lacks (D3D's copy or the
  binder's output) is first compared with the cached bound variants of the same created VS (`manifest.json`
  `derived_from`, plus those translated this session; microcode from `<dir>\<hash>.bin` or the research cache's
  `ucode\`). If the two differ only in vfetch dword 2 bits 0..30 (stride, offset) and dword 0 bits 27..29
  (prefetch count), and equal / distinct offsets stay so among fetches of one input key, the fork's HLSL is the
  same (offset and stride only reach the sidecar's `offset_dwords` / `stride_dwords`): the sibling's DXIL is used
  with each input's offset / stride read at its `vfetch_address` in the new microcode, at the first draw, with no
  pending draws and no translator run. Offline over the research cache: 624 sibling pairs accepted, every one with
  identical HLSL and the sidecar's own offsets / strides; 378 refused. Facility (log 190): the stride-24 variants
  `6aa9e384bf8a1cc5` (of `77862c6adeea4cb3`, cached at 28 / 32 / 44 / 52 bytes) and `8dfe03a860a4311c` (of
  `48eef7d3421cd5db`, cached at 28) waited 8 draws for D3D's copy and then for the translator (93 / 32 ms):
  23 draws skipped in 8 frames. Log line: `bound variant <h> built from the cached <s>`; coverage line:
  `VS variants built from a cached sibling N (not derivable M)`.
* **No-hole policy** (stage 2 M2; cvars `pdz_native_translate_sync` -1 = with `graphics_backend=null` only,
  `pdz_native_translate_sync_ms` 1500, `pdz_native_translate_sync_frame_ms` 1500): with the plugin a miss shows the
  emulator's frame, with the null backend it is a hole. There the draw that meets an uncached variant waits for the
  translator (the job jumps the queue; `Translator::WaitFor`, a watchdog-marked wait) and is drawn in that frame;
  a new VS variant whose binder output exists is translated at its first draw instead of waiting up to 8 draws for
  D3D's copy (unless the created VS is drawable by itself). A wait that times out is not repeated for that hash
  (its draws are skipped until the files exist, as before); over the frame budget the frame's further misses are
  skipped and waited for in a later frame. Log lines: `translating (...; the draw waits for it)`, `translated
  synchronously, the draw waited N ms`, `translation not done after N ms`; coverage line: `synchronous
  translations: waits W (done, failed, timed out), refused by the frame budget B, waited X ms (longest Y ms)`.
  `pdz_native_shader_fallback_dir` (default the research cache; `none` = off) lets a test run read one folder only.
* **Bound-shader record** (`pdz_native_shader_record`, on): `<shader dir>\record\bound_<time>_<pid>.txt`, one line
  per (created VS, declaration) pair as it settles (`vs <created> <decl> <bound> cache|sibling|translated|created|
  missed <source>`) and per pixel shader at its first lookup (`ps <hash> cache|missing <source>`); a sibling-built
  variant's microcode beside it as `<hash>.bin`. Input of `PDZRenderResearch\native\tools\warm_shader_cache.py`,
  which pre-translates offline what the cache lacks.
* **Sidecar fields used**: `type`, `vertex_inputs[]` (`semantic` ATTR<n>, `bound`, `slot` -> stream `95 - slot`,
  `offset_dwords`, `stride_dwords`, `dxgi_format` by name), `textures[]` (`slot`, `dimension` 2D / 3D / Cube ->
  spaces 0 / 1 / 2), `samplers[]` (`index`, `slot`, `code`), `color_outputs`, `writes_depth`, `uses_kill`,
  `writes_point_size`, `unsupported[]`, `literals[]`, `loop_constants_used[]` (bit mask, the microcode's absolute
  loop index: `[16]` in 183 PS, `[0]` in VS `4e9babb73e5dbfe9`; used only by the loop diagnostic).
* **Literals**: the fork bakes the values into the HLSL (`static const float4 cl<n>`), the sidecar only lists the
  indices. The cache reads the values from a future `literal_values: [{register, value[4]}]` key, else from the
  `.hlsl` next to the `.dxil`; index `n < 256` = float register (PS-relative), `512..513` = the 8 bool dwords,
  `514..521` = the 32 loop dwords (byte offset `0x300 + 16 n` in the GPU constant file; the one PDZ record of
  this kind, `4e9babb73e5dbfe9`, is `i0 = 0x00010002`). A bound VS also gets its parent's records
  (`manifest.json` `derived_from`, looked for in the shader dir and its parent). **Applied only with
  `pdz_native_literals` (default off)**: PART2_D3D_USAGE §169 shows `SetPixelShader` copies the container's
  defaults into the device's constant shadow at bind time, so the shadow already carries them in the right
  order relative to the game's own `Set*ShaderConstantF` writes; re-applying at every draw would shadow a game
  write made after `SetShader` (ORACLE_DIFF matters #1b). Turn it on if a shadow run shows unbaked parent
  literals missing (ORACLE_DIFF #1a).
* **Live literals** (`pdz_native_live_literals`, default on, `native_hlslfix.h` `PatchLiveLiterals`): the HLSL's
  baked `cl<n>` are the values of the one container the cache entry was built from, but the cache key is the
  microcode alone, and PDZ ships 61 pixel shaders in containers whose literals differ (no vertex shader): the
  material compiler's per-material values, above all the parallax scale c1.w of the G-buffer / material shaders
  (a9204a6c: 0.01-0.1, 35e06528: 0.01-0.1, 4b1fe1c7 / c127b524 / be6b8b23 ...). The draw dump shows the device
  shadow holding the bound object's value (Mansion f5800, log 282: c1.w 0.02 / 0.025 / 0.05 / 0.1 for PS
  35e06528, baked 0.1). At a pixel shader's first use each `static const float4 cl<n>` with n < 224 becomes
  `#define cl<n> c[<n>]`, so the shader reads the uploaded shadow as the emulator reads the register file (with the
  other first-use patches, cached in `<shader dir>\coissue`; offline: 1,154 of the 1,200 cached PS patched, all
  compile with dxc). With `pdz_native_literals` on, the upload carries the baked values again (the old result).
  stage1\subagents\temple2.md.

## 3. Binding model as implemented

| Root parameter | Register | Visibility | Content |
|---|---|---|---|
| 0 `kRootCbvFloatVs` | `b0 space0` | VS | `float4 c[256]`: device `+1920`, 256 float4, byte-swapped |
| 1 `kRootCbvFloatPs` | `b0 space0` | PS | `float4 c[256]`: device `+6016`, first 224 float4 (hardware c256..c479), rest 0 |
| 2 `kRootCbvBoolLoop` | `b1 space0` | all | `uint4 g_BoolBits[2]` = registers 0x4900..0x4907 = `+10112` (dwords 0..3 VS b0..127 at `+10112`, 4..7 PS b0..127 = b128..255 at `+10128`; bool n = bit `n & 31` of dword `n >> 5`); `uint4 g_LoopConstants[8]` = registers 0x4908..0x4927 = `+10144 + 4i` for loop index i (VS i0..15 = 0..15 at `+10144`, PS i0..15 = 16..31 at `+10208`; the microcode's loop_id is the absolute index, `loopConst(16u)` in PS). Same 40-dword layout as the SDK's bool / loop cbuffer (`command_processor.cpp` copies registers 0x4900..0x4927) |
| 3 `kRootCbvSystem` | `b2 space0` | all | `SystemConstants` (below) |
| 4 `kRootSrvTable` | `t0..t31 space0/1/2` | all | 96 descriptors: `[dim * 32 + slot]`, dim 0 = `Texture2D`, 1 = `Texture3D`, 2 = `TextureCube`; PS slots 0..15, VS slots 16..31 share the table |
| 5 `kRootSamplerVs` | `s0..s31 space0` | VS | the VS sidecar's sampler variants by `index` |
| 6 `kRootSamplerPs` | `s0..s31 space0` | PS | the PS sidecar's sampler variants by `index` |
| 7 `kRootConstDepth` | `b3 space0` | PS | 4 root constants, only read by the SV_Depth 20e4 variants: front (slope scale per host pixel, offset), back (same); set per draw when `Pipeline::shader_depth_bias` |

Root signature 1.0 (volatile descriptors), `ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT`, serialised through the device's
`D3D12SerializeRootSignature` pointer. Both `b0` parameters are root CBVs (GPU virtual addresses from the frame
upload ring); the two sampler tables are separate because each shader numbers its own variants from 0.

Tables are pre-filled from two non-shader-visible "null blocks" (96 null SRVs of the right view dimensions, 32
default samplers) with one `CopyDescriptorsSimple`, then the referenced slots are overwritten: SRVs copied from
the texture cache's staging descriptors (`Textures::Lookup(DecodeFetch(fetch_be + 24 * slot)).srv`), samplers
created in place from `GetSampler(fetch_be, code)`. A texture whose kind does not match the shader's fetch (a
stacked texture under `tfetch3D`, say) stays null and is counted (`texture_kind_mismatch`). Tables are reused
within a frame by key (shader hashes + the raw fetch constants of the referenced slots).

`SystemConstants` (`b2`): `g_HalfPixelOffset = 0` (see §6), `g_AlphaRef = RB_ALPHA_REF`, `g_AlphaFunc =
RB_COLORCONTROL.alpha_test_enable ? alpha_func + 1 : 0` (D3DCMPFUNC = xenos CompareFunction + 1), with
alpha-to-mask folded in when `alpha_to_mask_enable` is set (`AlphaTestFor`: the SDK's one-sample test
`oC0.w >= 1 - offset / 4`, one threshold per draw = the mean of the four quad offsets; cvar
`pdz_native_alpha_to_mask`, on),
`g_PointSize = (PA_SU_POINT_SIZE.width / 16, height / 16, 2 * scale / host viewport width, 2 * scale / host
viewport height)` (half sizes in guest pixels; `.zw` are this renderer's addition for the geometry shader, the game
HLSL reads `.x` at most), `g_Flags` = the VTE format bits (`pdzHostPosition`), `g_NdcScale` / `g_NdcOffset` from
`ComputeHostViewport`, `g_Reserved.x` = the resolution scale when `pdz_native_scale` > 1 (0 at scale 1; no shader
reads it yet: it is there for a translated PsParamGen position, `floor(iPos.xy) / scale` like the SDK).

Input layout: one `D3D12_INPUT_ELEMENT_DESC` per sidecar input: `"ATTR"`, `attr`, `dxgi_format`,
`InputSlot = 95 - slot` (D3D stream index), `AlignedByteOffset = offset_dwords * 4`, per-vertex. The IA stride is
the stream's `SetStreamSource` stride mirrored by the hook (`Stream::stride`), falling back to the sidecar's
`stride_dwords * 4`. `2_10_10_10` arrives as `R32_UINT` and is decoded in the HLSL (the only such format in PDZ;
`10_11_11` / `11_11_10` would come the same way). Shaders with `UNDEFINED` / declaration-usage inputs (unbound
container VS) are never drawn with (`inputs_unbound`).

## 4. Register -> PSO decoding

PSO key (hashed with XXH3): VS hash, PS hash, input layout hash, `RB_DEPTHCONTROL`, `RB_BLENDCONTROL0..3`,
`RB_COLOR_MASK`, stencil read / write masks, cull / face / fill / points bits, integer depth bias, slope bias,
topology type, RT count + 4 RT formats, DS format. Failures are remembered per key and logged once.
The key holds the topology TYPE, not the primitive type: TRILIST / TRISTRIP / TRIANGLEFAN / QUADLIST draws with
the same shaders and state share a PSO. The host topology (`IASetPrimitiveTopology`) and the fan / quad index
expansion therefore come from each draw's own primitive type (`DrawTopology` in native_draw.cpp, cvar
`pdz_native_draw_topology`, default on), as the SDK does, never from the draw that created the PSO (that drew the
Rooftops radar ring strips as fans of spikes; stage1/subagents/radar.md).

| Register | Field | D3D12 |
|---|---|---|
| `RB_DEPTHCONTROL` 0x2200 | `z_enable` | `DepthEnable` (only with a DSV bound) |
| | `z_write_enable` | `DepthWriteMask` ALL / ZERO |
| | `zfunc` | `DepthFunc = NEVER + zfunc` (same order) |
| | `stencil_enable` | `StencilEnable` (only with a DSV bound); `OMSetStencilRef(RB_STENCILREFMASK.stencilref)` |
| | `stencilfunc / fail / zfail / zpass` | `FrontFace` (`KEEP + op`, `NEVER + func`, same orders) |
| | `backface_enable` + `*_bf` | `BackFace`, else `BackFace = FrontFace` |
| `RB_STENCILREFMASK` 0x210D | `stencilmask`, `stencilwritemask` | `StencilReadMask`, `StencilWriteMask` (D3D12 has no per-face masks; `_BF` ignored) |
| `RB_BLENDCONTROL0..3` 0x2201 / 0x2209..B | `color_srcblend / destblend / comb_fcn`, `alpha_*` | per-RT `SrcBlend / DestBlend / BlendOp`, alpha variants (colour factors mapped to their alpha forms for the alpha channel); `BlendEnable` unless (ONE, ZERO, ADD) on both; constant factors -> `BLEND_FACTOR` (+ `OMSetBlendFactor(RB_BLEND_RED..ALPHA)`); `ALPHA_FACTOR` falls back to `BLEND_FACTOR` like the SDK |
| `RB_COLOR_MASK` 0x2104 | nibble per RT | `RenderTargetWriteMask` (same bit order R G B A) |
| `PA_SU_SC_MODE_CNTL` 0x2205 | `cull_front / cull_back` | `CullMode` FRONT / BACK / NONE (triangles only; lines and points NONE) |
| | `face` | `FrontCounterClockwise = (face == 0)` |
| | `poly_mode == kDualMode` and the surviving side's `ptype != triangles` | `FillMode = WIREFRAME` |
| | `poly_offset_front/back_enable` | bias of the side that is drawn (front unless front faces are culled) |
| `PA_SU_POLY_OFFSET_FRONT/BACK_OFFSET` 0x2381 / 0x2383 | | `DepthBias = GetD3D10IntegerPolygonOffset(depth format, offset)` (SDK formula: `ceil(|offset| * (float24 ? 2^24 / 8 : 2^24 - 1))`, `<< 3` for float24, signed) |
| `PA_SU_POLY_OFFSET_FRONT/BACK_SCALE` 0x2380 / 0x2382 | | `SlopeScaledDepthBias = scale / 16 * resolution scale` |
| `RB_COLORCONTROL` 0x2202 | `alpha_test_enable`, `alpha_func`, `alpha_to_mask_enable`, `alpha_to_mask_offset0..3` | NOT a PSO field: `b2 g_AlphaFunc` (the fork's `alphaTest()` runs at every PS end); alpha-to-mask = an alpha test (`AlphaTestFor`), `AlphaToCoverageEnable` stays `FALSE` (one sample) |
| `RB_ALPHA_REF` 0x210E | | `b2 g_AlphaRef` |
| `PA_SU_POINT_SIZE` 0x2280 | `width`, `height` (12.4, half size) | `b2 g_PointSize.xy` |
| `PA_SU_VTX_CNTL` 0x2302 | `pix_center` | viewport shift (§6) |
| `RB_DEPTH_INFO` 0x2002 | `depth_format` | bias units (the surface layer reports `depth_format` per DS surface; used from there) |
| draw argument | `D3DPRIMITIVETYPE` | 1 POINTLIST -> POINT + GS, 2 LINELIST, 3 LINESTRIP, 4 TRILIST, 6 TRISTRIP native; 5 TRIANGLEFAN and 13 QUADLIST -> 32-bit triangle-list indices generated per draw; 8 RECTLIST -> triangle list + rectangle-list GS (`pdz_native_rectlist`); the rest skipped + counted |

Other PSO fields: `DepthClipEnable = TRUE`, no MSAA (`SampleDesc 1`), `IndependentBlendEnable = TRUE`,
`NumRenderTargets` = colour targets bound (holes = `UNKNOWN`), `DSVFormat` from the bound depth surface.

## 5. Vertex / index upload and the endian swap

The fork's HLSL reads typed input-assembler attributes, so the guest data is converted on upload.

* **Swap rule** (verified against the SDK's `dxbc_translator_fetch.cpp`): the Xenos vertex fetch applies the
  *fetch constant's* endian field to each 32-bit word (`8in32` = reverse the 4 bytes, `8in16` = swap within
  each half, `16in32` = swap halves, `none`) and then unpacks the format from the little-endian dword with x in
  the low bits (`16_16`: x = bits 15:0; `8_8_8_8`: x = bits 7:0; `2_10_10_10`: x = bits 9:0). That is exactly
  the DXGI layout of `R16G16_*`, `R8G8B8A8_*`, `R32_UINT`, so the CPU swap depends only on the stream's endian
  mode, **not** on the element formats (the "swap 2 for 16-bit elements" rule would be wrong under `8in32`).
* **Where the endian comes from**: the D3D vertex buffer object stores the GPU fetch dwords: `VB+12` = byte
  address | type bits (1:0), `VB+16` = byte size | endian bits (1:0); `sub_826F9578` adds the `SetStreamSource`
  offset to both. So per stream: `address = (base & 0x1FFFFFFC) + offset`, `endian = size & 3`,
  `bytes = (size & 0x03FFFFFC) - offset`. The first endian value seen is logged; a type field other than 3 is
  logged once. `DrawVerticesUP` / `BeginVertices` data (CPU-written, stream 0) is swapped `8in32`.
* **Range**: only the vertices the draw can touch are uploaded: non-indexed `[start, start + count)`, indexed
  `[base + min index, base + max index]` (the min / max come from the index swap pass) with
  `BaseVertexLocation = -min`. Index buffers: 16-bit (`DrawIndexedVertices` adds `start * 2` to `IB+12`; the
  only PDZ shape), swapped `8in16`; `IB+0` bit 31 is logged once if set (unknown flag).
* **Geometry cache** (`GeometryCache`): persistent 32 MB upload-heap chunks (up to 8), linear suballocation,
  entries keyed by (guest address of the range, byte length, index flag) and validated by a content hash:
  full XXH3 up to 64 KB, above that head + tail 4 KB and 256 bytes every 64 KB. A chunk is recycled whole once
  every frame that used it has retired (frame index + 3). When no chunk is free the data goes through the
  frame upload (`uncached_fallbacks`). Known limit: a dynamic VB rewritten in place with identical sampled bytes
  would be served stale; a `VB/IB Unlock` hook (`826E1CF0`) invalidating the range is the proper fix (not
  wired tonight).
* **Constants**: `+1920` / `+6016` are byte-swapped into a 4 KB frame upload slice per draw unless the record's
  dirty mask 1 / 2 is zero and the shader is unchanged (then the previous slice is rebound: `constant_reuse`).

## 6. Conventions and stubs

* **Half-pixel offset**: `g_HalfPixelOffset = 0`. Instead, when `PA_SU_VTX_CNTL.pix_center == kD3DZero`,
  `ComputeHostViewport` adds 0.5 guest pixels to the viewport offset before the integer snapping, as the SDK's
  `GetHostViewportInfo` does (`offset_add_xy += 0.5`); it reaches the vertices through `ndc_offset`
  (`pdzHostPosition`). With `pdz_native_scale` N > 1 the half pixel stays half a guest pixel (N / 2 host pixels),
  and resolves get the SDK's half-pixel fill (CORE_NOTES.md "Resolution scale").
* **Viewport / scissor**: `PA_CL_VTE_CNTL`, `PA_CL_VPORT_*`, window offset and `PA_SC_WINDOW_SCISSOR_TL/BR` /
  screen scissor (`GetHostViewportInfo` / `GetScissor`), computed in guest pixels and multiplied by the surface
  scale (`TargetFormats::resolution_scale` = `HostSurface::scale`); the scissor clamped to the host target.
* **Unnormalized texel coordinates at `pdz_native_scale` > 1**: the fork's `unnormalize2D` / `3D` divide by the
  bound view's `GetDimensions`, the host size of a resolve mirror (the SDK divides by the guest size). The sidecar's
  `texture_fetches[].unnormalized` slots (`NativeShader::unnormalized_slots`) are checked when a table binds a
  mirror; such a binding is logged once per shader and slot (`NoteUnnormalizedScaled`), not corrected. In the
  cache only 63 VS use them (slot 16 lookups). Texel offsets (`tfetch2D(..., offset)` = `offset / texSize2D`) on
  a mirror come out in host texels, which is the SDK's default `draw_resolution_scaled_texture_offsets`.
* **Point expansion**: a geometry shader (`kPointGsSource`, `gs_5_0` via `d3dcompiler_47` through
  `NativeDevice::d3d_compile()`) turns each point into a quad of `g_PointSize.xy` guest pixels (`.zw` convert to
  NDC, times `pos.w`), copying `SV_Position` + `TEXCOORD0..15`; point PSOs use `CullMode NONE`. Not reproduced:
  `PA_SU_POINT_MINMAX` clamping, per-vertex point size (`oPts`, one VS), and param_gen sprite coordinates (no
  PDZ PS reads them). If the GS fails to compile or the DXBC GS cannot be combined with the DXIL VS / PS in one
  PSO, point draws are skipped and counted (`pso_failures`), logged once.
* **SV_Depth 20e4 variant** (`ApplyShaderDepthBias()`, `native_depth20e4.h`, cvar `pdz_native_depth20e4`,
  default 0 = off = the fixed-function D3D12 bias in `GetD3D10IntegerPolygonOffset` units, unchanged). On: draws
  with a D24FS8 depth buffer and a depth test (mode 1: only those with a polygon offset on some face; mode 2: all)
  use a variant of their PS that writes `SV_Depth` the way the SDK's ROV path computes guest depth: coarse
  `ddx/ddy(SV_Position.z)` before the guest code, `bias = max(|ddx|, |ddy|) * scale / 16 * resolution scale +
  offset` of the face (`SV_IsFrontFace`; lines / points: front registers via `poly_offset_para_enable`),
  `saturate`, round to the nearest even 20e4, back to float32 exactly; a PS that writes depth only gets the
  rounding. Draws without a PS get a depth-only shader (colour writes masked). The variant is built at run time
  from the fork's `.hlsl` (guest `main` renamed `pdzGuestMain`, a new `main` with the same parameters + `SV_Depth`
  calls it) with the fork's `dxcompiler.dll` / `dxil.dll` (same arguments as the fork, signed), synchronously on
  first use (median 14 ms per PS), cached per PS in memory and in `<shader dir>\depth20e4\` (keyed by the source
  text, reused by later runs); the PSO key carries `depth_variant` and drops the fixed-function bias; the bias goes in
  `kRootConstDepth`. A variant that cannot be built or whose PSO fails falls back to the fixed-function bias
  (logged, counted). Periodic draw line: `depth20e4 mode M: variants B (failed F, T ms) draws D`. SV_Depth output
  turns early-Z off for those draws. Details and the test plan: `stage1\subagents\stubs.md`.
  **Compare edge** (`pdz_native_depth20e4_edge`, default off; stage 2 polish 4, `stage2\polish\zfight.md`): mode 1
  rounds only the offset draw, so its 20e4 value meets the unrounded float32 depth of the surface under it, while
  the ROV compares two 20e4 numbers; for offsets under about one 20e4 step the result then depends on where the
  surface depth sits inside its rounding interval (banding / speckle). With the cvar on, offset draws that test
  without writing depth write the edge of their rounding interval on the side the depth function looks at
  (GREATER_EQUAL / LESS: upper midpoint; LESS_EQUAL / GREATER: lower midpoint; odd values one float32 ulp inward
  for the round-to-even ties), which gives the ROV's answer against a float32 or a 20e4 surface depth. Separate
  variants per edge (`depth_variant` 2 / 3 in the PSO key); the edge-0 source is unchanged.
* **RECTLIST** (`pdz_native_rectlist`, default on): drawn as a triangle list through `kRectListGsDxil`, the SDK's
  rectangle-list GS (fourth vertex `-v[a] + v[b] + v[c]` across the longest edge, every interpolator). Logged for
  the first 3 draws, counted (`rectlist draws` in the periodic line); a failed RECTLIST PSO counts as an
  unsupported primitive.
* **Unsupported primitive types** (QUADSTRIP, LINELOOP, POLYGON; RECTLIST with the cvar off): skipped, counted in
  `skipped_pipeline`.
* **Stencil per-face masks**, `ALPHA_FACTOR` blend factors, MSAA: as the SDK's own fallbacks (front masks for
  both faces, `BLEND_FACTOR`, no MSAA).
* **Mixed-dimension texture bindings** (fetch dimension != shader fetch kind): null SRV, counted.
* **Co-issued ALU operations** (`native_coissue.h`, `native_coissue_scan.h`, cvar `pdz_native_coissue_fix`, on):
  the vector and the scalar operation of one Xenos ALU instruction read their sources before either writes (the
  SDK's `ProcessAluInstruction` stores both results after both operations; the PDZ shaders rely on it, e.g. the
  shadow-mask blur `26eaa1f1` sums the previous tap's weight in the scalar slot). The fork emits the vector write
  first, so in 215 cached pixel shaders a scalar operation reads the vector result: among them the light pass
  `a95c4b61` of every level (rim term `exp2(N.L)` instead of `rim ^ (4 RT0.z)`: light buffer + (1..2) x sun colour
  x G-buffer RT0 alpha, Joanna's hair in Bridge / Arena ~5x; and `p0 = depth + c0.z > 0` instead of `depth > 0`,
  pixels without geometry lit). At a pixel shader's first draw the microcode (guest memory, verified against the
  hash) is scanned; a hazardous shader's `.hlsl` is patched (its scalar line(s) moved above its vector line; the n-th
  `ps = ` line is the n-th instruction with a scalar operation, verified over the 1,637 cached shaders with a
  container: 215 patched, identical to the offline reference), compiled with the depth20e4 compiler (fork's
  dxcompiler, fork's arguments) and cached in `<shader dir>\coissue\`. The depth20e4 variants start from the patched
  source. Coverage line: `co-issue fix on: checked C, hazardous H (I instructions), patched P (D from disk, T ms),
  failed F`. The proper fix is in the fork (`ShaderRecompiler::recompile(const AluInstruction&)`: compute the scalar
  part before the vector write); then this pass finds no hazard.
* **The cube instruction** (`native_cubefix.h`, cvar `pdz_native_cube_fix`, on): the fork's `cube()` returns
  `(0, 0, 0, index)` and keeps the direction in a two-entry side structure its tfetchCube reads by that index. The
  omni-light cookie pass `858ecd68` (light buffer; Nightclub, Mansion 21 draws a frame, MP Urban / Temple / Gasplant,
  Sabotage Urban) and `8c549720` project their 256x256x32 cookie stack with the face coordinates
  (`xy / (2 ma) + 1.5`, `tfetch3D .yxw`, w = the animation phase): every pixel sampled the cookie's centre texel, a
  uniform light following that one texel (Gasplant's ceiling). `24d8a196` runs five cube taps before its fetches:
  three sampled another tap's direction. At a pixel shader's first draw whose main() uses `cube(` or a cube fetch,
  the helpers are replaced by the SDK's formulas (`dxbc_translator_alu.cpp` kCube: x = T, y = S, z = 2 x signed
  major axis, w = face id; `dxbc_translator_fetch.cpp` kCube: (S, T) in 1..2 and the face back to a direction,
  `pdzCubeDirection`) and compiled with the co-issue / PsParamGen patches (same cache). Coverage line:
  `cube fix on: checked C, using U, patched P, failed F`. With the cvar on every pixel shader's `.hlsl` is read once.
* **Colour targets the PS does not write** (cvar `pdz_native_mask_unwritten_targets`, on): write mask 0 for every RT
  whose `SV_Target` the PS lacks (sidecar `color_outputs`), as the SDK's `draw_util::GetNormalizedColorMask`. D3D12
  would store undefined values (the hair / foliage forward draws `89e06287` / `7c273fb3` export oC0 only with
  `RB_COLOR_MASK` 0077: RT1 is the velocity target of the motion blur).
* **16-bit fixed-point targets** (`k_16_16` / `k_16_16_16_16`, the light buffer; cvar `pdz_native_fixed16_clamp`,
  on): host float16 does not saturate at the guest's -32..32 (light-buffer specular alpha up to 323 natively against
  32 emulated); the resolve clamps to the range and reads NaN as 0 (`native_passes.cpp` flag bit 2), like the 7e3
  clamp. Per-write clamping gives the same result for the light buffer's ONE/ZERO and ONE/ONE passes of
  non-negative terms.
* **Glow chain at scale 1 vs the 2x reference (no change; stage1\subagents\nightclub4.md).** The glow chain
  (`2795c43c` bright pass 320x160 -> 160x120, `6e68d641` 13-tap blur, `bb2b4402` 2x2 box -> 80x60, `6e68d641`,
  `0c9573f2` 15-tap h / v, composite `edcce21f` adds the 80x60 glow x1 and the star x1/3) is an exact 1x evaluation
  natively (a numerical model of these passes reproduces the native resolves). The emulator at resolution scale 2
  samples every pass at host texel positions and replaces the first host row / column of each resolve
  (`resolve_resolution_scale_fill_half_pixel_offset`): next to bright content on the top / left frame edge (Nightclub's
  club facade, its neon at rows 0-20) the 2x glow is ~1.6x the 1x one (80x60 row 0: 17 vs 10 levels). Interior glows
  match (same energy and centroid). This is the reference's resolution, not a renderer difference; `pdz_native_scale=2`
  (with `pdz_native_scale_fill_half_pixel`, on) follows the same rules.

## 7. Open points for the first run

1. `[object+12]` is assumed to be a pointer to the patched microcode copy (physical when < 0x20000000). If the
   coverage line shows every VS binding ending as a miss with the unbound container's hash (the warning
   "bound copy hashes to the UNBOUND container"), the copy lives elsewhere; the dump in `misses\` tells.
   The copy is read from the device's current vertex program object (device+12948) whenever its size matches,
   so it can be another VS of the same size or freed memory. A copy whose first control-flow triple (dwords
   0-2) differs from the created microcode is ignored ("D3D's copy is not a variant of this shader") and the
   binder's output is looked up instead: Subway (log 087) read 192 zero bytes for VS 8cbc4754... decl 40D5AB00,
   translated them ("loop not closed", exit 4) and missed for 38 frames while the binder's 475c5932... was cached.
   Every "first differing dword 0" binder mismatch in the logs to date (to 097) was this zero copy.
2. The stream fetch dword layout (type bits in `VB+12`, endian in `VB+16`) is read from `sub_826F9578`'s
   arithmetic; the two one-time log lines ("type bits", "endian first seen") confirm it in the first frame.
3. Literal application stays off by default (§2); flip `pdz_native_literals` if bound VS draw wrong with
   constants a parent literal should supply.
4. 16-bit indices only (the draw code's `start * 2`); a 32-bit index buffer would show as garbage geometry plus
   the "bit 31" log line.

## Datacore white glow above the door (2026-10-26, analysis only)

Full note with evidence: C:/Games/PDZRenderResearch/native/stage1/subagents/bloom.md.

- Pass dump run13 (tools/passdump.py): the first real divergence is HDR resolve #16 (403C16D0 -> 1D700000,
  after the 7 draws that follow resolve #15). The emulated HDR has a large white cloud under the light; native
  only thin spark streaks. The bloom chain after it (#17 320x160 16_16_16_16_EXPAND, #18-#22 64x64 16_FLOAT,
  #23-#40 bright pass / 4-direction star / 80x60 glow, composite #41) tracks the emulator (MAE 0.4-1.6), so the
  bloom formats, views, signs and samplers are not the cause.
- Per-draw lists (run13 native_draws.bin / emulated_draws.bin): same depth-only draws (26cb4d39 ...), the same
  alpha-blended glow meshes (VS 2d1be11c / b029f58d / 4ba3e678) and the same 4 particle QUADLIST batches
  (aee0d65f, identical VS constants). The emulator alone has 2 x 24 one-vertex POINTLIST draws (VS 6dd9dd04,
  pass-through float3 position + float4 colour, no constants) right before 26cb4d39 and before 99486c33; the
  native never records them (not hooked). Native issues 99486c33 twice, the emulator once.
- VS 2d1be11c scales the glow colour by a vertex texture fetch from 1D388000 (k_8_8_8_8 2048x1 tiled, an upload)
  at (attr 6 .x, c21.x). Open candidates: a stale native upload of that table (UploadKey hashes the first 4 KB =
  texels 0..31 of a tiled 2048x1; full re-check every 8 frames under a 64 MB budget), the missing 24-point pass,
  or the soft-depth fade reading depth resolve #14 (k_24_8_FLOAT) through a view with another encoding.
  pdz_native_volume_view on (autodiff fix6_volume_replicas) did not bring the glow back.

## Open: MISSION BRIEFING screen (Nightclub regression, log 085, MAE ~29)

Analysis only (stage1/subagents/uibrief.md has the evidence). Shadow dump f1800 of log 085: the panel is not a
render-to-texture; the photos, city map and (very likely) the swirl background all come from ONE opaque Bink
video quad (caller 8240B3E0, DrawUP TRISTRIP 4 verts stride 20, VS B2E19D06BE503178 decl 403BC810, PS
C2BDC141DB1B3BFF = YUV->RGB with PS c0-c3, k_8 planes 864x432 + 2 x 432x216), the only draw on the screen that
writes its own PA_CL_VTE_CNTL / PA_CL_CLIP_CNTL. Natively the background between the scan lines is black and the
panel is a bright blue (0x1374F0) tilted wash over the photos; luma differs as well as chroma. First thing to
check: that draw's VTE flags / ndc scale-offset and vertex source (heap 414B9874 vs ring copy) against the
emulator (frame trace line in native_draw.cpp, or pass dump + drawconsts.py). Second: PS c0/c1 alpha of the
additive UI draws (callers 827B58FC / 827B5C30, PS 81E9B4DAC3657C45 / 38B7D2F976311B20).

## Nightclub harbour water (analysis, 2026-10-26; no code change)

Full note: `PDZRenderResearch/native/stage1/subagents/water.md`.

- Water PS 59493F24D03164E7 (VS A589C22BD7632CAA, caller 82710E30; variants CCF49F44605B4187 / A2917857E96DAE46)
  binds the planar reflection 1C4EC000 (fmt 6, 384x384) at fetch slot 4 (s8 / s7 in the variants), the light
  buffer 1D700000 (fmt 32, resolve #12) at slot 2, rt1 normals 1AF30000 at slot 1 and a DXN normal map at slot 5.
  The reflection coordinate is projective in the PS (c50 / c51 / c53, divided by w, DXN-perturbed, then
  * (0.5, -0.5) + 0.5).
- Every colour term of the water is multiplied by the slot 2 light buffer, so a dark light buffer gives black water
  even with a correct reflection. The water runs after resolve #1 / #2 (reflection) and #12 (light buffer).
- m2_clip: reflection now close (resolve #2 MAE 4.84); light buffer resolve #12 MAE 13.34, and the native light
  buffer lacks the projected water caustics on the dock wall.
- Swap 3785 (m2_full capture 9): the native harbour keeps the orange lamp streaks (light-buffer terms) but loses
  the reflection term t4 * t2 * c7; prime suspect is the slot 4 reflection fetch (projective coordinate from
  c50 / c51 / c53 with w = r2.w in the translated PS, the native mirror view, or its sampler), not the light
  buffer. Next step: pass dump of that view and the per-draw PS constants / slot 4 view of PS 59493F24.
