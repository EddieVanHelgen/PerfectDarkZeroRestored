import struct, sys, capstone
import os
IMG_PATH = os.environ.get("PDZ_TU3_IMAGE", os.path.join(os.path.expanduser("~"), "Documents", "perfectdarkzerorecomp-tu3", "tu3_image.bin"))
IMG = open(IMG_PATH, "rb").read()  # 20 MB dump of the TU3 guest image at 0x82000000 (not in git)
BASE = 0x82000000
md = capstone.Cs(capstone.CS_ARCH_PPC, capstone.CS_MODE_32 | capstone.CS_MODE_BIG_ENDIAN)
a, e = int(sys.argv[1], 16), int(sys.argv[2], 16)
while a < e:
    s = next((f"{i.mnemonic:8s} {i.op_str}" for i in md.disasm(IMG[a-BASE:a-BASE+4], a)), f".long 0x{struct.unpack('>I', IMG[a-BASE:a-BASE+4])[0]:08X}")
    print(f"{a:08X}  {s}"); a += 4
