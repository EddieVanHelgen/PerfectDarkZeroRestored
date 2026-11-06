# Average the level-only 600-frame windows of a log: frame, game thread split, native phases, record thread, GPU passes.
import re, sys, collections

def strip(l):
    return re.sub(r'^\[[^\]]*\] \[[^\]]*\] \[[^\]]*\] \[[^\]]*\] ', '', l.rstrip())

def nums(pattern, text, cast=float):
    m = re.search(pattern, text)
    return cast(m.group(1)) if m else None

for path in sys.argv[1:]:
    lines = [strip(l) for l in open(path, encoding='utf-8', errors='replace')]
    acc = collections.defaultdict(list)
    level_window = False
    for l in lines:
        if l.startswith('pdz_timing: scenes (window)'):
            level_window = re.search(r'level \(state 21\): frames 600,', l) is not None
            if level_window:
                acc['frame'].append(nums(r'level \(state 21\): frames 600, frame ms avg ([\d.]+)', l))
                acc['p99'].append(nums(r'level \(state 21\): frames 600, frame ms avg [\d.]+ p50 [\d.]+ p99 ([\d.]+)', l))
                acc['over12'].append(nums(r'over 12.50 ms \d+ \(([\d.]+)%\)', l))
        elif not level_window:
            continue
        elif l.startswith('pdz_timing: game thread split ms per frame avg/p99 (window)'):
            for k, pat in [('hooks', r'our hooks ([\d.]+)'), ('guestd3d', r'guest D3D ([\d.]+)'),
                           ('spin', r'spin waits ([\d.]+)'), ('block', r'blocking waits ([\d.]+)'),
                           ('rest', r'rest ([\d.]+)'), ('cpu', r'thread cpu ([\d.]+)')]:
                acc[k].append(nums(pat, l))
        elif l.startswith('pdz_timing: phases ms per frame avg/p99 (window'):
            for k in ['native cpu', 'draw', 'tex.hash', 'tex.upload', 'pso.create', 'present.sdk_post', 'shadow.record',
                      'guest.d3d', 'wait.guest_fence', 'rt.enqueue', 'resolve']:
                v = nums(re.escape(k) + r' ([\d.]+)/', l)
                if v is not None:
                    acc[k].append(v)
            acc['draws'].append(nums(r'draws (\d+)/frame', l))
            acc['ns/draw'].append(nums(r'native draw (\d+) ns/draw', l))
        elif l.startswith('pdz_timing: record thread (window)'):
            acc['rt busy'].append(nums(r'worker busy ms per frame avg ([\d.]+)', l))
            acc['rt room wait'].append(nums(r'room ([\d.]+)/', l))
        elif l.startswith('pdz_timing: frames 600'):
            acc['gpu'].append(nums(r'native gpu ms avg ([\d.]+)', l))
    # GPU passes lines are not scene-tagged: take the windows after the first level window (the steady part).
    passes = [l for l in lines if l.startswith('pdz_native: gpu passes')]
    print('=====', path.split('/')[-1], 'level windows', len(acc['frame']))
    out = []
    for k, v in acc.items():
        v = [x for x in v if x is not None]
        if v:
            out.append(f'{k} {sum(v)/len(v):.2f}')
    print('  ' + ' | '.join(out))
    if passes:
        tail = passes[len(passes)//2:]
        kinds = collections.defaultdict(list)
        for l in tail:
            for name, ms in re.findall(r'([a-z]+ [a-z]+) ([\d.]+) \(', l):
                kinds[name].append(float(ms))
            kinds['total'].append(nums(r'total ([\d.]+)', l))
        print('  gpu passes (2nd half): ' + ', '.join(f'{k} {sum(v)/len(v):.2f}' for k, v in kinds.items()))
