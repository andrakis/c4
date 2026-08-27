// host.h -- running ONE compiled unit inside the interpreter.
//
// M2's shape (docs/c4sc-design.md): c4opt.lisp is compiled, everything
// else -- c4r.lisp's decoder and encoder, the driver -- stays
// interpreted, and the images that come out must be byte-identical to
// the interpreted optimizer's. Compiling one file at a time is only
// possible if the compiled file can still call the interpreted rest,
// which is what the bridge below is.
//
// The five names c4opt.lisp uses from c4r.lisp (cons, second, third,
// reverse, c4r:max-label) and the one global it reads (W) are resolved
// by LOOKING THEM UP IN THE GLOBAL ENVIRONMENT and applying the closure
// found there. That is slow -- an interpreted call per cons -- and it
// is correct, which is what this rung is for. It stops being slow when
// c4r.lisp is compiled too (M5), at which point these disappear.

int *sc_genv;          // the interpreter's global environment
int  sc_ready;         // sc_init() has run

// Look up a global by name and apply it to an already-built argument
// list, the way eval's T_LAMBDA case does.
int *sc_apply (int *proc, int *args) {
	int *e;
	int  t;

	// A name in the global environment can be either -- c4r.lisp's cons
	// is a lambda, file:path is a builtin -- and the bridge has to take
	// both, because the compiled unit cannot tell them apart either.
	t = cell_type(proc);
	if (t == T_PROC || t == T_PROCENV) return builtin_call(proc[CELL_A], args, sc_genv);
	if (t != T_LAMBDA) { c4sp_error("c4sc bridge: not applicable"); return 0; }
	e = mk_env((int *)proc[CELL_C]);
	env_bind(e, (int *)proc[CELL_A], args);
	return eval((int *)proc[CELL_B], e);
}

int *sc_lookup (char *name) {
	return env_get(sc_genv, atom_intern(name, cs_strlen(name)));
}

int *sc_call_named (char *name, int *args) {
	int *f;

	if (!(f = sc_lookup(name))) { c4sp_error("c4sc bridge: undefined global"); return 0; }
	return sc_apply(f, args);
}

// -- what the generated c4opt unit expects from c4r.lisp ---------------
int *L_cons (int *x, int *l) { return sc_call_named("cons", cons(x, cons(l, 0))); }
int *L_second (int *l) { return sc_call_named("second", cons(l, 0)); }
int *L_third (int *l) { return sc_call_named("third", cons(l, 0)); }
int *L_reverse (int *l) { return sc_call_named("reverse", cons(l, 0)); }
int *L_c4r_58max_45label (int *a, int *b) {
	return sc_call_named("c4r:max-label", cons(a, cons(b, 0)));
}
int *L_W;   // c4r.lisp's word size, copied in before each entry

// -- the entry point, as a builtin -------------------------------------
// The driver calls (c4sc:optimize M). opt:fuse-on is a variable the
// driver sets, and the compiled unit has its own copy of it, so it is
// carried across here rather than being read twice from two places.
int *L_c4opt_58optimize (int *M);
int *L_opt_58fuse_45on;
void sc_init_opt ();

int *sc_bridge_optimize (int *args) {
	int *fuse;

	if (!sc_ready) { sc_init_opt(); sc_ready = 1; }
	L_W = sc_lookup("W");
	fuse = sc_lookup("opt:fuse-on");
	L_opt_58fuse_45on = fuse ? fuse : cell_false;
	return L_c4opt_58optimize(car(args));
}
