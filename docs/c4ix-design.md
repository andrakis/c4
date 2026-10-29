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
  - *What is kept* is one ustar archive, `1:c4ix-persist.tar`: the changed
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
| `/tmp`, `/proc` | scratch files; `/proc/uptime` |

`/home` is for the person's own files: the page's uploads are listed
there (`libjs/uploads.js`, docs/c4ix-desktop.md part sixteen).

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
