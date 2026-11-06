// pdz_guest_ip_profile (31 Oct 2026, docs/RARE_CODE_PROFILE.md): an instruction-pointer sampler of the game thread.
//
// A sampler thread suspends the game (Swap) thread every pdz_guest_ip_profile microseconds (0 = off, the default;
// 1000 = 1 kHz), reads its registers, unwinds its host stack (RtlLookupFunctionEntry / RtlVirtualUnwind, up to 48
// frames, bounded by the thread's stack) and resumes it. Every frame is mapped to the recompiled guest function it is
// in (the generated PPCFuncMappings table plus the hooked functions' original bodies, guest_ip_profile_hooks.inc;
// a function's cold chunks are followed to its primary entry), or to the module and function start it is in (the
// mod's / SDK's code in the exe, rexruntime.dll, the GPU plugin, ntdll / kernelbase with the wait exports named).
// The game thread's section (game_thread_sampler.h g_where: D3D hook, Swap hook, gate, guest fence, ...) is kept per
// sample. Only frames whose Swap closes a level frame (pdz::pace PeekScene) and that end inside
// [pdz_guest_ip_profile_from_s, pdz_guest_ip_profile_to_s] (seconds after the process started; 0 = no bound) count.
//
// Output, when the window is asked to close (and at exit): logs\guest_ip_profile_<log number>.txt next to the run's
// log: the frames, their summed time, the samples and the sampler's own cost, then per-sample counts by section,
// category, self (top frame), innermost guest function, inclusive guest function, the innermost guest function's
// caller, and the non-guest top frames with the guest function that called into them. tools/perf/ip_profile_report.py
// turns one or more of these into tables.
//
// Off: one cvar read per Swap. On: the game thread is stopped for the sample (a few microseconds each, measured and
// written to the file), one mutex and a few stores per Swap; nothing else runs on the game thread.
#pragma once
#include <cstdint>

struct PPCContext;

namespace pdz::ipprof
{
  // shadow_hooks.cpp Swap hook, right after gtsample::OnSwapHook (the game thread, every Swap).
  void OnSwap(PPCContext &ctx, uint8_t *base);
  // The app's OnWindowCloseRequested (and an atexit): stop the sampler and write the file, once.
  void WriteAtExit(const char *why);
  // crash_dump.cpp: true on the sampler thread while it unwinds a stopped thread (a fault there is caught by the
  // sampler's own handler and must not be reported as a crash).
  bool InProfilerUnwind();
} // namespace pdz::ipprof
