// C4 Relocatable assembler for C4 CC
//
// Compiles C4 to an object format that can be loaded for use in C4.
// Outputs to 'a.c4r' or file of your choosing using the -o outfile option.
//
// Supports multiple files on the commandline, they are all concatenated together.
//
//
// Invocation:
// Options must be specified before source files.
//   [-S] [-o outfile] [file1.c] [...fileN.c]
//
// Options:
//   -o outfile     Output to outfile
//   -S             Produce assembly listing
//
// Use natively: gcc -g src/c4cc/asm-c4r.c -o c4cc
//
//    ./c4cc src/tests/hello.c                                             (produces a.c4r)
//    ./c4cc -o hello.c4r src/tests/hello.c                                (produces hello.c4r)
//    ./c4cc -o top.c4r include/u0.h src/c4ke/bin/ps.c src/c4ke/bin/top.c  (produces top.c4r)
//    ./c4cc -S include/u0.h src/c4ke/bin/ps.c src/c4ke/bin/top.c          (outputs assembly listing to screen)
//                                                                         (outputs a.c4r since no -o given)
//
// Use inside C4KE or c4:
//    Can only usefully use the -S flag. No file output or redirection currently supported.
//    c4sh>
//    c4cc -S src/tests/hello.c
//    c4cc -S include/u0.h src/tests/mandel.c
//    c4cc -S include/u0.h src/c4ke/bin/ps.c src/c4ke/bin/top.c
//
// C4R File Format: Version 2 (with proposals for V3 not implemented)
// |---------------------------------------------------------------------------|
// | Header:                                                                   |
// | |-----------------------------------------------------------------------| |
// | | Type | Name           | Purpose                                       | |
// | |-----------------------------------------------------------------------| |
// | | B*3  | "C4R"          | Signature, 3 bytes                            | |
// | | B    | Version        | Currently 2                                   | |
// | | B    | WordBits       | How large a word is                           | |
// | | W    | Entry          | Position to begin code execution              | |
// | | W    | CodeLen        | Length of code segment in words               | |
// | | W    | DataLen        | Length of data segment in bytes               | |
// | | W    | PatchLen       | Length of patch segment in entries            | |
// | | W    | SymbolsLen     | Length of symbols segment in entries          | |
// | | W    | ConstructLen   | Length of construct segment in entries        | |
// | | W    | DestructLen    | Length of deconstruct segment in entries      | |
// | |-----------------------------------------------------------------------| |
// |---------------------------------------------------------------------------|
// | Each segment is preceded by a symbol to indicate where in the file you    |
// |  are when viewing. Use c4rdump for a more user-friendly way to view.      |
// |---------------------------------------------------------------------------|
// | Data (D) and code segments (C) are just direct words to load into memory. |
// |---------------------------------------------------------------------------|
// | Patch segment format: P                                                   |
// | |-----------------------------------------------------------------------| |
// | | Type | Name       | Purpose                                           | |
// | |-----------------------------------------------------------------------| |
// | | W  : Type       Type of patch segment, see LT_*                       | |
// | | W  : Address    Address, before adjusting to loadaddr, to be patched  | |
// | | W  : Value      Offset to add to patch address                        | |
// | | Note: Type can be negative (see LT_*) or positive to refer to a symbol| |
// | |       and resolved after linking.                                     | |
// | | Address is a word offset into CODE for types -1/-2 and symbols, and a | |
// | | BYTE offset into DATA for types -3/-4 (data-resident words: pointer   | |
// | | initializers of globals, switch jump tables). Code-resident patches   | |
// | | are written first, in emission (= ascending address) order, then the  | |
// | | data-resident ones.                                                   | |
// | |-----------------------------------------------------------------------| |
// |---------------------------------------------------------------------------|
// | Construct / Destruct segment format:                                      |
// | These run before a program starts (constructor) or after the program      |
// | returns from main (destructor.) Destructors not called if exit() used.    |
// | c and d are the segment markers.                                          |
// | |-----------------------------------------------------------------------| |
// | | Type | Name       | Purpose                                           | |
// | |-----------------------------------------------------------------------| |
// | |  W   | Value      | Code offset of function to call                   | |
// | | (A priority byte was proposed and never written: both c4cc and    | |
// | |  c4lc emit one word per entry, and load-c4r.c reads one.)         | |
// |---------------------------------------------------------------------------|
// | Symbols segment format: S                                                 |
// | |-----------------------------------------------------------------------| |
// | | Type | Name       | Purpose                                           | |
// | |-----------------------------------------------------------------------| |
// | |  W   | Id         | C4 symbol id                                      | |
// | |  W   | Type       | C4 type                                           | |
// | |  W   | Class      | C4 class                                          | |
// | |  W   | Attributes | Eg static, external, etc                          | |
// | |  B   | NameLen    |                                                   | |
// | |  B.. | Name       | NameLen bytes, not including nul terminator       | |
// | |  W   | Value      |                                                   | |
// | | Type and Class are WORDS, not bytes, and there is no trailing      | |
// | | Length -- checked against what c4cc and c4lc actually write and    | |
// | | against c4r.lisp, which reads it. Attribute bits in use: 0x1       | |
// | | constructor, 0x2 destructor, 0x8 static, 0x20 variadic, 0x40       | |
// | | aggregate (an array or a struct variable, whose name stands for    | |
// | | its own address).                                                  | |
// | |-----------------------------------------------------------------------| |
// |---------------------------------------------------------------------------|
// |---------------------------------------------------------------------------|
// | Unimplemented proposals: Version 3                                        |
// | Version 3 would include a method to map instructions to source code, as   |
// | well as a check to ensure the opcodes used at compile time match the ones |
// | being used at runtime.                                                    |
// | The following additional header content is proposed:                      |
// | |-----------------------------------------------------------------------| |
// | | Type | Name           | Purpose                                       | |
// | |-----------------------------------------------------------------------| |
// | | W    | CodePageLen    | V3 only: code pages                           | |
// | | W    | InstructionsLen| V3 only: length of instructions section       | |
// | | B*N  | Instructions   | V3 only: instruction section                  | |
// | | W    | SourceMapLen   | V3: source map to function address table      | |
// | |-----------------------------------------------------------------------| |
// | Source map format: M (V3 only)                                            |
// | |-----------------------------------------------------------------------| |
// | | Type | Name       | Purpose                                           | |
// | |-----------------------------------------------------------------------| |
// | |  W   | Start      | Start offset of entry                             | |
// | |  W   | End        | End offset of entry                               | |
// | |  B   | Length     | Length of the text of this entry                  | |
// | |  B.. | Text       | Source code text referencing code offset          | |
// | |-----------------------------------------------------------------------| |
// |---------------------------------------------------------------------------|

#define C4CC_INCLUDED
#include "c4cc.c"

enum { C4R__Exported_Version = 2 };

/// Globals

// Commandline options
char *asmc4r_opt_outfile;
int   asmc4r_opt_verify;


///
// Command-line parsing
// TODO: Move this to c4cc
///

int include_symbols, include_static;

int    asmc4r_argc, asmc4r_argv_needsfree;
char **asmc4r_argv;
int    asmc4r_opt_source,  // -S    generate source
       asmc4r_opt_pie;     // -pie  generate position independant code

int asmc4r_parse_commandline (int *_argc, char ***_argv) {
	int endopts, endopt;
	char *arg;
	int argc;
	char **argv;

	argc = *_argc;
	argv = *_argv;

	endopts = endopt = 0;
	while (argc > 1 && !endopts) {
		--argc; ++argv;
		arg = *argv;
		if (*arg == '-') {
			++arg;
			endopt = 0;
			while (*arg && !endopt) {
				     if (*arg == 'g') include_static = 1;
				else if (*arg == 'S') asmc4r_opt_source = 1;
				else if (*arg == 'o') {
					// Grab outfile from next argument
					--argc; ++argv;
					if (!argc) {
						printf("error: -o requires a filename\n");
						return -2;
					}
					asmc4r_opt_outfile = *argv;
					endopt = 1;
				}
				else if (*arg == 'p' && *(arg + 1) == 'i' && *(arg + 2) == 'e' && *(arg + 3) == 0) asmc4r_opt_pie = 1;
				else {
					printf("error: unrecognised option '%s'\n", *argv);
					return -2;
				}
				++arg;
			}
		} else {
			// Must be a file(s)
			endopts = 1;
		}
	}

	// Move back one
	++argc; --argv;

	if (argc == 1) {
		printf("usage: [-g] [-o outfile]\n");
		return 1;
	}

	// Update passed in arg values
	*_argc = argc;
	*_argv = argv;

	return 0;
}

#ifdef __c4__
// Stub out writing function
int write (int fd, void *buf, int count) {
	return 0;
}
int is_c4 () { return 1; }
// dummy out fflush and stdout
int fflush (int stream) { return 0; }
enum { stdin, stdout, stderr };
// dummy out open flags
enum { O_TRUNC, O_WRONLY, O_CREAT };
// mode flags
enum { S_IRWXU, S_IRUSR, S_IWUSR };
// stub out this function
int load_c4r (char *file) { return 0; }
void dump_c4r_info (int *c4r) { }
void free_c4r (int *c4r) { }
#else
#define is_c4() 0
#endif

int writeoffset;

// Memory-output mode: under C4KE the VM has no write syscall, so the
// image is rendered into this growing buffer and then stored in the
// kernel's RAM filesystem (see asmc4r_Source).
char *asmc4r_membuf;
int   asmc4r_memlen, asmc4r_memcap, asmc4r_use_mem;

int asmc4r_memwrite (char *buf, int count) {
	char *nb;
	int i;
	if (asmc4r_memlen + count > asmc4r_memcap) {
		asmc4r_memcap = asmc4r_memcap * 2 + count + 65536;
		if (!(nb = malloc(asmc4r_memcap))) {
			printf("c4cc: out of memory writing image\n");
			exit(-1);
		}
		i = 0;
		while (i < asmc4r_memlen) { nb[i] = asmc4r_membuf[i]; ++i; }
		if (asmc4r_membuf) free(asmc4r_membuf);
		asmc4r_membuf = nb;
	}
	i = 0;
	while (i < count) { asmc4r_membuf[asmc4r_memlen + i] = buf[i]; ++i; }
	asmc4r_memlen = asmc4r_memlen + count;
	return count;
}

int writechecked (int fd, void *buf, int count) {
	if (asmc4r_use_mem) {
		writeoffset = writeoffset + asmc4r_memwrite((char *)buf, count);
		return 0;
	}
	writeoffset = writeoffset + write(fd, buf, count);
}

// We include load-c4r only for the dump_c4r_info function so we can compare
// writing and reading.
#ifndef NO_LOADC4R_MAIN
#define NO_LOADC4R_MAIN 1
#endif
//#include "load-c4r.c"

int  *asmc4r_e, *asmc4r_le, *asmc4r_e_start;

// labels that get patched later
enum {
	LBL_TYPE,    // int, see LT_*
	LBL_INDEX,   // int, the code offset that needs to be patched
	LBL_VALUE,   // int, the value it needs to be patched to
	LBL__Sz,
	LABELS_MAX = 65536 // switch jump tables emit one label per entry
};

enum {
	LT_CODE  = -1,    // code-resident word -> code address
	LT_DATA  = -2,    // code-resident word -> data address
	LT_DCODE = -3,    // data-resident word -> code address
	LT_DDATA = -4,    // data-resident word -> data address
	LT_DROP  = -5     // Drop this patch (unused)
};

int *asmc4r_labels, asmc4r_labels_count;

int *asmc4r_newlabel (int index, int type) {
	int *lbl;
	lbl = asmc4r_labels + (LBL__Sz * asmc4r_labels_count++);
	if (asmc4r_labels_count > LABELS_MAX) {
		printf("asmc4r_newlabel: reached LABELS_MAX\n");
		exit(-1);
	}
	lbl[LBL_TYPE]  = type;
	lbl[LBL_INDEX] = (int)index;
	return lbl;
}

// LEA: a = (int)(bp + *pcval)
void asmc4r_handler_LEA (int pcval) {
	*++asmc4r_e = LEA; *++asmc4r_e = pcval;
}

enum { DEC_THRESHOLD = 1024 };
// IMM : a = *pc++;
// OISC: a = val
void asmc4r_handler_IMM (int val) {
	char *p;
	int  *lbl;
	p = (char *)val;
	*++asmc4r_e = IMM;
	*++asmc4r_e = val;
	if (p >= data_s && p < data) {
		val = val - (int)data_s;
		// Create data patch label
		lbl = asmc4r_newlabel(asmc4r_e - asmc4r_e_start, LT_DATA);
		lbl[LBL_VALUE] = val;
		// printf("Create patch label referencing data at offset: %d\n", val);
	} else if (val >= (int)asmc4r_e_start && val <= (int)asmc4r_e) {
		val = (int *)val - asmc4r_e_start;
		// Create code patch label
		lbl = asmc4r_newlabel(asmc4r_e - asmc4r_e_start, LT_CODE);
		lbl[LBL_VALUE] = val;
		// printf("Create patch label referencing code at offset: %d\n", val);
	}
}

// LI: a = *(int *)a
// LC: a = *(char *)a;
void asmc4r_handler_LI () { *++asmc4r_e = LI; }
void asmc4r_handler_LC () { *++asmc4r_e = LC; }
void asmc4r_handler_rewind_li () { --asmc4r_e; }
void asmc4r_handler_rewind_lc () { --asmc4r_e; }

// SI  : *(int *)*sp++ = a;
void asmc4r_handler_SI (int mode) { *++asmc4r_e = SI; }
void asmc4r_handler_SC (int mode) { *++asmc4r_e = SC; }

// PSH: *--sp = a;
void asmc4r_handler_PSH () { *++asmc4r_e = PSH; }

// JMP : pc = (int *)*pc;
// OISC: pc = loc
void asmc4r_handler_JMP (int loc) {
	int *lbl; // Label required to get correct code offset when loaded
	*++asmc4r_e = JMP;
	*++asmc4r_e = loc;
	lbl = asmc4r_newlabel(asmc4r_e - asmc4r_e_start, LT_CODE);
	lbl[LBL_VALUE] = (int)((int *)loc - asmc4r_e_start);
	//printf("//asmc4r_handler_JMP: created new label with value %lld, 0x%llx\n", loc, loc);
}
int *asmc4r_handler_JMPPH() {
	int *lbl;
	*++asmc4r_e = JMP;
	*++asmc4r_e = 0;
	lbl = asmc4r_newlabel(asmc4r_e - asmc4r_e_start, LT_CODE);
	return lbl;
}

// JSR : *--sp = (int)(pc + 1); pc = (int *)pc*; }
// OISC: *--sp = oisc4_e + INSTR_SIZE; PC = loc
void asmc4r_handler_JSR (int *d) {
	int *lbl; // Label required to get correct code offset when loaded
	int  loc, type;
	loc = d[emit_Val];
	type = LT_CODE;
	*++asmc4r_e = JSR;
	*++asmc4r_e = loc;
	if (d[Attr] & ATTR_EXTERN) {
		type = symbol_id(d);
		print_symbol(d);
		printf("\nasm-c4r: JSR to external symbol %d\n", type);
	}
	lbl = asmc4r_newlabel(asmc4r_e - asmc4r_e_start, type);
	lbl[LBL_VALUE] = (int)((int *)loc - asmc4r_e_start);
	// printf("***loc=0x%lX, base = 0x%lX, rel = %d 0x%lX\n", loc, asmc4r_e, lbl[LBL_VALUE], lbl[LBL_VALUE]);
}

// JSRI: *--sp = (int)(pc + 1); pc = (int *)*pc; pc = (int *)*pc
// OISC: --SP; *SP = PH:after;  pc = DEREFERENCE(DEREFERENCE(loc))
void asmc4r_handler_JSRI(int loc) {
	int *lbl; // Label required to get correct data offset when loaded
	loc = loc - (int)data_s; // Get correct offset in DATA
	*++asmc4r_e = JSRI;
	*++asmc4r_e = loc;
	//printf("asmc4r_handler_JSRI: loc %lld, in data? %c\n", loc, (loc >= (int)data_s && loc <= (int)data) ? 'y' : 'n');
	lbl = asmc4r_newlabel(asmc4r_e - asmc4r_e_start, LT_DATA);
	lbl[LBL_VALUE] = loc;
}
// *--sp = (int)(pc + 1); pc = (int *)*(bp + *pc++);
void asmc4r_handler_JSRS(int loc) {
	*++asmc4r_e = JSRS;
	*++asmc4r_e = loc;
}

// BZ  : pc = a ? (pc + 1) : (int *)*pc;
// OISC: if(a) pc = loc;
int *asmc4r_handler_BZPH() {
	int *lbl;
	*++asmc4r_e = BZ;
	*++asmc4r_e = 0;
	lbl = asmc4r_newlabel(asmc4r_e - asmc4r_e_start, LT_CODE);
	return lbl;
}
// BNZ : pc = a ? (int *)*pc : (pc + 1);
// OISC: if(!a) pc = loc;
int *asmc4r_handler_BNZPH() {
	int *lbl;
	*++asmc4r_e = BNZ;
	*++asmc4r_e = 0;
	lbl = asmc4r_newlabel(asmc4r_e - asmc4r_e_start, LT_CODE);
	return lbl;
}

// ADJ : sp = sp + *pc++
// OISC: SP + adj -> SP
void asmc4r_handler_ADJ(int adj) {
	*++asmc4r_e = ADJ;
	*++asmc4r_e = adj;
}

void asmc4r_handler_ENT(int adj) {
	*++asmc4r_e = ENT;
	*++asmc4r_e = adj;
}
// LEV : sp = bp; bp = (int *)*sp++; pc = (int *)sp++;
void asmc4r_handler_LEV() {
	*++asmc4r_e = LEV;
}

void asmc4r_handler_SYSCALL(int num, int argcount) {
	*++asmc4r_e = num;
}

void asmc4r_handler_MATH(int operation) {
	*++asmc4r_e = operation;
}

// Record a data-resident patch: the word at byte offset dataoff in the
// data segment is relocated at load time to a code (LT_DCODE) or data
// (LT_DDATA) target. Used for pointer initializers of globals and for
// switch jump tables, which live in data where nothing executes them.
void asmc4r_handler_DATAPATCH (int type, int dataoff, int value) {
	int *lbl;
	lbl = asmc4r_newlabel(dataoff, type);
	// DCODE values arrive as backend code addresses (only the backend
	// knows where its code buffer starts); DDATA values are already
	// byte offsets into the data pool.
	if (type == LT_DCODE) value = (int)((int *)value - asmc4r_e_start);
	lbl[LBL_VALUE] = value;
}

int *asmc4r_handler_FunctionAddress () { return asmc4r_e + 1; }
int *asmc4r_handler_CurrentAddress () { return asmc4r_e + 1; }
void asmc4r_handler_UpdateAddress (int *label, int addr) {
	addr = (int)((int *)addr - asmc4r_e_start);
	if (0)
		printf("Rewrite label %llx to %llx (old = %lld)\n",
		       (int)label, (int)addr, *label);
	label[LBL_VALUE] = addr;
}

void asmc4r_InSource_Line (int line, int length, char *s) {
	if (asmc4r_opt_source)
		printf("%d: %.*s", line, length, s);
}

void asmc4r_PrintAccumulated () {
	if (asmc4r_opt_source) {
        while (asmc4r_le < asmc4r_e) {
		  ++asmc4r_le;
			printf("%8.4s", &c4cc_instructions[*asmc4r_le * 5]);
		    if (*asmc4r_le <= ADJ || *asmc4r_le == JSRS || *asmc4r_le == JSRI) printf(" %ld\n", *++asmc4r_le); else printf("\n");
          //printf("%8.4s", &c4cc_instructions[*++asmc4r_le * 5]);
          //if (*asmc4r_le <= ADJ) printf(" %d\n", *++asmc4r_le); else printf("\n");
        }
	}
}

void asmc4r_FunctionStart (int *sym) {
	//printf("asmc4r: function start\n");
}
void asmc4r_FunctionEnd (int *sym) {
	//sym[emit_Length] = (int)(asmc4r_e - (int *)sym[emit_Val]);
	//sym[emit_Length] = ((((int *)sym[emit_Val]) - asmc4r_e_start)) - asmc4r_e;
	//printf("asmc4r: current emit value: 0x%X\n", asmc4r_e);
	//printf("asmc4r: function end at 0x%X, length = %ld\n", asmc4r_e, ffs);
}


int should_export (int *d) {
	if(d[Class] == Num) {
		// Skip exporting enums
		return 0;
	}

	if (!include_static && (d[Attr] & ATTR_STATIC)) {
		// Skip, not exporting static
		return 0;
	} 

	return 1;
}

// Print the byte representation of an integer
void asmc4r_dump_int (int val) {
	char *c;
	int   i, v;
	v = val;
	c = (char *)&v;
	i = 0;
	while(i++ < sizeof(int)) printf("%c", *c++);
}

// See table at start of file for segment format
void asmc4r_dump_symbol (int *d) {
	char *strc_a, *strc_b;

	// Find symbol name and length
	strc_a = strc_b = (char *) d[Name];
	while ((*strc_b >= 'a' && *strc_b <= 'z') ||
		   (*strc_b >= 'A' && *strc_b <= 'Z') ||
		   (*strc_b >= '0' && *strc_b <= '9') ||
		    *strc_b == '_') {
		++strc_b;
	}

	asmc4r_dump_int(d[Type]);      // Type
	asmc4r_dump_int(d[Class]);     // Class
	printf("%c", strc_b - strc_a); // NameLen
	printf("%.*s", strc_b - strc_a, strc_a); // Name
	// TODO: check type, function, etc, output adjusted value
	asmc4r_dump_int(d[Val]);       // Value
}

void asmc4r_dump_symbol_to_file (int fd, int *d, int id) {
	char *strc_a, *strc_b;
	char  tmp;
	int   class, value;

	// Find symbol name and length
	strc_a = strc_b = (char *) d[Name];
	while (strc_b && ((*strc_b >= 'a' && *strc_b <= 'z') ||
		   (*strc_b >= 'A' && *strc_b <= 'Z') ||
		   (*strc_b >= '0' && *strc_b <= '9') ||
		    *strc_b == '_')) {
		++strc_b;
	}

	// printf("symbol '%.*s' writing at offset 0x%lX\n", strc_b - strc_a, strc_a, writeoffset);
	writechecked(fd, &id, sizeof(int));        // Id
	writechecked(fd, &d[Type], sizeof(int));   // Type
	writechecked(fd, &d[Class], sizeof(int));  // Class
	writechecked(fd, &d[Attr], sizeof(int));   // Attributes
	tmp = strc_b - strc_a; writechecked(fd, &tmp, 1); // NameLen
	// TODO: symbols end up unaligned, need to write strings with padding.
	writechecked(fd, strc_a, strc_b - strc_a); // Name
	// Value, REBASED. A symbol's value has to mean something to whoever
	// reads the image back, and a compile-time host address means
	// nothing: functions were already emitted as an offset from the code
	// start, and globals now get the same treatment against the data
	// start. Leaving Glo raw is what made C4DOS's inject_api (which
	// computes database + value) write through a wild pointer -- the
	// bug hid because nothing had ever asked for a global by name.
	class = d[Class];
	value = d[Val];
	if (class == Fun) value = (int *)d[emit_Val] - asmc4r_e_start;
	else if (class == Glo) value = (char *)d[Val] - data_s;
	writechecked(fd, &value, sizeof(int));
	// Length
	//writechecked(fd, &d[emit_Length], sizeof(int));
}

void dump_to_file (char *file) {
	char version, wordbits;
	int *e, *lbl, i, t;
	int symbol_count, *d, offset;
	int constructor_count, destructor_count;
	int fd, tmp;
	char tmp_c;

	//printf("Writing output to '%s'...", file);
	fflush(stdout);

	fd = -1;
	if (!asmc4r_use_mem) {
		if ((fd = open(file, O_TRUNC | O_WRONLY | O_CREAT, S_IRUSR | S_IWUSR)) < 0) {
			printf("failed to open file\n");
			return;
		}
	}

	version = C4R__Exported_Version;
	wordbits = sizeof(int) * 8; // Could also do it in bytes

	// Calculate counts
	symbol_count = constructor_count = destructor_count = 0;
	d = idmain;
	while(d[Tk]) {
		if (d[Class]) {
			if (should_export(d)) ++symbol_count;
			if (d[Attr] & ATTR_CONSTRUCTOR) ++constructor_count;
			if (d[Attr] & ATTR_DESTRUCTOR) ++destructor_count;
		}
		d = d + Idsz;
	}

	// Header
	writechecked(fd, "C4R", 3);       // Signature
	writechecked(fd, &version, 1);    // Version
	writechecked(fd, &wordbits, 1);   // WordBits
	// Padding
	// var aligned = ((adj_ptr + (alignment - 1)) & ~(alignment - 1));
	tmp = ((5 + 3)) & ~(3);
	i = sizeof(int) - 1;
	tmp = (5 + i) & ~i;
	while (tmp--) writechecked(fd, "p", 1); // for padding
	tmp = idmain[emit_Val];
	if (tmp) {                        // Has a main()
		tmp = (int *)tmp - asmc4r_e_start;
	} else {
		tmp = -1;                     // Has no main
	}
	writechecked(fd, &tmp, sizeof(int)); // Entry
	// Code length
	tmp = 1 + (asmc4r_e - asmc4r_e_start); writechecked(fd, &tmp, sizeof(int));
	// Data length
	tmp = data - data_s; writechecked(fd, &tmp, sizeof(int));
	// Patches length
	tmp = asmc4r_labels_count; writechecked(fd, &tmp, sizeof(int));
	// Symbols length
	tmp = symbol_count; writechecked(fd, &tmp, sizeof(int));
	// Constructors length
	tmp = constructor_count; writechecked(fd, &tmp, sizeof(int));
	// Destructors length
	tmp = destructor_count; writechecked(fd, &tmp, sizeof(int));

	// Code
	writechecked(fd, "C\0\0\0\0\0\0\0", sizeof(int));
	// The inner parentheses matter when c4cc compiles itself: it types
	// 1 + pointer as int, which un-scales the following subtraction and
	// inflates the write to byte-count words. gcc computed it correctly,
	// so this only ever broke the self-hosted compiler's output.
	writechecked(fd, asmc4r_e_start, sizeof(int) * (1 + (asmc4r_e - asmc4r_e_start)));
	// printf("Wrote %d bytes of code\n", 1 + asmc4r_e - asmc4r_e_start);
	// Data
	writechecked(fd, "D\0\0\0\0\0\0\0", sizeof(int));
	writechecked(fd, data_s, data - data_s);
	// Resolve symbol-typed patches whose symbol was defined later in this
	// compilation (a JSR emitted while the target was only a prototype).
	// The loader ignores symbol patches, so leaving them made standalone
	// executables with forward declarations jump to garbage; only symbols
	// still undefined at the end of the unit -- true externs, for
	// c4rlink -- stay symbol-typed.
	i = 0; lbl = asmc4r_labels;
	while (i < asmc4r_labels_count) {
		if (lbl[LBL_TYPE] >= 0) {
			d = idstart + lbl[LBL_TYPE] * Idsz;
			// ATTR_EXTERN is the discriminator: a definition clears it,
			// and emit_Val alone cannot be trusted (prototypes carry a
			// nonzero garbage value there).
			if (d[Class] == Fun && d[emit_Val] && !(d[Attr] & ATTR_EXTERN)) {
				lbl[LBL_TYPE]  = LT_CODE;
				lbl[LBL_VALUE] = (int *)d[emit_Val] - asmc4r_e_start;
			}
		}
		++i;
		lbl = lbl + LBL__Sz;
	}

	// Patches: code-resident first in emission order (readers walk them
	// in step with the instruction stream), then data-resident.
	writechecked(fd, "P\0\0\0\0\0\0\0", sizeof(int));
	i = 0; lbl = asmc4r_labels;
	while (i < asmc4r_labels_count) {
		if (lbl[LBL_TYPE] >= LT_DATA || lbl[LBL_TYPE] >= 0) {
			writechecked(fd, &lbl[LBL_TYPE], sizeof(int));
			writechecked(fd, &lbl[LBL_INDEX], sizeof(int));
			writechecked(fd, &lbl[LBL_VALUE], sizeof(int));
		}
		++i;
		lbl = lbl + LBL__Sz;
	}
	i = 0; lbl = asmc4r_labels;
	while (i < asmc4r_labels_count) {
		if (lbl[LBL_TYPE] == LT_DCODE || lbl[LBL_TYPE] == LT_DDATA) {
			writechecked(fd, &lbl[LBL_TYPE], sizeof(int));
			writechecked(fd, &lbl[LBL_INDEX], sizeof(int));
			writechecked(fd, &lbl[LBL_VALUE], sizeof(int));
		}
		++i;
		lbl = lbl + LBL__Sz;
	}
	// printf("wrote %d records\n", i);
	// Constructors
	// printf("Writing constructors at 0x%lX\n", writeoffset);
	writechecked(fd, "c\0\0\0\0\0\0\0", sizeof(int));
	i = 0; d = idmain;
	while (d[Tk]) {
		if (d[Attr] & ATTR_CONSTRUCTOR) {
#if 0
			// Obtain priority
			// t = d[Attr] >> PRIORITY_SHIFT;
			// writechecked(fd, &t, sizeof(int));
#endif
			offset = ((int *)d[emit_Val]) - asmc4r_e_start;
			writechecked(fd, &offset, sizeof(int));
		}
		d = d + Idsz;
		++i;
	}
	// Destructors
	// printf("Writing destructors at 0x%lX\n", writeoffset);
	writechecked(fd, "d\0\0\0\0\0\0\0", sizeof(int));
	i = 0; d = idmain;
	while(d[Tk]) {
		if (d[Attr] & ATTR_DESTRUCTOR) {
#if 0
			// Obtain priority
			// t = d[Attr] >> PRIORITY_SHIFT;
			// writechecked(fd, &t, sizeof(int));
#endif
			offset = ((int *)d[emit_Val]) - asmc4r_e_start;
			writechecked(fd, &offset, sizeof(int));
		}
		d = d + Idsz;
	}
	// Symbols
	// printf("Writing %d symbols at 0x%lX...", symbol_count, writeoffset); fflush(stdout);
	writechecked(fd, "S\0\0\0\0\0\0\0", sizeof(int));
	i = 0; d = idstart;
	while (d[Tk]) {
		if (d[Class]) {
			if (should_export(d)) {
				// printf("%d ", i);
				asmc4r_dump_symbol_to_file(fd, d, i);
			}
		}
		d = d + Idsz;
		++i;
	}
	// printf("wrote %d records\n", i);
	
	//printf("success\n");
	if (fd >= 0) close(fd);
}

void read_from_file (char *file) {
	int flags, *module, *modules;

	if (!(module = c4r_load(file))) {
		printf("Unable to open\n");
		return;
	}

	c4r_dump_info(module);

	c4r_free(module);
}

// C4CC always runs in source mode.
// This callback occurs once all compilation is done.
// This is where the compiled and interpreted versions differ. See dump_to_file for the
// compiled version of this function.
// TODO: this function is outdated, and does not produce a valid c4r file.
void asmc4r_Source () {
	int version, wordbits;
	int *e, *lbl, i, t;
	int symbol_count, *d, total;
	int constructor_count, destructor_count;
	int offset;

	if (!is_c4()) {
		dump_to_file(asmc4r_opt_outfile);
		if (asmc4r_opt_verify)
			read_from_file(asmc4r_opt_outfile);
		return;
	}

#if !NATIVE
	// Under C4KE: render the image into memory and store it in the
	// kernel's RAM filesystem, from where the kernel can execute it.
	// (The VM has no write syscall, so the host filesystem is out.)
	// The opcode is resolved by name so this file needs nothing from u0.
	// Probe only when a trap handler is installed (C4I_TRAPH), i.e. a
	// kernel is servicing opcodes; an answer above 128 means it resolved
	// the symbol (128 is what a missed trap leaves in the accumulator).
	i = 0;
	if (__c4_info() & C4I_TRAPH)
		i = __c4_opcode("OP_VFS_PUT", 128); // 128 = OP_REQUEST_SYMBOL
	if (i > 128) {
		asmc4r_use_mem = 1;
		asmc4r_memlen = 0;
		dump_to_file(asmc4r_opt_outfile);
		asmc4r_use_mem = 0;
		if (__c4_opcode(asmc4r_memlen, asmc4r_membuf, asmc4r_opt_outfile, i))
			printf("c4cc: unable to store '%s' in the RAM filesystem\n",
			       asmc4r_opt_outfile);
		else
			printf("c4cc: wrote %d bytes to ramfs:%s\n",
			       asmc4r_memlen, asmc4r_opt_outfile);
		return;
	}

	// Under C4DOS: the same render-into-memory path, handed to the DOS
	// RAM disk through __c4dos_api instead of to a kernel's ramfs.
	// C4DOS is deliberately trap-free, so the probe above finds
	// nothing there -- a DOS cannot answer custom opcodes and is not
	// pretending to. dos_can_write() is false when the machine booted
	// without DEVICE=RAMDISK.SYS, and then this falls through to the
	// honest complaint below rather than losing the output quietly.
	if (dos_can_write()) {
		asmc4r_use_mem = 1;
		asmc4r_memlen = 0;
		dump_to_file(asmc4r_opt_outfile);
		asmc4r_use_mem = 0;
		if (dos_put(asmc4r_opt_outfile, asmc4r_membuf, asmc4r_memlen) < 0)
			printf("c4cc: unable to store '%s' on the RAM disk\n",
			       asmc4r_opt_outfile);
		else
			printf("c4cc: wrote %d bytes to ram:%s\n",
			       asmc4r_memlen, asmc4r_opt_outfile);
		return;
	}
#endif

	// TODO: this function is outdated, and does not produce a valid c4r file.
	printf("c4cc: output in C4KE not supported\n");
	return;

	version = C4R__Exported_Version;
	wordbits = sizeof(int) * 8; // Could also do it in bytes

	// Calculate counts
	symbol_count = constructor_count = destructor_count = 0;
	d = idmain;
	total = 0;
	while(d[Tk]) {
		if (d[Class]) {
			if (include_static || !(d[Attr] & ATTR_STATIC)) ++symbol_count;
			if (d[Attr] & ATTR_CONSTRUCTOR) ++constructor_count;
			if (d[Attr] & ATTR_DESTRUCTOR) ++destructor_count;
		}
		d = d + Idsz;
		++total;
	}

	// Header
	printf("C4R%c%c", version, wordbits);
	asmc4r_dump_int(1 + ((int *)idmain[emit_Val] - asmc4r_e_start));
	asmc4r_dump_int(asmc4r_e - asmc4r_e_start);
	asmc4r_dump_int(data - data_s);
	asmc4r_dump_int(asmc4r_labels_count);
	asmc4r_dump_int(symbol_count);
	asmc4r_dump_int(constructor_count);
	asmc4r_dump_int(destructor_count);

	// Code
	printf("C"); // Code segment
	e = asmc4r_e_start; while (e <= asmc4r_e) asmc4r_dump_int(*e++);
	// Data
	printf("D"); // Data segment
	e = (int *)data_s; while (e < (int *)data) asmc4r_dump_int(*e++);
	// Patches
	printf("P");
	i = 0; lbl = asmc4r_labels;
	while (i < asmc4r_labels_count) {
		printf("%c", lbl[LBL_TYPE]);
		asmc4r_dump_int(lbl[LBL_INDEX]);
		asmc4r_dump_int(lbl[LBL_VALUE]);
		++i;
		lbl = lbl + LBL__Sz;
	}
	// Symbols
	printf("S");
	i = 0; d = idmain;
	while (i++ < total) {
		// Don't export static symbols
		if (d[Type] && d[Class]) {
			if (!include_static && d[Attr] & ATTR_STATIC) {
				// Skip, not exporting static
			} else  {
				asmc4r_dump_symbol(d);
			}
		}
		d = d + Idsz;
	}
	// Constructors
	printf("c");
	i = 0; d = idmain;
	while (i++ < total) {
		if (d[Attr] & ATTR_CONSTRUCTOR) {
#if 0
			// Obtain priority
			// t = d[Attr] >> PRIORITY_SHIFT;
			// asmc4r_dump_int(t);
#endif
			offset = ((int *)d[Val]) - asmc4r_e_start;
			asmc4r_dump_int(offset);
		}
		d = d + Idsz;
	}
	// Destructors
	printf("d");
	i = 0; d = idmain;
	while (i++ < total) {
		if (d[Attr] & ATTR_DESTRUCTOR) {
#if 0
			// Obtain priority
			// t = d[Attr] >> PRIORITY_SHIFT;
			// asmc4r_dump_int(t);
#endif
			offset = ((int *)d[Val]) - asmc4r_e_start;
			asmc4r_dump_int(offset);
		}
		d = d + Idsz;
	}
}

// -- response files ---------------------------------------------------
//
// An argument of the form @NAME is replaced by the whitespace-separated
// words in the file NAME. This is the DOS-era answer to a command line
// that will not hold the command, and it is here for exactly that
// reason: C4DOS keeps fifteen tokens of a line (ARGVMAX, c4dos.c:72,
// and parse_line stops at ARGVMAX - 1), while linking C4IX needs
//
//     RUN c4rlink.c4r <twelve objects> -o c4ix.c4r
//
// which is sixteen. With this, the same link is four tokens and the
// object list lives in a file the build wrote. See
// docs/compiler-on-the-board.md.
//
// The file is found the way every input here is found: ask C4DOS
// first (its opener sees the RAM disk, so a list another tool just
// wrote is findable), then the C4KE RAM filesystem, then the host.
// '#' begins a comment that runs to the end of the line, so a generated
// list can say what it is.
enum { RESPF_MAX = 4096, RESPF_BYTES = 65536 };

int respf_isspace (int c) { return c == ' ' || c == 9 || c == 10 || c == 13; }

char **respfile_expand (int argc, char **argv, int *outargc) {
	int    i, n, cap, got, fd, vfsget, vfslen;
	char  *buf, *p, *vfsbuf;
	char **out;

	i = 0; n = 0;
	while (i < argc) { if (argv[i] && *argv[i] == '@' && argv[i][1]) ++n; ++i; }
	if (!n) { *outargc = argc; return argv; }

	cap = RESPF_MAX;
	if (!(out = malloc((cap + 1) * sizeof(char *)))) {
		printf("response file: out of memory\n");
		return 0;
	}
	n = 0;
	i = 0;
	while (i < argc) {
		if (!(argv[i] && *argv[i] == '@' && argv[i][1])) {
			if (n >= cap) { printf("response file: more than %d arguments\n", cap); return 0; }
			out[n++] = argv[i];
			++i;
			continue;
		}
		if (!(buf = malloc(RESPF_BYTES))) {
			printf("response file: out of memory\n");
			return 0;
		}
		got = dos_readable() ? dos_slurp(argv[i] + 1, buf, RESPF_BYTES - 1) : -1;
		if (got < 0) {
			vfsget = 0;
			vfsbuf = 0;
#if !NATIVE
			if (__c4_info() & C4I_TRAPH)
				vfsget = __c4_opcode("OP_VFS_GET", 128); // 128 = OP_REQUEST_SYMBOL
			if (vfsget > 128)
				vfsbuf = (char *)__c4_opcode(&vfslen, argv[i] + 1, vfsget);
#endif
			if (vfsbuf) {
				got = vfslen;
				if (got > RESPF_BYTES - 1) got = RESPF_BYTES - 1;
				memcpy(buf, vfsbuf, got);
			}
			else {
				if ((fd = open(argv[i] + 1, 0)) < 0) {
					printf("response file: cannot open '%s'\n", argv[i] + 1);
					return 0;
				}
				got = read(fd, buf, RESPF_BYTES - 1);
				close(fd);
				if (got < 0) got = 0;
			}
		}
		buf[got] = 0;
		p = buf;
		while (*p) {
			while (*p && respf_isspace(*p)) ++p;
			if (!*p) break;
			if (*p == '#') { while (*p && *p != 10) ++p; continue; }
			if (n >= cap) { printf("response file: more than %d arguments\n", cap); return 0; }
			out[n++] = p;
			while (*p && !respf_isspace(*p)) ++p;
			if (*p) { *p = 0; ++p; }
		}
		++i;
	}
	out[n] = 0;
	*outargc = n;
	return out;
}

int asmc4r_main (int argc, char **argv) {
	int poolsz, result, i;
	char *arg;

	// Defaults
	asmc4r_opt_outfile = "a.c4r";
	asmc4r_opt_verify  = 1; // TODO: default to 0

	if(c4cc_init()) { return -1; }
	include_symbols = 1;
	// TODO: make a flag
	include_static  = 1;
	asmc4r_opt_source = 0;
	asmc4r_opt_pie = 1;
	//src = 0; // don't allow src output

	if (!(argv = respfile_expand(argc, argv, &argc))) return -1;

	if ((i = asmc4r_parse_commandline(&argc, &argv)))
		return i;

	poolsz = 256 * 1024;
	if(!(asmc4r_e_start = asmc4r_e = asmc4r_le = malloc(sizeof(int) * poolsz))) {
		printf("Unable to allocate %lld bytes\n", sizeof(int) * poolsz);
		return -1;
	}
	memset(asmc4r_e_start, 0, sizeof(int) * poolsz);

	// Allocate labels
	if (!(asmc4r_labels = malloc(i = sizeof(int) * (LBL__Sz * LABELS_MAX)))) {
		printf("Unable to allocate %lld bytes for labels\n", i);
		return -1;
	}
	memset(asmc4r_labels, 0, i);

	// Setup emit handlers
	c4cc_emithandlers[EH_LEA] = (int)&asmc4r_handler_LEA;
	c4cc_emithandlers[EH_IMM] = (int)&asmc4r_handler_IMM;
	c4cc_emithandlers[EH_LI] = (int)&asmc4r_handler_LI;
	c4cc_emithandlers[EH_LC] = (int)&asmc4r_handler_LC;
	c4cc_emithandlers[EH_RWLI] = (int)&asmc4r_handler_rewind_li;
	c4cc_emithandlers[EH_RWLC] = (int)&asmc4r_handler_rewind_lc;
	c4cc_emithandlers[EH_SI] = (int)&asmc4r_handler_SI;
	c4cc_emithandlers[EH_SC] = (int)&asmc4r_handler_SC;
	c4cc_emithandlers[EH_PSH] = (int)&asmc4r_handler_PSH;
	c4cc_emithandlers[EH_JMP] = (int)&asmc4r_handler_JMP;
	c4cc_emithandlers[EH_JMPPH] = (int)&asmc4r_handler_JMPPH;
	c4cc_emithandlers[EH_JSR] = (int)&asmc4r_handler_JSR;
	c4cc_emithandlers[EH_JSRI] = (int)&asmc4r_handler_JSRI;
	c4cc_emithandlers[EH_JSRS] = (int)&asmc4r_handler_JSRS;
	c4cc_emithandlers[EH_BZPH] = (int)&asmc4r_handler_BZPH;
	c4cc_emithandlers[EH_BNZPH] = (int)&asmc4r_handler_BNZPH;
	c4cc_emithandlers[EH_ADJ] = (int)&asmc4r_handler_ADJ;
	c4cc_emithandlers[EH_ENT] = (int)&asmc4r_handler_ENT;
	c4cc_emithandlers[EH_LEV] = (int)&asmc4r_handler_LEV;
	c4cc_emithandlers[EH_SYSCALL] = (int)&asmc4r_handler_SYSCALL;
	c4cc_emithandlers[EH_MATH] = (int)&asmc4r_handler_MATH;
	c4cc_emithandlers[EH_DATAPATCH] = (int)&asmc4r_handler_DATAPATCH;
	c4cc_emithandlers[EH_FUNCADDR] = (int)&asmc4r_handler_FunctionAddress;
	c4cc_emithandlers[EH_CURRADDR] = (int)&asmc4r_handler_CurrentAddress;
	c4cc_emithandlers[EH_UPDTADDR] = (int)&asmc4r_handler_UpdateAddress;
	c4cc_emithandlers[EH_PRINTACC] = (int)&asmc4r_PrintAccumulated;
	c4cc_emithandlers[EH_INSRC_LINE]= (int)&asmc4r_InSource_Line;
	c4cc_emithandlers[EH_SRC] = (int)&asmc4r_Source;
	c4cc_emithandlers[EH_FUNCTIONSTART] = (int)&asmc4r_FunctionStart;
	c4cc_emithandlers[EH_FUNCTIONEND] = (int)&asmc4r_FunctionEnd;

	//printf("asmc4r: emit asmc4r_e_start: %llx\n", asmc4r_e_start);
	//result = 0;
	//printf("Args: %lld\n", argc);
	//while(result < argc) printf("(%lld) %s\n", result++, *argv++);

	asmc4r_labels_count = 0;

	// Always using src mode
	src = 1;
	result = c4cc_main(argc, argv);
	free(asmc4r_e_start);
	free(asmc4r_labels);
	return result;
}

#ifndef NO_ASMC4R_MAIN
int main (int argc, char **argv) { return asmc4r_main(argc, argv); }
#endif
