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

// -- strings ----------------------------------------------------------
//
// These are written out rather than delegated to builtin_call, and the
// difference is the whole point of the exercise. A shim that calls
// builtin_call has to CONS AN ARGUMENT LIST first and then walk the
// thirty-way if-chain -- which is the call protocol compiling was
// supposed to remove, reintroduced one layer down. It does not show up
// on c4opt, which barely touches strings; it shows up immediately on
// the lexer, where string:byte is the hottest thing there is.
//
// Semantics copied from stdlib.h, error for error: an out-of-range read
// answers -1 rather than failing, an out-of-range write is an error.
int *sc_str_byte (int *s, int *i) {
	int n;

	if (cell_type(s) != T_STRING || cell_type(i) != T_INT) {
		c4sp_error("string:byte needs a string and an index"); return 0;
	}
	n = i[CELL_A];
	if (n < 0 || n >= s[CELL_B]) return mk_int(-1);
	return mk_int(((char *)s[CELL_A])[n] & 255);
}

int *sc_str_setbyte (int *s, int *i, int *v) {
	int n;

	if (cell_type(s) != T_STRING || cell_type(i) != T_INT || cell_type(v) != T_INT) {
		c4sp_error("string:byte! needs a string, index, value"); return 0;
	}
	n = i[CELL_A];
	if (n < 0 || n >= s[CELL_B]) { c4sp_error("string:byte! out of range"); return 0; }
	((char *)s[CELL_A])[n] = v[CELL_A];
	return s;
}

int *sc_str_substr (int *s, int *a, int *b) {
	int start, count, len;

	if (cell_type(s) != T_STRING) { c4sp_error("string:substr needs a string"); return 0; }
	len = s[CELL_B];
	start = (cell_type(a) == T_INT) ? a[CELL_A] : 0;
	if (start < 0) { start = len + start; if (start < 0) start = 0; }
	if (start > len) start = len;
	count = len - start;
	if (cell_type(b) == T_INT) {
		count = b[CELL_A];
		if (count < 0) count = 0;
		if (count > len - start) count = len - start;
	}
	return mk_string_len((char *)s[CELL_A] + start, count);
}

// The .c4r writer's word access; cold next to string:byte, so these stay
// delegated rather than duplicating the host-byte-order care in stdlib.h.
int *sc_str_alloc (int *n) { return builtin_call(B_STR_ALLOC, cons(n, 0), 0); }
int *sc_str_word (int *s, int *i) { return builtin_call(B_STR_WORD, cons(s, cons(i, 0)), 0); }
int *sc_str_setword (int *s, int *i, int *v) {
	return builtin_call(B_STR_SETWORD, cons(s, cons(i, cons(v, 0))), 0);
}
int *sc_file_read (int *p) { return builtin_call(B_FILE_READ, cons(p, 0), 0); }
int *sc_file_exists (int *p) { return builtin_call(B_FILE_EXISTS, cons(p, 0), 0); }
int *sc_wordsize () { return mk_int(sizeof(int)); }

// error takes only its first argument, whatever the arity at the call
// site, so the list is built for the same reason print's is: it is cold
// and it stops the shim inventing a different truncation rule.
int *sc_error (int *args) { return builtin_call(B_ERROR, args, 0); }
int *sc_file_path (int *p) { return builtin_call(B_FILE_PATH, cons(p, 0), 0); }

// print is variadic, and cold -- twice in all of c4opt -- so it is the
// one builtin that still gets an argument list built for it.
int *sc_print (int *args) { return builtin_call(B_PRINT, args, 0); }

// -- odds and ends -----------------------------------------------------
int *sc_typeof (int *x) { return builtin_call(B_TYPEOF, cons(x, 0), 0); }

// Literals. The generated file builds each string and each quoted datum
// once, in sc_init(), so a hot loop is not re-reading its own constants.
int *sc_str (char *s, int n) { return mk_string_len(s, n); }
int *sc_atom (char *s, int n) { return mk_atom(atom_intern(s, n)); }

// Kept for nothing now that quoted data is built structurally, and left
// here because a unit that wants the reader should not have to invent it.
int *sc_read (char *s, int n) { return rd_read(s, n); }
