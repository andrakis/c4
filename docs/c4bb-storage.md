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
