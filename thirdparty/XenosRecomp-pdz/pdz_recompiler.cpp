// PDZ translation path (pdz branch): legacy containers and raw microcode without a constant table.
// Shares the ALU emitter with the upstream path (shader_recompiler.cpp); everything that depended on the
// 0x102A11xx container (vertex elements, interpolator names, constant names, definition table) is replaced
// by information taken from the microcode itself plus, when present, the legacy container's program-info.
#include "shader_recompiler.h"
#include "shader_pdz.h"

namespace
{

constexpr uint32_t PDZ_INTERPOLATOR_COUNT = 16;

// xenos::VertexFormat
enum class VertexFormat : uint32_t
{
    Undefined = 0,
    k_8_8_8_8 = 6,
    k_2_10_10_10 = 7,
    k_10_11_11 = 16,
    k_11_11_10 = 17,
    k_16_16 = 25,
    k_16_16_16_16 = 26,
    k_16_16_FLOAT = 31,
    k_16_16_16_16_FLOAT = 32,
    k_32 = 33,
    k_32_32 = 34,
    k_32_32_32_32 = 35,
    k_32_FLOAT = 36,
    k_32_32_FLOAT = 37,
    k_32_32_32_32_FLOAT = 38,
    k_32_32_32_FLOAT = 57,
};

const char* const PDZ_USAGE_VARIABLES[] =
{
    "Position", "BlendWeight", "BlendIndices", "Normal", "PointSize", "TexCoord", "Tangent", "Binormal",
    "TessFactor", "PositionT", "Color", "Fog", "Depth", "Sample", "Usage14", "Usage15"
};

const char* const PDZ_USAGE_SEMANTICS[] =
{
    "POSITION", "BLENDWEIGHT", "BLENDINDICES", "NORMAL", "PSIZE", "TEXCOORD", "TANGENT", "BINORMAL",
    "TESSFACTOR", "POSITIONT", "COLOR", "FOG", "DEPTH", "SAMPLE", "USAGE14", "USAGE15"
};

struct FormatInfo
{
    uint32_t components;
    bool isFloat;       // IEEE float components
    bool packed;        // needs a decode helper from a raw dword (2_10_10_10, 10_11_11, 11_11_10)
    uint32_t bitsPerComponent;
    const char* name;
};

FormatInfo formatInfo(VertexFormat format)
{
    switch (format)
    {
    case VertexFormat::k_8_8_8_8: return { 4, false, false, 8, "8_8_8_8" };
    case VertexFormat::k_2_10_10_10: return { 4, false, true, 10, "2_10_10_10" };
    case VertexFormat::k_10_11_11: return { 3, false, true, 11, "10_11_11" };
    case VertexFormat::k_11_11_10: return { 3, false, true, 11, "11_11_10" };
    case VertexFormat::k_16_16: return { 2, false, false, 16, "16_16" };
    case VertexFormat::k_16_16_16_16: return { 4, false, false, 16, "16_16_16_16" };
    case VertexFormat::k_16_16_FLOAT: return { 2, true, false, 16, "16_16_FLOAT" };
    case VertexFormat::k_16_16_16_16_FLOAT: return { 4, true, false, 16, "16_16_16_16_FLOAT" };
    case VertexFormat::k_32: return { 1, false, false, 32, "32" };
    case VertexFormat::k_32_32: return { 2, false, false, 32, "32_32" };
    case VertexFormat::k_32_32_32_32: return { 4, false, false, 32, "32_32_32_32" };
    case VertexFormat::k_32_FLOAT: return { 1, true, false, 32, "32_FLOAT" };
    case VertexFormat::k_32_32_FLOAT: return { 2, true, false, 32, "32_32_FLOAT" };
    case VertexFormat::k_32_32_32_32_FLOAT: return { 4, true, false, 32, "32_32_32_32_FLOAT" };
    case VertexFormat::k_32_32_32_FLOAT: return { 3, true, false, 32, "32_32_32_FLOAT" };
    default: return { 0, false, false, 0, "UNDEFINED" };
    }
}

// DXGI format name for the input layout the renderer has to build, and the HLSL input type.
void inputTypeFor(VertexFormat format, bool isSigned, bool normalized, std::string& hlslType, std::string& dxgi, std::string& convert)
{
    const FormatInfo info = formatInfo(format);
    static const char* const COMPONENT_NAMES[] = { "", "R", "RG", "RGB", "RGBA" };
    static const char* const COMPONENT_NAMES_32[] = { "", "R32", "R32G32", "R32G32B32", "R32G32B32A32" };
    static const char* const COMPONENT_NAMES_16[] = { "", "R16", "R16G16", "", "R16G16B16A16" };
    static const char* const COMPONENT_NAMES_8[] = { "", "R8", "R8G8", "", "R8G8B8A8" };
    const char* base = info.bitsPerComponent == 32 ? COMPONENT_NAMES_32[info.components]
        : info.bitsPerComponent == 16 ? COMPONENT_NAMES_16[info.components] : COMPONENT_NAMES_8[info.components];

    if (info.packed)
    {
        hlslType = "uint";
        dxgi = "DXGI_FORMAT_R32_UINT";
        const char* decoder = format == VertexFormat::k_2_10_10_10 ? "decode2_10_10_10"
            : format == VertexFormat::k_10_11_11 ? "decode10_11_11" : "decode11_11_10";
        convert = fmt::format("{}({{}}, {}, {})", decoder, isSigned ? "true" : "false", normalized ? "true" : "false");
        return;
    }

    const char* vecSuffix = info.components == 1 ? "" : (info.components == 2 ? "2" : (info.components == 3 ? "3" : "4"));
    auto pad = [&](const std::string& expr)
    {
        switch (info.components)
        {
        case 1: return fmt::format("float4({}, 0.0, 0.0, 1.0)", expr);
        case 2: return fmt::format("float4({}, 0.0, 1.0)", expr);
        case 3: return fmt::format("float4({}, 1.0)", expr);
        default: return expr;
        }
    };

    if (info.isFloat)
    {
        hlslType = fmt::format("float{}", vecSuffix);
        dxgi = fmt::format("DXGI_FORMAT_{}_FLOAT", base);
        convert = pad("{}");
    }
    else if (normalized && info.bitsPerComponent < 32)
    {
        hlslType = fmt::format("float{}", vecSuffix);
        dxgi = fmt::format("DXGI_FORMAT_{}_{}", base, isSigned ? "SNORM" : "UNORM");
        convert = pad("{}");
    }
    else if (normalized)
    {
        // 32-bit normalized integers have no DXGI input format: fetch raw and divide
        hlslType = fmt::format("uint{}", vecSuffix);
        dxgi = fmt::format("DXGI_FORMAT_{}_UINT", base);
        std::string widen = info.components == 4 ? "{}" : fmt::format("uint4({}{})", "{}", std::string(", 0u", 0) + std::string(4 - info.components, ' ').replace(0, 4 - info.components, ""));
        // keep it simple: build a uint4 with zeros
        switch (info.components)
        {
        case 1: widen = "uint4({}, 0u, 0u, 0u)"; break;
        case 2: widen = "uint4({}, 0u, 0u)"; break;
        case 3: widen = "uint4({}, 0u)"; break;
        default: widen = "{}"; break;
        }
        convert = fmt::format("decode32Normalized({}, {})", widen, isSigned ? "true" : "false");
    }
    else
    {
        hlslType = fmt::format("{}{}", isSigned ? "int" : "uint", vecSuffix);
        dxgi = fmt::format("DXGI_FORMAT_{}_{}", base, isSigned ? "SINT" : "UINT");
        convert = pad(fmt::format("float{}({{}})", vecSuffix));
    }
}

std::string jsonEscape(const std::string& s)
{
    std::string out;
    for (char ch : s)
    {
        if (ch == '"' || ch == '\\')
        {
            out += '\\';
            out += ch;
        }
        else if (uint8_t(ch) < 0x20)
            out += fmt::format("\\u{:04x}", uint8_t(ch));
        else
            out += ch;
    }
    return out;
}

}

std::string ShaderRecompiler::pdzConstantRef(uint32_t reg, bool relative, bool useA0)
{
    if (relative)
    {
        pdz.usesRelativeConstants = true;
        return fmt::format("cRel({} + {})", reg, useA0 ? "a0" : "aL");
    }
    auto literal = pdz.literalRegisters.find(reg);
    if (literal != pdz.literalRegisters.end())
        return fmt::format("cl{}", reg);
    pdz.floatConstantsUsed.insert(reg);
    return fmt::format("c[{}]", reg);
}

void ShaderRecompiler::recompilePdzVertexFetch(const VertexFetchInstruction& instr, uint32_t address)
{
    if (instr.isPredicated)
    {
        indent();
        println("if ({}p0)", instr.predicateCondition ? "" : "!");
        indent();
        out += "{\n";
        ++indentation;
    }

    auto input = pdz.vertexInputsByAddress.find(address);
    assert(input != pdz.vertexInputsByAddress.end());
    const PdzVertexInput& vi = pdz.vertexInputs[input->second];

    if (hasDstComponents(instr.dstSwizzle))
    {
        indent();
        print("r{}.", instr.dstRegister);
        printDstSwizzle(instr.dstSwizzle, false);
        out += " = ";
        if (instr.expAdjust != 0)
            print("({} * {}).", vi.convertedName, std::ldexp(1.0f, instr.expAdjust));
        else
            print("{}.", vi.convertedName);
        printDstSwizzle(instr.dstSwizzle, true);
        out += ";\n";
    }

    printDstSwizzle01(instr.dstRegister, instr.dstSwizzle);

    if (instr.isPredicated)
    {
        --indentation;
        indent();
        out += "}\n";
    }
}

void ShaderRecompiler::recompilePdzTextureFetch(const TextureFetchInstruction& instr)
{
    if (instr.opcode == FetchOpcode::SetTextureLod)
    {
        indent();
        println("texLod = r{}.{};", instr.srcRegister, SWIZZLES[instr.srcSwizzle & 0x3]);
        pdz.usesRegisterLod = true;
        return;
    }

    if (instr.opcode != FetchOpcode::TextureFetch && instr.opcode != FetchOpcode::GetTextureWeights)
    {
        indent();
        println("// unsupported fetch opcode {}", uint32_t(instr.opcode));
        pdz.unsupported.push_back(fmt::format("fetch opcode {}", uint32_t(instr.opcode)));
        return;
    }

    if (instr.isPredicated)
    {
        indent();
        println("if ({}p0)", instr.predCondition ? "" : "!");
        indent();
        out += "{\n";
        ++indentation;
    }

    std::string_view dimension;
    uint32_t componentCount = 0;
    const char* textureArray = nullptr;
    switch (instr.dimension)
    {
    case TextureDimension::Texture1D: dimension = "2D"; componentCount = 1; textureArray = "g_Texture2D"; break;
    case TextureDimension::Texture2D: dimension = "2D"; componentCount = 2; textureArray = "g_Texture2D"; break;
    case TextureDimension::Texture3D: dimension = "3D"; componentCount = 3; textureArray = "g_Texture3D"; break;
    case TextureDimension::TextureCube: dimension = "Cube"; componentCount = 3; textureArray = "g_TextureCube"; break;
    }

    std::string texture = fmt::format("{}[{}]", textureArray, instr.constIndex);
    std::string sampler = fmt::format("g_Samplers[{}]", pdzSamplerVariant(instr));

    {
        PdzTextureFetchSite site{};
        site.slot = instr.constIndex;
        site.dimension = uint32_t(instr.dimension);
        site.unnormalized = instr.texCoordDenorm != 0;
        site.computedLod = instr.useCompLod && (isPixelShader || instr.useRegGradients);
        site.registerLod = instr.useRegLod != 0;
        site.lodBias = instr.lodBias / 16.0f;
        site.offsets = instr.offsetX != 0 || instr.offsetY != 0 || instr.offsetZ != 0;
        pdz.textureFetchSites.push_back(site);
    }

    std::string coord = fmt::format("r{}.", instr.srcRegister);
    for (uint32_t i = 0; i < componentCount; i++)
        coord += SWIZZLES[(instr.srcSwizzle >> (i * 2)) & 0x3];
    if (instr.dimension == TextureDimension::Texture1D)
        coord = fmt::format("float2({}, 0.0)", coord);

    if (instr.texCoordDenorm && instr.dimension != TextureDimension::TextureCube)
        coord = fmt::format("unnormalize{}({}, {})", dimension, texture, coord);

    std::string offset;
    if (instr.dimension == TextureDimension::Texture2D || instr.dimension == TextureDimension::Texture1D)
        offset = fmt::format("float2({}, {})", instr.offsetX * 0.5f, instr.offsetY * 0.5f);
    else if (instr.dimension == TextureDimension::Texture3D)
        offset = fmt::format("float3({}, {}, {})", instr.offsetX * 0.5f, instr.offsetY * 0.5f, instr.offsetZ * 0.5f);

    // a fetch whose destination swizzle is all 0/1/keep assigns nothing from the fetch itself; keep the
    // fetch result in a scratch variable so the 0/1 fills below still apply
    const bool assignsResult = hasDstComponents(instr.dstSwizzle);
    indent();
    if (assignsResult)
    {
        print("r{}.", instr.dstRegister);
        printDstSwizzle(instr.dstSwizzle, false);
        out += " = ";
    }
    else
        out += "float4 fetchUnused = ";

    if (instr.opcode == FetchOpcode::GetTextureWeights)
    {
        print("getWeights2D({}, {}, {}).", texture, coord, offset);
    }
    else
    {
        // LOD selection as the SDK translator does it: computed (gradient) LOD only in pixel shaders
        // (or with register gradients, unused in PDZ); otherwise the register LOD (setTexLOD) or 0,
        // plus the instruction bias in both cases.
        const bool computedLod = instr.useCompLod && (isPixelShader || instr.useRegGradients);
        const float lodBias = instr.lodBias / 16.0f;
        std::string lod;
        if (!computedLod)
        {
            if (instr.useRegLod)
            {
                pdz.usesRegisterLod = true;
                lod = lodBias != 0.0f ? fmt::format("texLod + {}", lodBias) : "texLod";
            }
            else
                lod = fmt::format("{}", lodBias);
        }

        if (instr.dimension == TextureDimension::TextureCube)
        {
            if (computedLod)
                print("tfetchCubeData({}, {}, {}, cubeMapData).", texture, sampler, coord);
            else
                print("tfetchCubeLevel({}, {}, cubeMapData.directions[uint({}.z) & 1u], {}).", texture, sampler, coord, lod);
        }
        else if (computedLod)
        {
            if (lodBias != 0.0f)
                print("tfetch{}Bias({}, {}, {}, {}, {}).", dimension, texture, sampler, coord, offset, lodBias);
            else
                print("tfetch{}({}, {}, {}, {}).", dimension, texture, sampler, coord, offset);
        }
        else
        {
            print("tfetch{}Level({}, {}, {}, {}, {}).", dimension, texture, sampler, coord, offset, lod);
        }
    }

    if (assignsResult)
        printDstSwizzle(instr.dstSwizzle, true);
    else
        out += "xyzw";
    out += ";\n";

    printDstSwizzle01(instr.dstRegister, instr.dstSwizzle);

    if (instr.isPredicated)
    {
        --indentation;
        indent();
        out += "}\n";
    }
}

// Packed override code (sidecar "code" and the HLSL comment): mag 1:0, min 3:2, mip 5:4 (0 point, 1 linear,
// 2 base map, 3 = use the fetch constant), aniso 8:6 (7 = fetch constant), volMag 10:9, volMin 12:11.
// 0x1FFF = no override at all.
uint32_t ShaderRecompiler::pdzSamplerCode(const PdzSamplerVariant& v)
{
    return (v.magFilter & 3) | ((v.minFilter & 3) << 2) | ((v.mipFilter & 3) << 4) | ((v.anisoFilter & 7) << 6) |
        ((v.volMagFilter & 3) << 9) | ((v.volMinFilter & 3) << 11);
}

uint32_t ShaderRecompiler::pdzSamplerVariant(const TextureFetchInstruction& instr)
{
    PdzSamplerVariant v{};
    v.slot = instr.constIndex;
    v.magFilter = instr.magFilter;
    v.minFilter = instr.minFilter;
    v.mipFilter = instr.mipFilter;
    v.anisoFilter = instr.anisoFilter;
    v.volMagFilter = instr.volMagFilter;
    v.volMinFilter = instr.volMinFilter;
    v.dimension = uint32_t(instr.dimension);
    for (size_t i = 0; i < pdz.samplerVariants.size(); i++)
    {
        const PdzSamplerVariant& e = pdz.samplerVariants[i];
        if (e.slot == v.slot && e.magFilter == v.magFilter && e.minFilter == v.minFilter && e.mipFilter == v.mipFilter &&
            e.anisoFilter == v.anisoFilter && e.volMagFilter == v.volMagFilter && e.volMinFilter == v.volMinFilter &&
            e.dimension == v.dimension)
            return uint32_t(i);
    }
    pdz.samplerVariants.push_back(v);
    return uint32_t(pdz.samplerVariants.size() - 1);
}

void ShaderRecompiler::recompilePdz(const pdz::Shader& shader, const std::string_view& include)
{
    pdzMode = true;
    isPixelShader = shader.isPixelShader;

    for (auto& lit : shader.literals)
        for (uint32_t i = 0; i < (uint32_t(lit.dwords.size()) + 3) / 4; i++)
            pdz.literalRegisters.emplace(lit.registerIndex + i, &lit);

    // ---- pass 1: walk the control flow, collect the instruction stream bound, fetch declarations,
    // exports, live-in registers.
    const be<uint32_t>* code = shader.ucode;
    uint32_t instrSize = shader.ucodeDwords * 4;

    union CfPair
    {
        ControlFlowInstruction controlFlow[2];
        struct
        {
            uint32_t code0, code1, code2, code3;
        };
    };

    auto readPair = [&](const be<uint32_t>* p, CfPair& pair)
    {
        pair.code0 = p[0];
        pair.code1 = p[1] & 0xFFFF;
        pair.code2 = (p[1] >> 16) | (p[2] << 16);
        pair.code3 = p[2] >> 16;
    };

    // Cf stream length: the lowest exec address bounds the CF instructions (like upstream).
    {
        uint32_t instrAddress = 0;
        auto cfCode = code;
        while (instrAddress < instrSize)
        {
            CfPair pair;
            readPair(cfCode, pair);
            for (auto& cf : pair.controlFlow)
            {
                uint32_t address = 0;
                switch (cf.opcode)
                {
                case ControlFlowOpcode::Exec:
                case ControlFlowOpcode::ExecEnd:
                    address = cf.exec.address;
                    break;
                case ControlFlowOpcode::CondExec:
                case ControlFlowOpcode::CondExecEnd:
                case ControlFlowOpcode::CondExecPredClean:
                case ControlFlowOpcode::CondExecPredCleanEnd:
                    address = cf.condExec.address;
                    break;
                case ControlFlowOpcode::CondExecPred:
                case ControlFlowOpcode::CondExecPredEnd:
                    address = cf.condExecPred.address;
                    break;
                case ControlFlowOpcode::CondJmp:
                    if (cf.condJmp.isUnconditional || cf.condJmp.direction)
                        pdz.simpleControlFlow = false;
                    else
                        ++ifEndLabels[cf.condJmp.address];
                    break;
                case ControlFlowOpcode::CondCall:
                case ControlFlowOpcode::Return:
                    pdz.simpleControlFlow = false;
                    pdz.unsupported.push_back("call/return control flow");
                    break;
                }
                if (address != 0)
                    instrSize = std::min<uint32_t>(instrSize, address * 12);
            }
            cfCode += 3;
            instrAddress += 12;
        }
    }
    pdz.cfBytes = instrSize;

    bool written[64]{};
    uint32_t lastFullSlot = 0, lastFullStride = 0;

    auto noteRead = [&](uint32_t reg)
    {
        if (reg < 32 && !written[reg])
            pdz.liveInRegisters.insert(reg);
    };

    auto analyseAlu = [&](const AluInstruction& alu)
    {
        auto srcRead = [&](uint32_t reg, bool select)
        {
            if (select)
                noteRead(reg & 0x3F);
        };
        // scalar-constant ops read src3 as a constant and a temp packed in the fields; approximate:
        srcRead(alu.src1Register, alu.src1Select);
        srcRead(alu.src2Register, alu.src2Select);
        srcRead(alu.src3Register, alu.src3Select);
        if (alu.exportData)
        {
            const uint32_t dest = alu.vectorDest;
            if (isPixelShader)
            {
                if (dest < 4)
                    pdz.colorOutputs |= 1u << dest;
                else if (dest == uint32_t(ExportRegister::PSDepth))
                    pdz.writesDepth = true;
                else
                    pdz.unsupported.push_back(fmt::format("pixel export {}", dest));
            }
            else
            {
                if (dest < PDZ_INTERPOLATOR_COUNT)
                    pdz.interpolatorsWritten |= 1u << dest;
                else if (dest == uint32_t(ExportRegister::VSPosition))
                    pdz.writesPosition = true;
                else if (dest == uint32_t(ExportRegister::VSPointSizeEdgeFlagKillVertex))
                    pdz.writesPointSize = true;
                else
                    pdz.unsupported.push_back(fmt::format("vertex export {} (memexport?)", dest));
            }
        }
        else
        {
            if (alu.vectorWriteMask != 0)
                written[alu.vectorDest & 0x3F] = true;
            if (alu.scalarWriteMask != 0)
                written[alu.scalarDest & 0x3F] = true;
        }
        if (alu.vectorOpcode == AluVectorOpcode::MaxA || alu.scalarOpcode == AluScalarOpcode::MaxAs || alu.scalarOpcode == AluScalarOpcode::MaxAsf)
            pdz.writesA0 = true;
        if (alu.vectorOpcode >= AluVectorOpcode::KillEq && alu.vectorOpcode <= AluVectorOpcode::KillNe)
            pdz.usesKill = true;
        if (alu.scalarOpcode >= AluScalarOpcode::KillsEq && alu.scalarOpcode <= AluScalarOpcode::KillsOne)
            pdz.usesKill = true;
        if (alu.vectorDestRelative || (alu.scalarDestRelative && !alu.exportData))
            pdz.unsupported.push_back("aL-relative temporary register");
    };

    {
        uint32_t instrAddress = 0;
        auto cfCode = code;
        while (instrAddress < instrSize)
        {
            CfPair pair;
            readPair(cfCode, pair);
            for (auto& cf : pair.controlFlow)
            {
                uint32_t address = 0, count = 0, sequence = 0;
                switch (cf.opcode)
                {
                case ControlFlowOpcode::Exec:
                case ControlFlowOpcode::ExecEnd:
                    address = cf.exec.address; count = cf.exec.count; sequence = cf.exec.sequence;
                    break;
                case ControlFlowOpcode::CondExec:
                case ControlFlowOpcode::CondExecEnd:
                case ControlFlowOpcode::CondExecPredClean:
                case ControlFlowOpcode::CondExecPredCleanEnd:
                    address = cf.condExec.address; count = cf.condExec.count; sequence = cf.condExec.sequence;
                    pdz.boolConstantsUsed.insert(cf.condExec.boolAddress);
                    break;
                case ControlFlowOpcode::CondExecPred:
                case ControlFlowOpcode::CondExecPredEnd:
                    address = cf.condExecPred.address; count = cf.condExecPred.count; sequence = cf.condExecPred.sequence;
                    break;
                case ControlFlowOpcode::LoopStart:
                    pdz.loopConstantsUsed.insert(cf.loopStart.loopId);
                    break;
                case ControlFlowOpcode::CondJmp:
                    if (!cf.condJmp.isPredicated)
                        pdz.boolConstantsUsed.insert(cf.condJmp.boolAddress);
                    break;
                }

                auto instructionCode = code + address * 3;
                for (uint32_t i = 0; i < count; i++)
                {
                    union
                    {
                        VertexFetchInstruction vertexFetch;
                        TextureFetchInstruction textureFetch;
                        AluInstruction alu;
                        struct { uint32_t code0, code1, code2; };
                    };
                    code0 = instructionCode[0];
                    code1 = instructionCode[1];
                    code2 = instructionCode[2];

                    if ((sequence & 0x1) != 0)
                    {
                        if (vertexFetch.opcode == FetchOpcode::VertexFetch)
                        {
                            noteRead(vertexFetch.srcRegister);
                            written[vertexFetch.dstRegister] = true;

                            PdzVertexInput vi{};
                            vi.address = address + i;
                            vi.isMini = vertexFetch.isMiniFetch;
                            if (!vertexFetch.isMiniFetch)
                            {
                                lastFullSlot = vertexFetch.constIndex * 3 + vertexFetch.constIndexSelect;
                                lastFullStride = vertexFetch.stride;
                            }
                            vi.slot = lastFullSlot;
                            vi.stride = lastFullStride;
                            vi.offset = vertexFetch.offset;
                            vi.format = vertexFetch.format;
                            vi.isSigned = vertexFetch.formatCompAll;
                            vi.normalized = vertexFetch.numFormatAll == 0;
                            vi.srcRegister = vertexFetch.srcRegister;
                            vi.srcSwizzle = vertexFetch.srcSwizzle;
                            vi.expAdjust = vertexFetch.expAdjust;
                            if (vertexFetch.srcRegisterAm || vertexFetch.dstRegisterAam)
                                pdz.unsupported.push_back("relative vfetch register");

                            // one HLSL input per distinct (slot, offset, format, signed, normalized); unbound fetches (format 0) stay one per instruction
                            if (vi.format == 0 && shader.fromContainer)
                            {
                                for (auto& e : shader.vertexElements)
                                    if (e.address == vi.address)
                                    {
                                        vi.usage = e.usage;
                                        vi.usageIndex = e.usageIndex;
                                    }
                            }
                            size_t index = pdz.vertexInputs.size();
                            for (size_t k = 0; k < pdz.vertexInputs.size(); k++)
                            {
                                auto& e = pdz.vertexInputs[k];
                                if (vi.format == 0)
                                {
                                    // unbound: one input per declaration element (an element can be fetched twice)
                                    if (vi.usage != 0xFFFFFFFF && e.usage == vi.usage && e.usageIndex == vi.usageIndex)
                                    {
                                        index = k;
                                        break;
                                    }
                                    continue;
                                }
                                if (e.slot == vi.slot && e.offset == vi.offset && e.format == vi.format &&
                                    e.isSigned == vi.isSigned && e.normalized == vi.normalized && e.expAdjust == vi.expAdjust)
                                {
                                    index = k;
                                    break;
                                }
                            }
                            if (index == pdz.vertexInputs.size())
                                pdz.vertexInputs.push_back(vi);
                            pdz.vertexInputsByAddress.emplace(vi.address, uint32_t(index));
                        }
                        else
                        {
                            noteRead(textureFetch.srcRegister);
                            if (textureFetch.opcode == FetchOpcode::TextureFetch || textureFetch.opcode == FetchOpcode::GetTextureWeights)
                            {
                                written[textureFetch.dstRegister] = true;
                                pdz.textureSlots[textureFetch.constIndex] |= 1u << uint32_t(textureFetch.dimension);
                                if (!isPixelShader)
                                    pdz.vertexTextureFetch = true;
                            }
                        }
                    }
                    else
                    {
                        analyseAlu(alu);
                    }
                    sequence >>= 2;
                    instructionCode += 3;
                }
            }
            cfCode += 3;
            instrAddress += 12;
        }
    }

    // interpolator inputs (PS): container declaration when present, else the live-in registers
    uint32_t psInterpolatorMask = 0;
    if (isPixelShader)
    {
        if (shader.fromContainer && !shader.interpolators.empty())
        {
            for (auto& i : shader.interpolators)
                psInterpolatorMask |= 1u << i.reg;
        }
        for (uint32_t reg : pdz.liveInRegisters)
            if (reg < PDZ_INTERPOLATOR_COUNT)
                psInterpolatorMask |= 1u << reg;
    }
    pdz.interpolatorsRead = psInterpolatorMask;

    // ---- vertex input naming
    const bool containerDecl = !isPixelShader && shader.fromContainer && !shader.vertexElements.empty();
    for (size_t k = 0; k < pdz.vertexInputs.size(); k++)
    {
        PdzVertexInput& vi = pdz.vertexInputs[k];
        vi.name = fmt::format("iAttr{}", k);
        vi.semantic = fmt::format("ATTR{}", k);
        vi.convertedName = fmt::format("vAttr{}", k);
        if (vi.format == uint32_t(VertexFormat::Undefined))
        {
            // unbound package vertex shader: fall back to the declaration usage
            vi.hlslType = "float4";
            vi.dxgiFormat = "(unbound: from the vertex declaration)";
            vi.convert = "{}";
            vi.bound = false;
            const pdz::VertexElementDecl* decl = nullptr;
            if (containerDecl)
                for (auto& e : shader.vertexElements)
                    if (e.address == vi.address)
                        decl = &e;
            if (decl != nullptr)
            {
                vi.name = fmt::format("i{}{}", PDZ_USAGE_VARIABLES[decl->usage], decl->usageIndex);
                vi.semantic = fmt::format("{}{}", PDZ_USAGE_SEMANTICS[decl->usage], decl->usageIndex);
                vi.usage = decl->usage;
                vi.usageIndex = decl->usageIndex;
            }
            else
                pdz.unsupported.push_back(fmt::format("unbound vfetch at {} without a declaration", vi.address));
        }
        else
        {
            vi.bound = true;
            inputTypeFor(VertexFormat(vi.format), vi.isSigned, vi.normalized, vi.hlslType, vi.dxgiFormat, vi.convert);
            if (containerDecl)
                for (auto& e : shader.vertexElements)
                    if (e.address == vi.address)
                    {
                        vi.usage = e.usage;
                        vi.usageIndex = e.usageIndex;
                    }
        }
    }

    // ---- emission
    out += include;
    out += '\n';
    println("// XenosRecomp pdz: {} {}, {} ucode dwords{}", isPixelShader ? "pixel" : "vertex",
        shader.fromContainer ? "shader from a 0x102A0E0x container" : "shader from raw microcode", shader.ucodeDwords,
        pdz.simpleControlFlow ? "" : ", structured control flow fallback (pc switch)");
    for (auto& c : shader.constantNames)
        println("// ctab {} = {}{}{}", c.name, "bics"[uint32_t(c.set) & 3], c.registerIndex,
            c.registerCount > 1 ? fmt::format("..{}", c.registerIndex + c.registerCount - 1) : "");

    // literal constants baked into the shader
    for (auto& lit : shader.literals)
    {
        for (uint32_t i = 0; i < (uint32_t(lit.dwords.size()) + 3) / 4; i++)
        {
            uint32_t v[4]{};
            for (uint32_t j = 0; j < 4 && i * 4 + j < lit.dwords.size(); j++)
                v[j] = lit.dwords[i * 4 + j];
            println("static const float4 cl{} = asfloat(uint4(0x{:08X}u, 0x{:08X}u, 0x{:08X}u, 0x{:08X}u));", lit.registerIndex + i, v[0], v[1], v[2], v[3]);
        }
    }
    out += '\n';

    // Sampler variants are only known after pass 2, so the table is inserted at the end (see below).
    const size_t samplerTablePos = out.size();

    out += "[shader(\"";
    out += isPixelShader ? "pixel" : "vertex";
    out += "\")]\n";
    out += "void main(\n";

    std::vector<std::string> params;
    if (isPixelShader)
    {
        params.push_back("\tin float4 iPos : SV_Position");
        // Every interpolator is declared, read or not: D3D12 links the PS input signature to the VS / GS output
        // by register, and the VS always writes TEXCOORD0..15, so a PS reading TEXCOORD0, 1 and 10 alone put
        // TEXCOORD10 in register 3 against the VS's register 11 (CreateGraphicsPipelineState E_INVALIDARG, log 028).
        for (uint32_t i = 0; i < PDZ_INTERPOLATOR_COUNT; i++)
            params.push_back(fmt::format("\tin float4 i{0} : TEXCOORD{0}", i));
        params.push_back("\tin uint iFace : SV_IsFrontFace");
        for (uint32_t i = 0; i < 4; i++)
            if (pdz.colorOutputs & (1u << i))
                params.push_back(fmt::format("\tout float4 oC{0} : SV_Target{0}", i));
        if (pdz.writesDepth)
            params.push_back("\tout float oDepth : SV_Depth");
    }
    else
    {
        for (auto& vi : pdz.vertexInputs)
            params.push_back(fmt::format("\tin {} {} : {}", vi.hlslType, vi.name, vi.semantic));
        params.push_back("\tout float4 oPos : SV_Position");
        for (uint32_t i = 0; i < PDZ_INTERPOLATOR_COUNT; i++)
            params.push_back(fmt::format("\tout float4 o{0} : TEXCOORD{0}", i));
    }
    for (size_t i = 0; i < params.size(); i++)
        println("{}{}", params[i], i + 1 < params.size() ? "," : "");
    out += ")\n{\n";

    if (isPixelShader)
    {
        for (uint32_t i = 0; i < 32; i++)
        {
            if (i < PDZ_INTERPOLATOR_COUNT && (psInterpolatorMask & (1u << i)))
                println("\tfloat4 r{0} = i{0};", i);
            else
                println("\tfloat4 r{} = 0.0;", i);
        }
        for (uint32_t i = 0; i < 4; i++)
            if (pdz.colorOutputs & (1u << i))
                println("\toC{} = 0.0;", i);
        if (pdz.writesDepth)
            out += "\toDepth = 0.0;\n";
        out += "\tfloat4 oPts = 0.0;\n";
    }
    else
    {
        for (auto& vi : pdz.vertexInputs)
            println("\tfloat4 {} = {};", vi.convertedName, fmt::vformat(vi.convert, fmt::make_format_args(vi.name)));
        for (uint32_t i = 0; i < 32; i++)
            println("\tfloat4 r{} = 0.0;", i);
        out += "\toPos = 0.0;\n";
        for (uint32_t i = 0; i < PDZ_INTERPOLATOR_COUNT; i++)
        {
            println("\to{} = 0.0;", i);
            interpolators.emplace(i, fmt::format("o{}", i));
        }
        out += "\tfloat4 oPts = 0.0;\n";
    }
    out += "\tint a0 = 0;\n";
    out += "\tint aL = 0;\n";
    out += "\tbool p0 = false;\n";
    out += "\tfloat ps = 0.0;\n";
    out += "\tfloat texLod = 0.0;\n";
    out += "\tCubeMapData cubeMapData = (CubeMapData)0;\n";

    if (pdz.simpleControlFlow)
    {
        out += '\n';
        indentation = 1;
    }
    else
    {
        out += "\n\tuint pc = 0;\n";
        out += "\twhile (true)\n";
        out += "\t{\n";
        out += "\t\tswitch (pc)\n";
        out += "\t\t{\n";
    }

    // ---- pass 2: emit
    auto cfCode = code;
    uint32_t instrAddress = 0;
    uint32_t pc = 0;
    uint32_t loopDepth = 0;

    while (instrAddress < instrSize)
    {
        CfPair pair;
        readPair(cfCode, pair);

        for (auto& cf : pair.controlFlow)
        {
            if (!pdz.simpleControlFlow)
            {
                indentation = 3;
                println("\t\tcase {}:", pc);
            }
            else
            {
                auto findResult = ifEndLabels.find(pc);
                if (findResult != ifEndLabels.end())
                {
                    for (uint32_t i = 0; i < findResult->second; i++)
                    {
                        --indentation;
                        indent();
                        out += "}\n";
                    }
                }
            }
            ++pc;

            uint32_t address = 0, count = 0, sequence = 0;
            bool shouldReturn = false;
            bool closeCondition = false;

            auto openCondition = [&](const std::string& condition)
            {
                indent();
                println("if ({})", condition);
                indent();
                out += "{\n";
                ++indentation;
                closeCondition = true;
            };

            switch (cf.opcode)
            {
            case ControlFlowOpcode::Exec:
            case ControlFlowOpcode::ExecEnd:
                address = cf.exec.address; count = cf.exec.count; sequence = cf.exec.sequence;
                shouldReturn = cf.opcode == ControlFlowOpcode::ExecEnd;
                break;

            case ControlFlowOpcode::CondExec:
            case ControlFlowOpcode::CondExecEnd:
            case ControlFlowOpcode::CondExecPredClean:
            case ControlFlowOpcode::CondExecPredCleanEnd:
                address = cf.condExec.address; count = cf.condExec.count; sequence = cf.condExec.sequence;
                shouldReturn = cf.opcode == ControlFlowOpcode::CondExecEnd || cf.opcode == ControlFlowOpcode::CondExecPredCleanEnd;
                openCondition(fmt::format("{}boolConst({}u)", cf.condExec.condition ? "" : "!", uint32_t(cf.condExec.boolAddress)));
                break;

            case ControlFlowOpcode::CondExecPred:
            case ControlFlowOpcode::CondExecPredEnd:
                address = cf.condExecPred.address; count = cf.condExecPred.count; sequence = cf.condExecPred.sequence;
                shouldReturn = cf.opcode == ControlFlowOpcode::CondExecPredEnd;
                openCondition(fmt::format("{}p0", cf.condExecPred.condition ? "" : "!"));
                break;

            case ControlFlowOpcode::LoopStart:
            {
                // Hardware semantics (SDK dxbc_translator.cpp ProcessLoopStart/EndInstruction): the loop
                // constant holds count (7:0), aL start (15:8) and a signed step (23:16); a zero count skips
                // the loop; aL is set to start unless the loop is a "repeat" (keeps the current aL); at
                // endloop the count is decremented first, then the loop exits when it reaches zero (or on
                // the predicated break), otherwise aL += step and the body runs again.  Each nesting level
                // (up to 4) saves and restores the outer aL.
                const uint32_t id = cf.loopStart.loopId;
                pdz.loopSites.push_back({ id, cf.loopStart.isRepeat != 0, false });
                if (pdz.simpleControlFlow)
                {
                    indent();
                    println("{{ const uint loopConst{0} = loopConst({1}u); const int aLSaved{0} = aL;", loopDepth, id);
                    indent();
                    if (!cf.loopStart.isRepeat)
                        println("aL = loopStart(loopConst{});", loopDepth);
                    indent();
                    println("[loop] for (uint loopCount{0} = loopCount(loopConst{0}); loopCount{0} != 0u; )", loopDepth);
                    indent();
                    out += "{\n";
                    ++indentation;
                    ++loopDepth;
                }
                else
                {
                    println("\t\t\tif (loopCount(loopConst({}u)) == 0u) {{ pc = {}; continue; }}", id, uint32_t(cf.loopStart.address));
                    println("\t\t\tloopCounts[loopLevel] = loopCount(loopConst({}u));", id);
                    out += "\t\t\taLStack[loopLevel] = aL;\n";
                    out += "\t\t\tloopLevel = (loopLevel + 1u) & 3u;\n";
                    if (!cf.loopStart.isRepeat)
                        println("\t\t\taL = loopStart(loopConst({}u));", id);
                    pdz.usesLoopInSwitch = true;
                }
                break;
            }

            case ControlFlowOpcode::LoopEnd:
            {
                const uint32_t id = cf.loopEnd.loopId;
                if (pdz.simpleControlFlow)
                {
                    if (loopDepth == 0)
                    {
                        pdz.unsupported.push_back("endloop without loop");
                        break;
                    }
                    --loopDepth;
                    indent();
                    if (cf.loopEnd.isPredicatedBreak)
                        println("if (--loopCount{} == 0u || {}p0) break;", loopDepth, cf.loopEnd.condition ? "" : "!");
                    else
                        println("if (--loopCount{} == 0u) break;", loopDepth);
                    indent();
                    println("aL += loopStep(loopConst{});", loopDepth);
                    --indentation;
                    indent();
                    out += "}\n";
                    indent();
                    println("aL = aLSaved{}; }}", loopDepth);
                }
                else
                {
                    if (cf.loopEnd.isPredicatedBreak)
                        println("\t\t\tif (--loopCounts[(loopLevel - 1u) & 3u] != 0u && !({}p0))", cf.loopEnd.condition ? "" : "!");
                    else
                        out += "\t\t\tif (--loopCounts[(loopLevel - 1u) & 3u] != 0u)\n";
                    out += "\t\t\t{\n";
                    println("\t\t\t\taL += loopStep(loopConst({}u));", id);
                    println("\t\t\t\tpc = {};", uint32_t(cf.loopEnd.address));
                    out += "\t\t\t\tcontinue;\n";
                    out += "\t\t\t}\n";
                    out += "\t\t\tloopLevel = (loopLevel - 1u) & 3u;\n";
                    out += "\t\t\taL = aLStack[loopLevel];\n";
                }
                if (!pdz.loopSites.empty())
                    pdz.loopSites.back().predicatedBreak = cf.loopEnd.isPredicatedBreak != 0;
                break;
            }

            case ControlFlowOpcode::CondJmp:
            {
                if (cf.condJmp.isUnconditional)
                {
                    println("\t\t\tpc = {};", uint32_t(cf.condJmp.address));
                    out += "\t\t\tcontinue;\n";
                }
                else
                {
                    std::string condition;
                    if (cf.condJmp.isPredicated)
                        condition = fmt::format("{}p0", (cf.condJmp.condition ^ pdz.simpleControlFlow) ? "" : "!");
                    else
                        condition = fmt::format("{}boolConst({}u)", (cf.condJmp.condition ^ pdz.simpleControlFlow) ? "" : "!", uint32_t(cf.condJmp.boolAddress));

                    indent();
                    println("if ({})", condition);
                    if (pdz.simpleControlFlow)
                    {
                        indent();
                        out += "{\n";
                        ++indentation;
                    }
                    else
                    {
                        out += "\t\t\t{\n";
                        println("\t\t\t\tpc = {};", uint32_t(cf.condJmp.address));
                        out += "\t\t\t\tcontinue;\n";
                        out += "\t\t\t}\n";
                    }
                }
                break;
            }

            case ControlFlowOpcode::Alloc:
            case ControlFlowOpcode::Nop:
            case ControlFlowOpcode::MarkVsFetchDone:
                break;

            default:
                indent();
                println("// unsupported control flow opcode {}", uint32_t(cf.opcode));
                break;
            }

            auto instructionCode = code + address * 3;
            for (uint32_t i = 0; i < count; i++)
            {
                union
                {
                    VertexFetchInstruction vertexFetch;
                    TextureFetchInstruction textureFetch;
                    AluInstruction alu;
                    struct { uint32_t code0, code1, code2; };
                };
                code0 = instructionCode[0];
                code1 = instructionCode[1];
                code2 = instructionCode[2];

                if ((sequence & 0x1) != 0)
                {
                    if (vertexFetch.opcode == FetchOpcode::VertexFetch)
                        recompilePdzVertexFetch(vertexFetch, address + i);
                    else
                        recompilePdzTextureFetch(textureFetch);
                }
                else
                {
                    recompile(alu);
                }
                sequence >>= 2;
                instructionCode += 3;
            }

            if (shouldReturn)
            {
                if (isPixelShader)
                {
                    if (pdz.colorOutputs & 1)
                    {
                        indent();
                        out += "alphaTest(oC0.w);\n";
                    }
                }
                else
                {
                    indent();
                    out += "oPos = pdzHostPosition(oPos);\n";
                }
                indent();
                out += pdz.simpleControlFlow ? "return;\n" : "break;\n";
            }

            if (closeCondition)
            {
                --indentation;
                indent();
                out += "}\n";
            }
        }

        cfCode += 3;
        instrAddress += 12;
    }

    if (!pdz.simpleControlFlow)
    {
        out += "\t\t\tbreak;\n";
        out += "\t\t}\n";
        out += "\t\tbreak;\n";
        out += "\t}\n";
    }
    else if (loopDepth != 0)
    {
        pdz.unsupported.push_back("loop not closed");
    }

    out += "}\n";

    {
        // g_Samplers[k] = fetch constant slot + per-instruction overrides (point/linear/basemap, or the fetch
        // constant's own value); the renderer resolves each entry to a D3D12 sampler when it binds the shader.
        static const char* const FILTERS[] = { "point", "linear", "basemap", "fetch" };
        static const char* const ANISO[] = { "off", "1x", "2x", "4x", "8x", "16x", "?", "fetch" };
        std::string table;
        for (size_t i = 0; i < pdz.samplerVariants.size(); i++)
        {
            auto& v = pdz.samplerVariants[i];
            table += fmt::format("// sampler {}: slot {} mag={} min={} mip={} aniso={} volMag={} volMin={} code=0x{:04x}\n", i, v.slot,
                FILTERS[v.magFilter & 3], FILTERS[v.minFilter & 3], FILTERS[v.mipFilter & 3], ANISO[v.anisoFilter & 7],
                FILTERS[v.volMagFilter & 3], FILTERS[v.volMinFilter & 3], pdzSamplerCode(v));
        }
        out.insert(samplerTablePos, table);
    }

    if (!pdz.simpleControlFlow && pdz.usesLoopInSwitch)
    {
        // declare the loop counter used by the switch model
        size_t pos = out.find("\tuint pc = 0;\n");
        if (pos != std::string::npos)
            out.insert(pos, "\tuint loopCounts[4] = { 0u, 0u, 0u, 0u };\n\tint aLStack[4] = { 0, 0, 0, 0 };\n\tuint loopLevel = 0u;\n");
    }

    pdzBuildJson(shader);
}

void ShaderRecompiler::pdzBuildJson(const pdz::Shader& shader)
{
    std::string& j = pdzJson;
    j.clear();
    j += "{\n";
    j += fmt::format("  \"type\": \"{}\",\n", isPixelShader ? "ps" : "vs");
    j += fmt::format("  \"source\": \"{}\",\n", shader.fromContainer ? "container" : "raw");
    j += fmt::format("  \"ucode_dwords\": {},\n", shader.ucodeDwords);
    j += fmt::format("  \"simple_control_flow\": {},\n", pdz.simpleControlFlow ? "true" : "false");

    auto listU = [](const std::set<uint32_t>& s)
    {
        std::string r = "[";
        bool first = true;
        for (uint32_t v : s)
        {
            if (!first) r += ", ";
            first = false;
            r += fmt::format("{}", v);
        }
        return r + "]";
    };

    j += fmt::format("  \"float_constants_used\": {},\n", listU(pdz.floatConstantsUsed));
    j += fmt::format("  \"uses_relative_constants\": {},\n", pdz.usesRelativeConstants ? "true" : "false");
    j += fmt::format("  \"writes_a0\": {},\n", pdz.writesA0 ? "true" : "false");
    j += fmt::format("  \"bool_constants_used\": {},\n", listU(pdz.boolConstantsUsed));
    j += fmt::format("  \"loop_constants_used\": {},\n", listU(pdz.loopConstantsUsed));
    j += "  \"loops\": [";
    for (size_t i = 0; i < pdz.loopSites.size(); i++)
        j += fmt::format("{}{{\"constant\": {}, \"repeat\": {}, \"predicated_break\": {}}}", i ? ", " : "",
            pdz.loopSites[i].constant, pdz.loopSites[i].repeat ? "true" : "false", pdz.loopSites[i].predicatedBreak ? "true" : "false");
    j += "],\n";

    j += "  \"literals\": [";
    bool first = true;
    for (auto& lit : shader.literals)
    {
        for (uint32_t i = 0; i < (uint32_t(lit.dwords.size()) + 3) / 4; i++)
        {
            if (!first) j += ", ";
            first = false;
            j += fmt::format("{}", lit.registerIndex + i);
        }
    }
    j += "],\n";

    if (!shader.constantNames.empty())
    {
        j += "  \"ctab\": [\n";
        for (size_t i = 0; i < shader.constantNames.size(); i++)
        {
            auto& c = shader.constantNames[i];
            j += fmt::format("    {{\"name\": \"{}\", \"set\": \"{}\", \"register\": {}, \"count\": {}}}{}\n", jsonEscape(c.name),
                "bics"[uint32_t(c.set) & 3], c.registerIndex, c.registerCount, i + 1 < shader.constantNames.size() ? "," : "");
        }
        j += "  ],\n";
    }

    if (!isPixelShader)
    {
        j += "  \"vertex_inputs\": [\n";
        for (size_t i = 0; i < pdz.vertexInputs.size(); i++)
        {
            auto& vi = pdz.vertexInputs[i];
            j += fmt::format("    {{\"semantic\": \"{}\", \"hlsl_type\": \"{}\", \"bound\": {}, \"slot\": {}, \"offset_dwords\": {}, \"stride_dwords\": {}, "
                "\"format\": \"{}\", \"format_code\": {}, \"signed\": {}, \"normalized\": {}, \"exp_adjust\": {}, \"dxgi_format\": \"{}\", \"vfetch_address\": {}, \"mini\": {}{}}}{}\n",
                vi.semantic, vi.hlslType, vi.bound ? "true" : "false", vi.slot, vi.offset, vi.stride, formatInfo(VertexFormat(vi.format)).name, vi.format,
                vi.isSigned ? "true" : "false", vi.normalized ? "true" : "false", vi.expAdjust, vi.dxgiFormat, vi.address, vi.isMini ? "true" : "false",
                vi.usage != 0xFFFFFFFF ? fmt::format(", \"decl_usage\": \"{}{}\"", PDZ_USAGE_SEMANTICS[vi.usage & 15], vi.usageIndex) : "",
                i + 1 < pdz.vertexInputs.size() ? "," : "");
        }
        j += "  ],\n";
        j += fmt::format("  \"interpolators_written\": \"0x{:04x}\",\n", pdz.interpolatorsWritten);
        j += fmt::format("  \"writes_position\": {},\n", pdz.writesPosition ? "true" : "false");
        j += fmt::format("  \"writes_point_size\": {},\n", pdz.writesPointSize ? "true" : "false");
        j += fmt::format("  \"vertex_texture_fetch\": {},\n", pdz.vertexTextureFetch ? "true" : "false");
    }
    else
    {
        j += fmt::format("  \"interpolators_read\": \"0x{:04x}\",\n", pdz.interpolatorsRead);
        j += fmt::format("  \"color_outputs\": \"0x{:x}\",\n", pdz.colorOutputs);
        j += fmt::format("  \"writes_depth\": {},\n", pdz.writesDepth ? "true" : "false");
        j += fmt::format("  \"uses_kill\": {},\n", pdz.usesKill ? "true" : "false");
        if (shader.fromContainer)
        {
            // bits 0..3 of the container mask are oC0..oC3 like the exports; 0x20 is set on two shaders and
            // its meaning is unknown, so only the low four bits are compared
            j += fmt::format("  \"container_output_mask\": \"0x{:x}\",\n", shader.outputMaskRaw);
            j += fmt::format("  \"container_output_mask_matches\": {},\n", (shader.outputMaskRaw & 0xF) == pdz.colorOutputs ? "true" : "false");
            if ((shader.outputMaskRaw & 0xF) != pdz.colorOutputs)
                pdz.unsupported.push_back(fmt::format("container output mask {:#x} differs from the exports {:#x}", shader.outputMaskRaw, pdz.colorOutputs));
        }
    }

    static const char* const DIMENSION_NAMES[] = { "1D", "2D", "3D", "Cube" };
    j += "  \"textures\": [";
    first = true;
    for (auto& [slot, dims] : pdz.textureSlots)
    {
        for (uint32_t d = 0; d < 4; d++)
        {
            if (!(dims & (1u << d)))
                continue;
            if (!first) j += ", ";
            first = false;
            j += fmt::format("{{\"slot\": {}, \"dimension\": \"{}\", \"space\": {}}}", slot, DIMENSION_NAMES[d], d <= 1 ? 0 : d - 1);
        }
    }
    j += "],\n";

    static const char* const FILTER_NAMES[] = { "point", "linear", "basemap", "fetch" };
    static const char* const ANISO_NAMES[] = { "disabled", "1", "2", "4", "8", "16", "?", "fetch" };
    j += "  \"samplers\": [\n";
    for (size_t i = 0; i < pdz.samplerVariants.size(); i++)
    {
        auto& v = pdz.samplerVariants[i];
        const bool overrides = (v.magFilter & 3) != 3 || (v.minFilter & 3) != 3 || (v.mipFilter & 3) != 3 || (v.anisoFilter & 7) != 7 ||
            (v.volMagFilter & 3) != 3 || (v.volMinFilter & 3) != 3;
        j += fmt::format("    {{\"index\": {}, \"slot\": {}, \"dimension\": \"{}\", \"mag\": \"{}\", \"min\": \"{}\", \"mip\": \"{}\", \"aniso\": \"{}\", \"vol_mag\": \"{}\", \"vol_min\": \"{}\", \"overrides\": {}, \"code\": {}}}{}\n",
            i, v.slot, DIMENSION_NAMES[v.dimension & 3], FILTER_NAMES[v.magFilter & 3], FILTER_NAMES[v.minFilter & 3], FILTER_NAMES[v.mipFilter & 3],
            ANISO_NAMES[v.anisoFilter & 7], FILTER_NAMES[v.volMagFilter & 3], FILTER_NAMES[v.volMinFilter & 3], overrides ? "true" : "false",
            pdzSamplerCode(v), i + 1 < pdz.samplerVariants.size() ? "," : "");
    }
    j += "  ],\n";
    j += fmt::format("  \"uses_register_lod\": {},\n", pdz.usesRegisterLod ? "true" : "false");
    j += "  \"texture_fetches\": [";
    for (size_t i = 0; i < pdz.textureFetchSites.size(); i++)
    {
        auto& s = pdz.textureFetchSites[i];
        j += fmt::format("{}{{\"slot\": {}, \"dimension\": \"{}\", \"unnormalized\": {}, \"lod\": \"{}\", \"lod_bias\": {}, \"offsets\": {}}}", i ? ", " : "",
            s.slot, DIMENSION_NAMES[s.dimension & 3], s.unnormalized ? "true" : "false",
            s.computedLod ? "computed" : (s.registerLod ? "register" : "zero"), s.lodBias, s.offsets ? "true" : "false");
    }
    j += "],\n";

    j += "  \"unsupported\": [";
    first = true;
    std::set<std::string> unique(pdz.unsupported.begin(), pdz.unsupported.end());
    for (auto& u : unique)
    {
        if (!first) j += ", ";
        first = false;
        j += fmt::format("\"{}\"", jsonEscape(u));
    }
    j += "]\n";
    j += "}\n";
}
