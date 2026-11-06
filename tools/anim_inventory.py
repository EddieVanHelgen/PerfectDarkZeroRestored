"""Inventory of every animation in the game's packages (read-only).

Usage: python tools/anim_inventory.py [PKGDIR] [--image TU3_IMAGE.bin]

PKGDIR defaults to assets/pdz/ASSETS/PackageData. Packages are inflated in
memory only. Writes:
- tools/anim_inventory.csv: one row per animation (aid_anim_*): group,
  packages and levels it ships in, what names it (group tables, level lists,
  prop interactions, weapon tables, prop setups; "exe" when the executable
  names it, needs --image, a raw memory image of the TU3 executable), the
  AnimType / AnimHanded of its group-table entries, and a note for the ones
  that are named but may never play (docs/SP_INVENTORY.md, "Animations in
  depth");
- src/mod/anim_inventory.inc: the character animations (aid_anim_chars_*)
  for pdz_debug_anim_viewer, with the suspect rank.

Package format (CAFF "darkpackage", inflated): .data at 0x180; directory at
data+0x20: count, then 0x20-byte entries (+0 name offset, +4 type offset,
data-relative). Animations are directory names only (no type); "xml" records
follow their name with the source path and the table. Tables name animations
as strings, either "aid_anim_chars_x" or the path form "aid\\anim\\guns\\x";
group-table entries are (0xc, L, size, L, fields, name) where fields are
[name offset, speed, AnimType, blend, mirror, ..., AnimHanded << 24 last].
Every runtime entry is rebuilt by sub_821E8A18 [name, speed, type, blend,
flags, handle at +20], so the logger and the viewer read names from +0.
"""
import collections
import csv
import os
import re
import struct
import sys
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASE = 0x180

# exe enum 82D22B90 (AnimType_*), in order.
ANIM_TYPES = (
    'Hits_Min Hits_Chest Hits_Back Hits_Stomach Hits_Arse Hits_Head Hits_Head_Back Hits_Head_Left Hits_Head_Right '
    'Hits_LeftShoulder Hits_LeftShoulder_Back Hits_LeftShoulder_Side Hits_RightShoulder Hits_RightShoulder_Back '
    'Hits_RightShoulder_Side Hits_LeftUpperArm Hits_LeftUpperArm_Back Hits_RightUpperArm Hits_RightUpperArm_Back '
    'Hits_LeftForeArm Hits_LeftForeArm_Back Hits_RightForeArm Hits_RightForeArm_Back Hits_LeftHand Hits_LeftHand_Back '
    'Hits_RightHand Hits_RightHand_Back Hits_LeftThigh Hits_LeftThigh_Back Hits_RightThigh Hits_RightThigh_Back '
    'Hits_LeftCalf Hits_LeftCalf_Back Hits_RightCalf Hits_RightCalf_Back Hits_LeftFoot Hits_RightFoot Hits_Groin '
    'Hits_Max Deaths_Min Deaths_General Deaths_Chest Deaths_Back Deaths_Stomach Deaths_Arse Deaths_Head '
    'Deaths_Head_Back Deaths_Head_Left Deaths_Head_Right Deaths_LeftShoulder Deaths_LeftShoulder_Back '
    'Deaths_LeftShoulder_Side Deaths_RightShoulder Deaths_RightShoulder_Back Deaths_RightShoulder_Side Deaths_LeftArm '
    'Deaths_LeftArm_Back Deaths_RightArm Deaths_RightArm_Back Deaths_LeftLeg Deaths_LeftLeg_Back Deaths_RightLeg '
    'Deaths_RightLeg_Back Deaths_LeftFoot Deaths_RightFoot Deaths_Explosion Deaths_Explosion_Front '
    'Deaths_Explosion_Left_Front Deaths_Explosion_Left Deaths_Explosion_Left_Back Deaths_Explosion_Back '
    'Deaths_Explosion_Right_Back Deaths_Explosion_Right Deaths_Explosion_Right_Front Deaths_Max Gun_Min Gun_Raise '
    'Gun_ShootSingle Gun_ShootMultiple Gun_Lower Gun_Aim_Idle Gun_Aim_Relaxed Gun_IntoPosition Gun_OutOfPosition '
    'Gun_PositionIdle Gun_Max General_Min Crouch_Idle Crouch_Stance Crouch_Walk Crouch_Jog Crouch_Run Crouch_Sprint '
    'Crouch_WalkBackwards Crouch_JogBackwards Crouch_StrafeLeft Crouch_StrafeRight Crouch_Turn180Left '
    'Crouch_Turn180Right Crouch_Turn45Left Crouch_Turn45Right Crouch_Turn90Left Crouch_Turn90Right '
    'Crouch_TurnOnTheSpot Motion_Min Idle Stance Walk Jog Run Sprint SprintUnarmed WalkBackwards JogBackwards '
    'RunBackwards StrafeLeft StrafeRight SprintStrafeLeft SprintStrafeRight Turn180Left Turn180Right Turn45Left '
    'Turn45Right Turn90Left Turn90Right TurnOnTheSpot WalkAim Motion_Max Punch Reload CrouchReload IntoCrouch '
    'OutofCrouch CrouchIntoWalk Comedy Search_Look Surprised Surprised_Left Surprised_Right Surprised_Behind '
    'Dodge_Forwards Dodge_Crouch_Forwards Dodge_Crouch_Back Dodge_Back CloseRangeAttack PressTheAlarmButton '
    'FacingFront_Dodge_Forwards FacingFront_Dodge_Left FacingFront_Dodge_Right FacingLeft_Dodge_Forwards '
    'FacingLeft_Dodge_Backwards FacingRight_Dodge_Forwards FacingRight_Dodge_Backwards FacingBack_Dodge_Forwards '
    'FacingBack_Dodge_Left FacingBack_Dodge_Right Moving_Dodge_Forwards Moving_Dodge_Backwards IdleTimeout '
    'IdleToWalk IdleToSprint WalkToIdle SprintToIdle Crouched_IdleTimeout Crouched_IdleToWalk Crouched_IdleToSprint '
    'Crouched_WalkToIdle Crouched_SprintToIdle Skid180 Ladder_BottomClimbOn Ladder_BottomClimbOff Ladder_ClimbLoop '
    'Ladder_TopClimbOn Ladder_TopClimbOff Ladder_IntoSlide Ladder_SlideEnd Ladder_IdleLeftHand Ladder_IdleRightHand '
    'Rail_GetOn Rail_GetOff Rail_Slide Rail_Hit Rail_Death Grenade_PullPin Grenade_Throw Continuous_Idle1 '
    'Continuous_Idle2 Continuous_Idle3 Continuous_Idle4 Continuous_Idle5 Fall_Min Fall_Jump Fall_Fall Fall_Land '
    'Fall_Recover Fall_Max Interaction_Min Interaction_Idle Interaction_In Interaction_Out Interaction_Death '
    'Interaction_Hit Interaction_Max Climb_Low Climb_High Blinded Tranquilised Revived ReviveIdlePose General_Max '
    'Characters_Max HUD_Min HUD_Entry HUD_Exit HUD_Max Spiderbot_Min Spiderbot_Sleep Spiderbot_SleepToAwake '
    'Spiderbot_Idle Spiderbot_Walk Spiderbot_Jog Spiderbot_Run Spiderbot_IntoAttack Spiderbot_AttackLoop '
    'Spiderbot_OutOfAttack Spiderbot_Jump Spiderbot_Land Spiderbot_Struggle Spiderbot_Upright Spiderbot_UprightTurn '
    'Spiderbot_Max Unknown AnyHits Max').split()
# exe enum 82D22F38 (AnimHanded_*).
HANDED = ['One', 'Two', 'One_and_Two', 'One_Aiming', 'Two_Aiming', 'One_and_Two_Aiming']

# Packages by in-game name (docs/SP_INVENTORY.md, docs/MP_INVENTORY.md).
PACKAGES = {
    '1708969700': 'Datacore', '1624233953': 'Nightclub', '1959974102': 'Subway (mission)',
    '1876152804': 'Rooftops', '1793118422': 'Mansion', '1776075990': 'Laboratory', '1591596254': 'River',
    '1617794024': 'Trinity | Infiltration', '1617794280': 'Trinity | Escape', '1742000362': 'Jungle',
    '1876675562': 'Temple (mission)', '1910233572': 'Outpost', '1607584743': 'Bridge', '1624430806': 'Arena',
    '119480370': 'SUBWAY', '415281596': 'URBAN', '471675425': 'DESERT', '722675495': 'TEMPLE',
    '774843938': 'OLD TOWN', '1499410256': 'TOWER', '1969908429': 'shared character animations',
}

# Named but maybe never played: (pattern, rank, note). Rank 1 = can't play in
# shipped content; higher = rarer conditions. See docs/SP_INVENTORY.md.
SUSPECTS = [
    # zipline_* (Rail_* types) play on the shipped rail-slides: not a suspect (corrected).
    (r'^aid_anim_chars_vehicles_van_lev3van1', 1, 'Rooftops level list only; the script plays van 2 by name'),
    (r'^aid_anim_chars_fight_', 2, 'close-range attack (unarmed 3rd-person slots); needs an unarmed fighter'),
    (r'^aid_anim_chars_interactions_clipboard_', 2, 'Trinity | Infiltration clipboard interaction'),
    (r'^aid_anim_chars_interactions_bar_', 2, 'Nightclub bar-stool interaction'),
    (r'^aid_anim_chars_dodges_', 3, 'dodge task (FacingRight pair only with pdz_fix_ai_dodge_facing)'),
    (r'^aid_anim_chars_others_crouch_strafe_', 4, 'female crouch-strafe (the only one); male only while aiming'),
    (r'^aid_anim_chars_cover_crate_[a-z0-9]+_2[14]_?over', 3, 'over-the-crate cover group'),
    (r'^aid_anim_chars_cover_table_', 3, 'table cover group'),
    (r'^aid_anim_chars_ladder_', 4, 'ladders: third-person climbing (multiplayer, co-op)'),
    (r'^aid_anim_chars_idle_male_mp5_(?!21standing)', 4, 'Comedy idles: male, rifle, standing still, unalerted'),
    (r'^aid_anim_chars_movement_male_(blinded|drugged)_', 4, 'flashbang / tranquilliser'),
    (r'^aid_anim_chars_movement_male_action_climb', 4, 'Climb_Low / Climb_High'),
    (r'^aid_anim_chars_vehicles_dropship_jump1', 4, 'Fall_Fall / Fall_Land (dropship jump)'),
]


def inflate(data):
    parts, pos = [], 0
    while pos < len(data):
        z = zlib.decompressobj()
        try:
            buf = data[pos:]
            while buf:
                parts.append(z.decompress(buf, 16 << 20))
                buf = z.unconsumed_tail
        except zlib.error:
            break
        if not z.unused_data:
            break
        pos = len(data) - len(z.unused_data)
    return b''.join(parts)


def u32(d, a):
    return struct.unpack_from('>I', d, a)[0]


def cstr(d, a):
    return d[a:d.find(b'\0', a)].decode('latin-1')


def directory(d):
    n = u32(d, BASE + 0x20)
    out = []
    for i in range(n):
        e = BASE + 0x24 + i * 0x20
        no, to = u32(d, e), u32(d, e + 4)
        if no:
            out.append((cstr(d, BASE + no), cstr(d, BASE + to) if to else '', BASE + no))
    return out


ANIM = re.compile(rb'(?<!uberroot.)aid[_\\]anim[_\\][A-Za-z0-9_\\]+')


def entry_fields(d, n):
    for L in range(0x8, 0x60, 4):
        e = n - 0xc - L
        if e < 0:
            break
        if u32(d, e) == 0xc and u32(d, e + 4) == L and u32(d, e + 0xc) == L:
            return struct.unpack_from('>%dI' % (L // 4), d, e + 0xc)
    return None


def ref_kind(record):
    k = record.lower()
    k = re.sub(r'^aid_xml_', '', k)
    k = re.sub(r'^propsetup_(single|multi)_([a-z0-9]+)_([a-z]+)_.*', r'propsetup_\1_\2_\3', k)
    return k


def group(name):
    p = name[len('aid_anim_'):].split('_')
    return p[0] + '/' + p[1] if p[0] == 'chars' and len(p) > 1 else p[0]


def main():
    args = sys.argv[1:]
    image = None
    if '--image' in args:
        i = args.index('--image')
        image = args[i + 1]
        del args[i:i + 2]
    pkgdir = args[0] if args else os.path.join(ROOT, 'assets', 'pdz', 'ASSETS', 'PackageData')
    pkgs = collections.defaultdict(set)
    refs = collections.defaultdict(set)
    types = collections.defaultdict(set)
    spelling = {}
    for f in sorted(os.listdir(pkgdir)):
        if not f.endswith('.pkg'):
            continue
        d = inflate(open(os.path.join(pkgdir, f), 'rb').read())
        if not d.startswith(b'CAFF'):
            continue
        pid = f[:-4]
        dirs = sorted(directory(d), key=lambda x: x[2])
        for i, (name, t, a) in enumerate(dirs):
            low = name.lower()
            if low.startswith('aid_anim_'):
                if low != 'aid_anim__noname_':
                    pkgs[low].add(pid)
                    spelling.setdefault(low, name)
                continue
            end = dirs[i + 1][2] if i + 1 < len(dirs) else len(d)
            for m in ANIM.finditer(d, a + 1, end):
                n = m.group().decode().replace('\\', '_').lower()
                refs[n].add(ref_kind(name))
                fields = entry_fields(d, m.start())
                if fields and len(fields) >= 6 and fields[2] < len(ANIM_TYPES):
                    # AnimHanded is the last word's top byte (7+ fields; hit tables have 6, no hand).
                    handed = fields[-1] >> 24 if len(fields) >= 7 else None
                    hand = '' if handed is None else '/' + (HANDED[handed] if handed < len(HANDED) else str(handed))
                    types[n].add(ANIM_TYPES[fields[2]] + hand)
        print(f, len(pkgs), file=sys.stderr, flush=True)
    exe = set()
    if image:
        img = open(image, 'rb').read()
        exe = set(m.group().decode().lower() for m in re.finditer(rb'aid_anim_[A-Za-z0-9_]+', img))
    rows = []
    for n in sorted(pkgs):
        note, rank = '', 0
        for pat, r, text in SUSPECTS:
            if re.search(pat, n):
                note, rank = text, r
                break
        named = sorted(refs.get(n, ()))
        levels = sorted(set(PACKAGES[p] for p in pkgs[n] if p in PACKAGES))
        rows.append({
            'name': n, 'group': group(n), 'packages': ' '.join(sorted(pkgs[n])), 'levels': '; '.join(levels),
            'exe': 'yes' if n in exe else '', 'named_by': ' '.join(named), 'anim_types': ' '.join(sorted(types.get(n, ()))),
            'suspect_rank': rank or '', 'note': note,
            'status': 'exe' if n in exe else ('table' if named else ('unknown' if not image else 'never named')),
        })
    out = os.path.join(ROOT, 'tools', 'anim_inventory.csv')
    with open(out, 'w', newline='', encoding='utf-8') as fh:
        w = csv.DictWriter(fh, fieldnames=list(rows[0]))
        w.writeheader()
        w.writerows(rows)
    inc = os.path.join(ROOT, 'src', 'mod', 'anim_inventory.inc')
    with open(inc, 'w', newline='\n', encoding='utf-8') as fh:
        fh.write('// Generated by tools/anim_inventory.py: character animations for\n'
                 '// pdz_debug_anim_viewer (name, suspect rank: 0 none, 1 can\'t play in shipped\n'
                 '// content ... 4 rare; see docs/SP_INVENTORY.md "Animations in depth").\n')
        for r in rows:
            if r['name'].startswith('aid_anim_chars_'):
                fh.write('{"%s", %d},\n' % (spelling[r['name']], r['suspect_rank'] or 0))
    c = collections.Counter(r['status'] for r in rows)
    print('%d animations: %s' % (len(rows), dict(c)))


if __name__ == '__main__':
    main()
