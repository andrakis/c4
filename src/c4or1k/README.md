# c4or1k

An OR1000/OpenRISC emulator, capable of booting Linux, written in C
and compiled by c4lc. A port of [jor1k](https://github.com/s-macke/jor1k)
(a JavaScript OR1000 emulator) to the C4 VM toolchain. See
`docs/c4or1k-design.md` for the milestone roadmap and design rationale.

Console only -- no framebuffer, no keyboard device. Terminal raw mode
is handled by `run-c4or1k.sh`, an external wrapper, not by the VM (the
C4 VM has no ioctl/termios facility, see that script's header comment).

## Status: M6 done -- it boots real Linux to an interactive shell; M7 (-O) and M8 (SR_SM-safe TLB lookup cache) landed since (M0-M4 preserved in docs/c4or1k-design.md)

    make c4or1k-boot                # boot a real kernel; N=<steps> to change the budget (default 2M)

Boots an unmodified `vmlinux.bin` from the reset vector through the
full kernel init sequence, mounts basefs.json's root filesystem over
9p (`VFS: Mounted root (9p filesystem) readonly on device 0:12.`),
execs real userspace (`/etc/init.d/rcS`, `busybox`, `udhcpc` -- fails
gracefully, no ethernet device exists), and reaches a real, interactive
BusyBox shell prompt (`~ $`) via `/etc/inittab`'s `ttyS1::respawn:
-login -f root`. Needs a large instruction budget (order of hundreds
of millions to low billions, depending mostly on `udhcpc`'s own DHCP
retry/backoff timing) and `stdbuf -oL` to see the output live, since
stdio is fully buffered against a non-tty output otherwise. See
`docs/c4or1k-design.md`'s M6 section for the exact transcript and a
known rough edge (the console's own login, ttyS0, hasn't yet been
demonstrated with live typed input racing against `ttyS1`'s).

Two real bugs surfaced getting here, both found by bisecting a real
boot under `gdb` rather than by inspection -- `bootfs.c`'s 32-bit
record reader wasn't sign-extending `-1` sentinels (silently misread
as a huge positive "index" that walking an empty directory then
dereferenced as a wild pointer), and `basefs.json`'s `/etc/inittab`
turns out to spawn a second getty on a second UART this project didn't
have yet. See `docs/c4or1k-design.md`'s M5 section for both stories in
full -- they're worth reading if you're adding a new device or backing
store, since both bugs were badly misleading before they were pinned
down.

Devices this project doesn't implement (virtio-block, DRM, ATA,
keyboard, touchscreen, ethernet, RTC) all probe and fail *gracefully*
instead of wedging the boot -- `mmio.c` reports each unique
unimplemented device once (`warn_once()`), not once per access, since
several of them get polled continuously once real Linux is actually
running rather than panicking early.

`cpu.c`/`mem.c` implement the full non-privileged OR1000 integer ISA,
SPRs, the SR flag register, exception delivery, and DTLB/ITLB miss +
permission checks (not a page-table walk -- that's the guest kernel's
job). `uart.c` (two units -- ttyS0 is the real console, ttyS1 has no
host backing and just blocks forever like an unconnected port),
`mmio.c`, `console.c` make it interactive. `boot.c` loads a raw kernel
image and patches its DTB's memory-size property. `bootfs.c` +
`virtio.c` + `virtio9p.c` are the 9p root filesystem -- an in-memory
inode tree loaded from `tools/mkbootfs.js`'s offline flattening of
jor1k's `basefs.json`, served over a virtio-mmio transport and a
9P2000.L protocol handler.

    make c4or1k-m1-check           # M1: full ISA, diffed against jor1k
    make c4or1k-m2-check           # M2: SPRs/exceptions/TLB, diffed against jor1k
    make c4or1k-m3                 # M3: interactive echo server (real terminal)
    make c4or1k-m3-check           # M3: same, piped input, diffed byte-for-byte
    make c4or1k-m3-int-check       # M3: interrupt-driven variant of the same test

M1/M2 are diffed bit-for-bit against jor1k's own `safecpu.js`
(`tools/or1k-oracle.js`); M3's two echo servers are verified by piping
known input through and diffing the echoed output. M1 caught a
delay-slot bug in the *test program* (not cpu.c) plus two bugs in the
oracle itself. M2 added a `.org` assembler directive (`tools/asm.py`)
for planting exception handlers at real vector addresses, and
surfaced the EPCR-offset distinction between `EXCEPT_SYSCALL` and
every other exception type. See `docs/c4or1k-design.md`'s per-
milestone sections for all of it.

M0's throughput result (~1.37M guest instructions/sec on a tight
8-opcode loop) is preserved in the design doc; the full decode hasn't
been re-benchmarked in isolation, though the M4 boot (~100M
instructions in a few minutes) gives a rough real-workload figure.
