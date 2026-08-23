# Vendored Forth-2012 test suite

`tester.fr` and `core.fr` are taken verbatim from Gerry Jackson's
Forth 200x / Forth-2012 test suite:

  https://github.com/gerryjackson/forth2012-test-suite
  src/tester.fr, src/core.fr — fetched 2026-08-23

`tester.fr` is John Hayes' original tester (C) 1995 Johns Hopkins
University / Applied Physics Laboratory, "may be distributed freely as
long as this copyright notice remains" — it does, at the top of the file.

**Do not edit them.** They are the oracle: the whole point of using a
standards suite is that it was not written by us and does not know what
c4th happens to implement. Anywhere c4th deliberately diverges from the
standard, the divergence is recorded in `docs/c4th-design.md`, not
patched into these files.

They are self-verifying — a test reads `T{ 1 2 + -> 3 }T` and prints only
on failure — so they need no reference Forth to compare against.
