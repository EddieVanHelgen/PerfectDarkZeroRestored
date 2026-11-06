#pragma once

// PDZ Restored (2026-11-03): DxcCreateInstance comes from the dxcompiler.dll the game loaded (pdzDxcCreate),
// not from an import library. Null: compile() returns nullptr.
using PdzDxcCreateProc = HRESULT(__stdcall *)(REFCLSID, REFIID, LPVOID *);
extern PdzDxcCreateProc pdzDxcCreate;

struct DxcCompiler
{
    IDxcCompiler3* dxcCompiler = nullptr;

    DxcCompiler();
    ~DxcCompiler();

    IDxcBlob* compile(const std::string& shaderSource, bool compilePixelShader, bool compileLibrary, bool compileSpirv);
};
