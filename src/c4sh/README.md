# c4sh — the C4KE shell

c4sh is the interactive shell C4KE's init process starts. It runs `.c4r`
programs as C4KE tasks, in the foreground or in the background with `&`,
and has a handful of builtins for job control and the environment. It is
an ordinary user task: it reaches the kernel only through `include/u0.h`.

## Build

    make c4sh.c4r       # part of `make pre`
    make c4sh32.c4r     # 32-bit, for the C4DOS build floppies

c4cc has no preprocessor, so the `#include` lines in these files do
nothing and the Makefile concatenates the sources on the command line
(`C4SH_SRCS`):

    ./c4cc -o c4sh.c4r include/u0.h src/c4ke/bin/ps.c \
           src/c4sh/c4sh.c src/c4sh/c4sh_builtins.c src/c4sh/c4sh_scripting.c

Order matters: each builtins file registers its commands from a
constructor, after `c4sh.c` has defined the registry.

## Run

c4sh is not started by hand; `make run` boots C4KE, whose init starts it
(`src/c4ke/services/init.c`) with the prompt `c4sh>`. It needs C4KE: it
calls kernel services (task start, focus, process table) that exist only
there.

- Any `.c4r` in the working directory can be run by name, with or without
  the extension. `PATH` is set to `./`.
- `cmd &` starts a task in the background; `fg` brings the last one back.
- Quotes (`"` or `'`) group words into one argument; there is no
  substitution inside them and no escaping of quotes.
- `#` starts a comment.
- Up-arrow then Enter repeats the previous command; there is no other line
  editing.
- `\q` is an alias for `exit`.

Builtins (`help [command]` prints this list):

| builtin | usage | what |
|---|---|---|
| `help` | `[command]` | command list, or one command's usage |
| `fg` | `[pid]` | foreground a process |
| `kill` | `[-L] [pid]` | send a signal; `kill -l` lists signals |
| `ps` | | process listing |
| `kdbg` | | print kernel state |
| `toggle` | `psf` | show a process listing while waiting on a foreground task |
| `echo` | `[-n] args` | print arguments |
| `set` | `[VAR=VALUE]` | set or list environment variables |
| `exit` | `[exit code]` | leave the shell |

## Hosts

c4sh runs wherever C4KE does: native c4m, plain c4 through c4m.c, c4bb and
libjs (the 32-bit build). Its input opens `/dev/stdin` non-blocking and
polls with a growing sleep, so other tasks keep running while it waits;
if that open fails it falls back to a blocking `read` of fd 0.

## Layout

| file | what |
|---|---|
| `c4sh.c` | the shell: line input, tokenizer (quotes, `&`, comments), builtin registry, task start and wait, `main` |
| `c4sh_builtins.c` | `fg`, `help`, `kdbg`, `kill`, `ps`, `toggle` |
| `c4sh_scripting.c` | `echo`, `exit`, `set` |

## Limits and known issues

- The header comment in `c4sh.c` is out of date: it shows an old build
  command (`./c4r … u0.c`), says `kill` is not implemented (it is), and
  lists "functions" and "running scripts". There is no script-file mode:
  `main` always reads from stdin.
- `set` has no usage text (`help set` prints nothing further).

## Docs

There is no design doc for c4sh. For the kernel it runs on, see
[src/c4ke/README.md](../c4ke/README.md) and
[docs/internals.md](../../docs/internals.md) Part 4.
