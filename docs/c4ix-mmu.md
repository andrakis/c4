# C4IX's MMU: paged virtual memory with protection in libjs

STATUS (2026-10-29): designed, with the baseline measured (section 12).

C4IX runs every task in one flat arena. Its protected mode (c4m's `mode`
register, `sys_pmviolation` in `src/c4ix/sys.c`) stops a user task from
running the host's IO opcodes, but not from storing anywhere: a wild
pointer in one program can write over the kernel or another program, and
the machine dies somewhere else, later. This document adds the other half
of protection: a memory management unit in libjs, page tables that C4IX
owns, and a page fault that kills only the task that took it.

The MMU exists only when the host fits it (`?mmu=1`, `cli.js --mmu`, the
Configuration dialog's Machine tab) and only does anything once C4IX turns
it on. Every other system -- C4KE, C4DOS, grin, bare programs -- never
sees it, and runs the interpreter loop it ran before, unchanged.

## 1. How libjs reaches memory today

`libjs/c4m.js` is a direct interpreter over one `Int32Array` (and a
`Uint8Array` view) of the arena. Registers are byte addresses into it.
The loop in `Machine.run` keeps `pc`, `sp`, `bp` and `a` in locals and
touches memory in these ways:

| what | opcodes | how |
|---|---|---|
| instruction fetch | every one, and the operand of LEA IMM JMP JSR BZ BNZ ENT ADJ LDL LDG PSHL PSHG LEAP IMMP STL JSRI JSRS | `i32[pc >> 2]`, no check |
| the stack | PSH, JSR, ENT, LEV, the binary operators (pop), POPA, _JMP, _ADJ, and the pushes of the fused opcodes | `i32[sp >> 2]`, no check |
| frame slots | LDL, STL, PSHL, JSRS | `i32[(bp >> 2) + n]`, no check |
| loads and stores through a pointer | LI LC SI SC LDG PSHG LIP ADDL JSRI | inline when aligned and at or above 0x1000, else `ld32`/`st32`/`ld8`/`st8` |
| block opcodes | MSET MCMP MCPY, READ (the disk controller writes the arena), PRTF and PUTS (read strings), FLT and FPU (arguments on the stack) | `sys()`, through `ld32` and typed-array methods |
| traps | `trap()` builds a 10-word frame below `sp`; TLEV reads it back | `i32[]` direct |

Below 0x1000 is the bus (`libjs/bus.js`): the null guard, the TLEV ROM
word, c4bb's device registers at 0x100, the opcode-name ROM at 0x200, the
display at 0x400, sound at 0x480, the CPU-local window at 0x800 with SMP.
Every access there takes the slow path already.

Hosts touch the arena too, by address: the disk controller writes what
READ reads, the display reads command rings and framebuffers and writes
its event ring, the sound card reads sample buffers, and the mailbox's
rings are the host's and the guest's at once.

So translation has to go in four places: the fetch, the stack and frame
accesses, the pointer loads and stores, and the block opcodes. Doing it in
the existing loop would put a table lookup and a compare on every one of
them for every guest, C4IX or not.

## 2. The decision that shapes the rest: the kernel runs untranslated

User mode is translated; kernel mode is not. That is MIPS's kseg0, or a
Linux kernel's direct map, taken to its simplest form: in kernel mode an
address is a physical address, and the kernel sees all of memory.

- The loop that runs in kernel mode is `Machine.run`'s loop as it is.
  C4IX's kernel, its trap handler, its scheduler and its kernel threads
  pay nothing. So does every other system, which never leaves mode 0.
- User mode gets a loop of its own (`runUser`, `libjs/mmu.js`), selected
  when the MMU is on and `mode` is 1. `Machine.run` looks once per call:
  `if (this.mmu && this.mmu.on) return this.mmu.run(...)`, then the MMU's
  dispatcher runs whichever loop the mode calls for until the budget is
  spent. The only places the mode changes are `trap()` and TLEV, and both
  are already on the loop's slow path (`sys()` and `boundary()`), so the
  switch between loops costs nothing per instruction.
- C4IX maps user memory at the same virtual address as its physical
  address (an identity map), so a pointer a task hands the kernel in a
  syscall is a pointer the kernel can use, exactly as now. The kernel
  checks that the task may use it (`uaccess`, section 8), as `copy_from_user`
  does. What a task can reach is decided by which pages its page table
  maps and how, not by where they are.

The identity map is a choice of the kernel's, not a limit of the MMU:
the page tables translate, and section 11 is what it would take for C4IX
to give a task addresses that are not physical ones.

## 3. The page size: 4 KB

- The heap already aligns its regions to 4 KB (`boot.js`), and Linux and
  x86 use 4 KB, which keeps the explanations short.
- A 64 MB arena is 16,384 pages: a page table of 1,024 entries covers
  4 MB, so a task's image, stack and blocks touch a handful of them.
- Smaller pages would protect more finely but cost more table and more
  misses; bigger ones (64 KB) would make every guard page and every small
  task cost 64 KB, and would round user allocations, which C4IX makes by
  the thousand, up to a size that wastes the arena.

## 4. Page tables

Two levels, as on the i386: a virtual address is 10 bits of directory
index, 10 of table index and 12 of offset. Both levels are one 4 KB page
of 1,024 words, page-aligned, in VM memory. The kernel allocates, fills
and frees them (`src/c4ix/mmu.c`); the MMU only reads them, and sets the
accessed and dirty bits.

    PDE  31..12 the page table's physical page   0 P (present)
    PTE  31..12 the frame's physical page
         0 P  present            1 W  writable          2 U  user may use it
         3 X  user may execute   5 A  accessed (MMU)    6 D  dirty (MMU)
         9..11 left to the kernel (C4IX keeps its books elsewhere, below)

There is no NX bit to set: execution needs X, so data is not executable
unless the kernel says so. A PDE has no permission bits of its own; the
PTE decides. A page the MMU cannot use is one without P, without U, or,
for a store, without W, or for a fetch, without X.

The MMU's registers are a 64-byte window at 0x500, below the CPU-local
window and above the sound card; like the local window every CPU sees its
own. A guest finds the MMU by `__c4_info() & C4I_MMU` (0x80000) and never
pokes 0x500 otherwise, because on native c4m that address is memory.

    0x500 CTL     r: 1 if fitted, 3 if on.  w: 1 on, 0 off
    0x504 PTBR    r/w the page directory's physical address; a write
                  flushes this CPU's translations
    0x508 KSP     r/w the stack a trap from user mode switches to (0: none)
    0x50C FADDR   r   the last fault's virtual address
    0x510 FINFO   r   its kind: 1 read, 2 write, 4 fetch; 8 if the page was
                  present (a protection fault), 0 if it was not
    0x514 FPC     r   the pc of the instruction that faulted
    0x518 INVLPG  w   forget the translation of this virtual address
    0x51C FLUSH   w   forget every translation
    0x520 IOPM    r/w which of page 0's windows user mode may use (section 7)
    0x524 MISSES  r   translations filled since the last read (statistics)
    0x528 FAULTS  r   faults raised since boot
    0x52C MEMSIZE r   the arena's size, so the kernel knows what it maps

## 5. The TLB: a JS-side cache of translations

A page table walk is two loads and a handful of tests; doing it for every
access would be the whole cost. So each CPU keeps three `Int32Array`s, one
each for reading, writing and fetching, indexed by virtual page number.
An entry holds the physical address of the page, or -1 where the
translation is not cached or not allowed:

    v = x >>> 12; p = v < LIMIT ? rd[v] : -1;
    if (p >= 0 && (x & 3) === 0) a = i32[(p | (x & 4095)) >> 2];
    else -> slow path: page 0, an unaligned word, a fill, or a fault

The arrays cover the first 256 MB of virtual space (65,536 pages, 768 KB a
CPU); an address above that always walks, and with an identity map there
are none. A fill writes all three arrays from one PTE, so a page that is
read and written is walked once. Every fill is logged, so a flush puts
back -1 only where it wrote, not over all 65,536 entries: a context switch
costs the pages the outgoing task touched, typically tens.

The accessed bit is set on a fill, and the dirty bit on a fill for
writing (a page is not put in the write array until it is dirty, so its
first store sets D): what a swapper's clock needs (section 11), and
nothing on the hit path.

The arrays are what LI, LC, SI, SC and the fused global loads use. They
are cheap. What is not cheap, measured (section 12), is a compare on
every fetch and on every push and pop: c4 code is mostly those. So two
windows sit in front of the arrays, and the loop does not compare against
them on every access either:

- the **code window** is the page `pc` is in, and the next one when it
  follows in physical memory and may be run. The fetch reads through it
  without looking. The loop already stops at the top every so often, for
  the budget and the interrupt checks (`lim`); now it also stops when the
  instructions since the window was filled could have run off it -- each
  is 8 bytes at most -- and after every jump, call and return, which set
  `lim` to -1. A jump is maybe one instruction in six; a fetch is every
  one.
- the **stack window** is the run of pages around `sp` the task may read
  and write, contiguous in physical memory too. Pushes and pops do not
  look at it: each moves `sp` a word at most, so the same countdown covers
  them -- the top of the loop counts how many words there are between `sp`
  and either end, and ENT, ADJ, LEV and _ADJ, which move `sp` further,
  end the count. When `sp` is near an end the window grows by the next
  page, if that is the task's too. **If a word either side of `sp` is
  not writable, the instruction faults before it runs**: `sp` has left the
  stack (or run into the guard page), and the next push or pop would fault
  anyway. That is the one place the MMU is not precise to the access; it
  is to the instruction, whose registers are as they were.
- Frame words (LDL, STL, PSHL, JSRS, LEV's two words) are at `bp` plus an
  operand anything could have written, so each is compared with the
  window, and goes through the arrays when it is outside.

A code window and an operand: an instruction whose opcode is the last word
of a page has its operand on the next. If that page is not mapped next to
this one in physical memory, the fetch faults (FINFO bit 4). The kernel's
loader never maps code that way; an identity map cannot.

## 6. User and kernel; read-only and no-execute

`mode` is c4m's: 0 kernel, 1 protected. With the MMU on:

- Every trap enters kernel mode. On c4m and on libjs without an MMU,
  `__c4_trap` and DEBUG trap without leaving protected mode, which is why
  C4IX's user tasks yield through the syscall gate (`sched_yield`); with
  translation on, a handler running translated could not reach the
  kernel at all. The frame keeps the old mode, and TLEV restores it.
- A trap from user mode switches to the kernel stack in KSP before it
  builds its frame, as an x86 does through its TSS. Without that a task
  with a wild `sp` would have the trap frame written wherever `sp`
  pointed, into the kernel, by the trap that was meant to stop it.
- TLEV in user mode (a C4KE program's signal handler returning) cannot
  raise its privilege: it restores `pc`, `sp`, `bp` and `a` from the
  frame, but not the mode or the preemption interval.
- The opcodes that control the machine rather than the task -- CPUS,
  IPI, CWAK and C4CF -- trap as protected-mode violations from user mode,
  as ITH, SIGH and USLP already do.
- U, W and X are checked by the user loop's arrays, and user-mode block
  opcodes (MSET, MCMP, MCPY) translate a page at a time, so a `memcpy`
  into the kernel faults as a store would.
- What the MMU has to say about a page is P, U, W and X, and nothing
  else: read-only text and no-execute data are only the kernel mapping
  code without W and data without X (section 9).

## 7. Page 0

Page 0 is the bus, not memory, and a page table entry for it would have
to say one thing about the null guard, the ROM, c4bb's devices, the
display and the CPU-local window together. So page 0 is never translated:
user-mode accesses there take the slow path they always took, and the
**IOPM** register says what user mode may do there, as an x86's I/O
permission bitmap does:

    bit 0  read the ROM: the null guard, the TLEV word, the opcode names;
           fetch the TLEV word, where trap frames return
    bit 1  c4bb's device registers, 0x100-0x1FF
    bit 2  the display, 0x400-0x47F
    bit 3  the sound card, 0x480-0x4FF
    bit 4  read the CPU-local window (the shared libraries' data slots)
    bit 5  write it

At reset every bit is set, which is what user mode could do before there
was an MMU; a kernel takes away what it keeps. The MMU's own registers
are never user mode's, and nor is anything else in page 0. A user access
the IOPM refuses is a page fault like any other: a store through a null
pointer is now a fault and a kill, where libjs dropped it (and said so)
before.

C4IX (stage 4) gives every user task the ROM, the display, the sound card
and reading the window. The window is its own `struct cpu`, which a task
that could write it could make itself the kernel's idle task with.
c4bb's devices -- the drives, the heap's generations, RESET -- are for the
task that asks, by `SYS_IOPERM`, as on Linux: only `persist` does, to
stream changes to drive 1. A null pointer plus an offset is the commonest
wild store there is, and 0x100-0x1FF is where it lands.

## 8. Faults: a trap the kernel already knows

A fault raises the trap c4m already numbers SEGV (4), with the faulting
virtual address as its parameter; FINFO and FPC say what and where. The
saved registers are the faulting instruction's as it found them -- pc its
first word, sp before any pop -- so the instruction can be run again once
the kernel has mapped the page. A slow path in the user loop saves them
before it touches memory, and an opcode run in `sys()` (MCPY, the FPU)
that faults part way throws, and the loop raises the fault from the
registers it kept. C4IX uses that once: a C4KE program that writes its own
code (C4KE's ps patches itself) has its text made writable and the store
run again (section 9). The other use, paging in, is section 11's.

C4IX's trap handler (`sched_trap`) treats any other SEGV from a user
task as the end of it (`mmu_fault`): a line to the console and the kernel
log that says which task, what kind of access, at which address and pc,
and whose the address is -- the kernel's image, the kernel's heap, another
task (by name), the stack guard of a task, shared library code, page 0 --
and whether it was mapped at all:

    c4ix: task 12 (wild.c4r): page fault: write to 0x00002000 (the kernel's image, not mapped) at pc 0x0011a03c; killed

The task dies with SIGSEGV (`killsig` 11, which `waitpid` reports), its
parent's `wait` returns, the shell prints its prompt, and every other task
carries on. Without a trap handler (no kernel) libjs stops the machine
with the same facts.

A syscall given a pointer the task may not use gets -1 rather than a
fault, Linux's EFAULT: `uaccess(p, n, w)` and `ustrlen(s)` (`mmu.c`) walk
the calling task's own page tables. `sys_args_ok` (`sys.c`) checks every
syscall's buffers, paths and vectors before it runs; write, read and open
look for themselves, since `sys_pmviolation` reaches them for a trapped
host opcode too; PRTF's format and PUTS's string are checked; and the C4KE
layer's calls that take or hand out memory check theirs, and hand back
copies in the program's own memory where its kernel would have handed it a
pointer into itself (`OP_VFS_GET`, `OP_VFS_NAME`, the task table export).

## 9. Address spaces and the context switch

An address space is a page directory and the page tables under it, made
on first use (`mmu_dir_of`) and freed with the task. Each user task has
one, and in it:

- its image's code, read and run (P U X), its data and BSS, read and
  written (P U W), its libraries' data, its stack (P U W, with a guard
  page below it that is never mapped), its argv and environment, and the
  blocks it allocates (`SYS_SBRK`, `malloc` behind protected mode);
- the code of the shared libraries it loads, mapped from the one cached
  copy (P U X), and the pages of `__c4_dbase` slots (P U);
- a page of its own to start in (P U X: below);
- nothing of the kernel's, and nothing of any other task's.

No page is both writable and executable. A C4KE program is the one
exception: C4KE's ps patches its own code, so a C4KE program (one that has
made a C4KE call, `ckabi`) that writes into its own code has that code
made writable, and goes on. A C4IX program that does the same is killed.

A vfork child runs in its parent's space, with its own stack mapped into
it, because it is using the parent's memory until it execs; then it gets
its own. Each region remembers the space it was mapped in, so the child's
old stack comes out of the parent's when it goes. Kernel tasks have no
space: they run in kernel mode, and the switch to one leaves the last
user space loaded, which the next user task is often the owner of.

**Every user allocation is whole pages.** The heap hands out 8-byte
aligned blocks, so a task's block and the kernel's could share a page, and
a page is the unit of protection. So `umalloc` (`mmu.c`) takes `n + 4095`
bytes from the heap and uses the aligned pages inside: a **region**. Its
books -- which task owns it, where it is mapped, what kind it is -- are an
SL4B object of the kernel's, never a header in the pages, which the task
can write; and the kernel keeps a word per page of the arena, the region
there, which is how a free finds its region and how a fault knows whose an
address is. Small blocks come from slabs: a page of a task's own cut into
blocks of one size, 16 to 2,048 bytes, with a bitmap in the kernel's
object, so a compiler's ten thousand small mallocs cost the pages their
bytes need, not ten thousand pages. A free of something that is not one of
the task's own blocks does nothing (it freed the kernel's block before).

What the kernel makes for a task before the task exists -- its image, as
`c4r_load` reads it; its stack; its argv and environment -- is umalloc'd
and the kernel's until `mmu_adopt` gives it to the task and maps it, when
`task_spawn` has made the task, when vfork has, and after execve.

**The switch.** `loader_switch(n)`, which every switch already calls to
point the shared libraries' data slots at the incoming task, writes the
incoming task's directory to PTBR (`mmu_switch`), and its IOPM. The write
flushes this CPU's translations. Nothing else changes: the trap handler
runs in kernel mode, untranslated, whichever space is loaded, and the
TLEV that ends it enters user mode in the new one.

**The task shim.** A user task used to start in `task_shim`, kernel code
that read the task's constructors from its struct, ran them, called
main, ran the destructors and exited. User mode can do neither now, so
`mmu_user_entry` writes the same sequence, unrolled, into a page of the
task's own, as `boot.js` does for the machine's first program:

    ENT 0
    IMM 0; PSH; JSR ctor; ADJ 1         each constructor, in order
    LEA 3; LI; PSH; LEA 2; LI; PSH      argc, argv from the forged frame
    JSRS 4; ADJ 2                       the entry, from the frame too
    PSH                                 its result, under the rest
    JSR dtor                            each destructor, in reverse
    IMM SYS_EXIT; PSH; OPCD             exit(result)

`ck_signal_deliver` had a C4KE program's signal handler return to a word
of the kernel's holding TLEV; it returns to the ROM's TLEV word instead,
which user mode may run, and libjs's TLEV in user mode restores the
registers only (section 6).

## 10. SMP

`?cpus=N` (smp.js) and `?parallel` (par.js) give each CPU a Machine of its
own, and the MMU's registers and TLB are per Machine, as `mode` is. Page
tables live in the arena -- a SharedArrayBuffer on the parallel engine --
and a walk reads them with plain loads.

- Each CPU sets its own KSP to a kernel stack of its own at start-up
  (`smp_ap`), and its own PTBR at every switch it makes.
- A CPU's TLB only ever holds the space it is running. C4IX changes a
  task's space while the task runs only from that task's own syscalls
  (sbrk, malloc, spawn's argument copies), on its own CPU, with
  INVLPG; and frees a space only when its task is dead and off every
  CPU (`oncpu`). So no other CPU can hold a stale translation of it, and
  there is no shootdown. Threads sharing a space on two CPUs at once
  would need one: an IPI to each CPU in the space's CPU mask, which
  flushes, before the page goes back to the heap.
- The parallel engine's other CPUs ask CPU 0 for device work. None of
  that is translated: a device sees physical addresses, which with the
  identity map are the task's own.

## 11. What this does not do (yet), and how it would

**Addresses that are not physical.** Every task could be linked at the
same address (0x00400000, as on Linux), and its heap grow contiguously,
with a non-identity map. The MMU already translates; what C4IX would need:
the loader relocates to the virtual base while writing through the
physical one; the kernel copies to and from user memory through the
task's tables (`copyin`/`copyout`), not through the pointer; and devices
that take addresses from user mode (the display's rings and framebuffer,
the sound card's buffers) are given physical ones by the kernel, or
translate through the writer's tables, which means a framebuffer must be
physically contiguous or the device must gather pages.

**DMA protection.** A task may write the display's RING register, and the
host then writes events at that address. The kernel does not see the
store. IOPM bit 2 is on for user tasks because the desktop and the games
draw from user mode; an IOMMU check would be the display asking the MMU
whether the writer's space maps the range writable.

**Swap** (designed, not built). Page out: the kernel picks a page whose A
bit has stayed clear across a clock sweep, writes its 4 KB to a swap
file -- host drive 1, which is IndexedDB in the page and a directory on
the command line, through the drive registers persist already uses --
clears P and keeps the swap slot in the PTE's upper bits, and INVLPGs.
Page in: a fault on a not-present PTE with a slot is not a kill; the
kernel reads the slot back into a free frame, sets P, and returns from
the trap to the faulting instruction, which runs again (faults are
precise for exactly this). The read is the drive's, which can block
(`READ` returns -2), so the faulting task parks like a reader on an empty
pipe and the CPU runs someone else. With the identity map a page that has
been swapped out cannot be given to anyone else at its address, so swap
needs the non-identity map first; and C4IX would also need to know which
of its pages are the kernel's, which never go.

## 12. Measurements

Everything here was measured on helios (node v26.5.0), under the build
lock, with `libjs/tests/bench-mmu.sh` and `bench-desktop.mjs`: C4IX booted
by `cli.js -m 64`, unpaced, a scripted session each. The games' times are
their session's less the boot's, which every session starts with (`wait
4`, the root archive unpacked).

**Baseline**, before anything here changed (c4ix_and_toppings, ea6ba82):

| workload | session | cycles | M instr/s | the workload alone |
|---|---|---|---|---|
| boot, root archive unpacked (`wait 4`) | 6.19 s | 1,282,662,077 | 216.1 | |
| tar the root archive into /tmp | 12.20 s | 2,572,975,291 | 213.7 | 6.01 s, 1.29G cycles |
| Doom, demo3 timedemo | 53.44 s | 12,678,455,070 | 237.9 | 47.25 s: 2,173 frames, 46.0 fps |
| Wolf3D, demo 1 timedemo | 28.79 s | 5,963,519,835 | 208.3 | 22.60 s: 1,214 frames, 53.7 fps |
| ROTT, demo 4 timedemo | 21.74 s | 4,773,282,411 | 221.1 | 15.55 s: 609 frames, 39.2 fps |

(The stage tables follow as each stage lands.)

## 13. Left undone

(Filled in at the end.)
