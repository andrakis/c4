# Busybox on C4IX — what a source port needs

This is milestone M7 of the C4IX userland plan. Busybox is to be compiled from
source by c4lc, not run as the OpenRISC binary. The resulting `busybox.c4r`
links `/lib/libc.c4s` and runs as an ordinary C4IX task, and `/bin` gets its
applets.

This file is the tracker. It records what is missing today and sets the
order of the work: §7 lists the steps, each green before the next. Each gap
was measured against Busybox 1.37.0 built on helios with host gcc, in the
configuration below (§2.1).

Status (2026-10-20):
- the write-up is done and checked against the source;
- M7b, the compiler, is under way (§7). Done so far:
  - the preprocessor;
  - `(void)` and unnamed parameters;
  - `sizeof` of any expression and in constants;
  - function-pointer declarators and calls through any expression;
  - `goto`;
  - `enum` as a type anywhere;
  - static locals;
  - aggregate initializers, with `[i] =` and `.m =` designators, and
    `extern T x[]`;
  - another unit's functions in data (patch type -5, which c4rlink
    resolves);
  - `inline`, `restrict` and `__attribute__`, dropped before parsing;
  - struct assignment.
- The source is not vendored into the repository yet; that is M7e.

## 1. The target

- **Busybox 1.37.0**, the current stable release, licensed GPLv2. The
  vendored tree keeps its `LICENSE` and every file's own header.
  - Tarball: `https://busybox.net/downloads/busybox-1.37.0.tar.bz2`,
    2,565,764 bytes.
  - sha256 `3311dff32e746499f4df0d5df04d7eb396382d7e108bb9250e7b519b837043a4`,
    which matches the `.sha256` that busybox.net publishes beside it. That
    file comes from the same server, so the check proves the copy is intact,
    not that it is authentic. The GPG signature has not been checked.
- **Configuration:** `make allnoconfig`, then these 27 options set to `y`
  and `yes "" | make oldconfig`. The vendoring step commits the generated
  `.config` (§6).
  - `NOMMU`, `BUSYBOX`, `FEATURE_INSTALLER`
  - `SH_IS_HUSH`, `HUSH`, `SHELL_HUSH`
  - `HUSH_TICK`, `HUSH_IF`, `HUSH_LOOPS`, `HUSH_CASE`, `HUSH_ECHO`,
    `HUSH_EXPORT`, `HUSH_SET`, `HUSH_UNSET`
  - the applets `CAT`, `CP`, `ECHO`, `FALSE`, `HEAD`, `LS`, `MKDIR`, `MV`,
    `RM`, `TAIL`, `TRUE`, `WC` and `GREP`.

  There is no fork, so it is a **NOMMU** build: `CONFIG_NOMMU=y` sets
  `BB_MMU` to 0. That is the configuration Busybox ships for uClinux, and its
  shells, daemons and `vfork` paths are already written for it.
  `FEATURE_BUFFERS_USE_MALLOC` comes with `allnoconfig`.
- **Applets for the first install:** `sh` (hush; ash needs fork and is out),
  `echo`, `cat`, `ls`, `wc`, `true`, `false`, `mkdir`, `rm`, `cp`, `mv`,
  `head`, `tail` and `grep`.
- **Off:** everything that only adds surface:
  - line editing, job control and interactive hush;
  - long options;
  - users and groups (`FEATURE_LS_USERNAME`);
  - timestamps in `ls`;
  - SUID handling;
  - compressed usage text;
  - `sendfile`;
  - "NOFORK" applets;
  - LFS (large files).

## 2. How the gaps were measured

The compiler gaps come from `src/tests/bbport/`, a battery of 113 small
programs, one construct each, taken from what Busybox's source and its
`platform.h` use. gcc is the oracle: each probe is built with both, and c4lc
must print what gcc's build prints. To run it:

```
bash src/tests/bbport/run.sh            # all of them
bash src/tests/bbport/run.sh goto vla   # some of them
```

c4lc runs as `-O -conforming -P -I src/c4ix/libc/include`:
- `-P` is the preprocessor, which a Busybox build cannot do without.
- `-conforming` turns on the `\x` and octal escapes. Without it, `'\x41'` is
  49, not 65.

**52 passed when this was written, 90 after M7b's first twelve steps,
and 97 after its P1 items (2026-10-20).** Every probe that still fails is
P2, apart from two that wait on libc's headers (`int_types_header`,
`setjmp`). `fn_arr_int` and `struct_init_fn` first kept a function in an
`int`, c4's old idiom, which gcc 14 and later reject; they now use typed
function pointers, as Busybox's tables do, so gcc can check them. It is a tracker and not a `make` gate yet. It
becomes one, in `make test-c4lc`, once it passes. The kernel and libc gaps
come from reading `src/c4ix/sys.c`, `vfs.c`, `loader.c` and `src/c4ix/libc`.

c4fc must match c4lc byte for byte (`make test-c4fc`). So every compiler item
below is done twice: in `src/c4sp/lisp/c4lc-*.lisp` and in `src/c4fc/*.f`. c4sc
is c4lc compiled, so it comes along by itself.

### 2.1 Checked against 1.37.0

The configuration in §1 was built on helios with gcc 15.2, out of tree
(`make O=…`). Then this was run:

```
python3 src/tests/bbport/bb-measure.py BUILD_DIR SRC_DIR
```

It reads the link map. What it found:

- **What links:** 67 units, 25,028 source lines before `#if`, and 64 KB of
  `-Os` x86-64 text. The stripped binary is 76 KB.
  - `shell/hush.c` is half of it: 12,365 lines and 25 KB.
  - Next come `libbb/xfuncs_printf.c`, `shell/shell_common.c`,
    `coreutils/ls.c` and `findutils/grep.c`, at 2–3 KB each.
- **What it imports:** 156 symbols, which become libc's list in §5.
  - c4rlink links whole units; it does not drop unused functions the way
    `--gc-sections` does. So every import of a linked unit must resolve, even
    those of functions that never run. `xfuncs_printf.c`, for one, brings in
    `socket`, `bind`, `listen`, `sendto` and `mmap`.
- **How often each construct appears** in the 67 units. This sets the
  priorities in §3:

| construct | uses | most in |
|---|---|---|
| `goto` | 195 | hush 112, appletlib 11, mv 10 |
| `(void)` parameter lists | 53, plus 57 in headers | everywhere |
| `__VA_ARGS__` | 22, plus 1,195 in headers | the generated `autoconf.h` alone defines one `IF_X(...)` per option |
| `defined(X)` | 22, plus 85 in headers | |
| `sizeof(*p)`, `sizeof x` | 24 | hush 16 |
| `static` locals | 19 | |
| 2-D arrays | **0 declarations.** The 19 the pattern found are `argv[i][j]`, indexing through a pointer to pointer, which c4 already does. | |
| `[i] =` designators | 18 | all in hush |
| function-pointer declarators | 13, plus 36 in headers | signals, recursive_action, `applet_main[]` |
| struct assignment | 3 | the pattern matched 9, of which 3 copy structs: libbb/time.c `*ptm = *lt;`, xfuncs.c `*newterm = *oldterm;`, hush `*ctx = *old;` |
| `inline` | 7, plus 46 in headers | |
| `alloca` | 5 | hush 2, getopt32 2, copyfd 1 |
| `setjmp`/`longjmp` | 2 | `vfork_daemon_rexec.c`, which links because of `prctl`, so it must resolve |
| `typedef enum` | 2 | hush |
| bitfields, `.x =` designators, statement expressions, case ranges, `typeof`, compound literals, `for (int …)` | **0** | |

  `asm` appears only in headers, as `barrier()`.
## 3. The compiler

Priorities:
- **P0:** libbb or hush cannot compile without it, or the use is everywhere,
  too common to patch around.
- **P1:** some applets need it, and a local patch is possible but should not
  be needed.
- **P2:** a GNU extension that a config choice or a one-line patch avoids.
  Those patches stay in `third_party/busybox/patches/`, and each one is
  listed in §6.

### 3.1 Declarations and types

| probe | construct | today | P | why Busybox needs it |
|---|---|---|---|---|
| `void_params` | `int f(void)`; unnamed parameters, `int f(int, char *)` | **DONE** (M7b) | P0 | It is the declaration of nearly every function in libbb. |
| `fnptr_decl`, `fnptr_param`, `fnptr_typedef`, `cast_fnptr` | `int (*fp)(int)`, function-pointer parameters, typedefs and casts | **DONE** (M7b), with calls through any expression; `fnptr_typedef` now waits only on initializers | P0 | applet mains, `recursive_action` callbacks, `qsort` comparators, hush's builtin table |
| `fn_returning_fnptr` | `int (*pick(void))(void)` | CFAIL | P2 | none in this configuration |
| `applet_table`, `array_of_ptr_to_fn_in_struct_member_call` | a struct member that is a function pointer | **DONE** (M7b); both probes now wait only on aggregate initializers | P0 | hush's `struct built_in_command`, libbb's `llist`/action structs |
| `typedef_enum`, `enum_typedef`, `enum_var`, `enum_local_decl` | `typedef enum {…} t;`, `enum e v;` inside a function | **DONE** (M7b): `enum` is a type specifier anywhere | P0 | Option bitmasks (`OPT_x`), hush's parse states |
| `static_local` | a `static` local | **DONE** (M7b): a global named `FUNCTION.NAME` | P0 | Common in libbb: buffers, one-time state |
| `inline_only` | `inline` / `static inline` | **DONE** (M7b): dropped before parsing | P0 | `platform.h`'s `ALWAYS_INLINE`, many small helpers in headers |
| `attribute` | `__attribute__((…))` anywhere | **DONE** (M7b): every one but constructor and destructor is dropped before parsing (c4lc's `p:untag`, c4fc's `UNTAG`) | P0 | `NORETURN`, `FAST_FUNC`, `ALIGN1`, `PACKED`, `UNUSED_PARAM` on almost every prototype |
| `attr_macro_away` | `#define __attribute__(x)` | CFAIL: the lexer reserves the name | P2: nothing needs it now that the filter drops them |
| `restrict_` | `restrict`, `__restrict`, `__extension__` | **DONE** (M7b), with the same filter | P1 | a few libbb prototypes |
| `array2d`, `array2d_init` | `int g[3][4]`, `char t[][8] = {…}` | CFAIL | P2 | no declaration in this configuration's units (§2.1) |
| `struct_copy` | `a = b` for structs | **DONE** (M7b): an `MCPY` of the size, which needs no libc | P0 | libbb's `*ptm = *lt;`, hush's `*ctx = *old;` |
| (no probe yet) | `alloca(n)` | not a builtin | P1 | 5 uses: hush, getopt32, copyfd. It can start as `malloc` in the `platform.h` block, a small leak per call, and become a real stack allocation later. |
| `bitfield` | `unsigned a : 3;` | CFAIL | P2 | 0 uses in this configuration |
| `struct_byval` | struct parameters and returns by value | CFAIL: "must be pointers" | P2 | rare. Patch it, or lower it to hidden pointers. |
| `for_decl` | `for (int i = …)` | CFAIL | P2 | 0 uses |
| `vla` | `char b[n]` | CFAIL | P2 | `FEATURE_BUFFERS_USE_MALLOC=y` avoids most uses; the rest get patched |
| `typeof_` | `typeof(x)` | CFAIL | P2 | 0 uses |
| `long_long` | `long long` | parses; word-sized by design (M4) | — | With LFS off, `off_t` fits in a word. `%lld` already prints a word. |

### 3.2 Initializers

| probe | construct | today | P |
|---|---|---|---|
| `strptr_arr`, `const_strptr_arr`, `const_arr` | `char *w[] = {"a", "b"}` | **DONE** (M7b) | P0 |
| `fn_arr_int` | an array of function addresses | **DONE** (M7b), for functions the unit defines | P0: the generated `applet_main[]` is exactly this |
| `struct_global_init`, `struct_arr_init_ints`, `struct_init_str`, `struct_init_fn`, `nested_struct_init` | struct and array-of-struct initializers with numbers, strings and function addresses | **DONE** (M7b), with brace elision | P0: every option, builtin and keyword table |
| `designated` | `[3] = 2`; `.x = 1` | **DONE** (M7b), both | P0 for `[i] =` (18 uses, all in hush); P2 for `.x =` (0 uses) |
| `extern_arr` | `extern const char t[];` without a size | **DONE** (M7b) | P0: `bb_msg_*`, `applet_names`, `bb_common_bufsiz1` are all declared this way |
| `c4lc_m7ext_*.c` | the address of a function in **another unit** inside data | **DONE** (M7b): patch type -5 (`C4R_PTYPE_DSYM`, a data byte offset and a symbol id) in the object, which c4rlink resolves to -3 or -4. A shared library's symbol in data is an error until the C4IX loader patches data | P0 for the link: `applet_main[]` sits in appletlib.c and names mains defined in each applet's file |
| `global_addr_var`, `global_ptr_init` | `int *p = &x;` and `char *p = buf;` in a global, a table or a struct, also of another unit's data | **DONE** (M7b): a data-to-data patch, or type -5 for extern data | P1 |
| `local_struct_init` | `struct pt p = {3, 4};` in a function, with any expressions, strings, nesting and designators | **DONE** (M7b): `MSET` zeroes the local, then each value is stored as the declaration runs | P1 |
| `compound_lit` | `(struct pt){3, 4}` | CFAIL | P2: 0 uses |

These were one piece of work, done in M7b: a general constant initializer
for data (c4lc-gen's `g:globalaggr`, c4fc's `AGG-*`).
- The initializer is a tree of numbers, strings, function names, braces
  and designators.
- It is walked against the type twice. The first walk places every string
  a pointer takes, before the global, as `char *s = "..."` always has.
- The second walk writes each value at its offset in the zeroed storage.
  A string pointer becomes a `-4` data-to-data patch where it lies, and a
  function address a `-3` data-to-code patch.
- A brace list of plain numbers keeps the old path, and its bytes are the
  same either way.

Getting c4fc to the same bytes found three old c4fc bugs, now fixed:
- **Unions were laid out as structs:** `x.c` of `union { int i; char c; }`
  was at 8, and `sizeof` was 16.
- **`extern T x[];` made an object of its own.** A later initialised
  definition made a second one, so code between the two read the wrong
  one.
- **String bytes were word-aligned:** a global initializer's string bytes
  were aligned to a word, where c4lc packs them after the previous
  global.

### 3.3 Expressions and statements

| probe | construct | today | P |
|---|---|---|---|
| `goto`, `label_only` | labels and `goto` | **DONE** (M7b) | P0: hush and libbb use `goto` for error exits throughout |
| `sizeof_var`, `long_long` | `sizeof(scalar variable)` | **DONE** (M7b) | P0 |
| `sizeof_expr`, `sizeof_deref`, `ptr_to_globals` | `sizeof x`, `sizeof(*p)`, `sizeof(G)` | **DONE** (M7b) | P0: `xzalloc(sizeof(*p))` is libbb's idiom, and `G` is `(*ptr_to_globals)` |
| (in `c4lc_m7.c`) | `sizeof(type)`, `sizeof("…")`, comparisons, `&&`, `\|\|` and `?:` in constant expressions | **DONE** (M7b) | P0: libbb.h's `char x[sizeof(off_t) == sizeof(uoff_t) ? 1 : -1]` |
| `offsetof` | `offsetof(T, m)` | **DONE** (M7b) as an expression: libc.h defines it | P0 |
| `neg_array_assert` | the size of a *struct* in a constant | CFAIL in c4lc, whose parser cannot lay out a struct; c4fc can | P2: no unit of this configuration does it |
| `func_name`, `line_macros` | `__func__`, `__LINE__`, `__FILE__` | **DONE** (M7b). Lines are logical: a backslash-newline does not advance them, so after one `__LINE__` is short of gcc's | P1 |
| `builtin_expect` | `__builtin_expect` | CFAIL | P2: `likely()`/`unlikely()` can be defined away in `platform.h` |
| `statement_expr` | `({ … })` | CFAIL | P2: 0 uses outside headers |
| `case_range` | `case 'a' ... 'z':` | CFAIL | P2: 0 uses |

Already right (52 probes):
- unions, flexible array members, multi-declarators, `char`/`short`/unsigned
  arithmetic;
- `const`/`volatile` locals, `do`/`continue`, switch fall-through, the comma
  and `?:` operators;
- string concatenation, `void *` arithmetic, pointer-to-pointer, arrays of
  structs and their pointer arithmetic;
- character escapes under `-conforming`.

A function value is already a word that any `int` can hold and call
(`int f; f = twice; f(21)` works). So function-pointer declarators are
parsing work; code generation needs nothing new.

### 3.4 The preprocessor

| probe | construct | today | P |
|---|---|---|---|
| `if_defined`, `if_defined_only` | `#if defined(X)` | **DONE** (M7b). It was always false. | P0 |
| (in `c4lc_pp.c`) | `#if 1` / `#elif 1` / `#else` | **DONE** (M7b). The `#else` arm ran as well. | P0: a silent miscompile |
| `va_macro`, `ifdef_func_macro` | `#define F(...) __VA_ARGS__`, `args...`, `, ##args` | **DONE** (M7b) | P0: the generated `autoconf.h` defines every `IF_FEATURE_X(...)` this way |
| (in `c4lc_pp.c`) | a macro that expands to a call of its own name | **DONE** (M7b), blue paint. It looped before. | P0: hush's `msg_and_die_if_script` |
| `include_quote` | `#include "x.h"` relative to the including file | **DONE** (M7b) | P1 |
| — | `#__VA_ARGS__` with commas | spaced as `a , b`, where gcc gives `a, b` | P2: both preprocessors join with single spaces |
| `int_types_header`, `setjmp` | `<stdint.h>`, `<inttypes.h>`, `<setjmp.h>` from libc's headers | missing from `src/c4ix/libc/include` (`include/stdint.h` exists, from M4) | P0 for `stdint.h`, a libc item |

Already right: `#pragma` (ignored), `#error`/`#warning` in skipped code,
`#elif`, `!`, `&&`, arithmetic in `#if`, an undefined name as 0, `#undef`,
nested function-like macros, `#` and `##`, and line continuations.

## 4. The kernel

C4IX has 29 syscalls (`src/c4ix/include/c4ix_user.h`). Busybox's NOMMU build
needs these as well:

| need | today | plan |
|---|---|---|
| **`vfork` + `execve`** | `SYS_SPAWN(path, argc, argv)` only; no way to run code in the child between creating it and starting the program | §4.1. P0: hush runs every pipeline and redirection through this pattern, and re-execs itself for subshells |
| **an environment for children** | none; libc's starts empty | `execve`'s `envp`, copied into the child the way `SYS_SPAWN` copies argv; libc's `environ` points at it. P0: `export` |
| **`lseek`** | the open file has a `pos`, but there is no syscall; libc returns `ESPIPE` | `SYS_LSEEK` for RAM files and lazy disk files. P0: `tail`, `head -c`, `cp`'s holes |
| **`fstat` and a fuller `stat`** | `stat(path)` gives type, size, program flag and mtime | `SYS_FSTAT`, plus `st_ino` (the vnode's index) and `st_dev`, so `cp a a` and `mv` across a directory can tell whether it is the same file. Modes are made up: 0755 for directories and programs, 0644 otherwise; uid/gid are 0; nlink is 1. P0 |
| **`O_APPEND`, `O_EXCL`** | libc emulates `O_APPEND` by rewriting the file | Real flags in `sys_open`. P1 |
| **`waitpid` with `WNOHANG` and a status** | `SYS_WAIT` blocks and returns the exit code | Add a no-wait flag; libc encodes `(code & 255) << 8` and a signal number for `kill`ed tasks. P0 for hush |
| **`getppid`** | the parent is in `SYS_TASKINFO`'s record | libc only. P1 |
| **links** | none | `SYS_LINK`, a second name for the same vnode, so `busybox --install` can make `/bin/ls`. §5 |
| **signals** | `SYS_KILL` cancels a task; there are no handlers | `signal`/`sigaction`/`sigprocmask` as libc stubs that record handlers and never deliver. With job control and interactive hush off, nothing waits on a signal. P1 |
| **termios, `ioctl(TIOCGWINSZ)`** | none | stubs: `ENOTTY` and an 80×25 terminal. P2 with line editing off |
| `chmod`, `fchmod`, `chown`, `umask`, `utime` | none | succeed and do nothing (the VFS has no owners or modes), except `utime`, which can set the vnode's mtime. P1 (`cp -p`) |
| `readlink`, `symlink`, `lstat` | none | `lstat` is `stat`; `readlink` fails with `EINVAL`; `symlink` fails with `EPERM`. P1 |
| `ftruncate`, `rmdir` | `O_TRUNC`; `rmdir` is `unlink` | `SYS_TRUNCATE`; make `unlink` refuse a non-empty directory. P1 |
| 16 fds per task (`FD_MAX`) | | Enough for the first install; hush keeps a few saved fds per redirection. Watch. |

### 4.1 vfork and exec

On a machine without an MMU, `vfork` means the child borrows the parent's
memory and stack until it calls `execve` or `_exit`, and the parent waits
until it does. C4IX already runs every task in one address space, so this is
the natural model:

- `SYS_VFORK` makes a task that shares the parent's image, heap and library
  data, and gets a copy of the fd table. Its first context is the parent's
  saved registers at the trap, with 0 in the accumulator. The parent is
  parked in a new state until the child execs or exits. Then the parent
  returns the child's id.
- The child runs on the parent's stack, below the `vfork` frame. That is
  allowed, because a vfork child must not return from the function that
  called `vfork`. This is the usual rule, and Busybox's NOMMU code keeps it.
- `SYS_EXECVE(path, argv, envp)` loads a new image for the calling task: a
  fresh stack, the same fds minus close-on-exec, and the same id. If the
  task was a vfork child, it then releases the parent.
- The task marks its image as borrowed, so a child's `_exit` frees nothing
  of the parent's.

Libraries under D2 need nothing new. A vfork child shares the parent's libc
data, which is what vfork means, and `loader_switch` already repoints it
for whichever task runs.

`spawn` stays: it is what C4IX's own programs use, and libc's `posix_spawn`
can be built on it.

## 5. libc

`/lib/libc.c4s` has about 140 functions (`src/c4ix/libc/README.md`). The
configured build imports 156 symbols (§2.1). 99 are not in `libc.h`:

- **24 are artifacts of glibc's headers,** and C4IX's headers never produce
  them:
  - the `_FORTIFY_SOURCE` `__*_chk` wrappers;
  - C23's `__isoc23_strtol` and the rest;
  - `__errno_location` and `__stack_chk_fail`.

  Their plain names, however, are real needs:
  - `vasprintf`, `sscanf`, `mempcpy`, `strtoll`, `strtoull`, which are
    missing;
  - `dprintf`, `memmove`, `vsnprintf`, which are already in libc.
- **Five `*_unlocked` stdio names and `major`/`minor`** are macros over
  libc's existing functions in the headers.
- **The rest, about 75, are real.** These are implemented:
  - **strings:** `stpcpy`, `strchrnul`, `strsep`, `strcasestr`, `putenv`,
    `realpath`, `mkstemp`;
  - **time:** `gettimeofday`, `localtime`, `mktime`, `strftime`;
  - **process:**
    - `vfork`, `execv`, `execve`, `execvp`, `wait`, `waitpid` and its `W*`
      macros;
    - `environ`, `getppid`, `setsid`, `raise`;
  - **files:** `fstat`, `lstat`, `fcntl` (`F_GETFD`/`F_SETFD` over
    `SYS_CLOEXEC`, and `F_DUPFD`), `link`, `utimes`, `poll` (pipes and the
    console, over `SYS_AVAIL`);
  - **patterns:** `fnmatch`, `glob`, `globfree`, `regcomp`, `regexec`,
    `regerror` (below).
- **Stubs**, which succeed or fail with an honest errno:
  - **signals:** `signal`, `sigaction`, `sigprocmask`, `sigsuspend` and the
    six `sig*set` functions. They record the handlers and never deliver.
  - **identity:** `geteuid`, `getegid` (0); `setuid`, `setgid`, `seteuid`,
    `setegid`, `chroot`.
  - **metadata:** `chmod`, `chown`, `lchown`, `umask`, `mknod`,
    `readlink`, `symlink`, `fchdir`.
  - **terminal:** `ioctl`, `tcgetattr`, `tcsetattr`, `ttyname_r`.
  - **resources:** `getrlimit`/`setrlimit`, `settimeofday`, `mallopt`,
    `prctl`, `mmap`.
  - **network:** `socket`, `bind`, `listen`, `sendto`.

  None of these runs in the configured applets. They are there because
  c4rlink links whole units.

Not needed after all: `memrchr`, `strverscmp`, `strsignal`, and
`basename`/`dirname` (libbb has its own `bb_basename`).

- **Patterns:**
  - `fnmatch` and `glob`, for hush's globbing.
  - **`regcomp`/`regexec`**, for `grep`. This is the largest piece. The
    candidates are musl's TRE-based regex (MIT, about 3,500 lines) or a
    small Thompson-NFA BRE/ERE. Either one is also a good workout for
    c4lc's §3 features. Until it exists, `grep -F` is all `grep` can do.
- **Headers:**
  - `stdint.h`, `inttypes.h` (`PRIu64` and the rest map to word formats),
    `setjmp.h`, `signal.h`, `sys/wait.h`, `sys/ioctl.h`, `termios.h`,
    `poll.h`, `paths.h`, `libgen.h`, `fnmatch.h`, `glob.h`, `regex.h`.
  - Each one declares only what exists. A use of anything else must fail at
    link time, not misbehave at run time.
- **`setjmp`/`longjmp`:** they must link, because `vfork_daemon_rexec.c` is
  linked, and it is linked because of `prctl`. They need not work with
  NOFORK applets off, since `die_jmp` is used only there.
  - A stub that aborts with a message is enough for M7f.
  - A real one is a trap that restores pc, sp and bp, so it is a kernel item
    too.

## 6. The build

- **Vendoring:** a script, `tools/bb-vendor.sh`, unpacks the pinned tarball
  and checks its sha256. It copies into `third_party/busybox/` only the files
  the configuration compiles, plus `LICENSE`, `README` and `AUTHORS`. That is
  the 67 `.c` files of §2.1 and the headers they include, so the 20 MB
  unpacked tree is not committed for 14 applets.
  - The generated headers are committed beside them: `autoconf.h`,
    `applets.h`, `applet_tables.h`, `bbconfigopts.h`, `common_bufsiz.h`,
    `NUM_APPLETS.h` and `usage.h`. Building never runs Kconfig.
  - The `.config` is committed too, so they can be regenerated on a host
    with gcc.
- **Patches:** `third_party/busybox/patches/NNNN-*.patch`, applied by the
  vendor script, each with a line in this file's table below. The goal is
  only P2 items. A P0 or P1 item gets fixed in the compiler, the kernel or
  libc instead.
- **`platform.h`:** an `#ifdef __c4__` block (the preprocessor already
  predefines `__c4_inttypes__`; c4lc gains `__c4__`).
  - It defines `FAST_FUNC`, `ALIGN1`, `barrier()` (an `asm` statement in
    Busybox), `likely`/`unlikely` and the byte-order macros.
  - It undefines the gcc-only paths.
- **Host build:** a Makefile target, `busybox.c4r`, mirrors Busybox's
  Kbuild list.
  - Each unit is compiled with `c4lc -O -conforming -fno-builtin -c -P -I
    third_party/busybox/include -I src/c4ix/libc/include`.
  - The link is `c4rlink *.c4o -l libc.c4s -o busybox.c4r`.
  - `build-images.sh` ships the result, and the manifest lists it.
- **In-machine build:** a `BUILD.B4K`, so b4ke can rebuild it inside C4IX
  with c4sc. That is a stretch goal: today's in-machine rebuild of about 30
  images takes 250–400 s, and Busybox is larger than all of them together.

### Patches

| patch | why |
|---|---|
| (none yet) | |

## 7. Order of work

Each step lands with tests, and the next starts only when they are green.

1. **M7b, compiler (c4lc and c4fc, byte-identical).** The order follows the
   priority tables above:
   1. ~~`defined(X)`~~: done, with `#elif` and quoted includes;
   2. ~~variadic macros~~: done, with blue paint;
   3. ~~`(void)` parameter lists~~: done, with unnamed parameters;
   4. ~~`sizeof` on any expression and in constant expressions~~: done,
      with comparisons, `&&`, `||` and `?:` in constants;
   5. ~~function-pointer declarators~~: done, with calls through any
      expression;
   6. ~~`goto`~~: done;
   7. ~~enum declarations~~: done, `enum` anywhere a type goes;
   8. ~~`static` locals~~: done;
   9. ~~aggregate initializers, with `[i] =` designators, and `extern T
      x[]`~~: done, for functions the unit defines. Addresses of another
      unit's functions in data need a c4rlink change, next;
   10. ~~another unit's function in data~~: done, patch type -5;
   11. ~~`inline` and `__attribute__`~~: done, dropped before parsing;
   12. ~~struct assignment~~: done. 2-D arrays proved to be P2;
   13. ~~the P1 items~~: done -- `__func__`, `__LINE__` and `__FILE__`; a
       global's address in an initializer, also another unit's data; and
       local aggregate initializers. What the compiler still lacks is P2.
       `alloca` is left to `platform.h` (§3.1).

   Each turns its probes PASS, and gets gcc-checked tests in
   `src/tests/c4lc_*.c` like M4's. `run.sh` joins `make test-c4lc` when it
   reaches 113 of 113, or with the P2 probes listed as expected failures.

   **c4fc** gets each item wherever its grammar reaches, and builds the
   same image as c4lc, plain and `-O`, which is all `test-c4fc` holds it
   to. Its `-minline` is a different inliner by design, and its images
   differ for every file.

   c4fc has no `typedef` at all, despite its README's old claim, so what
   needs one is c4lc's alone for now. Its parser stops at `typedef` with
   "a declaration was expected". Giving c4fc typedefs is a piece of work
   of its own.
2. **M7c, kernel:**
   1. `vfork`/`execve` with an environment;
   2. `lseek`;
   3. `fstat` and the fuller stat;
   4. `waitpid`;
   5. `O_APPEND`/`O_EXCL`, `link`, `truncate`.

   Each gets a C4IX test program in the `test-libjs` legs, the way
   `pictest` pins D2.
3. **M7d, libc:** §5's list, each with a line in `libctest`. `regcomp` gets
   its own test against host `grep -E` on fixed inputs.
4. **M7e, vendor** Busybox 1.37.0.
   - Fetched on helios on 2026-10-20 (`~/bbfetch`, outside the repository),
     configured, built with gcc and measured (§2.1).
   - What is left:
     - `tools/bb-vendor.sh`;
     - commit `.config` and the generated headers;
     - start the patch table.
5. **M7f, compile, link and run.** Do this with c4lc on helios first, under
   `node libjs/cli.js`, then in C4IX's shell. Fix what each applet hits, and
   diff every applet's output against host Busybox's on fixed inputs.
6. **M7g, install:**
   - ship `busybox.c4r` and `busybox --install /bin`, using `link`;
   - the shell's `/bin` lookup then finds the applets;
   - Busybox already dispatches on `basename(argv[0])`;
   - add the `BUILD.B4K`.

## 8. Risks

- **Image size is not a risk after all.** C4IX caps an image at 4 MB
  (`C4R_BUF_MAX`, `loader.c`).
  - The measured build is 64 KB of x86-64 text in whole units (§2.1).
  - `c4m.c` gives the ratio: it is 35.8 KB of x86-32 text from gcc, and a
    113 KB `.c4r` from 32-bit c4lc `-O`, symbols included. That is about
    3.2×.
  - So `busybox.c4r` should be roughly 0.2–0.4 MB. The earlier estimate of
    100 bytes per source line counted the desktop with libc4ix linked in.
  - To be measured at M7f.
  - The remedies stay noted in case: size the loader's buffer from `stat`,
    strip the symbols a dynamic image does not need, and let c4rlink drop
    unreferenced functions. The last would also remove most of §5's stubs.
- **Compile time on the host** is a few minutes: c4lc compiles the 275 KB
  `c4sp.c` in about 9 s natively. Inside C4IX it is an hour-scale job.
- **vfork is the one deep kernel change.** It touches the scheduler's
  trap-context path, the same place `sched_trap` and `coop_switch` already
  meet, and the stack guard's assumptions. Its tests must include a vfork
  child that writes to a redirected fd and then execs.
- **c4fc parity doubles the compiler work.** The byte-identity rule stays,
  since it is what has kept three compilers honest. If it ever blocks
  progress, the fallback is to land a feature in c4lc first and in c4fc
  right after, never more than one feature apart.
- **`test-c4ke-mbox`** fails about one run in two on this branch and on a
  disk without it. It is C4KE's own flake (`docs/c4ix-desktop.md`), and
  unrelated to this work.
