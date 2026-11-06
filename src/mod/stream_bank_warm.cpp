// pdz_stream_bank_warm (genuine fix, on; docs/MANSION_HITCH.md): read each XACT wave bank (.xwb) the game opens once,
// in the background, so its later streaming reads come from the Windows file cache.
//
// Why: Rare's XACT streams speech (the *_gen_s.xwb banks: guard barks, conversations) with overlapped reads from the
// game thread (XACT's read callback sub_82C27ED0 takes ERROR_IO_PENDING as normal: on the console the read returns at
// once and finishes in the background). The SDK runs every read synchronously (NtReadFile_entry), so the game thread
// waits for the disk. A read of a part of a bank that isn't cached took 32-36 ms for 6-10 KB on this PC (a cold NVMe
// read); the frame took 38-42 ms (logs mansion_hitch\before_reads1: both big hitches = the two slow reads; music
// streaming from the cached stock.xwb takes ~0.05 ms a read). In a fight new barks come often: log 393 had ~24
// hitches of 39-52 ms in 3.5 minutes.
//
// What it does: the guest's CreateFile (sub_82AD3B08) runs as before; when it returned a handle to a .xwb, that
// file's host path is queued to one background thread (lowest CPU priority; low I/O priority on its own handles, so
// the game's reads go first; normal memory priority, see below), which reads the file from start to end in 1 MB
// pieces with a sequential-scan handle of its own and throws the bytes away. The guest's reads, their results and
// their order are unchanged; only fewer of them wait for the disk. Banks over 2 GB are skipped. One
// 'pdz_stream_warm:' line per bank. Off = Rare's / the SDK's reads as before.
//
// Keeping them cached (2026-11-01, docs/BRIDGE_HITCH.md): until then the thread ran in THREAD_MODE_BACKGROUND, which
// also gives the pages it reads the lowest memory priority, the first ones Windows reuses when another program needs
// memory or writes a lot (a screen recorder): Bridge's fight music and barks were read from the disk again minutes
// after the warm-up. Now the pages keep normal priority, and every 20 s the banks opened in the last 30 minutes are
// read again (most recent first, up to 1.5 GB): a memory copy while they are cached, a low-priority disk read of
// whatever was dropped. A 'pdz_stream_warm: refresh' line when a refresh had to read from the disk.
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/filesystem/devices/host_path_entry.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xfile.h>

REXCVAR_DEFINE_BOOL(pdz_stream_bank_warm, true, "Audio",
                    "Read each XACT wave bank (.xwb) the game opens once in the background (lowest I/O priority), so "
                    "the speech that Rare's XACT streams during play comes from the Windows file cache instead of the "
                    "disk: the SDK runs XACT's asynchronous reads on the game thread, and an uncached read held a "
                    "frame for 30-50 ms (Mansion hitches in fights, docs/MANSION_HITCH.md). Same bytes, same reads; "
                    "off = as before")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REX_EXTERN(__imp__sub_82AD3B08); // the guest's CreateFile: returns the handle in r3 (-1 = failed)

namespace
{
  constexpr uint64_t kMaxBytes = 2ull << 30;
  constexpr auto kRewarmAfter = std::chrono::seconds(120);
  // Refresh (BRIDGE_HITCH.md): every kRefreshEvery the banks opened in the last kKeepFor are read again, most
  // recently opened first, up to kRefreshMaxBytes. Cached pages cost a memory copy and become the newest in the
  // cache; pages Windows dropped meanwhile (another program writing a lot, e.g. a screen recorder) come back from the
  // disk at low priority before the game asks for them.
  constexpr auto kRefreshEvery = std::chrono::seconds(20);
  constexpr auto kKeepFor = std::chrono::minutes(30);
  constexpr uint64_t kRefreshMaxBytes = 1536ull << 20;

  struct Job
  {
    std::filesystem::path host;
    std::string guest;
  };

  struct Bank
  {
    std::filesystem::path host;
    std::string guest;
    std::chrono::steady_clock::time_point opened{}, warmed{};
    uint64_t bytes = 0;
  };

  std::mutex g_mutex;
  std::condition_variable g_cv;
  std::deque<Job> g_queue;
  std::unordered_map<std::wstring, std::chrono::steady_clock::time_point> g_last; // host path -> last queued
  std::unordered_map<std::wstring, Bank> g_banks;                                 // host path -> warmed bank
  bool g_thread_started = false;

  // Reads the whole file once (the bytes are thrown away), with low I/O priority on this handle. The thread keeps
  // normal memory priority, so the cached pages are not the first ones Windows drops. THREAD_MODE_BACKGROUND_BEGIN,
  // used before 2026-11-01, also gave the pages the lowest memory priority: Bridge run before_probe read a warmed
  // bank from the disk 2.5 minutes later. Returns the bytes read, or -1 (could not open / too big).
  int64_t ReadWhole(const std::filesystem::path &host, std::vector<uint8_t> &buffer, double &ms, uint64_t &size_out)
  {
    const auto t0 = std::chrono::steady_clock::now();
    HANDLE h = CreateFileW(host.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE)
    {
      return -1;
    }
    FILE_IO_PRIORITY_HINT_INFO hint{};
    hint.PriorityHint = IoPriorityHintLow;
    SetFileInformationByHandle(h, FileIoPriorityHintInfo, &hint, sizeof(hint)); // best effort
    LARGE_INTEGER size{};
    GetFileSizeEx(h, &size);
    size_out = uint64_t(size.QuadPart);
    if (size_out > kMaxBytes)
    {
      CloseHandle(h);
      return -1;
    }
    uint64_t total = 0;
    DWORD got = 0;
    while (ReadFile(h, buffer.data(), DWORD(buffer.size()), &got, nullptr) && got)
    {
      total += got;
    }
    CloseHandle(h);
    ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return int64_t(total);
  }

  void WarmOne(const Job &job, std::vector<uint8_t> &buffer)
  {
    double ms = 0.0;
    uint64_t size = 0;
    const int64_t total = ReadWhole(job.host, buffer, ms, size);
    if (total < 0)
    {
      if (size > kMaxBytes)
      {
        REXLOG_INFO("pdz_stream_warm: {}: {:.1f} MB, too big: not warmed", job.guest, double(size) / 1048576.0);
      }
      else
      {
        REXLOG_INFO("pdz_stream_warm: {}: could not open the host file (error {})", job.guest, GetLastError());
      }
      return;
    }
    {
      std::lock_guard<std::mutex> lock(g_mutex);
      Bank &b = g_banks[job.host.native()];
      b.host = job.host;
      b.guest = job.guest;
      b.warmed = std::chrono::steady_clock::now();
      if (b.opened == std::chrono::steady_clock::time_point{})
      {
        b.opened = b.warmed;
      }
      b.bytes = uint64_t(total);
    }
    REXLOG_INFO("pdz_stream_warm: {}: {:.1f} MB read into the file cache in {:.0f} ms (background)", job.guest,
                double(total) / 1048576.0, ms);
  }

  void Refresh(std::vector<uint8_t> &buffer)
  {
    static std::chrono::steady_clock::time_point last_line{};
    std::vector<Bank> banks;
    const auto now = std::chrono::steady_clock::now();
    {
      std::lock_guard<std::mutex> lock(g_mutex);
      for (const auto &kv : g_banks)
      {
        if (now - kv.second.opened < kKeepFor)
        {
          banks.push_back(kv.second);
        }
      }
    }
    std::sort(banks.begin(), banks.end(), [](const Bank &a, const Bank &b) { return a.opened > b.opened; });
    uint64_t bytes = 0;
    double ms = 0.0, slowest = 0.0;
    std::string slowest_name;
    size_t n = 0;
    for (const Bank &b : banks)
    {
      if (bytes + b.bytes > kRefreshMaxBytes)
      {
        continue;
      }
      double one = 0.0;
      uint64_t size = 0;
      const int64_t got = ReadWhole(b.host, buffer, one, size);
      if (got < 0)
      {
        continue;
      }
      bytes += uint64_t(got);
      ms += one;
      ++n;
      if (one > slowest)
      {
        slowest = one;
        slowest_name = b.guest;
      }
      std::lock_guard<std::mutex> lock(g_mutex);
      g_banks[b.host.native()].warmed = std::chrono::steady_clock::now();
    }
    // A line when the refresh had to read from the disk (well above a memory copy), else one every 5 minutes.
    const bool from_disk = ms > double(bytes) / 1048576.0 * 2.0 + 50.0;
    if (n && (from_disk || now - last_line > std::chrono::minutes(5)))
    {
      last_line = now;
      REXLOG_INFO("pdz_stream_warm: refresh: {} bank(s), {:.1f} MB in {:.0f} ms{} (slowest {} {:.0f} ms)", n,
                  double(bytes) / 1048576.0, ms, from_disk ? ", some read from the disk again" : "", slowest_name,
                  slowest);
    }
  }

  void Worker()
  {
    SetThreadDescription(GetCurrentThread(), L"pdz stream bank warm");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST); // low CPU; normal memory priority (see ReadWhole)
    std::vector<uint8_t> buffer(1u << 20);
    auto next_refresh = std::chrono::steady_clock::now() + kRefreshEvery;
    for (;;)
    {
      Job job;
      bool have = false;
      {
        std::unique_lock<std::mutex> lock(g_mutex);
        g_cv.wait_until(lock, next_refresh, [] { return !g_queue.empty(); });
        if (!g_queue.empty())
        {
          job = std::move(g_queue.front());
          g_queue.pop_front();
          have = true;
        }
      }
      if (have)
      {
        WarmOne(job, buffer);
        continue;
      }
      if (REXCVAR_GET(pdz_stream_bank_warm))
      {
        Refresh(buffer);
      }
      next_refresh = std::chrono::steady_clock::now() + kRefreshEvery;
    }
  }

  bool EndsWithXwb(const std::string &s)
  {
    if (s.size() < 4)
    {
      return false;
    }
    std::string tail = s.substr(s.size() - 4);
    std::transform(tail.begin(), tail.end(), tail.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return tail == ".xwb";
  }

  void Queue(uint32_t handle)
  {
    auto file = REX_KERNEL_OBJECTS()->LookupObject<rex::system::XFile>(handle);
    if (!file || !EndsWithXwb(file->path()))
    {
      return;
    }
    const auto *entry = dynamic_cast<const rex::filesystem::HostPathEntry *>(file->entry());
    if (!entry)
    {
      return; // not a host file (a disc image / package): left alone
    }
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(g_mutex);
    {
      auto bank = g_banks.find(entry->host_path().native());
      if (bank != g_banks.end())
      {
        bank->second.opened = now; // kept in the refresh list while a level uses it
      }
    }
    auto it = g_last.find(entry->host_path().native());
    if (it != g_last.end() && now - it->second < kRewarmAfter)
    {
      return;
    }
    g_last[entry->host_path().native()] = now;
    g_queue.push_back(Job{entry->host_path(), file->path()});
    if (!g_thread_started)
    {
      g_thread_started = true;
      std::thread(Worker).detach();
    }
    g_cv.notify_one();
  }
} // namespace

// For pdz_file_read_log_ms (file_read_log.cpp): seconds since the bank at this host path was last read by the warm-up
// or a refresh; -1 = never.
double PdzStreamWarmAgeSeconds(const std::filesystem::path &host)
{
  std::lock_guard<std::mutex> lock(g_mutex);
  auto it = g_banks.find(host.native());
  if (it == g_banks.end() || it->second.warmed == std::chrono::steady_clock::time_point{})
  {
    return -1.0;
  }
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - it->second.warmed).count();
}

REX_HOOK_RAW(sub_82AD3B08)
{
  __imp__sub_82AD3B08(ctx, base);
  const uint32_t handle = ctx.r3.u32;
  if (handle && handle != 0xFFFFFFFFu && REXCVAR_GET(pdz_stream_bank_warm))
  {
    Queue(handle);
  }
}
