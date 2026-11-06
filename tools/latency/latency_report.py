"""Latency report of one game log (pdz_latency_log): per reaction kind (fire / turn), p50 / p95 / max of each step from
the input change (the pad read that first saw it) to the reacting frame's Swap, its native submit, its host present and
its refresh on screen; the frame count to the reaction; the SDK's frame_pacing summaries; the run's timing SUMMARY lines.

    python tools/latency/latency_report.py <PDZRestored_NNN.log> [--rows]
"""
import re
import sys
from collections import defaultdict


def spread(values):
    if not values:
        return 'n/a'
    v = sorted(values)
    at = lambda p: v[min(len(v) - 1, len(v) * p // 100)]
    return 'p50 %6.1f  p95 %6.1f  max %6.1f  (n %d)' % (at(50), at(95), v[-1], len(v))


def pacing(log_path):
    """Present intervals in the level (after the scripted input starts, 105 s after the first P row) from the
    frame_pacing CSV copy next to the log (run_latency.ps1 writes frame_pacing_<log>.csv)."""
    import os
    csv = os.path.join(os.path.dirname(log_path), 'frame_pacing_' + os.path.basename(log_path)[:-4] + '.csv')
    if not os.path.exists(csv):
        return
    t = []
    with open(csv) as f:
        for line in f:
            if line.startswith('P,'):
                t.append(float(line.split(',')[1]) / 1000.0)
    if len(t) < 100:
        return
    t = [x for x in t if x > t[0] + 105000.0]
    gaps = sorted(b - a for a, b in zip(t, t[1:]))
    if not gaps:
        return
    at = lambda p: gaps[min(len(gaps) - 1, len(gaps) * p // 100)]
    budget = at(50)
    over = sum(1 for g in gaps if g > budget * 1.5)
    print('present intervals (level): p50 %.2f p99 %.2f max %.2f ms, over 1.5x p50: %d of %d (%.2f%%)' % (
        at(50), at(99), gaps[-1], over, len(gaps), 100.0 * over / len(gaps)))


def main():
    path = sys.argv[1]
    rows = '--rows' in sys.argv
    text = open(path, encoding='utf-8', errors='replace').read()
    events = defaultdict(dict)
    for m in re.finditer(r"pdz_latency: reaction #(\d+) (\w+): input change -> the reacting frame's Swap ([\d.]+) ms "
                         r"\((\d+) frames? later", text):
        e = events[int(m[1])]
        e['kind'], e['swap'], e['frames'] = m[2], float(m[3]), int(m[4])
    for m in re.finditer(r'pdz_latency: reaction #(\d+): input change -> native submit ([\d.]+) ms', text):
        events[int(m[1])]['submit'] = float(m[2])
    for m in re.finditer(r'frame_pacing: reaction #(\d+) presented: input change -> present ([\d.]+) ms', text):
        events[int(m[1])]['present'] = float(m[2])
    for m in re.finditer(r'frame_pacing: reaction #(\d+) on screen: input change -> screen ([\d.]+) ms '
                         r'\(present -> screen ([\d.]+)\)', text):
        events[int(m[1])]['screen'] = float(m[2])
    for m in re.finditer(r'frame_pacing: reaction #(\d+) at the next display refresh: input change -> refresh ([\d.]+) ms',
                         text):
        events[int(m[1])]['refresh'] = float(m[2])
    none = defaultdict(int)
    for m in re.finditer(r'pdz_latency: #\d+ (\w+): no reaction', text):
        none[m[1]] += 1
    for kind in ('fire', 'turn'):
        ev = [e for e in events.values() if e.get('kind') == kind]
        if not ev and not none[kind]:
            continue
        print('%s: %d reactions, %d without one' % (kind, len(ev), none[kind]))
        for key, name in (('frames', 'frames to the reaction  '), ('swap', 'change -> reacting Swap '),
                          ('submit', 'change -> native submit '), ('present', 'change -> host present  '),
                          ('refresh', 'change -> next refresh  '),
                          ('screen', 'change -> on screen     ')):
            print('   %s %s' % (name, spread([e[key] for e in ev if key in e])))
    if rows:
        for i in sorted(events):
            print(i, events[i])
    summaries = re.findall(r'frame_pacing: (input->screen ms [^\n]*)', text)
    if summaries:
        print('frame_pacing summaries: %d; last: %s' % (len(summaries), summaries[-1][:400]))
    for m in re.finditer(r'(pdz_(?:timing|clock|native|latency): SUMMARY[^\n]*)', text):
        print(m[1][:600])
    pacing(path)
    hz = re.search(r'pdz_pace: [^\n]*', text)
    if hz:
        print(hz[0][:300])


if __name__ == '__main__':
    main()
