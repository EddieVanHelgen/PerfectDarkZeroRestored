// Fixes for bugs in the original game, Title Update 3 build (tu3-base). See
// README.md next to this file and docs/BUG_AUDIT.md for the evidence;
// docs/TU3_BASE.md (re-targeting checklist) for what is ported. Only the
// fixes the launch build keeps are here, each checked to be still needed in
// TU3's code; Rare's own title update fixes are native in this build.
// Addresses are TU3's; launch addresses in brackets.
//
// Each fix is its own option and only applies while the master switch
// pdz_game_fixes is on (bind_game_fixes_ab flips it); with either off the
// game runs its original code.
//
// - pdz_fix_halo_zoom_curve: the Standard ("Halo") look model
//   (sub_825B0AA0 [825A1280]) blends its records by horizontal sensitivity.
//   Below the default (s < 0.5, 825B0C90..0D28 [825A1470..1508]) every field
//   blends min -> default, except field 3 (the curve exponent used while
//   zoomed or in cover), which is loaded from the max record (825B0CA4
//   [825A1484] lfs f9,0xc(r9), r9 = [82DA4600] [82DA3764]). The fix gives the
//   max record the default's field 3 for the player's update while s < 0.5,
//   so that blend runs min -> default like the others.
// - pdz_fix_spread_cap_order: the weapon aim (sub_82588078 [825787F0])
//   passes the gun's current spread (+0x4D8) to the shot direction
//   (sub_82240A90 [822397C8], called at 825880E4 [8257885C]) and only then
//   caps it by stance (gun record byte +0xBD / 300 x 0.5 zoomed and
//   crouched, 0.75 zoomed, 1.0 crouched, 1.5 standing). The fix applies the
//   same cap to the spread it passes.
// - pdz_fix_cmp_noise: each shot makes noise that enemies hear
//   (sub_8258F410 [8257FB90]): the shooter's noise slot becomes
//   clamp(old + record +0xF0, +0xF4, +0xF8). Every firearm has +0xF4 == +0xF0
//   except the CMP (gun type 0x11): 500 per shot but 5000 minimum, so its
//   first shot is heard 10x further. TU3 kept the record (it changed only the
//   mine's). The fix uses the per-shot value as the minimum for the player's
//   CMP during that update.
// - pdz_fix_autoaim_zoom_bend: the shot direction divides its screen point by
//   the zoom factor (vtable +660 [+656] = 1 - player +0x1A9C), so it takes
//   points in unzoomed units, but auto-aim projects the bent point after
//   restoring the zoomed FOV, so a bent zoomed shot lands 1 / (1 - zoom) times
//   as far from the crosshair as the target's aim point. The fix scales the
//   bend (player +0x1F58 minus the reticle, auto-aim +0x30) by (1 - zoom) for
//   the weapon aim's call and puts it back after.
// - pdz_fix_derez_snapshot (emulation, not a game bug): the death "de-rez"
//   effect resolves a 256 x 256 snapshot of the body into the texture at
//   [82DA4614] [82DA3958] (sub_825F64E8 [825E6AD8]) and its DeResThread reads
//   the pixels on the CPU (sub_825F66E0 [825E6CD0]) to colour the cubes. Our
//   GPU emulation keeps resolves on the GPU, so the CPU read black and the
//   effect ended with no cubes (BUG_AUDIT VR8-VR10). The fix registers that
//   texture's memory with the SDK's resolve readback ranges, so that one
//   resolve is read back to guest memory before the game's GPU callback
//   wakes the thread.
// - pdz_fix_random_bytes (emulation, not a game bug): every random byte the
//   game asks the system for goes through sub_82773740 [827639E0] (buffer, length)
//   to NetDll_XNetRandom, which the SDK stubs with a constant 0xBB fill. That
//   seeds the game's main random generator the same on every boot
//   (sub_826365A0, "Seeded mathRand() with seed of") and makes direct users
//   repeat, e.g. Protection's live terminal (1 <-> 2 forever, log 037) and
//   hacker pick. 18 callers. The fix fills the buffer with real random bytes,
//   as the console does.
#include "game_fixes.h"
#include "fov_tuning.h"

#include <algorithm>
#include <cstring>
#include <random>

#include <rex/graphics/resolve_readback.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "../game_cvars.h"
#include "fix_log.h"
#include "hook_profile.h" // pdz_hook_profile (must follow <rex/hook.h>)

namespace look_tuning
{
  bool FastLookSetting();
  bool FastLookSetting(uint32_t player);
}

REX_EXTERN(__imp__sub_82240A90);
REX_EXTERN(__imp__sub_8258F410);
REX_EXTERN(__imp__sub_825F64E8);
REX_EXTERN(__imp__sub_82773740);
REX_EXTERN(sub_825AA278);

namespace
{
  uint32_t LoadBe32(const uint8_t *base, uint32_t address)
  {
    const uint8_t *p = base + address;
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
  }

  void StoreBe32(uint8_t *base, uint32_t address, uint32_t value)
  {
    uint8_t *p = base + address;
    p[0] = uint8_t(value >> 24);
    p[1] = uint8_t(value >> 16);
    p[2] = uint8_t(value >> 8);
    p[3] = uint8_t(value);
  }

  float LoadBeFloat(const uint8_t *base, uint32_t address)
  {
    const uint32_t bits = LoadBe32(base, address);
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
  }

  void StoreBeFloat(uint8_t *base, uint32_t address, float value)
  {
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    StoreBe32(base, address, bits);
  }

  // Guest heap and stack objects can sit below 0x40000000 too (the player
  // and its weapons do), so only rule out null-ish and the top page.
  bool IsGuestPointer(uint32_t address)
  {
    return address >= 0x00010000u && address < 0xFFFF0000u;
  }

  bool Master()
  {
    return REXCVAR_GET(pdz_game_fixes);
  }

  bool HaloZoomCurve()
  {
    return Master() && REXCVAR_GET(pdz_fix_halo_zoom_curve);
  }

  bool SpreadCapOrder()
  {
    return Master() && REXCVAR_GET(pdz_fix_spread_cap_order);
  }

  bool AutoAimZoomBend()
  {
    return Master() && REXCVAR_GET(pdz_fix_autoaim_zoom_bend);
  }

  bool CmpNoise()
  {
    return Master() && REXCVAR_GET(pdz_fix_cmp_noise);
  }

  // The player class has sub_825AAEF0 [8259B6D8] at vtable +748 [+744].
  bool IsPlayer(const uint8_t *base, uint32_t object)
  {
    if (!object)
    {
      return false;
    }
    const uint32_t vtbl = LoadBe32(base, object);
    return vtbl >= 0x82000000u && vtbl < 0x833E0000u && LoadBe32(base, vtbl + 748) == 0x825AAEF0;
  }

  // Gun noise (sub_8258F410's object: owner +0x5C4, weapon +0x494; the
  // weapon's record +0x35C, gun type +0x478; record noise per shot +0xF0,
  // minimum +0xF4). Same layout in TU3.
  constexpr uint32_t kShotOwner = 0x5C4;
  constexpr uint32_t kShotWeapon = 0x494;
  constexpr uint32_t kRecordGunType = 0x478;
  constexpr uint32_t kRecordNoisePerShot = 0xF0;
  constexpr uint32_t kRecordNoiseMinimum = 0xF4;
  constexpr uint32_t kGunTypeCmp = 0x11;

  // Input fields (TU3 moved them by +0x210).
  constexpr uint32_t kInputStyle = 0x694;            // [0x484] 1 = "Halo" model (Standard)
  constexpr uint32_t kInputSensitivity = 0x6E4;      // [0x4D4]
  constexpr uint32_t kInputSensitivityBoost = 0x702; // [0x4F2]
  constexpr double kFastLookOffBonus = 1.2;
  constexpr double kBoost = 1.75;
  // Look records: [max, default, min] x [pd, halo] at 82DA45FC [82DA3760].
  constexpr uint32_t kHaloMaxRecord = 0x82DA4600;     // [82DA3764]
  constexpr uint32_t kHaloDefaultRecord = 0x82DA4608; // [82DA376C]
  constexpr uint32_t kFieldZoomedCurve = 3;

  // Spread cap (sub_82588078).
  constexpr uint32_t kSpreadAimReturn = 0x825880E8;   // [82578860]
  constexpr uint32_t kWeaponRecord = 0x35C;
  constexpr uint32_t kRecordScoped = 0xE4;   // byte: the zoomed caps apply
  constexpr uint32_t kRecordSpreadCap = 0xBD; // byte, / 300
  constexpr uint32_t kPlayerCrouch = 0x1684;  // < 0 while crouched
  constexpr double kSpreadUnit = 1.0 / 300.0;

  // pdz_fix_autoaim_zoom_bend.
  constexpr uint32_t kPlayerZoom = 0x1A9C;            // 0 = not zoomed
  constexpr uint32_t kPlayerBentPoint = 0x1F58;       // x, y passed to the weapon aim
  constexpr uint32_t kAutoAimReticle = 0x1F60 + 0x30; // x, y, where the player aims

  // pdz_fix_derez_snapshot: the snapshot texture pointer (+20 base address).
  constexpr uint32_t kSnapshotTexture = 0x82DA4614;   // [82DA3958]
  constexpr uint32_t kSnapshotBytes = 256 * 256 * 4;

  void BeforeDeRezSnapshot(uint8_t *base)
  {
    static uint32_t registered = 0;         // guest physical address, 0 = none
    static uint32_t registered_address = 0; // its virtual address
    const uint32_t texture = LoadBe32(base, kSnapshotTexture);
    const uint32_t address = IsGuestPointer(texture) ? LoadBe32(base, texture + 20) & ~0xFFFu : 0;
    const uint32_t physical = address & 0x1FFFFFFFu;
    const bool on = Master() && REXCVAR_GET(pdz_fix_derez_snapshot) && physical;
    if (registered && (!on || registered != physical))
    {
      rex::graphics::resolve_readback::Remove(registered);
      // Clear the last snapshot read back, so with the fix off the CPU sees
      // what it sees without it (black), not the previous victim.
      std::memset(base + registered_address, 0, kSnapshotBytes);
      registered = 0;
      registered_address = 0;
    }
    if (on && !registered)
    {
      rex::graphics::resolve_readback::Add(physical, kSnapshotBytes);
      registered = physical;
      registered_address = address;
      fix_log::Count("de-rez snapshot registered");
      REXLOG_INFO("pdz_fix_derez_snapshot: the de-rez snapshot (texture {:08X}, memory {:08X}) is read back "
                  "from the GPU",
                  texture, address);
    }
  }
} // namespace

namespace game_fixes
{
  void BeforeLook(uint8_t *base, uint32_t player, uint32_t input, LookPatch &patch)
  {
    if (!HaloZoomCurve() || !IsGuestPointer(input) || LoadBe32(base, input + kInputStyle) != 1)
    {
      return;
    }
    // The blend parameter the Halo model will use (x1.2 with the Fast Look
    // option off, x1.75 with the sensitivity boost).
    double s = LoadBeFloat(base, input + kInputSensitivity);
    if (base[input + kInputSensitivityBoost])
    {
      s *= kBoost;
    }
    if (!look_tuning::FastLookSetting(player)) // log 099: this player's own option (split screen)
    {
      s *= kFastLookOffBonus;
    }
    if (!(s < 0.5))
    {
      return; // at or above default the max record's field 3 is meant to be used
    }
    const uint32_t max = LoadBe32(base, kHaloMaxRecord);
    const uint32_t def = LoadBe32(base, kHaloDefaultRecord);
    if (!IsGuestPointer(max) || !IsGuestPointer(def))
    {
      return;
    }
    fix_log::Count("halo zoom curve patched (frames)");
    patch.field = max + kFieldZoomedCurve * 4;
    patch.saved = LoadBe32(base, patch.field);
    StoreBe32(base, patch.field, LoadBe32(base, def + kFieldZoomedCurve * 4));
  }

  void AfterLook(uint8_t *base, const LookPatch &patch)
  {
    if (patch.field)
    {
      StoreBe32(base, patch.field, patch.saved);
    }
  }

  std::string ActiveSummary()
  {
    if (!Master())
    {
      return "off";
    }
    std::string text;
    auto add = [&](bool on, const char *name)
    {
      if (on)
      {
        text += text.empty() ? name : std::string(", ") + name;
      }
    };
    add(REXCVAR_GET(pdz_fix_halo_zoom_curve), "halo zoom curve");
    add(REXCVAR_GET(pdz_fix_spread_cap_order), "spread cap order");
    add(REXCVAR_GET(pdz_fix_autoaim_zoom_bend), "zoomed auto-aim bend");
    add(REXCVAR_GET(pdz_fix_cmp_noise), "CMP noise");
    add(REXCVAR_GET(pdz_fix_derez_snapshot), "de-rez snapshot");
    add(REXCVAR_GET(pdz_fix_random_bytes), "random bytes");
    add(REXCVAR_GET(pdz_fix_ai_semiauto_rhythm), "AI semi-auto rhythm");
    add(REXCVAR_GET(pdz_fix_ai_grenade_height), "AI grenade height");
    add(REXCVAR_GET(pdz_fix_ai_dodge_facing), "AI dodge facing");
    add(REXCVAR_GET(pdz_fix_ai_dodge_clearance), "AI dodge clearance");
    add(REXCVAR_GET(pdz_fix_ai_corner_speed), "AI corner speed");
    add(REXCVAR_GET(pdz_fix_ai_calm_flag), "AI calm flag");
    add(REXCVAR_GET(pdz_fix_speech_names), "speech names");
    add(REXCVAR_GET(pdz_fix_sound_names), "sound names");
    add(REXCVAR_GET(pdz_fix_ai_cover_min_distance), "AI cover min distance");
    return text.empty() ? "on, no fixes selected" : text;
  }
} // namespace game_fixes

// sub_82240A90 [822397C8](player, ..., f2 = spread, ...): the shot direction.
// From the player's weapon aim (returns to 825880E8) the spread is not
// capped yet.
REX_HOOK_RAW(sub_82240A90)
{
  if (uint32_t(ctx.lr) == kSpreadAimReturn && SpreadCapOrder())
  {
    // sub_82588078 keeps the weapon in r31 and the player in r26 / r3.
    const uint32_t weapon = ctx.r31.u32;
    const uint32_t player = ctx.r26.u32;
    const uint32_t record = IsGuestPointer(weapon) ? LoadBe32(base, weapon + kWeaponRecord) : 0;
    if (IsGuestPointer(record) && player)
    {
      // Same test and caps as 825880E8..825881D4.
      rex::CallFrame frame(ctx);
      frame.ctx.r3.u64 = player;
      sub_825AA278(frame.ctx, base);
      const bool zoomed = (frame.ctx.r3.u32 & 0xFF) && base[record + kRecordScoped];
      const bool crouched = LoadBeFloat(base, player + kPlayerCrouch) < 0.0f;
      const double stance = zoomed ? (crouched ? 0.5 : 0.75) : (crouched ? 1.0 : 1.5);
      const double cap = base[record + kRecordSpreadCap] * stance * kSpreadUnit;
      if (ctx.f2.f64 > cap)
      {
        fix_log::Count("spread capped before the shot direction (fix, frames)");
      }
      fix_log::Count("player weapon aim updates (frames)");
      ctx.f2.f64 = std::min(ctx.f2.f64, cap);
    }
  }
  // Zoomed: put the bend in the unzoomed units the weapon aim expects. With a wider field of view (fov_tuning.cpp:
  // the camera and the weapon aim both scaled) the bend, projected with the scaled camera, is scaled the same way.
  uint32_t bent = 0;
  uint32_t saved[2] = {};
  if (uint32_t(ctx.lr) == kSpreadAimReturn)
  {
    const uint32_t player = ctx.r26.u32;
    const float zoom = player ? LoadBeFloat(base, player + kPlayerZoom) : 0.0f;
    const bool zoom_fix = AutoAimZoomBend() && zoom > 0.0f && zoom < 1.0f;
    const float factor = (zoom_fix ? 1.0f - zoom : 1.0f) * float(pdz::fov::ScaleFor(player));
    if (player && ctx.r4.u32 == player + kPlayerBentPoint && factor != 1.0f)
    {
      bent = ctx.r4.u32;
      if (zoom_fix)
      {
        fix_log::Count("zoomed auto-aim bend scaled (fix, frames)");
      }
      for (uint32_t i = 0; i < 2; ++i)
      {
        saved[i] = LoadBe32(base, bent + i * 4);
        const float reticle = LoadBeFloat(base, player + kAutoAimReticle + i * 4);
        const float point = LoadBeFloat(base, bent + i * 4);
        StoreBeFloat(base, bent + i * 4, reticle + (point - reticle) * factor);
      }
    }
  }
  __imp__sub_82240A90(ctx, base);
  if (bent)
  {
    StoreBe32(base, bent, saved[0]);
    StoreBe32(base, bent + 4, saved[1]);
  }
}

// sub_8258F410 [8257FB90](shot, ...): a shot's effects, including its noise.
REX_HOOK_RAW(sub_8258F410)
{
  uint32_t field = 0;
  uint32_t saved = 0;
  if (CmpNoise())
  {
    const uint32_t shot = ctx.r3.u32;
    const uint32_t owner = LoadBe32(base, shot + kShotOwner);
    const uint32_t weapon = LoadBe32(base, shot + kShotWeapon);
    const uint32_t record = weapon ? LoadBe32(base, weapon + kWeaponRecord) : 0;
    if (IsPlayer(base, owner) && IsGuestPointer(record) && LoadBe32(base, record + kRecordGunType) == kGunTypeCmp)
    {
      field = record + kRecordNoiseMinimum;
      saved = LoadBe32(base, field);
      StoreBe32(base, field, LoadBe32(base, record + kRecordNoisePerShot));
      fix_log::Count("CMP shot noise minimum lowered (fix)");
    }
  }
  __imp__sub_8258F410(ctx, base);
  if (field)
  {
    StoreBe32(base, field, saved);
  }
}

// sub_825F64E8 [825E6AD8](): resolves the de-rez body snapshot into the
// texture and queues the GPU callback.
REX_HOOK_RAW(sub_825F64E8)
{
  BeforeDeRezSnapshot(base);
  __imp__sub_825F64E8(ctx, base);
}

// sub_82773740 [827639E0](buffer, length): random bytes for the game (XNetRandom).
REX_HOOK_RAW(sub_82773740)
{
  if (!Master() || !REXCVAR_GET(pdz_fix_random_bytes))
  {
    __imp__sub_82773740(ctx, base);
    return;
  }
  // pdz_random_seed (testing tool, stage 2 M5 cpusplit): a fixed seed makes the bytes repeat from boot to boot.
  static std::mt19937_64 rng{[] {
    const uint64_t fixed = REXCVAR_GET(pdz_random_seed);
    const uint64_t seed = fixed ? fixed : (uint64_t(std::random_device{}()) << 32) ^ std::random_device{}();
    if (fixed)
    {
      REXLOG_INFO("pdz_fix_random_bytes: random bytes from the fixed seed {} (pdz_random_seed)", fixed);
    }
    return seed;
  }()};
  const uint32_t buffer = ctx.r3.u32;
  const uint32_t length = ctx.r4.u32;
  for (uint32_t i = 0; IsGuestPointer(buffer) && i < length && i < 4096; ++i)
  {
    base[buffer + i] = uint8_t(rng());
  }
  fix_log::Count("random bytes filled (fix, calls)");
  ctx.r3.u64 = 0;
}
