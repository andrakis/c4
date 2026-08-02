// c4sp: cell constructors, list helpers, environments
//
// Everything here allocates from the arena in gc.h. Strings own a malloc'd
// buffer outside the arena; the sweep frees it when the cell dies (M2).

// -- small utilities (plain c4 has no memcpy opcode, so copies are manual) --

int cs_strlen (char *s) {
	int n;
	n = 0;
	while (s[n]) ++n;
	return n;
}

// Copy len bytes of s into a fresh nul-terminated malloc'd buffer.
char *cs_strdup_len (char *s, int len) {
	char *d;
	int i;
	if (!(d = malloc(len + 1))) {
		c4sp_error("out of memory copying string");
		return 0;
	}
	i = 0;
	while (i < len) { d[i] = s[i]; ++i; }
	d[len] = 0;
	return d;
}

// -- constructors --

int *cell_new (int type) {
	int *c;
	c = gc_alloc_cell();
	c[CELL_TYPE] = type;
	return c;
}

int *mk_int (int v) {
	int *c;
	c = cell_new(T_INT);
	c[CELL_A] = v;
	return c;
}

int *mk_float (int bits) {
	int *c;
	c = cell_new(T_FLOAT);
	c[CELL_A] = bits;
	return c;
}

int *mk_atom (int id) {
	int *c;
	c = cell_new(T_ATOM);
	c[CELL_A] = id;
	return c;
}

// Takes ownership of a malloc'd nul-terminated buffer.
int *mk_string_own (char *s, int len) {
	int *c;
	c = cell_new(T_STRING);
	c[CELL_A] = (int)s;
	c[CELL_B] = len;
	return c;
}

// Copies len bytes of s.
int *mk_string_len (char *s, int len) {
	char *d;
	if (!(d = cs_strdup_len(s, len))) return 0;
	return mk_string_own(d, len);
}

int *mk_string (char *s) { return mk_string_len(s, cs_strlen(s)); }

int *cons (int *a, int *d) {
	int *c;
	c = cell_new(T_CONS);
	c[CELL_A] = (int)a;
	c[CELL_B] = (int)d;
	return c;
}

// type is T_LAMBDA, T_MACRO or T_FASTMACRO
int *mk_closure (int type, int *args, int *body, int *env) {
	int *c;
	c = cell_new(type);
	c[CELL_A] = (int)args;
	c[CELL_B] = (int)body;
	c[CELL_C] = (int)env;
	return c;
}

// type is T_PROC or T_PROCENV; id indexes the builtin dispatch in stdlib.h
int *mk_proc (int type, int id) {
	int *c;
	c = cell_new(type);
	c[CELL_A] = id;
	return c;
}

// -- accessors and predicates --

int cell_type (int *x) { return x ? x[CELL_TYPE] : T_NIL; }

int *car (int *x) { return (cell_type(x) == T_CONS) ? (int *)x[CELL_A] : 0; }
int *cdr (int *x) { return (cell_type(x) == T_CONS) ? (int *)x[CELL_B] : 0; }

// A shallow copy sharing children -- alisp's clone(). Environments are
// returned as-is: alisp's clone copies only the wrapper around a shared
// binding map, and a cell is both wrapper and value, so the identical cell
// is the faithful translation.
int *cell_clone (int *x) {
	int *c;
	if (!x) return 0;
	if (x[CELL_TYPE] == T_ENV) return x;
	if (x[CELL_TYPE] == T_STRING)
		return mk_string_len((char *)x[CELL_A], x[CELL_B]);
	c = cell_new(x[CELL_TYPE]);
	c[CELL_A] = x[CELL_A];
	c[CELL_B] = x[CELL_B];
	c[CELL_C] = x[CELL_C];
	return c;
}

int list_length (int *x) {
	int n;
	n = 0;
	while (cell_type(x) == T_CONS) { ++n; x = (int *)x[CELL_B]; }
	return n;
}

// nth element (0-based), nil when out of range -- alisp's index()
int *list_index (int *x, int n) {
	while (n-- > 0 && cell_type(x) == T_CONS) x = (int *)x[CELL_B];
	return car(x);
}

// alisp treats strings as list-like for empty?/length; match that.
int cell_size (int *x) {
	int t;
	t = cell_type(x);
	if (t == T_CONS) return list_length(x);
	if (t == T_STRING) return x[CELL_B];
	if (t == T_LAMBDA || t == T_MACRO || t == T_FASTMACRO) return 3;
	return 0;
}

// Append list b after a copy of list a. Shares b's cells.
int *list_append (int *a, int *b) {
	int *head, *tail, *n;
	if (!a) return b;
	head = tail = cons(car(a), 0);
	a = cdr(a);
	while (cell_type(a) == T_CONS) {
		n = cons(car(a), 0);
		tail[CELL_B] = (int)n;
		tail = n;
		a = (int *)a[CELL_B];
	}
	tail[CELL_B] = (int)b;
	return head;
}

// Deep structural equality -- alisp's equal(). Numbers of differing
// numeric types compare by printed form there; here int==int and
// float==float bitwise, which the corpus never distinguishes.
int cell_equal (int *a, int *b) {
	int ta, tb;
	if (a == b) return 1;
	ta = cell_type(a); tb = cell_type(b);
	if (ta != tb) return 0;
	if (ta == T_INT || ta == T_FLOAT || ta == T_ATOM)
		return a[CELL_A] == b[CELL_A];
	if (ta == T_STRING)
		return a[CELL_B] == b[CELL_B] &&
		       !memcmp((char *)a[CELL_A], (char *)b[CELL_A], a[CELL_B]);
	if (ta == T_CONS)
		return cell_equal((int *)a[CELL_A], (int *)b[CELL_A]) &&
		       cell_equal((int *)a[CELL_B], (int *)b[CELL_B]);
	if (ta == T_LAMBDA || ta == T_MACRO || ta == T_FASTMACRO)
		return cell_equal((int *)a[CELL_A], (int *)b[CELL_A]) &&
		       cell_equal((int *)a[CELL_B], (int *)b[CELL_B]) &&
		       a[CELL_C] == b[CELL_C];
	// T_PROC/T_PROCENV/T_ENV: identity (a == b failed above)
	return a[CELL_A] == b[CELL_A] && ta != T_ENV;
}

// -- environments --
//
// A frame is a T_ENV cell: A = bindings, B = parent, C = numeric id.
// Bindings are an assoc list: cons((atom . value) pairs), each pair itself a
// cons whose car is a T_ATOM cell and cdr the value. Local frames hold a
// handful of bindings, where the linear scan wins over any hash.

int env_counter;

int *mk_env (int *parent) {
	int *c;
	c = cell_new(T_ENV);
	c[CELL_B] = (int)parent;
	c[CELL_C] = ++env_counter;
	return c;
}

// Find the (atom . value) pair for id in this frame only.
int *env_local_pair (int *env, int id) {
	int *b, *pair;
	b = (int *)env[CELL_A];
	while (b) {
		pair = (int *)b[CELL_A];
		if (((int *)pair[CELL_A])[CELL_A] == id) return pair;
		b = (int *)b[CELL_B];
	}
	return 0;
}

// Find the pair for id in this frame or any parent.
int *env_find_pair (int *env, int id) {
	int *pair;
	while (env) {
		if ((pair = env_local_pair(env, id))) return pair;
		env = (int *)env[CELL_B];
	}
	return 0;
}

// define creates (or overwrites) locally
int *env_define (int *env, int id, int *value) {
	int *pair;
	if ((pair = env_local_pair(env, id))) {
		pair[CELL_B] = (int)value;
		return value;
	}
	pair = cons(mk_atom(id), value);
	env[CELL_A] = (int)cons(pair, (int *)env[CELL_A]);
	return value;
}

// set! walks up; error when the name does not exist anywhere
int *env_set (int *env, int id, int *value) {
	int *pair;
	if ((pair = env_find_pair(env, id))) {
		pair[CELL_B] = (int)value;
		return value;
	}
	c4sp_error("set! of undefined variable");
	return 0;
}

int env_defined (int *env, int id) { return env_find_pair(env, id) != 0; }

// get walks up; error when not found
int *env_get (int *env, int id) {
	int *pair;
	if ((pair = env_find_pair(env, id))) return (int *)pair[CELL_B];
	c4sp_error("undefined variable");
	return 0;
}

// Bind formal names to values in env: the Environment(keys, values, parent)
// rule. keys is normally a list of atoms matched pairwise with values
// (unmatched names are simply not bound); a bare atom instead captures the
// entire value list -- that is how variadic macros receive their arguments.
void env_bind (int *env, int *keys, int *values) {
	if (cell_type(keys) == T_ATOM) {
		env_define(env, keys[CELL_A], values);
		return;
	}
	while (cell_type(keys) == T_CONS && cell_type(values) == T_CONS) {
		env_define(env, ((int *)keys[CELL_A])[CELL_A], (int *)values[CELL_A]);
		keys = (int *)keys[CELL_B];
		values = (int *)values[CELL_B];
	}
}
