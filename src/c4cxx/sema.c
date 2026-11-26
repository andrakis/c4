// sema.c -- types for every declaration and expression (M2)
//
// C's rules on c4's 32-bit target, which are gcc -m32's: the integer
// promotions and usual arithmetic conversions, pointer arithmetic, member
// lookup through anonymous members, constants typed by their value and
// suffix, string literals as char arrays. Integer constant expressions are
// folded (array sizes, bit-field widths, enum values, case labels). Structs
// are laid out as #pragma pack and __attribute__((packed)) say.
#include "types.h"

typedef struct Sym Sym;
struct Sym { Sym *next; Token *name; int kind; Ty *ty; long long val; int has_val; };
enum { S_VAR, S_FUNC, S_TYPEDEF, S_ENUMC };

typedef struct Tag Tag;
struct Tag { Tag *next; Token *name; Ty *ty; };

typedef struct Scope Scope;
struct Scope { Scope *up; Sym *syms; Tag *tags; };

static Scope *sc, *file_scope;
static int pack_cur, pack_stack[64], pack_sp;

static void push_scope (void)
{
	Scope *s = xalloc (sizeof *s);
	s->up = sc;
	sc = s;
}

static void pop_scope (void)
{
	sc = sc->up;
}

static int same_tok (Token *a, Token *b)
{
	return a->len == b->len && !memcmp (a->loc, b->loc, a->len);
}

static Sym *find_sym (Token *name)
{
	Scope *s;
	Sym *y;
	for (s = sc; s; s = s->up)
		for (y = s->syms; y; y = y->next)
			if (same_tok (y->name, name))
				return y;
	return 0;
}

static Sym *add_sym (Scope *s, Token *name, int kind, Ty *ty)
{
	Sym *y = xalloc (sizeof *y);
	y->name = name;
	y->kind = kind;
	y->ty = ty;
	y->next = s->syms;
	s->syms = y;
	return y;
}

static Tag *find_tag (Token *name, int here_only)
{
	Scope *s;
	Tag *t;
	for (s = sc; s; s = here_only ? 0 : s->up) {
		for (t = s->tags; t; t = t->next)
			if (same_tok (t->name, name))
				return t;
		if (here_only)
			break;
	}
	return 0;
}

static void add_tag (Token *name, Ty *ty)
{
	Tag *t = xalloc (sizeof *t);
	t->name = name;
	t->ty = ty;
	t->next = sc->tags;
	sc->tags = t;
}

// ---- #pragma pack ----

static void pragma (Token *t)
{
	// the line as "#pragma pack ( push , 1 )" (pp.c's make_pragma)
	char *s = xstrndup (t->loc, t->len), *p = strstr (s, "pack");
	int n;
	if (!p || strncmp (s, "#pragma pack", 12))
		return;
	p = strchr (p, '(');
	if (!p)
		return;
	p++;
	while (*p == ' ')
		p++;
	if (!strncmp (p, "push", 4)) {
		if (pack_sp < 64)
			pack_stack[pack_sp++] = pack_cur;
		p += 4;
		while (*p == ' ' || *p == ',')
			p++;
		if (isdigit ((unsigned char)*p))
			pack_cur = atoi (p);
	} else if (!strncmp (p, "pop", 3)) {
		if (pack_sp > 0)
			pack_cur = pack_stack[--pack_sp];
	} else if (isdigit ((unsigned char)*p)) {
		n = atoi (p);
		pack_cur = n;
	} else
		pack_cur = 0;
}

// ---- constant evaluation ----

static void expr (Node *n);
static Ty *spec_type (Spec *sp);
static Ty *decl_type (Spec *sp, Type *t);
static void initializer (Init *in, Ty *ty);

static long long truncate (long long v, Ty *t)
{
	if (!is_integer (t) || t->size >= 8)
		return v;
	if (t->kind == T_BOOL)
		return v != 0;
	if (is_unsigned (t))
		return t->size == 1 ? (unsigned char)v : t->size == 2 ? (unsigned short)v : (unsigned int)v;
	return t->size == 1 ? (signed char)v : t->size == 2 ? (short)v : (int)v;
}

// a constant's value, after expr() has typed it
static long long fold (Node *n, int *ok)
{
	*ok = 1;
	if (n->is_const)
		return n->val;
	*ok = 0;
	return 0;
}

static long long const_value (Node *n)
{
	int ok;
	long long v;
	expr (n);
	v = fold (n, &ok);
	if (!ok)
		error_at (n->tok, "not an integer constant expression");
	return v;
}

// ---- literals ----

static int str_chars (Token *t)
{
	char *p = t->loc, *e = t->loc + t->len - 1;	// the closing quote
	int n = 0;
	while (*p != '"')
		p++;
	for (p++; p < e; n++) {
		if (*p != '\\') {
			p++;
			continue;
		}
		p++;
		if (*p == 'x') {
			p++;
			while (isxdigit ((unsigned char)*p))
				p++;
		} else if (*p >= '0' && *p <= '7') {
			int k = 0;
			while (k < 3 && *p >= '0' && *p <= '7')
				p++, k++;
		} else
			p++;
	}
	return n;
}

static long long char_value (Token *t)
{
	char *p = t->loc, *end;
	long long v = 0;
	if (*p == 'L' || *p == 'u' || *p == 'U')
		p++;
	p++;
	while (*p != '\'') {
		int c;
		if (*p == '\\') {
			p++;
			switch (*p) {
			case 'n': c = '\n'; p++; break;
			case 't': c = '\t'; p++; break;
			case 'r': c = '\r'; p++; break;
			case 'a': c = 7; p++; break;
			case 'b': c = 8; p++; break;
			case 'f': c = 12; p++; break;
			case 'v': c = 11; p++; break;
			case 'x': c = (int)strtol (p + 1, &end, 16); p = end; break;
			default:
				if (*p >= '0' && *p <= '7') {
					int k = 0;
					c = 0;
					while (k < 3 && *p >= '0' && *p <= '7')
						c = c * 8 + (*p++ - '0'), k++;
				} else
					c = *p++;
			}
		} else
			c = (unsigned char)*p++;
		// gcc: a multi-character constant is the characters in order, as an
		// int; one character is a char's value (signed)
		v = (v << 8) | (c & 255);
	}
	if (t->len - 2 <= 2 || v < 256)
		return (signed char)v;
	return (int)v;
}

static void number (Node *n)
{
	Token *t = n->tok;
	char *s = xstrndup (t->loc, t->len), *end;
	int is_hex = s[0] == '0' && (s[1] == 'x' || s[1] == 'X');
	unsigned long long v;
	int u = 0, l = 0;
	if (strpbrk (s, ".") || (!is_hex && strpbrk (s, "eE")) || (is_hex && strpbrk (s, "pP"))) {
		char last = s[t->len - 1];
		n->ty = (last == 'f' || last == 'F') ? ty_float : (last == 'l' || last == 'L') ? ty_ldouble : ty_double;
		return;
	}
	if (is_hex)
		v = strtoull (s + 2, &end, 16);
	else if (s[0] == '0' && (s[1] == 'b' || s[1] == 'B'))
		v = strtoull (s + 2, &end, 2);
	else if (s[0] == '0')
		v = strtoull (s, &end, 8);
	else
		v = strtoull (s, &end, 10);
	for (; *end; end++) {
		if (*end == 'u' || *end == 'U') u = 1;
		else if (*end == 'l' || *end == 'L') l++;
	}
	// the first type that holds it (C99 6.4.4.1); long is 32 bits here
	if (l >= 2) {
		n->ty = (u || (v >> 63)) ? ty_ullong : ty_llong;
	} else if (l == 1) {
		if (u) n->ty = v <= 0xffffffffu ? ty_ulong : ty_ullong;
		else if (v <= 0x7fffffff) n->ty = ty_long;
		else if (v <= 0xffffffffu && (is_hex || s[0] == '0')) n->ty = ty_ulong;
		else n->ty = (v >> 63) ? ty_ullong : ty_llong;
	} else if (u) {
		n->ty = v <= 0xffffffffu ? ty_uint : ty_ullong;
	} else if (v <= 0x7fffffff) {
		n->ty = ty_int;
	} else if (v <= 0xffffffffu && (is_hex || s[0] == '0')) {
		n->ty = ty_uint;
	} else if (is_hex || s[0] == '0') {
		n->ty = (v >> 63) ? ty_ullong : (v <= 0x7fffffffffffffffull ? ty_llong : ty_ullong);
	} else
		n->ty = (v >> 63) ? ty_ullong : ty_llong;
	n->is_const = 1;
	n->val = (long long)v;
}

// ---- expressions ----

static int is_null_const (Node *n)
{
	return n->is_const && n->val == 0 && is_integer (n->ty);
}

static int is_void_ptr (Ty *t)
{
	return t->kind == T_PTR && t->base->kind == T_VOID;
}

static void binary (Node *n)
{
	Token *op = n->op;
	Ty *l, *r;
	int ok1, ok2;
	long long a, b;
	expr (n->lhs);
	expr (n->rhs);
	l = decay (n->lhs->ty);
	r = decay (n->rhs->ty);
	if (equal (op, "&&") || equal (op, "||") || equal (op, "==") || equal (op, "!=") ||
	    equal (op, "<") || equal (op, ">") || equal (op, "<=") || equal (op, ">=")) {
		n->ty = ty_int;
	} else if (equal (op, "<<") || equal (op, ">>")) {
		if (!is_integer (l)) { n->ty = ty_int; n->unsure = 1; return; }
		n->ty = promote (l);
	} else if (equal (op, "+")) {
		if (is_ptr (l) && is_integer (r)) n->ty = l;
		else if (is_integer (l) && is_ptr (r)) n->ty = r;
		else if (is_arith (l) && is_arith (r)) n->ty = usual_arith (l, r);
		else { n->ty = ty_int; n->unsure = 1; }
	} else if (equal (op, "-")) {
		if (is_ptr (l) && is_ptr (r)) {
			n->ty = ty_int;
			if (!same_type (unqualified (l->base), unqualified (r->base))) n->unsure = 1;
		} else if (is_ptr (l) && is_integer (r)) n->ty = l;
		else if (is_arith (l) && is_arith (r)) n->ty = usual_arith (l, r);
		else { n->ty = ty_int; n->unsure = 1; }
	} else {
		if (!is_arith (l) || !is_arith (r)) {
			// c4lc takes a pointer as a word here; gcc would not
			n->ty = ty_int;
			n->unsure = 1;
			return;
		}
		n->ty = usual_arith (l, r);
	}
	n->ty = unqualified (n->ty);
	a = fold (n->lhs, &ok1);
	b = fold (n->rhs, &ok2);
	n->cvar = n->lhs->cvar || n->rhs->cvar;
	n->unsure |= n->lhs->unsure || n->rhs->unsure;
	if (ok1 && ok2 && is_integer (n->ty)) {
		int u = is_unsigned (usual_arith (promote (l), is_integer (r) ? promote (r) : l));
		long long v = 0;
		if (equal (op, "&&")) v = a && b;
		else if (equal (op, "||")) v = a || b;
		else if (equal (op, "+")) v = a + b;
		else if (equal (op, "-")) v = a - b;
		else if (equal (op, "*")) v = a * b;
		else if (equal (op, "/")) { if (!b) return; v = u ? (long long)((unsigned long long)a / (unsigned long long)b) : a / b; }
		else if (equal (op, "%")) { if (!b) return; v = u ? (long long)((unsigned long long)a % (unsigned long long)b) : a % b; }
		else if (equal (op, "&")) v = a & b;
		else if (equal (op, "|")) v = a | b;
		else if (equal (op, "^")) v = a ^ b;
		else if (equal (op, "<<")) v = a << b;
		else if (equal (op, ">>")) v = is_unsigned (n->ty) ? (long long)((unsigned long long)truncate (a, n->ty) >> b) : a >> b;
		else if (equal (op, "==")) v = a == b;
		else if (equal (op, "!=")) v = a != b;
		else if (equal (op, "<")) v = u ? (unsigned long long)a < (unsigned long long)b : a < b;
		else if (equal (op, ">")) v = u ? (unsigned long long)a > (unsigned long long)b : a > b;
		else if (equal (op, "<=")) v = u ? (unsigned long long)a <= (unsigned long long)b : a <= b;
		else if (equal (op, ">=")) v = u ? (unsigned long long)a >= (unsigned long long)b : a >= b;
		n->is_const = 1;
		n->val = truncate (v, n->ty);
	}
}

static Ty *cond_type (Node *n)
{
	Ty *a = decay (n->then->ty), *b = decay (n->els->ty);
	if (is_arith (a) && is_arith (b))
		return usual_arith (a, b);
	if (a->kind == T_VOID || b->kind == T_VOID)
		return ty_void;
	if ((a->kind == T_STRUCT || a->kind == T_UNION) && same_type (a, b))
		return unqualified (a);
	if (is_ptr (a) && is_null_const (n->els))
		return a;
	if (is_ptr (b) && is_null_const (n->then))
		return b;
	if (is_ptr (a) && is_ptr (b)) {
		int q = a->base->qual | b->base->qual;
		if (is_void_ptr (a) || is_void_ptr (b))
			return pointer_to (qualified (ty_void, q));
		return pointer_to (qualified (unqualified (a->base), q));
	}
	if (is_ptr (a))
		return a;
	return b;
}

static void call (Node *n)
{
	Node *a;
	Ty *f;
	if (n->lhs->kind == ND_IDENT && !find_sym (n->lhs->tok)) {
		// an implicit declaration: int name()
		Ty *ft = func_type (ty_int);
		ft->oldstyle = 1;
		add_sym (file_scope, n->lhs->tok, S_FUNC, ft);
		n->implicit = 1;
	}
	expr (n->lhs);
	for (a = n->args; a; a = a->next)
		expr (a);
	f = decay (n->lhs->ty);
	if (is_ptr (f) && f->base->kind == T_FUNC)
		f = f->base;
	if (f->kind != T_FUNC) {
		// c4lc calls a word holding a function's address (c4cc's idiom)
		n->ty = ty_int;
		n->unsure = 1;
		return;
	}
	n->ty = unqualified (f->base);
}

static void member (Node *n)
{
	Ty *o;
	Field *f;
	expr (n->lhs);
	o = n->lhs->ty;
	if (equal (n->op, "->")) {
		o = decay (o);
		if (!is_ptr (o))
			error_at (n->op, "'->' on something not a pointer");
		o = o->base;
	}
	if (o->kind != T_STRUCT && o->kind != T_UNION)
		error_at (n->op, "member of something not a struct or union");
	if (!o->def->complete)
		error_at (n->op, "member of an incomplete struct");
	f = find_field (o, n->name, 0);
	if (!f)
		error_at (n->name, "no member named '%.*s'", n->name->len, n->name->loc);
	n->ty = qualified (f->ty, o->qual & (Q_CONST | Q_VOLATILE));
	n->lvalue = 1;
	n->bitfield = f->bit_width >= 0;
}

static Ty *type_name_type (Decl *d)
{
	return decl_type (d->spec, d->type);
}

static void expr (Node *n)
{
	Sym *y;
	Ty *t;
	int ok;
	if (n->ty)
		return;
	switch (n->kind) {
	case ND_NUM:
		number (n);
		return;
	case ND_CHAR:
		n->ty = ty_int;
		n->is_const = 1;
		n->val = char_value (n->tok);
		return;
	case ND_STR: {
		Token *s = n->tok;
		int i, len = 0;
		for (i = 0; i < n->ntoks; i++, s = s->next)
			len += str_chars (s);
		n->ty = array_of (ty_char, len + 1);
		n->lvalue = 1;
		return;
	}
	case ND_IDENT:
		y = find_sym (n->tok);
		if (!y) {
			if (equal (n->tok, "__func__") || equal (n->tok, "__FUNCTION__")) {
				n->ty = array_of (qualified (ty_char, Q_CONST), -1);
				return;
			}
			error_at (n->tok, "'%.*s' is not declared", n->tok->len, n->tok->loc);
		}
		if (y->kind == S_TYPEDEF)
			error_at (n->tok, "a type where a value should be");
		n->ty = y->ty;
		if (y->kind == S_ENUMC) {
			n->ty = ty_int;
			n->is_const = 1;
			n->val = y->val;
		} else if (y->kind == S_VAR) {
			n->lvalue = 1;
			if (y->has_val) {
				// a const integer with a constant initializer: a constant to
				// c4lc, and in C++; not in C (gcc is not asked about it)
				n->is_const = 1;
				n->cvar = 1;
				n->val = y->val;
			}
		}
		return;
	case ND_BINARY:
		binary (n);
		return;
	case ND_ASSIGN:
		expr (n->lhs);
		expr (n->rhs);
		n->ty = unqualified (n->lhs->ty);
		return;
	case ND_COND:
		expr (n->cond);
		expr (n->then);
		expr (n->els);
		n->ty = cond_type (n);
		n->cvar = n->cond->cvar || n->then->cvar || n->els->cvar;
		if (n->cond->is_const && n->then->is_const && n->els->is_const) {
			n->is_const = 1;
			n->val = truncate (n->cond->val ? n->then->val : n->els->val, n->ty);
		}
		return;
	case ND_COMMA:
		expr (n->lhs);
		expr (n->rhs);
		n->ty = unqualified (decay (n->rhs->ty));
		return;
	case ND_PREFIX:
		expr (n->lhs);
		t = n->lhs->ty;
		if (equal (n->op, "&")) {
			n->ty = pointer_to (t);
		} else if (equal (n->op, "*")) {
			t = decay (t);
			if (!is_ptr (t))
				error_at (n->op, "'*' of something not a pointer");
			n->ty = t->base;
			n->lvalue = t->base->kind != T_FUNC;
		} else if (equal (n->op, "!")) {
			n->ty = ty_int;
			if (n->lhs->is_const) { n->is_const = 1; n->val = !n->lhs->val; n->cvar = n->lhs->cvar; }
		} else if (equal (n->op, "++") || equal (n->op, "--")) {
			n->ty = unqualified (t);
		} else {
			n->ty = is_arith (t) ? promote (t) : unqualified (decay (t));
			n->cvar = n->lhs->cvar;
			if (n->lhs->is_const && is_integer (n->ty)) {
				long long v = n->lhs->val;
				n->is_const = 1;
				n->val = truncate (equal (n->op, "-") ? -v : equal (n->op, "~") ? ~v : v, n->ty);
			}
		}
		return;
	case ND_POSTFIX:
		expr (n->lhs);
		n->ty = unqualified (n->lhs->ty);
		return;
	case ND_CAST:
		expr (n->lhs);
		n->ty = unqualified (type_name_type (n->decl));
		n->cvar = n->lhs->cvar;
		if (n->lhs->is_const && is_integer (n->ty)) {
			n->is_const = 1;
			n->val = truncate (n->lhs->val, n->ty);
		}
		return;
	case ND_SIZEOF_TYPE:
		t = type_name_type (n->decl);
		n->ty = ty_uint;
		if (t->size >= 0) { n->is_const = 1; n->val = t->kind == T_VOID || t->kind == T_FUNC ? 1 : t->size; }
		return;
	case ND_SIZEOF_EXPR:
		expr (n->lhs);
		n->ty = ty_uint;
		t = n->lhs->ty;
		if (t->size >= 0) { n->is_const = 1; n->val = t->kind == T_VOID || t->kind == T_FUNC ? 1 : t->size; }
		return;
	case ND_COMPLIT:
		t = type_name_type (n->decl);
		initializer (n->initz, t);
		if (t->kind == T_ARRAY && t->len < 0)
			t = n->initz->is_list ? t : t;
		n->ty = t;
		n->lvalue = 1;
		return;
	case ND_CALL:
		call (n);
		return;
	case ND_INDEX: {
		Ty *a, *b;
		expr (n->lhs);
		expr (n->rhs);
		a = decay (n->lhs->ty);
		b = decay (n->rhs->ty);
		if (is_ptr (a))
			n->ty = a->base;
		else if (is_ptr (b))
			n->ty = b->base;
		else
			error_at (n->tok, "subscript of something not an array or pointer");
		n->lvalue = 1;
		return;
	}
	case ND_MEMBER:
		member (n);
		return;
	}
	(void)ok;
	error_at (n->tok, "sema: not an expression (%d)", n->kind);
}

// ---- initializers ----

// how many scalars an aggregate holds (for brace elision)
static int scalars (Ty *t)
{
	Field *f;
	int n = 0;
	if (t->kind == T_ARRAY)
		return t->len < 0 ? 1 : t->len * scalars (t->base);
	if (t->kind == T_STRUCT || t->kind == T_UNION) {
		for (f = t->def->fields; f; f = f->next) {
			if (!f->name && f->bit_width >= 0)
				continue;
			n += scalars (f->ty);
			if (t->kind == T_UNION)
				break;
		}
		return n ? n : 1;
	}
	return 1;
}

// how many elements an initializer gives an array of unknown length
static int init_length (Init *in, Ty *elem)
{
	InitItem *it;
	int i = 0, max = 0;
	if (!in->is_list) {
		if (in->expr->kind == ND_STR && (elem->kind == T_CHAR || elem->kind == T_SCHAR || elem->kind == T_UCHAR))
			return in->expr->ty->len;
		return 1;
	}
	// a string in braces
	if (in->items && !in->items->next && !in->items->desig && !in->items->init->is_list &&
	    in->items->init->expr->kind == ND_STR && elem->size == 1)
		return in->items->init->expr->ty->len;
	{
		// brace elision: unbraced scalars fill an aggregate element in turn
		int per = scalars (elem), pos = 0;
		for (it = in->items; it; it = it->next) {
			Ty *et = it->init->is_list ? 0 : it->init->expr->ty;
			if (it->desig && it->desig->index) {
				if (pos) { i++; pos = 0; }
				i = (int)it->desig->index->val;
			}
			if (it->init->is_list || per <= 1 ||
			    (et && !pos && (et->kind == T_STRUCT || et->kind == T_UNION || (et->kind == T_ARRAY && elem->kind == T_ARRAY)))) {
				if (pos) { i++; pos = 0; }
				i++;
			} else if (++pos == per) {
				i++;
				pos = 0;
			}
			if (i + (pos > 0) > max)
				max = i + (pos > 0);
		}
	}
	return max;
}

static void init_exprs (Init *in)
{
	InitItem *it;
	Desig *d;
	if (!in->is_list) {
		expr (in->expr);
		return;
	}
	for (it = in->items; it; it = it->next) {
		for (d = it->desig; d; d = d->next)
			if (d->index)
				const_value (d->index);
		init_exprs (it->init);
	}
}

static void initializer (Init *in, Ty *ty)
{
	(void)ty;
	init_exprs (in);
}

// ---- declarations ----

static Ty *arith_type (Spec *sp)
{
	int i, nlong = 0, nshort = 0, nchar = 0, nint = 0, sgn = 0, uns = 0, nvoid = 0, nbool = 0, nfloat = 0, ndouble = 0;
	for (i = 0; i < sp->nwords; i++) {
		Token *w = sp->words[i];
		if (equal (w, "long")) nlong++;
		else if (equal (w, "short")) nshort++;
		else if (equal (w, "char")) nchar++;
		else if (equal (w, "int")) nint++;
		else if (equal (w, "signed") || equal (w, "__signed__") || equal (w, "__signed")) sgn++;
		else if (equal (w, "unsigned")) uns++;
		else if (equal (w, "void")) nvoid++;
		else if (equal (w, "_Bool")) nbool++;
		else if (equal (w, "float")) nfloat++;
		else if (equal (w, "double")) ndouble++;
	}
	if (nvoid) return ty_void;
	if (nbool) return ty_bool;
	if (nfloat) return ty_float;
	if (ndouble) return nlong ? ty_ldouble : ty_double;
	if (nchar) return uns ? ty_uchar : sgn ? ty_schar : ty_char;
	if (nshort) return uns ? ty_ushort : ty_short;
	if (nlong >= 2) return uns ? ty_ullong : ty_llong;
	if (nlong) return uns ? ty_ulong : ty_long;
	return uns ? ty_uint : ty_int;
}

static int has_attr (Attr *a, const char *name)
{
	for (; a; a = a->next) {
		Token *t;
		for (t = a->start; t != a->end; t = t->next)
			if (equal (t, name))
				return 1;
	}
	return 0;
}

// __attribute__((aligned(N))): N, or 0
static int attr_aligned (Attr *a)
{
	for (; a; a = a->next) {
		Token *t;
		for (t = a->start; t != a->end; t = t->next)
			if ((equal (t, "aligned") || equal (t, "__aligned__")) && equal (t->next, "(") &&
			    t->next->next->kind == TK_NUM)
				return atoi (t->next->next->loc);
	}
	return 0;
}

static Ty *record (Spec *sp)
{
	Ty *rec = 0;
	int kind = sp->kind == SP_STRUCT ? T_STRUCT : sp->kind == SP_UNION ? T_UNION : T_ENUM;
	if (sp->tag) {
		Tag *tg = sp->has_body ? find_tag (sp->tag, 1) : find_tag (sp->tag, 0);
		if (tg && tg->ty->kind == kind && !(sp->has_body && tg->ty->complete))
			rec = tg->ty;
	}
	if (!rec) {
		rec = new_ty (kind, kind == T_ENUM ? 4 : -1, kind == T_ENUM ? 4 : 1);
		rec->def = rec;
		rec->tag = sp->tag;
		if (kind == T_ENUM)
			rec->complete = 1;
		if (sp->tag)
			add_tag (sp->tag, rec);
	}
	if (!sp->has_body)
		return rec;
	if (kind == T_ENUM) {
		Enumer *e;
		long long v = 0;
		int neg = 0;
		for (e = sp->enumers; e; e = e->next) {
			Sym *y;
			if (e->value)
				v = const_value (e->value);
			y = add_sym (sc, e->name, S_ENUMC, ty_int);
			y->val = v;
			e->val = v;
			if (v < 0)
				neg = 1;
			v++;
		}
		rec->is_unsigned_enum = !neg;
		return rec;
	}
	{
		Field head = {0}, *cur = &head;
		Decl *m;
		int pack = (has_attr (sp->tag_attrs, "packed") || has_attr (sp->tag_attrs, "__packed__")) ? 1 : pack_cur;
		for (m = sp->members; m; m = m->next) {
			Field *f;
			if (m->is_pragma) {
				pragma (m->tok);
				continue;
			}
			f = xalloc (sizeof *f);
			f->name = m->name;
			f->bit_width = m->width ? (int)const_value (m->width) : -1;
			if (!m->name && !m->width && m->type == 0)
				f->ty = spec_type (m->spec);
			else
				f->ty = decl_type (m->spec, m->type);
			m->rty = f->ty;
			if (has_attr (m->attrs, "packed") || has_attr (m->attrs, "__packed__"))
				f->ty = f->ty;	// a packed member: rare here; not modelled
			cur = cur->next = f;
		}
		rec->fields = head.next;
		layout (rec, pack);
		{
			int al = attr_aligned (sp->tag_attrs);
			if (attr_aligned (sp->attrs) > al)
				al = attr_aligned (sp->attrs);
			if (al > rec->align) {
				rec->align = al;
				rec->size = (rec->size + al - 1) / al * al;
			}
		}
	}
	return rec;
}

static Ty *spec_type (Spec *sp)
{
	Ty *t;
	if (sp->rty)
		return sp->rty;
	if (sp->kind != SP_WORDS)
		t = record (sp);
	else if (sp->is_typedef_name) {
		Sym *y = find_sym (sp->words[0]);
		if (!y || y->kind != S_TYPEDEF)
			error_at (sp->words[0], "not a type");
		t = y->ty;
	} else
		t = arith_type (sp);
	t = qualified (t, sp->qual & (Q_CONST | Q_VOLATILE));
	sp->rty = t;
	return t;
}

static Ty *adjust_param (Ty *t)
{
	if (t->kind == T_ARRAY)
		return pointer_to (t->base);
	if (t->kind == T_FUNC)
		return pointer_to (t);
	return t;
}

static Ty *chain (Spec *sp, Type *t)
{
	if (!t || t->kind == TY_BASE)
		return spec_type (sp);
	if (t->kind == TY_PTR)
		return qualified (pointer_to (chain (sp, t->base)), t->qual & (Q_CONST | Q_VOLATILE));
	if (t->kind == TY_ARRAY) {
		int len = t->len ? (int)const_value (t->len) : -1;
		return array_of (chain (sp, t->base), len);
	}
	{
		Ty *f = func_type (chain (sp, t->base));
		Decl *p;
		int n = 0;
		f->variadic = t->variadic;
		f->oldstyle = t->oldstyle;
		if (!t->oldstyle) {
			for (p = t->params; p; p = p->next)
				n++;
			f->params = xalloc (sizeof (Ty *) * (n + 1));
			f->nparams = n;
			n = 0;
			for (p = t->params; p; p = p->next) {
				p->rty = adjust_param (decl_type (p->spec, p->type));
				f->params[n++] = unqualified (p->rty);
			}
		}
		return f;
	}
}

static Ty *decl_type (Spec *sp, Type *t)
{
	return chain (sp, t);
}

static void declaration (Decl *d, int top)
{
	for (; d; d = d->next) {
		Ty *t;
		if (!d->name) {
			// struct x { ... }; and enum { ... };
			spec_type (d->spec);
			continue;
		}
		t = decl_type (d->spec, d->type);
		if (d->spec->storage & ST_TYPEDEF) {
			Ty *rec = t;
			add_sym (sc, d->name, S_TYPEDEF, t);
			if ((rec->kind == T_STRUCT || rec->kind == T_UNION || rec->kind == T_ENUM) && !rec->def->tag &&
			    !rec->def->name)
				rec->def->name = xstrndup (d->name->loc, d->name->len);
			d->rty = t;
			continue;
		}
		if (d->init) {
			initializer (d->init, t);
			if (t->kind == T_ARRAY && t->len < 0)
				t = array_of (t->base, init_length (d->init, t->base));
		}
		{
			Sym *old = 0;
			Scope *s;
			for (s = sc, old = 0; s && !old; s = 0) {
				Sym *y;
				for (y = s->syms; y; y = y->next)
					if (same_tok (y->name, d->name)) { old = y; break; }
			}
			// a declaration again in the same scope: the later one, with
			// any length it learned, stands
			if (old && old->kind != S_TYPEDEF && old->kind != S_ENUMC) {
				if (!(t->kind == T_ARRAY && t->len < 0 && old->ty->kind == T_ARRAY))
					old->ty = t;
				else
					t = old->ty;
			} else {
				Sym *y = add_sym (sc, d->name, t->kind == T_FUNC ? S_FUNC : S_VAR, t);
				if ((t->qual & Q_CONST) && is_integer (t) && d->init && !d->init->is_list &&
				    d->init->expr->is_const) {
					y->has_val = 1;
					y->val = truncate (d->init->expr->val, t);
				}
			}
		}
		d->rty = t;
	}
	(void)top;
}

// ---- statements ----

static void stmt (Node *n);

static void stmts (Node *n)
{
	for (; n; n = n->next)
		stmt (n);
}

static void stmt (Node *n)
{
	switch (n->kind) {
	case ND_PRAGMA:
		pragma (n->tok);
		return;
	case ND_BLOCK:
		push_scope ();
		stmts (n->body);
		pop_scope ();
		return;
	case ND_NULL: case ND_BREAK: case ND_CONTINUE: case ND_GOTO:
		return;
	case ND_EXPR_STMT:
		expr (n->lhs);
		return;
	case ND_DECL:
		declaration (n->decl, 0);
		return;
	case ND_IF:
		expr (n->cond);
		stmt (n->then);
		if (n->els)
			stmt (n->els);
		return;
	case ND_WHILE: case ND_SWITCH:
		expr (n->cond);
		stmt (n->body);
		return;
	case ND_DO:
		stmt (n->body);
		expr (n->cond);
		return;
	case ND_FOR:
		push_scope ();
		if (n->decl)
			declaration (n->decl, 0);
		if (n->init_expr)
			expr (n->init_expr);
		if (n->cond)
			expr (n->cond);
		if (n->inc)
			expr (n->inc);
		stmt (n->body);
		pop_scope ();
		return;
	case ND_CASE:
		const_value (n->cond);
		stmt (n->body);
		return;
	case ND_DEFAULT: case ND_LABEL:
		stmt (n->body);
		return;
	case ND_RETURN:
		if (n->lhs)
			expr (n->lhs);
		return;
	}
	error_at (n->tok, "sema: not a statement (%d)", n->kind);
}

static void funcdef (Node *n)
{
	Decl *d = n->decl, *p, *k;
	Ty *ft;
	declaration (d, 1);
	ft = d->rty;
	push_scope ();
	if (d->type && d->type->kind == TY_FUNC) {
		Type *ftype = d->type;
		for (p = ftype->params; p; p = p->next) {
			Ty *pt = p->rty;
			if (!pt) {
				// K&R: a declaration below, or int
				pt = ty_int;
				for (k = d->kr; k; k = k->next)
					if (k->name && same_tok (k->name, p->name)) {
						pt = adjust_param (decl_type (k->spec, k->type));
						k->rty = pt;
					}
			}
			if (p->name)
				add_sym (sc, p->name, S_VAR, pt);
		}
	}
	(void)ft;
	stmts (n->body->body);
	pop_scope ();
}

void sema (Node *unit)
{
	Node *n;
	types_init ();
	sc = 0;
	push_scope ();
	file_scope = sc;
	pack_cur = 0;
	pack_sp = 0;
	for (n = unit; n; n = n->next) {
		if (n->kind == ND_FUNCDEF)
			funcdef (n);
		else if (n->kind == ND_PRAGMA)
			pragma (n->tok);
		else
			declaration (n->decl, 1);
	}
}
