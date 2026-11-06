// Ragdoll tuning. See README.md next to this file.
//
// sub_825D8B30 builds a character's ragdoll from its asset (humans share
// aid_darkcharacter_ragdolls_jack; the spiderbot has its own): per body part
// it fills a Havok rigid body cinfo (sub_829B2218 defaults: linear damping 0
// at +0x94, angular damping 0.05 at +0x98), sets friction, restitution and
// mass from the asset, creates the body (sub_829B1A98) and then raises its
// inertia for stability (BUG_AUDIT RD2, deliberate). The bodies use Havok
// sphere motions, which keep one inverse inertia (motion +196): creating the
// body sets it to 1 / (largest principal moment of the computed tensor,
// sub_825D6E70); the builder reads it back (motion vtable +56, inverse),
// multiplies it by 0.1 and sets it again (sub_829B0790 -> motion vtable +52,
// setInertiaInvLocal: the smallest diagonal entry), so every axis gets 10 x
// the largest moment.
//
// The hooks below change only bodies created while the builder runs, so the
// rest of the game's physics is untouched. Ragdolls are built when a
// character is created: changes apply to characters spawned afterwards.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include <rex/hook.h>
#include <rex/logging.h>

#include "../game_cvars.h"

REX_EXTERN(__imp__sub_825D8B30);
REX_EXTERN(__imp__sub_829B1A98);
REX_EXTERN(__imp__sub_829B0790);
REX_EXTERN(__imp__sub_825DC560);
REX_EXTERN(__imp__sub_825DA950);
REX_EXTERN(__imp__sub_825D6E70);

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

  void StoreBeFloat(uint8_t *base, uint32_t address, float value)
  {
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    uint8_t *p = base + address;
    p[0] = uint8_t(bits >> 24);
    p[1] = uint8_t(bits >> 16);
    p[2] = uint8_t(bits >> 8);
    p[3] = uint8_t(bits);
  }

  // hkRigidBodyCinfo fields.
  constexpr uint32_t kCinfoLinearDamping = 0x94;
  constexpr uint32_t kCinfoAngularDamping = 0x98;

  // Inside the ragdoll builder (it may run on a loading thread).
  thread_local bool t_building_ragdoll = false;

  // Ragdoll controllers (pdz_ragdoll_log). sub_825DC560(controller, mode,
  // f1 = seconds) switches between animation (mode 0: weight 1 -> 0) and
  // ragdoll (modes 1-3: weight 0 -> 1); the per-frame update sub_825DA950
  // drives the bodies with the animation blended by (1 - weight) and, in
  // mode 1, makes the ragdoll fully physical once the weight passes 0.7.
  // Active controllers are linked from 82E3B6C4 through +0x2C.
  constexpr uint32_t kControllerList = 0x82E3B6C4;
  constexpr uint32_t kControllerNext = 0x2C;
  constexpr uint32_t kControllerMode = 0x3C;
  constexpr uint32_t kControllerWeight = 0x40;
  constexpr uint32_t kControllerRate = 0x48;
  constexpr uint32_t kControllerActive = 0x54;
  constexpr uint32_t kControllerPhysical = 0x55;
  constexpr uint32_t kControllerTimer = 0x58;

  struct ControllerLog
  {
    uint32_t mode = 0xFFFFFFFF;
    uint8_t active = 0xFF;
    uint8_t physical = 0xFF;
    std::chrono::steady_clock::time_point last_sample;
  };
  std::unordered_map<uint32_t, ControllerLog> g_controller_logs;

  // sub_825DA158 hands a dying character to its ragdoll here (mode 1). If it
  // dies during a death animation the blend lasts 2.5x the animation's length
  // (7-16 s), so the body stays mostly animation-driven while it falls: it
  // never reaches the 0.7 weight that makes it fully physical before it
  // settles, and drifts down instead of dropping (pdz_ragdoll_death_blend_max).
  constexpr uint32_t kDeathSwitchReturn = 0x825DA3D0;

  // The inertia tensor the builder computed for the body it is creating
  // (sub_825D6E70 output +32 / +48 / +64: three rows of four floats), for
  // the per-body log.
  constexpr uint32_t kMassPropsInertia = 32;
  thread_local bool t_have_inertia = false;
  thread_local float t_inertia[3][3];

  // Fall trace (pdz_ragdoll_log): the pelvis body for a second after a switch
  // to ragdoll. Controller +24 bodies, +28 count; body +80 motion, motion +64
  // position (metres). The humans' 15 bodies are in asset order, where
  // index 3 is Biped_base (the pelvis); others use body 0.
  constexpr uint32_t kControllerBodies = 0x18;
  constexpr uint32_t kControllerBodyCount = 0x1C;
  constexpr uint32_t kBodyMotion = 0x50;
  constexpr uint32_t kMotionPosition = 0x40;
  constexpr uint32_t kHumanBodies = 15;
  constexpr uint32_t kHumanPelvis = 3;
  constexpr double kFallTraceSeconds = 1.0;
  constexpr double kFallSampleSeconds = 0.05;

  struct FallTrace
  {
    std::chrono::steady_clock::time_point start, last_sample;
    float start_y = 0.0f, last_y = 0.0f;
    double last_t = 0.0;
    bool dropped_1m = false;
  };
  std::unordered_map<uint32_t, FallTrace> g_fall_traces;
  std::mutex g_fall_mutex;

  bool PelvisPosition(const uint8_t *base, uint32_t controller, float pos[3])
  {
    const uint32_t bodies = LoadBe32(base, controller + kControllerBodies);
    const uint32_t count = LoadBe32(base, controller + kControllerBodyCount);
    if (!bodies || !count || count > 64)
    {
      return false;
    }
    const uint32_t body = LoadBe32(base, bodies + (count == kHumanBodies ? kHumanPelvis : 0) * 4);
    const uint32_t motion = body ? LoadBe32(base, body + kBodyMotion) : 0;
    if (!motion)
    {
      return false;
    }
    for (int i = 0; i < 3; ++i)
    {
      pos[i] = LoadBeFloat(base, motion + kMotionPosition + i * 4);
    }
    return true;
  }
} // namespace

// sub_825D6E70(shape, f1 = mass, &mass properties): a body's mass properties.
// Inside the ragdoll builder, note the inertia tensor it computed (log only).
REX_HOOK_RAW(sub_825D6E70)
{
  const uint32_t props = ctx.r5.u32;
  __imp__sub_825D6E70(ctx, base);
  if (t_building_ragdoll && props && REXCVAR_GET(pdz_ragdoll_log))
  {
    for (int row = 0; row < 3; ++row)
    {
      for (int column = 0; column < 3; ++column)
      {
        t_inertia[row][column] = LoadBeFloat(base, props + kMassPropsInertia + row * 16 + column * 4);
      }
    }
    t_have_inertia = true;
  }
}

// sub_825D8B30(ragdoll, asset): builds the ragdoll's bodies and constraints.
REX_HOOK_RAW(sub_825D8B30)
{
  t_building_ragdoll = true;
  __imp__sub_825D8B30(ctx, base);
  t_building_ragdoll = false;
}

// sub_829B1A98(?, cinfo): creates a rigid body from a cinfo.
REX_HOOK_RAW(sub_829B1A98)
{
  if (t_building_ragdoll)
  {
    const uint32_t cinfo = ctx.r4.u32;
    const double linear = REXCVAR_GET(pdz_ragdoll_linear_damping);
    const double angular = REXCVAR_GET(pdz_ragdoll_angular_damping);
    if (linear >= 0.0)
    {
      StoreBeFloat(base, cinfo + kCinfoLinearDamping, float(linear));
    }
    if (angular >= 0.0)
    {
      StoreBeFloat(base, cinfo + kCinfoAngularDamping, float(angular));
    }
  }
  __imp__sub_829B1A98(ctx, base);
}

// sub_825DC560(controller, mode, f1 = seconds): animation/ragdoll switch.
REX_HOOK_RAW(sub_825DC560)
{
  const double requested = ctx.f1.f64;
  const double blend_max = REXCVAR_GET(pdz_ragdoll_death_blend_max);
  if (blend_max > 0.0 && uint32_t(ctx.lr) == kDeathSwitchReturn && ctx.r4.u32 == 1 && ctx.f1.f64 > blend_max)
  {
    ctx.f1.f64 = blend_max;
  }
  if (REXCVAR_GET(pdz_ragdoll_log))
  {
    REXLOG_INFO("pdz_ragdoll: switch controller {:08X} to mode {} over {:.2f} s (game asked {:.2f} s, "
                "from {:08X}); was mode {} weight {:.2f} physical {}",
                ctx.r3.u32, ctx.r4.u32, ctx.f1.f64, requested, uint32_t(ctx.lr),
                LoadBe32(base, ctx.r3.u32 + kControllerMode),
                LoadBeFloat(base, ctx.r3.u32 + kControllerWeight), base[ctx.r3.u32 + kControllerPhysical]);
    if (ctx.r4.u32 >= 1 && ctx.r4.u32 <= 3)
    {
      std::lock_guard lock(g_fall_mutex);
      g_fall_traces[ctx.r3.u32] = FallTrace{}; // starts at the next update
    }
  }
  __imp__sub_825DC560(ctx, base);
}

// sub_825DA950(?, f1 = dt): per-frame ragdoll update.
REX_HOOK_RAW(sub_825DA950)
{
  __imp__sub_825DA950(ctx, base);
  if (!REXCVAR_GET(pdz_ragdoll_log))
  {
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  // Fall traces: the pelvis' height and vertical speed every 50 ms for a
  // second after the switch, and when its first 1 m drop completed (free
  // fall: ~0.45 s at 9.8 m/s²).
  std::unique_lock fall_lock(g_fall_mutex);
  for (auto it = g_fall_traces.begin(); it != g_fall_traces.end();)
  {
    FallTrace &trace = it->second;
    float pos[3];
    if (!PelvisPosition(base, it->first, pos))
    {
      it = g_fall_traces.erase(it);
      continue;
    }
    if (trace.start == std::chrono::steady_clock::time_point{})
    {
      trace.start = trace.last_sample = now;
      trace.start_y = trace.last_y = pos[1];
      REXLOG_INFO("pdz_ragdoll: fall {:08X} start, pelvis y {:.3f} m", it->first, pos[1]);
      ++it;
      continue;
    }
    const double t = std::chrono::duration<double>(now - trace.start).count();
    const float drop = trace.start_y - pos[1];
    if (!trace.dropped_1m && drop >= 1.0f)
    {
      trace.dropped_1m = true;
      REXLOG_INFO("pdz_ragdoll: fall {:08X} dropped 1 m in {:.3f} s", it->first, t);
    }
    if (std::chrono::duration<double>(now - trace.last_sample).count() >= kFallSampleSeconds ||
        t >= kFallTraceSeconds)
    {
      const double dt = t - trace.last_t;
      const double vy = dt > 0.0 ? (pos[1] - trace.last_y) / dt : 0.0;
      REXLOG_INFO("pdz_ragdoll: fall {:08X} t {:.3f} s pelvis y {:.3f} m drop {:.3f} m vy {:+.2f} m/s", it->first, t,
                  pos[1], drop, vy);
      trace.last_sample = now;
      trace.last_t = t;
      trace.last_y = pos[1];
    }
    if (t >= kFallTraceSeconds)
    {
      if (!trace.dropped_1m)
      {
        REXLOG_INFO("pdz_ragdoll: fall {:08X} dropped {:.3f} m in {:.2f} s (less than 1 m)", it->first, drop, t);
      }
      it = g_fall_traces.erase(it);
      continue;
    }
    ++it;
  }
  fall_lock.unlock();
  for (uint32_t controller = LoadBe32(base, kControllerList), guard = 0; controller && guard < 256;
       controller = LoadBe32(base, controller + kControllerNext), ++guard)
  {
    ControllerLog &log = g_controller_logs[controller];
    const uint32_t mode = LoadBe32(base, controller + kControllerMode);
    const uint8_t active = base[controller + kControllerActive];
    const uint8_t physical = base[controller + kControllerPhysical];
    const bool changed = mode != log.mode || active != log.active || physical != log.physical;
    // While it's being driven by an animation, sample it ten times a second.
    const bool blending = active && !physical && now - log.last_sample > std::chrono::milliseconds(100);
    if (changed || blending)
    {
      REXLOG_INFO("pdz_ragdoll: controller {:08X} mode {} weight {:.2f} rate {:+.2f}/s active {} "
                  "physical {} timer {:.2f}",
                  controller, mode, LoadBeFloat(base, controller + kControllerWeight),
                  LoadBeFloat(base, controller + kControllerRate), active, physical,
                  LoadBeFloat(base, controller + kControllerTimer));
      log.mode = mode;
      log.active = active;
      log.physical = physical;
      log.last_sample = now;
    }
  }
}

// sub_829B0790(body, &inverse inertia): body->motion->setInertiaInvLocal
// (sphere motion: the smallest diagonal entry becomes its one inverse
// inertia). The builder calls it once per body with 0.1 x the inverse it read
// back, i.e. an inertia of 10 x the largest principal moment (a 3x3 matrix,
// three 16-byte rows). pdz_ragdoll_inertia_scale multiplies this inverse, so
// it divides the inertia: 10 = 10x lighter (the largest natural moment).
REX_HOOK_RAW(sub_829B0790)
{
  const uint32_t inverse = ctx.r4.u32;
  const double scale = REXCVAR_GET(pdz_ragdoll_inertia_scale);
  if (t_building_ragdoll && scale != 1.0)
  {
    for (uint32_t row = 0; row < 3; ++row)
    {
      for (uint32_t column = 0; column < 3; ++column)
      {
        const uint32_t address = inverse + row * 16 + column * 4;
        StoreBeFloat(base, address, LoadBeFloat(base, address) * float(scale));
      }
    }
  }
  if (t_building_ragdoll && t_have_inertia && REXCVAR_GET(pdz_ragdoll_log))
  {
    const float largest = std::max({t_inertia[0][0], t_inertia[1][1], t_inertia[2][2]});
    const float set = std::min({LoadBeFloat(base, inverse), LoadBeFloat(base, inverse + 20),
                                LoadBeFloat(base, inverse + 40)});
    const float inertia = set > 0.0f ? 1.0f / set : 0.0f;
    REXLOG_INFO("pdz_ragdoll: body inertia computed {:.5f} {:.5f} {:.5f}, set inverse {:.4f} = inertia {:.5f} "
                "on every axis ({:.1f} x the largest)",
                t_inertia[0][0], t_inertia[1][1], t_inertia[2][2], set, inertia,
                largest > 0.0f ? inertia / largest : 0.0f);
  }
  t_have_inertia = false;
  __imp__sub_829B0790(ctx, base);
}
