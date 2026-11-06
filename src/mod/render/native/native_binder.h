// Native renderer (stage 1): the vertex-declaration binder. Reproduces what the Xbox 360 D3D runtime does
// when it binds a package vertex shader (unbound vfetch instructions + a container table mapping each vfetch
// to a D3DDECLUSAGE / index) to a vertex declaration and the stream strides. Verified byte-for-byte against
// the 534 bound variants of the emulator's .xsh cache (PDZRenderResearch\native\tools\binder\binder.py,
// BINDER_NOTES.md). Cold path only (a new (shader, declaration) pair).
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "../shadow_record.h"

namespace pdz::native::binder
{
  // One declaration element, decoded from the raw big-endian D3DVERTEXELEMENT9 the ring keeps
  // (WORD Stream, WORD Offset, DWORD Type, BYTE Method, BYTE Usage, BYTE UsageIndex, BYTE pad). Type is the
  // Xbox 360 D3DDECLTYPE: bits 0..5 GPU vertex format, bit 8 signed, bit 9 num_format (1 = integer / float,
  // 0 = normalized), bits 10..21 the 12-bit destination swizzle (FLOAT3 0x2A23B9 -> xyz1, D3DCOLOR 0x182886
  // -> zyxw).
  struct Element
  {
    uint32_t usage = 0, index = 0, stream = 0, offset_bytes = 0;
    uint32_t type = 0;
    uint32_t fmt = 0, is_signed = 0, integer = 0, type_swz = 0;
  };
  // Container element table entry: the instruction address of a vfetch and the usage it fetches.
  struct TableEntry
  {
    uint32_t addr = 0, usage = 0, index = 0, hi = 0;
  };

  uint32_t ParseDeclaration(const shadow::DeclEntry &decl, Element *out, uint32_t max_out);
  // The XDK container (magic 0x102A0E01) that holds this shader's microcode: scanned backwards from the
  // microcode address for a header whose size field points at it. 0 when not found.
  uint32_t FindContainer(const uint8_t *base, const shadow::ShaderEntry &entry);
  uint32_t ReadTable(const uint8_t *base, uint32_t container, TableEntry *out, uint32_t max_out);

  // Bind `parent` (guest-order microcode bytes, a multiple of 12) with the table, the declaration and the
  // stream strides (bytes, indexed by D3D stream). Writes the bound guest-order bytes to `out`.
  bool Bind(const uint8_t *parent, uint32_t bytes, const TableEntry *table, uint32_t table_count,
            const Element *decl, uint32_t decl_count, const uint32_t *stream_stride_bytes, uint32_t stream_count,
            std::vector<uint8_t> &out, std::string *why);
} // namespace pdz::native::binder
