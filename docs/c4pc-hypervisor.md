# c4pc as a hypervisor

The user's idea (2026-11-23): a GRUB entry boots c4pc into a mode that
runs several c4 virtual machines of different configurations, with disk
files assigned to them, boots them, attaches to one so that it has the
screen and the keyboard, and detaches again -- an S/390's LPARs, without
the S/390's interface. Stages 1 to 3 are built (2026-11-24); 4 and on
are the plan.

## The shape

```
 PC: c4pc --hv
 ├── VM 0  "control": C4IX from the installed disk (or the CD), its desktop,
 │         `vm`, /vm/NAME/{vm.cfg,disk0.img}
 ├── VM 1  C4IX, 256 MB, its hd0 /vm/one/disk0.img, VM 0's CD drive
 └── VM 2  ...
```

- **VM 0** (the user's answer to Q2: a control C4IX boots first, as Xen's
  dom0) is the machine pc.c boots, with all of the PC's devices. With
  `--hv` it is given a quarter of the memory c4pc would give one machine
  (256 MB at most); the rest is the guests'.
- **The machine manager** (pc/hv.c): a window in every machine's page 0,
  HV_BASE 0x560-0x57c (C4I_HV in the host's info), user mode's where the
  MMU's IOPM allows the devices. A command is its arguments (A0..A3),
  then CMD; RES answers. VM 0 alone makes, starts, stops and attaches;
  any machine may ask its id and detach.

| CMD | | |
|---|---|---|
| 1 NAME, 2 MB, 3 ARGS | the next machine's name, memory, kernel arguments | VM 0 |
| 4 NEW | its kernel (A0, A1) and boot archive (A2, A3, 0 for none) copied from VM 0's memory: its id | VM 0 |
| 5 START, 6 STOP (A1: at once), 11 DELETE | | VM 0 |
| 7 ATTACH, 8 DETACH, 13 ATTACHED | the screen, keyboard and mouse | 8: any |
| 9 STATE, 10 INFO (text into A1, A2 bytes) | | VM 0 |
| 12 DISK, 14 CDROM | extents (A1 the table, A2 how many, A3 sectors); CDROM with none: VM 0's CD drive | VM 0 |
| 15 SEND | bytes (A1, A2) typed at a guest's console | VM 0 |
| 16 THUMB | a running guest's screen scaled to A2 x A3 (at most 640x480), a word a pixel at A1 | VM 0 |

- **The guests run on CPU 0's thread**, in turn with VM 0: a slice of
  4 ms of each runnable guest after each of VM 0's, and while VM 0 naps;
  a guest naps as VM 0 does, on the real clock. Each has its own arena,
  display (drawn off screen unless attached; its screens and Alt+Tab
  its own, but no modes: it is scaled into VM 0's), drives and console (the
  serial line's, a line at a time, "vm1 NAME: ..."). Their accelerators
  are off (theirs keep state a CPU at a time, which VM 0's CPU 0 has
  already), and they have no swap of the host's.
- **Disks (Q3: images on the disk)**: a guest's hd0 is a file on VM 0's
  ext2, read and written where it lies on the PC's IDE disk: C4IX's
  SYS_FMAP (257) says where a file keeps each block (the disk, the
  sector, sectors a block), `vm` gives the hypervisor the file as
  extents (its first sector in the image, the disk, the first sector
  there, how many), and the guest's block device (hv.c xmap) reads and
  writes the PC's disk directly -- no turn of VM 0's, no copy. The image
  must have all its blocks (`vm mkdisk` writes them) and VM 0 must leave
  it alone while the guest runs. An ISO file can be a guest's CD the same
  way, or VM 0's own CD drive is shared (`cdrom pc`). IDE disks are
  LBA48 to 2 TB in c4pc (ata.c) and C4IX counts sectors in an int (1
  TB); an image's size is an int at the 32-bit word (2 GB) until `vm`
  is built with C4IX_LFS. SCSI is a much later goal.
- **Attach and detach (Q5)**: `vm attach ID`; Ctrl+Alt+Home, or `detach`
  in a C4IX guest, gives VM 0 back the screen. A detached guest runs on
  headless. Ctrl+Alt+F1/F2 and the rest are the PC's keys as before.
- **Sound (Q6)**: VM 0's alone for now (the guests have no card).
- **Boot and settings (Q7)**: GRUB's "C4PC hypervisor" entries -- on the
  ISO, VM 0 from the CD; on an installed disk, VM 0 from the disk, and
  `-e vmd`. /vm/NAME/vm.cfg, a line each: kernel FILE, boot FILE (- for
  none: a machine that boots from its own disk, `args --root hd0p1`),
  mb N, mmu yes|no (yes if it does not say), disk FILE, cdrom FILE|pc,
  args ... (none: the text console), autostart yes|no, system NAME.
  `vmd` (vm under another name) starts every machine whose vm.cfg says
  autostart.

## The systems, and installing the hypervisor

`vm create NAME MB DISK_MB [SYSTEM]` makes a machine of one of four
systems, their kernels and boot archives in /vm/images:

| SYSTEM | what it is | its files | memory |
|---|---|---|---|
| c4ix (the default) | C4IX's desktop | c4ix32.c4r, c4ix.tar | 256 MB |
| c4ix-text | C4IX at its text console | c4ix32.c4r, c4ix.tar | 128 MB |
| c4ke | C4KE, at c4sh | c4ke32.c4r, c4ix.tar | 128 MB |
| c4dos | C4DOS: grin, and the games in \GAMES | c4dos32.c4r, c4dos.tar | 256 MB |

or an ISO file: C4IX's desktop with that ISO as its CD. /vm/images comes
from the C4PC disc (its /boot/c4ix32.c4r and disk.tar, and its
/boot/images: C4KE's kernel, C4DOS's kernel and its page's disk with
the games, libjs/images/dos, compressed as disk.tar is), copied by `vm
images` -- which vm create and vm up run themselves the first time a
file is not there -- so that machines start with no CD in the drive.
(Before, vm create named the disc's own files, and a machine would not
start once the CD was out.) C4KE has no games of its own: the games are
C4IX's, C4DOS's and the bare machine's.

A machine with no display ring (C4IX's text console, C4KE, C4DOS before
grin) has a text screen of its own (pc/vterm.h, as the PC's console):
Machines shows it, and attached, the PC's screen is it and the keyboard
types to it, as a terminal (echo, backspace, ^C, ^D).

**Installing the hypervisor** is install, run in the hypervisor's
control C4IX (GRUB's "C4PC hypervisor" entry on the CD, then the Install
C4IX icon, or `install` in a Terminal). Run there, install makes the
disk's menu start the hypervisor's entry by default (GRUB's `set
default` by its title) and copies /vm/images onto the disk (`vm images
/mnt/install/vm/images`); `install -h` does so from any entry. The disk
then boots the control C4IX from itself, vmd starting the machines that
say autostart, and needs the CD no more. Run in a guest, install puts C4IX
on the guest's hd0 with no GRUB and no CD (the hypervisor starts a
guest's kernel itself); then the machine's Options, Boots from: disk
(boot -, args --root hd0p1 ...) and it starts from its own disk, every
change kept.

## Using it

```
vm create dos 0 0 c4dos   # /vm/dos: C4DOS (the systems copied to /vm/images from the CD, once)
vm up dos                 # made and started; vm lists the machines
vm create one 256 1024    # /vm/one: C4IX's desktop, and disk0.img of 1 GB
vm up one
vm attach one             # its screen: Install C4IX onto its hd0, then shut it down
                          # (Ctrl+Alt+Home: back to VM 0's)
                          # vm.cfg: boot -, args --root hd0p1 --desktop --no-top, autostart yes
vm up one                 # from its own disk now; and at every boot by vmd
vm log                    # c4pc's log: its own lines, and the guests' consoles
vm send one ls /          # a line typed at its console; its output is the serial line's
vm stop one               # SIGTERM: it shuts down (-f: at once); vm delete one
```

*Measured* in QEMU with KVM (~/i386probe/hvtest.sh): VM 0 from the boot
disk with the ISO in its CD drive and a blank 2 GB disk; `mkfs hd0`,
`vm mkdisk /mnt/hd0/g1.img 64` (17 extents), a guest of 128 MB from the
CD's kernel and boot archive with that image as its hd0, its desktop
running 11 s after `vm start`; in the guest by `vm send`: mkfs hd0,
mount, a file written; attach (Ctrl+Shift+Esc opened the guest's System
Monitor: 126 MB, the guest's), Ctrl+Alt+Home (VM 0's desktop, whole);
stop, start again, mount: the file read back from the image.

## Machines

The control C4IX's desktop has **Machines** in its Go menu (only where
the hypervisor is), after Proxmox's look: a tree on the left -- the PC,
and under it each machine, from /vm/NAME/vm.cfg and from what the
hypervisor has made, its id and a monitor lit while it runs -- and for
the one chosen a toolbar (Start, Shutdown, Stop, Restart, Console) and
five tabs:

- **Summary**: state, id, memory, the instructions it runs a second and
  a minute's history of them, its disk, CD and start at boot, and its
  screen small (THUMB, once a second).
- **Console**: its screen larger; Attach (or a double-click in the tree)
  gives it the PC's screen, keyboard and mouse; Ctrl+Alt+Home comes back.
- **Hardware** (memory, MMU, hard disk, CD/DVD) and **Options** (system,
  boots from: its archive or its disk, kernel, boot archive, kernel
  arguments, start at boot): vm.cfg's lines; a click on a row edits it,
  for the machine's next start.
- **Log**: c4pc's log (LOG, 17: the last 32 KB of what its console and
  serial line were given) -- the PC's whole, or a machine's lines of it
  (its console, and the hypervisor's words about it).

Start runs `vm up NAME` in the background (it reads the kernel and the
boot archive), its output and errors into /vm/NAME/start.log: a start
that fails says why, in the status line and a message. Start on a
machine made before deletes what was made, so a changed vm.cfg counts.
Shutdown is SIGTERM, Stop at once, Restart Stop and Start. **Create VM**
asks a name, the system (c4ix, c4ix-text, c4ke, c4dos, or an ISO file),
the memory and a new disk image's size (0 for none), and runs `vm
create` in a Terminal, which shows the image being written.

## Stages

1. **Done**: the manager, machines made from VM 0's files, attach and
   detach (Ctrl+Alt+Home, `detach`), `vm`.
2. **Done**: guest disks and CDs as VM 0's files (SYS_FMAP, extents),
   VM 0's CD shared, vm.cfg, `vm create`, `vm up`, `vmd`, the GRUB
   entries.
3. **The Machines app**, Proxmox-like (Q8: the user finds VMware's
   library annoying). **Built (2026-11-24)**, above. Not yet: a guest's
   memory in use (the hypervisor knows only what it was given), a disk
   image added to a machine after it was made (edit Hardware's row), a
   Delete that removes /vm/NAME, and pausing.
4. **Host-only networking between guests (Q9)**: a virtual NIC in each
   machine (a ring of frames in its page 0 window, as the display's
   ring), and a switch in c4pc that passes frames between them; then a
   TCP/IP stack in C4IX. Networking to outside machines after that.
5. Guests on their own processors (a second smp per machine in par.c),
   sound for the attached guest, pause/save, raw partitions.
6. 32-bit and 64-bit machines side by side: the engine built twice into
   one c4pc (C4PC_W64's sources under other names).
