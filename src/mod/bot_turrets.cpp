// Bots firing hardpoint turrets (pdz_bot_turrets, our fix, on by default).
// tu3-base only; TU3 addresses, launch addresses in brackets.
//
// Hardpoint turrets (DP_Turret "Hardpoint": the fixed M60 / plasma / rocket
// emplacements and the hovercraft's gunner seat, which uses the hardpoint
// mount animations) are updated by sub_8258CEA0 [8257D618]. Bots can mount
// them (play test: a bot climbed onto the hovercraft's turret) but never
// fire: in the operating state (turret +1280 == 3) the fire step runs only
// when the gunner's character vtable +384 (flags +64 bit 2, the human-player
// bit) is set. Unchanged in TU3. The step, for a human:
// - trigger: gunner vtable +508 (the character's input device +6284, vtable
//   +40; bots have one too, driven by their AI) above 0.2, or turret +920;
// - ready: sub_82591DB0 [82582520] (the turret) and the gun type's cooldown
//   (gun +860 -> +1144: 10 M60 +1452, 18 plasma +1456, 23 rocket +1460) <= 0;
// - fire: the gun's (+1172) fire object (+1280) vtable +60, turret +1289 = 1,
//   +920 = 0, cooldown = 0 (rocket 1.5 s); released: vtable +64, +1289 = 0.
// The gunner leaving stops the fire only for humans (sub_825910F0 [82581860]).
//
// With the option on, the same step runs after Rare's update for a bot gunner
// (character vtable 820BF3B8), from the bot's own fire request (turret +920:
// log 052 showed the bot's trigger stays 0 and its AI sets +920); a turret a
// bot was firing is stopped when the bot leaves it. Test (log 052):
// "turret worked". pdz_bot_turret_log (off by
// default) logs bot gunners: mount, state, trigger, readiness and shots.
#include <chrono>
#include <cstring>
#include <unordered_map>

#include <rex/hook.h>
#include "guest_function.h" // our indirect calls: nullptr for no function (log 101)
#include <rex/ppc/func.h>
#include <rex/logging.h>

#include "../game_cvars.h"
#include "hook_profile.h" // pdz_hook_profile (must follow <rex/hook.h>)

REX_EXTERN(__imp__sub_8258CEA0);
REX_EXTERN(sub_82591DB0);

namespace
{
  constexpr uint32_t kBotVtable = 0x820BF3B8;   // bot character class
  constexpr uint32_t kTurretGunner = 1476;      // turret: gunner (character)
  constexpr uint32_t kTurretState = 1280;       // turret: 3 = operating
  constexpr uint32_t kTurretGun = 1172;         // turret: gun
  constexpr uint32_t kTurretFiring = 1289;      // turret: firing (byte)
  constexpr uint32_t kTurretFireRequest = 920;  // turret: fire request (byte)
  constexpr uint32_t kTurretCooldownM60 = 1452;
  constexpr uint32_t kTurretCooldownPlasma = 1456;
  constexpr uint32_t kTurretCooldownRocket = 1460;
  constexpr uint32_t kGunParams = 860;          // gun: params -> +1144 gun type
  constexpr uint32_t kGunType = 1144;
  constexpr uint32_t kGunFire = 1280;           // gun: fire object (vtable +60 start, +64 stop)
  constexpr uint32_t kCharacterTrigger = 508;   // character vtable: trigger (f1)
  constexpr uint32_t kOperating = 3;
  constexpr float kTriggerPressed = 0.2f;
  constexpr float kRocketCooldown = 1.5f;

  uint32_t Load32(const uint8_t *base, uint32_t address)
  {
    const uint8_t *p = base + address;
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
  }

  void Store32(uint8_t *base, uint32_t address, uint32_t value)
  {
    uint8_t *p = base + address;
    p[0] = uint8_t(value >> 24);
    p[1] = uint8_t(value >> 16);
    p[2] = uint8_t(value >> 8);
    p[3] = uint8_t(value);
  }

  float LoadFloat(const uint8_t *base, uint32_t address)
  {
    const uint32_t bits = Load32(base, address);
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
  }

  void StoreFloat(uint8_t *base, uint32_t address, float value)
  {
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    Store32(base, address, bits);
  }

  bool IsGuestPointer(uint32_t address)
  {
    return address >= 0x00010000u && address < 0xFFFF0000u;
  }

  bool Log()
  {
    return REXCVAR_GET(pdz_bot_turret_log);
  }

  // A virtual call on `object` (r3 only); returns r3 and f1.
  struct Result
  {
    uint32_t r3 = 0;
    double f1 = 0.0;
  };

  Result CallVirtual(PPCContext &ctx, uint8_t *base, uint32_t object, uint32_t slot)
  {
    PPCFunc *fn = pdz::GuestFunction(Load32(base, Load32(base, object) + slot));
    if (!fn)
    {
      return {};
    }
    rex::CallFrame frame(ctx);
    frame.ctx.r3.u64 = object;
    fn(frame.ctx, base);
    return {frame.ctx.r3.u32, frame.ctx.f1.f64};
  }

  uint32_t Call(PPCContext &ctx, uint8_t *base, PPCFunc *fn, uint32_t r3)
  {
    rex::CallFrame frame(ctx);
    frame.ctx.r3.u64 = r3;
    fn(frame.ctx, base);
    return frame.ctx.r3.u32;
  }

  bool IsBot(const uint8_t *base, uint32_t character)
  {
    return IsGuestPointer(character) && Load32(base, character) == kBotVtable;
  }

  // Turrets a bot is firing: turret -> the bot.
  std::unordered_map<uint32_t, uint32_t> g_firing;

  // The gun's fire object, or 0.
  uint32_t FireObject(const uint8_t *base, uint32_t turret)
  {
    const uint32_t gun = Load32(base, turret + kTurretGun);
    return IsGuestPointer(gun) ? Load32(base, gun + kGunFire) : 0;
  }

  void StopFiring(PPCContext &ctx, uint8_t *base, uint32_t turret)
  {
    const uint32_t fire = FireObject(base, turret);
    if (IsGuestPointer(fire))
    {
      CallVirtual(ctx, base, fire, 64);
    }
    base[turret + kTurretFiring] = 0;
  }
} // namespace

// sub_8258CEA0 [8257D618](turret, f1 dt): the hardpoint turret's update.
REX_HOOK_RAW(sub_8258CEA0)
{
  const uint32_t turret = ctx.r3.u32;
  __imp__sub_8258CEA0(ctx, base);
  if (!REXCVAR_GET(pdz_bot_turrets) || !IsGuestPointer(turret))
  {
    return;
  }
  const uint32_t gunner = Load32(base, turret + kTurretGunner);
  const uint32_t state = Load32(base, turret + kTurretState);
  const bool bot = IsBot(base, gunner);

  // A bot that was firing left (or the turret stopped operating).
  const auto firing = g_firing.find(turret);
  if (firing != g_firing.end() && (!bot || gunner != firing->second || state != kOperating))
  {
    if (base[turret + kTurretFiring])
    {
      StopFiring(ctx, base, turret);
    }
    if (Log())
    {
      REXLOG_INFO("pdz_bot_turret: turret {:08X}: bot {:08X} stops firing (gunner {:08X}, state {})", turret,
                  firing->second, gunner, state);
    }
    g_firing.erase(firing);
  }
  if (!bot)
  {
    return;
  }

  const uint32_t gun = Load32(base, turret + kTurretGun);
  const uint32_t params = IsGuestPointer(gun) ? Load32(base, gun + kGunParams) : 0;
  const int32_t type = IsGuestPointer(params) ? int32_t(Load32(base, params + kGunType)) : -1;
  const float trigger = state == kOperating ? float(CallVirtual(ctx, base, gunner, kCharacterTrigger).f1) : 0.0f;
  const bool pressed = trigger > kTriggerPressed || base[turret + kTurretFireRequest];

  if (Log())
  {
    // Once per second per turret, and on every state change.
    struct Seen
    {
      uint32_t state = 0xFFFFFFFFu;
      std::chrono::steady_clock::time_point at;
    };
    static std::unordered_map<uint32_t, Seen> seen;
    Seen &s = seen[turret];
    const auto now = std::chrono::steady_clock::now();
    if (s.state != state || now - s.at >= std::chrono::seconds(1))
    {
      REXLOG_INFO("pdz_bot_turret: turret {:08X}: bot {:08X} gunner, state {}, gun type {}, trigger {:.2f}, "
                  "firing {}",
                  turret, gunner, state, type, trigger, base[turret + kTurretFiring]);
      s = {state, now};
    }
  }
  if (state != kOperating || !IsGuestPointer(gun))
  {
    return;
  }

  if (pressed)
  {
    uint32_t cooldown = 0;
    switch (type)
    {
    case 10:
      cooldown = kTurretCooldownM60;
      break;
    case 18:
      cooldown = kTurretCooldownPlasma;
      break;
    case 23:
      cooldown = kTurretCooldownRocket;
      break;
    default:
      return; // Rare's step: an unknown gun never fires
    }
    if (LoadFloat(base, turret + cooldown) > 0.0f || !(Call(ctx, base, sub_82591DB0, turret) & 0xFF))
    {
      return;
    }
    const uint32_t fire = FireObject(base, turret);
    if (IsGuestPointer(fire))
    {
      CallVirtual(ctx, base, fire, 60);
    }
    base[turret + kTurretFiring] = 1;
    base[turret + kTurretFireRequest] = 0;
    StoreFloat(base, turret + cooldown, type == 23 ? kRocketCooldown : 0.0f);
    if (Log() && g_firing.find(turret) == g_firing.end())
    {
      REXLOG_INFO("pdz_bot_turret: turret {:08X}: bot {:08X} fires (gun type {}, trigger {:.2f})", turret, gunner,
                  type, trigger);
    }
    g_firing[turret] = gunner;
  }
  else if (base[turret + kTurretFiring])
  {
    StopFiring(ctx, base, turret);
  }
}
