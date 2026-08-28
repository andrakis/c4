# Climbing the ladder by hand: C4DOS to C4KE to C4IX, on the breadboard

Every rung of this is a system building the next one, inside a simulated
machine, with nothing brought in from the host except the floppy it
booted from. This is the walkthrough: what to type, what happens, and
what it costs.

    make c4dos-c4ix32
    node src/c4bb/sim/cli.js -i -m 64 -d c4dos-c4ix32

No image on the command line: the firmware boots like a BIOS, finds the
floppy in drive 0 and boots what its `boot.cfg` names
(docs/c4bb-storage.md M10). Naming `c4dos32.c4r` at the end still works
and is the machine with something in ROM instead.

## The session

| where | type | what happens |
|---|---|---|
| `A>` | `LADDER` | the seed compiler rebuilds itself and the preprocessor, reaches a fixed point, and the tools it just built compile C4KE from its own source |
| `A>` | `RUN dosload.c4r c4ke.c4r` | boots the kernel the machine compiled. C4DOS hands over its RAM disk on the way out, so everything just built is still there |
| `c4sh>` | `tar x c4ix-src.tar` | C4KE unpacks C4IX's source into its own RAM filesystem — 43 files |
| `c4sh>` | `b4ke -t -f c4ix.b4k` | twelve modules through the compiled compiler, then the link. This is the long one — `-t` runs `top` alongside so there is something to watch |
| `c4sh>` | `c4ix.c4r c4ix-sh.c4r` | boots C4IX — protected mode, preemption — with a shell |
| `c4ix:/$` | `ls`, `ps` | C4IX, running, showing its own tasks |

Three systems, each one compiled by the one before it, in a single
power-on of a machine whose CPU is 89 microcoded opcodes.

## What it actually does, verified end to end

Driven by a script with sleeps in it (`-i` streams a pipe now), on a
64 MB machine:

    A>LADDER
    ...
    c4cc: wrote 187771 bytes to ram:c4ke.c4r
    A>RUN dosload.c4r c4ke.c4r
    dosload: 4194304 bytes released, loading c4ke.c4r
    ...
    c4sh>tar x c4ix-src.tar
    tar: extracted 43 files, 257816 bytes
    c4sh>b4ke -f c4ix.b4k
    b4ke: C4IX, twelve modules and a link
    ...
    c4rlink: wrote 130665 bytes to ramfs:c4ix.c4r
    b4ke: 13 ran, 0 skipped, 0 failed
    c4sh>c4ix.c4r c4ix-sh.c4r
    C4IX booting, 203277198 cycles spent loading the kernel image
    c4ix: protected mode on for user tasks, preemption on
    c4ix-sh -- 'help' for builtins, 'exit' or end-of-file to leave
    c4ix:/$ ls
    ram/
    c4ix:/$ ps
      ID  PPID STATE   PRIV    SYSCALLS     TRAPS      CYCLES  NAME
       0     0 ready   kernel         0   2122118 ...           boot
       1     0 wait    kernel         0         7     69.883k  init
       2     1 wait    user          28        28    107.859k  c4ix-sh.c4r
       4     2 run     user          37        41     53.708k  c4ix-ps.c4r
    4 tasks

C4IX's own cycle column goes negative past 2^31 — it accounts in a
single 32-bit int and the whole climb is billions of cycles. Noted, not
fixed here.

## What each rung needs

The reason this is a *ladder* and not a menu is that each rung needs
strictly more machine than the last, which is the whole shape of
HOMEWARD:

| rung | needs |
|---|---|
| C4DOS | a disk to read, a RAM disk to write, the base opcode set |
| the tools | `dostar`, c4cc, cpp — and enough RAM disk for their output |
| C4KE | traps, a cycle interrupt, `ITH` — the kernel is a trap handler |
| C4IX | the ten fused opcodes (the compiled compiler uses them), and protected mode if you want it to mean anything |

## The pieces that had to exist for this to work at all

- **`tar`** (`src/c4ke/bin/tar.c`) — C4KE could not unpack anything.
  `dostar` does this one rung down onto the C4DOS RAM disk; this does it
  onto C4KE's ramfs, and the format and parser are deliberately the
  same. Without it, C4KE could only build what C4DOS had already
  staged for it, which is not a system building the next one.
- **`b4ke` and `c4ix.b4k` on the C4DOS floppy** — they were on the C4KE
  root disk, which is not the disk you get here.
- **`cli.js -i` streaming its input.** C4KE hands the console to the
  *focused* task, so a command queued behind a five-minute compile is
  simply lost. Prefeeding a whole pipe is right for a batch session and
  wrong for a scripted interactive one; `-i` now feeds stdin as it
  arrives whether it is a terminal or a pipe, which is what lets this
  walkthrough be driven by a shell script with sleeps in it.

## Keeping it

The session above lives entirely in RAM: C4DOS's RAM disk becomes
C4KE's ramfs, and when the machine stops, all of it goes. The machine
now has **drives** (docs/c4bb-storage.md), so it does not have to:

    node src/c4bb/sim/cli.js -i -m 64 -d c4dos-c4ix32 -w mydisk c4dos32.c4r

`-w` attaches a writable medium as drive 1 — the directory is created
if it is not there, because a blank disk is a real thing. Then:

| where | type | what happens |
|---|---|---|
| `A>` | `LADDER` | as before |
| `A>` | `RUN bbsave.c4r 1:` | every RAM-disk file onto drive 1 — including the kernel it just compiled |
| | *power off* | |
| | `cli.js -i -m 64 -d mydisk` | boot what you built, tomorrow |

or, without stopping the machine at all, `RUN reboot.c4r`: the arena is
zeroed, the drives are re-read, and the BIOS boots whatever is in them
now. Which is the point — on a homebrew computer, "switch it off and on
again" is a bigger ask than it sounds.

and the same inside C4KE, with `save 1:` for the ramfs, which is how
the C4IX that `b4ke` just built survives to be booted on its own.

## The machine's clock

`-hz` sets how fast the machine claims to be, default 20 MHz, and an
interactive run is **paced to it** — a simulated second is a real one,
`top` refreshes once a second and `sleep()` sleeps. `--fast` lets it
run flat out instead, which is what the batch tests want. It changes
nothing about what executes: the cycle counts are the same either way.

## In the browser

The same disk works: open `src/c4bb/web/index.html`, choose **c4dos32**,
and type the same things. The page's arena is 128 MB.

## Non-interactively

`make test-c4dos-c4ix32` runs the C4DOS-only route (`IX` under DOS) and
checks it reaches a C4IX shell. `make test-c4bb-storage` pins the drives:
two of them, one writable, and a medium that outlives the machine that
wrote it.
