# src/c4ix/libc — C4IX's C library

A C library for programs that run on C4IX: stdio with buffered `FILE`s and a
full `printf` (floating point too), the string and ctype functions, `malloc` with `realloc`,
`strtol`, `qsort`, `getenv`/`setenv`, `getopt`, `opendir`, the POSIX file
calls over C4IX's syscalls (`lseek`, `fstat`, `link` and `truncate` among
them), `vfork`, `execve` and `waitpid`, and `<math.h>`. [`include/libc.h`](include/libc.h) is the
interface, and `stdio.h`, `string.h` and the other standard names include
it; [`include/math.h`](include/math.h) adds the floating-point part, with
`strtod` and `atof` (docs/c4-float.md says why they are there).

It is a shared library, `/lib/libc.c4s`. C4IX's loader links it into a
program at spawn (`src/c4ix/loader.c`). It is built `-fpic`, so its code is
loaded once and shared by every task that uses it. Each task gets its own
1.2 KB copy of its data, so `errno` and the stdio buffers are per task.
`tests/pictest.c` checks that with two programs at once.

## Using it

```
c4sc -c 400000 compile -O -fno-builtin -c -I /usr/include/libc prog.c prog.c4o
c4rlink prog.c4o -l /lib/libc.c4s -o prog.c4r
```

`-fno-builtin` makes `printf`, `malloc`, `open` and the rest of c4cc's
library names ordinary functions instead of VM opcodes, so this library's
functions are the ones called. `c4rlink -l` leaves the references for the
loader to resolve instead of copying the library in. On the host it is the
same commands with `./c4sp32 ... src/c4sp/lisp/c4lc.lisp` and `-I src/c4ix/libc/include`.

## Layout

| file | what |
|---|---|
| `include/libc.h` | the interface; `include/*.h` are the standard names for it |
| `sys.h` | private: the libc4ix functions the library is built on |
| `string.c` | string.h and ctype.h |
| `stdlib.c` | `malloc` (a size word ahead of each block), `realloc`, `exit` and `atexit`, `strtol`, `qsort`, the environment (read in from the kernel's copy, which `execve` set), `rand` |
| `stdio.c` | `FILE`s, `fopen` and the rest, and `printf` with flags, widths, precisions and every conversion |
| `fp.c` | `printf`'s `%f %e %g` digits and `strtod`/`strtof`/`atof`, exact both ways (glibc's answers) |
| `math.c` | `<math.h>`, after fdlibm: within an ulp or two of glibc, the bit-exact ones exact |
| `unix.c` | `open` … `rename`, `lseek`, `stat` and `fstat`, `link`, `truncate`, `execve` and `waitpid`, `opendir`, `getopt`, `errno` and `strerror`, `time`. `vfork` is libc4ix's, since it must execute the syscall itself (`src/c4ix/sched.c`) |
| `opcodes.c` | the VM's `MALC`, `FREE`, `MCPY`, `MSET` and `MCMP`, as `__libc_*`. Compiled without `-fno-builtin` |
| `bare.c` | `sys.h` with no C4IX: the machine's own opcodes, and C4DOS's API when there is one (below). Compiled without `-fno-builtin` |
| `bareu.c` | the rest of libc4ix such a program needs: the varargs stack, `ualloc`, `uprintf` |
| `tests/libctest.c` | exercises all of the above; `tests/libctest.txt` is its output, compared by `make test-libjs` |
| `tests/fptest.c`, `tests/mathtest.c` | fp.c and math.c alone against glibc's vectors (`fpconvgen.c`, `mathgen.c`), in `make test-c4fp` |
| `tests/pictest.c` | two at once must keep their own libc data while sharing its code (`make test-libjs`) |
| `tests/unixtest.c` | the kernel's POSIX calls: `vfork` and `execve` with an environment, `waitpid`, `lseek`, `fstat`, `O_APPEND`/`O_EXCL`, `link`, `truncate`; `tests/unixtest.txt` is its output (`make test-libjs`) |
| `BUILD.B4K` | rebuilds `libc.c4s`, `libctest`, `pictest` and `unixtest` inside C4IX |

## Without C4IX

`bare.c` and `bareu.c` take libc4ix's place, so a program built against
this library can also run on the bare machine or under C4DOS. It is
linked whole, not as `libc.c4s`, with the units built without `-fpic`:

```
c4rlink prog.c4o string.c4o stdlib.c4o stdio.c4o unix.c4o fp.c4o math.c4o \
        opcodes.c4o bare.c4o bareu.c4o -o prog.c4r
```

(`make wolf3d-bare-objs` builds them, in `.wolf3d-bare/libc-*.c4o`.)
Files are the machine's: `open` and `read` on the host's directory, and
writing only to the console. Under C4DOS -- the loader fills in
`__c4dos_api` -- `bare.c` asks DOS first: `open` of a DOS file, `create`
and `write` on its RAM disk, and `exit` through `dos_exit`, back to the
prompt. Directories, `lseek`, pipes and `execve` are not there, and fail.

## Limits

- `%a` (hex float output) is not there; `strtod` reads hex floats.
- No locales, wide characters, `setjmp`/`longjmp` or signal handlers.
- `lseek` moves in RAM files only: a pipe, the console and a host file
  are `ESPIPE`.
- The environment is what `execve` passed, or for a spawned task its
  spawner's; `execv` passes the task's own. `environ` is not there yet
  (M7d).
- `vfork`'s child shares the parent's memory, libc's data included, until
  it calls `execve` or `_exit`. It must not call `exit` or return from the
  function that called `vfork`. `fflush` before a `vfork`, as Busybox does.
- Modes, uids and gids are made up: the VFS has none.
- `time()` is seconds since boot.
- A function pointer is an `int`, since c4lc calls any variable. So `qsort`,
  `bsearch` and `atexit` take one.
- `size_t` is `int`.
