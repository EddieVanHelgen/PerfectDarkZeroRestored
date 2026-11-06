"""boxtest.py MAP PADS_RAW SETUPS_TXT: Rare's link-vs-box test offline with Sabotage property as obstacles
(approximate boxes), and which property pads stay reachable from the map's biggest pad network."""
import sys, re, math, collections
sys.path.insert(0, '.')
from padgraph import load, near, reach, path
HALF = {'misc_sabotdrum': 35, 'misc_sabotcrate': 50, 'misc_sabotammo': 50, 'misc_sabotcomms': 60,
        'misc_sabotcommssmall': 40, 'misc_sabotpower': 80}

def boxes(fn):
    out = []
    for line in open(fn):
        m = re.match(r'(\S+)\s+\S+\s+(\S+)\s+size\s+(\d+) filter\s+(\d+) pos \(\s*(\S+)\s+(\S+)\s+(\S+)\)', line)
        if not m: continue
        t, name, size, f, x, y, z = m.groups()
        if int(f) != 19 or int(size) != 144 or not t.startswith('misc_sabot'): continue
        h = HALF.get(t, 50); x, y, z = float(x), float(y), float(z)
        out.append((t, name, (x, y, z), (x - h, y - 10, z - h), (x + h, y + 120, z + h)))
    return out

def seg_box(p, q, lo, hi, lift=30):
    p = (p[0], p[1] + lift, p[2]); q = (q[0], q[1] + lift, q[2]); t0, t1 = 0.0, 1.0
    for i in range(3):
        d = q[i] - p[i]
        if abs(d) < 1e-9:
            if p[i] < lo[i] or p[i] > hi[i]: return False
        else:
            a = (lo[i] - p[i]) / d; b = (hi[i] - p[i]) / d
            if a > b: a, b = b, a
            t0 = max(t0, a); t1 = min(t1, b)
            if t0 > t1: return False
    return True

mapname, raw, setups = sys.argv[1:4]
pads = load(raw); bx = boxes(setups)
closed = {}
for a, p in pads.items():
    for b in p['links']:
        if b in pads:
            for t, n, c, lo, hi in bx:
                if seg_box(p['pos'], pads[b]['pos'], lo, hi):
                    closed[(a, b)] = (t, n)
# the biggest network (directed reach) as the "rest of the map"
best = max(pads, key=lambda s: len(reach(pads, s)) if pads[s]['type'] == 7 else 0)
main = reach(pads, best)
main_closed = reach(pads, best, frozenset(closed))
print('== %s: %d pads, %d property pieces, %d links closed by property; biggest network from pad %d: %d pads (%d with property closed)' % (
    mapname, len(pads), len(bx), len(closed), best, len(main), len(main_closed)))
rows = []
for t, n, c, lo, hi in bx:
    nn = near(pads, c, 600)
    if not nn:
        rows.append((t, n, c, None, None, '-', '-')); continue
    d, pid = nn[0]
    rows.append((t, n, c, pid, round(d), pid in main, pid in main_closed))
for r in sorted(rows, key=lambda r: (r[0], r[2])):
    t, n, c, pid, d, o, cl = r
    print('  %-20s at (%6.0f %5.0f %6.0f): nearest pad %s %s cm; reachable: open graph %s, with property closed %s' % (
        t.replace('misc_', ''), c[0], c[1], c[2], pid, d, o, cl))
