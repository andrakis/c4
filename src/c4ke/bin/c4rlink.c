// C4R Link
//
// Link several .c4r files together, resolving external symbols.
// By convention, .c4r files intended to be linked use the extension .c4o or .c4l.
//
// Extensions:
//  - .c4r: C4 Relocatable, an executable file
//  - .c4o: C4R object, an intermediate object file with functions but requiring
//          additional files to produce an executable.
//  - .c4l: C4R library, to be linked with .c4o files
//
// Invocation: c4rlink file1.c4r file2.c4r -o file3.c4r
// Compilation: gcc -Isrc/c4cc -Iinclude -I. src/c4ke/bin/c4rlink.c -o c4rlink
//
// Notes:
//  - All code, data, and patches are copied. This includes unused/unreferenced
//    functions, as well as static functions and data.
//  - Static symbols are never merged: each module keeps its own copy.
//  - Non-static symbols are merged by name. An extern symbol that another
//    module defines resolves to that definition; two non-static definitions
//    keep the first and warn.
//  - Symbol-typed patches (JSR to an extern function) are rewritten to CODE
//    patches once the symbol is resolved. Any still unresolved at the end are
//    an error, unless -r (library mode) is given.
//
// How it works:
//  1: Load every .c4r given on the command line, recording each module's
//     code offset (words) and data offset (bytes) in the merged image.
//  2: Allocate a master structure sized from the summed segment lengths.
//  3: For each module, in order:
//     - append code and data;
//     - append constructors/destructors, values rebased by the code offset;
//     - import symbols, building an id remap table (module symbol id ->
//       master table index); defined symbols get their values rebased;
//     - append patches: addresses rebased by the code offset, CODE/DATA
//       values rebased, symbol references remapped through the id table;
//     - take the entry point from the first module that has one (rebased).
//  4: Resolve: every symbol-typed patch whose master symbol is now defined
//     becomes a CODE patch pointing at the definition.
//  5: Write the master image out.
//
// TODO: implement shared libraries in a similar manner, except that code and
//       data can be anywhere (not necessarily appended to another .c4r.)

#define NO_ASMC4R_MAIN 1
// Also includes c4cc.c, which includes load-c4r.c
#include "asm-c4r.c"

// Structure for holding loaded .c4r modules
enum {
	CL_ID,           // int, id of this entry
	CL_STRUCT,       // int *, c4r structure
	CL_HEADER,       // int *, c4r header
	CL_FILE,         // char *, .c4r file name
	CL_OFFSET_CODE,  // int, word offset of this module's code in the master image
	CL_OFFSET_DATA,  // int, byte offset of this module's data in the master image
	CL_REMAP,        // int *, module symbol id -> master symbol table index
	CL_REMAP_MAX,    // int, entries in CL_REMAP
	CL__Sz
};

int cl_verbose, cl_debug, cl_libmode;

static void show_help (char *spec) {
	printf("%s: Link multiple .c4r files into one\n"
	       "%s: [-dvr] [-o outfile] [--] file1.c4r [...fileN.c4r]\n"
	       "     -d            Turn on debug mode\n"
	       "     -v            Turn on verbose mode\n"
	       "     -r            Library mode: allow unresolved symbols\n"
	       "     -o outfile    Write to outfile (default: a.c4r)\n"
	       "     --            End arguments\n", spec, spec);
}

static int my_strcmp (char *s1, char *s2) { while(*s1 && (*s1 == *s2)) { ++s1; ++s2; } return *s1 - *s2; }

// Find a symbol in the master table by name; returns table index or -1.
static int master_symfind (int *master, char *name, int len) {
	int *hdr, *sym, i, count;
	hdr = (int *)master[C4R_HEADER];
	count = hdr[C4R_HDR_SYMBOLSLEN];
	sym = (int *)master[C4R_SYMBOLS];
	i = 0;
	while (i < count) {
		if (sym[C4R_SYMB_NAMELEN] == len &&
		    !memcmp((char *)sym[C4R_SYMB_NAME], name, len))
			return i;
		++i;
		sym = sym + C4R_SYMB__Sz;
	}
	return -1;
}

// Import one module's symbols into the master table, filling in the module's
// id remap table. Returns 0 on success.
static int import_symbols (int *master, int *cl) {
	int *hdr, *mhdr, *sym, *msym, *remap, *c4r;
	int  i, count, id, maxid, m, defined, mdefined, offsetCode;
	char *name;

	hdr  = (int *)cl[CL_HEADER];
	mhdr = (int *)master[C4R_HEADER];
	c4r  = (int *)cl[CL_STRUCT];
	count = hdr[C4R_HDR_SYMBOLSLEN];
	offsetCode = cl[CL_OFFSET_CODE];

	// Size the remap table from the largest symbol id present.
	maxid = -1;
	sym = (int *)c4r[C4R_SYMBOLS];
	i = 0;
	while (i < count) {
		if (sym[C4R_SYMB_ID] > maxid) maxid = sym[C4R_SYMB_ID];
		++i;
		sym = sym + C4R_SYMB__Sz;
	}
	cl[CL_REMAP_MAX] = maxid + 1;
	remap = 0;
	if (maxid >= 0) {
		if (!(remap = malloc((i = sizeof(int) * (maxid + 1))))) {
			printf("c4rlink: failed to allocate %d bytes for symbol remap\n", i);
			return 1;
		}
		memset(remap, -1, i);
	}
	cl[CL_REMAP] = (int)remap;

	sym = (int *)c4r[C4R_SYMBOLS];
	i = 0;
	while (i < count) {
		id      = sym[C4R_SYMB_ID];
		name    = (char *)sym[C4R_SYMB_NAME];
		defined = !(sym[C4R_SYMB_ATTRS] & ATTR_EXTERN);
		m = -1;
		// Static symbols are private to their module: never merged by name.
		if (!(sym[C4R_SYMB_ATTRS] & ATTR_STATIC))
			m = master_symfind(master, name, sym[C4R_SYMB_NAMELEN]);
		if (m < 0) {
			// New symbol: append to the master table.
			m = mhdr[C4R_HDR_SYMBOLSLEN];
			msym = (int *)master[C4R_SYMBOLS] + (m * C4R_SYMB__Sz);
			msym[C4R_SYMB_ID]      = m;
			msym[C4R_SYMB_TYPE]    = sym[C4R_SYMB_TYPE];
			msym[C4R_SYMB_CLASS]   = sym[C4R_SYMB_CLASS];
			msym[C4R_SYMB_ATTRS]   = sym[C4R_SYMB_ATTRS];
			msym[C4R_SYMB_NAMELEN] = sym[C4R_SYMB_NAMELEN];
			msym[C4R_SYMB_NAME]    = (int)c4r_strcpy_alloc(name);
			msym[C4R_SYMB_VALUE]   = sym[C4R_SYMB_VALUE];
			// A defined function's value is a code offset: rebase it.
			// Undefined externs carry garbage; zero the value for clarity.
			if (sym[C4R_SYMB_CLASS] == C4R_SCLASS_Fun) {
				if (defined) msym[C4R_SYMB_VALUE] = sym[C4R_SYMB_VALUE] + offsetCode;
				else         msym[C4R_SYMB_VALUE] = 0;
			}
			mhdr[C4R_HDR_SYMBOLSLEN] = m + 1;
		} else {
			msym = (int *)master[C4R_SYMBOLS] + (m * C4R_SYMB__Sz);
			mdefined = !(msym[C4R_SYMB_ATTRS] & ATTR_EXTERN);
			if (defined && !mdefined) {
				// This module provides a definition for an extern: resolve it.
				msym[C4R_SYMB_TYPE]  = sym[C4R_SYMB_TYPE];
				msym[C4R_SYMB_CLASS] = sym[C4R_SYMB_CLASS];
				msym[C4R_SYMB_ATTRS] = sym[C4R_SYMB_ATTRS];
				msym[C4R_SYMB_VALUE] = sym[C4R_SYMB_VALUE];
				if (sym[C4R_SYMB_CLASS] == C4R_SCLASS_Fun)
					msym[C4R_SYMB_VALUE] = sym[C4R_SYMB_VALUE] + offsetCode;
				if (cl_verbose)
					printf("c4rlink: '%.*s' resolved by '%s'\n",
					       sym[C4R_SYMB_NAMELEN], name, (char *)cl[CL_FILE]);
			} else if (defined && mdefined) {
				printf("c4rlink: warning: duplicate definition of '%.*s' in '%s', keeping first\n",
				       sym[C4R_SYMB_NAMELEN], name, (char *)cl[CL_FILE]);
			}
			// extern meeting anything: reference the master entry as-is.
		}
		if (remap) remap[id] = m;
		++i;
		sym = sym + C4R_SYMB__Sz;
	}
	return 0;
}

// Merge every module into master. Returns 0 on success.
static int merge_c4rs (int *master, int *c4rs, int count) {
	int *cl, *hdr, *mhdr, *c4r, c4r_index;
	int *srcPatch, *dstPatch, *src, *dst;
	int  i, n, pid, offsetCode, offsetData;

	mhdr = (int *)master[C4R_HEADER];
	dstPatch = (int *)master[C4R_PATCHES];

	c4r_index = 0;
	cl = c4rs;
	while (c4r_index < count) {
		if (!cl[CL_STRUCT]) {
			printf("merge_c4rs error: missing structure\n");
			return 1;
		}
		hdr = (int *)cl[CL_HEADER];
		c4r = (int *)cl[CL_STRUCT];
		offsetCode = cl[CL_OFFSET_CODE];
		offsetData = cl[CL_OFFSET_DATA];

		// Copy code and data
		memcpy((int *)master[C4R_CODE] + offsetCode,
		       (int *)c4r[C4R_CODE], hdr[C4R_HDR_CODELEN] * sizeof(int));
		if (hdr[C4R_HDR_DATALEN])
			memcpy((char *)master[C4R_DATA] + offsetData,
			       (char *)c4r[C4R_DATA], hdr[C4R_HDR_DATALEN]);

		// Entry point: first module that has one wins.
		if (hdr[C4R_HDR_ENTRY] != -1) {
			if (mhdr[C4R_HDR_ENTRY] == -1)
				mhdr[C4R_HDR_ENTRY] = hdr[C4R_HDR_ENTRY] + offsetCode;
			else
				printf("c4rlink: WARNING: ignoring entry point in '%s'\n",
				       (char *)cl[CL_FILE]);
		}

		// Constructors and destructors, rebased by the code offset
		src = (int *)c4r[C4R_CONSTRUCTORS];
		n = hdr[C4R_HDR_CONSTRUCTLEN];
		i = 0;
		while (i < n) {
			dst = (int *)master[C4R_CONSTRUCTORS] + (mhdr[C4R_HDR_CONSTRUCTLEN] * C4R_CNDE__Sz);
			dst[C4R_CNDE_Priority] = src[C4R_CNDE_Priority];
			dst[C4R_CNDE_Value]    = src[C4R_CNDE_Value] + offsetCode;
			mhdr[C4R_HDR_CONSTRUCTLEN] = mhdr[C4R_HDR_CONSTRUCTLEN] + 1;
			src = src + C4R_CNDE__Sz;
			++i;
		}
		src = (int *)c4r[C4R_DESTRUCTORS];
		n = hdr[C4R_HDR_DESTRUCTLEN];
		i = 0;
		while (i < n) {
			dst = (int *)master[C4R_DESTRUCTORS] + (mhdr[C4R_HDR_DESTRUCTLEN] * C4R_CNDE__Sz);
			dst[C4R_CNDE_Priority] = src[C4R_CNDE_Priority];
			dst[C4R_CNDE_Value]    = src[C4R_CNDE_Value] + offsetCode;
			mhdr[C4R_HDR_DESTRUCTLEN] = mhdr[C4R_HDR_DESTRUCTLEN] + 1;
			src = src + C4R_CNDE__Sz;
			++i;
		}

		// Import symbols, building the id remap table
		if (import_symbols(master, cl))
			return 1;

		// Copy and rebase patches
		srcPatch = (int *)c4r[C4R_PATCHES];
		n = hdr[C4R_HDR_PATCHLEN];
		i = 0;
		while (i < n) {
			pid = srcPatch[C4R_PAT_TYPE];
			// Code-resident patch addresses rebase by the code offset
			// (words); data-resident ones (-3/-4) by the data offset
			// (bytes)
			if (pid == C4R_PTYPE_DCODE || pid == C4R_PTYPE_DDATA)
				dstPatch[C4R_PAT_ADDRESS] = offsetData + srcPatch[C4R_PAT_ADDRESS];
			else
				dstPatch[C4R_PAT_ADDRESS] = offsetCode + srcPatch[C4R_PAT_ADDRESS];
			if (pid == C4R_PTYPE_CODE) {
				dstPatch[C4R_PAT_TYPE]  = pid;
				dstPatch[C4R_PAT_VALUE] = offsetCode + srcPatch[C4R_PAT_VALUE];
			} else if (pid == C4R_PTYPE_DATA) {
				dstPatch[C4R_PAT_TYPE]  = pid;
				dstPatch[C4R_PAT_VALUE] = offsetData + srcPatch[C4R_PAT_VALUE];
			} else if (pid == C4R_PTYPE_DCODE) {
				dstPatch[C4R_PAT_TYPE]  = pid;
				dstPatch[C4R_PAT_VALUE] = offsetCode + srcPatch[C4R_PAT_VALUE];
			} else if (pid == C4R_PTYPE_DDATA) {
				dstPatch[C4R_PAT_TYPE]  = pid;
				dstPatch[C4R_PAT_VALUE] = offsetData + srcPatch[C4R_PAT_VALUE];
			} else {
				// Symbol reference: remap to the master table index.
				// The value is resolved in a later pass.
				if (pid < 0 || pid >= cl[CL_REMAP_MAX] || ((int *)cl[CL_REMAP])[pid] < 0) {
					printf("c4rlink: '%s' patch %d references unknown symbol id %d\n",
					       (char *)cl[CL_FILE], i, pid);
					return 1;
				}
				dstPatch[C4R_PAT_TYPE]  = ((int *)cl[CL_REMAP])[pid];
				dstPatch[C4R_PAT_VALUE] = 0;
			}
			mhdr[C4R_HDR_PATCHLEN] = mhdr[C4R_HDR_PATCHLEN] + 1;
			srcPatch = srcPatch + C4R_PAT__Sz;
			dstPatch = dstPatch + C4R_PAT__Sz;
			++i;
		}

		++c4r_index;
		cl = cl + CL__Sz;
	}
	return 0;
}

// Stable-partition the master patch table into the canonical order
// asm-c4r writes: code-resident patches (types -1/-2/symbols) first,
// data-resident (-3/-4) after, each keeping their relative order.
static int canonicalize_patches (int *master) {
	int *mhdr, *patch, *out, *dst;
	int  i, n, t;
	mhdr = (int *)master[C4R_HEADER];
	n = mhdr[C4R_HDR_PATCHLEN];
	if (!n) return 0;
	if (!(out = malloc(sizeof(int) * C4R_PAT__Sz * n))) return 1;
	dst = out;
	patch = (int *)master[C4R_PATCHES];
	i = 0;
	while (i < n) {
		t = patch[C4R_PAT_TYPE];
		if (t >= C4R_PTYPE_DATA || t >= 0) {
			dst[C4R_PAT_TYPE] = t;
			dst[C4R_PAT_ADDRESS] = patch[C4R_PAT_ADDRESS];
			dst[C4R_PAT_VALUE] = patch[C4R_PAT_VALUE];
			dst = dst + C4R_PAT__Sz;
		}
		patch = patch + C4R_PAT__Sz;
		++i;
	}
	patch = (int *)master[C4R_PATCHES];
	i = 0;
	while (i < n) {
		t = patch[C4R_PAT_TYPE];
		if (t == C4R_PTYPE_DCODE || t == C4R_PTYPE_DDATA) {
			dst[C4R_PAT_TYPE] = t;
			dst[C4R_PAT_ADDRESS] = patch[C4R_PAT_ADDRESS];
			dst[C4R_PAT_VALUE] = patch[C4R_PAT_VALUE];
			dst = dst + C4R_PAT__Sz;
		}
		patch = patch + C4R_PAT__Sz;
		++i;
	}
	free((int *)master[C4R_PATCHES]);
	master[C4R_PATCHES] = (int)out;
	return 0;
}

// Rewrite symbol-typed patches whose symbols are now defined into CODE
// patches. Returns the number of unresolved symbol references remaining.
static int resolve_patches (int *master) {
	int *mhdr, *patch, *sym, i, n, t, unresolved;

	mhdr = (int *)master[C4R_HEADER];
	patch = (int *)master[C4R_PATCHES];
	n = mhdr[C4R_HDR_PATCHLEN];
	unresolved = 0;
	i = 0;
	while (i < n) {
		t = patch[C4R_PAT_TYPE];
		if (t >= 0) {
			sym = (int *)master[C4R_SYMBOLS] + (t * C4R_SYMB__Sz);
			if (!(sym[C4R_SYMB_ATTRS] & ATTR_EXTERN)) {
				if (sym[C4R_SYMB_CLASS] == C4R_SCLASS_Fun) {
					patch[C4R_PAT_TYPE]  = C4R_PTYPE_CODE;
					patch[C4R_PAT_VALUE] = sym[C4R_SYMB_VALUE];
				} else {
					printf("c4rlink: symbol '%.*s' is not a function, cannot resolve\n",
					       sym[C4R_SYMB_NAMELEN], (char *)sym[C4R_SYMB_NAME]);
					++unresolved;
				}
			} else {
				++unresolved;
				if (!cl_libmode)
					printf("c4rlink: undefined symbol '%.*s'\n",
					       sym[C4R_SYMB_NAMELEN], (char *)sym[C4R_SYMB_NAME]);
			}
		}
		patch = patch + C4R_PAT__Sz;
		++i;
	}
	return unresolved;
}

// Serialize a c4r structure to a file. Layout must match c4r_load_opt_real
// in load-c4r.c. Returns 0 on success.
static int c4r_write (int *c4r, char *file) {
	int  fd, i, n, tmp;
	int *hdr, *p, *sym;
	char version, wordbits, tmpc;

	if (is_c4()) {
		// The write() syscall does not exist under plain c4/C4KE.
		printf("c4rlink: writing output is not supported under c4\n");
		return 1;
	}

	hdr = (int *)c4r[C4R_HEADER];
	if ((fd = open(file, O_TRUNC | O_WRONLY | O_CREAT, S_IRUSR | S_IWUSR)) < 0) {
		printf("c4rlink: failed to open '%s' for writing\n", file);
		return 1;
	}
	writeoffset = 0;

	// Header
	writechecked(fd, "C4R", 3);
	version  = hdr[C4R_HDR_VERSION];  writechecked(fd, &version, 1);
	wordbits = hdr[C4R_HDR_WORDBITS]; writechecked(fd, &wordbits, 1);
	// Padding, same formula as the loader
	i = sizeof(int) - 1;
	tmp = (5 + i) & ~i;
	while (tmp--) writechecked(fd, "p", 1);
	tmp = hdr[C4R_HDR_ENTRY];        writechecked(fd, &tmp, sizeof(int));
	tmp = hdr[C4R_HDR_CODELEN];      writechecked(fd, &tmp, sizeof(int));
	tmp = hdr[C4R_HDR_DATALEN];      writechecked(fd, &tmp, sizeof(int));
	tmp = hdr[C4R_HDR_PATCHLEN];     writechecked(fd, &tmp, sizeof(int));
	tmp = hdr[C4R_HDR_SYMBOLSLEN];   writechecked(fd, &tmp, sizeof(int));
	tmp = hdr[C4R_HDR_CONSTRUCTLEN]; writechecked(fd, &tmp, sizeof(int));
	tmp = hdr[C4R_HDR_DESTRUCTLEN];  writechecked(fd, &tmp, sizeof(int));

	// Code
	writechecked(fd, "C\0\0\0\0\0\0\0", sizeof(int));
	writechecked(fd, (int *)c4r[C4R_CODE], hdr[C4R_HDR_CODELEN] * sizeof(int));
	// Data
	writechecked(fd, "D\0\0\0\0\0\0\0", sizeof(int));
	if (hdr[C4R_HDR_DATALEN])
		writechecked(fd, (char *)c4r[C4R_DATA], hdr[C4R_HDR_DATALEN]);
	// Patches
	writechecked(fd, "P\0\0\0\0\0\0\0", sizeof(int));
	p = (int *)c4r[C4R_PATCHES];
	n = hdr[C4R_HDR_PATCHLEN];
	i = 0;
	while (i < n) {
		tmp = p[C4R_PAT_TYPE];    writechecked(fd, &tmp, sizeof(int));
		tmp = p[C4R_PAT_ADDRESS]; writechecked(fd, &tmp, sizeof(int));
		tmp = p[C4R_PAT_VALUE];   writechecked(fd, &tmp, sizeof(int));
		p = p + C4R_PAT__Sz;
		++i;
	}
	// Constructors
	writechecked(fd, "c\0\0\0\0\0\0\0", sizeof(int));
	p = (int *)c4r[C4R_CONSTRUCTORS];
	n = hdr[C4R_HDR_CONSTRUCTLEN];
	i = 0;
	while (i < n) {
		tmp = p[C4R_CNDE_Value]; writechecked(fd, &tmp, sizeof(int));
		p = p + C4R_CNDE__Sz;
		++i;
	}
	// Destructors
	writechecked(fd, "d\0\0\0\0\0\0\0", sizeof(int));
	p = (int *)c4r[C4R_DESTRUCTORS];
	n = hdr[C4R_HDR_DESTRUCTLEN];
	i = 0;
	while (i < n) {
		tmp = p[C4R_CNDE_Value]; writechecked(fd, &tmp, sizeof(int));
		p = p + C4R_CNDE__Sz;
		++i;
	}
	// Symbols
	writechecked(fd, "S\0\0\0\0\0\0\0", sizeof(int));
	sym = (int *)c4r[C4R_SYMBOLS];
	n = hdr[C4R_HDR_SYMBOLSLEN];
	i = 0;
	while (i < n) {
		tmp = sym[C4R_SYMB_ID];    writechecked(fd, &tmp, sizeof(int));
		tmp = sym[C4R_SYMB_TYPE];  writechecked(fd, &tmp, sizeof(int));
		tmp = sym[C4R_SYMB_CLASS]; writechecked(fd, &tmp, sizeof(int));
		tmp = sym[C4R_SYMB_ATTRS]; writechecked(fd, &tmp, sizeof(int));
		tmpc = sym[C4R_SYMB_NAMELEN]; writechecked(fd, &tmpc, 1);
		writechecked(fd, (char *)sym[C4R_SYMB_NAME], sym[C4R_SYMB_NAMELEN]);
		tmp = sym[C4R_SYMB_VALUE]; writechecked(fd, &tmp, sizeof(int));
		sym = sym + C4R_SYMB__Sz;
		++i;
	}

	close(fd);
	return 0;
}

int main (int argc, char **argv) {
	int   i, endargs, endopt, failed, unresolved;
	int  *master, *loaded;
	int  *c4rs, *c4rs_it, c4r_alloc, c4r_pos;
	char *spec, *arg, *outfile;
	int  *hdr;
	int   count_code, count_data, count_patch, count_con, count_des, count_sym;

	// Set defaults
	spec = argv[0];
	outfile = "a.c4r";
	c4r_alloc = 256; // should be enough for everyone
	c4r_pos = 0;
	cl_debug = cl_verbose = cl_libmode = 0;
	count_code = count_data = count_patch = count_con = count_des = count_sym = 0;
	endargs = 0;
	failed = 0;

	// Allocate structures
	if (!(master = c4r_create_empty())) {
		printf("%s: failed to allocate master c4r structure\n", spec);
		return 1;
	}
	if (!(c4rs = malloc((i = c4r_alloc * sizeof(int) * CL__Sz)))) {
		c4r_free(master);
		printf("%s: failed to allocate space for %d c4r structures\n", spec, c4r_alloc);
		return 1;
	}
	memset(c4rs, 0, i);
	c4rs_it = c4rs;

	// Parse command line
	--argc; ++argv; // Skip first arg
	if (!argc) {
		show_help(spec);
		return 1;
	}
	while(argc) {
		if (!my_strcmp("--help", *argv)) {
			show_help(spec);
			return 1;
		} else if (!my_strcmp(*argv, "--")) {
			endargs = 1;
		} else if (endargs == 0 && **argv == '-') {
			arg = *argv + 1; // skip dash
			endopt = 0;
			while (!endopt && *arg) {
				if (*arg == 'd') cl_debug = 1;
				else if (*arg == 'v') cl_verbose = 1;
				else if (*arg == 'r') cl_libmode = 1;
				else if (*arg == 'o') {
					endopt = 1;
					// grab outfile from next argv
					--argc; ++argv;
					if (!argc) {
						printf("%s: -o requires a filename\n", spec);
						return 1;
					}
					outfile = *argv;
				} else {
					printf("%s: Unrecognised option: '%c'\n", spec, *arg);
					show_help(spec);
					return 1;
				}
				++arg;
			}
		} else {
			// .c4r to load
			if (c4r_pos >= c4r_alloc) {
				printf("%s: too many files given, maximum %d allowed\n", spec, c4r_alloc);
				return 3;
			}
			if (cl_verbose) printf("%s: loading '%s'...\n", spec, *argv);
			if (!(loaded = c4r_load_opt(*argv, C4ROPT_SYMBOLS))) {
				printf("%s: failed to load '%s', aborting\n", spec, *argv);
				return 2;
			}
			if (cl_verbose) c4r_dump_info(loaded);
			// Record the module and its position in the merged image
			c4rs_it[CL_ID] = c4r_pos;
			c4rs_it[CL_STRUCT] = (int)loaded;
			c4rs_it[CL_HEADER] = loaded[C4R_HEADER];
			c4rs_it[CL_FILE]   = (int)*argv;
			c4rs_it[CL_OFFSET_CODE] = count_code;
			c4rs_it[CL_OFFSET_DATA] = count_data;
			hdr = (int *)c4rs_it[CL_HEADER];
			// Track statistics
			count_code  = count_code  + hdr[C4R_HDR_CODELEN];
			count_data  = count_data  + hdr[C4R_HDR_DATALEN];
			count_patch = count_patch + hdr[C4R_HDR_PATCHLEN];
			count_con   = count_con   + hdr[C4R_HDR_CONSTRUCTLEN];
			count_des   = count_des   + hdr[C4R_HDR_DESTRUCTLEN];
			count_sym   = count_sym   + hdr[C4R_HDR_SYMBOLSLEN];
			// Advance pointer
			c4rs_it = c4rs_it + CL__Sz;
			++c4r_pos;
		}
		--argc; ++argv;
	}

	if (!c4r_pos) {
		show_help(spec);
		return 1;
	}

	if (cl_verbose) {
		printf("%s: command parsing complete, %d c4r structures loaded\n",
		       spec, c4r_pos);
		printf("  : counts - code: %d  data: %d  patches:  %d  cons: %d  des: %d  syms: %d\n",
		       count_code, count_data, count_patch, count_con, count_des, count_sym);
	}

	// Allocate master segments from the summed sizes. Lengths in the master
	// header start at zero and grow as entries are appended; the symbol
	// count can end up below count_sym after deduplication.
	if (cl_verbose) printf("%s: allocating master structure...\n", spec);
	hdr = (int *)master[C4R_HEADER];
	if (!(master[C4R_CODE] = (int)malloc((i = sizeof(int) * count_code)))) {
		printf("%s: failed to allocate %d bytes for master code\n", spec, i);
		return 4;
	}
	hdr[C4R_HDR_CODELEN] = count_code;
	if (count_data && !(master[C4R_DATA] = (int)malloc(count_data))) {
		printf("%s: failed to allocate %d bytes for master data\n", spec, count_data);
		return 5;
	}
	hdr[C4R_HDR_DATALEN] = count_data;
	if (count_patch && !(master[C4R_PATCHES] = (int)malloc((i = sizeof(int) * C4R_PAT__Sz * count_patch)))) {
		printf("%s: failed to allocate %d bytes for master patches\n", spec, i);
		return 6;
	}
	if (count_con && !(master[C4R_CONSTRUCTORS] = (int)malloc((i = sizeof(int) * C4R_CNDE__Sz * count_con)))) {
		printf("%s: failed to allocate %d bytes for master constructors\n", spec, i);
		return 7;
	}
	if (count_des && !(master[C4R_DESTRUCTORS] = (int)malloc((i = sizeof(int) * C4R_CNDE__Sz * count_des)))) {
		printf("%s: failed to allocate %d bytes for master destructors\n", spec, i);
		return 7;
	}
	if (count_sym && !(master[C4R_SYMBOLS] = (int)malloc((i = sizeof(int) * C4R_SYMB__Sz * count_sym)))) {
		printf("%s: failed to allocate %d bytes for master symbols\n", spec, i);
		return 8;
	}

	// Merge, canonicalize patch order, resolve, write
	if (merge_c4rs(master, c4rs, c4r_pos) ||
	    canonicalize_patches(master)) {
		printf("%s: link failure\n", spec);
		failed = 1;
	} else if ((unresolved = resolve_patches(master)) && !cl_libmode) {
		printf("%s: %d unresolved symbol reference(s)\n", spec, unresolved);
		failed = 1;
	} else {
		if (hdr[C4R_HDR_ENTRY] == -1 && !cl_libmode)
			printf("%s: warning: no entry point in any input\n", spec);
		if (cl_verbose) printf("%s: writing to '%s'...\n", spec, outfile);
		if (c4r_write(master, outfile))
			failed = 1;
		else if (cl_verbose)
			printf("%s: wrote %d bytes\n", spec, writeoffset);
	}

	// Cleanup
	c4rs_it = c4rs; i = 0; while(i++ < c4r_pos) {
		if (c4rs_it[CL_STRUCT])
			c4r_free((int *)c4rs_it[CL_STRUCT]);
		if (c4rs_it[CL_REMAP])
			free((int *)c4rs_it[CL_REMAP]);
		c4rs_it[CL_ID] = c4rs_it[CL_STRUCT] = c4rs_it[CL_HEADER] = c4rs_it[CL_REMAP] = 0;
		c4rs_it = c4rs_it + CL__Sz;
	}
	free(c4rs);
	c4r_free(master);
	return failed;
}
