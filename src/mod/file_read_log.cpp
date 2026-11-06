// pdz_file_read_log_ms (diagnostic, off; docs/MANSION_HITCH.md): times the guest's ReadFile wrapper sub_82AD43E0
// (handle r3, buffer r4, length r5, bytes read r6, OVERLAPPED r7: Offset +8, OffsetHigh +12) around the original and
// logs every read that took at least this many ms: the file, length, offset, whether the guest asked for an
// overlapped (asynchronous) read, and the time. The log prefix's thread id is the reading thread (the Swap / game
// thread is the one in the pdz_timing lines). One QPC pair per read while on; the file name is looked up only for a
// slow read. Mansion, 1 Nov: Rare's XACT streaming reads (asynchronous on the console) run synchronously in the SDK
// (NtReadFile_entry: `if (true || file->is_synchronous())`), on the game thread, 20-45 ms each.
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <string>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/filesystem/devices/host_path_entry.h>
#include <rex/system/xfile.h>

#include <filesystem>

double PdzStreamWarmAgeSeconds(const std::filesystem::path &host); // stream_bank_warm.cpp

REXCVAR_DEFINE_INT32(pdz_file_read_log_ms, 0, "Debug",
                     "Diagnostic (docs/MANSION_HITCH.md): log every guest ReadFile (sub_82AD43E0) that took at least "
                     "this many ms (file, length, offset, overlapped or not, ms); the log prefix's thread id is the "
                     "reader. A 'pdz_file_read: summary' line every 10 s with the reads, MB and the time spent "
                     "reading, per thread kind. 0 = off")
    .range(0, 100000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REX_EXTERN(__imp__sub_82AD43E0);

namespace
{
  uint32_t Load32(const uint8_t *base, uint32_t a)
  {
    const uint8_t *p = base + a;
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
  }

  double QpcMs(int64_t ticks)
  {
    static const double freq = [] {
      LARGE_INTEGER f;
      QueryPerformanceFrequency(&f);
      return double(f.QuadPart);
    }();
    return double(ticks) * 1000.0 / freq;
  }

  int64_t Qpc()
  {
    LARGE_INTEGER v;
    QueryPerformanceCounter(&v);
    return v.QuadPart;
  }

  // 10 s totals (all threads; relaxed counters).
  std::atomic<uint64_t> g_reads{0}, g_bytes{0}, g_ticks{0}, g_slow{0}, g_overlapped{0};
  std::atomic<int64_t> g_window_start{0};
} // namespace

REX_HOOK_RAW(sub_82AD43E0)
{
  const int32_t threshold = REXCVAR_GET(pdz_file_read_log_ms);
  if (threshold <= 0)
  {
    __imp__sub_82AD43E0(ctx, base);
    return;
  }
  const uint32_t handle = ctx.r3.u32, length = ctx.r5.u32, overlapped = ctx.r7.u32;
  uint64_t offset = ~0ull;
  if (overlapped && overlapped < 0xFFFF0000u)
  {
    offset = (uint64_t(Load32(base, overlapped + 12)) << 32) | Load32(base, overlapped + 8);
  }
  const int64_t t0 = Qpc();
  __imp__sub_82AD43E0(ctx, base);
  const int64_t t1 = Qpc();
  const double ms = QpcMs(t1 - t0);
  g_reads.fetch_add(1, std::memory_order_relaxed);
  g_bytes.fetch_add(length, std::memory_order_relaxed);
  g_ticks.fetch_add(uint64_t(t1 - t0), std::memory_order_relaxed);
  if (overlapped)
  {
    g_overlapped.fetch_add(1, std::memory_order_relaxed);
  }
  if (ms >= double(threshold))
  {
    g_slow.fetch_add(1, std::memory_order_relaxed);
    std::string name = "?";
    if (auto file = REX_KERNEL_OBJECTS()->LookupObject<rex::system::XFile>(handle))
    {
      name = file->path();
      // A bank the warm-up read: how long ago (BRIDGE_HITCH.md: warmed banks read from the disk again).
      if (const auto *entry = dynamic_cast<const rex::filesystem::HostPathEntry *>(file->entry()))
      {
        const double age = PdzStreamWarmAgeSeconds(entry->host_path());
        if (age >= 0.0)
        {
          name += " (warmed " + std::to_string(int(age)) + " s ago)";
        }
      }
    }
    REXLOG_INFO("pdz_file_read: {:.2f} ms {} len {} offset {} {} (handle {:08X}, result {:08X})", ms, name, length,
                offset == ~0ull ? std::string("current") : std::to_string(offset),
                overlapped ? "overlapped (async on the console)" : "synchronous", handle, ctx.r3.u32);
  }
  int64_t start = g_window_start.load(std::memory_order_relaxed);
  if (!start)
  {
    g_window_start.compare_exchange_strong(start, t1);
  }
  else if (QpcMs(t1 - start) >= 10000.0 && g_window_start.compare_exchange_strong(start, t1))
  {
    REXLOG_INFO("pdz_file_read: summary (10 s): {} reads ({} overlapped), {:.2f} MB, {:.1f} ms reading, {} slow "
                "(>= {} ms)",
                g_reads.exchange(0), g_overlapped.exchange(0), double(g_bytes.exchange(0)) / 1048576.0,
                QpcMs(int64_t(g_ticks.exchange(0))), g_slow.exchange(0), threshold);
  }
}
