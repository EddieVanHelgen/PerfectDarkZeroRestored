// Native renderer (stage 1): the Swap path, the F9 output selector and the gamma ramp (native.h OnSwap /
// RegisterBinds / NativeOutputSelected). Also hooks D3DDevice_SetGammaRamp (sub_826F14A8, not hooked by the
// shadow files).
//
// Present strategy (CORE_NOTES.md "Present"): the shadow hook forwards Swap to the guest D3D (the Xenia plugin
// keeps presenting), then calls OnSwap. With the native output selected the frame is handed to the SDK
// presenter from here (pdz_native_present_mode 0) or at the first native call of the next frame (mode 1).
// The plugin refreshes the same presenter from its GPU thread a few milliseconds later; whichever refresh is
// the latest when the window paints wins, so the picture can alternate between native and emulated. The
// plugin has no switch to skip its present; the clean fix is one cvar in the SDK's IssueSwap (stage 2).
//
// One producer at a time (stage1\subagents\outrace.md): the presenter's guest output mailbox is single-producer.
// With the native output shown the plugin still refreshes it on a same-frame capture swap (the auto-diff's
// RequestGuestOutputCapture: SDK patch sdk-04 lets the GPU thread bypass the owner guard for that swap). Its
// refresh ran at the same moment as ours on the game thread (both refresh the same guest swap a few ms apart), on
// the same mailbox image, and recreated the image texture at its own 2560x1440 size while we recorded the gamma
// pass into the 1280x720 one: a device loss (run 196, minidump: the GPU thread inside CaptureGuestOutput, the game
// thread in the D3D fence block) or a hang (run 197), at the Datacore level start because the first auto-diff
// tick fell there. pdz_native_output_capture: no plugin captures while the native output is shown (default), and
// the native refresh never runs while a capture is pending. The owner hand-overs (a shader-miss frame shown from
// the plugin, F9) can still overlap a plugin refresh already under way; SDK patch sdk-11 (one refresh at a time
// inside the presenter) closes those.
//
// The output chain (PresentFrame): the gamma pass, then the plugin's swap post effect (FXAA / SMAA, swap_post_effect)
// with the SDK's own shaders while pdz_native_post_effect is on (native_postfx.cpp), as the plugin's IssueSwap does
// for the emulated output; the presenter's paint effects (present_effect: FSR / CAS) follow for both outputs alike.
// Step by step against IssueSwap (stage1\subagents\present.md): the front buffer read as plain UNORM data (signs
// ignored, pdz_native_present_raw_front), the DC_LUT table (PDZ's front buffer is k_8_8_8_8, so the plugin's PWL ramp
// is never used; logged if it ever is), the swap post effect, R10G10B10A2 output, SetIs8bpc as the plugin. What the
// paint does differs only by the guest output's size: FSR upscales a 1280x720 output (pdz_native_scale 1) in gamma
// space, CAS downscales the emulator's 2560x1440 (resolution_scale 2) in a squared space (about +0.5-1 % mean).
//
// graphics_backend null (stage 2 M1, gpu_sync.h): the GPU plugin renders nothing, so the native output is forced on, F9
// and the per-frame shader-miss fallback are off (a frame with a missing shader is shown as drawn and counted), and the
// auto-diff, pass dump and draw dump refuse to start (one line each, NullBackendRefusals). The present itself is
// unchanged by default: from the Swap hook on the game thread; the plugin's null command processor hands nothing to the
// presenter.
//
// Present timing (stage 2 M3, graphics_backend null only, pdz_native_present_timing; STAGE2_PLAN.md open question 4):
// 0 (default) = from the Swap hook, as above; frame_pacing then pairs a presented frame with the newest VdSwap (its
// default pairing, the command processor's last XE_SWAP, is one frame off whenever the Swap-hook refresh beats the
// XE_SWAP). 1 = at XE_SWAP: PresentFrame stages the frame (gamma pass and swap post effect into one of four staging
// textures, R10G10B10A2 like the guest output, in the native frame's command list), OnSwap publishes it after the
// frame's submit, and it is handed to the presenter (a copy into the guest output, on our own command list on the same
// queue) when the null command processor reaches that swap: the SDK's XE_SWAP listener (rex/graphics/null_gpu.h) on
// the command processor thread, where the D3D12 backend's IssueSwap refreshes the presenter, after the guest's
// presentation-interval gate; or at the publish on the game thread when the command processor got there first. The
// game thread owns the presenter's output (SetGuestOutputExternalOwner); the command processor thread passes the owner
// check with the presenter's thread-local bypass, and one mutex (never held while waiting on a guest thread: the
// guest may be waiting for the ring that thread consumes; never the render mutex on that thread) keeps the two
// producers apart. Swap numbers: the native OnSwap count against the command processor's IssueSwap count (both from
// 1, as the auto-diff's captures assume); their lead at each XE_SWAP is counted for the summary.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>
#include <functional>
#include <mutex>
#include <string_view>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/graphics/gamma_mirror.h>
#include <rex/graphics/null_gpu.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>
#include <rex/runtime.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/ui/d3d12/d3d12_presenter.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/presenter.h>
#include <rex/ui/flags.h>
#include <rex/ui/frame_pacing.h>

#include "../game_pace.h"
#include "native.h"
#include "../shadow.h"
#include "native_device.h"
#include "native_passes.h"
#include "native_postfx.h"
#include "native_textures.h"
#include "native_autodiff.h"
#include <spdlog/sinks/basic_file_sink.h>
#include "native_draw.h"
#include "native_drawdump.h"
#include "native_memlog.h"
#include "native_recthread.h"
#include "native_view.h"
#include "native_watchdog.h"
#include "../test_harness.h"
#include "../frame_timing.h"
#include "../cutscene_dump.h" // pdz_cutscene_frame_dump: the present tap

// Trace on mark (0.9.2, transparency vs water captures by hand): an F12 mark also captures everything about its
// frame.
REXCVAR_DEFINE_INT32(pdz_test_mark, 0, "Testing",
                     "TESTING TOOL: each change to a new non-zero value takes one F12 mark (the game frame's PNG and "
                     "the pdz_mark line with the position), for input scripts: 'cvar <t> pdz_test_mark 1', then 2...")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(pdz_trace_on_mark, false, "Testing",
                    "Test aid (off): an F12 mark (bind_mark_spot) also traces its frame: pdz_native_frame_trace (every "
                    "clear / resolve / draw), pdz_native_draw_dump of every draw (Rare's caller, shaders, states), and "
                    "pdz_transparency_trace_frame (Rare's mesh / glow calls with guest call chains); then the same for a "
                    "second mark with pdz_fix_transparency_water flipped (F1), restored after. All lines of the capture "
                    "also go to logs\\marks\\trace_NNN\\trace.log; a label says when it is saved. Frame A also reads "
                    "back colour target 0 after each of the 64 draws from Rare's see-through depth rebuild (PS "
                    "675a96f9) on (trace_NNN\\draws, about 2.4 GB: which see-through draw paints what). The draw dump needs the "
                    "GPU plugin and no record thread (C:\\Games\\PDZ_PLAY_TRACE.bat).");
REXCVAR_DECLARE(std::string, pdz_native_draw_dump);
REXCVAR_DECLARE(int32_t, pdz_native_draw_dump_frames);
REXCVAR_DECLARE(int32_t, pdz_native_draw_dump_from);
REXCVAR_DECLARE(int32_t, pdz_native_draw_dump_every);
REXCVAR_DECLARE(int32_t, pdz_native_draw_dump_lines);
REXCVAR_DECLARE(int32_t, pdz_native_draw_dump_rt);
REXCVAR_DECLARE(int32_t, pdz_native_draw_dump_pick);
REXCVAR_DECLARE(std::string, pdz_native_draw_dump_dir);
REXCVAR_DECLARE(bool, pdz_fix_transparency_water);
namespace pdz::transwater
{
  void TraceFrame(uint32_t frame, uint32_t count); // transparency_water.cpp: trace these game frames (one-shot)
  uint32_t CurrentFrame(const uint8_t *base);
  void ShowStatus(const std::string &text, int seconds); // its on-screen label
} // namespace pdz::transwater
REXCVAR_DEFINE_BOOL(pdz_native_gamma_from_gpu, true, "Render",
                    "Native renderer: apply the gamma table the GPU plugin applies (DC_LUT registers, published by the "
                    "SDK) instead of the ramp the game passed to SetGammaRamp.");
REXCVAR_DEFINE_INT32(pdz_native_output_capture, 0, "Render",
                     "Native renderer, auto-diff while the native output is shown: 0 = no same-frame plugin captures "
                     "(the plugin's capture refresh raced ours on the presenter: device lost / hang, runs 196-197); "
                     "1 = captures allowed, the native refresh waits while one is pending (that swap shows the "
                     "emulated frame); 2 = captures with both refreshing at once (the old behaviour; only to "
                     "reproduce the race or test an SDK fix).")
    .range(0, 2);

REXCVAR_DEFINE_BOOL(pdz_native_present_raw_front, true, "Render",
                    "Native renderer: read the swap's front buffer as plain UNORM data whatever its fetch constant's "
                    "signs say, as the GPU plugin does (RequestSwapTexture: GetDXGIUnormFormat, the signs ignored). "
                    "Only matters for a front buffer served by an upload with gamma signs (an _SRGB view would "
                    "linearise it: a darker picture); PDZ's front buffer (1280x720 k_8_8_8_8) is a resolve mirror, "
                    "which is read as plain data either way. false = the fetch constant's signs.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_INT32(pdz_native_present_timing, 0, "Render",
                     "Native output with graphics_backend null: when a native frame is handed to the presenter. "
                     "0 = from the Swap hook on the game thread, right after the guest Swap (lowest latency, no pacing "
                     "gate; the stage-2 M1 behaviour). 1 = at XE_SWAP: the frame is staged at the Swap hook (gamma pass "
                     "and swap post effect into a staging texture) and handed to the presenter when the GPU plugin's "
                     "null command processor reaches that swap (its thread, after the presentation-interval gate: "
                     "where the D3D12 backend refreshes the presenter, so host vblank pacing and the vblank latch see "
                     "frames at the plugin's timing), or at the staging when the command processor got there first. "
                     "Ignored while the GPU plugin renders (logged). Compare with pdz_frame_timing (present delay / gap "
                     "columns, pdz_present SUMMARY) and frame_pacing_log ('L' / 'D' rows, frame_pacing: lines).")
    .range(0, 1)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REX_EXTERN(__imp__sub_826F14A8); // D3DDevice_SetGammaRamp
REXCVAR_DECLARE(int32_t, pdz_native_log_frames); // native_shaders.cpp
// The tools that need the GPU plugin, refused with the null GPU backend (NullBackendRefusals).
REXCVAR_DECLARE(bool, pdz_native_autodiff);         // native_autodiff.cpp
REXCVAR_DECLARE(int32_t, pdz_native_pass_dump);     // native_passdump.cpp
REXCVAR_DECLARE(std::string, pdz_native_draw_dump); // native_drawdump.cpp

namespace pdz::native
{
  namespace
  {
    struct PresentState
    {
      bool output_initialised = false;
      bool native_output = false;
      bool bind_registered = false;
      bool f9_was_down = false;
      // Gamma ramp: 256 x (r, g, b) 16-bit entries as the game passes them (D3DGAMMARAMP: red[256],
      // green[256], blue[256]); identity until SetGammaRamp is called.
      uint16_t ramp[3][256];
      bool ramp_set = false;
      // The table the GPU plugin applies (DC_LUT registers, published by the SDK at IssueSwap): D3D transforms the
      // game's ramp before writing it (log 045: the game's ramp took 16 -> 33 where the plugin's table gives
      // 16 -> 18), so this one is used when available (pdz_native_gamma_from_gpu).
      uint16_t gpu_ramp[3][256];
      uint64_t gpu_ramp_generation = 0;
      bool lut_dirty = true;
      ID3D12Resource *lut = nullptr;
      D3D12_RESOURCE_STATES lut_state = D3D12_RESOURCE_STATE_COPY_DEST;
      D3D12_CPU_DESCRIPTOR_HANDLE lut_srv{};
      // Mode 1: the frame to present at the next frame's first native call.
      bool pending = false;
      FetchDecoded pending_front;
      uint32_t frames_presented = 0;
      uint32_t frames_fallen_back = 0;
      // Every native frame, output selected or not: complete (no draw skipped for a shader) vs with a miss.
      uint64_t frames_complete = 0, frames_missed = 0;
      uint32_t window_complete = 0, window_missed = 0; // since the last periodic line
      bool last_frame_missed = false;
      bool fallback_logged = false;
      bool presenter_missing_logged = false;
      // pdz_native_output_capture: auto-diff ticks skipped while the native output was shown (0), native refreshes
      // held back while a plugin capture was pending (0 / 1; hold_swaps = consecutive), the request a concurrent
      // refresh was last logged for (2).
      uint32_t captures_skipped = 0;
      uint32_t frames_held = 0;
      uint32_t hold_swaps = 0;
      uint32_t holds_cancelled = 0;
      bool capture_skip_logged = false;
      bool hold_logged = false;
      uint64_t overlap_logged_for = 0;
      // The first presented front buffer's description (format, signs, mirror or upload, host format), logged once.
      bool front_logged = false;
      // graphics_backend null (gpu_sync.h): frames shown although a draw was skipped for a missing shader (there is no
      // emulated frame to fall back to), and the one-time lines.
      uint64_t frames_shown_with_miss = 0;
      bool null_miss_logged = false;
      bool null_refusals_logged = false;
      // Present timing (pdz_native_present_timing): the native OnSwap count (= the GPU plugin's IssueSwap count) and
      // the entry time of the current one; the slot PresentFrame staged this frame into (-1 = none; XE_SWAP mode).
      uint64_t swaps = 0;
      int64_t swap_us = 0;
      int32_t staged_slot = -1;
      bool present_mode_logged = false;
      // Swap-hook presents (mode 0): the last refresh, the delay sums for the present SUMMARY.
      int64_t last_present_us = 0;
      uint64_t hook_presents = 0;
      int64_t hook_delay_sum_us = 0, hook_delay_max_us = 0;
    };
    PresentState g_present;
    // A pending capture that the plugin has not served after this many swaps is dropped (its IssueSwap count is past
    // the requested swap, or a toggle left it behind) so the native output does not stay frozen.
    constexpr uint32_t kCaptureHoldSwaps = 30;

    // pdz_native_present_timing 1 with the null GPU backend: native frames handed to the presenter at XE_SWAP (top of
    // this file). Shared by the game thread (stage, publish) and the GPU command processor thread (the listener).
    namespace xeswap
    {
      constexpr uint32_t kSlots = 4; // frames the game thread may be ahead of the command processor, plus one
      enum class Stage : uint8_t
      {
        kFree,
        kStaging, // the game thread is recording into it (the listener never touches it)
        kReady,   // submitted, waiting for its XE_SWAP
      };
      struct Slot
      {
        ID3D12Resource *texture = nullptr; // kGuestOutputFormat, UAV-capable; rests in COPY_SOURCE
        D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COPY_SOURCE; // game thread only
        uint32_t width = 0, height = 0;                                 // host size (the guest output's)
        uint32_t display_width = 0, display_height = 0;                 // the guest front buffer (aspect)
        bool is_8bpc = true;
        Stage stage = Stage::kFree;
        uint64_t swap = 0;    // the native OnSwap number that staged it
        int64_t swap_us = 0;  // that OnSwap's entry
        int64_t ready_us = 0; // published (after the native frame's submit)
      };
      struct State
      {
        std::mutex mutex; // everything below except `active` and the slots' resource state
        // Set at startup (ChoosePresentTimingAtStartup), cleared if the copier cannot be made (before any frame is
        // ready, so no present from the command processor thread can follow).
        std::atomic<bool> active{false};
        Slot slots[kSlots];
        // The copy into the presenter's guest output: our own allocators and list on the native device's queue (the
        // provider's direct queue, which the presenter signals after the refresher).
        ID3D12CommandQueue *queue = nullptr;
        ID3D12CommandAllocator *allocators[kSlots] = {};
        uint64_t allocator_fence[kSlots] = {};
        ID3D12GraphicsCommandList *list = nullptr;
        ID3D12Fence *fence = nullptr;
        uint64_t fence_value = 0;
        HANDLE event = nullptr;
        uint32_t next_allocator = 0;
        // The command processor side.
        uint64_t cp_swap = 0; // the last XE_SWAP the listener saw
        // Counters (present SUMMARY).
        uint64_t notifications = 0, at_cp = 0, at_stage = 0, cp_without_frame = 0, dropped = 0, refresh_failed = 0;
        // Native OnSwap count - XE_SWAP index at a notification: < -1 (counts out of step), -1 (the XE_SWAP came
        // during its own guest Swap, before that Swap's native OnSwap), 0, 1, 2, >= 3 (native frames ahead).
        uint64_t lead[6] = {};
        int64_t last_present_us = 0;
        int64_t delay_sum_us = 0, delay_max_us = 0, wait_sum_us = 0;
        uint64_t presents = 0;
        bool logged_first_cp = false, logged_first_present = false, logged_drop = false, logged_lead = false;
        bool logged_failure = false;
      };
      State g;
      std::atomic<uint64_t> g_game_swaps{0}; // the native OnSwap count, read by the listener

      bool Active() { return g.active.load(std::memory_order_relaxed); }

      // The copier, created by the game thread at the first stage (mutex held). False (logged) when it cannot be.
      bool EnsureCopierLocked(NativeDevice &dev)
      {
        State &s = g;
        if (s.list)
        {
          return true;
        }
        ID3D12Device *device = dev.device();
        bool ok = device && dev.queue();
        for (uint32_t i = 0; ok && i < kSlots; ++i)
        {
          ok = SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&s.allocators[i])));
        }
        ok = ok && SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s.allocators[0], nullptr,
                                                       IID_PPV_ARGS(&s.list)));
        ok = ok && SUCCEEDED(s.list->Close());
        ok = ok && SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&s.fence)));
        if (ok)
        {
          s.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
          ok = s.event != nullptr;
        }
        if (!ok)
        {
          REXLOG_ERROR("pdz_native: null GPU backend: present timing XE_SWAP: the copy command list could not be made; "
                       "presenting from the Swap hook instead");
          if (s.list)
          {
            s.list->Release();
            s.list = nullptr;
          }
          return false;
        }
        s.queue = dev.queue();
        return true;
      }

      // Copies the slot into the presenter's guest output (both the same size and format) and submits (mutex held).
      bool RecordCopyLocked(Slot &slot, ID3D12Resource *output)
      {
        State &s = g;
        const uint32_t i = s.next_allocator;
        if (s.fence->GetCompletedValue() < s.allocator_fence[i])
        {
          // Four copies still queued on the GPU: rare (the GPU is frames behind); bounded.
          s.fence->SetEventOnCompletion(s.allocator_fence[i], s.event);
          WaitForSingleObject(s.event, 2000);
        }
        if (FAILED(s.allocators[i]->Reset()) || FAILED(s.list->Reset(s.allocators[i], nullptr)))
        {
          return false;
        }
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = output;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = rex::ui::d3d12::D3D12Presenter::kGuestOutputInternalState;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        s.list->ResourceBarrier(1, &barrier);
        s.list->CopyResource(output, slot.texture);
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        s.list->ResourceBarrier(1, &barrier);
        if (FAILED(s.list->Close()))
        {
          return false;
        }
        ID3D12CommandList *lists[] = {s.list};
        s.queue->ExecuteCommandLists(1, lists);
        s.queue->Signal(s.fence, ++s.fence_value);
        s.allocator_fence[i] = s.fence_value;
        s.next_allocator = (i + 1) % kSlots;
        return true;
      }

      // Hands a ready slot to the presenter (mutex held; the game thread or the command processor thread).
      void DeliverLocked(Slot &slot, bool at_cp)
      {
        State &s = g;
        rex::Runtime *runtime = rex::Runtime::instance();
        rex::system::IGraphicsSystem *graphics = runtime ? runtime->graphics_system() : nullptr;
        rex::ui::Presenter *presenter = graphics ? graphics->presenter() : nullptr;
        slot.stage = Stage::kFree;
        if (!presenter)
        {
          ++s.refresh_failed;
          return;
        }
        // The game thread owns the output (SetGuestOutputExternalOwner); this thread may be the command processor's.
        rex::ui::Presenter::SetGuestOutputCaptureBypass(true);
        const bool ok = presenter->RefreshGuestOutput(
            slot.width, slot.height, slot.display_width, slot.display_height,
            [&](rex::ui::Presenter::GuestOutputRefreshContext &context) -> bool {
              auto &d3d12_context =
                  static_cast<rex::ui::d3d12::D3D12Presenter::D3D12GuestOutputRefreshContext &>(context);
              if (!RecordCopyLocked(slot, d3d12_context.resource_uav_capable()))
              {
                return false;
              }
              context.SetIs8bpc(slot.is_8bpc);
              return true;
            });
        rex::ui::Presenter::SetGuestOutputCaptureBypass(false);
        const int64_t now = pdz::gpusync::NowUs();
        if (!ok)
        {
          ++s.refresh_failed;
          if (!s.logged_failure)
          {
            s.logged_failure = true;
            REXLOG_WARN("pdz_native: null GPU backend: present timing XE_SWAP: the presenter refused the refresh of "
                        "swap {} ({}; counted)",
                        slot.swap, at_cp ? "command processor thread" : "game thread");
          }
          return;
        }
        ++(at_cp ? s.at_cp : s.at_stage);
        const int64_t delay = std::max<int64_t>(now - slot.swap_us, 0);
        const int64_t gap = s.last_present_us ? std::max<int64_t>(now - s.last_present_us, 0) : 0;
        s.last_present_us = now;
        ++s.presents;
        s.delay_sum_us += delay;
        s.delay_max_us = std::max(s.delay_max_us, delay);
        s.wait_sum_us += std::max<int64_t>(now - slot.ready_us, 0);
        pdz::timing::AddPresent(uint64_t(delay), uint64_t(gap), "at XE_SWAP");
        if (!s.logged_first_present)
        {
          s.logged_first_present = true;
          REXLOG_INFO("pdz_native: null GPU backend: first native frame presented at XE_SWAP: swap {} {}x{} on the {} "
                      "thread {}, {:.2f} ms after its native OnSwap, {:.2f} ms after its submit",
                      slot.swap, slot.width, slot.height, at_cp ? "command processor" : "game (the XE_SWAP came first)",
                      GetCurrentThreadId(), double(delay) / 1000.0, double(now - slot.ready_us) / 1000.0);
        }
      }

      // The null command processor's XE_SWAP (its thread): present the newest ready frame up to this swap.
      void OnXeSwap(uint64_t swap_index, uint32_t frontbuffer_ptr, uint32_t width, uint32_t height)
      {
        State &s = g;
        if (!s.active)
        {
          return;
        }
        std::lock_guard<std::mutex> lock(s.mutex);
        s.cp_swap = swap_index;
        ++s.notifications;
        const int64_t lead = int64_t(g_game_swaps.load(std::memory_order_acquire)) - int64_t(swap_index);
        ++s.lead[std::clamp<int64_t>(lead + 2, 0, 5)];
        if (!s.logged_first_cp)
        {
          s.logged_first_cp = true;
          REXLOG_INFO("pdz_native: null GPU backend: first XE_SWAP listener call: command processor swap {} (front "
                      "buffer {:08X} {}x{}), native swaps so far {}, thread {}",
                      swap_index, frontbuffer_ptr, width, height, lead + int64_t(swap_index), GetCurrentThreadId());
        }
        if (lead < -1 && !s.logged_lead)
        {
          s.logged_lead = true;
          REXLOG_WARN("pdz_native: null GPU backend: present timing XE_SWAP: the command processor's swap {} is more "
                      "than one ahead of the native Swap count {} (a swap the Swap hook did not see?): frames are then "
                      "presented at their staging (counted in the present SUMMARY)",
                      swap_index, lead + int64_t(swap_index));
        }
        Slot *best = nullptr;
        for (Slot &slot : s.slots)
        {
          if (slot.stage == Stage::kReady && slot.swap <= swap_index && (!best || slot.swap > best->swap))
          {
            best = &slot;
          }
        }
        if (!best)
        {
          ++s.cp_without_frame; // still being staged (presented at its publish) or nothing staged (boot)
          return;
        }
        for (Slot &slot : s.slots)
        {
          if (slot.stage == Stage::kReady && slot.swap < best->swap)
          {
            slot.stage = Stage::kFree; // superseded before its XE_SWAP came (not expected: counted)
            ++s.dropped;
          }
        }
        DeliverLocked(*best, true);
      }

      // PresentFrame (game thread, render mutex held, the frame open): records the output chain into a staging slot.
      // False only when the copier cannot be had (XE_SWAP mode is then off for the session and the caller presents
      // from the Swap hook); a staging texture that cannot be made skips this frame's present (true), because a
      // Swap-hook refresh could run beside a command processor one.
      bool StageFrame(NativeDevice &dev, const FetchDecoded &front, const TextureView &view, uint32_t width,
                      uint32_t height, D3D12_CPU_DESCRIPTOR_HANDLE lut_srv)
      {
        State &s = g;
        PresentState &p = g_present;
        const uint32_t index = uint32_t(p.swaps % kSlots);
        Slot &slot = s.slots[index];
        {
          std::lock_guard<std::mutex> lock(s.mutex);
          if (!EnsureCopierLocked(dev))
          {
            s.active = false;
            return false;
          }
          if (slot.stage == Stage::kReady)
          {
            ++s.dropped; // its XE_SWAP never came while four newer frames were staged
            if (!s.logged_drop)
            {
              s.logged_drop = true;
              REXLOG_WARN("pdz_native: null GPU backend: present timing XE_SWAP: staged swap {} was never presented "
                          "(the command processor is at swap {}); dropped (counted in the present SUMMARY)",
                          slot.swap, s.cp_swap);
            }
          }
          slot.stage = Stage::kStaging;
          slot.swap = p.swaps;
          slot.swap_us = p.swap_us;
          slot.display_width = front.width;
          slot.display_height = front.height;
        }
        if (!slot.texture || slot.width != width || slot.height != height)
        {
          if (slot.texture)
          {
            dev.ReleaseLater(slot.texture); // a copy from it may still be queued: released after this frame's fence
            slot.texture = nullptr;
          }
          D3D12_RESOURCE_DESC desc{};
          desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
          desc.Width = width;
          desc.Height = height;
          desc.DepthOrArraySize = 1;
          desc.MipLevels = 1;
          desc.Format = rex::ui::d3d12::D3D12Presenter::kGuestOutputFormat;
          desc.SampleDesc.Count = 1;
          desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
          slot.texture = dev.CreateTexture(desc, D3D12_RESOURCE_STATE_COPY_SOURCE, nullptr,
                                           "pdz_native XE_SWAP staging output");
          slot.state = D3D12_RESOURCE_STATE_COPY_SOURCE;
          slot.width = slot.texture ? width : 0;
          slot.height = slot.texture ? height : 0;
        }
        if (!slot.texture)
        {
          std::lock_guard<std::mutex> lock(s.mutex);
          slot.stage = Stage::kFree;
          return true; // not presented (CreateTexture logged why)
        }
        dev.Transition(slot.texture, slot.state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        postfx::ChainDesc chain;
        chain.output = slot.texture;
        chain.width = width;
        chain.height = height;
        chain.src_srv = view.srv;
        chain.lut_srv = lut_srv;
        chain.effect = postfx::PresentEffect();
        postfx::Effect applied = postfx::Effect::kNone;
        const bool ok = postfx::RecordChain(dev, chain, applied);
        dev.Transition(slot.texture, slot.state, D3D12_RESOURCE_STATE_COPY_SOURCE);
        slot.is_8bpc = applied == postfx::Effect::kNone; // as the plugin (the presenter's dither rule)
        if (!ok)
        {
          std::lock_guard<std::mutex> lock(s.mutex);
          slot.stage = Stage::kFree;
          return true; // nothing usable written: this frame is not presented (as a failed refresh)
        }
        p.staged_slot = int32_t(index);
        return true;
      }

      // OnSwap after the native frame's submit (game thread): the staged frame is ready; presented now if its XE_SWAP
      // has already passed.
      void Publish(int32_t index)
      {
        State &s = g;
        std::lock_guard<std::mutex> lock(s.mutex);
        Slot &slot = s.slots[index];
        if (slot.stage != Stage::kStaging)
        {
          return;
        }
        slot.stage = Stage::kReady;
        slot.ready_us = pdz::gpusync::NowUs();
        if (s.cp_swap >= slot.swap)
        {
          DeliverLocked(slot, false);
        }
      }

      std::string Summary()
      {
        State &s = g;
        std::lock_guard<std::mutex> lock(s.mutex);
        const double n = double(std::max<uint64_t>(s.presents, 1));
        return fmt::format("timing XE_SWAP | presented {} (on the command processor thread {}, at the staging because "
                           "the XE_SWAP came first {}) | dropped {} | refresh failed {} | XE_SWAP calls {}, with no "
                           "staged frame {} | delay from the native OnSwap ms avg {:.2f} max {:.2f}, from the submit avg "
                           "{:.2f} | native swaps ahead of the XE_SWAP: out of step {}, -1 {}, 0 {}, 1 {}, 2 {}, 3+ {}",
                           s.presents, s.at_cp, s.at_stage, s.dropped, s.refresh_failed, s.notifications,
                           s.cp_without_frame, double(s.delay_sum_us) / 1000.0 / n, double(s.delay_max_us) / 1000.0,
                           double(s.wait_sum_us) / 1000.0 / n, s.lead[0], s.lead[1], s.lead[2], s.lead[3], s.lead[4],
                           s.lead[5]);
      }
    } // namespace xeswap

    void InitOutputFlag()
    {
      if (!g_present.output_initialised)
      {
        g_present.output_initialised = true;
        // graphics_backend null: no emulated frame exists, the native output is the only one (forced on).
        g_present.native_output = REXCVAR_GET(pdz_native_output) || pdz::gpusync::NullBackend();
        if (Enabled() && !REXCVAR_GET(pdz_render_shadow))
        {
          REXLOG_INFO("pdz_native: the native renderer turns the shadow recorder on (pdz_render_shadow is false; "
                      "the native renderer draws from the recorded draws)");
        }
        rex::ui::Presenter::SetGuestOutputExternalOwner(g_present.native_output); // the game thread owns the output
        for (uint32_t i = 0; i < 256; ++i)
        {
          const uint16_t v = uint16_t(i * 257);
          g_present.ramp[0][i] = g_present.ramp[1][i] = g_present.ramp[2][i] = v;
        }
      }
    }

    // The swap's front buffer as the GPU plugin reads it (pdz_native_present_raw_front): RequestSwapTexture views the
    // texture with GetDXGIUnormFormat, so the fetch constant's signs (gamma -> an _SRGB view in Textures::Lookup's
    // upload path) are dropped: the sign fields of dword 0 (bits 2..9) are cleared and the constant decoded again, so
    // the fast path and the upload key see the same dwords.
    FetchDecoded SwapFront(const uint8_t *base, uint32_t front_buffer)
    {
      FetchDecoded front = DecodeTextureObject(base, front_buffer);
      if (front.signs && REXCVAR_GET(pdz_native_present_raw_front))
      {
        uint32_t dwords[6];
        std::memcpy(dwords, front.dwords, sizeof(dwords));
        dwords[0] &= ~(0xFFu << 2);
        front = DecodeFetchDwords(dwords);
      }
      return front;
    }

    // Once: what the presented front buffer is. The plugin applies its PWL gamma ramp (not mirrored: REVIEW.md
    // finding 14) instead of the 256-entry table for a 2_10_10_10 front buffer, so that case is named.
    void LogFront(const FetchDecoded &front, const HostTexture &tex)
    {
      PresentState &p = g_present;
      if (p.front_logged)
      {
        return;
      }
      p.front_logged = true;
      REXLOG_INFO("pdz_native: presented front buffer {:08X}: format {} {}x{} signs {:02X} swizzle {:04o} exp_adjust {}, "
                  "{} host format {} {}x{}; gamma: {}",
                  front.base_address, front.format, front.width, front.height, front.signs, front.swizzle,
                  front.exp_adjust, tex.mirror ? "resolve mirror" : "upload", uint32_t(tex.resource_format), tex.width,
                  tex.height, p.gpu_ramp_generation ? "the plugin's DC_LUT table" : "the game's SetGammaRamp ramp");
      if (front.format == 7 || front.format == 54) // k_2_10_10_10, k_2_10_10_10_AS_16_16_16_16
      {
        REXLOG_WARN("pdz_native: the front buffer is 2_10_10_10: the GPU plugin applies its PWL gamma ramp to it, the "
                    "native output the 256-entry table (the PWL ramp is not published to the title)");
      }
    }

    // graphics_backend null (gpu_sync.h), once at the first native swap: the output policy, and one line for each tool
    // that needs the GPU plugin and was asked for (each refuses in its own file: autodiff::Enabled,
    // passdump::MaybeBegin, drawdump::NewFrame).
    // pdz_native_record_thread: the pass dump and the draw dump read the live device and guest memory around a draw and
    // flush the list mid-frame; they stay with the synchronous path (passdump::MaybeBegin, drawdump::NewFrame).
    void RecordThreadRefusals()
    {
      static bool logged = false;
      if (logged || !recthread::Active())
      {
        return;
      }
      logged = true;
      if (REXCVAR_GET(pdz_native_pass_dump) > 0)
      {
        REXLOG_WARN("pdz_native: pdz_native_record_thread: pdz_native_pass_dump refused (run it without the record "
                    "thread)");
      }
      if (!REXCVAR_GET(pdz_native_draw_dump).empty())
      {
        REXLOG_WARN("pdz_native: pdz_native_record_thread: pdz_native_draw_dump refused (run it without the record "
                    "thread)");
      }
    }

    void NullBackendRefusals()
    {
      PresentState &p = g_present;
      if (p.null_refusals_logged || !pdz::gpusync::NullBackend())
      {
        return;
      }
      p.null_refusals_logged = true;
      REXLOG_INFO("pdz_native: null GPU backend: the native output is the only output ({}); F9 and the per-frame "
                  "shader-miss fallback are off: a frame with a draw skipped for a missing shader is shown as drawn "
                  "(that draw is absent for the frame), counted in the SUMMARY",
                  p.native_output ? "on" : "OFF?");
      if (REXCVAR_GET(pdz_native_autodiff))
      {
        REXLOG_WARN("pdz_native: null GPU backend: pdz_native_autodiff refused (there is no emulated frame to compare "
                    "with; pdz_native_capture_s / pdz_native_capture_swaps still grab the presenter's native output)");
      }
      if (REXCVAR_GET(pdz_native_pass_dump) > 0)
      {
        REXLOG_WARN("pdz_native: null GPU backend: pdz_native_pass_dump refused (its emulated side is the GPU plugin's "
                    "resolve readback)");
      }
      if (!REXCVAR_GET(pdz_native_draw_dump).empty())
      {
        REXLOG_WARN("pdz_native: null GPU backend: pdz_native_draw_dump refused (the stage-1 diagnostics stay with the "
                    "plugin path in stage 2 M1)");
      }
      // Stage 2 M3: what the native output does in place of the plugin's IssueSwap.
      REXLOG_INFO("pdz_native: null GPU backend: present: resolution scale {}, swap post effect {} on the native output "
                  "chain (pdz_native_post_effect {}; the plugin's IssueSwap does not run it with the null backend), "
                  "gamma from the null command processor's DC_LUT table, present timing {}",
                  Scale(), postfx::Name(postfx::PresentEffect()), REXCVAR_GET(pdz_native_post_effect) ? "on" : "off",
                  xeswap::Active() ? "XE_SWAP (pdz_native_present_timing 1)" : "Swap hook (pdz_native_present_timing 0)");
    }

    // Refreshes the GPU table mirror (cheap when unchanged) and returns the ramp the output applies.
    const uint16_t (*ActiveRamp())[256]
    {
      PresentState &p = g_present;
      if (REXCVAR_GET(pdz_native_gamma_from_gpu))
      {
        const uint64_t generation = rex::graphics::gamma_mirror::Generation();
        if (generation && generation != p.gpu_ramp_generation)
        {
          p.gpu_ramp_generation = rex::graphics::gamma_mirror::Get(p.gpu_ramp);
          p.lut_dirty = true;
        }
        if (p.gpu_ramp_generation)
        {
          return p.gpu_ramp;
        }
      }
      return p.ramp;
    }

    bool EnsureLut(NativeDevice &dev)
    {
      ActiveRamp(); // marks the LUT dirty when the GPU table changed
      PresentState &p = g_present;
      if (!p.lut)
      {
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = 256;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R16G16B16A16_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        p.lut = dev.CreateTexture(desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, "pdz_native gamma LUT");
        if (!p.lut)
        {
          return false;
        }
        p.lut_state = D3D12_RESOURCE_STATE_COPY_DEST;
        p.lut_srv = dev.AllocStagingSrv();
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R16G16B16A16_UNORM;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        dev.device()->CreateShaderResourceView(p.lut, &srv, p.lut_srv);
        p.lut_dirty = true;
      }
      if (p.lut_dirty)
      {
        p.lut_dirty = false;
        UploadSlice slice = dev.Upload(256 * 8, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
        if (!slice.cpu)
        {
          return false;
        }
        uint16_t *texels = static_cast<uint16_t *>(slice.cpu);
        for (uint32_t i = 0; i < 256; ++i)
        {
          const uint16_t (*ramp)[256] = ActiveRamp();
          texels[i * 4 + 0] = ramp[0][i];
          texels[i * 4 + 1] = ramp[1][i];
          texels[i * 4 + 2] = ramp[2][i];
          texels[i * 4 + 3] = 0xFFFF;
        }
        dev.Transition(p.lut, p.lut_state, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = p.lut;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = slice.resource;
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Offset = slice.offset;
        src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R16G16B16A16_UNORM;
        src.PlacedFootprint.Footprint.Width = 256;
        src.PlacedFootprint.Footprint.Height = 1;
        src.PlacedFootprint.Footprint.Depth = 1;
        src.PlacedFootprint.Footprint.RowPitch = 256 * 8;
        dev.list()->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
      }
      dev.Transition(p.lut, p.lut_state,
                     D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      return true;
    }

    // The LUT for the auto-diff's readback of the native output chain (pdz_native_autodiff_post_effect after /
    // before), made or refreshed on the open command list only when that readback is due.
    D3D12_CPU_DESCRIPTOR_HANDLE AutodiffLut(NativeDevice &dev)
    {
      return EnsureLut(dev) ? g_present.lut_srv : D3D12_CPU_DESCRIPTOR_HANDLE{};
    }

    // pdz_cutscene_black_detect (overnight 30-31 Oct): an 8 x 8 grid of the front buffer's pixels copied into a readback
    // ring (one 512-byte-aligned slot per pixel, 8 frames), read once the device is 4 frames past it (3 in flight),
    // and handed to the cutscene dump as luma 0-255 (average, brightest). Only for formats read here (8-bit RGBA /
    // BGRA, 10:10:10:2, 16-bit float RGBA); others are logged once and skipped. Test aid only: off by default.
    struct BrightnessRing
    {
      static constexpr uint32_t kSlots = 8, kPixels = 64, kPixelBytes = 512;
      ID3D12Resource *buffer = nullptr;
      uint8_t *cpu = nullptr;
      uint64_t frame[kSlots] = {}, swap[kSlots] = {};
      uint32_t draws[kSlots] = {};
      uint64_t last_issued = 0; // GetDrawStats().issued at the previous sample
      DXGI_FORMAT format[kSlots] = {};
      bool used[kSlots] = {};
      uint32_t next = 0;
      bool failed = false, format_logged = false;
    } g_bright;

    float LumaOf(DXGI_FORMAT format, const uint8_t *p, bool &ok)
    {
      ok = true;
      switch (format)
      {
      case DXGI_FORMAT_R8G8B8A8_UNORM:
      case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return 0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2];
      case DXGI_FORMAT_B8G8R8A8_UNORM:
      case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return 0.2126f * p[2] + 0.7152f * p[1] + 0.0722f * p[0];
      case DXGI_FORMAT_R10G10B10A2_UNORM:
      {
        uint32_t v;
        std::memcpy(&v, p, 4);
        return (0.2126f * float(v & 1023) + 0.7152f * float((v >> 10) & 1023) + 0.0722f * float((v >> 20) & 1023)) *
               (255.0f / 1023.0f);
      }
      case DXGI_FORMAT_R16G16B16A16_FLOAT:
      {
        const auto half = [](uint16_t h) {
          const uint32_t e = (h >> 10) & 31, m = h & 1023;
          const float v = e == 0 ? std::ldexp(float(m), -24) : e == 31 ? 65504.0f : std::ldexp(float(m | 1024), int(e) - 25);
          return (h & 0x8000) ? -v : v;
        };
        uint16_t c[3];
        std::memcpy(c, p, 6);
        return std::clamp(0.2126f * half(c[0]) + 0.7152f * half(c[1]) + 0.0722f * half(c[2]), 0.0f, 1.0f) * 255.0f;
      }
      default:
        ok = false;
        return 0.0f;
      }
    }

    void BrightnessHarvest(NativeDevice &dev)
    {
      BrightnessRing &b = g_bright;
      for (uint32_t s = 0; s < BrightnessRing::kSlots; ++s)
      {
        if (!b.used[s] || b.frame[s] + 4 > dev.frame_index())
        {
          continue;
        }
        b.used[s] = false;
        float sum = 0.0f, brightest = 0.0f;
        uint32_t n = 0;
        for (uint32_t i = 0; i < BrightnessRing::kPixels; ++i)
        {
          bool ok = false;
          const float l = LumaOf(b.format[s], b.cpu + (s * BrightnessRing::kPixels + i) * BrightnessRing::kPixelBytes, ok);
          if (ok)
          {
            sum += l;
            brightest = std::max(brightest, l);
            ++n;
          }
        }
        if (n)
        {
          pdz::csdump::OnBrightness(b.swap[s], sum / float(n), brightest, b.draws[s]);
        }
      }
    }

    void BrightnessProbe(NativeDevice &dev, HostTexture &tex, uint64_t swap)
    {
      BrightnessRing &b = g_bright;
      if (b.failed || !tex.resource || !dev.list())
      {
        return;
      }
      if (!b.buffer)
      {
        b.buffer = dev.CreateBuffer(uint64_t(BrightnessRing::kSlots) * BrightnessRing::kPixels * BrightnessRing::kPixelBytes,
                                    D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE,
                                    "pdz_cutscene_black_detect readback");
        void *cpu = nullptr;
        if (!b.buffer || FAILED(b.buffer->Map(0, nullptr, &cpu)))
        {
          b.failed = true;
          REXLOG_WARN("pdz_cutscene_black_detect: no readback buffer: the detector is off");
          return;
        }
        b.cpu = static_cast<uint8_t *>(cpu);
      }
      BrightnessHarvest(dev);
      bool known = false;
      uint8_t probe[8] = {};
      LumaOf(tex.resource_format, probe, known);
      if (!known)
      {
        if (!b.format_logged)
        {
          b.format_logged = true;
          REXLOG_WARN("pdz_cutscene_black_detect: front buffer host format {} not read by the detector: not sampled",
                      uint32_t(tex.resource_format));
        }
        return;
      }
      const uint32_t s = b.next;
      if (b.used[s])
      {
        return; // the ring is full (frames not yet complete): this one is skipped
      }
      b.next = (b.next + 1) % BrightnessRing::kSlots;
      const D3D12_RESOURCE_STATES before = tex.state;
      Textures::Get().Transition(tex, D3D12_RESOURCE_STATE_COPY_SOURCE);
      for (uint32_t i = 0; i < BrightnessRing::kPixels; ++i)
      {
        const uint32_t x = uint32_t((double(i % 8) + 0.5) * tex.width / 8.0);
        const uint32_t y = uint32_t((double(i / 8) + 0.5) * tex.height / 8.0);
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = b.buffer;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Offset = uint64_t(s * BrightnessRing::kPixels + i) * BrightnessRing::kPixelBytes;
        dst.PlacedFootprint.Footprint.Format = tex.resource_format;
        dst.PlacedFootprint.Footprint.Width = 1;
        dst.PlacedFootprint.Footprint.Height = 1;
        dst.PlacedFootprint.Footprint.Depth = 1;
        dst.PlacedFootprint.Footprint.RowPitch = 256;
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = tex.resource;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        const D3D12_BOX box{x, y, 0, x + 1, y + 1, 1};
        dev.list()->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
      }
      Textures::Get().Transition(tex, before);
      b.used[s] = true;
      b.frame[s] = dev.frame_index();
      b.swap[s] = swap;
      b.format[s] = tex.resource_format;
      const uint64_t issued = GetDrawStats().issued;
      b.draws[s] = uint32_t(std::min<uint64_t>(issued - b.last_issued, 0xFFFFFFFFull));
      b.last_issued = issued;
    }

    // Hands the host texture of the front buffer to the presenter with the gamma pass. Ends the native frame
    // (the refresher must submit before returning).
    // With --d3d12_debug: log (and clear) the debug layer's stored messages, tagged with where they were read.
    // At most 200 lines per process so a noisy frame cannot flood the log.
    void DumpDebugMessages(NativeDevice &dev, const char *where)
    {
      static uint32_t budget = 200;
      if (!budget || !dev.device())
      {
        return;
      }
      ID3D12InfoQueue *queue = nullptr;
      if (FAILED(dev.device()->QueryInterface(IID_PPV_ARGS(&queue))) || !queue)
      {
        return;
      }
      const UINT64 n = queue->GetNumStoredMessages();
      for (UINT64 i = 0; i < n && budget; ++i)
      {
        SIZE_T size = 0;
        if (FAILED(queue->GetMessage(i, nullptr, &size)) || size == 0 || size > 64 * 1024)
        {
          continue;
        }
        std::vector<uint8_t> buffer(size);
        auto *m = reinterpret_cast<D3D12_MESSAGE *>(buffer.data());
        if (FAILED(queue->GetMessage(i, m, &size)))
        {
          continue;
        }
        const char *sev = m->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION ? "CORRUPTION"
                          : m->Severity == D3D12_MESSAGE_SEVERITY_ERROR    ? "ERROR"
                          : m->Severity == D3D12_MESSAGE_SEVERITY_WARNING  ? "WARNING"
                                                                           : "MESSAGE";
        --budget;
        REXLOG_WARN("pdz_native: d3d12 debug ({}) {} id {}: {}", where, sev, int(m->ID),
                    m->pDescription ? m->pDescription : "");
      }
      queue->ClearStoredMessages();
      queue->Release();
    }

    void PresentFrame(const FetchDecoded &front)
    {
      NativeDevice *dev = NativeDevice::Get();
      if (!dev || !dev->in_frame())
      {
        return;
      }
      PresentState &p = g_present;
      rex::Runtime *runtime = rex::Runtime::instance();
      rex::system::IGraphicsSystem *graphics = runtime ? runtime->graphics_system() : nullptr;
      rex::ui::Presenter *presenter = graphics ? graphics->presenter() : nullptr;
      if (!presenter)
      {
        if (!p.presenter_missing_logged)
        {
          p.presenter_missing_logged = true;
          REXLOG_ERROR("pdz_native: no presenter to hand the native frame to");
        }
        return;
      }
      // cutscene_dump.h (pdz_cutscene_repeat_frame): a cutscene Swap the game presented without its cutscene update
      // (nothing drawn; log 292's black frames): the previous frame stays on screen.
      if (pdz::csdump::SkipPresent(p.swaps))
      {
        return;
      }
      // pdz_cutscene_repeat_frame (night of 31 Oct; smoothing only): a cutscene Swap with no draws since the previous
      // present (the dump's native_draws 0: retime's empty frames, the black flashes) right after a drawn cutscene
      // frame shows that frame again, at most 4 in a row (the longest run seen).
      {
        static uint64_t issued_before = 0;
        static uint32_t empty_run = 0;
        static bool prev_drawn = false;
        const uint64_t issued = GetDrawStats().issued;
        const bool empty = issued == issued_before;
        issued_before = issued;
        const bool cutscene = pdz::csdump::CutsceneSwap(p.swaps);
        if (empty && cutscene && prev_drawn && empty_run < 4)
        {
          ++empty_run;
          pdz::csdump::NoteEmptyRepeat();
          return;
        }
        empty_run = 0;
        prev_drawn = cutscene && !empty;
      }
      // A same-frame plugin capture is pending (auto-diff): the GPU plugin refreshes the requested swap itself. Its
      // refresh and ours must not run at once (the race of runs 196 / 197, see the top of this file), so ours waits
      // until the plugin has stored the capture (the request returns to 0); this frame stays in the open native
      // frame and OnSwap ends it. The plugin only clears the request after its refresh and its capture readback.
      if (const uint64_t capture = rex::ui::Presenter::RequestedGuestOutputCapture())
      {
        if (REXCVAR_GET(pdz_native_output_capture) != 2)
        {
          ++p.frames_held;
          if (!p.hold_logged)
          {
            p.hold_logged = true;
            REXLOG_INFO("pdz_native: native output held while the GPU plugin's same-frame capture of swap {} is pending "
                        "(one presenter refresh at a time; the plugin's frame is shown for it)",
                        capture);
          }
          if (++p.hold_swaps > kCaptureHoldSwaps)
          {
            // Not served (the plugin's IssueSwap count is past it): drop it. The native refresh resumes at the next
            // swap, not this one, so a plugin refresh that took the request just now has a frame to finish.
            p.hold_swaps = 0;
            ++p.holds_cancelled;
            REXLOG_WARN("pdz_native: the plugin capture of swap {} was not served in {} swaps; request dropped",
                        capture, kCaptureHoldSwaps);
            autodiff::CancelRequest();
          }
          return;
        }
        if (p.overlap_logged_for != capture)
        {
          p.overlap_logged_for = capture;
          REXLOG_WARN("pdz_native: native refresh while the GPU plugin's capture of swap {} is pending: two producers "
                      "on the presenter (pdz_native_output_capture 2)",
                      capture);
        }
      }
      p.hold_swaps = 0;
      // pdz_frame_timing_phases: the output chain; the presenter's own work before / after our refresher is split out.
      const pdz::timing::PhaseScope chain_phase(pdz::timing::Phase::kPresentChain);
      if (dev->GpuPassesOn())
      {
        dev->GpuMark(GpuPass::kPresent); // pdz_native_gpu_passes
      }
      TextureView view = Textures::Get().Lookup(front);
      if (!view.texture)
      {
        return;
      }
      if (!EnsureLut(*dev))
      {
        return;
      }
      HostTexture &tex = *view.texture;
      LogFront(front, tex);
      const uint32_t width = tex.width, height = tex.height;
      if (xeswap::Active() && xeswap::StageFrame(*dev, front, view, width, height, p.lut_srv))
      {
        // pdz_native_present_timing 1: staged; OnSwap publishes it after the frame's submit, the presenter gets it at
        // its XE_SWAP (xeswap::OnXeSwap) or at the publish.
        if (++p.frames_presented == 1)
        {
          REXLOG_INFO("pdz_native: first native frame staged for its XE_SWAP ({}x{} from guest texture {:08X})", width,
                      height, front.base_address);
        }
        return;
      }
      // pdz_native_watchdog_ms: name the part of the refresh this thread is in should it stop here.
      watchdog::Wait wait("presenter refresh: SDK before the native refresher (owner check, guest output texture "
                          "await / recreation)");
      pdz::timing::PhaseScope sdk_phase(pdz::timing::Phase::kPresentSdkPre);
      // The display aspect is the guest front buffer's (1280x720 or 640x480); the host texture may be scaled.
      presenter->RefreshGuestOutput(
          width, height, front.width, front.height,
          [&](rex::ui::Presenter::GuestOutputRefreshContext &context) -> bool {
            wait.Phase("presenter refresh: the native refresher (gamma pass, EndFrame submit)");
            // After this refresher returns, RefreshGuestOutput continues as present.sdk_post (host vblank latch wait,
            // mailbox, paint request).
            pdz::timing::PhaseScope refresher_phase(pdz::timing::Phase::kPresentChain);
            refresher_phase.ResumeAs(pdz::timing::Phase::kPresentSdkPost);
            auto &d3d12_context = static_cast<rex::ui::d3d12::D3D12Presenter::D3D12GuestOutputRefreshContext &>(context);
            ID3D12Resource *output = d3d12_context.resource_uav_capable();
            D3D12_RESOURCE_STATES state = rex::ui::d3d12::D3D12Presenter::kGuestOutputInternalState;
            dev->Transition(output, state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            // The gamma pass, then the plugin's swap post effect (pdz_native_post_effect; with none this is the
            // gamma pass alone, as before).
            postfx::ChainDesc chain;
            chain.output = output;
            chain.width = width;
            chain.height = height;
            chain.src_srv = view.srv;
            chain.lut_srv = p.lut_srv;
            chain.effect = postfx::PresentEffect();
            postfx::Effect applied = postfx::Effect::kNone;
            const bool ok = postfx::RecordChain(*dev, chain, applied);
            dev->Transition(output, state, rex::ui::d3d12::D3D12Presenter::kGuestOutputInternalState);
            // pdz_cutscene_black_detect: this Swap's front buffer sampled for the cutscene dump (off by default).
            if (pdz::csdump::WantBrightness(p.swaps))
            {
              BrightnessProbe(*dev, tex, p.swaps);
            }
            // As the plugin: after FXAA / SMAA the image has more than 8 bits per channel (the presenter's dither rule).
            context.SetIs8bpc(applied == postfx::Effect::kNone);
            dev->EndFrame(); // submits on the provider's direct queue before returning, as required
            pdz::latency::OnNativeSubmit(p.swaps); // pdz_latency_log: a reacting frame's submit
            if (REXCVAR_GET(d3d12_debug))
            {
              DumpDebugMessages(*dev, "after the native refresh");
            }
            wait.Phase("presenter refresh: SDK after the native refresher (fence signal, mailbox publish, host vblank "
                       "latch wait of up to 8 ms, paint)");
            return ok;
          });
      pdz::csdump::OnPresent(p.swaps); // cutscene_dump.h: read-only (pdz_cutscene_frame_dump; one load while off)
      {
        // Present timing 0 (the Swap hook): the same delay / gap figures as the XE_SWAP mode (pdz_frame_timing).
        const int64_t now = pdz::gpusync::NowUs();
        const int64_t delay = std::max<int64_t>(now - p.swap_us, 0);
        const int64_t gap = p.last_present_us ? std::max<int64_t>(now - p.last_present_us, 0) : 0;
        p.last_present_us = now;
        ++p.hook_presents;
        p.hook_delay_sum_us += delay;
        p.hook_delay_max_us = std::max(p.hook_delay_max_us, delay);
        pdz::timing::AddPresent(uint64_t(delay), uint64_t(gap), "Swap hook");
      }
      if (++p.frames_presented == 1)
      {
        REXLOG_INFO("pdz_native: first native frame presented ({}x{} from guest texture {:08X})", width, height,
                    front.base_address);
      }
    }

    // Writes the presenter's current guest output to pdz_native_capture_dir as <n>_<tag>_<native|emulated>.ppm (the
    // presenter's own capture, so it shows exactly what the window shows, unlike a window grab of a flip-model swap
    // chain); `captures` counts the files written. tag = "<t>s" (pdz_native_capture_s) or "swap<N>"
    // (pdz_native_capture_swaps: the presenter's latest output at that swap; same-frame pairs are the auto-diff's).
    void GrabGuestOutput(uint32_t &captures, const std::string &tag)
    {
      rex::Runtime *runtime = rex::Runtime::instance();
      rex::system::IGraphicsSystem *graphics = runtime ? runtime->graphics_system() : nullptr;
      rex::ui::Presenter *presenter = graphics ? graphics->presenter() : nullptr;
      rex::ui::RawImage image;
      if (!presenter || !presenter->CaptureGuestOutput(image) || !image.width || !image.height)
      {
        REXLOG_INFO("pdz_native: guest output capture unavailable at {}", tag);
        return;
      }
      std::string dir = REXCVAR_GET(pdz_native_capture_dir);
      if (dir.empty()) dir = ".";
      std::error_code ec;
      std::filesystem::create_directories(dir, ec); // log 022: a missing folder made every capture fail silently
      const std::string path =
          fmt::format("{}\\{:02}_{}_{}.ppm", dir, ++captures, tag, g_present.native_output ? "native" : "emulated");
      if (FILE *f = std::fopen(path.c_str(), "wb"))
      {
        std::fprintf(f, "P6\n%u %u\n255\n", image.width, image.height);
        std::vector<uint8_t> row(size_t(image.width) * 3);
        for (uint32_t y = 0; y < image.height; ++y)
        {
          const uint8_t *src = image.data.data() + size_t(y) * image.stride;
          for (uint32_t x = 0; x < image.width; ++x)
          {
            row[3 * x] = src[4 * x];
            row[3 * x + 1] = src[4 * x + 1];
            row[3 * x + 2] = src[4 * x + 2];
          }
          std::fwrite(row.data(), 1, row.size(), f);
        }
        std::fclose(f);
        REXLOG_INFO("pdz_native: captured the guest output {}x{} ({}) to {}", image.width, image.height,
                    g_present.native_output ? "native" : "emulated", path);
      }
      else
      {
        REXLOG_WARN("pdz_native: capture could not open {} for writing", path);
      }
    }

    // Test aids (off by default): toggle the output like F9 every pdz_native_output_toggle_s seconds of wall-clock
    // time, and capture every pdz_native_capture_s seconds or at the pdz_native_capture_swaps swap numbers: the
    // auto-diff's same-frame pair with pdz_native_autodiff on, else a grab of the presenter's guest output (max 64).
    void PollTestAids()
    {
      const uint64_t now = GetTickCount64();
      static uint64_t start = now;
      const int32_t toggle = REXCVAR_GET(pdz_native_output_toggle_s);
      static uint64_t next_toggle = 0;
      if (toggle > 0)
      {
        if (!next_toggle) next_toggle = start + uint64_t(toggle) * 1000;
        if (now >= next_toggle)
        {
          next_toggle = now + uint64_t(toggle) * 1000;
          REXLOG_INFO("pdz_native: output toggle test (every {} s)", toggle);
          ToggleNativeOutput();
        }
      }
      static uint32_t captures = 0; // guest output grabs written
      // Capture points by swap number replace the seconds ticks. DueSwapPoint names the next swap when it is a
      // point, so the request made now captures exactly that swap.
      if (autodiff::SwapPointsSet())
      {
        if (const uint64_t point = autodiff::DueSwapPoint())
        {
          if (autodiff::Enabled())
          {
            if (autodiff::CaptureInFlight())
            {
              REXLOG_WARN("pdz_native: capture point swap {} skipped: the previous auto-diff capture is still in "
                          "flight",
                          point);
            }
            else
            {
              autodiff::Request();
            }
          }
          else if (captures < 64)
          {
            GrabGuestOutput(captures, fmt::format("swap{}", point));
          }
        }
        return;
      }
      const int32_t capture = REXCVAR_GET(pdz_native_capture_s);
      static uint64_t next_capture = 0;
      if (capture > 0 && captures < 64)
      {
        if (!next_capture) next_capture = start + uint64_t(capture) * 1000;
        if (now >= next_capture && autodiff::Enabled())
        {
          // Same-frame native / emulated pair, diff and score (native_autodiff.cpp) instead of a window grab.
          next_capture = now + uint64_t(capture) * 1000;
          if (g_present.native_output && REXCVAR_GET(pdz_native_output_capture) == 0)
          {
            // The plugin's capture refresh would be a second producer next to ours (top of this file).
            ++g_present.captures_skipped;
            if (!g_present.capture_skip_logged)
            {
              g_present.capture_skip_logged = true;
              REXLOG_INFO("pdz_native: auto-diff captures are skipped while the native output is shown "
                          "(pdz_native_output_capture 0: the GPU plugin's capture refresh would run beside ours)");
            }
          }
          else
          {
            autodiff::Request();
          }
        }
        else if (now >= next_capture)
        {
          next_capture = now + uint64_t(capture) * 1000;
          GrabGuestOutput(captures, fmt::format("{}s", (now - start) / 1000));
        }
      }
    }

    // Mark-this-spot (test aid; bind_mark_spot, default F12: F7 is the SDK's achievements overlay). A press is counted
    // by the bind on the UI thread (RegisterMarkBind), or, when the bind was never registered, by GetAsyncKeyState(VK_F12)
    // polled here once per swap, edge-triggered (as F9). The swap path (PollMark, every Swap, renderer on or off) turns
    // a press into a mark of the next swap: the pdz_mark line is built now from the game state read now (one swap
    // before the captured frame) and native_autodiff.cpp captures the native frame (renderer on) and the emulated one
    // (GPU plugin rendering) at full resolution, writes logs\marks\mark_NNN_{native,emulated}.png next to the exe on a
    // writer thread and logs the line with the files. NNN continues from the files already in the folder.
    namespace mark
    {
      std::atomic<uint32_t> g_presses{0};
      std::atomic<bool> g_bind_registered{false};
      bool g_key_was_down = false;
      uint32_t g_next = 0; // the next mark number; 0 = the folder not scanned yet
      std::string g_dir;

      // TU3 guest data, read only. A value that does not look right is logged as "?".
      // The game block (render/game_pace.cpp): +0 -> the game state object, +68 -> the cutscene player.
      constexpr uint32_t kGameBlock = 0x82D6BFD8;
      constexpr uint32_t kGameState = 8;      // game state object: state, 21 = in a level (game_pace.cpp)
      constexpr uint32_t kStateInLevel = 21;
      constexpr uint32_t kGameLevelId = 25;   // byte: the level id (sub_82230470 stores it; sub_822774D0 callers)
      constexpr uint32_t kCutscenePlayer = 68; // game block +68 (game_pace.cpp)
      constexpr uint32_t kCutsceneState = 12;  // u32, 3 = playing (sub_8221ED68)
      // The level list as sub_822774D0 reads it: [+32] id -> list index (s32, < 0 = none), [+8] index -> 8-byte entry
      // (+0 name, +4 id, +5 kind; dlc_maps.cpp).
      constexpr uint32_t kLevelList = 0x82D6C2B4;
      constexpr uint32_t kPlayerOne = 0x82E3C3E4; // player 1 (anim_viewer.cpp; launch 82E3B624)
      constexpr uint32_t kPosition = 48;          // vec3 float, cm
      // View yaw / pitch in degrees (docs/LOOK_PIPELINE.md: player vtable +24 / +28; TU3's accessors sub_8254E0E0 /
      // sub_8254E0E8 load the same offsets).
      constexpr uint32_t kViewYaw = 0x1670;
      constexpr uint32_t kViewPitch = 0x1674;
      constexpr uint32_t kZoom = 0x1A9C; // scope zoom amount, 0 = not zoomed (look_tuning.cpp)

      // Multiplayer maps by their menu names (docs/MULTIPLAYER_GUIDE.md); other levels keep Rare's name.
      constexpr std::pair<const char *, const char *> kMenuNames[] = {
          {"aid_xml_level_multi_subway", "Subway"},     {"aid_xml_level_multi_chinatown", "Urban"},
          {"aid_xml_level_multi_desert", "Desert"},     {"aid_xml_level_multi_temple", "Temple"},
          {"aid_xml_level_multi_africa", "Old Town"},   {"aid_xml_level_multi_towersmall", "Tower"},
          {"aid_xml_level_multi_alcatraz", "Facility"}, {"aid_xml_level_multi_temple64", "Ruin"},
      };

      std::string Number(float v)
      {
        return std::isfinite(v) && std::fabs(v) < 1.0e7f ? fmt::format("{:.1f}", v) : std::string("?");
      }

      std::string Zoom(float v)
      {
        return std::isfinite(v) && std::fabs(v) < 1.0e4f ? fmt::format("{:g}", v) : std::string("?");
      }

      // A guest C string (printable ASCII only, at most 64 characters); "?" when it is not one.
      std::string GuestText(const uint8_t *base, uint32_t address)
      {
        if (!IsGuestPointer(address))
        {
          return "?";
        }
        std::string s;
        for (uint32_t i = 0; i < 64; ++i)
        {
          const uint8_t c = Load8(base, address + i);
          if (!c)
          {
            return s.empty() ? std::string("?") : s;
          }
          if (c < 0x20 || c > 0x7E)
          {
            return "?";
          }
          s += char(c);
        }
        return s + "...";
      }

      std::string Level(const uint8_t *base, uint32_t game)
      {
        // The loaded level's own name first (game_pace's, as its "pdz_pace: level" line: single player levels too,
        // log 608 named Bridge "aid_xml_level_multi_trench" from the multiplayer list's id 0x0C).
        std::string raw;
        const std::string sp = pdz::pace::SpLevelName(base, &raw);
        if (!raw.empty())
        {
          for (const auto &[internal, menu] : kMenuNames)
          {
            if (raw == internal)
            {
              return fmt::format("{} ({})", menu, raw);
            }
          }
          return sp.empty() ? raw : fmt::format("{} ({})", sp, raw);
        }
        if (!IsGuestPointer(game))
        {
          return "?";
        }
        const uint32_t id = Load8(base, game + kGameLevelId);
        const uint32_t ids = Load32(base, kLevelList + 32), entries = Load32(base, kLevelList + 8);
        std::string name = "?";
        if (IsGuestPointer(ids) && IsGuestPointer(entries))
        {
          const int32_t index = int32_t(Load32(base, ids + id * 4));
          if (index < 0)
          {
            name = "none";
          }
          else if (index < 4096)
          {
            const uint32_t entry = Load32(base, entries + uint32_t(index) * 4);
            name = IsGuestPointer(entry) ? GuestText(base, Load32(base, entry)) : std::string("?");
          }
        }
        for (const auto &[internal, menu] : kMenuNames)
        {
          if (name == internal)
          {
            return fmt::format("{} ({}, id 0x{:02X})", menu, name, id);
          }
        }
        return fmt::format("{} (id 0x{:02X})", name, id);
      }

      // No camera-mode field is known in TU3: what the game is showing, from the cutscene player's state and the game
      // state.
      std::string Camera(const uint8_t *base, uint32_t game)
      {
        const uint32_t cutscene = Load32(base, kGameBlock + kCutscenePlayer);
        const uint32_t cutscene_state = IsGuestPointer(cutscene) ? Load32(base, cutscene + kCutsceneState) : 0;
        if (cutscene_state == 3)
        {
          return "cutscene";
        }
        const uint32_t state = IsGuestPointer(game) ? Load32(base, game + kGameState) : ~0u;
        if (state == kStateInLevel)
        {
          return cutscene_state ? fmt::format("gameplay (cutscene state {})", cutscene_state) : std::string("gameplay");
        }
        return state == ~0u ? std::string("?") : fmt::format("not in a level (game state {})", state);
      }

      // A cvar's value by name ("?" when it is not registered: the plugin's, or one from another branch).
      std::string Flag(std::string_view name)
      {
        const std::string value = rex::cvar::GetFlagByName(name);
        return value.empty() ? std::string("?") : value;
      }

      // pdz_shadow_distance_scale and pdz_shadow_map_scale, then every other pdz_shadow_* cvar this build has.
      std::string Shadows()
      {
        std::string s = fmt::format("distance {} map {}", Flag("pdz_shadow_distance_scale"), Flag("pdz_shadow_map_scale"));
        std::vector<std::string> names = rex::cvar::ListFlags();
        std::sort(names.begin(), names.end());
        for (const std::string &name : names)
        {
          if (name.rfind("pdz_shadow_", 0) == 0 && name != "pdz_shadow_distance_scale" && name != "pdz_shadow_map_scale")
          {
            s += fmt::format(" {} {}", name, Flag(name));
          }
        }
        return s;
      }

      // <exe dir>\logs\marks, made on the first mark.
      const std::string &Dir()
      {
        if (g_dir.empty())
        {
          char exe[MAX_PATH] = {};
          const DWORD n = GetModuleFileNameA(nullptr, exe, MAX_PATH);
          std::string exe_dir(exe, n < MAX_PATH ? n : 0);
          const size_t slash = exe_dir.find_last_of("\\/");
          exe_dir = slash == std::string::npos ? std::string(".") : exe_dir.substr(0, slash);
          g_dir = exe_dir + "\\logs\\marks";
          std::error_code ec;
          std::filesystem::create_directories(g_dir, ec);
        }
        return g_dir;
      }

      // The next mark number: one past the highest mark_NNN_* already in the folder (scanned once), then counting.
      uint32_t NextNumber()
      {
        if (!g_next)
        {
          uint32_t highest = 0;
          std::error_code ec;
          for (std::filesystem::directory_iterator it(Dir(), ec), end; !ec && it != end; it.increment(ec))
          {
            const std::string name = it->path().filename().string();
            if (name.rfind("mark_", 0) != 0)
            {
              continue;
            }
            uint32_t value = 0;
            size_t i = 5;
            for (; i < name.size() && i < 14 && name[i] >= '0' && name[i] <= '9'; ++i)
            {
              value = value * 10 + uint32_t(name[i] - '0');
            }
            if (i > 5)
            {
              highest = std::max(highest, value);
            }
          }
          g_next = highest + 1;
        }
        return g_next++;
      }

      void Start(const uint8_t *base)
      {
        const std::string &dir = Dir();
        const uint32_t number = NextNumber();
        const uint64_t swap = autodiff::SwapCount() + 1;
        const bool null_backend = pdz::gpusync::NullBackend();
        const bool native = Enabled();
        const bool native_shown = native && NativeOutputSelected();
        autodiff::MarkRequest r;
        r.number = number;
        r.native = native;
        r.emulated = !null_backend;
        if (r.emulated && native_shown && REXCVAR_GET(pdz_native_output_capture) == 0)
        {
          // The plugin's capture refresh beside ours is the race of runs 196 / 197 (top of this file).
          r.emulated = false;
          r.emulated_skipped = "emulated not taken while the window shows the native frame (pdz_native_output_capture "
                               "0; 1 takes it, that swap then shows the emulated frame)";
        }
        r.native_path = fmt::format("{}\\mark_{:03}_native.png", dir, number);
        r.emulated_path = fmt::format("{}\\mark_{:03}_emulated.png", dir, number);

        const uint32_t game = Load32(base, kGameBlock);
        const uint32_t player = Load32(base, kPlayerOne);
        const bool have_player = IsGuestPointer(player);
        const auto field = [&](uint32_t offset) {
          return have_player ? Number(LoadFloat(base, player + offset)) : std::string("?");
        };
        const std::string renderer =
            null_backend ? std::string("native (graphics_backend null)")
            : native     ? fmt::format("mixed, window shows {}", native_shown ? "native" : "emulated")
                         : std::string("emulated (native renderer off)");
        r.line = fmt::format("pdz_mark: #{} level {}, player pos ({} {} {}), look yaw {} pitch {}, camera {}, zoom {}, "
                             "swap {}, renderer {}, resolution_scale {}{}, shadows {}",
                             number, Level(base, game), field(kPosition), field(kPosition + 4), field(kPosition + 8),
                             field(kViewYaw), field(kViewPitch), Camera(base, game),
                             have_player ? Zoom(LoadFloat(base, player + kZoom)) : std::string("?"), swap,
                             renderer, Flag("resolution_scale"),
                             native ? fmt::format(" (native {})", Scale()) : std::string(), Shadows());
        autodiff::StartMark(std::move(r));
      }
    } // namespace mark

    // pdz_trace_on_mark: frame A (the fix as it is) and frame B (F1 flipped), each a mark plus the frame trace, the
    // draw dump and the transwater trace of its swap; the lines also go to trace_NNN\trace.log.
    namespace tom
    {
      struct State
      {
        int stage = 0; // 0 idle, 1 frame A armed, 2 waiting with F1 flipped, 3 frame B armed, 4 closing
        uint64_t armed_swap = 0, wait_until = 0;
        bool fix_before = true;
        std::filesystem::path dir;
        spdlog::sink_ptr sink;
      } g;

      void Arm(const uint8_t *base, const char *which)
      {
        // Frame A: every draw dumped (the '*'), and the picked draw is the first of PS 675a96f9, Rare's see-through
        // depth rebuild (sub_82622C8C, once a frame right before the first see-through layer): colour target 0 is
        // read back before each target change and after it and the next 63 draws (the see-through layers' draws;
        // trace_NNN\draws). Frame B: lines only. A new text re-arms the dump.
        const bool a = *which == 'A';
        REXCVAR_SET(pdz_native_draw_dump, std::string(a ? "675a96f9,*" : "*,*"));
        REXCVAR_SET(pdz_native_draw_dump_frames, 1);
        REXCVAR_SET(pdz_native_draw_dump_from, 0);
        REXCVAR_SET(pdz_native_draw_dump_every, 1);
        REXCVAR_SET(pdz_native_draw_dump_lines, 400000);
        REXCVAR_SET(pdz_native_draw_dump_pick, 0);
        REXCVAR_SET(pdz_native_draw_dump_rt, a ? 64 : 0);
        REXCVAR_SET(pdz_native_draw_dump_dir, a ? (g.dir / "draws").string() : std::string());
        // Rare's mesh calls of this game frame and the next: the dumped draws are in one of them (with the record
        // thread off the native draws inline, so each call's draws sit between its two lines).
        pdz::transwater::TraceFrame(pdz::transwater::CurrentFrame(base), 2);
        g.armed_swap = autodiff::SwapCount() + 1;
        SetFrameTrace(true);
        REXLOG_INFO("pdz_trace_on_mark: frame {} at swap {} (pdz_fix_transparency_water {}); pdz_trace: begin frame of "
                    "swap {}",
                    which, g.armed_swap, REXCVAR_GET(pdz_fix_transparency_water) ? "on" : "off", g.armed_swap);
        mark::Start(base); // the mark of the same swap: screenshot and pdz_mark line
      }

      void Begin(const uint8_t *base)
      {
        const std::filesystem::path marks = mark::Dir();
        std::error_code ec;
        for (uint32_t n = 1; n < 1000; ++n)
        {
          const auto d = marks / fmt::format("trace_{:03}", n);
          if (!std::filesystem::exists(d, ec))
          {
            g.dir = d;
            break;
          }
        }
        std::filesystem::create_directories(g.dir, ec);
        try
        {
          g.sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>((g.dir / "trace.log").string(), true);
          rex::AddSink(g.sink);
        }
        catch (...)
        {
          g.sink = nullptr;
        }
        g.fix_before = REXCVAR_GET(pdz_fix_transparency_water);
        pdz::transwater::ShowStatus("Trace capture started: hold still...", 30);
        REXLOG_INFO("pdz_trace_on_mark: capture {} begins (two frames: the fix {} then {})", g.dir.string(),
                    g.fix_before ? "on" : "off", g.fix_before ? "off" : "on");
        g.stage = 1;
        Arm(base, "A");
      }

      void EndTraceIfDone()
      {
        if (FrameTraceActive() && autodiff::SwapCount() > g.armed_swap)
        {
          ResolveTraceQueries();
          SetFrameTrace(false);
          REXLOG_INFO("pdz_trace: end frame of swap {}", g.armed_swap);
        }
      }

      void Step(const uint8_t *base)
      {
        if (g.stage == 0)
        {
          return;
        }
        const uint64_t now = autodiff::SwapCount();
        if (g.stage == 1 || g.stage == 3)
        {
          EndTraceIfDone();
          if (FrameTraceActive() || now <= g.armed_swap + 2)
          {
            return;
          }
          if (g.stage == 1)
          {
            REXCVAR_SET(pdz_fix_transparency_water, !g.fix_before);
            g.wait_until = now + 15;
            g.stage = 2;
          }
          else
          {
            REXCVAR_SET(pdz_fix_transparency_water, g.fix_before);
            g.wait_until = now + 30;
            g.stage = 4;
          }
          return;
        }
        if (g.stage == 2 && now >= g.wait_until && !autodiff::MarkBusy())
        {
          g.stage = 3;
          Arm(base, "B");
          return;
        }
        if (g.stage == 4 && now >= g.wait_until && !autodiff::MarkBusy())
        {
          REXCVAR_SET(pdz_native_draw_dump, std::string());
          REXCVAR_SET(pdz_native_draw_dump_rt, 0);
          REXCVAR_SET(pdz_native_draw_dump_dir, std::string());
          REXLOG_INFO("pdz_trace_on_mark: capture {} saved (frame A with the fix {}, frame B with it {}; the fix is back "
                      "{})",
                      g.dir.string(), g.fix_before ? "on" : "off", g.fix_before ? "off" : "on",
                      g.fix_before ? "on" : "off");
          rex::FlushLogging();
          if (g.sink)
          {
            rex::RemoveSink(g.sink);
            g.sink->flush();
            g.sink = nullptr;
          }
          if (FILE *f = std::fopen((g.dir / "README.txt").string().c_str(), "w"))
          {
            std::fprintf(f, "Trace on mark (pdz_trace_on_mark): two frames, A with pdz_fix_transparency_water %s, B "
                            "with it %s.\ntrace.log: every log line of the capture (pdz_mark, pdz_trace, pdz_drawdump, "
                            "pdz_transwater).\ndraws\\: frame A's colour target 0 after each draw from Rare's "
                            "see-through depth rebuild on (f<frame>_d<draw>_rt0_after.ppm, the draw numbers as in the "
                            "pdz_trace lines).\nThe two screenshots are the mark_NNN_*.png files named in the pdz_mark "
                            "lines (logs\\marks).\n",
                         g.fix_before ? "on" : "off", g.fix_before ? "off" : "on");
            std::fclose(f);
          }
          pdz::transwater::ShowStatus("Trace saved: " + g.dir.filename().string() + " (2 frames, fix on and off). You can move again.", 6);
          g.stage = 0;
        }
      }
    } // namespace tom

    // Every Swap, renderer on or off (after autodiff::CountSwap).
    void PollMark(const uint8_t *base)
    {
      if (!mark::g_bind_registered.load(std::memory_order_relaxed))
      {
        const bool down = (GetAsyncKeyState(VK_F12) & 0x8000) != 0;
        if (down && !mark::g_key_was_down)
        {
          mark::g_presses.fetch_add(1, std::memory_order_relaxed);
        }
        mark::g_key_was_down = down;
      }
      // pdz_test_mark (testing tool): each new value is one F12 press (input scripts: 'cvar <t> pdz_test_mark 1', 2...).
      static int32_t test_mark_seen = 0;
      if (const int32_t m = REXCVAR_GET(pdz_test_mark); m != test_mark_seen)
      {
        test_mark_seen = m;
        if (m != 0)
        {
          mark::g_presses.fetch_add(1, std::memory_order_relaxed);
        }
      }
      autodiff::MarkOnSwap();
      tom::Step(base);
      if (!mark::g_presses.load(std::memory_order_relaxed) || autodiff::MarkBusy() || tom::g.stage != 0)
      {
        return; // a press during a capture waits for it
      }
      mark::g_presses.fetch_sub(1, std::memory_order_relaxed);
      if (REXCVAR_GET(pdz_trace_on_mark))
      {
        tom::Begin(base);
        return;
      }
      mark::Start(base);
    }

    void PollF9()
    {
      PollTestAids();
      PresentState &p = g_present;
      if (p.bind_registered)
      {
        return;
      }
      const bool down = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
      if (down && !p.f9_was_down)
      {
        ToggleNativeOutput();
      }
      p.f9_was_down = down;
    }
  } // namespace

  bool NativeOutputSelected()
  {
    InitOutputFlag();
    return g_present.native_output;
  }

  void ToggleNativeOutput()
  {
    if (pdz::gpusync::NullBackend())
    {
      // graphics_backend null: there is no emulated frame to switch to (checked before the render mutex: the F9 bind
      // runs on the UI thread).
      static std::atomic<bool> logged{false};
      if (!logged.exchange(true))
      {
        REXLOG_INFO("pdz_native: null GPU backend: F9 / pdz_native_output_toggle_s ignored (there is no emulated "
                    "frame to switch to)");
      }
      return;
    }
    // Also called from the F9 bind on the UI thread: a game thread stuck while holding the render mutex would stop the
    // window's message loop here (pdz_native_watchdog_ms names it).
    std::unique_lock<std::recursive_mutex> lock(RenderMutex(), std::defer_lock);
    {
      watchdog::Wait wait("render mutex (output toggle: F9 or pdz_native_output_toggle_s)");
      lock.lock();
    }
    InitOutputFlag();
    g_present.native_output = !g_present.native_output;
    rex::ui::Presenter::SetGuestOutputExternalOwner(g_present.native_output);
    REXLOG_INFO("pdz_native: window shows the {} frame{}", g_present.native_output ? "NATIVE" : "emulated",
                g_present.native_output && g_present.last_frame_missed
                    ? " (the last frame had a draw skipped for a missing shader: such frames still show emulated)"
                    : "");
  }

  void RegisterBinds()
  {
    g_present.bind_registered = true;
    rex::ui::RegisterBind("bind_native_output", "F9",
                          "Native renderer: toggle between the native and the emulated frame in the window",
                          [] { ToggleNativeOutput(); });
  }

  void RegisterMarkBind()
  {
    mark::g_bind_registered = true;
    rex::ui::RegisterBind("bind_mark_spot", "F12",
                          "Test aid: mark this spot: full-resolution capture of the next frame (native, and emulated "
                          "too with the GPU plugin rendering) to logs/marks, and one pdz_mark log line (level, player "
                          "position, look, swap, renderer and shadow settings)",
                          [] { mark::g_presses.fetch_add(1, std::memory_order_relaxed); });
  }

  void PresentPendingFrame()
  {
    PresentState &p = g_present;
    if (p.pending)
    {
      p.pending = false;
      PresentFrame(p.pending_front);
    }
  }

  void ArmPresenterOwner() { InitOutputFlag(); }

  void OnSwap(uint8_t *base, uint32_t device, uint32_t front_buffer)
  {
    const pdz::timing::Scope timing(pdz::timing::Bucket::kNative); // pdz_frame_timing
    (void)device;
    RenderLock lock(RenderMutex());
    const pdz::timing::PhaseScope phase(pdz::timing::Phase::kSwapOther); // pdz_frame_timing_phases (nested: present)
    // Present timing: this Swap's number (= the GPU plugin's IssueSwap count) and entry time.
    // pdz_native_record_thread: the present delay counts from the game thread's Swap, not from the worker's OnSwap.
    const int64_t queued_us = recthread::CurrentSwapUs();
    g_present.swap_us = queued_us ? queued_us : pdz::gpusync::NowUs();
    xeswap::g_game_swaps.store(++g_present.swaps, std::memory_order_release);
    g_present.staged_slot = -1;
    NoteRenderThread("swap");
    FlushPendingBegin(base); // a BeginVertices batch of this frame ended without EndVertices
    drawdump::OnSwap(base);  // pdz_native_draw_dump: ends a dump frame (guest memory only, no GPU work)
    autodiff::CountSwap(); // every swap, so the index matches the GPU plugin's IssueSwap count
    PollMark(base);        // bind_mark_spot: renderer on or off
    memlog::OnSwap();      // pdz_native_memlog_s: a memory sample every N seconds, renderer on or off
    watchdog::OnSwap(NativeDevice::Existing()); // pdz_native_watchdog_ms: the Swap the hang report counts from
    if (!Enabled())
    {
      // Existing, not Get: with the renderer off from the start no device is made (Get used to make it at the first
      // swap: three 64 MB upload buffers and the descriptor heaps for nothing). One switched off at run time ends its
      // open frame and releases what the GPU is done with (REVIEW2.md M2: only BeginFrame drained them).
      if (NativeDevice *existing = NativeDevice::Existing())
      {
        if (existing->in_frame())
        {
          existing->EndFrame();
        }
        existing->ReleaseCompleted();
      }
      return;
    }
    NativeDevice *dev = NativeDevice::Get();
    InitOutputFlag();
    NullBackendRefusals(); // graphics_backend null: the policy line and the refused tools, once
    RecordThreadRefusals(); // pdz_native_record_thread: the refused tools, once
    PollF9();
    if (!EnsureFrame() || !dev)
    {
      return;
    }
    {
      PresentState &p = g_present;
      const bool missed = FrameHadShaderMiss(dev->frame_index());
      p.last_frame_missed = missed;
      ++(missed ? p.frames_missed : p.frames_complete);
      ++(missed ? p.window_missed : p.window_complete);
      const int32_t every = REXCVAR_GET(pdz_native_log_frames);
      if (every > 0 && p.window_complete + p.window_missed >= uint32_t(every))
      {
        PDZ_LOG_DEFERRED("pdz_native: frames (periodic): last {} complete {} with a shader miss {} | total complete {} "
                    "missed {} | shown native {} fell back {}",
                    p.window_complete + p.window_missed, p.window_complete, p.window_missed, p.frames_complete,
                    p.frames_missed, p.frames_presented, p.frames_fallen_back);
        p.window_complete = p.window_missed = 0;
      }
    }
    // The auto-diff before the present: its native readback needs this frame still open, and with the native output
    // shown the refresh ends it (a capture swap with pdz_native_output_capture 1, or one requested before an F9
    // switch, was never read back or taken before: the request stayed pending for the rest of the run).
    if (autodiff::Enabled() && dev->in_frame())
    {
      const bool front_valid = IsGuestPointer(front_buffer);
      const FetchDecoded front = front_valid ? SwapFront(base, front_buffer) : FetchDecoded();
      autodiff::OnSwap(*dev, front, front_valid, !FrameHadShaderMiss(dev->frame_index()), ActiveRamp(), &AutodiffLut);
    }
    // bind_mark_spot: the native frame of a mark, read back like the auto-diff's before the present ends the frame.
    if (autodiff::MarkNativeDue() && dev->in_frame())
    {
      const bool front_valid = IsGuestPointer(front_buffer);
      const FetchDecoded front = front_valid ? SwapFront(base, front_buffer) : FetchDecoded();
      autodiff::MarkReadNative(*dev, front, front_valid, ActiveRamp(), &AutodiffLut);
    }
    if (NativeOutputSelected() && IsGuestPointer(front_buffer))
    {
      // A frame with a draw skipped for a missing shader is never shown half-drawn: the GPU plugin presents
      // its own frame instead (the owner flag is released for it) and the native output resumes with the
      // next complete frame. The miss itself is logged once per hash by the shader cache.
      // graphics_backend null: the plugin renders nothing, so handing it the frame would show a stale picture (and
      // freeze it for as long as a shader stays missing): the frame is shown as drawn instead, the skipped draws
      // absent for that frame (a new vertex shader variant is pending translation for a few frames; a missing pixel
      // shader stays missing), counted for the SUMMARY.
      const bool missed_frame = FrameHadShaderMiss(dev->frame_index());
      if (missed_frame && pdz::gpusync::NullBackend())
      {
        ++g_present.frames_shown_with_miss;
        if (!g_present.null_miss_logged)
        {
          g_present.null_miss_logged = true;
          REXLOG_WARN("pdz_native: null GPU backend: frame {} had a draw skipped for a missing shader; it is shown as "
                      "drawn (no emulated frame to fall back to). Further such frames are counted for the SUMMARY",
                      dev->frame_index());
        }
      }
      if (missed_frame && !pdz::gpusync::NullBackend())
      {
        rex::ui::Presenter::SetGuestOutputExternalOwner(false);
        ++g_present.frames_fallen_back;
        if (!g_present.fallback_logged)
        {
          g_present.fallback_logged = true;
          REXLOG_INFO("pdz_native: frame {} had a shader miss; the emulated frame is shown for such frames",
                      dev->frame_index());
        }
        g_present.pending = false;
      }
      else
      {
        rex::ui::Presenter::SetGuestOutputExternalOwner(true);
        const FetchDecoded front = SwapFront(base, front_buffer);
        if (REXCVAR_GET(pdz_native_present_mode) != 0 && xeswap::Active() && !g_present.present_mode_logged)
        {
          g_present.present_mode_logged = true;
          REXLOG_INFO("pdz_native: pdz_native_present_mode 1 ignored with pdz_native_present_timing 1 (the frame is "
                      "staged at its own Swap and presented at its XE_SWAP)");
        }
        if (REXCVAR_GET(pdz_native_present_mode) == 0 || xeswap::Active())
        {
          PresentFrame(front);
        }
        else
        {
          g_present.pending = true;
          g_present.pending_front = front;
        }
      }
    }
    Textures::Get().EndFrame(dev->frame_index());
    if (dev->in_frame())
    {
      dev->EndFrame();
    }
    if (g_present.staged_slot >= 0)
    {
      // pdz_native_present_timing 1: the staged frame is submitted; it goes to the presenter at its XE_SWAP.
      if (REXCVAR_GET(d3d12_debug))
      {
        DumpDebugMessages(*dev, "after the native frame staged for XE_SWAP");
      }
      const pdz::timing::PhaseScope publish_phase(pdz::timing::Phase::kPresentSdkPost); // the refresh, if due now
      xeswap::Publish(g_present.staged_slot);
      g_present.staged_slot = -1;
    }
  }

  void ApplyGammaRamp(const uint8_t *base, uint32_t ramp)
  {
    const pdz::timing::Scope timing(pdz::timing::Bucket::kNative); // pdz_frame_timing
    RenderLock lock(RenderMutex());
    InitOutputFlag();
    // The record thread's copy of the 1536 bytes when it runs this (native_view.h), guest memory otherwise.
    const uint8_t *p = view::Ptr(base, ramp, 3 * 256 * 2);
    for (uint32_t c = 0; c < 3; ++c)
    {
      for (uint32_t i = 0; i < 256; ++i)
      {
        const uint8_t *e = p + (c * 256 + i) * 2;
        g_present.ramp[c][i] = uint16_t((uint16_t(e[0]) << 8) | e[1]);
      }
    }
    g_present.ramp_set = true;
    g_present.lut_dirty = true;
  }

  void ChoosePresentTimingAtStartup()
  {
    const int32_t timing = REXCVAR_GET(pdz_native_present_timing);
    if (!pdz::gpusync::NullBackend())
    {
      if (timing != 0)
      {
        REXLOG_INFO("pdz_native: pdz_native_present_timing {} ignored: it needs graphics_backend null (the GPU plugin "
                    "renders and presents at its own IssueSwap; a shown native output presents from the Swap hook)",
                    timing);
      }
      return; // the plugin path is unchanged
    }
    if (timing == 1)
    {
      xeswap::g.active = true;
      rex::graphics::null_gpu::SetSwapListener(&xeswap::OnXeSwap);
      REXLOG_INFO("pdz_native: null GPU backend: present timing XE_SWAP (pdz_native_present_timing 1): each native frame "
                  "is staged at the Swap hook (gamma pass and swap post effect into one of {} staging textures) and "
                  "handed to the presenter when the null command processor reaches its XE_SWAP (that thread, as the "
                  "D3D12 backend's IssueSwap), or at its staging if the XE_SWAP came first; frame_pacing pairs a "
                  "presented frame with the command processor's swap",
                  xeswap::kSlots);
      return;
    }
    rex::ui::frame_pacing::SetOutputPairing(rex::ui::frame_pacing::OutputPairing::kNewestVdSwap);
    REXLOG_INFO("pdz_native: null GPU backend: present timing Swap hook (pdz_native_present_timing 0): each native frame "
                "is handed to the presenter on the game thread right after the guest Swap; frame_pacing pairs a "
                "presented frame with the newest VdSwap (the 'L' / 'D' latency rows)");
  }

  void LogTestSummary(const char *why)
  {
    RenderLock lock(RenderMutex());
    const PresentState &p = g_present;
    const DrawStats &d = GetDrawStats();
    REXLOG_INFO("pdz_native: SUMMARY ({}): frames complete {} with a shader miss {} | shown native {} fell back {} | "
                "draws issued {} skipped shader {} pipeline {} geometry {} targets {} descriptors {} | {} | scripted "
                "input presses {}{}",
                why, p.frames_complete, p.frames_missed, p.frames_presented, p.frames_fallen_back, d.issued,
                d.skipped_shader, d.skipped_pipeline, d.skipped_geometry, d.skipped_targets, d.skipped_descriptors,
                autodiff::Summary(), pdz::test::ScriptedPresses(),
                pdz::gpusync::NullBackend()
                    ? fmt::format(" | null GPU backend: frames shown with a shader miss {}", p.frames_shown_with_miss)
                    : std::string());
    if (xeswap::Active())
    {
      // Not "pdz_native: SUMMARY" (run_all_missions.ps1 takes the last line with that text).
      REXLOG_INFO("pdz_present: SUMMARY ({}): {}", why, xeswap::Summary());
    }
    else if (p.hook_presents)
    {
      REXLOG_INFO("pdz_present: SUMMARY ({}): timing Swap hook | presented {} | delay from the native OnSwap ms avg {:.2f} "
                  "max {:.2f}",
                  why, p.hook_presents, double(p.hook_delay_sum_us) / 1000.0 / double(p.hook_presents),
                  double(p.hook_delay_max_us) / 1000.0);
    }
    if (p.captures_skipped || p.frames_held || p.holds_cancelled)
    {
      // Not "pdz_native: SUMMARY": run_all_missions.ps1 parses the last line with that text.
      REXLOG_INFO("pdz_native: native output and plugin captures ({}): auto-diff ticks skipped {} | native refreshes "
                  "held for a pending capture {} | captures dropped unserved {}",
                  why, p.captures_skipped, p.frames_held, p.holds_cancelled);
    }
    LogDrawStats(why);
  }
} // namespace pdz::native

// D3DDevice_SetGammaRamp(device, flags, pRamp): r3 device, r4 flags, r5 ramp (memcpy of 1536 bytes =
// 3 x 256 x u16, big-endian). Not hooked by shadow_hooks.cpp. Always recorded (cold) so a later switch-on
// has the ramp; forwards unchanged.
REX_HOOK_RAW(sub_826F14A8)
{
  const uint32_t ramp = ctx.r5.u32;
  if (pdz::native::IsGuestPointer(ramp))
  {
    if (pdz::native::recthread::Active())
    {
      pdz::native::recthread::OnGammaRamp(base, ramp); // pdz_native_record_thread: the ramp copied, applied in order
    }
    else
    {
      pdz::native::ApplyGammaRamp(base, ramp);
    }
  }
  __imp__sub_826F14A8(ctx, base);
}
