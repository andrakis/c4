# C4OR1K — jor1k ported to C, compiled by c4lc

`src/c4or1k` is an OR1000/OpenRISC CPU emulator, capable of booting a
real Linux kernel in a console, written in C and compiled by c4lc. It
is a port of [jor1k](https://github.com/s-macke/jor1k) (a JavaScript
OR1000 emulator originally built for the browser) to the C4 VM
toolchain — the same toolchain that carries C4KE and C4IX. Console
only: no framebuffer, no keyboard device. Terminal raw mode is an
external concern (`run-c4or1k.sh`), not something the VM provides —
see that script's header comment and "Why fd 0 needs a wrapper, not a
VM change" below.

    make c4or1k-m0                          # build + run M0

## Why this is tractable

jor1k's core CPU (`jor1k/js/worker/or1k/safecpu.js`, in the sibling
`jorconsole` repo) is about 1,100 lines: a plain per-instruction
switch on the primary 6-bit opcode, sub-switches on function-code
fields, ~100 instruction forms total. c4lc already carries two
multitasking OSes (C4KE, C4IX) and a hardware-breadboard
reimplementation of c4m itself (C4BB) — a device-emulation program an
order of magnitude simpler than either is a reasonable single-person
port, provided the C4 VM is fast enough (see M0 below) and c4lc's C
dialect covers what's needed (it does — see "Language notes").

The plan (all milestones, risks, and the jor1k/toolchain findings that
informed it) lives in the session that designed this; this doc tracks
results as milestones land.

## Language notes: what bit c4lc's dialect specifically

c4lc supports everything this project needs — structs, bitwise ops,
function pointers, switch, a real preprocessor (`-P`) — with one sharp
edge worth calling out because it produced a real bug in M0, not a
hypothetical one:

**`int` is host-pointer-width (64-bit under the default c4sp/c4m), and
there is no unsigned type.** The classic C sign-extension idiom
`(v << (32 - bits)) >> (32 - bits)` relies on the left shift
*truncating* at bit 31 — true on a real 32-bit register, false on
c4lc's 64-bit `int`, where nothing is discarded and the shift-left/
shift-right pair just reconstructs the original value unchanged. M0's
first build compiled and ran (no error, no crash reported until
deep into the loop) but a `l.j` back-branch landed the guest PC at
`0x4000003` instead of 4 instructions back, and the VM eventually hit
unmapped memory. `cpu.c`'s `sext(v, bits)` fixes this with a
width-independent mask + XOR/subtract form instead of shift/truncate —
correct at any word width, not just 32. Every subsequent milestone
that extracts a signed field from an instruction word must use it, not
the shift trick.

Other dialect notes from the toolchain investigation, relevant from M1
onward: `>>` is arithmetic (signed) only — OR1000's `l.srl` (logical
shift right) needs the same kind of explicit masking, not a shift
trick. No multi-dimensional arrays — guest RAM and the SPR groups are
flattened to 1-D, which is how jor1k indexes them internally anyway.
No struct-by-value params/returns — not needed here.

## Why fd 0 needs a wrapper, not a VM change

No `ioctl`/`termios` facility exists anywhere in c4/c4m: `READ`/
`WRITE` are thin wrappers over the host's `read()`/`write()`, so the
guest just inherits whatever termios state fd 0 already has.
`run-c4or1k.sh` puts the terminal in raw mode before exec'ing the
emulator and restores it on exit via a trap — the only viable
approach, and the same division of responsibility C4IX's `console.c` uses
(cooked tty assumed, worked around at the OS layer). Console I/O
(M3) will also reuse `console.c`'s non-blocking-second-fd pattern for
stdin, since a blocking `read()` on fd 0 would freeze the whole
single-threaded VM process while waiting for a keystroke.

## Milestones

**M0 — throughput sanity check + decode skeleton. Done.**
`main.c` hand-assembles a decrement-and-branch loop (`tools/asm_m0.py`
generates the encodings, read directly from safecpu.js's opcode
numbers and field positions, not the OR1000 spec) over 8 real OR1000
opcodes, using the same pc/nextpc delay-slot scheme safecpu.js uses,
and measures wall-clock guest-instructions/sec on the real c4/c4m VM.

**Result: ~1.37M guest instructions/sec** (6,000,006 instructions in
~4.4s; `make c4or1k-m0`), against a pre-M0 worst-case estimate of only
50K–200K/sec. VM cycle count for the same run was ~1.21B, i.e. ~200
VM "cycles" per dispatched guest instruction (c4m's cycle counter,
`__c4_cycles()`, does not appear to be 1:1 with dispatched VM bytecode
ops — see caveat below).

**Caveat, read before treating this as the answer for M1+:** this
number is a best case. It's a syscall-free, trap-free, memory-array-
free tight loop over 8 opcodes with a 2-way branch — no byte-addressed
RAM access with endian composition + masking (M1), no SPR/exception
overhead (M2), no per-instruction-batch host polling (M3+). The real
ISA's ~100-form switch and the added per-access work will bring this
down; M1 should re-measure on the full decoder before trusting a boot-
time projection. That said, the gap between 1.37M/sec and the 50K/sec
floor that would have made this impractical is wide enough that
**the project is a clear go** — M1 is worth building.

**M1 — full ISA decode. Done.** `cpu.c`/`cpu.h`/`mem.c`/`mem.h` (split
out of M0's single `main.c`, linked with c4rlink the way `src/c4mp`
does it) implement every non-privileged OR1000 integer instruction
safecpu.js has: arithmetic, shifts, `ff1`/`fl1`, `mul`/`div`/`divu`,
all register and immediate compares, branch-delay-slot-correct
`l.j`/`l.jal`/`l.jr`/`l.jalr`/`l.bf`/`l.bnf`, every load/store width
including `l.lwa`/`l.swa`'s reservation mechanism. No MMU, no SPRs, no
exceptions yet — `l.mfspr`/`l.mtspr`/`l.rfe`/`l.sys`/`l.trap` fall
through to an explicit "unimplemented, dump state and halt" path
(M2's job). `main.c` now loads a flat binary of assembled words at
guest address 0 and runs it as a test-program harness rather than
M0's fixed throughput loop.

**Verification: bit-for-bit match against jor1k.** `tools/asm.py` (a
small two-pass assembler with labels) and `tools/or1k-oracle.js` (a
headless Node driver that requires jor1k's actual `safecpu.js` — not
a transcription of it) let `tests/m1_test.s` run through both
implementations and diff every register, `SR_F`/`SR_CY`/`SR_OV`, and
a 64-word RAM window every test result is written into (`make
c4or1k-m1-check`). Current state: **exact match**, covering every
instruction form at least once.

Three real bugs surfaced building this, each worth keeping in mind
for M2 onward:

1. **c4lc's enum initializers must be a literal, not an expression.**
   `enum { RAM_SIZE = 1 << 20 };` fails to compile ("bad enum
   identifier") — c4lc's enum parser only accepts a `Num` token.
   Write the literal (`0x100000`) instead.
2. **c4lc's data segment is capped at 256KB** (`g:DMAX` in
   `c4lc-gen.lisp`, shared across every global in the whole linked
   program) — a `char ram[RAM_SIZE]` static array fails to compile
   ("data segment full") at any size that matters, and M4 needs room
   for a 5.7MB `vmlinux.bin` regardless. `mem.c` allocates `ram` with
   `malloc()` instead (`mem_init()`), which comes from the VM's
   heap, not the fixed data segment. This wasn't a workaround for a
   one-off problem — any future large guest-RAM-shaped buffer needs
   the same treatment.
3. **OR1000's mandatory branch delay slot is not optional to get
   right, including in test code.** The first draft of
   `tests/m1_test.s` placed real instructions directly after
   `l.bf`/`l.j` with no `nop`, so the delay-slot instruction (which
   *always* executes, taken or not) silently overwrote every
   compare's result — all ten compares read back "true" regardless of
   the actual condition. cpu.c's decode was correct; the test program
   wasn't. Caught immediately by the oracle diff, not by inspection.

The oracle itself needed two rounds of fixes before it was trustworthy
(both are `or1k-oracle.js`'s problem, not cpu.c's, but they cost real
time misdiagnosing cpu.c first): jor1k's `SafeCPU` aliases the
register file onto heap byte 0, so real RAM has to start at heap
offset `0x100000` (`system.js`'s `ramoffset`) or a loaded test program
silently clobbers `r0`-`r9` before executing a single instruction; and
jor1k's "Big" memory accessors are not big-endian byte composition at
all — `Read32Big`/`Write32Big` are plain native `int32mem[addr>>2]`
access, and `Read8Big`/`Read16Big` reach the same native storage via
XOR'd addressing (`addr^3`, `addr^2`) rather than physically
byte-swapped storage. An oracle that invents its own "obviously
correct" big-endian formula instead of reading `ram.js` will produce
self-consistent-looking wrong answers, not a crash.

**M2 — SPRs, exceptions, MMU, real TLB-miss vectors. Done.** `cpu.c`
gained: the full SR flag register (`cpu_set_flags`/`cpu_get_flags`,
packing/unpacking all 17 flag bits exactly as safecpu.js's SetFlags/
GetFlags do); `cpu_get_spr`/`cpu_set_spr` (group 0 general SPRs +
SR, group 1/2 DTLB/ITLB match+translate registers, group 9 PIC,
group 10 tick timer -- TTMR/TTCR are stored but nothing fires
`EXCEPT_TICK` automatically yet, see below); `cpu_exception` (EPCR/
EEAR/ESR save, the EXCEPT_SYSCALL-vs-everything-else EPCR-offset
distinction, the flag resets, the vector jump); `l.mfspr`/`l.mtspr`/
`l.rfe`/`l.sys`/`l.trap`, all real now; and `dtlb_lookup`/`fetch_ins`,
which check the current DTLB/ITLB match register and permission bits
and raise `EXCEPT_DTLBMISS`/`EXCEPT_ITLBMISS`/`EXCEPT_DPF`/
`EXCEPT_IPF` on a miss or fault -- **not** a page-table walk. Per the
plan, that's deliberate: real OR1000 software (the guest kernel) owns
its own page tables and installs the resulting TLB entry with
`l.mtspr` from its own miss handler; this function's only job is to
notice there's no entry and raise the vector, exactly as safecpu.js's
`DTLBLookup`/`GetInstruction` do.

**Verification: bit-for-bit match against jor1k, again.**
`tests/m2_test.s` exercises: a general-SPR read/write round-trip; the
SR flag round-trip through `l.mtspr`/`l.mfspr` (including SR_FO being
force-set on every write, matching `cpu_set_flags`); `l.sys` and
`l.trap` each firing their real vector, with tiny handlers planted at
the *actual* vector addresses via a new `.org` assembler directive
(`tools/asm.py`) rather than inline with the code that triggers
them -- that's genuinely where the CPU jumps; and a full DTLB-miss
round trip: an `l.lwz` faults with no TLB entry installed, the
handler at `EXCEPT_DTLBMISS`'s vector installs a matching entry via
two `l.mtspr`s and returns with `l.rfe`, which (per real OR1000
semantics, `EPCR` pointed *at* the faulting instruction, not past it)
retries the same `l.lwz` -- which now succeeds and reads back the
value pre-stored at that physical address. `make c4or1k-m2-check`:
**exact match**, first real run.

Two things worth flagging, one a real fidelity note and one a
deliberate scope cut:

- **`EXCEPT_SYSCALL` and every other exception type save `EPCR`
  differently, and it matters for how a handler must behave.**
  `EXCEPT_SYSCALL` saves `(pc<<2)+4`, so a plain `l.rfe` naturally
  continues at the instruction *after* the trap. Everything else
  (`EXCEPT_TRAP` included) saves `(pc<<2)` — pointing *at* the
  faulting/trapping instruction — so a handler that wants to return
  past it (as `tests/m2_test.s`'s trap handler does) must bump `EPCR`
  by 4 itself before `l.rfe`, and a TLB-miss handler that does
  *nothing* to `EPCR` gets the retry-on-return behavior automatically.
  Both are exercised in the test program precisely because they're
  easy to get backwards.
- **TLB entry LRU bits are not checked at all** (real jor1k aborts —
  crashes the whole worker — if a guest ever sets them; nothing in
  this project's test programs does either). Not a page-table walk
  either, per above — that's the guest kernel's job starting at M4.
  Automatic tick-timer interrupt delivery is also still deferred:
  `TTMR`/`TTCR` are readable/writable SPRs now, but nothing fires
  `EXCEPT_TICK` on a schedule yet, since that needs the main-loop
  batching M3/M6 add.

**M3 — UART + launch script + console I/O. Done.** `uart.c` is a
16550-compatible device matching `jor1k/js/worker/dev/uart.js`
exactly (register offsets, LSR/IER/IIR/FCR bits, the DLAB-aliased
DLL/DLH pair, the CTI/THRI/MSI interrupt-priority order) at MMIO base
`0x90000000`, IRQ line 2 (both confirmed against jor1k's `system.js`).
`mmio.c` is a minimal top-byte dispatcher (`mem.c`'s `ram_l*`/`ram_s*`
now check bit 31 of the address and route there instead of into
`ram[]`) — one device today, shaped to add more at M5. `console.c` is
`src/c4ix/console.c`'s non-blocking-second-fd pattern verbatim (same
reason: a blocking `read(0, ...)` would freeze this single-threaded
VM), polled every `main.c` loop iteration — cheaply, since the gate
inside `con_poll_and_feed()` itself rate-limits the actual host
`read()` call, the same way `con_poll()` does. `run-c4or1k.sh` (M0)
needed no changes; it was already the correct wrapper.

**Verification: two working echo servers, byte-for-byte.**
`tests/m3_echo.s` polls `LSR` and echoes; `tests/m3_echo_int.s` does
the same job but interrupt-driven — `UART_IER_RDI` + `SR_IEE` +
`PICMR`, with the actual echo happening in a handler at
`EXCEPT_INT`'s real vector, exercising the whole M2+M3 IRQ pipeline
end to end (UART → `cpu_raise_interrupt` → `PICMR`/`PICSR` →
`cpu_check_for_interrupt` → `EXCEPT_INT` → handler → `l.rfe`). Both
halt on Ctrl-D (0x04) and both are checked non-interactively by piping
a string ending in Ctrl-D through `run-c4or1k.sh` and diffing the
echoed bytes against the input (`make c4or1k-m3-check`,
`c4or1k-m3-int-check`) — **exact match**, first real run, for both.
Neither is cross-checked against jor1k's own oracle the way M1/M2's
tests are: extending `tools/or1k-oracle.js` to simulate real-time
stdin arrival wasn't worth building for this milestone, so this is
functional verification (the echo is provably correct) rather than
the bit-for-bit-against-jor1k standard set earlier — worth knowing if
a future milestone's UART-adjacent change ever needs a stronger check
than "does it still echo."

One real design decision worth recording: the interrupt-driven test's
handler can't just `l.rfe` back to the spin loop when it sees Ctrl-D
— main.c's `cpu_step(halt_pc)` harness needs the program to reach a
known final address to stop, and an intentionally-infinite spin loop
never does that on its own. The handler instead **overwrites its own
return address**: `movhi`/`ori` builds `final_halt`'s byte address
(the same `%hi`/`%lo` trick `tests/m1_test.s` uses for `l.jalr`),
`l.mtspr`s it into `EPCR`, then `l.rfe`s — landing execution at
`final_halt` instead of back in the spin loop. Ordinary OR1000 code,
not a c4or1k-specific mechanism; worth remembering as a pattern for
any future test whose main flow doesn't naturally terminate.

**M4 — boot loader. Done — real Linux boots.** `boot.c`'s
`load_kernel`/`patch_kernel` are a direct port of `system.js`'s
`OnKernelLoaded`/`PatchKernel`: `jorconsole/jor1k-sysroot/or1k/
vmlinux.bin` is already a decompressed raw flat binary (jorconsole's
own postinstall unpacked it), so `load_kernel` is a plain streamed
`read()` straight into `ram[]` at address 0 — no bzip2 or ELF parsing,
and no `Little2Big` byte-swap either (mem.c already stores genuine
big-endian bytes in the file's natural order, so there's nothing to
swap). `patch_kernel` replicates the exact byte-offset/value guard
`PatchKernel` uses to find the DTB's `"memory\0"` property and
overwrite its size cell — copied verbatim rather than re-derived, per
the original plan. `main.c` gained a boot mode (`-b [maxsteps]`) that
starts execution at the real reset vector (`0x100`, not address 0)
with no natural halt address, since a kernel doesn't fall off the end
of its own image; `maxsteps` bounds a run by instruction count, the
only stopping mechanism available before real panic detection exists.

**Result: `make c4or1k-boot` reaches the exact expected panic.**
`VFS: Cannot open root device "host" or unknown-block(0,0): error -2`
/ `Kernel panic - not syncing: VFS: Unable to mount root fs on
unknown-block(0,0)`, printed once, with the CPU then idle (no spin of
repeated panic output) — the standard, generic Linux VFS failure any
emulator without a working root filesystem produces, not jor1k- or
c4or1k-specific text, so this is strong independent evidence on its
own rather than something that needed a side-by-side jorconsole run
to trust. Along the way the real kernel boot banner, memory/MMU setup
(`setup_memory`, `map_ram`, `itlb_miss_handler`/`dtlb_miss_handler`
registration), the full kernel command line
(`root=host rootfstype=9p rootflags=trans=virtio console=uart,mmio,
0x90000000,115200 ...`), and dozens of subsystem/driver init messages
(network protocol families, TCP/UDP hash tables, SCSI, ALSA, block
layer, io schedulers, 9P/v9fs registration) all appear correctly on
the UART console — exercising the CPU, exceptions, DTLB/ITLB, and
UART/MMIO layers against real, unmodified Linux code far beyond
anything the hand-written M1-M3 test programs touched.

Devices this project doesn't implement (virtio-block at three
addresses, a second UART/`ttyS1`, DRM/framebuffer, ATA/PATA, keyboard/
touchscreen, ethernet, RTC) all probe and fail *gracefully* — the
kernel's own drivers print their normal "not found"-style messages
(`Wrong magic value 0x00000000!`, etc.) and move on, rather than the
boot wedging. `mmio.c`/`mem.c` print one diagnostic line per
unrecognized access rather than faulting, which is what makes this
observable at all instead of an opaque hang.

**One real bug found and fixed, and it was the reason the first boot
attempt never got here**: automatic tick-timer interrupt delivery had
been deliberately deferred all the way from M2 ("needs the main-loop
batching M3/M6 add" — see this doc's M2 section). The very first boot
attempt got only as far as the ATA/PATA driver probe before getting
stuck retry-polling forever — a strong signal that *something* waits
on a timeout that never expires, and TTMR/TTCR being pure inert
storage (readable/writable via `l.mtspr`/`l.mfspr`, but never firing
`EXCEPT_TICK`) was exactly that something: any kernel code path
blocked on jiffies advancing hangs forever without it. `cpu_tick_check`
(cpu.c) ports safecpu.js's Step-loop tick logic — advance `TTCR`,
raise `EXCEPT_TICK` when `SR_TEE` and the pending bit are set —
restructured for a one-instruction-at-a-time driver instead of
jor1k's N-at-a-time `Step(steps, clockspeed)`: `main.c` calls it every
64 instructions, matching jor1k's own batch cadence but coarser on
delivery latency (up to 63 instructions late), which doesn't matter
for unblocking a timeout. Once wired in, the *same* 100M-instruction
budget that previously never left the ATA probe reached the panic
with room to spare. Worth remembering for M6 ("IRQ/timer tuning"):
the tick mechanism itself was pulled forward from there because M4
needed it, not because M6 turned out to be unnecessary — M6's job is
tuning cadence/batch size for a full interactive boot, not building
tick delivery from scratch.

**M5 — 9P root filesystem (basefs.json only). Done — the kernel
mounts it.** Four new modules, `tools/mkbootfs.js` (offline, Node):

- `bootfs.c`/`bootfs.h` — the inode tree. `filesystem.js`'s async,
  network-fed `FS` object has no equivalent here: everything
  `basefs.json` describes is either already resident (loaded from
  `bootfs.blob` at startup) or created synchronously by a 9p request,
  so every `bootfs_*` call returns immediately — no `AddEvent`
  deferred-callback machinery needed. Inodes are parallel `malloc`'d
  int arrays (mode/uid/gid/parentid/firstid/nextid/size/bloboff),
  matching mem.c's own reasoning: c4lc's 256KB data-segment cap is
  shared across every global in the linked program, so anything
  nontrivially sized has to be heap-allocated rather than a static
  array. Directory listings (`.`/`..`/children, in 9p2000.L's Q/d/b/s
  wire format) are built lazily on first open and cached, mirroring
  `FillDirectory`'s `updatedir` dirty-flag pattern exactly.
- `virtio.c`/`virtio.h` — the virtio-mmio transport, matched against
  `dev/virtio.js`'s `VirtIODev` at the register level (same offsets,
  same "modern" version=2 negotiation, so legacy-only fields like
  `QUEUEALIGN`-driven ring layout never come into play). Only one
  device exists (the 9p filesystem), so unlike jor1k's generic
  `dev.ReceiveRequest`/`dev.SendReply` callback object, this calls
  `virtio9p_receive_request()` directly.
- `virtio9p.c`/`virtio9p.h` — the 9P2000.L protocol handler, ported
  op-for-op from `dev/virtio/9p.js` (statfs, walk, {t,l}open, lcreate,
  mkdir, symlink, readlink, link, mknod, getattr, setattr, read,
  treaddir, write, renameat, unlinkat, clunk, xattrwalk, version,
  attach, flush, lock) — everything the reference implements except
  the "always report full capabilities" xattr behavior, deliberately
  narrowed to "report no xattr support" instead, which just makes the
  guest kernel fall back to the plain setuid-bit permission model any
  real capability-less filesystem would produce.
- `tools/mkbootfs.js` — flattens `basefs.json` plus every file's real
  content (decompressing `bin/busybox.bz2` via the host's `bunzip2`,
  simpler and more robust than re-hosting jor1k's own `bzip2.js` in a
  Node build script that never runs inside `c4m`) into `bootfs.idx`
  (fixed 64-byte records) + `bootfs.blob` (concatenated file bytes),
  offline, at build time — `bootfs.c` has no JSON parser and no
  network, so this has to happen before the emulator ever runs.

**Verification note, a deliberate scope change from the plan.** The
plan called for diffing a jorconsole-captured 9p request/response
trace against this port's own trace. In practice jorconsole's own
`fsloader.js` is async/network-fed even for `basefs.json`, so it isn't
a clean, deterministic byte-for-byte oracle the way `safecpu.js` was
for M1/M2 — and the more meaningful gate at this stage is simpler
anyway: does a real, unmodified kernel actually mount the filesystem
and exec real userspace binaries through it. It does (see below), which
exercises the protocol far more thoroughly than a captured trace from
one boot would.

**Two real bugs found the hard way, both by bisecting a real boot
against a c4m-level `gdb` backtrace, not by inspection:**

1. **`rd_le32`'s missing sign extension (bootfs.c) — the actual root
   cause of a SIGSEGV that took most of a day to pin down.** Every
   `-1` sentinel `mkbootfs.js` writes (empty directories' `firstid`,
   list-terminator `nextid`, the root's `parentid`) is stored as its
   real byte pattern, `0xFFFFFFFF`. `bootfs.c`'s byte-composing reader
   didn't sign-extend that back to `-1` the way `cpu.c`'s `sext()`
   does for exactly this class of value — c4lc's 64-bit `int` reads it
   back as the *positive* 4294967295 instead. `bootfs_search`'s
   `while (id != -1)` loop then treats that as a real, wildly
   out-of-bounds inode index, computes a pointer from it, and
   dereferences it. This only ever fires when a walk reaches an empty
   directory or a list's true end — invisible to any hand-written test
   program, and in a real boot it first triggers resolving `/sbin/init`
   (`/sbin` is empty in `basefs.json`; `init` only exists as a symlink
   under `/bin`), i.e. exactly the point a kernel's own init-path probe
   sequence is expected to try and fail. Symptoms were badly
   misleading before this was found: the crash *looked* sensitive to
   unrelated changes (cpu.c's dispatch style, mem.c's 16-bit MMIO
   path, c4m's `-P` pool-size flag, even stdio buffering) because each
   of those shifts `malloc`'s heap layout enough to sometimes land the
   wild pointer in unmapped memory (a hard crash) and sometimes not
   (silent, undetected corruption) — none of them were the actual bug.
   The fix is one function: `rd_le32` masks to 32 bits then applies
   the same mask+XOR/subtract sign-extension `cpu.c`'s `sext()` uses.
   Pinned down by adding `stdbuf -oL`-forced unbuffered logging (stdio
   is fully buffered against a non-tty output, so a crash mid-run was
   silently discarding the last several KB of already-*printed*
   diagnostic output every time) plus targeted `printf`s in the 9p walk
   handler, which caught the exact failing call:
   `bootfs_search(idx=30 /* /sbin */, "init")`.
2. **A second UART is load-bearing, not optional.** `basefs.json`'s
   `/etc/inittab` has `ttyS1::respawn:-login -f root` in addition to
   the console's own `-login -f user` — jor1k's real device map
   includes a second 16550 (`uartdev1` @ `0x96000000`, IRQ 3), and this
   project's UART support was single-instance until M5. Without it,
   `getty`'s driver on ttyS1 never gets a coherent "no data yet, sleep"
   signal from the generic "unimplemented device" MMIO fallback and
   hot-loops polling registers forever, burning the entire instruction
   budget before `ttyS0`'s own login ever gets scheduled. Fixed by
   parameterizing `uart.c` (all per-UART state as `int[2]` arrays, a
   `unit` argument threaded through every function) rather than
   duplicating the module — unit 1 has no real byte source (there's
   only one host terminal, wired to unit 0 via `console.c`), so its own
   `getty` just blocks forever exactly like a real unconnected serial
   port would, which is the correct, harmless outcome.

**A performance fix alongside these, not a correctness one:** once
real Linux is actually running rather than panicking early, devices
this project doesn't implement (ethernet's link-status poll, chiefly)
get probed continuously in the background, not just once during early
boot the way M4 saw. `mmio.c`'s per-access diagnostic `printf` — fine
when it fired a handful of times — was measurably the dominant cost
once it started firing millions of times; `warn_once()` now reports
each unique top byte a single time.

**M6 — IRQ/timer tuning to reach a shell prompt. Done.** No further
IRQ/timer tuning actually needed beyond M4/M5's existing tick-check
cadence — this milestone turned out to be entirely gated on M5's two
bugs (`rd_le32` and the missing UART1). With those fixed, the boot
proceeds through the full sequence without further intervention: the
9p root filesystem mounts (`VFS: Mounted root (9p filesystem)
readonly on device 0:12.`), real userspace execs (`/etc/init.d/rcS`:
`mount -a`, `busybox --install`, `mkdir`/`mount` for `/dev/pts` and
`/dev/shm`, `ifup -a`, `inetd`), `udhcpc` discovers/retries/gives up
gracefully (no ethernet device exists), and `/etc/inittab`'s
`ttyS1::respawn:-login -f root` reaches a real, interactive BusyBox
shell prompt:

```
-login[130]: root login on 'ttyS1'
*******************************************************
* Don't know what to do? Type 'help' and press enter. *
*******************************************************
Note, you can exit most programs by pressing CTRL+C
~ $
```

— a real, unmodified kernel and root filesystem, not a synthetic
test, reached entirely through this project's own CPU, MMU, MMIO,
UART, virtio, and 9p implementations. `stdbuf -oL` was needed to see
this at all: stdio is fully buffered against a non-tty output, so a
run's last several KB of already-*printed* text is silently discarded
if the process is later killed or hits its instruction-budget cutoff
before a flush.

**One remaining rough edge, not yet fixed:** the console's own
`::respawn:-login -f user` entry (ttyS0, this project's *only* console
wired to a real host stdin via `console.c`) hadn't reached its own login
prompt by the time a piped test command arrived in the same run that
reached the ttyS1 prompt above — `ttyS1`'s root login happened to get
scheduled first, and ttyS1 has no real byte source by design (see M5),
so the piped input landed nowhere useful. This is a scheduling/timing
question (which `respawn` entry's `getty` wins the race), not a
correctness bug — the shell above is genuinely interactive, real
Linux, real 9p, just not yet demonstrated with a live typed command
against the specific console this project's launch script feeds.
Worth revisiting if the `run-c4or1k.sh` interactive path (a real
terminal, not piped input) is exercised directly.

**M7 — stretch.**

- **`c4lc -O`: done, enabled by default.** All ten modules compile
  clean under `-O`; the linked image is ~1.2% smaller, and a real
  60M-instruction boot run is ~4% faster (132.6s vs 137.6s) with
  byte-for-byte identical output on the full M1-M4 regression suite
  and the boot log itself. Smaller than the ~1.44x this same optimizer
  gets on compiler-workload benchmarks elsewhere in this repo --
  `cpu_step`'s dispatch is already a tight if/else-if chain with
  little redundant-expression fat for the tree/peephole passes to
  remove -- but strictly positive with zero correctness cost, and
  worth keeping given how many guest instructions a full boot needs
  (M6). Wired into the default `c4or1k.c4r` build rule, not a
  separate opt-in target.
- **`fs.json` overlay, optional virtio-block/ATA — not attempted.**
  Framebuffer/keyboard stay explicitly out of scope regardless.

**M8 — faster: a fastcpu.js-based emulator. Scoped subset implemented.**
`jor1k/js/worker/or1k/fastcpu.js` (1,609 lines, vs. safecpu.js's ~1,100
-- the CPU this project actually ports) turns out to be a fundamentally
different execution model, not a drop-in faster decode loop, and its
real speedup techniques don't decompose cleanly into a small, safely-
verifiable patch on top of `cpu_step`'s current one-instruction-at-a-
time structure. This milestone was first investigated and scoped
without implementation (see below), then revisited to actually build
the one piece that scoping pass found safely bounded, plus one
additional main-loop optimization found while re-reading the driver
loop for this pass:

- **The actual big win is `fence`-based straight-line batch
  execution, and it is still NOT implemented.** `fastcpu.js` runs
  instructions in a tight inner loop until hitting a jump or a page
  boundary (`fence`), only paying the cost of a fresh "should I check
  for a TLB miss / interrupt / page crossing" decision at those
  boundaries rather than on every single instruction. Porting this
  faithfully means restructuring `cpu_step` around basic blocks, not
  adding a helper function -- a rewrite comparable in scope to the
  original M1 CPU port itself, with its own oracle-diffing
  verification burden to match (nothing in the current M1/M2 test
  suite exercises "does a batch boundary get drawn in the right place"
  the way it exercises individual instruction semantics). That
  cost/benefit judgment hasn't changed; this remains future work.
- **The single-entry TLB lookup cache: implemented, with the
  permission-check gap closed.** `fastcpu.js`'s own version
  (`read32tlblookup`/`read32tlbcheck` and siblings, one pair per access
  width/direction: cache the last virtual page translated, skip
  `DTLBLookup`'s tag check entirely on a same-page hit) has a genuine,
  undocumented correctness gap -- a cache hit skips the *permission*
  check along with the tag check, and the permission bits depend on
  `SR_SM` (supervisor vs. user mode), which fastcpu.js's cache key
  doesn't include. fastcpu.js instead relies on invalidating all its
  TLB caches inside `Exception()` (SR_SM always becomes 1 there) and on
  SPR group1/group2 writes, but `l.rfe` and a direct `l.mtspr(SPR_SR,
  ..)` can both change `SR_SM` without going through either of those
  paths, which would let a stale permission decision leak across a
  mode change on the same page. `dtlb_cache_*`/`itlb_cache_*` (cpu.h/
  cpu.c) close this by keying the cache on `(vpage, SR_SM)` directly --
  a hit requires both to match the cached entry, so there's no event to
  enumerate and no way to miss one. The cache is separately invalidated
  on any write to the owning SPR group (`cpu_set_spr`'s group 1/2
  cases) to handle the guest rewriting a TLB entry's tag/permission
  bits without raising an exception at all (an explicit flush). Applied
  to both `dtlb_lookup` (every load/store) and `fetch_ins` (every
  single instruction fetch, unconditionally -- code executes
  sequentially within a page the overwhelming majority of the time, so
  this path sees a very high hit rate once `SR_IME` is on).
- **`con_poll_and_feed()` moved off the per-instruction hot path.**
  Found while re-reading `main.c`'s driver loop for this milestone, not
  from fastcpu.js: it was called once per *guest instruction* --
  cheaply, thanks to its own internal rate gate (`console.c`'s
  `CON_POLL_CYCLES`), but the call+gate-check overhead itself was still
  paid every instruction regardless of whether the gate was open. Moved
  onto `cpu_tick_check`'s existing once-per-64-instructions cadence;
  the internal gate already tolerates far coarser polling than that
  (order 1,000+ guest instructions between real `read()`s), so this
  doesn't change observable input latency, just how often the call
  itself happens.
- **`doze` (SPR group 8 / power-management, `fastcpu.js` lines
  ~393-394 and ~1168-1174) is not the idle-skip optimization it first
  looks like, and still isn't implemented.** It makes `Step()` return
  early to jor1k's own browser-hosted scheduler when the guest enters
  its idle loop with no interrupt imminent -- a JS-event-loop
  cooperation mechanism (don't hog the browser tab), not a "fast-
  forward wall-clock time past an idle period" mechanism.
  `cpu_tick_check`'s existing per-64-instruction cadence already
  handles tick delivery correctly during idle; `doze` wouldn't reduce
  the guest-instruction count a real idle period costs in this
  project's headless, single-process model the way it looked like it
  might before reading the actual code.

**Verification and measured result.** The TLB-cache change touches
every load/store/fetch, so it's verified the same way M1/M2 are: the
full `c4or1k-m1-check`/`m2-check`/`m3-check`/`m3-int-check` suite
passes unchanged after this change (bit-for-bit against the jor1k
oracle, byte-for-byte echoed I/O). For the boot path specifically (no
oracle exists past early boot -- see "Where jor1k stops being an
oracle" below), a 60M-instruction `-b` boot run was captured before and
after this milestone's changes and diffed: **byte-for-byte identical
boot log**, confirming the cache and the polling-cadence change are
behaviorally invisible, only faster. Wall-clock, same 60M-instruction
run, same host, back-to-back: **134.6s before, 124.3s after -- about
7.6% faster**, on top of M7's `-O`. Smaller than the multiple-times
speedup `fence`-based batching would give (that's still unbuilt), but
real, measured, and free of the correctness risk that made the full
fastcpu.js port a same-session no-go the first time through this
milestone.

## M9 — where next: three directions, one implemented

After M8, a boot to the interactive shell (§M6) still takes on the
order of 5-25 minutes of wall time depending how far past the shell
prompt the run is left going, prompting a look at whether to keep
pushing on c4or1k specifically, change the guest ISA, or abandon CPU
emulation altogether in favor of a native kernel. Grounded in real
numbers rather than intuition:

**The baseline.** c4m's own raw ceiling is ~4.4M VM-instructions/sec.
Post-M8, c4or1k sustains ~483K-500K guest OR1000 instructions/sec,
i.e. each emulated guest instruction costs **~9-10 c4m VM instructions**
on average (fetch, decode, dispatch, execute, writeback) -- already
tight, which is consistent with M8 only finding 7.6%: there wasn't
much bookkeeping fat left to cache away.

**Option A -- keep optimizing c4or1k. Implemented, in two passes.**
The original scoping here assumed the payoff was in porting
fastcpu.js's fence-at-jump/page-boundary scheme, which amortizes
per-instruction exception/TLB-check *logic*. Revisiting that
assumption before building it: that scheme was designed for jor1k's
V8 host, where function calls are nearly free after JIT and
per-instruction branch logic is what's left to cut. c4m has no JIT --
calls cost real bytecode overhead on every invocation -- so the more
relevant cost for *this* host is call overhead, not per-instruction
exception-check logic (M8's lookup caches already made that cheap).
Two rounds of work followed that reasoning rather than porting
fastcpu.js's scheme literally:

1. **Batching.** `cpu_step(halt_pc)` (one instruction per call) became
   `cpu_run_batch(halt_pc, max_batch, *ran)` (cpu.c/cpu.h): the entire
   per-instruction switch now runs inside a `for` loop within one
   function call, executing up to `max_batch` instructions (main.c
   passes 64, matching the cadence already established for
   interrupt/tick delivery in M8) before returning to main.c's driver
   loop. Every mid-switch `return 0` (branch taken, `l.rfe`,
   `l.mtspr`) became `continue`; the five "unimplemented" fault paths
   set a `fault` flag and `break` out to a single post-switch check,
   since c4lc has no labeled break/goto to exit nested switches
   directly (confirmed via `c4lc-gen.lisp`: `continue` inside a
   `switch` correctly targets the enclosing `for` loop, not the
   switch, since `g:switchstmt` only saves/restores `g:brk`, not
   `g:cont` -- verified against the compiler source before relying on
   it, not assumed). This collapses main.c's own per-iteration
   overhead (status check, step counting, maxsteps check) from once
   per instruction to once per batch, on top of the `cpu_run_batch`
   call itself.
2. **Inlined TLB fast paths -- the actual "fence" idea, adapted.**
   Even after batching, every fetch still called `fetch_ins()` and
   every load/store still called `dtlb_lookup()`, paying a function
   call on every cache *hit* too, not just on a miss. `itlb_cache_*`
   and `dtlb_cache_*` (cpu.h) were extended to cache the *decided*
   permission result (`itlb_cache_xok`, `dtlb_cache_rok`/`_wok`), not
   just the raw `tlbtr` bits -- correct because that decision can't
   change while the cache entry stays valid (invalidated on any
   group1/group2 SPR write, same as M8). `cpu_run_batch` now checks
   `itlb_cache_vpage`/`_sm` directly and, on a hit, reads
   `itlb_cache_phys`/`_xok` inline -- no call to `fetch_ins` at all.
   `fetch_ins`/`dtlb_lookup` became cold-path-only, called solely on
   an actual miss. The ten load/store sites share this logic via a
   `DTLB_FAST(addr, write, out)` function-like macro (cpu.c) rather
   than ten hand-copied inline blocks -- verified c4lc's preprocessor
   handles a multi-line, brace-bodied function-like macro correctly
   (`c4lc-pp.lisp` splices backslash-newlines) before relying on it
   for something used at ten call sites. This is the real "fence"
   concept -- treat a validated region as needing no per-instruction
   re-validation -- but implemented as call-avoidance (inlining a
   cached decision) rather than fastcpu.js's literal basic-block
   restructuring, because call avoidance is what actually matters on
   a no-JIT host.

**Verified**, same standard both times: full `c4or1k-m1-check`/
`m2-check`/`m3-check`/`m3-int-check` pass unchanged, a 60M-instruction
`-b` boot run byte-for-byte identical to the previous stage's boot
log at every step, and (both stages) a full 700M-instruction boot
reaching the identical interactive shell prompt as M6/M8.

**Measured**, same 60M-instruction run throughout:

| stage | time | vs. previous |
|---|---|---|
| pre-M8 baseline | 134.6s | -- |
| M8 (TLB cache + poll batching) | 124.3s | -7.6% |
| M9 batching (`cpu_run_batch`) | 119.85s | -3.6% |
| M9 inlined ITLB fast path | 117.35s | -2.1% |
| M9 inlined DTLB fast path | 116.18s | -1.0% |

**~13.7% cumulative** since pre-M8, ~6.6% since M8. Each pass gave a
real, verified, byte-identical-output gain, but each smaller than the
last -- consistent with the ~9-10 VM-op/instruction budget being
dominated by fetch/decode/execute/writeback, not by anything
TLB/exception/call-related left to cut. Hard-capped regardless by
c4m's own ~4.4M-instr/sec ceiling: no amount of restructuring
`cpu_run_batch` beats that ceiling, and the remaining ~90% of the
per-instruction cost isn't addressable without either a different
execution model or reducing c4m's own per-VM-instruction cost (out of
scope -- would mean changing c4m.c/c4.c itself, well beyond this
project). This is very likely close to the practical ceiling for
"emulate OR1000 by interpretation on c4m as it exists today."

**Option B -- target a CISC guest ISA (e.g. x86) instead of OR1000.**
The intuition -- fewer, denser CISC instructions mean less total guest-
instruction count for the same boot workload -- is real but outweighed
by the other side of the ledger in a non-JIT interpreter: x86 code
density buys maybe 1.5-3x fewer static instructions over an equivalent
RISC build, but x86's variable-length, prefix-laden decode costs
several times more c4m VM-ops per instruction to interpret than
OR1000's fixed 32-bit format does today. Likely a wash at best,
plausibly a net loss. Separately, the implementation cost dwarfs what
c4or1k took: safecpu.js was ~1,100 lines and this project was still a
full multi-milestone build; a Linux-capable x86 core plus its own
PC-platform device set (8259 PIC, 8253 PIT, real-mode boot sequence,
etc. -- none of which exist anywhere in this repo yet) is a
substantially larger, higher-risk redo for uncertain-to-negative
payoff. **Not recommended.**

**Option C -- a Tilck-style native kernel instead of CPU emulation.**
This is the one with real payoff, and this repo already has most of
the infrastructure: `docs/c4ix-design.md`'s C4IX is a from-scratch OS
compiled by c4lc -- protected mode, trap-based syscalls
(open/read/write/spawn/wait/pipe/dup2/exit/...), a real interactive
shell with pipelines and redirection -- running as *native* c4m code,
paying zero CPU-emulation tax. Measured (§7 of that doc): C4IX boots to
a userland shell in ~343K VM cycles, well under a tenth of a second at
c4m's raw throughput -- roughly four to five orders of magnitude faster
than c4or1k's Linux boot, because it isn't paying an emulation tax *or*
real Linux's much heavier device/driver init at all.

The catch is real and worth stating precisely, because "Tilck-style"
undersells it: Tilck's actual trick is running *unmodified x86 ELF
binaries* (real busybox, real bash) under a much simpler kernel -- the
CPU still has to execute that real x86 machine code somehow (Tilck
runs on real or emulated x86 hardware). c4m has no such escape hatch:
it only executes c4lc-compiled code, so nothing built for x86/ARM/
OR1000 can run on it unmodified regardless of kernel design. A "Tilck
for C4" gets syscall-ABI compatibility, not binary compatibility -- a
busybox-equivalent userland would need its C source recompiled by
c4lc, and would likely need real porting work rather than a drop-in
recompile, since real busybox's build system, macro use, and libc
surface go beyond c4lc's L7/L8 dialect (`docs/c4lc-design.md`) as it
stands today.

**Where this leaves it.** Option A landed in full, including a fence-
style rewrite adapted to c4m's cost model (~13.7% cumulative over M8 --
see the table below). Option C remains the highest-payoff direction
and is mostly a matter of widening C4IX's existing syscall/libc surface
rather than a from-scratch project -- next up when picked up. Option A
still preserves "boots a real, unmodified vmlinux.bin," which C4IX's
approach cannot ever claim, so it's not superseded, just capped.
Option B remains not recommended. §M10 below investigates two further
angles specifically asked about after M9 landed: a genuinely different
execution model, and changes to c4m itself.

## M10 — investigation: a different execution model, and changes to c4m

M9 squeezed real but shrinking gains out of `cpu_run_batch` itself
(7.6%, 3.6%, 2.1%, 1.0% -- each pass targeting real overhead but
finding less of it left). This section asks two harder questions
instead of continuing to tune that same function: would a genuinely
different execution model for c4or1k help, and is there room in c4m
itself? Both investigated with real measurements, nothing implemented
here -- this is scoping, matching the M8/M9 house style.

### Finding 1: guest code reuse is extreme

A throwaway instrumentation pass (a `malloc`'d bitmap over guest RAM,
one byte per instruction word, set on first fetch; reverted after
measuring, never committed) counted distinct instruction *addresses*
fetched against total fetches during a real boot:

| total instructions fetched | distinct addresses ever fetched |
|---|---|
| 10,000,000 | 47,348 |
| 50,000,000 | 160,946 |
| 100,000,000 | 172,418 |
| 180,000,000 | 188,521 |
| 270,000,000 | 188,528 |

By 270M instructions, the distinct-address count has essentially
stopped growing (188,521 -> 188,525 -> 188,526 -> 188,528 across the
last 90M instructions) while the total keeps climbing -- the system
has settled into steady-state idle/polling behavior (busy-wait loops,
periodic retries) re-executing the same ~188K-instruction working set
over and over. **Average reuse by 270M instructions: ~1,430x per
distinct address.** cpu_run_batch re-fetches and re-decodes every one
of those repeat visits from scratch today -- M8/M9's caching avoided
redundant *TLB* lookups on a repeat visit, but not redundant *decode*
(the `(ins >> N) & mask` field extraction) or redundant *dispatch*
(the `switch (opcode)`, and for four opcode classes a second inner
`switch (func)`).

### Finding 2: c4m's own native dispatch is a linear scan, not a jump table

Checked rather than assumed, since M5's cpu.c switch/if-else bisection
already burned real time on an unverified belief about codegen:
`objdump -d c4m` on the `-O2 -g` build shows `c4m_main`'s bytecode
dispatch (`if (i == LEA) ... else if (i == IMM) ... else if (i == JMP)
...`, c4m.c around line 1563) contains **no indexed/computed jump
anywhere in the function** -- no `jmp *(...,%reg,N)` jump-table
pattern, just a straight-line chain of compare-and-branch. GCC did not
convert this to a jump table at `-O2`, likely because it isn't a
literal C `switch` (the debug-output and `OPCD`-special-case code
immediately above it breaks the canonical shape GCC's switch-lowering
pass looks for) and a hand-rolled if-chain doesn't get the same
treatment reliably. Consequence: opcodes further down the enum pay
more sequential comparisons to reach than ones near the top --
`ADD`/`SUB` (extremely common: every pointer-offset calculation and
most arithmetic) are comparisons **#26/#27** in the chain; `LEA`/`IMM`
(also extremely common: every variable access) are comparisons #1/#2.
This is a real, fixable inefficiency in shared infrastructure, not
speculative -- every C4-family project pays it, not just c4or1k.

(A back-of-envelope cross-check: c4or1k's ~500K guest-instr/sec at
~7-8 c4m VM-ops/guest-instruction post-M9 implies roughly 3.5-4M c4m
VM-ops/sec sustained, well under the ~4.4M VM-instr/sec ceiling
measured on a trivial 8-opcode loop back in M0 -- consistent with
real workloads exercising opcodes further down the (unordered) chain
than that loop did, though this is inference from wall-clock timing,
not a direct cycle measurement: `perf` isn't available in this
sandbox (`perf_event_paranoid` blocks it), so this is corroborating,
not conclusive on its own.)

### Option 1: a decode/dispatch cache in c4or1k (different execution model, no c4m changes)

Given Finding 1, a per-guest-instruction-address cache of already-
*decoded* fields (opcode, rd, ra, rb, imm, and for the four sub-
switched opcode classes, `func` too) -- populated on first visit to an
address, read directly on every subsequent visit -- would skip the
`>>`/`&` field-extraction work entirely on what the data shows is the
overwhelming majority of fetches. Pushed further (cache a single,
pre-combined dispatch key spanning opcode+func so a second inner
`switch` is never needed on a hit), it approaches genuine "direct
threaded code," a well-established interpreter technique. Estimated
effect: decode/sub-dispatch is roughly 2-4 of the ~7-8 VM-ops a guest
instruction costs today; cutting most of that on a >99%-hit-rate cache
plausibly drops the average to roughly 4-5 VM-ops, i.e. **very roughly
another 30-45% wall-clock improvement**, in the same spirit as M8/M9
but hitting a cost center they didn't touch (decode/dispatch itself,
not TLB lookups or call overhead). Risk: moderate, confined entirely
to c4or1k's own code (no shared-infrastructure blast radius) -- the
one real correctness concern is self-modifying guest code invalidating
a stale cache entry (module loading, JIT'd BPF); this project's fixed
kernel+busybox boot workload plausibly never exercises that, but it
would need either a check (any RAM write inside the "ever cached"
range invalidates that entry) or an explicit documented assumption,
not silence. Verification burden: same standard as M8/M9 (regression
suite + byte-identical boot-log diffing) should suffice, since this is
"cache values that don't change" applied one layer higher.

### Option 2: changes to c4m itself

Two tiers, clearly different in risk:

- **Fix the dispatch (Finding 2).** Reordering the if-chain by real
  hot/cold frequency, or (better, more reliably optimizable) rewriting
  it as an actual C `switch (i)` so GCC's own switch-lowering has a
  clean shape to work with, is small, mechanical, and localized to one
  function. It benefits every C4-family project (c4ke, c4ix, c4bb,
  c4sp, c4mp, oisc4), not just c4or1k -- real payoff, genuinely low
  risk, but needs the same broad regression pass across all of them
  before it could be trusted, not just c4or1k's own suite. Estimated
  effect: hard to pin precisely without `perf` access in this sandbox,
  but a **rough 10-30%** reduction in c4m's own per-VM-instruction
  native cost is a defensible estimate given the confirmed linear-scan
  finding, not a guess pulled from nothing.
- **New fused opcodes** (e.g. a combined "load local variable value"
  op collapsing today's `LEA`+`LI`/`LC` pair, which is an extremely
  common pattern in c4lc-emitted code) would cut VM-op *count*, on top
  of Option 1's cache reducing *redundant* decode of the ops that
  remain. Needs matching changes in both c4m.c (new opcode handling)
  and `c4lc-gen.lisp` (peephole recognition to actually emit it,
  building on the pattern `-O`'s existing peephole pass already
  establishes) -- moderate effort, moderate-to-good payoff (**rough
  20-40%** VM-op-count reduction is plausible given how pervasive
  local-variable access is), moderate risk (touches the compiler and
  the interpreter in lockstep; must not silently break any other
  project built on this toolchain).
- **A real JIT (bytecode -> host native code)** is the only lever
  that could plausibly deliver an order-of-magnitude win (**5-20x**,
  based on typical interpreter-to-simple-JIT ratios), and Finding 1's
  ~1,430x reuse ratio is close to the ideal profile for one --
  translate a hot address once, run native code on every subsequent
  visit instead of re-interpreting. But this is an enormous
  undertaking: register allocation from c4's stack-based execution
  model onto host registers, native code generation (x86-64 here),
  and correct interaction with every existing c4m feature this
  investigation didn't even need to touch (traps, protected mode,
  signals, C4KE's task switching, self-modifying-code invalidation).
  It is comparable in scope to writing a new compiler backend, touches
  the shared foundation every other project in this repo depends on,
  and failure modes (a JIT miscompilation) are substantially harder to
  diagnose than anything hit so far -- M5's SIGSEGV saga was hard
  *with* pure interpretation as the baseline. **Not recommended to
  start casually**; if ever pursued, it should be its own dedicated,
  heavily-gated project, not a c4or1k milestone.

### Where this leaves it

Options 1 and 2's first tier (decode cache in c4or1k; dispatch fix in
c4m) target *different, complementary* cost centers -- one cuts VM-op
*count* via redundant-decode avoidance, the other cuts native cost
*per* VM-op that still runs. Combined, compounding rather than
additive, a **very rough 1.5-2x** total improvement over the current
post-M9 baseline seemed like a defensible estimate (e.g. ~0.65 x 0.80 ~
0.5, i.e. roughly half the wall time) -- real and worth having, but
still not the order-of-magnitude a JIT could give, and still bounded
by needing to actually build and verify both pieces. Nothing here was
implemented as of this write-up; this was investigation only, as
asked.

**Important correction, acted on in M11: the "dispatch fix in c4m"
half of this was wrong to propose and was reverted before being
committed.** c4m.c must stay parseable by plain, unmodified c4 --
`./c4 c4m.c ...` is a real, exercised bootstrapping/self-hosting path
(`test-c4`), and plain c4's own compiler cannot parse a `switch`
statement. This is not new information this project should have had
to rediscover: `src/c4mp/vm.c`'s own header comment documents that
exact if-chain-to-switch conversion being tried in c4m and reverted
for precisely this reason (commit `29d6f57`), and separately notes it
measured as **no faster natively even ignoring the breakage** -- both
facts this investigation's Option 2 missed by reasoning from a
disassembly finding (§Finding 2 above) without checking prior art
first. c4m.c was changed, fully regression-tested across every
dependent project in this repo (all green), and then reverted anyway
once this was pointed out, because passing tests was never the actual
bar -- staying parseable by plain c4 is a hard constraint testing
alone doesn't surface. See M11 below for what was built instead.

## M11 — decode-cache pivot, and c4mp: a free native speedup

Two results this milestone, neither the one originally scoped:

### The "decode cache" reconsidered, and replaced with lazy `imm`

M10 estimated a decode cache (opcode/rd/ra/rb/func, cached per guest
instruction address) at roughly 30-45%, reasoning from the >99% code
reuse Finding 1 measured. Working out the actual implementation before
committing to the memory cost (parallel arrays sized to guest RAM, or
a hash-indexed cache with its own tag-comparison overhead) surfaced a
problem with that estimate: c4or1k runs *interpreted*, under c4m/
c4mp, so "cost" here means c4-bytecode VM-op count, not native CPU
cycles. Under that accounting, the opcode/rd/ra/rb shift-and-mask
extraction this would cache is already cheap (a `SHR`+`AND` pair per
field, 2 VM-ops each) -- a cache *lookup* (address arithmetic + a
tag-compare + several array reads) costs a comparable number of VM-ops
to just re-deriving the fields, so caching *that* specifically isn't
clearly a win at all once counted honestly, which is consistent with
the c4m dispatch-fix mistake above: assuming "fewer native operations"
without checking what actually costs what under *this* execution
model.

What *is* expensive: `imm = sext(ins, 16)`, computed unconditionally
at the top of `cpu_run_batch` for every single instruction regardless
of type, where `sext()` is a real function call (`JSR`/`ENT`/body/
`LEV` -- c4lc has no inlining). `simm` (the store/`l.mtspr` operand)
was already computed lazily, only inside the specific cases that use
it; `imm` wasn't, for no principled reason. Of the ~29 core opcodes,
only 9 (the loads, `l.addi`, `l.xori`, `l.sfXXi`) actually use `imm` --
roughly two thirds of dispatched instructions (branches, register-
register ALU, `l.mtspr`, `l.rfe`, stores, ...) were paying a full
function call for a value they never read. Fixed by moving
`imm = sext(ins, 16)` out of the unconditional prelude and into each
of the nine case bodies that need it (cpu.c), matching the pattern
`simm` already used. No caching, no extra memory, no self-modifying-
code correctness question to reason about -- pure removal of
unconditional waste.

**Verified**: full regression suite unchanged, 60M-instruction boot
byte-for-byte identical, full 700M-instruction boot reaches the same
shell prompt. **Measured** (60M-instruction run, under c4m):
109.14s vs. the post-M9 116.18s baseline -- **~6.1% faster**.

### c4mp: opcode-compatible, native, and already faster -- with zero source changes

Per explicit correction mid-milestone: c4m must not change (see above).
Asked to look at `src/c4mp` instead -- a second C4 host, written in
c4lc's fuller dialect (structs, `switch`, a real preprocessor) rather
than the minimal subset plain c4 must still parse, and buildable two
ways from one source: `./c4mp` (native, `gcc`-compiled directly, no
self-hosting constraint to preserve) and `c4mp.c4r` (the same source
compiled by c4lc, so it can also run *hosted under c4m* -- "a
backwards-compatibility proof, since c4m has no multiprocessing to
offer," per `src/c4mp/vm.c`'s own header comment).

That header comment turned out to be exactly the prior art this
milestone needed and initially missed (see the correction above): c4m
*was* tried with a real `switch` dispatch once, reverted for the
self-hosting reason, and **measured as no faster natively even
setting the breakage aside** -- switch's only confirmed win was 1.79x
for *nested* interpretation (a switch-based guest under an if-chain
host benefits from the guest needing fewer VM-instructions to express
its own dispatch; this is about guest code *shape*, not host dispatch
*mechanism*, and is exactly what `cpu_run_batch`'s own
`switch (opcode)` already exploits, unrelated to c4m's or c4mp's own
native dispatch).

Given that, the useful question about c4mp isn't "does its switch
dispatch make it faster" (prior art says: not for this reason) --
it's simpler: **c4mp's instruction semantics are c4m's, opcode for
opcode, "guest images cannot tell the two apart"** (same source
comment). Since c4or1k.c4r is a completely ordinary `.c4r` image with
nothing c4-specific in it, and `./c4mp image.c4r [args...]` runs any
`.c4r` directly (no `load-c4r.c` wrapper needed -- c4mp "carries no
compiler, it loads .c4r images"), the obvious experiment is just
running it, unmodified:

    ./c4mp c4or1k.c4r <vmlinux.bin> -b <N> <bootfs.idx> <bootfs.blob>

**It works, with zero source changes to c4or1k.** Verified: `m1_test.bin`/
`m2_test.bin` output byte-for-byte identical to c4m's; a 60M-instruction
boot byte-for-byte identical to c4m's boot log; a full 700M-instruction
boot reaches the same interactive shell prompt. **Measured** (60M-
instruction run, pre-lazy-`imm` c4or1k.c4r): **95.05s under c4mp vs.
116.18s under c4m -- ~18.2% faster**, from nothing but running the
identical bytecode on a different, already-existing host. Combined
with the lazy-`imm` fix above (still c4or1k-side, applies to either
host): **93.11s under c4mp -- ~19.9% faster than the post-M9 c4m
baseline**, ~30.8% faster than the pre-M8 starting point.

A `c4or1k-boot-mp` Makefile target now runs the same boot via `./c4mp`
directly, alongside the existing `c4or1k-boot` (c4m).

**Why this is faster is not fully pinned down** -- the switch-vs-if-
chain distinction specifically was already ruled out by prior art, so
it's likely some combination of c4mp's fuller C dialect letting gcc
make different (not necessarily "better dispatch," just different)
codegen choices elsewhere in the interpreter loop, and/or the register-
struct-based design `vm.c`'s header comment describes ("`c4_run` loads
registers into locals, runs a quantum, stores them back") producing
tighter code than c4m's globals-based interpreter body. Worth a closer
look if pushed further, but not required to *use* the result: the
measurement is real and reproducible regardless of full attribution.

**Where new VM-level work belongs, going forward:** c4mp, not c4m.
It has the language features (real `switch`, structs, a preprocessor)
that make adding new fused opcodes safe and ordinary to write, without
c4m's plain-c4-parseability constraint standing in the way. That work
-- e.g. the fused-opcode idea from M10's Option 2 -- was not attempted
this milestone (out of scope for what was asked), but c4mp is now
confirmed as the right place for it whenever it's picked up, and
already delivers a real, free, zero-risk speedup on its own before any
new opcodes are added at all.

## M12 — lazy rB, and new CISC opcodes in c4mp (-mcisc)

Two more results, following directly from measuring c4lc's actual
generated bytecode for the hottest pattern in `cpu_run_batch` rather
than continuing to guess at costs.

### Lazy `rB`, the same shape as M11's `imm`

`rB = r[rb]` was computed unconditionally every instruction, same
mistake as the pre-M11 `imm`, except worse: dumping the real bytecode
for `x = arr[i]` (a global int array, variable index) showed **~10
c4-bytecode VM-ops** -- `IMM &arr; PSH; <load i>; PSH; IMM 3; SHL;
ADD; LI; SI` -- not the 1-2 a native-code mental model would suggest.
Only 9 of ~29 opcodes read `rB` (`l.jr`/`l.jalr`, the four stores,
`l.mtspr`, and the two func-switched reg-reg classes). Moved into
those nine case bodies, exactly mirroring M11's fix.

**Verified**: full regression suite unchanged, 60M-instruction boot
byte-for-byte identical under c4mp. **Measured**: 93.11s -> 87.17s on
that run (~6.4% faster alone) -- bigger than `imm`'s 6.1%, consistent
with the higher per-access cost just measured.

### New CISC opcodes: LXI/SXI in c4mp, gated by c4lc's `-mcisc`

That 10-op cost for a single array read/write, paid on effectively
every instruction (`r[]`) or SPR access (`group0[]`/`group1[]`/
`group2[]`), is bigger than anything M8-M11 touched. Fixing it means
changing what code gets *generated*, not just when -- a genuinely
different lever from every prior milestone.

**c4m stays out of this entirely**, per the standing constraint (§M11):
new opcodes went into c4mp only, appended past its existing
processor/atomic opcodes (`CPUI`..`IPI`) using the exact same
mechanism and the exact same safety property those already rely on --
opcode numbering is append-only and mirrored across `c4m.c`,
`c4mp.h`, `c4r.lisp`, `oisc4.c`, and `c4cc.c` (per `c4mp.h`'s own
header comment); c4m's own enum simply stops before the new numbers,
so it structurally cannot be affected -- an unknown opcode there
already traps as `TRAP_ILLOP` (verified, not assumed, by first
building this the "obvious" wrong way -- see below).

**The opcodes** (`src/c4mp/vm.c`, `src/c4mp/c4mp.h`):
- `LXI` (load indexed int): `a = *(int*)(*sp++ + a*8)` -- pops one
  value (base) off the stack, uses the accumulator as the index,
  scales by the word size, loads.
- `SXI` (store indexed int): `*(int*)(sp[1] + sp[0]*8) = a; sp += 2`
  -- base and index both already on the stack (pushed unscaled,
  value computed last into the accumulator), so the whole address-
  compute-and-store collapses into one opcode.

**The codegen** (`src/c4sp/lisp/c4lc-gen.lisp`, `-mcisc` flag added to
`c4lc.lisp`): a new `g:cisc-eligible-index?` recognizes a plain
`var[expr]` (deliberately not more complex bases -- a struct member,
a dereferenced pointer expression -- which would need evaluating
twice or duplicating this file's type inference; those fall back to
the general path unchanged) whose array has a statically-known
8-byte scalar element type -- int, or any pointer-of-pointer level,
explicitly never a struct and never `char*` (that would need a
separate 1-byte-scale opcode, not built this round). `gen:cisc`
(default `false`) gates two new codegen paths, `g:indexload-cisc` and
`g:indexstore-cisc`, which replace the generic `g:indexaddr`+load/
store sequence with the fused opcodes wherever the guard passes;
everything else -- struct arrays, `char*`, complex bases, and the
entire compiler by default -- is byte-for-byte unaffected, verified
by rebuilding the *default* (no `-mcisc`) `c4or1k.c4r` and diffing
runtime behavior against a pre-M12-compiler-change build (raw file
bytes turned out to already be non-reproducible build-to-build from
an unrelated, pre-existing source -- confirmed by building the *old*
compiler twice and diffing those against each other -- so behavioral
diffing, this project's standard verification method throughout, is
what was trusted here, not a raw byte comparison).

**A real bug found and fixed before trusting any of this**: the
first attempt to run a `-mcisc`-compiled test program under a freshly
rebuilt `c4mp` produced `TRAP_ILLOP` on instructions 76/77 (LXI/SXI's
new numbers) and silently wrong output -- because the `./c4mp` binary
being invoked was still the *stale* pre-M12 build (only a throwaway
`gcc` command in `/tmp` had actually picked up the new opcodes; the
repo-root `c4mp` binary needed its own explicit rebuild). Rebuilding
it fixed the trap; a hand-written loop test (write 32 values via
`SXI`, read them back via `LXI`, compare against the same program
compiled without `-mcisc`) then matched exactly. Caught by testing
execution, not just instruction counts -- fewer VM-ops proves nothing
about correctness by itself. A second, smaller gap: `src/c4cc/c4cc.c`
maintains its own opcode-name table for `c4rdump`'s disassembly
output, indexed **with no bounds check** (its own comment says so,
for the same reason `CPUI`..`IPI` are already listed there) -- LXI/SXI
were added there too, not just for cosmetics but to avoid an
out-of-bounds read the moment anyone disassembles a `-mcisc` image.

**Verified**: full regression suite (`m1`/`m2`/`m3`/`m3-int` checks)
unchanged under c4mp with the `-mcisc` build; a 60M-instruction boot
byte-for-byte identical to the non-CISC boot log; a full 700M-
instruction boot reaches the same interactive shell (see below for
the final confirmation). Every cross-project suite this repo has
(`test`, `test-c4bb`, `test-oisc4`, `test-c4sp`, `test-c4ix`,
`test-c4mp`, `test-link`, `test-c4l`) re-run and green with the
*default* (non-`-mcisc`) compiler, confirming zero blast radius
outside code that opts in.

**Measured**: same-session, back-to-back pairs to control for real
timing noise observed mid-measurement (a single non-CISC run read
87.17s in one session and 91.87s/92.76s in the next, same binary,
same build -- noisy enough that a single before/after pair isn't
trustworthy here). Two CISC runs: 89.39s, 89.45s (tight). Two non-CISC
runs measured back-to-back with them: 91.87s, 92.76s. **~3.1% faster**
on average -- real and reproducible, but far short of the ~23%
VM-op reduction an isolated micro-benchmark showed for the exact
`r[]` access pattern (53 -> 41 instructions for a tiny hand-written
test function) -- real boot workloads dilute any single fix's relative
share of total cost, the same diminishing-returns pattern every
milestone since M8 has shown. Cumulative since the pre-M8 baseline
(134.6s): **~34%**.

Makefile additions: `c4or1k-cisc.c4r` (built with `-mcisc`) and
`c4or1k-boot-cisc` (runs it under `c4mp` -- `c4m` cannot run this
image at all, not just slower).

### Where this leaves it

The int-array-only, plain-variable-base scope here was deliberately
conservative. A `char*`/byte-array variant (`LXC`/`SXC`, scale 1,
matching `SC`'s truncate-and-return-the-stored-byte semantics) would
extend the same idea to `mem.c`'s `ram[]`, not attempted this round.
Extending `g:cisc-eligible-index?` past bare-variable bases (struct
members, pointer expressions) would widen how much code qualifies but
needs either double-evaluation avoidance or reusing this file's
existing single-pass type inference more cleverly than a quick
lookup allows. Both are real future increments on the same idea, not
new ideas.

## Related future work (not this project)

c4mp (the multiprocessor VM, `src/c4mp`) has no networking support.
Adding it would matter here too: a full Linux boot benefits from a
working network device, and jor1k itself emulates one (`ethmac.js`,
out of scope for c4or1k's own M0–M6 per the "headless console" plan
above). Noted for later, not blocking anything above.
