// Polish 6: longer sun shadows (pdz_shadow_distance_scale; BUG_AUDIT VR4-VR7). Enhancement, off by default (1).
//
// The shadow setup (TU3 sub_8260C0D8 = launch sub_825FC758 + 0xF980, per light and view; the light's vtable entry at
// 82108618) loads three constants and stores them into the light (float index = view + n, view = [82E1BA20]):
//   8260C3A4  lfs f13,-20096(r10)  1600.0 (820EB180)  -> 8260C3CC stfsx f13 = light[424 + view]: the fade start
//   8260C3C4  lfs f27,-32416(r9)   300.0  (82088160)  -> 8260C408 stfsx f27 = light[430 + view]: the near cascade's
//                                                         end (the split)
//   8260C3D4  lfs f26,30612(r8)    2000.0 (820A7794)  -> 8260C420 stfsx f26 = light[432 + view]: the far cascade's end
//                                                         = the shadow cut-off = the fade end
// (light[434 + view] = 0.0017 (820EB184) is stored next to them and not touched.) In the cascade loop each cascade's
// box is the view frustum from the eye to its end (8260C46C reads light[430 / 432 + view] back), fitted by the
// ortho fit sub_8260D518 with the fixed light depth range 0..20000 (820DFCA0; the eye 10000 back): VR6 puts the limit
// of that range at about k = 4. Every receiver reads the light back (sub_82607470 at 82607AD8, sub_82607E08 at
// 826080F4, sub_8260D3C0 at 8260D4C0) and builds its pixel-shader fade constant from it: c.x = 1 / (end - start),
// c.y = start * c.x + 1 (the mask shader then does saturate(z * c.x + c.y)), c.w = the split. So scaling the two
// values in the setup moves the cut-off, the fade and the far box together; nothing else needs a hook.
//
// The constants themselves are shared (2000.0 has 33 readers, some in AI code; 1600.0 at 820EB180 is used elsewhere
// too), so they are never written: two mid-function hooks (midasm_hooks_tu3.toml) scale the registers after the
// loads, before the stores:
//   PdzShadowFadeStart before 8260C3CC (the store of f13): f13 *= k
//   PdzShadowFarEnd    before 8260C3D8 (right after the load of f26): f26 *= k (stored for cascade 1 at 8260C420)
// The near split (f27, 300) is left alone: the near cascade's box, so its texel density, stays as Rare made it. The
// far cascade's box grows k times, so its texels do too (VR6: about 2 cm per texel at 20 m, 2x / 3x coarser at
// k = 2 / 3); the native renderer's pdz_shadow_map_scale 2 renders the shadow maps at twice the size to make up for
// it (native_cvars.cpp). With k = 1 the hooks return at once and touch no register: the game runs as before.
//
// Read on the game thread, once per light and view per frame (two calls a frame in a single-view level): one cvar
// read each, no syscalls; one log line when the value in effect changes.
//
// Polish 6b: the far cascade's bias and soft edges (pdz_shadow_bias_scale, pdz_shadow_far_pcf; PDZRenderResearch
// native\stage2\polish\shadowbias.md).
// The bias is not a render state: the shadow pass (sub_8260C8C0) sets no DepthBias / SlopeScaleDepthBias, the casters
// write plain depth. It is in the receiver, the sun shadow mask shader 34d98195 that sub_82607470 binds (SetPixelShader
// at 826079A8): per pixel it rebuilds the view position from the scene depth, picks the cascade (far = beyond the split
// c8.w: matrix c4-c6, maps t4 / t2; near: c0-c2, t6 / t1), point-samples the map and compares in the cascade's light
// depth d / 20000 (OrthoOffCenter zn 0, zf 20000 = 820DFCA0, in the ortho fit sub_8260D518):
//   z' = z + NdotL * c13.x + c13.y        c13 = {0.0013333, -0.0015334} (a literal: D3D copies it at SetPixelShader)
//   lit = (z' - map <= 0), soft = saturate(1 - 200 * (z' - map))   (200 = the literal c10.w)
// i.e. the receiver is pulled toward the sun by 20000 * (0.0015334 - 0.0013333 NdotL) = 4 cm (facing the sun) to 31 cm
// (edge-on): a slope bias in world units, the same for both cascades. A cascade k times longer has texels k times
// wider (k / m with an m times larger map); the depth error of a sloped receiver inside one texel grows with the texel,
// so the same 4-31 cm loses against it sooner: acne stripes. The fix scales the far cascade's bias only, by
//   t = clamp(pdz_shadow_distance_scale / m * pdz_shadow_bias_scale, 1, 8)
// (m = the native shadow maps' extra resolution, ShadowMapRelativeScale, when the native renderer is the only output;
// else 1: the emulated maps have none), without touching a constant: PdzShadowFarDepthRange stretches the far
// cascade's light depth range from 0..20000 to 0..20000 t (divides the z column of its ortho matrix by t in the setup,
// before the matrix is copied into the cascade table 82FBD6C0 that the caster pass, the mask shader and the forward
// receivers all read). The normalized bias then means t times the centimetres; the casters write and the receivers
// compare in the same stretched units, so nothing else moves (the soft-compare widths, 100 cm for c10.w, grow by t
// too). The near cascade (cascade 0) keeps Rare's 0..20000. At t = 1 the hook returns before touching anything. 24-bit
// depth over 8 x 20000 cm still resolves 0.01 cm.
// PCF: the mask shader point-samples (a filter override in the fetch instruction), so softer far edges need another
// shader: pdz_shadow_far_pcf is native only (native_hlslfix.h PatchShadowFarPcf, applied in native_coissue.cpp). The
// patched shader reads its radius from c13.z, a component of the bias literal the original never reads (0 in the
// literal), which PdzShadowMaskConstants writes after the mask's SetPixelShader while the cvar is above 0 and the
// native renderer is on. The emulated path never reads c13.z.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/runtime.h>

#include "cutscene_dump.h" // pdz_cutscene_frame_dump: the frame's camera matrix
#include "frame_timing.h"  // pdz::timing::Enabled (the caster counter's switch)
#include "native/native.h" // Enabled, ShadowMapRelativeScale; gpu_sync.h NullBackend; pdz::shadow guest helpers

REXCVAR_DEFINE_DOUBLE(pdz_shadow_distance_scale, 1.0, "Render",
                      "Enhancement (polish 6): how far the sun's shadows reach. The game ends them at 20 m and fades "
                      "them out from 16 m (BUG_AUDIT VR5); this scales both, and the far shadow cascade with them: 2 = "
                      "shadows to 40 m fading from 32 m, 4 = 80 m (the light's depth range holds about 4). The near "
                      "cascade (to 3 m) follows with pdz_shadow_split_follow. The far cascade's map keeps its size, so far shadows get "
                      "coarser by the same factor (with the native renderer, pdz_shadow_map_scale 2 doubles the shadow "
                      "maps' resolution). The shadow casters follow (polish 6d): the game draws into the shadow maps "
                      "only the objects within about 14 m of a point 10 m ahead of the camera, and that caster box "
                      "scales too (pdz_shadow_caster_scale: automatically up to x2, within 28 m of a point 20 m ahead), "
                      "so far props cast again; more casters cost CPU time in the shadow pass. Both GPU paths; changes apply on the next frame. 1 = original, "
                      "clamped 1-4")
    .range(1.0, 4.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(pdz_shadow_bias_scale, 1.0, "Render",
                      "Enhancement (polish 6b / shadows 2, with pdz_shadow_distance_scale): extra factor on the far "
                      "sun-shadow cascade's automatic depth bias. The game's bias is fixed in world units (4 cm on "
                      "ground facing the sun to 31 cm edge-on, in the shadow mask shader), tuned for its 20 m cascade; "
                      "even then low-sun slopes show some acne, and the longer cascade shows it out to 60 m. The far "
                      "cascade's bias is x the texel size (pdz_shadow_distance_scale / the native shadow maps' extra "
                      "resolution, pdz_shadow_map_scale, when the native renderer is the only output; at least 1) x "
                      "the longer-shadow factor (1 at distance 1, rising to 4 from distance 2: calibrated on the "
                      "user's 2026-10-27 test, where bias 4 at distance 3 / map 3 removed the acne on the River "
                      "rocks) x this, kept within 1-8 (the near cascade is untouched). 1 = the automatic bias: x4 at "
                      "distance 3 / map 3, exactly the original at the stock settings (distance 1); 0.25-0.5 = less, "
                      "if far shadows look detached from their casters; 2 = more, if stripes remain. The far PCF does "
                      "not need more (shadows 2 sweep: the same picture against the bias with it on or off). Both GPU "
                      "paths; changes apply on the next frame. Range 0.25-4")
    .range(0.25, 4.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(pdz_shadow_far_pcf, 0.0, "Render",
                      "Native renderer only, enhancement (polish 6b): soft edges in the far sun-shadow cascade. The game "
                      "point-samples its shadow maps (one texel decides lit or not), so the far cascade stretched by "
                      "pdz_shadow_distance_scale shows blocky texel edges. Above 0: the shadow mask shader (34d98195) is "
                      "recompiled at its first use with nine bilinear comparison taps (2 x 2 texels each, tent "
                      "weighted) spaced this many shadow-map texels apart in the far cascade: 1 = a smooth edge about "
                      "4 texels wide, 2 = softer. The near cascade keeps the game's single sample. 0 = off (the shader "
                      "is not touched). The shader is patched only if this is above 0 at its first use (set it at "
                      "start; a later change from 0 needs a restart); the radius is read every frame. The emulated GPU "
                      "path has no shader patch layer and draws as before. Range 0-4")
    .range(0.0, 4.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_shadow_split_follow, true, "Render",
                    "Enhancement (polish 6c, part of pdz_shadow_distance_scale): the near sun-shadow cascade's end (3 m, "
                    "the split where the sharp near map hands over to the far one) scales with "
                    "pdz_shadow_distance_scale like the far end, so the two cascades keep the game's proportions (near "
                    "texels 6.7 x finer than far ones instead of 20 x at distance 3) and the visible ring at the split "
                    "moves out with the shadows (9 m at distance 3). The near map then covers k times more ground, "
                    "still finer than the game's own far cascade. Off: the split stays at 3 m whatever the distance. "
                    "Does nothing while the distance scale is 1. Both GPU paths; changes apply on the next frame")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(pdz_shadow_split_blend, 0.25, "Render",
                      "Native renderer only, enhancement (polish 6c, part of pdz_shadow_distance_scale): blends the near "
                      "sun-shadow cascade into the far one over the last part of the near cascade instead of switching "
                      "at one distance (the ring around the player). The width of that band as a fraction of the near "
                      "cascade's end: 0.25 = its last quarter (6.75-9 m at distance 3 with pdz_shadow_split_follow). "
                      "There the shadow mask shader (34d98195) also samples the far map (with pdz_shadow_far_pcf's "
                      "taps when that is on) and mixes the two results. Active only while pdz_shadow_distance_scale is "
                      "above 1; the shader is patched only if this is above 0 and the distance scale above 1 at its first "
                      "use (set both at start); the width is read every frame (0 live = the original switch). The "
                      "emulated GPU path has no shader patch layer and switches as before. 0 = off, range 0-0.75")
    .range(0.0, 0.75)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_shadow_stable, false, "Render",
                    "Enhancement (polish 6c): rotation-stable sun-shadow cascades. The game fits each cascade's shadow "
                    "map to the view and orients it by the camera, so every shadow edge crawls when the camera turns "
                    "or moves (original behaviour). On: the shadow maps keep one orientation in the world, each "
                    "cascade's square covers a sphere around its slice of the view (the same size whatever the "
                    "direction) and moves in whole shadow-map texels, so standing shadows stay still while looking "
                    "around. The squares are about 15-20% wider than the game's fitted boxes (slightly coarser shadows). "
                    "Both GPU paths; changes apply on the next frame. Off = original")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace
{
  // The factor in effect: 1 for 1, anything below it or not a number (the original), at most 4.
  float DistanceScale()
  {
    const double v = REXCVAR_GET(pdz_shadow_distance_scale);
    if (!(v > 1.0))
    {
      return 1.0f;
    }
    return float(std::min(v, 4.0));
  }

  std::atomic<float> g_logged{1.0f};

  // One line per change of the value in effect (the first use included).
  void NoteScale(float k, float far_end)
  {
    if (g_logged.exchange(k, std::memory_order_relaxed) != k)
    {
      REXLOG_INFO("pdz_shadow: sun shadow distance x{:g} (pdz_shadow_distance_scale): cut-off {:g} cm, fade from "
                  "{:g} cm (original 2000 / 1600); the near cascade's split: see pdz_shadow_split_follow",
                  k, far_end, 1600.0f * k);
    }
  }

  // The native shadow maps' extra resolution the far bias can count on: ShadowMapRelativeScale() when the native
  // renderer is the only output (graphics_backend null), else 1 (the emulated maps are the game's size x the plugin's
  // resolution_scale like every other target; with both renderers on, the bias suits the coarser one).
  float MapRelativeScale()
  {
    if (!pdz::gpusync::NullBackend())
    {
      return 1.0f;
    }
    const float m = pdz::native::ShadowMapRelativeScale();
    return m > 1.0f ? m : 1.0f;
  }

  // The longer-shadow factor (shadows 2): 1 at distance 1 (the original), rising linearly to 4 at distance 2 and held
  // there. Rare's 4-31 cm already leave some acne on low-sun slopes inside 20 m; the longer far cascade shows it out to
  // 60 m. 4 = the user's 2026-10-27 calibration (pdz_shadow_bias_scale 4 at distance 3 / map 3, where the texel size
  // equals the stock 4K one, removed the River rock acne).
  float LongShadowFactor(float k)
  {
    return 1.0f + 3.0f * std::clamp(k - 1.0f, 0.0f, 1.0f);
  }

  // The far cascade's bias factor t: texel size (distance scale / map scale, at least 1) x the longer-shadow factor x
  // pdz_shadow_bias_scale, at least 1 (the original) and at most 8. Exactly 1 at the stock settings (distance 1, bias
  // scale 1); x4 at distance 3 / map 3 (the user's calibration); x8 (the cap) for the emulated path at distance 3.
  float FarBiasScale(float k, float m, float p)
  {
    double b = REXCVAR_GET(pdz_shadow_bias_scale);
    if (!(b > 0.0))
    {
      b = 1.0; // not a number
    }
    const double t = std::max(1.0, double(k) / double(m)) * double(p) * std::clamp(b, 0.25, 4.0);
    if (!(t > 1.0))
    {
      return 1.0f;
    }
    return float(std::min(t, 8.0));
  }

  std::atomic<float> g_bias_logged{1.0f};

  // One line per change of the far bias factor in effect (the first use included).
  void NoteBias(float t, float k, float m, float p)
  {
    if (g_bias_logged.exchange(t, std::memory_order_relaxed) != t)
    {
      REXLOG_INFO("pdz_shadow: far cascade bias x{:g} (texel size: distance x{:g} / shadow map x{:g}, x longer-shadow "
                  "factor x{:g} x "
                  "pdz_shadow_bias_scale {:g}): its light depth range 0..{:g} cm (original 0..20000), so its slope "
                  "bias {:g}-{:g} cm (original 4-31); near cascade unchanged",
                  t, k, m, p, REXCVAR_GET(pdz_shadow_bias_scale), 20000.0f * t, 4.0f * t, 30.7f * t);
    }
  }

  void StoreFloat(uint8_t *base, uint32_t address, float value)
  {
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    bits = pdz::shadow::Swap32(bits);
    std::memcpy(const_cast<uint8_t *>(pdz::shadow::GuestPtr(base, address)), &bits, sizeof(bits));
  }

  void Store64(uint8_t *base, uint32_t address, uint64_t value)
  {
    const uint32_t hi = pdz::shadow::Swap32(uint32_t(value >> 32)), lo = pdz::shadow::Swap32(uint32_t(value));
    uint8_t *p = const_cast<uint8_t *>(pdz::shadow::GuestPtr(base, address));
    std::memcpy(p, &hi, sizeof(hi));
    std::memcpy(p + 4, &lo, sizeof(lo));
  }

  // Polish 6c: the split blend band in effect (pdz_shadow_split_blend, a fraction of the near cascade's end): 0 = off,
  // which it always is while the distance scale is 1 (the longer shadows off) or the value is not a number.
  float SplitBlendBand()
  {
    const double v = REXCVAR_GET(pdz_shadow_split_blend);
    if (!(v > 0.0) || DistanceScale() == 1.0f)
    {
      return 0.0f;
    }
    return float(std::min(v, 0.75));
  }
} // namespace

// Mid-function hook (midasm_hooks_tu3.toml), before T 8260C3CC (stfsx f13,r10,r31) in the shadow setup sub_8260C0D8:
// f13 = the fade start (1600.0, loaded at 8260C3A4), about to be stored into light[424 + view].
void PdzShadowFadeStart(PPCRegister &f13)
{
  const float k = DistanceScale();
  if (k == 1.0f)
  {
    return;
  }
  f13.f64 = double(float(f13.f64) * k);
}

// Mid-function hook (midasm_hooks_tu3.toml), before T 8260C3D8 (addi r25,r10,-10560) in the shadow setup
// sub_8260C0D8: f26 = the far cascade's end (2000.0, loaded at 8260C3D4), stored into light[432 + view] by the
// cascade loop (8260C420) and read back for the far box (8260C46C) and by every receiver.
namespace
{
  // Test trail (2026-10-28): every pdz_shadow_* setting is logged once at the session's first shadow setup, and
  // every later change of one (F4 overlay, console, the test script's cvar command, code) logs one line old -> new,
  // from the cvar registry's change callback, so a play test's log shows what was really switched and when. Logging
  // only: nothing reads these values.
  std::mutex g_cvar_log_mutex;
  std::unordered_map<std::string, std::string> g_cvar_last;
  std::once_flag g_cvar_log_once;

  void RegisterShadowCvarLog()
  {
    std::vector<std::string> names;
    for (std::string &name : rex::cvar::ListFlags())
    {
      if (name.rfind("pdz_shadow_", 0) == 0)
      {
        names.push_back(std::move(name));
      }
    }
    // The values are read before taking g_cvar_log_mutex: a change callback runs under the registry's lock and then
    // takes ours, so ours is never held while asking the registry (one lock order).
    std::vector<std::string> values;
    std::string all;
    for (const std::string &name : names)
    {
      values.push_back(rex::cvar::GetFlagByName(name));
      all += fmt::format("{}{} {}", all.empty() ? "" : ", ", name, values.back());
    }
    {
      std::lock_guard lock(g_cvar_log_mutex);
      for (size_t i = 0; i < names.size(); ++i)
      {
        g_cvar_last.emplace(names[i], values[i]); // a change logged meanwhile keeps its newer value
      }
    }
    for (const std::string &name : names)
    {
      rex::cvar::RegisterChangeCallback(name, [](std::string_view n, std::string_view value) {
        std::lock_guard lock(g_cvar_log_mutex);
        std::string &last = g_cvar_last[std::string(n)];
        if (last != value)
        {
          REXLOG_INFO("pdz_shadow: {} {} -> {} (live change)", n, last, value);
          last = std::string(value);
        }
      });
    }
    REXLOG_INFO("pdz_shadow: settings at the first shadow setup: {} (live changes are logged 'old -> new')", all);
  }
} // namespace

void PdzShadowFarEnd(PPCRegister &f26)
{
  std::call_once(g_cvar_log_once, RegisterShadowCvarLog);
  const float k = DistanceScale();
  if (k == 1.0f)
  {
    if (g_logged.load(std::memory_order_relaxed) != 1.0f)
    {
      NoteScale(1.0f, float(f26.f64)); // back to the original (a live change)
    }
    return;
  }
  f26.f64 = double(float(f26.f64) * k);
  NoteScale(k, float(f26.f64));
}

// Mid-function hook (midasm_hooks_tu3.toml), before T 8260C568 (add r11,r29,r28) in the shadow setup sub_8260C0D8,
// right after the ortho fit sub_8260D518 (8260C564) built cascade r28's light projection at r1 + 288 (OrthoOffCenter
// sub_827D7280, row-major: element 10 = 1 / (zn - zf), 14 = zn / (zn - zf); zn 0, zf 20000) and before the loop at
// 8260C5A8 copies it into the cascade table. For the far cascade (r28 = 1) the z column (elements 2, 6, 10, 14) is
// divided by t (FarBiasScale: texel size x the longer-shadow factor x pdz_shadow_bias_scale): the same projection with zf' = zn + t (zf - zn), so the shader's normalized bias is t
// times the centimetres. Cascade 0 (near) and t = 1 return without a write.
void PdzShadowFarDepthRange(PPCRegister &r28, PPCRegister &r1)
{
  if (r28.u32 != 1)
  {
    return;
  }
  const float k = DistanceScale(), m = MapRelativeScale(), p = LongShadowFactor(k);
  const float t = FarBiasScale(k, m, p);
  if (t == 1.0f)
  {
    if (g_bias_logged.load(std::memory_order_relaxed) != 1.0f)
    {
      NoteBias(1.0f, k, m, p); // back to the original (a live change)
    }
    return;
  }
  uint8_t *base = rex::Runtime::instance()->virtual_membase();
  const uint32_t matrix = r1.u32 + 288;
  for (uint32_t i = 2; i < 16; i += 4)
  {
    const uint32_t address = matrix + 4 * i;
    StoreFloat(base, address, pdz::shadow::BitsToFloat(pdz::shadow::Load32(base, address)) / t);
  }
  NoteBias(t, k, m, p);
}

// Mid-function hook (midasm_hooks_tu3.toml), before T 826079AC in the sun shadow mask pass sub_82607470, right after
// its SetPixelShader (826079A8, the mask shader 34d98195; D3D has just copied the shader's literals into the device's
// PS constants, c13 = {0.0013333, -0.0015334, 0, 0}, and marked them dirty). With the native renderer on: c13.z = the
// pdz_shadow_far_pcf radius in texels while it is above 0, c13.w = the pdz_shadow_split_blend band while it is above 0
// and the distance scale above 1 (polish 6c, SplitBlendBand); the native renderer's patched shader reads them, the
// original shader never reads c13.z / w. The PS group c12-c15 is then marked dirty again (bit 63 - 13 / 4 of the PS mask
// at device + 24, as the game marks its own constant writes). Neither: nothing is written; every SetPixelShader restores
// the literal's 0 anyway.
void PdzShadowMaskConstants()
{
  const double radius = REXCVAR_GET(pdz_shadow_far_pcf);
  const float band = SplitBlendBand();
  if ((!(radius > 0.0) && band == 0.0f) || !pdz::native::Enabled())
  {
    return;
  }
  namespace dev = pdz::shadow::dev;
  uint8_t *base = rex::Runtime::instance()->virtual_membase();
  const uint32_t device = pdz::shadow::Load32(base, dev::kDevicePointerGlobal);
  if (!pdz::shadow::IsGuestPointer(device))
  {
    return;
  }
  constexpr uint32_t kMaskBiasRegister = 13;
  if (radius > 0.0)
  {
    StoreFloat(base, device + dev::kPsAluConstants + 16 * kMaskBiasRegister + 8, float(std::min(radius, 4.0)));
  }
  if (band > 0.0f)
  {
    StoreFloat(base, device + dev::kPsAluConstants + 16 * kMaskBiasRegister + 12, band);
  }
  const uint32_t dirty = device + dev::kDirtyMasks + 8; // PS ALU constants: one bit per 4 registers, from bit 63
  Store64(base, dirty, pdz::shadow::Load64(base, dirty) | (uint64_t(1) << (63 - kMaskBiasRegister / 4)));
}

// Polish 6c: the cascade layout (PDZRenderResearch native\stage2\polish\shadows2_cascade.md).
// The setup's cascade loop (8260C3E4-8260C5C4, r28 = cascade) builds, per cascade, the 8 corners of the view frustum
// from the eye's near plane (light[426 / 428 + view] = f25) to the cascade's end (light[430 + view] = f27 = 300 for the
// near cascade, light[432 + view] = f26 for the far one) in view space at r1 + 160 (4 far corners, x y z) and r1 + 208
// (4 near ones), then the ortho fit sub_8260D518 transforms them by the light view at 82FBDAC0 (r26, built once per
// call at 8260C37C by the LookAt sub_827D6470 from the view-space light direction light[1136 + 12 view], eye 10000
// back), takes their 2D hull (sub_8260D5F8), its bounding rectangle, and OrthoOffCenterRH (sub_827D7280) at r1 + 288:
// [0] = 2 / (r - l), [5] = 2 / (t - b), [12] = -(l + r) / (r - l), [13] = -(t + b) / (t - b), the z column = the
// 0..20000 depth range. 8260C588 multiplies the world->view matrix (r23, the setup's argument r5) by the light view
// into the table 82FBD2C0 (world->light), the copy loop puts the projection into 82FBD6C0; the caster pass, the mask
// shader (c0-c7 = view->world x both) and the forward receivers read only those tables. Matrices are row-major for row
// vectors (p' = p M), element [4 row + column].
//
// pdz_shadow_split_follow: PdzShadowSplit scales f27 (the split, 300) by the distance scale before the loop stores it
// (8260C408); every receiver reads the split back from light[430 + view] (c8.w), so nothing else moves.
//
// pdz_shadow_stable: the light view is built in view space (its up vector from the camera's axes) and each box is the
// view slice's bounding rectangle, so the shadow map's texel grid turns and changes size with the camera, and slides
// with it (no snapping): every shadow edge crawls when the camera turns (the original, kept by default). With the
// switch on:
//   PdzShadowStableBasis (before 8260C380, right after the light view is built): turns the light view about its own z
//     axis so that a fixed world axis (Y, else X, else Z: the first one more than ~25 degrees from the sun) projects
//     onto light +y. The light's x / y axes are then fixed in the world; z (the sun) and the depth range do not change.
//   PdzShadowStableBox (before 8260C56C, after the fit and after PdzShadowFarDepthRange at 8260C568): replaces the
//     box with a square of side 2R around the bounding sphere of the cascade's view slice (from the corners at
//     r1 + 160 / 208: the same for any camera direction; R rounded up to 1/16 octave so a changing field of view does
//     not resize it every frame), its corner a whole number of texels of the guest's 1344-texel grid from the world
//     origin's light-space position (the native maps are an integer multiple of 1344, so their texels line up too).
//     Only elements 0, 5, 12 and 13 are written; the z column stays as the fit and the bias hook left it.
// Off: both return before reading anything.
namespace
{
  constexpr double kGuestShadowTexels = 1344.0; // the shadow pass's viewport (8260C93C)

  struct Matrix4
  {
    double m[16]; // row-major, row vectors: m[4 * row + column]
  };

  bool LoadMatrix4(const uint8_t *base, uint32_t address, Matrix4 &out)
  {
    if (!pdz::shadow::IsGuestPointer(address))
    {
      return false;
    }
    for (uint32_t i = 0; i < 16; ++i)
    {
      out.m[i] = double(pdz::shadow::BitsToFloat(pdz::shadow::Load32(base, address + 4 * i)));
      if (!std::isfinite(out.m[i]))
      {
        return false;
      }
    }
    return true;
  }

  double LoadDouble(const uint8_t *base, uint32_t address)
  {
    return double(pdz::shadow::BitsToFloat(pdz::shadow::Load32(base, address)));
  }

  // The near cascade's end factor in effect: the distance scale with pdz_shadow_split_follow on, else 1.
  float SplitFactor()
  {
    if (!REXCVAR_GET(pdz_shadow_split_follow))
    {
      return 1.0f;
    }
    return DistanceScale();
  }

  std::atomic<float> g_split_logged{1.0f};

  // One line per change of the split factor in effect (the first use included).
  void NoteSplit(float f, float split)
  {
    if (g_split_logged.exchange(f, std::memory_order_relaxed) != f)
    {
      REXLOG_INFO("pdz_shadow: near cascade split x{:g} (pdz_shadow_split_follow, distance x{:g}): {:g} cm (original "
                  "300); split blend band {:g} (pdz_shadow_split_blend, native renderer only)",
                  f, DistanceScale(), split, SplitBlendBand());
    }
  }

  std::atomic<bool> g_stable_logged{false};
  std::atomic<uint32_t> g_stable_box_logs{0};
  std::atomic<float> g_stable_radius[2] = {0.0f, 0.0f};

  // One line per change of pdz_shadow_stable in effect.
  void NoteStable(bool on)
  {
    if (g_stable_logged.exchange(on, std::memory_order_relaxed) != on)
    {
      REXLOG_INFO("pdz_shadow: rotation-stable cascades {} (pdz_shadow_stable){}", on ? "on" : "off",
                  on ? ": world-fixed light axes, square boxes around each view slice's bounding sphere, snapped to "
                       "whole shadow-map texels"
                     : ": the game's fit (turns and slides with the camera)");
      g_stable_radius[0].store(0.0f, std::memory_order_relaxed);
      g_stable_radius[1].store(0.0f, std::memory_order_relaxed);
    }
  }
} // namespace

// Mid-function hook (midasm_hooks_tu3.toml), before T 8260C3D0 (lis r10,-32004) in the shadow setup sub_8260C0D8:
// f27 = the near cascade's end (300.0, loaded at 8260C3C4), stored into light[430 + view] by the cascade loop
// (8260C408) and read back for the near box (8260C46C) and by every receiver (the split, c8.w). With
// pdz_shadow_split_follow on and the distance scale k above 1: f27 *= k. Otherwise it returns without touching it.
void PdzShadowSplit(PPCRegister &f27)
{
  const float f = SplitFactor();
  if (f == 1.0f)
  {
    if (g_split_logged.load(std::memory_order_relaxed) != 1.0f)
    {
      NoteSplit(1.0f, float(f27.f64)); // back to the original (a live change)
    }
    return;
  }
  f27.f64 = double(float(f27.f64) * f);
  NoteSplit(f, float(f27.f64));
}

// Mid-function hook (midasm_hooks_tu3.toml), before T 8260C380 (lis r10,-32241) in the shadow setup sub_8260C0D8,
// right after the LookAt sub_827D6470 (8260C37C) built the light view at r26 (82FBDAC0) in view space; r23 = the
// world->view matrix. pdz_shadow_stable only: the light view is turned about its z axis (V' = V Rz: its x / y columns,
// every row) so that a world-fixed axis points along light +y. Off: no read, no write.
void PdzShadowStableBasis(PPCRegister &r23, PPCRegister &r26)
{
  if (!REXCVAR_GET(pdz_shadow_stable))
  {
    if (g_stable_logged.load(std::memory_order_relaxed))
    {
      NoteStable(false);
    }
    return;
  }
  uint8_t *base = rex::Runtime::instance()->virtual_membase();
  Matrix4 view, light;
  if (!LoadMatrix4(base, r23.u32, view) || !LoadMatrix4(base, r26.u32, light))
  {
    return;
  }
  // The sun's direction in the world: light z in view space is the light view's column 2; view->world is the
  // transpose of the world->view rotation, so dir[i] = sum_j view[i][j] * column2[j].
  double dir[3];
  for (int i = 0; i < 3; ++i)
  {
    dir[i] = view.m[4 * i] * light.m[2] + view.m[4 * i + 1] * light.m[6] + view.m[4 * i + 2] * light.m[10];
  }
  const double len = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
  if (!(len > 1e-6))
  {
    return;
  }
  // World Y, else X, else Z: the first axis more than ~25 degrees away from the sun (fixed for a fixed sun).
  int axis = 1;
  if (std::fabs(dir[1]) > 0.9 * len)
  {
    axis = std::fabs(dir[0]) > 0.9 * len ? 2 : 0;
  }
  // That axis in view space is row `axis` of the world->view rotation; its light x / y, through the light view.
  double u[2];
  for (int j = 0; j < 2; ++j)
  {
    u[j] = view.m[4 * axis] * light.m[j] + view.m[4 * axis + 1] * light.m[4 + j] + view.m[4 * axis + 2] * light.m[8 + j];
  }
  const double ulen = std::sqrt(u[0] * u[0] + u[1] * u[1]);
  if (!(ulen > 1e-6))
  {
    return;
  }
  const double a = u[0] / ulen, b = u[1] / ulen; // new x = (b, -a), new y = (a, b) in the old light x / y
  for (uint32_t row = 0; row < 4; ++row)
  {
    const double x = light.m[4 * row], y = light.m[4 * row + 1];
    StoreFloat(base, r26.u32 + 16 * row, float(x * b - y * a));
    StoreFloat(base, r26.u32 + 16 * row + 4, float(x * a + y * b));
  }
  NoteStable(true);
}

// Mid-function hook (midasm_hooks_tu3.toml), before T 8260C56C (mr r5,r26) in the shadow setup sub_8260C0D8, after the
// ortho fit (8260C564) wrote cascade r28's projection at r1 + 288 and PdzShadowFarDepthRange (8260C568) its z column;
// r1 + 160 / + 208 = the cascade's far / near frustum corners in view space, r26 = the light view (turned by
// PdzShadowStableBasis), r23 = world->view. pdz_shadow_stable only: x / y of the projection = a square of side 2R
// around the bounding sphere of the view slice, its corner on the world-fixed grid of 2R / 1344 texels. Off: no read,
// no write.
void PdzShadowStableBox(PPCRegister &r28, PPCRegister &r1, PPCRegister &r23, PPCRegister &r26)
{
  if (!REXCVAR_GET(pdz_shadow_stable) || r28.u32 > 1)
  {
    return;
  }
  uint8_t *base = rex::Runtime::instance()->virtual_membase();
  Matrix4 view, light;
  if (!LoadMatrix4(base, r23.u32, view) || !LoadMatrix4(base, r26.u32, light))
  {
    return;
  }
  // The slice: far corners at depth L, radius rf from the view axis; near corners at depth n, radius rn.
  double far_depth = 0.0, far_radius = 0.0, near_depth = 1e30, near_radius = 0.0;
  for (uint32_t c = 0; c < 4; ++c)
  {
    const uint32_t f = r1.u32 + 160 + 12 * c, n = r1.u32 + 208 + 12 * c;
    const double fx = LoadDouble(base, f), fy = LoadDouble(base, f + 4), fz = LoadDouble(base, f + 8);
    const double nx = LoadDouble(base, n), ny = LoadDouble(base, n + 4), nz = LoadDouble(base, n + 8);
    far_depth = std::max(far_depth, -fz);
    far_radius = std::max(far_radius, std::sqrt(fx * fx + fy * fy));
    near_depth = std::min(near_depth, -nz);
    near_radius = std::max(near_radius, std::sqrt(nx * nx + ny * ny));
  }
  if (!std::isfinite(far_depth + far_radius + near_depth + near_radius) || !(near_depth >= 0.0) ||
      !(far_depth > near_depth) || !(far_radius > 0.0))
  {
    return;
  }
  // The sphere through both rings, its centre on the view axis at depth zc (kept inside the slice).
  double zc = (far_depth * far_depth + far_radius * far_radius - near_depth * near_depth - near_radius * near_radius) /
              (2.0 * (far_depth - near_depth));
  zc = std::clamp(zc, near_depth, far_depth);
  double radius = std::max(std::hypot(far_depth - zc, far_radius), std::hypot(zc - near_depth, near_radius));
  radius = std::exp2(std::ceil(std::log2(radius) * 16.0) / 16.0);
  // Its centre (0, 0, -zc) and the world origin (world->view row 3) in light space (x, y).
  const double cx = -zc * light.m[8] + light.m[12], cy = -zc * light.m[9] + light.m[13];
  double ox = light.m[12], oy = light.m[13];
  for (int i = 0; i < 3; ++i)
  {
    ox += view.m[12 + i] * light.m[4 * i];
    oy += view.m[12 + i] * light.m[4 * i + 1];
  }
  const double texel = 2.0 * radius / kGuestShadowTexels;
  const double left = ox + std::round((cx - radius - ox) / texel) * texel; // u = (x - left) / 2R
  const double top = oy + std::round((cy + radius - oy) / texel) * texel;  // v = (top - y) / 2R
  const uint32_t matrix = r1.u32 + 288;
  StoreFloat(base, matrix + 4 * 0, float(1.0 / radius));
  StoreFloat(base, matrix + 4 * 5, float(1.0 / radius));
  StoreFloat(base, matrix + 4 * 12, float(-(left + radius) / radius));
  StoreFloat(base, matrix + 4 * 13, float(-(top - radius) / radius));
  NoteStable(true);
  const uint32_t cascade = r28.u32;
  if (g_stable_radius[cascade].exchange(float(radius), std::memory_order_relaxed) != float(radius))
  {
    const uint32_t n = g_stable_box_logs.fetch_add(1, std::memory_order_relaxed);
    if (n < 16)
    {
      REXLOG_INFO("pdz_shadow: stable {} cascade: view slice {:g}-{:g} cm, box {:g} cm square, guest texel {:g} cm{}",
                  cascade == 0 ? "near" : "far", near_depth, far_depth, 2.0 * radius, texel,
                  n == 15 ? " (further size changes not logged)" : "");
    }
  }
}

// Polish 6d: the shadow casters follow the distance (pdz_shadow_distance_scale; PDZRenderResearch
// native\stage2\polish\shadows2_casters.md).
// The caster passes (sub_8260C8C0 -> sub_8261D728 -> the callback sub_8261CA68 -> sub_824119A0 in renderer mode 2,
// flags 0x4001) draw only the objects whose shadow bit for the view is set; the visibility pass sets it per view in
// sub_82628888: it builds a caster box with sub_82629B78, and its room / object loops (sub_82627DE8, sub_826282A0,
// sub_82628568) set an object's shadow bit (object + 38, copied to the render table 82D73900 bits 4-7) only if the
// object's bounding sphere (object + 48, radius + 60) touches that box (sub_82727B50, sphere against an oriented box).
// The box is three values the shadow setup sub_8260C0D8 stores in the light (r31; idx = 2 view + [82E1BA20], M = r28,
// the camera matrix the setup transforms with):
//   8260C1BC  P0 = M (0, 0, -1000)             -> light + 1472 + 12 idx: the box's centre, 10 m ahead (820EB18C)
//   8260C1DC  P1 = M (10000 L + (0, 0, -1000)) -> light + 1568 + 12 idx: P0 + 100 m along the light (the 1000 is
//                                                 8208C2F4, L = light + 1136)
//   8260C200  R = 1414.0 (820EB188)            -> light[416 + idx]: the half width across the light (1000 x sqrt 2)
// sub_82629B78 turns them into a box around P0: axis P0 - P1 (half length 2 |P0 - P1| = 200 m, no limit in practice),
// +-R across it. So only objects within about 14 m (+ their radius) of a point 10 m ahead of the camera cast, whatever
// the cut-off: with the far cascade stretched to 60 m, props from about 25 m out lost their shadows. Characters take
// another path (sub_825A8688, no box) and kept theirs. Only sub_82629B78 reads the three values (the engine has no
// other reader of light + 1472 / 1568 / [416 + idx]), so PdzShadowCasterBox rewrites them right after the setup
// stored them: the centre k x 10 m ahead (recomputed with M as the setup does), P1 moved with it (the light axis and
// its length are kept) and R x k: the box keeps Rare's proportions to the k times longer cascade. At k = 1 it returns
// before any read or write. No camera / LOD distance cut-off was found for props (their cull state, vtable + 128 =
// sub_825173C8, only checks visibility flags), so normal rendering is untouched.
// Cost: the casters grow with the box's cross-section (up to k^2 x the props in it; the level's static geometry and
// the characters are not in this box), and every caster is drawn into both cascades (the near one clips it away on
// the GPU, but its draw is paid on the CPU). With pdz_frame_timing on, one 'pdz_shadow: casters' line every
// kCasterLogPasses shadow passes (any k): caster objects per pass (sub_824119A0's selection count in the caster
// passes, both cascades), the shadow pass's own time on the game thread (sub_8260C8C0 entry to exit: the guest's
// caster draws plus our draw hooks inside them) and PdzShadowCasterBox's time. Off: one cvar load per hook call.
namespace
{
  constexpr uint32_t kCasterBoxAhead = 0x820EB18C; // -1000.0: the box centre's view z (read, never written)
  constexpr float kCasterBoxHalfWidth = 1414.0f;   // 820EB188, for the log lines
  constexpr uint32_t kCasterLogPasses = 600;

  std::atomic<float> g_box_logged{1.0f};

  // One line per change of the caster box scale in effect (the first use included).
  void NoteCasterBox(float k)
  {
    if (g_box_logged.exchange(k, std::memory_order_relaxed) != k)
    {
      REXLOG_INFO("pdz_shadow: sun shadow casters x{:g} (pdz_shadow_caster_scale): caster box centre {:g} cm ahead "
                  "of the camera, half width {:g} cm across the light (original 1000 / 1414)",
                  k, 1000.0f * k, kCasterBoxHalfWidth * k);
    }
  }

  // The pdz_frame_timing counter (game thread: the setup, the shadow pass and the object renderer all run there).
  struct CasterStats
  {
    bool in_pass = false;
    std::chrono::steady_clock::time_point pass_start;
    uint64_t pass_objects = 0; // this pass, both cascades
    uint32_t passes = 0;
    uint64_t objects = 0, objects_max = 0;
    double pass_ms = 0.0, pass_ms_max = 0.0;
    uint64_t box_calls = 0;
    double box_us = 0.0;
  };
  CasterStats g_casters;

  double MsSince(std::chrono::steady_clock::time_point t0)
  {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  }
} // namespace

// Mid-function hook (midasm_hooks_tu3.toml), before T 8260C204 (addi r11,r10,95) in the shadow setup sub_8260C0D8,
// right after the caster box's last value was stored (8260C200 stfsx f0,r11,r31: R = 1414.0 at light + r11):
// r31 = the light, r10 = idx (P0 at light + 1472 + 12 idx, P1 at light + 1568 + 12 idx), r28 = the camera matrix the
// setup transformed P0 / P1 with (sub_827D15A8: row vectors, v M divided by w). With k > 1: P0 = M (0, 0, -1000 k),
// P1 += the same move, R *= k. k = 1: returns at once.
REXCVAR_DEFINE_DOUBLE(pdz_shadow_caster_scale, 0.0, "Render",
                      "Enhancement (shadows 2, with pdz_shadow_distance_scale): how far the sun-shadow caster box "
                      "grows with the longer shadows (the game draws into the shadow maps only the objects within about "
                      "14 m of a point 10 m ahead of the camera; this scales both). Every caster is drawn into both "
                      "cascades on the game thread: at distance 3 the full x3 box drew 230-340 objects per pass in the "
                      "31-bot Urban match (stock 36-55) and cost about 2 ms of game thread and 1.5 ms of GPU, too much "
                      "for 4K60. 0 = automatic: the distance scale, at most 2 (props cast to about 48 m, where the "
                      "longer shadows start to fade at distance 3); 1-4 = this factor, at most the distance scale (3 "
                      "at distance 3 = every prop to 60 m). Both GPU paths; changes apply on the next frame")
    .range(0.0, 4.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace
{
  // The caster box factor in effect: 1 at distance 1 (the original, no write), else pdz_shadow_caster_scale (0 =
  // automatic: the distance scale, at most kAutoCasterScale), never above the distance scale.
  constexpr float kAutoCasterScale = 2.0f;
  float CasterScale()
  {
    const float k = DistanceScale();
    if (k == 1.0f)
    {
      return 1.0f;
    }
    const double v = REXCVAR_GET(pdz_shadow_caster_scale);
    const float c = v >= 1.0 ? float(std::min(v, 4.0)) : kAutoCasterScale;
    return std::clamp(c, 1.0f, k);
  }
} // namespace

void PdzShadowCasterBox(PPCRegister &r31, PPCRegister &r10, PPCRegister &r11, PPCRegister &r28)
{
  // cutscene_dump.h: the frame's camera matrix, read only (pdz_cutscene_frame_dump; one load while off).
  pdz::csdump::OnShadowCamera(r28.u32);
  const float k = CasterScale();
  if (k == 1.0f)
  {
    if (g_box_logged.load(std::memory_order_relaxed) != 1.0f)
    {
      NoteCasterBox(1.0f); // back to the original (a live change)
    }
    return;
  }
  const bool timed = pdz::timing::Enabled();
  const auto t0 = timed ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
  const uint32_t light = r31.u32, matrix = r28.u32, idx = r10.u32;
  if (!pdz::shadow::IsGuestPointer(light) || !pdz::shadow::IsGuestPointer(matrix) || idx >= 16)
  {
    return;
  }
  uint8_t *base = rex::Runtime::instance()->virtual_membase();
  auto load = [base](uint32_t address) { return pdz::shadow::BitsToFloat(pdz::shadow::Load32(base, address)); };
  const float z = load(kCasterBoxAhead) * k; // -1000 k
  float row2[4], row3[4];
  for (uint32_t i = 0; i < 4; ++i)
  {
    row2[i] = load(matrix + 32 + 4 * i);
    row3[i] = load(matrix + 48 + 4 * i);
  }
  const float w = z * row2[3] + row3[3];
  if (!(std::fabs(w) > 1e-6f))
  {
    return;
  }
  const uint32_t p0 = light + 1472 + 12 * idx, p1 = light + 1568 + 12 * idx, radius = light + r11.u32;
  for (uint32_t i = 0; i < 3; ++i)
  {
    const float centre = (z * row2[i] + row3[i]) / w;
    const float move = centre - load(p0 + 4 * i);
    StoreFloat(base, p0 + 4 * i, centre);
    StoreFloat(base, p1 + 4 * i, load(p1 + 4 * i) + move);
  }
  StoreFloat(base, radius, load(radius) * k);
  NoteCasterBox(k);
  if (timed)
  {
    g_casters.box_us += 1000.0 * MsSince(t0);
    ++g_casters.box_calls;
  }
}

// Mid-function hook (midasm_hooks_tu3.toml), before T 8260C8F4 (lwz r31,8(r16)) in the shadow pass sub_8260C8C0,
// after its prologue: the pass's start (pdz_frame_timing only).
void PdzShadowPassBegin()
{
  if (!pdz::timing::Enabled())
  {
    g_casters.in_pass = false;
    return;
  }
  g_casters.in_pass = true;
  g_casters.pass_objects = 0;
  g_casters.pass_start = std::chrono::steady_clock::now();
}

// Mid-function hook (midasm_hooks_tu3.toml), before T 82411BEC (li r11,1) in the object renderer sub_824119A0, after
// its selection loop: r20 = the objects it is about to draw, r16 = the renderer mode (2 in the caster passes; set by
// sub_8261D310), r15 = the pass flags (0x4001 there). Adds them to the shadow pass in progress (pdz_frame_timing only).
void PdzShadowCasterCount(PPCRegister &r15, PPCRegister &r16, PPCRegister &r20)
{
  if (!g_casters.in_pass || r16.u32 != 2 || (r15.u32 & 0xFFFFu) != 0x4001u)
  {
    return;
  }
  g_casters.pass_objects += r20.u32;
}

// Mid-function hook (midasm_hooks_tu3.toml), before T 8260CF5C (addi r1,r1,640) in sub_8260C8C0, the epilogue both of
// its paths reach: the pass's end; every kCasterLogPasses passes one 'pdz_shadow: casters' line.
void PdzShadowPassEnd()
{
  CasterStats &s = g_casters;
  if (!s.in_pass)
  {
    return;
  }
  s.in_pass = false;
  const double ms = MsSince(s.pass_start);
  s.pass_ms += ms;
  s.pass_ms_max = std::max(s.pass_ms_max, ms);
  s.objects += s.pass_objects;
  s.objects_max = std::max(s.objects_max, s.pass_objects);
  if (++s.passes < kCasterLogPasses)
  {
    return;
  }
  const float k = CasterScale();
  PDZ_LOG_DEFERRED("pdz_shadow: casters (last {} shadow passes) | caster box x{:g} (pdz_shadow_caster_scale): caster box {:g} cm ahead, half width "
              "{:g} cm | caster objects per pass avg {:.1f} max {} (drawn into both cascades) | shadow pass ms avg "
              "{:.3f} max {:.3f} (game thread, sub_8260C8C0) | box hook us avg {:.2f} ({} calls)",
              s.passes, k, 1000.0f * k, kCasterBoxHalfWidth * k, double(s.objects) / s.passes, s.objects_max,
              s.pass_ms / s.passes, s.pass_ms_max, s.box_calls ? s.box_us / double(s.box_calls) : 0.0, s.box_calls);
  s = CasterStats{};
}
