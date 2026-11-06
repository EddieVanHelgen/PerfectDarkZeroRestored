# XenosRecomp, pdz branch (vendored)

The Xbox 360 shader translator the native renderer uses, built into PDZRestored.exe (no separate XenosRecomp.exe).

- Upstream: https://github.com/hedge-dev/XenosRecomp at 990d03b, MIT licence (`LICENSE.md`, hedge-dev and
  contributors).
- Plus the project's 9 `pdz` commits (b33529e .. b668b30, 25-26 Oct 2026, from the checkout
  `C:\Games\PDZRenderResearch\native\tools\XenosRecomp`): the legacy 0x102A0E0x container parser and raw microcode
  input, translation without a constant table, a0/aL relative constants, loops, per-fetch texture filter overrides,
  two colour targets, vertex texture fetches, the guest -> host viewport transform, and every interpolator declared
  in pixel shaders. The translated shader cache was made with b668b30.
- Published fork: https://github.com/EddieVanHelgen/XenosRecomp, branch `pdz`. Its commits were re-authored for
  publishing (same trees), so there the 9 commits are 7fbff7a .. 7ca36b8: b668b30 here and in the translator id
  (`native_generate.cpp`) is 7ca36b8 there. Branch `pdz-oracle` adds 5 later commits (tip 7ddf3f1).
- Copied at b668b30: `constant_table.h`, `dxc_compiler.cpp/.h`, `pdz_recompiler.cpp`, `shader.h`, `shader_code.h`,
  `shader_common.h`, `shader_common_pdz.hlsli`, `shader_pdz.h`, `shader_recompiler.cpp/.h`, `pch.h`.
- Changed here (2026-11-03):
  - `pch.h`: the smol-v / zstd / xxHash / `<execution>` includes are removed (only the Unleashed CLI used them).
  - `dxc_compiler.cpp/.h`: `DxcCreateInstance` comes from the `dxcompiler.dll` the game loaded (`pdzDxcCreate`),
    not from an import library. The compile arguments are unchanged.
  - `xenosrecomp_pdz.cpp/.h` (new): the CLI's `pdzLoadShader` / `pdzTranslate` (`pdz_main.cpp`) on memory. The CLI's
    `main.cpp` / `pdz_main.cpp` are not included.
- `shader_common_pdz.hlsli` goes byte for byte into every translated shader, so `.gitattributes` keeps git from
  changing its line endings.
