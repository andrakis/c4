# Storage on the breadboard: drives, media, and what each rung can reach

## Why

c4bb has **one disk**, it is **read-only**, and everything a session
builds lives in RAM — C4DOS's RAM disk, then C4KE's ramfs. That is why
the climb has always had to be one sitting: the moment the machine is
switched off, or the moment a kernel boots and the RAM disk goes with
the system that owned it, the work is gone.

It is also the wrong shape for HOMEWARD, where the progression is the
point:

| rung | what the player has just made work | what it needs to keep |
|---|---|---|
| a letter on the wire | UART TX | nothing |
| `Hellorld` | a program in ROM | nothing |
| firmware | malloc, printf, a heap | nothing |
| C4DOS | a disk to read, a RAM disk to write | nothing yet |
| the tools | `dostar x`, c4cc, cpp | the tools it just built |
| raycaster, editor | enough opcodes to be fun | — |
| **C4KE** | a kernel it compiled itself | **the kernel, on a medium that survives a reboot** |
| C4KE's tools and games | traps, preemption | — |
| **C4IX** | c4sc, b4ke, a linker | **a third medium, and a blank one to write to** |
| C4IX | protected mode, the lot | — |

Two of those rows are impossible today. Building C4KE and then *booting
what you built* works only because `dosload` hands one system to the
next inside a single power-on; there is no medium to put the kernel on,
no second drive to boot it from, and no way to come back tomorrow.

The player-facing story the user wants is the ordinary one: **the build
writes to the floppy in drive 2, you eject the C4DOS disk, and the
machine boots from drive 2.** Each rung needs a little more hardware
than the last — another drive, a write head, a firmware that knows about
either — which is exactly the shape of every other rung in this game.

## What the machine looks like today

- `sim/cli.js` `loadDisk(dir)` walks one directory into a
  `Map<name, Uint8Array>` and hands it to `Devices`.
- `sim/devices.js` holds it as `this.files`, and serves
  `DISK_NAME` / `DISK_FD` / `DISK_ADDR` / `DISK_LEN` / `DISK_CLOSE` —
  open, read, close. **There is no write register.**
- `hw/microcode.uc` drives those five registers from the `OPEN`, `READ`
  and `CLOS` opcodes and nothing else. Guest code never touches them
  directly; it calls `open()`/`read()`.
- The web build (`web/app.js`) fetches `images/<dir>/manifest.json` and
  builds the same map, keyed off a `DISKS` table that maps a program to
  its disk.

So the disk is a device the CPU already knows how to talk to, and every
register is memory-mapped. That matters for the design below: `fw.c`
already reaches `DEV_INTERVAL` and `DEV_HEAP_BASE` with a plain
`*(int *)ADDR = x`, so **a guest can drive new disk registers with no
new opcode and no microcode change at all.** C4 has no `write` syscall
and does not need one.

## The design

**Drives, not a disk.** `Devices` holds an array of media. Drive 0 is
the boot drive. Two ways to reach another one, both of which a strict-c4
program can use:

- a **name prefix** — `1:c4ke.c4r` — which is how C4DOS already thinks
  and costs nothing at all in the machine;
- a **select register**, `DISK_DRIVE`, for code that would rather set it
  once, and so that a program can ask how many drives there are and
  whether the current one is writable.

**Writing.** Four new registers, mirroring the read side exactly:

    DISK_WNAME   create/truncate on the selected drive -> fd or -1
    DISK_WADDR   buffer address
    DISK_WLEN    write N bytes from it -> bytes written
    DISK_WCLOSE  close and flush

In the CLI a writable drive is a host directory and a write goes
through to it. In the browser it is an image in IndexedDB, so the disk
survives a reload — which is the whole point of a medium.

**Ejecting.** A drive can be empty. `DISK_EJECT` empties one; the CLI
and the web both let you say which directory is in which drive at start,
and the web adds a picker, because "switch drive 1 to a blank medium" is
a thing the player does with their hands.

**Firmware stages.** `fw.c4r` is already loaded separately from the
program image, so a staged firmware is a file name, not a code change:
`-fw fw-min.c4r`. What each stage does or does not implement is a
HOMEWARD question, not a c4bb one; the flag is all the machine owes it.

## Milestones

- [x] **M0** This tracker, before any code.
- [x] **M1** Several drives, read-only. `-d` repeatable, `N:`/`B:` name
      prefix, `DISK_DRIVE` (0x13c), `DISK_COUNT` (0x188), `DISK_RO`
      (0x18c), `DISK_EJECT` (0x190). No microcode change: the read
      registers already existed and now resolve against the selected
      drive. Which drive *boots* is which image you hand the CLI, so
      the flag the design asked for turned out not to be needed.
- [x] **M2** A writable drive. `DISK_WNAME`/`WADDR`/`WLEN`/`WCLOSE`
      (0x178–0x184), `include/c4bb.h` for guests, and `cli.js -w dir`
      writing through to a host directory. A file becomes real on
      close, in one piece.
- [x] **M3** C4DOS can save: `RUN bbsave.c4r 1:` writes every RAM-disk
      file onto drive 1 (`src/c4dos/bbsave.c`, a transient — C4DOS
      itself is untouched, the same rung dostar and dosload sit on).
- [x] **M4** C4KE can save: `save 1:` does the same for the ramfs
      (`src/c4ke/bin/save.c`), so the C4IX that b4ke just built can be
      put on a medium and booted on its own.
- [ ] **M5** The browser: a drive panel — what is in each drive, eject,
      insert, and a medium that survives a reload (IndexedDB). Not
      started; the CLI is where the climb is tested.
- [ ] **M6** `-fw` and a staged firmware set, so a rung can be denied
      the hardware it has not built yet. Not started. `fw.c4r` is
      already loaded as a separate file, so this is a flag and a set of
      cut-down firmwares, not a change to the machine.

### Bar, met

`make test-c4bb-storage` (`src/c4bb/tests/test-storage.sh`):

- two drives visible, drive 0 read as before;
- `dostar x tools-src.tar` then `RUN bbsave.c4r 1:` → five files onto
  drive 1, each `cmp`-identical to the source file that went in;
- **a second machine, started later, with drive 1 as its drive 0, reads
  them back** — which is the whole point;
- and the same save onto a `-d` (read-only) drive is refused.

---

# Part 2 — a BIOS, an install step, and a C4KE you can actually use

Requested 2026-08-28, after the first walkthrough
(`docs/climbing-the-ladder.md`) worked but showed up five gaps. In
priority order as agreed: the walkthrough is written down, C4KE is
missing most of its userland on the disk the player actually boots, `ls`
is unreadable, `ps`/`top` are too wide, and the firmware should behave
like a BIOS so that inserting a floppy and rebooting is the whole
interface.

## The shape it is aiming at

    power on ─► banner
             ─► probe and report RAM
             ─► probe drives, boot the first with a boot image
             ─► no media?  say so, wait, try again  ◄── player inserts a disk

    C4DOS floppy in drive 0, blank in drive 1
      LADDER          builds the toolchain and C4KE
      INSTALL 1:      kernel + tools + SOURCES for everything C4KE and
                      C4IX will need, onto the blank
      eject, reboot ─► C4KE boots from drive 1
      build its own tools, `save 1:`  ─► they persist
      B4KE builds C4IX onto a third medium ─► reboot ─► C4IX

Each rung writes the next rung's disk. Nothing has to be built twice,
and nothing is lost when the machine stops.

## Milestones

- [x] **M7 — `ls` that understands directories.** ramfs is flat and
      every name is a full path, so `ls` prints all 151 of them. It
      should show one level: the files in a directory, and the
      subdirectories as names ending in `/`. Bar: `ls /bin` on the C4KE
      root disk fits in 80x25 and `ls` at the root shows directories,
      not their contents.
- [x] **M8 — `ps` and `top` that fit.** Most of the width is column
      titles, not data. Bar: default output ≤ 80 columns with the
      information that matters kept, and a `-w` for the wide form that
      exists today.
- [x] **M9 — C4KE's userland on the disk the player boots.** `top`,
      `bench`, `innerbench`, `mandel`, `raycast` and the sources
      `innerbench` needs (`c4.c`, `c4m.c`) are on `c4ke-root` and on
      none of the climb disks. Bar: after the walkthrough's C4KE boots,
      those all run.
- [x] **M10 — the firmware boots like a BIOS.** Banner, a RAM probe it
      prints, a drive probe, boot the first medium that has a boot
      image, and a retry loop that says `no valid media` and picks up a
      disk inserted while it waits. Bar: start the machine with no
      media, insert one, and it boots without being restarted.
- [ ] **M11 — `INSTALL`.** A C4DOS transient that writes a bootable
      medium: the kernel, the tools, and the sources for everything the
      next two rungs will build. Bar: `LADDER`, `INSTALL 1:`, eject,
      reboot, and C4KE comes up from drive 1 with its own source tree.
- [ ] **M12 — the same one rung up.** C4KE builds its tools and saves
      them; B4KE builds C4IX and installs a C4IX boot disk. Bar: three
      power-ons, three systems, each booted from a disk the previous
      one wrote.

### M7, M8, M9 — done, with the evidence

**`ls`.** ramfs stays flat; the directory structure is inferred from the
names. Everything under the prefix is split at the next `/`, files are
listed by what remains and the first component of anything deeper is
listed once with a trailing `/`. Root went from 151 full paths to 26
entries:

    c4sh> ls
    bench       benchtop    bin/        c4          c4cc        c4ke.vfs
    c4le        c4m         c4rdump     c4rlink     c4sh        cat
    echo        eshell      home/       init        innerbench  kill
    ls          ps          spin        top         type        usr/
    vfsload     xxd
    26 entries in /
    c4sh> ls /usr
    lib/   lisp/  src/
    3 entries in /usr/

`ls -a` is the old flat listing. Names that are not absolute — anything
a tool wrote into the ramfs, like c4rlink's `c4ix.c4r` — show at the top
level too, which is where they are.

**`ps`.** 170 columns to 56, and the wide one is still there under `-w`:

      PID PPID S P  NI  T%  C%     TIME  CYCLES     MEM  CMD
        0    0 W K   0 78% 77%     4254   4.0M   1.3M  kernel
        1    0 R K   9  0%  1%       51  53.0k      0  kernel/idle
        5    2 R U   9 10% 10%      553 553.2k      0  C4SH

Two things had to change beyond the printing: `print_int_compact`,
because `print_int_readable` spends ten columns on every number; and
c4sh's `ps` builtin, which calls `ps()` directly rather than exec'ing
`ps.c4r`, so it has to parse `-w` itself. **c4sh links ps.c** — a
rebuilt `ps.c4r` alone changes nothing about what the shell prints,
which cost an hour to notice.

**The userland.** `top bench benchtop innerbench mandel raycast c4 c4m
cat echo kill spin type xxd c4le` are now built onto the climb floppy,
plus `load-c4r.c`, `c4m.c` and `src/c4ke/c4ke.c` under the exact names
innerbench opens them by. Built in the disk rule with c4cc32 rather than
copied from `c4ke-root`, because that disk is derived from this one and
the dependency would be a circle. raycast is the exception: its enums
have expressions in them, which is a c4lc L11 feature c4cc has not got,
so the stock-c4 `raycast-dos32.c4r` is what goes on the disk.

Verified by booting C4KE against the climb floppy: `top -b -n 1` prints
the narrow table, `bench -q` runs, `mandel` renders in 7582 ms.

### M10 — done

`fw.c` has a `main()` now, and it is the BIOS. It runs when the machine
is started with **no program** — `cli.js -d disk` with nothing after it
— because then the firmware *is* the program (`loader.js` places it as
both). An image named on the command line still boots exactly as before;
that is the machine with something in ROM.

    c4bb -- the breadboard computer
    firmware: malloc, free, realloc, printf, 1 drives
    bios: 30 MB RAM ok (0x7000-0x1eff000)
    bios: drive 0 has c4dos32.c4r
    bios: booting c4dos32.c4r
    C4DOS version 0.1

The RAM line is a real probe, not a report: it writes a pattern near
each end of the heap and reads it back, so a machine configured with
more memory than it has says so here rather than three minutes into a
compile.

**A medium is bootable if it has `boot.c4r`, or a `boot.cfg` naming the
image.** The second exists so a disk that already carries a kernel under
its own name does not need a second 200 KB copy of it.

**Nothing to boot is not an error.** It says so, waits half a second off
the machine's own millisecond counter, writes `DISK_RESCAN` for each
drive and looks again — so a disk put in *while it waits* is picked up:

    bios: drive 0: no boot media
    bios: insert a bootable disk
    ... (disk inserted here) ...
    bios: drive 0 has c4dos32.c4r

**The loader is in the firmware.** `__bios_exec` is a port of `c4l.c`,
which is the same job in plain c4 — header, code, data, patch table,
constructors, entry, destructors. Two differences: the word size, and
that both segments are copied out of the read buffer so nothing depends
on where it landed. Where c4l has to rewrite its own call site (plain c4
has no indirect call), the board has `JSRS` and c4lc emits it, so a
variable holding an address is simply called.

### The reset — asked for while M10 was being built

An operating system that has just written a boot disk needs to say "now
boot it", and "stop the machine and start it again" is a different and
much bigger thing on a homebrew computer.

`RESET` (0x198): writing it halts like `POWER`, and the host zeroes the
arena, resets every CPU latch (`Machine.reset()`), re-reads the media
and boots the firmware again. **The drives survive** — they are the
media, and a disk that has just been written is the whole reason to
reset. A reset always boots the BIOS, never the image named at start,
for the same reason.

`bb_reboot()` in `include/c4bb.h`, and `src/c4bb/tools/reboot.c` builds
to an image with no u0, no C4DOS API and no kernel service in it, so the
same `reboot.c4r` is a C4DOS transient, a C4KE task and a C4IX program.

    A>RUN reboot.c4r
    rebooting...
    c4bb: soft reset

    c4bb -- the breadboard computer
    bios: drive 0 has c4dos32.c4r
    A>

### raycast, on all three

Shipped as the compiled `raycast-dos32.c4r` (c4lc, `-conforming`, stock
c4 opcodes plus TIME), which is what makes one image enough. Verified
rendering under **C4DOS**, under **C4KE**, and under **C4IX** — C4IX
runs C4KE binaries, so no third build was needed.

### Four things fixed before M11, 2026-08-28

**The BIOS must not need JSRS.** It did: c4lc will happily emit an
indirect call for a variable holding an address, and the board has the
opcode — but `JSRS` is c4m's, not c4's, and the BIOS and C4DOS are the
rungs a player reaches *before* they have extended their CPU. Giving
those rungs a c4m opcode gives away the milestone. So the firmware uses
c4l.c's trick instead: a stub function finds its own `ENT` by walking
back from its caller's return address, overwrites it with `JMP`, and
remembers the operand slot — an indirect call built out of a direct one
and a store. `-O` cannot turn the wrappers into tail calls, because a
tail call replaces the frame the walk depends on; the result is read
back through a local to stop it.

**And `putchar` is not base c4 either.** `PUTC` is opcode 39, one above
`EXIT`, and the firmware's own `printf` was using it — the thing that
*provides* the syscalls cannot need one. It writes the UART's transmit
register now, which is a plain `SI` and what the hardware actually does.

`src/c4bb/tools/opscan.mjs` is the pin, and it reports what an image
really uses:

    $ node src/c4bb/tools/opscan.mjs src/c4bb/fw/fw.c4r
    src/c4bb/fw/fw.c4r: ok, nothing above EXIT
      uses: LEA IMM JMP JSR BZ BNZ ENT ADJ LEV LI LC SI SC PSH AND EQ NE
            LT GT LE GE SHL SHR ADD SUB MUL DIV MOD OPEN READ CLOS PRTF EXIT

`make test-c4bb-baseops` is the pin in the suite: the BIOS, `dostar`,
`dosload` and `reboot` must have nothing above `EXIT`, and C4DOS
nothing above `TIME`.

`c4l.c` does the same job and refuses such an image by name, but it is
built for the host's word size and these are 32-bit. Scanned alongside:
`dostar`, `dosload` and `reboot` are clean; **C4DOS itself uses `TIME`
(53)** and nothing else above `EXIT`, which is the CLOCK.SYS rung and
already deliberate.

**The clock was twenty times too fast.** `CYCLES_PER_MS` was 1000 — "a
1 MHz machine" — while the simulator really executes fifteen to twenty
million instructions a second, so a simulated second went past in a
twentieth of a real one and `top`, which refreshes once a second,
redrew twenty times. Two changes: the default is 20000 (`cli.js -hz`
sets it), which is about what this simulator manages; and the
interactive loop now **paces** execution to the machine's own clock, so
the match is exact rather than approximate and `sleep()` sleeps.
Deterministic — nothing about *what* runs changes, only when the host
lets it — and `--fast` opts out for batch. `top -b -n 3` now refreshes
three times, 992 ms and 1002 ms apart, and C4KE measures itself at
"20.000 M" instructions per second.

**Backspace never erased anything.** `devices.js` holds a line in
`rxFifo` until Enter *precisely so that* not-yet-committed characters
can be erased — the comment says so — and then nothing ever erased
them. `cli.js` handles DEL and BS now (terminals disagree about which
the key sends): pop the last uncommitted byte, echo ` `. Typing
`lsXX<bs><bs>` reaches the guest as `ls`.

**`b4ke -t` runs `top` alongside the build.** Twelve compiles is five
minutes of a blank screen; innerbench solved this years ago and a build
is the same problem. The watcher shows which task is running, what it
is compiling and how much of the machine it is using, and is stopped
before the summary so the last thing on screen is the result:

        7    5 W U   9  0%  0%       13 260.7k      0  b4ke -t -f two.b4k
        8    7 R U   1  0%  0%       40 812.1k      0  top -b
        9    7 R U   9 99% 98%     5187 103.1M      0  c4sc.c4r -c 200000 compile -O -c -I src/c4ix ...

---

# Part 3 — clocks, and what each rung is allowed to need

Decided with the user 2026-08-28, after the pacing fix landed.

## Clocks

A fixed cycle-derived clock is what makes tests repeatable, and it is
also a lie: everyone's machine runs the simulator at a different speed,
so a "second" is whatever the host managed. Both are wanted, for
different jobs.

- **Simulated ms** (`TIME_MS` / the `TIME` opcode) stays cycle-derived
  and deterministic. Tests keep pinning it, and `cli.js -hz` sets the
  rate.
- **A real-time clock** the guest can read, which is what c4m already
  has: natively a timestamp call, under plain c4 a read of
  `/proc/uptime` (`c4m.c:1010`). C4KE measures itself against it, and
  it is why `top` can refresh once a second on any host.
- **A programmable interrupt timer**, so a kernel can ask for a tick at
  a real rate instead of deriving one from a cycle count. Optional: the
  cycle interrupt stays, and the PIT replaces it for kernels that want
  wall-clock scheduling.

`src/tests/test_timekeeping.c` is the user's existing experiment with
the modes (`CRTK_BUILTIN` / `CRTK_UPTIME` / `CRTK_CYCLES`) and is the
reference for what a guest should be able to choose between.

Both new devices are **registers, not opcodes**, which matters: a
program at the base-c4 rung can read a clock with an ordinary load and
does not need the CPU extended to do it.

## What each rung is allowed to need

| rung | budget |
|---|---|
| **C4DOS and its programs** | opcodes ≤ `EXIT`, plus the UART and the clock registers, which are devices rather than opcodes |
| **compilers for the next rung** | c4m level — `JSRS` and up. C4DOS may be stuck with `c4cc` until the player has added them, and that is the point |
| **C4KE** | c4m level: `JSRS`, traps/`ITH`, a real-time clock query, and optionally the PIT in place of the cycle timer |
| **C4IX** | the c4th extended opcodes on top. A player can experiment in a c4m-level C4KE for as long as they like before needing them |

The rule of the ladder: **a compiler is allowed to need what the system
it builds needs.** What must run at the lower rung is the *programs* —
bench, innerbench, c4m.c, mandel, rps, raycast, c4tui.

### Measured, and it is not free

    $ node src/c4bb/tools/opscan.mjs bench.c4r innerbench.c4r c4m.c4r \
                                     mandel.c4r rps.c4r raycast.c4r
    bench.c4r:      USES OPCD (48) at code+22 and 70 more
    innerbench.c4r: USES OPCD (48) at code+22 and 64 more
    c4m.c4r:        USES OPCD (48) at code+10 and 108 more
    mandel.c4r:     USES OPCD (48) at code+22 and 66 more
    rps.c4r:        USES OPCD (48) at code+22 and 61 more
    raycast.c4r:    USES TIME (53) at code+5958 and 2 more

Every one of them except raycast is over budget for the same reason:
**they are linked with `u0.h`**, whose opcode-request machinery is
`__c4_opcode`, which is `OPCD` (48). raycast is the exception because
its C4DOS build does not link u0 at all — which is exactly the shape
the others need. The work is per-program and is not a recompile: they
use u0 for pids, signals and the kernel task table, none of which
exists at the C4DOS rung.

### M13 and the PIT device — done

Two registers, both readable and writable with ordinary loads and
stores, so **a base-c4 program can read a clock and arm a timer without
the CPU being extended**:

    RTC_MS  0x19c   host milliseconds since power-on
    PIT_MS  0x1a0   tick every N real ms (0 = off)

`src/tests` proof that the clock is at the right rung — a program that
reads it and nothing else:

    $ node src/c4bb/sim/cli.js -m 8 clk.c4r
    rtc: 18 -> 6912 ms (6894 elapsed), sum -204937536
    $ node src/c4bb/tools/opscan.mjs clk.c4r
    clk.c4r: ok, nothing above EXIT

And the timer, `src/c4bb/tests/src/bb_pit.c`, four ticks at 100 ms:

    pit: 4 ticks, 100ms apart requested
    pit: spread over 300ms, real time

Three intervals between four ticks, on the wall clock. One thing that
had to be fixed to make it real: the jam check lives in
`machine.boundaryChecks()`, and **turbo.js only calls it when
`cycleInterval || pendingSignal`** — so the PIT fired in the step engine
and never in the one that actually runs. `m.dev.pitMs` joins that
condition. The host clock is consulted every 4096 cycles rather than
every instruction, which is a fifth of a millisecond at 20 MHz and
costs nothing measurable.

## Milestones

- [x] **M13 — the real-time clock.** A register the guest reads for
      host milliseconds, alongside the deterministic simulated one.
      Bar: `top` refreshes once a real second on a host that runs the
      simulator at half speed, and the pinned cycle-derived tests are
      unaffected.
- [x] **M14 — the programmable interrupt timer.** The device is done
      and pinned; C4KE is to schedule on it. `PIT_MS` (0x1a0) asks for
      a tick every N real milliseconds and raises the same
      `TRAP_HARD_IRQ` the cycle interrupt does, so a kernel that has a
      handler needs no new one.
  - [x] Masking preserves the timer's phase, so a kernel that hides
        from it in every critical path is not a kernel that never
        gets a tick.
  - [x] C4KE asks whether the device is there, asks whether it
        answers, and schedules on it when both say yes.
  - [x] A machine without it, or with one that does not answer, boots
        and runs exactly as it did before.
- [ ] **M15 — per-rung opcode budgets in the suite.** `opscan` already
      takes a ceiling; give each rung's images their own and make it a
      test, so "the player must extend the CPU before X" is asserted
      rather than described.
- [x] **M16 — u0-free builds of the C4DOS-rung programs.** `c4.c`,
      `c4m.c`, mandel and rps at opcodes <= EXIT (plus TIME), the way
      `raycast-dos` already is. **bench and innerbench are not on this
      list** — they are C4KE and C4IX programs and stay that way.
  - [x] `c4.c` and `c4m.c`, as separate images from the u0 ones.
        Running `c4m.c` inside `c4.c4r` inside the board is the
        software proof of the VM, and it must need no extended opcode
        to be that.
  - [x] mandel and rps, from the same sources the C4KE builds use.

## Notes taken while scoping

- **The tools are not missing, the disk is.** `src/c4bb/images/c4ke-root`
  already carries `top`, `bench`, `innerbench`, `mandel`, `raycast`,
  `c4.c4r`, `c4m.c4r` and the sources. `c4dos-c4ix32` — the disk the
  walkthrough boots — carries almost none of it. M9 is mostly a
  packaging question, and M11 is the answer to it: the player *installs*
  them rather than the floppy having shipped with them.
- **Console width is a HOMEWARD decision, not a c4bb one.** The CLI
  writes to the host terminal and has no width. Widening the game's
  console to 180x25 and narrowing `ps` are independent, and both are
  worth doing — the second one helps at any width.
- **The firmware already has what a BIOS needs except a loader.**
  `fw.c` has malloc, a printf, and the device registers; what it cannot
  do is load a `.c4r` and jump into it, which is what `sim/loader.js`
  does from the host today. `src/c4dos/dosload.c` is that loader in
  strict c4, so the code to copy exists.

### The cycle columns — implemented 2026-08-28

Both kernels now keep the count in **two words**, decimal carry:

    cycles = cycles_hi * 1000000000 + cycles

Decimal rather than binary because printing it is then two ordinary
numbers and nothing is ever wider than an `int` — this system has no
64-bit type and adding one for a counter would be the wrong trade.
`kernel_add_cycles` (C4KE) and `sched_add_cycles` (C4IX) carry on the
way in; `print_cycles_compact` / `print_cycles_readable` (C4KE's ps) and
`upadcycles2` (libc4ix) print the pair, scaling from G because the high
word is already in billions. A negative delta means the VM's counter
wrapped between samples and is dropped: it is not a slice anyone ran.

**It is an ABI change, and it touched four places that all had to agree**
— `include/u0.h` and `include/c4ke.h` (C4KE's exported task record),
`src/c4ix/c4ix.h`'s `CK_KTE_*` (the same record, spoken by C4IX's C4KE
compatibility layer), and `TASKINFO_WORDS` for C4IX's own. Getting the
third wrong segfaults C4IX the moment it runs C4KE's `ps`, which is
exactly what happened.

**A pre-existing bug found on the way.** `TASKINFO_WORDS` was 9: seven
integers plus `TASK_NAME_MAX` bytes of name. Sixteen bytes is two words
on a 64-bit host and **four** on the board — so on 32 bits `utaskinfo`
wrote two words past the caller's array, every call. It is sized in
words now, generously, with a comment saying why.

`top`'s per-interval delta also had to learn about the carry: a delta
taken across one looks negative and is a billion more.

Goldens regenerated and read before committing: `x5-c4m.txt` (the demo's
ping/pong interleaving moved, twice, because the task struct changed
size — stable across three runs each time), `ck-ps.txt` and
`ck-top.txt` (which had not been regenerated since `ps` went narrow).

### Detecting the clock

`c4_info()` on the board sets **`C4I_PIT` (0x800)**, and `bb_has_clock()`
in `include/c4bb_info.h` tests it. **A separate header, deliberately:**
everything in `c4bb.h` is a plain load or store and costs no opcode
above `EXIT`, which is what lets a C4DOS-rung program read the clock;
asking `c4_info()` is `INFO`, opcode 57, and c4cc compiles every
function in a header whether it is called or not — `reboot.c4r` went
over budget the moment the check shared a file with the accessors, and
`make test-c4bb-baseops` said so. A kernel cannot simply poke `0x19c` to
find out whether the registers exist: **native c4m has no device window
there**, so the poke is a wild access rather than a zero. The capability
is announced, and anything that does not see the bit keeps doing what it
did before — which is what C4KE will do when it moves onto the PIT.

### M14: C4KE schedules on the timer

The kernel that boots on the board now takes its scheduling tick from
the wall clock instead of from a count of instructions, and the two
paragraphs that used to say what the cycle interrupt was for say it
best:

    c4ke: measuring instructions per second, if this step gets stuck, pass -a to c4m...
    c4ke: instructions per second roughly: 1.000 M, measurement cycles: 200.265 k
    c4ke: setting cycle interrupt to 50003 (50.003 k) cycles

became

    c4ke: scheduling on the interval timer, every 10ms of real time

The measurement existed to turn "how fast is this host" into a cycle
count, and a timer that counts milliseconds has no use for the answer.
Deleting the question took the board's boot from **726ms and 2.047 G
cycles to 3ms and 76.5 k cycles** -- a good deal of that was the
measurement, and the rest is that a kernel booting on a real 10ms tick
is not being interrupted 20 times on the way up.

**Detection is two questions and both have to be asked.** `__c4_info()`
answers the first, and it has to: 0x1a0 cannot be probed blind, because
on every host that is not the board it is ordinary memory, and a store
there corrupts whatever lives at 0x1a0 rather than doing nothing. Only
once the capability bit says the window is real is it safe to ask the
second, which is whether the device behind it behaves -- arm an
interval, read it back, mask it, read that back. A machine that claims
the bit and answers something else schedules on the cycle counter
exactly as before, and so does native c4m, and so does a C4KE running
nested inside a c4m on the board, because the inner VM's `c4_info()` is
its own and does not carry the board's bits. `-c nn` is the override,
and it is the honest one: it says "use the cycle interrupt, at this
interval", which is a thing to say only if that is what you want.

### What the kernel taught the hardware

Masking. A kernel hides from its own timer on the way into every
critical path and comes back out on the way from it -- that is what
`critical_path_start`/`critical_path_end` are, and on a busy kernel it
happens many times per tick. The PIT as first built restarted its
countdown on every write of the interval, so **a kernel that took a
critical path more often than once per period would never be
interrupted again.** It would mask itself to death, and the symptom
would have read as "the scheduler stopped", not as a timer bug.

So writing 0 now stops the timer and leaves the deadline where it is,
and re-arming the same interval resumes toward it; only a genuinely
different interval starts a new countdown. The cycle interrupt has this
for free, because its counter is free-running and `cycle % interval`
does not care when the interval was written -- the PIT had to be told.
`src/c4bb/tests/src/bb_pit.c` has a second pass for it: the same four
ticks over the same 300ms, with the wait loop masking and unmasking on
every turn.

    pit: 100ms apart requested
    pit: free running -- 4 ticks over 300ms, real time
    pit: masked every pass -- 4 ticks over 300ms, real time

Not a hypothetical: with the phase-preserving line taken back out, the
second pass delivers no ticks at all.

### What a faster kernel found: the reaper was on the wrong clock

`test-task-mem` failed the moment C4KE stopped burning cycles on the
way up, and the failure is worth keeping written down because the bug
was always there.

    leak: round allocated 24 of 24 blocks     x4
    fw: malloc(262160) failed: 2 free blocks, 156920 bytes
    leak: round allocated 18 of 24 blocks
    test_leak: round 5 only got some of its memory

`test_leak` starts twelve children in turn, each of which allocates 6 MB
and frees none of it, and it passes only because the kernel takes a dead
task's memory back. The idle task does that -- and it used to wait a
second between sweeps. Two things were wrong with that. The small one is
that the wait bought nothing: `kernel_tasks_zombie` already gates the
sweep, and one sweep takes it to zero, so the table was never being
walked without something to find. The large one is that the second is
measured with `__time()`, which is **derived from the cycle counter** --
so it is a count of work, not an amount of time. The old kernel spent
2 G cycles measuring its own speed before it ever scheduled anything,
and that made the wait look short. The new one boots in 76 k cycles, and
suddenly twelve dead tasks' worth of heap was sitting there waiting for
a clock that was barely moving.

So the sweep now happens whenever there is something to sweep. It was
always safe there: `kernel_task_finish` has already copied the exit code
into whatever task was waiting on this one, so nothing still needs to
read the corpse.

This is the shape of thing to expect more of. Anything in C4KE that
says "after N milliseconds" is really saying "after N cycles", and the
two have just stopped being interchangeable.

### And one from the machine itself

`test-c4dos-build32` -- C4DOS compiling this kernel on the board --
failed on the first attempt with

    cpp: c4ke.c:1135: backslash-newline continuation is not supported

The new `critical_path_start`/`critical_path_end` had been written
across three lines with backslashes, which gcc's preprocessor takes
without comment. The one that runs on the machine (`src/c4dos/cpp.c`)
does not, and this kernel has to be buildable by it or the ladder has a
missing rung. They are one line each now, with a comment saying why.

### M16: the C4DOS rung gets c4, c4m, mandel and rps

All four at opcodes <= EXIT, and none of them needed a second source, a
`-D` or an `#ifdef` -- which matters, because the compiler that runs on
the machine has no `#` to read.

**c4 and c4m were already there.** Hand c4cc the plain sources and it
walks into `c4m.c`'s `#if C4_ONLY` branch -- the one written for
running under plain c4 -- because c4cc skips `#` lines and compiles the
code between them. What comes out uses nothing above `EXIT`, and it
runs:

    $ ./c4m32 load-c4r.c -- c4-dos32.c4r c4m.c load-c4r.c -- hello32.c4r
    yello
    exit(0) cycle = 14957271

That chain is the VM proved in software, and it has to be provable at
this rung or it proves nothing about what the machine could already do
on the day it booted. On the board, from the C4DOS prompt, with the
whole stack underneath:

    A>RUN c4.c4r c4m.c load-c4r.c -- mandel.c4r 24x10 -m
    Custom size: 24x10
    c4m: unable to open uptime file
    ......,''''''~~=[&~~',,,
    .....,'''''~~~=;..==~',,
    ...
    exit(0) cycle = 48432715

(The uptime complaint is that branch's clock: it reads `/proc/uptime`,
because the one host it was written for is a Linux box running real c4,
and c4 has no `TIME`. Harmless, and it is why the render claims 0ms.) The u0 and preprocessed builds of both are the
ones one rung up, and they are separate images: those reach `INFO` and
`JSRS`.

**mandel and rps needed `include/u0lite.h`.** Not a stripped u0 --
u0's own definitions, copied, of the parts that need no kernel and no
opcode: `strlen`, `strcmp`, the character tests, and the same linear
congruential generator. The names match u0's exactly, so a program's
source does not know which build it is in:

    c4cc -o prog.c4r include/u0.h     prog.c   # the C4KE / C4IX build
    c4cc -o prog.c4r include/u0lite.h prog.c   # the C4DOS build

That is the whole mechanism. c4cc compiles every function in a source
it is handed whether it is called or not, so what makes an image
base-c4 is **which library it was handed**, and nothing else.

One thing was deleted rather than ported. mandel's `main()` used to
patch `mandelbrot_render()` into a jump to one of two bodies, choosing
the `__c4_invoke()` one when `__c4_info()` said we were on plain c4,
where a pure function is called faster. That cost `OPCD`, `INFO` and
the invoke -- three opcodes the rung does not have -- to buy some
milliseconds on the one host that is not the interesting one. The
picture out of both builds is byte-identical.

---

## Examined, and now fixed: C4IX's cycle column went negative

Asked for 2026-08-28. The finding, so it does not have to be found
again.

**It is the accumulator, not the sampling.** `sched.c:272` does

    t->cycles = t->cycles + (__c4_cycles() - t->cycles_in);

and the *delta* is fine even when the counter wraps, because the
subtraction wraps with it. What overflows is `t->cycles` itself —
`int cycles` in `c4ix.h:291`, 32 bits on the board — after 2^31 cycles,
which the C4IX build reaches several times over. `user/ps.c:59`'s
`total` sums those, so it goes wrong sooner.

`C4CY` is a 32-bit read (`CYC_OUT` in the microcode), but the board also
exposes the full count as `CYCLE_LO`/`CYCLE_HI` device registers at
0x110/0x114 — so the width is available, just not through the opcode.

**Three shapes, cheapest last:**

1. Read `CYCLE_HI`/`CYCLE_LO` instead of `C4CY`. Board-only — native c4m
   has no such registers — so C4IX would need a host check for
   something it should not have to care about.
2. Keep counting in units of 1024 cycles. One line, loses precision, and
   every number printed anywhere would change.
3. **A second word.** `cycles_hi` alongside `cycles`, bumped when the
   low word wraps, and a printer that combines the pair. About twenty
   lines across `sched.c`, `c4ix.h`, the task export in `task.c` and
   `user/ps.c` — and it touches the kernel/userland task-info ABI, so
   those two have to change together.

C4KE has the same shape in `TASK_CYCLES` and will do the same thing at
the same point; whatever is done should be done to both.
