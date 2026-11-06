"""Per-sample analysis of pdz_guest_ip_profile's collapsed stacks (docs/RARE_CODE_PROFILE.md).

    python -I tools/perf/ip_profile_stacks.py --map <exe .map> [--top 30] [--thread render|swap|both] <profile.txt> [...]

All files are one group (the runs of one level). Each sample of a thread is put in one bucket, from its stack:
  os wait        an OS wait / yield export at the top (ntdll NtWaitFor..., NtDelayExecution, NtYieldExecution)
  spin wait      anywhere inside the guest D3D fence wait sub_826DE258 or the ring-space wait sub_826E3968
                 (they poll a value the GPU side writes: our record thread and the GPU plugin's command processor)
  interception   the first frame that is not OS / CRT code is the mod's own (pdz:: code, a hook)
  sdk            ... is rexruntime.dll / the GPU plugin (kernel shims, memory, threads)
  rare           ... is Rare's recompiled code (a guest function, its cold chunk, or the register save / restore
                 helpers __savegprlr_* / __restgprlr_* / __savefpr_* / ...)
ms per frame = the share x the mean frame time (the thread is busy or waiting for the whole frame).
"""
import argparse
import os
import re
import sys
from collections import defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ip_profile_report as rep  # noqa: E402

SPIN = {"826DE258", "826E3968"}
HELPER = re.compile(r"^(?:__imp__)?__(save|rest)(gpr|fpr|vmx)(lr)?_\d+")


class Frames:
    """Turns profile ids into (kind, key, label): kind os / crt / rare / helper / mod / sdk / other."""

    def __init__(self, namer):
        self.namer = namer
        self.cache = {}

    def get(self, ident):
        c = self.cache.get(ident)
        if c:
            return c
        kind, _, val = ident.partition(":")
        if kind == "guest":
            c = ("rare", val, "sub_" + val)
        elif kind == "hook":
            c = ("mod", "hook:" + val, self.namer.name(ident))
        elif kind == "exe":
            n = self.namer.exe(int(val, 16))
            base = n.split(" (")[0]
            m = re.match(r"^(?:__imp__)?sub_([0-9A-F]{8})", base)
            h = HELPER.match(base)
            if h:
                c = ("helper", h.group(0).replace("__imp__", ""), h.group(0).replace("__imp__", ""))
            elif m:
                c = ("rare", m.group(1), "sub_" + m.group(1))
            elif re.match(r"^(memcpy|memset|memmove|memcmp)", base):
                c = ("crt", base, base)
            else:
                c = ("mod", base.split("+0x")[0], base.split("+0x")[0] + " (" + n.split(" (")[-1])
        elif kind == "wait":
            c = ("os", ident, ident)
        elif kind in ("ntdll", "kernel"):
            c = ("os", ident, self.namer.name(ident))
        elif kind.upper().startswith("VCRUNTIME") or kind.lower().startswith("ucrtbase"):
            c = ("crt", ident.split(":")[0], ident.split(":")[0])
        elif kind in ("rexruntime", "gpu"):
            c = ("sdk", ident, ident)
        else:
            c = ("other", ident, ident)
        self.cache[ident] = c
        return c


def load_stacks(path):
    out = defaultdict(lambda: defaultdict(int))  # thread -> stack tuple -> count
    meta = {"frames": 0, "frame_ms": 0.0}
    names = {}
    with open(path, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            if line.startswith("stack "):
                _, who, count, frames = line.rstrip("\n").split(" ", 3)
                out[who][tuple(frames.split(";"))] += int(count)
            elif line.startswith("frames "):
                meta["frames"] = int(line.split()[1])
            elif line.startswith("frame_ms "):
                meta["frame_ms"] = float(line.split()[1])
            elif line.startswith("thread "):
                p = line.split()
                names[p[1]] = " ".join(p[5:])
    return out, meta, names


def analyse(stacks, fr):
    """Buckets, Rare self / inclusive / callers, the mod's functions, for one thread's stacks."""
    total = sum(stacks.values())
    buckets = defaultdict(int)
    rare_self = defaultdict(int)
    rare_incl = defaultdict(int)
    callers = defaultdict(lambda: defaultdict(int))
    mod_self = defaultdict(int)
    mod_under = defaultdict(lambda: defaultdict(int))
    helper_self = 0
    for st, n in stacks.items():
        frames = [fr.get(i) for i in st]
        seen = set()
        for k, key, _ in frames:
            if k == "rare" and key not in seen:
                seen.add(key)
                rare_incl[key] += n
        if frames and frames[0][0] == "os" and frames[0][1].startswith("wait:"):
            buckets["os wait"] += n
            continue
        if any(k in ("rare", "mod") and key.replace("hook:", "") in SPIN for k, key, _ in frames):
            buckets["spin wait"] += n
            continue
        first = next(((i, f) for i, f in enumerate(frames) if f[0] not in ("os", "crt")), None)
        if first is None:
            buckets["os / crt (no caller)"] += n
            continue
        i, (k, key, label) = first
        if k == "mod":
            buckets["interception"] += n
            mod_self[label] += n
            under = next((f[1] for f in frames[i + 1:] if f[0] == "rare"), "-")
            mod_under[label][under] += n
            continue
        if k == "sdk":
            buckets["sdk"] += n
            continue
        if k == "helper":
            buckets["rare"] += n
            helper_self += n
            key = next((f[1] for f in frames[i + 1:] if f[0] == "rare"), key)
        elif k != "rare":
            buckets["other"] += n
            continue
        else:
            buckets["rare"] += n
        rare_self[key] += n
        caller = next((f[1] for f in frames[i + 1:] if f[0] == "rare" and f[1] != key), None)
        if caller:
            callers[key][caller] += n
    return {"total": total, "buckets": buckets, "rare_self": rare_self, "rare_incl": rare_incl, "callers": callers,
            "mod_self": mod_self, "mod_under": mod_under, "helper_self": helper_self}


def pick_record(stacks_by_thread, names):
    for who in stacks_by_thread:
        if "record thread" in names.get(who, ""):
            return who
    return None


def pick_render(stacks_by_thread, names):
    best, best_busy = None, -1.0
    for who, st in stacks_by_thread.items():
        if who == "swap" or "record thread" in names.get(who, ""):
            continue
        tot = sum(st.values())
        waits = sum(v for s, v in st.items() if s and s[0].startswith("wait:"))
        busy = (tot - waits) / max(1, tot)
        if tot >= 1000 and busy > best_busy:
            best, best_busy = who, busy
    return best


def table_rows(runs, field, top, frame_ms, extra):
    mean = defaultdict(float)
    for r in runs:
        for k, v in r[field].items():
            mean[k] += 100.0 * v / r["total"] / len(runs)
    return sorted(mean.items(), key=lambda kv: -kv[1])[:top]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--map", default=rep.DEFAULT_MAP)
    ap.add_argument("--top", type=int, default=30)
    ap.add_argument("--thread", default="both", choices=["render", "swap", "record", "both", "all"])
    ap.add_argument("files", nargs="+")
    a = ap.parse_args()
    namer = rep.Namer(a.map)
    fr = Frames(namer)
    loaded = [load_stacks(f) for f in a.files]
    frame_ms = sum(m["frame_ms"] / max(1, m["frames"]) for _, m, _ in loaded) / len(loaded)
    print("Runs: %s; mean frame %.3f ms.\n" % (", ".join(os.path.basename(f) for f in a.files), frame_ms))
    threads = {"both": ["render", "swap"], "all": ["render", "record", "swap"]}.get(a.thread, [a.thread])
    for th in threads:
        runs = []
        for st, m, names in loaded:
            who = "swap" if th == "swap" else pick_record(st, names) if th == "record" else pick_render(st, names)
            if who is None or who not in st:
                continue
            r = analyse(st[who], fr)
            r["name"] = "Swap (main) thread" if who == "swap" else names.get(who, who)
            runs.append(r)
        if not runs:
            continue
        title = "Main thread (the Swap thread)" if th == "swap" else "%s thread (%s)" % (
            "Record" if th == "record" else "Render", ", ".join(sorted(set(r["name"] for r in runs))))
        print("## %s\n" % title)
        print("Samples: %s.\n" % ", ".join(str(r["total"]) for r in runs))
        print("| bucket | " + " | ".join("run %d %%" % (i + 1) for i in range(len(runs))) + " | mean % | ms / frame |")
        print("|---|" + "---|" * (len(runs) + 2))
        keys = sorted({k for r in runs for k in r["buckets"]}, key=lambda k: -sum(r["buckets"].get(k, 0) / r["total"] for r in runs))
        for k in keys:
            per = [100.0 * r["buckets"].get(k, 0) / r["total"] for r in runs]
            m = sum(per) / len(per)
            print("| %s | %s | %.1f | %.2f |" % (k, " | ".join("%.1f" % p for p in per), m, m / 100 * frame_ms))
        hs = sum(100.0 * r["helper_self"] / r["total"] for r in runs) / len(runs)
        print("\nOf 'rare': the register save / restore helpers (__savegprlr_* etc., counted to the function that "
              "called them below) %.1f%% = %.2f ms per frame.\n" % (hs, hs / 100 * frame_ms))

        callers = defaultdict(lambda: defaultdict(int))
        for r in runs:
            for k, c in r["callers"].items():
                for kk, v in c.items():
                    callers[k][kk] += v

        def tc(k):
            c = callers.get(k)
            if not c:
                return ""
            t = sum(c.values())
            return ", ".join("sub_%s %.0f%%" % (kk, 100.0 * v / t) for kk, v in sorted(c.items(), key=lambda kv: -kv[1])[:3])

        print("### Rare's code: top %d functions by self time (incl. the helpers and host code they call)\n" % a.top)
        print("| # | guest function | " + " | ".join("run %d %%" % (i + 1) for i in range(len(runs))) +
              " | mean % of thread | ms / frame | inclusive % | top callers |")
        print("|---|---|" + "---|" * (len(runs) + 4))
        for i, (k, m) in enumerate(table_rows(runs, "rare_self", a.top, frame_ms, None), 1):
            per = " | ".join("%.2f" % (100.0 * r["rare_self"].get(k, 0) / r["total"]) for r in runs)
            inc = sum(100.0 * r["rare_incl"].get(k, 0) / r["total"] for r in runs) / len(runs)
            print("| %d | sub_%s | %s | %.2f | %.3f | %.1f | %s |" % (i, k, per, m, m / 100 * frame_ms, inc, tc(k)))
        rs = sum(100.0 * r["buckets"].get("rare", 0) / r["total"] for r in runs) / len(runs)
        top_sum = sum(m for _, m in table_rows(runs, "rare_self", a.top, frame_ms, None))
        top10 = sum(m for _, m in table_rows(runs, "rare_self", 10, frame_ms, None))
        nfun = len({k for r in runs for k in r["rare_self"]})
        print("\nRare's code %.1f%% of the thread: the top 10 functions %.1f%%, the top %d %.1f%%, the other %.1f%% "
              "spread over the rest of %d functions seen.\n" % (rs, top10, a.top, top_sum, rs - top_sum, nfun))
        print("### Rare's code: top 15 by inclusive time\n")
        print("| guest function | inclusive % | ms / frame |")
        print("|---|---|---|")
        for k, m in table_rows(runs, "rare_incl", 15, frame_ms, None):
            print("| sub_%s | %.1f | %.2f |" % (k, m, m / 100 * frame_ms))
        if any(r["mod_self"] for r in runs):
            print("\n### The interception: the mod's functions at the top (incl. the OS / CRT calls they make)\n")
            print("| mod function | mean % of thread | ms / frame | Rare function under it |")
            print("|---|---|---|---|")
            under = defaultdict(lambda: defaultdict(int))
            for r in runs:
                for k, c in r["mod_under"].items():
                    for kk, v in c.items():
                        under[k][kk] += v
            for k, m in table_rows(runs, "mod_self", 15, frame_ms, None):
                u = under[k]
                t = sum(u.values())
                us = ", ".join("sub_%s %.0f%%" % (kk, 100.0 * v / t) for kk, v in sorted(u.items(), key=lambda kv: -kv[1])[:2])
                print("| %s | %.2f | %.3f | %s |" % (k, m, m / 100 * frame_ms, us))
        print()


if __name__ == "__main__":
    main()
