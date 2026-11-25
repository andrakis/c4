// C4R Dump
//
// Dump the details of a .c4r file
//

#define NO_ASMC4R_MAIN 1
#define NO_LOADC4R_MAIN 1
#include "asm-c4r.c"

enum { CH_TAB = 9 };

// Disassemble the code segment. c4cc leaves word 0 unused (it emits
// with *++e, as c4 does), while c4lc's code starts at word 0 with the
// first function's ENT -- so start at 0 when that word is an ENT. Opcodes
// that carry an operand: everything up to ADJ, JSRI/JSRS, and the fused
// LDL..IMMP and STL (docs/fused-opcodes.md). An opcode past the name
// table prints as ?N rather than indexing off its end.
enum { OP_LDL = 79, OP_IMMP = 84, OP_STL = 87 };
void c4r_dump_code (int *c4r) {
	int *base, *le, *e;
	int *hdr, nnames, op;
	hdr = (int *)c4r[C4R_HEADER];
	base = le = (int *)c4r[C4R_CODE];
	e = base + hdr[C4R_HDR_CODELEN];
	nnames = 0;
	while (c4cc_instructions[nnames * 5]) ++nnames;
	if (*le != ENT) ++le;
	printf("Disassemble code from 0x%x - 0x%x (%d words)\n", base, e, (int)(e - base));
	while (le < e) {
		printf("%6d: ", (int)(le - base));
		op = *le++;
		if (op >= 0 && op < nnames) printf("%8.4s", &c4cc_instructions[op * 5]);
		else printf("     ?%d", op);
		if (le < e && (op <= ADJ || op == JSRS || op == JSRI ||
		    (op >= OP_LDL && op <= OP_IMMP) || op == OP_STL)) printf(" %ld\n", *le++);
		else printf("\n");
	}
}

// What a program linked against shared libraries (c4rlink -l) needs at
// run time: each library (a __c4_needed symbol names its path in the
// data), and how many symbol references are still to be resolved.
void c4r_dump_dynamic (int *c4r) {
	int *hdr, *sym, *patch, i, n, refs;
	hdr = (int *)c4r[C4R_HEADER];
	sym = (int *)c4r[C4R_SYMBOLS];
	n = hdr[C4R_HDR_SYMBOLSLEN];
	i = 0;
	while (i < n) {
		if (sym[C4R_SYMB_NAMELEN] == 11 && !memcmp((char *)sym[C4R_SYMB_NAME], "__c4_needed", 11))
			printf("  Needs %s\n", (char *)c4r[C4R_DATA] + sym[C4R_SYMB_VALUE]);
		sym = sym + C4R_SYMB__Sz;
		++i;
	}
	patch = (int *)c4r[C4R_PATCHES];
	n = hdr[C4R_HDR_PATCHLEN];
	refs = 0;
	i = 0;
	while (i < n) { if (patch[C4R_PAT_TYPE] >= 0) ++refs; patch = patch + C4R_PAT__Sz; ++i; }
	if (refs) printf("  %d symbol references to resolve at link or load time\n", refs);
}

// The source map (docs/c4-explorer.md 2.2): bytes, with unsigned and
// signed LEB128 numbers. c4rd_at is the cursor into it.
int c4rd_at;
int c4rd_u (char *p) {
	int v, s, c;
	c = p[c4rd_at++] & 255;
	v = c & 127; s = 7;
	while (c & 128) { c = p[c4rd_at++] & 255; v = v | ((c & 127) << s); s = s + 7; }
	return v;
}
int c4rd_s (char *p) {
	int v, s, c;
	c = p[c4rd_at++] & 255;
	v = c & 127; s = 7;
	while (c & 128) { c = p[c4rd_at++] & 255; v = v | ((c & 127) << s); s = s + 7; }
	if ((c & 64) && s < sizeof(int) * 8) v = v | (-1 << s);
	return v;
}

// -l: the files the image was compiled from, its functions, and the rows
// that say which line each run of code came from
void c4r_dump_srcmap (int *c4r) {
	char *p, *name;
	int len, flags, n, i, nl, size, crc, start, words, file, line, off, x;
	p = (char *)c4r[C4R_SRCMAP];
	if (!p) { printf("No source map\n"); return; }
	len = c4r[C4R_SRCMAPLEN];
	printf("Source map: version %d, %d bytes\n", p[0] & 255, len);
	if ((p[0] & 255) != 1) return;
	flags = p[1] & 255;
	c4rd_at = 2;
	n = c4rd_u(p);
	printf("  %d files\n", n);
	i = 0;
	while (i < n && c4rd_at < len) {
		nl = c4rd_u(p); name = p + c4rd_at; c4rd_at = c4rd_at + nl;
		size = c4rd_u(p);
		if (flags & 1) {
			crc = (p[c4rd_at] & 255) | ((p[c4rd_at + 1] & 255) << 8) |
			      ((p[c4rd_at + 2] & 255) << 16) | ((p[c4rd_at + 3] & 255) << 24);
			c4rd_at = c4rd_at + 4;
			printf("    %d %.*s, %d bytes, crc %08x\n", i, nl, name, size, crc & 0xffffffff);
		} else printf("    %d %.*s, %d bytes\n", i, nl, name, size);
		++i;
	}
	n = c4rd_u(p);
	printf("  %d functions\n", n);
	i = 0; start = 0;
	while (i < n && c4rd_at < len) {
		start = start + c4rd_u(p);
		words = c4rd_u(p);
		nl = c4rd_u(p); name = p + c4rd_at; c4rd_at = c4rd_at + nl;
		file = c4rd_u(p);
		line = c4rd_u(p);
		printf("    %6d %5d %.*s, file %d line %d\n", start, words, nl, name, file, line);
		++i;
	}
	n = c4rd_u(p);
	printf("  %d rows\n", n);
	i = 0; off = 0; file = 0; line = 0;
	while (i < n && c4rd_at < len) {
		x = c4rd_u(p);
		off = off + (x >> 1);
		if (x & 1) file = c4rd_u(p);
		line = line + c4rd_s(p);
		printf("    %6d %d:%d\n", off, file, line);
		++i;
	}
	if (c4rd_at > len) printf("  (the map runs %d bytes past its end)\n", c4rd_at - len);
}

int c4r_data_is_printable (char c) {
	return c >= ' ' && c <= '~';
}

void c4r_dump_data (int *c4r) {
	int width, *hdr, i;
	char *start, *d, *ds, *de, *target;

	hdr = (int *)c4r[C4R_HEADER];
	width = 16; // sizeof(int) * 2;
	ds = start = (char *)c4r[C4R_DATA];
	de = ds;
	target = ds + hdr[C4R_HDR_DATALEN];
	printf("Data segment: %ld bytes\n", hdr[C4R_HDR_DATALEN]);
	while(de <= target) {
		// Print address
		// TODO: align properly
		printf("%08x: ", ds - start);
		// Print hex repre
		d = ds; i = 0; while(i++ < width) {
			printf("%02x", *d++);
			if (i % 2 == 0) printf(" ");
		}
		// Separator
		printf("  ");
		// Print ASCII repre
		d = ds;
		d = ds; i = 0; while(i++ < width) {
			if (c4r_data_is_printable(*d)) printf("%c", *d);
			else printf(".");
			++d;
		}
		printf("\n");
		ds = ds + width;
		de = de + width;
	}
}

enum {
	MODE_NONE = 0,
	MODE_INFO    = 0x1,
	MODE_CODE    = 0x2,
	MODE_DATA    = 0x4,
	MODE_PATCHES = 0x8,
	MODE_SYMBOLS = 0x10,
	MODE_SRCMAP  = 0x20,
	MODE_ALL = 0xFF
};

static void show_help (char *spec) {
	printf("%s: Dump the contents of a .c4r file for easy viewing\n"
	       "%s: [-aicdpsl] [--] file1.c4r [...fileN.c4r]\n"
	       "     -a      Show all below options\n"
	       "     -i      Show info (default)\n"
	       "     -c      Show code\n"
	       "     -d      Show data\n"
	       "     -p      Show patches\n"
	       "     -s      Show symbols\n"
	       "     -l      Show the source map (files, functions, rows)\n"
	       "     --      End arguments\n", spec, spec);
}

static int my_strcmp (char *s1, char *s2) { while(*s1 && (*s1 == *s2)) { ++s1; ++s2; } return *s1 - *s2; }

int main (int argc, char **argv) {
	int *c4r;
	int  mode, endargs;
	char *spec, *arg;

	mode = MODE_NONE;
	endargs = 0;
	spec = *argv;
	c4cc_init_instructions();

	// Skip argv[0]
	--argc; ++argv;
	if (!argc) {
		show_help(spec);
		return 1;
	}

	// Parse options and dump info about requested files
	while(argc) {
		if (!my_strcmp("--help", *argv)) {
			show_help(spec);
			return 1;
		} else if (!my_strcmp(*argv, "--")) {
			endargs = 1;
		} else if (endargs == 0 && **argv == '-') {
			arg = *argv + 1; // skip dash
			while (*arg) {
				if (*arg == 'a') mode = mode | MODE_ALL;
				else if (*arg == 'i') mode = mode | MODE_INFO;
				else if (*arg == 'c') mode = mode | MODE_CODE;
				else if (*arg == 'd') mode = mode | MODE_DATA;
				else if (*arg == 'p') mode = mode | MODE_PATCHES;
				else if (*arg == 's') mode = mode | MODE_SYMBOLS;
				else if (*arg == 'l') mode = mode | MODE_SRCMAP;
				else {
					printf("Unrecognised option: '%s' (specifically, '%c')\n", argv, *arg);
					show_help(spec);
					return 1;
				}
				++arg;
			}
		} else {
			c4r = c4r_load_opt(*argv, C4ROPT_SYMBOLS | C4ROPT_SRCMAP);
			// a damaged or future image loads partway: say so, rather
			// than dump what is not there
			if (c4r && !c4r[C4R_LOADCOMPLETE]) {
				printf("c4rdump: '%s' is not a complete C4R image\n", *argv);
				c4r_free(c4r);
			} else if (c4r) {
				if (!mode) mode = MODE_INFO;
				if (mode & MODE_INFO) { c4r_dump_info(c4r); c4r_dump_dynamic(c4r); }
				if (mode & MODE_CODE) c4r_dump_code(c4r);
				if (mode & MODE_DATA) c4r_dump_data(c4r);
				if (mode & MODE_PATCHES) c4r_dump_patches(c4r);
				if (mode & MODE_SYMBOLS) c4r_dump_symbols(c4r);
				if (mode & MODE_SRCMAP) c4r_dump_srcmap(c4r);
				c4r_free(c4r);
			} else printf("c4rdump: failed to open '%s'\n", *argv);
		}
		--argc; ++argv;
	}
	return 0;
}
