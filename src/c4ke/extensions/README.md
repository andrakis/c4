# src/c4ke/extensions — optional C4KE kernel modules

Each file here is `#include`d at the end of `src/c4ke/c4ke.c` and compiled
into the kernel image; the Makefile lists all of them in `C4KE_SRCS`, so a
change to one rebuilds `c4ke.c4r`. They see all of the kernel's internals
through `<c4ke/extension.h>`.

## Contract

An extension registers itself from a constructor:

    static int __attribute__((constructor)) name_constructor () {
        kext_register("name", (int *)&name_init, (int *)&name_start, (int *)&name_shutdown);
    }

The three callbacks return `KXERR_NONE` on success. `init` runs early,
`start` runs after the kernel's own opcodes exist and before the init task
is created, `shutdown` runs as the kernel stops. An extension that adds
custom opcodes publishes them by name with `kext_register_symbol("OP_…",
number)`; user programs look them up at start with
`__c4_opcode("OP_…", OP_REQUEST_SYMBOL)`, so the numbers are the kernel's
to choose.

Every extension must be inert on a host that lacks what it needs, because
the same kernel image boots on native c4m, under plain c4 through c4m.c, on
c4bb and in libjs.

## Files

| file | registers as | what | active when |
|---|---|---|---|
| `c4ke_ipc.c` | `ipc` | inter-process communication by signals: a word is sent as a sequence of `SIGIO`, `SIGCONT` and `SIGRTMIN+x` signals | always compiled in |
| `c4ke_plus.c` | `plus` | placeholder for features of the c4plus interpreter; reports "requires c4plus" and does nothing | never (no c4plus host) |
| `c4ke_pm.c` | `pm` | protected mode: handles the trap a user task raises on a privileged instruction (I/O, `MALC`/`FREE`) and emulates it per task | only in kernels built with `-DCONFIG_ENABLE_PM=1`, and only on a c4m that reports `C4I_PROT` |
| `c4ke_dos.c` | `dos` | the C4DOS hand-over: if C4DOS loaded the kernel, it wrote its API table into the exported `__c4dos_api`; at start this copies every file on the DOS RAM disk into the kernel RAM filesystem, so a kernel built in C4DOS boots the init it was just handed | only when `__c4dos_api` is set and its magic and version check out |
| `c4ke_boot.c` | `boot` | `OP_KBOOT`, which `bin/kboot.c` raises: the kernel shuts down as when init exits, and then loads and runs another kernel image (C4IX, built by b4ke) through `kernel_handoff`, last thing in `main`. When that returns, so does C4KE. [docs/c4dos-ladder.md](../../../docs/c4dos-ladder.md) | always |
| `c4ke_mbox.c` | `mbox` | message frames: `OP_MBOX_INFO/SEND/RECV/AWAIT` for the c4bb host mailbox and `OP_MSG_SEND/RECV` for per-task inboxes. User side: `include/c4ke_mbox.h` | the task-to-task half always; the host half only when the host announces a mailbox |

`c4ke_pm.c`'s header still says "Shelved"; protected mode is optional
rather than shelved now, and `make test-task-mem` depends on it
(`include/config.h`, [docs/task-memory.md](../../../docs/task-memory.md)).

The mailbox is documented in [docs/c4bb-design.md](../../../docs/c4bb-design.md)
("The mailbox") and tested only by `make test-c4bb`
(`src/c4bb/tests/test-c4ke-mbox.mjs`, with `bin/mbecho.c` and
`bin/mbpair.c`).

## Why the kernel does not rely on these headers

When `innerbench` compiles `c4ke.c` from source inside c4m, there is no
preprocessor: the `#include` lines at the end of `c4ke.c` bring in
nothing and these extensions are simply absent. That is why state the
scheduler needs from an extension (for example the mailbox registers) is
declared in `c4ke.c` itself rather than here.
