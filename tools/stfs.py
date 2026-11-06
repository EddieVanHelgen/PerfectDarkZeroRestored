"""Minimal read-only STFS (LIVE/PIRS/CON) reader and extractor.

usage: python stfs.py <package> <outdir>
Opens the package read-only, prints the header and file table, extracts
every file under <outdir>.
"""
import os
import struct
import sys

BLOCK = 0x1000
PER_LEVEL = 170


def be32(b, o):
    return struct.unpack_from('>I', b, o)[0]


def int24_le(b, o):
    return b[o] | (b[o + 1] << 8) | (b[o + 2] << 16)


def int24_be(b, o):
    return (b[o] << 16) | (b[o + 1] << 8) | b[o + 2]


class Stfs:
    def __init__(self, path):
        self.f = open(path, 'rb')  # read-only
        h = self.f.read(0xA000)
        self.h = h
        self.magic = h[0:4].decode('latin1')
        self.header_size = be32(h, 0x340)
        self.content_type = be32(h, 0x344)
        self.metadata_version = be32(h, 0x348)
        self.content_size = struct.unpack_from('>Q', h, 0x34C)[0]
        self.media_id = be32(h, 0x354)
        self.version = be32(h, 0x358)
        self.base_version = be32(h, 0x35C)
        self.title_id = be32(h, 0x360)
        vd = 0x379
        self.vd_size = h[vd]
        self.block_separation = h[vd + 2]
        self.ft_block_count = struct.unpack_from('<H', h, vd + 3)[0]
        self.ft_block = int24_le(h, vd + 5)
        self.total_alloc = be32(h, vd + 0x1C)
        self.total_unalloc = be32(h, vd + 0x20)
        self.data_file_count = be32(h, 0x39D)
        self.display_name = h[0x411:0x411 + 0x80].decode('utf-16-be', 'replace').split('\0')[0]
        self.description = h[0xD11:0xD11 + 0x80].decode('utf-16-be', 'replace').split('\0')[0]
        self.title_name = h[0x1691:0x1691 + 0x80].decode('utf-16-be', 'replace').split('\0')[0]
        self.read_only = bool(self.block_separation & 1)
        self.tables_per_level = 1 if self.read_only else 2
        self.base = (self.header_size + BLOCK - 1) & ~(BLOCK - 1)

    def backing(self, block):
        """Data block number -> backing block number (hash tables interleaved)."""
        result = block
        base = PER_LEVEL
        for _ in range(3):
            result += ((block + base) // base) * self.tables_per_level
            if block < base:
                break
            base *= PER_LEVEL
        return result

    def read_block(self, block):
        self.f.seek(self.base + self.backing(block) * BLOCK)
        return self.f.read(BLOCK)

    def hash_entry(self, block):
        """Level-0 hash entry for a data block: (status, next block)."""
        group_first = (block // PER_LEVEL) * PER_LEVEL
        table = self.backing(group_first) - self.tables_per_level
        self.f.seek(self.base + table * BLOCK + (block % PER_LEVEL) * 0x18)
        e = self.f.read(0x18)
        return e[0x14], int24_be(e, 0x15)

    def chain(self, start, count, consecutive):
        blocks = []
        b = start
        for i in range(count):
            blocks.append(b)
            if consecutive:
                b += 1
            else:
                b = self.hash_entry(b)[1]
        return blocks

    def file_table(self):
        data = b''.join(self.read_block(b) for b in self.chain(self.ft_block, self.ft_block_count, False))
        entries = []
        for i in range(0, len(data), 0x40):
            e = data[i:i + 0x40]
            name_len = e[0x28] & 0x3F
            if name_len == 0 or e[0] == 0:
                continue
            entries.append({
                'index': i // 0x40,
                'name': e[:name_len].decode('latin1'),
                'directory': bool(e[0x28] & 0x80),
                'consecutive': bool(e[0x28] & 0x40),
                'blocks': int24_le(e, 0x29),
                'start': int24_le(e, 0x2F),
                'parent': struct.unpack_from('>h', e, 0x32)[0],
                'size': be32(e, 0x34),
            })
        return entries

    def path(self, entries, entry):
        by_index = {e['index']: e for e in entries}
        parts = [entry['name']]
        p = entry['parent']
        while p != -1 and p in by_index:
            parts.append(by_index[p]['name'])
            p = by_index[p]['parent']
        return '/'.join(reversed(parts))

    def read_file(self, e):
        out = bytearray()
        for b in self.chain(e['start'], e['blocks'], e['consecutive']):
            out += self.read_block(b)
        return bytes(out[:e['size']])


def main():
    pkg, outdir = sys.argv[1], sys.argv[2]
    s = Stfs(pkg)
    print(f"magic {s.magic} type {s.content_type:08X} title {s.title_id:08X} metadata v{s.metadata_version} "
          f"version {s.version:08X} base {s.base_version:08X} header {s.header_size:#x}")
    print(f"name: {s.display_name} | title: {s.title_name}")
    print(f"description: {s.description}")
    print(f"volume: block separation {s.block_separation} (read-only format {s.read_only}), file table "
          f"{s.ft_block_count} blocks at {s.ft_block}, allocated {s.total_alloc}, unallocated {s.total_unalloc}, "
          f"content size {s.content_size}")
    entries = s.file_table()
    os.makedirs(outdir, exist_ok=True)
    for e in entries:
        p = s.path(entries, e)
        if e['directory']:
            print(f"  [dir] {p}")
            os.makedirs(os.path.join(outdir, p), exist_ok=True)
            continue
        print(f"  {e['size']:>11}  {p}  (start {e['start']}, {e['blocks']} blocks, consecutive {e['consecutive']})")
        target = os.path.join(outdir, p)
        os.makedirs(os.path.dirname(target) or outdir, exist_ok=True)
        with open(target, 'wb') as f:
            f.write(s.read_file(e))


if __name__ == '__main__':
    main()
