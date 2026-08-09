# c4or1k

An OR1000/OpenRISC emulator, capable of booting Linux, written in C
and compiled by c4lc. A port of [jor1k](https://github.com/s-macke/jor1k)
(a JavaScript OR1000 emulator) to the C4 VM toolchain. See
`docs/c4or1k-design.md` for the milestone roadmap and design rationale.

Console only -- no framebuffer, no keyboard device. Terminal raw mode
is handled by `run-c4or1k.sh`, an external wrapper, not by the VM (the
C4 VM has no ioctl/termios facility, see that script's header comment).

## Status: M6 done -- it boots real Linux to an interactive shell. M7-M13: hosted perf work (c4mp host, -mcisc, a real JIT emitting c4m bytecode -- fastest hosted config ~2x the original). M14: the NATIVE build boots the same kernel to the same shell in ~4 SECONDS (vs ~25 hosted minutes originally) -- byte-identical output, same sources. M15: an ethmac ethernet device + a pure-C synthetic LAN peer bring eth0 up and get a real DHCP lease (native and hosted alike). (M0-M4 preserved in docs/c4or1k-design.md)

    make c4or1k-boot-native N=700000000  # THE fast path: gcc-compiled emulator, full Linux boot to a real shell in ~7s (see M14)

    make c4or1k-boot                # boot a real kernel under c4m; N=<steps> to change the budget (default 2M)
    make c4or1k-boot-mp              # same, under c4mp instead -- ~18-20% faster, same .c4r, zero source changes (see M11)
    make c4or1k-boot-cisc            # under c4mp, compiled with c4lc's -mcisc (LXI/SXI fused opcodes) -- c4m cannot run this image at all (see M12)
    make c4or1k-boot-jit             # M13 v2: the FASTEST configuration -- the JIT build under c4mp. Translates guest code (ALU runs, loads/
                                     # stores, fused loops, call/return/branch terminators) into real c4m bytecode at runtime; ~28% faster than
                                     # M12 on the standard 60M-instruction benchmark with 56% of instructions running inside translated blocks,
                                     # byte-identical output. On by default in this image (-nojit to disable). See docs/c4or1k-design.md's M13
                                     # section for the whole story, including why v1 of this same JIT was net-slower and what changed.

Boots an unmodified `vmlinux.bin` from the reset vector through the
full kernel init sequence, mounts basefs.json's root filesystem over
9p (`VFS: Mounted root (9p filesystem) readonly on device 0:12.`),
execs real userspace (`/etc/init.d/rcS`, `busybox`, `udhcpc` -- which
now gets a real DHCP lease from the M15 ethernet device + synthetic
LAN peer), and reaches a real, interactive
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
keyboard, touchscreen, RTC -- ethernet IS implemented as of M15) all
probe and fail *gracefully*
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
