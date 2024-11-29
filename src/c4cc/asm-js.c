// JavaScript assembler module for C4 CC
//
// Compiles C4 to a form loadable by the JavaScript emulator - see libjs/c4.js,
// or the resulting output for more information.
//
// Requires a node module for the JavaScript emulator to work, ensure you run:
//   npm install
// Invocation:
//   ./c4 c4m.c c4cc.c asm-js.c -- -s factorial.c > factorial.js && node factorial.js
//
// Note: The 64bit version is fairly slow. If you compile c4 for 32bit, the resulting Node
//       application is also much faster, as it uses Int32 rather than BigNum for its word
//       type.

#define C4CC_INCLUDED
#include "c4cc.c"

enum { BUF_INT = 64 };
enum { BASE_OCTAL = 8, BASE_DEC = 10, BASE_HEX = 16 };
char *asmjs_e, *asmjs_le, *asmjs_e_start;
int   asmjs_counter;

// Size of integer words in bytes
enum {
	SZINT_32 = 4,
	SZINT_64 = 8
};

// labels that get patched later
enum {
	LBL_TYPE,    // int, see LT_*
	LBL_INDEX,   // int, the code offset that needs to be patched
	LBL_VALUE,   // int, the value it needs to be patched to
	LBL__Sz,
	LABELS_MAX = 8096
};

enum {
	LT_CODE,     // Code patch
	LT_DATA      // Data patch
};

int *asmjs_labels, asmjs_labels_count;

int *asmjs_newlabel (int index, int type) {
	int *lbl;
	lbl = asmjs_labels + (LBL__Sz * asmjs_labels_count++);
	if (asmjs_labels_count > LABELS_MAX) {
		printf("asmjs_newlabel: reached LABELS_MAX\n");
		exit(-1);
	}
	lbl[LBL_TYPE]  = type;
	lbl[LBL_INDEX] = index;
	return lbl;
}

void asmjs_emit (char *s) {
	while(*s) *++asmjs_e = *s++;
}

char*asmjs_emit_int_buf;
char*asmjs_emit_int_using (int i, int base, char *dest) {
	char *prefix, *x, *y;
	int   l, offset;

	if (!asmjs_emit_int_buf) {
		if (!(asmjs_emit_int_buf = malloc(BUF_INT))) {
			printf("Error allocating %d bytes for int buffer\n", BUF_INT);
			exit(-1);
		}
	}
	memset(asmjs_emit_int_buf, 0, BUF_INT);

	prefix = 0;
	offset = 0;

	if (!base) base = BASE_DEC;
	else if(base == BASE_OCTAL) { prefix = "0"; offset = 1; }
	else if(base == BASE_HEX)   { prefix = "0x"; offset = 2; }

	x = dest;
	if (prefix) while (*prefix) *++x = *prefix++;
	
	c4cc_itoa(i, asmjs_emit_int_buf, base);
	// Copy to dest
	y = asmjs_emit_int_buf;
	while (*y) *++x = *y++;
	return x;
}

void asmjs_emit_int (int i, int base) {
	asmjs_e = asmjs_emit_int_using(i, base, asmjs_e);
}

void asmjs_emit_char (char c) {
	*++asmjs_e = c;
}

void asmjs_emit_ins (int ins, char *comment) {
	asmjs_emit("\n  /* ");
	asmjs_emit_int(asmjs_counter * sizeof(int), BASE_HEX);
	asmjs_emit(" */ , ");
	asmjs_emit_int(ins, BASE_DEC);
	asmjs_emit(" ");
	asmjs_emit("// ");
	asmjs_emit(comment);
	asmjs_emit(" ");
	asmjs_counter = asmjs_counter + 1;
}

void asmjs_emit_insarg (int ins, int arg, int argmode, char *comment) {
	asmjs_emit("\n  /* ");
	asmjs_emit_int(asmjs_counter * sizeof(int), BASE_HEX);
	asmjs_emit(" */ , ");
	asmjs_emit_int(ins, BASE_DEC);
	asmjs_emit(", ");
	asmjs_emit_int(arg, argmode);
	asmjs_emit(" ");
	asmjs_emit("// ");
	asmjs_emit(comment);
	asmjs_emit(" ");
	asmjs_emit_int(arg, argmode);
	asmjs_emit(" ");
	asmjs_counter = asmjs_counter + 2;
}

// LEA: a = (int)(bp + *pcval)
void asmjs_handler_LEA (int pcval) {
	asmjs_emit_insarg(LEA, pcval, BASE_DEC, "LEA");
}

enum { DEC_THRESHOLD = 1024 };
// IMM : a = *pc++;
// OISC: a = val
void asmjs_handler_IMM (int val) {
	char *p;
	int   base, *lbl;
	p = (char *)val;
	base = BASE_HEX;
	if (p >= data_s && p < data) {
		val = val - (int)data_s;
		if (val < DEC_THRESHOLD) base = BASE_DEC;
		// Create data patch label
		lbl = asmjs_newlabel(asmjs_counter + 1, LT_DATA);
		lbl[LBL_VALUE] = val;
		asmjs_emit_insarg(IMM, val, base, "IMMDATA");
	} else if (val >= (int)asmjs_e_start && val <= (int)asmjs_e) {
		if (val < DEC_THRESHOLD) base = BASE_DEC;
		// Create code patch label
		lbl = asmjs_newlabel(asmjs_counter + 1, LT_CODE);
		lbl[LBL_VALUE] = val;
		asmjs_emit_insarg(IMM, val, base, "IMMCODE");
	} else {
		if (val < DEC_THRESHOLD) base = BASE_DEC;
		asmjs_emit_insarg(IMM, val, base, "IMM");
	}
}

// LI: a = *(int *)a
// LC: a = *(char *)a;
char *asmjs_li_marker, *asmjs_lc_marker;
void asmjs_handler_LI () { asmjs_li_marker = asmjs_e + 1; asmjs_emit_ins(LI, "LI"); }
void asmjs_handler_LC () { asmjs_lc_marker = asmjs_e + 1; asmjs_emit_ins(LC, "LC"); }
void asmjs_handler_rewind_li () { asmjs_e = asmjs_li_marker; --asmjs_counter; }
void asmjs_handler_rewind_lc () { asmjs_e = asmjs_lc_marker; --asmjs_counter; }

// SI  : *(int *)*sp++ = a;
void asmjs_handler_SI (int mode) { asmjs_emit_ins(SI, "SI"); }
void asmjs_handler_SC (int mode) { asmjs_emit_ins(SC, "SC"); }

// PSH: *--sp = a;
void asmjs_handler_PSH () { asmjs_emit_ins(PSH, "PSH"); }

// JMP : pc = (int *)*pc;
// OISC: pc = loc
void asmjs_handler_JMP (int loc) {
	int *lbl; // Label required to get correct code offset when loaded
	lbl = asmjs_newlabel(asmjs_counter + 1, LT_CODE);
	lbl[LBL_VALUE] = loc;
	//printf("//asmjs_handler_JMP: created new label with value %d, 0x%x\n", loc, loc);
	asmjs_emit_insarg(JMP, loc, BASE_HEX, "JMP no placeholder ");
}
int *asmjs_handler_JMPPH() {
	int *lbl;
	lbl = asmjs_newlabel(asmjs_counter + 1, LT_CODE);
	asmjs_emit_insarg(JMP, 0, BASE_DEC, "JMP with placeholder ");
	asmjs_emit_int(lbl[LBL_INDEX], BASE_DEC);
	asmjs_emit(" ");
	return lbl;
}

// JSR : *--sp = (int)(pc + 1); pc = (int *)pc*; }
// OISC: *--sp = oisc4_e + INSTR_SIZE; PC = loc
void asmjs_handler_JSR (int loc) {
	int *lbl; // Label required to get correct code offset when loaded
	lbl = asmjs_newlabel(asmjs_counter + 1, LT_CODE);
	lbl[LBL_VALUE] = loc;
	asmjs_emit_insarg(JSR, loc * sizeof(int), BASE_HEX, "JSR");
}

// JSRI: *--sp = (int)(pc + 1); pc = (int *)*pc; pc = (int *)*pc
// OISC: --SP; *SP = PH:after;  pc = DEREFERENCE(DEREFERENCE(loc))
void asmjs_handler_JSRI(int loc) {
	int *lbl; // Label required to get correct data offset when loaded
	loc = loc - (int)data_s; // Get correct offset in DATA
	//printf("asmjs_handler_JSRI: loc %d, in data? %c\n", loc, (loc >= (int)data_s && loc <= (int)data) ? 'y' : 'n');
	lbl = asmjs_newlabel(asmjs_counter + 1, LT_DATA);
	lbl[LBL_VALUE] = loc;
	asmjs_emit_insarg(JSRI, loc, BASE_HEX, "JSRI");
}
// *--sp = (int)(pc + 1); pc = (int *)*(bp + *pc++);
void asmjs_handler_JSRS(int loc) {
	asmjs_emit_insarg(JSRS, loc, BASE_DEC, "JSRS");
}

// BZ  : pc = a ? (pc + 1) : (int *)*pc;
// OISC: if(a) pc = loc;
int *asmjs_handler_BZPH() {
	int *lbl;
	lbl = asmjs_newlabel(asmjs_counter + 1, LT_CODE);
	asmjs_emit_insarg(BZ, 0, BASE_DEC, "BZ with placeholder ");
	asmjs_emit_int(lbl[LBL_INDEX], BASE_DEC);
	asmjs_emit(" ");
	return lbl;
}
// BNZ : pc = a ? (int *)*pc : (pc + 1);
// OISC: if(!a) pc = loc;
int *asmjs_handler_BNZPH() {
	int *lbl;
	lbl = asmjs_newlabel(asmjs_counter + 1, LT_CODE);
	asmjs_emit_insarg(BNZ, 0, BASE_DEC, "BNZ with placeholder ");
	asmjs_emit_int(lbl[LBL_INDEX], BASE_DEC);
	asmjs_emit(" ");
	return lbl;
}

// ADJ : sp = sp + *pc++
// OISC: SP + adj -> SP
void asmjs_handler_ADJ(int adj) {
	asmjs_emit_insarg(ADJ, adj, BASE_DEC, "ADJ");
}

void asmjs_handler_ENT(int adj) {
	asmjs_emit_insarg(ENT, adj, BASE_DEC, "ENT");
}
// LEV : sp = bp; bp = (int *)*sp++; pc = (int *)sp++;
void asmjs_handler_LEV() {
	asmjs_emit_ins(LEV, "LEV");
}

char *asmjs_instruction_lookup_buf;
void asmjs_emit_c4_ins (int ins) {
	int i;
	// copy the 4 bytes from c4cc_instructions to the buffer
	i = 0;
	while(i < 4) {
		asmjs_instruction_lookup_buf[i] = c4cc_instructions[(ins * 5) + i];
		++i;
	}
	asmjs_instruction_lookup_buf[4] = 0;
	asmjs_emit_ins(ins, asmjs_instruction_lookup_buf);
}
void asmjs_handler_SYSCALL(int num, int argcount) {
	asmjs_emit_c4_ins(num);
}

void asmjs_handler_MATH(int operation) {
	asmjs_emit_c4_ins(operation);
}

int asmjs_handler_FunctionAddress () { return asmjs_counter; }
int asmjs_handler_CurrentAddress () { return asmjs_counter; }
void asmjs_handler_UpdateAddress (int *label, int addr) {
	if (0)
		printf("Rewrite label %x to %x (old = %d)\n",
		       (int)label, (int)addr, *label);
	label[LBL_VALUE] = addr;
}

void asmjs_InSource_Line (int line, int length, char *s) {
	printf("  // %d: %.*s", line, length - 1, s);
}

void asmjs_PrintAccumulated () {
	if(asmjs_e != asmjs_le + 1) {
		printf("%.*s\n", (asmjs_e - asmjs_le) + 1, asmjs_le);
		asmjs_le = asmjs_e + 1;
	}
}

// classic c isprint()
int c4cc_isprint (char c) {
	return (c >= ' ' && c <= '~');
}

// more selective isprint that disallows backticks, double quotes and backslashes
int c4cc_isstring (char c) {
	return !(c == '`' || c == '"' || c == '\\') && c4cc_isprint(c);
}

void emit_boilerplate () {
	int poolsz, i;
	char *d;
	poolsz = 256 * 1024;
	printf("var c4 = require('./libjs/c4.js');\n"
	       "const debugFlag = false;\n"
	       "const poolsz = %d;\n"
	       "const codeaddr = 0x000;\n"
	       "var vm = new c4.C4VM(c4.MT_%d, poolsz);\n", poolsz, sizeof(int) * 8);

	// Emit instruction table
	i = 0;
	d = c4cc_instructions;
	printf("vm.configure_instructions([\n");
	while(i++ <= EXIT) {
		printf("  [%d, '", i - 1);
		while(*d != ' ' && *d != ',') printf("%c", *d++);
		while(*d != ',') ++d;
		++d;
		printf("']%s\n", (i - 1) == EXIT ? "" : ",");
	}
	printf("]);\n");

	// Instruction stream starts
	printf("// Program follows\n"
	       "vm.loadwords(codeaddr, [\n");
}

void asmjs_Source () {
	int *lbl, i;
	char *d;

	i = asmjs_counter;
	printf("]);\n"
	       "var dataaddr = 0x%x; // %d words, %d bytes\n", i * sizeof(int), i, i * sizeof(int));
	// Emit label patchups
	printf("// Label patchups (%d)\n", asmjs_labels_count);
	lbl = asmjs_labels;
	i = 0;
	while(i < asmjs_labels_count) {
		if (lbl[LBL_TYPE] == LT_CODE)
			printf("vm.patch(codeaddr + 0x%x, 0x%x + codeaddr);\n", lbl[LBL_INDEX] * sizeof(int), lbl[LBL_VALUE] * sizeof(int));
		else
			printf("vm.patch(codeaddr + 0x%x, 0x%x + dataaddr);\n", lbl[LBL_INDEX] * sizeof(int), lbl[LBL_VALUE]);
		lbl = lbl + LBL__Sz;
		++i;
	}
	// Emit data
	printf("// Data: (%d bytes)\n", data - data_s);
	printf("vm.loadchars(dataaddr, %c", '"');
	i = 0;
	d = data_s;
	while (d < data) {
		if (++i > 60) { printf("%c\n  + %c", '"', '"'); i = 0; }
		if (c4cc_isstring(*d)) printf("%c", *d);
		else printf("\\x%02x", *d);
		++d;
	}
	printf("%c);\n", '"');
	// Store arguments
	printf("// Store arguments as char **\n"
	       "var dest = vm.memory.align(vm.memory.convert(dataaddr + %d) + vm.word);\n", data - data_s);
	printf("var ptrs = dest;\n"
	       "var args = process.argv.slice(1); // Skip first argument\n"
	       "var bytesNeeded = vm.memory.convert(args.length) * vm.word + vm.memory.convert(args.reduce((acc, s) => acc + s.length + 1, 0));\n"
	       "var argvAddr = dest + (vm.memory.convert(args.length) * vm.word);\n"
	       "// Store argv chars into their pointers\n"
	       "for (var i = 0; i < args.length; i++) {\n"
	       "  vm.writeword(ptrs, argvAddr);\n"
	       "  vm.loadchars(argvAddr, args[i] + String.fromCharCode(0));\n"
	       "  ptrs += vm.word;\n"
	       "  argvAddr += vm.memory.convert(args[i].length + 1);\n"
	       "}\n"
	       "var afterArgs = vm.memory.align(argvAddr);\n");
	// Initialization
	printf("vm.PC = vm.memory.convert(0x%x * %d);\n", idmain[emit_Val], sizeof(int));
	printf("vm.SP = vm.memory.align(afterArgs);\n"
	       "vm.SP = vm.memory.convert(poolsz) - vm.SP;\n");
	printf("vm.MallocStart = vm.BP = vm.SP; vm.SPMax = vm.SP;\n");
	// Stack setup - code for PSH and EXIT on return from main not working
	printf("if (debugFlag) console.log(vm.SP, vm.word);\n");
	printf("vm.SP -= vm.word; vm.writeword(vm.SP, %d); // *--sp = EXIT\n", EXIT);
	printf("vm.SP -= vm.word; vm.writeword(vm.SP, %d); // *--sp = PSH\n", PSH);
	printf("var t = vm.SP;                             // t = sp\n"
	       "vm.SP -= vm.word; vm.writeword(vm.SP, args.length);  // *--sp = argc\n"
	       "vm.SP -= vm.word; vm.writeword(vm.SP, dest);  // *--sp = argv\n"
	       "vm.SP -= vm.word; vm.writeword(vm.SP, t);  // *--sp = t\n");
	// Dump memory
	printf("if (debugFlag) {\n"
	       "  console.log('After patching, before run:');\n"
	       "  var max = vm.memory.convert(dataaddr + %d);\n", data - data_s);
	printf("  for (var i = vm.memory.convert(codeaddr); i < max; i += vm.word) {\n"
	       "    var str =            '' + i.toString(10) + ': ' + vm.readword(vm.memory.convert(i)).toString();\n"
		   "    i += vm.word; str += '\\t\\t' + i.toString(10) + ': ' + vm.readword(vm.memory.convert(i)).toString();\n"
		   "    i += vm.word; str += '\\t\\t' + i.toString(10) + ': ' + vm.readword(vm.memory.convert(i)).toString();\n"
		   "    console.log(str);\n"           
		   "  }\n"
	       "}\n");
	printf("vm.init();\n");
	// Leave a comment so that if C4 outputs its cycle count, it doesn't
	// interrupt code.
	printf("vm.run(debugFlag); // ");
}

int main (int argc, char **argv) {
	int poolsz, result, i;

	if(c4cc_init()) { return -1; }

	poolsz = 256 * 1024;
	if(!(asmjs_e_start = asmjs_e = asmjs_le = malloc(sizeof(int) * poolsz))) {
		printf("Unable to allocate %d bytes\n", sizeof(int) * poolsz);
		return -1;
	}
	memset(asmjs_e_start, 0, sizeof(int) * poolsz);

	// Allocate labels
	if (!(asmjs_labels = malloc(i = sizeof(int) * (LBL__Sz * LABELS_MAX)))) {
		printf("Unable to allocate %d bytes for labels\n", i);
		return -1;
	}
	memset(asmjs_labels, 0, i);

	if (!(asmjs_instruction_lookup_buf = malloc(5))) {
		printf("Malloc error\n");
		return -1;
	}

	// Setup emit handlers
	c4cc_emithandlers[EH_LEA] = (int)&asmjs_handler_LEA;
	c4cc_emithandlers[EH_IMM] = (int)&asmjs_handler_IMM;
	c4cc_emithandlers[EH_LI] = (int)&asmjs_handler_LI;
	c4cc_emithandlers[EH_LC] = (int)&asmjs_handler_LC;
	c4cc_emithandlers[EH_RWLI] = (int)&asmjs_handler_rewind_li;
	c4cc_emithandlers[EH_RWLC] = (int)&asmjs_handler_rewind_lc;
	c4cc_emithandlers[EH_SI] = (int)&asmjs_handler_SI;
	c4cc_emithandlers[EH_SC] = (int)&asmjs_handler_SC;
	c4cc_emithandlers[EH_PSH] = (int)&asmjs_handler_PSH;
	c4cc_emithandlers[EH_JMP] = (int)&asmjs_handler_JMP;
	c4cc_emithandlers[EH_JMPPH] = (int)&asmjs_handler_JMPPH;
	c4cc_emithandlers[EH_JSR] = (int)&asmjs_handler_JSR;
	c4cc_emithandlers[EH_JSRI] = (int)&asmjs_handler_JSRI;
	c4cc_emithandlers[EH_JSRS] = (int)&asmjs_handler_JSRS;
	c4cc_emithandlers[EH_BZPH] = (int)&asmjs_handler_BZPH;
	c4cc_emithandlers[EH_BNZPH] = (int)&asmjs_handler_BNZPH;
	c4cc_emithandlers[EH_ADJ] = (int)&asmjs_handler_ADJ;
	c4cc_emithandlers[EH_ENT] = (int)&asmjs_handler_ENT;
	c4cc_emithandlers[EH_LEV] = (int)&asmjs_handler_LEV;
	c4cc_emithandlers[EH_SYSCALL] = (int)&asmjs_handler_SYSCALL;
	c4cc_emithandlers[EH_MATH] = (int)&asmjs_handler_MATH;
	c4cc_emithandlers[EH_FUNCADDR] = (int)&asmjs_handler_FunctionAddress;
	c4cc_emithandlers[EH_CURRADDR] = (int)&asmjs_handler_CurrentAddress;
	c4cc_emithandlers[EH_UPDTADDR] = (int)&asmjs_handler_UpdateAddress;
	c4cc_emithandlers[EH_PRINTACC] = (int)&asmjs_PrintAccumulated;
	c4cc_emithandlers[EH_INSRC_LINE]= (int)&asmjs_InSource_Line;
	c4cc_emithandlers[EH_SRC] = (int)&asmjs_Source;

	//printf("asmjs: emit asmjs_e_start: %x\n", asmjs_e_start);
	//result = 0;
	//printf("Args: %d\n", argc);
	//while(result < argc) printf("(%d) %s\n", result++, *argv++);

	asmjs_counter = 0;
	asmjs_labels_count = 0;

	// Always using src mode
	src = 1;
	emit_boilerplate();
	result = c4cc_main(argc, argv);
	free(asmjs_e_start);
	free(asmjs_labels);
	if (asmjs_emit_int_buf) free(asmjs_emit_int_buf);
	free(asmjs_instruction_lookup_buf);
	return result;
}

