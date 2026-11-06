// Crash breadcrumbs (log 301: a guest access violation 2 s into a
// 31-bot Infection match on Desert left only "read of guest 0x01000004", no
// dump, no host address). A vectored exception handler registered LAST (the
// SDK's own, registered first, resolves the guest MMIO faults it owns and
// declines the rest) logs the host fault address as module+offset, a stack
// walk in the same form (resolved against PDZRestored.map, the exe
// is linked with /MAP) and writes a minidump next to the logs; then the
// SDK's unhandled-exception path goes on as before.
//
// Log 101 (a REX_FATAL, "Call to invalid or unregistered function at guest
// address 0x00000000 (LR 0x00000000)"): REX_FATAL logs, flushes and calls
// std::abort, which no exception handler sees, so that crash left no stack and
// no dump. A SIGABRT handler now writes the same breadcrumbs (the stack from
// the abort, through the caller of the fatal call) and a minidump, then lets
// the abort go on.
#include <windows.h>
#include <dbghelp.h>
#include <atomic>
#include <csignal>
#include <cstring>
#include <string>
#include <fmt/format.h>
#include <rex/logging.h>
#include "render/guest_ip_profile.h" // InProfilerUnwind
#pragma comment(lib, "dbghelp.lib")

namespace
{
  std::atomic<bool> g_crash_busy{false};

  std::string Where(uintptr_t address)
  {
    HMODULE module = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(address), &module) &&
        module)
    {
      char path[MAX_PATH] = {};
      GetModuleFileNameA(module, path, MAX_PATH);
      const char *name = std::strrchr(path, '\\');
      name = name ? name + 1 : path;
      return fmt::format("{}+0x{:X}", name, address - reinterpret_cast<uintptr_t>(module));
    }
    return fmt::format("0x{:016X}", address);
  }

  void Report(CONTEXT ctx, EXCEPTION_POINTERS *info);

  LONG CALLBACK CrashHandler(EXCEPTION_POINTERS *info)
  {
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    if (pdz::ipprof::InProfilerUnwind())
    {
      return EXCEPTION_CONTINUE_SEARCH; // pdz_guest_ip_profile's guarded unwind handles its own faults
    }
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_PRIV_INSTRUCTION &&
        code != EXCEPTION_INT_DIVIDE_BY_ZERO && code != EXCEPTION_STACK_OVERFLOW && code != EXCEPTION_IN_PAGE_ERROR)
    {
      return EXCEPTION_CONTINUE_SEARCH;
    }
    if (g_crash_busy.exchange(true))
    {
      return EXCEPTION_CONTINUE_SEARCH;
    }
    CONTEXT ctx = *info->ContextRecord;
    const bool av = code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR;
    REXLOG_INFO("pdz crash: exception 0x{:08X} at {} (rip 0x{:016X}){}; thread 0x{:X}", code, Where(ctx.Rip), ctx.Rip,
                av ? fmt::format(", {} 0x{:016X}", info->ExceptionRecord->ExceptionInformation[0] == 1 ? "write of" : "read of",
                                 uintptr_t(info->ExceptionRecord->ExceptionInformation[1]))
                   : std::string(),
                GetCurrentThreadId());
    Report(ctx, info);
    return EXCEPTION_CONTINUE_SEARCH;
  }

  // The stack walk from `ctx` and a minidump (with the exception when there is
  // one), both logged.
  void Report(CONTEXT ctx, EXCEPTION_POINTERS *info)
  {
    HANDLE process = GetCurrentProcess(), thread = GetCurrentThread();
    SymInitialize(process, nullptr, TRUE);
    STACKFRAME64 frame = {};
    frame.AddrPC.Offset = ctx.Rip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = ctx.Rbp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = ctx.Rsp;
    frame.AddrStack.Mode = AddrModeFlat;
    std::string trace;
    for (int i = 0; i < 40; ++i)
    {
      if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &ctx, nullptr, SymFunctionTableAccess64,
                       SymGetModuleBase64, nullptr) ||
          !frame.AddrPC.Offset)
      {
        break;
      }
      trace += fmt::format(" [{}] {}", i, Where(uintptr_t(frame.AddrPC.Offset)));
    }
    REXLOG_INFO("pdz crash: stack:{}", trace.empty() ? std::string(" (none)") : trace);
    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string dir(exe);
    const size_t slash = dir.find_last_of('\\');
    dir = (slash == std::string::npos ? std::string(".") : dir.substr(0, slash)) + "\\logs";
    CreateDirectoryA(dir.c_str(), nullptr);
    SYSTEMTIME st;
    GetLocalTime(&st);
    const std::string path = fmt::format("{}\\crash_{:04}{:02}{:02}_{:02}{:02}{:02}_{}.dmp", dir, st.wYear, st.wMonth, st.wDay,
                                         st.wHour, st.wMinute, st.wSecond, GetCurrentProcessId());
    HANDLE file = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE)
    {
      MINIDUMP_EXCEPTION_INFORMATION mei = {GetCurrentThreadId(), info, FALSE};
      const MINIDUMP_TYPE type = MINIDUMP_TYPE(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithDataSegs |
                                               MiniDumpWithThreadInfo | MiniDumpWithHandleData);
      const BOOL ok = MiniDumpWriteDump(process, GetCurrentProcessId(), file, type, info ? &mei : nullptr, nullptr, nullptr);
      CloseHandle(file);
      REXLOG_INFO("pdz crash: minidump {} {}", ok ? "written to" : "FAILED at", path);
    }
    else
    {
      REXLOG_INFO("pdz crash: minidump could not be created at {} (error {})", path, GetLastError());
    }
    if (auto logger = rex::GetLogger())
    {
      logger->flush();
    }
  }

  // std::abort (REX_FATAL and the like): the same breadcrumbs, then the abort
  // goes on (the default action is restored first).
  void AbortHandler(int)
  {
    std::signal(SIGABRT, SIG_DFL);
    if (!g_crash_busy.exchange(true))
    {
      CONTEXT ctx = {};
      RtlCaptureContext(&ctx);
      REXLOG_INFO("pdz crash: abort (a fatal error; its message is the line before) on thread 0x{:X}", GetCurrentThreadId());
      Report(ctx, nullptr);
    }
    std::abort();
  }

  struct InstallCrashHandler
  {
    InstallCrashHandler()
    {
      AddVectoredExceptionHandler(0, CrashHandler); // last: after the SDK's own
      std::signal(SIGABRT, AbortHandler);
    }
  } g_install_crash_handler;
} // namespace
