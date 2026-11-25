// M0: the same kernel as m0.lisp, hand-written as C over c4sp's OWN
// cells and collector -- which is what c4sc would generate. Identical
// allocation (every arithmetic result is a fresh cell, exactly as the
// `+` builtin makes one), so the GC does identical work and the only
// difference measured is the interpretation.
#include "c4.h"
#include "c4m.h"
#include "c4_float.h"
#include "src/c4sp/include/cell.h"
#include "src/c4sp/include/gc.h"
#include "src/c4sp/include/cells.h"
#include "src/c4sp/include/atoms.h"
#include "src/c4sp/include/read.h"
#include "src/c4sp/include/stdlib.h"

// (define cons (lambda (x l) (+ (list x) l)))
int *m0_cons (int *x, int *l) { return cons(x, l); }

// (define build (lambda (n acc) (if (= n 0) acc (next build (- n 1) (cons n acc)))))
int *m0_build (int *n, int *acc) {
	while (1) {
		if (n[CELL_A] == 0) return acc;
		acc = m0_cons(n, acc);
		n = mk_int(n[CELL_A] - 1);
	}
}

// (define scan (lambda (l acc)
//    (if (empty? l) acc
//        (next scan (tail l) (if (= (head l) 7) (+ acc 1) (+ acc 0))))))
int *m0_scan (int *l, int *acc) {
	int *h;
	while (1) {
		if (cell_type(l) != T_CONS) return acc;
		h = (int *)l[CELL_A];
		if (h[CELL_A] == 7) acc = mk_int(acc[CELL_A] + 1);
		else                acc = mk_int(acc[CELL_A] + 0);
		l = (int *)l[CELL_B];
	}
}

// (define runs (lambda (l k acc) (if (= k 0) acc (next runs l (- k 1) (+ acc (scan l 0))))))
int *m0_runs (int *l, int *k, int *acc) {
	int *r;
	while (1) {
		if (k[CELL_A] == 0) return acc;
		r = m0_scan(l, mk_int(0));
		acc = mk_int(acc[CELL_A] + r[CELL_A]);
		k = mk_int(k[CELL_A] - 1);
	}
}

int main (int argc, char **argv) {
	int *genv, *L, *r;

	gc_stack_base = (int *)&genv;
	if (atoms_init()) return 1;
	if (gc_init(4000000)) return 1;
	if (pr_init()) return 1;
	genv = mk_env(0);
	gc_root_genv = genv;
	stdlib_init(genv);

	L = m0_build(mk_int(2000), 0);
	r = m0_runs(L, mk_int(5000), mk_int(0));
	printf("%d\n", r[CELL_A]);
	return 0;
}
