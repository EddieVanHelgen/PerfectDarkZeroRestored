// Input-to-screen latency of the game's reactions (pdz_latency_log). See latency_log.h for the chain.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/frame_pacing.h>

#include "latency_log.h"
#include "shadow_record.h"

REXCVAR_DEFINE_BOOL(pdz_latency_log, false, "Render",
                    "Input latency: for each trigger pull or right-stick push on pad 0, time the pad read that first "
                    "saw it, the first frame whose view or gun reacts, its native submit, its present and its refresh "
                    "on screen ('pdz_latency:' and 'frame_pacing: reaction' lines, a summary every 10 s). Turns "
                    "frame_pacing_log on.");

namespace pdz::latency
{
  namespace
  {
    using Clock = std::chrono::steady_clock;

    // Player fields (TU3): view yaw / pitch in degrees (vtable +24 / +28 return them), the input block and its pad.
    constexpr uint32_t kPlayerViewYaw = 5744;
    constexpr uint32_t kPlayerViewPitch = 5748;
    constexpr uint32_t kPlayerInput = 0x19A0;
    constexpr uint32_t kInputPad = 0x690;
    constexpr uint32_t kWeaponOwner = 0x5C;
    // The hands (player +0xFEC left, +0xFF0 right; button_log.cpp): the gun state machine (+1280, its action at +4:
    // 2 = shot starting, 3 = automatic fire) and the clip (+1316, 16 bits).
    constexpr uint32_t kPlayerHands[2] = {0xFEC, 0xFF0};
    constexpr uint32_t kHandGun = 1280;
    constexpr uint32_t kGunAction = 4;
    constexpr uint32_t kHandClip = 1316;

    // Change thresholds: the trigger past half from below a quarter; the right stick past half from below a quarter.
    constexpr uint8_t kTriggerOn = 128, kTriggerOff = 64;
    constexpr int kStickOn = 16384, kStickOff = 8192;
    // A turn reacts when the view moves this much more per frame than it did in the frame before the change.
    constexpr float kTurnEpsilonDeg = 0.02f;
    constexpr auto kGiveUp = std::chrono::seconds(1);
    constexpr auto kSummaryPeriod = std::chrono::seconds(10);

    enum class Kind : uint32_t
    {
      kFire,
      kTurn,
    };
    const char *Name(Kind k) { return k == Kind::kFire ? "fire" : "turn"; }

    struct Hands
    {
      int32_t action[2] = {-1, -1};
      int32_t clip[2] = {-1, -1};
    };

    struct Pending
    {
      uint32_t id = 0;
      Kind kind = Kind::kFire;
      Clock::time_point change;
      uint64_t swap = 0;       // the Swap count when the change was read
      uint64_t shots = 0;      // the player's shots then
      Hands hands_before;      // the guns then
      float turn_before = 0.0f; // the view's per-frame move then (degrees)
    };

    struct Reacted
    {
      uint32_t id = 0;
      Kind kind = Kind::kFire;
      uint64_t swap = 0;
      Clock::time_point change;
    };

    struct State
    {
      std::mutex mutex;
      bool armed = false;
      uint8_t last_rt = 0;
      int last_rx = 0, last_ry = 0;
      bool have_pad = false;
      uint32_t player = 0;
      uint64_t shots = 0;
      uint64_t swaps = 0;
      bool have_view = false;
      float yaw = 0.0f, pitch = 0.0f, turn = 0.0f; // the last Swap's view and its move since the one before
      uint32_t next_id = 0;
      std::vector<Pending> pending;
      std::vector<Reacted> reacted; // waiting for their native submit
      // Statistics: input change -> reacting frame's Swap (ms), frames, and -> native submit (ms), per kind.
      std::vector<double> to_swap[2], to_submit[2], frames[2];
      uint32_t given_up[2] = {};
      Clock::time_point last_summary;
    };
    State g;

    Hands ReadHands(const uint8_t *base, uint32_t player)
    {
      Hands h;
      for (uint32_t i = 0; i < 2 && player; ++i)
      {
        const uint32_t hand = pdz::shadow::Load32(base, player + kPlayerHands[i]);
        if (!pdz::shadow::IsGuestPointer(hand))
        {
          continue;
        }
        const uint32_t gun = pdz::shadow::Load32(base, hand + kHandGun);
        h.action[i] = pdz::shadow::IsGuestPointer(gun) ? int32_t(pdz::shadow::Load32(base, gun + kGunAction)) : -1;
        const uint8_t *c = pdz::shadow::GuestPtr(base, hand + kHandClip);
        h.clip[i] = int32_t((uint32_t(c[0]) << 8) | c[1]);
      }
      return h;
    }

    // A gun firing: an action of 2 / 3 in either hand.
    bool Firing(const Hands &h)
    {
      return h.action[0] == 2 || h.action[0] == 3 || h.action[1] == 2 || h.action[1] == 3;
    }

    double Ms(Clock::time_point from, Clock::time_point to)
    {
      return std::chrono::duration<double, std::milli>(to - from).count();
    }

    float LoadFloat(const uint8_t *base, uint32_t address)
    {
      return pdz::shadow::BitsToFloat(pdz::shadow::Load32(base, address));
    }

    uint16_t Load16(const uint8_t *base, uint32_t address)
    {
      const uint8_t *p = pdz::shadow::GuestPtr(base, address);
      return uint16_t((uint16_t(p[0]) << 8) | p[1]);
    }

    float WrapDeg(float d)
    {
      while (d > 180.0f)
      {
        d -= 360.0f;
      }
      while (d < -180.0f)
      {
        d += 360.0f;
      }
      return d;
    }

    // pdz_latency_log needs the SDK's frame_pacing trace for the present / screen half: switched on with it, once.
    void Arm()
    {
      if (g.armed)
      {
        return;
      }
      g.armed = true;
      g.last_summary = Clock::now();
      if (rex::cvar::GetFlagByName("frame_pacing_log") != "true")
      {
        rex::cvar::SetFlagByName("frame_pacing_log", "true");
      }
      REXLOG_INFO("pdz_latency: on: trigger pulls and right-stick pushes on pad 0 are timed to the first frame whose "
                  "gun or view reacts, its native submit, present and screen (frame_pacing_log {})",
                  rex::cvar::GetFlagByName("frame_pacing_log"));
    }

    std::string Spread(std::vector<double> v, const char *unit)
    {
      if (v.empty())
      {
        return "n/a";
      }
      std::sort(v.begin(), v.end());
      const auto at = [&](size_t percent) { return v[std::min(v.size() - 1, v.size() * percent / 100)]; };
      return fmt::format("p50 {:.1f} p95 {:.1f} max {:.1f}{} (n {})", at(50), at(95), v.back(), unit, v.size());
    }

    std::string SummaryLocked()
    {
      std::string text;
      for (uint32_t k = 0; k < 2; ++k)
      {
        if (g.to_swap[k].empty() && !g.given_up[k])
        {
          continue;
        }
        text += fmt::format("{}{}: input change -> reacting frame's Swap {}, frames {}, -> native submit {}, no "
                            "reaction {}",
                            text.empty() ? "" : "; ", Name(Kind(k)), Spread(g.to_swap[k], " ms"),
                            Spread(g.frames[k], ""), Spread(g.to_submit[k], " ms"), g.given_up[k]);
      }
      return text;
    }
  } // namespace

  bool Enabled() { return REXCVAR_GET(pdz_latency_log); }

  void OnPadRead(const uint8_t *base, uint32_t state)
  {
    if (!Enabled())
    {
      return;
    }
    const Clock::time_point now = Clock::now();
    // XINPUT_STATE: +0 packet, +4 buttons, +6 LT, +7 RT, +8 LX, +10 LY, +12 RX, +14 RY.
    const uint8_t rt = pdz::shadow::GuestPtr(base, state + 7)[0];
    const int rx = int16_t(Load16(base, state + 12));
    const int ry = int16_t(Load16(base, state + 14));
    std::lock_guard<std::mutex> lock(g.mutex);
    Arm();
    if (g.have_pad)
    {
      const auto start = [&](Kind kind) {
        for (const Pending &p : g.pending)
        {
          if (p.kind == kind)
          {
            return; // one at a time per kind
          }
        }
        Pending p;
        p.id = ++g.next_id;
        p.kind = kind;
        p.change = now;
        p.swap = g.swaps;
        p.shots = g.shots;
        p.turn_before = g.turn;
        p.hands_before = ReadHands(base, g.player);
        g.pending.push_back(p);
      };
      if (rt >= kTriggerOn && g.last_rt < kTriggerOff)
      {
        start(Kind::kFire);
      }
      const int was = std::max(std::abs(g.last_rx), std::abs(g.last_ry));
      if (std::max(std::abs(rx), std::abs(ry)) >= kStickOn && was < kStickOff)
      {
        start(Kind::kTurn);
      }
    }
    g.have_pad = true;
    g.last_rt = rt;
    g.last_rx = rx;
    g.last_ry = ry;
  }

  void OnPlayerLook(const uint8_t *base, uint32_t player)
  {
    if (!Enabled() || player == g.player)
    {
      return;
    }
    const uint32_t input = pdz::shadow::Load32(base, player + kPlayerInput);
    if (!pdz::shadow::IsGuestPointer(input) || int32_t(pdz::shadow::Load32(base, input + kInputPad)) != 0)
    {
      return;
    }
    std::lock_guard<std::mutex> lock(g.mutex);
    g.player = player;
    g.have_view = false;
    REXLOG_INFO("pdz_latency: player {:08X} (pad 0)", player);
  }

  void OnShot(const uint8_t *base, uint32_t shooter)
  {
    if (!Enabled() || !g.player)
    {
      return;
    }
    const bool his = shooter == g.player ||
                     (pdz::shadow::IsGuestPointer(shooter) && pdz::shadow::Load32(base, shooter + kWeaponOwner) == g.player);
    if (his)
    {
      std::lock_guard<std::mutex> lock(g.mutex);
      ++g.shots;
    }
  }

  void OnSwap(const uint8_t *base)
  {
    // Counted on or off, so the count stays the native present's (native_present.cpp counts every Swap too).
    static uint64_t s_swaps = 0;
    ++s_swaps;
    if (!Enabled())
    {
      return;
    }
    const Clock::time_point now = Clock::now();
    std::vector<std::string> lines;
    {
      std::lock_guard<std::mutex> lock(g.mutex);
      Arm();
      g.swaps = s_swaps;
      if (g.player)
      {
        const float yaw = LoadFloat(base, g.player + kPlayerViewYaw);
        const float pitch = LoadFloat(base, g.player + kPlayerViewPitch);
        g.turn = g.have_view ? std::max(std::fabs(WrapDeg(yaw - g.yaw)), std::fabs(pitch - g.pitch)) : 0.0f;
        g.yaw = yaw;
        g.pitch = pitch;
        g.have_view = true;
      }
      const Hands hands = g.pending.empty() ? Hands{} : ReadHands(base, g.player);
      const auto fired = [&](const Pending &p) {
        bool clip_down = false;
        for (uint32_t i = 0; i < 2; ++i)
        {
          clip_down |= hands.clip[i] >= 0 && p.hands_before.clip[i] >= 0 && hands.clip[i] < p.hands_before.clip[i];
        }
        return g.shots > p.shots || clip_down || (Firing(hands) && !Firing(p.hands_before));
      };
      for (auto it = g.pending.begin(); it != g.pending.end();)
      {
        const Pending &p = *it;
        const uint32_t k = uint32_t(p.kind);
        const bool reacted = p.swap < g.swaps &&
                             (p.kind == Kind::kFire ? fired(p) : g.turn > p.turn_before + kTurnEpsilonDeg);
        if (reacted)
        {
          // The newest VdSwap (this Swap's, just done by the guest's Swap) is the reacting frame.
          rex::ui::frame_pacing::MarkReaction(p.id, p.change);
          const double ms = Ms(p.change, now);
          const uint64_t frames = g.swaps - p.swap;
          g.to_swap[k].push_back(ms);
          g.frames[k].push_back(double(frames));
          g.reacted.push_back({p.id, p.kind, g.swaps, p.change});
          if (g.reacted.size() > 8)
          {
            g.reacted.erase(g.reacted.begin());
          }
          lines.push_back(fmt::format("reaction #{} {}: input change -> the reacting frame's Swap {:.2f} ms ({} "
                                      "frame{} later, swap {}){}",
                                      p.id, Name(p.kind), ms, frames, frames == 1 ? "" : "s", g.swaps,
                                      p.kind == Kind::kTurn ? fmt::format(", view moved {:.3f} deg", g.turn) : ""));
          it = g.pending.erase(it);
        }
        else if (now - p.change > kGiveUp)
        {
          ++g.given_up[k];
          lines.push_back(fmt::format(
              "#{} {}: no reaction within 1 s ({}){}", p.id, Name(p.kind),
              p.kind == Kind::kFire
                  ? fmt::format("no shot: gun actions left {} right {}, clips {} / {}", hands.action[0],
                                hands.action[1], hands.clip[0], hands.clip[1])
                  : std::string("the view didn't move"),
              g.player ? "" : "; no player seen yet"));
          it = g.pending.erase(it);
        }
        else
        {
          ++it;
        }
      }
      if (now - g.last_summary >= kSummaryPeriod)
      {
        g.last_summary = now;
        std::string text = SummaryLocked();
        if (!text.empty())
        {
          lines.push_back("summary: " + text);
        }
      }
    }
    for (const std::string &line : lines)
    {
      REXLOG_INFO("pdz_latency: {}", line);
    }
  }

  void OnNativeSubmit(uint64_t swap)
  {
    if (!Enabled())
    {
      return;
    }
    const Clock::time_point now = Clock::now();
    std::string line;
    {
      std::lock_guard<std::mutex> lock(g.mutex);
      for (auto it = g.reacted.begin(); it != g.reacted.end(); ++it)
      {
        if (it->swap == swap)
        {
          const double ms = Ms(it->change, now);
          g.to_submit[uint32_t(it->kind)].push_back(ms);
          line = fmt::format("reaction #{}: input change -> native submit {:.2f} ms", it->id, ms);
          g.reacted.erase(it);
          break;
        }
      }
    }
    if (!line.empty())
    {
      REXLOG_INFO("pdz_latency: {}", line);
    }
  }

  void LogSummary(const char *why)
  {
    if (!Enabled())
    {
      return;
    }
    std::lock_guard<std::mutex> lock(g.mutex);
    const std::string text = SummaryLocked();
    REXLOG_INFO("pdz_latency: SUMMARY ({}): {}", why, text.empty() ? "no input change seen" : text);
  }
} // namespace pdz::latency
