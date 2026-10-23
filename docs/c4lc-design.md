# c4lc — a C compiler written in c4sp Lisp

c4lc is a ground-up replacement for c4cc's frontend and code generator,
written in the c4sp Lisp dialect. It targets the same C4 subset and the
same .c4r output format, but is built around an AST instead of c4cc's
single-pass token-to-opcode emission.

Status: the original roadmap, L0–L6, is done. c4lc compiles the full
c4cc subset with identical behavior, optimizes at tree level and via
the peephole passes in one process (-O), compiles the interpreter it
runs on AND the C4KE kernel AND the c4m VM — all three drop-in
replacements, all smaller and never slower than their c4cc twins
(§11) — and compiles inside C4KE with the kernel executing the
result from memory. See §10 for the milestones.

Beyond that roadmap, c4lc is now the compiler for **C4IX**
(docs/c4ix-design.md), which drives the post-c4cc language work: L7
(DONE) adds structs, unions, typedef, `.`/`->` member access with
struct-size pointer arithmetic, `do/while`, all ten compound
assignment operators, and block-scoped declarations with full
expression initializers — differentially tested against gcc, since
c4cc cannot compile any of it (src/tests/c4lc_l7.c). L8 (DONE) adds
object mode: `c4lc -c` emits .c4o units whose undefined prototypes
are extern symbols with SYMBOL-typed patches, resolved by c4rlink —
interoperable with c4cc objects in the same link. X0 (the first
C4IX boot) is DONE on this toolchain; the next compiler-side goal
is L9, a native preprocessor (§10).

Status 2026-09-30: L9 (preprocessor), L10 (`-conforming` escapes) and
L11 (constant expressions in enums) are DONE too (§10). Since then c4lc
has gained three opt-in code-generation flags, `-mcisc`, `-mfuse` and
`-minline`, and four fixes from 2026-09-07 to 09-10 (§10, "Beyond
L11"). c4lc now builds C4IX, C4DOS, `c4sp.c4r`, c4mp, c4or1k, c4tui and
the 32-bit c4th. How an image is built from units is in §13.

Since 2026-10: the integer types and qualifiers (M4, §14), what the C
Busybox needs (M7b, §10), `long long` in two words on a 32-bit target
(M8, §14.7), `float` and `double` (docs/c4-float.md, which tracks
that work and its limits), and what Wolfenstein 3D needed: arrays of
arrays, C's struct layout, a struct by value and the rest (M9, §14.8).

## 1. Why

c4cc inherits c4's architecture: one pass, no AST, code emitted while
parsing. Every recent feature fought that shape — global initializers
needed a declarator re-lex, local initializers a side buffer of LINIT
records, `switch` a body-first parse with placeholder stacks, `break` a
shared patch stack. A tree-based compiler dissolves all of these: parse
everything into an AST, then walk it.

What Lisp buys specifically:

- **The AST is just data.** S-expressions print, diff, and round-trip
  through the c4sp reader for free. Golden-file tests of the parser cost
  nothing.
- **The optimizer imports.** c4opt.lisp already speaks the labelled
  instruction-list form that c4r.lisp decodes/encodes. c4lc emits that
  form directly, so `-O` is `(c4opt:optimize ...)` in-process — no .c4r
  intermediate, no separate tool.
- **Tree-level optimization becomes possible.** Constant folding across
  expressions, dead-function elimination, inlining — things a peephole
  pass over flat opcodes cannot see.
- **Extension is cheap.** New statement forms are new cases in a
  dispatch, not surgery inside a 2000-line expr() with dual emission
  streams.

## 2. Measured feasibility (2026-08-03)

The real L0 lexer over all of src/c4cc/c4cc.c (75,651 bytes, 14,981
tokens, full token-list construction):

| host                          | time    |
|-------------------------------|---------|
| native c4sp (gcc -O0)         | 0.67 s  |
| c4sp.c4r under c4m            | 40.9 s  |

So native compiles of large files will take seconds; fully self-hosted
compiles (c4lc on c4sp.c4r on c4m) will take minutes for c4cc-sized
inputs and seconds for typical test programs. Acceptable; mitigations in
§12.

Primitive inventory is sufficient: `string:byte`/`string:byte!`/
`string:word!`/`string:alloc`/`string:substr` for scanning and building
byte segments, `file:read`/`file:write` (RAM-FS aware under C4KE),
`bit:*` ops, first-class environments for symbol tables, and the
c4r.lisp encoder for output. The cell arena defaults to 65,536 cells,
which c4cc.c's token list alone exhausts — c4cc-sized inputs need
`c4sp -c 2000000`.

## 3. Language target

Exactly the subset c4cc accepts today, including the 2026-08 additions:
`char`/`int`/pointers, enums, functions, globals with initializers,
global/local arrays (`ATTR_ARRAY` semantics), local initializers,
`switch`/`case`/`default`/`break`, `&array`, `sizeof(array)`, variadic
printf-style calls. Divergences from c4cc are only where c4cc has
outright bugs, and each is listed in §5.1.

Not goals (for now): structs, typedef, preprocessor beyond `#`-line
skipping, floats beyond what c4cc does. (That was the starting target.
Structs, unions, typedef and `do/while` arrived in L7, the
preprocessor in L9, `signed`/`unsigned`/`short`/`long`/`const`/
`volatile` in M4, §14, and `float` and `double`, in software, in F2 of
the floating-point plan: docs/c4-float.md.)

## 4. Pipeline

    source ── lex ──> tokens ── parse ──> AST ── sema ──> typed AST
        ── gen ──> (module ...) instruction lists ── [c4opt:optimize]
        ── c4r:encode ──> bytes ── file:write ──> .c4r  (or RAM-FS)

Modules, each a loadable file in src/c4sp/lisp/:

| file             | provides                  | milestone |
|------------------|---------------------------|-----------|
| c4lc-lex.lisp    | `lex:file`, `lex:string`  | L0        |
| c4lc-parse.lisp  | `parse:program`           | L1        |
| c4lc-gen.lisp    | `gen:module`              | L2–L3     |
| c4lc.lisp        | driver (flags, -O, out)   | L2        |
| c4lc-tokens.lisp | token-dump driver (tests) | L0        |
| c4lc-ast.lisp    | AST-dump driver (tests)   | L1        |
| c4lc-tree.lisp   | tree passes (fold, DFE, inlining) | L6, T3/T4 |
| c4lc-pp.lisp     | preprocessor (`-P -I -D`) | L9        |
| c4lc-ppdump.lisp | preprocessor dump driver  | L9        |
| c4lc-eq.lisp     | compile-and-compare driver (tests) | L5 |

All offsets in bytes unless noted, matching c4r.lisp.

## 5. Token stream (L0)

`(lex:string SRC)` → list of `(KIND VALUE LINE)`, terminated by
`(Eof 0 LINE)`. KIND is an atom; VALUE is 0 except:

- `(Num N LINE)` — integer literals, character literals (value =
  decoded char), hex `0x`, octal `0...`, decimal.
- `(Id "name" LINE)` — identifier, name as a string.
- `(Str "bytes" LINE)` — decoded string-literal bytes. Data-pool
  placement (alignment, the empty-string nul byte, adjacent-literal
  concatenation) is codegen's job, not the lexer's.

Keyword kinds: `Char Else Enum If Int Return Sizeof While Switch Case
Default Break For Continue Static Extern Attribute Constructor
Destructor`; `void` lexes as `Char` (void IS char in c4, as in c4's own
symbol seeding). Operator kinds, in c4cc precedence order: `Assign Cond
Lor Lan Or Xor And Eq Ne Lt Gt Le Ge Shl Shr Add Sub Mul Div Mod Inc
Dec Brak`. Punctuation kinds: `Not Tilde Semi Colon Comma Lparen Rparen
Lbrace Rbrace Rbrak Dot` (c4cc returns these as raw chars; c4lc names
them).

Escape decoding mirrors c4cc exactly, quirks included: `\n`→10,
`\t`→8 (sic — c4cc maps tab to 8, not 9), `\r`→10 (sic), `\0`→0, any
other escaped char is itself — **unless `-conforming` is given**, which
swaps in the real C table (L10, §5.1). `#` skips to end of line. `//`
and `/* */` comments.

### 5.1 Deliberate divergences from c4cc (all c4cc bugs)

- Newlines inside `/* */` comments increment the line counter (c4cc
  loses count).
- An empty char literal `''` is Num 0 (c4cc leaves stale ival).
- A multi-char literal `'ab'` is the last char, as c4cc.

Those three are unconditional. One divergence is **opt-in**, because
turning it on by default would break the differential battery's
premise (the same source, both compilers, byte-identical output):

- `-conforming` decodes escapes the way C defines them rather than the
  way c4cc does: `\t`→9, `\r`→13, `\a\b\f\v`→7/8/12/11, and `\xHH`
  and `\NNN` decode at all (octal capped at three digits, hex
  unbounded, both as C specifies). `\0` is unchanged — it is octal in
  both tables. Without the flag the lexer behaves exactly as it always
  did, which is what `expected/c4lc-tokens.txt` and
  `expected/c4lc-esc-quirks.txt` pin.

  The flag exists because emitting an ANSI escape is otherwise
  impossible: `"\033[2J"` decodes to a NUL byte followed by `33[2J`,
  so every program that wants one has to poke 27 in as an integer
  (see src/tests/mandel.c, and the `enum { TAB = 9, CR = 13 };`
  workarounds in src/c4ke/bin/vfsload.c and src/c4dos/cpp.c).

## 6. AST (L1) — implemented

S-expression nodes, tagged by head atom (authoritative comment in
c4lc-parse.lisp):

    (program TOP...)
    (enum (NAME VAL)...)
    (global TYPE "name" SIZE ATTRS INIT)        SIZE nil=scalar, N=array
    (proto TYPE "name" (PTYPE...) VARIADIC ATTRS)
    (func TYPE "name" ((PTYPE "p")...) VARIADIC ATTRS
          (locals (local TYPE "name" SIZE INIT)...) (block STMT...))
    INIT:  nil | (num N) | (str S) | (fnaddr F) | (braces (N...))
    stmts: (block S...) (if E S S|nil) (while E S) (for I C P S)
           (switch E ITEM...) with (case N)/(default) label items
           (break) (continue) (return E|nil) (expr E) (empty)
    exprs: (num N) (str S) (var X) (call F A...) (deref E) (addr E)
           (lognot E) (bitnot E) (neg E) (preinc/predec/postinc/postdec E)
           (cast TYPE E) (sizeof TYPE) (sizeofa X) (comma E...)
           (index E E) (assign L R) (cond C T F) (add..lor L R)

Types are integers exactly as c4cc: CHAR=0, INT=1, PTR levels +2 each;
ATTRS is c4cc's bitmask. (M4 added a band of types at 512, a typed
literal `(num V TYPE)` and optional qualifier-mask fields; §14.) Enum constants substitute as (num N) at use
sites, shadowed by locals/params, as c4cc's symbol table does. The
grammar mirrors c4cc's parse()/stmt()/expr() with two deliberate
differences: `for` is parsed CORRECTLY (c4cc's For branch never
consumes its token, so every `for` dies on "open paren expected" —
dead code no compilable source can reach), and checks that need symbol
classes (lvalues, duplicate cases, sizeof of a non-array identifier)
are deferred to sema/codegen. A function definition's closing brace
terminates its declarator line, and a prototype's ';' is left for the
declarator loop — both exactly as c4cc's parse loop consumes them.

## 7. Codegen (L2–L3)

- **Symbol tables**: assoc lists (globals, then a locals overlay per
  function), entries `(name class type value attrs)` mirroring c4cc's
  Class/Type/Val/Attr so the .c4r symbol section can be emitted
  compatibly.
- **Code**: emitted as the c4r.lisp labelled instruction list —
  `((label N) (ENT 2) (IMM (data 0)) (JSR (code N)) ...)` — with
  `(data OFF)`, `(code LABEL)`, `(extern SYMID)` operand references
  generating the patch table. Labels are allocated from a counter;
  unlike c4cc's asm-c4r there is no second stream to keep in sync.
- **Data segment**: built in a growable byte string
  (`string:alloc` + `string:byte!`); string literals, global scalars,
  arrays, and switch jumptables appended with word alignment where
  c4cc aligns.
- **Data-resident patches**: global initializers holding addresses emit
  `(dcode BYTEOFF LABEL)` / `(ddata BYTEOFF DATAOFF)` — the same
  dpatches c4r.lisp already encodes (types -3/-4).
- **switch**: jumptable in the data segment as dcode patches, same
  shape c4cc emits, but trivially generated from the AST (no
  body-first parse needed).
- **Entry point**: `main` looked up after codegen; `(code N)` entry.

Differential testing is the oracle at every step: compile each
src/tests/*.c with c4cc and with c4lc, run both under c4m, diff stdout.
Byte-identical .c4r output is NOT a goal (c4lc may order data
differently); identical behavior is.

## 8. Optimizer integration (L4)

`c4lc.lisp -O` runs `(c4opt:optimize M)` on the module before
`c4r:encode` — same passes, no round trip through a file. Acceptance:
for each test, `c4lc -O out.c4r` behaves identically to
`c4lc out.c4r` + `c4opt-run.lisp`.

## 9. Bootstrap and C4KE (L5)

- **Self-host circle**: c4lc compiles src/c4sp/c4sp.c → c4sp.c4r; that
  image (under c4m) runs c4lc's own test battery. c4lc is written in
  the language c4sp implements, and compiles the program that runs it —
  the Lisp analogue of c4cc compiling itself.
- **C4KE**: `file:read`/`file:write` already use the kernel RAM-FS
  when present, so compile-edit-run entirely inside C4KE works the day
  codegen does. `c4lc` under C4KE writes its .c4r to RAM-FS and the
  kernel loads it from there (same path test_ramopt.c proved for
  c4opt).

## 10. Roadmap

One commit per milestone; every milestone keeps the full existing
battery green and adds its own target.

- **L0 — lexer.** c4lc-lex.lisp + c4lc-tokens.lisp; golden token dump
  of a sample exercising every kind and quirk; runs native and under
  c4m (`test-c4lc`).
- **L1 — parser.** DONE. Full-subset recursive-descent to AST;
  c4lc-ast.lisp dump driver; golden AST for the sample (native + c4m);
  parses every c4cc-compilable src/tests/*.c (the two files c4cc
  rejects fail at the same constructs), the exact raw-concatenation
  self-compile unit of c4cc (u0.h + load-c4r.c + c4cc.c + asm-c4r.c,
  371 decls, 6.7s native), preprocessed c4sp.c (210 decls), c4.c,
  c4m.c and load-c4r.c.
- **L2 — minimal codegen.** DONE (c4lc-gen.lisp + c4lc.lisp driver).
  Ints/chars/pointers, globals and locals with scalar initializers
  (including "str" and &fn via dpatches), all control flow (`for`
  works), direct + function-pointer calls (JSR/JSRI/JSRS), syscalls
  as opcode+ADJ. A pre-pass gives every function a label, so forward
  calls need none of c4cc's placeholder machinery. Verified four
  ways: src/tests/c4lc_l2.c behaves identically to the c4cc build
  under c4m; src/tests/c4lc_for.c matches committed gcc output; the
  image round-trips through c4r.lisp byte-identically; and c4opt
  optimizes it (514→505 instructions) with identical behavior.
  Deferred to L3: arrays, switch, variadics, sizeof(array).
- **L3 — full subset.** DONE. Arrays (ATTR_ARRAY addressing, global +
  local initializers with zero-fill, char[] packing), sizeof(array),
  &array, &fn in expressions, switch (data-segment jumptable of dcode
  label patches, c4cc's exact dispatch + oob shims), variadics
  (__c4cc_make_va, fake-slot argc), the full __c4_* builtin table.
  Battery: every deterministic raw src/tests program both compilers
  build behaves byte-identically (17 exact + 4 with runtime pointers
  masked + 2 variadic via cpp); switch images roundtrip and survive
  c4opt (the jumptable follows moved code); test_tailcall's million
  mutual calls run flat after the tail pass; and the bootstrap smoke:
  c4lc compiles preprocessed c4sp.c (275KB, 9s native), the resulting
  interpreter runs fac.lisp and c4lc's own parser. Notable c4cc bug
  found: a bare function name as a value emits a bogus LI (loading
  from the code address) — &fn only works there because & rewinds it.
  Excluded from the battery: vararg.c prints return-pc values (layout-
  dependent by nature), oldtest_vararg crashes identically under both
  compilers, genfloat/test_illins are not c4cc inputs.
- **L4 — optimizer in-process.** DONE. `c4lc -O` feeds the fresh
  module straight into `c4opt:optimize` before encoding. The result
  differs from the two-step pipeline only in patch-covered operand
  words (dead values the loader overwrites: in-process resolves them
  fresh, the pipeline preserves stale pre-optimization RAWs), so the
  acceptance bar is identical behavior — checked against the c4cc
  build, the gcc-oracle switch output, roundtrip, and the tail-pass
  million-call test.
- **L5 — bootstrap + C4KE.** DONE, three parts. (1) Battery: c4lc -O
  compiles preprocessed c4sp.c; the resulting interpreter passes the
  sample tests (fac, truthy, call/cc on the CEK machine, switch),
  byte-roundtrips hello.c4r, and runs c4lc's parser. (2) Host
  independence: compiling c4lc_l2.c on three hosts — native c4sp,
  c4sp.c4r under c4m, and the c4lc-built interpreter — produces
  byte-identical images (c4lc-eq.lisp compiles and compares in
  memory; the bare VM cannot write). (3) The compiler inside the OS:
  test_ramcc starts c4sp+c4lc as a C4KE task, the image lands in the
  kernel RAM filesystem, and the kernel executes it from memory — no
  write ever touches the host filesystem.
- **L6 — tree optimizations.** DONE (c4lc-tree.lisp, applied by -O
  before generation). T1: constant folding over expressions and
  control flow — sizeof made literal, ?: / if / while / for with
  constant conditions reduced, && and || collapsed with c4's exact
  result-value semantics; folding whole subtrees before emission beats
  the peephole's fold (c4lc_l2.c: 484 instructions vs 505 the two-pass
  way). T2: dead function elimination — reachability closure from
  main, constructors, destructors, taken addresses and global &fn
  initializers, `__c4cc_make_va` always kept. L6 also forced two
  fidelity additions: a real symbol section (Fun=129 entries carry
  their code label as value — load-c4r's trap-time stacktrace walks
  these, and a symbol-less kernel null-derefs in it), and exit-255
  stubs for prototype-only calls (c4cc compiles those into unresolved
  extern jumps; c4m.c's never-taken float branch needs exactly this).
  See §11 for measurements.
- **L9 — preprocessor.** DONE (c4lc-pp.lisp, `-P` / `-I` / `-D`).
  The last host-toolchain dependency is gone: c4lc preprocesses for
  itself, and all of C4IX -- kernel, library and userland -- now
  builds with no `gcc -E` anywhere.
  It works on TOKENS, not text, which is the only way expansion is
  correct. The lexer gained a pp mode (off by default, so sources
  already through gcc -E lex exactly as before): `#` becomes a Hash
  token, a backslash at end of line splices without advancing the
  line counter, `<header>` comes back as one token, and an identifier
  carries a fourth field saying whether `(` TOUCHED it. That last bit
  is load-bearing -- it is the only thing separating
  `#define ADD(a,b) ...` from `#define TWO (x+y)`, and the standard
  draws the line exactly at that space.
  Supported: `#include` (`"..."` and `<...>`, searched along -I),
  `#define` object- and function-like, `#undef`, `#ifdef`, `#ifndef`,
  `#if`/`#elif`/`#else`/`#endif` with a real constant-expression
  evaluator (defined(), literals, macros, `!` `-` `*` `/` `+` `<`
  `>` `<=` `>=` `==` `!=` `&&` `||`), and gcc's `# 123 "file"`
  markers, skipped so already-preprocessed input still works.
  Stringize (`#param`) and paste (`##`) are supported too, including
  the two-level idiom: an argument is macro-expanded before
  substitution EXCEPT as an operand of `#` or `##`, which is exactly
  why `STR(V)` yields `"V"` while `XSTR(V)` yields V's value. Paste
  splices the two spellings and re-lexes the result, so it can
  produce an identifier, a number or an operator without the
  preprocessor knowing which in advance.
  A wrinkle worth recording: `#if` and `#else` lex as C KEYWORDS, not
  identifiers, so the directive dispatcher maps them back -- without
  that both silently do nothing and every `#endif` looks unbalanced.
  Verified two ways in test-c4lc: src/tests/c4lc_pp.c exercises every
  supported construct against committed gcc-verified output, and --
  the property that actually matters -- preprocessing a real C4IX
  module with c4lc instead of gcc -E produces a BYTE-IDENTICAL
  object. That held for all eleven kernel modules, and the X5 boot
  pins are unchanged with gcc gone from the build.

- **L10 (DONE): `-conforming` escape sequences.** `lex:conforming`
  selects a real C escape table; off by default (§5.1). The decoder
  moved from `lex:escape`, which returned one byte for a fixed
  two-byte escape, to `lex:escape2`, which returns the index just past
  the escape and leaves the byte in the `lex:val` mailbox — the same
  contract `lex:hex` and `lex:octal` already used, which is what makes
  variable-width `\xHH` and `\NNN` expressible. `lex:strend` needed no
  change: a backslash still swallows exactly one byte for
  quote-scanning purposes, and no hex or octal digit is a quote.
  The preprocessor comes along for free, since it works on tokens that
  the lexer has already decoded.

- **L11 (DONE): integer constant expressions in enum bodies.**
  `p:enumbody` took `Num` or `-Num` only; it now calls `p:const`, the
  constant-expression parser that array sizes already used and that
  already resolved enum constants by name. `p:enums` is updated before
  the recursive call, so a constant named earlier in the same body is
  visible to the next one: `enum { A = 1, B = A + 1 }` works.
  `p:const` itself was widened from `+ - * /` to C's full
  integer-constant operator set — `| ^ & << >> + - * / %` and the
  unary `- + ~ !`, with the unary case recursing so `- -1` and `!!X`
  parse. Its other three call sites (case labels, array sizes, brace
  initializers) inherit that, so `int t[1 << SHIFT]` and
  `case FLAG_A|FLAG_B:` now work too. gcc is the oracle for both:
  c4cc's enum parser (c4cc.c:1342) has the same literal-only
  restriction, so this cannot go in the differential battery.

### Beyond L11 (to 2026-09-30)

Three code-generation flags, all off by default, each documented in its
own design doc:

- **`-mcisc`** (c4or1k M12, [c4or1k-design.md](c4or1k-design.md)):
  emits c4mp's `LXI`/`SXI` for a plain `var[idx]` over an 8-byte-element
  array, instead of the generic `IMM/PSH/SHL/ADD/LI`-or-`SI` sequence.
  Only c4mp executes them; c4m traps them. c4or1k's `-cisc` image is the
  user.
- **`-mfuse`** ([fused-opcodes.md](fused-opcodes.md)): runs c4opt's
  `fuse` pass after the peephole pipeline reaches its fixpoint,
  rewriting common two- and three-instruction sequences into the ten
  fused opcodes (79-88). Implies `-O`. c4mp, oisc4, c4bb and libjs run
  the result; c4m has only three of the ten and plain c4 none, so a fused
  image is for those machines. The same pass is reachable for any image
  as `c4sp c4opt-run.lisp -mfuse in.c4r out.c4r`.
- **`-minline`** (2026-09-07, `d445540`,
  [inline-small-functions.md](inline-small-functions.md)): T3
  substitutes an expression body at its call sites, T4 splices a whole
  body in statement position, and T4b lets a body return early through
  optimizer-only `ijmp`/`ilabel` nodes (the C dialect gained no `goto`
  for it; M7b added `goto` later, below).
  Budget 60 AST nodes. A function whose address is taken is never
  spliced or spliced into. Implies `-O`. `-minline-why` reports what the
  pass decided for every function.

Fixes, 2026-09-07 to 09-10 (each with a regression in `test-c4lc` or the
build that found it):

- **`#else` inside skipped text** (2026-09-07, `25498f0`). `#else` and
  `#elif` flipped the top of the conditional stack without asking
  whether an enclosing level was already skipping, so an `#else` nested
  in dropped text started reading it. Both now inherit the enclosing
  skip (`pp:any-skip?`). Found through `raycast-dos32.c4r` in
  `make c4dos-c4ix32`.
- **Codegen errors name their function** (2026-09-10, `b7d3e7a`). AST
  nodes carry no line numbers, so a codegen error now ends
  `(in function NAME)`.
- **A swallowed `#include`** (2026-09-10, `ab6205b`). A directive
  collected every token sharing its line number, but an included file's
  tokens carry their own file's numbers, so a header ending in `#endif`
  could share a number with the includer's next `#include` line and
  swallow it. The header was silently never included. A directive now
  also stops at the next Hash, except `#define`, whose body may hold `#`
  and `##`. Regression: `src/tests/c4lc_ppinc.c`.
- **Inlining** (above) landed on 2026-09-07 with its own correctness
  tests.

### M7b: the C Busybox needs (from 2026-10-20)

The Busybox port (docs/busybox-port.md) measured what c4lc rejects or
gets wrong, with 113 probes in `src/tests/bbport/` checked against gcc.
Every item lands in c4lc and c4fc together, byte for byte, with gcc's
output as the golden: `src/tests/c4lc_pp.c` for the preprocessor and
`src/tests/c4lc_m7.c` for the language.

- **Preprocessor, now c4fc's rules** (`81ca7e7` and the next commit):
  - **`#if defined(X)`** was always false for a defined X. Expansion
    protected a name only when the token just before it was `defined`; in
    `defined(X)` that token is `(`.
  - **`#if 1 / #elif 1 / #else`** ran the `#else` arm too. A level is now
    false, true or `'Done`.
  - **`#include "x.h"`** is found beside the including file, then along
    `-I`. `<x.h>` searches `-I` only, and the working directory is the
    last resort.
  - **Blue paint:** a macro is not expanded inside its own expansion, so
    hush's `#define msg_and_die_if_script(lineno, ...)
    msg_and_die_if_script(__VA_ARGS__)` terminates.
  - **A function-like name with no `(`** stands for itself instead of
    being an error.
- **Variadic macros:** `...` with `__VA_ARGS__`, GNU `name...`, and GNU
  `, ## __VA_ARGS__`, which drops the comma when the variable part is
  empty. Busybox's generated `autoconf.h` defines every
  `IF_FEATURE_X(...)` this way.
- **`(void)` parameter lists and unnamed parameters.** `void` has a token
  kind of its own (`Void`) for this one purpose. The parser takes it
  wherever it takes `char`, which is what `void` is in c4.
- **`sizeof` of any expression** (`2781e7a`):
  - `sizeof x`, `sizeof(*p)`, `sizeof(G.buf)` and `sizeof("lit")`;
  - `sizeof(T[n])` is `sizeof(T) * n`.
  - The parser keeps `(sizeofa NAME)` for a name and makes
    `(sizeofe E)` for anything else.
  - The generator works the size out from the operand's type and never
    evaluates it (`g:sizeofx`). A string is its bytes and the NUL, and an
    array member is its storage.
  - In constant expressions, `p:const` gains `sizeof(type)`,
    `sizeof("lit")`, the comparisons, `&&`, `||` and `?:`. A struct's
    size is not available there yet.
- **Function pointers** (`c8b1bf0`):
  - `T (*name[N])(params)` declares an `int` (or an array of them), since
    c4 calls through any word. The parameter list is skipped.
  - A call through any expression, `(calle TARGET ARGS…)`, compiles to
    what a JSR would leave behind: the arguments, `IMM` of a return
    label, `PSH`, the target in A, then `JMPA`. The return label is a
    code patch, so the optimizers keep it.
- **`goto` and labels**:
  - `goto NAME;` and `NAME:` are recognised by their shape; `goto` is no
    keyword.
  - The labels are per function (`g:ulab`). A goto to a label that is
    never placed is an error.
  - In a function that has one, T1 drops no constant-condition branch,
    since a goto may enter it. T3 and T4 never splice such a body.
  - `g:scanstmt` counts the frame words declared inside a labelled
    statement.
- **`enum` anywhere a type goes** (`e0d26d3`): `p:spec` reads
  `enum [TAG] [{…}]` as an `int` (`p:enumspec`).
- **Static locals** (`2a0dc89`):
  - A static local is a global named `FUNCTION.NAME`, which `p:statics`
    declares. `p:hoist` puts it just before the function, and
    `p:srenames` maps the plain name inside it.
  - T2 drops it with a dead function.
- **Aggregate initializers**:
  - The parser builds `(agg ITEMS)`, whose items are `(num)`, `(str)`,
    `(name F)`, `(fnaddr F)`, nested `(agg)`, `(desig I X)` and
    `(mdesig M X)`. A list of plain numbers stays `(braces (N…))`.
  - `g:globalaggr` walks it against the type twice: first placing the
    strings pointers take, then writing values at their offsets in the
    zeroed storage. String pointers and function addresses become data
    patches.
  - Brace elision, `[i] =` and `.m =` work, and a union takes one value.
  - T2's roots include every function named anywhere in an initializer.
  - `extern T x[];` declares without sizing.
- **Another unit's function in data**: in an object, a data word holding
  an extern function's address is `(dsym SLOT K)`, written as patch type
  -5 (address a data offset, value the symbol id). c4rlink resolves it
  to -3 or -4.
- **`inline`, `restrict`, `__extension__` and `__attribute__`**:
  `p:untag` drops them from the tokens before parsing. It keeps an
  `__attribute__` that names constructor or destructor.
- **Struct assignment**: `g:structassign` does both addresses, the size
  and `MCPY`, what a builtin `memcpy` call compiles to. The right side
  must be an lvalue.
- **A global's address in an initializer**: `&x`, or an array's name,
  wherever a function's address may go.
  - The patch is `(dglo SLOT NAME)` until the end of the unit, when
    `g:resolvedglo` makes it `(ddata SLOT OFF)` with the bss placed, or
    `(dsym SLOT K)` for extern data, which `g:extdata` registers last.
  - c4fc's `GLOBAL-PAT` does the same at once: `STRPAT,` for an
    initialised global, `SYMPAT,` for extern data, and `GLOPAT,` (flag 4,
    the value a global's number, mapped through `GOFF`) for the rest.
  - `int x;` before `int x = 9;` is one object, at the definition, as in
    c4fc: `g:laterinit?` skips the first.
- **A local array's or struct's brace list**:
  - `p:linit` parses it as a global's, but a leaf that is no constant is
    `(expr E)`. A leaf is an expression when it starts with `&`, or has a
    string, a type name, or a name that no enum or static const answers
    before the next `,` or `}`.
  - An array's list of plain numbers keeps its element-by-element stores.
    Anything else is `g:localaggr`'s: an `MSET` of the whole local, then
    the global walk in a third mode, `'local`, storing each value at its
    offset as the declaration runs.
  - c4fc walks it at parse time into `(offset type node value)` records,
    which its `n_lagg` node stores in the same order.
  - T2 sees the calls in it (`t:aggrefs`, c4fc's `REFS n_lagg`); T1 folds
    none of it.
- **`__func__`, `__LINE__` and `__FILE__`**:
  - `__func__` (and GNU's `__FUNCTION__` and `__PRETTY_FUNCTION__`) is a
    string of the function's name, made by the parser.
  - `__LINE__` and `__FILE__` are made by the preprocessor (`pp:builtin`)
    when a name would be emitted as itself, so a `#define` of either
    still wins. The file is the name it was opened by: a `PPDir` marker
    now carries it next to the directory.
  - A line number counts logical lines: a backslash-newline does not
    advance it, in either compiler, so after one it is short of gcc's.

## 11. Measured results (2026-08-03)

The three big images, compiled three ways. "c4opt" is the peephole
pipeline over the c4cc image; "c4lc -O" is tree passes + generation +
peephole in one process. Counts by c4r-roundtrip's decoder.

| image | c4cc          | c4opt(c4cc)   | c4lc -O       | c4lc vs c4cc |
|-------|---------------|---------------|---------------|--------------|
| c4ke  | 23,260 / 339K | 20,710 / 312K | 18,495 / 276K | −20.5%       |
| c4sp  | 19,695 / 280K | 18,719 / 270K | 16,125 / 235K | −18.1%       |
| c4m   | 15,215 / 225K | 14,538 / 218K | 11,139 / 172K | −26.8%       |

Runtime, all through native c4m:

- **Kernel boot** (`c4ke.c4r test_basic`, cycles to "Kernel ready",
  exactly reproducible run to run): c4cc 898.0k, c4opt 890.5k, c4lc
  **801.9k (−10.7%)**. Wall-clock boot time is unchanged — C4KE's
  boot is dominated by fixed measurement windows, so the win shows in
  cycles, i.e. in work done, not in waiting.
- **Compiler workload** (c4sp.c4r hosting c4lc-eq.lisp compiling
  c4lc_l2.c, median of 3): c4cc build 10.5s, c4opt build 7.32s, c4lc
  build **7.31s — 1.44x faster** than the c4cc build. Output stays
  byte-identical on every variant.
- **innerbench** (`make test`): scores carry ±5-10% run-to-run noise
  from scheduling and interval sampling; medians move the right way
  (total runtime 9.55s c4cc-everything → 8.6-9.4s optimized configs)
  but individual runs overlap. The deterministic metrics above are
  the honest comparison.
- All three c4lc images are drop-in: the c4lc kernel passes
  test-c4ke-ramfs (including compiling inside itself), the c4lc c4sp
  passes the sample battery, and the c4lc c4m runs innerbench's
  nested source-interpreted C4KE.

## 12. Risks and mitigations

- **Speed.** 70x interpretation penalty under c4m (§2). Mitigations:
  native c4sp for development; `c4opt`-optimize c4sp.c4r itself (the
  optimizer improving its own host); if needed, pack the token stream
  into a byte string instead of cons cells.
- **GC pressure.** Token list + AST for c4cc.c ≈ a few hundred
  thousand cells. As first written this needed a large `-c`; since
  2026-08-26 the arena grows with the live set, so no `-c` is needed
  (c4sp-design.md §4.10). The GC is conservative mark&sweep and
  already survives gcloop; watch `gc:stats` in stress tests.
- **Dialect traps** (hard-won this session):
  - Falsiness is `false`, `nil`/`()` and integer `0` (Lisp-style, since
    2026-08-03 — see c4sp-design §2). `0.0` and `""` are truthy. Note
    that a `Num 0` token VALUE is falsy: branch on token kind, never on
    a value that can legitimately be 0.
  - `next` is the only tail call; use it for self-loops. Non-tail
    helper calls are fine (CEK absorbs depth).
  - `=` compares strings by content, atoms by id — both cheap.
  - `argv` elements are ATOMS, not strings: `length` is 0 and
    `string:byte` errors on them. Stringify with `(+ "" a)` before
    string operations; `file:path`/`file:read` accept atoms directly.
  - Paren discipline: check edits with a paren-stack script before
    running; the reader's errors are terse.
- **c4cc quirk drift.** Divergences are pinned in §5.1 and asserted by
  the golden lexer test; anything else that differs is a c4lc bug
  until proven otherwise by the differential battery.

## 13. How images are built: units and c4rlink

The standard way to build a multi-file image in this tree (C4IX,
c4or1k, c4sc, c4mp) is **separate compilation, then link**:

1. Each module is compiled on its own to an object:
   `c4sp c4lc.lisp -O -c -I <dir> <dir>/mod.c mod.c4o`. `-c` makes
   undefined prototypes extern symbols with SYMBOL-typed patches and
   makes `main` optional. `-P`/`-I`/`-D` use c4lc's own preprocessor,
   so no `gcc -E` is involved. The Makefile's `c4lc_compile_par` macro
   runs the units in parallel.
2. `c4rlink a.c4o b.c4o ... -o image.c4r` resolves the externs and
   writes a `.c4r` (format version 3). `-s` strips the symbol table for
   a release image; `-r` (library mode) allows unresolved symbols, which is
   how a `.c4l` library such as `libc4ix.c4l` is made.
3. For 32-bit targets (c4bb, `c4m32`), use `c4sp32` and `c4rlink32`,
   which are the same sources built with `gcc -m32`.

Do not build these with c4cc: it has no structs, typedef or
preprocessor, and its images are 18–27% larger (§11). Single-file
images (`c4sp.c4r`, C4DOS) are compiled by c4lc in one step without
`-c`.

## 14. Integer types and qualifiers (M4, 2026-10)

`signed`, `unsigned`, `short`, `long`, `const` and `volatile` are real,
in c4lc and in c4fc, and the two still emit the same bytes (c4fc's side
is in [c4fc-design.md](c4fc-design.md), "Integer types").

### 14.1 The types

| type | code (+2 per `*`) | `sizeof` | value range |
|---|---|---|---|
| `char`, `void` | 0 | 1 | signed 8-bit (`LC` sign-extends; `CHAR_MIN` is -128) |
| `int`, `signed`, `long` (and `long long` on a 64-bit target) | 1 | word | the word, signed |
| `unsigned char` | 512 | 1 | 0..255 |
| `signed char` | 576 | 1 | -128..127, same representation as `char` |
| `short` | 640 | 2 | -32768..32767 |
| `unsigned short` | 704 | 2 | 0..65535 |
| `unsigned`, `unsigned int` | 768 | word | 0..2^32-1 **at both word sizes** |
| `unsigned long` (and `unsigned long long` on a 64-bit target) | 832 | word | the word, unsigned |
| `long long` on a 32-bit target (M8, 14.7) | 896 | 8 | signed 64-bit |
| `unsigned long long` on a 32-bit target | 960 | 8 | 0..2^64-1 |
| `struct` k | 1024 + 64k | layout | — |

The band at 512 uses the struct rule — 64 apart, two per pointer level
— so `g:plevel` is one formula for everything at or above 512, and the
codes in a `.c4r` symbol record for the old types did not change.
`long` is word-sized. `long long` is 64 bits at both sizes: the word on
a 64-bit target, and two words on a 32-bit one (14.7). Any
order of specifiers parses (`unsigned const short`, `long unsigned`),
and a typedef name only counts as a type while no other type word has
been seen, so `unsigned T;` declares a `T`.

Storage did not change shape for scalars: a global or a local still
takes a whole word. ARRAYS of the new narrow types pack, one byte for
`unsigned char` and `signed char` as for `char`, two for the shorts
(`g:estride`), so `sizeof(short) == 2` and `sizeof(short[5]) == 10`.
Struct members took whole words too until M9; since then a struct is
laid out by C's rule, as gcc lays it out (14.8).

`unsigned int` is 32 bits on a 64-bit host too, because that is what
code written for it (CRC32, inflate, hashes) assumes. Its values are
kept in [0, 2^32) there: every load of one masks (`LI; PSH; IMM
0xFFFFFFFF; AND`), and so does every cast, assignment, `++` and result
that could leave the range. In that range the signed opcodes already
give the unsigned answer, so a 64-bit host compares and divides a uint
with the ordinary `LT`/`DIV`.

### 14.2 What the generator emits

`g:typeof` answers what an expression's type will be WITHOUT generating
it. A binary operator needs that for its right operand before it emits
its left one: once the left side is pushed, nothing in the VM can reach
it to convert it. It mirrors `g:expr`'s typing exactly, c4's own quirks
included (`char + 1` is a `char`; `?:` has its else-arm's type).

- **Usual arithmetic conversions.** Everything narrower than a word
  promotes to `int`; then `int < unsigned < unsigned long`. When neither
  operand is unsigned the code is what it always was. Because c4's
  `int` is a word and `unsigned` is 32 bits, the 32-bit C rule is
  applied at both sizes: `-1 < 1u` is false, `u == -1` is true — what
  gcc says for `int`. For `long` against `unsigned` LP64 gcc says long;
  c4, whose `long` IS `int`, says unsigned (a documented deviation).
- **Ordering** of a whole unsigned word (`unsigned long`, and
  `unsigned` on a 32-bit host): both operands `XOR` the sign bit, then
  the signed compare. `==` and `!=` need nothing.
- **Division** of a whole unsigned word: a call to the `udiv`/`umod`
  helper.
- **`>>`** of a whole unsigned word: by a constant `k`, `SHR` then `AND
  ~(-1 << (W-k))` (nothing for k = 0); by a variable, the `lsr` helper.
  `<<` is the same opcode either way.
- **Loads**: `unsigned char` is `LC; PSH; IMM 255; AND`; `short` and
  `unsigned short` call `ld16s`/`ld16u`; a 64-bit host's `unsigned`
  masks. **Stores**: the one-byte types `SC`, the shorts call `st16`,
  everything else `SI`.
- **Casts emit conversion code**: truncation to a narrow type (`AND`
  for the unsigned ones, a shift up and an arithmetic shift down for
  the signed), and the 32-bit mask for `unsigned` on a 64-bit host.
  A source type that already fits emits nothing. This includes
  `(char)`, which until M4 emitted nothing: it is the one change M4
  makes to an image built from existing source, and it is a fix
  (`(char)300` is 44). Measured over the corpus: the 272 images of
  `src/tests/*.c` (plain, `-O`, `-minline`), C4KE, C4DOS and the twelve
  C4IX objects are byte-identical to the pre-M4 compiler; the 17 that
  differ all differ only because `include/stdio.h` casts with `(char)`,
  and are identical again with that one conversion switched off.
- **Assignment, `++`/`--`, `return`** convert to the target type for
  the new narrow types and for `unsigned` on a 64-bit host, so the value
  of `(s = 70000)` for a `short s` is 4464. `char` keeps c4's own rule —
  `c = 300` stores 44 and has the value 300 — because changing it would
  change every program there is. A post-increment stores the new value
  unconverted and takes the step back off it, which is exact because
  every load converts.
- **`?:`** with an unsigned arm converts both arms to the usual
  arithmetic type (otherwise it keeps c4's else-arm type). One
  deviation: under `-O` a `?:` whose CONDITION is a constant folds to
  the arm it keeps, which then has its own type — `1 ? -1 : 0u` is the
  int -1 there and `4294967295u` without `-O`. The tree pass has names,
  not types, so it cannot see the other arm's; c4fc folds the same way.

The six helpers (`ld16s ld16u st16 udiv umod lsr`, `g:helpbody`) are
emitted once each, after the last function and the prototype stubs,
in that fixed order, and only if something called them. A call is
`PSH; JSR label; ADJ n` to a label in the same unit: there is no symbol,
nothing to link and nothing for the loader to know, so `-c` objects
work unchanged. 16-bit loads go through a helper because computing two
byte addresses from one address needs it twice, and this machine has no
way to copy the top of the stack.

### 14.3 Literals and constant folding

`u`/`U` and `l`/`L` suffixes lex (`NumU`, `NumUL` tokens). `123u` is
`unsigned`, or `unsigned long` if it does not fit 32 bits; any `l` with
a `u` makes it `unsigned long`; `l` alone changes nothing. The AST node
is `(num V TYPE)` — the third field only for an unsigned literal. A hex
literal WITHOUT a suffix stays a signed `int` however large (C would
make `0xFFFFFFFF` unsigned on a 32-bit `int`): doing that would change
the meaning of existing c4 code, so write the `u`.

`p:tfold` (c4lc-parse.lisp) is c4's arithmetic on typed constants, used
by the parser's constant expressions (array sizes, case labels, enum
values, initializers) AND by the tree pass, so a constant folds to the
same number wherever it is folded. Its signed branch is exactly what
both always did. The `>>` it folds is arithmetic for `int` and logical
for unsigned — which also fixed c4fc's constant expressions, whose `>>`
was Forth's logical `RSHIFT`.

### 14.4 Qualifiers

A qualifier mask rides BESIDE the type, never in it: two bits per
pointer level, bit 2k `const` and bit 2k+1 `volatile`, level 0 the
object itself. `const char *p` is const at level 1, `char * const p` at
level 0, an array's elements one level up from the array. The AST
carries it as an optional trailing field, written only when it is not
zero, so a program without qualifiers parses to exactly the tree it
always did (`c4lc-ast.txt` is unchanged):

    (global TYPE "x" SIZE ATTRS INIT Q)   (local TYPE "x" SIZE INIT Q)
    ((PTYPE "p" Q) ...)   (cast TYPE E Q)   member (TYPE "m" ASIZE Q)
    (func ... BODY RQ)    (proto ... ATTRS RQ PQS)   (typedefd "T" TYPE Q)

The symbol table carries it too (`g:symq`), and `g:qualof` computes an
expression's mask the way `g:typeof` computes its type: `*` and `[]`
shift down a level, `&` up, a member inherits its struct's level-0 bits
(an array member hands them to its elements), a cast says its own.

**const.** Errors, which stop the compile:

    error: assignment of read-only location (in function f)
    error: increment of read-only location (in function f)
    error: decrement of read-only location (in function f)

for a write to a const object, through a pointer to const, to a member
of a const struct, or to an element of a const array. Warnings, when a
const below level 0 is dropped without a cast:

    warning: assignment discards const qualifier (in function f)
    warning: initialization discards const qualifier (in function f)
    warning: passing argument N of g discards const qualifier (in function f)
    warning: return discards const qualifier (in function f)

c4fc words them identically (test-c4fc holds it to the same golden
files). Storage is unchanged — the VM has no read-only pages. A
`static const` scalar with a constant initializer is a constant from
there on wherever C wants one (`int t[N]`, `case N:`), converted to its
type, and keeps its storage.

**volatile.** Every read and write of a volatile object must be one
access, in order. The generator emits one load or store per access the
source asks for and caches nothing, so the audit is of what could merge
or drop one later:

- the tree folds combine LITERALS only and never drop an operand that
  is not one — nothing to do;
- the peephole passes fold `IMM`/`IMM` windows and delete only code
  after `JMP`/`LEV` up to the next label — unreachable code — and the
  fused forms (`LDL`, `LDG`, …) are still one access each;
- `-minline` was the one real risk. T3 substitutes an argument's TEXT,
  so a parameter read twice reads the argument twice and an unused one
  drops it. The tree has names, not types, so it is told by name:
  every variable declared with a volatile anywhere in its type, or of a
  struct type with a volatile member, is an access with a side effect
  (`t:vnames`). T3 then refuses such an argument (`t:pure?`), and T4
  binds it to a local, evaluated once (`t:dupok?`).

`src/tests/c4lc_m4v.c` pins it in `c4rdump`'s patch listing: eight
references to a volatile global plain, `-O` and `-minline`, against a
non-volatile control that `-minline` takes from five to six. A 16-bit
volatile access is two byte accesses — the VM has no 16-bit operation.

A spliced call also keeps the conversions the call made: an argument
for an `unsigned char`/`signed char`/`short`/`unsigned short`/`unsigned`
parameter, and a value returned as one, is wrapped in a cast (`t:cwrap`).

### 14.5 The preprocessor

The six new words lex as identifiers in `-P` mode and become keywords
after expansion (`pp:kwfix`) — C's own phase order — because a header
can `#define` them: `include/stddef.h` says `#define unsigned` (to
nothing) and `#define long int` under `__c4cc__`, for c4cc. Since
0f47b17 every keyword does the same: in `-P` mode every word lexes as
an identifier, and `pp:kwfix` turns the survivors back into keywords
from both tables (`lex:keywords`, `lex:keywords2`), as c4fc's lexer
already did. So `#define int long long` (src/tests/factorial.c),
`#undef`/`#ifdef` of a keyword and `#x` of one (its spelling, not 0)
work; `src/tests/c4lc_pp.c` pins them against gcc.

Those two defines are guarded by `#ifndef __c4_inttypes__`, and c4lc's
preprocessor predefines `__c4_inttypes__=1` before any `-D`
(`pp:builtins`, run when c4lc-pp.lisp loads); c4fc's `PP-RESET` does
the same. So stdio.h, stdlib.h and stdarg.h, which pull in stddef.h, no
longer take `unsigned` away — and `unsigned long` is `ulong`, not `int`.
A `gcc -E` whose output goes to c4lc or c4fc stands in for that
preprocessor and passes it too: `$(PREPROC_LC)` in the Makefile is
`$(PREPROC) -D__c4_inttypes__=1`, used by every rule that pipes gcc -E
into c4lc or c4fc (the c4lc -P / gcc -E object identity on vfs.c and
test-c4fc's preprocessor and codegen sweeps among them). `$(PREPROC)`
alone still feeds c4cc, and the c4cc-vs-c4lc runtime differentials,
which compile the same text with both. c4dos's `cpp` does not define
it (nor does build-images.sh's `$PREPROC`, which feeds c4cc32; its
`$PREPROC_LC` feeds c4lc). Every image c4lc builds was checked
byte-identical across the change: the sources it reaches use `long`,
which was `int` before and is `int` now. c4sc.c4r is the exception by
design — it is c4lc's preprocessor compiled, so it gains the builtin
too.

`-D NAME=VALUE` no longer leaves the lexer's `Eof` inside the macro,
which made `#if N == 1` misread.

`#if` arithmetic is one word, signed, and suffixes are dropped, where C
asks for `intmax_t`, at least 64 bits, and unsigned where a `u` says so.
On a 32-bit target `#if 4294967295u >= 1` is false, and a test such as
`UINTPTR_MAX >= UINT64_MAX` cannot tell 32 bits from 64. OpenTyrian's
`swar.h` makes exactly that test, and its port settles it by hand
(src/opentyrian/README.md).

c4lc runs its preprocessor with `-P`, or with `-I` or `-D`, which imply
it. Otherwise the source is taken as already preprocessed, as `gcc -E`
output is.

### 14.6 Tests (test-c4lc, test-c4fc, test-c4sc-back, test-libjs)

- `src/tests/c4lc_m4.c` — the battery: wraparound, compares across the
  sign bit, division of top-bit values, logical shifts, packed 16-bit
  arrays, struct members, casts, the usual arithmetic conversions,
  CRC32 and FNV-1a. Its goldens are gcc's: `c4lc-m4-64.txt` (`gcc`)
  for the 64-bit image and `c4lc-m4-32.txt` (`gcc -m32`) for the one
  `c4sp32` builds and `c4m32` and libjs run; plain, `-O`, `-minline`.
- `src/tests/c4lc_m4q.c` (warnings, then it runs) and
  `src/tests/c4lc_m4e.c` (nine refused writes, `-P -D CASE=n`).
- `src/tests/c4lc_m4v.c` — volatile, above.
- All three are in `C4FC_GEN_SWEEP` and `C4SC_BACK_DIFF`.
- `src/tests/c4lc_m4std.c` — unsigned compares, division, shifts,
  wraparound, `unsigned long` and a CRC32 in a file that includes
  stdio.h and stdlib.h; golden `c4lc-m4std.txt` is gcc's and does not
  depend on the word size. test-c4lc runs it through `$(PREPROC_LC)`
  (c4lc's own preprocessor refuses stdarg.h's function-like macro named
  without arguments) at 64 and 32 bits, plain and `-O`, and through
  `c4lc -P -D STDDEF_ONLY` (stddef.h alone); test-c4fc compiles it with
  c4fc's own `-P` and runs it, and `C4FC_GEN_SWEEP` holds that image to
  c4lc's.

### 14.7 `long long` on a 32-bit target (M8, 2026-10-20)

On a 64-bit target `long long` is the word, as before, so nothing there
changed. On a 32-bit target -- the browser's C4IX, `c4sp32`, and c4sc
inside C4IX -- it is now two words, `long long` (896) and `unsigned long
long` (960), the codes the parser makes only when `sys:wordsize` is 4.

- **A value is an address.** The VM's one register holds a word, so a
  long long is handled as a struct is: an expression of the type leaves
  the ADDRESS of its eight bytes (low word first) in A. A variable, a
  member, an element or `*p` is its own storage (`g:load!` emits
  nothing); a computed value goes to a temporary in the frame
  (`g:ltemp`), below the locals and block declarations, freed at the
  end of the statement that made it (`g:stmt`). How many a function
  needs is known only after its body, so its ENT is emitted as
  `(ENT (frame ID WORDS))` and `g:resolvecode` adds the temporaries at
  the end -- 0 for code without long long, whose bytes are unchanged
  (all of C4IX's 92 objects compile identically).
- **Stores** copy eight bytes with `MCPY`, which leaves the destination
  in A, so an assignment's value is the stored one.
- **Conversions.** A word becomes a long long where it is generated
  (`g:llval`, `g:llinto`): its low half, and for its high half the sign
  (an arithmetic `SHR` by 31) or 0 for the unsigned types and pointers.
  A long long becomes a word with `LI`, its low half (`g:convert!`,
  `g:sconv!`). An index, a shift count and a `switch` are words
  (`g:iexpr`); a condition tests both halves (`g:texpr`, `__c4ll_nz`).
- **Arithmetic** calls helpers written in C, `p:llsrc` in
  c4lc-parse.lisp: add, sub, mul (16-bit limbs), and, or, xor, the three
  shifts, signed and unsigned compares, div, mod, udiv, umod (shift and
  subtract, with a one-word fast path when both high halves are 0), neg.
  `~` is inline. The parser reads them after any unit that says `long
  long`; they are static, registered when first called (`g:find`, kept
  in `g:llsyms` outside the scopes), and generated only then -- each
  helper's own calls come before its turn, so the order of `p:llsrc` is
  callers first. -O's dead-function pass keeps them as roots.
- **Calls.** A long long argument is two words, the high one pushed
  first so the low one is at the lower address, the parameter's
  (`g:params`, `g:pwords`: a function's ArgCount is in words). A long
  long result comes back through a pointer to the caller's temporary,
  pushed after the arguments: the callee's `return` copies through
  `LEA 2`.
- **Literals** with an `ll` suffix are read again in four 16-bit limbs
  (`lex:wide`), since a 32-bit compiler cannot hold them in one number:
  a `NumLL`/`NumULL` token, an `(llnum LO HI TYPE)` node, and the parser
  adds the helpers for one as it does for the keyword. Without the
  suffix a literal keeps 14.3's rule however large -- code written for
  the 64-bit word (c4sp's float library has `0x7FFFFFFFFFFFFFFF`) wraps
  to a word, as it always has.
- **Inlining.** A long long parameter of an inlined call gets a cast
  (`t:convty?`), so an int argument widens; a function returning long
  long is never spliced (T4), since its result is a temporary the
  splice's block would free.
- **Not done:** `printf`'s `%lld` on a 32-bit host (a long long argument
  is pushed as two words, which the runtimes do not read yet); a long
  long constant expression (array sizes, `case`); a long long returned
  through a function pointer. c4fc on a 32-bit host refuses `long long`
  instead of giving it 32 bits; its parity with c4lc is held at 64 bits,
  where long long is the word in both.
- **Test:** `src/tests/c4lc_ll.c`, golden `c4lc-ll.txt` from gcc (the same
  at 32 and 64 bits): literals and globals, every operator, unsigned
  division and compares, shifts by constants and variables, widening and
  narrowing, `++`/`--` and compound assignment, arguments and results,
  recursion (20!), struct members, arrays, pointers, conditions. In
  test-c4lc at both sizes, plain, `-O` and `-minline`, and in
  `C4FC_GEN_SWEEP` and `C4SC_BACK_DIFF`.

Found on the way: c4fc had no compound assignment (`+=` and the rest
lexed, but its parser stopped at them). It now rewrites `L op= R` as
`L = L op R`, as c4lc does, and produces the same bytes.

### 14.8 What Wolfenstein 3D needed (M9, 2026-10-22)

The Wolf3D port (docs/c4ix-wolf3d.md) wrote around what c4lc lacked:
2-D arrays flattened by hand, file headers read a byte at a time, a
struct passed by pointer, and so on. These are now c4lc's, and c4fc's
where c4fc can follow, byte for byte, and the port is back to Wolf4SDL's
own source.

- **Several function declarators in one declaration**, `void A(void),
  B(void);`: a prototype ends at a comma as at a semicolon.
- **Array parameters**: `char *argv[]` and `int v[10]` are pointers, and
  `int m[][4]` is a pointer to a row (below).
- **Braces around a scalar's initializer**, `int y = { 7 };` and `const
  char *h = { "a" "b" };`, local or global.
- **A typedef inside a function** (c4lc only: c4fc has no typedef). It
  names its type to the end of the function; p:function puts the file's
  typedefs back after the body. A struct it defines is hoisted before the
  function, as a static local's global is.
- **A struct passed by value.** A parameter takes the struct's words
  (`g:pw`), the first at the lowest address, as a long long's do. The
  caller keeps the struct's address in a two-word temporary and pushes
  its words from the last (`g:structpush`); the temporary is free again
  once they are pushed. The callee's copy is its own storage (the symbol
  carries its bytes, as a local struct's does), so changing it changes
  nothing of the caller's. A function with a struct parameter is never
  spliced (T3, T4: `t:sparams?`), since a splice would hand it the
  caller's struct. The argument must be an lvalue of the parameter's
  struct type. Struct layouts now go first of all (`g:structpass`): the
  function passes count parameter words from them.
- **sizeof of an expression in a constant**: `sizeof(table) /
  sizeof(table[0])`, `sizeof(*menu)`, `sizeof x.member`, `sizeof msg`, in
  an array size, a case label or an initializer, over GLOBALS (c4fc's
  parser answers it for locals too). The parser now lays out a struct as
  it reads it (`p:slayout`) and keeps every global's type and size
  (`p:gvars`); `p:csizeofx` works the size out from the expression's type,
  as `g:sizeofx` does, without generating anything. The generator takes
  the parser's layout (`g:structdef`). In whole-program mode an extern
  array no unit defines and nothing uses -- Wolf3D's `CASSERT` -- is no
  longer an error; using it still is. (c4fc gives such an extern storage
  there, so the two differ on it.)
- **Arrays of arrays.** `T a[N][M]` is an array of N ROWS: a row is a
  type of its own in the struct band, M elements of T and exactly M times
  T's stride, with no members (`p:rowtype`). `p:arraysize` reads every
  dimension and `p:elemty` makes the element type of those after the
  first, innermost first, reusing a row already made. A row travels to
  the generator as a `structdef` node with two more fields, ELEM and
  COUNT. Used as a value, a row is the address of its first element: an
  index or a `*` that comes to a row decays it (`g:decay`) instead of
  loading, and `g:typeof` says so, so `a[i][j]`, `&a[i][j]`, `a[i] + 1`
  and passing `a[i]` all follow from the one-dimensional rules. `sizeof`
  of an index or a `*` is taken before the decay, so `sizeof(a[i])` is the
  row's. Initializers walk a row as an array of its elements: nested
  braces, strings for char rows, and a flat list (`int m[][2] = {1, 2, 3,
  4}`, whose row count `p:initsize` divides out). Globals, statics,
  locals, externs, struct members and parameters all take them.
- **Initializers computed at start-up** (c4lc only). C lets a global's
  initializer be any constant expression, and gcc folds floating point
  and an address plus an offset. c4lc folds neither: a float constant is
  its bits, and another unit's global is a patch that carries no offset.
  So a scalar global whose initializer is none of the forms `p:init`
  takes (`p:dyninit?`: not a string, `&NAME`, a lone NAME or float
  literal, or an integer constant expression) is set by `__c4lc_init`, a
  static constructor the parser writes after the unit, before the long
  long and float helpers so that the ones it calls are compiled. It
  stores through `*(T *)&NAME`, so a `const` global is set too. Wolf3D's
  `const float radtoint = (float)(FINEANGLES/2/PI);` and `fixed *costable =
  sintable+(ANGLES/4);` are two. Nothing can tell, short of another
  constructor reading the global first.
- **C's struct layout.** A member starts at a multiple of its own
  alignment -- 1 for the one-byte types, 2 for the shorts, the word for
  the word-sized ones, min(8, word) for long long and double (as i386's
  gcc has it), a row's element's, a struct's own -- and a struct is as
  aligned as its most aligned member, its size rounded up to that. An
  array of structs steps by the struct's size (`g:estride`). So a struct
  laid over a file's bytes -- a map header, a font, a sprite -- matches
  them as it does under gcc, at both word sizes. The rule is `p:slayout`;
  every image in the repo was rebuilt with it.
- **`#pragma pack`**: `pack(n)`, `pack()`, `pack(push[, n])` and
  `pack(pop)`, as gcc takes them, between declarations. A member's
  alignment is capped at n (`p:palign`), and so its struct's, which the
  struct keeps wherever it is used later. Wolf4SDL's `wl_def.h` sets
  `pack(1)` for every struct after it, the file headers among them. The
  preprocessor passes the line on as a `PragmaPack` token and its
  arguments; with no `-P` the lexer does the same with what gcc -E left,
  and `p:pragmapack` settles push and pop. Every other pragma is still
  dropped.
- **Tests**: `src/tests/c4lc_m9.c` (c4lc and c4fc) and
  `src/tests/c4lc_m9lc.c` (c4lc only), goldens `c4lc-m9.txt` and
  `c4lc-m9lc.txt` from gcc, the same at 32 and 64 bits: sizes are printed
  in units of `long`, the word to both, or past a multiple of it. In test-c4lc at both sizes,
  plain, `-O` and `-minline`; `c4lc_m9` is in `C4FC_GEN_SWEEP`, both in
  `C4SC_BACK_DIFF`.

### 14.9 What Doom needed (M10, 2026-10-23)

docs/ports-feasibility.md measured what linuxdoom-1.10 needs from c4lc,
and its §8 put the fixes in order. Steps 1-5 are these, and with them
c4lc compiles all 59 of Doom's units as they stand, and the platform
stub. `doomprobe.c` (Doom's idioms, in the session notes' scan tools)
prints what gcc `-m32` prints, without its rewrites, plain, `-O` and
`-minline`. c4fc follows where its grammar reaches, byte for byte.

- **`register`** goes with `inline` (`p:dropids`). Alone before a name,
  as am_map.c's `register outcode1 = 0;`, it is C89's int: `register`
  followed by a name and then `=`, `,`, `;`, `[` or `)` becomes an `int`
  token (`p:implicitint?`).
- **Declarations of globals inside a block**: `extern int
  forwardmove[2];` and `void V_MarkRect(int, int, int, int);` in a
  function body. They are queued as a static local's global is and go
  just before the function (`p:blockexterns`, `p:localproto`), so their
  scope is wider than C gives them, which costs nothing: they allocate
  nothing. A prototype may share a line with locals.
- **Sparse switches.** A switch whose cases span `g:SWRANGE` (4096) or
  more -- or so much that the span wraps a 32-bit host's word, INT_MIN to
  INT_MAX -- is a chain of compares in the order the cases are written,
  not a jump table (`g:swchain`): `PSH; IMM c1; XOR; BZ case1`, then
  `PSH; IMM c1^c2; XOR; BZ case2` and so on, and a JMP to the default or
  the end. a holds the value XORed with the case at hand, zero on a
  match, so no branch leaves anything on the stack. st_stuff.c's
  `case AM_MSGENTERED:` is `('a'<<24)+('m'<<16)` plus a little. A dense
  switch keeps its table, byte for byte.
- **`&a[k]` as a scalar global's initializer**, `boolean *mousebuttons =
  &mousearray[1];`, is set at start-up as `a + k` is (14.8): `p:dyninit?`
  now passes anything after `&` but a lone name.
- **Casts in a constant expression**: `case (Uint32)KEY | (Uint32)MOD
  << 16:`, an enum value, an array size. `p:constcast` converts the
  operand as the generated code would (`p:convval`); a pointer type keeps
  the value.
- **Constants folded in a global's brace list.** An element that none of
  `p:init`'s forms takes -- a cast, an address plus an offset, floating
  point -- is parsed as an expression and folded (`p:foldleaf?`,
  `p:cfold`), as gcc folds it:
  - integers by `p:tfold`, as `p:const` folds them;
  - an address as a global's name and a byte offset: `&S_sfx[sfx_pistol]`
    (a table naming its own element: `p:curgv` is the global being
    initialized), a row `seq[0]` of an array of arrays, `&a[k][j]`,
    `hold.arr`, `&s.m`, `a + k`, `(int *)&x`, `(long)"str"`, `"abc" + 1`.
    It is `(gaddr NAME OFF)` or `(sadr STR OFF)`, and the generator's data
    patch carries the offset: a `dglo` patch gains a fourth field. For
    another unit's data the patch is a symbol's (type -5), which has no
    room for one, so the offset is left in the data word and c4rlink adds
    it when it resolves the symbol (zero, as before, for the symbol
    itself);
  - floating point exactly: a value is a rational over the lexer's big
    integers (`lex:bmul` and the rest, with `p:badd`, `p:bmul`, `p:bdiv`),
    and after every operation it is rounded to float or double as C
    rounds, through the lexer's own `lex:fpscale` (`p:ratround`). So
    `-.867*R` for an int member is what gcc makes of it, and `0.1f + 0.2f`
    has a float's bits. The result is `(fval FLT)`, which `g:fpinit`
    converts to the member's type: a float's or a double's bits, or the
    integer C's conversion makes of it.
  A scalar global's initializer still goes through `p:dyninit?` to
  start-up when it is more than `p:init` takes; only a lone float literal
  of the other type, `float mouse_acceleration = 2.0;`, or one
  initializing an integer, is now converted at compile time rather than
  refused (`p:fvalinit`, `p:litrat`).
- **Tests**: `src/tests/c4lc_m10.c` (c4lc and c4fc) and
  `src/tests/c4lc_m10lc.c` (c4lc only: typedefs, floats, start-up),
  goldens `c4lc-m10.txt` and `c4lc-m10lc.txt` from gcc (`-std=gnu89`, for
  the implicit int), the same at 32 and 64 bits; in test-c4lc at both
  sizes, plain, `-O` and `-minline`. `c4lc_m10` is in `C4FC_GEN_SWEEP`,
  both in `C4SC_BACK_DIFF`.
  `c4lc_m7ext_a.c` gained `&shared_s[1]` over another unit's array,
  linked by c4rlink at both sizes.

### 14.10 A typedef of an array type (M11, 2026-10-23)

Step 6 of docs/ports-feasibility.md §8, for doomgeneric's `sha1.h`,
`typedef byte sha1_digest_t[20];`. c4lc only: c4fc has no typedef.

- `typedef T NAME[N]` (and `[N][M]`) names a **row** (14.8): N elements of
  T, of N times T's stride, which stands for the address of its first
  element wherever its value is used. That is C's array, and `sizeof` of
  the type is the row's size.
- A declarator of a row's type with no brackets of its own -- a global, a
  local, a member -- is laid out as the array it is: `digest_t d;` is
  `byte d[20];`. `p:arraysize/t` reports the row's count, and `p:elemty`
  then gives its element, so every site that took `T name[N]` takes it.
  With brackets of its own, `digest_t ds[3];`, it is an array of rows, as
  `byte ds[3][20]` is.
- A parameter of a row's type is a pointer to its element, as `T p[N]`'s
  is. A pointer to one, `digest_t *p`, steps a row at a time.
- Tests: `c4lc_m11lc.c` (globals, an extern, struct members, parameters,
  `sizeof`, a 2-D typedef, a pointer to one, `memcpy` over one), gcc's
  golden, the same at 32 and 64 bits; in test-c4lc at both sizes, plain,
  `-O` and `-minline`, and in `C4SC_BACK_DIFF`.

### 14.11 What OpenTyrian needed (M12, 2026-10-23)

Steps 8-10 of docs/ports-feasibility.md §8, and what OpenTyrian then
turned up. c4lc only: c4fc has no typedef, so most of this cannot be
written there.

- **Declarations in `for`.** `for (int i = 0, j = 2; ...)` (`p:forstmt`)
  is the loop wrapped in a block that starts with a `declstmt`. The names
  are scoped to the loop, an outer `i` is shadowed and comes back after
  it, and loops can nest.
- **Structs returned by value.** A function that returns a struct takes
  a hidden last argument: where to put it (`g:sret?`). This is how
  `long long` is returned (14.7).
  - The caller passes a temporary of its own frame (`g:ltempw`), so each
    call, a recursive one too, has its own.
  - `return E;` copies E there with `MCPY`.
  - The value can initialize, be assigned, be a member access
    (`mk(6).b`), be an argument to a struct parameter, or be returned
    again (`g:sval`).
  - A struct initializer can also be a struct expression: a variable, a
    member, a call.
- **Compound literals.** `(T) { ... }` is a hidden local, `$clN`, which
  is initialized where the literal is, each time it is reached, and then
  is an lvalue (`p:cmpdlit`, the `cmpd` node). A function that has one
  is never inlined (attribute bit 128, `t:noinline?`): its frame is not
  the caller's. OpenTyrian's `MemReader` returns a struct literal, and
  its `demo.c` passes an array literal.
- **`sizeof` of a local array in an array size**, `int parents[sizeof
  menus / sizeof *menus]` (`p:lvars`). `sizeof` of a static local
  reaches it through its renamed global, `FN.NAME`.
- **A parenthesized declarator**, `unsigned int (i)` (`p:unparen`).
- **`&A`, A an array of a row's shape (a typedef's, 14.10),** is a
  pointer to that row, as C's `&A` points at the whole array
  (`g:addrty`, `g:arrrow`). OpenTyrian's `JE_gammaCorrect(&colors)`,
  `colors` a `Palette`, `SDL_Color[256]`, indexes `(*colorBuffer)[x]`.
  As a pointer to a pointer, that read the palette's first bytes as an
  address.
- **A scalar local's initializer with a float on one side converts**, as
  an assignment's value does (`g:fpinit/conv`). Before this,
  `int t = c * r;` with `r` a float, and `float f = i;`, stored the other
  type's bits. OpenTyrian's `JE_gammaCorrect_func` is the first of those,
  and its palettes faded to white. A global's initializer was already
  right: it is folded (`p:fvalinit`).
- **Lexer quirks.** c4cc's escapes (section 5, L10) are kept unless `-conforming`
  is given. OpenTyrian is built with it, because a `switch` on `'\n'`
  and `10` was a duplicate case.
- Tests: `c4lc_m12lc.c`. It covers the `for` forms, struct values in
  every position above, recursion, compound literals (one in a loop, made
  afresh each time), `sizeof` of a local, and initializers that convert.
  `c4lc_m11lc.c` covers `&A` of a typedef'd array. Both have gcc's
  goldens, the same at 32 and 64 bits. They run in test-c4lc at both
  sizes, plain, `-O` and `-minline`.

### 14.12 What Rise of the Triad needed (M13, 2026-10-24)

Steps 11 and 12 of docs/ports-feasibility.md §8, for icculus.org's port.
c4lc only.

- **K&R definitions**, `int CompareHeights(s1p,s2p) visobj_t **s1p,**s2p;`
  (`p:krdecls`). The declarations between `)` and `{` give the parameters
  they name their types; one no declaration names stays an int, as C89
  has it. `T name[]` there is a pointer to T, as in a prototype.
- **`int + pointer`** is the pointer's (`g:bintype/2`) and is generated as
  `pointer + int` (`g:binary`). Rise of the Triad writes
  `*(dest+bufferofs) = pixel;` with `dest` the int.
- **A pointer to a function pointer**, `T (**pf)(args)` (`p:fnstars`,
  `p:fnty`): each `*` after the first adds a pointer to the word a function
  pointer is. A call through `(*pf)(x)` calls what `pf` points at. The
  parser used to drop the `*` of `(*fp)(x)` whatever `fp` was; the
  generator now drops it only where the operand is not a pointer
  (`g:calltgt`).
- **A string that fills its array exactly**, `char SaveName[13] =
  "rottgam?.rot\0";`. Its NUL is dropped, as C says, instead of "string
  does not fit".
- Tests: `c4lc_m13lc.c`, with gcc's golden (C89, for the K&R
  definitions), the same at 32 and 64 bits. It runs in test-c4lc at both
  sizes, plain, `-O` and `-minline`, and in `C4SC_BACK_DIFF`.
