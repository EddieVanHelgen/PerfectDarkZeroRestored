// Stage 2 M5: guest vblank pacing on the null GPU backend (pdz_null_refresh_hz), the cutscene step at high frame
// rates (pdz_cutscene_step) and the unattended game-speed check (pdz_game_clock_log). game_pace.cpp.
//
// pdz_null_refresh_hz = N (default 0 = off) with graphics_backend null and vsync on: the SDK's vsync worker paces the
// guest vblanks at N Hz (rex::graphics::null_gpu::SetGuestVblankHz, SDK patch sdk-14; the guest's video mode still
// reports video_mode_refresh_rate), the presentation interval is 1 vblank (pdz_fps60_unlock's patch), so the game's
// presentation gate, and with it the Swap and the present, run at up to N frames/s. The game's update is variable-step
// on the CPU timebase (sub_82140500, dt capped at 0.1 s), so gameplay keeps real-time speed at any rate; the cutscene
// update (sub_8221ED68) is the exception: when the picture is not behind its clock it steps a fixed 1/60 s, which runs
// a cutscene at fps/60 (2x at 120 fps). pdz_cutscene_step fixes that step (or drops the vblanks to 60 Hz while a
// cutscene plays). See C:\Games\PDZRenderResearch\native\stage2\m5\m5_framerate_research.md and m5_pace.md.
//
// Front-end pacing (pdz_fix_frontend_pacing, default on; pdz_frontend_120, default off; m5_frontend.md): outside
// state 21 the present step sub_82141788 spins until the game's "vblanks since the gate callback was last seen"
// counter reaches 2. That counter only moves at vblanks while the game thread's Swaps reach the command processor at
// once, so two Swaps go through back to back and the third waits about four vblanks: 64 / 50 ms every third frame
// (36-46 fps at vsync on). The fix (mid-function hook PdzFrontendPace, both GPU backends, vsync on only) replaces
// that spin: a front-end frame is presented N guest vblanks after the previous one (N = the presentation interval,
// 1 with pdz_fps60_unlock; x round(rate / 60) while pdz_null_refresh_hz is in effect, unless pdz_frontend_120 is on).
#pragma once

#include <cstdint>
#include <string>

namespace pdz::pace
{
  // App setup (gpu_sync.cpp OnBackendSelected, after the GPU plugin is loaded and the null backend latched, before the
  // GPU's presentation setup starts the vsync worker and the presenter's vblank thread): latches pdz_null_refresh_hz
  // and starts the game clock log thread when it is wanted.
  void ChooseAtStartup(bool null_backend);

  // True while pdz_null_refresh_hz is in effect (latched at startup).
  bool Active();

  // The paced guest vblank rate in Hz, 0 when off.
  uint32_t RefreshHz();

  // The frame rate the current scene is paced at: RefreshHz / the guest vblank divider (pdz_sp_120: 60 in a
  // multiplayer level; pdz_splitscreen_cap: 60 / 40 / 30 while local players share the screen), 0 when off.
  uint32_t EffectiveHz();

  // game_patches.cpp: the presentation interval patch (pdz_fps60_unlock) is applied whenever this is true.
  inline bool ForcesInterval1() { return Active(); }

  // The pdz_clock SUMMARY line (test exit); prints nothing while the clock log is off. Stops the log thread.
  void LogSummary(const char *why);

  // The scene of the frame the current Swap closes, for the pdz_timing split (frame_timing.cpp): set by the present
  // step's hook (PdzFrontendPace, every frame that goes through sub_82141788, whatever the switches) and taken (reset
  // to kOther) by pdz::timing::OnSwap. kOther = a Swap that did not come from the present step.
  enum class Scene : uint32_t
  {
    kOther = 0,
    kFrontEnd = 1, // the present step outside state 21 (menus, setup, lobby, briefing, loading screens)
    kLevel = 2,    // the present step in state 21
  };
  Scene TakeScene();
  // The same without taking it (pdz_guest_ip_profile, the Swap hook before pdz::timing::OnSwap takes it).
  Scene PeekScene();

  // Read-only (pdz_cutscene_frame_dump, cutscene_smooth.cpp): the last cutscene update's values from PdzCutsceneStep:
  // its call count, Rare's picture time r27 (sub_82221878, ms, before the step), the cutscene's own clock r3 (ms), the
  // final step f31 (s) and the sound sync's position (ms). False before the first cutscene frame.
  bool LastCutsceneStep(uint64_t &calls, uint32_t &picture_ms, uint32_t &clock_ms, double &step_s, double &sound_ms);
  // The current single player level as pdz_sp_60_levels names it ("jungle", "trinity1"; Rare's level list entry,
  // see game_pace.cpp CurrentSpLevel), or "" when not found or not a level. raw: Rare's entry name.
  std::string SpLevelName(const uint8_t *base, std::string *raw = nullptr);
} // namespace pdz::pace
