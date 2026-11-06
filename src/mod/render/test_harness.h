// Unattended test harness (test_harness.cpp): scripted controller input and a clean timed exit.
#pragma once
#include <cstdint>

#include "latency_log.h"

namespace pdz::test
{
  bool InputScriptActive();
  // Called from the Swap hook every frame (cheap when --pdz_test_exit_s is 0).
  void OnSwap();
  uint32_t ScriptedPresses();
} // namespace pdz::test

namespace pdz::native
{
  // One summary line (frames complete / missed, shown native, draws and skips, auto-diff average); native_present.cpp.
  void LogTestSummary(const char *why);
} // namespace pdz::native

namespace pdz::timing
{
  // The pdz_timing SUMMARY line (frame_timing.cpp); prints nothing while pdz_frame_timing is off.
  void LogSummary(const char *why);
} // namespace pdz::timing

namespace pdz::native::memlog
{
  // The pdz_memlog SUMMARY lines (native/native_memlog.cpp); print nothing while pdz_native_memlog_s is 0.
  void LogSummary(const char *why);
} // namespace pdz::native::memlog

namespace pdz::census
{
  // The pdz_census SUMMARY line and its detail lines (gpu_census.cpp); prints nothing while pdz_gpu_census is off.
  void LogSummary(const char *why);
} // namespace pdz::census

namespace pdz::gpusync
{
  // The pdz_sync SUMMARY line (gpu_sync.cpp); prints nothing while pdz_sync_log is off and the GPU backend is not
  // null. Also stops the sync heartbeat.
  void LogSummary(const char *why);
} // namespace pdz::gpusync

namespace pdz::pm4skip
{
  // The pdz_pm4 SUMMARY line (pm4_skip.cpp); prints nothing while pdz_native_skip_pm4 was never on.
  void LogSummary(const char *why);
} // namespace pdz::pm4skip

namespace pdz::pace
{
  // The pdz_clock SUMMARY line (game_pace.cpp); prints nothing while the game clock log is off.
  void LogSummary(const char *why);
} // namespace pdz::pace

namespace pdz::test
{
  // The pdz_test_mp SUMMARY line (test_mp_setup.cpp): a last snapshot while a match runs, then the totals and the
  // hash chain; prints nothing while pdz_test_mp_snapshot_s is 0.
  void LogMpSnapshotSummary(const char *why);
} // namespace pdz::test

namespace pdz::native::recthread
{
  // pdz_native_record_thread: waits until the worker has done everything queued (native/native_recthread.h).
  void Drain(const char *why);
} // namespace pdz::native::recthread

namespace pdz::test
{
  // Printed whether or not the native renderer is on (LogTestSummary has no early return); timing line, then the
  // memory log's lines, the sync log's, then the GPU census last (its detail lines are long).
  inline void LogSummary(const char *why)
  {
    pdz::native::recthread::Drain(why); // the record thread's queued work first (no-op when it is off)
    pdz::native::LogTestSummary(why);
    pdz::timing::LogSummary(why);
    pdz::pace::LogSummary(why);
    pdz::native::memlog::LogSummary(why);
    pdz::gpusync::LogSummary(why);
    pdz::pm4skip::LogSummary(why);
    pdz::census::LogSummary(why);
    pdz::test::LogMpSnapshotSummary(why);
    pdz::latency::LogSummary(why);
  }
} // namespace pdz::test
