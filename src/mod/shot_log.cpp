// Per-shot diagnostic (pdz_shot_log). See README.md next to this file.
//
// Read-only. One "pdz_shot:" line per hitscan shot by a player: weapon,
// zoom, cover state, where the shot starts, where the crosshair points, where
// auto-aim bent it to, the spread, whether the straight path to the bent aim
// point is blocked, and what the bullet hit.
//
// Sources:
// - the shot's aim: sub_822397C8 called by the weapon aim sub_825787F0
//   (returns to 82578860; weapon in r31, player in r26), once per update per
//   hand: r4 = the screen point it aims through (player +0x1F58: 0, 0 = the
//   crosshair, else auto-aim's bent point), f2 = the gun's spread, r8 = the
//   aim point it writes (player +0x1FF0 + 12 x hand: what the camera ray
//   through that point, moved at random within the spread, hits first, 2 cm
//   past it, else 500 m out), r9 = the spread angle it writes. The spread
//   cone is f2 + (gun record byte +0xB2 - 1) / 300 (screen units);
// - auto-aim (player +0x1F60): +0x14 target, +0x24 the target's aim point
//   (sub_82535E00: a fixed point of the target's model, not a bone), +0x30
//   where the player aims and +0x38 the bent point (screen), +0x40 the eye,
//   +0x4C the crosshair's world point (25000 cm out);
// - the shot: sub_82238938(shooter, weapon, &origin, &aim_point, ...): the
//   shooter is the player or a gun whose +0x5C is the player; the weapon's
//   gun record is +0x35C, its gun type +0x478 (names: GunType table
//   82D21D28); hand 1 is the weapon at player +0xFF0, hand 0 at +0xFEC. Per
//   segment sub_82175658(&from, &to, weapon): the last segment ends where
//   the bullet stopped. A hit on an object: sub_82512D48(object, weapon,
//   gun, entry, ...); characters (vtable +0xB8 = sub_82539910) read the hit
//   box from the entry (+0x34, low 16 bits, sign-extended: HitBox table
//   82D21940 from -1, 0 = Head);
// - cover (player): state +0x1CA4 (0 out, 1 entering, 2 leaning out / back,
//   3 in cover, 4 popped out, 5 exiting), camera blend timer +0x1CA0 (0.3 s,
//   s), edge-step blend flag +0x1C78, cover camera position +0x1C88.
// The path test casts two rays from the shot's origin to the target's aim
// point with the game's ray (sub_82252798): the bullets' layer (1, as
// sub_82238938 casts) and the aim ray's layer (32, as sub_822397C8 casts).
// The crosshair ray (layer 32, eye -> crosshair point) says what the
// crosshair itself is on.
#include "shot_log.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <rex/hook.h>
#include "guest_function.h" // our indirect calls: nullptr for no function (log 101)
#include <rex/logging.h>

#include "../game_cvars.h"

REX_EXTERN(__imp__sub_82175658);
REX_EXTERN(__imp__sub_82512D48);
REX_EXTERN(sub_82252798);
REX_EXTERN(sub_8214C7E8);

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
    for (int i = 0; i < 4; ++i)
    {
      base[address + i] = uint8_t(bits >> (24 - 8 * i));
    }
  }

  // Guest heap and stack objects can sit below 0x40000000 too (the player
  // does), so only rule out null-ish and the top page.
  bool IsGuestPointer(uint32_t address)
  {
    return address >= 0x00010000u && address < 0xFFFF0000u;
  }

  bool InImage(uint32_t address)
  {
    return address >= 0x82000000u && address < 0x83400000u;
  }

  bool Enabled()
  {
    return REXCVAR_GET(pdz_shot_log);
  }

  using Clock = std::chrono::steady_clock;
  using Vec = std::array<float, 3>;
  constexpr double kRadToDeg = 57.29577951308232;

  // Player fields.
  constexpr uint32_t kPlayerHand0 = 0xFEC;
  constexpr uint32_t kPlayerHand1 = 0xFF0;
  constexpr uint32_t kPlayerZoom = 0x1A9C;
  constexpr uint32_t kPlayerAutoAim = 0x1F60;
  constexpr uint32_t kCoverBlendFlag = 0x1C78;
  constexpr uint32_t kCoverCamera = 0x1C88;
  constexpr uint32_t kCoverBlendTimer = 0x1CA0;
  constexpr uint32_t kCoverState = 0x1CA4;
  // Auto-aim fields.
  constexpr uint32_t kAutoAimOwner = 0xC; // not the player; its vtable +476 is auto-aim's camera
  constexpr uint32_t kAutoAimTarget = 0x14;
  constexpr uint32_t kAutoAimTargetPoint = 0x24;
  constexpr uint32_t kAutoAimReticle = 0x30;
  constexpr uint32_t kAutoAimBent = 0x38;
  constexpr uint32_t kAutoAimEye = 0x40;
  constexpr uint32_t kAutoAimCrosshairPoint = 0x4C;
  // Weapon and gun record.
  constexpr uint32_t kWeaponOwner = 0x5C;
  constexpr uint32_t kWeaponRecord = 0x35C;
  constexpr uint32_t kRecordBaseInaccuracy = 0xB2; // byte, (n - 1) / 300
  constexpr uint32_t kRecordGunType = 0x478;
  constexpr double kSpreadUnit = 1.0 / 300.0;
  // Hit entry and hit boxes.
  constexpr uint32_t kEntryHitBox = 0x34;
  constexpr uint32_t kCharacterHitSlot = 0xB8;
  constexpr uint32_t kCharacterHitFunc = 0x82539910;
  constexpr uint32_t kHitBoxNames = 0x82D21940; // index + 1
  constexpr int kHitBoxMax = 19;
  constexpr uint32_t kGunTypeNames = 0x82D21D28;
  constexpr uint32_t kGunTypeCount = 0x40;
  // Cameras: vtable +476 of the player (the weapon aim's, sub_822397C8) and
  // of auto-aim's owner; +68 = horizontal FOV (radians, 58 deg at 4:3).
  constexpr uint32_t kCameraGetter = 476;
  constexpr uint32_t kCameraFov = 68;
  // Calls.
  constexpr uint32_t kAimReturn = 0x82578860; // sub_822397C8 from sub_825787F0
  constexpr uint32_t kRayCollectorVtbl = 0x820925C0;
  constexpr uint32_t kBulletLayer = 1;
  constexpr uint32_t kAimLayer = 32;
  // A ray "stops short" of the target this far before its aim point.
  constexpr float kBlockedMarginCm = 50.0f;
  // Origin "is" a camera within this distance.
  constexpr float kSamePointCm = 2.0f;

  struct Aim
  {
    bool valid = false;
    float screen[2] = {};
    float spread = 0.0f;
    float cone = 0.0f;
    float spread_angle_out = 0.0f;
    Vec aim_point{};
  };

  struct Player
  {
    uint32_t player = 0;
    std::array<Aim, 2> aims;
    uint32_t cover_state = 0;
    Clock::time_point cover_since;
    int shots_in_state = 0;
  };
  std::vector<Player> g_players;
  Clock::time_point g_start;
  // The first calls each hook sees, so a run with no shot lines shows why.
  constexpr int kSetupLines = 6;
  int g_aim_lines = 0;
  int g_shot_lines = 0;

  Player *Find(uint32_t player)
  {
    for (Player &p : g_players)
    {
      if (p.player == player)
      {
        return &p;
      }
    }
    return nullptr;
  }

  Player &Get(uint32_t player)
  {
    if (Player *p = Find(player))
    {
      return *p;
    }
    if (g_players.size() >= 4)
    {
      g_players.erase(g_players.begin());
    }
    g_players.push_back({});
    g_players.back().player = player;
    return g_players.back();
  }

  Vec LoadVec(const uint8_t *base, uint32_t address)
  {
    return {LoadBeFloat(base, address), LoadBeFloat(base, address + 4), LoadBeFloat(base, address + 8)};
  }

  Vec Sub(const Vec &a, const Vec &b)
  {
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
  }

  float Length(const Vec &v)
  {
    return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
  }

  // Yaw / pitch of a direction, degrees (y is up).
  void Angles(const Vec &v, double &yaw, double &pitch)
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

  // "yaw +0.42 pitch -1.30 deg": direction b relative to direction a.
  std::string Deviation(const Vec &a, const Vec &b)
  {
    double ya, pa, yb, pb;
    Angles(a, ya, pa);
    Angles(b, yb, pb);
    return fmt::format("yaw {:+.2f} pitch {:+.2f} deg", WrapDeg(yb - ya), pb - pa);
  }

  std::string GuestString(const uint8_t *base, uint32_t address, const char *prefix)
  {
    if (!InImage(address))
    {
      return "?";
    }
    std::string s(reinterpret_cast<const char *>(base + address));
    const size_t n = std::strlen(prefix);
    return s.compare(0, n, prefix) == 0 ? s.substr(n) : s;
  }

  std::string GunName(const uint8_t *base, uint32_t weapon)
  {
    const uint32_t record = IsGuestPointer(weapon) ? LoadBe32(base, weapon + kWeaponRecord) : 0;
    if (!IsGuestPointer(record))
    {
      return "?";
    }
    const uint32_t type = LoadBe32(base, record + kRecordGunType);
    if (type >= kGunTypeCount)
    {
      return fmt::format("type {}", type);
    }
    return GuestString(base, LoadBe32(base, kGunTypeNames + type * 4), "GunType_");
  }

  std::string HitBoxName(const uint8_t *base, int box)
  {
    if (box < -1 || box > kHitBoxMax)
    {
      return fmt::format("box {}", box);
    }
    return GuestString(base, LoadBe32(base, kHitBoxNames + uint32_t(box + 1) * 4), "HitBox_");
  }

  uint32_t CallVirtual(PPCContext &ctx, uint8_t *base, uint32_t object, uint32_t slot)
  {
    PPCFunc *fn = pdz::GuestFunction(LoadBe32(base, LoadBe32(base, object) + slot));
    if (!fn)
    {
      return 0;
    }
    ctx.r3.u64 = object;
    fn(ctx, base);
    return ctx.r3.u32;
  }

  // The game's ray from -> to on a layer, built like sub_822397C8 does.
  // Returns the distance to the first hit, or -1 for none.
  float CastRay(PPCContext &ctx, uint8_t *base, uint32_t layer, const Vec &from, const Vec &to)
  {
    const float length = Length(Sub(to, from));
    if (length < 1.0f)
    {
      return -1.0f;
    }
    // Scratch below the caller's frame; the callees build their frames under sp.
    rex::CallFrame frame(ctx);
    const uint32_t sp = (ctx.r1.u32 - 0x400) & ~0xFu;
    const uint32_t from_address = sp + 0x200;
    const uint32_t to_address = sp + 0x210;
    const uint32_t collector = sp + 0x280;
    frame.ctx.r1.u64 = sp;
    for (uint32_t i = 0; i < 3; ++i)
    {
      StoreBeFloat(base, from_address + i * 4, from[i]);
      StoreBeFloat(base, to_address + i * 4, to[i]);
    }
    std::memset(base + collector, 0, 0x100);
    for (int i = 0; i < 4; ++i)
    {
      base[collector + i] = uint8_t(kRayCollectorVtbl >> (24 - 8 * i));
    }
    CallVirtual(frame.ctx, base, collector, 12);
    frame.ctx.r3.u64 = layer;
    frame.ctx.r4.u64 = from_address;
    frame.ctx.r5.u64 = to_address;
    frame.ctx.r6.u64 = collector;
    frame.ctx.r7.u64 = 1;
    sub_82252798(frame.ctx, base);
    if ((CallVirtual(frame.ctx, base, collector, 8) & 0xFF) == 0)
    {
      return -1.0f;
    }
    const float fraction = LoadBeFloat(base, collector + 80);
    return fraction >= 0.0f && fraction <= 1.0f ? fraction * length : -1.0f;
  }

  // "clear" or "blocked at 11.8 m" for a ray to a point `distance` away.
  std::string PathText(float hit, float distance)
  {
    if (hit < 0.0f || hit > distance - kBlockedMarginCm)
    {
      return "clear";
    }
    return fmt::format("blocked at {:.2f} m", hit / 100.0f);
  }

  Vec CharacterPosition(PPCContext &ctx, uint8_t *base, uint32_t character)
  {
    rex::CallFrame frame(ctx);
    frame.ctx.r3.u64 = character;
    sub_8214C7E8(frame.ctx, base);
    const uint32_t address = frame.ctx.r3.u32 ? frame.ctx.r3.u32 : character + 0x30;
    return LoadVec(base, address);
  }

  // The aim call in progress (BeforeAim -> AfterAim).
  thread_local Player *t_aim_player = nullptr;
  thread_local int t_aim_hand = 0;
  thread_local uint32_t t_aim_point_address = 0;
  thread_local uint32_t t_aim_spread_address = 0;

  // The shot in progress (BeforeShot -> AfterShot).
  struct Shot
  {
    Player *player = nullptr;
    uint32_t weapon = 0;
    int hand = 0;
    Vec origin{};
    Vec aim_point{};
    int segments = 0;
    Vec end{};
    uint32_t hit_object = 0;
    uint32_t hit_vtable = 0;
    bool hit_character = false;
    int hit_box = 0;
    uint32_t damaged = 0; // first character damaged during the shot
    double damage = 0.0;
  };
  thread_local bool t_in_shot = false;
  thread_local Shot t_shot;
} // namespace

namespace shot_log
{
  void BeforeAim(PPCContext &ctx, uint8_t *base)
  {
    t_aim_player = nullptr;
    if (!Enabled() || uint32_t(ctx.lr) != kAimReturn)
    {
      return;
    }
    const uint32_t weapon = ctx.r31.u32;
    const uint32_t player = ctx.r26.u32;
    if (g_aim_lines < kSetupLines)
    {
      ++g_aim_lines;
      REXLOG_INFO("pdz_shot: aim update player {:08X} weapon {:08X} hand1 {:08X}", player, weapon,
                  player ? LoadBe32(base, player + kPlayerHand1) : 0);
    }
    if (!player || !IsGuestPointer(weapon))
    {
      return;
    }
    Player &p = Get(player);
    // Cover state changes, for "time in this state" and the shot count.
    const uint32_t state = LoadBe32(base, player + kCoverState);
    if (state != p.cover_state || p.cover_since == Clock::time_point{})
    {
      p.cover_state = state;
      p.cover_since = Clock::now();
      p.shots_in_state = 0;
    }
    t_aim_player = &p;
    t_aim_hand = weapon == LoadBe32(base, player + kPlayerHand1) ? 1 : 0;
    t_aim_point_address = ctx.r8.u32;
    t_aim_spread_address = ctx.r9.u32;
    Aim &aim = p.aims[t_aim_hand];
    aim.screen[0] = LoadBeFloat(base, ctx.r4.u32);
    aim.screen[1] = LoadBeFloat(base, ctx.r4.u32 + 4);
    aim.spread = float(ctx.f2.f64);
    const uint32_t record = ctx.r5.u32;
    aim.cone = aim.spread + (IsGuestPointer(record) ? float((int(base[record + kRecordBaseInaccuracy]) - 1) * kSpreadUnit)
                                                    : 0.001f);
  }

  void AfterAim(PPCContext &, uint8_t *base)
  {
    if (!t_aim_player)
    {
      return;
    }
    Aim &aim = t_aim_player->aims[t_aim_hand];
    aim.aim_point = LoadVec(base, t_aim_point_address);
    aim.spread_angle_out = LoadBeFloat(base, t_aim_spread_address);
    aim.valid = true;
    t_aim_player = nullptr;
  }

  void BeforeShot(PPCContext &ctx, uint8_t *base)
  {
    t_in_shot = false;
    if (!Enabled())
    {
      return;
    }
    const uint32_t shooter = ctx.r3.u32;
    Player *player = Find(shooter);
    if (!player && IsGuestPointer(shooter))
    {
      player = Find(LoadBe32(base, shooter + kWeaponOwner));
    }
    if (g_shot_lines < kSetupLines)
    {
      ++g_shot_lines;
      REXLOG_INFO("pdz_shot: hitscan shooter {:08X} (+0x5C {:08X}) weapon {:08X} from {:08X} -> {}", shooter,
                  IsGuestPointer(shooter) ? LoadBe32(base, shooter + kWeaponOwner) : 0, ctx.r4.u32,
                  uint32_t(ctx.lr), player ? "a player's shot" : "not a known player");
    }
    if (!player)
    {
      return;
    }
    t_shot = {};
    t_shot.player = player;
    t_shot.weapon = ctx.r4.u32;
    t_shot.hand = t_shot.weapon == LoadBe32(base, player->player + kPlayerHand1) ? 1 : 0;
    t_shot.origin = LoadVec(base, ctx.r5.u32);
    t_shot.aim_point = LoadVec(base, ctx.r6.u32);
    t_in_shot = true;
  }

  void AfterShot(PPCContext &ctx, uint8_t *base)
  {
    if (!t_in_shot)
    {
      return;
    }
    t_in_shot = false;
    const Shot shot = t_shot;
    Player &p = *shot.player;
    const uint32_t player = p.player;
    const auto now = Clock::now();
    if (g_start == Clock::time_point{})
    {
      g_start = now;
    }
    ++p.shots_in_state;

    const uint32_t autoaim = player + kPlayerAutoAim;
    const Vec eye = LoadVec(base, autoaim + kAutoAimEye);
    const Vec crosshair_point = LoadVec(base, autoaim + kAutoAimCrosshairPoint);
    const Vec crosshair_dir = Sub(crosshair_point, eye);
    const Vec shot_dir = Sub(shot.aim_point, shot.origin);
    const float zoom = LoadBeFloat(base, player + kPlayerZoom);
    const uint32_t cover = LoadBe32(base, player + kCoverState);

    std::string line = fmt::format(
        "pdz_shot: t {:.3f} {:08X} {} hand {} | zoom {:.2f} | cover {} ({:.2f} s, shot {} in state, blend {:.2f} s{})",
        std::chrono::duration<double>(now - g_start).count(), player, GunName(base, shot.weapon), shot.hand, zoom, cover,
        std::chrono::duration<double>(now - p.cover_since).count(), p.shots_in_state,
        LoadBeFloat(base, player + kCoverBlendTimer), base[player + kCoverBlendFlag] ? " edge" : "");

    // Where the shot starts: the eye (the camera the crosshair belongs to),
    // the cover camera, or somewhere else (offset from the eye, cm).
    const Vec from_eye = Sub(shot.origin, eye);
    const Vec cover_camera = LoadVec(base, player + kCoverCamera);
    if (Length(from_eye) < kSamePointCm)
    {
      line += " | origin eye";
    }
    else if (cover && Length(Sub(shot.origin, cover_camera)) < kSamePointCm)
    {
      line += fmt::format(" | origin cover camera ({:+.1f} {:+.1f} {:+.1f} cm from eye)", from_eye[0], from_eye[1],
                          from_eye[2]);
    }
    else
    {
      line += fmt::format(" | origin {:+.1f} {:+.1f} {:+.1f} cm from eye", from_eye[0], from_eye[1], from_eye[2]);
    }
    if (cover)
    {
      const Vec cam = Sub(cover_camera, eye);
      line += fmt::format(" (cover camera {:+.1f} {:+.1f} {:+.1f})", cam[0], cam[1], cam[2]);
    }

    // What the crosshair is on (aim-ray layer, eye -> crosshair point).
    const float crosshair_hit = CastRay(ctx, base, kAimLayer, eye, crosshair_point);
    line += crosshair_hit >= 0.0f ? fmt::format(" | crosshair on world at {:.2f} m", crosshair_hit / 100.0f)
                                  : std::string(" | crosshair on open space");

    // Auto-aim: the target, its aim point and the bend.
    const uint32_t target = LoadBe32(base, autoaim + kAutoAimTarget);
    const float reticle[2] = {LoadBeFloat(base, autoaim + kAutoAimReticle),
                              LoadBeFloat(base, autoaim + kAutoAimReticle + 4)};
    const float bent[2] = {LoadBeFloat(base, autoaim + kAutoAimBent), LoadBeFloat(base, autoaim + kAutoAimBent + 4)};
    const Aim &aim = p.aims[shot.hand];
    // Bent: auto-aim moved the point (as aim_assist.cpp's "locked") and the
    // weapon aimed through it (player +0x1F58 holds it unless the game
    // skipped auto-aim this update, then 0, 0).
    const bool bending = target && (bent[0] != reticle[0] || bent[1] != reticle[1]) && aim.valid &&
                         aim.screen[0] == bent[0] && aim.screen[1] == bent[1];
    Vec target_point{};
    float target_distance = 0.0f;
    if (target)
    {
      target_point = LoadVec(base, autoaim + kAutoAimTargetPoint);
      target_distance = Length(Sub(target_point, shot.origin));
      const Vec feet = CharacterPosition(ctx, base, target);
      line += fmt::format(" | target {:08X} at {:.2f} m, aim point {:.0f} cm above its feet", target,
                          target_distance / 100.0f, target_point[1] - feet[1]);
      line += bending ? fmt::format(", bent {} (screen {:+.3f} {:+.3f} -> {:+.3f} {:+.3f})",
                                    Deviation(crosshair_dir, Sub(target_point, eye)), reticle[0], reticle[1], bent[0],
                                    bent[1])
                      : std::string(", not bent");
      // The straight path from the shot's origin to the target's aim point.
      const float bullet_hit = CastRay(ctx, base, kBulletLayer, shot.origin, target_point);
      const float aim_hit = CastRay(ctx, base, kAimLayer, shot.origin, target_point);
      line += fmt::format(" | path to aim point: bullets {}, aim ray {}", PathText(bullet_hit, target_distance),
                          PathText(aim_hit, target_distance));
    }
    else
    {
      line += " | no target";
    }

    // Spread, and where the shot went relative to the crosshair (and to the
    // bent point), plus how far off that is at the aim point's distance.
    if (aim.valid)
    {
      line += fmt::format(" | spread {:.4f} cone {:.4f} angle {:.4f}", aim.spread, aim.cone, aim.spread_angle_out);
    }
    const float shot_distance = Length(shot_dir);
    double ya, pa, ys, ps;
    Angles(crosshair_dir, ya, pa);
    Angles(shot_dir, ys, ps);
    line += fmt::format(" | shot vs crosshair {} ({:+.0f} cm up at {:.2f} m)", Deviation(crosshair_dir, shot_dir),
                        std::tan((ps - pa) / kRadToDeg) * shot_distance, shot_distance / 100.0f);
    if (bending)
    {
      line += fmt::format(", vs bent point {}", Deviation(Sub(target_point, shot.origin), shot_dir));
    }

    // The cameras the bend is projected with (auto-aim) and unprojected with
    // (the weapon aim): a FOV mismatch moves every bent shot.
    {
      rex::CallFrame frame(ctx);
      const uint32_t weapon_camera = CallVirtual(frame.ctx, base, player, kCameraGetter);
      const uint32_t owner = LoadBe32(base, autoaim + kAutoAimOwner);
      const uint32_t autoaim_camera = IsGuestPointer(owner) ? CallVirtual(frame.ctx, base, owner, kCameraGetter) : 0;
      const auto fov = [&](uint32_t camera)
      { return IsGuestPointer(camera) ? LoadBeFloat(base, camera + kCameraFov) * kRadToDeg : 0.0; };
      line += fmt::format(" | cameras: weapon {:08X} fov {:.2f}, auto-aim {:08X} fov {:.2f}", weapon_camera,
                          fov(weapon_camera), autoaim_camera, fov(autoaim_camera));
    }

    // What the bullet hit.
    if (shot.hit_character)
    {
      line += fmt::format(" | hit {:08X}{} {}", shot.hit_object, shot.hit_object == target ? " (target)" : "",
                          HitBoxName(base, shot.hit_box));
    }
    else if (shot.hit_object)
    {
      line += fmt::format(" | hit object {:08X} (vtable {:08X})", shot.hit_object, shot.hit_vtable);
    }
    else if (shot.segments)
    {
      const float end = Length(Sub(shot.end, shot.origin));
      line += end < 49000.0f ? fmt::format(" | hit world at {:.2f} m", end / 100.0f) : std::string(" | hit nothing");
    }
    if (shot.damaged)
    {
      line += fmt::format(" | damage {:.1f} to {:08X}{}", shot.damage, shot.damaged,
                          shot.damaged == target ? " (target)" : "");
    }
    REXLOG_INFO("{}", line);
  }

  void OnDamage(uint32_t victim, double damage)
  {
    if (t_in_shot && !t_shot.damaged)
    {
      t_shot.damaged = victim;
      t_shot.damage = damage;
    }
  }
} // namespace shot_log

// sub_82175658(&from, &to, weapon): one segment of a hitscan shot (tracer and
// impact); during a player's shot the last one ends where the bullet stopped.
REX_HOOK_RAW(sub_82175658)
{
  if (t_in_shot)
  {
    ++t_shot.segments;
    t_shot.end = LoadVec(base, ctx.r4.u32);
  }
  __imp__sub_82175658(ctx, base);
}

// sub_82512D48(object, weapon, gun, entry, ...): a hitscan hit on an object.
REX_HOOK_RAW(sub_82512D48)
{
  if (t_in_shot && !t_shot.hit_object)
  {
    const uint32_t object = ctx.r3.u32;
    t_shot.hit_object = object;
    const uint32_t vtbl = object ? LoadBe32(base, object) : 0;
    t_shot.hit_vtable = vtbl;
    t_shot.hit_character = InImage(vtbl) && LoadBe32(base, vtbl + kCharacterHitSlot) == kCharacterHitFunc;
    if (t_shot.hit_character)
    {
      t_shot.hit_box = int(int16_t(LoadBe32(base, ctx.r6.u32 + kEntryHitBox) & 0xFFFF));
    }
  }
  __imp__sub_82512D48(ctx, base);
}
