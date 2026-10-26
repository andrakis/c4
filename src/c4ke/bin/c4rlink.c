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
//             c4rlink -s ...          strip: write no symbol table (a release
//                                     build; nothing a loader needs, only what
//                                     c4rdump -s and stacktraces read, and what
//                                     C4IX's loader uses to find __c4ix_systable
//                                     on PLAIN c4 hosts -- c4m-class hosts go
//                                     through the trap gateway and never look)
//             c4rstrip [-o out] in.c4r  the same program under a second name:
//                                     one input, strip on, output over the input
//                                     unless -o (Makefile builds it from this file)
//             c4rlink --gc prog.c4o --start-lib libc-*.c4o -o prog.c4r
//                                     a program with no shared library under
//                                     it (the bare machine, C4DOS): of libc's
//                                     objects only those it needs, and of
//                                     everything only the functions it reaches
// Compilation: gcc -Isrc/c4cc -Iinclude -I. src/c4ke/bin/c4rlink.c -o c4rlink
//
// Notes:
//  - By default all code, data, and patches are copied, of every input. This
//    includes unused/unreferenced functions, as well as static functions and
//    data. Two options, off unless asked for, make the image smaller:
//    --start-lib makes the inputs after it library members, each linked only
//    if it defines a symbol something linked still lacks, as ld does an
//    archive's (lib_select); --gc drops every function that the entry point,
//    the constructors and destructors and the code addresses held in data do
//    not reach (gc_functions). Neither changes the image when not given.
//  - Static symbols are never merged: each module keeps its own copy.
//  - Non-static symbols are merged by name. An extern symbol that another
//    module defines resolves to that definition; two non-static definitions
//    keep the first and warn.
//  - Symbol-typed patches (JSR to an extern function) are rewritten to CODE
//    patches once the symbol is resolved. Any still unresolved at the end are
//    an error, unless -r (library mode) is given.
//
// How it works:
//  1: Load every .c4r given on the command line. With --start-lib, choose
//     the members to link and drop the rest. Record each module's code
//     offset (words) and data offset (bytes) in the merged image.
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
//  5: With --gc, drop the functions nothing reaches and move the rest down.
//  6: Write the master image out.
//
// Shared libraries (-l LIB): LIB is a library (a -r image, named .c4s by
// convention) that is NOT copied in. A reference to something LIB
// defines stays a symbol patch in the output, and the output records
// that it needs LIB: a static global named __c4_needed whose value is
// the data offset of LIB's run-time path, "/lib/" and LIB's base name.
// C4IX's loader (src/c4ix/loader.c) reads those, loads each library,
// and resolves the patches against what it exports. The record is a
// symbol rather than a new section because every reader of .c4r in
// this repo already skips symbols it has no use for; so a dynamic
// image keeps its symbol table, and -s is refused for one.
//
// Source maps (docs/c4-explorer.md 2.4): an object compiled with -g
// carries an SMAP chunk after S, which says what file and line each run
// of its code came from. The linked image gets one map that is the
// objects' maps, rebased (link_srcmap). -s drops it with the symbols;
// -g0 drops only the map, for a dynamic image, which keeps its symbols.
// When no object has a map, no trailer is written, and the image is the
// same bytes as before maps.

#define NO_ASMC4R_MAIN 1
// Also includes c4cc.c, which includes load-c4r.c
enum { CL_DOSMAX = 1048576 };   // biggest object c4rlink reads from DOS

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
	CL_MEMBER,       // int, 1: a library member (--start-lib), linked only if wanted
	CL__Sz
};

int cl_verbose, cl_debug, cl_libmode, cl_strip, cl_striptool, cl_nomap;

// A data word holding a symbol's address (M7b): ADDRESS is a byte offset
// into data, VALUE the symbol's id. Only objects and libraries carry it --
// c4lc and c4fc write one for an extern function an initializer names,
// such as Busybox's applet_main[] -- and resolving it makes it DCODE or
// DDATA, so no image a loader sees has one. For data, the word holds an
// offset to add, Doom's &S_sfx[sfx_pistol] (zero for the symbol itself).
enum { C4R_PTYPE_DSYM = -5 };
// -l: per library, its loaded structure, its run-time path, and whether
// anything was resolved against it. CL_LIBMAX of them, malloc'd: c4cc
// has no global arrays.
enum { CL_LIBMAX = 8, CLL_STRUCT = 0, CLL_PATH, CLL_USED, CLL__Sz };
int *cl_libs;
int  cl_nlibs;

static void show_help (char *spec) {
	if (cl_striptool)
	printf("%s: Strip the symbol table from a .c4r\n"
	       "%s: [-v] [-o outfile] [--] file.c4r\n"
	       "     -v            Turn on verbose mode\n"
	       "     -o outfile    Write to outfile (default: overwrite file.c4r)\n"
	       "     --            End arguments\n", spec, spec);
	else
	printf("%s: Link multiple .c4r files into one\n"
	       "%s: [-dvrs] [-g0] [-l lib.c4s] [-o outfile] [--] file1.c4r [...fileN.c4r]\n"
	       "         [--gc] [--start-lib member.c4o ... [--end-lib]]\n"
	       "     -d            Turn on debug mode\n"
	       "     -v            Turn on verbose mode\n"
	       "     -r            Library mode: allow unresolved symbols\n"
	       "     -s            Strip: write no symbol table and no source map\n"
		       "     -g0           Write no source map, but keep the symbols\n"
	       "     -l lib.c4s    Link against a shared library: load it at run time\n"
	       "     -o outfile    Write to outfile (default: a.c4r)\n"
	       "     --start-lib   The inputs after it are library members: each is\n"
	       "                   linked only if it defines a symbol still undefined\n"
	       "     --end-lib     The inputs after it are linked whole again\n"
	       "     --gc          Drop the functions nothing reaches\n"
	       "     --            End arguments\n", spec, spec);
}

static int my_strcmp (char *s1, char *s2) { while(*s1 && (*s1 == *s2)) { ++s1; ++s2; } return *s1 - *s2; }
static int my_strncmp (char *s1, char *s2, int n) { while(n > 1 && *s1 && (*s1 == *s2)) { ++s1; ++s2; --n; } return n ? *s1 - *s2 : 0; }

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
	int  i, count, id, maxid, m, defined, mdefined, offsetCode, offsetData;
	char *name;

	hdr  = (int *)cl[CL_HEADER];
	mhdr = (int *)master[C4R_HEADER];
	c4r  = (int *)cl[CL_STRUCT];
	count = hdr[C4R_HDR_SYMBOLSLEN];
	offsetCode = cl[CL_OFFSET_CODE];
	offsetData = cl[CL_OFFSET_DATA];

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
			// A defined function's value is a code offset and a defined
			// global's value is a data byte offset: rebase them.
			// Undefined externs carry garbage; zero the value for clarity.
			if (sym[C4R_SYMB_CLASS] == C4R_SCLASS_Fun) {
				if (defined) msym[C4R_SYMB_VALUE] = sym[C4R_SYMB_VALUE] + offsetCode;
				else         msym[C4R_SYMB_VALUE] = 0;
			} else if (sym[C4R_SYMB_CLASS] == C4R_SCLASS_Glo) {
				if (defined) msym[C4R_SYMB_VALUE] = sym[C4R_SYMB_VALUE] + offsetData;
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
				else if (sym[C4R_SYMB_CLASS] == C4R_SCLASS_Glo)
					msym[C4R_SYMB_VALUE] = sym[C4R_SYMB_VALUE] + offsetData;
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
			if (pid == C4R_PTYPE_DCODE || pid == C4R_PTYPE_DDATA || pid == C4R_PTYPE_DSYM)
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
			} else if (pid == C4R_PTYPE_DSYM) {
				// the symbol id is in VALUE: remap it as a symbol type is
				if (srcPatch[C4R_PAT_VALUE] < 0 || srcPatch[C4R_PAT_VALUE] >= cl[CL_REMAP_MAX] ||
				    ((int *)cl[CL_REMAP])[srcPatch[C4R_PAT_VALUE]] < 0) {
					printf("c4rlink: '%s' patch %d references unknown symbol id %d\n",
					       (char *)cl[CL_FILE], i, srcPatch[C4R_PAT_VALUE]);
					return 1;
				}
				dstPatch[C4R_PAT_TYPE]  = pid;
				dstPatch[C4R_PAT_VALUE] = ((int *)cl[CL_REMAP])[srcPatch[C4R_PAT_VALUE]];
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
		if (t == C4R_PTYPE_DCODE || t == C4R_PTYPE_DDATA || t == C4R_PTYPE_DSYM) {
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

// Blank the operand words that the patch table covers, so that linking the
// same objects twice produces the same bytes.
//
// c4rlink reads its inputs through c4r_load_opt(), which APPLIES their
// patches -- so every patched word arrives already holding a real address
// in this process's heap. Merging copies those words into the master image
// and writing them out puts them in the file. That is why linking the same
// twelve objects twice gave images of identical size differing in 7,395
// bytes: the varying part was ASLR, and under setarch -R the two links
// matched exactly.
//
// The words are dead. load-c4r.c's patch loop assigns each target outright
// from the entry's VALUE field and never reads what was there, and c4rlink
// itself only ever rebases the table's ADDRESS and VALUE fields -- neither
// consults the word. c4r.lisp already documents that an operand emitted
// without its raw value is written as 0 and loads identically. So zeroing
// costs nothing and buys reproducible images, which matters because
// byte-comparison is how nearly everything in this tree is verified.
//
// Address space follows the patch type: DCODE and DDATA name a byte offset
// into data; everything else -- including symbol-typed patches still
// unresolved in library mode -- names a word index into code.
static void blank_patch_targets (int *master) {
	int  *mhdr, *patch, *code;
	char *data;
	int   i, n, t;

	mhdr  = (int *)master[C4R_HEADER];
	patch = (int *)master[C4R_PATCHES];
	code  = (int *)master[C4R_CODE];
	data  = (char *)master[C4R_DATA];
	n = mhdr[C4R_HDR_PATCHLEN];
	i = 0;
	while (i < n) {
		t = patch[C4R_PAT_TYPE];
		if (t == C4R_PTYPE_DCODE || t == C4R_PTYPE_DDATA || t == C4R_PTYPE_DSYM) {
			if (data) *(int *)(data + patch[C4R_PAT_ADDRESS]) = 0;
		} else {
			if (code) code[patch[C4R_PAT_ADDRESS]] = 0;
		}
		patch = patch + C4R_PAT__Sz;
		++i;
	}
}

// Rewrite symbol-typed patches whose symbols are now defined into CODE
// patches. Returns the number of unresolved symbol references remaining.
// The -l library that defines NAME (non-static, a function or a global),
// or -1.
static int lib_defines (char *name, int len) {
	int *c4r, *hdr, *sym, k, i, n;
	k = 0;
	while (k < cl_nlibs) {
		c4r = (int *)cl_libs[k * CLL__Sz + CLL_STRUCT];
		hdr = (int *)c4r[C4R_HEADER];
		sym = (int *)c4r[C4R_SYMBOLS];
		n = hdr[C4R_HDR_SYMBOLSLEN];
		i = 0;
		while (i < n) {
			if (sym[C4R_SYMB_NAMELEN] == len && !(sym[C4R_SYMB_ATTRS] & (ATTR_EXTERN | ATTR_STATIC)) &&
			    (sym[C4R_SYMB_CLASS] == C4R_SCLASS_Fun || sym[C4R_SYMB_CLASS] == C4R_SCLASS_Glo) &&
			    !memcmp((char *)sym[C4R_SYMB_NAME], name, len))
				return k;
			sym = sym + C4R_SYMB__Sz;
			++i;
		}
		++k;
	}
	return -1;
}

// Record each library something was resolved against: its run-time path
// appended to the data, and a static __c4_needed symbol pointing at it.
// The symbol table has room: main allocates CL_LIBMAX spare entries.
static int add_needed (int *master) {
	int *mhdr, *sym, k, n, len, memsz;
	char *path, *ndata;
	mhdr = (int *)master[C4R_HEADER];
	k = 0;
	while (k < cl_nlibs) {
		if (cl_libs[k * CLL__Sz + CLL_USED]) {
			path = (char *)cl_libs[k * CLL__Sz + CLL_PATH];
			len = 0; while (path[len]) ++len;
			memsz = mhdr[C4R_HDR_MEMSZ];
			if (!(ndata = malloc(memsz + len + 1))) return 1;
			if (memsz) memcpy(ndata, (char *)master[C4R_DATA], memsz);
			memcpy(ndata + memsz, path, len + 1);
			if (master[C4R_DATA]) free((char *)master[C4R_DATA]);
			master[C4R_DATA] = (int)ndata;
			mhdr[C4R_HDR_MEMSZ] = memsz + len + 1;
			n = mhdr[C4R_HDR_SYMBOLSLEN];
			sym = (int *)master[C4R_SYMBOLS] + (n * C4R_SYMB__Sz);
			sym[C4R_SYMB_ID]      = n;
			sym[C4R_SYMB_TYPE]    = 2;                  // char *
			sym[C4R_SYMB_CLASS]   = C4R_SCLASS_Glo;
			sym[C4R_SYMB_ATTRS]   = ATTR_STATIC;
			sym[C4R_SYMB_NAMELEN] = 11;
			sym[C4R_SYMB_NAME]    = (int)c4r_strcpy_alloc("__c4_needed");
			sym[C4R_SYMB_VALUE]   = memsz;
			mhdr[C4R_HDR_SYMBOLSLEN] = n + 1;
			if (cl_verbose) printf("c4rlink: needs %s\n", path);
		}
		++k;
	}
	return 0;
}

static int resolve_patches (int *master) {
	int *mhdr, *patch, *sym, i, n, t, k, unresolved;

	mhdr = (int *)master[C4R_HEADER];
	patch = (int *)master[C4R_PATCHES];
	n = mhdr[C4R_HDR_PATCHLEN];
	unresolved = 0;
	i = 0;
	while (i < n) {
		t = patch[C4R_PAT_TYPE];
		if (t == C4R_PTYPE_DSYM) {
			// a data word: DCODE or DDATA once the symbol is defined
			sym = (int *)master[C4R_SYMBOLS] + (patch[C4R_PAT_VALUE] * C4R_SYMB__Sz);
			if (!(sym[C4R_SYMB_ATTRS] & ATTR_EXTERN)) {
				if (sym[C4R_SYMB_CLASS] == C4R_SCLASS_Fun) {
					patch[C4R_PAT_TYPE]  = C4R_PTYPE_DCODE;
					patch[C4R_PAT_VALUE] = sym[C4R_SYMB_VALUE];
				} else if (sym[C4R_SYMB_CLASS] == C4R_SCLASS_Glo) {
					// &a[k] (docs/ports-feasibility.md 8, step 4): the
					// compiler leaves the offset in the word itself
					patch[C4R_PAT_TYPE]  = C4R_PTYPE_DDATA;
					patch[C4R_PAT_VALUE] = sym[C4R_SYMB_VALUE] +
						*(int *)((char *)master[C4R_DATA] + patch[C4R_PAT_ADDRESS]);
				} else {
					printf("c4rlink: symbol '%.*s' is not a function or global, cannot resolve\n",
					       sym[C4R_SYMB_NAMELEN], (char *)sym[C4R_SYMB_NAME]);
					++unresolved;
				}
			} else {
				// a shared library's symbol in a data word would need the
				// loader to patch data, which it does not yet
				++unresolved;
				if (!cl_libmode)
					printf("c4rlink: undefined symbol '%.*s' (named in data)\n",
					       sym[C4R_SYMB_NAMELEN], (char *)sym[C4R_SYMB_NAME]);
			}
		} else if (t >= 0) {
			sym = (int *)master[C4R_SYMBOLS] + (t * C4R_SYMB__Sz);
			if (!(sym[C4R_SYMB_ATTRS] & ATTR_EXTERN)) {
				if (sym[C4R_SYMB_CLASS] == C4R_SCLASS_Fun) {
					patch[C4R_PAT_TYPE]  = C4R_PTYPE_CODE;
					patch[C4R_PAT_VALUE] = sym[C4R_SYMB_VALUE];
				} else if (sym[C4R_SYMB_CLASS] == C4R_SCLASS_Glo) {
					patch[C4R_PAT_TYPE]  = C4R_PTYPE_DATA;
					patch[C4R_PAT_VALUE] = sym[C4R_SYMB_VALUE];
				} else {
					printf("c4rlink: symbol '%.*s' is not a function or global, cannot resolve\n",
					       sym[C4R_SYMB_NAMELEN], (char *)sym[C4R_SYMB_NAME]);
					++unresolved;
				}
			} else if ((k = lib_defines((char *)sym[C4R_SYMB_NAME], sym[C4R_SYMB_NAMELEN])) >= 0) {
				// a shared library has it: the patch stays a symbol
				// reference, for the loader
				cl_libs[k * CLL__Sz + CLL_USED] = 1;
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

// ---- The source map ----
//
// An SMAP payload (docs/c4-explorer.md 2.2, c4r.lisp's c4r:smap) is
// bytes with unsigned and signed LEB128 numbers: a version (1) and flags
// (bit 0: each file has a CRC-32), the files (path, size, CRC), the
// functions (start as a delta from the one before, length in words,
// name, file, line) and the rows (code delta << 1 | a new file follows,
// the file, a signed line delta). A row covers the code up to the next.

// A byte buffer that grows: SB_BYTES (char *), the bytes in it, its room
enum { SB_BYTES, SB_LEN, SB_CAP, SB__Sz };
int cl_oom;      // a buffer could not grow: the map is dropped

static int *sb_new () {
	int *sb;
	if (!(sb = malloc(sizeof(int) * SB__Sz))) { cl_oom = 1; return 0; }
	sb[SB_CAP] = 256;
	sb[SB_LEN] = 0;
	if (!(sb[SB_BYTES] = (int)malloc(sb[SB_CAP]))) { cl_oom = 1; free(sb); return 0; }
	return sb;
}
static void sb_free (int *sb) {
	if (!sb) return;
	if (sb[SB_BYTES]) free((char *)sb[SB_BYTES]);
	free(sb);
}
static void sb_put (int *sb, int c) {
	char *b;
	if (!sb || cl_oom) return;
	if (sb[SB_LEN] >= sb[SB_CAP]) {
		if (!(b = malloc(sb[SB_CAP] * 2))) { cl_oom = 1; return; }
		memcpy(b, (char *)sb[SB_BYTES], sb[SB_LEN]);
		free((char *)sb[SB_BYTES]);
		sb[SB_BYTES] = (int)b;
		sb[SB_CAP] = sb[SB_CAP] * 2;
	}
	b = (char *)sb[SB_BYTES];
	b[sb[SB_LEN]] = c;
	sb[SB_LEN] = sb[SB_LEN] + 1;
}
static void sb_mem (int *sb, char *p, int n) {
	int i;
	i = 0;
	while (i < n) { sb_put(sb, p[i]); ++i; }
}
static void sb_u (int *sb, int v) {
	while (v >= 128) { sb_put(sb, 128 + (v & 127)); v = v >> 7; }
	sb_put(sb, v);
}
// (v - b) / 128 is exact, so this needs no arithmetic shift
static void sb_s (int *sb, int v) {
	int b, more;
	more = 1;
	while (more) {
		b = v & 127;
		v = (v - b) / 128;
		if ((v == 0 && !(b & 64)) || (v == -1 && (b & 64))) more = 0;
		else b = b + 128;
		sb_put(sb, b);
	}
}

// Reading a map: cl_mat is the cursor, as c4rdump's c4rd_at is
int cl_mat;
static int cl_mu (char *p) {
	int v, s, c;
	c = p[cl_mat++] & 255;
	v = c & 127; s = 7;
	while (c & 128) { c = p[cl_mat++] & 255; v = v | ((c & 127) << s); s = s + 7; }
	return v;
}
static int cl_ms (char *p) {
	int v, s, c;
	c = p[cl_mat++] & 255;
	v = c & 127; s = 7;
	while (c & 128) { c = p[cl_mat++] & 255; v = v | ((c & 127) << s); s = s + 7; }
	if ((c & 64) && s < sizeof(int) * 8) v = v | (-1 << s);
	return v;
}

// The merged rows, and the row before the next: offset, file, line
int cl_roff, cl_rfile, cl_rline, cl_nrows;
static void cl_row (int *rows, int off, int file, int line) {
	if (file == cl_rfile) sb_u(rows, (off - cl_roff) * 2);
	else { sb_u(rows, (off - cl_roff) * 2 + 1); sb_u(rows, file); }
	sb_s(rows, line - cl_rline);
	cl_roff = off; cl_rfile = file; cl_rline = line;
	++cl_nrows;
}

// A module's map, if it is one this reads: version 1
static char *cl_map (int *c4r) {
	char *p;
	p = (char *)c4r[C4R_SRCMAP];
	if (!p || c4r[C4R_SRCMAPLEN] < 3 || (p[0] & 255) != 1) return 0;
	return p;
}

// The merged file table: per file its path (in a module's map), the
// path's length, the size and the CRC's four bytes (or 0)
enum { MF_PATH, MF_PLEN, MF_SIZE, MF_CRC, MF__Sz };

// Merge the modules' maps into master[C4R_SRCMAP], as linking merges
// their code: modules in command-line order, each at CL_OFFSET_CODE.
// - Files are unioned by path; a module's file numbers go through FM.
// - Function starts and row offsets move by the module's code offset.
// - A module with no map has one row of line 0 at its start, so that the
//   module before does not seem to cover its code; so does a module
//   whose first row is past its start.
// Nothing is made when no module has a map. Returns 0, or 1 when out of
// memory (the image is then written without a map).
static int link_srcmap (int *master, int *c4rs, int count) {
	int *cl, *c4r, *hdr, *files, *fm, *funcs, *rows, *out;
	int  k, i, j, n, nf, len, flags, crcs, maxfiles, nfiles, nfuncs, have;
	int  base, codelen, start, fstart, pl, size, off, file, line, x, first;
	char *p, *path, *crc, *q;

	have = 0; maxfiles = 0;
	cl = c4rs; k = 0;
	while (k < count) {
		c4r = (int *)cl[CL_STRUCT];
		if ((p = cl_map(c4r))) {
			have = 1;
			cl_mat = 2;
			maxfiles = maxfiles + cl_mu(p);
		} else if (c4r[C4R_SRCMAP])
			printf("c4rlink: warning: '%s' has a source map of a version this does not know; it is left out\n",
			       (char *)cl[CL_FILE]);
		cl = cl + CL__Sz; ++k;
	}
	if (!have) return 0;

	cl_oom = 0;
	files = malloc(sizeof(int) * MF__Sz * (maxfiles + 1));
	fm = malloc(sizeof(int) * (maxfiles + 1));
	funcs = sb_new();
	rows = sb_new();
	if (!files || !fm) cl_oom = 1;
	nfiles = 0; nfuncs = 0; crcs = 1;
	cl_roff = 0; cl_rfile = 0; cl_rline = 0; cl_nrows = 0;
	fstart = 0;

	cl = c4rs; k = 0;
	while (k < count && !cl_oom) {
		c4r = (int *)cl[CL_STRUCT];
		hdr = (int *)cl[CL_HEADER];
		base = cl[CL_OFFSET_CODE];
		codelen = hdr[C4R_HDR_CODELEN];
		p = cl_map(c4r);
		len = c4r[C4R_SRCMAPLEN];
		if (!p) {
			if (codelen) cl_row(rows, base, cl_rfile, 0);
		} else {
			flags = p[1] & 255;
			if (!(flags & 1)) crcs = 0;
			cl_mat = 2;
			// files: the first module to name a path gives its size and CRC
			nf = cl_mu(p);
			i = 0;
			while (i < nf && cl_mat < len) {
				pl = cl_mu(p); path = p + cl_mat; cl_mat = cl_mat + pl;
				size = cl_mu(p);
				crc = 0;
				if (flags & 1) { crc = p + cl_mat; cl_mat = cl_mat + 4; }
				j = 0;
				while (j < nfiles && !(files[j * MF__Sz + MF_PLEN] == pl &&
				                       !memcmp((char *)files[j * MF__Sz + MF_PATH], path, pl))) ++j;
				if (j == nfiles) {
					files[j * MF__Sz + MF_PATH] = (int)path;
					files[j * MF__Sz + MF_PLEN] = pl;
					files[j * MF__Sz + MF_SIZE] = size;
					files[j * MF__Sz + MF_CRC]  = (int)crc;
					++nfiles;
				} else {
					q = (char *)files[j * MF__Sz + MF_CRC];
					if (files[j * MF__Sz + MF_SIZE] != size || (q && crc && memcmp(q, crc, 4)))
						printf("c4rlink: warning: '%s' was compiled from another %.*s than the objects before it\n",
						       (char *)cl[CL_FILE], pl, path);
				}
				fm[i] = j;
				++i;
			}
			// functions
			n = cl_mu(p);
			i = 0; start = 0;
			while (i < n && cl_mat < len) {
				start = start + cl_mu(p);
				sb_u(funcs, base + start - fstart);
				fstart = base + start;
				sb_u(funcs, cl_mu(p));                  // length in words
				pl = cl_mu(p);
				sb_u(funcs, pl);
				sb_mem(funcs, p + cl_mat, pl);
				cl_mat = cl_mat + pl;
				file = cl_mu(p);
				sb_u(funcs, file < nf ? fm[file] : 0);
				sb_u(funcs, cl_mu(p));                  // line
				++nfuncs; ++i;
			}
			// rows
			n = cl_mu(p);
			i = 0; off = 0; file = 0; line = 0; first = 1;
			while (i < n && cl_mat < len) {
				x = cl_mu(p);
				off = off + (x >> 1);
				if (x & 1) file = cl_mu(p);
				line = line + cl_ms(p);
				if (first && off > 0 && codelen) cl_row(rows, base, cl_rfile, 0);
				first = 0;
				if (off < codelen) cl_row(rows, base + off, file < nf ? fm[file] : 0, line);
				++i;
			}
			if (first && codelen) cl_row(rows, base, cl_rfile, 0);
			if (cl_mat > len)
				printf("c4rlink: warning: '%s' has a damaged source map\n", (char *)cl[CL_FILE]);
		}
		cl = cl + CL__Sz; ++k;
	}

	out = sb_new();
	sb_put(out, 1);                                     // version
	sb_put(out, crcs);                                  // flags
	sb_u(out, nfiles);
	j = 0;
	while (j < nfiles && files) {
		sb_u(out, files[j * MF__Sz + MF_PLEN]);
		sb_mem(out, (char *)files[j * MF__Sz + MF_PATH], files[j * MF__Sz + MF_PLEN]);
		sb_u(out, files[j * MF__Sz + MF_SIZE]);
		if (crcs) sb_mem(out, (char *)files[j * MF__Sz + MF_CRC], 4);
		++j;
	}
	sb_u(out, nfuncs);
	if (funcs) sb_mem(out, (char *)funcs[SB_BYTES], funcs[SB_LEN]);
	sb_u(out, cl_nrows);
	if (rows) sb_mem(out, (char *)rows[SB_BYTES], rows[SB_LEN]);
	if (files) free(files);
	if (fm) free(fm);
	sb_free(funcs);
	sb_free(rows);
	if (cl_oom) { sb_free(out); return 1; }
	master[C4R_SRCMAP] = out[SB_BYTES];
	master[C4R_SRCMAPLEN] = out[SB_LEN];
	free(out);
	return 0;
}

// ---- Dropping the functions nothing reaches (--gc) ----
//
// The code is cut into regions at every function's start: a defined
// function's symbol (static ones too), and with a source map each of its
// functions' start and end -- the map also names the code c4lc puts in
// an object without a symbol, its soft-float and long long routines.
// Each input's first word starts a region too. Every word of code that
// holds a code address is a patch, since the loader has to relocate it,
// so a CODE patch from region A to region B is all there is of "A uses
// B". A tail call jumps past its callee's ENT, into the middle of the
// region: that is still B. What is live is what the entry point, the
// constructors and destructors and every code address held in data
// (DCODE: function pointers in initializers, tables) reach; with -r or
// -l also every function another image could name. The rest goes: its
// code, its patches, its functions' symbols, its part of the source map.
// Everything after moves down. Data is not touched, so globals a loader
// looks up by name (__c4dos_api, __c4ke_exit) are all still there.
//
// One thing would make that unsafe: code that runs on, without a jump,
// into the region after it. So each region is decoded, and one that
// does not end exactly at the next start on a JMP, LEV, JMPA or TLEV is
// glued to the next one, which then stays or goes with it.

int cl_gc;

// Does opcode OP carry an operand word? (c4m.c's c4m_has_operand)
static int gc_operand (int op) {
	return op <= ADJ || op == JSRI || op == JSRS || (op >= LDL && op <= IMMP) || op == STL;
}

// The region holding code word A: the last of RS's N starts at or before it
static int gc_region (int *rs, int n, int a) {
	int lo, hi, mid;
	lo = 0; hi = n - 1;
	while (lo < hi) {
		mid = (lo + hi + 1) / 2;
		if (rs[mid] <= a) lo = mid; else hi = mid - 1;
	}
	return lo;
}

// Where code word A ends up: its region's new start, plus its place in it
static int gc_moved (int *rs, int *newpos, int n, int a) {
	int r;
	r = gc_region(rs, n, a);
	return newpos[r] + a - rs[r];
}

// Make region R live, and queue it to follow what it uses
int *gc_live, *gc_work, gc_nwork;
static void gc_reach (int r) {
	if (r < 0 || gc_live[r]) return;
	gc_live[r] = 1;
	gc_work[gc_nwork++] = r;
}

// The source map, for what is left: the files as they were, the live
// functions, and the rows in live code. Where a removed region ends
// and code that had no row of its own follows, a row says what the
// code there was. Returns 0, or 1 when out of memory.
static int gc_srcmap (int *master, int *rs, int *newpos, int nreg) {
	int *funcs, *rows, *out, *ro, *rf, *rl;
	int  nfiles, flags, i, j, n, r, nf, start, len, pl, file, line, nstart, kept, x, off, s, e;
	int  have, cf, cline, prevdead, filesend;
	char *p;
	p = cl_map(master);
	if (!p) return 0;
	cl_mat = 1;
	flags = cl_mu(p);
	nfiles = cl_mu(p);
	i = 0;
	while (i < nfiles) { pl = cl_mu(p); cl_mat = cl_mat + pl; cl_mu(p); if (flags & 1) cl_mat = cl_mat + 4; ++i; }
	filesend = cl_mat;
	cl_oom = 0;
	funcs = sb_new();
	rows = sb_new();
	// functions
	nf = cl_mu(p);
	i = 0; start = 0; nstart = 0; kept = 0;
	while (i < nf) {
		start = start + cl_mu(p);
		len = cl_mu(p);
		pl = cl_mu(p);
		x = cl_mat;
		cl_mat = cl_mat + pl;
		file = cl_mu(p);
		line = cl_mu(p);
		r = gc_region(rs, nreg, start);
		if (gc_live[r]) {
			off = newpos[r] + start - rs[r];
			sb_u(funcs, off - nstart);
			nstart = off;
			sb_u(funcs, len);
			sb_u(funcs, pl);
			sb_mem(funcs, p + x, pl);
			sb_u(funcs, file);
			sb_u(funcs, line);
			++kept;
		}
		++i;
	}
	// rows: read them all, then write those in live code
	n = cl_mu(p);
	ro = malloc(sizeof(int) * (n + 1));
	rf = malloc(sizeof(int) * (n + 1));
	rl = malloc(sizeof(int) * (n + 1));
	if (!ro || !rf || !rl) cl_oom = 1;
	i = 0; off = 0; file = 0; line = 0;
	while (i < n && !cl_oom) {
		x = cl_mu(p);
		off = off + (x >> 1);
		if (x & 1) file = cl_mu(p);
		line = line + cl_ms(p);
		ro[i] = off; rf[i] = file; rl[i] = line;
		++i;
	}
	cl_roff = 0; cl_rfile = 0; cl_rline = 0; cl_nrows = 0;
	j = 0; have = 0; cf = 0; cline = 0; prevdead = 0;
	r = 0;
	while (r < nreg && !cl_oom) {
		if (!gc_live[r]) prevdead = 1;
		else {
			s = rs[r]; e = rs[r + 1];
			// rows in the removed code before it: what holds where it starts
			while (j < n && ro[j] < s) { have = 1; cf = rf[j]; cline = rl[j]; ++j; }
			if (prevdead && have && !(j < n && ro[j] == s)) cl_row(rows, newpos[r], cf, cline);
			while (j < n && ro[j] < e) {
				cl_row(rows, newpos[r] + ro[j] - s, rf[j], rl[j]);
				have = 1; cf = rf[j]; cline = rl[j];
				++j;
			}
			prevdead = 0;
		}
		++r;
	}
	if (ro) free(ro);
	if (rf) free(rf);
	if (rl) free(rl);
	out = sb_new();
	sb_mem(out, p, filesend);                            // version, flags, files
	sb_u(out, kept);
	if (funcs) sb_mem(out, (char *)funcs[SB_BYTES], funcs[SB_LEN]);
	sb_u(out, cl_nrows);
	if (rows) sb_mem(out, (char *)rows[SB_BYTES], rows[SB_LEN]);
	sb_free(funcs);
	sb_free(rows);
	if (cl_oom) { sb_free(out); return 1; }
	free((char *)master[C4R_SRCMAP]);
	master[C4R_SRCMAP] = out[SB_BYTES];
	master[C4R_SRCMAPLEN] = out[SB_LEN];
	free(out);
	return 0;
}

// Drop the regions nothing reaches. Returns 0, or 1 when out of memory.
static int gc_functions (int *master, int *c4rs, int count) {
	int *mhdr, *code, *sym, *patch, *q, *rs, *newpos, *adjn, *adj, *idmap, *cd;
	int  codelen, nreg, i, j, k, n, r, t, a, pc, e, op, np, words, nfun, kfun, flags, nf, start, len;
	char *mark, *p;

	mhdr = (int *)master[C4R_HEADER];
	code = (int *)master[C4R_CODE];
	codelen = mhdr[C4R_HDR_CODELEN];
	if (!codelen) return 0;
	if (!(mark = malloc(codelen + 1))) return 1;
	memset(mark, 0, codelen + 1);

	// The starts: word 0, each input's first word, each function
	mark[0] = 1;
	k = 0;
	while (k < count) {
		a = c4rs[k * CL__Sz + CL_OFFSET_CODE];
		if (a >= 0 && a < codelen) mark[a] = 1;
		++k;
	}
	sym = (int *)master[C4R_SYMBOLS];
	n = mhdr[C4R_HDR_SYMBOLSLEN];
	i = 0;
	while (i < n) {
		if (sym[C4R_SYMB_CLASS] == C4R_SCLASS_Fun && !(sym[C4R_SYMB_ATTRS] & ATTR_EXTERN) &&
		    sym[C4R_SYMB_VALUE] >= 0 && sym[C4R_SYMB_VALUE] < codelen)
			mark[sym[C4R_SYMB_VALUE]] = 1;
		sym = sym + C4R_SYMB__Sz; ++i;
	}
	if ((p = cl_map(master))) {
		cl_mat = 1;
		flags = cl_mu(p);
		nf = cl_mu(p);
		i = 0;
		while (i < nf) { j = cl_mu(p); cl_mat = cl_mat + j; cl_mu(p); if (flags & 1) cl_mat = cl_mat + 4; ++i; }
		nf = cl_mu(p);
		i = 0; start = 0;
		while (i < nf) {
			start = start + cl_mu(p);
			len = cl_mu(p);
			j = cl_mu(p); cl_mat = cl_mat + j;
			cl_mu(p); cl_mu(p);
			if (start >= 0 && start < codelen) mark[start] = 1;
			if (start + len > 0 && start + len < codelen) mark[start + len] = 1;
			++i;
		}
	}

	// Glue a region that does not end in a jump or return to the next
	pc = 0;
	while (pc < codelen) {
		e = pc + 1;
		while (e < codelen && !mark[e]) ++e;
		op = -1;
		while (pc < e) { op = code[pc]; pc = pc + (gc_operand(op) ? 2 : 1); }
		if (e < codelen && (pc != e || !(op == JMP || op == LEV || op == JMPA || op == TLEV))) {
			if (cl_debug) printf("c4rlink: --gc: code at %d runs on into %d\n", e - 1, e);
			mark[e] = 0;
		}
	}

	// The regions
	nreg = 0;
	i = 0;
	while (i < codelen) { if (mark[i]) ++nreg; ++i; }
	rs = malloc(sizeof(int) * (nreg + 1));
	newpos = malloc(sizeof(int) * (nreg + 1));
	adjn = malloc(sizeof(int) * (nreg + 2));
	gc_live = malloc(sizeof(int) * (nreg + 1));
	gc_work = malloc(sizeof(int) * (nreg + 1));
	if (!rs || !newpos || !adjn || !gc_live || !gc_work) return 1;
	j = 0;
	i = 0;
	while (i < codelen) { if (mark[i]) rs[j++] = i; ++i; }
	rs[nreg] = codelen;
	free(mark);

	// What uses what: per region, the regions its CODE patches name
	patch = (int *)master[C4R_PATCHES];
	np = mhdr[C4R_HDR_PATCHLEN];
	memset(adjn, 0, sizeof(int) * (nreg + 2));
	i = 0; q = patch;
	while (i < np) {
		if (q[C4R_PAT_TYPE] == C4R_PTYPE_CODE)
			adjn[gc_region(rs, nreg, q[C4R_PAT_ADDRESS]) + 1] = adjn[gc_region(rs, nreg, q[C4R_PAT_ADDRESS]) + 1] + 1;
		q = q + C4R_PAT__Sz; ++i;
	}
	r = 0;
	while (r < nreg) { adjn[r + 1] = adjn[r + 1] + adjn[r]; ++r; }
	if (!(adj = malloc(sizeof(int) * (adjn[nreg] + 1)))) return 1;
	// newpos is the fill cursor for now
	memcpy(newpos, adjn, sizeof(int) * nreg);
	i = 0; q = patch;
	while (i < np) {
		if (q[C4R_PAT_TYPE] == C4R_PTYPE_CODE) {
			r = gc_region(rs, nreg, q[C4R_PAT_ADDRESS]);
			adj[newpos[r]] = gc_region(rs, nreg, q[C4R_PAT_VALUE]);
			newpos[r] = newpos[r] + 1;
		}
		q = q + C4R_PAT__Sz; ++i;
	}

	// The roots, and all they reach
	memset(gc_live, 0, sizeof(int) * (nreg + 1));
	gc_nwork = 0;
	if (mhdr[C4R_HDR_ENTRY] >= 0 && mhdr[C4R_HDR_ENTRY] < codelen)
		gc_reach(gc_region(rs, nreg, mhdr[C4R_HDR_ENTRY]));
	cd = (int *)master[C4R_CONSTRUCTORS];
	i = 0;
	while (i < mhdr[C4R_HDR_CONSTRUCTLEN]) { gc_reach(gc_region(rs, nreg, cd[C4R_CNDE_Value])); cd = cd + C4R_CNDE__Sz; ++i; }
	cd = (int *)master[C4R_DESTRUCTORS];
	i = 0;
	while (i < mhdr[C4R_HDR_DESTRUCTLEN]) { gc_reach(gc_region(rs, nreg, cd[C4R_CNDE_Value])); cd = cd + C4R_CNDE__Sz; ++i; }
	i = 0; q = patch;
	while (i < np) {
		if (q[C4R_PAT_TYPE] == C4R_PTYPE_DCODE) gc_reach(gc_region(rs, nreg, q[C4R_PAT_VALUE]));
		q = q + C4R_PAT__Sz; ++i;
	}
	if (cl_libmode || cl_nlibs) {
		sym = (int *)master[C4R_SYMBOLS];
		i = 0;
		while (i < n) {
			if (sym[C4R_SYMB_CLASS] == C4R_SCLASS_Fun && !(sym[C4R_SYMB_ATTRS] & (ATTR_EXTERN | ATTR_STATIC)) &&
			    sym[C4R_SYMB_VALUE] >= 0 && sym[C4R_SYMB_VALUE] < codelen)
				gc_reach(gc_region(rs, nreg, sym[C4R_SYMB_VALUE]));
			sym = sym + C4R_SYMB__Sz; ++i;
		}
	}
	while (gc_nwork) {
		r = gc_work[--gc_nwork];
		k = adjn[r];
		while (k < adjn[r + 1]) { gc_reach(adj[k]); ++k; }
	}
	free(adj); free(adjn); free(gc_work);

	// Where each live region goes
	words = 0;
	r = 0;
	while (r < nreg) {
		newpos[r] = words;
		if (gc_live[r]) words = words + rs[r + 1] - rs[r];
		++r;
	}
	newpos[nreg] = words;
	if (words == codelen) { free(rs); free(newpos); free(gc_live); return 0; }

	// The code, moved down
	r = 0;
	while (r < nreg) {
		if (gc_live[r] && newpos[r] != rs[r]) {
			i = 0;
			while (i < rs[r + 1] - rs[r]) { code[newpos[r] + i] = code[rs[r] + i]; ++i; }
		}
		++r;
	}
	mhdr[C4R_HDR_CODELEN] = words;

	// The patches: those in removed code go, the rest move
	i = 0; q = patch; k = 0;
	while (i < np) {
		t = q[C4R_PAT_TYPE];
		a = 1;                                      // kept
		if (t == C4R_PTYPE_DCODE || t == C4R_PTYPE_DDATA || t == C4R_PTYPE_DSYM) {
			if (t == C4R_PTYPE_DCODE) q[C4R_PAT_VALUE] = gc_moved(rs, newpos, nreg, q[C4R_PAT_VALUE]);
		} else {
			r = gc_region(rs, nreg, q[C4R_PAT_ADDRESS]);
			if (!gc_live[r]) a = 0;
			else {
				q[C4R_PAT_ADDRESS] = newpos[r] + q[C4R_PAT_ADDRESS] - rs[r];
				if (t == C4R_PTYPE_CODE) q[C4R_PAT_VALUE] = gc_moved(rs, newpos, nreg, q[C4R_PAT_VALUE]);
			}
		}
		if (a) {
			if (k != i) memcpy(patch + k * C4R_PAT__Sz, q, sizeof(int) * C4R_PAT__Sz);
			++k;
		}
		q = q + C4R_PAT__Sz; ++i;
	}
	mhdr[C4R_HDR_PATCHLEN] = k;

	// The entry point, constructors and destructors
	if (mhdr[C4R_HDR_ENTRY] >= 0 && mhdr[C4R_HDR_ENTRY] < codelen)
		mhdr[C4R_HDR_ENTRY] = gc_moved(rs, newpos, nreg, mhdr[C4R_HDR_ENTRY]);
	cd = (int *)master[C4R_CONSTRUCTORS];
	i = 0;
	while (i < mhdr[C4R_HDR_CONSTRUCTLEN]) { cd[C4R_CNDE_Value] = gc_moved(rs, newpos, nreg, cd[C4R_CNDE_Value]); cd = cd + C4R_CNDE__Sz; ++i; }
	cd = (int *)master[C4R_DESTRUCTORS];
	i = 0;
	while (i < mhdr[C4R_HDR_DESTRUCTLEN]) { cd[C4R_CNDE_Value] = gc_moved(rs, newpos, nreg, cd[C4R_CNDE_Value]); cd = cd + C4R_CNDE__Sz; ++i; }

	// The symbols: a removed function's goes, the rest are renumbered
	if (!(idmap = malloc(sizeof(int) * (n + 1)))) return 1;
	sym = (int *)master[C4R_SYMBOLS];
	nfun = 0; kfun = 0;
	i = 0; j = 0;
	while (i < n) {
		q = sym + i * C4R_SYMB__Sz;
		idmap[i] = -1;
		if (q[C4R_SYMB_CLASS] == C4R_SCLASS_Fun && !(q[C4R_SYMB_ATTRS] & ATTR_EXTERN) &&
		    q[C4R_SYMB_VALUE] >= 0 && q[C4R_SYMB_VALUE] < codelen) {
			++nfun;
			r = gc_region(rs, nreg, q[C4R_SYMB_VALUE]);
			if (gc_live[r]) { q[C4R_SYMB_VALUE] = newpos[r] + q[C4R_SYMB_VALUE] - rs[r]; ++kfun; }
			else { free((char *)q[C4R_SYMB_NAME]); q = 0; }
		}
		if (q) {
			if (j != i) memcpy(sym + j * C4R_SYMB__Sz, q, sizeof(int) * C4R_SYMB__Sz);
			sym[j * C4R_SYMB__Sz + C4R_SYMB_ID] = j;
			idmap[i] = j;
			++j;
		}
		++i;
	}
	mhdr[C4R_HDR_SYMBOLSLEN] = j;
	// a symbol reference names an extern, which stays
	i = 0; q = patch;
	while (i < k) {
		t = q[C4R_PAT_TYPE];
		if (t >= 0 && t < n) q[C4R_PAT_TYPE] = idmap[t];
		else if (t == C4R_PTYPE_DSYM && q[C4R_PAT_VALUE] >= 0 && q[C4R_PAT_VALUE] < n)
			q[C4R_PAT_VALUE] = idmap[q[C4R_PAT_VALUE]];
		q = q + C4R_PAT__Sz; ++i;
	}
	free(idmap);

	if (master[C4R_SRCMAP] && gc_srcmap(master, rs, newpos, nreg)) {
		printf("c4rlink: warning: out of memory for the source map; the image has none\n");
		free((char *)master[C4R_SRCMAP]);
		master[C4R_SRCMAP] = 0;
		master[C4R_SRCMAPLEN] = 0;
	}
	if (cl_verbose)
		printf("c4rlink: --gc: kept %d of %d functions, %d of %d words of code\n", kfun, nfun, words, codelen);
	free(rs); free(newpos); free(gc_live);
	return 0;
}

// Serialize a c4r structure to a file. Layout must match c4r_load_opt_real
// in load-c4r.c. Returns 0 on success.
static int c4r_write (int *c4r, char *file) {
	int  fd, i, n, tmp, vfsput;
	int *hdr, *p, *sym;
	char version, wordbits, tmpc;

	// Writing under the VM. There is no write() syscall on c4/c4m, which
	// is why this used to refuse outright -- and why C4IX could not be
	// linked on the machine that runs it. It does not need one: the
	// image is rendered into memory through the asmc4r_use_mem path this
	// file already links (writechecked, asm-c4r.c), then handed to
	// whoever can store it. Under C4KE that is the kernel's RAM
	// filesystem (OP_VFS_PUT); under C4DOS it is the RAM disk. The
	// pattern is asmc4r_Source's, deliberately -- c4cc already writes
	// its objects exactly this way, and this is the consumer of them.
	fd = 0 - 1;
	vfsput = 0;
	if (is_c4()) {
#if !NATIVE
		// Probe only when a trap handler is installed, i.e. a kernel is
		// servicing opcodes; 128 is what a missed trap leaves behind.
		if (__c4_info() & C4I_TRAPH)
			vfsput = __c4_opcode("OP_VFS_PUT", 128); // 128 = OP_REQUEST_SYMBOL
		if (vfsput <= 128 && !dos_can_write()) {
			printf("c4rlink: cannot write '%s' -- no kernel RAM filesystem and no C4DOS RAM disk\n", file);
			return 1;
		}
		asmc4r_use_mem = 1;
		asmc4r_memlen = 0;
#endif
	}

	hdr = (int *)c4r[C4R_HEADER];
	if (!asmc4r_use_mem)
	if ((fd = open(file, O_TRUNC | O_WRONLY | O_CREAT, S_IRUSR | S_IWUSR)) < 0) {
		printf("c4rlink: failed to open '%s' for writing\n", file);
		return 1;
	}
	writeoffset = 0;

	// Header
	writechecked(fd, "C4R", 3);
	version  = hdr[C4R_HDR_VERSION];  writechecked(fd, &version, 1);
	wordbits = hdr[C4R_HDR_WORDBITS]; writechecked(fd, &wordbits, 1);
	// The padding word (byte offset 5) carries MEMSZ in format v3: the
	// data segment's total in-memory size, the excess over DATALEN
	// being zero-filled BSS (see load-c4r.c). One word, then 'p' filler
	// for any remaining padding bytes (only when int < 8 bytes wide).
	i = sizeof(int) - 1;
	tmp = (5 + i) & ~i;
	writechecked(fd, &hdr[C4R_HDR_MEMSZ], sizeof(int));
	tmp = tmp - sizeof(int);
	while (tmp-- > 0) writechecked(fd, "p", 1);
	tmp = hdr[C4R_HDR_ENTRY];        writechecked(fd, &tmp, sizeof(int));
	tmp = hdr[C4R_HDR_CODELEN];      writechecked(fd, &tmp, sizeof(int));
	tmp = hdr[C4R_HDR_DATALEN];      writechecked(fd, &tmp, sizeof(int));
	tmp = hdr[C4R_HDR_PATCHLEN];     writechecked(fd, &tmp, sizeof(int));
	tmp = cl_strip ? 0 : hdr[C4R_HDR_SYMBOLSLEN]; writechecked(fd, &tmp, sizeof(int));
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
	n = cl_strip ? 0 : hdr[C4R_HDR_SYMBOLSLEN];
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
	// The extension trailer (docs/c4-explorer.md 2.1): 'X', the SMAP
	// chunk's tag, length and payload padded to a word, then a 0 tag.
	// link_srcmap made the map, or there is none and nothing follows S.
	if (c4r[C4R_SRCMAP]) {
		writechecked(fd, "X\0\0\0\0\0\0\0", sizeof(int));
		tmp = c4r__charword('S', 'M', 'A', 'P'); writechecked(fd, &tmp, sizeof(int));
		n = c4r[C4R_SRCMAPLEN];                writechecked(fd, &n, sizeof(int));
		writechecked(fd, (char *)c4r[C4R_SRCMAP], n);
		n = (sizeof(int) - n % sizeof(int)) % sizeof(int);
		if (n) writechecked(fd, "\0\0\0\0\0\0\0\0", n);
		tmp = 0;                               writechecked(fd, &tmp, sizeof(int));
	}

	if (asmc4r_use_mem) {
		// Clear the flag FIRST: it is a global shared with c4cc's writer
		// and every path out of here has to leave it off.
		asmc4r_use_mem = 0;
#if !NATIVE
		if (vfsput > 128) {
			if (__c4_opcode(asmc4r_memlen, asmc4r_membuf, file, vfsput)) {
				printf("c4rlink: unable to store '%s' in the RAM filesystem\n", file);
				return 1;
			}
			printf("c4rlink: wrote %d bytes to ramfs:%s\n", asmc4r_memlen, file);
			return 0;
		}
		if (dos_put(file, asmc4r_membuf, asmc4r_memlen) < 0) {
			printf("c4rlink: unable to store '%s' on the RAM disk\n", file);
			return 1;
		}
		printf("c4rlink: wrote %d bytes to ram:%s\n", asmc4r_memlen, file);
#endif
		return 0;
	}

	close(fd);
	return 0;
}

// Load one .c4r with its symbols: from the kernel's RAM filesystem when
// there is one, else C4DOS's RAM disk, else the host. 0 if it cannot be.
static int *cl_load (char *name) {
	int  *loaded;
	char *vfsbuf;    // kernel RAM filesystem contents, when there are any
	int   vfslen, vfsget, opts;
	loaded = 0;
	// the source map too, except for c4rstrip, which drops it
	opts = C4ROPT_SYMBOLS | C4ROPT_SRCMAP;
	if (cl_striptool) opts = C4ROPT_SYMBOLS;
#if !NATIVE
	// Resolved by name, the way asm-c4r.c resolves OP_VFS_PUT,
	// rather than through u0.h -- c4rlink's own build does not
	// include u0.h (C4R_C4CC_SRCS expands $(U0) before it is
	// defined), and load-c4r.c already supplies C4I_TRAPH.
	// Probe only with a trap handler installed; 128 is what a
	// missed trap leaves in the accumulator.
	vfsget = 0;
	vfsbuf = 0;
	if (__c4_info() & C4I_TRAPH)
		vfsget = __c4_opcode("OP_VFS_GET", 128); // 128 = OP_REQUEST_SYMBOL
	if (vfsget > 128)
		vfsbuf = (char *)__c4_opcode(&vfslen, name, vfsget);
	if (vfsbuf)
		loaded = c4r_load_mem(name, vfsbuf, vfslen, opts);
	// And the same thing one rung down. Under C4DOS the objects
	// a compiler just wrote are on the RAM disk, which a
	// transient's own open() cannot see -- the identical
	// problem the C4KE branch above solves, with DOS's opener
	// in place of the kernel's.
	if (!loaded && dos_readable()) {
		if (!(vfsbuf = malloc(CL_DOSMAX))) return 0;
		if ((vfslen = dos_slurp(name, vfsbuf, CL_DOSMAX)) > 0)
			loaded = c4r_load_mem(name, vfsbuf, vfslen, opts);
		if (!loaded) free(vfsbuf);
	}
#endif
	if (!loaded) loaded = c4r_load_opt(name, opts);
	// A damaged or future image loads partway, and linking what is
	// there would crash or write nonsense
	if (loaded && !loaded[C4R_LOADCOMPLETE]) {
		printf("c4rlink: '%s' is not a complete C4R image\n", name);
		c4r_free(loaded);
		loaded = 0;
	}
	return loaded;
}

// ---- Library members (--start-lib ... --end-lib) ----
//
// An input after --start-lib (up to --end-lib, or the end) is a library
// member, as an object in an ar archive is to ld: it is linked only if
// it defines a symbol that something linked references and nothing
// linked defines, and that is repeated until nothing more comes in. As
// lld's --start-lib, not ld's archives, the order does not matter: a
// member is wanted by inputs named after it as much as before it, and
// by members already pulled in. "References" means a patch names the
// symbol -- an object's table also lists, as externs, every function
// its headers declared, used or not. A member pulled in is linked
// whole; members left out are as if not named. With no --start-lib,
// every input is linked, and the image is the same bytes as before.

// The names seen: a hash of chains. Per name the bytes, their length,
// NF_DEF (something linked defines it) and NF_REF (something linked
// names it in a patch, as an extern), and the next in the chain.
enum { NE_NAME, NE_LEN, NE_FLAGS, NE_NEXT, NE__Sz };
enum { NF_DEF = 1, NF_REF = 2, NH_BUCKETS = 4096 };
int *cl_nh, *cl_ne, cl_nen, cl_nemax;

// The entry for NAME, made if ADD and there is none; 0 if none (or no room)
static int *nh_find (char *name, int len, int add) {
	int h, i, k, *e;
	h = 0; i = 0;
	while (i < len) { h = (h * 31 + (name[i] & 255)) & (NH_BUCKETS - 1); ++i; }
	k = cl_nh[h];
	while (k >= 0) {
		e = cl_ne + k * NE__Sz;
		if (e[NE_LEN] == len && !memcmp((char *)e[NE_NAME], name, len)) return e;
		k = e[NE_NEXT];
	}
	if (!add || cl_nen >= cl_nemax) return 0;
	e = cl_ne + cl_nen * NE__Sz;
	e[NE_NAME] = (int)name; e[NE_LEN] = len; e[NE_FLAGS] = 0;
	e[NE_NEXT] = cl_nh[h];
	cl_nh[h] = cl_nen++;
	return e;
}

// Does symbol SYM define its name for another module: not extern, not
// static, a function or a global (what resolve_patches can resolve)?
static int lib_def (int *sym) {
	return !(sym[C4R_SYMB_ATTRS] & (ATTR_EXTERN | ATTR_STATIC)) &&
	       (sym[C4R_SYMB_CLASS] == C4R_SCLASS_Fun || sym[C4R_SYMB_CLASS] == C4R_SCLASS_Glo);
}

// An input is linked: note what it defines and which externs it names.
// Returns 0, or 1 when out of memory.
static int lib_add (int *cl) {
	int *c4r, *hdr, *sym, *p, *refd, *e;
	int  i, n, id, t, maxid;
	c4r = (int *)cl[CL_STRUCT];
	hdr = (int *)cl[CL_HEADER];
	n = hdr[C4R_HDR_SYMBOLSLEN];
	maxid = -1;
	sym = (int *)c4r[C4R_SYMBOLS];
	i = 0;
	while (i < n) {
		if (sym[C4R_SYMB_ID] > maxid) maxid = sym[C4R_SYMB_ID];
		sym = sym + C4R_SYMB__Sz; ++i;
	}
	if (maxid < 0) return 0;
	if (!(refd = malloc(sizeof(int) * (maxid + 1)))) return 1;
	memset(refd, 0, sizeof(int) * (maxid + 1));
	p = (int *)c4r[C4R_PATCHES];
	i = 0;
	while (i < hdr[C4R_HDR_PATCHLEN]) {
		t = p[C4R_PAT_TYPE];
		id = -1;
		if (t >= 0) id = t;
		else if (t == C4R_PTYPE_DSYM) id = p[C4R_PAT_VALUE];
		if (id >= 0 && id <= maxid) refd[id] = 1;
		p = p + C4R_PAT__Sz; ++i;
	}
	sym = (int *)c4r[C4R_SYMBOLS];
	i = 0;
	while (i < n) {
		e = 0;
		if (lib_def(sym)) {
			if (!(e = nh_find((char *)sym[C4R_SYMB_NAME], sym[C4R_SYMB_NAMELEN], 1))) { free(refd); return 1; }
			e[NE_FLAGS] = e[NE_FLAGS] | NF_DEF;
		} else if ((sym[C4R_SYMB_ATTRS] & ATTR_EXTERN) && !(sym[C4R_SYMB_ATTRS] & ATTR_STATIC) &&
		           sym[C4R_SYMB_ID] >= 0 && refd[sym[C4R_SYMB_ID]]) {
			if (!(e = nh_find((char *)sym[C4R_SYMB_NAME], sym[C4R_SYMB_NAMELEN], 1))) { free(refd); return 1; }
			e[NE_FLAGS] = e[NE_FLAGS] | NF_REF;
		}
		sym = sym + C4R_SYMB__Sz; ++i;
	}
	free(refd);
	return 0;
}

// The symbol of member CL that something linked wants, or 0
static int *lib_wanted (int *cl) {
	int *c4r, *hdr, *sym, *e, i, n;
	c4r = (int *)cl[CL_STRUCT];
	hdr = (int *)cl[CL_HEADER];
	n = hdr[C4R_HDR_SYMBOLSLEN];
	sym = (int *)c4r[C4R_SYMBOLS];
	i = 0;
	while (i < n) {
		if (lib_def(sym) && (e = nh_find((char *)sym[C4R_SYMB_NAME], sym[C4R_SYMB_NAMELEN], 0)) &&
		    (e[NE_FLAGS] & (NF_DEF | NF_REF)) == NF_REF)
			return sym;
		sym = sym + C4R_SYMB__Sz; ++i;
	}
	return 0;
}

// Choose the members to link, to a fixed point, and drop the others from
// C4RS (freeing them), the rest keeping their order. NSYM is the inputs'
// symbols summed, which bounds the names. Returns the inputs left, or -1
// when out of memory.
static int lib_select (int *c4rs, int count, int nsym) {
	int *cl, *dst, *sym, k, more, left;
	k = 0; more = 0;
	while (k < count) { if (c4rs[k * CL__Sz + CL_MEMBER]) more = 1; ++k; }
	if (!more) return count;
	cl_nemax = nsym; cl_nen = 0;
	if (!(cl_nh = malloc(sizeof(int) * NH_BUCKETS))) return -1;
	memset(cl_nh, -1, sizeof(int) * NH_BUCKETS);
	if (!(cl_ne = malloc(sizeof(int) * NE__Sz * (nsym + 1)))) { free(cl_nh); return -1; }
	k = 0;
	while (k < count) {
		cl = c4rs + k * CL__Sz;
		if (!cl[CL_MEMBER] && lib_add(cl)) return -1;
		++k;
	}
	// CL_MEMBER: 1 still out, 2 pulled in
	more = 1;
	while (more) {
		more = 0;
		k = 0;
		while (k < count) {
			cl = c4rs + k * CL__Sz;
			if (cl[CL_MEMBER] == 1 && (sym = lib_wanted(cl))) {
				if (cl_verbose)
					printf("c4rlink: '%.*s' brings in '%s'\n",
					       sym[C4R_SYMB_NAMELEN], (char *)sym[C4R_SYMB_NAME], (char *)cl[CL_FILE]);
				cl[CL_MEMBER] = 2;
				if (lib_add(cl)) return -1;
				more = 1;
			}
			++k;
		}
	}
	free(cl_nh); free(cl_ne);
	// keep the linked, in order
	left = 0; dst = c4rs;
	k = 0;
	while (k < count) {
		cl = c4rs + k * CL__Sz;
		if (cl[CL_MEMBER] == 1) {
			if (cl_verbose) printf("c4rlink: '%s' is not needed\n", (char *)cl[CL_FILE]);
			c4r_free((int *)cl[CL_STRUCT]);
		} else {
			if (dst != cl) memcpy(dst, cl, sizeof(int) * CL__Sz);
			dst[CL_ID] = left;
			dst = dst + CL__Sz; ++left;
		}
		++k;
	}
	memset(dst, 0, sizeof(int) * CL__Sz * (count - left));
	return left;
}

int main (int argc, char **argv) {
	int   i, endargs, endopt, failed, unresolved, inlib;
	int  *master, *loaded;
	int  *c4rs, *c4rs_it, c4r_alloc, c4r_pos;
	char *spec, *arg, *outfile;
	int  *hdr;
	int   count_code, count_data, count_patch, count_con, count_des, count_sym;
	int   dstored, k;
	char *dbytes;

	// @file arguments first, so an object list written by a build can
	// stand in for a command line C4DOS cannot hold (asm-c4r.c).
	if (!(argv = respfile_expand(argc, argv, &argc))) return 1;

	// Set defaults
	spec = argv[0];
	outfile = "a.c4r";
	c4r_alloc = 256; // should be enough for everyone
	c4r_pos = 0;
	cl_debug = cl_verbose = cl_libmode = cl_strip = cl_nomap = cl_gc = 0;
	// The strip tool is this program under another name (argv[0] ends in
	// "c4rstrip", with or without .c4r): one input, strip on.
	cl_striptool = 0;
	arg = spec; while (*arg) ++arg;
	while (arg > spec && *(arg - 1) != '/') --arg;
	if (!my_strncmp(arg, "c4rstrip", 8)) { cl_striptool = 1; cl_strip = 1; }
	count_code = count_data = count_patch = count_con = count_des = count_sym = 0;
	cl_nlibs = 0;
	if (!(cl_libs = malloc(CL_LIBMAX * CLL__Sz * sizeof(int)))) return 1;
	endargs = 0;
	inlib = 0;
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
		} else if (endargs == 0 && !my_strcmp(*argv, "--start-lib")) {
			inlib = 1;
		} else if (endargs == 0 && !my_strcmp(*argv, "--end-lib")) {
			inlib = 0;
		} else if (endargs == 0 && !my_strcmp(*argv, "--gc")) {
			cl_gc = 1;
		} else if (endargs == 0 && **argv == '-') {
			arg = *argv + 1; // skip dash
			endopt = 0;
			while (!endopt && *arg) {
				if (*arg == 'd') cl_debug = 1;
				else if (*arg == 'v') cl_verbose = 1;
				else if (*arg == 'r') cl_libmode = 1;
				else if (*arg == 's') cl_strip = 1;
				else if (*arg == 'g') {
					// -g0: no source map; -g, the default, keeps it
					cl_nomap = 0;
					if (arg[1] == '0') { cl_nomap = 1; ++arg; }
				}
				else if (*arg == 'l') {
					endopt = 1;
					--argc; ++argv;
					if (!argc) { printf("%s: -l requires a library\n", spec); return 1; }
					if (cl_nlibs >= CL_LIBMAX) { printf("%s: at most %d -l libraries\n", spec, CL_LIBMAX); return 1; }
					if (!(loaded = cl_load(*argv))) {
						printf("%s: failed to load library '%s', aborting\n", spec, *argv);
						return 2;
					}
					// at run time it is /lib/ and its base name
					arg = *argv; while (*arg) ++arg;
					while (arg > *argv && *(arg - 1) != '/') --arg;
					i = 0; while (arg[i]) ++i;
					cl_libs[cl_nlibs * CLL__Sz + CLL_STRUCT] = (int)loaded;
					cl_libs[cl_nlibs * CLL__Sz + CLL_PATH] = (int)malloc(i + 6);
					memcpy((char *)cl_libs[cl_nlibs * CLL__Sz + CLL_PATH], "/lib/", 5);
					memcpy((char *)cl_libs[cl_nlibs * CLL__Sz + CLL_PATH] + 5, arg, i + 1);
					cl_libs[cl_nlibs * CLL__Sz + CLL_USED] = 0;
					++cl_nlibs;
					arg = "";
				}
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
			// The RAM filesystem shadows the host, the same way the
			// kernel's own loader does it (c4ke.c's task_loadc4r). This
			// is the consumer side of the write path above: a .c4o that
			// c4lc or c4cc just produced under C4KE exists only in
			// memory, and without this c4rlink could not see the twelve
			// objects C4IX is linked from.
			if (!(loaded = cl_load(*argv))) {
				printf("%s: failed to load '%s', aborting\n", spec, *argv);
				return 2;
			}
			if (cl_verbose) c4r_dump_info(loaded);
			// Record the module; its place in the merged image is
			// worked out below, once the members are chosen
			c4rs_it[CL_ID] = c4r_pos;
			c4rs_it[CL_STRUCT] = (int)loaded;
			c4rs_it[CL_HEADER] = loaded[C4R_HEADER];
			c4rs_it[CL_FILE]   = (int)*argv;
			c4rs_it[CL_MEMBER] = inlib;
			hdr = (int *)c4rs_it[CL_HEADER];
			count_sym = count_sym + hdr[C4R_HDR_SYMBOLSLEN];
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

	// Library members: only those wanted stay
	if ((c4r_pos = lib_select(c4rs, c4r_pos, count_sym)) < 0) {
		printf("%s: out of memory choosing the library members\n", spec);
		return 4;
	}

	// Each module's position in the merged image, and the totals
	count_sym = 0;
	c4rs_it = c4rs; i = 0;
	while (i < c4r_pos) {
		c4rs_it[CL_OFFSET_CODE] = count_code;
		c4rs_it[CL_OFFSET_DATA] = count_data;
		hdr = (int *)c4rs_it[CL_HEADER];
		// The data cursor advances by each module's MEMSZ (its data
		// image PLUS its BSS), so the next module -- and every
		// relocated data reference in this one -- sits past this
		// module's BSS, and that BSS becomes a zero gap in the merged
		// image. Only DATALEN bytes are copied in (below); the gap
		// stays zero because the master data buffer is zeroed.
		count_code  = count_code  + hdr[C4R_HDR_CODELEN];
		count_data  = count_data  + hdr[C4R_HDR_MEMSZ];
		count_patch = count_patch + hdr[C4R_HDR_PATCHLEN];
		count_con   = count_con   + hdr[C4R_HDR_CONSTRUCTLEN];
		count_des   = count_des   + hdr[C4R_HDR_DESTRUCTLEN];
		count_sym   = count_sym   + hdr[C4R_HDR_SYMBOLSLEN];
		c4rs_it = c4rs_it + CL__Sz; ++i;
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
	// Zero the whole merged data region so inter-module BSS gaps (and
	// each module's own BSS) read as zero -- only DATALEN bytes per
	// module are copied over this.
	if (count_data) memset((char *)master[C4R_DATA], 0, count_data);
	// count_data is the total in-memory size = MEMSZ. DATALEN (the
	// stored/trimmed length) is computed after the merge, below.
	hdr[C4R_HDR_MEMSZ]   = count_data;
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
	// CL_LIBMAX spare entries: add_needed's __c4_needed symbols
	if (!(master[C4R_SYMBOLS] = (int)malloc((i = sizeof(int) * C4R_SYMB__Sz * (count_sym + CL_LIBMAX))))) {
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
		// Trim the merged data's trailing zeros for storage: DATALEN
		// (stored) shrinks to the last non-zero byte, MEMSZ keeps the
		// full size so the loader zero-fills the tail. Data-resident
		// patches land in the loader's zero-filled MEMSZ region, so
		// trimming placeholder zeros is safe. (The common win: a large
		// uninitialized global at the end of the last module drops out
		// of the image entirely.)
		// Before the trim, so data-resident patch slots that are now
		// zero can also fall off the end.
		if (add_needed(master)) { printf("%s: out of memory\n", spec); return 4; }
		if (cl_strip && hdr[C4R_HDR_SYMBOLSLEN] && !cl_striptool) {
			k = 0;
			while (k < cl_nlibs) { if (cl_libs[k * CLL__Sz + CLL_USED]) { cl_strip = 0; k = cl_nlibs; } ++k; }
			if (!cl_strip) printf("%s: warning: -s ignored: a program that uses a shared library keeps its symbols\n", spec);
		}
		// the objects' source maps, unless -s or -g0 dropped them (or
		// c4rstrip, which is -s)
		// --gc reads the map for where functions are, so it is made
		// for it, and dropped after when the image is not to have one
		if ((cl_gc || (!cl_strip && !cl_nomap)) && link_srcmap(master, c4rs, c4r_pos))
			printf("%s: warning: out of memory for the source map; the image has none\n", spec);
		if (cl_gc && !cl_striptool && gc_functions(master, c4rs, c4r_pos)) {
			printf("%s: out of memory for --gc\n", spec);
			return 4;
		}
		if ((cl_strip || cl_nomap) && master[C4R_SRCMAP]) {
			free((char *)master[C4R_SRCMAP]);
			master[C4R_SRCMAP] = 0;
			master[C4R_SRCMAPLEN] = 0;
		}
		blank_patch_targets(master);
		dstored = hdr[C4R_HDR_MEMSZ];
		dbytes = (char *)master[C4R_DATA];
		while (dstored > 0 && dbytes[dstored - 1] == 0) --dstored;
		hdr[C4R_HDR_DATALEN] = dstored;
		if (cl_striptool) {
			if (c4r_pos != 1) { printf("%s: strip one file at a time (%d given)\n", spec, c4r_pos); return 1; }
			if (!my_strcmp(outfile, "a.c4r")) outfile = (char *)c4rs[CL_FILE];
			printf("%s: %s: %d symbols dropped -> %s\n", spec, (char *)c4rs[CL_FILE], count_sym, outfile);
		} else if (cl_strip && cl_libmode)
			printf("%s: warning: -s with -r: a stripped library cannot be linked against\n", spec);
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
