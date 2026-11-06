"""tu3dis.py ADDR [BEFORE AFTER]: disassemble the TU3 image around ADDR (walk back to the prologue, forward to blr)."""
import struct, sys, capstone
import os
IMG_PATH = os.environ.get("PDZ_TU3_IMAGE", os.path.join(os.path.expanduser("~"), "Documents", "perfectdarkzerorecomp-tu3", "tu3_image.bin"))
IMG = open(IMG_PATH, "rb").read()  # 20 MB dump of the TU3 guest image at 0x82000000 (not in git)
BASE = 0x82000000
md = capstone.Cs(capstone.CS_ARCH_PPC, capstone.CS_MODE_32 | capstone.CS_MODE_BIG_ENDIAN)
def u32(a): return struct.unpack(">I", IMG[a-BASE:a-BASE+4])[0]
def dis(a):
    for i in md.disasm(IMG[a-BASE:a-BASE+4], a):
        return f"{i.mnemonic:8s} {i.op_str}"
    return f".long 0x{u32(a):08X}"
addr = int(sys.argv[1], 16)
before = int(sys.argv[2]) if len(sys.argv) > 2 else 400
after = int(sys.argv[3]) if len(sys.argv) > 3 else 400
# walk back to a prologue: 'mflr r12' or 'stwu r1' preceded by blr/nop region
start = addr
a = addr
while addr - a < before:
    s = dis(a)
    if s.startswith("mflr") or (s.startswith("stwu") and "r1," in s):
        start = a
        if s.startswith("mflr"):
            break
    a -= 4
a = start
end = start
while a - start < before + after:
    s = dis(a)
    print(f"{'>>' if a == addr else '  '} {a:08X}  {s}")
    if s.startswith("blr") and a >= addr:
        break
    a += 4
