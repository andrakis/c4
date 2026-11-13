# C4IX — a Linux-like operating system, compiled by c4lc

C4IX is a ground-up successor to C4KE: written against c4lc's extended
language (structs and friends), built as separately compiled objects
linked by c4rlink, optimized end to end by `c4lc -O`, targeting c4m.
(It was first planned with a degraded plain-c4 mode via the c4l loader;
that was dropped on 2026-08-04 -- see 4.1. Under plain c4, C4IX runs
through c4m itself: c4 -> c4m.c -> C4IX, `make test-c4ix-c4`.)
The name is the user's, from an earlier rewrite attempt.

STATUS (2026-10-21): X0–X6 done, plus directories and the preemption
fixes (section 6). Since X6 C4IX has been built on the machine that
runs it (the ladder), gained a sleep syscall, `gui`, a Windows-style
desktop (docs/c4ix-desktop.md), a lazy disk, compilers that run inside
it, a root archive it rebuilds itself from, shared libraries and a
libc, and CPU accounting; "Since X6" at the end of section 6 lists
them. Sections 1–5 are the original design and keep its dates.
`src/c4ix/README.md` has the current build, run and test targets.

Everything here builds on docs/c4lc-design.md (the compiler) and
docs/internals.md §6.5 (the c4l loading system).

## 1. Why a rewrite

C4KE is written in the bare c4 subset: every kernel structure is an
int array indexed by enum offsets (`task[TASK_NICE]`), IO goes
straight to the host (PRTF prints to stdout, unredirectable), and the
whole kernel is one 3300-line translation unit compiled in one shot
(3300 lines when this was written; `src/c4ke/c4ke.c` is about 4,600
now).
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
- **C4IX targets c4m.** (Revised 2026-08-04.) It is not a plain-c4
  program and does not try to be: its images use extended opcodes
  (C4CY, PUTC) and indirect calls (JSRI/JSRS) that the base VM does
  not have -- `./c4 c4l.c c4ix.c4r` dies on "unknown instruction 52".
  The layer that degrades to plain c4 is **c4m itself**, which is a
  c4 program: `./c4 c4m.c load-c4r.c -- c4ix.c4r` runs the whole
  system under an unmodified interpreter, just slowly. C4LM is the
  project written for plain c4 directly; every kernel above it
  requires c4m.
  The kernel still detects its host via `__c4_info()`. The
  cooperative/no-protected-mode paths were kept for a while as the
  degradation story, never reached because c4m always reports itself;
  they were removed on 2026-10-21 ("Plain c4 support removed", end of
  section 6), and a host that is not c4m is refused at boot.

### 4.2 Milestones

- **X0 — boot.** Kernel of 3+ objects linked by c4rlink, boots on c4m,
  prints via its own kprintf, runs one init task, clean shutdown.
  Also booted under `./c4 c4l.c` at the time (degraded path proven
  early; dropped on 2026-08-04, see 4.1).
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
- **X6 — C4KE compatibility.** DONE, see 6.2. C4KE's own binaries run
  unmodified as C4IX tasks: its syscall ABI is a second door into the
  same trap gate, and answering it costs one module.

### 4.3 What gets reused

- c4m unchanged (traps, cycle interrupt, protected mode are enough).
- load-c4r's format knowledge; the in-kernel loader is new code
  against the same .c4r format.
- C4KE's hard-won lessons (documented in internals.md): trap design,
  the idle-task discovery dance (C4IX will pass the idle address
  explicitly instead), RAM-FS opcode interface if kexts are wanted.

### 4.4 Priority: nice (2026-10-23)

Scheduling was a plain round robin: equal slices, and a task that woke
from a sleep waited for whatever ran to finish its slice. Under C4DOS or
on the bare machine a game has the machine to itself; inside C4IX, top,
the shell and persist each took whole turns from it. So a task now has a
**nice**, -10 (most favoured) to 10, and 0 for most. While every task has
0 the scheduling is exactly the round robin it was.

A favoured task is served **sooner**: when it wakes, from a sleep, a pipe
or a wait, it preempts a less favoured task at the next trap. It does not
wait out that task's slice (sched.c, `sched_waker`). It is also served
**longer**: a turn of 1 - nice slices in a row (`sched_turn`).

The turns still go round, so a favoured task that never sleeps cannot
starve the rest. That matters most for a game in a window, because the
desktop is what draws it. Strict priority was rejected for exactly that
reason. A positive nice is the other way: such a task is preempted by
any less nice task that wakes.

Children start with their parent's nice. `SYS_NICE` (243, `unice` in
libc4ix) sets and reads it. `nice` is the command:
- `nice -n -5 cc big.c` runs a command at a given nice, and Unix's
  `nice -5`/`nice --5` spellings work too;
- `nice -p PID [N]` reads or changes a running task's.

Wolf3D, Doom and Tyrian ask for -5 themselves on C4IX. On one CPU, a
spinner at -3 beside one at 0 gets 4.6 times the cycles
(test-c4ix-smp.sh checks for more than three).

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
      c4rlink into c4ix.c4r; boots natively on c4m and (then)
      degraded on plain c4 via the c4l loader, pinned exactly by
      `make test-c4ix` against src/c4ix/tests/. Host detection via one
      INFO opcode (0 = plain c4, true only while c4.c carried extended
      opcodes; reverted in 29d6f57, and the c4l route went with 4.1's
      revision). Console is kprintf over PUTC only. Tasks are a
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
      the shipped preemption rate.
- [x] Preemption race four, and the end of the stall (2026-08-05):
      **a trap handler cannot re-arm the interrupt from inside
      itself.** It does not own the registers -- TLEV installs them,
      several instructions after the handler's last statement. So
      between `sched_cur = n` and that TLEV the scheduler said one
      task was running while the machine still ran another, and an
      interrupt landing in the gap saved the outgoing registers into
      the incoming task and restored the incoming task's stale ones.
      The contexts swapped; one then built a trap frame over the
      frame the other was parked on, and the machine spun forever on
      a TLEV whose frame returned to itself. Caught under gdb as a
      self-referential frame at `tlev_instruction`, then reproduced
      exactly with a trap/TLEV ring buffer in a scratch VM.
      The fix is in the VM, because the window cannot be closed from
      the kernel: c4m's new `CONF_TRAP_RESTORES_INTERVAL` makes the
      interrupt mask part of the trapped context, saved at trap entry
      into frame slot bp+9 and restored by TLEV. C4IX opts in and
      declares an eighth leading parameter on `sched_trap`, so it
      sets preemption for the INCOMING task by writing that slot; the
      machine stays masked until the switch is complete. c4m also
      refuses to interrupt a TLEV instruction at all, which covers
      kernels that have not opted in. Either alone fixes it; both are
      in. Handlers with the usual seven parameters -- C4KE's -- are
      untouched, because C4 numbers parameters down from the top of
      the frame.
      Measured: 4/4 at twenty times the shipped preemption rate, so
      the earlier reading of that failure as "below the handler's own
      cost, outside the design envelope" was wrong; it was this bug.
      The `spin 1`-from-the-shell hang and the `ps -s` hang are both
      this, and both pass now.)
- [x] Console input without stopping the machine (2026-08-05):
      a read on fd 0 blocks the HOST, and the host is the whole
      virtual machine -- every task stopped while one of them waited
      for a keystroke. console.c now opens a second descriptor on the
      same terminal with O_NONBLOCK (c4sh's technique, and it works
      on both hosts: on a delayed pipe, native c4m spun two million
      times without blocking and the c4-hosted chain a quarter
      million). vfs_readable answers for the console the same way it
      answers for a pipe, so a task waiting for input parks in
      TS_BLOCKED and rides the existing restartable-syscall path;
      nothing new was needed in sys.c.
      Two things fall out of it, one good and one that had to be
      paid for. The good one: Ctrl-C works at an idle prompt, since
      the VM is no longer parked inside a host read. The cost: the
      idle loop now really is a loop, so it backs off with an
      escalating nap (100us doubling to 20ms) and the machine sits at
      0% CPU at a prompt; and polling costs a host read() every time
      the scheduler asks, once per blocked reader per scheduling
      decision, so console.c rate-limits the actual read to one per
      100k virtual cycles. Virtual cycles stop during the nap, which
      is why the idle loop calls con_wake() to reopen the gate.
      Two signal bugs surfaced with it, both real and both older
      than the change. c4m delivered a pending signal regardless of
      the interrupt mask, so a Ctrl-C landing while the kernel was
      inside its own trap handler hit the handler's re-entry guard
      and was DROPPED; signals are now deferred while masked (for
      kernels that opted into trap_restores_interval, since only they
      have told c4m that a zero interval means masked). And Ctrl-C
      cancelled "the current task", which stopped being the right
      answer once the idle loop ran alongside user work. C4IX has no
      process groups but does not need them: a shell waits on the job
      it is running, so the foreground task is the user task that
      another USER task waits on -- init waits on the shell the same
      way, which is exactly why the waiter must be userland. A '&'
      job has nobody waiting on it and is left alone, and a Ctrl-C
      typed while any shell sits at a prompt cancels nothing at all.)
- [x] X6 C4KE compatibility (2026-08-05): C4KE's binaries -- ps, top,
      spin, bench, benchtop, innerbench -- run UNMODIFIED as C4IX
      tasks, including innerbench's nested-kernel mode. See 6.2 for
      the design, the measurements and the parts that were not
      obvious. Pinned by `make test-c4ix-c4ke`.
      Two changes to C4IX proper came out of it and are worth having
      on their own. printf became real in both implementations, which
      is what `make test-c4ix-fmt` now pins by driving one format
      string through the userland formatter AND the trapped-PRTF path
      and comparing. And the idle loop reaps: a zombie used to be
      released only by whoever waited on it, so a parent that never
      waits -- innerbench spawns several benches and can only wait on
      one -- left them on the list for the whole run. Only children of
      USER tasks are swept, and that is not caution: kernel code waits
      by POINTER and holds those pointers across waits, so sweeping
      init's second child while it waited on its first handed
      task_wait a dangling pointer. Userland only names a task by id,
      and a reaped id is answered from a 32-entry table of recent exit
      statuses.
      The shell gained `kill [-SIG] %JOB|PID` at the same time, since
      `jobs` could list a background task and nothing could stop it.

### Since X6 (recorded 2026-09-30; details in the commits named)

- **c4r v3** (78c3601, 2026-08-09): the loader reads BSS and data of
  any size (no 256 KB cap).
- **Built on the machine that runs it** (36b58b8, 2026-08-23, and the
  climb after it): c4rlink reads and writes the RAM filesystem, so C4KE
  compiles C4IX's twelve modules and links them in-machine
  (`c4ix-src.tar`, `b4ke -f c4ix.b4k`). docs/ladder-plan.md,
  docs/climbing-the-ladder.md; `make test-c4dos-c4ix32`.
- **Cycles in two words** (f5c82a1, 2026-08-28): `sched_add_cycles`
  keeps billions and the rest, so `ps`/`top` stop printing negative
  time on long runs.
- **c4th on C4IX** (664ffdb, 2026-08-25): the loader looks in the RAM
  filesystem (docs/c4th-design.md, B6).
- **libc4ix declines under C4DOS** (fb6bf7a, 2026-09-06) instead of
  trap-storming, using the `__c4dos_api` patch as the probe.
- **`SYS_SLEEP`** (220) and `gui` (5493bec, 2026-09-23): a task parks
  on the clock (`umsleep`); `user/gui.c` is the libjs display demo as a
  C4IX program (natively it prints `gui: not fitted`).
- **The desktop** (a38e63a, d5a9ff4, 2026-09-23): `c4ix-desktop.c4r`,
  six modules in `src/c4ix/user/desktop/`, with `SYS_AVAIL`,
  `SYS_INTR`, `SYS_CLOEXEC`, `SYS_STAT`, `SYS_UNLINK` and `SYS_RENAME`
  added for it. docs/c4ix-desktop.md is its design and tracker.
- **A lazy disk** (3f3bd65, 2026-09-24): `SYS_LAZYFILE`; vfsload makes
  each manifest entry a RAM file the kernel reads in on first use, and
  Explorer runs programs because `stat` says which files are programs.
- **Compilers inside C4IX** (e427c9c, 2026-09-24): the compatibility
  layer answers C4KE's RAM-filesystem opcodes and reports a trap
  handler, so c4cc, c4rlink and c4lc (on c4sp) write what they build
  into `/ram`; c4th, cpp and c4rdump are on the disk beside them, with
  `/usr/src/examples`. The loader now prefers a RAM file to the host
  file of the same name. `include/window.h` lets a program in a desktop
  Terminal use that window as a canvas.

- **The root archive** (branch c4ix-userland, 2026-10-19):
  `c4ix-root.tgz`, a reproducible gzipped ustar of the repo's text files
  (622 of them, 5.5 MB, 1.57 MB packed) plus `/usr/lib` and
  `/usr/include`, built by `build-images.sh`. `ARCHIVES` in
  `c4ix.vfs.txt` names it; vfsload starts `c4ix-tar x -C /` on it in the
  background and prints the task id. On a lazy web disk that is one
  request for all of it. libarc (`lib/archive.c`) is the inflater and
  tar reader, built `-mfuse` for the disk: about 530M instructions for
  the whole archive, 5.9 s under node. With it: `SYS_SPAWN` copies argv
  into the child (`ck_argv_copy`, which the compatibility layer already
  had), because vfsload exits before tar reads its arguments; RAM-file
  and pipe reads and writes copy with `memcpy`, not a byte loop (about
  a quarter of the unpack's cost); `VN_NAME_MAX` is 64 and `PATH_MAX` 256;
  Explorer holds 400 entries a folder and 256 folders ten deep; the
  shell's `wait` takes a task id.

- **Rebuilt inside itself** (branch c4ix-userland, 2026-10-19): C4IX
  runs `b4ke -C /usr/src/c4` over the root archive's tree and rebuilds
  the kernel, libc4ix, libarc, every userland program, the desktop and
  the c4cc-built toolchain into `/ram/build`. All 24 c4lc-built images
  come out byte for byte the host's. `user/b4ke.c` is a native port of
  C4KE's b4ke with B4KE2's verbs; DEPS needs a modification order, so
  vnodes carry `mtime` (a counter every create and write advances, out[3]
  of `stat`). c4sc needed more stack than C4IX's 64 KB, and running off
  the end took every task down, so a task stack is 256 KB (C4KE's), the
  lowest 64 words are poison checked at every switch (an overrun kills
  that task by name), and `SYS_STACK` (228) sets the stack a task's
  later spawns get -- b4ke's `$(STACK)`, 1 MB for c4sc. The shell tries
  `/bin/NAME` before `c4ix-NAME.c4r`, so `INSTALL.B4K`'s copies win.

- **Already built** (2026-10-21): the root archive ends with `/build`
  (then `/ram/build`) as `b4ke -C /usr` leaves it -- the host's own objects and images
  from build-images.sh, the same bytes -- so b4ke (and the IDE, which runs
  it) starts with all 86 steps up to date and an edit remakes only what it
  feeds. It works because mtime is the RAM filesystem's write clock: the
  archive puts those files after every source, objects before the
  libraries made from them and libraries before what links them. They go
  in the same archive because vfsload starts one task per archive, and
  two would unpack at once. The archive grows from 1.9 MB to 2.6 MB and
  its unpack from 5.3 to 8.2 machine-seconds (`wait 4`).

- **The repo is /usr; builds go to /build; no /ram** (2026-10-21). The
  archive unpacked the repo at `/usr/src/c4`, while `/usr/include`,
  `/usr/src/bin`, `/usr/src/examples` and `/usr/src/forth` held copies
  of some of the same files, so the same source had two names and a C4
  program's `#include <u0.h>` found nothing. Now `/usr` is the repo:
  its `src/` is `/usr/src`, its `include/` is `/usr/include` beside
  C4IX's own headers (no name is in both), and its docs, `Makefile` and
  `BUILD.B4K` are `/usr`'s; the BUILD.B4K files' `ROOT` is `/usr`. The
  manifest no longer makes the copies. `b4ke -C /usr` builds into
  `/build`. The whole tree is writable, so C4IX has no `/ram` -- that
  is C4KE's and C4DOS's -- and the kernel makes `/tmp` at boot for
  scratch files instead (init's demo, test.sh, cc's objects, the IDE's
  one-file builds, which land in `/tmp/NAME.c4r`).

- **`/proc/uptime`** (2026-10-26): the kernel's clock as a file, in the
  format c4m.c reads from a Linux host when plain c4 runs it
  (`c4_time_host`). So `c4 c4m.c` has a clock inside C4IX, and
  `innerbench -c` (src/bench) runs here. Two numbers, seconds with two
  decimals: the time since the VFS came up, then, where Linux has its
  idle time, the same clock in tenths of a second, because c4m's
  default reading takes that number's integer part times 100 as
  milliseconds (`c4m -a` reads the first). A `VN_PROC` vnode
  (`vfs.c`, `vfs_proc`): its bytes are made when it is read from its
  start, stat'd or fetched with C4KE's `OP_VFS_GET`. It can be read and
  sought, but not written, truncated, linked, renamed or removed, and
  `persist` never keeps `/proc`.

- **`cc`** (8b01e1a, 955bc82, 2026-10-21): `src/c4ix/user/cc.c` runs
  c4sc with c4lc's preprocessor and `/usr/include` on the include path,
  on a task's default stack (1 MB at first, then 4 MB, until 2026-10-25:
  c4lc's g:resolvecode recursed once per instruction, fixed in 29cbac9;
  c4sc needs 11 KB at most now), and links with c4rlink when given `-l`
  or more than one source. It defines `C4CC` and `__c4cc__`, as the
  Makefile does, so the repo's own programs (`/usr/src/tests/mandel.c`)
  build as they are. src/c4tui/demo.c takes 18 machine-seconds with
  it, against 344 with `c4sp c4lc.lisp`. The IDE's `:make` uses the
  nearest BUILD.B4K that names the file, and has cc build a `.c` file
  none names. Since c4lc has `float` and `double` (docs/c4-float.md),
  so has cc.

- **c4m inside C4IX** (409e8c0, 2026-10-21): `/bin/c4m` is c4m.c built
  for the VM, lending its guest the outer machine's `FLT`, and the disk
  carries C4KE's kernel as `/usr/lib/c4ke.c4r`, so `cd /usr; c4m
  load-c4r.c -- /usr/lib/c4ke.c4r` boots C4KE to its shell as a C4IX
  task, and `\q` comes back. docs/c4-float.md; pinned by
  `libjs/tests/test-c4ix-c4m.mjs` in `make test-libjs`.

- **Shared libraries and a libc** (branch c4ix-userland, 2026-10-19).
  - *Linking:* `c4rlink -l LIB` links against a library without copying
    it in. The output keeps those symbol references and records
    `/lib/<base name>` in a static `__c4_needed` global (a symbol, so no
    `.c4r` reader needs to change).
  - *Loading:* C4IX's loader (`loader.c`) loads every library a program
    needs, breadth first and each once. It resolves the symbol
    references against the libraries' exports, and runs library
    constructors before the program's. A missing library or symbol fails
    the spawn by name.
  - *D1 (any library):* each task gets its own copy of the library, code
    and data.
  - *D2 (`-fpic` libraries):* the code is loaded once and cached, and
    each task gets only a copy of the data. c4lc's `-fpic` reaches every
    datum as `IMM off; PSH; IMM __c4_dbase; LI; ADD`. `__c4_dbase` is a
    word the loader gives the library, and the scheduler points it at
    the incoming task's data at every switch (`loader_switch`).
  - *Measured* on libc (18,088 code words, 1,236 bytes of data): per
    task, libc costs 1.2 KB instead of about 73 KB.
    - A first `libctest` run costs 3.42M cycles, against 3.35M with a
      non-pic libc loaded whole.
    - Later runs, from the cache, cost 3.15M, despite the four extra
      instructions per data reference.
    - Resolving `__c4_dbase` by id once per module, not by name per
      patch, is what keeps the first load level.
  - *Restriction:* a `-fpic` library may use other shared code's
    functions, not its globals; the loader refuses otherwise.
  - *Pins:* `pictest` runs two tasks at once, which must keep their own
    `errno`, environment and stdout. With the switch disabled, task A
    reads B's `TAG` by its third iteration.
  - *libc:* `/lib/libc.c4s` (`src/c4ix/libc`) is the first `-fpic`
    library, built with c4lc's new `-fno-builtin`. `ls` was linked
    against a non-pic `/lib/libc4ix.c4s`; since "code out of the headers"
    below, every program but two loads `-fpic` libraries.

- **CPU accounting** (branch c4ix-userland, 2026-10-21): docs/c4ix-cpu.md.
  The trap handler charges cycles to the task they were spent on, as its
  own or as the kernel's on its behalf. The counts are 64-bit (`long
  long`), and the idle loop's naps are counted in machine ms.
  `SYS_CPUSTAT` (229) is one atomic snapshot, read through
  `include/cpustat.h`. System Monitor and top show System and System Idle
  Process rows, and the rows add up to 100%.

- **Plain c4 support removed** (2026-10-21, docs/c4ix-cpu.md step 10):
  the cooperative backend, the systable door and `host_type()`. A host
  without c4m's traps is refused at boot.

- **`SYS_EVWAIT`** (230, 2026-10-21, docs/c4ix-cpu.md step 11): sleep
  until a deadline, an event in a GUI ring, or data on a pipe the caller
  reads. The desktop's loop waits on it, and redraws only damaged
  regions.

- **`SYS_DISPLAY`** (240, 2026-10-23, docs/c4ix-desktop.md part
  thirteen): who has libjs's display. The desktop holds it and gives way
  to a program that asks (`include/display.h`). It has the display back
  when that program gives it up or dies.

- **`SYS_DISCARD`** (231, 2026-10-21): drop what a pipe holds unread,
  from either end, as a tty drops its input on Ctrl-C. A Command
  Prompt does it on Ctrl-C, so lines typed at a job that never read
  them are not run by the shell afterwards. In the same change,
  `/dev/stdin` and `/dev/tty` name a task's own stdin when that is a
  pipe (C4KE's shell under c4m in a Terminal polls them), and a
  read on a non-blocking descriptor with nothing yet returns -1.

- **`SIGH` guarded** (2026-10-22): `__c4_signal` sets the machine's
  handler for a signal, which in C4IX is the kernel's own. A protected
  task that ran it replaced that handler with an address of its own,
  and the next Ctrl-C trapped into it, after the task had exited too.
  c4m, c4mpg, libjs and the board now trap it in protected mode, as
  they do USLP, and `sys_pmviolation` makes it the task's own handler
  (`ck_signal`, as a C4KE program's `signal()` does); C4KE's PM
  extension does the same. `libjs/tests/test-c4ix-sigh.mjs`.

- **`ITH` guarded** (2026-10-22): `install_trap_handler` sets the
  machine's trap handler, C4IX's own. `c4ke` typed at C4IX's shell
  started C4KE's kernel as a protected task, and its first act took
  that handler: every trap of every task went to C4KE's early handler,
  which serviced none of them. Under c4m, which runs its guests on raw
  addresses, C4KE then cleared and wrote text through its failed
  allocations into the TLEV word at 0x10, and the whole machine died
  ("illegal opcode 541148492", the bytes "LLA "). c4m, c4mpg, libjs and
  the board now trap ITH in protected mode, and `sys_pmviolation` kills
  the task, saying why. C4KE's PM extension hands the machine over, as
  the unguarded opcode did. libjs also drops guest stores to its ROM
  words (0x00-0x1F and the opcode names) and names the first on the
  console. `libjs/tests/test-rom.mjs`, `test-c4ix-c4m.mjs`.

- **Code out of the headers** (2026-10-22, asked for: "header files often
  have entire libraries worth of code in them").
  - *Where the code went.* The three headers of C4IX's own that held
    functions now hold their declarations only:
    - `libjs/guest/gui.h` (the display, 40 functions) has its code in
      `gui.c`;
    - `src/c4ix/include/window.h` (a Terminal's canvas) has its code in
      `window.c`;
    - `cpustat.h` has its code in `cpustat.c`.
    Each `.c` lies beside its header, and the header includes it unless
    `C4IX_SHARED` is defined. A program built as before therefore still
    builds: `cc prog.c` with `#include "window.h"` works unchanged. c4cc
    has no `#include`, so it is given the `.c` as well, as it is given
    the header (`c4cc gui.h gui.c gui-demo.c`). `/usr/include` has both.
  - *The shared libraries.* They are built `-fpic`, so their code is
    loaded once for every task:
    - `/lib/libc4ix.c4s` is `libc4ix.c` with `window.c` and `cpustat.c`.
      It used to be the static library's image under another name.
    - `/lib/libarc.c4s` is libarc (inflate, CRC-32, tar).
    - `/lib/libgui.c4s` is the display.
  - *The programs.* C4IX's programs are compiled `-D C4IX_SHARED` and
    linked `-l` against those libraries (build-images.sh, and the
    BUILD.B4K files that rebuild them inside the machine). The exceptions
    carry their libraries themselves:
    - `vfsload` runs before there is a `/lib`;
    - `splash` runs while the boot unpacks the root archive.
    `gui.h`'s globals are `gui.c`'s own. A program asks for them with
    `gui_width()`, `gui_height()` and `gui_events_ring()`.
  - *What it saved:*

    | program | before | after |
    |---|---|---|
    | `tar` | 59,666 bytes | 4,669 |
    | `crc32` | 58,893 | 3,896 |
    | `cat` | 25,504 | 2,501 |
    | `b4ke` | 58,876 | 35,873 |
    | `cc` | 45,606 | 22,603 |
    | the desktop | 911,844 | 836,799 |

    The root archive went from 2.90 MB to 2.82 MB compressed.
  - *Not moved:* the repository's own `include/` (`u0.h`, `c4ke.h`, the
    C `stdio.h` and the rest, also in C4IX's `/usr/include`) is for
    programs that run on C4KE or bare c4m, which load no libraries. Code
    in those headers is the only way they can share it.
  - *Memory.* A cached library keeps its file for as long as the machine
    runs, as the template of each task's copy of its data, and the loader
    read every image into a 4 MB buffer. With every program loading
    libraries, those buffers were a third of the heap. An image's buffer
    is now its own size: a RAM file is copied, and a host file, whose
    size the host does not say, is counted in 64 KB chunks and then read
    into a buffer that size. The heap in use once the boot has unpacked
    the root archive went from 24.6 MB to 16.6 MB.

- **Persistence** (2026-10-22, asked for: "browser storage persistence for
  filesystem changes"). C4IX's files are RAM, and every boot starts from
  the disk and the root archive. What changed now lasts, where the host
  gives the machine somewhere to keep it: host drive 1. In the browser
  that is IndexedDB, kept by the page; on the command line it is the
  directory `cli.js -w` names.
  - *`persist`* (`src/c4ix/user/persist.c`) does all of it, in userland:
    - `persist boot` is now the boot's unpacking task (vfsload runs it
      where it ran `c4ix-tar`, so it is still task 4). It unpacks the
      archives, writes every path there is to `/var/persist/base`, then
      lays the last session's saved changes over the tree;
    - `persist save`, or `sync`, writes the changes to the drive;
    - `persist status` lists them, and `persist forget` saves nothing, so
      the next boot starts clean;
    - `persist -d SECS` saves every SECS seconds while things change. The
      desktop's boot runs it every minute, and Power Off saves before the
      machine stops.
  - *What a change is.* The stamp on `/var/persist/base` (the VFS's order
    of changes, `ustat` out[3]) is the epoch. A path not in the base list,
    or stamped after the epoch, has changed. A path in the list that is
    gone was deleted. The saved changes are laid over the tree after the
    list is made, so the next save counts them as changes again and keeps
    them. `/tmp`, `/proc`, `/var/log` and `/var/persist` are never kept.
  - *What is kept* is one ustar archive, `1:c4ix-persist.tar` (since
    2026-11-02 that or `1:c4ix-persist-b.tar`, below): the changed
    files in the order they were changed, so that b4ke still sees an
    object as newer than its source, then `var/persist/whiteouts`, the
    deleted paths, which the next boot deletes again. It is streamed to
    the drive through c4bb's disk write registers (`DISK_WNAME` and the
    rest, 0x178-0x184), a 64 KB chunk at a time. The host replaces the
    file only when it is closed, so a save cut short leaves the last one.
    The registers are announced only by libjs (`C4I_C4MJS`), and `persist`
    does nothing on any other host.
  - *The shell* gained `rm` and `mv`, over the `uunlink` and `urename`
    syscalls that Files and the IDE already used.

- **A task's memory goes with it** (2026-10-22). `SYS_SBRK` (libc4ix's
  `ualloc`) was a bare kernel `malloc`, and so was `malloc` behind
  protected mode (`C4IX_OP_MALC`). Whatever a program did not free stayed
  allocated after it exited, and most programs free very little. A
  `b4ke -C /usr` build inside C4IX climbed towards the top of the 64 MB
  arena. The table shows the heap left in use by each further run of a
  command from the shell (32-bit image under libjs: `machine.heap.used`
  after `wait 4`, from the second run on, so caches already filled are
  not counted):

    | command | before | after |
    |---|---|---|
    | `cc -o /tmp/h hello.c` | 5,264,890 bytes | 0 |
    | `crc32 /bin/desktop` | 1,567 | 0 |
    | `ls /usr/src` | 536 | 0 |
    | `/tmp/h` | 15 | 0 |

    The boot itself keeps 220 KB less, from what `vfsload` and the
    unpack left behind. The last few bytes were the shell's own: it kept
    a copy of every command's argv, from before `SYS_SPAWN` gave the child
    a kernel copy. Now only a background job keeps its copy, and `wait`
    frees it.
  - *Most of cc's 5 MB was not SBRK.* It was c4sc, which cc runs: its
    4 MB stack (a task's default 256 KB since 2026-10-25), its code and
    data, and its argv, which `task_destroy`
    asked to free on every run and which were never freed. A user task
    whose `main` returns dies in `task_shim`, in its own context:
    `task_exit` yields until the trap handler takes it, and `sched_yield`
    raised that trap with `__c4_trap`. That opcode does not drop
    protected mode, on c4m or on libjs, so the handler ran protected. When
    cc exits, that handler reaps c4sc, which died a moment earlier. Every
    `free` in `task_destroy` then became a nested trap, and the re-entry
    guard at the top of `sched_trap` drops nested traps. A user task now
    yields through the syscall gate (`SYS_YIELD`), whose trap is
    unprotected. The first `cc` after boot now leaves 2.1 MB in use,
    the loader's caches, where it left 7.4 MB.
  - *How.* `task_ualloc` (task.c) puts a four-word header in front of
    each block: next, a back link to the word that points at the block,
    the size, and a magic number. That links the block onto the task's
    `ublocks` list. `free()` behind protected mode is `task_ufree`, which
    unlinks the block. A pointer without the magic is freed as before.
    `task_destroy` frees whatever is left on the list, next to the image
    and library data.
  - *Only protected tasks.* An unprotected task runs `FREE` on the host
    itself, which would be handed the block's middle, so its `ualloc` is
    still a plain `malloc` that it keeps.
  - *Nothing else holds a task's memory.* A child's argv is the kernel's
    copy (`ck_argv_copy`). Pipes, files and the console copy what is
    written to them. The exception is libjs's display, whose host writes
    events into the ring the task attached. A task killed while it is
    attached never detaches, so `task_destroy` clears `GUI_RING` when the
    ring lies in a block it frees. (A ring in a program's static data, as
    `raycast` has, is still the program's own business.)

- **POSIX processes and files, for Busybox** (2026-10-22, M7c of
  docs/busybox-port.md). Eight syscalls, 232–239, with libc's wrappers;
  `src/c4ix/libc/tests/unixtest.c` tests them all in `make test-libjs`.
  - *`SYS_VFORK`* (`sched_vfork`). The child borrows the parent's image,
    library data and heap; the parent parks in `TS_VFORK` until the child
    execs or exits, and then its vfork returns the child's id. The child's
    `SYS_SBRK` and `malloc` allocate for the parent, since libc's allocator
    lives in the data they share.
    - *Not the parent's stack.* On a register machine a vfork child runs on
      the parent's stack, below the frame that called vfork. On this stack
      machine that frame's caller F keeps its expression temporaries
      there -- in `pid = vfork()`, the address of `pid` -- and the child's
      first call would overwrite them and vfork's return address. So the
      child gets a stack of its own, and the trap returns it from vfork
      straight into F: pc is vfork's return address, bp is F's frame in the
      parent's stack, and sp is in the new stack, over a copy of what lay
      between vfork's frame and F's. F's locals are therefore the parent's
      own, as vfork promises (Busybox's `spawn()` reads a `failed = errno`
      the child wrote), and nothing the child calls touches the parent's
      stack.
    - libc's `vfork` must therefore execute the syscall itself, through no
      helper (it is libc4ix's, `__c4_opcode` direct). The usual rules
      hold: the child leaves by `execve` or `_exit` and never returns from
      F.
    - A vfork child gets every descriptor, close-on-exec ones too.
      Killing the parent kills the child, whose memory it was.
  - *`SYS_EXECVE(path, argv, envp)`*. The image loads first, and argv and
    envp are copied, so a failure is -1 with nothing changed. Then the
    task drops its image, libraries, heap and signal handlers, closes its
    close-on-exec descriptors, and keeps its id, descriptors and
    directory. The trap resumes it from a freshly forged frame
    (`sched_reload`) rather than from its trap registers, and the old
    stack, which the handler is standing on, is freed at the next trap.
  - *`SYS_ENVIRON`*. The kernel keeps the environment execve passed
    (`env_vec`), and a spawned task gets a copy of its spawner's. libc's
    `getenv` reads it in on first use.
  - *`SYS_WAITPID(pid, st, options)`*: a child, or any child (-1), with
    `WNOHANG`; `st` gets the exit code and the signal that killed it
    (`killsig`, now kept for `kill` and Ctrl-C). A blocking waitpid parks
    and runs again when a child dies, so -1 needs no bookkeeping: the
    waiter is woken, not handed a child. A task that has used vfork or
    waitpid (`posix`) is left its zombies by the idle sweep, and a child
    swept before its parent asked leaves a ghost with the parent's id.
    Ctrl-C follows a `TS_VFORK` parent, or a waitpid(-1), to the newest
    live child.
  - *`SYS_LSEEK`* moves a RAM file's position, past the end too (the gap
    reads as zeros); anything else is -1, libc's `ESPIPE`.
  - *`SYS_FSTAT`*, and *`SYS_STAT`*'s third argument: how many words of
    the `ST_*` record (c4ix.h) to write, 0 for the four there always
    were. Added: the vnode's number (`ino`), its link count, a made-up
    mode (0755 for directories and programs, 0644 for files) and a
    device. libc's `struct stat` keeps its first four members where they
    were.
  - *`SYS_LINK`*: a second name, a `VN_LINK` entry in a directory that
    points at the file. A path that ends at one resolves to the file.
    Unlinking a file's own name while it has links moves the vnode into
    its first link's place, under that name, so open descriptors never
    notice. Only files: a directory with two parents would give `..` two
    meanings. `persist` keeps each name as a file of its own.
  - *`SYS_FTRUNCATE(fd, len)`*, and libc's `truncate` over it; and real
    `O_APPEND` and `O_EXCL` in `sys_open`, where libc used to read a file
    in and write it back to append.
  - *A spawn race, found on the way.* `task_spawn_priv` put the task on
    the run list (`task_create`) before giving it its image and its
    privilege. A kernel caller, init, can be preempted there, and then the
    child ran as a kernel task, unprotected, its printf the host's own:
    x5-c4m.txt's hello said "after 0 syscalls", once the kernel's code
    had shifted the preemption points onto that window. The setup now
    runs under `sched_lock`.

- **The kernel log** (branch ix-mmu, 2026-10-29). Every line the kernel
  says goes into a 64 KB ring, stamped with the machine's time the way
  Linux stamps its log (`[    0.012345]`). `dmesg` prints it, and
  `/proc/kmsg` is the ring as a file. The boot fills it with what it finds
  (`boot.c`, `boot_report`): the host's capabilities, the clock it
  calibrates, the kernel image's segments (libjs's loader gives them, in
  devices.js IMG_*), the drives and devices, the syscall table, each SL4B
  cache, the scheduler, the CPUs, each task the boot spawns and its exit,
  and how long the boot took. On libjs they show on the console too while
  the boot runs, until the console is the shell's (`klog_boot`), unless
  the kernel was booted with `-q`; after, the new lines go only to the
  log, unless it was booted with `-v`.

- **An MMU** (branch ix-mmu, 2026-10-29): docs/c4ix-mmu.md. When the host
  fits one (cli.js and the tests do unless `--no-mmu` or `mmu: false`;
  C4IX's page does unless `?mmu=0`, but for the GitHub Pages build, whose
  `libjs/web/site.js` turns it off), user tasks
  run translated, each in an address space of its own, at user addresses
  from 0x10000000 that are not physical ones. A store into the kernel or
  into another task, or a null pointer's read, is a page fault that kills
  only the task that made it, and says so in the log; so is a device
  given an address that is not the task's (an IOMMU's check). Stacks have
  guard pages, text is read-only, data cannot be run, and c4bb's device
  registers are for a task that asks (`SYS_IOPERM`, 244). When memory runs
  out, idle tasks' pages go to the host's swap (up to 256 MB beyond the
  arena), and come back a few at a time when they are touched; a task
  another CPU is running forgets them first, by IPI. A program may borrow
  a task's memory it started read-only (`SYS_MAPFROM`, 245), as the
  desktop does to draw a window's canvas. The kernel runs untranslated,
  reaching user memory through its map of it, and every other system is
  untouched.
  `libjs/tests/test-c4ix-mmu.mjs`, `test-c4ix-swap.mjs`.

- **The root archive, read where it is kept** (branch lazy-root,
  2026-10-30, asked for: "If the unpacked files are persisted, surely we
  can remove them from ram as they are saved to browser storage?"). Persist
  already kept the unpacked root archive on drive 1, as
  `1:c4ix-base.tar`, and every boot then untarred that copy into RAM:
  1,178 files, 16.5 MB, there for good: `/usr/src` 5.3 MB, `/build` 7 MB,
  `/usr/docs` 1.5 MB, `/usr/libjs` 1 MB. Swap could not help: it puts
  out idle tasks' pages, never RAM files.
  - *A span of a host file.* The host opens `NAME@OFF+LEN` as LEN bytes of
    NAME from OFF (`Devices.diskOpen`, docs/c4bb-storage.md), where no
    file has that whole name: a read at an offset, with no new register.
    libjs announces it (`C4I_C4MJS`, `vfs_spans`); native c4m has none.
  - *The tree, lazy.* `persist boot` no longer untars the kept tar. It
    reads its headers, a span each, and makes every file a lazy file
    (`SYS_LAZYFILE`) on its span of the tar, `1:c4ix-base.tar@OFF+LEN`,
    and every directory. Nothing else is read. A lazy file now takes the
    next modification stamp, as writing it would, so the tree is stamped
    in the tar's order as untarring it was, and b4ke finds `/build` as
    up to date as before. The first boot inflates the archive straight
    onto the drive, a 256 KB window at a time (libarc's new
    `arc_gunzip_to`, whose sink is a function: c4lc calls any variable),
    and then does the same, so no boot holds the tree in RAM, and the
    first holds the 4 MB archive and the window, not 4 + 16.5 + 16.5 MB.
    With no drive (`cli.js` without `-w`, a browser that refuses storage),
    or a drive that will not take the tar, the archive is unpacked into
    RAM as before.
  - *Reading.* `vfs_read` of a lazy file on a host with spans takes what
    it asks for from the host, at its offset; a file of 64 KB or less
    (`VFS_KEEP`) is read in whole and kept while memory is not short. The
    loader reads a lazy program from the host straight into the image's
    buffer (`vfs_pread`), so a program run is not kept as well. A write,
    a truncate or C4KE's `OP_VFS_GET` (which hands out the bytes in place)
    reads a file in. `stat` says whether a lazy span is a program by its
    first three bytes, read once (`vn->sig`), not by its name.
  - *The cache.* A file read in whose bytes are still the host's
    (`vn->backed`: lazy, or read in and not changed since) is on a list,
    most recently used first. `vfs_trim` drops files from its tail, their
    bytes freed and the files lazy again, while 7/8 of the heap is in use
    (where the host says how much is, `C4IX_HEAP_USED`); every kernel
    allocation that could find no memory (`vfs_malloc`: a RAM file's
    growth, an image, a stack, a non-MMU task's memory) and the MMU's
    region and frame allocators (`mmu_raw`, `mmu_fget`, before swap) drop
    files until it can. Never one open for writing (`writers` now counts
    those for every vnode, not just pipes), nor one held: `OP_VFS_GET`
    holds a file while it copies it for a task under the MMU, and without
    the MMU, where the task keeps the kernel's pointer, keeps it for good.
    A file that changes leaves the list (`vfs_touch`), and is RAM's until
    `persist save` keeps it (and then the drive's again, below). `/proc/cache` says how many files are lazy
    and their bytes, how many are cached and theirs, and how many times one
    was dropped.
  - *Out of memory, said.* A first boot whose unpack failed (`tar: cannot
    write /build/...`) went on to the desktop on half a tree. A later one,
    at 32 MB without the MMU, read the 16.5 MB kept tar with
    `arc_readfile`, which did not check its allocation: the tar went to
    address 0 (libjs: "store of 0x00000075 to ROM 0x00000000", the `u` of
    its first name, `usr/`), and the boot never finished. `arc_readfile`
    checks now, and `persist boot` tells memory from other failures: it
    says the archive does not fit in this machine's N MB of heap and exits
    3; vfsload, waiting for it at the desktop's boot, says so and exits 3;
    and init prints "c4ix: out of memory: the system does not fit in this
    machine's memory" and stops, instead of starting a desktop on half a
    tree. The loader says when a program will not fit (`c4ix: loader: out
    of memory loading PATH`), and checks the allocations it did not: the
    constructor lists, the library table, a shared library's record. And
    under the MMU, a task whose entry page could not be had
    (`mmu_user_entry`, which spawn and execve did not check) would start
    at its image's kernel address; it now ends at once, with `c4ix: out of
    memory starting NAME`.
  - *Measured* (libjs, the 32-bit image, the desktop's boot as the page
    makes it, headless; heap from `machine.heap`):

    | | before | after |
    |---|---|---|
    | heap in use, the desktop up (MMU off / on) | 29,073 / 30,517 KB | 4,698 / 6,141 KB |
    | heap at its most, first boot (off / on) | 43,664 / 44,933 KB | 9,625 / 10,895 KB |
    | heap at its most, from the kept copy (off / on) | 42,006 / 43,277 KB | 5,416 / 6,628 KB |
    | console boot (`wait 4`, MMU off): in use, at its most | 22,880, 40,144 KB | 913, 2,179 KB |
    | ... its cycles, from the kept copy | 75.2M | 72.2M |
    | System Monitor's System, 64 MB, MMU on (Chrome 151) | 26.0 MB | 2,257 K |
    | ... MMU off | | 878 K |
    | smallest machine to the desktop, from the kept copy (off / on) | 38 / 44 MB | 7 / 9 MB |
    | ... first boot (off / on) | | 12 / 14 MB |

    The smallest machines were found headless, a megabyte at a time, and
    the kept-copy ones checked in the page. Below them a first boot stops
    with the message above; from the kept copy the tree is read, and it is
    the desktop that does not fit (`c4ix: loader: out of memory loading
    ...`, then the shell's "cannot run desktop"). At 45 MB with the MMU on,
    which ran Wolf3D but neither Doom nor ROTT, both start now, in the page
    and headless (and so does Wolf3D, headless); Doom does at 20 MB too,
    and ROTT from 28 MB, below which its own 8.7 MB zone is what it cannot
    have ("Z_AllocateZone: Couldn't malloc"). Before, at 45 MB, the page's
    unpack failed part way and System held 25.4 MB of half a tree.
    `test-c4ix-rebuild` runs without a drive, as before; run with one
    (`-w`), so that the whole build reads the tree from the drive, it
    passes too, in 719 s against 739 s.
  - *Not done* (then). With the MMU and 6 MB (a 4 MB heap, 3 MB under the
    smallest that boots), the first shell still faults at its first
    instruction, a jump into the kernel's heap ("page fault: jump to
    0x001e1a80"), while swap is busy. It is not the entry page, which the
    check above would have reported. Left for the MMU's own work on
    memory that runs out. (Not seen since 2026-11-13: a first boot to
    the shell at 6 MB with the MMU now reads the tree whole. Most likely
    the same refused read as below, though the old message was not made
    again to show it.)
  - *Checked again* (2026-11-13; asked: is the tree still read from drive
    1, and does the desktop still start in 8 MB?). The page's path had not
    changed: 1,226 files from the copy kept on drive 1, System 2,744 K at
    64 MB with the MMU. Two harnesses read the tree from RAM where the page
    does not. `test-c4ix-innerbench` gave drive 1 no sink, which
    `devices.js` makes read-only, so persist fell back to unpacking into
    RAM: 23.9 MB in 1,227 files, 4.4 MB free of 32. `test-desktop` gave no
    drive 1 at all. Both now have a drive 1 that keeps what is written,
    and check that persist read the tree from it. `test-lazy-root` keeps
    the fall-back, on a drive with no sink, and checks it, and fails if a
    boot from the kept copy holds 1 MB of RAM files or the desktop no
    longer starts in the machines below.
    The first boot without the MMU had grown from 12 MB to 21.
    `arc_readfile` reads the root archive, which is not in the VFS and
    so has no size to stat, by growing its buffer as it goes, doubling
    from 64 KB with the old one held while it copies: 4 + 8 MB for the
    4.3 MB archive. A file it could stat took twice its size too, as the
    buffer was grown when full, before the read that finds the end. It now
    counts a host file by reading it through once, and reads one byte into
    the room for the 0 before it grows a full buffer. The smallest
    machines, headless, MMU off / on (the page, Chrome 151, the same: 10
    MB with the MMU on a first boot, 8 and 7 from the kept copy, where 7
    without the MMU could not load the desktop before):

    | | 78889dc | before | after |
    |---|---|---|---|
    | to the desktop, first boot | 12 / 14 MB | 21 / 10 MB | 10 / 10 MB |
    | ... from the kept copy | 7 / 9 MB | 8 / 8 MB | 7 / 8 MB |
    | to the shell, first boot | | 20 / 9 MB | 9 / 9 MB |
    | ... from the kept copy | | 5 / 6 MB | 5 / 6 MB |
    | ... no drive 1, the tree in RAM | | 44 / 30 MB | 44 / 29 MB |

    *Found* (2026-11-13, later). A first boot to the desktop in 8 or 9
    MB with the MMU read the archive wrong: persist said "not a tar
    archive" ("bad literal/length code" on c4ix_and_toppings), and as
    that was not memory the boot went on, and the desktop started with
    no tree. Neither the inflater nor swap's pages were at fault, but a
    read. `arc_readfile` reads the 4.3 MB archive into its buffer in one
    `read`. The syscall's check (`uaccess`) brings every page of the
    buffer in first, and in a small machine the first of them are out
    again by the time the read is made. The kernel's host read gives the
    drive the user address, through the window. libjs's `dmaWrite`
    stopped at the first page that was out, and `diskRead` still said it
    had read all 4,328,592 bytes. The buffer stayed zeros. With no gzip
    magic, persist took it for a plain tar.
    The kernel's own loads and stores through the window have reached
    the host's copy of a page that is out since swap began (c4m.js
    `kld8`, `kst8`). Now a device's copy reaches it too (arena.js
    `dmaSwapped`, for `dmaRead` and `dmaWrite`). A drive's read and
    write also count only the bytes they moved. The smallest machines
    with the MMU on a first boot, headless: the desktop 8 MB (was 10),
    the shell 6 MB (was 9). In 7 MB the archive inflates whole too, but
    the boot pages so hard that the desktop is not up within the test's
    8,000 M cycles, and from the kept copy the desktop needs 8 anyway.
    In 5 MB persist cannot start, and says so.

    A root archive that cannot be read for a reason other than memory
    now stops the desktop's boot, as running out of memory does. A
    desktop on no tree looks like one, then every program on it fails,
    and the reason is on a console hidden behind it. persist exits 4
    (`DAMAGED`), vfsload passes it on, and init says "c4ix: the root
    archive could not be read, so the system's files are not there" and
    stops. A console boot goes on as before (vfsload does not wait for
    persist there), with persist's message on the console. The drive
    keeps no copy: the id is written last, so the next boot tries again.

  `libjs/tests/test-lazy-root.mjs`; in the page, `web-lazy-root.mjs`.

- **Saved changes, read where they are kept** (2026-11-02, asked for:
  Wolfenstein's shareware, unpacked at first run on a 12 MB machine,
  "seemed to permanently use some extra memory"). A file a program wrote
  stayed in RAM for good: `persist save` copied it to drive 1 and kept
  it, and every boot read the whole snapshot into one buffer and untarred
  it into RAM again, so the 1.8 MB of game data were back after every
  reboot, and the registered game no longer fit.
  - *After a save.* Once the tar is whole, each file saved is made the
    host's again, at its span of the tar (`SYS_RELAZY`, `vfs_relazy`): a
    file over 64 KB has its bytes freed then, a smaller one goes on the
    cache's list, to be dropped when memory is short. Its stamp stays,
    since it did not change (`persist -d` would see a change at every
    save otherwise); a file stamped since the survey is newer than what
    was saved and is left as it is.
  - *At boot,* the saved files are made lazy spans of the snapshot, as
    the kept system's are (`lazy_tar`), and nothing is untarred into RAM.
    The whiteouts are read and deleted.
  - *Two snapshots.* Files read from the last snapshot are read from it
    until this save has made them the new one's, and the host replaces a
    tar the moment it is closed. So saves alternate between
    `1:c4ix-persist.tar` and `1:c4ix-persist-b.tar`, and
    `1:c4ix-persist.cur` ("a" or "b") names the one finished last, written
    after it. A drive from before has only the first, which is the
    current one; `forget` writes its empty archive the same way.
  - *Measured* (`test-lazy-root.mjs`, 64 MB): a 1,251 KB file written and
    saved gives back 2,048 KB of heap, the file and its buffer's slack;
    the next boot's heap is that of a boot with nothing saved, where it
    was the snapshot's size more. A file written since the last save is
    still RAM's, so a long build is RAM's until the desktop's
    once-a-minute save.

- **free, and memory that did not come back** (2026-11-02, asked for: a
  `b4ke -j 3` of the tree in the browser, 32 MB, four CPUs, the MMU on,
  stopped at `src/c4ix/libc` with "c4sp: cannot allocate 400000-cell
  arena", and would not finish even one job at a time with 11 MB free;
  "System" held 16 MB; and persist said "writeing 1:c4ix-persist-b.tar
  failed").
  - *free* (`src/c4ix/user/free.c`) reads `/proc/meminfo` (vfs.c): the
    heap, what is used and free and its biggest free block; RAM files
    not saved, the cache, and under the MMU every block of frames, the
    frames in use in them and the blocks less than an eighth in use; and
    swap. It says when the free memory is in pieces (the biggest block
    under half of it) and how much is kept by a few pages.
  - *Why the build stopped.* Each compile is c4sc with `-c 400000`, a
    6.4 MB arena that c4sp clears (and so touches) at start: three at
    once are 19 MB of a 31 MB heap, which put out 277,969 pages. When
    room to page in fell under a MB, the arena had to be one run of the
    heap, and was not there. The build files start the arena at c4sp's
    default, 65,536 cells, and it grows to four times what is live
    (`gc_collect`) -- the big files need as much as before, the small
    ones far less; and an arena that cannot start at `-c` starts at half,
    down to 16,384 cells (`gc_init`), where it could only stop.
  - *Why System kept it:* frames lent from programs' blocks, and a pool
    flag never set (`docs/c4ix-mmu.md`, "Which block a frame comes from").
  - *c4sc's page fault* (the "page fault: write to ... not mapped" that
    killed a compile, and once a write to the kernel's heap). The light
    path (`sched_light`: a program's malloc or free served without the
    kernel lock) reaches the slab caches when it needs a region's record
    (`sl4b_alloc`, for a new block or slab) and the files' cache
    (`vfs_trim`, on every malloc while memory is short), and those lock
    with `sched_lock` -- which in the trap handler took nothing, the
    handler holding the kernel lock already but for there. A record freed
    by a task's exit on one CPU went to a compile's malloc on another and
    to the next asker as well: two regions in one record, and a list of
    them with a cycle in it, where `mmu_release` went round for ever with
    `mmu_ulock` held. On the light path `sched_lock` takes the kernel
    lock now (`cpu_self->light`), so a malloc that stays in the task's
    own blocks still runs without it and one that goes further waits for
    it, as any trap does.
  - *Why the build then hung* (output stopped after `splash.c`, b4ke
    ready with three children to reap, three CPUs parked). A CPU is
    marked on the task it switched out (`oncpu`) until its idle loop
    passes `smp_settle`; no other CPU resumes the task meanwhile, and no
    IPI is sent for it (`smp_kick`). The loop could be switched out
    between settling and parking -- an IPI held while it had the lock
    lands as it lets go -- and came back there, after the task it ran
    had blocked, and parked with the mark still on. A switch back to an
    idle task now sets its CPU's park word (`smp_park`), which the loop
    clears under the lock before it settles: a park after such a switch
    falls through to settle again.
  - *persist's failed write.* A READ from a drive on any CPU overwrote
    the drive's `DISK_FD` and `DISK_WADDR` (libjs's device registers are
    CPU 0's for every CPU), between persist's own writes of them: c4m.js
    READ puts them back. And two saves at once -- `persist -d`'s and a
    `sync` typed -- wrote into the same snapshot: a save holds
    `/tmp/persist.lock` (a `mkdir`, its holder's pid inside); `sync`
    waits for it, and `-d` skips its turn.
  - *Measured* (`libjs/tests/test-c4ix-free.mjs`; and the build, 32 MB,
    four CPUs, the MMU on, `-j 3`): the whole tree, 127 steps (`b4ke: all
    built, in /build`), 3,195 s of machine time (18 minutes in node), 1.48
    million pages put out; where it had stopped at `src/c4ix/libc`. After
    it, `free` has 11.4 MB in use -- 4.2 MB of it the build's RAM files,
    not saved yet, 1.5 MB the cache, 2.5 MB programs' blocks (1.6 MB of
    pages in them), 3.2 MB other -- and a biggest free block of 13.4 MB of
    the 19.9 MB free. `x5-c4m.txt` is re-pinned for the vnode
    `/proc/meminfo` adds.

### The tree (2026-10-29)

`/` holds directories only. The programs are in `/bin`, and the shell
finds a bare name in the directories its `PATH` names (`sh.c`):
`/bin:/usr/bin:/usr/games/bin`, until a line `PATH=DIR:DIR...` changes
it. Until now each program had a second name at the root too, an ilink,
from before the shell looked anywhere but where it was; and the kernel's
log was `/log/kernel`. `pin` and `nice` look where the shell does.

| where | what |
|---|---|
| `/bin` | the programs, `c4ix-NAME.c4r` as `NAME`; the toolchain; `sync` |
| `/lib` | the shared libraries a program links with `-l` |
| `/usr` | the repository, from the root archive: `/usr/src`, `/usr/include`, `/usr/lib` |
| `/usr/games/NAME` | each game, its data, and its settings and saved games (Doom, ROTT and Wolf3D change to it; Tyrian names it, `tyr-c4ix.c`) |
| `/usr/games/bin` | each game's programs by name, ilinks into those directories, so `doom` runs from anywhere |
| `/build` | what `b4ke -C /usr` makes, already made |
| `/etc` | the desktop's settings, `desktop.cfg` |
| `/var/log/kernel` | the console, logged (`console.c`) |
| `/var/persist` | persist's list of the tree as it booted |
| `/tmp`, `/proc` | scratch files; `/proc/uptime`, `/proc/kmsg`, `/proc/cache` (files on the host and in RAM) |

`/home` is for the person's own files: the page's uploads are listed
there (`libjs/uploads.js`, docs/c4ix-desktop.md part sixteen).

### Memory given back when tasks have gone (2026-11-05)

The kernel's arena is fixed, and when it is full a slab comes from the heap, wherever
the heap has room: at the peak of a crowd of programs, that is the middle of what will
be free memory once they have gone. It used to stay there until a reboot. Now when a
task has gone, a second or more later and with no kernel task wanting to run, a trap
tidies (`mmu_tidy`). Slabs with nothing in them go back to the heap (`sl4b_tidy`), and
a cache whose objects can be told apart from what names them (`c->moves`, so far
`urgn`, by `mmu_rmove`) has its live objects copied down first. With the MMU, page
tables move down too. Alloc and free stay a pop and a push.

Programs' memory comes from the heap's top and the kernel's from its bottom, with or
without the MMU (`umem_block`, `SWAP_HIGH`). With it, programs' pages also move
(`mmu_compact`, `mmu_raise`). Without it, programs cannot move, so a program that stays
cuts free memory until it exits, and a task, vnode or file alive in a slab keeps that
slab. `docs/c4ix-mmu.md` section 11 has the evidence, what each case can and cannot
do, and the numbers.

### Fast paths (2026-11-04)

A call in c4 costs its `JSR`, `ENT`, `ADJ` and `LEV`, a push per argument and
a load per parameter read, and every `&&` term a load, a compare and a branch.
What runs at every trap, at every switch and for every page is written to ask
as little as it can. The rules, for code added to those paths:

- **Compute a predicate where what it depends on changes, not where it is
  asked.** A task's `tflags` (`TF_*`, c4ix.h) answers what the hot paths ask of
  a task, in one AND and one compare (`TF_PAGEOK`, `TF_HOLDOK`, `sched_held`),
  and a field it depends on is written with its bit beside it: `TF_SET`,
  `TF_CLR`, `sched_setnice`. A region's half of `mmu_canout` is `r->canout`
  (`MMU_RCAN`, mmu.c), kept the same way.
- **But not at the cost of a hotter path.** `state` changes at every switch, and
  is not in the word: a bit kept there would tax the hottest path to spare the
  coldest. `TF_HOLDOK` compares it where it is asked, and `TF_NOPAGE` is only
  for the two states it takes rarely.
- **A cross-module one-liner is a macro in c4ix.h**, with the rare case in a
  `*_slow` function: `-minline` cannot cross modules, and the kernel is thirteen.
  `sched_lock`, `klock_take`, `sched_has_bkl`, `task_next`, `vfs_trim`,
  `mmu_trap_in`. They are expressions (`?:` and the comma, as `va_arg` is), and
  an argument may be evaluated more than once: pass a plain variable.
- **Ask when something has changed, or by the clock.** `smp_kick` looks for work
  for an idle CPU at a switch, when `sched_wake` says so, and each millisecond of
  the machine's clock, for what nothing says. A site that forgets to say costs that
  millisecond, never a wakeup: which is what makes it safe to skip the walk.
- **A cached answer names what ends it.** `UARGS_OK` keeps one stack page checked
  per CPU, the check being a fifth of a syscall: ended by a switch, or by
  `mmu_mapgen`, bumped after every unmap and read before the tables are. It vouches
  only for reading a syscall's three words. (A task cannot aim a syscall at a page
  it has unmapped: its own pushes would fault first.)
- **A walk that finds nothing should not be walked.** The scheduler's walks of the
  task table became queues (below). What is left of them reads the clock once.
- **What is invariant in a loop is asked once.** `mmu_clock` asks a region's
  questions once for all its pages, and looks up a table once per 1,024 of them.
- **Measure it.** Each of these was measured against the last, with
  `/bin/bench PROG N` (C4IX's, `src/c4ix/user/bench.c`).

Baseline f76fb05 to c4ix_and_toppings at the end of the pass, under libjs with
the MMU, in 32 MB. Cycles, but for what is marked:

| | 1 CPU | 4 CPUs |
|---|---:|---:|
| syscall (`getpid`) | 1,276 -> 819 | 2,768 -> 1,205 |
| yield | 1,838 -> 1,215 | 3,563 -> 1,869 |
| malloc + free | 2,823 -> 2,158 | 3,168 -> 2,502 |
| syscall, 50 tasks asleep | 1,456 -> 873 | 13,161 -> 1,613 |
| yield, 50 tasks asleep | 9,085 -> 4,870 | 21,517 -> 6,894 |
| malloc + free, 50 asleep | 3,248 -> 2,277 | 5,021 -> 2,854 |

| | before | after |
|---|---:|---:|
| `innerbench -Tln 50` | 303 s | 250 s |
| `innerbench -Tln 40` | 193 s | 174 s |
| `b4ke -B -j 3` inside C4IX | 3,558 s | 3,343 s |
| 48 MB walked 4 times, in 32 MB, one program | 12.0 s | 10.2 s |
| the kernel's image | 521,650 bytes | 596,105 bytes |
| userland reached, native c4m | 3.17 M cycles | 3.74 M cycles |

The times are the machine's, on 4 CPUs. The last two lines are the price: 14%
more kernel, most of it the lock macros' 220 sites, and a boot that loads it.

What the pass tried and left out, measured or not worth the keeping:

- **`-minline` on the kernel.** On top of the macros: a syscall 2,453 -> 2,445
  cycles, `-Tln 40` 179.07 -> 178.85 s, for 28 KB more image. And c4sc's
  `compile`, which rebuilds the kernel inside C4IX, has no `-minline` to match.
- **Counters of free frames** for `mmu_pageroom` and `mmu_compact`, whose walks
  run for a big malloc and when a task is done, beside work far bigger. Counters
  that can drift are not worth that.
- **A free-word hint per block of frames, and a cached best block in
  `mmu_ftake`.** After the clock's rewrite and `mmu_nofnew` a program bigger than
  memory went fast enough that neither has been measured. A hint for the first
  slab with room is no use either: the newest slab is the list's head, and the
  walk past full ones happens once per slab filled.
- **Caching a library's user address** (`loader_switch`): one `mmu_uva` per
  library per switch of a program that has one. Not measured.

- **A slice deadline per CPU.** `SCHED_STAYS` does what it was for, with no CPU
  having to tell another.
- **`cpu_self->udir`.** The check's cost was in the call, and `UARGS_OK` skips that;
  a stale directory would be a security hole, for a fraction of the gain.
- **Counts of the ready, to skip `sched_pick`'s walk whole.** A site that missed
  them would lose a wakeup, unlike `sched_wake`. The walk was 72 cycles a task, at
  a switch: 4,870 cycles to yield with 50 asleep, against 1,215 alone. The run
  queue removed it instead.

Where the kernel's time goes under three whole loads, function by function, is
under [its own heading](#where-the-kernels-time-goes-2026-11-13) below.

### The run queue (2026-11-05)

A task is on the queue its state says, and the scheduler looks at the queues rather
than the task table, so what a switch costs no longer depends on how many tasks are
waiting.

| Queue | Who | What takes them off |
|---|---|---|
| `Q_READY` | ready, oldest first; a task goes on the tail as it is preempted or woken | a CPU taking the first it may run (`sched_mine`) |
| `Q_SLEEP` | asleep on the clock, soonest first | one compare with the head says if anyone is due |
| `Q_POLL` | waiting for what the host writes with no event: a window's ring (`SYS_EVWAIT`), the console | looked at each ms (`sched_tick`) |
| `Q_PIPE` | blocked reading a pipe, on that pipe's own list (`vn->waiters`) | a write, or the last writer's close (`sched_pipewake`) |

- **Every change of state goes through `task_state`**, under the lock. The only other
  writes are the switch's own, between `q_off` and `rq_add`, where it is the one
  moving the task. A new site writes `task_state` or it breaks the watch.
- **The old walk is kept as a watch.** Every 16 ms `sched_tick` walks the table for a
  task whose state says it should be on a queue and is not, or left on a pipe that has
  data. It puts the task right, logs the first, and counts in `sched_rqlost`,
  `/proc/meminfo`'s `rqlost`. So a site that forgets costs a few ms, not a task that
  never runs. `test-c4ix-free`, `-loadctl`, `-movable`, `-tidy`, `test-desktop` and
  `test-rebuild` fail if the count is not 0.
- **A task held by load control stays on the ready queue**, skipped by `sched_mine`,
  rather than coming off it. A signal must release a held task (`sched_held` asks
  `TF_SIG`), and any of the places that post one would otherwise have to requeue it.
  Few tasks are ever held, so the skip costs little.
- **Blocked on the console stays polled.** The host delivers those bytes with no event
  the kernel sees. Only a pipe has a writer in the kernel to wake its reader.
- **The turn order changed.** It was "the next in the table after this one"; it is now
  the order they became ready, which is fairer to a task woken late. A parent that has
  waited comes back on whichever CPU is free, and each CPU has its own count of cycles,
  so `bench`'s spawn loop now pins itself for its two readings.

Before to after, the same kernel but for this, under libjs with the MMU in 32 MB, in
cycles. (`spawn + wait` is left out of the comparison: the pin changed what it measures.)

| | 1 CPU | 4 CPUs |
|---|---:|---:|
| syscall (`getpid`) | 829 -> 856 | 1,222 -> 1,212 |
| yield | 1,227 -> 944 | 1,891 -> 1,379 |
| malloc + free | 2,182 -> 2,236 | 2,599 -> 2,559 |
| syscall, 50 tasks asleep | 877 -> 849 | 1,646 -> 1,210 |
| yield, 50 tasks asleep | 4,897 -> 960 | 6,906 -> 1,400 |
| malloc + free, 50 asleep | 2,283 -> 2,179 | 2,950 -> 2,527 |

Native c4m, 1 CPU: yield 1,095 -> 808, syscall 697 -> 722, malloc + free 1,746 -> 1,793.
The kernel's image went from 623,323 to 633,338 bytes. With 50 asleep a task now costs
what it does alone, which was the point. The price is about 27 cycles on a syscall on
1 CPU, for the clock's compare at each trap and what the setter's calls add.

Whole loads move less. The 32 MB `b4ke -B -j 3` took 3,372,315 ms against 3,400,500.
`innerbench -Tln 40` under node took 175 s against 178, leaving 26,116 KB in one piece of
26,382.

`-Tln 50` is past what fits, and is about the same on both. A single run says little: its
time follows how many of the 50 get to start, and that turns on small shifts in timing,
since a start that finds no room is refused. Under node a run repeats exactly, so nine runs
each were shifted by typing a few `echo`s first, on the same disk:

| Benchmarks that ran | Before, s | Run queue, s |
|---:|---:|---:|
| 50 | 245, 251, 254, 263 | 258, 261 |
| 49 | 237, 249 | 244 |
| 48 | | 234, 238 |
| 47 | 222, 222, 224 | 227, 227 |
| 46 | | 216, 216 |

At the same count the queue is 0 to 2% slower on these few runs, which is within the
spread. Inner kernels refused: 11 before, 17 with the queue, also too few to call. Swap
traffic follows the count, not the kernel. Compare `-Tln 50` runs only at the same count.

`spawn + wait` on 1 CPU read 159,399 and 177,378 cycles on two runs, so it is
noisy at 5 reps; 158,475 before.

In the page, 4 CPUs at 400 MHz, 32 MB with the desktop: `-Tln 40` 103 s, `-Tln 50` 118 to
149 s, nothing killed, `rqlost` 0, and 22,692 to 23,230 KB in one piece of about 24,400
afterwards. With 50 in 32 MB some inner kernels are refused at start ("out of memory
starting /bin/c4m"), so 42 to 48 of the 50 report; the kernel before the queue did the
same. `mmu=0` in 64 MB: `-Tln 20` 47 to 49 s, `rqlost` 0.

### Where the kernel's time goes (2026-11-13)

Three loads under libjs with the MMU, 4 CPUs, 32 MB, on c4ix_and_toppings 564cf1a,
profiled by function: the CPU's pc sampled in chunks of random length, and a helper
(`klock_spin` and `klock_let`, the `<...>` stubs, `mem*` and `str*`) charged to the
function that called it, found by walking `bp` (`[bp]` the caller's `bp`, `[bp+4]` the
return pc). So `bkl_take` is the time spent spinning for the kernel lock.

| Load | Machine time | Kernel | User | Idle |
|---|---:|---:|---:|---:|
| Quake in a desktop window, frames 6 to 120 | 66,700 ms | 2.13% | 22.94% | 74.92% |
| `innerbench -Tln 40` | 182,479 ms | 21.10% | 75.09% | 3.80% |
| `b4ke -B -j 3` inside C4IX | 3,278,075 ms | 5.02% | 51.02% | 43.96% |

The top 20 of each, as a share of the kernel's time:

| | Quake in a window | | `-Tln 40` | | `b4ke -B -j 3` | |
|---:|---|---:|---|---:|---|---:|
| 1 | `sched_events` | 53.2% | `bkl_take` | 27.7% | `sched_trap` | 25.1% |
| 2 | `sched_trap` | 22.0% | `sched_trap` | 18.9% | `mmu_ftake` | 13.1% |
| 3 | `sched_tick` | 5.3% | `vfs_span_at` | 12.2% | `umem_alloc` | 9.1% |
| 4 | `acct_cpu` | 2.5% | `vfs_read` | 8.1% | `bkl_take` | 6.6% |
| 5 | `sched_allows` | 1.7% | `vfs_span_read` | 4.3% | `mmu_clock` | 4.9% |
| 6 | `bkl_take` | 1.6% | `vfs_dec` | 4.0% | `mmu_fput` | 2.4% |
| 7 | `sched_run` | 1.6% | `vfs_pread` | 3.5% | `mmu_sop` | 2.3% |
| 8 | `smp_ap` | 1.6% | `uaccess` | 2.5% | `mod_read` | 2.1% |
| 9 | `mmu_ftake` | 1.6% | `sys_pmviolation` | 1.8% | `mmu_salloc` | 2.0% |
| 10 | `sched_mine_smp` | 1.5% | `acct_cpu` | 1.8% | `mmu_pageout` | 1.9% |
| 11 | `bkl_drop` | 1.2% | `sys_read` | 1.2% | `acct_cpu` | 1.9% |
| 12 | `smp_kick` | 0.8% | `umem_alloc` | 1.1% | `mmu_pagein` | 1.8% |
| 13 | `sched_unblocked` | 0.8% | `sched_run` | 1.1% | `mod_data_relocate` | 1.7% |
| 14 | `sched_pick` | 0.6% | `mmu_ftake` | 1.0% | `sched_run` | 1.6% |
| 15 | `smp_idlefor` | 0.5% | `smp_idlefor` | 1.0% | `mmu_shootdown` | 1.5% |
| 16 | `sched_waker` | 0.5% | `sched_tick` | 0.9% | `link_modules` | 1.5% |
| 17 | `uaccess` | 0.3% | `mmu_clock` | 0.8% | `sched_pick` | 1.5% |
| 18 | `sched_cpustat` | 0.3% | `sched_mine_smp` | 0.8% | `mmu_sput` | 1.5% |
| 19 | `mmu_pagein` | 0.2% | `mmu_switch` | 0.7% | `sched_tick` | 1.4% |
| 20 | `mmu_sput` | 0.2% | `sched_pick` | 0.6% | `mmu_uva` | 1.2% |

`sched_trap` is the trap itself: the switch, the accounting, the clock's checks. What
each load says:

- **Quake: the poll for pipes was half the kernel.** The desktop and each program in a
  window sleep in `SYS_EVWAIT` on `Q_POLL`, which `sched_tick` looks at every ms, and
  `sched_events` walked all 64 fds of each, every time, for a pipe with something to
  read. Fixed below.
- **`-Tln 40`: a third of the kernel is waiting for the lock, and a third is reading
  spans.** Each inner c4m loads `c4ke.c4r` a few bytes at a time (`load-c4r.c`): 555,641
  reads of a host span in the run, 555,560 of them of `c4ke.c4r` and 537,160 of 2 to 16
  bytes. Each is a trap, the span parsed again (`vfs_span_at`, `vfs_dec`) and the host's
  file opened, read and closed, all under the kernel lock, which is why the lock is
  also the top line. Not changed here (below).
- **`b4ke`: memory.** `mmu_ftake`'s walk of the frame lists and `umem_alloc` are a fifth
  of the kernel's time, and the kernel is 5% of the machine's.

#### The poll that waited for pipes

A pipe's write, and its last writer's close, already call `sched_pipewake` to wake the
readers blocked on it. That now also bumps `sched_pipegen`. A task's walk of its pipes
(`sched_pipesready`) notes the count it saw in `t->evgen`, and the poll each ms
(`sched_evpoll`) walks them again only once the count has moved. The window's ring is
still looked at each ms: the host writes it with no event the kernel sees. And a walk
goes only as far as the task has ever had an fd (`t->fdtop`, kept by `fd_install`,
`fd_dup2` and the clones), not all 64.

The bump and the walk are both under the scheduler's lock, the bump after the data is
in, so a write the walk missed has always moved the count. A wakeup lost anyway would leave the task asleep until its
timeout: the watch (above) now also looks every 16 ms for a task asleep in `SYS_EVWAIT`
with a pipe ready and its count current, wakes it, and counts it in `rqlost`.
`test-c4ix-pipewake` has a reader on CPU 2 wait for 200 lines written 3 ms apart from
another CPU, asleep in `uevwait` and then blocked in `read`, on threads and on simulated
CPUs: every line arrives, no wake finds nothing, no timeout is waited out, and `rqlost`
stays 0.

Before to after, the same disk, the windowed timedemo of Wolf3D in node (`ix.mjs`,
measured from its first frames to its last):

| | 564cf1a | after |
|---|---:|---:|
| Wolf3D in a window, 1,208 frames | 32,806 machine ms | 32,035 machine ms |
| ... the cycles it ran | 3,280.6 M | 3,203.5 M |
| ... the kernel's share | 4.1% | 1.8% |
| ... the poll's (`sched_events`; then `sched_evpoll` and `sched_pipesready`) | 2.42% | 0.08% |
| Quake in a window: the kernel's share | 2.13% | 1.05% |

`innerbench -Tln 40` took 175.2 s against 174.2, the same: nothing there waits in
`SYS_EVWAIT`.

#### What a task's exit cost the next one

A task that ends is freed at the next trap, on whichever task trapped (`task_reap`):
its memory given back and the rest moved up (`task_destroy`, `mmu_compact`), 140,000 to
240,000 cycles. Those were charged to the task that trapped. On one CPU with the run
queue, the trap after an exit is most often the plain program's, so in
`test-c4ix-smp`'s nice check a program never stopping showed in `ps` as using three or
four slices more for each one beside it that ended, and `nice -n -3` got 2.7 to 2.9
times its cycles where the check asks more than 3. The reap's cycles now go to the
kernel task: 3.7 times (43,565k cycles against 11,860k), the four slices to one that
nice -3 is.

#### Left for later

What the profiles found that is not cheap, or not low in risk:

- **Tiny reads of a host span** (`-Tln 40`): a read-ahead per open file in the kernel, or
  `load-c4r.c` reading in bulk, or the span's parse kept with the open file. Each would
  take most of a third of that load's kernel time, and with it time under the lock.
- **The kernel lock** (`bkl_take`, 28% of the kernel in `-Tln 40`): shorter work under
  it, starting with the span reads.
- **`mmu_ftake`'s walk** and **`umem_alloc`** (`b4ke`, 13% and 9%): a cached best block
  per pool and a free-word hint, left out on 2026-11-04 as not then measured.
- **A sleeper pinned to a CPU other than 0 wakes up to 100 ms late**: each `umsleep(3)`
  of a task pinned to CPU 1 took 100 ms. Before the run queue too (dd3f564).
  `test-c4ix-pipewake` keeps its writer on CPU 0 for this.
- **`b4ke -B -j 3` in 32 MB runs out of memory at 564cf1a** ("loader: out of memory
  loading /bin/c4sc (2101 KB)", `BUILD.B4K:30`), where on 2026-11-05 it finished.
  `test-c4ix-rebuild` runs it in 64 and 128 MB, and passes.

## 6.1 Running it

    make run-c4ix        an interactive shell on c4m
    make run-c4ix-c4     the same through plain c4 -> c4m
    make demo-c4ix       the guided tour: every milestone in order
    make test-c4ix       the pinned suite
    make test-c4ix-fmt   both printf implementations, against each other
    make test-c4ix-c4ke  C4KE's own binaries, unmodified, as C4IX tasks
    make bench-c4ix      boot cost and the OS microbenchmarks

    make test-c4ix-c4ke-nested   a whole C4KE inside a nested c4m (slow)

### The kernel's command line

    c4ix.c4r [OPTION] [PROGRAM [ARGUMENT]...]

      (no arguments)     boot to an interactive shell
      PROGRAM [ARG]...   boot, run PROGRAM with ARGs, shut down
      --demo IMAGE...    run the built-in demonstrations
      -h, --help         print the usage and shut down
      -q                 accepted and ignored
      -v                 the kernel log on the console as well (dmesg reads it either way)
      --desktop          the web page's boot: the root archive unpacks first,
                         with a loading screen, then the shell runs
                         "desktop &" and "top", as if they were typed

`init_usage()` in init.c is the authority; keep this section in step
with it. **No arguments boots to a shell**, because that is what
anyone running this wants -- the demonstrations used to be what you
got for asking for nothing, which had the common case behind the
uncommon one.

PROGRAM is an image path as the host filesystem sees it. The SHELL
expands a bare name to `c4ix-NAME.c4r` or `NAME.c4r`; init does not,
so name the image in full on the kernel's own command line.

`--demo` takes six images and gives each a fixed role: (1) a program
that calls printf directly, which is what proves redirection reaches
a program that has never heard of C4IX, (2) one that uses libc4ix
syscalls, (3) and (4) the writer and reader of a pipeline, (5) a
shell and (6) a script for it to run. Fewer than six still runs the
stages it has arguments for, so a short list puts one image into
several roles -- which is a real trap: name a shell there and it will
be spawned with its stdout redirected into a RAM file, prompt and
all, and the terminal goes quiet while it waits to be typed at. Init
announces that before it happens for exactly this reason.

`-q` used to mean "skip the demonstrations". They are opt-in now, so
it does nothing; it is still accepted so older command lines work.

In the shell, anything in the tree named `c4ix-NAME.c4r` is a
command: echo, cat, wc, ls, mkdir, ps, top, bench, spin, hello, cycles,
fmt, gui, desktop (the last two need the libjs display). The
prompt carries the working directory, `help` lists the builtins, and
`exit` or end-of-file leaves, which shuts the kernel down. Ctrl-C
cancels the foreground job and does nothing at an idle prompt;
`jobs` lists background tasks and `kill [-SIG] %JOB|PID` ends one.

## 6.2 The C4KE compatibility layer (2026-08-05)

C4KE's own binaries run as C4IX tasks, **unmodified** -- the same
files C4KE's suite runs. If one ever needs a rebuild to pass, the
layer is wrong, and that is the strongest check available.

    ./c4m load-c4r.c -- c4ix.c4r ps.c4r
    ./c4m load-c4r.c -- c4ix.c4r top.c4r -b -n 2
    ./c4m load-c4r.c -- c4ix.c4r innerbench.c4r -n 2

Working: `ps`, `top`, `spin`, `bench`, `benchtop`, `innerbench` --
including innerbench's default mode, where each "benchmark" is a whole
C4KE compiled from source inside a nested c4m.

### How it attaches

C4KE reaches its kernel through custom opcodes >= 128, which no VM
implements, so they arrive as `TRAP_ILLOP` -- the same door C4IX's own
`SYS_*` gateway uses, with the same argument shape. The hook is one
`else if` in `sched_trap`'s ILLOP gate, and one module, `c4ke.c`.

Only `OP_REQUEST_SYMBOL` (128) is a fixed number. u0 asks for every
other service **by name** at startup, so C4IX assigns its own
numbering and the binaries never need to know. Lookup uses `memcmp`
-- the `MCMP` opcode, one instruction -- over `len + 1` bytes, so the
match is exact rather than C4KE's prefix compare, with a length test
in front that rejects nearly every candidate in two instructions. A
hand-rolled comparator would be ~8 instructions per character, and u0
issues 27 lookups before `main`.

### Why it is slower, and why that is the right trade

`bench` scores about 72% of its C4KE figure, the nested kernel about
65%, with wall time within 6%. Under C4KE every `printf` goes straight
to the host; under C4IX each one traps and the kernel formats it onto
whatever fd the parent supplied. That is the tax for having pipes,
redirection and a VFS at all -- C4KE is faster the way DOS is faster,
by not offering the feature.

### The parts that were not obvious

- **Constructors must run in the task**, not at load time. C4IX ran
  them in the SPAWNING task's context, and u0's constructor caches its
  pid and its parent's and installs eight signal handlers -- all of
  which would have been wired to the wrong task. They now run from
  `task_shim`, which also made destructors possible for the first
  time, closing an X1 debt.
- **printf had to become real.** Both implementations knew five
  conversions; `ps` uses `%3ld`, `%4ld.%03d`, `%2d`, `%3d%%` and
  `%*s`. The last is the one that proves it: an unrecognised spec that
  does not *consume* its width argument shifts every conversion after
  it, so the output would be wrong rather than merely misaligned.
- **Signal delivery builds a trap frame by hand**, and two slots have
  no local symptom if omitted: `bp+6` mode (the garbage C4KE left
  there was its shutdown segfault) and `bp+9` interval (C4IX opted
  into `CONF_TRAP_RESTORES_INTERVAL`, so TLEV restores preemption from
  it).
- **The default action for an unhandled signal is IGNORE**, except
  KILL, TERM and INT. `top` signals its parent with SIGUSR1
  unconditionally and `innerbench` installs no handler for it, so a
  POSIX "terminate" default would kill innerbench the moment top
  started.
- **argv must be copied.** `benchtop` rewrites its own argv array
  between spawns and `innerbench` frees its argv while the spawned
  tasks still run.
- **Ctrl-C derives the foreground job** from the wait chain rather
  than being told. C4KE has programs declare themselves with
  `c4ke_set_focus`; the compat layer accepts that call and ignores it.
- **The image comes from `argv[0]`**, not from the `name` argument,
  which is a label -- innerbench passes the title "inner-kernel" with
  `argv[0] = "c4m"`.

### What a nested kernel does NOT do

It does not steal the host's trap handler. `c4m.c` never *calls*
`install_trap_handler`; its `ITH` case is a store to the interpreter's
own variable, so a nested C4KE's `ITH` is absorbed by the nested c4m
and never reaches the host. `c4m.c4r` emits no `ITH` and no `C4CF` at
all. The hazard is narrower: loading `c4ke.c4r` **directly** as a C4IX
task, with no c4m in between, would replace the handler -- the
documented "a child kernel disables its parent". Nothing in the target
set does that.

Running `c4ke.c4r` directly would need per-task trap handlers, which
C4IX could actually provide (unlike C4KE, it has per-task state: a
trapped `ITH` could go in the task struct and `sched_trap` could
dispatch faults to it). Not done, and not needed.

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
