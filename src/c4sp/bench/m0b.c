#include "c4.h"
#include "c4m.h"
#include "c4_float.h"
#include "src/c4sp/include/cell.h"
#include "src/c4sp/include/gc.h"
#include "src/c4sp/include/cells.h"
#include "src/c4sp/include/atoms.h"
#include "src/c4sp/include/read.h"
#include "src/c4sp/include/stdlib.h"

// (define fib (lambda (n) (if (<= n 1) n (+ (fib (- n 1)) (fib (- n 2))))))
int *m0_fib (int *n) {
	int *a, *b;
	if (n[CELL_A] <= 1) return n;
	a = m0_fib(mk_int(n[CELL_A] - 1));
	b = m0_fib(mk_int(n[CELL_A] - 2));
	return mk_int(a[CELL_A] + b[CELL_A]);
}

int main (int argc, char **argv) {
	int *genv, *r;
	gc_stack_base = (int *)&genv;
	if (atoms_init()) return 1;
	if (gc_init(4000000)) return 1;
	if (pr_init()) return 1;
	genv = mk_env(0);
	gc_root_genv = genv;
	stdlib_init(genv);
	r = m0_fib(mk_int(30));
	printf("%d\n", r[CELL_A]);
	return 0;
}
