# Installing C4IX onto a PC's disk

c4pc's ISO boots C4IX from the CD. `install` puts it on the PC's first
IDE disk, so that the PC (a VM: VMware, QEMU, VirtualBox; or a real one
with a BIOS) boots it from there with no CD, and the person's files are
kept on the disk.

**It erases the disk.** `install` says so, with the disk's size, and goes
on only when `yes` is typed.

## Doing it

Boot the ISO and pick "Install C4IX onto the IDE disk (erases it: asks
first)", or at any C4IX prompt booted from the ISO:

    install          # hd0; install hd1 for the second disk; -y asks nothing

It writes GRUB, partitions the disk, makes partition 1 ext2 and copies
c4pc and the kernel onto it, writes the system's files into partition 2,
and says when it is done. Then take the CD out (or put the disk first in
the VM's boot order) and `reboot`. The disk's own menu is the ISO's, from
the disk.

The disk must be at least the system's files (160 MB now) plus 64 MB to
spare plus 32 MB: 260 MB or so. A VM's default disk is far bigger.

## What is on the disk

| sectors | what |
|---|---|
| 0 | GRUB's boot.img (440 bytes), a disk signature, the partition table |
| 1 to 2047 | GRUB's core.img: the BIOS's disks, the MBR's partitions, ext2, multiboot, the menu's commands; its prefix `(hd0,msdos1)/boot/grub` |
| partition 1, type 0x83, active | ext2, label C4IX: `/boot` (c4pc.elf, c4pc64.elf, c4ix32.c4r, grub/grub.cfg) and `/home` |
| partition 2, type 0xDA | the ISO's disk.tar as it is, sector for sector, and room to spare |

Partition 2 is the boot drive: GRUB loads it by its block list,
`module (hd0,msdos2)0+SECTORS disk.tar drive`, as the ISO's GRUB loads
/boot/disk.tar. It is raw because C4IX's VFS holds a file it is writing
to ext2 in memory until it is closed (docs/c4ix-ext2.md 3), and the
system's files are 160 MB.

Partition 1's `/home` is the person's: the disk's menu boots C4IX with
`--home hd0p1`, and init mounts that partition's `/home` at `/home` once
the system's files are there (vfs.c vfs_mount_sub). What is saved there is
on the disk at once and there at the next boot. The rest of the tree is
the system's, from partition 2, in memory as it is when booted from the
CD: a change outside /home lasts until the machine is switched off.
`mount hd0p1 /mnt/disk` shows the whole partition, /boot too (unmount
/home first: a partition is mounted once).

GRUB's two images are made when the ISO is (Makefile, c4pc.iso):
grub-mkimage with the modules the menu needs, then pc/grubdisk.py sets
them as grub-bios-setup would for core.img from sector 1 -- boot.img's
boot-drive check made NOPs (a hard disk), diskboot's block list given
core.img's length. They are on the ISO as /boot/grub/c4pc-boot.img and
c4pc-core.img, with the disk's menu, /boot/grub/disk.cfg (pc/
grub-disk.cfg): install fills in its @DISK@, partition 2's block list,
and @HOME@, partition 1's name.

## Partitions in C4IX

The kernel reads each disk's MBR (blk.c): its four primary partitions,
not an extended partition's logical ones, nor a GPT. A device is a disk,
hd0 to hd7 (0 to 7), or a partition, hdDpP (8 + 4 * D + P - 1: hd0p1 is
8). A partition's sectors are numbered from its own first, and nothing
reaches past its last. `cat /proc/disks` lists them ("hd0p1 SECTORS rw
START TYPE"); `mount`, `mkfs`, `umount` and `df` take the names. A disk and
a partition of it are not mounted, or made, at once.

SYS_BLKIO (255) reads or writes up to 256 sectors of a device from a
task's buffer, through the kernel's own; a disk's sector 0 written reads
its partitions again. install.c is its user.

## Not yet

- / on the disk: the system's files in ext2, read when wanted instead of
  unpacked into memory at boot. Partition 2 would then be a filesystem
  too, or gone.
- Updating an installed disk from a newer ISO without erasing /home.
- SATA (AHCI) disks: C4IX sees IDE ones (pc/ata.c). A VM's disk must be
  on an IDE controller.
- A disk bigger than 2 TB (the MBR's limit), or a UEFI PC.

## Measured

2026-11-23, QEMU 10.2 with KVM on helios (itself a Hyper-V VM), the pc
machine, IDE disks:

- From the ISO's menu, install onto a blank 2 GB disk takes 13 s, with
  the disks and the CD by DMA (pc/ata.c). By PIO it would have taken
  hours: writes went at 38 KB a second, the CD at 90 KB.
- From Linux: the MBR has the two partitions, `e2fsck -fn` finds
  partition 1 clean, debugfs reads /boot/c4pc.elf back byte for byte, and
  partition 2 is disk.tar byte for byte.
- Booted from the disk alone, its text and desktop entries: /home is
  hd0p1's, a file saved there is there at the next boot, the power button
  (system_powerdown) shuts C4IX down and the VM off, and e2fsck finds the
  partition clean.
- From power on to /home mounted, on the desktop entry: about 70 s, most
  of it the BIOS reading partition 2 for GRUB (SeaBIOS's disk reads are
  PIO); from the ISO, 25 s.
- On c4pc-host (`--cdrom c4pc.iso --disk FILE`) install takes 1.2 s.

VMware not yet tried. Its disk must be on an IDE controller (VMware's
default for some guest types is SCSI, which C4IX does not see).
