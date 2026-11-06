// Native renderer (stage 1): the GPU plugin's swap post effect on the native output chain (native_postfx.h).
//
// What the plugin's IssueSwap records per swap_post_effect (rexglue-sdk src/graphics/d3d12/command_processor.cpp,
// shaders in src/graphics/shaders), and what RecordChain records for it:
//   none            gamma ramp -> guest output.
//   fxaa, fxaa_extreme
//                   gamma ramp + perceptual luma in alpha -> R16G16B16A16_UNORM source (apply_gamma_table_fxaa_luma);
//                   fxaa_cs / fxaa_extreme_cs: source -> guest output. Root: b0 = (size, 1 / size), t0, u0; static
//                   sampler MIN_MAG_MIP_LINEAR, clamp, LOD 0.
//   smaa            gamma ramp -> R10G10B10A2_UNORM colour; smaa_edge_detection_cs: colour -> R10G10B10A2_UNORM edges;
//                   smaa_blending_weight_calculation_cs: edges + AreaTex (R8G8 160x560) + SearchTex (R8 64x16) ->
//                   R16G16B16A16_FLOAT weights; smaa_neighborhood_blending_cs: colour + weights -> guest output. Root:
//                   b0, t0..t2, u0 (one table per view); static sampler MIN_MAG_LINEAR_MIP_POINT, clamp, LOD 0.
// Every effect pass dispatches 16x8 groups over the output size; the intermediates rest in NON_PIXEL_SHADER_RESOURCE.
// The native gamma pass stands in for the SDK's apply_gamma_table (same values, native_postfx.h); everything after it
// is the SDK's DXBC and lookup data, so a native frame equal to the emulated one gets the same post effect.
#include "native_postfx.h"

#include <cctype>
#include <cstring>
#include <string>

#include <rex/logging.h>

#include "native_passes.h"

REXCVAR_DEFINE_BOOL(pdz_native_post_effect, true, "Render",
                    "Native renderer: the native output (F9 / pdz_native_output) applies the GPU plugin's swap post "
                    "effect (swap_post_effect: fxaa, fxaa_extreme or smaa) after its gamma pass, with the SDK's own "
                    "compute shaders, as the plugin does for the emulated output. false = the gamma pass only (the "
                    "native output before 2026-10-26). The auto-diff has its own switch, "
                    "pdz_native_autodiff_post_effect.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace pdz::native::postfx
{
  namespace
  {
    // The SDK's swap post-effect compute shaders, D3D12 DXBC (shader model 5.1): what D3D12CommandProcessor creates its
    // FXAA / SMAA pipelines from. The game's include path has the repository root.
    namespace sdk_shaders
    {
#include "thirdparty/rexglue-sdk/src/graphics/shaders/bytecode/d3d12_5_1/fxaa_cs.h"
#include "thirdparty/rexglue-sdk/src/graphics/shaders/bytecode/d3d12_5_1/fxaa_extreme_cs.h"
#include "thirdparty/rexglue-sdk/src/graphics/shaders/bytecode/d3d12_5_1/smaa_blending_weight_calculation_cs.h"
#include "thirdparty/rexglue-sdk/src/graphics/shaders/bytecode/d3d12_5_1/smaa_edge_detection_cs.h"
#include "thirdparty/rexglue-sdk/src/graphics/shaders/bytecode/d3d12_5_1/smaa_neighborhood_blending_cs.h"
    } // namespace sdk_shaders

    // SMAA's precomputed lookup textures (the SMAA repository, MIT): the data the SDK uploads (its
    // src/graphics/util/smaa_textures.cpp includes the same two files).
    namespace smaa_lookup
    {
#include "thirdparty/rexglue-sdk/thirdparty/smaa/AreaTex.h"
#include "thirdparty/rexglue-sdk/thirdparty/smaa/SearchTex.h"
    } // namespace smaa_lookup

    // The SDK's SmaaConstants / FxaaConstants (root constants, b0).
    struct EffectConstants
    {
      uint32_t size[2];
      float size_inv[2];
    };
    static_assert(sizeof(EffectConstants) == 4 * sizeof(uint32_t));

    // The SDK's formats (D3D12CommandProcessor::kSmaaTextureFormats, kFxaaSourceTextureFormat,
    // D3D12Presenter::kGuestOutputFormat) and the state its intermediates rest in.
    constexpr DXGI_FORMAT kSmaaColorFormat = DXGI_FORMAT_R10G10B10A2_UNORM;
    constexpr DXGI_FORMAT kSmaaEdgesFormat = DXGI_FORMAT_R10G10B10A2_UNORM;
    constexpr DXGI_FORMAT kSmaaWeightsFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
    constexpr DXGI_FORMAT kFxaaSourceFormat = DXGI_FORMAT_R16G16B16A16_UNORM;
    constexpr DXGI_FORMAT kOutputFormat = DXGI_FORMAT_R10G10B10A2_UNORM;
    constexpr D3D12_RESOURCE_STATES kRestState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    struct Texture
    {
      ID3D12Resource *resource = nullptr;
      D3D12_RESOURCE_STATES state = kRestState;
    };

    struct State
    {
      // Effect pipelines, made together at the first effect use.
      bool init_tried = false;
      ID3D12RootSignature *smaa_root = nullptr;
      ID3D12RootSignature *fxaa_root = nullptr;
      ID3D12PipelineState *smaa[3] = {}; // edge detection, blending weight calculation, neighbourhood blending
      ID3D12PipelineState *fxaa = nullptr;
      ID3D12PipelineState *fxaa_extreme = nullptr;
      // Intermediates at the output size (the SDK's smaa_textures_ / fxaa_source_texture_), remade when it changes.
      Texture smaa_color, smaa_edges, smaa_weights, fxaa_source;
      // SMAA's lookup textures, uploaded once (one attempt).
      Texture area, search;
      bool lookup_tried = false;
      // Once-per-effect log lines, by Effect.
      bool unavailable_logged[4] = {};
      bool recorded_logged[4] = {};
    };
    State g_state;

    // The SDK's ParseSwapPostEffect (graphics_system.cpp): case-insensitive, '-' read as '_', anything else is none.
    Effect ParseEffect(std::string name)
    {
      for (char &c : name)
      {
        c = c == '-' ? '_' : char(std::tolower(static_cast<unsigned char>(c)));
      }
      if (name == "fxaa")
      {
        return Effect::kFxaa;
      }
      if (name == "fxaa_extreme" || name == "extreme")
      {
        return Effect::kFxaaExtreme;
      }
      if (name == "smaa")
      {
        return Effect::kSmaa;
      }
      return Effect::kNone;
    }

    // Root signature with the SDK's bindings: b0 = 4 root constants, `srv_count` SRV tables t0.. and one UAV table u0
    // (one descriptor per table, as the SDK's), one static sampler s0 (clamp, LOD 0) with `filter`.
    ID3D12RootSignature *MakeRoot(NativeDevice &dev, uint32_t srv_count, D3D12_FILTER filter, const char *what)
    {
      D3D12_DESCRIPTOR_RANGE ranges[4]{};
      D3D12_ROOT_PARAMETER params[5]{};
      params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
      params[0].Constants.ShaderRegister = 0;
      params[0].Constants.RegisterSpace = 0;
      params[0].Constants.Num32BitValues = sizeof(EffectConstants) / sizeof(uint32_t);
      params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
      for (uint32_t i = 0; i <= srv_count; ++i)
      {
        const bool uav = i == srv_count;
        D3D12_DESCRIPTOR_RANGE &range = ranges[i];
        range.RangeType = uav ? D3D12_DESCRIPTOR_RANGE_TYPE_UAV : D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = 1;
        range.BaseShaderRegister = uav ? 0 : i;
        range.RegisterSpace = 0;
        range.OffsetInDescriptorsFromTableStart = 0;
        D3D12_ROOT_PARAMETER &param = params[1 + i];
        param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        param.DescriptorTable.NumDescriptorRanges = 1;
        param.DescriptorTable.pDescriptorRanges = &range;
        param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
      }
      D3D12_STATIC_SAMPLER_DESC sampler{};
      sampler.Filter = filter;
      sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
      sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
      sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
      sampler.MipLODBias = 0.0f;
      sampler.MaxAnisotropy = 1;
      sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
      sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
      sampler.MinLOD = 0.0f;
      sampler.MaxLOD = 0.0f;
      sampler.ShaderRegister = 0;
      sampler.RegisterSpace = 0;
      sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
      D3D12_ROOT_SIGNATURE_DESC desc{};
      desc.NumParameters = 2 + srv_count;
      desc.pParameters = params;
      desc.NumStaticSamplers = 1;
      desc.pStaticSamplers = &sampler;
      desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
      ID3DBlob *blob = nullptr;
      ID3DBlob *errors = nullptr;
      HRESULT hr = dev.serialize_root_signature()(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors);
      if (FAILED(hr))
      {
        REXLOG_ERROR("pdz_native: {} root signature serialisation failed: 0x{:08X} {}", what, uint32_t(hr),
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
      ID3D12RootSignature *root = nullptr;
      hr = dev.device()->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root));
      blob->Release();
      if (FAILED(hr))
      {
        REXLOG_ERROR("pdz_native: {} CreateRootSignature failed: 0x{:08X}", what, uint32_t(hr));
        return nullptr;
      }
      return root;
    }

    ID3D12PipelineState *MakePso(NativeDevice &dev, ID3D12RootSignature *root, const void *code, size_t size,
                                 const char *what)
    {
      D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
      desc.pRootSignature = root;
      desc.CS.pShaderBytecode = code;
      desc.CS.BytecodeLength = size;
      ID3D12PipelineState *pso = nullptr;
      const HRESULT hr = dev.device()->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso));
      if (FAILED(hr))
      {
        REXLOG_ERROR("pdz_native: {} compute PSO (the SDK's shader) failed: 0x{:08X}", what, uint32_t(hr));
        return nullptr;
      }
      return pso;
    }

    void Init(NativeDevice &dev)
    {
      State &s = g_state;
      if (s.init_tried)
      {
        return;
      }
      s.init_tried = true;
      if (!dev.serialize_root_signature())
      {
        REXLOG_ERROR("pdz_native: swap post effect: D3D12SerializeRootSignature unavailable");
        return;
      }
      s.smaa_root = MakeRoot(dev, 3, D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT, "SMAA");
      s.fxaa_root = MakeRoot(dev, 1, D3D12_FILTER_MIN_MAG_MIP_LINEAR, "FXAA");
      if (s.smaa_root)
      {
        s.smaa[0] = MakePso(dev, s.smaa_root, sdk_shaders::smaa_edge_detection_cs,
                            sizeof(sdk_shaders::smaa_edge_detection_cs), "SMAA edge detection");
        s.smaa[1] = MakePso(dev, s.smaa_root, sdk_shaders::smaa_blending_weight_calculation_cs,
                            sizeof(sdk_shaders::smaa_blending_weight_calculation_cs), "SMAA blending weights");
        s.smaa[2] = MakePso(dev, s.smaa_root, sdk_shaders::smaa_neighborhood_blending_cs,
                            sizeof(sdk_shaders::smaa_neighborhood_blending_cs), "SMAA neighbourhood blending");
      }
      if (s.fxaa_root)
      {
        s.fxaa = MakePso(dev, s.fxaa_root, sdk_shaders::fxaa_cs, sizeof(sdk_shaders::fxaa_cs), "FXAA");
        s.fxaa_extreme = MakePso(dev, s.fxaa_root, sdk_shaders::fxaa_extreme_cs, sizeof(sdk_shaders::fxaa_extreme_cs),
                                 "FXAA extreme");
      }
      REXLOG_INFO("pdz_native: swap post effect pipelines from the SDK's shaders: smaa {}, fxaa {}, fxaa_extreme {}",
                  s.smaa[0] && s.smaa[1] && s.smaa[2] ? "ready" : "FAILED", s.fxaa ? "ready" : "FAILED",
                  s.fxaa_extreme ? "ready" : "FAILED");
    }

    // An intermediate of the output size (UAV-capable, resting in NON_PIXEL_SHADER_RESOURCE), remade on a size change.
    bool EnsureTexture(NativeDevice &dev, Texture &t, DXGI_FORMAT format, uint32_t width, uint32_t height,
                       const char *name)
    {
      if (t.resource)
      {
        const D3D12_RESOURCE_DESC current = t.resource->GetDesc();
        if (current.Width == width && current.Height == height)
        {
          return true;
        }
        dev.ReleaseLater(t.resource); // frames in flight may still read it
        t = Texture();
      }
      D3D12_RESOURCE_DESC desc{};
      desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
      desc.Width = width;
      desc.Height = height;
      desc.DepthOrArraySize = 1;
      desc.MipLevels = 1;
      desc.Format = format;
      desc.SampleDesc.Count = 1;
      desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
      desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
      t.resource = dev.CreateTexture(desc, kRestState, nullptr, name);
      t.state = kRestState;
      return t.resource != nullptr;
    }

    // One SMAA lookup texture: created, its upload recorded on the open list (the frame's upload memory, a texture
    // copy), left in NON_PIXEL_SHADER_RESOURCE like the SDK's.
    bool UploadLookup(NativeDevice &dev, Texture &t, DXGI_FORMAT format, uint32_t width, uint32_t height,
                      uint32_t pitch, const unsigned char *data, const char *name)
    {
      D3D12_RESOURCE_DESC desc{};
      desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
      desc.Width = width;
      desc.Height = height;
      desc.DepthOrArraySize = 1;
      desc.MipLevels = 1;
      desc.Format = format;
      desc.SampleDesc.Count = 1;
      desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
      desc.Flags = D3D12_RESOURCE_FLAG_NONE;
      D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
      UINT rows = 0;
      UINT64 row_size = 0, total = 0;
      dev.device()->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &rows, &row_size, &total);
      if (!total || rows < height || row_size < pitch)
      {
        return false;
      }
      const UploadSlice slice = dev.Upload(uint32_t(total), D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
      if (!slice.cpu)
      {
        return false;
      }
      t.resource = dev.CreateTexture(desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, name);
      if (!t.resource)
      {
        return false;
      }
      t.state = D3D12_RESOURCE_STATE_COPY_DEST;
      for (uint32_t y = 0; y < height; ++y)
      {
        std::memcpy(static_cast<uint8_t *>(slice.cpu) + size_t(y) * footprint.Footprint.RowPitch,
                    data + size_t(y) * pitch, pitch);
      }
      footprint.Offset = slice.offset;
      D3D12_TEXTURE_COPY_LOCATION dst{};
      dst.pResource = t.resource;
      dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      dst.SubresourceIndex = 0;
      D3D12_TEXTURE_COPY_LOCATION src{};
      src.pResource = slice.resource;
      src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      src.PlacedFootprint = footprint;
      dev.list()->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
      dev.Transition(t.resource, t.state, kRestState);
      return true;
    }

    // Pipelines, intermediates (and for SMAA the lookup textures) of `effect` at this size; false (logged once per
    // effect) when something could not be made.
    bool EnsureEffect(NativeDevice &dev, Effect effect, uint32_t width, uint32_t height)
    {
      Init(dev);
      State &s = g_state;
      bool ok = false;
      const char *missing = "pipelines";
      if (effect == Effect::kSmaa)
      {
        ok = s.smaa[0] && s.smaa[1] && s.smaa[2];
        if (ok && !s.lookup_tried)
        {
          s.lookup_tried = true;
          if (!UploadLookup(dev, s.area, DXGI_FORMAT_R8G8_UNORM, AREATEX_WIDTH, AREATEX_HEIGHT, AREATEX_PITCH,
                            smaa_lookup::areaTexBytes, "pdz_native SMAA AreaTex") ||
              !UploadLookup(dev, s.search, DXGI_FORMAT_R8_UNORM, SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, SEARCHTEX_PITCH,
                            smaa_lookup::searchTexBytes, "pdz_native SMAA SearchTex"))
          {
            for (Texture *t : {&s.area, &s.search})
            {
              dev.ReleaseLater(t->resource);
              *t = Texture();
            }
          }
        }
        if (ok && (!s.area.resource || !s.search.resource))
        {
          ok = false;
          missing = "lookup textures";
        }
        if (ok && !(EnsureTexture(dev, s.smaa_color, kSmaaColorFormat, width, height, "pdz_native SMAA colour") &&
                    EnsureTexture(dev, s.smaa_edges, kSmaaEdgesFormat, width, height, "pdz_native SMAA edges") &&
                    EnsureTexture(dev, s.smaa_weights, kSmaaWeightsFormat, width, height, "pdz_native SMAA weights")))
        {
          ok = false;
          missing = "intermediate textures";
        }
      }
      else if (effect == Effect::kFxaa || effect == Effect::kFxaaExtreme)
      {
        ok = (effect == Effect::kFxaa ? s.fxaa : s.fxaa_extreme) != nullptr;
        if (ok && !EnsureTexture(dev, s.fxaa_source, kFxaaSourceFormat, width, height, "pdz_native FXAA source"))
        {
          ok = false;
          missing = "source texture";
        }
      }
      if (!ok && !s.unavailable_logged[uint32_t(effect)])
      {
        s.unavailable_logged[uint32_t(effect)] = true;
        REXLOG_WARN("pdz_native: swap post effect {}: the {} could not be made; the native output chain writes the "
                    "gamma pass alone",
                    Name(effect), missing);
      }
      return ok;
    }

    // Consecutive descriptors of the frame's shader-visible ring, used one per root table.
    struct Views
    {
      Descriptor base;
      uint32_t stride = 0;
      D3D12_CPU_DESCRIPTOR_HANDLE Cpu(uint32_t i) const
      {
        D3D12_CPU_DESCRIPTOR_HANDLE h = base.cpu;
        h.ptr += SIZE_T(i) * stride;
        return h;
      }
      D3D12_GPU_DESCRIPTOR_HANDLE Gpu(uint32_t i) const
      {
        D3D12_GPU_DESCRIPTOR_HANDLE h = base.gpu;
        h.ptr += UINT64(i) * stride;
        return h;
      }
    };

    void WriteUav(NativeDevice &dev, ID3D12Resource *resource, DXGI_FORMAT format, D3D12_CPU_DESCRIPTOR_HANDLE where)
    {
      D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
      uav.Format = format;
      uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
      dev.device()->CreateUnorderedAccessView(resource, nullptr, &uav, where);
    }
  } // namespace

  const char *Name(Effect effect)
  {
    switch (effect)
    {
    case Effect::kFxaa:
      return "fxaa";
    case Effect::kFxaaExtreme:
      return "fxaa_extreme";
    case Effect::kSmaa:
      return "smaa";
    default:
      return "none";
    }
  }

  Effect PluginEffect()
  {
    static bool latched = false;
    static Effect effect = Effect::kNone;
    if (!latched)
    {
      const std::string value = rex::cvar::GetFlagByName("swap_post_effect");
      if (value.empty())
      {
        return Effect::kNone; // not registered (yet): the plugin applies none; read again next time
      }
      latched = true;
      effect = ParseEffect(value);
      REXLOG_INFO("pdz_native: the GPU plugin's swap post effect is {} (swap_post_effect \"{}\", latched as the SDK "
                  "latches it at start)",
                  Name(effect), value);
    }
    return effect;
  }

  Effect PresentEffect() { return REXCVAR_GET(pdz_native_post_effect) ? PluginEffect() : Effect::kNone; }

  bool RecordChain(NativeDevice &dev, const ChainDesc &desc, Effect &applied)
  {
    applied = Effect::kNone;
    passes::GammaDesc gamma;
    gamma.output = desc.output;
    gamma.width = desc.width;
    gamma.height = desc.height;
    gamma.src_srv = desc.src_srv;
    gamma.lut_srv = desc.lut_srv;
    if (desc.effect == Effect::kNone || !desc.output || !desc.width || !desc.height ||
        !EnsureEffect(dev, desc.effect, desc.width, desc.height))
    {
      return passes::ApplyGamma(dev, gamma); // the gamma pass alone, straight into the output
    }
    State &s = g_state;
    const bool smaa = desc.effect == Effect::kSmaa;
    // Descriptors first, so a full ring still leaves the plain gamma pass: SMAA 3 passes x (3 sources + the
    // destination), FXAA the source + the destination.
    const Views views{dev.AllocSrv(smaa ? 12 : 2), dev.srv_descriptor_size()};
    if (!views.base.cpu.ptr)
    {
      return passes::ApplyGamma(dev, gamma);
    }

    // 1. The gamma pass into the effect's input (the SDK's apply_gamma_dest): SMAA colour or FXAA source.
    Texture &input = smaa ? s.smaa_color : s.fxaa_source;
    gamma.output = input.resource;
    gamma.format = smaa ? kSmaaColorFormat : kFxaaSourceFormat;
    gamma.fxaa_luma = !smaa;
    dev.Transition(input.resource, input.state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const bool gamma_ok = passes::ApplyGamma(dev, gamma);
    dev.Transition(input.resource, input.state, kRestState);
    if (!gamma_ok)
    {
      return false;
    }

    // 2. The effect with the SDK's pipelines, constants and bindings, ending in the output.
    RecList *list = dev.list();
    EffectConstants constants{};
    constants.size[0] = desc.width;
    constants.size[1] = desc.height;
    constants.size_inv[0] = 1.0f / float(desc.width);
    constants.size_inv[1] = 1.0f / float(desc.height);
    const UINT groups_x = (desc.width + 15) / 16, groups_y = (desc.height + 7) / 8;
    dev.ListStateChanged(); // the draw path re-binds its own root signature and PSO after this
    if (smaa)
    {
      ID3D12Resource *color = s.smaa_color.resource;
      ID3D12Resource *edges = s.smaa_edges.resource;
      ID3D12Resource *weights = s.smaa_weights.resource;
      struct Pass
      {
        ID3D12Resource *sources[3]; // unused sources are bound to a valid texture too, as the SDK does
        Texture *dest;              // nullptr = the output
        DXGI_FORMAT dest_format;
        ID3D12PipelineState *pso;
      };
      const Pass chain[3] = {
          {{color, color, color}, &s.smaa_edges, kSmaaEdgesFormat, s.smaa[0]},
          {{edges, s.area.resource, s.search.resource}, &s.smaa_weights, kSmaaWeightsFormat, s.smaa[1]},
          {{color, weights, weights}, nullptr, kOutputFormat, s.smaa[2]},
      };
      list->SetComputeRootSignature(s.smaa_root);
      list->SetComputeRoot32BitConstants(0, 4, &constants, 0);
      for (uint32_t i = 0; i < 3; ++i)
      {
        const Pass &pass = chain[i];
        for (uint32_t j = 0; j < 3; ++j)
        {
          dev.device()->CreateShaderResourceView(pass.sources[j], nullptr, views.Cpu(i * 4 + j));
          list->SetComputeRootDescriptorTable(1 + j, views.Gpu(i * 4 + j));
        }
        ID3D12Resource *dest = pass.dest ? pass.dest->resource : desc.output;
        WriteUav(dev, dest, pass.dest_format, views.Cpu(i * 4 + 3));
        list->SetComputeRootDescriptorTable(4, views.Gpu(i * 4 + 3));
        if (pass.dest)
        {
          dev.Transition(dest, pass.dest->state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        list->SetPipelineState(pass.pso);
        list->Dispatch(groups_x, groups_y, 1);
        if (pass.dest)
        {
          dev.Transition(dest, pass.dest->state, kRestState);
        }
      }
    }
    else
    {
      list->SetComputeRootSignature(s.fxaa_root);
      list->SetComputeRoot32BitConstants(0, 4, &constants, 0);
      dev.device()->CreateShaderResourceView(s.fxaa_source.resource, nullptr, views.Cpu(0));
      list->SetComputeRootDescriptorTable(1, views.Gpu(0));
      WriteUav(dev, desc.output, kOutputFormat, views.Cpu(1));
      list->SetComputeRootDescriptorTable(2, views.Gpu(1));
      list->SetPipelineState(desc.effect == Effect::kFxaaExtreme ? s.fxaa_extreme : s.fxaa);
      list->Dispatch(groups_x, groups_y, 1);
    }
    applied = desc.effect;
    if (!s.recorded_logged[uint32_t(applied)])
    {
      s.recorded_logged[uint32_t(applied)] = true;
      REXLOG_INFO("pdz_native: swap post effect {} recorded on the native output chain for the first time ({}x{}, "
                  "the SDK's compute shaders)",
                  Name(applied), desc.width, desc.height);
    }
    return true;
  }

  uint32_t PipelineCount()
  {
    const State &s = g_state;
    return uint32_t((s.smaa[0] ? 1 : 0) + (s.smaa[1] ? 1 : 0) + (s.smaa[2] ? 1 : 0) + (s.fxaa ? 1 : 0) +
                    (s.fxaa_extreme ? 1 : 0));
  }

  uint32_t RootSignatureCount() { return uint32_t((g_state.smaa_root ? 1 : 0) + (g_state.fxaa_root ? 1 : 0)); }
} // namespace pdz::native::postfx
