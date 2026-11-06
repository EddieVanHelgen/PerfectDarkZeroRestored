"""sabotage_navcheck.py: offline check of the pads our Sabotage bot code would hand out, per map.

Two modes.

1. Level data alone (no game, no log):

    python sabotage_navcheck.py level [MAP ...] [--packages DIR ...] [--cache DIR] [--markdown]

  MAP       menu or level names (subway, urban/chinatown, temple, oldtown/africa, desert,
            tower/towersmall, trench, rooftops, plaza, gasplant); none = all 10 Sabotage maps
  DIR       package folders (read-only); default: the repo's assets\\pdz\\ASSETS\\PackageData, the
            installed map pack 1 and the TU3 update under Documents\\perfectdarkzerorecomp-tu3
  --cache   where inflated packages and the package index are kept (default %TEMP%\\pdz_navcheck)
  --markdown  also print the per-map table rows for docs/TU3_BASE.md
  --as-fixed  model WallTick as fixed on 2026-10-26 (Rare's wall direction, sandbag walls of any
            mode filter) instead of the committed one
  --old-flag-rule  model the morning-of-2026-10-26 LevelPad (pad flags 0x02000000 / 0x1 refused; log 307)
  --one-way  a one-way file link's reverse is not walked (Rare's loader: line-of-sight only; log 095,
            Temple's power node stand-in); off by default, as it cuts Tower's top caches off (see load_pads)
  Every run prints a WALK SETS line per map (walk set sizes, pieces reachable / walled / cut off per
  team): compare it before and after any general change.
  --validate LOG  compare the rebuilt walls and WallTick's closed links with a log's own lines
  --via     stage 5's first-leg via pads (TeamFrame, pdz_sabotage_play_team) from each team's spawn
            centroid to every piece over 40 m away: the pad Rare's search finds within 8 m of the
            point halfway along, a quarter of the length (10-20 m) to either side; whether it is in
            the side's walk set, 10 m+ from both ends, and reaches the piece's pad over open links;
            prints a VIA line per team (and a table with --markdown)
  --desert-nav  Desert only: also run with the Desert pad table (pdz_sabotage_desert_nav: trap pads, no-spot pads,
            the dump's firing spot) and print the DESERT NAV check: per piece and team the spot, line, guard spots and a
            revive at the spot; per place the failing handouts and the revives before -> after
  --squads  the squads' target areas (SquadAreas: pieces chained within 12 m flat and 6 m of height)
            per attacking team: every area reachable from the spawn (all / some / none of its pieces'
            pads in the walk set), and the via rule from the spawn centroid to each area's centre on
            both sides (a squad takes one side, the next squad the other); prints a SQUADS line per
            team and adds a squads column to the --markdown table

  Inputs, all from the level data: the pads and links (aid_aipads_multi_<map>; doors joined, see
  load_pads), the Sabotage property (propsetup records with mode filter 19 and a misc_sabot*
  template), the sandbag walls (filter 19, model props_sandbag_wall; plus TU3's filter-0 one on
  Temple, reported, not modelled by the game code) and each team's spawn pads (type 7, group
  colour + 1). Each team is checked as the attackers (the other as defenders).

  The game code's rules, offline (cut_scenarios.cpp): WallTick's link closing (our wall line: free
  end cut to 85%, joined end carried 60 cm on, its direction from the setup yaw only), Rare's
  piece boxes (approximate, with Rare's 55 cm margin), the open pad (Rare's reachable pad search,
  6 / 12 / 20 m, with the behind-a-wall swap), the attack spot (AttackSpot), the walk check
  (ReachCheck: level 0 / level 1 round a free end within kWallEndGap / stand-ins), the shooting
  spots and the guard spots (SpotsFor), each against its side's walkable set. What it can't do:
  Rare's physics line tests (only our sandbag-wall test is applied), the line-of-sight links and
  route-tested links Rare adds at load (82163C68), scripts / doors that start shut.

2. With a play log (the first version):

    python sabotage_navcheck.py MAP PADS_RAW LOG

  MAP       the level folder name (desert, chinatown, towersmall, subway, africa, temple, ...):
            only the log's match on that map is read (the first "sandbag wall ... multi_<MAP>_"
            line, back to the "match starts" line before it and on to the next one)
  PADS_RAW  the inflated aid_aipads_multi_<map> package
  LOG       a game log of a Sabotage match on that map with pdz_scenario_log
  The walkable set per side is then a walk from every pad an attacker (a defender) stood on.
"""
import json
import math
import os
import re
import struct
import sys
import tempfile
import zlib
from collections import Counter, deque

# --- pads -----------------------------------------------------------------------------------

BOTH_WAYS = True  # False (--one-way): a one-way file link's reverse is line-of-sight only, as Rare's loader makes it


def load_pads(path, doors=True):
    """Rare's aid_aipads file (an inflated package). 60-byte records from pad 0, then the u16
    link table. Record: +0 position (3 floats), +12 rotation (3 floats), +24 type (u32: 7 spawn,
    5 objective, 18 / 19 the two sides of a door), +28 first link (u16), +30 link count (u16), +32
    pad number (u16), +52 flags (u32), +56 area (u8), +57 group (s8: a spawn pad's team colour + 1,
    6 the shared room). Pad numbers have gaps (Plaza jumps 2053 -> 2249: the first version stopped
    at a gap of 50 and lost 14 pads and every link). The loader adds a reverse link for every
    one-way one, but as a line-of-sight link (link +15 = 0: the second link pass of 821629D0 stores
    r19 = 0; file links get 1), which Rare's A* and our ReachPads never walk (log 095: Temple's pad
    2234 is entered only up its 1.6 m drop from pad 1960). Taken both ways by default all the same:
    with every one-way link one-way, Tower's 9 top ammo caches are cut off (reached only up 5.2-5.7 m
    one-way links), while the live walk check reaches all 34 (log 096), so something at run time
    (lifts, Rare's route-tested links of the third pass) takes bots up that the data doesn't show.
    `--one-way` (BOTH_WAYS = False) is Rare's loader as written. Doors: the file has no link through a door; each type-19 pad is joined to the
    nearest type-18 pad within 3.5 m (78-340 cm apart on every map, always on two pad islands),
    as an open door (Sabotage opens both bases)."""
    d = open(path, 'rb').read()
    base = next(b for b in range(0x100, 0x4000, 4)
                if all(struct.unpack('>H', d[b + 60 * k + 40:b + 60 * k + 42])[0] == k for k in range(12))) + 8
    pads = {}
    k = 0
    last = -1
    while True:
        r = d[base + 60 * k:base + 60 * k + 60]
        if len(r) < 60:
            break
        pid = struct.unpack('>H', r[32:34])[0]
        if pid <= last:
            break
        first, cnt = struct.unpack('>HH', r[28:32])
        pads[pid] = dict(id=pid, first=first, cnt=cnt, pos=struct.unpack('>3f', r[0:12]),
                         type=struct.unpack('>I', r[24:28])[0], flags=struct.unpack('>I', r[52:56])[0],
                         area=r[56], group=struct.unpack('>b', r[57:58])[0])
        last = pid
        k += 1
    lb = base + 60 * k
    for p in pads.values():
        p['links'] = [struct.unpack('>H', d[lb + 2 * i:lb + 2 * i + 2])[0] for i in range(p['first'], p['first'] + p['cnt'])]
        p['links'] = [b for b in p['links'] if b in pads]
    if BOTH_WAYS:
        for a, p in pads.items():
            for b in list(p['links']):
                if a not in pads[b]['links']:
                    pads[b]['links'].append(a)
    if doors:
        t18 = [k for k, p in pads.items() if p['type'] == 18]
        for a, p in pads.items():
            if p['type'] != 19 or not t18:
                continue
            b = min(t18, key=lambda b: math.dist(p['pos'], pads[b]['pos']))
            if math.dist(p['pos'], pads[b]['pos']) <= 350 and b not in p['links']:
                p['links'].append(b)
                pads[b]['links'].append(a)
                p['door'] = pads[b]['door'] = True
    return pads


def dist(a, b):
    return math.dist(a, b)


def flat(a, b):
    return math.hypot(a[0] - b[0], a[2] - b[2])


def nearest(pads, pos):
    return min(pads, key=lambda k: dist(pads[k]['pos'], pos))


def segments_cross(a, b, c, d):
    def side(p, q, r):
        return (q[0] - p[0]) * (r[1] - p[1]) - (q[1] - p[1]) * (r[0] - p[0])
    return (side(c, d, a) > 0) != (side(c, d, b) > 0) and (side(a, b, c) > 0) != (side(a, b, d) > 0)


# --- packages -------------------------------------------------------------------------------

MAPS = [('Subway', 'subway'), ('Urban', 'chinatown'), ('Temple', 'temple'), ('Old Town', 'africa'),
        ('Desert', 'desert'), ('Tower', 'towersmall'), ('Trench', 'trench'), ('Rooftops', 'rooftops'),
        ('Plaza', 'plaza'), ('Gasplant', 'gasplant')]
ALIASES = {'urban': 'chinatown', 'oldtown': 'africa', 'old town': 'africa', 'tower': 'towersmall', 'gas plant': 'gasplant'}


def inflate(data):
    parts = []
    pos = 0
    while pos < len(data):
        z = zlib.decompressobj()
        try:
            buf = data[pos:]
            while buf:
                parts.append(z.decompress(buf, 16 << 20))
                buf = z.unconsumed_tail
        except zlib.error:
            break
        if not z.unused_data:
            break
        pos = len(data) - len(z.unused_data)
    return b''.join(parts)


BS = re.escape(bytes([0x5c]))
SETUP_PATH = re.compile(b'uberroot' + BS + b'aid' + BS + b'xml' + BS + b'propsetup' + BS + b'multi' + BS + b'([a-z0-9_]+)' +
                        BS + b'((?:[A-Za-z0-9_]|' + BS + b')+)[.]xml' + bytes([0]))
PADS_NAME = re.compile(rb'aid_aipads_multi_([a-z0-9_]+)\x00')


class Packages:
    """Finds each map's pad and setup packages (read-only) and keeps inflated copies in a cache."""

    def __init__(self, dirs, cache):
        self.dirs, self.cache = dirs, cache
        os.makedirs(cache, exist_ok=True)
        ipath = os.path.join(cache, 'index.json')
        self.index = json.load(open(ipath)) if os.path.exists(ipath) else {}
        changed = False
        for d in dirs:
            if not os.path.isdir(d):
                continue
            for n in sorted(os.listdir(d)):
                p = os.path.join(d, n)
                if not n.endswith('.pkg'):
                    continue
                key = '%s|%d' % (p, os.path.getsize(p))
                if key in self.index:
                    continue
                raw = inflate(open(p, 'rb').read())
                pads = sorted({m.group(1).decode() for m in PADS_NAME.finditer(raw)})
                sets = sorted({m.group(1).decode() for m in SETUP_PATH.finditer(raw)})
                self.index[key] = dict(path=p, pads=pads, setups=sets)
                if pads or sets:
                    open(self.raw_path(p), 'wb').write(raw)
                changed = True
        if changed:
            json.dump(self.index, open(ipath, 'w'), indent=1)

    def raw_path(self, p):
        return os.path.join(self.cache, os.path.basename(p) + '.raw')

    def pads_raw(self, level):
        # the map's own package (one name), not the shared index package that lists them all
        best = [v for v in self.index.values() if v['pads'] == [level]]
        return self.raw_path(best[0]['path']) if best else None

    def setup_raws(self, level):
        return [self.raw_path(v['path']) for v in self.index.values() if level in v['setups']]


def read_setups(raws, level):
    """Every placed propsetup record of the map: name, model / template (the first aid_ reference
    after the record's data: the earlier scanner took the next proptemplate and so gave a wall the
    following record's template), mode filter (+49), position (+4) and rotation (+16, degrees)."""
    out = {}
    for raw in raws:
        d = open(raw, 'rb').read()
        for m in SETUP_PATH.finditer(d):
            if m.group(1).decode() != level:
                continue
            h = d.find(bytes([0, 0, 0, 12]), m.end(), m.end() + 32)
            if h < 0:
                continue
            size = struct.unpack('>I', d[h + 4:h + 8])[0]
            data = d[h + 12:h + 12 + size]
            if len(data) < 64:
                continue
            n = d.rfind(b'aid_xml_propsetup_multi_', max(0, m.start() - 400), m.start())
            if n < 0:
                continue
            name = d[n:d.find(b'\x00', n)].decode()
            t = re.search(rb'aid_[a-z0-9_]+', d[h + 12 + size:h + 12 + size + 0x100])
            ref = t.group().decode() if t else ''
            tpl = re.sub(r'^aid_(xml_proptemplate_|darkcharacter_xenon_props_multiplayer_props_|darkcharacter_)', '', ref)
            out[name] = dict(name=name, tpl=tpl, filt=data[49], pos=struct.unpack('>3f', data[4:16]),
                             rot=struct.unpack('>3f', data[16:28]), pkg=os.path.basename(raw))
    return out


# --- the game's rules, offline ---------------------------------------------------------------

WALL_HALF, WALL_TALL, WALL_SETUP_DX = 362.1, 163.9, 362.1  # model half extents (prop +0x108 / +0x10C); centre = setup + (half, tall, 0), unrotated
VALUE = {'misc_sabotcrate': 100, 'misc_sabotdrum': 100, 'misc_sabotammo': 300, 'misc_sabotcommssmall': 500,
         'misc_sabotcomms': 2000, 'misc_sabotpower': 4000}
KIND = {'misc_sabotcrate': 'crate', 'misc_sabotdrum': 'drum', 'misc_sabotammo': 'ammo cache',
        'misc_sabotcommssmall': 'comms (small)', 'misc_sabotcomms': 'comms', 'misc_sabotpower': 'power node'}
# Rare's record box per piece (half extents x/z, height), with its 55 cm margin; ammo caches and the
# power node from the logs' "obstacle boxes larger than the model" lines, the rest estimated.
BOX = {'misc_sabotcrate': (105, 160), 'misc_sabotdrum': (90, 170), 'misc_sabotammo': (100, 140),
       'misc_sabotcommssmall': (95, 160), 'misc_sabotcomms': (130, 400), 'misc_sabotpower': (280, 400)}
PIECE_DY = {'misc_sabotcrate': 50, 'misc_sabotdrum': 51, 'misc_sabotammo': 45}  # runtime origin = setup + half height
K_STANDOFF_MIN, K_STANDOFF_MAX, K_STANDOFF_FLOOR = 450.0, 1500.0, 300.0
K_EYE, K_AIM, K_FIRE_MIN = 150.0, 40.0, 250.0
K_GUARD_RADIUS, K_GUARD_FLOOR, K_GUARD_SEARCH = 1000.0, 200.0, 1200.0
K_WALL_END_GAP, K_WALL_END_SLIDE = 100.0, 150.0
PAD_RADII = (600.0, 1200.0, 2000.0)


def wall_dir(rot, ours):
    """The wall's direction on the floor. Ours (WallTick): (cos yaw, sin yaw) from the setup's +0x14
    alone. Rare's model (the logged transforms: a yaw-90 wall's row 0 is (0, 0, -1); the corners of
    the walls on Tower and Trench meet only this way): (cos yaw, -sin yaw), and a wall turned 180
    about x and z (rotation (180, yaw, 180)) is a yaw of 180 - yaw: (cos yaw, sin yaw) as a line."""
    t = math.radians(rot[1])
    if ours:
        return math.cos(t), math.sin(t)
    # local x through Rz(rz) Ry(yaw) Rx(rx), on the floor: (cos yaw cos rz, -sin yaw)
    ux, uz = math.cos(t) * math.cos(math.radians(rot[2])), -math.sin(t)
    n = math.hypot(ux, uz)
    ux, uz = ux / n, uz / n
    if ux * math.cos(t) + uz * math.sin(t) < 0:  # the old sense along the line (as the fixed WallTick)
        ux, uz = -ux, -uz
    return ux, uz


def build_walls(setups, ours, any_filter=False):
    """WallTick pass 1 + 2: the walls, their ends (free: cut to 85%; within 1.5 m of another wall:
    joined, carried on to the other wall's middle line + 30 cm, or 60 cm for walls end to end), the
    shot / walk line (lo .. hi) and the link line (a joined end at half + 60). any_filter: also the
    sandbag walls placed for every mode (TU3's one on Temple), as the fixed WallTick takes them."""
    found = []
    for r in setups.values():
        if r['tpl'] != 'sandbag_wall' or (r['filt'] != 19 and not any_filter):
            continue
        p = r['pos']
        ux, uz = wall_dir(r['rot'], ours)
        found.append(dict(name=r['name'].split('_')[-1], cx=p[0] + WALL_SETUP_DX, cy=p[1] + WALL_TALL, cz=p[2], ux=ux, uz=uz,
                          half=WALL_HALF, tall=WALL_TALL, lo=-WALL_HALF, hi=WALL_HALF, jlo=False, jhi=False, yaw=r['rot'][1],
                          rot=r['rot']))

    def near_other(i, x, z):
        for j, o in enumerate(found):
            if j == i or abs(o['cy'] - found[i]['cy']) > 300:
                continue
            a = max(-o['half'], min(o['half'], (x - o['cx']) * o['ux'] + (z - o['cz']) * o['uz']))
            if math.hypot(o['cx'] + o['ux'] * a - x, o['cz'] + o['uz'] * a - z) < 150:
                return j
        return -1
    for i, w in enumerate(found):
        for sign in (-1, 1):
            ex, ez = w['cx'] + w['ux'] * w['half'] * sign, w['cz'] + w['uz'] * w['half'] * sign
            j = near_other(i, ex, ez)
            reach = w['half'] * 0.85
            if j >= 0:
                o = found[j]
                denom = -w['ux'] * o['uz'] + w['uz'] * o['ux']
                b0 = -(w['cx'] - o['cx']) * o['uz'] + (w['cz'] - o['cz']) * o['ux']
                a = -b0 / denom if abs(denom) > 0.3 else 0.0
                reach = abs(a) + 30 if abs(denom) > 0.3 and a * sign > 0 else w['half'] + 60
                w['jlo' if sign < 0 else 'jhi'] = True
            w['lo' if sign < 0 else 'hi'] = reach * sign
    for w in found:
        mid, L = (w['lo'] + w['hi']) / 2, (w['hi'] - w['lo']) / 2
        w['mx'], w['mz'], w['L'] = w['cx'] + w['ux'] * mid, w['cz'] + w['uz'] * mid, L
        llo = -(w['half'] + 60) if w['jlo'] else w['lo']
        lhi = w['half'] + 60 if w['jhi'] else w['hi']
        lmid, lL = (llo + lhi) / 2, (lhi - llo) / 2
        w['link'] = ((w['cx'] + w['ux'] * (lmid - lL), w['cz'] + w['uz'] * (lmid - lL)),
                     (w['cx'] + w['ux'] * (lmid + lL), w['cz'] + w['uz'] * (lmid + lL)))
        w['free_lo'], w['free_hi'] = not w['jlo'], not w['jhi']
    return found


def wall_closes(pads, walls):
    """WallTick's links (both ways): crossing a wall's link line on its floor. -> {(a, b): wall}"""
    closed = {}
    for a, p in pads.items():
        for b in p['links']:
            A, B = p['pos'], pads[b]['pos']
            for w in walls:
                if max(A[1], B[1]) < w['cy'] - w['tall'] - 100 or min(A[1], B[1]) > w['cy'] + w['tall']:
                    continue
                if segments_cross((A[0], A[2]), (B[0], B[2]), w['link'][0], w['link'][1]):
                    closed[(a, b)] = closed[(b, a)] = w['name']
                    break
    return closed


def crossing(w, p, q):
    """(a along the wall's middle line, height) where the segment p -> q crosses it, or None."""
    a0 = (p[0] - w['mx']) * w['ux'] + (p[2] - w['mz']) * w['uz']
    b0 = -(p[0] - w['mx']) * w['uz'] + (p[2] - w['mz']) * w['ux']
    a1 = (q[0] - w['mx']) * w['ux'] + (q[2] - w['mz']) * w['uz']
    b1 = -(q[0] - w['mx']) * w['uz'] + (q[2] - w['mz']) * w['ux']
    if (b0 > 0) == (b1 > 0):
        return None
    t = -b0 / (b1 - b0)
    return a0 + (a1 - a0) * t, p[1] + (q[1] - p[1]) * t


def end_gap(w, a):
    return min(a + w['L'] if w['free_lo'] else 1e9, w['L'] - a if w['free_hi'] else 1e9)


def wall_blocks(walls, p, q, walk=False):
    """WallBlocks (shots, eye lines) / WallBlocksWalk (walk=True: within 1.5 m of a free end is round it)."""
    for w in walls:
        if max(p[1], q[1]) < w['cy'] - w['tall'] - 100 or min(p[1], q[1]) > w['cy'] + w['tall']:
            continue
        c = crossing(w, p, q)
        if c is None:
            continue
        a, y = c
        if abs(a) > w['L'] or y > w['cy'] + w['tall'] or y < w['cy'] - w['tall'] - 100:
            continue
        if walk and end_gap(w, a) <= K_WALL_END_SLIDE:
            continue
        return True
    return False


def wall_end_link(walls, p, q, gap):
    """WallEndLink: crosses only walls within `gap` of a free end (and at least one)."""
    crossed = False
    for w in walls:
        if max(p[1], q[1]) < w['cy'] - w['tall'] - 100 or min(p[1], q[1]) > w['cy'] + w['tall']:
            continue
        c = crossing(w, p, q)
        if c is None or abs(c[0]) > w['L']:
            continue
        crossed = True
        if end_gap(w, c[0]) > gap:
            return False
    return crossed


def seg_box(p, q, lo, hi, lift=30):
    p = (p[0], p[1] + lift, p[2])
    q = (q[0], q[1] + lift, q[2])
    t0, t1 = 0.0, 1.0
    for i in range(3):
        dd = q[i] - p[i]
        if abs(dd) < 1e-9:
            if p[i] < lo[i] or p[i] > hi[i]:
                return False
        else:
            a, b = (lo[i] - p[i]) / dd, (hi[i] - p[i]) / dd
            if a > b:
                a, b = b, a
            t0, t1 = max(t0, a), min(t1, b)
            if t0 > t1:
                return False
    return True


def piece_closes(pads, pieces):
    """Rare's obstacle closing for the pieces' boxes: a link through a box, every link of a pad inside
    one (flag 0x01000000 pads and links between two 0x1000 pads are never closed)."""
    closed = set()
    for pc in pieces:
        h, tall = BOX[pc['tpl']]
        x, y, z = pc['setup']
        lo, hi = (x - h, y - 55, z - h), (x + h, y + tall, z + h)
        for a, p in pads.items():
            if math.hypot(p['pos'][0] - x, p['pos'][2] - z) > 1500 or p['flags'] & 0x01000000:
                continue
            inside = all(lo[i] <= p['pos'][i] + (30 if i == 1 else 0) <= hi[i] for i in range(3))
            for b in p['links']:
                if pads[b]['flags'] & 0x01000000 or (p['flags'] & 0x1000 and pads[b]['flags'] & 0x1000):
                    continue
                if inside or seg_box(p['pos'], pads[b]['pos'], lo, hi):
                    closed.add((a, b))
                    closed.add((b, a))
    return closed


class Live:
    """The pad graph as the walk check sees it: piece boxes and our walls closed; `reopened` links
    the walk check gave back round a free end."""

    def __init__(self, pads, walls, wall_closed, piece_closed, excluded=frozenset(), nospot=frozenset()):
        self.pads, self.walls, self.wall_closed, self.piece_closed = pads, walls, wall_closed, piece_closed
        self.reopened = set()
        self.excluded = excluded  # pads our code refuses as walk-set members / spots (--old-flag-rule, Desert trap pads)
        self.nospot = nospot      # walkable pads never handed out as a spot or stand-in (Desert no-spot pads)

    def closed(self, a, b):
        return (a, b) in self.piece_closed or ((a, b) in self.wall_closed and (a, b) not in self.reopened)

    def open_links(self, a):
        return sum(1 for b in self.pads[a]['links'] if not self.closed(a, b))

    def reach(self, starts, level=0, gap=K_WALL_END_GAP):
        prev = {s: None for s in starts}
        q = deque(starts)
        while q:
            a = q.popleft()
            for b in self.pads[a]['links']:
                if b in prev or b in self.excluded:
                    continue
                if self.closed(a, b):
                    ours = (a, b) in self.wall_closed and (a, b) not in self.piece_closed
                    if not (ours and (level >= 2 or (level == 1 and wall_end_link(self.walls, self.pads[a]['pos'], self.pads[b]['pos'], gap)))):
                        continue
                prev[b] = a
                q.append(b)
        return prev

    def pad_within(self, pos, radius, reachable=True):
        best, bd = None, radius
        for k, p in self.pads.items():
            d = dist(p['pos'], pos)
            if d <= bd and k not in self.excluded and (not reachable or self.open_links(k) > 0):
                best, bd = k, d
        return best


def piece_list(setups):
    """Sabotage's property: every filter-19 prop but the walls (Rare's collector takes any with health;
    Subway's five plain wooden crates and Old Town's plain oil drum are live pieces, log 189)."""
    out = []
    for r in setups.values():
        if r['filt'] != 19 or r['tpl'] == 'sandbag_wall':
            continue
        tpl = r['tpl'] if r['tpl'] in VALUE else 'misc_sabotdrum' if 'drum' in r['tpl'] else 'misc_sabotcrate'
        x, y, z = r['pos']
        out.append(dict(name=r['name'].split('_')[-1], tpl=tpl, kind=KIND[tpl], value=VALUE[tpl],
                        soft=VALUE[tpl] == 100, setup=(x, y, z), pos=(x, y + PIECE_DY.get(tpl, 0), z)))
    return sorted(out, key=lambda p: (-p['value'], p['pos']))


def open_pad(live, pc):
    """Rare's reachable pad nearest the piece (6 / 12 / 20 m), swapped when its eye line to the piece
    crosses one of our walls (8 directions x 2 / 4 / 6 / 9 m, Rare's reachable pad within 2.5 m)."""
    found = None
    for r in PAD_RADII:
        found = live.pad_within(pc['pos'], r)
        if found is not None:
            break
    if found is None:
        return None, None
    aim = (pc['pos'][0], pc['pos'][1] + K_AIM, pc['pos'][2])
    eye = lambda k: (live.pads[k]['pos'][0], live.pads[k]['pos'][1] + K_EYE, live.pads[k]['pos'][2])
    swapped = None
    if live.walls and wall_blocks(live.walls, eye(found), aim):
        best, bd = None, 0
        for r in (200, 400, 600, 900):
            for k in range(8):
                v = (aim[0] + r * math.cos(k * 0.785398), aim[1] - K_AIM, aim[2] + r * math.sin(k * 0.785398))
                pad = live.pad_within(v, 250)
                if pad is None or wall_blocks(live.walls, eye(pad), aim) or live.open_links(pad) < 1:
                    continue
                d = dist(live.pads[pad]['pos'], pc['pos'])
                if best is None or d < bd:
                    best, bd = pad, d
        swapped = found
        if best is not None:
            found = best
    return found, swapped


def attack_spot(live, pc, nearest_pad):
    """AttackSpot: from the open pad and Rare's reachable pads 6 / 10 m round the piece, along open
    links; 4.5-15 m out on the flat, within 3 m of the piece's height, an open link, a clear shot
    (here: our wall test only); the piece's own level (-0.6 .. +1.5 m) first, the nearest."""
    px, py, pz = pc['pos']
    queue = [nearest_pad] if nearest_pad is not None else []
    for r in (600, 1000):
        for k in range(8):
            a = k * 0.78539816
            pad = live.pad_within((px + r * math.cos(a), py + 50, pz + r * math.sin(a)), 400)
            if pad is not None:
                queue.append(pad)
    seen = set(queue)
    best = low = None
    bd = ld = 0
    aim = (px, py + K_AIM, pz)
    i = 0
    while i < len(queue) and len(queue) < 512:
        pad = queue[i]
        i += 1
        P = live.pads[pad]['pos']
        for b in live.pads[pad]['links']:
            Q = live.pads[b]['pos']
            if not live.closed(pad, b) and flat(Q, pc['pos']) <= K_STANDOFF_MAX + 300 and abs(Q[1] - py) <= 2 * K_STANDOFF_FLOOR and b not in seen:
                seen.add(b)
                queue.append(b)
        d, dy = flat(P, pc['pos']), P[1] - py
        level = -60 <= dy <= 150
        if d < K_STANDOFF_MIN or d > K_STANDOFF_MAX or abs(dy) > K_STANDOFF_FLOOR or live.open_links(pad) == 0 or pad in live.nospot:
            continue
        if (best is not None and d >= bd) if level else (best is not None or (low is not None and d >= ld)):
            continue
        if not wall_blocks(live.walls, (P[0], P[1] + K_EYE, P[2]), aim):
            if level:
                best, bd = pad, d
            else:
                low, ld = pad, d
    return best if best is not None else low


def walkable_near(live, pc, walk):
    """The walk check's stand-in: the nearest walkable pad within 10 m (flat), 2 m below to 3 m above
    the piece, with a line (here: our wall test)."""
    best, bd = None, 1000
    aim = (pc['pos'][0], pc['pos'][1] + K_AIM, pc['pos'][2])
    for k in walk:
        P = live.pads[k]['pos']
        d, dy = flat(P, pc['pos']), P[1] - pc['pos'][1]
        if d >= bd or dy < -200 or dy > 300 or k in live.nospot:
            continue
        if not wall_blocks(live.walls, (P[0], P[1] + 165, P[2]), aim):
            best, bd = k, d
    return best


def shoot_spots(live, pc, walk):
    """The walk check's shooting spots (hard pieces but power nodes): walkable pads 2.5-30 m out, 6 m
    below to 15 m above, 1.5 m apart, at most 6 of the first 40 tested; not steeper than 35 deg down,
    a line from the eye and the gun (here: our wall test)."""
    px, py, pz = pc['pos']
    cands = sorted((flat(live.pads[k]['pos'], pc['pos']), k) for k in walk
                   if K_FIRE_MIN <= flat(live.pads[k]['pos'], pc['pos']) <= 3000 and -600 <= live.pads[k]['pos'][1] - py <= 1500
                   and k not in live.nospot)
    spots, tested, rej = [], 0, Counter()
    aim = (px, py + K_AIM, pz)
    for d, k in cands:
        if len(spots) >= 6 or tested >= 40:
            break
        P = live.pads[k]['pos']
        if any(dist(live.pads[o]['pos'], P) < 150 for o in spots):
            continue
        tested += 1
        gun = (P[0], P[1] + 130, P[2])
        if math.degrees(math.atan2(gun[1] - aim[1], max(d, 1))) > 35:
            rej['steep'] += 1
        elif wall_blocks(live.walls, (P[0], P[1] + 165, P[2]), aim) or wall_blocks(live.walls, gun, aim):
            rej['wall'] += 1
        else:
            spots.append(k)
    return spots, rej


def guard_spots(live, pc, use_pad, dset, dump_fire=None, game_seeds=False):
    """SpotsFor: from the piece's pad as g_property holds it (the attack spot or the walk check's
    stand-in, NOT the open pad) and Rare's reachable pads 6 m round the piece; its floor is that pad's
    height; pads within 10 m flat with 2+ open links on that floor (+-2 m), walkable for the
    defenders, a line past our walls.
    dump_fire (Desert nav, log 313): a dump cache's search centres on the entrance pad, which counts with
    one open link (the caches' boxes close its other four). game_seeds (Desert nav): as the game, Rare's
    pad search answers the nearest reachable pad, trap pads too, and LevelPad then drops a trap pad, and
    the link walk never enters one."""
    if use_pad is None:
        return [], 0, 0, None
    px, py, pz = pc['pos']
    centre = live.pads[dump_fire]['pos'] if dump_fire is not None else pc['pos']
    cx, cz = centre[0], centre[2]
    floor = live.pads[use_pad]['pos'][1]
    queue = [use_pad]
    for k in range(8):
        a = k * 0.78539816
        at = (cx + 600 * math.cos(a), floor + 50, cz + 600 * math.sin(a))
        if game_seeds:
            pad, bd = None, 400
            for q, p in live.pads.items():
                d = dist(p['pos'], at)
                if d <= bd and live.open_links(q) > 0:
                    pad, bd = q, d
            if pad in live.excluded:
                pad = None
        else:
            pad = live.pad_within(at, 400)
        if pad is not None:
            queue.append(pad)
    seen = set(queue)
    spots, unwalk, walled = [], 0, 0
    aim = (px, py + K_AIM, pz)
    i = 0
    while i < len(queue) and len(queue) < 512:
        pad = queue[i]
        i += 1
        opn = 0
        for b in live.pads[pad]['links']:
            if live.closed(pad, b):
                continue
            opn += 1
            Q = live.pads[b]['pos']
            if flat(Q, centre) <= K_GUARD_SEARCH and abs(Q[1] - floor) <= 2 * K_GUARD_FLOOR and b not in seen and \
                    not (game_seeds and b in live.excluded):
                seen.add(b)
                queue.append(b)
        P = live.pads[pad]['pos']
        if (opn >= 2 or (pad == dump_fire and opn >= 1)) and flat(P, centre) <= K_GUARD_RADIUS and \
                abs(P[1] - floor) <= K_GUARD_FLOOR:
            if pad not in dset or live.pads[pad]['flags'] & 0x200000 or pad in live.nospot:
                unwalk += 1
                continue
            if wall_blocks(live.walls, (P[0], P[1] + 150, P[2]), aim):
                walled += 1
                continue
            spots.append(pad)
    return spots, unwalk, walled, floor


def fmt_pad(pads, k):
    return '%d(%d,%d,%d)' % (k, *[round(c) for c in pads[k]['pos']]) if k is not None else '-'


def check_side(pads, walls, wall_closed, piece_closed, pieces, att_team, def_team, out, fixed=False, excluded=frozenset(),
               desert=None):
    """One side attacking: the walk check, every piece's pads, the self-test. Returns a summary dict. desert: the
    Desert pad table (DESERT_NAV, pdz_sabotage_desert_nav): trap pads out of the walk sets and every spot, no-spot pads
    out of every spot, the ammo caches (all in the dump) on the tested firing spot."""
    if desert:
        excluded = frozenset(excluded) | frozenset(desert['trap'])
    live = Live(pads, walls, wall_closed, piece_closed, excluded, frozenset(desert['no_spot']) if desert else frozenset())
    starts = [k for k, p in pads.items() if p['type'] == 7 and p['group'] == att_team + 1]
    dstarts = [k for k, p in pads.items() if p['type'] == 7 and p['group'] == def_team + 1]
    # the pads every piece gets before the walk check (looked up at the property read)
    info = {}
    for pc in pieces:
        op, swapped = open_pad(live, pc)
        spot = attack_spot(live, pc, op) if op is not None else None
        info[pc['name']] = dict(open=op, swapped=swapped, pad=spot if spot is not None else op, spot=spot)
        if desert and pc['kind'] == 'ammo cache':
            info[pc['name']] = dict(open=desert['fire'], swapped=None, pad=desert['fire'], spot=desert['fire'])
    walk = live.reach(starts)
    res = dict(ok=0, sub=0, reopened=0, walled=[], sealed=[], cut=[], reopened_links=[], notes=[])
    reach_pad, reach_open = {}, {}
    for pc in pieces:
        i = info[pc['name']]
        pad_ok = i['pad'] is not None and i['pad'] in walk
        open_ok = i['open'] is not None and i['open'] in walk
        if pad_ok and open_ok:
            res['ok'] += 1
            continue
        if fixed and (pad_ok or open_ok):
            # the fixed walk check: the pad that can't be walked to gets the way round a free end first
            ends = live.reach(starts, 1)
            want = i['open'] if pad_ok else i['pad']
            if want in ends:
                k, opened = want, []
                while ends.get(k) is not None:
                    a = ends[k]
                    if (a, k) in wall_closed and (a, k) not in live.reopened:
                        live.reopened.update({(a, k), (k, a)})
                        opened.append((a, k))
                    k = a
                res['reopened'] += 1
                res['reopened_links'] += [(pc['name'], a, b) for a, b in opened]
                walk = live.reach(starts)
                pad_ok, open_ok = i['pad'] in walk, i['open'] in walk
                if pad_ok and open_ok:
                    continue
        if pad_ok or open_ok:
            sub = walkable_near(live, pc, walk)
            if sub is not None:
                (reach_open if pad_ok else reach_pad)[pc['name']] = sub
            res['ok'] += 1
            res['sub'] += 1
            res['notes'].append('%s %s: its %s pad %s not walkable; stand-in %s' % (
                pc['kind'], pc['name'], 'open' if pad_ok else 'shooting', fmt_pad(pads, i['open'] if pad_ok else i['pad']),
                fmt_pad(pads, sub) if sub is not None else 'NONE within 10 m with a line'))
            continue
        ends = live.reach(starts, 1)
        target = i['open'] if i['open'] in ends else i['pad'] if i['pad'] in ends else None
        if target is not None:
            k = target
            opened = []
            while ends.get(k) is not None:
                a = ends[k]
                if (a, k) in wall_closed and (a, k) not in live.reopened:
                    live.reopened.update({(a, k), (k, a)})
                    opened.append((a, k))
                k = a
            res['reopened'] += 1
            res['reopened_links'] += [(pc['name'], a, b) for a, b in opened]
            walk = live.reach(starts)
            continue
        walled = live.reach(starts, 2)
        is_walled = (i['open'] in walled) or (i['pad'] in walled)
        sub = walkable_near(live, pc, walk)
        if sub is not None:
            reach_pad[pc['name']] = reach_open[pc['name']] = sub
        (res['walled'] if is_walled else res['sealed']).append(pc['name'])
        res['notes'].append('%s %s at (%d, %d, %d): no walking route to its pads %s (open %s, pad %s); stand-in %s' % (
            pc['kind'], pc['name'], *[round(c) for c in pc['pos']],
            'except through the middle of a sandbag wall' if is_walled else 'at all', fmt_pad(pads, i['open']),
            fmt_pad(pads, i['pad']), fmt_pad(pads, sub) if sub is not None else 'NONE: cut off'))
        if sub is None:
            res['cut'].append(pc['name'])
    dset = set(live.reach(dstarts))
    aset = set(walk)
    # the self-test, per piece
    bad = []
    rows = []
    for pc in pieces:
        i = info[pc['name']]
        use_pad = reach_pad.get(pc['name'], i['pad'])
        use_open = reach_open.get(pc['name'], i['open'])
        sh, rej = (shoot_spots(live, pc, aset) if not pc['soft'] and pc['value'] != 4000 else ([], Counter()))
        if desert and pc['kind'] == 'ammo cache' and desert['fire'] in aset:
            sh, rej = [desert['fire']], Counter()
        dump = desert is not None and pc['kind'] == 'ammo cache' and desert.get('dump_guard', True)
        gs, gun, gwall, gfloor = guard_spots(live, pc, use_pad, dset, desert['fire'] if dump else None, desert is not None)
        problems = []
        for what, k, s in (('attack spot', use_pad, aset), ('open pad', use_open, aset)):
            if k is None:
                problems.append('no %s' % what)
            elif k not in s or pads[k]['flags'] & 0x200000:
                problems.append('%s %s unwalkable' % (what, fmt_pad(pads, k)))
                bad.append((pc['name'], what, k))
        if not pc['soft'] and pc['value'] != 4000 and not sh:
            problems.append('no shooting spot (rejected: %s)' % (dict(rej) or 'no candidates'))
        if not gs:
            problems.append('no guard spot (%d not walkable for the defenders, %d behind a wall)' % (gun, gwall))
        if gfloor is not None and abs(gfloor - pc['pos'][1]) > 200:
            problems.append('guard floor %.1f m from the piece (spots taken round the attack spot %s)' % (
                (gfloor - pc['pos'][1]) / 100, fmt_pad(pads, use_pad)))
        rows.append(dict(name=pc['name'], kind=pc['kind'], value=pc['value'], pos=pc['pos'], open=use_open, pad=use_pad,
                         raw_open=i['open'], raw_pad=i['pad'],
                         swapped=i['swapped'], shoot=len(sh), guard=len(gs), problems=problems, shoot_pads=sh, guard_pads=gs))
    out['att%d' % att_team] = dict(res=res, rows=rows, aset=len(aset), dset=len(dset), starts=len(starts), dstarts=len(dstarts),
                                   bad=bad, live=live, walk=aset, defend=dset)
    return out['att%d' % att_team]


def wall_report(pads, ours, true, wall_closed, sides):
    """Per wall: the links WallTick closes, those within 1.5 m of a free end, and where our line (the
    setup yaw alone) differs from the wall Rare places (the rotation rule)."""
    lines = []
    true_closed = wall_closes(pads, true)
    by_wall = Counter(wall_closed.values())
    for w, t in zip(ours, true):
        ang = math.degrees(math.acos(max(-1.0, min(1.0, abs(w['ux'] * t['ux'] + w['uz'] * t['uz'])))))
        near_end = []
        for (a, b), name in wall_closed.items():
            if name != w['name'] or a > b:
                continue
            c = crossing(w, pads[a]['pos'], pads[b]['pos'])
            if c is None:
                continue
            g = end_gap(w, c[0])
            if g <= K_WALL_END_SLIDE:
                near_end.append((round(g), a, b))
        wrong = sorted({tuple(sorted(k)) for k, n in wall_closed.items() if n == w['name'] and k not in true_closed})
        missed = sorted({tuple(sorted(k)) for k, n in true_closed.items() if n == t['name'] and k not in wall_closed})
        lines.append(dict(name=w['name'], centre=(round(w['cx']), round(w['cy']), round(w['cz'])), yaw=w['yaw'], rot=w['rot'],
                          ends='%s/%s' % ('free' if w['free_lo'] else 'joined', 'free' if w['free_hi'] else 'joined'),
                          closes=by_wall[w['name']] // 2, near_end=sorted(near_end), angle=ang, wrong=wrong, missed=missed))
    return lines


def via_report(pads, side, pieces, att_team):
    """Stage 5's via rule (cut_scenarios.cpp TeamFrame) for one attacking side, from the spawn centroid
    (in the game: where the bot stands when he takes his first piece). Returns a dict of counts and notes."""
    live, walk = side['live'], side['walk']
    starts = [k for k, p in pads.items() if p['type'] == 7 and p['group'] == att_team + 1]
    if not starts:
        return dict(far=0, both=0, one=0, none=0, reach=0, chosen=0, worst=1.0, notes=['no spawn pads'])
    s = [sum(pads[k]['pos'][i] for k in starts) / len(starts) for i in range(3)]
    rows = {r['name']: r for r in side['rows']}
    res = dict(far=0, both=0, one=0, none=0, reach=0, chosen=0, worst=1.0, notes=[])
    for pc in pieces:
        g = pc['pos']
        dx, dz = g[0] - s[0], g[2] - s[2]
        length = math.hypot(dx, dz)
        if length <= 4000:
            continue
        res['far'] += 1
        off = min(max(length * 0.25, 1000), 2000)
        target = rows[pc['name']]['pad'] if rows[pc['name']]['pad'] is not None else rows[pc['name']]['open']
        good = []
        for sd in (1, -1):
            pt = (s[0] + dx * 0.5 - dz / length * off * sd, (s[1] + g[1]) * 0.5, s[2] + dz * 0.5 + dx / length * off * sd)
            k = live.pad_within(pt, 800, reachable=False)
            if k is None or k not in walk or dist(pads[k]['pos'], s) <= 1000 or dist(pads[k]['pos'], g) <= 1000:
                continue
            reaches = target is not None and target in live.reach([k])
            good.append((k, reaches))
        res['both' if len(good) == 2 else 'one' if good else 'none'] += 1
        if not good:
            res['notes'].append('%s %s (%.0f m): no via pad either side: straight' % (pc['kind'], pc['name'], length / 100))
            continue
        for k, reaches in good:
            res['chosen'] += 1
            res['reach'] += reaches
            if not reaches:
                res['notes'].append('%s %s: via pad %s does NOT reach its pad %s' % (pc['kind'], pc['name'], fmt_pad(pads, k),
                                                                                    fmt_pad(pads, target)))
            vp = pads[k]['pos']
            ratio = (flat(s, vp) + flat(vp, g)) / max(flat(s, g), 1.0)
            res['worst'] = max(res['worst'], ratio)
    return res


def squad_areas(pieces, rows):
    """SquadAreas offline: pieces with a pad, chained within 12 m flat and 6 m of height."""
    ps = [pc for pc in pieces if rows[pc['name']]['pad'] is not None or rows[pc['name']]['open'] is not None]
    parent = list(range(len(ps)))

    def root(i):
        while parent[i] != i:
            parent[i] = parent[parent[i]]
            i = parent[i]
        return i
    for i in range(len(ps)):
        for j in range(i + 1, len(ps)):
            a, b = ps[i]['pos'], ps[j]['pos']
            if (a[0] - b[0]) ** 2 + (a[2] - b[2]) ** 2 < 1200 ** 2 and abs(a[1] - b[1]) < 600:
                parent[root(i)] = root(j)
    groups = {}
    for i, pc in enumerate(ps):
        groups.setdefault(root(i), []).append(pc)
    return list(groups.values())


def squads_report(pads, side, pieces, att_team):
    """Per attacking team: each squad area's reachability and the via rule to its centre on both sides."""
    live, walk = side['live'], side['walk']
    rows = {r['name']: r for r in side['rows']}
    starts = [k for k, p in pads.items() if p['type'] == 7 and p['group'] == att_team + 1]
    res = dict(areas=0, full=0, some=0, none=0, node=0, far=0, both=0, one=0, nvia=0, reach=0, chosen=0, notes=[])
    if not starts:
        return res
    s = [sum(pads[k]['pos'][i] for k in starts) / len(starts) for i in range(3)]
    for area in squad_areas(pieces, rows):
        res['areas'] += 1
        res['node'] += any(pc['value'] == 4000 for pc in area)
        pads_ok = [pc for pc in area if (rows[pc['name']]['pad'] in walk) or (rows[pc['name']]['open'] in walk)]
        key = 'full' if len(pads_ok) == len(area) else 'some' if pads_ok else 'none'
        res[key] += 1
        g = [sum(pc['pos'][i] for pc in area) / len(area) for i in range(3)]
        label = '%d piece(s) round (%d, %d, %d)%s' % (len(area), *[round(c) for c in g],
                                                     ' with the node' if any(pc['value'] == 4000 for pc in area) else '')
        if key != 'full':
            res['notes'].append('area of %s: %d of %d pieces walkable' % (label, len(pads_ok), len(area)))
        dx, dz = g[0] - s[0], g[2] - s[2]
        length = math.hypot(dx, dz)
        if length <= 4000 or not pads_ok:
            continue
        res['far'] += 1
        off = min(max(length * 0.25, 1000), 2000)
        targets = {rows[pc['name']]['pad'] for pc in pads_ok} | {rows[pc['name']]['open'] for pc in pads_ok}
        targets.discard(None)
        good = []
        for sd in (1, -1):
            pt = (s[0] + dx * 0.5 - dz / length * off * sd, (s[1] + g[1]) * 0.5, s[2] + dz * 0.5 + dx / length * off * sd)
            k = live.pad_within(pt, 800, reachable=False)
            if k is None or k not in walk or dist(pads[k]['pos'], s) <= 1000 or dist(pads[k]['pos'], g) <= 1000:
                continue
            good.append((k, bool(targets & set(live.reach([k])))))
        res['both' if len(good) == 2 else 'one' if good else 'nvia'] += 1
        for k, ok in good:
            res['chosen'] += 1
            res['reach'] += ok
            if not ok:
                res['notes'].append('area of %s: via pad %s reaches none of its pads' % (label, fmt_pad(pads, k)))
    return res


# --- Desert pathfinding (pdz_sabotage_desert_nav, 2026-10-27) ----------------------------------

# The Desert pad table of cut_scenarios.cpp (kDesertPads), by level-file pad number. What the level data
# can't show and the play logs do (docs/TU3_BASE.md "Desert pathfinding", table 1): trap pads are never in a
# walk set or a spot (the dump's cache floor is sealed by the caches' boxes and the back wall; the power node's
# catwalk joins the ground only by an 8.2 m drop; the comms pen's inside is closed by its east wall and the
# comms tower's box), no-spot pads are walkable but never a spot (behind the dump's back wall), and the ten
# ammo caches (all in the dump) are shot and planted from the entrance pad, the one pad logs 286-300 show
# hits on every cache from.
DESERT_NAV = dict(
    trap={3493: 'dump cache floor', 3494: 'dump cache floor', 3495: 'dump cache floor',
          4118: 'power node catwalk', 4119: 'power node catwalk', 4194: 'power node catwalk',
          1807: 'comms pen inside', 2024: 'comms pen inside'},
    no_spot={747: "behind the dump's back wall", 749: "behind the dump's back wall"},
    fire=232)
# Rare's revive (821C5528 -> sub_8214A360): under 4 m, heights within 1.5 m (constant 82D35C30); our hook widens
# it to 6 m flat / 2.5 m: a reviver needs a walkable pad that close to the body, not across a sandbag wall.
REVIVE_FLAT, REVIVE_HEIGHT = 600.0, 250.0
DESERT_PLACES = [
    ('dump: entrance', lambda p: -7400 <= p[0] <= -6850 and -5300 <= p[2] <= -4300 and p[1] < 1500),
    ('dump: upper level', lambda p: -7400 <= p[0] <= -5600 and -5200 <= p[2] <= -3600 and p[1] >= 1500),
    ('dump: cache floor', lambda p: -6850 <= p[0] <= -5600 and -4660 <= p[2] <= -3800 and p[1] < 1500),
    ('dump: south of the back wall', lambda p: -6850 <= p[0] <= -5600 and -5400 <= p[2] < -4660 and p[1] < 1500),
    ('comms pen (inside)', lambda p: -15400 <= p[0] <= -14728 and 1487 <= p[2] <= 2150 and p[1] >= 1800),
    ('comms walkway (round the pen)', lambda p: -16500 <= p[0] <= -13800 and 0 <= p[2] <= 3000 and p[1] >= 1800),
    ('crate yard', lambda p: math.hypot(p[0] + 11100, p[2] + 5300) <= 1500),
    ('drum pen (spiral bottom)', lambda p: math.hypot(p[0] + 18100, p[2] + 11150) <= 1500),
    ('power node', lambda p: math.hypot(p[0] + 17235, p[2] + 7439) <= 1500),
]


def desert_place(p):
    return next((n for n, t in DESERT_PLACES if t(p)), None)


def revive_stand(pads, walls, walk, body, flat_max=REVIVE_FLAT, height=REVIVE_HEIGHT, wall_test=True):
    """The walkable pad nearest the body within Rare's (widened) revive reach, not across a sandbag wall."""
    best, bd = None, flat_max
    for k in walk:
        P = pads[k]['pos']
        d = flat(P, body)
        if d < bd and abs(P[1] - body[1]) < height and not (wall_test and wall_blocks(walls, (P[0], P[1] + 100, P[2]), body)):
            best, bd = k, d
    return best


def desert_report(pads, walls, pieces, before, after):
    """The Desert check (pdz_sabotage_desert_nav): per piece and team the attackers' spot, its line, the guards'
    spots and a revive at the spot; per table-1 place the pads handed out that the logs show failing (trap pads,
    no line, unwalkable) and the revives, before and after the table."""
    trap, nospot, fire = DESERT_NAV['trap'], DESERT_NAV['no_spot'], DESERT_NAV['fire']
    print('  DESERT NAV: trap pads %s; no-spot pads %s; dump firing spot %s' % (
        ', '.join(fmt_pad(pads, k) for k in trap), ', '.join(fmt_pad(pads, k) for k in nospot), fmt_pad(pads, fire)))
    print('  per piece (with the table), team 0 attacking | team 1 attacking: spot (attack spot / open pad walkable) / '
          'line (our wall test; dump: the tested pad) / guard spots (walkable for the defenders) / revive at the spot')
    totals = Counter()
    for pc in sorted(pieces, key=lambda p: (p['kind'], p['pos'])):
        cells = []
        for att in (0, 1):
            side = after['att%d' % att]
            r = next(x for x in side['rows'] if x['name'] == pc['name'])
            aset = side['walk']
            spot_ok = r['pad'] in aset and r['open'] in aset and r['pad'] not in trap and r['open'] not in trap
            if pc['kind'] == 'ammo cache':
                line = 'tested' if r['pad'] == fire else 'NO'
            elif pc['value'] == 4000:
                line = 'plant'
            else:
                P = pads[r['pad']]['pos'] if r['pad'] is not None else None
                aim = (pc['pos'][0], pc['pos'][1] + K_AIM, pc['pos'][2])
                line = 'yes' if P and not wall_blocks(walls, (P[0], P[1] + K_EYE, P[2]), aim) else 'NO'
            rv = revive_stand(pads, walls, aset, pads[r['pad']]['pos']) if r['pad'] is not None else None
            totals['spot'] += spot_ok
            totals['line'] += line != 'NO'
            totals['guard'] += r['guard'] > 0
            totals['revive'] += rv is not None
            totals['n'] += 1
            cells.append('%s %s / %s / %d / %s' % ('yes' if spot_ok else 'NO', fmt_pad(pads, r['pad']), line, r['guard'],
                                                   'yes' if rv is not None else 'NO'))
        print('     %-10s %-17s %s | %s' % (pc['kind'], pc['name'], cells[0], cells[1]))
    print('  DESERT NAV totals (both teams, %d piece-sides): spot walkable %d, line %d, guard spot %d, revive at the spot %d' % (
        totals['n'], totals['spot'], totals['line'], totals['guard'], totals['revive']))

    def bad_handouts(side):
        out = Counter()
        for r in side['rows']:
            items = [('attack spot', r['pad']), ('open pad', r['open'])] + [('shooting spot', k) for k in r['shoot_pads']] + \
                    [('guard spot', k) for k in r['guard_pads']]
            for what, k in items:
                if k is None:
                    continue
                place = desert_place(pads[k]['pos'])
                if not place:
                    continue
                if k in trap:
                    why = "trap pad (refused / no route / can't get nearer in the logs)"
                elif k in nospot and what != 'open pad':
                    why = 'no line (behind the back wall)'
                elif k not in (side['defend'] if what == 'guard spot' else side['walk']):
                    why = 'not walkable'
                elif what == 'shooting spot' and r['kind'] == 'ammo cache' and k != fire:
                    why = 'untested shot at a dump cache'
                else:
                    continue
                out[(place, what, why)] += 1
        return out
    b = bad_handouts(before['att0']) + bad_handouts(before['att1'])
    a = bad_handouts(after['att0']) + bad_handouts(after['att1'])
    print('  DESERT NAV handouts by place (pads our code hands out that the logs show failing; both teams): before -> after')
    for key in sorted(set(b) | set(a)):
        print('     %-30s %-14s %-60s %3d -> %d' % (key[0], key[1], key[2], b[key], a[key]))
    if not (set(b) | set(a)):
        print('     none')
    # revives: a teammate down on any pad of a place (team 0 attacking: its walk set)
    print('  DESERT NAV revives by place (a teammate down on each pad of the place): before = the old rule (a pad in the '
          'walk set within 3 m / 2 m: the reviver is sent to it), after = a stand pad within 6 m flat / 2.5 m, not across '
          'a wall, else written off 20 s')
    rows = Counter()
    for k, p in pads.items():
        place = desert_place(p['pos'])
        if not place:
            continue
        body = (p['pos'][0], p['pos'][1] + 30, p['pos'][2])
        old = revive_stand(pads, walls, before['att0']['walk'], body, 300.0, 200.0, wall_test=False)
        new = revive_stand(pads, walls, after['att0']['walk'], body)
        rows[(place, 'pads')] += 1
        rows[(place, 'trap')] += old is not None and old in trap
        rows[(place, 'refused')] += old is None
        rows[(place, 'stand')] += new is not None
        rows[(place, 'off')] += new is None
    for place, _ in DESERT_PLACES:
        if rows[(place, 'pads')]:
            print('     %-30s pads %3d: before sent to a trap pad %d, refused %d; after stand pad %d, written off %d' % (
                place, rows[(place, 'pads')], rows[(place, 'trap')], rows[(place, 'refused')], rows[(place, 'stand')],
                rows[(place, 'off')]))


def run_level(level, menu, pk, md_rows, verbose=True, fixed=False, old_flag_rule=False, via_rows=None, squad_rows=None,
              desert_nav=False):
    praw = pk.pads_raw(level)
    if not praw:
        print('== %s: no aid_aipads_multi_%s package found' % (menu, level))
        return None
    pads = load_pads(praw)
    setups = read_setups(pk.setup_raws(level), level)
    pieces = piece_list(setups)
    # fixed: the uncommitted WallTick (Rare's wall direction, walls of any mode filter)
    ours = build_walls(setups, ours=not fixed, any_filter=fixed)
    true = build_walls(setups, ours=False, any_filter=fixed)
    other_walls = [r for r in setups.values() if r['tpl'] == 'sandbag_wall' and r['filt'] != 19 and not fixed]
    wall_closed = wall_closes(pads, ours)
    piece_closed = piece_closes(pads, pieces)
    doors = sum(1 for p in pads.values() if p.get('door')) // 2
    print('== %s (%s): %d pads, %d links, %d door pairs joined; %d pieces (%s); %d sandbag walls (WallTick closes %d links); '
          'piece boxes close %d links' % (
              menu, level, len(pads), sum(len(p['links']) for p in pads.values()) // 2, doors, len(pieces),
              ', '.join('%d %s' % (n, k) for k, n in Counter(p['kind'] for p in pieces).most_common()), len(ours),
              len(wall_closed) // 2, len(piece_closed) // 2))
    for r in other_walls:
        print('  NOTE: sandbag wall %s with mode filter %d (every mode; package %s) at (%d, %d, %d), rotation %s: not a '
              'filter-19 wall, so WallTick never models it' % (r['name'].split('_')[-1], r['filt'], r['pkg'],
                                                               *[round(c) for c in r['pos']], [round(x) for x in r['rot']]))
    # --old-flag-rule: the 2026-10-26 morning LevelPad (flags 0x02000000 / 0x1 refused). Log 307: level pads
    # carry 0x00006001 at run time; modelled as 0x1 on every pad with the file's 0x200 (all MP pads).
    excluded = frozenset(k for k, p in pads.items() if p['flags'] & 0x200) if old_flag_rule else frozenset()
    out = {}
    desert = DESERT_NAV if desert_nav and level == 'desert' else None
    if desert_nav and level == 'desert':
        before = {}
        for att in (0, 1):
            check_side(pads, ours, wall_closed, piece_closed, pieces, att, 1 - att, before, fixed, excluded)
    for att in (0, 1):
        check_side(pads, ours, wall_closed, piece_closed, pieces, att, 1 - att, out, fixed, excluded, desert)
    if desert:
        desert_report(pads, ours, pieces, before, out)
    walls = wall_report(pads, ours, true, wall_closed, out)
    # the walk-set check (2026-10-26, log 307): set sizes and reachable pieces per team, every run
    ws = []
    for att in (0, 1):
        s_ = out['att%d' % att]
        r_ = s_['res']
        ws.append('team %d attacks: walk set %d pads (defenders %d), pieces reachable %d of %d, walled in %d, cut off %d' % (
            att, s_['aset'], s_['dset'], r_['ok'], len(pieces), len(r_['walled']), len(r_['sealed'])))
    print('  WALK SETS: ' + '; '.join(ws))
    if via_rows is not None:
        vr = []
        for att in (0, 1):
            v = via_report(pads, out['att%d' % att], pieces, att)
            vr.append(v)
            print('  VIA: team %d attacking: pieces over 40 m %d; via pad both sides %d, one side %d, none %d; via pads reaching '
                  'their piece %d of %d; longest first leg %.2f x the straight line' % (
                      att, v['far'], v['both'], v['one'], v['none'], v['reach'], v['chosen'], v['worst']))
            for n in v['notes']:
                print('       ' + n)
        via_rows.append((menu, vr))
    if squad_rows is not None:
        sr = []
        for att in (0, 1):
            q = squads_report(pads, out['att%d' % att], pieces, att)
            sr.append(q)
            print('  SQUADS: team %d attacking: %d areas (%d with the node): all pieces walkable %d, some %d, none %d; areas over '
                  '40 m %d: via pad both sides %d, one side %d, none %d; via pads reaching their area %d of %d' % (
                      att, q['areas'], q['node'], q['full'], q['some'], q['none'], q['far'], q['both'], q['one'], q['nvia'],
                      q['reach'], q['chosen']))
            for n in q['notes']:
                print('       ' + n)
        squad_rows[menu] = sr
    for w in walls:
        flag = []
        if w['near_end']:
            flag.append('%d link(s) cross within 1.5 m of a free end: %s' % (len(w['near_end']), ' '.join(
                '%s-%s @%dcm' % (fmt_pad(pads, a), fmt_pad(pads, b), g) for g, a, b in w['near_end'])))
        if w['angle'] > 1:
            flag.append('OUR LINE %.0f deg off the real wall (rotation %s): %d link(s) closed that the wall does not cross, '
                        '%d crossing it left open' % (w['angle'], [round(x) for x in w['rot']], len(w['wrong']), len(w['missed'])))
        if verbose or flag:
            print('  wall %s at %s yaw %.0f (%s): closes %d link(s)%s' % (w['name'], w['centre'], w['yaw'], w['ends'], w['closes'],
                                                                         ('; ' + '; '.join(flag)) if flag else ''))
    summary = {}
    for att in (0, 1):
        s = out['att%d' % att]
        r = s['res']
        print('  -- team %d attacks (start %d spawn pads -> %d walkable pads), team %d defends (%d -> %d)' % (
            att, s['starts'], s['aset'], 1 - att, s['dstarts'], s['dset']))
        print('     pieces walkable %d of %d (%d via a stand-in), re-opened round a free end %d, walled in %d, no route %d, cut '
              'off (no stand-in) %d' % (r['ok'], len(pieces), r['sub'], r['reopened'], len(r['walled']), len(r['sealed']),
                                        len(r['cut'])))
        for n in r['notes']:
            print('     ' + n)
        for name, a, b in r['reopened_links']:
            print('     re-opened for %s: %s -> %s' % (name, fmt_pad(pads, a), fmt_pad(pads, b)))
        for row in s['rows']:
            if row['problems'] or verbose:
                print('     %-13s %s at (%d, %d, %d): open %s, spot %s%s, %d shooting, %d guard%s' % (
                    row['kind'], row['name'], *[round(c) for c in row['pos']], fmt_pad(pads, row['open']), fmt_pad(pads, row['pad']),
                    ' (open pad swapped from %s: behind a wall)' % fmt_pad(pads, row['swapped']) if row['swapped'] is not None else '',
                    row['shoot'], row['guard'], (': ' + '; '.join(row['problems'])) if row['problems'] else ''))
        summary[att] = s
    # the Desert dump shape: pads a human reaches round a free end (WallBlocksWalk's 1.5 m) that the
    # walk sets leave out, and which our code hands out for a piece
    dump = []
    for att in (0, 1):
        s = out['att%d' % att]
        live = s['live']
        starts = [k for k, p in pads.items() if p['type'] == 7 and p['group'] == att + 1]
        slide = set(live.reach(starts, 1, K_WALL_END_SLIDE))
        extra = slide - s['walk']
        used = {}
        for row in s['rows']:
            for what in ('open', 'pad', 'raw_open', 'raw_pad'):
                if row.get(what) is not None:
                    used.setdefault(row[what], []).append('%s %s %s' % (row['kind'], row['name'], what))
        hits = [(k, used[k]) for k in extra if k in used]
        dump.append((att, len(extra), hits))
        if extra:
            print('  -- team %d attacking: %d pad(s) reachable round a free end (crossing within 1.5 m; the walk set re-opens '
                  'only within 1.0 m, per piece) but outside the walk set%s' % (
                      att, len(extra), (': used by ' + '; '.join('%s for %s' % (fmt_pad(pads, k), ', '.join(v)) for k, v in hits))
                      if hits else ''))
    if md_rows is not None:
        md_rows.append((menu, level, len(pads), len(pieces), len(ours), len(wall_closed) // 2, summary, walls, dump, other_walls, pads))
    return summary


def markdown(md_rows, squad_rows=None):
    print()
    print('| Map | Pads / pieces / walls (links WallTick closes) | Walk set pads (team 0 / team 1 attacking) | '
          'Team 0 attacks: walkable / stand-in / re-opened / walled / no route | '
          'Team 1 attacks | Spots unwalkable (self-test, both sides) | No guard spot / no shooting spot | Wall notes |' +
          (' Squads (team 0 / team 1): areas, all walkable, via both sides / one / none |' if squad_rows else ''))
    print('|---|---|---|---|---|---|---|---|' + ('---|' if squad_rows else ''))
    for menu, level, npads, npieces, nwalls, nclosed, summary, walls, dump, other, pads in md_rows:
        cells = []
        for att in (0, 1):
            r = summary[att]['res']
            cells.append('%d / %d / %d / %d / %d' % (r['ok'], r['sub'], r['reopened'], len(r['walled']), len(r['sealed'])))
        bad = sum(len(summary[a]['bad']) for a in (0, 1))
        ng = sum(1 for a in (0, 1) for r in summary[a]['rows'] if any(p.startswith('no guard') for p in r['problems']))
        ns = sum(1 for a in (0, 1) for r in summary[a]['rows'] if any(p.startswith('no shooting') for p in r['problems']))
        notes = []
        ne = sum(len(w['near_end']) for w in walls)
        if ne:
            notes.append('%d link(s) within 1.5 m of a free end' % ne)
        off = [w for w in walls if w['angle'] > 1]
        if off:
            notes.append('our line off on %s' % ', '.join('%s (%.0f deg)' % (w['name'][-4:], w['angle']) for w in off))
        for o in other:
            notes.append('TU3 filter-%d wall (not modelled)' % o['filt'])
        sq = ''
        if squad_rows and menu in squad_rows:
            sq = ' %s |' % ' / '.join('%d, %d, %d / %d / %d' % (q['areas'], q['full'], q['both'], q['one'], q['nvia'])
                                     for q in squad_rows[menu])
        print('| %s | %d / %d / %d (%d) | %d / %d | %s | %s | %d | %d / %d | %s |%s' % (
            menu, npads, npieces, nwalls, nclosed, summary[0]['aset'], summary[1]['aset'], cells[0], cells[1], bad, ng, ns,
            '; '.join(notes) or '-', sq))


def validate(level, pk, log_paths, fixed=False):
    """The offline rebuild against live logs: each wall's centre, yaw, line and ends ('sandbag wall X
    '...' at (c), yaw y: line lo .. hi m along it (free / joined)') and every link WallTick closed
    ('sandbag wall X: closes link L (a) -> (b)'), from the first match on the map in each log; the
    log's link ends are matched to file pads by position (within 2 cm). Returns the report lines."""
    pads = load_pads(pk.pads_raw(level))
    setups = read_setups(pk.setup_raws(level), level)
    ours = build_walls(setups, ours=not fixed, any_filter=fixed)
    closed = wall_closes(pads, ours)
    by_pos = {tuple(round(c) for c in p['pos']): k for k, p in pads.items()}

    def pad_at(xyz):
        k = by_pos.get(xyz)
        if k is not None:
            return k
        k = nearest(pads, xyz)
        return k if dist(pads[k]['pos'], xyz) <= 2.5 else None
    F = r'\((-?\d+), (-?\d+), (-?\d+)\)'
    out = []
    for lp in log_paths:
        text = open(lp, encoding='utf-8', errors='replace').read()
        key = '_multi_%s_' % level
        i = text.find(key)
        if i < 0:
            out.append('%s: no %s match' % (os.path.basename(lp), level))
            continue
        # the first match: from the end of the previous WallTick (or the log's start) to this one's
        # "sandbag walls: N found" summary (a wall's "closes link" lines come before its own line)
        start = max(0, text.rfind('sandbag walls: ', 0, i))
        end = text.find('sandbag walls: ', i)
        seg = text[start:end + 200]
        live_walls = {}
        for m in re.finditer(r"sandbag wall (\w+) '[^']*_(Static\d+)' at " + F + r", yaw (-?\d+): line (-?[\d.]+) \.\. (-?[\d.]+) m "
                             r"along it \((\w+), (\w+)\); (\d+) pad link", seg):
            live_walls[m.group(2).lower()] = dict(prop=m.group(1), c=tuple(int(m.group(k)) for k in (3, 4, 5)), yaw=int(m.group(6)),
                                                  lo=float(m.group(7)), hi=float(m.group(8)), ends=(m.group(9), m.group(10)),
                                                  n=int(m.group(11)))
        prop_name = {v['prop']: k for k, v in live_walls.items()}
        live_links = {}
        unmatched = []
        for m in re.finditer(r'sandbag wall (\w+): closes link \w+ ' + F + ' -> ' + F, seg):
            a = pad_at(tuple(int(m.group(k)) for k in (2, 3, 4)))
            b = pad_at(tuple(int(m.group(k)) for k in (5, 6, 7)))
            if a is None or b is None:
                unmatched.append(m.group(0))
                continue
            live_links[(a, b)] = prop_name.get(m.group(1), m.group(1))
        wall_diffs = []
        for w in ours:
            lw = live_walls.get(w['name'])
            if not lw:
                wall_diffs.append('%s missing in the log' % w['name'])
                continue
            dc = dist(lw['c'], (w['cx'], w['cy'], w['cz']))
            ends = ('joined' if w['jlo'] else 'free', 'joined' if w['jhi'] else 'free')
            if dc > 3 or abs(lw['yaw'] - round(w['yaw'])) > 0 or abs(lw['lo'] - w['lo'] / 100) > 0.051 or \
                    abs(lw['hi'] - w['hi'] / 100) > 0.051 or lw['ends'] != ends:
                wall_diffs.append('%s: log at %s yaw %d line %.1f..%.1f %s, rebuilt at (%d, %d, %d) yaw %.0f line %.1f..%.1f %s' % (
                    w['name'], lw['c'], lw['yaw'], lw['lo'], lw['hi'], lw['ends'], w['cx'], w['cy'], w['cz'], w['yaw'],
                    w['lo'] / 100, w['hi'] / 100, ends))
        extra_live = set(live_walls) - {w['name'] for w in ours}
        mine = set(closed)
        live = set(live_links)
        only_log = sorted(live - mine)
        only_off = sorted(mine - live)
        per_wall = Counter(live_links.values())
        count_diffs = ['%s: log says %d closed, %d close lines' % (n, v['n'], per_wall.get(n, 0)) for n, v in live_walls.items()
                       if v['n'] != per_wall.get(n, 0)]
        out.append('%s: walls %d in the log, %d rebuilt, %d differ%s%s; links closed (one way each) %d in the log, %d rebuilt, '
                   '%d the same, %d log only, %d rebuilt only; %d log link end(s) with no file pad within 2.5 cm%s' % (
                       os.path.basename(lp), len(live_walls), len(ours), len(wall_diffs), (': ' + '; '.join(wall_diffs)) if wall_diffs else '',
                       (' (log only: %s)' % ', '.join(sorted(extra_live))) if extra_live else '', len(live), len(mine), len(live & mine),
                       len(only_log), len(only_off), len(unmatched), ('; per-wall count mismatch: ' + '; '.join(count_diffs)) if count_diffs else ''))
        for a, b in only_log[:12]:
            out.append('   log only: %s -> %s (%s)' % (fmt_pad(pads, a), fmt_pad(pads, b), live_links[(a, b)]))
        for a, b in only_off[:12]:
            out.append('   rebuilt only: %s -> %s (%s)' % (fmt_pad(pads, a), fmt_pad(pads, b), closed[(a, b)]))
        for u in unmatched[:5]:
            out.append('   unmatched: ' + u)
    return out


def via_markdown(via_rows):
    print()
    print('| Map | Team 0 attacks: over 40 m / via both sides / one side / none / via reaches its piece | Team 1 attacks | '
          'Longest first leg (x straight) |')
    print('|---|---|---|---|')
    for menu, vr in via_rows:
        cells = ['%d / %d / %d / %d / %d of %d' % (v['far'], v['both'], v['one'], v['none'], v['reach'], v['chosen']) for v in vr]
        print('| %s | %s | %s | %.2f / %.2f |' % (menu, cells[0], cells[1], vr[0]['worst'], vr[1]['worst']))


def level_main(args):
    maps = []
    dirs = []
    logs = []
    via = False
    squads = False
    desert_nav = False
    fixed = False
    old_flag_rule = False
    cache = os.path.join(tempfile.gettempdir(), 'pdz_navcheck')
    md = False
    verbose = False
    it = iter(args)
    for a in it:
        if a == '--packages':
            dirs.append(next(it))
        elif a == '--cache':
            cache = next(it)
        elif a == '--markdown':
            md = True
        elif a == '-v':
            verbose = True
        elif a == '--as-fixed':
            fixed = True
        elif a == '--old-flag-rule':
            old_flag_rule = True
        elif a == '--validate':
            logs.append(next(it))
        elif a == '--via':
            via = True
        elif a == '--squads':
            squads = True
        elif a == '--desert-nav':
            desert_nav = True
        elif a == '--one-way':
            global BOTH_WAYS
            BOTH_WAYS = False
        else:
            maps.append(ALIASES.get(a.lower(), a.lower()))
    if not dirs:
        here = os.path.dirname(os.path.abspath(__file__))
        docs = os.path.join(os.path.expanduser('~'), 'Documents', 'perfectdarkzerorecomp-tu3')
        dirs = [os.path.join(here, '..', 'assets', 'pdz', 'ASSETS', 'PackageData'),
                os.path.join(docs, '0000000000000000', '4D5307D3', '00000002', '28A3533063A3643DD1AF810EE03BFEA893A7C9D74D',
                             'Pdz', 'Assets', 'PackageData'),
                os.path.join(docs, 'update', 'Pdz', 'Assets', 'PackageData')]
    pk = Packages(dirs, cache)
    if logs:
        for level in maps or ['desert']:
            print('== validation, %s:' % level)
            for line in validate(level, pk, logs, fixed):
                print('  ' + line)
        return 0
    rows = [] if md else None
    via_rows = [] if via else None
    squad_rows = {} if squads else None
    for menu, level in MAPS:
        if maps and level not in maps and menu.lower() not in maps:
            continue
        run_level(level, menu, pk, rows, verbose, fixed, old_flag_rule, via_rows, squad_rows, desert_nav)
    if md:
        markdown(rows, squad_rows)
        if via_rows:
            via_markdown(via_rows)
    return 0


# --- mode 2: with a play log ------------------------------------------------------------------

def match_text(log, map_name):
    """The log text of the match on map_name (by its sandbag-wall lines)."""
    key = "_multi_%s_" % map_name
    i = log.find(key)
    if i < 0:
        return None
    starts = [m.start() for m in re.finditer(r'match starts with', log)]
    begin = max([s for s in starts if s < i] or [0])
    after = [s for s in starts if s > i]
    end = after[0] if after else len(log)
    return log[begin:end]


def read_log(path, map_name):
    log = match_text(open(path, encoding='utf-8', errors='replace').read(), map_name)
    if log is None:
        return None
    F = r'\((-?\d+), (-?\d+), (-?\d+)\)'
    walls = []
    seen = set()
    for m in re.finditer(r"sandbag wall (\w+) '[^']*' at " + F + r", yaw (-?\d+): line (-?[\d.]+) \.\. (-?[\d.]+) m", log):
        if m.group(1) in seen:
            continue
        seen.add(m.group(1))
        c = tuple(float(m.group(i)) for i in (2, 3, 4))
        yaw = math.radians(float(m.group(5)))
        ux, uz = math.cos(yaw), math.sin(yaw)
        lo, hi = float(m.group(6)) * 100, float(m.group(7)) * 100
        walls.append(dict(id=m.group(1), c=c, a=(c[0] + ux * lo, c[2] + uz * lo), b=(c[0] + ux * hi, c[2] + uz * hi), tall=164))
    pieces = {}
    for m in re.finditer(r'property (\w{8}) at ' + F, log):
        pieces.setdefault(m.group(1), tuple(float(m.group(i)) for i in (2, 3, 4)))
    att = [tuple(float(m.group(i)) for i in (1, 2, 3)) for m in re.finditer(r'attacker \w{8} \([^)]*\) at ' + F, log)]
    guards = [tuple(float(m.group(i)) for i in (1, 2, 3)) for m in re.finditer(r'guard \w{8} \([^)]*\) at ' + F, log)]
    return walls, pieces, att, guards


def closed_links(pads, walls):
    closed = set()
    for k, p in pads.items():
        for b in p['links']:
            if b not in pads:
                continue
            A, B = p['pos'], pads[b]['pos']
            for w in walls:
                if max(A[1], B[1]) < w['c'][1] - w['tall'] - 100 or min(A[1], B[1]) > w['c'][1] + w['tall']:
                    continue
                if segments_cross((A[0], A[2]), (B[0], B[2]), w['a'], w['b']):
                    closed.add((k, b))
                    break
    return closed


def reach(pads, starts, closed):
    seen = set(starts)
    q = deque(starts)
    while q:
        a = q.popleft()
        for b in pads[a]['links']:
            if b in pads and b not in seen and (a, b) not in closed:
                seen.add(b)
                q.append(b)
    return seen


def open_links(pads, k, closed):
    return sum(1 for b in pads[k]['links'] if b in pads and (k, b) not in closed)


def log_main(argv):
    name, pads_path, log_path = argv[0:3]
    pads = load_pads(pads_path, doors=False)
    got = read_log(log_path, name)
    if got is None:
        print('== %s: the log has no sandbag-wall line for this map' % name)
        return 1
    walls, pieces, att, guards = got
    closed = closed_links(pads, walls)
    att_set = reach(pads, list({nearest(pads, p) for p in att}), closed) if att else set()
    def_set = reach(pads, list({nearest(pads, p) for p in guards}), closed) if guards else set()
    if not def_set:
        def_set = att_set
    print('== %s: %d pads, %d walls, %d links closed by walls, %d pieces; attackers stood on %d pads -> %d walkable; '
          'defenders on %d -> %d' % (name, len(pads), len(walls), len(closed), len(pieces), len({nearest(pads, p) for p in att}),
                                     len(att_set), len({nearest(pads, p) for p in guards}), len(def_set)))
    if not att_set:
        print('  no attacker positions in the log: nothing to check')
        return 1
    bad_total = 0
    sealed = []
    for prop, pos in sorted(pieces.items(), key=lambda kv: kv[1]):
        near = [k for k in pads if dist(pads[k]['pos'], pos) < 3000]
        open_pad = min(near, key=lambda k: dist(pads[k]['pos'], pos)) if near else None
        floor = pads[open_pad]['pos'][1] if open_pad else pos[1]
        attack = [k for k in near if 450 <= flat(pads[k]['pos'], pos) <= 1500 and abs(pads[k]['pos'][1] - floor) <= 300]
        guard = [k for k in near if flat(pads[k]['pos'], pos) <= 1000 and abs(pads[k]['pos'][1] - floor) <= 200
                 and open_links(pads, k, closed) >= 2]
        shoot = [k for k in near if 250 <= flat(pads[k]['pos'], pos) <= 3000 and -600 <= pads[k]['pos'][1] - pos[1] <= 1500]
        rows = []
        all_bad = True
        for what, cands, side in (('open pad', [open_pad] if open_pad else [], att_set), ('attack spots', attack, att_set),
                                  ('shooting spots', shoot, att_set), ('guard spots', guard, def_set)):
            bad = [k for k in cands if k not in side or (pads[k]['flags'] & 0x200000)]
            if len(cands) - len(bad):
                all_bad = False
            if bad:
                bad_total += len(bad)
                rows.append('%s %d of %d unwalkable: %s' % (what, len(bad), len(cands), ' '.join(
                    '%d(%d,%d,%d)' % (k, *[round(c) for c in pads[k]['pos']]) for k in bad[:4])))
        if all_bad:
            sealed.append(prop)
        if rows:
            print('  piece %s at (%d, %d, %d): %s' % (prop, *[round(c) for c in pos], '; '.join(rows)))
    print('  summary: %d unwalkable candidate pad(s); pieces with NO walkable candidate at all: %s' % (
        bad_total, ' '.join(sealed) if sealed else 'none'))
    return 0


def main():
    if len(sys.argv) >= 2 and sys.argv[1] == 'level':
        return level_main(sys.argv[2:])
    if len(sys.argv) < 4:
        print(__doc__)
        return 2
    return log_main(sys.argv[1:4])


if __name__ == '__main__':
    sys.exit(main())
