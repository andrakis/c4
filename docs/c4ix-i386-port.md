# C4IX on a real PC: an i386 port, and a Linux-like source tree

Kind: investigation (2026-11-21). Nothing is ported or moved yet. This
is a study and a plan, for the user to read and decide on. The numbers
marked *measured* were taken on helios on 2026-11-21. Those marked
*estimate* have not been measured.

The question: how hard is it to run C4IX on real i386 hardware, tested
first in DOSBox and later as x86-64 in VirtualBox or VMware? And if it
is viable, how should C4IX's source be laid out, in Linux's manner, so
that architecture code and drivers each have a folder of their own?

## The answer, short

- **It is viable, and the cheap way in leaves C4IX unchanged.** Route
  (a) is a new machine, **c4pc**: a boot stub plus libjs's engine
  ported to C, running `c4ix32.c4r` as it is. The PC's devices sit
  behind the same page-0 registers libjs and c4bb answer. Everything
  that runs on libjs today would run on c4pc: the kernel, the MMU, the
  userland, the desktop, the games, even the C4DOS → C4KE → C4IX
  ladder. A shell over the serial port in QEMU is about four milestones
  away. A desktop with sound in DOSBox is about seven.
- **A native kernel is nearer than it looked, but it comes second.**
  *Measured:* gcc 15 `-m32` compiles twelve of C4IX's thirteen kernel
  modules as they stand, once a shim declares the `__c4_*` intrinsics
  and `struct task` ahead of use. `sched.c` has five errors, all of one
  kind: calls through an `int`. So route (b) does not need an i386
  backend for c4lc; gcc can build the kernel. The work is the
  architecture layer: the trap entry, the context switch, the timer,
  real page tables, and a way to run user programs. That is where
  mmu.c's i386-shaped tables would finally drive the hardware. It wants
  the layout below in place first.
- **Translating `.c4r` images ahead of time (route c) is not a route
  for the kernel.** C4IX's traps rewrite pc, sp and bp. Code addresses
  are stored as data, and C4KE programs patch their own code. Those
  make a static translator into a JIT that cannot fall back. It can
  still come later, as an accelerator *inside* c4pc's interpreter.
- **Recommendation:** build route (a), milestones M0–M8 below. Do the
  layout's first, path-only step (L1a) whenever convenient; it is
  independent of the port. Decide on route (b) after M3, when c4pc's
  speed in DOSBox and QEMU has been measured rather than estimated.
- **What the user must decide or install** is listed in section 10:
  QEMU (and maybe Bochs) on helios for headless tests, DOSBox-X for
  boot images, fonts for the desktop, and whether to use GRUB or our
  own boot sector.

## 1. What C4IX asks of its machine

C4IX is c4 VM bytecode. c4lc compiles it, c4rlink links it, and c4m,
libjs or c4bb runs it. Everything it knows about "hardware" is one of
four things:

| interface | what C4IX uses | where it lives today |
|---|---|---|
| the instruction set | opcodes 0–94; traps whose handler may assign to its own parameters to choose where the machine resumes (`sched_trap(interval, trap, param, mode, a, bp, sp, returnpc)`); `TLEV`; the cycle interrupt; protected mode; `CAS XCHG FADD CWAI CWAK IPI`; the `FPU` opcode | c4m.c, `libjs/c4m.js`, c4bb microcode ([internals.md](internals.md) Part 3) |
| host opcodes | `OPEN READ CLOS` (host files: the loader's fallback and the console), `PRTF PUTC PUTS` (the console), **`MALC FREE RALC` (the kernel's heap is the host's**: `libjs/heap.js`), `USLP`, `TIME`, `INFO` | the same engines |
| page-0 registers | c4bb's devices at 0x100–0x1FF (UART, clocks, `PIT_MS`, **drives that open files by name**, the heap's extent and generations, the mailbox, the console tap, swap); the image and boot stack at 0x1E4–0x1FC; the display at 0x400; the sound card at 0x480; the MMU at 0x500; the CPU-local window at 0x800 | `src/c4bb/sim/devices.js`, `libjs/bus.js`, `gui-device.js`, `snd-device.js`, `mmu.js`, `smp.js` |
| memory | one arena from address 0, page 0 the bus, the kernel at 0x1000 | `libjs/boot.js` |

The kernel's own use, counted in the modules: `__c4_cycles` 27 times,
`__c4_configure` 17, `__c4_dbase` 9, `__c4_trap` 7, `__c4_cas` 7,
`__c4_ipi` 5, `__c4_fadd` 5, and a handful of the rest. Page-0
registers are read or written in boot.c, mmu.c, sched.c, task.c,
vfs.c, sl4b.c and loader.c (`c4ix.h` names them all).

Four facts shape every route:

1. **The drives are file servers, not disks.** A drive opens a file by
   its name (`DISK_NAME`, `DISK_FD`, `DISK_ADDR`, `DISK_LEN`), and
   libjs fetches a file lazily over HTTP. A PC disk gives sectors. Some
   layer has to turn sectors into named files.
2. **The kernel's heap belongs to the host.** `malloc` in the kernel is
   `MALC`, and heap.js keeps the books, with generations and the
   `SWAP_CMD` operations the MMU uses. On bare metal, someone has to
   implement it.
3. **The display is a 2D drawing API, not a framebuffer.** Commands
   `CLEAR`, `RECT`, `LINE`, `CIRCLE`, `TEXT2` (nine faces, among them
   sans, Verdana, Courier, Lucida and serif, which the browser draws),
   `IMG`, `BLIT8`, `CLIP`, plus a framebuffer path (`FB`, `FBFLIP`).
   The page's canvas renders them. A PC needs a rasterizer and bitmap
   fonts.
4. **Sound is a stream of stamped events.** OPL register writes,
   sixteen 8-bit PCM channels, the PC speaker and MIDI all arrive
   stamped on the card's stream clock (`WAIT`, `AHEAD`), and a mixer
   renders them later. A real card has to be written in real time,
   from a timer.

Accelerators (`C4I_ACCEL`, games' `accel.mjs`) are optional by design.
A host that does not announce the bit gets the games' own C loops, as
c4bb does.

## 2. The MMU against a real i386

[c4ix-mmu.md](c4ix-mmu.md) chose the i386's shape on purpose. Here is
how close it is, item by item:

| C4IX / libjs | i386 | match |
|---|---|---|
| 4 KB pages; two levels, 10 + 10 + 12 bits | the same (non-PAE) | exact |
| PTE bits P 0, W 1, U 2, A 5, D 6 | P 0, R/W 1, U/S 2, A 5, D 6 | exact |
| kernel's bits PIN 0x200, SWAP 0x400, ZERO 0x800 | bits 9–11 are "available to software" | exact |
| PDE: only P is used, the PTE decides | the PDE's W and U combine with the PTE's | compatible, if the kernel sets PDEs to P W U |
| **X, bit 3: user may execute** | **bit 3 is PWT, write-through caching**; there is no execute bit without PAE and NX (bit 63 of a 64-bit entry) | **differs** |
| `PTBR` 0x504, a write flushes | CR3 | same idea |
| `INVLPG` 0x518, `FLUSH` 0x51C | `invlpg`; a CR3 reload | same |
| `KSP` 0x508, the stack a user-mode trap switches to | `TSS.esp0` | same idea |
| `FADDR`, `FINFO`, `FPC`, trap `SEGV` (4) | CR2; the #PF error code (P, W/R, U/S; I/D only with NX); the saved EIP; vector 14 | same information, different bits |
| `IOPM` 0x520: which of page 0's windows user mode may use | the TSS I/O permission bitmap, over ports | the same idea, over another thing |
| **the kernel runs untranslated** (kernel mode sees physical memory) | paging is on or off for the whole CPU (CR0.PG) | **differs**: the kernel must be mapped into every space, supervisor-only |
| the kernel's map (`UDIR`) maps every task's user addresses at once; kernel mode reaches any task's pointer through it (`c4m.js kpa`) | one CR3 at a time | **differs**: another task's pointer needs a software walk, or a temporary mapping |
| the stack window faults *before* an instruction if a word beside `sp` is not writable | faults are precise to the access | the hardware is stricter, and simpler |
| DMA checks (`FK_DMA`, the device checks against the task's space, like an IOMMU) | ISA DMA and PIO see physical memory | the kernel or host does the copy and the check |

So, for route (a), mmu.c runs **unchanged** against an emulated MMU: a
C port of `mmu.js` with its software TLB, code window and stack window.
The i386's own paging is used only by the host, for itself. For route
(b), mmu.c's tables can be the hardware's tables, with four changes:

- X moves to software (no NX), or to bit 63 under PAE;
- the kernel's direct map goes into every directory;
- `uaccess` walks `mmu_kdir` in software for a pointer that is not the
  current task's;
- the register writes become `mov cr3`, `invlpg` and the TSS.

## 3. The routes compared

### (a) c4pc: a native host that runs C4IX unchanged

**What it is.** A program that boots on a PC: a multiboot ELF, or a
DOS loader that jumps into it. It contains:

- the c4 engine, a C port of `libjs/c4m.js` (the interpreter, traps,
  TLEV, the cycle interrupt, protected mode, the fused and 16-bit
  opcodes), with `FPU` served by softfp compiled natively, as c4m does;
- the user loop and TLB of `mmu.js`;
- `smp.js`'s round robin;
- the arena layout of `boot.js`, and the heap of `heap.js`;
- device models that answer the page-0 registers, with PC drivers
  behind them.

C4IX, its userland and its tests see a machine they already know. In
Linux's terms, c4pc is to C4IX what QEMU is to a guest kernel, except
that c4pc is itself the kernel of the PC.

**Why port libjs rather than grow c4mp.** c4mp (`src/c4mp`, 1,645
lines) is the obvious native core. It is struct-based C, it is fast,
and it compiles `-m32` today. But it has no MMU, no devices and no
display, and *measured*: `./c4mp c4ix.c4r --help` segfaults, while
`./c4m load-c4r.c -- c4ix.c4r --help` boots. No test runs C4IX on c4mp.
libjs is the engine that runs `c4ix32.c4r`, with the MMU, SMP, the
display and sound, every day, and test-libjs and test-desktop define
how it behaves. c4mp's switch dispatch is still the model for speed.

**What carries over:**

| part | in route (a) |
|---|---|
| the kernel, userland, desktop, games, the ladder | unchanged: `c4ix32.c4r` and the disk's images |
| the MMU (mmu.c) | unchanged, over an emulated MMU |
| traps, syscalls, protected mode | unchanged: they are the engine's |
| SMP | simulated round robin first (smp.js), as libjs's `?cpus=N`. Real APs later, in par.js's model: CPU 0 owns the devices, and the others forward to it |
| drivers | new, in the host, behind the existing registers |

**The cost is speed.** *Measured:* c4m spends about 30 x86 instructions
per c4 instruction on a counting loop (valgrind lackey, 1,548M x86
instructions for 52M c4 instructions). That is the 370M c4 instructions
a second that [gcc-port.md](gcc-port.md) measured on helios's Ryzen 5
3600. libjs runs 100–150M a second per CPU in the browser. A
switch-dispatched C engine should beat c4m: c4or1k's M11 found c4mp
faster with no source changes. Taking about 10–15 x86 instructions per
c4 instruction as a guess:

| where c4pc runs | x86 rate (*estimate*) | c4 instructions a second (*estimate*) | against libjs |
|---|---|---|---|
| VirtualBox or VMware, hardware virtualization | near native | 300–600M | 2–4× faster |
| QEMU TCG on helios (no KVM: helios is itself a Hyper-V guest) | about a tenth of native, or less | 30–100M | a third to level |
| DOSBox-X or DOSBox, dynamic core, `cycles=max` | a few hundred MIPS | 15–40M | a tenth to a third |
| Bochs | tens of MIPS | 2–5M | for debugging only |

The DOSBox row is the one that matters for a desktop in DOSBox, and M1
measures it first. At 20M a second, the shell, top and the compilers are
usable; the desktop is slow; Doom is a slideshow.

### (b) C4IX's kernel as native i386 code

**The compiler is not the problem.** An i386 backend for c4lc would mean
a new code generator in c4lc's Lisp, plus native relocations and an
object format for c4rlink, and c4fc kept in step. *Measured:* gcc
already nearly compiles the kernel:

    gcc -m32 -std=gnu17 -ffreestanding -fno-builtin -O2 -include shim.h -I. -I../../include -c MODULE.c

`shim.h` declares the `__c4_*` intrinsics and the host opcodes (`int
__c4_cas();`, `int malloc();` and so on), and forward-declares `struct
task; struct vnode; struct file;`. Without those forward declarations
there are about forty errors: prototypes in c4ix.h mention a struct
before it is declared. c4lc accepts that; gcc scopes each one to its
prototype. With them,
twelve modules compile. `sched.c` fails on `e(0)`, a call through an
`int`, which c4 allows and gcc does not (four sites), and on
`sched_interrupt` declared `static` after a non-static use. The twelve
objects are 134 KB of text. This proves only that the code compiles, not
that it works: c4's calling convention pushes arguments left to right,
which `va.c` and any argument walking depend on. The argument-count scan
of 2026-11-18 found calls that pass more arguments than the callee
declares, and c4 and cdecl read those slots differently.

**What would change in C4IX:**

- **Trap entry.** `sched_trap` chooses where the machine resumes by
  assigning to its own parameters. gcc keeps parameters wherever it
  likes. The arch layer passes a frame pointer instead (`struct
  trapframe *`), and c4vm's entry builds the same frame from the
  handler's slots, so the generic code is the same for both.
- **The context.** A task saves four words, `sv_a sv_bp sv_sp sv_pc`
  (sched.c 1206, 1213). Native code needs all the general registers.
  The context becomes an arch type, saved and loaded by arch functions.
  About a dozen lines of sched.c assign the frame's registers.
- **The timer.** The cycle interrupt (`__c4_configure(CONF_*)`, 17
  calls) becomes the PIT or the local APIC timer. `__c4_cycles` becomes
  `rdtsc`, or a count the timer keeps.
- **Intrinsics.** `__c4_cas` becomes `lock cmpxchg`, `__c4_ipi` the
  APIC's ICR, `__c4_wait` and `__c4_wake` become `hlt` and an IPI, and
  `__c4_dbase` becomes a per-CPU segment (`%fs` or `%gs`).
- **Memory.** `MALC` and `FREE` become a kernel heap of C4IX's own,
  heap.js's algorithm moved into mm/.
- **Devices.** The page-0 registers become drivers: PS/2, VGA/VBE, ATA,
  SB16. The host files (`OPEN` on a host path, `SYS_LAZYFILE`) become a
  filesystem on a disk.

**Userland is the real question.** C4IX's programs are `.c4r` bytecode,
built inside the machine by c4sc and c4cc, and C4KE's programs need the
compatibility layer's custom opcodes. Three ways:

- **(b1) Recompile userland native.** This loses the in-machine
  compilers' output, C4KE programs, and the byte-identity rules. No.
- **(b2) Every user task is a ring-3 c4 interpreter.** One read-only
  interpreter page is mapped into every space, like a vDSO, and it runs
  the task's bytecode at the task's own user addresses. The hardware MMU
  is then the only MMU, with no software TLB, because C4IX's user
  addresses are the linear addresses. A syscall opcode becomes `int
  0x80` carrying the c4 registers. Kernel threads are native.
  **Recommended form**, and the reason the i386-shaped tables pay off.
- **(b3) The interpreter in ring 0**, switching CR3 per task. This is
  simpler to start, but a bug in the interpreter is a kernel bug, and
  user faults arrive as kernel faults.

**What carries over:** sched, task, sys, vfs, loader, init and sl4b are
generic, and gcc builds them. mmu.c's generic part (regions, frames,
swap, the clock, load control) carries over, and its table code drives
real tables. SMP was built on par.js's real threads with real atomics,
so the locking already assumes true parallelism. **What does not:** the
in-machine rebuild. The ladder builds the c4vm kernel, and nothing in
the machine can build the native one until c4lc has a backend. The
c4fc byte-identity gate covers only the c4vm build.

### (c) Translating `.c4r` images to i386 ahead of time

Static translation needs every jump target and every resumable point
known in advance. C4IX defeats both:

- **Code addresses as data:** function pointers (`JSRI`, `JSRS`), switch
  tables (`JMPA`), the constructor and destructor lists, the trap
  handler, and the TLEV word.
- **Traps that rewrite pc, sp and bp.** The scheduler resumes a task at
  any c4 pc it saved. So the translation must keep c4's sp and bp in
  real memory, in c4's frame layout, and map every c4 pc to a native
  one at every point where a trap can land.
- **Self-modifying code.** C4KE's ps patches itself (c4ix-mmu.md §9).

Meeting all of that is a JIT's machinery: a pc map, safepoints, and
invalidation on writes. A pure ahead-of-time translator has no
interpreter to fall back to. As an accelerator inside c4pc's engine, it
is sound: translate hot functions, keep the interpreter for the rest.
c4or1k's M13 is the precedent (a JIT to c4 bytecode, "decisively
positive" after v2). It is a later milestone (M9 below) and optional.

### The three side by side

| | (a) c4pc host | (b) native kernel (b2) | (c) AOT `.c4r` |
|---|---|---|---|
| C4IX source changed | none | an arch layer, drivers, a kernel heap | none |
| MMU | mmu.c over an emulated MMU | mmu.c drives CR3 | needs (a)'s or (b)'s |
| speed of user code | interpreted, software TLB | interpreted, hardware TLB | native, where it translates |
| speed of the kernel | interpreted | native | native, where it translates |
| ladder and in-machine rebuild | unchanged | c4vm build only | unchanged |
| C4KE programs | yes | yes (the interpreter traps the custom opcodes) | needs the interpreter |
| first boot to a shell | M3 | after the layout and about six more milestones | not alone |
| risk | low; speed | high: semantics, two builds of one kernel | high |

## 4. The PC pieces

Each piece below is for c4pc (route a); most of them are reused by
route (b) as C4IX drivers.

**Boot**, four ways in, one image:

- **Multiboot v1 ELF.** QEMU's `-kernel` loads it directly, with no
  bootloader and no image (QEMU passes `-initrd a,b` as multiboot
  modules: the drive files). GRUB on an ISO or floppy loads it in
  VirtualBox, VMware or Hyper-V. *Measured:* helios has `grub-file`,
  which checks the header (`grub-file --is-x86-multiboot`). It has no
  `grub-pc-bin` (only `x86_64-efi` modules) and no `xorriso`, so
  `grub-mkrescue` cannot make a BIOS ISO yet.
- **Our own boot sector** for a floppy or hard-disk image. It is 512
  bytes, built with gas `.code16` and `ld --oformat binary`. It reads
  c4pc and the ramdisk with INT 13h, sets a VBE mode with INT 10h, and
  enters protected mode. No GRUB is needed. DOSBox-X boots it with
  `IMGMOUNT` and `BOOT`.
- **A DOS loader in the style of LOADLIN.** A `.COM` file, built with
  gcc `-m16` and gas (both on helios), reads `C4PC.BIN` and the drive
  files through INT 21h, sets VBE, and enters protected mode raw (no
  VCPI, so no EMM386 loaded). This is the way in for **plain DOSBox**,
  which cannot boot a protected-mode kernel from an image as reliably.
  It suits DOSBox well: the drives are a host folder `MOUNT`ed as C:.
- Later, for x64: **multiboot2 or UEFI** (section 8).

**CPU set-up:**

- **GDT:** flat 4 GB ring-0 code and data (c4pc has no ring 3; route (b)
  adds ring 3 and a TSS).
- **IDT:** the exceptions, IRQ 0–15, and an IPI vector.
- **The 8259 PICs,** remapped to 0x20–0x2F.
- **The PIT,** channel 0 at 1 kHz. It is the clock behind `TIME`,
  `PIT_MS` and `RTC_MS`, and `USLP` becomes `hlt` until the next IRQ.
  Channel 2 is the PC speaker (`SPK`).
- **The CMOS RTC** gives the time of day (the display's `CLOCK`).
- **The local APIC and I/O APIC** only for real SMP (M7). DOSBox and
  DOSBox-X have neither.

**The keyboard.** The 8042 on IRQ 1, scancode set 1 (translated). A
table maps scancodes to two things: what `keys.js` makes of a key
(cooked and raw modes, the escape sequences of `KEYSEQ`), and the
display's `EV.KEYDOWN` and `EV.KEYUP` codes. That is a port of keys.js
plus one table.

**The mouse.** The 8042's aux port on IRQ 12, standard PS/2 3-byte
packets (the wheel through the IntelliMouse handshake). QEMU, Bochs,
VirtualBox, VMware and DOSBox-X emulate it. Plain DOSBox offers the
mouse only through its built-in INT 33h driver, which a protected-mode
kernel cannot call without dropping to real mode. So plain DOSBox gets
no mouse until the DOS-services path (M6). Absolute pointing (VMware's
backdoor, VirtualBox's guest additions) is a later nicety.

**The text console.** VGA text mode at 0xB8000, 80×25, with enough of
ANSI for top and the editors. COM1 at 0x3F8 mirrors the UART, and that
is what the tests read.

**The display:**

- **Mode setting.** A VBE 2.0 linear framebuffer, set by the loader or
  GRUB before protected mode: 640×480 to 1024×768, 32 bpp. Bochs's BGA
  ports (0x1CE/0x1CF) can change the mode at run time, which the
  display's `W`/`H` writes ask for, in QEMU, Bochs and VirtualBox. In
  DOSBox the mode is fixed at boot.
- **The renderer** is new code. It rasterizes the command ring (fills,
  lines, circles, clipping, images, `BLIT8` through a palette, the
  framebuffer flip with scaling and column-major mode) into a back
  buffer, then copies it to the LFB on `PRESENT`. It is a C rendition
  of what the page's `display.js` does with a canvas.
- **Fonts.** `TEXT2`'s nine faces are browser fonts. On a PC they must
  be bitmap fonts, at the sizes winkit uses. This is a licence decision
  (section 10), and the desktop will not look exactly as it does in the
  browser.

**Storage**, in three steps:

1. **A ramdisk** (M1): the drives' files packed into one archive, which
   GRUB or QEMU passes as a module or the DOS loader reads. A drive is
   then a table of named files in memory. Writes stay in RAM, as on a
   page with no store. The boot disk is 162 MB with the games, but the
   kernel, userland and the 4.5 MB root archive are a few tens of MB.
2. **DOS services** (M6, DOSBox and real DOS only): drop to real mode
   (or V86) to call INT 21h, so a drive is a DOS directory, read and
   written. In DOSBox that is a host folder, which gives persistence and
   the games' big files without a ramdisk. This is the DOS-extender
   trick.
3. **ATA PIO plus a filesystem** (M6): port 0x1F0, IRQ 14, on QEMU,
   Bochs, VirtualBox, VMware, Hyper-V Gen 1 and DOSBox-X's IDE. FAT16
   or FAT32 is the format the host can also write (mtools, or a VM's
   mounted disk), and it needs a FAT driver, written or adopted (FatFs
   is BSD-licensed; a download). A drive is then a directory on it,
   which is exactly the named-file shape the drive registers already
   have.

**Sound:**

- **PCM.** A Sound Blaster 16 at 0x220, IRQ 5, DMA 1 or 5,
  auto-initialized: one ring that c4pc's mixer (a port of `snd-mix.js`)
  refills at the IRQ, summing the sixteen PCM channels.
- **The OPL.** Written straight to the real (or emulated) chip, at
  0x388 or 0x220–0x223 for the OPL3. libjs's `opl.js` is a port of
  DOSBox's dbopl, so under DOSBox the music is the same chip. The
  stamped OPL writes go on a queue that the PIT drains at their times.
  A host without an OPL (VirtualBox's SB16 is said to have none, to be
  checked; VMware emulates an ES1371 or HD Audio) needs `opl.js` ported to C and mixed into the PCM.
- **MIDI.** An MPU-401 at 0x330 in UART mode for General MIDI
  (DOSBox's mididevice, as libjs's MIDI port imitates).
- **The speaker:** PIT channel 2.

**SMP.** Simulated first: one CPU, the round robin of smp.js. Real APs
come in M7: INIT-SIPI-SIPI to a real-mode trampoline, each AP running an
engine instance, and the device work forwarded to CPU 0 as par.js does.
QEMU `-smp 4`, VirtualBox and VMware have APs; DOSBox does not.

## 5. Testing

| | protected mode, paging | memory | boots our image | IDE | PS/2 mouse | VBE LFB | SB16 + OPL | APIC, SMP | headless on helios |
|---|---|---|---|---|---|---|---|---|---|
| **DOSBox** (0.74, Staging) | yes (it runs DOS extenders and Windows 3.1) | 63 MB (more in Staging) | DOS loader; `BOOT` of a floppy | no | INT 33h only | yes (S3) | yes | no | no: it wants a window; for the user's own checks |
| **DOSBox-X** | yes | up to GBs | yes: `IMGMOUNT` + `BOOT`, floppy or HDD | yes | yes | yes | yes, and MPU-401 | no | probably (SDL dummy drivers, serial to a file); to confirm in M3 |
| **QEMU** (`qemu-system-i386`) | yes | any | `-kernel` multiboot, or images | yes | yes | yes (std VGA, BGA) | `-device sb16`, `-device adlib` | `-smp N` | **yes**: `-display none`, serial to a file, `isa-debug-exit`, QMP `screendump`, `send-key`, sound to a WAV |
| **Bochs** | yes, and exact | any | images | yes | yes | yes (BGA) | SB16 + OPL | yes | yes, slowly; a debugger for triple faults |
| **VirtualBox, VMware** | yes, fast | any | ISO or floppy with GRUB | yes | yes | yes | SB16 without OPL / not SB16 | yes | not on helios (no nested virtualization); on the Windows machines |
| **Hyper-V Gen 1** (Cyron) | yes, fast | any | ISO or floppy | yes | yes | S3 VBE | none | yes | on Cyron, with PowerShell |

**Headless tests on helios** go through QEMU, run under `~/bin/c4-lock`
as every build and test is:

    ~/bin/c4-lock qemu-system-i386 -m 64 -display none -no-reboot \
        -serial file:.c4pc_serial.txt -device isa-debug-exit,iobase=0xf4 \
        -kernel c4pc.elf -initrd c4pc-drives.img -append "--demo ..."

c4pc writes the exit code to port 0xF4 when C4IX shuts down, so `make`
sees pass or fail. The checks would be:

- **test-c4pc**: the `--demo` tour over serial, masked and compared with
  `src/c4ix/tests/x5-c4m.txt`, as test-c4ix does.
- **test-c4pc-mmu**: the MMU tests libjs runs with `--mmu`.
- **test-c4pc-smp**: the same with `-smp 4` and real APs (M7).
- **test-c4pc-desktop**: QMP `screendump` frames, compared at fixed
  points, with input from QMP `send-key` and `mouse_move`. The engine's
  deterministic clock (time from cycles, as c4bb's `TIME_MS`) makes the
  frames repeatable.
- **test-c4pc-sound**: `-audiodev wav` captures, against `cli.js --wav`.

TCG is slow, so the gate gets the short tests and the suite the rest.
DOSBox-X can join the gate if its headless mode holds up. Plain DOSBox
and the VMs are for the user's own checks, on the Windows machines.

**Determinism.** libjs's tests pin the clock to simulated milliseconds.
c4pc needs the same switch: `TIME` from the cycle count, and the PIT
ignored for scheduling. Then a QEMU run is a pure function of its
inputs, as a c4bb run is, and goldens can be pinned.

## 6. Milestones (route a)

The effort is in sessions of the kind this project has run, each a day
or so of work with its tests. *Estimates*, to be corrected as each
milestone lands.

| | milestone | what is done when it is done | size (*estimate*) |
|---|---|---|---|
| M0 | **boot** | a multiboot ELF and our own boot sector; GDT, IDT, PIC, PIT, serial and VGA text; "c4pc" printed in QEMU and DOSBox-X; `grub-file` passes | ~500 lines of C and gas; 1–2 sessions |
| M1 | **the engine** | libjs's interpreter, traps, TLEV, the cycle interrupt, protected mode and FPU in C; boot.js's layout; heap.js; c4bb's device registers (UART, clocks, ramdisk drives, heap, swap to RAM above the arena). `c4ix32.c4r --demo` matches the golden over serial in QEMU; inst/s measured in QEMU and DOSBox-X | ~4,000 lines; 3–5 sessions |
| M2 | **the MMU** | mmu.js's user loop, TLB, code and stack windows, IOPM, DMA checks; C4IX boots with the MMU on; test-c4pc-mmu | ~1,500 lines; 2–3 sessions |
| M3 | **console and DOS** | PS/2 keyboard through keys.js's logic; ANSI on VGA text; the DOS loader; a shell typed at in DOSBox and DOSBox-X; test-c4pc in the gate | ~1,500 lines; 2–3 sessions. **Decision point for route (b)** |
| M4 | **display** | VBE/BGA modes; the renderer for every command; bitmap fonts; the PS/2 mouse; the desktop boots and is driven in QEMU (test-c4pc-desktop) and DOSBox-X | ~3,000 lines plus fonts; 4–6 sessions |
| M5 | **sound** | SB16 PCM with the mixer; the OPL direct, with a timed queue; MPU-401; the speaker; Doom's music in DOSBox; opl.js in C for hosts without an OPL | ~2,000 lines; 2–4 sessions |
| M6 | **storage** | ATA PIO and FAT for drives that persist; DOS services for DOSBox's host folders | ~2,500 lines (less with FatFs); 3–4 sessions |
| M7 | **SMP** | APs started, an engine per CPU, devices on CPU 0; test-c4pc-smp with `-smp 4` | ~1,000 lines; 3–4 sessions |
| M8 | **x86-64** | section 8 | 2–4 sessions |
| M9 | *optional:* **a translator** | hot c4 functions compiled to x86 in c4pc, falling back to the interpreter | open |

Route (a) to a desktop with sound in DOSBox is M0–M5, about 15–25
sessions. All of it through x64 is about 25–35. Route (b), if chosen at
M3, comes after the layout's L1 steps and would be its own plan. A
rough shape:

1. the arch interface in c4vm, with no change in behaviour;
2. arch/i386 with gcc, booting to init with kernel threads only;
3. the ring-3 interpreter (b2) and user tasks;
4. mmu.c on real tables;
5. native SMP.

That is about 15–25 sessions more, reusing c4pc's drivers and boot.

## 7. Risks

- **Speed in DOSBox.** It is estimated at 15–40M c4 instructions a
  second. If M1 measures the low end, the desktop in DOSBox is a demo,
  not a place to work, and DOSBox becomes the place to test the drivers
  while QEMU, VirtualBox and VMware are where C4IX is used. The
  translator (M9) or route (b) are the answers to that, and both are
  large.
- **Two engines to keep in step.** Opcode numbers are already mirrored
  in c4m.c, c4mp, oisc4, c4bb, libjs and the compilers' tables. c4pc is
  one more. The mitigation is the same images and goldens: c4pc runs
  test-libjs's guest programs (smp0, smp1, smpwin, spinmp) and the C4IX
  goldens, so a missed opcode fails a test.
- **Host-thread assumptions.** The display's bell drains the ring
  synchronously, inside the store, which is easy in a single engine. But
  sound is stamped on a stream clock and rendered later. On a real card,
  OPL writes must go out on time from an IRQ, and the jitter of a 1 kHz
  PIT is a millisecond. That is audible only in fast arpeggios; a faster
  RTC IRQ 8 (up to 8 kHz) is the fix.
- **Fonts and look.** The desktop's text was laid out against browser
  fonts. Bitmap fonts at winkit's sizes will shift metrics, and some
  tests (test-desktop's window sizes) assume the browser's. Expect a
  "c4pc" column in the goldens.
- **Memory in DOSBox.** 63 MB in plain DOSBox has to hold the arena
  (32 MB is comfortable for the desktop), c4pc, and a ramdisk. That
  works without the games' big files, which need DOS services (M6) or
  DOSBox-X's larger memory.
- **The real-mode corners.** The DOS loader must not be run under
  EMM386 (raw protected-mode entry fails under a V86 monitor). VCPI
  support would cure it, at some cost. VBE mode changes after boot are
  not possible without BGA.
- **Real hardware differs from emulators.** Modern PCs have USB
  keyboards (BIOS legacy emulation usually covers PS/2), UEFI without a
  BIOS (no VBE, no INT 13h), and HD Audio rather than an SB16. DOSBox
  and VMs are the target. Bare metal is a bonus, best tried with an
  older machine.
- **c4mp does not boot C4IX** (*measured*, a segfault). It does not
  block route (a), which ports libjs. But it says no C engine has run
  C4IX before, so M1 will find bugs that libjs has already shaken out.
- **For route (b) only.**
  - Arguments are pushed in the opposite order. c4 pushes left to
    right, cdecl right to left, and `va.c` and the C4KE layer's
    argument handling assume c4's order.
  - `int` is the pointer's size only on i386, not on x86-64 without
    `#define int __INTPTR_TYPE__`, which c4.h already does for native
    builds.
  - gcc's undefined-behaviour optimizations need `-fwrapv
    -fno-strict-aliasing` at least.
  - Two builds of one kernel can drift apart; the c4vm build stays the
    reference.

## 8. The x86-64 milestone

**Route (a), M8.**

- **Entry.** c4pc enters long mode from its 32-bit entry: enable PAE,
  load a 4-level table (PML4 → PDPT → PD with 2 MB pages,
  identity-mapping the arena and c4pc), set EFER.LME, turn on paging,
  and far-jump to a 64-bit code segment. The IDT gets 16-byte gates.
- **The engine,** compiled `-m64`, can run both word sizes:
  `c4ix32.c4r` with a 32-bit guest word, as libjs does, and
  `c4ix.c4r`, the 64-bit image native c4m boots today.
- **Memory.** Above 4 GB the arena could be big, but C4IX's MMU keeps
  its 256 MB user window, and memory beyond 256 MB keeps the identity
  map and no swap (c4ix-mmu.md §11). Nothing there needs changing for a
  first x64 boot.
- **Boot.** Multiboot2 through GRUB covers VirtualBox and VMware in
  BIOS mode. **UEFI** is a further step: a PE entry, GOP for the
  framebuffer in place of VBE, and no INT 13h or INT 10h. VMware and
  VirtualBox boot either way.
- **Testing.** `qemu-system-x86_64` TCG on helios for the gate;
  VirtualBox and VMware on the Windows machines for speed. DOSBox has
  no long mode, so x64 leaves it behind.

**Route (b) on x64.**

- **Paging.** mmu.c's two levels become four, with 9 bits a level and
  8-byte entries. `MMU_PTE0` and `MMU_TBL` (mmu.c 212–215) become a
  level walk. NX (bit 63) finally lets X be enforced by the hardware,
  and PAE on i386 would give the same on 32 bits with three levels.
- **The word.** gcc's `int` is 32 bits, so the kernel is built with
  `-Dint=long`, as c4.h does.

## 9. A Linux-like layout for C4IX

### What is there today

`src/c4ix/` holds the thirteen kernel modules and `c4ix.h` at its top,
next to userland folders: `include/` (the user API: `c4ix_user.h`,
`window.h`, `display.h`), `lib/` (libc4ix, libarc), `libc/`, `user/`
(the programs and the desktop), `examples/`, `tests/` and `art/`. The
kernel has no inner structure: architecture-specific code (the trap
frame, page-0 registers, the cycle interrupt, the CPU-local window, the
MMU's registers), drivers (drive, display, sound and mailbox registers,
spread through sys.c, task.c, vfs.c and boot.c) and generic code share
files.

### The proposal

Only the kernel moves. The userland folders stay where they are,
because thirteen game repositories compile against
`src/c4ix/libc/include`, `src/c4ix/include` (`display.h`, `window.h`,
`window.c`, `archive.h`) and `src/c4ix/lib/archive.c` (*measured*:
every `games/*/c4.mk`, wolf3d's `arc.c`, and the host-math files of
puzzles and sbk). Moving those would mean a coordinated commit in every
game repo. Linux's split between `include/linux` (the kernel's own) and
`include/uapi` (what userland sees) maps onto that: today's
`src/c4ix/include` *is* the uapi, so the kernel's headers go into a
folder beside it.

    src/c4ix/
      arch/
        c4vm/                     the c4 VM: c4m, libjs, c4bb, c4pc
          include/asm/            trap numbers, CONF_*, C4I_* bits, the
                                  trap frame, the intrinsic wrappers,
                                  page-0 register numbers, MMU registers
          kernel/  entry.c        trap entry and TLEV return, the context
                                  (sv_*), the cycle-interrupt timer,
                                  CPU start and IPI          (from sched.c)
                   host.c         host detection             (host.c)
                   va.c           c4's varargs area          (va.c)
                   setup.c        the boot report, image and boot-stack
                                  registers                  (from boot.c)
          mm/      mmu.c          PTE bits, PTBR/INVLPG/FLUSH/KSP/IOPM,
                                  the kernel's map           (from mmu.c)
                   heap.c         kmalloc over MALC/FREE
        i386/                     route (b), later: boot, GDT/IDT/TSS,
                                  entry.S, paging, APIC, kernel heap
        x86_64/                   later still
      init/        main.c         boot sequence and init     (boot.c, init.c)
      kernel/      sched.c        the scheduler, run queue, load control
                   task.c         tasks
                   sys.c          the syscall dispatcher
                   printk.c       kprintf and the kernel log (from console.c)
      mm/          sl4b.c         the slab allocator         (sl4b.c)
                   vm.c           regions, umalloc, uaccess, lending
                   swap.c         frames, the clock, swap, memory on
                                  demand, compaction         (the rest of mmu.c)
      fs/          vfs.c          vnodes, files, fd tables   (from vfs.c)
                   pipe.c, ramfs.c, procfs.c, hostfs.c       (from vfs.c)
                   binfmt_c4r.c   the .c4r loader            (loader.c)
      compat/      c4ke.c         the C4KE personality       (c4ke.c)
      drivers/
        tty/       console.c      console input and output   (from console.c)
        c4vm/      drive.c, gui.c, snd.c, mbox.c, swap.c
                                  the page-0 devices' glue (from sys.c,
                                  task.c, vfs.c, boot.c, mmu.c)
        pc/        (route b)      ps2.c, vga.c, vbe.c, ata.c, sb16.c, opl.c
      include/c4ix/               the kernel's headers, split from c4ix.h
                                  (c4ix.h kept as an umbrella that includes them)
      include/  lib/  libc/  user/  examples/  tests/  art/   unchanged

Notes on the names:

- **`hostfs.c`** is the host-file layer (`SYS_LAZYFILE`, a vnode backed
  by a host file). That is UML's name for the same thing.
- **`compat/`** is not one of Linux's folders. It holds C4KE's syscall
  ABI as a personality, which is what it is.
- **c4pc**, route (a)'s machine, is not part of C4IX. It goes beside
  libjs and c4bb, as `src/c4pc/`, with its own `drivers/`.
- **Every kernel `.c` keeps a unique basename** across the tree. The
  ladder's source kit can then stay flat (below).

### In three steps, each gated

- **L1a, paths only.** `git mv` the thirteen `.c` files to their
  folders with no split, and leave c4ix.h where it is, so every
  `#include "c4ix.h"` and every `-I src/c4ix` still works. The link
  order stays `boot console va host sl4b task sched vfs sys c4ke loader
  mmu init`.
  - Expected: `c4ix.c4r` (built without `-g`) is byte-identical. grep
    found no `__FILE__` in the kernel.
  - `c4ix32.c4r`, built `-g` for the source maps, differs only in its
    SMAP's file names. `c4rdump -l` shows which.
  - The whole gate runs.
- **L1b, the headers.** c4ix.h is split into `include/c4ix/*.h` and
  `arch/c4vm/include/asm/*.h`, with c4ix.h as the umbrella. The images
  are byte-identical when the split only moves declarations; the gate
  includes test-c4fc, which refuses use-before-declaration where c4lc
  accepts it, so the order of the new headers matters.
- **L1c, the splits.** mmu.c, vfs.c, sched.c, console.c and boot.c are
  split along the lines above. Code moves between objects, so the
  images change, but behaviour must not: test-c4ix, test-c4ix-mmu,
  test-c4ix-smp, test-libjs and test-c4ix-rebuild all pass, and the
  boot cycle count (`bench-c4ix`) is within noise.

### What the move means for each user of the paths

- **The Makefile.**
  - `C4IX_MODS` becomes paths (`init/main`, `kernel/sched`, `mm/sl4b`,
    ...), and the rules that loop over it change their object names: 93
    lines name `src/c4ix` or `C4IX_SRC`.
  - **`c4lc_compile_par`** builds `$(3)/{}.c` into `$(4){}.c4o` with one
    `-I $(3)`. A module name with a slash puts a slash into the object
    name (`.c4ix_kernel/sched.c4o`, a directory that does not exist).
    So the macro gains a fifth argument for extra `-I` flags, and writes
    `$(4)$$(echo {} | tr / _).c4o`. c4or1k's two calls keep their
    meaning.
  - The c4sc checks at lines 1166–1261, 1363 and 1443 loop the same
    way, and `C4FC_PP_SWEEP` uses the pattern `src/c4ix/%.c:src/c4ix`;
    both change to match.
- **test-c4fc's byte-identical kernel.** It compiles each module with
  c4lc and with c4fc, requires the objects to match, links c4fc's into
  `.c4fc_ix.c4r`, and requires that to equal `make c4ix.c4r`.
  - Both sides take their list from `C4IX_MODS`, so the check holds
    across the move, as long as the order of the list (the link order)
    is unchanged.
  - The object names need the same `tr / _`.
  - c4fc must be given the same `-I` list as c4lc; its `-I S" ..."` form
    takes several.
- **`build-images.sh`.** It has its own `C4IX_MODS` (line 326), builds
  `src/c4ix/$m.c` with `-I src/c4ix` into `.c4bb_ix_$m.c4o` (line 387),
  and watches `find src/c4ix` for changes (379). The first two change
  as the Makefile does; the third needs nothing. The root archive ships
  the source tree under `/usr`, so C4IX sees the new folders inside
  itself with no further change.
- **`src/c4ix/BUILD.B4K`** (the rebuild inside C4IX) lists all thirteen
  modules by path, with `DEPS` lines on c4ix.h. It is rewritten with
  the new paths, the same flags and the same order. test-c4ix-rebuild
  compares what it builds with the disk's images byte for byte, which
  both sides make from the new paths. `user/`, `lib/` and `libc/`
  BUILD.B4K files are untouched.
- **The C4DOS → C4KE → C4IX ladder.** `IX.BAT` runs `dostar.c4r x
  c4ix-src.tar` and then thirteen explicit `c4sc.c4r ... -I src/c4ix
  src/c4ix/NAME.c` lines. `c4ix-src.tar` is made from `C4IX_KIT`, whose
  entries are `repo-path:tar-path` pairs (Makefile 476).
  - **So the ladder can keep a flat tree.** Map
    `src/c4ix/kernel/sched.c:src/c4ix/sched.c` and so on, with c4ix.h
    and the split headers at their flat names. IX.BAT and the C4KE-side
    build then need no change at all in L1a, and only new file names in
    L1c.
  - This relies on unique basenames. It also spares C4DOS deep paths,
    whose limits on its RAM disk would need checking otherwise.
  - The ladder builds without `-g`, so its kernel has no source map to
    disagree with the host's.
- **The game ports.** They use only userland paths, so L1 changes
  nothing in any game repository. A later move of the uapi, if ever
  wanted, should leave forwarding headers at the old paths and land in
  the game repos first, as the toppings merge did.
- **Everything else** that names a kernel file does so in comments or
  docs:
  - c4ke.c, c4dos.c, dosload.c, c4rlink.c, load-c4r.c, c4mp's
    loader.c, devices.js, boot.js, cli.js and c4ix.vfs.txt;
  - two scripts, `src/tests/test_ixbuild.c` (`src/c4ix/boot.c` as its
    compile job) and `src/c4bb/tests/climb-disks.sh` (copies
    `src/c4ix/sched.c` as a sample source);
  - `src/c4sp/tests/srcmap-invariant.sh`, which pairs every module by
    path.

  The scripts change. Comments and reference docs get a sed pass.
  History docs keep the old paths, since they describe the tree as it
  was.

## 10. What the user must decide or install

Nothing has been installed or downloaded for this study. Helios already
has gcc 15 with multilib (`-m32`, `-m16`), GNU ld 2.46, `grub-file` and
`grub-mkrescue` (with `x86_64-efi` modules only), and valgrind.

| what | why | from where | size (*estimate*) |
|---|---|---|---|
| **QEMU**, `qemu-system-x86` (and `qemu-utils`) | the headless gate for M0 onwards; i386 and x86-64 | Ubuntu archive, `apt install qemu-system-x86` | ~100–150 MB installed with dependencies |
| **DOSBox-X** | boot images, IDE, PS/2 mouse, a headless try in M3 | Ubuntu universe (`dosbox-x`), or Flathub, or the project's GitHub releases for Windows | ~20–40 MB |
| plain **DOSBox** (or DOSBox Staging) | the user's own checks of the DOS loader, on Windows | dosbox.com, or dosbox-staging.github.io | ~10–30 MB |
| `grub-pc-bin`, `xorriso`, `mtools` | only if GRUB ISOs or floppies are wanted, rather than our own boot sector | Ubuntu archive | ~10 MB together |
| **Bochs** | only when a triple fault needs its debugger | Ubuntu archive (`bochs`, `bochs-sdl` or `bochs-term`) | ~10 MB |
| **bitmap fonts** for the desktop (M4) | `TEXT2`'s nine faces | a choice: draw our own; X11's misc-fixed (public domain); Terminus (OFL); GNU Unifont (GPL with font exception); or rasterize free TrueType faces (DejaVu, Liberation) ahead of time with a host tool | small; the licence is the decision |
| **FatFs** (M6), or write our own FAT | drives that persist on IDE | elm-chan.org, 1-clause BSD | ~100 KB of source |
| VirtualBox and/or VMware on a Windows machine | the x64 milestone, and speed | the user's own machines; Hyper-V Gen 1 on Cyron works for i386 without anything new | — |

And the decisions:

1. **Route (a) first?** (recommended) And the name, `c4pc`, and its
   place, `src/c4pc/`.
2. **GRUB, or our own boot sector?** Our own needs nothing installed,
   and DOSBox-X boots it. GRUB is the safer bet on VMs and real
   machines.
3. **Whether DOSBox must run the desktop well**, or only boot it. That
   sets whether M9 (a translator) or route (b) becomes urgent after M1's
   measurement.
4. **Fonts:** which, under which licence.
5. **The layout:** whether to do L1a now (it is independent of the
   port), and whether userland should ever move (recommended: no).

## Appendix: the probes behind the measured numbers

Everything is in helios `~/i386probe`, outside the repository, on
2026-11-21:

- `gcc/shim.h` and the twelve `.o` files: the route (b) compile. The
  command is in section 3.
- `spin1000000.c` and `spin3000000.c` (the counting loop from gcc-port's
  `~/gccprobe/spin.c`), run as `valgrind --tool=lackey ./c4m
  spinN.c`:
  - 775,259,134 and 2,323,259,150 guest instructions;
  - 26,000,018 and 78,000,018 c4 cycles;
  - 29.8 x86 instructions per c4 instruction.
- `c4mp_*.o`: c4mp built `-m32 -c`. Its undefined symbols are the
  libc a freestanding c4mp would need: `malloc free memcpy memset memcmp
  open read close printf putchar puts clock_gettime usleep signal raise
  tcgetattr tcsetattr cfmakeraw isatty atexit exit` and `__udivdi3`,
  about twenty functions.
- `./c4mp c4ix.c4r --help`: segfault (exit 139), against `./c4m
  load-c4r.c -- c4ix.c4r --help`, which boots.
