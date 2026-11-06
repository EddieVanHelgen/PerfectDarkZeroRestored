"""stfs_verify.py PACKAGE NAME_SUFFIX... : check each block of the named files against the level-0 SHA-1s (read-only)."""
import hashlib, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import stfs
s = stfs.Stfs(sys.argv[1])
entries = s.file_table()
for want in sys.argv[2:]:
    for e in entries:
        if e['directory'] or not s.path(entries, e).endswith(want):
            continue
        bad = []
        for b in s.chain(e['start'], e['blocks'], e['consecutive']):
            group_first = (b // stfs.PER_LEVEL) * stfs.PER_LEVEL
            table = s.backing(group_first) - s.tables_per_level
            s.f.seek(s.base + table * stfs.BLOCK + (b % stfs.PER_LEVEL) * 0x18)
            h = s.f.read(20)
            if hashlib.sha1(s.read_block(b)).digest() != h:
                bad.append(b)
        print(s.path(entries, e), e['blocks'], 'blocks,', len(bad), 'bad', bad[:10])
