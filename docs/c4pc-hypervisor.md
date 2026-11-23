# c4pc as a hypervisor

The user's idea (2026-11-23): a GRUB entry boots c4pc into a mode that
runs several c4 virtual machines of different configurations, with disk
files assigned to them, boots them, attaches to one so that it has the
screen and the keyboard, and detaches again -- an S/390's LPARs, without
the S/390's interface. Stages 1 and 2 are built (2026-11-24); 3 and on
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

- **The guests run on CPU 0's thread**, in turn with VM 0: a slice of
  4 ms of each runnable guest after each of VM 0's, and while VM 0 naps;
  a guest naps as VM 0 does, on the real clock. Each has its own arena,
  display (drawn off screen unless attached), drives and console (the
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
  mb N, disk FILE, cdrom FILE|pc, args ..., autostart yes|no. `vmd` (vm
  under another name) starts every machine whose vm.cfg says autostart.

## Using it

```
vm create one 256 1024    # /vm/one: disk0.img of 1 GB, vm.cfg for a first boot from the CD
mount cdrom               # the C4PC disc at /mnt/cdrom
vm up one                 # made and started; vm lists the machines
vm attach one             # its screen: Install C4IX onto its hd0, then shut it down
                          # (Ctrl+Alt+Home: back to VM 0's)
                          # vm.cfg: boot -, args --root hd0p1 --desktop --no-top, autostart yes
vm up one                 # from its own disk now; and at every boot by vmd
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

## Stages

1. **Done**: the manager, machines made from VM 0's files, attach and
   detach (Ctrl+Alt+Home, `detach`), `vm`.
2. **Done**: guest disks and CDs as VM 0's files (SYS_FMAP, extents),
   VM 0's CD shared, vm.cfg, `vm create`, `vm up`, `vmd`, the GRUB
   entries.
3. **The Machines app**, Proxmox-like (Q8: the user finds VMware's
   library annoying): a tree of the machines on the left, each one's
   summary (state, CPU and memory graphs, its disks), a console tab that
   shows its screen live (a thumbnail of its framebuffer, attach on a
   click), and its settings as a form; New, Start, Shutdown, Stop.
4. **Host-only networking between guests (Q9)**: a virtual NIC in each
   machine (a ring of frames in its page 0 window, as the display's
   ring), and a switch in c4pc that passes frames between them; then a
   TCP/IP stack in C4IX. Networking to outside machines after that.
5. Guests on their own processors (a second smp per machine in par.c),
   sound for the attached guest, pause/save, raw partitions.
6. 32-bit and 64-bit machines side by side: the engine built twice into
   one c4pc (C4PC_W64's sources under other names).
