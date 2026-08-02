// c4sp: reader and printer
//
// A port of alisp's parser (parser.cpp / lib/parser.js): tokens are (, ),
// "strings", ;; comments and generic runs of non-delimiter characters.
// 'x reads as (quote x) -- the corrected behaviour from alisp efdf948.
// String contents are kept raw: alisp performs no escape translation, so
// "a\nb" is three characters a, backslash-n... i.e. four raw bytes.
//
// The printer is the inverse and is also how the optimizer will emit its
// output, so it needs to be exact rather than pretty. Two modes:
//   display (pr_quote = 0): strings print raw, alisp's toString
//   write   (pr_quote = 1): strings print quoted, so output re-reads
//
// Character constants appear as numbers where C4's lexer would mangle the
// escape: only \n survives its single-escape rule (docs/internals.md 1.3).

// -- output buffer --

char *pr_buf;
int   pr_len, pr_cap;

int pr_init () {
	pr_cap = 65536;
	pr_len = 0;
	if (!(pr_buf = malloc(pr_cap))) {
		printf("c4sp: cannot allocate print buffer\n");
		return 1;
	}
	return 0;
}

void pr_reset () { pr_len = 0; }

void pr_ch (int c) {
	char *n;
	int i;
	if (pr_len + 2 > pr_cap) {
		pr_cap = pr_cap * 2;
		if (!(n = malloc(pr_cap))) { c4sp_error("print buffer overflow"); return; }
		i = 0;
		while (i < pr_len) { n[i] = pr_buf[i]; ++i; }
		free(pr_buf);
		pr_buf = n;
	}
	pr_buf[pr_len++] = c;
}

void pr_sn (char *s, int len) {
	int i;
	i = 0;
	while (i < len) pr_ch(s[i++]);
}

void pr_s (char *s) { while (*s) pr_ch(*s++); }

// Nul-terminate without consuming length, so pr_buf works with %s.
char *pr_term () {
	pr_buf[pr_len] = 0;
	return pr_buf;
}

void pr_uint (int v) {
	if (v >= 10) pr_uint(v / 10);
	pr_ch('0' + v % 10);
}

void pr_int (int v) {
	if (v < 0) { pr_ch('-'); pr_uint(-v); }
	else pr_uint(v);
}

// -- printer --

void cell_write (int *x, int quote) {
	int t, first;
	if (!x) { pr_s("nil"); return; }
	t = x[CELL_TYPE];
	if (t == T_ATOM) { pr_s(atom_name(x[CELL_A])); return; }
	if (t == T_INT) { pr_int(x[CELL_A]); return; }
	if (t == T_FLOAT) { pr_s("?float?"); return; } // arrives with M4
	if (t == T_STRING) {
		if (quote) pr_ch('"');
		pr_sn((char *)x[CELL_A], x[CELL_B]);
		if (quote) pr_ch('"');
		return;
	}
	if (t == T_CONS) {
		pr_ch('(');
		first = 1;
		while (cell_type(x) == T_CONS) {
			if (!first) pr_ch(' ');
			first = 0;
			cell_write((int *)x[CELL_A], quote);
			x = (int *)x[CELL_B];
		}
		if (x) { // improper list; nothing in the reader makes one
			pr_s(" . ");
			cell_write(x, quote);
		}
		pr_ch(')');
		return;
	}
	if (t == T_LAMBDA || t == T_MACRO || t == T_FASTMACRO) {
		// alisp prints closures env-first: (lambda (#env 2) (n) (body))
		if (t == T_LAMBDA) pr_s("(lambda ");
		else if (t == T_MACRO) pr_s("(macro ");
		else pr_s("(fastmacro ");
		cell_write((int *)x[CELL_C], quote);
		pr_ch(' ');
		cell_write((int *)x[CELL_A], quote);
		pr_ch(' ');
		cell_write((int *)x[CELL_B], quote);
		pr_ch(')');
		return;
	}
	if (t == T_PROC) { pr_s("#proc"); return; }
	if (t == T_PROCENV) { pr_s("#procenv"); return; }
	if (t == T_ENV) {
		pr_s("(#env ");
		pr_int(x[CELL_C]);
		pr_ch(')');
		return;
	}
	pr_s("?cell?");
}

// -- reader --

char *rd_src;
int   rd_pos, rd_len;

int rd_isspace (int c) { return c == 32 || c == 9 || c == 10 || c == 13 || c == 12 || c == 11; }
int rd_isdigit (int c) { return c >= '0' && c <= '9'; }
int rd_isdelim (int c) { return rd_isspace(c) || c == '(' || c == ')'; }

// Skip whitespace and ;; comments
void rd_skip () {
	int c;
	while (rd_pos < rd_len) {
		c = rd_src[rd_pos];
		if (rd_isspace(c)) ++rd_pos;
		else if (c == ';' && rd_pos + 1 < rd_len && rd_src[rd_pos + 1] == ';') {
			while (rd_pos < rd_len && rd_src[rd_pos] != 10 && rd_src[rd_pos] != 13)
				++rd_pos;
		}
		else return;
	}
}

// Classify and convert a generic token: integer, float, or interned atom.
int *rd_token_cell (char *s, int len) {
	int i, neg, v, isint;
	// Numbers start with a digit, or a minus followed by a digit
	if (rd_isdigit(s[0]) || (len > 1 && s[0] == '-' && rd_isdigit(s[1]))) {
		neg = (s[0] == '-');
		i = neg ? 1 : 0;
		isint = 1;
		v = 0;
		while (i < len) {
			if (!rd_isdigit(s[i])) { isint = 0; i = len; }
			else v = v * 10 + (s[i++] - '0');
		}
		if (isint) return mk_int(neg ? -v : v);
		// Not a plain integer: a float literal ("1.5", "2e3").
		// Floats arrive with M4 (binary32 via c4_float.h).
		c4sp_error("float literals are not supported yet");
		return 0;
	}
	return mk_atom(atom_intern(s, len));
}

int *rd_expr () {
	int c, start;
	int *head, *tail, *e, escape;

	rd_skip();
	if (c4sp_err) return 0;
	if (rd_pos >= rd_len) { c4sp_error("unexpected end of input"); return 0; }
	c = rd_src[rd_pos];

	if (c == '(') {
		++rd_pos;
		head = tail = 0;
		while (1) {
			rd_skip();
			if (rd_pos >= rd_len) { c4sp_error("missing closing )"); return 0; }
			if (rd_src[rd_pos] == ')') { ++rd_pos; return head; }
			e = cons(rd_expr(), 0);
			if (c4sp_err) return 0;
			if (tail) { tail[CELL_B] = (int)e; tail = e; }
			else head = tail = e;
		}
	}
	if (c == ')') { c4sp_error("unexpected )"); return 0; }
	if (c == 39) { // quote character: 'x -> (quote x)
		++rd_pos;
		e = rd_expr();
		if (c4sp_err) return 0;
		return cons(mk_atom(A_QUOTE), cons(e, 0));
	}
	if (c == '"') {
		// alisp's escape scan: a backslash shields the next character from
		// ending the string, and the contents are kept raw.
		start = rd_pos + 1;
		escape = 0;
		while (1) {
			++rd_pos;
			if (escape) --escape;
			if (rd_pos >= rd_len) { c4sp_error("unterminated string"); return 0; }
			if (rd_src[rd_pos] == 92) escape = 2;
			if (!escape && rd_src[rd_pos] == '"') {
				++rd_pos;
				return mk_string_len(rd_src + start, rd_pos - 1 - start);
			}
		}
	}
	// Generic token
	start = rd_pos;
	while (rd_pos < rd_len && !rd_isdelim(rd_src[rd_pos])) ++rd_pos;
	return rd_token_cell(rd_src + start, rd_pos - start);
}

// Parse the first expression in a string.
int *rd_read (char *src, int len) {
	rd_src = src;
	rd_pos = 0;
	rd_len = len;
	return rd_expr();
}

// -- file loading --

enum { C4SP_OPEN_MODE = 0x8000 }; // matches load-c4r.c's FILE_OPEN_MODE

// Read a whole file into a malloc'd buffer; length in *out_len.
char *rd_file (char *path, int *out_len) {
	int fd, cap, len, n, i;
	char *buf, *nbuf;
	if ((fd = open(path, C4SP_OPEN_MODE)) < 0) return 0;
	cap = 65536;
	len = 0;
	if (!(buf = malloc(cap))) { close(fd); return 0; }
	while ((n = read(fd, buf + len, cap - len - 1)) > 0) {
		len = len + n;
		if (len + 1 >= cap) {
			cap = cap * 2;
			if (!(nbuf = malloc(cap))) { free(buf); close(fd); return 0; }
			i = 0;
			while (i < len) { nbuf[i] = buf[i]; ++i; }
			free(buf);
			buf = nbuf;
		}
	}
	close(fd);
	buf[len] = 0;
	*out_len = len;
	return buf;
}
