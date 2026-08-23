// c4th -- a Forth for C4. See docs/c4th-design.md.
//
// Built two ways from this one source, the way src/c4sp/c4sp.c is:
//   native:  make c4th        (gcc -O2; there is no collector to pin it open)
//   c4r:     make c4th.c4r
//   run:     ./c4m load-c4r.c -- c4th.c4r [args]
//
// Usage:
//   c4th [-selftest] [-e text] [-i cells] [-d cells] [-r cells] [file.f ...]
//     -selftest   run the B1 hand-threaded checks and exit
//     -e text     interpret text, then continue with any files
//     -i cells    image size   (default 262144 cells)
//     -d cells    data stack   (default 1024)
//     -r cells    return stack (default 256)
//   Files are interpreted in order. With neither -e nor a file, c4th reads
//   standard input.

#include "c4.h"
#include "c4m.h"

#include "src/c4th/include/mem.h"
#include "src/c4th/include/dict.h"
#include "src/c4th/include/io.h"
#include "src/c4th/include/inner.h"
#include "src/c4th/include/prim.h"
#include "src/c4th/include/outer.h"

// A hand-threaded 10! -- there is no outer interpreter until B2, so B1
// proves the engine by building a body cell by cell and running it.
//
// The Forth this encodes, with the stack as ( i acc ):
//
//   10 1
//   BEGIN  OVER WHILE          \ while i is non-zero
//          OVER *              \ acc = acc * i
//          SWAP 1 - SWAP       \ i = i - 1
//   REPEAT
//   SWAP DROP .                \ drop i, print acc
//
// Every cell is an execution token except the operands that BRANCH,
// 0BRANCH and LIT read from the body, which is why the three of them step
// th_ip past the following cell themselves.
int th_selftest () {
	int *body;
	int *loop;
	int *fixup_end;
	int *xLIT, *xDUP, *xDROP, *xSWAP, *xOVER, *xMUL, *xSUB;
	int *xBRANCH, *xZBRANCH, *xDOT, *xCR, *xBYE;

	xLIT     = th_findz("LIT");
	xDUP     = th_findz("DUP");
	xDROP    = th_findz("DROP");
	xSWAP    = th_findz("SWAP");
	xOVER    = th_findz("OVER");
	xMUL     = th_findz("*");
	xSUB     = th_findz("-");
	xBRANCH  = th_findz("BRANCH");
	xZBRANCH = th_findz("0BRANCH");
	xDOT     = th_findz(".");
	xCR      = th_findz("CR");
	xBYE     = th_findz("BYE");
	if (!xLIT || !xDUP || !xDROP || !xSWAP || !xOVER || !xMUL || !xSUB ||
	    !xBRANCH || !xZBRANCH || !xDOT || !xCR || !xBYE) {
		printf("c4th: selftest: a primitive is missing from the dictionary\n");
		return 1;
	}

	body = th_here;
	th_comma((int)xLIT); th_comma(10);
	th_comma((int)xLIT); th_comma(1);
	loop = th_here;
	th_comma((int)xOVER);
	th_comma((int)xZBRANCH); fixup_end = th_here; th_comma(0);
	th_comma((int)xOVER);
	th_comma((int)xMUL);
	th_comma((int)xSWAP);
	th_comma((int)xLIT); th_comma(1);
	th_comma((int)xSUB);
	th_comma((int)xSWAP);
	th_comma((int)xBRANCH); th_comma((int)loop);
	*fixup_end = (int)th_here;
	th_comma((int)xSWAP);
	th_comma((int)xDROP);
	th_comma((int)xDOT);
	th_comma((int)xCR);
	th_comma((int)xBYE);
	if (th_err) return 1;

	th_ip = body;
	th_run();
	if (th_err) return 1;

	// The engine is only proved if the stack came back empty: a body that
	// leaves junk behind would still print the right number.
	if (th_sp != th_dstack) {
		printf("c4th: selftest: data stack not empty (%d cells)\n",
		       (int)(th_sp - th_dstack));
		return 1;
	}
	if (th_rp != th_rstack) {
		printf("c4th: selftest: return stack not empty\n");
		return 1;
	}
	printf("c4th: selftest ok\n");
	return 0;
}

// strcmp() and atoi() are not C4 builtins -- the VM's list is open, read,
// close, printf, malloc, free, memset, memcmp, exit plus c4m's putchar,
// puts, realloc, memcpy and stacktrace -- so flag parsing brings its own.
int th_eqz (char *a, char *b) {
	while (*a && *b) {
		if (*a != *b) return 0;
		++a; ++b;
	}
	return *a == *b;
}

int th_atoi (char *s) {
	int n, neg;

	n = 0; neg = 0;
	if (*s == '-') { neg = 1; ++s; }
	while (*s >= '0' && *s <= '9') { n = n * 10 + (*s - '0'); ++s; }
	if (neg) return 0 - n;
	return n;
}

// Interpret a whole text, then drop the source. Returns 0 on failure.
int th_run_text (char *text, int len, int owned) {
	int ok;

	if (!th_src_push(text, len, owned)) return 0;
	ok = th_interpret();
	th_src_pop();
	return ok;
}

int th_run_file (char *path) {
	char *text;
	int   len, ok;

	if (!(text = th_slurp(path, &len))) {
		printf("c4th: cannot open '%s'\n", path);
		return 0;
	}
	ok = th_run_text(text, len, 1);
	return ok;
}

void th_usage () {
	printf("usage: c4th [-selftest] [-e text] [-i cells] [-d cells] [-r cells] [file.f ...]\n");
}

int main (int argc, char **argv) {
	int   image, dcells, rcells, selftest, ran, i, n;
	char *a;
	char *evaltext;
	char *stdintext;

	image    = 262144;
	dcells   = 1024;
	rcells   = 256;
	selftest = 0;
	evaltext = 0;
	ran      = 0;

	--argc; ++argv;
	while (argc > 0) {
		a = *argv;
		if (*a != '-') break;
		if      (th_eqz(a, "-selftest")) selftest = 1;
		else if (th_eqz(a, "-i") && argc > 1) { --argc; ++argv; image  = th_atoi(*argv); }
		else if (th_eqz(a, "-d") && argc > 1) { --argc; ++argv; dcells = th_atoi(*argv); }
		else if (th_eqz(a, "-r") && argc > 1) { --argc; ++argv; rcells = th_atoi(*argv); }
		else if (th_eqz(a, "-e") && argc > 1) { --argc; ++argv; evaltext = *argv; }
		else { th_usage(); return 1; }
		--argc; ++argv;
	}

	if (!th_mem_init(image, dcells, rcells)) {
		printf("c4th: could not allocate image or stacks\n");
		return 1;
	}
	if (!(th_srcs = malloc(TH_SRC_MAX * SRC__Sz * sizeof(int)))) return 1;
	if (!(th_tramps = malloc(TH_TRAMP_MAX * 2 * sizeof(int)))) return 1;
	th_srcd   = 0;
	th_latest = 0;
	th_prims_init();
	th_outer_init();
	if (th_err) return 1;

	if (selftest) return th_selftest();

	if (evaltext) {
		n = 0; while (evaltext[n]) ++n;
		if (!th_run_text(evaltext, n, 0)) return 1;
		ran = 1;
	}
	i = 0;
	while (i < argc && !th_quit) {
		if (!th_run_file(argv[i])) return 1;
		ran = 1;
		++i;
	}
	if (!ran && !th_quit) {
		// No -e and no files: read standard input, so c4th composes with
		// a pipe the way every other tool here does.
		if (!(stdintext = th_slurp("/dev/stdin", &n))) {
			printf("c4th: cannot read standard input\n");
			return 1;
		}
		if (!th_run_text(stdintext, n, 1)) return 1;
	}
	return 0;
}
