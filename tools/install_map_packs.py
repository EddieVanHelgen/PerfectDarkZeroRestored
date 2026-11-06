"""Install the official map packs (and Title Update 3's string tables) for pdz_dlc_maps.

usage: python -I tools/install_map_packs.py PACK [PACK ...] [--update TU_PACKAGE]
                                            [--root DIR] [--replace]

  PACK        a map pack's STFS package file (Map Pack One: 28A35330...,
              Map Pack Two: 7841AEFC...), as copied from your own Xbox 360.
  --update    your Title Update 3 package, copied from your own Xbox 360
              (any disc's: file names start with TU_16L61UJ_); only its generalTU1 /
              frontendTU1 string tables (pack one's map names) and its data
              package 1726605008.pkg (aid_darkpackage_titleupdate: the
              Township textures and the Township / Chinatown / Desert hit-fix
              props for pdz_fix_tu_township / pdz_fix_tu_mp_hitfix) are
              installed.
  --root      title folder to install into (default:
              %USERPROFILE%\\Documents\\perfectdarkzerorecomp\\0000000000000000\\4D5307D3).
  --replace   replace a pack / update folder that is already installed.

The packages are only read. Each pack is extracted to <root>\\00000002\\<package
file name>\\, the known damage in some copies of Map Pack One is repaired
(Plaza 1994253005.pkg: 1 byte, Trench 454830383.pkg: 2 bytes; see
docs/MAP_PACKS.md), then every file is checked against the package's SHA-1
block hashes and every .pkg must inflate with a valid Adler-32 trailer.
Exit code 1 if anything fails to verify (the folder is then left as <name>.failed).
"""
import argparse
import hashlib
import os
import shutil
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import stfs  # noqa: E402

# Damage found in Map Pack One copies (two copies tested, both
# identical): file name -> [(offset, damaged byte, original byte)]. Each fix
# was found by searching against the block's stored SHA-1, and with it the
# file's own Adler-32 trailer verifies.
REPAIRS = {
    '1994253005.pkg': [(6618007, 0x88, 0xAB)],                         # Plaza, block 8608
    '454830383.pkg': [(48589206, 0x00, 0x3A), (48589207, 0x00, 0x3E)],  # Trench, block 44745
}
UPDATE_TABLES = ('generalTU1', 'frontendTU1')
# The title update's data package (aid_darkpackage_titleupdate). pdz_dlc_maps
# registers only its hit-fix props and Township textures (src/mod/dlc_maps.cpp).
UPDATE_PACKAGE = ['Pdz', 'Assets', 'PackageData', '1726605008.pkg']


def block_hash(s, block):
    group_first = (block // stfs.PER_LEVEL) * stfs.PER_LEVEL
    table = s.backing(group_first) - s.tables_per_level
    s.f.seek(s.base + table * stfs.BLOCK + (block % stfs.PER_LEVEL) * 0x18)
    return s.f.read(20)


def verify(s, entry, data):
    """Blocks of data that don't match the package's hashes (the last block is
    padded with the package's own bytes past the end of the file)."""
    bad = []
    for i, b in enumerate(s.chain(entry['start'], entry['blocks'], entry['consecutive'])):
        chunk = data[i * stfs.BLOCK:(i + 1) * stfs.BLOCK]
        if len(chunk) < stfs.BLOCK:
            chunk += s.read_block(b)[len(chunk):]
        if hashlib.sha1(chunk).digest() != block_hash(s, b):
            bad.append(b)
    return bad


def repair(name, data):
    fixes = []
    for offset, damaged, original in REPAIRS.get(name, []):
        if offset < len(data) and data[offset] == damaged:
            data[offset] = original
            fixes.append(offset)
    return fixes


def install_pack(path, root, replace):
    s = stfs.Stfs(path)
    entries = s.file_table()
    files = {s.path(entries, e): e for e in entries if not e['directory']}
    if not any(p.lower() == 'pdz/dlc_contents/default.bin' for p in files):
        print(f'{path}: not a map pack (no Pdz/dlc_contents/default.bin)')
        return False
    name = os.path.basename(path)
    target = os.path.join(root, '00000002', name)
    print(f'{s.display_name}: {len(files)} files -> {target}')
    return extract(s, files, target, replace, name)


def install_update(path, root, replace):
    s = stfs.Stfs(path)
    entries = s.file_table()
    files = {}
    for e in entries:
        p = s.path(entries, e)
        parts = p.split('/')
        # Pdz/Assets/loc/<language>/aid/strings/text/<table>/default.str and
        # the data package only (never the update's default.xexp).
        if e['directory']:
            continue
        if ((len(parts) == 9 and parts[:3] == ['Pdz', 'Assets', 'loc'] and parts[7] in UPDATE_TABLES and
             parts[8] == 'default.str') or parts == UPDATE_PACKAGE):
            files[p] = e
    if not files:
        print(f'{path}: no generalTU1 / frontendTU1 string tables or data package in this package')
        return False
    if '/'.join(UPDATE_PACKAGE) not in files:
        print(f'  warning: {"/".join(UPDATE_PACKAGE)} not found; the title update fixes that need it stay off')
    name = os.path.basename(path)
    target = os.path.join(root, '000B0000', name)
    print(f'Title Update string tables and data package: {len(files)} files -> {target}')
    return extract(s, files, target, replace, name)


def extract(s, files, target, replace, name):
    if os.path.exists(target) and not replace:
        print(f'  already installed (use --replace to reinstall): {target}')
        return False
    work = target + '.new'
    if os.path.exists(work):
        shutil.rmtree(work)
    ok = True
    for p, e in sorted(files.items()):
        data = bytearray(s.read_file(e))
        base_name = os.path.basename(p)
        fixes = repair(base_name, data)
        bad = verify(s, e, data)
        if fixes:
            print(f'  {p}: repaired byte(s) at {", ".join(map(str, fixes))}')
        if bad:
            print(f'  {p}: {len(bad)} block(s) do not match the package hashes: {bad[:8]}')
            ok = False
        if base_name.lower().endswith('.pkg'):
            try:
                zlib.decompress(bytes(data))
            except zlib.error as err:
                print(f'  {p}: does not inflate ({err})')
                ok = False
        out = os.path.join(work, *p.split('/'))
        os.makedirs(os.path.dirname(out), exist_ok=True)
        with open(out, 'wb') as f:
            f.write(data)
    if not ok:
        failed = target + '.failed'
        if os.path.exists(failed):
            shutil.rmtree(failed)
        os.rename(work, failed)
        print(f'  NOT installed: verification failed; files left in {failed}')
        return False
    if os.path.exists(target):
        shutil.rmtree(target)
    os.rename(work, target)
    print('  installed, every file verified')
    return True


def main():
    default_root = os.path.join(os.path.expanduser('~'), 'Documents', 'perfectdarkzerorecomp',
                                '0000000000000000', '4D5307D3')
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('packs', nargs='*', help='map pack STFS package files')
    ap.add_argument('--update', help="Title Update 3 package (string tables and data package)")
    ap.add_argument('--root', default=default_root, help='title folder to install into')
    ap.add_argument('--replace', action='store_true', help='replace an installed pack / update folder')
    args = ap.parse_args()
    if not args.packs and not args.update:
        ap.error('give at least one map pack package or --update')
    ok = True
    for pack in args.packs:
        ok = install_pack(pack, args.root, args.replace) and ok
    if args.update:
        ok = install_update(args.update, args.root, args.replace) and ok
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
