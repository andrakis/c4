# c4sp tests

`expected/` holds the reference output for each sample in `../lisp`, compared
byte-for-byte by `make test-c4sp` (native build and `c4sp.c4r` under c4m).

Every file was verified against the Node build of alisp
(`node index.js <sample>` in the alisp checkout) with **one deliberate
divergence**: c4sp represents the empty list and `nil` as the same value (the
null pointer — see docs/c4sp-design.md §3), so `arguments.lisp` prints
`Arguments provided: nil` where alisp prints `Arguments provided: ()`.
Everything else, including `seval.lisp`'s 456 lines of debug trace with its
`(#env N)` environment numbering, is byte-identical to alisp.

To re-verify against the oracle:

```sh
cd ~/git/alisp
node index.js /path/to/c4/src/c4sp/lisp/seval.lisp | diff - /path/to/c4/src/c4sp/tests/expected/seval.txt
```
