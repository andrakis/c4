// C4R Link
//
// Link several .c4r files together, resolving external symbols.
// By convention, .c4r files intended to be linked use the extension .c4o or .c4l.
//
// Extensions:
//  - .c4r: C4 Relocatable, an executable file
//  - .c4o: C4R object, an intermediate object file with functions but requiring
//          additional files to produce an executable.
//  - .c4l: C4R library, to be linked with .c4o fis
//
// Invocation: c4rlink file1.c4r file2.c4r -o file3.c4r
// Compilation: ./c4r load-c4r.c c4cc.c asm-c4r.c c4rlink.c
//
// Notes:
//  - All code, data, and patches are copied. This includes unused/unreferenced functions,
//    as well as static functions and data.
//  - Symbols are merged, with any duplicates (that are not extern) causing an error.
//
// Steps:
//  1: Create empty .c4r structure, master
//  2: For each file given on command line:
//  3:   Load .c4r
//  4:   Add to loaded list
//  5:   Track total code / data size
//  6: Next
//  7: Resize master code and data structure using total size
//  8: For each loaded .c4r file:
//  9:   Merge loaded file into master
//       - save code position as offsetC; copy new code
//       - save data position as offsetD; copy new data
//       - Import symbols:
//         o If symbol not in table, add it. Update patch references to new id.
//         o If in symbol table:
//           > if was extern but now is provided, update symbol with
//             (adjusted) code or data address, and remove extern flag.
//             + Search for patches referencing this symbol and update with new id
//           > if was not extern, and not static, error?
//       - Update patches
//         o Code and data patches adjusted by offetC and D
//         o Attempt to resolve all symbol patches with current symbols
//           > If patch type > 0, check if extern. If not extern, change to
//             CODE/DATA patch.
//       - code position += code size
//       - data position += data size
// 10: Next
// 11: Check patches for unresolved external symbols
//    - Error if unresolved > 0 and not library mode
// 12: Write master .c4r file
//
// TODO: implement shared libraries in a similar manner, except that code and data
//       can be anywhere (not necessarily appended to another .c4r.)

#define NO_ASMC4R_MAIN 1
// Also includes load-c4r.c
#include "asm-c4r.c"

// Structure for holding .c4r structures
enum {
	CL_ID,           // int, id of this entry
	CL_STRUCT,       // int *, c4r structure
	CL_HEADER,       // int *, c4r header
	CL_FILE,         // char *, .c4r file name
	CL__Sz
};

enum {
	MODE_NONE = 0
};

static void show_help (char *spec) {
	printf("%s: Link multiple .c4r files into one\n"
	       "%s: [-dv] [-o outfile] [--] file1.c4r [...fileN.c4r]\n"
	       "     -d            Turn on debug mode\n"
	       "     -v            Turn on verbose mode\n"
	       "     -o outfile    Write to outfile (default: a.c4r)\n"
	       "     --            End arguments\n", spec, spec);
}

static int my_strcmp (char *s1, char *s2) { while(*s1 && (*s1 == *s2)) { ++s1; ++s2; } return *s1 - *s2; }

int merge_c4rs (int *master, int startCode, int startData, int startPatch, int *c4rs, int count) {
	int *c4rs_it, c4r_index, tempCode, tempData, i, pid;
	int  offsetCode, offsetData;
	int *hdr, *c4r, *srcPatch, *dstPatch;

	c4r_index = 0;
	offsetCode = startCode;
	offsetData = startData;
	c4rs_it = c4rs;
	dstPatch = (int *)master[C4R_PATCHES] + (startPatch * C4R_PAT__Sz);

	// Copy code, data, patches, constructors, destructors, and symbols.
	while(c4r_index < count) {
		if (!c4rs_it[CL_STRUCT]) {
			printf("merge_c4rs error: missing structure\n");
			return 1;
		}

		// Copy code and data
		hdr = (int *)c4rs_it[CL_HEADER];
		c4r = (int *)c4rs_it[CL_STRUCT];
		memcpy((int *)master[C4R_CODE] + offsetCode,
		       (int *)c4r[C4R_CODE], (tempCode = hdr[C4R_HDR_CODELEN] * sizeof(int)));
		memcpy((char *)master[C4R_DATA] + offsetData,
		       (char *)c4r[C4R_DATA], (tempData = hdr[C4R_HDR_DATALEN] * sizeof(char)));

		// Copy and update patches
		srcPatch = (int *)c4r[C4R_PATCHES];
		i = 0; while(i++ < hdr[C4R_HDR_PATCHLEN]) {
			pid = srcPatch[C4R_PAT_TYPE];
			dstPatch[C4R_PAT_TYPE] = pid;
			// Adjust address
			dstPatch[C4R_PAT_ADDRESS] = offsetCode + srcPatch[C4R_PAT_ADDRESS];
			// Adjust value
			if (pid == C4R_PTYPE_CODE) {
				dstPatch[C4R_PAT_VALUE] = offsetCode + srcPatch[C4R_PAT_VALUE];
			} else if(pid == C4R_PTYPE_DATA) {
				dstPatch[C4R_PAT_VALUE] = offsetData + srcPatch[C4R_PAT_VALUE];
			} else {
				// External symbol. Do nothing yet.
				// A loop below this will resolve all externals
			}
			srcPatch = srcPatch + C4R_PAT__Sz;
			dstPatch = dstPatch + C4R_PAT__Sz;
		}

		// Copy constructors (adjusted)
		// Copy destructors (adjusted)
		// Import symbols

		++c4r_index;
		c4rs_it = c4rs_it + CL__Sz;
	}

	// Now run through again to resolve external symbols
	// Each time an external symbol is found, all subsequent instances are updated.
	// This requires multiple runs through the patches table.
	// Might it be better instead to run through the smaller symbols table multiple times?
	return 0;
}

int main (int argc, char **argv) {
	int   i, endargs, endopt;
	int  *master, *loaded;
	int  *c4rs, *c4rs_it, c4r_alloc, c4r_pos;
	char *spec, *arg, *outfile;
	int   debug, verbose;
	int   entry, *hdr;
	char *entry_name;
	int   count_code, count_data, count_patch, count_con, count_des, count_sym;

	// Set defaults
	spec = argv[0];
	outfile = "a.c4r";
	c4r_alloc = 256; // should be enough for everyone
	c4r_pos = 0;
	debug = 1;
	verbose = 1;
	entry = -1;
	entry_name = "(none)";
	count_code = count_data = count_patch = count_con = count_des = count_sym = 0;
	endargs = 0;

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
				if (*arg == 'd') debug = 1;
				else if (*arg == 'v') verbose = 1;
				else if (*arg == 'o') {
					endopt = 1;
					// grab spec from next argv
					--argc; ++argv;
					outfile = *argv;
				} else {
					printf("%s: Unrecognised option: '%s' (specifically, '%c')\n", argv, *arg);
					show_help(spec);
					// TODO: goto cleanup
					return 1;
				}
				++arg;
			}
		} else {
			// .c4r to load
			if (c4r_pos > c4r_alloc) {
				printf("%s: too many files given, maximum %d allowed\n", c4r_alloc);
				// TODO: goto cleanup
				return 3;
			}
			if (verbose) printf("%s: loading '%s'...\n", spec, *argv);
			if (!(loaded = c4r_load_opt(*argv, C4ROPT_SYMBOLS))) {
				printf("%s: failed to load '%s', aborting\n", spec, *argv);
				// TODO: goto cleanup
				return 2;
			}
			if (verbose) { printf("%s: load success\n", spec); c4r_dump_info(loaded); }
			// Copy into loaded structure
			c4rs_it[CL_ID] = c4r_pos;
			c4rs_it[CL_STRUCT] = (int)loaded;
			c4rs_it[CL_HEADER] = loaded[C4R_HEADER];
			c4rs_it[CL_FILE]   = (int)*argv;
			hdr = (int *)c4rs_it[CL_HEADER];
			// Update entry point
			// Sanity check: if loaded module has an entry, but we already have an entry
			//               set, warn about using original entry
			if (hdr[C4R_HDR_ENTRY] != -1) {
				if (entry == -1) {
					entry = hdr[C4R_HDR_ENTRY];
					entry_name = *argv;
				} else {
					printf("%s: WARNING: ignoring entry point in '%s', using entry seen in '%s'\n",
					       spec, *argv, entry_name);
				}
			}
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

	if (verbose) {
		printf("%s: command parsing complete, %d c4r structures loaded\n",
		       spec, c4r_pos);
		printf("  : counts - code: %d  data: %d  patches:  %d  cons: %d  des: %d  syms: %d\n",
		       count_code, count_data, count_patch, count_con, count_des, count_sym);
		printf("  : entry at %s+%x\n", entry_name, entry);
	}

	// Allocate space for code and data
	if (verbose) printf("%s: allocating master structure...\n", spec);
	hdr = (int *)master[C4R_HEADER];
	if (!(master[C4R_CODE] = (int)malloc((i = sizeof(int) * count_code)))) {
		printf("%s: failed to allocate %d bytes for master code\n", spec, i);
		return 4;
	}
	hdr[C4R_HDR_CODELEN] = count_code;
	if (!(master[C4R_DATA] = (int)malloc((i = count_data)))) {
		printf("%s: failed to allocate %d bytes for master data\n", spec, i);
		return 5;
	}
	hdr[C4R_HDR_DATALEN] = count_code;
	if (!(master[C4R_PATCHES] = (int)malloc((i = sizeof(int) * C4R_PAT__Sz * count_patch)))) {
		printf("%s: failed to allocate %d bytes for master patches\n", spec, i);
		return 6;
	}
	hdr[C4R_HDR_PATCHLEN] = count_patch;
	if (!(master[C4R_CONSTRUCTORS] = (int)malloc((i = sizeof(int) * C4R_CNDE__Sz * count_con)))) {
		printf("%s: failed to allocate %d bytes for master constructors\n", spec, i);
		return 7;
	}
	hdr[C4R_HDR_CONSTRUCTLEN] = count_con;
	if (!(master[C4R_DESTRUCTORS] = (int)malloc((i = sizeof(int) * C4R_CNDE__Sz * count_des)))) {
		printf("%s: failed to allocate %d bytes for master destructors\n", spec, i);
		return 7;
	}
	hdr[C4R_HDR_DESTRUCTLEN] = count_des;

	// Merge code and data
	// Merge patches
	// Merge symbols
	if (!merge_c4rs(master, 0, 0, 0, c4rs, c4r_pos)) {
		printf("%s: link failure\n");
		// TODO: goto cleanup
	} else {
		// Write output file
		if (verbose) printf("%s: writing to '%s'...\n", spec, outfile);
	}

// cleanup:
	// Cleanup
	c4rs_it = c4rs; i = 0; while(i++ < c4r_pos) {
		if (c4rs_it[CL_STRUCT])
			c4r_free((int *)c4rs_it[CL_STRUCT]);
		c4rs_it[CL_ID] = c4rs_it[CL_STRUCT] = c4rs_it[CL_HEADER] = 0;
		c4rs_it = c4rs_it + CL__Sz;
	}
	free(c4rs);
	c4r_free(master);
	return 0;
}
