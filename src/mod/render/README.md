# Shadow-mode skeleton (native renderer, stage 0)

Files only: nothing in this folder is in the PDZ build or in the game. The
hooks observe and forward; the emulator keeps rendering (RENDER_FEASIBILITY.md
5.2 option C, 6.4 step 1). Target checkout: `C:\Games\PerfectDarkZeroRecomp-tu3`
(Title Update 3; every `sub_` address below is a TU3 address).

## Files

| File | What |
|---|---|
| `shadow_record.h` | The device-struct offset map (`pdz::shadow::dev`), the ring record `DrawRecord`, the shader / vertex-declaration tables, the recorder API, guest-memory helpers (big-endian loads, physical-window translation). |
| `shadow_record.cpp` | Static storage (~7.5 MB), the hot-path `Record()` (one struct fill per draw), the O(1) open-addressing tables filled by the Create* hooks (XXH3 of the microcode cached per shader object). |
| `shadow_hooks.cpp` | `REX_HOOK_RAW` on the 21 entry points (draws, Clear, Resolve, Swap, 10 state setters, 3 creators). Each forwards to `__imp__sub_X`; the draw kinds snapshot before forwarding, Create* after. |
| `shadow_cvars.cpp` / `shadow.h` | `pdz_render_shadow` (bool, default off) and `pdz_render_shadow_dump_frames` (int, default 600) in the game's `REXCVAR_DEFINE_*` pattern. In the real build the defines join `src\game_cvars.cpp` and the declares `src\game_cvars.h`. |
| `shadow_dump.cpp` | Cold path: `OnSwap()` (frame counter, dump policy) and `DumpFrame()` (one `REXLOG_INFO` line per record with the key registers decoded). |
| `shadow_assert.md` | The plan for asserting the reader against D3D's own flush `sub_826FA8B8`, with the exact dirty-mask bit layout read from the generated code. |
| `shadow_assert.cpp` / `shadow_assert.h` | The assertion itself (`pdz_render_shadow_assert`, off): hooks `sub_826FA8B8`, parses the PM4 the flush wrote into the ring and compares it with the newest record; per-frame summary line, first 50 mismatches logged, ignore table for the registers D3D fixes up at flush. Optional call sites in `ASSERT_WIRING.md`; `check_syntax_assert.bat` checks it. |
| `CMakeLists.txt` | Standalone OBJECT library against the SDK headers (not referenced by the PDZ build). |
| `check_syntax.bat` | `clang-cl /Zs` and `clang++ -fsyntax-only` over the four `.cpp` with the game's include set and defines (read-only on the PDZ tree). |

## How it would be enabled

1. Append the four `.cpp` files to `PERFECTDARKZERORECOMP_SOURCES` in the game's
   `CMakeLists.txt` (they must be linked into the executable: the hooks are
   strong definitions that override the weak generated `sub_X` aliases, see
   `generated\default\perfectdarkzerorecomp_pch.h` DEFINE_REX_FUNC).
2. Move the two cvar definitions into `src\game_cvars.cpp` / declarations into
   `src\game_cvars.h` (or keep `shadow_cvars.cpp`; both work).
3. `pdz_render_shadow = true` in the settings toml or on the command line
   (`--pdz_render_shadow`); hot-reloadable. The first full frame after enabling
   is written to the SDK log (the `logs` folder next to the exe), then every
   `pdz_render_shadow_dump_frames` swaps (0 = only once).
4. Create* hooks always record (init / level load, cold), so the shader and
   declaration tables are complete even when the switch is turned on later.
   `SetStreamSource` always mirrors the stride (one store) for the same reason.

## Recording format

`DrawRecord` (one per `DrawIndexedVertices`, `DrawVertices`, `DrawVerticesUP`,
`BeginVertices`, `Clear`, `Resolve`, `Swap`), ring of 4096, single producer
(the game's render thread), newest at `head-1`:

| Field | Source |
|---|---|
| `sequence`, `frame`, `kind`, `caller` (guest `lr`) | hook |
| `a[0..4]`, `f0_bits` | raw r4..r9 / f1 per kind (see the enum comment in `shadow_record.h`) |
| `vertex_shader` (+20456), `pixel_shader` (+12944), `vs_hash`, `ps_hash` | device struct, table lookup |
| `vertex_decl` (+11408), `index_buffer` (+12532) | device struct |
| `streams[16]`: `offset`, `vb` (+12556+8*s), `stride` (hook mirror), `base` (vb+12), `size` (vb+16) | device struct + VB object |
| `textures[26]` (+12704+4*s) | device struct |
| `render_targets[4]` (+12536), `depth_stencil` (+12552) | device struct |
| `viewport[6]` (+12808: X, Y, W, H, MinZ, MaxZ), `scissor[4]` (+12832), `window_scissor[2]` (+11524/+11528) | device struct |
| `dirty[5]` (+16..+55) | device struct, as of hook entry (before the flush clears them) |
| `regs_be[124]` | raw copy of +11456..+11952: the seven register runs 0x2000(16) 0x2100(21) 0x2180(5) 0x2200(12) 0x2280(21) 0x2300(38) 0x2380(8) plus the 12-byte gap at +11520 (scissor TL/BR at +11524/+11528) |
| `fetch_be[768]` | raw copy of the 32 fetch constants at +1152 (textures 0..25 and the stream constants in slots 31 downwards) |
| `bools_be[8]` | raw copy of +10112 |

ALU constants (+1920 VS, +6016 PS, 8 KB each) are not copied; their dirty
ranges come from masks 1 and 2 (1 bit = 4 float4 constants, bit 63 = c0..c3).

Tables: `ShaderEntry` (object pointer, container magic, microcode source
address and size, XXH3-64 over the guest-order dwords exactly as the SDK's
`PipelineCache::LoadShader` hashes them), `DeclEntry` (object pointer, element
count, max stream, first 16 raw `D3DVERTEXELEMENT9`, 12 bytes each).

Dump line (one per record): kind and arguments, caller, VS/PS pointers and
hashes, declaration, IB, bound streams, RT0-3 / DS pointers with
`RB_COLOR_INFO` / `RB_DEPTH_INFO` decoded (tile base, format), viewport,
scissor, `RB_DEPTHCONTROL` (z enable / write / func, stencil func and ops,
ref/mask), `PA_SU_SC_MODE_CNTL` (cull, front face, polygon mode, offset
enables with scale/bias), `RB_COLORCONTROL` + `RB_ALPHA_REF` (alpha test),
`RB_COLOR_MASK`, `RB_BLENDCONTROL0`, GPR split, point size, the five dirty
masks with the dirty register names, dirty ALU ranges, bound textures with the
fetch constant decoded (format, base, size, tiling). The first dump also lists
the shader and declaration tables and the previous frame's hook call counters.

## Per-draw cost estimate (hot path, switch on)

* 1 cvar load + branch, ~30 big-endian scalar loads (shaders, decl, IB, RTs,
  DS, viewport, scissor, 5 masks), 16 stream slots (2 loads each, +2 when
  bound), 26 texture slots, 2 table lookups (hash + 1-2 probes),
  3 `memcpy`: 496 + 768 + 32 bytes; `kBytesCopiedPerRecord` = 1,336 bytes
  copied (plus ~300 bytes of scalars) into a 1.9 KB record.
* Estimated 150-300 ns per draw on the game thread (no allocation, no clock,
  no logging, no syscalls); ~0.5 ms per 2,000-draw frame. Switch off: one
  relaxed load and a branch per hooked call.
* Create* hooks: one XXH3 over the microcode (~600 bytes) per shader, at
  init / level load only.
* Dump: a few thousand `REXLOG_INFO` lines once per N frames; expect a visible
  hitch on that frame (cold path by design).

## Uncertainties (argument registers, offsets, semantics)

Checked against the generated code (`DEFINE_REX_FUNC(sub_X)` bodies):

* `DrawIndexedVertices` r4 prim, r5 base vertex, r6 start index, r7 index
  count: r5 is compared with and stored to +11540 (`VGT_INDX_OFFSET`), r6 is
  spilled to the stack (284(r1)) and r7 kept in r16; the XDK order
  (BaseVertexIndex, StartIndex, IndexCount) fits but r6/r7 were not followed to
  the draw packet. Medium confidence on r6 vs r7.
* `DrawVertices` r5 start vertex -> +11540, r6 count: high.
* `DrawVerticesUP` r4 prim, r5 count, r6 data, r7 stride: high (count*stride
  ring allocation, data copied).
* `BeginVertices` r4 prim, r5 count, r6 stride (`mullw r11,r27,r6`), returns the
  ring data pointer in r3 (`mr r3,r28` before the epilogue; +13640 stores it):
  high. The XDK's `ppVertexData` out-parameter does not exist in this build.
  `EndVertices` = `sub_826E9A50` (3 instructions: `device[0] = device[13636]`),
  single caller `sub_827BA8E8`: high.
* `Clear` r4 count, r5 rects, r6 flags, r7 colour, f1 Z, r9 stencil (the float
  consumed the r8 slot: `mr r27,r9`): high for flags/colour/Z, medium for
  stencil.
* `Resolve` r4 flags, r5 src rect, r6 dest texture, r7 dest point, r8 level,
  r9 slice, r10 clear colour, f1 Z: high for r4..r7 (PART2 section 4); r8..r10
  from the XDK order only. The body reads one caller-frame word at
  `468(r1)` (frame 368 + 0x64); whether that is ClearStencil or pParameters
  was not resolved (the Xbox 360 stack-argument base was not confirmed).
* `Swap` r4 front buffer texture, r5 parameters: high (r5 -> r27; r4 released
  as the previous front buffer).
* `SetTexture` r4 sampler, r5 texture: high (slot = +12704 + 4*sampler, fetch
  constant = +1152 + 24*sampler). `SetRenderTarget` r4 index, r5 surface: high
  (+12536 + 4*index). `SetDepthStencilSurface` r4: high (+12552).
  `SetVertexShader` / `SetPixelShader` / `SetVertexDeclaration` / `SetIndices`
  r4: high (+20456 / +12944 / +11408 / +12532). `SetStreamSource` r4 stream,
  r5 VB, r6 offset, r7 stride: high for r4/r5, medium for r6 vs r7 (the slot
  stores `+0` = r6, `+4` = VB; the stride is not stored in the slot, which is
  what the XDK order implies). `SetViewport` r4 -> struct, `SetScissorRect` r4
  -> RECT: high.
* `CreateVertexDeclaration(r3 = pElements) -> r3 object`,
  `CreatePixelShader(r3 = pFunction) -> r3`, `CreateVertexShader(r3 = pFunction)
  -> r3`: high (no device argument; r4 is overwritten before any read; the game
  stores r3 into globals after the call). Shader object: +4 refcount, +12
  microcode copy (physical allocation), +28 flags, +52 parsed header. Vertex
  declaration object: +8 count, +12 max stream, +16/+24 per-stream usage masks,
  +36 element copy: high (read from the allocation and copy code).
* Device offsets: register runs, dirty masks and their bit layout: high
  (read from `sub_826FA8B8` and the run writers, see `shadow_assert.md`).
  Texture slot count 26 (12704 + 4*26 = 12808 = viewport): inferred from the
  layout, not from a bound check in `SetTexture`. Stream slot count 16: the
  stream builder loops to the declaration's max stream; 16 is the D3D9 limit
  and fits before +12688. The 12-byte gap at +11520..+11531: +11524/+11528 are
  the window scissor (from `SetScissorRect`), +11520 unknown.
* `IsGuestPointer` / `GuestPtr`: the physical window offset (+0x1000 for
  addresses >= 0xE0000000) mirrors `REX_PHYS_HOST_OFFSET` in the generated
  PCH for Win32; the D3D allocations seen (shader copies, VBs) use tags
  0xB580.. / 0x2480.. whose address ranges were not checked.
* Vertex shader hashes: cached at create time over the UNBOUND microcode; most
  will not equal the `.xsh` keys (bound variants), see `shadow_assert.md` 4.3.
  Pixel shader hashes should match.
* Thread model: the recorder assumes every hooked D3D call comes from one
  thread (the game's render thread). PDZ's video player / XUI paths call the
  same entry points; if any of them runs on another thread the ring would need
  a lock or per-thread rings. Not verified.

## Syntax check

`check_syntax.bat` (run 2026-10-25 with LLVM 23.1.3 from `C:\Program Files\LLVM\bin`):
both `clang-cl /Zs` and `clang++ -fsyntax-only -std=c++23` pass on the four
files with the game's include set and defines; `-Wall -Wextra` clean. No PCH is
needed: the mods in `src\mod` do not include the generated PCH either, only the
SDK headers (`rex/hook.h`, `rex/cvar.h`, `rex/logging.h`, `rex/hash.h`, and
`rex/graphics/register_table.inc` for the register names).
