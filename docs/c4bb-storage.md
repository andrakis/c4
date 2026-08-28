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
