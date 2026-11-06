// Game thread sampler (stage 2 M5 cpusplit; pdz_game_thread_profile, default 0 = off; game_thread_sampler.cpp).
//
// What the exact timers cannot split: the game thread's time outside every timed scope ("rest" in the pdz_timing game
// thread lines: the guest's own code, i.e. game logic, the D3D calls that are not hooked or that the game inlines,
// Rare's other waits, the mods' hooks). A sampler thread wakes every pdz_game_thread_profile microseconds (a
// high-resolution waitable timer) and reads, without stopping the game thread:
//   - g_where below: which hooked section the Swap thread is in (a hooked D3D entry, the Swap hook, the presentation
//     gate, a wait). Set by Mark objects in the hooks, on the Swap thread only (a thread_local test, then one relaxed
//     store at entry and one at exit; nothing else);
//   - outside every section ("rest"): the guest link register of the Swap thread's PPCContext (the return address of
//     the guest's last call: inside the function running now, or in the caller of a leaf) and the guest stack (r1's
//     back chain; each frame's saved link register at back chain - 8, kept only when the word before it is a bl /
//     bcctrl / bclrl). Both map to guest functions through the generated PPCFuncMappings table (sorted starts).
// Per sample: one count per section, or for rest the function of the link register ("self") and every distinct
// function on the guest stack ("inclusive"), plus whether that is in the D3D library (0x826DC500-0x82700F30, the
// closure of the hooked D3D entry points in the generated call graph). Racy reads by design (aligned 32/64-bit loads;
// stack reads bounded by the committed region that holds r1, code reads by the committed image).
//
// Output (needs pdz_frame_timing and pdz_frame_timing_phases >= 1; after the 'game thread split' line, every window
// and at SUMMARY):
//   pdz_timing: guest sampler (<window / SUMMARY>) swaps X-Y | samples N (n per frame, every U us) | where: rest a%
//   ~A ms, d3d hooks b% ~B ms, ... | rest (exact R ms) by code: game G ms (g%), D3D library self D ms (d%), D3D on
//   the guest stack E ms (e%) | self top: <guest function> ms (% of rest), ... | inclusive top: ...
// ms per frame = the share of the samples x the frame avg (where) or x the exact rest (rest by code).
//
// Cost on the game thread while off: one thread_local bool read per hooked D3D call. While on: plus two relaxed
// stores per hooked call; the sampler itself never stops or interrupts the game thread.
#pragma once
#include <atomic>
#include <cstdint>

#include <rex/cvar.h>

struct PPCContext;

REXCVAR_DECLARE(int32_t, pdz_game_thread_profile);
REXCVAR_DECLARE(bool, pdz_guest_profile);

namespace pdz::gtsample
{
  enum class Where : uint8_t
  {
    kRest = 0,   // outside every marked section: guest code (split by function)
    kD3dHook,    // a hooked D3D entry: our hook (shadow record / assert, native renderer) + the guest D3D call
    kSwapHook,   // the Swap hook: the guest Swap, our present, the per-frame bookkeeping
    kGate,       // the presentation gate sub_826DF7F0
    kRingWait,   // unused since 3 Nov 2026 (the sub_826E3968 hook was dead in TU3 and removed)
    kGuestFence, // the guest's fence block sub_826DE258
    kBlockIdle,  // BlockUntilIdle sub_826DE6A0
    kNotBusy,    // BlockUntilNotBusy sub_826DE880
    kAdaptive,   // the present step's adaptive extra wait (NtYieldExecution loop, lr 821418C4)
    kFeSpin,     // Rare's front-end spin (NtYieldExecution loop, lr 821418FC)
    kFePace,     // pdz_fix_frontend_pacing's wait (PdzFrontendPace)
    kYield,      // NtYieldExecution from any other call site
    kCount,
  };

  // The Swap thread's section (read by the sampler thread).
  inline std::atomic<uint8_t> g_where{0};
  // True on the Swap thread while the sampler is on (OnSwapHook, once per Swap).
  inline thread_local bool t_mark = false;

  // Marks a section for the sampler (Swap thread only); restores the enclosing section at exit.
  class Mark
  {
  public:
    explicit Mark(Where w)
    {
      if (t_mark)
      {
        on_ = true;
        prev_ = g_where.load(std::memory_order_relaxed);
        g_where.store(uint8_t(w), std::memory_order_relaxed);
      }
    }
    ~Mark()
    {
      if (on_)
      {
        g_where.store(prev_, std::memory_order_relaxed);
      }
    }
    void Retarget(Where w)
    {
      if (on_)
      {
        g_where.store(uint8_t(w), std::memory_order_relaxed);
      }
    }
    Mark(const Mark &) = delete;
    Mark &operator=(const Mark &) = delete;

  private:
    bool on_ = false;
    uint8_t prev_ = 0;
  };

  // A section that spans hook calls (the ring wait: from the first poll that says "wait" to the one that says "done").
  inline uint8_t Enter(Where w)
  {
    const uint8_t prev = g_where.load(std::memory_order_relaxed);
    if (t_mark)
    {
      g_where.store(uint8_t(w), std::memory_order_relaxed);
    }
    return prev;
  }
  inline void Leave(uint8_t prev)
  {
    if (t_mark)
    {
      g_where.store(prev, std::memory_order_relaxed);
    }
  }

  // The Swap hook (shadow_hooks.cpp), first thing, every Swap: the game thread's context and the guest memory base
  // for the sampler; starts the sampler thread when pdz_game_thread_profile is set.
  void OnSwapHook(PPCContext &ctx, uint8_t *base);
  // frame_timing.cpp: the first kept frame (after pdz_frame_timing_skip): the SUMMARY counts from here.
  void StartSession();
  // frame_timing.cpp LogPhases, after the game thread split line: the sampler line of the range (window: the samples
  // since the previous window line, then reset; SUMMARY: since StartSession). frame_ms / rest_ms: the range's avg.
  void LogRange(const char *kind, uint64_t first_swap, uint64_t last_swap, double frame_ms, double rest_ms,
                bool window);
  // pdz_guest_profile: the slow-frame profile's total so far, once (the app when the window is asked to close; the
  // sampler's atexit for a normal exit).
  void LogGuestProfileTotal(const char *why);
} // namespace pdz::gtsample
