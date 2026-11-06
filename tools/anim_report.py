"""Animations never seen in play, by group.

Usage: python tools/anim_report.py [--log PATH] [--group TEXT] [--names] [--everything]

Reads tools/anim_inventory.csv (every animation in the packages, from
tools/anim_inventory.py) and the pdz_anim_log file, logs/anim_seen.csv
(default: out/build/win-amd64-release/logs/anim_seen.csv). Prints, per group,
how many animations exist, how many were seen, and the ones never seen (with
--names, or for groups of 40 or fewer); the likely-unused ones from
docs/SP_INVENTORY.md ("Animations in depth") are marked with their rank.

The log only records character animations (picked by type, looked up by name,
or played by scripts), so by default only those groups are listed;
--everything adds weapon, prop and camera animations (never seen there means
"not recorded", not "unused"). --group keeps the groups containing TEXT.
"""
import collections
import csv
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOGGED = ('chars/', 'characters', 'testanims')


def main():
    args = sys.argv[1:]
    log = os.path.join(ROOT, 'out', 'build', 'win-amd64-release', 'logs', 'anim_seen.csv')
    group_filter = None
    if '--log' in args:
        log = args[args.index('--log') + 1]
    if '--group' in args:
        group_filter = args[args.index('--group') + 1]
    names = '--names' in args
    everything = '--everything' in args

    rows = list(csv.DictReader(open(os.path.join(ROOT, 'tools', 'anim_inventory.csv'), encoding='utf-8')))
    seen = {}
    if os.path.exists(log):
        for r in csv.DictReader(open(log, encoding='utf-8')):
            seen[r['name'].lower()] = r
    else:
        print('no log at %s yet (run with --pdz_anim_log=true); everything counts as unseen' % log)

    groups = collections.defaultdict(list)
    for r in rows:
        g = r['group']
        if not everything and not g.startswith(LOGGED):
            continue
        if group_filter and group_filter not in g:
            continue
        groups[g].append(r)

    total = [0, 0]
    print('%-22s %6s %6s %6s' % ('group', 'anims', 'seen', 'unseen'))
    for g in sorted(groups):
        rs = groups[g]
        unseen = [r for r in rs if r['name'] not in seen]
        total[0] += len(rs)
        total[1] += len(rs) - len(unseen)
        print('%-22s %6d %6d %6d' % (g, len(rs), len(rs) - len(unseen), len(unseen)))
        if unseen and (names or len(unseen) <= 40):
            for r in sorted(unseen, key=lambda r: (int(r['suspect_rank'] or 9), r['name'])):
                mark = ('[%s] ' % r['suspect_rank']) if r['suspect_rank'] else ''
                where = (' (%s)' % r['levels']) if r['levels'] and r['levels'] != 'shared character animations' else ''
                note = ('  - ' + r['note']) if r['note'] else ''
                print('      %s%s%s%s' % (mark, r['name'][len('aid_anim_'):], where, note))
    print('%-22s %6d %6d %6d' % ('total', total[0], total[1], total[0] - total[1]))
    extra = sorted(n for n in seen if n not in {r['name'] for r in rows})
    if extra:
        print('\nseen but not in the inventory (DLC map packs or a different name): %d' % len(extra))
        for n in extra[:40]:
            print('      ' + n)


if __name__ == '__main__':
    main()
