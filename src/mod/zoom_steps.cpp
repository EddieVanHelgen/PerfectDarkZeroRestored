// Step zoom (pdz_zoom_steps, an enhancement of ours, off by default = Rare's zoom exactly). Title Update 3 addresses.
//
// Rare's zoom (read 2026-11-04, logs PDZ_Job6 001-003):
// - The update: sub_825AC408(player, f1 = dt), every frame. The gun in hand (player +4076, else +4080) gives the
//   zoom range and its number of levels: gun +860 (its record) +32 = most zoom, +36 = least, +40 = levels (1-3;
//   gun vtable +580 / +584 / +588, sub_825886F8 / 82588770 / 825887E8). Zoom amount (player +6812, field of view
//   x (1 - amount)) = least + (most - least) x fraction; fraction = Rare's table for (level, levels) (sub_825AC3B8:
//   82D35CB8 one level, CC0 two, CCC three). A level change blends from the old amount over Rare's time (+6808 start,
//   +6804 time left).
// - How the level is chosen: the player's input block (player +6560) +1692 = zoom mode (player vtable +576).
//   Mode 2 (what the game uses on the pad and with the mouse, logged): the aim / zoom input's depth (+1756, 0..1:
//   the left trigger, analog) picks the level: level = depth x (levels - 1) + 1, so a light pull gives level 1 and a
//   full pull the last level; depth 0 = no zoom. The mouse's right click is a full pull: always the last level.
//   Mode 3: each press of the zoom action (+1804 bit 8) steps 0 -> 1 -> .. -> levels -> 0.
// - R3: sets no action in the input block (+1800 held, +1804 pressed, +1808 released words): on foot it does nothing.
//
// pdz_zoom_steps (mode 2 only): while the aim / zoom input is held, the depth Rare reads is replaced, for this update
// only, by the depth of the level chosen here: level 1 at the press; R3 (pad) steps 1 -> 2 -> 3 -> 1; wheel up / down
// (mouse, SDK mnk zoom wheel) one level up / down, no wrap. Rare's own levels, amounts, blend and everything that
// reads the zoom (sensitivity, auto-aim, the aim enhancements) stay as they are. One-level guns: unchanged.
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/input/mnk/mnk_input_driver.h>
#include <rex/logging.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <set>
#include <string>
#include <tuple>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <fmt/format.h>

#include "../game_constants.h"
#include "zoom_steps.h"

REXCVAR_DEFINE_BOOL(pdz_zoom_steps, false, "Mouse",
                    "Step zoom: holding the aim / zoom input (LT, right click) zooms to the gun's first zoom level; R3 "
                    "(pad) steps to its next level and back to the first, the mouse wheel up / down to the next / "
                    "previous (while aiming the wheel doesn't switch weapons). Off = Rare's zoom (the trigger's depth "
                    "picks the level)");
REXCVAR_DEFINE_BOOL(pdz_zoom_log, false, "Mouse",
                    "Log the zoom: each level change with its cause, the guns' zoom data, and once the zoom data of "
                    "every gun record found in memory");

REX_EXTERN(__imp__sub_825AC408);

namespace
{
  std::atomic<uint16_t> g_pad_buttons[4] = {};
} // namespace

void pdz::zoom::OnPadButtons(uint32_t user, uint16_t buttons)
{
  if (user < 4)
  {
    g_pad_buttons[user].store(buttons, std::memory_order_relaxed);
  }
}

namespace
{
  constexpr uint32_t kPlayerHand0 = 4076;
  constexpr uint32_t kPlayerHand1 = 4080;
  constexpr uint32_t kPlayerInput = 6560;
  constexpr uint32_t kPlayerZoomAmount = 6812;
  constexpr uint32_t kPlayerZoomLevel = 6824;
  constexpr uint32_t kInputUser = 1680;
  constexpr uint32_t kInputZoomMode = 1692;
  constexpr uint32_t kInputZoomDepth = 1756;
  constexpr uint32_t kGunRecord = 860;
  constexpr uint32_t kRecordMostZoom = 32;
  constexpr uint32_t kRecordLeastZoom = 36;
  constexpr uint32_t kRecordLevels = 40;
  constexpr uint32_t kRecordGunType = 1144;
  constexpr uint32_t kModeDepth = 2;
  constexpr uint16_t kPadRightThumb = 0x0080;

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

  void StoreBe32(uint8_t *base, uint32_t address, uint32_t v)
  {
    uint8_t *p = base + address;
    p[0] = uint8_t(v >> 24);
    p[1] = uint8_t(v >> 16);
    p[2] = uint8_t(v >> 8);
    p[3] = uint8_t(v);
  }

  bool Valid(uint32_t address) { return address >= 0x10000u && address < 0xF0000000u; }

  // Rare's names for the GunTypes (record +1144), as the Sabotage work found them (cut_scenarios.cpp).
  const char *GunName(uint32_t type)
  {
    static const char *kNames[] = {
        "Null",       "Ak47",         "P99",          "MP5",          "Phoenix",      "Uzi",        "Flachete",
        "Falcon",     "Laptop",       "M16",          "M60",          "Magnum",       "Shotgun",    "SuperDragon",
        "MiniDragon", "Magsec",       "Plastigun",    "CMP",          "Plasmarifle",  "Reaper",     "Rcp120",
        "Gauss",      "Devastator",   "Rocketlauncher", "Slayer",     "Oocgun",       "Combatshield", "Viblade",
        "Capacirod",  "Tranquilizer", "Flashbang",    "Smokegrenade", "Soundstunner", "Grenade",    "Multimine",
        "Boomerang",  "Bombbot",      "Sniperrifle",  "Shockwave",    "Farsight",     "BlueFlag",   "RedFlag",
        "GreenFlag",  "YellowFlag",   "Explosives"};
    return type < sizeof(kNames) / sizeof(kNames[0]) ? kNames[type] : "?";
  }

  struct GunZoom
  {
    bool ok = false;
    uint32_t record = 0, type = 0, levels = 1;
    float most = 0.0f, least = 0.0f;
  };

  GunZoom ZoomOfRecord(const uint8_t *base, uint32_t record)
  {
    GunZoom z;
    if (!Valid(record))
    {
      return z;
    }
    z.record = record;
    z.type = LoadBe32(base, record + kRecordGunType);
    z.levels = LoadBe32(base, record + kRecordLevels);
    z.most = LoadBeFloat(base, record + kRecordMostZoom);
    z.least = LoadBeFloat(base, record + kRecordLeastZoom);
    z.ok = z.type < 64 && z.levels >= 1 && z.levels <= 8 && std::isfinite(z.most) && std::isfinite(z.least) &&
           z.most >= 0.0f && z.most < 1.0f && z.least >= 0.0f && z.least < 1.0f;
    return z;
  }

  // A weapon, as the zoom update checks it: the item's info (+8) has flag 0x10.
  bool IsWeapon(const uint8_t *base, uint32_t item)
  {
    if (!Valid(item))
    {
      return false;
    }
    const uint32_t info = LoadBe32(base, item + 8);
    return Valid(info) && (LoadBe32(base, info) & 0x10) != 0;
  }

  // The gun the zoom update takes its range from: the first hand's weapon when it has one, else the second's.
  GunZoom ZoomOfHeldGun(const uint8_t *base, uint32_t player)
  {
    uint32_t gun = LoadBe32(base, player + kPlayerHand0);
    if (!IsWeapon(base, gun))
    {
      gun = LoadBe32(base, player + kPlayerHand1);
    }
    return IsWeapon(base, gun) ? ZoomOfRecord(base, LoadBe32(base, gun + kGunRecord)) : GunZoom{};
  }

  // Once (pdz_zoom_log): the zoom data of every gun record in committed memory within 64 MB of a real one (records
  // matched by their fields: GunType 0-63, 1-8 levels, both zoom amounts in 0..1, most >= least); one line per
  // distinct GunType and zoom data.
  void LogAllRecords(const uint8_t *base, uint32_t record)
  {
    static std::atomic<bool> done{false};
    if (done.exchange(true))
    {
      return;
    }
    const uint32_t from = (record > 0x04000000u ? record - 0x04000000u : 0x10000u) & ~0xFFFFu;
    const uint32_t to = uint32_t(std::min<uint64_t>(uint64_t(record) + 0x04000000u, 0xE0000000u));
    std::set<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t>> seen;
    int found = 0;
    for (uint32_t region = from; region < to;)
    {
      MEMORY_BASIC_INFORMATION mbi{};
      if (!VirtualQuery(base + region, &mbi, sizeof(mbi)))
      {
        break;
      }
      const uint64_t region_end =
          std::min<uint64_t>(uint64_t(reinterpret_cast<const uint8_t *>(mbi.BaseAddress) - base) + mbi.RegionSize, to);
      const bool readable = mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
                            (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ |
                                            PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY));
      if (readable)
      {
        for (uint64_t a = region; a + kRecordGunType + 4 <= region_end; a += 4)
        {
          const GunZoom r = ZoomOfRecord(base, uint32_t(a));
          if (!r.ok || r.levels > 3 || r.most < r.least || r.type == 0)
          {
            continue;
          }
          uint32_t most_bits, least_bits;
          std::memcpy(&most_bits, &r.most, 4);
          std::memcpy(&least_bits, &r.least, 4);
          if (!seen.insert({r.type, r.levels, most_bits, least_bits}).second)
          {
            continue;
          }
          ++found;
          REXLOG_INFO("pdz_zoom: record {:08X}: {} (GunType {}): {} zoom level{}, zoom {:.3f} .. {:.3f} (x{:.2f} .. "
                      "x{:.2f})", uint32_t(a), GunName(r.type), r.type, r.levels, r.levels == 1 ? "" : "s", r.least,
                      r.most, 1.0 / (1.0 - r.least), 1.0 / (1.0 - r.most));
        }
      }
      region = uint32_t(std::max<uint64_t>(region_end, uint64_t(region) + 0x1000));
    }
    REXLOG_INFO("pdz_zoom: record scan {:08X}..{:08X}: {} distinct gun records", from, to, found);
  }

  struct StepState
  {
    uint32_t player = 0;
    bool held = false;
    uint32_t level = 1;
    bool r3_was_down = false;
    uint32_t last_record = 0;
    // pdz_zoom_log: the amount a level change ends at, logged once it stops moving.
    bool settling = false;
    float last_amount = -1.0f;
  };

  void LogSettled(const uint8_t *base, StepState &st, uint32_t player)
  {
    const float amount = LoadBeFloat(base, player + kPlayerZoomAmount);
    // +6804: the blend time left (0 = done).
    if (st.settling && amount == st.last_amount && LoadBeFloat(base, player + 6804) <= 0.0f)
    {
      st.settling = false;
      REXLOG_INFO("pdz_zoom: player {:08X}: zoom amount settled at {:.3f} (x{:.2f} magnification), Rare's level {}",
                  player, amount, 1.0 / std::max(0.001, 1.0 - amount), LoadBe32(base, player + kPlayerZoomLevel));
    }
    st.last_amount = amount;
  }
  StepState g_state[4];

  StepState &StateOf(uint32_t player)
  {
    for (StepState &s : g_state)
    {
      if (s.player == player)
      {
        return s;
      }
    }
    for (StepState &s : g_state)
    {
      if (s.player == 0)
      {
        s.player = player;
        return s;
      }
    }
    g_state[3] = StepState{};
    g_state[3].player = player;
    return g_state[3];
  }

  // The depth Rare turns into this level: level = depth x (levels - 1) + 1 (truncated), the last level at 1.0.
  float DepthForLevel(uint32_t level, uint32_t levels)
  {
    if (levels <= 1 || level >= levels)
    {
      return 1.0f;
    }
    return std::clamp((float(level) - 0.5f) / float(levels - 1), 0.01f, 1.0f);
  }
} // namespace

// sub_825AC408(player r3, f1 = dt): the zoom update.
REX_HOOK_RAW(sub_825AC408)
{
  const uint32_t player = ctx.r3.u32;
  const uint32_t input = Valid(player) ? LoadBe32(base, player + kPlayerInput) : 0;
  if (!Valid(input))
  {
    __imp__sub_825AC408(ctx, base);
    return;
  }
  const int32_t user = int32_t(LoadBe32(base, input + kInputUser));
  const uint32_t mode = LoadBe32(base, input + kInputZoomMode);
  const uint32_t depth_bits = LoadBe32(base, input + kInputZoomDepth);
  float depth;
  std::memcpy(&depth, &depth_bits, sizeof(depth));
  const bool on = REXCVAR_GET(pdz_zoom_steps);
  const bool log = REXCVAR_GET(pdz_zoom_log);
  if (user == 0)
  {
    // The keyboard and mouse are guest user 0's: the wheel zooms while aiming (SDK mnk).
    rex::input::mnk::SetZoomWheel(on && mode == kModeDepth);
  }
  StepState &st = StateOf(player);
  const bool local = user >= 0 && user < 4;
  const bool r3 = local && (g_pad_buttons[user].load(std::memory_order_relaxed) & kPadRightThumb) != 0;
  const bool r3_pressed = r3 && !st.r3_was_down;
  st.r3_was_down = r3;
  const int wheel = on && user == 0 ? rex::input::mnk::TakeZoomWheelSteps() : 0;
  const GunZoom gun = (on || log) ? ZoomOfHeldGun(base, player) : GunZoom{};
  if (log && gun.ok && gun.record != st.last_record)
  {
    st.last_record = gun.record;
    REXLOG_INFO("pdz_zoom: player {:08X} (user {}) holds {} (GunType {}, record {:08X}): {} zoom level{}, zoom {:.3f} "
                ".. {:.3f}, zoom mode {}", player, user, GunName(gun.type), gun.type, gun.record, gun.levels,
                gun.levels == 1 ? "" : "s", gun.least, gun.most, mode);
    LogAllRecords(base, gun.record);
  }
  // One-level guns, and guns whose levels all give the same zoom (the Uzi's two at zoom 0), stay as they are.
  if (!on || !local || mode != kModeDepth || !(depth > 0.0f) || !gun.ok || gun.levels < 2 || !(gun.most > gun.least))
  {
    if (st.held && log)
    {
      REXLOG_INFO("pdz_zoom: player {:08X}: aim released (zoom level was {})", player, st.level);
    }
    st.held = false;
    const uint32_t before = log ? LoadBe32(base, player + kPlayerZoomLevel) : 0;
    __imp__sub_825AC408(ctx, base);
    if (log)
    {
      LogSettled(base, st, player);
    }
    if (log && Valid(player) && LoadBe32(base, player + kPlayerZoomLevel) != before)
    {
      st.settling = true;
      REXLOG_INFO("pdz_zoom: player {:08X}: Rare's zoom level {} -> {} (depth {:.2f}, {})", player, before,
                  LoadBe32(base, player + kPlayerZoomLevel), depth, on ? "step zoom: not this gun / mode" : "step zoom off");
    }
    return;
  }
  const char *why = nullptr;
  if (!st.held)
  {
    st.held = true;
    st.level = 1;
    why = "aim pressed";
  }
  else if (r3_pressed)
  {
    st.level = st.level % gun.levels + 1;
    why = "R3";
  }
  if (wheel != 0)
  {
    const int next = std::clamp(int(st.level) + wheel, 1, int(gun.levels));
    if (uint32_t(next) != st.level || !why)
    {
      why = wheel > 0 ? "wheel up" : "wheel down";
    }
    st.level = uint32_t(next);
  }
  st.level = std::clamp<uint32_t>(st.level, 1, gun.levels);
  const float stepped = DepthForLevel(st.level, gun.levels);
  uint32_t stepped_bits;
  std::memcpy(&stepped_bits, &stepped, sizeof(stepped_bits));
  StoreBe32(base, input + kInputZoomDepth, stepped_bits);
  __imp__sub_825AC408(ctx, base);
  StoreBe32(base, input + kInputZoomDepth, depth_bits);
  if (log)
  {
    if (why)
    {
      st.settling = true;
    }
    LogSettled(base, st, player);
  }
  if (log && why)
  {
    REXLOG_INFO("pdz_zoom: player {:08X} (user {}) {}: {} zoom level {} of {} (Rare's level now {}, zoom amount "
                "{:.3f} on its way; real trigger depth {:.2f}, given {:.2f})",
                player, user, why, GunName(gun.type), st.level, gun.levels, LoadBe32(base, player + kPlayerZoomLevel),
                LoadBeFloat(base, player + kPlayerZoomAmount), depth, stepped);
  }
}
