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

void th_outer_init () {
	th_state  = 0;
	th_base   = 10;
	th_trampd = 0;
	th_quit   = 0;

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
	th_defword("WORDS", 0, (int)&th_p_words);

	th_xlit  = th_find("LIT", 3);
	th_xexit = th_find("EXIT", 4);
}
