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
interoperable with c4cc objects in the same link. Next: X0, the
first C4IX boot.

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
skipping, floats beyond what c4cc does.

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
other escaped char is itself. `#` skips to end of line. `//` and
`/* */` comments.

### 5.1 Deliberate divergences from c4cc (all c4cc bugs)

- Newlines inside `/* */` comments increment the line counter (c4cc
  loses count).
- An empty char literal `''` is Num 0 (c4cc leaves stale ival).
- A multi-char literal `'ab'` is the last char, as c4cc.

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
ATTRS is c4cc's bitmask. Enum constants substitute as (num N) at use
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
  thousand cells; pass `-c`. The GC is conservative mark&sweep and
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
