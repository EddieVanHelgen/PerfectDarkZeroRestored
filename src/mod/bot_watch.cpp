// "watch:" bot diagnostic: see bot_watch.h. tu3-base only; TU3 addresses.
#include "bot_watch.h"

#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>
#include "hook_profile.h" // pdz_hook_profile (must follow <rex/hook.h>)

#include "../game_cvars.h"
#include "objective_bots.h"

REX_EXTERN(sub_8241F418);         // (DarkOps mode, player participant): the participant it spectates
REX_EXTERN(__imp__sub_821C4308);  // the bot activity chooser (brain, reason, target, force)

namespace
{
  constexpr uint32_t kParticipantList = 0x82E34224; // -> first participant (+4 next)
  constexpr uint32_t kParticipantKind = 944;        // 2 player, 3 bot
  constexpr uint32_t kParticipantCharacter = 964;
  constexpr uint32_t kParticipantIndex = 1008;
  constexpr uint32_t kDarkOpsMode = 0x82E2F66C;     // -> the DarkOps mode (class vtables 820B66E0-820B6DE0)
  constexpr uint32_t kSpectating = 16172;           // DarkOps mode: + participant index: spectating (82421298)
  constexpr uint32_t kBotVtable = 0x820BF3B8;
  constexpr uint32_t kPosition = 48;
  constexpr uint32_t kName = 136;
  constexpr uint32_t kAimPoint = 6216;              // bot: aim point (8252CF58), +6228 valid
  constexpr uint32_t kAimValid = 6228;
  constexpr uint32_t kGoal = 12976;
  constexpr uint32_t kMoveTarget = 14200;           // bot: the pad its goal routes to
  constexpr uint32_t kHand1 = 4080;                 // current hand
  constexpr uint32_t kBrainActivity = 56;
  constexpr uint32_t kBrainRoute = 92;              // brain: route task
  constexpr float kNearest = 4000.0f;               // cm: the nearest bot when not spectating
  constexpr double kBurst = 5.0;                    // s of per-frame lines
  constexpr double kFlipWindow = 0.25;              // s
  constexpr double kFlipDegrees = 30.0;
  constexpr int kChangesPerSecond = 2;              // more than this starts a burst
  constexpr size_t kBurstsPerMinute = 2;

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

  struct Sample
  {
    double t = 0.0;
    float pos[3] = {};
    double yaw = 0.0;
    bool has_yaw = false;
    bool aim = false; // the facing is the aim point's (else the movement's)
    uint32_t goal = 0, activity = 0, route = 0, target = 0, gun_state = 0;
  };

  uint32_t g_watched = 0;
  bool g_spectated = false; // the watched bot is the spectate target (else the nearest)
  std::chrono::steady_clock::time_point g_picked{};
  const auto g_start = std::chrono::steady_clock::now();
  std::deque<Sample> g_samples;
  std::vector<std::string> g_notes;
  std::string g_anim;
  double g_burst_until = -1.0;
  double g_burst_start = 0.0;
  std::unordered_map<uint32_t, std::deque<double>> g_bursts; // bot -> burst start times

  double Now()
  {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
  }

  std::string Name(const uint8_t *base, uint32_t character)
  {
    const uint32_t name = Load32(base, character + kName);
    if (!IsGuestPointer(name))
    {
      return "?";
    }
    std::string n(reinterpret_cast<const char *>(base + name), strnlen(reinterpret_cast<const char *>(base + name), 32));
    return n.rfind("BOT-", 0) == 0 ? n.substr(4) : n;
  }

  // The bot to watch: Rare's spectate target of the first local player (the
  // test the DarkOps HUD's "SPECTATING" line makes, 82421298), else the bot
  // nearest the player within 40 m.
  uint32_t Pick(PPCContext &ctx, uint8_t *base, bool &spectated)
  {
    uint32_t player = 0;
    std::vector<uint32_t> bots;
    uint32_t p = Load32(base, kParticipantList);
    for (int n = 0; IsGuestPointer(p) && n < 64; ++n, p = Load32(base, p + 4))
    {
      const uint32_t kind = Load32(base, p + kParticipantKind);
      if (kind == 2 && !player)
      {
        player = p;
      }
      else if (kind == 3)
      {
        const uint32_t character = Load32(base, p + kParticipantCharacter);
        if (IsGuestPointer(character) && Load32(base, character) == kBotVtable)
        {
          bots.push_back(character);
        }
      }
    }
    spectated = false;
    if (!player)
    {
      return 0;
    }
    const uint32_t mode = Load32(base, kDarkOpsMode);
    const uint32_t index = Load32(base, player + kParticipantIndex);
    if (IsGuestPointer(mode) && Load32(base, mode) >= 0x820B6600u && Load32(base, mode) < 0x820B6F00u && index < 64 &&
        base[mode + index + kSpectating])
    {
      rex::CallFrame frame(ctx);
      frame.ctx.r3.u64 = mode;
      frame.ctx.r4.u64 = player;
      sub_8241F418(frame.ctx, base);
      const uint32_t target = frame.ctx.r3.u32;
      const uint32_t character = IsGuestPointer(target) ? Load32(base, target + kParticipantCharacter) : 0;
      if (IsGuestPointer(character) && Load32(base, character) == kBotVtable)
      {
        spectated = true;
        return character;
      }
    }
    const uint32_t me = Load32(base, player + kParticipantCharacter);
    if (!IsGuestPointer(me))
    {
      return 0;
    }
    uint32_t best = 0;
    float best_d = kNearest;
    for (uint32_t bot : bots)
    {
      float d2 = 0.0f;
      for (uint32_t i = 0; i < 3; ++i)
      {
        const float d = LoadFloat(base, bot + kPosition + i * 4) - LoadFloat(base, me + kPosition + i * 4);
        d2 += d * d;
      }
      if (std::sqrt(d2) < best_d)
      {
        best = bot;
        best_d = std::sqrt(d2);
      }
    }
    return best;
  }

  double Wrap(double degrees)
  {
    while (degrees > 180.0)
    {
      degrees -= 360.0;
    }
    while (degrees < -180.0)
    {
      degrees += 360.0;
    }
    return degrees;
  }

  // Why a burst should start ("" for no).
  std::string Trigger(const Sample &now)
  {
    if (now.has_yaw)
    {
      for (const Sample &s : g_samples)
      {
        if (s.t >= now.t - kFlipWindow && s.has_yaw && s.aim == now.aim)
        {
          const double turn = Wrap(now.yaw - s.yaw);
          if (std::fabs(turn) > kFlipDegrees)
          {
            return fmt::format("facing ({}) turned {:.0f} deg in {:.2f} s", now.aim ? "aim" : "move", turn, now.t - s.t);
          }
          break;
        }
      }
    }
    int goal = 0, activity = 0, route = 0, target = 0, gun = 0;
    const Sample *prev = nullptr;
    for (const Sample &s : g_samples)
    {
      if (s.t < now.t - 1.0)
      {
        continue;
      }
      if (prev)
      {
        goal += s.goal != prev->goal;
        activity += s.activity != prev->activity;
        route += s.route != prev->route;
        target += s.target != prev->target;
        gun += s.gun_state != prev->gun_state;
      }
      prev = &s;
    }
    const std::pair<const char *, int> counts[] = {
        {"goal", goal}, {"activity", activity}, {"route task", route}, {"move target", target}, {"gun state", gun}};
    for (const auto &[what, n] : counts)
    {
      if (n > kChangesPerSecond)
      {
        return fmt::format("{} changed {} times in 1 s", what, n);
      }
    }
    return {};
  }
} // namespace

namespace bot_watch
{
  uint32_t Watched()
  {
    return g_watched;
  }

  void Note(uint32_t character, const std::string &what)
  {
    if (!character || character != g_watched)
    {
      return;
    }
    if (what.rfind("anim ", 0) == 0)
    {
      g_anim = what.substr(5);
    }
    if (g_notes.size() < 32)
    {
      g_notes.push_back(what);
    }
  }

  void Frame(PPCContext &ctx, uint8_t *base, uint32_t character, bool brain_skipped)
  {
    if (!REXCVAR_GET(pdz_scenario_log))
    {
      g_watched = 0;
      return;
    }
    const auto clock = std::chrono::steady_clock::now();
    if (clock - g_picked >= std::chrono::milliseconds(250))
    {
      g_picked = clock;
      bool spectated = false;
      const uint32_t pick = Pick(ctx, base, spectated);
      if (pick != g_watched)
      {
        g_watched = pick;
        g_samples.clear();
        g_notes.clear();
        g_anim.clear();
        g_burst_until = -1.0;
        if (pick)
        {
          REXLOG_INFO("watch: now watching {:08X} ({}, {})", pick, Name(base, pick),
                      spectated ? "spectated" : "nearest bot within 40 m");
        }
      }
      g_spectated = spectated;
    }
    if (character != g_watched || !character)
    {
      return;
    }
    Sample s;
    s.t = Now();
    for (uint32_t i = 0; i < 3; ++i)
    {
      s.pos[i] = LoadFloat(base, character + kPosition + i * 4);
    }
    const uint32_t brain = objective_bots::BrainOf(base, character);
    s.goal = Load32(base, character + kGoal);
    s.activity = brain ? Load32(base, brain + kBrainActivity) : 0xFFFFFFFFu;
    s.route = brain ? Load32(base, brain + kBrainRoute) : 0;
    s.target = Load32(base, character + kMoveTarget);
    const uint32_t gun = Load32(base, character + kHand1);
    const uint32_t fire = IsGuestPointer(gun) ? Load32(base, gun + 1280) : 0;
    s.gun_state = IsGuestPointer(fire) ? Load32(base, fire + 4) : 0xFFFFFFFFu;
    float speed = 0.0f;
    if (!g_samples.empty())
    {
      const Sample &last = g_samples.back();
      const double dt = s.t - last.t;
      const float dx = s.pos[0] - last.pos[0], dz = s.pos[2] - last.pos[2];
      speed = dt > 0.0 ? float(std::sqrt(dx * dx + dz * dz) / dt) : 0.0f;
      if (speed > 50.0f)
      {
        s.yaw = std::atan2(double(dx), double(dz)) * 57.29577951;
        s.has_yaw = true;
      }
    }
    if (base[character + kAimValid])
    {
      const float dx = LoadFloat(base, character + kAimPoint) - s.pos[0];
      const float dz = LoadFloat(base, character + kAimPoint + 8) - s.pos[2];
      if (dx * dx + dz * dz > 1.0f)
      {
        s.yaw = std::atan2(double(dx), double(dz)) * 57.29577951;
        s.has_yaw = true;
        s.aim = true;
      }
    }
    if (g_burst_until < 0.0 || s.t > g_burst_until)
    {
      g_burst_until = -1.0;
      const std::string why = Trigger(s);
      if (!why.empty())
      {
        auto &starts = g_bursts[character];
        while (!starts.empty() && s.t - starts.front() > 60.0)
        {
          starts.pop_front();
        }
        if (starts.size() < kBurstsPerMinute)
        {
          starts.push_back(s.t);
          g_burst_until = s.t + kBurst;
          g_burst_start = s.t;
          REXLOG_INFO("watch: burst {:08X} ({}, {}): {}", character, Name(base, character),
                      g_spectated ? "spectated" : "nearest", why);
        }
      }
    }
    if (g_burst_until >= 0.0)
    {
      std::string notes;
      for (const std::string &n : g_notes)
      {
        notes += (notes.empty() ? "" : "; ") + n;
      }
      REXLOG_INFO("watch: {} +{:.2f} s pos ({:.0f}, {:.0f}, {:.0f}) speed {:.1f} m/s facing {} goal {} activity {} "
                  "route {:08X} target {:08X} gun state {} anim {}{}{}",
                  Name(base, character), s.t - g_burst_start, s.pos[0], s.pos[1], s.pos[2], speed / 100.0f,
                  s.has_yaw ? fmt::format("{:.0f} ({})", s.yaw, s.aim ? "aim" : "move") : std::string("-"),
                  int32_t(s.goal), int32_t(s.activity), s.route, s.target, int32_t(s.gun_state),
                  g_anim.empty() ? std::string("-") : g_anim, brain_skipped ? "; brain skipped (our hold)" : "",
                  notes.empty() ? std::string() : "; writes: " + notes);
    }
    g_notes.clear();
    g_samples.push_back(s);
    while (!g_samples.empty() && s.t - g_samples.front().t > 1.5)
    {
      g_samples.pop_front();
    }
  }
} // namespace bot_watch

// sub_821C4308 [brain, reason, target, force]: Rare's bot activity chooser.
// Only noted for the watched bot (its caller and answer).
bool PdzHoldRoamRoute(const uint8_t *base, uint32_t brain, uint32_t request); // cut_scenarios.cpp
void PdzNoteRouteAsk(const uint8_t *base, uint32_t brain, uint32_t request, uint32_t lr); // cut_scenarios.cpp

REX_HOOK_RAW(sub_821C4308)
{
  if (ctx.r4.u32 == 1)
  {
    PdzNoteRouteAsk(base, ctx.r3.u32, ctx.r5.u32, uint32_t(ctx.lr));
  }
  // Rare's roam re-route (think, call at 821C2A38): cut_scenarios may keep
  // the current route (pdz_sabotage_people_stage1).
  if (ctx.r4.u32 == 1 && uint32_t(ctx.lr) == 0x821C2A3Cu && PdzHoldRoamRoute(base, ctx.r3.u32, ctx.r5.u32))
  {
    if (g_watched && IsGuestPointer(ctx.r3.u32) && Load32(base, ctx.r3.u32 + 64) == g_watched)
    {
      bot_watch::Note(g_watched, "roam re-route kept (ours: pad within 10 m, 4 s)");
    }
    return;
  }
  const uint32_t brain = ctx.r3.u32;
  const uint32_t watched = g_watched;
  const uint32_t character = watched && IsGuestPointer(brain) ? Load32(base, brain + 64) : 0;
  if (!watched || character != watched)
  {
    __imp__sub_821C4308(ctx, base);
    return;
  }
  const uint32_t reason = ctx.r4.u32;
  const uint32_t lr = uint32_t(ctx.lr);
  const uint32_t before = Load32(base, brain + kBrainActivity);
  __imp__sub_821C4308(ctx, base);
  const uint32_t after = Load32(base, brain + kBrainActivity);
  bot_watch::Note(character, fmt::format("chooser reason {} from {:08X}: activity {} -> {}", reason, lr,
                                         int32_t(before), int32_t(after)));
}
