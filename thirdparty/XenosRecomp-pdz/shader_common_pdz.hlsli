// Runtime side of the HLSL emitted by XenosRecomp --pdz (pdz branch).  Binding model for the native renderer:
//
//   b0 space0  FloatConstants      float4 c[256]    VS: c0..c255; PS: c0..c223 (hardware registers 256..479)
//   b1 space0  BoolLoopConstants   256 bool constants as bits (hardware index: VS b0..b127, PS b128..b255,
//                                  i.e. SetPixelShaderConstantB(n) sets bit 128+n) and the 32 loop constants
//                                  as the hardware dword (count 7:0, start 15:8, step 23:16, signed;
//                                  SetPixelShaderConstantI(n, {count, start, step}) packs into loop 16+n)
//   b2 space0  SystemConstants     half-pixel offset, alpha test, VTE flags, guest -> host NDC scale / offset
//   t0..t31    space0 Texture2D, space1 Texture3D, space2 TextureCube: one per texture fetch constant slot
//   s0..s31    space0 SamplerState: per-shader sampler variants; the sidecar .json lists, for each index,
//                                  the fetch constant slot and the per-instruction filter overrides
//
// Vertex input semantics (raw / bound microcode): ATTR<n> per distinct (slot, offset, format) fetched, with
// the DXGI format and byte offset in the sidecar .json; legacy-container vertex shaders with unbound
// vfetches use the declaration usages (POSITION0, TEXCOORD1, ...).  VS -> PS linkage is positional: VS export
// o<n> and PS register r<n> both map to TEXCOORD<n>.

#ifndef SHADER_COMMON_PDZ_HLSLI
#define SHADER_COMMON_PDZ_HLSLI

#define FLT_MIN asfloat(0xff7fffff)
#define FLT_MAX asfloat(0x7f7fffff)

cbuffer FloatConstants : register(b0, space0)
{
    float4 c[256];
};

cbuffer BoolLoopConstants : register(b1, space0)
{
    uint4 g_BoolBits[2];
    uint4 g_LoopConstants[8];
};

cbuffer SystemConstants : register(b2, space0)
{
    float2 g_HalfPixelOffset;   // added to oPos.xy * oPos.w (D3D9 half pixel convention)
    float g_AlphaRef;           // alpha test reference 0..1
    uint g_AlphaFunc;           // 0 = off, else D3DCMPFUNC (1 never .. 8 always)
    float4 g_PointSize;         // x = default point size
    uint g_Flags;               // PA_CL_VTE_CNTL formats: bit 0 VTX_XY_FMT (XY pre-divided by W), bit 1 VTX_Z_FMT
                                // (Z pre-divided by W), bit 2 VTX_W0_FMT (W exported as W, not 1/W)
    uint3 g_Reserved;
    float4 g_NdcScale;          // xyz: guest clip space -> host clip space scale (viewport, clip space, Y flip)
    float4 g_NdcOffset;         // xyz: offset multiplied by W (viewport / window / half-pixel offsets)
};

// Vertex position epilogue, the same steps as the ReXGlue SDK's DxbcShaderTranslator::CompleteVertexOrDomainShader
// with ndc_scale / ndc_offset from draw_util::GetHostViewportInfo (computed per draw by the native renderer).
float4 pdzHostPosition(float4 p)
{
    if ((g_Flags & 4u) == 0u)
        p.w = 1.0 / p.w;
    if ((g_Flags & 1u) != 0u)
        p.xy *= p.w;
    if ((g_Flags & 2u) != 0u)
        p.z *= p.w;
    p.xy += g_HalfPixelOffset * p.w;
    p.xyz = p.xyz * g_NdcScale.xyz + g_NdcOffset.xyz * p.w;
    return p;
}

Texture2D<float4> g_Texture2D[32] : register(t0, space0);
Texture3D<float4> g_Texture3D[32] : register(t0, space1);
TextureCube<float4> g_TextureCube[32] : register(t0, space2);
SamplerState g_Samplers[32] : register(s0, space0);

// Relative constant access (c[a0 + n], c[aL + n]): out-of-range reads return 0 like the hardware.
float4 cRel(int index)
{
    return (uint(index) < 256u) ? c[uint(index) & 255u] : float4(0.0, 0.0, 0.0, 0.0);
}

bool boolConst(uint index)
{
    return ((g_BoolBits[(index >> 7) & 1u][(index >> 5) & 3u] >> (index & 31u)) & 1u) != 0u;
}

uint loopConst(uint index)
{
    return g_LoopConstants[(index >> 2) & 7u][index & 3u];
}

uint loopCount(uint value) { return value & 0xFFu; }
int loopStart(uint value) { return int((value >> 8) & 0xFFu); }
int loopStep(uint value) { return (int(value << 8)) >> 24; }

// Vertex attribute decoding for formats D3D12 input layouts cannot express directly.
// 2_10_10_10: x bits 9:0, y 19:10, z 29:20, w 31:30 (hardware component order).
float4 decode2_10_10_10(uint v, bool isSigned, bool normalized)
{
    float4 r;
    if (isSigned)
    {
        int4 i = int4(int(v << 22) >> 22, int(v << 12) >> 22, int(v << 2) >> 22, int(v) >> 30);
        r = float4(i);
        if (normalized)
            r = max(r / float4(511.0, 511.0, 511.0, 1.0), -1.0);
    }
    else
    {
        uint4 u = uint4(v & 0x3FFu, (v >> 10) & 0x3FFu, (v >> 20) & 0x3FFu, v >> 30);
        r = float4(u);
        if (normalized)
            r /= float4(1023.0, 1023.0, 1023.0, 3.0);
    }
    return r;
}

// 10_11_11: x bits 10:0 (11 bits), y 21:11 (11 bits), z 31:22 (10 bits).
float4 decode10_11_11(uint v, bool isSigned, bool normalized)
{
    float4 r;
    if (isSigned)
    {
        int3 i = int3(int(v << 21) >> 21, int(v << 10) >> 21, int(v) >> 22);
        r = float4(float3(i), 0.0);
        if (normalized)
            r.xyz = max(r.xyz / float3(1023.0, 1023.0, 511.0), -1.0);
    }
    else
    {
        uint3 u = uint3(v & 0x7FFu, (v >> 11) & 0x7FFu, v >> 22);
        r = float4(float3(u), 0.0);
        if (normalized)
            r.xyz /= float3(2047.0, 2047.0, 1023.0);
    }
    return r;
}

// 11_11_10: x bits 9:0 (10 bits), y 20:10 (11 bits), z 31:21 (11 bits).
float4 decode11_11_10(uint v, bool isSigned, bool normalized)
{
    float4 r;
    if (isSigned)
    {
        int3 i = int3(int(v << 22) >> 22, int(v << 11) >> 21, int(v) >> 21);
        r = float4(float3(i), 0.0);
        if (normalized)
            r.xyz = max(r.xyz / float3(511.0, 1023.0, 1023.0), -1.0);
    }
    else
    {
        uint3 u = uint3(v & 0x3FFu, (v >> 10) & 0x7FFu, v >> 21);
        r = float4(float3(u), 0.0);
        if (normalized)
            r.xyz /= float3(1023.0, 2047.0, 2047.0);
    }
    return r;
}

float4 decode32Normalized(uint4 v, bool isSigned)
{
    return isSigned ? max(float4(int4(v)) / 2147483647.0, -1.0) : float4(v) / 4294967295.0;
}

// Texture fetch helpers. `offset` is the instruction's texel offset (already in texels).
float2 texSize2D(Texture2D<float4> t)
{
    float2 s;
    t.GetDimensions(s.x, s.y);
    return s;
}

float3 texSize3D(Texture3D<float4> t)
{
    float3 s;
    t.GetDimensions(s.x, s.y, s.z);
    return s;
}

float4 tfetch2D(Texture2D<float4> t, SamplerState s, float2 uv, float2 offset)
{
    return t.Sample(s, uv + offset / texSize2D(t));
}

float4 tfetch2DBias(Texture2D<float4> t, SamplerState s, float2 uv, float2 offset, float bias)
{
    return t.SampleBias(s, uv + offset / texSize2D(t), bias);
}

float4 tfetch2DLevel(Texture2D<float4> t, SamplerState s, float2 uv, float2 offset, float lod)
{
    return t.SampleLevel(s, uv + offset / texSize2D(t), lod);
}

float4 tfetch3D(Texture3D<float4> t, SamplerState s, float3 uvw, float3 offset)
{
    return t.Sample(s, uvw + offset / texSize3D(t));
}

float4 tfetch3DBias(Texture3D<float4> t, SamplerState s, float3 uvw, float3 offset, float bias)
{
    return t.SampleBias(s, uvw + offset / texSize3D(t), bias);
}

float4 tfetch3DLevel(Texture3D<float4> t, SamplerState s, float3 uvw, float3 offset, float lod)
{
    return t.SampleLevel(s, uvw + offset / texSize3D(t), lod);
}

float4 tfetchCube(TextureCube<float4> t, SamplerState s, float3 dir)
{
    return t.Sample(s, dir);
}

float4 tfetchCubeLevel(TextureCube<float4> t, SamplerState s, float3 dir, float lod)
{
    return t.SampleLevel(s, dir, lod);
}

// Unnormalized coordinates (texels) -> normalized.
float2 unnormalize2D(Texture2D<float4> t, float2 texels)
{
    return texels / texSize2D(t);
}

float3 unnormalize3D(Texture3D<float4> t, float3 texels)
{
    return texels / texSize3D(t);
}

float2 getWeights2D(Texture2D<float4> t, float2 uv, float2 offset)
{
    return select(isnan(uv), 0.0, frac(uv * texSize2D(t) + offset - 0.5));
}

// Xenos `cube` instruction: returns (2 * major axis magnitude? no:) the hardware returns
// (s face coordinate, t face coordinate, 2 * major axis, face id) - the tfetchCube below only needs the
// direction, so we keep the direction and the face id in a side structure like upstream does.
struct CubeMapData
{
    float3 directions[2];
    uint index;
};

float4 cube(float4 value, inout CubeMapData data)
{
    uint index = data.index;
    data.directions[index] = value.xyz;
    data.index = (index + 1u) & 1u;
    return float4(0.0, 0.0, 0.0, index);
}

float4 tfetchCubeData(TextureCube<float4> t, SamplerState s, float3 coord, CubeMapData data)
{
    return t.Sample(s, data.directions[uint(coord.z) & 1u]);
}

float4 dst(float4 src0, float4 src1)
{
    return float4(1.0, src0.y * src1.y, src0.z, src1.w);
}

float4 max4(float4 v)
{
    return max(max(v.x, v.y), max(v.z, v.w));
}

void alphaTest(float alpha)
{
    [branch] if (g_AlphaFunc != 0u)
    {
        bool pass = true;
        switch (g_AlphaFunc)
        {
        case 1u: pass = false; break;
        case 2u: pass = alpha < g_AlphaRef; break;
        case 3u: pass = alpha == g_AlphaRef; break;
        case 4u: pass = alpha <= g_AlphaRef; break;
        case 5u: pass = alpha > g_AlphaRef; break;
        case 6u: pass = alpha != g_AlphaRef; break;
        case 7u: pass = alpha >= g_AlphaRef; break;
        default: pass = true; break;
        }
        clip(pass ? 1.0 : -1.0);
    }
}

#endif
