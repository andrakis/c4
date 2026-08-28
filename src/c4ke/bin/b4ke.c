// B4KE - Build for the Kernel Experiment.
//
// A build tool for a machine that cannot say the command it needs to
// run. C4DOS keeps fifteen tokens of a command line (ARGVMAX,
// c4dos.c:72); linking C4IX is sixteen. Response files close that gap
// for one command (asm-c4r.c), but a twelve-module build is not one
// command -- it is compile, compile, ... , write the object list, link,
// in an order, with somewhere to stop when a step fails. That is what
// this is, and it is deliberately the SMALLEST thing that is.
//
// It runs under C4KE and only under C4KE, because C4KE is what gives it
// the two things it needs: tasks it can start and wait for
// (kern_user_start_c4r / await_pid) and a filesystem it can write to
// and read back (OP_VFS_*). C4DOS has neither, keeps BUILD.BAT, and is
// the rung below. So B4KE is part of the answer to "what did building
// the kernel buy me".
//
//   b4ke [-f FILE] [-n] [-k] [-t]
//     -f FILE   build file (default BUILD.B4K)
//     -t        run `top` alongside, so there is something to watch
//     -n        say what would run, run nothing
//     -k        keep going after a failed step
//
// The format is five verbs, one per line, dull on purpose in the way
// C4TAR1 is dull:
//
//   B4KE1                     magic, first line
//   # ...                     comment
//   ECHO text                 progress, for a person watching
//   LIST name word word ...   write those words to `name` as a file,
//                             one per line -- an object list a later
//                             RUN can pass as @name
//   TARGET name               the file the NEXT run must produce; if it
//                             already exists the run is skipped
//   RUN prog arg arg ...      start prog, wait for it
//
// WHY PRESENCE AND NOT TIMESTAMPS. make decides by comparing
// modification times. Neither filesystem here has any: C4DOS's RAM disk
// keeps name, data, length and capacity per slot and nothing else
// (c4dos.c:102-108), and C4KE's ramfs is the same shape. So the rule is
// "if the target is already there, skip it", which is enough to resume
// an interrupted build and honest about what the machine can actually
// know. Deleting a target is how you force a rebuild -- which is why
// `rm` exists on every system that ever worked this way.

#include <stdio.h>

enum { B4_LINE = 1024, B4_ARGS = 256, B4_LIST = 65536 };

static char *b4_file;        // build file name
static int   b4_dry;         // -n
static int   b4_top;         // -t: pid of the top we started, or 0
static char *b4_topargv[4];
static int   b4_keep;        // -k
static int   b4_line;        // line number, for messages
static int   b4_ran;         // steps actually run
static int   b4_skipped;     // steps skipped because the target existed
static int   b4_failed;      // steps that did not produce their target
static char *b4_target;      // pending TARGET, or 0

static int b4_isspace (int c) { return c == ' ' || c == 9 || c == 13; }

static int b4_streq (char *a, char *b) {
	while (*a && *a == *b) { ++a; ++b; }
	return *a == *b;
}

// Split a line into words in place. Returns the count; argv[n] is 0.
static int b4_words (char *p, char **argv, int max) {
	int n;
	n = 0;
	while (*p && n < max - 1) {
		while (*p && b4_isspace(*p)) ++p;
		if (!*p) break;
		argv[n++] = p;
		while (*p && !b4_isspace(*p)) ++p;
		if (*p) { *p = 0; ++p; }
	}
	argv[n] = 0;
	return n;
}

// The build file, wherever it is: the kernel's ramfs first (a build
// file another tool wrote exists only there), then the host.
static char *b4_slurp (char *name, int *plen) {
	char *buf, *mem;
	int   fd, n;

	if ((mem = vfs_get(name, &n)) && n > 0) {
		if (!(buf = malloc(n + 1))) return 0;
		memcpy(buf, mem, n);
		buf[n] = 0;
		*plen = n;
		return buf;
	}
	if ((fd = open(name, 0)) < 0) return 0;
	if (!(buf = malloc(B4_LIST + 1))) { close(fd); return 0; }
	if ((n = read(fd, buf, B4_LIST)) < 0) n = 0;
	close(fd);
	buf[n] = 0;
	*plen = n;
	return buf;
}

static int b4_exists (char *name) {
	int n;
	n = 0;
	return vfs_get(name, &n) && n > 0;
}

// LIST: one word per line, which is what respfile_expand reads back.
static int b4_write_list (char **argv, int argc) {
	char *buf, *p, *w;
	int   i;

	if (b4_dry) {
		printf("b4ke: would write %s with %d name(s)\n", argv[1], argc - 2);
		return 1;
	}
	if (!(buf = malloc(B4_LIST))) { printf("b4ke: out of memory\n"); return 0; }
	p = buf;
	i = 2;
	while (i < argc) {
		w = argv[i];
		while (*w) *p++ = *w++;
		*p++ = 10;
		++i;
	}
	*p = 0;
	// vfs_put answers ZERO for success -- the kernel opcode reports a
	// problem, it does not report a length (asm-c4r.c:801).
	if ((i = vfs_put(argv[1], buf, p - buf))) {
		printf("b4ke: %d: could not write list '%s' (error %d)\n", b4_line, argv[1], i);
		free(buf);
		return 0;
	}
	free(buf);
	return 1;
}

static int b4_run (char **argv, int argc) {
	int task, i;

	if (b4_target && b4_exists(b4_target)) {
		printf("b4ke: %s is already here, skipping\n", b4_target);
		++b4_skipped;
		b4_target = 0;
		return 1;
	}
	if (b4_dry) {
		printf("b4ke: would run:");
		i = 1;
		while (i < argc) printf(" %s", argv[i++]);
		printf("\n");
		b4_target = 0;
		return 1;
	}
	// argv[1] is the program; the kernel wants it as argv[0] of the child.
	task = kern_user_start_c4r(argc - 1, argv + 1, argv[1], PRIV_USER);
	if (task <= 0) {
		printf("b4ke: %d: could not start '%s'\n", b4_line, argv[1]);
		++b4_failed;
		b4_target = 0;
		return 0;
	}
	await_pid(task);
	++b4_ran;
	// A step is judged by what it produced, because that is the only
	// thing this machine can check afterwards.
	if (b4_target && !b4_exists(b4_target)) {
		printf("b4ke: %d: '%s' did not produce %s\n", b4_line, argv[1], b4_target);
		++b4_failed;
		b4_target = 0;
		return 0;
	}
	b4_target = 0;
	return 1;
}

int main (int argc, char **argv) {
	char  *text, *p, *eol;
	char **words;
	int    len, n, i, ok;

	b4_file    = "BUILD.B4K";
	b4_dry     = 0;
	b4_keep    = 0;
	b4_line    = 0;
	b4_ran     = 0;
	b4_skipped = 0;
	b4_failed  = 0;
	b4_target  = 0;

	i = 1;
	while (i < argc) {
		if (b4_streq(argv[i], "-f") && i + 1 < argc) { ++i; b4_file = argv[i]; }
		else if (b4_streq(argv[i], "-n")) b4_dry = 1;
		else if (b4_streq(argv[i], "-k")) b4_keep = 1;
		else if (b4_streq(argv[i], "-t")) b4_top = 1;
		else { printf("usage: b4ke [-f FILE] [-n] [-k] [-t]\n"); return 1; }
		++i;
	}

	// Twelve compiles is five minutes of a blank screen. innerbench
	// solved this years ago by running `top` in the background, and a
	// build is exactly the same problem: the interesting thing is not
	// the output, it is that something is happening at all -- which
	// task, how much memory, how far the machine has got.
	if (b4_top) {
		b4_topargv[0] = "top";
		b4_topargv[1] = "-b";
		if (!(b4_top = kern_user_start_c4r(2, b4_topargv, "top", PRIV_USER)))
			printf("b4ke: could not start top; carrying on without it\n");
	}

	if (!(text = b4_slurp(b4_file, &len))) {
		printf("b4ke: cannot read '%s'\n", b4_file);
		return 1;
	}
	if (!(words = malloc(B4_ARGS * sizeof(char *)))) {
		printf("b4ke: out of memory\n");
		return 1;
	}

	p = text;
	ok = 1;
	while (*p && (ok || b4_keep)) {
		eol = p;
		while (*eol && *eol != 10) ++eol;
		if (*eol) { *eol = 0; ++eol; }
		++b4_line;

		n = b4_words(p, words, B4_ARGS);
		p = eol;
		if (!n || *words[0] == '#') continue;

		if (b4_line == 1) {
			if (!b4_streq(words[0], "B4KE1")) {
				printf("b4ke: '%s' does not begin with B4KE1\n", b4_file);
				return 1;
			}
			continue;
		}
		if (b4_streq(words[0], "ECHO")) {
			i = 1;
			while (i < n) {
				printf("%s", words[i]);
				++i;
				if (i < n) printf(" ");
			}
			printf("\n");
		}
		else if (b4_streq(words[0], "TARGET")) {
			if (n < 2) { printf("b4ke: %d: TARGET needs a name\n", b4_line); return 1; }
			b4_target = words[1];
		}
		else if (b4_streq(words[0], "LIST")) {
			if (n < 2) { printf("b4ke: %d: LIST needs a name\n", b4_line); return 1; }
			if (!b4_write_list(words, n)) { ++b4_failed; ok = 0; }
		}
		else if (b4_streq(words[0], "RUN")) {
			if (n < 2) { printf("b4ke: %d: RUN needs a program\n", b4_line); return 1; }
			if (!b4_run(words, n)) ok = 0;
		}
		else {
			printf("b4ke: %d: unknown verb '%s'\n", b4_line, words[0]);
			return 1;
		}
	}

	// Stop watching before the summary, so the last thing on the screen
	// is the result and not a refresh over the top of it.
	if (b4_top) {
		kill(b4_top, SIGTERM);
		sleep(200);
	}
	printf("b4ke: %d ran, %d skipped, %d failed\n", b4_ran, b4_skipped, b4_failed);
	return b4_failed ? 1 : 0;
}
