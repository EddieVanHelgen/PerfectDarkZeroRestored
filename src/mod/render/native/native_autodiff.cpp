// Native renderer (stage 1): auto-diff. At each capture tick (every pdz_native_capture_s seconds, or at the exact
// swap numbers of pdz_native_capture_swaps) the native frame and the emulated frame of the SAME guest swap are
// written side by side with a per-pixel difference image and a score:
//   - native: our host copy of the swap's front buffer (the resolve mirror), read back on our command list, the
//     fetch swizzle and the game's gamma ramp applied on the CPU (what the gamma pass shows);
//   - emulated: the GPU plugin's guest output for that swap, captured by the SDK right after the plugin's own
//     refresh (Presenter::RequestGuestOutputCapture, SDK patch sdk-04), so both are one guest frame.
// Swaps are counted here from the first Swap call and by the plugin from its first IssueSwap; both see every
// guest swap, so the indices agree (logged with each result).
//
// Score: mean absolute error over RGB (0..255) and the share of pixels whose largest channel difference is over
// pdz_native_autodiff_threshold. One log line and one CSV row per capture; files <n>_swap<i>_{native,emulated,
// diff}.ppm in pdz_native_capture_dir (the diff image is |native - emulated| x 4), of every capture or of every
// Nth one (pdz_native_autodiff_images: dense series without 8 MB of images per capture).
//
// Capture points by swap (pdz_native_capture_swaps): the wall-clock ticks drift with each run's loading stalls, so
// two runs of one build and script capture different swaps (3420 vs 3427). A point P is requested at swap P - 1
// (Request asks for the next swap), so its CSV row says swap P exactly. Equal swap numbers are equal points of the
// boot, not of the level: Datacore's level began anywhere from swap 1,044 to 1,393 in the 2026-10-26 runs (the
// cutscene skip waits for the script's 2 s A grid; stage1\subagents\capswap.md).
//
// Resolution: the score is taken at the native frame's host size; the emulated frame is box-filtered to it when it
// is an integer multiple (scale 1 against the emulator's 2560x1440: 2x2 boxes). With pdz_native_scale = 2 both are
// 2560x1440 and are compared pixel for pixel (CSV, files and the first log line). A second log line and the summary
// add the same pair box-filtered to the guest front buffer size (both frames, so comparable with the scale-1 runs).
//
// Swap post effect (pdz_native_autodiff_post_effect): the plugin applies swap_post_effect (FXAA / SMAA) to the
// emulated frame inside its refresh, so the capture above is taken after it, while the native frame above has none.
//   none   = that pair, the scores before 2026-10-26 (the native gamma on the CPU truncates the 16-bit ramp: c10 >> 2).
//   after  = both after the effect: the native frame goes through the output chain on the GPU with the same effect
//            (native_postfx.cpp, the SDK's shaders: what the window shows with pdz_native_post_effect), and is read
//            back and rounded to 8 bits like the presenter's capture of the emulated frame.
//   before = both before it: the plugin refreshes the capture swap without its effect
//            (Presenter::SetGuestOutputCaptureBeforePostEffect, SDK patch sdk-09) and the native frame goes through
//            the output chain without it. Without the patch: after, logged (or run with --swap_post_effect=none).
// The CSV row records the mode and the effect each side got; the log line and the summary name them (not in none).
// The presenter's paint effects (present_effect: FSR / CAS) are never in either frame: the capture precedes them.
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/presenter.h>

#include "native.h"
#include "native_autodiff.h"
#include "native_device.h"
#include "native_draw.h"
#include "native_passdump.h"
#include "native_postfx.h"
#include "native_textures.h"

REXCVAR_DEFINE_BOOL(pdz_native_autodiff, false, "Render",
                    "Native renderer: at each pdz_native_capture_s tick (or pdz_native_capture_swaps point) write the "
                    "native and emulated frames of the same guest swap, a difference image and a score (log + CSV).");
REXCVAR_DEFINE_INT32(pdz_native_autodiff_images, 1, "Render",
                     "Native renderer: write the native / emulated / diff images (.ppm, about 8 MB per capture at "
                     "1280x720) of every Nth auto-diff capture: 1 = every capture, 0 = none, 10 = captures 1, 11, 21, "
                     "... The log line and the CSV row are written for every capture (both name the file stem "
                     "either way).")
    .range(0, 1000000);
REXCVAR_DEFINE_STRING(pdz_native_autodiff_csv, "", "Render",
                      "Native renderer: CSV file the auto-diff appends one row per capture to (empty = "
                      "<pdz_native_capture_dir>\\autodiff.csv).");
REXCVAR_DEFINE_INT32(pdz_native_frame_trace, 0, "Render",
                     "Native renderer: log every clear / resolve / draw of the frame of this auto-diff capture number "
                     "(1 = the first capture; 0 = off). Lines start with pdz_trace:.");
REXCVAR_DEFINE_INT32(pdz_native_autodiff_threshold, 16, "Render",
                     "Native renderer: a pixel counts as different when its largest channel difference exceeds this "
                     "(0..255).");
REXCVAR_DEFINE_STRING(pdz_native_autodiff_post_effect, "none", "Render",
                      "Native renderer auto-diff: where both frames are taken relative to the GPU plugin's swap post "
                      "effect (swap_post_effect: fxaa / fxaa_extreme / smaa). none = the emulated frame after it, the "
                      "native frame without one (gamma on the CPU): the scores before 2026-10-26. after = both after "
                      "it: the native frame through the output chain on the GPU with the same effect (the SDK's "
                      "shaders), read back like the presenter's capture. before = both before it: the plugin skips "
                      "its effect for the capture swap (needs the SDK's Presenter::SetGuestOutputCaptureBeforePostEffect, "
                      "patch sdk-09; else after is used, logged), the native output chain without it. With "
                      "swap_post_effect = none, after and before are the same pair.")
    .allowed({"none", "after", "before"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace pdz::native::autodiff
{
  namespace
  {
    // pdz_native_capture_swaps, parsed once (SwapPointsSet / DueSwapPoint). An item is a series first, first + step,
    // ... up to last: "N" is first = last = N with step 0, "A/S" runs to the end (last = kOpenEnd).
    constexpr uint64_t kOpenEnd = ~uint64_t(0);
    struct SwapSeries
    {
      uint64_t first = 0, last = 0, step = 0;
    };
    struct SwapPoints
    {
      bool parsed = false;
      bool started = false; // `next` computed (first DueSwapPoint call)
      std::vector<SwapSeries> series;
      uint64_t next = 0; // the next point not yet due (0 = none left)
    };
    SwapPoints g_points;

    // Decimal digits only, at most 18 (no overflow).
    bool ParseSwap(std::string_view text, uint64_t &out)
    {
      if (text.empty() || text.size() > 18)
      {
        return false;
      }
      uint64_t value = 0;
      for (const char c : text)
      {
        if (c < '0' || c > '9')
        {
          return false;
        }
        value = value * 10 + uint64_t(c - '0');
      }
      out = value;
      return true;
    }

    // Comma-separated "N", "A-B/S" or "A/S" items, spaces ignored; an item not understood is logged and left out.
    std::vector<SwapSeries> ParseSwapPoints(const std::string &text)
    {
      std::string clean;
      for (const char c : text)
      {
        if (c != ' ' && c != '\t')
        {
          clean += c;
        }
      }
      std::vector<SwapSeries> out;
      size_t pos = 0;
      while (pos <= clean.size())
      {
        size_t comma = clean.find(',', pos);
        if (comma == std::string::npos)
        {
          comma = clean.size();
        }
        const std::string_view item(clean.data() + pos, comma - pos);
        pos = comma + 1;
        if (item.empty())
        {
          continue;
        }
        SwapSeries s;
        const size_t slash = item.find('/');
        const bool stepped = slash != std::string_view::npos;
        const std::string_view range = item.substr(0, slash);
        const size_t dash = range.find('-');
        bool ok = !stepped || (ParseSwap(item.substr(slash + 1), s.step) && s.step > 0);
        if (dash == std::string_view::npos)
        {
          ok = ok && ParseSwap(range, s.first);
          s.last = stepped ? kOpenEnd : s.first;
        }
        else
        {
          // A-B needs a step: consecutive swaps cannot all be captured (one capture in flight at a time).
          ok = ok && stepped && ParseSwap(range.substr(0, dash), s.first) && ParseSwap(range.substr(dash + 1), s.last) &&
               s.last >= s.first;
        }
        if (ok)
        {
          out.push_back(s);
        }
        else
        {
          REXLOG_WARN("pdz_native: pdz_native_capture_swaps: item '{}' not understood (N, A-B/S or A/S); ignored",
                      item);
        }
      }
      return out;
    }

    // The smallest capture point >= s; 0 when none is left.
    uint64_t NextSwapPoint(uint64_t s)
    {
      uint64_t best = 0;
      for (const SwapSeries &r : g_points.series)
      {
        uint64_t p = r.first;
        if (s > r.first)
        {
          if (!r.step || s > r.last)
          {
            continue;
          }
          p = r.first + (s - r.first + r.step - 1) / r.step * r.step;
          if (p > r.last || p < r.first) // past the end (or wrapped)
          {
            continue;
          }
        }
        if (!best || p < best)
        {
          best = p;
        }
      }
      return best;
    }

    std::string DescribeSwapPoints()
    {
      std::string s;
      for (const SwapSeries &r : g_points.series)
      {
        s += s.empty() ? "" : ", ";
        if (!r.step)
        {
          s += fmt::format("{}", r.first);
        }
        else if (r.last == kOpenEnd)
        {
          s += fmt::format("{} then every {} to the end", r.first, r.step);
        }
        else
        {
          s += fmt::format("{}-{} every {} ({} points)", r.first, r.last, r.step, (r.last - r.first) / r.step + 1);
        }
      }
      return s;
    }

    // pdz_native_autodiff_images: whether WritePpm writes the images of the capture being scored (capture numbers
    // 1, N + 1, 2N + 1, ...); set by Score through ImagesForCapture.
    bool g_write_images = true;

    void ImagesForCapture(uint32_t capture)
    {
      const int32_t every = REXCVAR_GET(pdz_native_autodiff_images);
      g_write_images = every > 0 && (capture - 1) % uint32_t(every) == 0;
      static int32_t logged = 1;
      if (every != logged)
      {
        logged = every;
        REXLOG_INFO("pdz_native: auto-diff images {} (pdz_native_autodiff_images {}); the log line and the CSV row "
                    "of every capture still name the file stem",
                    every > 0 ? fmt::format("for captures 1, {}, {}, ... only", every + 1, 2 * every + 1)
                              : std::string("not written"),
                    every);
      }
    }

    struct Image
    {
      uint32_t width = 0, height = 0;
      std::vector<uint8_t> rgb; // 3 bytes per pixel, rows packed
    };
    // pdz_native_autodiff_post_effect.
    enum class Mode : uint32_t
    {
      kNone,  // the emulated frame after the plugin's swap post effect, the native frame without (CPU readback)
      kAfter, // both after it (the native output chain with the SDK's effect shaders)
      kBefore // both before it (the plugin skips its effect for the capture swap; the native chain without it)
    };
    const char *ModeName(Mode mode)
    {
      return mode == Mode::kAfter ? "after" : mode == Mode::kBefore ? "before" : "none";
    }
    struct State
    {
      uint64_t swap_count = 0;     // swaps seen (this module's index = the plugin's IssueSwap index)
      uint64_t want_swap = 0;      // native readback due at this swap (and the plugin capture requested for it)
      uint64_t requested_at = 0;   // swap_count when requested (timeout)
      bool have_native = false;
      bool native_complete = false;
      uint64_t native_swap = 0;
      Image native;
      uint32_t captures = 0;
      double mae_sum = 0.0, over_sum = 0.0;
      uint32_t scored = 0;
      ID3D12Resource *readback = nullptr;
      uint64_t readback_size = 0;
      // The front buffer's guest size at the native readback; when the native frame is an integer multiple of it
      // (pdz_native_scale > 1) the pair is also scored box-filtered to it (GuestFactor, Score).
      uint32_t guest_width = 0, guest_height = 0;
      double guest_mae_sum = 0.0, guest_over_sum = 0.0;
      uint32_t guest_scored = 0, guest_factor = 0;
      // pdz_native_autodiff_post_effect of the pending capture (latched at Request) and the swap post effect each side
      // got; the same three of the last scored capture for the summary.
      Mode mode = Mode::kNone;
      postfx::Effect emulated_effect = postfx::Effect::kNone;
      postfx::Effect native_effect = postfx::Effect::kNone;
      Mode scored_mode = Mode::kNone;
      postfx::Effect scored_emulated = postfx::Effect::kNone, scored_native = postfx::Effect::kNone;
      int logged_mode = -1; // the mode's info line is logged when it changes
      bool before_fallback_logged = false;
      // Modes after / before: the native output chain's target (R10G10B10A2_UNORM, the presenter's guest output
      // format; tracked state) for the readback.
      ID3D12Resource *chain_output = nullptr;
      D3D12_RESOURCE_STATES chain_state = D3D12_RESOURCE_STATE_COPY_SOURCE;
      uint64_t chain_bytes = 0;
      // The native frame was read in split screen (SplitActive at its swap): Score also logs the top and bottom halves
      // (the two players' views of a two-player match; stage1/subagents/split.md). Log only, the CSV is unchanged.
      bool native_split = false;
    };
    State g;

    // Presenter::SetGuestOutputCaptureBeforePostEffect (SDK patch sdk-09), detected at compile time so this file builds
    // against an SDK with or without it.
    template <typename P>
    constexpr bool kSdkCaptureBeforePostEffect = requires { P::SetGuestOutputCaptureBeforePostEffect(true); };

    template <typename P>
    void SetCaptureBeforePostEffect(bool before)
    {
      if constexpr (kSdkCaptureBeforePostEffect<P>)
      {
        P::SetGuestOutputCaptureBeforePostEffect(before);
      }
      else
      {
        (void)before;
      }
    }

    // pdz_native_autodiff_post_effect for the next capture. before needs the SDK patch unless the plugin applies no
    // effect at all; without it the pair falls back to after (both frames with the effect), logged once.
    Mode RequestedMode()
    {
      std::string value = REXCVAR_GET(pdz_native_autodiff_post_effect);
      std::transform(value.begin(), value.end(), value.begin(),
                     [](unsigned char c) { return char(std::tolower(c)); });
      Mode mode = value == "after" ? Mode::kAfter : value == "before" ? Mode::kBefore : Mode::kNone;
      if (mode == Mode::kBefore && postfx::PluginEffect() != postfx::Effect::kNone &&
          !kSdkCaptureBeforePostEffect<rex::ui::Presenter>)
      {
        if (!g.before_fallback_logged)
        {
          g.before_fallback_logged = true;
          REXLOG_WARN("pdz_native: auto-diff: pdz_native_autodiff_post_effect = before needs the SDK's "
                      "Presenter::SetGuestOutputCaptureBeforePostEffect (patch sdk-09), not in this build; both "
                      "frames are compared after the plugin's swap post effect {} instead (mode after). "
                      "--swap_post_effect=none gives the pair without any effect, no patch needed.",
                      postfx::Name(postfx::PluginEffect()));
        }
        mode = Mode::kAfter;
      }
      return mode;
    }

    // One info line whenever the mode changes: which frame is taken where.
    void LogMode(Mode mode)
    {
      if (g.logged_mode == int(mode))
      {
        return;
      }
      g.logged_mode = int(mode);
      const char *effect = postfx::Name(postfx::PluginEffect());
      switch (mode)
      {
      case Mode::kAfter:
        REXLOG_INFO("pdz_native: auto-diff post effect after: both frames after the GPU plugin's swap post effect {} "
                    "(the native frame through the output chain with the SDK's shaders, read back and rounded like the "
                    "presenter's capture)",
                    effect);
        break;
      case Mode::kBefore:
        REXLOG_INFO("pdz_native: auto-diff post effect before: both frames before the GPU plugin's swap post effect "
                    "{} (the plugin skips it for the capture swap; the native frame through the output chain without "
                    "it, read back and rounded like the presenter's capture)",
                    effect);
        break;
      default:
        REXLOG_INFO("pdz_native: auto-diff post effect none: the emulated frame after the GPU plugin's swap post "
                    "effect {}, the native frame without one, gamma applied on the CPU (pdz_native_autodiff_post_effect; "
                    "the scores before 2026-10-26)",
                    effect);
        break;
      }
    }

    bool EnsureReadback(NativeDevice &dev, uint64_t total)
    {
      if (!g.readback || g.readback_size < total)
      {
        if (g.readback)
        {
          dev.ReleaseLater(g.readback);
        }
        g.readback = dev.CreateBuffer(total, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST,
                                      D3D12_RESOURCE_FLAG_NONE, "pdz_native auto-diff readback");
        g.readback_size = g.readback ? total : 0;
      }
      return g.readback != nullptr;
    }

    bool WritePpm(const std::string &path, const Image &img)
    {
      if (!g_write_images)
      {
        return false; // pdz_native_autodiff_images: not this capture
      }
      FILE *f = std::fopen(path.c_str(), "wb");
      if (!f)
      {
        return false;
      }
      std::fprintf(f, "P6\n%u %u\n255\n", img.width, img.height);
      std::fwrite(img.rgb.data(), 1, img.rgb.size(), f);
      std::fclose(f);
      return true;
    }

    // Reads the front buffer's host texture back and applies the fetch swizzle + gamma ramp.
    bool ReadNative(NativeDevice &dev, const FetchDecoded &front, const uint16_t (*ramp)[256], Image &out)
    {
      TextureView view = Textures::Get().Lookup(front);
      if (!view.texture || !view.texture->resource)
      {
        return false;
      }
      HostTexture &t = *view.texture;
      D3D12_RESOURCE_DESC desc = t.resource->GetDesc();
      if (desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM && desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB &&
          desc.Format != DXGI_FORMAT_R10G10B10A2_UNORM)
      {
        static bool logged = false;
        if (!logged)
        {
          logged = true;
          REXLOG_WARN("pdz_native: auto-diff: front buffer host format {} not handled", int(desc.Format));
        }
        return false;
      }
      D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
      UINT rows = 0;
      UINT64 row_size = 0, total = 0;
      dev.device()->GetCopyableFootprints(&desc, 0, 1, 0, &fp, &rows, &row_size, &total);
      if (!total || !EnsureReadback(dev, total))
      {
        return false;
      }
      Textures::Get().Transition(t, D3D12_RESOURCE_STATE_COPY_SOURCE);
      D3D12_TEXTURE_COPY_LOCATION dst{};
      dst.pResource = g.readback;
      dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      dst.PlacedFootprint = fp;
      D3D12_TEXTURE_COPY_LOCATION src{};
      src.pResource = t.resource;
      src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      src.SubresourceIndex = 0;
      dev.list()->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
      dev.Flush(); // submit + wait: the frame's draws and the copy (cold: one per capture)
      Textures::Get().Transition(t, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      uint8_t *mapped = nullptr;
      D3D12_RANGE range{0, SIZE_T(total)};
      if (FAILED(g.readback->Map(0, &range, reinterpret_cast<void **>(&mapped))) || !mapped)
      {
        return false;
      }
      out.width = uint32_t(desc.Width);
      out.height = desc.Height;
      out.rgb.assign(size_t(out.width) * out.height * 3, 0);
      const bool is_1010102 = desc.Format == DXGI_FORMAT_R10G10B10A2_UNORM;
      for (uint32_t y = 0; y < out.height; ++y)
      {
        const uint8_t *row = mapped + fp.Offset + size_t(y) * fp.Footprint.RowPitch;
        for (uint32_t x = 0; x < out.width; ++x)
        {
          uint8_t c[4];
          if (is_1010102)
          {
            uint32_t v;
            std::memcpy(&v, row + 4 * x, 4);
            c[0] = uint8_t((v & 1023) >> 2);
            c[1] = uint8_t(((v >> 10) & 1023) >> 2);
            c[2] = uint8_t(((v >> 20) & 1023) >> 2);
            c[3] = uint8_t(((v >> 30) & 3) * 85);
          }
          else
          {
            std::memcpy(c, row + 4 * x, 4);
          }
          for (uint32_t i = 0; i < 3; ++i)
          {
            const uint32_t sel = (front.swizzle >> (3 * i)) & 7;
            const uint8_t v = sel < 4 ? c[sel] : (sel == 5 ? 255 : 0);
            out.rgb[(size_t(y) * out.width + x) * 3 + i] = uint8_t(ramp[i][v] >> 8);
          }
        }
      }
      D3D12_RANGE none{0, 0};
      g.readback->Unmap(0, &none);
      return true;
    }

    // Presenter::Packed10bpcRGBTo8bpcBytes (protected in the SDK): unorm10 -> unorm8 with the D3D rounding, as the
    // presenter's capture converts the emulated frame.
    uint8_t Unorm10To8(uint32_t v) { return uint8_t(float(v) * (255.0f / 1023.0f) + 0.5f); }

    // The output chain's target for modes after / before: the presenter's guest output format at the frame's size.
    bool EnsureChainOutput(NativeDevice &dev, uint32_t width, uint32_t height)
    {
      if (g.chain_output)
      {
        const D3D12_RESOURCE_DESC current = g.chain_output->GetDesc();
        if (current.Width == width && current.Height == height)
        {
          return true;
        }
        dev.ReleaseLater(g.chain_output);
        g.chain_output = nullptr;
        g.chain_bytes = 0;
      }
      D3D12_RESOURCE_DESC desc{};
      desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
      desc.Width = width;
      desc.Height = height;
      desc.DepthOrArraySize = 1;
      desc.MipLevels = 1;
      desc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
      desc.SampleDesc.Count = 1;
      desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
      desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
      g.chain_output = dev.CreateTexture(desc, D3D12_RESOURCE_STATE_COPY_SOURCE, nullptr,
                                         "pdz_native auto-diff output chain");
      g.chain_state = D3D12_RESOURCE_STATE_COPY_SOURCE;
      g.chain_bytes = g.chain_output ? uint64_t(width) * height * 4 : 0;
      return g.chain_output != nullptr;
    }

    // Modes after / before: the native output chain (the gamma pass + `effect`, native_postfx.cpp: the present path's
    // own passes) into the auto-diff's R10G10B10A2 texture, read back and rounded to 8 bits like the presenter's
    // capture of the emulated frame. The swizzle comes with the SRV and the gamma is applied on the GPU, so this is
    // the frame the window shows, quantised like the emulated one.
    // `raw` (the mark): the R10G10B10A2 texels are copied there (rows packed) and `out` gets only the size, so the
    // 8-bit conversion runs on the mark's writer thread instead of the render thread.
    bool ReadNativeChain(NativeDevice &dev, const FetchDecoded &front, LutSource lut, postfx::Effect effect, Image &out,
                         std::vector<uint32_t> *raw = nullptr)
    {
      TextureView view = Textures::Get().Lookup(front);
      if (!view.texture || !view.texture->resource || !lut)
      {
        return false;
      }
      const uint32_t width = view.texture->width, height = view.texture->height;
      const D3D12_CPU_DESCRIPTOR_HANDLE lut_srv = lut(dev);
      if (!lut_srv.ptr || !width || !height || !EnsureChainOutput(dev, width, height))
      {
        return false;
      }
      dev.Transition(g.chain_output, g.chain_state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      postfx::ChainDesc chain;
      chain.output = g.chain_output;
      chain.width = width;
      chain.height = height;
      chain.src_srv = view.srv;
      chain.lut_srv = lut_srv;
      chain.effect = effect;
      postfx::Effect applied = postfx::Effect::kNone;
      const bool recorded = postfx::RecordChain(dev, chain, applied);
      dev.Transition(g.chain_output, g.chain_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
      if (!recorded)
      {
        return false;
      }
      g.native_effect = applied;
      const D3D12_RESOURCE_DESC desc = g.chain_output->GetDesc();
      D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
      UINT rows = 0;
      UINT64 row_size = 0, total = 0;
      dev.device()->GetCopyableFootprints(&desc, 0, 1, 0, &fp, &rows, &row_size, &total);
      if (!total || !EnsureReadback(dev, total))
      {
        return false;
      }
      D3D12_TEXTURE_COPY_LOCATION dst{};
      dst.pResource = g.readback;
      dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      dst.PlacedFootprint = fp;
      D3D12_TEXTURE_COPY_LOCATION src{};
      src.pResource = g.chain_output;
      src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      src.SubresourceIndex = 0;
      dev.list()->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
      dev.Flush(); // submit + wait: the frame's draws, the output chain and the copy (cold: one per capture)
      uint8_t *mapped = nullptr;
      D3D12_RANGE range{0, SIZE_T(total)};
      if (FAILED(g.readback->Map(0, &range, reinterpret_cast<void **>(&mapped))) || !mapped)
      {
        return false;
      }
      out.width = width;
      out.height = height;
      if (raw)
      {
        raw->resize(size_t(width) * height);
        for (uint32_t y = 0; y < height; ++y)
        {
          std::memcpy(raw->data() + size_t(y) * width, mapped + fp.Offset + size_t(y) * fp.Footprint.RowPitch,
                      size_t(width) * 4);
        }
        D3D12_RANGE none{0, 0};
        g.readback->Unmap(0, &none);
        return true;
      }
      out.rgb.assign(size_t(width) * height * 3, 0);
      for (uint32_t y = 0; y < height; ++y)
      {
        const uint8_t *row = mapped + fp.Offset + size_t(y) * fp.Footprint.RowPitch;
        uint8_t *rgb = out.rgb.data() + size_t(y) * width * 3;
        for (uint32_t x = 0; x < width; ++x)
        {
          uint32_t v;
          std::memcpy(&v, row + 4 * x, 4);
          rgb[3 * x] = Unorm10To8(v & 0x3FF);
          rgb[3 * x + 1] = Unorm10To8((v >> 10) & 0x3FF);
          rgb[3 * x + 2] = Unorm10To8((v >> 20) & 0x3FF);
        }
      }
      D3D12_RANGE none{0, 0};
      g.readback->Unmap(0, &none);
      return true;
    }

    // Emulated RawImage (R8 G8 B8 X8, presenter size) -> RGB at the native size (box filter on integer ratios,
    // else nearest).
    Image Resample(const rex::ui::RawImage &raw, uint32_t width, uint32_t height)
    {
      Image out;
      out.width = width;
      out.height = height;
      out.rgb.assign(size_t(width) * height * 3, 0);
      const bool box = raw.width % width == 0 && raw.height % height == 0;
      const uint32_t kx = std::max(1u, raw.width / width), ky = std::max(1u, raw.height / height);
      for (uint32_t y = 0; y < height; ++y)
      {
        for (uint32_t x = 0; x < width; ++x)
        {
          for (uint32_t i = 0; i < 3; ++i)
          {
            uint32_t v = 0;
            if (box)
            {
              for (uint32_t sy = 0; sy < ky; ++sy)
              {
                for (uint32_t sx = 0; sx < kx; ++sx)
                {
                  v += raw.data[size_t(y * ky + sy) * raw.stride + size_t(x * kx + sx) * 4 + i];
                }
              }
              v /= kx * ky;
            }
            else
            {
              const uint32_t sx = uint32_t(uint64_t(x) * raw.width / width);
              const uint32_t sy = uint32_t(uint64_t(y) * raw.height / height);
              v = raw.data[size_t(sy) * raw.stride + size_t(sx) * 4 + i];
            }
            out.rgb[(size_t(y) * width + x) * 3 + i] = uint8_t(v);
          }
        }
      }
      return out;
    }

    // pdz_native_scale > 1: the integer factor between the native frame (host pixels) and the front buffer's guest
    // size, the same on both axes; 0 when the native frame is at the guest size (scale 1, or a front buffer served by
    // an upload rather than a resolve mirror) or not an exact multiple of it.
    uint32_t GuestFactor()
    {
      if (!g.guest_width || !g.guest_height || g.native.width <= g.guest_width || g.native.width % g.guest_width ||
          g.native.height % g.guest_height)
      {
        return 0;
      }
      const uint32_t k = g.native.width / g.guest_width;
      return g.native.height / g.guest_height == k ? k : 0;
    }

    // RGB image box-filtered by k x k (the same truncating average as Resample's box path for the emulated frame).
    Image BoxDown(const Image &src, uint32_t k)
    {
      Image out;
      out.width = src.width / k;
      out.height = src.height / k;
      out.rgb.assign(size_t(out.width) * out.height * 3, 0);
      for (uint32_t y = 0; y < out.height; ++y)
      {
        for (uint32_t x = 0; x < out.width; ++x)
        {
          for (uint32_t i = 0; i < 3; ++i)
          {
            uint32_t v = 0;
            for (uint32_t sy = 0; sy < k; ++sy)
            {
              for (uint32_t sx = 0; sx < k; ++sx)
              {
                v += src.rgb[(size_t(y * k + sy) * src.width + size_t(x * k + sx)) * 3 + i];
              }
            }
            out.rgb[(size_t(y) * out.width + x) * 3 + i] = uint8_t(v / (k * k));
          }
        }
      }
      return out;
    }

    // The guest-size score of a capture at pdz_native_scale > 1 (log line + summary; the CSV keeps the full-size one):
    // both frames box-filtered to the front buffer's guest size, so the numbers compare with the scale-1 runs.
    void ScoreAtGuestSize(uint32_t capture, uint64_t swap, const rex::ui::RawImage &raw, uint32_t k, int threshold)
    {
      const Image native = BoxDown(g.native, k);
      const Image emu = Resample(raw, native.width, native.height);
      uint64_t sum = 0, over = 0;
      const size_t pixels = size_t(native.width) * native.height;
      for (size_t p = 0; p < pixels; ++p)
      {
        int worst = 0;
        for (uint32_t i = 0; i < 3; ++i)
        {
          const int d = std::abs(int(native.rgb[p * 3 + i]) - int(emu.rgb[p * 3 + i]));
          sum += uint64_t(d);
          worst = std::max(worst, d);
        }
        if (worst > threshold)
        {
          ++over;
        }
      }
      const double mae = pixels ? double(sum) / double(pixels * 3) : 0.0;
      const double over_pct = pixels ? 100.0 * double(over) / double(pixels) : 0.0;
      g.guest_mae_sum += mae;
      g.guest_over_sum += over_pct;
      ++g.guest_scored;
      g.guest_factor = k;
      REXLOG_INFO("pdz_native: auto-diff #{} swap {} at the guest size {}x{} (native {}x{} and emulated {}x{} both "
                  "box-filtered, scale {}): MAE {:.2f}, {:.1f}% of pixels over {}",
                  capture, swap, native.width, native.height, g.native.width, g.native.height, raw.width, raw.height, k,
                  mae, over_pct, threshold);
    }

    void Score(uint64_t swap, const rex::ui::RawImage &raw)
    {
      ImagesForCapture(g.captures + 1); // pdz_native_autodiff_images: WritePpm writes this capture's files or not
      const Image emu = Resample(raw, g.native.width, g.native.height);
      Image diff;
      diff.width = g.native.width;
      diff.height = g.native.height;
      diff.rgb.assign(g.native.rgb.size(), 0);
      const int threshold = REXCVAR_GET(pdz_native_autodiff_threshold);
      uint64_t sum = 0, over = 0;
      const size_t pixels = size_t(diff.width) * diff.height;
      // Split screen: the same sums per half (rows above / from height / 2).
      const size_t half_start = size_t(diff.height / 2) * diff.width;
      uint64_t half_sum[2] = {}, half_over[2] = {};
      for (size_t p = 0; p < pixels; ++p)
      {
        int worst = 0;
        uint64_t pixel_sum = 0;
        for (uint32_t i = 0; i < 3; ++i)
        {
          const int d = std::abs(int(g.native.rgb[p * 3 + i]) - int(emu.rgb[p * 3 + i]));
          pixel_sum += uint64_t(d);
          worst = std::max(worst, d);
          diff.rgb[p * 3 + i] = uint8_t(std::min(255, d * 4));
        }
        sum += pixel_sum;
        half_sum[p >= half_start ? 1 : 0] += pixel_sum;
        if (worst > threshold)
        {
          ++over;
          ++half_over[p >= half_start ? 1 : 0];
        }
      }
      const double mae = pixels ? double(sum) / double(pixels * 3) : 0.0;
      const double over_pct = pixels ? 100.0 * double(over) / double(pixels) : 0.0;
      ++g.captures;
      g.mae_sum += mae;
      g.over_sum += over_pct;
      ++g.scored;
      g.scored_mode = g.mode;
      g.scored_emulated = g.emulated_effect;
      g.scored_native = g.native_effect;

      std::string dir = REXCVAR_GET(pdz_native_capture_dir);
      if (dir.empty()) dir = ".";
      std::error_code ec;
      std::filesystem::create_directories(dir, ec);
      const std::string stem = fmt::format("{}\\{:02}_swap{}", dir, g.captures, swap);
      WritePpm(stem + "_native.ppm", g.native);
      WritePpm(stem + "_emulated.ppm", emu);
      WritePpm(stem + "_diff.ppm", diff);
      // The swap post effect of each side (pdz_native_autodiff_post_effect); mode none keeps the line as it was.
      const std::string effects =
          g.mode == Mode::kNone ? std::string()
                                : fmt::format(", post effect {}: emulated {}, native {}", ModeName(g.mode),
                                              postfx::Name(g.emulated_effect), postfx::Name(g.native_effect));
      REXLOG_INFO("pdz_native: auto-diff #{} swap {}: MAE {:.2f}, {:.1f}% of pixels over {} ({}x{}, native frame {}{}); "
                  "{}_*.ppm",
                  g.captures, swap, mae, over_pct, threshold, diff.width, diff.height,
                  g.native_complete ? "complete" : "had a shader miss", effects, stem);
      if (g.native_split && half_start && half_start < pixels)
      {
        const size_t half_pixels[2] = {half_start, pixels - half_start};
        REXLOG_INFO("pdz_native: auto-diff #{} swap {} split screen halves: top (rows 0-{}) MAE {:.2f}, {:.1f}% over {} | "
                    "bottom (rows {}-{}) MAE {:.2f}, {:.1f}% over {}",
                    g.captures, swap, diff.height / 2 - 1, double(half_sum[0]) / double(half_pixels[0] * 3),
                    100.0 * double(half_over[0]) / double(half_pixels[0]), threshold, diff.height / 2, diff.height - 1,
                    double(half_sum[1]) / double(half_pixels[1] * 3),
                    100.0 * double(half_over[1]) / double(half_pixels[1]), threshold);
      }
      std::string csv = REXCVAR_GET(pdz_native_autodiff_csv);
      if (csv.empty()) csv = dir + "\\autodiff.csv";
      std::filesystem::create_directories(std::filesystem::path(csv).parent_path(), ec);
      const bool fresh = !std::filesystem::exists(csv);
      if (FILE *f = std::fopen(csv.c_str(), "ab"))
      {
        // The three post-effect columns are last: readers of a file begun with the older 11-column header (Import-Csv,
        // csv.DictReader) see the old columns unchanged.
        if (fresh)
        {
          std::fprintf(f, "time,process,capture,swap,width,height,mae,pct_over,threshold,native_complete,files,"
                          "post_effect_mode,emulated_post_effect,native_post_effect\n");
        }
        const std::time_t now = std::time(nullptr);
        char when[32];
        std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
        std::fprintf(f, "%s,%lu,%u,%llu,%u,%u,%.3f,%.3f,%d,%d,%s,%s,%s,%s\n", when, GetCurrentProcessId(), g.captures,
                     (unsigned long long)swap, diff.width, diff.height, mae, over_pct, threshold,
                     g.native_complete ? 1 : 0, stem.c_str(), ModeName(g.mode), postfx::Name(g.emulated_effect),
                     postfx::Name(g.native_effect));
        std::fclose(f);
      }
      // pdz_native_scale > 1 only (GuestFactor is 0 at scale 1): the same pair at the guest size, both box-filtered.
      if (const uint32_t k = GuestFactor())
      {
        ScoreAtGuestSize(g.captures, swap, raw, k, threshold);
      }
    }

    // PNG writer (mark-this-spot): 8-bit RGB, each row filtered by the usual minimum-sum heuristic (none / sub / up /
    // paeth), one zlib stream of a single fixed-Huffman deflate block with a hash-chain LZ77 (the build has no zlib).
    // Runs on the mark's writer thread, never on the render thread.
    class PngBits
    {
    public:
      explicit PngBits(std::vector<uint8_t> &out) : out_(out) {}
      // `count` bits of `value`, least significant first (deflate's order for headers and extra bits).
      void Bits(uint32_t value, uint32_t count)
      {
        acc_ |= uint64_t(value) << fill_;
        fill_ += count;
        while (fill_ >= 8)
        {
          out_.push_back(uint8_t(acc_));
          acc_ >>= 8;
          fill_ -= 8;
        }
      }
      // A Huffman code, most significant bit first.
      void Code(uint32_t code, uint32_t length)
      {
        uint32_t reversed = 0;
        for (uint32_t i = 0; i < length; ++i)
        {
          reversed |= ((code >> i) & 1u) << (length - 1 - i);
        }
        Bits(reversed, length);
      }
      void Flush()
      {
        if (fill_)
        {
          out_.push_back(uint8_t(acc_));
          acc_ = 0;
          fill_ = 0;
        }
      }

    private:
      std::vector<uint8_t> &out_;
      uint64_t acc_ = 0;
      uint32_t fill_ = 0;
    };

    // Fixed Huffman literal / length symbol (RFC 1951 3.2.6).
    void PngSymbol(PngBits &w, uint32_t symbol)
    {
      if (symbol < 144)
      {
        w.Code(0x30 + symbol, 8);
      }
      else if (symbol < 256)
      {
        w.Code(0x190 + symbol - 144, 9);
      }
      else if (symbol < 280)
      {
        w.Code(symbol - 256, 7);
      }
      else
      {
        w.Code(0xC0 + symbol - 280, 8);
      }
    }

    void PngMatch(PngBits &w, uint32_t length, uint32_t distance)
    {
      static constexpr uint16_t kLengthBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                                   31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
      static constexpr uint8_t kLengthExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                                   2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
      static constexpr uint16_t kDistBase[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,
                                                 33,  49,  65,  97,  129, 193,  257,  385,  513,  769,
                                                 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
      static constexpr uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                                 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
      uint32_t l = 0;
      while (l + 1 < 29 && kLengthBase[l + 1] <= length)
      {
        ++l;
      }
      PngSymbol(w, 257 + l);
      w.Bits(length - kLengthBase[l], kLengthExtra[l]);
      uint32_t d = 0;
      while (d + 1 < 30 && kDistBase[d + 1] <= distance)
      {
        ++d;
      }
      w.Code(d, 5);
      w.Bits(distance - kDistBase[d], kDistExtra[d]);
    }

    // zlib stream (RFC 1950) of `data`: one final fixed-Huffman block, LZ77 over a 32 KB window.
    void PngDeflate(const std::vector<uint8_t> &data, std::vector<uint8_t> &out)
    {
      constexpr uint32_t kWindow = 32768, kHashBits = 15, kMinMatch = 3, kMaxMatch = 258, kChain = 16;
      out.push_back(0x78); // CMF: deflate, 32 KB window
      out.push_back(0x01); // FLG: no dictionary, check bits ((0x78 << 8) | 0x01) % 31 == 0
      PngBits w(out);
      w.Bits(1, 1); // BFINAL
      w.Bits(1, 2); // BTYPE 01: fixed Huffman
      const size_t n = data.size();
      const uint8_t *p = data.data();
      std::vector<int32_t> head(size_t(1) << kHashBits, -1), prev(kWindow, -1);
      const auto hash = [p](size_t i) {
        const uint32_t v = (uint32_t(p[i]) << 16) | (uint32_t(p[i + 1]) << 8) | p[i + 2];
        return (v * 2654435761u) >> (32 - kHashBits);
      };
      const auto insert = [&](size_t i) {
        const uint32_t h = hash(i);
        prev[i & (kWindow - 1)] = head[h];
        head[h] = int32_t(i);
      };
      size_t i = 0;
      while (i < n)
      {
        uint32_t best_length = 0, best_distance = 0;
        if (i + kMinMatch <= n)
        {
          const uint32_t max_length = uint32_t(std::min<size_t>(kMaxMatch, n - i));
          int32_t candidate = head[hash(i)];
          for (uint32_t chain = 0; candidate >= 0 && chain < kChain && i - size_t(candidate) <= kWindow; ++chain)
          {
            const uint8_t *a = p + candidate, *b = p + i;
            uint32_t length = 0;
            while (length < max_length && a[length] == b[length])
            {
              ++length;
            }
            if (length > best_length)
            {
              best_length = length;
              best_distance = uint32_t(i - size_t(candidate));
              if (length == max_length)
              {
                break;
              }
            }
            candidate = prev[size_t(candidate) & (kWindow - 1)];
          }
          insert(i);
        }
        if (best_length >= kMinMatch)
        {
          PngMatch(w, best_length, best_distance);
          for (size_t k = i + 1; k < i + best_length; ++k)
          {
            if (k + kMinMatch <= n)
            {
              insert(k);
            }
          }
          i += best_length;
        }
        else
        {
          PngSymbol(w, p[i]);
          ++i;
        }
      }
      PngSymbol(w, 256); // end of block
      w.Flush();
      uint32_t s1 = 1, s2 = 0; // Adler-32, big-endian after the block
      for (size_t at = 0; at < n;)
      {
        const size_t end = std::min(n, at + 5552);
        for (; at < end; ++at)
        {
          s1 += p[at];
          s2 += s1;
        }
        s1 %= 65521;
        s2 %= 65521;
      }
      const uint32_t adler = (s2 << 16) | s1;
      for (int shift = 24; shift >= 0; shift -= 8)
      {
        out.push_back(uint8_t(adler >> shift));
      }
    }

    uint32_t PngCrc(const uint8_t *data, size_t size, uint32_t crc = 0)
    {
      static const std::array<uint32_t, 256> kTable = [] {
        std::array<uint32_t, 256> table{};
        for (uint32_t n = 0; n < 256; ++n)
        {
          uint32_t c = n;
          for (int k = 0; k < 8; ++k)
          {
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
          }
          table[n] = c;
        }
        return table;
      }();
      crc = ~crc;
      for (size_t i = 0; i < size; ++i)
      {
        crc = kTable[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
      }
      return ~crc;
    }

    void PngChunk(std::vector<uint8_t> &file, const char type[4], const std::vector<uint8_t> &body)
    {
      const uint32_t length = uint32_t(body.size());
      for (int shift = 24; shift >= 0; shift -= 8)
      {
        file.push_back(uint8_t(length >> shift));
      }
      const size_t start = file.size();
      file.insert(file.end(), type, type + 4);
      file.insert(file.end(), body.begin(), body.end());
      const uint32_t crc = PngCrc(file.data() + start, file.size() - start);
      for (int shift = 24; shift >= 0; shift -= 8)
      {
        file.push_back(uint8_t(crc >> shift));
      }
    }

    uint8_t Paeth(int a, int b, int c)
    {
      const int pa = std::abs(b - c), pb = std::abs(a - c), pc = std::abs(a + b - 2 * c);
      return uint8_t(pa <= pb && pa <= pc ? a : pb <= pc ? b : c);
    }

    // `rgb`: width x height x 3 bytes, rows packed.
    bool WritePng(const std::string &path, uint32_t width, uint32_t height, const std::vector<uint8_t> &rgb)
    {
      const size_t row_bytes = size_t(width) * 3;
      if (!width || !height || rgb.size() < row_bytes * height || (row_bytes + 1) * height > 0x7FFFFFFFu)
      {
        return false;
      }
      std::vector<uint8_t> filtered((row_bytes + 1) * height);
      std::vector<uint8_t> candidate[4];
      for (std::vector<uint8_t> &c : candidate)
      {
        c.resize(row_bytes);
      }
      static constexpr uint8_t kFilterType[4] = {0, 1, 2, 4}; // none, sub, up, paeth
      for (uint32_t y = 0; y < height; ++y)
      {
        const uint8_t *row = rgb.data() + size_t(y) * row_bytes;
        const uint8_t *above = y ? row - row_bytes : nullptr;
        uint64_t best_sum = ~uint64_t(0);
        uint32_t best = 0;
        for (uint32_t f = 0; f < 4; ++f)
        {
          uint8_t *out = candidate[f].data();
          uint64_t sum = 0;
          for (size_t x = 0; x < row_bytes; ++x)
          {
            const int a = x >= 3 ? row[x - 3] : 0, b = above ? above[x] : 0, c = (x >= 3 && above) ? above[x - 3] : 0;
            const int predicted = f == 0 ? 0 : f == 1 ? a : f == 2 ? b : Paeth(a, b, c);
            out[x] = uint8_t(row[x] - predicted);
            sum += uint64_t(std::abs(int(int8_t(out[x]))));
          }
          if (sum < best_sum)
          {
            best_sum = sum;
            best = f;
          }
        }
        uint8_t *dst = filtered.data() + size_t(y) * (row_bytes + 1);
        dst[0] = kFilterType[best];
        std::memcpy(dst + 1, candidate[best].data(), row_bytes);
      }
      std::vector<uint8_t> idat;
      idat.reserve(filtered.size() / 2);
      PngDeflate(filtered, idat);
      filtered.clear();
      filtered.shrink_to_fit();
      std::vector<uint8_t> file = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
      std::vector<uint8_t> ihdr;
      for (const uint32_t v : {width, height})
      {
        for (int shift = 24; shift >= 0; shift -= 8)
        {
          ihdr.push_back(uint8_t(v >> shift));
        }
      }
      ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0}); // 8-bit, RGB, deflate, adaptive filters, no interlace
      PngChunk(file, "IHDR", ihdr);
      PngChunk(file, "IDAT", idat);
      PngChunk(file, "IEND", {});
      FILE *f = std::fopen(path.c_str(), "wb");
      if (!f)
      {
        return false;
      }
      const bool ok = std::fwrite(file.data(), 1, file.size(), f) == file.size();
      return std::fclose(f) == 0 && ok;
    }

    // Mark-this-spot (MarkRequest, native_present.cpp): the capture in flight (render thread only) and what its writer
    // thread gets.
    struct MarkJob
    {
      MarkRequest request;
      // Native: the output chain's R10G10B10A2 texels (rows packed; converted on the writer thread), or RGB from the
      // CPU readback when the chain could not be recorded.
      uint32_t native_width = 0, native_height = 0;
      std::vector<uint32_t> native_raw;
      Image native_rgb;
      bool have_native = false;
      rex::ui::RawImage emulated;
      bool have_emulated = false;
      std::string native_note, emulated_note; // why a side is missing
    };
    struct MarkState
    {
      bool active = false;
      uint64_t swap = 0; // the swap captured
      bool want_native = false, native_tried = false;
      bool want_emulated = false;
      MarkJob job;
    };
    MarkState g_mark;

    // The writer thread: 8-bit RGB, PNG, then the mark's one log line with the files.
    void WriteMark(MarkJob job)
    {
      std::string files;
      const auto add = [&files](const std::string &text) { files += files.empty() ? text : ", " + text; };
      const auto write = [&add](const std::string &path, const Image &img) {
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
        add(WritePng(path, img.width, img.height, img.rgb) ? fmt::format("{} {}x{}", path, img.width, img.height)
                                                           : fmt::format("{} NOT WRITTEN", path));
      };
      if (job.have_native)
      {
        Image &img = job.native_rgb;
        if (!job.native_raw.empty())
        {
          img.width = job.native_width;
          img.height = job.native_height;
          img.rgb.resize(job.native_raw.size() * 3);
          for (size_t i = 0; i < job.native_raw.size(); ++i)
          {
            const uint32_t v = job.native_raw[i];
            img.rgb[3 * i] = Unorm10To8(v & 0x3FF);
            img.rgb[3 * i + 1] = Unorm10To8((v >> 10) & 0x3FF);
            img.rgb[3 * i + 2] = Unorm10To8((v >> 20) & 0x3FF);
          }
          job.native_raw.clear();
          job.native_raw.shrink_to_fit();
        }
        write(job.request.native_path, img);
        img.rgb.clear();
        img.rgb.shrink_to_fit();
      }
      else if (!job.native_note.empty())
      {
        add(job.native_note);
      }
      if (job.have_emulated)
      {
        const rex::ui::RawImage &raw = job.emulated; // R8 G8 B8 X8
        Image img;
        img.width = raw.width;
        img.height = raw.height;
        img.rgb.resize(size_t(raw.width) * raw.height * 3);
        for (uint32_t y = 0; y < raw.height; ++y)
        {
          const uint8_t *src = raw.data.data() + size_t(y) * raw.stride;
          uint8_t *dst = img.rgb.data() + size_t(y) * raw.width * 3;
          for (uint32_t x = 0; x < raw.width; ++x)
          {
            dst[3 * x] = src[4 * x];
            dst[3 * x + 1] = src[4 * x + 1];
            dst[3 * x + 2] = src[4 * x + 2];
          }
        }
        write(job.request.emulated_path, img);
      }
      else if (!job.emulated_note.empty())
      {
        add(job.emulated_note);
      }
      REXLOG_INFO("{}, files {}", job.request.line, files.empty() ? std::string("none") : files);
    }

    // Both sides in (or given up): off to the writer thread; the mark is free again.
    void MaybeFinishMark()
    {
      MarkState &m = g_mark;
      if (!m.active || (m.want_native && !m.native_tried) || (m.want_emulated && !m.job.have_emulated))
      {
        return;
      }
      m.active = false;
      std::thread(WriteMark, std::move(m.job)).detach();
      m.job = MarkJob{};
    }
  } // namespace

  // graphics_backend null: refused (no emulated frame to capture; the refusal is logged once by native_present.cpp).
  bool Enabled() { return REXCVAR_GET(pdz_native_autodiff) && !pdz::gpusync::NullBackend(); }
} // namespace pdz::native::autodiff

namespace pdz::native
{
  namespace
  {
    bool g_trace = false;
  }
  bool FrameTraceActive() { return g_trace; }
  void SetFrameTrace(bool active) { g_trace = active; }
  std::string TraceSurface(uint32_t guest, uint32_t width, uint32_t height, uint32_t format, uint32_t edram, bool depth)
  {
    if (!guest)
    {
      return "-";
    }
    return fmt::format("{:08X}({}x{} {}fmt {} edram {})", guest, width, height, depth ? "depth " : "", format, edram);
  }
} // namespace pdz::native

namespace pdz::native::autodiff
{

  void CountSwap() { ++g.swap_count; }

  bool SwapPointsSet()
  {
    if (!g_points.parsed)
    {
      g_points.parsed = true;
      const std::string &text = REXCVAR_GET(pdz_native_capture_swaps);
      if (!text.empty())
      {
        g_points.series = ParseSwapPoints(text);
        if (g_points.series.empty())
        {
          REXLOG_WARN("pdz_native: pdz_native_capture_swaps '{}' names no capture point", text);
        }
        else
        {
          const int32_t seconds = REXCVAR_GET(pdz_native_capture_s);
          REXLOG_INFO("pdz_native: capture points at guest swaps {} (pdz_native_capture_swaps){}",
                      DescribeSwapPoints(),
                      seconds > 0 ? fmt::format("; pdz_native_capture_s {} ignored", seconds) : std::string());
        }
      }
    }
    return !g_points.series.empty();
  }

  uint64_t DueSwapPoint()
  {
    if (!SwapPointsSet())
    {
      return 0;
    }
    SwapPoints &sp = g_points;
    const uint64_t upcoming = g.swap_count + 1; // the swap a Request made now captures
    if (!sp.started)
    {
      sp.started = true;
      sp.next = NextSwapPoint(1);
    }
    if (sp.next && sp.next < upcoming)
    {
      // Went by without a call: swap 1 (the first request is made at swap 1 for swap 2), or the native renderer
      // was off at those swaps.
      REXLOG_WARN("pdz_native: capture points from swap {} to swap {} went by without a native swap; skipped",
                  sp.next, g.swap_count);
      sp.next = NextSwapPoint(upcoming);
    }
    if (!sp.next || sp.next != upcoming)
    {
      return 0;
    }
    sp.next = NextSwapPoint(upcoming + 1);
    return upcoming;
  }

  // A mark in flight counts too (Request would skip).
  bool CaptureInFlight() { return g.want_swap != 0 || g_mark.active; }

  void Request()
  {
    if (g.want_swap || g_mark.active)
    {
      return; // one at a time (a mark of bind_mark_spot included)
    }
    g.want_swap = g.swap_count + 1;
    g.requested_at = g.swap_count;
    g.have_native = false;
    // Where both frames are taken relative to the plugin's swap post effect (pdz_native_autodiff_post_effect): the
    // flag goes to the SDK before the request, which publishes it to the plugin's IssueSwap.
    g.mode = RequestedMode();
    LogMode(g.mode);
    g.emulated_effect = g.mode == Mode::kBefore ? postfx::Effect::kNone : postfx::PluginEffect();
    g.native_effect = postfx::Effect::kNone;
    SetCaptureBeforePostEffect<rex::ui::Presenter>(g.mode == Mode::kBefore);
    rex::ui::Presenter::RequestGuestOutputCapture(g.want_swap);
    // The draws of the requested swap's frame come next: trace them when this is the chosen capture.
    if (REXCVAR_GET(pdz_native_frame_trace) == int32_t(g.captures + 1))
    {
      SetFrameTrace(true);
      REXLOG_INFO("pdz_trace: begin frame of swap {} (auto-diff capture #{})", g.want_swap, g.captures + 1);
    }
    passdump::MaybeBegin(g.want_swap, g.captures + 1);
  }

  void CancelRequest()
  {
    rex::ui::Presenter::RequestGuestOutputCapture(0);
    if (g.want_swap && FrameTraceActive())
    {
      SetFrameTrace(false);
    }
    g.want_swap = 0;
    MarkState &m = g_mark;
    if (m.active && m.want_emulated && !m.job.have_emulated)
    {
      // The native output held its refresh for the mark's plugin capture and gave up (native_present.cpp).
      m.want_emulated = false;
      m.job.emulated_note = fmt::format("emulated: swap {} not served by the GPU plugin (request dropped)", m.swap);
      MaybeFinishMark();
    }
  }

  void OnSwap(NativeDevice &dev, const FetchDecoded &front, bool front_valid, bool frame_complete,
              const uint16_t (*ramp)[256], LutSource lut)
  {
    if (!g.want_swap)
    {
      return;
    }
    if (g.swap_count == g.want_swap && FrameTraceActive())
    {
      ResolveTraceQueries(); // per-draw pipeline statistics / occlusion of the traced frame
      SetFrameTrace(false);
      REXLOG_INFO("pdz_trace: end frame of swap {}", g.swap_count);
    }
    if (g.swap_count == g.want_swap)
    {
      passdump::End();
    }
    if (g.swap_count == g.want_swap && !g.have_native)
    {
      g.native_complete = frame_complete;
      g.native_swap = g.swap_count;
      if (g.mode == Mode::kNone)
      {
        g.have_native = front_valid && ReadNative(dev, front, ramp, g.native); // as before 2026-10-26
      }
      else
      {
        // Like for like: the native output chain with the plugin's effect (after) or without it (before).
        const postfx::Effect effect = g.mode == Mode::kAfter ? postfx::PluginEffect() : postfx::Effect::kNone;
        g.have_native = front_valid && ReadNativeChain(dev, front, lut, effect, g.native);
      }
      g.guest_width = front_valid ? front.width : 0; // GuestFactor: the guest-size score at pdz_native_scale > 1
      g.guest_height = front_valid ? front.height : 0;
      g.native_split = SplitActive(); // Score: per-half lines in split screen
      if (!g.have_native)
      {
        REXLOG_WARN("pdz_native: auto-diff: native readback failed at swap {}", g.swap_count);
      }
    }
    uint64_t index = 0;
    rex::ui::RawImage raw;
    if (rex::ui::Presenter::TakeGuestOutputCapture(index, raw))
    {
      if (index == g.want_swap && g.have_native && raw.width && raw.height)
      {
        Score(index, raw);
      }
      else
      {
        REXLOG_WARN("pdz_native: auto-diff: plugin capture for swap {} ({}x{}) does not pair with native swap {} "
                    "(have native {})",
                    index, raw.width, raw.height, g.want_swap, g.have_native);
      }
      g.want_swap = 0;
      return;
    }
    if (g.swap_count > g.requested_at + 300)
    {
      REXLOG_WARN("pdz_native: auto-diff: no emulated capture for swap {} after 300 swaps; request dropped",
                  g.want_swap);
      rex::ui::Presenter::RequestGuestOutputCapture(0);
      g.want_swap = 0;
    }
  }

  std::string Summary()
  {
    if (!g.scored)
    {
      return "auto-diff: none";
    }
    std::string s = fmt::format("auto-diff: {} captures, mean MAE {:.2f}, mean {:.1f}% pixels over threshold",
                                g.scored, g.mae_sum / g.scored, g.over_sum / g.scored);
    if (g.guest_scored) // pdz_native_scale > 1 only: the same captures box-filtered to the guest size
    {
      s += fmt::format(" (at the guest size, both box-filtered by {}: {} captures, mean MAE {:.2f}, mean {:.1f}%)",
                       g.guest_factor, g.guest_scored, g.guest_mae_sum / g.guest_scored,
                       g.guest_over_sum / g.guest_scored);
    }
    if (g.scored_mode != Mode::kNone) // pdz_native_autodiff_post_effect of the last capture; none keeps the line
    {
      s += fmt::format(" (post effect {}: emulated {}, native {})", ModeName(g.scored_mode),
                       postfx::Name(g.scored_emulated), postfx::Name(g.scored_native));
    }
    return s;
  }

  uint64_t KeptBytes() { return (g.readback ? g.readback_size : 0) + g.native.rgb.capacity() + g.chain_bytes; }

  uint64_t SwapCount() { return g.swap_count; }

  bool MarkBusy()
  {
    return g_mark.active || g.want_swap != 0 || rex::ui::Presenter::RequestedGuestOutputCapture() != 0;
  }

  void StartMark(MarkRequest request)
  {
    MarkState &m = g_mark;
    m = MarkState{};
    m.active = true;
    m.swap = g.swap_count + 1;
    m.want_native = request.native;
    m.want_emulated = request.emulated;
    m.job.emulated_note = request.emulated_skipped;
    m.job.request = std::move(request);
    if (m.want_emulated)
    {
      // The frame as the window shows it: after the plugin's swap post effect.
      SetCaptureBeforePostEffect<rex::ui::Presenter>(false);
      rex::ui::Presenter::RequestGuestOutputCapture(m.swap);
    }
    MaybeFinishMark(); // neither side wanted: the line alone
  }

  void MarkOnSwap()
  {
    MarkState &m = g_mark;
    if (!m.active)
    {
      return;
    }
    if (m.want_emulated && !m.job.have_emulated)
    {
      uint64_t index = 0;
      rex::ui::RawImage raw;
      if (rex::ui::Presenter::TakeGuestOutputCapture(index, raw))
      {
        if (index == m.swap && raw.width && raw.height)
        {
          m.job.emulated = std::move(raw);
          m.job.have_emulated = true;
        }
        else if (index > m.swap || !raw.width || !raw.height)
        {
          rex::ui::Presenter::RequestGuestOutputCapture(0);
          m.want_emulated = false;
          m.job.emulated_note = fmt::format("emulated: the GPU plugin's capture was of swap {} ({}x{}), not {}", index,
                                            raw.width, raw.height, m.swap);
        }
        // else an older capture nobody took: dropped, the request stays
      }
      else if (g.swap_count > m.swap + 300)
      {
        rex::ui::Presenter::RequestGuestOutputCapture(0);
        m.want_emulated = false;
        m.job.emulated_note = fmt::format("emulated: no GPU plugin capture of swap {} after 300 swaps", m.swap);
      }
    }
    if (m.want_native && !m.native_tried && g.swap_count > m.swap)
    {
      // MarkReadNative never ran for it: the renderer was switched off or the frame was not open.
      m.native_tried = true;
      m.job.native_note = fmt::format("native: no native frame at swap {}", m.swap);
    }
    MaybeFinishMark();
  }

  bool MarkNativeDue()
  {
    const MarkState &m = g_mark;
    return m.active && m.want_native && !m.native_tried && g.swap_count == m.swap;
  }

  void MarkReadNative(NativeDevice &dev, const FetchDecoded &front, bool front_valid, const uint16_t (*ramp)[256],
                      LutSource lut)
  {
    if (!MarkNativeDue())
    {
      return;
    }
    MarkState &m = g_mark;
    MarkJob &job = m.job;
    m.native_tried = true;
    Image size;
    // What the window shows for native (the gamma pass and the native output's swap post effect, on the GPU); the CPU
    // readback (gamma on the CPU, no post effect) if the chain cannot be recorded.
    if (front_valid && ReadNativeChain(dev, front, lut, postfx::PresentEffect(), size, &job.native_raw))
    {
      job.native_width = size.width;
      job.native_height = size.height;
      job.have_native = true;
    }
    else if (front_valid && ReadNative(dev, front, ramp, job.native_rgb))
    {
      job.native_raw.clear();
      job.have_native = true;
    }
    else
    {
      job.native_note = fmt::format("native: swap {} not read back{}", m.swap,
                                    front_valid ? std::string() : std::string(" (no front buffer)"));
    }
    MaybeFinishMark();
  }
} // namespace pdz::native::autodiff
