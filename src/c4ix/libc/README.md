# src/c4ix/libc — C4IX's C library

A C library for programs that run on C4IX: stdio with buffered `FILE`s and a
full integer `printf`, the string and ctype functions, `malloc` with `realloc`,
`strtol`, `qsort`, `getenv`/`setenv`, `getopt`, `opendir`, and the POSIX file
calls over C4IX's syscalls. [`include/libc.h`](include/libc.h) is the whole
interface. `stdio.h`, `string.h` and the other standard names include it.

It is a shared library, `/lib/libc.c4s`. C4IX's loader links it into a
program at spawn (`src/c4ix/loader.c`), and every task gets its own copy of
its data, so `errno` and the stdio buffers are per task.

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
| `stdlib.c` | `malloc` (a size word ahead of each block), `realloc`, `exit` and `atexit`, `strtol`, `qsort`, the environment, `rand` |
| `stdio.c` | `FILE`s, `fopen` and the rest, and `printf` with flags, widths, precisions and every integer conversion |
| `unix.c` | `open` … `rename`, `stat`, `opendir`, `getopt`, `errno` and `strerror`, `time` |
| `opcodes.c` | the VM's `MALC`, `FREE`, `MCPY`, `MSET` and `MCMP`, as `__libc_*`. The one file compiled without `-fno-builtin` |
| `tests/libctest.c` | exercises all of the above; `tests/libctest.txt` is its output, compared by `make test-libjs` |
| `BUILD.B4K` | rebuilds `libc.c4s` and `libctest` inside C4IX |

## Limits

- No floating point: `%f` and friends print `?`.
- No locales, wide characters, `setjmp`/`longjmp` or signal handlers.
- No `lseek`: C4IX has no seek. `O_APPEND` works by reading the file in and
  writing it back.
- The environment starts empty, because C4IX passes none.
- `time()` is seconds since boot.
- A function pointer is an `int`, since c4lc calls any variable. So `qsort`,
  `bsearch` and `atexit` take one.
- `size_t` is `int`.
