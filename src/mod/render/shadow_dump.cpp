// Shadow-mode recorder: the cold path. OnSwap() runs once per frame from the
// Swap hook (after the original Swap returned) and applies the dump policy;
// DumpFrame() writes one frame's ring records to the log through REXLOG_INFO
// (the SDK log lives in the logs folder next to the exe), decoding the key
// registers: depth func / write, cull, alpha test, colour mask, blend, the
// render-target and depth formats, viewport, scissor, dirty masks, streams,
// textures.
//
// Policy (pdz_render_shadow_dump_frames, default 0; was 600): the first FULL frame
// after the recorder is switched on is dumped (the frame in which it was
// switched on may be partial), then every N swaps; 0 = only that first frame.
// A dump is one REXLOG_INFO line per record (a few hundred to a few thousand
// lines, ~400 bytes each) and never runs while the recorder is off.
#include <cstdint>
#include <cstring>

#include <fmt/format.h>

#include <rex/logging.h>

#include "shadow.h"
#include "shadow_record.h"

namespace pdz::shadow
{
  namespace
  {
    // ---- names ------------------------------------------------------------
    struct RegName
    {
      uint32_t index;
      const char *name;
    };
    // The SDK's Xenos register table (Xenia's), used read-only for names.
    const RegName kRegNames[] = {
#define XE_GPU_REGISTER(index, type, name) {index, #name},
#include <rex/graphics/register_table.inc>
#undef XE_GPU_REGISTER
    };

    const char *RegisterName(uint32_t index)
    {
      for (const RegName &r : kRegNames)
      {
        if (r.index == index)
        {
          return r.name;
        }
      }
      return "?";
    }

    const char *PrimName(uint32_t prim)
    {
      switch (prim)
      {
      case 1: return "POINTLIST";
      case 2: return "LINELIST";
      case 3: return "LINESTRIP";
      case 4: return "TRILIST";
      case 5: return "TRIFAN";
      case 6: return "TRISTRIP";
      case 7: return "TRI_WFLAGS";
      case 8: return "RECTLIST";
      case 12: return "LINELOOP";
      case 13: return "QUADLIST";
      case 14: return "QUADSTRIP";
      case 15: return "POLYGON";
      default: return "?";
      }
    }

    const char *CompareName(uint32_t f)
    {
      static const char *const kNames[8] = {"NEVER", "LESS", "EQUAL", "LEQUAL",
                                            "GREATER", "NOTEQUAL", "GEQUAL", "ALWAYS"};
      return kNames[f & 7];
    }

    const char *StencilOpName(uint32_t op)
    {
      static const char *const kNames[8] = {"KEEP", "ZERO", "REPLACE", "INCRSAT",
                                            "DECRSAT", "INVERT", "INCR", "DECR"};
      return kNames[op & 7];
    }

    const char *BlendFactorName(uint32_t f)
    {
      switch (f)
      {
      case 0: return "ZERO";
      case 1: return "ONE";
      case 4: return "SRC_COLOR";
      case 5: return "INV_SRC_COLOR";
      case 6: return "SRC_ALPHA";
      case 7: return "INV_SRC_ALPHA";
      case 8: return "DST_COLOR";
      case 9: return "INV_DST_COLOR";
      case 10: return "DST_ALPHA";
      case 11: return "INV_DST_ALPHA";
      case 12: return "CONST_COLOR";
      case 13: return "INV_CONST_COLOR";
      case 14: return "CONST_ALPHA";
      case 15: return "INV_CONST_ALPHA";
      case 16: return "SRC_ALPHA_SAT";
      default: return "?";
      }
    }

    const char *BlendOpName(uint32_t op)
    {
      static const char *const kNames[8] = {"ADD", "SUB", "MIN", "MAX", "REVSUB", "?", "?", "?"};
      return kNames[op & 7];
    }

    const char *ColorFormatName(uint32_t f)
    {
      switch (f)
      {
      case 0: return "8_8_8_8";
      case 1: return "8_8_8_8_GAMMA";
      case 2: return "2_10_10_10";
      case 3: return "2_10_10_10_FLOAT";
      case 4: return "16_16";
      case 5: return "16_16_16_16";
      case 6: return "16_16_FLOAT";
      case 7: return "16_16_16_16_FLOAT";
      case 10: return "2_10_10_10_AS_10_10_10_10";
      case 12: return "2_10_10_10_FLOAT_AS_16_16_16_16";
      case 14: return "32_FLOAT";
      case 15: return "32_32_FLOAT";
      default: return "?";
      }
    }

    const char *TextureFormatName(uint32_t f)
    {
      switch (f)
      {
      case 2: return "8";
      case 6: return "8_8_8_8";
      case 7: return "2_10_10_10";
      case 10: return "8_8";
      case 15: return "4_4_4_4";
      case 18: return "DXT1";
      case 19: return "DXT2_3";
      case 20: return "DXT4_5";
      case 22: return "24_8";
      case 23: return "24_8_FLOAT";
      case 24: return "16";
      case 25: return "16_16";
      case 26: return "16_16_16_16";
      case 27: return "16_EXPAND";
      case 29: return "16_16_16_16_EXPAND";
      case 30: return "16_FLOAT";
      case 31: return "16_16_FLOAT";
      case 32: return "16_16_16_16_FLOAT";
      case 36: return "32_FLOAT";
      case 37: return "32_32_FLOAT";
      case 49: return "DXN";
      case 60: return "CTX1";
      default: return "?";
      }
    }

    const char *KindName(Kind k)
    {
      switch (k)
      {
      case Kind::kDrawIndexedVertices: return "DrawIndexed";
      case Kind::kDrawVertices: return "Draw";
      case Kind::kDrawVerticesUP: return "DrawUP";
      case Kind::kBeginVertices: return "BeginVertices";
      case Kind::kClear: return "Clear";
      case Kind::kResolve: return "Resolve";
      case Kind::kSwap: return "Swap";
      case Kind::kDrawIndexedVerticesUP: return "DrawIndexedUP";
      default: return "?";
      }
    }

    // ---- register runs and dirty-mask layout (from sub_826FA8B8) ----------
    struct Run
    {
      uint32_t first_reg;
      uint32_t dev_offset;
      uint32_t count;
      uint32_t mask;   // index into DrawRecord::dirty
      uint32_t msb;    // bit of the FIRST register in that mask (next registers go down)
    };
    constexpr Run kRuns[] = {
        {0x2000, dev::kRun2000, 16, 2, 29},
        {0x2100, dev::kRun2100, 21, 3, 63},
        {0x2180, dev::kRun2180, 5, 3, 42},
        {0x2200, dev::kRun2200, 12, 3, 37},
        {0x2280, dev::kRun2280, 21, 3, 25},
        {0x2300, dev::kRun2300, 38, 4, 63},
        {0x2380, dev::kRun2380, 8, 4, 25},
    };

    inline uint32_t Reg(const DrawRecord &r, uint32_t dev_offset)
    {
      return Swap32(r.regs_be[(dev_offset - dev::kRegShadowBegin) / 4]);
    }

    inline uint32_t Fetch(const DrawRecord &r, uint32_t slot, uint32_t dword)
    {
      uint32_t be;
      std::memcpy(&be, r.fetch_be + slot * dev::kFetchConstantBytes + 4 * dword, 4);
      return Swap32(be);
    }

    // Appends "name(,name)*" for every set bit of a run; returns the count.
    uint32_t AppendDirtyRun(fmt::memory_buffer &out, const DrawRecord &r, const Run &run)
    {
      uint32_t n = 0;
      for (uint32_t i = 0; i < run.count; ++i)
      {
        const uint32_t bit = run.msb - i;
        if ((r.dirty[run.mask] >> bit) & 1)
        {
          fmt::format_to(std::back_inserter(out), "{}{}", n ? "," : "", RegisterName(run.first_reg + i));
          ++n;
        }
      }
      return n;
    }

    // ALU constant masks: bit 63 = float constants c0..c3, bit 62 = c4..c7, ...
    void AppendAluRanges(fmt::memory_buffer &out, uint64_t mask)
    {
      bool first = true;
      int start = -1;
      for (int bit = 63; bit >= -1; --bit)
      {
        const bool set = bit >= 0 && ((mask >> bit) & 1);
        if (set && start < 0)
        {
          start = 63 - bit;
        }
        else if (!set && start >= 0)
        {
          const int end = 63 - bit - 1;
          fmt::format_to(std::back_inserter(out), "{}c{}-c{}", first ? "" : ",", start * 4, end * 4 + 3);
          first = false;
          start = -1;
        }
      }
      if (first)
      {
        fmt::format_to(std::back_inserter(out), "-");
      }
    }

    void DumpTables(uint32_t frame)
    {
      const State &s = Get();
      REXLOG_INFO("pdz_render_shadow: tables at frame {}: {} shaders ({} dropped), {} declarations ({} dropped)",
                  frame, s.shader_count, s.shader_table_overflow, s.decl_count, s.decl_table_overflow);
      for (const ShaderEntry &e : s.shaders)
      {
        if (e.object)
        {
          REXLOG_INFO("pdz_render_shadow: shader {} obj={:08X} magic={:08X} ucode={:08X} bytes={} xxh3={:016X} frame={}",
                      e.type ? "VS" : "PS", e.object, e.magic, e.ucode_src, e.ucode_bytes, e.hash, e.created_frame);
        }
      }
      for (const DeclEntry &e : s.decls)
      {
        if (!e.object)
        {
          continue;
        }
        fmt::memory_buffer buf;
        const uint32_t n = e.count < 16 ? e.count : 16;
        for (uint32_t i = 0; i < n; ++i)
        {
          // D3DVERTEXELEMENT9 (Xbox 360): WORD Stream, WORD Offset, DWORD Type
          // (packed 360 decl type, e.g. 0x2A23B9 FLOAT3), BYTE Method, Usage,
          // UsageIndex, pad.
          const uint32_t w0 = Swap32(e.elements_be[i][0]);
          const uint32_t w1 = Swap32(e.elements_be[i][1]);
          const uint32_t w2 = Swap32(e.elements_be[i][2]);
          fmt::format_to(std::back_inserter(buf), "{}[s{} off{} type={:06X} m{} u{} i{}]", i ? " " : "",
                         w0 >> 16, w0 & 0xFFFF, w1, (w2 >> 24) & 0xFF, (w2 >> 16) & 0xFF, (w2 >> 8) & 0xFF);
        }
        REXLOG_INFO("pdz_render_shadow: decl obj={:08X} from={:08X} count={} maxstream={} frame={} {}",
                    e.object, e.source, e.count, e.max_stream, e.created_frame, fmt::to_string(buf));
      }
    }

    void DumpRecord(const DrawRecord &r)
    {
      fmt::memory_buffer out;
      auto o = std::back_inserter(out);

      fmt::format_to(o, "pdz_render_shadow: #{} f{} {} ", r.sequence, r.frame, KindName(r.kind));
      switch (r.kind)
      {
      case Kind::kDrawIndexedVertices:
        fmt::format_to(o, "prim={} base={} start={} count={}", PrimName(r.a[0]), int32_t(r.a[1]), r.a[2], r.a[3]);
        break;
      case Kind::kDrawVertices:
        fmt::format_to(o, "prim={} start={} count={}", PrimName(r.a[0]), r.a[1], r.a[2]);
        break;
      case Kind::kDrawVerticesUP:
        fmt::format_to(o, "prim={} count={} data={:08X} stride={}", PrimName(r.a[0]), r.a[1], r.a[2], r.a[3]);
        break;
      case Kind::kBeginVertices:
        fmt::format_to(o, "prim={} count={} stride={} ring={:08X}", PrimName(r.a[0]), r.a[1], r.a[2], r.a[3]);
        break;
      case Kind::kClear:
        fmt::format_to(o, "count={} rects={:08X} flags={:02X} color={:08X} z={} stencil={}", r.a[0], r.a[1], r.a[2],
                       r.a[3], BitsToFloat(r.f0_bits), r.a[4]);
        break;
      case Kind::kResolve:
        fmt::format_to(o, "flags={:X} srcrect={:08X} dest={:08X} destpt={:08X} level={} z={}", r.a[0], r.a[1],
                       r.a[2], r.a[3], r.a[4], BitsToFloat(r.f0_bits));
        break;
      case Kind::kSwap:
        fmt::format_to(o, "front={:08X} params={:08X}", r.a[0], r.a[1]);
        break;
      case Kind::kDrawIndexedVerticesUP:
        fmt::format_to(o, "prim={} count={} data={:08X} stride={} indices={:08X} {}-bit vertices={}+{}", PrimName(r.a[0]),
                       r.a[3], r.a[6], r.a[7], r.a[4], r.a[5] == 1 ? 32 : 16, r.a[1], r.a[2]);
        break;
      default:
        break;
      }
      fmt::format_to(o, " caller={:08X}", r.caller);
      if (r.tiling_flag == 0xFF)
      {
        fmt::format_to(o, " (no device snapshot: r3={:08X})", r.device);
        REXLOG_INFO("{}", fmt::to_string(out));
        return;
      }
      if (r.tiling_flag)
      {
        fmt::format_to(o, " TILING");
      }

      // Shaders and geometry bindings.
      fmt::format_to(o, " | VS={:08X}/{:016X} PS={:08X}/{:016X} decl={:08X} IB={:08X}", r.vertex_shader, r.vs_hash,
                     r.pixel_shader, r.ps_hash, r.vertex_decl, r.index_buffer);
      for (uint32_t i = 0; i < dev::kStreamCount; ++i)
      {
        const Stream &st = r.streams[i];
        if (st.vb)
        {
          fmt::format_to(o, " s{}=vb{:08X}+{} stride={} base={:08X} size={}", i, st.vb, st.offset, st.stride,
                         st.base, st.size);
        }
      }

      // Targets: object pointers plus the register view (base tile, format).
      const uint32_t surface_info = Reg(r, dev::kRun2000);          // RB_SURFACE_INFO
      const uint32_t color_info = Reg(r, dev::kRun2000 + 4);        // RB_COLOR_INFO
      const uint32_t depth_info = Reg(r, dev::kRun2000 + 8);        // RB_DEPTH_INFO
      fmt::format_to(o, " | RT={:08X},{:08X},{:08X},{:08X} DS={:08X} pitch={} msaa={} color0: tile={} fmt={}({}) depth: tile={} fmt={}",
                     r.render_targets[0], r.render_targets[1], r.render_targets[2], r.render_targets[3],
                     r.depth_stencil, surface_info & 0x3FFF, (surface_info >> 16) & 3, color_info & 0xFFF,
                     (color_info >> 16) & 0xF, ColorFormatName((color_info >> 16) & 0xF), depth_info & 0xFFF,
                     ((depth_info >> 16) & 1) ? "D24FS8" : "D24S8");
      for (uint32_t i = 1; i < 4; ++i)
      {
        if (r.render_targets[i])
        {
          const uint32_t info = Reg(r, dev::kRun2000 + 4 * (2 + i)); // RB_COLOR1..3_INFO
          fmt::format_to(o, " color{}: tile={} fmt={}({})", i, info & 0xFFF, (info >> 16) & 0xF,
                         ColorFormatName((info >> 16) & 0xF));
        }
      }

      // Viewport / scissor.
      fmt::format_to(o, " | vp={},{} {}x{} z={}..{} scissor={},{}-{},{} winscissor={:08X},{:08X}", r.viewport[0],
                     r.viewport[1], r.viewport[2], r.viewport[3], BitsToFloat(r.viewport[4]),
                     BitsToFloat(r.viewport[5]), r.scissor[0], r.scissor[1], r.scissor[2], r.scissor[3],
                     r.window_scissor[0], r.window_scissor[1]);

      // RB_DEPTHCONTROL (SDK registers.h layout): bit0 stencil, bit1 z, bit2
      // z write, bits 4-6 zfunc, bit 7 backface, 8-10 stencil func,
      // 11-13 fail, 14-16 zpass, 17-19 zfail (+12 for the back face set).
      const uint32_t dc = Reg(r, dev::kRB_DEPTHCONTROL);
      fmt::format_to(o, " | depth[{:08X}]: z={} write={} func={} stencil={}", dc, dc & 2 ? "on" : "off",
                     dc & 4 ? "on" : "off", CompareName(dc >> 4), dc & 1 ? "on" : "off");
      if (dc & 1)
      {
        const uint32_t sr = Reg(r, dev::kRB_STENCILREFMASK);
        fmt::format_to(o, "(func={} fail={} zpass={} zfail={} ref={} mask={:02X} wmask={:02X}{})",
                       CompareName(dc >> 8), StencilOpName(dc >> 11), StencilOpName(dc >> 14),
                       StencilOpName(dc >> 17), sr & 0xFF, (sr >> 8) & 0xFF, (sr >> 16) & 0xFF,
                       dc & 0x80 ? " backface" : "");
      }

      // PA_SU_SC_MODE_CNTL: bit0 cull front, bit1 cull back, bit2 face
      // (0 = CCW front), bits 3-4 poly mode, 5-7 / 8-10 front / back polygon
      // type, bit 11 / 12 polygon offset front / back enable.
      const uint32_t mc = Reg(r, dev::kPA_SU_SC_MODE_CNTL);
      const char *cull = (mc & 3) == 0 ? "none" : (mc & 3) == 1 ? "front" : (mc & 3) == 2 ? "back" : "both";
      fmt::format_to(o, " | cull[{:08X}]: {} front={} polymode={} ptype={}/{} offset={}{}", mc, cull,
                     mc & 4 ? "CW" : "CCW", (mc >> 3) & 3, (mc >> 5) & 7, (mc >> 8) & 7,
                     mc & 0x800 ? "F" : "-", mc & 0x1000 ? "B" : "-");
      if (mc & 0x1800)
      {
        fmt::format_to(o, " scale={} bias={}", BitsToFloat(Reg(r, dev::kRun2380)),
                       BitsToFloat(Reg(r, dev::kRun2380 + 4)));
      }

      // RB_COLORCONTROL: bits 0-2 alpha func, bit 3 alpha test enable, bit 4
      // alpha-to-mask; RB_ALPHA_REF float.
      const uint32_t cc = Reg(r, dev::kRB_COLORCONTROL);
      fmt::format_to(o, " | alphatest[{:08X}]: {} func={} ref={}{}", cc, cc & 8 ? "on" : "off", CompareName(cc),
                     BitsToFloat(Reg(r, dev::kRB_ALPHA_REF)), cc & 0x10 ? " a2m" : "");

      // RB_COLOR_MASK: 4 bits (RGBA) per target.
      const uint32_t cm = Reg(r, dev::kRB_COLOR_MASK);
      fmt::format_to(o, " | colormask={:04X}", cm & 0xFFFF);

      // RB_BLENDCONTROL0: src 0-4, op 5-7, dst 8-12; alpha src 16-20, op 21-23, dst 24-28.
      const uint32_t bc = Reg(r, dev::kRB_BLENDCONTROL0);
      fmt::format_to(o, " | blend0[{:08X}]: {} {} {} / alpha {} {} {}", bc, BlendFactorName(bc & 0x1F),
                     BlendOpName((bc >> 5) & 7), BlendFactorName((bc >> 8) & 0x1F),
                     BlendFactorName((bc >> 16) & 0x1F), BlendOpName((bc >> 21) & 7),
                     BlendFactorName((bc >> 24) & 0x1F));

      // SQ_PROGRAM_CNTL (GPR split) and point size for the particle draws.
      const uint32_t pc = Reg(r, dev::kRun2180);
      fmt::format_to(o, " | gpr vs={} ps={} pointsize={:08X}", (pc & 0x3F) + 1, ((pc >> 8) & 0x3F) + 1,
                     Reg(r, dev::kRun2280));

      // Dirty masks.
      fmt::format_to(o, " | dirty={:016X},{:016X},{:016X},{:016X},{:016X}", r.dirty[0], r.dirty[1], r.dirty[2],
                     r.dirty[3], r.dirty[4]);
      fmt::format_to(o, " vsalu:");
      AppendAluRanges(out, r.dirty[0]);
      fmt::format_to(o, " psalu:");
      AppendAluRanges(out, r.dirty[1]);
      {
        fmt::format_to(o, " fetch:");
        bool any = false;
        for (uint32_t i = 0; i < 32; ++i)
        {
          if ((r.dirty[2] >> (63 - i)) & 1)
          {
            fmt::format_to(o, "{}{}", any ? "," : "", i);
            any = true;
          }
        }
        if (!any)
        {
          fmt::format_to(o, "-");
        }
        if ((r.dirty[2] >> 31) & 1)
        {
          fmt::format_to(o, " bool");
        }
        if ((r.dirty[4] >> 10) & 1)
        {
          fmt::format_to(o, " streams");
        }
        if ((r.dirty[4] >> 5) & 0xF)
        {
          fmt::format_to(o, " program({:X})", uint32_t((r.dirty[4] >> 5) & 0xF));
        }
      }
      for (const Run &run : kRuns)
      {
        fmt::memory_buffer names;
        if (AppendDirtyRun(names, r, run))
        {
          fmt::format_to(o, " regs{:X}:{}", run.first_reg, fmt::to_string(names));
        }
      }

      // Textures: samplers with an object bound, decoded from the fetch
      // constant (dword1: format bits 0-5, base >> 12 at bits 12-31; dword2:
      // 2D width-1 bits 0-12, height-1 bits 13-25; dword0: tiled bit 31,
      // type bits 0-1).
      bool any_tex = false;
      for (uint32_t i = 0; i < dev::kTextureCount; ++i)
      {
        if (!r.textures[i])
        {
          continue;
        }
        if (!any_tex)
        {
          fmt::format_to(o, " | tex:");
          any_tex = true;
        }
        const uint32_t d0 = Fetch(r, i, 0);
        const uint32_t d1 = Fetch(r, i, 1);
        const uint32_t d2 = Fetch(r, i, 2);
        fmt::format_to(o, " s{}={:08X}[fmt={}({}) base={:08X} {}x{} {}{}]", i, r.textures[i], d1 & 0x3F,
                       TextureFormatName(d1 & 0x3F), (d1 >> 12) << 12, (d2 & 0x1FFF) + 1, ((d2 >> 13) & 0x1FFF) + 1,
                       d0 >> 31 ? "tiled" : "linear", (d0 & 3) == 2 ? "" : " type!=2");
      }

      REXLOG_INFO("{}", fmt::to_string(out));
    }
  } // namespace

  void DumpFrame(uint8_t *base, uint32_t frame)
  {
    (void)base;
    static bool tables_done = false;
    const State &s = Get();

    // Collect the frame's records from the newest backwards (bounded by the ring).
    static uint32_t indices[kRingSize];
    uint32_t n = 0;
    for (uint32_t back = 1; back <= RingSlots() && back <= s.head; ++back)
    {
      const uint32_t i = (s.head - back) & RingMask();
      const DrawRecord &r = s.ring[i];
      if (r.kind == Kind::kNone || r.frame != frame)
      {
        if (r.frame < frame || r.kind == Kind::kNone)
        {
          break;
        }
        continue; // newer frame already in the ring (should not happen on the swap thread)
      }
      indices[n++] = i;
    }

    fmt::memory_buffer counters;
    for (uint32_t h = 0; h < uint32_t(Hook::kCount); ++h)
    {
      if (s.last_counters[h])
      {
        fmt::format_to(std::back_inserter(counters), " {}={}", HookName(Hook(h)), s.last_counters[h]);
      }
    }
    REXLOG_INFO("pdz_render_shadow: frame {} dump begin: {} records (ring head {}, {} bytes copied per record){}",
                frame, n, s.head, kBytesCopiedPerRecord, fmt::to_string(counters));
    if (!tables_done)
    {
      DumpTables(frame);
      tables_done = true;
    }
    for (uint32_t k = n; k > 0; --k)
    {
      DumpRecord(s.ring[indices[k - 1]]);
    }
    REXLOG_INFO("pdz_render_shadow: frame {} dump end", frame);
  }

  void OnSwap(uint8_t *base)
  {
    State &s = Get();
    const uint32_t frame = s.frame; // the frame just swapped
    std::memcpy(s.last_counters, s.counters, sizeof(s.counters));
    std::memset(s.counters, 0, sizeof(s.counters));
    s.frame = frame + 1;

    static bool was_enabled = false;
    static bool armed = false; // dump the next full frame
    const bool enabled = Enabled();
    if (enabled)
    {
      const int32_t every = REXCVAR_GET(pdz_render_shadow_dump_frames);
      bool dump = false;
      if (!was_enabled)
      {
        armed = true; // this frame was (possibly) partial; dump the next one
      }
      else if (armed)
      {
        armed = false;
        dump = true;
      }
      else if (every > 0 && frame % uint32_t(every) == 0)
      {
        dump = true;
      }
      if (dump)
      {
        DumpFrame(base, frame);
      }
    }
    else
    {
      armed = false;
    }
    was_enabled = enabled;
  }
} // namespace pdz::shadow
