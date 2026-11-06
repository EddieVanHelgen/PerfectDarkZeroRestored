"""padgraph.py: Rare's aipads (aid_aipads_*) decoded from an inflated package.

Pad records: 60 bytes from 0x2F8: +0 u32 flags, +4 u8 type, +8 vec3 position,
+20 4 floats (rotation), +36 u16 first link, +38 u16 link count, +40 u16 pad
number, +42 u16, +44 u16 group. The link table (u16 pad numbers) follows the
last record, after an 8-byte header.
"""
import math
import struct
from collections import deque


def load(path):
    d = open(path, 'rb').read()
    base = next(b for b in range(0x100, 0x4000, 4)
                if all(struct.unpack('>H', d[b + 60 * k + 40:b + 60 * k + 42])[0] == k for k in range(12)))
    pads = {}
    k = 0
    last = -1
    while True:
        o = base + 60 * k
        r = d[o:o + 60]
        if len(r) < 60:
            break
        pid = struct.unpack('>H', r[40:42])[0]
        if pid <= last or pid > last + 50:
            break
        first, cnt = struct.unpack('>HH', r[36:40])
        pads[pid] = dict(id=pid, off=o, first=first, cnt=cnt, pos=struct.unpack('>3f', r[8:20]),
                         rot=struct.unpack('>4f', r[20:36]), flags=struct.unpack('>I', r[0:4])[0], type=r[4],
                         b5=r[5], b6=r[6], b7=r[7], w42=struct.unpack('>H', r[42:44])[0],
                         grp=struct.unpack('>H', r[44:46])[0], tail=r[46:60].hex())
        last = pid
        k += 1
    lb = base + 60 * k + 8
    for p in pads.values():
        p['links'] = [struct.unpack('>H', d[lb + 2 * i:lb + 2 * i + 2])[0] for i in range(p['first'], p['first'] + p['cnt'])]
    return pads


def near(pads, pos, radius):
    return sorted((math.dist(p['pos'], pos), pid) for pid, p in pads.items() if math.dist(p['pos'], pos) <= radius)


def reach(pads, start, closed=frozenset()):
    """Pads reachable from `start` over links (a link (a, b) in `closed` is skipped)."""
    seen = {start}
    q = deque([start])
    while q:
        a = q.popleft()
        for b in pads[a]['links']:
            if b in pads and b not in seen and (a, b) not in closed:
                seen.add(b)
                q.append(b)
    return seen


def path(pads, start, goal, closed=frozenset()):
    prev = {start: None}
    q = deque([start])
    while q:
        a = q.popleft()
        if a == goal:
            out = []
            while a is not None:
                out.append(a)
                a = prev[a]
            return out[::-1]
        for b in pads[a]['links']:
            if b in pads and b not in prev and (a, b) not in closed:
                prev[b] = a
                q.append(b)
    return None
