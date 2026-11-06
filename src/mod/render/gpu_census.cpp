// GPU census (gpu_census.h; STAGE2_PLAN.md section 4 "M0"): the pdz_gpu_census switches, the title-side counter
// hooks, the flush pairing and the pdz_census log lines. The PM4 / fence / interrupt / MMIO counters are the SDK's
// (rex/graphics/gpu_census.h); this file switches them on and formats them.
//
// Every hook here forwards to the original unchanged and only counts (by the caller's return address) while the
// census is on. Call sites and callers (TU3, generated\default):
//   sub_826FA8B8 D3D state flush: 7 call sites, all in draw entries: 826E9840 BeginVertices, 826E9B84
//     BeginIndexedVertices, 826E9F34 DrawVertices, 826EA180 DrawIndexedVertices (hooked by the shadow files; the
//     DrawIndexedVerticesUP hook covers BeginIndexedVertices) and 826EA4A0 sub_826EA3C8, 826EA818 sub_826EA688,
//     82700AB8 sub_82700A68 (not hooked; the last two have no direct callers). Clear, ClearF and Resolve do not
//     flush, so a flush pairs only with a draw record.
//   sub_826E0AF8 ClearF <- Clear (826E0AB0, 826E0AE0), Resolve (826F2E04), the game directly (82761A24, 8276213C).
//   sub_826E4680 SetShaderGPRAllocation <- 826215A4, 826217E4, 82621898, 826219F0, 82621A58, 82625FB4, 826F5C48:
//     each call emits 24 one-vertex POINTLIST draws through sub_826E0270 (stage1\subagents\flarehook.md).
//   sub_826DF568 <- 826053C8, 82605FF4, 8260DF7C: EVENT_WRITE 15 only.
//   sub_826DE910 InsertCallback <- 825F654C (de-rez sub_825F64E8), 825FB860, 82AE0590, 82AE3C00.
//   sub_826EC238 texture lock (r6 = flags) <- 22 sites; sub_826DE880 BlockUntilNotBusy, tail-called by 8240B018 /
//     8240B9D0 (the LR is their caller); sub_826DE258 fence block <- 13 D3D sites; sub_826DE6A0 BlockUntilIdle <-
//     24 sites (game: 82225420, 82225468, 82225580, 8239D57C, 827458C0, 827459E8).
#include "gpu_census.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <windows.h>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/graphics/gpu_census.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>

#include "frame_timing.h"        // cpusplit: wait / guest D3D phases
#include "game_thread_sampler.h" // cpusplit: sampler sections
#include "gpu_sync.h"
#include "native/native_textures.h" // OnTextureLock (pdz_native_texture_lock_check)
#include "shadow.h"
#include "shadow_record.h"
#include "native/native.h"
#include "native/native_watchdog.h"

REXCVAR_DEFINE_BOOL(pdz_gpu_census, false, "Render",
                    "GPU census (native renderer stage 2, M0): counts what the game uses of the emulator's GPU path "
                    "(PM4 packets, fences, waits, interrupts, MMIO, CPU readbacks, draws that bypass the native "
                    "hooks); a pdz_census line every pdz_gpu_census_every swaps and a SUMMARY at test exit. Logging "
                    "only. Set it at startup (command line or settings) to count from the first ring packet.");
REXCVAR_DEFINE_INT32(pdz_gpu_census_every, 600, "Render",
                     "With pdz_gpu_census on: swaps per periodic pdz_census line (0 = SUMMARY only)")
    .range(0, 1000000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REX_EXTERN(__imp__sub_826EA688); // DrawIndexedVertices variant (flush lr 826EA818), not hooked by the shadow files
REX_EXTERN(__imp__sub_826EA3C8); // DrawVertices-shaped draw entry (flush lr 826EA4A0), no direct callers
REX_EXTERN(__imp__sub_82700A68); // flush + packet copy (flush lr 82700AB8), no direct callers
REX_EXTERN(__imp__sub_826E9AD8); // BeginIndexedVertices (its only caller is the hooked DrawIndexedVerticesUP)
REX_EXTERN(__imp__sub_826E0AF8); // D3DDevice_ClearF
REX_EXTERN(__imp__sub_826E4680); // D3DDevice_SetShaderGPRAllocation
REX_EXTERN(__imp__sub_826DF568); // EVENT_WRITE 15 (HiZ / tile flush)
REX_EXTERN(__imp__sub_826EC238); // texture lock (LockRect helper; r6 = flags)
REX_EXTERN(__imp__sub_826DE880); // D3DResource_BlockUntilNotBusy
REX_EXTERN(__imp__sub_826DE258); // block until a GPU fence has passed
REX_EXTERN(__imp__sub_826DE6A0); // D3DDevice_BlockUntilIdle

namespace pdz::census
{
  namespace
  {
    namespace gc = rex::graphics::gpu_census;
    using pdz::shadow::Hook;
    using pdz::shadow::Kind;

    // The SDK switch follows the cvar from the moment it is set (command line or settings file), before the guest
    // starts, so the SDK counts from the first ring packet (ME_INIT). OnSwap re-syncs it as a fallback.
    [[maybe_unused]] const bool g_change_callback = [] {
      rex::cvar::RegisterChangeCallback("pdz_gpu_census", [](std::string_view, std::string_view) {
        gc::SetEnabled(REXCVAR_GET(pdz_gpu_census));
      });
      return true;
    }();

    // -------------------------------------------------------------------------
    // Title-side sites
    // -------------------------------------------------------------------------
    enum class Site : uint8_t
    {
      kUnpairedFlush,      // sub_826FA8B8 with no new draw record in the shadow ring (lr = the flush's return address)
      kFlushNoRecorder,    // sub_826FA8B8 while pdz_render_shadow is off (cannot pair; by return address)
      kDrawIndexedVariant, // sub_826EA688: x = primitive (r4), sum = index count (r7)
      kDrawEntry3C8,       // sub_826EA3C8: x = primitive (r4), sum = r6
      kPacketCopy,         // sub_82700A68: x = r4
      kBeginIndexed,       // sub_826E9AD8
      kClearF,             // sub_826E0AF8: x = flags (r4)
      kGprAllocation,      // sub_826E4680: x = r5 << 8 | r6 (the GPR split arguments)
      kHiZFlush,           // sub_826DF568
      kInsertCallback,     // sub_826DE910: x = callback (r5), y = type (r4)
      kLock,               // sub_826EC238: x = flags (r6)
      kBlockUntilNotBusy,  // sub_826DE880
      kFenceBlock,         // sub_826DE258: us = time blocked
      kBlockUntilIdle,     // sub_826DE6A0: us = time blocked
      kCount,
    };
    constexpr uint32_t kSiteCount = uint32_t(Site::kCount);

    constexpr const char *kSiteName[kSiteCount] = {
        "flush unpaired",
        "flush (recorder off)",
        "draw sub_826EA688",
        "draw sub_826EA3C8",
        "flush+copy sub_82700A68",
        "BeginIndexedVertices",
        "ClearF",
        "SetShaderGPRAllocation",
        "HiZ flush event",
        "InsertCallback",
        "texture lock",
        "BlockUntilNotBusy",
        "fence block",
        "BlockUntilIdle",
    };
    constexpr const char *kSiteX[kSiteCount] = {
        nullptr, nullptr, "prim", "prim", "r4", nullptr, "flags", "gpr", nullptr, "callback", "flags",
        nullptr, nullptr, nullptr,
    };
    // Short names used in the periodic line.
    constexpr const char *kSiteShort[kSiteCount] = {
        "unpaired", "flush-norec", "826EA688", "826EA3C8", "82700A68", "begin-idx", "clearf", "gpr-alloc",
        "hiz", "icb", "lock", "bunb", "fence", "idle",
    };

    // Return address -> enclosing function (TU3, every call site of the hooked functions; generated\default).
    struct LrFunction
    {
      uint32_t lr, function;
    };
    constexpr LrFunction kLrFunctions[] = {
        {0x82225420, 0x82225388}, {0x82225468, 0x82225388}, {0x82225580, 0x82225570}, {0x8226BC84, 0x8226BC30},
        {0x8239D57C, 0x8239D460}, {0x8240AE4C, 0x8240AD58}, {0x8240B584, 0x8240B528}, {0x825F51A8, 0x825F4EC0},
        {0x825F654C, 0x825F64E8}, {0x825FB860, 0x825FB850}, {0x826033FC, 0x82603398}, {0x826053C8, 0x826048F0},
        {0x82605FF4, 0x82605488}, {0x8260DF7C, 0x8260DF60}, {0x82612688, 0x82612628}, {0x82612728, 0x826126C8},
        {0x826127CC, 0x82612768}, {0x8261E83C, 0x8261DFC8}, {0x826215A4, 0x82621520}, {0x826217E4, 0x826216D0},
        {0x82621898, 0x826216D0}, {0x826219F0, 0x826216D0}, {0x82621A58, 0x826216D0}, {0x82625D0C, 0x82625CB0},
        {0x82625E2C, 0x82625DD0}, {0x82625FB4, 0x82625F98}, {0x8262DFF4, 0x8262DF88}, {0x826A5620, 0x826A5590},
        {0x826A6244, 0x826A5F88}, {0x826A9F00, 0x826A9EA0}, {0x826AA1F0, 0x826AA188}, {0x826AA2B8, 0x826AA188},
        {0x826DD5E4, 0x826DD4F8}, {0x826DD610, 0x826DD4F8}, {0x826DDF0C, 0x826DDE30}, {0x826DE0F0, 0x826DE088},
        {0x826DE710, 0x826DE6A0}, {0x826DE7D8, 0x826DE770}, {0x826DE8EC, 0x826DE880}, {0x826DEDD4, 0x826DEDB0},
        {0x826DF1EC, 0x826DEDB0}, {0x826DF380, 0x826DF360}, {0x826DF484, 0x826DF438}, {0x826E0AB0, 0x826E0A48},
        {0x826E0AE0, 0x826E0A48}, {0x826E1590, 0x826E1570}, {0x826E15C8, 0x826E1570}, {0x826E1880, 0x826E17F0},
        {0x826E18DC, 0x826E17F0}, {0x826E1934, 0x826E17F0}, {0x826E1B48, 0x826E1AB8}, {0x826E3960, 0x826E38D0},
        {0x826E3B18, 0x826E3A88}, {0x826E9840, 0x826E97B0}, {0x826E9B84, 0x826E9AD8}, {0x826E9E90, 0x826E9E18},
        {0x826E9F34, 0x826E9EE8}, {0x826EA180, 0x826EA128}, {0x826EA4A0, 0x826EA3C8}, {0x826EA818, 0x826EA688},
        {0x826EC358, 0x826EC328}, {0x826EC3F0, 0x826EC3B8}, {0x826EC7D8, 0x826EC740}, {0x826F2E04, 0x826F21D0},
        {0x826F2EB8, 0x826F21D0}, {0x826F5C48, 0x826F5AC0}, {0x826F5E68, 0x826F5E18}, {0x826F6498, 0x826F6348},
        {0x826F7590, 0x826F7578}, {0x826F7670, 0x826F75E8}, {0x826F7764, 0x826F75E8}, {0x826F77C0, 0x826F75E8},
        {0x826F7F48, 0x826F7DE0}, {0x826F8DB8, 0x826F8D40}, {0x826F8E08, 0x826F8D40}, {0x826FBCF4, 0x826FBCB8},
        {0x826FC8B8, 0x826FC888}, {0x82700AB8, 0x82700A68}, {0x8270FCF0, 0x8270FCB0}, {0x82710548, 0x827104C8},
        {0x8271B640, 0x8271B5A8}, {0x8271B720, 0x8271B680}, {0x8271B804, 0x8271B760}, {0x827458C0, 0x827458A8},
        {0x827459E8, 0x82745908}, {0x82761A24, 0x827619F8}, {0x8276213C, 0x82762048}, {0x82AE0590, 0x82AE0428},
        {0x82AE3C00, 0x82AE3858},
    };
    struct FunctionName
    {
      uint32_t function;
      const char *name;
    };
    constexpr FunctionName kFunctionNames[] = {
        {0x826E97B0, "BeginVertices, hooked"},
        {0x826E9AD8, "BeginIndexedVertices <- DrawIndexedVerticesUP, hooked"},
        {0x826E9E18, "DrawIndexedVerticesUP, hooked"},
        {0x826E9EE8, "DrawVertices, hooked"},
        {0x826EA128, "DrawIndexedVertices, hooked"},
        {0x826EA3C8, "draw entry, NOT hooked"},
        {0x826EA688, "DrawIndexedVertices variant, NOT hooked"},
        {0x82700A68, "flush + packet copy, NOT hooked"},
        {0x826E0A48, "Clear, hooked"},
        {0x826F21D0, "Resolve, hooked"},
        {0x827619F8, "game, ClearF direct"},
        {0x82762048, "game, ClearF direct"},
        {0x825F64E8, "de-rez snapshot"},
        {0x82AE0428, "movie player?"},
        {0x82AE3858, "movie player?"},
        {0x826DDE30, "D3D ring space"},
        {0x826DE6A0, "D3D BlockUntilIdle"},
        {0x826DE880, "D3D BlockUntilNotBusy"},
        {0x826E1AB8, "D3D resource lock"},
        {0x826DEDB0, "D3D ring init"},
        {0x826F5AC0, "D3D"},
    };

    std::string DescribeLr(uint32_t lr)
    {
      for (const LrFunction &e : kLrFunctions)
      {
        if (e.lr == lr)
        {
          for (const FunctionName &n : kFunctionNames)
          {
            if (n.function == e.function)
            {
              return fmt::format("{:08X} (sub_{:08X} {})", lr, e.function, n.name);
            }
          }
          return fmt::format("{:08X} (sub_{:08X})", lr, e.function);
        }
      }
      return fmt::format("{:08X}", lr);
    }

    // -------------------------------------------------------------------------
    // PM4 names and the STAGE2_PLAN section 3.2 classification of each type-3 opcode: 'x' = executed by the minimum
    // packet set, 's' = skipped by count, '-' = not listed there.
    // -------------------------------------------------------------------------
    struct OpcodeInfo
    {
      const char *name;
      char plan;
    };
    OpcodeInfo Opcode(uint32_t opcode)
    {
      switch (opcode)
      {
      case 0x10: return {"NOP", 's'};
      case 0x21: return {"REG_RMW", 'x'};
      case 0x22: return {"DRAW_INDX", 's'};
      case 0x23: return {"VIZ_QUERY", 's'};
      case 0x25: return {"SET_STATE", '-'};
      case 0x26: return {"WAIT_FOR_IDLE", 's'};
      case 0x27: return {"IM_LOAD", 's'};
      case 0x2B: return {"IM_LOAD_IMMEDIATE", 's'};
      case 0x2C: return {"IM_STORE", '-'};
      case 0x2D: return {"SET_CONSTANT", 'x'};
      case 0x2E: return {"LOAD_CONSTANT_CONTEXT", '-'};
      case 0x2F: return {"LOAD_ALU_CONSTANT", 'x'};
      case 0x34: return {"DRAW_INDX_BIN", 's'};
      case 0x35: return {"DRAW_INDX_2_BIN", 's'};
      case 0x36: return {"DRAW_INDX_2", 's'};
      case 0x37: return {"INDIRECT_BUFFER_PFD", 'x'};
      case 0x3B: return {"INVALIDATE_STATE", 's'};
      case 0x3C: return {"WAIT_REG_MEM", 'x'};
      case 0x3D: return {"MEM_WRITE", 'x'};
      case 0x3E: return {"REG_TO_MEM", 'x'};
      case 0x3F: return {"INDIRECT_BUFFER", 'x'};
      case 0x44: return {"COND_EXEC", '-'};
      case 0x45: return {"COND_WRITE", 'x'};
      case 0x46: return {"EVENT_WRITE", '-'};
      case 0x48: return {"ME_INIT", 's'};
      case 0x4A: return {"SET_SHADER_BASES", '-'};
      case 0x4B: return {"SET_BIN_BASE_OFFSET", '-'};
      case 0x4F: return {"MEM_WRITE_CNTR", '-'};
      case 0x50: return {"SET_BIN_MASK", 'x'};
      case 0x51: return {"SET_BIN_SELECT", 'x'};
      case 0x52: return {"WAIT_REG_EQ", '-'};
      case 0x53: return {"WAIT_REG_GTE", '-'};
      case 0x54: return {"INTERRUPT", 'x'};
      case 0x55: return {"SET_CONSTANT2", 'x'};
      case 0x56: return {"SET_SHADER_CONSTANTS", 'x'};
      case 0x58: return {"EVENT_WRITE_SHD", 'x'};
      case 0x59: return {"EVENT_WRITE_CFL", '-'};
      case 0x5A: return {"EVENT_WRITE_EXT", 'x'};
      case 0x5B: return {"EVENT_WRITE_ZPD", 'x'};
      case 0x5C: return {"WAIT_UNTIL_READ", '-'};
      case 0x5D: return {"WAIT_IB_PFD_COMPLETE", 's'};
      case 0x5E: return {"CONTEXT_UPDATE", 's'};
      case 0x60: return {"SET_BIN_MASK_LO", 'x'};
      case 0x61: return {"SET_BIN_MASK_HI", 'x'};
      case 0x62: return {"SET_BIN_SELECT_LO", 'x'};
      case 0x63: return {"SET_BIN_SELECT_HI", 'x'};
      case 0x64: return {"XE_SWAP", 'x'};
      default: return {"?", '-'};
      }
    }

    // VGT_EVENT_INITIATOR events (xenos.h Event).
    const char *EventName(uint32_t event)
    {
      switch (event)
      {
      case 0: return "VS_DEALLOC";
      case 1: return "PS_DEALLOC";
      case 2: return "VS_DONE_TS";
      case 3: return "PS_DONE_TS";
      case 4: return "CACHE_FLUSH_TS";
      case 5: return "CONTEXT_DONE";
      case 6: return "CACHE_FLUSH";
      case 7: return "VIZQUERY_START";
      case 8: return "VIZQUERY_END";
      case 9: return "SC_WAIT_WC";
      case 13: return "RST_PIX_CNT";
      case 14: return "RST_VTX_CNT";
      case 15: return "TILE_FLUSH";
      case 20: return "CACHE_FLUSH_AND_INV_TS";
      case 21: return "ZPASS_DONE";
      case 22: return "CACHE_FLUSH_AND_INV";
      case 25: return "SCREEN_EXT_INIT";
      case 26: return "SCREEN_EXT_RPT";
      case 27: return "VS_FETCH_DONE_TS";
      default: return "?";
      }
    }

    const char *PrimitiveName(uint32_t prim)
    {
      switch (prim)
      {
      case 0x00: return "none";
      case 0x01: return "POINTLIST";
      case 0x02: return "LINELIST";
      case 0x03: return "LINESTRIP";
      case 0x04: return "TRILIST";
      case 0x05: return "TRIFAN";
      case 0x06: return "TRISTRIP";
      case 0x07: return "TRI_WITH_WFLAGS";
      case 0x08: return "RECTLIST";
      case 0x0C: return "LINELOOP";
      case 0x0D: return "QUADLIST";
      case 0x0E: return "QUADSTRIP";
      case 0x0F: return "POLYGON";
      case 0x10: return "2D_COPY_RECT_V0";
      case 0x11: return "2D_COPY_RECT_V1";
      case 0x12: return "2D_COPY_RECT_V2";
      case 0x13: return "2D_COPY_RECT_V3";
      case 0x14: return "2D_FILL_RECT";
      case 0x15: return "2D_LINESTRIP";
      case 0x16: return "2D_TRISTRIP";
      default: return prim >= 0x10 ? "patch / explicit" : "?";
      }
    }

    struct RegName
    {
      uint32_t index;
      const char *name;
    };
    const RegName kRegNames[] = {
#define XE_GPU_REGISTER(index, type, name) {index, #name},
#include <rex/graphics/register_table.inc>
#undef XE_GPU_REGISTER
    };
    std::string RegisterName(uint32_t index)
    {
      for (const RegName &r : kRegNames)
      {
        if (r.index == index)
        {
          return r.name;
        }
      }
      return fmt::format("reg_{:04X}", index);
    }

    // -------------------------------------------------------------------------
    // State (one mutex; it is never held across a guest call or another lock)
    // -------------------------------------------------------------------------
    struct Entry
    {
      Site site;
      uint32_t lr;
      uint32_t x;
      uint32_t y;
      uint64_t count;
      uint64_t sum;
      uint64_t us;
      uint64_t max_us;
    };
    constexpr size_t kMaxEntries = 512;

    // The SDK counters the periodic line reports (copied at each window boundary).
    struct SdkSnap
    {
      uint64_t bursts = 0, burst_dwords = 0;
      uint64_t type0 = 0, type0_config = 0, type0_context = 0, type0_constants = 0, type1 = 0, type2 = 0;
      uint64_t type3[128] = {};
      uint64_t type3_total = 0, type3_indirect = 0;
      uint64_t indirect_buffers = 0;
      uint64_t draws = 0, draws_indirect = 0, resolves = 0, single_points = 0, draws_failed = 0;
      uint64_t waits = 0, waits_waited = 0, wait_us = 0;
      uint64_t cp_interrupts = 0, vblank_interrupts = 0, vblanks = 0, xe_swaps = 0;
      uint64_t mmio_reads = 0, mmio_writes = 0;
      uint64_t resolve_readbacks = 0;
      uint64_t rows_dropped = 0;
    };

    SdkSnap TakeSdk()
    {
      const gc::Census &c = gc::Get();
      auto v = [](const gc::Counter &counter) { return counter.load(std::memory_order_relaxed); };
      SdkSnap s;
      s.bursts = v(c.bursts);
      s.burst_dwords = v(c.burst_dwords);
      s.type0 = v(c.type0_packets[0]) + v(c.type0_packets[1]);
      s.type0_config = v(c.type0_range_dwords[gc::kRangeConfig]);
      s.type0_context = v(c.type0_range_dwords[gc::kRangeContext]);
      for (uint32_t r = gc::kRangeAluVertex; r < gc::kRangeCount; ++r)
      {
        s.type0_constants += v(c.type0_range_dwords[r]);
      }
      s.type1 = v(c.type1_packets[0]) + v(c.type1_packets[1]);
      s.type2 = v(c.type2_packets[0]) + v(c.type2_packets[1]);
      for (uint32_t op = 0; op < 128; ++op)
      {
        s.type3[op] = v(c.type3_packets[op][0]) + v(c.type3_packets[op][1]);
        s.type3_total += s.type3[op];
        s.type3_indirect += v(c.type3_packets[op][1]);
      }
      s.indirect_buffers = v(c.indirect_buffers);
      s.draws = v(c.draws[0]) + v(c.draws[1]);
      s.draws_indirect = v(c.draws[1]);
      s.resolves = v(c.resolves);
      s.single_points = v(c.draw_single_point);
      s.draws_failed = v(c.draw_failed);
      s.waits = v(c.wait_reg_mem);
      s.waits_waited = v(c.wait_reg_mem_waited);
      s.wait_us = v(c.wait_reg_mem_us);
      s.cp_interrupts = v(c.interrupts[1]);
      s.vblank_interrupts = v(c.interrupts[0]);
      s.vblanks = v(c.vblanks);
      s.xe_swaps = v(c.xe_swaps);
      s.mmio_reads = v(c.mmio_reads);
      s.mmio_writes = v(c.mmio_writes);
      s.resolve_readbacks = v(c.resolve_readbacks[0]) + v(c.resolve_readbacks[1]) + v(c.resolve_readbacks[2]);
      s.rows_dropped = v(c.rows_dropped);
      return s;
    }

    struct State
    {
      std::mutex mutex;
      Entry entries[kMaxEntries];
      size_t entry_count = 0;
      uint64_t entries_dropped = 0;
      uint64_t site_count[kSiteCount] = {};
      uint64_t site_us[kSiteCount] = {};

      // Flush pairing (shadow ring, like shadow_assert.cpp AfterFlush, but only draw records pair).
      bool have_last = false;
      uint32_t last_sequence = 0;
      uint64_t flushes = 0, flushes_paired = 0;

      // Frames (Swap hook calls with the census on) and the hooked call totals from the shadow recorder.
      uint64_t swaps = 0;
      uint64_t hooked[uint32_t(Hook::kCount)] = {};
      std::atomic<bool> sdk_on{false}; // what OnSwap last told the SDK (read without the mutex)
      bool announced = false;

      // D3D device fields sampled at Swap: +10384 fence pointer, +10388 scratch block, +15596 vblank callback,
      // +15600 the vblank count of D3D's interrupt handler (sub_826DE528 source 0).
      uint32_t device = 0, fence_ptr = 0, scratch_block = 0, vblank_callback = 0;
      uint32_t d3d_vblank_first = 0, d3d_vblank_last = 0;
      bool have_d3d_vblank = false;
      uint64_t vblank_callback_changes = 0;

      // The open window of the periodic line.
      uint64_t window_swaps = 0; // swaps at the window start
      SdkSnap window_sdk;
      uint64_t window_site_count[kSiteCount] = {};
      uint64_t window_site_us[kSiteCount] = {};
      uint64_t window_hooked[uint32_t(Hook::kCount)] = {};
      uint64_t window_flushes = 0;
      uint32_t window_d3d_vblank = 0;

      int64_t qpc_frequency = 0;
    };
    State g;

    int64_t Now()
    {
      LARGE_INTEGER t;
      QueryPerformanceCounter(&t);
      return int64_t(t.QuadPart);
    }

    uint64_t TicksToUs(int64_t ticks)
    {
      if (g.qpc_frequency == 0)
      {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        g.qpc_frequency = int64_t(f.QuadPart);
      }
      return ticks > 0 ? uint64_t(ticks) * 1000000ull / uint64_t(g.qpc_frequency) : 0;
    }

    // Call with g.mutex held.
    void NoteLocked(Site site, uint32_t lr, uint32_t x, uint32_t y, uint64_t sum, uint64_t us)
    {
      ++g.site_count[uint32_t(site)];
      g.site_us[uint32_t(site)] += us;
      for (size_t i = 0; i < g.entry_count; ++i)
      {
        Entry &e = g.entries[i];
        if (e.site == site && e.lr == lr && e.x == x)
        {
          ++e.count;
          e.sum += sum;
          e.us += us;
          e.max_us = std::max(e.max_us, us);
          e.y = y;
          return;
        }
      }
      if (g.entry_count == kMaxEntries)
      {
        ++g.entries_dropped;
        return;
      }
      g.entries[g.entry_count++] = Entry{site, lr, x, y, 1, sum, us, us};
    }

    void Note(Site site, uint32_t lr, uint32_t x = 0, uint32_t y = 0, uint64_t sum = 0, uint64_t us = 0)
    {
      std::lock_guard<std::mutex> lock(g.mutex);
      NoteLocked(site, lr, x, y, sum, us);
    }

    double PerFrame(uint64_t value, uint64_t frames) { return frames ? double(value) / double(frames) : 0.0; }

    // Physical (29-bit) address annotation from the D3D device fields. Call with g.mutex held.
    std::string AnnotateLocked(uint32_t address)
    {
      const uint32_t phys = address & 0x1FFFFFFFu;
      if (g.scratch_block)
      {
        const uint32_t scratch = g.scratch_block & 0x1FFFFFFFu;
        if (phys >= scratch && phys < scratch + 32)
        {
          static const char *const kScratch[8] = {"callback CPU mask", "presentation interval gate", "scratch 2",
                                                  "scratch 3", "callback", "callback context", "scratch 6",
                                                  "scratch 7"};
          return fmt::format(" [D3D scratch block +{}: {}]", phys - scratch, kScratch[(phys - scratch) / 4]);
        }
      }
      if (g.fence_ptr && phys == (g.fence_ptr & 0x1FFFFFFFu))
      {
        return " [D3D fence, read by the CPU in sub_826DE258]";
      }
      return "";
    }

    // CP draws (non-copy) the hooked and counted entry points account for: one draw packet per hooked draw entry
    // (DrawVerticesUP draws through the hooked BeginVertices), one per ClearF (Clear, Resolve and direct callers),
    // 24 per SetShaderGPRAllocation, one per unhooked draw entry.
    uint64_t ExpectedDraws(const uint64_t *hooked, const uint64_t *site_count)
    {
      return hooked[uint32_t(Hook::kDrawIndexedVertices)] + hooked[uint32_t(Hook::kDrawVertices)] +
             hooked[uint32_t(Hook::kBeginVertices)] + hooked[uint32_t(Hook::kDrawIndexedVerticesUP)] +
             site_count[uint32_t(Site::kClearF)] + 24 * site_count[uint32_t(Site::kGprAllocation)] +
             site_count[uint32_t(Site::kDrawIndexedVariant)] + site_count[uint32_t(Site::kDrawEntry3C8)];
    }

    // The periodic line: per-frame averages over the window that ends now. Call with g.mutex held.
    void LogWindowLocked()
    {
      const uint64_t frames = g.swaps - g.window_swaps;
      if (frames == 0)
      {
        return;
      }
      const SdkSnap now = TakeSdk();
      const SdkSnap &w = g.window_sdk;
      auto pf = [&](uint64_t a, uint64_t b) { return PerFrame(a - b, frames); };

      // The busiest type-3 opcodes of the window.
      std::vector<std::pair<uint64_t, uint32_t>> ops;
      for (uint32_t op = 0; op < 128; ++op)
      {
        if (now.type3[op] != w.type3[op])
        {
          ops.emplace_back(now.type3[op] - w.type3[op], op);
        }
      }
      std::sort(ops.begin(), ops.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
      std::string op_text;
      for (const auto &[n, op] : ops)
      {
        op_text += fmt::format(" {} {:.1f}", Opcode(op).name, PerFrame(n, frames));
      }

      uint64_t hooked_now[uint32_t(Hook::kCount)];
      std::memcpy(hooked_now, g.hooked, sizeof(hooked_now));
      uint64_t hooked_window[uint32_t(Hook::kCount)];
      for (uint32_t h = 0; h < uint32_t(Hook::kCount); ++h)
      {
        hooked_window[h] = hooked_now[h] - g.window_hooked[h];
      }
      uint64_t site_window[kSiteCount];
      std::string sites;
      for (uint32_t s = 0; s < kSiteCount; ++s)
      {
        site_window[s] = g.site_count[s] - g.window_site_count[s];
        if (site_window[s])
        {
          sites += fmt::format(" {} {:.2f}", kSiteShort[s], PerFrame(site_window[s], frames));
          const uint64_t us = g.site_us[s] - g.window_site_us[s];
          if (us)
          {
            sites += fmt::format(" ({:.3f} ms)", double(us) / 1000.0 / double(frames));
          }
        }
      }
      const uint64_t cp_draws = (now.draws - w.draws) - (now.resolves - w.resolves);
      const uint64_t expected = ExpectedDraws(hooked_window, site_window);
      REXLOG_INFO(
          "pdz_census: swaps {}-{} ({} frames; CP swaps {:.2f}/frame, vblanks {:.2f}, D3D vblanks {:.2f}) per frame: "
          "ring {:.0f} dw in "
          "{:.1f} bursts, {:.1f} IBs | type0 {:.1f} (dw: config {:.1f}, context {:.1f}, constants {:.1f}) type1 {:.1f} "
          "type2 {:.1f} | type3 {:.1f} (IB {:.0f}%):{} | CP draws {:.1f} (IB {:.1f}), resolves {:.2f}, 1-vertex "
          "points {:.1f}, failed {:.2f} | hooked draws {:.1f} clears {:.2f} resolves {:.2f}, expected CP draws "
          "{:.1f} (residual {:+.2f}) | waits {:.2f} (waited {:.2f}, {:.3f} ms) | interrupts CP {:.2f} vblank {:.2f} | "
          "mmio r {:.1f} w {:.1f} | resolve readbacks {:.2f} | flushes {:.1f}, title sites:{}",
          g.window_swaps + 1, g.swaps, frames, pf(now.xe_swaps, w.xe_swaps), pf(now.vblanks, w.vblanks),
          PerFrame(uint32_t(g.d3d_vblank_last - g.window_d3d_vblank), frames), pf(now.burst_dwords, w.burst_dwords),
          pf(now.bursts, w.bursts),
          pf(now.indirect_buffers, w.indirect_buffers), pf(now.type0, w.type0),
          pf(now.type0_config, w.type0_config), pf(now.type0_context, w.type0_context),
          pf(now.type0_constants, w.type0_constants), pf(now.type1, w.type1), pf(now.type2, w.type2),
          pf(now.type3_total, w.type3_total),
          now.type3_total > w.type3_total
              ? 100.0 * double(now.type3_indirect - w.type3_indirect) / double(now.type3_total - w.type3_total)
              : 0.0,
          op_text, PerFrame(cp_draws, frames), pf(now.draws_indirect, w.draws_indirect),
          pf(now.resolves, w.resolves), pf(now.single_points, w.single_points),
          pf(now.draws_failed, w.draws_failed),
          PerFrame(hooked_window[uint32_t(Hook::kDrawIndexedVertices)] + hooked_window[uint32_t(Hook::kDrawVertices)] +
                       hooked_window[uint32_t(Hook::kBeginVertices)] +
                       hooked_window[uint32_t(Hook::kDrawIndexedVerticesUP)],
                   frames),
          PerFrame(hooked_window[uint32_t(Hook::kClear)], frames),
          PerFrame(hooked_window[uint32_t(Hook::kResolve)], frames), PerFrame(expected, frames),
          (double(cp_draws) - double(expected)) / double(frames), pf(now.waits, w.waits),
          pf(now.waits_waited, w.waits_waited), double(now.wait_us - w.wait_us) / 1000.0 / double(frames),
          pf(now.cp_interrupts, w.cp_interrupts), pf(now.vblank_interrupts, w.vblank_interrupts),
          pf(now.mmio_reads, w.mmio_reads), pf(now.mmio_writes, w.mmio_writes),
          pf(now.resolve_readbacks, w.resolve_readbacks), PerFrame(g.flushes - g.window_flushes, frames),
          sites.empty() ? std::string(" none") : sites);

      // Next window.
      g.window_swaps = g.swaps;
      g.window_sdk = now;
      std::memcpy(g.window_site_count, g.site_count, sizeof(g.window_site_count));
      std::memcpy(g.window_site_us, g.site_us, sizeof(g.window_site_us));
      std::memcpy(g.window_hooked, hooked_now, sizeof(g.window_hooked));
      g.window_flushes = g.flushes;
      g.window_d3d_vblank = g.d3d_vblank_last;
    }

    // The settings that decide what the plugin does on top of the native path (STAGE2_PLAN 1.4).
    std::string Settings()
    {
      static constexpr const char *kNames[] = {
          "graphics_backend", "vsync", "gpu_max_frames_in_flight", "host_vblank_pacing", "swap_post_effect",
          "resolution_scale", "draw_resolution_scale_x", "draw_resolution_scale_y", "render_target_path_d3d12",
          "native_2x_msaa", "readback_resolve", "d3d12_readback_resolve", "readback_memexport", "occlusion_query",
          "occlusion_query_viz", "present_effect", "present_letterbox", "pdz_render_shadow",
          "pdz_render_shadow_assert", "pdz_native_renderer", "pdz_native_output", "pdz_native_scale",
          "pdz_fix_derez_snapshot"};
      std::string text;
      for (const char *name : kNames)
      {
        const std::string value = rex::cvar::GetFlagByName(name);
        text += fmt::format(" {}={}", name, value.empty() ? "?" : value);
      }
      return text;
    }
  } // namespace

  // ---------------------------------------------------------------------------
  // Entry points (gpu_census.h)
  // ---------------------------------------------------------------------------
  void OnFlush(uint32_t device, uint32_t lr)
  {
    std::lock_guard<std::mutex> lock(g.mutex);
    ++g.flushes;
    if (!pdz::shadow::Enabled())
    {
      NoteLocked(Site::kFlushNoRecorder, lr, 0, 0, 0, 0);
      return;
    }
    const pdz::shadow::State &s = pdz::shadow::Get();
    bool paired = false;
    if (s.head != 0)
    {
      const pdz::shadow::DrawRecord &r = s.ring[(s.head - 1) & pdz::shadow::RingMask()];
      // Only the draw entries flush (Clear, ClearF and Resolve do not): a flush pairs with a new draw record.
      const bool draw_kind = r.kind == Kind::kDrawIndexedVertices || r.kind == Kind::kDrawVertices ||
                             r.kind == Kind::kDrawVerticesUP || r.kind == Kind::kBeginVertices ||
                             r.kind == Kind::kDrawIndexedVerticesUP;
      if (draw_kind && r.device == device && r.tiling_flag != 0xFF &&
          !(g.have_last && r.sequence == g.last_sequence))
      {
        paired = true;
        g.have_last = true;
        g.last_sequence = r.sequence;
      }
    }
    if (paired)
    {
      ++g.flushes_paired;
    }
    else
    {
      NoteLocked(Site::kUnpairedFlush, lr, 0, 0, 0, 0);
    }
  }

  void OnInsertCallback(uint32_t lr, uint32_t type, uint32_t callback)
  {
    Note(Site::kInsertCallback, lr, callback, type);
  }

  void OnSwap(uint8_t *base, uint32_t device)
  {
    const bool on = Enabled();
    if (on != g.sdk_on.load(std::memory_order_relaxed))
    {
      gc::SetEnabled(on); // fallback for the change callback (also turns the SDK census off with the cvar)
      g.sdk_on.store(on, std::memory_order_relaxed);
    }
    if (!on)
    {
      return;
    }
    std::lock_guard<std::mutex> lock(g.mutex);
    if (!g.announced)
    {
      g.announced = true;
      const gc::Census &c = gc::Get();
      const uint64_t me_init = c.type3_packets[0x48][0].load(std::memory_order_relaxed) +
                               c.type3_packets[0x48][1].load(std::memory_order_relaxed);
      REXLOG_INFO("pdz_census: on at swap 1 of the census (SDK ring bursts so far {}, ME_INIT {}: {}); periodic line "
                  "every {} swaps; interrupt callback {:08X} data {:08X} (set {} times);{}",
                  c.bursts.load(std::memory_order_relaxed), me_init,
                  me_init ? "the census saw the ring from boot" : "the boot packets were missed",
                  REXCVAR_GET(pdz_gpu_census_every), c.interrupt_callback.load(std::memory_order_relaxed),
                  c.interrupt_callback_data.load(std::memory_order_relaxed),
                  c.set_interrupt_callback.load(std::memory_order_relaxed), Settings());
      // The first window starts at boot on both sides (SDK counters and title sites), like the totals.
    }
    ++g.swaps;
    // The frame the shadow recorder just closed (shadow_dump.cpp OnSwap moved it to last_counters).
    const pdz::shadow::State &s = pdz::shadow::Get();
    for (uint32_t h = 0; h < uint32_t(Hook::kCount); ++h)
    {
      g.hooked[h] += s.last_counters[h];
    }
    if (pdz::shadow::IsGuestPointer(device))
    {
      g.device = device;
      g.fence_ptr = pdz::shadow::Load32(base, device + 10384);
      g.scratch_block = pdz::shadow::Load32(base, device + 10388);
      const uint32_t callback = pdz::shadow::Load32(base, device + 15596);
      if (callback != g.vblank_callback)
      {
        if (g.have_d3d_vblank)
        {
          ++g.vblank_callback_changes;
        }
        g.vblank_callback = callback;
      }
      const uint32_t vblank = pdz::shadow::Load32(base, device + 15600);
      if (!g.have_d3d_vblank)
      {
        g.have_d3d_vblank = true;
        g.d3d_vblank_first = vblank;
        g.window_d3d_vblank = vblank;
      }
      g.d3d_vblank_last = vblank;
    }
    const int32_t every = REXCVAR_GET(pdz_gpu_census_every);
    if (every > 0 && g.swaps - g.window_swaps >= uint64_t(every))
    {
      LogWindowLocked();
    }
  }

  void LogSummary(const char *why)
  {
    if (!Enabled())
    {
      return;
    }
    // The native de-rez snapshot readbacks (takes the native render mutex: before ours).
    const uint32_t native_readbacks = pdz::native::Enabled() ? pdz::native::SnapshotReadbacks() : 0;
    std::lock_guard<std::mutex> lock(g.mutex);
    const gc::Census &c = gc::Get();
    auto v = [](const gc::Counter &counter) { return counter.load(std::memory_order_relaxed); };
    const uint64_t frames = g.swaps;
    const SdkSnap t = TakeSdk();

    // Opcodes seen, and the ones STAGE2_PLAN 3.2 does not list.
    std::string seen, unlisted;
    uint32_t seen_count = 0;
    for (uint32_t op = 0; op < 128; ++op)
    {
      if (t.type3[op])
      {
        ++seen_count;
        const OpcodeInfo info = Opcode(op);
        seen += fmt::format(" {:02X}", op);
        if (info.plan == '-')
        {
          unlisted += fmt::format(" {:02X} {}", op, info.name);
        }
      }
    }
    const uint64_t cp_draws = t.draws - t.resolves;
    const uint64_t expected = ExpectedDraws(g.hooked, g.site_count);
    const uint64_t unpaired = g.site_count[uint32_t(Site::kUnpairedFlush)];
    REXLOG_INFO(
        "pdz_census: SUMMARY ({}): frames {} (CP swaps {}, vblanks {}, D3D vblank count +{}) | per frame: ring {:.0f} "
        "dw, type0 {:.1f}, type3 {:.1f} (IB {:.0f}%), CP draws {:.1f}, resolves {:.2f}, waits {:.2f} (waited {:.2f}, "
        "{:.3f} ms), CP interrupts {:.2f} | type-3 opcodes seen {}:{} | not in STAGE2_PLAN 3.2:{} | CP draws {} vs "
        "expected from the hooks {} (residual {:+}) ; resolves {} vs Resolve hooks {} | flushes {}: unpaired {} | "
        "unhooked calls: 826EA688 {}, 826EA3C8 {}, 82700A68 {}, ClearF {} | resolve readbacks {} (title ranges {}), "
        "native snapshot readbacks {} | memexport draws {}, ZPD {}, VIZ {} | SDK rows dropped {}, title entries "
        "dropped {}",
        why, frames, t.xe_swaps, t.vblanks,
        g.have_d3d_vblank ? uint64_t(g.d3d_vblank_last - g.d3d_vblank_first) : 0, PerFrame(t.burst_dwords, frames),
        PerFrame(t.type0, frames), PerFrame(t.type3_total, frames),
        t.type3_total ? 100.0 * double(t.type3_indirect) / double(t.type3_total) : 0.0, PerFrame(cp_draws, frames),
        PerFrame(t.resolves, frames), PerFrame(t.waits, frames), PerFrame(t.waits_waited, frames),
        frames ? double(t.wait_us) / 1000.0 / double(frames) : 0.0, PerFrame(t.cp_interrupts, frames), seen_count,
        seen, unlisted.empty() ? std::string(" none") : unlisted, cp_draws, expected,
        int64_t(cp_draws) - int64_t(expected), t.resolves, g.hooked[uint32_t(Hook::kResolve)], g.flushes, unpaired,
        g.site_count[uint32_t(Site::kDrawIndexedVariant)], g.site_count[uint32_t(Site::kDrawEntry3C8)],
        g.site_count[uint32_t(Site::kPacketCopy)], g.site_count[uint32_t(Site::kClearF)], t.resolve_readbacks,
        v(c.resolve_readbacks[0]), native_readbacks, v(c.draw_memexport), v(c.zpd_events),
        v(c.viz_query[0]) + v(c.viz_query[1]), t.rows_dropped, g.entries_dropped);

    // ---- (1) packets -------------------------------------------------------------------------------------
    REXLOG_INFO("pdz_census: total ring: {} bursts, {} dwords; {} indirect buffers ({} dwords, deepest nesting {}); "
                "zero headers {}",
                t.bursts, t.burst_dwords, t.indirect_buffers, v(c.indirect_dwords), v(c.indirect_max_depth),
                v(c.zero_headers));
    static constexpr const char *kRangeName[gc::kRangeCount] = {
        "config 0000-1FFF", "context 2000-3FFF", "VS ALU 4000-43FF", "PS ALU 4400-47FF",
        "fetch 4800-48FF", "bool 4900-4907", "loop 4908-4927", "other 4928+"};
    REXLOG_INFO("pdz_census: total type0: {} packets (primary {}, IB {}; one-register form {}), type1 {} (IB {}), "
                "type2 {} (IB {})",
                t.type0, v(c.type0_packets[0]), v(c.type0_packets[1]), v(c.type0_single_register_packets), t.type1,
                v(c.type1_packets[1]), t.type2, v(c.type2_packets[1]));
    for (uint32_t r = 0; r < gc::kRangeCount; ++r)
    {
      if (v(c.type0_range_packets[r]) || v(c.type0_range_dwords[r]))
      {
        REXLOG_INFO("pdz_census: total type0 range {}: {} runs starting here, {} dwords ({:.1f} per frame)",
                    kRangeName[r], v(c.type0_range_packets[r]), v(c.type0_range_dwords[r]),
                    PerFrame(v(c.type0_range_dwords[r]), frames));
      }
    }
    for (uint32_t op = 0; op < 128; ++op)
    {
      if (!t.type3[op])
      {
        continue;
      }
      const OpcodeInfo info = Opcode(op);
      REXLOG_INFO("pdz_census: total type3 {:02X} {} [3.2: {}]: {} ({:.2f} per frame; primary {}, IB {}), {} payload "
                  "dwords, predicated {} (skipped {}), unhandled by the CP {}",
                  op, info.name,
                  info.plan == 'x' ? "execute" : (info.plan == 's' ? "skip" : "NOT LISTED"), t.type3[op],
                  PerFrame(t.type3[op], frames), v(c.type3_packets[op][0]), v(c.type3_packets[op][1]),
                  v(c.type3_dwords[op]), v(c.type3_predicated[op]), v(c.type3_skipped[op]),
                  v(c.type3_unhandled[op]));
    }
    // Registers below the constants (config: the side-effect registers; context: render state, summarised).
    uint64_t context_total = 0, context_distinct = 0;
    std::vector<std::pair<uint64_t, uint32_t>> context_top;
    for (uint32_t i = 0; i < gc::kCountedRegisters; ++i)
    {
      const uint64_t n = v(c.register_writes[i]);
      if (!n)
      {
        continue;
      }
      if (i < 0x2000)
      {
        REXLOG_INFO("pdz_census: total register {:04X} {}: {} writes ({:.2f} per frame)", i, RegisterName(i), n,
                    PerFrame(n, frames));
      }
      else
      {
        context_total += n;
        ++context_distinct;
        context_top.emplace_back(n, i);
      }
    }
    std::sort(context_top.begin(), context_top.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
    std::string top;
    for (size_t i = 0; i < context_top.size() && i < 12; ++i)
    {
      top += fmt::format(" {} {:.1f}", RegisterName(context_top[i].second), PerFrame(context_top[i].first, frames));
    }
    REXLOG_INFO("pdz_census: total context registers 2000-3FFF: {} distinct, {} writes ({:.1f} per frame); top per "
                "frame:{}",
                context_distinct, context_total, PerFrame(context_total, frames), top);
    static constexpr const char *kConstName[6] = {"ALU", "FETCH", "BOOL", "LOOP", "REGISTERS", "other"};
    std::string constants;
    for (uint32_t k = 0; k < 6; ++k)
    {
      if (v(c.set_constant[k][0]))
      {
        constants += fmt::format(" SET_CONSTANT {} {} pk / {} dw;", kConstName[k], v(c.set_constant[k][0]),
                                 v(c.set_constant[k][1]));
      }
      if (v(c.load_alu_constant[k][0]))
      {
        constants += fmt::format(" LOAD_ALU_CONSTANT {} {} pk / {} dw;", kConstName[k],
                                 v(c.load_alu_constant[k][0]), v(c.load_alu_constant[k][1]));
      }
    }
    REXLOG_INFO("pdz_census: total constant packets:{} SET_CONSTANT2 {} pk / {} dw; SET_SHADER_CONSTANTS {} pk / {} dw",
                constants.empty() ? std::string(" none of type 2D / 2F;") : constants, v(c.set_constant2[0]),
                v(c.set_constant2[1]), v(c.set_shader_constants[0]), v(c.set_shader_constants[1]));
    REXLOG_INFO("pdz_census: total shaders: IM_LOAD vertex {} pixel {}, IM_LOAD_IMMEDIATE vertex {} pixel {}, {} "
                "microcode dwords",
                v(c.im_load[0]), v(c.im_load[1]), v(c.im_load_immediate[0]), v(c.im_load_immediate[1]),
                v(c.im_load_dwords));

    // ---- draws ----------------------------------------------------------------------------------------------
    std::string prims;
    for (uint32_t p = 0; p < 64; ++p)
    {
      if (v(c.draw_primitive[p]))
      {
        prims += fmt::format(" {} {}", PrimitiveName(p), v(c.draw_primitive[p]));
      }
    }
    static constexpr const char *kEdramName[8] = {"no-op", "1", "2", "3", "color+depth", "depth-only", "copy", "7"};
    std::string edram;
    for (uint32_t m = 0; m < 8; ++m)
    {
      if (v(c.draw_edram_mode[m]))
      {
        edram += fmt::format(" {} {}", kEdramName[m], v(c.draw_edram_mode[m]));
      }
    }
    REXLOG_INFO("pdz_census: total draws: {} (primary {}, IB {}), {} indices; primitives:{} | index source: DMA {} "
                "immediate {} auto {} | EDRAM mode:{} | MSAA 1x {} 2x {} 4x {} | 32-bit indices {}, primitive reset {}, "
                "VIZ-predicated {}, tessellated {}, explicit major mode {}, 1-vertex points {}, failed in the backend "
                "{}, memexport {}",
                t.draws, v(c.draws[0]), v(c.draws[1]), v(c.draw_indices), prims, v(c.draw_source_select[0]),
                v(c.draw_source_select[1]), v(c.draw_source_select[2]), edram, v(c.draw_msaa[0]), v(c.draw_msaa[1]),
                v(c.draw_msaa[2]), v(c.draw_index32), v(c.draw_primitive_reset), v(c.draw_viz_conditional),
                v(c.draw_tessellated), v(c.draw_explicit_major), v(c.draw_single_point), v(c.draw_failed),
                v(c.draw_memexport));
    REXLOG_INFO("pdz_census: total resolves: {} (colour clear {}, depth clear {}, depth source {}); readbacks to "
                "guest memory: title ranges {} ({} bytes), readback_resolve mode {} ({} bytes), pass dump {} ({} "
                "bytes); native de-rez snapshot readbacks {}",
                t.resolves, v(c.resolve_color_clear), v(c.resolve_depth_clear), v(c.resolve_depth_source),
                v(c.resolve_readbacks[0]), v(c.resolve_readback_bytes[0]), v(c.resolve_readbacks[1]),
                v(c.resolve_readback_bytes[1]), v(c.resolve_readbacks[2]), v(c.resolve_readback_bytes[2]),
                native_readbacks);

    // ---- (2)-(5) rows: fences, waits, memory writes, interrupts, MMIO, swaps, resolves --------------------------
    REXLOG_INFO("pdz_census: total sync: WAIT_REG_MEM {} (waited {}, {:.1f} ms, vblank-paced {}), INTERRUPT packets "
                "per CPU {} {} {} {} {} {}, scratch write-backs {}, read-pointer write-backs {}, COHER requests {} "
                "(flushed {}), XE_SWAP {}, gamma writes 256-entry {} PWL {}, ZPD events {} (to the host {}), VIZ "
                "begin {} end {}",
                t.waits, t.waits_waited, double(t.wait_us) / 1000.0, v(c.wait_reg_mem_paced), v(c.interrupt_packets[0]),
                v(c.interrupt_packets[1]), v(c.interrupt_packets[2]), v(c.interrupt_packets[3]),
                v(c.interrupt_packets[4]), v(c.interrupt_packets[5]), v(c.scratch_writes), v(c.rptr_writes),
                v(c.coherency_requests), v(c.coherency_flushes), t.xe_swaps, v(c.gamma_256_writes),
                v(c.gamma_pwl_writes), v(c.zpd_events), v(c.zpd_reports_queued), v(c.viz_query[0]), v(c.viz_query[1]));
    REXLOG_INFO("pdz_census: total interrupts: vblanks {} (MarkVblank), dispatched vblank {} CP {}, with no callback "
                "{}; interrupt callback {:08X} data {:08X} set {} times; D3D vblank callback (device+15596) {:08X} "
                "(changed {} times), D3D vblank count +{} over {} frames; MMIO reads {} writes {}",
                t.vblanks, t.vblank_interrupts, t.cp_interrupts, v(c.interrupts_no_callback),
                c.interrupt_callback.load(std::memory_order_relaxed),
                c.interrupt_callback_data.load(std::memory_order_relaxed), v(c.set_interrupt_callback),
                g.vblank_callback, g.vblank_callback_changes,
                g.have_d3d_vblank ? uint64_t(g.d3d_vblank_last - g.d3d_vblank_first) : 0, frames, t.mmio_reads,
                t.mmio_writes);
    REXLOG_INFO("pdz_census: total D3D device {:08X}: fence pointer (+10384) {:08X}, scratch block (+10388) {:08X}",
                g.device, g.fence_ptr, g.scratch_block);
    // Rows by kind, busiest first; at most kRowsPrinted per kind, the rest summed in one line.
    struct RowRef
    {
      uint32_t kind;
      uint64_t count;
      uint32_t index;
    };
    std::vector<RowRef> rows;
    for (uint32_t i = 0; i < gc::kRowCount; ++i)
    {
      const uint64_t key = c.rows[i].key.load(std::memory_order_acquire);
      if (key)
      {
        rows.push_back({uint32_t(key >> 56), v(c.rows[i].count), i});
      }
    }
    std::sort(rows.begin(), rows.end(), [](const RowRef &a, const RowRef &b) {
      return a.kind != b.kind ? a.kind < b.kind : a.count > b.count;
    });
    constexpr uint32_t kRowsPrinted = 32;
    for (size_t r = 0, printed = 0; r < rows.size(); ++r)
    {
      printed = (r == 0 || rows[r].kind != rows[r - 1].kind) ? 0 : printed + 1;
      if (printed == kRowsPrinted)
      {
        uint64_t more_rows = 0, more_events = 0;
        for (size_t m = r; m < rows.size() && rows[m].kind == rows[r].kind; ++m)
        {
          ++more_rows;
          more_events += rows[m].count;
        }
        REXLOG_INFO("pdz_census: total (row kind {}: {} more keys, {} events, not listed)", rows[r].kind, more_rows,
                    more_events);
      }
      if (printed >= kRowsPrinted)
      {
        continue;
      }
      const gc::Row &row = c.rows[rows[r].index];
      const uint64_t key = row.key.load(std::memory_order_acquire);
      const gc::RowKind kind = gc::RowKind(key >> 56);
      const uint32_t a = uint32_t(key), b = uint32_t(key >> 32) & 0xFFFFFF;
      const uint64_t n = v(row.count), aux = v(row.aux), sum = v(row.sum), mx = v(row.max);
      const uint32_t l0 = row.last0.load(std::memory_order_relaxed), l1 = row.last1.load(std::memory_order_relaxed);
      const bool other = a == gc::kOtherA && b == gc::kOtherB;
      const double per = PerFrame(n, frames);
      static constexpr const char *kFunction[8] = {"never", "<", "<=", "==", "!=", ">=", ">", "always"};
      std::string text;
      switch (kind)
      {
      case gc::RowKind::kWaitRegMem:
        text = fmt::format("WAIT_REG_MEM {} {:08X}{} {} ref {:08X} mask {:08X}: {} ({:.2f}/frame), waited {} "
                           "({:.1f} ms, longest {} us)",
                           (b & 0x10) ? "memory" : "register", a,
                           (b & 0x10) ? AnnotateLocked(a) : fmt::format(" {}", RegisterName(a)), kFunction[b & 7], l0,
                           l1, n, per, aux, double(sum) / 1000.0, mx);
        break;
      case gc::RowKind::kEventWriteShd:
        text = fmt::format("EVENT_WRITE_SHD {:08X}{} event {} {}{}: {} ({:.2f}/frame), last value {:08X}", a,
                           AnnotateLocked(a), b & 0x3F, EventName(b & 0x3F),
                           (b & 0x80) ? " (writes the vblank counter)" : "", n, per, l0);
        break;
      case gc::RowKind::kEventWriteExt:
        text = fmt::format("EVENT_WRITE_EXT {:08X} event {} {}: {} ({:.2f}/frame)", a, b & 0x3F, EventName(b & 0x3F),
                           n, per);
        break;
      case gc::RowKind::kEventWrite:
        text = fmt::format("EVENT_WRITE event {} {}: {} ({:.2f}/frame), with an address {}", b & 0x3F,
                           EventName(b & 0x3F), n, per, aux);
        break;
      case gc::RowKind::kEventWriteZpd:
        text = fmt::format("EVENT_WRITE_ZPD report {:08X} event {}: {}", a, b & 0x3F, n);
        break;
      case gc::RowKind::kMemWrite:
        text = fmt::format("MEM_WRITE {:08X}{}: {} ({:.2f}/frame), {} dwords, first dword last {:08X}", a,
                           AnnotateLocked(a), n, per, sum, l0);
        break;
      case gc::RowKind::kCondWrite:
        text = fmt::format("COND_WRITE {} {:08X}{} poll {} {:08X} {} ref {:08X}: {} ({:.2f}/frame), written {}",
                           (b & 0x100) ? "memory" : "register", a,
                           (b & 0x100) ? AnnotateLocked(a) : fmt::format(" {}", RegisterName(a)),
                           (b & 0x10) ? "memory" : "register", l0, kFunction[b & 7], l1, n, per, aux);
        break;
      case gc::RowKind::kRegToMem:
        text = fmt::format("REG_TO_MEM {} -> {:08X}{}: {}, last {:08X}", RegisterName(b), a, AnnotateLocked(a), n, l0);
        break;
      case gc::RowKind::kRegRmw:
        text = fmt::format("REG_RMW {}: {} ({:.2f}/frame)", RegisterName(a), n, per);
        break;
      case gc::RowKind::kScratchWrite:
        text = fmt::format("scratch write-back SCRATCH_REG{} -> {:08X}{}: {} ({:.2f}/frame), last {:08X}", b, a,
                           AnnotateLocked(a), n, per, l0);
        break;
      case gc::RowKind::kRptrWrite:
        text = fmt::format("read-pointer write-back -> {:08X}: {} ({:.2f}/frame), last index {}", a, n, per, l0);
        break;
      case gc::RowKind::kSwap:
        text = fmt::format("XE_SWAP front buffer {:08X} format {} {}x{}: {}", a, b, l0, l1, n);
        break;
      case gc::RowKind::kResolveReadback:
        text = fmt::format("resolve readback {:08X} ({}): {}, {} bytes", a,
                           b == 0 ? "title range" : (b == 1 ? "readback_resolve mode" : "pass dump"), n, sum);
        break;
      case gc::RowKind::kMmioRead:
        text = fmt::format("MMIO read {:04X} {}: {} ({:.2f}/frame)", a, RegisterName(a), n, per);
        break;
      case gc::RowKind::kMmioWrite:
        text = fmt::format("MMIO write {:04X} {}: {} ({:.2f}/frame), last {:08X}", a, RegisterName(a), n, per, l0);
        break;
      case gc::RowKind::kInterrupt:
        text = fmt::format("interrupt source {} ({}) on CPU {}: {} ({:.2f}/frame), {:.1f} ms in the title's handler "
                           "(longest {} us)",
                           b, b ? "command processor" : "vblank", a, n, per, double(sum) / 1000.0, mx);
        break;
      case gc::RowKind::kInvalidateState:
        text = fmt::format("INVALIDATE_STATE mask {:08X}: {} ({:.2f}/frame)", a, n, per);
        break;
      case gc::RowKind::kImLoadImmediate:
        text = fmt::format("IM_LOAD_IMMEDIATE {} shader, {} dwords: {} ({:.2f}/frame)", b ? "pixel" : "vertex", a, n,
                           per);
        break;
      case gc::RowKind::kResolve:
        text = fmt::format("resolve: format {} source {} {}{}: {} ({:.2f}/frame), last destination {:08X}", a & 0x3F,
                           (a >> 8) & 7, ((a >> 8) & 7) >= 4 ? "depth" : "colour",
                           std::string((a & 0x1000) ? " +colour clear" : "") + ((a & 0x2000) ? " +depth clear" : ""),
                           n, per, l0);
        break;
      default:
        text = fmt::format("row kind {} a {:08X} b {:06X}: {}", uint32_t(kind), a, b, n);
        break;
      }
      REXLOG_INFO("pdz_census: total {}{}", other ? "(other keys of this kind) " : "", text);
    }

    // ---- (6) the title side ----------------------------------------------------------------------------------
    std::string hooked;
    for (uint32_t h = 0; h < uint32_t(Hook::kCount); ++h)
    {
      if (g.hooked[h])
      {
        hooked += fmt::format(" {} {}", pdz::shadow::HookName(Hook(h)), g.hooked[h]);
      }
    }
    REXLOG_INFO("pdz_census: total hooked calls (shadow recorder{}):{}",
                pdz::shadow::Enabled() ? "" : ", OFF: no counts", hooked.empty() ? std::string(" none") : hooked);
    REXLOG_INFO("pdz_census: total CP non-copy draws {} = hooked draw entries {} (DrawIndexedVertices {} + "
                "DrawVertices {} + BeginVertices {} (DrawVerticesUP included) + DrawIndexedVerticesUP {}) + ClearF {} "
                "+ 24 x SetShaderGPRAllocation {} + unhooked draw entries {} + residual {:+}",
                cp_draws,
                g.hooked[uint32_t(Hook::kDrawIndexedVertices)] + g.hooked[uint32_t(Hook::kDrawVertices)] +
                    g.hooked[uint32_t(Hook::kBeginVertices)] + g.hooked[uint32_t(Hook::kDrawIndexedVerticesUP)],
                g.hooked[uint32_t(Hook::kDrawIndexedVertices)], g.hooked[uint32_t(Hook::kDrawVertices)],
                g.hooked[uint32_t(Hook::kBeginVertices)], g.hooked[uint32_t(Hook::kDrawIndexedVerticesUP)],
                g.site_count[uint32_t(Site::kClearF)], g.site_count[uint32_t(Site::kGprAllocation)],
                g.site_count[uint32_t(Site::kDrawIndexedVariant)] + g.site_count[uint32_t(Site::kDrawEntry3C8)],
                int64_t(cp_draws) - int64_t(expected));
    REXLOG_INFO("pdz_census: total D3D state flushes {}: paired with a hooked draw {}, unpaired {}, recorder off {}",
                g.flushes, g.flushes_paired, unpaired, g.site_count[uint32_t(Site::kFlushNoRecorder)]);
    std::vector<const Entry *> entries;
    for (size_t i = 0; i < g.entry_count; ++i)
    {
      entries.push_back(&g.entries[i]);
    }
    std::sort(entries.begin(), entries.end(), [](const Entry *a, const Entry *b) {
      return a->site != b->site ? a->site < b->site : a->count > b->count;
    });
    for (const Entry *e : entries)
    {
      const uint32_t s = uint32_t(e->site);
      std::string extra;
      if (kSiteX[s])
      {
        extra += fmt::format(" {} {:08X}", kSiteX[s], e->x);
      }
      if (e->site == Site::kInsertCallback)
      {
        extra += fmt::format(" type {}", e->y);
      }
      if (e->sum)
      {
        extra += fmt::format(" (count argument sum {})", e->sum);
      }
      if (e->us)
      {
        extra += fmt::format(", blocked {:.1f} ms (longest {} us)", double(e->us) / 1000.0, e->max_us);
      }
      REXLOG_INFO("pdz_census: total title {} lr {}: {} ({:.3f} per frame){}", kSiteName[s], DescribeLr(e->lr),
                  e->count, PerFrame(e->count, frames), extra);
    }
    REXLOG_INFO("pdz_census: total settings:{}", Settings());
  }
} // namespace pdz::census

// -----------------------------------------------------------------------------
// Counter hooks: forward unchanged; count only while the census is on.
// -----------------------------------------------------------------------------

// sub_826EA688(device, prim r4, ?, start index r6, index count r7): a DrawIndexedVertices variant that sets
// VGT_INDX_OFFSET to 0; callers 8270FCF0 (sub_8270FCB0, called by pointer) and 82710548 (sub_827104C8).
REX_HOOK_RAW(sub_826EA688)
{
  // cpusplit: guest D3D work outside the shadow files' hooked entries (Swap thread, exclusive).
  const pdz::timing::ExclusiveScope guest_d3d(pdz::timing::Phase::kGuestD3D);
  const pdz::gtsample::Mark sampler_mark(pdz::gtsample::Where::kD3dHook);
  if (pdz::census::Enabled())
  {
    pdz::census::Note(pdz::census::Site::kDrawIndexedVariant, uint32_t(ctx.lr), ctx.r4.u32, 0, ctx.r7.u32);
  }
  __imp__sub_826EA688(ctx, base);
}

// sub_826EA3C8(device, prim r4, base vertex r5 -> VGT_INDX_OFFSET, r6): another draw entry with a flush; no
// direct callers.
REX_HOOK_RAW(sub_826EA3C8)
{
  // cpusplit: guest D3D work outside the shadow files' hooked entries (Swap thread, exclusive).
  const pdz::timing::ExclusiveScope guest_d3d(pdz::timing::Phase::kGuestD3D);
  const pdz::gtsample::Mark sampler_mark(pdz::gtsample::Where::kD3dHook);
  if (pdz::census::Enabled())
  {
    pdz::census::Note(pdz::census::Site::kDrawEntry3C8, uint32_t(ctx.lr), ctx.r4.u32, 0, ctx.r6.u32);
  }
  __imp__sub_826EA3C8(ctx, base);
}

// sub_82700A68(device, r4, r5, r6): flush, then a packet header built from r4 and a copy of r6 bytes from r5 into
// the ring; no direct callers.
REX_HOOK_RAW(sub_82700A68)
{
  // cpusplit: guest D3D work outside the shadow files' hooked entries (Swap thread, exclusive).
  const pdz::timing::ExclusiveScope guest_d3d(pdz::timing::Phase::kGuestD3D);
  const pdz::gtsample::Mark sampler_mark(pdz::gtsample::Where::kD3dHook);
  if (pdz::census::Enabled())
  {
    pdz::census::Note(pdz::census::Site::kPacketCopy, uint32_t(ctx.lr), ctx.r4.u32, 0, ctx.r6.u32);
  }
  __imp__sub_82700A68(ctx, base);
}

// BeginIndexedVertices: its only caller is DrawIndexedVerticesUP (826E9E90), hooked since 2986448.
REX_HOOK_RAW(sub_826E9AD8)
{
  // cpusplit: guest D3D work outside the shadow files' hooked entries (Swap thread, exclusive).
  const pdz::timing::ExclusiveScope guest_d3d(pdz::timing::Phase::kGuestD3D);
  const pdz::gtsample::Mark sampler_mark(pdz::gtsample::Where::kD3dHook);
  if (pdz::census::Enabled())
  {
    pdz::census::Note(pdz::census::Site::kBeginIndexed, uint32_t(ctx.lr));
  }
  __imp__sub_826E9AD8(ctx, base);
}

// ClearF(device, flags r4, rect r5, colour pointer r6, Z f1, stencil r8).
REX_HOOK_RAW(sub_826E0AF8)
{
  // cpusplit: guest D3D work outside the shadow files' hooked entries (Swap thread, exclusive).
  const pdz::timing::ExclusiveScope guest_d3d(pdz::timing::Phase::kGuestD3D);
  const pdz::gtsample::Mark sampler_mark(pdz::gtsample::Where::kD3dHook);
  if (pdz::census::Enabled())
  {
    pdz::census::Note(pdz::census::Site::kClearF, uint32_t(ctx.lr), ctx.r4.u32);
  }
  __imp__sub_826E0AF8(ctx, base);
}

// SetShaderGPRAllocation(device, r4, VS GPRs r5, PS GPRs r6): 24 one-vertex POINTLIST draws through sub_826E0270.
REX_HOOK_RAW(sub_826E4680)
{
  // cpusplit: guest D3D work outside the shadow files' hooked entries (Swap thread, exclusive).
  const pdz::timing::ExclusiveScope guest_d3d(pdz::timing::Phase::kGuestD3D);
  const pdz::gtsample::Mark sampler_mark(pdz::gtsample::Where::kD3dHook);
  if (pdz::census::Enabled())
  {
    pdz::census::Note(pdz::census::Site::kGprAllocation, uint32_t(ctx.lr),
                      ((ctx.r5.u32 & 0xFF) << 8) | (ctx.r6.u32 & 0xFF));
  }
  __imp__sub_826E4680(ctx, base);
}

// sub_826DF568(device): EVENT_WRITE with initiator 15 (TILE_FLUSH; the "FlushHiZStencil" of STAGE2_PLAN 1.4).
REX_HOOK_RAW(sub_826DF568)
{
  // cpusplit: guest D3D work outside the shadow files' hooked entries (Swap thread, exclusive).
  const pdz::timing::ExclusiveScope guest_d3d(pdz::timing::Phase::kGuestD3D);
  const pdz::gtsample::Mark sampler_mark(pdz::gtsample::Where::kD3dHook);
  if (pdz::census::Enabled())
  {
    pdz::census::Note(pdz::census::Site::kHiZFlush, uint32_t(ctx.lr));
  }
  __imp__sub_826DF568(ctx, base);
}

// Texture lock (texture r3, level r4, r5, flags r6, outputs r7..r10): CPU access to texture memory; the flags tell
// a read (D3DLOCK_READONLY 0x10) from a write.
REX_HOOK_RAW(sub_826EC238)
{
  // cpusplit: guest D3D work outside the shadow files' hooked entries (Swap thread, exclusive).
  const pdz::timing::ExclusiveScope guest_d3d(pdz::timing::Phase::kGuestD3D);
  const pdz::gtsample::Mark sampler_mark(pdz::gtsample::Where::kD3dHook);
  if (pdz::census::Enabled())
  {
    pdz::census::Note(pdz::census::Site::kLock, uint32_t(ctx.lr), ctx.r6.u32);
  }
  const uint32_t texture = ctx.r3.u32;
  const uint32_t flags = ctx.r6.u32;
  __imp__sub_826EC238(ctx, base);
  // The CPU writes the texture next: the native texture cache re-checks it at its next use
  // (native_textures.h OnTextureLock, pdz_native_texture_lock_check).
  pdz::native::OnTextureLock(base, texture, flags);
}

// BlockUntilNotBusy(resource r3): the CPU waits until the GPU is done with a resource.
REX_HOOK_RAW(sub_826DE880)
{
  // cpusplit (pdz_frame_timing_phases / pdz_game_thread_profile): a game-thread wait, exclusive.
  const pdz::timing::ExclusiveScope wait_time(pdz::timing::Phase::kWaitNotBusy);
  const pdz::gtsample::Mark sampler_mark(pdz::gtsample::Where::kNotBusy);
  if (!pdz::census::Enabled())
  {
    __imp__sub_826DE880(ctx, base);
    return;
  }
  const uint32_t lr = uint32_t(ctx.lr);
  const int64_t start = pdz::census::Now();
  __imp__sub_826DE880(ctx, base);
  const int64_t end = pdz::census::Now();
  std::lock_guard<std::mutex> lock(pdz::census::g.mutex);
  pdz::census::NoteLocked(pdz::census::Site::kBlockUntilNotBusy, lr, 0, 0, 0, pdz::census::TicksToUs(end - start));
}

// Block until a GPU fence has passed (device r3, fence pointer r4): the CPU polls the fence value the GPU writes
// (EVENT_WRITE_SHD) through sub_826F36B8.
REX_HOOK_RAW(sub_826DE258)
{
  // cpusplit (pdz_frame_timing_phases / pdz_game_thread_profile): a game-thread wait, exclusive.
  const pdz::timing::ExclusiveScope wait_time(pdz::timing::Phase::kWaitGuestFence);
  const pdz::gtsample::Mark sampler_mark(pdz::gtsample::Where::kGuestFence);
  // pdz_native_watchdog_ms: where a game thread waits when the GPU thread stops consuming the ring (run 196: KickOff's
  // ring-segment wait behind a capture swap that never finished). Nothing but a cvar load while the watchdog is off.
  const pdz::native::watchdog::Wait watchdog_wait("guest D3D fence block (sub_826DE258): waiting for the GPU thread",
                                                  uint32_t(ctx.lr));
  const bool census = pdz::census::Enabled();
  const bool sync = pdz::gpusync::Enabled(); // gpu_sync.h: pdz_sync_log / the null GPU backend
  if (!census && !sync)
  {
    __imp__sub_826DE258(ctx, base);
    return;
  }
  const uint32_t lr = uint32_t(ctx.lr);
  if (sync)
  {
    pdz::gpusync::OnFenceBlockEnter(base, ctx.r3.u32, ctx.r4.u32, lr); // the awaited fence: [r4]
  }
  const int64_t start = pdz::census::Now();
  __imp__sub_826DE258(ctx, base);
  const int64_t end = pdz::census::Now();
  const uint64_t us = pdz::census::TicksToUs(end - start);
  if (sync)
  {
    pdz::gpusync::OnFenceBlockLeave(int64_t(us));
  }
  if (census)
  {
    std::lock_guard<std::mutex> lock(pdz::census::g.mutex);
    pdz::census::NoteLocked(pdz::census::Site::kFenceBlock, lr, 0, 0, 0, us);
  }
}

// BlockUntilIdle(device r3): WAIT_UNTIL + fence + block.
REX_HOOK_RAW(sub_826DE6A0)
{
  // cpusplit (pdz_frame_timing_phases / pdz_game_thread_profile): a game-thread wait, exclusive.
  const pdz::timing::ExclusiveScope wait_time(pdz::timing::Phase::kWaitBlockIdle);
  const pdz::gtsample::Mark sampler_mark(pdz::gtsample::Where::kBlockIdle);
  const bool census = pdz::census::Enabled();
  const bool sync = pdz::gpusync::Enabled(); // gpu_sync.h: pdz_sync_log / the null GPU backend
  if (!census && !sync)
  {
    __imp__sub_826DE6A0(ctx, base);
    return;
  }
  const uint32_t lr = uint32_t(ctx.lr);
  if (sync)
  {
    pdz::gpusync::OnBlockUntilIdleEnter(base, ctx.r3.u32, lr);
  }
  const int64_t start = pdz::census::Now();
  __imp__sub_826DE6A0(ctx, base);
  const int64_t end = pdz::census::Now();
  const uint64_t us = pdz::census::TicksToUs(end - start);
  if (sync)
  {
    pdz::gpusync::OnBlockUntilIdleLeave(lr, int64_t(us));
  }
  if (census)
  {
    std::lock_guard<std::mutex> lock(pdz::census::g.mutex);
    pdz::census::NoteLocked(pdz::census::Site::kBlockUntilIdle, lr, 0, 0, 0, us);
  }
}
