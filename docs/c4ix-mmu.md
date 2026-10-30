# C4IX's MMU: paged virtual memory with protection in libjs

STATUS (2026-10-29): stages 1-10 built and committed on ix-mmu, measured
(section 12): translation, the user/kernel split, a space per task,
guard pages and read-only text (1-4); null reads, devices checked against
the task's tables, C4KE's text a page at a time (5); user addresses that
are not physical (6); swap, and the TLB kept right across CPUs (7); swap
kept by the host, which the kernel can read (8); a page at a time (9);
the TLB's shootdown by IPI (10); memory on demand, a clock over every
task's pages and the kernel's touches unpinned (11, 2026-10-30, on
c4ix-swap: section 11's "Memory on demand"). Fitted by the tests (cli.js --mmu,
createMachine's default, the browser checks' ?mmu=1) and while
developing; off by default in cli.js and the page since 2026-10-30, for
its cost (section 12). What is left is section 13.

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
- A pointer a task hands the kernel in a syscall is a pointer the kernel
  can use, as it always was. Through stage 5 C4IX mapped user memory at
  its own address (an identity map); since stage 6 user addresses are a
  window of their own, and kernel mode reaches them through the kernel's
  map (libjs's user window, section 11). Either way the kernel checks
  that the task may use the pointer (`uaccess`, section 8), as
  `copy_from_user` does. What a task can reach is decided by which pages
  its page table maps and how.

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

The arrays cover the first 512 MB of virtual space (131,072 pages, 1.5 MB
a CPU): memory, and the user window above it (section 11); an address
above that always walks, and C4IX makes none. A fill writes all three arrays from one PTE, so a page that is
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
loader never maps code that way: a region's pages are one run in memory
and one run of user addresses.

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

    bit 0  read the ROM: the null guard, the TLEV word, the opcode names
           (the TLEV word, where trap frames return, runs whatever the
           IOPM says)
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

C4IX gives every user task the display, the sound card and reading the
window. Not the ROM, since stage 5: a null pointer's read is a fault, as
its store is ("read of 0x00000004 (page 0, not allowed)"). The window is its own `struct cpu`, which a task
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
registers it kept. C4IX uses that twice: a C4KE program that writes its own
code (C4KE's ps patches itself) has the page it wrote made writable and the
store run again (section 9); and a task that touches its own memory on
swap has it brought back and the instruction run again (section 11).

C4IX's trap handler (`sched_trap`) treats any other SEGV from a user
task as the end of it (`mmu_fault`): a line to the console and the kernel
log that says which task, what kind of access, at which address and pc,
and whose the address is -- the kernel's image, the kernel's heap, another
task (by name), the stack guard of a task, shared library code, page 0,
nobody's -- and whether it was mapped at all. A device's access at an
address the task gave it is the task's fault too ("a device's write to",
section 11):

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
made a C4KE call, `ckabi`) that writes into its own code has the page it
wrote made writable, that page alone, and goes on. A C4IX program that does the same is killed.

**Lending** (`SYS_MAPFROM`, 245, libc4ix `umapfrom`). A compositor
draws what other programs made: the desktop shows a window's 8-bit canvas
by handing the display the program's framebuffer and palette (GUI_BLIT8),
which the display checks against the desktop's space (section 11). So a
task may map memory of a task it started, or one its tasks did -- the
window's shell's program -- read-only into its own space, at the same
address (user addresses are the machine's, so it is free there). The
region is pinned, so it stays where it is, and leaves the borrower's
space when its owner frees it; a small block's chunk is what is lent.
The desktop borrows a canvas's memory before it draws it.

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
- C4IX changes a running task's space from that task's own syscalls
  (sbrk, malloc, spawn's argument copies), on its own CPU, with INVLPG;
  and frees a space only when its task is dead and off every CPU
  (`oncpu`). Swap (section 11) takes pages out of other tasks' spaces.
  Two ways a CPU could keep a translation of such a page in its TLB, and
  a shootdown for each:
  - A CPU that ran the task last, and switched to a kernel task, which
    leaves the last space in: each swap-out bumps a generation
    (`mmu_gen`), each CPU remembers the one it last loaded a space at,
    and `mmu_switch`, finding the space it is about to run already loaded
    from an older one, flushes first -- the one moment the translation
    could be used.
  - A CPU running the task now (stage 10): the page leaves the task's
    tables, the CPU is asked to forget (`mmu_shoot`) and sent an IPI,
    which arrives as a trap whatever its mask, and the trap's first thing
    (`mmu_trap_in`), before it waits for the kernel lock the swapping CPU
    holds, is the flush and the answer. Only then is the page copied out
    and its frame given up, so no store of the running task lands after
    the copy. A CPU anywhere but user mode is not waited for: it flushes
    on its way back (`mmu_trap_out`). The flags change by compare-and-swap,
    a fence, so each CPU sees the other's in the order it was written.
- The parallel engine's other CPUs ask CPU 0 for device work. The ask
  carries the asker's PTBR and the kernel's map (par.js `K.PTBR`,
  `K.UDIR`), so CPU 0 checks and translates what the device is given
  against the asker's space, as the asker's own CPU would, and a refusal
  goes back to be raised there (`DMA_FAULT`).

## 11. Addresses that are not physical, devices, and swap

**User addresses** (stage 6, 7). Where memory fits below 256 MB, user
mode's addresses are a window from 0x10000000, 256 MB of them (UWIN), and
none is physical. An allocator of its own hands them out a run of pages
at a time (`mmu_valloc`), so a region keeps its user address while its
memory moves. Every task's pages are at their own user addresses in the
task's space, and at the same ones in **the kernel's map** (`mmu_kdir`,
UDIR), which maps every user address there is: kernel mode, which runs
untranslated, reaches a user address through it (libjs's user window,
`c4m.js kpa`) as readily as its own. The loop's inline loads and stores
stop at the window; `ld32` and the rest, `cstring` and the block opcodes
take it. So a pointer a task passes in a syscall is one the kernel follows
as it always did, and nothing in its syscalls changed; and an address the
map does not have is a bug in the kernel, which halts the machine there
("libjs: the kernel used user address ..., which nothing is at").

The kernel's own handles on user memory stay physical addresses -- what
the loader writes an image through, what `ufree` frees -- and what it hands
user mode is the user address of its memory (`mmu_uva`): the loader
relocates a user task's image to them (`loader_uva`), and so its entry,
constructors, destructors, libraries' data and slots; argv and the
environment's pointers; the forged frame, which `mmu_adopt` rewrites; the
entry page; a vfork child's stack; what `umem_alloc` gives. A region is
found by either (`mmu_rgn`). Memory larger than 256 MB keeps the identity
map, and no swap.

**Devices** (stage 5). Every device that reads or writes guest memory at
an address it was given goes through `arena.dma`, `dmaRead`, `dmaWrite` or
`dmaCstring`: the display's event ring, framebuffer and BLIT8 sources, the
sound card's samples, the drives' buffers, names and directory lists, the
UART's and the console tap's buffers. While a user task's load or store is
at a device, the task's MMU is the arena's `dmaCtx`, and the range must be
the task's to read (and write), one run of memory, or the task takes the
page fault it would have taken itself, with FK_DMA in FINFO:

    c4ix: task 14 (wild.c4r): page fault: a device's write to 0x00002000 (the kernel's image, not mapped) at pc ...; killed

-- an IOMMU's check. Given by the kernel, an address is found the way the
kernel finds it (the window). The display's ring keeps the space that
attached it, for the frames the host drains on its own. A page a device
has used is **pinned**: the host sets PTE_PIN (0x200) in the kernel's
map, and swap leaves it alone, so the framebuffer and the rings never
move under the devices that hold them. (Until stage 11 a page the kernel
had used through the window was pinned too; now its loads and stores look
the page up each time, below.)

**Swap** (stages 7-10). User memory is frames (stage 9): the pages of
heap blocks, each block taken for one region and its frames that
region's until swap puts a page out; then the frame is free for any page
coming back, and a block's record counts its frames in use wherever they
are mapped; a block with none in use goes back to the heap (`mmu_fput`).
When the heap has no block for a region, or there is no frame for a page
coming back, pages of idle tasks go out to **the host's swap** (stage 8,
devices.js `SWAP_ADDR`, `SWAP_CMD`: a page to a numbered slot, up to 256
MB, memory beyond the arena, forgotten with the machine), and their
frames are free. A page put out stays in the kernel's map, not present,
its slot in the entry (`PTE_SWAP`, 0x400): libjs's window reads and
writes the host's copy there, so the kernel finds the page's bytes even
if it follows a pointer into it that it never checked.

Which go (stage 11, below; until then the largest idle block of a task
no CPU was running, never the asking task's): pages of what tasks asked
for (`umem_alloc`, not their images, stacks or arguments), not pinned,
chosen by a clock. A task that touches a page that is out takes a page
fault, and `mmu_fault` brings it back into any free frame, with up to
fifteen after it that are out too, at the same user address, and returns
to the instruction, which runs again. A syscall given a pointer into one
brings it back as it checks the pointer (`uaccess`, `ustrlen`). On SMP,
allocation takes the kernel lock when memory is short (`mmu_short`: seven
eighths of the heap used, or a page gone out since), since swap runs
only under it.

**Memory on demand** (stage 11). Until stage 11 a program could not have
more than the heap had in one piece: a block was a heap block, and swap
could only make room by putting out *other* tasks' blocks. At 45 MB with
the MMU on, Rise of the Triad's 8.3 MB zone failed with 10 MB free (the
largest run 5.3 MB) and nothing idle to put out; one big program alone
has nothing to swap against. Now:

- **A block a task asks for has no memory** (`mmu_lazy`). It is user
  addresses, and for each page a slot of swap's that the host has never
  written, which the host gives back as a page of zeros (devices.js
  `swapOp`: IN of a slot it has no copy of fills the frame with zeros; a
  kernel store through the window makes the copy). So a page's first
  touch is a page fault like any other, and comes "back in" to a frame of
  its own, from wherever one is, sixteen pages at a time. No block needs
  one run of memory, and a task may ask for more than the machine has --
  up to swap's 256 MB, the window's size -- and holds what it uses.
- **The frames** of those pages come from heap blocks of 256 frames (a
  MB; sixteen where the heap has no MB in one piece), while the heap
  keeps a sixteenth for the kernel's own (`mmu_roomy`); past that, from
  pages put out. Any frame serves any page, so a block's free frames are
  as good as a new block's. The blocks come from the heap's **top**
  (`SWAP_CMD` op 5, heap.js `setHigh`: the highest free range that fits,
  or a second bump pointer down from the end), and the kernel's own
  blocks from its bottom, as they always did: the two grow toward each
  other. Taken from anywhere, sixty-four KB at a time, the frames of the
  root archive's unpack left the heap 323 holes between the RAM files,
  none over 2.5 MB, and ROTT's image could not be read in; from the top,
  the same unpack leaves one free run of 17.5 MB. Each frame knows the
  user address it holds (`mmu_fva`).
- **The clock** (`mmu_clock`) is a hand that goes round the window's
  pages. A page used since the hand last passed (PTE_A, set as the MMU
  fills a translation) has its bit cleared and its translation forgotten,
  so that a use sets it again, and is passed over; the first page not
  used since goes. Any task's -- an idle one's, one another CPU is running
  (which forgets it first, by IPI), and **the asking task's own**: one
  program bigger than memory pages against itself. Never the sixteen
  pages a fault is bringing in, nor a page a device has used. A page
  comes in marked used: the instruction that wanted it has not run yet,
  and one that wants two pages would otherwise put the first out to bring
  the second in, for ever. The pages after the one a fault wants come in
  with it only into frames that are free (`mmu_fget`'s EVICT): when memory
  is short, a page a program may never touch is not worth one it will.
- **A block opcode goes on from its fault** (libjs mmu.js `block`). MSET,
  MCMP and MCPY in user mode run a page at a time, and a page that is out
  stops them part way. Run again once the page is in, the same instruction
  -- the same CPU, space, pc, stack and operands -- goes on from the byte
  it stopped at rather than its first. A game's `memset` of its 9 MB zone
  is one MSET: from the first byte each time, it would need every page of
  the zone in at once, and with fewer frames than that it brought each in
  only to put out one it needed again. Doom at 36 MB went on like that for
  95 billion cycles.
- **The kernel's touches no longer pin.** libjs's window (c4m.js `kpa`)
  looks a page up at every load and store, and reads and writes the
  host's copy of one that is out, so a page may move between two of the
  kernel's accesses: a `read` into a game's zone no longer keeps it in
  memory for good, as it did when every page the kernel had used was
  pinned. A device's use still pins (`arena.dma`): a device may keep
  what it was given, and the display does.
- **A device that wants one run** -- the display's event ring, attached
  by the desktop from user mode -- faults on a block whose pages are
  frames from anywhere (FK_DMA). The block is made one run then
  (`mmu_contig`): a heap block of its size, each page copied or brought
  into its place at the same user address, and the access runs again.
  The device then pins it. ("its 16 KB at 0x... made one run of memory,
  for a device" in the kernel's log.)
- **One run of memory**, when the kernel needs one -- an image as the
  loader reads it, a region for an image or a stack (`mmu_raw`), a big
  buffer (`mmu_kalloc`) -- is made by freeing frame blocks whole
  (`mmu_reclaim`): of the blocks whose pages can all go, the one with the
  fewest in use, its pages moved into free frames of other blocks
  (`mmu_move`, a copy and no swap at all) or put out where there are
  none, and the heap is asked again, until it has the run or nothing more
  can go.

Faults are the same fault, whatever brought the page out (swap, or never
having been touched), and the same path brings it in; a page fault that
finds no frame even after the clock has gone round twice kills its task
("its memory ... is on swap, and there is no room for it"). The log says
what went out a MB of one block at a time, and what came back in when
the block changes: a program bigger than memory puts pages out at every
fault, and the log is 64 KB. A page's first touch is not news, and is not
logged (PTE_ZERO, 0x800, marks a page whose slot was never written).

Swap's registers are its own, so a program that drives the drives itself
(`SYS_IOPERM`, persist) and swap never come between each other's stores,
and nothing is left on a drive.

`libjs/tests/test-c4ix-swap.mjs`: one task alone takes half as much
again as the whole machine (72 MB of a 48 MB one), fills it and finds
every word, its own pages out and back; then as one 72 MB block, set by
one `memset` and copied by one `memcpy`, every word again; then, with the heap's free
memory read from its registers, two holders take a third of it each and
sleep; a hog takes six tenths, which only swap can give it; while a
holder's page is out, the kernel reads it
from the host's copy word for word; pages come back sixteen at most at a
time; all three find every word as they left it; and the host keeps none
of their pages once they are gone. With CPUS=2 the holders spin, and the
log has pages going from a task another CPU was running, which forgot
them first.

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

**Stage 1** (one address space, every page the user's), the same
sessions, the plain loop and then `--mmu`:

| workload | without the MMU | with it | |
|---|---|---|---|
| boot (`wait 4`) | 6.32 s | 7.57 s | +20% |
| cc builds and runs mandel.c | 10.02 s | 13.19 s | +32% |
| tar the root archive into /tmp | 12.63 s | 14.31 s | +13% |
| Doom demo3 | 54.32 s | 72.00 s | +33% |
| Wolf3D demo 1 | 28.65 s | 37.06 s | +29% |
| ROTT demo 4 | 22.67 s | 27.64 s | +22% |
| boot to the desktop (bench-desktop.mjs) | 6.41 s | 8.40 s | +31% |

**Final, stage 4** (f0d52ab: an address space per task, guard pages,
read-only text, no-execute data, the IOPM), `bench-mmu.sh` and
`bench-mmu.sh --mmu`:

| workload | without the MMU: session, M instr/s | with it: session, M instr/s | the workload alone, without / with |
|---|---|---|---|
| boot (`wait 4`) | 6.24 s, 216.5 | 7.95 s, 170.5 (+27%) | |
| cc builds and runs mandel.c | 9.94 s, 219.7 | 13.10 s, 167.7 (+32%) | 3.70 s / 5.15 s |
| tar the root archive into /tmp | 12.48 s, 214.3 | 15.55 s, 173.3 (+25%) | 6.24 s / 7.60 s |
| Doom demo3 (2,173 frames, checksum 01914fe7 both ways) | 53.85 s, 236.8 | 76.80 s, 165.9 (+43%) | 45.6 fps / 31.6 fps |
| Wolf3D demo 1 (1,214 frames) | 28.41 s, 212.3 | 36.70 s, 164.4 (+29%) | 54.8 fps / 42.2 fps |
| ROTT demo 4 (609 frames) | 22.34 s, 216.8 | 27.50 s, 176.3 (+23%) | 37.8 fps / 31.2 fps |
| boot to the desktop (bench-desktop.mjs) | 6.56 s, 230.9 | 8.67 s, 176.1 (+32%) | |

Without the MMU nothing moved: the same sessions run at the baseline's
rate (216.5 against 216.1 M instr/s for the boot, 236.8 against 237.9
for Doom), within the noise of a machine other sessions share. The
cycles are up 1-3%, which is the kernel's log and the boot's report
doing their work, not the interpreter. The plain loop is the one that
ran before, chosen once per `run()`.

With it, on helios, the sessions cost a fifth to two fifths more. Doom
pays most. Part of that is stage 3: with one address space (stage 1) it
took 72.0 s, against stage 4's 76.8 s, since every switch to another task
and back loads another page directory and empties the TLB (section 9).
The code window covering a whole program's text (stage 4's libjs change)
took Doom from 78.3 s to 76.1 s on the stage-2 kernel.

**In the browser** (`libjs/tests/web-c4ix-mmu.mjs`, over CDP in the
Windows machine's Chrome 158, against the page served from helios,
`?system=c4ix&persist=0&fast`, the better of two boots each, timed from
the page's load to the desktop saying it runs on the display): 11.8 s
without the MMU, 11.6 s with it, at about 100 M instr/s either way. The
page's boot is mostly not the interpreter's work -- the disk read lazily
over the network, the display, the worker's round trips -- and V8 on that
machine pays 3-5% for the checks (below), so the MMU does not show there.


**Final, stage 7** (42c00f7, with ix-kmem merged: a space per task, user
addresses that are not physical, devices checked, swap on drive 1), the
same sessions:

| workload | without the MMU: session, M instr/s | with it: session, M instr/s | the workload alone, without / with |
|---|---|---|---|
| boot (`wait 4`) | 6.56 s, 207.6 | 8.30 s, 165.3 (+27%) | |
| cc builds and runs mandel.c | 10.39 s, 211.3 | 13.90 s, 159.7 (+34%) | 3.83 s / 5.60 s |
| tar the root archive into /tmp | 13.04 s, 207.0 | 16.40 s, 166.3 (+26%) | 6.48 s / 8.10 s |
| Doom demo3 (2,173 frames) | 57.28 s, 222.8 | 78.97 s, 161.6 (+38%) | 42.8 fps / 30.7 fps |
| Wolf3D demo 1 (1,214 frames) | 28.73 s, 210.4 | 38.14 s, 158.7 (+33%) | 54.8 fps / 40.7 fps |
| ROTT demo 4 (609 frames) | 22.67 s, 214.1 | 29.70 s, 164.0 (+31%) | 37.8 fps / 28.5 fps |
| boot to the desktop (bench-desktop.mjs) | 7.57 s, 201.9 | 9.32 s, 165.9 (+23%) | |

Without the MMU these are 1-6% slower than stage 4's, and the boot to the
desktop 15%: the merge brought in ix-kmem's boot-stack guard, one compare
more on each of the plain loop's inlined loads and stores (its commit
measured 7% on the boot); the desktop's more is not looked into. None of
the MMU's code runs without one. With it, the sessions take 2-5% longer
than stage 4's with it: user addresses come
from their own allocator now, so a window's offset is no longer 0 and the
identity loop (section 12, below) never runs, and kernel mode reaches a
user address through the kernel's map, a walk each time.

In the browser, the same check as before with this build: 10.1 s to the
desktop without the MMU, 11.3 s with it (Chrome 158 on the Windows
machine, the better of two each). C4IX's page now fits the MMU unless
`?mmu=0`.

**Where the time goes** (after stage 7, Doom's demo3 under the MMU,
12.7 billion instructions): 8 loads of a space, 8,611 TLB fills, 2.9
million code-window refills, 1.6 million opcodes run in `sys()`. None of
these is the cost: it is each instruction of the user loop, a table
lookup on every load and store through a pointer, and the windows'
offsets, which the identity loop (below) no longer saves since user
addresses are not physical. What is left to try: user addresses chosen
at their memory's address plus the window's base wherever that is free,
so that every window has the same offset and a loop with it constant
could run (the identity loop gave 5%); a window for the heap as there is
one for the stack. Neither is done. So the MMU is fitted where it catches
bugs -- every test (test-libjs.sh's `cli.js --mmu`, `createMachine`'s
default, which the node tests use, and the browser checks' `?mmu=1`) --
and is off by default for use: `cli.js` without `--mmu`, and the page
without `?mmu=1`, wherever it is served from (2026-10-30; until then the
tree's page fitted one, and only GitHub Pages' build wrote
`libjs/web/site.js` with it off).

**Stage 11** (memory on demand, 2026-10-31, c4ix-swap). All with the MMU
(`cli.js -s --mmu -m MB`), on helios, a game's timedemo after `wait 4`;
the cycles are the whole session's. With a 45 MB machine the games need
one thing that is not swap's: the vfs reads a WAD into one run of kernel
memory (`vfs_fill`, as big as the file, rounded up to a power of two),
which the heap that user frames fill has no room for. These were measured
with that run made exact and taken through `mmu_kalloc` -- a two-line
change to vfs.c, the root-files work's file, which is where it belongs --
and each game in a machine of its own:

| game | 64 MB, before (9f81a64) | 64 MB, stage 11 | 45 MB, stage 11 | frames, checksum (all three) |
|---|---|---|---|---|
| Doom, demo3 | 80.5 s, 12,788,020,551 | 81.1 s, 12,807,173,080 | 82.0 s, 12,807,173,080 | 2,173, 01914fe7 |
| ROTT, demo 4 | 31.1 s, 4,891,776,473 | 31.0 s, 4,912,219,205 | 30.7 s, 4,914,557,838 | 609, 3b083b33 |
| Wolf3D, demo 0 | 39.8 s, 6,078,138,575 | 39.5 s, 6,100,713,599 | 39.3 s, 6,100,713,599 | 1,214, d7358845 |

What memory on demand costs is its first touches: 0.15-0.4% more
instructions, the page faults that back a block as it is used. At 45 MB
the games run the instructions they run at 64 (ROTT 0.05% more: ten
runs of pages out and back): the zones fit once the image, stack and
zone are no longer each a heap block of their own. Before, at 45 MB, ROTT
did not start (its 8.3 MB zone in one block, of a heap with 10 MB free in
pieces of 5.3 MB at most) and Doom read an empty WAD.

In the page (Chrome 151 over CDP, `web-c4ix-swap.mjs`, `?mmu=1&fast`, the
desktop running), Doom's demo3 at 45 MB took 92.8 s against 92.5 s at 64
MB, the same 2,173 frames and checksum. ROTT in a 45 MB page did not:
with the desktop's memory taken too, its 6.4 MB WAD found no run of
kernel memory even with every user page out -- the root archive's RAM
files are the rest of the heap (section 13). With the root files read
from drive 1 instead (the root-files work, lazy-root 78889dc, put
together with this for the test, `?persist` on), it did: ROTT's 609
frames in 27.8 s against 28.1 s at 64 MB, checksum 3b083b33, and Doom's
in 93.1 s against 93.2 s.

**Two Dooms at once** in a 45 MB machine, each `doom -timedemo demo3` in
the background: both ran their 2,173 frames to checksum 01914fe7, at 9.5
fps each against 19.1 for one alone -- the CPU shared, and nothing more
lost to paging -- with 91 runs of pages going out and none killed. With
the root files on drive 1 as well, which leaves 3.2 MB of the heap in use
after boot rather than 24, **two Dooms ran in a 32 MB machine** at 9.5 fps
each, and **four** at 4.7 fps each: 2,173 frames and checksum 01914fe7
every one, 29 runs of pages out, none killed. Four zones of 6 MB, four
images and the WAD are more than 32 MB; what the four use at once is
not.

**Smaller machines.** C4IX with the MMU booted in 44 MB at least before
(the root archive's unpack, user memory and RAM files at once, peaked at
40 MB); now it boots in 32, its unpacker's memory paged, but in 32 the
unpack cannot make every RAM file it should, and some are missing. Doom at
36 MB now sets up its 6 MB zone (which, a block bigger than the frames
free, went round for 95 billion cycles before `block` went on from its
faults), and stops at its WAD: the kernel's RAM files, not user memory,
leave no 4 MB run. Both are what the root files read from the drive
instead of kept in RAM take away.

### What translation cost, and what was tried

The workload for this is the boot with the root archive unpacked
(`wait 4`): 1.2 billion instructions, nearly all of them `persist` and
libarc inflating and writing 600 files, in user mode. Times are the whole
session's, so everything the kernel does is in them too.

The user loop as first written compared against a window on every fetch
and every stack access, and cost 60-80% (on a Windows machine first,
node 24, where the same session without an MMU took 4.5-4.8 s):

| user loop | session | |
|---|---|---|
| without an MMU | 4.5-4.8 s | |
| a compare on every fetch and every push, pop and frame word | 7.7-8.5 s | |
| ... the fetch's compare taken out (unsafe, to see) | 6.0 s | |
| ... and the stack's too (unsafe) | 4.8 s | the compares were the cost |
| ... the windows' offsets taken out instead (unsafe) | 7.5 s | the adds were not |
| the code window by countdown: a jump comes back to look | 7.0 s | |
| the stack window by countdown too, and no `ip` register | 5.3-5.4 s | |

The `ip` register (the instruction's first word, kept for faults) alone
was 0.4 s: a slow path works it out from pc instead.

On helios (Ryzen 5 3600, node 26) the same session costs more, and the
rest of the work was measured there:

| user loop, stage-1 kernel | session |
|---|---|
| without an MMU | 6.1-6.3 s |
| countdowns, a jump forcing a look at the top of the loop | 8.3-8.7 s |
| a jump, ENT and ADJ looking at the windows themselves | 7.4-7.7 s |
| the identity loop (the offsets' adds gone where they are 0) | 7.3-7.6 s |

Taking out one kind of check at a time (unsafe, to see) moved nothing
past the noise: the frame words' compares, the arrays for pointers, the
windows' offsets (0.2-0.3 s), the jumps' look, user mode's `memcpy`
going a page at a time. All of the checks at once (still unsafe) gave
6.1-6.25 s, and the plain interpreter's loop run for user mode with the
MMU on, translating nothing, 5.5 s -- faster than without an MMU at all,
for a reason not looked into (with the MMU on, the kernel's traps from
user mode land on one trap stack rather than on each task's). So
on helios about a second is the checks, each a little, and half a
second is the user loop's shape: its locals, its countdowns, its bigger
body. On the Windows machine the same final loop costs 3-5%: how much a
compare costs depends on the machine and the V8 that runs it.

## 13. Left undone

- **Every task's addresses are apart**, from one window of 256 MB,
  rather than each task having the same ones (0x00400000 on Linux). A
  user address is unique in the machine, which is what keeps the
  kernel's map one map: the kernel follows any task's pointer, a vfork
  child's or a borrowed canvas's, without asking whose it is. The same
  addresses in every task would make each such access name its space,
  and a wrong one would read another task's memory where it now halts.
  The window is all tasks' together: 256 MB of user memory at once,
  swap included.
- **A device that needs one run of memory** -- the display's event ring,
  and the framebuffer's fast path -- is given a page that has been out
  and come back elsewhere only if it is next to the rest. Such memory is
  pinned once a device has used it, so it does not move after; before,
  the ring is refused (the task's fault) and the framebuffer copied a
  page at a time.
- **The light path on SMP** (`sched_light`: malloc and free without the
  kernel lock) cannot swap. It is off while memory is short; a light
  allocation that finds the heap full in between fails as without swap.
  A big block made there has no memory to find (stage 11), so only the
  slabs' chunks can fail that way. Its frees take the frame and slot
  locks (`mmu_flock`), but a light free of a block while another CPU's
  clock is putting a page of that same block out is not excluded: the
  first page out makes memory short, which ends the light path, but one
  already in it is not waited for.
- **Images, stacks and arguments stay in.** The kernel keeps physical
  handles on them (what the loader wrote through, `mod_is_data`, the
  stack's measure in C4KE's task table), which a page moved under would
  make wrong without a word. A program's code and data are a fixed cost
  of each copy that runs (Doom's about 1.5 MB).
- **The RAM files are the kernel's, and stay in.** A file the vfs holds
  -- the root archive's, Doom's WAD once it is read -- is kernel memory,
  one run of it. `mmu_kalloc` can make that run by putting user pages
  out; making the files themselves leave memory is the vfs's own work.
- **C4KE programs** may write their own code, a page at a time.
- **c4bb's device registers** are for a task that asks (`SYS_IOPERM`),
  and only `persist` asks: nothing else in the tree uses them from user
  mode.
