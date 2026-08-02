// c4sp -- a Lisp interpreter in C4, modelled on alisp
//
// See docs/c4sp-design.md. Runs natively (gcc), under c4m, and -- compiled
// by c4cc to a .c4r -- under plain c4 and C4KE.
//
// Build:
//   native:  gcc -O2 -Iinclude -I. -o c4sp src/c4sp/c4sp.c
//   c4r:     gcc -E -P -Iinclude -I. -D__c4cc__=1 src/c4sp/c4sp.c \
//              | ./c4cc -o c4sp.c4r -
//   run:     ./c4m load-c4r.c -- c4sp.c4r [-p] file.lisp [args...]
//
// Usage:
//   c4sp [-p] [-c ncells] file.lisp [args...]
//     -p         parse only: print the canonical written form
//     -c ncells  set the cell arena size (default 65536)

#include "c4.h"

#include "src/c4sp/include/cell.h"
#include "src/c4sp/include/gc.h"
#include "src/c4sp/include/cells.h"
#include "src/c4sp/include/atoms.h"
#include "src/c4sp/include/read.h"
#include "src/c4sp/include/stdlib.h"
#include "src/c4sp/include/eval.h"

void c4sp_usage () {
	printf("usage: c4sp [-p] [-c ncells] file.lisp [args...]\n");
}

int main (int argc, char **argv) {
	int   opt_parse, opt_cells, endopts;
	char *file, *src;
	int   srclen, i;
	int  *x, *genv, *head, *tail, *e;
	int   stack_base_marker;

	// The address of a local in main is the upper bound for the
	// conservative stack scan (M2).
	gc_stack_base = &stack_base_marker;

	opt_parse = 0;
	opt_cells = 65536;
	endopts = 0;
	file = 0;

	--argc; ++argv;
	while (argc > 0 && !endopts) {
		if (**argv == '-' && (*argv)[1]) {
			if ((*argv)[1] == 'p') opt_parse = 1;
			else if ((*argv)[1] == 'c') {
				--argc; ++argv;
				if (!argc) { c4sp_usage(); return 1; }
				opt_cells = 0;
				src = *argv;
				while (*src >= '0' && *src <= '9')
					opt_cells = opt_cells * 10 + (*src++ - '0');
				if (opt_cells < 256) { printf("c4sp: -c needs at least 256 cells\n"); return 1; }
			}
			else { c4sp_usage(); return 1; }
		} else {
			file = *argv;
			endopts = 1; // everything after the file belongs to the script
		}
		--argc; ++argv;
	}
	if (!file) { c4sp_usage(); return 1; }

	if (atoms_init()) return 1;
	if (gc_init(opt_cells)) return 1;
	if (pr_init()) return 1;

	if (!(src = rd_file(file, &srclen))) {
		printf("c4sp: unable to open '%s'\n", file);
		return 1;
	}

	x = rd_read(src, srclen);
	if (c4sp_err) {
		printf("c4sp: parse error: %s\n", c4sp_err_msg);
		return 2;
	}

	if (opt_parse) {
		pr_reset();
		cell_write(x, 1);
		printf("%s\n", pr_term());
		return 0;
	}

	// Set up the global environment and evaluate. Remaining command line
	// arguments are parsed as expressions into argv, as alisp does.
	genv = mk_env(0);
	stdlib_init(genv);
	head = tail = 0;
	while (argc > 0) {
		e = cons(rd_read(*argv, cs_strlen(*argv)), 0);
		if (c4sp_err) {
			printf("c4sp: cannot parse argument '%s': %s\n", *argv, c4sp_err_msg);
			return 1;
		}
		if (tail) { tail[CELL_B] = (int)e; tail = e; }
		else head = tail = e;
		--argc; ++argv;
	}
	env_define(genv, atom_intern("argv", 4), head);

	x = eval(x, genv);
	if (c4sp_err) {
		printf("c4sp: error: %s\n", c4sp_err_msg);
		return 2;
	}
	// alisp prints the final result
	pr_reset();
	cell_write(x, 0);
	printf("%s\n", pr_term());
	return 0;
}
