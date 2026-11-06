// Native renderer (stage 1): the small fixed passes the core needs (native_passes.cpp): a full-screen
// blit / format-convert / depth-encode pass (resolves) and the gamma pass into the presenter's output
// texture (compute, the presenter's resource is UAV-only). Shaders are HLSL compiled at run time with
// D3DCompile (d3dcompiler_47.dll); PSOs are cached per render-target format.
#pragma once

#include <cstdint>
#include <d3d12.h>

#include "native_device.h"

namespace pdz::native::passes
{
  enum class BlitMode : uint32_t
  {
    kCopy = 0,        // colour: c.rgb *= exp_scale, optional R/B swap, written as-is into the destination format
    kDepth20e4 = 1,   // depth: float -> 20e4 (D24FS8 encoding) -> float, written to a R32_FLOAT destination
    kDepthUnorm24 = 2, // depth: float -> unorm24 (D24S8 encoding) -> float, written to a R32_FLOAT destination
    // Cross cases (the resolve copies the EDRAM bits; the destination's format decides how they read back):
    kDepth20e4AsUnorm24 = 3, // D24FS8 source, k_24_8 destination: the 20e4 bits read as unorm24 (SDK texture load)
    kDepthUnorm24As20e4 = 4, // D24S8 source, k_24_8_FLOAT destination: the unorm24 bits read as 20e4
    // Depth and stencil as the EDRAM word (depth24 << 8 | stencil), its bytes 0..3 written to R, G, B, A of a
    // R8G8B8A8_UNORM destination: a k_8_8_8_8 view of a depth resolve's memory (pdz_native_depth_stencil_alias; the
    // vision modes read the stencil in byte 0, stage2/polish/rcp90.md). Separate pixel shader; t1 = the stencil plane.
    kDepthStencilAs8888 = 5
  };

  struct BlitDesc
  {
    // Destination (the caller has transitioned it to RENDER_TARGET).
    ID3D12Resource *dst = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE dst_rtv{};
    DXGI_FORMAT rtv_format = DXGI_FORMAT_UNKNOWN;
    // Source (the caller has transitioned it to PIXEL_SHADER_RESOURCE); a staging SRV handle.
    D3D12_CPU_DESCRIPTOR_HANDLE src_srv{};
    // Host-pixel rectangles: the pass reads src (src_x + i, src_y + j) and writes dst (dst_x + i, dst_y + j)
    // for i < width, j < height (texel loads, no filtering; the integer resolution scale keeps both sides
    // the same size).
    int32_t src_x = 0, src_y = 0;
    int32_t dst_x = 0, dst_y = 0;
    uint32_t width = 0, height = 0;
    BlitMode mode = BlitMode::kCopy;
    bool swap_rb = false;   // RB_COPY_DEST_INFO.copy_dest_swap: write (b, g, r, a)
    float exp_scale = 1.0f; // 2^copy_dest_exp_bias
    bool source_7e3 = false; // source surface is k_2_10_10_10_FLOAT(_AS_16_16_16_16): clamp to its stored range
    // ... and round the colour to 7e3 (nearest even, the SDK's pack) after the clamp (pdz_native_7e3_blend 2 or
    // pdz_native_7e3_resolve_round).
    bool round_7e3 = false;
    // Source surface is k_16_16 / k_16_16_16_16 (16-bit fixed point, -32..32 in the EDRAM; the light buffer): clamp
    // to that range, NaN to 0, before the exponent bias.
    bool source_fixed16 = false;
    // kDepthStencilAs8888 only: the depth-stencil resource (TYPELESS; its stencil plane gets an SRV of this format,
    // X32_TYPELESS_G8X24_UINT or X24_TYPELESS_G8_UINT, written into the pass's table) and the depth encoding of the
    // EDRAM word (true = 20e4 / D24FS8, false = unorm24 / D24S8).
    ID3D12Resource *stencil_resource = nullptr;
    DXGI_FORMAT stencil_srv_format = DXGI_FORMAT_UNKNOWN;
    bool depth_float24 = false;
  };
  // Records the pass on the device's open command list. Returns false (logged once) when the shaders or the
  // PSO could not be created.
  bool Blit(NativeDevice &dev, const BlitDesc &desc);

  struct GammaDesc
  {
    // The presenter's guest output texture (R10G10B10A2_UNORM, UAV-capable) or a swap post-effect intermediate
    // (native_postfx.cpp); the caller transitions it to UNORDERED_ACCESS before and back after.
    ID3D12Resource *output = nullptr;
    // The UAV format of `output`: R10G10B10A2_UNORM (the guest output, the SMAA colour texture) or R16G16B16A16_UNORM
    // (the FXAA source).
    DXGI_FORMAT format = DXGI_FORMAT_R10G10B10A2_UNORM;
    // The FXAA source, as the SDK's apply_gamma_table_fxaa_luma: the table's 10-bit values in RGB and the perceptual
    // luma (0.299, 0.587, 0.114) in alpha. Off: RGB = the LUT texel, alpha 1 (the output before 2026-10-26).
    bool fxaa_luma = false;
    uint32_t width = 0, height = 0;
    // Source frame (PIXEL_SHADER_RESOURCE state), a staging SRV with the swizzle already applied, and the
    // 256x1 R16G16B16A16_UNORM gamma LUT (identity when the game never set a ramp).
    D3D12_CPU_DESCRIPTOR_HANDLE src_srv{};
    D3D12_CPU_DESCRIPTOR_HANDLE lut_srv{};
  };
  bool ApplyGamma(NativeDevice &dev, const GammaDesc &desc);

  // Memory log: the passes' PSOs (gamma + one blit PSO per destination format, failures included) and root signatures.
  uint32_t PipelineCount();
  uint32_t RootSignatureCount();
} // namespace pdz::native::passes
