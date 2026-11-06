// Native renderer (stage 1): the fixed passes (native_passes.h). One root signature for both passes:
//   [0] 32-bit root constants b0 (12 dwords)   [1] SRV table t0..t1   [2] UAV table u0
//   static sampler s0 (point, clamp; unused by the texel-load paths but kept for later filtered blits).
// The 20e4 conversion mirrors the SDK's pixel_formats.xesli (XeFloat32To20e4 / XeFloat20e4To32, BSD-3),
// truncating like the default depth_float24_round = false path.
#include "native_passes.h"

#include <cstring>
#include <unordered_map>

#include <rex/logging.h>

namespace pdz::native::passes
{
  namespace
  {
    const char kShaderSource[] = R"HLSL(
cbuffer Constants : register(b0)
{
  int2 src_origin;
  int2 dst_origin;
  uint mode;
  uint flags;      // bit 0: swap r/b, bit 1: source is a 7e3 (2_10_10_10_FLOAT) target, bit 2: 16-bit fixed point,
                   // bit 3: round the 7e3 colour to nearest even 7e3 (pdz_native_7e3_blend 2 / _7e3_resolve_round)
                   // cs_gamma: bit 0 = the FXAA source (GammaDesc::fxaa_luma)
  float exp_scale;
  uint out_width;
  uint out_height;
  uint pad0, pad1, pad2;
};
Texture2D<float4> t0 : register(t0);
Texture2D<float4> t1 : register(t1);
RWTexture2D<unorm float4> u0 : register(u0);

float4 vs_main(uint id : SV_VertexID) : SV_Position
{
  float2 p = float2((id << 1) & 2, id & 2);
  return float4(p * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

uint Float32To20e4(uint f32u32)
{
  f32u32 = min((f32u32 <= 0x7FFFFFFFu) ? f32u32 : 0u, 0x3FFFFFF8u);
  uint denormalized = ((f32u32 & 0x7FFFFFu) | 0x800000u) >> min(113u - (f32u32 >> 23u), 24u);
  uint f24u32 = (f32u32 < 0x38800000u) ? denormalized : (f32u32 + 0xC8000000u);
  return (f24u32 >> 3u) & 0xFFFFFFu;
}

uint Float20e4To32(uint f24u32)
{
  uint mantissa = f24u32 & 0xFFFFFu;
  uint exponent = f24u32 >> 20u;
  bool is_denormalized = exponent == 0u;
  uint mantissa_lzcnt = 20u - firstbithigh(mantissa);
  exponent = is_denormalized ? (1u - mantissa_lzcnt) : exponent;
  mantissa = is_denormalized ? ((mantissa << mantissa_lzcnt) & 0xFFFFFu) : mantissa;
  return (f24u32 != 0u) ? (((exponent + 112u) << 23u) | (mantissa << 3u)) : 0u;
}

// The SDK's PreClampedFloat32To7e3 (round to nearest even) then Float7e3To32: f in [0, 31.875] -> the 7e3 value.
float Round7e3(float f)
{
  uint u = asuint(f);
  uint biased = (u < 0x3E800000u) ? (((u & 0x7FFFFFu) | 0x800000u) >> min(125u - (u >> 23u), 24u))
                                  : (u + 0xC2000000u);
  biased += 0x7FFFu + ((biased >> 16u) & 1u);
  uint f10 = (biased >> 16u) & 0x3FFu;
  uint e = (f10 >> 7u) & 7u;
  uint m = f10 & 0x7Fu;
  return (e != 0u) ? asfloat(((e + 124u) << 23u) | (m << 16u)) : float(m) * (1.0 / 512.0);
}

float4 ps_main(float4 pos : SV_Position) : SV_Target
{
  int2 st = int2(pos.xy) - dst_origin + src_origin;
  float4 c = t0.Load(int3(st, 0));
  if (mode == 1u)
  {
    c = asfloat(Float20e4To32(Float32To20e4(asuint(c.r)))).xxxx;
  }
  else if (mode == 2u)
  {
    c = (floor(saturate(c.r) * 16777215.0 + 0.5) / 16777215.0).xxxx;
  }
  else if (mode == 3u)
  {
    uint n24 = Float32To20e4(asuint(c.r));
    c = (float(n24 + (n24 >> 23u)) * (1.0 / 16777216.0)).xxxx; // SDK XeUNorm24To32
  }
  else if (mode == 4u)
  {
    uint n24 = uint(floor(saturate(c.r) * 16777215.0 + 0.5));
    c = asfloat(Float20e4To32(n24)).xxxx;
  }
  else
  {
    if (flags & 2u)
    {
      // A 7e3 render target holds 0..31.875 per colour channel and a 2-bit alpha; the host float16 copy does not
      // clamp, so the stored range is applied when it is read (the resolve), before the exponent bias.
      c.rgb = clamp(c.rgb, 0.0, 31.875);
      c.a = round(saturate(c.a) * 3.0) / 3.0;
      if (flags & 8u)
      {
        // ... and every write is rounded to 7e3 (pdz_native_7e3_blend 2: the blended draws store rounded values, the
        // others are rounded here, as the EDRAM would hold them; pdz_native_7e3_resolve_round: rounded here only).
        c.rgb = float3(Round7e3(c.r), Round7e3(c.g), Round7e3(c.b));
      }
    }
    if (flags & 4u)
    {
      // k_16_16 / k_16_16_16_16: 16-bit fixed point, -32..32 in the EDRAM (every write is clamped to it, the SDK's ROV
      // path too); the host float16 copy does not clamp (the light buffer's specular alpha reaches 300+), so the range
      // is applied when it is read. NaN reads as 0 (D3D SNORM rule). For the light buffer (ONE/ZERO and ONE/ONE of
      // non-negative terms) this ends at the value per-write clamping gives.
      const uint4 bits = asuint(c);
      c = ((bits & 0x7FFFFFFFu) > 0x7F800000u) ? 0.0 : c;
      c = clamp(c, -32.0, 32.0);
    }
    c.rgb *= exp_scale;
    if (flags & 1u)
    {
      c = c.bgra;
    }
  }
  return c;
}

[numthreads(8, 8, 1)]
void cs_gamma(uint3 id : SV_DispatchThreadID)
{
  if (id.x >= out_width || id.y >= out_height)
  {
    return;
  }
  float4 c = t0.Load(int3(int2(id.xy), 0));
  float3 g;
  g.r = t1.Load(int3(int(saturate(c.r) * 255.0 + 0.5), 0, 0)).r;
  g.g = t1.Load(int3(int(saturate(c.g) * 255.0 + 0.5), 0, 0)).g;
  g.b = t1.Load(int3(int(saturate(c.b) * 255.0 + 0.5), 0, 0)).b;
  float4 o = float4(g, 1.0);
  if (flags & 1u)
  {
    // The FXAA source (the SDK's apply_gamma_table_fxaa_luma, stored to R16G16B16A16_UNORM): the table's 10-bit value
    // (the LUT holds c10 << 6 | c10 >> 4) and the perceptual luma in alpha.
    o.rgb = float3(uint3(g * 65535.0 + 0.5) >> 6) / 1023.0;
    o.a = dot(o.rgb, float3(0.299, 0.587, 0.114));
  }
  u0[id.xy] = o;
}
)HLSL";

    // BlitMode::kDepthStencilAs8888 (pdz_native_depth_stencil_alias), its own source so that the shared one above keeps
    // its float t1 (the gamma LUT): the EDRAM depth-stencil word (depth24 << 8 | stencil) of a depth resolve, its bytes
    // 0..3 written to R, G, B, A, i.e. what a k_8_8_8_8 fetch of the resolved memory returns as components 0..3 (the
    // stencil in component 0: the .z of PDZ's A8R8G8B8 alias, stage2/polish/rcp90.md). t0 = the depth plane (float),
    // t1 = the stencil plane (uint, in G); flags bit 4 = the depth is 20e4 (D24FS8), else unorm24 (D24S8).
    const char kPackSource[] = R"HLSL(
cbuffer Constants : register(b0)
{
  int2 src_origin;
  int2 dst_origin;
  uint mode;
  uint flags;
  float exp_scale;
  uint out_width;
  uint out_height;
  uint pad0, pad1, pad2;
};
Texture2D<float4> t0 : register(t0);
Texture2D<uint2> t1 : register(t1);

uint Float32To20e4(uint f32u32)
{
  f32u32 = min((f32u32 <= 0x7FFFFFFFu) ? f32u32 : 0u, 0x3FFFFFF8u);
  uint denormalized = ((f32u32 & 0x7FFFFFu) | 0x800000u) >> min(113u - (f32u32 >> 23u), 24u);
  uint f24u32 = (f32u32 < 0x38800000u) ? denormalized : (f32u32 + 0xC8000000u);
  return (f24u32 >> 3u) & 0xFFFFFFu;
}

float4 ps_pack(float4 pos : SV_Position) : SV_Target
{
  int2 st = int2(pos.xy) - dst_origin + src_origin;
  float d = t0.Load(int3(st, 0)).r;
  uint s = t1.Load(int3(st, 0)).g & 0xFFu;
  uint n24 = (flags & 16u) ? Float32To20e4(asuint(d)) : uint(floor(saturate(d) * 16777215.0 + 0.5));
  uint w = (n24 << 8u) | s;
  return float4(float(w & 0xFFu), float((w >> 8u) & 0xFFu), float((w >> 16u) & 0xFFu), float(w >> 24u)) *
         (1.0 / 255.0);
}
)HLSL";

    struct Constants
    {
      int32_t src_origin[2];
      int32_t dst_origin[2];
      uint32_t mode;
      uint32_t flags;
      float exp_scale;
      uint32_t out_width;
      uint32_t out_height;
      uint32_t pad[3];
    };
    static_assert(sizeof(Constants) == 12 * 4);

    struct State
    {
      bool init_tried = false;
      bool ok = false;
      ID3D12RootSignature *root = nullptr;
      ID3DBlob *vs = nullptr;
      ID3DBlob *ps = nullptr;
      ID3D12PipelineState *gamma_pso = nullptr;
      std::unordered_map<uint32_t, ID3D12PipelineState *> blit_psos; // by DXGI_FORMAT
      // kDepthStencilAs8888: built at its first use (R8G8B8A8_UNORM destinations only); a failure is logged once and
      // the pass is not recorded again.
      bool pack_tried = false;
      ID3D12PipelineState *pack_pso = nullptr;
    };
    State g_state;

    ID3DBlob *CompileShader(NativeDevice &dev, const char *entry, const char *target,
                            const char *source = kShaderSource, size_t source_size = sizeof(kShaderSource) - 1)
    {
      ID3DBlob *code = nullptr;
      ID3DBlob *errors = nullptr;
      const HRESULT hr = dev.d3d_compile()(source, source_size, "pdz_native_passes", nullptr, nullptr, entry, target,
                                           0, 0, &code, &errors);
      if (FAILED(hr))
      {
        REXLOG_ERROR("pdz_native: D3DCompile({}, {}) failed: 0x{:08X} {}", entry, target, uint32_t(hr),
                     errors ? static_cast<const char *>(errors->GetBufferPointer()) : "");
        if (errors)
        {
          errors->Release();
        }
        return nullptr;
      }
      if (errors)
      {
        errors->Release();
      }
      return code;
    }

    bool Init(NativeDevice &dev)
    {
      State &s = g_state;
      if (s.init_tried)
      {
        return s.ok;
      }
      s.init_tried = true;

      // Root signature.
      D3D12_DESCRIPTOR_RANGE srv_range{};
      srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
      srv_range.NumDescriptors = 2;
      srv_range.BaseShaderRegister = 0;
      srv_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
      D3D12_DESCRIPTOR_RANGE uav_range{};
      uav_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
      uav_range.NumDescriptors = 1;
      uav_range.BaseShaderRegister = 0;
      uav_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
      D3D12_ROOT_PARAMETER params[3]{};
      params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
      params[0].Constants.ShaderRegister = 0;
      params[0].Constants.Num32BitValues = 12;
      params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
      params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
      params[1].DescriptorTable.NumDescriptorRanges = 1;
      params[1].DescriptorTable.pDescriptorRanges = &srv_range;
      params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
      params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
      params[2].DescriptorTable.NumDescriptorRanges = 1;
      params[2].DescriptorTable.pDescriptorRanges = &uav_range;
      params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
      D3D12_STATIC_SAMPLER_DESC sampler{};
      sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
      sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
      sampler.MaxLOD = D3D12_FLOAT32_MAX;
      sampler.ShaderRegister = 0;
      sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
      D3D12_ROOT_SIGNATURE_DESC rs{};
      rs.NumParameters = 3;
      rs.pParameters = params;
      rs.NumStaticSamplers = 1;
      rs.pStaticSamplers = &sampler;
      rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
      ID3DBlob *blob = nullptr;
      ID3DBlob *errors = nullptr;
      HRESULT hr = dev.serialize_root_signature()(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors);
      if (FAILED(hr))
      {
        REXLOG_ERROR("pdz_native: root signature serialisation failed: 0x{:08X} {}", uint32_t(hr),
                     errors ? static_cast<const char *>(errors->GetBufferPointer()) : "");
        if (errors)
        {
          errors->Release();
        }
        return false;
      }
      hr = dev.device()->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                             IID_PPV_ARGS(&s.root));
      blob->Release();
      if (FAILED(hr))
      {
        REXLOG_ERROR("pdz_native: CreateRootSignature failed: 0x{:08X}", uint32_t(hr));
        return false;
      }

      s.vs = CompileShader(dev, "vs_main", "vs_5_0");
      s.ps = CompileShader(dev, "ps_main", "ps_5_0");
      ID3DBlob *cs = CompileShader(dev, "cs_gamma", "cs_5_0");
      if (!s.vs || !s.ps || !cs)
      {
        return false;
      }
      D3D12_COMPUTE_PIPELINE_STATE_DESC cdesc{};
      cdesc.pRootSignature = s.root;
      cdesc.CS.pShaderBytecode = cs->GetBufferPointer();
      cdesc.CS.BytecodeLength = cs->GetBufferSize();
      hr = dev.device()->CreateComputePipelineState(&cdesc, IID_PPV_ARGS(&s.gamma_pso));
      cs->Release();
      if (FAILED(hr))
      {
        REXLOG_ERROR("pdz_native: gamma compute PSO failed: 0x{:08X}", uint32_t(hr));
        return false;
      }
      s.ok = true;
      REXLOG_INFO("pdz_native: passes ready (blit / depth encode / gamma)");
      return true;
    }

    ID3D12PipelineState *BlitPso(NativeDevice &dev, DXGI_FORMAT rtv_format)
    {
      State &s = g_state;
      auto it = s.blit_psos.find(uint32_t(rtv_format));
      if (it != s.blit_psos.end())
      {
        return it->second;
      }
      D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
      d.pRootSignature = s.root;
      d.VS.pShaderBytecode = s.vs->GetBufferPointer();
      d.VS.BytecodeLength = s.vs->GetBufferSize();
      d.PS.pShaderBytecode = s.ps->GetBufferPointer();
      d.PS.BytecodeLength = s.ps->GetBufferSize();
      d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
      d.SampleMask = UINT_MAX;
      d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
      d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
      d.RasterizerState.DepthClipEnable = TRUE;
      d.DepthStencilState.DepthEnable = FALSE;
      d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
      d.NumRenderTargets = 1;
      d.RTVFormats[0] = rtv_format;
      d.SampleDesc.Count = 1;
      ID3D12PipelineState *pso = nullptr;
      const HRESULT hr = dev.device()->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso));
      if (FAILED(hr))
      {
        REXLOG_ERROR("pdz_native: blit PSO for format {} failed: 0x{:08X}", uint32_t(rtv_format), uint32_t(hr));
        pso = nullptr;
      }
      s.blit_psos.emplace(uint32_t(rtv_format), pso); // null cached too: one failure log per format
      return pso;
    }

    // kDepthStencilAs8888: the shared root signature and vertex shader, ps_pack, an R8G8B8A8_UNORM target.
    ID3D12PipelineState *PackPso(NativeDevice &dev)
    {
      State &s = g_state;
      if (s.pack_tried)
      {
        return s.pack_pso;
      }
      s.pack_tried = true;
      ID3DBlob *ps = CompileShader(dev, "ps_pack", "ps_5_0", kPackSource, sizeof(kPackSource) - 1);
      if (!ps)
      {
        return nullptr; // logged by CompileShader
      }
      D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
      d.pRootSignature = s.root;
      d.VS.pShaderBytecode = s.vs->GetBufferPointer();
      d.VS.BytecodeLength = s.vs->GetBufferSize();
      d.PS.pShaderBytecode = ps->GetBufferPointer();
      d.PS.BytecodeLength = ps->GetBufferSize();
      d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
      d.SampleMask = UINT_MAX;
      d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
      d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
      d.RasterizerState.DepthClipEnable = TRUE;
      d.DepthStencilState.DepthEnable = FALSE;
      d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
      d.NumRenderTargets = 1;
      d.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
      d.SampleDesc.Count = 1;
      const HRESULT hr = dev.device()->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&s.pack_pso));
      ps->Release();
      if (FAILED(hr))
      {
        REXLOG_ERROR("pdz_native: depth-stencil pack PSO failed: 0x{:08X}", uint32_t(hr));
        s.pack_pso = nullptr;
      }
      return s.pack_pso;
    }
  } // namespace

  bool Blit(NativeDevice &dev, const BlitDesc &desc)
  {
    if (!Init(dev) || !desc.dst || !desc.width || !desc.height)
    {
      return false;
    }
    const bool pack = desc.mode == BlitMode::kDepthStencilAs8888;
    if (pack && (!desc.stencil_resource || desc.rtv_format != DXGI_FORMAT_R8G8B8A8_UNORM))
    {
      return false;
    }
    ID3D12PipelineState *pso = pack ? PackPso(dev) : BlitPso(dev, desc.rtv_format);
    if (!pso)
    {
      return false;
    }
    RecList *list = dev.list();
    Descriptor table = dev.AllocSrv(2);
    if (!table.cpu.ptr)
    {
      return false; // descriptor ring full (warned by the device)
    }
    dev.CopySrv(table.cpu, desc.src_srv);
    D3D12_CPU_DESCRIPTOR_HANDLE second = table.cpu;
    second.ptr += dev.srv_descriptor_size();
    if (pack)
    {
      // The stencil plane (plane slice 1 of the TYPELESS depth resource), created straight into the table's second
      // slot: the depth surfaces keep one staging SRV, of the depth plane.
      D3D12_SHADER_RESOURCE_VIEW_DESC stencil{};
      stencil.Format = desc.stencil_srv_format;
      stencil.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
      stencil.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
      stencil.Texture2D.MipLevels = 1;
      stencil.Texture2D.PlaneSlice = 1;
      dev.device()->CreateShaderResourceView(desc.stencil_resource, &stencil, second);
    }
    else
    {
      dev.CopySrv(second, desc.src_srv);
    }

    Constants c{};
    c.src_origin[0] = desc.src_x;
    c.src_origin[1] = desc.src_y;
    c.dst_origin[0] = desc.dst_x;
    c.dst_origin[1] = desc.dst_y;
    c.mode = uint32_t(desc.mode);
    c.flags = (desc.swap_rb ? 1u : 0u) | (desc.source_7e3 ? 2u : 0u) | (desc.source_fixed16 ? 4u : 0u) |
              (desc.source_7e3 && desc.round_7e3 ? 8u : 0u) | (pack && desc.depth_float24 ? 16u : 0u);
    c.exp_scale = desc.exp_scale;
    c.out_width = desc.width;
    c.out_height = desc.height;

    dev.ListStateChanged(); // the draw path re-binds its own root signature / PSO after this pass
    list->SetPipelineState(pso);
    list->SetGraphicsRootSignature(g_state.root);
    list->SetGraphicsRoot32BitConstants(0, 12, &c, 0);
    list->SetGraphicsRootDescriptorTable(1, table.gpu);
    list->OMSetRenderTargets(1, &desc.dst_rtv, FALSE, nullptr);
    D3D12_VIEWPORT vp{};
    vp.TopLeftX = float(desc.dst_x);
    vp.TopLeftY = float(desc.dst_y);
    vp.Width = float(desc.width);
    vp.Height = float(desc.height);
    vp.MaxDepth = 1.0f;
    D3D12_RECT scissor{desc.dst_x, desc.dst_y, desc.dst_x + LONG(desc.width), desc.dst_y + LONG(desc.height)};
    list->RSSetViewports(1, &vp);
    list->RSSetScissorRects(1, &scissor);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->DrawInstanced(3, 1, 0, 0);
    return true;
  }

  bool ApplyGamma(NativeDevice &dev, const GammaDesc &desc)
  {
    if (!Init(dev) || !desc.output || !desc.width || !desc.height)
    {
      return false;
    }
    RecList *list = dev.list();
    Descriptor srvs = dev.AllocSrv(2);
    Descriptor uav = dev.AllocSrv(1);
    if (!srvs.cpu.ptr || !uav.cpu.ptr)
    {
      return false;
    }
    dev.CopySrv(srvs.cpu, desc.src_srv);
    D3D12_CPU_DESCRIPTOR_HANDLE second = srvs.cpu;
    second.ptr += dev.srv_descriptor_size();
    dev.CopySrv(second, desc.lut_srv);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc{};
    uav_desc.Format = desc.format;
    uav_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    dev.device()->CreateUnorderedAccessView(desc.output, nullptr, &uav_desc, uav.cpu);

    Constants c{};
    c.out_width = desc.width;
    c.out_height = desc.height;
    c.exp_scale = 1.0f;
    c.flags = desc.fxaa_luma ? 1u : 0u; // cs_gamma: bit 0 = the FXAA source (10-bit values, luma in alpha)
    dev.ListStateChanged();
    list->SetPipelineState(g_state.gamma_pso);
    list->SetComputeRootSignature(g_state.root);
    list->SetComputeRoot32BitConstants(0, 12, &c, 0);
    list->SetComputeRootDescriptorTable(1, srvs.gpu);
    list->SetComputeRootDescriptorTable(2, uav.gpu);
    list->Dispatch((desc.width + 7) / 8, (desc.height + 7) / 8, 1);
    return true;
  }

  uint32_t PipelineCount()
  {
    return uint32_t(g_state.blit_psos.size()) + (g_state.gamma_pso ? 1u : 0u) + (g_state.pack_pso ? 1u : 0u);
  }
  uint32_t RootSignatureCount() { return g_state.root ? 1u : 0u; }
} // namespace pdz::native::passes
