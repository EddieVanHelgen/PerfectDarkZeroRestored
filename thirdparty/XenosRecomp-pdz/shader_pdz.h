#pragma once

// Legacy Xbox 360 XDK shader container (magic 0x102A0E0x) and raw-microcode input for the pdz branch.
//
// Layout (all big-endian), worked out empirically from Perfect Dark Zero's 1,637 static containers
// (XDK compiler 2.0.2112.0) and the 16 executable-embedded ones that still carry a D3DX constant table:
//
//   +0x00 u32 magic            0x102A0E01 = vertex shader, 0x102A0E00 = pixel shader
//   +0x04 u32 headerSize       size of the header block; the microcode starts at +headerSize
//   +0x08 u32 ucodeSize        microcode byte count (multiple of 12: CF pairs and ALU / fetch instructions)
//   +0x0C u32 literalOffset    0, or 0x18: the literal-constant block
//   +0x10 u32 ctabOffset       0, or offset of {u32 size; D3DXSHADER_CONSTANTTABLE} (version 0xFFFE0300 /
//                              0xFFFF0300, creator "Microsoft (R) Xbox 360 Shader Compiler 2.0.2112.0");
//                              only the executable's UI shaders have one, every package shader has 0
//   +0x14 u32 programOffset    the program-info block (runs to headerSize)
//
// Literal block (at literalOffset, ends at ctabOffset or programOffset):
//   u32 groupMask              bit mask (units unknown, D3D dirty tracking); not needed
//   u32 zero[3]
//   u32 payloadSize            bytes that follow this 24-byte header (incl. the zero terminator and padding)
//   u32 (uninitialised)
//   records, terminated by a zero dword:
//     u32 header               bits 31..16: byte offset of the destination in the GPU constant file,
//                              bits 15..0: payload dword count.  The constant file starts with the 32
//                              fetch constants (6 dwords each = 0x300 bytes), the 512 float4 ALU constants
//                              follow, so float register n is at 0x300 + 16*n; pixel shader registers are
//                              256 + n (register 0 of a pixel shader = 0x1300).  Examples: 0x1300000C =
//                              PS c0..c2 (12 dwords), 0x04100004 = VS c17, 0x12F00004 = VS c255,
//                              0x22E00008 = PS c254..c255.
//     u32 payload[count]       IEEE floats
//   The D3D runtime uploads these when the shader is set, so the translator has to bake them in.
//
// Program-info block (at programOffset, to headerSize), dword indexed:
//   [0]  flags / register count word (PS: 0x1000_0000 | 0x2000_0000 | temps << 8; VS: 0x0001_0002 ...),
//        not needed for translation
//   [1]  PS: 4 / 0x606 / 0 (unknown), VS: 0
//   [2], [3] zero (one PS: 0x08000080)
//   [4]  SQ_PROGRAM_CNTL-like word (0x821, 0xC21, 0x3C84 ...); not needed
//   [5]  uninitialised garbage (ASCII fragments)
//   [6]  PS: interpolator mask, twice (low and high 16 bits, e.g. 0x000F000F); VS: 1
//   [7]  PS: output mask: bit0 oC0, bit1 oC1 (.. bit3 oC3), bit5 = the shader kills pixels; VS: vertex element count
//   [8]  PS: interpolators start here; VS: export-patch entry count
//   [9]  VS: 0
//   [10] VS: 0x290 (constant)
//   [11] VS: vertex elements (count = [7]), then interpolators, then the patch list (count = [8])
//
//   Interpolator entry: bits 3..0 usage index, 7..4 D3DDECLUSAGE, 11..8 register (PS input r#, VS export o#),
//                       15..12 component mask; VS entries carry extra bits in 23..16 (unknown).
//   Vertex element (VS): bits 11..0 microcode instruction address of the vfetch, 15..12 D3DDECLUSAGE,
//                       23..20: 1 for POSITION, 2 or 0 for others (unknown); bits 19..16 usage index
//                       (never set in PDZ's declarations).
//   Patch list entry (VS): bits 11..0 instruction address of an exporting ALU instruction, bit 12 set on some.
//
// The vfetch instructions of every package vertex shader are unbound (format 0, stride 0, offset 0); the D3D
// runtime fills ten fields from the vertex declaration at bind time, and the emulator's .xsh cache holds
// those bound variants as raw microcode.  Hence the raw-microcode input mode: a file holding only the ucode
// dwords in guest (big-endian) order plus the shader type given on the command line.

#include "constant_table.h"

namespace pdz
{

enum class Usage : uint32_t
{
    Position = 0, BlendWeight, BlendIndices, Normal, PointSize, TexCoord, Tangent, Binormal, TessFactor,
    PositionT, Color, Fog, Depth, Sample
};

struct LiteralRecord
{
    uint32_t registerIndex;            // float4 register, shader-relative (PS 0..223)
    std::vector<uint32_t> dwords;       // raw IEEE bits, 4 per register
};

struct InterpolatorDecl
{
    uint32_t usageIndex, usage, reg, mask, raw;
};

struct VertexElementDecl
{
    uint32_t address, usage, usageIndex, raw;
};

struct ConstantName
{
    std::string name;
    RegisterSet set;
    uint32_t registerIndex, registerCount;
};

struct Shader
{
    bool isPixelShader = false;
    bool fromContainer = false;
    const be<uint32_t>* ucode = nullptr;        // guest order dwords
    uint32_t ucodeDwords = 0;
    std::vector<LiteralRecord> literals;
    std::vector<InterpolatorDecl> interpolators; // container only
    std::vector<VertexElementDecl> vertexElements; // container VS only
    std::vector<uint32_t> patchList;            // container VS only
    uint32_t outputMaskRaw = 0;                 // container PS only ([7])
    std::vector<ConstantName> constantNames;    // container with CTAB only
    std::string ctabCreator, ctabTarget;
    uint32_t programWord0 = 0, programWord4 = 0;
};

static inline uint32_t rd32(const uint8_t* p)
{
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

static inline uint32_t rd16(const uint8_t* p)
{
    return (uint32_t(p[0]) << 8) | uint32_t(p[1]);
}

static inline bool isLegacyContainer(const uint8_t* data, size_t size)
{
    return size >= 0x18 && (rd32(data) & 0xFFFFFFFE) == 0x102A0E00;
}

// Parses a legacy container. Returns false (with a message in `error`) when the data is not one.
static inline bool parseLegacyContainer(const uint8_t* data, size_t size, Shader& out, std::string& error)
{
    if (!isLegacyContainer(data, size))
    {
        error = "not a 0x102A0E0x container";
        return false;
    }

    const uint32_t magic = rd32(data);
    const uint32_t headerSize = rd32(data + 4);
    const uint32_t ucodeSize = rd32(data + 8);
    const uint32_t literalOffset = rd32(data + 0xC);
    const uint32_t ctabOffset = rd32(data + 0x10);
    const uint32_t programOffset = rd32(data + 0x14);

    if (headerSize < 0x18 || ucodeSize == 0 || (ucodeSize % 12) != 0 || size_t(headerSize) + ucodeSize > size)
    {
        error = fmt::format("bad container sizes: header {:#x} ucode {:#x} file {:#x}", headerSize, ucodeSize, size);
        return false;
    }

    out = {};
    out.isPixelShader = (magic & 1) == 0;
    out.fromContainer = true;
    out.ucode = reinterpret_cast<const be<uint32_t>*>(data + headerSize);
    out.ucodeDwords = ucodeSize / 4;

    const uint32_t psRegisterBase = out.isPixelShader ? 256 : 0;

    if (literalOffset != 0)
    {
        const uint32_t end = ctabOffset != 0 ? ctabOffset : programOffset;
        if (literalOffset + 24 > end || end > headerSize)
        {
            error = "bad literal block bounds";
            return false;
        }
        uint32_t pos = literalOffset + 24;
        while (pos + 4 <= end)
        {
            const uint32_t header = rd32(data + pos);
            if (header == 0)
                break;
            const uint32_t byteOffset = header >> 16;
            const uint32_t count = header & 0xFFFF;
            pos += 4;
            if (pos + count * 4 > end || byteOffset < 0x300 || (byteOffset & 0xF) != 0)
            {
                error = fmt::format("bad literal record {:#x}", header);
                return false;
            }
            LiteralRecord rec;
            const uint32_t fileRegister = (byteOffset - 0x300) / 16;
            if (fileRegister < psRegisterBase)
            {
                error = fmt::format("literal record {:#x} targets register {} below the shader base", header, fileRegister);
                return false;
            }
            rec.registerIndex = fileRegister - psRegisterBase;
            for (uint32_t i = 0; i < count; i++)
                rec.dwords.push_back(rd32(data + pos + i * 4));
            pos += count * 4;
            out.literals.push_back(std::move(rec));
        }
    }

    if (ctabOffset != 0)
    {
        // {u32 size} then D3DXSHADER_CONSTANTTABLE; all offsets relative to the table itself
        const uint8_t* table = data + ctabOffset + 4;
        if (ctabOffset + 4 + sizeof(ConstantTable) > headerSize)
        {
            error = "bad constant table bounds";
            return false;
        }
        const uint32_t creator = rd32(table + 4);
        const uint32_t version = rd32(table + 8);
        const uint32_t count = rd32(table + 12);
        const uint32_t infoOffset = rd32(table + 16);
        const uint32_t target = rd32(table + 24);
        if (version != 0xFFFE0300 && version != 0xFFFF0300)
        {
            error = fmt::format("unexpected constant table version {:#x}", version);
            return false;
        }
        auto cstr = [&](uint32_t off) -> std::string
        {
            if (ctabOffset + 4 + off >= headerSize)
                return {};
            const char* s = reinterpret_cast<const char*>(table + off);
            return std::string(s, strnlen(s, headerSize - (ctabOffset + 4 + off)));
        };
        out.ctabCreator = cstr(creator);
        out.ctabTarget = cstr(target);
        for (uint32_t i = 0; i < count; i++)
        {
            const uint8_t* info = table + infoOffset + i * sizeof(ConstantInfo);
            if (uint32_t(info - data) + sizeof(ConstantInfo) > headerSize)
                break;
            ConstantName c;
            c.name = cstr(rd32(info));
            c.set = RegisterSet(rd16(info + 4));
            c.registerIndex = rd16(info + 6);
            c.registerCount = rd16(info + 8);
            out.constantNames.push_back(std::move(c));
        }
    }

    if (programOffset != 0 && programOffset + (out.isPixelShader ? 8 : 11) * 4 <= headerSize)
    {
        auto word = [&](uint32_t i) { return rd32(data + programOffset + i * 4); };
        const uint32_t words = (headerSize - programOffset) / 4;
        out.programWord0 = word(0);
        out.programWord4 = word(4);
        auto decodeInterpolator = [](uint32_t raw)
        {
            return InterpolatorDecl{ raw & 0xF, (raw >> 4) & 0xF, (raw >> 8) & 0xF, (raw >> 12) & 0xF, raw };
        };
        if (out.isPixelShader)
        {
            out.outputMaskRaw = word(7);
            const uint32_t count = __builtin_popcount(word(6) & 0xFFFF);
            for (uint32_t i = 0; i < count && 8 + i < words; i++)
                out.interpolators.push_back(decodeInterpolator(word(8 + i)));
        }
        else
        {
            const uint32_t elementCount = word(7);
            const uint32_t patchCount = word(8);
            uint32_t pos = 11;
            for (uint32_t i = 0; i < elementCount && pos < words; i++, pos++)
            {
                const uint32_t raw = word(pos);
                out.vertexElements.push_back({ raw & 0xFFF, (raw >> 12) & 0xF, (raw >> 16) & 0xF, raw });
            }
            const uint32_t interpolatorCount = (words > pos + patchCount) ? words - pos - patchCount : 0;
            for (uint32_t i = 0; i < interpolatorCount; i++, pos++)
                out.interpolators.push_back(decodeInterpolator(word(pos)));
            for (uint32_t i = 0; i < patchCount && pos < words; i++, pos++)
                out.patchList.push_back(word(pos));
        }
    }

    return true;
}

// Raw microcode: the whole file is guest-order dwords.
static inline bool parseRawUcode(const uint8_t* data, size_t size, bool isPixelShader, Shader& out, std::string& error)
{
    if (size == 0 || (size % 12) != 0)
    {
        error = fmt::format("raw microcode size {} is not a multiple of 12", size);
        return false;
    }
    out = {};
    out.isPixelShader = isPixelShader;
    out.ucode = reinterpret_cast<const be<uint32_t>*>(data);
    out.ucodeDwords = uint32_t(size / 4);
    return true;
}

}
