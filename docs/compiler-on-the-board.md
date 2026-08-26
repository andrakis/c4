# Compiling C4KE on the breadboard

Tracker for one question: **what is a realistic way to compile C4KE inside
c4bb quickly?** Companion to `docs/compiler-speed.md` (which is about the
host build) and `docs/c4fc-design.md` (which is about the compiler).

**Rule for this document:** tick a box only when its verification command
has actually been run and is green, and paste the one-line evidence beside
it. Say plainly what was *not* done and why.

## Where the 5m29s went

`docs/c4fc-design.md` records c4fc compiling one C4IX module on the board in
**5m29s** and says "nineteen seconds of that is loading the compiler". That
was true and it was the wrong thing to look at. The measurements below were
taken 2026-08-26 to find the real distribution.

### The unit, and the conversion factor

c4bb's `-s` counts VM instructions, the same unit `__c4_cycles()` returns
under c4m, so a cycle count taken on either host converts to board seconds
by one constant:

    node src/c4bb/sim/cli.js -s -m 64 spin32.c4r
    -> 78,001,780 cycles in 3.80s (20,516k inst/s)

**20.5M instructions/second.** Everything below is quoted in instructions
first and board-seconds second, because instructions are what a change to
the compiler moves and seconds are what the user waits.

The model checks out against the published figure: c4fc compiling
`src/c4ix/vfs.c` (`-O -c`) costs **7.22e9 instructions** under c4m, which
predicts 352 s on the board against 5m29s = 329 s measured. Close enough
that the constant can be trusted for everything else.

### c4fc compiling all of C4KE

    ./c4m load-c4r.c -- c4th.c4r <the 15 .f files> prof.f \
       -e ': GO 1 OPTIMIZE ! C4FC-INIT -P ... S" c4ke.c" C4FC-PROF ; GO'

| phase | instructions | share |
|---|---:|---:|
| lex + preprocess | 19,161,602,013 | 22.4% |
| parse pass 1 (discovery) | 8,771,431,501 | 10.3% |
| T2 close + externs | 5,347,305,429 | 6.3% |
| parse pass 2 | 8,592,802,408 | 10.1% |
| **optimizer (`-O`)** | **40,966,931,887** | **47.9%** |
| **total** | **~85.5e9** | |

**85.5 billion instructions = 69 minutes on the board.** My guess before
measuring was "the parser and the peephole fixpoint". Half right: the
optimizer is indeed the single biggest phase at 48%, but the *parser* is
only 20% and the *preprocessor and lexer* are 22% — and the preprocessor is
the interesting one, for the reason in the next section.

### The same work by a compiler that is native VM code

`cpp.c4r` and `c4cc.c4r` are ordinary `.c4r` images: C compiled to C4
instructions. Both were run on the breadboard against the same kernel
source, with the sources on a `-d` disk:

| tool | instructions | board seconds |
|---|---:|---:|
| `cpp32.c4r` preprocessing `c4ke.c` (5,513 lines out) | 56,604,737 | **2.89** |
| `c4cc32.c4r` compiling that `c4ke.i` | 124,140,840 | **5.93** |
| **together** | **180,745,577** | **8.8** |

**c4fc: 85.5e9. cpp + c4cc: 0.18e9. A factor of 472.**

Phase against phase, on identical work: c4fc's preprocessor+lexer costs
19.16e9 where `cpp` costs 0.057e9. **338x for the same job.**

### Where the 472x actually comes from

An instrumented build of c4th (a counter in `th_run`, scratch only) counted
every word the C4KE compile executes:

    words executed: 841,252,663  (colon 94,999,793, prim 746,252,870)  distinct 884

85.5e9 instructions / 841e6 words = **102 VM instructions per Forth word
executed**. The threaded `NEXT` is only about twenty of those; the rest is
that every primitive is a C function reached through a code field, and
inside it `th_push`/`th_pop` are C calls of their own.

The top of the profile says what those words are:

| share | cum | word |
|---:|---:|---|
| 13.06% | 13.06% | `@` |
| 12.49% | 25.55% | `EXIT` |
| 11.89% | 37.44% | `+` |
| 6.22% | 43.66% | `CELLS` |
| 5.65% | 49.32% | `LFP` |
| 5.41% | 54.72% | `LIT` |
| 4.82% | 59.54% | `LA` |
| 4.82% | 64.36% | `LSTK` |
| 4.72% | 69.08% | `1+` |
| 3.02% | 72.10% | `(LOCAL@)` |
| 2.91% | 75.02% | `!` |
| 2.54% | 77.55% | `0BRANCH` |

Read it as two facts:

1. **`@ + CELLS LIT 1+` = 41% of all word executions is field access.**
   The DSL stores every node, token and symbol as a cell array indexed by
   field number, so `y.nlen` is `LIT CELLS + @` — four words, ~400 VM
   instructions, where C's `y[NLEN]` is one `LI` at a constant offset,
   two or three instructions. That single idiom is over a hundred times
   more expensive than the C it stands for, and it is the most common
   thing a compiler does.
2. **`LFP LA LSTK (LOCAL@) (LOCAL!) LSP (LFRAME) (LDROP)` = 20%** is
   `locals.f` — named locals, entirely bookkeeping.

So the gap is not mainly the threaded interpreter. It is that c4fc's data
model spends ~400 instructions on what C spends 3 on, and does it 340
million times.

### What compiling the Forth to native code would actually buy

`native.f`'s strategy (b) turns `@` into `LI`, `+` into `ADD`, `LIT` into
`IMM`, and constant-folds `CELLS`; `locals.f`'s words become `bp`-relative
frame slots, which is what B5c's frame already is. Bottom-up estimate:

    746M primitives x ~2 instructions        = 1.5e9
     95M colon entries x ~14 (call + return) = 1.3e9
                                        total ~2.8e9  -> 137 s on the board

**About 30x, so 69 minutes becomes a bit over two minutes.** Real, and
still 15x slower than `cpp` + `c4cc` at 8.8 s. The B5 probe's 14x
(343 cycles/iteration threaded vs 24 native) is the *arithmetic* ceiling;
the estimate above is higher than it because the DSL's field-access idiom
folds away completely, which a counting loop has none of.

And it is not free: `native.f` inlines calls and declines recursion
(`docs/c4th-design.md`, "native.f and c4fc: measured, and the answer is
no"), c4fc is recursive descent, and `self.f` deliberately has no
compile-time execution while the DSL is built on `CREATE`/`DOES>`,
`POSTPONE` and `EVALUATE`. Either route is a project, not a flag.

## The decision

**Two compilers for two jobs, which is what the numbers say and what DOS
always did.**

- **`cpp` + `c4cc` is how C4KE gets built on the board.** It is native VM
  code, it is 472x cheaper, it is already what the host Makefile uses to
  build `c4ke.c4r`, and `c4dos-build/` already carries it as a bootable
  floppy. Nothing has to be written; it has to be built at 32 bits and
  demonstrated.
- **c4fc stays what it is:** the compiler that matches c4lc byte for byte,
  has a real preprocessor, `-O` and object mode, and is written in
  something a person can read and extend at the C4DOS prompt. It is the
  interactive compiler, not the batch one.

The native-Forth path is not abandoned, but it is now correctly sized: it
is worth ~30x, it lands at ~2 minutes, and it should be judged against
what else two months of work could buy. It is **not** the way to make
C4KE build quickly on c4bb.

## The other half: C4IX

The progression the machine has to walk is **C4DOS -> compile C4KE ->
compile C4IX**. The first arrow is done and takes 13.7 s. The second one
is where the remaining work is, and it is not a speed problem.

### What it costs today

| compiler | instructions / preprocessed line | C4IX (8,913 lines) |
|---|---:|---:|
| `c4cc` (native VM code) | 22,500 | **~10 s** |
| `c4lc` on `c4sp32` (Lisp on Lisp) | 3,200,000 | ~23 min |
| `c4fc` on `c4th32` (Forth on Forth) | 7,300,000 | ~66 min |

The c4lc figure is measured, not scaled: `va.c`, the *smallest* C4IX
module at 514 preprocessed lines, on the board:

    node src/c4bb/sim/cli.js -s -m 512 -d ixdisk c4sp32.c4r -R \
         src/c4sp/lisp/c4lc.lisp -O -c va.i out.c4o
    -> 1,646,519,810 cycles in 81.20s

Worth recording that the Lisp compiler on the Lisp interpreter is **2.3x
cheaper per line than the Forth compiler on the Forth interpreter**. Both
are three hundred times the compiler that is native code.

### Why c4cc cannot do it, exactly

Every one of the twelve C4IX modules dies in the same place:

    boot: 139: bad global declaration struct vnode {

Probed feature by feature, what c4cc has and has not:

| | |
|---|---|
| has | `enum`, `switch`, `sizeof`, `?:`, `while`, `break`, `continue` |
| **lacks** | `struct`, `union`, `typedef`, `.`/`->`, struct-size pointer arithmetic, `do/while`, all ten compound assignments, block-scoped declarations with initializers |

That list is exactly **c4lc's L7** (`docs/c4lc-design.md` §10), the
milestone written *because* C4IX needed it. So the question "how does the
board compile C4IX quickly" has one honest answer: **teach c4cc L7.**

It is a bounded job with three oracles — `src/tests/c4lc_l7.c` differs
c4lc against gcc already, c4lc is a working reference, and c4fc is a
second one whose whole struct algebra is 86 lines of `src/c4fc/types.f`.
Object mode is *not* needed: c4cc concatenates its inputs, so the twelve
modules compile whole-program in one invocation and c4rlink never enters
the picture.

### A bug found while probing, and fixed

c4cc's `for` statement **had never worked**. The branch never consumed
its own keyword, so the open-paren check saw `For` and every for-loop
c4cc was ever handed died on `open paren expected`. With that fixed the
rest of the branch was still wrong — the `;` handling was one expression
out of step, and the closing jump targeted the `BZ`'s patch slot rather
than the step — so it is rewritten to the layout its own comment
described, and `src/tests/test_for.c` pins it against gcc's output.

Nothing in the tree writes a `for`: it was all written against c4, which
has no `for`, and `src/tests/test_continue.c` says "We only have while
loops currently" in its second line. That is why five years of tests
never touched it. It matters now because a person writing C at the C4DOS
prompt will type one within a minute.

## The ladder, as it now runs in the machine

HOMEWARD's premise (`docs/homeward-ladder.md`) is that the player climbs
from transistors to C4IX. The software half of that climb should be
*earned* — each rung built by the rung below it, not handed over
preinstalled. `LADDER.BAT` on the build floppy is that climb, and on
2026-08-26 the breadboard ran all of it:

| | rung | evidence |
|---|---|---|
| 1 | the seed compiler builds **a compiler** | `wrote 213438 bytes to ram:c4cc2.c4r` |
| 2 | that compiler builds **the preprocessor** | `wrote 71846 bytes to ram:cpp2.c4r` |
| 3 | and **builds itself again** | `wrote 213438 bytes to ram:c4cc3.c4r` — same size: the fixed point |
| 4 | the new tools build **the kernel** | `wrote 187771 bytes to ram:c4ke.c4r` — the same byte count the shipped tools produce |
| 5 | and **the init process** | `wrote 61911 bytes to ram:init.c4r` |
| 6 | the machine **boots what it built** | `C4SH - The C4 SHell`, then `clean shutdown` |

    c4bb: 666752554 cycles in 31.49s

**Thirty-one seconds**, and every image after rung 1 was made by an image
this machine compiled. `make test-c4dos-ladder32` pins it — including the
two numbers that would move first if a self-built compiler ever drifted.

## Why B4KE has to exist, and it is not a convenience

C4DOS parses a command line into at most **fifteen tokens including the
verb** (`ARGVMAX = 16`, and `parse_line` stops at `argc < ARGVMAX - 1`).
Demonstrated on the board rather than read off the source — fifteen
arguments handed to a program that prints its own `argv`:

    A>RUN args.c4r a1 a2 a3 a4 a5 a6 a7 a8 a9 a10 a11 a12 a13 a14 a15
    Arg count: 14
    ...
    At 13: a13

`a14` and `a15` are gone, silently. Now count the sentence that links
C4IX:

    RUN c4rlink.c4r boot.c4o console.c4o va.c4o host.c4o sl4b.c4o
        task.c4o sched.c4o vfs.c4o sys.c4o c4ke.c4o loader.c4o
        init.c4o -o c4ix.c4r

That is sixteen tokens. **The machine cannot say the sentence that
links its own operating system.** A build tool that reads a file instead
of a command line is therefore not a quality-of-life feature; it is the
only way the sentence gets said. That is B4KE's reason to exist, it is
demonstrable in-game, and it is the same wall DOS linkers hit and
answered with response files.

The kernel rung does not hit that wall, which is why C4KE builds today
and C4IX does not. So the ladder's shape is forced and it is exactly the
one the narrative wants:

    C4DOS -> toolchain -> C4KE -> [B4KE + L7] -> C4IX

## B4KE, and response files

Both open questions were answered on 2026-08-26, and the answers divide
the work cleanly.

### Response files close the command-line gap

`@NAME` on any c4cc or c4rlink command line is replaced by the
whitespace-separated words in `NAME` (`respfile_expand`, in
`src/c4cc/asm-c4r.c`, so both tools get it from one place). The file is
found the way every input here is found: C4DOS first, then the C4KE RAM
filesystem, then the host — so a list a build just wrote is findable by
the tool that has to read it. `#` starts a comment.

The link that could not be typed is now four tokens:

    RUN c4rlink.c4r @c4ix.objs -o c4ix.c4r

`make test-respfile` pins the only thing that matters: the image built
through a response file is **byte-identical** to the one built by typing
the arguments. An expanded argument has to be indistinguishable from one
that was there all along.

### B4KE is a C4KE program, by decision

C4DOS keeps `BUILD.BAT` and `LADDER.BAT`. B4KE runs only under C4KE,
because C4KE is what gives it the two things a build tool needs and DOS
has neither: **tasks it can start and wait for**
(`kern_user_start_c4r` / `await_pid`) and **a filesystem it can write
and read back** (`OP_VFS_*`). That is not a limitation dressed up as a
feature — it is part of the answer to "what did building the kernel buy
me", which is the question the ladder exists to make the player ask.

`src/c4ke/bin/b4ke.c`, 250 lines, five verbs, one per line, dull on
purpose in the way `C4TAR1` is dull:

    B4KE1                     magic, first line
    # ...                     comment
    ECHO text                 progress, for a person watching
    LIST name word word ...   write those words to `name`, one per line
                              -- the object list a later RUN passes as
                              @name
    TARGET name               what the NEXT run must produce; if it is
                              already there the run is skipped
    RUN prog arg arg ...      start it, wait for it

`-f FILE` chooses the build file (default `BUILD.B4K`), `-n` says what
it would do without doing it, `-k` keeps going past a failure.

**Presence, not timestamps — and that is not laziness.** `make` decides
by comparing modification times. Neither filesystem here has any: C4DOS's
RAM disk keeps name, data, length and capacity per slot and nothing else
(`c4dos.c:102-108`), and C4KE's ramfs is the same shape. So the rule is
"if the target is already there, skip it", which is enough to resume an
interrupted build and honest about what this machine can actually know.
Deleting a target is how you force a rebuild — which is why `rm` exists
on every system that ever worked this way.

**A step is judged by what it produced.** There is no exit status to
read across `await_pid`, so B4KE checks that `TARGET` exists afterwards
and stops if it does not. That is also the only check that survives a
tool which fails halfway and leaves nothing behind.

### The C4IX build in miniature, and it runs

`src/c4ke/bin/b4ke-selftest.b4k` is the C4IX shape at 1/6th scale: two
modules that reference each other's symbols, compiled separately into
the RAM filesystem, an object list **written by the build**, a link that
reads that list back as a response file, and the image run. Under C4KE:

    b4ke: compiling module a...
    c4cc: wrote 983 bytes to ramfs:tla.c4o
    b4ke: compiling module b...
    c4cc: wrote 1133 bytes to ramfs:tlb.c4o
    b4ke: writing the object list...
    b4ke: linking through the list...
    c4rlink: wrote 1898 bytes to ramfs:b4ked.c4r
    b4ke: running what we built...
    b_add(3, 4) = 7
    b4ke: 4 ran, 0 skipped, 0 failed

1,898 bytes is what the same link produces on the host. `make test-b4ke`
pins it, and pins the dry run touching nothing.

**What is still missing for the real thing is L7, not B4KE.** The
twelve-module C4IX build is this file with twelve `TARGET`/`RUN` pairs
and a longer `LIST`. It will work the day a compiler on this machine can
read `struct vnode {`.

## Milestones

- [x] **M0** This tracker, with the measurements above, before any code.
- [x] **M1** `c4cc32.c4r` plus `init32.c4r`, `c4sh32.c4r`,
      `c4ke.vfs32.c4r`; `dostar32.c4r` and `dosload32.c4r` already had
      rules and had never been built. `make c4dos-build32` builds all six
      with `c4cc32` and nothing else.
- [x] **M2** `c4dos-build32/`, and `make run-c4dos-build32` for the
      interactive session.
- [x] **M3** `BUILD` on the breadboard. Sources unpacked in the machine,
      preprocessed in the machine, compiled in the machine:

          c4ke.i    <ram> 199124 bytes
          c4ke.c4r  <ram> 187771 bytes
          init.c4r  <ram>  61911 bytes
          c4bb: 268504123 cycles in 13.26s (20243k inst/s), status 0

      **13.26 seconds**, against c4fc's 85.5e9 instructions = 69 minutes
      for the kernel alone. A factor of **312** on the whole task.
- [x] **M4** Boot what it built. `RUN dosload.c4r c4ke.c4r` on the board:

          c4ke: Kernel ready in 631ms after 278.791 M cycles
          C4SH - The C4 SHell v 0.1a
          c4ke: clean shutdown in 390ms, ... total C4KE runtime 4.478s

- [x] **M5** `make test-c4dos-build32` — build, boot and clean shutdown,
      non-interactive:

          c4bb: 282761026 cycles in 13.73s
          test-c4dos-build32: OK

      **Fourteen seconds for a machine to compile and boot its own
      operating system**, which is the whole point of the exercise.
- [x] **M6** `for` in c4cc: fixed, rewritten, and pinned against gcc by
      `make test-c4cc-for` / `src/tests/test_for.c`.
- [x] **M7** `LADDER.BAT`: the machine rebuilds its own toolchain and
      builds the kernel with what it built, pinned by
      `make test-c4dos-ladder32` — which hardcodes no byte counts and
      instead derives the two facts that matter:

          ladder: c4cc is a fixed point at 218124 bytes;
                  both kernels are 187771 bytes
          c4bb: 824181150 cycles in 38.63s

      "Both kernels" is the self-built compiler's output against the
      shipped compiler's, from the same `.i` — if a self-built compiler
      ever drifted, that is where it would show.
- [x] **M8a** Response files in c4cc and c4rlink — `make test-respfile`:
      `cmp .rf_plain.c4r .rf_resp.c4r` passes, so an expanded argument
      is indistinguishable from a typed one.
- [x] **M8b** B4KE, a C4KE program — `make test-b4ke`:
      `b4ke: 4 ran, 0 skipped, 0 failed`, and the linked image is the
      same 1898 bytes the host link produces.
- [ ] **M9** L7 in c4cc — `struct`, `union`, `typedef`, `.`/`->` with
      struct-size pointer arithmetic, `do/while`, compound assignment,
      block-scoped declarations. The bar is `src/tests/c4lc_l7.c` against
      gcc, then the twelve C4IX modules compiling, then `c4ix.c4r`
      booting on c4bb.

## What is deliberately not here

- **No change to c4fc.** Nothing measured here says c4fc is wrong; it says
  it is not a batch compiler for a 20 MHz machine.
- **No new VM opcodes.** `docs/c4th-design.md` §8 already rules on that.
- **No `-O` on the board.** `c4cc` does not optimize and the kernel does
  not need it to boot; `c4opt` remains a host-side pass.
- **`vfsload` is missing from the floppy.** The booted kernel says
  `lc4r: unable to open 'vfsload' or 'vfsload.c4r'` and carries on to the
  shell; the 64-bit `c4dos-build/` has the same gap. Not fixed here
  because it is not what this tracker is about.
- **The `.c4o` route is not on this disk.** c4rlink and object mode are
  c4fc's, and `c4cc` compiles the kernel whole in one pass, so there is
  nothing to link. If the board ever wants separate compilation it is
  `c4cc -c` that would have to grow it, not c4fc that would have to get
  faster.
