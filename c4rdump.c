// C4R Dump
//
// Dump the details of a .c4r file
//

#define NO_ASMC4R_MAIN 1
#define NO_LOADC4R_MAIN 1
#include "asm-c4r.c"

enum { CH_TAB = 9 };

void c4r_dump_code (int *c4r) {
	int *e, *le;
	int *hdr, codelen;
	int *x;
	hdr = (int *)c4r[C4R_HEADER];
	le = (int *)c4r[C4R_CODE];
	e = le + hdr[C4R_HDR_CODELEN] - 1;
	x = 0;
	printf("Disassemble code from 0x%x - 0x%x (%d instructions)\n", le, e, (int)(e - le));
	printf("Instructions @ 0x%x\n", c4cc_instructions);
	while (le < e) {
		printf("0x%x: %8.4s", x, &c4cc_instructions[*++le * 5]);
		if (*le <= ADJ) { ++x; printf(" %d\n", *++le); } else printf("\n");
		++x;
	}
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
	MODE_ALL = 0xFF
};

static void show_help (char *spec) {
	printf("%s: Dump the contents of a .c4r file for easy viewing\n"
	       "%s: [-aicdps] [--] file1.c4r [...fileN.c4r]\n"
	       "     -a      Show all below options\n"
	       "     -i      Show info (default)\n"
	       "     -c      Show code\n"
	       "     -d      Show data\n"
	       "     -p      Show patches\n"
	       "     -s      Show symbols\n"
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
				else {
					printf("Unrecognised option: '%s' (specifically, '%c')\n", argv, *arg);
					show_help(spec);
					return 1;
				}
				++arg;
			}
		} else {
			if ((c4r = c4r_load_opt(*argv, C4ROPT_SYMBOLS))) {
				if (!mode) mode = MODE_INFO;
				if (mode & MODE_INFO) c4r_dump_info(c4r);
				if (mode & MODE_CODE) c4r_dump_code(c4r);
				if (mode & MODE_DATA) c4r_dump_data(c4r);
				if (mode & MODE_PATCHES) c4r_dump_patches(c4r);
				if (mode & MODE_SYMBOLS) c4r_dump_symbols(c4r);
				c4r_free(c4r);
			} else printf("c4rdump: failed to open '%s'\n", *argv);
		}
		--argc; ++argv;
	}
	return 0;
}
