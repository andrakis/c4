// c4th: the dictionary.
//
// Enum-as-record-offsets, the idiom this tree uses everywhere a struct
// would go in a dialect that has none (c4.c:53's { Tk, Hash, ... Idsz },
// include/u0.h's KTE__Sz). The execution token IS the header address, so
// >BODY is xt + W__Sz and every accessor is one indexed load.
//
// W_HASH packs the folded name length and its first three characters into
// one word, so FIND rejects almost every candidate with a single integer
// compare instead of a string compare.
//
// W_PRIM and W_NATIVE are unused until the native backend at B5. They go
// in now anyway: growing a header later means rewriting every saved image,
// and layouts here get pinned by golden files the moment B3 lands.

enum { W_LINK,      // previous header, 0 ends the chain
       W_HASH,      // folded (len, first three chars)
       W_NAME,      // char * into the image's name pool, nul terminated
       W_NLEN,      // name length in bytes
       W_FLAGS,
       W_CODE,      // address of do_colon / do_var / do_const / a primitive
       W_PRIM,      // primitive number, or -1        (B5)
       W_NATIVE,    // natively compiled body, or 0   (B5)
       W__Sz };

enum { FL_IMMEDIATE = 1, FL_HIDDEN = 2, FL_COMPONLY = 4, FL_NATIVE = 8 };

int *th_latest;     // most recent header

// Case matters in Forth-2012 only in that the standard words are upper
// case; c4th treats names as case sensitive, which is what gforth does by
// default and what the test suite expects.
int th_hash (char *name, int len) {
	int h;

	h = len;
	if (len > 0) h = h + (name[0] << 8);
	if (len > 1) h = h + (name[1] << 16);
	if (len > 2) h = h + (name[2] << 24);
	return h;
}

int th_streq (char *a, char *b, int len) {
	int i;

	i = 0;
	while (i < len) {
		if (a[i] != b[i]) return 0;
		++i;
	}
	return 1;
}

// Lay down a header and return its xt. The name is copied into the image,
// so the caller's buffer need not outlive the call.
int *th_create (char *name, int len, int flags, int code) {
	int  *xt;
	char *np;
	int   i;

	np = th_alloc_bytes(len + 1);
	if (!np) return 0;
	i = 0;
	while (i < len) { np[i] = name[i]; ++i; }
	np[len] = 0;

	xt = th_here;
	if (th_here + W__Sz > th_limit) { printf("c4th: image full\n"); th_err = 1; return 0; }
	th_here = th_here + W__Sz;

	xt[W_LINK]   = (int)th_latest;
	xt[W_HASH]   = th_hash(name, len);
	xt[W_NAME]   = (int)np;
	xt[W_NLEN]   = len;
	xt[W_FLAGS]  = flags;
	xt[W_CODE]   = code;
	xt[W_PRIM]   = -1;
	xt[W_NATIVE] = 0;
	th_latest = xt;
	return xt;
}

// Most recent definition wins, which is what redefinition means in Forth.
int *th_find (char *name, int len) {
	int *xt;
	int  h;

	h  = th_hash(name, len);
	xt = th_latest;
	while (xt) {
		if (xt[W_HASH] == h && !(xt[W_FLAGS] & FL_HIDDEN)) {
			if (xt[W_NLEN] == len && th_streq((char *)xt[W_NAME], name, len))
				return xt;
		}
		xt = (int *)xt[W_LINK];
	}
	return 0;
}
