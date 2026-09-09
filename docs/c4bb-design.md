# C4BB — the c4m breadboard computer

`src/c4bb` is a hardware-style reimplementation of the c4m virtual
machine in the spirit of Ben Eater's breadboard 8-bit computer: a
microcoded 32-bit CPU whose every control line can be watched, simulated
in JavaScript, rendered as an interactive board in the browser — and
capable of booting C4KE and C4IX with full preemptive multitasking,
protected mode, and a keyboard.

    make test-c4bb                          # build everything + verify
    make serve-c4bb                         # then open
    #   http://localhost:8471/src/c4bb/web/index.html
    #   (python3 -m http.server from the REPO ROOT -- app.js fetches
    #    repo-root-relative paths, so serving web/ itself breaks drives)
    node src/c4bb/sim/cli.js -i -d src/c4bb/images/disk \
         src/c4bb/images/c4ix32.c4r         # interactive in a terminal

## Why it works: what the c4m "CPU" actually is

The VM loop at c4m.c:1490 has architectural state of just `pc sp bp a`,
a cycle counter, and a mode bit — no flags, no register file. Everything
else is memory. The extensions that make kernels possible (ITH, the
cycle-interval interrupt, the synthesized trap frame, TLEV) are, in
hardware terms, one long microcode routine plus four latches. That is a
buildable machine.

## Word size and toolchain

The machine is 32-bit: words, pointers and instructions are 4 bytes, and
`.c4r` images are stamped WordBits=32. The 32-bit toolchain is native
`-m32` builds of the existing tools:

| tool | built by | role |
|---|---|---|
| `c4cc32` | `make c4bb-32bit` | c4cc emitting 32-bit images |
| `c4m32` | `make c4bb-32bit` | native 32-bit c4m, the parity baseline |
| `c4sp32` | `make c4bb-32bit` | 32-bit c4sp; running c4lc.lisp on it makes **c4lc a 32-bit compiler** |
| `c4rlink32` | `make c4bb-32bit` | 32-bit linker for C4IX objects |

c4lc's target word size follows the host running it: `g:WORD` in
c4lc-gen.lisp is `(sys:wordsize)` (as is the sizeof fold in c4lc-tree
and the header stamp), so the same Lisp emits byte-identical 64-bit
images under `./c4sp` and 32-bit images under `./c4sp32`. Full
cross-compilation (32-bit output from a 64-bit c4sp) would additionally
need c4r.lisp's `string:word`/`string:word!` builtin calls replaced with
width-parameterized byte-composed accessors — about 30 mechanical sites,
left for another day.

The kernels compile unmodified except for genuine word-size portability
fixes in C4IX (hardcoded `8`/`16`/`24` byte offsets in sched.c, c4ke.c
and loader.c became `sizeof(int)` arithmetic; 64-bit output verified
unchanged by `make test-c4ix`).

## The machine

### Memory map (one flat byte-addressed arena, 32 MB default)

    0x0000-0x000F  null guard
    0x0010         TLEV ROM word (trap handlers' LEV lands here)
    0x0014         loader sentinel (constructor/main return address)
    0x0020-0x003F  firmware vector latches: MALC FREE RALC PRTF STRC
    0x0100-0x01FF  device registers (below)
    0x0200-0x034F  opcode-name ROM (66 x 5 bytes, c4m's layout; OPSL/_OPC)
    0x1000         MEM_BASE: firmware image, program image(s), heap
    top - 4K       argv area; stack grows down from just below it

### Device registers

    0x100 UART_TX      write byte -> terminal
    0x104 UART_RX      read: next keyboard byte or -1
    0x108 UART_RXAVL   read: bytes available
    0x1AC UART_TXADDR  write: buffer address (does not auto-advance)
    0x1B0 UART_TXLEN   write N: emit N bytes from it; read -> N taken
    0x10C TIME_MS      simulated ms = cycle/1000 + usleep credit (1 MHz machine)
    0x110/4 CYCLE_LO/HI
    0x118 USLP_US      write: advance simulated clock
    0x120-0x138 disk controller: NAME (write ptr -> open, read -> fd),
                FD/ADDR/LEN (LEN write triggers DMA, read -> result),
                CLOSE, FLAGS (O_NONBLOCK honored for "/dev/stdin"
                and "/dev/tty")
    0x134 OPNAME       write name ptr, read opcode number (_OPC)
    0x140 POWER        write -> halt with status (EXIT lands here)
    0x144/8 HEAP_BASE/END   set by the loader, read by the firmware
    0x14C INFO         capability bits, mirrors native c4_info()|TRAPH
    0x150-0x174 CPU control latches: TRAPH INTERVAL TRESTORE MODE,
                and the trap-jam latches TT TP HND JMODE JINTERVAL
    0x1B4/8 MBOX_BASE/LEN  read: the mailbox region (0 = not fitted; INFO bit
                C4I_MBOX 0x2000 announces it)
    0x1BC MBOX_BELL    write: guest->host doorbell; read: host->guest
                interrupts pending (read-to-clear)

### The mailbox

A host that boots with `boot(machine, fw, prog, argv, { mbox: bytes })`
gets a region carved out below the stack reserve, `HEAP_END` lowered so
the firmware allocator never reaches it, and `MBOX_BASE/LEN` pointing at
it. Both rings live inside it (host->guest in the first half, guest->host
in the second): `[cap][head][tail][data...]` in i32 words, head and tail
monotonic word counters, a frame `[len][type][seq][payload...]` never
wrapping (a `-1` marker skips to the next multiple of cap). The host
reads and writes the rings straight through the arena typed array; the
guest with loads and stores. A guest write to `MBOX_BELL` reaches the
host's `onDoorbell(value)`; the host's `raiseMbox()` jams a
`HARD_IRQ` with parameter `2` (HIRQ_MBOX) through the cycle handler at
the next boundary -- the PIT's path with its own parameter -- or, for a
guest with no handler, counts up in `MBOX_BELL` until read.
Header for guests: `include/c4bb_mbox.h` (board only, after the INFO check).

`UART_TXADDR`/`UART_TXLEN` are the same shape as the disk write head and
exist for the same reason (`docs/c4bb-uart-block.md`): the firmware
formatter already holds a pointer and a length for every run it emits,
and `PUTS` can serve none of them — it is NUL-terminated and appends a
newline, where a literal run is a *slice* of the format string. Being
registers rather than an opcode, they work at the base-c4 rung, which is
the rung the C4DOS build of `raycast` is pinned to.

fd 0 is the blocking line-buffered keyboard (a cooked tty); opening
`/dev/stdin` gives a byte fd where empty reads return -1 exactly like
Linux O_NONBLOCK — so C4IX's console (src/c4ix/console.c) works unmodified.

Opening **`/dev/tty`** gives the same non-blocking byte fd with the line
discipline switched off: a keystroke is readable the instant it arrives
rather than when Enter commits the line. `/dev/stdin` keeps cooked
semantics unchanged, deliberately — the line buffer is what lets
Backspace erase a character that no reader has taken yet, and an
earlier attempt to skip it for non-blocking readers let a turbo-mode
reader drain the FIFO faster than a human could type. Two names, two
behaviours, no flag day.

The split exists because a shell and a game want opposite things.
`src/tests/raycast.c` polls for WASD every frame and must never wait
for a newline; c4sh must. `/dev/tty` is the right name for it because
it already means this on a real host — it is the controlling terminal,
so the identical guest code works under native c4m, where rawness comes
from `stty raw -echo` outside the VM (src/c4or1k/run-c4or1k.sh does
exactly that). A guest that asks for `/dev/tty` and gets -1 falls back
to `/dev/stdin` and degrades to batched input.

`Devices.rawKbd` counts the open raw descriptors, so an embedding UI
can suppress its own local echo while one is held.

The web terminal (`web/panels.js`) has a SCREEN, not just a scrollback.
It began as a teletype -- one string, re-rendered whole, every non-SGR
CSI dropped -- which is right for a shell and wrong for anything that
repaints: a full-screen program had to scroll the previous frame off to
draw the next, and the picture visibly jumped. It now keeps a cell grid
with a cursor and handles the sequences such a program actually uses:
`H`/`f` (position), `J` (erase display), `K` (erase line), `A`-`D` and
`G` (movement), and `?25` hide/show. Scrollback is unchanged -- lines
accumulate, and "the screen" is the last `rows` of them, which is what
`ESC[H` homes to -- so a program that never positions the cursor
behaves exactly as it always did.

One detail is load-bearing: **the wrap at the last column is deferred**,
as on real hardware. Filling the final column does not move the cursor;
it arms a wrap that the next printable character takes. Wrapping eagerly
costs an extra line for every full-width row, so a program drawing an
80-column frame grows the buffer by a whole screen per frame instead of
repainting in place. `src/c4bb/tests/test-terminal.mjs` pins both that
and the teletype behaviour, and runs at the top of test-c4bb.sh.
A blocking READ that would wait returns -2 to the microcode, which
rewinds PC one word and retries: the machine keeps taking cycle
interrupts while a task waits for input, so the OS keeps scheduling
(the same convention C4IX uses for blocking syscalls).

### Microcode

`hw/microcode.uc` is the single source of truth for CPU behavior; the
assembler (`sim/ucode.js`) turns it into step tables that BOTH engines
execute:

- the **step engine** (`sim/machine.js`) interprets one microstep at a
  time and emits an event per step for the board renderer;
- the **turbo engine** (`sim/turbo.js`) compiles each opcode's step
  list into a JS function — same transfers, same cycle counts,
  ~15-18 M instructions/s (the 1 MHz machine at ~17x realtime).

`tools/lockstep.js` proves the two engines register-identical at every
instruction boundary. One bus, one transfer per microstep; the ALU
computes `left OP right` with left = B (or A), right = A, T or MDR —
mirroring c4's `a = *sp++ OP a`. Wired shifts (`OPR_OUTX4`) scale word
operands to byte addresses for free, and an offset adder on MAR's input
(`MAR_IN+n`) gives the indexed stack reads syscall opcodes need.

A few opcodes (`ITH C4CF SIGH SIGI _TRP DBG C4IV FLT`) run on a
"system controller" (jsop) — a single black-box microstep with
semantics lifted verbatim from c4m.c; candidates for real microcode
later. Unknown opcodes >= 128 raise TRAP_ILLOP, which is the kernels'
syscall mechanism; a missed trap is silent, the oisc4 convention.

### The trap machinery

The trap microroutine (~45 steps, `routine trap` in microcode.uc)
implements c4m's `trap()` verbatim: back off TRAP_OFFSET, push
interval/type/param/mode/a/bp/sp/returnpc/&TLEV/bp-link, read the
handler's ENT operand for locals, mask the interval, enter at
handler+2. TLEV restores pc/sp/bp/a/mode (+ interval when
CONF_TRAP_RESTORES_INTERVAL). Wrinkles that matter, all mirrored:

- the frame captures mode and interval AS THEY WERE at the jam
  (JMODE/JINTERVAL latches) — call sites drop protection and mask
  afterwards;
- per-site effects differ: HIRQ/SIGNAL/OPV/PM zero the interval and
  drop protection, ILLOP only drops protection, _TRP/DBG touch nothing;
- a boundary trap (HIRQ/signal) shares its ++cycle with the handler's
  first instruction; a dispatch trap gives the handler a fresh cycle
  (boundaryChecks returns the increment on boundary jams);
- TLEV is uninterruptible (`*pc != TLEV` guard before fetch).

Verified byte-for-byte against native c4m32 by three purpose-built
tests (custom opcodes/re-entrant traps, preemption with both re-arm
styles, protected-mode violation and emulation) — see
`tests/src/bb_*.c`. The stock `src/tests/test_customop.c` cannot be
used: it predates TLEV/DBG and picks custom opcodes at 64, which now
collide with them (it emits only trap chatter under today's native c4m
too).

### Firmware

`fw/fw.c` (compiled by 32-bit c4lc) supplies what native c4m borrows
from libc: malloc/free/realloc (address-ordered free list with
coalescing, heap bounds from HEAP_BASE/END) and the printf formatter
(adapted from c4lm's vsnprintf, emitting through PUTC). The mechanism:
a syscall opcode's in-place stack arguments are indistinguishable from
ordinary function arguments after a JSR, so MALC's microcode is
literally "JSR through the vector latch at 0x20". No trap is raised,
so the firmware can never conflict with a kernel's ITH handler.
fw_prtf reads its argument count from the ADJ operand after its return
address — the same pc[1] trick c4m uses. Length modifiers (l/ll) are
no-ops, one word per value; this is the well-defined semantic where
native 32-bit c4m + host printf reads garbage for %lld.

## The C4KE filesystem

C4KE ships a kernel-side RAM filesystem (`ramfs_*` in c4ke.c, exposed
as `OP_VFS_PUT/GET/UNLINK/COUNT/NAME`) that was fully implemented but
never populated or exposed: `ls.c` printed a hardcoded fake listing,
`cat.c` only ever read real disk files, and the `c4ke.vfs` "service"
that was meant to load it from a text manifest was a stub
(`// TODO: load c4ke.vfs.txt`). `src/c4ke/bin/vfsload.c` closes that
loop: at boot (spawned by `init.c`, before the shell starts) it reads
`c4ke.vfs.txt` off the c4bb disk - the same real OPEN/READ device
C4IX uses, not ramfs - and populates ramfs from it. `ls.c` now lists
real entries (with a prefix filter standing in for `ls <dir>`) and
`cat.c` checks `vfs_get` before falling back to a real disk read.

The manifest format (documented in full in `c4ke.vfs.txt` itself,
originally sketched at the repo root and never implemented) is a
small declarative tree language: `NAME = word word \` variables
(referenced as `$(NAME)` inside other variables, make-style, or as
bare `$1`/`$2`... inside `each()`), `path/:` directories, `key: value`
or bare `key` file entries, `each(WORDS):` loops in both block and
single-line form, and `ilink(path)` aliases resolved in a second pass
after every real file has loaded (so declaration order never
matters). Since ramfs itself is flat, every entry is really a full
path; `vfsload.c` computes those by concatenating directory prefixes
as it walks the tree. The curated manifest at
`src/c4bb/fs/c4ke.vfs.txt` (copied onto the disk by build-images.sh -
it's curated source, unlike the rest of `images/`, which is safe to
`rm -rf` and rebuild) populates `/bin`, root-level
aliases, `/home/user` (test binaries plus their own source), and
`/usr/src` (every core binary's source plus headers) - about 100
entries from files `build-images.sh` actually places on the disk;
`c4ke.c` itself is deliberately left out of the runnable set (a nested
kernel would fight the parent for the one trap-handler slot) but its
source is still browsable.

Two `c4cc`/`c4lc` gotchas surfaced building this, both worth knowing
before writing another kernel-adjacent program: c4cc's grammar
requires every local declared at the top of its function - no
block-scoped `{ int x; ... }` mid-body - which `c4lc` doesn't share
(`vfsload.c` needs it and is built with c4lc; `ls.c`/`cat.c` stay
c4cc-compatible); and both compilers share a lexer quirk where `'\t'`
and `'\r'` character literals come out as 8 and 10, not 9 and 13 (use
numeric constants instead of fighting it). Separately, `c4lc`'s own
preprocessor hangs on `u0.h` specifically (reproduces at 64-bit too,
so it's not a word-size issue) - preprocess with `gcc -E` first, the
same workaround the kernel build already used.

## The disks

`build-images.sh` builds one shared disk, `images/disk/`, where all
three systems live together: C4DOS's `config.sys` sits beside C4KE's
`c4ke.vfs.txt` and C4IX's binaries, and each system opens the parts it
knows about. That is a fine thing to *demonstrate* — an embedder who
already serves this directory gets all three for the cost of one more
image — and a bad thing to *build on*.

So the tail of the script derives two curated disks from it. That
section is **append-only**: everything above it produces the shared
disk `test-c4bb.sh` boots, and must not move, so the derivation only
copies and deletes — it compiles nothing.

- **`images/dos-recovery/`** — the emergency recovery floppy: C4DOS's
  boot files with a 16 MB RAM disk, `dostar`, `cpp`, `c4cc`, `dosload`,
  `c4ke-src.tar`, `build.bat`, and `c4sh.c4r`. **No `init.c4r`**, on
  purpose: `BUILD.BAT` compiles one, and the kernel is supposed to boot
  *that* one out of memory. Putting a prebuilt init here would hide a
  broken handover.

      node src/c4bb/sim/cli.js -i -d src/c4bb/images/dos-recovery            src/c4bb/images/c4dos32.c4r

- **`images/c4ke-root/`** — a C4KE root filesystem with its own
  toolchain: c4sp, c4rlink, c4cc, the 27 `.lisp` files c4lc is written
  in, `u0.h`, and C4IX's sources under `src/c4ix/`. Derived by
  **subtraction** — copy the shared disk, delete the C4DOS and C4IX
  *binary* parts — because the manifest names ~106 entries and every
  one has to exist on the disk; a hand-written include list would drift
  out of step with it, subtraction cannot.

      node src/c4bb/sim/cli.js -i -m 64 -d src/c4bb/images/c4ke-root            src/c4bb/images/c4ke32.c4r

  Its manifest is `c4ke.vfs.txt` concatenated with
  `src/c4bb/fs/c4ke-dev.vfs.txt` (both curated source), adding
  `/usr/src/c4ix`, `/usr/src/c4sp.c`, `/usr/lib/u0.h` and
  `/usr/lib/lisp`. 151 entries, against `RAMFS_MAX` of 256. Those files
  were physically on the shared disk already and named in no manifest,
  so from inside C4KE they did not exist — `ls /usr/src` showed a
  machine that could not see what it is for.

  The shared disk's own manifest is deliberately left alone: another
  ~45 entries there would spend `test-c4bb`'s cycle budget and eat into
  `RAMFS_MAX` for files only a build needs.

`web/app.js` picks the disk per program through a `DISKS` map (default
`disk`), with one cache entry per directory, so `c4dos32` in the web
demo boots the recovery floppy.

## The C4IX filesystem

`src/c4ix/user/vfsload.c` is the same idea aimed at C4IX's real,
already-hierarchical VFS (`src/c4ix/vfs.c`) instead of C4KE's flat
ramfs: no kernel changes needed, since `sys_open`'s existing
RAM-first/host-fallback precedence (`sys.c:67`) means a plain
`open(name, O_CREATE)` + `write()` from ordinary userland is enough to
put a real, readable, `ls`-visible file anywhere in the tree.
Directories are created with `umkdir()` as the manifest is walked (the
same top-down recursive descent as C4KE's parser, reused almost
unchanged) - a real filesystem, not the flattened-path workaround C4KE
needed. Manifest at `src/c4bb/fs/c4ix.vfs.txt`; run automatically by
`init.c` before the shell starts, same as C4KE.

**Known issue, not yet root-caused**: roughly one run in a few, a
single manifest entry's value comes back empty or corrupted by the
time `load_entries()` reads it, even though tracing confirms it was
stored correctly moments earlier. It heals when unrelated debug prints
are added (which only shift cycle timing) - the signature of a
preemption race, not a logic bug in vfsload.c itself. Confirmed NOT
caused by `ualloc` (=`sbrk`=kernel `malloc`, stress-tested directly:
30 sequential 256KB allocations plus a reference string survive
intact). A related firmware bug WAS found and fixed along the way:
`fw_malc`/`fw_free` (`fw/fw.c`) mutate the shared `__fw_free` list
across many ordinary instructions with no interrupt masking - fine
under C4KE, which never preempts, but a real hazard under C4IX, which
does (`PREEMPT_INTERVAL=10000`). That fix is real and kept, but
doesn't fully close this window. Masking the cycle-interrupt-interval
device register around vfsload's entire run (ordinary memory writes
aren't protected-mode gated, so userland can do this the same way
firmware does) was tried as a fix and made things reliably *worse*
(5/5 failures instead of an intermittent one) - something else
depends on preemption staying live even during this boot-time task,
not yet understood. Likely related to a separate, more severe bug:
`c4ix-top.c4r` run long enough (~1.1M cycles, several redraw
iterations) corrupts a live stack slot with what looks like leftover
heap/string bytes and locks PC at the TLEV ROM address permanently;
root-caused as far as confirming the trap microcode itself is
byte-parity-verified against native c4m (so not a c4bb VM bug) and
that C4IX_STACK_WORDS=8192 rules out simple stack exhaustion, but not
further. Both practical impacts are contained for now: vfsload fails
gracefully (reports "N/M entries loaded" and continues; the c4bb test
suite only requires most entries, not all), and `top` is simply not
recommended for long unattended runs.

## The board

`hw/board.hwd` describes the physical board (modules, nets, layout,
which control signals belong to which chip); `sim/netlist.js` parses
it; `web/board.js` renders it on canvas. Behavior comes exclusively
from microcode.uc, so the picture cannot disagree with the machine. In
step/run modes the current microstep's signals light chips, bus and
control stubs (with the live bus value); in turbo the board glows by
value-change heat. Panels: registers, current microcode routine with
step highlight, UART terminal with cooked-mode echo and line editing.
Module declarations carry a `schem=` field reserved for zoom-in
illustrative chip schematics (not yet drawn).

## Verification

`make test-c4bb` = build 32-bit toolchain + images, then
`tests/test-c4bb.sh`:

- ~15 userland images diffed byte-for-byte against native `c4m32
  load-c4r.c` (trap chatter filtered, the oisc4 recipe); cycles.c4r
  pins exact cycle-counter parity; mandel/test_float pin the FLT
  device
- images whose native-32 baseline is broken (test_args, test_printf
  segfault c4m32; %lld tests) are diffed against 64-bit native instead
- the three bb_* trap tests, byte-identical including re-entrant traps
- C4KE boot to c4sh + running hello off the disk (grep assertions —
  interleavings legitimately differ once firmware printf costs cycles)
- C4IX boot with protected mode + spawned ps (grep assertions)
- step-vs-turbo lockstep over three images
- C4DOS booting the shared disk

Browser verification is headless Chromium via raw CDP capture (see
`docs/` notes in the repo user's memory: page.screenshot hangs).

`tests/test-ladder.sh` is **not** part of `make test-c4bb` and is meant
to be run by hand: it compiles an operating system inside a virtual
machine and takes minutes. It walks the whole climb — C4DOS builds
C4KE, `dosload` hands the machine over, the kernel seeds its RAM
filesystem from DOS's RAM disk and boots the init that only exists in
memory, then c4cc/c4rlink and c4lc/c4rlink each complete a compile-link
round trip entirely in that filesystem. `docs/homeward-ladder.md`
tracks the whole thing.

## Upstream discoveries made along the way

- native c4m32 (`gcc -m32` c4m) segfaults on u0 images that probe
  custom opcodes hard (test_args, test_printf) and on booting C4IX —
  the c4bb simulator is currently the more robust 32-bit host
- src/tests/test_customop.c uses custom opcodes 64+, colliding with
  TLEV/DBG; produces nothing but trap chatter on any modern c4m
- C4LM is marked non-working upstream ("cannot task switch more than
  twice"), so the "OS on plain c4" milestone was dropped in favor of
  C4KE/C4IX
- C4IX had 64-bit word-size assumptions (sched.c trampoline +16 and
  returnpc -8, loader.c's byte walk, several malloc sizes); fixed with
  sizeof(int) arithmetic, 64-bit behavior pinned by make test-c4ix
- sim/loader.js ignored the format-v3 MEMSZ word (byte 5), so the BSS
  a c4lc image declares -- its uninitialized globals, which occupy no
  image bytes -- was never reserved. `top` came from the on-disk data
  length, so the heap (and any image loaded after) began INSIDE those
  globals and malloc handed out memory that aliased them. It hid for a
  while because it only bites a program that both has uninitialized
  globals and allocates: src/tests/raycast.c does both, and showed up
  as a maze that generated identically on c4bb and c4m32 but rendered
  from a different camera position. Found 2026-08-22; the fix is to
  read MEMSZ and reserve max(DATALEN, MEMSZ). The arena is already
  zero-filled, so reserving the space is the whole fix.

## Known deviations from native c4m

- PUTS returns 10; glibc's return value is unspecified beyond >= 0
- STRC prints nothing (native prints a symbol stacktrace; symbols are
  not loaded)
- %p prints arena offsets, not host pointers (as under oisc4)
- RALC is implemented (native c4m's is documented-broken)
- printf costs hundreds of instructions instead of one host call, so
  absolute cycle positions past the first printf differ from native;
  everything cycle-relative in a print-free stretch is exact
