// c4th: the outer (text) interpreter.
//
// Parse a word, look it up, and either execute it or compile it depending
// on STATE -- with IMMEDIATE words always executing, which is what makes
// the control structures of core.f expressible in Forth at B3 rather than
// wired into C here.
//
// Executing a single xt from C needs care in a threaded system: th_run is
// a loop over th_ip, and a colon word only SETS th_ip and returns. So an
// xt is run by building a two-cell body -- the word, then an internal stop
// -- and pointing th_ip at it. Those bodies come from a small stack, so an
// IMMEDIATE word that itself interprets (EVALUATE at B3) nests safely.

int  th_state;        // 0 interpreting, 1 compiling
char *th_wname;       // most recent parsed word
int   th_wlen;

enum { TH_TRAMP_MAX = 32 };
int *th_tramps;
int  th_trampd;
int *th_xstop;        // internal: ends th_run
int *th_xlit;         // LIT, for compiling literals
int *th_xexit;        // EXIT, for ;
int *th_xcomma;       // , for POSTPONE of a non-immediate word
int *th_xpsquote;     // (S") for inline strings
int *th_xtype;        // TYPE, for ."
int *th_xpdoes;       // (DOES>)

void th_p_stop (int *w) { th_ip = 0; }

void th_call (int *xt) {
	int *t;
	int *saved;

	if (th_trampd >= TH_TRAMP_MAX) { printf("c4th: interpreter nested too deeply\n"); th_err = 1; return; }
	t = th_tramps + th_trampd * 2;
	th_trampd = th_trampd + 1;
	saved = th_ip;
	t[0] = (int)xt;
	t[1] = (int)th_xstop;
	th_ip = t;
	th_run();
	th_ip = saved;
	th_trampd = th_trampd - 1;
}

int th_isspace (int c) { return c == ' ' || c == 9 || c == 13; }

// Parse the next space-delimited word from the current line into
// th_wname/th_wlen. Returns 0 when the line holds no more.
int th_word () {
	int  *s;
	char *line;
	int   in, len;

	if (th_srcd <= 0) return 0;
	s    = th_src();
	line = (char *)s[SRC_LINE];
	len  = s[SRC_LLEN];
	in   = s[SRC_IN];
	while (in < len && th_isspace(line[in])) ++in;
	if (in >= len) { s[SRC_IN] = in; return 0; }
	th_wname = line + in;
	while (in < len && !th_isspace(line[in])) ++in;
	th_wlen = (line + in) - th_wname;
	// Consume the delimiter that ended the word, so >IN points at what
	// comes next rather than at the space. WORD and S" both parse from
	// wherever the interpreter left off, and the standard's
	// `CHAR " GS3 GOODBYE"` expects seven characters, not eight.
	if (in < len) ++in;
	s[SRC_IN] = in;
	return 1;
}

// Signed integer in the current BASE. Returns 0 if the text is not a
// number, in which case *out is untouched.
int th_number (char *p, int n, int *out) {
	int i, v, neg, d, c;

	if (n <= 0) return 0;
	i = 0; neg = 0;
	if (p[0] == '-') { if (n == 1) return 0; neg = 1; i = 1; }
	v = 0;
	while (i < n) {
		c = p[i];
		if (c >= '0' && c <= '9') d = c - '0';
		else if (c >= 'A' && c <= 'Z') d = c - 'A' + 10;
		else if (c >= 'a' && c <= 'z') d = c - 'a' + 10;
		else return 0;
		if (d >= th_base) return 0;
		v = v * th_base + d;
		++i;
	}
	if (neg) v = 0 - v;
	*out = v;
	return 1;
}

// -- words that need the compiler's state -----------------------------

void th_p_colon (int *w) {           // : name
	int *xt;

	if (!th_word()) { printf("c4th: : needs a name\n"); th_err = 1; return; }
	// Hidden while compiling, so a mention of the name inside the body
	// still finds the PREVIOUS definition -- that is what makes
	// redefinition-in-terms-of-itself work, and what RECURSE is for.
	xt = th_create(th_wname, th_wlen, FL_HIDDEN, (int)&th_do_colon);
	if (!xt) return;
	th_state = 1;
}

void th_p_semi (int *w) {            // ; (immediate)
	th_comma((int)th_xexit);
	if (th_latest) th_latest[W_FLAGS] = th_latest[W_FLAGS] & ~FL_HIDDEN;
	th_state = 0;
}

void th_p_immediate (int *w) {
	if (th_latest) th_latest[W_FLAGS] = th_latest[W_FLAGS] | FL_IMMEDIATE;
}

void th_p_lbrac (int *w) { th_state = 0; }   // [ (immediate)
void th_p_rbrac (int *w) { th_state = 1; }   // ]

void th_p_tick (int *w) {            // ' name
	int *xt;
	if (!th_word()) { printf("c4th: ' needs a name\n"); th_err = 1; return; }
	if (!(xt = th_find(th_wname, th_wlen))) {
		printf("c4th: %.*s ?\n", th_wlen, th_wname); th_err = 1; return;
	}
	th_push((int)xt);
}

void th_p_literal (int *w) {         // LITERAL (immediate)
	th_comma((int)th_xlit);
	th_comma(th_pop());
}

void th_p_state (int *w) { th_push((int)&th_state); }
void th_p_base (int *w)  { th_push((int)&th_base); }

void th_p_backslash (int *w) {       // \ -- comment to end of line
	int *s;
	s = th_src();
	s[SRC_IN] = s[SRC_LLEN];
}

void th_p_paren (int *w) {           // ( -- comment to the next )
	int  *s;
	char *line;
	int   in, len;

	s    = th_src();
	line = (char *)s[SRC_LINE];
	len  = s[SRC_LLEN];
	in   = s[SRC_IN];
	while (in < len && line[in] != ')') ++in;
	if (in < len) ++in;
	s[SRC_IN] = in;
}

void th_p_words (int *w) {           // WORDS -- what is defined
	int *xt;
	int  n;
	xt = th_latest; n = 0;
	while (xt) {
		printf("%.*s ", xt[W_NLEN], (char *)xt[W_NAME]);
		xt = (int *)xt[W_LINK];
		++n;
	}
	printf("\n(%d words)\n", n);
}


// -- parsing to a delimiter -------------------------------------------

// Collect text up to (and consuming) delim, into th_wname/th_wlen.
int th_parse (int delim) {
	int  *sp;
	char *line;
	int   in, len, start;

	if (th_srcd <= 0) return 0;
	sp   = th_src();
	line = (char *)sp[SRC_LINE];
	len  = sp[SRC_LLEN];
	in   = sp[SRC_IN];
	start = in;
	while (in < len && line[in] != delim) ++in;
	th_wname = line + start;
	th_wlen  = in - start;
	if (in < len) ++in;            // step over the delimiter
	sp[SRC_IN] = in;
	return 1;
}

// -- words that compile other words ------------------------------------

void th_p_bracktick (int *w) {        // ['] name  (immediate)
	int *xt;
	if (!th_word()) { printf("c4th: ['] needs a name\n"); th_err = 1; return; }
	if (!(xt = th_find(th_wname, th_wlen))) { printf("c4th: %.*s ?\n", th_wlen, th_wname); th_err = 1; return; }
	th_comma((int)th_xlit);
	th_comma((int)xt);
}

void th_p_postpone (int *w) {         // POSTPONE name  (immediate)
	int *xt;
	if (!th_word()) { printf("c4th: POSTPONE needs a name\n"); th_err = 1; return; }
	if (!(xt = th_find(th_wname, th_wlen))) { printf("c4th: %.*s ?\n", th_wlen, th_wname); th_err = 1; return; }
	// An immediate word's compilation semantics are "run it later", so it
	// goes in directly. A plain word's are "compile it later", so what
	// goes in is code that will compile it.
	if (xt[W_FLAGS] & FL_IMMEDIATE) th_comma((int)xt);
	else { th_comma((int)th_xlit); th_comma((int)xt); th_comma((int)th_xcomma); }
}

void th_p_recurse (int *w) {          // RECURSE (immediate)
	// The word being defined is hidden, so its own name will not find it;
	// that is deliberate, and this is the way back to it.
	if (!th_latest) { printf("c4th: RECURSE outside a definition\n"); th_err = 1; return; }
	th_comma((int)th_latest);
}

void th_p_char (int *w) {             // CHAR name
	if (!th_word() || th_wlen < 1) { printf("c4th: CHAR needs a name\n"); th_err = 1; return; }
	th_push(th_wname[0]);
}

void th_p_brackchar (int *w) {        // [CHAR] name (immediate)
	if (!th_word() || th_wlen < 1) { printf("c4th: [CHAR] needs a name\n"); th_err = 1; return; }
	th_comma((int)th_xlit);
	th_comma(th_wname[0]);
}

// Lay a counted string into the body: (S"), a length cell, then the bytes
// rounded up to a whole number of cells so the next token stays aligned.
void th_compile_string (char *p, int n) {
	char *dst;
	int   i, cells;

	th_comma((int)th_xpsquote);
	th_comma(n);
	cells = (n + sizeof(int) - 1) / sizeof(int);
	dst = (char *)th_align_here();
	if (th_hp + cells * sizeof(int) > th_hlimit) { printf("c4th: image full\n"); th_err = 1; return; }
	th_hp = th_hp + cells * sizeof(int);
	i = 0;
	while (i < n) { dst[i] = p[i]; ++i; }
	while (i < cells * sizeof(int)) { dst[i] = 0; ++i; }
}

void th_p_squote (int *w) {           // S" text"  (immediate in the standard)
	char *save;
	int   n, i;

	if (!th_parse('"')) return;
	if (th_state) { th_compile_string(th_wname, th_wlen); return; }
	// Interpreting: the standard allows a transient buffer, so copy into
	// the image and hand back that. It is never reclaimed, which is fine
	// for the interpretation-time use the suite makes of it.
	n = th_wlen;
	save = th_alloc_bytes(n + 1);
	if (!save) return;
	i = 0;
	while (i < n) { save[i] = th_wname[i]; ++i; }
	save[n] = 0;
	th_push((int)save);
	th_push(n);
}

void th_p_dotquote (int *w) {         // ." text"  (immediate)
	if (!th_parse('"')) return;
	if (th_state) {
		th_compile_string(th_wname, th_wlen);
		th_comma((int)th_xtype);
	} else th_type(th_wname, th_wlen);
}

void th_p_dotparen (int *w) {         // .( ccc)  -- print now, immediate
	if (!th_parse(')')) return;
	th_type(th_wname, th_wlen);
}

void th_p_create (int *w) {           // CREATE name
	if (!th_word()) { printf("c4th: CREATE needs a name\n"); th_err = 1; return; }
	th_defword_n(th_wname, th_wlen, 0, (int)&th_do_var);
}

void th_p_pdoes (int *w) {            // (DOES>) -- runs inside the definer
	if (!th_latest) { printf("c4th: DOES> with nothing defined\n"); th_err = 1; return; }
	th_latest[W_CODE] = (int)&th_do_does;
	th_latest[W_DOES] = (int)th_ip;
	th_ip = (int *)th_rpop();          // and return from the defining word
}

void th_p_does (int *w) {             // DOES> (immediate)
	th_comma((int)th_xpdoes);
}

void th_p_tobody (int *w) { th_push(th_pop() + W__Sz * sizeof(int)); }

// LATEST is how a tool word reaches the definition just compiled -- the
// peephole optimizer's way in.
void th_p_latest (int *w) { th_push((int)th_latest); }


// WORD returns a COUNTED string -- length byte first -- in a transient
// buffer, and FIND consumes one. They are the old-style pair; the modern
// PARSE-NAME returns address and length instead, but the suite exercises
// these.
enum { TH_WORDBUF = 260 };
char *th_wordbuf;

void th_p_wordw (int *w) {           // WORD ( char -- c-addr )
	int  *sp;
	char *line;
	int   d, in, len, n, i;

	d = th_pop();
	sp   = th_src();
	line = (char *)sp[SRC_LINE];
	len  = sp[SRC_LLEN];
	in   = sp[SRC_IN];
	while (in < len && line[in] == d) ++in;      // skip leading delimiters
	i = in;
	while (in < len && line[in] != d) ++in;
	n = in - i;
	if (in < len) ++in;
	sp[SRC_IN] = in;
	if (n > TH_WORDBUF - 2) n = TH_WORDBUF - 2;
	th_wordbuf[0] = n;
	while (n > 0) { th_wordbuf[n] = line[i + n - 1]; --n; }
	th_wordbuf[th_wordbuf[0] + 1] = 0;
	th_push((int)th_wordbuf);
}

void th_p_findw (int *w) {           // FIND ( c-addr -- c-addr 0 | xt +-1 )
	char *cs;
	int  *xt;

	cs = (char *)th_pop();
	xt = th_find(cs + 1, cs[0]);
	if (!xt) { th_push((int)cs); th_push(0); return; }
	th_push((int)xt);
	if (xt[W_FLAGS] & FL_IMMEDIATE) th_push(1); else th_push(-1);
}

// -- the loop ---------------------------------------------------------

// Interpret the current source to exhaustion. Returns 0 on error or BYE.
int th_interpret () {
	int *xt;
	int  n;

	while (1) {
		if (!th_word()) {
			if (!th_refill()) return 1;
			continue;
		}
		xt = th_find(th_wname, th_wlen);
		if (xt) {
			if (th_state && !(xt[W_FLAGS] & FL_IMMEDIATE)) th_comma((int)xt);
			else th_call(xt);
		} else if (th_number(th_wname, th_wlen, &n)) {
			if (th_state) { th_comma((int)th_xlit); th_comma(n); }
			else th_push(n);
		} else {
			printf("c4th: %.*s ?\n", th_wlen, th_wname);
			th_err = 1;
		}
		if (th_err || th_quit) return 0;
	}
}

// EVALUATE interprets a string as if it were input. It pushes a source
// and re-enters the interpreter, which is exactly why sources are a stack
// and why th_call's trampolines are one too -- this can happen from
// inside an IMMEDIATE word that is itself running from a file.
void th_p_evaluate (int *w) {
	int n, a;

	n = th_pop();
	a = th_pop();
	if (!th_src_push((char *)a, n, 0)) return;
	th_interpret();
	th_src_pop();
}

// >NUMBER ( ud c-addr u -- ud' c-addr' u' ) -- accumulate digits in BASE
// until a character that is not one. It never consumes a sign; that is
// the caller's job, which is why the interpreter's own number parser is
// separate from this.
void th_p_tonumber (int *w) {
	int   u, hi, lo, c, d, lo2, hi2, junk;
	char *a;

	u  = th_pop();
	a  = (char *)th_pop();
	hi = th_pop();
	lo = th_pop();
	while (u > 0) {
		c = a[0];
		if      (c >= '0' && c <= '9') d = c - '0';
		else if (c >= 'A' && c <= 'Z') d = c - 'A' + 10;
		else if (c >= 'a' && c <= 'z') d = c - 'a' + 10;
		else break;
		if (d >= th_base) break;
		// ud = ud * base + d, in double precision
		th_ummul(lo, th_base, &lo2, &junk);
		hi2 = hi * th_base + junk;
		lo  = lo2 + d;
		if (th_uless(lo, lo2)) hi2 = hi2 + 1;
		hi  = hi2;
		++a; --u;
	}
	th_push(lo); th_push(hi); th_push((int)a); th_push(u);
}

void th_p_abort (int *w) {
	th_sp = th_dstack;
	th_ip = 0;
	th_err = 1;
}

void th_outer_init () {
	th_state  = 0;
	th_base   = 10;
	th_trampd = 0;
	th_quit   = 0;
	th_wordbuf = malloc(TH_WORDBUF);

	th_xstop = th_defword("(STOP)", FL_HIDDEN, (int)&th_p_stop);
	th_defword(":", 0, (int)&th_p_colon);
	th_defword(";", FL_IMMEDIATE | FL_COMPONLY, (int)&th_p_semi);
	th_defword("IMMEDIATE", 0, (int)&th_p_immediate);
	th_defword("[", FL_IMMEDIATE, (int)&th_p_lbrac);
	th_defword("]", 0, (int)&th_p_rbrac);
	th_defword("'", 0, (int)&th_p_tick);
	th_defword("LITERAL", FL_IMMEDIATE | FL_COMPONLY, (int)&th_p_literal);
	th_defword("STATE", 0, (int)&th_p_state);
	th_defword("BASE", 0, (int)&th_p_base);
	th_defword("\\", FL_IMMEDIATE, (int)&th_p_backslash);
	th_defword("(", FL_IMMEDIATE, (int)&th_p_paren);
	th_defword("(DOES>)", FL_COMPONLY, (int)&th_p_pdoes);
	th_defword("[']", FL_IMMEDIATE | FL_COMPONLY, (int)&th_p_bracktick);
	th_defword("POSTPONE", FL_IMMEDIATE | FL_COMPONLY, (int)&th_p_postpone);
	th_defword("RECURSE", FL_IMMEDIATE | FL_COMPONLY, (int)&th_p_recurse);
	th_defword("CHAR", 0, (int)&th_p_char);
	th_defword("[CHAR]", FL_IMMEDIATE | FL_COMPONLY, (int)&th_p_brackchar);
	th_defword("S\"", FL_IMMEDIATE, (int)&th_p_squote);
	th_defword(".\"", FL_IMMEDIATE, (int)&th_p_dotquote);
	th_defword(".(", FL_IMMEDIATE, (int)&th_p_dotparen);
	th_defword("CREATE", 0, (int)&th_p_create);
	th_defword("DOES>", FL_IMMEDIATE | FL_COMPONLY, (int)&th_p_does);
	th_defword(">BODY", 0, (int)&th_p_tobody);
	th_defword("LATEST", 0, (int)&th_p_latest);
	th_defword("EVALUATE", 0, (int)&th_p_evaluate);
	th_defword(">NUMBER", 0, (int)&th_p_tonumber);
	th_defword("ABORT", 0, (int)&th_p_abort);
	th_defword("WORD", 0, (int)&th_p_wordw);
	th_defword("FIND", 0, (int)&th_p_findw);
	th_defword("WORDS", 0, (int)&th_p_words);

	th_xlit      = th_findz("LIT");
	th_xexit     = th_findz("EXIT");
	th_xcomma    = th_findz(",");
	th_xpsquote  = th_findz("(S\")");
	th_xtype     = th_findz("TYPE");
	th_xpdoes    = th_findz("(DOES>)");
}
