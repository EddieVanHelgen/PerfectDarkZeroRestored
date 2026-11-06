"""make_pipeline_list.py: the native renderer's pipeline list (data/native_pipeline_list.txt), from the developer's
PSO caches (native_cache\\pso_recipes.bin, written by the game's pdz_native_pso_cache).

A pipeline (PSO) is fixed by its shaders and its state. The recipe file the game saves holds, per pipeline, the key
(shader hashes, vertex layout hash, depth / blend / stencil / raster state, topology, target formats, the depth20e4 /
blend7e3 variant flags), the input elements, and the hashes of the three DXIL blobs it was built with, plus the
blobs themselves. This list keeps everything but the blobs: no microcode, no translated shader code, no DXIL. At first
start the game rebuilds the blobs from the player's own translated shaders and compiles every pipeline before play
(src/mod/render/native/native_pipeline_seed.cpp; docs/RELEASE_FIRST_RUN.md "Stage 4"). The list is embedded in the exe
(cmake/pdz_xenosrecomp.cmake); rebuild after regenerating it.

Inputs (read only): every pso_recipes.bin given, merged by (key, elements); default: the play build's, the play
copy's and the fresh test folder's.

  python -I tools\\shaders\\make_pipeline_list.py [--out data\\native_pipeline_list.txt] [recipes.bin ...]
"""
import argparse, os, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
DEFAULT_INPUTS = [
    os.path.join(REPO, "out", "build", "win-amd64-release-play", "native_cache", "pso_recipes.bin"),
    r"C:\Games\PDZ_PlayCopy\native_cache\pso_recipes.bin",
    r"C:\Games\PDZ_FreshTest\userdata\cache\native_cache\pso_recipes.bin",
]
KEY_SIZE = 104
# PsoKey (native_pipeline.cpp): vs_hash, ps_hash, layout_hash (u64); depthcontrol, blendcontrol[4], color_mask,
# stencil_masks, raster (u32); depth_bias (i32); slope_bias (f32, kept as its bits); topology_type, rt_count, rt[4], ds,
# depth_variant, strip_cut, blend7e3_variant (u32).
KEY_FMT = "<QQQ" + "I" * 8 + "i" + "I" + "I" * 10  # 24 + 32 + 4 + 4 + 40 = 104 bytes; the slope read as its bits


def read_recipes(path):
    d = open(path, "rb").read()
    if d[:8] != b"PDZPSORC":
        raise ValueError("not a recipe file")
    fmt, key_size, nblobs, nrecipes = struct.unpack_from("<IIII", d, 8)
    if fmt != 1 or key_size != KEY_SIZE:
        raise ValueError("format %d / key size %d not supported" % (fmt, key_size))
    p = 24
    for _ in range(nblobs):
        _, size = struct.unpack_from("<QI", d, p)
        p += 12 + size
    out = []
    for _ in range(nrecipes):
        key = struct.unpack_from(KEY_FMT, d, p)
        p += KEY_SIZE
        vs_id, ps_id, var_id, age, n = struct.unpack_from("<QQQHH", d, p)
        p += 28
        els = [struct.unpack_from("<IIII", d, p + 16 * i) for i in range(n)]
        p += 16 * n
        out.append((key, (vs_id, ps_id, var_id), tuple(els)))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(REPO, "data", "native_pipeline_list.txt"))
    ap.add_argument("inputs", nargs="*")
    a = ap.parse_args()
    inputs = a.inputs or DEFAULT_INPUTS
    rows, seen, per_file = [], set(), []
    for path in inputs:
        if not os.path.exists(path):
            per_file.append("%s: none" % path)
            continue
        recs = read_recipes(path)
        new = 0
        for key, ids, els in recs:
            k = (key, els)
            if k in seen:
                continue
            seen.add(k)
            rows.append((key, ids, els))
            new += 1
        per_file.append("%s: %d recipes, %d new" % (path, len(recs), new))
    with open(a.out, "w", encoding="ascii", newline="\n") as f:
        f.write("# PDZ Restored: the native renderer's pipeline list. It holds only what fixes each pipeline: shader hashes,\n")
        f.write("# the vertex layout, the depth / blend / stencil / raster state, topology, target formats, the variant flags,\n")
        f.write("# and the hashes of the DXIL it was built with. No game code, no microcode, no translated shader code, no\n")
        f.write("# DXIL. At first start the game rebuilds each pipeline from the player's own translated shaders, before play.\n")
        f.write("# Made by tools/shaders/make_pipeline_list.py from the developer's PSO caches.\n")
        f.write("# <vs> <ps> <layout> <depthcontrol> <blendcontrol x4> <color_mask> <stencil> <raster> <depth_bias> <slope bits>\n")
        f.write("#   <topology> <rt_count> <rt x4> <ds> <depth_variant> <strip_cut> <blend7e3> | <vs dxil> <ps dxil> <variant dxil>\n")
        f.write("#   | <attr:format:stream:offset;...>\n")
        for key, ids, els in rows:
            fields = ["%016x" % key[0], "%016x" % key[1], "%016x" % key[2]] + ["%x" % v for v in key[3:11]] + \
                     ["%d" % key[11], "%08x" % key[12]] + ["%x" % v for v in key[13:]]
            f.write("%s | %016x %016x %016x | %s\n" % (" ".join(fields), ids[0], ids[1], ids[2],
                                                      ";".join("%d:%d:%d:%d" % e for e in els)))
    print("\n".join(per_file))
    print("written %d pipelines to %s (%d KB)" % (len(rows), a.out, os.path.getsize(a.out) // 1024))


if __name__ == "__main__":
    main()
