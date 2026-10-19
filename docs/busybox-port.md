# Busybox on C4IX — what a source port needs

This is milestone M7 of the C4IX userland plan. Busybox is to be compiled from
source by c4lc, not run as the OpenRISC binary. The resulting `busybox.c4r`
links `/lib/libc.c4s` and runs as an ordinary C4IX task, and `/bin` gets its
applets.

This file is the tracker. It records what is missing today, measured where
that can be measured now and estimated where it needs Busybox's source. It
also sets the order of the work: §7 lists the steps, each green before the
next.

Status (2026-10-20): **write-up done; nothing ported yet.** The Busybox source
has not been fetched.

## 1. The target

- **Busybox 1.37.0**, the current stable release, pinned by its sha256 when it
  is fetched. GPLv2, like the rest of Busybox. The vendored tree keeps its
  `LICENSE` and every file's own header.
- **Configuration:** `make allnoconfig` plus a short list, generated on the
  host and committed (§6). There is no fork, so it is a **NOMMU** build
  (`CONFIG_NOMMU=y`, which sets `BB_MMU` to 0). That is the configuration
  Busybox ships for uClinux, and the one its shells, daemons and `vfork`
  paths are already written for.
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

**Today 52 pass and 61 fail.** It is a tracker and not a `make` gate yet. It
becomes one, in `make test-c4lc`, once it passes. The kernel and libc gaps
come from reading `src/c4ix/sys.c`, `vfs.c`, `loader.c` and `src/c4ix/libc`.

c4fc must match c4lc byte for byte (`make test-c4fc`). So every compiler item
below is done twice: in `src/c4sp/lisp/c4lc-*.lisp` and in `src/c4fc/*.f`. c4sc
is c4lc compiled, so it comes along by itself.

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
| `void_params` | `int f(void)` | CFAIL: bad parameter declaration | P0 | It is the declaration of nearly every function in libbb. |
| `fnptr_decl`, `fnptr_param`, `fnptr_typedef`, `fn_returning_fnptr`, `cast_fnptr` | `int (*fp)(int)`, function-pointer parameters, typedefs and casts | CFAIL | P0 | applet mains, `recursive_action` callbacks, `qsort` comparators, hush's builtin table |
| `applet_table`, `array_of_ptr_to_fn_in_struct_member_call` | a struct member that is a function pointer | CFAIL: bad struct member name | P0 | hush's `struct built_in_command`, libbb's `llist`/action structs |
| `typedef_enum`, `enum_typedef`, `enum_var`, `enum_local_decl` | `typedef enum {…} t;`, `enum e v;` inside a function | CFAIL (a global `enum e g;` passes) | P0 | Option bitmasks (`OPT_x`), hush's parse states |
| `static_local` | a `static` local | CFAIL: "not implemented" | P0 | Common in libbb: buffers, one-time state |
| `inline_only` | `inline` / `static inline` | CFAIL | P0 | `platform.h`'s `ALWAYS_INLINE`, many small helpers in headers |
| `attribute`, `attr_macro_away` | `__attribute__((…))` anywhere; `#define __attribute__(x)` | CFAIL: the lexer reserves the name, and the parser takes it in one position only | P0 | `NORETURN`, `FAST_FUNC`, `ALIGN1`, `PACKED`, `UNUSED_PARAM` on almost every prototype. Either parse it and ignore it everywhere a declaration allows it, or let the preprocessor redefine it. |
| `restrict_` | `restrict` | CFAIL: read as a name | P1 | a few libbb prototypes |
| `array2d`, `array2d_init` | `int g[3][4]`, `char t[][8] = {…}` | CFAIL | P1 | keyword tables, `ls`'s column arrays |
| `bitfield` | `unsigned a : 3;` | CFAIL | P1 | a few structs. Read-modify-write on a word. |
| `struct_copy` | `a = b` for structs | CFAIL: bad lvalue | P1 | `struct stat` copies in `ls`/`cp`. It becomes a `memcpy`. |
| `struct_byval` | struct parameters and returns by value | CFAIL: "must be pointers" | P2 | rare. Patch it, or lower it to hidden pointers. |
| `for_decl` | `for (int i = …)` | CFAIL | P1 | a handful of loops in newer code |
| `vla` | `char b[n]` | CFAIL | P2 | `FEATURE_BUFFERS_USE_MALLOC=y` avoids most uses; the rest get patched |
| `typeof_` | `typeof(x)` | CFAIL | P2 | a few macros |
| `long_long` | `long long` | parses; word-sized by design (M4) | — | With LFS off, `off_t` fits in a word. `%lld` already prints a word. |

### 3.2 Initializers

| probe | construct | today | P |
|---|---|---|---|
| `strptr_arr`, `const_strptr_arr`, `const_arr` | `char *w[] = {"a", "b"}` | CFAIL: "must be an integer constant" (a single `char *g = "x"` passes) | P0 |
| `fn_arr_int` | an array of function addresses | CFAIL | P0: the generated `applet_main[]` is exactly this |
| `struct_global_init`, `struct_arr_init_ints`, `struct_init_str`, `struct_init_fn`, `nested_struct_init` | struct and array-of-struct initializers with numbers, strings and function addresses | CFAIL: "struct globals cannot be initialized" | P0: every option, builtin and keyword table |
| `global_addr_var`, `global_ptr_init` | `int *p = &x;` in a global | CFAIL: `&` is allowed on functions only | P1 |
| `local_struct_init` | `struct pt p = {3, 4};` in a function | CFAIL | P1 |
| `designated` | `.x = 1`, `[3] = 2` | CFAIL | P1 |
| `extern_arr` | `extern const char t[];` without a size | CFAIL: "needs an initializer" | P0: `bb_msg_*`, `applet_names`, `bb_common_bufsiz1` are all declared this way |
| `compound_lit` | `(struct pt){3, 4}` | CFAIL | P2 |

These are one piece of work: a general constant initializer for data. Its
values are integers, string addresses, function addresses and global
addresses, laid out by the type, and the image's existing `-2`/`-3`/`-4`
patches relocate them. c4lc's data section already places strings and
functions. What is missing is walking an aggregate type to lay out the
values.

### 3.3 Expressions and statements

| probe | construct | today | P |
|---|---|---|---|
| `goto`, `label_only` | labels and `goto` | CFAIL | P0: hush and libbb use `goto` for error exits throughout |
| `sizeof_var`, `long_long` | `sizeof(scalar variable)` | CFAIL: "sizeof needs an array" (struct variables pass) | P0 |
| `sizeof_expr`, `sizeof_deref`, `ptr_to_globals` | `sizeof x`, `sizeof(*p)`, `sizeof(G)` | CFAIL | P0: `xzalloc(sizeof(*p))` is libbb's idiom, and `G` is `(*ptr_to_globals)` |
| `neg_array_assert`, `offsetof` | `sizeof` and `offsetof` inside constant expressions (array sizes) | CFAIL | P0: the `BUG_*_too_big` checks in every applet that has `struct globals` |
| `func_name`, `line_macros` | `__func__`, `__LINE__`, `__FILE__` | CFAIL | P1: cheap |
| `builtin_expect` | `__builtin_expect` | CFAIL | P2: `likely()`/`unlikely()` can be defined away in `platform.h` |
| `statement_expr` | `({ … })` | CFAIL | P2 |
| `case_range` | `case 'a' ... 'z':` | CFAIL | P2 |

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
| `if_defined`, `if_defined_only` | `#if defined(X)` | **DIFF: always false.** `defined X` without parentheses works. | P0: a silent miscompile, and the first fix |
| `va_macro`, `ifdef_func_macro` | `#define F(...) __VA_ARGS__`, `args...`, `, ##args` | CFAIL: bad macro parameter | P0: the generated `autoconf.h` defines every `IF_FEATURE_X(...)` this way |
| `include_quote` | `#include "x.h"` relative to the including file | CFAIL: only the working directory and `-I` are searched | P1: `-I` per directory works around it |
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

`/lib/libc.c4s` has about 140 functions (`src/c4ix/libc/README.md`). libbb
and the chosen applets need these as well. The list is from Busybox's
headers and will be checked against the link's undefined symbols at M7e:

- **stdio and strings:**
  - `vasprintf`/`asprintf`: libbb's message functions are built on them.
  - `stpcpy`, `strchrnul`, `memrchr`, `mempcpy`, `strsep`, `strverscmp`.
  - `strtoll`/`strtoull`, word-sized, like M4's `long long`.
  - `strsignal`, `basename`/`dirname`, `realpath`.
- **Process:**
  - `vfork`, `execve`/`execv`/`execvp`, `waitpid` and its `W*` macros;
  - `environ`, `getppid`, `_exit`;
  - `signal`/`sigaction`/`sigprocmask` stubs;
  - `getuid`/`geteuid`/`getgid`, all returning 0.
- **Files:**
  - `lseek`, `fstat`, `lstat`, `ftruncate`, `fchdir` (`ENOSYS`), `access`
    (already there);
  - `chmod` and the other no-ops in §4;
  - `opendir` is already there, plus `fdopendir` if needed.
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
- **`setjmp`/`longjmp`:** not needed with NOFORK applets off, since libbb's
  `die_jmp` is used only there. It is still wanted later. It needs a trap
  that restores pc, sp and bp, so it is a kernel item too.

## 6. The build

- **Vendoring:** a script, `tools/bb-vendor.sh`, unpacks the pinned tarball
  and checks its sha256. It copies into `third_party/busybox/` only the files
  the configuration compiles, plus `LICENSE`, `README` and `AUTHORS`, so a
  15 MB tree is not committed for 14 applets.
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
   1. `defined(X)`;
   2. variadic macros;
   3. `(void)` parameter lists;
   4. `sizeof` on any expression and in constant expressions;
   5. function-pointer declarators;
   6. `goto`;
   7. enum declarations;
   8. `static` locals;
   9. aggregate initializers and `extern T x[]`;
   10. `inline` and `__attribute__`;
   11. then the P1 items.

   Each turns its probes PASS, and gets gcc-checked tests in
   `src/tests/c4lc_*.c` like M4's. `run.sh` joins `make test-c4lc` when it
   reaches 113 of 113, or with the P2 probes listed as expected failures.
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
4. **M7e, fetch and vendor** Busybox 1.37.0. **This is a download, so it
   waits for the user's go-ahead.** Then:
   - generate the configuration on helios with host gcc;
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

- **Image size.** C4IX caps an image at 4 MB (`C4R_BUF_MAX`, `loader.c`).
  - The desktop is 4,500 lines and its image is 442 KB, libc4ix included:
    about 100 bytes per line.
  - The minimal configuration compiles perhaps 25–35 thousand lines of libbb
    and applets. That is near the cap, before symbols.
  - If it is over:
    - size the loader's buffer from `stat` instead of a fixed 4 MB;
    - strip the symbols a dynamic image does not need. Today `-s` is
      ignored for one, because the loader resolves the image's imports from
      its symbol table, but only the import records are needed;
    - and let c4rlink drop unreferenced functions.
  - Measure at M7f before choosing.
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
