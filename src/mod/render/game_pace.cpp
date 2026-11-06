// Stage 2 M5: guest vblank pacing on the null GPU backend, the cutscene step, the game clock log (game_pace.h).
//
// Guest data read (TU3, generated\default; m5_framerate_research.md section 1b):
//   82D6BFD8          the game's frame clock block, written by sub_82140500 once per frame on the game thread:
//                     +0 pointer to the game state object (+8 = state, 21 = in a level), +68 the cutscene player
//                     (the cutscene update's `this`), +108 float sum of the clamped dt, +112 float average fps (5
//                     frames), +116 int fps, +120 float dt (the CPU timebase, 1 / 49.8776 MHz per tick, capped at
//                     0.1 s), +128 double sum of the raw dt, +136 frame counter.
//   82D3DB00          the game's vblank callback block (sub_82140438, every guest vblank): +0 vblank counter, +108
//                     vblanks of the last frame, +144 the present step's adaptive extra wait (vblanks, 0-3).
//   82E2F30C          byte: the adaptive extra wait is on (sub_82141788, in state 21).
//   82E2F334          float: the AI clock (sum of dt, per level; ai_game_fixes.cpp kAiClock).
// The log thread reads them only after VirtualQuery says the page is committed and readable (it starts before the
// image is loaded). The cutscene hook only stores atomics; the present step's hook (PdzFrontendPace, game thread,
// once per frame) reads the state, the vblank counter and the device's interval directly (mapped by then).
//
// The cutscene update sub_8221ED68, state 3 (playing), r31 = the cutscene player:
//   r27 = sub_82221878 = the picture's time in ms (frame +88 and fraction +276 x seconds per frame x 1000);
//   r3  = sub_82221828 = the cutscene's own clock in ms (the ms tick - start +112 + banked +116; it stops while
//         paused);
//   if r3 + 66 > r27 (the picture is behind its clock): f31 = the difference x 0.001 s (catch up at once);
//   else (loc_8221EE24): f31 = 1/60 s (820EAEC0), a fixed step;
//   then (loc_8221EE2C) every channel advances by f31 (vtable +320).
// At 60 frames/s the fixed step equals the frame time. Above 60 the picture, once ahead, gains 1/60 s per frame and
// never falls behind again: it runs at fps / 60 (2x at 120) while the cues play on the audio clock (the 120 Hz
// cutscene speed-up of 2026-10-18). The hook below runs before loc_8221EE2C's first instruction (both branches reach
// it) and, when the picture is not behind and pdz_cutscene_step asks for it, replaces the 1/60 s by the real time
// that passed on the cutscene's own clock since the previous frame (at most 1/60 s): the picture then follows real
// time at any frame rate, and the step is the same as Rare's at 60 frames/s.
//
// The present step sub_82141788 (every frame, game thread; m5_frontend.md):
//   state 21 with the byte 82E2F30C set: the adaptive extra wait (spin while 82D3DB00+104 < +144);
//   sub_826DF7F0(device [82E2B64C]): the presentation gate, ring packets only (the CPU does not wait): SCRATCH_REG1 =
//     1, wait for it, InsertCallback(sub_826DF720, interval << 16 | threshold), wait for the release;
//   sub_82621520; then, outside state 21 only, spin (sub_82AD31F0 = NtYieldExecution) until 82D3DB00+104 >= 2
//     (821418F8-82141904); then the Swap (sub_826DF960), which kicks the gate and XE_SWAP to the CP.
// The gate callback (CP INTERRUPT, sub_826DF720) counts frames (device+15612) and releases the gate at the vblank
// (device+15608 countdown in the D3D vblank handler sub_826DE528) or at once when the frame is late. The game's vblank
// callback sub_82140438 sets +104 = 0 at the first vblank after that frame count changed, else +104++. So +104 is
// "vblanks since the CP last reached a gate", not since the game thread's last Swap: right after a vblank that made
// it 2, the thread passes the spin, Swaps (the gate is late: released at once), passes the spin again for a second
// and third Swap (+104 still 2), blocks until the CP's XE_SWAP of the second (next vblank), and then waits for two
// more vblanks after the gate of the third: Swaps 1.5 ms, 1.1 ms, 64 ms apart, XE_SWAPs 1, 1, 2 vblanks apart (the
// vsync-on trace: 212 of 217 long front-end intervals end on the third vblank after the last gate callback).
// PdzFrontendPace (midasm hook before 821418E4, jump to the Swap at 82141908 on true) replaces that spin outside state
// 21 with: wait until the vblank counter 82D3DB00+0 is N past its value at the previous front-end Swap. With N = the
// presentation interval the gate is then just due (released at once) at every Swap: one Swap per N vblanks, evenly.
//   82E2B64C          the D3D device pointer: +13772 the presentation interval (0x80000000 = immediate: no gate),
//                     +13976 0 = no gate written.
// The XUI counters of the pdz_clock line: sub_82792150 = XuiTimersRun (once per frame from the XUI render step
// sub_823A21F0: a timer fires when the ms tick moved >= its interval since it last fired, at most once per call) and
// sub_82792100 (builds the XM_TIMER message; called only for a firing timer). The front-end controls use 16 ms timers
// (id 0xAABBCCDD, e.g. sub_82799010), so their rate is min(frames/s, ~60): the check that the front end at 120
// (pdz_frontend_120) keeps the XUI animations at real-time speed.
#include "game_pace.h"
#include "cpu_placement.h"

#include "frame_timing.h"        // cpusplit: wait.fe_pace
#include "game_thread_sampler.h" // cpusplit: the front-end pace section
#include "native/native_recthread.h" // pdz_native_record_thread: a frame in flight (the cutscene picture's lead)
#include "native/native_textures.h"  // pdz_native_texture_prewarm: OnLevelChange (log 315)

#include <algorithm>
#include <chrono>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>
#include <thread>
#include <cstdlib>

#include <windows.h>
#include <dxgi1_5.h>

#include <fmt/format.h>

#include <rex/audio/audio_driver.h>
#include <rex/cvar.h>
#include <rex/graphics/null_gpu.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/runtime.h>

#include "../../game_cvars.h"   // pdz_fps60_unlock
#include "../../game_patches.h" // the presentation interval patch (pdz_fps60_unlock or the pacing)

REXCVAR_DEFINE_INT32(pdz_null_refresh_hz, 0, "Render",
                     "Stage 2 M5: with graphics_backend null and vsync on, pace the guest vblanks at this rate instead "
                     "of video_mode_refresh_rate (the guest's video mode still reports that): the presentation gate, "
                     "the Swap and the present then run at up to this many frames per second. 120 = 120 Hz (on a "
                     "fixed 120 Hz display one guest vblank per host vblank, with G-Sync / VRR an even 1/120 s timer), "
                     "-1 = the primary display's refresh rate, 0 = off (default: the video mode's 60 Hz). While it is "
                     "in effect the presentation interval is 1 vblank (pdz_fps60_unlock's patch), the cutscene step "
                     "follows real time (pdz_cutscene_step 0) and the game clock log is on. Gameplay keeps real-time "
                     "speed at any rate (the game steps by the CPU clock). Ignored, with one line, with the GPU plugin "
                     "or with vsync off; values 1-29 are ignored. Needs an SDK with null_gpu::SetGuestVblankHz (sdk-14)")
    .range(-1, 240)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_INT32(pdz_cutscene_step, 0, "Render",
                     "Cutscene timing above 60 frames per second (the cutscene update steps a fixed 1/60 s whenever "
                     "the picture is not behind its clock, so above 60 fps cutscenes run fast). 0 = auto: 1 while "
                     "pdz_null_refresh_hz is in effect, else 2. 1 = real time: that step is the time the cutscene's "
                     "clock moved since the previous frame, at most 1/60 s (the same as Rare's at 60 fps). 2 = original "
                     "(Rare's fixed 1/60 s). 3 = guest vblanks at about 60 Hz while a cutscene plays (the pacing rate "
                     "divided by round(rate / 60), so 60 frames/s and Rare's own timing; needs pdz_null_refresh_hz in "
                     "effect, otherwise as 2)")
    .range(0, 3)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_sp_120, true, "Render",
                    "120 fps where it fits, switched by the game itself: with graphics_backend null, vsync on and "
                    "pdz_null_refresh_hz not set, the guest vblanks are paced at the display's rate (pdz_present_pacing "
                    "fixed: its refresh up to 120 Hz, above it an even division of at most 120; vrr / latch: 120 Hz on a "
                    "display of 119 Hz or more) and every frame picks the rate: single player levels that rate; "
                    "co-op (split screen offline: two views did not hold 120, log 181) and multiplayer levels 60 "
                    "(every second vblank); menus, loading and briefings 60 (front-end pacing). Cutscenes follow "
                    "their clock and sound at any rate (pdz_cutscene_step 1, pdz_cutscene_audio_sync). Off = 60 "
                    "everywhere (or pdz_null_refresh_hz's fixed rate). docs/FPS120_REPORT.md")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_STRING(pdz_sp_60_levels, "", "Render",
                      "With pdz_sp_120: single player levels that run at a locked 60 instead (one game frame every "
                      "2 vblanks at 120 Hz, the same fixed pacing as co-op and multiplayer; their cutscenes as in any 60 "
                      "fps level). A comma-separated list, case and spaces ignored; empty (the default) = every level "
                      "at 120. Names: datacore, nightclub, subway, rooftops, mansion, laboratory (or lab), river, "
                      "trinity1 (Trinity Infiltration), trinity2 (Trinity Escape), trinity (both), jungle, temple, "
                      "outpost, bridge, arena; the internal codes work too (holo, club, ware, roof, mans, labo, airp, "
                      "cs1, cs2, jung, ruin, town, brid, cave). Read at each level start; one 'pdz_pace: level' line "
                      "per level. Example: rooftops,jungle")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_cutscene_audio_sync, true, "Render",
                    "Cutscene picture follows the sound: the cutscene's picture keeps Rare's 66 ms lead on its sound "
                    "even when the sound falls behind real time (the audio output played silence because the game's "
                    "audio had no frame ready, e.g. a CPU stall: each such frame delays every sound 5.33 ms). Rare's "
                    "picture follows the game's ms tick, which doesn't wait. With no underrun nothing changes. Off = "
                    "the picture follows the ms tick only (as before)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_STRING(pdz_present_pacing, "fixed", "Render",
                      "How pdz_sp_120 times frames on the display (log 272). fixed (the default since log 279: in-level "
                      "frames p99 8.8-8.9 ms, no hitch) = guest vblanks phase-locked to the display's real refresh, "
                      "each frame presented once at its vblank; single player runs at the refresh itself up to 120 Hz, "
                      "above that at the largest even division of it that is at most 120 (144 -> 72, 240 -> 120); any "
                      "display of 30 Hz or more. auto (older settings files) is read as fixed, with a log line: a "
                      "display's EDID says it can do VRR, not whether the graphics driver has VRR on for it (a FreeSync "
                      "EDID with VRR off in the driver juddered at 117 fps). "
                      "vrr = each frame presented as soon as it is ready (flip model, tearing allowed, the display "
                      "times it), single player capped pdz_vrr_cap_below under the display's refresh (117 at 120 Hz, "
                      "at most 120), front end / co-op / multiplayer 60 (log 274: judder, Rare's 1/60 s steps don't "
                      "divide into 117). vrr / latch need a display of 119 Hz or more. latch = the pacing "
                      "before log 272 (a 120 Hz guest vblank latches the newest frame, Present(1, 0)), for A/B. "
                      "present = for a compositor that owns the display (Steam Deck: gamescope / Proton, where DXGI "
                      "reports the panel's native 90 Hz even when the screen runs at 60): every display refresh is "
                      "presented with Present(1, 0) on a present queue of 1 and the game's vblanks are counted from "
                      "those presents (one per refresh at 60 Hz, every second at 120), no timer; a refresh that isn't "
                      "a multiple of the game's 60 (90 Hz) can't be paced this way and falls back to an even timer "
                      "(set the Deck to 60 Hz). With pdz_sp_120 it needs a reported 119 Hz or more; set it by hand "
                      "under Wine / Proton, gamescope or on a Steam Deck. One pdz_pace line at start-up says which is in effect and why "
                      "(present: also 'present pacing:' lines every 10 s)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(pdz_display_switch_120, true, "Render",
                    "pdz_sp_120 on a display above 120 Hz that 120 doesn't divide (144, 165 Hz), pdz_present_pacing "
                    "auto: the display is switched to 120 Hz at its current resolution for the game's run (Windows "
                    "puts it back when the game exits) when it has that mode; else VRR capped at exactly 120 fps on a "
                    "VRR display; else the even division (144 -> 72), with a log line. 240 Hz needs no switch "
                    "(240 / 2). Off = never switch")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_INT32(pdz_vrr_cap_below, 3, "Render",
                     "pdz_present_pacing vrr: single player levels run this many frames per second under the display's "
                     "refresh (117 on a 120 Hz display), so the display never reaches its maximum (VRR then behaves "
                     "like plain vsync). The cap is at most 120")
    .range(1, 30)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_INT32(pdz_cutscene_av_latency_ms, -1, "Render",
                     "pdz_cutscene_audio_sync: the picture leads its sound by this much more, the time a frame takes "
                     "from the game to the screen (its sound plays on the audio output at once). -1 = auto: the frames "
                     "in flight (record thread 1, present 1, latch pacing 1 more) x the measured frame time; 0 = none "
                     "(as before log 272)")
    .range(-1, 100)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_game_clock_log, false, "Render",
                    "Game speed check ('pdz_clock:' lines, read at startup): every 10 s the game's own clock (the sum "
                    "of its frame times, from the CPU timebase), its frame count and fps, and the guest vblank count "
                    "against the host's wall clock (QPC), one line per cutscene (how far its picture moved against "
                    "the wall clock, the steps taken) and a SUMMARY at test exit with the game clock / wall clock "
                    "ratio (1.0025 = right: the game converts the 50 MHz timebase with 49.88 MHz). Reads guest "
                    "memory from its own low-priority thread only. On by itself while pdz_null_refresh_hz is in "
                    "effect. Also times the host presents (SDK null_gpu present statistics) for its lines, and "
                    "counts the front-end frames (pdz_fix_frontend_pacing) and the menus' XUI timer runs and fires");

REXCVAR_DEFINE_INT32(pdz_null_present_latency, 1, "Render",
                     "While pdz_null_refresh_hz is in effect (only then): the presenter's swap chain gets a frame "
                     "latency waitable object of this depth, waited for before each paint (SDK "
                     "null_gpu::SetPresentMaxFrameLatency; present_max_frame_latency wins when it is set). At guest "
                     "vblanks as frequent as the display's refreshes, DXGI's own queue of 3 fills up and every Present "
                     "then blocks inside the runtime until the next flip, and the game thread's command list submit and "
                     "texture creation wait behind it (about 7 ms a frame at 120). 1 = one present queued at most "
                     "(default), 2-3 deeper, 0 = no waitable object (DXGI's queue of 3: the behaviour before, for an "
                     "A/B)")
    .range(0, 3)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(pdz_fix_frontend_pacing, true, "Render",
                    "Front-end frame pacing (menus, setup, lobby, briefing: the present step outside a level). Rare's "
                    "front end waits for 2 vblanks counted from the GPU's last presentation gate, which lets two "
                    "frames through at once and holds the third for about 4 vblanks (50-64 ms every third frame, "
                    "36-46 fps with pdz_fps60_unlock). On: a front-end frame is presented a fixed number of vblanks "
                    "after the previous one (the presentation interval: 60 fps with pdz_fps60_unlock, 30 without; "
                    "60 while pdz_null_refresh_hz is in effect, unless pdz_frontend_120). Both GPU backends, vsync on "
                    "only. Off: Rare's wait. Levels are not affected")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_pace_state_log, false, "Render",
                    "Diagnostics: one 'pdz_pace: game state' line each time the game state (state object +8) or the "
                    "level id byte changes, read in the present step (front end, loading, level)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_frontend_120, false, "Render",
                    "With pdz_fix_frontend_pacing and pdz_null_refresh_hz in effect: the front end runs at the paced "
                    "rate like the levels (one frame per guest vblank, 120 fps at 120 Hz) instead of about 60. The "
                    "menus' XUI timers fire at most every 16 ms whatever the frame rate (pdz_clock line: xui timers)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace
{
  // Guest addresses (see the top of this file).
  constexpr uint32_t kClock = 0x82D6BFD8;
  constexpr uint32_t kClockState = kClock + 0;       // pointer; [ptr + 8] = the game state
  constexpr uint32_t kClockClampedSum = kClock + 108; // float
  constexpr uint32_t kClockAvgFps = kClock + 112;     // float
  constexpr uint32_t kClockIntFps = kClock + 116;     // int32
  constexpr uint32_t kClockDt = kClock + 120;         // float
  constexpr uint32_t kClockRawSum = kClock + 128;     // double
  constexpr uint32_t kClockFrames = kClock + 136;     // uint32
  constexpr uint32_t kVblankBlock = 0x82D3DB00;
  constexpr uint32_t kVblankCount = kVblankBlock + 0;
  constexpr uint32_t kVblankLastFrame = kVblankBlock + 108;
  constexpr uint32_t kVblankExtraWait = kVblankBlock + 144;
  constexpr uint32_t kAdaptiveWaitFlag = 0x82E2F30C;
  constexpr uint32_t kAiClock = 0x82E2F334;
  constexpr uint32_t kStateInLevel = 21;

  constexpr float kRareStep = 1.0f / 60.0f; // 820EAEC0
  constexpr uint32_t kCutsceneLeadMs = 66;  // sub_8221ED68: wall + 66 > picture = behind

  // Latched by ChooseAtStartup (app setup, before any guest thread).
  bool g_active = false;
  uint32_t g_hz = 0;
  bool g_log = false;
  bool g_auto = false; // pdz_sp_120: the rate follows the mode (ModeDivider)
  // pdz_sp_120: the divider the present step chose for the current scene (1 = the paced rate, 2 = half).
  std::atomic<uint32_t> g_mode_divider{1};
  constexpr uint32_t kGameMode = 40; // game state +40: 0 single player, 1 co-op, 2 / 3 multiplayer
  // pdz_present_pacing (log 272), latched by ChooseAtStartup: VRR = immediate presents, single player levels at
  // g_vrr_cap_hz (set by the present step per scene); frames in flight from the Swap to the screen, for the cutscene
  // picture's lead on its sound (pdz_cutscene_av_latency_ms -1).
  bool g_vrr = false;
  double g_vrr_cap_hz = 0.0;
  bool g_cap_on = false; // game thread: the cap is set for the current scene
  // pdz_sp_60_levels (game thread, the present step): the level last seen and whether it is on the list.
  uint32_t g_level_key = ~0u; // state object low bits, mode and level id; ~0 = not in a level
  bool g_level_listed = false;
  std::atomic<uint32_t> g_cap_fps{0}; // the capped rate while it is on (EffectiveHz)
  uint32_t g_frames_in_flight = 2;
  // pdz_present_pacing present (Deck log: gamescope), latched by ChooseAtStartup.
  bool g_present_mode = false;
  const char *g_pacing_name = "latch";
  const char *PacingName() { return g_pacing_name; }
  // The clock log thread runs for the per-cutscene lines only (pdz_game_clock_log off; log 272 had none).
  bool g_cutscene_log_only = false;

  // The cutscene hook (game thread) -> the log thread.
  std::atomic<uint64_t> g_cs_calls{0};
  std::atomic<uint32_t> g_cs_this{0};
  std::atomic<uint64_t> g_cs_last{0}; // picture ms << 32 | the cutscene's clock ms (one frame's pair)
  std::atomic<int64_t> g_cs_last_step_us{0}; // that frame's final step (LastCutsceneStep, pdz_cutscene_frame_dump)
  std::atomic<uint64_t> g_cs_behind{0}, g_cs_ahead{0}, g_cs_real_time{0}, g_cs_step_us{0};
  std::atomic<uint32_t> g_divider{1}; // what the hook / the log thread last set (pdz_cutscene_step 3)
  // The hook's own previous frame (game thread only).
  uint32_t g_prev_this = 0, g_prev_wall = 0;
  bool g_prev_valid = false;
  // Picture / sound (pdz_cutscene_audio_sync): the audio output's clock when this cutscene started (game thread), and
  // for the log thread: how far the sound fell behind real time in this cutscene (underruns), the picture's offset
  // from its sound + 66 ms after this frame's step (latest and the largest either way), and the frames it corrected.
  rex::audio::OutputClock g_av_start{}, g_av_live{}; // live: at the last frame the game's audio delivered
  std::atomic<int64_t> g_cs_av_behind_us{0}, g_cs_av_offset_us{0}, g_cs_av_offset_max_us{0};
  std::atomic<int64_t> g_cs_av_clock_lead_us{0}; // the ms clock's lead on the sound (what Rare's picture would gain)
  std::atomic<uint64_t> g_cs_av_synced{0}, g_cs_av_frames{0}, g_cs_av_offset_max_frame{0};
  double g_av_real_ms = 0.0;    // the sound's position (game thread)
  double g_av_pending_ms = 0.0; // real time since the cutscene's ms clock last moved
  double g_av_frame_ms = 0.0;   // the frame time in cutscenes, smoothed (game thread)
  std::atomic<int64_t> g_cs_av_lead_us{0}; // the picture's lead for the frames in flight (pdz_cutscene_av_latency_ms)
  std::chrono::steady_clock::time_point g_av_prev_time{};

  // Front-end pacing (PdzFrontendPace below).
  constexpr uint32_t kD3dDevicePtr = 0x82E2B64C;     // sub_82141788: lwz r3,-18868(r30), r30 = 82E30000
  constexpr uint32_t kDevPresentInterval = 13772;    // sub_826DF7F0: 0x80000000 = immediate (no gate); 0/1, 2, 4
  constexpr uint32_t kDevGateOn = 13976;             // sub_826DF7F0: 0 = no gate written
  constexpr uint32_t kIntervalImmediate = 0x80000000u;
  constexpr double kFrontEndWaitLimitS = 0.25;       // a wait never legitimately lasts this long (vblanks stopped?)
  std::atomic<uint32_t> g_scene{0};                  // pdz::pace::Scene of the frame the next Swap closes
  std::atomic<uint8_t *> g_game_base{nullptr};       // guest memory base, cached by the game-thread hook
  // Counters for the pdz_clock line (written on the game thread while the clock log is on).
  std::atomic<uint64_t> g_fe_frames{0};    // present steps outside state 21
  std::atomic<uint64_t> g_fe_paced{0};     // of those, paced by the fix
  std::atomic<uint64_t> g_fe_waits{0};     // of those, with a wait (the CPU was early)
  std::atomic<uint64_t> g_fe_wait_us{0};   // the time waited
  std::atomic<uint64_t> g_fe_timeouts{0};  // waits cut at kFrontEndWaitLimitS
  std::atomic<uint64_t> g_xui_runs{0};     // XuiTimersRun calls (sub_82792150)
  std::atomic<uint64_t> g_xui_fires{0};    // XUI timers fired (sub_82792100)
  // The game thread's previous front-end Swap and the logged mode.
  uint32_t g_fe_last_vblank = 0;
  bool g_fe_have_last = false;
  uint32_t g_fe_logged_mode = ~0u;

  int StepMode()
  {
    const int32_t mode = REXCVAR_GET(pdz_cutscene_step);
    return mode == 0 ? (g_active ? 1 : 2) : mode;
  }

  uint32_t CutsceneDivider()
  {
    return g_hz >= 90 ? uint32_t(std::lround(double(g_hz) / 60.0)) : 1u;
  }

  void SetDivider(uint32_t divider)
  {
    if (g_divider.exchange(divider, std::memory_order_relaxed) != divider)
    {
      rex::graphics::null_gpu::SetGuestVblankDivider(divider);
    }
  }
} // namespace

// Mid-function hook (midasm_hooks_tu3.toml), before T 8221EE2C (lwz r11,84(r31)) in the cutscene update
// sub_8221ED68, state 3: f31 = this frame's step (s), r3 = the cutscene's clock (ms), r27 = the picture's time (ms),
// r31 = the cutscene player. Once per frame while a cutscene plays; atomics only.
void PdzCutsceneStep(PPCRegister &f31, PPCRegister &r3, PPCRegister &r27, PPCRegister &r31)
{
  const uint32_t wall = r3.u32, picture = r27.u32, player = r31.u32;
  const bool behind = uint32_t(wall + kCutsceneLeadMs) > picture; // cmplw / ble 8221EE24, as the game
  const int mode = StepMode();
  if (behind)
  {
    g_cs_behind.fetch_add(1, std::memory_order_relaxed);
  }
  else
  {
    g_cs_ahead.fetch_add(1, std::memory_order_relaxed);
    if (mode == 1)
    {
      // The time the cutscene's clock moved since the previous frame of this cutscene, at most Rare's 1/60 s; the
      // first frame keeps Rare's step.
      float step = kRareStep;
      if (g_prev_valid && player == g_prev_this && wall >= g_prev_wall)
      {
        step = std::min(kRareStep, float(wall - g_prev_wall) * 0.001f);
      }
      f31.f64 = double(step);
      g_cs_real_time.fetch_add(1, std::memory_order_relaxed);
    }
  }
  // The picture follows the sound (pdz_cutscene_audio_sync). Rare keeps the picture 66 ms ahead of the cutscene's
  // clock, the game's ms tick, and starts the cutscene's three sounds in the same update that starts that clock
  // (sub_8221ED68 state 2 -> 3). The sounds then run on the audio output, at real time, less every frame the output
  // had to play as silence (an underrun: the game's audio had no frame ready). Rare's picture follows the ms tick,
  // which doesn't wait for the sound. Here the sound's position is the clock's start plus the real time that passed
  // while the clock ran (it stops while the cutscene is paused), less the underruns; the picture's step is cut to
  // what reaches that position + 66 ms (0 while the picture is ahead). With no underrun the picture moves as before
  // (logs 184-186, Datacore's 26 s opening: the tick within 15 ms of real time, end offset 0 at 60 and 120).
  const bool new_cutscene = !g_prev_valid || player != g_prev_this || wall < g_prev_wall;
  const rex::audio::OutputClock audio = rex::audio::GetOutputClock();
  const auto now = std::chrono::steady_clock::now();
  double frame_ms = 0.0;
  if (new_cutscene)
  {
    g_av_start = audio;
    g_av_live = audio;
    g_av_real_ms = double(wall);
    g_av_pending_ms = 0.0;
    g_cs_av_offset_max_us.store(0, std::memory_order_relaxed);
    g_cs_av_offset_max_frame.store(0, std::memory_order_relaxed);
    g_cs_av_frames.store(0, std::memory_order_relaxed);
    g_cs_av_synced.store(0, std::memory_order_relaxed);
  }
  else
  {
    // Real time counts while the clock runs. A frame where the tick didn't move (a frame shorter than the tick, or a
    // pause) holds its time until the tick moves again; a resume adds no more than the tick's own move + 50 ms, so a
    // pause can't add its length.
    const double real = std::chrono::duration<double, std::milli>(now - g_av_prev_time).count();
    frame_ms = real;
    g_av_pending_ms += real;
    if (wall > g_prev_wall)
    {
      g_av_real_ms += std::min(g_av_pending_ms, double(wall - g_prev_wall) + 50.0);
      g_av_pending_ms = 0.0;
    }
  }
  g_av_prev_time = now;
  const uint64_t av_frame = g_cs_av_frames.fetch_add(1, std::memory_order_relaxed) + 1;
  if (audio.played != g_av_live.played)
  {
    g_av_live = audio; // the game's audio is still delivering
  }
  // Silence while the game delivers no audio at all (its audio stopped, not late) counts for 250 ms at most, so a
  // dead audio thread can't hold the picture.
  const uint64_t silent = std::min(audio.silent, g_av_live.silent + 48);
  const double behind_ms = double(silent - g_av_start.silent) * rex::audio::kOutputFrameMs;
  // The cutscene time its sound is at; time held since the tick last moved counts up to two ticks (none in a pause).
  const double held_ms = g_av_pending_ms <= 34.0 ? g_av_pending_ms : 0.0;
  const double sound_ms = g_av_real_ms + held_ms - behind_ms;
  g_cs_av_clock_lead_us.store(int64_t((double(wall) - sound_ms) * 1000.0), std::memory_order_relaxed);
  // Log 272 (sound ahead of the picture): this frame reaches the screen the frames in flight later (the record
  // thread, the present queue, the latch), its sound plays on the audio output at once, so the picture leads by that
  // much more. Frame time smoothed over the cutscene's frames (pauses and stalls left out).
  if (frame_ms > 1.0 && frame_ms < 50.0)
  {
    g_av_frame_ms = g_av_frame_ms > 0.0 ? g_av_frame_ms * 0.9 + frame_ms * 0.1 : frame_ms;
  }
  const int32_t latency_setting = REXCVAR_GET(pdz_cutscene_av_latency_ms);
  const double lead_ms =
      !REXCVAR_GET(pdz_cutscene_audio_sync) ? 0.0
      : latency_setting >= 0                ? double(latency_setting)
                                            : double(g_frames_in_flight) * (g_av_frame_ms > 0.0 ? g_av_frame_ms : 1000.0 / 60.0);
  g_cs_av_lead_us.store(int64_t(lead_ms * 1000.0), std::memory_order_relaxed);
  if (REXCVAR_GET(pdz_cutscene_audio_sync))
  {
    // The step may run ahead of the clock's by the lead (once, at the start, or as the lead grows), never more.
    const double reach = std::max(0.0, (sound_ms + lead_ms + kCutsceneLeadMs - double(picture)) * 0.001);
    f31.f64 = behind ? reach : std::min(f31.f64 + lead_ms * 0.001, reach);
    g_cs_av_synced.fetch_add(1, std::memory_order_relaxed);
  }
  const int64_t offset_us =
      int64_t((double(picture) + f31.f64 * 1000.0 - kCutsceneLeadMs - lead_ms - sound_ms) * 1000.0);
  g_cs_av_behind_us.store(int64_t(behind_ms * 1000.0), std::memory_order_relaxed);
  g_cs_av_offset_us.store(offset_us, std::memory_order_relaxed);
  // The first frames are Rare's start catch-up (the picture jumps to its clock + 66 ms; logs 184-186: up to +230 ms
  // at frame 2 with or without the sync), not counted.
  constexpr uint64_t kStartFrames = 5;
  if (av_frame > kStartFrames &&
      std::llabs(offset_us) > std::llabs(g_cs_av_offset_max_us.load(std::memory_order_relaxed)))
  {
    g_cs_av_offset_max_frame.store(av_frame, std::memory_order_relaxed);
    g_cs_av_offset_max_us.store(offset_us, std::memory_order_relaxed); // the first frame's catch-up isn't counted
  }
  if (mode == 3 && g_active)
  {
    SetDivider(CutsceneDivider());
  }
  else
  {
    SetDivider(g_mode_divider.load(std::memory_order_relaxed));
  }
  g_prev_this = player;
  g_prev_wall = wall;
  g_prev_valid = true;
  g_cs_step_us.fetch_add(uint64_t(std::max(0.0, f31.f64) * 1e6 + 0.5), std::memory_order_relaxed);
  g_cs_this.store(player, std::memory_order_relaxed);
  g_cs_last.store((uint64_t(picture) << 32) | wall, std::memory_order_relaxed);
  g_cs_last_step_us.store(int64_t(f31.f64 * 1e6), std::memory_order_relaxed); // read-only tap (LastCutsceneStep)
  g_cs_calls.fetch_add(1, std::memory_order_release);
}

namespace
{
  uint8_t *GameBase()
  {
    uint8_t *base = g_game_base.load(std::memory_order_relaxed);
    if (!base)
    {
      if (auto *rt = rex::Runtime::instance())
      {
        base = rt->virtual_membase();
        g_game_base.store(base, std::memory_order_relaxed);
      }
    }
    return base;
  }

  // Big-endian guest word, on the game thread (the image's data and the D3D device are mapped by then).
  uint32_t GuestU32(const uint8_t *base, uint32_t address)
  {
    return _byteswap_ulong(*reinterpret_cast<const volatile uint32_t *>(base + address));
  }

  int64_t QpcNow()
  {
    LARGE_INTEGER v;
    QueryPerformanceCounter(&v);
    return v.QuadPart;
  }

  int64_t QpcFrequency()
  {
    static const int64_t frequency = [] {
      LARGE_INTEGER f;
      QueryPerformanceFrequency(&f);
      return int64_t(f.QuadPart);
    }();
    return frequency;
  }

  // vsync (restart cvar) latched at the first front-end frame. Off, the vsync worker marks a guest vblank about every
  // 2 ms (unpaced): Rare's wait is left alone there.
  bool VsyncOn()
  {
    static const bool on = [] {
      const std::string v = rex::cvar::GetFlagByName("vsync");
      return v.empty() || v == "true" || v == "1";
    }();
    return on;
  }

  // Guest vblanks per front-end frame: the gate's interval as sub_826DF7F0 maps the device's setting (0 / 1 -> 1,
  // 2 -> 2, 4 -> 3; any other value takes an uninitialised stack word there, 2 here), 1 with the interval patch
  // (pdz_fps60_unlock, or the pacing), times round(rate / 60) at a paced rate unless pdz_frontend_120.
  struct FrontEndBudget
  {
    uint32_t vblanks = 1, interval = 1, scale = 1;
  };

  FrontEndBudget BudgetFor(uint32_t setting)
  {
    FrontEndBudget b;
    b.interval = game_patches::IsEnabled(game_patches::PatchId::Fps60Unlock) ? 1u
                 : setting <= 1                                              ? 1u
                 : setting == 4                                              ? 3u
                                                                             : 2u;
    b.scale = g_active && !REXCVAR_GET(pdz_frontend_120)
                  ? std::max(1u, uint32_t(std::lround(double(g_hz) / 60.0)))
                  : 1u;
    b.vblanks = b.interval * b.scale;
    return b;
  }

  // One line whenever the front end's pacing mode changes (game thread; a handful per session).
  void NoteFrontEndMode(uint32_t mode, const FrontEndBudget *b, const char *why)
  {
    if (mode == g_fe_logged_mode)
    {
      return;
    }
    g_fe_logged_mode = mode;
    if (b)
    {
      REXLOG_INFO("pdz_pace: front end: pdz_fix_frontend_pacing on: one frame every {} guest vblank(s) (presentation "
                  "interval {}{}), counted from the previous front-end Swap; Rare's 2-vblank spin skipped",
                  b->vblanks, b->interval,
                  g_active ? fmt::format(" x {} at the paced {} Hz, pdz_frontend_120 {}", b->scale, g_hz,
                                         REXCVAR_GET(pdz_frontend_120) ? "on" : "off")
                  : REXCVAR_GET(pdz_frontend_120) ? std::string("; pdz_frontend_120 has no effect without "
                                                                "pdz_null_refresh_hz")
                                                  : std::string());
    }
    else
    {
      REXLOG_INFO("pdz_pace: front end: Rare's pacing (2 vblanks after the last presentation gate): {}", why);
    }
  }
  // pdz_sp_60_levels: the single player missions, the names the user knows (docs/SP_INVENTORY.md) and Rare's
  // internal level codes.
  struct SpLevel
  {
    const char *code;
    const char *name;
    const char *aliases; // comma-separated, already normalised
  };
  constexpr SpLevel kSpLevels[] = {{"holo", "datacore", ""},
                                   {"club", "nightclub", ""},
                                   {"ware", "subway", ""},
                                   {"roof", "rooftops", "rooftop"},
                                   {"mans", "mansion", ""},
                                   {"labo", "laboratory", "lab"},
                                   {"airp", "river", ""},
                                   {"cs1", "trinity1", "trinity,trinityinfiltration"},
                                   {"cs2", "trinity2", "trinity,trinityescape"},
                                   {"jung", "jungle", ""},
                                   {"ruin", "temple", ""},
                                   {"town", "outpost", ""},
                                   {"brid", "bridge", ""},
                                   {"cave", "arena", ""}};
  // TU3's level list (sub_82276F20 / sub_822774D0): +8 entries (each: +0 name string), +24 the single player id ->
  // index table (+20 its size); the current level's id is the signed byte at game state +25 (outside multiplayer).
  constexpr uint32_t kLevelList = 0x82D6C2B4;
  constexpr uint32_t kGameLevelId = 25;

  std::string Normalise(std::string_view in)
  {
    std::string out;
    for (const char c : in)
    {
      if (std::isalnum(static_cast<unsigned char>(c)))
      {
        out += char(std::tolower(static_cast<unsigned char>(c)));
      }
    }
    return out;
  }

  // The current single player level: Rare's entry name (raw) and the matching mission (nullptr: not found).
  const SpLevel *CurrentSpLevel(const uint8_t *base, uint32_t state_object, std::string &raw, int &id)
  {
    id = int(int8_t(base[state_object + kGameLevelId]));
    raw.clear();
    if (id < 0)
    {
      return nullptr;
    }
    const uint32_t size = GuestU32(base, kLevelList + 20);
    const uint32_t table = GuestU32(base, kLevelList + 24);
    const uint32_t entries = GuestU32(base, kLevelList + 8);
    if (table < 0x10000u || entries < 0x10000u || (size > 0 && size < 4096 && uint32_t(id) >= size))
    {
      return nullptr;
    }
    const int32_t index = int32_t(GuestU32(base, table + uint32_t(id) * 4));
    const uint32_t entry = index >= 0 && index < 4096 ? GuestU32(base, entries + uint32_t(index) * 4) : 0;
    const uint32_t name = entry >= 0x10000u ? GuestU32(base, entry) : 0;
    for (uint32_t i = 0; name >= 0x10000u && i < 64 && base[name + i]; ++i)
    {
      raw += char(base[name + i]);
    }
    const std::string lower = Normalise(raw);
    const SpLevel *best = nullptr;
    for (const SpLevel &l : kSpLevels)
    {
      if (lower.find(l.code) != std::string::npos && (!best || std::strlen(l.code) > std::strlen(best->code)))
      {
        best = &l;
      }
    }
    return best;
  }

  // Splits a comma-separated list and calls `match` on each normalised, non-empty item until it returns true.
  template <typename Fn>
  bool AnyItem(std::string_view list, Fn &&match)
  {
    size_t start = 0;
    while (start <= list.size())
    {
      const size_t comma = list.find(',', start);
      const std::string item =
          Normalise(list.substr(start, comma == std::string_view::npos ? std::string_view::npos : comma - start));
      start = comma == std::string_view::npos ? list.size() + 1 : comma + 1;
      if (!item.empty() && match(item))
      {
        return true;
      }
    }
    return false;
  }

  // Is this level on pdz_sp_60_levels? Its name, an alias, its internal code, or Rare's raw entry name.
  bool ListedFor60(const SpLevel *level, const std::string &raw)
  {
    const std::string list = REXCVAR_GET(pdz_sp_60_levels);
    const std::string raw_name = Normalise(raw);
    return AnyItem(list, [&](const std::string &token) {
      if (!raw_name.empty() && token == raw_name)
      {
        return true;
      }
      if (!level)
      {
        return false;
      }
      return token == level->code || token == level->name ||
             AnyItem(level->aliases, [&](const std::string &alias) { return alias == token; });
    });
  }
} // namespace

// Mid-function hook (midasm_hooks_tu3.toml), before T 821418E4 (lwz r11,-16424(r29)) in the present step
// sub_82141788, after the presentation gate's packets are written and before the front end's spin; every frame that
// goes through the present step. Returns true to jump to the Swap (82141908, the li r4,3 before sub_8239E738) with
// the front-end wait done here; false runs Rare's code (state 21: no spin; or the spin until 82D3DB00+104 >= 2).
bool PdzFrontendPace()
{
  uint8_t *const base = GameBase();
  if (!base)
  {
    return false;
  }
  const uint32_t state_object = GuestU32(base, kClockState);
  if (state_object < 0x10000u)
  {
    return false;
  }
  const bool level = GuestU32(base, state_object + 8) == kStateInLevel;
  if (REXCVAR_GET(pdz_pace_state_log))
  {
    static uint32_t last_state = ~0u, last_object = 0;
    static int last_id = -1000;
    const uint32_t state = GuestU32(base, state_object + 8);
    const int id = int(int8_t(base[state_object + kGameLevelId]));
    if (state != last_state || id != last_id || state_object != last_object)
    {
      REXLOG_INFO("pdz_pace: game state {} -> {} (level id {} -> {}, state object {:08X})", int32_t(last_state),
                  state, last_id, id, state_object);
      last_state = state;
      last_id = id;
      last_object = state_object;
    }
  }
  {
    // pdz_native_texture_prewarm / pdz_native_pso_level_wait_ms: the level's name from the start of its loading (a
    // single player level's id is set at the mission select, state 4; loading runs states 5, 30, 6, 29, 19, 20, 7 in
    // Datacore; 21 = in the level), so its textures and pipelines are prepared during the loading screen; "" when
    // leaving. Then OnLevelPlaying at the first frame in the level. Any mode (multiplayer: from state 21 only).
    static std::string tex_key;
    static bool tex_playing = false;
    const uint32_t state = GuestU32(base, state_object + 8);
    const int id = int(int8_t(base[state_object + kGameLevelId]));
    const bool loading = id >= 0 && state > 4 && state != kStateInLevel;
    std::string want;
    if (level || loading)
    {
      int unused = -1;
      CurrentSpLevel(base, state_object, want, unused);
      if (want.empty())
      {
        want = fmt::format("level{}_mode{}", id, GuestU32(base, state_object + kGameMode));
      }
    }
    if (want != tex_key)
    {
      tex_key = want;
      tex_playing = false;
      pdz::native::OnLevelChange(tex_key.c_str());
    }
    if (level && !tex_playing && !tex_key.empty())
    {
      tex_playing = true;
      pdz::native::OnLevelPlaying();
    }
  }
  if (g_auto)
  {
    // pdz_sp_120: single player levels at the paced rate, co-op (split screen) and multiplayer levels at 60; the
    // front end keeps the paced vblanks (its own pacing presents at 60). Logged on change.
    const uint32_t mode = GuestU32(base, state_object + kGameMode);
    // pdz_sp_60_levels: a listed single player level runs as co-op and multiplayer do. Decided once per level start
    // (the state object, mode and level id), with one line.
    const uint32_t key =
        level ? ((state_object & 0xFFFFu) << 16) | ((mode & 0xFFu) << 8) | base[state_object + kGameLevelId] : ~0u;
    if (key != g_level_key)
    {
      g_level_key = key;
      g_level_listed = false;
      if (level && mode == 0)
      {
        std::string raw;
        int id = -1;
        const SpLevel *sp = CurrentSpLevel(base, state_object, raw, id);
        g_level_listed = ListedFor60(sp, raw);
        REXLOG_INFO("pdz_pace: level {} (id {}, Rare's '{}'): {}", sp ? std::string(sp->name) : (raw.empty() ? "?" : raw),
                    id, raw, g_level_listed ? "locked 60 (pdz_sp_60_levels)" : "120 (pdz_sp_120)");
      }
    }
    const uint32_t divider = level && (mode != 0 || g_level_listed) ? CutsceneDivider() : 1u;
    // pdz_present_pacing vrr: single player levels at the cap under the display's refresh (the vsync worker's rate
    // while the divider is 1); the front end, co-op, multiplayer and pdz_sp_60_levels keep the paced rate's 60.
    const bool cap = g_vrr && level && mode == 0 && !g_level_listed;
    const bool cap_changed = cap != g_cap_on;
    if (cap_changed)
    {
      g_cap_on = cap;
      g_cap_fps.store(cap ? uint32_t(std::lround(g_vrr_cap_hz)) : 0u, std::memory_order_relaxed);
      rex::graphics::null_gpu::SetGuestVblankCapHz(cap ? g_vrr_cap_hz : 0.0);
    }
    if (g_mode_divider.exchange(divider, std::memory_order_relaxed) != divider || cap_changed)
    {
      REXLOG_INFO("pdz_pace: pdz_sp_120: {} (game mode {}): {} fps{}", level ? "level" : "front end", mode,
                  cap ? g_cap_fps.load(std::memory_order_relaxed) : level ? g_hz / divider : 60,
                  cap ? " (VRR cap)" : "");
    }
    if (g_divider.load(std::memory_order_relaxed) != divider && !(StepMode() == 3 && g_cs_this.load() != 0 && level))
    {
      SetDivider(divider);
    }
  }
  if (level)
  {
    g_scene.store(uint32_t(pdz::pace::Scene::kLevel), std::memory_order_relaxed);
    return false; // a level: Rare's path
  }
  g_scene.store(uint32_t(pdz::pace::Scene::kFrontEnd), std::memory_order_relaxed);
  if (g_log)
  {
    g_fe_frames.fetch_add(1, std::memory_order_relaxed);
  }
  if (!REXCVAR_GET(pdz_fix_frontend_pacing))
  {
    NoteFrontEndMode(1, nullptr, "pdz_fix_frontend_pacing off");
    return false;
  }
  if (!VsyncOn())
  {
    NoteFrontEndMode(2, nullptr, "vsync off (guest vblanks unpaced, about every 2 ms)");
    return false;
  }
  const uint32_t device = GuestU32(base, kD3dDevicePtr);
  if (device < 0x10000u)
  {
    return false;
  }
  const uint32_t setting = GuestU32(base, device + kDevPresentInterval);
  if (setting == kIntervalImmediate || GuestU32(base, device + kDevGateOn) == 0)
  {
    NoteFrontEndMode(3, nullptr, "no presentation gate (immediate presentation interval)");
    return false;
  }
  const FrontEndBudget b = BudgetFor(setting);
  NoteFrontEndMode(0x10000u | (REXCVAR_GET(pdz_frontend_120) ? 0x20000u : 0u) | (b.vblanks << 8) | (b.interval << 4) |
                       b.scale,
                   &b, nullptr);
  uint32_t vblank = GuestU32(base, kVblankCount);
  const uint32_t target = g_fe_last_vblank + b.vblanks;
  if (g_fe_have_last && int32_t(vblank - target) < 0)
  {
    // Early: wait for the vblank N after the previous front-end Swap, yielding like Rare's spin (NtYieldExecution =
    // SwitchToThread). The gate of this frame is then due at the Swap and released at once: one Swap per N vblanks.
    const pdz::timing::ExclusiveScope wait_time(pdz::timing::Phase::kWaitFePace); // cpusplit
    const pdz::gtsample::Mark sampler_mark(pdz::gtsample::Where::kFePace);
    const int64_t t0 = QpcNow();
    const int64_t limit = t0 + int64_t(kFrontEndWaitLimitS * double(QpcFrequency()));
    int64_t now = t0;
    bool timed_out = false;
    while (int32_t(vblank - target) < 0)
    {
      if (now >= limit)
      {
        timed_out = true;
        break;
      }
      SwitchToThread();
      vblank = GuestU32(base, kVblankCount);
      now = QpcNow();
    }
    if (g_log)
    {
      g_fe_waits.fetch_add(1, std::memory_order_relaxed);
      g_fe_wait_us.fetch_add(uint64_t(double(now - t0) * 1e6 / double(QpcFrequency())), std::memory_order_relaxed);
      if (timed_out)
      {
        g_fe_timeouts.fetch_add(1, std::memory_order_relaxed);
      }
    }
  }
  g_fe_last_vblank = vblank;
  g_fe_have_last = true;
  if (g_log)
  {
    g_fe_paced.fetch_add(1, std::memory_order_relaxed);
  }
  return true;
}

// XuiTimersRun (once per frame from the XUI render step sub_823A21F0): counted for the pdz_clock line.
REX_EXTERN(__imp__sub_82792150);
REX_HOOK_RAW(sub_82792150)
{
  if (g_log)
  {
    g_xui_runs.fetch_add(1, std::memory_order_relaxed);
  }
  __imp__sub_82792150(ctx, base);
}

// The XM_TIMER message of a firing XUI timer (called only from XuiTimersRun): counted for the pdz_clock line.
REX_EXTERN(__imp__sub_82792100);
REX_HOOK_RAW(sub_82792100)
{
  if (g_log)
  {
    g_xui_fires.fetch_add(1, std::memory_order_relaxed);
  }
  __imp__sub_82792100(ctx, base);
}

namespace pdz::pace
{
  namespace
  {
    constexpr DWORD kTickMs = 250;
    constexpr double kWindowS = 10.0;
    constexpr int kCutsceneIdleTicks = 2;    // a segment ends after 0.5 s without a cutscene frame
    constexpr double kSegmentLogS = 1.0;     // shorter segments (a skipped cutscene) are not logged
    constexpr double kSegmentTotalS = 2.0;   // nor counted in the SUMMARY ratio

    // ---------------------------------------------------------------------------------------------------------------
    // Guest reads (log thread / test exit only).
    // ---------------------------------------------------------------------------------------------------------------
    uint8_t *Base()
    {
      auto *rt = rex::Runtime::instance();
      return rt ? rt->virtual_membase() : nullptr;
    }

    bool Readable(const uint8_t *p, size_t size)
    {
      MEMORY_BASIC_INFORMATION mbi{};
      if (!VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) ||
          !(mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                           PAGE_EXECUTE_WRITECOPY)))
      {
        return false;
      }
      return p + size <= static_cast<const uint8_t *>(mbi.BaseAddress) + mbi.RegionSize;
    }

    // Big-endian guest value of 1, 4 or 8 bytes.
    bool Read(uint8_t *base, uint32_t address, size_t size, uint64_t &out)
    {
      if (!base || address < 0x10000u)
      {
        return false;
      }
      const uint8_t *p = base + address;
      if (!Readable(p, size))
      {
        return false;
      }
      out = 0;
      for (size_t i = 0; i < size; ++i)
      {
        out = (out << 8) | p[i];
      }
      return true;
    }

    bool Read32(uint8_t *base, uint32_t address, uint32_t &out)
    {
      uint64_t v = 0;
      if (!Read(base, address, 4, v))
      {
        return false;
      }
      out = uint32_t(v);
      return true;
    }

    float AsFloat(uint32_t bits)
    {
      float f;
      std::memcpy(&f, &bits, sizeof(f));
      return f;
    }

    double AsDouble(uint64_t bits)
    {
      double d;
      std::memcpy(&d, &bits, sizeof(d));
      return d;
    }

    struct Snap
    {
      bool ok = false; // the frame clock block was readable and the game has counted frames
      int64_t qpc = 0;
      double raw_sum = 0.0;
      float clamped_sum = 0.0f, avg_fps = 0.0f, dt = 0.0f, ai_clock = 0.0f;
      int32_t int_fps = 0;
      uint32_t frames = 0, vblanks = 0, last_frame_vblanks = 0, extra_wait = 0, state = 0;
      bool state_ok = false, vblank_ok = false, ai_ok = false;
      uint8_t adaptive = 0;
      // Host presents (SDK null_gpu present statistics, cumulative; the max fields only since the previous take:
      // State keeps the running maxima).
      rex::graphics::null_gpu::PresentStats ps{};
      // Front-end pacing and XUI timer counters (cumulative).
      uint64_t fe_frames = 0, fe_paced = 0, fe_waits = 0, fe_wait_us = 0, fe_timeouts = 0, xui_runs = 0, xui_fires = 0;
    };

    int64_t Qpc()
    {
      LARGE_INTEGER v;
      QueryPerformanceCounter(&v);
      return v.QuadPart;
    }

    double g_qpc_s = 0.0; // seconds per QPC tick

    Snap TakeSnap()
    {
      Snap s;
      uint8_t *base = Base();
      s.qpc = Qpc();
      s.ps = rex::graphics::null_gpu::TakePresentStats();
      s.fe_frames = g_fe_frames.load(std::memory_order_relaxed);
      s.fe_paced = g_fe_paced.load(std::memory_order_relaxed);
      s.fe_waits = g_fe_waits.load(std::memory_order_relaxed);
      s.fe_wait_us = g_fe_wait_us.load(std::memory_order_relaxed);
      s.fe_timeouts = g_fe_timeouts.load(std::memory_order_relaxed);
      s.xui_runs = g_xui_runs.load(std::memory_order_relaxed);
      s.xui_fires = g_xui_fires.load(std::memory_order_relaxed);
      uint64_t raw = 0;
      uint32_t frames = 0, clamped = 0, avg = 0, ifps = 0, dt = 0;
      if (!Read(base, kClockRawSum, 8, raw) || !Read32(base, kClockFrames, frames) ||
          !Read32(base, kClockClampedSum, clamped) || !Read32(base, kClockAvgFps, avg) ||
          !Read32(base, kClockIntFps, ifps) || !Read32(base, kClockDt, dt))
      {
        return s;
      }
      s.raw_sum = AsDouble(raw);
      s.clamped_sum = AsFloat(clamped);
      s.avg_fps = AsFloat(avg);
      s.int_fps = int32_t(ifps);
      s.dt = AsFloat(dt);
      s.frames = frames;
      s.ok = frames != 0 && std::isfinite(s.raw_sum);
      uint32_t state_ptr = 0;
      s.state_ok = Read32(base, kClockState, state_ptr) && state_ptr >= 0x10000u && Read32(base, state_ptr + 8, s.state);
      s.vblank_ok = Read32(base, kVblankCount, s.vblanks) && Read32(base, kVblankLastFrame, s.last_frame_vblanks) &&
                    Read32(base, kVblankExtraWait, s.extra_wait);
      uint64_t flag = 0;
      if (Read(base, kAdaptiveWaitFlag, 1, flag))
      {
        s.adaptive = uint8_t(flag);
      }
      uint32_t ai = 0;
      s.ai_ok = Read32(base, kAiClock, ai);
      s.ai_clock = AsFloat(ai);
      return s;
    }

    double Seconds(int64_t from, int64_t to) { return double(to - from) * g_qpc_s; }

    // ---------------------------------------------------------------------------------------------------------------
    // Cutscene segments (from the hook's atomics, sampled every tick).
    // ---------------------------------------------------------------------------------------------------------------
    struct CsCounts
    {
      uint64_t calls = 0, behind = 0, ahead = 0, real_time = 0, step_us = 0;
    };

    CsCounts ReadCsCounts()
    {
      CsCounts c;
      c.calls = g_cs_calls.load(std::memory_order_acquire);
      c.behind = g_cs_behind.load(std::memory_order_relaxed);
      c.ahead = g_cs_ahead.load(std::memory_order_relaxed);
      c.real_time = g_cs_real_time.load(std::memory_order_relaxed);
      c.step_us = g_cs_step_us.load(std::memory_order_relaxed);
      return c;
    }

    struct Segment
    {
      bool open = false;
      uint32_t player = 0;
      int64_t qpc0 = 0, qpc1 = 0;
      uint32_t picture0 = 0, picture1 = 0, wall0 = 0, wall1 = 0;
      CsCounts c0{}, c1{};
      rex::audio::OutputClock audio0{}, audio1{}; // the audio output's clock at qpc0 / qpc1
      int idle = 0;
    };

    struct State
    {
      std::mutex mutex; // the log thread's tick and LogSummary
      int64_t start_qpc = 0;
      Snap first{};       // the first ok sample
      Snap window{};      // the current 10 s window's start
      Snap last{};        // the latest ok sample
      // In-level windows (state 21 at both ends).
      double level_game_s = 0.0, level_wall_s = 0.0, level_min = 0.0, level_max = 0.0;
      uint32_t level_windows = 0;
      // Cutscenes.
      Segment seg{};
      uint32_t segments = 0, segments_counted = 0;
      double av_offset_max_ms = 0.0, av_underrun_ms = 0.0; // cutscenes of 2 s or more
      double cs_picture_s = 0.0, cs_clock_s = 0.0, cs_wall_s = 0.0;
      // Host presents: the longest Present call / latency wait in the current 10 s window, in the run, and the
      // in-level windows' totals (as level_game_s).
      uint64_t win_present_max_us = 0, win_wait_max_us = 0, all_present_max_us = 0, all_wait_max_us = 0;
      uint64_t level_presents = 0, level_present_us = 0, level_over_2ms = 0, level_repeats = 0, level_ui = 0,
               level_waits = 0, level_wait_us = 0;
      bool summary_done = false;
    };
    State g;
    std::atomic<bool> g_thread_started{false};
    std::atomic<bool> g_stop{false};
    std::atomic<bool> g_exited{false};

    std::string Pacing()
    {
      if (g_active)
      {
        return fmt::format("pdz_null_refresh_hz in effect: guest vblanks paced at {} Hz", g_hz);
      }
      return fmt::format("guest vblanks at video_mode_refresh_rate {} Hz (pdz_null_refresh_hz off)",
                         rex::cvar::GetFlagByName("video_mode_refresh_rate"));
    }

    const char *StepName(int mode)
    {
      switch (mode)
      {
      case 1:
        return "real time";
      case 3:
        return "60 Hz vblanks during cutscenes";
      default:
        return "original fixed 1/60 s";
      }
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Host presents (SDK null_gpu present statistics: the presenter's Present calls and its frame latency wait).
    // ---------------------------------------------------------------------------------------------------------------
    void TrackPresentMax(const Snap &s)
    {
      g.win_present_max_us = std::max(g.win_present_max_us, s.ps.present_max_us);
      g.win_wait_max_us = std::max(g.win_wait_max_us, s.ps.latency_wait_max_us);
      g.all_present_max_us = std::max(g.all_present_max_us, s.ps.present_max_us);
      g.all_wait_max_us = std::max(g.all_wait_max_us, s.ps.latency_wait_max_us);
    }

    std::string PresentPart(uint64_t presents, uint64_t present_us, uint64_t present_max_us, uint64_t over_2ms,
                            uint64_t repeats, uint64_t ui_paints, uint64_t waits, uint64_t wait_us,
                            uint64_t wait_max_us, uint64_t timeouts, uint32_t depth, double wall_s)
    {
      if (!presents)
      {
        return "host presents: none timed";
      }
      std::string wait =
          depth ? fmt::format("latency waitable depth {}: wait avg {:.3f} ms max {:.2f}, timeouts {}", depth,
                              waits ? double(wait_us) / double(waits) / 1000.0 : 0.0, double(wait_max_us) / 1000.0,
                              timeouts)
                : std::string("no latency waitable (DXGI queue of 3)");
      return fmt::format("host presents +{} ({:.1f}/s; same frame again {}, with UI {}): Present call avg {:.3f} ms max "
                         "{:.2f}, over 2 ms {} ({:.1f}%) | {}",
                         presents, wall_s > 0.0 ? double(presents) / wall_s : 0.0, repeats, ui_paints,
                         double(present_us) / double(presents) / 1000.0, double(present_max_us) / 1000.0, over_2ms,
                         100.0 * double(over_2ms) / double(presents), wait);
    }

    std::string PresentWindow(const Snap &a, const Snap &b, double wall_s, bool in_level)
    {
      const auto &p = a.ps, &q = b.ps;
      if (in_level)
      {
        g.level_presents += q.presents - p.presents;
        g.level_present_us += q.present_us - p.present_us;
        g.level_over_2ms += q.presents_over_2ms - p.presents_over_2ms;
        g.level_repeats += q.repeats - p.repeats;
        g.level_ui += q.ui_paints - p.ui_paints;
        g.level_waits += q.latency_waits - p.latency_waits;
        g.level_wait_us += q.latency_wait_us - p.latency_wait_us;
      }
      const std::string s = PresentPart(q.presents - p.presents, q.present_us - p.present_us, g.win_present_max_us,
                                        q.presents_over_2ms - p.presents_over_2ms, q.repeats - p.repeats,
                                        q.ui_paints - p.ui_paints, q.latency_waits - p.latency_waits,
                                        q.latency_wait_us - p.latency_wait_us, g.win_wait_max_us,
                                        q.latency_timeouts - p.latency_timeouts, q.frame_latency, wall_s);
      g.win_present_max_us = 0;
      g.win_wait_max_us = 0;
      return s;
    }

    void CloseSegment(const char *why)
    {
      Segment &s = g.seg;
      if (!s.open)
      {
        return;
      }
      s.open = false;
      ++g.segments;
      const double wall_s = Seconds(s.qpc0, s.qpc1);
      if (wall_s < kSegmentLogS)
      {
        return;
      }
      const double picture_s = double(int64_t(s.picture1) - int64_t(s.picture0)) / 1000.0;
      const double clock_s = double(int64_t(s.wall1) - int64_t(s.wall0)) / 1000.0;
      const uint64_t frames = s.c1.calls - s.c0.calls;
      const uint64_t behind = s.c1.behind - s.c0.behind, ahead = s.c1.ahead - s.c0.ahead;
      const uint64_t real_time = s.c1.real_time - s.c0.real_time;
      const double step_ms = frames ? double(s.c1.step_us - s.c0.step_us) / 1000.0 / double(frames) : 0.0;
      if (wall_s >= kSegmentTotalS)
      {
        ++g.segments_counted;
        g.cs_picture_s += picture_s;
        g.cs_clock_s += clock_s;
        g.cs_wall_s += wall_s;
      }
      // Picture / sound: the audio output's frames over the same QPC span, and the hook's offsets.
      const uint64_t out_frames = (s.audio1.played + s.audio1.silent) - (s.audio0.played + s.audio0.silent);
      const double out_s = double(out_frames) * rex::audio::kOutputFrameMs / 1000.0;
      const double underrun_ms = double(s.audio1.silent - s.audio0.silent) * rex::audio::kOutputFrameMs;
      const double offset_ms = double(g_cs_av_offset_us.load(std::memory_order_relaxed)) / 1000.0;
      const double offset_max_ms = double(g_cs_av_offset_max_us.load(std::memory_order_relaxed)) / 1000.0;
      const double clock_lead_ms = double(g_cs_av_clock_lead_us.load(std::memory_order_relaxed)) / 1000.0;
      const uint64_t max_frame = g_cs_av_offset_max_frame.load(std::memory_order_relaxed);
      const uint64_t av_frames = g_cs_av_frames.load(std::memory_order_relaxed);
      if (wall_s >= kSegmentTotalS)
      {
        g.av_offset_max_ms = std::max(g.av_offset_max_ms, std::fabs(offset_max_ms));
        g.av_underrun_ms += underrun_ms;
      }
      REXLOG_INFO("pdz_clock: cutscene {:08X} ({}): the picture moved {:.3f} s in {:.3f} s of wall clock (QPC): ratio "
                  "{:.4f}; its own clock {:.3f} s (ratio {:.4f}); {} frames ({:.1f}/s): behind its clock {} (catch-up "
                  "step), not behind {} ({} with the real-time step, the rest Rare's 1/60 s); mean step {:.2f} ms; "
                  "pdz_cutscene_step: {} | picture / sound: offset {:+.1f} ms at the end, largest {:+.1f} ms at "
                  "frame {} of {} (the picture's time - 66 ms - the lead - its sound's, after the first 5 frames; + = picture ahead); "
                  "lead for the frames in flight {:.1f} ms ({} frames, pdz_cutscene_av_latency_ms {}, pacing {}); the cutscene's ms "
                  "clock ran {:+.1f} ms ahead of its sound (Rare's picture follows that clock); the sound fell {:.1f} "
                  "ms behind real time (audio output underruns), {} frames set by the sound (pdz_cutscene_audio_sync "
                  "{}); audio output clock {:.3f} s in the same span (ratio {:.4f}){}",
                  s.player, why, picture_s, wall_s, picture_s / wall_s, clock_s, clock_s / wall_s, frames,
                  double(frames) / wall_s, behind, ahead, real_time, step_ms, StepName(StepMode()), offset_ms,
                  offset_max_ms, max_frame, av_frames,
                  double(g_cs_av_lead_us.load(std::memory_order_relaxed)) / 1000.0, g_frames_in_flight,
                  REXCVAR_GET(pdz_cutscene_av_latency_ms), PacingName(), clock_lead_ms, underrun_ms, g_cs_av_synced.load(std::memory_order_relaxed),
                  REXCVAR_GET(pdz_cutscene_audio_sync) ? "on" : "off", out_s, wall_s > 0.0 ? out_s / wall_s : 0.0,
                  wall_s < kSegmentTotalS ? " (under 2 s: not in the SUMMARY ratio)" : "");
    }

    void TickCutscene(int64_t qpc)
    {
      const CsCounts c = ReadCsCounts();
      Segment &s = g.seg;
      if (s.open && c.calls == s.c1.calls)
      {
        if (++s.idle >= kCutsceneIdleTicks)
        {
          CloseSegment("ended");
        }
      }
      else if (c.calls != s.c1.calls || !s.open)
      {
        if (c.calls == 0)
        {
          return; // no cutscene yet
        }
        if (!s.open && c.calls == s.c1.calls)
        {
          return; // nothing new since the last closed segment
        }
        const uint32_t player = g_cs_this.load(std::memory_order_relaxed);
        const uint64_t packed = g_cs_last.load(std::memory_order_relaxed);
        const uint32_t picture = uint32_t(packed >> 32), wall = uint32_t(packed);
        if (s.open && (player != s.player || picture < s.picture1 || wall < s.wall1))
        {
          CloseSegment("next cutscene");
        }
        if (!s.open)
        {
          s = Segment{};
          s.open = true;
          s.player = player;
          s.qpc0 = qpc;
          s.picture0 = picture;
          s.wall0 = wall;
          s.c0 = c;
          s.audio0 = rex::audio::GetOutputClock();
        }
        s.qpc1 = qpc;
        s.picture1 = picture;
        s.wall1 = wall;
        s.c1 = c;
        s.audio1 = rex::audio::GetOutputClock();
        s.idle = 0;
      }
      // pdz_cutscene_step 3: back to the scene's rate once no cutscene frame came for a tick.
      const uint32_t scene_divider = g_mode_divider.load(std::memory_order_relaxed);
      if (g_divider.load(std::memory_order_relaxed) != scene_divider && (!s.open || s.idle > 0))
      {
        SetDivider(scene_divider);
      }
    }

    // ---------------------------------------------------------------------------------------------------------------
    // The 10 s line and the in-level totals.
    // ---------------------------------------------------------------------------------------------------------------
    // Front-end pacing (present steps outside state 21) and XUI timers between two snapshots. XUI's 16 ms control
    // timers fire at most once per XuiTimersRun call: fires per second follow min(frames/s, ~60) per timer, so equal
    // rates at 60 and 120 frames/s mean the menus' timer-driven animations keep their speed.
    std::string FrontEndPart(const Snap &a, const Snap &b, double wall_s)
    {
      const uint64_t frames = b.fe_frames - a.fe_frames, paced = b.fe_paced - a.fe_paced;
      const uint64_t waits = b.fe_waits - a.fe_waits, timeouts = b.fe_timeouts - a.fe_timeouts;
      const uint64_t runs = b.xui_runs - a.xui_runs, fires = b.xui_fires - a.xui_fires;
      return fmt::format(" | front end: frames +{} ({:.1f}/s), paced by pdz_fix_frontend_pacing {}, of them waited {} "
                         "(avg {:.2f} ms){} | xui timers: runs +{} ({:.1f}/s), fires +{} ({:.1f}/s, {:.2f} per run)",
                         frames, double(frames) / wall_s, paced, waits,
                         waits ? double(b.fe_wait_us - a.fe_wait_us) / 1000.0 / double(waits) : 0.0,
                         timeouts ? fmt::format(", {} cut at 250 ms", timeouts) : std::string(), runs,
                         double(runs) / wall_s, fires, double(fires) / wall_s,
                         runs ? double(fires) / double(runs) : 0.0);
    }

    void LogWindow(const Snap &a, const Snap &b)
    {
      const double wall_s = Seconds(a.qpc, b.qpc);
      if (wall_s <= 0.0)
      {
        return;
      }
      const double game_s = b.raw_sum - a.raw_sum;
      const double clamped_s = double(b.clamped_sum) - double(a.clamped_sum);
      const uint32_t frames = b.frames - a.frames;
      const uint32_t vblanks = b.vblank_ok && a.vblank_ok ? b.vblanks - a.vblanks : 0;
      const bool in_level = a.state_ok && b.state_ok && a.state == kStateInLevel && b.state == kStateInLevel;
      if (in_level && game_s > 0.0)
      {
        const double ratio = game_s / wall_s;
        g.level_min = g.level_windows ? std::min(g.level_min, ratio) : ratio;
        g.level_max = g.level_windows ? std::max(g.level_max, ratio) : ratio;
        ++g.level_windows;
        g.level_game_s += game_s;
        g.level_wall_s += wall_s;
      }
      std::string ai;
      if (a.ai_ok && b.ai_ok && b.ai_clock >= a.ai_clock)
      {
        ai = fmt::format(", AI clock {:.4f}", (double(b.ai_clock) - double(a.ai_clock)) / wall_s);
      }
      const std::string presents = PresentWindow(a, b, wall_s, in_level);
      REXLOG_INFO("pdz_clock: t={:.0f}s | game clock +{:.3f} s in {:.3f} s of wall clock (QPC): ratio {:.4f} (raw dt sum), "
                  "{:.4f} (clamped{}) | frames +{} ({:.1f}/s; the game's fps avg {:.1f}, int {}, dt {:.2f} ms) | guest "
                  "vblanks +{} ({:.1f}/s, {:.2f} per frame; last frame {}, extra wait {}{}) | state {}{}{} | {}",
                  Seconds(g.start_qpc, b.qpc), game_s, wall_s, game_s / wall_s, clamped_s / wall_s, ai, frames,
                  double(frames) / wall_s, b.avg_fps, b.int_fps, double(b.dt) * 1000.0, vblanks, double(vblanks) / wall_s,
                  frames ? double(vblanks) / double(frames) : 0.0, b.last_frame_vblanks, b.extra_wait,
                  b.adaptive ? ", adaptive on" : "", b.state_ok ? std::to_string(b.state) : std::string("?"),
                  in_level ? " (in a level)" : "", FrontEndPart(a, b, wall_s), presents);
    }

    void Tick()
    {
      std::lock_guard<std::mutex> lock(g.mutex);
      if (g_cutscene_log_only)
      {
        TickCutscene(Qpc()); // one pdz_clock line per cutscene, nothing else
        return;
      }
      const Snap s = TakeSnap();
      TrackPresentMax(s);
      TickCutscene(s.qpc);
      if (!s.ok)
      {
        return;
      }
      if (!g.first.ok)
      {
        g.first = s;
        g.window = s;
        REXLOG_INFO("pdz_clock: first game clock sample (frame {}, game clock {:.3f} s); {}; cutscene step: {}",
                    s.frames, s.raw_sum, Pacing(), StepName(StepMode()));
      }
      g.last = s;
      if (Seconds(g.window.qpc, s.qpc) >= kWindowS)
      {
        LogWindow(g.window, s);
        g.window = s;
      }
    }

    DWORD WINAPI ThreadMain(void *)
    {
      pdz::cpu::PlaceHelperThread("game clock log"); // pdz_cpu_placement
      using SetThreadDescriptionFn = HRESULT(WINAPI *)(HANDLE, PCWSTR);
      if (HMODULE kernel = GetModuleHandleW(L"kernel32.dll"))
      {
        if (auto set_name = reinterpret_cast<SetThreadDescriptionFn>(GetProcAddress(kernel, "SetThreadDescription")))
        {
          set_name(GetCurrentThread(), L"pdz game clock log");
        }
      }
      while (!g_stop.load(std::memory_order_acquire))
      {
        Sleep(kTickMs);
        if (g_stop.load(std::memory_order_acquire))
        {
          break;
        }
        Tick();
      }
      g_exited.store(true, std::memory_order_release);
      return 0;
    }

    void StopAtExit()
    {
      g_stop.store(true, std::memory_order_release);
      for (int i = 0; i < 100 && !g_exited.load(std::memory_order_acquire); ++i)
      {
        Sleep(10);
      }
    }

    void StartThread()
    {
      bool expected = false;
      if (!g_thread_started.compare_exchange_strong(expected, true))
      {
        return;
      }
      LARGE_INTEGER f;
      QueryPerformanceFrequency(&f);
      g_qpc_s = 1.0 / double(f.QuadPart);
      g.start_qpc = Qpc();
      HANDLE thread = CreateThread(nullptr, 256 * 1024, ThreadMain, nullptr, 0, nullptr);
      if (!thread)
      {
        g_exited.store(true, std::memory_order_release);
        REXLOG_WARN("pdz_clock: the log thread could not be created: no game clock log");
        return;
      }
      SetThreadPriority(thread, THREAD_PRIORITY_BELOW_NORMAL);
      CloseHandle(thread);
      std::atexit(StopAtExit);
      if (g_cutscene_log_only)
      {
        REXLOG_INFO("pdz_clock: cutscene lines on (pdz_cutscene_audio_sync {}): one 'pdz_clock: cutscene' line per "
                    "cutscene of 1 s or more (picture / sound offset, lead, underruns); pdz_game_clock_log adds the rest",
                    REXCVAR_GET(pdz_cutscene_audio_sync) ? "on" : "off");
        return;
      }
      REXLOG_INFO("pdz_clock: on ({}): the game clock, frames and guest vblanks against the wall clock every 10 s, one "
                  "line per cutscene, a SUMMARY at test exit",
                  g_active ? "pdz_null_refresh_hz" : "pdz_game_clock_log");
    }

    bool FlagTrue(const char *name, bool missing)
    {
      const std::string v = rex::cvar::GetFlagByName(name);
      if (v.empty())
      {
        return missing;
      }
      return v == "true" || v == "1";
    }

    uint32_t PrimaryDisplayHz()
    {
      DEVMODEW mode{};
      mode.dmSize = sizeof(mode);
      if (!EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode) || mode.dmDisplayFrequency <= 1)
      {
        return 0;
      }
      return uint32_t(mode.dmDisplayFrequency);
    }

    // pdz_present_pacing (log 272): the primary display's exact refresh (its signal's, e.g. 119.88; EnumDisplaySettings
    // rounds) and whether its EDID advertises variable refresh rate. The window opens there (monitor 0).
    struct DisplayInfo
    {
      double hz = 0.0; // 0 = unknown
      std::string name;
      bool edid = false; // read
      bool vrr = false;  // advertised
      uint32_t vrr_min = 0, vrr_max = 0;
      std::string vrr_how;
    };

    // EDID: CTA-861 HDMI Forum VSDB / SCDB (OUI C4-5D-D8 or extended tag 0x79: VRRmin byte 9 bits 0-5, VRRmax byte 9
    // bits 6-7 + byte 10), AMD's FreeSync VSDB (OUI 00-00-1A: range bytes 6 / 7), DisplayID 2.x Adaptive-Sync block
    // (tag 0x2B). A plain range limits descriptor isn't taken (fixed displays have one too).
    void ParseEdidVrr(const uint8_t *e, size_t n, DisplayInfo &d)
    {
      static constexpr uint8_t kHeader[8] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
      if (n < 128 || std::memcmp(e, kHeader, 8) != 0)
      {
        return;
      }
      d.edid = true;
      const size_t blocks = std::min<size_t>(n / 128, size_t(e[126]) + 1);
      for (size_t i = 1; i < blocks && !d.vrr; ++i)
      {
        const uint8_t *b = e + i * 128;
        if (b[0] == 0x02)
        {
          const size_t end = std::min<size_t>(b[2] >= 4 ? b[2] : 4, 127);
          for (size_t pos = 4; pos < end && !d.vrr;)
          {
            const uint8_t *p = b + pos;
            const uint32_t tag = p[0] >> 5, len = p[0] & 31;
            if (pos + 1 + len > end)
            {
              break;
            }
            const uint32_t oui = len >= 3 ? uint32_t(p[1]) | uint32_t(p[2]) << 8 | uint32_t(p[3]) << 16 : 0;
            if (((tag == 3 && oui == 0xC45DD8) || (tag == 7 && len >= 1 && p[1] == 0x79)) && len >= 10 &&
                (p[9] & 0x3F) != 0)
            {
              d.vrr = true;
              d.vrr_min = p[9] & 0x3F;
              d.vrr_max = uint32_t(p[9] & 0xC0) << 2 | p[10];
              d.vrr_how = tag == 3 ? "HDMI VRR (HDMI Forum VSDB)" : "HDMI VRR (HDMI Forum SCDB)";
            }
            else if (tag == 3 && oui == 0x00001A && len >= 5)
            {
              d.vrr = true;
              if (len >= 7 && p[6] && p[7] > p[6])
              {
                d.vrr_min = p[6];
                d.vrr_max = p[7];
              }
              d.vrr_how = "FreeSync (AMD VSDB)";
            }
            pos += 1 + len;
          }
        }
        else if (b[0] == 0x70)
        {
          const size_t end = std::min<size_t>(5 + size_t(b[2]), 127);
          for (size_t pos = 5; pos + 3 <= end && !d.vrr;)
          {
            if (b[pos] == 0x2B)
            {
              d.vrr = true;
              d.vrr_how = "Adaptive-Sync (DisplayID)";
            }
            pos += 3 + size_t(b[pos + 2]);
          }
        }
      }
    }

    // pdz_display_switch_120: the primary display to 120 Hz at its current size (CDS_FULLSCREEN: for this process
    // only, undone at exit). True when it now runs at 119-121 Hz.
    bool SwitchPrimaryTo120(std::string &note)
    {
      MONITORINFOEXW monitor{};
      monitor.cbSize = sizeof(monitor);
      if (!GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &monitor))
      {
        note = "no primary display";
        return false;
      }
      DEVMODEW current{};
      current.dmSize = sizeof(current);
      if (!EnumDisplaySettingsW(monitor.szDevice, ENUM_CURRENT_SETTINGS, &current))
      {
        note = "its current mode can't be read";
        return false;
      }
      DEVMODEW best{};
      bool found = false;
      for (DWORD i = 0;; ++i)
      {
        DEVMODEW m{};
        m.dmSize = sizeof(m);
        if (!EnumDisplaySettingsW(monitor.szDevice, i, &m))
        {
          break;
        }
        if (m.dmPelsWidth == current.dmPelsWidth && m.dmPelsHeight == current.dmPelsHeight &&
            m.dmBitsPerPel == current.dmBitsPerPel && (m.dmDisplayFrequency == 120 || m.dmDisplayFrequency == 119))
        {
          best = m;
          found = true;
          if (m.dmDisplayFrequency == 120)
          {
            break;
          }
        }
      }
      if (!found)
      {
        note = fmt::format("it has no 120 Hz mode at {}x{}", current.dmPelsWidth, current.dmPelsHeight);
        return false;
      }
      best.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL | DM_DISPLAYFREQUENCY;
      const LONG r = ChangeDisplaySettingsExW(monitor.szDevice, &best, nullptr, CDS_FULLSCREEN, nullptr);
      if (r != DISP_CHANGE_SUCCESSFUL)
      {
        note = fmt::format("the switch to {} Hz failed ({})", best.dmDisplayFrequency, r);
        return false;
      }
      note = fmt::format("switched from {} Hz to {} Hz at {}x{} for this run", current.dmDisplayFrequency,
                         best.dmDisplayFrequency, best.dmPelsWidth, best.dmPelsHeight);
      return true;
    }

    DisplayInfo PrimaryDisplayInfo()
    {
      DisplayInfo d;
      MONITORINFOEXW monitor{};
      monitor.cbSize = sizeof(monitor);
      if (!GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &monitor))
      {
        return d;
      }
      UINT32 path_count = 0, mode_count = 0;
      if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count) != ERROR_SUCCESS)
      {
        return d;
      }
      std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
      std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
      if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(), &mode_count, modes.data(), nullptr) !=
          ERROR_SUCCESS)
      {
        return d;
      }
      for (UINT32 i = 0; i < path_count; ++i)
      {
        const DISPLAYCONFIG_PATH_INFO &path = paths[i];
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = path.sourceInfo.adapterId;
        source.header.id = path.sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS ||
            wcscmp(source.viewGdiDeviceName, monitor.szDevice) != 0)
        {
          continue;
        }
        const UINT32 mode_index = path.targetInfo.modeInfoIdx;
        if (mode_index < mode_count && modes[mode_index].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_TARGET)
        {
          const DISPLAYCONFIG_RATIONAL &v = modes[mode_index].targetMode.targetVideoSignalInfo.vSyncFreq;
          d.hz = v.Denominator ? double(v.Numerator) / double(v.Denominator) : 0.0;
        }
        if (d.hz <= 1.0 && path.targetInfo.refreshRate.Denominator)
        {
          d.hz = double(path.targetInfo.refreshRate.Numerator) / double(path.targetInfo.refreshRate.Denominator);
        }
        DISPLAYCONFIG_TARGET_DEVICE_NAME target{};
        target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        target.header.size = sizeof(target);
        target.header.adapterId = path.targetInfo.adapterId;
        target.header.id = path.targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&target.header) != ERROR_SUCCESS)
        {
          break;
        }
        for (const wchar_t *c = target.monitorFriendlyDeviceName; *c; ++c)
        {
          d.name += *c >= 32 && *c < 127 ? char(*c) : '?';
        }
        // \\?\DISPLAY#GSM8221#5&2a..&0&UID4352#{guid} -> HKLM\SYSTEM\CurrentControlSet\Enum\DISPLAY\GSM8221\5&2a..
        // \Device Parameters, value EDID (readable without elevation).
        std::wstring device = target.monitorDevicePath;
        if (device.rfind(L"\\\\?\\", 0) == 0)
        {
          device = device.substr(4);
        }
        std::wstring parts[3];
        size_t start = 0;
        for (int k = 0; k < 3; ++k)
        {
          const size_t hash = device.find(L'#', start);
          parts[k] = device.substr(start, hash == std::wstring::npos ? std::wstring::npos : hash - start);
          start = hash == std::wstring::npos ? device.size() : hash + 1;
        }
        const std::wstring key = L"SYSTEM\\CurrentControlSet\\Enum\\" + parts[0] + L"\\" + parts[1] + L"\\" +
                                 parts[2] + L"\\Device Parameters";
        uint8_t edid[1024];
        DWORD size = sizeof(edid);
        if (RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), L"EDID", RRF_RT_REG_BINARY, nullptr, edid, &size) ==
            ERROR_SUCCESS)
        {
          ParseEdidVrr(edid, size, d);
        }
        break;
      }
      if (d.hz <= 1.0)
      {
        d.hz = double(PrimaryDisplayHz());
      }
      return d;
    }

    // DXGI's tearing support (DXGI_FEATURE_PRESENT_ALLOW_TEARING), which a VRR present needs in a flip-model swap
    // chain; dxgi.dll loaded by name like native_cvars.cpp.
    bool DxgiAllowsTearing()
    {
      using CreateFactoryFn = HRESULT(WINAPI *)(REFIID, void **);
      HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
      auto create = dxgi ? reinterpret_cast<CreateFactoryFn>(
                               reinterpret_cast<void *>(GetProcAddress(dxgi, "CreateDXGIFactory1")))
                         : nullptr;
      IDXGIFactory5 *factory = nullptr;
      BOOL allow = FALSE;
      if (create && SUCCEEDED(create(__uuidof(IDXGIFactory5), reinterpret_cast<void **>(&factory))) && factory)
      {
        if (FAILED(factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof(allow))))
        {
          allow = FALSE;
        }
        factory->Release();
      }
      return allow != FALSE;
    }

    // pdz_sp_120's present pacing (pdz_present_pacing, log 272); returns the guest vblank rate. Logs one line.
    // pdz_present_pacing, lower case; an unknown value is the default (fixed).
    std::string WantedPacing(bool warn)
    {
      std::string want = REXCVAR_GET(pdz_present_pacing);
      for (char &c : want)
      {
        c = char(std::tolower(static_cast<unsigned char>(c)));
      }
      if (want == "auto")
      {
        // No Auto since 2026-11-06 (the settings window offers On = fixed, VRR, Off): an older settings file's auto is
        // fixed. Said once.
        static std::atomic<bool> noted{false};
        if (!noted.exchange(true))
        {
          REXLOG_INFO("pdz_pace: pdz_present_pacing auto (an older settings file) is read as fixed: the display's EDID "
                      "can't tell whether VRR is on in the graphics driver; choose VRR in the settings window if it is");
        }
        want = "fixed";
      }
      if (want != "vrr" && want != "fixed" && want != "latch" && want != "present")
      {
        if (warn)
        {
          REXLOG_WARN("pdz_pace: pdz_present_pacing '{}' unknown (fixed, vrr, latch, present): fixed", want);
        }
        want = "fixed";
      }
      return want;
    }

    // pdz_present_pacing auto / present (Deck log): whether a compositor owns the display. Wine / Proton = ntdll's
    // wine_get_version export; gamescope / a Steam Deck = the environment Steam gives a game there (Wine passes the
    // Linux environment through).
    struct HostInfo
    {
      std::string wine; // the Wine version; empty = not Wine
      bool gamescope = false;
      bool deck = false;
      std::string env; // the variables found, for the log
    };

    std::string EnvVar(const char *name)
    {
      char buf[256];
      const DWORD n = GetEnvironmentVariableA(name, buf, DWORD(sizeof(buf)));
      return n && n < sizeof(buf) ? std::string(buf, n) : std::string();
    }

    HostInfo DetectHost()
    {
      HostInfo h;
      if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll"))
      {
        using WineVersionFn = const char *(__cdecl *)();
        if (auto fn = reinterpret_cast<WineVersionFn>(reinterpret_cast<void *>(GetProcAddress(ntdll, "wine_get_version"))))
        {
          const char *v = fn();
          h.wine = v && *v ? v : "unknown version";
        }
      }
      const std::string deck = EnvVar("SteamDeck");
      const std::string gamescope = EnvVar("SteamGamescope");
      const std::string wayland = EnvVar("GAMESCOPE_WAYLAND_DISPLAY");
      h.deck = deck == "1";
      h.gamescope = (!gamescope.empty() && gamescope != "0") || !wayland.empty();
      for (const auto &[name, value] : {std::pair<const char *, const std::string &>{"SteamDeck", deck},
                                        {"SteamGamescope", gamescope},
                                        {"GAMESCOPE_WAYLAND_DISPLAY", wayland}})
      {
        if (!value.empty())
        {
          h.env += fmt::format("{}{}={}", h.env.empty() ? "" : ", ", name, value);
        }
      }
      return h;
    }

    // pdz_present_pacing present: logs one line, sets the SDK's present pacing.
    void ChoosePresentMode(const std::string &why, const HostInfo &host, uint32_t reported_hz, double guest_hz)
    {
      g_present_mode = true;
      g_pacing_name = "present";
      g_frames_in_flight = (REXCVAR_GET(pdz_native_record_thread) ? 1u : 0u) + 2u;
      rex::graphics::null_gpu::SetPresentPacing(rex::graphics::null_gpu::PresentPacing::kPresent);
      rex::graphics::null_gpu::SetPresentMaxFrameLatency(1);
      REXLOG_INFO("pdz_pace: present pacing present ({}): every display refresh presented with Present(1, 0) (no "
                  "tearing) on a present queue of 1; the game's vblanks ({:.0f} Hz) counted from those presents, one "
                  "every round({:.0f} Hz / refresh) refreshes, no timer of ours (an even timer only while presents "
                  "stop or the measured refresh isn't a multiple: at 90 Hz 60 fps can't be even, set the display to "
                  "60 Hz) | Wine {}, gamescope {}, Steam Deck {} ({}), the display reports {} Hz (under a compositor "
                  "that is the panel's, not necessarily the rate it runs at; the 'present pacing:' lines measure it); "
                  "vsync {}; cutscene picture lead {} frames in flight",
                  why, guest_hz, guest_hz, host.wine.empty() ? "no" : "yes (" + host.wine + ")",
                  host.gamescope ? "yes" : "no", host.deck ? "yes" : "no",
                  host.env.empty() ? std::string("no SteamDeck / SteamGamescope / GAMESCOPE_WAYLAND_DISPLAY")
                                   : host.env,
                  reported_hz, FlagTrue("vsync", true) ? "on" : "OFF (nothing is paced)", g_frames_in_flight);
    }

    double ChoosePresentPacing(uint32_t hz)
    {
      if (g_present_mode)
      {
        // Chosen (and logged) by ChooseAtStartup: 120 Hz guest vblanks, every refresh at 120 Hz, every second for the
        // 60 fps scenes (the divider).
        REXLOG_INFO("pdz_pace: pdz_sp_120 with present pacing: guest vblanks at {} Hz (single player one per refresh "
                    "on a 120 Hz display; co-op, multiplayer and the front end every second)",
                    hz);
        return double(hz);
      }
      const std::string want = WantedPacing(true);
      DisplayInfo d = PrimaryDisplayInfo();
      // Above 120 Hz where 120 isn't a division of the refresh (144, 165): 120 Hz if the display has it, else VRR
      // capped at exactly 120, else the even division (pdz_display_switch_120).
      std::string switch_note;
      bool cap_120 = false;
      const bool odd_high = d.hz > 121.5 && std::fabs(d.hz / std::round(d.hz / 120.0) - 120.0) > 1.5;
      if (odd_high && want == "auto" && REXCVAR_GET(pdz_sp_120) && REXCVAR_GET(pdz_display_switch_120))
      {
        const double before = d.hz;
        if (SwitchPrimaryTo120(switch_note))
        {
          d = PrimaryDisplayInfo();
          if (d.hz < 118.0 || d.hz > 122.0)
          {
            d.hz = 120.0; // the new mode isn't reported yet
          }
        }
        else if (d.vrr)
        {
          cap_120 = true;
          switch_note += "; VRR capped at exactly 120 fps instead";
        }
        else
        {
          switch_note += fmt::format("; no VRR either: the campaign runs at {:.0f} fps ({:.0f} Hz / {:.0f}). Set the "
                                     "display to 120 Hz in Windows for 120 fps",
                                     before / std::ceil(before / 121.5), before, std::ceil(before / 121.5));
        }
        REXLOG_INFO("pdz_pace: display {:.0f} Hz: {}", before, switch_note);
      }
      const bool tearing = DxgiAllowsTearing();
      const bool allow_tearing = FlagTrue("d3d12_allow_variable_refresh_rate_and_tearing", true);
      const bool fullscreen = FlagTrue("fullscreen", false);
      std::string mode = want, why;
      if (want == "auto")
      {
        why = !d.vrr                ? (d.edid ? "the display's EDID advertises no variable refresh rate"
                                              : "the display's EDID could not be read")
              : !tearing            ? "DXGI does not allow tearing"
              : !allow_tearing      ? "d3d12_allow_variable_refresh_rate_and_tearing is off"
              : !fullscreen         ? "windowed (fullscreen is off)"
                                    : "";
        mode = why.empty() ? "vrr" : "fixed";
        if (why.empty())
        {
          why = "auto: the display's EDID advertises " + d.vrr_how;
        }
        else
        {
          why = "auto: " + why + " (pdz_present_pacing vrr forces it)";
        }
      }
      else
      {
        why = "pdz_present_pacing " + want;
      }
      const double display_hz = d.hz > 1.0 ? d.hz : double(hz);
      const bool record_thread = REXCVAR_GET(pdz_native_record_thread);
      double vblank_hz = double(hz);
      std::string how;
      if (mode == "vrr")
      {
        g_vrr = true;
        g_vrr_cap_hz = cap_120 ? 120.0
                               : std::clamp(std::floor(display_hz) - double(REXCVAR_GET(pdz_vrr_cap_below)), 30.0, 120.0);
        g_pacing_name = "vrr";
        g_frames_in_flight = (record_thread ? 1u : 0u) + 1u;
        rex::graphics::null_gpu::SetPresentPacing(rex::graphics::null_gpu::PresentPacing::kImmediate);
        how = fmt::format("VRR on: each frame presented as soon as it is ready (flip model, Present(0) with tearing "
                          "allowed{}: the display times it); single player levels capped at {:.0f} fps "
                          "(pdz_vrr_cap_below {}), front end / co-op / multiplayer 60 (guest vblanks {} Hz / 2)",
                          tearing && allow_tearing ? "" : ", BUT tearing is not allowed here", g_vrr_cap_hz,
                          REXCVAR_GET(pdz_vrr_cap_below), hz);
      }
      else if (mode == "fixed")
      {
        // Lock to the display's real refresh (119.88 on a 119.88 Hz display, never an assumed 120): the refresh itself
        // up to 120 Hz (+ 1.5 for rounding), above that refresh / n with the smallest n that brings it to 120 or less,
        // so every guest vblank is n display vblanks (144 -> 72, 165 -> 82.5, 240 -> 120). The host grid divisor
        // (presenter.cpp GetHostVblankDivisor: round(display / guest)) is then n.
        const double n = std::max(1.0, std::ceil(display_hz / 121.5));
        vblank_hz = display_hz / n;
        // Log 281: the presenter's phase lock started from EnumDisplaySettings' integer and allowed its period estimate
        // 1 % either way; on the 120.000 Hz G-Sync TV it walked to the -1 % bound (guest vblanks every 8.25 ms, a frame
        // dropped about once a second). With the exact rate it starts there and stays within 0.05 %.
        if (d.hz > 1.0)
        {
          rex::graphics::null_gpu::SetDisplayRefreshHz(d.hz);
        }
        g_pacing_name = "fixed";
        g_frames_in_flight = (record_thread ? 1u : 0u) + 2u;
        how = fmt::format("VRR off: guest vblanks at {:.3f} Hz = the display's {:.3f} Hz / {:.0f}, phase-locked to its "
                          "vblank (host_vblank_pacing), each frame presented once at its vblank (Present(1, 0))",
                          vblank_hz, display_hz, n);
      }
      else
      {
        g_pacing_name = "latch";
        g_frames_in_flight = (record_thread ? 1u : 0u) + 2u;
        how = "the pacing before log 272: guest vblanks at 120 Hz latch the newest frame, Present(1, 0)";
      }
      REXLOG_INFO("pdz_pace: present pacing {} ({}): {} | display '{}' {:.3f} Hz, EDID {}{}; DXGI tearing {}, "
                  "d3d12_allow_variable_refresh_rate_and_tearing {}, fullscreen {}; cutscene picture lead {} frames "
                  "in flight (pdz_cutscene_av_latency_ms {}); pdz_present_pacing latch = the old pacing",
                  mode, why, how, d.name, display_hz, !d.edid ? "unread" : d.vrr ? d.vrr_how : "no VRR",
                  d.vrr && d.vrr_max ? fmt::format(" {}-{} Hz", d.vrr_min, d.vrr_max) : std::string(),
                  tearing ? "yes" : "no", allow_tearing ? "true" : "false", fullscreen ? "true" : "false",
                  g_frames_in_flight, REXCVAR_GET(pdz_cutscene_av_latency_ms));
      return vblank_hz;
    }
  } // namespace

  namespace
  {
    // The start-up summary line (release item, 2026-11-04): what the player's PC is and what was chosen for it.
    void LogStartupSummary()
    {
      std::string gpu = "unknown";
      using CreateFactoryFn = HRESULT(WINAPI *)(REFIID, void **);
      HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
      auto create = dxgi ? reinterpret_cast<CreateFactoryFn>(
                               reinterpret_cast<void *>(GetProcAddress(dxgi, "CreateDXGIFactory1")))
                         : nullptr;
      IDXGIFactory1 *factory = nullptr;
      if (create && SUCCEEDED(create(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory))) && factory)
      {
        uint64_t best = 0;
        IDXGIAdapter1 *adapter = nullptr;
        for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
        {
          DXGI_ADAPTER_DESC1 desc{};
          if (SUCCEEDED(adapter->GetDesc1(&desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
              desc.DedicatedVideoMemory >= best)
          {
            best = desc.DedicatedVideoMemory;
            std::wstring w(desc.Description);
            const char *make = desc.VendorId == 0x10DE   ? "NVIDIA"
                               : desc.VendorId == 0x1002 ? "AMD"
                               : desc.VendorId == 0x8086 ? "Intel"
                                                         : "other";
            const std::string name(w.begin(), w.end());
            // The adapter name usually starts with the maker already ("NVIDIA GeForce ...").
            gpu = name.rfind(make, 0) == 0 ? fmt::format("{} ({} MB)", name, best >> 20)
                                           : fmt::format("{} {} ({} MB)", make, name, best >> 20);
          }
          adapter->Release();
        }
        factory->Release();
      }
      uint32_t physical = 0;
      DWORD bytes = 0;
      GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &bytes);
      std::vector<uint8_t> info(bytes);
      if (bytes && GetLogicalProcessorInformationEx(RelationProcessorCore,
                                                    reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *>(info.data()),
                                                    &bytes))
      {
        for (DWORD at = 0; at < bytes;)
        {
          const auto *e = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *>(info.data() + at);
          physical += e->Relationship == RelationProcessorCore ? 1 : 0;
          at += e->Size;
        }
      }
      const DisplayInfo d = PrimaryDisplayInfo();
      const int32_t workers = rex::cvar::GetFlagByName("pdz_native_record_workers").empty()
                                  ? 0
                                  : std::atoi(rex::cvar::GetFlagByName("pdz_native_record_workers").c_str());
      const std::string second = workers == 2   ? "on (pdz_native_record_workers 2)"
                                 : workers == 1 ? "off (pdz_native_record_workers 1)"
                                 : physical >= 6 ? fmt::format("on (auto: {} physical cores)", physical)
                                                 : fmt::format("off (auto: {} physical cores)", physical);
      REXLOG_INFO("pdz: this PC: GPU {} | display {:.2f} Hz, VRR {} | pacing {} ({} Hz guest vblanks, campaign {}) | "
                  "CPU {} cores, {} threads | second record thread {}",
                  gpu, d.hz, d.vrr ? "yes (" + d.vrr_how + ")" : (d.edid ? "no" : "unknown"), g_pacing_name,
                  EffectiveHz(), REXCVAR_GET(pdz_sp_120) ? "auto 120 / 60" : "60", physical,
                  std::thread::hardware_concurrency(), second);
    }
  } // namespace

  static void ChooseAtStartupImpl(bool null_backend);

  void ChooseAtStartup(bool null_backend)
  {
    ChooseAtStartupImpl(null_backend);
    LogStartupSummary();
  }

  static void ChooseAtStartupImpl(bool null_backend)
  {
    int32_t want = REXCVAR_GET(pdz_null_refresh_hz);
    bool log = REXCVAR_GET(pdz_game_clock_log);
    // pdz_sp_120: 120 Hz pacing with the rate chosen per scene, on a display that does 120 (pdz_null_refresh_hz set
    // = that fixed rate instead).
    bool auto120 = false;
    // pdz_present_pacing present / auto (Deck log: gamescope): a compositor that owns the display, or a reported
    // refresh that isn't a multiple of the game's rate (only without pdz_sp_120: fixed divides any display).
    {
      const std::string wanted = WantedPacing(false);
      const HostInfo host = DetectHost();
      const uint32_t reported = PrimaryDisplayHz();
      const bool sp120 = want == 0 && REXCVAR_GET(pdz_sp_120);
      double guest_hz = want > 0 ? double(want) : 0.0;
      if (guest_hz <= 0.0)
      {
        const std::string v = rex::cvar::GetFlagByName("video_mode_refresh_rate");
        guest_hz = v.empty() ? 60.0 : std::atof(v.c_str());
        if (guest_hz < 24.0)
        {
          guest_hz = 60.0;
        }
      }
      const double ratio = reported ? double(reported) / guest_hz : 0.0;
      const bool multiple = ratio >= 0.99 && std::abs(double(reported) - std::round(ratio) * guest_hz) <= 1.5;
      std::string why;
      if (wanted == "present")
      {
        why = "pdz_present_pacing present";
      }
      else if (wanted == "auto")
      {
        if (!host.wine.empty())
        {
          why = "auto: Wine / Proton";
        }
        else if (host.gamescope || host.deck)
        {
          why = host.gamescope ? "auto: gamescope" : "auto: Steam Deck";
        }
        else if (!sp120 && reported && !multiple)
        {
          why = fmt::format("auto: the display's {} Hz isn't a multiple of the game's {:.0f} Hz", reported, guest_hz);
        }
      }
      if (!why.empty())
      {
        ChoosePresentMode(why, host, reported, sp120 ? 120.0 : guest_hz);
      }
      else
      {
        REXLOG_INFO("pdz_pace: present pacing host check: Wine {}, gamescope {}, Steam Deck {}, the display reports {} "
                    "Hz ({} of the game's {:.0f} Hz); pdz_present_pacing {}",
                    host.wine.empty() ? "no" : "yes (" + host.wine + ")", host.gamescope ? "yes" : "no",
                    host.deck ? "yes" : "no", reported, multiple ? "a multiple" : "NOT a multiple", guest_hz,
                    wanted == "auto" ? "auto: not present (the auto choice between fixed and vrr follows)" : wanted);
      }
    }
    if (want == 0 && REXCVAR_GET(pdz_sp_120))
    {
      const uint32_t display = PrimaryDisplayHz();
      // fixed paces any display at its own refresh (or an even division of it); vrr / latch / present assume 120 Hz.
      const uint32_t min_display = !g_present_mode && WantedPacing(false) == "fixed" ? 30u : 119u;
      if (!null_backend || !FlagTrue("vsync", true) || display < min_display)
      {
        REXLOG_INFO("pdz_pace: pdz_sp_120: off ({}): 60 fps everywhere",
                    !null_backend ? "graphics_backend null is not in effect"
                                  : !FlagTrue("vsync", true) ? "vsync is off"
                                                              : fmt::format("the display runs at {} Hz", display));
      }
      else
      {
        want = 120;
        auto120 = true;
      }
    }
    if (want != 0)
    {
      if (!null_backend)
      {
        REXLOG_INFO("pdz_pace: pdz_null_refresh_hz {} ignored: graphics_backend null is not in effect (with the GPU "
                    "plugin the guest refresh is video_mode_refresh_rate)",
                    want);
      }
      else if (!FlagTrue("vsync", true))
      {
        REXLOG_WARN("pdz_pace: pdz_null_refresh_hz {} ignored: vsync is off (the vsync worker then delivers a guest "
                    "vblank about every 2 ms, unpaced)",
                    want);
      }
      else
      {
        const uint32_t hz = want < 0 ? PrimaryDisplayHz() : uint32_t(want);
        if (hz < 30)
        {
          REXLOG_WARN("pdz_pace: pdz_null_refresh_hz {} ignored: {}", want,
                      want < 0 ? "the primary display's refresh rate could not be read" : "below 30 Hz");
        }
        else
        {
          g_active = true;
          g_hz = hz;
          g_auto = auto120;
          log = log || !auto120; // pdz_null_refresh_hz set by hand: the clock log, as before
          // pdz_sp_120: VRR / fixed / latch (pdz_present_pacing); pdz_null_refresh_hz by hand keeps the latch. Fixed
          // may pace at the display's own rate (g_hz then follows it: the 60 fps scenes' divider, the budgets).
          const double vblank_hz = auto120 ? ChoosePresentPacing(hz) : double(hz);
          g_hz = std::max(30u, uint32_t(std::lround(vblank_hz)));
          rex::graphics::null_gpu::SetGuestVblankHz(vblank_hz);
          // The presenter's swap chain is created later (the window's surface connects after the GPU setup).
          const int32_t latency = g_present_mode ? 1 : REXCVAR_GET(pdz_null_present_latency);
          rex::graphics::null_gpu::SetPresentMaxFrameLatency(uint32_t(std::clamp(latency, 0, 3)));
          REXLOG_INFO("pdz_pace: {}pdz_null_refresh_hz {}: guest vblanks paced at {:.3f} Hz (null_gpu::SetGuestVblankHz; "
                      "the guest's video mode still reports video_mode_refresh_rate {} Hz; host_vblank_pacing {}: a "
                      "fixed-refresh display gets a guest vblank every round(display Hz / {}) host vblanks, VRR / no "
                      "lock an even 1/{} s timer); presentation interval 1 vblank ({}); cutscene step: {} "
                      "(pdz_cutscene_step {}); present timing as pdz_native_present_timing; present queue: {} "
                      "(pdz_null_present_latency {}, present_max_frame_latency {}); game clock log {}",
                      auto120 ? "pdz_sp_120 (single player levels at the paced rate, up to 120 fps; co-op, multiplayer "
                                "and the front end about 60): "
                              : "",
                      want, vblank_hz, rex::cvar::GetFlagByName("video_mode_refresh_rate"),
                      rex::cvar::GetFlagByName("host_vblank_pacing"), g_hz, g_hz,
                      REXCVAR_GET(pdz_fps60_unlock) ? "pdz_fps60_unlock is on too" : "forced on",
                      StepName(StepMode()), REXCVAR_GET(pdz_cutscene_step),
                      latency > 0 ? fmt::format("a frame latency waitable object of depth {} unless "
                                                "present_max_frame_latency is set (null_gpu::SetPresentMaxFrameLatency)",
                                                std::min(latency, 3))
                                  : std::string("DXGI's own (no waitable object)"),
                      latency, rex::cvar::GetFlagByName("present_max_frame_latency"), log ? "on" : "off");
        }
      }
    }
    g_log = log;
    if (log)
    {
      rex::graphics::null_gpu::EnablePresentStats(true); // the host present part of the pdz_clock lines
      StartThread();
    }
    else if (g_active)
    {
      g_cutscene_log_only = true; // one line per cutscene (log 272 had none)
      StartThread();
    }
  }

  bool Active() { return g_active; }

  uint32_t RefreshHz() { return g_hz; }

  uint32_t EffectiveHz()
  {
    const uint32_t divider = std::max(1u, g_divider.load(std::memory_order_relaxed));
    const uint32_t cap = g_cap_fps.load(std::memory_order_relaxed);
    return divider == 1 && cap ? cap : g_hz / divider;
  }

  Scene TakeScene() { return Scene(g_scene.exchange(uint32_t(Scene::kOther), std::memory_order_relaxed)); }
  Scene PeekScene() { return Scene(g_scene.load(std::memory_order_relaxed)); }

  std::string SpLevelName(const uint8_t *base, std::string *raw)
  {
    const uint32_t state_object = base ? GuestU32(base, kClockState) : 0;
    if (state_object < 0x10000u)
    {
      return {};
    }
    std::string name;
    int id = -1;
    const SpLevel *sp = CurrentSpLevel(base, state_object, name, id);
    if (raw)
    {
      *raw = name;
    }
    return sp ? std::string(sp->name) : std::string();
  }

  bool LastCutsceneStep(uint64_t &calls, uint32_t &picture_ms, uint32_t &clock_ms, double &step_s, double &sound_ms)
  {
    calls = g_cs_calls.load(std::memory_order_acquire);
    if (!calls)
    {
      return false;
    }
    const uint64_t pair = g_cs_last.load(std::memory_order_relaxed);
    picture_ms = uint32_t(pair >> 32);
    clock_ms = uint32_t(pair);
    step_s = double(g_cs_last_step_us.load(std::memory_order_relaxed)) * 1e-6;
    sound_ms = double(clock_ms) - double(g_cs_av_clock_lead_us.load(std::memory_order_relaxed)) * 1e-3;
    return true;
  }

  void LogSummary(const char *why)
  {
    g_stop.store(true, std::memory_order_release);
    if (!g_log || !g_thread_started.load(std::memory_order_acquire))
    {
      return;
    }
    std::lock_guard<std::mutex> lock(g.mutex);
    if (g.summary_done)
    {
      return;
    }
    g.summary_done = true;
    const Snap s = TakeSnap();
    TrackPresentMax(s);
    TickCutscene(s.qpc);
    CloseSegment("test exit");
    if (s.ok)
    {
      g.last = s;
    }
    const Snap &a = g.first, &b = g.last;
    std::string clock = "no game clock sample (the game never counted a frame)";
    if (a.ok && b.ok && b.qpc > a.qpc)
    {
      const double wall_s = Seconds(a.qpc, b.qpc);
      const uint32_t frames = b.frames - a.frames;
      const uint32_t vblanks = a.vblank_ok && b.vblank_ok ? b.vblanks - a.vblanks : 0;
      clock = fmt::format("game clock / wall clock (QPC) ratio {:.4f} over {:.1f} s (raw dt sum; clamped {:.4f}) | "
                          "frames {} ({:.1f}/s), guest vblanks {} ({:.1f}/s, {:.2f} per frame){}",
                          (b.raw_sum - a.raw_sum) / wall_s, wall_s,
                          (double(b.clamped_sum) - double(a.clamped_sum)) / wall_s, frames, double(frames) / wall_s,
                          vblanks, double(vblanks) / wall_s, frames ? double(vblanks) / double(frames) : 0.0,
                          FrontEndPart(a, b, wall_s));
    }
    const std::string level =
        g.level_windows ? fmt::format("in a level: ratio {:.4f} over {:.1f} s in {} windows of 10 s (min {:.4f}, max "
                                      "{:.4f})",
                                      g.level_game_s / g.level_wall_s, g.level_wall_s, g.level_windows, g.level_min,
                                      g.level_max)
                        : std::string("in a level: no full 10 s window");
    const CsCounts c = ReadCsCounts();
    const std::string cutscenes =
        g.segments_counted
            ? fmt::format("cutscenes: {} segments, {} of 2 s or more: picture / wall clock ratio {:.4f} (the picture "
                          "{:.3f} s in {:.3f} s), their own clock {:.4f}; picture / sound: largest offset {:.1f} ms, "
                          "audio output underruns {:.1f} ms in all",
                          g.segments, g.segments_counted, g.cs_picture_s / g.cs_wall_s, g.cs_picture_s, g.cs_wall_s,
                          g.cs_clock_s / g.cs_wall_s, g.av_offset_max_ms, g.av_underrun_ms)
            : fmt::format("cutscenes: {} segments, none of 2 s or more (the scripts skip them with A)", g.segments);
    // In-level host presents (the max values are the run's; timeouts the run's).
    const std::string presents =
        g.level_windows ? "in a level, " + PresentPart(g.level_presents, g.level_present_us, g.all_present_max_us,
                                                       g.level_over_2ms, g.level_repeats, g.level_ui, g.level_waits,
                                                       g.level_wait_us, g.all_wait_max_us, s.ps.latency_timeouts,
                                                       s.ps.frame_latency, g.level_wall_s)
                        : std::string("in a level: no host present totals");
    REXLOG_INFO("pdz_clock: SUMMARY ({}): {} | {} | {} | {} | cutscene frames {}: behind {}, not behind {} (real-time "
                "step {}); pdz_cutscene_step: {} (expected: game clock ratio 1.0025 at any rate, the game's 49.88 MHz "
                "conversion of the 50 MHz timebase; cutscene picture ratio 1.00) | {}",
                why, Pacing(), clock, level, cutscenes, c.calls, c.behind, c.ahead, c.real_time, StepName(StepMode()),
                presents);
  }
} // namespace pdz::pace
