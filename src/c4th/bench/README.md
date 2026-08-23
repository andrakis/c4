# The B5 probe

Three measurements of one loop -- sum 0..99999 -- taken before writing the
native backend, to decide whether to write it at all and which of the two
stack strategies to use. All three run under `c4m`, counting VM cycles with
`__c4_cycles()`, which is exactly reproducible.

| | file | cycles | per iteration | vs threaded |
|---|---|---|---|---|
| threaded, as c4th runs today | `threaded.f` | 34,318,112 | 343 | — |
| strategy (a): software data stack, one `JSR` per Forth word | `strategy_a.c` | 12,600,041 | 126 | 2.7x |
| strategy (b) ceiling: native C4 code, values in registers and a frame | `native.c` | 2,400,018 | 24 | **14.3x** |

**Conclusion: build strategy (b).** It is 5.3x better than (a), and (a)'s
figure is generous to it -- the C model lets native code run the loop
control that a real strategy-(a) backend would also thread, so 126 is a
lower bound on what (a) would really cost.

The (b) row is a ceiling, not a promise: it is what c4cc emits for the
equivalent C, with both live values in frame slots. A Forth backend keeps
its values on a stack rather than in named locals, so it pays more for any
operation that reorders them -- `SWAP`, `OVER`, `ROT`, `!` -- and less for
the ones C4's own instruction set already performs natively, where `+` is
one `ADD`.

Reproduce:

    ./c4cc -o /tmp/b.c4r src/c4th/bench/native.c && ./c4m load-c4r.c -- /tmp/b.c4r
    ./c4cc -o /tmp/a.c4r src/c4th/bench/strategy_a.c && ./c4m load-c4r.c -- /tmp/a.c4r
    ./c4m load-c4r.c -- c4th.c4r src/c4th/forth/core.f src/c4th/bench/threaded.f

## b5c.f — counted loops, inlined calls, stack reordering (B5c)

The B5 probe answered "which strategy"; this one answers "what did B5c
buy". Five loops, each run threaded and then compiled and run natively,
under c4m where `CYCLES` is a real counter:

    ./c4m load-c4r.c -- c4th.c4r src/c4th/forth/core.f \
        src/c4th/forth/asm.f src/c4th/forth/native.f src/c4th/bench/b5c.f

| | threaded | native | |
|---|---|---|---|
| `DO`/`LOOP` with `I` | 34,300,951 | 1,700,354 | 20x |
| `BEGIN`/`WHILE` with `>R`/`R>` | 107,201,208 | 3,100,351 | 34x |
| loop calling another word | 20,080,951 | 440,354 | 45x |
| loop reordering the stack | 21,681,110 | 1,540,357 | 14x |
| loop through a `VARIABLE` | 71,501,323 | 2,200,356 | 32x |

The call case is the fastest because inlining removes the call
altogether; the shuffle case is the slowest because a permutation the
compiler cannot do by moving code has to go through the frame, at about
ten instructions an item.
