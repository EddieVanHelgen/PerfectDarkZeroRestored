// CPU placement (performance pass, pdz_cpu_placement auto / off): soft CPU-set hints for the game's frame-critical
// threads on CPUs whose cores are not all equal, chosen from the detected layout (never a fixed core list).
//
//   Intel hybrid (more than one efficiency class): the game (Swap) thread and the native record thread prefer the
//     highest efficiency class (P-cores), the background helpers (audio worker / XMA decoder, PSO and shader
//     workers, log and sampler threads) the lower classes (E-cores).
//   Two or more last-level caches of different sizes (AMD X3D parts with two CCDs): the game and record threads prefer
//     the cores of the largest L3; helpers are left alone.
//   Anything else (all cores equal, one CCD, the Steam Deck, two equal CCDs): nothing is changed.
//
// Hints use SetThreadSelectedCpuSets (Windows' "soft" affinity, compatible with its power management and Thread
// Director; no SetThreadAffinityMask). One 'pdz_cpu:' line at the first use logs the layout and the choice.
#pragma once

namespace pdz::cpu
{
  // The game's Swap thread (call on it each Swap; acts once per thread).
  void OnSwapThread();
  // The native record thread (call once at its start).
  void PlaceCriticalThread(const char *what);
  // One of our background helper threads (call once at its start).
  void PlaceHelperThread(const char *what);
  // A background worker that must never take CPU from the game or record thread (texture uploads, PSO / shader
  // workers, the shader file warm-up; overnight 30-31 Oct): PlaceHelperThread, and with pdz_background_priority (on)
  // THREAD_PRIORITY_BELOW_NORMAL. Call once at the thread's start.
  void LowerBackgroundThread(const char *what);
} // namespace pdz::cpu
