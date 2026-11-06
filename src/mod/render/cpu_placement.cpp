// CPU placement (cpu_placement.h): layout detection, the choice and the soft CPU-set hints.
#include "cpu_placement.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_STRING(pdz_cpu_placement, "off", "Render",
                      "CPU placement (performance pass; default 'off' since 2026-10-30: log 279 had no blocked hitches "
                      "with it off, one run only): 'auto' = soft CPU-set hints (SetThreadSelectedCpuSets, no hard "
                      "affinity) chosen from the detected CPU layout: on a hybrid CPU (P-cores + E-cores) the game "
                      "thread and the native record thread prefer the performance cores and the background helpers "
                      "(audio decode, PSO / shader workers, log / sampler threads) the efficiency cores; on a CPU with "
                      "last-level caches of different sizes (AMD X3D with two CCDs) the game and record threads prefer "
                      "the large-cache cores; on every other CPU (all cores equal) nothing is changed. 'off' = no hints. "
                      "A 'pdz_cpu:' line at startup logs the layout and the choice. Read once (restart to change).");
REXCVAR_DEFINE_BOOL(pdz_background_priority, true, "Render",
                    "Background workers (texture upload, PSO warm-up, shader translation, shader file warm-up) run at "
                    "below-normal thread priority, so they never take a core from the game or record thread. Off = "
                    "normal priority as before. Read when each worker starts")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace pdz::cpu
{
  namespace
  {
    struct Plan
    {
      bool ready = false;
      std::vector<ULONG> critical; // CPU set ids for the game / record threads (empty = leave them)
      std::vector<ULONG> helpers;  // CPU set ids for the helpers (empty = leave them)
      std::string mode;            // what was chosen, for the log lines
    };

    std::string IdRanges(const std::vector<uint32_t> &v)
    {
      std::string out;
      for (size_t i = 0; i < v.size();)
      {
        size_t j = i;
        while (j + 1 < v.size() && v[j + 1] == v[j] + 1)
        {
          ++j;
        }
        out += fmt::format("{}{}", out.empty() ? "" : ",", i == j ? fmt::format("{}", v[i]) : fmt::format("{}-{}", v[i], v[j]));
        i = j + 1;
      }
      return out.empty() ? "none" : out;
    }

    Plan Detect()
    {
      Plan plan;
      plan.ready = true;
      std::string mode_cvar = REXCVAR_GET(pdz_cpu_placement);
      std::transform(mode_cvar.begin(), mode_cvar.end(), mode_cvar.begin(), [](char c) { return char(tolower(c)); });

      // CPU sets: one per logical processor, with its core, last-level cache and efficiency class.
      struct Lp
      {
        ULONG id;
        uint16_t group;
        uint8_t lp, core, llc, eff;
      };
      std::vector<Lp> lps;
      ULONG length = 0;
      GetSystemCpuSetInformation(nullptr, 0, &length, GetCurrentProcess(), 0);
      std::vector<uint8_t> buffer(length);
      if (length && GetSystemCpuSetInformation(reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(buffer.data()), length,
                                               &length, GetCurrentProcess(), 0))
      {
        for (ULONG offset = 0; offset + sizeof(DWORD) * 2 <= length;)
        {
          const auto *info = reinterpret_cast<const SYSTEM_CPU_SET_INFORMATION *>(buffer.data() + offset);
          if (!info->Size)
          {
            break;
          }
          if (info->Type == CpuSetInformation)
          {
            lps.push_back(Lp{info->CpuSet.Id, info->CpuSet.Group, info->CpuSet.LogicalProcessorIndex,
                             info->CpuSet.CoreIndex, info->CpuSet.LastLevelCacheIndex, info->CpuSet.EfficiencyClass});
          }
          offset += info->Size;
        }
      }
      if (lps.empty())
      {
        plan.mode = "no CPU set information: nothing changed";
        REXLOG_INFO("pdz_cpu: {}", plan.mode);
        return plan;
      }

      // L3 sizes per logical processor (group 0 masks; a processor outside them has an unknown size).
      std::map<std::pair<uint16_t, uint8_t>, uint64_t> l3_of;
      length = 0;
      GetLogicalProcessorInformationEx(RelationCache, nullptr, &length);
      std::vector<uint8_t> caches(length);
      if (length && GetLogicalProcessorInformationEx(
                        RelationCache, reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(caches.data()), &length))
      {
        for (DWORD offset = 0; offset < length;)
        {
          const auto *info = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *>(caches.data() + offset);
          if (!info->Size)
          {
            break;
          }
          if (info->Relationship == RelationCache && info->Cache.Level == 3)
          {
            const GROUP_AFFINITY &g = info->Cache.GroupMask;
            for (uint32_t bit = 0; bit < 64; ++bit)
            {
              if (g.Mask & (KAFFINITY(1) << bit))
              {
                l3_of[{g.Group, uint8_t(bit)}] = info->Cache.CacheSize;
              }
            }
          }
          offset += info->Size;
        }
      }

      // The layout.
      std::set<std::pair<uint16_t, uint8_t>> cores;
      std::map<uint8_t, std::vector<const Lp *>> by_class; // efficiency class -> processors
      std::map<uint8_t, std::vector<const Lp *>> by_llc;   // last-level cache index -> processors
      for (const Lp &p : lps)
      {
        cores.insert({p.group, p.core});
        by_class[p.eff].push_back(&p);
        by_llc[p.llc].push_back(&p);
      }
      const bool smt = cores.size() < lps.size();
      std::string classes, llcs;
      for (const auto &[eff, v] : by_class)
      {
        std::set<std::pair<uint16_t, uint8_t>> c;
        std::vector<uint32_t> ids;
        for (const Lp *p : v)
        {
          c.insert({p->group, p->core});
          ids.push_back(p->lp);
        }
        std::sort(ids.begin(), ids.end());
        classes += fmt::format("{}class {}: {} cores / {} logical (LP {})", classes.empty() ? "" : "; ", eff, c.size(),
                               v.size(), IdRanges(ids));
      }
      std::map<uint8_t, uint64_t> llc_size;
      for (const auto &[llc, v] : by_llc)
      {
        uint64_t size = 0;
        for (const Lp *p : v)
        {
          auto it = l3_of.find({p->group, p->lp});
          if (it != l3_of.end())
          {
            size = std::max(size, it->second);
          }
        }
        llc_size[llc] = size;
        llcs += fmt::format("{}{} logical, {} MB", llcs.empty() ? "" : "; ", v.size(), size >> 20);
      }
      const std::string layout =
          fmt::format("{} logical processors, {} cores, SMT {}, efficiency {}, last-level caches: {}", lps.size(),
                      cores.size(), smt ? "yes" : "no", classes, llcs);

      if (mode_cvar == "off")
      {
        plan.mode = "off (pdz_cpu_placement off): nothing changed";
      }
      else if (by_class.size() > 1)
      {
        // Hybrid: the highest efficiency class is the performance cores (Windows numbers them that way).
        const uint8_t top = by_class.rbegin()->first;
        for (const Lp &p : lps)
        {
          (p.eff == top ? plan.critical : plan.helpers).push_back(p.id);
        }
        if (plan.critical.size() < 2 || plan.helpers.empty())
        {
          plan.critical.clear();
          plan.helpers.clear();
          plan.mode = "hybrid but too few cores of a class: nothing changed";
        }
        else
        {
          plan.mode = fmt::format("hybrid: game + record thread prefer the {} class-{} logical processors, helpers the "
                                  "{} others (soft CPU-set hints)",
                                  plan.critical.size(), top, plan.helpers.size());
        }
      }
      else if (by_llc.size() > 1)
      {
        uint64_t largest = 0, smallest = ~uint64_t(0);
        uint8_t largest_llc = 0;
        for (const auto &[llc, size] : llc_size)
        {
          if (size > largest)
          {
            largest = size;
            largest_llc = llc;
          }
          smallest = std::min(smallest, size);
        }
        if (smallest && largest >= smallest + smallest / 2) // at least 1.5x: an X3D cache die
        {
          for (const Lp &p : lps)
          {
            if (p.llc == largest_llc)
            {
              plan.critical.push_back(p.id);
            }
          }
          plan.mode = fmt::format("large-cache CCD: game + record thread prefer the {} logical processors of the {} MB L3 "
                                  "(soft CPU-set hints); helpers unchanged",
                                  plan.critical.size(), largest >> 20);
        }
        else
        {
          plan.mode = "equal last-level caches: nothing changed";
        }
      }
      else
      {
        plan.mode = "all cores equal: nothing changed";
      }
      REXLOG_INFO("pdz_cpu: {} | pdz_cpu_placement {}: {}", layout, mode_cvar, plan.mode);
      return plan;
    }

    Plan &GetPlan()
    {
      static Plan plan = Detect();
      return plan;
    }

    bool Apply(HANDLE thread, const std::vector<ULONG> &ids)
    {
      return !ids.empty() && SetThreadSelectedCpuSets(thread, ids.data(), ULONG(ids.size()));
    }

    // SDK helper threads found by name once the game runs (they start before any of our code sees a frame).
    constexpr const wchar_t *kSdkHelpers[] = {L"Audio Worker", L"XMA Decoder"};

    void PlaceSdkHelpers(const Plan &plan)
    {
      if (plan.helpers.empty())
      {
        return;
      }
      HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
      if (snap == INVALID_HANDLE_VALUE)
      {
        return;
      }
      const DWORD pid = GetCurrentProcessId();
      std::string placed;
      THREADENTRY32 te{};
      te.dwSize = sizeof(te);
      for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
      {
        if (te.th32OwnerProcessID != pid)
        {
          continue;
        }
        HANDLE t = OpenThread(THREAD_QUERY_LIMITED_INFORMATION | THREAD_SET_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
        if (!t)
        {
          continue;
        }
        PWSTR name = nullptr;
        if (SUCCEEDED(GetThreadDescription(t, &name)) && name)
        {
          for (const wchar_t *helper : kSdkHelpers)
          {
            // The SDK's host threads are named "<name> (<handle>)" (XThread::set_name).
            if (wcsncmp(name, helper, wcslen(helper)) == 0 && Apply(t, plan.helpers))
            {
              std::wstring w(name);
              placed += fmt::format("{}{}", placed.empty() ? "" : ", ", std::string(w.begin(), w.end()));
            }
          }
          LocalFree(name);
        }
        CloseHandle(t);
      }
      CloseHandle(snap);
      REXLOG_INFO("pdz_cpu: SDK helpers on the helper cores: {}", placed.empty() ? "none found" : placed);
    }
  } // namespace

  void OnSwapThread()
  {
    thread_local bool placed = false;
    if (placed)
    {
      return;
    }
    placed = true;
    const Plan &plan = GetPlan();
    if (Apply(GetCurrentThread(), plan.critical))
    {
      REXLOG_INFO("pdz_cpu: a Swap thread ({}) prefers {} logical processors", GetCurrentThreadId(),
                  plan.critical.size());
    }
    static std::once_flag sdk_once;
    std::call_once(sdk_once, [&] { PlaceSdkHelpers(plan); });
  }

  void PlaceCriticalThread(const char *what)
  {
    const Plan &plan = GetPlan();
    if (Apply(GetCurrentThread(), plan.critical))
    {
      REXLOG_INFO("pdz_cpu: {} prefers {} logical processors", what, plan.critical.size());
    }
  }

  void PlaceHelperThread(const char *what)
  {
    (void)what;
    Apply(GetCurrentThread(), GetPlan().helpers);
  }

  void LowerBackgroundThread(const char *what)
  {
    PlaceHelperThread(what);
    if (REXCVAR_GET(pdz_background_priority))
    {
      SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    }
  }
} // namespace pdz::cpu
