# c4or1k — an OpenRISC emulator that boots Linux, compiled by c4lc

c4or1k emulates an OR1000/OpenRISC machine well enough to boot an unmodified
Linux kernel to an interactive BusyBox shell. It is a port of
[jor1k](https://github.com/s-macke/jor1k), a JavaScript OR1000 emulator, to
the C4 toolchain: the sources are in the c4lc dialect of C, compiled by c4lc
into `.c4r` images that run under c4m or c4mp. The same unmodified sources
also compile with gcc, through a small shim, into a native executable.

Console only: no framebuffer and no keyboard device. The kernel boots from
the reset vector, mounts a root filesystem over 9p (virtio-mmio), brings up
`eth0` with a DHCP lease from a synthetic LAN peer, and reaches a shell on
the first UART. In boot mode the terminal is put in raw mode, so a typed
Ctrl+C reaches the guest; `Ctrl-] x` quits the emulator and `Ctrl-] Ctrl-]`
sends a literal Ctrl-].

Status: milestones M0–M18 are done. M0–M6 got real Linux to a shell; M7–M13
were hosted performance work (c4mp as host, c4lc's `-mcisc`, a JIT that
emits c4m bytecode); M14 the native build; M15 ethernet and DHCP; M16 the
extended filesystem and single-precision floating point; M17 idle detection
(an idle shell drops to about 4% CPU); M18 raw terminal mode, Ctrl+C
forwarding, and a synthetic DNS responder. The native build boots to the
shell in about 3–4 seconds (M15 measured 3.6 s, M17 3 s); the best hosted
configuration took about 13 minutes for the same boot.

## Needs: the jorconsole checkout

The kernel, the root filesystem and the reference CPU are not in this repo.
The Makefile expects [jorconsole](https://github.com/andrakis/jorconsole)
checked out beside it as `../jorconsole`, with its `jor1k` and
`jor1k-sysroot` submodules:

| Path | Used for |
|---|---|
| `../jorconsole/jor1k-sysroot/or1k/vmlinux.bin` | The kernel (already decompressed). |
| `../jorconsole/jor1k-sysroot/or1k/basefs.json` and `../jorconsole/jor1k/sys/or1k/basefs` | The standard root filesystem. |
| `../jorconsole/jor1k-sysroot/fs.json` and `../jorconsole/jor1k-sysroot/fs` | The extended filesystem (bash, perl, X libraries). |
| `../jorconsole/jor1k/js/worker/or1k/safecpu.js` | jor1k's own CPU, which `tools/or1k-oracle.js` runs for the M1/M2 checks. |

`tools/mkbootfs.js` flattens a filesystem manifest and its files into
`images/bootfs.idx` + `images/bootfs.blob` (or `bootfs-ext.*`), because
`bootfs.c` has no JSON parser. The boot targets build these on demand; they
are generated and untracked (`src/c4or1k/.gitignore` ignores `images/`).
Building the blob decompresses files with the host's `bunzip2`.

## Build

| Target | Output |
|---|---|
| `make c4or1k.c4r` | The hosted image, c4lc `-O`. Runs under c4m or c4mp. |
| `make c4or1k-cisc.c4r` | Built with c4lc `-mcisc`, which emits c4mp's `LXI`/`SXI`. Runs under c4mp only. |
| `make c4or1k-jit.c4r` | Adds `jit.c` and `-D C4OR1K_JIT=1`: translates guest code into c4m bytecode at run time. |
| `make c4or1k-native` | `gcc -O2 -include src/c4or1k/native.h` over the same sources. |

The hosted builds compile each module separately and in parallel
(`C4LC_JOBS`, default `nproc`), then link with c4rlink.

## Run

    c4or1k PROGRAM.bin                          test mode: run a flat big-endian binary at address 0, dump registers
    c4or1k PROGRAM.bin -r                       console mode: the program talks to the UART until it halts
    c4or1k vmlinux.bin -b MAXSTEPS [IDX BLOB]   boot mode: load a kernel, start at the reset vector
                                                (MAXSTEPS 0 = unbounded; IDX/BLOB default to images/bootfs.*)
    ... -nojit                                  last argument: turn translation off in the JIT build

Hosted, prefix with `./c4m load-c4r.c -- c4or1k.c4r` or `./c4mp c4or1k.c4r`;
native, run `./c4or1k-native`. The boot targets wrap this:

| Target | Runs |
|---|---|
| `make c4or1k-boot-native N=700000000` | The native build, full boot to a shell. The fast path. |
| `make c4or1k-boot-native-ext N=900000000` | The same with the extended filesystem. Native only; the blob is too large to be practical hosted. |
| `make c4or1k-boot-mp` | `c4or1k.c4r` under c4mp. |
| `make c4or1k-boot-cisc` | `c4or1k-cisc.c4r` under c4mp. |
| `make c4or1k-boot-jit` | `c4or1k-jit.c4r` under c4mp: the fastest hosted configuration. |
| `make c4or1k-boot` | `c4or1k.c4r` under c4m (see Limits). |

`N` is the instruction budget and defaults to 2,000,000, which only gets
partway through the kernel. A full boot needs hundreds of millions to low
billions, depending mostly on `udhcpc`'s DHCP retry timing. When the output
is not a terminal, run through `stdbuf -oL` to see it live.

## Test

None of these are in `make test`. The M1 and M2 checks build all three
emulators and need `../jorconsole`.

| Target | Checks |
|---|---|
| `make c4or1k-m1-check` | The full integer ISA, bit for bit against jor1k's `safecpu.js`, for the default, JIT and native builds. |
| `make c4or1k-m2-check` | SPRs, exceptions and TLB, the same way. |
| `make c4or1k-m3-check` | A polled UART echo program: piped input comes back byte for byte. |
| `make c4or1k-m3-int-check` | The interrupt-driven version. |
| `make c4or1k-m1`, `c4or1k-m2`, `c4or1k-m3` | The same programs, run without the diff (`m3` interactive). |

The test programs are `tests/*.s`, assembled by `tools/asm.py` into
`tests/*.bin` (untracked).

## Layout

| File | What it is |
|---|---|
| `cpu.c`, `cpu.h` | The OR1000 integer ISA, SPRs, the SR flags, exception delivery, DTLB/ITLB checks, and the batch runner `cpu_run_batch`. |
| `fpu.c`, `fpu.h` | Single-precision floating point (`lf.*`). |
| `jit.c`, `jit.h` | The JIT: guest code to c4m bytecode, cached by guest physical address. Only in `c4or1k-jit.c4r`. |
| `mem.c`, `mem.h` | Guest RAM. |
| `mmio.c`, `mmio.h` | Routes a physical address to a device by its top byte; reports each unimplemented device once. |
| `uart.c`, `uart.h` | Two 16550-compatible UARTs. The first is the console; the second has no host backing. |
| `console.c`, `console.h` | Non-blocking stdin polling, raw mode and the `Ctrl-]` escape. |
| `virtio.c`, `virtio.h` | The virtio-mmio transport. |
| `virtio9p.c`, `virtio9p.h` | The 9P2000.L server for the root filesystem. |
| `bootfs.c`, `bootfs.h` | The in-memory inode tree loaded from `bootfs.idx`/`bootfs.blob`. |
| `eth.c`, `eth.h` | The OpenCores ethmac controller. |
| `net.c`, `net.h` | The synthetic LAN peer: ARP, DHCP, ICMP echo, DNS. |
| `boot.c`, `boot.h` | Loads the kernel and patches its device tree's memory size. |
| `main.c` | Argument handling and the three modes. |
| `native.h` | The shim force-included by the native build: libc headers, `#define int long`, `__time`, `__c4_cycles`, and termios for raw mode. |
| `run-c4or1k.sh` | Puts the terminal in raw mode from outside, then runs `c4m load-c4r.c`. Used by the M3 targets. |
| `tools/asm.py` | A two-pass OR1000 assembler for the test programs. `tools/asm_m0.py` is M0's original hand-assembler. |
| `tools/or1k-oracle.js` | Runs a test binary through jor1k's `safecpu.js` and dumps state in `main.c`'s format. |
| `tools/mkbootfs.js` | Flattens `basefs.json` (and optionally `fs.json`) into `bootfs.idx`/`bootfs.blob`. |
| `tests/` | The M1–M3 test programs. |

## Hosts

| Host | Status |
|---|---|
| Native (`c4or1k-native`) | Full boot, standard and extended filesystem, raw mode through termios. |
| c4mp | Runs every hosted image, including `-mcisc` and the JIT build. Raw mode through c4mp's `TRAW` opcode. |
| c4m | Runs `c4or1k.c4r` and `c4or1k-jit.c4r` in test and console mode (the M1/M2/M3 checks). c4m has no `TRAW`, so boot mode stops (see Limits), and it cannot run the `-mcisc` image. |

The hosted images are 64-bit, so they do not run on c4bb or libjs.

## Limits and known issues

- `make c4or1k-boot` runs boot mode under c4m, and boot mode calls
  `__c4_termraw` (the `TRAW` opcode) before the kernel starts. c4m names
  `TRAW` but does not implement it, and c4m halts on such an opcode when no
  trap handler is installed ("not an instruction this machine has"). So this
  target is expected to stop there; use `c4or1k-boot-mp`, `-jit` or
  `-native`. This comes from reading `c4m.c` and `main.c`; it was not re-run
  for this README.
- Real network access is not there yet: the LAN peer is synthetic. `ping`
  to a name resolves to a made-up 11.x.x.x address that the gateway answers.
- The extended filesystem is native only.
- Devices not implemented (virtio-block, DRM, ATA, keyboard, touchscreen,
  RTC) probe and fail without stopping the boot.
- The second getty, on the unbacked second UART, blocks forever, as an
  unconnected port would.

## Docs

[docs/c4or1k-design.md](../../docs/c4or1k-design.md) has a section per
milestone with transcripts, measurements and the bugs found on the way. Its
opening paragraph has drifted: it says raw mode is external (`run-c4or1k.sh`)
and not provided by the VM, which M18 changed, and it names `make
c4or1k-m0`, which no longer exists.
