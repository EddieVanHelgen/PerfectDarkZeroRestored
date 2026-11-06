// PGO training builds only (PDZ_PGO=gen, cmake/pdz_compiler.cmake, docs/PERFORMANCE.md "Compiler experiments"):
// compiled into the exe only in that configuration.
//
// Each instrumented module (the exe, rexruntime.dll, rexgpu-xenos.dll) links its own copy of clang's profile
// runtime, which writes that module's counters to LLVM_PROFILE_FILE from an atexit handler. The SDK ends the process
// with std::_Exit (rex_app.cpp OnClosing): no atexit handlers run in the exe, so its profile, the one with the
// generated code, would be lost. WriteProfiles() runs from the app's close-request hook
// (perfectdarkzerorecomp_app.h) and writes the exe's profile, then asks each SDK DLL to write its own through the
// __llvm_profile_write_file export pdz_compiler.cmake adds to them (scope "all"). The DLLs write once more from
// DLL_PROCESS_DETACH (ExitProcess runs it), to the same file name, with the final counts.
#include <windows.h>

#include <rex/logging.h>

extern "C" int __llvm_profile_write_file(void);

namespace pdz::pgo
{
  void WriteProfiles()
  {
    const int exe_rc = __llvm_profile_write_file();
    int dlls = 0, dll_failed = 0;
    for (const wchar_t *name : {L"rexruntime.dll", L"rexgpu-xenos.dll"})
    {
      HMODULE module = GetModuleHandleW(name);
      if (!module)
      {
        continue;
      }
      using WriteFn = int (*)(void);
      auto write = reinterpret_cast<WriteFn>(GetProcAddress(module, "__llvm_profile_write_file"));
      if (!write)
      {
        continue;
      }
      ++dlls;
      if (write() != 0)
      {
        ++dll_failed;
      }
    }
    char pattern[512] = {};
    const DWORD n = GetEnvironmentVariableA("LLVM_PROFILE_FILE", pattern, sizeof(pattern));
    REXLOG_INFO("pdz_pgo: profiles written at close: exe {}, SDK DLLs {} ({} failed); LLVM_PROFILE_FILE '{}'",
                exe_rc == 0 ? "ok" : "FAILED", dlls, dll_failed,
                n && n < sizeof(pattern) ? pattern : "(unset: default.profraw in the working folder)");
  }
} // namespace pdz::pgo
