// scrt.h -- the runtime c4sc's output calls.
//
// There is deliberately almost nothing here. Compiled code keeps using
// c4sp's OWN cells (cells.h), its OWN allocator and its OWN collector
// (gc.h) -- the collector is conservative over the C4 stack, and its
// own comment records that under c4m the scan is exact by construction,
// so a generated function's locals are rooted for free. No shadow
// stack, no write barrier, no root registration.
//
// What these are is the builtins WITHOUT the call protocol. c4sp reaches
// `head` by evaluating the atom, consing a one-element argument list and
// running a thirty-way if-chain; a compiled call reaches sc_head(x) with
// the pointer already in a register. The builtin's body is identical.
// docs/c4sc-design.md.
//
// The polymorphic ones (+ on ints, strings and lists) keep the fast path
// inline and fall back to c4sp's own builtin for the rest, so there is
// exactly one implementation of the awkward cases.

// -- truth -------------------------------------------------------------
// c4sp's rule: only the atom `false` is falsy. cell_is_false is cells.h's.
int sc_true (int *x) { return !cell_is_false(x); }

int *sc_bool (int v) { return bool_cell(v); }

// -- lists -------------------------------------------------------------
int *sc_cons (int *a, int *d) { return cons(a, d); }
int *sc_head (int *l) { return (cell_type(l) == T_CONS) ? (int *)l[CELL_A] : 0; }
int *sc_tail (int *l) { return (cell_type(l) == T_CONS) ? (int *)l[CELL_B] : 0; }
int *sc_empty (int *l) { return sc_bool(cell_type(l) != T_CONS); }

int *sc_index (int *l, int *n) {
	int i;
	i = (cell_type(n) == T_INT) ? n[CELL_A] : 0;
	while (i > 0 && cell_type(l) == T_CONS) { l = (int *)l[CELL_B]; --i; }
	return (cell_type(l) == T_CONS) ? (int *)l[CELL_A] : 0;
}

int *sc_length (int *l) {
	int n;
	n = 0;
	if (cell_type(l) == T_STRING) return mk_int(l[CELL_B]);
	while (cell_type(l) == T_CONS) { ++n; l = (int *)l[CELL_B]; }
	return mk_int(n);
}

// -- arithmetic --------------------------------------------------------
// `+` is c4sp's polymorphic one: numbers add, strings and lists join.
int *sc_add (int *a, int *b) {
	if (cell_type(a) == T_INT && cell_type(b) == T_INT)
		return mk_int(a[CELL_A] + b[CELL_A]);
	return builtin_add(cons(a, cons(b, 0)));
}
int *sc_sub (int *a, int *b) {
	if (cell_type(a) == T_INT && cell_type(b) == T_INT)
		return mk_int(a[CELL_A] - b[CELL_A]);
	return builtin_numeric(B_SUB, cons(a, cons(b, 0)));
}
int *sc_mul (int *a, int *b) {
	if (cell_type(a) == T_INT && cell_type(b) == T_INT)
		return mk_int(a[CELL_A] * b[CELL_A]);
	return builtin_numeric(B_MUL, cons(a, cons(b, 0)));
}
int *sc_div (int *a, int *b) {
	if (cell_type(a) == T_INT && cell_type(b) == T_INT && b[CELL_A])
		return mk_int(a[CELL_A] / b[CELL_A]);
	return builtin_numeric(B_DIV, cons(a, cons(b, 0)));
}

// -- comparison --------------------------------------------------------
// `=` is structural in c4sp, so the general case goes to its builtin;
// two ints are the overwhelming majority and are answered here.
int *sc_eq (int *a, int *b) {
	if (cell_type(a) == T_INT && cell_type(b) == T_INT)
		return sc_bool(a[CELL_A] == b[CELL_A]);
	return bool_cell(cell_equal(a, b));
}
int *sc_ne (int *a, int *b) { return sc_bool(!sc_true(sc_eq(a, b))); }

int sc_num (int *x) { return (cell_type(x) == T_INT) ? x[CELL_A] : 0; }
int *sc_lt (int *a, int *b) { return sc_bool(sc_num(a) <  sc_num(b)); }
int *sc_le (int *a, int *b) { return sc_bool(sc_num(a) <= sc_num(b)); }
int *sc_gt (int *a, int *b) { return sc_bool(sc_num(a) >  sc_num(b)); }
int *sc_ge (int *a, int *b) { return sc_bool(sc_num(a) >= sc_num(b)); }
int *sc_not (int *a) { return sc_bool(!sc_true(a)); }

// -- bits --------------------------------------------------------------
int *sc_bitand (int *a, int *b) { return mk_int(sc_num(a) &  sc_num(b)); }
int *sc_bitor  (int *a, int *b) { return mk_int(sc_num(a) |  sc_num(b)); }
int *sc_bitxor (int *a, int *b) { return mk_int(sc_num(a) ^  sc_num(b)); }
int *sc_bitshl (int *a, int *b) { return mk_int(sc_num(a) << sc_num(b)); }
int *sc_bitshr (int *a, int *b) { return mk_int(sc_num(a) >> sc_num(b)); }

// -- strings, as the .c4r writer uses them -----------------------------
int *sc_str_alloc (int *n) { return builtin_call(B_STR_ALLOC, cons(n, 0), 0); }
int *sc_str_byte (int *s, int *i) { return builtin_call(B_STR_BYTE, cons(s, cons(i, 0)), 0); }
int *sc_str_setbyte (int *s, int *i, int *v) {
	return builtin_call(B_STR_SETBYTE, cons(s, cons(i, cons(v, 0))), 0);
}
int *sc_str_word (int *s, int *i) { return builtin_call(B_STR_WORD, cons(s, cons(i, 0)), 0); }
int *sc_str_setword (int *s, int *i, int *v) {
	return builtin_call(B_STR_SETWORD, cons(s, cons(i, cons(v, 0))), 0);
}

// print is variadic, and cold -- twice in all of c4opt -- so it is the
// one builtin that still gets an argument list built for it.
int *sc_print (int *args) { return builtin_call(B_PRINT, args, 0); }

// -- odds and ends -----------------------------------------------------
int *sc_typeof (int *x) { return builtin_call(B_TYPEOF, cons(x, 0), 0); }

// Literals. The generated file builds each string and each quoted datum
// once, in sc_init(), so a hot loop is not re-reading its own constants.
int *sc_str (char *s, int n) { return mk_string_len(s, n); }
int *sc_read (char *s, int n) { return rd_read(s, n); }
