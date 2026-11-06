"""Tables from pdz_guest_ip_profile files (guest_ip_profile.cpp; docs/RARE_CODE_PROFILE.md).

    python -I tools/perf/ip_profile_report.py [--map <exe .map>] [--top 30] <profile.txt> [...]

All files given are one group (the runs of one level): every table shows each run's share and the mean, and ms per
frame = the mean share x the mean frame time (the Swap thread is busy or waiting the whole frame, so its samples
cover the frame). Exe addresses are named from the linker map (the play build links with /MAP).
"""
import argparse
import bisect
import os
import re
import sys
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_MAP = os.path.join(HERE, "..", "..", "out", "build", "win-amd64-release-play", "PDZRestored.map")


def load_map(path):
    vas, names = [], []
    if not path or not os.path.exists(path):
        return vas, names
    pat = re.compile(r"^\s*[0-9a-fA-F]{4}:[0-9a-fA-F]{8}\s+(\S+)\s+([0-9a-fA-F]{16})\s+(?:f\s+)?(?:i\s+)?(\S+)")
    with open(path, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            m = pat.match(line)
            if not m:
                continue
            va = int(m.group(2), 16)
            if va < 0x140001000:
                continue
            obj = m.group(3).replace("PDZRestored.exe.lto.", "").replace("perfectdarkzerorecomp.exe.lto.", "")  # old maps too
            vas.append(va)
            names.append((m.group(1), obj))
    order = sorted(range(len(vas)), key=lambda i: vas[i])
    return [vas[i] for i in order], [names[i] for i in order]


def load_exports(path):
    """A system DLL's exports by rva (this machine's copy: the one the game loaded)."""
    import struct
    try:
        d = open(path, "rb").read()
    except OSError:
        return [], []
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    nsec = struct.unpack_from("<H", d, pe + 6)[0]
    optsz = struct.unpack_from("<H", d, pe + 20)[0]
    opt = pe + 24
    exp_rva = struct.unpack_from("<I", d, opt + 112)[0]
    secs = []
    for i in range(nsec):
        o = opt + optsz + 40 * i
        vsz, va, rsz, rptr = struct.unpack_from("<IIII", d, o + 8)
        secs.append((va, vsz, rptr))

    def off(rva):
        for va, vsz, rptr in secs:
            if va <= rva < va + max(vsz, 1):
                return rva - va + rptr
        raise ValueError
    try:
        nf, nn, af, anp, aop = struct.unpack_from("<IIIII", d, off(exp_rva) + 20)
        funcs = [struct.unpack_from("<I", d, off(af) + 4 * i)[0] for i in range(nf)]
        names = {}
        for i in range(nn):
            np_ = struct.unpack_from("<I", d, off(anp) + 4 * i)[0]
            ordi = struct.unpack_from("<H", d, off(aop) + 2 * i)[0]
            names.setdefault(funcs[ordi], d[off(np_):off(np_) + 96].split(b"\0")[0].decode(errors="replace"))
    except (ValueError, struct.error):
        return [], []
    starts = sorted(names)
    return starts, [names[s] for s in starts]


class Namer:
    def __init__(self, map_path):
        self.vas, self.names = load_map(map_path)
        self.by_name = {}
        for va, (n, obj) in zip(self.vas, self.names):
            self.by_name.setdefault(n, (va, obj))
        sysdir = os.path.join(os.environ.get("SystemRoot", r"C:\Windows"), "System32")
        self.dll = {"ntdll": load_exports(os.path.join(sysdir, "ntdll.dll")),
                    "kernel": load_exports(os.path.join(sysdir, "kernelbase.dll"))}

    def sysname(self, kind, rva):
        starts, names = self.dll.get(kind, ([], []))
        i = bisect.bisect_right(starts, rva) - 1
        if i < 0:
            return "%s+%X" % (kind, rva)
        o = rva - starts[i]
        return "%s!%s%s" % (kind, names[i], "" if o == 0 else "+0x%X" % o)

    def exe(self, rva):
        va = 0x140000000 + rva
        i = bisect.bisect_right(self.vas, va) - 1
        if i < 0:
            return "exe+%X" % rva
        n, obj = self.names[i]
        off = va - self.vas[i]
        n = demangle(n)
        return ("%s (%s)" % (n, obj)) if off == 0 else ("%s+0x%X (%s)" % (n, off, obj))

    def name(self, ident):
        kind, _, val = ident.partition(":")
        if not val:
            return ident
        if kind == "guest":
            return "sub_" + val
        if kind == "hook":
            obj = self.by_name.get("sub_" + val, (0, "?"))[1]
            return "sub_%s hook (%s)" % (val, obj)
        if kind == "exe":
            return self.exe(int(val, 16))
        if kind in ("ntdll", "kernel"):
            return self.sysname(kind, int(val, 16))
        return ident


def demangle(n):
    # Short, readable MSVC names: ?Name@Ns@pdz@@... -> pdz::Ns::Name
    if n.startswith("?") and "@@" in n:
        parts = n[1:].split("@@")[0].split("@")
        parts = [p for p in parts if p and not p.startswith("?")]
        if parts:
            return "::".join(reversed(parts))
    return n


def load(path):
    d = {"frames": 0, "frame_ms": 0.0, "samples": 0, "cost": 0.0, "cost_max": 0.0, "sampler_samples": 0}
    t = defaultdict(lambda: defaultdict(int))
    threads = {}
    with open(path, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            p = line.split()
            if not p or p[0].startswith("#"):
                continue
            k = p[0]
            if k in ("frames", "samples", "sampler_samples"):
                d[k] = int(p[1])
            elif k == "frame_ms":
                d[k] = float(p[1])
            elif k == "sampler_cost_us_avg":
                d["cost"] = float(p[1])
            elif k == "sampler_cost_us_max":
                d["cost_max"] = float(p[1])
            elif k in ("where", "category", "self", "inner", "incl"):
                t[k][p[1]] += int(p[2])
            elif k == "caller":
                t["caller"][(p[1], p[2])] += int(p[3])
            elif k == "host":
                t["host"][(p[1], p[2])] += int(p[3])
            elif k == "thread":
                threads[p[1]] = {"id": p[2], "samples": int(p[3]), "waits": int(p[4]), "name": " ".join(p[5:]),
                                 "self": defaultdict(int), "inner": defaultdict(int), "incl": defaultdict(int)}
            elif k in ("tself", "tinner", "tincl"):
                threads[p[1]][k[1:]][p[2]] += int(p[3])
            elif k == "tcaller":
                threads[p[1]].setdefault("caller", defaultdict(int))[(p[2], p[3])] += int(p[4])
    d["t"] = t
    d["threads"] = threads
    return d


def pct(n, total):
    return 100.0 * n / total if total else 0.0


def table(runs, key, namer, top, frame_ms, extra=None, filt=None):
    keys = defaultdict(float)
    for r in runs:
        for k, v in r["t"][key].items():
            if filt and not filt(k):
                continue
            keys[k] += pct(v, r["samples"]) / len(runs)
    rows = sorted(keys.items(), key=lambda kv: -kv[1])[:top]
    head = "| # | id | name | " + " | ".join("run %d %%" % (i + 1) for i in range(len(runs))) + " | mean % | ms / frame |"
    if extra:
        head += " " + extra[0] + " |"
    out = [head, "|" + "---|" * (head.count("|") - 1)]
    for i, (k, mean) in enumerate(rows, 1):
        per = " | ".join("%.2f" % pct(r["t"][key].get(k, 0), r["samples"]) for r in runs)
        line = "| %d | %s | %s | %s | %.2f | %.3f |" % (i, k, namer.name(k), per, mean, mean / 100.0 * frame_ms)
        if extra:
            line += " " + extra[1](k) + " |"
        out.append(line)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--map", default=DEFAULT_MAP)
    ap.add_argument("--top", type=int, default=30)
    ap.add_argument("files", nargs="+")
    a = ap.parse_args()
    namer = Namer(a.map)
    runs = [load(f) for f in a.files]
    frame_ms = sum(r["frame_ms"] / max(1, r["frames"]) for r in runs) / len(runs)
    print("## Runs\n")
    print("| run | file | level frames | frame ms | samples | samples / frame | sampler us / sample (max) |")
    print("|---|---|---|---|---|---|---|")
    for i, (f, r) in enumerate(zip(a.files, runs), 1):
        print("| %d | %s | %d | %.3f | %d | %.2f | %.1f (%.0f) |" % (
            i, os.path.basename(f), r["frames"], r["frame_ms"] / max(1, r["frames"]), r["samples"],
            r["samples"] / max(1, r["frames"]), r["cost"], r["cost_max"]))
    print("\nMean frame %.3f ms.\n" % frame_ms)

    print("## Swap thread by section (game_thread_sampler marks)\n")
    print("\n".join(table(runs, "where", namer, 20, frame_ms)))
    print("\n## Swap thread by code kind (top frame)\n")
    print("\n".join(table(runs, "category", namer, 20, frame_ms)))

    callers = defaultdict(lambda: defaultdict(int))
    for r in runs:
        for (callee, caller), v in r["t"]["caller"].items():
            callers[callee][caller] += v

    def top_callers(ident):
        val = ident.partition(":")[2]
        c = callers.get(val)
        if not c:
            return ""
        tot = sum(c.values())
        best = sorted(c.items(), key=lambda kv: -kv[1])[:3]
        return ", ".join("%s %.0f%%" % (k, pct(v, tot)) for k, v in best)

    def incl_of(ident):
        return "%.1f" % (sum(pct(r["t"]["incl"].get(ident, 0), r["samples"]) for r in runs) / len(runs))

    print("\n## Swap thread: top %d by self time (the function the instruction pointer is in)\n" % a.top)
    print("\n".join(table(runs, "self", namer, a.top, frame_ms,
                          ("incl % | top callers (of the innermost guest frame)",
                           lambda k: incl_of(k) + " | " + top_callers(k)))))
    print("\n## Swap thread: top %d guest functions by self + the host code they called (innermost guest frame)\n" % a.top)
    print("\n".join(table(runs, "inner", namer, a.top, frame_ms,
                          ("incl % | top callers", lambda k: incl_of(k) + " | " + top_callers(k)))))
    print("\n## Swap thread: top %d guest functions by inclusive time\n" % a.top)
    print("\n".join(table(runs, "incl", namer, a.top, frame_ms)))

    host = defaultdict(float)
    for r in runs:
        for (top, inner), v in r["t"]["host"].items():
            host[(top, inner)] += pct(v, r["samples"]) / len(runs)
    print("\n## Swap thread: non-guest top frames and the guest function under them (top 25)\n")
    print("| top frame | name | guest function under it | mean % | ms / frame |")
    print("|---|---|---|---|---|")
    for (top, inner), m in sorted(host.items(), key=lambda kv: -kv[1])[:25]:
        print("| %s | %s | sub_%s | %.2f | %.3f |" % (top, namer.name(top), inner, m, m / 100.0 * frame_ms))

    # Other threads, matched across runs by role: the guest function nearest the thread's entry that every sample has
    # (the highest inclusive count outside the kernel / CRT wrappers at 0x82AD0000-0x82AE0000).
    def role(th):
        cands = [(v, k) for k, v in th["incl"].items()
                 if k.startswith("guest:") and not (0x82AD0000 <= int(k[6:], 16) < 0x82AE0000)]
        if not cands:
            return re.sub(r"XThread[0-9A-F]+ |\s*\(F8[0-9A-F]+\)", "", th["name"]).strip() or th["name"]
        top = max(cands)[0]
        entry = max(k for v, k in cands if v >= 0.98 * top) if top else ""
        return "thread under " + namer.name(entry)

    roles = defaultdict(list)  # role -> [(run index, thread)]
    for ri, r in enumerate(runs):
        groups = defaultdict(list)
        for th in r["threads"].values():
            groups[role(th)].append(th)
        for g, lst in groups.items():
            # Threads with the same entry (a worker pool) are told apart by how busy they are in this run.
            lst.sort(key=lambda t: -(t["samples"] - t["waits"]) / max(1, t["samples"]))
            for rank, th in enumerate(lst, 1):
                roles[g if len(lst) == 1 else "%s #%d by busy" % (g, rank)].append((ri, th))
    if roles:
        print("\n## Other guest threads (share of each thread's own samples; matched across runs by entry function)\n")
        print("| thread | OS names | samples | waiting % | busy % | top inner guest functions (% of the thread) |")
        print("|---|---|---|---|---|---|")

        def busy_share(item):
            s = sum(t["samples"] for _, t in item[1])
            w = sum(t["waits"] for _, t in item[1])
            return (s - w) / max(1, s)

        for rname, lst in sorted(roles.items(), key=lambda kv: -busy_share(kv)):
            s = sum(t["samples"] for _, t in lst)
            w = sum(t["waits"] for _, t in lst)
            inner = defaultdict(int)
            for _, t in lst:
                for k, v in t["inner"].items():
                    inner[k] += v
            best = sorted(inner.items(), key=lambda kv: -kv[1])[:4]
            print("| %s | %s | %d | %.1f | %.1f | %s |" % (
                rname, ", ".join(sorted(set(t["name"] for _, t in lst))), s, pct(w, s), pct(s - w, s),
                ", ".join("%s %.1f" % (namer.name(k), pct(v, s)) for k, v in best)))
        for rname, lst in sorted(roles.items(), key=lambda kv: -busy_share(kv))[:2]:
            if busy_share((rname, lst)) < 0.5:
                continue
            per_run = [None] * len(runs)
            for ri, t in lst:
                if per_run[ri] is None or t["samples"] > per_run[ri]["samples"]:
                    per_run[ri] = t
            present = [t for t in per_run if t is not None]
            tcallers = defaultdict(lambda: defaultdict(int))
            for t in present:
                for (callee, caller), v in t.get("caller", {}).items():
                    tcallers[callee][caller] += v

            def tc(ident):
                c = tcallers.get(ident.partition(":")[2])
                if not c:
                    return ""
                tot = sum(c.values())
                return ", ".join("%s %.0f%%" % (k, pct(v, tot)) for k, v in sorted(c.items(), key=lambda kv: -kv[1])[:3])

            for what, key in (("self (the function the instruction pointer is in)", "self"),
                              ("self + the host code it called (innermost guest frame)", "inner"),
                              ("inclusive", "incl")):
                mean = defaultdict(float)
                for t in present:
                    for k, v in t[key].items():
                        mean[k] += pct(v, t["samples"]) / len(present)
                print("\n### %s: top %d by %s\n" % (rname, a.top, what))
                head = "| # | id | name | " + " | ".join("run %d %%" % (i + 1) for i in range(len(present))) + \
                       " | mean % | ms / frame | incl % | top callers |"
                print(head)
                print("|" + "---|" * (head.count("|") - 1))
                for i, (k, m) in enumerate(sorted(mean.items(), key=lambda kv: -kv[1])[:a.top], 1):
                    per = " | ".join("%.2f" % pct(t[key].get(k, 0), t["samples"]) for t in present)
                    inc = sum(pct(t["incl"].get(k, 0), t["samples"]) for t in present) / len(present)
                    print("| %d | %s | %s | %s | %.2f | %.3f | %.1f | %s |" % (
                        i, k, namer.name(k), per, m, m / 100.0 * frame_ms, inc, tc(k)))
            cats = defaultdict(float)
            for t in present:
                for k, v in t["self"].items():
                    cats[k.partition(":")[0]] += pct(v, t["samples"]) / len(present)
            print("\n### %s: by code kind (top frame)\n" % rname)
            print("| kind | mean % | ms / frame |")
            print("|---|---|---|")
            for k, m in sorted(cats.items(), key=lambda kv: -kv[1]):
                print("| %s | %.2f | %.3f |" % (k, m, m / 100.0 * frame_ms))


if __name__ == "__main__":
    main()
