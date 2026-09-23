libjs: c4m in JavaScript
========================

A direct interpreter for the c4m virtual machine, written the way
`c4m.c`'s dispatch loop is: one switch over an `Int32Array`. It boots the
32-bit C4KE and C4IX images, in Node or in a browser Web Worker.
Design and progress: [docs/libjs-design.md](../docs/libjs-design.md).

It shares its memory map, device registers, `.c4r` loader, disk
controller, keyboard and clocks with the c4bb breadboard computer
([src/c4bb](../src/c4bb)), by importing them. The difference is the CPU:
c4bb runs microcode, libjs runs opcodes, and MALC/FREE/PRTF happen on the
host side as they do in native c4m, with no guest firmware.

Running it
----------

The images are the 32-bit ones c4bb builds:

	make c4bb-images

Then, from the repo root:

	node libjs/cli.js src/c4bb/images/hello32.c4r
	node libjs/cli.js -i -d src/c4bb/images/disk src/c4bb/images/c4ix32.c4r
	node libjs/cli.js -i -d src/c4bb/images/disk src/c4bb/images/c4ke32.c4r
	printf 'ps\nexit\n' | node libjs/cli.js -d src/c4bb/images/disk src/c4bb/images/c4ix32.c4r

`-i` streams the keyboard; without it, piped stdin is fed up front and
the run ends at end-of-file. `-s` prints cycles and instructions per
second. The flags match c4bb's `src/c4bb/sim/cli.js`.

64-bit `.c4r` images, such as the ones at the repo root, do not load.

Testing
-------

	make test-libjs

This compares every test image byte for byte against native `c4m32` (or
64-bit `c4m` where c4m32 cannot be trusted), runs the fused-opcode
differential, the PIT, and boots both kernels with grep assertions.

Files
-----

| File | What it is |
|---|---|
| `c4m.js` | The machine: registers, the dispatch loop, traps, syscalls |
| `bus.js` | Addresses below 0x1000: the device window and the ROM |
| `heap.js` | MALC/FREE/RALC, with the size side table native c4m keeps |
| `printf.js` | PRTF, glibc's format rules over 32-bit words |
| `boot.js` | Builds the machine, loads an image, assembles the call to main |
| `cli.js` | The Node runner |
