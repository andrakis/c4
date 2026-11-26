// pp.c -- the preprocessor
//
// With c4lc-pp's rules (src/c4sp/lisp/c4lc-pp.lisp) where they matter to
// what c4lc makes of the result: includes are found as an absolute name,
// then (for "...") beside the including file, then along -I, then in the
// working directory; __c4_inttypes__ is predefined; `# 123 "file"` line
// markers are skipped. Macro expansion is Prosser's algorithm: a token
// carries the set of macros it came out of (its hideset) and is not
// expanded by them again. #pragma lines pass through whole.
#include "c4cxx.h"

int opt_cplusplus;

typedef struct Param Param;
struct Param { Param *next; char *name; };

typedef struct Arg Arg;
struct Arg { Arg *next; char *name; int is_va; Token *tok; };

typedef struct Macro Macro;
struct Macro {
	Macro *next;
	char *name;
	int objlike;
	Param *params;
	char *va_name;		// the variadic parameter's name, or 0
	Token *body;
	Token *(*builtin) (Token *at);
};

struct Hideset { Hideset *next; char *name; };

typedef struct Cond Cond;
struct Cond { Cond *next; int ctx; Token *tok; int included; };
enum { IN_THEN, IN_ELIF, IN_ELSE };

static Macro *macros[4096];
static Cond *conds;
static char **inc_paths;
static int n_inc_paths;

// ---- small helpers ----

static unsigned hash (const char *s, int n)
{
	unsigned h = 2166136261u;
	int i;
	for (i = 0; i < n; i++)
		h = (h ^ (unsigned char)s[i]) * 16777619u;
	return h & 4095;
}

static Macro *find_macro (Token *t)
{
	Macro *m;
	if (t->kind != TK_IDENT)
		return 0;
	for (m = macros[hash (t->loc, t->len)]; m; m = m->next)
		if ((int)strlen (m->name) == t->len && !memcmp (m->name, t->loc, t->len))
			return m;
	return 0;
}

static Macro *add_macro (char *name, int objlike, Token *body)
{
	Macro *m = xalloc (sizeof *m);
	unsigned h = hash (name, (int)strlen (name));
	Token key = {0};
	Macro **pp;
	key.kind = TK_IDENT; key.loc = name; key.len = (int)strlen (name);
	for (pp = &macros[h]; *pp; pp = &(*pp)->next)
		if (!strcmp ((*pp)->name, name)) {
			*pp = (*pp)->next;
			break;
		}
	m->name = name;
	m->objlike = objlike;
	m->body = body;
	m->next = macros[h];
	macros[h] = m;
	return m;
}

void pp_undef (const char *name)
{
	unsigned h = hash (name, (int)strlen (name));
	Macro **pp;
	for (pp = &macros[h]; *pp; pp = &(*pp)->next)
		if (!strcmp ((*pp)->name, name)) {
			*pp = (*pp)->next;
			return;
		}
}

static Token *new_eof (Token *t)
{
	Token *e = copy_token (t);
	e->kind = TK_EOF;
	e->len = 0;
	return e;
}

static Hideset *new_hideset (char *name)
{
	Hideset *h = xalloc (sizeof *h);
	h->name = name;
	return h;
}

static int hideset_contains (Hideset *h, char *s, int len)
{
	for (; h; h = h->next)
		if ((int)strlen (h->name) == len && !memcmp (h->name, s, len))
			return 1;
	return 0;
}

static Hideset *hideset_union (Hideset *a, Hideset *b)
{
	Hideset head = {0}, *cur = &head;
	for (; a; a = a->next)
		cur = cur->next = new_hideset (a->name);
	cur->next = b;
	return head.next;
}

static Hideset *hideset_intersection (Hideset *a, Hideset *b)
{
	Hideset head = {0}, *cur = &head;
	for (; a; a = a->next)
		if (hideset_contains (b, a->name, (int)strlen (a->name)))
			cur = cur->next = new_hideset (a->name);
	return head.next;
}

static Token *add_hideset (Token *t, Hideset *hs)
{
	Token head = {0}, *cur = &head;
	for (; t; t = t->next) {
		Token *n = copy_token (t);
		n->hideset = hideset_union (n->hideset, hs);
		cur = cur->next = n;
	}
	return head.next;
}

static Token *append (Token *a, Token *b)
{
	Token head = {0}, *cur = &head;
	if (a->kind == TK_EOF)
		return b;
	for (; a->kind != TK_EOF; a = a->next)
		cur = cur->next = copy_token (a);
	cur->next = b;
	return head.next;
}

static Token *skip_line (Token *t)
{
	if (t->bol)
		return t;
	warn_at (t, "extra tokens after the directive");
	while (!t->bol)
		t = t->next;
	return t;
}

static Token *copy_line (Token **rest, Token *t)
{
	Token head = {0}, *cur = &head;
	for (; !t->bol; t = t->next)
		cur = cur->next = copy_token (t);
	cur->next = new_eof (t);
	*rest = t;
	return head.next;
}

static char *spelling (Token *t)
{
	return xstrndup (t->loc, t->len);
}

// ---- stringize and paste ----

static char *quote (const char *s)
{
	Buf b = {0};
	buf_char (&b, '"');
	for (; *s; s++) {
		if (*s == '"' || *s == '\\')
			buf_char (&b, '\\');
		buf_char (&b, *s);
	}
	buf_char (&b, '"');
	return b.p;
}

static Token *new_str_token (const char *s, Token *at)
{
	Token *t = tokenize_string (quote (s), at->file);
	t->line = at->line;
	t->file = at->file;
	return t;
}

static char *join_tokens (Token *t, Token *end)
{
	Buf b = {0};
	buf_add (&b, "", 0);
	for (; t != end && t->kind != TK_EOF; t = t->next) {
		if (t != end && b.n > 0 && t->space)
			buf_char (&b, ' ');
		buf_add (&b, t->loc, t->len);
	}
	return b.p;
}

static Token *stringize (Token *hash, Token *arg)
{
	return new_str_token (join_tokens (arg, 0), hash);
}

static Token *paste (Token *lhs, Token *rhs)
{
	char *s = xformat ("%.*s%.*s", lhs->len, lhs->loc, rhs->len, rhs->loc);
	Token *t = tokenize_string (s, lhs->file);
	if (t->next->kind != TK_EOF)
		error_at (lhs, "pasting gives '%s', not one token", s);
	t->line = lhs->line;
	t->file = lhs->file;
	t->space = lhs->space;
	t->bol = 0;
	return t;
}

// ---- macro definitions ----

static Param *read_params (Token **rest, Token *t, char **va_name)
{
	Param head = {0}, *cur = &head;
	while (!equal (t, ")")) {
		if (cur != &head) {
			if (!equal (t, ","))
				error_at (t, "expected ',' in the parameters");
			t = t->next;
		}
		if (equal (t, "...")) {
			*va_name = "__VA_ARGS__";
			*rest = t->next->next;
			if (!equal (t->next, ")"))
				error_at (t, "expected ')' after '...'");
			return head.next;
		}
		if (t->kind != TK_IDENT)
			error_at (t, "expected a parameter name");
		if (equal (t->next, "...")) {
			*va_name = spelling (t);
			if (!equal (t->next->next, ")"))
				error_at (t, "expected ')' after '...'");
			*rest = t->next->next->next;
			return head.next;
		}
		cur = cur->next = xalloc (sizeof (Param));
		cur->name = spelling (t);
		t = t->next;
	}
	*rest = t->next;
	return head.next;
}

static void read_define (Token **rest, Token *t)
{
	char *name;
	if (t->kind != TK_IDENT)
		error_at (t, "expected a macro name");
	name = spelling (t);
	t = t->next;
	if (!t->space && equal (t, "(") && !t->bol) {
		char *va = 0;
		Param *params = read_params (&t, t->next, &va);
		Macro *m = add_macro (name, 0, copy_line (rest, t));
		m->params = params;
		m->va_name = va;
	} else
		add_macro (name, 1, copy_line (rest, t));
}

// ---- macro arguments ----

static Arg *read_one_arg (Token **rest, Token *t, int read_rest)
{
	Token head = {0}, *cur = &head;
	int level = 0;
	Arg *a;
	for (;;) {
		if (level == 0 && equal (t, ")"))
			break;
		if (level == 0 && !read_rest && equal (t, ","))
			break;
		if (t->kind == TK_EOF)
			error_at (t, "the macro's arguments do not end");
		if (equal (t, "("))
			level++;
		else if (equal (t, ")"))
			level--;
		cur = cur->next = copy_token (t);
		t = t->next;
	}
	cur->next = new_eof (t);
	a = xalloc (sizeof *a);
	a->tok = head.next;
	*rest = t;
	return a;
}

static Arg *read_args (Token **rest, Token *t, Param *params, char *va_name)
{
	Token *start = t;
	Arg head = {0}, *cur = &head;
	Param *p;
	t = t->next->next;	// the name and '('
	for (p = params; p; p = p->next) {
		if (cur != &head) {
			if (!equal (t, ","))
				error_at (start, "too few arguments to the macro");
			t = t->next;
		}
		cur = cur->next = read_one_arg (&t, t, 0);
		cur->name = p->name;
	}
	if (va_name) {
		Arg *a;
		if (equal (t, ")")) {
			a = xalloc (sizeof *a);
			a->tok = new_eof (t);
		} else {
			if (params && !equal (t, ","))
				error_at (start, "too few arguments to the macro");
			if (params)
				t = t->next;
			a = read_one_arg (&t, t, 1);
		}
		a->name = va_name;
		a->is_va = 1;
		cur = cur->next = a;
	} else if (params == 0 && !equal (t, ")")) {
		// a macro of no parameters called with an empty argument only
		Arg *a = read_one_arg (&t, t, 0);
		if (a->tok->kind != TK_EOF)
			error_at (start, "too many arguments to the macro");
	}
	if (!equal (t, ")"))
		error_at (start, "too many arguments to the macro");
	*rest = t;
	return head.next;
}

static Arg *find_arg (Arg *args, Token *t)
{
	for (; args; args = args->next)
		if (t->kind == TK_IDENT && (int)strlen (args->name) == t->len && !memcmp (args->name, t->loc, t->len))
			return args;
	return 0;
}

static Token *preprocess2 (Token *t);
static int expand_macro (Token **rest, Token *t);

// the macros of an argument expanded, as for a parameter not next to # or ##.
// A copy: expansion relinks the tokens it passes, and the argument may be
// used again (stringized, pasted, or expanded once more).
static Token *expand_arg (Token *t)
{
	Token head = {0}, *cur = &head;
	for (; t->kind != TK_EOF; t = t->next)
		cur = cur->next = copy_token (t);
	cur->next = new_eof (t);
	return preprocess2 (head.next);
}

static Token *subst (Token *t, Arg *args)
{
	Token head = {0}, *cur = &head;
	while (t->kind != TK_EOF) {
		Arg *arg;
		// # param
		if (equal (t, "#")) {
			arg = find_arg (args, t->next);
			if (!arg)
				error_at (t->next, "'#' is not followed by a parameter");
			cur = cur->next = stringize (t, arg->tok);
			cur->space = t->space;
			t = t->next->next;
			continue;
		}
		// GNU: , ## __VA_ARGS__ drops the comma when the variable part is empty
		if (equal (t, ",") && equal (t->next, "##")) {
			arg = find_arg (args, t->next->next);
			if (arg && arg->is_va) {
				if (arg->tok->kind == TK_EOF)
					t = t->next->next->next;
				else {
					cur = cur->next = copy_token (t);
					t = t->next->next;
				}
				continue;
			}
		}
		// ## rhs
		if (equal (t, "##")) {
			if (cur == &head)
				error_at (t, "'##' cannot begin a macro's body");
			if (t->next->kind == TK_EOF)
				error_at (t, "'##' cannot end a macro's body");
			arg = find_arg (args, t->next);
			if (arg) {
				if (arg->tok->kind != TK_EOF) {
					Token *a;
					*cur = *paste (cur, arg->tok);
					for (a = arg->tok->next; a->kind != TK_EOF; a = a->next)
						cur = cur->next = copy_token (a);
				}
				t = t->next->next;
				continue;
			}
			*cur = *paste (cur, t->next);
			t = t->next->next;
			continue;
		}
		arg = find_arg (args, t);
		// param ## : the argument unexpanded
		if (arg && equal (t->next, "##")) {
			Token *rhs = t->next->next;
			if (arg->tok->kind == TK_EOF) {
				Arg *arg2 = find_arg (args, rhs);
				if (arg2) {
					Token *a;
					for (a = arg2->tok; a->kind != TK_EOF; a = a->next)
						cur = cur->next = copy_token (a);
				} else
					cur = cur->next = copy_token (rhs);
				t = rhs->next;
				continue;
			}
			{
				Token *a;
				for (a = arg->tok; a->kind != TK_EOF; a = a->next)
					cur = cur->next = copy_token (a);
			}
			t = t->next;
			continue;
		}
		// param: the argument expanded
		if (arg) {
			Token *a = expand_arg (arg->tok);
			int first = 1;
			for (; a->kind != TK_EOF; a = a->next) {
				cur = cur->next = copy_token (a);
				if (first)
					cur->space = t->space;
				first = 0;
			}
			t = t->next;
			continue;
		}
		cur = cur->next = copy_token (t);
		t = t->next;
	}
	cur->next = t;
	return head.next;
}

static int expand_macro (Token **rest, Token *t)
{
	Macro *m;
	Token *body, *p;
	if (hideset_contains (t->hideset, t->loc, t->len))
		return 0;
	m = find_macro (t);
	if (!m)
		return 0;
	if (m->builtin) {
		*rest = m->builtin (t);
		(*rest)->next = t->next;
		return 1;
	}
	if (m->objlike) {
		Hideset *hs = hideset_union (t->hideset, new_hideset (m->name));
		body = add_hideset (m->body, hs);
		for (p = body; p && p->kind != TK_EOF; p = p->next) {
			p->origin = t;
			p->line = t->line;
			p->file = t->file;
		}
		*rest = append (body, t->next);
		(*rest)->bol = t->bol;
		(*rest)->space = t->space;
		return 1;
	}
	// a function-like macro's name with no '(' after it is just the name
	if (!equal (t->next, "("))
		return 0;
	{
		Token *macro_tok = t, *rparen;
		Arg *args = read_args (&t, t, m->params, m->va_name);
		Hideset *hs;
		rparen = t;
		hs = hideset_intersection (macro_tok->hideset, rparen->hideset);
		hs = hideset_union (hs, new_hideset (m->name));
		body = subst (m->body, args);
		body = add_hideset (body, hs);
		for (p = body; p && p->kind != TK_EOF; p = p->next) {
			p->origin = macro_tok;
			p->line = macro_tok->line;
			p->file = macro_tok->file;
		}
		*rest = append (body, t->next);
		(*rest)->bol = macro_tok->bol;
		(*rest)->space = macro_tok->space;
		return 1;
	}
}

// ---- #if ----

static Token *read_const_expr (Token **rest, Token *t)
{
	Token head = {0}, *cur = &head;
	t = copy_line (rest, t);
	while (t->kind != TK_EOF) {
		// defined X, defined(X)
		if (equal (t, "defined")) {
			Token *start = t;
			int paren = equal (t->next, "(");
			Macro *m;
			Token *n;
			t = paren ? t->next->next : t->next;
			if (t->kind != TK_IDENT)
				error_at (start, "'defined' wants a name");
			m = find_macro (t);
			t = t->next;
			if (paren) {
				if (!equal (t, ")"))
					error_at (start, "expected ')' after 'defined('");
				t = t->next;
			}
			n = copy_token (start);
			n->kind = TK_NUM;
			n->loc = m ? "1" : "0";
			n->len = 1;
			cur = cur->next = n;
			continue;
		}
		cur = cur->next = t;
		t = t->next;
	}
	cur->next = t;
	return head.next;
}

typedef long long ll;
static Token *ce_tok;

static ll ce_expr (void);

static ll num_value (Token *t)
{
	char *s = xstrndup (t->loc, t->len), *end;
	ll v;
	if (t->kind == TK_CHAR) {
		char *p = s;
		v = 0;
		if (*p == 'L' || *p == 'u' || *p == 'U')
			p++;
		p++;
		// c4lc's rule: the value of the last character
		while (*p && *p != '\'') {
			int c;
			if (*p == '\\') {
				p++;
				switch (*p) {
				case 'n': c = '\n'; p++; break;
				case 't': c = '\t'; p++; break;
				case 'r': c = '\r'; p++; break;
				case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7':
					c = (int)strtol (p, &end, 8); p = end; break;
				case 'x': c = (int)strtol (p + 1, &end, 16); p = end; break;
				default: c = *p++; break;
				}
			} else
				c = (unsigned char)*p++;
			v = c;
		}
		return v;
	}
	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
		v = (ll)strtoull (s + 2, &end, 16);
	else if (s[0] == '0' && (s[1] == 'b' || s[1] == 'B'))
		v = (ll)strtoull (s + 2, &end, 2);
	else if (s[0] == '0')
		v = (ll)strtoull (s, &end, 8);
	else
		v = (ll)strtoull (s, &end, 10);
	while (*end == 'u' || *end == 'U' || *end == 'l' || *end == 'L')
		end++;
	if (*end)
		error_at (t, "bad number in #if");
	return v;
}

static ll ce_primary (void)
{
	Token *t = ce_tok;
	if (equal (t, "(")) {
		ll v;
		ce_tok = t->next;
		v = ce_expr ();
		if (!equal (ce_tok, ")"))
			error_at (ce_tok, "expected ')' in #if");
		ce_tok = ce_tok->next;
		return v;
	}
	if (t->kind == TK_NUM || t->kind == TK_CHAR) {
		ce_tok = t->next;
		return num_value (t);
	}
	error_at (t, "bad #if expression");
	return 0;
}

static ll ce_unary (void)
{
	Token *t = ce_tok;
	if (equal (t, "+")) { ce_tok = t->next; return ce_unary (); }
	if (equal (t, "-")) { ce_tok = t->next; return -ce_unary (); }
	if (equal (t, "!")) { ce_tok = t->next; return !ce_unary (); }
	if (equal (t, "~")) { ce_tok = t->next; return ~ce_unary (); }
	return ce_primary ();
}

static int binprec (Token *t)
{
	static const char *ops[] = { "*", "/", "%", "+", "-", "<<", ">>", "<", ">", "<=", ">=",
		"==", "!=", "&", "^", "|", "&&", "||", 0 };
	static const int prec[] = { 10, 10, 10, 9, 9, 8, 8, 7, 7, 7, 7, 6, 6, 5, 4, 3, 2, 1 };
	int i;
	if (t->kind != TK_PUNCT)
		return 0;
	for (i = 0; ops[i]; i++)
		if (equal (t, ops[i]))
			return prec[i];
	return 0;
}

static ll ce_binary (int minprec)
{
	ll lhs = ce_unary ();
	for (;;) {
		Token *op = ce_tok;
		int p = binprec (op);
		ll rhs;
		if (!p || p < minprec)
			return lhs;
		ce_tok = op->next;
		rhs = ce_binary (p + 1);
		if (equal (op, "*")) lhs *= rhs;
		else if (equal (op, "/")) { if (!rhs) error_at (op, "division by zero in #if"); lhs /= rhs; }
		else if (equal (op, "%")) { if (!rhs) error_at (op, "division by zero in #if"); lhs %= rhs; }
		else if (equal (op, "+")) lhs += rhs;
		else if (equal (op, "-")) lhs -= rhs;
		else if (equal (op, "<<")) lhs <<= rhs;
		else if (equal (op, ">>")) lhs >>= rhs;
		else if (equal (op, "<")) lhs = lhs < rhs;
		else if (equal (op, ">")) lhs = lhs > rhs;
		else if (equal (op, "<=")) lhs = lhs <= rhs;
		else if (equal (op, ">=")) lhs = lhs >= rhs;
		else if (equal (op, "==")) lhs = lhs == rhs;
		else if (equal (op, "!=")) lhs = lhs != rhs;
		else if (equal (op, "&")) lhs &= rhs;
		else if (equal (op, "^")) lhs ^= rhs;
		else if (equal (op, "|")) lhs |= rhs;
		else if (equal (op, "&&")) lhs = lhs && rhs;
		else if (equal (op, "||")) lhs = lhs || rhs;
	}
}

static ll ce_expr (void)
{
	ll c = ce_binary (1);
	if (equal (ce_tok, "?")) {
		ll a, b;
		ce_tok = ce_tok->next;
		a = ce_expr ();
		if (!equal (ce_tok, ":"))
			error_at (ce_tok, "expected ':' in #if");
		ce_tok = ce_tok->next;
		b = ce_expr ();
		return c ? a : b;
	}
	return c;
}

static ll eval_const_expr (Token **rest, Token *t)
{
	Token *start = t, *e, *p;
	ll v;
	e = read_const_expr (rest, t->next);
	e = preprocess2 (e);
	if (e->kind == TK_EOF)
		error_at (start, "#if with no expression");
	// names left after expansion are 0
	for (p = e; p->kind != TK_EOF; p = p->next)
		if (p->kind == TK_IDENT) {
			p->kind = TK_NUM;
			p->loc = "0";
			p->len = 1;
		}
	ce_tok = e;
	v = ce_expr ();
	if (ce_tok->kind != TK_EOF)
		error_at (ce_tok, "extra tokens in #if");
	return v;
}

static Cond *push_cond (Token *t, int included)
{
	Cond *c = xalloc (sizeof *c);
	c->next = conds;
	c->ctx = IN_THEN;
	c->tok = t;
	c->included = included;
	conds = c;
	return c;
}

// to the #elif, #else or #endif that ends this group, past nested ones
static Token *skip_cond (Token *t)
{
	while (t->kind != TK_EOF) {
		if (t->bol && equal (t, "#") && t->next->kind == TK_IDENT &&
		    (equal (t->next, "if") || equal (t->next, "ifdef") || equal (t->next, "ifndef"))) {
			int depth = 1;
			t = t->next->next;
			while (t->kind != TK_EOF && depth) {
				if (t->bol && equal (t, "#")) {
					Token *d = t->next;
					if (equal (d, "if") || equal (d, "ifdef") || equal (d, "ifndef"))
						depth++;
					else if (equal (d, "endif"))
						depth--;
				}
				t = t->next;
			}
			continue;
		}
		if (t->bol && equal (t, "#") &&
		    (equal (t->next, "elif") || equal (t->next, "else") || equal (t->next, "endif")))
			return t;
		t = t->next;
	}
	return t;
}

// ---- #include ----

void pp_add_include_path (const char *dir)
{
	char **n = xalloc (sizeof (char *) * (n_inc_paths + 1));
	int len = (int)strlen (dir);
	if (n_inc_paths)
		memcpy (n, inc_paths, sizeof (char *) * n_inc_paths);
	n[n_inc_paths++] = (len && dir[len - 1] == '/') ? xstrndup (dir, len) : xformat ("%s/", dir);
	inc_paths = n;
}

static int file_exists (const char *p)
{
	FILE *f = fopen (p, "rb");
	if (!f)
		return 0;
	fclose (f);
	return 1;
}

static char *resolve_include (Token *at, const char *name, int angle)
{
	int i;
	if (name[0] == '/')
		return file_exists (name) ? xformat ("%s", name) : 0;
	if (!angle) {
		char *p = xformat ("%s%s", at->file->dir, name);
		if (file_exists (p))
			return p;
	}
	for (i = 0; i < n_inc_paths; i++) {
		char *p = xformat ("%s%s", inc_paths[i], name);
		if (file_exists (p))
			return p;
	}
	if (file_exists (name))
		return xformat ("%s", name);
	return 0;
}

static Token *include_file (Token *rest, char *path, Token *at)
{
	char *text = read_file (path);
	File *f;
	Token *t;
	if (!text)
		error_at (at, "cannot read include: %s", path);
	f = new_file (path, text);
	t = tokenize (f);
	return append (t, rest);
}

static char *read_include_name (Token **rest, Token *t, int *angle)
{
	if (t->kind == TK_STR) {
		*angle = 0;
		*rest = skip_line (t->next);
		return xstrndup (t->loc + 1, t->len - 2);
	}
	if (equal (t, "<")) {
		Token *start = t;
		Buf b = {0};
		buf_add (&b, "", 0);
		for (t = t->next; !equal (t, ">"); t = t->next) {
			if (t->bol || t->kind == TK_EOF)
				error_at (start, "expected '>'");
			if (t->space && b.n)
				buf_char (&b, ' ');
			buf_add (&b, t->loc, t->len);
		}
		*angle = 1;
		*rest = skip_line (t->next);
		return b.p;
	}
	// a macro naming the header
	if (t->kind == TK_IDENT) {
		Token *line = copy_line (rest, t);
		line = preprocess2 (line);
		return read_include_name (&line, line, angle);
	}
	error_at (t, "expected a file name");
	return 0;
}

// ---- builtins ----

static Token *builtin_line (Token *at)
{
	Token *o = at;
	Token *t;
	while (o->origin)
		o = o->origin;
	t = tokenize_string (xformat ("%d", o->line), at->file);
	t->line = at->line;
	t->file = at->file;
	t->space = at->space;
	t->bol = at->bol;
	return t;
}

static Token *builtin_file (Token *at)
{
	Token *o = at;
	Token *t;
	while (o->origin)
		o = o->origin;
	t = new_str_token (o->file->name, at);
	t->space = at->space;
	t->bol = at->bol;
	return t;
}

static void add_builtin (char *name, Token *(*fn) (Token *))
{
	Macro *m = add_macro (name, 1, 0);
	m->builtin = fn;
}

void pp_define (const char *spec)
{
	const char *eq = strchr (spec, '=');
	char *name = eq ? xstrndup (spec, (int)(eq - spec)) : xformat ("%s", spec);
	char *val = eq ? xformat ("%s", eq + 1) : xformat ("1");
	Token *t = tokenize_string (val, 0);
	add_macro (name, 1, t);
}

// ---- the main loop ----

static Token *make_pragma (Token **rest, Token *hash)
{
	Token *t = hash->next;		// "pragma"
	Token *line = copy_line (rest, t->next);
	Token *p = copy_token (hash);
	Buf b = {0};
	buf_str (&b, "#pragma");
	for (; line->kind != TK_EOF; line = line->next) {
		buf_char (&b, ' ');
		buf_add (&b, line->loc, line->len);
	}
	p->kind = TK_PRAGMA;
	p->loc = b.p;
	p->len = b.n;
	p->bol = 1;
	return p;
}

static Token *preprocess2 (Token *t)
{
	Token head = {0}, *cur = &head;
	while (t->kind != TK_EOF) {
		Token *start;
		if (expand_macro (&t, t))
			continue;
		if (!(t->bol && equal (t, "#"))) {
			cur = cur->next = t;
			t = t->next;
			continue;
		}
		start = t;
		t = t->next;
		// the null directive, and gcc's "# 123 file" line markers: skipped
		if (t->bol)
			continue;
		if (t->kind == TK_NUM) {
			while (!t->bol)
				t = t->next;
			continue;
		}
		if (equal (t, "include")) {
			int angle;
			char *name = read_include_name (&t, t->next, &angle);
			char *path = resolve_include (start, name, angle);
			if (!path)
				error_at (start, "cannot find include: %s", name);
			t = include_file (t, path, start);
			continue;
		}
		if (equal (t, "define")) {
			read_define (&t, t->next);
			continue;
		}
		if (equal (t, "undef")) {
			t = t->next;
			if (t->kind != TK_IDENT)
				error_at (t, "expected a macro name");
			pp_undef (spelling (t));
			t = skip_line (t->next);
			continue;
		}
		if (equal (t, "if")) {
			ll v = eval_const_expr (&t, t);
			push_cond (start, v != 0);
			if (!v)
				t = skip_cond (t);
			continue;
		}
		if (equal (t, "ifdef") || equal (t, "ifndef")) {
			int def = find_macro (t->next) != 0;
			int want = equal (t, "ifdef");
			push_cond (t, def == want);
			t = skip_line (t->next->next);
			if (def != want)
				t = skip_cond (t);
			continue;
		}
		if (equal (t, "elif")) {
			if (!conds || conds->ctx == IN_ELSE)
				error_at (start, "stray #elif");
			conds->ctx = IN_ELIF;
			if (!conds->included && eval_const_expr (&t, t))
				conds->included = 1;
			else {
				while (!t->bol)
					t = t->next;
				t = skip_cond (t);
			}
			continue;
		}
		if (equal (t, "else")) {
			if (!conds || conds->ctx == IN_ELSE)
				error_at (start, "stray #else");
			conds->ctx = IN_ELSE;
			t = skip_line (t->next);
			if (conds->included)
				t = skip_cond (t);
			else
				conds->included = 1;
			continue;
		}
		if (equal (t, "endif")) {
			if (!conds)
				error_at (start, "stray #endif");
			conds = conds->next;
			t = skip_line (t->next);
			continue;
		}
		if (equal (t, "pragma")) {
			cur = cur->next = make_pragma (&t, start);
			continue;
		}
		if (equal (t, "line")) {
			while (!t->bol)
				t = t->next;
			continue;
		}
		if (equal (t, "error"))
			error_at (start, "#error %s", join_tokens (t->next, 0));
		if (equal (t, "warning") || equal (t, "ident") || equal (t, "sccs")) {
			while (!t->bol)
				t = t->next;
			continue;
		}
		error_at (t, "unknown directive");
	}
	cur->next = t;
	return head.next;
}

static char **cl_defs, **cl_undefs;
static int cl_ndef, cl_nundef;

void pp_set_command_line (char **defs, int ndef, char **undefs, int nundef)
{
	cl_defs = defs; cl_ndef = ndef;
	cl_undefs = undefs; cl_nundef = nundef;
}

static void init_macros (void)
{
	int i;
	add_builtin ("__LINE__", builtin_line);
	add_builtin ("__FILE__", builtin_file);
	pp_define ("__c4_inttypes__=1");
	if (opt_cplusplus)
		pp_define ("__cplusplus=199711L");
	for (i = 0; i < cl_ndef; i++)
		pp_define (cl_defs[i]);
	for (i = 0; i < cl_nundef; i++)
		pp_undef (cl_undefs[i]);
}

Token *preprocess (const char *path)
{
	char *text = read_file (path);
	File *f;
	Token *t;
	if (!text)
		fatal ("cannot read %s", path);
	f = new_file (path, text);
	init_macros ();
	t = preprocess2 (tokenize (f));
	if (conds)
		error_at (conds->tok, "unterminated conditional");
	return t;
}

// ---- -E ----

// One token per position with spaces between, a line marker when the file
// changes or the line jumps, and line breaks kept within a file, so that a
// c4lc error's line is the source's.
void print_tokens (FILE *out, Token *t)
{
	File *file = 0;
	int line = 0, first = 1;
	for (; t && t->kind != TK_EOF; t = t->next) {
		if (t->kind == TK_PRAGMA) {
			fprintf (out, "\n%.*s\n", t->len, t->loc);
			line++;
			first = 1;
			continue;
		}
		if (t->file != file || t->line < line || t->line > line + 8) {
			if (!first || file)
				fputc ('\n', out);
			fprintf (out, "# %d \"%s\"\n", t->line, t->file ? t->file->name : "");
			file = t->file;
			line = t->line;
			first = 1;
		}
		while (line < t->line) {
			fputc ('\n', out);
			line++;
			first = 1;
		}
		if (!first)
			fputc (' ', out);
		fprintf (out, "%.*s", t->len, t->loc);
		first = 0;
	}
	fputc ('\n', out);
}
