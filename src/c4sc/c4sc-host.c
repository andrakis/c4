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

int *sc_eval_str (char *text) {
	return eval(rd_read(text, cs_strlen(text)), sc_genv);
}

// tokens mode: c4lc-tokens.lisp, with lex:file compiled. The output has
// to match the interpreted driver line for line, which is what
// src/c4sp/tests/expected/c4lc-tokens.txt already pins for c4lc.
int sc_tokens (int argc, char **argv) {
	int *toks, *l;
	int  counting, conforming, n;
	char *file;

	counting = conforming = 0;
	if (argc > 0 && !memcmp(*argv, "-count", 7)) { counting = 1; --argc; ++argv; }
	else if (argc > 0 && !memcmp(*argv, "-conforming", 12)) { conforming = 1; --argc; ++argv; }
	if (argc < 1) { printf("usage: c4sc-host tokens [-count|-conforming] file.c\n"); return 1; }
	file = *argv;

	sc_init_lex();
	L_lex_58conforming = conforming ? cell_true : cell_false;
	toks = L_lex_58file(mk_string(file));
	if (c4sp_err) { printf("c4sc-host: %s\n", c4sp_err_msg); return 1; }

	if (counting) {
		n = 0; l = toks;
		while (cell_type(l) == T_CONS) { ++n; l = (int *)l[CELL_B]; }
		printf("tokens %d\n", n);
		return 0;
	}
	l = toks;
	while (cell_type(l) == T_CONS) {
		pr_reset();
		cell_write((int *)l[CELL_A], 0);
		printf("%s\n", pr_term());
		l = (int *)l[CELL_B];
	}
	return 0;
}

int main (int argc, char **argv) {
	int *orig, *m, *m2, *out, *r;
	char *in, *outname;
	int   fuse, tokens, cells;

	gc_stack_base = (int *)&orig;
	fuse = 0;
	tokens = 0;
	cells = 4000000;   // the same default the timings use for c4sp
	--argc; ++argv;
	if (argc > 1 && !memcmp(*argv, "-c", 3)) {
		--argc; ++argv;
		cells = 0;
		in = *argv;
		while (*in >= '0' && *in <= '9') cells = cells * 10 + (*in++ - '0');
		--argc; ++argv;
	}
	if (argc > 0 && !memcmp(*argv, "tokens", 7)) { tokens = 1; --argc; ++argv; }
	else if (argc > 0 && !memcmp(*argv, "opt", 4)) { --argc; ++argv; }
	if (argc > 0 && !memcmp(*argv, "-mfuse", 7)) { fuse = 1; --argc; ++argv; }
	if (!tokens && argc < 2) { printf("usage: c4sc-host [opt] [-mfuse] in.c4r out.c4r\n"); return 1; }
	in = *argv; outname = tokens ? 0 : argv[1];

	if (atoms_init()) return 1;
	if (gc_init(cells)) return 1;
	if (pr_init()) return 1;
	sc_genv = mk_env(0);
	gc_root_genv = sc_genv;
	stdlib_init(sc_genv);

	if (tokens) return sc_tokens(argc, argv);

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
