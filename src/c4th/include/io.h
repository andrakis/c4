// c4th: input sources, and character output.
//
// Forth-2012's input model is a LINE at a time: SOURCE hands back the
// current line, >IN indexes into it, and REFILL fetches the next one. A
// source is therefore a whole text plus a cursor for the next line, and
// sources form a stack so that INCLUDED and EVALUATE can nest -- which
// they must, because B3's test run is core.f, then tester.fs, then
// core.fr, each pulling in the next.
//
// Files are read whole rather than streamed. The VM has no seek and its
// read is a syscall per call; slurping once keeps REFILL to pointer
// arithmetic, and the largest thing c4th will read is a few hundred KB.

enum { SRC_TEXT,     // char * to the whole text
       SRC_LEN,
       SRC_NEXT,     // offset of the next line
       SRC_LINE,     // char * to the current line
       SRC_LLEN,     // its length, newline excluded
       SRC_IN,       // >IN within the current line
       SRC_OWNED,    // 1 if TEXT should be freed when the source is popped
       SRC__Sz };

enum { TH_SRC_MAX = 16 };

int *th_srcs;        // TH_SRC_MAX * SRC__Sz
int  th_srcd;        // number of sources on the stack

int *th_src () { return th_srcs + (th_srcd - 1) * SRC__Sz; }

int th_src_push (char *text, int len, int owned) {
	int *s;

	if (th_srcd >= TH_SRC_MAX) { printf("c4th: input sources nested too deeply\n"); th_err = 1; return 0; }
	s = th_srcs + th_srcd * SRC__Sz;
	s[SRC_TEXT]  = (int)text;
	s[SRC_LEN]   = len;
	s[SRC_NEXT]  = 0;
	s[SRC_LINE]  = (int)text;
	s[SRC_LLEN]  = 0;
	s[SRC_IN]    = 0;
	s[SRC_OWNED] = owned;
	++th_srcd;
	return 1;
}

void th_src_pop () {
	int *s;

	if (th_srcd <= 0) return;
	s = th_src();
	if (s[SRC_OWNED] && s[SRC_TEXT]) free((void *)s[SRC_TEXT]);
	--th_srcd;
}

// Advance to the next line. Returns 0 at end of text, which is what REFILL
// reports as false.
int th_refill () {
	int  *s;
	char *t;
	int   i, n, start;

	if (th_srcd <= 0) return 0;
	s = th_src();
	if (s[SRC_NEXT] >= s[SRC_LEN]) return 0;
	t     = (char *)s[SRC_TEXT];
	n     = s[SRC_LEN];
	start = s[SRC_NEXT];
	i     = start;
	while (i < n && t[i] != '\n') ++i;
	s[SRC_LINE] = (int)(t + start);
	s[SRC_LLEN] = i - start;
	s[SRC_IN]   = 0;
	s[SRC_NEXT] = i + 1;
	return 1;
}

// Read a whole file. Returns 0 and leaves th_err alone if it cannot be
// opened, so the caller can report the name.
char *th_slurp (char *path, int *lenout) {
	int   fd, n, cap, got;
	char *buf;

	if ((fd = open(path, 0)) < 0) return 0;
	cap = 65536;
	if (!(buf = malloc(cap))) { close(fd); return 0; }
	n = 0;
	while (1) {
		if (n == cap) {
			cap = cap + cap;
			if (!(buf = realloc(buf, cap))) { close(fd); return 0; }
		}
		got = read(fd, buf + n, cap - n);
		if (got <= 0) break;
		n = n + got;
	}
	close(fd);
	*lenout = n;
	return buf;
}

void th_emit (int c) { putchar(c); }

void th_type (char *p, int n) {
	int i;
	i = 0;
	while (i < n) { putchar(p[i]); ++i; }
}
