# c4mp — the C4 MultiProcessor

c4mp is a C4 virtual machine with room for more than one processor. It
executes the same instruction set as c4m, opcode for opcode and with the
same trap frame, so a guest image cannot tell the two apart. On top of that
it adds processors, atomics, a futex-style park/wake pair, inter-processor
interrupts, two array opcodes, raw terminal mode, and all ten fused opcodes.

It differs from c4m in three ways that shape everything else:

- **No compiler.** It loads `.c4r` images and nothing else. Dropping the
  compiler drops about half of what c4m carries, including the plain-c4
  fallback machinery (the `C4_ONLY` shims, the invoke-stub jailbreak, the
  `/proc/uptime` clock).
- **Compiled by c4lc, not by c4.** c4m has to stay parsable by `c4.c`, so it
  cannot use structs, `for`, `->` or `switch`. c4mp can: registers live in a
  `struct c4_cpu`, dispatch is a `switch`, and the native build marks the
  registers `restrict`.
- **One source, two builds.** A native executable, and a `.c4r` image that
  c4m hosts. The hosted build proves the design needs nothing c4m lacks: c4m
  has no multiprocessing, so there SMP is simulated.

There is no design document for c4mp; this README is its reference. Other
documents describe parts of it: [docs/fused-opcodes.md](../../docs/fused-opcodes.md)
(the fused opcodes), and the M11, M12 and M18 sections of
[docs/c4or1k-design.md](../../docs/c4or1k-design.md) (c4mp as a faster host,
`LXI`/`SXI`, `TRAW`).

## Build

| Target | Output |
|---|---|
| `make c4mp` | `./c4mp`, native, built with the host C compiler from `vm.c smp.c loader.c main.c` and `c4m_float.c`. |
| `make c4mp.c4r` | The same modules compiled by c4lc (with its own preprocessor, `-D __c4cc__=1`) and linked by c4rlink. Runs under c4m, or under c4mp. |
| `make c4mp-smp0.c4r`, `c4mp-smp1.c4r`, `c4mp-deadlock.c4r` | The guest programs in `guest/`, compiled by c4lc. c4cc does not know the processor builtins. |

## Run

    ./c4mp [options] image.c4r [args...]

| Option | Meaning |
|---|---|
| `-d` | Trace every instruction. |
| `-v` | Report load and exit details, and each CPU's cycle count. |
| `-p KB` | Stack size per CPU in KB (default 256). |
| `-q N` | Instructions per slice (default 1000 with `-cpus`, otherwise unbroken). |
| `-cpus N` | Number of processors (default 1). |
| `--` | End of options. |

    ./c4mp src/tests/hello.c4r
    ./c4mp -cpus 4 c4mp-smp1.c4r
    ./c4m load-c4r.c -- c4mp.c4r -cpus 4 c4mp-smp0.c4r    # hosted: simulated SMP inside c4m
    ./c4mp c4mp.c4r src/tests/hello.c4r                    # c4mp running itself

The exit status is CPU 0's. A machine that deadlocked exits non-zero.

## How SMP works

**Processors.** CPU 0 starts on a bootstrap trampoline that `main.c`
assembles out of real instructions: `JSR` to each constructor, push `argc`
and `argv`, `JSR main`, push the result, `JSR` to each destructor in reverse,
`EXIT`. (c4mp is the machine, so it cannot call guest code from the host;
it builds code that does.) Every other CPU is off until the guest starts it
with `__c4_cpu_start(id, entry, stacktop)`, giving it a function and a stack
it allocated. A secondary whose entry function returns lands on a `CPUH` and
halts.

**Scheduling.** `c4_smp_run` walks the CPU table from 0 each pass and runs
every runnable CPU for one quantum (`-q`, default `C4MP_QUANTUM` = 1000
instructions). CPUs change only between `c4_run` calls, so:

- every instruction is atomic, and the interleaving between instructions is
  where races show up;
- a run is deterministic: same quantum, same order, same output. The tests
  pin whole transcripts;
- deadlock is diagnosable. When nothing can run and some CPU is parked, c4mp
  prints "deadlock: every processor is parked and none can be woken", lists
  each CPU with the address it waits on, and exits non-zero.

`EXIT` on CPU 0 ends the machine, as `main` returning ends a program; `EXIT`
or `CPUH` on a secondary halts only that CPU. With one CPU and no `-q`, the
run is never sliced, which is exactly c4m.

**Interrupts are per CPU.** Each CPU has its own trap handler, cycle
interrupt interval and protected-mode bit, so an SMP kernel masks
preemption on the CPU it is running on. An IPI arrives as `TRAP_HARD_IRQ`
with sub-code `HIRQ_IPI` (the cycle interrupt is `HIRQ_CYCLE`), and wakes a
parked target.

**Pass 2 is not implemented.** The plan in `smp.c` is to replace the round
robin with one host thread per CPU and real `__atomic_*` builtins in the
atomic opcodes, leaving the opcodes' interface unchanged. Today `-cpus N`
always runs the simulated scheduler in one host thread; the header comment in
`c4mp.h` that says "N host threads (pass 2)" describes the plan, not the
code.

## The added opcodes

Numbers are c4m's and continue its table; c4m names all of these (so a
disassembler can) but executes none of 66–78 and only three of 79–88. A
guest should test `__c4_info() & C4I_SMP` (0x100) before using the processor
opcodes: on c4m, `CPUI`..`TRAW` without a trap handler halt the machine with
"not an instruction this machine has", and with a handler installed they
arrive as `TRAP_ILLOP`, so a kernel can emulate them.

| # | Opcode | c4lc builtin | Does |
|---|---|---|---|
| 66 | `CPUI` | `__c4_cpu_id()` | This CPU's index. |
| 67 | `CPUN` | `__c4_cpu_count()` | How many CPUs the machine has. |
| 68 | `CPUS` | `__c4_cpu_start(id, entry, stacktop)` | Start a CPU on `entry` with the given stack. 1 on success, 0 if `id` is out of range or already running. |
| 69 | `CPUH` | `__c4_cpu_halt()` | Halt this CPU, not the machine. |
| 70 | `CAS` | `__c4_cas(addr, expect, new)` | Compare and swap; returns the value found. |
| 71 | `XCHG` | `__c4_xchg(addr, val)` | Swap; returns the old value. |
| 72 | `FADD` | `__c4_fadd(addr, delta)` | Fetch and add; returns the old value. |
| 73 | `CWAI` | `__c4_wait(addr, val)` | Park while `*addr == val`. Returns 1 if it did not park, 0 when woken. A wake is a hint: re-test in a loop. |
| 74 | `CWAK` | `__c4_wake(addr, n)` | Wake up to `n` CPUs parked on `addr` (`n <= 0`: all). Returns how many. |
| 75 | `IPI` | `__c4_ipi(cpu)` | Raise `HIRQ_IPI` on another CPU. |
| 76 | `LXI` | emitted by c4lc `-mcisc` | `a = *(int *)(base + a * 8)`, base popped from the stack. |
| 77 | `SXI` | emitted by c4lc `-mcisc` | `*(int *)(base + index * 8) = a`, base and index popped. |
| 78 | `TRAW` | `__c4_termraw(on)` | Put fd 0 in or out of raw mode (`tcsetattr`/`cfmakeraw`). Returns 1 if raw mode is on. A no-op returning 0 when stdin is not a tty. Restored at exit and on SIGTERM/SIGHUP. |

There is no lost-wakeup race between `CWAI`'s test and its park: it is one
instruction, and instructions are atomic here. The atomics are plain
read-modify-writes for the same reason.

`LXI`/`SXI` index 8-byte elements, so they belong to 64-bit images.
`c4mp.h` reserves a `C4I_CISC` bit (0x200) that `INFO` does not report, so a
guest cannot ask for them; only `-mcisc` builds use them.

**Fused opcodes (79–88).** `LDL LDG PSHL PSHG LEAP IMMP LIP ADDL STL POPA`,
each replacing a two- or three-instruction sequence. c4opt's `-mfuse` pass
emits them. c4mp runs all ten; c4m runs `LDL`, `STL` and `POPA` and halts on
the other seven. See [docs/fused-opcodes.md](../../docs/fused-opcodes.md).

## Test

    make test-c4mp

Not in `make test`: it runs every test image through two VMs, and some
through three. [test-c4mp.sh](test-c4mp.sh) checks, in order:

1. **Equivalence.** Every `src/tests/*.c4r` image gives the same output and
   exit status under `./c4m load-c4r.c` and `./c4mp` (addresses and
   millisecond timings masked).
2. **Hosted.** A subset under `./c4m load-c4r.c -- c4mp.c4r` matches c4m.
3. **Nesting.** c4mp → c4mp.c4r → hello, and one level deeper.
4. **Slicing.** `-q 1`, `2`, `7` and `1000` give the same output and cycle
   count as an unbroken run.
5. **Multiprocessing.** `smp0` on 4 CPUs matches the pinned transcript
   [tests/smp0-4cpu.txt](tests/smp0-4cpu.txt), is deterministic, changes
   when the quantum changes, gets the right totals on 1, 2, 4 and 8 CPUs, and
   matches the pin when hosted by c4m.
6. **Atomics, park/wake, interrupts.** `smp1` on 4 CPUs and on 1, and hosted
   by c4m against [tests/smp1-4cpu.txt](tests/smp1-4cpu.txt).
7. **Deadlock.** `deadlock` on 4 CPUs must be diagnosed (all four parked CPUs
   named, non-zero exit), not hang.

Stages 5–7 skip if their guest images are not built; the make target builds
them.

c4mp is also a prerequisite of `make test-fuse` (which runs a fused image on
it) and `make test-c4th`, and the host for c4or1k's `c4or1k-boot-mp`,
`-cisc` and `-jit` targets.

## Layout

| File | What it is |
|---|---|
| `c4mp.h` | Shared declarations: the opcode enum (c4m's numbering, append only), trap codes, `C4I_*` bits, `struct c4_cpu`, `struct c4r_image`, module interfaces. |
| `vm.c` | The interpreter, `c4_run(cpu, quantum)`: a `switch` over opcodes, the trap frame, the syscall opcodes, `TRAW`. |
| `smp.c` | The CPU table, `c4_smp_run` (round robin), start/wake/IPI, the deadlock report. |
| `loader.c` | `.c4r` loader, a port of `src/c4ix/loader.c`. Images up to 4 MB, word size must match the host. |
| `main.c` | Options, the bootstrap trampoline, and the run. |
| `test-c4mp.sh` | The suite above. |
| `guest/smp0.c` | Starts every CPU; each works on its own slot. No atomics needed. |
| `guest/smp1.c` | Every added opcode: atomics, a spinlock, a lock-free counter, park and wake, IPIs. |
| `guest/deadlock.c` | Every CPU parks on a word nobody changes. Exists to be diagnosed. |
| `tests/smp0-4cpu.txt`, `tests/smp1-4cpu.txt` | Pinned transcripts. |

## Hosts

| Host | Notes |
|---|---|
| Native `./c4mp` | Full: floats (`FLT`), signals, the host clock, termios for `TRAW`. |
| c4m, via `c4mp.c4r` | SMP simulated inside c4m. `FLT` traps as an illegal opcode (the host's float call would need a host address). `TRAW` returns 0. `INFO` reports only what the host can do. Host signals are not forwarded to the guest. |
| c4mp, via `c4mp.c4r` | Works; the test suite nests it two deep. |

Images are loaded only if their word size matches the machine, so the native
64-bit c4mp does not run the 32-bit c4bb images.

## Limits and known issues

- Pass 2 (real host threads, real atomics) does not exist; see above.
- `RALC` traps as an illegal opcode, as in c4m, where realloc has never been
  implemented.
- `STRC` prints a frame walk without names; the loader discards the image's
  symbol section.
- No compiler: build images with c4lc (or c4cc, for images without the
  processor builtins).
