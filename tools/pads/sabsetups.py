"""sabsetups.py RAW: every placed propsetup record (144 bytes) with template, mode filter (+49) and position (+4 vec3)."""
import re, struct, sys
BS = re.escape(b'\x5c')
PAT = re.compile(b'uberroot' + BS + b'aid' + BS + b'xml' + BS + b'propsetup' + BS + b'multi' + BS +
                 b'([a-z0-9_]+)' + BS + b'([a-z0-9_]+)' + BS + b'([A-Za-z0-9_]+)' + BS + b'[A-Za-z0-9_]+[.]xml\x00')
TPL = re.compile(rb'aid_xml_proptemplate_([a-z0-9_]+)')
d = open(sys.argv[1], 'rb').read(); seen = set()
for m in PAT.finditer(d):
    k = m.groups()
    if k in seen: continue
    seen.add(k)
    h = d.find(b'\x00\x00\x00\x0c', m.end(), m.end() + 32)
    if h < 0: continue
    size = struct.unpack('>I', d[h + 4:h + 8])[0]
    data = d[h + 12:h + 12 + size]
    t = TPL.search(d[h + 12 + size:h + 12 + size + 0x200])
    if not t or len(data) < 64: continue
    x, y, z = struct.unpack('>3f', data[4:16])
    print('%-34s %-10s %-22s size %3d filter %3d pos (%7.0f %6.0f %7.0f)' % (t.group(1).decode(), k[1].decode(), k[2].decode()[:22], size, data[49], x, y, z))
