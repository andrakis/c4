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
