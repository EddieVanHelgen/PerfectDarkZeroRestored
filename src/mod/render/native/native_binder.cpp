// Native renderer (stage 1): the vertex-declaration binder (native_binder.h). A port of
// PDZRenderResearch\native\tools\binder\binder.py, which reproduces all 534 .xsh bound variants.
#include "native_binder.h"

#include <algorithm>
#include <cstring>

namespace pdz::native::binder
{
  namespace
  {
    inline uint32_t Bits(uint32_t v, uint32_t lo, uint32_t n) { return (v >> lo) & ((1u << n) - 1u); }
    inline uint32_t Be32(const uint8_t *p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
    inline void PutBe32(uint8_t *p, uint32_t v)
    {
      p[0] = uint8_t(v >> 24);
      p[1] = uint8_t(v >> 16);
      p[2] = uint8_t(v >> 8);
      p[3] = uint8_t(v);
    }

    // Control flow: two 48-bit instructions per 3 dwords; exec-type opcodes carry an instruction address
    // (bits 0..11), a count (12..14) and 2 bits per instruction in 16..27 (bit 0 = fetch).
    constexpr uint32_t kExecOps = (1u << 1) | (1u << 2) | (1u << 3) | (1u << 4) | (1u << 5) | (1u << 6) | (1u << 13) | (1u << 14);
    struct Cf
    {
      uint32_t w0, w1;
    };
    inline void UnpackCf(uint32_t d0, uint32_t d1, uint32_t d2, Cf out[2])
    {
      out[0] = {d0, d1 & 0xFFFFu};
      out[1] = {((d1 >> 16) | (d2 << 16)) & 0xFFFFFFFFu, d2 >> 16};
    }
    inline bool IsExec(const Cf &cf) { return (kExecOps >> Bits(cf.w1, 12, 4)) & 1u; }

    uint32_t CfBound(const uint32_t *dw, uint32_t count)
    {
      uint32_t bound = count / 3;
      for (uint32_t i = 0; i < bound; ++i)
      {
        Cf cf[2];
        UnpackCf(dw[3 * i], dw[3 * i + 1], dw[3 * i + 2], cf);
        for (const Cf &c : cf)
        {
          if (IsExec(c))
          {
            bound = std::min(bound, Bits(c.w0, 0, 12));
          }
        }
      }
      return bound;
    }

    // Runs of vfetch instructions with consecutive addresses (an exec clause holds at most six instructions, so
    // a run spans clause boundaries; an ALU instruction in between ends the run).
    void FetchRuns(const uint32_t *dw, uint32_t count, std::vector<std::vector<uint32_t>> &runs)
    {
      runs.clear();
      const uint32_t bound = CfBound(dw, count);
      std::vector<uint32_t> run;
      for (uint32_t i = 0; i < bound; ++i)
      {
        Cf cf[2];
        UnpackCf(dw[3 * i], dw[3 * i + 1], dw[3 * i + 2], cf);
        for (const Cf &c : cf)
        {
          if (!IsExec(c))
          {
            continue;
          }
          const uint32_t addr = Bits(c.w0, 0, 12), n = Bits(c.w0, 12, 3), seq = Bits(c.w0, 16, 12);
          for (uint32_t k = 0; k < n; ++k)
          {
            const uint32_t idx = addr + k;
            if (3 * idx + 3 > count)
            {
              continue;
            }
            const bool fetch = ((seq >> (2 * k)) & 1u) != 0;
            if (fetch && Bits(dw[3 * idx], 0, 5) == 0)
            {
              run.push_back(idx);
            }
            else if (!run.empty())
            {
              runs.push_back(run);
              run.clear();
            }
          }
        }
      }
      if (!run.empty())
      {
        runs.push_back(run);
      }
      // merge runs with contiguous addresses
      std::vector<std::vector<uint32_t>> merged;
      for (auto &r : runs)
      {
        if (!merged.empty() && r.front() == merged.back().back() + 1)
        {
          merged.back().insert(merged.back().end(), r.begin(), r.end());
        }
        else
        {
          merged.push_back(r);
        }
      }
      runs.swap(merged);
    }

    enum : uint32_t
    {
      kSwX = 0,
      kSwY = 1,
      kSwZ = 2,
      kSwW = 3,
      kSwKeep = 7
    };
    // result[c] = type_swz[parent_swz[c]] for component selectors; constants / keep pass through.
    uint32_t Compose(uint32_t parent_swz, uint32_t type_swz)
    {
      uint32_t out = 0;
      for (uint32_t c = 0; c < 4; ++c)
      {
        const uint32_t p = (parent_swz >> (3 * c)) & 7u;
        const uint32_t v = p <= kSwW ? ((type_swz >> (3 * p)) & 7u) : p;
        out |= v << (3 * c);
      }
      return out;
    }
    bool Is16Bit(uint32_t fmt) { return fmt == 25 || fmt == 26 || fmt == 31 || fmt == 32; }
    // The swizzle the hardware fetch needs: the type's swizzle, with x<->y / z<->w swapped for 16-bit formats
    // (the stream's 32-bit endian swap leaves 16-bit halves exchanged).
    uint32_t HwSwizzle(const Element &el)
    {
      uint32_t s = el.type_swz;
      if (Is16Bit(el.fmt))
      {
        uint32_t out = 0;
        for (uint32_t c = 0; c < 4; ++c)
        {
          uint32_t v = (s >> (3 * c)) & 7u;
          switch (v)
          {
          case kSwX: v = kSwY; break;
          case kSwY: v = kSwX; break;
          case kSwZ: v = kSwW; break;
          case kSwW: v = kSwZ; break;
          default: break;
          }
          out |= v << (3 * c);
        }
        s = out;
      }
      return s;
    }
    uint32_t FormatDwords(uint32_t fmt)
    {
      switch (fmt)
      {
      case 26: case 32: case 34: case 37: return 2; // 16_16_16_16, 16_16_16_16_FLOAT, 32_32, 32_32_FLOAT
      case 57: return 3;                            // 32_32_32_FLOAT
      case 35: case 38: return 4;                   // 32_32_32_32, 32_32_32_32_FLOAT
      default: return 1;
      }
    }
    constexpr uint32_t kChainWindow = 8; // dwords from the chain start a mini fetch may still end within

    struct Patched
    {
      uint32_t src_addr = 0;
      const Element *el = nullptr;
    };

    void PatchInstruction(uint32_t &i0, uint32_t &i1, uint32_t &i2, const Element &el, uint32_t stride_dwords)
    {
      const uint32_t slot = 95u - el.stream;
      const uint32_t ci = slot / 3, sel = slot % 3;
      i0 = (i0 & ~(0x1Fu << 20) & ~(0x3u << 25) & ~(0x7u << 27)) | (ci << 20) | (sel << 25);
      const uint32_t swz = Compose(Bits(i1, 0, 12), HwSwizzle(el));
      i1 = (i1 & ~0xFFFu & ~(1u << 12) & ~(1u << 13) & ~(0x3Fu << 16) & ~(1u << 30)) | swz | (el.is_signed << 12) |
           (el.integer << 13) | ((el.fmt & 0x3Fu) << 16);
      const uint32_t off_dw = el.offset_bytes / 4;
      i2 = (i2 & ~0xFFu & ~(0x7FFFFFu << 8)) | (stride_dwords & 0xFFu) | ((off_dw & 0x7FFFFFu) << 8);
    }
  } // namespace

  uint32_t ParseDeclaration(const shadow::DeclEntry &decl, Element *out, uint32_t max_out)
  {
    uint32_t n = 0;
    const uint32_t count = decl.count < 16 ? decl.count : 16;
    for (uint32_t e = 0; e < count && n < max_out; ++e)
    {
      const uint8_t *raw = reinterpret_cast<const uint8_t *>(decl.elements_be[e]);
      const uint32_t w0 = Be32(raw), w1 = Be32(raw + 4), w2 = Be32(raw + 8);
      if (w0 == 0x00FF0000u && w2 == 0x00000011u) // D3DDECL_END(): stream 0xFF, type UNUSED
      {
        break;
      }
      Element &el = out[n];
      el.stream = w0 >> 16;
      el.offset_bytes = w0 & 0xFFFFu;
      el.type = w1;
      el.usage = (w2 >> 16) & 0xFFu;
      el.index = (w2 >> 8) & 0xFFu;
      el.fmt = Bits(w1, 0, 6);
      el.is_signed = Bits(w1, 8, 1);
      el.integer = Bits(w1, 9, 1);
      el.type_swz = Bits(w1, 10, 12);
      if (el.stream == 0xFF)
      {
        break;
      }
      ++n;
    }
    return n;
  }

  uint32_t FindContainer(const uint8_t *base, const shadow::ShaderEntry &entry)
  {
    if (!shadow::IsGuestPointer(entry.ucode_src))
    {
      return 0;
    }
    // The header is at most a few KB: magic, header size (= distance to the microcode), ucode size.
    for (uint32_t back = 24; back <= 16384; back += 4)
    {
      const uint32_t p = entry.ucode_src - back;
      if (!shadow::IsGuestPointer(p))
      {
        break;
      }
      const uint32_t magic = shadow::Load32(base, p);
      if ((magic == 0x102A0E01u || magic == 0x102A0E00u) && shadow::Load32(base, p + 4) == back &&
          shadow::Load32(base, p + 8) == entry.ucode_bytes)
      {
        return p;
      }
    }
    return 0;
  }

  uint32_t ReadTable(const uint8_t *base, uint32_t container, TableEntry *out, uint32_t max_out)
  {
    if (!shadow::IsGuestPointer(container))
    {
      return 0;
    }
    const uint32_t header_size = shadow::Load32(base, container + 4);
    const uint32_t program = shadow::Load32(base, container + 0x14);
    if (program == 0 || program + 12 * 4 > header_size)
    {
      return 0;
    }
    const uint32_t count = shadow::Load32(base, container + program + 7 * 4);
    uint32_t n = 0;
    for (uint32_t i = 0; i < count && n < max_out; ++i)
    {
      const uint32_t at = container + program + (11 + i) * 4;
      if (at + 4 > container + header_size)
      {
        break;
      }
      const uint32_t raw = shadow::Load32(base, at);
      out[n++] = {raw & 0xFFFu, (raw >> 12) & 0xFu, (raw >> 16) & 0xFu, (raw >> 20) & 0xFu};
    }
    return n;
  }

  bool Bind(const uint8_t *parent, uint32_t bytes, const TableEntry *table, uint32_t table_count,
            const Element *decl, uint32_t decl_count, const uint32_t *stream_stride_bytes, uint32_t stream_count,
            std::vector<uint8_t> &out, std::string *why)
  {
    if (!parent || bytes == 0 || (bytes % 12) != 0)
    {
      if (why) *why = "microcode size is not a multiple of 12";
      return false;
    }
    const uint32_t count = bytes / 4;
    std::vector<uint32_t> dw(count);
    for (uint32_t i = 0; i < count; ++i)
    {
      dw[i] = Be32(parent + 4 * i);
    }
    std::vector<uint32_t> result = dw;

    auto find_table = [&](uint32_t addr) -> const TableEntry * {
      for (uint32_t i = 0; i < table_count; ++i)
      {
        if (table[i].addr == addr) return &table[i];
      }
      return nullptr;
    };
    auto find_element = [&](const TableEntry &te) -> const Element * {
      for (uint32_t i = 0; i < decl_count; ++i)
      {
        if (decl[i].usage == te.usage && decl[i].index == te.index) return &decl[i];
      }
      return nullptr;
    };
    auto stride_dwords = [&](uint32_t stream) -> uint32_t {
      return stream < stream_count ? stream_stride_bytes[stream] / 4 : 0;
    };

    std::vector<std::vector<uint32_t>> runs;
    FetchRuns(dw.data(), count, runs);
    uint32_t bound_fetches = 0, unbound_fetches = 0;
    for (const std::vector<uint32_t> &run : runs)
    {
      // Groups: a repeated (usage, index) inside the run starts a new group; each group is sorted by
      // (stream, offset) into its own instruction slots.
      std::vector<std::vector<Patched>> groups;
      std::vector<Patched> cur;
      std::vector<uint32_t> seen_keys;
      for (uint32_t a : run)
      {
        const TableEntry *te = find_table(a);
        const Element *el = te ? find_element(*te) : nullptr;
        if (!el)
        {
          ++unbound_fetches;
          if (!cur.empty())
          {
            groups.push_back(cur);
            cur.clear();
            seen_keys.clear();
          }
          continue;
        }
        const uint32_t key = (te->usage << 8) | te->index;
        if (std::find(seen_keys.begin(), seen_keys.end(), key) != seen_keys.end())
        {
          groups.push_back(cur);
          cur.clear();
          seen_keys.clear();
        }
        cur.push_back({a, el});
        seen_keys.push_back(key);
        ++bound_fetches;
      }
      if (!cur.empty())
      {
        groups.push_back(cur);
      }
      // final: position -> (source instruction, element)
      std::vector<std::pair<uint32_t, Patched>> final_order;
      for (std::vector<Patched> &group : groups)
      {
        std::vector<uint32_t> positions;
        for (const Patched &p : group) positions.push_back(p.src_addr);
        std::stable_sort(group.begin(), group.end(), [](const Patched &a, const Patched &b) {
          if (a.el->stream != b.el->stream) return a.el->stream < b.el->stream;
          return a.el->offset_bytes / 4 < b.el->offset_bytes / 4;
        });
        for (size_t i = 0; i < positions.size(); ++i)
        {
          final_order.push_back({positions[i], group[i]});
        }
      }
      if (final_order.empty())
      {
        continue;
      }
      std::sort(final_order.begin(), final_order.end(),
                [](const std::pair<uint32_t, Patched> &a, const std::pair<uint32_t, Patched> &b) { return a.first < b.first; });
      // Chains over the final instruction order: adjacent, same stream, strictly increasing offset, ending
      // within kChainWindow dwords of the chain start. Head: full fetch with prefetch = min(7, end-start-1)
      // when the chain has more than one element; the rest are mini fetches.
      size_t i = 0;
      while (i < final_order.size())
      {
        size_t j = i + 1;
        const Element &head = *final_order[i].second.el;
        const uint32_t start = head.offset_bytes / 4;
        uint32_t last_off = start;
        uint32_t end = start + FormatDwords(head.fmt);
        while (j < final_order.size())
        {
          const Element &el = *final_order[j].second.el;
          const uint32_t off = el.offset_bytes / 4, e_end = off + FormatDwords(el.fmt);
          if (final_order[j].first != final_order[j - 1].first + 1 || el.stream != head.stream || off <= last_off ||
              e_end - start > kChainWindow)
          {
            break;
          }
          last_off = off;
          end = std::max(end, e_end);
          ++j;
        }
        for (size_t k = i; k < j; ++k)
        {
          const uint32_t pos = final_order[k].first, src = final_order[k].second.src_addr;
          const Element &el = *final_order[k].second.el;
          uint32_t i0 = dw[3 * src], i1 = dw[3 * src + 1], i2 = dw[3 * src + 2];
          PatchInstruction(i0, i1, i2, el, stride_dwords(el.stream));
          if (k == i)
          {
            if (j - i > 1)
            {
              i0 |= std::min<uint32_t>(7, end > start + 1 ? end - start - 1 : 0) << 27;
            }
          }
          else
          {
            i1 |= 1u << 30;
          }
          result[3 * pos] = i0;
          result[3 * pos + 1] = i1;
          result[3 * pos + 2] = i2;
        }
        i = j;
      }
    }
    if (bound_fetches == 0)
    {
      if (why) *why = "no vfetch matched the declaration";
      return false;
    }
    if (unbound_fetches && why)
    {
      *why = "some vfetch instructions have no declaration element (left unbound)";
    }
    out.resize(bytes);
    for (uint32_t i = 0; i < count; ++i)
    {
      PutBe32(out.data() + 4 * i, result[i]);
    }
    return true;
  }
} // namespace pdz::native::binder
