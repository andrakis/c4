// c4sp -- a Lisp interpreter in C4, modelled on alisp
//
// See docs/c4sp-design.md. Runs natively (gcc), under c4m, and -- compiled
// by c4cc to a .c4r -- under plain c4 and C4KE.
//
// Build:
//   native:  make c4sp        (gcc -O0: the collector scans the stack)
//   c4r:     make c4sp.c4r
//   run:     ./c4m load-c4r.c -- c4sp.c4r file.lisp [args...]
//   C4KE:    ./c4m load-c4r.c -- c4ke.c4r c4sp.c4r file.lisp
//
// Usage:
//   c4sp [-p] [-R] [-i] [-c ncells] [file.lisp] [args...]
//     -p         parse only: print the canonical written form
//     -R         use the recursive reference evaluator instead of the CEK
//                machine (kept as the oracle the machine is diffed against)
//     -i         interactive REPL (after running file.lisp, when given)
//     -c ncells  first block of the cell arena (default 65536; it grows)

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
#include "src/c4sp/include/cek.h"

void c4sp_usage () {
	printf("usage: c4sp [-p] [-R] [-i] [-c ncells] [file.lisp] [args...]\n");
}

// Evaluate with whichever evaluator was selected, printing any error.
// Returns the result; *ok is cleared on error.
int c4sp_opt_recursive;

int *c4sp_eval_top (int *x, int *genv) {
	if (c4sp_opt_recursive) return eval(x, genv);
	return eval_cek(x, genv);
}

// The interactive loop: one expression per line, errors reported and
// cleared so the session continues.
void c4sp_repl (int *genv) {
	char *line;
	int   pos, r, done, blank, *x;

	if (!(line = malloc(4096))) return;
	while (1) {
		printf("c4sp> ");
#if NATIVE
		fflush(stdout);
#endif
		// Read one line, byte at a time (no break in C4: a done flag)
		pos = 0;
		done = 0;
		r = 1;
		while (!done && pos < 4095) {
			r = read(0, line + pos, 1);
			if (r <= 0) done = 1;             // EOF or error
			else if (line[pos] == 10) done = 1; // newline ends the line
			else ++pos;
		}
		if (r <= 0 && pos == 0) { printf("\n"); return; } // EOF
		// Skip blank lines
		blank = 1;
		r = 0;
		while (r < pos) if (!rd_isspace(line[r++])) blank = 0;
		if (blank) continue;
		x = rd_read(line, pos);
		if (c4sp_err) {
			printf("c4sp: parse error: %s\n", c4sp_err_msg);
			c4sp_err = 0;
			continue;
		}
		x = c4sp_eval_top(x, genv);
		if (c4sp_err) {
			printf("c4sp: error: %s\n", c4sp_err_msg);
			c4sp_err = 0;
			continue;
		}
		pr_reset();
		cell_write(x, 0);
		printf("%s\n", pr_term());
	}
}

int main (int argc, char **argv) {
	int   opt_parse, opt_cells, opt_repl, endopts;
	char *file, *src;
	int   srclen;
	int  *x, *genv, *head, *tail, *e;
	int   stack_base_marker;

	// The address of a local in main is the upper bound for the
	// conservative stack scan (M2).
	gc_stack_base = &stack_base_marker;

	opt_parse = 0;
	opt_repl = 0;
	c4sp_opt_recursive = 0;
	opt_cells = 65536;
	endopts = 0;
	file = 0;

	--argc; ++argv;
	while (argc > 0 && !endopts) {
		if (**argv == '-' && (*argv)[1]) {
			if ((*argv)[1] == 'p') opt_parse = 1;
			else if ((*argv)[1] == 'R') c4sp_opt_recursive = 1;
			else if ((*argv)[1] == 'i') opt_repl = 1;
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
	if (!file && !opt_repl) { c4sp_usage(); return 1; }

	if (atoms_init()) return 1;
	if (gc_init(opt_cells)) return 1;
	if (pr_init()) return 1;

	// Global environment first: the REPL needs it even with no file.
	// Remaining command line arguments are parsed as expressions into
	// argv, as alisp does.
	genv = mk_env(0);
	gc_root_genv = genv;
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

	if (file) {
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
		x = c4sp_eval_top(x, genv);
		if (c4sp_err) {
			printf("c4sp: error: %s\n", c4sp_err_msg);
			return 2;
		}
		// alisp prints the final result
		pr_reset();
		cell_write(x, 0);
		printf("%s\n", pr_term());
	}

	if (opt_repl) c4sp_repl(genv);
	return 0;
}
