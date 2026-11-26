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
typedef struct Method Method;
typedef struct Class Class;
struct Sym {
	Sym *next; Token *name; int kind; Ty *ty; long long val; int has_val;
	// C++
	int is_tagname;		// S_TYPEDEF: a class or enum's tag, as a type name
	Method *m;		// S_METHOD
	Token *cname;		// S_SVAR: the global's name in C (X__name); S_FUNC: mangled
	int c_link;		// S_FUNC: extern "C" (its name is its own)
};
enum { S_VAR, S_FUNC, S_TYPEDEF, S_ENUMC,
	// C++: a class's members, in its scope
	S_FIELD, S_METHOD, S_SVAR };

typedef struct Tag Tag;
struct Tag { Tag *next; Token *name; Ty *ty; };

typedef struct Scope Scope;
struct Scope { Scope *up; Sym *syms; Tag *tags; };

static Scope *sc, *file_scope;
static int pack_cur, pack_stack[64], pack_sp;

// ---- C++: classes (M3) ----
//
// A class is a C struct of its data members. Its member functions become
// functions named Class__name with `this` first; a constructor is
// Class__C (returning this) and a destructor Class__D. Member functions
// defined in the class body are static in each unit that sees the class.

struct Method {
	Method *next;
	Token *name;
	Decl *decl;		// as declared in the class
	int is_static, is_ctor, is_dtor;
	int inline_body;	// defined in the class body
	struct Class *owner;
	Token *cname;		// its name in C
	Ty *fty;		// its type in C: `this` first unless static
	// M5
	int is_virtual, pure;
	struct Slot *slot;	// its vtable slot, when virtual
};

// a vtable slot (plan section 3.2): a field of struct X__vt, typed as the
// class that introduced it declared it (`this` of that class)
typedef struct Slot Slot;
struct Slot {
	Slot *next;
	Token *name;		// Paint__iic, or _dtor
	Method *m;		// the class's final overrider
	Ty *fty;		// the introducer's type
	int index, is_dtor;
};

struct Class {
	Token *name;
	Ty *rec;
	Scope *scope;		// S_FIELD, S_METHOD, S_SVAR; up is where the class is
	Method *methods;
	Method *ctor, *dtor;	// the user's, or made when members need them
	int needs_ctor, needs_dtor;
	int anon;		// anonymous members named so far (_u0, _u1, ...)
	// M5
	Class *base;		// its one base class, the member _b
	int poly;		// has a vtable (its _vp is its own, or in _b)
	Class *root;		// the polymorphic class whose first member is _vp
	Slot *slots;		// the vtable's, the base's first
	Ty *vt;			// struct X__vt
	Method *key;		// the key function, whose unit defines X__vtbl; or 0
};

// an object to destroy when its scope ends: a local, or a member (in a
// destructor)
typedef struct Obj Obj;
struct Obj { Obj *next; Class *cls; Token *var; Field *member; };

// a scope that destructors run at the end of
typedef struct CScope CScope;
struct CScope { CScope *up; int kind; Obj *objs; };
enum { CS_BLOCK, CS_LOOP, CS_SWITCH, CS_FUNC };

static CScope *cs;
static Class *cur_class;	// in a member function of
static Method *cur_method;
static Ty *cur_ret;		// the function's return type
static Node *cur_top;		// the top-level node being typed (pre and post go on it)
static int in_function;
static Node *init_stmts, **init_tail = &init_stmts;	// global objects to construct
static Node *fini_stmts;	// and to destroy (last first)
static Scope *fn_scope;		// the function's outermost scope (temporaries go there)
static Node *fn_temps;		// and their declarations
static int n_temps;
static Type sema_base = { TY_BASE };

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
	if (opt_cplusplus) {
		// C++: the tag is a type name too
		Sym *y = add_sym (sc, name, S_TYPEDEF, ty);
		y->is_tagname = 1;
	}
}

static Sym *find_in (Scope *s, Token *name)
{
	Sym *y;
	for (y = s->syms; y; y = y->next)
		if (same_tok (y->name, name))
			return y;
	return 0;
}

// the class a name (X in X::f) names
static Class *class_named (Token *name)
{
	Sym *y = find_sym (name);
	if (!y || y->kind != S_TYPEDEF || (y->ty->kind != T_STRUCT && y->ty->kind != T_UNION) || !y->ty->def->cls)
		error_at (name, "'%.*s' is not a class", name->len, name->loc);
	return y->ty->def->cls;
}

static Class *class_of (Ty *t)
{
	return t && (t->kind == T_STRUCT || t->kind == T_UNION) && t->def ? t->def->cls : 0;
}

// is D the class B, or derived from it?
static int derives (Class *d, Class *b)
{
	for (; d; d = d->base)
		if (d == b)
			return 1;
	return 0;
}

// is D derived from B (and not B)?
static int strictly_derives (Class *d, Class *b)
{
	return d && b && d != b && derives (d, b);
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
static void member (Node *n);
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

// ---- C++: made nodes, references, calls ----

static Node *mk (int kind, Token *at)
{
	Node *n = xalloc (sizeof *n);
	n->kind = kind;
	n->tok = at;
	return n;
}

static Node *mk_ident (Token *at, const char *name)
{
	return mk (ND_IDENT, make_ident (at, name));
}

static Node *mk_this (Token *at)
{
	Node *n = mk_ident (at, "this");
	expr (n);
	return n;
}

static Node *mk_stmt (Node *e)
{
	Node *s = mk (ND_EXPR_STMT, e->tok);
	s->lhs = e;
	return s;
}

// N, of a reference type, as the object it refers to: *N
static void deref_ref (Node *n)
{
	Node *m, *next = n->next;
	Token *at = n->tok;
	if (!n->ty || n->ty->kind != T_REF)
		return;
	m = xalloc (sizeof *m);
	*m = *n;
	m->next = 0;
	m->ty = pointer_to (n->ty->base);
	m->lvalue = 0;
	memset (n, 0, sizeof *n);
	n->kind = ND_PREFIX;
	n->tok = at;
	n->op = make_ident (at, "*");
	n->lhs = m;
	n->ty = m->ty->base;
	n->lvalue = 1;
	n->next = next;
}

// N as its address (&N; *p as p)
static void take_address (Node *n)
{
	Node *m, *next = n->next;
	Token *at = n->tok;
	if (n->kind == ND_PREFIX && equal (n->op, "*")) {
		*n = *n->lhs;
		n->next = next;
		return;
	}
	m = xalloc (sizeof *m);
	*m = *n;
	m->next = 0;
	memset (n, 0, sizeof *n);
	n->kind = ND_PREFIX;
	n->tok = at;
	n->op = make_ident (at, "&");
	n->lhs = m;
	n->ty = pointer_to (m->ty);
	n->next = next;
}

// N as (TO) N
static void tcast (Node *n, Ty *to)
{
	Node *m = xalloc (sizeof *m), *next = n->next;
	Token *at = n->tok;
	*m = *n;
	m->next = 0;
	memset (n, 0, sizeof *n);
	n->kind = ND_TCAST;
	n->tok = at;
	n->lhs = m;
	n->ty = to;
	n->next = next;
}

// the class a pointer of type T points to
static Class *ptr_class (Ty *t)
{
	t = t ? decay (t) : 0;
	return t && is_ptr (t) ? class_of (t->base) : 0;
}

// C++'s conversion that C lacks (M5): a pointer to a derived class as a
// pointer to its base, a cast at offset 0 (plan section 3.2)
static void conv (Node *n, Ty *to)
{
	if (!n->ty || !to || !is_ptr (to) || !strictly_derives (ptr_class (n->ty), class_of (to->base)))
		return;
	tcast (n, unqualified (to));
}

// N bound to a reference of type REF: its address, or a const reference's
// copy of an rvalue
static void bind_ref (Node *n, Ty *ref)
{
	Node *m, *next = n->next;
	if (n->lvalue && !n->bitfield && (!n->ty || same_type (unqualified (n->ty), unqualified (ref->base)))) {
		take_address (n);
		return;
	}
	if (n->lvalue && !n->bitfield && strictly_derives (class_of (n->ty), class_of (ref->base))) {
		// a derived object as its base
		take_address (n);
		tcast (n, pointer_to (ref->base));
		return;
	}
	if (!(ref->base->qual & Q_CONST))
		error_at (n->tok, "a reference (not const) to what is not an lvalue");
	if (!is_scalar (decay (ref->base)))
		error_at (n->tok, "a const reference to a class rvalue is not supported");
	m = xalloc (sizeof *m);
	*m = *n;
	m->next = 0;
	memset (n, 0, sizeof *n);
	n->kind = ND_TMPREF;
	n->tok = m->tok;
	n->lhs = m;
	n->ty = array_of (unqualified (ref->base), 1);	// (T [1]) { v }, which decays
	n->next = next;
}

static Node *copy_node (Node *n)
{
	Node *c = xalloc (sizeof *c);
	*c = *n;
	c->next = 0;
	return c;
}

// C++: a call's default arguments, reference arguments, and a reference
// result (args typed, n->ty the result)
static void finish_call (Node *n, Ty *f)
{
	Node *a, **p = &n->args;
	int i = 0;
	for (a = n->args; a; a = a->next, i++)
		p = &a->next;
	for (; i < f->nparams && f->defaults && f->defaults[i]; i++) {
		*p = copy_node (f->defaults[i]);
		p = &(*p)->next;
	}
	if (i < f->nparams && !f->oldstyle)
		error_at (n->tok, "too few arguments");
	for (a = n->args, i = 0; a && i < f->nparams; a = a->next, i++)
		if (f->params[i]->kind == T_REF)
			bind_ref (a, f->params[i]);
		else
			conv (a, f->params[i]);
	n->ty = unqualified (f->base);
	if (n->ty->kind == T_REF)
		deref_ref (n);
}

// a call of a constructor or destructor on the object at ADDR
static Node *special_call (Method *m, Node *addr, Node *args, Token *at)
{
	Node *call = mk (ND_CALL, at), *fn = mk (ND_IDENT, m->cname), *a;
	fn->ty = m->fty;
	fn->tok = make_ident (at, xstrndup (m->cname->loc, m->cname->len));
	addr->next = args;
	call->lhs = fn;
	call->args = addr;
	for (a = args; a; a = a->next)
		expr (a);
	finish_call (call, m->fty);
	return mk_stmt (call);
}

static Node *var_addr (Token *var, Token *at)
{
	Node *n = mk (ND_IDENT, make_ident (at, xstrndup (var->loc, var->len)));
	expr (n);
	take_address (n);
	return n;
}

static Node *member_addr (Field *f, Token *at)
{
	Node *n = mk (ND_MEMBER, at);
	n->lhs = mk_this (at);
	n->op = make_ident (at, "->");
	n->name = f->name;
	expr (n);
	take_address (n);
	return n;
}

static Node *dtor_call (Obj *o, Token *at)
{
	return special_call (o->cls->dtor, o->member ? member_addr (o->member, at) : var_addr (o->var, at), 0, at);
}

// the destructor calls on leaving scopes up to one of KIND (CS_LOOP for
// continue; CS_SWITCH for break also stops at a loop)
static Node *cleanups (int kind, Token *at)
{
	Node head = {0}, *cur = &head;
	CScope *s;
	Obj *o;
	for (s = cs; s; s = s->up) {
		if (kind == CS_SWITCH && (s->kind == CS_LOOP || s->kind == CS_SWITCH))
			break;
		if (kind == CS_LOOP && s->kind == CS_LOOP)
			break;
		for (o = s->objs; o; o = o->next)
			cur = cur->next = dtor_call (o, at);
		if (s->kind == CS_FUNC)
			break;
	}
	return head.next;
}

static void push_cs (int kind)
{
	CScope *s = xalloc (sizeof *s);
	s->kind = kind;
	s->up = cs;
	cs = s;
}

static Node *pop_cs (Token *at)
{
	Node head = {0}, *cur = &head;
	Obj *o;
	for (o = cs->objs; o; o = o->next)
		cur = cur->next = dtor_call (o, at);
	cs = cs->up;
	return head.next;
}

// does any scope up to the function's hold an object to destroy?
static int live_objects (void)
{
	CScope *s;
	for (s = cs; s; s = s->up) {
		if (s->objs)
			return 1;
		if (s->kind == CS_FUNC)
			break;
	}
	return 0;
}

// ---- C++: mangling and overloads (M4) ----

// operator names in C: operator[] as op_idx
static const char *op_names[][2] = {
	{ "[]", "idx" }, { "()", "call" }, { "!", "not" }, { "~", "compl" }, { "+", "add" }, { "-", "sub" },
	{ "*", "mul" }, { "/", "div" }, { "%", "mod" }, { "&", "and" }, { "|", "or" }, { "^", "xor" },
	{ "=", "assign" }, { "+=", "addeq" }, { "-=", "subeq" }, { "*=", "muleq" }, { "/=", "diveq" },
	{ "%=", "modeq" }, { "&=", "andeq" }, { "|=", "oreq" }, { "^=", "xoreq" }, { "<<", "shl" },
	{ ">>", "shr" }, { "<<=", "shleq" }, { ">>=", "shreq" }, { "==", "eq" }, { "!=", "ne" }, { "<", "lt" },
	{ ">", "gt" }, { "<=", "le" }, { ">=", "ge" }, { "&&", "land" }, { "||", "lor" }, { "++", "inc" },
	{ "--", "dec" }, { "->", "arrow" }, { ",", "comma" }, { 0, 0 }
};

static int is_operator_name (Token *t)
{
	return t->len > 8 && !strncmp (t->loc, "operator", 8) && !is_ident_char (t->loc[8]);
}

// a function's name as part of a C name
static char *c_name (Token *name)
{
	int i, n;
	const char *op;
	if (!is_operator_name (name))
		return xstrndup (name->loc, name->len);
	op = name->loc + 8;
	n = name->len - 8;
	for (i = 0; op_names[i][0]; i++)
		if ((int)strlen (op_names[i][0]) == n && !strncmp (op, op_names[i][0], n))
			return xformat ("op_%s", op_names[i][1]);
	error_at (name, "'%.*s' is not supported", name->len, name->loc);
	return 0;
}

// plan section 3.4: Itanium's letters, kept short
static void mangle_ty (Buf *b, Ty *t, Token *at)
{
	int i;
	if (t->qual & Q_CONST)
		buf_char (b, 'K');
	switch (t->kind) {
	case T_VOID: buf_char (b, 'v'); return;
	case T_BOOL: buf_char (b, 'b'); return;
	case T_CHAR: buf_char (b, 'c'); return;
	case T_SCHAR: buf_char (b, 'a'); return;
	case T_UCHAR: buf_char (b, 'h'); return;
	case T_SHORT: buf_char (b, 's'); return;
	case T_USHORT: buf_char (b, 't'); return;
	case T_INT: buf_char (b, 'i'); return;
	case T_UINT: buf_char (b, 'j'); return;
	case T_LONG: buf_char (b, 'l'); return;
	case T_ULONG: buf_char (b, 'm'); return;
	case T_LLONG: buf_char (b, 'x'); return;
	case T_ULLONG: buf_char (b, 'y'); return;
	case T_FLOAT: buf_char (b, 'f'); return;
	case T_DOUBLE: buf_char (b, 'd'); return;
	case T_LDOUBLE: buf_char (b, 'e'); return;
	case T_PTR: buf_char (b, 'P'); mangle_ty (b, t->base, at); return;
	case T_REF: buf_char (b, 'R'); mangle_ty (b, t->base, at); return;
	case T_ARRAY: buf_fmt (b, "A%d_", t->len < 0 ? 0 : t->len); mangle_ty (b, t->base, at); return;
	case T_FUNC:
		buf_char (b, 'F');
		mangle_ty (b, t->base, at);
		if (!t->nparams && !t->variadic)
			buf_char (b, 'v');
		for (i = 0; i < t->nparams; i++)
			mangle_ty (b, t->params[i], at);
		if (t->variadic)
			buf_char (b, 'z');
		buf_char (b, 'E');
		return;
	case T_STRUCT: case T_UNION: case T_ENUM: {
		Ty *d = t->def;
		if (d->tag)
			buf_fmt (b, "%d%.*s", d->tag->len, d->tag->len, d->tag->loc);
		else if (d->name)
			buf_fmt (b, "%d%s", (int)strlen (d->name), d->name);
		else
			error_at (at, "a parameter of a type without a name cannot be mangled");
		return;
	}
	}
	error_at (at, "a type that cannot be mangled");
}

// PREFIX__codes, the codes of F's parameters from FROM on (after `this`)
static Token *mangled (Token *at, const char *prefix, Ty *f, int from)
{
	Buf b = {0};
	int i;
	buf_fmt (&b, "%s__", prefix);
	if (f->nparams <= from && !f->variadic)
		buf_char (&b, 'v');
	for (i = from; i < f->nparams; i++)
		mangle_ty (&b, f->params[i], at);
	if (f->variadic)
		buf_char (&b, 'z');
	return make_ident (at, b.p);
}

static int same_params (Ty *a, Ty *b, int skip_a, int skip_b)
{
	int i;
	if (a->nparams - skip_a != b->nparams - skip_b || a->variadic != b->variadic)
		return 0;
	for (i = 0; i < a->nparams - skip_a; i++)
		if (!same_type (a->params[i + skip_a], b->params[i + skip_b]))
			return 0;
	return 1;
}

// how well argument A converts to a parameter of type TO (plan section
// 3.3), lower being better: R_EXACT; R_QUAL, exact but with const added
// (int& is better than const int&, char* than const char*); R_LITERAL, a
// string literal as char *; R_PROMOTE; R_CONVERT; R_ELLIPSIS. -1: not at all
enum { R_EXACT, R_QUAL, R_LITERAL, R_PROMOTE, R_CONVERT, R_ELLIPSIS };

static int rank_val (Node *a, Ty *to)
{
	Ty *from = unqualified (decay (a->ty));
	to = unqualified (to);
	if (same_type (from, to))
		return R_EXACT;
	if (is_ptr (to)) {
		int lost, added;
		if (is_null_const (a))
			return R_CONVERT;
		if (!is_ptr (from))
			return -1;
		lost = from->base->qual & ~to->base->qual & (Q_CONST | Q_VOLATILE);
		added = to->base->qual & ~from->base->qual & (Q_CONST | Q_VOLATILE);
		if (lost && a->kind == ND_STR && !added && same_type (unqualified (from->base), unqualified (to->base)))
			return R_LITERAL;	// C++98's conversion of a literal to char *
		if (same_type (unqualified (from->base), unqualified (to->base)))
			return lost ? -1 : added ? R_QUAL : R_EXACT;
		if (to->base->kind == T_VOID && from->base->kind != T_FUNC)
			return lost ? -1 : R_CONVERT;
		if (strictly_derives (class_of (from->base), class_of (to->base)))
			return lost ? -1 : R_CONVERT;	// derived to base
		return -1;
	}
	if (to->kind == T_BOOL && is_ptr (from))
		return R_CONVERT;
	if (is_arith (from) && is_arith (to)) {
		if (to->kind == T_ENUM)
			return -1;
		if (to->kind == T_INT && (from->kind == T_BOOL || from->kind == T_CHAR || from->kind == T_SCHAR ||
		                          from->kind == T_UCHAR || from->kind == T_SHORT || from->kind == T_USHORT))
			return R_PROMOTE;
		if (from->kind == T_ENUM && same_type (promote (from), to))
			return R_PROMOTE;
		if (from->kind == T_FLOAT && to->kind == T_DOUBLE)
			return R_PROMOTE;
		return R_CONVERT;
	}
	return -1;
}

static int rank_arg (Node *a, Ty *to)
{
	if (to->kind == T_REF) {
		Ty *b = to->base;
		if (a->lvalue && !a->bitfield && same_type (unqualified (a->ty), unqualified (b)) &&
		    !(a->ty->qual & ~b->qual & (Q_CONST | Q_VOLATILE)))
			return (b->qual & ~a->ty->qual & (Q_CONST | Q_VOLATILE)) ? R_QUAL : R_EXACT;
		if (a->lvalue && !a->bitfield && strictly_derives (class_of (a->ty), class_of (b)) &&
		    !(a->ty->qual & ~b->qual & (Q_CONST | Q_VOLATILE)))
			return R_CONVERT;	// a derived object to a base's reference
		if (!(b->qual & Q_CONST))
			return -1;
		{
			int r = rank_val (a, b);
			return r == R_EXACT ? R_QUAL : r;
		}
	}
	return rank_val (a, to);
}

// a candidate function: its type, and how many parameters (this) come
// before the arguments
typedef struct { Ty *f; int skip; Sym *y; Method *m; } Cand;

#define MAXARGS 32

// the candidate's ranks for ARGS, or 0 if it cannot be called with them
static int ranks (Cand *c, Node *args, int *r)
{
	Ty *f = c->f;
	Node *a;
	int i = 0, k;
	for (a = args; a; a = a->next, i++) {
		if (i >= MAXARGS)
			return 0;
		if (i + c->skip < f->nparams) {
			r[i] = rank_arg (a, f->params[i + c->skip]);
			if (r[i] < 0)
				return 0;
		} else if (f->variadic)
			r[i] = R_ELLIPSIS;
		else
			return 0;
	}
	for (k = i + c->skip; k < f->nparams; k++)
		if (!f->defaults || !f->defaults[k])
			return 0;
	return 1;
}

static int better (int *a, int *b, int n)
{
	int i, strictly = 0;
	for (i = 0; i < n; i++) {
		if (a[i] > b[i])
			return 0;
		if (a[i] < b[i])
			strictly = 1;
	}
	return strictly;
}

static char *cand_str (Cand *c, const char *name)
{
	Ty *f = c->f;
	Buf b = {0};
	int i;
	buf_fmt (&b, "%s(", name);
	for (i = c->skip; i < f->nparams; i++) {
		char *s = ty_str (f->params[i], "");
		buf_fmt (&b, "%s%s", i > c->skip ? ", " : "", s ? s : "?");
	}
	if (f->variadic)
		buf_str (&b, f->nparams > c->skip ? ", ..." : "...");
	buf_str (&b, ")");
	return b.p;
}

// overload resolution among N candidates for ARGS (typed): the best one's
// index; or -1 if QUIET and none fits; otherwise an error naming them
static int choose (Cand *c, int n, Node *args, Token *at, const char *name, int quiet)
{
	static int r[64][MAXARGS];
	int ok[64], i, j, best = -1, nargs = 0;
	Node *a;
	Buf why = {0};
	for (a = args; a; a = a->next)
		nargs++;
	if (n > 64)
		n = 64;
	for (i = 0; i < n; i++) {
		ok[i] = ranks (&c[i], args, r[i]);
		if (ok[i] && (best < 0 || better (r[i], r[best], nargs)))
			best = i;
	}
	if (best >= 0) {
		for (j = 0; j < n; j++)
			if (j != best && ok[j] && !better (r[best], r[j], nargs))
				break;
		if (j == n)
			return best;
	}
	if (best < 0 && quiet)
		return -1;
	for (i = 0; i < n; i++)
		buf_fmt (&why, "\n    %s%s", cand_str (&c[i], name), ok[i] ? "" : " (does not fit)");
	error_at (at, "%s call to '%s'; the candidates:%s", best < 0 ? "no fitting" : "an ambiguous", name, why.p);
	return -1;
}

// the functions (S_FUNC) named NAME in the innermost scope from S that has
// the name
static int func_set (Scope *s, Token *name, Cand *c, int max)
{
	int n = 0;
	Sym *y;
	for (; s; s = s->up) {
		for (y = s->syms; y; y = y->next)
			if (same_tok (y->name, name)) {
				if (y->kind != S_FUNC)
					return n;
				if (n < max) {
					c[n].f = y->ty;
					c[n].skip = 0;
					c[n].y = y;
					c[n].m = 0;
					n++;
				}
			}
		if (n)
			return n;
	}
	return 0;
}

// a class's member functions named NAME (or its constructors)
static int method_set (Class *k, Token *name, int ctors, Cand *c, int max)
{
	Method *m;
	int n = 0;
	for (m = k->methods; m && n < max; m = m->next)
		if (ctors ? m->is_ctor : !m->is_ctor && !m->is_dtor && same_tok (m->name, name)) {
			c[n].f = m->fty;
			c[n].skip = m->is_static ? 0 : 1;
			c[n].y = 0;
			c[n].m = m;
			n++;
		}
	return n;
}

static Token *cname_tok (Token *at, Token *cname)
{
	return make_ident (at, xstrndup (cname->loc, cname->len));
}

// a constructor of class K, chosen for ARGS, called on the object at ADDR;
// 0 if QUIET and none fits
static Node *ctor_call (Class *k, Node *addr, Node *args, Token *at, int quiet)
{
	Cand c[64];
	Node *a, *call, *fn;
	int n = method_set (k, 0, 1, c, 64), i;
	Method *m;
	for (a = args; a; a = a->next)
		expr (a);
	if (!n)
		error_at (at, "'%.*s' has no constructor", k->name->len, k->name->loc);
	i = choose (c, n, args, at, xformat ("%.*s::%.*s", k->name->len, k->name->loc, k->name->len, k->name->loc), quiet);
	if (i < 0)
		return 0;
	m = c[i].m;
	call = mk (ND_CALL, at);
	fn = mk (ND_IDENT, cname_tok (at, m->cname));
	fn->ty = m->fty;
	addr->next = args;
	call->lhs = fn;
	call->args = addr;
	finish_call (call, m->fty);
	return mk_stmt (call);
}

// ::name (file scope) or X::name (X's members, then where X's nested types'
// enumerators are)
static Sym *find_qualified (Node *n)
{
	Sym *y;
	if (equal (n->qual, "::"))
		return find_in (file_scope, n->tok);
	y = find_in (class_named (n->qual)->scope, n->tok);
	return y ? y : find_sym (n->tok);
}

// C++: a name that is a class's member: a data member as this->name, a
// static member as its global. 1 if N was one (and is typed).
static int cxx_ident (Node *n)
{
	Sym *y = n->qual ? find_qualified (n) : find_sym (n->tok);
	Token *name = n->tok;
	Node *next = n->next;
	if (!y)
		return 0;
	switch (y->kind) {
	case S_FIELD:
		if (!cur_class)
			error_at (name, "a data member used without an object");
		memset (n, 0, sizeof *n);
		n->kind = ND_MEMBER;
		n->tok = name;
		n->lhs = mk_this (name);
		n->op = make_ident (name, "->");
		n->name = name;
		n->next = next;
		member (n);
		return 1;
	case S_SVAR:
		n->tok = make_ident (name, xstrndup (y->cname->loc, y->cname->len));
		n->qual = 0;
		n->ty = y->ty;
		n->lvalue = 1;
		if (y->has_val) {
			n->is_const = 1;
			n->cvar = 1;
			n->val = y->val;
		}
		deref_ref (n);
		return 1;
	case S_METHOD:
		if (!y->m->is_static)
			error_at (name, "a member function not called (pointers to members are not supported)");
		n->tok = make_ident (name, xstrndup (y->m->cname->loc, y->m->cname->len));
		n->qual = 0;
		n->ty = y->m->fty;
		return 1;
	case S_FUNC: {
		Cand c[2];
		if (func_set (n->qual ? file_scope : sc, name, c, 2) > 1)
			error_at (name, "the address of an overloaded function is not supported");
		if (y->cname)
			n->tok = cname_tok (name, y->cname);
		n->qual = 0;
		n->ty = y->ty;
		return 1;
	}
	}
	return 0;
}

static Spec *class_spec (struct Class *c, Token *at);
static void append (Node **list, Node *n);
static Type *ptr_type (Type *base);

// a hidden local of the function, of class K (or a pointer to one):
// __c4x_tN, its name
static Token *hidden_local (Class *k, int ptr, Token *at)
{
	Decl *x;
	Node *d;
	Ty *t = ptr ? pointer_to (k->rec) : k->rec;
	if (!fn_scope)
		error_at (at, ptr ? "a virtual call on this object outside a function is not supported" :
		                    "a temporary outside a function is not supported");
	x = xalloc (sizeof *x);
	x->spec = class_spec (k, at);
	x->spec->rty = k->rec;
	x->type = ptr ? ptr_type (&sema_base) : &sema_base;
	x->name = make_ident (at, xformat ("__c4x_t%d", n_temps++));
	x->tok = at;
	x->first_of_spec = 1;
	x->rty = t;
	add_sym (fn_scope, x->name, S_VAR, t);
	d = mk (ND_DECL, at);
	d->decl = x;
	append (&fn_temps, d);
	return x->name;
}

// can N be evaluated twice? (no calls, assignments, ++ or --)
static int simple (Node *n)
{
	switch (n->kind) {
	case ND_IDENT: case ND_NUM:
		return 1;
	case ND_MEMBER: case ND_CAST: case ND_TCAST:
		return simple (n->lhs);
	case ND_PREFIX:
		return (equal (n->op, "*") || equal (n->op, "&")) && simple (n->lhs);
	case ND_INDEX:
		return simple (n->lhs) && simple (n->rhs);
	}
	return 0;
}

// a copy of a simple N (typed)
static Node *clone (Node *n)
{
	Node *c;
	if (!n)
		return 0;
	c = copy_node (n);
	c->lhs = clone (n->lhs);
	c->rhs = clone (n->rhs);
	return c;
}

// a call of a virtual M on OBJ (a pointer, typed) with N's arguments:
// through its vtable, ((X__vt *) obj->_vp)->slot (obj, args); OBJ is kept
// in a hidden local first unless it is simple
static void virtual_call (Node *n, Method *m, Node *obj)
{
	Class *c = ptr_class (obj->ty);
	Slot *s = m->slot;
	Token *at = n->tok;
	Node *vp, *fn, *pre = 0;
	if (!c || !c->poly)
		error_at (at, "a virtual call on what is not a pointer to a polymorphic class");
	if (!simple (obj)) {
		Token *t = hidden_local (c, 1, at);
		Node *lhs = mk_ident (at, xstrndup (t->loc, t->len));
		expr (lhs);
		pre = mk (ND_ASSIGN, at);
		pre->op = make_ident (at, "=");
		pre->lhs = lhs;
		pre->rhs = obj;
		pre->ty = lhs->ty;
		obj = clone (lhs);
	}
	vp = mk (ND_MEMBER, at);
	vp->lhs = clone (obj);
	vp->op = make_ident (at, "->");
	vp->name = make_ident (at, "_vp");
	expr (vp);
	if (c != c->root)
		tcast (vp, pointer_to (qualified (c->vt, Q_CONST)));
	fn = mk (ND_MEMBER, at);
	fn->lhs = vp;
	fn->op = make_ident (at, "->");
	fn->name = s->name;
	expr (fn);
	n->lhs = fn;
	obj->next = n->args;
	n->args = obj;
	finish_call (n, s->fty);
	if (pre) {
		Node *call = copy_node (n), *next = n->next;
		memset (n, 0, sizeof *n);
		n->kind = ND_COMMA;
		n->tok = at;
		n->lhs = pre;
		n->rhs = call;
		n->ty = call->ty;
		n->next = next;
	}
}

// a member function call, obj.f(...), p->f(...), f(...) in a member
// function, or X::f(...): to Class__f (this, ...), or through the vtable
// when f is virtual (and not called as X::f). 0 if N is not one.
static int method_call (Node *n)
{
	Node *f = n->lhs, *obj = 0, *a;
	Method *m = 0;
	Sym *y = 0;
	Cand cs[64];
	int nc, i;
	if (f->kind == ND_MEMBER) {
		Ty *o;
		Class *c;
		expr (f->lhs);
		o = f->lhs->ty;
		if (equal (f->op, "->")) {
			o = decay (o);
			if (!is_ptr (o))
				return 0;
			o = o->base;
		}
		c = class_of (o);
		if (!c || !(y = find_in (c->scope, f->name)) || y->kind != S_METHOD)
			return 0;
		m = y->m;
		obj = f->lhs;
		if (!equal (f->op, "->")) {
			if (!obj->lvalue)
				error_at (obj->tok, "a member function called on a class rvalue is not supported");
			take_address (obj);
		}
	} else if (f->kind == ND_IDENT) {
		if (f->qual && !equal (f->qual, "::"))
			y = find_in (class_named (f->qual)->scope, f->tok);
		else if (!f->qual)
			y = find_sym (f->tok);
		if (!y || y->kind != S_METHOD)
			return 0;
		m = y->m;
	} else
		return 0;
	if (m->is_ctor || m->is_dtor)
		error_at (f->tok, "constructors and destructors are not called by name");
	// the overloads, and the one these arguments pick
	for (a = n->args; a; a = a->next)
		expr (a);
	nc = method_set (m->owner, m->name, 0, cs, 64);
	i = nc == 1 ? 0 : choose (cs, nc, n->args, f->kind == ND_MEMBER ? f->name : f->tok,
	                          xformat ("%.*s::%s", m->owner->name->len, m->owner->name->loc, c_name (m->name)), 0);
	m = cs[i].m;
	if (f->kind == ND_IDENT && !m->is_static) {
		if (!cur_class)
			error_at (f->tok, "a member function called without an object");
		obj = mk_this (f->tok);
	}
	if (m->is_static)
		obj = 0;
	if (m->slot && obj && !(f->kind == ND_IDENT && f->qual)) {
		virtual_call (n, m, obj);
		return 1;
	}
	n->lhs = mk (ND_IDENT, make_ident (f->tok, xstrndup (m->cname->loc, m->cname->len)));
	n->lhs->ty = m->fty;
	if (obj) {
		obj->next = n->args;
		n->args = obj;
	}
	finish_call (n, m->fty);
	return 1;
}

// an operand's type as arithmetic takes it: C promotes a bit-field
// narrower than int to int, unsigned or not (as c4lc's values are)
static Ty *operand_ty (Node *n)
{
	Ty *t = decay (n->ty);
	if (n->bitfield && is_integer (t) && t->size <= 4 && n->bitfield - 1 < 32)
		return ty_int;
	return t;
}

static void binary (Node *n)
{
	Token *op = n->op;
	Ty *l, *r;
	int ok1, ok2;
	long long a, b;
	expr (n->lhs);
	expr (n->rhs);
	l = operand_ty (n->lhs);
	r = operand_ty (n->rhs);
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
	// C++ promotes an enum to int; gcc's C (the check) may to unsigned int
	if (opt_cplusplus && (l->kind == T_ENUM || r->kind == T_ENUM))
		n->unsure = 1;
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
	Ty *a = operand_ty (n->then), *b = operand_ty (n->els);
	if (opt_cplusplus && a->kind == T_ENUM && same_type (a, b)) {
		// C++: the enum (evPost (..., c ? COMMAND_ID_1 : COMMAND_ID_0));
		// C's int, for enumerators, is what gcc would say
		n->unsure = 1;
		return unqualified (a);
	}
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
	if (opt_cplusplus && method_call (n))
		return;
	if (opt_cplusplus && n->lhs->kind == ND_IDENT && (!n->lhs->qual || equal (n->lhs->qual, "::"))) {
		// a function, of its overloads the one the arguments pick
		Node *fn = n->lhs;
		Cand c[64];
		int k = func_set (fn->qual ? file_scope : sc, fn->tok, c, 64), i;
		if (k) {
			Sym *y;
			for (a = n->args; a; a = a->next)
				expr (a);
			i = k == 1 ? 0 : choose (c, k, n->args, fn->tok, c_name (fn->tok), 0);
			y = c[i].y;
			if (y->cname)
				fn->tok = cname_tok (fn->tok, y->cname);
			fn->ty = y->ty;
			fn->qual = 0;
			finish_call (n, y->ty);
			return;
		}
	}
	if (opt_cplusplus && n->lhs->kind == ND_IDENT && !n->lhs->qual && !find_sym (n->lhs->tok) &&
	    strncmp (n->lhs->tok->loc, "__builtin_", 10))
		error_at (n->lhs->tok, "'%.*s' is not declared (C++ has no implicit declarations)", n->lhs->tok->len,
		          n->lhs->tok->loc);
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
	if (opt_cplusplus)
		finish_call (n, f);
	else
		n->ty = unqualified (f->base);
	// gcc knows its builtins' types (__builtin_va_start's is void); c4cxx not
	if (n->lhs->kind == ND_IDENT && !strncmp (n->lhs->tok->loc, "__builtin_", 10))
		n->unsure = 1;
}

static int anon_path (Node *n, Ty *o);

// C++: a member operator, OBJ.operator OP (ARG), if OBJ is of a class that
// has one; N becomes the call. 0 if not (C's meaning stands).
static int op_call (Node *n, Token *op, Node *obj, Node *arg)
{
	Class *k;
	Node *m, *next = n->next;
	Token *at = n->tok, *name;
	expr (obj);
	k = class_of (obj->ty);
	if (!k)
		return 0;
	name = make_ident (at, xformat ("operator%.*s", op->len, op->loc));
	if (!find_in (k->scope, name))
		return 0;
	m = mk (ND_MEMBER, at);
	m->lhs = obj;
	m->op = make_ident (at, ".");
	m->name = name;
	if (arg)
		arg->next = 0;
	memset (n, 0, sizeof *n);
	n->kind = ND_CALL;
	n->tok = at;
	n->lhs = m;
	n->args = arg;
	n->next = next;
	call (n);
	return 1;
}

// C++: X (args), a temporary: built by its constructor in a hidden local
// of the function, *X__C__... (&__c4x_tN, args)
static void class_temp (Node *n, Class *k)
{
	Node *next = n->next, *call;
	Token *at = n->tok, *t;
	if (k->needs_dtor)
		error_at (at, "a temporary of a class with a destructor is not supported");
	if (!k->ctor)
		error_at (at, "a temporary of a class without a constructor is not supported");
	t = hidden_local (k, 0, at);
	call = ctor_call (k, var_addr (t, at), n->args, at, 0)->lhs;
	memset (n, 0, sizeof *n);
	n->kind = ND_PREFIX;
	n->tok = at;
	n->op = make_ident (at, "*");
	n->lhs = call;
	n->ty = k->rec;
	n->lvalue = 1;
	n->next = next;
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
	if (opt_cplusplus && anon_path (n, o))
		return;
	f = find_field (o, n->name, 0);
	if (!f) {
		Class *c = class_of (o);
		Sym *y = c ? find_in (c->scope, n->name) : 0;
		if (y && y->kind == S_SVAR) {
			// obj.static_member: the global
			Node *next = n->next;
			memset (n, 0, sizeof *n);
			n->kind = ND_IDENT;
			n->tok = make_ident (y->name, xstrndup (y->cname->loc, y->cname->len));
			n->next = next;
			n->ty = y->ty;
			n->lvalue = 1;
			deref_ref (n);
			return;
		}
		if (y && y->kind == S_METHOD)
			error_at (n->name, "a member function not called (pointers to members are not supported)");
		if (c && !y && (y = find_sym (n->name)) && y->kind == S_ENUMC) {
			// obj.kFoo: a nested enum's enumerator (at file scope)
			Node *next = n->next;
			Token *name = n->name;
			memset (n, 0, sizeof *n);
			n->kind = ND_IDENT;
			n->tok = name;
			n->next = next;
			expr (n);
			return;
		}
		error_at (n->name, "no member named '%.*s'", n->name->len, n->name->loc);
	}
	n->ty = qualified (f->ty, o->qual & (Q_CONST | Q_VOLATILE));
	n->lvalue = 1;
	n->bitfield = f->bit_width >= 0 ? f->bit_width + 1 : 0;	// its width, plus one
	if (opt_cplusplus)
		deref_ref (n);
}

// C++: a member inside anonymous structs and unions, which have names in C
// (_u0): obj.x as obj._u0.x. 1 if it was one (and N is typed).
static int anon_path (Node *n, Ty *o)
{
	Field *f;
	for (f = o->def->fields; f; f = f->next)
		if (f->name && same_tok (f->name, n->name))
			return 0;	// its own, which hides a base's (M5: _b is looked through too)
	for (f = o->def->fields; f; f = f->next) {
		if (f->anon && find_field (f->ty, n->name, 0)) {
			Node *in = mk (ND_MEMBER, n->tok);
			in->lhs = n->lhs;
			in->op = n->op;
			in->name = f->name;
			n->lhs = in;
			n->op = make_ident (n->op, ".");
			expr (in);
			n->ty = 0;
			member (n);
			return 1;
		}
	}
	return 0;
}

static Ty *type_name_type (Decl *d)
{
	return decl_type (d->spec, d->type);
}

// ---- C++: new, delete and the runtime, libc4cxx (M5, plan section 3.7) ----

static int rt_declared;		// the runtime's prototypes printed in this unit

static Node *raw (Token *at, char *text)
{
	Node *n = mk (ND_RAW, at);
	n->tok = make_ident (at, text);
	return n;
}

// the runtime's prototypes, once, before the top-level node being typed
static void rt_decls (Token *at)
{
	if (rt_declared)
		return;
	if (!cur_top)
		error_at (at, "new, delete or a vtable here is not supported");
	rt_declared = 1;
	append (&cur_top->pre, raw (at, "void *__c4x_new (unsigned int); "
	                                "void *__c4x_new_vec (unsigned int, unsigned int, void *(*) (void *), int); "
	                                "void __c4x_delete (void *); void __c4x_delete_d (void *, void (*) (void *)); "
	                                "void __c4x_delete_v (void *, int); "
	                                "void __c4x_delete_vec (void *, unsigned int, void (*) (void *)); "
	                                "void __c4x_pure (void);"));
}

// a function type RET (P0, P1, ...), N parameters
static Ty *fn_ty (Ty *ret, int n, Ty *p0, Ty *p1, Ty *p2, Ty *p3)
{
	Ty *f = func_type (ret);
	f->params = xalloc (4 * sizeof (Ty *));
	f->params[0] = p0;
	f->params[1] = p1;
	f->params[2] = p2;
	f->params[3] = p3;
	f->nparams = n;
	return f;
}

static Ty *void_ptr (void)
{
	return pointer_to (ty_void);
}

// a call of the runtime's NAME, of type FTY, with ARGS (typed, in a list)
static Node *rt_call (const char *name, Ty *fty, Node *args, Token *at)
{
	Node *call = mk (ND_CALL, at), *fn = mk_ident (at, name);
	rt_decls (at);
	fn->ty = fty;
	call->lhs = fn;
	call->args = args;
	finish_call (call, fty);
	return call;
}

static Node *list2 (Node *a, Node *b)
{
	a->next = b;
	return a;
}

static Node *num_node (Token *at, long long v)
{
	Node *n = mk (ND_NUM, make_ident (at, xformat ("%lld", v)));
	expr (n);
	return n;
}

// sizeof (struct K)
static Node *class_size (Class *k, Token *at)
{
	Node *n = mk (ND_SIZEOF_TYPE, at);
	Decl *d = xalloc (sizeof *d);
	d->spec = class_spec (k, at);
	d->spec->rty = k->rec;
	d->type = &sema_base;
	d->tok = at;
	d->first_of_spec = 1;
	n->decl = d;
	expr (n);
	return n;
}

// a constructor or destructor M of K as (void *(*) (void *)), or
// (void (*) (void *)), for the runtime's loops
static Node *special_ptr (Method *m, Token *at)
{
	Node *n = mk (ND_IDENT, cname_tok (at, m->cname));
	n->ty = m->fty;
	tcast (n, pointer_to (fn_ty (m->is_ctor ? void_ptr () : ty_void, 1, void_ptr (), 0, 0, 0)));
	return n;
}

// new T, new T (args), new T[n]: the storage from __c4x_new, and a class
// built by its constructor, K__C__codes ((struct K *) __c4x_new (sizeof
// (struct K)), args), which returns it
static void new_expr (Node *n)
{
	Ty *t = type_name_type (n->decl), *pt;
	Class *k = class_of (t);
	Token *at = n->tok;
	Node *next = n->next, *size, *r;
	if (t->kind == T_VOID || t->kind == T_FUNC || t->kind == T_REF || t->size < 0)
		error_at (at, "new of an incomplete type, void, a function or a reference");
	pt = pointer_to (t);
	size = mk (ND_SIZEOF_TYPE, at);
	size->decl = n->decl;
	expr (size);
	if (n->is_array) {
		Node *cnt = n->rhs;
		expr (cnt);
		if (n->functional)
			error_at (at, "new T[n] (...) is not supported");
		if (k && (k->needs_ctor || k->needs_dtor)) {
			// each element by the default constructor; a count before them
			// for delete[] when they have a destructor
			Node *ctor;
			if (k->needs_ctor) {
				Cand c[64];
				int nc = method_set (k, 0, 1, c, 64), i;
				i = choose (c, nc, 0, at, xformat ("%.*s::%.*s", k->name->len, k->name->loc, k->name->len, k->name->loc), 0);
				ctor = special_ptr (c[i].m, at);
			} else {
				ctor = num_node (at, 0);
				tcast (ctor, pointer_to (fn_ty (void_ptr (), 1, void_ptr (), 0, 0, 0)));
			}
			r = rt_call ("__c4x_new_vec", fn_ty (void_ptr (), 4, ty_uint, ty_uint,
			             pointer_to (fn_ty (void_ptr (), 1, void_ptr (), 0, 0, 0)), ty_int),
			             list2 (cnt, list2 (size, list2 (ctor, num_node (at, k->needs_dtor)))), at);
		} else {
			Node *mul = mk (ND_BINARY, at);
			mul->op = make_ident (at, "*");
			mul->lhs = cnt;
			mul->rhs = size;
			expr (mul);
			r = rt_call ("__c4x_new", fn_ty (void_ptr (), 1, ty_uint, 0, 0, 0), mul, at);
		}
		tcast (r, pt);
	} else {
		r = rt_call ("__c4x_new", fn_ty (void_ptr (), 1, ty_uint, 0, 0, 0), size, at);
		tcast (r, pt);
		if (k && k->ctor)
			r = ctor_call (k, r, n->args, at, 0)->lhs;
		else if (n->functional)
			error_at (at, "new T (...) of a type without a constructor is not supported");
	}
	r->next = next;
	*n = *r;
}

// delete p, delete[] p: the destructor, through the vtable when it is
// virtual (__c4x_delete_v reads the slot), then the storage
static void delete_expr (Node *n)
{
	Token *at = n->tok;
	Node *next = n->next, *p = n->lhs, *r;
	Ty *t;
	Class *k;
	expr (p);
	t = decay (p->ty);
	if (!is_ptr (t))
		error_at (at, "delete of what is not a pointer");
	k = class_of (t->base);
	if (n->is_array && k && k->needs_dtor)
		r = rt_call ("__c4x_delete_vec", fn_ty (ty_void, 3, void_ptr (), ty_uint, pointer_to (fn_ty (ty_void, 1, void_ptr (), 0, 0, 0)), 0),
		             list2 (p, list2 (class_size (k, at), special_ptr (k->dtor, at))), at);
	else if (!n->is_array && k && k->dtor && k->dtor->slot)
		r = rt_call ("__c4x_delete_v", fn_ty (ty_void, 2, void_ptr (), ty_int, 0, 0),
		             list2 (p, num_node (at, k->dtor->slot->index)), at);
	else if (!n->is_array && k && k->needs_dtor)
		r = rt_call ("__c4x_delete_d", fn_ty (ty_void, 2, void_ptr (), pointer_to (fn_ty (ty_void, 1, void_ptr (), 0, 0, 0)), 0, 0),
		             list2 (p, special_ptr (k->dtor, at)), at);
	else
		r = rt_call ("__c4x_delete", fn_ty (ty_void, 1, void_ptr (), 0, 0, 0), p, at);
	r->next = next;
	*n = *r;
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
		// C++: const char[N]
		n->ty = array_of (opt_cplusplus ? qualified (ty_char, Q_CONST) : ty_char, len + 1);
		n->lvalue = 1;
		return;
	}
	case ND_IDENT:
		if (opt_cplusplus) {
			if (cxx_ident (n))
				return;
			y = n->qual ? find_qualified (n) : find_sym (n->tok);
		} else
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
			n->ty = opt_cplusplus ? y->ty : ty_int;	// C++: its enum's type
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
		if (opt_cplusplus)
			deref_ref (n);
		return;
	case ND_BINARY:
		if (opt_cplusplus && op_call (n, n->op, n->lhs, n->rhs))
			return;
		binary (n);
		return;
	case ND_ASSIGN:
		if (opt_cplusplus && op_call (n, n->op, n->lhs, n->rhs))
			return;
		expr (n->lhs);
		expr (n->rhs);
		n->ty = unqualified (n->lhs->ty);
		// gcc types an assignment to a bit-field as the field: no typeof
		n->bitfield = n->lhs->bitfield;
		if (opt_cplusplus && equal (n->op, "="))
			conv (n->rhs, n->ty);
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
		if (opt_cplusplus && !equal (n->op, "&") && op_call (n, n->op, n->lhs, 0))
			return;
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
			n->bitfield = n->lhs->bitfield;
		} else {
			n->ty = is_arith (t) ? promote (operand_ty (n->lhs)) : unqualified (decay (t));
			n->cvar = n->lhs->cvar;
			if (n->lhs->is_const && is_integer (n->ty)) {
				long long v = n->lhs->val;
				n->is_const = 1;
				n->val = truncate (equal (n->op, "-") ? -v : equal (n->op, "~") ? ~v : v, n->ty);
			}
		}
		return;
	case ND_POSTFIX:
		if (opt_cplusplus) {
			// x++ is x.operator++ (0)
			Node *zero = mk (ND_NUM, make_ident (n->tok, "0"));
			if (op_call (n, n->op, n->lhs, zero))
				return;
		}
		expr (n->lhs);
		n->ty = unqualified (n->lhs->ty);
		n->bitfield = n->lhs->bitfield;
		return;
	case ND_CAST:
		if (n->functional && class_of (type_name_type (n->decl))) {
			class_temp (n, class_of (type_name_type (n->decl)));
			return;
		}
		if (n->functional && (!n->args || n->args->next))
			error_at (n->tok, "T(...) of a type not a class, without exactly one argument, is not supported");
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
		if (opt_cplusplus && op_call (n, make_ident (n->tok, "[]"), n->lhs, n->rhs))
			return;
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
	case ND_VA_ARG:
		expr (n->lhs);
		n->ty = unqualified (type_name_type (n->decl));
		return;
	case ND_OFFSETOF: {
		Ty *t = type_name_type (n->decl);
		Field *f;
		int off = 0;
		n->ty = ty_uint;
		if ((t->kind == T_STRUCT || t->kind == T_UNION) && (f = find_field (t, n->name, &off)) && f->bit_width < 0) {
			n->is_const = 1;
			n->val = off;
		}
		return;
	}
	case ND_NEW:
		new_expr (n);
		return;
	case ND_DELETE:
		delete_expr (n);
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
		else if (equal (w, "_Bool") || equal (w, "bool")) nbool++;
		else if (equal (w, "wchar_t")) nlong++;	// C++: gcc -m32's long
		else if (equal (w, "float")) nfloat++;
		else if (equal (w, "double")) ndouble++;
		else if (equal (w, "__builtin_va_list")) {
			// gcc -m32's: a char * underneath, but a type of its own
			static Ty *va;
			if (!va) {
				va = new_ty (T_PTR, 4, 4);
				va->base = ty_char;
				va->cname = "__builtin_va_list";
			}
			return va;
		}
		// gcc's _FloatN, which glibc declares functions of: as near as C has
		else if (equal (w, "_Float32")) nfloat++;
		else if (equal (w, "_Float64") || equal (w, "_Float32x")) ndouble++;
		else if (equal (w, "_Float128") || equal (w, "_Float64x")) { ndouble++; nlong++; }
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

// ---- C++: a class's definition ----

static void declaration (Decl *d, int top);
static void function_body (Node *n, Class *c, Method *m);

static void append (Node **list, Node *n)
{
	while (*list)
		list = &(*list)->next;
	*list = n;
}

static Spec *class_spec (Class *c, Token *at)
{
	Spec *s = xalloc (sizeof *s);
	s->kind = c->rec->kind == T_UNION ? SP_UNION : SP_STRUCT;
	s->tag = c->name;
	s->tok = at;
	return s;
}

static Spec *void_spec (Token *at)
{
	Spec *s = xalloc (sizeof *s);
	s->tok = at;
	s->words = xalloc (sizeof (Token *));
	s->words[0] = make_ident (at, "void");
	s->nwords = 1;
	return s;
}

static Type *ptr_type (Type *base)
{
	Type *t = xalloc (sizeof *t);
	t->kind = TY_PTR;
	t->base = base;
	return t;
}

// member function M's declaration in C, from D (its declaration in the
// class, or its definition outside): Class__name, with `this` first
static Decl *lowered (Class *c, Method *m, Decl *d, int storage)
{
	Decl *x = xalloc (sizeof *x);
	Type *f = xalloc (sizeof *f);
	*x = *d;
	x->next = 0;
	x->first_of_spec = 1;
	x->qual = 0;
	x->body = 0;
	x->init = 0;
	x->rty = 0;
	x->is_ctor = x->is_dtor = 0;
	x->cname_done = 1;
	x->name = make_ident (d->tok, xstrndup (m->cname->loc, m->cname->len));
	*f = *d->type;
	if (m->is_ctor) {
		x->spec = class_spec (c, d->tok);
		f->base = ptr_type (&sema_base);
	} else if (m->is_dtor) {
		x->spec = void_spec (d->tok);
		f->base = &sema_base;
	} else {
		Spec *s = xalloc (sizeof *s);
		*s = *d->spec;
		x->spec = s;
	}
	x->spec->storage = storage;
	if (!m->is_static) {
		Decl *t = xalloc (sizeof *t);
		t->spec = class_spec (c, d->tok);
		t->first_of_spec = 1;
		t->type = ptr_type (&sema_base);
		t->name = make_ident (d->tok, "this");
		t->tok = d->tok;
		t->next = d->type->params;
		f->params = t;
		f->oldstyle = 0;
	}
	x->type = f;
	return x;
}

static void add_fields (Class *c, Ty *rec)
{
	Field *f;
	for (f = rec->def->fields; f; f = f->next) {
		if (!f->name)
			continue;
		if (f->anon) {
			if (f->ty->kind == T_STRUCT || f->ty->kind == T_UNION)
				add_fields (c, f->ty);
		} else
			add_sym (c->scope, f->name, S_FIELD, f->ty);
	}
}

// a nested type or typedef, printed before the class (at file scope, under
// its own name, as Watcom-era code expects)
static void hoist (Decl *m)
{
	Node *n = mk (ND_DECL, m->tok);
	Decl *x = xalloc (sizeof *x);
	*x = *m;
	x->next = 0;
	x->first_of_spec = 1;
	n->decl = x;
	if (!cur_top)
		error_at (m->tok, "a nested type here is not supported");
	append (&cur_top->pre, n);
}

static Method *new_method (Class *c, Decl *m, int is_ctor, int is_dtor, int is_static)
{
	Method *me = xalloc (sizeof *me), **p;
	Token *n = c->name;
	me->name = m->name;
	me->decl = m;
	me->is_ctor = is_ctor;
	me->is_dtor = is_dtor;
	me->is_static = is_static;
	me->inline_body = m->body != 0;
	me->owner = c;
	// for now: mangled once its type is known (cxx_record)
	me->cname = make_ident (m->tok, is_ctor ? xformat ("%.*s__C", n->len, n->loc) :
	                                is_dtor ? xformat ("%.*s__D", n->len, n->loc) :
	                                xformat ("%.*s__%s", n->len, n->loc, c_name (m->name)));
	for (p = &c->methods; *p; p = &(*p)->next)
		if (is_dtor && (*p)->is_dtor)
			error_at (m->tok, "a second destructor");
	*p = me;
	if (is_ctor)
		c->ctor = me;
	if (is_dtor)
		c->dtor = me;
	return me;
}

// a constructor or destructor the class needs for its members, the user
// having written none
static Method *implied (Class *c, int is_dtor, Token *at)
{
	Decl *d = xalloc (sizeof *d);
	Type *f = xalloc (sizeof *f);
	f->kind = TY_FUNC;
	f->base = &sema_base;
	d->type = f;
	d->name = c->name;
	d->tok = at;
	d->spec = void_spec (at);
	d->body = mk (ND_BLOCK, at);
	return new_method (c, d, !is_dtor, is_dtor, 0);
}

static int needs (Ty *t, int dtor)
{
	Class *c;
	while (t->kind == T_ARRAY)
		t = t->base;
	c = class_of (t);
	return c && (dtor ? c->needs_dtor : c->needs_ctor);
}

static int has_attr (Attr *a, const char *name);
static int attr_aligned (Attr *a);

// ---- C++: inheritance and virtual functions (M5, plan section 3.2) ----

// the base clause, `: public Base {`: one base, not virtual. Its members
// are the class's until the class's own hide them.
static void base_class (Class *c, Token *t)
{
	Sym *y;
	while (equal (t, "public") || equal (t, "protected") || equal (t, "private"))
		t = t->next;
	if (equal (t, "virtual"))
		error_at (t, "virtual base classes are not supported");
	if (t->kind != TK_IDENT)
		error_at (t, "expected a base class");
	c->base = class_named (t);
	if (!c->base->rec->def->complete)
		error_at (t, "'%.*s' is incomplete as a base class", t->len, t->loc);
	t = t->next;
	if (equal (t, ","))
		error_at (t, "multiple inheritance is not supported");
	if (!equal (t, "{"))
		error_at (t, "expected '{' after the base class");
	for (y = c->base->scope->syms; y; y = y->next) {
		Sym *x = add_sym (c->scope, y->name, y->kind, y->ty);
		x->val = y->val;
		x->has_val = y->has_val;
		x->is_tagname = y->is_tagname;
		x->m = y->m;
		x->cname = y->cname;
		x->c_link = y->c_link;
	}
}

// a member the class has in C only: _b (the base) or _vp (the vtable)
static Decl *hidden_member (Spec *s, Type *t, const char *name, Token *at, Ty *ty, Field **cur)
{
	Decl *d = xalloc (sizeof *d);
	Field *f = xalloc (sizeof *f);
	d->spec = s;
	d->type = t;
	d->name = make_ident (at, name);
	d->tok = at;
	d->first_of_spec = 1;
	d->rty = ty;
	f->name = d->name;
	f->ty = ty;
	f->anon = 1;		// looked through, as an anonymous member is
	f->bit_width = -1;
	*cur = (*cur)->next = f;
	return d;
}

static Field *field_named (Class *c, const char *name)
{
	Field *f;
	for (f = c->rec->def->fields; f; f = f->next)
		if (f->name && equal (f->name, name))
			return f;
	return 0;
}

// the slots: the base's, each overridden where the class declares a
// function of its name and parameters, then the class's new virtuals
static void vtable_slots (Class *c)
{
	Slot head = {0}, *cur = &head, *s, *b;
	Method *me;
	int n = 0;
	if (c->base && c->base->poly)
		for (b = c->base->slots; b; b = b->next) {
			s = xalloc (sizeof *s);
			*s = *b;
			s->next = 0;
			cur = cur->next = s;
			n++;
		}
	for (me = c->methods; me; me = me->next) {
		int declared = (me->decl->spec->storage & ST_VIRTUAL) || me->decl->pure;
		if (me->is_ctor || me->is_static) {
			if (declared)
				error_at (me->decl->tok, "a constructor or static member function declared virtual");
			continue;
		}
		for (s = head.next; s; s = s->next)
			if (me->is_dtor ? s->is_dtor : !s->is_dtor && same_tok (s->m->name, me->name) && same_params (s->fty, me->fty, 1, 1))
				break;
		if (!s && declared) {
			s = xalloc (sizeof *s);
			s->name = me->is_dtor ? make_ident (me->decl->tok, "_dtor") :
			          make_ident (me->decl->tok, xstrndup (me->cname->loc + c->name->len + 2, me->cname->len - c->name->len - 2));
			s->fty = me->fty;
			s->is_dtor = me->is_dtor;
			s->index = n++;
			cur = cur->next = s;
		}
		if (s) {
			s->m = me;
			me->slot = s;
			me->is_virtual = 1;
			me->pure = me->decl->pure;
			// Itanium's key function: the first virtual neither pure nor inline
			if (!c->key && !me->pure && !me->inline_body && !(me->decl->spec->storage & ST_INLINE))
				c->key = me;
		}
	}
	c->slots = head.next;
	// struct X__vt's fields
	{
		Field fh = {0}, *fc = &fh;
		for (s = c->slots; s; s = s->next) {
			Field *f = xalloc (sizeof *f);
			f->name = s->name;
			f->ty = pointer_to (s->fty);
			f->bit_width = -1;
			fc = fc->next = f;
		}
		c->vt->fields = fh.next;
		layout (c->vt, 0);
		c->vt->complete = 1;
	}
}

static char *slot_type (Slot *s, const char *inner)
{
	char *t = ty_str (pointer_to (s->fty), inner);
	if (!t)
		error_at (s->name, "a virtual function of a type that cannot be named in C");
	return t;
}

// struct X__vt { void (*Paint__iic) (struct Widget *, int, int, char); ... };
static char *vt_struct_text (Class *c)
{
	Buf b = {0};
	Slot *s;
	buf_fmt (&b, "struct %.*s__vt {", c->name->len, c->name->loc);
	for (s = c->slots; s; s = s->next)
		buf_fmt (&b, " %s;", slot_type (s, xstrndup (s->name->loc, s->name->len)));
	buf_str (&b, " };");
	return b.p;
}

// [static] const struct X__vt X__vtbl = { (type) Label__Paint__iic, ... };
static char *vtbl_text (Class *c, const char *storage)
{
	Buf b = {0};
	Slot *s;
	buf_fmt (&b, "%sconst struct %.*s__vt %.*s__vtbl = {", storage, c->name->len, c->name->loc, c->name->len, c->name->loc);
	for (s = c->slots; s; s = s->next)
		buf_fmt (&b, " (%s) %.*s,", slot_type (s, ""), s->m->pure ? 10 : s->m->cname->len,
		         s->m->pure ? "__c4x_pure" : s->m->cname->loc);
	buf_str (&b, " };");
	return b.p;
}

// this->_vp = (const struct Root__vt *) &X__vtbl
static Node *set_vp (Class *c, Token *at)
{
	Node *lhs = mk (ND_MEMBER, at), *v = mk_ident (at, xformat ("%.*s__vtbl", c->name->len, c->name->loc)), *asg;
	lhs->lhs = mk_this (at);
	lhs->op = make_ident (at, "->");
	lhs->name = make_ident (at, "_vp");
	expr (lhs);
	v->ty = qualified (c->vt, Q_CONST);
	v->lvalue = 1;
	take_address (v);
	if (c != c->root)
		tcast (v, unqualified (lhs->ty));
	asg = mk (ND_ASSIGN, at);
	asg->op = make_ident (at, "=");
	asg->lhs = lhs;
	asg->rhs = v;
	asg->ty = unqualified (lhs->ty);
	return mk_stmt (asg);
}

static void cxx_record (Spec *sp, Ty *rec, int pack)
{
	Class *c = xalloc (sizeof *c);
	Field head = {0}, *cur = &head, *f;
	Decl kh = {0}, *kc = &kh, *m, *next;
	Method *me;
	int has_virtual = 0;
	c->name = sp->tag;
	c->rec = rec;
	rec->cls = c;
	c->scope = xalloc (sizeof (Scope));
	c->scope->up = sc;
	if (sp->bases)
		base_class (c, sp->bases);
	for (m = sp->members; m; m = m->next)
		if (!m->is_pragma && m->spec && m->type && m->type->kind == TY_FUNC && ((m->spec->storage & ST_VIRTUAL) || m->pure))
			has_virtual = 1;
	c->poly = has_virtual || (c->base && c->base->poly);
	if (c->poly) {
		if (c->base && !c->base->poly)
			error_at (sp->tok, "a class with virtual functions whose base has none is not supported");
		if (!c->name || in_function || !cur_top)
			error_at (sp->tok, "a local or unnamed class with virtual functions is not supported");
		c->root = c->base ? c->base->root : c;
		c->vt = new_ty (T_STRUCT, -1, 1);
		c->vt->def = c->vt;
		c->vt->tag = make_ident (sp->tok, xformat ("%.*s__vt", c->name->len, c->name->loc));
		append (&cur_top->pre, raw (sp->tok, xformat ("struct %.*s__vt;", c->name->len, c->name->loc)));
	}
	if (c->root == c) {
		// const struct X__vt *_vp, first
		Spec *s = xalloc (sizeof *s);
		s->kind = SP_STRUCT;
		s->tag = c->vt->tag;
		s->qual = Q_CONST;
		s->tok = sp->tok;
		s->rty = c->vt;
		kc = kc->next = hidden_member (s, ptr_type (&sema_base), "_vp", sp->tok, pointer_to (qualified (c->vt, Q_CONST)), &cur);
	}
	if (c->base) {
		// struct Base _b, first
		Spec *s = class_spec (c->base, sp->tok);
		s->rty = c->base->rec;
		kc = kc->next = hidden_member (s, &sema_base, "_b", sp->tok, c->base->rec, &cur);
	}
	for (m = sp->members; m; m = next) {
		Spec *ms = m->spec;
		next = m->next;
		m->next = 0;
		if (m->is_pragma) {
			pragma (m->tok);
			kc = kc->next = m;
			continue;
		}
		if (!m->name && !m->width && m->type == 0) {
			if ((ms->kind == SP_STRUCT || ms->kind == SP_UNION) && !ms->tag && ms->has_body) {
				// an anonymous struct or union: named, as c4lc has none
				m->name = make_ident (m->tok, xformat ("_u%d", c->anon++));
				f = xalloc (sizeof *f);
				f->name = m->name;
				f->anon = 1;
				f->bit_width = -1;
				f->ty = m->rty = spec_type (ms);
				cur = cur->next = f;
				kc = kc->next = m;
			} else {
				// a nested type: where the class is
				spec_type (ms);
				hoist (m);
			}
			continue;
		}
		if (ms->storage & ST_TYPEDEF) {
			Decl *x = xalloc (sizeof *x);
			*x = *m;
			x->next = 0;
			declaration (x, 0);
			hoist (x);
			continue;
		}
		if (m->type && m->type->kind == TY_FUNC) {
			if (!c->name)
				error_at (m->tok, "member functions in a class without a name");
			new_method (c, m, m->is_ctor, m->is_dtor, (ms->storage & ST_STATIC) != 0);
			continue;
		}
		if (ms->storage & ST_STATIC) {
			// a static data member: the global Class__name
			Ty *t = decl_type (ms, m->type);
			Sym *y = add_sym (c->scope, m->name, S_SVAR, t);
			Node *n = mk (ND_DECL, m->tok);
			Decl *x = xalloc (sizeof *x);
			Spec *s = xalloc (sizeof *s);
			if (!c->name || !cur_top)
				error_at (m->tok, "static members in a class without a name, or in a function");
			y->cname = make_ident (m->name, xformat ("%.*s__%.*s", c->name->len, c->name->loc, m->name->len, m->name->loc));
			*x = *m;
			*s = *ms;
			x->spec = s;
			x->first_of_spec = 1;
			x->name = y->cname;
			if (m->init) {
				// static const int N = 5: in each unit
				s->storage = ST_STATIC;
				initializer (m->init, t);
				if ((t->qual & Q_CONST) && is_integer (t) && !m->init->is_list && m->init->expr->is_const) {
					y->has_val = 1;
					y->val = truncate (m->init->expr->val, t);
				}
			} else
				s->storage = ST_EXTERN;
			x->rty = t;
			n->decl = x;
			append (&cur_top->post, n);
			continue;
		}
		f = xalloc (sizeof *f);
		f->name = m->name;
		f->bit_width = m->width ? (int)const_value (m->width) : -1;
		f->ty = m->rty = decl_type (ms, m->type);
		cur = cur->next = f;
		kc = kc->next = m;
	}
	if (!head.next && sp->kind == SP_STRUCT && sp->tag) {
		// a class with no data is one byte in C++ (and an empty struct none
		// in GNU C): Blood's CFX and CCheatMgr, g++'s size
		Spec *s = xalloc (sizeof *s);
		s->tok = sp->tok;
		s->words = xalloc (sizeof (Token *));
		s->words[0] = make_ident (sp->tok, "char");
		s->nwords = 1;
		s->rty = ty_char;
		kc = kc->next = hidden_member (s, &sema_base, "_e", sp->tok, ty_char, &cur);
	}
	// the data members are the struct; each declaration group printed from
	// the first that is left
	sp->members = kh.next;
	{
		Decl *prev = 0;
		for (m = sp->members; m; m = m->next) {
			if (m->is_pragma)
				continue;
			m->first_of_spec = !prev || prev->spec != m->spec;
			prev = m;
		}
	}
	rec->fields = head.next;
	layout (rec, has_attr (sp->tag_attrs, "packed") || has_attr (sp->tag_attrs, "__packed__") ? 1 : pack);
	{
		int al = attr_aligned (sp->tag_attrs);
		if (attr_aligned (sp->attrs) > al)
			al = attr_aligned (sp->attrs);
		if (al > rec->align) {
			rec->align = al;
			rec->size = (rec->size + al - 1) / al * al;
		}
	}
	add_fields (c, rec);
	for (f = rec->fields; f; f = f->next) {
		if (f->ty->kind == T_ARRAY && (needs (f->ty, 0) || needs (f->ty, 1)))
			error_at (f->name ? f->name : sp->tok, "arrays of objects with constructors or destructors are not supported yet");
		if (needs (f->ty, 0))
			c->needs_ctor = 1;
		if (needs (f->ty, 1))
			c->needs_dtor = 1;
	}
	if (c->ctor || c->poly)
		c->needs_ctor = 1;	// a polymorphic class's sets _vp
	if (c->dtor)
		c->needs_dtor = 1;
	if (c->needs_ctor && !c->ctor)
		implied (c, 0, sp->tok);
	if (c->needs_dtor && !c->dtor)
		implied (c, 1, sp->tok);
	if (c->methods && (in_function || !cur_top))
		error_at (sp->tok, "a local class with member functions is not supported");
	// prototypes for all, then the bodies written in the class
	for (me = c->methods; me; me = me->next) {
		Decl *x = lowered (c, me, me->decl, me->inline_body ? ST_STATIC : 0);
		Node *n = mk (ND_DECL, me->decl->tok);
		Sym *y;
		Method *o;
		me->fty = decl_type (x->spec, x->type);
		for (o = c->methods; o != me; o = o->next)
			if (o->is_ctor == me->is_ctor && o->is_dtor == me->is_dtor && (me->is_ctor || same_tok (o->name, me->name)) &&
			    same_params (o->fty, me->fty, !o->is_static, !me->is_static))
				error_at (me->decl->tok, "a member function declared twice");
		// its name in C: X__name__codes, X__C__codes, X__D (plan section 3.4)
		if (!me->is_dtor)
			me->cname = mangled (me->decl->tok, xstrndup (me->cname->loc, me->cname->len), me->fty, !me->is_static);
		x->name = cname_tok (x->name, me->cname);
		declaration (x, 1);
		// constructors and destructors are found as the class's (method_set,
		// c->dtor); under the class's name they would hide the class
		if (!me->is_ctor && !me->is_dtor) {
			y = add_sym (c->scope, me->name, S_METHOD, me->fty);
			y->m = me;
		}
		n->decl = x;
		append (&cur_top->post, n);
	}
	if (c->poly) {
		// the vtable's type; the vtable here, static, when no function is
		// its key, or else declared, and defined with the key function
		// (funcdef)
		Slot *s;
		vtable_slots (c);
		append (&cur_top->post, raw (sp->tok, vt_struct_text (c)));
		for (s = c->slots; s; s = s->next)
			if (s->m->pure)
				rt_decls (sp->tok);
		append (&cur_top->post, raw (sp->tok, c->key ? xformat ("extern const struct %.*s__vt %.*s__vtbl;", c->name->len,
		                                                        c->name->loc, c->name->len, c->name->loc) :
		                                               vtbl_text (c, "static ")));
	}
	for (me = c->methods; me; me = me->next) {
		Node *n;
		if (!me->inline_body)
			continue;
		n = mk (ND_FUNCDEF, me->decl->tok);
		n->decl = lowered (c, me, me->decl, ST_STATIC);
		n->body = me->decl->body;
		function_body (n, c, me);
		append (&cur_top->post, n);
	}
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
		long long v = 0, lo = 0, hi = 0;
		int neg = 0;
		for (e = sp->enumers; e; e = e->next) {
			Sym *y;
			if (e->value)
				v = const_value (e->value);
			y = add_sym (sc, e->name, S_ENUMC, opt_cplusplus ? rec : ty_int);
			y->val = v;
			e->val = v;
			if (v < 0)
				neg = 1;
			if (e == sp->enumers || v < lo)
				lo = v;
			if (e == sp->enumers || v > hi)
				hi = v;
			v++;
		}
		rec->is_unsigned_enum = !neg;
		if (opt_short_enums) {
			// gcc -fshort-enums (Watcom's enums): the smallest type that holds
			// the values, unsigned when none is negative
			int s = !neg ? (hi <= 255 ? 1 : hi <= 65535 ? 2 : 4)
			             : (lo >= -128 && hi <= 127 ? 1 : lo >= -32768 && hi <= 32767 ? 2 : 4);
			rec->size = rec->align = s;
		}
		return rec;
	}
	if (opt_cplusplus) {
		cxx_record (sp, rec, pack_cur);
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
		if (y->is_tagname)
			sp->kw = t->kind == T_ENUM ? "enum" : t->kind == T_UNION ? "union" : "struct";
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
	if (t->kind == TY_REF)
		return ref_to (chain (sp, t->base));
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
				if (p->init) {
					// C++: a default argument
					if (!f->defaults)
						f->defaults = xalloc (sizeof (Node *) * (f->nparams + 1));
					expr (p->init->expr);
					f->defaults[n] = p->init->expr;
				}
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

// C++: a function's declaration: another of its overloads, or the same one
// again; named in C by its mangled name, unless its linkage is C
static void cxx_function (Decl *d, Ty *t)
{
	Sym *y, *same = 0;
	int clink = d->cname_done || d->spec->extern_c || equal (d->name, "main");
	// C++'s inline is a definition in each unit that has it; C99's (not
	// extern) is no definition at all: static
	if ((d->spec->storage & ST_INLINE) && !(d->spec->storage & ST_EXTERN) && !d->cname_done)
		d->spec->storage = (d->spec->storage & ~ST_INLINE) | ST_STATIC;
	for (y = sc->syms; y; y = y->next)
		if (same_tok (y->name, d->name) && y->kind == S_FUNC) {
			if (same_params (y->ty, t, 0, 0)) {
				same = y;
				break;
			}
			if (clink && y->c_link)
				error_at (d->name, "two extern \"C\" functions of one name");
		}
	if (same) {
		// default arguments add up over the declarations (Blood's header
		// has them, its definition not)
		Ty *o = same->ty;
		int i;
		if (o->defaults && !t->defaults)
			t->defaults = o->defaults;
		else if (o->defaults)
			for (i = 0; i < t->nparams && i < o->nparams; i++)
				if (!t->defaults[i])
					t->defaults[i] = o->defaults[i];
		same->ty = t;
		y = same;
	} else {
		y = add_sym (sc, d->name, S_FUNC, t);
		y->c_link = clink;
		if (!clink)
			y->cname = mangled (d->name, c_name (d->name), t, 0);
	}
	if (y->cname)
		d->name = cname_tok (d->name, y->cname);
	d->rty = t;
}

// C++: is an object of type T const (a const integer, or an array of
// const), which at file scope has internal linkage?
static int const_object (Ty *t)
{
	while (t->kind == T_ARRAY)
		t = t->base;
	return (t->qual & Q_CONST) && t->kind != T_FUNC;
}

// C: is N a constant expression an object of static storage may be
// initialized with (arithmetic, or an address)?
static int addr_const (Node *n);

static int c_const (Node *n)
{
	if (n->is_const && !n->unsure)
		return 1;
	switch (n->kind) {
	case ND_NUM: case ND_CHAR: case ND_STR: case ND_SIZEOF_TYPE: case ND_SIZEOF_EXPR:
		return 1;
	case ND_IDENT:
		return n->ty && (n->ty->kind == T_ARRAY || n->ty->kind == T_FUNC);
	case ND_PREFIX:
		if (equal (n->op, "&"))
			return addr_const (n->lhs);
		if (equal (n->op, "*") || equal (n->op, "++") || equal (n->op, "--"))
			return 0;
		return c_const (n->lhs);
	case ND_CAST:
		return c_const (n->lhs);
	case ND_BINARY:
		return c_const (n->lhs) && c_const (n->rhs);
	case ND_COND:
		return c_const (n->cond) && c_const (n->then) && c_const (n->els);
	}
	return 0;
}

static int addr_const (Node *n)
{
	switch (n->kind) {
	case ND_IDENT: case ND_STR: case ND_COMPLIT:
		return 1;
	case ND_MEMBER:
		return equal (n->op, ".") ? addr_const (n->lhs) : c_const (n->lhs);
	case ND_INDEX:
		return (addr_const (n->lhs) || c_const (n->lhs)) && c_const (n->rhs);
	case ND_PREFIX:
		return equal (n->op, "*") && c_const (n->lhs);
	}
	return 0;
}

static int list_const (Init *in)
{
	InitItem *it;
	if (!in->is_list)
		return c_const (in->expr);
	for (it = in->items; it; it = it->next)
		if (!list_const (it->init))
			return 0;
	return 1;
}

// C++: a global initialized by what C cannot initialize it with (another
// global's value, a call): assigned in the unit's initializer
static void dynamic_init (Decl *d, Ty *t, int top)
{
	Node *lhs, *asg;
	if (list_const (d->init))
		return;
	if (!top)
		error_at (d->name, "a static local initialized at run time is not supported");
	if (t->kind == T_REF || (d->init->is_list && ((t->kind != T_STRUCT && t->kind != T_UNION) || !ty_str (t, ""))))
		error_at (d->name, "a global initialized at run time by a list (of other than a struct), or a reference, is not supported");
	if (d->spec->qual & Q_CONST)
		d->spec->qual &= ~Q_CONST;	// C would refuse the assignment
	lhs = mk (ND_IDENT, make_ident (d->name, xstrndup (d->name->loc, d->name->len)));
	lhs->ty = unqualified (t);
	lhs->lvalue = 1;
	asg = mk (ND_ASSIGN, d->name);
	asg->op = make_ident (d->name, "=");
	asg->lhs = lhs;
	if (d->init->is_list) {
		// AISTATE batBite = { 7, nBiteClient, ... }: batBite = (AISTATE) { ... }
		Node *c = mk (ND_COMPLIT, d->name);
		c->initz = d->init;
		c->ty = lhs->ty;
		c->lvalue = 1;
		asg->rhs = c;
	} else
		asg->rhs = d->init->expr;
	asg->ty = lhs->ty;
	asg = mk_stmt (asg);
	*init_tail = asg;
	init_tail = &asg->next;
	d->init = 0;
}

// &VAR[I]
static Node *elem_addr (Token *var, int i, Token *at)
{
	Node *n = mk (ND_INDEX, at);
	n->lhs = mk_ident (at, xstrndup (var->loc, var->len));
	n->rhs = num_node (at, i);
	expr (n);
	take_address (n);
	return n;
}

// C++: an object of a class with a constructor or destructor: the call to
// construct it (after its declaration, or in the unit's initializer), and
// its destruction at the end of its scope (or the unit's finalizer)
static void class_object (Decl *d, Ty *t, int top)
{
	Class *c = class_of (t);
	int global = top || (d->spec->storage & ST_STATIC);
	if (t->kind == T_ARRAY && (needs (t, 0) || needs (t, 1))) {
		// a global array of objects: each built by its default constructor
		// in the unit's initializer, and destroyed, last first, after main
		Class *k = class_of (t->base);
		int i;
		if (!top || !k || t->len < 0 || t->len > 4096 || d->init || d->has_args)
			error_at (d->name, "arrays of objects with constructors or destructors are supported only as globals, of one dimension, without initializers");
		for (i = 0; i < t->len; i++) {
			if (k->needs_ctor) {
				Node *call = ctor_call (k, elem_addr (d->name, i, d->tok), 0, d->tok, 0);
				*init_tail = call;
				init_tail = &call->next;
			}
			if (k->needs_dtor) {
				Node *call = special_call (k->dtor, elem_addr (d->name, i, d->tok), 0, d->tok);
				call->next = fini_stmts;
				fini_stmts = call;
			}
		}
		return;
	}
	if (!c)
		return;
	if (c->ctor && (!d->init || !d->init->is_list)) {
		Node *call = 0;
		if (d->init) {
			// X x = v: a constructor of v (the user's copy constructor too),
			// or else a copy
			Node *v = d->init->expr;
			v->next = 0;
			call = ctor_call (c, var_addr (d->name, d->tok), v, d->tok, 1);
			if (call)
				d->init = 0;
			else if (!same_type (unqualified (v->ty), unqualified (t)))
				ctor_call (c, var_addr (d->name, d->tok), v, d->tok, 0);
		} else
			call = ctor_call (c, var_addr (d->name, d->tok), d->args, d->tok, 0);
		if (call && global && !top)
			error_at (d->name, "a static local object with a constructor is not supported");
		if (!call)
			;
		else if (top) {
			*init_tail = call;
			init_tail = &call->next;
		} else
			d->ctor = call;
	} else if (d->has_args)
		error_at (d->name, "constructor arguments for a class without a constructor");
	if (c->needs_dtor) {
		if (global) {
			Obj o = {0};
			Node *call;
			o.cls = c;
			o.var = d->name;
			call = dtor_call (&o, d->tok);
			call->next = fini_stmts;
			fini_stmts = call;
		} else {
			Obj *o = xalloc (sizeof *o);
			o->cls = c;
			o->var = d->name;
			o->next = cs->objs;
			cs->objs = o;
		}
	}
}

static void declaration (Decl *d, int top)
{
	for (; d; d = d->next) {
		Ty *t;
		int member_def = 0;	// T X::s = v (external, const or not)
		if (!d->name) {
			// struct x { ... }; and enum { ... };
			spec_type (d->spec);
			continue;
		}
		if (opt_cplusplus && d->qual) {
			// T X::s = v: the static member's definition, as its global
			Sym *y = find_in (class_named (d->qual)->scope, d->name);
			if (!y || y->kind != S_SVAR)
				error_at (d->name, "'%.*s' is not a static data member of '%.*s'", d->name->len, d->name->loc,
				          d->qual->len, d->qual->loc);
			d->name = make_ident (d->name, xstrndup (y->cname->loc, y->cname->len));
			d->qual = 0;
			member_def = 1;
			// `static char *X::s = "On"` (gamemenu.cpp): Watcom took the
			// static; the member's linkage is the class's
			d->spec->storage &= ~ST_STATIC;
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
		if (opt_cplusplus && t->kind == T_FUNC) {
			cxx_function (d, t);
			continue;
		}
		if (opt_cplusplus && d->has_args && !class_of (t)) {
			// int x(5)
			if (!d->args || d->args->next)
				error_at (d->name, "a scalar initialized with other than one value");
			d->init = xalloc (sizeof (Init));
			d->init->tok = d->args->tok;
			d->init->expr = d->args;
			d->has_args = 0;
			d->args = 0;
		}
		if (d->init) {
			initializer (d->init, t);
			if (t->kind == T_ARRAY && t->len < 0)
				t = array_of (t->base, init_length (d->init, t->base));
			if (opt_cplusplus && t->kind == T_REF && !d->init->is_list)
				bind_ref (d->init->expr, t);
			else if (opt_cplusplus && !d->init->is_list)
				conv (d->init->expr, t);
		}
		{
			Sym *old = 0;
			Scope *s;
			for (s = sc, old = 0; s && !old; s = 0) {
				Sym *y;
				for (y = s->syms; y; y = y->next)
					if (same_tok (y->name, d->name)) { old = y; break; }
			}
			// C++: a const object at file scope is internal (static), unless
			// declared extern before
			if (opt_cplusplus && top && !member_def && t->kind != T_FUNC && const_object (t) &&
			    !(d->spec->storage & (ST_EXTERN | ST_STATIC)) && !(old && old->kind == S_VAR))
				d->spec->storage |= ST_STATIC;
			// a declaration again in the same scope: the later one, with
			// any length it learned, stands
			if (opt_cplusplus && old && old->kind == S_FUNC && t->kind == T_FUNC && !same_type (old->ty, t))
				error_at (d->name, "overloaded functions are not supported yet (M4)");
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
		if (opt_cplusplus && t->kind != T_FUNC && !(d->spec->storage & ST_EXTERN))
			class_object (d, t, top);
		if (opt_cplusplus && d->init && (top || (d->spec->storage & ST_STATIC)) && t->kind != T_FUNC)
			dynamic_init (d, t, top);
	}
}

// ---- statements ----

static void stmt (Node *n);

static void stmts (Node *n)
{
	for (; n; n = n->next)
		stmt (n);
}

// a loop's or switch's body: break and continue destroy what is inside
static void body_of (Node *n, int kind)
{
	push_cs (kind);
	stmt (n);
	pop_cs (n->tok);
}

static void stmt (Node *n)
{
	switch (n->kind) {
	case ND_PRAGMA:
		pragma (n->tok);
		return;
	case ND_BLOCK:
		push_scope ();
		push_cs (CS_BLOCK);
		stmts (n->body);
		n->cleanup = pop_cs (n->tok);
		pop_scope ();
		return;
	case ND_NULL:
		return;
	case ND_BREAK:
		n->cleanup = cleanups (CS_SWITCH, n->tok);
		return;
	case ND_CONTINUE:
		n->cleanup = cleanups (CS_LOOP, n->tok);
		return;
	case ND_GOTO:
		if (live_objects ())
			error_at (n->tok, "a goto where objects with destructors are in scope is not supported");
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
	case ND_WHILE:
		expr (n->cond);
		body_of (n->body, CS_LOOP);
		return;
	case ND_SWITCH:
		expr (n->cond);
		body_of (n->body, CS_SWITCH);
		return;
	case ND_DO:
		body_of (n->body, CS_LOOP);
		expr (n->cond);
		return;
	case ND_FOR:
		push_scope ();
		push_cs (CS_LOOP);
		if (n->decl)
			declaration (n->decl, 0);
		if (cs->objs)
			error_at (n->tok, "an object with a destructor declared in a for is not supported");
		if (n->init_expr)
			expr (n->init_expr);
		if (n->cond)
			expr (n->cond);
		if (n->inc)
			expr (n->inc);
		stmt (n->body);
		pop_cs (n->tok);
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
		if (opt_cplusplus && cur_method && cur_method->is_ctor) {
			if (n->lhs)
				error_at (n->tok, "a constructor returns no value");
			n->lhs = mk_this (n->tok);
		}
		if (n->lhs)
			expr (n->lhs);
		if (opt_cplusplus) {
			if (n->lhs && cur_ret && cur_ret->kind == T_REF)
				bind_ref (n->lhs, cur_ret);
			else if (n->lhs && cur_ret)
				conv (n->lhs, cur_ret);
			n->cleanup = cleanups (CS_FUNC, n->tok);
			n->ty = cur_ret;
		}
		return;
	}
	error_at (n->tok, "sema: not a statement (%d)", n->kind);
}

// C++: a constructor's members, in declaration order: from its init list,
// or by their own constructors
static Node *ctor_inits (Class *c, MemInit *inits, Token *at)
{
	Node head = {0}, *cur = &head;
	MemInit *mi, *bi = 0;
	Field *f;
	for (mi = inits; mi; mi = mi->next) {
		if (c->base && same_tok (mi->name, c->base->name)) {
			bi = mi;
			continue;
		}
		for (f = c->rec->def->fields; f; f = f->next)
			if (f->name && !f->anon && same_tok (f->name, mi->name))
				break;
		if (!f)
			error_at (mi->name, "'%.*s' is neither a member nor the base of '%.*s'",
			          mi->name->len, mi->name->loc, c->name->len, c->name->loc);
	}
	// the base first, by the init list's constructor or its default one;
	// then _vp, so that virtual calls from here on are the class's own
	if (c->base && c->base->ctor)
		cur = cur->next = ctor_call (c->base, member_addr (field_named (c, "_b"), at), bi ? bi->args : 0, bi ? bi->name : at, 0);
	else if (bi && bi->args)
		error_at (bi->name, "'%.*s' has no constructor", bi->name->len, bi->name->loc);
	if (c->poly)
		cur = cur->next = set_vp (c, at);
	for (f = c->rec->def->fields; f; f = f->next) {
		Class *fc = class_of (f->ty);
		if (!f->name || f->anon)
			continue;
		for (mi = inits; mi; mi = mi->next)
			if (same_tok (f->name, mi->name))
				break;
		if (fc && fc->needs_ctor)
			cur = cur->next = ctor_call (fc, member_addr (f, at), mi ? mi->args : 0, mi ? mi->name : at, 0);
		else if (mi) {
			Node *lhs = mk (ND_MEMBER, mi->name), *a = mi->args, *asg;
			if (a && a->next)
				error_at (mi->name, "a member initialized with more than one value");
			if (!a) {
				a = mk (ND_NUM, make_ident (mi->name, "0"));
				expr (a);
			}
			lhs->lhs = mk_this (mi->name);
			lhs->op = make_ident (mi->name, "->");
			lhs->name = f->name;
			if (f->ty->kind == T_REF) {
				// the pointer underneath
				lhs->ty = pointer_to (f->ty->base);
				lhs->lvalue = 1;
				expr (a);
				bind_ref (a, f->ty);
			} else {
				expr (lhs);
				expr (a);
			}
			asg = mk (ND_ASSIGN, mi->name);
			asg->op = make_ident (mi->name, "=");
			asg->lhs = lhs;
			asg->rhs = a;
			asg->ty = unqualified (lhs->ty);
			cur = cur->next = mk_stmt (asg);
		}
	}
	return head.next;
}

// a function's body: its parameters in scope, and in a member function of C
// the class's members and `this`
static void function_body (Node *n, Class *c, Method *m)
{
	Decl *d = n->decl, *p, *k;
	Scope *saved_sc = sc;
	Class *saved_class = cur_class;
	Method *saved_method = cur_method;
	Ty *saved_ret = cur_ret;
	CScope *saved_cs = cs;
	int saved_in = in_function;
	Scope *saved_fn = fn_scope;
	Node *saved_temps = fn_temps;
	declaration (d, 1);
	cur_class = c;
	cur_method = m;
	cur_ret = d->rty->kind == T_FUNC ? unqualified (d->rty->base) : ty_int;
	if (c)
		sc = c->scope;
	in_function = 1;
	push_scope ();
	fn_scope = sc;
	fn_temps = 0;
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
	cs = 0;
	push_cs (CS_FUNC);
	if (m && m->is_dtor) {
		// the members' destructors, last first, after the body, and the
		// base's after them
		Field *f;
		if (c->base && c->base->needs_dtor) {
			Obj *o = xalloc (sizeof *o);
			o->cls = c->base;
			o->member = field_named (c, "_b");
			cs->objs = o;
		}
		if (c->poly) {
			// _vp the class's own again: virtual calls in the body are its
			Node *v = set_vp (c, n->body->tok);
			v->next = n->body->body;
			n->body->body = v;
		}
		for (f = c->rec->def->fields; f; f = f->next) {
			Class *fc = class_of (f->ty);
			if (f->name && !f->anon && fc && fc->needs_dtor) {
				Obj *o = xalloc (sizeof *o);
				o->cls = fc;
				o->member = f;
				o->next = cs->objs;
				cs->objs = o;
			}
		}
	}
	if (m && m->is_ctor) {
		Node *init = ctor_inits (c, d->inits, n->body->tok), *ret = mk (ND_RETURN, n->body->tok), **q;
		// the members, the body, and return this
		for (q = &init; *q; q = &(*q)->next)
			;
		*q = n->body->body;
		n->body->body = init;
		append (&n->body->body, ret);
	}
	stmts (n->body->body);
	n->body->cleanup = pop_cs (n->body->tok);
	if (fn_temps) {
		// the temporaries' storage, first
		append (&fn_temps, n->body->body);
		n->body->body = fn_temps;
	}
	fn_scope = saved_fn;
	fn_temps = saved_temps;
	pop_scope ();
	sc = saved_sc;
	cur_class = saved_class;
	cur_method = saved_method;
	cur_ret = saved_ret;
	cs = saved_cs;
	in_function = saved_in;
}

static void funcdef (Node *n)
{
	Decl *d = n->decl;
	if (opt_cplusplus && d->qual) {
		// a member function defined outside its class
		Class *c = class_named (d->qual);
		Method *m;
		Ty *dt = decl_type (d->spec, d->type);
		for (m = c->methods; m; m = m->next)
			if ((d->is_ctor ? m->is_ctor : d->is_dtor ? m->is_dtor :
			     !m->is_ctor && !m->is_dtor && same_tok (m->name, d->name)) &&
			    same_params (m->fty, dt, !m->is_static, 0))
				break;
		if (!m)
			error_at (d->name, "'%.*s' is not a member function of '%.*s'", d->name->len, d->name->loc,
			          d->qual->len, d->qual->loc);
		if (m->inline_body)
			error_at (d->name, "a member function defined twice");
		n->decl = lowered (c, m, d, d->spec->storage & ~(ST_STATIC | ST_VIRTUAL | ST_INLINE | ST_EXPLICIT));
		function_body (n, c, m);
		if (m == c->key)
			// the class's vtable, in the unit of its key function
			append (&n->post, raw (d->tok, vtbl_text (c, "")));
		return;
	}
	function_body (n, 0, 0);
}

// C++: the unit's global objects constructed before main, and destroyed
// after it, last first (c4's .c4r constructor and destructor lists)
static Node *unit_function (const char *what, const char *attr, Node *body)
{
	Node *f = mk (ND_RAWFUNC, 0), *b = mk (ND_BLOCK, 0);
	const char *u = opt_unit ? opt_unit : "unit", *s = strrchr (u, '/');
	char *name = xstrndup (s ? s + 1 : u, strlen (s ? s + 1 : u)), *p;
	for (p = name; *p; p++)
		if (!isalnum ((unsigned char)*p))
			*p = '_';
	f->tok = make_ident (body->tok, xformat ("static void __attribute__ ((%s)) __c4x_%s_%s (void)", attr, what, name));
	b->body = body;
	f->body = b;
	return f;
}

void sema (Node *unit)
{
	Node *n, *last = 0;
	types_init ();
	sc = 0;
	push_scope ();
	file_scope = sc;
	pack_cur = 0;
	pack_sp = 0;
	cs = 0;
	init_stmts = 0;
	init_tail = &init_stmts;
	fini_stmts = 0;
	rt_declared = 0;
	for (n = unit; n; n = n->next) {
		cur_top = n;
		if (n->kind == ND_FUNCDEF)
			funcdef (n);
		else if (n->kind == ND_PRAGMA)
			pragma (n->tok);
		else
			declaration (n->decl, 1);
		last = n;
	}
	cur_top = 0;
	if (last && init_stmts)
		append (&last->post, unit_function ("init", "constructor", init_stmts));
	if (last && fini_stmts)
		append (&last->post, unit_function ("fini", "destructor", fini_stmts));
}
