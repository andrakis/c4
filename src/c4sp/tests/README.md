# c4sp tests

`expected/` holds two kinds of reference output.

- The samples in `../lisp` (`arguments.txt`, `fac.txt`, `seval.txt` and the
  rest), compared byte for byte by `make test-c4sp` (native build and
  `c4sp.c4r` under c4m). These were verified against the Node build of alisp
  (`node index.js <sample>` in the alisp checkout) with **one deliberate
  divergence**: c4sp represents the empty list and `nil` as the same value
  (the null pointer — see docs/c4sp-design.md §3), so `arguments.lisp` prints
  `Arguments provided: nil` where alisp prints `Arguments provided: ()`.
  Everything else, including `seval.lisp`'s 456 lines of debug trace with its
  `(#env N)` environment numbering, is byte-identical to alisp.
- c4lc's goldens (`c4lc-*.txt`: tokens, ASTs, diagnostics, program output)
  and the source-map goldens (`srcmap-*.txt`), compared by test-c4lc,
  test-c4fc, test-c4sc and test-srcmap. They are c4lc's own answers, not
  alisp's.

The scripts beside it are parts of test-c4lc and test-srcmap:
`common-link.sh`, `fold-link.sh`, `m4-volatile.sh`, `srcmap-link.sh` and
`srcmap-invariant.sh`. `c4lc_opassign.c` and `c4lc_mainproto.c` are test
sources for c4lc and c4fc.

To re-verify against the oracle:

```sh
cd ~/git/alisp
node index.js /path/to/c4/src/c4sp/lisp/seval.lisp | diff - /path/to/c4/src/c4sp/tests/expected/seval.txt
```
