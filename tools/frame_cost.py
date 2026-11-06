"""Summarize where frame time goes, from frame_pacing_log's 'K' / 'k' / 'T' rows.

usage: python tools/frame_cost.py [path to frame_pacing.csv] [--from SECONDS] [--to SECONDS]

Default path: out/build/win-amd64-release/logs/frame_pacing.csv. Rows are
"type,t_us,a,b,c,d,e,f" (see thirdparty/rexglue-sdk/include/rex/ui/frame_pacing.h):
  K  a = wall us since the previous guest swap, b = CP idle (ring empty),
     c = CP WAIT_REG_MEM, d = CP blocked on the host GPU, e = draws, f = copies
  k  a = host submissions, b = draws skipped (pipeline not ready), c = memexport draws
  T  a = frame, b = host GPU busy us, c = first start -> last end us, d = submissions
CP busy = wall - idle - WAIT_REG_MEM - GPU wait: the command processor's own
CPU time (PM4 processing, D3D12 recording, texture / pipeline work).
Prints a per-second table and an overall summary with medians and 90th
percentiles; compare two runs (one setting changed) side by side.
"""
import csv
import os
import statistics
import sys


def pct(values, p):
    if not values:
        return 0.0
    values = sorted(values)
    return values[min(len(values) - 1, int(len(values) * p))]


def main():
    args = sys.argv[1:]
    path = os.path.join('out', 'build', 'win-amd64-release', 'logs', 'frame_pacing.csv')
    t_from, t_to = 0.0, float('inf')
    i = 0
    while i < len(args):
        if args[i] == '--from':
            t_from = float(args[i + 1]); i += 2
        elif args[i] == '--to':
            t_to = float(args[i + 1]); i += 2
        else:
            path = args[i]; i += 1
    frames = []    # (second, wall, idle, waitreg, gpuwait, draws, copies)
    counts = []    # (second, submissions, skipped, memexport)
    gpu = []       # (second, busy, span)
    with open(path, newline='') as f:
        for row in csv.reader(f):
            if len(row) < 8 or row[0] not in ('K', 'k', 'T'):
                continue
            t = float(row[1]) / 1e6
            if not (t_from <= t <= t_to):
                continue
            v = [int(x) for x in row[2:8]]
            s = int(t)
            if row[0] == 'K':
                frames.append((s, *v))
            elif row[0] == 'k':
                counts.append((s, v[0], v[1], v[2]))
            else:
                gpu.append((s, v[1], v[2]))
    if not frames:
        print('no K rows: run with frame_pacing_log = true on a build that has them')
        return
    print(f'{path}: {len(frames)} guest frames, {len(gpu)} GPU frames')
    print('sec  fps   cp_busy  cp_idle  waitreg  gpu_wait  gpu_busy(med/p90)  draws copies skipped')
    seconds = sorted({f[0] for f in frames})
    for s in seconds:
        fr = [f for f in frames if f[0] == s]
        busy = [max(0, f[1] - f[2] - f[3] - f[4]) for f in fr]
        g = [x[1] for x in gpu if x[0] == s]
        sk = sum(c[2] for c in counts if c[0] == s)
        print(f'{s:4d} {len(fr):4d} {statistics.median(busy) / 1000:8.2f} '
              f'{statistics.median([f[2] for f in fr]) / 1000:8.2f} '
              f'{statistics.median([f[3] for f in fr]) / 1000:8.2f} '
              f'{statistics.median([f[4] for f in fr]) / 1000:9.2f} '
              f'{(statistics.median(g) if g else 0) / 1000:8.2f}/{pct(g, 0.9) / 1000:6.2f} '
              f'{statistics.median([f[5] for f in fr]):10.0f} {statistics.median([f[6] for f in fr]):6.0f} {sk:7d}')
    busy = [max(0, f[1] - f[2] - f[3] - f[4]) for f in frames]
    g = [x[1] for x in gpu]
    wall = [f[1] for f in frames]
    print()
    print('summary (ms, median / p90):')
    print(f'  frame wall      {statistics.median(wall) / 1000:6.2f} / {pct(wall, 0.9) / 1000:6.2f}')
    print(f'  CP busy         {statistics.median(busy) / 1000:6.2f} / {pct(busy, 0.9) / 1000:6.2f}')
    print(f'  CP waiting GPU  {statistics.median([f[4] for f in frames]) / 1000:6.2f} / {pct([f[4] for f in frames], 0.9) / 1000:6.2f}')
    if g:
        print(f'  host GPU busy   {statistics.median(g) / 1000:6.2f} / {pct(g, 0.9) / 1000:6.2f}')
    print(f'  draws / frame   {statistics.median([f[5] for f in frames]):.0f}, copies (resolves) {statistics.median([f[6] for f in frames]):.0f}')
    print(f'  skipped draws (shader not ready) total {sum(c[2] for c in counts)}, memexport draws total {sum(c[3] for c in counts)}')
    print('reading it: GPU busy near the frame time = host-GPU-bound; CP busy near it = emulator CPU-bound;')
    print('both well under it with low fps = the game thread (guest CPU) or a rate the game chose.')


if __name__ == '__main__':
    main()
