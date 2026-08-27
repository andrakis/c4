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

// c4r.lisp used to be reached through this bridge -- cons, second,
// third, reverse, c4r:max-label and W were interpreted, and the compiled
// c4opt called into the evaluator for every one. From M5 c4r.lisp is
// compiled too, so those definitions are gone and the calls are direct.
// What is left of the bridge is what the HOST still needs: applying a
// name from the global environment, which is how file:read and
// file:write are reached without duplicating c4sp's path handling.

// What the driver calls in the generated units. Declaring them here
// serves both builds: in the native one the generated file's definition
// follows and agrees, and in the object one c4lc -c turns each into an
// extern symbol for c4rlink.
extern int *L_lex_58conforming;
extern int *L_lex_58pp;
int *L_lex_58file (int *a0);
extern int *L_pp_58paths;
int *L_pp_58file (int *a0);
int *L_pp_58predefine (int *a0);
int *L_pp_58predefines (int *a0);
int *L_parse_58program (int *a0);
extern int *L_c4r_58v3;
extern int *L_c4r_58bss_45extra;
int *L_c4r_58decode (int *a0);
int *L_c4r_58encode (int *a0);
extern int *L_tree_58objmode;
int *L_tree_58optimize (int *a0);
extern int *L_gen_58objmode;
extern int *L_gen_58cisc;
extern int *L_gen_58bssextra;
int *L_gen_58module (int *a0);
extern int *L_opt_58fuse_45on;
int *L_c4opt_58optimize (int *a0);

// The compiled units' initialisers.
void sc_init_opt ();
void sc_init_lex ();
void sc_init_pp ();
void sc_init_parse ();
void sc_init_c4r ();
void sc_init_tree ();
void sc_init_gen ();
