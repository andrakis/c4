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
// | |-----------------------------------------------------------------------| |
// |---------------------------------------------------------------------------|
// | Construct / Destruct segment format:                                      |
// | These run before a program starts (constructor) or after the program      |
// | returns from main (destructor.) Destructors not called if exit() used.    |
// | c and d are the segment markers.                                          |
// | |-----------------------------------------------------------------------| |
// | | Type | Name       | Purpose                                           | |
// | |-----------------------------------------------------------------------| |
// | |  B   | Priority   | Lower priorities run first                        | |
// | |  W   | Value      | Code offset of function to call                   | |
// |---------------------------------------------------------------------------|
// | Symbols segment format: S                                                 |
// | |-----------------------------------------------------------------------| |
// | | Type | Name       | Purpose                                           | |
// | |-----------------------------------------------------------------------| |
// | |  W   | Id         | C4 symbol id                                      | |
// | |  B   | Type       | C4 type                                           | |
// | |  B   | Class      | C4 class                                          | |
// | |  W   | Attributes | Eg static, external, etc                          | |
// | |  B   | NameLen    |                                                   | |
// | |  B.. | Name       | NameLen bytes, not including nul terminator       | |
// | |  W   | Value      |                                                   | |
// | |  W   | Length     | Useful only for functions currently               | |
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
int    asmc4r_opt_source;

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
int writechecked (int fd, void *buf, int count) {
	writeoffset = writeoffset + write(fd, buf, count);
}

// We include load-c4r only for the dump_c4r_info function so we can compare
// writing and reading.
#ifndef NO_LOADC4R_MAIN
#define NO_LOADC4R_MAIN 1
#endif
#include "load-c4r.c"

int  *asmc4r_e, *asmc4r_le, *asmc4r_e_start;

// labels that get patched later
enum {
	LBL_TYPE,    // int, see LT_*
	LBL_INDEX,   // int, the code offset that needs to be patched
	LBL_VALUE,   // int, the value it needs to be patched to
	LBL__Sz,
	LABELS_MAX = 8096
};

enum {
	LT_CODE = -1,     // Code patch
	LT_DATA = -2      // Data patch
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
          printf("%8.4s", &c4cc_instructions[*++asmc4r_le * 5]);
          if (*asmc4r_le <= ADJ) printf(" %d\n", *++asmc4r_le); else printf("\n");
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
	while ((*strc_b >= 'a' && *strc_b <= 'z') ||
		   (*strc_b >= 'A' && *strc_b <= 'Z') ||
		   (*strc_b >= '0' && *strc_b <= '9') ||
		    *strc_b == '_') {
		++strc_b;
	}

	// printf("symbol '%.*s' writing at offset 0x%lX\n", strc_b - strc_a, strc_a, writeoffset);
	writechecked(fd, &id, sizeof(int));        // Id
	writechecked(fd, &d[Type], sizeof(int));   // Type
	writechecked(fd, &d[Class], sizeof(int));  // Class
	writechecked(fd, &d[Attr], sizeof(int));   // Attributes
	tmp = strc_b - strc_a; writechecked(fd, &tmp, 1); // NameLen
	writechecked(fd, strc_a, strc_b - strc_a); // Name
	// TODO: check type, function, etc, output adjusted value
	// Value
	class = d[Class];
    value = d[Val];
	if (class == Fun) value = (int *)d[emit_Val] - asmc4r_e_start;
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

	if ((fd = open(file, O_TRUNC | O_WRONLY | O_CREAT, S_IRUSR | S_IWUSR)) < 0) {
		printf("failed to open file\n");
		return;
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
	writechecked(fd, "C", 1);
	writechecked(fd, asmc4r_e_start, sizeof(int) * (1 + asmc4r_e - asmc4r_e_start));
	// printf("Wrote %d bytes of code\n", 1 + asmc4r_e - asmc4r_e_start);
	// Data
	writechecked(fd, "D", 1);
	writechecked(fd, data_s, data - data_s);
	// Patches
	writechecked(fd, "P", 1);
	i = 0; lbl = asmc4r_labels;
	// printf("Writing %d patches at 0x%lX...", asmc4r_labels_count, writeoffset); fflush(stdout);
	while (i < asmc4r_labels_count) {
		writechecked(fd, &lbl[LBL_TYPE], sizeof(int));
		// printf("  patch %d type %d label index = 0x%lX  value = 0x%lX", i, lbl[LBL_TYPE], lbl[LBL_INDEX], lbl[LBL_VALUE]);
		// printf(" (%d)\n", lbl[LBL_VALUE]);
		writechecked(fd, &lbl[LBL_INDEX], sizeof(int));
		//tmp = lbl[LBL_INDEX] * sizeof(int); writechecked(fd, &tmp, sizeof(int));
		writechecked(fd, &lbl[LBL_VALUE], sizeof(int));
		// tmp = lbl[LBL_VALUE] * sizeof(int); writechecked(fd, &tmp, sizeof(int));
		++i;
		lbl = lbl + LBL__Sz;
	}
	// printf("wrote %d records\n", i);
	// Constructors
	// printf("Writing constructors at 0x%lX\n", writeoffset);
	writechecked(fd, "c", 1);
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
	writechecked(fd, "d", 1);
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
	writechecked(fd, "S", 1);
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
	close(fd);
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
	//src = 0; // don't allow src output

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
