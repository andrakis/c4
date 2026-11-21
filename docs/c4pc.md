# c4pc: the c4 machine on a PC

c4pc is route (a) of [c4ix-i386-port.md](c4ix-i386-port.md): libjs's
engine in C, booted on a PC with nothing under it, so that C4IX's images
run there unchanged. It is in `src/c4pc/`.

| status | what |
|---|---|
| done (M0, M1) | the engine, devices, heap, formatter and FPU, as libjs has them; multiboot; GDT, IDT, PIC, PIT; COM1; VGA text with enough ANSI for a shell; the keyboard; drives from tar archives; the DOS loader |
| *measured* | C4IX boots, runs `ps` and shuts down with output byte for byte libjs's, in the same 1,474,118,389 cycles, on c4pc-host, on c4pc.elf in QEMU (`make test-c4pc`), and in DOSBox-X through C4PC.COM; test-libjs's 27 native-oracle programs print what they print on libjs, with the same status |
| not yet | the MMU (M2), the display and mouse (M4), sound (M5), persistent storage (M6), real SMP (M7), x86-64 (M8), accelerators |

## One engine, two front ends

```
src/c4pc/
  c4pc.h  clib.h        the machine, and the C library calls it makes
  engine.c              libjs/c4m.js's interpreter, traps and syscalls; boot.js's loader and trampoline
  dev.c                 src/c4bb/sim/devices.js: the registers at 0x100-0x1FF
  heap.c                libjs/heap.js, block for block
  fmt.c                 libjs/printf.js (PRTF), with JavaScript's float formatting
  fpu.c                 src/c4bb/sim/fpu.js (F3) and c4m.js's FLT
  drive.c util.c        drives (and ustar archives as drives), maps, blobs
  host.c                c4pc-host: Linux, with libjs/cli.js's flags
  pc/                   c4pc.elf: boot.S, pc.c (the PC), libc.c, link.ld
  dos/                  C4PC.COM: the DOS loader
  test-c4pc.sh          make test-c4pc: C4IX on libjs, c4pc-host and QEMU
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

**What differs from libjs.** One CPU; no MMU, so C4IX runs as libjs runs
it with `--no-mmu`; no display, sound, mailbox, downloads or accelerators,
so INFO says so and C4IX does without (its root archive unpacks in its own
C, as with libjs's `--no-osaccel`). Past the arena's end libjs's typed
arrays read 0 and drop the write, where c4pc's word index wraps inside the
arena: only a wild program can tell.

## Speed

*Measured*, the test session (1,474M c4 instructions), on helios:

| machine | c4 instructions a second |
|---|---|
| libjs (node 24), `--no-accel --no-osaccel` | 153–155M |
| c4pc-host (gcc -O2, x86-64) | 303–307M |
| c4pc.elf in QEMU 10.2, TCG (helios has no KVM) | 15.1–15.5M |
| c4pc.elf in DOSBox-X 2026.01, dynamic core, `cycles=max`, through C4PC.COM | 13.5M |

The study guessed 15–40M in DOSBox: the low end. On helios, then, a
C4IX shell, `ps` and `top` are usable under both emulators; the desktop
will be slow until a VM with hardware virtualization (VirtualBox, VMware,
Hyper-V on the Windows machines) runs it at the host's speed. The
interpreter is libjs's loop as it is. c4mp's register-held dispatch,
threaded code and a translator (M9) are where more would come from.

## Running it

The tools (QEMU, Bochs, DOSBox-X, DOSBox 0.74, mtools, xorriso, GRUB's
i386-pc modules) are unpacked on helios in `~/opt/pctools`, from the
Ubuntu archive without root; `. ~/opt/pctools/env.sh` puts wrappers for
them on PATH (`~/opt/pctools/mkwrap.sh` makes them).

**QEMU**, the headless one: `-kernel` loads the multiboot ELF and
`-initrd` its modules. The program comes first; a module whose string's
second word is `drive` is a drive (a ustar archive, `drive rw` writable in
RAM), and `stdin` is fed to the keyboard up front with end of input, as
cli.js feeds piped input. `-append` takes cli.js's `-m -c -s -hz`, then
the program's arguments after `--`. c4pc writes the status to
isa-debug-exit, so QEMU exits `(status << 1) | 1`.

    make c4pc.elf .c4pc/disk.tar
    qemu-system-i386 -m 512 -display none -serial file:serial.txt \
        -device isa-debug-exit,iobase=0xf4,iosize=4 -kernel c4pc.elf \
        -append "-s -m 64" -initrd "c4ix32.c4r,.c4pc/disk.tar drive,in.txt stdin"

Without `-display none` the VGA screen shows the same console, and the
keyboard types at it.

**GRUB** (VirtualBox, VMware, Hyper-V Gen 1, real PCs): an ISO from
`grub-mkrescue-pc` (the wrapper that points the system's grub-mkrescue at
the unpacked i386-pc modules) with

    menuentry c4pc { multiboot /boot/c4pc.elf -m 64
                     module /boot/c4ix32.c4r c4ix32.c4r
                     module /boot/disk.tar disk.tar drive }

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

## Next

- **M2, the MMU**: mmu.js's user loop and TLB, so C4IX's user tasks run
  in their own spaces, as libjs runs them by default.
- **M3, the console**: `-i`-style pacing to the machine's clock, a
  scrollback, and the keyboard's raw mode as keys.js has it.
- **M4, the display**: the 0x400 registers on a VBE framebuffer (BGA in
  QEMU and Bochs), bitmap fonts, the PS/2 mouse.
- A test in the gate: `test-c4pc` takes about two minutes, most of it QEMU.
