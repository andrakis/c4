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
// New keywords:
//   extern
//   static
//   __attribute__(constructor)
//   __attribute__(destructor)
// 
// Example invocation:
// Using c4m, the multiloader:
//   ./c4 c4m.c c4cc.c asm-c4.c -- -s factorial.c
//   ./c4 c4m.c c4cc.c asm-js.c -- factorial.c > factorial.js && node factorial.js
// Using the compiled version: (run make)
//   ./c4r factorial.c
//   (Outputs to a.c4r)
//
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

#include "c4.h"

int *idstart, *idmain;
char *p, *lp, // current position in source code
     *data,   // data/bss pointer
     *data_s; // data start
char *c4cc_instructions; // Instructions as a char*
int  *c4cc_emithandlers; // Instruction emit handlers (see EH_*)

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
};

// tokens and classes (operators last and in precedence order)
enum {
  // Types
  Num = 128, Fun, Sys, Glo, Loc, Id,
  // Keywords and attributes
  Static, Extern, Attribute, Constructor, Destructor,
  Char, Else, Enum, If, Int, Return, Sizeof, While,
  // Operators
  Assign, Cond, Lor, Lan, Or, Xor, And, Eq, Ne, Lt, Gt, Le, Ge, Shl, Shr, Add, Sub, Mul, Div, Mod, Inc, Dec, Brak
};

char *c4cc_keywords;

// opcodes
enum { LEA ,IMM ,JMP ,JSR ,JSRI,JSRS,BZ  ,BNZ ,ENT ,ADJ ,LEV ,LI  ,LC  ,SI  ,SC  ,PSH ,
       JMPA,TLEV,
       OR  ,XOR ,AND ,EQ  ,NE  ,LT  ,GT  ,LE  ,GE  ,SHL ,SHR ,ADD ,SUB ,MUL ,DIV ,MOD ,
       OPEN,READ,CLOS,PRTF,MALC,RALC,FREE,MSET,MCMP,MCPY,STRC,ITH ,_OPC,_BLT,_TRP,
	   OPCD,_JMP,_ADJ,CSYS,C4CY,TIME,SIGH,SIGI,USLP,INFO,OPSL,
	   EXIT };
void c4cc_init_instructions() {
	c4cc_instructions = 
	   "LEA ,IMM ,JMP ,JSR ,JSRI,JSRS,BZ  ,BNZ ,ENT ,ADJ ,LEV ,LI  ,LC  ,SI  ,SC  ,PSH ,"
       "JMPA,TLEV,"
	   "OR  ,XOR ,AND ,EQ  ,NE  ,LT  ,GT  ,LE  ,GE  ,SHL ,SHR ,ADD ,SUB ,MUL ,DIV ,MOD ,"
	   "OPEN,READ,CLOS,PRTF,MALC,RALC,FREE,MSET,MCMP,MCPY,STRC,ITH ,_OPC,_BLT,_TRP,"
	   "OPCD,_JMP,_ADJ,CSYS,C4CY,TIME,SIGH,SIGI,USLP,INFO,OPSL,"
	   "EXIT,";
	c4cc_keywords = "static extern __attribute__ constructor destructor "
      "char else enum if int return sizeof while "
      "open read close printf malloc realloc free memset memcmp memcpy stacktrace "
      "install_trap_handler __opcode __builtin __c4_trap __c4_opcode "
      "__c4_jmp __c4_adjust __c4_configure __c4_cycles __time __c4_signal __c4_sigint "
	  "__c4_usleep __c4_info __c4_ops_list "
	  "exit void main";
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
       EH__Sz };

// types
enum { CHAR, INT, PTR };

// identifier offsets (since we can't create an ident struct)
enum { Tk, Hash, Name,
       Class, Type, Val, emit_Val, Attr, emit_Length,
       HClass, HType, HVal, Hemit_Val, HAttr, Hemit_Length,
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

/////
// Emitters
/////

void C4CC_PrintAccC4 () {
  while (le < e) {
    printf("%8.4s", &c4cc_instructions[*++le * 5]);
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

// 
void die (int exit_code) {
	char *c, *e;
	int   l;
	c = line_start;
	printf("%.*s", statement_start - line_start, line_start);
	// Print out the statement that caused the error
	c = statement_start;
	l = statement_start - line_start;
	printf("%.*s", p - c, c);
	// printf("(would print %d bytes", p - c);
	printf("/* <- here */");
	// Find end of line
	c = p;
	while(*c && *c != '\n') {
		++c;
	}
	// Print it
	printf("%.*s\n", c - p, p);
	// Print spacer
	printf("%*s^ here\n", l, " ");
	exit(exit_code);
}

/////
// C4 compiler
/////
void next()
{
  char *pp;

  while (tk = *p) {
    ++p;
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
      if (tk == '"') { ival = (int)pp; } else tk = Num;
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
  int t, *d, *d1, *d2;

  if (!tk) { printf("%d: unexpected eof in expression\n", line); die(-1); }
  else if (tk == Num) {
    *++e = IMM; *++e = ival;
    emit_IMM(ival);
    if (id[Attr] & id[ATTR_EXTERN]) { // load updated reference from data
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
    ty = INT; if (tk == Int) next(); else if (tk == Char) { next(); ty = CHAR; }
    while (tk == Mul) { next(); ty = ty + PTR; }
    if (tk == ')') next(); else { printf("%d: close paren expected in sizeof\n", line); die(-1); }
    *++e = IMM; *++e = (ty == CHAR) ? sizeof(char) : sizeof(int);
    emit_IMM((ty == CHAR) ? emit_sizeof_char() : emit_sizeof_int());
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
      else if (d[Class] == Fun) { *++e = JSR; *++e = d[Val]; emit_JSR(d); }
      // A C4 subroutine stored in a global variable
      else if (d[Class] == Glo) { *++e = JSRI; *++e = d[Val]; emit_JSRI((int *)d[Val]); } // Jump subroutine indirect
      // A C4 subroutine stored in a stack variable
      else if (d[Class] == Loc) { *++e = JSRS; *++e = loc - d[Val]; emit_JSRS(loc - d[Val]); } // Jump subroutine on stack
      else { printf("%d: bad function call (%d)\n", line, d[Class]); die(-1); }
      // Cleanup pushed arguments upon return
      if (t) { *++e = ADJ; *++e = t; emit_ADJ(t); }
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
      *++e = ((ty = d[Type]) == CHAR) ? LC : LI;
      emit_LI(*e);
    }
  }
  else if (tk == '(') {
    next();
    if (tk == Int || tk == Char) {
      t = (tk == Int) ? INT : CHAR; next();
      while (tk == Mul) { next(); t = t + PTR; }
      if (tk == ')') next(); else { printf("%d: bad cast\n", line); die(-1); }
      expr(Inc);
      ty = t;
    }
    else {
      expr(Assign);
      if (tk == ')') next(); else { printf("%d: close paren expected\n", line); die(-1); }
    }
  }
  else if (tk == Mul) {
    next(); expr(Inc);
    if (ty > INT) ty = ty - PTR; else { printf("%d: bad dereference\n", line); die(-1); }
    *++e = (ty == CHAR) ? LC : LI;
    emit_LI(*e);
  }
  else if (tk == And) {
    next(); expr(Inc);
    if (*e == LC || *e == LI) {
      if (*e == LC) emit_rewind_lc();
      else emit_rewind_li();
      --e;
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

  statement_start = p;

  if (tk == If) {
    next();
    if (tk == '(') next(); else { printf("%d: open paren expected\n", line); die(-1); }
    expr(Assign);
    if (tk == ')') next(); else { printf("%d: close paren expected\n", line); die(-1); }
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
  else if (tk == While) {
    next();
    a = e + 1;
    oa = emit_CurrentAddress();
    if (tk == '(') next(); else { printf("%d: open paren expected\n", line); die(-1); }
    expr(Assign);
    if (tk == ')') next(); else { printf("%d: close paren expected\n", line); die(-1); }
    *++e = BZ; b = ++e;
    ob = emit_BZPH();
    stmt();
    *++e = JMP; *++e = (int)a;
    emit_JMP(oa);
    *b = (int)(e + 1);
    emit_UpdateAddress(ob, emit_CurrentAddress());
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
  int bt, ty, i, attr, *fun;
  // parse declarations
  line = 1;
  line_start = statement_start = p;
  next();
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
		  // printf("xxx, tk now == %d '%c', %.*s, Id == %d\n", tk, tk, 5, p - 5, Id);
      if (tk != Id) {
		  printf("%d: unknown post attribute, tk(%d) != %d or %d\n", line, tk, Constructor, Destructor);
		  printf("%d: bad global declaration\n", line); die(-1);
	  }
	  // TODO: re-enable?
      //if (id[Class]) { printf("%d: duplicate global definition\n", line); die(-1); }
      next();
      id[Type] = ty;
      id[Attr] = attr;
	  // printf("xxx, attr set to %d\n", attr);
      if (tk == '(') { // function
        // keep track of function
		// printf("xxx, function definition\n");
        fun = id;
        id[Class] = Fun;
        id[Val] = (int)(e + 1);
        id[emit_Val] = (int)emit_FunctionAddress();
		//id[emit_Length] = id[Hemit_Length] = 0;
        //id[Attr] = ATTR_NONE;
        next(); i = 0;
        while (tk != ')') {
          ty = INT;
          if (tk == Static) { printf("%d: parameters cannot be marked static\n", line); die(-1); }
          if (tk == Int) next();
          else if (tk == Char) { next(); ty = CHAR; }
          while (tk == Mul) { next(); ty = ty + PTR; }
		  // printf("xxx, tk now == %d '%c', %.*s, Id == %d\n", tk, tk, 5, p - 5, Id);
          if (tk != Id) { printf("%d: bad parameter declaration, tk = %d ('%c')\n", line, tk, tk); die(-1); }
          if (id[Class] == Loc) { printf("%d: duplicate parameter definition\n", line); die(-1); }
          id[HClass] = id[Class]; id[Class] = Loc;
          id[HType]  = id[Type];  id[Type] = ty;
          id[HVal]   = id[Val];
          id[Hemit_Val] = id[emit_Val]; id[emit_Val] = 0; // no effect
		  //id[Hemit_Length] = id[emit_Length];
          id[HAttr]  = id[Attr];
          id[Val]    = i++;
          next();
          if (tk == ',') next();
        }
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
              id[HClass] = id[Class]; id[Class] = Loc;
              id[HType]  = id[Type];  id[Type] = ty;
              id[HVal]   = id[Val];
              id[Hemit_Val] = id[emit_Val]; id[emit_Val] = 0; // no effect
			  // id[Hemit_Length] = id[emit_Length];
              id[HAttr] = id[Attr];
              id[Val] = ++i;
              next();
              if (tk == ',') next();
            }
            next();
          }
          *++e = ENT; *++e = i - loc; emit_ENT(i - loc);
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
			// id[emit_Length] = id[Hemit_Length];
          }
          id = id + Idsz;
        }
      } else { // if (tk == '(')
        id[Class] = Glo;
        id[Val] = (int)data;
        data = data + sizeof(int);
      }
      if (tk == ',') next();
    }
    next();
  }
}
int *c4cc_compile (char *code) {
  // keep track of main
  id = idmain = idstart;
  parse(code);
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
  while ((*strc_b >= 'a' && *strc_b <= 'z') || (*strc_b >= 'A' && *strc_b <= 'Z') || (*strc_b >= '0' && *strc_b <= '9') || *strc_b == '_')
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
  int i, poolsz;

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
  i = Static; while (i <= While) { next(); id[Tk] = i++; } // add keywords to symbol table
  i = OPEN; while (i <= EXIT) { // add library to symbol table
    next(); id[Class] = Sys; id[Type] = INT; id[Val] = i++;
    // printf("  builtin '%.4s' = %d\n", id[Name], id[Val]);
  }
  next(); id[Tk] = Char; // handle void type
  next(); idstart = id;

  if (!(lp = p = _p = malloc(poolsz))) { printf("could not malloc(%d) source area\n", poolsz); return -1; }

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
	  if ((i = read(STDIN, p, r)) <= 0) {
		  printf("c4cc: failed to read from standard input\n");
		  return -1;
	  }
	  p[i] = 0;
	  --argc; ++argv;
  } else {
  while (r > 0 && argc >= 1 && **argv && **argv != '-' && *(*argv + 1) != '-') {
    //printf("open(%s) (argc=%lld)\n", *argv, argc);
    if ((fd = open(*argv, 0)) < 0) { printf("could not open(%s)\n", *argv); return -1; }
    if ((i = read(fd, p, r)) <= 0) { printf("read() returned %d\n", i); return -1; }
    p[i] = 0;
    // Advance p to the nul we just wrote, new content will go here
    p = p + i;
    r = r - i;
    close(fd);
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
