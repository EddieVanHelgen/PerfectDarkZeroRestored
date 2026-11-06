"""ctf_basecheck.py: offline check that Plant the Flag carriers can reach every enemy base.

    python ctf_basecheck.py [--markdown] [LEVEL ...]

For every multiplayer level whose pad data has Capture the Flag bases (objective pads, type 5,
group 5 + team colour; the "small game" base config uses group 44 + colour instead,
`8233E520` / `8233E5F0`, settings +4487), and for each team colour that has spawn pads:

- the goal-5 destination near each ENEMY base: Rare's route ends on a linked pad by the base
  (the plant code's "goal-5 destination pad ~2.45 m from the base"); here the nearest pad with an
  open link within 5 m of the base pad (the base pad itself when it is linked);
- whether that pad is reachable from the team's spawn pads in the level data (pad links with door
  pairs joined, as sabotage_navcheck.py loads them; no Sabotage props: in Capture the Flag /
  Plant the Flag the sandbag walls and property are not spawned).

Spawns: type-7 pads of group colour + 1 (the normal config). For the small-game config the
spawn groups 10 + colour are used when the level has them (Rooftops, Plaza, GasPlant), else the
normal ones. Respawns go through the shared room under the map (group 6) and its teleporter to
those pads, so they are the start.

Reads only the game's packages (sabotage_navcheck.Packages: the repo's assets\\pdz\\ASSETS\\PackageData,
map pack 1, the TU3 update); inflated copies are cached in %TEMP%\\pdz_navcheck.
"""
import math
import os
import sys
import tempfile
from collections import deque

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sabotage_navcheck as nc  # noqa: E402

MENU = {'subway': 'Subway', 'chinatown': 'Urban', 'temple': 'Temple', 'africa': 'Old Town', 'desert': 'Desert',
        'towersmall': 'Tower', 'trench': 'Trench', 'rooftops': 'Rooftops', 'plaza': 'Plaza', 'gasplant': 'Gasplant',
        'alcatraz': 'Facility', 'temple64': 'Ruin'}


def reach(pads, starts):
    seen = set(starts)
    q = deque(starts)
    while q:
        a = q.popleft()
        for b in pads[a]['links']:
            if b not in seen:
                seen.add(b)
                q.append(b)
    return seen


def destination(pads, base_pad):
    """The linked pad goal 5 ends on by a base: the base pad if linked, else the nearest linked pad within 5 m."""
    if pads[base_pad]['links']:
        return base_pad, 0.0
    best, bd = None, 500.0
    for k, p in pads.items():
        if k == base_pad or not p['links']:
            continue
        d = math.dist(p['pos'], pads[base_pad]['pos'])
        if d <= bd:
            best, bd = k, d
    return best, bd


def check_level(pads, small):
    group0 = 44 if small else 5
    bases = {c: [k for k, p in pads.items() if p['type'] == 5 and p['group'] == group0 + c] for c in range(4)}
    bases = {c: v for c, v in bases.items() if v}
    rows = []
    for team in range(4):
        spawn_group = (10 + team) if small else (team + 1)
        starts = [k for k, p in pads.items() if p['type'] == 7 and p['group'] == spawn_group]
        used = 'groups 10+' if small and starts else 'group colour+1'
        if small and not starts:
            starts = [k for k, p in pads.items() if p['type'] == 7 and p['group'] == team + 1]
        if not starts:
            continue
        walk = reach(pads, starts)
        for enemy, base_list in bases.items():
            if enemy == team:
                continue
            for bp in base_list:
                dest, d = destination(pads, bp)
                rows.append(dict(team=team, enemy=enemy, base=bp, dest=dest, d=d, ok=dest is not None and dest in walk,
                                 spawns=len(starts), spawn_kind=used, walk=len(walk)))
    return bases, rows


def main():
    args = sys.argv[1:]
    md = '--markdown' in args
    wanted = [a for a in args if not a.startswith('--')]
    here = os.path.dirname(os.path.abspath(__file__))
    docs = os.path.join(os.path.expanduser('~'), 'Documents', 'perfectdarkzerorecomp-tu3')
    dirs = [os.path.join(here, '..', 'assets', 'pdz', 'ASSETS', 'PackageData'),
            os.path.join(docs, '0000000000000000', '4D5307D3', '00000002', '28A3533063A3643DD1AF810EE03BFEA893A7C9D74D',
                         'Pdz', 'Assets', 'PackageData'),
            os.path.join(docs, 'update', 'Pdz', 'Assets', 'PackageData')]
    pk = nc.Packages(dirs, os.path.join(tempfile.gettempdir(), 'pdz_navcheck'))
    levels = sorted({n for v in pk.index.values() for n in v['pads']})
    table = []
    for level in levels:
        if wanted and level not in wanted and MENU.get(level, '').lower() not in [w.lower() for w in wanted]:
            continue
        raw = pk.pads_raw(level)
        if not raw:
            continue
        try:
            pads = nc.load_pads(raw)
        except Exception as e:  # a pad package that isn't a level's (no record run)
            print('== %s: unreadable (%s)' % (level, e))
            continue
        for small in (False, True):
            bases, rows = check_level(pads, small)
            cfg = 'small game (groups 44 / 45)' if small else 'normal (groups 5 + colour)'
            name = '%s (%s)' % (MENU.get(level, level), level)
            if not bases:
                print('== %s, %s: no flag bases' % (name, cfg))
                if not small:
                    table.append((name, 'normal', 'no flag bases', ''))
                continue
            ok = sum(r['ok'] for r in rows)
            print('== %s, %s: bases %s; enemy bases reachable %d of %d' % (
                name, cfg, ', '.join('colour %d: %s' % (c, ' '.join(map(str, v))) for c, v in bases.items()), ok, len(rows)))
            for r in rows:
                print('   team %d (%d spawn pads, %s; walk set %d) -> colour %d base pad %d: goal-5 pad %s%s: %s' % (
                    r['team'], r['spawns'], r['spawn_kind'], r['walk'], r['enemy'], r['base'],
                    r['dest'] if r['dest'] is not None else 'NONE within 5 m',
                    ' (%.1f m off)' % (r['d'] / 100) if r['dest'] is not None and r['dest'] != r['base'] else ' (the base pad)',
                    'reachable' if r['ok'] else 'NOT REACHABLE'))
            table.append((name, 'small game' if small else 'normal',
                          '%d of %d' % (ok, len(rows)) if rows else 'no team spawns',
                          ', '.join('%d->%d %s' % (r['team'], r['enemy'], 'ok' if r['ok'] else 'NO') for r in rows)))
    if md:
        print()
        print('| Map | Base config | Enemy bases reachable from the spawns (team -> base colour) | Pairs |')
        print('|---|---|---|---|')
        for name, cfg, ok, pairs in table:
            print('| %s | %s | %s | %s |' % (name, cfg, ok, pairs or '-'))
    return 0


if __name__ == '__main__':
    sys.exit(main())
