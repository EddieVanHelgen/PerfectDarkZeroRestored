# Shadow assert: wiring for the orchestrator

`shadow_assert.cpp` hooks `sub_826FA8B8` itself and pairs each flush with
`ring[head-1]` (consumed once per record), so the comparison needs NO change
to `shadow_hooks.cpp`. Two optional call sites make it nicer; both are
declared in `shadow_assert.h` (`#include "shadow_assert.h"` in
`shadow_hooks.cpp`).

## 1. Per-frame summary at Swap (recommended)

In the `sub_826DF960` (Swap) hook, after `OnSwap(base);`:

```cpp
  __imp__sub_826DF960(ctx, base);
  OnSwap(base);
  AssertOnSwap(); // shadow_assert.h: "shadow assert: N draws, M registers compared, K mismatches (top: ...)"
```

`AssertOnSwap()` is cheap when the assert is off (one branch on an internal
flag) and needs no cvar check. Without this call the same line is written at
the next frame's first flush (the flush hook notices the frame change), so
the summary is never lost, only delayed by one draw.

## 2. Draw packet check after the draw returned (optional)

In the four draw hooks (`sub_826EA128` DrawIndexedVertices, `sub_826E9EE8`
DrawVertices, `sub_826E9A60` DrawVerticesUP, `sub_826E97B0` BeginVertices),
after the `__imp__` call, guarded by the assert switch:

```cpp
  __imp__sub_826EA128(ctx, base);
  if (AssertEnabled())
  {
    AssertOnDrawReturn(base); // shadow_assert.h: VGT_DRAW_INITIATOR prim/count vs the record's arguments
  }
```

(for BeginVertices put it after `r.a[3] = ctx.r3.u32;` in the enabled
branch and after the plain `__imp__` call in the other). `AssertOnDrawReturn`
uses the device and ring pointer the flush hook saved, so the hook does not
need to keep `r3`. Without it the `draw-packet` item simply stays at 0.

## 3. Build

`src/mod/render/shadow_assert.cpp` was appended to
`PERFECTDARKZERORECOMP_SOURCES` in `CMakeLists.txt` (after
`shadow_dump.cpp`); nothing else to add. Syntax-checked with
`src\mod\render\check_syntax_assert.bat` (clang-cl /Zs and clang++
-fsyntax-only -std=c++23 -Wall -Wextra, the game's include set and defines).

## 4. Running

```
--pdz_render_shadow --pdz_render_shadow_assert
```

(or both in the settings toml; both hot-reloadable). Log lines:

| Line | When |
|---|---|
| `shadow assert: on (...)` | first flush with both switches on |
| `shadow assert: mismatch #seq fN Kind ITEM: ...` | first 50 unexpected mismatches (REXLOG_WARN) |
| `shadow assert: expected mismatch (first of its kind) ...` | once per entry of the ignore table / structural class |
| `shadow assert: N draws, M registers compared, K mismatches (top: ...)` | every swap (or next frame's first flush) |
| `shadow assert: totals after D draws: ...` + one line per item | every `pdz_render_shadow_dump_frames` swaps from `AssertOnSwap` |
| `shadow assert: note: device+12948 ... differs from ... +20456` | once, if the VS program object is not the D3DVertexShader pointer |

Expected on a healthy reader: only the `expected mismatch` lines (VGT_INDX_OFFSET,
the four program registers, stream fetch constants, mask 4 bit 61) and
`K mismatches = 0`. Anything else is an offset or bit-layout error in
`shadow_record.h` / `shadow_assert.md` section 1, or a flush path not yet
described (the `type3` opcode counts and `other-type0` item say which).
