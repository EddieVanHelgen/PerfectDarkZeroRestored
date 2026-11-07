// Transparency vs water (0.9.2, Rare's bug; pdz_fix_transparency_water, on by default, F1 toggles it).
//
// Rare's transparent stage sub_826216D0 (traced in Nightclub's harbour, overnight 092's follow-up) runs, in order:
//   1. sub_82622510 (returns to 82621ABC): a bucket of meshes, the water's first layer (PS ccf49f44) among them;
//   2. sub_82603AC0 (returns to 82621AE0): the additive glows (82608510; the street-lamp halos, PS d5ce1e70);
//   3. sub_826F21D0: a resolve of the scene colour (with the glows in it) to 1D700000;
//   4. sub_82622510 (returns to 82621B68): the same water meshes again, the second layer (PS a2917857, the
//      refraction fog), which composites that copy over the water area.
// The glows write no depth, so step 4 paints over every glow in front of the water: looking at the sea through a
// street-lamp halo, the water covers the halo (play testing, 7 Nov). On a real Xbox 360 too (the emulator path draws the
// same calls in the same order).
// The fix: the glow call of step 2 is held back (its eight arguments, all from sub_826216D0's callee-saved
// registers, are kept) and made right after step 4 returns, before the next resolve. The glows are then drawn over
// the finished water, depth-tested against the same scene depth; nothing else moves. Should step 4 not run in a
// frame, the held call is made when sub_826216D0 returns, so no glow is ever lost.
//
// Diagnostic stage: pdz_transparency_trace_frame = N logs, for the one game frame whose counter (82D6BFD8 + 136,
// the Swap count) reaches N, every entry of Rare's mesh drawer sub_82710CC0 (the water layers, the alpha meshes)
// and of the glow / particle pass sub_826216D0 (-> 82603AC0 -> 82608510, the additive street-light glows), each
// with its guest call chain, so the frame's own order can be read.
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>

#include <imgui.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/keybinds.h>

#include <fmt/format.h>

#include "../game_cvars.h"

REX_EXTERN(__imp__sub_82710CC0);
REX_EXTERN(__imp__sub_826216D0);
REX_EXTERN(__imp__sub_82603AC0);
REX_EXTERN(__imp__sub_82622510);

namespace
{
  constexpr uint32_t kFrameCounter = 0x82D6BFD8 + 136; // the game's frame counter (one per Swap; test_mp_setup.cpp)

  bool IsGuestPointer(uint32_t a) { return a >= 0x00010000u && a < 0xFFFF0000u; }

  uint32_t Load32(const uint8_t *base, uint32_t a)
  {
    return _byteswap_ulong(*reinterpret_cast<const uint32_t *>(base + a));
  }

  uint32_t Frame(const uint8_t *base)
  {
    return Load32(base, kFrameCounter);
  }

  // Return addresses on the guest stack (back chain at +0, saved LR at -8 of the caller's frame).
  std::string Callers(const PPCContext &ctx, const uint8_t *base, int depth = 10)
  {
    std::string chain = fmt::format("{:08X}", uint32_t(ctx.lr));
    uint32_t sp = ctx.r1.u32;
    for (int i = 0; i < depth && IsGuestPointer(sp); ++i)
    {
      const uint32_t next = Load32(base, sp);
      if (!IsGuestPointer(next) || next <= sp || next - sp > 0x8000 || (next & 0xFFFF) == 0)
      {
        break;
      }
      chain += fmt::format(" < {:08X}", Load32(base, next - 8));
      sp = next;
    }
    return chain;
  }

  int g_seq = 0;
  uint32_t g_frame = 0;

  constexpr uint32_t kGlowCallReturn = 0x82621AE0;    // sub_826216D0's call of the glow pass (step 2)
  constexpr uint32_t kWaterLayer2Return = 0x82621B68; // its bucket call that draws the water's second layer (step 4)

  uint32_t Frame(const uint8_t *base); // below

  // pdz_transparency_test_alternate (testing): the fix on odd game frames only, so two consecutive captures with a
  // still camera show the fix's own change.
  bool FixOn(const uint8_t *base)
  {
    if (!REXCVAR_GET(pdz_game_fixes) || !REXCVAR_GET(pdz_fix_transparency_water))
    {
      return false;
    }
    return !REXCVAR_GET(pdz_transparency_test_alternate) || (Frame(base) & 1u);
  }

  struct HeldGlows
  {
    bool pending = false;
    uint64_t r[8] = {}; // r3..r10
  } g_held;
  uint64_t g_moved = 0, g_flushed = 0;

  void RunHeldGlows(PPCContext &ctx, uint8_t *base, const char *where)
  {
    if (!g_held.pending)
    {
      return;
    }
    g_held.pending = false;
    rex::CallFrame frame(ctx);
    frame.ctx.r1.u64 = (ctx.r1.u32 - 0x800) & ~0xFu; // a frame of its own below the caller's
    uint64_t *regs[8] = {&frame.ctx.r3.u64, &frame.ctx.r4.u64, &frame.ctx.r5.u64, &frame.ctx.r6.u64,
                         &frame.ctx.r7.u64, &frame.ctx.r8.u64, &frame.ctx.r9.u64, &frame.ctx.r10.u64};
    for (int i = 0; i < 8; ++i)
    {
      *regs[i] = g_held.r[i];
    }
    __imp__sub_82603AC0(frame.ctx, base);
    ++g_moved;
    if ((g_moved & (g_moved - 1)) == 0) // 1, 2, 4, 8, ...
    {
      REXLOG_INFO("pdz_fix_transparency_water: the glows drawn after the water's second layer ({}; {} frames so far, "
                  "{} by the end-of-stage flush)",
                  where, g_moved, g_flushed);
    }
  }

  uint32_t g_target = 0;       // one-shot frames from pdz::transwater::TraceFrame (pdz_trace_on_mark), 0: none
  uint32_t g_target_count = 1; // ... this many game frames from g_target
  uint32_t g_last_logged = 0;

  bool Tracing(const uint8_t *base)
  {
    const int32_t want = REXCVAR_GET(pdz_transparency_trace_frame);
    const uint32_t target = g_target ? g_target : want > 0 ? uint32_t(want) : 0;
    if (!target)
    {
      return false;
    }
    const uint32_t count = g_target ? g_target_count : 1u;
    const uint32_t f = Frame(base);
    if (g_frame == 0 && f >= target)
    {
      g_frame = f;
      g_seq = 0;
      g_last_logged = f;
      REXLOG_INFO("pdz_transwater: trace of frame {} begins ({} frame(s))", f, count);
    }
    if (g_target && g_frame && f >= g_frame + count)
    {
      REXLOG_INFO("pdz_transwater: trace of frame {} ends", g_frame + count - 1);
      g_target = 0; // one-shot done; the cvar's own frame stays traced once
      g_frame = 0;
      return false;
    }
    const bool on = g_frame != 0 && f >= g_frame && f < g_frame + count;
    if (on && f != g_last_logged)
    {
      g_last_logged = f;
      REXLOG_INFO("pdz_transwater: trace of frame {} begins (frame {} of {})", f, f - g_frame + 1, count);
    }
    return on;
  }
} // namespace

REX_HOOK_RAW(sub_82710CC0)
{
  static bool first = true;
  if (first && REXCVAR_GET(pdz_transparency_trace_frame) > 0)
  {
    first = false;
    REXLOG_INFO("pdz_transwater: sub_82710CC0 hooked; frame counter now {}", Frame(base));
  }
  const bool t = Tracing(base);
  const int seq = t ? ++g_seq : 0;
  if (t)
  {
    // The native renderer draws inline with the record thread off (PDZ_PLAY_TRACE.bat), so the frame trace's draw
    // lines between this line and its "ends" line are this call's draws.
    REXLOG_INFO("pdz_transwater: #{} mesh sub_82710CC0 r3 {:08X} r4 {:08X} r5 {:08X} r6 {:08X} | {}", seq, ctx.r3.u32,
                ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, Callers(ctx, base));
  }
  __imp__sub_82710CC0(ctx, base);
  if (t)
  {
    REXLOG_INFO("pdz_transwater: #{} mesh ends", seq);
  }
}

REX_HOOK_RAW(sub_826216D0)
{
  static bool first = true;
  if (first && REXCVAR_GET(pdz_transparency_trace_frame) > 0)
  {
    first = false;
    REXLOG_INFO("pdz_transwater: sub_826216D0 hooked; frame counter now {}", Frame(base));
  }
  const bool t = Tracing(base);
  if (t)
  {
    REXLOG_INFO("pdz_transwater: #{} GLOW PASS begins sub_826216D0 r3 {:08X} r4 {:08X} | {}", ++g_seq, ctx.r3.u32,
                ctx.r4.u32, Callers(ctx, base));
  }
  __imp__sub_826216D0(ctx, base);
  if (g_held.pending)
  {
    ++g_flushed;
    RunHeldGlows(ctx, base, "the stage's end: no second water layer this frame");
  }
  if (t)
  {
    REXLOG_INFO("pdz_transwater: #{} GLOW PASS ends", ++g_seq);
  }
}

// Step 4's bucket: the held glows right after it.
REX_HOOK_RAW(sub_82622510)
{
  const uint32_t lr = uint32_t(ctx.lr);
  const bool t = Tracing(base);
  if (t)
  {
    REXLOG_INFO("pdz_transwater: #{} BUCKET sub_82622510 from {:08X} begins", ++g_seq, lr);
  }
  __imp__sub_82622510(ctx, base);
  if (t)
  {
    REXLOG_INFO("pdz_transwater: #{} BUCKET from {:08X} ends", ++g_seq, lr);
  }
  if (lr == kWaterLayer2Return && g_held.pending)
  {
    RunHeldGlows(ctx, base, "after the second water layer");
  }
}

REX_HOOK_RAW(sub_82603AC0)
{
  if (uint32_t(ctx.lr) == kGlowCallReturn && FixOn(base) && !g_held.pending)
  {
    const uint64_t *regs[8] = {&ctx.r3.u64, &ctx.r4.u64, &ctx.r5.u64, &ctx.r6.u64,
                               &ctx.r7.u64, &ctx.r8.u64, &ctx.r9.u64, &ctx.r10.u64};
    for (int i = 0; i < 8; ++i)
    {
      g_held.r[i] = *regs[i];
    }
    g_held.pending = true;
    if (Tracing(base))
    {
      REXLOG_INFO("pdz_transwater: #{} GLOWS held for after the second water layer", ++g_seq);
    }
    return;
  }
  const bool t = Tracing(base);
  if (t)
  {
    REXLOG_INFO("pdz_transwater: #{} GLOWS begin sub_82603AC0 r3 {:08X} r4 {:08X} r5 {:08X} | {}", ++g_seq, ctx.r3.u32,
                ctx.r4.u32, ctx.r5.u32, Callers(ctx, base));
  }
  __imp__sub_82603AC0(ctx, base);
  if (t)
  {
    REXLOG_INFO("pdz_transwater: #{} GLOWS end", ++g_seq);
  }
}

// Water loop (0.9.2; pdz_fix_water_loop, on by default, Insert toggles it; the fix itself is in the native renderer).
// Rare's water animations are stacks of frames (Nightclub's sea: 16 ripple normal maps, DXN 128x128x16; the
// caustics: DXT1 256x256x32) played as loops: the layer coordinate (the sea: VS c35.w) moves with the clock and Rare
// wraps it by 1.0 every few seconds (the sea: 0.25 a second, every 4 s). The fetch constants say repeat on the layer
// axis, but the renderer clamped a stacked texture's layer to the first / last layer (as the SDK does), so at each
// wrap the animation froze for a moment and then jumped back: the snap seen on the sea, the pool, the caustics
// and running water (draw dump of the harbour, 7 Nov: the layer coordinate is the only constant that jumps). The fix
// (native_draw.cpp WaterLoopMask, native_hlslfix.h pdzTfetch3DS): those slots' layers wrap (the last blends into the
// first), so w and w - 1 read the same. Rare's data and the emulator are untouched. Earlier tries: Rare's wave phase
// scale 0.98 -> 1.0 (no visible change, dropped); a +64 mark on the sea's texture matrix only (replaced by the
// per-fetch rule, which covers every such stack).

namespace pdz::transwater
{
  // pdz_trace_on_mark (native_present.cpp): trace these game frames once.
  void TraceFrame(uint32_t frame, uint32_t count)
  {
    g_target_count = count ? count : 1u;
    g_target = frame;
    g_frame = 0;
  }

  uint32_t CurrentFrame(const uint8_t *base) { return Frame(base); }
} // namespace pdz::transwater

// F1 (bind_transparency_water_ab) and its on-screen label. The TU3 build doesn't compile live_toggles.cpp (its A/B
// keys aren't ported), so the key and the label live here; OnCreateDialogs calls Init (perfectdarkzerorecomp_app.h).
namespace
{
  using LabelClock = std::chrono::steady_clock;
  std::mutex g_label_mutex;
  std::string g_label;
  LabelClock::time_point g_label_until;

  class LabelDialog final : public rex::ui::ImGuiDialog
  {
  public:
    explicit LabelDialog(rex::ui::ImGuiDrawer *drawer) : ImGuiDialog(drawer) {}

  protected:
    void OnDraw(ImGuiIO &io) override
    {
      std::string text;
      {
        std::lock_guard<std::mutex> lock(g_label_mutex);
        if (LabelClock::now() >= g_label_until)
        {
          return;
        }
        text = g_label;
      }
      ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.20f), ImGuiCond_Always,
                              ImVec2(0.5f, 0.0f));
      ImGui::SetNextWindowBgAlpha(0.75f);
      constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                          ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoFocusOnAppearing |
                                          ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings;
      if (ImGui::Begin("##pdz_transwater_label", nullptr, kFlags))
      {
        ImGui::TextUnformatted(text.c_str());
      }
      ImGui::End();
    }
  };
  std::unique_ptr<LabelDialog> g_dialog;
} // namespace

namespace pdz::transwater
{
  void ShowStatus(const std::string &text, int seconds)
  {
    REXLOG_INFO("pdz_transwater: label: {}", text);
    std::lock_guard<std::mutex> lock(g_label_mutex);
    g_label = text;
    g_label_until = LabelClock::now() + std::chrono::seconds(seconds);
  }

  void Init(rex::ui::ImGuiDrawer *drawer)
  {
    g_dialog = std::make_unique<LabelDialog>(drawer);
    rex::ui::RegisterBind("bind_transparency_water_ab", "F1",
                          "Switch the transparency vs water fix (pdz_fix_transparency_water) on/off (A/B)", []
                          {
      static auto last = LabelClock::time_point{};
      const auto now = LabelClock::now();
      if (now - last < std::chrono::milliseconds(300))
      {
        return; // a held key repeats
      }
      last = now;
      const bool on = !REXCVAR_GET(pdz_fix_transparency_water);
      REXCVAR_SET(pdz_fix_transparency_water, on);
      ShowStatus(on ? std::string("Transparency vs water fix ON (glows in front of the water)")
                    : std::string("Transparency vs water fix OFF (Rare's original order)"),
                 3); });
    rex::ui::RegisterBind("bind_water_loop_ab", "Insert",
                          "Switch the water animation loop fix (pdz_fix_water_loop) on/off (A/B)", []
                          {
      static auto last = LabelClock::time_point{};
      const auto now = LabelClock::now();
      if (now - last < std::chrono::milliseconds(300))
      {
        return; // a held key repeats
      }
      last = now;
      const bool on = !REXCVAR_GET(pdz_fix_water_loop);
      REXCVAR_SET(pdz_fix_water_loop, on);
      ShowStatus(on ? std::string("Water loop fix ON (water animations loop smoothly)")
                    : std::string("Water loop fix OFF (water animations jump back every few seconds)"),
                 3); });
  }
} // namespace pdz::transwater
