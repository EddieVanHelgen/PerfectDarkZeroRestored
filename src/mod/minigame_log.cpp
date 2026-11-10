// Minigame calibration log (pdz_minigame_log, diagnostics, off by default).
// tu3-base only; TU3 addresses, launch addresses in brackets.
//
// Records every Data Thief and demo kit minigame the player plays, in single
// player and multiplayer, to calibrate simulated bot hacks (docs/TU3_BASE.md,
// "Objective bots"). Two CSV files in logs/:
// - minigame_events.csv: one row per event (press with its outcome, ring
//   change, success, failure, damage taken);
// - minigame_sessions.csv: one row per minigame (time per ring, presses,
//   hits, misses, wrong blocks, result, damage).
//
// Data Thief minigame (object = subgame; update sub_82647A08 [82637BA0]):
//   +4 gadget (-> +92 holder), +32 state (2 = failed), +36 rings, +40 blocks
//   per ring, +44 blocks (20 bytes: type byte, +4 / +8 / +12 / +16 speed
//   change by difficulty), +48 cursor speed (blocks per second, 1-20), +52
//   cursor phase, +56 press lockout (0.2 s after a press), +128 ring, +132
//   block under the cursor. Block types: 0 empty (a press marks it an error
//   block: a miss; the third error in a ring drops a ring, at ring 0 the hack
//   fails), 1 blue ICE (a hit; the ring clears when none is left), 2 back a
//   ring, 3 slower, 4 faster, 5 clears the ring, 6 error block (a press is a
//   wrong block). Success: sub_826478E8 [82637A80] (PROCESSING cue).
// Demo kit minigame (update sub_8264ADE0 [8263AF90], a tile puzzle): events
// come from its cues (TURN_TILE = a press, CONNECT), success from the
// DO_EXPLOSIVES_SET cue the explosives subgame plays (825F0644).
// Damage: the player-class damage handler sub_825AAF50 [8259B738] (victim,
// attacker, f1 damage) for the minigame's holder.
// A minigame not updated for 1 s ends as "interrupted" (unless it ended).
#include "minigame_log.h"
#include "minigame_input.h" // NoteDemoKit: the keyboard fix follows the demo kit's puzzle too

#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include <fmt/format.h>

#include <rex/hook.h>
#include "guest_function.h" // our indirect calls: nullptr for no function (log 101)
#include <rex/ppc/func.h>
#include <rex/logging.h>

#include "../game_cvars.h"
#include "hook_profile.h" // pdz_hook_profile (must follow <rex/hook.h>)

REX_EXTERN(__imp__sub_82647A08);
REX_EXTERN(__imp__sub_826478E8);
REX_EXTERN(__imp__sub_8264ADE0);
REX_EXTERN(__imp__sub_825AAF50);

namespace
{
  using Clock = std::chrono::steady_clock;

  constexpr uint32_t kGameInfo = 0x82D6BFD8;    // -> +4 difficulty, +40 mode (2 = multiplayer)
  constexpr uint32_t kSubgameGadget = 4;
  constexpr uint32_t kGadgetHolder = 92;
  constexpr uint32_t kDtState = 32;
  constexpr uint32_t kDtRings = 36;
  constexpr uint32_t kDtBlocksPerRing = 40;
  constexpr uint32_t kDtBlocks = 44;
  constexpr uint32_t kDtSpeed = 48;
  constexpr uint32_t kDtLockout = 56;
  constexpr uint32_t kDtRing = 128;
  constexpr uint32_t kDtCursor = 132;
  constexpr uint32_t kDtFailed = 2;
  constexpr uint32_t kHealth = 332;              // character vtable: health (f1)
  constexpr uint32_t kCueTurnTile = 0x820CD88C;  // XACT_SOUNDBANK_GUNS_DEMOKIT_TURN_TILE
  constexpr uint32_t kCueConnect = 0x820CD868;   // XACT_SOUNDBANK_GUNS_DEMOKIT_CONNECT
  constexpr uint32_t kCueExplosivesSet = 0x820CB76C; // XACT_SOUNDBANK_MULTIPLAYER_DO_EXPLOSIVES_SET
  constexpr auto kIdle = std::chrono::seconds(1);

  uint32_t Load32(const uint8_t *base, uint32_t address)
  {
    const uint8_t *p = base + address;
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
  }

  float LoadFloat(const uint8_t *base, uint32_t address)
  {
    const uint32_t bits = Load32(base, address);
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
  }

  bool IsGuestPointer(uint32_t address)
  {
    return address >= 0x00010000u && address < 0xFFFF0000u;
  }

  bool Enabled()
  {
    return REXCVAR_GET(pdz_minigame_log);
  }

  enum class Kind
  {
    DataThief,
    DemoKit,
  };

  const char *KindName(Kind kind)
  {
    return kind == Kind::DataThief ? "datathief" : "demokit";
  }

  struct Session
  {
    uint32_t id = 0;
    uint32_t object = 0;
    uint32_t holder = 0;
    Kind kind = Kind::DataThief;
    uint32_t mode = 0;
    uint32_t difficulty = 0;
    Clock::time_point start;
    Clock::time_point last_update;
    Clock::time_point ring_start;
    std::vector<double> ring_times;
    int32_t rings = 0;
    int32_t ring = 0;
    uint32_t presses = 0;
    uint32_t hits = 0;
    uint32_t misses = 0;
    uint32_t wrong_blocks = 0;
    uint32_t specials = 0;
    uint32_t connects = 0;
    uint32_t damage_events = 0;
    float damage_total = 0.0f;
    std::string result;
  };

  Session g_session;
  bool g_active = false;
  uint32_t g_next_id = 1;
  FILE *g_events = nullptr;
  FILE *g_sessions = nullptr;

  // The minigame being updated on this thread (cues are attributed to it).
  thread_local uint32_t t_object = 0;
  thread_local Kind t_kind = Kind::DataThief;

  double Seconds(Clock::time_point from, Clock::time_point to)
  {
    return std::chrono::duration<double>(to - from).count();
  }

  bool Exists(const char *path)
  {
    FILE *f = std::fopen(path, "r");
    if (f)
    {
      std::fclose(f);
    }
    return f != nullptr;
  }

  bool Open()
  {
    if (!g_events)
    {
      const bool fresh = !Exists("logs/minigame_events.csv");
      g_events = std::fopen("logs/minigame_events.csv", "a");
      if (g_events && fresh)
      {
        std::fprintf(g_events, "session,time_s,minigame,mode,event,ring,rings,block,speed,health,detail\n");
      }
    }
    if (!g_sessions)
    {
      const bool fresh = !Exists("logs/minigame_sessions.csv");
      g_sessions = std::fopen("logs/minigame_sessions.csv", "a");
      if (g_sessions && fresh)
      {
        std::fprintf(g_sessions, "session,date,minigame,mode,difficulty,total_s,rings,rings_done,ring_times_s,"
                                 "presses,hits,misses,wrong_blocks,specials,connects,result,damage_events,"
                                 "damage_total\n");
      }
    }
    return g_events && g_sessions;
  }

  float Health(PPCContext &ctx, uint8_t *base, uint32_t character)
  {
    if (!IsGuestPointer(character) || !IsGuestPointer(Load32(base, character)))
    {
      return -1.0f;
    }
    PPCFunc *fn = pdz::GuestFunction(Load32(base, Load32(base, character) + kHealth));
    if (!fn)
    {
      return -1.0f;
    }
    rex::CallFrame frame(ctx);
    frame.ctx.r3.u64 = character;
    frame.ctx.r4.u64 = 0;
    fn(frame.ctx, base);
    return float(frame.ctx.f1.f64);
  }

  void Event(PPCContext &ctx, uint8_t *base, const char *event, int32_t block, const std::string &detail)
  {
    if (!Open())
    {
      return;
    }
    const float speed = g_session.kind == Kind::DataThief ? LoadFloat(base, g_session.object + kDtSpeed) : 0.0f;
    std::fprintf(g_events, "%u,%.3f,%s,%u,%s,%d,%d,%d,%.2f,%.1f,%s\n", g_session.id,
                 Seconds(g_session.start, Clock::now()), KindName(g_session.kind), g_session.mode, event,
                 g_session.ring, g_session.rings, block, speed, Health(ctx, base, g_session.holder), detail.c_str());
    std::fflush(g_events);
  }

  void Finish(const char *result)
  {
    if (!g_active)
    {
      return;
    }
    g_active = false;
    if (!Open())
    {
      return;
    }
    std::string times;
    for (double t : g_session.ring_times)
    {
      times += fmt::format("{}{:.2f}", times.empty() ? "" : " ", t);
    }
    const std::time_t now = std::time(nullptr);
    char date[32] = {};
    std::strftime(date, sizeof(date), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    std::fprintf(g_sessions, "%u,%s,%s,%u,%u,%.2f,%d,%zu,%s,%u,%u,%u,%u,%u,%u,%s,%u,%.1f\n", g_session.id, date,
                 KindName(g_session.kind), g_session.mode, g_session.difficulty,
                 Seconds(g_session.start, g_session.last_update), g_session.rings, g_session.ring_times.size(),
                 times.c_str(), g_session.presses, g_session.hits, g_session.misses, g_session.wrong_blocks,
                 g_session.specials, g_session.connects, result, g_session.damage_events, g_session.damage_total);
    std::fflush(g_sessions);
    if (g_events)
    {
      std::fprintf(g_events, "%u,%.3f,%s,%u,end,%d,%d,-1,0,0,%s\n", g_session.id,
                   Seconds(g_session.start, g_session.last_update), KindName(g_session.kind), g_session.mode,
                   g_session.ring, g_session.rings, result);
      std::fflush(g_events);
    }
    REXLOG_INFO("pdz_minigame: {} #{} {} in {:.1f} s ({} presses, {} hits, {} misses, {} wrong, {} damage events)",
                KindName(g_session.kind), g_session.id, result, Seconds(g_session.start, g_session.last_update),
                g_session.presses, g_session.hits, g_session.misses, g_session.wrong_blocks,
                g_session.damage_events);
  }

  // The session for this update (starts one if needed); false if off.
  bool Track(PPCContext &ctx, uint8_t *base, uint32_t object, Kind kind)
  {
    const auto now = Clock::now();
    if (g_active && (g_session.object != object || now - g_session.last_update > kIdle))
    {
      Finish("interrupted");
    }
    if (!g_active)
    {
      g_session = Session{};
      g_session.id = g_next_id++;
      g_session.object = object;
      g_session.kind = kind;
      const uint32_t gadget = Load32(base, object + kSubgameGadget);
      g_session.holder = IsGuestPointer(gadget) ? Load32(base, gadget + kGadgetHolder) : 0;
      const uint32_t info = Load32(base, kGameInfo);
      g_session.mode = IsGuestPointer(info) ? Load32(base, info + 40) : 0;
      g_session.difficulty = IsGuestPointer(info) ? Load32(base, info + 4) : 0;
      g_session.start = now;
      g_session.ring_start = now;
      if (kind == Kind::DataThief)
      {
        g_session.rings = int32_t(Load32(base, object + kDtRings));
        g_session.ring = int32_t(Load32(base, object + kDtRing));
      }
      g_active = true;
      Event(ctx, base, "start", -1, fmt::format("holder {:08X}", g_session.holder));
    }
    g_session.last_update = now;
    return true;
  }

  int32_t BlockUnderCursor(const uint8_t *base, uint32_t object)
  {
    const uint32_t blocks = Load32(base, object + kDtBlocks);
    const int32_t per = int32_t(Load32(base, object + kDtBlocksPerRing));
    const int32_t ring = int32_t(Load32(base, object + kDtRing));
    const int32_t cursor = int32_t(Load32(base, object + kDtCursor));
    if (!IsGuestPointer(blocks) || per <= 0 || per > 64 || ring < 0 || ring > 16 || cursor < 0 || cursor >= per)
    {
      return -1;
    }
    return base[blocks + uint32_t((ring * per + cursor) * 20)];
  }
} // namespace

// sub_82647A08 [82637BA0](subgame, f1 dt): the Data Thief minigame update.
REX_HOOK_RAW(sub_82647A08)
{
  const uint32_t object = ctx.r3.u32;
  if (!Enabled() || !IsGuestPointer(object))
  {
    __imp__sub_82647A08(ctx, base);
    return;
  }
  PPCContext saved = ctx;
  Track(saved, base, object, Kind::DataThief);
  const float lockout = LoadFloat(base, object + kDtLockout);
  const int32_t block = BlockUnderCursor(base, object);
  const int32_t ring = int32_t(Load32(base, object + kDtRing));
  t_object = object;
  t_kind = Kind::DataThief;
  __imp__sub_82647A08(ctx, base);
  t_object = 0;
  if (!g_active || g_session.object != object)
  {
    return; // finished during the update (success)
  }
  const float after = LoadFloat(base, object + kDtLockout);
  if (lockout <= 0.0f && after > 0.1f)
  {
    // A press: its outcome is the block it hit.
    ++g_session.presses;
    const char *outcome = "special";
    switch (block)
    {
    case 0:
      ++g_session.misses;
      outcome = "miss";
      break;
    case 1:
      ++g_session.hits;
      outcome = "hit";
      break;
    case 6:
      ++g_session.wrong_blocks;
      outcome = "wrong_block";
      break;
    default:
      ++g_session.specials;
      break;
    }
    Event(saved, base, "press", block, outcome);
  }
  const int32_t now_ring = int32_t(Load32(base, object + kDtRing));
  if (now_ring != ring)
  {
    const auto now = Clock::now();
    if (now_ring > ring)
    {
      g_session.ring_times.push_back(Seconds(g_session.ring_start, now));
    }
    g_session.ring_start = now;
    g_session.ring = now_ring;
    Event(saved, base, now_ring > ring ? "ring_up" : "ring_down", -1, fmt::format("{} -> {}", ring, now_ring));
  }
  if (Load32(base, object + kDtState) == kDtFailed)
  {
    Event(saved, base, "failed", -1, "");
    Finish("failed");
  }
}

// sub_826478E8 [82637A80](subgame): the Data Thief hack succeeded.
REX_HOOK_RAW(sub_826478E8)
{
  const uint32_t object = ctx.r3.u32;
  __imp__sub_826478E8(ctx, base);
  if (Enabled() && g_active && g_session.object == object)
  {
    g_session.ring_times.push_back(Seconds(g_session.ring_start, Clock::now()));
    g_session.ring = g_session.rings;
    Event(ctx, base, "success", -1, "");
    Finish("success");
  }
}

// sub_8264ADE0 [8263AF90](subgame, ...): the demo kit minigame update.
REX_HOOK_RAW(sub_8264ADE0)
{
  const uint32_t object = ctx.r3.u32;
  pdz::minigame::NoteDemoKit(object); // minigame_input.cpp: the demo kit's tile puzzle reads the left stick too
  if (!Enabled() || !IsGuestPointer(object))
  {
    __imp__sub_8264ADE0(ctx, base);
    return;
  }
  PPCContext saved = ctx;
  Track(saved, base, object, Kind::DemoKit);
  t_object = object;
  t_kind = Kind::DemoKit;
  __imp__sub_8264ADE0(ctx, base);
  t_object = 0;
}

void PdzCharacterHit(uint32_t victim); // cut_scenarios.cpp: Protection, hack under fire
#if PDZ_COUNTEROP
bool PdzCounterOpIsPlayer2(uint32_t character); // counter_op.cpp: player 2 of a running counter-op game
#endif

// sub_825AAF50 [8259B738](victim, attacker, f1 damage, ...): player damage.
REX_HOOK_RAW(sub_825AAF50)
{
  if (ctx.f1.f64 > 0.0)
  {
    PdzCharacterHit(ctx.r3.u32);
  }
  if (Enabled() && g_active && ctx.r3.u32 == g_session.holder && ctx.f1.f64 > 0.0)
  {
    ++g_session.damage_events;
    g_session.damage_total += float(ctx.f1.f64);
    Event(ctx, base, "damage", -1, fmt::format("{:.1f} from {:08X}", ctx.f1.f64, ctx.r4.u32));
  }
  // pdz_test_god_mode (testing tool): the player's damage handler (this
  // hook, the player class) runs with no damage, so the hit still happens
  // (enemies keep shooting and reacting) but no health is lost.
#if PDZ_COUNTEROP
  // Counter-op: god mode is for the agent; player 2 (the counter-operative) can still be killed.
  const bool counterop_p2 = PdzCounterOpIsPlayer2(ctx.r3.u32);
#else
  const bool counterop_p2 = false;
#endif
  if (REXCVAR_GET(pdz_test_god_mode) && ctx.f1.f64 > 0.0 && !counterop_p2)
  {
    ctx.f1.f64 = 0.0;
  }
#if PDZ_COUNTEROP
  // Counter-op diagnostic: what reaches player 2's damage handler (the agent's hits did no damage in co_jungle_2).
  static int counterop_lines = 0;
  const auto f32 = [base](uint32_t a) {
    uint32_t v = (uint32_t(base[a]) << 24) | (uint32_t(base[a + 1]) << 16) | (uint32_t(base[a + 2]) << 8) | base[a + 3];
    float f;
    std::memcpy(&f, &v, 4);
    return f;
  };
  const uint32_t victim = ctx.r3.u32, attacker = ctx.r4.u32;
  const double damage_in = ctx.f1.f64, f2_in = ctx.f2.f64;
  const uint32_t r7_in = ctx.r7.u32;
  const auto human_health = [base, &f32](uint32_t c) { // [+6560 participant]+980: a human's health
    const uint32_t p = (uint32_t(base[c + 6560]) << 24) | (uint32_t(base[c + 6561]) << 16) | (uint32_t(base[c + 6562]) << 8) | base[c + 6563];
    return p >= 0x10000u ? f32(p + 980) : f32(c + 120);
  };
  const float health_before = counterop_p2 ? human_health(victim) : 0.0f;
#endif
  __imp__sub_825AAF50(ctx, base);
#if PDZ_COUNTEROP
  if (counterop_p2 && counterop_lines < 40)
  {
    ++counterop_lines;
    REXLOG_INFO("pdz_counterop: damage handler 825AAF50: player 2 {:08X} from {:08X}: damage {:.2f}, f2 {:.2f}, part {}, "
                "floor +7828 {:.2f}, health {:.2f} -> {:.2f}",
                victim, attacker, damage_in, f2_in, r7_in, f32(victim + 7828), health_before, human_health(victim));
  }
#endif
}

namespace minigame_log
{
  void OnCue(PPCContext &ctx, uint8_t *base, uint32_t name)
  {
    if (!Enabled() || !g_active)
    {
      return;
    }
    if (!t_object && Clock::now() - g_session.last_update > kIdle)
    {
      Finish("interrupted"); // not updated for a second: walked off, put away, killed
      return;
    }
    if (t_object && t_object == g_session.object && t_kind == Kind::DemoKit)
    {
      if (name == kCueTurnTile)
      {
        ++g_session.presses;
        Event(ctx, base, "press", -1, "turn_tile");
      }
      else if (name == kCueConnect)
      {
        ++g_session.connects;
        Event(ctx, base, "connect", -1, "");
      }
    }
    if (name == kCueExplosivesSet && g_session.kind == Kind::DemoKit)
    {
      Event(ctx, base, "success", -1, "explosives set");
      Finish("success");
    }
  }
} // namespace minigame_log
