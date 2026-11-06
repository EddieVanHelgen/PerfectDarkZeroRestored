// Single-thread CPU calibration for the slow-CPU modes (tools/perf/cpu_modes.ps1, docs/PERF_FOR_EVERYONE.md): an
// emulator-like mix of byte-swapped loads / stores through a "guest memory", a 16-way switch dispatch, float maths
// and pointer chasing. "compute": 128 KB + 64 KB (cache resident: core speed); "memory": 1 MB + 8 MB (cache and memory
// latency as well). Usage: stbench.exe <logical processor or -1> [compute|memory]; prints the best of 7 runs in ms.
// Built by tools/perf/cpu_calibrate.ps1 with the Visual Studio clang.
#include <windows.h>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>
static uint32_t bs(uint32_t v) { return __builtin_bswap32(v); }
int main(int argc, char **argv) {
  int lp = argc > 1 ? atoi(argv[1]) : -1;
  const bool memory = argc > 2 && argv[2][0] == 'm';
  const uint32_t mask = memory ? 0x3FFFF : 0x7FFF;
  if (lp >= 0) SetThreadAffinityMask(GetCurrentThread(), 1ull << lp);
  std::vector<uint32_t> mem(memory ? 1 << 18 : 1 << 15);
  std::vector<uint32_t> chase(memory ? 1 << 21 : 1 << 14);
  for (size_t i = 0; i < mem.size(); ++i) mem[i] = uint32_t(i * 2654435761u);
  uint32_t x = 1; for (size_t i = 0; i < chase.size(); ++i) { x = x * 1103515245u + 12345u; chase[i] = x & (chase.size() - 1); }
  double best = 1e9; volatile uint64_t sink = 0;
  for (int run = 0; run < 7; ++run) {
    auto t0 = std::chrono::steady_clock::now();
    uint64_t acc = 0; uint32_t pc = 0, r3 = 7, r4 = 9; double f1 = 1.0, f2 = 0.5; uint32_t c = 0;
    for (int i = 0; i < 60000000; ++i) {
      uint32_t op = bs(mem[pc & mask]) & 63;
      switch (op & 15) {
        case 0: r3 += r4; break; case 1: r3 ^= r4 << 3; break; case 2: r4 = bs(mem[(r3 >> 4) & mask]); break;
        case 3: mem[(r4 >> 6) & mask] = bs(r3); break; case 4: f1 = f1 * 1.0000001 + f2; break; case 5: f2 = f2 * 0.9999 - f1 * 1e-9; break;
        case 6: r3 = (r3 >> 7) | (r3 << 25); break; case 7: if (r3 & 1) r4 += 3; else r4 -= 5; break;
        case 8: c = chase[c]; break; case 9: r3 = r3 * 2654435761u; break; case 10: r4 = r4 + (r3 > r4); break;
        case 11: acc += r3; break; case 12: f1 = f1 > 1e6 ? 1.0 : f1 + 0.25; break; case 13: r3 -= r4 >> 2; break;
        case 14: mem[(r3 >> 8) & mask] ^= r4; break; default: acc ^= r4; break;
      }
      pc = pc * 5 + 1 + op;
    }
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    sink += acc + r3 + r4 + c + uint64_t(f1 + f2);
    if (ms < best) best = ms;
  }
  std::printf("%.1f\n", best);
}
