// pdz_native_memcpy (2 Nov 2026; docs/PERF_FOR_EVERYONE.md): the XDK C runtime's two copy routines run as a host
// memcpy with exactly the guest-visible results of the recompiled routines.
//
// - sub_82AA8CC0 (r3 dest, r4 src, r5 bytes; returns r3): the plain memcpy. RARE_CODE_PROFILE.md called it memset;
//   it copies (loads through r4, stores through r3). Guest-visible effects: dest[0, n) = src[0, n), forward, in 1 / 4 /
//   8-byte and 128-byte unrolled steps, and `std r3,-8(r1)` (the red zone). No cache-line zeroing, no vector code.
// - sub_82AD97C0 (r3 dest, r4 src, r5 bytes, r6 mode; returns r3): the streaming copy (XMemCpy-style). Effects:
//   dest[0, n) = src[0, n); the frame (stwu r1,-176: the back chain r1 at r1-176; __savegprlr_26: r26-r31 as 64-bit
//   at r1-56 .. r1-16 and LR at r1-8); for n >= 16 the lvsl permute vector stored at (r1-80) & ~15 (bytes sh .. sh+15,
//   sh = the source's alignment after the destination's 16-byte head); dcbzl of whole 128-byte lines that the copy
//   then overwrites completely (the line after the head at 82AD9988, the next line at 82AD9A10 / 82ADD9C8 / 82ADDA88
//   when 256 or more bytes remain), so no zeroing survives. Its tail byte loop counts down by the global at
//   0x82D1137C (1): any other value runs Rare's code.
// dcbt / dcbtst / dcbf generate no code. Volatile registers (r0, r4-r12, ctr, cr, v0-v13) are left as they are; by
// the PowerPC ABI no caller reads them after a call (the callers are compiled C / C++: Rare's render code, D3D).
//
// The host copy is used only when it can't differ: the ranges don't overlap (also through the physical views
// 0xA0000000 / 0xC0000000 / 0xE0000000, which alias the same memory), don't wrap and don't cross 0xE0000000 (the
// host offset of the 0xE0000000 view). Everything else runs the recompiled routine. A host copy into a write-watched
// page faults and is handled exactly like the guest's own stores (the watch handler doesn't decode the instruction).
//
// pdz_native_memcpy = 2 (verify): the recompiled routine runs, then its result (the destination's 128-byte lines,
// the stack area it writes, r1, r3) is compared with what the native path would have written; the first mismatches
// are logged in full, and a summary every 10 s.
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>

REXCVAR_DEFINE_INT32(pdz_native_memcpy, 1, "Performance",
                     "The guest C runtime's memcpy (sub_82AA8CC0) and streaming copy (sub_82AD97C0) as a host "
                     "copy with identical results, on by default (docs/PERF_FOR_EVERYONE.md): 0 = the recompiled routines, 1 = "
                     "native, 2 = verify (run the recompiled routine, compare with the native result, log "
                     "mismatches and a 10 s summary)")
    .range(0, 2)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REX_EXTERN(__imp__sub_82AA8CC0);
REX_EXTERN(__imp__sub_82AD97C0);

namespace
{
  constexpr uint32_t kByteStepGlobal = 0x82D1137C; // lis r11,-32047; lwz r11,4988(r11) at 82AD985C

  uint8_t *Host(uint8_t *base, uint32_t a)
  {
    return base + a + (a >= 0xE0000000u ? 0x1000u : 0u);
  }

  uint32_t Load32(uint8_t *base, uint32_t a)
  {
    uint32_t v;
    std::memcpy(&v, Host(base, a), 4);
    return __builtin_bswap32(v);
  }

  // A range the host copy can address as one block: no wrap past 4 GB, not across 0xE0000000.
  bool RangeOk(uint32_t a, uint32_t n)
  {
    const uint64_t end = uint64_t(a) + n;
    return end <= 0x100000000ull && !(a < 0xE0000000u && end > 0xE0000000u);
  }

  // Disjoint, also as physical memory (the 0xA0000000 / 0xC0000000 / 0xE0000000 views alias it).
  uint64_t Phys(uint32_t a)
  {
    return a >= 0xA0000000u ? (uint64_t(a & 0x1FFFFFFFu) + (a >= 0xE0000000u ? 0x1000u : 0u)) : uint64_t(a);
  }

  bool Disjoint(uint32_t d, uint32_t s, uint32_t n)
  {
    if (!n)
    {
      return true;
    }
    if (uint64_t(d) + n <= s || uint64_t(s) + n <= d)
    {
      if ((d >= 0xA0000000u) == (s >= 0xA0000000u) && d >= 0xA0000000u)
      {
        const uint64_t pd = Phys(d), ps = Phys(s);
        return pd + n <= ps || ps + n <= pd;
      }
      return true;
    }
    return false;
  }

  // Also clear of the stack area the routine writes ([r1 - frame, r1)): there its stack stores and the copy would
  // interleave in an order the native path doesn't reproduce.
  bool Copyable(uint32_t d, uint32_t s, uint32_t n, uint32_t r1, uint32_t frame)
  {
    return RangeOk(d, n) && RangeOk(s, n) && Disjoint(d, s, n) && Disjoint(d, r1 - frame, frame) &&
           Disjoint(s, r1 - frame, frame);
  }

  // The stack writes, as (address, bytes) in guest order.
  struct StackWrite
  {
    uint32_t address;
    uint8_t size;
    uint8_t bytes[16];
  };

  void Be(StackWrite &w, uint32_t a, uint64_t v, uint8_t size)
  {
    w.address = a;
    w.size = size;
    for (uint8_t i = 0; i < size; ++i)
    {
      w.bytes[i] = uint8_t(v >> (8 * (size - 1 - i)));
    }
  }

  // sub_82AA8CC0: std r3,-8(r1).
  int MemcpyStack(const PPCContext &ctx, StackWrite *out)
  {
    Be(out[0], ctx.r1.u32 - 8, ctx.r3.u64, 8);
    return 1;
  }

  // sub_82AD97C0: the frame and, for n >= 16, the permute vector.
  int StreamStack(const PPCContext &ctx, StackWrite *out)
  {
    const uint32_t r1 = ctx.r1.u32;
    int k = 0;
    Be(out[k++], r1 - 176, r1, 4); // stwu r1,-176(r1)
    const uint64_t saved[6] = {ctx.r26.u64, ctx.r27.u64, ctx.r28.u64, ctx.r29.u64, ctx.r30.u64, ctx.r31.u64};
    for (int i = 0; i < 6; ++i)
    {
      Be(out[k++], r1 - 56 + 8 * i, saved[i], 8); // __savegprlr_26
    }
    Be(out[k++], r1 - 8, uint32_t(ctx.lr), 4);    // mflr r12; stw r12,-8(r1)
    const uint32_t n = ctx.r5.u32;
    if (n >= 16)
    {
      const uint32_t head = (0u - ctx.r3.u32) & 15u;
      const uint32_t sh = (ctx.r4.u32 + head) & 15u; // lvsl v0,r0,r31 at 82AD999C
      StackWrite &w = out[k++];
      w.address = (r1 - 176 + 96) & ~15u; // stvx v0,r0,r3 (r3 = r1 + 96 after stwu)
      w.size = 16;
      for (uint32_t i = 0; i < 16; ++i)
      {
        w.bytes[i] = uint8_t(sh + i);
      }
    }
    return k;
  }

  void ApplyStack(uint8_t *base, const StackWrite *w, int count)
  {
    for (int i = 0; i < count; ++i)
    {
      std::memcpy(Host(base, w[i].address), w[i].bytes, w[i].size);
    }
  }

  // pdz_native_memcpy = 2.
  struct VerifyStats
  {
    std::atomic<uint64_t> checked{0}, fallback{0}, mismatched{0}, bytes{0};
    std::atomic<int64_t> last_log_ms{0};
  };
  VerifyStats g_verify[2]; // 0 = memcpy, 1 = streaming copy
  std::atomic<int> g_mismatch_lines{0};
  const char *const kNames[2] = {"memcpy sub_82AA8CC0", "streaming copy sub_82AD97C0"};

  int64_t NowMs()
  {
    return int64_t(rex::chrono::Clock::QueryHostUptimeMillis());
  }

  void MaybeSummary()
  {
    const int64_t now = NowMs();
    int64_t last = g_verify[0].last_log_ms.load(std::memory_order_relaxed);
    if (now - last < 10000 || !g_verify[0].last_log_ms.compare_exchange_strong(last, now))
    {
      return;
    }
    for (int f = 0; f < 2; ++f)
    {
      REXLOG_INFO("pdz_native_memcpy verify: {}: {} calls checked ({} MB), {} mismatches, {} ran Rare's routine "
                  "(overlap / range / step) and were not compared",
                  kNames[f], g_verify[f].checked.load(), g_verify[f].bytes.load() >> 20, g_verify[f].mismatched.load(),
                  g_verify[f].fallback.load());
    }
  }

  // Runs the recompiled routine and compares its effects with the native ones.
  template <typename Original>
  void Verify(int which, PPCContext &ctx, uint8_t *base, Original original, bool native_ok,
              int (*stack_writes)(const PPCContext &, StackWrite *))
  {
    if (!native_ok)
    {
      g_verify[which].fallback.fetch_add(1, std::memory_order_relaxed);
      original(ctx, base);
      MaybeSummary();
      return;
    }
    const uint32_t d = ctx.r3.u32, s = ctx.r4.u32, n = ctx.r5.u32, r1 = ctx.r1.u32;
    const uint64_t r3 = ctx.r3.u64;
    StackWrite w[9];
    const int count = stack_writes(ctx, w);
    // The destination's whole 128-byte lines (dcbz granularity; a line never crosses a page) and the stack area.
    const uint32_t lo = d & ~127u;
    const uint32_t hi = uint32_t(std::min<uint64_t>((uint64_t(d) + n + 127) & ~uint64_t(127), 0x100000000ull - 128));
    const bool lines_ok = RangeOk(lo, hi - lo);
    const uint32_t stack_n = which == 0 ? 8 : 176, stack_lo = r1 - stack_n; // the area each routine writes
    std::vector<uint8_t> before_lines(lines_ok ? hi - lo : 0), before_stack(stack_n);
    if (lines_ok)
    {
      std::memcpy(before_lines.data(), Host(base, lo), hi - lo);
    }
    std::memcpy(before_stack.data(), Host(base, stack_lo), stack_n);
    original(ctx, base);
    // Expected: the lines as before with dest = src; the stack as before plus the native stack writes.
    std::string why;
    if (ctx.r3.u64 != r3)
    {
      why += fmt::format(" r3 {:016X} expected {:016X};", ctx.r3.u64, r3);
    }
    if (ctx.r1.u32 != r1)
    {
      why += fmt::format(" r1 {:08X} expected {:08X};", ctx.r1.u32, r1);
    }
    if (lines_ok)
    {
      std::vector<uint8_t> expect = before_lines;
      for (int i = 0; i < count; ++i) // a stack write inside the destination's lines (the dest is just above r1)
      {
        for (uint32_t b = 0; b < w[i].size; ++b)
        {
          const uint32_t a = w[i].address + b;
          if (a >= lo && a < hi)
          {
            expect[a - lo] = w[i].bytes[b];
          }
        }
      }
      if (n)
      {
        std::memcpy(expect.data() + (d - lo), Host(base, s), n);
      }
      const uint8_t *now = Host(base, lo);
      // memcpy writes nothing outside its destination (no dcbz): there, compare the destination only (bytes beside
      // it in the same lines may be written by other threads meanwhile: 2 such in 46 M calls, 3 Nov). The streaming
      // copy's whole lines are compared (its dcbzl works on lines).
      const uint32_t from = which == 0 ? d - lo : 0, to = which == 0 ? d - lo + n : hi - lo;
      for (uint32_t i = from; i < to; ++i)
      {
        if (now[i] != expect[i])
        {
          why += fmt::format(" dest line byte at {:08X} = {:02X}, expected {:02X} (before {:02X});", lo + i, now[i],
                             expect[i], before_lines[i]);
          break;
        }
      }
    }
    std::vector<uint8_t> expect_stack = before_stack;
    for (int i = 0; i < count; ++i)
    {
      std::memcpy(expect_stack.data() + (w[i].address - stack_lo), w[i].bytes, w[i].size);
    }
    const uint8_t *stack_now = Host(base, stack_lo);
    for (uint32_t i = 0; i < stack_n; ++i)
    {
      if (stack_now[i] != expect_stack[i])
      {
        why += fmt::format(" stack byte at r1-{} = {:02X}, expected {:02X} (before {:02X});", stack_n - i,
                           stack_now[i], expect_stack[i], before_stack[i]);
        break;
      }
    }
    g_verify[which].checked.fetch_add(1, std::memory_order_relaxed);
    g_verify[which].bytes.fetch_add(n, std::memory_order_relaxed);
    if (!why.empty())
    {
      g_verify[which].mismatched.fetch_add(1, std::memory_order_relaxed);
      if (g_mismatch_lines.fetch_add(1, std::memory_order_relaxed) < 20)
      {
        REXLOG_WARN("pdz_native_memcpy verify MISMATCH: {} dest {:08X} src {:08X} bytes {} mode r6 {:08X} r1 {:08X} "
                    "lr {:08X}:{}",
                    kNames[which], d, s, n, ctx.r6.u32, r1, uint32_t(ctx.lr), why);
      }
    }
    MaybeSummary();
  }

  std::atomic<bool> g_logged{false};
  void LogOnce()
  {
    if (!g_logged.exchange(true, std::memory_order_relaxed))
    {
      REXLOG_INFO("pdz_native_memcpy: {} (sub_82AA8CC0 memcpy and sub_82AD97C0 streaming copy)",
                  REXCVAR_GET(pdz_native_memcpy) == 2 ? "verify: Rare's routines, checked against the native result"
                                                      : "native host copy, identical results");
    }
  }
} // namespace

REX_HOOK_RAW(sub_82AA8CC0)
{
  const int32_t mode = REXCVAR_GET(pdz_native_memcpy);
  if (mode <= 0)
  {
    __imp__sub_82AA8CC0(ctx, base);
    return;
  }
  LogOnce();
  const uint32_t d = ctx.r3.u32, s = ctx.r4.u32, n = ctx.r5.u32;
  const bool ok = Copyable(d, s, n, ctx.r1.u32, 8);
  if (mode == 2)
  {
    Verify(0, ctx, base, __imp__sub_82AA8CC0, ok, MemcpyStack);
    return;
  }
  if (!ok)
  {
    __imp__sub_82AA8CC0(ctx, base);
    return;
  }
  StackWrite w[1];
  ApplyStack(base, w, MemcpyStack(ctx, w));
  if (n)
  {
    std::memcpy(Host(base, d), Host(base, s), n);
  }
  // r3 (dest) and r1 are unchanged, as the routine leaves them.
}

REX_HOOK_RAW(sub_82AD97C0)
{
  const int32_t mode = REXCVAR_GET(pdz_native_memcpy);
  if (mode <= 0)
  {
    __imp__sub_82AD97C0(ctx, base);
    return;
  }
  LogOnce();
  const uint32_t d = ctx.r3.u32, s = ctx.r4.u32, n = ctx.r5.u32;
  const bool ok = Copyable(d, s, n, ctx.r1.u32, 176) && Load32(base, kByteStepGlobal) == 1;
  if (mode == 2)
  {
    Verify(1, ctx, base, __imp__sub_82AD97C0, ok, StreamStack);
    return;
  }
  if (!ok)
  {
    __imp__sub_82AD97C0(ctx, base);
    return;
  }
  StackWrite w[9];
  ApplyStack(base, w, StreamStack(ctx, w));
  if (n)
  {
    std::memcpy(Host(base, d), Host(base, s), n);
  }
  // r3 (dest), r1 and r26-r31 (saved and restored by the routine) are unchanged.
}
