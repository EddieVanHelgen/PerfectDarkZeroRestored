"""Summarise a PDZ Sabotage-bots log: python -I sabotage_log_summary.py <log path>."""
import re
import sys
from collections import Counter

path = sys.argv[1]
lines = open(path, encoding="utf-8", errors="replace").read().splitlines()
text = "\n".join(lines)


def count(pattern):
    return len(re.findall(pattern, text))


maps = Counter(re.findall(r"aid_xml_propsetup_multi_([a-z0-9]+)", text))
print("map:", maps.most_common(1)[0][0] if maps else "?", "| lines:", len(lines))
print("errors:", count(r"\[error\]"), "| crash:", "yes" if re.search(r"Unhandled|invalid or unregistered", text) else "no")
for m in re.finditer(r"(\d\d:\d\d:\d\d)\.\d+\].*Sabotage round (\d+): defenders team (\d+), property (\d+) pieces", text):
    print(f"round {m.group(2)} at {m.group(1)}: defenders team {m.group(3)}, {m.group(4)} pieces")
metrics = [
    ("pieces destroyed", r"pdz_mp_announcer: Sabotage: DO_FUEL_DUMP_DESTROYED"),
    ("attack spots found", r"attack spot [0-9A-F]+ at"),
    ("no spot (open pad used)", r"no spot 4\.5-15 m"),
    ("spots rejected by a prop box", None),
    ("hold starts (shooting ...)", r"holds the trigger of hand"),
    ("reloaded (animation)", r"reloaded after \d+ press"),
    ("reloaded (AI way, direct)", r"clip refilled Rare's AI way"),
    ("gun won't reload (bad gun)", r"gun won't reload"),
    ("gun path failed -> melee", r"gun path failed"),
    ("no other gun -> melee", r"no other gun: melee"),
    ("punching did nothing -> gun", r"did nothing; back to the gun"),
    ("punching did nothing -> give up", r"did nothing and no loaded gun"),
    ("out of reach give-ups", r"out of reach, picking another"),
    ("pulsing", r"pulsing it"),
    ("stops shooting", r"stops shooting"),
    ("draws", r"has no gun out: draws"),
    ("team plans", r"team \d+ plan:"),
]
for name, pat in metrics:
    if pat is None:
        n = sum(int(x) for x in re.findall(r"(\d+) behind a prop's box", text))
    else:
        n = count(pat)
    print(f"  {name:34s} {n}")
states = Counter(re.findall(r"Sabotage bots: attacker [0-9A-F]+ at \(.*?\): property [0-9A-F]+ .*?enemy target [0-9A-F]+: (.*)$", text, re.M))
print("attacker states (5 s status lines):")
for s, n in states.most_common(10):
    print(f"  {n:5d} {s}")
dist = re.findall(r"nearest human (\d+) m", text)
if dist:
    d = sorted(int(x) for x in dist)
    print(f"nearest human on reload lines: min {d[0]} m, median {d[len(d)//2]} m, max {d[-1]} m")
shots = Counter()
for m in re.finditer(r"attacker ([0-9A-F]+) holds the trigger at [0-9A-F]+ \([\d.]+ s\): \d+ controller frames, \d+ with the trigger set[^;]*, (\d+) shots", text):
    shots[m.group(1)] = max(shots[m.group(1)], int(m.group(2)))
if shots:
    print("max shots per hold, per attacker:", dict(shots.most_common(8)))
last = [l for l in lines if "Sabotage bots: attacker" in l and " at (" in l][-3:]
print("last attacker status lines:")
for l in last:
    print("  " + l[12:24] + l[59:220])
# CP3 (demo kit plant, escorts, announcer cues), per round
print("CP3 per round:")
rounds = [(i, l) for i, l in enumerate(lines) if re.search(r"Sabotage round \d+: defenders team", l)]
bounds = [i for i, _ in rounds] + [len(lines)]
for r in range(len(rounds)):
    seg = lines[bounds[r]:bounds[r + 1]]
    def c(p):
        return sum(1 for l in seg if re.search(p, l))
    times = [float(m.group(1)) for l in seg for m in [re.search(r"plants demo kit .* after ([\d.]+) s", l)] if m]
    mp = Counter(m.group(1) for l in seg for m in [re.search(r"propsetup_multi_([a-z0-9]+)_", l)] if m)
    print(f" round {r + 1} map {mp.most_common(1)[0][0] if mp else '?'}: plants started {c('starts planting demo kit')}, completed {c('plants demo kit .* calling')}"
          f" (times {', '.join(f'{t:.1f}' for t in times) or '-'} s), cancelled {c('plant cancelled')},"
          f" kept planter role {c('keeps the planter role')}")
    print(f"   escorts assigned {c('escort: attacker .* escorts')}, ended {c('stops escorting')}")
    for l in seg:
        m = re.search(r"escort: attacker (\w+) escorts demo-kit carrier (\w+).*\), ([\d.]+) m away, height (-?[\d.]+) m", l)
        if m:
            print(f"     {l[12:20]} {m.group(1)} -> {m.group(2)} {m.group(3)} m (height {m.group(4)} m)")
    cues = Counter(m.group(1) for l in seg for m in [re.search(r"announcer cue XACT_SOUNDBANK_MULTIPLAYER_(\w+)", l)] if m)
    print(f"   announcer cues: {dict(cues)}")
    ann = Counter(m.group(1) for l in seg for m in [re.search(r"pdz_mp_announcer: Sabotage: (\w+)", l)] if m)
    if ann:
        print(f"   announcer extras: {dict(ann)}")
    print(f"   start cue lines: {Counter(m.group(1) for l in seg for m in [re.search(r'start cue: (no use point|use \w+ already|825F05E0)', l)] if m)}")
    print(f"   bullet-ray rejections logged: {c('bullet ray: from')}")
    print(f"   charges: detached {c('done planting .* the charge detached')}, no detach {c('done planting .* NO detach')}; guard spots given {c('guard .* gets spot')}, dropped {c('to his spot .* spot dropped')}; stall probes {c('stall probe:')}")
    print(f"   cue fix: {c('Sabotage charge set')}, pieces destroyed (msg 20 lines): {c('DO_.*_DESTROYED|property [0-9A-F]+ is gone')}")
