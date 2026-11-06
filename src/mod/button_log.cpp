// Button log (pdz_button_log, diagnostic, off by default; needs log_level
// "info"). For the "first press of reload / secondary fire sometimes doesn't
// register" report: every press and release of A / B / X / Y, the bumpers and
// the triggers as the game read them, whether the game registered the press
// (its committed "pressed this frame" bits), and for X (reload) and B
// (secondary) what the hand did with it: started, or refused and why.
//
// How the game reads the pad (see docs/BUG_AUDIT.md, Controls):
// - Poll: sub_82714230, once per frame on the game thread (task
//   "JoyThreadEndFrame" 8226CC70), reads XInput into 82E2A868 + 16 * user
//   (+4 buttons, +6 / +7 triggers) and works out edges against its own copy
//   of the previous state (once per poll, not per read). The pads (array
//   [82D6B2D8], 4 pointers; +4 = record slot) accumulate them and
//   sub_8226C2E8 commits them: pad +8 = pressed this frame (XInput bits, LT
//   0x10000000, RT 0x20000000). The game never reads dwPacketNumber.
// - Hand: sub_825781E0 (r3 = hand: +92 owner, +1220 index (1 = right),
//   +1280 gun state machine) asks for a reload on a press of logical bit 2
//   (X) and a secondary-mode switch on a press of bit 14 (B; or held B while
//   the gun is already switching), only on the press frame. The gun accepts
//   a reload only in gun actions 1, 5, 6, 12 (sub_822322A0) and with room in
//   the clip and reserve ammo (sub_82232570), a switch only in 1, 10, 12, 18
//   or 14 with +24 set (sub_822336D0); a refused press is dropped, there is
//   no buffer.
#include <chrono>
#include <cstring>
#include <string>
#include <vector>

#include <rex/hook.h>
#include <rex/logging.h>

#include "../game_cvars.h"

REX_EXTERN(__imp__sub_82714230);
REX_EXTERN(__imp__sub_8226C2E8);
REX_EXTERN(__imp__sub_825781E0);
REX_EXTERN(sub_8253E380);

namespace
{
  constexpr uint32_t kPadStates = 0x82E2A868; // XINPUT_STATE per user, 16 bytes
  constexpr uint32_t kPads = 0x82D6B2D8;      // -> 4 pad pointers
  constexpr uint32_t kFrameCounter = 0x82D6B338;
  constexpr uint32_t kLtBit = 0x10000000;
  constexpr uint32_t kRtBit = 0x20000000;
  constexpr int kTriggerThreshold = 30; // as the poll (sub_82714688)

  struct Button
  {
    uint32_t bit;
    const char *name;
  };
  constexpr Button kButtons[] = {{0x1000, "A"},  {0x2000, "B"},  {0x4000, "X"},  {0x8000, "Y"},
                                 {0x0100, "LB"}, {0x0200, "RB"}, {kLtBit, "LT"}, {kRtBit, "RT"}};

  // Gun actions (gun state machine +4); names partly inferred.
  const char *ActionName(int32_t action)
  {
    switch (action)
    {
    case 1:
      return "idle";
    case 2:
      return "shot starting";
    case 3:
      return "automatic fire";
    case 5:
    case 6:
      return "after a shot";
    case 8:
      return "lowering";
    case 9:
      return "raising";
    case 10:
      return "reloading";
    case 12:
      return "idle fidget";
    case 14:
      return "switching fire mode";
    default:
      return "other";
    }
  }

  uint32_t LoadBe32(const uint8_t *base, uint32_t address)
  {
    const uint8_t *p = base + address;
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
  }

  uint16_t LoadBe16(const uint8_t *base, uint32_t address)
  {
    return uint16_t(base[address] << 8 | base[address + 1]);
  }

  bool IsGuestPointer(uint32_t address)
  {
    return address >= 0x00010000u && address < 0xFFFF0000u;
  }

  bool Log()
  {
    return REXCVAR_GET(pdz_button_log);
  }

  using Clock = std::chrono::steady_clock;
  const Clock::time_point g_start = Clock::now();
  double Ms()
  {
    return std::chrono::duration<double, std::milli>(Clock::now() - g_start).count();
  }

  // Per user: buttons (with trigger bits) at the last poll, when each went
  // down, and the edges of the current poll waiting for the commit.
  struct User
  {
    uint32_t held = 0;
    double down_at[32] = {};
    uint32_t pressed = 0;
    uint32_t released = 0;
  };
  User g_users[4];
  double g_last_poll = -1.0;
  uint64_t g_polls = 0;

  uint32_t ReadButtons(const uint8_t *base, int user)
  {
    const uint32_t state = kPadStates + 16 * user;
    uint32_t bits = LoadBe16(base, state + 4);
    if (base[state + 6] > kTriggerThreshold)
    {
      bits |= kLtBit;
    }
    if (base[state + 7] > kTriggerThreshold)
    {
      bits |= kRtBit;
    }
    return bits;
  }

  int BitIndex(uint32_t bit)
  {
    int i = 0;
    while (bit > 1)
    {
      bit >>= 1;
      ++i;
    }
    return i;
  }

  // The player class has sub_8259B6D8 at vtable +744 (see third_person.cpp).
  bool IsPlayer(const uint8_t *base, uint32_t object)
  {
    if (!IsGuestPointer(object))
    {
      return false;
    }
    const uint32_t vtbl = LoadBe32(base, object);
    return vtbl >= 0x82000000u && vtbl < 0x833D0000u && LoadBe32(base, vtbl + 744) == 0x8259B6D8;
  }

  bool AbilityOn(PPCContext &ctx, uint8_t *base, uint32_t character, uint64_t mask)
  {
    rex::CallFrame frame(ctx);
    frame.ctx.r3.u64 = character;
    frame.ctx.r4.u64 = mask;
    sub_8253E380(frame.ctx, base);
    return (frame.ctx.r3.u32 & 0xFF) != 0;
  }
} // namespace

// sub_82714230(f1 = dt): the pad poll.
REX_HOOK_RAW(sub_82714230)
{
  if (!Log())
  {
    __imp__sub_82714230(ctx, base);
    return;
  }
  __imp__sub_82714230(ctx, base);
  const double now = Ms();
  if (g_last_poll >= 0.0 && now - g_last_poll > 25.0)
  {
    REXLOG_INFO("pdz_button: t {:.1f} ms frame {}: {:.1f} ms since the last pad poll", now,
                LoadBe32(base, kFrameCounter), now - g_last_poll);
  }
  g_last_poll = now;
  ++g_polls;
  for (int u = 0; u < 4; ++u)
  {
    User &user = g_users[u];
    const uint32_t held = ReadButtons(base, u);
    user.pressed |= held & ~user.held;
    user.released |= user.held & ~held;
    for (const Button &b : kButtons)
    {
      if (held & ~user.held & b.bit)
      {
        user.down_at[BitIndex(b.bit)] = now;
      }
    }
    user.held = held;
  }
}

// sub_8226C2E8(): commits every pad's edges for this frame.
REX_HOOK_RAW(sub_8226C2E8)
{
  __imp__sub_8226C2E8(ctx, base);
  if (!Log())
  {
    return;
  }
  const double now = Ms();
  const uint32_t frame = LoadBe32(base, kFrameCounter);
  const uint32_t pads = LoadBe32(base, kPads);
  for (int u = 0; u < 4; ++u)
  {
    User &user = g_users[u];
    if (!user.pressed && !user.released)
    {
      continue;
    }
    // The pad committing this user's record slot.
    const uint8_t slot = base[0x82E2A840 + u];
    uint32_t committed = 0;
    bool found = false;
    for (int i = 0; IsGuestPointer(pads) && i < 4; ++i)
    {
      const uint32_t pad = LoadBe32(base, pads + 4 * i);
      if (IsGuestPointer(pad) && base[pad + 4] == slot)
      {
        committed = LoadBe32(base, pad + 8);
        found = true;
        break;
      }
    }
    for (const Button &b : kButtons)
    {
      if (user.pressed & b.bit)
      {
        REXLOG_INFO("pdz_button: t {:.1f} ms frame {} poll {}: user {} {} down, game {}", now, frame, g_polls, u,
                    b.name, !found ? "has no pad for it" : (committed & b.bit) ? "registered the press" : "did NOT register the press");
      }
      if (user.released & b.bit)
      {
        REXLOG_INFO("pdz_button: t {:.1f} ms frame {} poll {}: user {} {} up after {:.0f} ms", now, frame, g_polls, u,
                    b.name, now - user.down_at[BitIndex(b.bit)]);
      }
    }
    user.pressed = 0;
    user.released = 0;
  }
}

// sub_825781E0(hand): the hand's buttons (fire, reload, secondary).
REX_HOOK_RAW(sub_825781E0)
{
  if (!Log())
  {
    __imp__sub_825781E0(ctx, base);
    return;
  }
  const uint32_t hand = ctx.r3.u32;
  const uint32_t owner = IsGuestPointer(hand) ? LoadBe32(base, hand + 92) : 0;
  const uint32_t gun = IsGuestPointer(hand) ? LoadBe32(base, hand + 1280) : 0;
  const uint32_t ctrl = IsPlayer(base, owner) ? LoadBe32(base, owner + 0x19A0) : 0;
  const uint32_t pressed_ptr = IsGuestPointer(ctrl) ? LoadBe32(base, ctrl + 0x510) : 0;
  if (!IsGuestPointer(pressed_ptr) || int32_t(LoadBe32(base, ctrl + 0x480)) < 0)
  {
    __imp__sub_825781E0(ctx, base);
    return;
  }
  const uint32_t pressed = LoadBe32(base, pressed_ptr);
  const bool reload = pressed & 0x4;
  const bool secondary = pressed & 0x4000;
  if (!reload && !secondary)
  {
    __imp__sub_825781E0(ctx, base);
    return;
  }
  const uint32_t index = LoadBe32(base, hand + 1220);
  const int32_t before = IsGuestPointer(gun) ? int32_t(LoadBe32(base, gun + 4)) : -1;
  const bool switch_retry = IsGuestPointer(gun) && base[gun + 24];
  // Ability gates, as the hand code asks them (sub_8253E380).
  const bool hand_on = AbilityOn(ctx, base, owner, 0x80000000u);
  const bool reload_on = AbilityOn(ctx, base, owner, 1);
  const bool secondary_on = AbilityOn(ctx, base, owner, 0x200000);
  // Reload preconditions (sub_82232570 on the hand's weapon, the hand itself).
  const uint16_t clip = LoadBe16(base, hand + 1316);
  const uint32_t record = LoadBe32(base, hand + 860);
  const uint16_t clip_size = IsGuestPointer(record) ? LoadBe16(base, record + 154) : 0;
  uint16_t reserve = LoadBe16(base, hand + 1318);
  const uint32_t ammo_table = LoadBe32(base, hand + 1100);
  const uint32_t ammo_owner = LoadBe32(base, hand + 1248);
  if (IsGuestPointer(ammo_table) && IsGuestPointer(ammo_owner) && IsGuestPointer(LoadBe32(base, ammo_owner)))
  {
    reserve = LoadBe16(base, ammo_table + 2 * (base[LoadBe32(base, ammo_owner) + 199] + 26));
  }
  const uint32_t lock_obj = LoadBe32(base, hand + 1096);
  const bool weapon_locked = IsGuestPointer(lock_obj) && base[lock_obj + 28];

  __imp__sub_825781E0(ctx, base);

  const int32_t after = IsGuestPointer(gun) ? int32_t(LoadBe32(base, gun + 4)) : -1;
  const double now = Ms();
  const uint32_t frame = LoadBe32(base, kFrameCounter);
  const char *side = index == 1 ? "right" : "left";
  if (reload)
  {
    std::string result;
    if (after == 10 && before != 10)
    {
      result = "reload started";
    }
    else if (!hand_on)
    {
      result = "refused: hand input locked (ability 0x80000000)";
    }
    else if (!reload_on)
    {
      result = "refused: reload locked (ability 1)";
    }
    else if (!IsGuestPointer(gun))
    {
      result = "refused: no gun";
    }
    else if (clip_size && clip >= clip_size)
    {
      result = "refused: clip full";
    }
    else if (!reserve)
    {
      result = "refused: no reserve ammo";
    }
    else if (weapon_locked)
    {
      result = "refused: weapon locked";
    }
    else if (before != 1 && before != 5 && before != 6 && before != 12)
    {
      result = std::string("refused: gun busy, ") + ActionName(before) + " (action " + std::to_string(before) + ")";
    }
    else
    {
      result = "refused by this gun type's own rule (action " + std::to_string(before) + ")";
    }
    REXLOG_INFO("pdz_button: t {:.1f} ms frame {}: X (reload), {} hand, clip {}/{} reserve {}: {}; gun action {} -> {}",
                now, frame, side, clip, clip_size, reserve, result, before, after);
  }
  if (secondary)
  {
    std::string result;
    if (after == 14 && before != 14)
    {
      result = "fire mode switch started";
    }
    else if (before == 14 && after == 14)
    {
      result = "already switching";
    }
    else if (!hand_on)
    {
      result = "refused: hand input locked (ability 0x80000000)";
    }
    else if (!secondary_on)
    {
      result = "refused: secondary locked (ability 0x200000)";
    }
    else if (!IsGuestPointer(gun))
    {
      result = "refused: no gun";
    }
    else if (index != 1)
    {
      result = "refused or not for this hand (left hand needs a gun that allows it)";
    }
    else if (before != 1 && before != 10 && before != 12 && before != 18 && !(before == 14 && switch_retry))
    {
      result = std::string("refused: gun busy, ") + ActionName(before) + " (action " + std::to_string(before) + ")";
    }
    else
    {
      result = "refused by this gun type's own rule (action " + std::to_string(before) + ")";
    }
    REXLOG_INFO("pdz_button: t {:.1f} ms frame {}: B (secondary), {} hand: {}; gun action {} -> {}", now, frame,
                side, result, before, after);
  }
}
