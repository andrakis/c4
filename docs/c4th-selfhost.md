# B5d.5 — the fixed point

The tracker for the last rung of B5d. See `docs/c4th-design.md` for the
ladder this hangs off.

## What "the fixed point" means here

A compiler that compiles its own source, and whose output does not change
when you feed the output back in. Three generations:

    gen1.c4r   = c4th, running self.f's compiler, compiling self.f
    gen2.bin   = gen1.c4r,  under c4m, compiling self.f
    gen3.bin   = gen2 (=gen1), under c4m, compiling self.f

    cmp gen1.c4r gen2.bin        the compiled compiler agrees with the hosted one
    cmp gen2.bin gen3.bin        and with itself

`gen1 == gen2` is the strong claim: the *hosted* compiler (running on
c4th's threaded engine, which is the engine that passes the Forth-2012
CORE suite) and the *compiled* compiler emit the same bytes. That is the
differential B5d established, turned on the compiler itself.

## The shape, and why it is not native.f

B5d.5 was written as "native.f compiling itself". Pointing native.f at
its own source is what found the four bugs in `7896de2`, and it is why
`EXECUTE` compiles (`2635138`). But native.f's *input* is c4th's threaded
code, not text: to compile native.f, a generated image would first have
to BE c4th — outer interpreter, threaded compiler, dictionary, inner
interpreter — and only then run native.f over the result. That is the
whole C kernel ported to Forth before a single line of the rung is
reached.

The fixed point does not need that tower. It needs a compiler written in
the language it compiles. So `src/c4th/forth/self.f` is exactly that: a
single-pass Forth-to-`.c4r` compiler, in the subset of Forth it itself
implements, reading text and emitting an image. It is the third
independent `.c4r` writer the plan asked for, and it is small enough to
be read in one sitting.

**Subroutine threading, not strategy (b).** self.f emits `JSR helper`
per word against a software data stack — §8's strategy (a). native.f
keeps strategy (b) and keeps being the fast backend; self.f is the one
that has to compile *itself*, and for that, simple and obviously correct
beats fast. Every opcode it emits is <= `EXIT`, so a generated image runs
on plain `c4` as well as c4m, c4mp and oisc4.

## The ladder

- [ ] **S0** The tracker, before the code.
- [ ] **S1** The four primitives c4th lacks: `ALLOCATE`, `OPENF`,
      `READF`, `CLOSEF`. Each is one C4 syscall, so self.f can emit them
      directly. *Verify:* `make test-c4th` still green.
- [ ] **S2** The assembler and the runtime library: the data stack, the
      return stack, the loop stack, and the ~55 helpers, most from four
      templates (BINOP, CMP, UNOP, MOVCELL). *Verify:* a hand-built image
      that pushes two numbers, adds and exits with the answer.
- [ ] **S3** The front end: source buffer, `WORD`, comments, number
      conversion, the dictionary, `:` `;`, control flow, `VARIABLE`
      `CONSTANT` `CREATE ALLOT`, `S"` `."` `CHAR`. *Verify:* self.f
      compiles `src/c4th/tests/self1.f` and the image prints what c4th
      prints running the same file.
- [ ] **S4** The `.c4r` writer, to stdout. *Verify:* `c4r-roundtrip`
      decodes and re-encodes the image byte for byte, as B5d required of
      c4r.f.
- [ ] **S5** The closure check: self.f compiles self.f. Every word its
      own source uses is one its own compiler knows. *Verify:* gen1.c4r
      exists and c4rdump reads it.
- [ ] **S6** The fixed point. *Verify:* `cmp gen1.c4r gen2.bin` and
      `cmp gen2.bin gen3.bin`, wired into `make test-c4th`.
