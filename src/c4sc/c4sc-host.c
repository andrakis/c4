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

// pp mode: c4lc-ppdump.lisp, with the preprocessor compiled. The flags
// are parsed HERE rather than in a Lisp driver, because pp:paths is a
// global the compiled unit owns -- a driver's (set! pp:paths ...) would
// set the interpreter's copy and the compiled code would read its own.
// Same reason opt mode carries opt:fuse-on across by hand.
int sc_pp (int argc, char **argv) {
	int *paths, *tail, *e, *toks, *t;

	sc_init_lex();
	sc_init_pp();
	paths = tail = 0;
	while (argc > 1 && **argv == '-') {
		if (!memcmp(*argv, "-I", 3)) {
			e = cons(mk_string(argv[1]), 0);
			if (tail) { tail[CELL_B] = (int)e; tail = e; } else paths = tail = e;
		} else if (!memcmp(*argv, "-D", 3)) {
			L_pp_58predefine(mk_string(argv[1]));
		} else { printf("c4sc-host: unknown flag %s\n", *argv); return 1; }
		argc = argc - 2; argv = argv + 2;
	}
	if (argc < 1) { printf("usage: c4sc-host pp [-I dir] [-D name] file.c\n"); return 1; }
	L_pp_58paths = paths;

	toks = L_pp_58file(mk_string(*argv));
	if (c4sp_err) { printf("c4sc-host: %s\n", c4sp_err_msg); return 1; }
	while (cell_type(toks) == T_CONS) {
		// the driver prints (+ "" kind) and (+ "" value), which is
		// cell_write on each with a space between
		t = (int *)toks[CELL_A];
		pr_reset();
		cell_write(sc_head(t), 0);
		pr_ch(' ');
		cell_write(sc_head(sc_tail(t)), 0);
		printf("%s\n", pr_term());
		toks = (int *)toks[CELL_B];
	}
	return 0;
}

// pptok mode: lex an ALREADY preprocessed file in pp mode and dump the
// same kind/value pairs pp mode dumps. This is the gcc -E side of the
// differential test-c4fc uses: gcc preprocesses, we lex; we preprocess,
// we lex. The two token streams must agree, which is a stronger check
// than comparing text (whitespace and line markers differ).
int sc_pptok (int argc, char **argv) {
	int *toks, *t;

	if (argc < 1) { printf("usage: c4sc-host pptok file.i\n"); return 1; }
	// NOT pp mode: an ordinary lex SKIPS '#' lines, which is how gcc's
	// `# 12 "file"` markers stay out of the comparison. test-c4fc's
	// differential is built the same way, LEX-FILE against PP-FILE.
	sc_init_lex();
	toks = L_lex_58file(mk_string(*argv));
	if (c4sp_err) { printf("c4sc-host: %s\n", c4sp_err_msg); return 1; }
	while (cell_type(toks) == T_CONS) {
		t = (int *)toks[CELL_A];
		pr_reset();
		cell_write(sc_head(t), 0);
		pr_ch(' ');
		cell_write(sc_head(sc_tail(t)), 0);
		printf("%s\n", pr_term());
		toks = (int *)toks[CELL_B];
	}
	return 0;
}

// ast mode: c4lc-ast.lisp, with lex:file and parse:program compiled.
int sc_ast (int argc, char **argv) {
	int *ast, *l;
	int  checking, n;

	checking = 0;
	if (argc > 0 && !memcmp(*argv, "-check", 7)) { checking = 1; --argc; ++argv; }
	if (argc < 1) { printf("usage: c4sc-host ast [-check] file.c\n"); return 1; }

	sc_init_lex();
	sc_init_parse();
	ast = L_parse_58program(L_lex_58file(mk_string(*argv)));
	if (c4sp_err) { printf("c4sc-host: %s\n", c4sp_err_msg); return 1; }

	l = sc_tail(ast);
	if (checking) {
		n = 0;
		while (cell_type(l) == T_CONS) { ++n; l = (int *)l[CELL_B]; }
		printf("parse ok %s (%d decls)\n", *argv, n);
		return 0;
	}
	while (cell_type(l) == T_CONS) {
		pr_reset();
		cell_write((int *)l[CELL_A], 0);
		printf("%s\n", pr_term());
		l = (int *)l[CELL_B];
	}
	return 0;
}

// compile mode: c4lc.lisp, with every phase compiled -- lexer,
// preprocessor, parser, tree passes, code generator, peephole passes
// and the .c4r writer. Nothing is left in the interpreter but file:read
// and file:write, which is why this mode is the one the byte-identity
// test uses.
//
//   c4sc-host compile [-O] [-c] [-P] [-mcisc] [-mfuse] [-conforming]
//                     [-I dir] [-D name] in.c out.c4r
int sc_compile (int argc, char **argv) {
	int *paths, *predefs, *ptail, *dtail, *e, *ast, *m, *out, *r;
	int  opt, obj, usepp, cisc, fuse, conf;
	char *in, *outname;

	opt = obj = usepp = cisc = fuse = conf = 0;
	paths = predefs = ptail = dtail = 0;
	while (argc > 0 && **argv == '-') {
		if      (!memcmp(*argv, "-O", 3)) opt = 1;
		else if (!memcmp(*argv, "-c", 3)) obj = 1;
		else if (!memcmp(*argv, "-P", 3)) usepp = 1;
		else if (!memcmp(*argv, "-mcisc", 7)) cisc = 1;
		else if (!memcmp(*argv, "-mfuse", 7)) { fuse = 1; opt = 1; }
		else if (!memcmp(*argv, "-conforming", 12)) conf = 1;
		else if (!memcmp(*argv, "-I", 3) && argc > 1) {
			usepp = 1;   // -I implies -P, exactly as c4lc.lisp does
			e = cons(mk_string(argv[1]), 0);
			if (ptail) { ptail[CELL_B] = (int)e; ptail = e; } else paths = ptail = e;
			--argc; ++argv;
		}
		else if (!memcmp(*argv, "-D", 3) && argc > 1) {
			usepp = 1;   // and so does -D
			e = cons(mk_string(argv[1]), 0);
			if (dtail) { dtail[CELL_B] = (int)e; dtail = e; } else predefs = dtail = e;
			--argc; ++argv;
		}
		else { printf("c4sc-host: unknown flag %s\n", *argv); return 1; }
		--argc; ++argv;
	}
	if (argc < 2) { printf("usage: c4sc-host compile [flags] in.c out.c4r\n"); return 1; }
	in = *argv; outname = argv[1];

	sc_init_lex();
	sc_init_pp();
	sc_init_parse();
	sc_init_c4r();
	sc_init_tree();
	sc_init_gen();
	sc_init_opt();

	L_lex_58conforming = conf ? cell_true : cell_false;
	L_gen_58objmode = obj ? cell_true : cell_false;
	L_gen_58cisc = cisc ? cell_true : cell_false;

	if (usepp) {
		L_pp_58paths = paths;
		L_pp_58predefines(predefs);
		if (c4sp_err) { printf("c4sc-host: %s\n", c4sp_err_msg); return 1; }
		ast = L_pp_58file(mk_string(in));
	} else {
		ast = L_lex_58file(mk_string(in));
	}
	if (c4sp_err) { printf("c4sc-host: %s\n", c4sp_err_msg); return 1; }
	ast = L_parse_58program(ast);
	if (c4sp_err) { printf("c4sc-host: %s\n", c4sp_err_msg); return 1; }

	if (opt) {
		L_tree_58objmode = obj ? cell_true : cell_false;
		ast = L_tree_58optimize(ast);
		if (c4sp_err) { printf("c4sc-host: %s\n", c4sp_err_msg); return 1; }
	}
	m = L_gen_58module(ast);
	if (c4sp_err) { printf("c4sc-host: %s\n", c4sp_err_msg); return 1; }
	if (opt) {
		L_opt_58fuse_45on = fuse ? cell_true : cell_false;
		m = L_c4opt_58optimize(m);
		if (c4sp_err) { printf("c4sc-host: %s\n", c4sp_err_msg); return 1; }
	}

	// v3 for both objects and whole-program images: uninitialized
	// globals are segregated to BSS, exactly as c4lc.lisp does it.
	L_c4r_58v3 = cell_true;
	L_c4r_58bss_45extra = L_gen_58bssextra;
	out = L_c4r_58encode(m);
	L_c4r_58bss_45extra = mk_int(0);
	if (c4sp_err) { printf("c4sc-host: %s\n", c4sp_err_msg); return 1; }

	r = sc_call_named("file:write", cons(mk_string(outname), cons(out, 0)));
	if (c4sp_err || cell_is_false(r)) { printf("c4sc-host: write failed\n"); return 1; }
	pr_reset(); cell_write(out, 0);
	printf(";; c4lc: %s - %d bytes - %s\n", in, out[CELL_B], outname);
	return 0;
}

int main (int argc, char **argv) {
	int *orig, *m, *m2, *out, *r;
	char *in, *outname;
	int   fuse, tokens, pp, ast, pptok, comp, cells;

	gc_stack_base = (int *)&orig;
	fuse = 0;
	tokens = pp = ast = pptok = comp = 0;
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
	else if (argc > 0 && !memcmp(*argv, "compile", 8)) { comp = 1; --argc; ++argv; }
	else if (argc > 0 && !memcmp(*argv, "pptok", 6)) { pptok = 1; --argc; ++argv; }
	else if (argc > 0 && !memcmp(*argv, "pp", 3)) { pp = 1; --argc; ++argv; }
	else if (argc > 0 && !memcmp(*argv, "ast", 4)) { ast = 1; --argc; ++argv; }
	else if (argc > 0 && !memcmp(*argv, "opt", 4)) { --argc; ++argv; }
	if (argc > 0 && !memcmp(*argv, "-mfuse", 7)) { fuse = 1; --argc; ++argv; }
	if (!tokens && !pp && !ast && !pptok && !comp && argc < 2) { printf("usage: c4sc-host [opt] [-mfuse] in.c4r out.c4r\n"); return 1; }
	in = *argv; outname = (tokens || pp || ast || pptok || comp) ? 0 : argv[1];

	if (atoms_init()) return 1;
	if (gc_init(cells)) return 1;
	if (pr_init()) return 1;
	sc_genv = mk_env(0);
	gc_root_genv = sc_genv;
	stdlib_init(sc_genv);

	if (tokens) return sc_tokens(argc, argv);
	if (pp) return sc_pp(argc, argv);
	if (pptok) return sc_pptok(argc, argv);
	if (comp) return sc_compile(argc, argv);
	if (ast) return sc_ast(argc, argv);

	sc_init_c4r();
	sc_init_opt();

	// file:read and file:write stay in the interpreter: they are where
	// c4sp resolves a path and where it chooses between the host and
	// C4KE's ramfs, and duplicating that would be duplicating a policy.
	orig = sc_call_named("file:path", cons(mk_string(in), 0));
	orig = sc_call_named("file:read", cons(orig, 0));
	if (c4sp_err || !orig) { printf("c4sc-host: cannot read %s\n", in); return 1; }
	m = L_c4r_58decode(orig);
	if (c4sp_err) { printf("c4sc-host: %s\n", c4sp_err_msg); return 1; }

	L_opt_58fuse_45on = fuse ? cell_true : cell_false;
	m2 = L_c4opt_58optimize(m);
	if (c4sp_err) { printf("c4sc-host: %s\n", c4sp_err_msg); return 1; }

	out = L_c4r_58encode(m2);
	if (c4sp_err) { printf("c4sc-host: %s\n", c4sp_err_msg); return 1; }
	printf(";; c4opt: %s - %d -> %d bytes\n", in, orig[CELL_B], out[CELL_B]);
	r = sc_call_named("file:write", cons(mk_string(outname), cons(out, 0)));
	if (c4sp_err || cell_is_false(r)) { printf("c4sc-host: write failed\n"); return 1; }
	return 0;
}
