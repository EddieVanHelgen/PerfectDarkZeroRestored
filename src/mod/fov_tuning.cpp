// Field of view (pdz_fov_scale, 1.0 = original; an enhancement of ours). In the TU3 build since 2026-11-04 (the
// settings window's Field of view slider). Title Update 3 addresses; launch addresses in brackets.
//
// The gameplay camera (mode id 0, update sub_82214C48 [8220D9C8]) sets its
// FOV to 58 degrees (820EAF40 [820EC228]) times the player's zoom factor,
// which it gets from player vtable +0x294 [+0x290] (sub_825AC8E0 [8259D0D8]:
// 1 - player +0x1A9C). That is a 4:3 horizontal FOV: the camera manager
// converts it to vertical (2 * atan(0.75 * tan(h / 2)) = 45.2 degrees) and
// the projection widens it by the aspect ratio, so at 16:9 the original is
// 2 * atan(4/3 * tan(29 deg)) = 72.9 degrees across.
//
// pdz_fov_scale scales that zoom factor for this one call, so the on-foot
// view (first person, single player and multiplayer) widens, and a scope's or
// a step zoom level's field of view is widened by the same factor: each keeps
// its magnification relative to the unzoomed view. The mouse's zoomed turn rate
// (mouse_look.cpp) uses the same scaled angles. Vehicles, turrets, cutscenes,
// menus and other camera modes ask elsewhere (pdz_fov_log lists every caller)
// and keep Rare's FOV.
//
// Callers seen in play (log PDZ_Job8 003, 103 degrees): the gameplay camera, the Halo look model's zoomed turn rate
// (825B0DD8: an angular rate, kept), and the weapon aim (sub_82240A90 [822397C8], 82240B70 / 82240C1C / 82240C34):
// it divides the aim point and the spread offset by the zoom factor (units of Rare's unzoomed 58 degree view) and
// casts the aim ray through the camera, so with a scaled camera it gets the same scale: the shots keep Rare's spread
// angle and go where the crosshair points. Only for players whose camera was scaled (local players; bots keep
// Rare's). game_fixes.cpp's auto-aim bend conversion uses ScaleFor() as well.
#include <array>
#include <atomic>
#include <chrono>
#include <string>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "../game_cvars.h"
#include "fov_tuning.h"

REXCVAR_DEFINE_BOOL(pdz_fov_log, false, "Debug",
                    "Log every distinct caller of the player's zoom factor (sub_825AC8E0) with its count every "
                    "10 s, and whether pdz_fov_scale applied to it");

REX_EXTERN(__imp__sub_825AC8E0);

namespace
{
  // Return address of the gameplay camera's zoom-factor call (822156C8:
  // lwz r11, 0x294(r11); mtctr; bctrl; then 58.0 * f1).
  constexpr uint32_t kCameraZoomReturn = 0x822156D4; // [8220E454]
  // The weapon aim's three zoom-factor calls (sub_82240A90 [822397C8]).
  constexpr uint32_t kAimZoomReturns[] = {0x82240B70, 0x82240C1C, 0x82240C34};

  // Players whose gameplay camera was scaled (one per local view).
  std::array<std::atomic<uint32_t>, 4> g_scaled_players{};

  void NoteScaledPlayer(uint32_t player)
  {
    for (auto &p : g_scaled_players)
    {
      uint32_t seen = p.load(std::memory_order_relaxed);
      if (seen == player)
      {
        return;
      }
      if (seen == 0 && p.compare_exchange_strong(seen, player))
      {
        return;
      }
    }
  }

  struct Caller
  {
    std::atomic<uint32_t> lr{0};
    std::atomic<uint32_t> count{0};
  };
  std::array<Caller, 32> g_callers;
  std::atomic<int64_t> g_last_report{0};

  void NoteCaller(uint32_t lr)
  {
    for (Caller &c : g_callers)
    {
      uint32_t seen = c.lr.load(std::memory_order_relaxed);
      if (seen == 0 && c.lr.compare_exchange_strong(seen, lr))
      {
        seen = lr;
      }
      if (seen == lr)
      {
        c.count.fetch_add(1, std::memory_order_relaxed);
        break;
      }
    }
    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
    int64_t last = g_last_report.load(std::memory_order_relaxed);
    if (now - last < 10000 || !g_last_report.compare_exchange_strong(last, now))
    {
      return;
    }
    std::string text;
    for (Caller &c : g_callers)
    {
      const uint32_t a = c.lr.load(std::memory_order_relaxed);
      if (a)
      {
        const bool aim = a == kAimZoomReturns[0] || a == kAimZoomReturns[1] || a == kAimZoomReturns[2];
        text += fmt::format(" {:08X}{} x{}", a,
                            a == kCameraZoomReturn ? " (gameplay camera: scaled)"
                            : aim                  ? " (weapon aim: scaled for scaled players)"
                                                   : "",
                            c.count.exchange(0, std::memory_order_relaxed));
      }
    }
    REXLOG_INFO("pdz_fov: zoom factor callers in the last 10 s (pdz_fov_scale {:.3f}):{}", REXCVAR_GET(pdz_fov_scale),
                text);
  }
} // namespace

double pdz::fov::ScaleFor(uint32_t player)
{
  const double scale = REXCVAR_GET(pdz_fov_scale);
  if (scale == 1.0 || !player)
  {
    return 1.0;
  }
  for (const auto &p : g_scaled_players)
  {
    if (p.load(std::memory_order_relaxed) == player)
    {
      return scale;
    }
  }
  return 1.0;
}

// sub_825AC8E0 [8259D0D8](player): zoom factor (1 = not zoomed).
REX_HOOK_RAW(sub_825AC8E0)
{
  const uint32_t lr = uint32_t(ctx.lr);
  const uint32_t player = ctx.r3.u32;
  const bool camera = lr == kCameraZoomReturn;
  bool aim = false;
  for (uint32_t r : kAimZoomReturns)
  {
    aim = aim || lr == r;
  }
  __imp__sub_825AC8E0(ctx, base);
  if (REXCVAR_GET(pdz_fov_log))
  {
    NoteCaller(lr);
  }
  static std::atomic<int> logged{0};
  const double scale = REXCVAR_GET(pdz_fov_scale);
  if (aim && scale != 1.0)
  {
    ctx.f1.f64 *= pdz::fov::ScaleFor(player);
    return;
  }
  if (camera && scale != 1.0)
  {
    NoteScaledPlayer(player);
    const double before = ctx.f1.f64;
    ctx.f1.f64 *= scale;
    if (logged.fetch_add(1) == 0)
    {
      REXLOG_INFO("pdz_fov_scale {:.3f}: camera zoom factor {:.3f} -> {:.3f} (call from {:08X})", scale, before,
                  ctx.f1.f64, lr);
    }
  }
}
