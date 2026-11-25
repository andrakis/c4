# c4sp/bench — M0: is compiling the Lisp worth it?

The gate in `docs/c4sc-design.md`: measure the ceiling before writing a
compiler, and stop if it is under 3x. Guessing this number wrong is the
mistake `native.f` made (`docs/c4th-design.md`).

Two kernels, each written twice — once in the c4sp Lisp, once as C over
c4sp's **own** cells and collector, which is what c4sc would generate.

- **A** (`m0.lisp` / `m0.c`) — list walking: `head`, `tail`, `empty?`,
  `=`, `+`, `cons`. This is c4lc's dominant shape: profiling
  `c4lc -O -c` on `src/c4ix/sched.c` at the Lisp level counted
  2,867,236 applications, of which **76% are builtins** — `head` 20.7%,
  `=` 16.3%, `tail` 11.2%, `empty?` 10.4%, `+` 6.8%.
- **B** (`m0b.lisp` / `m0b.c`) — `fib`, two non-tail **user** calls per
  node. The other extreme, and the pessimistic bracket.

## Running them

    # native
    gcc -O2 -fwrapv -fno-omit-frame-pointer -Iinclude -I. -o /tmp/m0 src/c4sp/bench/m0.c
    hyperfine "./c4sp -R -c 4000000 src/c4sp/bench/m0.lisp" /tmp/m0

    # hosted, which is the case that matters -- under c4m the
    # interpreted side is TWO levels of interpretation and the compiled
    # side is one
    gcc -E -Iinclude -I. -DC4CC=1 -D__c4__=1 -D__C4CC__=1 -D__c4cc__=1 -C \
        src/c4sp/bench/m0.c > /tmp/m0_pp.c
    ./c4sp -R src/c4sp/lisp/c4lc.lisp -O /tmp/m0_pp.c /tmp/m0.c4r
    time ./c4m load-c4r.c -- c4sp.c4r -R -c 4000000 src/c4sp/bench/m0.lisp
    time ./c4m load-c4r.c -- /tmp/m0.c4r

Both sides must print the same answer (5000 for A, 832040 for B). The
hosted interpreted run of A takes about eight minutes; it is not a test.

## Results, 2026-08-26

| | interpreted | compiled | |
|---|---:|---:|---:|
| **A native** | 2.435 s | 152.8 ms | **15.9x** |
| **A hosted (c4m)** | 8m14s | 11.2 s | **44.1x** |
| **B native** | 543.5 ms | 118.6 ms | **4.6x** |
| **B hosted (c4m)** | 1m26s | 5.3 s | **16.2x** |

**Hosted beats native, and that is the point.** Natively you remove one
level of interpretation out of one. Hosted you remove one out of two,
and the level that survives is itself a VM. The machine that needs the
win most is the one that gets the most of it.

## What is and is not fair here

- The compiled twins are hand-written. Generated code would be a direct
  transliteration of the same shape, so these are representative but a
  little optimistic.
- The compiled side allocates **less**: the interpreter conses an
  argument list per application, and compiled code does not. That is not
  a rigged comparison, it is a large part of what compiling buys.
- Heap sizing was checked and favours the interpreter: at `-c 16000000`
  kernel A takes 2.66 s and at `-c 64000000` 3.57 s, against 2.44 s at
  4,000,000. It is not GC-starved.
- Both native builds are `gcc -O2`; both hosted runs are the same `c4m`.
