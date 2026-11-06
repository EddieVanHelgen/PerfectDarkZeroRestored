"""make_bind_list.py: the native renderer's bind list (data/native_bind_list.txt), from the developer's shader cache.

A bound vertex shader is a package VS after the Xbox D3D runtime filled in its vertex fetches from a vertex
declaration (formats, strides, offsets). The player's game files hold only the unbound parents, so the release
ships this list: for each bound variant seen in play, its hash, its parent's hash, and the declaration it was bound
with (per element: usage, usage index, stream, offset, vertex format, signed, integer, type swizzle; per stream: the
stride). No microcode, no translated shader code. At first start the game binds the player's own parent with
binder::Bind and translates the result (native_generate.cpp).

Inputs (read only):
  C:\\Games\\PDZRenderResearch\\native\\cache  manifest.json (the 534 .xsh bound VS and their parents), ucode\\, containers\\
  <build>\\native_shaders                    <hash>.bin of bound VS translated at first use, record\\bound_*.txt
                                            (created VS -> bound variant pairs) and record\\<hash>.bin
Every row is checked: the research binder (tools\\binder\\binder.py) rebuilds the declaration from (parent, bound,
container table) and must bind the parent back to the bound microcode byte for byte; rows that don't are left out.

  python -I tools\\shaders\\make_bind_list.py [--build DIR] [--out data\\native_bind_list.txt]
"""
import argparse, glob, json, os, struct, sys

NATIVE = r"C:\Games\PDZRenderResearch\native"
CACHE = os.path.join(NATIVE, "cache")
sys.path.insert(0, os.path.join(NATIVE, "tools", "binder"))
sys.path.insert(0, os.path.join(NATIVE, "tools", "harvest"))
import binder  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))


def read_dwords(path):
    d = open(path, "rb").read()
    return list(struct.unpack(">%dI" % (len(d) // 4), d))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", default=os.path.join(REPO, "out", "build", "win-amd64-release-play", "native_shaders"))
    ap.add_argument("--out", default=os.path.join(REPO, "data", "native_bind_list.txt"))
    a = ap.parse_args()

    pairs = {}  # bound -> parent
    for r in json.load(open(os.path.join(CACHE, "manifest.json")))["shaders"]:
        if r.get("origin") == "xsh-bound" and r.get("derived_from"):
            pairs[r["hash"]] = r["derived_from"]
    from_records = 0
    for f in glob.glob(os.path.join(a.build, "record", "bound_*.txt")):
        for line in open(f, encoding="utf-8", errors="replace"):
            p = line.split()
            if len(p) >= 5 and p[0] == "vs" and len(p[1]) == 16 and len(p[3]) == 16 and p[1] != p[3]:
                if p[3] not in pairs:
                    pairs[p[3]] = p[1]
                    from_records += 1

    def ucode_of(h):
        for d in (os.path.join(CACHE, "ucode"), a.build, os.path.join(a.build, "record")):
            p = os.path.join(d, h + ".bin")
            if os.path.exists(p):
                return read_dwords(p)
        return None

    rows, skipped = [], {}
    for bound_h, parent_h in sorted(pairs.items()):
        cont = os.path.join(CACHE, "containers", parent_h + ".bin")
        parent, bound = ucode_of(parent_h), ucode_of(bound_h)
        if parent is None or bound is None or not os.path.exists(cont):
            skipped["no parent container or no microcode"] = skipped.get("no parent container or no microcode", 0) + 1
            continue
        table = binder.container_elements(open(cont, "rb").read())
        decl, strides = binder.synthesize_declaration(parent, bound, table)
        if decl is None or any(isinstance(el.type_swz, tuple) for el in decl):
            skipped["no declaration rebuilt"] = skipped.get("no declaration rebuilt", 0) + 1
            continue
        if binder.bind(parent, table, decl, strides) != bound:
            skipped["the binder doesn't reproduce it"] = skipped.get("the binder doesn't reproduce it", 0) + 1
            continue
        els = ";".join("%d:%d:%d:%d:%d:%d:%d:%d" % (e.usage, e.index, e.stream, e.offset_bytes, e.fmt, e.signed, e.integer, e.type_swz)
                       for e in decl)
        st = ";".join("%d:%d" % (s, strides[s]) for s in sorted(strides))
        rows.append("%s %s %s %s" % (bound_h, parent_h, st, els))

    os.makedirs(os.path.dirname(a.out), exist_ok=True)
    with open(a.out, "w", encoding="ascii", newline="\n") as f:
        f.write("# PDZ Restored: the native renderer's bind list. It holds only shader hashes and vertex declaration\n")
        f.write("# formats / strides: no game code, no microcode, no translated shader code. At first start the game binds\n")
        f.write("# the player's own unbound package vertex shader (the parent) to each declaration and translates the result.\n")
        f.write("# Made by tools/shaders/make_bind_list.py from the developer's shader cache; every row checked byte for\n")
        f.write("# byte against the bound microcode it came from.\n")
        f.write("# <bound hash> <parent hash> <stream:stride dwords;...> <usage:index:stream:offset bytes:format:signed:integer:type swizzle;...>\n")
        for r in rows:
            f.write(r + "\n")
    print("pairs %d (%d from the run-time records); written %d rows to %s; skipped %s" % (
        len(pairs), from_records, len(rows), a.out, skipped))


if __name__ == "__main__":
    main()
