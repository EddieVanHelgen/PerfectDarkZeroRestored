// pdz_fix_log (tu3-base): how often each ported fix acts, so a play session
// shows the fixes fire in the TU3 build without the launch branch's larger
// AI / cover logs.
#include "fix_log.h"

#include <chrono>
#include <map>
#include <mutex>
#include <string>

#include <rex/logging.h>

#include "../game_cvars.h"

namespace
{
  struct Counter
  {
    uint64_t total = 0;
    uint64_t since = 0;
  };

  std::mutex g_mutex;
  std::map<std::string, Counter> g_counts;
  std::chrono::steady_clock::time_point g_last = std::chrono::steady_clock::now();
  constexpr auto kInterval = std::chrono::seconds(10);
} // namespace

namespace fix_log
{
  void Count(const char *what)
  {
    if (!REXCVAR_GET(pdz_fix_log))
    {
      return;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    Counter &counter = g_counts[what];
    ++counter.total;
    ++counter.since;
    const auto now = std::chrono::steady_clock::now();
    if (now - g_last < kInterval)
    {
      return;
    }
    g_last = now;
    std::string text;
    for (auto &[name, c] : g_counts)
    {
      if (!c.since)
      {
        continue;
      }
      text += text.empty() ? "" : ", ";
      text += name + " " + std::to_string(c.since) + " (total " + std::to_string(c.total) + ")";
      c.since = 0;
    }
    REXLOG_INFO("pdz_fix_log: {}", text);
  }
} // namespace fix_log
