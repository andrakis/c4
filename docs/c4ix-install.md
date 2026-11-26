# Installing C4IX onto a PC's disk

c4pc's ISO boots C4IX from the CD. `install` puts it on the PC's first
IDE disk, so that the PC (a VM: VMware, QEMU, VirtualBox; or a real one
with a BIOS) boots it from there with no CD. The whole system is then on
the disk: `/` is the disk's ext2, nothing is unpacked into memory at
boot, and every change is kept.

**It erases the disk.** `install` says so, with the disk's size, and goes
on only when `yes` is typed.

## Doing it

Boot the ISO and pick "Install C4IX onto the IDE disk (erases it: asks
first)", or at any C4IX prompt booted from the ISO:

    install          # hd0; install hd1 for the second disk; -y asks nothing

It installs the system that is running: the one the ISO's menu started.
It waits for the boot to finish unpacking the root archive, then writes
GRUB, partitions the disk, makes partition 1 ext2, copies the whole tree
onto it, then c4pc, the kernel and the disk's menu into `/boot`, and says
when it is done. Then take the CD out (or put the disk first in the VM's
boot order) and `reboot`. The disk's own menu is the ISO's, from the
disk.

The desktop has an Install C4IX icon, and its Go menu an "Install
C4IX..." item, when it was booted from the ISO and / is not on a disk
already; either runs `install` in a terminal.

A disk persist keeps its store on (c4pc's drive 1: a blank disk the ISO
booted with, or one `--store` made) is no block device, so there is no
hd0 at first. `install` then names the store's disk instead, warns that
the store's saved files go with it, and after "yes" has c4pc give the
disk up (DISK_EJECT on drive 1: pc/store.c hands its channel and
position to ata.c, and it becomes block device 0); it is hd0 from then
on, and the install goes on as above.

A swap partition follows the system's: install asks its size, 512 MB
(Enter, on a disk of 2 GB or more; else 256), 256 or 0 for none, and
`install -s 0|256|512` says it without asking. It is partition 2, type
82h. c4pc finds it at every boot (the first IDE disk with one: `c4pc: swap
on block device 0's partition 2, 512 MB`), and the pages C4IX's MMU puts
out go there, 8 sectors a page, instead of into c4pc's own memory; a
smaller size, or none, where the system would not fit beside it.

The disk must hold the system's files (230 MB now) and an eighth more for
ext2's own blocks, and 16 MB more: a VM's default disk is far bigger.

## Upgrading

A disk install brought up to a newer CD's system, keeping what is yours:
boot the new ISO and pick "Upgrade C4IX on the IDE disk (keeps your
files: asks first)", or at a C4IX prompt booted from it:

    install -u       # hd0; install -u hd1; -y asks nothing

Nothing is erased: no partitioning, no mkfs, the swap partition as it
was. Every file of the running system is written over the disk's of the
same path, but those in `/home`: a file there is kept as it is, and only
the ones the disk does not have are added. Nothing on the disk is
deleted, so whatever the system does not ship stays as it was -- the
games' settings and saved games (in `/home`: `/home/duke3d/duke3d.cfg`),
the desktop's settings (`/etc/desktop.cfg`, `notepad.cfg`, `ide.cfg`,
`machines.cfg`), the hypervisor's machines in `/vm`, and files of your
own anywhere, a game's directory under `/usr/games` too. A file
the system ships and you edited is the new system's afterwards; a file
an older system shipped and this one does not is left on the disk.

GRUB's two images are written again (core.img before partition 1, and
boot.img's code into sector 0 with the disk's signature and partition
table as they are), and `/boot` -- c4pc, the kernel, the menu -- is the
new system's. A disk whose menu started the hypervisor first still does,
and its `/vm/images` are updated (`vm images`).

install -u checks first that partition 1 is ext2 with an installed
C4IX's `/bin` and `/etc` on it, that its system is of the running one's
word (a 64-bit install upgrades from the CD's "C4IX 64-bit, text" entry),
and that it has room: the new files' size less the ones they replace,
and 4 MB more. Otherwise it says why and leaves the disk as it was.

## What is on the disk

| sectors | what |
|---|---|
| 0 | GRUB's boot.img (440 bytes), a disk signature, the partition table |
| 1 to 2047 | GRUB's core.img: the BIOS's disks, the MBR's partitions, ext2, multiboot, the menu's commands; its prefix `(hd0,msdos1)/boot/grub` |
| partition 1, type 0x83, active, the rest of the disk | ext2, label C4IX: the whole of `/` |

The ISO's boot disk is compressed (docs/c4pc.md, "The boot disk,
compressed"): c4pc inflates each file as install reads it, so what lands
on the disk is the system's files as they are.

install copies `/` as it is, every directory but `/proc` (the kernel's),
`/tmp` and `/mnt` (made empty on the disk): `/bin`, `/lib`, `/usr` (the
root archive's sources, headers and libraries, unpacked), `/etc`, `/var`,
`/home` and the rest. Programs get mode 0755. A file the boot drive left
on the host is read from there as it is copied.

`/boot` holds what GRUB loads: the files the ISO's menu for a disk
names, `/boot/grub/disk.cfg` (`src/c4pc/pc/grub-disk.cfg`) for the 32-bit
system, `disk64.cfg` for the 64-bit one -- every `/boot/NAME` that a
`multiboot` or `module` line names is copied -- and that menu as
`/boot/grub/grub.cfg`, its `@ROOT@` the partition's name. There is no
boot drive to load: each entry is c4pc and the kernel, and C4IX's
`--root hd0p1`.

## Booted with --root

init's `--root hd0p1` (init.c) mounts the partition at `/` before
anything else starts (vfs.c, `vfs_mount_root`): the root directory is
backed by the disk's ext2 as a mounted directory is, listed when first
looked in. What the RAM made there already stays in front: `/proc` (the
kernel's) and `/tmp` (the session's, in memory); the disk's of the same
name are not seen. Anything made at the top otherwise is the disk's.
`/proc/mounts` says `hd0p1 / ext2 rw`; `/` is not unmounted until
shutdown, which writes everything and marks it clean.

There is no vfsload: no manifest, no root archive. A program asked for by
its name on the boot drive -- `c4ix-sh.c4r`, which init and the desktop's
terminal start, `c4sc.c4r` -- is `/bin`'s (loader.c, `loader_alias`:
`/bin/sh`, `/bin/c4sc`). Libraries are `/lib`'s, as always.

Memory: booted so, a 64 MB machine has 6% of its memory in use at the
prompt, where booted from the CD the system's files stay on the boot
drive in the PC's memory and the root archive is unpacked into C4IX's.

## Big files

A file written to the disk is the RAM's until its last writer closes it
(docs/c4ix-ext2.md 3). One that grows past 1 MB so is written out and let
go of there (vfs.c, `e2_spill`), and a disk's file not in RAM is written
to the disk at once, a range at a time (`e2_write_through`, ext2.c's
`ext2_pwrite`), so a file bigger than the memory can be written: install
copies Quake's 18 MB pak0.pak on a 128 MB machine, and `cp` copies it
again on a 64 MB one.

## GRUB's images

GRUB's two images are made when the ISO is (Makefile, c4pc.iso):
grub-mkimage with the modules the menu needs, then pc/grubdisk.py sets
them as grub-bios-setup would for core.img from sector 1 -- boot.img's
boot-drive check made NOPs (a hard disk), diskboot's block list given
core.img's length. They are on the ISO as /boot/grub/c4pc-boot.img and
c4pc-core.img.

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

## On the hypervisor

(2026-11-24) Run in c4pc's hypervisor's control C4IX (GRUB's "C4PC
hypervisor" entry), or with `-h`, install makes the disk's menu start
the hypervisor's entry first (`set default="C4PC hypervisor..."`, by
its title) and copies the systems Machines makes machines of onto the
disk's /vm/images (`vm images`, from the CD): the disk is then the
hypervisor's, needing no CD. Run in a guest of the hypervisor, it writes
no GRUB (the hypervisor starts the guest's kernel itself), needs no CD
and makes no swap partition; the machine then boots from its disk with
`--root hd0p1` (Machines: Options, Boots from: disk).
docs/c4pc-hypervisor.md has the rest.

## Not yet

- The 64-bit system's install: install already picks `disk64.cfg` on a
  64-bit word; the menu and the 64-bit c4pc come with that system.
- Updating an installed disk from a newer ISO without erasing it.
- SATA (AHCI) disks: C4IX sees IDE ones (pc/ata.c). A VM's disk must be
  on an IDE controller (VMware's default for some guest types is SCSI).
- A disk bigger than 2 TB (the MBR's limit), or a UEFI PC.
- persist's store: on a disk with `/` on it, nothing is the store, and
  `persist` (the desktop starts it) says so and keeps nothing; the disk
  keeps everything itself.

## Measured

2026-11-23, c4pc-host (`--cdrom c4pc.iso --disk FILE`): install onto a
blank 1 GB disk, 4,717 files and 299 directories, 216 MB, in 19 s;
e2fsck finds it clean, and debugfs reads /bin/sh, Quake's pak0.pak and
Duke Nukem 3D's GRP back byte for byte. Booted with `--root hd0p1` and no
boot drive at all: programs run from the disk, a 64 MB machine has 6% in
use, an 18 MB file copied there has the same CRC and is the same byte for
byte from Linux, files made in one boot are there in the next, and
e2fsck finds it clean.
