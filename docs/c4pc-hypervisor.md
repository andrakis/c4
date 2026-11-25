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

- **Every processor**: under `--hv` c4pc starts all the PC's processors.
  VM 0's CPUs have the first (`--cpus N`; 2 of 4 or more when it does
  not say, else 1), each on one of its own (par.c), and
  the machines the rest (hv.c's pool: "c4pc: hv: the machines on
  processors 4 to 7"): a guest goes to the pool's processor with fewest
  on it, whose worker (guest_ap) runs its guests' slices of 4 ms in turn
  and naps until the soonest wakes or it is kicked (started, resumed,
  typed at, stopped). A guest of several CPUs has as many of the pool's
  processors to itself when that many have nothing on them (claim): par.c,
  its own instance, CPU k on processor procs[k] (par_start_on), its CPU 0
  run by its first processor's worker, which answers the others' requests
  while it naps or parks (par_after, par_wait0), and the others given to
  theirs as a job (par_ap_take). At its halt they leave their loops
  (par_shutdown, par_aps) before its memory goes; a pause holds them
  (par_pause). With too few free, its CPUs take turns on one (smp.c), and
  the log says so; with every processor another's alone, a guest runs on
  processor 0 in turn with VM 0, said too. On a PC
  of one processor, or with VM 0 on all of them, the guests run on
  processor 0 in turn with VM 0, a slice of each after each of VM 0's
  and while VM 0 naps (hv_run). A guest naps as VM 0 does, on the real
  clock. Each has its own arena, display (drawn off screen unless
  attached; its screens and Alt+Tab its own, but no modes: it is scaled
  into VM 0's), drives, sound card (heard while attached) and console
  (the serial line's, a line at a time, "vm1 NAME: ..."); no swap of the
  host's. Their accelerators hold their writes in their processor's
  place (accel.h acc_k), apart from VM 0's CPUs.
- **The device lock**: with a pool, what the machines share is one
  processor's at a time (engine.c eng_lock_fn, pc/ap.c pc_lock): every
  machine's device registers, rare opcodes (OPEN, READ, PRTF ...) and
  console, c4pc's own console, the screen and the sound card's mixing.
  Their scratch buffers, the font cache, the drives' caches and the IDE
  disks are the PC's, not a machine's; the instructions between run side
  by side. The lock is taken again by the processor that holds it, and
  a fault out of a device (a DMA a task's space refuses), which jumps
  past the unlock, gives it back at the catch it lands in (mmu.c's,
  par.c's). A guest that halts on its processor is finished on processor
  0 (hv_run: the screen given back, its memory freed, a restart).
- **A guest's frames** are drawn and shown by its own processors, not
  processor 0's: a CPU of several reaches the display's registers itself,
  under the lock (engine.c AP_DEV; without the lock, as with VM 0 alone,
  they are still asked of CPU 0), and an attached guest's frame goes onto
  the screen from the processor that presented it (gui.c's
  gui_frame_hook, pc.c guest_frame); processor 0's display_tick finds it
  there. Every processor takes the boot processor's PAT as it starts
  (pc/ap.c), so the framebuffer is write-combined on each. BLIT8 and
  FBFLIP scale by a column table, a repeated row copied, not two
  divisions a pixel. In QEMU on 4 processors (2026-11-26), frames a
  second before and after: Quake's attract loop in a 1-CPU guest 30-34
  to 34-38, on a 2-CPU guest's CPU 1 29-34 to 34-38, in VM 0 27-31 to
  31-35; Doom's in a 1-CPU guest 38-39 to 44, in VM 0 36-37 to 41-42.
  What is left of a frame is the game's own instructions.
- **The 30 s report** (with a pool, on c4pc's log): the device lock's
  share held and waited for, each processor's; frames a second the screen
  showed (and whose) and each machine's display drew; each guest's
  instructions a second and how its sound kept up.
- **VM 0's commands** read and write its memory through the machine of
  the CPU that asked (with VM 0 on several processors, CPU 0's machine
  had another task's space: a guest started with another's arguments).
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
  The PC's own disk may be far bigger than 2 GB: install, the control
  C4IX's ext2 and the guests' images on it were checked on a 20 GB IDE
  disk (2026-11-24; mkfs gives an inode to every 64 KB past 16 GB, 16 KB
  past 1 GB). **Later**: guest images of 2 GB or more (`vm mkdisk`
  refuses them now) -- `vm` and the kernel's file positions with
  C4IX_LFS at the 32-bit word, as docs/c4ix-lfs.md has it.
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

The control C4IX's desktop (only where the hypervisor is: its own colour
scheme, "Hypervisor") opens **Machines** and System Monitor as it starts,
and has Machines on the desktop and in the Go menu, after Proxmox's look.
"Hypervisor" is dark -- charcoal faces, light text, amber title bars and
selection, to go with Kiln's picture behind it -- so the PC's own
desktop is told from a guest's at a glance; guests keep their light
schemes. A scheme gives its text and field colours (winkit's `C_DARK` and
`C_WHITE` are the scheme's: black and white in every other one), and
what is black whatever the scheme -- a terminal, a graph, a screen --
says 0x000000.
A tree on the left: c4pc, the PC; under it the control C4IX (VM 0, this
one: shown, its log and screen, not changed) and each machine, from
/vm/NAME/vm.cfg and from what the hypervisor has made, its id and a
monitor lit while it runs (amber, paused). For the one chosen a toolbar
(Start, Shutdown, Stop, Restart, Pause or Resume, Attach, Delete) and
seven tabs:

- **Summary**: its state and settings, compact, with its instructions a
  second and a minute of them beside; under them its screen, as large as
  the window lets it (THUMB, once a second; the control C4IX's too). For
  c4pc: its processor, processors, memory and disks (HOST, 21), the
  machines, and CPU 0's time busy.
- **Console**: its screen larger, and Attach (as the toolbar's, or a
  double-click in the tree): the PC's screen, keyboard and mouse;
  Ctrl+Alt+Home comes back.
- **Hardware** (memory, CPUs, MMU, hard disk, swap and CD images) and
  **Options** (boots from its archive or its disk, kernel, boot archive,
  arguments, start at boot): a form -- dropdowns for a choice, a field
  edged in red while it holds what it may not (memory 16 to 1024 MB; a
  file that is not there), Browse... for a file (the kit's file box) --
  and Apply writes vm.cfg, for the machine's next start. For c4pc,
  Hardware is what c4pc found of the PC.
- **Load**: four tabs of its own, the last minute, each graph with its
  scale at the top left and its value now at the top right (and the
  value in its title):
  - **CPU**: each of its CPUs' instructions a second, and its time busy
    where c4pc says (INFO's idle<k>) (a machine of several CPUs: them
    taking turns on its processor, smp.c); for c4pc, a graph for each
    of the PC's processors (the firmware's count): processor 0's time
    busy, the control's CPUs on theirs, and on each of the pool's the
    instructions of the machines on it, added up, with their names
    (INFO's "processor").
  - **Disk**, after btop's disks box: each disk image's reads and
    writes, KB a second, with its size (INFO's blk<k>); for c4pc, its
    own disks' (HOST's io and diskio<k>) and every running machine's.
  - **Memory**: total, used, free, cached (the control's, from its
    /proc/meminfo) and swap; for c4pc, what the machines were given of
    the PC's and each one's heap in use.
  - **Network**: its layout, received and sent for each machine, empty
    until the machines have a network card (stage 4).
- **Overview**, after btop, in the GUI: the processors across the top
  (all of them busy, the last minute; a bar and share for each, c4pc's
  uptime), then memory and disks side by side, the network under them,
  and on the right the machines (state, instructions a second, CPUs, MB)
  and the control C4IX's tasks, the busiest first (cpustat.h).
- **Log**: its own: c4pc's (its boot, the hypervisor's words), the
  control C4IX's console, or a machine's console (LOG, 17: A2 0, 1, or
  its id + 1), scrolled with the bar, the wheel or Page Up and Down.

A counter c4pc does not keep (an older c4pc) shows as "-", or a line
saying so: INFO's heap, heapused, swapslots, swapused, blk<k> and
idle<k>, and HOST's io, diskio<k> and pidle<k>, are cumulative and read
each second.

**View** (the window's menu) chooses between that and **VMware's look**,
kept in /etc/machines.cfg (`view 0` or `view 1`): the same toolbar in
VMware's words (New VM, Power On, Shut Down, Power Off, Restart,
Suspend, Full Screen, Delete) and the tree as a library; on the right
the one chosen, no tabs -- a running machine's console with Full Screen
(Attach) and Settings...; a stopped one's "Power on this virtual
machine", "Edit virtual machine settings", its Devices and its Details;
c4pc's summary. Settings are Hardware and Options, the same form, with
Back.

Start runs `vm up NAME` in the background (it reads the kernel and the
boot archive), its output and errors into /vm/NAME/start.log: a start
that fails says why, in the status line and a message. Start on a
machine made before deletes what was made, so a changed vm.cfg counts.
Shutdown is SIGTERM, Stop at once, Restart Stop and Start. **Pause**
(PAUSE, 18) stops a running machine where it is -- its memory and screen
kept, its clock stopped, so it finds no time gone at **Resume** (RESUME,
19); a paused machine is not attached. **Delete** asks, then takes a
stopped machine out of the hypervisor and removes /vm/NAME, its vm.cfg
and disk images (`vm delete NAME`, which asks too, or -y; `vm pause ID`,
`vm resume ID`). **Create VM** is one form: a name, the system (a
dropdown: C4IX's desktop or text console, C4KE, C4DOS, C4IX with an ISO
file as its CD), the memory, the CPUs, a disk image's and a swap image's
size, start at boot; it runs `vm create NAME MB DISK SYSTEM [cpus=N]
[swap=MB] [autostart]` in a Terminal, which shows the images being
written.

A machine that halts with status 82 (C4IX's reboot, C4DOS's REBOOT,
c4sh's reboot) starts again; one halted gives its arena and screen back
(c4pc's engine frees nothing else). Its accelerators are on (hv.c says
why that is safe; not for a machine of several CPUs while VM 0 runs on
several of the PC's processors). Its swap is an image (`vm mkswap NAME
MB`, vm.cfg's swap line). The control C4IX's Shut Down and Restart, with
machines running, shut them down first in Machines' window, with how each
goes, Stop them now and Cancel; and however VM 0 ends (`shutdown` in a
Terminal, the power button, VMware's Shut Down Guest), c4pc asks each
machine still running to shut down and runs them until they have, a
minute at most, on the screen, before the PC powers off.

## Stages

1. **Done**: the manager, machines made from VM 0's files, attach and
   detach (Ctrl+Alt+Home, `detach`), `vm`.
2. **Done**: guest disks and CDs as VM 0's files (SYS_FMAP, extents),
   VM 0's CD shared, vm.cfg, `vm create`, `vm up`, `vmd`, the GRUB
   entries.
3. **The Machines app**, Proxmox-like (Q8: the user finds VMware's
   library annoying). **Built (2026-11-24)**, above, with Delete and
   Pause (the same day). Not yet: a guest's memory in use (the
   hypervisor knows only what it was given), a disk image added to a
   machine after it was made (edit Hardware's row), a pause kept on the
   disk (suspend), and freeing a big image faster (a 1.5 GB image's
   delete takes about 10 s: ext2 frees its blocks one at a time).
4. **Host-only networking between guests (Q9)**: a virtual NIC in each
   machine (a ring of frames in its page 0 window, as the display's
   ring), and a switch in c4pc that passes frames between them; then a
   TCP/IP stack in C4IX. Networking to outside machines after that.
5. Guests on their own processors (a second smp per machine in par.c),
   sound for the attached guest, pause/save, raw partitions.
6. 32-bit and 64-bit machines side by side: the engine built twice into
   one c4pc (C4PC_W64's sources under other names).
