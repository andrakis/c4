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

- [x] **S0** The tracker, before the code.
- [x] **S1** The five primitives c4th lacked: `ALLOCATE`, `OPENF`,
      `READF`, `CLOSEF` and `HALT`. Each is one C4 syscall, so self.f
      emits them directly and the compiled compiler reaches memory and
      files by the route the hosted one does. `ALLOCATE` rather than
      static arrays for the same reason `SAVE-FILE` is a primitive: the
      image's data segment is written out byte for byte, so half a
      megabyte of compiler buffers there is half a megabyte the image
      has to print. *Verify:* `make test-c4th` green.
- [x] **S2** The assembler and the runtime library: the data stack, the
      return stack, the loop stack, and the fifty-odd helpers -- forty of
      them from four templates. C4's ALU is `a = *sp++ OP a`, a Forth
      stack machine already, so the only mismatch a template has to
      absorb is that C4's comparisons yield 0/1 where Forth's yield
      0/-1: one `SUB` from zero.
- [x] **S3** The front end: source buffer, `WORD`, comments, number
      conversion, the dictionary, `:` `;`, control flow, `VARIABLE`
      `CONSTANT` `CREATE ALLOT`, `S"`. No `."` and no `CHAR`: `S" ..."
      TYPE` says the first and a named constant says the second, and a
      directive not owed is a directive that cannot be wrong.
      *Verify:* `src/c4th/tests/self1.f`, run on c4th and then compiled,
      prints the same thing on c4m, c4mp, oisc4 and plain c4.
- [x] **S4** The `.c4r` writer, to standard output rather than to a
      file -- the C4 VM has no write syscall, so a compiled compiler
      could not open one, and a compiler that writes to stdout is the
      same compiler on both sides of the bootstrap. *Verify:*
      `c4r-roundtrip` decodes and re-encodes the image byte for byte, as
      B5d required of c4r.f.
- [x] **S5** The closure check: self.f compiles self.f. Every word its
      own source uses is one its own compiler knows -- the compiler
      refuses a word it does not have, so the check is the compile.
      *Verify:* 9,619 instructions, 238,850 bytes, and `c4r-roundtrip`
      re-encodes it byte for byte.
- [x] **S6** The fixed point. gen1 == gen2 == gen3, and the same three
      bytes-for-bytes on c4m, c4mp, oisc4 and plain c4. It also survives
      `c4opt`: the optimizer rewrites the compiler and the rewritten
      compiler emits the identical image. Wired into `make test-c4th`.

## What it cost to say, and two things it found

The compiler is 822 lines and reached its own fixed point on the first
attempt that ran -- which is much less a claim about the code than about
the oracle underneath it. Every one of the fifty-odd helpers had already
been checked by `self1.f` against c4th's threaded engine before the
compiler was ever pointed at itself.

**The patch table has to be in ADDRESS order.** `c4r.lisp` walks the
instruction stream and the patch list together and says so outright:
"patch list out of sync with instruction stream". A forward branch whose
patch is recorded when it is RESOLVED lands after the patches for
instructions between the branch and its target. So a forward branch here
carries its PATCH index rather than its code address, and the patch goes
in when the branch is emitted, with a value filled in later. Nothing
else in the tree emits patches out of order, so nothing else had found
this; `load-c4r.c` applies them in any order and never noticed.

**`c4l.c`'s pre-flight scan started at word 0.** The `.c4r` code stream
is 1-based -- c4cc emits through `*++e` -- so `scan_extended` walked
every image one word out of phase, reading each operand as an opcode. It
survived years of images because their operands are small and stayed
inside the opcode table by luck. `self.f` emits an `IMM` of a sign mask,
and reading that as an opcode indexes the name table a gigabyte past its
end: a segfault, on the first image this rung produced. Same off-by-one
the B5d writer made from the other side, and the same lesson -- the
1-based convention was written down in exactly the two readers that
depended on it.
