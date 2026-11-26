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
	Token *cname;		// S_SVAR: the global's name in C (X__name)
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
	Token *cname;		// its name in C
	Ty *fty;		// its type in C: `this` first unless static
};

struct Class {
	Token *name;
	Ty *rec;
	Scope *scope;		// S_FIELD, S_METHOD, S_SVAR; up is where the class is
	Method *methods;
	Method *ctor, *dtor;	// the user's, or made when members need them
	int needs_ctor, needs_dtor;
	int anon;		// anonymous members named so far (_u0, _u1, ...)
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

// N bound to a reference of type REF: its address, or a const reference's
// copy of an rvalue
static void bind_ref (Node *n, Ty *ref)
{
	Node *m, *next = n->next;
	if (n->lvalue && !n->bitfield) {
		take_address (n);
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
	}
	return 0;
}

// a member function call, obj.f(...), p->f(...), f(...) in a member
// function, or X::f(...): to Class__f (this, ...). 0 if N is not one.
static int method_call (Node *n)
{
	Node *f = n->lhs, *obj = 0, *a;
	Method *m = 0;
	Sym *y = 0;
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
		if (!m->is_static) {
			if (!cur_class)
				error_at (f->tok, "a member function called without an object");
			obj = mk_this (f->tok);
		}
	} else
		return 0;
	if (m->is_ctor || m->is_dtor)
		error_at (f->tok, "constructors and destructors are not called by name");
	if (m->is_static)
		obj = 0;
	n->lhs = mk (ND_IDENT, make_ident (f->tok, xstrndup (m->cname->loc, m->cname->len)));
	n->lhs->ty = m->fty;
	for (a = n->args; a; a = a->next)
		expr (a);
	if (obj) {
		obj->next = n->args;
		n->args = obj;
	}
	finish_call (n, m->fty);
	return 1;
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
	if (opt_cplusplus && method_call (n))
		return;
	if (opt_cplusplus && n->lhs->kind == ND_IDENT && !n->lhs->qual && !find_sym (n->lhs->tok))
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
	if (opt_cplusplus) {
		finish_call (n, f);
		return;
	}
	n->ty = unqualified (f->base);
}

static int anon_path (Node *n, Ty *o);

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
	n->bitfield = f->bit_width >= 0;
	if (opt_cplusplus)
		deref_ref (n);
}

// C++: a member inside anonymous structs and unions, which have names in C
// (_u0): obj.x as obj._u0.x. 1 if it was one (and N is typed).
static int anon_path (Node *n, Ty *o)
{
	Field *f;
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
		if (opt_cplusplus)
			deref_ref (n);
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
		if (n->functional) {
			if (!n->args || n->args->next)
				error_at (n->tok, "T(...) without exactly one argument (a temporary) is not supported");
			if (class_of (type_name_type (n->decl)))
				error_at (n->tok, "a class's temporary, X(...), is not supported");
		}
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
	case ND_NEW: case ND_DELETE:
		error_at (n->tok, "new and delete are not supported yet (M5)");
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
		if (f->anon)
			add_fields (c, f->ty);
		else
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
	me->cname = make_ident (m->tok, is_ctor ? xformat ("%.*s__C", n->len, n->loc) :
	                                is_dtor ? xformat ("%.*s__D", n->len, n->loc) :
	                                xformat ("%.*s__%.*s", n->len, n->loc, m->name->len, m->name->loc));
	for (p = &c->methods; *p; p = &(*p)->next)
		if ((is_ctor && (*p)->is_ctor) || (is_dtor && (*p)->is_dtor) ||
		    (!is_ctor && !is_dtor && !(*p)->is_ctor && !(*p)->is_dtor && same_tok ((*p)->name, m->name)))
			error_at (m->tok, "overloaded member functions are not supported yet (M4)");
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

static void cxx_record (Spec *sp, Ty *rec, int pack)
{
	Class *c = xalloc (sizeof *c);
	Field head = {0}, *cur = &head, *f;
	Decl kh = {0}, *kc = &kh, *m, *next;
	Method *me;
	c->name = sp->tag;
	c->rec = rec;
	rec->cls = c;
	c->scope = xalloc (sizeof (Scope));
	c->scope->up = sc;
	if (sp->bases)
		error_at (sp->bases, "base classes are not supported yet (M5)");
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
			if ((ms->storage & ST_VIRTUAL) || m->pure)
				error_at (m->tok, "virtual functions are not supported yet (M5)");
			if (!strncmp (m->name->loc, "operator", 8) && m->name->len > 8 && !is_ident_char (m->name->loc[8]))
				error_at (m->tok, "member operators are not supported yet (M4)");
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
	if (c->ctor)
		c->needs_ctor = 1;
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
		declaration (x, 1);
		me->fty = x->rty;
		y = add_sym (c->scope, me->name, S_METHOD, me->fty);
		y->m = me;
		n->decl = x;
		append (&cur_top->post, n);
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
	if (d->init->is_list || t->kind == T_REF)
		error_at (d->name, "a global initialized at run time by a list, or a reference, is not supported");
	if (d->spec->qual & Q_CONST)
		d->spec->qual &= ~Q_CONST;	// C would refuse the assignment
	lhs = mk (ND_IDENT, make_ident (d->name, xstrndup (d->name->loc, d->name->len)));
	lhs->ty = unqualified (t);
	lhs->lvalue = 1;
	asg = mk (ND_ASSIGN, d->name);
	asg->op = make_ident (d->name, "=");
	asg->lhs = lhs;
	asg->rhs = d->init->expr;
	asg->ty = lhs->ty;
	asg = mk_stmt (asg);
	*init_tail = asg;
	init_tail = &asg->next;
	d->init = 0;
}

// C++: an object of a class with a constructor or destructor: the call to
// construct it (after its declaration, or in the unit's initializer), and
// its destruction at the end of its scope (or the unit's finalizer)
static void class_object (Decl *d, Ty *t, int top)
{
	Class *c = class_of (t);
	int global = top || (d->spec->storage & ST_STATIC);
	if (t->kind == T_ARRAY && (needs (t, 0) || needs (t, 1)))
		error_at (d->name, "arrays of objects with constructors or destructors are not supported yet");
	if (!c)
		return;
	if (d->init && !d->init->is_list && c->ctor && !same_type (unqualified (d->init->expr->ty), unqualified (t))) {
		// X x = v: X's constructor of v
		d->args = d->init->expr;
		d->args->next = 0;
		d->has_args = 1;
		d->init = 0;
	}
	if (c->needs_ctor && !d->init) {
		Node *call;
		if (global && !top)
			error_at (d->name, "a static local object with a constructor is not supported");
		call = special_call (c->ctor, var_addr (d->name, d->tok), d->args, d->tok);
		if (top) {
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
			if (opt_cplusplus && top && t->kind != T_FUNC && const_object (t) &&
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
	MemInit *mi;
	Field *f;
	for (mi = inits; mi; mi = mi->next) {
		for (f = c->rec->def->fields; f; f = f->next)
			if (f->name && !f->anon && same_tok (f->name, mi->name))
				break;
		if (!f)
			error_at (mi->name, "'%.*s' is not a member of '%.*s' (base classes are not supported yet)",
			          mi->name->len, mi->name->loc, c->name->len, c->name->loc);
	}
	for (f = c->rec->def->fields; f; f = f->next) {
		Class *fc = class_of (f->ty);
		if (!f->name || f->anon)
			continue;
		for (mi = inits; mi; mi = mi->next)
			if (same_tok (f->name, mi->name))
				break;
		if (fc && fc->needs_ctor)
			cur = cur->next = special_call (fc->ctor, member_addr (f, at), mi ? mi->args : 0, mi ? mi->name : at);
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
	declaration (d, 1);
	cur_class = c;
	cur_method = m;
	cur_ret = d->rty->kind == T_FUNC ? unqualified (d->rty->base) : ty_int;
	if (c)
		sc = c->scope;
	in_function = 1;
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
	cs = 0;
	push_cs (CS_FUNC);
	if (m && m->is_dtor) {
		// the members' destructors, last first, after the body
		Field *f;
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
		for (m = c->methods; m; m = m->next)
			if (d->is_ctor ? m->is_ctor : d->is_dtor ? m->is_dtor :
			    !m->is_ctor && !m->is_dtor && same_tok (m->name, d->name))
				break;
		if (!m)
			error_at (d->name, "'%.*s' is not a member function of '%.*s'", d->name->len, d->name->loc,
			          d->qual->len, d->qual->loc);
		if (m->inline_body)
			error_at (d->name, "a member function defined twice");
		n->decl = lowered (c, m, d, d->spec->storage & ~(ST_STATIC | ST_VIRTUAL | ST_INLINE | ST_EXPLICIT));
		function_body (n, c, m);
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
