// c4sp: cell layout
//
// C4 has no structs, so a cell is CELL__Sz consecutive words addressed
// through an enum -- the same idiom as C4KE's task struct. Four words rather
// than three wastes one word per cons, but buys a single uniform cell size,
// which is what keeps the allocator and collector simple.
//
// THE ONE DESIGN RULE (docs/c4sp-design.md 4.7):
//   Every structure that can hold a cell reference must itself be a cell in
//   the arena, or be registered as an explicit root. The atom table stores
//   only plain strings (no cell references), so it is exempt.
//
// nil is the null pointer, 0. The empty list and the nil atom are therefore
// the same value, unlike alisp where Nil is an atom and () a distinct empty
// vector. Nothing in the sample corpus can tell the difference.

enum { CELL_TYPE, CELL_A, CELL_B, CELL_C, CELL__Sz };

// Cell types. Slot use per type:
//   T_INT       A = value
//   T_FLOAT     A = binary32 bit pattern
//   T_ATOM      A = atom id
//   T_STRING    A = char* (malloc'd, nul terminated), B = length
//   T_CONS      A = car cell, B = cdr cell
//   T_LAMBDA,
//   T_MACRO,
//   T_FASTMACRO A = args, B = body, C = env cell
//   T_PROC,
//   T_PROCENV   A = builtin id
//   T_ENV       A = bindings (list of (atom value) pair conses),
//               B = parent env cell, C = env id (plain int, for printing)
//   -- CEK continuation frames (design 6.2); the chain itself is built
//      from ordinary conses, so a frame cell never links to the next --
//   T_KARG,
//   T_KARGN     A = values so far (reversed), B = remaining exprs
//               (car = the one being evaluated), C = caller env.
//               T_KARGN is the (next ...) flavour.
//   T_KIF       A = (conseq [alt]) list, B = env
//   T_KDEF,
//   T_KSET      A = symbol atom, B = env
//   T_KBEGIN    A = remaining body list, B = env
//   T_KMACRO    A = caller env to re-evaluate the expansion in
//   T_CONT      A = a captured kont chain (call/cc). Frames are immutable,
//               so capturing is copying this one pointer, and a saved
//               continuation stays valid however often it is invoked.
enum {
	T_NIL, T_ATOM, T_INT, T_FLOAT, T_STRING, T_CONS,
	T_LAMBDA, T_MACRO, T_FASTMACRO, T_PROC, T_PROCENV, T_ENV, T_CONT,
	T_KARG, T_KARGN, T_KIF, T_KDEF, T_KSET, T_KBEGIN, T_KMACRO,
	T__COUNT
};

// Global error flag and message, checked by the reader, evaluator and main.
// C4 has no exceptions or setjmp; this is the explicit alternative.
int   c4sp_err;
char *c4sp_err_msg;

void c4sp_error (char *msg) {
	// First error wins; later ones are usually cascades.
	if (!c4sp_err) {
		c4sp_err = 1;
		c4sp_err_msg = msg;
	}
}

// As c4sp_error, but appends a name: "undefined variable: foo"
void c4sp_error_name (char *msg, char *name) {
	char *buf, *d;
	int n;
	if (c4sp_err) return;
	n = 0;
	while (msg[n]) ++n;
	d = name;
	while (*d++) ++n;
	if (!(buf = malloc(n + 3))) { c4sp_error(msg); return; }
	d = buf;
	while (*msg) *d++ = *msg++;
	*d++ = ':';
	*d++ = ' ';
	while (*name) *d++ = *name++;
	*d = 0;
	c4sp_err = 1;
	c4sp_err_msg = buf;
}
