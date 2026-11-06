"""Per-match verdict for a Sabotage bot log: pieces destroyed, plants, stuck bots, crash.

Usage: python sabotage_map_verdict.py <log>
A match starts at "Sabotage round 1:"; its map comes from the property names.
"""
import re
import sys
from collections import Counter

lines = open(sys.argv[1], encoding="utf-8", errors="replace").read().splitlines()
crash = any(re.search(r"Unhandled guest access violation|invalid or unregistered", l) for l in lines)
starts = [i for i, l in enumerate(lines) if "Sabotage round 1: defenders" in l]
starts.append(len(lines))
for k in range(len(starts) - 1):
    seg = lines[starts[k]:starts[k + 1]]
    maps = Counter(m.group(1) for l in seg for m in [re.search(r"propsetup_multi_([a-z0-9]+)_", l)] if m)
    rounds = sum(1 for l in seg if re.search(r"Sabotage round \d+: defenders", l))
    destroyed = sum(1 for l in seg if "Sabotage message 20:" in l)
    by_bot = sum(1 for l in seg if "Sabotage message 20:" in l and "(vtable 820BF3B8), r7" in l)
    by_human = sum(1 for l in seg if "Sabotage message 20:" in l and "(vtable 820C7DE0), r7" in l)
    started = sum(1 for l in seg if "starts planting demo kit" in l)
    done = sum(1 for l in seg if re.search(r"plants demo kit .* calling", l))
    detached = sum(1 for l in seg if re.search(r"done planting .* the charge detached", l))
    no_kit = sum(1 for l in seg if "not ready after 6 s" in l)
    stalls = sum(1 for l in seg if re.search(r"stalled [\d.]+ m from property", l))
    give_up = sum(1 for l in seg if re.search(r"can't get nearer than [\d.]+ m to property", l))
    guard_stuck = sum(1 for l in seg if re.search(r"guard \w+ at .* can't get nearer", l))
    no_land = sum(1 for l in seg if "the shots don't land" in l)
    written = sum(1 for l in seg if "piece written off for the team" in l)
    idle = sum(1 for l in seg if "no soft property left" in l)
    print(f"match {k + 1}: {maps.most_common(1)[0][0] if maps else '?'}, {rounds} round(s), "
          f"{seg[0][12:20]}-{seg[-1][12:20]}")
    print(f"  pieces destroyed {destroyed} (bots {by_bot}, human {by_human})")
    print(f"  plants started {started}, completed {done}, charges detached {detached}; kit never ready {no_kit}")
    print(f"  stuck: attacker stalls {stalls}, out-of-reach give-ups {give_up}, guard spots dropped {guard_stuck}; "
          f"shots-don't-land {no_land} (pieces written off {written}); idle 'no target' {idle}")
print("crash:", "YES" if crash else "no")
