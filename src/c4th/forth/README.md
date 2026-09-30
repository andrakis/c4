# c4th/forth — the Forth half of c4th

The C kernel ([`../c4th.c`](../c4th.c) and [`../include/`](../include/)) has
the primitives and the text interpreter. Everything here is Forth source,
loaded by naming the files on c4th's command line, in dependency order. Every
file needs `core.f` first.

| file | what | load after |
|---|---|---|
| [`core.f`](core.f) | the part of Forth-2012 CORE written in Forth: control structures and the rest | — |
| [`ext.f`](ext.f) | CORE EXT and Structures words the kernel lacks: `CASE`/`OF`, `DEFER`/`IS`, `VALUE`/`TO`, `BEGIN-STRUCTURE` | core |
| [`locals.f`](locals.f) | Forth-2012 named locals, `{: a b | c -- d :}` | core |
| [`dos.f`](dos.f) | write a file: through the C4DOS API table under C4DOS, the host's calls natively (`SAVE-BLOCK`, `DOS?`) | core, ext, locals |
| [`asm.f`](asm.f) | assembler for C4 opcodes | core |
| [`peep.f`](peep.f) | P1, a peephole optimizer over threaded code | core |
| [`native.f`](native.f) | native backend: compiles a colon definition to C4 instructions, top of stack in the accumulator | core, asm |
| [`c4r.f`](c4r.f) | metacompiler: copies native code into a target image and writes a `.c4r` (`TMAIN`, `TCOMPILE`) | core, asm, native |
| [`trt.f`](trt.f) | runtime for generated images (output words ending in `T`) | c4r |
| [`self.f`](self.f) | a Forth compiler in Forth: reads Forth, writes a `.c4r` to stdout; compiles itself to a fixed point (`MAIN`) | core |

Load orders the Makefile uses:

```
core.f                                          # the CORE suite (tests/core.fr)
core.f asm.f native.f c4r.f trt.f prog.f        # B5d, a standalone image
core.f self.f -e MAIN                           # B5d.5, the self-hosting compiler
core.f ext.f locals.f dos.f  + src/c4fc/*.f     # c4fc
```

## Contract

- `core.f` has to stay loadable by the C kernel alone, and the vendored
  Forth-2012 suite in [`../tests/`](../tests/) is the check for CORE
  behaviour: `make test-c4th` runs it natively, under c4m and inside C4KE.
- The threaded engine is the oracle for the native backend and for images
  written by `c4r.f` and `self.f`: they must produce the same output.
- Generated images must stay within opcodes plain c4 has (nothing above
  `EXIT`), which `make test-c4th` checks by running them with `./c4 c4l.c`.
- `self.f` must reproduce itself byte for byte (generation 1 = generation 2).
