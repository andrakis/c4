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
| done | the clock the wall's when the PC falls behind (libjs's realtime): games drop frames rather than play in slow motion; the screen written a row at a time, write-combined through the PAT; the console drawn on a framebuffer while no program has the display |
| not yet | sound (M5), persistent storage (M6), real SMP (M7), x86-64 (M8), games' accelerators |

## One engine, two front ends

```
src/c4pc/
  c4pc.h  clib.h        the machine, and the C library calls it makes
  engine.c              libjs/c4m.js's interpreter, traps and syscalls, the user window; boot.js's loader and trampoline
  mmu.c                 libjs/mmu.js: the MMU's registers, translation, the user loop; arena.js's kernel map
  dev.c                 src/c4bb/sim/devices.js: the registers at 0x100-0x1FF
  accel.c               src/c4ix/accel.mjs: C4IX's inflater and CRC-32, behind libjs/accel.js's numbers
  gui.c                 libjs/gui-device.js's display at 0x400, and display.js's drawing
  font.c mkfont.py      the display's text: outlines from the system's free fonts, filled at any size
  heap.c                libjs/heap.js, block for block
  fmt.c                 libjs/printf.js (PRTF), with JavaScript's float formatting
  fpu.c                 src/c4bb/sim/fpu.js (F3) and c4m.js's FLT
  drive.c util.c        drives (and ustar archives as drives), maps, blobs
  host.c                c4pc-host: Linux, with libjs/cli.js's flags
  pc/                   c4pc.elf: boot.S, pc.c (the PC), screen.c (framebuffer, mouse), libc.c, link.ld
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

**What differs from libjs.** One CPU; no display, sound, mailbox or
downloads, so INFO says so and C4IX does without. Of the accelerators
only C4IX's own (accel.c): a game's ACC_FIND gets 0, and the game runs
its own loops, as on libjs with `--no-accel`. Past
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
`--mmu`, then the program's arguments after `--`. c4pc writes the status to
isa-debug-exit, so QEMU exits `(status << 1) | 1`.

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

**At the keyboard**, with no `stdin` module, c4pc paces the machine to
its own clock as libjs/worker.js does (4 simulated milliseconds at a
time, then a wait for the real clock to catch up), so that `top` redraws
once a second; `--fast` turns that off.

When the PC cannot keep up with the clock, the clock is the wall's, as
libjs's page has it by default (worker.js, `realtime`): more than 20 ms
behind, TIME moves on by what the CPU could not do, so a game gets fewer
cycles a second, drops frames and keeps its speed, instead of playing in
slow motion. `--no-realtime` lets the machine's second stretch instead
(libjs's `?realtime=0`). What falls behind is the games' own work, dearer
than the calibration's loop, and the drawing: before the screen was
written a row at a time (below), Doom from the desktop under KVM spent
20% of the time drawing to the screen and the clock fell 20% behind,
which is how Duke Nukem 3D came to say 50 to 250 frames a second and
move in slow motion in VMware. `-s` at the keyboard says every 10
seconds on COM1 (not on the screen) how the PC kept up: `c4pc: 128262k inst/s, 1% drawing to
the screen, 2% of the clock skipped`; at the end, the totals.

The clock is then one the PC keeps up with: before the boot, c4pc times
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
