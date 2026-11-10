// pdz_unarmed_log (Diagnostics, off): Rare's "unarmed reaction" (char +0x30E0: 0 attack, 1 ignore, 2 greet, 3 warn;
// the Laboratory research follow-up Q1 / Q2, docs/NIGHTCLUB_GUARDS.md 3a-3b). The level-wide
// greeting broadcast sub_8214F0F8(r4 = event) gives every character with exactly 2 the pending sighting event
// (+0x30DC) that it says when it sees an EMPTY-HANDED target: Laboratory event 23 ("she checks out", 167
// SEE_FRIEND_TEAM, 823241C8), Nightclub 164 SEE_PLAYER_PARTY (822AE384), Mansion 161 (8232E1AC). After Rare's
// broadcast this logs every character with a non-zero unarmed reaction: setup id, position, +0x30E0, +0x30DC.
// It also logs each local player's hands (+0xFEC / +0xFF0 items with their class bits, +0xFF4 count, Rare's weapon
// test sub_8254BC28: an item whose class word 0 has bit 0x10) whenever they change, whatever changed them (Rare's
// own D-pad UP holster, which toggles; a switch; a pickup; a cutscene), so a log shows when Jo is empty-handed.
#include <cstdint>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>
#include "hook_profile.h" // pdz_hook_profile (must follow <rex/hook.h>)

REXCVAR_DEFINE_BOOL(pdz_unarmed_log, false, "Diagnostics",
                    "Log Rare's unarmed-reaction state: who gets the greeting at a level's greeting broadcast "
                    "(Laboratory \"she checks out\", Nightclub party, Mansion guests) and player 1's hands when "
                    "they change (empty hands = guards with the greet reaction leave Jo alone)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_STRING(pdz_unarmed_watch, "", "Diagnostics",
                      "With pdz_unarmed_log: once a second, log these characters by setup id (comma-separated, e.g. "
                      "\"s1m2,s2s1,s4l2\"): distance to player 1, unarmed reaction +0x30E0 / pending greeting +0x30DC, "
                      "the sight bits (+6268: 0x400000 sees a hostile, 0x800000 it is a valid target), the per-frame "
                      "stimuli (+6260) and the perception profile's alert / search counts and squared sight range")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_STRING(pdz_test_autopilot, "", "Testing",
                      "TESTING TOOL: walk player 1 through these waypoints (\"x,z;x,z;...\" in cm, from AI pads): each "
                      "frame the view is turned to the next waypoint and the left stick pushed forward "
                      "(pdz_test_autopilot_speed); a waypoint counts as reached within 60 cm. Changing the value "
                      "starts over; empty = off. For scripted approach tests (pdz_test_autopilot lines in the log)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(pdz_test_autopilot_speed, 22000, "Testing",
                     "pdz_test_autopilot: the left stick's forward push (22000 = a walk, 32767 = a run)")
    .range(0, 32767)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REX_EXTERN(__imp__sub_8214F0F8);
REX_EXTERN(sub_825AD3B0); // a human's look angles (character, f1 pitch, f2 yaw; radians): pdz_test_autopilot // greeting broadcast (r4 event): every character with +0x30E0 == 2 gets +0x30DC = event

namespace
{
  uint32_t Load32(const uint8_t *base, uint32_t a)
  {
    const uint8_t *p = base + a;
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
  }
  float LoadF(const uint8_t *base, uint32_t a)
  {
    const uint32_t v = Load32(base, a);
    float f;
    std::memcpy(&f, &v, 4);
    return f;
  }
  bool Ptr(uint32_t a)
  {
    return a >= 0x10000u && a < 0xFFFF0000u;
  }
  std::string Id(const uint8_t *base, uint32_t c)
  {
    const uint32_t data = Load32(base, c + 0x54);
    if (!Ptr(data))
    {
      return "?";
    }
    const uint32_t text = Load32(base, data + 0x34);
    if (!Ptr(text))
    {
      return "?";
    }
    std::string s;
    for (int i = 0; i < 16 && base[text + i]; ++i)
    {
      s += char(base[text + i]);
    }
    return s;
  }
  std::string Item(const uint8_t *base, uint32_t item)
  {
    if (!Ptr(item))
    {
      return "-";
    }
    const uint32_t cls = Load32(base, item + 8);
    const uint32_t w0 = Ptr(cls) ? Load32(base, cls) : 0, w2 = Ptr(cls) ? Load32(base, cls + 8) : 0;
    return fmt::format("{:08X} (type {}, class w0 {:08X}{} w2 {:08X}{})", item, int32_t(Load32(base, item + 12)), w0,
                       (w0 & 0x10) ? " weapon" : "", w2, (w2 & 0x20) ? " gadget" : "");
  }
  bool Armed(const uint8_t *base, uint32_t item)
  {
    const uint32_t cls = Ptr(item) ? Load32(base, item + 8) : 0;
    return Ptr(cls) && (Load32(base, cls) & 0x10) != 0;
  }
  uint32_t g_hands[4][2] = {};
  uint32_t g_watch_frames = 0;

  // pdz_test_autopilot
  struct Autopilot
  {
    std::string source;
    std::vector<std::pair<float, float>> points;
    size_t next = 0;
    bool active = false;
    float best = 1e30f;
    uint32_t frames_since_best = 0;
    int16_t stick = 0;
  };
  Autopilot g_ap;

  // pdz_unarmed_watch: the named characters, from Rare's character list (0x82E454A0 +0x710 .. +0x714).
  void Watch(const uint8_t *base, uint32_t player)
  {
    const std::string want = "," + std::string(REXCVAR_GET(pdz_unarmed_watch)) + ",";
    if (want.size() <= 2)
    {
      return;
    }
    constexpr uint32_t list = 0x82E454A0;
    uint32_t node = Load32(base, list + 0x710);
    const uint32_t end = Load32(base, list + 0x714);
    for (int guard = 0; node != end && Ptr(node) && guard < 2048; ++guard)
    {
      const uint32_t c = Load32(base, node);
      node = Load32(base, node + 8);
      if (!Ptr(c))
      {
        continue;
      }
      const std::string id = Id(base, c);
      if (want.find("," + id + ",") == std::string::npos)
      {
        continue;
      }
      const float dx = LoadF(base, c + 48) - LoadF(base, player + 48), dy = LoadF(base, c + 52) - LoadF(base, player + 52),
                  dz = LoadF(base, c + 56) - LoadF(base, player + 56);
      const int32_t slot = int32_t(Load32(base, c + 0x354));
      std::string prof = "no profile";
      if (slot >= 0 && slot < 171)
      {
        const uint32_t pr = 0x82E5D590 + 0x50 * uint32_t(slot);
        prof = fmt::format("alert {} search {} range2 {:.0f}", int16_t(uint16_t(base[pr + 0xC] << 8 | base[pr + 0xD])),
                           uint16_t(base[pr + 0xE] << 8 | base[pr + 0xF]), LoadF(base, pr + 0x1C));
      }
      REXLOG_INFO("pdz_unarmed: watch {} ({:08X}) at ({:.1f}, {:.1f}, {:.1f}), {:.0f} cm from player 1 | unarmed "
                  "reaction {} greeting {} | sight {:08X} (sees hostile {}, valid target {}) stimuli {:08X} | {}",
                  id, c, LoadF(base, c + 48), LoadF(base, c + 52), LoadF(base, c + 56),
                  std::sqrt(dx * dx + dy * dy + dz * dz), Load32(base, c + 0x30E0), Load32(base, c + 0x30DC),
                  Load32(base, c + 0x187C), (Load32(base, c + 0x187C) >> 22) & 1, (Load32(base, c + 0x187C) >> 23) & 1,
                  Load32(base, c + 0x1874), prof);
    }
  }
} // namespace

namespace
{
  void AutopilotTick(PPCContext &ctx, uint8_t *base, uint32_t player)
  {
    const std::string value = REXCVAR_GET(pdz_test_autopilot);
    if (value != g_ap.source)
    {
      g_ap = Autopilot{};
      g_ap.source = value;
      std::string t = value;
      for (char &ch : t)
      {
        if (ch == ';')
        {
          ch = '\n';
        }
      }
      std::istringstream lines(t);
      std::string line;
      while (std::getline(lines, line))
      {
        float x = 0, z = 0;
        if (std::sscanf(line.c_str(), "%f,%f", &x, &z) == 2)
        {
          g_ap.points.emplace_back(x, z);
        }
      }
      g_ap.active = !g_ap.points.empty();
      if (g_ap.active)
      {
        REXLOG_INFO("pdz_test_autopilot: {} waypoints", g_ap.points.size());
      }
    }
    if (!g_ap.active)
    {
      g_ap.stick = 0;
      return;
    }
    const float px = LoadF(base, player + 48), pz = LoadF(base, player + 56);
    while (g_ap.next < g_ap.points.size())
    {
      const float dx = g_ap.points[g_ap.next].first - px, dz = g_ap.points[g_ap.next].second - pz;
      if (dx * dx + dz * dz > 60.0f * 60.0f)
      {
        break;
      }
      REXLOG_INFO("pdz_test_autopilot: waypoint {} of {} reached at ({:.0f} {:.0f} {:.0f})", g_ap.next + 1, g_ap.points.size(),
                  px, LoadF(base, player + 52), pz);
      ++g_ap.next;
      g_ap.best = 1e30f;
      g_ap.frames_since_best = 0;
    }
    if (g_ap.next >= g_ap.points.size())
    {
      REXLOG_INFO("pdz_test_autopilot: done");
      g_ap.active = false;
      g_ap.stick = 0;
      return;
    }
    const float dx = g_ap.points[g_ap.next].first - px, dz = g_ap.points[g_ap.next].second - pz;
    const float d = std::sqrt(dx * dx + dz * dz);
    if (d < g_ap.best - 5.0f)
    {
      g_ap.best = d;
      g_ap.frames_since_best = 0;
    }
    else if (++g_ap.frames_since_best == 480)
    {
      REXLOG_INFO("pdz_test_autopilot: no progress towards waypoint {} for 480 frames at ({:.0f} {:.0f} {:.0f}), {:.0f} cm "
                  "away",
                  g_ap.next + 1, px, LoadF(base, player + 52), pz, d);
    }
    rex::CallFrame frame(ctx);
    frame.ctx.r3.u64 = player;
    frame.ctx.f1.f64 = 0.0;
    frame.ctx.f2.f64 = std::atan2(double(dx), double(dz));
    sub_825AD3B0(frame.ctx, base);
    g_ap.stick = int16_t(REXCVAR_GET(pdz_test_autopilot_speed));
  }
} // namespace

// test_harness.cpp's pad hook (after the input script): pdz_test_autopilot's left stick for pad 0.
void PdzAutopilotPad(uint8_t *base, uint32_t user, uint32_t state)
{
  if (user != 0 || !g_ap.active || !Ptr(state))
  {
    return;
  }
  const uint16_t v = uint16_t(g_ap.stick);
  base[state + 8] = 0; // left stick x
  base[state + 9] = 0;
  base[state + 10] = uint8_t(v >> 8); // left stick y (forward)
  base[state + 11] = uint8_t(v);
}

// From mnk_layout.cpp's hook on the per-frame controller update sub_822748C8 (r3 controller = participant +1680,
// r4 participant; the character is participant +964): the hands line.
void PdzUnarmedControllerUpdate(PPCContext &ctx, uint8_t *base, uint32_t ctrl, uint32_t participant)
{
  if (!Ptr(ctrl) || !Ptr(participant) || ctrl != participant + 1680)
  {
    return;
  }
  const uint32_t player = Load32(base, participant + 964);
  const uint32_t user = Load32(base, ctrl);
  if (!Ptr(player) || user >= 4)
  {
    return;
  }
  if (user == 0)
  {
    AutopilotTick(ctx, base, player);
  }
  if (!REXCVAR_GET(pdz_unarmed_log))
  {
    return;
  }
  if (user == 0 && ++g_watch_frames % 60 == 0)
  {
    Watch(base, player);
  }
  const uint32_t h0 = Load32(base, player + 0xFEC), h1 = Load32(base, player + 0xFF0);
  if (h0 == g_hands[user][0] && h1 == g_hands[user][1])
  {
    return;
  }
  g_hands[user][0] = h0;
  g_hands[user][1] = h1;
  REXLOG_INFO("pdz_unarmed: player {:08X} (pad {}) hands changed: hand 0 {}, hand 1 {}, count {}, Rare's weapon test {}",
              player, user, Item(base, h0), Item(base, h1), int32_t(Load32(base, player + 0xFF4)),
              (Armed(base, h0) || Armed(base, h1)) ? "armed" : "unarmed");
}

// The broadcast walks Rare's character list at 0x82E454A0 +0x710 .. +0x714 (8214F0F8-8214F140: node +0 character,
// +8 next; r3 is not used).
REX_HOOK_RAW(sub_8214F0F8)
{
  constexpr uint32_t list = 0x82E454A0;
  const uint32_t event = ctx.r4.u32;
  __imp__sub_8214F0F8(ctx, base);
  if (!REXCVAR_GET(pdz_unarmed_log))
  {
    return;
  }
  uint32_t node = Load32(base, list + 0x710);
  const uint32_t end = Load32(base, list + 0x714);
  int greet = 0, other = 0;
  std::string names;
  for (int guard = 0; node != end && Ptr(node) && guard < 2048; ++guard)
  {
    const uint32_t c = Load32(base, node);
    if (Ptr(c))
    {
      const uint32_t reaction = Load32(base, c + 0x30E0);
      if (reaction != 0)
      {
        (reaction == 2 ? greet : other)++;
        REXLOG_INFO("pdz_unarmed: broadcast {}: {} ({:08X}) at ({:.0f} {:.0f} {:.0f}) unarmed reaction {} pending sighting "
                    "event {}",
                    event, Id(base, c), c, LoadF(base, c + 48), LoadF(base, c + 52), LoadF(base, c + 56), reaction,
                    Load32(base, c + 0x30DC));
        if (reaction == 2)
        {
          names += " " + Id(base, c);
        }
      }
    }
    node = Load32(base, node + 8);
  }
  REXLOG_INFO("pdz_unarmed: broadcast event {}: {} character(s) with the greet reaction got it:{} ({} with ignore / warn)",
              event, greet, names, other);
}
