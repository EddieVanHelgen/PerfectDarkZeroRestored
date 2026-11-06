// pdz_guest_ip_profile: the game thread's instruction-pointer sampler (guest_ip_profile.h). Diagnostic only: reads
// the game thread's registers and stack while it is suspended for a few microseconds; writes nothing the game sees.
#include "guest_ip_profile.h"

#include "cpu_placement.h"
#include "game_pace.h"
#include "game_thread_sampler.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <windows.h>

#include <psapi.h>
#include <tlhelp32.h>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/logging/api.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

REXCVAR_DEFINE_INT32(pdz_guest_ip_profile, 0, "Debug",
                     "Diagnostic (31 Oct, docs/RARE_CODE_PROFILE.md): sample the game thread every N microseconds "
                     "(0 = off; 1000 = 1 kHz) by suspending it, reading its instruction pointer and unwinding its host "
                     "stack; each frame is mapped to its recompiled guest function (or to the mod / SDK / OS code it "
                     "is in). Level frames only, inside pdz_guest_ip_profile_from_s / _to_s. Writes "
                     "logs\\guest_ip_profile_<log number>.txt when the window closes (tools/perf/ip_profile_report.py "
                     "makes the tables). Stops the game thread a few microseconds per sample; off costs one cvar read "
                     "per Swap")
    .range(0, 100000);
REXCVAR_DEFINE_INT32(pdz_guest_ip_profile_from_s, 0, "Debug",
                     "pdz_guest_ip_profile: count frames from this many seconds after the process started (0 = from "
                     "the first level frame)")
    .range(0, 100000);
REXCVAR_DEFINE_INT32(pdz_guest_ip_profile_threads, 0, "Debug",
                    "pdz_guest_ip_profile: also sample the other guest threads (named '<name> (F8xxxxxx)' by the SDK) "
                    "inside the time window, any scene: 0 = no, 1 = one per tick in turn after the Swap thread, 2 = "
                    "the busiest of them (re-chosen after each pass) every tick and the rest in turn every 4th tick. "
                    "Their self / inclusive / caller counts go to the same file per thread")
    .range(0, 2);
REXCVAR_DEFINE_INT32(pdz_guest_ip_profile_spike_ms, 0, "Debug",
                     "pdz_guest_ip_profile: count only level frames (Swap intervals) of at least this many ms, and "
                     "write one 'spikeframe' line per such frame (its end time, length, samples and its top guest "
                     "functions), so the profile shows what ran in the hitches only (docs/MANSION_HITCH.md). 0 = every "
                     "frame")
    .range(0, 100000);
REXCVAR_DEFINE_INT32(pdz_guest_ip_profile_to_s, 0, "Debug",
                     "pdz_guest_ip_profile: count frames up to this many seconds after the process started (0 = to the "
                     "end)")
    .range(0, 100000);

// The hooked functions' original bodies (__imp__sub_X): PPCFuncMappings points the hooked ones at the hook.
#define PDZ_IP_HOOK(name, address) REX_EXTERN(__imp__##name);
#include "guest_ip_profile_hooks.inc"
#undef PDZ_IP_HOOK

namespace pdz::ipprof
{
  namespace
  {
    // A frame's identity: the kind in the top byte, the guest address or the module-relative function start below.
    enum Kind : uint64_t
    {
      kGuest = 1,      // a recompiled guest function (unhooked, or a hooked one's original body)
      kHook = 2,       // the hook replacing a guest function (the mod's C++, which calls the original)
      kExe = 3,        // other code in the exe: the mod, the native renderer, the SDK's static parts, the CRT
      kRuntime = 4,    // rexruntime.dll (the SDK: kernel exports, memory, threads)
      kGpu = 5,        // rexgpu-*.dll
      kNtdll = 6,      // ntdll.dll
      kKernel = 7,     // kernelbase.dll / kernel32.dll
      kOtherModule = 8,
      kUnknown = 9,    // no module (JIT code, a bad unwind)
      kWait = 10,      // an ntdll / kernelbase wait or yield export (value = its index in kWaitNames)
    };
    constexpr uint64_t Id(uint64_t kind, uint64_t value) { return (kind << 56) | (value & 0x00FFFFFFFFFFFFFFull); }
    constexpr uint64_t KindOf(uint64_t id) { return id >> 56; }

    constexpr uint32_t kDepth = 48;
    constexpr const char *kWaitNames[] = {"NtWaitForSingleObject",  "NtWaitForMultipleObjects",
                                          "NtDelayExecution",       "NtYieldExecution",
                                          "NtWaitForAlertByThreadId", "NtSignalAndWaitForSingleObject",
                                          "NtRemoveIoCompletion",   "ZwWaitForWorkViaWorkerFactory",
                                          "NtWaitForKeyedEvent",    "NtReleaseKeyedEvent"};

    struct Module
    {
      uint64_t lo = 0, hi = 0;
      uint64_t kind = kOtherModule;
      std::string name;
    };

    struct Entry
    {
      uint32_t rva;
      uint32_t guest;
      bool hook;
    };

    std::vector<Module> g_modules;
    std::vector<Entry> g_exe;           // sorted by rva (function starts in the exe)
    std::vector<uint64_t> g_wait_starts; // the wait exports' addresses (index = kWaitNames)
    uint64_t g_exe_base = 0;

    std::atomic<bool> g_started{false}, g_stop{false}, g_exited{false}, g_written{false};
    std::atomic<uint32_t> g_period_us{1000};
    HANDLE g_thread = nullptr;     // the game thread (suspend / context)
    uint64_t g_stack_lo = 0, g_stack_hi = 0;
    LARGE_INTEGER g_qpc_freq{};
    int64_t g_process_start_qpc = 0;

    // The stream (g_mutex): samples [kSample | where, n, ids...] and frame ends [kFrame, scene, frame_us, end_s].
    constexpr uint64_t kSampleTag = 0xFFFFFFFF00000000ull;
    constexpr uint64_t kFrameTag = 0xFFFFFFFE00000000ull;
    std::mutex g_mutex;
    std::vector<uint64_t> g_stream;
    int64_t g_prev_swap_qpc = 0;

    // Folded totals (the sampler thread, or the exit writer after it stopped).
    struct Totals
    {
      uint64_t frames = 0, samples = 0, dropped_frames = 0, dropped_samples = 0;
      double frame_ms = 0.0;
      double suspend_us_sum = 0.0, suspend_us_max = 0.0;
      uint64_t suspend_n = 0, unwind_frames = 0, failed = 0;
      std::unordered_map<uint64_t, uint64_t> where, category, self, inner, incl, caller;
      std::map<std::pair<uint64_t, uint32_t>, uint64_t> host;
      std::unordered_map<std::string, uint64_t> stacks; // collapsed stacks (ids' bytes, top first) -> samples
      std::vector<std::string> spike_frames;            // pdz_guest_ip_profile_spike_ms: one line per kept frame
    } g_totals;
    size_t g_hooked_n = 0;
    // The sampler's own cost (sampler thread only).
    double g_cost_us_sum = 0.0, g_cost_us_max = 0.0;
    uint64_t g_cost_n = 0, g_failed = 0, g_frames_sum = 0;

    double QpcUs(int64_t a, int64_t b) { return double(b - a) * 1e6 / double(g_qpc_freq.QuadPart); }
    int64_t QpcNow()
    {
      LARGE_INTEGER v;
      QueryPerformanceCounter(&v);
      return v.QuadPart;
    }

    void BuildTables()
    {
      HMODULE exe = GetModuleHandleW(nullptr);
      g_exe_base = reinterpret_cast<uint64_t>(exe);
      for (const PPCFuncMapping *m = PPCFuncMappings; m->guest || m->host; ++m)
      {
        if (m->host && m->guest >= 0x80000000u && m->guest < 0x100000000ull)
        {
          const uint64_t h = reinterpret_cast<uint64_t>(m->host);
          if (h > g_exe_base && h - g_exe_base < 0x100000000ull)
          {
            g_exe.push_back({uint32_t(h - g_exe_base), uint32_t(m->guest), false});
          }
        }
      }
      // Hooked functions: the table's entry is the hook; the original body is __imp__sub_X.
      std::unordered_map<uint32_t, uint32_t> hooked; // guest -> original rva
#define PDZ_IP_HOOK(name, address)                                                                                     \
  {                                                                                                                    \
    const uint64_t h = reinterpret_cast<uint64_t>(&__imp__##name);                                                     \
    hooked[uint32_t(address)] = uint32_t(h - g_exe_base);                                                              \
  }
#include "guest_ip_profile_hooks.inc"
#undef PDZ_IP_HOOK
      for (Entry &e : g_exe)
      {
        auto it = hooked.find(e.guest);
        if (it != hooked.end() && it->second != e.rva)
        {
          e.hook = true; // the table points at the hook, not at the original body
        }
      }
      for (const auto &kv : hooked)
      {
        g_exe.push_back({kv.second, kv.first, false});
      }
      g_hooked_n = hooked.size();
      std::sort(g_exe.begin(), g_exe.end(), [](const Entry &a, const Entry &b) {
        return a.rva != b.rva ? a.rva < b.rva : (a.hook < b.hook); // the original before a hook at the same rva
      });
      g_exe.erase(std::unique(g_exe.begin(), g_exe.end(), [](const Entry &a, const Entry &b) { return a.rva == b.rva; }),
                  g_exe.end());

      // psapi's K32 functions from kernel32 at run time (as native_memlog.cpp: no import library needed).
      using EnumFn = BOOL(WINAPI *)(HANDLE, HMODULE *, DWORD, LPDWORD);
      using InfoFn = BOOL(WINAPI *)(HANDLE, HMODULE, LPMODULEINFO, DWORD);
      using NameFn = DWORD(WINAPI *)(HANDLE, HMODULE, LPSTR, DWORD);
      HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
      auto enum_modules = k32 ? reinterpret_cast<EnumFn>(reinterpret_cast<void *>(GetProcAddress(k32, "K32EnumProcessModules"))) : nullptr;
      auto module_info = k32 ? reinterpret_cast<InfoFn>(reinterpret_cast<void *>(GetProcAddress(k32, "K32GetModuleInformation"))) : nullptr;
      auto module_name = k32 ? reinterpret_cast<NameFn>(reinterpret_cast<void *>(GetProcAddress(k32, "K32GetModuleBaseNameA"))) : nullptr;
      HMODULE mods[512];
      DWORD needed = 0;
      if (enum_modules && module_info && module_name && enum_modules(GetCurrentProcess(), mods, sizeof(mods), &needed))
      {
        for (DWORD i = 0; i < std::min<DWORD>(needed / sizeof(HMODULE), 512); ++i)
        {
          MODULEINFO mi{};
          char name[MAX_PATH] = {};
          if (!module_info(GetCurrentProcess(), mods[i], &mi, sizeof(mi)))
          {
            continue;
          }
          module_name(GetCurrentProcess(), mods[i], name, MAX_PATH);
          Module m;
          m.lo = reinterpret_cast<uint64_t>(mi.lpBaseOfDll);
          m.hi = m.lo + mi.SizeOfImage;
          m.name = name;
          std::string lower = m.name;
          for (char &ch : lower)
          {
            ch = char(std::tolower(static_cast<unsigned char>(ch)));
          }
          m.kind = m.lo == g_exe_base                   ? kExe
                   : lower == "rexruntime.dll"          ? kRuntime
                   : lower.rfind("rexgpu", 0) == 0      ? kGpu
                   : lower == "ntdll.dll"               ? kNtdll
                   : lower == "kernelbase.dll" || lower == "kernel32.dll" ? kKernel
                                                        : kOtherModule;
          g_modules.push_back(m);
        }
      }
      HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
      for (const char *n : kWaitNames)
      {
        g_wait_starts.push_back(ntdll ? reinterpret_cast<uint64_t>(GetProcAddress(ntdll, n)) : 0);
      }
    }

    // The primary function start of a code address: RtlLookupFunctionEntry's entry, followed through chained unwind
    // info (a cold chunk's entry chains to the function's own).
    bool FunctionStart(uint64_t rip, uint64_t &start)
    {
      DWORD64 image = 0;
      PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(rip, &image, nullptr);
      if (!f)
      {
        return false;
      }
      for (int i = 0; i < 8; ++i)
      {
        const uint8_t *info = reinterpret_cast<const uint8_t *>(image + f->UnwindData);
        if (!((info[0] >> 3) & UNW_FLAG_CHAININFO))
        {
          break;
        }
        const uint32_t codes = info[2];
        f = reinterpret_cast<PRUNTIME_FUNCTION>(const_cast<uint8_t *>(info + 4 + 2 * ((codes + 1) & ~1u)));
      }
      start = image + f->BeginAddress;
      return true;
    }

    uint64_t Classify(uint64_t rip)
    {
      const Module *mod = nullptr;
      for (const Module &m : g_modules)
      {
        if (rip >= m.lo && rip < m.hi)
        {
          mod = &m;
          break;
        }
      }
      if (!mod)
      {
        return Id(kUnknown, rip);
      }
      uint64_t start = 0;
      const bool known = FunctionStart(rip, start);
      if (mod->kind == kExe)
      {
        if (known)
        {
          const uint32_t rva = uint32_t(start - g_exe_base);
          auto it = std::lower_bound(g_exe.begin(), g_exe.end(), rva, [](const Entry &e, uint32_t r) { return e.rva < r; });
          if (it != g_exe.end() && it->rva == rva)
          {
            return Id(it->hook ? kHook : kGuest, it->guest);
          }
          return Id(kExe, rva);
        }
        return Id(kExe, rip - g_exe_base); // a leaf without unwind data: its address, not a start
      }
      if (known && (mod->kind == kNtdll || mod->kind == kKernel))
      {
        for (size_t i = 0; i < g_wait_starts.size(); ++i)
        {
          if (g_wait_starts[i] && g_wait_starts[i] == start)
          {
            return Id(kWait, i);
          }
        }
      }
      const uint64_t rva = (known ? start : rip) - mod->lo;
      if (mod->kind == kOtherModule)
      {
        return Id(kOtherModule, (uint64_t(mod - g_modules.data()) << 32) | rva);
      }
      return Id(mod->kind, rva);
    }

    // The unwind of a stopped thread (Capture). A function that addresses its frame through a register (UNWIND_INFO
    // FrameRegister) is only unwound when that register points into the thread's stack (main_outpost_prof_b, 31 Oct:
    // RtlVirtualUnwind read address 8 through a frame register of 0); the stack pointer is checked after every frame.
    std::atomic<uint64_t> g_unwind_faults{0}, g_unwind_frame_stops{0};
    thread_local bool t_unwinding = false;

    void UnwindFrames(CONTEXT &c, uint64_t lo, uint64_t hi, uint64_t *rips, uint32_t *n)
    {
      while (*n < kDepth)
      {
        rips[(*n)++] = c.Rip;
        DWORD64 image = 0;
        PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(c.Rip, &image, nullptr);
        if (!f)
        {
          if (c.Rsp < lo || c.Rsp + 8 > hi)
          {
            return;
          }
          c.Rip = *reinterpret_cast<const uint64_t *>(c.Rsp);
          c.Rsp += 8;
        }
        else
        {
          const uint8_t *info = reinterpret_cast<const uint8_t *>(image + f->UnwindData);
          for (int i = 0; i < 8 && ((info[0] >> 3) & UNW_FLAG_CHAININFO); ++i)
          {
            const auto *chained =
                reinterpret_cast<const RUNTIME_FUNCTION *>(info + 4 + 2 * ((uint32_t(info[2]) + 1) & ~1u));
            info = reinterpret_cast<const uint8_t *>(image + chained->UnwindData);
          }
          const uint32_t frame_register = info[3] & 0x0F;
          if (frame_register)
          {
            const uint64_t v = (&c.Rax)[frame_register];
            if (v < lo || v >= hi)
            {
              g_unwind_frame_stops.fetch_add(1, std::memory_order_relaxed);
              return;
            }
          }
          PVOID handler = nullptr;
          DWORD64 establisher = 0;
          RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, c.Rip, f, &c, &handler, &establisher, nullptr);
        }
        if (!c.Rip || c.Rsp < lo || c.Rsp >= hi)
        {
          return;
        }
      }
    }

    // A fault in the unwind ends the walk (the frames so far are kept); crash_dump.cpp leaves it alone
    // (InProfilerUnwind), so it never reaches the crash report.
    void UnwindGuarded(CONTEXT &c, uint64_t lo, uint64_t hi, uint64_t *rips, uint32_t *n)
    {
      t_unwinding = true;
      __try
      {
        UnwindFrames(c, lo, hi, rips, n);
      }
      __except (EXCEPTION_EXECUTE_HANDLER)
      {
        g_unwind_faults.fetch_add(1, std::memory_order_relaxed);
      }
      t_unwinding = false;
    }

    // One sample: suspend, read, unwind, resume (nothing that allocates or logs while the thread is stopped).
    uint32_t Capture(HANDLE thread, uint64_t lo, uint64_t hi, bool swap_thread, uint64_t *rips, uint8_t &where)
    {
      CONTEXT c{};
      c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
      const int64_t t0 = QpcNow();
      if (SuspendThread(thread) == DWORD(-1))
      {
        ++g_failed;
        return 0;
      }
      uint32_t n = 0;
      if (GetThreadContext(thread, &c))
      {
        where = swap_thread ? pdz::gtsample::g_where.load(std::memory_order_relaxed) : 0;
        UnwindGuarded(c, lo, hi, rips, &n);
      }
      else
      {
        ++g_failed;
      }
      ResumeThread(thread);
      const double us = QpcUs(t0, QpcNow());
      g_cost_us_sum += us;
      g_cost_us_max = std::max(g_cost_us_max, us);
      ++g_cost_n;
      g_frames_sum += n;
      return n;
    }

    // pdz_guest_ip_profile_threads: the other guest threads (OS name "<name> (F8xxxxxx)", the SDK's XThread naming),
    // one sampled per tick in turn after the Swap thread; counted per thread inside the time window, any scene.
    struct OtherThread
    {
      DWORD id = 0;
      HANDLE handle = nullptr;
      uint64_t lo = 0, hi = 0;
      std::string name;
      uint64_t samples = 0, waits = 0;
      uint64_t recent = 0, recent_busy = 0; // pdz_guest_ip_profile_threads 2: the last pass, for the focus choice
      std::unordered_map<uint64_t, uint64_t> self, inner, incl, caller;
      std::unordered_map<std::string, uint64_t> stacks;
      std::unordered_map<uint64_t, uint64_t> rip; // the exact top-frame address (exe rva) when it is our own code
    };
    size_t g_focus = SIZE_MAX; // the busiest other thread (mode 2), sampled every tick
    size_t g_record_thread = SIZE_MAX; // "PDZ native record thread" (modes 1-2: sampled every tick too)
    std::vector<OtherThread> g_others;
    size_t g_other_next = 0;

    std::string ThreadName(HANDLE h)
    {
      using GetFn = HRESULT(WINAPI *)(HANDLE, PWSTR *);
      static GetFn get = [] {
        HMODULE k = GetModuleHandleW(L"kernel32.dll");
        return k ? reinterpret_cast<GetFn>(reinterpret_cast<void *>(GetProcAddress(k, "GetThreadDescription"))) : nullptr;
      }();
      PWSTR w = nullptr;
      std::string out;
      if (get && SUCCEEDED(get(h, &w)) && w)
      {
        for (const wchar_t *c = w; *c; ++c)
        {
          out.push_back(*c < 128 ? char(*c) : '?');
        }
        LocalFree(w);
      }
      return out;
    }

    // The thread's stack: [the reservation's base, StackBase) from its TEB (NtQueryInformationThread, class 0).
    bool StackOf(HANDLE h, uint64_t &lo, uint64_t &hi)
    {
      struct Basic
      {
        LONG status;
        PVOID teb;
        void *pid, *tid;
        ULONG_PTR affinity;
        LONG priority, base_priority;
      } info{};
      using QueryFn = LONG(WINAPI *)(HANDLE, int, PVOID, ULONG, PULONG);
      static QueryFn query = [] {
        HMODULE n = GetModuleHandleW(L"ntdll.dll");
        return n ? reinterpret_cast<QueryFn>(reinterpret_cast<void *>(GetProcAddress(n, "NtQueryInformationThread")))
                 : nullptr;
      }();
      if (!query || query(h, 0, &info, sizeof(info), nullptr) < 0 || !info.teb)
      {
        return false;
      }
      const NT_TIB *tib = static_cast<const NT_TIB *>(info.teb);
      MEMORY_BASIC_INFORMATION mbi{};
      if (!VirtualQuery(tib->StackLimit, &mbi, sizeof(mbi)))
      {
        return false;
      }
      lo = reinterpret_cast<uint64_t>(mbi.AllocationBase);
      hi = reinterpret_cast<uint64_t>(tib->StackBase);
      return hi > lo;
    }

    void SampleOther(OtherThread &o, uint64_t *rips, uint64_t *ids)
    {
      uint8_t unused = 0;
      const uint32_t m = Capture(o.handle, o.lo, o.hi, false, rips, unused);
      if (!m)
      {
        return;
      }
      ++o.samples;
      ++o.recent;
      int64_t inner = -1;
      uint64_t seen[kDepth];
      uint32_t ns = 0;
      for (uint32_t i = 0; i < m; ++i)
      {
        ids[i] = Classify(rips[i]);
        const uint64_t k = KindOf(ids[i]);
        if (k == kGuest || k == kHook)
        {
          if (inner < 0)
          {
            inner = i;
          }
          else if (ids[i] != ids[inner] && inner >= 0 && seen[0] == ids[inner] && ns == 1)
          {
            ++o.caller[(ids[inner] & 0xFFFFFFFFull) << 32 | (ids[i] & 0xFFFFFFFFull)];
          }
          bool dup = false;
          for (uint32_t q = 0; q < ns && !dup; ++q)
          {
            dup = seen[q] == ids[i];
          }
          if (!dup && ns < kDepth)
          {
            seen[ns++] = ids[i];
            ++o.incl[ids[i]];
          }
        }
      }
      ++o.self[ids[0]];
      if (KindOf(ids[0]) == kExe)
      {
        ++o.rip[rips[0] - g_exe_base];
      }
      ++o.stacks[std::string(reinterpret_cast<const char *>(ids), sizeof(uint64_t) * m)];
      const bool wait = KindOf(ids[0]) == kWait;
      o.waits += wait;
      o.recent_busy += !wait;
      if (inner >= 0)
      {
        ++o.inner[ids[inner]];
      }
    }

    void FindOtherThreads(DWORD swap_id)
    {
      HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
      if (snap == INVALID_HANDLE_VALUE)
      {
        return;
      }
      THREADENTRY32 te{};
      te.dwSize = sizeof(te);
      const DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId();
      for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
      {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == swap_id || te.th32ThreadID == self)
        {
          continue;
        }
        bool known = false;
        for (const OtherThread &o : g_others)
        {
          known = known || o.id == te.th32ThreadID;
        }
        if (known || g_others.size() >= 128)
        {
          continue;
        }
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE,
                              te.th32ThreadID);
        if (!h)
        {
          continue;
        }
        const std::string name = ThreadName(h);
        OtherThread o;
        const bool record = name.find("PDZ native record thread") != std::string::npos;
        if ((name.find(" (F8") == std::string::npos && !record) || !StackOf(h, o.lo, o.hi))
        {
          CloseHandle(h);
          continue;
        }
        o.id = te.th32ThreadID;
        o.handle = h;
        o.name = name;
        if (record)
        {
          g_record_thread = g_others.size();
        }
        g_others.push_back(std::move(o));
      }
      CloseHandle(snap);
    }

    std::string IdName(uint64_t id); // below

    // Folds every complete frame of the stream into the totals (the sampler thread; the exit writer after it stopped).
    void Fold(bool all)
    {
      std::vector<uint64_t> take;
      {
        std::lock_guard<std::mutex> lock(g_mutex);
        size_t end = 0;
        for (size_t i = 0; i < g_stream.size();)
        {
          if ((g_stream[i] & 0xFFFFFFFF00000000ull) == kFrameTag)
          {
            i += 4;
            end = i;
          }
          else
          {
            i += 2 + size_t(g_stream[i + 1]);
          }
        }
        if (all)
        {
          end = g_stream.size();
        }
        take.assign(g_stream.begin(), g_stream.begin() + ptrdiff_t(end));
        g_stream.erase(g_stream.begin(), g_stream.begin() + ptrdiff_t(end));
      }
      Totals &t = g_totals;
      size_t frame_begin = 0;
      for (size_t i = 0; i < take.size();)
      {
        if ((take[i] & 0xFFFFFFFF00000000ull) != kFrameTag)
        {
          i += 2 + size_t(take[i + 1]);
          continue;
        }
        const uint32_t scene = uint32_t(take[i + 1]);
        const double frame_us = double(take[i + 2]);
        const double end_s = double(take[i + 3]) / 1000.0;
        const int32_t from = REXCVAR_GET(pdz_guest_ip_profile_from_s), to = REXCVAR_GET(pdz_guest_ip_profile_to_s);
        const int32_t spike_ms = REXCVAR_GET(pdz_guest_ip_profile_spike_ms);
        const bool keep = scene == uint32_t(pdz::pace::Scene::kLevel) && (from <= 0 || end_s >= double(from)) &&
                          (to <= 0 || end_s <= double(to)) && frame_us > 0.0 && frame_us < 1e6 &&
                          (spike_ms <= 0 || frame_us >= double(spike_ms) * 1000.0);
        std::unordered_map<uint64_t, uint64_t> frame_inner, frame_top; // spike mode: this frame's counts
        size_t samples = 0;
        for (size_t j = frame_begin; j < i;)
        {
          const uint64_t n = take[j + 1];
          if (keep && n)
          {
            const uint64_t where = take[j] & 0xFF;
            const uint64_t *ids = &take[j + 2];
            ++t.samples;
            ++t.where[where];
            ++t.category[KindOf(ids[0])];
            ++t.self[ids[0]];
            ++t.stacks[std::string(reinterpret_cast<const char *>(ids), sizeof(uint64_t) * n)];
            // The innermost guest function (its own code or the host code it called), its guest caller, the
            // non-guest top frame's guest entry point, and every distinct guest function on the stack.
            int64_t inner = -1;
            for (uint64_t k = 0; k < n; ++k)
            {
              if (KindOf(ids[k]) == kGuest || KindOf(ids[k]) == kHook)
              {
                inner = int64_t(k);
                break;
              }
            }
            if (spike_ms > 0)
            {
              ++frame_top[ids[0]];
              ++frame_inner[inner >= 0 ? ids[inner] : ids[0]];
            }
            if (inner >= 0)
            {
              ++t.inner[ids[inner]];
              for (uint64_t k = uint64_t(inner) + 1; k < n; ++k)
              {
                if ((KindOf(ids[k]) == kGuest || KindOf(ids[k]) == kHook) && ids[k] != ids[inner])
                {
                  ++t.caller[(ids[inner] & 0xFFFFFFFFull) << 32 | (ids[k] & 0xFFFFFFFFull)];
                  break;
                }
              }
              if (inner > 0)
              {
                ++t.host[{ids[0], uint32_t(ids[inner])}]; // non-guest top frame, and the guest function under it
              }
            }
            else
            {
              ++t.host[{ids[0], 0u}];
            }
            uint64_t seen[kDepth];
            uint32_t ns = 0;
            for (uint64_t k = 0; k < n; ++k)
            {
              const uint64_t id = ids[k];
              if (KindOf(id) != kGuest && KindOf(id) != kHook)
              {
                continue;
              }
              bool dup = false;
              for (uint32_t s = 0; s < ns && !dup; ++s)
              {
                dup = seen[s] == id;
              }
              if (!dup && ns < kDepth)
              {
                seen[ns++] = id;
                ++t.incl[id];
              }
            }
          }
          ++samples;
          j += 2 + size_t(n);
        }
        if (keep)
        {
          ++t.frames;
          t.frame_ms += frame_us / 1000.0;
          if (spike_ms > 0 && t.spike_frames.size() < 4000)
          {
            const auto top = [](const std::unordered_map<uint64_t, uint64_t> &m) {
              std::vector<std::pair<uint64_t, uint64_t>> v(m.begin(), m.end());
              std::sort(v.begin(), v.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
              std::string out;
              for (size_t k = 0; k < v.size() && k < 8; ++k)
              {
                out += fmt::format("{}{}={}", k ? ";" : "", IdName(v[k].first), v[k].second);
              }
              return out.empty() ? std::string("-") : out;
            };
            t.spike_frames.push_back(fmt::format("spikeframe {:.3f} {:.2f} {} inner {} top {}", end_s,
                                                 frame_us / 1000.0, samples, top(frame_inner), top(frame_top)));
          }
        }
        else
        {
          ++t.dropped_frames;
          t.dropped_samples += samples;
        }
        i += 4;
        frame_begin = i;
      }
    }

    DWORD WINAPI Main(void *)
    {
      pdz::cpu::PlaceHelperThread("guest ip profiler");
      using SetThreadDescriptionFn = HRESULT(WINAPI *)(HANDLE, PCWSTR);
      if (HMODULE kernel = GetModuleHandleW(L"kernel32.dll"))
      {
        if (auto set_name = reinterpret_cast<SetThreadDescriptionFn>(
                reinterpret_cast<void *>(GetProcAddress(kernel, "SetThreadDescription"))))
        {
          set_name(GetCurrentThread(), L"pdz guest ip profiler");
        }
      }
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
      HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
      uint64_t rips[kDepth];
      uint64_t ids[kDepth];
      int64_t last_fold = QpcNow();
      int64_t last_scan = 0;
      const int32_t others = REXCVAR_GET(pdz_guest_ip_profile_threads);
      uint64_t tick = 0;
      while (!g_stop.load(std::memory_order_acquire))
      {
        LARGE_INTEGER due;
        due.QuadPart = -int64_t(g_period_us.load(std::memory_order_relaxed)) * 10;
        if (timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE))
        {
          WaitForSingleObject(timer, 1000);
        }
        else
        {
          Sleep(1);
        }
        if (g_stop.load(std::memory_order_acquire))
        {
          break;
        }
        uint8_t where = 0;
        const uint32_t n = Capture(g_thread, g_stack_lo, g_stack_hi, true, rips, where);
        for (uint32_t i = 0; i < n; ++i)
        {
          ids[i] = Classify(rips[i]);
        }
        {
          std::lock_guard<std::mutex> lock(g_mutex);
          if (g_stream.size() < (64u << 20) / 8)
          {
            g_stream.push_back(kSampleTag | where);
            g_stream.push_back(n);
            g_stream.insert(g_stream.end(), ids, ids + n);
          }
        }
        if (others)
        {
          if (!last_scan || QpcUs(last_scan, QpcNow()) > 5e6)
          {
            last_scan = QpcNow();
            FindOtherThreads(GetThreadId(g_thread));
          }
          const double now_s = QpcUs(g_process_start_qpc, QpcNow()) / 1e6;
          const int32_t from = REXCVAR_GET(pdz_guest_ip_profile_from_s), to = REXCVAR_GET(pdz_guest_ip_profile_to_s);
          ++tick;
          if (!g_others.empty() && (from <= 0 || now_s >= from) && (to <= 0 || now_s <= to))
          {
            // Mode 1: one other thread per tick in turn. Mode 2: the focus (the busiest other thread of the last
            // pass) every tick, the others in turn every 4th tick; a new focus after every pass over them all.
            if (others >= 2 && g_focus < g_others.size())
            {
              SampleOther(g_others[g_focus], rips, ids);
            }
            if (g_record_thread < g_others.size() && g_record_thread != g_focus)
            {
              SampleOther(g_others[g_record_thread], rips, ids); // our record thread, every tick
            }
            if (others < 2 || tick % 4 == 0)
            {
              const size_t i = g_other_next++ % g_others.size();
              if (i != g_focus && i != g_record_thread)
              {
                SampleOther(g_others[i], rips, ids);
              }
              if (others >= 2 && g_other_next % g_others.size() == 0 && g_other_next >= 8 * g_others.size())
              {
                double best = 0.0;
                for (size_t k = 0; k < g_others.size(); ++k)
                {
                  OtherThread &o = g_others[k];
                  const double busy = o.recent ? double(o.recent_busy) / double(o.recent) : 0.0;
                  if (o.recent >= 4 && busy > best && k != g_record_thread)
                  {
                    best = busy;
                    g_focus = k;
                  }
                }
                for (OtherThread &o : g_others)
                {
                  o.recent = o.recent_busy = 0;
                }
              }
            }
          }
        }
        if (QpcUs(last_fold, QpcNow()) > 500000.0)
        {
          last_fold = QpcNow();
          Fold(false);
        }
      }
      if (timer)
      {
        CloseHandle(timer);
      }
      g_exited.store(true, std::memory_order_release);
      return 0;
    }

    std::string IdName(uint64_t id)
    {
      const uint64_t v = id & 0x00FFFFFFFFFFFFFFull;
      switch (KindOf(id))
      {
      case kGuest: return fmt::format("guest:{:08X}", v);
      case kHook: return fmt::format("hook:{:08X}", v);
      case kExe: return fmt::format("exe:{:X}", v);
      case kRuntime: return fmt::format("rexruntime:{:X}", v);
      case kGpu: return fmt::format("gpu:{:X}", v);
      case kNtdll: return fmt::format("ntdll:{:X}", v);
      case kKernel: return fmt::format("kernel:{:X}", v);
      case kWait: return fmt::format("wait:{}", v < std::size(kWaitNames) ? kWaitNames[v] : "?");
      case kOtherModule:
      {
        const size_t m = size_t(v >> 32);
        return fmt::format("{}:{:X}", m < g_modules.size() ? g_modules[m].name : "?", v & 0xFFFFFFFFull);
      }
      default: return fmt::format("unknown:{:X}", v);
      }
    }

    const char *KindName(uint64_t k)
    {
      switch (k)
      {
      case kGuest: return "guest";
      case kHook: return "hook";
      case kExe: return "exe";
      case kRuntime: return "rexruntime";
      case kGpu: return "gpu";
      case kNtdll: return "ntdll";
      case kKernel: return "kernel";
      case kWait: return "wait";
      case kOtherModule: return "other_module";
      default: return "unknown";
      }
    }

    const char *WhereName(uint64_t w)
    {
      static const char *names[] = {"rest", "d3d_hook", "swap_hook", "gate", "ring_wait", "guest_fence",
                                    "block_idle", "not_busy", "adaptive", "fe_spin", "fe_pace", "yield"};
      return w < std::size(names) ? names[w] : "?";
    }
  } // namespace

  bool InProfilerUnwind() { return t_unwinding; }

  void OnSwap(PPCContext &ctx, uint8_t *base)
  {
    (void)ctx;
    (void)base;
    const int32_t period = REXCVAR_GET(pdz_guest_ip_profile);
    if (period <= 0)
    {
      return;
    }
    // The game thread's sections (game_thread_sampler.h) are kept while this runs: the Marks store g_where.
    pdz::gtsample::t_mark = true;
    pdz::gtsample::g_where.store(uint8_t(pdz::gtsample::Where::kRest), std::memory_order_relaxed);
    const int64_t now = QpcNow();
    if (!g_started.load(std::memory_order_relaxed))
    {
      g_started.store(true, std::memory_order_relaxed);
      QueryPerformanceFrequency(&g_qpc_freq);
      FILETIME created{}, ex{}, k{}, u{};
      GetProcessTimes(GetCurrentProcess(), &created, &ex, &k, &u);
      FILETIME now_ft{};
      GetSystemTimeAsFileTime(&now_ft);
      const uint64_t c = (uint64_t(created.dwHighDateTime) << 32) | created.dwLowDateTime;
      const uint64_t n = (uint64_t(now_ft.dwHighDateTime) << 32) | now_ft.dwLowDateTime;
      g_process_start_qpc = now - int64_t(double(n - c) / 1e7 * double(g_qpc_freq.QuadPart));
      DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_thread,
                      THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
      ULONG_PTR lo = 0, hi = 0;
      GetCurrentThreadStackLimits(&lo, &hi);
      g_stack_lo = lo;
      g_stack_hi = hi;
      BuildTables();
      g_stream.reserve(1u << 20);
      g_period_us.store(uint32_t(std::clamp(period, 100, 100000)), std::memory_order_relaxed);
      HANDLE t = g_thread ? CreateThread(nullptr, 256 * 1024, Main, nullptr, 0, nullptr) : nullptr;
      if (!t)
      {
        g_exited.store(true, std::memory_order_release);
        REXLOG_WARN("pdz_guest_ip_profile: the sampler could not start");
        return;
      }
      SetThreadPriority(t, THREAD_PRIORITY_TIME_CRITICAL); // wakes on time; a few microseconds per sample
      CloseHandle(t);
      std::atexit([] { WriteAtExit("exit"); });
      REXLOG_INFO("pdz_guest_ip_profile: on, every {} us; {} exe functions mapped ({} hooked originals), {} modules, "
                  "game thread stack {:X}-{:X}",
                  g_period_us.load(), g_exe.size(), g_hooked_n, g_modules.size(), g_stack_lo, g_stack_hi);
      g_prev_swap_qpc = now;
      return;
    }
    const uint64_t frame_us = uint64_t(std::max(0.0, QpcUs(g_prev_swap_qpc, now)));
    const uint64_t end_ms = uint64_t(std::max(0.0, QpcUs(g_process_start_qpc, now) / 1000.0));
    g_prev_swap_qpc = now;
    const uint32_t scene = uint32_t(pdz::pace::PeekScene());
    std::lock_guard<std::mutex> lock(g_mutex);
    g_stream.push_back(kFrameTag);
    g_stream.push_back(scene);
    g_stream.push_back(frame_us);
    g_stream.push_back(end_ms);
  }

  void WriteAtExit(const char *why)
  {
    if (!g_started.load(std::memory_order_relaxed) || g_written.exchange(true))
    {
      return;
    }
    g_stop.store(true, std::memory_order_release);
    for (int i = 0; i < 200 && !g_exited.load(std::memory_order_acquire); ++i)
    {
      Sleep(5);
    }
    if (!g_exited.load(std::memory_order_acquire))
    {
      return; // the sampler is stuck: never touch its data
    }
    Fold(false);
    const std::filesystem::path log = rex::CurrentLogFilePath();
    std::filesystem::path folder = !log.empty() && log.has_parent_path() ? log.parent_path() : std::filesystem::path(L"logs");
    std::string tag = log.stem().string();
    const size_t us = tag.rfind('_');
    tag = us == std::string::npos ? std::string("run") : tag.substr(us + 1);
    const std::filesystem::path path = folder / ("guest_ip_profile_" + tag + ".txt");
    FILE *f = _wfopen(path.c_str(), L"w");
    if (!f)
    {
      REXLOG_WARN("pdz_guest_ip_profile: could not write {}", path.string());
      return;
    }
    const Totals &t = g_totals;
    fmt::print(f, "# pdz_guest_ip_profile ({}) every {} us; window {}-{} s\n", why, g_period_us.load(),
               REXCVAR_GET(pdz_guest_ip_profile_from_s), REXCVAR_GET(pdz_guest_ip_profile_to_s));
    fmt::print(f, "frames {}\nframe_ms {:.3f}\nsamples {}\ndropped_frames {}\ndropped_samples {}\n", t.frames,
               t.frame_ms, t.samples, t.dropped_frames, t.dropped_samples);
    fmt::print(f, "sampler_cost_us_avg {:.2f}\nsampler_cost_us_max {:.2f}\nsampler_samples {}\nsampler_failed {}\n"
                  "unwind_frames_avg {:.1f}\nunwind_faults {}\nunwind_frame_register_stops {}\n",
               g_cost_n ? g_cost_us_sum / double(g_cost_n) : 0.0, g_cost_us_max, g_cost_n, g_failed,
               g_cost_n ? double(g_frames_sum) / double(g_cost_n) : 0.0, g_unwind_faults.load(),
               g_unwind_frame_stops.load());
    for (const auto &kv : t.where)
    {
      fmt::print(f, "where {} {}\n", WhereName(kv.first), kv.second);
    }
    for (const auto &kv : t.category)
    {
      fmt::print(f, "category {} {}\n", KindName(kv.first), kv.second);
    }
    for (const auto &kv : t.self)
    {
      fmt::print(f, "self {} {}\n", IdName(kv.first), kv.second);
    }
    for (const auto &kv : t.inner)
    {
      fmt::print(f, "inner {} {}\n", IdName(kv.first), kv.second);
    }
    for (const auto &kv : t.incl)
    {
      fmt::print(f, "incl {} {}\n", IdName(kv.first), kv.second);
    }
    for (const auto &kv : t.caller)
    {
      fmt::print(f, "caller {:08X} {:08X} {}\n", uint32_t(kv.first >> 32), uint32_t(kv.first), kv.second);
    }
    for (const auto &kv : t.host)
    {
      fmt::print(f, "host {} {:08X} {}\n", IdName(kv.first.first), kv.first.second, kv.second);
    }
    const auto print_stacks = [&](const char *who, const std::unordered_map<std::string, uint64_t> &stacks) {
      for (const auto &kv : stacks)
      {
        const uint64_t *ids = reinterpret_cast<const uint64_t *>(kv.first.data());
        const size_t n = kv.first.size() / sizeof(uint64_t);
        std::string line;
        for (size_t k = 0; k < n; ++k)
        {
          if (k)
          {
            line += ';';
          }
          line += IdName(ids[k]);
        }
        fmt::print(f, "stack {} {} {}\n", who, kv.second, line);
      }
    };
    print_stacks("swap", t.stacks);
    for (const std::string &line : t.spike_frames)
    {
      fmt::print(f, "{}\n", line);
    }
    for (size_t i = 0; i < g_others.size(); ++i)
    {
      const OtherThread &o = g_others[i];
      if (!o.samples)
      {
        continue;
      }
      fmt::print(f, "thread {} {} {} {} {}\n", i, o.id, o.samples, o.waits, o.name);
      for (const auto &kv : o.self)
      {
        fmt::print(f, "tself {} {} {}\n", i, IdName(kv.first), kv.second);
      }
      for (const auto &kv : o.inner)
      {
        fmt::print(f, "tinner {} {} {}\n", i, IdName(kv.first), kv.second);
      }
      for (const auto &kv : o.incl)
      {
        fmt::print(f, "tincl {} {} {}\n", i, IdName(kv.first), kv.second);
      }
      print_stacks(std::to_string(i).c_str(), o.stacks);
      for (const auto &kv : o.rip)
      {
        fmt::print(f, "trip {} {:X} {}\n", i, kv.first, kv.second);
      }
      for (const auto &kv : o.caller)
      {
        fmt::print(f, "tcaller {} {:08X} {:08X} {}\n", i, uint32_t(kv.first >> 32), uint32_t(kv.first), kv.second);
      }
    }
    std::fclose(f);
    REXLOG_INFO("pdz_guest_ip_profile: {} frames ({:.1f} ms each), {} samples written to {} ({}); sampler {:.1f} us "
                "per sample (max {:.1f})",
                t.frames, t.frames ? t.frame_ms / double(t.frames) : 0.0, t.samples, path.string(), why,
                g_cost_n ? g_cost_us_sum / double(g_cost_n) : 0.0, g_cost_us_max);
  }
} // namespace pdz::ipprof
