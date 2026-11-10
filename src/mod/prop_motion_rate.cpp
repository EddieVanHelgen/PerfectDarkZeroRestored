// Animated props above 60 fps (pdz_fix_prop_root_motion, default on): doors and other animated props move by their
// animation's root motion at any frame rate, as at 60.
//
// The bug (8 Nov 2026, 0.9.3): at 120 fps Mansion's doors (the intercom door the guard opens, and doors opened
// with the Locktopus) move a little, then stop part-way and never open; at 60 they open fully. PDZ Reinstated's players
// see the same. Not Havok: the door (DoorProp, ".\Game\Prop\Props\DoorProp.cpp", vtable 820C27C8, per-frame update
// sub_82561240) is moved by its animation, not a motor. Each frame sub_8251E0B8(prop, prop +0x400, dt) advances the
// animation by dt, takes the root motion's delta since the previous frame (rotation quaternion + translation) and
// multiplies it into the prop's transform. Before that, 8251E19C asks sub_82705460 "is the delta rotation identity?"
// (|length^2 - 1| <= 1e-5 and |w - 1| <= 1e-5, the constant at 82073C2C) and, when it is and the translation is under
// 1e-10 squared, skips the frame: no transform is set and that frame's motion is lost for good (the deltas are
// per-frame, never accumulated). The test is per frame: |w - 1| = 1 - cos(angle / 2) <= 1e-5 means an angle under
// 0.51 degrees a frame, so Rare drops rotations slower than 30.7 deg/s at 60 fps, but slower than 61.5 deg/s at 120.
// A door swinging between those speeds moves at 60 and freezes at 120 while its state machine (driven by the
// animation's progress, which does follow dt) still finishes "opening".
//
// The fix: in sub_82705460, when the guest is paced above 60 Hz (pdz::pace::EffectiveHz) and Rare's test said
// "identity", the w test is repeated with 1e-5 * (60 / Hz)^2 (1 - cos scales with the angle squared): at 120,
// 2.5e-6, the same 30.7 deg/s Rare drops at 60. At 60, or with no pacing, nothing changes (Rare's result is kept
// untouched). sub_82705460's only caller is 8251E19C; sub_8251E0B8 also moves the other animated props (callers
// 82530DA4, 825601AC, 8258AD24, 82594814, 825A03B0, 825CD1DC), which get the same correction.
// Hand test (8 Nov 2026, 120 fps, Mansion, Locktopus): both halves of the door turned the full 77 degrees; of 359
// frames Rare's test dropped 357, the fix applied 185 of them. (Not every door moves this way: one Mansion door's
// root-motion turn is exactly zero at any rate; it moves otherwise and opened at 120 before the fix too.)
//
// pdz_door_30hz (default off): PDZ Reinstated's door method (their game_patches.cpp, MIT licence): the door's update runs at most every 1/30 s with the accumulated dt, so each frame's delta is
// large enough for Rare's test. Kept as an alternative for A/B; with pdz_fix_prop_root_motion on it isn't needed.
//
// pdz_fix_lab_keycard (default OFF since 0.9.3.1: unproven; the 8 Nov hand test still found the card on the floor
// beside a broken monitor): Laboratory's desk keycard (on the table, sometimes on the floor or missing at high
// frame rates) gets a fixed pickup body instead of a moving one, so it stays where Rare placed it. Ported from PDZ
// Reinstated's physics_timing.cpp (pdz_lab_key_stability, MIT licence): hook sub_825E0EF0 (the placed prop's body
// setup, r4 = prop, r5 = descriptor, +32 = motion type), only for model aid_darkcharacter_props_niknak_keycard at
// Rare's authored position (-3541.062, 787.969, 2166.362), mission 5, single player; motion 1 -> 7 for that call only.
// Since 0.9.4 the same switch also fixes the desk the card lies on and its desk group, each only at its authored
// spot: the table aid_darkcharacter_xenon_props_furniture_lab_table_level0 (-3450.81, 699.94, 2211.63), the
// computer_monitor (-3368.90, 787.50, 2229.43), the computer_keyboard (-3389.65, 799.38, 2180.94) (motion 1, dynamic)
// and the furniture_chair_swivel (-3392.36, 700.00, 2130.99) (motion 6, keyframed): Rare's sit-down snap of the
// scientist s4l3 at the level start flung the monitor and knocked the chair over at high frame rates.
//
// pdz_door_log (default off, diagnostics): a line per door per frame while a door is opening or closing, plus every
// state change: state, dt, the frame's root-motion rotation (degrees), whether Rare's test dropped it and whether the
// fix applied it, and the total turned since the move began. Summary of all props' drops once per level.
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <chrono>
#include <cstring>
#include <string>
#include <unordered_map>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/ppc/func.h>
#include "hook_profile.h" // pdz_hook_profile (must follow <rex/hook.h>)

#include "render/game_pace.h" // EffectiveHz

REXCVAR_DEFINE_BOOL(pdz_fix_prop_root_motion, true, "Gameplay",
                    "Doors and other animated props keep their animation's movement above 60 fps (Rare drops any "
                    "frame that turns less than 0.5 degrees, so at 120 fps Mansion's doors stopped part-way). At 60 "
                    "fps nothing changes")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(pdz_door_30hz, false, "Gameplay",
                    "Alternative door fix (PDZ Reinstated's): a door's update runs at most 30 times a second with the "
                    "time saved up. Off by default (pdz_fix_prop_root_motion covers it)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(pdz_fix_lab_keycard, true, "Gameplay",
                    "Laboratory: the desk keycard stays on the table at any frame rate (a fixed pickup body; from PDZ "
                    "Reinstated, MIT licence). Rare's card sits 9 cm inside the desk top and is forced out when "
                    "the scientist sits down (even at 30 fps on the console the card goes missing). The desk, its "
                    "monitor, keyboard and chair get fixed bodies too, so his sit-down can't throw them. On by default")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(pdz_door_log, false, "Diagnostics",
                    "Log doors per frame while they move (state, dt, the frame's turn, dropped / applied, total turn)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_lab_prop_log, false, "Diagnostics",
                    "Laboratory: log the props within 4 m of the desk keycard (position, its first 16 words, motion type) "
                    "every frame for 10 s after the level starts; then whenever one moves, and all every 10 s")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_fix_keyframe_jump, false, "Gameplay",
                    "Above 30 fps, a keyframed body whose target jumps (a character snapped into a chair, a teleport) "
                    "is driven at Rare's 30 fps rate until it gets there, so it hits what it overlaps no harder than on "
                    "the Xbox (Laboratory: the scientist sitting down at the keycard desk threw the desk, monitor and "
                    "keycard across the room at 120 fps). Continuous animation is unchanged. Default off until tested")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_INT32(pdz_test_open_doors, 0, "Diagnostics",
                     "Testing: open every closed door this many seconds after a level starts (0 = off)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REX_EXTERN(__imp__sub_82705460); // is the quaternion at r3 identity? (r3 = 1 / 0)
REX_EXTERN(__imp__sub_82561240); // DoorProp per-frame update (r3 = door, f1 = dt)
REX_EXTERN(__imp__sub_825E0EF0); // placed prop body setup (r4 = prop, r5 = descriptor)
REX_EXTERN(__imp__sub_824E8BC8);
REX_EXTERN(__imp__sub_824E7A08); // Havok hard keyframe (r3 = &target position, r4 = &target rotation, r6 = body, f1 = 1/dt) // the physics manager's update (f1 = frame dt): steps Havok once a frame
REX_EXTERN(sub_82562E98);          // DoorProp Open(r3 = door, r4 = side)

namespace
{
  constexpr uint32_t kGame = 0x82D6BFD8; // -> +0 game state (+8 = 21 in a level); session block (+25 mission, +40 mode)
  constexpr float kRareEps = 1e-5f;      // 82073C2C
  constexpr float kMinEps = 1e-6f;       // well above the delta's float noise (1.0's ulp below is 6e-8)
  constexpr uint32_t kDoorState = 0x49C; // 0 closed, 1 open, 2 opening, 3 closing

  uint32_t Load32(const uint8_t *base, uint32_t a)
  {
    const uint8_t *p = base + a;
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
  }
  float LoadF(const uint8_t *base, uint32_t a) { return std::bit_cast<float>(Load32(base, a)); }
  void Store32(uint8_t *base, uint32_t a, uint32_t v)
  {
    uint8_t *p = base + a;
    p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v);
  }
  bool Readable(uint8_t *base, uint32_t address, uint32_t size)
  {
    if (address < 0x10000u || uint64_t(address) + size > 0xFFFF0000ull)
    {
      return false;
    }
    for (uint32_t at = address; at < address + size; at = (at & ~0xFFFu) + 0x1000u)
    {
      size_t length = 1;
      rex::memory::PageAccess access = rex::memory::PageAccess::kNoAccess;
      if (!rex::memory::QueryProtect(base + at, length, access) ||
          (uint32_t(access) & uint32_t(rex::memory::PageAccess::kReadOnly)) == 0)
      {
        return false;
      }
    }
    return true;
  }
  float TurnDegrees(float w)
  {
    const float c = std::min(1.0f, std::fabs(w));
    return 2.0f * std::acos(c) * 57.2957795f;
  }

  // The frame record of the door whose update is running (game thread), filled by the identity test's hook.
  struct FrameRecord
  {
    uint32_t door = 0;
    uint32_t tests = 0, rare_dropped = 0, applied_by_fix = 0;
    float turn = 0.0f; // degrees, this frame (the last test's)
  };
  thread_local FrameRecord t_frame;

  struct DoorTrack
  {
    uint32_t door = 0, state = ~0u;
    float total = 0.0f, time = 0.0f;
    uint32_t frames = 0, dropped = 0, fixed = 0;
  };
  std::array<DoorTrack, 64> g_doors{};

  DoorTrack &Track(uint32_t door)
  {
    DoorTrack *free_slot = nullptr;
    for (auto &d : g_doors)
    {
      if (d.door == door)
      {
        return d;
      }
      if (!free_slot && d.door == 0)
      {
        free_slot = &d;
      }
    }
    DoorTrack &d = free_slot ? *free_slot : g_doors[door / 16 % g_doors.size()];
    d = DoorTrack{};
    d.door = door;
    return d;
  }

  // All props: Rare's drops and the fix's rescues, per level.
  uint64_t g_tests = 0, g_rare_dropped = 0, g_fixed = 0;
  uint32_t g_level_state = 0;
  std::chrono::steady_clock::time_point g_level_start{};
  std::array<uint32_t, 128> g_test_opened{};
  uint32_t g_test_opened_n = 0;

  // The door's authored position (its setup, +84 -> +4/8/12), stable across runs.
  std::string DoorWhere(const uint8_t *base, uint32_t door)
  {
    const uint32_t setup = Load32(base, door + 84);
    if (setup < 0x10000u)
    {
      return "?";
    }
    return fmt::format("({:.0f}, {:.0f}, {:.0f})", LoadF(base, setup + 4), LoadF(base, setup + 8),
                       LoadF(base, setup + 12));
  }

  struct Clock
  {
    uint32_t door = 0, model = 0, setup = 0;
    double pending = 0.0;
  };
  std::array<Clock, 64> g_clocks{};
} // namespace

REX_HOOK_RAW(sub_82705460)
{
  const uint32_t q = ctx.r3.u32;
  __imp__sub_82705460(ctx, base);
  ++g_tests;
  const bool rare_identity = ctx.r3.u32 != 0;
  bool fixed = false;
  float w = 1.0f;
  if (rare_identity)
  {
    ++g_rare_dropped;
    const uint32_t hz = pdz::pace::EffectiveHz();
    if (REXCVAR_GET(pdz_fix_prop_root_motion) && hz > 60 && hz <= 480)
    {
      const float s = 60.0f / float(hz);
      const float eps = std::max(kRareEps * s * s, kMinEps);
      w = LoadF(base, q + 12);
      if (!(std::fabs(w - 1.0f) <= eps))
      {
        ctx.r3.u64 = 0; // not identity at this rate: the caller applies the frame's motion
        fixed = true;
        ++g_fixed;
      }
    }
  }
  if (t_frame.door && REXCVAR_GET(pdz_door_log))
  {
    if (!rare_identity || !fixed)
    {
      w = LoadF(base, q + 12);
    }
    ++t_frame.tests;
    t_frame.rare_dropped += rare_identity ? 1 : 0;
    t_frame.applied_by_fix += fixed ? 1 : 0;
    t_frame.turn = TurnDegrees(w);
  }
}

REX_HOOK_RAW(sub_82561240)
{
  const uint32_t door = ctx.r3.u32;
  const double dt_in = ctx.f1.f64;
  const bool log = REXCVAR_GET(pdz_door_log) && door >= 0x10000u;

  // Once per level: the summary of the previous level's prop drops (when logging).
  const int32_t test_open_s = REXCVAR_GET(pdz_test_open_doors);
  if (log || (test_open_s > 0 && door >= 0x10000u))
  {
    const uint32_t so = Load32(base, kGame);
    const uint32_t state = so >= 0x10000u && Load32(base, so + 8) == 21 ? so : 0;
    if (state != g_level_state)
    {
      REXLOG_INFO("pdz_door: level change: animated-prop root-motion tests {}, Rare's identity drops {}, applied by "
                  "pdz_fix_prop_root_motion {} (paced {} Hz)",
                  g_tests, g_rare_dropped, g_fixed, pdz::pace::EffectiveHz());
      g_level_state = state;
      g_tests = g_rare_dropped = g_fixed = 0;
      g_doors.fill(DoorTrack{});
      g_level_start = std::chrono::steady_clock::now();
      g_test_opened_n = 0;
    }
  }

  // Testing (pdz_test_open_doors): open each closed door once, N s after the level starts.
  if (test_open_s > 0 && door >= 0x10000u && g_level_state && Load32(base, door + kDoorState) == 0 &&
      std::chrono::steady_clock::now() - g_level_start >= std::chrono::seconds(test_open_s))
  {
    bool done = false;
    for (uint32_t i = 0; i < g_test_opened_n; ++i)
    {
      done = done || g_test_opened[i] == door;
    }
    if (!done && g_test_opened_n < g_test_opened.size())
    {
      g_test_opened[g_test_opened_n++] = door;
      REXLOG_INFO("pdz_door: test: opening door {:08X} at {}", door, DoorWhere(base, door));
      const PPCRegister r3 = ctx.r3, r4 = ctx.r4;
      const PPCRegister f1 = ctx.f1;
      ctx.r4.u64 = 0;
      sub_82562E98(ctx, base);
      ctx.r3 = r3, ctx.r4 = r4, ctx.f1 = f1;
    }
  }

  // PDZ Reinstated's cadence (pdz_door_30hz): at most one native update per 1/30 s, with the time saved up.
  if (REXCVAR_GET(pdz_door_30hz) && door >= 0x10000u && std::isfinite(dt_in) && dt_in > 0.0 && dt_in <= 0.1)
  {
    Clock *clock = nullptr;
    for (auto &c : g_clocks)
    {
      if (c.door == door)
      {
        clock = &c;
        break;
      }
    }
    if (!clock)
    {
      clock = &g_clocks[door / 16 % g_clocks.size()];
      *clock = Clock{door, 0, 0, 0.0};
    }
    const uint32_t model = Load32(base, door + 72), setup = Load32(base, door + 84);
    if (clock->model != model || clock->setup != setup)
    {
      *clock = Clock{door, model, setup, 0.0};
    }
    clock->pending += dt_in;
    if (clock->pending + 1e-9 < 1.0 / 30.0)
    {
      return;
    }
    ctx.f1.f64 = clock->pending;
    clock->pending = 0.0;
  }

  if (!log)
  {
    __imp__sub_82561240(ctx, base);
    return;
  }

  const double dt = ctx.f1.f64;
  const uint32_t before = Load32(base, door + kDoorState);
  t_frame = FrameRecord{};
  t_frame.door = door;
  __imp__sub_82561240(ctx, base);
  const FrameRecord rec = t_frame;
  t_frame.door = 0;
  const uint32_t after = Load32(base, door + kDoorState);

  DoorTrack &d = Track(door);
  if (after != d.state)
  {
    REXLOG_INFO("pdz_door: door {:08X} at {} state {} -> {} (after {} frames, {:.3f} s; turned {:.2f} deg; frames "
                "dropped by Rare's test {}, applied by the fix {}; paced {} Hz)",
                door, DoorWhere(base, door), d.state == ~0u ? -1 : int(d.state), after, d.frames, d.time, d.total,
                d.dropped, d.fixed, pdz::pace::EffectiveHz());
    if (after == 2 || after == 3)
    {
      d.total = 0.0f, d.time = 0.0f, d.frames = 0, d.dropped = 0, d.fixed = 0;
    }
    d.state = after;
  }
  if (before == 2 || before == 3 || after == 2 || after == 3)
  {
    const bool applied = rec.tests && (rec.rare_dropped == 0 || rec.applied_by_fix != 0);
    if (applied)
    {
      d.total += rec.turn;
    }
    ++d.frames;
    d.time += float(dt);
    d.dropped += rec.rare_dropped;
    d.fixed += rec.applied_by_fix;
    REXLOG_INFO("pdz_door: door {:08X} f{} state {} dt {:.5f} turn {:.4f} deg {} total {:.2f} deg", door, d.frames,
                after, dt, rec.turn,
                !rec.tests ? "no motion test" : rec.applied_by_fix ? "APPLIED BY FIX" : rec.rare_dropped ? "DROPPED"
                                                                                                          : "applied",
                d.total);
  }
}

namespace
{
  // pdz_lab_prop_log: the props near the Laboratory desk keycard's authored spot.
  constexpr float kCardAt[3] = {-3541.062f, 787.969f, 2166.362f};
  struct LabProp
  {
    uint32_t prop = 0, setup = 0, motion = 0;
    std::string name;
    float last[3] = {};
  };
  std::array<LabProp, 16> g_lab{};
  uint32_t g_lab_n = 0, g_lab_state = 0;
  double g_lab_time = -1.0, g_lab_next = 0.0;

  std::string SetupName(uint8_t *base, uint32_t setup)
  {
    const uint32_t name = Readable(base, setup, 144) ? Load32(base, setup + 140) : 0;
    if (!Readable(base, name, 64))
    {
      return "?";
    }
    const char *c = reinterpret_cast<const char *>(base + name);
    size_t n = 0;
    while (n < 63 && c[n] >= 32 && c[n] < 127)
    {
      ++n;
    }
    return std::string(c, n);
  }

  std::string Words(const uint8_t *base, uint32_t a, int n)
  {
    std::string s;
    for (int i = 0; i < n; ++i)
    {
      s += fmt::format("{}{:.3f}", i ? " " : "", LoadF(base, a + 4 * i));
    }
    return s;
  }

  void LabRegister(uint8_t *base, uint32_t prop, uint32_t descriptor)
  {
    if (!REXCVAR_GET(pdz_lab_prop_log) || !Readable(base, prop, 128))
    {
      return;
    }
    const uint32_t session = Load32(base, kGame);
    const uint32_t setup = Load32(base, prop + 84);
    if (!Readable(base, session, 44) || base[session + 25] != 5 || !Readable(base, setup, 64))
    {
      return;
    }
    const float x = LoadF(base, setup + 4), y = LoadF(base, setup + 8), z = LoadF(base, setup + 12);
    const float d = std::sqrt((x - kCardAt[0]) * (x - kCardAt[0]) + (y - kCardAt[1]) * (y - kCardAt[1]) +
                              (z - kCardAt[2]) * (z - kCardAt[2]));
    if (!(d <= 400.0f))
    {
      return;
    }
    const uint32_t state = Load32(base, kGame);
    if (state != g_lab_state)
    {
      g_lab_state = state;
      g_lab_n = 0;
      g_lab_time = -1.0;
    }
    if (g_lab_n >= g_lab.size())
    {
      return;
    }
    LabProp &l = g_lab[g_lab_n++];
    l = LabProp{};
    l.prop = prop;
    l.setup = setup;
    l.motion = Readable(base, descriptor, 36) ? Load32(base, descriptor + 32) : ~0u;
    l.name = SetupName(base, setup);
    REXLOG_INFO("pdz_lab_prop: registered {:08X} '{}' type {} motion {} | setup at ({:.2f}, {:.2f}, {:.2f}), {:.0f} cm "
                "from the card | setup +16.. {} | prop +0.. {}",
                prop, l.name, Load32(base, prop + 12), l.motion, x, y, z, d, Words(base, setup + 16, 12),
                Words(base, prop, 32));
  }
} // namespace

namespace
{
  // pdz_fix_keyframe_jump: per body, the last distance to its target and whether it's catching up after a jump.
  struct KeyState
  {
    float last = 0.0f;
    bool jumping = false;
    uint32_t frames = 0;
  };
  std::unordered_map<uint32_t, KeyState> g_keys;
  uint32_t g_keys_state = 0;
  uint64_t g_jumps = 0;
} // namespace

// sub_824E7A08 (Havok's hard keyframe): the body's velocity = (target - its position) x 1/dt, so it gets there in one
// step. The position read: [body +80] (the motion) +96. Havok works in metres.
REX_HOOK_RAW(sub_824E7A08)
{
  const uint32_t target = ctx.r3.u32, body = ctx.r6.u32;
  const double inv_dt = ctx.f1.f64;
  const bool fix = REXCVAR_GET(pdz_fix_keyframe_jump);
  const bool log = REXCVAR_GET(pdz_lab_prop_log) && g_lab_time >= 0.0 && g_lab_time < 10.0;
  const uint32_t motion = body >= 0x10000u ? Load32(base, body + 80) : 0;
  if ((!fix && !log) || motion < 0x10000u || target < 0x10000u)
  {
    __imp__sub_824E7A08(ctx, base);
    return;
  }
  const float dx = LoadF(base, target) - LoadF(base, motion + 96);
  const float dy = LoadF(base, target + 4) - LoadF(base, motion + 100);
  const float dz = LoadF(base, target + 8) - LoadF(base, motion + 104);
  const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
  const uint32_t state = Load32(base, kGame);
  if (state != g_keys_state || g_keys.size() > 8192)
  {
    g_keys.clear();
    g_keys_state = state;
  }
  KeyState &k = g_keys[body];
  bool capped = false;
  if (fix && inv_dt > 31.0 && inv_dt < 1000.0)
  {
    if (!k.jumping && d > 0.08f && d > 3.0f * k.last + 0.02f)
    {
      k.jumping = true; // the target jumped: drive it at Rare's 30 fps rate until it gets there
      k.frames = 0;
      ++g_jumps;
    }
    if (k.jumping && (d < 0.01f || ++k.frames > 120))
    {
      k.jumping = false;
    }
    if (k.jumping)
    {
      ctx.f1.f64 = 30.0;
      capped = true;
    }
  }
  if (log && d > 0.05f)
  {
    REXLOG_INFO("pdz_lab_prop: keyframe t {:.4f} caller {:08X} body {:08X} target ({:.3f}, {:.3f}, {:.3f}) m, {:.3f} m "
                "away (last {:.3f}), 1/dt {:.1f} -> {:.1f} m/s{}",
                g_lab_time, uint32_t(ctx.lr), body, LoadF(base, target), LoadF(base, target + 4), LoadF(base, target + 8),
                d, k.last, inv_dt, d * ctx.f1.f64, capped ? " (pdz_fix_keyframe_jump: Rare's 30 fps rate)" : "");
  }
  k.last = d;
  __imp__sub_824E7A08(ctx, base);
}

// The physics manager's update: after Havok's step, log the registered props.
REX_HOOK_RAW(sub_824E8BC8)
{
  const double dt = ctx.f1.f64;
  __imp__sub_824E8BC8(ctx, base);
  if (!g_lab_n || !REXCVAR_GET(pdz_lab_prop_log))
  {
    return;
  }
  const uint32_t so = Load32(base, kGame);
  if (so < 0x10000u || Load32(base, so + 8) != 21)
  {
    return;
  }
  if (g_lab_time < 0.0)
  {
    g_lab_time = 0.0;
    g_lab_next = 0.0;
    REXLOG_INFO("pdz_lab_prop: level running: logging {} props (paced {} Hz)", g_lab_n, pdz::pace::EffectiveHz());
  }
  g_lab_time += dt;
  // Every frame for the first 10 s; after that, a prop whenever it moves (over 5 cm/s) and all of them every 10 s, for
  // the whole level (where the card is when the player gets there).
  const bool heartbeat = g_lab_time > 10.0 && g_lab_time >= g_lab_next;
  if (heartbeat)
  {
    g_lab_next = g_lab_time + 10.0;
  }
  for (uint32_t i = 0; i < g_lab_n; ++i)
  {
    LabProp &l = g_lab[i];
    const float p[3] = {LoadF(base, l.prop + 48), LoadF(base, l.prop + 52), LoadF(base, l.prop + 56)};
    const float v = dt > 0 ? float(std::sqrt((p[0] - l.last[0]) * (p[0] - l.last[0]) + (p[1] - l.last[1]) *
                                             (p[1] - l.last[1]) + (p[2] - l.last[2]) * (p[2] - l.last[2])) / dt)
                           : 0.0f;
    if (g_lab_time <= 10.0 || heartbeat || v > 5.0f)
    {
      REXLOG_INFO("pdz_lab_prop: t {:.4f} dt {:.4f} {} '{}' pos ({:.2f}, {:.2f}, {:.2f}) speed {:.1f} cm/s | +0.. {}",
                  g_lab_time, dt, i, l.name, p[0], p[1], p[2], v, Words(base, l.prop, 12));
    }
    std::memcpy(l.last, p, sizeof(p));
  }
}

REX_HOOK_RAW(sub_825E0EF0)
{
  const uint32_t descriptor = ctx.r5.u32;
  const uint32_t prop = ctx.r4.u32;
  LabRegister(base, prop, descriptor);
  bool key = false;
  if (REXCVAR_GET(pdz_fix_lab_keycard) && Readable(base, descriptor, 36) && Load32(base, descriptor + 32) == 1 &&
      Readable(base, prop, 96) && Load32(base, prop + 12) == 14 && Load32(base, prop + 92) == 0)
  {
    const uint32_t session = Load32(base, kGame);
    const uint32_t setup = Load32(base, prop + 84);
    if (Readable(base, session, 44) && Readable(base, setup, 144) && base[session + 25] == 5 &&
        Load32(base, session + 40) == 0)
    {
      static constexpr char kModel[] = "aid_darkcharacter_props_niknak_keycard";
      const uint32_t name = Load32(base, setup + 140);
      if (Readable(base, name, sizeof(kModel)) && std::memcmp(base + name, kModel, sizeof(kModel)) == 0)
      {
        constexpr float kAt[3] = {-3541.062f, 787.969f, 2166.362f};
        key = true;
        for (int i = 0; i < 3; ++i)
        {
          const float v = LoadF(base, setup + 4 + 4 * i);
          key = key && std::isfinite(v) && std::fabs(v - kAt[i]) <= 0.01f;
        }
      }
    }
  }
  // The desk group (0.9.4): the same switch, scope and spot test, for the card's desk, monitor, keyboard and chair.
  const char *desk_part = nullptr;
  uint32_t desk_motion = 0;
  if (!key && REXCVAR_GET(pdz_fix_lab_keycard) && Readable(base, descriptor, 36) && Readable(base, prop, 96) &&
      Load32(base, prop + 12) == 8)
  {
    desk_motion = Load32(base, descriptor + 32);
    const uint32_t session = Load32(base, kGame);
    const uint32_t setup = Load32(base, prop + 84);
    if ((desk_motion == 1 || desk_motion == 6) && Readable(base, session, 44) && Readable(base, setup, 144) &&
        base[session + 25] == 5 && Load32(base, session + 40) == 0)
    {
      struct DeskPart
      {
        const char *model;
        float at[3];
      };
      static constexpr DeskPart kDesk[] = {
          {"aid_darkcharacter_xenon_props_furniture_lab_table_level0", {-3450.81f, 699.94f, 2211.63f}},
          {"aid_darkcharacter_xenon_props_electrical_computer_monitor", {-3368.90f, 787.50f, 2229.43f}},
          {"aid_darkcharacter_xenon_props_electrical_computer_keyboard", {-3389.65f, 799.38f, 2180.94f}},
          {"aid_darkcharacter_xenon_props_furniture_chair_swivel", {-3392.36f, 700.00f, 2130.99f}},
      };
      const uint32_t name = Load32(base, setup + 140);
      for (const DeskPart &part : kDesk)
      {
        const size_t n = std::strlen(part.model) + 1;
        if (!Readable(base, name, uint32_t(n)) || std::memcmp(base + name, part.model, n) != 0)
        {
          continue;
        }
        bool here = true;
        for (int i = 0; i < 3; ++i)
        {
          const float v = LoadF(base, setup + 4 + 4 * i);
          here = here && std::isfinite(v) && std::fabs(v - part.at[i]) <= 0.05f;
        }
        if (here)
        {
          desk_part = part.model;
        }
        break;
      }
    }
  }
  if (desk_part)
  {
    Store32(base, descriptor + 32, 7); // fixed body for this call (Havok motion 7)
    __imp__sub_825E0EF0(ctx, base);
    Store32(base, descriptor + 32, desk_motion);
    REXLOG_INFO("pdz_fix_lab_keycard: Laboratory keycard desk: fixed body for '{}' (motion {} -> 7)", desk_part,
                desk_motion);
    return;
  }
  if (!key)
  {
    __imp__sub_825E0EF0(ctx, base);
    return;
  }
  Store32(base, descriptor + 32, 7); // fixed body for this call (Havok motion 7)
  __imp__sub_825E0EF0(ctx, base);
  Store32(base, descriptor + 32, 1);
  REXLOG_INFO("pdz_fix_lab_keycard: Laboratory desk keycard: fixed pickup body (from PDZ Reinstated)");
}
