// PDZ Restored (2026-11-03): the XenosRecomp fork (pdz branch) as a library inside the game, in place of the
// XenosRecomp.exe command line (`XenosRecomp --pdz in out.hlsl [--type vs|ps] --json out.json --dxil out.dxil`).
// Same steps as the CLI's pdzTranslate (pdz_main.cpp at b668b30), on memory instead of files.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace xenosrecomp_pdz
{
  enum class Type
  {
    kAuto, // a 0x102A0E0x container says what it is; raw microcode needs kVertex / kPixel
    kVertex,
    kPixel,
  };

  struct Result
  {
    // The CLI's exit codes: 0 ok, 1 the input can't be read as a shader, 3 unsupported features (the HLSL / JSON /
    // DXIL are still made, as the CLI writes them), 4 DXC failed.
    int code = 1;
    bool pixel = false;
    std::string hlsl, json;
    std::vector<uint8_t> dxil; // empty without `compile` or when DXC failed
    std::string message;       // what went wrong (the CLI's stderr)
  };

  // The fork's shared HLSL include (shader_common_pdz.hlsli), byte-exact as the translated shaders embed it.
  std::string_view CommonInclude();

  // Translates one container / raw microcode. `compile`: also DXIL, with DXC (SetDxcCreate first). Thread-safe:
  // each call has its own recompiler and DXC compiler.
  Result Translate(const uint8_t *data, size_t size, Type type, bool compile);

  // DxcCreateInstance from the dxcompiler.dll the game loaded (with dxil.dll, so the DXIL is signed).
  void SetDxcCreate(void *dxc_create_instance);
} // namespace xenosrecomp_pdz
