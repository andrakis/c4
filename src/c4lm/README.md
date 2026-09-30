# c4lm — CALM, the C4 Lightweight Microkernel (on hold, does not work)

CALM was an attempt at a multitasking kernel that runs under **plain c4**,
without c4m's traps, custom opcodes or register access. It is on hold: it
cannot switch tasks more than twice. There are no Makefile targets for it.

The reason is worked out in [internals.md](../../docs/internals.md) Part 6.
`perform_task_switch1` switches by overwriting the saved frame pointer and
return address in its own frame; that leaves `sp` pointing into the outgoing
task's stack, and the switch clobbers the pair it saved. The fix given there
is a double `LEV` through an empty trampoline function plus three saved words
per task, demonstrated by
[`src/tests/test_coop_switch.c`](../tests/test_coop_switch.c).

Two things from here live on elsewhere:

- The invoke-stub trick in [`include/c4.h`](include/c4.h) (a stub that finds
  its own address and rewrites itself into a jump, so plain c4 can call a code
  pointer) is how [`c4l.c`](../../c4l.c) runs `.c4r` images under plain c4.
- `vsnprintf` in [`include/stdio.h`](include/stdio.h) is the base of the
  formatter in c4bb's firmware ([`../c4bb/fw/fw.c`](../c4bb/fw/fw.c)).

## Layout

| path | what |
|---|---|
| [`c4lm.c`](c4lm.c) | the kernel; its header comment has the intended build and boot steps |
| [`load-c4r.c`](load-c4r.c) | a copy of the `.c4r` loader with a `PURE_C4` mode, used as the boot loader |
| [`include/`](include/) | CALM's own libc headers, `c4lm.h`, `c4r.h`, `c4lm/syscall.h` |

## Known issues

- The build steps in `c4lm.c`'s header (`make run`, `make boot.c`) do not
  exist for CALM; `make run` in this repo boots C4KE.
- The top-level [`c4lm/`](../../c4lm/) directory is a duplicate of this one:
  `c4lm/include`, `c4lm/src/include` and `src/c4lm/include` are
  byte-identical, `c4lm/src/*.c` match the files here, and it carries a stray
  object file, `c4lm/load-c4r.o`.
