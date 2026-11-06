"""Top-down PNG of a Sabotage area: breadcrumbs, obstacle boxes, pieces, pads.
Usage: python plot_walk.py <log> <walk.txt> <out.png> xmin xmax zmin zmax [t0 t1]"""
import re, sys, zlib, struct
log, walk, out = sys.argv[1:4]
x0, x1, z0, z1 = map(float, sys.argv[4:8])
t0, t1 = (float(sys.argv[8]), float(sys.argv[9])) if len(sys.argv) > 9 else (-1e9, 1e9)
W = 900
scale = W / max(x1 - x0, z1 - z0)
H = int((z1 - z0) * scale) + 1
Wp = int((x1 - x0) * scale) + 1
img = [[(255, 255, 255)] * Wp for _ in range(H)]
def px(x, z):
    return int((x - x0) * scale), int((z1 - z) * scale)  # +z up
def dot(x, z, c, r=2):
    cx, cy = px(x, z)
    for dy in range(-r, r + 1):
        for dx in range(-r, r + 1):
            if 0 <= cx + dx < Wp and 0 <= cy + dy < H and dx * dx + dy * dy <= r * r:
                img[cy + dy][cx + dx] = c
def line(xa, za, xb, zb, c):
    n = int(max(abs(xb - xa), abs(zb - za)) * scale) + 1
    for i in range(n + 1):
        dot(xa + (xb - xa) * i / n, za + (zb - za) * i / n, c, 0)
text = open(log, encoding='utf-8', errors='replace').read()
# grid every 2 m
g = 200
gx = (int(x0 // g) + 1) * g
while gx < x1:
    line(gx, z0, gx, z1, (235, 235, 235)); gx += g
gz = (int(z0 // g) + 1) * g
while gz < z1:
    line(x0, gz, x1, gz, (235, 235, 235)); gz += g
seen = set()
for m in re.finditer(r"obstacle (\w+): prop \w+ '([^']*)' \([^)]*mode filter (\d+)[^)]*\) at \([^)]*\); box \((-?\d+), (-?\d+), (-?\d+)\) to \((-?\d+), (-?\d+), (-?\d+)\)", text):
    ob, name, f = m.group(1), m.group(2), m.group(3)
    a = list(map(float, m.groups()[3:]))
    if ob in seen or a[0] == a[3]:
        continue
    seen.add(ob)
    if a[3] < x0 or a[0] > x1 or a[5] < z0 or a[2] > z1:
        continue
    c = (230, 120, 0) if f != '19' else (200, 0, 0)
    for (xa, za, xb, zb) in ((a[0], a[2], a[3], a[2]), (a[3], a[2], a[3], a[5]), (a[3], a[5], a[0], a[5]), (a[0], a[5], a[0], a[2])):
        line(xa, za, xb, zb, c)
for m in re.finditer(r"pad (\w{8}) at \((-?\d+), (-?\d+), (-?\d+)\)", text):
    x, z = float(m.group(2)), float(m.group(4))
    if x0 <= x <= x1 and z0 <= z <= z1:
        dot(x, z, (0, 90, 255), 3)
for l in open('sabotage_spots/desert_links.txt'):
    v = list(map(float, l.split()))
    if len(v) == 6:
        line(v[0], v[2], v[3], v[5], (0, 170, 0))
prev = None
for l in open(walk):
    v = l.split()
    if len(v) < 4:
        continue
    t, x, y, z = map(float, v[:4])
    if not (t0 <= t <= t1):
        prev = None
        continue
    if x0 <= x <= x1 and z0 <= z <= z1:
        if prev:
            line(prev[0], prev[1], x, z, (0, 0, 0))
        dot(x, z, (0, 0, 0), 1)
        prev = (x, z)
    else:
        prev = None
for m in re.finditer(r"human's firing spot \((-?\d+), (-?\d+), (-?\d+)\)", text):
    dot(float(m.group(1)), float(m.group(3)), (160, 0, 200), 4)
raw = b''.join(b'\x00' + bytes(sum(row, ())) for row in img)
def chunk(k, d):
    return struct.pack('>I', len(d)) + k + d + struct.pack('>I', zlib.crc32(k + d) & 0xffffffff)
open(out, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', Wp, H, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))
print("wrote", out, Wp, H)
