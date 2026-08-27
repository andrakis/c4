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

int *sc_eval_str (char *text) {
	return eval(rd_read(text, cs_strlen(text)), sc_genv);
}

int main (int argc, char **argv) {
	int *orig, *m, *m2, *out, *r;
	char *in, *outname;
	int   fuse;

	gc_stack_base = (int *)&orig;
	fuse = 0;
	--argc; ++argv;
	if (argc > 0 && !memcmp(*argv, "-mfuse", 7)) { fuse = 1; --argc; ++argv; }
	if (argc < 2) { printf("usage: c4sc-host [-mfuse] in.c4r out.c4r\n"); return 1; }
	in = *argv; outname = argv[1];

	if (atoms_init()) return 1;
	if (gc_init(8000000)) return 1;
	if (pr_init()) return 1;
	sc_genv = mk_env(0);
	gc_root_genv = sc_genv;
	stdlib_init(sc_genv);

	sc_eval_str("(load \"src/c4sp/lisp/c4r.lisp\")");
	if (c4sp_err) { printf("c4sc-host: %s\n", c4sp_err_msg); return 1; }
	sc_eval_str(fuse ? "(define opt:fuse-on true)" : "(define opt:fuse-on false)");

	// the driver reads through file:path, and so does this: c4sp resolves
	// a name against its search path there, not in file:read.
	orig = sc_call_named("file:path", cons(mk_string(in), 0));
	orig = sc_call_named("file:read", cons(orig, 0));
	if (c4sp_err || !orig) { printf("c4sc-host: cannot read %s\n", in); return 1; }
	m = sc_call_named("c4r:decode", cons(orig, 0));
	if (c4sp_err) { printf("c4sc-host: %s\n", c4sp_err_msg); return 1; }

	m2 = sc_bridge_optimize(cons(m, 0));
	if (c4sp_err) { printf("c4sc-host: %s\n", c4sp_err_msg); return 1; }

	out = sc_call_named("c4r:encode", cons(m2, 0));
	if (c4sp_err) { printf("c4sc-host: %s\n", c4sp_err_msg); return 1; }
	pr_reset(); cell_write(orig, 0);
	printf(";; c4opt: %s - %d -> %d bytes\n", in, orig[CELL_B], out[CELL_B]);
	r = sc_call_named("file:write", cons(mk_string(outname), cons(out, 0)));
	if (c4sp_err || cell_is_false(r)) { printf("c4sc-host: write failed\n"); return 1; }
	return 0;
}
