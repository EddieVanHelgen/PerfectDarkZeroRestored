"""pads.py RAW...: parse the aipads asset (launch pad loader sub_8215DF20, v2 layout) in inflated pad packages.
Pad record = 60 bytes at data+260; +0/+4/+8 position (floats), +24 type (u32), +32 id (u16), +57 group (s8).
Spawn pads = type 7 (sub_82160B08: flag 0x400000, subtype 3 -> pad manager +1100 list)."""
import re, struct, sys, collections
TYPES_SPECIAL = {5: 'sub1', 6: 'sub2', 7: 'SPAWN(sub3)', 18: 'sub4', 19: 'sub5', 20: 'sub6'}
def parse(path, verbose=False):
    d = open(path, 'rb').read()
    m = re.search(rb'(aid_aipads_[a-z0-9_]+)\x00+aipads\x00', d)
    if not m: return None
    name = m.group(1).decode()
    t = m.end()
    for v in range((t + 3) & ~3, t + 0x40, 4):
        ver = struct.unpack('>I', d[v:v+4])[0]
        if ver in (1, 2):
            data = v + 16
            n = struct.unpack('>H', d[data:data+2])[0]
            p0 = data + 260
            if ver == 2 and struct.unpack('>H', d[p0+32:p0+34])[0] == 0 and struct.unpack('>H', d[p0+60+32:p0+60+34])[0] == 1:
                break
    else:
        return name, None
    pads = []
    for i in range(n):
        o = p0 + 60 * i
        x, y, z = struct.unpack('>3f', d[o:o+12])
        typ = struct.unpack('>I', d[o+24:o+28])[0]
        pid = struct.unpack('>H', d[o+32:o+34])[0]
        grp = struct.unpack('>b', d[o+57:o+58])[0]
        pads.append((pid, typ, grp, x, y, z, d[o:o+60]))
    return name, dict(version=ver, data=data, count=n, pads=pads)
for path in sys.argv[1:]:
    r = parse(path)
    if r is None or r[1] is None:
        print(path, 'no pads', r); continue
    name, info = r
    tc = collections.Counter(p[1] for p in info['pads'])
    spawn = [p for p in info['pads'] if p[1] == 7]
    gc = collections.Counter(p[2] for p in spawn)
    print(f"{name}: v{info['version']} data@{info['data']:#x} pads={info['count']} types={dict(sorted(tc.items()))}")
    print(f"   spawn pads (type 7) = {len(spawn)}; by group byte (+57): {dict(sorted(gc.items()))}")
    for t in (5, 6, 18, 19, 20):
        sp = [p for p in info['pads'] if p[1] == t]
        if sp: print(f"   type {t}: {len(sp)} by group {dict(sorted(collections.Counter(p[2] for p in sp).items()))}")
    if '-v' in sys.argv[0:0] : pass
