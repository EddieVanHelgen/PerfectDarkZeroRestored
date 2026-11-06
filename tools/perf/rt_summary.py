"""Swap rate and record-thread load of one run's log over a time window (docs/INTERCEPTION_SPEEDUP.md).

    python -I tools/perf/rt_summary.py [--from 100] [--to 215] <log> [...]

--from / --to: seconds after the log's first line (the scripted bench's stick pans are at 100-215 s).
From the null GPU's 10 s lines: guest swaps per second against guest vblanks per second, and the missed frames
(vblanks - swaps). From the 'pdz_timing: record thread (window)' lines: the record worker's busy ms per frame
(avg of the windows' avg, the worst p99 and max) and commands per frame.
"""
import argparse
import datetime
import re

TS = re.compile(r"^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d+)\]")
NULL = re.compile(r"null_gpu: t=\d+s \| vblanks (\d+) \(([\d.]+)/s\) \| XE_SWAP (\d+) \(([\d.]+)/s\)")
RT = re.compile(r"record thread \(window\) swaps \d+-\d+ \| worker busy ms per frame avg ([\d.]+) p99 ([\d.]+) max "
                r"([\d.]+) \((\d+)% of the frame\), commands (\d+)/frame")


def ts(line):
    m = TS.match(line)
    return datetime.datetime.strptime(m.group(1), "%Y-%m-%d %H:%M:%S.%f") if m else None


def summarise(path, t_from, t_to):
    start = None
    nulls, rts = [], []
    with open(path, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            t = ts(line)
            if t is None:
                continue
            if start is None:
                start = t
            s = (t - start).total_seconds()
            if s < t_from or s > t_to:
                continue
            m = NULL.search(line)
            if m:
                nulls.append((int(m.group(1)), float(m.group(2)), int(m.group(3)), float(m.group(4))))
                continue
            m = RT.search(line)
            if m:
                rts.append(tuple(float(g) for g in m.groups()))
    out = {}
    if len(nulls) >= 2:
        dv = nulls[-1][0] - nulls[0][0]
        ds = nulls[-1][2] - nulls[0][2]
        span = dv / max(1e-9, sum(n[1] for n in nulls[1:]) / (len(nulls) - 1))
        out["vblanks_s"] = dv / span
        out["swaps_s"] = ds / span
        out["missed_per_s"] = (dv - ds) / span
        out["worst_10s_swaps_s"] = min(n[3] for n in nulls[1:])
    if rts:
        out["rt_busy_avg"] = sum(r[0] for r in rts) / len(rts)
        out["rt_busy_p99_worst"] = max(r[1] for r in rts)
        out["rt_busy_max"] = max(r[2] for r in rts)
        out["rt_commands"] = sum(r[4] for r in rts) / len(rts)
        out["rt_commands_max"] = max(r[4] for r in rts)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--from", dest="t_from", type=float, default=100.0)
    ap.add_argument("--to", dest="t_to", type=float, default=215.0)
    ap.add_argument("logs", nargs="+")
    a = ap.parse_args()
    print("| log | swaps / s | vblanks / s | missed / s | worst 10 s swaps / s | record busy ms avg | p99 (worst) | max | "
          "commands / frame (max) |")
    print("|---|---|---|---|---|---|---|---|---|")
    for p in a.logs:
        o = summarise(p, a.t_from, a.t_to)
        f = lambda k, fmt="%.2f": fmt % o[k] if k in o else "-"
        print("| %s | %s | %s | %s | %s | %s | %s | %s | %s (%s) |" % (
            p.replace("\\", "/").split("/")[-1], f("swaps_s"), f("vblanks_s"), f("missed_per_s"), f("worst_10s_swaps_s", "%.1f"),
            f("rt_busy_avg"), f("rt_busy_p99_worst"), f("rt_busy_max"), f("rt_commands", "%.0f"),
            f("rt_commands_max", "%.0f")))


if __name__ == "__main__":
    main()
