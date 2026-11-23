# C4IX on a disk: a block device, ext2 in the kernel, file modes

Status, 2026-11-23: sections 1 to 4 are built, and 5's modes are kept and
shown but not yet enforced; 6 and 7 are to come.

| status | what |
|---|---|
| done | the block device (blk.js, c4pc's dev.c; `--disk FILE.img` on cli.js and c4pc-host, a PC's IDE disks on c4pc), the kernel's blk.c and /proc/disks |
| done | ext2 in the kernel (ext2.c): read and write, 1, 2 and 4 KB blocks, indirect blocks to triple, directories, links, rename, a mode; mkfs |
| done | mounted into the VFS (vfs.c): `mount hdN DIR`, `mount -r`, `umount`, /proc/mounts, `df`, `mkfs`, `chmod`; every disk unmounted clean at shutdown |
| *measured* | a disk Linux's mke2fs made (1 KB and 4 KB blocks) and one C4IX's mkfs made: listed, read, written (mkdir, cp, cp -r, mv, rm, chmod) and unmounted, and `e2fsck -fn` finds nothing wrong with any; debugfs reads the copies back byte for byte. On c4pc-host with and without the MMU and on 4 threads, on libjs's cli.js, and on c4pc64.elf in QEMU with an IDE disk |
| done | MBR partitions as devices (blk.c: hd0p1 ...), and `install` (docs/c4ix-install.md): the system on the disk, / its ext2 (`--root`) |
| done | a disk's file past 1 MB written to the disk as it is written (vfs.c `e2_spill`, `e2_write_through`; ext2.c `ext2_pwrite`), so one bigger than the memory can be: an 18 MB copy on a 64 MB machine |
| not yet | modes enforced (read-only, execute bits), `ls -l`; /dev/hdN; the page's disk; / on the disk (6); mmap (7) |


C4IX's files are RAM (src/c4ix/vfs.c). What lasts is persist's tars on
host drive 1 (src/c4ix/user/persist.c), and the boot disk and a CD are
host drives it reads but never writes. This is the plan for a filesystem
C4IX keeps itself, on a disk, written as it changes: ext2, the second
extended filesystem, which Linux, GRUB and e2fsprogs all know, so the
disk is never a mystery (mke2fs makes one, e2fsck checks what C4IX
wrote, debugfs and a loop mount show it).

## 1. A block device

A machine's disks, as sectors: a window of registers at 0x540, between
the MMU's (0x500) and the CPU-local window (0x800). The kernel's alone:
a user task does not reach it (its IOPM has no bit for it).

| address | name | |
|---|---|---|
| 0x540 | BLK_COUNT | r: block devices fitted |
| 0x544 | BLK_SEL | r/w: the selected one, from 0 |
| 0x548 | BLK_SECTORS | r: its 512-byte sectors |
| 0x54c | BLK_FLAGS | r: 1 read-only, 2 removable |
| 0x550 | BLK_LBA | w: the first sector of the next command |
| 0x554 | BLK_NSECT | w: how many |
| 0x558 | BLK_ADDR | w: where in memory (the kernel's: physical) |
| 0x55c | BLK_CMD | w: 1 read, 2 write, 3 flush; r: 0 done, -1 failed |

INFO says the window is there with C4I_BLK (0x200000). A command is
synchronous: the write to BLK_CMD returns when the sectors have moved.

- **c4pc on a PC**: each IDE disk that is not the store (pc/store.c) is
  one, by ATA PIO (LBA28, then LBA48 past 128 GB). The store keeps its
  disk for persist until ext2 replaces it (section 6).
- **c4pc-host and cli.js**: `--disk FILE.img`, a disk image, read and
  written in place.
- **The web page**: later (an image kept in IndexedDB, written back as
  it changes).

The kernel's side (blk.c): the devices, and a cache of blocks in the
heap, least recently used first, written through.

## 2. ext2 in the kernel

ext2.c, revision 1 as mke2fs makes it with `-t ext2`: 1, 2 or 4 KB
blocks, block groups with their bitmaps and inode tables, inodes of 128
or 256 bytes, direct blocks and single, double and triple indirect
ones, linked-list directories (no htree: read as linear, which htree
directories still are), symlinks fast and slow. No journal (that is
ext3), no extents (ext4). Features C4IX does not know make it refuse
the disk, or mount it read-only where they are read-only compatible.

- Read: the superblock and group descriptors at mount, an inode, its
  blocks, a directory's entries, a lookup.
- Write: blocks and inodes allocated from the bitmaps (near the parent's
  group), the counts in the group descriptors and the superblock kept,
  a directory entry added (in a block with room, else a new block) and
  removed (merged into the one before), truncate, unlink, link, rename,
  mkdir and rmdir, the mtime and ctime set from the clock.
- Consistency: every change is written as it is made, metadata first
  where an order matters (a block marked used before an inode points at
  it), so a machine stopped part way leaves a disk e2fsck repairs. The
  superblock is marked not clean while mounted (s_state), clean at
  unmount, so a disk never unmounted is checked.
- mkfs: `mkfs hd0` (src/c4ix/user/mkfs.c, the kernel's ext2_mkfs through
  SYS_MKFS), so that a blank disk in a VM can be made one without Linux:
  rev 1, 128-byte inodes, an inode for every 8 KB, sparse superblock
  backups, filetype in directory entries, a root and lost+found.

## 3. Into the VFS

(2026-11-23: what is written is the RAM's until its last writer closes it
only up to 1 MB. A file that grows past that is written out and let go
of, and from then on, as for a disk's file not in RAM, each write goes to
the disk at once: docs/c4ix-install.md, "Big files".)

A mounted ext2 is a directory whose vnodes are backed by the disk, as a
host directory's are backed by the host (vfs_lazydir, vfs_lazyfile), and
the VFS's machinery for those carries over:

- A vnode gets a backing: the filesystem and the inode number. A
  directory is listed from its entries when it is first looked in
  (vfs_dirlist's way); a file is lazy until it is read, and then read in
  whole (vfs_fill) or, a big one, a range at a time (vfs_span_read's
  way), and kept as a cache the memory can take back (backed).
- A write makes the file RAM's (vn_unback), as now, and it goes to the
  disk when the last writer closes it, and at sync: the inode's blocks
  rewritten, grown or cut to the new size. A big file being written is
  RAM's until then.
- mkdir, unlink, rmdir, rename and link in a mounted directory are made
  on the disk at once, and in the vnode tree.
- stat gives the inode's mode, size, links and number, and a device of
  its own (4), which persist does not go into: the disk is its own
  persistence.
- Mounting: `mount hd0 /mnt/disk` (mount.c, the CD's mount, taught
  ext2; SYS_MOUNT), `umount` writes what is dirty and marks the disk
  clean (SYS_UMOUNT), and so does the kernel's shutdown for every disk
  still mounted. Not yet: at boot, an fstab on the disk or the kernel's
  command line mounting it at /home, or at / (section 6).

What a user task asks of the kernel: SYS_MOUNT (248: disk, path,
read-only), SYS_UMOUNT (249: path), SYS_MKFS (250: disk, block size,
label), SYS_STATFS (251: path, seven words: block size, blocks, free,
inodes, free, read-only, disk), SYS_SYNC (252), SYS_CHMOD (253: path,
mode).

Found on the way: a file read a range at a time must not have the disk
read straight into the caller's buffer (under the MMU a user task's
address is not memory's): ext2_read reads into the kernel's own and
copies.

## 4. Devices as files

/dev/hd0, /dev/hd1 ...: a block device read and written as a file, for
dd. Not yet: mkfs is the kernel's instead (SYS_MKFS), and /proc/disks
lists the devices. A device file past 2 GB wants 64-bit positions, which
a kernel built with C4IX_LFS has (docs/c4ix-lfs.md): the native 64-bit
build's, and a 32-bit one's where it is asked for.

## 5. Modes

Every vnode gets a mode (the permission bits and the type, as ext2 keeps
them), a uid and a gid. RAM files get 0644 (0755 a program, 0755 a
directory), as stat says now; an ext2 file has its inode's. chmod (a
syscall, and /bin/chmod), stat's ST_MODE from the vnode, `ls -l`. What
is enforced, while C4IX has one user (uid 0): a file without a write bit
for anyone is read-only (EACCES, as root on Linux is not, deliberately:
it is how a file is protected here), and a program runs only with an
execute bit. Users and groups (uid, gid checked, /etc/passwd) come later,
when C4IX has logins.

## 6. Installed: the system on the disk

Built (2026-11-23): `install` (docs/c4ix-install.md) writes GRUB and a
partition table, makes the disk one ext2 partition and copies the whole
running system onto it, c4pc and the kernel into its /boot. Booted from
the disk, init mounts it at / (`--root hd0p1`, vfs.c `vfs_mount_root`):
nothing unpacked, nothing in RAM that is not in use, every change on the
disk; /proc and /tmp stay the RAM's. The kernel reads MBR partitions
(blk.c: hdDpP). A file bigger than 1 MB written to a disk goes to the
disk as it is written, not into RAM (section 3). persist and the store
are for the CD boot and the browser only, as planned.

The plan was: `install /dev/hd0` (a program, and the desktop's): a partition table, an
ext2 partition, the system copied on (the boot disk's files, the root
archive unpacked once), and GRUB written to the disk (its boot sector,
its core image after it, built with ext2 support), with a grub.cfg that
loads c4pc from the disk by path. Booted from the disk, C4IX's / is the
ext2 partition: nothing unpacked at boot, nothing in RAM that is not in
use, every change on the disk as it is made. persist and the store are
then for the CD boot and the browser only.

## 7. Files mapped into memory

mmap of a file (later): under the MMU a task's pages are the kernel's to
place (mmu.c), so a file's pages can be its cache's: mapped read-only and
shared, or private and copied on write; MAP_SHARED written back at
msync and munmap. Without the MMU, a copy.

## Order

1. The block device: c4pc's IDE disks and images, cli.js's images, the
   kernel's blk.c, /dev/hdN. Tested with images mke2fs makes.
2. ext2 read: mount an image Linux made (mke2fs -d), list and read it,
   against what is in it.
3. ext2 write, and mkfs: every change checked by e2fsck -fn afterwards,
   and the files read back by debugfs.
4. Modes and chmod.
5. Install, and booting from the disk.
6. mmap.
