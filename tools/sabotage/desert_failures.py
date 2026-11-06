"""desert_failures.py: every Desert Sabotage failure line in play logs, by place (table 1 of docs/TU3_BASE.md
"Desert pathfinding").

    python tools/sabotage/desert_failures.py LOG [LOG ...]

Each log is read from its first Desert line (aid_*_multi_desert) to the next map's (another multi_<map> line), so a
log with several matches gives its Desert match only. Counts per place and kind, per log, and the level-file pads
the lines name (pads never move: a logged position within 60 cm of a file pad is that pad). "line blocked" counts
only within 15 m of the piece (further out it is the walking status). Read-only; needs the game's level packages as
tools/sabotage_navcheck.py finds them.
"""
import re, sys, os, math
from collections import Counter, defaultdict

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
import sabotage_navcheck as nc


def _desert_pads():
    here = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
    docs = os.path.join(os.path.expanduser('~'), 'Documents', 'perfectdarkzerorecomp-tu3')
    dirs = [os.path.join(here, '..', 'assets', 'pdz', 'ASSETS', 'PackageData'),
            os.path.join(docs, '0000000000000000', '4D5307D3', '00000002', '28A3533063A3643DD1AF810EE03BFEA893A7C9D74D',
                         'Pdz', 'Assets', 'PackageData'),
            os.path.join(docs, 'update', 'Pdz', 'Assets', 'PackageData')]
    import tempfile
    pk = nc.Packages(dirs, os.path.join(tempfile.gettempdir(), 'pdz_navcheck'))
    return nc.load_pads(pk.pads_raw('desert'))


PADS = _desert_pads()

P3 = r'\((-?\d+), (-?\d+), (-?\d+)\)'
NAME = r'\(([^)]*?)(?:, team \d)?\)'
pos_re = re.compile(r'([0-9A-F]{8}) ' + NAME + r' at ' + P3)
padpos_re = re.compile(r'(?:pad|spot) ([0-9A-F]{8}) at ' + P3)
prop_re = re.compile(r'property ([0-9A-F]{8}) (?:\(value \d+\) )?at ' + P3)

EV = [
    ('no walking route', re.compile(r'walk check: property ([0-9A-F]{8}) at ' + P3 + r' \(open pad ([0-9A-F]{8}), pad ([0-9A-F]{8})\): no walking route')),
    ("hasn't moved", re.compile(r'([0-9A-F]{8}) ' + NAME + r' at ' + P3 + r" hasn't moved for (\d+) s \(([^)]*)\)")),
    ('line blocked', re.compile(r'attacker ([0-9A-F]{8}) ' + NAME + r' at ' + P3 + r': property ([0-9A-F]{8}) ([\d.]+) m .*: line blocked')),
    ("guard can't get nearer", re.compile(r"guard ([0-9A-F]{8}) " + NAME + r" at " + P3 + r" can't get nearer than ([\d.]+) m to his spot ([0-9A-F]{8}) at " + P3)),
    ('health stayed', re.compile(r'attacker ([0-9A-F]{8}) ' + NAME + r': (\d+) shots in ([\d.]+) s from spot ([0-9A-F]{8}) at property ([0-9A-F]{8}) \(([\d.]+) m\) and its health stayed')),
    ('refused pad', re.compile(r'choke point: refused pad ([0-9A-F]{8}) at ' + P3 + r' for ([0-9A-F]{8}) ' + NAME + r' \(([^)]*)\)')),
    ('BUG across a wall', re.compile(r'BUG: ([0-9A-F]{8}) ' + NAME + r' at ' + P3 + r': his current pad ([0-9A-F]{8}) is across a sandbag wall')),
    ("won't revive", re.compile(r"([0-9A-F]{8}) " + NAME + r" won't revive ([0-9A-F]{8}) " + NAME + r", ([\d.]+) m away: (.*)")),
    ('stalled', re.compile(r'attacker ([0-9A-F]{8}) ' + NAME + r' at ' + P3 + r' stalled ([\d.]+) m from property ([0-9A-F]{8})')),
    ("can't move", re.compile(r"([0-9A-F]{8}) " + NAME + r" can't move: (\d+) s at " + P3)),
    ('pad written off', re.compile(r'pad ([0-9A-F]{8}) at ' + P3 + r' written off for ([0-9A-F]{8})')),
    ('no route (fallback)', re.compile(r'no route to pad ([0-9A-F]{8}) at ' + P3 + r' for ([0-9A-F]{8}) ' + NAME + r' \(([^)]*)\)')),
    ('no shooting spot', re.compile(r'shooting spots for property ([0-9A-F]{8}) \(value (\d+)\) at ' + P3 + r': no shooting spot')),
    ('wall push', re.compile(r'([0-9A-F]{8}) ' + NAME + r' at ' + P3 + r'.*wall push')),
]

PLACES = [
    # name, test(pos) -> bool ; order matters
    ('dump: entrance (west end, round the back wall)', lambda p: -7400 <= p[0] <= -6850 and -5300 <= p[2] <= -4300 and p[1] < 1500),
    ('dump: upper level (walkway over the caches)', lambda p: -7400 <= p[0] <= -5600 and -5200 <= p[2] <= -3600 and p[1] >= 1500),
    ('dump: inside (cache floor)', lambda p: -6850 <= p[0] <= -5600 and -4660 <= p[2] <= -3800 and p[1] < 1500),
    ('dump: south of the back wall', lambda p: -6850 <= p[0] <= -5600 and -5400 <= p[2] < -4660 and p[1] < 1500),
    ('comms tower: pen (inside its sandbags)', lambda p: -15400 <= p[0] <= -14728 and 1487 <= p[2] <= 2150 and p[1] >= 1800),
    ('comms tower: round the pen (walkway outside)', lambda p: -16500 <= p[0] <= -13800 and 0 <= p[2] <= 3000 and p[1] >= 1800),
    ('comms tower: below / approach', lambda p: -16500 <= p[0] <= -13800 and -1500 <= p[2] <= 3000),
    ('crate yard (sandbags 563475 / 563450)', lambda p: math.hypot(p[0] + 11100, p[2] + 5300) <= 1500),
    ('drum pen (spiral bottom)', lambda p: math.hypot(p[0] + 18100, p[2] + 11150) <= 1500),
    ('power node', lambda p: math.hypot(p[0] + 17235, p[2] + 7439) <= 1500),
    ('team 0 base (-15247, 470, -4289)', lambda p: math.hypot(p[0] + 15247, p[2] + 4289) <= 3000),
    ('team 1 base (4707, 790, -11494) and its exit', lambda p: math.hypot(p[0] - 4707, p[2] + 11494) <= 6000),
]


def place_of(p):
    if p is None:
        return 'unknown (no position)'
    for n, t in PLACES:
        if t(p):
            return n
    return 'elsewhere (%d, %d)' % (round(p[0] / 2000) * 20, round(p[2] / 2000) * 20)


_fp = {}


def file_pad(p, within=60):
    if p in _fp:
        return _fp[p]
    _fp[p] = _file_pad(p, within)
    return _fp[p]


def _file_pad(p, within=60):
    """Level-file pad number at a runtime position (pads never move; the run-time ones aren't in the file)."""
    best, bd = None, within
    for k, q in PADS.items():
        d = math.dist(q['pos'], p)
        if d < bd:
            best, bd = k, d
    return best


def logs(paths):
    out = []
    for path in paths:
        t = open(path, encoding='utf-8', errors='replace').read().splitlines()
        start = next((i for i, l in enumerate(t) if 'multi_desert' in l), None)
        if start is None:
            print('%s: no Desert match' % path)
            continue
        end = next((i for i in range(start + 1, len(t)) if re.search(r'multi_(?!desert)[a-z]+', t[i])), len(t))
        out.append((os.path.basename(path), t[start:end]))
    return out


def mine(paths):
    events = []
    for name, lines in logs(paths):
        lastpos, padpos, proppos = {}, {}, {}
        for line in lines:
            if 'pdz_scenario' not in line:
                continue
            ts = line[12:20]
            for m in pos_re.finditer(line):
                lastpos[m.group(1)] = tuple(int(m.group(i)) for i in (3, 4, 5))
            for m in padpos_re.finditer(line):
                padpos[m.group(1)] = tuple(int(m.group(i)) for i in (2, 3, 4))
            for m in prop_re.finditer(line):
                proppos[m.group(1)] = tuple(int(m.group(i)) for i in (2, 3, 4))
            for kind, rx in EV:
                m = rx.search(line)
                if not m:
                    continue
                g = m.groups()
                pos, where, pad, detail = None, None, None, ''
                if kind == 'no walking route':
                    pos = tuple(map(int, g[1:4])); detail = 'piece %s' % g[0]
                elif kind == "hasn't moved":
                    pos = tuple(map(int, g[2:5])); detail = g[6]
                elif kind == 'line blocked':
                    pos = tuple(map(int, g[2:5])); where = proppos.get(g[5]); detail = '%s m to %s' % (g[6], g[5])
                elif kind == "guard can't get nearer":
                    pos = tuple(map(int, g[7:10])); where = tuple(map(int, g[2:5])); pad = g[6]; detail = '%s m short' % g[5]
                elif kind == 'health stayed':
                    pad = g[4]; pos = padpos.get(pad) or lastpos.get(g[0]); detail = '%s shots at %s (%s m)' % (g[2], g[5], g[6])
                elif kind == 'refused pad':
                    pad = g[0]; pos = tuple(map(int, g[1:4])); detail = g[6]
                elif kind == 'BUG across a wall':
                    pos = tuple(map(int, g[2:5])); pad = g[5]
                elif kind == "won't revive":
                    pos = lastpos.get(g[2]); detail = g[5]
                elif kind == 'stalled':
                    pos = tuple(map(int, g[2:5])); where = proppos.get(g[6]); detail = '%s m from the piece' % g[5]
                elif kind == "can't move":
                    pos = tuple(map(int, g[3:6]))
                elif kind == 'pad written off':
                    pad = g[0]; pos = tuple(map(int, g[1:4]))
                elif kind == 'no route (fallback)':
                    pad = g[0]; pos = tuple(map(int, g[1:4])); detail = g[6]
                elif kind == 'no shooting spot':
                    pos = tuple(map(int, g[2:5])); detail = 'piece %s value %s' % (g[0], g[1])
                elif kind == 'wall push':
                    pos = tuple(map(int, g[2:5]))
                events.append(dict(log=name, ts=ts, kind=kind, pos=pos, where=where, pad=pad, detail=detail, line=line))
                break
    for e in events:
        e['place'] = place_of(e['pos'])
        e['fpad'] = file_pad(e['pos']) if e['pos'] and e['kind'] in ('refused pad', 'pad written off', 'no route (fallback)', "guard can't get nearer", 'health stayed') else None
    return events


if __name__ == '__main__':
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    ev = mine(sys.argv[1:])
    names = [n for n, _ in logs(sys.argv[1:])]
    by = defaultdict(Counter)
    pads = defaultdict(Counter)
    for e in ev:
        if e['kind'] == 'line blocked':
            try:
                if float(e['detail'].split(' m')[0]) > 15:
                    continue
            except ValueError:
                pass
        by[e['place']][(e['log'], e['kind'])] += 1
        if e['fpad'] is not None:
            pads[e['place']][e['fpad']] += 1
    for place in sorted(by, key=lambda p: -sum(by[p].values())):
        c = by[place]
        print('\n## %s: %s' % (place, ', '.join('%s %d' % (n, sum(v for (g, k), v in c.items() if g == n)) for n in names)))
        for k in sorted({k for g, k in c}, key=lambda k: -sum(c[(n, k)] for n in names)):
            print('   %-26s' % k + ''.join('%8d' % c[(n, k)] for n in names))
        if pads[place]:
            print('   file pads: ' + ', '.join('%d(%d,%d,%d) x%d' % (k, *[round(x) for x in PADS[k]['pos']], n)
                                            for k, n in pads[place].most_common(10)))
