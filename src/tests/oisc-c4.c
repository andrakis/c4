// oisc-c4: Compiles C code directly to OISC4 code
// Many more than 4 functions are used, mainly to simplify code generations (DRY).

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

#include "oisc-min.c"

char *p, *lp, // current position in source code
     *data,   // data/bss pointer
     *data_s; // data start

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
int *oisc4_e, // Emitted OISC4 code
    *oisc4_le;// "                " last emit position
int *liptr,   // Points to last LI
    *lcptr;   // Points to last LC

int *OISC4;   // OISC4 registers. See following enum
enum {
    _PC,      //
    _SP,
    _BP,
    _A,
    _CYCLE,
    _RUN,
    _STATUS,
    _I,
    _T,
    __OISC4__Sz
};

// tokens and classes (operators last and in precedence order)
enum {
  Num = 128, Fun, Sys, Glo, Loc, Id,
  Char, Else, Enum, If, Int, Return, Sizeof, While,
  Assign, Cond, Lor, Lan, Or, Xor, And, Eq, Ne, Lt, Gt, Le, Ge, Shl, Shr, Add, Sub, Mul, Div, Mod, Inc, Dec, Brak
};

// opcodes
enum { LEA ,IMM ,JMP ,JSR ,JSRI,JSRS,BZ  ,BNZ ,ENT ,ADJ ,LEV ,LI  ,LC  ,SI  ,SC  ,PSH ,
       JMPA,TLEV,
       OR  ,XOR ,AND ,EQ  ,NE  ,LT  ,GT  ,LE  ,GE  ,SHL ,SHR ,ADD ,SUB ,MUL ,DIV ,MOD ,
       OPEN,READ,CLOS,PRTF,MALC,RALC,FREE,MSET,MCMP,MCPY,STRC,ITH ,_OPC,_BLT,_TRP,
       OPCD,_JMP,_ADJ,
       EXIT };

// types
enum { CHAR, INT, PTR };

// identifier offsets (since we can't create an ident struct)
enum { Tk, Hash, Name, Class, Type, Val, OISC4_Val, HClass, HType, HVal, Idsz };

/////
// Helper functions for OISC4
/////

// MOVREG(Reg1, Reg2)
// Helper to move one register into another
void OISC4_movreg (int *reg1, int *reg2) {
  *++oisc4_e = (int)reg1; *++oisc4_e = 0; *++oisc4_e = (int)reg2;
}
// MOVINT(Value, DestinationPtr)
void OISC4_movint (int val, int *dest) {
  *++oisc4_e = (int)&registers[Z]; *++oisc4_e = val; *++oisc4_e = (int)dest;
}
// OBSERVERVEREG(Reg)
// Read the value in Reg so that flags are updated
void OISC4_observereg(int *reg) {
  *++oisc4_e = (int)reg; *++oisc4_e = 0; *++oisc4_e = (int)reg;
}

// DEREFERENCE(Source, Dest)
// Load the value Source points to into Dest
void OISC4_dereference(int *source, int *dest) {
  int *d;
  //   source + 0 -> :0
  *++oisc4_e = (int)source; *++oisc4_e = 0; *++oisc4_e = PH; d = oisc4_e;
  //   PH:0   + 0 -> dest
  *++oisc4_e = PH; *d = (int)oisc4_e; *++oisc4_e  = 0; *++oisc4_e = (int)dest;
	// from, 0, :0
	//*++oisc4_e = (int)source; *++oisc4_e = 0; *++oisc4_e = (int)(oisc4_e + 5);
	// :0   PH      ; src=from
	//      0       ; add=
	//      to      ; dst=to
	//*++oisc4_e = PH; *++oisc4_e = 0; *++oisc4_e = (int)dest;
}

// DEREFERENCE(Source, Dest)
// Load the value Source points to into Dest, using char mode
void OISC4_dereference_char(int *source, int *dest) {
  int *d;
  //   source + 0 -> :0
  *++oisc4_e = (int)source; *++oisc4_e = 0; *++oisc4_e = PH; d = oisc4_e;
  // set word read to char
  // MOVINT(WM_CHAR, WR)
  OISC4_movint(WM_CHAR, &registers[WR]);
  //   PH:0   + 0 -> dest
  *++oisc4_e = PH; *d = (int)oisc4_e; *++oisc4_e  = 0; *++oisc4_e = (int)dest;
  // restore word read to int
  OISC4_movint(WM_INT, &registers[WR]);
  // observe the value so that final comparisons act consistently to
  // integer dereference.
  OISC4_observereg(dest);
}

// ADDREG(Source, Dest, Incr)
// Add a value to a register and store in Dest
void OISC4_addreg(int *source, int incr, int *dest) {
  *++oisc4_e = (int)source; *++oisc4_e = incr; *++oisc4_e = (int)dest;
}

// ADDREGS(Reg1, Reg2, Dest)
// Add two registers together.
void OISC4_addregs (int *reg1, int *reg2, int *dest) {
  int *d;
  // Two instructions:
  //  Move reg2+0 into :1
  *++oisc4_e = (int)reg2; *++oisc4_e = 0; *++oisc4_e = PH; d = oisc4_e;
  //  Move reg1+reg2 into dest
  *++oisc4_e = (int)reg1; *++oisc4_e = PH; *d = (int)oisc4_e; *++oisc4_e = (int)dest;
}

// PREINC(Source, Dest, Incr)
// Store Source into Dest, then increment Source by Incr
void OISC4_preinc (int *source, int *dest, int incr) {
  //   source + incr -> source
  OISC4_addreg(source, incr, source);
  //   source + 0 -> dest
  OISC4_movreg(source, dest);
}

// POSTINC(Source, Dest, Incr)
// Increment Source by Incr, then store result into Dest
void OISC4_postinc (int *source, int *dest, int incr) {
  //   source + 0 -> dest
  OISC4_movreg(source, dest);
  //   source + incr -> source
  OISC4_addreg(source, incr, source);
}

/////
// OISC4 Emitters
/////

// LEA: a = bp + pcval
void OISC4_LEA (int pcval) {
  OISC4_addreg(&OISC4[_BP], pcval * sizeof(int), &OISC4[_A]);
}

// IMM : a = *pc++;
// OISC: a = val
void OISC4_IMM (int val) {
  OISC4_movint(val, &OISC4[_A]);
}

// LI: a = *(int *)a
// LC: a = *(char *)a;
void OISC4_LI (int mode) {
  if(mode == LI) { liptr = oisc4_e; OISC4_dereference(&OISC4[_A], &OISC4[_A]); }
  else if(mode == LC) { lcptr = oisc4_e; OISC4_dereference_char(&OISC4[_A], &OISC4[_A]); }
  else {
    printf("OISC4_LI: bad mode %lld (should be %lld or %lld)\n", mode, LI, LC);
    exit(-1);
  }
}
void OISC4_rewind_li () { oisc4_e = liptr; }
void OISC4_rewind_lc () { oisc4_e = lcptr; }

// SI  : *(int *)*sp++ = a;
void OISC4_SI (int mode) {
  int *d;
  if (mode != SI && mode != SC) {
    printf("OISC4_SI: bad mode %lld (should be %lld or %lld)\n", mode, LI, LC);
    exit(-1);
  }
  OISC4_postinc(&OISC4[_SP], &registers[R0], sizeof(int));
  OISC4_dereference(&registers[R0], &registers[R0]);
  OISC4_dereference(&registers[R0], &registers[R0]); // TODO: needed?
  // R0 + 0 -> :0
  *++oisc4_e = (int)&registers[R0]; *++oisc4_e = 0; *++oisc4_e = PH; d = oisc4_e;
  if(mode == SC) OISC4_movint(WM_CHAR, &registers[WW]);
  // A + 0 -> PH:0
  *++oisc4_e = (int)&OISC4[_A]; *++oisc4_e = 0; *++oisc4_e = PH; *d = (int)oisc4_e;
  if(mode == SC) OISC4_movint(WM_INT, &registers[WW]);
}

// PSH: *--sp = a;
void OISC4_PSH () {
  int *d;
  // *--sp = A;
  //   SP - 1 -> SP
  OISC4_addreg(&OISC4[_SP], (int)(sizeof(int)) * -1, &OISC4[_SP]);
  //   SP + 0 -> :2
  *++oisc4_e = (int)&OISC4[_SP]; *++oisc4_e = 0; *++oisc4_e = PH; d = oisc4_e;
  //   A  + 0 -> PH:2
  *++oisc4_e = (int)&OISC4[_A];  *++oisc4_e = 0; *++oisc4_e = PH; *d = (int)oisc4_e;
}

// JMP : pc = (int *)*pc;
// OISC: pc = loc
void OISC4_JMP (int *loc) {
  // Z + loc -> PC
  OISC4_movint((int)loc, &registers[PC]);
}
int *OISC4_JMPPH() {
  int *r;
  // Z + PH -> PC
  *++oisc4_e = (int)&registers[Z]; *++oisc4_e = PH; r = oisc4_e; *++oisc4_e = (int)&registers[PC];
  return r;
}

// JSR : *--sp = (int)(pc + 1); pc = (int *)pc*; }
// OISC: *--sp = oisc4_e + INSTR_SIZE; PC = loc
void OISC4_JSR (int *loc) {
  int *after, *d;
  // --sp
  *++oisc4_e = (int)&OISC4[_SP]; *++oisc4_e = ((int)sizeof(int)) * -1; *++oisc4_e = (int)&OISC4[_SP];
  // *sp =
  //   SP + 0 -> :2
  *++oisc4_e = (int)&OISC4[_SP]; *++oisc4_e = 0; *++oisc4_e = PH; d = oisc4_e;
  //   0: Z 1: PH:after 2: PH:2
  *++oisc4_e = (int)&registers[Z]; *++oisc4_e = PH; after = oisc4_e; *++oisc4_e = PH; *d = (int)oisc4_e;
  // pc = loc;
  //   Z + loc -> PC
  *++oisc4_e = (int)&registers[Z]; *++oisc4_e = (int)loc; *++oisc4_e = (int)&registers[PC];

  *after = (int)(oisc4_e + 1);
}

// JSRI: *--sp = (int)(pc + 1); pc = (int *)*pc; pc = (int *)*pc
// OISC: --SP; *SP = PH:after;  pc = DEREFERENCE(DEREFERENCE(loc))
void OISC4_JSRI(int *loc) {
  int *after, *d;

  // --sp
  *++oisc4_e = (int)&OISC4[_SP]; *++oisc4_e = ((int)sizeof(int)) * -1; *++oisc4_e = (int)&OISC4[_SP];
  // *sp =
  //   SP + 0 -> :2
  *++oisc4_e = (int)&OISC4[_SP]; *++oisc4_e = 0; *++oisc4_e = PH; d = oisc4_e;
  // oisc4_e + INSTR_SIZE;
  //   0: Z 1: PH:after 2: PH:2
  *++oisc4_e = (int)&registers[Z]; *++oisc4_e = PH; after = oisc4_e; *++oisc4_e = PH; *d = (int)oisc4_e;
  // Z + loc -> :0
  *++oisc4_e = (int)&registers[Z]; *++oisc4_e = (int)loc; *++oisc4_e = PH; d = oisc4_e;
  // 0: PH + 0 -> :1
  *++oisc4_e = PH; *d = (int) oisc4_e; *++oisc4_e = 0; *++oisc4_e = PH; d = oisc4_e;
  // 1: PH + 0 -> PC
  *++oisc4_e = PH; *d = (int) oisc4_e; *++oisc4_e = 0; *++oisc4_e = (int)&registers[PC];
  // Update PH:after
  *after = (int)(oisc4_e + 1);
}
// *--sp = (int)(pc + 1); pc = (int *)*(bp + *pc++);
void OISC4_JSRS(int *loc) {
  int *after, *d, *t;

  // --sp
  *++oisc4_e = (int)&OISC4[_SP]; *++oisc4_e = ((int)sizeof(int)) * -1; *++oisc4_e = (int)&OISC4[_SP];
  // *sp =
  //   SP + 0 -> :2
  *++oisc4_e = (int)&OISC4[_SP]; *++oisc4_e = 0; *++oisc4_e = PH; d = oisc4_e;
  // oisc4_e + INSTR_SIZE;
  //   0: Z 1: PH:after 2: PH:2
  *++oisc4_e = (int)&registers[Z]; *++oisc4_e = PH; after = oisc4_e; *++oisc4_e = PH; *d = (int)oisc4_e;
  // Z + loc -> :0
  *++oisc4_e = (int)&registers[Z]; *++oisc4_e = (int)loc; *++oisc4_e = PH; d = oisc4_e;
  // BP + 0 -> PH:3
  *++oisc4_e = (int)&OISC4[_BP]; *++oisc4_e = 0; *++oisc4_e = PH; *d = (int)oisc4_e;
  // 0: PH + PH:3(BP) -> :1
  *++oisc4_e = PH; *d = (int) oisc4_e; *++oisc4_e = PH; *d = (int)oisc4_e; *++oisc4_e = PH; d = oisc4_e;
  // 1: PH + 0 -> PC
  *++oisc4_e = PH; *d = (int) oisc4_e; *++oisc4_e = 0; *++oisc4_e = (int)&registers[PC];
  // Update PH:after
  *after = (int)(oisc4_e + 1);
}

// BZ  : pc = a ? (pc + 1) : (int *)*pc;
// OISC: if(a) pc = loc;
int *OISC4_BZPH() {
  int *ph0, *neq0, *eq0, *r;
  // OBSERVE A
  *++oisc4_e = (int)&OISC4[_A]; *++oisc4_e = 0; *++oisc4_e = (int)&OISC4[_A];
  // EQ0 + :0 -> PC
  *++oisc4_e = (int)&registers[EQ0];
  *++oisc4_e = PH; ph0 = oisc4_e;
  *++oisc4_e = (int)&registers[PC];
  // 0: Z + :neq0 -> PC
  *ph0 = (int)(oisc4_e + 1); *++oisc4_e = (int)&registers[Z];
  *++oisc4_e = PH; neq0 = oisc4_e;
  *++oisc4_e = (int)&registers[PC];
  // 1: Z + :eq0  -> PC
                      *++oisc4_e = (int)&registers[Z];
  *++oisc4_e = PH; eq0 = oisc4_e;
  *++oisc4_e = (int)&registers[PC];
  // eq0:
  *eq0 = (int)(oisc4_e);
  //   Z + PH -> PC
  *++oisc4_e = (int)&registers[Z];
  *++oisc4_e = (int)PH; r = oisc4_e;
  *++oisc4_e = (int)&registers[PC];
  // neq0:
  *neq0 = (int)(oisc4_e + 1);
  return r;
}
// BNZ : pc = a ? (int *)*pc : (pc + 1);
// OISC: if(!a) pc = loc;
int *OISC4_BNZPH() {
  int *ph0, *neq0, *eq0, *r;
  // OBSERVE A
  *++oisc4_e = (int)&OISC4[_A]; *++oisc4_e = 0; *++oisc4_e = (int)&OISC4[_A];
  // EQ0 + :0 -> PC
  *++oisc4_e = (int)&registers[EQ0];
  *++oisc4_e = PH; ph0 = oisc4_e;
  *++oisc4_e = (int)&registers[PC];
  // 0: Z + :neq0 -> PC
  *ph0 = (int)(oisc4_e + 1); *++oisc4_e = (int)&registers[Z];
  *++oisc4_e = PH; neq0 = oisc4_e;
  *++oisc4_e = (int)&registers[PC];
  // 1: Z + :eq0  -> PC
                      *++oisc4_e = (int)&registers[Z];
  *++oisc4_e = PH; eq0 = oisc4_e;
  *++oisc4_e = (int)&registers[PC];
  // neq0:
  *neq0 = (int)(oisc4_e + 1);
  //   Z + PH -> PC
  *++oisc4_e = (int)&registers[Z];
  *++oisc4_e = (int)PH; r = oisc4_e;
  *++oisc4_e = (int)&registers[PC];
  // eq0:
  *eq0 = (int)(oisc4_e + 1);
  return r;
}
void OISC4_ENT(int adj) {
  int *d;
  // *--sp = bp;
  //  --sp
  //    SP -1 -> SP
  OISC4_addreg(&OISC4[_SP], ((int)sizeof(int)) * -1, &OISC4[_SP]);
  // *sp   =
  //   SP + 0 -> :0
  *++oisc4_e = (int)&OISC4[_SP]; *++oisc4_e = 0; *++oisc4_e = PH; d = oisc4_e;
  //         bp;
  //   BP + 0 -> PH:0
  *++oisc4_e = (int)&OISC4[_BP]; *++oisc4_e = 0; *++oisc4_e = PH; *d = (int)oisc4_e;
  // bp = sp
  OISC4_movreg(&OISC4[_SP], &OISC4[_BP]);
  if (adj) {
	  // sp = sp - adj
	  OISC4_addreg(&OISC4[_SP], -adj * sizeof(int), &OISC4[_SP]);
  }
  return;

  // old fashioned method
  // *--sp = (int)bp;
  // SP + -1 -> SP
  *++oisc4_e = (int)&OISC4[_SP]; *++oisc4_e = ((int)sizeof(int)) * -1; *++oisc4_e = (int)&OISC4[_SP];
  // *  sp = bp
  // BP + 0 -> [SP]
  //   SP + 0 -> :0
  *++oisc4_e = (int)&OISC4[_SP]; *++oisc4_e = 0; *++oisc4_e = PH; d = oisc4_e;
  //   BP + 0 -> 0: PH
  *++oisc4_e = (int)&OISC4[_BP]; *++oisc4_e = 0; *++oisc4_e = PH; *d = (int)oisc4_e;
  // bp = sp;
  // BP + 0 -> SP
  *++oisc4_e = (int)&OISC4[_BP]; *++oisc4_e = 0; *++oisc4_e = (int)&OISC4[_SP];
  // sp = sp - *pc++ (or adj)
  // SP - ADJ -> SP
  *++oisc4_e = (int)&OISC4[_SP]; *++oisc4_e = -adj; *++oisc4_e = (int)&OISC4[_SP];
}

// ADJ : sp = sp + *pc++
// OISC: SP + adj -> SP
void OISC4_ADJ(int adj) {
  OISC4_addreg(&OISC4[_SP], adj * sizeof(int), &OISC4[_SP]);
}

// LEV : sp = bp; bp = (int *)*sp++; pc = (int *)sp++;
void OISC4_LEV() {
  OISC4_movreg(&OISC4[_BP], &OISC4[_SP]);
  OISC4_postinc(&OISC4[_SP], &OISC4[_BP], sizeof(int));
  OISC4_dereference(&OISC4[_BP], &OISC4[_BP]);
  OISC4_postinc(&OISC4[_SP], &registers[R0], sizeof(int));
  OISC4_dereference(&registers[R0], &registers[PC]);
}

// Setup for syscall:
// Write OISC4[_SP] to R0
void OISC4_SYSCALL(int num) {
  // Copy _SP to R0
  // SP + 0 -> R0
  //*++oisc4_e = (int)&OISC4[_SP]; *++oisc4_e = 0; *++oisc4_e = (int)&registers[R0];
  OISC4_movreg(&OISC4[_SP], &registers[R0]);
  // Z + num -> IO_SYSCALL
  //*++oisc4_e = (int)&registers[Z]; *++oisc4_e = num; *++oisc4_e = IO_SYSCALL;
  OISC4_movint(num, (int *)IO_SYSCALL);
  // IO_SYSCALL -> A
  OISC4_movreg((int *)IO_SYSCALL, &OISC4[_A]);
}

void OISC4_MATH(int operation) {
  // X = *sp++
  OISC4_postinc(&OISC4[_SP], &registers[R0], sizeof(int));
  OISC4_dereference(&registers[R0], (int *)IO_MATH_X);
  // Y = a
  OISC4_movreg(&OISC4[_A], (int *)IO_MATH_Y);
  // Op = operation (subtract first C4 math operator from it to get correct value)
  OISC4_movint(operation - OR, (int *)IO_MATH_OP);
  // A = Math.V
  OISC4_movreg((int *)IO_MATH_V, &OISC4[_A]);
}

void OISC4_Debug_PrintSymbol(int *s) {
	char *m;
       if(s == &OISC4[_PC])     m = "oisc4.pc    ";
  else if(s == &OISC4[_SP])     m = "oisc4.sp    ";
  else if(s == &OISC4[_BP])     m = "oisc4.bp    ";
  else if(s == &OISC4[_A])      m = "oisc4.a     ";
  else if(s == &OISC4[_CYCLE])  m = "oisc4.cycle ";
  else if(s == &OISC4[_RUN])    m = "oisc4.run   ";
  else if(s == &OISC4[_STATUS]) m = "oisc4.status";
  else if(s == &OISC4[_I])      m = "oisc4.i     ";
  else if(s == &OISC4[_T])      m = "oisc4.t     ";
  else if((char *)s >= data_s && (char *)s <= data) {
                             printf("[data + %llx]", (char *)s - data_s);
    return;
  } else { Debug_PrintSymbolC4(s); return; }
  printf("%s", m);
}

/////
// C4
/////
void next()
{
  char *pp;

  while (tk = *p) {
    ++p;
    if (tk == '\n') {
      if (src) {
        printf("%d: %.*s", line, p - lp, lp);
        lp = p;
        while (le < e) {
          if (*++le <= EXIT) {
              printf("%d", *le);
              printf("%8.4s", &"LEA ,IMM ,JMP ,JSR ,JSRI,JSRS,BZ  ,BNZ ,ENT ,ADJ ,LEV ,LI  ,LC  ,SI  ,SC  ,PSH ,"
               "JMPA,TLEV,"
               "OR  ,XOR ,AND ,EQ  ,NE  ,LT  ,GT  ,LE  ,GE  ,SHL ,SHR ,ADD ,SUB ,MUL ,DIV ,MOD ,"
               "OPEN,READ,CLOS,PRTF,MALC,RALC,FREE,MSET,MCMP,MCPY,STRC,ITH ,_OPC,_BLT,_TRP,"
               "OPCD,_JMP,_ADJ,EXIT,"[*le * 5]);
          } else {
              printf("unknown (%d)", *le);
          }
          if (*le <= ADJ) printf(" %llx\n", *++le); else printf("\n");
        }
        while (oisc4_le < oisc4_e) {
          printf("   %.12llx:%c%c", (int)oisc4_le, 9, 9);
          OISC4_Debug_PrintSymbol((int *)*oisc4_le++);
          printf(" + ");
          if(*oisc4_le == 0) printf("           0");
          else if(*oisc4_le < 10000) printf("%12.lld", *oisc4_le);
          else printf("%12.llx", *oisc4_le);
          oisc4_le++;
          printf("%c-> ", 9);
          OISC4_Debug_PrintSymbol((int *)*oisc4_le++);
          printf("\n");
        }
      }
      ++line;
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
        ++p;
        while (*p != 0 && *p != '\n') ++p;
      }
      else {
        tk = Div;
        return;
      }
    }
    else if (tk == '\'' || tk == '"') {
      pp = data;
      while (*p != 0 && *p != tk) {
        if ((ival = *p++) == '\\') {
          if ((ival = *p++) == 'n') ival = '\n';
        }
        if (tk == '"') *data++ = ival;
      }
      ++p;
      if (tk == '"') ival = (int)pp; else tk = Num;
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
  int t, *d, *o;

  if (!tk) { printf("%d: unexpected eof in expression\n", line); exit(-1); }
  else if (tk == Num) {
    *++e = IMM; *++e = ival;
    OISC4_IMM(ival);
    next(); ty = INT;
  }
  else if (tk == '"') {
    *++e = IMM; *++e = ival;
    OISC4_IMM(ival);
    next();
    while (tk == '"') next();
    data = (char *)((int)data + sizeof(int) & -sizeof(int)); ty = PTR;
  }
  else if (tk == Sizeof) {
    next(); if (tk == '(') next(); else { printf("%d: open paren expected in sizeof\n", line); exit(-1); }
    ty = INT; if (tk == Int) next(); else if (tk == Char) { next(); ty = CHAR; }
    while (tk == Mul) { next(); ty = ty + PTR; }
    if (tk == ')') next(); else { printf("%d: close paren expected in sizeof\n", line); exit(-1); }
    *++e = IMM; *++e = (ty == CHAR) ? sizeof(char) : sizeof(int);
    OISC4_IMM(*e);
    ty = INT;
  }
  else if (tk == Id) {
    d = id; next();
    if (tk == '(') {
      next();
      t = 0;
      while (tk != ')') { expr(Assign); *++e = PSH; OISC4_PSH(); ++t; if (tk == ',') next(); }
      next();
      // A syscall, ie all the builtin functions like open,read,etc
      if (d[Class] == Sys) {
        *++e = d[Val];
        // Special handling for PRTF: write number of arguments to R1
        // C4's PRTF peeks at the ADJ (X) following this syscall.
        if(d[Val] == PRTF) {
          // Z + t -> R1
          *++oisc4_e = (int)&registers[Z]; *++oisc4_e = t; *++oisc4_e = (int)&registers[R1];
        }
        // OPEN is the first syscall
        OISC4_SYSCALL(d[Val] - OPEN);
      }
      // A C4 subroutine
      else if (d[Class] == Fun) { *++e = JSR; *++e = d[Val]; OISC4_JSR((int *)d[OISC4_Val]); }
      // A C4 subroutine stored in a global variable
      else if (d[Class] == Glo) { *++e = JSRI; *++e = d[Val]; OISC4_JSRI((int *)d[OISC4_Val]); } // Jump subroutine indirect
      // A C4 subroutine stored in a stack variable
      else if (d[Class] == Loc) { *++e = JSRS; *++e = loc - d[Val]; OISC4_JSRS((int *)(loc - d[Val])); } // Jump subroutine on stack
      else { printf("%d: bad function call (%d)\n", line, d[Class]); exit(-1); }
      // Cleanup pushed arguments upon return
      if (t) { *++e = ADJ; *++e = t; OISC4_ADJ(t); }
      ty = d[Type];
    } else if (d[Class] == Num) {
      *++e = IMM; *++e = d[Val]; ty = INT;
      OISC4_IMM(d[Val]);
    } else {
      if (d[Class] == Loc) {        // Local variable
        *++e = LEA; *++e = loc - d[Val];
        OISC4_LEA(loc - d[Val]);
      } else if (d[Class] == Glo) { // Global variable
        *++e = IMM; *++e = d[Val];
        OISC4_IMM(d[Val]);
      } else if (d[Class] == Fun) { // Function address
        *++e = IMM; *++e = d[Val];
        OISC4_IMM(d[OISC4_Val]);
      } else { printf("%d: undefined variable\n", line); exit(-1); }
      *++e = ((ty = d[Type]) == CHAR) ? LC : LI;
      OISC4_LI(*e);
    }
  }
  else if (tk == '(') {
    next();
    if (tk == Int || tk == Char) {
      t = (tk == Int) ? INT : CHAR; next();
      while (tk == Mul) { next(); t = t + PTR; }
      if (tk == ')') next(); else { printf("%d: bad cast\n", line); exit(-1); }
      expr(Inc);
      ty = t;
    }
    else {
      expr(Assign);
      if (tk == ')') next(); else { printf("%d: close paren expected\n", line); exit(-1); }
    }
  }
  else if (tk == Mul) {
    next(); expr(Inc);
    if (ty > INT) ty = ty - PTR; else { printf("%d: bad dereference\n", line); exit(-1); }
    *++e = (ty == CHAR) ? LC : LI;
    OISC4_LI(*e);
  }
  else if (tk == And) {
    next(); expr(Inc);
    if (*e == LC || *e == LI) {
      if (*e == LC) OISC4_rewind_lc();
      else OISC4_rewind_li();
      --e;
    } else { printf("%d: bad address-of\n", line); exit(-1); }
    ty = ty + PTR;
  }
  else if (tk == '!') {
    next(); expr(Inc);
    *++e = PSH; OISC4_PSH();
    *++e = IMM; *++e = 0; OISC4_IMM(0);
    *++e = EQ; OISC4_MATH(EQ);
    ty = INT;
  }
  else if (tk == '~') {
    next(); expr(Inc);
    *++e = PSH; OISC4_PSH();
    *++e = IMM; *++e = -1; OISC4_IMM(-1);
    *++e = XOR; OISC4_MATH(XOR);
    ty = INT;
  }
  else if (tk == Add) { next(); expr(Inc); ty = INT;  }
  else if (tk == Sub) {
    next(); *++e = IMM;
    if (tk == Num) {
      *++e = -ival; OISC4_IMM(-ival);
      next();
    } else {
      *++e = -1; OISC4_IMM(-1);
      *++e = PSH; OISC4_PSH();
      expr(Inc);
      *++e = MUL; OISC4_MATH(MUL);
    }
    ty = INT;
  }
  else if (tk == Inc || tk == Dec) {
    t = tk; next(); expr(Inc);
    if (*e == LC) {
      *e = PSH; *++e = LC;
      OISC4_rewind_lc();
      OISC4_PSH();
      OISC4_LI(LC);
    }
    else if(*e == LI) {
      *e = PSH; *++e = LI;
      OISC4_rewind_li();
      OISC4_PSH();
      OISC4_LI(LI);
    }
    else { printf("%d: bad lvalue in pre-increment\n", line); exit(-1); }
    *++e = PSH;
    OISC4_PSH();
    *++e = IMM; *++e = (ty > PTR) ? sizeof(int) : sizeof(char);
    OISC4_IMM(*e);
    *++e = (t == Inc) ? ADD : SUB;
    OISC4_MATH(*e);
    *++e = (ty == CHAR) ? SC : SI;
    OISC4_SI(*e);
  }
  else { printf("%d: bad expression\n", line); exit(-1); }

  while (tk >= lev) { // "precedence climbing" or "Top Down Operator Precedence" method
    t = ty;
    if (tk == Assign) {
      next();
      if (*e == LC || *e == LI) {
        if (*e == LC) OISC4_rewind_lc();
        else OISC4_rewind_li();
        *e = PSH;
        OISC4_PSH();
      } else { printf("%d: bad lvalue in assignment\n", line); exit(-1); }
      expr(Assign);
      *++e = ((ty = t) == CHAR) ? SC : SI;
      OISC4_SI(*e);
    }
    else if (tk == Cond) {
      next();
      *++e = BZ; d = ++e; o = OISC4_BZPH();
      expr(Assign);
      if (tk == ':') next(); else { printf("%d: conditional missing colon\n", line); exit(-1); }
      *d = (int)(e + 3); *++e = JMP; d = ++e;
      *o = (int)(oisc4_e + 3); o = OISC4_JMPPH(); // TODO: ensure offsets correct
      expr(Cond);
      *d = (int)(e + 1);                          // TODO: as above
      *o = (int)(oisc4_e + 3);
    }
    else if (tk == Lor) {
      next(); *++e = BNZ; d = ++e;
      o = OISC4_BNZPH();
      expr(Lan);
      *d = (int)(e + 1);
      *o = (int)(oisc4_e + 3);                    // TODO: as above
      ty = INT;  }
    else if (tk == Lan) {
      next(); *++e = BZ;  d = ++e;
      o = OISC4_BZPH();
      expr(Or);  *d = (int)(e + 1);
                 *o = (int)(oisc4_e + 3);
      ty = INT;
    }
    else if (tk == Or)  { next(); *++e = PSH; OISC4_PSH(); expr(Xor); *++e = OR; OISC4_MATH(OR); ty = INT;  }
    else if (tk == Xor) { next(); *++e = PSH; OISC4_PSH(); expr(And); *++e = XOR; OISC4_MATH(XOR); ty = INT;  }
    else if (tk == And) { next(); *++e = PSH; OISC4_PSH(); expr(Eq); *++e = AND; OISC4_MATH(AND); ty = INT;  }
    else if (tk == Eq)  { next(); *++e = PSH; OISC4_PSH(); expr(Lt); *++e = EQ; OISC4_MATH(EQ); ty = INT;  }
    else if (tk == Ne)  { next(); *++e = PSH; OISC4_PSH(); expr(Lt); *++e = NE; OISC4_MATH(NE); ty = INT;  }
    else if (tk == Lt)  { next(); *++e = PSH; OISC4_PSH(); expr(Shl); *++e = LT; OISC4_MATH(LT); ty = INT;  }
    else if (tk == Gt)  { next(); *++e = PSH; OISC4_PSH(); expr(Shl); *++e = GT; OISC4_MATH(GT); ty = INT;  }
    else if (tk == Le)  { next(); *++e = PSH; OISC4_PSH(); expr(Shl); *++e = LE; OISC4_MATH(LE); ty = INT;  }
    else if (tk == Ge)  { next(); *++e = PSH; OISC4_PSH(); expr(Shl); *++e = GE; OISC4_MATH(GE);ty = INT;  }
    else if (tk == Shl) { next(); *++e = PSH; OISC4_PSH(); expr(Add); *++e = SHL; OISC4_MATH(SHL);ty = INT;  }
    else if (tk == Shr) { next(); *++e = PSH; OISC4_PSH(); expr(Add); *++e = SHR; OISC4_MATH(SHR);ty = INT;  }
    else if (tk == Add) {
      next(); *++e = PSH; OISC4_PSH(); expr(Mul);
      if ((ty = t) > PTR) { *++e = PSH; OISC4_PSH(); *++e = IMM; *++e = sizeof(int); OISC4_IMM(sizeof(int)); *++e = MUL; OISC4_MATH(MUL); }
      *++e = ADD;
      OISC4_MATH(ADD);
    }
    else if (tk == Sub) {
      next(); *++e = PSH; OISC4_PSH(); expr(Mul);
      if (t > PTR && t == ty) { *++e = SUB; OISC4_MATH(SUB); *++e = PSH; OISC4_PSH(); *++e = IMM; *++e = sizeof(int); OISC4_IMM(sizeof(int)); *++e = DIV; OISC4_MATH(DIV); ty = INT; }
      else if ((ty = t) > PTR) { *++e = PSH; OISC4_PSH(); *++e = IMM; *++e = sizeof(int); OISC4_IMM(sizeof(int)); *++e = MUL; OISC4_MATH(MUL); *++e = SUB; OISC4_MATH(SUB); }
      else { *++e = SUB; OISC4_MATH(SUB); }
    }
    else if (tk == Mul) { next(); *++e = PSH; OISC4_PSH(); expr(Inc); *++e = MUL; OISC4_MATH(MUL); ty = INT; }
    else if (tk == Div) { next(); *++e = PSH; OISC4_PSH(); expr(Inc); *++e = DIV; OISC4_MATH(DIV); ty = INT; }
    else if (tk == Mod) { next(); *++e = PSH; OISC4_PSH(); expr(Inc); *++e = MOD; OISC4_MATH(MOD); ty = INT; }
    else if (tk == Inc || tk == Dec) {
      if (*e == LC) {
        *e = PSH; *++e = LC;
        OISC4_rewind_lc();
        OISC4_PSH();
        OISC4_LI(LC);
      }
      else if (*e == LI) {
        *e = PSH; *++e = LI;
        OISC4_rewind_li();
        OISC4_PSH();
        OISC4_LI(LI);
      }
      else { printf("%d: bad lvalue in post-increment\n", line); exit(-1); }
      *++e = PSH; *++e = IMM; *++e = (ty > PTR) ? sizeof(int) : sizeof(char);
      OISC4_PSH(); OISC4_IMM(*e);
      *++e = (tk == Inc) ? ADD : SUB;
      OISC4_MATH(*e);
      *++e = (ty == CHAR) ? SC : SI;
      OISC4_SI(*e);
      *++e = PSH; *++e = IMM; *++e = (ty > PTR) ? sizeof(int) : sizeof(char);
      OISC4_PSH(); OISC4_IMM(*e);
      *++e = (tk == Inc) ? SUB : ADD;
      OISC4_MATH(*e);
      next();
    }
    else if (tk == Brak) {
      next(); *++e = PSH; OISC4_PSH(); expr(Assign);
      if (tk == ']') next(); else { printf("%d: close bracket expected\n", line); exit(-1); }
      if (t > PTR) { *++e = PSH; OISC4_PSH(); *++e = IMM; *++e = sizeof(int); OISC4_IMM(*e); *++e = MUL; OISC4_MATH(MUL); }
      else if (t < PTR) { printf("%d: pointer type expected\n", line); exit(-1); }
      *++e = ADD; OISC4_MATH(ADD);
      *++e = ((ty = t - PTR) == CHAR) ? LC : LI;
      OISC4_LI(*e);
    }
    else { printf("%d: compiler error tk=%d\n", line, tk); exit(-1); }
  }
}

void stmt()
{
  int *a, *b;
  int *oa, *ob, *oc;

  if (tk == If) {
    next();
    if (tk == '(') next(); else { printf("%d: open paren expected\n", line); exit(-1); }
    expr(Assign);
    if (tk == ')') next(); else { printf("%d: close paren expected\n", line); exit(-1); }
    *++e = BZ; b = ++e;
    ob = OISC4_BZPH();
    stmt();
    if (tk == Else) {
      *b = (int)(e + 3); *++e = JMP; b = ++e;
      oc = OISC4_JMPPH();
      *ob = (int)oisc4_e;
      ob = oc;
      next();
      stmt();
    }
    *b = (int)(e + 1);
    *ob = (int)(oisc4_e + 1);
  }
  else if (tk == While) {
    next();
    a = e + 1;
    oa = oisc4_e + 1;
    if (tk == '(') next(); else { printf("%d: open paren expected\n", line); exit(-1); }
    expr(Assign);
    if (tk == ')') next(); else { printf("%d: close paren expected\n", line); exit(-1); }
    *++e = BZ; b = ++e;
    ob = OISC4_BZPH();
    stmt();
    *++e = JMP; *++e = (int)a;
    oc = OISC4_JMPPH();
    *b = (int)(e + 1);
    *ob = (int)(oisc4_e + 1);
    ob = oc;
    *ob = (int)(oisc4_e + 1);
  }
  else if (tk == Return) {
    next();
    if (tk != ';') expr(Assign);
    *++e = LEV;
    OISC4_LEV();
    if (tk == ';') next(); else { printf("%d: semicolon expected\n", line); exit(-1); }
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
    if (tk == ';') next(); else { printf("%d: semicolon expected (tk: '%c')\n", line, tk); exit(-1); }
  }
}

int OISC4_Debug_PrintHandlerC4 (int *pc, int *src, int add, int *dst) {
	printf("PC:%.12llx:%c%c", (int)pc, 9, 9);
	OISC4_Debug_PrintSymbol(src);
	printf(" + ");
	if(add == 0) printf("           0");
	else if(add < 10000) printf("%12.lld", add);
	else printf("%12.llx", add);
	printf("%c-> ", 9);
	OISC4_Debug_PrintSymbol(dst);
}

int main(int argc, char **argv)
{
  int fd, bt, ty, poolsz, *idmain;
  int *pc, *sp, *bp, a, cycle, run, status; // vm registers
  int i, *t; // temps
  char *_p, *_data;       // initial pointer locations
  int  *_sym, *_e, *_sp;  // initial pointer locations
  int  *_oisc4_e;

  status = 0;

  if(oisc_init()) { printf("oisc init failed\n"); return -1; }

  OISC4 = malloc(sizeof(int) * __OISC4__Sz);
  if(!OISC4) { printf("Malloc error\n"); return -1; }
  memset(OISC4, 0, sizeof(int) * __OISC4__Sz);

  poolsz = 2048 * 1024;
  if (!(oisc4_e = oisc4_le = malloc(poolsz))) { printf("could not malloc(%d) oisc area\n", poolsz); return -1; }
  _oisc4_e = oisc4_e;
  memset(oisc4_e, 0, poolsz);
  oisc4_le++;

  --argc; ++argv;
  if (argc > 0 && **argv == '-' && (*argv)[1] == 's') { src = 1; --argc; ++argv; }
  if (argc > 0 && **argv == '-' && (*argv)[1] == 'd') { debug = 1; --argc; ++argv; }
  if (argc < 1) { printf("usage: c4 [-s] [-d] file ...\n"); return -1; }

  if ((fd = open(*argv, 0)) < 0) { printf("could not open(%s)\n", *argv); return -1; }

  poolsz = 256 * 1024;
  if (!(sym = _sym = malloc(poolsz))) { printf("could not malloc(%d) symbol area\n", poolsz); return -1; }
  if (!(le = e = _e = malloc(poolsz))) { printf("could not malloc(%d) text area\n", poolsz); return -1; }
  if (!(data = _data = malloc(poolsz))) { printf("could not malloc(%d) data area\n", poolsz); return -1; }
  if (!(sp = _sp = malloc(poolsz))) { printf("could not malloc(%d) stack area\n", poolsz); return -1; }

  memset(sym,  0, poolsz);
  memset(e,    0, poolsz);
  memset(data, 0, poolsz);

  data_s = data;

  p = "char else enum if int return sizeof while "
      "open read close printf malloc realloc free memset memcmp memcpy stacktrace "
      "install_trap_handler __opcode __builtin __c4_trap __c4_opcode "
      "__c4_jmp __c4_adjust exit void main";
  i = Char; while (i <= While) { next(); id[Tk] = i++; } // add keywords to symbol table
  i = OPEN; while (i <= EXIT) { next(); id[Class] = Sys; id[Type] = INT; id[Val] = i++; } // add library to symbol table
  next(); id[Tk] = Char; // handle void type
  next(); idmain = id; // keep track of main

  if (!(lp = p = _p = malloc(poolsz))) { printf("could not malloc(%d) source area\n", poolsz); return -1; }
  if ((i = read(fd, p, poolsz-1)) <= 0) { printf("read() returned %d\n", i); return -1; }
  p[i] = 0;
  close(fd);

  // parse declarations
  line = 1;
  next();
  while (tk) {
    bt = INT; // basetype
    if (tk == Int) next();
    else if (tk == Char) { next(); bt = CHAR; }
    else if (tk == Enum) {
      next();
      if (tk != '{') next();
      if (tk == '{') {
        next();
        i = 0;
        while (tk != '}') {
          if (tk != Id) { printf("%d: bad enum identifier %d\n", line, tk); return -1; }
          next();
          if (tk == Assign) {
            next();
            if (tk != Num) { printf("%d: bad enum initializer\n", line); return -1; }
            i = ival;
            next();
          }
          id[Class] = Num; id[Type] = INT; id[Val] = i++;
          if (tk == ',') next();
        }
        next();
      }
    }
    while (tk != ';' && tk != '}') {
      ty = bt;
      while (tk == Mul) { next(); ty = ty + PTR; }
      if (tk != Id) { printf("%d: bad global declaration\n", line); return -1; }
      // if (id[Class]) { printf("%d: duplicate global definition\n", line); return -1; }
      next();
      id[Type] = ty;
      if (tk == '(') { // function
        id[Class] = Fun;
        id[Val] = (int)(e + 1);
        id[OISC4_Val] = (int)(oisc4_e + 1);
        next(); i = 0;
        while (tk != ')') {
          ty = INT;
          if (tk == Int) next();
          else if (tk == Char) { next(); ty = CHAR; }
          while (tk == Mul) { next(); ty = ty + PTR; }
          if (tk != Id) { printf("%d: bad parameter declaration\n", line); return -1; }
          if (id[Class] == Loc) { printf("%d: duplicate parameter definition\n", line); return -1; }
          id[HClass] = id[Class]; id[Class] = Loc;
          id[HType]  = id[Type];  id[Type] = ty;
          // TODO: track OISC4_Val too?
          id[HVal]   = id[Val];   id[Val] = i++;
          next();
          if (tk == ',') next();
        }
        next();
        if (tk != '{') { printf("%d: bad function definition\n", line); return -1; }
        loc = ++i;
        next();
        while (tk == Int || tk == Char) {
          bt = (tk == Int) ? INT : CHAR;
          next();
          while (tk != ';') {
            ty = bt;
            while (tk == Mul) { next(); ty = ty + PTR; }
            if (tk != Id) { printf("%d: bad local declaration\n", line); return -1; }
            if (id[Class] == Loc) { printf("%d: duplicate local definition\n", line); return -1; }
            id[HClass] = id[Class]; id[Class] = Loc;
            id[HType]  = id[Type];  id[Type] = ty;
            id[HVal]   = id[Val];   id[Val] = ++i;
            next();
            if (tk == ',') next();
          }
          next();
        }
        *++e = ENT; *++e = i - loc; OISC4_ENT(i - loc);
        while (tk != '}') stmt();
        if (*e != LEV) { *++e = LEV; OISC4_LEV(); }
        id = sym; // unwind symbol table locals
        while (id[Tk]) {
          if (id[Class] == Loc) {
            id[Class] = id[HClass];
            id[Type] = id[HType];
            id[Val] = id[HVal];
          }
          id = id + Idsz;
        }
      }
      else {
        id[Class] = Glo;
        id[Val] = (int)data;
        data = data + sizeof(int);
      }
      if (tk == ',') next();
    }
    next();
  }

  // free source
  free(_p);

  if (!(pc = (int *)idmain[Val])) { printf("main() not defined\n"); return -1; }
  registers[PC] = idmain[OISC4_Val];

  // setup stack
  bp = sp = (int *)((int)_sp + poolsz);
  *--sp = EXIT; // call exit if main returns
  *--sp = PSH; t = sp;
  *--sp = argc;
  *--sp = (int)argv;
  *--sp = (int)t;

  // as above
  sp = bp = (int *)((int)_sp + poolsz);
  t = oisc4_e + 1;
  printf("Exit stub address: %llx SP: %llx\n", (int)t, (int)sp);
  OISC4_PSH();
  OISC4_SYSCALL(EXIT - OPEN);
  *--sp = argc;
  *--sp = (int)argv;
  *--sp = (int)t; printf("Return address %llx written to %llx\n", (int)t, (int)sp);
  OISC4[_BP] = OISC4[_SP] = (int)sp;
  printf("SP at start: %llx points to %llx\n", (int)sp, *sp);

  // install our print handler
  debug_printhandler = (int *)&OISC4_Debug_PrintHandlerC4;

  // run...
  run = 1;
  cycle = 0;
  status = 0;
  printf("Entry point: %llx\n", registers[PC]);

  if (src) {
    printf("\n\nFinal source layout:\n");
    t = _oisc4_e + 1;
    while (t < oisc4_e) {
      printf("   %.12llx:%c%c", (int)t, 9, 9);
      OISC4_Debug_PrintSymbol((int *)*t++);
      printf(" + ");
      if(*t == 0) printf("           0");
      else if(*t < 10000) printf("%12.lld", *t);
      else printf("%12.llx", *t);
      t++;
      printf("%c-> ", 9);
      OISC4_Debug_PrintSymbol((int *)*t++);
      printf("\n");
    }
  } else {
    a = oisc4_run();
  }

  printf("Done.\n");

  if(0)
  while (run) {
    i = *pc++; ++cycle;
    if (debug) {
      printf("%d> %.4s", cycle,
        &"LEA ,IMM ,JMP ,JSR ,JSRI,JSRS,BZ  ,BNZ ,ENT ,ADJ ,LEV ,LI  ,LC  ,SI  ,SC  ,PSH ,"
         "OR  ,XOR ,AND ,EQ  ,NE  ,LT  ,GT  ,LE  ,GE  ,SHL ,SHR ,ADD ,SUB ,MUL ,DIV ,MOD ,"
         "OPEN,READ,CLOS,PRTF,MALC,FREE,MSET,MCMP,EXIT,"[i * 5]);
      if (i <= ADJ) printf(" %d\n", *pc); else printf("\n");
    }
    if      (i == LEA) a = (int)(bp + *pc++);                             // load local address
    else if (i == IMM) a = *pc++;                                         // load global address or immediate
    else if (i == JMP) pc = (int *)*pc;                                   // jump
    else if (i == JSR) { *--sp = (int)(pc + 1); pc = (int *)*pc; }        // jump to subroutine
    else if (i == JSRI) { *--sp = (int)(pc + 1); pc = (int *)*pc; pc = (int *)*pc;}  // jump to subroutine indirect
    else if (i == JSRS) { *--sp = (int)(pc + 1); pc = (int *)*(bp + *pc++); }  // jump to subroutine indirect on stack
    else if (i == BZ)  pc = a ? pc + 1 : (int *)*pc;                      // branch if zero
    else if (i == BNZ) pc = a ? (int *)*pc : pc + 1;                      // branch if not zero
    else if (i == ENT) { *--sp = (int)bp; bp = sp; sp = sp - *pc++; }     // enter subroutine
    else if (i == ADJ) sp = sp + *pc++;                                   // stack adjust
    else if (i == LEV) { sp = bp; bp = (int *)*sp++; pc = (int *)*sp++; } // leave subroutine
    else if (i == LI)  a = *(int *)a;                                     // load int
    else if (i == LC)  a = *(char *)a;                                    // load char
    else if (i == SI)  *(int *)*sp++ = a;                                 // store int
    else if (i == SC)  a = *(char *)*sp++ = a;                            // store char
    else if (i == PSH) *--sp = a;                                         // push

    else if (i == OR)  a = *sp++ |  a;
    else if (i == XOR) a = *sp++ ^  a;
    else if (i == AND) a = *sp++ &  a;
    else if (i == EQ)  a = *sp++ == a;
    else if (i == NE)  a = *sp++ != a;
    else if (i == LT)  a = *sp++ <  a;
    else if (i == GT)  a = *sp++ >  a;
    else if (i == LE)  a = *sp++ <= a;
    else if (i == GE)  a = *sp++ >= a;
    else if (i == SHL) a = *sp++ << a;
    else if (i == SHR) a = *sp++ >> a;
    else if (i == ADD) a = *sp++ +  a;
    else if (i == SUB) a = *sp++ -  a;
    else if (i == MUL) a = *sp++ *  a;
    else if (i == DIV) a = *sp++ /  a;
    else if (i == MOD) a = *sp++ %  a;

    else if (i == OPEN) a = open((char *)sp[1], *sp);
    else if (i == READ) a = read(sp[2], (char *)sp[1], *sp);
    else if (i == CLOS) a = close(*sp);
    else if (i == PRTF) { t = sp + pc[1]; a = printf((char *)t[-1], t[-2], t[-3], t[-4], t[-5], t[-6]); }
    else if (i == MALC) a = (int)malloc(*sp);
    else if (i == FREE) free((void *)*sp);
    else if (i == MSET) a = (int)memset((char *)sp[2], sp[1], *sp);
    else if (i == MCMP) a = memcmp((char *)sp[2], (char *)sp[1], *sp);
    else if (i == EXIT) { printf("exit(%d) cycle = %d\n", *sp, cycle); status = *sp; run = 0; }
    else { printf("unknown instruction = %d! cycle = %d\n", i, cycle); status = -1; run = 0; }
  }

  // free memory
  free(_sym);
  free(_e);
  free(_data);
  free(_sp);
  free(_oisc4_e);
  free(OISC4);
  oisc_cleanup();

  return status;
}


