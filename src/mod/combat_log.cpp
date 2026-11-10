// Combat aiming diagnostic (pdz_combat_log). See README.md next to this file.
//
// Read-only. While the player has a target (the auto-aim's selected target,
// or else the nearest enemy that is targeting them within 40 m and in view),
// it samples at 20 Hz: distance, the turn rate needed to stay on the target,
// the player's actual turn rate and the most the look model allows, stick,
// fast look, zoom, aim error, the rotational assist and shot bending, and
// what the enemy is doing. Every hitscan shot is logged with its error and
// whether it hit. Per-sample rows go to logs/combat_<time>.csv; a summary by
// distance band is logged every minute and kept in logs/combat_summary.txt.
//
// Sources:
// - enemies: the AI character update sub_8251DB70 (character class vtable
//   820BF290 +4; subclasses such as 820C0A18 call it; position from
//   the game's own getter sub_8214C7E8 (mover position, else +0x30);
//   its target is [+0x3168 if +0x316C else +0x3150] +4; in a cover task while
//   +0x187C & 0x8000);
// - aim: aim_assist.cpp's auto-aim hook (unbent aim direction, target);
// - turn limits: the look records (82DA3760, [max, default, min] x [pd,
//   halo]) blended by the sensitivity like the look models do (horizontal
//   x1.2 with the Fast Look option off, both x1.75 with input +0x4F2): yaw =
//   6 x field 0 (field 11 in fast look), pitch = 6 x field 1 (vertical
//   sensitivity), x 60 for deg/s, x the look state's turn scales (+0xC /
//   +0x10), x (1 - zoom) while holding an item (+0xFF4 > 0), x aim
//   sensitivity (+0x4DC) x field 4 while zoomed or in cover;
// - shots: sub_82238938 (hitscan) when its weapon's owner (+0x5C) is the
//   player; hits: the AI character damage handler sub_8251EBB0 (vtable
//   +0x32C, also reached from subclasses) called during that shot.
// - multiplayer bots and other players use the player class (vtable
//   820C93A0): its update sub_82598420 (+4) and damage handler sub_8259B738
//   (+0x32C, same arguments) are hooked the same way; every other player
//   counts as hostile.
#include "combat_log.h"
#include "aim_assist.h" // pdz_autoaim_log: a line per player shot

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <iterator>
#include <string>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "../game_cvars.h"
#include "shot_log.h"
#include "render/latency_log.h"

namespace look_tuning
{
  bool FastLookSetting();
}

REX_EXTERN(__imp__sub_8251DB70);
REX_EXTERN(__imp__sub_82238938);
REX_EXTERN(__imp__sub_8251EBB0);
REX_EXTERN(__imp__sub_82598420);
REX_EXTERN(__imp__sub_8259B738);
REX_EXTERN(sub_8214C7E8);
REX_EXTERN(sub_8253C108);

namespace
{
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
    return address >= 0x40000000u && address < 0xFFFF0000u;
  }

  bool Enabled()
  {
    return REXCVAR_GET(pdz_combat_log);
  }

  using Clock = std::chrono::steady_clock;
  constexpr double kPi = 3.14159265358979;
  constexpr double kRadToDeg = 180.0 / kPi;

  // Player fields.
  constexpr uint32_t kPlayerLookState = 0xF50;
  constexpr uint32_t kPlayerInput = 0x19A0;
  constexpr uint32_t kPlayerZoom = 0x1A9C;
  constexpr uint32_t kPlayerHeldItem = 0xFF4;
  constexpr uint32_t kPlayerCoverState = 0x1CA4;
  constexpr uint32_t kInputAimSensitivity = 0x4DC;
  // Look state fields.
  constexpr uint32_t kStateFastLook = 0x0;
  constexpr uint32_t kStatePitchVelocity = 0x4;
  constexpr uint32_t kStateYawVelocity = 0x8;
  constexpr uint32_t kStateYawScale = 0xC;
  constexpr uint32_t kStatePitchScale = 0x10;
  constexpr uint32_t kStateAssistYawRate = 0x30;
  constexpr uint32_t kStateAssistWeight = 0x38;
  // Input fields.
  constexpr uint32_t kInputStyle = 0x484; // 1 = "Halo" model, else "PD"
  constexpr uint32_t kInputLookX = 0x4A4;
  constexpr uint32_t kInputLookY = 0x4A8;
  constexpr uint32_t kInputSensitivity = 0x4D4; // horizontal, vertical
  constexpr uint32_t kInputSensitivityBoost = 0x4F2;
  // Look records.
  constexpr uint32_t kAimParamTable = 0x82DA3760;
  constexpr uint32_t kFieldYawRate = 0;
  constexpr uint32_t kFieldPitchRate = 1;
  constexpr uint32_t kFieldAimMode = 4;
  constexpr uint32_t kFieldFastLookRate = 11;
  constexpr double kFastLookOffBonus = 1.2;
  constexpr float kZoomedThreshold = 0.001f;
  constexpr double kTurnBase = 6.0; // movementparams +8
  constexpr double kRateToDegPerSecond = 60.0;
  // AI character fields.
  constexpr uint32_t kCharPosition = 0x30;
  constexpr uint32_t kCharFlags = 0x187C;
  constexpr uint32_t kCharInCoverTask = 0x8000;
  constexpr uint32_t kCharTargetA = 0x3150;
  constexpr uint32_t kCharTargetB = 0x3168;
  constexpr uint32_t kCharTargetBValid = 0x316C;
  // Weapon owner.
  constexpr uint32_t kWeaponOwner = 0x5C;

  constexpr double kRange = 4000.0;     // cm
  constexpr double kViewHalfAngle = 37.0; // deg, half of ~73 across
  constexpr auto kSampleInterval = std::chrono::milliseconds(50);
  constexpr auto kEnemyTimeout = std::chrono::milliseconds(500);
  constexpr double kLostDeg = 5.0;
  constexpr double kReacquiredDeg = 2.0;
  constexpr double kBodyHalfWidthCm = 25.0;

  struct Enemy
  {
    float position[3] = {};
    float velocity[3] = {}; // cm/s
    Clock::time_point seen;
    bool in_cover = false;
    bool targets_player = false;
  };
  std::unordered_map<uint32_t, Enemy> g_enemies;
  // Enemy list health, in the summaries: AI updates seen, and samples whose
  // target was / wasn't in the list.
  uint64_t g_enemy_updates = 0;
  uint64_t g_target_known = 0;
  uint64_t g_target_unknown = 0;
  uint64_t g_damage_events = 0;
  uint64_t g_damage_in_player_shot = 0;
  uint64_t g_damage_by_player = 0;

  uint32_t g_player = 0;

  // Distance bands, metres: 0-3, 3-6, 6-10, 10-20, 20-40.
  constexpr std::array<double, 5> kBandEnd = {3.0, 6.0, 10.0, 20.0, 40.0};
  const char *kBandName[] = {"0-3 m", "3-6 m", "6-10 m", "10-20 m", "20-40 m"};
  int BandOf(double metres)
  {
    for (size_t i = 0; i < kBandEnd.size(); ++i)
    {
      if (metres < kBandEnd[i])
      {
        return int(i);
      }
    }
    return int(kBandEnd.size()) - 1;
  }

  struct Band
  {
    double seconds = 0.0;
    double over_normal_s = 0.0; // needed turn above the normal maximum
    double over_fast_s = 0.0;   // above the fast-look maximum too
    double fast_look_s = 0.0;
    std::vector<float> errors;  // deg
    int shots = 0;
    int hits = 0;
    int bent_shots = 0;
    int bend_needed = 0; // bent shots whose unbent aim was off the body
    std::vector<float> reacquire_ms;
  };
  struct Stats
  {
    std::array<Band, 5> bands;
    Clock::time_point since;
  };
  Stats g_minute, g_run;

  struct Track
  {
    uint32_t target = 0;
    Clock::time_point last;
    double los_yaw = 0.0, los_pitch = 0.0, aim_yaw = 0.0, aim_pitch = 0.0;
    bool lost = false;
    Clock::time_point lost_at;
    int lost_band = 0;
    // Latest sample, used by shots.
    double distance = 0.0;
    double error_deg = 0.0;
    bool bending = false;
  } g_track;

  FILE *g_csv = nullptr;
  Clock::time_point g_last_summary;
  Clock::time_point g_start;

  void OpenCsv()
  {
    if (g_csv)
    {
      return;
    }
    const std::time_t now = std::time(nullptr);
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", std::localtime(&now));
    const std::string path = fmt::format("logs/combat_{}.csv", stamp);
    g_csv = std::fopen(path.c_str(), "w");
    if (g_csv)
    {
      std::fputs("t_s,kind,target,selected,distance_m,band,need_yaw_dps,need_pitch_dps,actual_yaw_dps,"
                  "actual_pitch_dps,max_yaw_dps,max_fast_yaw_dps,max_pitch_dps,stick_x,stick_y,fast_look,"
                  "fast_ramp,zoom,error_deg,error_cm,assist_weight,assist_rate_dps,bending,enemy_speed_mps,"
                  "enemy_lateral_mps,enemy_in_cover,model,sens_x,sens_y,hit,bend_needed,gun\n",
                  g_csv);
      REXLOG_INFO("pdz_combat: writing {}", path);
    }
  }

  double Seconds(Clock::time_point t)
  {
    return std::chrono::duration<double>(t - g_start).count();
  }

  void Angles(const float v[3], double &yaw, double &pitch)
  {
    yaw = std::atan2(double(v[0]), double(v[2])) * kRadToDeg;
    pitch = std::atan2(double(v[1]), std::sqrt(double(v[0]) * v[0] + double(v[2]) * v[2])) * kRadToDeg;
  }

  double WrapDeg(double d)
  {
    while (d > 180.0)
    {
      d -= 360.0;
    }
    while (d < -180.0)
    {
      d += 360.0;
    }
    return d;
  }

  // The look records blended by sensitivity, as the look models do.
  double Blend(const uint8_t *base, bool halo, uint32_t field, double s)
  {
    const auto value = [&](uint32_t level)
    {
      const uint32_t record = LoadBe32(base, kAimParamTable + (level * 2 + (halo ? 1 : 0)) * 4);
      return IsGuestPointer(record) ? double(LoadBeFloat(base, record + field * 4)) : 0.0;
    };
    const double max = value(0), def = value(1), min = value(2);
    if (s > 0.5)
    {
      return def + (max - def) * (s - 0.5) * 2.0;
    }
    return min + (def - min) * s * 2.0;
  }

  struct Limits
  {
    double yaw = 0.0, fast_yaw = 0.0, pitch = 0.0;
    bool halo = false;
    float sens_x = 0.0f, sens_y = 0.0f;
  };

  Limits MaxRates(const uint8_t *base, uint32_t player)
  {
    Limits limits;
    const uint32_t input = LoadBe32(base, player + kPlayerInput);
    if (!IsGuestPointer(input))
    {
      return limits;
    }
    limits.halo = LoadBe32(base, input + kInputStyle) == 1;
    limits.sens_x = LoadBeFloat(base, input + kInputSensitivity);
    limits.sens_y = LoadBeFloat(base, input + kInputSensitivity + 4);
    const double boost = base[input + kInputSensitivityBoost] ? 1.75 : 1.0;
    const double sx = limits.sens_x * boost * (look_tuning::FastLookSetting() ? 1.0 : kFastLookOffBonus);
    const double sy = limits.sens_y * boost;
    const float zoom_amount = LoadBeFloat(base, player + kPlayerZoom);
    double scale = kTurnBase * kRateToDegPerSecond;
    if (int32_t(LoadBe32(base, player + kPlayerHeldItem)) > 0)
    {
      scale *= 1.0 - std::clamp(double(zoom_amount), 0.0, 1.0);
    }
    if (zoom_amount > kZoomedThreshold || LoadBe32(base, player + kPlayerCoverState) != 0)
    {
      scale *= LoadBeFloat(base, input + kInputAimSensitivity) * Blend(base, limits.halo, kFieldAimMode, sx);
    }
    const uint32_t state = player + kPlayerLookState;
    const double yaw_scale = scale * LoadBeFloat(base, state + kStateYawScale);
    limits.yaw = Blend(base, limits.halo, kFieldYawRate, sx) * yaw_scale;
    limits.fast_yaw = Blend(base, limits.halo, kFieldFastLookRate, sx) * yaw_scale;
    limits.pitch = Blend(base, limits.halo, kFieldPitchRate, sy) * scale * LoadBeFloat(base, state + kStatePitchScale);
    return limits;
  }

  uint32_t TargetOf(const uint8_t *base, uint32_t character)
  {
    const uint32_t slot = LoadBe32(base, character + kCharTargetBValid) ? kCharTargetB : kCharTargetA;
    return LoadBe32(base, character + slot + 4);
  }

  double Percentile(std::vector<float> values, double p)
  {
    if (values.empty())
    {
      return 0.0;
    }
    std::sort(values.begin(), values.end());
    return values[std::min(values.size() - 1, size_t(p * double(values.size())))];
  }

  std::string Summary(const Stats &stats, const char *title, Clock::time_point now)
  {
    std::string text = fmt::format("pdz_combat: {} ({:.0f} s):", title,
                                   std::chrono::duration<double>(now - stats.since).count());
    for (size_t i = 0; i < stats.bands.size(); ++i)
    {
      const Band &b = stats.bands[i];
      if (b.seconds <= 0.0 && b.shots == 0)
      {
        continue;
      }
      double mean = 0.0;
      for (float e : b.errors)
      {
        mean += e;
      }
      mean = b.errors.empty() ? 0.0 : mean / double(b.errors.size());
      double reacquire = 0.0;
      for (float r : b.reacquire_ms)
      {
        reacquire += r;
      }
      reacquire = b.reacquire_ms.empty() ? 0.0 : reacquire / double(b.reacquire_ms.size());
      text += fmt::format(
          "\n  {:>7}: {:.0f} s | needed turn > normal max {:.0f}% of the time, > fast-look max {:.0f}% | aim "
          "error mean {:.1f} deg, p90 {:.1f} | shots {} hit {} ({:.0f}%), bent {} (bend needed {}) | fast look "
          "{:.0f}% | reacquire {} x, mean {:.0f} ms",
          kBandName[i], b.seconds, b.seconds > 0 ? 100.0 * b.over_normal_s / b.seconds : 0.0,
          b.seconds > 0 ? 100.0 * b.over_fast_s / b.seconds : 0.0, mean, Percentile(b.errors, 0.9), b.shots,
          b.hits, b.shots ? 100.0 * b.hits / b.shots : 0.0, b.bent_shots, b.bend_needed,
          b.seconds > 0 ? 100.0 * b.fast_look_s / b.seconds : 0.0, b.reacquire_ms.size(), reacquire);
    }
    return text;
  }

  void MaybeSummarise(Clock::time_point now)
  {
    if (now - g_last_summary < std::chrono::minutes(1))
    {
      return;
    }
    g_last_summary = now;
    REXLOG_INFO("pdz_combat: enemy list {} characters, {} AI updates; selected target in the list {} / {} samples; "
                "damage events {} ({} during your shots, {} with you as attacker)",
                g_enemies.size(), g_enemy_updates, g_target_known, g_target_known + g_target_unknown, g_damage_events,
                g_damage_in_player_shot, g_damage_by_player);
    REXLOG_INFO("{}", Summary(g_minute, "last minute", now));
    const std::string run = Summary(g_run, "whole run", now);
    REXLOG_INFO("{}", run);
    if (FILE *f = std::fopen("logs/combat_summary.txt", "w"))
    {
      std::fputs(run.c_str(), f);
      std::fputs("\n", f);
      std::fclose(f);
    }
    if (g_csv)
    {
      std::fflush(g_csv);
    }
    g_minute = Stats{};
    g_minute.since = now;
  }

  // Gun type names: GunType_* string table at 82D21D28, indexed by type.
  constexpr uint32_t kGunTypeNames = 0x82D21D28;
  constexpr uint32_t kGunTypeCount = 0x40;

  std::string GunName(const uint8_t *base, uint32_t type)
  {
    if (type >= kGunTypeCount)
    {
      return fmt::format("type{}", type);
    }
    const uint32_t text = LoadBe32(base, kGunTypeNames + type * 4);
    if (text < 0x82000000u || text >= 0x82140000u)
    {
      return fmt::format("type{}", type);
    }
    std::string name(reinterpret_cast<const char *>(base + text));
    const std::string prefix = "GunType_";
    return name.compare(0, prefix.size(), prefix) == 0 ? name.substr(prefix.size()) : name;
  }

  // The gun in each hand (sub_8253C108(player, hand) = gun type), e.g.
  // "MP5" or "Magnum+Magnum" when dual wielding.
  std::string HeldGuns(PPCContext &ctx, uint8_t *base, uint32_t player)
  {
    uint32_t types[2] = {};
    for (uint32_t hand = 0; hand < 2; ++hand)
    {
      rex::CallFrame frame(ctx);
      frame.ctx.r3.u64 = player;
      frame.ctx.r4.u64 = hand;
      sub_8253C108(frame.ctx, base);
      types[hand] = frame.ctx.r3.u32;
    }
    if (types[0] && types[1])
    {
      return GunName(base, types[1]) + "+" + GunName(base, types[0]);
    }
    return GunName(base, types[1] ? types[1] : types[0]);
  }

  // First few distinct shooters seen, to check in the log that the
  // player's shots are recognised.
  std::vector<uint32_t> g_seen_shooters;

  // Shots in progress (sub_82238938 by the player's weapon).
  thread_local bool t_player_shot = false;
  thread_local int t_shot_hits = 0;
} // namespace

namespace combat_log
{
  void OnAimUpdate(uint8_t *base, const AimFrame &frame)
  {
    if (!Enabled() || !frame.player || !frame.aim_valid)
    {
      return;
    }
    const auto now = Clock::now();
    if (g_start == Clock::time_point{})
    {
      g_start = now;
      g_last_summary = now;
      g_minute.since = now;
      g_run.since = now;
    }
    g_player = frame.player;
    if (now - g_track.last < kSampleInterval)
    {
      return;
    }
    OpenCsv();

    // The target: the selected one, else the nearest enemy after the player
    // within range and in view.
    uint32_t target = frame.target;
    float point[3] = {frame.target_point[0], frame.target_point[1], frame.target_point[2]};
    const Enemy *enemy = nullptr;
    if (target)
    {
      auto it = g_enemies.find(target);
      enemy = it != g_enemies.end() ? &it->second : nullptr;
      ++(enemy ? g_target_known : g_target_unknown);
    }
    else
    {
      double best = kRange;
      for (auto &[character, e] : g_enemies)
      {
        if (now - e.seen > kEnemyTimeout || !e.targets_player)
        {
          continue;
        }
        float los[3];
        double length2 = 0.0;
        for (int i = 0; i < 3; ++i)
        {
          los[i] = e.position[i] - frame.origin[i];
          length2 += double(los[i]) * los[i];
        }
        const double length = std::sqrt(length2);
        if (length < 1.0 || length >= best)
        {
          continue;
        }
        const double cosine =
            (los[0] * frame.aim_dir[0] + los[1] * frame.aim_dir[1] + los[2] * frame.aim_dir[2]) / length;
        if (std::acos(std::clamp(cosine, -1.0, 1.0)) * kRadToDeg > kViewHalfAngle)
        {
          continue;
        }
        best = length;
        target = character;
        enemy = &e;
        std::memcpy(point, e.position, sizeof(point));
      }
    }
    const double dt = std::chrono::duration<double>(now - g_track.last).count();
    g_track.last = now;
    if (!target)
    {
      g_track.target = 0;
      return;
    }

    float los[3];
    double length2 = 0.0;
    for (int i = 0; i < 3; ++i)
    {
      los[i] = point[i] - frame.origin[i];
      length2 += double(los[i]) * los[i];
    }
    const double distance_cm = std::sqrt(length2);
    if (distance_cm < 1.0 || distance_cm > kRange)
    {
      g_track.target = 0;
      return;
    }
    const double metres = distance_cm / 100.0;
    const int band = BandOf(metres);
    double los_yaw, los_pitch, aim_yaw, aim_pitch;
    Angles(los, los_yaw, los_pitch);
    Angles(frame.aim_dir, aim_yaw, aim_pitch);
    const double cosine =
        (los[0] * frame.aim_dir[0] + los[1] * frame.aim_dir[1] + los[2] * frame.aim_dir[2]) / distance_cm;
    const double error_deg = std::acos(std::clamp(cosine, -1.0, 1.0)) * kRadToDeg;
    const double error_cm = std::tan(std::min(error_deg, 89.0) / kRadToDeg) * distance_cm;

    const bool continuing = g_track.target == target && dt > 0.0 && dt < 0.25;
    double need_yaw = 0.0, need_pitch = 0.0, actual_yaw = 0.0, actual_pitch = 0.0;
    if (continuing)
    {
      need_yaw = WrapDeg(los_yaw - g_track.los_yaw) / dt;
      need_pitch = (los_pitch - g_track.los_pitch) / dt;
      actual_yaw = WrapDeg(aim_yaw - g_track.aim_yaw) / dt;
      actual_pitch = (aim_pitch - g_track.aim_pitch) / dt;
    }
    else
    {
      g_track.lost = false;
    }
    g_track.target = target;
    g_track.los_yaw = los_yaw;
    g_track.los_pitch = los_pitch;
    g_track.aim_yaw = aim_yaw;
    g_track.aim_pitch = aim_pitch;
    g_track.distance = metres;
    g_track.error_deg = error_deg;
    g_track.bending = frame.bending;

    // Losing and reacquiring the target.
    if (!g_track.lost && error_deg > kLostDeg)
    {
      g_track.lost = true;
      g_track.lost_at = now;
      g_track.lost_band = band;
    }
    else if (g_track.lost && error_deg < kReacquiredDeg)
    {
      g_track.lost = false;
      const float ms = std::chrono::duration<float, std::milli>(now - g_track.lost_at).count();
      g_minute.bands[g_track.lost_band].reacquire_ms.push_back(ms);
      g_run.bands[g_track.lost_band].reacquire_ms.push_back(ms);
    }

    const uint32_t player = frame.player;
    const uint32_t state = player + kPlayerLookState;
    const Limits limits = MaxRates(base, player);
    const uint32_t input = LoadBe32(base, player + kPlayerInput);
    const float stick_x = IsGuestPointer(input) ? LoadBeFloat(base, input + kInputLookX) : 0.0f;
    const float stick_y = IsGuestPointer(input) ? LoadBeFloat(base, input + kInputLookY) : 0.0f;
    const bool fast_look = base[state + kStateFastLook] != 0;
    const double yaw_speed = std::fabs(LoadBeFloat(base, state + kStateYawVelocity)) * kRateToDegPerSecond;
    const double fast_ramp = limits.fast_yaw > 0.0 ? yaw_speed / limits.fast_yaw : 0.0;
    const float zoom = LoadBeFloat(base, player + kPlayerZoom);
    const float assist_weight = LoadBeFloat(base, state + kStateAssistWeight);
    const float assist_rate = LoadBeFloat(base, state + kStateAssistYawRate);

    double enemy_speed = 0.0, enemy_lateral = 0.0;
    bool enemy_cover = false;
    if (enemy)
    {
      const double v2 = double(enemy->velocity[0]) * enemy->velocity[0] + double(enemy->velocity[2]) * enemy->velocity[2];
      enemy_speed = std::sqrt(v2) / 100.0;
      // Across the line of sight (horizontal).
      const double lx = los[0], lz = los[2], ll = std::sqrt(lx * lx + lz * lz);
      if (ll > 1.0)
      {
        enemy_lateral = std::fabs(enemy->velocity[0] * (lz / ll) - enemy->velocity[2] * (lx / ll)) / 100.0;
      }
      enemy_cover = enemy->in_cover;
    }

    if (continuing)
    {
      for (Stats *stats : {&g_minute, &g_run})
      {
        Band &b = stats->bands[band];
        b.seconds += dt;
        const bool over_normal = std::fabs(need_yaw) > limits.yaw || std::fabs(need_pitch) > limits.pitch;
        const bool over_fast = std::fabs(need_yaw) > limits.fast_yaw || std::fabs(need_pitch) > limits.pitch;
        b.over_normal_s += over_normal ? dt : 0.0;
        b.over_fast_s += over_fast ? dt : 0.0;
        b.fast_look_s += fast_look ? dt : 0.0;
        b.errors.push_back(float(error_deg));
      }
    }
    if (g_csv)
    {
      std::fprintf(g_csv,
                   "%.3f,sample,%08X,%d,%.2f,%d,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.3f,%.3f,%d,%.2f,%.3f,%.2f,%.1f,"
                   "%.2f,%.1f,%d,%.2f,%.2f,%d,%s,%.3f,%.3f,,\n",
                   Seconds(now), target, frame.target == target ? 1 : 0, metres, band, need_yaw, need_pitch,
                   actual_yaw, actual_pitch, limits.yaw, limits.fast_yaw, limits.pitch, stick_x, stick_y,
                   fast_look ? 1 : 0, fast_ramp, zoom, error_deg, error_cm, assist_weight, assist_rate,
                   frame.bending ? 1 : 0, enemy_speed, enemy_lateral, enemy_cover ? 1 : 0,
                   limits.halo ? "Halo" : "PD", limits.sens_x, limits.sens_y);
    }
    MaybeSummarise(now);
  }
} // namespace combat_log

namespace
{
  // Adds / refreshes a character in the enemy list after its update.
  void TrackCharacter(PPCContext &ctx, uint8_t *base, uint32_t character, bool other_player);
} // namespace

// sub_8251DB70(character, ...): AI character update; keeps the enemy list.
REX_HOOK_RAW(sub_8251DB70)
{
  const uint32_t character = ctx.r3.u32;
  __imp__sub_8251DB70(ctx, base);
  if (Enabled() && IsGuestPointer(character))
  {
    TrackCharacter(ctx, base, character, false);
  }
}

// sub_82598420(player, ...): player-class update (you, other players, bots).
REX_HOOK_RAW(sub_82598420)
{
  const uint32_t character = ctx.r3.u32;
  __imp__sub_82598420(ctx, base);
  if (Enabled() && character && g_player && character != g_player)
  {
    TrackCharacter(ctx, base, character, true);
  }
}

namespace
{
void TrackCharacter(PPCContext &ctx, uint8_t *base, uint32_t character, bool other_player)
{
  const auto now = Clock::now();
  Enemy &e = g_enemies[character];
  ++g_enemy_updates;
  // The game's position getter: the mover's position, or +0x30 without one.
  uint32_t position_address = character + kCharPosition;
  {
    rex::CallFrame frame(ctx);
    frame.ctx.r3.u64 = character;
    sub_8214C7E8(frame.ctx, base);
    if (frame.ctx.r3.u32 != 0)
    {
      position_address = frame.ctx.r3.u32;
    }
  }
  float position[3];
  for (int i = 0; i < 3; ++i)
  {
    position[i] = LoadBeFloat(base, position_address + i * 4);
  }
  const double dt = std::chrono::duration<double>(now - e.seen).count();
  if (e.seen != Clock::time_point{} && dt > 0.005 && dt < 0.5)
  {
    for (int i = 0; i < 3; ++i)
    {
      // Smoothed velocity, cm/s.
      const float v = float((position[i] - e.position[i]) / dt);
      e.velocity[i] += (v - e.velocity[i]) * 0.5f;
    }
  }
  std::memcpy(e.position, position, sizeof(position));
  e.seen = now;
  e.in_cover = !other_player && (LoadBe32(base, character + kCharFlags) & kCharInCoverTask) != 0;
  e.targets_player = other_player || (g_player && TargetOf(base, character) == g_player);
  // Forget characters not updated for a while (dead or removed).
  if (g_enemies.size() > 256)
  {
    for (auto it = g_enemies.begin(); it != g_enemies.end();)
    {
      it = now - it->second.seen > std::chrono::seconds(5) ? g_enemies.erase(it) : std::next(it);
    }
  }
}

} // namespace

// sub_82238938(weapon, ...): one hitscan shot.
REX_HOOK_RAW(sub_82238938)
{
  const uint32_t weapon = ctx.r3.u32;
  pdz::latency::OnShot(base, weapon); // render/latency_log.h (pdz_latency_log)
  // The shooter is the character itself (the player lives below the 1 GB
  // mark, so compare before any pointer check); a weapon's owner is +0x5C.
  const bool player_shot = Enabled() && g_player &&
                           (weapon == g_player || (IsGuestPointer(weapon) && LoadBe32(base, weapon + kWeaponOwner) == g_player));
  if (Enabled() && g_seen_shooters.size() < 8 &&
      std::find(g_seen_shooters.begin(), g_seen_shooters.end(), weapon) == g_seen_shooters.end())
  {
    g_seen_shooters.push_back(weapon);
    REXLOG_INFO("pdz_combat: hitscan shot by {:08X} (owner {:08X}), player {:08X} -> {}", weapon,
                IsGuestPointer(weapon) ? LoadBe32(base, weapon + kWeaponOwner) : 0, g_player,
                player_shot ? "player's" : "not the player's");
  }
  aim_assist::OnShot(base, weapon);
  shot_log::BeforeShot(ctx, base);
  if (!player_shot)
  {
    __imp__sub_82238938(ctx, base);
    shot_log::AfterShot(ctx, base);
    return;
  }
  t_player_shot = true;
  t_shot_hits = 0;
  __imp__sub_82238938(ctx, base);
  t_player_shot = false;
  shot_log::AfterShot(ctx, base);

  const auto now = Clock::now();
  const bool hit = t_shot_hits > 0;
  const bool tracking = g_track.target && now - g_track.last < std::chrono::milliseconds(250);
  const int band = BandOf(g_track.distance);
  const double off_cm = std::tan(std::min(g_track.error_deg, 89.0) / kRadToDeg) * g_track.distance * 100.0;
  const bool bend_needed = tracking && g_track.bending && off_cm > kBodyHalfWidthCm;
  if (tracking)
  {
    for (Stats *stats : {&g_minute, &g_run})
    {
      Band &b = stats->bands[band];
      ++b.shots;
      b.hits += hit ? 1 : 0;
      b.bent_shots += g_track.bending ? 1 : 0;
      b.bend_needed += bend_needed ? 1 : 0;
    }
  }
  if (g_csv)
  {
    const std::string gun = HeldGuns(ctx, base, g_player);
    std::fprintf(g_csv, "%.3f,shot,%08X,,%.2f,%d,,,,,,,,,,,,,%.2f,%.1f,,,%d,,,,,,,%d,%d,%s\n", Seconds(now),
                 tracking ? g_track.target : 0, tracking ? g_track.distance : 0.0, tracking ? band : -1,
                 tracking ? g_track.error_deg : 0.0, tracking ? off_cm : 0.0, g_track.bending ? 1 : 0, hit ? 1 : 0,
                 bend_needed ? 1 : 0, gun.c_str());
  }
}

namespace
{
  void CountDamage(uint32_t victim, uint32_t attacker);
}

// sub_8259B738(player, attacker, f1 = damage, ...): player-class damage.
// During one of your shots, damage to another player (a bot) is a hit.
REX_HOOK_RAW(sub_8259B738)
{
  shot_log::OnDamage(ctx.r3.u32, ctx.f1.f64);
  if (Enabled() && ctx.r3.u32 != g_player)
  {
    CountDamage(ctx.r3.u32, ctx.r4.u32);
  }
  __imp__sub_8259B738(ctx, base);
}

namespace
{
void CountDamage(uint32_t victim, uint32_t attacker)
{
  ++g_damage_events;
  g_damage_in_player_shot += t_player_shot ? 1 : 0;
  g_damage_by_player += (g_player && attacker == g_player) ? 1 : 0;
  if (t_player_shot)
  {
    ++t_shot_hits;
  }
  if (g_csv)
  {
    // Every damage event, so damage that lands after the shot call can be
    // matched to shots offline (hit column = during a shot of yours,
    // bend_needed column = attacker is you).
    std::fprintf(g_csv, "%.3f,damage,%08X,,,,,,,,,,,,,,,,,,,,,,,,,,,%d,%d\n", Seconds(Clock::now()), victim,
                 t_player_shot ? 1 : 0, (g_player && attacker == g_player) ? 1 : 0);
  }
}
} // namespace

// sub_8251EBB0(character, attacker?, f1 = damage?, ...): AI character damage.
REX_HOOK_RAW(sub_8251EBB0)
{
  shot_log::OnDamage(ctx.r3.u32, ctx.f1.f64);
  if (Enabled())
  {
    CountDamage(ctx.r3.u32, ctx.r4.u32);
  }
  __imp__sub_8251EBB0(ctx, base);
}
