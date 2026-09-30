# The HOMEWARD ladder — DOS→C4KE handover, and C4IX in-machine

**Status: M0–M8 complete, 2026-08-23.** Every suite green in one sweep afterwards —
`test-c4dos` `test-cpp` `test-c4l` `test-link` `test` `test-c4ke-ramfs` `test-c4lc`
`test-c4ix` `test-c4ix-c4ke` `test-raycast` `test-dosload` `test-c4bb` `test-ladder`,
all exit 0. The next rung is **networking on c4mp** — see the bottom of this file, and do
not start it without the user.

**Status tracker.** This file is the authoritative record of where the ladder work stands.
Tick a box only when its verification command has actually been run and is green, and paste
the one-line evidence beside it. Update this file in the same commit as the work, so
`git log docs/homeward-ladder.md` is a true history. Anything discovered mid-implementation
that changes the shape of a later milestone gets written *here*, not left in conversation.

---

## Why

HOMEWARD's premise is that the player builds a CPU from the transistors up and then climbs it:
enough hardware for `IMM n` → more opcodes → the sample programs → **C4DOS** at pure-C4 level →
C4DOS builds and boots **C4KE** → under C4KE, c4sp/c4lc/c4rlink build **C4IX** → C4IX gains
networking (via c4mp, which is also where CISC lives for c4or1k).

The hardware side of that ladder is Homeward's job. Two *software* things block it:

1. **C4KE can't see what C4DOS built.** `BUILD.BAT` already compiles a kernel *and* an init
   onto the DOS RAM disk, but the kernel's loader only knows the host filesystem — so the
   `init.c4r` that actually boots is the prebuilt one on disk, not the one just built.
2. **C4IX can't be built in-machine.** `c4rlink` refuses to write under c4 and cannot read the
   `.c4o` files c4lc just produced — and C4IX is linked from twelve objects.

## Decisions taken

| Question | Answer |
|---|---|
| Seeding C4KE's RAM filesystem | A new **kernel extension**, alongside ipc/plus/pm — not a change to `load-c4r.c`. |
| What gets seeded | **Everything on the DOS RAM disk** — it is the initrd. |
| The loader | A **C4DOS-specific loader**, LOADLIN-shaped. May assume c4m-level hardware. |
| Opcode gating in c4bb | **Dropped.** Homeward already surfaces this to the player, and c4bb is Homeward's *truth oracle* — a fitted-opcode gate would duplicate that work and diverge the reference from the thing testing against it. |
| Scope | Loader rung + C4IX unblock + disk reorganisation. |

## What is already true (do not rebuild these)

- **`c4r_load_mem` already exists** (`load-c4r.c:655`) and `task_loadc4r` already checks the
  kernel ramfs **before** the host for *every* program including init (`c4ke.c:2491-2513`).
  **`load-c4r.c` needs no change at all** — only *seeding*.
- **`KEXT_START` is exactly the right window.** `kext_run_all(KEXT_START)` at `c4ke.c:3653`
  runs after the `OP_VFS_*` opcodes are installed (`:3552-3556`) and before the init task is
  created (`:3685`).
- **C4DOS already injects `__c4dos_api` into every transient it loads** (`c4dos.c:225-247`,
  called at `:531`). `RUN c4ke.c4r` already hands the kernel the table; the kernel simply
  never looks at it.
- **`c4rlink` already links the memory-output plumbing.** It `#include`s `asm-c4r.c`
  (`c4rlink.c:47`) and serialises through `writechecked` (`asm-c4r.c:250-256`), which already
  has the `asmc4r_use_mem` path. Only the `is_c4()` bail stands in the way.
- **`sim/cli.js -d dir` already takes any directory** — per-system disks need no CLI change.
- **`c4l.c` already refuses an image using opcodes above `EXIT`, naming the instruction**
  (`c4l.c:93-101`, message at `:167-173`). That is the existing purity check, and it is how
  dosload's opcode rung gets verified.

### Bugs found on the way

**1. `g_api` slots 9–15 were uninitialised heap.** `c4dos.c` allocated 16 words and assigned
only 0–8. Any transient reading a high slot got garbage. Fixed in M1 (32 words, `memset`).

**2. An in-machine kernel build had NO constructors, so NO extensions — ever.** Found in M2.
`include/c4.h` carries `#ifndef __GNUC__ / #define __attribute__(x)` for Windows. The host
build preprocesses with `gcc -E`, which defines `__GNUC__`, so the strip never fired there.
The *in-machine* build runs our own `cpp.c4r`, which does not — so every
`__attribute__((constructor))` in the kernel was erased and `kext_initialize` reported
`0 extensions`. Invisible until now because nothing asserted on extension count, and the
kernel boots fine without ipc/plus. Fixed by nesting the strip in `#ifndef __c4cc__` in all
four copies of `c4.h` (`include/`, `include/c4ke/`, `src/c4ke/stdlib/`, `src/c4ke/include/`).
Without this fix the whole handover is impossible: `c4ke_dos.c` registers from a constructor.

---

## Part 1 — `src/c4ke/extensions/c4ke_dos.c` (the payoff; independent of Part 2)

**This, not the loader, is what closes the loop.**

A new extension registered like `c4ke_ipc`/`c4ke_plus`/`c4ke_pm` (`kext_register(name, init,
start, shutdown)`, `c4ke.c:1372`). It declares `int *__c4dos_api;` — **not `static`**, because
`inject_api` scans *exported* symbols and requires class `Glo` (131). At `KEXT_START` it
enumerates the DOS RAM disk and `ramfs_put`s every entry, then calls `RELEASE` so DOS hands the
memory back. `ramfs_put` copies, so passing DOS's own buffer straight through is safe and
avoids a third copy; peak cost is 2× RAM-disk bytes for the duration of the loop.

Indirect calls go through a **local** `int *` (emits `JSRS`, already in C4KE's opcode set)
rather than a global (`JSRI`, which is not). Copy the exact shape of `load-c4r.c:1007-1019`'s
`__loadc4r_execute_entry`, including its `#ifndef __c4cc__` macro.

**New DOS API v2 slots** (`c4dos.c`, `include/c4dos.h`, `include/c4dos_native.h`), because
there is currently no way to *enumerate* the RAM disk:

```
9 COUNT   10 ENTNAME   11 ENTSIZE   12 ENTDATA   13 TRIM   14 RELEASE
```

Bump `g_api[5]` to 2 and gate every slot ≥9 on it. Grow the allocation to 32 words and
`memset` it — fixing the uninitialised-slot bug. Advertise the enumeration slots only when
`g_ramdisk`, the same discipline slot 2 already follows.

**Precedence note:** the extension seeds *before* `vfsload` runs (vfsload is a user task
started by init), so a name present in both `c4ke.vfs.txt` and the DOS RAM disk gets silently
replaced by the disk copy. Keep the recovery disk's manifest free of the names `BUILD.BAT`
produces (`c4ke.c4r`, `init.c4r`, `c4ke.i`), proved by deleting the prebuilt `init.c4r` in M2.

Files: **new** `src/c4ke/extensions/c4ke_dos.c`; **modified** `src/c4ke/c4ke.c` (one `#include`
beside `:3838-3840`), `src/c4dos/c4dos.c`, `include/c4dos.h`, `include/c4dos_native.h`,
`Makefile` (`C4KE_KIT`, `C4KE_SRCS`), `src/c4dos/tests/test-c4dos.sh` ("extracted 38 files" → 39).

## Part 2 — `dosload`: the LOADLIN

**Honest framing, and it belongs in the file's header comment:** `RUN c4ke.c4r` already works
and already injects the API. dosload is *not* what makes the kernel see the RAM disk. It adds:

- **Memory.** DOS holds a 4 MB `g_scratch` and `run_program` cannot free it, because
  `img_cons`/`img_des` point *into* it (`c4dos.c:521-534`). A kernel about to build C4IX wants
  every byte.
- **A kernel command line** — `-v 50`, `-c N`, an alternate init file, reaching
  `parse_commandline` (`c4ke.c:3113`).

`src/c4dos/dosload.c` is strict c4. About 70 of the ~80 lines of `c4dos.c`'s `c4r_load`
(`:458-539`) copy verbatim — parse, v3 MEMSZ/BSS, code/data placement, patch types −1..−4, the
`'C'/'D'/'P'/'c'/'d'/'S'` marker walk. Four deliberate differences:

1. its **own** grown buffer, read through `dos_fopen`/`dos_fread`/`dos_fclose` (slots 6/7/8,
   which reach DOS's RAM-disk-first opener, so it gets case resolution and `./` stripping free);
2. **cons/des copied out** and pre-resolved to absolute addresses (the `src/c4ix/loader.c:174-192`
   move), so the buffer is not pinned by them;
3. `free(buf)` **before** any constructor runs, then `dos_trim()`;
4. `inject_api(..., __c4dos_api)` — forwarding the table it was itself handed, which is what
   makes Part 1 work under dosload as well as under plain `RUN`.

**Use the invoke stub, not `JSRI`.** dosload is the tool the player runs *at the C4DOS rung*;
JSRI/JSRS would raise that rung by two opcodes purely so the loader can call a function.
`include/c4dos.h` needs `__c4dos_call0` and `__c4dos_call2` (it has only `call1`/`call3`).

**No second exit trampoline.** `g_api[1]` is permanently 0, dosload returns from `main`
normally, and a kernel that never returns EXITs instead.

`dos_api_trim` frees `g_scratch`; a new `scratch_need()` re-allocates lazily. **Riskiest
mechanical edit in the plan** — every `g_scratch` user must call it (`c4r_load`, `type_file`,
`cmd_copy`, `run_batch`, `read_config`). Grep every hit; a missed one is a NULL write into low
memory.

## Part 3 — C4IX in-machine, and the disks

### c4rlink (`src/c4ke/bin/c4rlink.c` only — the hard blocker)

- **Writing** (`:384-394`): replace the `is_c4()` bail with the `asm-c4r.c:772-822` pattern —
  probe `OP_VFS_PUT` behind `__c4_info() & C4I_TRAPH`, render to memory via the already-linked
  `asmc4r_use_mem`, store. Fall back to `dos_put` under C4DOS. Reset `asmc4r_use_mem = 0` on
  **every** exit path.
- **Reading** (`:558`): try `OP_VFS_GET` → `c4r_load_mem`, mirroring `c4ke.c:2495-2504`, so it
  can see the `.c4o` files c4lc just wrote. The producer side already works
  (`src/c4sp/include/stdlib.h:572-588`); only the consumer was blind.

Under gcc `is_c4()` is 0 (`asm-c4r.c:219`), so this is dead code there and `test-link` is
unaffected — **confirm by running it, not by reasoning.**

### Arena — measure before committing

At 32 bits a c4sp cell costs 21 bytes (`gc.h:39-60`, `cell.h:17`), so `-c 8000000` ≈ 168 MB
against c4bb's 32 MB default (`sim/cli.js:33`). The 8M is very likely headroom. M5 bisects the
real floor per module, validating each result by `cmp` against the 8M output — "it did not
crash" is not enough for a mark-sweep collector near its floor. Take `2 × max(N_min)`. No
source change needed: too few cells already produces `arena exhausted` + `exit(1)`
(`gc.h:166-169`). `cli.js -m MB` already exists.

### Disks

`build-images.sh` gains an **append-only** section after the existing `manifest.json`
generation — every byte above it untouched, so the disk `test-c4bb.sh` boots cannot move:

- **`images/dos-recovery/`** — emergency recovery disk: C4DOS boot files (`SIZE=16777216`),
  `dostar.c4r`, `cpp.c4r`, `c4cc.c4r`, `dosload.c4r`, `c4ke-src.tar`, `build.bat`.
- **`images/c4ke-root/`** — C4KE root filesystem: userland, `c4sp.c4r`, `c4rlink.c4r`, all 27
  `.lisp`, `u0.h`, the C4IX tree under `src/c4ix/`, plus a new `src/c4bb/fs/c4ke-dev.vfs.txt`
  so the toolchain is *discoverable* from `ls` inside C4KE.
- Also fix two files missing from the shared disk entirely: `src/c4sp/c4sp.c` and
  `src/c4ix/user/vfsload.c`.

**Deliberately do not touch `src/c4bb/fs/c4ke.vfs.txt`.** Adding ~45 entries (~870 KB) to the
*shared* disk's boot manifest risks both `test-c4bb`'s 30 M-cycle budget and `RAMFS_MAX = 256`
(currently ~106 entries).

`web/app.js:74` hardcodes one manifest path — make `loadDisk(dir)` per-directory and cached.

---

## Milestones

### M0 — tracker + baseline

- [x] `docs/homeward-ladder.md` written (this file)
- [x] Baseline recorded 2026-08-23, all three green before any change:
      `test-c4dos: ramdisk OK` (exit 0) · `test-link: OK` (exit 0) ·
      `test-c4bb: OK` (exit 0, through `c4dos boots the shared disk OK`)

### M1 — DOS API v2

- [x] `g_api` grown to 32 words (`C4DOS_API_SLOTS`) and `memset` — the uninitialised-slot bug
- [x] Version gate: `g_api[5] = 2`, `dos_version()`/`dos_can_enum()` gate every v2 call,
      enumeration slots advertised only when `g_ramdisk`
- [x] Slots 9–14: COUNT / ENTNAME / ENTSIZE / ENTDATA / TRIM / RELEASE in `c4dos.c`
- [x] `include/c4dos.h` (+ `__c4dos_call0`/`call2`, which M3 needs) and
      `include/c4dos_native.h` wrappers
- [x] `scratch_need()` + all five `g_scratch` call sites converted (`c4r_load`, `type_file`,
      `cmd_copy`, `run_batch`, `read_config`) — `grep -n g_scratch` shows no unguarded use
- [x] New test leg `src/tests/dosenum.c` + test-c4dos leg 2e: enumeration after two `COPY`s
      (`head=C4R` / `head=REM` prove ENTDATA points at content), then trim+release and DOS
      still working: `test-c4dos: API v2 enumeration and memory hand-back OK`
- [x] `bash src/c4dos/tests/test-c4dos.sh` green (exit 0)
- [x] `make test-c4bb` green (exit 0), `make test-cpp` green (exit 0)

### M2 — `c4ke_dos.c` (the payoff; needs no dosload)

- [x] `src/c4ke/extensions/c4ke_dos.c` written, `#include`d from `c4ke.c`, in `C4KE_KIT`/`C4KE_SRCS`;
      `c4rdump -s c4ke.c4r` confirms `__c4dos_api` exports as class 131 (Glo), which is what
      `inject_api` requires
- [x] `$(C4KE_C4R)` now depends on `$(C4KE_SRCS)` — it depended only on `$(C4CC)`, so an
      extension edit did not rebuild the kernel
- [x] **The constructor-strip fix** (see "Bugs found on the way" #2) — without it the
      in-machine kernel registers no extensions at all
- [x] `test-c4dos.sh` "extracted 38 files" → 39; the ladder leg no longer copies `init.c4r`
      onto the floppy and asserts `seeded N/N`
- [x] Boot-from-memory proof:
      ```sh
      make c4ke-src.tar dostar.c4r cpp.c4r c4cc.c4r c4dos-build
      rm -f c4dos-build/init.c4r        # prove the BUILT one is what runs
      cd c4dos-build && printf 'BUILD\nRUN c4ke.c4r\n' \
        | timeout 300 stdbuf -o0 ../c4m ../load-c4r.c -- ../c4dos-clock.c4r
      ```
      Got `c4ke: seeded 42/42 file(s) from the C4DOS RAM disk`, `reclaimed 1294100 bytes of
      C4DOS RAM disk` + `reclaimed 4194304 bytes of C4DOS scratch`, then `Kernel ready` and
      `C4SH` from an init that exists **only in memory** (no `init.c4r` on that floppy)
- [x] `make test` (exit 0) and `make test-c4ke-ramfs` (exit 0) unaffected — with nothing
      injected `__c4dos_api` is 0 and the extension is inert
- [x] `bash src/c4dos/tests/test-c4dos.sh` green (exit 0), including
      `the ladder holds (C4DOS built C4KE, seeded it, and booted its own init)`
- [x] `make test-c4bb` green (exit 0)

### M3 — dosload

- [x] `__c4dos_call0` / `__c4dos_call2` added to `include/c4dos.h` (M1)
- [x] `src/c4dos/dosload.c` written (strict c4) + `dosload.c4r`/`dosload32.c4r`/`test-dosload`
      Makefile rules; it is also copied onto the `c4dos-build` floppy
- [x] `./c4 c4l.c dosload.c4r` prints no "needs XXXX" — stays on the C4DOS rung.
      `Cons = 0  Des = 0`, which the header comment requires: DOS walks *our* destructor table
      out of the scratch we just freed, and zero iterations never touch it
- [x] `RUN dosload.c4r c4ke.c4r -v 50` → `dosload: 4194304 bytes released, loading c4ke.c4r`,
      then the kernel boots and its DOS extension reports the (empty) disk. `-v 0` silences
      all kernel chatter, which is the proof the command line reaches `parse_commandline`
- [x] The BUILD-then-dosload path — M7's ladder script runs it end to end and it is green

### M4 — c4rlink writes and reads memory

- [x] Write path: `is_c4()` bail replaced with the ramfs/`dos_put` pattern, `asmc4r_use_mem`
      cleared before every return out of the memory branch
- [x] Read path: `OP_VFS_GET` → `c4r_load_mem`. **Resolved by name, not via `u0.h`** — the
      `c4rlink.c4r` build has no u0 (`C4R_C4CC_SRCS` expands `$(U0)` at line 54, before `U0`
      is defined at line 64), so `vfs_get` does not exist there; `C4I_TRAPH` comes from
      `load-c4r.c`. First attempt used `vfs_get` and broke the `c4rlink.c4r` build
- [x] `make test-link` green (exit 0, native path untouched); both c4rlink builds compile
- [x] New pin `src/tests/test_ramlink.c` + a line in `test-c4ke-ramfs`: compile two objects
      under C4KE, link them, run the result — all three files existing only in memory.
      Output matches the host `test-link` exactly (`b_add(3, 4) = 7`, constructor and
      destructor from the non-first module)
- [x] The c4bb variant is covered by M6's boot of `c4ke-root` and by M7 rung 6 under c4m;
      not repeated under the simulator, where one c4lc module already costs ~1.5 minutes
      natively. Left explicitly undone rather than quietly dropped:
      ```sh
      printf 'c4cc.c4r -o tla.c4o test_link_a.c\nc4cc.c4r -o tlb.c4o test_link_b.c\nc4rlink.c4r tla.c4o tlb.c4o -o t.c4r\nt.c4r\n\\q\n' \
        | node src/c4bb/sim/cli.js -m 64 -c 2000000000 -d src/c4bb/images/c4ke-root src/c4bb/images/c4ke32.c4r
      ```
      Expect `c4rlink: wrote N bytes to ramfs:t.c4r` and the linked program running

### M5 — measure the cell floor

- [x] Measured with a doubling ladder (50k…8M), each candidate `cmp`'d against a fresh 8M
      reference **at the object level**:

      | cells | modules |
      |---|---|
      | 50,000 | `user/fmt`, `user/ls` |
      | 100,000 | `boot` `console` `va` `host` `sl4b` `task` `sched` `vfs` `loader` `init`, `user/sh`, `user/top` |
      | 200,000 | `sys`, `c4ke`, `libc4ix` |

- [x] **`C4IX_CELLS=400000`** (`2 × max`) in `build-images.sh`, replacing an unmeasured
      `8000000`. Every C4IX object — 12 kernel modules, libc4ix, and all 15 userland
      programs — is byte-identical to its 8M build at this size. ~8.4 MB at 21 bytes/cell
      against c4bb's 32 MB, where 8,000,000 cells would have been ~168 MB and could never
      have been built inside the machine
- [x] The Makefile's native 64-bit path still says `4000000`, deliberately: these floors were
      measured at 32 bits, and changing the native number without measuring it would just be
      the old guess with a smaller value

**Found while validating: `c4rlink` output is not byte-reproducible.** The same objects
linked twice differ in ~4400 bytes, all inside code words, all address-shaped
(`0d66 7b55` vs `0d59 37a5`) — the loader patches data references to real heap addresses
when c4rlink loads each module, and c4rlink writes those words straight back out. Harmless
(the patch table re-applies them at load) but it means **`cmp` on a linked image proves
nothing**; validate objects, not images. Pre-existing, unrelated to this work.

### M6 — per-system disks + `c4ke-dev.vfs.txt`

- [x] `build-images.sh` append-only section deriving `images/dos-recovery/` (11 files) and
      `images/c4ke-root/` (135 files). **`c4ke-root` is derived by SUBTRACTION** — copy the
      shared disk, delete the C4DOS and C4IX-binary parts — because the manifest names ~106
      entries and a hand-curated include list would drift out of step with it
- [x] `src/c4bb/fs/c4ke-dev.vfs.txt`: concatenated onto the base manifest at build time (not
      duplicated), adding `/usr/src/c4ix` (12 modules + headers + libc4ix + vfsload),
      `/usr/src/c4sp.c`, `/usr/lib/u0.h` and `/usr/lib/lisp/*` (27 files)
- [x] The two files the shared disk had binaries for but no source: `src/c4sp/c4sp.c`,
      `src/c4ix/user/vfsload.c`
- [x] `dosload.c4r` built onto the shared disk and the recovery disk
- [x] `web/app.js`: `loadDisk(dir)` per-directory and cached, a `DISKS` map, and `c4dos32`
      added to the program list so the recovery disk is reachable from the web demo
- [x] `make test-c4bb` unchanged (exit 0) — the append-only rule held
- [x] Booted `c4ke-root` on c4bb: `vfsload: 151/151 entries loaded` (< `RAMFS_MAX` 256) and
      `ls /usr/src` shows `/usr/src/c4ix/{boot,console,va,host,sl4b,task,sched,vfs,sys,c4ke,loader,init}.c`
      plus `c4ix.h`, `libc4ix.c`, `c4ix_user.h`, `vfsload.c`, `c4sp.c`

### M7 — the whole ladder, one opt-in script

- [x] `src/c4bb/tests/test-ladder.sh` + a `make test-ladder` target (minutes long,
      deliberately *not* in `make test-c4bb`). Six rungs, each with its own assertion:
      1–4 C4DOS boots a floppy **with no `init.c4r` on it**, `BUILD` compiles kernel and init
      into the RAM disk, `dosload` releases DOS's scratch and starts the kernel, the kernel
      seeds and reaches a shell; 5 c4cc + c4rlink round trip in the RAM filesystem
      (`test_ramlink`); 6 c4lc on c4sp compiles a real C4IX module and c4rlink reads it back
      (`test_ixbuild`, new)
- [x] Green end to end: `rungs 1-4 OK (C4DOS built C4KE, seeded 42 files, booted its own
      init)` · `rung 5 OK` · `rung 6 OK` · `test-ladder: OK` (exit 0)
- [x] **Scope correction, deliberate:** the plan said "compile and link one C4IX module and
      run it". Running it is not possible at this rung — C4IX userland links against
      `libc4ix` and runs under C4IX, not under C4KE, and one kernel module alone has
      unresolved externs by design. Rung 6 links in library mode (`-r`) instead, which is
      what actually proves the path: an object that exists only in memory, read back by the
      linker. The script says so in its header rather than implying more than it shows
- [x] The DOS stage's `timeout` is the expected end, not a failure: the kernel waits at its
      shell forever and cannot be told to quit (DOS reads the whole pipe into its own line
      buffer before the kernel starts, so a trailing `\q` never reaches c4sh). 360s

### M8 — docs

- [x] `docs/c4dos-design.md`: API v2 (with the slot table and the version-gate rule), a
      `dosload` section that leads with what it does *not* add over `RUN`, and the ladder
      note rewritten now the rung is closed
- [x] `docs/c4bb-design.md`: a new "The disks" section — the shared disk, the two derived
      ones, why `c4ke-root` is derived by subtraction, and `test-ladder.sh` under Verification
- [x] `docs/MILESTONES.md`: a "The HOMEWARD ladder" section, and the arena measurement
- [x] This file closed out

---

## Verification

**Final sweep, 2026-08-23** — thirteen suites, all exit 0:

| suite | | suite | |
|---|---|---|---|
| `test-c4dos` | 0 | `test-c4lc` | 0 |
| `test-cpp` | 0 | `test-c4ix` | 0 |
| `test-c4l` | 0 | `test-c4ix-c4ke` | 0 |
| `test-link` | 0 | `test-raycast` | 0 |
| `test` (C4KE) | 0 | `test-dosload` | 0 |
| `test-c4ke-ramfs` | 0 | `test-c4bb` | 0 |
| | | `test-ladder` | 0 |

(Beware `make x | tail` in a sweep script: `$?` is `tail`'s status, so every suite reports
0. Redirect to a file, capture `$?`, then tail the file.)

Every existing suite must stay green throughout: `test-c4bb`, `test-c4dos`, `test-c4lc`,
`test-c4ix`, `test-c4ix-c4ke`, `test-link`, `test` (C4KE), `test-c4l`, `test-cpp`,
`test-raycast`. The new pins are the DOS enumeration leg, the M2 boot-from-memory check, the
c4rlink round trip, and `test-ladder.sh`.

## Risks

1. **`test-c4dos.sh` hard-codes "extracted 38 files"** → 39 in M2, same commit.
2. **`scratch_need()` touches five call sites.** A missed one is a NULL write into low memory;
   the legs of `test-c4dos.sh` cover `TYPE`/`COPY`/`RUN`/batch/config.
3. **`RAMFS_MAX = 256` is a shared budget.** vfsload's ~106 plus the DOS kit fits, but only
   just; the extension must print per-file failures and the ladder test assert `seeded N/N`.
4. **`build-images.sh` must stay append-only** — anything above the new section changes the disk
   `test-c4bb.sh` boots.
5. **`asmc4r_use_mem` is a global shared with c4cc's writer.** c4rlink links `c4cc.c` but never
   calls `asmc4r_Source`, so there is no interaction — still, reset it on every exit path.
6. **Releasing DOS's RAM disk after seeding** is a visible behaviour change if a future dosload
   ever returns to the prompt. `scratch_need()` covers the scratch; note the empty RAM disk in
   `MEM`/`DIR`.

---

## Later — networking (the end goal, NOT this plan)

The rung *above* everything above, and the game's actual win condition: **C4IX with networking,
sending a command over the network card to the time-travel device.** Nothing here is designed
yet; it is the shape of the next conversation. **Do not start until M8 is ticked.**

- **Networking targets a third interpreter: `c4mp`** — originally the multiprocessor variant of
  c4m, and the logical home for both the network device and the CISC instructions.
- **The CISC instructions** exist to make `c4or1k` faster — it boots real Linux in ~18 minutes
  under the VM against seconds native.
- **A Linux-compatibility layer in C4IX**, enough to run busybox, is the stretch beyond that.
- The ladder this plan finishes (C4DOS → C4KE → C4IX in-machine) is the prerequisite: you
  cannot ship a networked C4IX until C4IX can be *built* on the machine that runs it.

## Deliberately out of scope (for this plan)

- **Any change to c4bb's execution semantics.** It is Homeward's truth oracle; Homeward already
  surfaces missing-opcode feedback to the player. The only c4bb edits here are to
  `build-images.sh` and `web/app.js`'s disk selection — nothing in `sim/`.
- Everything in **Later — networking** above, and any change to the HOMEWARD repo.
