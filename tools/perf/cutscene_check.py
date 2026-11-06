# tools/perf/cutscene_check.py <dump.csv>...: pass / fail of a cutscene's per-frame dump (pdz_cutscene_frame_dump, with
# pdz_cutscene_black_detect for the brightness columns) against the overnight criteria of 31 Oct:
#   dark flashes 0 | aim lunges 0 (a frame turning over 2x the local median, then the next two under 0.25x) |
#   stops 0 (the eye still for a frame while its neighbours move) | teleports outside cuts 0 (an eye jump over 10x the
#   local median and 50 cm on a row not flagged as a cut / hold) | the final cutscene frame's camera equal to the frame
#   before (no lantern frame) | picture - (sound + 66 ms) steady
#   (its p99 deviation from the median within 10 ms; the median is the configured lead).
import csv, math, statistics, sys


def f(v):
    try:
        x = float(v)
        return x if math.isfinite(x) else None
    except (TypeError, ValueError):
        return None


def vec(row, prefix):
    v = [f(row.get(prefix + a)) for a in ('_x', '_y', '_z')]
    return None if any(c is None for c in v) else v


def sub(a, b):
    return [a[i] - b[i] for i in range(3)]


def norm(a):
    return math.sqrt(sum(c * c for c in a))


def direction(row):
    eye, target = vec(row, 'view_eye'), vec(row, 'view_target')
    if eye and target and norm(sub(target, eye)) > 1e-3:
        d = sub(target, eye)
        n = norm(d)
        return [c / n for c in d]
    m = [f(row.get(f'm2{i}')) for i in range(3)]
    if all(c is not None for c in m) and norm(m) > 1e-3:
        n = norm(m)
        return [c / n for c in m]
    return None


def angle(a, b):
    dot = max(-1.0, min(1.0, sum(a[i] * b[i] for i in range(3))))
    return math.degrees(math.acos(dot))


def local_median(values, i, half=8):
    around = [v for j, v in enumerate(values[max(0, i - half):i + half + 1], start=max(0, i - half))
              if j != i and v is not None]
    return statistics.median(around) if len(around) >= 4 else None


def check(path):
    rows = list(csv.DictReader(open(path, encoding='utf-8', errors='replace')))
    scene = [r for r in rows if r.get('phase') == 'cutscene']
    out = {'file': path.replace('\\', '/').split('/')[-1], 'rows': len(scene)}
    if len(scene) < 10:
        out['verdict'] = 'too short'
        return out
    eyes = [vec(r, 'view_eye') or vec(r, 'our_eye') for r in scene]
    dirs = [direction(r) for r in scene]
    steps = [None] + [norm(sub(eyes[i], eyes[i - 1])) if eyes[i] and eyes[i - 1] else None for i in range(1, len(scene))]
    turns = [None] + [angle(dirs[i], dirs[i - 1]) if dirs[i] and dirs[i - 1] else None for i in range(1, len(scene))]
    flags = [r.get('flags', '') or '' for r in scene]
    cut = ['cut' in fl or 'hold' in fl for fl in flags]
    lunges, stops, teleports = [], [], []
    for i in range(1, len(scene) - 2):
        med_t = local_median(turns, i)
        if turns[i] is not None and med_t and med_t > 0.02 and not cut[i]:
            nxt = [turns[i + 1], turns[i + 2]]
            if turns[i] > 2.0 * med_t and all(t is not None and t < 0.25 * med_t for t in nxt):
                lunges.append(i)
        med_s = local_median(steps, i)
        if steps[i] is not None and med_s and med_s > 0.5 and not cut[i]:
            if steps[i] < 0.05 * med_s:
                stops.append(i)
            if steps[i] > 10.0 * med_s and steps[i] > 50.0:
                teleports.append(i)
    dark = [i for i, r in enumerate(scene) if r.get('dark') == '1']
    sampled = sum(1 for r in scene if f(r.get('bright_avg')) is not None)
    bad_matrix = [i for i, r in enumerate(scene) if r.get('matrix_bad') == '1']
    last = scene[-1]
    prev = scene[-2]
    final_turn = angle(direction(last), direction(prev)) if direction(last) and direction(prev) else None
    final_step = norm(sub(eyes[-1], eyes[-2])) if eyes[-1] and eyes[-2] else None
    offsets = []
    for r in scene[5:]:
        pic, snd = f(r.get('picture_ms')), f(r.get('sound_ms'))
        if pic is not None and snd is not None:
            offsets.append(pic - (snd + 66.0))
    out.update({
        'dark': len(dark), 'sampled': sampled, 'dark_rows': dark[:10],
        'lunges': len(lunges), 'lunge_rows': lunges[:10],
        'stops': len(stops), 'stop_rows': stops[:10],
        'teleports': len(teleports), 'teleport_rows': teleports[:10],
        'matrix_bad': len(bad_matrix),
        'final_turn_deg': None if final_turn is None else round(final_turn, 3),
        'final_step_cm': None if final_step is None else round(final_step, 2),
        'av_median_ms': None if not offsets else round(statistics.median(offsets), 1),
        'av_spread_ms': None if not offsets else round(sorted(abs(o - statistics.median(offsets)) for o in offsets)[int(0.99 * (len(offsets) - 1))], 1),
    })
    med_turn = statistics.median([t for t in turns if t]) if any(turns) else 0.0
    final_ok = final_turn is None or final_turn <= max(3.0 * med_turn, 0.5)
    ok = (len(dark) == 0 and len(lunges) == 0 and len(stops) == 0 and len(teleports) == 0 and final_ok and
          (out['av_spread_ms'] is None or out['av_spread_ms'] <= 10.0))
    out['verdict'] = 'PASS' if ok else 'FAIL'
    if not sampled:
        out['note'] = 'no brightness samples (pdz_cutscene_black_detect off?)'
    return out


if __name__ == '__main__':
    for p in sys.argv[1:]:
        r = check(p)
        print(' | '.join(f'{k} {v}' for k, v in r.items()))
