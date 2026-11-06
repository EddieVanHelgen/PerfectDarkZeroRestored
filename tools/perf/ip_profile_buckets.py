"""Where a thread's time goes, in buckets, from pdz_guest_ip_profile files (docs/RARE_CODE_PROFILE.md).

APPROXIMATE: it works from the per-function counts, so a spin-wait that calls host code (or the register helpers)
can be counted twice and the buckets can sum to over 100%. Use ip_profile_stacks.py (exact, per sample, from the
collapsed stacks) when the files have them; this one is for files written before the stacks were added.

    python -I tools/perf/ip_profile_buckets.py --map <exe .map> [--thread render|swap] <profile.txt> [...]

Buckets per sample (by the top frame, then the innermost guest frame):
  wait (os)       an OS wait / yield export at the top
  spin (fence)    inside the guest D3D fence wait sub_826DE258 (its poll sub_826F36B8 and what they call)
  interception    the mod's code at the top (pdz:: functions, hooks, the CRT and ntdll calls they make)
  rare            Rare's recompiled code at the top (incl. the register save / restore helpers and cold chunks)
  sdk             rexruntime.dll / the GPU plugin
  other
"""
import argparse
import os
import sys
from collections import defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ip_profile_report as rep  # noqa: E402

SPIN = {"826DE258", "826F36B8"}


def classify(ident, namer):
    kind, _, val = ident.partition(":")
    if kind == "wait":
        return "wait (os)"
    if kind == "guest":
        return "rare"
    if kind == "hook":
        return "interception"
    if kind == "exe":
        n = namer.exe(int(val, 16))
        if n.startswith("__imp__sub_") or n.startswith("sub_") or n.startswith("__imp____savegpr") or \
                n.startswith("__imp____restgpr") or n.startswith("__imp____savefpr") or n.startswith("__imp____restfpr") \
                or n.startswith("__imp____savevmx") or n.startswith("__imp____restvmx"):
            return "rare"
        return "interception"
    if kind in ("rexruntime", "gpu"):
        return "sdk"
    if kind in ("ntdll", "kernel") or kind.startswith("VCRUNTIME") or kind.startswith("ucrtbase"):
        return "interception?"  # resolved below by the guest frame under it
    return "other"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--map", default=rep.DEFAULT_MAP)
    ap.add_argument("--thread", default="render", choices=["render", "swap"])
    ap.add_argument("files", nargs="+")
    a = ap.parse_args()
    namer = rep.Namer(a.map)
    totals = []
    for f in a.files:
        r = rep.load(f)
        if a.thread == "swap":
            self_ = r["t"]["self"]
            inner = r["t"]["inner"]
            samples = r["samples"]
        else:
            th = max(r["threads"].values(), key=lambda t: t["samples"] - t["waits"])
            self_, inner, samples = th["self"], th["inner"], th["samples"]
        b = defaultdict(int)
        spin_inner = sum(v for k, v in inner.items() if k.partition(":")[2] in SPIN)
        for k, v in self_.items():
            c = classify(k, namer)
            if k.partition(":")[2] in SPIN:
                c = "spin (fence)"
            b[c] += v
        # Samples whose innermost guest frame is the spin wait but whose top is host code: count them as spin.
        spin_self = b["spin (fence)"]
        moved = max(0, spin_inner - spin_self)
        b["spin (fence)"] += moved
        b["interception?"] -= min(b["interception?"], moved)
        b["interception"] += b.pop("interception?", 0)
        totals.append({k: 100.0 * v / samples for k, v in b.items()})
        frame = r["frame_ms"] / max(1, r["frames"])
        totals[-1]["_frame"] = frame
    keys = sorted({k for t in totals for k in t if not k.startswith("_")}, key=lambda k: -sum(t.get(k, 0) for t in totals))
    print("| bucket | " + " | ".join("run %d %%" % (i + 1) for i in range(len(totals))) + " | mean % | ms / frame |")
    print("|---|" + "---|" * (len(totals) + 2))
    frame = sum(t["_frame"] for t in totals) / len(totals)
    for k in keys:
        m = sum(t.get(k, 0) for t in totals) / len(totals)
        print("| %s | %s | %.1f | %.2f |" % (k, " | ".join("%.1f" % t.get(k, 0) for t in totals), m, m / 100 * frame))
    print("\nframe %.3f ms" % frame)


if __name__ == "__main__":
    main()
