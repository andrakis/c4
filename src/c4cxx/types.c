// types.c -- C types and their layout on c4's 32-bit target (types.h)
#include "types.h"

Ty *ty_void, *ty_bool, *ty_char, *ty_schar, *ty_uchar, *ty_short, *ty_ushort,
	*ty_int, *ty_uint, *ty_long, *ty_ulong, *ty_llong, *ty_ullong,
	*ty_float, *ty_double, *ty_ldouble;

Ty *new_ty (int kind, int size, int align)
{
	Ty *t = xalloc (sizeof *t);
	t->kind = kind;
	t->size = size;
	t->align = align;
	return t;
}

void types_init (void)
{
	ty_void = new_ty (T_VOID, 1, 1);
	ty_bool = new_ty (T_BOOL, 1, 1);
	ty_char = new_ty (T_CHAR, 1, 1);
	ty_schar = new_ty (T_SCHAR, 1, 1);
	ty_uchar = new_ty (T_UCHAR, 1, 1);
	ty_short = new_ty (T_SHORT, 2, 2);
	ty_ushort = new_ty (T_USHORT, 2, 2);
	ty_int = new_ty (T_INT, 4, 4);
	ty_uint = new_ty (T_UINT, 4, 4);
	ty_long = new_ty (T_LONG, 4, 4);
	ty_ulong = new_ty (T_ULONG, 4, 4);
	ty_llong = new_ty (T_LLONG, 8, 4);
	ty_ullong = new_ty (T_ULLONG, 8, 4);
	ty_float = new_ty (T_FLOAT, 4, 4);
	ty_double = new_ty (T_DOUBLE, 8, 4);
	ty_ldouble = new_ty (T_LDOUBLE, 12, 4);
}

Ty *pointer_to (Ty *t)
{
	Ty *p = new_ty (T_PTR, 4, 4);
	p->base = t;
	return p;
}

Ty *array_of (Ty *t, int len)
{
	Ty *a = new_ty (T_ARRAY, len < 0 || t->size < 0 ? -1 : t->size * len, t->align);
	a->base = t;
	a->len = len;
	return a;
}

Ty *func_type (Ty *ret)
{
	Ty *f = new_ty (T_FUNC, 1, 1);
	f->base = ret;
	return f;
}

Ty *qualified (Ty *t, int qual)
{
	Ty *c;
	if (!qual || (t->qual & qual) == qual)
		return t;
	if (t->kind == T_ARRAY) {
		// qualifiers of an array are its elements'
		c = array_of (qualified (t->base, qual), t->len);
		return c;
	}
	c = xalloc (sizeof *c);
	*c = *t;
	c->qual |= qual;
	return c;
}

Ty *unqualified (Ty *t)
{
	Ty *c;
	if (!t->qual)
		return t;
	c = xalloc (sizeof *c);
	*c = *t;
	c->qual = 0;
	return c;
}

int is_integer (Ty *t)
{
	return (t->kind >= T_BOOL && t->kind <= T_ULLONG) || t->kind == T_ENUM;
}

int is_arith (Ty *t)
{
	return is_integer (t) || (t->kind >= T_FLOAT && t->kind <= T_LDOUBLE);
}

int is_ptr (Ty *t)
{
	return t->kind == T_PTR;
}

int is_scalar (Ty *t)
{
	return is_arith (t) || is_ptr (t);
}

int is_unsigned (Ty *t)
{
	switch (t->kind) {
	case T_BOOL: case T_UCHAR: case T_USHORT: case T_UINT: case T_ULONG: case T_ULLONG:
		return 1;
	case T_ENUM:
		return t->def && t->def->is_unsigned_enum;
	}
	return 0;
}

int same_type (Ty *a, Ty *b)
{
	int i;
	if (a == b)
		return 1;
	if (a->kind != b->kind)
		return 0;
	switch (a->kind) {
	case T_PTR:
		return (a->base->qual == b->base->qual) && same_type (a->base, b->base);
	case T_ARRAY:
		return (a->len == b->len || a->len < 0 || b->len < 0) && same_type (a->base, b->base);
	case T_FUNC:
		if (!same_type (a->base, b->base))
			return 0;
		if (a->oldstyle || b->oldstyle)
			return 1;
		if (a->nparams != b->nparams || a->variadic != b->variadic)
			return 0;
		for (i = 0; i < a->nparams; i++)
			if (!same_type (a->params[i], b->params[i]))
				return 0;
		return 1;
	case T_STRUCT: case T_UNION: case T_ENUM:
		return a->def == b->def;
	}
	return 1;
}

Ty *decay (Ty *t)
{
	if (t->kind == T_ARRAY)
		return pointer_to (t->base);
	if (t->kind == T_FUNC)
		return pointer_to (t);
	return t;
}

// the integer promotions; gcc's enum promotes as the type it is
Ty *promote (Ty *t)
{
	switch (t->kind) {
	case T_BOOL: case T_CHAR: case T_SCHAR: case T_UCHAR: case T_SHORT: case T_USHORT:
		return ty_int;
	case T_ENUM:
		return is_unsigned (t) ? ty_uint : ty_int;
	}
	return unqualified (t);
}

static int rank (Ty *t)
{
	switch (t->kind) {
	case T_INT: case T_UINT: return 3;
	case T_LONG: case T_ULONG: return 4;
	case T_LLONG: case T_ULLONG: return 5;
	}
	return 0;
}

Ty *usual_arith (Ty *a, Ty *b)
{
	if (a->kind == T_LDOUBLE || b->kind == T_LDOUBLE) return ty_ldouble;
	if (a->kind == T_DOUBLE || b->kind == T_DOUBLE) return ty_double;
	if (a->kind == T_FLOAT || b->kind == T_FLOAT) return ty_float;
	a = promote (a);
	b = promote (b);
	if (a->kind == b->kind)
		return a;
	if (is_unsigned (a) == is_unsigned (b))
		return rank (a) >= rank (b) ? a : b;
	{
		Ty *u = is_unsigned (a) ? a : b, *s = is_unsigned (a) ? b : a;
		if (rank (u) >= rank (s))
			return u;
		if (s->size > u->size)
			return s;
		// the signed type cannot hold all the unsigned's values
		switch (s->kind) {
		case T_LONG: return ty_ulong;
		case T_LLONG: return ty_ullong;
		}
		return ty_uint;
	}
}

static int align_to (int n, int a)
{
	return a > 1 ? (n + a - 1) / a * a : n;
}

// gcc -m32's layout. PACK is #pragma pack's value (0: none), or 1 for
// __attribute__((packed)).
void layout (Ty *rec, int pack)
{
	Field *f;
	int bits = 0, align = 1, size = 0;
	for (f = rec->fields; f; f = f->next) {
		Ty *t = f->ty;
		int a = t->align;
		if (pack && a > pack)
			a = pack;
		if (rec->kind == T_UNION) {
			f->offset = 0;
			f->bit_offset = 0;
			if (f->bit_width >= 0) {
				if (f->name && a > align) align = a;
				if ((f->bit_width + 7) / 8 > size) size = (f->bit_width + 7) / 8;
			} else {
				if (a > align) align = a;
				if (t->size > size) size = t->size;
			}
			continue;
		}
		if (f->bit_width >= 0) {
			int w = f->bit_width, unit = t->size * 8;
			if (w == 0) {
				bits = align_to (bits, t->align * 8);
				f->offset = bits / 8;
				f->bit_offset = 0;
				continue;
			}
			// gcc: it may not run past a unit of its type counted from a
			// boundary of its (packed) alignment; if it would, it starts at the
			// next boundary. Under pack(1) that is almost never.
			if ((bits % (a * 8)) + w > unit)
				bits = align_to (bits, a * 8);
			f->offset = bits / 8;
			f->bit_offset = bits % 8;
			bits += w;
			if (f->name && a > align)
				align = a;
			continue;
		}
		bits = align_to (bits, a * 8);
		f->offset = bits / 8;
		f->bit_offset = 0;
		if (t->size < 0) {
			// a flexible array member last
			continue;
		}
		bits += t->size * 8;
		if (a > align)
			align = a;
	}
	if (rec->kind == T_UNION)
		rec->size = align_to (size, align);
	else
		rec->size = align_to ((bits + 7) / 8, align);
	rec->align = align;
	rec->complete = 1;
}

Field *find_field (Ty *rec, Token *name, int *offset)
{
	Field *f;
	if (!rec->def)
		return 0;
	for (f = rec->def->fields; f; f = f->next) {
		if (f->name && f->name->len == name->len && !memcmp (f->name->loc, name->loc, name->len)) {
			if (offset) *offset += f->offset;
			return f;
		}
		if (!f->name && (f->ty->kind == T_STRUCT || f->ty->kind == T_UNION)) {
			int o = f->offset;
			Field *g = find_field (f->ty, name, &o);
			if (g) {
				if (offset) *offset += o;
				return g;
			}
		}
	}
	return 0;
}

// ---- as C ----

static const char *base_name (Ty *t)
{
	switch (t->kind) {
	case T_VOID: return "void";
	case T_BOOL: return "_Bool";
	case T_CHAR: return "char";
	case T_SCHAR: return "signed char";
	case T_UCHAR: return "unsigned char";
	case T_SHORT: return "short";
	case T_USHORT: return "unsigned short";
	case T_INT: return "int";
	case T_UINT: return "unsigned int";
	case T_LONG: return "long";
	case T_ULONG: return "unsigned long";
	case T_LLONG: return "long long";
	case T_ULLONG: return "unsigned long long";
	case T_FLOAT: return "float";
	case T_DOUBLE: return "double";
	case T_LDOUBLE: return "long double";
	}
	return 0;
}

static char *quals (Ty *t, const char *s)
{
	if (t->qual & Q_CONST)
		s = xformat ("const %s", s);
	if (t->qual & Q_VOLATILE)
		s = xformat ("volatile %s", s);
	return (char *)s;
}

char *ty_str (Ty *t, const char *inner)
{
	const char *b = base_name (t);
	char *sp = (char *)(*inner ? " " : "");
	if (b)
		return xformat ("%s%s%s", quals (t, b), sp, inner);
	switch (t->kind) {
	case T_STRUCT: case T_UNION: case T_ENUM: {
		Ty *d = t->def;
		char *n;
		if (d->tag)
			n = xformat ("%s %.*s", t->kind == T_STRUCT ? "struct" : t->kind == T_UNION ? "union" : "enum",
			             d->tag->len, d->tag->loc);
		else if (d->name)
			n = d->name;
		else if (t->kind == T_ENUM)
			n = (char *)(d->is_unsigned_enum ? "unsigned int" : "int");
		else
			return 0;
		return xformat ("%s%s%s", quals (t, n), sp, inner);
	}
	case T_PTR: {
		char *s = xformat ("*%s%s%s", t->qual & Q_CONST ? " const" : "", t->qual & Q_VOLATILE ? " volatile" : "",
		                   *inner ? xformat (" %s", inner) : "");
		if (t->base->kind == T_ARRAY || t->base->kind == T_FUNC)
			s = xformat ("(%s)", s);
		return ty_str (t->base, s);
	}
	case T_ARRAY:
		if (t->len < 0)
			return ty_str (t->base, xformat ("%s[]", inner));
		return ty_str (t->base, xformat ("%s[%d]", inner, t->len));
	case T_FUNC: {
		Buf b = {0};
		int i;
		buf_str (&b, inner);
		buf_str (&b, "(");
		if (!t->oldstyle) {
			if (!t->nparams && !t->variadic)
				buf_str (&b, "void");
			for (i = 0; i < t->nparams; i++) {
				char *p = ty_str (t->params[i], "");
				if (!p)
					return 0;
				if (i)
					buf_str (&b, ", ");
				buf_str (&b, p);
			}
			if (t->variadic)
				buf_str (&b, t->nparams ? ", ..." : "...");
		}
		buf_str (&b, ")");
		return ty_str (t->base, b.p);
	}
	}
	return 0;
}
