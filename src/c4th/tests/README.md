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


---

TODO: Difficulty selection determine endgame goals:
* Easy: boot C4DOS, connect a serial cable to time travel machine, use a terminal application to communicate over serial with the time travel device.
* Medium: boot C4KE, connect a network cable to a low bandwidth network connection (better than serial, not as good as ethernet), communicate to the time travel device. No DHCP, static addresses. I'm thinking an address could be 0 - 127, a single byte.
* Hard: boot C4IX, connect a network cable to a proper full networking stack (device that does routing / DHCP already provided by the workshop.) Requires a graphical display device and mouse input support. We could target a monochrome display to make this easier, not sure. Alternatively, we could do a terminal text UI version instead. The main idea is that we need to be complex enough to justify this being the hard option. A display adapter is a very complex target that would be amazing to have.

The idea is that each difficulty builds upon the one before it. A player reaching the end goal of an easy difficulty could be presented with the option to finish now or continue to the medium difficulty. Same with finishing medium.
Depending on how much work it would be, the graphical adapter oculd be an "Extra Hard" option.