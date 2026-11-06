// Native renderer (stage 1): the GPU plugin's swap post effect on the native output chain (native_postfx.cpp).
//
// The plugin's IssueSwap (rexglue-sdk src/graphics/d3d12/command_processor.cpp) applies swap_post_effect after its
// gamma ramp and before the presenter gets the image: FXAA (fxaa, fxaa_extreme) or SMAA (smaa), compute passes at the
// guest output size writing the presenter's guest output. The presenter's paint effects (present_effect: bilinear,
// CAS, FSR; present_dither) act later, on whatever guest output was refreshed, native or emulated, and the same-frame
// capture (CaptureGuestOutput) is taken before them, so only the swap post effect needs a native counterpart.
//
// RecordChain runs that chain natively with the SDK's own compiled shaders (included from the SDK's source tree), the
// same root-signature bindings, static samplers, intermediate formats and SMAA lookup textures, after the native gamma
// pass (native_passes.cpp: its output equals the plugin's gamma output, R10G10B10A2 c10 for c10). Used by the present
// path (pdz_native_post_effect) and by the auto-diff (pdz_native_autodiff_post_effect after / before).
#pragma once

#include <cstdint>
#include <d3d12.h>

#include <rex/cvar.h>

#include "native_device.h"

REXCVAR_DECLARE(bool, pdz_native_post_effect);

namespace pdz::native::postfx
{
  // The SDK's CommandProcessor::SwapPostEffect.
  enum class Effect : uint32_t
  {
    kNone,
    kFxaa,
    kFxaaExtreme,
    kSmaa
  };
  // The swap_post_effect spelling: none, fxaa, fxaa_extreme, smaa.
  const char *Name(Effect effect);

  // The effect the GPU plugin applies to the emulated output: swap_post_effect, parsed like the SDK's
  // ParseSwapPostEffect. The cvar lives in the plugin (read through the cvar registry by name) and the SDK latches it
  // once when the graphics system starts (kRequiresRestart), so it is latched here at the first call too.
  Effect PluginEffect();
  // The effect the native output applies when it is shown: PluginEffect() with pdz_native_post_effect on, else kNone.
  Effect PresentEffect();

  struct ChainDesc
  {
    // Destination: R10G10B10A2_UNORM (D3D12Presenter::kGuestOutputFormat), UAV-capable, already in
    // UNORDERED_ACCESS: the presenter's guest output or the auto-diff's own texture. The chain leaves it there.
    ID3D12Resource *output = nullptr;
    uint32_t width = 0, height = 0;
    // The front buffer's host texture (staging SRV with the fetch swizzle applied, PIXEL|NON_PIXEL shader resource,
    // as Textures::Lookup leaves it) and the 256x1 gamma LUT (native_present.cpp).
    D3D12_CPU_DESCRIPTOR_HANDLE src_srv{};
    D3D12_CPU_DESCRIPTOR_HANDLE lut_srv{};
    Effect effect = Effect::kNone;
  };
  // Records the output chain on the device's open command list: the gamma pass, then `effect` as the plugin runs it.
  // With kNone it is exactly the gamma pass into `output` (the native output as it was before
  // pdz_native_post_effect). Returns false when nothing usable was written (passes::ApplyGamma failed). `applied` is
  // the effect actually recorded: kNone when the effect's pipelines, textures or descriptors could not be had (logged
  // once per effect; the gamma pass alone is written to `output` then).
  bool RecordChain(NativeDevice &dev, const ChainDesc &desc, Effect &applied);

  // Memory log: the effect PSOs and root signatures (created together at the first effect use).
  uint32_t PipelineCount();
  uint32_t RootSignatureCount();
} // namespace pdz::native::postfx
