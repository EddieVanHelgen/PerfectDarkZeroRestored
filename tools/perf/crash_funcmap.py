# Map host crash addresses (the 'pdz crash: stack' RVAs, exe+0x...) to guest functions, for builds without a PDB.
# Reads the exe's PPCFuncMappings table ({guest, host} 16-byte pairs in a data section) and names, for each RVA, the
# mapped function starting at or before it. PGO / LTO can move cold blocks, so check the offset is plausible.
#   python -I tools/perf/crash_funcmap.py <PDZRestored.exe> 0x45174B7 0x4515BDF ...
import struct, sys, bisect
exe = sys.argv[1]
d = open(exe, "rb").read()
pe = struct.unpack_from("<I", d, 0x3C)[0]
nsec = struct.unpack_from("<H", d, pe + 6)[0]
optsz = struct.unpack_from("<H", d, pe + 20)[0]
opt = pe + 24
base = struct.unpack_from("<Q", d, opt + 24)[0]
secs = []
for i in range(nsec):
    o = opt + optsz + 40 * i
    name = d[o:o + 8].rstrip(b"\0").decode(errors="replace")
    vsz, va, rsz, rptr = struct.unpack_from("<IIII", d, o + 8)
    secs.append((name, va, vsz, rptr, rsz))
text = [s for s in secs if s[0] == ".text"][0]
tlo, thi = base + text[1], base + text[1] + text[2]
best = []
for name, va, vsz, rptr, rsz in secs:
    if name == ".text":
        continue
    blob = d[rptr:rptr + rsz]
    run = []
    for off in range(0, len(blob) - 16, 8):
        pass
    # scan aligned 16-byte entries at both 8-byte phases
    for phase in (0, 8):
        run = []
        for off in range(phase, len(blob) - 16, 16):
            g, h = struct.unpack_from("<QQ", blob, off)
            if 0x82000000 <= g < 0x84000000 and tlo <= h < thi:
                run.append((g, h - base))
            else:
                if len(run) > len(best):
                    best = run
                run = []
        if len(run) > len(best):
            best = run
print("mapping entries", len(best), "first", hex(best[0][0]) if best else None)
starts = sorted((h, g) for g, h in best)
hs = [h for h, g in starts]
for a in sys.argv[2:]:
    rva = int(a, 16)
    i = bisect.bisect_right(hs, rva) - 1
    if i < 0:
        print(a, "before all"); continue
    h, g = starts[i]
    nxt = hs[i + 1] if i + 1 < len(hs) else None
    print(f"{a}: guest sub_{g:08X} (host +0x{h:X}, offset +0x{rva - h:X}, next fn at +0x{nxt:X})" if nxt else a)
