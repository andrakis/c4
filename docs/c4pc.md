# c4pc: the c4 machine on a PC

c4pc is route (a) of [c4ix-i386-port.md](c4ix-i386-port.md): libjs's
engine in C, booted on a PC with nothing under it, so that C4IX's images
run there unchanged. It is in `src/c4pc/`.

| status | what |
|---|---|
| done (M0, M1) | the engine, devices, heap, formatter and FPU, as libjs has them; multiboot; GDT, IDT, PIC, PIT; COM1; VGA text with enough ANSI for a shell; the keyboard; drives from tar archives; the DOS loader |
| done (M2) | the MMU, libjs/mmu.js as it is (`--mmu`): its registers, the TLB arrays and the stack and code windows, the user loop, faults, the user window, devices checked against the task's space, swap through the window |
| *measured* | C4IX boots, runs `ps` and shuts down with output byte for byte libjs's, in the same 1,474,118,389 cycles, on c4pc-host, on c4pc.elf in QEMU (`make test-c4pc`), and in DOSBox-X through C4PC.COM; test-libjs's 27 native-oracle programs print what they print on libjs, with the same status |
| *measured* | with the MMU: the same session byte for byte libjs `--mmu`'s, in its 1,500,630,450 cycles, on c4pc-host and on c4pc.elf in QEMU; test-c4ix-mmu.mjs's cases but the display's (test-c4pc-mmu.sh) are killed by the page faults that test names, byte for byte and cycle for cycle as libjs, on c4pc-host and in QEMU |
| done | C4IX's own accelerators (accel.c, src/c4ix/accel.mjs in C): libarc's inflater and CRC-32, behind libjs's loader numbers (INFO's C4I_ACCEL, ACC_FIND, ACC_OS); `--no-accel`, `--no-osaccel` as cli.js |
| *measured* | with them and the MMU: the session byte for byte libjs's (its build listing os/c4ix), in its 111,571,007 cycles, on c4pc-host and in QEMU: 13 times fewer than C4IX's own C |
| done (M4) | the display (gui.c): libjs's device at 0x400, its rings and IRQ, the page's drawing done in C with c4pc's own faces (font.c); on a PC a VBE framebuffer from GRUB or the BGA, the PS/2 mouse, the keyboard to the display |
| *measured* | C4IX's desktop from the ISO under KVM, mouse and keyboard; on c4pc-host, scripted, in `make test-c4pc`; in DOSBox-X through C4PC.COM's VESA mode; Doom from the desktop at the clock's whole rate under KVM, 1% of the time drawing |
| done | at the keyboard, the real clock: games drop frames on a slow PC rather than play in slow motion or at uneven speed; the screen written a row at a time, write-combined through the PAT; the console drawn on a framebuffer while no program has the display |
| done | the games' accelerators (games/NAME.c, each game's c4/accel.mjs in C): Doom, Duke Nukem 3D, Quake, Rise of the Triad, Super Blit Kart, Tyrian and Wolfenstein 3D, behind libjs's loader numbers as its full build lists them |
| *measured* | every game's acceltest passes on c4pc-host and on c4pc.elf in QEMU, with libjs's count of cases; every game's timedemo shows the same frames with them and without, on the bare machine and inside C4IX under the MMU; a timedemo takes a sixth to a half of the time it did (section Speed) |
| done (M5) | the sound card (snd.c, opl.c): libjs's device at 0x480 and its mixer, the AdLib (DOSBox's dbopl, as libjs/opl.js has it), sixteen sample channels and the speaker, in C; on a PC played through a Sound Blaster 16 (pc/sb16.c) or Intel HD Audio (pc/hda.c); `c4pc-host --wav` as cli.js's |
| *measured* | on c4pc-host, Rise of the Triad's timedemo 4 and Doom's demo3 with a card fitted give the WAV cli.js --wav gives, byte for byte (8,619,724 and 15,956,420 bytes; test-c4pc holds the first); on c4pc.elf in QEMU with KVM and -device sb16, Doom's demo3 plays its music and sounds through the card (QEMU's wav audiodev records them) at 190 frames a second |
| done (M8) | x86-64: c4pc64.elf, the same machine built for long mode (pc/boot64.S), on the ISO beside the i386 one |
| *measured* | under KVM the session byte for byte libjs's in its cycles, with the MMU and without; every acceltest; Doom's demo3 with the SB16; the desktop and Doom on it. With the MMU 24% faster than c4pc.elf (194M against 157M c4 instructions a second), without it 3% |
| done (M6) | drive 1 kept on an IDE disk (pc/store.c), where C4IX's persist saves, as the page keeps it in IndexedDB and cli.js in a `-w` folder |
| *measured* | under KVM on both builds: a file written and synced in one boot is there in the next, and after a second save both are; the store's drive 1 is what c4pc-host's `-w` folder gets, file for file |
| done (M7) | several CPUs: libjs's round robin (smp.c, `--cpus N`) and its parallel engine (par.c): a thread each in c4pc-host (`--parallel`), and on a PC each of its own processors, started by INIT and Startup IPIs (pc/ap.c, pc/aptramp.S) |
| *measured* | taking turns: c4mp's transcripts, and the session with the MMU on 4 CPUs byte for byte libjs's `--cpus 4`, in its cycles; libjs's C4IX tests for several CPUs (test-c4ix-smp.sh) pass on c4pc-host at 2 and 4, taking turns and on threads; on c4pc.elf and c4pc64.elf in QEMU with 4 processors, what test-libjs asks of par.js, and C4IX on all four; four compiles at once take a third of the time on 4 processors (section Several processors) |
| done | a CD to copy from (cdrom.c): the PC's ATAPI drives from drive 2, ISO 9660 with Rock Ridge or Joliet names, read when wanted; C4IX's `mount`, `umount` (and eject), `df` and `cp`; `c4pc-host --cdrom FILE.iso` |
| *measured* | on c4pc-host with an image and on c4pc.elf in QEMU with KVM, the ISO as one CD and a test disc as the other: the disc's files listed, read and copied byte for byte, kept on the store by `sync` and there at the next boot |
| done | block devices (dev.c BLK_*): the PC's IDE disks but the store's (pc/ata.c, LBA28 and LBA48, by the controller's bus-master DMA, else PIO in multiple mode), `c4pc-host --disk FILE.img`; C4IX keeps ext2 on them, its own (docs/c4ix-ext2.md) |
| *measured* | in QEMU with KVM, helios (itself a Hyper-V VM): by PIO, writes 38 KB a second and CD reads 90 KB a second -- a request a sector, and the CD's read a word at a time -- so that install's 160 MB would have taken hours; by DMA a 1.9 MB file is written to ext2 in 0.48 s (was 50) and 1.8 MB read from the CD in 0.39 s (was 20) |
| done | the machine off, the power button and a restart (pc/acpi.c): ACPI's S5 from the FADT and the DSDT's \_S5, the fixed power button polled and handed to C4IX as SIGTERM (a clean shutdown), C4IX's `shutdown` and `reboot` |
| *measured* | in QEMU with KVM (pc and q35, c4pc.elf and c4pc64.elf): system_powerdown at the prompt, and `shutdown`, are C4IX's shutdown and then the VM off; `reboot` resets the PC; SIGTERM to c4pc-host or cli.js is the same shutdown. VMware not yet tried |
| done | installed onto an IDE disk from the ISO (C4IX's `install`, docs/c4ix-install.md): GRUB and c4pc on the disk, and the whole system, / on its ext2 (`--root`): nothing unpacked at boot, every change kept |
| *measured* | in QEMU with KVM: from the ISO's menu the system installs onto a blank 2 GB disk in 89 s (4,716 files, 220 MB); e2fsck finds it clean and /boot's files are the build's byte for byte; the disk boots alone through its own GRUB, its text entry and its desktop, a file saved is there at the next boot, and the power button shuts it down clean |
| done | the ISO's boot disk compressed (pc/packdisk.py: each file gzipped alone, as NAME.c4pcz), inflated by c4pc's drive when a file is first read (drive.c, inflate.c), what is inflated kept to 96 MB; `c4pc-host --tar FILE` loads such an archive |
| *measured* | the boot disk 152 MB to 48.5 MB (3,235 of its 3,721 files compressed); every file inflated is the original byte for byte; test-c4pc holds the session from the packed archive to the plain one's, byte for byte and in its cycles |
| done | the 64-bit system (docs/c4pc-64.md): the engine at either word size from one source (`C4PC_W64`: c4pc-host64w, c4pcw64.elf), and C4IX compiled 64-bit -- kernel, libraries, programs, desktop, compilers -- on a disk of its own (disk64.sh); three ISO entries beside the 32-bit ones; the MMU at 64 bits (512 eight-byte entries a table). The games stay on the 32-bit system |
| *measured* | test-c4ix's session and 18 test programs byte for byte native c4m's (`c4pc-host64w --c4m`), c4mp's SMP transcripts on 4 CPUs; c4pcw64.elf in QEMU byte and cycle for cycle c4pc-host64w; the 64-bit desktop, a CD, ext2, 4 CPUs; from the ISO under KVM, the desktop (on 1 and 4 processors) and the text entry; `cc` and `c4cc` compile and run a program inside |
| not yet | General MIDI (no synth: games play the AdLib), an AHCI (SATA) or NVMe disk, or a SATA CD drive |

## One engine, two front ends

```
src/c4pc/
  c4pc.h  clib.h        the machine, and the C library calls it makes
  engine.c              libjs/c4m.js's interpreter, traps and syscalls, the user window; boot.js's loader and trampoline
  mmu.c                 libjs/mmu.js: the MMU's registers, translation, the user loop; arena.js's kernel map
  smp.c                 libjs/smp.js: several CPUs taking turns
  cdrom.c               CD drives (ATAPI) and ISO 9660, as drives from 2; an ISO image's for c4pc-host
  par.c                 libjs/par.js and ap.js: several CPUs at once, the devices CPU 0's
  dev.c                 src/c4bb/sim/devices.js: the registers at 0x100-0x1FF
  accel.c accel.h       libjs/accel.js's loader and kit; src/c4ix/accel.mjs: C4IX's inflater and CRC-32
  games/                the games' accelerators, games/NAME/c4/accel.mjs each in C
  gui.c                 libjs/gui-device.js's display at 0x400, and display.js's drawing
  snd.c snd.h           libjs/snd-device.js's sound card at 0x480, and snd-mix.js's mixer
  opl.c mkopl.py        libjs/opl.js: the OPL2/OPL3 (DOSBox's dbopl), its tables from libjs/opl-tables.js
  font.c mkfont.py      the display's text: outlines from the system's free fonts, filled at any size
  heap.c                libjs/heap.js, block for block
  fmt.c                 libjs/printf.js (PRTF), with JavaScript's float formatting
  fpu.c                 src/c4bb/sim/fpu.js (F3) and c4m.js's FLT
  drive.c util.c        drives (and ustar archives as drives), maps, blobs
  host.c                c4pc-host: Linux, with libjs/cli.js's flags
  pc/                   c4pc.elf: boot.S, pc.c (the PC), screen.c (framebuffer, mouse), sb16.c and hda.c (sound), store.c (drive 1 on a disk),
                        ap.c and aptramp.S (the other processors), libc.c, link.ld;
                        c4pc64.elf: boot64.S in boot.S's place
  dos/                  C4PC.COM: the DOS loader
  test-c4pc.sh          make test-c4pc: C4IX on libjs, c4pc-host and QEMU, without an MMU and with one
  test-c4pc-mmu.sh      ... test-c4ix-mmu.mjs's wild programs, on libjs, c4pc-host and QEMU
  test-c4pc-progs.sh    ... and test-libjs's native-oracle programs on libjs and c4pc-host
```

**Two things a bare i386 needed that Linux did not.** Ubuntu's libgcc is
built with CET, so its 64-bit division starts with `ENDBR32`, which
DOSBox's cores do not know (QEMU and real CPUs read it as a NOP): c4pc has
its own `__divdi3` and friends (pc/libc.c), and everything is built
`-fcf-protection=none`. And the x87 rounds to 64 bits unless told: pc.c
sets it to double, so that fpu.c's doubles are IEEE's.

Each C file names the JavaScript it follows, and keeps its order and its
comments' reasons, so that a change to libjs has one obvious place to go.
c4pc-host takes cli.js's flags (`-m -c -s -d -w -i -hz`), so one can stand
in for the other in a test.

**The MMU.** `--mmu` (c4pc-host's flag, and c4pc.elf's on its command
line) fits mmu.c, as cli.js's does; INFO then has C4I_MMU, and C4IX turns
it on. Without it C4IX runs as libjs runs it with `--no-mmu`, the default
of both. mmu.js throws a FAULT from deep in sys(), the FPU or a device;
mmu.c longjmps instead, to the innermost of the places mmu.js catches it
(a `c4_jmp_buf` stack through `u->jb`: libc's `_setjmp` on Linux,
pc/boot.S's own on the bare PC). mmu.js runs a copy of its user loop
without the windows' offsets while they are all 0 and hands over to the
general one, a cycle later, when one is not; mmu.c has one loop and
counts that cycle the same way, so that the two agree to the cycle.

**What differs from libjs.** No mailbox, downloads or General MIDI
synth, so INFO says so and C4IX does without; on several CPUs at once,
the swap's registers run where they are touched (section Several
processors). Past
the arena's end libjs's typed arrays read 0 and drop the write, where
c4pc's word index wraps inside the arena: only a wild program can tell.

## Speed

*Measured*, the test session (1,474M c4 instructions), on helios:

| machine | c4 instructions a second |
|---|---|
| libjs (node 24), `--no-accel --no-osaccel` | 153–155M |
| c4pc-host (gcc -O2, x86-64) | 292–312M |
| libjs `--mmu` (most of the session is user mode, in mmu.js's loop) | 131–135M |
| c4pc-host `--mmu` | 223–226M |
| c4pc-host built as c4pc.elf is, `-m32 -march=i486` | 228M; `--mmu` 197M |
| c4pc.elf in QEMU 10.2 with KVM, `-cpu host` (helios is a Hyper-V guest: KVM nested) | 180M |
| c4pc.elf `--mmu` in QEMU with KVM | 140M |
| c4pc.elf in QEMU 10.2, TCG | 15.1–15.5M |
| c4pc.elf `--mmu` in QEMU, TCG | 13.4–13.6M |
| c4pc.elf in DOSBox-X 2026.01, dynamic core, `cycles=max`, through C4PC.COM | 13.5M |
| c4pc.elf in Bochs 2.8 from a GRUB ISO, the disk less the games, `clock: sync=none` | 5.3M (output byte for byte c4pc-host's on the same disk) |

With KVM (2026-11-22, after nested virtualization was turned on for
helios), c4pc.elf is faster than libjs: 12 times TCG without the MMU,
10 times with it, and about 60% of c4pc-host. Part of the gap is the
build: a 32-bit i486 build of c4pc-host is 22% slower than the x86-64
one (13% with the MMU), with fewer registers and no 64-bit arithmetic.
The guest loses 21% more without the MMU and 29% with it. Perhaps the
MMU's tables miss the TLB more, and each miss walks nested page tables
(a guess, not measured). The output is byte for byte c4pc-host's in all
four QEMU runs.

The study guessed 15–40M in DOSBox: the low end. Under the emulators a
C4IX shell, `ps` and `top` are usable; under KVM the machine is faster
than libjs. The interpreter is libjs's loop as it is. c4mp's
register-held dispatch, threaded code and a translator (M9) are where
more would come from; so might an x86-64 build (M8), for the registers.

### The games' accelerators

libjs runs a game's innermost loops in the game's own JavaScript
(docs/accel.md): a column, a span, a blit, a 64-bit multiply, one
instruction however much it does. c4pc has the same, in C: games/NAME.c
is games/NAME/c4/accel.mjs line for line, with accel.h's kit (readers and
writers with a page each, `acc_probe`, `acc_span`, the arguments) as
accel.js's, and a `hold_t` for the loops that read what they write (a
blend, a shadow), whose writes wait until every page they go to is known
to be there. The arithmetic is JavaScript's: int32 wrapping, shifts by
their count mod 32, a single's operations done in double and rounded
(`fr`, Math.fround), which the x87 set to double precision makes the
single's own. The modules are numbered as libjs's full build lists them
(src/c4bb/images/accel.json): doom, duke3d, quake, rott, sbk, tyrian,
wolf3d, then C4IX's own. Each game asks with ACC_FIND, and its menu's
switch (ACCELERATED) turns them off as on libjs; `--no-accel` takes the
loader away.

*Measured* (2026-11-23, helios): each game's acceltest (its loops
against the C they stand for, on made-up data) passes on c4pc-host and on
c4pc.elf in QEMU with KVM, with libjs's count of cases (Doom's 14,556,
Quake's 1,000). Each timedemo shows the same frames with the accelerators
and without, on the bare machine and inside C4IX with the MMU; Doom's
demo3 01914fe7, Duke's script 96ab05bb and Quake's demo3 1f87d635 are the
checksums docs/accel.md gives. On c4pc-host, seconds without and with:

| timedemo | bare machine | in C4IX, the MMU on |
|---|---|---|
| Doom demo3 | 31.5 / 9.7 | 44.9 / 12.8 |
| Wolfenstein 3D demo 0 | 9.6 / 2.6 | 22.0 / 6.3 |
| Rise of the Triad demo 4 | 8.0 / 3.5 | 16.6 / 5.3 |
| Tyrian demo 4 | 11.5 / 1.7 | 21.1 / 3.3 |
| Super Blit Kart -demo | 133.6 / 25.9 | |
| Duke Nukem 3D, 600 frames of the script | 14.7 / 8.2 | 27.3 / 13.0 |
| Quake demo3 | 47.7 / 25.1 | 61.7 / 29.6 |

### Sound

snd.c is libjs/snd-device.js and snd-mix.js in C: the card's registers
at 0x480 (bus.js), the events they make stamped with the card's clock
(OPL writes at the stream's cursor, which WAIT moves on, the rest at
now), and the mixer that makes them samples: opl.c (libjs/opl.js, which
is DOSBox's dbopl line for line, its tables from libjs/opl-tables.js
through mkopl.py), sixteen channels of 8-bit samples, the PC speaker's
square wave, the card's MASTER, each stage rounded to a single as the
page's Float32Arrays round it. CAPS says there is no General MIDI synth,
so a game plays its music on the AdLib.

`c4pc-host --wav FILE` fits the card and writes the session's sound when
it ends, as cli.js's `--wav`: the card's now is the machine's clock
(cycles and sleeps), so the file is the same every time, and libjs's,
sample for sample.

On a PC the card is fitted at the keyboard when there is a Sound Blaster
16 (pc/sb16.c: the DSP's reset at 0x220 or 0x240, version 4 or more, the
16-bit DMA channel its mixer names): `c4pc: sound: SB16 at 220h, DSP
4.5, 16-bit DMA 5`. The mixer's output goes round a ring of 8,192 frames
at 44,100 Hz by the DSP's auto-initialised DMA, and the run loop, after
every slice and at every tick it waits through, reads how far the DMA
has got and mixes the ring 2,048 frames (46 ms) ahead of it again; no
interrupt reaches the CPU. The card's now is then the real clock, and the
events of now go 10 ms after the last frame made. QEMU (`-device sb16`),
DOSBox, DOSBox-X and VMware (`sound.virtualDev = "sb16"` in the VM's
.vmx) have one; `--no-sound` leaves it be.

Where there is no Sound Blaster, Intel High Definition Audio (pc/hda.c):
VMware's default sound device, VirtualBox's, QEMU's `-device intel-hda
-device hda-output`, and most PCs since 2004. The controller is the first
PCI function of class 0403h, its registers at BAR0; after its reset the
first codec that answers is spoken to through the CORB and the RIRB
(RIRBSTS cleared after every answer: QEMU stops at RINTCNT until it is).
In its audio function group every pin that can output and is connected
(a line out, a speaker or headphones, by its default configuration) is
walked back through its connections to a DAC, every node on the way
powered, its input chosen and its amplifiers unmuted at 0 dB, and every
such DAC put on stream 1, 44.1 kHz, 16 bits, stereo. The first output
stream reads the same ring as the SB16's, its two halves a buffer
descriptor list of two, and the poll goes by its link position (LPIB):
`c4pc: sound: HD Audio, codec 1af4:0012, outputs: 1` under QEMU.
`--debug-sound` prints the codec's nodes, their connections and their
pins' configurations. A PC with neither runs silent, and says why.

Where there is neither, the Ensoniq AudioPCI (pc/es137x.c): the ES1371,
VMware's sound device for many guest types -- the user's VMware said
"no Sound Blaster 16 ...; no HD Audio controller" -- its CT5880 and
EV1938 kin, and the ES1370 before it (QEMU's `-device ES1370`). The
first on any PCI bus (VMware puts devices behind its PCIe root ports; the
HD Audio probe looks on every bus now too), its registers I/O ports at
BAR0. DAC2 goes round the same ring, 16-bit stereo, looping, no
interrupt, and the poll reads its frame's count (the DAC page's 0x3c,
high half). The ES1370 divides its clock for 44.1 kHz (CONTROL's
PCLKDIV, 1411200 / 44100 - 2) and has an AK4531, reset and opened; the
ES1371 sends DAC2 through its sample-rate converter, whose RAM is
cleared and set before it is enabled, as Linux's ens1370.c does (one
left unset locks the chip), and has an AC'97 codec: reset, then master,
headphone and PCM out at 0 dB. `c4pc: sound: Ensoniq AudioPCI ES1371,
DAC2 at 44.1 kHz`. *Measured*: on QEMU's ES1370, Doom's title loop
recorded at the SB16's levels second for second; the ES1371's converter
and codec are VMware's to try. In a VM's .vmx, `sound.virtualDev =
"sb16"` or `"hdaudio"` chooses the other two.

*Measured* (QEMU with KVM, Doom's demo3, QEMU's wav audiodev recording
what the card played): through the SB16 and through HD Audio, on
c4pc.elf and c4pc64.elf, the music and the sounds at levels of 4,000 to
8,000 RMS a second, 4 or 5 underruns each, all while the game loads.
Untried on VMware's and on real hardware's codecs.

### x86-64

`make c4pc64.elf` builds the same sources for long mode: twice the
registers and 64-bit arithmetic for the interpreter's loop, and SSE for
the floats (IEEE's doubles, with no x87 precision to set). pc/boot64.S
takes the 32-bit protected mode a multiboot loader leaves, maps the first
4 GB to itself in 2 MB pages (boot_pd, whose framebuffer pages screen.c
gives the PAT's write-combining entry), turns on PAE, SSE, EFER.LME and
paging, and calls pc_main as boot.S does; its interrupt stubs save every
register for isr() (pc.c's `struct regs` and 16-byte gates under
`__x86_64__`), and its c4_setjmp keeps rbx, rbp and r12-r15. Built with
`-mno-red-zone`, since an interrupt pushes its frame on the stack the
code is using; linked for x86-64 and made an ELF32 (objcopy), which is
what multiboot loaders take. A CPU without long mode gets a line on the
screen saying to boot c4pc.elf. DOSBox has no long mode: its package keeps
c4pc.elf.

*Measured* (2026-11-23, QEMU 10.2 with KVM on helios, test-c4pc's
session, two runs each, alternating):

| | c4pc.elf (i386) | c4pc64.elf |
|---|---|---|
| no MMU | 211M inst/s | 217M |
| the MMU on | 157M | 194M |

Both print libjs's output byte for byte in its 1,474,118,389 and
1,500,630,450 cycles. The MMU's user loop is where the registers tell:
C4IX's desktop and every game run with it.

### The 64-bit system

c4pc64.elf's guest is still the 32-bit machine. The 64-bit machine is the
same sources built with `-DC4PC_W64` (c4pc.h's `word` is then int64_t,
native c4m's word): `make c4pc-host64w c4pcw64.elf .c4pc/disk64.tar`, and
the ISO's "C4IX 64-bit" entries boot c4pcw64.elf with the 64-bit kernel and
its own disk. docs/c4pc-64.md has the design (the word, the memory map and
the devices at 64 bits, the disk), the tests (test-c4pc64.sh, run by
test-c4pc; `NO_W64=1` leaves it out) and stage two: the 64-bit MMU and the
games.

    make c4pc-host64w .c4pc/disk64.tar
    ./c4pc-host64w -s -m 64 -d .c4pc/disk64 .c4pc/disk64/c4ix.c4r
    make c4pcw64.elf
    qemu-system-x86_64 -accel kvm -cpu host -m 512 -display none -serial file:serial.txt \
        -device isa-debug-exit,iobase=0xf4,iosize=4 -kernel c4pcw64.elf \
        -append "-s -m 64" -initrd ".c4pc/disk64/c4ix.c4r,.c4pc/disk64.tar drive,in.txt stdin"

### A drive that outlasts the machine

C4IX keeps what changes on host drive 1 (src/c4ix/user/persist.c): at its
first boot the unpacked system as one tar (c4ix-base.tar, read from there
afterwards), and at each save (`sync`, the desktop's) the changes since
boot as another, c4ix-persist.tar or -b.tar in turn. The page keeps drive
1 in IndexedDB, cli.js in its `-w` folder; c4pc on a PC keeps it on a disk
(pc/store.c).

The disk is an IDE one, spoken to by PIO (1F0h and 170h, master and
slave, LBA28): QEMU's `-drive file=X,format=raw,if=ide`, VMware's and
VirtualBox's IDE disks, a PC's in IDE mode. It is the store when its first
sector says `C4PC STORE 1`; with `--store` (on the ISO's desktop entries)
a disk whose first 64 sectors are all zero, a new blank virtual disk,
is made one. No other disk is written. Its layout: the header, 64 sectors
of directory (256 entries: a name, a run of sectors, a length, a CRC-32),
then the files, each in one run. A file written goes to a run no file uses,
not even its old self, and only then is its directory sector written, so
a write cut short leaves the old file or the new one; at boot every file
is read back into drive 1 and its CRC checked. `c4pc: drive 1: the store
on the primary master, 64 MB: 5 files, 31163 KB` says what it found;
`--no-store` leaves the disks alone.

*Measured* (QEMU with KVM, a blank 64 MB image, both builds): a file
written after persist's boot has noted the system, then `sync`, is there
at the next boot (`cat` prints it); a second save onto the same store,
and a third boot has both. 19 MB of base tar and 6 MB of changes a save.
A file written before persist has noted the system counts as the
system's, as on libjs, and is not saved.

To give a VMware VM one: add a hard disk on an IDE controller (a SATA or
NVMe one is not spoken to yet), new and blank, of 256 MB or more.

### Several processors

C4IX runs on several CPUs (docs/c4ix-smp.md): an idle task and a run
queue each, a kernel lock, c4mp's ten processor opcodes (CPUI to IPI) and
a window of 64 words at 0x800 that each CPU sees its own of. libjs has two
engines for them, and c4pc has both.

The menus' "every processor" entries (32-bit and x86-64) pass `--cpus
all`; the others run one CPU, as the web page does, so a VM given four
processors still shows one in the System Monitor unless one of those is
chosen. `c4pc: 4 processors, each the machine's CPU on one of the PC's
own` in the boot log says how many came up.

**Taking turns** (smp.c, smp.js line for line): `--cpus N` (and `-q`,
the quantum) makes N machines over one arena, heap and devices, and CPU
0's run hands over to a round robin that runs each runnable CPU for a
quantum. Every instruction is atomic, so CAS and the rest are plain
read-modify-writes, and a run is libjs's to the byte and the cycle. Each
CPU is a `machine` of its own, its registers, trap state and MMU; its
`dv` names the machine whose device fields are the machine's (CPU 0's),
so the devices (dev.c) read `m->dv->...` for themselves and `m->...` for
the CPU that reached them, as devices.js reads `dev.machine`.

**At once** (par.c, par.js and ap.js): `--parallel` on c4pc-host gives
each CPU but 0 a thread, and on a PC `--cpus N` (or `all`) gives each one
of the PC's processors. CAS, XCHG and FADD are the host's own atomic
instructions on the guest's word; CWAI parks until the word moves, a
CWAK, an IPI or the end, looking again every 10 ms as ap.js does. CPU 0
keeps the devices and the console: another CPU that reaches them (OPEN,
READ, PRTF and the rest, a device register, a message) asks CPU 0, which
runs the same code on the asking CPU's own machine while it waits; CPU 0
answers between slices of 16K cycles, and at once when it is idle. The
sound card's OPL and PCM writes are queued instead (par.js's posted
writes). The heap every CPU uses itself, under a lock, and so the swap's
pages.

One thing c4pc does that libjs does not yet: the swap's registers
(SWAP_ADDR, SWAP_CMD) run on the CPU that touches them, under the swap's
lock, rather than being asked of CPU 0. C4IX turns the heap's high end on
and off through them around every allocation a task makes
(src/c4ix/mmu.c mmu_sop, under a lock of its own), and asked, they were
nearly all the requests: a boot and a compile on 4 CPUs made 212,550, with
13.5 s of round trips, and took 19 s against 5.4 s on one CPU. Run where
they are touched, 565 requests and 5.7 s. libjs's par.js still asks
(the same session: 9.4 s on one CPU, 26.3 s on four).

**On a PC** (pc/ap.c, pc/aptramp.S): the boot processor's local APIC
sends INIT and two Startup IPIs to all the others. Each starts in real
mode at 0x7000, where the trampoline takes it to the boot processor's
protected mode and paging (and, for c4pc64.elf, long mode), a stack by
the order it arrived in, and ap_entry, which loads the kernel's GDT and
IDT and waits in hlt for the engine. How many processors there are is how
many arrive. A processor with nothing to do waits in hlt; a kick is an IPI
at vector F0h, sent only to one that is waiting, and a wait with an end
gets its IPI from the boot processor's 1 kHz tick. Where no other
processor answers (DOSBox, a VM of one), the CPUs take turns on the one
there is.

*Measured* (2026-11-23, helios; c4pc-host on the 4 processors c4-lock
gives; QEMU 10.2 with KVM and `-smp 4`): four compiles of mandel.c at once
inside C4IX, with the MMU, less the same session without them:

| | 1 CPU | 2 CPUs | 4 CPUs |
|---|---|---|---|
| c4pc-host, on threads | 18.6 s | 10.4 s | 5.6 s |
| c4pc.elf in QEMU | 27.3 s | | 8.5 s |
| c4pc64.elf in QEMU | 21.4 s | | 7.8 s |
| libjs, `--parallel` (asking for the swap's registers) | 31.4 s | | 56.6 s |

And the whole of a session that rebuilds C4IX's libraries, `b4ke -B -j`
(13 steps; the boot and the root archive's unpack included), the
libraries byte for byte the disk's each time:

| | 1 CPU | 4 CPUs |
|---|---|---|
| c4pc-host, on threads | 46.0 s | 19.9 s |
| c4pc.elf in QEMU | 69.2 s | 29.7 s |
| c4pc64.elf in QEMU | 53.4 s | 23.8 s |
| libjs, `--parallel` | 81.7 s | 79.6 s |

The four programs are byte for byte the same each time. libjs's spinmp, 4
CPUs summing 70 million each with nothing to ask, does four CPUs' work in
2.06 s on c4pc-host against 1.50 s for one CPU's. A game is one task, and
runs on one CPU however many there are.

### A second CD, to copy from

A VM's way to give it files is a second CD drive with an ISO in it.
c4pc speaks to CD drives on the IDE channels, as store.c does to disks:
ATAPI's PACKET commands, polled (TEST UNIT READY, REQUEST SENSE,
READ CAPACITY, READ(10), START STOP UNIT to eject). READ(10) goes by the IDE
controller's bus-master DMA where it has one (PCI IDE: the PIIX in
VMware, QEMU's pc machine, a real PC), the rest by PIO. Each is a host drive
from drive 2 on (drive 1 is persist's, an empty read-only one where
there is no store). The disc is ISO 9660, its names Rock Ridge's where
it has them (Linux's mkisofs -R, xorriso), else Joliet's (Windows'),
else its own less ";1".

Nothing is read ahead. C4IX's `mount cdrom` makes /mnt/cdrom a host
directory of "N:/" (vfs.c vfs_lazydir, as the games' directories are):
when it is first looked in, the kernel asks for N:/.c4ix-dir, which
cdrom.c makes from the directory's records ("SIZE NAME" or "d NAME", a
line each); a file is read from the disc a 32 KB run at a time as it is
read (dev.c's drive hooks, an fd of kind FD_DRV), never whole. So a disc
of any size mounts at once, and costs memory only for what is copied off
it: `cp -r /mnt/cdrom/stuff /home` makes RAM files, which `sync` (or the
desktop's save) keeps on the store, and the next boot reads from there.

Each drive says what it is in N:.c4pc-drive ("kind cdrom", "label",
"size", "used", "free", a line each; the boot disk's and the store's
too), which `mount` (alone: the mounts and the drives), `df` and `mount
cdrom` read: the first CD drive with a disc in it that is not c4pc's own
(the ISO's volume id is C4PC). `mount` looks at the disc again first
(DISK_RESCAN), so a disc changed while nothing was mounted is the new
one; `umount` lets the directory's entries go and opens the tray
(DISK_EJECT), and VMware lets the ISO go with it. A file open there
keeps the mount (busy).

    mount cdrom
    cp -r /mnt/cdrom/project /home
    sync
    umount

In VMware: a second CD/DVD drive on an IDE controller (a SATA one is not
spoken to), its ISO connected. On c4pc-host, `--cdrom FILE.iso` is the
same with an image, for the tests; on a PC, `--no-cdrom` leaves the CD
drives be (test-c4pc does, so that its sessions are libjs's: QEMU has an
empty one by default).

*Measured* (2026-11-23, QEMU 10.2 with KVM): c4pc.elf from `-kernel`
with the ISO on the secondary master, a test disc (xorriso: Rock Ridge
and Joliet, a 5 MB file, long names with spaces, folders three deep) on
the secondary slave and a blank 256 MB disk for the store: `mount cdrom`
takes the test disc, its files list and read with the CRCs their
originals have, `cp -r` copies them, `sync` keeps them, and the next boot
has them. On c4pc-host the same with Rock Ridge, Joliet-only and
xorriso's default discs.

### Off, the power button, and a restart

C4IX's `shutdown` (and `reboot`, or `shutdown -r`) asks the kernel
(SYS_SHUTDOWN, sched.c's sched_halt): every disk's changes are written,
every program gets SIGTERM, and what is still there 3 seconds later is
killed; init's wait for the shell returns, and the kernel task's end
unmounts the disks clean (boot.c). Then the machine stops, with status 0
(off) or 82 (C4IX_EXIT_REBOOT: restart).

On a PC, off is ACPI's sleep state S5 (pc/acpi.c), as an OS without an
AML interpreter does it: the RSDP, the RSDT or XSDT, the FADT's PM1a and
PM1b control blocks, and the DSDT's \_S5 package for SLP_TYPa and
SLP_TYPb; SLP_TYP | SLP_EN into PM1a_CNT turns off VMware's VM, QEMU's or
a real PC. QEMU's, Bochs's and VirtualBox's fixed ports stay as the
fallback, after QEMU's isa-debug-exit (the tests' status). A restart is
the FADT's RESET_REG, then the chipset's 0xCF9, then the keyboard
controller's pulse, then a triple fault.

The power button -- VMware's "Shut Down Guest" without VMware Tools,
QEMU's system_powerdown, a real one -- is ACPI's fixed power button. At
boot c4pc puts the chipset in ACPI mode (ACPI_ENABLE to SMI_CMD) and sets
PWRBTN_EN, so that a press is PWRBTN_STS in PM1_STS rather than the
firmware's; the SCI's IRQ stays masked and the run loop looks every 50
ms. A press is SIGTERM to the program, as Ctrl-C is SIGINT: C4IX takes it
as `shutdown`. A second press a second or more later, or a program that
takes no SIGTERM, is the machine off at once, as holding a real button
is. c4pc-host and cli.js take their own SIGTERM the same way (`kill PID`:
C4IX shuts down; a second `kill` a second later ends the process).

    shutdown        # or the VM's power button
    reboot

The boot says which it found: "c4pc: ACPI: off by S5, the power button
shuts C4IX down (by the SCI)". The press comes by ACPI's interrupt, the
SCI (the FADT's SCI_INT, IRQ 9 on most), with every GPE's enable cleared
so that nothing else raises it; polling the button instead is a port read
each time, a trip out to the hypervisor, which is why it is only the
fallback (an SCI that is not an ISA IRQ, or one that keeps firing with
nothing to say). A control method power button (FADT flag PWR_BUTTON) is
the AML's, which needs an interpreter: c4pc says so, and VMware's, if it
is one, may then still need `shutdown` typed. `--no-acpi` leaves ACPI be:
off by the fixed ports alone, and no power button.

**The PC's own keys**, read by c4pc whoever has the keyboard (pc.c):
Ctrl+Alt+Del restarts -- SIGTERM to C4IX, as the power button, which
shuts down, and then the PC resets (status 82) where it would have
switched off; pressed again before that, at once. Ctrl+Alt+F1 puts the
text console on the screen and sends the keys to it (its shell, the
boot's log), whatever the display was showing; Ctrl+Alt+F2 the display
again, drawn whole. *Measured* in QEMU: a command typed at the console
after Ctrl+Alt+F1 ran, Ctrl+Alt+F2 showed the desktop as it was, and
Ctrl+Alt+Del ended in "shutdown complete" and the PC restarting.

### The boot disk, compressed

The ISO's boot disk -- the tar GRUB hands c4pc as drive 0, which stays in
the PC's memory while it runs -- is .c4pc/diskz.tar, .c4pc/disk.tar with
its files compressed (pc/packdisk.py): each one gzipped alone at level 9
and named NAME.c4pcz, unless it is compressed already (.tgz, .zip ...) or
gzip does not make it a tenth smaller; a name the archive has twice is
the later one's. 152 MB is 48.5 MB so: programs to a fifth, sources to a
quarter, the games' data to a little under half.

c4pc's drive (drive.c) takes a .c4pcz member as NAME, of the size its
gzip trailer says, and inflates it (inflate.c: stored, fixed and dynamic
DEFLATE blocks, the CRC checked) the first time it is read, into memory
of its own. C4IX never sees the difference: c4ix.sizes, the listings and
every read are the file's own. What is inflated is kept while it all fits
96 MB; past that the one least recently read and not open is let go of,
to be inflated again if it is wanted -- so `install`, which reads every
file once, extracts the whole system onto the disk in passing. The DOS
package (c4pc-dos) is the packed archive too. `c4pc-host --tar FILE`
loads an archive as a drive, packed or not, as a PC's loader hands one
over.

### The clock on several processors

At the keyboard the machine's TIME is the real clock (`m->wall`), and on
processors on threads every CPU reads it for itself: `plat_now_ms`, the
TSC's cycles since a base clock_init sets once, divided whole (two 32-bit
divides), a TSC a little behind the first processor's (a VM's vCPUs)
taken as no time since, and never earlier than any processor last read
(`clock_seen`). It once folded its base on as it read, unlocked: two
processors at it at once were 256 ms out, and a TSC behind made it fold
on for ever, so a game's clock leapt ahead -- Quake pinned to CPU 1 (`pin
1 quake --window`) played twenty seconds of its demo in a few. A read of
TIME on a CPU but 0 is its own (engine.c AP_DEV), not a round trip to CPU
0 as the other devices are: Doom asks the time between frames, and on
CPU 1 drew a few frames a second at 100%.

**Under VMware** the clock is the host's own time, read through the
backdoor (vmware.c: GETTIMEFULL, else GETTIME; `c4pc: the clock:
VMware's host time`): VMware's TSC is the VM's "apparent" time, which it
runs fast to catch up after the VM fell behind, and Quake, loading, then
played the first ten or fifteen seconds of its demo fast. A read is a VM
exit, a microsecond or two. Only where CPUID's hypervisor leaf says
VMwareVMware, or with `--clock vmware`; where the host gives no time, the
TSC as before. QEMU's vmport answers neither command (0xffffffff), so it
is VMware's to try. In QEMU (KVM's steady TSC) Quake's demo1 lasts 75 s
and demo2 70 s, on one processor and pinned to CPU 1 of four alike.

**Frames a second**: Ctrl+Alt+F11 shows the display's frames a second
(its presents and flips, a second at a time) in a box in the screen's
top right corner, over a game or the desktop, and hides it again.

### Memory, and swap

C4IX gets all of the PC's memory unless `-m N` says otherwise: the arena
is the largest power of two (the engine's word index wraps at its size)
that leaves c4pc 160 MB of its own -- the drives' unpacked files, 96 MB
at most, the display's buffers, swap's pages where there is no swap
partition -- and at most 1 GB (512 MB on the 64-bit word), the user
window going above it (c4ix-mmu.md
section 11): 512 MB on a VM of 1 GB, 1 GB on one of 2 GB or more. The
boot says `c4pc: 512 MB arena with an MMU (the PC's memory but c4pc's
own; -m N for less)`. `-m` stays for later, for a machine that shares
the PC.

Swap is the host's (SWAP_CMD, dev.c): a page C4IX's MMU puts out is kept
by c4pc, which put it in its own memory, up to 256 MB (as libjs does in
the page's). With all the memory C4IX's, that is little or none; the swap
partition install makes (type 82h, c4ix-install.md) is where pages go
when there is one, 8 sectors a slot, a bit a slot saying which were
written since the boot's reset, and a page the kernel reaches while it is
out (a device's DMA, its own loads) read into memory until it comes in.
The disks' block I/O, C4IX's and swap's, is one at a time, the processors
sharing the disks.

GRUB's desktop entries pass init `--no-top`: --desktop's top on the
console is the web page's, where the console shows; here it is the
serial line, which no one sees, and it filled its log. `top` at the
serial prompt runs it.

### The hypervisor

`--hv` (GRUB's "C4PC hypervisor" entries) makes the machine c4pc boots
VM 0, a control C4IX that makes and runs others -- each a c4 machine with
its own memory, display and drives, its disk a file on VM 0's ext2 read
and written where it lies -- and attaches to one, which has the screen,
keyboard and mouse until Ctrl+Alt+Home (or `detach`). C4IX's `vm` does
it: docs/c4pc-hypervisor.md.

### VMware, without VMware Tools

pc/vmware.c does what open-vm-tools and Linux's vmmouse driver do, over
VMware's "backdoor": an `in` from port 5658h with EAX the magic `VMXh`
and ECX a command, which VMware answers in the registers. A PC where EBX
does not come back holding the magic is not VMware, and nothing below
happens there; `--no-vmware` leaves it alone anyway.

- **The mouse, absolute.** When the backdoor's pointer answers
  (ABSPOINTER_COMMAND, STATUS, DATA: `c4pc: a display of 1024 x 768, and
  VMware's absolute mouse`), the pointer goes where the host's is, from
  0 to 65535 across the screen, and the VM gives the mouse up at the
  window's edge, with no grab and no Ctrl+Alt. The PS/2 mouse's packets
  still arrive, but only to say there is something to read. A program
  that locks the pointer (a game: `gui_locked`) gets relative motion
  while it holds it (REQUEST_RELATIVE), and VMware then grabs the mouse
  as before. QEMU's vmport has the same pointer (`-machine vmport=on`, on
  by default on the pc machine).
- **Copy and paste**, VMware's first way (copy/paste version 1: the
  host's text is GETSELLENGTH, then GETNEXTPIECE 4 bytes a call; the
  guest's is SETSELLENGTH and SETNEXTPIECE). The VMware drive's file
  `clipboard` is the host's text, and `clipboard.set` written there is
  the guest's. The desktop (shell.c) hooks winkit's clipboard to them
  (`wk_clip_host`): a copy in any of its windows is also the host's, and
  a paste first takes the host's text if it changed since. VMware's own
  setting has to allow it (VM, Settings, Options, Guest Isolation).
- **GuestRPC.** Messages on a channel (BDOOR_CMD_MESSAGE): a size, then
  the bytes 4 at a time, or all at once by the high-bandwidth port 5659h
  (`rep outsb` / `rep insb`) where VMware offers it; `--vmware-lb` keeps
  to 4 bytes a call. RPCI asks VMware things; TCLO is VMware asking the
  guest, polled every 100 ms and answered as vmtoolsd answers "reset",
  "ping" and the rest, so VMware takes Tools to be running (c4pc sends
  `tools.set.version 2147483647`, the version open-vm-tools gives for
  Tools VMware does not manage). **Shut Down Guest** then arrives as
  `OS_Halt`, and is the power button's shutdown (above): C4IX shuts down
  and the VM switches off.
- **Shared folders** (HGFS, protocol version 1: an RPCI request `f `
  and the packet). The VMware drive (`c4pc: drive N: VMware's`, kind
  vmware) has them under `hgfs/`: `hgfs/SHARE/...` is the host's folder,
  listed and read when something looks there (SEARCH_OPEN, GETATTR,
  OPEN, READ). `mount N /mnt/host` puts them at /mnt/host/hgfs. A mount
  keeps what is written into it in RAM, as a CD's does; `hostcp FILE...
  SHARE[/DIR]` writes onto the host (the drive hands a file written to
  `hgfs/...` to VMware whole when it is closed: OPEN with create-or-empty,
  WRITE, CLOSE), and `hostcp` alone lists the shares. A folder a mount
  has already listed shows what was written since after a umount and
  mount. Turn them on in VM, Settings, Options, Shared Folders.

*Measured* in QEMU with KVM (`-machine pc,vmport=on`): the backdoor
found, the drive made (drive 3 after the CD drive; `mount` lists it,
`hostcp` says there are no shared folders), and the pointer put where
QMP's `input-send-event` said (absolute positions; QEMU's vmmouse gives
them as VMware does, 0 to 65535); with `vmport=off`, neither, and the
PS/2 mouse as before. The monitor's `mouse_move` is relative motion,
which QEMU gives the PS/2 mouse, not the vmmouse: `--debug-mouse` logs
each vmmouse packet. QEMU has no GuestRPC, so the clipboard, TCLO (Shut
Down Guest) and the shared folders are untried there: VMware's to try.

### Installed onto a disk

C4IX's `install` puts it on the PC's first IDE disk, from the ISO, and
the disk then boots without the CD: GRUB in its first sectors, and one
ext2 partition holding the whole system as it was running, c4pc and the
kernel in its /boot. Its menu boots C4IX with `--root hd0p1`: / is the
disk's, so there is no boot drive to load into the PC's memory and no
root archive to unpack, and every change is kept. It erases the disk,
says so, and asks for "yes" first. The ISO's menu has an entry for it.
docs/c4ix-install.md has the layout and how GRUB is set up.

## Running it

The tools (QEMU, Bochs, DOSBox-X, DOSBox 0.74, mtools, xorriso, GRUB's
i386-pc modules) are unpacked on helios in `~/opt/pctools`, from the
Ubuntu archive without root; `. ~/opt/pctools/env.sh` puts wrappers for
them on PATH (`~/opt/pctools/mkwrap.sh` makes them).

**QEMU**, the headless one: `-kernel` loads the multiboot ELF and
`-initrd` its modules. The program comes first; a module whose string's
second word is `drive` is a drive (a ustar archive, `drive rw` writable in
RAM), and `stdin` is fed to the keyboard up front with end of input, as
cli.js feeds piped input. `-append` takes cli.js's `-m -c -s -hz` and
`--mmu`, `--cpus N` (QEMU's `-smp` gives it the processors; `--cpus all`,
as many as there are), then the program's arguments after `--`. c4pc
writes the status to isa-debug-exit, so QEMU exits `(status << 1) | 1`.

    make c4pc.elf .c4pc/disk.tar
    qemu-system-i386 -m 512 -display none -serial file:serial.txt \
        -device isa-debug-exit,iobase=0xf4,iosize=4 -kernel c4pc.elf \
        -append "-s -m 64" -initrd "c4ix32.c4r,.c4pc/disk.tar drive,in.txt stdin"

Where `/dev/kvm` is there (helios has it since 2026-11-22), add
`-accel kvm -cpu host`: the same session takes 8 seconds, not 96.

Without `-display none` the VGA screen shows the same console, and the
keyboard types at it. Helios has no X: there `-display curses` draws the
text screen in the ssh terminal (the wrapper sets `QEMU_MODULE_DIR` so
that QEMU finds the unpacked ui-curses module), and `-vnc :1` serves it
to a VNC viewer on port 5901.

**At the keyboard**, with no `stdin` module, the machine's clock is the
real one: TIME (and `/proc/uptime`) are milliseconds since the boot on
the PIT's clock, as the display's TICKS and the PIT's interrupt already
were. The machine runs flat out in slices of 4 ms' worth of cycles, and
a nap (USLP) waits for the real time it asked to wake at, or the PIT's
next tick, or a key or the mouse, which end it early as an interrupt
would. A game sees each frame's real length, whatever the PC keeps up
with: a slower PC gives it fewer frames, never slower ones.

The real clock is the TSC where it keeps time: under a hypervisor (CPUID's
hypervisor bit: VMware, KVM, Hyper-V give a steady TSC) or with an
invariant TSC, measured at the boot against the PIT's channel 2, polled
(`c4pc: the clock: the TSC (a hypervisor's), 3603 MHz` under KVM on
helios, whose CPU is 3600 MHz). Elsewhere (DOSBox, whose TSC follows its
emulated cycles) it is the PIT's interrupts at 1 kHz, counted, as it was
at first. Counted, they are a VM's "apparent" time: VMware holds timer
interrupts back while the VM is not running and then delivers them in a
burst, so Quake and Duke Nukem 3D sped up and slowed down in VMware with
TIME already the real clock. `--clock tsc` or `--clock pit` chooses.

First (ea38e68) TIME was the cycles' clock, cycles / MHz plus what was
slept, pulled up to the real one between slices when more than 20 ms
behind, as libjs's page does (worker.js, `realtime`). That cured the slow
motion of a PC falling behind -- Duke Nukem 3D in VMware said 50 to 250
frames a second and moved in slow motion -- but within a slice TIME
crawled at the cycles' rate and then jumped: Duke then said 40 to 240,
and turned at a speed that jumped with the frame rate. `--no-realtime`
keeps the cycles' clock, paced as cli.js `-i` paces it (a simulated
second stretching when the PC falls behind); a scripted run (a `stdin`
module) or `--fast` keeps it unpaced, cycle for cycle libjs's.

What falls behind is the games' own work, dearer than the calibration's
loop, and the drawing: before the screen was written a row at a time
(below), Doom from the desktop under KVM spent 20% of the time drawing to
the screen. `-s` at the keyboard says every 10 seconds on COM1 (not on
the screen) how the PC kept up: `c4pc: 120779k inst/s, 2% drawing to the
screen, 0% idle`; at the end, the totals.

The cycles' clock (`--no-realtime`; and the slices' size) is one the PC
keeps up with: before the boot, c4pc times
the interpreter for 100 ms on a loop of a scratch machine (engine.c
`machine_kips`: locals, a call, and a load from across 16 MB each time
round) and sets the clock to three quarters of that rate, saying so:
`c4pc: 174M c4 instructions a second here, so a 130 MHz clock`. `-hz N`
names one instead, and a scripted run (a `stdin` module) or `--fast`
keeps libjs's 20 MHz, so that it is cycle for cycle libjs's. *Measured*
2026-11-22, the loop against C4IX's own work: 177M and 180M (140M with
the MMU) under KVM, 13M and 15M (13M) under TCG. And the boot with the
MMU, `exit` at the first prompt, which waits for `c4ix-persist` to
inflate the root archive (1,500M cycles in C4IX's own C; 111M with its
accelerators, accel.c):

| | 20 MHz, no accelerators | auto clock | auto clock, accelerators |
|---|---|---|---|
| KVM | 75 s | 12 s (130 MHz, running 127M a second) | 3.1 s |
| TCG | about 112 s (behind its clock: the unpaced run's time) | 150 s (10 MHz, keeping time) | 12 s |

Under an emulator the share kept back costs a quarter; `--fast` or a
higher `-hz` trades the clock's truth for it. Keys come from the PS/2 keyboard and
from COM1, cooked as a terminal cooks them until a program opens the raw
keyboard. *Measured* in QEMU with `-serial stdio`: typed commands echo,
run, and `exit` shuts C4IX down.

**The display** (M4, 2026-11-22). `gui.c` is libjs/gui-device.js's
device, register for register at 0x400: INFO's C4I_GUI, the command and
event rings in the guest's memory (src/c4bb/sim/mbox.js's format), the
IRQ through the cycle handler, the framebuffer flips and BLIT8, checked
against the task's space under the MMU as libjs checks them. Where the
page draws the commands on a canvas (libjs/display.js), c4pc draws them
itself, as each is taken from the ring, into a back buffer, and a
PRESENT copies what changed to the front:

- rectangles, outlines and lines on whole pixels, as a canvas strokes
  them at +0.5; circles with coverage; images over what is there by their
  alpha; BLIT8 and framebuffer frames scaled by the nearest pixel
  (`imageSmoothingEnabled` false); clips as a canvas's (CLEAR inside one);
- text in c4pc's own faces, anti-aliased and unhinted, as a browser
  draws a canvas's: `mkfont.py` reads the system's free fonts with
  fonttools at build time and writes their outlines for the characters
  0-255 as C tables (`.c4pc/fontdata.c`), which `font.c` fills at any size
  and keeps. TEXT2's nine faces are DejaVu Sans Mono (mono, Lucida),
  Liberation Sans and Bold (the UI face), Liberation Mono (Courier),
  DejaVu Sans and Bold (Verdana), Liberation Serif and Bold. A string is
  the face's width, near what the window kit guesses (`ui_len * 6` to `7`),
  not the browser's to the pixel. Liberation is under the SIL OFL 1.1,
  DejaVu the Bitstream Vera licence; each face's copyright lines go into
  the generated file beside its data.

On a PC (`pc/screen.c`) the front goes to a linear framebuffer: GRUB's,
when the multiboot header's video request is granted (the ISO's first
entry: `gfxpayload=1024x768x32`), or the Bochs display adapter's (BGA,
PCI 1234:1111 in QEMU and Bochs, 80EE:BEEF in VirtualBox) where c4pc is
given `--gui WxH` and no GRUB set a mode. A display smaller than the
screen is scaled up and centred, as the page's canvas is (C4IX's desktop
asks for 800 x 600); a pointer is drawn over it. The mouse is the 8042's
second port, with the IntelliMouse wheel where it has one, each packet
queued so that a press and its release between two looks are both seen.
While a program has the display's keys (the ring attached, keys in its
event mask) the keyboard's scancodes go to it as the page's keydown and
keyup -- the DOM's keyCode, the character, the modifiers -- and COM1
stays the console's. A key or the mouse during a nap wakes the machine
with its clock given only what it slept (libjs/worker.js wake): kept
whole, two clicks a moment apart were seconds apart to the desktop.

*Measured*, 2026-11-22, under KVM on helios: the ISO's first entry boots
C4IX's desktop in QEMU (std VGA, GRUB's 1024 x 768 x 32); the desktop
is as the page draws it (wallpaper, icons, the Terminal, the taskbar's
clock); a PS/2 double-click on the Calculator's icon opens it, and keys
typed at the Terminal appear there. `make test-c4pc` boots the desktop
on c4pc-host with `--gui`, double-clicks the Calculator by script and
looks for its strings among those drawn (`--texts`).

    make c4pc.elf .c4pc/disk.tar
    qemu-system-i386 -accel kvm -cpu host -m 512 -vga std -vnc :1 -serial stdio \
        -kernel c4pc.elf -append "--mmu -m 128 --gui 1024x768 -- --desktop" \
        -initrd "c4ix32.c4r,.c4pc/disk.tar drive"

A PS/2 packet carries motion and the buttons at once, where the page's
mousemove never changes the buttons: a packet that moves and presses or
lets go gives a MOVE with the buttons as they were, then the DOWN or UP.
Taken as the new buttons with the move, a press or a release that came
with motion sent no DOWN or UP at all, and Duke Nukem 3D went on firing
after the button let go; test-c4pc's double-click on the Calculator is
made of such packets (`packet DX DY BUTTONS` events).

c4pc-host fits one with `--gui WxH`; `--events FILE` plays mouse and key
events at the machine's own milliseconds, and `--shot FILE.png` (or a
`shot` event) writes what the display shows.

*The screen's cost.* What changed goes to the framebuffer a row at a time,
made in memory and written in one run (`pc/screen.c`), a row scaled up
from the same row of the display made once. A pixel at a time, each with
a 64-bit division for its place, a 1024 x 768 frame cost more than the
frame: Doom from the desktop, *measured* under KVM, 20% of the time
drawing, now 1%. And the framebuffer is write-combined where the CPU
can say so: a BIOS leaves it uncached (SeaBIOS by an MTRR range over the
whole PCI hole), every store a bus cycle of its own. With a PAT, c4pc
turns paging on, every 4 GB mapped to itself in 4 MB pages, and gives the
framebuffer's pages the PAT's WC, which wins over an MTRR's UC; without
one, a free variable MTRR saying WC where no range covers it. c4pc says
which (`c4pc: the framebuffer: write-combining (PAT)`); `--no-wc` leaves
the memory types alone. KVM ignores the guest's types, so the gain is
VMware's and real PCs'; it is not measured yet.

*The console on the framebuffer.* While no program has the display (no
ring attached, no frame flipped in the last second), c4pc draws the
console's 80 x 25 text screen on the framebuffer in DejaVu Sans Mono,
the changed rows each time; the VGA's text is kept in memory and is not
on the screen in a graphics mode. A GRUB may set a graphics mode though
the entry says `gfxpayload=text`, as VMware's did: the text entries then
showed nothing but the pointer. A program that takes the display (Doom
from the shell takes it as it would the page's) has the screen until it
lets go, and the console comes back.

**GRUB** (VirtualBox, VMware, Hyper-V Gen 1, real PCs with a BIOS):
`make c4pc.iso` makes a 169 MB ISO with the whole disk and a menu of four
(src/c4pc/pc/grub.cfg: C4IX's desktop, then the text screen with the
MMU, without it, and 128 MB with statistics), by `grub-mkrescue-pc`, the wrapper that points
the system's grub-mkrescue at the unpacked i386-pc modules. GRUB passes a
module's arguments, not its path, so each `module` line names its file
again:

    multiboot /boot/c4pc.elf --mmu -m 64
    module /boot/c4ix32.c4r c4ix32.c4r
    module /boot/disk.tar disk.tar drive

In VMware GRUB sets the graphics mode for the text entries too; c4pc
then draws the console there (above). GRUB reads COM1 too, so keys typed there while it loads (a minute in
QEMU, for 160 MB from the CD) can be its own: wait for C4IX's prompt.
*Measured* 2026-11-22 under KVM: the first entry boots with the MMU in
36 seconds, most of it GRUB reading the CD, and `c4ix-ps.c4r` then
`exit` typed on COM1 run and shut down. The ISO is hybrid (GRUB's
boot_hybrid.img), so it can be written to a USB stick for a PC with a
BIOS or CSM.

**Bochs** boots the same ISO. Its build here has the debugger, so it
needs `-rc` with a `c` in it, and its display wants `rfb` or `sdl2` (there
is no `nogui`); BIOS-bochs-latest triple-faults in its rombios32 with this
config, BIOS-bochs-legacy and SeaBIOS do not.

**DOSBox and DOSBox-X**: `C4PC.COM` (src/c4pc/dos) loads the kernel and
its modules from DOS, the way LOADLIN loads Linux, then leaves DOS for
good. Its `C4PC.CFG`:

    kernel C4PC.ELF
    cmdline -s -m 32
    module C4IX32.C4R
    module DISK.TAR drive

and `DESKTOP.CFG`, C4IX's desktop:

    kernel C4PC.ELF
    video 800x600
    cmdline --mmu -m 96 -- --desktop
    module C4IX32.C4R
    module DISK.TAR drive

`video WxH` asks for a VESA mode, as GRUB's gfxpayload does: the loader
asks VBE (INT 10h AX=4F00h, 4F01h) for its modes and takes the largest
with a linear framebuffer and direct colour that fits in W x H, 32 bits
a pixel before 24 before 16; it sets it (4F02h, the linear bit) as the
last thing before the kernel, and describes it in the multiboot
information's framebuffer fields as GRUB does, so that c4pc finds it the
same way. 800 x 600 is the desktop's own size: no scaling, and the
display's buffers half what 1024 x 768's are. DOSBox-X's S3 with
`vmemsize=8` has modes to 1024 x 768 x 32; plain DOSBox's 2 MB, 800 x 600
x 32.

It reads with INT 21h, copies above 1 MB with the BIOS's block move
(INT 15h AH=87h), asks INT 15h E801/88h for the memory, and needs real
mode (no EMM386). DOSBox's own XMS and EMS keep the memory from the BIOS:
`xms=false`, `ems=false`, `umb=false`. Plain DOSBox has at most 63 MB
(`memsize=63`): enough for a 32 MB arena and a disk without the games.

`make c4pc-dos` makes that folder, `.c4pc/dos/`, for DOSBox-X: C4PC.COM,
C4PC.ELF, C4IX32.C4R, DISK.TAR (the whole disk, 160 MB: the desktop's
files are among the big ones `.c4pc/small.tar` leaves out), C4PC.CFG,
DESKTOP.CFG and DOSBOX.CONF (`memsize=512`: the disk, and the desktop's
machine, whose swap lives in c4pc's memory; with less, `c4pc: out of
host memory`). Run DOSBox-X in it with `-conf DOSBOX.CONF`, then `C4PC`
or `C4PC DESKTOP.CFG`. Build it with main's images
(`make C4PC_IMAGES=... C4PC_IMAGES_DEP= c4pc-dos`): a worktree's own
`src/c4bb/images` may be a small one.

*Measured:* DOSBox-X runs the whole disk this way (`memsize=512`,
`core=dynamic`, COM1 to a file), byte for byte as libjs. And the desktop,
2026-11-22, DOSBox-X 2026.01 in a window on helios's VNC desktop: the
package as made boots it at 800 x 600 x 32 in about four minutes (the
clock 3 MHz, DOSBox-X giving 4-5M c4 instructions a second there; it
renders its window over VNC), keys typed at the Terminal run, and the
PS/2 mouse moves the pointer and clicks (DOSBox-X's `autolock`: the mouse
is the machine's once clicked into the window). Plain DOSBox 0.74, seen in
a window: C4PC.COM stops while it copies the disk (`DISK.TAR drive, 17720
KB` its last line, no progress past the first megabyte), the build before
the VESA mode too: not yet understood.

DOSBox has no isa-debug-exit: c4pc halts when C4IX ends, and a headless
run watches COM1 for c4pc's last line and stops the emulator (DOSBox-X
then needs SIGKILL, ignoring SIGTERM while halted).

## Next

- **M3, the console**: `-i`-style pacing to the machine's clock, a
  scrollback, and the keyboard's raw mode as keys.js has it.
- **The display, further**: the page's pointer shapes; a resize to the
  screen's own size rather than scaling; the display's command stream
  held against libjs's in a test; write-combining measured in VMware and
  on a real PC.
- **Plain DOSBox 0.74**: why C4PC.COM stops copying the disk there.
- A test in the gate: `test-c4pc` takes about ten minutes, most of it
  QEMU's three runs (`NO_MMU=1`: three minutes; `NO_QEMU=1`: two).
- **libjs's par.js**: the swap's registers run on the CPU that touches
  them, as par.c runs them (section Several processors): C4IX on four of
  its threads is slower than on one until then.
- **Installing to the IDE disk**, and booting from it: the store read a
  file at a time as it is wanted, rather than whole at boot; the store as
  a known filesystem (FAT32: GRUB reads it, and Windows and Linux open
  it), the kernel, C4IX and the boot disk on it, GRUB on the disk.
- **Several processors on real hardware and VMware**: only QEMU's have
  been started; ACPI's MADT is not read (every processor that answers
  the broadcast is taken), and x2APIC-only machines are not spoken to.
