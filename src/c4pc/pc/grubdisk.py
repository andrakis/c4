#!/usr/bin/env python3
# grubdisk.py BOOT.IMG CORE.IMG OUTDIR -- GRUB's two images for a disk C4IX
# is installed on (src/c4ix/user/install.c), set as grub-bios-setup sets
# them for core.img in the sectors from 1 (docs/c4ix-install.md):
#
#   c4pc-boot.img  boot.img, its boot-drive check made two NOPs (a hard
#                  disk: the BIOS's DL is trusted, as grub-bios-setup does
#                  for one); its kernel sector is 1 already. install.c
#                  takes its first 440 bytes and adds the partition table.
#   c4pc-core.img  core.img, diskboot's block list (the rest of core.img,
#                  from sector 2) given its length in sectors.
import sys

boot = bytearray(open(sys.argv[1], "rb").read())
core = bytearray(open(sys.argv[2], "rb").read())
out = sys.argv[3]
assert len(boot) == 512 and boot[510:512] == b"\x55\xaa", "boot.img is not GRUB's"
# GRUB_BOOT_MACHINE_KERNEL_SECTOR (0x5c): core.img's first sector, 1
assert int.from_bytes(boot[0x5C:0x64], "little") == 1, "boot.img's kernel sector is not 1"
# GRUB_BOOT_MACHINE_DRIVE_CHECK (0x66): a jmp short over the check
assert boot[0x66] == 0x74 or boot[0x66] == 0xEB or boot[0x66] == 0x90, "boot.img's drive check is not where it was"
boot[0x66] = 0x90
boot[0x67] = 0x90
# diskboot.img's block list, at the end of core.img's first sector: start
# (8 bytes), length (2), segment (2)
n = (len(core) + 511) // 512
assert n <= 2047, "core.img is %d sectors: more than fit before sector 2048" % n
start = int.from_bytes(core[0x1F4:0x1FC], "little")
assert start == 2, "core.img's block list starts at %d, not 2" % start
core[0x1FC:0x1FE] = (n - 1).to_bytes(2, "little")
core += b"\0" * (n * 512 - len(core))
open(out + "/c4pc-boot.img", "wb").write(boot)
open(out + "/c4pc-core.img", "wb").write(core)
print("grubdisk: core.img %d sectors, boot.img for a hard disk" % n)
