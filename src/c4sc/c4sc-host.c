// c4sc-host.c -- the M2 harness: c4opt.lisp COMPILED, everything else
// interpreted, and the images have to come out identical.
//
//   c4sc-host [-mfuse] in.c4r out.c4r
//
// This is c4opt-run.lisp with exactly one substitution. c4r.lisp's
// decoder and encoder run in the interpreter, reached through the same
// bridge the compiled unit uses for cons and second; only the optimizer
// is compiled. If a single byte of the output moved, the transliteration
// would be wrong, and this is the smallest program that can say so.
//
// It changes nothing in c4sp: same reader, same evaluator, same
// collector, same builtins, all included unmodified below.
#include "c4.h"
#include "c4m.h"
#include "c4_float.h"
#include "src/c4sp/include/cell.h"
#include "src/c4sp/include/gc.h"
#include "src/c4sp/include/cells.h"
#include "src/c4sp/include/atoms.h"
#include "src/c4sp/include/read.h"
#include "src/c4sp/include/stdlib.h"
#include "src/c4sp/include/eval.h"
#include "src/c4sc/scrt.h"
#include "src/c4sc/host.h"
#include "src/c4sc/c4opt_gen.c"
#include "src/c4sc/c4lex_gen.c"
#include "src/c4sc/c4pp_gen.c"
#include "src/c4sc/c4parse_gen.c"
#include "src/c4sc/c4c4r_gen.c"
#include "src/c4sc/c4tree_gen.c"
#include "src/c4sc/c4gen_gen.c"

#include "src/c4sc/body.h"
