// Hook profiler: see hook_profile.h.
#include "hook_profile.h"

#include <algorithm>
#include <mutex>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <rex/logging.h>

#include "../game_cvars.h"

namespace hook_profile
{
  namespace
  {
    std::mutex g_mutex;
    std::vector<Slot *> &Slots()
    {
      static std::vector<Slot *> slots;
      return slots;
    }
    std::atomic<int64_t> g_next{0}; // steady_clock ticks of the next report
  } // namespace

  bool On()
  {
    return REXCVAR_GET(pdz_hook_profile);
  }

  Slot *Register(const char *name)
  {
    std::lock_guard lock(g_mutex);
    Slot *slot = new Slot();
    slot->name = name;
    Slots().push_back(slot);
    return slot;
  }

  void Tick()
  {
    const int64_t now = std::chrono::steady_clock::now().time_since_epoch().count();
    int64_t next = g_next.load(std::memory_order_relaxed);
    if (now < next)
    {
      return;
    }
    const int64_t period = std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::seconds(5)).count();
    if (!g_next.compare_exchange_strong(next, now + period))
    {
      return;
    }
    if (next == 0)
    {
      return; // first call: start the window
    }
    struct Row
    {
      const char *name;
      uint64_t calls, ns;
    };
    std::vector<Row> rows;
    uint64_t total_ns = 0;
    {
      std::lock_guard lock(g_mutex);
      for (Slot *s : Slots())
      {
        const uint64_t calls = s->calls.exchange(0, std::memory_order_relaxed);
        const uint64_t ns = s->ns.exchange(0, std::memory_order_relaxed);
        if (calls)
        {
          rows.push_back({s->name, calls, ns});
          total_ns += ns;
        }
      }
    }
    std::sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) { return a.ns > b.ns; });
    std::string top;
    for (size_t i = 0; i < rows.size() && i < 12; ++i)
    {
      top += fmt::format(" | {} {:.2f} ms/s {:.0f}/s", rows[i].name, double(rows[i].ns) / 5e6, double(rows[i].calls) / 5.0);
    }
    REXLOG_INFO("pdz_hook_profile: hooks (inclusive, incl. Rare's code they call) {:.2f} ms/s in {} hooks; top:{}",
                double(total_ns) / 5e6, rows.size(), top);
  }
} // namespace hook_profile
