"""Lift the launch exe's interactive music player into the Title Update 3 build.

Title Update 3 removed the interactive (stealth / action) music player and the
parts of the music manager that drive it (docs/BUG_AUDIT.md U5,
docs/TU3_BASE.md). This takes those functions from the launch build's
generated C++ (the launch checkout's generated/default, read only) and writes
src/music/music_lifted.inc for the TU3 build:

- each function becomes `static void mus_sub_<launch address>`; calls to other
  lifted functions are renamed, calls to functions TU3 kept (identical or
  offset-only) go to the TU3 function, anything else is an error;
- data addresses (lis-based) are remapped launch -> TU3 per instruction: the
  immediate of the instruction that forms the address from the lis register is
  adjusted (a CFG dataflow decides which register holds which lis value; a
  second "may" pass flags any use the exact pass couldn't resolve);
- FIELD_FIX rewrites struct offsets that moved in TU3 (found by aligning the
  launch functions TU3 still has against their TU3 versions).

Inputs are the reverse-engineering caches of the launch / TU3 comparison
(disassembly per function, launch -> TU3 function mapping); see the constants
below. Usage: python tools/lift_music.py LAUNCH_GENERATED_DIR OUT.inc
"""
import collections, glob, os, pickle, re, sys

# PDZ_RE_CACHE: the folder with your own launch / TU3 reverse-engineering caches (dlc2\cache_L.pkl, cache_T.pkl,
# tu3\diff\mapping.tsv), which this repository does not ship.
CACHE = os.path.expandvars(os.environ.get('PDZ_RE_CACHE', r'%LOCALAPPDATA%\pdz_re_cache'))
L = pickle.load(open(os.path.join(CACHE, 'dlc2', 'cache_L.pkl'), 'rb'))  # launch: {addr: [insn text]}
T = pickle.load(open(os.path.join(CACHE, 'dlc2', 'cache_T.pkl'), 'rb'))  # TU3

KIND = {}  # launch function -> (TU3 function or None, kind)
for line in open(os.path.join(CACHE, 'tu3', 'diff', 'mapping.tsv')):
    p = line.split()
    if len(p) >= 3 and p[0] != 'launch':
        KIND[int(p[0], 16)] = (None if p[1] == '-' else int(p[1], 16), p[2])
for line in open(os.path.join(CACHE, 'tu3', 'diff', 'removed_functions.txt')):
    if line.startswith('=== sub_'):
        KIND[int(line[8:16], 16)] = (None, 'removed')

# Entry points: level music setup, fade / start, stop, player tick, intensity,
# play tune, player constructor. Their callees are lifted when TU3 removed them.
ROOTS = [0x821F8450, 0x821F8570, 0x821F8600, 0x821F86C0, 0x821F8708, 0x821F8B28, 0x82200920]
# Changed in TU3 only by dropping the player: lift the launch version.
LIFT_CHANGED = {0x821F8570, 0x821F8600, 0x821F8708, 0x821F8B28}
# 821F8708 calls a member function through the object's vtable; TU3 does the same.
INDIRECT_OK = {0x821F8708}

# Music manager state (launch 82E67500, TU3 82E682C8, same layout; the player
# object is at +4 and TU3 still reserves it).
BLOCK_L, BLOCK_T, BLOCK_N = 0x82E67500, 0x82E682C8, 0xC0
# Checked by hand (string / aligned instruction in the TU3 version).
MANUAL = {0x82095FF8: 0x820941F0,  # 'XACT_SOUNDBANK_MUSIC_DEATH_DEATH_MAIN'
          0x82E2E620: 0x82E2F3F0}  # intensity timer (821F8984 = T 82200B9C)
# Struct offsets that moved in TU3: launch instruction -> (old, new).
FIELD_FIX = {
    0x821F83C8: (36, 40), 0x821F842C: (36, 40),  # game state: game mode
    0x821F8750: (36, 40), 0x821F89F8: (36, 40),
    0x821F87B4: (4588, 4700), 0x821F8A30: (4588, 4700),  # 82E2ED64 object: member
}

R = re.compile(r'^(\w+\.?) (.*)$')
VOLATILE = {'r0'} | {'r%d' % i for i in range(3, 13)}
CALLS = ('bl', 'bctrl', 'blrl')


def parse(x):
    m = R.match(x)
    if not m:
        return x.strip(), []
    return m.group(1), [s.strip() for s in m.group(2).split(',')]


def step(x, regs, may=False):
    """Apply instruction x to regs {reg: value-or-set}. Returns (use, regs).
    use = (address(es), base kind, base reg) for an address formed from a
    lis-derived register, or ('other',) for any other read of one."""
    regs = dict(regs)
    op, args = parse(x)
    use = None
    if op == 'lis' and len(args) == 2:
        v = ((int(args[1], 0) << 16) & 0xFFFFFFFF, 'lis')
        regs[args[0]] = {v} if may else v
        return None, regs
    mm = re.match(r'(-?(?:0x)?[0-9a-fA-F]+)\((r\d+)\)$', args[-1]) if args else None
    src = None
    if mm and mm.group(2) in regs:
        src, off = mm.group(2), int(mm.group(1), 0)
    elif op == 'addi' and len(args) == 3 and args[1] in regs:
        src, off = args[1], int(args[2], 0)
    if src:
        vals = regs[src] if may else {regs[src]}
        use = ({((v + off) & 0xFFFFFFFF, k) for v, k in vals}, src)
    elif any(a in regs for a in args[1:]) and not op.startswith('b'):
        use = ('other',)
    if op in CALLS:
        for r in VOLATILE:
            regs.pop(r, None)
    elif args and args[0] in regs and not op.startswith(('st', 'cmp', 'b')):
        regs.pop(args[0], None)
    if op == 'addi' and use and use[0] != 'other':
        derived = {(a, 'der:%08X' % a) for a, _ in use[0]}
        regs[args[0]] = derived if may else next(iter(derived))
    return use, regs


def successors(a, i, x, n):
    op, args = parse(x)
    tgt = None
    if args and args[-1].startswith('0x'):
        t = int(args[-1], 16)
        if a <= t < a + 4 * n:
            tgt = (t - a) // 4
    if op in ('blr', 'bctr'):
        return []
    if op == 'b':
        return [tgt] if tgt is not None else []  # local jump or tail call
    out = [i + 1] if i + 1 < n else []
    if tgt is not None and op not in CALLS:
        out.append(tgt)
    return out


def dataflow(a, ins, may):
    n = len(ins)
    IN = [None] * n
    IN[0] = {}
    work = [0]
    while work:
        i = work.pop()
        _, out = step(ins[i], IN[i], may)
        for j in successors(a, i, ins[i], n):
            if IN[j] is None:
                IN[j] = dict(out)
                work.append(j)
                continue
            if may:
                merged = dict(IN[j])
                for r, v in out.items():
                    merged[r] = merged.get(r, set()) | v
            else:
                merged = {r: v for r, v in IN[j].items() if out.get(r) == v}
            if merged != IN[j]:
                IN[j] = merged
                work.append(j)
    return [step(ins[i], IN[i], may)[0] if IN[i] is not None else None for i in range(n)]


def resolve(a, ins, errs):
    """per instruction: (launch address, kind, base reg) or None."""
    must = dataflow(a, ins, False)
    may = dataflow(a, ins, True)
    res = []
    for i, (u, w) in enumerate(zip(must, may)):
        if u and u[0] != 'other':
            (addr, kind), = u[0]
            res.append((addr, kind, u[1]))
            continue
        if w:
            errs.append('%08X %s: lis-derived register use not resolved (%s)' % (a + 4 * i, ins[i], w))
        res.append(None)
    return res


def build_datamap():
    """launch -> TU3 data addresses from instruction-aligned identical/offset-only functions."""
    M = collections.defaultdict(collections.Counter)
    for la, (ta, kind) in KIND.items():
        if kind not in ('identical', 'offsets') or la not in L or ta not in T or len(L[la]) != len(T[ta]):
            continue
        dl, dt = dataflow(la, L[la], False), dataflow(ta, T[ta], False)
        for x, y in zip(dl, dt):
            if x and y and x[0] != 'other' and y[0] != 'other':
                (xa, _), = x[0]
                (ya, _), = y[0]
                if xa >= 0x82000000:
                    M[xa][ya] += 1
    return M


def main():
    gen_dir, out_path = sys.argv[1], sys.argv[2]
    errs = []
    dmap = build_datamap()

    def map_data(x):
        if BLOCK_L <= x < BLOCK_L + BLOCK_N:
            return x - BLOCK_L + BLOCK_T, 'block'
        if x in MANUAL:
            return MANUAL[x], 'manual'
        if x in dmap:
            t, n = dmap[x].most_common(1)[0]
            return t, 'aligned x%d' % n
        return None, None

    # lift set
    lift, ext = [], {}
    q = list(ROOTS)
    while q:
        a = q.pop(0)
        if a in lift:
            continue
        lift.append(a)
        for i, x in enumerate(L[a]):
            op, args = parse(x)
            if op not in ('bl', 'b') or not args or not args[-1].startswith('0x'):
                continue
            c = int(args[-1], 16)
            if a <= c < a + 4 * len(L[a]) or 0x82A98000 <= c < 0x82A99000:
                continue  # local branch / register save-restore helper (named by role)
            t, kind = KIND.get(c, (None, 'unknown'))
            if kind == 'removed' or c in LIFT_CHANGED:
                q.append(c)
            elif kind in ('identical', 'offsets'):
                ext[c] = t
            else:
                errs.append('callee %08X of %08X is %s' % (c, a, kind))
    lift.sort()

    sources = {}
    for f in glob.glob(os.path.join(gen_dir, 'perfectdarkzerorecomp_recomp.*.cpp')):
        s = open(f, encoding='utf-8').read()
        for a in lift:
            i = s.find('DEFINE_REX_FUNC(sub_%08X) {' % a)
            if i >= 0:
                sources[a] = s[i:s.find('\n}\n', i) + 3]

    report = collections.Counter()
    body = []
    for a in lift:
        name = 'sub_%08X' % a
        if a not in sources:
            errs.append('no generated C++ for ' + name)
            continue
        ins = L[a]
        res = resolve(a, ins, errs)
        k = -1
        pending = None  # [reg fix or None, field fix or None, addr, text]
        new = []
        for line in sources[a].split('\n'):
            if line.startswith('DEFINE_REX_FUNC('):
                new.append('static void mus_%s(PPCContext& __restrict ctx, uint8_t* base) {' % name)
                continue
            m = re.match(r'^\t// (.*)$', line)
            if m:
                if pending and (pending[0] or pending[1]):
                    errs.append('%08X %s: fix not applied' % (pending[2], pending[3]))
                k += 1
                txt = m.group(1).strip()
                if k >= len(ins) or txt.split()[0] != ins[k].split()[0]:
                    errs.append('%s: comment %d %r does not match %r' % (name, k, txt, ins[k] if k < len(ins) else None))
                addr = a + 4 * k
                fix = None
                if res[k]:
                    la, kind, reg = res[k]
                    ta, how = map_data(la)
                    if ta is None:
                        errs.append('%08X %s: %08X has no TU3 address' % (addr, txt, la))
                    elif kind == 'lis':
                        fix = (reg, ta - la)
                        report['%08X -> %08X (%s)' % (la, ta, how)] += 1
                    else:
                        bl_ = int(kind.split(':')[1], 16)
                        bt_, _ = map_data(bl_)
                        if bt_ is None:
                            errs.append('%08X %s: base %08X has no TU3 address' % (addr, txt, bl_))
                        elif ta - la != bt_ - bl_:
                            fix = (reg, (ta - la) - (bt_ - bl_))
                            report['%08X -> %08X (%s, via %08X)' % (la, ta, how, bl_)] += 1
                pending = [fix, FIELD_FIX.get(addr), addr, txt]
                new.append(line)
                continue
            if pending and not line.startswith('loc_'):
                if pending[0]:
                    reg, d = pending[0]
                    pat = r'(ctx\.%s\.(?:u32|s64) \+ )(-?\d+)' % reg
                    if re.search(pat, line):
                        line = re.sub(pat, lambda mm: '%s%d' % (mm.group(1), int(mm.group(2)) + d), line, count=1)
                        pending[0] = None
                if pending[1]:
                    old, nv = pending[1]
                    pat = r'(\.(?:u32|s64) \+ )%d\b' % old
                    if re.search(pat, line):
                        line = re.sub(pat, r'\g<1>%d' % nv, line, count=1)
                        report['field %08X: +%d -> +%d' % (pending[2], old, nv)] += 1
                        pending[1] = None

            def rename(mm):
                c = int(mm.group(1), 16)
                if c in lift:
                    return 'mus_sub_%08X(' % c
                if c in ext:
                    return 'sub_%08X(' % ext[c]
                errs.append('%s calls %08X (not lifted, no TU3 function)' % (name, c))
                return mm.group(0)
            line = re.sub(r'\bsub_([0-9A-F]{8})\(', rename, line)
            if 'CALL_INDIRECT' in line and a not in INDIRECT_OK:
                errs.append('%s: indirect call' % name)
            new.append(line)
        if pending and (pending[0] or pending[1]):
            errs.append('%08X %s: fix not applied' % (pending[2], pending[3]))
        if k + 1 != len(ins):
            errs.append('%s: %d instruction comments, %d instructions' % (name, k + 1, len(ins)))
        body.append('\n'.join(new))

    text = '\n'.join(body)
    hdr = ["// Generated by tools/lift_music.py (do not edit): the launch exe's interactive",
           "// music player and music manager, taken from the launch build's generated C++",
           '// and retargeted to Title Update 3 (callees renamed to their TU3 functions,',
           '// data addresses and moved struct offsets remapped). Included by',
           '// src/music/music_restore.cpp. Function names keep the launch address.',
           '#include "perfectdarkzerorecomp_pch.h"', '']
    for h in sorted(set(re.findall(r'\b(__(?:save|rest)\w+_\d+)\(', text))):
        hdr.append('DECLARE_REX_FUNC(%s);' % h)
    for c, t in sorted(ext.items()):
        hdr.append('DECLARE_REX_FUNC(sub_%08X); // launch %08X' % (t, c))
    hdr.append('')
    for a in lift:
        hdr.append('static void mus_sub_%08X(PPCContext& __restrict ctx, uint8_t* base);' % a)
    hdr.append('')
    print('lifted %d functions: %s' % (len(lift), ' '.join('%08X' % a for a in lift)))
    print('TU3 callees: %d' % len(ext))
    for r, n in sorted(report.items()):
        print('  %s x%d' % (r, n))
    for e in errs:
        print('ERROR', e)
    if errs:
        sys.exit(1)
    open(out_path, 'w', newline='\n').write('\n'.join(hdr) + '\n' + text)


main()
