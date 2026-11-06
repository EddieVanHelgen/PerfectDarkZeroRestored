// Hook profiler (pdz_hook_profile, Debug, off by default; added 2026-10-24
// for the Temple frame drops). Included after <rex/hook.h> in a mod file, it
// redefines REX_HOOK_RAW so each hook counts its calls and its host time
// (inclusive: our code plus Rare's original it calls) and, every 5 s, the
// file's busiest hooks are logged. With the switch off a hook costs one
// relaxed atomic load more than before.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

#include <rex/hook.h>
#include <rex/ppc/func.h>

namespace hook_profile
{
  struct Slot
  {
    const char *name = nullptr;
    std::atomic<uint64_t> calls{0};
    std::atomic<uint64_t> ns{0};
  };

  bool On();
  Slot *Register(const char *name);
  void Tick(); // logs the top hooks every 5 s (any thread)

  struct Probe
  {
    Slot *slot;
    std::chrono::steady_clock::time_point start;
    explicit Probe(Slot *s) : slot(On() ? s : nullptr)
    {
      if (slot)
      {
        start = std::chrono::steady_clock::now();
      }
    }
    ~Probe()
    {
      if (slot)
      {
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start);
        slot->calls.fetch_add(1, std::memory_order_relaxed);
        slot->ns.fetch_add(uint64_t(ns.count()), std::memory_order_relaxed);
        Tick();
      }
    }
  };
} // namespace hook_profile

#undef REX_HOOK_RAW
#define REX_HOOK_RAW(name)                                                                  \
  static void name##_profiled(PPCContext &__restrict ctx, uint8_t *base);                  \
  extern "C" REX_FUNC(name)                                                                 \
  {                                                                                         \
    static hook_profile::Slot *slot = hook_profile::Register(#name);                      \
    hook_profile::Probe probe(slot);                                                        \
    name##_profiled(ctx, base);                                                             \
  }                                                                                         \
  static void name##_profiled([[maybe_unused]] PPCContext &__restrict ctx, [[maybe_unused]] uint8_t *base)
