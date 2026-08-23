// C4 Cross Compiler
//
// Compiles to any language of choice by way of implementation files.
// Different modules have their own flags. See asm-*.c for these implementations.
//
// The compiler allows code to be theoratically be generated for any architecture,
// and the current implementation files serve as examples of such usage. All an
// implementation needs to do is provide a few emit handler callback functions,
// and update an emit handler table to point to these.
//
// Supported extra keywords:
//   extern
//   static
//   __attribute__((constructor))
//   __attribute__((destructor))
//
// Example invocation:
// Using the compiled version: (run make)
//   ./c4cc src/tests/factorial.c              (Outputs to a.c4r)
//   ./c4cc -o fac.c4r src/tests/factorial.c   (Outputs to fac.c4r)
//   Using a preprocessor: (makes use of - argument to read from stdin)
//   gcc -E -DC4CC=1 -Iinclude src/tests/vararg.h | ./c4cc -o vararg.c4r -
//
// 2026/01/05: Added 'for'
// 2026/01/03: Added 'continue' for while loops
// 2025/05/19: variadic functions now supported:
//             int some_func (int count, ...) { ... }
//             include/stdarg.h has been implemented to support this.
// 2025/05/07: compound statements now supported:
//             a = (some_update(), some_return_value);
// 2024/08/02: finally printing out the statement causing the error, as well as
//             where in the line the error is.
//
// Planned:
//   __asm(char *name)             Insert the opcode for given string
//   __asm(int  *ptr)              Insert a pointer to a memory address
//   __asm(int   opcode)           Insert a direct opcode
//   __asm {                       Assembly block in following possible formats:
//      IMM 1
//      IMM &variable
//   }

// Original comments:
// char, int, and pointer types
// if, while, return, and expression statements
// just enough features to allow self-compilation and a bit more

// Originally written by Robert Swierczek

#include <stdio.h>
#include <stdlib.h>
#include <memory.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>

#include "c4.h"
// The C4DOS API. gcc takes the STUBS -- there is no DOS on a host, and
// dos_can_write() answering 0 is the honest result. c4 and c4cc skip
// '#' lines entirely and never see this; they are handed the real
// include/c4dos.h as a source file instead.
#include "c4dos_native.h"

#define NO_LOADC4R_MAIN 1
#include "load-c4r.c"

int *idstart, *idmain;
char *p, *lp, // current position in source code
     *data,   // data/bss pointer
     *data_s; // data start
char *c4cc_instructions; // Instructions as a char*
int  *c4cc_emithandlers; // Instruction emit handlers (see EH_*)
char *make_va_function;

int *e, *le,  // current position in emitted code
    *id,      // currently parsed identifier
    *sym,     // symbol table (simple list of identifiers)
    tk,       // current token
    ival,     // current token value
    ty,       // current expression type
    loc,      // local variable offset
    line,     // current line number
    src,      // print source and assembly flag
    debug;    // print executed instructions
char *line_start, *statement_start;

char *_p, *_data;       // initial pointer locations
int  *_sym, *_e, *_sp;  // initial pointer locations
int  *_oisc4_e;
int   c4cc_initialized;

// Patch label types shared with the backends (asm-c4r.c defines the same
// values in its LT_* enum; c4cc.c is included first, so they live here)
enum {
	PT_CODE  = -1,    // code-resident word -> code address
	PT_DATA  = -2,    // code-resident word -> data address
	PT_DCODE = -3,    // data-resident word -> code address
	PT_DDATA = -4     // data-resident word -> data address
};

// Local initializer support: declarations are parsed before ENT is
// emitted, so initializer stores are recorded here and emitted right
// after it. Each record is LINIT__Sz ints: kind (0 = word store,
// 1 = byte store), the symbol's slot value, the element index, and the
// value for each emission stream (they differ for &function).
enum { LINIT_KIND, LINIT_VAL, LINIT_IDX, LINIT_BVAL, LINIT_EVAL, LINIT__Sz,
       LINIT_MAX = 4096, LTMP_MAX = 512 };
int *linits;               // pending records
int  linits_n;
int *ltmp;                 // brace-list scratch, one initializer at a time

// Set when the most recent primary expression was an array name (which
// evaluates to an address, with no load to rewind): lets & accept it
int last_array;

int *curr_continue;        // Marks current begin of while loop
// break support: a stack of unresolved jump placeholders. Each while/for/
// switch records the depth on entry and resolves everything above it on
// exit, so breaks bind to the innermost construct.
int *brk_labels;           // backend labels from emit_JMPPH
int *brk_eslots;           // matching operand slots in the legacy e stream
int  brk_top;
int  brk_depth;            // constructs a break may target; 0 = error
enum { BRK_MAX = 256, SWITCH_MAX_CASES = 256, SWITCH_MAX_RANGE = 4096 };

// Original C4 doesn't recognise \t
enum { TAB = 9 };

// Used by ATTR_PRIORITY to shift the value given.
// May need to be adjusted if other attributes are added.
enum { PRIORITY_SHIFT = 6 };

// Attributes
enum {
    // Default
    ATTR_NONE = 0,
    // Puts functions into the constructor segment and automatically called
    // prior to main when using c4r executable format.
    ATTR_CONSTRUCTOR = 0x1,
    // Like ATTR_CONSTRUCTOR, but runs when main finishes
    ATTR_DESTRUCTOR  = 0x2,
    // Used by constructor and destructor attributes, value is shifted by
    // PRIORITY_SHIFT to obtain true priority value.
    ATTR_PRIORITY    = 0x4,
    // Static, not exported in symbol table, and not referencable by other
    // code.
    ATTR_STATIC      = 0x8,
    // External, resolved by linker. Default for functions with no body.
    // Turned into code patches
    ATTR_EXTERN      = 0x10,
    // Variadic function. Argument count is pushed onto the stack, and a
    // call to __c4cc_make_va is issued (see include/stdarg.h), as well
    // as some minor other instructions, such that extra arguments may be
    // accessed in the usual stdarg way, via va_arg and such macros.
    ATTR_VARIADIC    = 0x20,
    // Symbol is an array: its name evaluates to the address of its
    // storage rather than loading a value from it
    ATTR_ARRAY       = 0x40,
};

// tokens and classes (operators last and in precedence order)
enum {
  // Types
  Num = 128, Fun, Sys, Glo, Loc, Id,
  // Keywords and attributes
  Static, Extern, Attribute, Constructor, Destructor,
  Char, Else, Enum, If, Int, Return, Sizeof, For, Continue, While,
  Switch, Case, Default, Break,
  // Operators
  Assign, Cond, Lor, Lan, Or, Xor, And, Eq, Ne, Lt, Gt, Le, Ge, Shl, Shr, Add, Sub, Mul, Div, Mod, Inc, Dec, Brak,
};

char *c4cc_keywords;

// opcodes
//enum { LEA ,IMM ,JMP ,JSR ,JSRI,JSRS,BZ  ,BNZ ,ENT ,ADJ ,LEV ,LI  ,LC  ,SI  ,SC  ,PSH ,
//       JMPA,TLEV,DBG ,
//       OR  ,XOR ,AND ,EQ  ,NE  ,LT  ,GT  ,LE  ,GE  ,SHL ,SHR ,ADD ,SUB ,MUL ,DIV ,MOD ,
//       OPEN,READ,CLOS,PUTC,PUTS,PRTF,MALC,RALC,FREE,MSET,MCMP,MCPY,STRC,ITH ,_OPC,_BLT,
//	   _TRP,OPCD,_JMP,_ADJ,C4CF,C4CY,TIME,SIGH,SIGI,USLP,INFO,OPSL,FLT ,
//	   EXIT,
//	 };
//enum {
//	// Opcodes
//	LEA ,IMM ,JMP ,JSR ,BZ  ,BNZ ,ENT ,ADJ ,LEV ,LI  ,LC  ,SI  ,SC  ,PSH ,
//	OR  ,XOR ,AND ,EQ  ,NE  ,LT  ,GT  ,LE  ,GE  ,SHL ,SHR ,ADD ,SUB ,MUL ,DIV ,MOD ,
//	// Syscalls
//	OPEN,READ,CLOS,PRTF,MALC,FREE,MSET,MCMP,EXIT,
//	// C4M Extended opcodes
//	PUTC,PUTS,RALC,MCPY,STRC,
//	ITH ,_OPC,_BLT,_TRP,OPCD,
//	_JMP,_ADJ,C4CF,C4CY,TIME,
//	SIGH,SIGI,USLP,INFO,OPSL,
//	// C4 Invoke: call a section of code as if it were a C4 function
//	C4IV,
//	// Unsupported float instruction
//	FLT ,
//	// Instructions
//	JSRI,JSRS,JMPA,TLEV,DBG ,
//	// End of instructions
//	INS_SIZE,
//};
void c4cc_init_instructions() {
	c4cc_instructions = 
//	   "LEA ,IMM ,JMP ,JSR ,JSRI,JSRS,BZ  ,BNZ ,ENT ,ADJ ,LEV ,LI  ,LC  ,SI  ,SC  ,PSH ,"
//       "JMPA,TLEV,DBG ,"
//	   "OR  ,XOR ,AND ,EQ  ,NE  ,LT  ,GT  ,LE  ,GE  ,SHL ,SHR ,ADD ,SUB ,MUL ,DIV ,MOD ,"
//	   "OPEN,READ,CLOS,PUTC,PUTS,PRTF,MALC,RALC,FREE,MSET,MCMP,MCPY,STRC,ITH ,_OPC,_BLT,"
//	   "_TRP,OPCD,_JMP,_ADJ,C4CF,C4CY,TIME,SIGH,SIGI,USLP,INFO,OPSL,FLT ,"
//	   "EXIT,";
	// Opcodes
	"LEA ,IMM ,JMP ,JSR ,BZ  ,BNZ ,ENT ,ADJ ,LEV ,LI  ,LC  ,SI  ,SC  ,PSH ,"
	"OR  ,XOR ,AND ,EQ  ,NE  ,LT  ,GT  ,LE  ,GE  ,SHL ,SHR ,ADD ,SUB ,MUL ,DIV ,MOD ,"
	// Syscalls
	"OPEN,READ,CLOS,PRTF,MALC,FREE,MSET,MCMP,EXIT,"
	// C4M Extended opcodes
	"PUTC,PUTS,RALC,MCPY,STRC,"
	"ITH ,_OPC,_BLT,_TRP,OPCD,"
	"_JMP,_ADJ,C4CF,C4CY,TIME,"
	"SIGH,SIGI,USLP,INFO,OPSL,"
	// C4 Invoke: call a section of code as if it were a C4 function
	"C4IV,"
	// Unsupported float instruction
	"FLT ,"
	// Instructions
	"JSRI,JSRS,JMPA,TLEV,DBG ,"
	// c4mp's processor opcodes. Names only, so c4rdump can disassemble
	// a c4mp image -- it indexes this table with no bounds check, so
	// without them an opcode of 66 reads past the end. c4cc_keywords
	// below is deliberately NOT extended: c4cc targets c4m, which does
	// not have these, and c4lc is the compiler that emits them.
	"CPUI,CPUN,CPUS,CPUH,"
	"CAS ,XCHG,FADD,CWAI,CWAK,IPI ,"
	// M12: c4mp-only fused array-element load/store, -mcisc only.
	// Same reason as the processor opcodes above -- c4rdump indexes
	// this table with no bounds check.
	"LXI ,SXI ,";
	c4cc_keywords =
		"static extern __attribute__ constructor destructor "     // Ignored by c4m
		"char else enum if int return sizeof for continue while " // Keywords
		"switch case default break "                              // ... with a jumptable switch
		"open read close printf malloc free memset memcmp exit "  // Syscalls
		"putchar puts realloc memcpy stacktrace "                 // C4M extended opcodes...
		"install_trap_handler __opcode __builtin __c4_trap __c4_opcode "
		"__c4_jmp __c4_adjust __c4_configure __c4_cycles __time __c4_signal __c4_sigint "
		"__c4_usleep __c4_info __c4_ops_list "
		// C4 Invoke
		"__c4_invoke "
		// Future use: floating point support
		"__c4_float "
		"void main";                                             // void type and main entry
//	"static extern __attribute__ constructor destructor "
//      "char else enum if int return sizeof while "
//      "open read close putchar puts printf malloc realloc free memset memcmp memcpy stacktrace "
//      "install_trap_handler __opcode __builtin __c4_trap __c4_opcode "
//      "__c4_jmp __c4_adjust __c4_configure __c4_cycles __time __c4_signal __c4_sigint "
//	  "__c4_usleep __c4_info __c4_ops_list __c4_float "
//	  "exit void main";
}

// emit handlers
enum { EH_LEA, EH_IMM, EH_LI, EH_LC, EH_RWLI, EH_RWLC, EH_SI, EH_SC, EH_PSH,
       EH_JMP, EH_JMPPH, EH_JSR, EH_JSRI, EH_JSRS, EH_BZPH, EH_BNZPH, EH_ADJ,
       EH_ENT, EH_LEV, EH_SYSCALL, EH_MATH,
       EH_SIZEOF_CHAR, EH_SIZEOF_INT,
       EH_FUNCADDR, EH_CURRADDR, EH_UPDTADDR,
       EH_SRC,
       EH_INSRC_LINE, EH_PRINTACC,
       EH_FUNCTIONSTART, EH_FUNCTIONEND,
       EH_DATAPATCH,
       EH__Sz };

// types
enum { CHAR, INT, PTR };

// identifier offsets (since we can't create an ident struct)
enum { Tk, Hash, Name,
       Class, Type, Val, emit_Val, Attr, emit_Length, ArgCount,
       HClass, HType, HVal, Hemit_Val, HAttr, Hemit_Length, HArgCount,
       Idsz = 16 // TODO: Some values don't work
};

/////
// Utility
/////

// The only part of this file that requires c4_multiload, as it involves calling
// function pointers stored in variables. Also, some macro hackery to get it to
// work in traditional compilers.
#define ptr() ((int(*)())ptr)()
int invoke0 (int *ptr) { return ptr(); }
#undef ptr
#define ptr(a) ((int(*)(int))ptr)(a)
int invoke1 (int *ptr, int a) { return ptr(a); }
#undef ptr
#define ptr(a,b) ((int(*)(int,int))ptr)(a,b)
int invoke2 (int *ptr, int a, int b) { return ptr(a, b); }
#undef ptr
#define ptr(a,b,c) ((int(*)(int,int,int))ptr)(a,b,c)
int invoke3 (int *ptr, int a, int b, int c) { return ptr(a, b, c); }
#undef ptr

int symbol_id (int *d) {
	return (d - idstart) / Idsz;
}

// Test if given character is a valid c variable name
int iscvariable (char b) {
  return (
    (b >= 'a' && b <= 'z') ||
    (b >= 'A' && b <= 'Z') ||
    (b >= '0' && b <= '9') ||
     b == '_');
}

/////
// Emitters
/////

void C4CC_PrintAccC4 () {
  while (le < e) {
    ++le;
    printf("%8.4s", &c4cc_instructions[*le * 5]);
    if (*le <= ADJ) printf(" %llx\n", *++le); else printf("\n");
  }
}

// LEA: a = bp + pcval
void emit_LEA (int pcval) {
  invoke1((int*)c4cc_emithandlers[EH_LEA], pcval);
}

// IMM : a = *pc++;
// OISC: a = val
void emit_IMM (int val) {
  invoke1((int*)c4cc_emithandlers[EH_IMM], val);
}

// LI: a = *(int *)a
// LC: a = *(char *)a;
void emit_LI (int mode) {
  if(mode == LI) invoke0((int*)c4cc_emithandlers[EH_LI]);
  else if(mode == LC) invoke0((int*)c4cc_emithandlers[EH_LC]);
  else {
    printf("emit_LI: bad mode %lld (should be %lld or %lld)\n", mode, LI, LC);
    exit(-1);
  }
}
void emit_rewind_li () { invoke0((int*)c4cc_emithandlers[EH_RWLI]); }
void emit_rewind_lc () { invoke0((int*)c4cc_emithandlers[EH_RWLC]); }

// SI  : *(int *)*sp++ = a;
void emit_SI (int mode) {
  if (mode == SI) invoke0((int*)c4cc_emithandlers[EH_SI]);
  else if (mode == SC) invoke0((int*)c4cc_emithandlers[EH_SC]);
  else {
    printf("emit_SI: bad mode %lld (should be %lld or %lld)\n", mode, LI, LC);
    exit(-1);
  }
}

// PSH: *--sp = a;
void emit_PSH () {
  invoke0((int*)c4cc_emithandlers[EH_PSH]);
}

// JMP : pc = (int *)*pc;
// OISC: pc = loc
void emit_JMP (int *loc) {
  invoke1((int*)c4cc_emithandlers[EH_JMP], (int)loc);
}

int *emit_JMPPH() {
  return (int*)invoke0((int*)c4cc_emithandlers[EH_JMPPH]);
}

// JSR : *--sp = (int)(pc + 1); pc = (int *)pc*; }
// OISC: *--sp = oisc4_e + INSTR_SIZE; PC = loc
void emit_JSR (int *loc) {
  invoke1((int*)c4cc_emithandlers[EH_JSR], (int)loc);
}

// JSRI: *--sp = (int)(pc + 1); pc = (int *)*pc; pc = (int *)*pc
// OISC: --SP; *SP = PH:after;  pc = DEREFERENCE(DEREFERENCE(loc))
void emit_JSRI(int *loc) {
  invoke1((int*)c4cc_emithandlers[EH_JSRI], (int)loc);
}
// *--sp = (int)(pc + 1); pc = (int *)*(bp + *pc++);
void emit_JSRS(int loc) {
  invoke1((int*)c4cc_emithandlers[EH_JSRS], (int)loc);
}

// BZ  : pc = a ? (pc + 1) : (int *)*pc;
// OISC: if(a) pc = loc;
int *emit_BZPH() {
  return (int*)invoke0((int*)c4cc_emithandlers[EH_BZPH]);
}
// BNZ : pc = a ? (int *)*pc : (pc + 1);
// OISC: if(!a) pc = loc;
int *emit_BNZPH() {
  return (int*)invoke0((int*)c4cc_emithandlers[EH_BNZPH]);
}

// ADJ : sp = sp + *pc++
// OISC: SP + adj -> SP
void emit_ADJ(int adj) {
  invoke1((int*)c4cc_emithandlers[EH_ADJ], adj);
}

void emit_ENT(int adj) {
  invoke1((int*)c4cc_emithandlers[EH_ENT], adj);
}
// LEV : sp = bp; bp = (int *)*sp++; pc = (int *)sp++;
void emit_LEV() {
  invoke0((int*)c4cc_emithandlers[EH_LEV]);
}

void emit_SYSCALL(int num, int argcount) {
  invoke2((int*)c4cc_emithandlers[EH_SYSCALL], num, argcount);
}

// Record a data-resident patch (LT_DCODE/LT_DDATA): the word at byte
// offset dataoff in the data segment is relocated at load time. Used for
// pointer initializers of globals and switch jump tables.
void emit_DataPatch (int type, int dataoff, int value) {
  invoke3((int*)c4cc_emithandlers[EH_DATAPATCH], type, dataoff, value);
}

void emit_MATH(int operation) {
  invoke1((int*)c4cc_emithandlers[EH_MATH], operation);
}

// TODO: can this be replaced by emit_CurrentAddress?
int *emit_FunctionAddress () {
  return (int*)invoke0((int*)c4cc_emithandlers[EH_FUNCADDR]);
}
int *emit_CurrentAddress () {
  return (int*)invoke0((int*)c4cc_emithandlers[EH_CURRADDR]);
}
// Update a given label address. The label is whatever is returned
// from emit_FunctionAddress and emit_CurrentAddress, so they could
// be simple pointers or more complex structures could be used.
void emit_UpdateAddress (int *label, int *addr) {
  invoke2((int*)c4cc_emithandlers[EH_UPDTADDR], (int)label, (int)addr);
}
void emit_PrintAcc () {
  invoke0((int*)c4cc_emithandlers[EH_PRINTACC]);
}

int emit_sizeof_char () { return invoke0((int *)c4cc_emithandlers[EH_SIZEOF_CHAR]); }
int emit_sizeof_int  () { return invoke0((int *)c4cc_emithandlers[EH_SIZEOF_INT]); }

void emit_Done () {
  le = _e;
  invoke0((int*)c4cc_emithandlers[EH_SRC]);
}

#define stacktrace() do { printf("stacktrace()\n"); } while(0)
int stub_emithandler () {
  stacktrace();
  printf("STUB: emithandler\n");
  return 0;
}

void emit_InSource_Line (int line, int length, char *s) {
  invoke3((int*)c4cc_emithandlers[EH_INSRC_LINE], line, length, (int)s);
}

void stub_insource_line (int line, int length, char *s) {
  printf("%lld: %.*s", line, length, s);
}

void emit_FunctionStart (int *fun) {
  invoke1((int*)c4cc_emithandlers[EH_FUNCTIONSTART], (int)fun);
}

void stub_FunctionStart (int *fun) { }

void emit_FunctionEnd (int *fun) {
  invoke1((int *)c4cc_emithandlers[EH_FUNCTIONEND], (int)fun);
}

void stub_FunctionEnd (int *fun) { }

int stub_sizeof_char () { return sizeof(char); }
int stub_sizeof_int  () { return sizeof(int); }

int c4cc_strlen (char *s) {
  char *i; i = s;
  while(*s) ++s;
  return s - i;
}

char *c4cc_strncat (char *dest, char *src, int n) {
  int i, dest_len;
  i = 0;
  dest_len = c4cc_strlen(dest);

  while(i < n && src[i] != 0) {
    dest[dest_len + i] = src[i];
    ++i;
  }
  dest[dest_len + i] = 0;

  return dest;
}

void  c4cc_swapchar(char *x, char *y) { char t; t = *x; *x = *y; *y = t; }
char* c4cc_reverse(char *buffer, int i, int j) {
	while (i < j) {
		c4cc_swapchar(&buffer[i++], &buffer[j--]);
	}
	return buffer;
}
int c4cc_abs(int v) { return v >= 0 ? v : -v; }
// Iterative function to implement `itoa()` function in C
char* c4cc_itoa(int value, char* buffer, int base)
{
	int n, i, r;

	// invalid input
	if (base < 2 || base > 32) {
		return buffer;
	}

	// consider the absolute value of the number
	n = c4cc_abs(value);

	i = 0;
	while (n) {
		r = n % base;
		if (r >= 10) {
			buffer[i++] = 65 + (r - 10);
		} else {
			buffer[i++] = 48 + r;
		}
		n = n / base;
	}

	// if the number is 0
	if (i == 0) {
		buffer[i++] = '0';
	}

	// If the base is 10 and the value is negative, the resulting string
	// is preceded with a minus sign (-)
	// With any other base, value is always considered unsigned
	if (value < 0 && base == 10) {
		buffer[i++] = '-';
	}

	buffer[i] = 0; // null terminate string

	// reverse the string and return it
	return c4cc_reverse(buffer, 0, i - 1);
}

// Cause the compiler to exit, printing out the (possibly partial) statement
// that caused the error, with arrows (<-, ^) pointing to where the problem
// was encountered.
void die (int exit_code) {
	char *c, *e;
	int   l;
	// also uses global char *p, current position in source code

	c = statement_start;
	l = statement_start - line_start;

	// Print out the part not causing the issue (may be nothing)
	printf("%.*s", statement_start - line_start, line_start);
	// Print out the statement that caused the error
	printf("%.*s", p - c, c);
	// Point to the exact location parsing errored out at
	printf("/* <- here */");
	// Print the remainder of the line
	c = p;
	while(*c && *c != '\n') ++c;
	printf("%.*s\n", c - p, p);
	// Print spacer to the approximate location of the error, sometimes
	// this is inaccurate.
	printf("%*s^ here\n", l, " ");
	exit(exit_code);
}

int *symbolFind (char *name) {
  int *d, l;
  d = sym;
  l = c4cc_strlen(name);
  while (d[Tk]) {
    if (d[Name] && !memcmp((char *)d[Name], name, l)) return d;
    d = d + Idsz;
  }
  return 0;
}

/////
// C4 C Compiler
/////
void next()
{
  char *pp;

  while (tk = *p) {
    ++p;
    if (tk == '.') return; // va_args support
    if (tk == '\n') {
      if (src) {
        emit_InSource_Line(line, p - lp, lp);
        //printf("%d: %.*s", line, p - lp, lp);
        lp = p;
        emit_PrintAcc();
      }
      ++line;
	  line_start = p;
    }
    else if (tk == '#') {
      while (*p != 0 && *p != '\n') ++p;
    }
    else if ((tk >= 'a' && tk <= 'z') || (tk >= 'A' && tk <= 'Z') || tk == '_') {
      pp = p - 1;
      while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_')
        tk = tk * 147 + *p++;
      tk = (tk << 6) + (p - pp);
      id = sym;
      while (id[Tk]) {
        if (tk == id[Hash] && !memcmp((char *)id[Name], pp, p - pp)) { tk = id[Tk]; return; }
        id = id + Idsz;
      }
      id[Name] = (int)pp;
      id[Hash] = tk;
      tk = id[Tk] = Id;
      return;
    }
    else if (tk >= '0' && tk <= '9') {
      if (ival = tk - '0') { while (*p >= '0' && *p <= '9') ival = ival * 10 + *p++ - '0'; }
      else if (*p == 'x' || *p == 'X') {
        while ((tk = *++p) && ((tk >= '0' && tk <= '9') || (tk >= 'a' && tk <= 'f') || (tk >= 'A' && tk <= 'F')))
          ival = ival * 16 + (tk & 15) + (tk >= 'A' ? 9 : 0);
      }
      else { while (*p >= '0' && *p <= '7') ival = ival * 8 + *p++ - '0'; }
      tk = Num;
      return;
    }
    else if (tk == '/') {
      if (*p == '/') {
        // C++ style comment
        ++p;
        while (*p != 0 && *p != '\n') ++p;
      } else if (*p == '*') {
        // /* C style comment */
        ++p;
        while(*p && !(*p == '*' && *(p + 1) == '/'))
            ++p;
        p = p + 2;
      } else {
        tk = Div;
        return;
      }
    }
    else if (tk == '\'' || tk == '"') {
      pp = data;
      while (*p != 0 && *p != tk) {
        if ((ival = *p++) == '\\') {
          if ((ival = *p++) == 'n') ival = '\n';
          else if (ival == 't') ival = 8;
          else if (ival == 'r') ival = 10;
          else if (ival == '0') ival = 0;
        }
        if (tk == '"') { *data++ = ival; }
      }
      ++p;
      if (tk == '"') {
        // An empty string literal must still occupy a data byte: the
        // emitted IMM otherwise points at the current end of the data
        // pool, and asm-c4r only creates relocation patches for
        // addresses strictly below it, so "" in a .c4r held a garbage
        // absolute address.
        if (data == pp) *data++ = 0;
        ival = (int)pp;
      } else tk = Num;
      return;
    }
    else if (tk == '=') { if (*p == '=') { ++p; tk = Eq; } else tk = Assign; return; }
    else if (tk == '+') { if (*p == '+') { ++p; tk = Inc; } else tk = Add; return; }
    else if (tk == '-') { if (*p == '-') { ++p; tk = Dec; } else tk = Sub; return; }
    else if (tk == '!') { if (*p == '=') { ++p; tk = Ne; } return; }
    else if (tk == '<') { if (*p == '=') { ++p; tk = Le; } else if (*p == '<') { ++p; tk = Shl; } else tk = Lt; return; }
    else if (tk == '>') { if (*p == '=') { ++p; tk = Ge; } else if (*p == '>') { ++p; tk = Shr; } else tk = Gt; return; }
    else if (tk == '|') { if (*p == '|') { ++p; tk = Lor; } else tk = Or; return; }
    else if (tk == '&') { if (*p == '&') { ++p; tk = Lan; } else tk = And; return; }
    else if (tk == '^') { tk = Xor; return; }
    else if (tk == '%') { tk = Mod; return; }
    else if (tk == '*') { tk = Mul; return; }
    else if (tk == '[') { tk = Brak; return; }
    else if (tk == '?') { tk = Cond; return; }
    else if (tk == '~' || tk == ';' || tk == '{' || tk == '}' || tk == '(' || tk == ')' || tk == ']' || tk == ',' || tk == ':') return;
  }
}

void expr(int lev)
{
  int t, *d, *d1, *d2, i, x;
  char *b;

  if (!tk) { printf("%d: unexpected eof in expression\n", line); die(-1); }
  else if (tk == Num) {
    *++e = IMM; *++e = ival;
    emit_IMM(ival);
	// TODO: External symbols not implemented.
    if (0 && id[Attr] & id[ATTR_EXTERN]) { // load updated reference from data
      *++e = PSH; emit_PSH();
      *++e = LI;  emit_LI(LI);
    }
    next(); ty = INT;
  }
  else if (tk == '"') {
    *++e = IMM; *++e = ival;
    emit_IMM(ival);
    next();
    while (tk == '"') next();
    data = (char *)((int)data + sizeof(int) & -sizeof(int)); ty = PTR;
  }
  else if (tk == Sizeof) {
    next(); if (tk == '(') next(); else { printf("%d: open paren expected in sizeof\n", line); die(-1); }
    if (tk == Id && (id[Attr] & ATTR_ARRAY)) {
      // sizeof(array): total storage in bytes, recorded at declaration
      *++e = IMM; *++e = id[emit_Length];
      emit_IMM(id[emit_Length]);
      next();
    } else {
      ty = INT; if (tk == Int) next(); else if (tk == Char) { next(); ty = CHAR; }
      while (tk == Mul) { next(); ty = ty + PTR; }
      *++e = IMM; *++e = (ty == CHAR) ? sizeof(char) : sizeof(int);
      emit_IMM((ty == CHAR) ? emit_sizeof_char() : emit_sizeof_int());
    }
    if (tk == ')') next(); else { printf("%d: close paren expected in sizeof\n", line); die(-1); }
    ty = INT;
  }
  else if (tk == Id) {
    d = id; next();
    if (tk == '(') {
      next();
      t = 0;
      while (tk != ')') { expr(Assign); *++e = PSH; emit_PSH(); ++t; if (tk == ',') next(); }
      next();
      // A syscall, ie all the builtin functions like open,read,etc
      if (d[Class] == Sys) {
        *++e = d[Val];
        emit_SYSCALL(d[Val], t);
      }
      // A C4 subroutine
      else if (d[Class] == Fun) {
        // find symbol name length
        x = 0; b = (char *)d[Name]; while (iscvariable(*b)) { ++b; ++x; }
        // Variadic function support
        if (d[Attr] & ATTR_VARIADIC) {
          // If ATTR_VARIADIC, before emitting JSR, call make_va
#if 0
          printf("(c4cc: calling '");
          printf("%.*s", x, (char *)d[Name]);
          printf("' that takes %d args with %d arguments)\n", d[ArgCount], t);
#endif
          // IMM extra_argcount
          *++e = IMM; *++e = t + 1 - d[ArgCount]; emit_IMM(t + 1 - d[ArgCount]);
          // PSH
          *++e = PSH; emit_PSH();
          // JSR __c4cc_make_va
          d1 = symbolFind("__c4cc_make_va"); // TODO: move this elsewhere
          if (!d1) { printf("vararg support requires a __c4cc_make_va function, include stdarg.h\n"); die(-1); }
          *++e = JSR; *++e = d1[Val]; emit_JSR(d1);
          // ADJ extra_argcount+2 to clean up arguments to __c4cc_make_va
          *++e = ADJ; *++e = t + 2 - d[ArgCount]; emit_ADJ(t + 2 - d[ArgCount]);
          // PSH the result of __c4cc_make_va
          *++e = PSH; emit_PSH();
          // t (used in ADJ below) should be the correct arg count
          t = d[ArgCount];
        } else if (t != d[ArgCount]) {
          // Argument count mismatches result in arguments referring to the
          // wrong parameters, and locals referring to incorrect offsets.
          // TODO: Is it better to error here than cause strange runtime errors?
		  // Changed to a warning.
          printf("%d: WARNING argument count mismatch in call to '%.*s', ", line, x, (char *)d[Name]);
          printf("expected %d arguments, %d given\n", d[ArgCount], t);
        }
        *++e = JSR; *++e = d[Val]; emit_JSR(d);
      }
      // A C4 subroutine stored in a global variable. Cannot be variadic, as
      // the only type info we have is INT+PTR.
      else if (d[Class] == Glo) { *++e = JSRI; *++e = d[Val]; emit_JSRI((int *)d[Val]); } // Jump subroutine indirect
      // A C4 subroutine stored in a stack variable. Cannot be variadic, as above.
      else if (d[Class] == Loc) { *++e = JSRS; *++e = loc - d[Val]; emit_JSRS(loc - d[Val]); } // Jump subroutine on stack
      else { printf("%d: bad function call (%d)\n", line, d[Class]); die(-1); }
      // Cleanup pushed arguments upon return
      if (t) {
        *++e = ADJ; *++e = t; emit_ADJ(t);
      }
      ty = d[Type];
    } else if (d[Class] == Num) {
      *++e = IMM; *++e = d[Val]; ty = INT;
      emit_IMM(d[Val]);
    } else {
      if (d[Class] == Loc) {        // Local variable
        *++e = LEA; *++e = loc - d[Val];
        emit_LEA(loc - d[Val]);
      } else if (d[Class] == Glo) { // Global variable
        *++e = IMM; *++e = d[Val];
        emit_IMM(d[Val]);           // TODO: use custom data area?
      } else if (d[Class] == Fun) { // Function address
        *++e = IMM; *++e = d[Val];
        emit_IMM(d[emit_Val]);
      } else { printf("%d: undefined variable\n", line); die(-1); }
      // An array name evaluates to the address of its storage; anything
      // else loads the value found there
      if (d[Attr] & ATTR_ARRAY) {
        ty = d[Type];
        last_array = 1;
      } else {
        *++e = ((ty = d[Type]) == CHAR) ? LC : LI;
        emit_LI(*e);
        last_array = 0;
      }
    }
  }
  else if (tk == '(') {
    next();
    // Casting: (char) (int), with pointers
    if (tk == Int || tk == Char) {
      t = (tk == Int) ? INT : CHAR; next();
      while (tk == Mul) { next(); t = t + PTR; }
      if (tk == ')') next(); else { printf("%d: bad cast\n", line); die(-1); }
      expr(Inc);
      ty = t;
    } else {
      // Normal C expression
      expr(Assign);
      // Allow compound statements in brackets
      while (tk == ',') { next(); expr(Assign); }
      if (tk == ')') next();
      else { printf("%d: close paren expected (1)\n", line); die(-1); }
    }
  }
  else if (tk == Mul) {
    next(); expr(Inc);
    if (ty > INT) ty = ty - PTR; else { printf("%d: bad dereference\n", line); die(-1); }
    *++e = (ty == CHAR) ? LC : LI;
    emit_LI(*e);
  }
  else if (tk == And) {
    next();
    last_array = 0;
    expr(Inc);
    if (*e == LC || *e == LI) {
      if (*e == LC) emit_rewind_lc();
      else emit_rewind_li();
      --e;
    } else if (last_array) {
      // &array: the name already evaluated to its address, so this is a
      // no-op (as in C, where &a and a differ only in type)
    } else { printf("%d: bad address-of\n", line); die(-1); }
    ty = ty + PTR;
  }
  else if (tk == '!') {
    next(); expr(Inc);
    *++e = PSH; emit_PSH();
    *++e = IMM; *++e = 0; emit_IMM(0);
    *++e = EQ; emit_MATH(EQ);
    ty = INT;
  }
  else if (tk == '~') {
    next(); expr(Inc);
    *++e = PSH; emit_PSH();
    *++e = IMM; *++e = -1; emit_IMM(-1);
    *++e = XOR; emit_MATH(XOR);
    ty = INT;
  }
  else if (tk == Add) { next(); expr(Inc); ty = INT;  }
  else if (tk == Sub) {
    next(); *++e = IMM;
    if (tk == Num) {
      *++e = -ival; emit_IMM(-ival);
      next();
    } else {
      *++e = -1; emit_IMM(-1);
      *++e = PSH; emit_PSH();
      expr(Inc);
      *++e = MUL; emit_MATH(MUL);
    }
    ty = INT;
  }
  else if (tk == Inc || tk == Dec) {
    t = tk; next(); expr(Inc);
    if (*e == LC) {
      *e = PSH; *++e = LC;
      emit_rewind_lc();
      emit_PSH();
      emit_LI(LC);
    }
    else if(*e == LI) {
      *e = PSH; *++e = LI;
      emit_rewind_li();
      emit_PSH();
      emit_LI(LI);
    }
    else { printf("%d: bad lvalue in pre-increment\n", line); die(-1); }
    *++e = PSH;
    emit_PSH();
    *++e = IMM; *++e = (ty > PTR) ? sizeof(int) : sizeof(char);
    emit_IMM((ty > PTR) ? emit_sizeof_int() : emit_sizeof_char());
    *++e = (t == Inc) ? ADD : SUB;
    emit_MATH(*e);
    *++e = (ty == CHAR) ? SC : SI;
    emit_SI(*e);
  }
  else { printf("%d: bad expression\n", line); die(-1); }

  while (tk >= lev) { // "precedence climbing" or "Top Down Operator Precedence" method
    t = ty;
    if (tk == Assign) {
      next();
      if (*e == LC || *e == LI) {
        if (*e == LC) emit_rewind_lc();
        else emit_rewind_li();
        *e = PSH;
        emit_PSH();
      } else { printf("%d: bad lvalue in assignment\n", line); die(-1); }
      expr(Assign);
      *++e = ((ty = t) == CHAR) ? SC : SI;
      emit_SI(*e);
    }
    else if (tk == Cond) {
      next();
      *++e = BZ; d = ++e; d1 = emit_BZPH();
      expr(Assign);
      if (tk == ':') next(); else { printf("%d: conditional missing colon\n", line); die(-1); }
      *d = (int)(e + 3); *++e = JMP; d = ++e;
      d2 = emit_JMPPH();
      emit_UpdateAddress(d1, emit_CurrentAddress());
      d1 = d2;
      expr(Cond);
      *d = (int)(e + 1);
      emit_UpdateAddress(d1, emit_CurrentAddress());
    }
    else if (tk == Lor) {
      next(); *++e = BNZ; d = ++e;
      d1 = emit_BNZPH();
      expr(Lan);
      *d = (int)(e + 1);
      emit_UpdateAddress(d1, emit_CurrentAddress());
      ty = INT;
    }
    else if (tk == Lan) {
      next(); *++e = BZ;  d = ++e;
      d1 = emit_BZPH();
      expr(Or);  *d = (int)(e + 1);
                 emit_UpdateAddress(d1, emit_CurrentAddress());
      ty = INT;
    }
    else if (tk == Or)  { next(); *++e = PSH; emit_PSH(); expr(Xor); *++e = OR; emit_MATH(OR); ty = INT;  }
    else if (tk == Xor) { next(); *++e = PSH; emit_PSH(); expr(And); *++e = XOR; emit_MATH(XOR); ty = INT;  }
    else if (tk == And) { next(); *++e = PSH; emit_PSH(); expr(Eq); *++e = AND; emit_MATH(AND); ty = INT;  }
    else if (tk == Eq)  { next(); *++e = PSH; emit_PSH(); expr(Lt); *++e = EQ; emit_MATH(EQ); ty = INT;  }
    else if (tk == Ne)  { next(); *++e = PSH; emit_PSH(); expr(Lt); *++e = NE; emit_MATH(NE); ty = INT;  }
    else if (tk == Lt)  { next(); *++e = PSH; emit_PSH(); expr(Shl); *++e = LT; emit_MATH(LT); ty = INT;  }
    else if (tk == Gt)  { next(); *++e = PSH; emit_PSH(); expr(Shl); *++e = GT; emit_MATH(GT); ty = INT;  }
    else if (tk == Le)  { next(); *++e = PSH; emit_PSH(); expr(Shl); *++e = LE; emit_MATH(LE); ty = INT;  }
    else if (tk == Ge)  { next(); *++e = PSH; emit_PSH(); expr(Shl); *++e = GE; emit_MATH(GE);ty = INT;  }
    else if (tk == Shl) { next(); *++e = PSH; emit_PSH(); expr(Add); *++e = SHL; emit_MATH(SHL);ty = INT;  }
    else if (tk == Shr) { next(); *++e = PSH; emit_PSH(); expr(Add); *++e = SHR; emit_MATH(SHR);ty = INT;  }
    else if (tk == Add) {
      next(); *++e = PSH; emit_PSH(); expr(Mul);
      if ((ty = t) > PTR) { *++e = PSH; emit_PSH(); *++e = IMM; *++e = sizeof(int); emit_IMM(emit_sizeof_int()); *++e = MUL; emit_MATH(MUL); }
      *++e = ADD;
      emit_MATH(ADD);
    }
    else if (tk == Sub) {
      next(); *++e = PSH; emit_PSH(); expr(Mul);
      if (t > PTR && t == ty) { *++e = SUB; emit_MATH(SUB); *++e = PSH; emit_PSH(); *++e = IMM; *++e = sizeof(int); emit_IMM(emit_sizeof_int()); *++e = DIV; emit_MATH(DIV); ty = INT; }
      else if ((ty = t) > PTR) { *++e = PSH; emit_PSH(); *++e = IMM; *++e = sizeof(int); emit_IMM(emit_sizeof_int()); *++e = MUL; emit_MATH(MUL); *++e = SUB; emit_MATH(SUB); }
      else { *++e = SUB; emit_MATH(SUB); }
    }
    else if (tk == Mul) { next(); *++e = PSH; emit_PSH(); expr(Inc); *++e = MUL; emit_MATH(MUL); ty = INT; }
    else if (tk == Div) { next(); *++e = PSH; emit_PSH(); expr(Inc); *++e = DIV; emit_MATH(DIV); ty = INT; }
    else if (tk == Mod) { next(); *++e = PSH; emit_PSH(); expr(Inc); *++e = MOD; emit_MATH(MOD); ty = INT; }
    else if (tk == Inc || tk == Dec) {
      if (*e == LC) {
        *e = PSH; *++e = LC;
        emit_rewind_lc();
        emit_PSH();
        emit_LI(LC);
      }
      else if (*e == LI) {
        *e = PSH; *++e = LI;
        emit_rewind_li();
        emit_PSH();
        emit_LI(LI);
      }
      else { printf("%d: bad lvalue in post-increment\n", line); die(-1); }
      *++e = PSH; *++e = IMM; *++e = (ty > PTR) ? sizeof(int) : sizeof(char);
      emit_PSH(); emit_IMM((ty > PTR) ? emit_sizeof_int() : emit_sizeof_char());
      *++e = (tk == Inc) ? ADD : SUB;
      emit_MATH(*e);
      *++e = (ty == CHAR) ? SC : SI;
      emit_SI(*e);
      *++e = PSH; *++e = IMM; *++e = (ty > PTR) ? sizeof(int) : sizeof(char);
      emit_PSH(); emit_IMM((ty > PTR) ? emit_sizeof_int() : emit_sizeof_char());
      *++e = (tk == Inc) ? SUB : ADD;
      emit_MATH(*e);
      next();
    }
    else if (tk == Brak) {
      next(); *++e = PSH; emit_PSH(); expr(Assign);
      if (tk == ']') next(); else { printf("%d: close bracket expected\n", line); die(-1); }
      if (t > PTR) { *++e = PSH; emit_PSH(); *++e = IMM; *++e = sizeof(int); emit_IMM(emit_sizeof_int()); *++e = MUL; emit_MATH(MUL); }
      else if (t < PTR) { printf("%d: pointer type expected\n", line); die(-1); }
      *++e = ADD; emit_MATH(ADD);
      *++e = ((ty = t - PTR) == CHAR) ? LC : LI;
      emit_LI(*e);
    }
    else { printf("%d: c4cc_compiler error tk=%d\n", line, tk); die(-1); }
  }
}

void stmt()
{
  int *a, *b;
  int *oa, *ob, *oc;
  int *last_continue;
  // switch state (see the Switch branch below)
  int *swv, *swa, *swea, *tlbl;
  int  swn, swdef, swmin, swmax, swrange, i2, v2, neg2, brk_base;
  int *b3, *b4, *bend, *bdef, *odef, *oend, *o3, *o4;

  statement_start = p;

  if (tk == If) {
    next();
    if (tk == '(') next(); else { printf("%d: open paren expected\n", line); die(-1); }
    a = e; // save begin
    expr(Assign);
    if (tk == ')') next(); else { printf("%d: close paren expected (2)\n", line); die(-1); }
    // TODO: if expression was if (1)
    *++e = BZ; b = ++e;
    ob = emit_BZPH();
    stmt();
    if (tk == Else) {
      *b = (int)(e + 3); *++e = JMP; b = ++e;
      oc = emit_JMPPH();
      emit_UpdateAddress(ob, emit_CurrentAddress());
      ob = oc;
      next();
      stmt();
    }
    *b = (int)(e + 1);
    emit_UpdateAddress(ob, emit_CurrentAddress());
  }
  else if (tk == Continue) {
    next();
    if (tk != ';') { printf("%d: semicolon expected after 'continue'\n", line); die(-1); }
    *++e = JMP; *++e = (int)curr_continue;
    emit_JMP(curr_continue);
  }
  else if (tk == Break) {
    next();
    if (tk != ';') { printf("%d: semicolon expected after 'break'\n", line); die(-1); }
    if (!brk_depth) { printf("%d: 'break' outside of loop or switch\n", line); die(-1); }
    if (brk_top >= BRK_MAX) { printf("%d: too many pending breaks\n", line); die(-1); }
    *++e = JMP; b = ++e;
    brk_eslots[brk_top] = (int)b;
    brk_labels[brk_top] = (int)emit_JMPPH();
    ++brk_top;
  }
  else if (tk == Switch) {
    // switch (expr) { case C: ... default: ... }, with C fallthrough and
    // break, compiled to a jump table in the DATA segment: each entry is
    // a data-resident CODE patch (LT_DCODE), relocated by the loader.
    // Layout, in emission order:
    //
    //     <expr>                a = value
    //     JMP dispatch
    //   body:                   cases record addresses; break -> end
    //     JMP end               (running off the end of the body)
    //   dispatch:
    //     [PSH; IMM min; SUB]   a = idx = value - min    (when min != 0)
    //     PSH; PSH; PSH         three idx copies on the stack
    //     IMM range; GT; BNZ oob2
    //     IMM 0;     LT; BNZ oob1
    //     IMM 8; MUL            a = idx * wordsize, stack clean again
    //     PSH; IMM table; ADD; LI; JMPA
    //   oob2: ADJ 2; JMP default-or-end
    //   oob1: ADJ 1; JMP default-or-end
    //   end:
    //
    // The table is filled after the shims, when every target (cases,
    // default, end) is a known address -- no placeholders needed.
    // JMPA is a c4m opcode: like function pointers (JSRI), switch needs
    // c4m or better at runtime; plain c4 will trap on it.
    next();
    if (tk == '(') next(); else { printf("%d: in 'switch': open paren expected\n", line); die(-1); }
    expr(Assign);
    if (tk == ')') next(); else { printf("%d: in 'switch': close paren expected\n", line); die(-1); }
    if (!(swv = malloc(SWITCH_MAX_CASES * sizeof(int))) ||
        !(swa = malloc(SWITCH_MAX_CASES * sizeof(int)))) { printf("%d: switch: out of memory\n", line); die(-1); }
    swn = 0; swdef = 0; odef = 0; bdef = 0;
    *++e = JMP; b = ++e;
    ob = emit_JMPPH();                      // entry -> dispatch
    brk_base = brk_top; ++brk_depth;
    if (tk == '{') next(); else { printf("%d: in 'switch': open brace expected\n", line); die(-1); }
    while (tk != '}') {
      if (tk == Case) {
        next();
        neg2 = 0;
        if (tk == Sub) { next(); neg2 = 1; }
        if (tk == Num) v2 = ival;
        else if (tk == Id && id[Class] == Num) v2 = id[Val];
        else { printf("%d: 'case' needs an integer constant\n", line); die(-1); }
        if (neg2) v2 = -v2;
        next();
        if (tk == ':') next(); else { printf("%d: colon expected after 'case'\n", line); die(-1); }
        i2 = 0;
        while (i2 < swn) {
          if (swv[i2] == v2) { printf("%d: duplicate case value %d\n", line, v2); die(-1); }
          ++i2;
        }
        if (swn >= SWITCH_MAX_CASES) { printf("%d: too many cases\n", line); die(-1); }
        swv[swn] = v2;
        swa[swn] = (int)emit_CurrentAddress();
        ++swn;
      }
      else if (tk == Default) {
        next();
        if (tk == ':') next(); else { printf("%d: colon expected after 'default'\n", line); die(-1); }
        if (swdef) { printf("%d: duplicate 'default'\n", line); die(-1); }
        swdef = 1;
        odef = emit_CurrentAddress();
        bdef = e + 1;
      }
      else stmt();
    }
    next();
    // running off the end of the body goes to end
    *++e = JMP; bend = ++e;
    oend = emit_JMPPH();
    // dispatch
    *b = (int)(e + 1);
    emit_UpdateAddress(ob, emit_CurrentAddress());
    if (!swn) {
      // no cases at all: default if present, else straight through
      if (swdef) { *++e = JMP; *++e = (int)bdef; emit_JMP(odef); }
    } else {
      swmin = swmax = swv[0];
      i2 = 1;
      while (i2 < swn) {
        if (swv[i2] < swmin) swmin = swv[i2];
        if (swv[i2] > swmax) swmax = swv[i2];
        ++i2;
      }
      swrange = swmax - swmin;
      if (swrange >= SWITCH_MAX_RANGE) { printf("%d: switch range %d too sparse for a jump table\n", line, swrange); die(-1); }
      // reserve the table in the data segment
      data = (char *)(((int)data + sizeof(int) - 1) & (0 - sizeof(int)));
      tlbl = (int *)data;
      data = data + (swrange + 1) * sizeof(int);
      // bounds check, three copies of idx on the stack
      if (swmin) {
        *++e = PSH; *++e = IMM; *++e = swmin; *++e = SUB;
        emit_PSH(); emit_IMM(swmin); emit_MATH(SUB);
      }
      *++e = PSH; emit_PSH();
      *++e = PSH; emit_PSH();
      *++e = PSH; emit_PSH();
      *++e = IMM; *++e = swrange; emit_IMM(swrange);
      *++e = GT; emit_MATH(GT);
      *++e = BNZ; b3 = ++e; o3 = emit_BNZPH();
      *++e = IMM; *++e = 0; emit_IMM(0);
      *++e = LT; emit_MATH(LT);
      *++e = BNZ; b4 = ++e; o4 = emit_BNZPH();
      *++e = IMM; *++e = sizeof(int); emit_IMM(sizeof(int));
      *++e = MUL; emit_MATH(MUL);
      *++e = PSH; emit_PSH();
      *++e = IMM; *++e = (int)tlbl; emit_IMM((int)tlbl); // auto DATA patch
      *++e = ADD; emit_MATH(ADD);
      *++e = LI; emit_LI(LI);
      *++e = JMPA; emit_MATH(JMPA);
      // out-of-range: drop the leftover idx copies, then default or end
      *b3 = (int)(e + 1);
      emit_UpdateAddress(o3, emit_CurrentAddress());
      *++e = ADJ; *++e = 2; emit_ADJ(2);
      if (swdef) { *++e = JMP; *++e = (int)bdef; emit_JMP(odef); }
      else {
        if (brk_top >= BRK_MAX) { printf("%d: too many pending breaks\n", line); die(-1); }
        *++e = JMP; b = ++e;
        brk_eslots[brk_top] = (int)b;
        brk_labels[brk_top] = (int)emit_JMPPH();
        ++brk_top;
      }
      *b4 = (int)(e + 1);
      emit_UpdateAddress(o4, emit_CurrentAddress());
      *++e = ADJ; *++e = 1; emit_ADJ(1);
      if (swdef) { *++e = JMP; *++e = (int)bdef; emit_JMP(odef); }
      else {
        if (brk_top >= BRK_MAX) { printf("%d: too many pending breaks\n", line); die(-1); }
        *++e = JMP; b = ++e;
        brk_eslots[brk_top] = (int)b;
        brk_labels[brk_top] = (int)emit_JMPPH();
        ++brk_top;
      }
      // fill the table: cases where present, else default, else end
      // (nothing else is emitted below, so end == the current address).
      // Each entry is a data-resident CODE patch; the pool word itself
      // stays 0, the loader writes the run-time address.
      i2 = 0;
      while (i2 <= swrange) {
        neg2 = 0;
        v2 = 0;
        while (v2 < swn) {
          if (swv[v2] == swmin + i2) {
            emit_DataPatch(PT_DCODE, (char *)(tlbl + i2) - data_s, swa[v2]);
            neg2 = 1;
            v2 = swn;
          }
          ++v2;
        }
        if (!neg2) {
          if (swdef) emit_DataPatch(PT_DCODE, (char *)(tlbl + i2) - data_s, (int)odef);
          else emit_DataPatch(PT_DCODE, (char *)(tlbl + i2) - data_s, (int)emit_CurrentAddress());
        }
        ++i2;
      }
    }
    // end: resolve the fallthrough jump and every pending break
    *bend = (int)(e + 1);
    emit_UpdateAddress(oend, emit_CurrentAddress());
    --brk_depth;
    while (brk_top > brk_base) {
      --brk_top;
      emit_UpdateAddress((int *)brk_labels[brk_top], emit_CurrentAddress());
      *(int *)brk_eslots[brk_top] = (int)(e + 1);
    }
    free(swv); free(swa);
  }
  else if (tk == For) {
    // for( initializers; condition; each-loop )
    //   [{ statement... } | statement];
    // initializers
    // a: condition
    //    bz d     // TODO
    //    jmp c    // TODO
    // b: each-loop
    //    jmp a
    // c: statement
    //    jmp b
    // d: out of loop
    if (tk == '(') next(); else { printf("%d: in 'for': open paren expected\n", line); die(-1); }
    expr(Assign); // Possibly compound
    a = e; // Save comparison start
    oa = emit_CurrentAddress();
    expr(Assign);
    *++e = BZ; b = ++e; // Skip loop if test fails
    ob = emit_BZPH();
    if (tk == ';') next(); else { printf("%d: in 'for': semicolon expected after initializers\n", line); die(-1); }
    // b: each-loop
    last_continue = curr_continue;
    curr_continue = ob + 1;
    brk_base = brk_top; ++brk_depth;
    expr(Assign);
    if (tk == ';') next(); else { printf("%d: in 'for': semicolon expected after condition\n", line); die(-1); }
    *++e = JMP; *++e = (int)a; // return to a
    emit_JMP(oa);
    if (tk == ')') next(); else { printf("%d: in 'for': close paren expected\n", line); die(-1); }
    stmt();
    *++e = JMP; *++e = (int)b; // return to b (each-loop)
    emit_JMP(ob);
    *b = (int)(e + 1); // Update end of loop address
    emit_UpdateAddress(ob, emit_CurrentAddress());
    curr_continue = last_continue;
    --brk_depth;
    while (brk_top > brk_base) {
      --brk_top;
      emit_UpdateAddress((int *)brk_labels[brk_top], emit_CurrentAddress());
      *(int *)brk_eslots[brk_top] = (int)(e + 1);
    }
  }
  else if (tk == While) {
    next();
    a = e + 1;
    oa = emit_CurrentAddress();
    last_continue = curr_continue;
    curr_continue = oa;
    brk_base = brk_top; ++brk_depth;
    if (tk == '(') next(); else { printf("%d: open paren expected\n", line); die(-1); }
    expr(Assign);
    if (tk == ')') next(); else { printf("%d: close paren expected (3)\n", line); die(-1); }
    *++e = BZ; b = ++e;
    ob = emit_BZPH();
    stmt();
    *++e = JMP; *++e = (int)a;
    emit_JMP(oa);
    *b = (int)(e + 1);
    emit_UpdateAddress(ob, emit_CurrentAddress());
    curr_continue = last_continue;
    --brk_depth;
    while (brk_top > brk_base) {
      --brk_top;
      emit_UpdateAddress((int *)brk_labels[brk_top], emit_CurrentAddress());
      *(int *)brk_eslots[brk_top] = (int)(e + 1);
    }
  }
  else if (tk == Return) {
    next();
    if (tk != ';') expr(Assign);
    *++e = LEV;
    emit_LEV();
    if (tk == ';') next(); else { printf("%d: semicolon expected\n", line); die(-1); }
  }
  else if (tk == '{') {
    next();
    while (tk != '}') stmt();
    next();
  }
  else if (tk == ';') {
    next();
  }
  else {
    expr(Assign);
    if (tk == ';') next(); else { printf("%d: semicolon expected (tk: '%c')\n", line, tk); die(-1); }
  }
}

int parse () {
  int bt, ty, i, attr, *fun, v, s;
  int *dcl, elem, neg;
  int pend, pcnt, pbval, peval;
  char *sstart;
  // parse declarations
  line = 1;
  line_start = statement_start = p;
  next();
  v = 0;
  s = 1;
  while (tk) {
    statement_start = p;
    bt = INT; // basetype
    attr = ATTR_NONE;
    if (tk == Static) { next(); attr = attr | ATTR_STATIC; }
    if (tk == Extern) { next(); attr = attr | ATTR_EXTERN; }
    if (tk == Int) next();
    else if (tk == Char) { next(); bt = CHAR; }
    else if (tk == Enum) {
      next();
      if (tk != '{') next();
      if (tk == '{') {
        next();
        i = 0;
        while (tk != '}') {
          if (tk != Id) { printf("%d: bad enum identifier %d\n", line, tk); die(-1); }
          next();
          if (tk == Assign) {
            next();
            if (tk == Sub) { next(); ival = -ival; } // Negative numbers
            if (tk != Num) { printf("%d: bad enum initializer\n", line); die(-1); }
            i = ival;
            next();
          }
          id[Class] = Num; id[Type] = INT; id[Val] = i++; id[Attr] = attr;
          if (tk == ',') next();
        }
        next();
      }
    }
    while (tk != ';' && tk != '}') {
      ty = bt;
      while (tk == Mul) { next(); ty = ty + PTR; }
      if (tk == Attribute) {
          next();
          if (tk == '(') next();     // (format)
          if (tk == '(') next();     // ((format))
          if (tk == Constructor)     // __attribute__(constructor)
            attr = attr | ATTR_CONSTRUCTOR;
          else if (tk == Destructor) // __attribute__(destructor)
            attr = attr | ATTR_DESTRUCTOR;
          else { printf("%d: unknown attribute, tk(%d,%c) != %d or %d\n", line, tk, tk, Constructor, Destructor); die(-1); }
		  next();     // ((format))
		  // printf("attributes set2, tk now == %d '%c', %.*s\n", tk, tk, 5, p - 5);
		  if (tk == ')') next();     // ((format))
		  // printf("attributes set3, tk now == %d '%c', %.*s\n", tk, tk, 5, p - 5);
		  tk = Id;
		  next();
      }
      if (tk != Id) {
		  printf("%d: bad global declaration\n", line); die(-1);
	  }
	  // TODO: re-enable?
      //if (id[Class]) { printf("%d: duplicate global definition\n", line); die(-1); }
      // Remember the declarator: lexing array sizes or initializers below
      // replaces the global 'id' with whatever identifier appears there.
      dcl = id;
      next();
      dcl[Type] = ty;
      dcl[Attr] = attr;
      if (tk == '(') { // function
        // keep track of function
		// printf("xxx, function definition\n");
        fun = dcl;
        fun[Class] = Fun;
        linits_n = 0;
        fun[Val] = (int)(e + 1);
        fun[emit_Val] = (int)emit_FunctionAddress();
        fun[ArgCount] = 0;
		//fun[emit_Length] = fun[Hemit_Length] = 0;
        //fun[Attr] = ATTR_NONE;
        next(); i = 0;
        while (tk != ')') {
          // printf("fun, tk now == '%c'\n", tk);
          ty = INT;
          if (tk == Static) { printf("%d: parameters cannot be marked static\n", line); die(-1); }
          if (tk == Int) next();
          else if (tk == Char) { next(); ty = CHAR; }
          while (tk == Mul) { next(); ty = ty + PTR; }
		  // printf("xxx, tk now == %d '%c', %.*s, Id == %d\n", tk, tk, 5, p - 5, Id);
          if (tk == '.') { // variadic function
            fun[Attr] = fun[Attr] | ATTR_VARIADIC;
            next(); if (tk != '.') { printf("%d: expected more dots\n", line); die(-1); }
            next(); if (tk != '.') { printf("%d: expected more dots\n", line); die(-1); }
			// Insert a fake parameter here so that parameter offsets are correct.
            // Do this by just pretending we encountered a parameter.
            id = id + Idsz;
            id[Name] = 0;
            id[Hash] = 0;
            id[HClass] = id[Class]; id[Class] = Loc;
            id[HType]  = id[Type];  id[Type] = ty;
            id[HVal]   = id[Val];
            id[Hemit_Val] = id[emit_Val]; id[emit_Val] = 0; // no effect
            //id[Hemit_Length] = id[emit_Length];
            id[HAttr]  = id[Attr];
            id[Val]    = i++;
          } else {
            if (tk != Id) { printf("%d: bad parameter declaration, tk = %d ('%c')\n", line, tk, tk); die(-1); }
            if (id[Class] == Loc) { printf("%d: duplicate parameter definition\n", line); die(-1); }
            id[HClass] = id[Class]; id[Class] = Loc;
            id[HType]  = id[Type];  id[Type] = ty;
            id[HVal]   = id[Val];
            id[Hemit_Val] = id[emit_Val]; id[emit_Val] = 0; // no effect
            //id[Hemit_Length] = id[emit_Length];
            id[HAttr]  = id[Attr];
            id[Val]    = i++;
          }
          next();
          if (tk == ',') next();
          // printf("(c4cc: tk now %c %ld)\n", tk, tk);
        }
        fun[ArgCount] = i;
        // printf("(c4cc: function declared with arg count: %d)\n", i);
        next();
        if (tk == ';') {
          // Mark as external, add a data word for it
          fun[Attr] = fun[Attr] | ATTR_EXTERN;
          fun[Val]  = (int)data;
          data = data + sizeof(int);
        } else {
          if (tk != '{') { printf("%d: bad function definition\n", line); die(-1); }
          emit_FunctionStart(fun);
          loc = ++i;
          next();
          // TODO: static function variables?
          while (tk == Int || tk == Char || tk == Static) {
            if (tk == Static) { printf("%d: static function variables not implemented\n", line); die(-1); }
            bt = (tk == Int) ? INT : CHAR;
            next();
            while (tk != ';') {
              ty = bt;
              while (tk == Mul) { next(); ty = ty + PTR; }
              if (tk != Id) { printf("%d: bad local declaration\n", line); die(-1); }
              if (id[Class] == Loc) { printf("%d: duplicate local definition\n", line); die(-1); }
              dcl = id; // lexing an array size below clobbers 'id'
              next();
              // Optional [size]: s elements. char arrays pack bytes into
              // whole words; the name evaluates to the address of the
              // LOWEST slot so indexing ascends.
              v = 0;    // becomes ATTR_ARRAY
              s = 1;    // element count; 0 = take it from the initializer
              elem = ty;
              pend = 0; // pending init: 1 braces, 2 char[] string, 3 word
              pcnt = 0;
              if (tk == Brak) {
                next();
                if (tk == Num) { s = ival; next(); }
                else if (tk == Id && id[Class] == Num) { s = id[Val]; next(); }
                else if (tk == ']') s = 0;
                else { printf("%d: bad array size\n", line); die(-1); }
                if (tk != ']') { printf("%d: expected ']' after array size\n", line); die(-1); }
                next();
                v = ATTR_ARRAY;
                ty = ty + PTR;
              }
              if (tk == Assign) {
                // Local initializers become stores emitted after ENT, so
                // they re-run on every entry to the function, as C
                // requires. Constants only, like globals.
                next();
                if (tk == '{') {
                  if (!v) { printf("%d: brace initializer requires an array\n", line); die(-1); }
                  next();
                  while (tk != '}') {
                    neg = 0;
                    if (tk == Sub) { next(); neg = 1; }
                    if (tk == Num) pbval = ival;
                    else if (tk == Id && id[Class] == Num) pbval = id[Val];
                    else { printf("%d: array initializers must be integer constants\n", line); die(-1); }
                    if (neg) pbval = -pbval;
                    next();
                    if (pcnt >= LTMP_MAX) { printf("%d: too many initializers\n", line); die(-1); }
                    if (s) { if (pcnt >= s) { printf("%d: too many initializers\n", line); die(-1); } }
                    ltmp[pcnt] = pbval;
                    ++pcnt;
                    if (tk == ',') next();
                  }
                  next();
                  if (!s) s = pcnt;
                  pend = 1;
                } else if (tk == '"') {
                  // The lexer has already copied the contents into the
                  // data pool at [ival, data); for a char array that copy
                  // is the template the stores read from
                  sstart = (char *)ival;
                  pcnt = data - sstart;
                  if (v && elem == CHAR) {
                    if (!s) s = pcnt + 1;
                    if (s < pcnt + 1) { printf("%d: string does not fit the array\n", line); die(-1); }
                    pend = 2;
                  } else if (ty == CHAR + PTR) {
                    pbval = (int)sstart;
                    peval = (int)sstart;
                    pend = 3;
                    ++data; // reserve the zeroed byte after the copy as the nul
                  } else { printf("%d: string initializer requires char* or char[]\n", line); die(-1); }
                  next();
                } else if (tk == And) {
                  next();
                  if (tk != Id || id[Class] != Fun || !id[emit_Val] || (id[Attr] & ATTR_EXTERN)) {
                    printf("%d: & initializer requires an already-defined function\n", line); die(-1);
                  }
                  pbval = id[emit_Val];
                  peval = id[Val];
                  pend = 3;
                  next();
                } else {
                  neg = 0;
                  if (tk == Sub) { next(); neg = 1; }
                  if (tk == Num) pbval = ival;
                  else if (tk == Id && id[Class] == Num) pbval = id[Val];
                  else { printf("%d: local initializers must be constants; assign in the statement block instead\n", line); die(-1); }
                  if (neg) pbval = -pbval;
                  if (v) { printf("%d: array initializer needs braces\n", line); die(-1); }
                  peval = pbval;
                  pend = 3;
                  next();
                }
              }
              if (!s) { printf("%d: array [] needs an initializer\n", line); die(-1); }
              dcl[HClass] = dcl[Class]; dcl[Class] = Loc;
              dcl[HType]  = dcl[Type];  dcl[Type] = ty;
              dcl[HVal]   = dcl[Val];
              dcl[Hemit_Val] = dcl[emit_Val]; dcl[emit_Val] = 0; // no effect
              dcl[Hemit_Length] = dcl[emit_Length];
              dcl[HAttr] = dcl[Attr];
              dcl[Attr] = v;
              if (v) dcl[emit_Length] = (elem == CHAR) ? s : s * sizeof(int);
              if (v && elem == CHAR) i = i + (s + sizeof(int) - 1) / sizeof(int);
              else i = i + s;
              dcl[Val] = i;
              // Queue the stores now the slot is known. Elements past the
              // initializer zero-fill, as C requires (the frame is not
              // otherwise cleared). neg reused as the element counter.
              if (pend == 1) {
                neg = 0;
                while (neg < s) {
                  if (linits_n >= LINIT_MAX) { printf("%d: too many initializers in one function\n", line); die(-1); }
                  linits[linits_n * LINIT__Sz + LINIT_KIND] = (elem == CHAR) ? 1 : 0;
                  linits[linits_n * LINIT__Sz + LINIT_VAL] = dcl[Val];
                  linits[linits_n * LINIT__Sz + LINIT_IDX] = neg;
                  linits[linits_n * LINIT__Sz + LINIT_BVAL] = (neg < pcnt) ? ltmp[neg] : 0;
                  linits[linits_n * LINIT__Sz + LINIT_EVAL] = (neg < pcnt) ? ltmp[neg] : 0;
                  ++linits_n;
                  ++neg;
                }
              } else if (pend == 2) {
                neg = 0;
                while (neg < s) {
                  if (linits_n >= LINIT_MAX) { printf("%d: too many initializers in one function\n", line); die(-1); }
                  linits[linits_n * LINIT__Sz + LINIT_KIND] = 1;
                  linits[linits_n * LINIT__Sz + LINIT_VAL] = dcl[Val];
                  linits[linits_n * LINIT__Sz + LINIT_IDX] = neg;
                  linits[linits_n * LINIT__Sz + LINIT_BVAL] = (neg < pcnt) ? sstart[neg] : 0;
                  linits[linits_n * LINIT__Sz + LINIT_EVAL] = (neg < pcnt) ? sstart[neg] : 0;
                  ++linits_n;
                  ++neg;
                }
              } else if (pend == 3) {
                if (linits_n >= LINIT_MAX) { printf("%d: too many initializers in one function\n", line); die(-1); }
                linits[linits_n * LINIT__Sz + LINIT_KIND] = 0;
                linits[linits_n * LINIT__Sz + LINIT_VAL] = dcl[Val];
                linits[linits_n * LINIT__Sz + LINIT_IDX] = 0;
                linits[linits_n * LINIT__Sz + LINIT_BVAL] = pbval;
                linits[linits_n * LINIT__Sz + LINIT_EVAL] = peval;
                ++linits_n;
              }
              if (tk == ',') next();
            }
            next();
          }
          *++e = ENT; *++e = i - loc; emit_ENT(i - loc);
          // Local initializer stores, re-run on every entry. Word stores
          // use LEA's constant offset; byte stores add the index at run
          // time since LEA only reaches word slots. IMM values of string
          // and function addresses pick up their relocation patches
          // automatically through emit_IMM.
          s = 0;
          while (s < linits_n) {
            v = s * LINIT__Sz;
            if (linits[v + LINIT_KIND]) {
              *++e = LEA; *++e = loc - linits[v + LINIT_VAL];
              emit_LEA(loc - linits[v + LINIT_VAL]);
              *++e = PSH; emit_PSH();
              *++e = IMM; *++e = linits[v + LINIT_IDX];
              emit_IMM(linits[v + LINIT_IDX]);
              *++e = ADD; emit_MATH(ADD);
              *++e = PSH; emit_PSH();
              *++e = IMM; *++e = linits[v + LINIT_EVAL];
              emit_IMM(linits[v + LINIT_BVAL]);
              *++e = SC; emit_SI(SC);
            } else {
              *++e = LEA; *++e = loc - linits[v + LINIT_VAL] + linits[v + LINIT_IDX];
              emit_LEA(loc - linits[v + LINIT_VAL] + linits[v + LINIT_IDX]);
              *++e = PSH; emit_PSH();
              *++e = IMM; *++e = linits[v + LINIT_EVAL];
              emit_IMM(linits[v + LINIT_BVAL]);
              *++e = SI; emit_SI(SI);
            }
            ++s;
          }
          linits_n = 0;
          while (tk != '}') stmt();
          if (*e != LEV) { *++e = LEV; emit_LEV(); }
          emit_FunctionEnd(fun);
        }
        id = sym; // unwind symbol table locals
        while (id[Tk]) {
          if (id[Class] == Loc) {
            id[Class] = id[HClass];
            id[Type] = id[HType];
            id[Val] = id[HVal];
            id[emit_Val] = id[Hemit_Val];
            id[Attr] = id[HAttr];
            id[emit_Length] = id[Hemit_Length];
          }
          id = id + Idsz;
        }
      } else { // if (tk == '(')
        // Global variable, possibly an array, possibly initialized.
        // elem is the element base type (before array promotion): char
        // arrays store bytes, everything else words.
        elem = ty;
        s = 1;
        v = 0;
        dcl[Class] = Glo;
        if (tk == Brak) { // [size], or [] with the size taken from the initializer
          next();
          if (tk == Num) { s = ival; next(); }
          else if (tk == Id && id[Class] == Num) { s = id[Val]; next(); }
          else if (tk == ']') s = 0;
          else { printf("%d: bad array size\n", line); die(-1); }
          if (tk != ']') { printf("%d: expected ']' after array size\n", line); die(-1); }
          next();
          ty = ty + PTR;
          dcl[Type] = ty;
          dcl[Attr] = dcl[Attr] | ATTR_ARRAY;
        }
        if (tk != Assign) {
          // Uninitialized: reserve zeroed storage
          if (!s) { printf("%d: array [] needs an initializer\n", line); die(-1); }
          data = (char *)(((int)data + sizeof(int) - 1) & (0 - sizeof(int)));
          dcl[Val] = (int)data;
          if ((dcl[Attr] & ATTR_ARRAY) && elem == CHAR) data = data + s;
          else data = data + sizeof(int) * s;
          if (dcl[Attr] & ATTR_ARRAY)
            dcl[emit_Length] = (elem == CHAR) ? s : s * sizeof(int);
        } else {
          next(); // past '='; NOTE: a string literal is copied into the
                  // data pool by this very next() call
          if (tk == '{') {
            // Brace list of integer constants (arrays only)
            if (!(dcl[Attr] & ATTR_ARRAY)) { printf("%d: brace initializer requires an array\n", line); die(-1); }
            data = (char *)(((int)data + sizeof(int) - 1) & (0 - sizeof(int)));
            dcl[Val] = (int)data;
            next();
            i = 0;
            while (tk != '}') {
              neg = 0;
              if (tk == Sub) { next(); neg = 1; }
              if (tk == Num) v = ival;
              else if (tk == Id && id[Class] == Num) v = id[Val];
              else { printf("%d: array initializers must be integer constants\n", line); die(-1); }
              if (neg) v = -v;
              next();
              if (s && i >= s) { printf("%d: too many initializers\n", line); die(-1); }
              if (elem == CHAR) *((char *)dcl[Val] + i) = v;
              else *((int *)dcl[Val] + i) = v;
              ++i;
              if (tk == ',') next();
            }
            next();
            if (!s) s = i;
            if (elem == CHAR) data = data + s;
            else data = data + sizeof(int) * s;
            dcl[emit_Length] = (elem == CHAR) ? s : s * sizeof(int);
          }
          else if (tk == '"') {
            // The lexer has already copied the contents to [ival, data)
            sstart = (char *)ival;
            if ((dcl[Attr] & ATTR_ARRAY) && elem == CHAR) {
              // char name[] = "...": the copied bytes ARE the array
              dcl[Val] = (int)sstart;
              i = data - sstart + 1; // + nul (pool is zeroed)
              if (!s) s = i;
              if (s < i) { printf("%d: string does not fit the array\n", line); die(-1); }
              data = sstart + s;
              dcl[emit_Length] = s;
              next();
            } else if (ty == CHAR + PTR) {
              // char *name = "...": a pointer word in data, relocated at
              // load time with a data-resident DATA patch. The lexer does
              // not terminate pool strings; reserve the zeroed byte after
              // the copy as the nul before anything else claims it.
              next();
              ++data;
              data = (char *)(((int)data + sizeof(int) - 1) & (0 - sizeof(int)));
              dcl[Val] = (int)data;
              *((int *)data) = (int)sstart; // dead compile-time value
              emit_DataPatch(PT_DDATA, (char *)dcl[Val] - data_s, sstart - data_s);
              data = data + sizeof(int);
            } else { printf("%d: string initializer requires char* or char[]\n", line); die(-1); }
          }
          else if (tk == And) {
            // int *name = &function: a code pointer in data, relocated
            // with a data-resident CODE patch. The target must already be
            // defined (its patch value is its code position).
            next();
            if (tk != Id || id[Class] != Fun || !id[emit_Val] || (id[Attr] & ATTR_EXTERN)) {
              printf("%d: & initializer requires an already-defined function\n", line); die(-1);
            }
            data = (char *)(((int)data + sizeof(int) - 1) & (0 - sizeof(int)));
            dcl[Val] = (int)data;
            *((int *)data) = id[emit_Val]; // dead compile-time value
            emit_DataPatch(PT_DCODE, (char *)dcl[Val] - data_s, id[emit_Val]);
            data = data + sizeof(int);
            next();
          }
          else {
            // Scalar integer constant: number, char literal, enum, negative
            neg = 0;
            if (tk == Sub) { next(); neg = 1; }
            if (tk == Num) v = ival;
            else if (tk == Id && id[Class] == Num) v = id[Val];
            else { printf("%d: initializer must be a constant\n", line); die(-1); }
            if (neg) v = -v;
            next();
            if (dcl[Attr] & ATTR_ARRAY) { printf("%d: array initializer needs braces\n", line); die(-1); }
            data = (char *)(((int)data + sizeof(int) - 1) & (0 - sizeof(int)));
            dcl[Val] = (int)data;
            *((int *)data) = v;
            data = data + sizeof(int);
          }
        }
      }
      if (tk == ',') next();
    }
    next();
  }
}
int *c4cc_compile (char *code) {
  // keep track of main
  id = idmain = idstart;
  parse();
  return (int *)idmain[Val];
}

// Dummy out stacktrace()
#undef stacktrace
#define stacktrace()

void print_symbol(int *i) {
  char *strc_a, *strc_b, *misc, *type;
  char *ptrstring;
  int   t, ptrcount;

  ptrstring = "****************";

  // Find symbol name length
  strc_a = strc_b = (char*)i[Name];
  while (iscvariable(*strc_b))
      ++strc_b;

  // Calculate type string
  if (i[Type] == CHAR) type = "char|void"; // void is represented as char
  else if(i[Type] & INT)  type = "int";
  else type = "char";
  t = i[Type];
  ptrcount = 0;
  while(t > PTR) { ++ptrcount; t = t - PTR; }
  ptrcount = ptrcount % 16; // Not more than ptrstring length
  //printf("ptrcount:%lld type:%lld\n", ptrcount, i[Type]);

  // Print depending on type
  if (i[Tk] == Id && i[Class] == Fun) {
    // Function: print type name() [address]
    printf("%s%.*s %.*s() [%p]", type, ptrcount, ptrstring, strc_b - strc_a, strc_a, (int*)i[Val]);
  } else if(i[Tk] > Id && i[Tk] < Brak) {
    // Builtin: just print name
    printf("builtin: %.*s", strc_b - strc_a, strc_a);
  } else if(i[Tk] == Id) {
    // Named symbols
    if (i[Class] == Sys) {
      // Functions converted to opcodes
      printf("opcode: %.*s = %d", strc_b - strc_a, strc_a, i[Val]);
    } else if(i[Class] == Glo || i[Class] == Loc || i[Class] == 0) {
        // Variables (globals and those on the stack)
        printf("%s%.*s %.*s @ %d (0x%X)", type, ptrcount, ptrstring, strc_b - strc_a, strc_a, i[Val], i[Val]);
        if (i[Class] == 0) printf(" (temporary)");
    } else if(i[Class] == Num) {
      // Enum: print its value
      printf("enum %.*s = %d / 0x%X", strc_b - strc_a, strc_a, i[Val], i[Val]);
    } else printf("(unknown object: %.*s)", strc_b - strc_a, strc_a);
  } else printf("(unknown object: %.*s)", strc_b - strc_a, strc_a);
}

void print_stacktrace (int *pc_orig, int *idmain, int *bp, int *sp) {
    int *pc, found, done, *idold, *t, range, depth;
    //int comparisons; // debug info

    pc = pc_orig + 1; // Gets decremented in main loop
    idold = id;
    done = 0;
    t = 0;
    depth = 0;

    while (!done) {
        found = 0;
        //comparisons = 0;
        while(!found) {
            id = idmain;
            --pc;
            while(!found && id[Tk]) {
                //++comparisons;
                if (id[Tk] == Id && id[Class] == Fun && pc == (int *)id[Val]) {
                    t = id;
                    found = 1;
                } else
                    id = id + Idsz;
            }
        }
        if (depth++) printf("%*s", depth - 1, " ");
        print_symbol(t);
        //printf(" (%d comparisons)", comparisons);
        printf("\n");
        // Finish when we encounter main
        if (t == idmain)
            done = found = 1;
        // find stack return addresss: simulate LEV
        sp = bp;
        bp = (int*)*sp++;
        pc = (int*)*sp++;
    }
}

int c4cc_init () {
  int i, poolsz, len;
  char *x;

  if(c4cc_initialized) return 0;
  //printf("c4cc_init, initialized=%lld\n", c4cc_initialized);
  c4cc_init_instructions();
  if(!(c4cc_emithandlers = malloc(sizeof(int) * EH__Sz))) { printf("Could not malloc(%d) emit area\n", sizeof(int) * EH__Sz); return -1; }
  // Set emit handlers to stubs
  i = 0;
  while(i < EH__Sz) c4cc_emithandlers[i++] = (int)&stub_emithandler;
  // Set default handlers
  c4cc_emithandlers[EH_INSRC_LINE] = (int)&stub_insource_line;
  c4cc_emithandlers[EH_SIZEOF_CHAR]= (int)&stub_sizeof_char;
  c4cc_emithandlers[EH_SIZEOF_INT] = (int)&stub_sizeof_int;
  c4cc_emithandlers[EH_FUNCTIONSTART] = (int)&stub_FunctionStart;
  c4cc_emithandlers[EH_FUNCTIONEND] = (int)&stub_FunctionEnd;

  poolsz = 512 * 1024;
  if (!(sym = _sym = malloc(poolsz))) { printf("could not malloc(%d) symbol area\n", poolsz); return -1; }
  if (!(le = e = _e = malloc(poolsz))) { printf("could not malloc(%d) text area\n", poolsz); return -1; }
  if (!(data = _data = malloc(poolsz))) { printf("could not malloc(%d) data area\n", poolsz); return -1; }
  if (!(_sp = malloc(poolsz))) { printf("could not malloc(%d) stack area\n", poolsz); return -1; }

  memset(sym,  0, poolsz);
  memset(e,    0, poolsz);
  memset(data, 0, poolsz);

  data_s = data;

  p = c4cc_keywords;
  i = Static; while (i <= Break) { next(); id[Tk] = i++; } // add keywords to symbol table
  i = OPEN; while (i <= FLT) { // add library to symbol table
    next(); id[Class] = Sys; id[Type] = INT; id[Val] = i++;
	if (0) { // don't be so verbose
		len = 0;
		x = (char *)id[Name];
		while (*x++ != ' ') ++len;
		printf("  builtin '%.*s' = %d\n", len, id[Name], id[Val]);
	}
  }

  next(); id[Tk] = Char; // handle void type
  next(); idstart = id;

  if (!(lp = p = _p = malloc(poolsz))) { printf("could not malloc(%d) source area\n", poolsz); return -1; }

  if (!(brk_labels = malloc(BRK_MAX * sizeof(int))) ||
      !(brk_eslots = malloc(BRK_MAX * sizeof(int)))) {
    printf("could not malloc break stacks\n");
    return -1;
  }
  brk_top = 0;
  brk_depth = 0;
  if (!(linits = malloc(LINIT_MAX * LINIT__Sz * sizeof(int))) ||
      !(ltmp = malloc(LTMP_MAX * sizeof(int)))) {
    printf("could not malloc initializer buffers\n");
    return -1;
  }
  linits_n = 0;

  c4cc_initialized = 1;

  return 0;
}

void c4cc_cleanup () {
  free(c4cc_emithandlers);
  free(_p);
  free(_sym);
  free(_e);
  free(_data);
  free(_sp);
}

int c4cc_argc;
char **c4cc_argv;
enum { STDIN, STDOUT, STDERR };
int c4cc_readargs (int argc, char **argv) {
  int poolsz;
  int r, i, fd;
  int use_stdin;

  poolsz = 256 * 1024;
  --argc; ++argv;
  if (argc > 0 && **argv == '-' && (*argv)[1] == 's') { src = 1; --argc; ++argv; }
  if (argc > 0 && **argv == '-' && (*argv)[1] == 'd') { debug = 1; --argc; ++argv; }
  if (argc < 1) { printf("usage: c4cc [-s] [-d] [-] file ...\n"); return -1; }

  //i = 0; printf("(C4CC) Argc: %lld\n", argc); while(i < argc) { printf("(C4CC) argv[%lld] = %s\n", i, *(argv + i)); ++i; }

  // Read all specified source files
  r = poolsz - 1;        // track memory remaining
  use_stdin = **argv == '-' && *(argv + 1) == 0;

  if (use_stdin) {
		printf("c4cc: Reading from standard input...\n");
	  while ((i = read(STDIN, p, r)) > 0) {
	  //printf("Content:\n%s\n%ld bytes read from standard input\n", p, i);
	    p = p + i;
		r = r - i;
      }
	  if (i < 0) {
		  printf("c4cc: failed to read from standard input\n");
		  return -1;
	  }
	  p[i] = 0;
	  --argc; ++argv;
  } else {
  while (r > 0 && argc >= 1 && **argv && **argv != '-' && *(*argv + 1) != '-') {
    //printf("open(%s) (argc=%lld)\n", *argv, argc);
    // Ask C4DOS first when there is one: its opener checks the RAM disk
    // before the real disk, so a source file another tool just produced
    // is findable. A transient's own open() only ever sees the host.
    i = dos_readable() ? dos_slurp(*argv, p, r) : -1;
    if (i >= 0) fd = -1;
    else {
      if ((fd = open(*argv, 0)) < 0) { printf("could not open(%s)\n", *argv); return -1; }
      if ((i = read(fd, p, r)) <= 0) { printf("read() returned %d\n", i); return -1; }
    }
    p[i] = 0;
    // Advance p to the nul we just wrote, new content will go here
    p = p + i;
    r = r - i;
    if (fd >= 0) close(fd);   // -1 when C4DOS supplied the source
    --argc; ++argv;
  }
  }
  // Reset pointer to start of code
  p = _p;

  c4cc_argc = argc;
  c4cc_argv = argv;

  return 0;
}

// Returns sp, with bp stored on it
int *c4_setupstack (int *sp, int poolsz, int argc, char **argv) {
  int *bp, *t;
  bp = sp = (int *)((int)sp + poolsz);
  *--sp = EXIT; // call exit if main returns
  *--sp = PSH; t = sp;
  *--sp = argc;
  *--sp = (int)argv;
  *--sp = (int)t;
  *--sp = (int)bp;
  return sp;
}

int c4cc_main(int argc, char **argv)
{
  int fd, poolsz;
  int i, *t, r;
  int *pc, *sp, *bp, a, status; // vm registers

  curr_continue = 0;

  //i = 0;
  //while(i < argc) { printf("(C4CC) argv[%lld] = %s\n", i, *(argv + i)); ++i; }
  if(c4cc_init()) { return -1; }
  sp = _sp;

  //printf("(C4CC) Argc: %lld\n", argc); i = 0; while(i < argc) { printf("(C4CC) Argv[%lld] = %s\n", i, argv[i]); ++i; }
  //printf("Reading args...\n");
  if(c4cc_readargs(argc, argv)) { return -1; }
  //printf("Arg read done\n");

  status = 0;
  poolsz = 256 * 1024; // TODO: stop using magic numbers

  //printf("//Compiling...\n");

  if (!(pc = c4cc_compile(p))) {
    printf("warning: main() not defined\n");
    pc = (int *)-1;
  }
  //printf("//Success\n");

  // use alt compiled
  //pc = (int*)idmain[emit_Val];

  // setup stack. leaves bp and sp on stack for us to grab
  //bp = sp = (int *)((int)_sp + poolsz);
  sp = c4_setupstack(_sp, poolsz, c4cc_argc, c4cc_argv);
  bp = (int *)*++sp;

  //printf("Passing argc=%lld\n", argc); i = 0; while(i < argc) { printf("(C4CC.2) argv[%lld] = %s\n", i, *(argv + i)); ++i; }

  if (src) {
    status = 0;
    //printf("\n\nFinal source layout:\n");
    emit_Done();
  }

  //printf("Done.\n");

  c4cc_cleanup();

  return status;
}

#ifndef C4CC_INCLUDED
int main(int argc, char **argv) { return c4cc_main(argc, argv); }
#endif
