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
| not yet | the display and mouse (M4), sound (M5), persistent storage (M6), real SMP (M7), x86-64 (M8), accelerators |

## One engine, two front ends

```
src/c4pc/
  c4pc.h  clib.h        the machine, and the C library calls it makes
  engine.c              libjs/c4m.js's interpreter, traps and syscalls, the user window; boot.js's loader and trampoline
  mmu.c                 libjs/mmu.js: the MMU's registers, translation, the user loop; arena.js's kernel map
  dev.c                 src/c4bb/sim/devices.js: the registers at 0x100-0x1FF
  heap.c                libjs/heap.js, block for block
  fmt.c                 libjs/printf.js (PRTF), with JavaScript's float formatting
  fpu.c                 src/c4bb/sim/fpu.js (F3) and c4m.js's FLT
  drive.c util.c        drives (and ustar archives as drives), maps, blobs
  host.c                c4pc-host: Linux, with libjs/cli.js's flags
  pc/                   c4pc.elf: boot.S, pc.c (the PC), libc.c, link.ld
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

**What differs from libjs.** One CPU; no display, sound, mailbox,
downloads or accelerators, so INFO says so and C4IX does without (its
root archive unpacks in its own C, as with libjs's `--no-osaccel`). Past
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
its own clock as cli.js `-i` does (16 simulated milliseconds at a time,
then a wait for the real clock to catch up), so that `top` redraws once a
second; `--fast` turns that off.

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
inflate the root archive (1,500M cycles; libjs inflates it natively,
and keeps the result on drive 1):

| | 20 MHz, before | auto |
|---|---|---|
| KVM | 75 s | 12 s (130 MHz, running 127M a second) |
| TCG | about 112 s (behind its clock: the unpaced run's time) | 150 s (10 MHz, keeping time) |

Under an emulator the share kept back costs a quarter; `--fast` or a
higher `-hz` trades the clock's truth for it. Keys come from the PS/2 keyboard and
from COM1, cooked as a terminal cooks them until a program opens the raw
keyboard. *Measured* in QEMU with `-serial stdio`: typed commands echo,
run, and `exit` shuts C4IX down.

**GRUB** (VirtualBox, VMware, Hyper-V Gen 1, real PCs with a BIOS):
`make c4pc.iso` makes a 169 MB ISO with the whole disk and a menu of three
(src/c4pc/pc/grub.cfg: with the MMU, without it, and 128 MB with
statistics), by `grub-mkrescue-pc`, the wrapper that points
the system's grub-mkrescue at the unpacked i386-pc modules. GRUB passes a
module's arguments, not its path, so each `module` line names its file
again:

    multiboot /boot/c4pc.elf --mmu -m 64
    module /boot/c4ix32.c4r c4ix32.c4r
    module /boot/disk.tar disk.tar drive

GRUB reads COM1 too, so keys typed there while it loads (a minute in
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

It reads with INT 21h, copies above 1 MB with the BIOS's block move
(INT 15h AH=87h), asks INT 15h E801/88h for the memory, and needs real
mode (no EMM386). DOSBox's own XMS and EMS keep the memory from the BIOS:
`xms=false`, `ems=false`, `umb=false`. Plain DOSBox has at most 63 MB
(`memsize=63`): enough for a 32 MB arena and a disk without the games.

`make c4pc-dos` makes that folder, `.c4pc/dos/`: C4PC.COM, C4PC.ELF,
C4IX32.C4R, DISK.TAR (the disk less the games, 18 MB: `.c4pc/small.tar`),
C4PC.CFG and DOSBOX.CONF. Run DOSBox in it with `-conf DOSBOX.CONF`.

*Measured:* DOSBox-X runs the whole disk this way (`memsize=512`,
`core=dynamic`, COM1 to a file), byte for byte as libjs. Plain DOSBox 0.74
runs C4PC.COM, which starts the kernel; what C4IX printed there could not
be seen headless (DOSBox 0.74 has no serial-to-file, and its null-modem
link delivered nothing on helios, even for a trivial program), so plain
DOSBox wants a look at its screen.

DOSBox has no isa-debug-exit: c4pc halts when C4IX ends, and a headless
run watches COM1 for c4pc's last line and stops the emulator (DOSBox-X
then needs SIGKILL, ignoring SIGTERM while halted).

## Next

- **M3, the console**: `-i`-style pacing to the machine's clock, a
  scrollback, and the keyboard's raw mode as keys.js has it.
- **M4, the display**: the 0x400 registers on a VBE framebuffer (BGA in
  QEMU and Bochs), bitmap fonts, the PS/2 mouse.
- A test in the gate: `test-c4pc` takes about ten minutes, most of it
  QEMU's three runs (`NO_MMU=1`: three minutes; `NO_QEMU=1`: two).
