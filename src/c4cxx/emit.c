// emit.c -- the syntax tree back to C
//
// Declarations print from their Specs and declarator chains; expressions
// with the parentheses precedence needs and no more; each statement and
// declaration after the line it came from, with a `# N "file"` marker when
// the file changes or the line jumps, which c4lc honours under -g.
#include "types.h"

int emit_check;

static Buf out;
static File *cur_file;
static int cur_line;

static void put (const char *s) { buf_str (&out, s); }
static void putn (const char *s, int n) { buf_add (&out, s, n); }
static void puttok (Token *t) { putn (t->loc, t->len); }

static void nl (void)
{
	buf_char (&out, '\n');
	cur_line++;
}

static void mark (Token *t)
{
	if (!t || !t->file || t->kind == TK_EOF)
		return;
	if (t->file != cur_file || t->line < cur_line || t->line > cur_line + 8) {
		if (out.n && out.p[out.n - 1] != '\n')
			buf_char (&out, '\n');
		buf_fmt (&out, "# %d \"%s\"\n", t->line, t->file->name);
		cur_file = t->file;
		cur_line = t->line;
		return;
	}
	while (cur_line < t->line)
		nl ();
	if (out.n && out.p[out.n - 1] != '\n')
		buf_char (&out, ' ');
}

// ---- strings built apart (declarators nest inside out) ----

static char *take (Buf *saved)
{
	char *s = out.p ? out.p : xstrndup ("", 0);
	out = *saved;
	return s;
}

static Buf begin (void)
{
	Buf saved = out;
	out.p = 0; out.n = 0; out.cap = 0;
	buf_add (&out, "", 0);
	return saved;
}

static void expr (Node *n, int minprec);
static void const_expr (Node *n, int minprec);
static void initz (Init *in);
static void spec (Spec *sp);
static char *declarator_str (Type *t, char *inner);
static void stmt (Node *n);

static void attrs (Attr *a)
{
	for (; a; a = a->next) {
		Token *t;
		put (" ");
		for (t = a->start; t != a->end; t = t->next) {
			if (t != a->start)
				put (" ");
			puttok (t);
		}
	}
}

static char *expr_str (Node *n, int minprec)
{
	Buf saved = begin ();
	expr (n, minprec);
	return take (&saved);
}

static char *params_str (Type *f)
{
	Buf saved = begin ();
	Decl *p;
	if (f->oldstyle) {
		for (p = f->params; p; p = p->next) {
			if (p != f->params)
				put (", ");
			puttok (p->name);
		}
		return take (&saved);
	}
	if (!f->params && !f->variadic) {
		put ("void");
		return take (&saved);
	}
	for (p = f->params; p; p = p->next) {
		char *d;
		if (p != f->params)
			put (", ");
		spec (p->spec);
		d = declarator_str (p->type, p->name ? xstrndup (p->name->loc, p->name->len) : "");
		if (*d) {
			put (" ");
			put (d);
		}
		attrs (p->attrs);
	}
	if (f->variadic)
		put (f->params ? ", ..." : "...");
	return take (&saved);
}

static char *declarator_str (Type *t, char *inner)
{
	if (!t || t->kind == TY_BASE)
		return inner;
	if (t->kind == TY_PTR || t->kind == TY_REF) {
		// a reference is a pointer underneath
		Buf saved = begin ();
		put ("*");
		if (t->qual & Q_CONST) put (" const");
		if (t->qual & Q_VOLATILE) put (" volatile");
		if (t->qual & Q_RESTRICT) put (" restrict");
		attrs (t->attrs);
		if (*inner) {
			if (t->qual || t->attrs)
				put (" ");
			put (inner);
		}
		inner = take (&saved);
		if (t->base && (t->base->kind == TY_ARRAY || t->base->kind == TY_FUNC))
			inner = xformat ("(%s)", inner);
		return declarator_str (t->base, inner);
	}
	if (t->kind == TY_ARRAY) {
		char *len = "";
		if (t->len) {
			Buf saved = begin ();
			const_expr (t->len, 3);
			len = take (&saved);
		}
		return declarator_str (t->base, xformat ("%s[%s]", inner, len));
	}
	return declarator_str (t->base, xformat ("%s(%s)", inner, params_str (t)));
}

// ---- specifiers ----

static void group (Decl *d, int members);

static void spec (Spec *sp)
{
	int first = 1;
	int i;
#define W(s) do { if (!first) put (" "); put (s); first = 0; } while (0)
	if (sp->storage & ST_TYPEDEF) W ("typedef");
	if (sp->storage & ST_EXTERN) W ("extern");
	if (sp->storage & ST_STATIC) W ("static");
	if (sp->storage & ST_AUTO) W ("auto");
	if (sp->storage & ST_REGISTER) W ("register");
	if (sp->storage & ST_INLINE) W ("inline");
	if (sp->qual & Q_CONST) W ("const");
	if (sp->qual & Q_VOLATILE) W ("volatile");
	if (sp->qual & Q_RESTRICT) W ("restrict");
	if (sp->kind == SP_WORDS) {
		if (sp->kw)
			W (sp->kw);	// C++: a class named by its tag
		for (i = 0; i < sp->nwords; i++) {
			if (!first)
				put (" ");
			if (opt_cplusplus && equal (sp->words[i], "bool"))
				put ("_Bool");
			else
				puttok (sp->words[i]);
			first = 0;
		}
	} else {
		W (sp->kind == SP_STRUCT ? "struct" : sp->kind == SP_UNION ? "union" : "enum");
		attrs (sp->tag_attrs);
		if (sp->tag) {
			put (" ");
			puttok (sp->tag);
		}
		if (sp->has_body) {
			put (" {");
			if (sp->kind == SP_ENUM) {
				Enumer *e;
				for (e = sp->enumers; e; e = e->next) {
					put (" ");
					puttok (e->name);
					if (e->value) {
						put (" = ");
						const_expr (e->value, 3);
					}
					if (e->next)
						put (",");
				}
				put (" }");
			} else {
				Decl *m = sp->members;
				while (m) {
					Decl *g = m;
					if (m->is_pragma) {
						buf_char (&out, '\n');
						puttok (m->tok);
						buf_char (&out, '\n');
						m = m->next;
						continue;
					}
					put (" ");
					group (g, 1);
					put (";");
					m = m->next;
					while (m && !m->first_of_spec && !m->is_pragma)
						m = m->next;
				}
				put (" }");
			}
		}
	}
	attrs (sp->attrs);
#undef W
}

// one declarator of a declaration
static void one (Decl *d)
{
	char *s = declarator_str (d->type, d->name ? xstrndup (d->name->loc, d->name->len) : "");
	if (*s) {
		if (out.n && out.p[out.n - 1] != ' ')
			put (" ");
		put (s);
	}
	if (d->width) {
		put (" : ");
		const_expr (d->width, 3);
	}
	attrs (d->attrs);
	if (d->init) {
		put (" = ");
		initz (d->init);
	}
}

// a Spec and its declarators (from D, while they share it)
static void group (Decl *d, int members)
{
	Spec *sp = d->spec;
	int first = 1;
	(void)members;
	spec (sp);
	for (; d && d->spec == sp && (first || !d->first_of_spec); d = d->next) {
		if (!d->name && !d->width && d->type == 0)
			break;
		if (!first)
			put (",");
		one (d);
		first = 0;
	}
}

// ---- initializers ----

static void initz (Init *in)
{
	InitItem *it;
	if (!in->is_list) {
		const_expr (in->expr, 2);	// C++: a const variable as its value
		return;
	}
	put ("{");
	for (it = in->items; it; it = it->next) {
		Desig *d;
		put (" ");
		for (d = it->desig; d; d = d->next) {
			if (d->field) {
				put (".");
				puttok (d->field);
			} else {
				put ("[");
				expr (d->index, 3);
				put ("]");
			}
		}
		if (it->desig)
			put (" = ");
		initz (it->init);
		if (it->next)
			put (",");
	}
	put (" }");
}

// ---- expressions ----

static int binprec (Token *op)
{
	static const char *ops[] = { "||", "&&", "|", "^", "&", "==", "!=", "<", ">", "<=", ">=",
		"<<", ">>", "+", "-", "*", "/", "%", 0 };
	static const int prec[] = { 4, 5, 6, 7, 8, 9, 9, 10, 10, 10, 10, 11, 11, 12, 12, 13, 13, 13 };
	int i;
	for (i = 0; ops[i]; i++)
		if (equal (op, ops[i]))
			return prec[i];
	return 4;
}

static int prec (Node *n)
{
	switch (n->kind) {
	case ND_COMMA: return 1;
	case ND_ASSIGN: return 2;
	case ND_COND: return 3;
	case ND_BINARY: return binprec (n->op);
	case ND_CAST: case ND_PREFIX: case ND_SIZEOF_TYPE: case ND_SIZEOF_EXPR: return 14;
	case ND_POSTFIX: case ND_CALL: case ND_INDEX: case ND_MEMBER: case ND_COMPLIT: case ND_TMPREF: return 15;
	default: return 16;
	}
}

static char *type_name_str (Decl *d)
{
	Buf saved = begin ();
	char *s;
	spec (d->spec);
	s = declarator_str (d->type, "");
	if (*s) {
		put (" ");
		put (s);
	}
	return take (&saved);
}

// C++: where C wants a constant (array sizes, case labels, enumerators,
// bit-field widths), an expression through a const variable is its value
static void const_expr (Node *n, int minprec)
{
	if (opt_cplusplus && n->is_const && n->cvar && n->ty && is_integer (n->ty)) {
		long long v = n->val;
		if (is_unsigned (n->ty))
			buf_fmt (&out, "%lluU", (unsigned long long)v);
		else if (v < 0)
			buf_fmt (&out, minprec > 3 ? "(%lld)" : "%lld", v);
		else
			buf_fmt (&out, "%lld", v);
		return;
	}
	expr (n, minprec);
}

static void expr (Node *n, int minprec)
{
	int p = prec (n), paren = p < minprec;
	Node *a;
	int i;
	Token *t;
	if (paren)
		put ("(");
	switch (n->kind) {
	case ND_NUM: case ND_CHAR: case ND_IDENT:
		puttok (n->tok);
		break;
	case ND_STR:
		for (t = n->tok, i = 0; i < n->ntoks; i++, t = t->next) {
			if (i)
				put (" ");
			puttok (t);
		}
		break;
	case ND_BINARY:
		expr (n->lhs, p);
		put (" ");
		puttok (n->op);
		put (" ");
		expr (n->rhs, p + 1);
		break;
	case ND_ASSIGN:
		expr (n->lhs, 14);
		put (" ");
		puttok (n->op);
		put (" ");
		expr (n->rhs, 2);
		break;
	case ND_COND:
		expr (n->cond, 4);
		put (" ? ");
		expr (n->then, 1);
		put (" : ");
		expr (n->els, 3);
		break;
	case ND_COMMA:
		expr (n->lhs, 1);
		put (", ");
		expr (n->rhs, 2);
		break;
	case ND_PREFIX:
		puttok (n->op);
		put (" ");
		expr (n->lhs, 14);
		break;
	case ND_POSTFIX:
		expr (n->lhs, 15);
		put (" ");
		puttok (n->op);
		break;
	case ND_CAST:
		put ("(");
		put (type_name_str (n->decl));
		put (") ");
		expr (n->lhs, 14);
		break;
	case ND_SIZEOF_TYPE:
		put ("sizeof (");
		put (type_name_str (n->decl));
		put (")");
		break;
	case ND_SIZEOF_EXPR:
		put ("sizeof ");
		expr (n->lhs, 14);
		break;
	case ND_COMPLIT:
		put ("(");
		put (type_name_str (n->decl));
		put (") ");
		initz (n->initz);
		break;
	case ND_CALL:
		expr (n->lhs, 15);
		put (" (");
		for (a = n->args; a; a = a->next) {
			if (a != n->args)
				put (", ");
			expr (a, 2);
		}
		put (")");
		break;
	case ND_INDEX:
		expr (n->lhs, 15);
		put (" [");
		expr (n->rhs, 1);
		put ("]");
		break;
	case ND_MEMBER:
		expr (n->lhs, 15);
		put (" ");
		puttok (n->op);
		put (" ");
		puttok (n->name);
		break;
	case ND_TMPREF: {
		// a const reference to an rvalue: a compound literal, an array of one
		// so that it is the pointer (c4lc takes no scalar compound literal)
		char *t = ty_str (unqualified (n->ty->base), "[1]");
		if (!t)
			error_at (n->tok, "emit: a temporary of a type without a name");
		buf_fmt (&out, "(%s) { ", t);
		expr (n->lhs, 2);
		put (" }");
		break;
	}
	default:
		error_at (n->tok, "emit: not an expression (%d)", n->kind);
	}
	if (paren)
		put (")");
}

// ---- -check-types: assertions for gcc -m32 ----

static void check_expr (Node *n);

static void check_children (Node *n)
{
	Node *a;
	// a function declared by its call is not to be named before it
	if (n->lhs && !(n->kind == ND_CALL && (n->implicit || n->lhs->kind == ND_IDENT && n->lhs->ty && n->lhs->ty->oldstyle)))
		check_expr (n->lhs);
	if (n->rhs) check_expr (n->rhs);
	if (n->cond) check_expr (n->cond);
	if (n->then) check_expr (n->then);
	if (n->els) check_expr (n->els);
	if (n->kind == ND_CALL)
		for (a = n->args; a; a = a->next)
			check_expr (a);
}

static void check_expr (Node *n)
{
	char *t;
	if (!emit_check || !n)
		return;
	check_children (n);
	if (!n->ty || n->bitfield || n->unsure)
		return;
	t = ty_str (n->ty, "");
	if (!t)
		return;
	put (" _Static_assert (__builtin_types_compatible_p (__typeof__ (");
	expr (n, 1);
	buf_fmt (&out, "), %s), \"c4cxx: the type at %s:%d\");", t, n->tok->file ? n->tok->file->name : "", n->tok->line);
	if (n->is_const && !n->cvar && is_integer (n->ty) && n->kind != ND_NUM && n->kind != ND_CHAR) {
		put (" _Static_assert ((");
		expr (n, 1);
		buf_fmt (&out, ") == (%lldLL), \"c4cxx: the value at %s:%d\");", n->val,
		         n->tok->file ? n->tok->file->name : "", n->tok->line);
	}
}

static void check_init (Init *in)
{
	InitItem *it;
	if (!in)
		return;
	if (!in->is_list) {
		check_expr (in->expr);
		return;
	}
	for (it = in->items; it; it = it->next)
		check_init (it->init);
}

static void check_record (Spec *sp)
{
	Ty *r = sp->rty;
	char *t;
	Decl *m;
	Field *f;
	if (!emit_check || !r || !sp->has_body)
		return;
	for (m = sp->members; m; m = m->next)
		if (!m->is_pragma && m->first_of_spec && m->spec && m->spec->has_body)
			check_record (m->spec);
	if (sp->kind == SP_ENUM) {
		Enumer *e;
		for (e = sp->enumers; e; e = e->next)
			buf_fmt (&out, " _Static_assert ((%.*s) == (%lldLL), \"c4cxx: the value of %.*s\");", e->name->len,
			         e->name->loc, e->val, e->name->len, e->name->loc);
		return;
	}
	if (r->kind != T_STRUCT && r->kind != T_UNION)
		return;
	t = ty_str (r, "");
	if (!t || !r->def->complete)
		return;
	buf_fmt (&out, " _Static_assert (sizeof (%s) == %d, \"c4cxx: sizeof %s\");", t, r->def->size, t);
	for (f = r->def->fields; f; f = f->next)
		if (f->name && f->bit_width < 0)
			buf_fmt (&out, " _Static_assert (__builtin_offsetof (%s, %.*s) == %d, \"c4cxx: offset of %.*s\");",
			         t, f->name->len, f->name->loc, f->offset, f->name->len, f->name->loc);
}

static void check_decls (Decl *d, int top)
{
	if (!emit_check)
		return;
	for (; d; d = d->next) {
		Ty *t = d->rty;
		if (d->first_of_spec && d->spec)
			check_record (d->spec);
		check_init (d->init);
		if (top && d->name && t && t->kind != T_FUNC && !(d->spec->storage & ST_TYPEDEF) && t->size > 0 &&
		    ty_str (t, ""))
			buf_fmt (&out, " _Static_assert (sizeof (%.*s) == %d, \"c4cxx: sizeof %.*s\");", d->name->len,
			         d->name->loc, t->size, d->name->len, d->name->loc);
	}
}

// ---- statements ----

static void decl_stmt (Decl *d)
{
	Decl *g = d, *k;
	while (g) {
		group (g, 0);
		put (";");
		// C++: constructor calls, in order, after the declaration
		for (k = g; k && (k == g || !k->first_of_spec); k = k->next)
			if (k->ctor) {
				put (" ");
				expr (k->ctor->lhs, 1);
				put (";");
			}
		g = g->next;
		while (g && !g->first_of_spec)
			g = g->next;
		if (g)
			put (" ");
	}
}

// C++: destructor calls before a jump or a block's end
static void cleanup (Node *s)
{
	for (; s; s = s->next) {
		put (" ");
		expr (s->lhs, 1);
		put (";");
	}
}

static void stmt (Node *n);

// a statement under if, while, for, do, case or a label: in braces under
// -check-types, where assertions (declarations) may come before it
static void sub (Node *n)
{
	if (emit_check && n->kind != ND_BLOCK) {
		put (" {");
		stmt (n);
		put (" }");
	} else
		stmt (n);
}

static void stmt (Node *n)
{
	Node *s;
	if (n->kind == ND_PRAGMA) {
		if (out.n && out.p[out.n - 1] != '\n')
			nl ();
		puttok (n->tok);
		nl ();
		return;
	}
	mark (n->tok);
	switch (n->kind) {
	case ND_BLOCK:
		put ("{");
		for (s = n->body; s; s = s->next)
			stmt (s);
		cleanup (n->cleanup);
		put (" }");
		break;
	case ND_NULL:
		put (";");
		break;
	case ND_EXPR_STMT:
		check_expr (n->lhs);
		expr (n->lhs, 1);
		put (";");
		break;
	case ND_DECL:
		decl_stmt (n->decl);
		check_decls (n->decl, 0);
		break;
	case ND_IF:
		check_expr (n->cond);
		put ("if (");
		expr (n->cond, 1);
		put (")");
		sub (n->then);
		if (n->els) {
			put (" else");
			sub (n->els);
		}
		break;
	case ND_WHILE:
		check_expr (n->cond);
		put ("while (");
		expr (n->cond, 1);
		put (")");
		sub (n->body);
		break;
	case ND_DO:
		put ("do");
		sub (n->body);
		put (" while (");
		expr (n->cond, 1);
		put (");");
		check_expr (n->cond);
		break;
	case ND_FOR:
		if (!n->decl) {
			check_expr (n->init_expr);
			check_expr (n->cond);
			check_expr (n->inc);
		}
		put ("for (");
		if (n->decl)
			decl_stmt (n->decl);
		else {
			if (n->init_expr)
				expr (n->init_expr, 1);
			put (";");
		}
		put (" ");
		if (n->cond)
			expr (n->cond, 1);
		put ("; ");
		if (n->inc)
			expr (n->inc, 1);
		put (")");
		if (n->decl && emit_check) {
			put (" {");
			check_decls (n->decl, 0);
			check_expr (n->cond);
			check_expr (n->inc);
			sub (n->body);
			put (" }");
		} else
			sub (n->body);
		break;
	case ND_SWITCH:
		check_expr (n->cond);
		put ("switch (");
		expr (n->cond, 1);
		put (")");
		sub (n->body);
		break;
	case ND_CASE:
		put ("case ");
		const_expr (n->cond, 3);
		put (":");
		sub (n->body);
		break;
	case ND_DEFAULT:
		put ("default:");
		sub (n->body);
		break;
	case ND_BREAK:
		if (n->cleanup) {
			put ("{");
			cleanup (n->cleanup);
			put (" break; }");
		} else
			put ("break;");
		break;
	case ND_CONTINUE:
		if (n->cleanup) {
			put ("{");
			cleanup (n->cleanup);
			put (" continue; }");
		} else
			put ("continue;");
		break;
	case ND_RETURN:
		check_expr (n->lhs);
		if (n->cleanup) {
			// the value first, then the destructors
			put ("{");
			if (n->lhs) {
				char *t = ty_str (n->ty, "__c4x_r");
				if (!t)
					error_at (n->tok, "emit: a return value of a type without a name");
				buf_fmt (&out, " %s = ", t);
				expr (n->lhs, 2);
				put (";");
			}
			cleanup (n->cleanup);
			put (n->lhs ? " return __c4x_r; }" : " return; }");
			break;
		}
		put ("return");
		if (n->lhs) {
			put (" ");
			expr (n->lhs, 1);
		}
		put (";");
		break;
	case ND_GOTO:
		put ("goto ");
		puttok (n->name);
		put (";");
		break;
	case ND_LABEL:
		puttok (n->name);
		put (":");
		sub (n->body);
		break;
	default:
		error_at (n->tok, "emit: not a statement (%d)", n->kind);
	}
}

static void funcdef (Node *n)
{
	Decl *d = n->decl, *k;
	mark (n->tok);
	spec (d->spec);
	one (d);
	for (k = d->kr; k; ) {
		Decl *g = k;
		put (" ");
		group (g, 0);
		put (";");
		k = k->next;
		while (k && !k->first_of_spec)
			k = k->next;
	}
	stmt (n->body);
}

// a top-level node, with what lowering put before and after it
static void top (Node *n)
{
	Node *p;
	for (p = n->pre; p; p = p->next)
		top (p);
	if (n->kind == ND_FUNCDEF)
		funcdef (n);
	else if (n->kind == ND_PRAGMA)
		stmt (n);
	else if (n->kind == ND_RAWFUNC) {
		if (out.n && out.p[out.n - 1] != '\n')
			nl ();
		puttok (n->tok);
		stmt (n->body);
	} else {
		mark (n->tok);
		decl_stmt (n->decl);
		check_decls (n->decl, 1);
	}
	for (p = n->post; p; p = p->next)
		top (p);
}

void emit_unit (FILE *f, Node *unit)
{
	Node *n;
	out.p = 0; out.n = 0; out.cap = 0;
	buf_add (&out, "", 0);
	cur_file = 0;
	cur_line = 0;
	for (n = unit; n; n = n->next)
		top (n);
	buf_char (&out, '\n');
	fwrite (out.p, 1, out.n, f);
}
