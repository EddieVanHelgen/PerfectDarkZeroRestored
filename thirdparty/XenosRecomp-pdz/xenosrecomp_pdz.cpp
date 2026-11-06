// PDZ Restored (2026-11-03): the fork as a library (xenosrecomp_pdz.h). The load / translate steps follow
// pdz_main.cpp's pdzLoadShader / pdzTranslate at b668b30 line for line, so the output is the CLI's.
#include "xenosrecomp_pdz.h"

#include "shader_pdz.h"
#include "shader_recompiler.h"
#include "dxc_compiler.h"

namespace xenosrecomp_pdz
{
  namespace
  {
    // shader_common_pdz.hlsli's bytes, written by CMake at configure time (cmake/pdz_xenosrecomp.cmake).
    const unsigned char kInclude[] = {
#include "xenosrecomp_include.inc"
    };

    bool LoadShader(const uint8_t *data, size_t size, Type type, pdz::Shader &shader, std::string &message)
    {
      if (size == 0)
      {
        message = "empty input";
        return false;
      }
      std::string error;
      if (pdz::isLegacyContainer(data, size))
      {
        if (!pdz::parseLegacyContainer(data, size, shader, error))
        {
          message = error;
          return false;
        }
        return true;
      }
      if (type == Type::kAuto)
      {
        message = "raw microcode needs --type vs|ps";
        return false;
      }
      if (!pdz::parseRawUcode(data, size, type == Type::kPixel, shader, error))
      {
        message = error;
        return false;
      }
      return true;
    }
  } // namespace

  std::string_view CommonInclude()
  {
    return std::string_view(reinterpret_cast<const char *>(kInclude), sizeof(kInclude));
  }

  void SetDxcCreate(void *dxc_create_instance)
  {
    pdzDxcCreate = reinterpret_cast<PdzDxcCreateProc>(dxc_create_instance);
  }

  Result Translate(const uint8_t *data, size_t size, Type type, bool compile)
  {
    Result r;
    pdz::Shader shader;
    if (!LoadShader(data, size, type, shader, r.message))
    {
      r.code = 1;
      return r;
    }
    ShaderRecompiler recompiler;
    recompiler.recompilePdz(shader, CommonInclude());
    r.pixel = recompiler.isPixelShader;
    r.hlsl = recompiler.out;
    r.json = recompiler.pdzJson;
    r.code = 0;
    if (!recompiler.pdz.unsupported.empty())
    {
      std::set<std::string> unique(recompiler.pdz.unsupported.begin(), recompiler.pdz.unsupported.end());
      for (const auto &u : unique)
      {
        r.message += (r.message.empty() ? "unsupported: " : ", ") + u;
      }
      r.code = 3;
    }
    if (compile)
    {
      DxcCompiler compiler;
      IDxcBlob *dxil = compiler.compile(recompiler.out, recompiler.isPixelShader, false, false);
      if (dxil == nullptr)
      {
        r.message += r.message.empty() ? "dxc failed" : "; dxc failed";
        r.code = 4;
        return r;
      }
      const auto *bytes = static_cast<const uint8_t *>(dxil->GetBufferPointer());
      r.dxil.assign(bytes, bytes + dxil->GetBufferSize());
      dxil->Release();
    }
    return r;
  }
} // namespace xenosrecomp_pdz
