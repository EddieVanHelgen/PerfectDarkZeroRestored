"""One table row per bench run for docs/PERF_FOR_EVERYONE.md: frame avg / p99, swaps per second, the render
thread's split and the main thread's busy share.

    python -I tools/perf/everyone_table.py --map <tag>_PDZRestored.map <tag>_<label>.log [...]

Frame figures come from tools/perf/level_split.py (the level-only 600-frame windows of the pdz_timing lines);
swaps per second = 1000 / the mean frame. The thread split comes from tools/perf/ip_profile_stacks.py on the run's
<tag>_<label>.profile.txt (pdz_guest_ip_profile with threads=2): render thread Rare / interception / waits (spin =
the D3D fence / ring wait, os = OS waits), main thread busy = 100% minus its OS waits.
"""
import argparse
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def run(args):
    return subprocess.run([sys.executable, "-I"] + args, capture_output=True, text=True).stdout


def frame_stats(log):
    out = run([os.path.join(HERE, "level_split.py"), log])
    m = re.search(r"level windows (\d+)\s*\n\s*frame ([\d.]+) \| p99 ([\d.]+)(?: \| over12 ([\d.]+))?", out)
    if not m:
        return None
    return {"windows": int(m.group(1)), "frame": float(m.group(2)), "p99": float(m.group(3)),
            "over12": float(m.group(4)) if m.group(4) else float("nan")}


def threads(profile, mapfile):
    out = run([os.path.join(HERE, "ip_profile_stacks.py"), "--map", mapfile, "--thread", "both", "--top", "1", profile])
    res, section = {}, None
    for line in out.splitlines():
        if line.startswith("## Render thread"):
            section = "render"
        elif line.startswith("## Main thread"):
            section = "main"
        elif line.startswith("## "):
            section = None
        m = re.match(r"\| (rare|interception|os wait|spin wait|sdk|other) \| [\d.]+ \| ([\d.]+) \| ([\d.]+) \|", line)
        if m and section:
            res[(section, m.group(1))] = (float(m.group(2)), float(m.group(3)))
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--map", required=True)
    ap.add_argument("logs", nargs="+")
    a = ap.parse_args()
    print("| run | level windows | frame ms avg / p99 | swaps/s | frames over 12 ms % | render: Rare / interception "
          "/ spin / OS wait (ms) | main thread busy % |")
    print("|---|---|---|---|---|---|---|")
    for log in a.logs:
        label = os.path.basename(log)[:-4]
        fs = frame_stats(log)
        prof = log[:-4] + ".profile.txt"
        th = threads(prof, a.map) if os.path.exists(prof) else {}
        if not fs:
            print("| %s | no level windows | | | | | |" % label)
            continue
        r = lambda k: th.get(("render", k), (0.0, 0.0))[1]
        render = "%.2f / %.2f / %.2f / %.2f" % (r("rare"), r("interception"), r("spin wait"), r("os wait")) if th else "-"
        main_busy = "%.0f" % (100.0 - th.get(("main", "os wait"), (0.0, 0.0))[0]) if th else "-"
        print("| %s | %d | %.2f / %.2f | %.0f | %.1f | %s | %s |" % (label, fs["windows"], fs["frame"], fs["p99"],
                                                                  1000.0 / fs["frame"], fs["over12"], render,
                                                                  main_busy))


if __name__ == "__main__":
    main()
