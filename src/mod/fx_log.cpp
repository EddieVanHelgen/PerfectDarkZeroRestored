// De-rez effect log (pdz_fx_log, diagnostic, off by default; needs log_level
// "info"). For the missing death effect (docs/BUG_AUDIT.md VR8): a defeated
// multiplayer / holo character is replaced by a cloud of cubes in its colours.
// Read-only; it changes nothing in the game.
//
// How the game does it ("DeRes"): sub_82510640(prop, mode) requests it (mode 3
// = de-rez on death, 1 = rez-in on spawn); sub_825106D8 aims a camera at the
// body, takes one of 64 slots (sub_825E69A0; 6384 bytes at 82DA43C0) and
// plays XACT_SOUNDBANK_STOCK_REZOUT. The next frame draws the body alone into
// a 256 x 256 target and resolves it into the texture at [82DA3958]
// (sub_825E6AD8, D3D resolve at 825E6B20), with a GPU callback
// (sub_825E6BF8) that wakes the DeResThread. Its work (sub_825E6CD0) reads
// the resolved pixels on the CPU from the texture's memory ([tex+20]) and
// spawns a particle per non-black pixel it finds (slot +6352 count, +6360
// state: 2 = particles, 3 = none / done). sub_825E70C8 draws them, slot
// +6348 counts their 1 s down (sub_825E8370).
//
// The log shows each step; the decisive line is the thread's: how many
// non-black pixels were in the resolved texture's memory when the CPU read
// it, and how many particles that gave.
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>

#include <rex/hook.h>
#include <rex/logging.h>

#include "../game_cvars.h"
#include "game_fixes.h"

REX_EXTERN(__imp__sub_82510640);
REX_EXTERN(__imp__sub_825E69A0);
REX_EXTERN(__imp__sub_825E6AD8);
REX_EXTERN(__imp__sub_825E6BF8);
REX_EXTERN(__imp__sub_825E6CD0);
REX_EXTERN(__imp__sub_825E8370);

namespace
{
  constexpr uint32_t kSlots = 0x82DA43C0;
  constexpr uint32_t kSlotSize = 6384;
  constexpr uint32_t kSlotCount = 64;
  constexpr uint32_t kSlotParticles = 6352;
  constexpr uint32_t kSlotState = 6360;
  constexpr uint32_t kSlotLife = 6348;
  constexpr uint32_t kSnapshotTexture = 0x82DA3958; // -> texture (+20 base address)
  constexpr uint32_t kSnapshotSize = 256;
  constexpr uint32_t kSlotGrantReturn = 0x825109BC; // sub_825E69A0 from sub_825106D8

  uint32_t LoadBe32(const uint8_t *base, uint32_t address)
  {
    const uint8_t *p = base + address;
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
  }

  float LoadBeFloat(const uint8_t *base, uint32_t address)
  {
    const uint32_t bits = LoadBe32(base, address);
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
  }

  bool IsGuestPointer(uint32_t address)
  {
    return address >= 0x00010000u && address < 0xFFFF0000u;
  }

  bool Log()
  {
    return REXCVAR_GET(pdz_fx_log);
  }

  using Clock = std::chrono::steady_clock;
  const Clock::time_point g_start = Clock::now();
  double Ms()
  {
    return std::chrono::duration<double, std::milli>(Clock::now() - g_start).count();
  }

  const char *ModeName(uint32_t mode)
  {
    return mode == 3 ? "de-rez (death)" : mode == 1 ? "rez-in (spawn)" : "other";
  }

  // Non-black pixels (RGB != 0, as 825E700C tests) in the snapshot texture's
  // memory; the layout (tiling) doesn't matter for a count.
  uint32_t SnapshotPixels(const uint8_t *base, uint32_t *texture_base)
  {
    const uint32_t texture = LoadBe32(base, kSnapshotTexture);
    const uint32_t address = IsGuestPointer(texture) ? LoadBe32(base, texture + 20) & ~0xFFFu : 0;
    *texture_base = address;
    if (!IsGuestPointer(address))
    {
      return 0;
    }
    uint32_t count = 0;
    for (uint32_t i = 0; i < kSnapshotSize * kSnapshotSize; ++i)
    {
      count += (LoadBe32(base, address + i * 4) & 0x00FFFFFFu) != 0;
    }
    return count;
  }

  struct SlotView
  {
    uint32_t state[kSlotCount];
    uint32_t particles[kSlotCount];
  };

  void ReadSlots(const uint8_t *base, SlotView &view)
  {
    for (uint32_t i = 0; i < kSlotCount; ++i)
    {
      const uint32_t slot = kSlots + i * kSlotSize;
      view.state[i] = LoadBe32(base, slot + kSlotState);
      view.particles[i] = LoadBe32(base, slot + kSlotParticles);
    }
  }
} // namespace

// sub_82510640(prop, mode): requests a de-rez / rez-in.
REX_HOOK_RAW(sub_82510640)
{
  if (Log())
  {
    const uint32_t prop = ctx.r3.u32;
    const uint32_t mode = ctx.r4.u32;
    const bool ok = IsGuestPointer(prop);
    REXLOG_INFO("pdz_fx: t {:.1f} ms: request {} ({}) for {:08X} from {:08X} at ({:.0f}, {:.0f}, {:.0f}) radius {:.0f}",
                Ms(), mode, ModeName(mode), prop, uint32_t(ctx.lr), ok ? LoadBeFloat(base, prop + 48) : 0.0f,
                ok ? LoadBeFloat(base, prop + 52) : 0.0f, ok ? LoadBeFloat(base, prop + 56) : 0.0f,
                ok ? LoadBeFloat(base, prop + 60) : 0.0f);
  }
  __imp__sub_82510640(ctx, base);
}

// sub_825E69A0(...): a free effect slot (or -1). From sub_825106D8 it is
// followed by the REZOUT sound.
REX_HOOK_RAW(sub_825E69A0)
{
  const bool grant = Log() && uint32_t(ctx.lr) == kSlotGrantReturn;
  __imp__sub_825E69A0(ctx, base);
  if (grant)
  {
    const int32_t slot = int32_t(ctx.r3.u32);
    REXLOG_INFO("pdz_fx: t {:.1f} ms: {}", Ms(),
                slot < 0 ? std::string("no free slot (effect skipped, no sound)")
                         : "slot " + std::to_string(slot) + " granted (REZOUT sound plays)");
  }
}

// sub_825E6AD8(): resolves the body snapshot into the texture and queues the
// GPU callback.
REX_HOOK_RAW(sub_825E6AD8)
{
  game_fixes::BeforeDeRezSnapshot(base);
  if (Log())
  {
    const uint32_t texture = LoadBe32(base, kSnapshotTexture);
    REXLOG_INFO("pdz_fx: t {:.1f} ms: snapshot resolve into texture {:08X} (memory {:08X})", Ms(), texture,
                IsGuestPointer(texture) ? LoadBe32(base, texture + 20) & ~0xFFFu : 0);
  }
  __imp__sub_825E6AD8(ctx, base);
}

// sub_825E6BF8(): the GPU callback (it reached the resolve).
REX_HOOK_RAW(sub_825E6BF8)
{
  if (Log())
  {
    REXLOG_INFO("pdz_fx: t {:.1f} ms: GPU callback (snapshot resolve done on the GPU), waking DeResThread", Ms());
  }
  __imp__sub_825E6BF8(ctx, base);
}

// sub_825E6CD0(): DeResThread's work: reads the snapshot on the CPU and
// spawns the particles.
REX_HOOK_RAW(sub_825E6CD0)
{
  if (!Log())
  {
    __imp__sub_825E6CD0(ctx, base);
    return;
  }
  uint32_t texture_base = 0;
  const uint32_t pixels = SnapshotPixels(base, &texture_base);
  SlotView before;
  ReadSlots(base, before);
  __imp__sub_825E6CD0(ctx, base);
  SlotView after;
  ReadSlots(base, after);
  REXLOG_INFO("pdz_fx: t {:.1f} ms: DeResThread reads the snapshot at {:08X}: {} of {} pixels non-black in "
              "guest memory",
              Ms(), texture_base, pixels, kSnapshotSize * kSnapshotSize);
  for (uint32_t i = 0; i < kSlotCount; ++i)
  {
    if (after.state[i] != before.state[i] || after.particles[i] != before.particles[i])
    {
      REXLOG_INFO("pdz_fx:   slot {}: state {} -> {}, {} particle(s){}", i, before.state[i], after.state[i],
                  after.particles[i],
                  after.state[i] == 3 ? " (no non-black pixel found: effect ends, nothing drawn)" : "");
    }
  }
}

// sub_825E8370(...): counts the particles' life down; logs effects ending.
REX_HOOK_RAW(sub_825E8370)
{
  if (!Log())
  {
    __imp__sub_825E8370(ctx, base);
    return;
  }
  SlotView before;
  ReadSlots(base, before);
  __imp__sub_825E8370(ctx, base);
  for (uint32_t i = 0; i < kSlotCount; ++i)
  {
    const uint32_t state = LoadBe32(base, kSlots + i * kSlotSize + kSlotState);
    if (state != before.state[i])
    {
      REXLOG_INFO("pdz_fx: t {:.1f} ms: slot {}: state {} -> {} ({} particle(s), life {:.2f})", Ms(), i,
                  before.state[i], state, before.particles[i],
                  LoadBeFloat(base, kSlots + i * kSlotSize + kSlotLife));
    }
  }
}
