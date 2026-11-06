# Native renderer core: hook wiring (for the orchestrator)

The core adds no `REX_HOOK_RAW` for functions `src\mod\render\shadow_hooks.cpp` already hooks (duplicate
strong symbols would not link). It needs three calls from the existing shadow hooks plus one optional call in
the app. Three functions the shadow does not hook are hooked by the core itself:
`sub_826DE910` (InsertCallback, in `native_surfaces.cpp`), `sub_826F14A8` (SetGammaRamp, in
`native_present.cpp`) and `sub_825F66E0` (DeResThread's snapshot read, in `native_surfaces.cpp`, for
`pdz_native_async_readback`). `grep` of `src\` shows no other hook on any of them.

## 1. shadow_hooks.cpp

Add near the other includes:

```cpp
#include "native/native.h"
```

### Clear hook `REX_HOOK_RAW(sub_826E0A48)` -- call BEFORE forwarding

The bound targets and the viewport are read from the device struct; order against the shadow `Record` does
not matter.

```cpp
REX_HOOK_RAW(sub_826E0A48)
{
  if (Enabled()) { Count(Hook::kClear); Record(...); }          // existing
  if (pdz::native::Enabled())
  {
    pdz::native::OnClear(base, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32,
                         FloatArgBits(ctx), ctx.r9.u32);
  }
  __imp__sub_826E0A48(ctx, base);
}
```

### Resolve hook `REX_HOOK_RAW(sub_826F21D0)` -- call AFTER forwarding, with the arguments saved first

The core reads `RB_COPY_DEST_INFO` (device+11876: component swap, exponent bias) that D3D writes inside the
call, so the call goes after `__imp__`. The guest body clobbers r4..r10 and f1: copy them first.

```cpp
REX_HOOK_RAW(sub_826F21D0)
{
  const uint32_t device = ctx.r3.u32, flags = ctx.r4.u32, src_rect = ctx.r5.u32, dest_texture = ctx.r6.u32,
                 dest_point = ctx.r7.u32, level = ctx.r8.u32, slice = ctx.r9.u32, clear_color_ptr = ctx.r10.u32;
  const uint32_t z_bits = FloatArgBits(ctx);
  const uint32_t call_site = uint32_t(ctx.lr);                  // read before forwarding
  if (Enabled()) { Count(Hook::kResolve); Record(...); }        // existing
  __imp__sub_826F21D0(ctx, base);
  if (pdz::native::Enabled())
  {
    pdz::native::OnResolve(base, device, flags, src_rect, dest_texture, dest_point, level, slice, clear_color_ptr,
                           z_bits, call_site);
  }
}
```

### Swap hook `REX_HOOK_RAW(sub_826DF960)` -- call AFTER forwarding and after the shadow's `OnSwap`, ALWAYS

`pdz::native::OnSwap` checks the switch itself and closes an open native frame even when the switch was just
turned off. r3/r4 must be saved before the forward.

```cpp
REX_HOOK_RAW(sub_826DF960)
{
  const uint32_t device = ctx.r3.u32, front_buffer = ctx.r4.u32;
  if (Enabled()) { Count(Hook::kSwap); Record(...); }           // existing
  __imp__sub_826DF960(ctx, base);
  OnSwap(base);                                                  // existing (shadow dump policy)
  pdz::native::OnSwap(base, device, front_buffer);
}
```

### Draw hooks (pipeline/draw agent)

Nothing for the core. The draw agent's entry point should start with `pdz::native::EnsureFrame()` (opens the
native frame, returns false when there is no device) and use `Surfaces::BindTargets`, `Textures::Lookup`,
`GetSampler` (see CORE_NOTES.md "What the pipeline/draw agent must call").

### State setters / Create*

Nothing: surfaces are decoded from the guest object at SetRenderTarget/Clear/Resolve time, textures from the
fetch constants (device +1152 + 24 * sampler, already in the DrawRecord), the resolve destination from the
dest texture object (+16). No CreateSurface / CreateTexture hook is needed.

## 2. src\perfectdarkzerorecomp_app.h (optional)

In `OnCreateDialogs`, next to the other bind registrations (both branches of the `PDZ_NO_MODS` #if):

```cpp
#include "mod/render/native/native.h"
...
pdz::native::RegisterBinds(); // bind_native_output, default F9
```

Without it the Swap path polls `GetAsyncKeyState(VK_F9)` (edge-triggered) once per frame; with it the key is
rebindable from `mapping.toml` like the other binds. Never both: `RegisterBinds` disables the polling.

## 3. CMakeLists.txt

Append to `PERFECTDARKZERORECOMP_SOURCES`:

```
src/mod/render/native/native_cvars.cpp
src/mod/render/native/native_device.cpp
src/mod/render/native/native_passes.cpp
src/mod/render/native/native_textures.cpp
src/mod/render/native/native_surfaces.cpp
src/mod/render/native/native_present.cpp
```

No new link inputs: `D3D12SerializeRootSignature` is fetched from the already-loaded `d3d12.dll` and
`D3DCompile` from `d3dcompiler_47.dll` (LoadLibrary) at run time; the D3D12 COM interfaces need no import
library. `xxhash.h` is already on the include path (the shadow files use it too).

## 4. Switches

`pdz_native_renderer = true` (settings toml or `--pdz_native_renderer`), `pdz_native_scale` 1..4 (latched at the
first surface), `pdz_native_present_mode` 0/1 (hot-reloadable), `pdz_native_output` (start with the native
frame selected). The GPU plugin must stay loaded (`gpu_plugin = "xenos"`, D3D12 backend): the core runs on
its device and presenter.
