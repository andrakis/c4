// parse.c -- C (as c4lc takes it) to the syntax tree of ast.h
//
// Recursive descent. A name is a type when a typedef in scope says so, so
// the parser keeps block scopes of the names declared in them.
#include "ast.h"

static Token *T;	// the next token

// C++: what the last declarator's name said besides the name, and whether a
// '(' after a declaration's name may hold a constructor's arguments
static Token *name_qual;	// X in X::name
static int name_dtor;		// ~X
static int ctor_args_ok;
static int extern_c;		// inside extern "C" { }
static Token *ctor_name_at;	// a constructor's name, where specifiers stop

Token *make_ident (Token *like, const char *name)
{
	Token *t = copy_token (like);
	t->kind = TK_IDENT;
	t->loc = (char *)name;
	t->len = strlen (name);
	t->next = 0;
	return t;
}

// ---- scopes ----

typedef struct Name Name;
struct Name { Name *next; Token *tok; int is_typedef; };
typedef struct Scope Scope;
struct Scope { Scope *up; Name *names; };
static Scope *scope;

static void push_scope (void)
{
	Scope *s = xalloc (sizeof *s);
	s->up = scope;
	scope = s;
}

static void pop_scope (void)
{
	scope = scope->up;
}

static void declare (Token *name, int is_typedef)
{
	Name *n;
	if (!name)
		return;
	n = xalloc (sizeof *n);
	n->tok = name;
	n->is_typedef = is_typedef;
	n->next = scope->names;
	scope->names = n;
}

static int same_name (Token *a, Token *b)
{
	return a->len == b->len && !memcmp (a->loc, b->loc, a->len);
}

static int is_typedef_name (Token *t)
{
	Scope *s;
	Name *n;
	if (t->kind != TK_IDENT)
		return 0;
	for (s = scope; s; s = s->up)
		for (n = s->names; n; n = n->next)
			if (same_name (n->tok, t))
				return n->is_typedef;
	return 0;
}

// ---- tokens ----

static int is (const char *s)
{
	return equal (T, s);
}

static Token *advance (void)
{
	Token *t = T;
	if (T->kind != TK_EOF)
		T = T->next;
	return t;
}

static int accept (const char *s)
{
	if (is (s)) {
		advance ();
		return 1;
	}
	return 0;
}

static Token *expect (const char *s)
{
	if (!is (s))
		error_at (T, "expected '%s'", s);
	return advance ();
}

static const char *type_words[] = { "void", "char", "short", "int", "long", "float", "double",
	"signed", "unsigned", "_Bool", "__signed__", "__signed", 0 };
static const char *qual_words[] = { "const", "volatile", "restrict", "__restrict", "__restrict__",
	"__const", "__const__", "__volatile", "__volatile__", 0 };
static const char *storage_words[] = { "typedef", "extern", "static", "auto", "register",
	"inline", "__inline", "__inline__", 0 };
static const char *keywords[] = { "break", "case", "continue", "default", "do", "else", "enum",
	"for", "goto", "if", "return", "sizeof", "struct", "switch", "union", "while",
	"__attribute__", "__attribute", "__extension__", 0 };

// C++'s words on top of C's
static const char *cxx_storage_words[] = { "virtual", "explicit", "mutable", 0 };
static const char *cxx_keywords[] = { "bool", "class", "new", "delete", "operator", "template",
	"namespace", "public", "private", "protected", "friend", "using", "true", "false", "try",
	"catch", "throw", "typename", "static_cast", "dynamic_cast", "reinterpret_cast",
	"const_cast", "typeid", 0 };

static int in_list (Token *t, const char **l)
{
	int i;
	if (t->kind != TK_IDENT)
		return 0;
	for (i = 0; l[i]; i++)
		if (equal (t, l[i]))
			return 1;
	return 0;
}

static int is_type_word (Token *t)
{
	return in_list (t, type_words) || (opt_cplusplus && equal (t, "bool"));
}

static int is_storage_word (Token *t)
{
	return in_list (t, storage_words) || (opt_cplusplus && in_list (t, cxx_storage_words));
}

static int is_keyword (Token *t)
{
	return in_list (t, type_words) || in_list (t, qual_words) || is_storage_word (t) || in_list (t, keywords) ||
	       (opt_cplusplus && in_list (t, cxx_keywords));
}

static int is_attribute (Token *t)
{
	return equal (t, "__attribute__") || equal (t, "__attribute");
}

// C++: a class name before '::' qualifies a name; it does not start a type
static int is_type_ident (Token *t)
{
	return is_typedef_name (t) && !(opt_cplusplus && equal (t->next, "::"));
}

// C++: X::Inner, a nested type (not X::X, a constructor)
static int is_nested_type (Token *t)
{
	return opt_cplusplus && is_typedef_name (t) && equal (t->next, "::") && is_typedef_name (t->next->next) &&
	       !same_name (t, t->next->next) && !equal (t->next->next->next, "::");
}

// can a type name start here (a cast, sizeof, a parameter)?
static int is_typename_start (Token *t)
{
	return is_type_word (t) || in_list (t, qual_words) || equal (t, "struct") ||
	       equal (t, "union") || equal (t, "enum") || is_type_ident (t) || is_nested_type (t) || is_attribute (t) ||
	       equal (t, "__extension__") || (opt_cplusplus && equal (t, "class"));
}

// can a declaration start here?
static int is_decl_start (Token *t)
{
	return is_typename_start (t) || is_storage_word (t);
}

// ---- nodes ----

static Node *new_node (int kind, Token *tok)
{
	Node *n = xalloc (sizeof *n);
	n->kind = kind;
	n->tok = tok;
	return n;
}

static Type *new_type (int kind, Type *base)
{
	Type *t = xalloc (sizeof *t);
	t->kind = kind;
	t->base = base;
	return t;
}

static Type base_type = { TY_BASE };

static Node *expr (void);
static Node *assign (void);
static Node *conditional (void);
static Node *cast_expr (void);
static Node *compound_stmt (void);
static Node *stmt (void);
static Spec *parse_spec (int allow_storage);
static Type *declarator (Type *base, Token **name);
static Init *initializer (void);
static Decl *type_name (void);

// ---- attributes ----

static Attr *parse_attr (void)
{
	Attr *a = xalloc (sizeof *a);
	Token *start = advance ();	// __attribute__
	int depth = 0;
	a->start = start;
	if (!is ("("))
		error_at (T, "expected '(' after __attribute__");
	do {
		if (is ("("))
			depth++;
		else if (is (")"))
			depth--;
		if (T->kind == TK_EOF)
			error_at (start, "the attribute does not end");
		advance ();
	} while (depth);
	a->end = T;
	return a;
}

static Attr *attrs (Attr *list)
{
	while (is_attribute (T)) {
		Attr *a = parse_attr (), **p = &list;
		while (*p)
			p = &(*p)->next;
		*p = a;
	}
	return list;
}

// ---- specifiers ----

static Decl *struct_members (void);
static Decl *class_members (Token *cname);

// is the name known in any scope (as anything)?
static int name_known (Token *t)
{
	Scope *s;
	Name *n;
	for (s = scope; s; s = s->up)
		for (n = s->names; n; n = n->next)
			if (same_name (n->tok, t))
				return 1;
	return 0;
}

// C++: a class or enum tag names a type. A tag that is only referred to
// (struct stat st, beside a function stat) declares nothing new.
static void declare_tag (Token *tag, int defining)
{
	if (opt_cplusplus && tag && (defining || is (";") || !name_known (tag)))
		declare (tag, 1);
}

static void struct_spec (Spec *sp)
{
	sp->kind = equal (T, "union") ? SP_UNION : SP_STRUCT;
	sp->is_class = equal (T, "class");
	advance ();
	sp->tag_attrs = attrs (0);
	if (T->kind == TK_IDENT && !is ("{"))
		sp->tag = advance ();
	if (opt_cplusplus && is (":") && sp->tag) {
		// a base clause (M5): kept as its tokens
		advance ();
		sp->bases = T;
		while (!is ("{")) {
			if (T->kind == TK_EOF || is (";"))
				error_at (T, "expected '{' after the base classes");
			advance ();
		}
	}
	declare_tag (sp->tag, is ("{"));
	if (is ("{")) {
		advance ();
		sp->has_body = 1;
		sp->members = opt_cplusplus ? class_members (sp->tag) : struct_members ();
		expect ("}");
	} else if (!sp->tag)
		error_at (T, "expected a struct tag or '{'");
}

static void enum_spec (Spec *sp)
{
	Enumer head = {0}, *cur = &head;
	sp->kind = SP_ENUM;
	advance ();
	sp->tag_attrs = attrs (0);
	if (T->kind == TK_IDENT && !is ("{"))
		sp->tag = advance ();
	declare_tag (sp->tag, is ("{"));
	if (!is ("{")) {
		if (!sp->tag)
			error_at (T, "expected an enum tag or '{'");
		return;
	}
	advance ();
	sp->has_body = 1;
	while (!is ("}")) {
		Enumer *e = xalloc (sizeof *e);
		if (T->kind != TK_IDENT)
			error_at (T, "expected an enumerator");
		e->name = advance ();
		declare (e->name, 0);
		if (accept ("="))
			e->value = conditional ();
		cur = cur->next = e;
		if (!accept (","))
			break;
	}
	sp->enumers = head.next;
	expect ("}");
}

static void add_word (Spec *sp, Token *t)
{
	Token **w = xalloc (sizeof (Token *) * (sp->nwords + 1));
	if (sp->nwords)
		memcpy (w, sp->words, sizeof (Token *) * sp->nwords);
	w[sp->nwords++] = t;
	sp->words = w;
}

static Spec *parse_spec (int allow_storage)
{
	Spec *sp = xalloc (sizeof *sp);
	sp->tok = T;
	sp->extern_c = extern_c;
	for (;;) {
		if (is_storage_word (T)) {
			if (!allow_storage)
				error_at (T, "a storage class is not allowed here");
			if (equal (T, "typedef")) sp->storage |= ST_TYPEDEF;
			else if (equal (T, "extern")) sp->storage |= ST_EXTERN;
			else if (equal (T, "virtual")) sp->storage |= ST_VIRTUAL;
			else if (equal (T, "explicit")) sp->storage |= ST_EXPLICIT;
			else if (equal (T, "mutable")) sp->storage |= ST_MUTABLE;
			else if (equal (T, "static")) sp->storage |= ST_STATIC;
			else if (equal (T, "auto")) sp->storage |= ST_AUTO;
			else if (equal (T, "register")) sp->storage |= ST_REGISTER;
			else sp->storage |= ST_INLINE;
			advance ();
			continue;
		}
		if (in_list (T, qual_words)) {
			if (equal (T, "const") || equal (T, "__const") || equal (T, "__const__")) sp->qual |= Q_CONST;
			else if (equal (T, "volatile") || equal (T, "__volatile") || equal (T, "__volatile__")) sp->qual |= Q_VOLATILE;
			else sp->qual |= Q_RESTRICT;
			advance ();
			continue;
		}
		if (is_attribute (T)) {
			sp->attrs = attrs (sp->attrs);
			continue;
		}
		if (equal (T, "__extension__")) {
			advance ();
			continue;
		}
		if (sp->kind == SP_WORDS && sp->nwords == 0 &&
		    (equal (T, "struct") || equal (T, "union") || (opt_cplusplus && equal (T, "class")))) {
			struct_spec (sp);
			continue;
		}
		if (sp->kind == SP_WORDS && sp->nwords == 0 && equal (T, "enum")) {
			enum_spec (sp);
			continue;
		}
		if (is_type_word (T) && sp->kind == SP_WORDS && !sp->is_typedef_name) {
			add_word (sp, advance ());
			continue;
		}
		if (opt_cplusplus && sp->kind == SP_WORDS && sp->nwords == 0 && is_nested_type (T)) {
			// X::Inner: a nested type, which is at file scope under its own name
			advance ();
			advance ();
			continue;
		}
		if (sp->kind == SP_WORDS && sp->nwords == 0 && is_type_ident (T) &&
		    // C++: X(...) inside class X, or X::X, is a constructor
		    !(opt_cplusplus && T == ctor_name_at)) {
			add_word (sp, advance ());
			sp->is_typedef_name = 1;
			continue;
		}
		break;
	}
	return sp;
}

// ---- declarators ----

static void ptr_quals (Type *p)
{
	for (;;) {
		if (in_list (T, qual_words)) {
			if (equal (T, "const") || equal (T, "__const") || equal (T, "__const__")) p->qual |= Q_CONST;
			else if (equal (T, "volatile") || equal (T, "__volatile") || equal (T, "__volatile__")) p->qual |= Q_VOLATILE;
			else p->qual |= Q_RESTRICT;
			advance ();
		} else if (is_attribute (T))
			p->attrs = attrs (p->attrs);
		else
			return;
	}
}

static Decl *params (Type *fn)
{
	Decl head = {0}, *cur = &head;
	if (is (")")) {
		fn->oldstyle = !opt_cplusplus;	// C++: f() is f(void)
		return 0;
	}
	if (equal (T, "void") && equal (T->next, ")")) {
		advance ();
		return 0;
	}
	// K&R: an identifier list
	if (!opt_cplusplus && T->kind == TK_IDENT && !is_typename_start (T) && !in_list (T, storage_words) &&
	    (equal (T->next, ",") || equal (T->next, ")"))) {
		fn->oldstyle = 1;
		for (;;) {
			Decl *d = xalloc (sizeof *d);
			if (T->kind != TK_IDENT)
				error_at (T, "expected a parameter name");
			d->name = d->tok = advance ();
			cur = cur->next = d;
			if (!accept (","))
				break;
		}
		return head.next;
	}
	for (;;) {
		Decl *d;
		if (is ("...")) {
			advance ();
			fn->variadic = 1;
			break;
		}
		d = xalloc (sizeof *d);
		d->tok = T;
		d->spec = parse_spec (1);
		d->first_of_spec = 1;
		d->type = declarator (&base_type, &d->name);
		d->attrs = attrs (0);
		if (opt_cplusplus && accept ("=")) {
			// a default argument
			d->init = xalloc (sizeof (Init));
			d->init->tok = T;
			d->init->expr = assign ();
		}
		cur = cur->next = d;
		if (!accept (","))
			break;
	}
	return head.next;
}

static Type *type_suffix (Type *base)
{
	if (is ("[")) {
		Type *a = new_type (TY_ARRAY, 0);
		advance ();
		while (equal (T, "static") || in_list (T, qual_words))
			advance ();
		if (!is ("]"))
			a->len = assign ();
		expect ("]");
		a->base = type_suffix (base);
		return a;
	}
	if (is ("(")) {
		Type *f = new_type (TY_FUNC, base);
		int save = ctor_args_ok;
		advance ();
		push_scope ();
		ctor_args_ok = 0;
		f->params = params (f);
		ctor_args_ok = save;
		pop_scope ();
		expect (")");
		if (opt_cplusplus) {
			while (is ("const") || is ("volatile")) {
				f->const_method = 1;
				advance ();
			}
			if (is ("throw"))
				error_at (T, "exception specifications are not supported");
		}
		return f;
	}
	return base;
}

// C++: after a declaration's name, does '(' begin parameters (a function)
// or a constructor's arguments (an object)?
static int looks_like_params (void)
{
	Token *n = T->next;
	return equal (n, ")") || equal (n, "...") || is_decl_start (n);
}

// C++: a name after 'operator', as one made token ("operator==")
static Token *operator_name (void)
{
	Token *op = advance ();	// operator
	char *s;
	if (equal (T, "(") && equal (T->next, ")")) {
		advance ();
		advance ();
		return make_ident (op, "operator()");
	}
	if (equal (T, "[") && equal (T->next, "]")) {
		advance ();
		advance ();
		return make_ident (op, "operator[]");
	}
	if (equal (T, "new") || equal (T, "delete")) {
		Token *w = advance ();
		int arr = equal (T, "[") && equal (T->next, "]");
		if (arr) {
			advance ();
			advance ();
		}
		return make_ident (op, xformat ("operator %.*s%s", w->len, w->loc, arr ? "[]" : ""));
	}
	if (T->kind != TK_PUNCT)
		error_at (T, "conversion operators are not supported");
	s = xformat ("operator%.*s", T->len, T->loc);
	advance ();
	return make_ident (op, s);
}

// does the '(' at T begin a nested declarator, rather than parameters?
static int nested_declarator (void)
{
	Token *n = T->next;
	if (equal (n, "*") || equal (n, "(") || equal (n, "[") || is_attribute (n))
		return 1;
	if (n->kind == TK_IDENT && !is_typename_start (n) && !in_list (n, storage_words) && !equal (n, "..."))
		return 1;
	return 0;
}

static Token *skip_parens (Token *t)
{
	int depth = 0;
	do {
		if (equal (t, "("))
			depth++;
		else if (equal (t, ")"))
			depth--;
		if (t->kind == TK_EOF)
			error_at (t, "unbalanced parentheses");
		t = t->next;
	} while (depth);
	return t;
}

static Type *declarator (Type *base, Token **name)
{
	name_qual = 0;
	name_dtor = 0;
	while (is ("*") || (opt_cplusplus && is ("&"))) {
		Type *p = new_type (is ("&") ? TY_REF : TY_PTR, base);
		advance ();
		ptr_quals (p);
		base = p;
	}
	if (opt_cplusplus && !is ("(")) {
		// a name, qualified (X::f, X::X, X::~X, ::f) or an operator
		Token *q = 0;
		int dtor = 0;
		Type *r;
		if (is ("::"))
			advance ();
		if (T->kind == TK_IDENT && equal (T->next, "::") && T->next->next->kind != TK_EOF) {
			q = advance ();
			advance ();
		}
		if (is ("~")) {
			advance ();
			dtor = 1;
		}
		if (is ("operator"))
			*name = operator_name ();
		else if (T->kind == TK_IDENT && !is_keyword (T))
			*name = advance ();
		else if (q || dtor)
			error_at (T, "expected a name");
		if (*name && ctor_args_ok && is ("(") && !looks_like_params ()) {
			r = base;	// X x(args): the caller reads the arguments
		} else
			r = type_suffix (base);
		name_qual = q;
		name_dtor = dtor;
		return r;
	}
	if (is ("(") && nested_declarator ()) {
		Token *start = T, *after;
		Type *suffixed, *inner;
		T = skip_parens (start);
		suffixed = type_suffix (base);
		after = T;
		T = start->next;
		inner = declarator (suffixed, name);
		expect (")");
		T = after;
		return inner;
	}
	if (T->kind == TK_IDENT && !is_keyword (T)) {
		*name = advance ();
	}
	return type_suffix (base);
}

static Decl *type_name (void)
{
	Decl *d = xalloc (sizeof *d);
	d->tok = T;
	d->spec = parse_spec (0);
	d->first_of_spec = 1;
	d->type = declarator (&base_type, &d->name);
	if (d->name)
		error_at (d->name, "a type name has no name");
	return d;
}

static Decl *struct_members (void)
{
	Decl head = {0}, *cur = &head;
	while (!is ("}")) {
		Spec *sp;
		int first = 1;
		if (T->kind == TK_PRAGMA) {
			Decl *d = xalloc (sizeof *d);
			d->is_pragma = 1;
			d->tok = advance ();
			cur = cur->next = d;
			continue;
		}
		if (accept (";"))
			continue;
		sp = parse_spec (0);
		if (is (";")) {
			// an anonymous struct or union member
			Decl *d = xalloc (sizeof *d);
			d->spec = sp;
			d->first_of_spec = 1;
			d->tok = sp->tok;
			cur = cur->next = d;
			advance ();
			continue;
		}
		for (;;) {
			Decl *d = xalloc (sizeof *d);
			d->spec = sp;
			d->tok = T;
			d->first_of_spec = first;
			first = 0;
			if (!is (":"))
				d->type = declarator (&base_type, &d->name);
			else
				d->type = &base_type;
			if (accept (":"))
				d->width = conditional ();
			d->attrs = attrs (0);
			cur = cur->next = d;
			if (!accept (","))
				break;
		}
		expect (";");
	}
	return head.next;
}

// C++: a constructor's init list, after the ':'
static MemInit *mem_inits (void)
{
	MemInit head = {0}, *cur = &head;
	for (;;) {
		MemInit *m = xalloc (sizeof *m);
		Node ah = {0}, *ac = &ah;
		if (T->kind != TK_IDENT)
			error_at (T, "expected a member to initialize");
		m->name = advance ();
		expect ("(");
		while (!is (")")) {
			ac = ac->next = assign ();
			if (!accept (","))
				break;
		}
		expect (")");
		m->args = ah.next;
		cur = cur->next = m;
		if (!accept (","))
			break;
	}
	return head.next;
}

// a function's body, its parameters in scope
static Node *function_body (Decl *d)
{
	Node *body;
	Decl *p;
	push_scope ();
	if (d->type && d->type->kind == TY_FUNC)
		for (p = d->type->params; p; p = p->next)
			declare (p->name, 0);
	body = compound_stmt ();
	pop_scope ();
	return body;
}

// skip to the ';' that ends a declaration (friend), keeping to its braces
static void skip_declaration (void)
{
	int depth = 0;
	while (!(depth == 0 && is (";"))) {
		if (T->kind == TK_EOF)
			error_at (T, "the declaration does not end");
		if (is ("{") || is ("(")) depth++;
		if (is ("}") || is (")")) depth--;
		advance ();
	}
	advance ();
}

// C++: a class body. Data members as struct_members reads them, and member
// functions (declared, or defined inline), constructors, destructors,
// static members and nested types; access specifiers and friends are
// skipped.
static Decl *class_members (Token *cname)
{
	Decl head = {0}, *cur = &head;
	while (!is ("}")) {
		Spec *sp;
		int first = 1;
		Token *t;
		if (T->kind == TK_PRAGMA) {
			Decl *d = xalloc (sizeof *d);
			d->is_pragma = 1;
			d->tok = advance ();
			cur = cur->next = d;
			continue;
		}
		if (accept (";"))
			continue;
		if ((is ("public") || is ("private") || is ("protected")) && equal (T->next, ":")) {
			advance ();
			advance ();
			continue;
		}
		if (is ("friend")) {
			skip_declaration ();
			continue;
		}
		if (is ("template") || is ("using"))
			error_at (T, "'%.*s' is not supported", T->len, T->loc);
		// a constructor or destructor: [specifiers] X( or [specifiers] ~X(
		for (t = T; is_storage_word (t); t = t->next)
			;
		ctor_name_at = 0;
		if (cname && t->kind == TK_IDENT && same_name (t, cname) && equal (t->next, "("))
			ctor_name_at = t;
		sp = parse_spec (1);
		ctor_name_at = 0;
		if (is (";")) {
			// an anonymous struct or union member, or a nested type
			Decl *d = xalloc (sizeof *d);
			d->spec = sp;
			d->first_of_spec = 1;
			d->tok = sp->tok;
			cur = cur->next = d;
			advance ();
			continue;
		}
		for (;;) {
			Decl *d = xalloc (sizeof *d);
			d->spec = sp;
			d->tok = T;
			d->first_of_spec = first;
			first = 0;
			if (!is (":"))
				d->type = declarator (&base_type, &d->name);
			else
				d->type = &base_type;
			d->qual = name_qual;
			d->is_dtor = name_dtor;
			if (cname && d->name && same_name (d->name, cname) && d->type->kind == TY_FUNC && !sp->nwords &&
			    sp->kind == SP_WORDS)
				d->is_ctor = !d->is_dtor;
			if (d->type && d->type->kind == TY_FUNC) {
				d->attrs = attrs (0);
				if (is ("=") && equal (T->next, "0")) {
					advance ();
					advance ();
					d->pure = 1;
				}
				if (accept (":")) {
					if (!d->is_ctor)
						error_at (T, "an init list on what is not a constructor");
					d->inits = mem_inits ();
					if (!is ("{"))
						error_at (T, "expected the constructor's body");
				}
				if (is ("{")) {
					d->body = function_body (d);
					cur = cur->next = d;
					accept (";");
					break;
				}
				cur = cur->next = d;
				if (!accept (","))
					break;
				continue;
			}
			if (sp->storage & ST_TYPEDEF)
				declare (d->name, 1);
			if (accept (":"))
				d->width = conditional ();
			d->attrs = attrs (0);
			if (accept ("="))
				d->init = initializer ();	// a static const member's value
			cur = cur->next = d;
			if (!accept (","))
				break;
		}
		if (cur->body)
			continue;
		expect (";");
	}
	return head.next;
}

// ---- initializers ----

static Init *initializer (void)
{
	Init *in = xalloc (sizeof *in);
	in->tok = T;
	if (!is ("{")) {
		in->expr = assign ();
		return in;
	}
	advance ();
	in->is_list = 1;
	{
		InitItem head = {0}, *cur = &head;
		while (!is ("}")) {
			InitItem *it = xalloc (sizeof *it);
			Desig dh = {0}, *dc = &dh;
			for (;;) {
				if (is (".") && T->next->kind == TK_IDENT) {
					Desig *d = xalloc (sizeof *d);
					advance ();
					d->field = advance ();
					dc = dc->next = d;
				} else if (is ("[")) {
					Desig *d = xalloc (sizeof *d);
					advance ();
					d->index = conditional ();
					expect ("]");
					dc = dc->next = d;
				} else
					break;
			}
			it->desig = dh.next;
			if (it->desig)
				expect ("=");
			it->init = initializer ();
			cur = cur->next = it;
			if (!accept (","))
				break;
		}
		in->items = head.next;
	}
	expect ("}");
	return in;
}

// ---- declarations ----

static int is_function (Type *t)
{
	return t && t->kind == TY_FUNC;
}

// a declaration (Spec and declarators); a function definition if top and
// the first declarator is a function with a body
static Node *declaration (int top)
{
	Node *n = new_node (ND_DECL, T);
	Spec *sp = parse_spec (1);
	Decl head = {0}, *cur = &head;
	int first = 1;
	if (is (";")) {
		Decl *d = xalloc (sizeof *d);
		d->spec = sp;
		d->first_of_spec = 1;
		d->tok = sp->tok;
		n->decl = d;
		advance ();
		return n;
	}
	for (;;) {
		Decl *d = xalloc (sizeof *d);
		d->spec = sp;
		d->tok = T;
		d->first_of_spec = first;
		ctor_args_ok = opt_cplusplus && !(sp->storage & ST_TYPEDEF);
		d->type = declarator (&base_type, &d->name);
		ctor_args_ok = 0;
		d->qual = name_qual;
		d->is_dtor = name_dtor;
		d->attrs = attrs (0);
		if (!d->name)
			error_at (d->tok, "expected a name in the declaration");
		if (d->qual && is_function (d->type) && same_name (d->qual, d->name) && !sp->nwords && sp->kind == SP_WORDS)
			d->is_ctor = !d->is_dtor;
		// X::f names a member, not a new name here
		if (!d->qual)
			declare (d->name, (sp->storage & ST_TYPEDEF) != 0);
		if (opt_cplusplus && is ("(") && !is_function (d->type)) {
			// X x(args)
			Node ah = {0}, *ac = &ah;
			advance ();
			while (!is (")")) {
				ac = ac->next = assign ();
				if (!accept (","))
					break;
			}
			expect (")");
			d->has_args = 1;
			d->args = ah.next;
		}
		if (top && first && is_function (d->type) && !(sp->storage & ST_TYPEDEF) &&
		    (is ("{") || (opt_cplusplus && is (":")) ||
		     (d->type->oldstyle && d->type->params && !is (";") && !is (",")))) {
			Node *f = new_node (ND_FUNCDEF, n->tok);
			Decl *p;
			if (opt_cplusplus && accept (":")) {
				if (!d->is_ctor)
					error_at (T, "an init list on what is not a constructor");
				d->inits = mem_inits ();
			}
			// K&R parameter declarations
			if (!is ("{")) {
				Decl kh = {0}, *kc = &kh;
				push_scope ();
				while (!is ("{")) {
					Node *kd = declaration (0);
					Decl *x = kd->decl;
					kc->next = x;
					while (kc->next)
						kc = kc->next;
				}
				pop_scope ();
				d->kr = kh.next;
			}
			push_scope ();
			for (p = d->type->params; p; p = p->next)
				declare (p->name, 0);
			for (p = d->kr; p; p = p->next)
				declare (p->name, 0);
			f->decl = d;
			f->body = compound_stmt ();
			pop_scope ();
			return f;
		}
		if (accept ("="))
			d->init = initializer ();
		cur = cur->next = d;
		first = 0;
		if (!accept (","))
			break;
	}
	expect (";");
	n->decl = head.next;
	return n;
}

// ---- statements ----

static Node *compound_stmt (void)
{
	Node *n = new_node (ND_BLOCK, T);
	Node head = {0}, *cur = &head;
	expect ("{");
	push_scope ();
	while (!is ("}")) {
		if (T->kind == TK_EOF)
			error_at (n->tok, "the block does not end");
		cur = cur->next = stmt ();
	}
	pop_scope ();
	advance ();
	n->body = head.next;
	return n;
}

static int is_label (void)
{
	return T->kind == TK_IDENT && !is_keyword (T) && equal (T->next, ":") && !is_typedef_name (T);
}

static Node *stmt (void)
{
	Token *t = T;
	Node *n;
	if (T->kind == TK_PRAGMA) {
		n = new_node (ND_PRAGMA, advance ());
		return n;
	}
	if (is ("{"))
		return compound_stmt ();
	if (accept (";"))
		return new_node (ND_NULL, t);
	if (accept ("if")) {
		n = new_node (ND_IF, t);
		expect ("(");
		n->cond = expr ();
		expect (")");
		n->then = stmt ();
		if (accept ("else"))
			n->els = stmt ();
		return n;
	}
	if (accept ("while")) {
		n = new_node (ND_WHILE, t);
		expect ("(");
		n->cond = expr ();
		expect (")");
		n->body = stmt ();
		return n;
	}
	if (accept ("do")) {
		n = new_node (ND_DO, t);
		n->body = stmt ();
		expect ("while");
		expect ("(");
		n->cond = expr ();
		expect (")");
		expect (";");
		return n;
	}
	if (accept ("for")) {
		n = new_node (ND_FOR, t);
		expect ("(");
		push_scope ();
		if (is_decl_start (T))
			n->decl = declaration (0)->decl;
		else {
			if (!is (";"))
				n->init_expr = expr ();
			expect (";");
		}
		if (!is (";"))
			n->cond = expr ();
		expect (";");
		if (!is (")"))
			n->inc = expr ();
		expect (")");
		n->body = stmt ();
		pop_scope ();
		return n;
	}
	if (accept ("switch")) {
		n = new_node (ND_SWITCH, t);
		expect ("(");
		n->cond = expr ();
		expect (")");
		n->body = stmt ();
		return n;
	}
	if (accept ("case")) {
		n = new_node (ND_CASE, t);
		n->cond = conditional ();
		expect (":");
		n->body = is ("}") ? new_node (ND_NULL, T) : stmt ();
		return n;
	}
	if (accept ("default")) {
		n = new_node (ND_DEFAULT, t);
		expect (":");
		n->body = is ("}") ? new_node (ND_NULL, T) : stmt ();
		return n;
	}
	if (accept ("break")) {
		expect (";");
		return new_node (ND_BREAK, t);
	}
	if (accept ("continue")) {
		expect (";");
		return new_node (ND_CONTINUE, t);
	}
	if (accept ("return")) {
		n = new_node (ND_RETURN, t);
		if (!is (";"))
			n->lhs = expr ();
		expect (";");
		return n;
	}
	if (accept ("goto")) {
		n = new_node (ND_GOTO, t);
		if (T->kind != TK_IDENT)
			error_at (T, "expected a label");
		n->name = advance ();
		expect (";");
		return n;
	}
	if (is_label ()) {
		n = new_node (ND_LABEL, t);
		n->name = advance ();
		advance ();	// ':'
		n->body = is ("}") ? new_node (ND_NULL, T) : stmt ();
		return n;
	}
	if (is_decl_start (T))
		return declaration (0);
	n = new_node (ND_EXPR_STMT, t);
	n->lhs = expr ();
	expect (";");
	return n;
}

// ---- expressions ----

static Node *binary (int kind, Token *op, Node *l, Node *r)
{
	Node *n = new_node (kind, l->tok);
	n->op = op;
	n->lhs = l;
	n->rhs = r;
	return n;
}

static Node *expr (void)
{
	Node *n = assign ();
	while (is (",")) {
		Token *op = advance ();
		n = binary (ND_COMMA, op, n, assign ());
	}
	return n;
}

static const char *assign_ops[] = { "=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", ">>=", 0 };

static Node *assign (void)
{
	Node *n = conditional ();
	int i;
	if (T->kind == TK_PUNCT)
		for (i = 0; assign_ops[i]; i++)
			if (is (assign_ops[i])) {
				Token *op = advance ();
				return binary (ND_ASSIGN, op, n, assign ());
			}
	return n;
}

static Node *logor (void);

static Node *conditional (void)
{
	Node *c = logor ();
	if (is ("?")) {
		Node *n = new_node (ND_COND, c->tok);
		n->op = advance ();
		n->cond = c;
		n->then = expr ();
		expect (":");
		n->els = conditional ();
		return n;
	}
	return c;
}

// the binary levels, loosest first
static const char *levels[][5] = {
	{ "||", 0 }, { "&&", 0 }, { "|", 0 }, { "^", 0 }, { "&", 0 },
	{ "==", "!=", 0 }, { "<", ">", "<=", ">=", 0 }, { "<<", ">>", 0 },
	{ "+", "-", 0 }, { "*", "/", "%", 0 }
};
#define NLEVELS 10

static Node *binlevel (int lv)
{
	Node *n;
	if (lv == NLEVELS)
		return cast_expr ();
	n = binlevel (lv + 1);
	for (;;) {
		int i, hit = 0;
		for (i = 0; levels[lv][i]; i++)
			if (T->kind == TK_PUNCT && is (levels[lv][i])) {
				Token *op = advance ();
				n = binary (ND_BINARY, op, n, binlevel (lv + 1));
				hit = 1;
				break;
			}
		if (!hit)
			return n;
	}
}

static Node *logor (void)
{
	return binlevel (0);
}

static Node *postfix (Node *n);
static Node *unary (void);

static Node *cast_expr (void)
{
	if (is ("(") && is_typename_start (T->next)) {
		Token *t = advance ();
		Decl *ty = type_name ();
		expect (")");
		if (is ("{")) {
			Node *n = new_node (ND_COMPLIT, t);
			n->decl = ty;
			n->initz = initializer ();
			return postfix (n);
		}
		{
			Node *n = new_node (ND_CAST, t);
			n->decl = ty;
			n->lhs = cast_expr ();
			return n;
		}
	}
	return unary ();
}

static Node *primary (void);

static Node *unary (void)
{
	Token *t = T;
	if (is ("++") || is ("--") || is ("&") || is ("*") || is ("+") || is ("-") || is ("~") || is ("!")) {
		Node *n = new_node (ND_PREFIX, t);
		n->op = advance ();
		n->lhs = (equal (n->op, "++") || equal (n->op, "--")) ? unary () : cast_expr ();
		return n;
	}
	if (opt_cplusplus && (is ("new") || (is ("::") && equal (T->next, "new")))) {
		// new T, new T(args), new T[n] (lowered in M5)
		Node *n = new_node (ND_NEW, t);
		Decl *d = xalloc (sizeof *d);
		accept ("::");
		advance ();
		if (is ("("))
			error_at (T, "placement new is not supported");
		d->tok = T;
		d->spec = parse_spec (0);
		d->first_of_spec = 1;
		d->type = &base_type;
		while (is ("*")) {
			Type *p = new_type (TY_PTR, d->type);
			advance ();
			ptr_quals (p);
			d->type = p;
		}
		n->decl = d;
		if (accept ("[")) {
			n->is_array = 1;
			n->rhs = expr ();
			expect ("]");
		} else if (accept ("(")) {
			Node ah = {0}, *ac = &ah;
			n->functional = 1;
			while (!is (")")) {
				ac = ac->next = assign ();
				if (!accept (","))
					break;
			}
			expect (")");
			n->args = ah.next;
		}
		return n;
	}
	if (opt_cplusplus && (is ("delete") || (is ("::") && equal (T->next, "delete")))) {
		Node *n = new_node (ND_DELETE, t);
		accept ("::");
		advance ();
		if (accept ("[")) {
			expect ("]");
			n->is_array = 1;
		}
		n->lhs = cast_expr ();
		return n;
	}
	if (opt_cplusplus && (is_type_word (T) || is_type_ident (T)) && equal (T->next, "(")) {
		// T(args): a functional cast (a class's temporary, for M4)
		Node *n = new_node (ND_CAST, t), ah = {0}, *ac = &ah;
		Decl *d = xalloc (sizeof *d);
		d->tok = T;
		d->spec = xalloc (sizeof (Spec));
		d->spec->tok = T;
		add_word (d->spec, advance ());
		d->spec->is_typedef_name = !is_type_word (d->spec->words[0]);
		d->first_of_spec = 1;
		d->type = &base_type;
		n->decl = d;
		n->functional = 1;
		expect ("(");
		while (!is (")")) {
			ac = ac->next = assign ();
			if (!accept (","))
				break;
		}
		expect (")");
		n->args = ah.next;
		n->lhs = n->args;
		return postfix (n);
	}
	if (is ("sizeof")) {
		advance ();
		if (is ("(") && is_typename_start (T->next)) {
			Node *n;
			advance ();
			n = new_node (ND_SIZEOF_TYPE, t);
			n->decl = type_name ();
			expect (")");
			return n;
		}
		{
			Node *n = new_node (ND_SIZEOF_EXPR, t);
			n->lhs = unary ();
			return n;
		}
	}
	return postfix (primary ());
}

static Node *postfix (Node *n)
{
	for (;;) {
		Token *t = T;
		if (is ("[")) {
			Node *x = new_node (ND_INDEX, n->tok);
			advance ();
			x->lhs = n;
			x->rhs = expr ();
			expect ("]");
			n = x;
			continue;
		}
		if (is ("(")) {
			Node *c = new_node (ND_CALL, n->tok), head = {0}, *cur = &head;
			advance ();
			c->lhs = n;
			while (!is (")")) {
				cur = cur->next = assign ();
				if (!accept (","))
					break;
			}
			expect (")");
			c->args = head.next;
			n = c;
			continue;
		}
		if (is (".") || is ("->")) {
			Node *m = new_node (ND_MEMBER, n->tok);
			m->op = advance ();
			m->lhs = n;
			if (T->kind != TK_IDENT)
				error_at (T, "expected a member name");
			m->name = advance ();
			n = m;
			continue;
		}
		if (is ("++") || is ("--")) {
			Node *p = new_node (ND_POSTFIX, n->tok);
			p->op = advance ();
			p->lhs = n;
			n = p;
			continue;
		}
		(void)t;
		return n;
	}
}

static Node *primary (void)
{
	Token *t = T;
	if (is ("(")) {
		Node *n;
		advance ();
		n = expr ();
		expect (")");
		return n;
	}
	if (T->kind == TK_NUM) {
		advance ();
		return new_node (ND_NUM, t);
	}
	if (T->kind == TK_CHAR) {
		advance ();
		return new_node (ND_CHAR, t);
	}
	if (T->kind == TK_STR) {
		Node *n = new_node (ND_STR, t);
		while (T->kind == TK_STR) {
			advance ();
			n->ntoks++;
		}
		return n;
	}
	if (opt_cplusplus) {
		if (is ("true") || is ("false")) {
			advance ();
			return new_node (ND_NUM, make_ident (t, equal (t, "true") ? "1" : "0"));
		}
		if (is ("::") && T->next->kind == TK_IDENT) {
			// ::name, at file scope (sema looks it up from there)
			Node *n;
			advance ();
			n = new_node (ND_IDENT, advance ());
			n->qual = t;
			return n;
		}
		if (T->kind == TK_IDENT && equal (T->next, "::")) {
			// X::name
			Node *n;
			Token *q = advance ();
			advance ();
			if (is ("~") || is ("operator"))
				error_at (T, "explicit destructor and operator calls are not supported");
			if (T->kind != TK_IDENT)
				error_at (T, "expected a name after '::'");
			n = new_node (ND_IDENT, advance ());
			n->qual = q;
			return n;
		}
		if (is ("static_cast") || is ("dynamic_cast") || is ("reinterpret_cast") || is ("const_cast") ||
		    is ("typeid") || is ("throw") || is ("operator"))
			error_at (T, "'%.*s' is not supported", T->len, T->loc);
	}
	if (T->kind == TK_IDENT && !is_keyword (T)) {
		advance ();
		return new_node (ND_IDENT, t);
	}
	error_at (T, "expected an expression");
	return 0;
}

// ---- the unit ----

Node *parse (Token *t)
{
	Node head = {0}, *cur = &head;
	T = t;
	push_scope ();
	while (T->kind != TK_EOF) {
		if (T->kind == TK_PRAGMA) {
			cur = cur->next = new_node (ND_PRAGMA, advance ());
			continue;
		}
		if (accept (";"))
			continue;
		if (opt_cplusplus && is ("extern") && T->next->kind == TK_STR) {
			// extern "C" { ... } or extern "C" declaration
			advance ();
			advance ();
			if (accept ("{")) {
				extern_c++;
				continue;
			}
			extern_c++;
			cur = cur->next = declaration (1);
			extern_c--;
			continue;
		}
		if (opt_cplusplus && extern_c && is ("}")) {
			advance ();
			extern_c--;
			continue;
		}
		if (opt_cplusplus && (is ("template") || is ("namespace") || is ("using")))
			error_at (T, "'%.*s' is not supported", T->len, T->loc);
		cur = cur->next = declaration (1);
	}
	return head.next;
}
