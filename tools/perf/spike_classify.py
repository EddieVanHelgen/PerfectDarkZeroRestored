# Classify every level spike line (> 12.5 ms) of a pdz_timing spike log by what made the frame long.
import re, sys, collections

def f(pat, s, d=0.0):
    m = re.search(pat, s)
    return float(m.group(1)) if m else d

def phase_list(section):
    return {k: float(v) for k, v in re.findall(r'([a-z_.]+) ([\d.]+)', section)}

def classify(l):
    frame = f(r'frame ([\d.]+) ms', l)
    cpu = f(r'game thread: cpu ([\d.]+)', l)
    blocked = f(r'blocked ([\d.]+) \(frame - cpu\)', l)
    outside_sec = re.search(r'outside native [\d.]+: ([^|]*)\|', l)
    outside = phase_list(outside_sec.group(1)) if outside_sec else {}
    native_sec = re.search(r'\| native cpu ([\d.]+) \(record thread\): ([^|]*)\|', l)
    native = float(native_sec.group(1)) if native_sec else 0.0
    nph = phase_list(native_sec.group(2)) if native_sec else {}
    res_ms = f(r'resources created \d+ \(([\d.]+)\)', l)
    tex_created = f(r'textures created (\d+)', l)
    game_code = cpu - sum(outside.values())
    fence = outside.get('wait.guest_fence', 0.0)
    room = f(r'game thread waits room ([\d.]+) ms', l)
    upload = nph.get('tex.upload', 0.0)
    if upload + res_ms >= 3.0 or (tex_created and upload >= 2.0):
        return 'native: texture creation / upload (streaming)', frame
    if nph.get('pso.create', 0.0) >= 2.0:
        return 'native: PSO creation', frame
    if nph.get('tex.hash', 0.0) >= 3.0:
        return 'native: texture hashing', frame
    if nph.get('frame.begin', 0.0) >= 3.0:
        return 'wait: GPU fence (native frame slot, GPU-bound)', frame
    if nph.get('present.sdk_post', 0.0) >= 3.0:
        return 'wait: host present / vblank latch', frame
    if blocked >= 4.0:
        return 'OS / thread scheduling (game thread off the CPU)', frame
    if game_code >= 8.33:
        return 'game code (guest, own CPU time over budget)', frame
    if native >= 8.33 and fence + room >= 1.0:
        return 'native: record thread over budget (draw recording)', frame
    return f'mixed (game {game_code:.1f}, native {native:.1f}, fence {fence:.1f}, blocked {blocked:.1f})', frame

for path in sys.argv[1:]:
    lines = [l for l in open(path, encoding='utf-8', errors='replace') if 'pdz_timing: spike swap' in l and 'scene level' in l]
    summary = [l for l in open(path, encoding='utf-8', errors='replace') if 'spikes SUMMARY' in l]
    hist = collections.Counter()
    worst = {}
    n = 0
    for l in lines:
        c, frame = classify(l)
        if frame <= 12.5:
            continue
        n += 1
        key = c if not c.startswith('mixed') else 'mixed'
        hist[key] += 1
        worst[key] = max(worst.get(key, 0), frame)
    tot = re.search(r'over 12 ms: (\d+) of (\d+)', summary[-1]) if summary else None
    print(f'===== {path.split("/")[-1]}: {n} level spike lines over 12.5 ms classified'
          + (f' (spikes over 12 ms in run: {tot.group(1)} of {tot.group(2)} frames)' if tot else ''))
    for k, v in hist.most_common():
        print(f'   {v:4d}  {k}  (worst {worst[k]:.1f} ms)')
