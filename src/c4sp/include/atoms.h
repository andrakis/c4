// c4sp: interned atoms
//
// A global id <-> name table so atom comparison is an integer compare.
// The table stores plain malloc'd strings and no cell references, so the
// design rule in cell.h does not require it to be scanned by the collector.
//
// Growth is copy-into-bigger-arrays: realloc is broken under c4m
// (docs/internals.md 3.6), and plain malloc/free is all this needs.

char **atom_names;  // id -> nul-terminated name
int   *atom_lens;   // id -> name length
int    atom_count;
int    atom_cap;

// Atoms interned by atoms_init, in this order, so the evaluator can compare
// against compile-time constants.
enum {
	A_NIL, A_TRUE, A_FALSE,
	A_QUOTE, A_IF, A_DEFINE, A_SET, A_LAMBDA, A_MACRO, A_FASTMACRO,
	A_BEGIN, A_NEXT, A_LOAD
};

// Intern a name (len bytes of s), returning its id.
int atom_intern (char *s, int len) {
	int i;
	char **nnames;
	int  *nlens;
	// Linear scan; the table stays small enough that this is fine, and it
	// runs only in the reader, not the evaluator.
	i = 0;
	while (i < atom_count) {
		if (atom_lens[i] == len && !memcmp(atom_names[i], s, len))
			return i;
		++i;
	}
	if (atom_count == atom_cap) {
		atom_cap = atom_cap * 2;
		if (!(nnames = malloc(atom_cap * sizeof(int))) ||
		    !(nlens  = malloc(atom_cap * sizeof(int)))) {
			c4sp_error("out of memory growing atom table");
			return 0;
		}
		i = 0;
		while (i < atom_count) {
			nnames[i] = atom_names[i];
			nlens[i]  = atom_lens[i];
			++i;
		}
		free(atom_names); free(atom_lens);
		atom_names = nnames;
		atom_lens  = nlens;
	}
	atom_names[atom_count] = cs_strdup_len(s, len);
	atom_lens[atom_count]  = len;
	return atom_count++;
}

char *atom_name (int id) {
	if (id < 0 || id >= atom_count) return "?badatom?";
	return atom_names[id];
}

// Returns 0 on success.
int atoms_init () {
	atom_cap = 512;
	atom_count = 0;
	if (!(atom_names = malloc(atom_cap * sizeof(int))) ||
	    !(atom_lens  = malloc(atom_cap * sizeof(int)))) {
		printf("c4sp: cannot allocate atom table\n");
		return 1;
	}
	// Must match the A_* enum order above.
	atom_intern("nil", 3);
	atom_intern("true", 4);
	atom_intern("false", 5);
	atom_intern("quote", 5);
	atom_intern("if", 2);
	atom_intern("define", 6);
	atom_intern("set!", 4);
	atom_intern("lambda", 6);
	atom_intern("macro", 5);
	atom_intern("fastmacro", 9);
	atom_intern("begin", 5);
	atom_intern("next", 4);
	atom_intern("load", 4);
	return 0;
}
