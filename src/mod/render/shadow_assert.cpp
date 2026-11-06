// Shadow-mode assertion (shadow_assert.md): compare what the D3D library's own
// state flush sub_826FA8B8 writes into the command ring with the most recent
// DrawRecord taken by the draw hooks a few hundred instructions earlier.
//
// How (shadow_assert.md section 2, verified against generated\default\*.cpp on
// 2026-10-26):
//   * sub_826FA8B8(r3 = device) is hooked (REX_HOOK_RAW, forward in the
//     middle). At entry the five dirty masks (+16..+55), the ring write
//     pointer (+0: the address of the LAST written dword, the run writers use
//     stwu r,4(r4)), the segment limit (+4) and VGT_INDX_OFFSET (+11540) are
//     read. After the original returned, the dwords written in between,
//     (ring_before, ring_after], are parsed as PM4:
//       - type 0 (bits 31-30 = 00): (count-1)<<16 | reg, count dwords
//         (sub_826F9298 register runs 0x2000-0x2387, sub_826F9450 ALU constants
//         0x4000/0x4400 in 16-dword groups, sub_826F9358 fetch constants
//         0x4800 in 6-dword slots, the 0x4900..0x4927 bool + loop run
//         from device+10112, the loader's own RB_SAMPLE_COUNT-style
//         single register writes);
//       - type 2 (0x80000000): the alignment NOPs of the ALU / fetch writers;
//       - type 3: IM_LOAD 0x27 (shader microcode), INVALIDATE_STATE 0x3B,
//         SET_CONSTANT 0x2D and the loader's other packets (counted, IM_LOAD
//         checked against the shader objects).
//   * Every (register, value) pair is compared with the record's register
//     shadow (regs_be), fetch_be, bools_be, or, for ALU constants (not
//     recorded), with the device's constant store, which the flush does not
//     change. Dirty bits are compared with the record's masks, and the set of
//     emitted registers with the set of dirty bits (missing / unexpected
//     emission).
//   * Registers D3D fixes up at flush time (section 4) are in kFixups: they
//     are compared against their real source instead (draw argument or the
//     post-flush shadow) and the first record-vs-emitted difference per entry
//     is logged separately as an expected mismatch.
//   * A ring segment change (sub_826DDE30 inside the writers, detected by a
//     changed limit at +4 or a backwards / oversized window) skips that draw.
//
// Pairing: every draw-kind hook record (DrawIndexedVertices, DrawVertices,
// DrawVerticesUP, BeginVertices, Clear, Resolve) is followed by exactly one
// flush call on the same thread, inside the hooked function, so the flush
// pairs with ring[head-1] unless that record was already consumed by a
// previous flush (then this is one of the library-internal callers: counted
// as unpaired, by return address). No change to shadow_hooks.cpp is needed
// for this; see ASSERT_WIRING.md for the two optional call sites (per-frame
// summary at Swap, draw-packet check after the draw returned).
//
// Cost: with pdz_render_shadow_assert off, one relaxed load and a branch per
// flush. On: O(dwords emitted) per draw plus the fixed scalar loads above;
// debugging sessions only. Single producer thread assumed (as the recorder).
#include <cstdint>
#include <cstring>

#include <fmt/format.h>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>

#include "frame_timing.h" // pdz_frame_timing_phases: shadow.assert (drawcost)
#include "gpu_census.h"
#include "shadow.h"
#include "shadow_assert.h"
#include "shadow_record.h"

REX_EXTERN(__imp__sub_826FA8B8); // D3D state flush (dirty masks -> PM4 register runs)

namespace pdz::shadow
{
  namespace
  {
    // -------------------------------------------------------------------------
    // Constants of the flush (shadow_assert.md section 1)
    // -------------------------------------------------------------------------
    constexpr uint32_t kRegFirst = 0x2000;
    constexpr uint32_t kRegLast = 0x2387;
    constexpr uint32_t kRegSpace = 0x400; // 0x2000..0x23FF indexable

    constexpr uint32_t kVsAluBase = 0x4000;  // 1024 dwords, mirror at device+1920
    constexpr uint32_t kPsAluBase = 0x4400;  // 1024 dwords, mirror at device+6016
    constexpr uint32_t kFetchBase = 0x4800;  // 192 dwords (32 slots x 6)
    constexpr uint32_t kBoolBase = 0x4900;   // 8 dwords
    // Loop constants: 32 dwords at 0x4908, emitted in the same type-0 run as the bools (sub_826F9298 with mask
    // 0xFFFFFFFFFF000000 from device+10112: register 0x4900 + k = device+10112 + 4k for k < 40), so 0x4908 + i =
    // device+10144 + 4i (dev::kLoopConstants). Settled 2026-10-26 (shadow_assert.md 1.3): SetVertexShaderConstantI
    // sub_826E3788 writes VS i n at +10144 + 4n (index n), SetPixelShaderConstantI sub_826E37D8 PS i n at
    // +10208 + 4n (index 16 + n), the state block capture sub_826ED690 copies 64 bytes from +10144 (vertex state)
    // and +10208 (pixel state), and Datacore (logs 045 / 053) had every emitted dword equal to +10144 + 4i
    // (4,342,624 dwords, 134,665 non-zero; +10208 as the base matched only zeros). Compared like any register.
    constexpr uint32_t kLoopBase = 0x4908;
    constexpr uint32_t kLoopDwords = dev::kLoopConstantCount;
    constexpr uint32_t kAluDwords = 1024;
    constexpr uint32_t kAluGroupDwords = 16; // one dirty bit = 4 float4
    constexpr uint32_t kFetchDwords = 192;
    constexpr uint32_t kFetchSlotDwords = 6;

    // Dirty-mask indices (DrawRecord::dirty) and the bits that name registers.
    constexpr uint32_t kMaskVsAlu = 0;   // +16: all 64 bits
    constexpr uint32_t kMaskPsAlu = 1;   // +24: all 64 bits
    constexpr uint32_t kMaskFetch = 2;   // +32: 63..32 fetch slots, 31 bools, 29..14 regs 0x2000..0x200F
    constexpr uint32_t kMaskRegsA = 3;   // +40: 63..43 0x2100, 42..38 0x2180, 37..26 0x2200, 25..5 0x2280
    constexpr uint32_t kMaskRegsB = 4;   // +48: 63..26 0x2300, 25..18 0x2380, 10 streams, 8..5 program
    constexpr uint64_t kRegisterBits[dev::kDirtyMaskCount] = {
        ~0ull,
        ~0ull,
        0xFFFFFFFF80000000ull | 0x3FFFC000ull,
        ~0x1Full,
        ~0x3FFFFull,
    };
    constexpr uint64_t kBoolBit = 1ull << 31;              // mask 3 (index 2)
    constexpr uint64_t kStreamsChangedBit = 1ull << 10;    // mask 5 (index 4)
    constexpr uint64_t kProgramBits = 0x1E0ull;            // mask 5 (index 4) bits 8..5
    constexpr uint64_t kVgtIndxOffsetBit = 1ull << 61;     // mask 4 (index 3): 0x2102
    constexpr uint64_t kStreamFetchSlotBits = 0x3Full << 32; // fetch slots 26..31 (bit 63 - slot)
    // sub_826FA2E8 ORs these into mask 4 inside the flush when the program is
    // reloaded: 0x2180..0x2182 (bits 42..40) and RB_HIZCONTROL 0x2203 (bit 34).
    constexpr uint64_t kProgramLoaderRegBits = (7ull << 40) | (1ull << 34);

    constexpr uint32_t kDevVgtIndxOffset = 11540;  // 0x2102
    constexpr uint32_t kDevRingLimit = 4;          // end of the current ring segment
    constexpr uint32_t kDevVsProgram = 12948;      // program object the loader IM_LOADs (+40 ucode, +600 bytes)
    constexpr uint32_t kVsProgramUcode = 40;
    constexpr uint32_t kVsProgramUcodeBytes = 600;
    constexpr uint32_t kPsObjectUcode = 12;        // D3DPixelShader +12 ucode, +60 bytes
    constexpr uint32_t kPsObjectUcodeBytes = 60;

    constexpr uint32_t kMaxFlushBytes = 256u * 1024u; // larger = segment change, skip
    constexpr uint32_t kMaxLoggedMismatches = 50;
    constexpr uint32_t kMaxDrawPacketScan = 512;      // dwords scanned after the flush for the draw packet
    constexpr uint32_t kTopCount = 5;

    // PM4 (Xenia's pm4_opcodes)
    constexpr uint32_t kPm4ImLoad = 0x27;
    constexpr uint32_t kPm4DrawIndx = 0x22;

    struct Run
    {
      uint32_t first_reg;
      uint32_t dev_offset;
      uint32_t count;
      uint32_t mask; // DrawRecord::dirty index
      uint32_t msb;  // bit of the first register; the next registers go downwards
    };
    constexpr Run kRuns[] = {
        {0x2000, dev::kRun2000, 16, kMaskFetch, 29},
        {0x2100, dev::kRun2100, 21, kMaskRegsA, 63},
        {0x2180, dev::kRun2180, 5, kMaskRegsA, 42},
        {0x2200, dev::kRun2200, 12, kMaskRegsA, 37},
        {0x2280, dev::kRun2280, 21, kMaskRegsA, 25},
        {0x2300, dev::kRun2300, 38, kMaskRegsB, 63},
        {0x2380, dev::kRun2380, 8, kMaskRegsB, 25},
    };

    // -------------------------------------------------------------------------
    // Expected mismatches (shadow_assert.md section 4): registers whose
    // record value is stale by design. Compared against `source` instead; the
    // record-vs-emitted difference is counted and logged once per entry.
    // Not in this table because they are handled structurally: 4.2 stream
    // fetch constants (slots 26..31 rebuilt by sub_826F9578 when mask 5 bit 10
    // is set: recomputed from DrawRecord::streams and compared with the
    // post-flush device), 4.4 fetch slot 31 of DrawVerticesUP / BeginVertices
    // (written after the flush, outside the window), 4.5 bool constants (all
    // 8 emitted whenever bit 31 is set; compared as 8 values), 4.6
    // RB_COLOR_MASK (in the record already, no mismatch expected), 4.7 the
    // window scissor (not in any run, never in the flush window), 4.8
    // predicated tiling (never active in PDZ; the parse would see SET_BIN_*
    // type-3 packets and count them), 4.9 ring wrap (draw skipped).
    enum class Source : uint8_t
    {
      kDrawArgument,    // base vertex of the draw (0 for BeginVertices, flush-entry value otherwise)
      kPostFlushShadow, // the device shadow after the flush (rewritten inside the flush)
    };
    struct Fixup
    {
      uint32_t reg;
      Source source;
      const char *why;
    };
    constexpr Fixup kFixups[] = {
        {0x2102, Source::kDrawArgument, "VGT_INDX_OFFSET: the draw stores its base vertex before the flush (4.1)"},
        {0x2180, Source::kPostFlushShadow, "SQ_PROGRAM_CNTL: rewritten by the program loader sub_826FA2E8 inside the flush (4.3)"},
        {0x2181, Source::kPostFlushShadow, "SQ_CONTEXT_MISC: rewritten by the program loader sub_826FA2E8 inside the flush (4.3)"},
        {0x2182, Source::kPostFlushShadow, "SQ_INTERPOLATOR_CNTL: rewritten by the program loader sub_826FA2E8 inside the flush (4.3)"},
        {0x2203, Source::kPostFlushShadow, "RB_HIZCONTROL: rewritten by the program loader sub_826FA2E8 inside the flush (4.3)"},
    };
    constexpr uint32_t kFixupCount = sizeof(kFixups) / sizeof(kFixups[0]);
    // Structural expected-mismatch classes, counted and logged once like kFixups.
    enum StructuralFixup : uint32_t
    {
      kFixupStreamFetch = kFixupCount, // 4.2
      kFixupMaskVgtIndx,               // mask 4 bit 61 set at flush entry but not in the record (4.1)
      kFixupMaskProgramRegs,           // 0x2180..0x2182 / 0x2203 emitted without a dirty bit in the record (4.3)
      kFixupMaskStreams,               // mask 5 bit 10 (stream fetch slot) in the record, cleared at flush entry (4.2)
      kFixupTotal
    };
    const char *const kStructuralWhy[kFixupTotal - kFixupCount] = {
        "stream fetch constants (slots 26..31): rebuilt by sub_826F9578 inside the flush (4.2)",
        "mask 4 bit 61 (VGT_INDX_OFFSET) set by the draw after the record (4.1)",
        "program registers emitted without a record dirty bit: sub_826FA2E8 sets them inside the flush (4.3)",
        "mask 5 bit 10 (streams changed): the draw consumes it (stream builder sub_826F9578) before the flush (4.2)",
    };

    // -------------------------------------------------------------------------
    // Mismatch classes that are not one shadow register
    // -------------------------------------------------------------------------
    enum Category : uint32_t
    {
      kCatMaskDiff = 0,      // record dirty masks != flush-entry masks
      kCatMaskNotCleared,    // a mask is not zero after the flush
      kCatVsAluValue,        // emitted VS constant != device constant store
      kCatVsAluRange,        // VS group emitted with != 16 dwords, or missing / unexpected group
      kCatPsAluValue,
      kCatPsAluRange,
      kCatFetchValue,        // emitted fetch dword != record (or != post-flush device for stream slots)
      kCatFetchRange,        // slot with != 6 dwords, missing / unexpected slot
      kCatStreamRebuild,     // our recompute of the stream fetch constant != what the builder wrote
      kCatBoolValue,
      kCatBoolRange,
      kCatLoopValue,         // emitted loop constant 0x4908 + i != device+10144 + 4i (dev::kLoopConstants)
      kCatImLoad,            // IM_LOAD address / size != shader object
      kCatOtherType0,        // type-0 packet outside every known register range
      kCatParse,             // type-1 header or a packet past the window
      kCatDrawPacket,        // VGT_DRAW_INITIATOR != draw arguments (optional wiring)
      kCatCount
    };
    const char *const kCategoryNames[kCatCount] = {
        "dirty-masks", "masks-not-cleared", "VS-ALU-value", "VS-ALU-range", "PS-ALU-value", "PS-ALU-range",
        "fetch-value", "fetch-range", "stream-rebuild", "bool-value", "bool-range", "loop-value", "IM_LOAD", "other-type0",
        "pm4-parse", "draw-packet",
    };

    // Item index space for the counters: 0..kRegSpace-1 shadow registers
    // (0x2000 + i), then the categories.
    constexpr uint32_t kItemCount = kRegSpace + kCatCount;
    constexpr uint32_t kNoFrame = 0xFFFFFFFFu;

    struct RegInfo
    {
      int16_t dword;  // index into DrawRecord::regs_be, -1 = not in a run
      int8_t mask;    // DrawRecord::dirty index
      int8_t bit;
    };

    struct Pre
    {
      uint64_t masks[dev::kDirtyMaskCount];
      uint32_t ring_before;
      uint32_t limit_before;
      uint32_t indx_offset; // VGT_INDX_OFFSET shadow at flush entry
    };

    struct UnpairedCaller
    {
      uint32_t lr;
      uint32_t count;
    };

    struct AssertState
    {
      // tables
      RegInfo reg[kRegSpace];
      const char *reg_name[kRegSpace];
      bool tables_ready;
      bool announced;
      bool warned_no_recorder;

      // pairing
      bool have_last;
      uint32_t last_sequence;    // record consumed by the previous flush
      uint32_t flush_device;     // device of the last paired flush
      uint32_t flush_ring_after; // ring pointer after the last paired flush (draw packet window start)
      bool draw_pending;         // AssertOnDrawReturn may check the draw packet
      uint32_t frame;            // frame the per-frame counters belong to (kNoFrame = none yet)

      // per-frame
      uint32_t draws_frame;
      uint32_t compared_frame;      // shadow registers compared
      uint32_t alu_compared_frame;  // ALU dwords compared
      uint32_t fetch_compared_frame;
      uint32_t mismatches_frame;
      uint32_t wrapped_frame;
      uint32_t unpaired_frame;
      uint32_t type3_frame[128];
      uint32_t item_mismatch_frame[kItemCount];

      // totals
      uint32_t draws_total;
      uint32_t mismatches_total;
      uint32_t logged;
      uint32_t item_match_total[kItemCount];
      uint32_t item_mismatch_total[kItemCount];
      uint32_t fixup_count[kFixupTotal];
      // Window run 0x2080..0x2082 (PA_SC_WINDOW_OFFSET / SCISSOR_TL / BR, emitted by sub_826EE1C0 outside the
      // flush runs) checked against device +11520 / +11524 / +11528 (cumulative).
      uint64_t window_match[3];
      uint64_t window_mismatch[3];
      bool window_logged[3];
      bool other_reg_logged[0x400]; // registers in no run: first sighting logged by name
      bool fixup_logged[kFixupTotal];
      UnpairedCaller unpaired[16];
      bool vs_slot_logged;

      // per-draw scratch (reset per flush)
      uint64_t emitted[dev::kDirtyMaskCount];
      uint8_t alu_dwords[2][64];
      uint8_t fetch_dwords[32];
      uint8_t bool_dwords;
    };
    AssertState g_assert{};

    // -------------------------------------------------------------------------
    // Tables
    // -------------------------------------------------------------------------
    struct RegNameEntry
    {
      uint32_t index;
      const char *name;
    };
    const RegNameEntry kRegNameTable[] = {
#define XE_GPU_REGISTER(index, type, name) {index, #name},
#include <rex/graphics/register_table.inc>
#undef XE_GPU_REGISTER
    };

    void BuildTables()
    {
      AssertState &a = g_assert;
      for (uint32_t i = 0; i < kRegSpace; ++i)
      {
        a.reg[i] = RegInfo{-1, -1, -1};
        a.reg_name[i] = "?";
      }
      for (const RegNameEntry &e : kRegNameTable)
      {
        if (e.index >= kRegFirst && e.index < kRegFirst + kRegSpace)
        {
          a.reg_name[e.index - kRegFirst] = e.name;
        }
      }
      for (const Run &run : kRuns)
      {
        for (uint32_t i = 0; i < run.count; ++i)
        {
          RegInfo &info = a.reg[run.first_reg + i - kRegFirst];
          info.dword = int16_t((run.dev_offset - dev::kRegShadowBegin) / 4 + i);
          info.mask = int8_t(run.mask);
          info.bit = int8_t(run.msb - i);
        }
      }
      a.frame = kNoFrame;
      a.tables_ready = true;
    }

    const char *RegName(uint32_t reg)
    {
      const AssertState &a = g_assert;
      return reg >= kRegFirst && reg < kRegFirst + kRegSpace ? a.reg_name[reg - kRegFirst] : "?";
    }

    const char *ItemName(uint32_t item)
    {
      return item < kRegSpace ? g_assert.reg_name[item] : kCategoryNames[item - kRegSpace];
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

    // Finds the register of a dirty bit for the missing / unexpected reports.
    uint32_t RegOfBit(uint32_t mask, uint32_t bit)
    {
      for (const Run &run : kRuns)
      {
        if (run.mask == mask && bit <= run.msb && bit > run.msb - run.count)
        {
          return run.first_reg + (run.msb - bit);
        }
      }
      return 0;
    }

    const Fixup *FindFixup(uint32_t reg)
    {
      for (const Fixup &f : kFixups)
      {
        if (f.reg == reg)
        {
          return &f;
        }
      }
      return nullptr;
    }

    // Guest virtual -> the address form D3D writes into fetch constants and
    // IM_LOAD (sub_826F9578 / sub_826FA2E8): low 29 bits, plus 0x1000 when the
    // top nibble is 0xE (the physical window's host offset, REX_PHYS_HOST_OFFSET).
    inline uint32_t GuestToPacketAddress(uint32_t va)
    {
      return (va & 0x1FFFFFFFu) + ((((va >> 20) & 0xFFFu) + 0x200u) & 0x1000u);
    }

    // -------------------------------------------------------------------------
    // Counting and logging
    // -------------------------------------------------------------------------
    inline void Match(uint32_t item)
    {
      ++g_assert.item_match_total[item];
    }

    template <typename... Args>
    void Mismatch(const DrawRecord &r, uint32_t item, fmt::format_string<Args...> what, Args &&...args)
    {
      AssertState &a = g_assert;
      ++a.item_mismatch_frame[item];
      ++a.item_mismatch_total[item];
      ++a.mismatches_frame;
      ++a.mismatches_total;
      if (a.logged < kMaxLoggedMismatches)
      {
        ++a.logged;
        REXLOG_WARN("shadow assert: mismatch #{} f{} {} {}: {}{}", r.sequence, r.frame, KindName(r.kind),
                    ItemName(item), fmt::format(what, std::forward<Args>(args)...),
                    a.logged == kMaxLoggedMismatches ? " (last logged mismatch; counters continue)" : "");
      }
    }

    template <typename... Args>
    void ExpectedMismatch(const DrawRecord &r, uint32_t fixup, const char *why, fmt::format_string<Args...> what,
                          Args &&...args)
    {
      AssertState &a = g_assert;
      ++a.fixup_count[fixup];
      if (!a.fixup_logged[fixup])
      {
        a.fixup_logged[fixup] = true;
        REXLOG_INFO("shadow assert: expected mismatch (first of its kind) #{} f{} {}: {} [{}]", r.sequence, r.frame,
                    KindName(r.kind), fmt::format(what, std::forward<Args>(args)...), why);
      }
    }

    void NoteUnpaired(uint32_t lr)
    {
      AssertState &a = g_assert;
      ++a.unpaired_frame;
      for (UnpairedCaller &u : a.unpaired)
      {
        if (u.lr == lr || u.lr == 0)
        {
          u.lr = lr;
          ++u.count;
          return;
        }
      }
    }

    void ResetFrame()
    {
      AssertState &a = g_assert;
      a.draws_frame = 0;
      a.compared_frame = 0;
      a.alu_compared_frame = 0;
      a.fetch_compared_frame = 0;
      a.mismatches_frame = 0;
      a.wrapped_frame = 0;
      a.unpaired_frame = 0;
      std::memset(a.type3_frame, 0, sizeof(a.type3_frame));
      std::memset(a.item_mismatch_frame, 0, sizeof(a.item_mismatch_frame));
    }

    void EmitSummary()
    {
      AssertState &a = g_assert;
      if (a.frame == kNoFrame)
      {
        return;
      }
      // Top items by this frame's mismatch count.
      uint32_t top_item[kTopCount];
      uint32_t top_count[kTopCount] = {};
      for (uint32_t item = 0; item < kItemCount; ++item)
      {
        const uint32_t c = a.item_mismatch_frame[item];
        if (c == 0)
        {
          continue;
        }
        for (uint32_t k = 0; k < kTopCount; ++k)
        {
          if (c > top_count[k])
          {
            for (uint32_t m = kTopCount - 1; m > k; --m)
            {
              top_count[m] = top_count[m - 1];
              top_item[m] = top_item[m - 1];
            }
            top_count[k] = c;
            top_item[k] = item;
            break;
          }
        }
      }
      fmt::memory_buffer top;
      for (uint32_t k = 0; k < kTopCount && top_count[k]; ++k)
      {
        fmt::format_to(std::back_inserter(top), "{}{}={}", k ? ", " : "", ItemName(top_item[k]), top_count[k]);
      }
      fmt::memory_buffer type3;
      for (uint32_t op = 0; op < 128; ++op)
      {
        if (a.type3_frame[op])
        {
          fmt::format_to(std::back_inserter(type3), " {:02X}x{}", op, a.type3_frame[op]);
        }
      }
      fmt::memory_buffer fixups;
      for (uint32_t f = 0; f < kFixupTotal; ++f)
      {
        if (a.fixup_count[f])
        {
          fmt::format_to(std::back_inserter(fixups), " {}={}", f < kFixupCount ? RegName(kFixups[f].reg)
                                                                              : f == kFixupStreamFetch ? "stream-fetch"
                                                                              : f == kFixupMaskVgtIndx ? "mask-vgt-indx"
                                                                              : f == kFixupMaskStreams ? "mask-streams"
                                                                                                       : "mask-program-regs",
                         a.fixup_count[f]);
        }
      }
      REXLOG_INFO("shadow assert: {} draws, {} registers compared, {} mismatches (top: {}) | frame {} alu dwords {} "
                  "fetch dwords {} wrapped {} unpaired {} type3{} | expected fixups (total){} | totals: {} draws {} "
                  "mismatches | window run match {} {} {} mismatch {} {} {}",
                  a.draws_frame, a.compared_frame, a.mismatches_frame, top.size() ? fmt::to_string(top) : "-", a.frame,
                  a.alu_compared_frame, a.fetch_compared_frame, a.wrapped_frame, a.unpaired_frame,
                  type3.size() ? fmt::to_string(type3) : " -", fixups.size() ? fmt::to_string(fixups) : " -",
                  a.draws_total, a.mismatches_total, a.window_match[0], a.window_match[1], a.window_match[2],
                  a.window_mismatch[0], a.window_mismatch[1], a.window_mismatch[2]);
      ResetFrame();
      a.frame = kNoFrame;
    }

    // -------------------------------------------------------------------------
    // Per-draw comparison
    // -------------------------------------------------------------------------
    inline uint32_t RecordReg(const DrawRecord &r, const RegInfo &info)
    {
      return Swap32(r.regs_be[info.dword]);
    }

    inline uint32_t RecordFetchDword(const DrawRecord &r, uint32_t d)
    {
      uint32_t be;
      std::memcpy(&be, r.fetch_be + 4 * d, 4);
      return Swap32(be);
    }

    struct Walk
    {
      uint8_t *base;
      uint32_t device;
      const DrawRecord *r;
      const Pre *pre;
      uint32_t decl_max_stream; // 0xFFFFFFFF = no declaration
    };

    // Stream s -> fetch dword index of its dword0 / dword1 (sub_826F9578: dword1
    // at device+1916-8*s, dword0 at +1912-8*s; fetch dword d is at +1152+4*d).
    inline bool StreamOfFetchDword(uint32_t d, uint32_t &stream, uint32_t &part)
    {
      if (d < 160)
      {
        return false;
      }
      stream = (191 - d) >> 1;
      part = ((191 - d) & 1) ? 0 : 1;
      return true;
    }

    void CompareShadowRegister(Walk &w, uint32_t reg, uint32_t value)
    {
      AssertState &a = g_assert;
      const DrawRecord &r = *w.r;
      const uint32_t item = reg - kRegFirst;
      const RegInfo &info = a.reg[item];
      if (reg >= 0x2080 && reg <= 0x2082)
      {
        // The native viewport reads these from the record (dev::kWindowOffset / kWindowScissorTL / BR).
        const uint32_t k = reg - 0x2080;
        const uint32_t recorded = Swap32(r.regs_be[(dev::kWindowOffset - dev::kRegShadowBegin) / 4 + k]);
        ++a.compared_frame;
        if (value == recorded)
        {
          ++a.window_match[k];
          Match(item);
          if (!a.window_logged[k])
          {
            a.window_logged[k] = true;
            REXLOG_INFO("shadow assert: window run: {} emitted {:08X} == device +{} at draw entry (first match)",
                        RegName(reg), value, dev::kWindowOffset + 4 * k);
          }
        }
        else
        {
          ++a.window_mismatch[k];
          Mismatch(r, item, "window run: record (device +{})={:08X} emitted={:08X}", dev::kWindowOffset + 4 * k,
                   recorded, value);
        }
        return;
      }
      if (info.dword < 0)
      {
        if (reg - kRegFirst < 0x400 && !a.other_reg_logged[reg - kRegFirst])
        {
          a.other_reg_logged[reg - kRegFirst] = true;
          REXLOG_INFO("shadow assert: register 0x{:04X} ({}) written in a draw's window outside every dirty run "
                      "(first sighting, emitted {:08X})", reg, RegName(reg), value);
        }
        Mismatch(r, kRegSpace + kCatOtherType0, "register 0x{:04X} ({}) is in no dirty run: emitted={:08X}", reg,
                 RegName(reg), value);
        return;
      }
      ++a.compared_frame;
      a.emitted[info.mask] |= 1ull << info.bit;
      const uint32_t recorded = RecordReg(r, info);
      const Fixup *fixup = FindFixup(reg);
      if (!fixup)
      {
        if (value == recorded)
        {
          Match(item);
          return;
        }
        const uint32_t post = Load32(w.base, w.device + dev::kRegShadowBegin + 4 * uint32_t(info.dword));
        Mismatch(r, item, "record={:08X} emitted={:08X} post-flush shadow={:08X}{}", recorded, value, post,
                 post == value ? " (shadow rewritten inside the flush?)" : "");
        return;
      }
      // Expected-mismatch register: compare with the real source.
      uint32_t expected;
      const char *source;
      if (fixup->source == Source::kDrawArgument)
      {
        source = "draw argument";
        switch (r.kind)
        {
        case Kind::kDrawIndexedVertices:
        case Kind::kDrawVertices:
          expected = r.a[1];
          break;
        case Kind::kBeginVertices:
          expected = 0;
          break;
        case Kind::kDrawIndexedVerticesUP:
          expected = 0u - r.a[1]; // BeginIndexedVertices stores -MinVertexIndex before its flush
          break;
        default:
          expected = w.pre->indx_offset;
          source = "flush-entry shadow";
          break;
        }
      }
      else
      {
        source = "post-flush shadow";
        expected = Load32(w.base, w.device + dev::kRegShadowBegin + 4 * uint32_t(info.dword));
      }
      const uint32_t f = uint32_t(fixup - kFixups);
      if (value != recorded)
      {
        ExpectedMismatch(r, f, fixup->why, "{} record={:08X} emitted={:08X} {}={:08X}", RegName(reg), recorded, value,
                         source, expected);
      }
      if (value == expected)
      {
        Match(item);
      }
      else
      {
        Mismatch(r, item, "emitted={:08X} != {}={:08X} (record={:08X})", value, source, expected, recorded);
      }
    }

    void CompareAlu(Walk &w, uint32_t bank, uint32_t idx, uint32_t value)
    {
      AssertState &a = g_assert;
      const DrawRecord &r = *w.r;
      const uint32_t group = idx / kAluGroupDwords;
      a.emitted[bank == 0 ? kMaskVsAlu : kMaskPsAlu] |= 1ull << (63 - group);
      if (a.alu_dwords[bank][group] < 255)
      {
        ++a.alu_dwords[bank][group];
      }
      ++a.alu_compared_frame;
      const uint32_t store = bank == 0 ? dev::kVsAluConstants : dev::kPsAluConstants;
      const uint32_t device_value = Load32(w.base, w.device + store + 4 * idx);
      const uint32_t item = kRegSpace + (bank == 0 ? kCatVsAluValue : kCatPsAluValue);
      if (value == device_value)
      {
        Match(item);
      }
      else
      {
        Mismatch(r, item, "c{}.{} device={:08X} emitted={:08X}", idx / 4, "xyzw"[idx & 3], device_value, value);
      }
    }

    void CompareFetch(Walk &w, uint32_t d, uint32_t value)
    {
      AssertState &a = g_assert;
      const DrawRecord &r = *w.r;
      const uint32_t slot = d / kFetchSlotDwords;
      a.emitted[kMaskFetch] |= 1ull << (63 - slot);
      if (a.fetch_dwords[slot] < 255)
      {
        ++a.fetch_dwords[slot];
      }
      ++a.fetch_compared_frame;
      const uint32_t recorded = RecordFetchDword(r, d);
      const uint32_t item = kRegSpace + kCatFetchValue;

      uint32_t stream, part;
      const bool rebuilt = (w.pre->masks[kMaskRegsB] & kStreamsChangedBit) && StreamOfFetchDword(d, stream, part) &&
                           w.decl_max_stream != 0xFFFFFFFFu && stream <= w.decl_max_stream &&
                           stream < dev::kStreamCount && r.streams[stream].vb != 0;
      if (!rebuilt)
      {
        if (value == recorded)
        {
          Match(item);
        }
        else
        {
          Mismatch(r, item, "slot {} dword {} record={:08X} emitted={:08X} post-flush device={:08X}", slot,
                   d % kFetchSlotDwords, recorded, value, Load32(w.base, w.device + dev::kFetchConstants + 4 * d));
        }
        return;
      }

      // 4.2: the stream builder rewrote this dword inside the flush. Ground
      // truth is the post-flush device; our recompute (shadow_assert.md 1.1,
      // from the generated sub_826F9578) is asserted on top of it.
      const uint32_t post = Load32(w.base, w.device + dev::kFetchConstants + 4 * d);
      if (value != recorded)
      {
        ExpectedMismatch(r, kFixupStreamFetch, kStructuralWhy[kFixupStreamFetch - kFixupCount],
                         "stream {} dword{} record={:08X} emitted={:08X} post-flush={:08X}", stream, part, recorded,
                         value, post);
      }
      if (value == post)
      {
        Match(item);
      }
      else
      {
        Mismatch(r, item, "stream {} dword{} emitted={:08X} != post-flush device={:08X}", stream, part, value, post);
      }
      const Stream &st = r.streams[stream];
      uint32_t expected;
      if (part == 0)
      {
        expected = GuestToPacketAddress(st.base + st.offset);
      }
      else
      {
        expected = ((st.size - st.offset) & 0xCFFFFFFFu) | 0x10000000u;
      }
      const uint32_t rebuild_item = kRegSpace + kCatStreamRebuild;
      if (expected == value)
      {
        Match(rebuild_item);
      }
      else
      {
        Mismatch(r, rebuild_item, "stream {} dword{} recompute={:08X} (vb={:08X} base={:08X}+{} size={}) emitted={:08X}",
                 stream, part, expected, st.vb, st.base, st.offset, st.size, value);
      }
    }

    // Loop constants: index i < 16 = VS i i, i >= 16 = PS i (i - 16). The non-zero / zero-count tallies per half
    // feed the one-line summary in AssertDumpTotals (a PS light loop over i16 with count 0 draws no light).
    struct LoopStats
    {
      uint64_t dwords = 0;
      uint64_t nonzero[2] = {0, 0};    // [0] VS half (0x4908..0x4917), [1] PS half (0x4918..0x4927)
      uint64_t zero_count[2] = {0, 0}; // non-zero dwords whose count (bits 7:0) is 0 anyway
    };
    LoopStats g_loops;

    void CompareLoop(Walk &w, uint32_t idx, uint32_t value)
    {
      const DrawRecord &r = *w.r;
      LoopStats &s = g_loops;
      const uint32_t half = idx < 16 ? 0 : 1;
      ++s.dwords;
      if (value != 0)
      {
        ++s.nonzero[half];
        s.zero_count[half] += (value & 0xFFu) == 0;
      }
      const uint32_t device_value = Load32(w.base, w.device + dev::kLoopConstants + 4 * idx);
      const uint32_t item = kRegSpace + kCatLoopValue;
      if (value == device_value)
      {
        Match(item);
      }
      else
      {
        Mismatch(r, item, "loop dword {} ({} i{}) device+{}={:08X} emitted={:08X}", idx, half ? "PS" : "VS", idx & 15,
                 dev::kLoopConstants + 4 * idx, device_value, value);
      }
    }

    void CompareBool(Walk &w, uint32_t idx, uint32_t value)
    {
      AssertState &a = g_assert;
      const DrawRecord &r = *w.r;
      a.emitted[kMaskFetch] |= kBoolBit;
      if (a.bool_dwords < 255)
      {
        ++a.bool_dwords;
      }
      const uint32_t recorded = Swap32(r.bools_be[idx]);
      const uint32_t item = kRegSpace + kCatBoolValue;
      if (value == recorded)
      {
        Match(item);
      }
      else
      {
        Mismatch(r, item, "bool dword {} record={:08X} emitted={:08X}", idx, recorded, value);
      }
    }

    void CheckImLoad(Walk &w, uint32_t addr_type, uint32_t start_size)
    {
      const DrawRecord &r = *w.r;
      const uint32_t type = addr_type & 3;
      const uint32_t addr = addr_type & ~3u;
      const uint32_t item = kRegSpace + kCatImLoad;
      uint32_t object, ucode_field, bytes_field;
      const char *what;
      if (type == 0)
      {
        // sub_826FA2E8: the vertex program object at device+12948 (+40 ucode,
        // +600 size), not the D3DVertexShader the game set (+20456).
        object = Load32(w.base, w.device + kDevVsProgram);
        ucode_field = kVsProgramUcode;
        bytes_field = kVsProgramUcodeBytes;
        what = "VS";
        AssertState &a = g_assert;
        if (!a.vs_slot_logged && object != r.vertex_shader)
        {
          a.vs_slot_logged = true;
          REXLOG_INFO("shadow assert: note: device+12948 (VS program {:08X}) differs from the record's +20456 "
                      "(D3DVertexShader {:08X}); IM_LOAD is checked against +12948",
                      object, r.vertex_shader);
        }
      }
      else
      {
        object = r.pixel_shader;
        ucode_field = kPsObjectUcode;
        bytes_field = kPsObjectUcodeBytes;
        what = "PS";
      }
      if (!IsGuestPointer(object))
      {
        Mismatch(r, item, "{} IM_LOAD addr={:08X} but no shader object ({:08X})", what, addr, object);
        return;
      }
      const uint32_t ucode = Load32(w.base, object + ucode_field);
      const uint32_t expected_addr = GuestToPacketAddress(ucode) & ~3u;
      const uint32_t expected_size = Load32(w.base, object + bytes_field) >> 2;
      const uint32_t size = start_size & 0xFFFFu;
      if (addr == expected_addr && size == expected_size)
      {
        Match(item);
      }
      else
      {
        Mismatch(r, item, "{} IM_LOAD addr={:08X} size={} dwords; object {:08X} ucode={:08X} -> {:08X} size={}", what,
                 addr, size, object, ucode, expected_addr, expected_size);
      }
    }

    // Parses the dwords the flush wrote: (ring_before, ring_after], guest
    // addresses, every dword big-endian.
    void WalkPackets(Walk &w, uint32_t first, uint32_t last)
    {
      AssertState &a = g_assert;
      const DrawRecord &r = *w.r;
      uint32_t at = first;
      while (at <= last)
      {
        const uint32_t hdr = Load32(w.base, at);
        const uint32_t type = hdr >> 30;
        if (type == 2)
        {
          at += 4; // NOP padding (ALU / fetch writers)
          continue;
        }
        if (type == 1)
        {
          Mismatch(r, kRegSpace + kCatParse, "type-1 packet header {:08X} at {:08X}", hdr, at);
          return;
        }
        const uint32_t count = ((hdr >> 16) & 0x3FFFu) + 1;
        if (at + 4 * count > last)
        {
          Mismatch(r, kRegSpace + kCatParse, "packet {:08X} at {:08X} ({} dwords) runs past the window end {:08X}", hdr,
                   at, count, last);
          return;
        }
        if (type == 3)
        {
          const uint32_t opcode = (hdr >> 8) & 0x7Fu;
          ++a.type3_frame[opcode];
          if (opcode == kPm4ImLoad && count >= 2)
          {
            CheckImLoad(w, Load32(w.base, at + 4), Load32(w.base, at + 8));
          }
          at += 4 * (count + 1);
          continue;
        }
        // type 0
        const uint32_t reg0 = hdr & 0x7FFFu;
        for (uint32_t i = 0; i < count; ++i)
        {
          const uint32_t reg = reg0 + i;
          const uint32_t value = Load32(w.base, at + 4 + 4 * i);
          if (reg >= kRegFirst && reg <= kRegLast)
          {
            CompareShadowRegister(w, reg, value);
          }
          else if (reg >= kVsAluBase && reg < kVsAluBase + kAluDwords)
          {
            CompareAlu(w, 0, reg - kVsAluBase, value);
          }
          else if (reg >= kPsAluBase && reg < kPsAluBase + kAluDwords)
          {
            CompareAlu(w, 1, reg - kPsAluBase, value);
          }
          else if (reg >= kFetchBase && reg < kFetchBase + kFetchDwords)
          {
            CompareFetch(w, reg - kFetchBase, value);
          }
          else if (reg >= kBoolBase && reg < kBoolBase + dev::kBoolConstantCount)
          {
            CompareBool(w, reg - kBoolBase, value);
          }
          else if (reg >= kLoopBase && reg < kLoopBase + kLoopDwords)
          {
            CompareLoop(w, reg - kLoopBase, value);
          }
          else
          {
            Mismatch(r, kRegSpace + kCatOtherType0, "register 0x{:04X} emitted={:08X} (packet {:08X} at {:08X})", reg,
                     value, hdr, at);
          }
        }
        at += 4 * (count + 1);
      }
    }

    // Every dirty bit must have been emitted and nothing else (with the
    // documented exceptions).
    void CheckCoverage(Walk &w)
    {
      AssertState &a = g_assert;
      const DrawRecord &r = *w.r;
      const Pre &pre = *w.pre;
      for (uint32_t m = 0; m < dev::kDirtyMaskCount; ++m)
      {
        const uint64_t expected = pre.masks[m] & kRegisterBits[m];
        uint64_t allowed_extra = 0;
        if (m == kMaskFetch && (pre.masks[kMaskRegsB] & kStreamsChangedBit))
        {
          allowed_extra |= kStreamFetchSlotBits; // 4.2
        }
        if (m == kMaskRegsA && (pre.masks[kMaskRegsB] & kProgramBits))
        {
          allowed_extra |= kProgramLoaderRegBits; // 4.3
        }
        const uint64_t missing = expected & ~a.emitted[m];
        const uint64_t unexpected = a.emitted[m] & ~expected & ~allowed_extra;
        const uint64_t program_extra = a.emitted[m] & ~expected & allowed_extra & (m == kMaskRegsA ? ~0ull : 0ull);
        if (program_extra)
        {
          ExpectedMismatch(r, kFixupMaskProgramRegs, kStructuralWhy[kFixupMaskProgramRegs - kFixupCount],
                           "mask {} extra bits {:016X}", m + 1, program_extra);
        }
        for (uint32_t bit = 0; bit < 64; ++bit)
        {
          const uint64_t b = 1ull << bit;
          const bool is_missing = (missing & b) != 0;
          const bool is_unexpected = (unexpected & b) != 0;
          if (!is_missing && !is_unexpected)
          {
            continue;
          }
          const char *state = is_missing ? "dirty but not emitted" : "emitted but not dirty";
          if (m == kMaskVsAlu || m == kMaskPsAlu)
          {
            const uint32_t group = 63 - bit;
            Mismatch(r, kRegSpace + (m == kMaskVsAlu ? kCatVsAluRange : kCatPsAluRange), "c{}-c{} {}", group * 4,
                     group * 4 + 3, state);
          }
          else if (m == kMaskFetch && bit >= 32)
          {
            Mismatch(r, kRegSpace + kCatFetchRange, "fetch slot {} {}", 63 - bit, state);
          }
          else if (m == kMaskFetch && bit == 31)
          {
            Mismatch(r, kRegSpace + kCatBoolRange, "bool constants {}", state);
          }
          else
          {
            const uint32_t reg = RegOfBit(m, bit);
            Mismatch(r, reg ? reg - kRegFirst : kRegSpace + kCatOtherType0, "{} (mask {} bit {}) {}", RegName(reg),
                     m + 1, bit, state);
          }
        }
      }
      // Group / slot completeness.
      for (uint32_t bank = 0; bank < 2; ++bank)
      {
        for (uint32_t group = 0; group < 64; ++group)
        {
          const uint8_t n = a.alu_dwords[bank][group];
          if (n != 0 && n != kAluGroupDwords)
          {
            Mismatch(r, kRegSpace + (bank == 0 ? kCatVsAluRange : kCatPsAluRange), "c{}-c{} emitted with {} dwords",
                     group * 4, group * 4 + 3, n);
          }
        }
      }
      for (uint32_t slot = 0; slot < dev::kFetchConstantCount; ++slot)
      {
        const uint8_t n = a.fetch_dwords[slot];
        if (n != 0 && n != kFetchSlotDwords)
        {
          Mismatch(r, kRegSpace + kCatFetchRange, "fetch slot {} emitted with {} dwords", slot, n);
        }
      }
      if (a.bool_dwords != 0 && a.bool_dwords != dev::kBoolConstantCount)
      {
        Mismatch(r, kRegSpace + kCatBoolRange, "bool run emitted with {} dwords", a.bool_dwords);
      }
    }

    void AfterFlush(uint8_t *base, uint32_t device, const Pre &pre, uint32_t lr)
    {
      AssertState &a = g_assert;
      State &s = Get();

      // Frame bookkeeping (fallback when AssertOnSwap is not wired: the
      // summary of the previous frame goes out at its successor's first flush).
      if (a.frame != kNoFrame && a.frame != s.frame)
      {
        EmitSummary();
      }
      if (a.frame == kNoFrame)
      {
        a.frame = s.frame;
      }

      // Pair with the newest record.
      if (s.head == 0)
      {
        NoteUnpaired(lr);
        return;
      }
      const DrawRecord &r = s.ring[(s.head - 1) & RingMask()];
      if ((a.have_last && r.sequence == a.last_sequence) || r.device != device || r.kind == Kind::kSwap ||
          r.kind == Kind::kNone || r.tiling_flag == 0xFF)
      {
        NoteUnpaired(lr);
        return;
      }
      a.have_last = true;
      a.last_sequence = r.sequence;
      a.flush_device = device;
      a.draw_pending = false;
      ++a.draws_frame;
      ++a.draws_total;

      const uint32_t ring_after = Load32(base, device + dev::kRingWritePtr);
      const uint32_t limit_after = Load32(base, device + kDevRingLimit);
      a.flush_ring_after = ring_after;

      // The flush clears every mask it consumed.
      for (uint32_t m = 0; m < dev::kDirtyMaskCount; ++m)
      {
        const uint64_t after = Load64(base, device + dev::kDirtyMasks + 8 * m);
        if (after != 0)
        {
          Mismatch(r, kRegSpace + kCatMaskNotCleared, "mask {} = {:016X} after the flush", m + 1, after);
        }
      }

      // Record masks vs flush-entry masks (4.1: bit 61 of mask 4 may be new).
      for (uint32_t m = 0; m < dev::kDirtyMaskCount; ++m)
      {
        uint64_t diff = r.dirty[m] ^ pre.masks[m];
        if (m == kMaskRegsA && (diff & kVgtIndxOffsetBit) && (pre.masks[m] & kVgtIndxOffsetBit))
        {
          ExpectedMismatch(r, kFixupMaskVgtIndx, kStructuralWhy[kFixupMaskVgtIndx - kFixupCount],
                           "record mask 4={:016X} flush-entry={:016X}", r.dirty[m], pre.masks[m]);
          diff &= ~kVgtIndxOffsetBit;
        }
        if (m == kMaskRegsB && (diff & kStreamsChangedBit) && (r.dirty[m] & kStreamsChangedBit))
        {
          // Set when the game called SetStreamSource; the draw rebuilds the stream fetch constants and clears
          // the bit before it calls the flush, so the flush-entry mask never has it (expected, logs 002/022).
          ExpectedMismatch(r, kFixupMaskStreams, kStructuralWhy[kFixupMaskStreams - kFixupCount],
                           "record mask 5={:016X} flush-entry={:016X}", r.dirty[m], pre.masks[m]);
          diff &= ~kStreamsChangedBit;
        }
        if (diff)
        {
          Mismatch(r, kRegSpace + kCatMaskDiff, "mask {} record={:016X} flush-entry={:016X} (xor {:016X})", m + 1,
                   r.dirty[m], pre.masks[m], diff);
        }
      }
      if ((r.kind == Kind::kDrawIndexedVertices || r.kind == Kind::kDrawVertices) && pre.indx_offset != r.a[1])
      {
        Mismatch(r, 0x2102 - kRegFirst, "flush-entry VGT_INDX_OFFSET={:08X} != base vertex argument {:08X}",
                 pre.indx_offset, r.a[1]);
      }

      // Window.
      if (limit_after != pre.limit_before || ring_after < pre.ring_before ||
          ring_after - pre.ring_before > kMaxFlushBytes || !IsGuestPointer(pre.ring_before) ||
          !IsGuestPointer(ring_after))
      {
        ++a.wrapped_frame; // 4.9: segment change inside the writers, packets split
        return;
      }
      a.draw_pending = r.kind == Kind::kDrawIndexedVertices || r.kind == Kind::kDrawVertices ||
                       r.kind == Kind::kDrawVerticesUP || r.kind == Kind::kBeginVertices ||
                       r.kind == Kind::kDrawIndexedVerticesUP;

      std::memset(a.emitted, 0, sizeof(a.emitted));
      std::memset(a.alu_dwords, 0, sizeof(a.alu_dwords));
      std::memset(a.fetch_dwords, 0, sizeof(a.fetch_dwords));
      a.bool_dwords = 0;

      Walk w{base, device, &r, &pre, 0xFFFFFFFFu};
      if (IsGuestPointer(r.vertex_decl))
      {
        const DeclEntry *decl = FindDeclaration(r.vertex_decl);
        w.decl_max_stream = decl ? decl->max_stream : Load32(base, r.vertex_decl + dev::kDeclMaxStream);
      }
      if (ring_after != pre.ring_before)
      {
        WalkPackets(w, pre.ring_before + 4, ring_after);
      }
      CheckCoverage(w);
    }
  } // namespace

  // ---------------------------------------------------------------------------
  // Public entry points (shadow.h)
  // ---------------------------------------------------------------------------
  void AssertOnSwap()
  {
    AssertState &a = g_assert;
    if (!a.tables_ready)
    {
      return;
    }
    if (a.frame != kNoFrame)
    {
      EmitSummary();
    }
    // Per-register totals table every dump interval (reuses the recorder's cvar).
    const int32_t every = REXCVAR_GET(pdz_render_shadow_dump_frames);
    const uint32_t frame = Get().frame;
    if (a.draws_total && every > 0 && frame % uint32_t(every) == 0)
    {
      AssertDumpTotals();
    }
  }

  void AssertDumpTotals()
  {
    const AssertState &a = g_assert;
    REXLOG_INFO("shadow assert: totals after {} draws: {} mismatches, {} logged; per item (matches/mismatches):",
                a.draws_total, a.mismatches_total, a.logged);
    REXLOG_INFO("shadow assert:   loop constants 0x4908..0x4927 vs device+{}: {} dwords, {} mismatches; non-zero VS "
                "i0..15 {} (count 0: {}), PS i0..15 = 16..31 {} (count 0: {})",
                dev::kLoopConstants, g_loops.dwords, a.item_mismatch_total[kRegSpace + kCatLoopValue],
                g_loops.nonzero[0], g_loops.zero_count[0], g_loops.nonzero[1], g_loops.zero_count[1]);
    for (uint32_t item = 0; item < kItemCount; ++item)
    {
      if (a.item_mismatch_total[item] || (item >= kRegSpace && a.item_match_total[item]))
      {
        REXLOG_INFO("shadow assert:   {}{}: {} / {}", ItemName(item),
                    item < kRegSpace ? fmt::format(" (0x{:04X})", kRegFirst + item) : "", a.item_match_total[item],
                    a.item_mismatch_total[item]);
      }
    }
    for (const UnpairedCaller &u : a.unpaired)
    {
      if (u.lr)
      {
        REXLOG_INFO("shadow assert:   unpaired flush caller lr={:08X}: {}", u.lr, u.count);
      }
    }
  }

  void AssertOnDrawReturn(uint8_t *base)
  {
    AssertState &a = g_assert;
    if (!a.tables_ready || !a.draw_pending || !a.have_last)
    {
      return;
    }
    a.draw_pending = false;
    const State &s = Get();
    const DrawRecord &r = s.ring[(s.head - 1) & RingMask()];
    if (r.sequence != a.last_sequence)
    {
      return;
    }
    const uint32_t device = a.flush_device;
    const uint32_t ring_now = Load32(base, device + dev::kRingWritePtr);
    const uint32_t first = a.flush_ring_after + 4;
    if (ring_now < first || !IsGuestPointer(first) || !IsGuestPointer(ring_now))
    {
      return; // segment change after the flush; nothing to say
    }
    uint32_t expected_count;
    switch (r.kind)
    {
    case Kind::kDrawIndexedVertices: expected_count = r.a[3]; break;
    case Kind::kDrawVertices: expected_count = r.a[2]; break;
    case Kind::kDrawIndexedVerticesUP: expected_count = r.a[3]; break; // index count
    default: expected_count = r.a[1]; break; // DrawVerticesUP, BeginVertices
    }
    // DrawVerticesUP / BeginVertices put vertex data into the ring too, so the
    // window is scanned for the PM4_DRAW_INDX header instead of parsed.
    const uint32_t limit = ring_now - first > 4 * kMaxDrawPacketScan ? first + 4 * kMaxDrawPacketScan : ring_now;
    for (uint32_t at = first; at + 4 <= limit; at += 4)
    {
      const uint32_t hdr = Load32(base, at);
      if ((hdr >> 30) != 3 || ((hdr >> 8) & 0x7Fu) != kPm4DrawIndx)
      {
        continue;
      }
      const uint32_t initiator = Load32(base, at + 8); // dword0 = viz query, dword1 = VGT_DRAW_INITIATOR
      const uint32_t prim = initiator & 0x3Fu;
      const uint32_t count = initiator >> 16;
      const uint32_t item = kRegSpace + kCatDrawPacket;
      if (prim == r.a[0] && count == expected_count)
      {
        Match(item);
      }
      else
      {
        Mismatch(r, item, "VGT_DRAW_INITIATOR={:08X} (prim {} count {} src_sel {}) vs arguments prim {} count {}",
                 initiator, prim, count, (initiator >> 6) & 3, r.a[0], expected_count);
      }
      return;
    }
    Mismatch(r, kRegSpace + kCatDrawPacket, "no PM4_DRAW_INDX in {:08X}..{:08X}", first, limit);
  }
} // namespace pdz::shadow

// -----------------------------------------------------------------------------
// The hook: sub_826FA8B8(r3 = device). Called by every draw path, Clear,
// Resolve and seven internal sites; forwards in the middle.
// -----------------------------------------------------------------------------
REX_HOOK_RAW(sub_826FA8B8)
{
  if (pdz::census::Enabled())
  {
    pdz::census::OnFlush(ctx.r3.u32, uint32_t(ctx.lr)); // gpu_census.h: unpaired flushes by return address
  }
  using namespace pdz::shadow;
  if (!REXCVAR_GET(pdz_render_shadow_assert))
  {
    __imp__sub_826FA8B8(ctx, base);
    return;
  }
  // pdz_frame_timing_phases (drawcost): this hook's own work around the guest flush, charged to shadow.assert and
  // subtracted from the enclosing guest D3D call (guest.d3d).
  const uint64_t assert_t0 = pdz::timing::PhaseLevel() > 0 ? __rdtsc() : 0;
  AssertState &a = g_assert;
  if (!a.tables_ready)
  {
    BuildTables();
  }
  if (!Enabled())
  {
    if (!a.warned_no_recorder)
    {
      a.warned_no_recorder = true;
      REXLOG_WARN("shadow assert: pdz_render_shadow_assert is on but pdz_render_shadow is off: nothing to compare");
    }
    __imp__sub_826FA8B8(ctx, base);
    return;
  }
  if (!a.announced)
  {
    a.announced = true;
    REXLOG_INFO("shadow assert: on ({} fixup registers ignored by table, first {} mismatches logged)", kFixupCount,
                kMaxLoggedMismatches);
  }
  const uint32_t device = ctx.r3.u32;
  if (!IsGuestPointer(device))
  {
    __imp__sub_826FA8B8(ctx, base);
    return;
  }
  Pre pre;
  for (uint32_t m = 0; m < dev::kDirtyMaskCount; ++m)
  {
    pre.masks[m] = Load64(base, device + dev::kDirtyMasks + 8 * m);
  }
  pre.ring_before = Load32(base, device + dev::kRingWritePtr);
  pre.limit_before = Load32(base, device + kDevRingLimit);
  pre.indx_offset = Load32(base, device + kDevVgtIndxOffset);
  const uint32_t lr = uint32_t(ctx.lr);

  const uint64_t assert_t1 = assert_t0 ? __rdtsc() : 0;
  __imp__sub_826FA8B8(ctx, base);
  const uint64_t assert_t2 = assert_t0 ? __rdtsc() : 0;

  AfterFlush(base, device, pre, lr);
  if (assert_t0)
  {
    pdz::timing::ChargeOutside(pdz::timing::Phase::kShadowAssert, (assert_t1 - assert_t0) + (__rdtsc() - assert_t2));
  }
}
