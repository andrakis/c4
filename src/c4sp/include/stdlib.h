// c4sp: builtins
//
// A T_PROC or T_PROCENV cell holds an integer id dispatched through
// builtin_call() below. The distinction between the two types matters:
// seval.lisp switches on (typeof p) being 'proc or 'proc_env and invokes
// them through cell:proc / cell:proc_env, so each builtin must carry the
// same type it has in alisp's stdlib.
//
// The dispatch is one if-chain, which keeps c4sp runnable under unmodified
// c4 (no JSRI). Slow but portable; see design 7 for the c4m alternative.

enum {
	B_PRINT, B_ADD, B_SUB, B_MUL, B_DIV, B_EQ, B_NE,
	B_LT, B_LE, B_GT, B_GE, B_NOT,
	B_LENGTH, B_LIST, B_INDEX, B_HEAD, B_TAIL, B_EMPTY, B_TYPEOF, B_ERROR,
	B_CELL_LAMBDA, B_CELL_MACRO, B_CELL_FASTMACRO,
	B_CELL_LAMBDA_ARGS, B_CELL_LAMBDA_BODY, B_CELL_LAMBDA_ENV,
	B_CELL_PROC, B_CELL_PROC_ENV,
	B_ENV_CAPTURE, B_ENV_RECAPTURE, B_ENV_CURRENT, B_ENV_DEFINED,
	B_ENV_DEFINE, B_ENV_SET, B_ENV_NEW, B_ENV_GET, B_ENV_GETOR,
	B_GC, B_GC_STATS,
	B_STR_SPLIT, B_STR_JOIN, B_STR_SUBSTR, B_STR_REPEAT,
	B_FILE_EXISTS, B_FILE_PATH, B_FILE_READ, B_DEBUG_PARSE,
	B__COUNT
};

// env_get/env_set with the variable's name in the error message. Defined
// here rather than cells.h because atom_name lives above this file in the
// include chain.
int *env_get_named (int *env, int id) {
	int *pair;
	if ((pair = env_find_pair(env, id))) return (int *)pair[CELL_B];
	c4sp_error_name("undefined variable", atom_name(id));
	return 0;
}

int *env_set_named (int *env, int id, int *value) {
	int *pair;
	if ((pair = env_find_pair(env, id))) {
		pair[CELL_B] = (int)value;
		return value;
	}
	c4sp_error_name("set! of undefined variable", atom_name(id));
	return 0;
}

// Singletons, created by stdlib_init
int *cell_true, *cell_false;

int *bool_cell (int b) { return b ? cell_true : cell_false; }

// Is x the atom false? The only falsy value in alisp: nil and 0 are true.
int cell_is_false (int *x) {
	return cell_type(x) == T_ATOM && x[CELL_A] == A_FALSE;
}

// Resolve a symbol argument (atom or string) to an atom id.
int arg_atom_id (int *x) {
	int t;
	t = cell_type(x);
	if (t == T_ATOM) return x[CELL_A];
	if (t == T_STRING) return atom_intern((char *)x[CELL_A], x[CELL_B]);
	c4sp_error("expected a symbol");
	return 0;
}

// The typeof atom for a cell. nil reports 'atom, mirroring alisp where Nil
// is an atom; the empty list is indistinguishable from it here.
int *cell_typeof (int *x) {
	int t;
	char *n;
	t = cell_type(x);
	if (t == T_NIL) n = "atom";
	else if (t == T_ATOM) n = "atom";
	else if (t == T_INT) n = "number";
	else if (t == T_FLOAT) n = "number";
	else if (t == T_STRING) n = "string";
	else if (t == T_CONS) n = "list";
	else if (t == T_LAMBDA) n = "lambda";
	else if (t == T_MACRO) n = "macro";
	else if (t == T_FASTMACRO) n = "fastmacro";
	else if (t == T_PROC) n = "proc";
	else if (t == T_PROCENV) n = "proc_env";
	else if (t == T_ENV) n = "env";
	else n = "none";
	return mk_atom(atom_intern(n, cs_strlen(n)));
}

// (+ ...): behaviour follows the first argument -- numeric sum, string
// concatenation (later arguments printed in display form), or list append.
// An empty list is nil, so nil-first also appends.
int *builtin_add (int *args) {
	int *first, *rest, *r, v, t;
	first = car(args);
	rest = cdr(args);
	t = cell_type(first);
	if (t == T_INT) {
		v = first[CELL_A];
		while (rest) {
			if (cell_type(car(rest)) != T_INT) { c4sp_error("+ on mixed types"); return 0; }
			v = v + car(rest)[CELL_A];
			rest = cdr(rest);
		}
		return mk_int(v);
	}
	if (t == T_STRING) {
		pr_reset();
		cell_write(first, 0);
		while (rest) { cell_write(car(rest), 0); rest = cdr(rest); }
		return mk_string_len(pr_buf, pr_len);
	}
	if (t == T_CONS || t == T_NIL) {
		r = first;
		while (rest) {
			v = cell_type(car(rest));
			if (v == T_CONS || v == T_NIL)
				r = list_append(r, car(rest));
			else
				r = list_append(r, cons(car(rest), 0));
			rest = cdr(rest);
		}
		return r;
	}
	c4sp_error("cannot add to this type");
	return 0;
}

// -, *, /: numeric folds
int *builtin_arith (int op, int *args) {
	int *rest, v, w;
	if (cell_type(car(args)) != T_INT) { c4sp_error("arithmetic on non-number"); return 0; }
	v = car(args)[CELL_A];
	rest = cdr(args);
	while (rest) {
		if (cell_type(car(rest)) != T_INT) { c4sp_error("arithmetic on non-number"); return 0; }
		w = car(rest)[CELL_A];
		if (op == B_SUB) v = v - w;
		else if (op == B_MUL) v = v * w;
		else if (op == B_DIV) {
			if (!w) { c4sp_error("division by zero"); return 0; }
			v = v / w;
		}
		rest = cdr(rest);
	}
	return mk_int(v);
}

// <, <=, >, >=: compare the value words, as alisp compares .value
int *builtin_cmp (int op, int *args) {
	int a, b;
	a = car(args) ? car(args)[CELL_A] : 0;
	b = list_index(args, 1) ? list_index(args, 1)[CELL_A] : 0;
	if (op == B_LT) return bool_cell(a < b);
	if (op == B_LE) return bool_cell(a <= b);
	if (op == B_GT) return bool_cell(a > b);
	return bool_cell(a >= b);
}

// string:substr with alisp (JS substr) semantics: (string:substr s start
// [length]); a negative start counts from the end.
int *builtin_substr (int *args) {
	int *s, start, count, len;
	s = car(args);
	if (cell_type(s) != T_STRING) { c4sp_error("string:substr needs a string"); return 0; }
	len = s[CELL_B];
	start = list_index(args, 1) ? list_index(args, 1)[CELL_A] : 0;
	if (start < 0) { start = len + start; if (start < 0) start = 0; }
	if (start > len) start = len;
	count = len - start;
	if (list_length(args) >= 3 && cell_type(list_index(args, 2)) == T_INT) {
		count = list_index(args, 2)[CELL_A];
		if (count < 0) count = 0;
		if (count > len - start) count = len - start;
	}
	return mk_string_len((char *)s[CELL_A] + start, count);
}

// (string:split sep str) -> list of strings
int *builtin_split (int *args) {
	int *sep, *s, *head, *tail, *piece, *n;
	char *sc, *str;
	int seplen, len, i, start;
	sep = car(args);
	s = list_index(args, 1);
	if (cell_type(sep) != T_STRING || cell_type(s) != T_STRING) {
		c4sp_error("string:split needs two strings");
		return 0;
	}
	sc = (char *)sep[CELL_A]; seplen = sep[CELL_B];
	str = (char *)s[CELL_A]; len = s[CELL_B];
	if (!seplen) { // JS "".split: every character separately
		head = tail = 0;
		i = 0;
		while (i < len) {
			n = cons(mk_string_len(str + i, 1), 0);
			if (tail) { tail[CELL_B] = (int)n; tail = n; } else head = tail = n;
			++i;
		}
		return head;
	}
	head = tail = 0;
	start = 0;
	i = 0;
	while (i <= len - seplen) {
		if (!memcmp(str + i, sc, seplen)) {
			piece = cons(mk_string_len(str + start, i - start), 0);
			if (tail) { tail[CELL_B] = (int)piece; tail = piece; } else head = tail = piece;
			i = i + seplen;
			start = i;
		} else ++i;
	}
	piece = cons(mk_string_len(str + start, len - start), 0);
	if (tail) tail[CELL_B] = (int)piece;
	else head = piece;
	return head;
}

// (string:join sep list) -> string
int *builtin_join (int *args) {
	int *sep, *l, first;
	sep = car(args);
	l = list_index(args, 1);
	pr_reset();
	first = 1;
	while (cell_type(l) == T_CONS) {
		if (!first) cell_write(sep, 0);
		first = 0;
		cell_write((int *)l[CELL_A], 0);
		l = (int *)l[CELL_B];
	}
	return mk_string_len(pr_buf, pr_len);
}

// (string:repeat s n) -> string
int *builtin_repeat (int *args) {
	int *s, n;
	s = car(args);
	if (cell_type(list_index(args, 1)) != T_INT) return s;
	n = list_index(args, 1)[CELL_A];
	pr_reset();
	while (n-- > 0) cell_write(s, 0);
	return mk_string_len(pr_buf, pr_len);
}

// file:path search order, mirroring alisp: as given, ./, ./lisp/, and this
// repo's sample directory.
int file_exists (char *path) {
	int fd;
	if ((fd = open(path, C4SP_OPEN_MODE)) < 0) return 0;
	close(fd);
	return 1;
}

char *file_path_buf;

// Returns a nul-terminated path (static buffer), or the input name.
char *file_find (char *name, int len) {
	char *pre, *d, *s;
	int i, n;
	if (!file_path_buf) file_path_buf = malloc(1024);
	i = 0;
	while (i < 4) {
		if (i == 0) pre = "";
		else if (i == 1) pre = "./";
		else if (i == 2) pre = "./lisp/";
		else pre = "src/c4sp/lisp/";
		d = file_path_buf;
		s = pre;
		while (*s) *d++ = *s++;
		n = 0;
		while (n < len && d - file_path_buf < 1000) *d++ = name[n++];
		*d = 0;
		if (file_exists(file_path_buf)) return file_path_buf;
		++i;
	}
	// Not found: hand back the name itself
	d = file_path_buf;
	n = 0;
	while (n < len && n < 1000) { d[n] = name[n]; ++n; }
	d[n] = 0;
	return file_path_buf;
}

// The builtin dispatcher. args is a cons list of evaluated arguments
// (unevaluated for macros, but macros never reach here); env is the
// caller's environment, used by the PROC_ENV builtins.
int *builtin_call (int id, int *args, int *env) {
	int *a0, *a1, *a2, *x, *e;
	int  n, len;
	char *s;

	a0 = car(args);
	a1 = list_index(args, 1);
	a2 = list_index(args, 2);

	if (id == B_PRINT) {
		pr_reset();
		x = args;
		while (cell_type(x) == T_CONS) {
			if (x != args) pr_ch(' ');
			cell_write((int *)x[CELL_A], 0);
			x = (int *)x[CELL_B];
		}
		printf("%s\n", pr_term());
		return 0;
	}
	if (id == B_ADD) return builtin_add(args);
	if (id == B_SUB || id == B_MUL || id == B_DIV) return builtin_arith(id, args);
	if (id == B_EQ) return bool_cell(cell_equal(a0, a1));
	if (id == B_NE) return bool_cell(!cell_equal(a0, a1));
	if (id == B_LT || id == B_LE || id == B_GT || id == B_GE)
		return builtin_cmp(id, args);
	if (id == B_NOT) return bool_cell(!cell_equal(a0, cell_true));
	if (id == B_LENGTH) return mk_int(cell_size(a0));
	if (id == B_LIST) return args;
	if (id == B_INDEX) {
		if (cell_type(a1) != T_INT) return 0;
		return cell_clone(list_index(a0, a1[CELL_A]));
	}
	if (id == B_HEAD) return car(a0);
	if (id == B_TAIL) return cdr(a0);
	if (id == B_EMPTY) {
		n = cell_type(a0);
		if (n == T_NIL) return cell_true;
		if (n == T_STRING) return bool_cell(a0[CELL_B] == 0);
		return cell_false;
	}
	if (id == B_TYPEOF) return cell_typeof(a0);
	if (id == B_ERROR) {
		pr_reset();
		cell_write(a0, 0);
		c4sp_error(cs_strdup_len(pr_buf, pr_len));
		return 0;
	}

	// cell:*
	if (id == B_CELL_LAMBDA || id == B_CELL_MACRO || id == B_CELL_FASTMACRO) {
		e = a2 ? a2 : env;
		if (id == B_CELL_LAMBDA) return mk_closure(T_LAMBDA, a0, a1, e);
		if (id == B_CELL_MACRO) return mk_closure(T_MACRO, a0, a1, e);
		return mk_closure(T_FASTMACRO, a0, a1, mk_env(e));
	}
	if (id == B_CELL_LAMBDA_ARGS) return a0 ? (int *)a0[CELL_A] : 0;
	if (id == B_CELL_LAMBDA_BODY) return a0 ? (int *)a0[CELL_B] : 0;
	if (id == B_CELL_LAMBDA_ENV) return a0 ? (int *)a0[CELL_C] : 0;
	if (id == B_CELL_PROC) {
		if (cell_type(a0) != T_PROC) { c4sp_error("cell:proc needs a proc"); return 0; }
		return builtin_call(a0[CELL_A], a1, env);
	}
	if (id == B_CELL_PROC_ENV) {
		if (cell_type(a0) != T_PROCENV) { c4sp_error("cell:proc_env needs a proc_env"); return 0; }
		return builtin_call(a0[CELL_A], a1, a2 ? a2 : env);
	}

	// env:*
	if (id == B_ENV_CAPTURE) {
		e = mk_env(a2 ? a2 : env);
		env_bind(e, a0, a1);
		return e;
	}
	if (id == B_ENV_RECAPTURE) {
		env_bind(a0, a1, a2);
		return a0;
	}
	if (id == B_ENV_CURRENT) return env;
	if (id == B_ENV_DEFINED) {
		e = a1 ? a1 : env;
		return bool_cell(env_defined(e, arg_atom_id(a0)));
	}
	if (id == B_ENV_DEFINE) {
		e = a2 ? a2 : env;
		env_define(e, arg_atom_id(a0), cell_clone(a1));
		return cell_clone(a1);
	}
	if (id == B_ENV_SET) {
		e = a2 ? a2 : env;
		env_set(e, arg_atom_id(a0), cell_clone(a1));
		return cell_clone(a1);
	}
	if (id == B_ENV_NEW) {
		if (!args) return mk_env(env);
		if (!a0) return mk_env(0);
		return mk_env(a0);
	}
	if (id == B_ENV_GET) {
		e = a1 ? a1 : env;
		return env_get(e, arg_atom_id(a0));
	}
	if (id == B_ENV_GETOR) {
		e = a2 ? a2 : env;
		if (!env_defined(e, arg_atom_id(a0))) return a1;
		return env_get(e, arg_atom_id(a0));
	}

	// gc
	if (id == B_GC) { gc_collect(); return 0; }
	if (id == B_GC_STATS) {
		printf("c4sp gc: %d cells, %d free, %d allocs, %d collections, last freed %d live %d\n",
		       gc_ncells, gc_free_count, gc_total_allocs, gc_collections,
		       gc_last_freed, gc_last_live);
		return 0;
	}

	// string:*
	if (id == B_STR_SPLIT) return builtin_split(args);
	if (id == B_STR_JOIN) return builtin_join(args);
	if (id == B_STR_SUBSTR) return builtin_substr(args);
	if (id == B_STR_REPEAT) return builtin_repeat(args);

	// file:*, debug:parse
	if (id == B_FILE_EXISTS) {
		pr_reset(); cell_write(a0, 0);
		return bool_cell(file_exists(file_find(pr_term(), pr_len)) != 0);
	}
	if (id == B_FILE_PATH) {
		pr_reset(); cell_write(a0, 0);
		s = file_find(pr_term(), pr_len);
		return mk_string(s);
	}
	if (id == B_FILE_READ) {
		pr_reset(); cell_write(a0, 0);
		s = file_find(pr_term(), pr_len);
		if (!(s = rd_file(s, &len))) { c4sp_error("file:read: cannot open file"); return 0; }
		x = mk_string_len(s, len);
		free(s);
		return x;
	}
	if (id == B_DEBUG_PARSE) {
		if (cell_type(a0) != T_STRING) { c4sp_error("debug:parse needs a string"); return 0; }
		return rd_read((char *)a0[CELL_A], a0[CELL_B]);
	}

	c4sp_error("unknown builtin");
	return 0;
}

// Bind one builtin into env. type is T_PROC or T_PROCENV, matching alisp.
void stdlib_bind (int *env, char *name, int type, int id) {
	env_define(env, atom_intern(name, cs_strlen(name)), mk_proc(type, id));
}

// Populate the global environment.
void stdlib_init (int *env) {
	cell_true  = gc_root_true  = mk_atom(A_TRUE);
	cell_false = gc_root_false = mk_atom(A_FALSE);
	env_define(env, A_NIL, 0);
	env_define(env, A_TRUE, cell_true);
	env_define(env, A_FALSE, cell_false);
	stdlib_bind(env, "print", T_PROC, B_PRINT);
	stdlib_bind(env, "+", T_PROC, B_ADD);
	stdlib_bind(env, "-", T_PROC, B_SUB);
	stdlib_bind(env, "*", T_PROC, B_MUL);
	stdlib_bind(env, "/", T_PROC, B_DIV);
	stdlib_bind(env, "=", T_PROC, B_EQ);
	stdlib_bind(env, "!=", T_PROC, B_NE);
	stdlib_bind(env, "<", T_PROC, B_LT);
	stdlib_bind(env, "<=", T_PROC, B_LE);
	stdlib_bind(env, ">", T_PROC, B_GT);
	stdlib_bind(env, ">=", T_PROC, B_GE);
	stdlib_bind(env, "not", T_PROC, B_NOT);
	stdlib_bind(env, "length", T_PROC, B_LENGTH);
	stdlib_bind(env, "list", T_PROC, B_LIST);
	stdlib_bind(env, "index", T_PROC, B_INDEX);
	stdlib_bind(env, "head", T_PROC, B_HEAD);
	stdlib_bind(env, "tail", T_PROC, B_TAIL);
	stdlib_bind(env, "empty?", T_PROC, B_EMPTY);
	stdlib_bind(env, "typeof", T_PROC, B_TYPEOF);
	stdlib_bind(env, "error", T_PROCENV, B_ERROR);
	stdlib_bind(env, "cell:lambda", T_PROCENV, B_CELL_LAMBDA);
	stdlib_bind(env, "cell:macro", T_PROCENV, B_CELL_MACRO);
	stdlib_bind(env, "cell:fastmacro", T_PROCENV, B_CELL_FASTMACRO);
	stdlib_bind(env, "cell:lambda_args", T_PROC, B_CELL_LAMBDA_ARGS);
	stdlib_bind(env, "cell:lambda_body", T_PROC, B_CELL_LAMBDA_BODY);
	stdlib_bind(env, "cell:lambda_env", T_PROC, B_CELL_LAMBDA_ENV);
	stdlib_bind(env, "cell:proc", T_PROC, B_CELL_PROC);
	stdlib_bind(env, "cell:proc_env", T_PROCENV, B_CELL_PROC_ENV);
	stdlib_bind(env, "env:capture", T_PROCENV, B_ENV_CAPTURE);
	stdlib_bind(env, "env:recapture", T_PROC, B_ENV_RECAPTURE);
	stdlib_bind(env, "env:current", T_PROCENV, B_ENV_CURRENT);
	stdlib_bind(env, "env:defined", T_PROCENV, B_ENV_DEFINED);
	stdlib_bind(env, "env:define", T_PROCENV, B_ENV_DEFINE);
	stdlib_bind(env, "env:set!", T_PROCENV, B_ENV_SET);
	stdlib_bind(env, "env:new", T_PROCENV, B_ENV_NEW);
	stdlib_bind(env, "env:get", T_PROCENV, B_ENV_GET);
	stdlib_bind(env, "env:getor", T_PROCENV, B_ENV_GETOR);
	stdlib_bind(env, "gc", T_PROC, B_GC);
	stdlib_bind(env, "gc:stats", T_PROC, B_GC_STATS);
	stdlib_bind(env, "string:split", T_PROC, B_STR_SPLIT);
	stdlib_bind(env, "string:join", T_PROC, B_STR_JOIN);
	stdlib_bind(env, "string:substr", T_PROC, B_STR_SUBSTR);
	stdlib_bind(env, "string:repeat", T_PROC, B_STR_REPEAT);
	stdlib_bind(env, "file:exists", T_PROC, B_FILE_EXISTS);
	stdlib_bind(env, "file:path", T_PROC, B_FILE_PATH);
	stdlib_bind(env, "file:read", T_PROC, B_FILE_READ);
	stdlib_bind(env, "debug:parse", T_PROC, B_DEBUG_PARSE);
}
