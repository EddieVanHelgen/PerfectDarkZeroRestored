# Asserting the shadow reader against D3D's own state flush (`sub_826FA8B8`)

Stage 0 plan (RENDER_FEASIBILITY.md 6.4 step 1: "Assert against `sub_826FA8B8`
output"). Everything below was read from `generated\default\*.cpp` of the TU3
checkout on 2026-10-25; the function bodies are quoted as PPC comment streams.

## 1. What the flush does (exact structure)

`sub_826FA8B8(r3 = device)` (`perfectdarkzerorecomp_recomp.178.cpp:17560`) is
called by every draw path (`DrawIndexedVertices` 826EA128, `DrawVertices`
826E9EE8, `DrawVerticesUP` 826E9A60, `BeginVertices` 826E97B0, `Resolve`,
`Clear`, 7 internal sites) just before the draw packet. It walks the five
64-bit dirty masks at device+16..+55, in this order, and clears each mask
(`std r30(=0), N(r31)`) after emitting it:

| Step | Mask (device offset) | Bits tested (bit 63 = MSB) | Emitter | Register run / payload |
|---|---|---|---|---|
| 1 | +48 (mask 5) | bit 10 | `sub_826F9578` | vertex streams -> fetch constants (see 1.1) |
| 2 | +48 | bits 8..5 (any) | `sub_826FA2E8` | shader program load (IM_LOAD, INVALIDATE_STATE, SQ_PROGRAM_CNTL, 0x2D01) |
| 3 | +48 | bits 63..26 | `sub_826F9298(dev, mask<<0 keep 38, 0x2300, dev+11768)` | 38 regs 0x2300..0x2325 |
| 4 | +48 | bits 25..18 | `sub_826F9298(dev, rotl(mask,38) keep 8, 0x2380, dev+11920)` | 8 regs 0x2380..0x2387 |
|   | +48 | cleared |  |  |
| 5 | +16 (mask 1) | all | `sub_826F9450(dev, mask, 0x4000, dev+1920)` | VS ALU constants, 1 bit = 4 float4 (64 bytes) |
|   | +16 | cleared |  |  |
| 6 | +24 (mask 2) | all | `sub_826F9450(dev, mask, 0x4400, dev+6016)` | PS ALU constants, same granularity |
|   | +24 | cleared |  |  |
| 7 | +32 (mask 3) | bits 63..32 | `sub_826F9358(dev, mask keep 32, 0x4800, dev+1152)` | fetch constants 0..31 (6 dwords each; bit 63 = fetch 0) |
| 8 | +32 | bit 31 | `sub_826F9298(dev, 0xFFFFFFFFFF000000, 0x4900, dev+10112)` (`lis r4,-256`: sign-extended, 40 bits); also `std r11, 11440(dev)` | 8 bool + 32 loop constant dwords, 0x4900..0x4927 from +10112..+10271, always all 40 (see 1.3) |
| 9 | +32 | bits 29..14 | `sub_826F9298(dev, rotl(mask,34) keep 16, 0x2000, dev+11456)` | 16 regs 0x2000..0x200F |
|   | +32 | cleared |  |  |
| 10 | +40 (mask 4) | bits 63..43 | `sub_826F9298(dev, mask keep 21, 0x2100, dev+11532)` | 21 regs 0x2100..0x2114 |
| 11 | +40 | bits 42..38 | `sub_826F9298(dev, rotl(mask,21) keep 5, 0x2180, dev+11616)` | 5 regs 0x2180..0x2184 |
| 12 | +40 | bits 37..26 | `sub_826F9298(dev, rotl(mask,26) keep 12, 0x2200, dev+11636)` | 12 regs 0x2200..0x220B |
| 13 | +40 | bits 25..5 | `sub_826F9298(dev, rotl(mask,38) keep 21, 0x2280, dev+11684)` | 21 regs 0x2280..0x2294 |
|   | +40 | cleared |  |  |

Bit numbering: in every mask the first register of a run is the HIGHEST bit of
its field and the following registers go downwards (verified in
`sub_826F9298`: `cntlzd` of the mask = registers to skip, then `cntlzd` of the
inverted mask = run length, header `(len-1)<<16 | reg`, `len` dwords copied
from `shadow + 4*skip`). Mask 5 bit 10 and bits 8..5 are not registers:
bit 10 = "streams changed" (set by `SetStreamSource`, `SetVertexDeclaration`,
`SetIndices`), bits 8 and 5 are set by `SetPixelShader` (`ori 256`, `ori 32`),
bit 7 by `sub_826F9578` when the (declaration, VS) pair changed (`ori 128`);
the exact meaning of bit 6 was not traced (likely `SetVertexShader`).

### 1.1 Vertex streams -> fetch constants (`sub_826F9578`)

Reads the current declaration (device+11408, `+12` = max stream index) and
for each stream `s` the slot `device+12556+8*s` (`+0` offset, `+4` VB object;
VB `+12` base address, `+16` size), builds the 2-dword vertex fetch constant
(`base+offset` with the 2-bit endian/type in the top bits, `size-offset` in
words with bits 28-29 set) and stores it at `device + 1916 - 8*s` (dword 1) and
`device + 1912 - 8*s` (dword 0): stream 0 is vertex-fetch constant 95 (the top
2 dwords of texture fetch slot 31), stream 1 fetch slot 31 dwords 2-3, stream
2 dwords 0-1, stream 3 fetch slot 30 dwords 4-5, and so on downwards. It sets
the matching fetch-slot bit in mask 3 (`1<<63 >> (slot)`). PDZ binds streams
0 and 1 (strides 12/16/20/24 seen at `SetStreamSource`); `DrawVerticesUP` and
`BeginVertices` write fetch slot 31 themselves (`0x48BA`).

### 1.2 The two draw-time register writes

Inside the draw functions, before the flush: `VGT_INDX_OFFSET` (0x2102,
device+11540) is stored with the base vertex (`DrawIndexedVertices` r5,
`DrawVertices` r5, `BeginVertices` 0) and mask 4 bit 61 is set; this is why the
hook records the base vertex from the argument rather than from the shadow.

### 1.3 Constant stores: register <-> device offset (settled 2026-10-26)

Every store is in register order: the writers advance the register number and
the source pointer together (`sub_826F9298`: skip = `cntlzd(mask)` registers
and `4 * skip` bytes), so register `first + k` comes from `store + 4k`.

| Registers | Device | Shader index | D3D writers (generated code) |
|---|---|---|---|
| 0x4000 + 4n .. +3 | +1920 + 16n | VS `c n` (n < 256) | flush step 5 `sub_826F9450(dev, mask1, 0x4000, dev+1920)` |
| 0x4400 + 4n .. +3 | +6016 + 16n | PS `c n` = hardware c(256 + n) | flush step 6 `sub_826F9450(dev, mask2, 0x4400, dev+6016)` |
| 0x4900 + d (d < 8) | +10112 + 4d | bool b = 32d + bit (bit `b & 31`, LSB first); VS b0..127 = d 0..3, PS b0..127 = b128..255 = d 4..7 | `SetVertexShaderConstantB` `sub_826E3648` (dword `2528 + (n >> 5)` = +10112), `SetPixelShaderConstantB` `sub_826E36A0` (dword `2532 + (n >> 5)` = +10128); getters `sub_826E36F8` / `sub_826E3740` |
| 0x4908 + i (i < 32) | +10144 + 4i | loop i: VS i0..15 = 0..15, PS i0..15 = 16..31 | `SetVertexShaderConstantI` `sub_826E3788` (`(2536 + n) * 4` = +10144 + 4n), `SetPixelShaderConstantI` `sub_826E37D8` (`(2552 + n) * 4` = +10208 + 4n); dword = count (x) \| start (y) << 8 \| step (z) << 16 from the low bytes of the int4 |

* All six setters set mask 3 bit 31 (`ld 32(dev); oris 0x8000; std`), the bit
  that emits the whole 40-dword bool + loop run (step 8).
* State block capture `sub_826ED690` copies, for vertex state, 4096 bytes from
  +1920, 64 bytes (16 loop dwords) from +10144 and 16 bytes (4 bool dwords) from
  +10112; for pixel state 4096 bytes from +6016, 64 bytes from +10208 and 16
  bytes from +10128.
* Shader side: the microcode's loop id and bool address are absolute. The SDK
  reads `SHADER_CONSTANT_LOOP_00 + loop_id` (`interpreter.cpp:129`) and cbuffer
  vector `2 + (i >> 2)`, component `i & 3` (`dxbc_translator.cpp:1744-1747`) of a
  copy of registers 0x4900..0x4927 (`d3d12/command_processor.cpp:4782-4790`);
  bools as vector `b >> 7`, component `(b >> 5) & 3`, bit `b & 31`
  (`dxbc_translator.cpp:1597-1601`). The fork emits `loopConst(cf.loopStart.loopId)`
  and `boolConst(boolAddress)` without a stage offset (`pdz_recompiler.cpp:560,
  922`); PDZ's pixel shaders carry loop id 16 (183 sidecars list
  `loop_constants_used: [16]`) and bools 128..130 (339 sidecars), the one VS loop
  (`4e9babb73e5dbfe9`) loop id 0.
* The game's light loop: `sub_826082F0` calls
  `SetPixelShaderConstantI(dev, 0, {n, 0, 1, 0}, 1)`: PS i0 = `n | 1 << 16` at
  +10208 = register 0x4918 = `loopConst(16u)`.
* Data (Datacore, logs 045 / 053, the dual-candidate assert of 3171011): every
  emitted 0x4908 + i equalled device+10144 + 4i (4,342,624 dwords, 134,665
  non-zero); with +10208 as the base 4,073,294 matched, none of them non-zero.
  The value checks against the stores in the same runs: VS-ALU 260,690,080 / 0,
  PS-ALU 163,732,528 / 0, bool 1,085,656 / 0 (matches / mismatches, log 045).
* Since then the assert compares the loop dwords against +10144 only, counted in
  `loop-value` like any register, plus one summary line (section 3).

## 2. Hook design for the assertion

Hook `sub_826FA8B8` itself (REX_HOOK_RAW, forward last). At entry:

1. `r3` = device. Copy the five masks (+16..+55) and the seven register runs
   (+11456..+11952), the 32 fetch constants (+1152..+1920) and the 8 bool
   dwords (+10112) into a side buffer `flush_snapshot`.
2. Record the ring write pointer `device+0` (`ring_before`).
3. Call `__imp__sub_826FA8B8`.
4. Record `device+0` again (`ring_after`) and the five masks (must all be 0).
5. Parse the PM4 bytes between `ring_before` and `ring_after` (the ring is a
   guest physical buffer; translate like `REX_RAW_ADDR`): type-0 packets
   `(count-1)<<16 | reg` + `count` dwords; `0x80000000` NOP pads from the
   ALU/fetch writers; type-3 packets from `sub_826FA2E8` (IM_LOAD 0xC0012700,
   INVALIDATE_STATE 0xC0003B00, SET_CONSTANT 0xC0xx2D01). The wrap case
   (`sub_826F91E0` / `sub_826DDE30` = segment advance when the ring is full)
   is detected by `ring_after < ring_before`; skip the frame's assertion then.
6. Compare (section 3) with the newest ring record (`Get().ring[(head-1) & 4095]`),
   which the draw hook wrote microseconds earlier at the draw's entry.

Pairing rule: every draw hook record is followed by exactly one flush call on
the same thread (Clear and Resolve also flush; `BeginVertices` flushes
inside). The flush hook pairs with `ring[head-1]` when that record's
`sequence` is the one the draw hook just produced (store the sequence in a
thread-local `pending_flush_sequence`). Flushes with no pending record are the
7 internal callers (ignore, but count them).

Cost: this is the assertion build only (a second cvar `pdz_render_shadow_assert`);
the PM4 parse is O(bytes emitted), fine for a debugging session, not for the
recorder.

## 3. What to compare

For the pending record `r` and the flush snapshot `f` (both taken before the
original flush ran; they should be identical except for 1.2):

| Item | Record source | Flush-side source | Expected |
|---|---|---|---|
| Dirty masks 1-5 | `r.dirty[0..4]` | `f.masks` | equal, except mask 4 bit 61 (VGT_INDX_OFFSET) may be set only in `f` |
| Register runs | `r.regs_be` decoded by the mask bits (section 1 table) | the type-0 packets actually emitted between `ring_before` and `ring_after`: `(reg, value)` pairs | every emitted `(reg, value)` with reg in 0x2000-0x2387 equals the record's shadow value; every record register whose dirty bit is set appears in the emission |
| Fetch constants | `r.fetch_be` slots whose mask-3 bit is set | the 0x4800+6*i runs emitted by `sub_826F9358` | equal for texture slots (0..25); stream slots (26..31) differ when mask 5 bit 10 was set (rebuilt by 1.1): recompute from `r.streams[]` and compare that instead |
| ALU constants | not recorded (8 KB; only the masks are) | 0x4000 / 0x4400 runs from `sub_826F9450` | assert that the emitted constant ranges equal the ranges the record's masks 1-2 decode to (`c4k..c4k+3` per bit) and nothing else; values compared against the device at flush entry |
| Bool constants | `r.bools_be` | the 0x4900 run (8 dwords) | equal when mask 3 bit 31 set |
| Loop constants | not recorded; the device store `+10144 + 4i` at flush time (the flush does not change it) | 0x4908..0x4927, the rest of the same run (32 dwords) | equal (`loop-value`); summary line `loop constants 0x4908..0x4927 vs device+10144: N dwords, M mismatches; non-zero VS i0..15 a (count 0: b), PS i0..15 = 16..31 c (count 0: d)` with the totals |
| Shader load | `r.vertex_shader` / `r.pixel_shader` object pointers, hashes | `sub_826FA2E8`'s IM_LOAD addresses: the physical microcode copies (shader object `+12`) | IM_LOAD address == `Load32(shader+12)` for the recorded objects; XXH3 of the loaded range == the record's hash for pixel shaders; for vertex shaders see 4.3 |
| Draw packet | `r.a[]` (prim, counts) | `PM4_DRAW_INDX` (0xC0012201 / 0xC0032201) + `VGT_DRAW_INITIATOR = prim \| count<<16 \| 0x80` | prim type and index/vertex count equal; index buffer address == `Load32(r.index_buffer + 12)`? (field offset to verify: the IB object layout was not read) |
| Viewport, scissor | `r.viewport`, `r.scissor` | `PA_CL_VPORT_*` (0x210F..0x2114) in the 0x2100 run; scissor via `sub_826EE1C0`'s separate PA_SC_WINDOW_SCISSOR packet (not in a dirty run) | VPORT scale/offset == (W/2, X+W/2, -H/2, Y+H/2, MaxZ-MinZ, MinZ) within float rounding; scissor packet values == `r.window_scissor` |

## 4. Expected mismatches (registers D3D fixes up at flush)

1. `VGT_INDX_OFFSET` (0x2102): written by the draw itself (1.2), so the
   record's value is the previous draw's base vertex. Compare the emitted value
   with `r.a[1]` (DrawIndexed / Draw) or 0 (BeginVertices) instead.
2. Stream fetch constants (slots 26..31 region, in practice slot 31 and 30):
   rebuilt from the stream slots at flush time; the record holds the previous
   build. Recompute as in 1.1.
3. `SQ_PROGRAM_CNTL` (0x2180), `SQ_CONTEXT_MISC` (0x2181),
   `SQ_INTERPOLATOR_CNTL` (0x2182) and `RB_HIZCONTROL` (0x2203): `sub_826FA2E8`
   writes them into the shadow (PART2 section 10 lists `shadow:RB_HIZCONTROL
   shadow:SQ_CONTEXT_MISC shadow:SQ_INTERPOLATOR_CNTL shadow:SQ_PROGRAM_CNTL`
   for it) when the program is reloaded (mask 5 bits 8..5), i.e. the record
   taken before the flush holds the previous program's values whenever a shader
   changed. Compare these four against the post-flush shadow, not the record.
4. Fetch slot 31 for `DrawVerticesUP` / `BeginVertices`: written by the draw
   function after the flush (`0x48BA` in their PM4), so it is in neither
   snapshot; ignore.
5. Mask 3 bit 31 (bool and loop constants): the flush emits all 8 bool and 32
   loop dwords regardless of which changed; the record cannot tell which were
   set by the game, only the 8 bool values (the loop dwords are compared with
   the device store, 1.3).
6. `RB_COLOR_MASK` (0x2104): `SetRenderTarget` with a NULL surface rewrites the
   mask (PART2: `shadow:RB_COLOR_MASK` in 826F0E90); already in the record since
   it happens before the draw. No mismatch expected, listed because it is the
   one "colour write" register the game writes both inline and through D3D.
7. `PA_SC_WINDOW_SCISSOR_TL/BR`, `PA_SC_WINDOW_OFFSET`, `RB_SURFACE_INFO` go
   through `sub_826EE1C0` (its own SET_BIN_MASK + type-0 packet, dirty mask 3
   bit for RB_SURFACE_INFO only): the scissor is not in any dirty run, so a
   draw whose scissor changed shows an extra packet the record explains only
   through `r.window_scissor`.
8. Predicated tiling: never active in PDZ (device+10432 bit 2 = 0); if it ever
   is, `SET_BIN_MASK/SELECT` packets wrap the draw and the parse must skip them.
9. Ring wrap (`sub_826F91E0` path inside the writers): the packets are split
   across the wrap with a jump; skip the assertion for that draw.

### 4.3 Vertex shader hash caveat

Package vertex shaders are stored with unbound `vfetch` instructions (format 0,
stride 0: PART3_SHADERS.md 4.1) and D3D patches the copy at shader object `+12`
when the shader is first used with a declaration (which copy, and whether it is
re-patched per declaration, is not yet traced: `sub_826F9578`'s final compare of
device+11424/11432 against +12688/12696 and the mask-5 bit 7 it sets are the
trigger). Consequence: the XXH3 cached at `CreateVertexShader` is the hash of
the unbound microcode and will NOT match the `.xsh` entry for most vertex
shaders (the .xsh holds the bound variant the GPU executed). The assertion
should therefore rehash `Load32(vs_object+12)` for `ucode_bytes` at flush time
(cold path) and compare that with the `.xsh` keys; the create-time hash still
identifies the shader container. Pixel shader hashes should match directly.

## 5. Step-by-step plan

1. Add `pdz_render_shadow_assert` (bool, default off) and `shadow_assert.cpp`
   with the `sub_826FA8B8` hook of section 2 (REX_EXTERN(__imp__sub_826FA8B8)).
2. Thread-local `pending_flush_sequence`: every draw-kind hook stores the
   sequence it recorded; the flush hook consumes it.
3. Implement the PM4 walker over `[ring_before, ring_after)` (guest physical;
   use the same translation as `GuestPtr`): collect `(reg, value)` from type-0
   packets, ALU / fetch / bool runs, the IM_LOAD addresses and the draw packet.
4. Compare per section 3; log one `REXLOG_WARN` line per mismatch with record
   sequence, register name (`register_table.inc`), record value, emitted value;
   keep per-register mismatch counters and print them at every dump frame.
5. First run: enable both cvars in the front end (2 resolves, few draws per
   frame), then a gameplay frame. Expect only the section 4 items; anything
   else is a wrong offset in `shadow_record.h`.
6. Then extend: compare the stream fetch rebuild (1.1) from `r.streams[]`, and
   the `VGT_DRAW_INITIATOR` fields against `r.a[]`.
7. Exit criterion for stage 0: 1,000 consecutive draws with zero unexplained
   mismatches on Datacore (or an MP map), and the per-draw hook cost measured
   with `pdz_hook_profile` (src\mod\hook_profile.h) under 1 us.
