// c4sc-self.c -- c4sc, compiled by c4sc.
//
// Its own binary rather than another unit of c4sc-host, for one
// concrete reason: c4sc.lisp defines `second` and `third`, and so does
// c4r.lisp. Two units that both define a name cannot share a link, and
// the mangling is deliberately name-based rather than unit-based so
// that a call from one unit to another resolves at all. A compiler is
// its own program anyway.
//
//   c4sc-self in.lisp out.c [unitname]
//
// The fixed point (M7): gen1 is c4sc.lisp compiled by the INTERPRETED
// c4sc, gen2 is c4sc.lisp compiled by gen1, gen3 by gen2. All three
// have to be the same bytes -- the first equality is the interesting
// one, because it says the compiler running on c4sp's evaluator and the
// same compiler running as compiled C emit the same text.
#include "c4.h"
#include "c4m.h"
#include "c4_float.h"
#include "src/c4sp/include/cell.h"
#include "src/c4sp/include/gc.h"
#include "src/c4sp/include/cells.h"
#include "src/c4sp/include/atoms.h"
#include "src/c4sp/include/read.h"
#include "src/c4sp/include/stdlib.h"
#include "src/c4sc/scrt.h"

int *L_argv;              // c4sc.lisp reads its arguments from here
void sc_init_c4sc ();     // the generated unit's top level IS the program

int main (int argc, char **argv) {
	int *genv, *l, *t, *e;

	gc_stack_base = (int *)&genv;
	if (atoms_init()) return 1;
	if (gc_init(8000000)) return 1;
	if (pr_init()) return 1;
	genv = mk_env(0);
	gc_root_genv = genv;
	stdlib_init(genv);

	// argv as a Lisp list of strings. The interpreter hands c4sc atoms
	// and c4sc stringifies them with (+ "" x); strings survive that
	// unchanged, so the same source works either way.
	l = 0; t = 0;
	--argc; ++argv;
	while (argc > 0) {
		e = cons(mk_string(*argv), 0);
		if (t) { t[CELL_B] = (int)e; t = e; } else { l = e; t = e; }
		--argc; ++argv;
	}
	gc_add_root((int *)&L_argv);
	L_argv = l;

	sc_init_c4sc();
	if (c4sp_err) { printf("c4sc-self: %s\n", c4sp_err_msg); return 1; }
	return 0;
}

#include "src/c4sc/c4self_gen.c"
