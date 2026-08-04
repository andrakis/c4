# C4IX — a Linux-like operating system, compiled by c4lc

C4IX is a ground-up successor to C4KE: written against c4lc's extended
language (structs and friends), built as separately compiled objects
linked by c4rlink, optimized end to end by `c4lc -O`, targeting c4m
first with a degraded-but-working plain-c4 mode via the c4l loader.
The name is the user's, from an earlier rewrite attempt.

Everything here builds on docs/c4lc-design.md (the compiler) and
docs/internals.md §6.5 (the c4l loading system).

## 1. Why a rewrite

C4KE is written in the bare c4 subset: every kernel structure is an
int array indexed by enum offsets (`task[TASK_NICE]`), IO goes
straight to the host (PRTF prints to stdout, unredirectable), and the
whole kernel is one 3300-line translation unit compiled in one shot.
Each of those is a language problem, not an OS problem. Fix the
language, and the OS gets to be ordinary readable code.

## 2. The language: c4lc L7

The compiler grows past the c4 subset. c4cc stays where it is — the
oracle for the old subset — and gcc becomes the differential oracle
for everything new (same source, same output, both compilers).

### 2.1 Structs, unions, typedef

- `struct T { ... };`, `union T { ... };`, `typedef`, member access
  via `.` and `->`, `sizeof(struct T)` and sizeof of typedef names,
  pointers to structs (including in casts), arrays of structs,
  struct-typed globals and locals.
- NOT in L7: struct-by-value parameters, returns, or whole-struct
  assignment (use pointers and memcpy — kernels do anyway); bitfields;
  nested struct *values* as members (struct pointers as members are
  fine); multi-dimensional arrays.

**Type encoding** stays integer, extending c4's: `CHAR=0, INT=1`,
`+2` per pointer level. Struct types live at `1024 + 64*id +
2*ptrlevel` — 64 per struct leaves 31 pointer levels, and `ty >= 1024`
is the "is a struct type" test. The parser owns the struct registry
and typedef table (it must, to resolve `struct T` and typedef names
while parsing — the classic lexer-hack, solved by construction).

**Layout**: every member starts word-aligned; char array members
occupy `(n+7)/8` words; `sizeof(struct)` is a word multiple. Simple,
predictable, and never compared against gcc's layout — the oracle
compares program OUTPUT, not offsets.

**Semantics changes in codegen**: pointer arithmetic and indexing
scale by the ELEMENT SIZE (struct pointers step by the struct size —
real C semantics, verified by the gcc oracle; int*/char* keep their
old 8/1). A struct-typed name evaluates to its address, like arrays;
loading or assigning a whole struct is an error.

### 2.2 C99-flavored conveniences

- `do { } while (e);`
- Compound assignment `+= -= *= /= %= &= |= ^= <<= >>=`, desugared by
  the parser to `L = L op R`. Caveat, documented: the lvalue is
  evaluated twice, so `a[i++] += x` is wrong — the linter rule is the
  same one kernels already follow.
- Declarations anywhere in a block, with block scoping and
  NON-CONSTANT initializers (`int x = f(a);`). The AST makes this
  cheap: a pre-pass over the function body assigns every declaration
  a frame slot so ENT still reserves the whole frame up front;
  initializers become ordinary assignments at the declaration point;
  the symbol list save/restore at block boundaries gives scoping.
- `continue` (already works), `for` (already works, better than c4cc).

## 3. The toolchain: c4lc L8

- `c4lc -c file.c -o file.c4o`: compile one unit, leaving undefined
  externs as SYMBOL-typed patches (positive patch types) instead of
  exit-stubs, with a full symbol table (defined + extern entries).
  c4rlink already merges objects, resolves symbols, rebases segments,
  and has `-r` library mode for `.c4l` archives — it was built for
  this.
- Build shape: `c4lc -O -c` each module, `c4rlink` the objects plus
  `libc4ix.c4l` into the kernel image and into each userland binary.
- Full optimization everywhere: tree passes per module; the peephole
  passes run per module (post-link peephole is a later option).

## 4. The OS: C4IX

### 4.1 Principles

- **Syscalls own all IO.** Userland never executes PRTF/OPEN/etc
  directly: binaries run in protected mode, the VM traps privileged
  opcodes into the kernel (the mechanism C4KE proved), and userland's
  printf is a LIBRARY over `write(fd, buf, len)`. That single decision
  is what makes redirection, pipes, and ttys possible.
- **Everything is a struct.** Tasks, fds, vnodes, wait queues:
  ordinary c4lc structs.
- **Degraded plain-c4 mode.** Plain c4 has no traps and no cycle
  interrupt, so under `./c4 c4l.c c4ix.c4r`: cooperative scheduling
  (explicit yield), no protected mode, direct IO. The kernel detects
  the host via `__c4_info()` (0 under plain c4) and configures itself.
  c4m first; c4 is a supported degradation, not the design center.

### 4.2 Milestones

- **X0 — boot.** Kernel of 3+ objects linked by c4rlink, boots on c4m,
  prints via its own kprintf, runs one init task, clean shutdown.
  Also boots under `./c4 c4l.c` (degraded path proven early).
- **X1 — tasks.** DONE, see §6. Tasks on the linked list X0
  established (no fixed table, no TASK_MAX), scheduler (preemptive on
  c4m via cycle interrupt, cooperative fallback) context-switching
  along the round-robin walk task_next() already provides, fork-less
  spawn from .c4r images (loader in-kernel), wait/exit. Task
  allocation moves from malloc-per-task to **SL4B**, a Linux-style
  slab allocator with per-struct caches (tasks first; fds and vnodes
  join it in X3).
- **X2 — syscalls.** DONE, see §6. Protected mode on, trap-based
  syscall layer: write/read/open/close/spawn/wait/exit/yield/sbrk/
  getpid. Userland libc4ix (printf over write) as a `.c4l` library.
- **X3 — IO.** DONE, see §6. Per-task fd table, vnode layer: console,
  RAM files, pipes. `dup2`, and with it redirection.
- **X4 — shell.** DONE, see §6. c4ix-sh: argv parsing, `>` `<` `|`
  `&`, builtins enough to demo `cat file | wc > out`.
- **X5 — polish.** DONE, see 6 and 7. Suite and benchmark ported;
  boot-cycle and workload measurements against C4KE.

### 4.3 What gets reused

- c4m unchanged (traps, cycle interrupt, protected mode are enough).
- load-c4r's format knowledge; the in-kernel loader is new code
  against the same .c4r format.
- C4KE's hard-won lessons (documented in internals.md): trap design,
  the idle-task discovery dance (C4IX will pass the idle address
  explicitly instead), RAM-FS opcode interface if kexts are wanted.

## 5. Testing

- L7: gcc differential on every language feature (c4cc cannot compile
  structs; gcc can). The battery pattern from c4lc L3.
- L8: link a multi-object program, byte-compare behavior against the
  same source compiled whole; c4rlink's existing test-link extended
  with c4lc objects.
- C4IX: boot pins like test-c4lc's kernel pin; per-milestone
  functional tests (X3: redirect a program's output into a RAM file
  and read it back; X4: pipe two programs).

## 6. Status

- [x] L7 structs/typedef/unions + C99 conveniences (2026-08-03: full
      feature set gcc-differential-identical, plain and -O, pinned by
      src/tests/c4lc_l7.c in test-c4lc; struct parameters remain
      pointer-only, whole-struct assignment intentionally rejected)
- [x] L8 objects + linking (2026-08-03: `c4lc -c` emits .c4o objects —
      undefined prototypes become extern symbols with SYMBOL-typed
      patches, main optional (entry -1), static/extern attrs carried
      so c4rlink's merge rules apply; -O keeps every non-static
      function as a dead-elimination root. Verified: c4lc objects
      link with c4cc objects in either direction; an L7 struct
      program built from separately compiled -O objects matches the
      whole-program compile and gcc. Extern DATA followed on
      2026-08-03 (`7d784f6`): extern globals become class-Glo
      ATTR_EXTERN symbols, c4rlink rebases Glo values by the data
      offset and resolves their symbol patches to DATA patches;
      pinned by test_link_e/f against gcc. The kernel uses it for
      task_head.)
- [x] X0 boot (2026-08-03: src/c4ix/ -- six modules (boot, con, va,
      host, task, init) each compiled `c4lc -O -c` and linked by
      c4rlink into c4ix.c4r; boots natively on c4m and degraded on
      plain c4 via the c4l loader, pinned exactly by `make test-c4ix`
      against src/c4ix/tests/. Host detection via one INFO opcode
      (0 = plain c4). Console is kprintf over PUTC only. Tasks are a
      malloc'd singly linked list -- no TASK_MAX -- with task_next()
      wrapping tail->head, the walk X1's context switch slots into;
      init runs as task 0 and task_shutdown() frees the list before
      the shutdown banner. Varargs lessons, learned the hard way:
      (1) the stock stdarg.h keeps its va area in per-unit statics
      which never merge across objects, so C4IX owns ONE va area in
      va.c behind extern functions (__c4cc_make_va resolves to it at
      link time); (2) va_end must see the va_list exactly where
      va_start left it -- it re-reads the count slot to pop the va
      area -- so consumers walk a copy (kprintf's ap/walk pair);
      (3) `va_list a, b` declares b as plain int -- one declarator
      per line.)
- [x] X1 tasks (2026-08-03: nine modules -- sl4b, sched and loader
      join X0's six. ONE switch mechanism, TWO backends behind the
      same saved-state format: on c4m every switch runs in a trap
      handler (soft trap = yield, cycle interrupt = preemption) that
      rewrites its own parameters for TLEV; on plain c4 sched_yield
      rewrites its own frame pair and returns through a double LEV
      (test_coop_switch.c's proof, verified under c4lc plain/-O on
      both hosts before building on it). Fresh tasks are forged
      SV_FRAME states both backends resume; task_shim turns an
      entry's return value into task_exit. SL4B slab caches allocate
      task structs (stacks and images stay malloc: variable-size).
      sched_lock/unlock = interval masking, nestable; kprintf lines
      and the shared va area are lock-bracketed (a preempted
      make_va's globals would otherwise be clobbered mid-flight).
      task_spawn: in-kernel c4l.c-port loader, constructors at load,
      destructors counted but not run (X1 debt), image freed on
      reap. sched_stop() disarms the trap machinery before teardown
      -- without it the cycle interrupt fires into the freed task
      list after main returns. Pins: exact boot transcripts per host
      (src/c4ix/tests/x1-*.txt); the cooperative ping/pong runs
      under sched_lock so its interleaving is pure round-robin on
      both hosts, and the preemption demo prints only booleans
      (busy tasks that never yield each observed the other mid-run),
      verified stable across preemption intervals 7777/10000/43210.)
- [x] X2 syscalls (2026-08-03: sys.c joins the kernel, libc4ix.c4l
      and c4ix_user.h join userland. TWO DOORS, one dispatcher:
      (a) the gateway -- userland executes custom opcode SYS_* (>=
      200), unknown to c4m, so it raises TRAP_ILLOP into the same
      handler that does context switches; (b) plain c4 has no traps
      at all, so the loader injects &sys_dispatch into the image's
      __c4ix_systable global (found by walking the .c4r symbol
      section) and libc4ix calls it directly. libc4ix picks its door
      once, in its constructor, by whether the slot was filled.
      Spawned tasks are PRIV_USER and resume in c4m's protected mode
      -- the handler assigns its own `mode` parameter, which TLEV
      loads. So a program that never heard of C4IX and just calls
      printf traps on PRTF and gets EMULATED onto sys_write: the
      argument count comes from returnpc[1] (the compiler always
      emits "PRTF; ADJ n"), which is how redirection reaches
      unmodified binaries. c4m change: the EXIT guard, commented out
      since the C4KE work, is enabled -- a protected task calling
      exit() must not halt the VM. C4KE is unaffected (its PM is
      compiled out) and `make test` confirms it.
      Two performance bugs found and fixed by measurement, both
      worth remembering: task_wait SPUN on sched_yield (now parks in
      TS_WAITING and sched_waitdone delivers the exit code on wake),
      and libc4ix's formatter wrote ONE CHARACTER PER SYSCALL --
      through the trap gateway that is a kernel round trip per
      letter. Buffered to one write per call, the uhello run went
      from >30s to 0.107s.
      Pins show the boundary working from both sides: on c4m the raw
      printf program costs 2 syscalls (trapped and emulated) and the
      libc4ix program 172; on plain c4 the same raw program costs 0
      (no boundary to enforce) and libc4ix 9 via the direct door.)
- [x] X3 IO (2026-08-03: vfs.c joins the kernel -- the classic three
      layers, because dup2 and pipes need exactly them. A VNODE is
      the thing (console, RAM file, host file, pipe); an open FILE
      description holds flags and position; an FD indexes a per-task
      table. dup2 points two fds at ONE description (shared
      position), while two opens get two. Spawning CLONES the table,
      sharing descriptions -- which is redirection without fork: set
      fd 1 up, spawn, put fd 1 back. vnode and file objects come
      from SL4B caches, as X1 planned. RAM files are the only
      writable storage (no write() opcode exists), and open()
      consults them before the host, C4KE's rule.
      Blocking reads use RESTARTABLE syscalls: a task reading an
      empty pipe parks in TS_BLOCKED and the handler rewinds its pc
      one word, so on wake it re-executes the syscall opcode with
      its arguments untouched on its own stack. Kernel-context
      callers spin on yield instead. Demonstrated by init doing a
      shell's job by hand: a RAM file written and read back; the
      UNMODIFIED raw-printf program run with fd 1 redirected into a
      RAM file (58 bytes captured on c4m; 0 on plain c4, which the
      pin states outright -- no protected mode means a raw printf
      never reaches the kernel); and uecho | uwc through a real
      pipe, which works on both hosts.
      Four bugs worth remembering, all found by measurement:
      (1) freeing an exiting task from inside the trap handler frees
      THE STACK THE HANDLER IS RUNNING ON -- corpses now go on a
      reap list drained at the next safe point (C4KE's idle-task
      lesson, relearned);
      (2) the shared va area was not preemption-safe: push happens
      at the call site, pop in the callee, so an interleaved task
      could rewind the cursor below live arguments. make_va now
      holds sched_lock until va_end, making variadic calls atomic;
      (3) a prototype CANNOT shadow a c4lc builtin -- uwc's read()
      compiled to the READ opcode, which only worked on c4m because
      protected mode trapped it. Hence libc4ix's u-prefixes;
      (4) the root cause of a week of heisenbugs: c4m's TRAP_ILLOP
      site did not force MODE_UNPROTECTED like every other trap
      site, so a syscall from a protected task ran the handler still
      protected and its first putchar raised a nested trap. Fixed in
      c4m.c; C4KE is unaffected because its PM is compiled out.)
- [x] X4 shell (2026-08-03: src/c4ix/user/sh.c -- an ordinary
      userland program, protected mode and all, built from nothing
      but X2/X3 syscalls. Pipelines of any length, `<` and `>`
      redirection, `&` background jobs with `jobs`/`wait`, builtins
      exit/jobs/wait/cd/help, `#` comments, and operators that need
      no surrounding spaces (`cat f|wc` parses). Commands resolve
      bare names to c4ix-<name>.c4r; anything with a dot or slash is
      a path. Joined by cat.c, and echo/wc renamed from the u-prefix
      (that prefix belongs to libc4ix FUNCTION names, which must not
      collide with c4lc builtins -- program names should read like
      commands).
      There is still no fork. A stage runs by pointing the SHELL's
      own fd 0 and fd 1 at that stage's ends, spawning -- the child
      inherits the table -- and then restoring. init's X3 code did
      this by hand; X4 hands the job to userland, which is the point
      of the milestone. Two details worth keeping: a background
      command's argv must outlive the line that spawned it, since
      children read argv out of the parent's memory, so every spawn
      gets its own copy; and the shell's line reader buffers, because
      through the trap gateway a per-character read is a kernel round
      trip (the X2 lesson).
      `cd` deliberately reports that the filesystem is flat rather
      than pretending to succeed -- there are no directories yet.
      Pinned by src/c4ix/user/demo.sh, whose output is now identical
      on both hosts.)
- [x] X5 polish (2026-08-03: SYS_CYCLES and SYS_TASKINFO join the
      syscall set, with ps.c reading the task table one fixed-shape
      record at a time -- userland cannot walk kernel memory. bench.c
      carries C4KE's own pi/factorial workload verbatim so the
      compute numbers mean the same thing, plus microbenchmarks for
      the things C4IX actually costs. test.sh is the suite: the C4KE
      .c4r tests cannot run here (they are binaries for a different
      OS, built against u0.h and its custom opcodes), so the COVERAGE
      was re-expressed against C4IX's interfaces and is driven by the
      shell -- every line also exercises spawn, wait, pipe and dup2.
      KNOWN OPEN BUG, deliberately not papered over: a backgrounded,
      redirected command can intermittently stall when it follows
      about a dozen other commands. Adding any per-command output
      makes it complete, which is a scheduling race, not a logic
      error. Ruled out: descriptor exhaustion (saved fds stay at 4/5
      all run), per-command fd leaks, four-stage pipelines, cat on a
      missing file, truncate-and-rewrite, and background jobs in
      isolation. It is kept in src/c4ix/user/stress.sh, OUTSIDE the
      pinned suite, rather than pinned in whatever state passes.)

- [x] Directories (2026-08-04: the RAM filesystem became a tree. A
      vnode can be VN_DIR with child/parent links, names are single
      COMPONENTS rather than whole paths, and resolution walks one
      component at a time from either the root or the task's working
      directory -- which is what makes "." and ".." mean anything.
      Each task carries a cwd, inherited on spawn, so a child
      resolves relative paths where its parent stood. New syscalls:
      chdir, mkdir, getcwd, readdir (index-based, so userland never
      sees a kernel pointer). `cd` and `pwd` became real shell
      builtins -- they change the shell's own state -- while `ls` and
      `mkdir` are programs. cd's honest "this filesystem is flat"
      message is retired.
      One consequence worth stating: intermediate directories are NOT
      created implicitly, because open() does not do that in any
      Unix. `/ram` therefore exists from boot -- before directories
      it was simply one flat name among many, and paths like
      `/ram/out` would otherwise have stopped resolving.)
- [x] Preemption races fixed (2026-08-04, three of them):
      (1) the trap handler claimed its re-entry guard AFTER masking
      the cycle interrupt rather than before, so an interrupt landing
      in that window ran the whole non-reentrant handler recursively
      -- the source of the garbage program counters;
      (2) fd and vnode reference counts were mutated by preemptible
      kernel tasks without masking, so a preemption mid-update could
      free a description another task still held;
      (3) **the preemption mask was global rather than per-task**,
      and this is the interesting one. c4m zeroes the cycle interval
      whenever the interrupt fires and relies on the handler to
      re-arm it. The handler re-armed only when the mask depth was
      zero -- but the depth belonged to the MACHINE, so a task that
      held the mask across a context switch handed the next task an
      interrupt-masked machine. A compute-bound task landing in that
      state never traps again: it simply runs, burning cycles, while
      nothing else is scheduled and nothing can stop it. The depth
      now travels with the task, saved and restored by both switch
      backends, and the re-arm reflects whoever is about to run.
      The same shape exists in C4KE, which is where this was first
      observed: `critical_path_start/end` set the interval globally,
      and `ih_cycle` restores an `old_ih_cycle_interval` captured in
      the outgoing task's context. That is a plausible explanation
      for its long-standing "rogue task" -- one process with a cycle
      count thousands of times everyone else's, issuing no traps,
      starving the rest.
      Measured: the stress script went from 0/4 to 4/4 at five times
      the shipped preemption rate. At twenty times and beyond it
      still fails, but there the interrupt period is below the
      handler's own cost (~1237 cycles), which is outside the design
      envelope rather than the same bug.)
## 6.1 Running it

    make run-c4ix        an interactive shell on c4m
    make run-c4ix-c4     the same under plain c4, cooperatively
    make demo-c4ix       the guided tour: every milestone in order
    make test-c4ix       the pinned suite, both hosts
    make bench-c4ix      boot cost and the OS microbenchmarks

`run-c4ix` boots quietly (`-q`, which skips init's demonstrations)
and starts c4ix-sh with no script, so the shell reads fd 0 -- you. It
prints a prompt carrying the working directory; `help` lists the
builtins, and `exit` or end-of-file leaves, which shuts the kernel
down. Anything in the tree named `c4ix-NAME.c4r` is a command: echo,
cat, wc, ls, mkdir, ps, bench, hello.

## 7. Measured results (2026-08-03)

All C4IX figures are VM cycles from the machine's own counter, taken
in a single run, so they are exact and repeatable rather than
wall-clock estimates.

| C4IX boot | cycles |
|---|---|
| loading the kernel image (load-c4r.c, before any C4IX code) | 311,615 |
| kernel init: main to handing off to init | 12,994 |
| spawn + load + run the first user program | 18,859 |
| **total, power-on to userland** | **343,468** |

| C4IX operation | cycles |
|---|---|
| syscall (getpid through the trap gateway) | 1,237 |
| yield (a full context switch) | 1,237 |
| spawn + wait a trivial program | 15,005 |
| pi100 (same code as C4KE's bench) | 165,896 |
| pi300 | 592,056 |
| factorial(10), recursive | 1,393 |

**Against C4KE.** End to end -- boot a kernel and run one trivial
program -- C4IX takes under 10ms of wall time where C4KE takes 0.31s.
Most of that gap is not efficiency: C4KE spends ~200ms at boot
deliberately measuring the host's instructions-per-second, which
C4IX does not do. C4KE's own report is "Kernel ready in 201ms after
897.7k cycles" (801.7k for the c4lc-compiled build), against C4IX's
324.6k cycles to the same point -- 2.8x fewer, though 96% of C4IX's
figure is the shared loader reading the image, so the kernel's own
initialization is only ~13k cycles.

One honest caveat on methodology: a probe task calling
`__c4_cycles()` under C4KE returns a number (159k) that cannot be
reconciled with either C4KE's own accounting or the wall clock, so
that measurement is NOT used for the comparison above. C4KE appears
to account cycles per task rather than exposing the raw counter.
Cross-kernel claims here rest on wall time and each kernel's own
boot report, both of which agree.
