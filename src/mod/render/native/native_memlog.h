// Native renderer (stage 1): periodic memory log for soak tests (native_memlog.cpp).
//
// --pdz_native_memlog_s=<n> (default 0 = off): every n seconds of wall-clock time, at the guest Swap, one line
//   pdz_memlog: t <s> s swap <n> frame <f> | proc private_mb=.. ws_mb=.. | gpu local_mb=.. local_budget_mb=.. | ...
// with the process totals (GetProcessMemoryInfo), the adapter's video memory (IDXGIAdapter3::QueryVideoMemoryInfo,
// local and non-local, the whole process: the GPU plugin's resources included) and what the native renderer holds:
// upload buffers, descriptor heaps, deferred releases, texture cache, surfaces, geometry cache, PSOs, shader cache and
// the diagnostic buffers. At test exit (pdz::test::LogSummary) a final sample and SUMMARY lines: first / peak / last
// of every value, then the values that still grew in the second half of the run (mid -> last), which is what a leak
// looks like once the loads are over. Works with the native renderer off too (process and GPU totals only; the
// device is never created by it). stage1/subagents/memlog.md has the keys and the soak run.
//
// Cost: one cvar load per Swap while off. On: one GetTickCount64 per Swap; a sample walks the caches (thousands of
// entries) and asks the device for the allocation size of each texture / surface it has not sized yet, once.
#pragma once

namespace pdz::native::memlog
{
  // native OnSwap, first thing after the swap counters, render lock held, whatever pdz_native_renderer says.
  void OnSwap();
  // Test exit (pdz::test::LogSummary): a last sample and the SUMMARY lines. Nothing while the log is off.
  void LogSummary(const char *why);
} // namespace pdz::native::memlog
