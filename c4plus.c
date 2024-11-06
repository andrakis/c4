// c4_plus.c - Modern implementation of C4 with as many bells and whistles
// as possible.
//
// Compilation:
//  gcc -O2 -g c4plus.c -o c4plus
//  Optional Flags:
//    -DWord=...   Word size big enough to store a pointer.
//                 Defaults to 'long long', but may need to be 'long' or 'int'
//                 on 32bit platforms.
// Usage:
//  c4plus <first file.c> [file n.c, ...] [-- arguments...]
// Example:
//  c4plus classes.c classes2.c -- test arguments
// All given .c files are read into one big string, in the order given.
// Functions of the same name will silently overwrite any previous
// definition.

// Additional functions:
// - void stacktrace();   Print a stacktrace
// - Adds JSRI: Jump to SubRoutine Indirect
// - Adds JSRS: Jump to SubRoutine on Stack
// - Adds &function to get function address. Can be called if stored in an Word *.
// - Adds stacktrace() builtin
// - Adds realloc() builtin
// - Adds memcpy() builtin

// char, Word, and pointer types
// if, while, return, and expression statements
// ability to obtain and call function pointers
// just enough features to allow self-compilation and a bit more
//

// Originally written by Robert Swierczek

#include <stdio.h>
#include <stdlib.h>
#include <memory.h>
#include <unistd.h>
#include <fcntl.h>

// Word: Emulator word type.
//       Has to be large enough to store a pointer.
#ifndef Word
#define Word long long
#endif
#pragma GCC diagnostic ignored "-Wformat"

// Allow testing stacktrace() by running in c4_multiload
#define stacktrace renamed_when_not_C4
void stacktrace () { }
#undef stacktrace
#define stacktrace()

char *p, *lp, // current position in source code
     *data;   // data/bss pointer

Word *e, *le,  // current position in emitted code
    *id,      // currently parsed identifier
    *sym,     // symbol table (simple list of identifiers)
    tk,       // current token
    ival,     // current token value
    ty,       // current expression type
    loc,      // local variable offset
    line,     // current line number
    src,      // print source and assembly flag
    debug;    // print executed instructions
Word interrupt_waiting, interrupt_enterred, interrupt_signal, *interrupt_handler;

// tokens and classes (operators last and in precedence order)
enum {
  Num = 128, Fun, Sys, Glo, Loc, Id,
  Char, Else, Enum, If, Int, Return, Sizeof, While,
  Assign, Cond, Lor, Lan, Or, Xor, And, Eq, Ne, Lt, Gt, Le, Ge, Shl, Shr, Add, Sub, Mul, Div, Mod, Inc, Dec, Brak
};

// opcodes
enum { LEA ,IMM ,JMP ,JSR ,JSRI,JSRS,BZ  ,BNZ ,ENT ,ADJ ,LEV ,LI  ,LC  ,SI  ,SC  ,PSH ,
       OR  ,XOR ,AND ,EQ  ,NE  ,LT  ,GT  ,LE  ,GE  ,SHL ,SHR ,ADD ,SUB ,MUL ,DIV ,MOD ,
       OPEN,READ,CLOS,PRTF,MALC,RALC,FREE,MSET,MCMP,MCPY,STRC,EXIT };
enum { // extra opcodes to move above
    JMPA = 100,
    LISP, LIBP,
    FESP, FEBP,
    INTR
};
char *opcodes;
void setup_opcodes () {
    char *def;
    def = "LEA ,IMM ,JMP ,JSR ,JSRI,JSRS,BZ  ,BNZ ,ENT ,ADJ ,LEV ,LI  ,LC  ,SI  ,SC  ,PSH ,"
          "OR  ,XOR ,AND ,EQ  ,NE  ,LT  ,GT  ,LE  ,GE  ,SHL ,SHR ,ADD ,SUB ,MUL ,DIV ,MOD ,"
          "OPEN,READ,CLOS,PRTF,MALC,RALC,FREE,MSET,MCMP,MCPY,STRC,EXIT";
    strcpy(opcodes, def);
}

// types
enum { CHAR, INT, PTR };

// identifier offsets (since we can't create an ident struct)
enum { Tk, Hash, Name, Class, Type, Val, Length, HClass, HType, HVal, Idsz };

void dump_exit (Word code) {
  printf("%d: %.*s", line, p - lp, lp);
  exit(code);
}

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
          printf("%8.4s", &"LEA ,IMM ,JMP ,JSR ,JSRI,JSRS,BZ  ,BNZ ,ENT ,ADJ ,LEV ,LI  ,LC  ,SI  ,SC  ,PSH ,"
                           "OR  ,XOR ,AND ,EQ  ,NE  ,LT  ,GT  ,LE  ,GE  ,SHL ,SHR ,ADD ,SUB ,MUL ,DIV ,MOD ,"
                           "OPEN,READ,CLOS,PRTF,MALC,RALC,FREE,MSET,MCMP,MCPY,STRC,EXIT"[*++le * 5]);
          if (*le <= ADJ) printf(" %d\n", *++le); else printf("\n");
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
      id[Name] = (Word)pp;
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
        // C++ style comments
        ++p;
        while (*p != 0 && *p != '\n') ++p;
      } else if (*p == '*') {
        /* C style comments */
        ++p;
        while (*p && *p != '*' && (*(p + 1) == '/'))
            ++p;
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
          ival = *p++;
          if (ival == 'n') ival = '\n';
          if (ival == 'r') ival = '\r';
          if (ival == 't') ival = '\t';
          if (ival == '0') ival = '\0';
          if (ival == 'a') ival = '\a';
          if (ival == 'v') ival = '\v';
          if (ival == '\\') ival = '\\';
        }
        if (tk == '"') *data++ = ival;
      }
      ++p;
      if (tk == '"') ival = (Word)pp; else tk = Num;
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

void expr(Word lev)
{
  Word t, *d;

  if (!tk) { printf("%d: unexpected eof in expression\n", line); exit(-1); }
  else if (tk == Num) { *++e = IMM; *++e = ival; next(); ty = INT; }
  else if (tk == '"') {
    *++e = IMM; *++e = ival; next();
    while (tk == '"') next();
    data = (char *)((Word)data + sizeof(Word) & -sizeof(Word)); ty = PTR;
  }
  else if (tk == Sizeof) {
    next(); if (tk == '(') next(); else { printf("%d: open paren expected in sizeof\n", line); exit(-1); }
    ty = INT; if (tk == Int) next(); else if (tk == Char) { next(); ty = CHAR; }
    while (tk == Mul) { next(); ty = ty + PTR; }
    if (tk == ')') next(); else { printf("%d: close paren expected in sizeof\n", line); exit(-1); }
    *++e = IMM; *++e = (ty == CHAR) ? sizeof(char) : sizeof(Word);
    ty = INT;
  }
  else if (tk == Id) {
    d = id; next();
    if (tk == '(') {
      next();
      t = 0;
      while (tk != ')') { expr(Assign); *++e = PSH; ++t; if (tk == ',') next(); }
      next();
      if (d[Class] == Sys) *++e = d[Val];
      else if (d[Class] == Fun) { *++e = JSR; *++e = d[Val]; }
      else if (d[Class] == Glo) { *++e = JSRI; *++e = d[Val]; } // Jump subroutine indirect
      else if (d[Class] == Loc) { *++e = JSRS; *++e = loc - d[Val]; } // Jump subroutine on stack
      else { printf("%d: bad function call (%d)\n", line, d[Class]); dump_exit(-1); }
      if (t) { *++e = ADJ; *++e = t; }
      ty = d[Type];
    }
    else if (d[Class] == Num) { *++e = IMM; *++e = d[Val]; ty = INT; }
    else {
      if (d[Class] == Loc) { *++e = LEA; *++e = loc - d[Val]; }
      else if (d[Class] == Glo) { *++e = IMM; *++e = d[Val]; }
      else if (d[Class] == Fun) { *++e = IMM; *++e = d[Val]; } // Function address
      else { printf("%d: undefined variable\n", line); exit(-1); }
      *++e = ((ty = d[Type]) == CHAR) ? LC : LI;
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
  }
  else if (tk == And) {
    next(); expr(Inc);
    if (*e == LC || *e == LI) --e; else { printf("%d: bad address-of\n", line); exit(-1); }
    ty = ty + PTR;
  }
  else if (tk == '!') { next(); expr(Inc); *++e = PSH; *++e = IMM; *++e = 0; *++e = EQ; ty = INT; }
  else if (tk == '~') { next(); expr(Inc); *++e = PSH; *++e = IMM; *++e = -1; *++e = XOR; ty = INT; }
  else if (tk == Add) { next(); expr(Inc); ty = INT; }
  else if (tk == Sub) {
    next(); *++e = IMM;
    if (tk == Num) { *++e = -ival; next(); } else { *++e = -1; *++e = PSH; expr(Inc); *++e = MUL; }
    ty = INT;
  }
  else if (tk == Inc || tk == Dec) {
    t = tk; next(); expr(Inc);
    if (*e == LC) { *e = PSH; *++e = LC; }
    else if (*e == LI) { *e = PSH; *++e = LI; }
    else { printf("%d: bad lvalue in pre-increment\n", line); exit(-1); }
    *++e = PSH;
    *++e = IMM; *++e = (ty > PTR) ? sizeof(Word) : sizeof(char);
    *++e = (t == Inc) ? ADD : SUB;
    *++e = (ty == CHAR) ? SC : SI;
  }
  else { printf("%d: bad expression\n", line); exit(-1); }

  while (tk >= lev) { // "precedence climbing" or "Top Down Operator Precedence" method
    t = ty;
    if (tk == Assign) {
      next();
      if (*e == LC || *e == LI) *e = PSH; else { printf("%d: bad lvalue in assignment\n", line); exit(-1); }
      expr(Assign); *++e = ((ty = t) == CHAR) ? SC : SI;
    }
    else if (tk == Cond) {
      next();
      *++e = BZ; d = ++e;
      expr(Assign);
      if (tk == ':') next(); else { printf("%d: conditional missing colon\n", line); exit(-1); }
      *d = (Word)(e + 3); *++e = JMP; d = ++e;
      expr(Cond);
      *d = (Word)(e + 1);
    }
    else if (tk == Lor) { next(); *++e = BNZ; d = ++e; expr(Lan); *d = (Word)(e + 1); ty = INT; }
    else if (tk == Lan) { next(); *++e = BZ;  d = ++e; expr(Or);  *d = (Word)(e + 1); ty = INT; }
    else if (tk == Or)  { next(); *++e = PSH; expr(Xor); *++e = OR;  ty = INT; }
    else if (tk == Xor) { next(); *++e = PSH; expr(And); *++e = XOR; ty = INT; }
    else if (tk == And) { next(); *++e = PSH; expr(Eq);  *++e = AND; ty = INT; }
    else if (tk == Eq)  { next(); *++e = PSH; expr(Lt);  *++e = EQ;  ty = INT; }
    else if (tk == Ne)  { next(); *++e = PSH; expr(Lt);  *++e = NE;  ty = INT; }
    else if (tk == Lt)  { next(); *++e = PSH; expr(Shl); *++e = LT;  ty = INT; }
    else if (tk == Gt)  { next(); *++e = PSH; expr(Shl); *++e = GT;  ty = INT; }
    else if (tk == Le)  { next(); *++e = PSH; expr(Shl); *++e = LE;  ty = INT; }
    else if (tk == Ge)  { next(); *++e = PSH; expr(Shl); *++e = GE;  ty = INT; }
    else if (tk == Shl) { next(); *++e = PSH; expr(Add); *++e = SHL; ty = INT; }
    else if (tk == Shr) { next(); *++e = PSH; expr(Add); *++e = SHR; ty = INT; }
    else if (tk == Add) {
      next(); *++e = PSH; expr(Mul);
      if ((ty = t) > PTR) { *++e = PSH; *++e = IMM; *++e = sizeof(Word); *++e = MUL;  }
      *++e = ADD;
    }
    else if (tk == Sub) {
      next(); *++e = PSH; expr(Mul);
      if (t > PTR && t == ty) { *++e = SUB; *++e = PSH; *++e = IMM; *++e = sizeof(Word); *++e = DIV; ty = INT; }
      else if ((ty = t) > PTR) { *++e = PSH; *++e = IMM; *++e = sizeof(Word); *++e = MUL; *++e = SUB; }
      else *++e = SUB;
    }
    else if (tk == Mul) { next(); *++e = PSH; expr(Inc); *++e = MUL; ty = INT; }
    else if (tk == Div) { next(); *++e = PSH; expr(Inc); *++e = DIV; ty = INT; }
    else if (tk == Mod) { next(); *++e = PSH; expr(Inc); *++e = MOD; ty = INT; }
    else if (tk == Inc || tk == Dec) {
      if (*e == LC) { *e = PSH; *++e = LC; }
      else if (*e == LI) { *e = PSH; *++e = LI; }
      else { printf("%d: bad lvalue in post-increment\n", line); exit(-1); }
      *++e = PSH; *++e = IMM; *++e = (ty > PTR) ? sizeof(Word) : sizeof(char);
      *++e = (tk == Inc) ? ADD : SUB;
      *++e = (ty == CHAR) ? SC : SI;
      *++e = PSH; *++e = IMM; *++e = (ty > PTR) ? sizeof(Word) : sizeof(char);
      *++e = (tk == Inc) ? SUB : ADD;
      next();
    }
    else if (tk == Brak) {
      next(); *++e = PSH; expr(Assign);
      if (tk == ']') next(); else { printf("%d: close bracket expected\n", line); exit(-1); }
      if (t > PTR) { *++e = PSH; *++e = IMM; *++e = sizeof(Word); *++e = MUL;  }
      else if (t < PTR) { printf("%d: pointer type expected\n", line); exit(-1); }
      *++e = ADD;
      *++e = ((ty = t - PTR) == CHAR) ? LC : LI;
    }
    else { printf("%d: compiler error tk=%d\n", line, tk); exit(-1); }
  }
}

void stmt()
{
  Word *a, *b;

  if (tk == If) {
    next();
    if (tk == '(') next(); else { printf("%d: open paren expected\n", line); exit(-1); }
    expr(Assign);
    if (tk == ')') next(); else { printf("%d: close paren expected\n", line); exit(-1); }
    *++e = BZ; b = ++e;
    stmt();
    if (tk == Else) {
      *b = (Word)(e + 3); *++e = JMP; b = ++e;
      next();
      stmt();
    }
    *b = (Word)(e + 1);
  }
  else if (tk == While) {
    next();
    a = e + 1;
    if (tk == '(') next(); else { printf("%d: open paren expected\n", line); exit(-1); }
    expr(Assign);
    if (tk == ')') next(); else { printf("%d: close paren expected\n", line); exit(-1); }
    *++e = BZ; b = ++e;
    stmt();
    *++e = JMP; *++e = (Word)a;
    *b = (Word)(e + 1);
  }
  else if (tk == Return) {
    next();
    if (tk != ';') expr(Assign);
    *++e = LEV;
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

void print_symbol(Word *i) {
  char *strc_a, *strc_b, *misc, *type;
  char *ptrstring;
  Word   t, ptrcount;

  ptrstring = "****************";

  // Find symbol name length
  strc_a = strc_b = (char *)i[Name];
  while ((*strc_b >= 'a' && *strc_b <= 'z') || (*strc_b >= 'A' && *strc_b <= 'Z') || (*strc_b >= '0' && *strc_b <= '9') || *strc_b == '_')
      ++strc_b;

  // Calculate type string
  if (i[Type] == CHAR) type = "char|void"; // void is represented as char
  else if(i[Type] & INT)  type = "int";
  else type = "char";
  t = i[Type];
  ptrcount = 0;
  while(t > PTR) { ptrcount++; t = t - PTR; }
  ptrcount = ptrcount % 16; // Not more than ptrstring length
  //printf("ptrcount:%lld type:%lld\n", ptrcount, i[Type]);

  // Print depending on type
  if (i[Tk] == Id && i[Class] == Fun) {
    // Function: print type name() [address]
    printf("%s%.*s %.*s() [%p]", type, ptrcount, ptrstring, strc_b - strc_a, strc_a, (Word *)i[Val]);
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

void print_stacktrace (Word *pc, Word *idmain, Word *bp, Word *sp) {
    Word found, done, *idold, *t, range, depth;

    idold = id;
    done = 0;
    t = 0;
    depth = 0;

    while (!done) {
        found = 0;
        //comparisons = 0;
        while(!found && id[Tk]) {
            //++comparisons;
            if (id[Tk] == Id && id[Class] == Fun && pc >= (Word *)id[Val] && pc <= (Word *)(id[Val] + id[Length])) {
                t = id;
                found = 1;
            } else
                id = id + Idsz;
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
        bp = (Word *)*sp++;
        pc = (Word *)*sp++;
    }
}

char *interrupts_waiting, interrupts_waiting_max, interrupts_waiting_index;
int main(int argc, char **argv)
{
  Word fd, bt, ty, poolsz, *idmain, printsyms;
  Word *pc, *sp, *bp, a, cycle, run, status; // vm registers
  Word i, *t, r; // temps
  char *_p, *_data;       // initial pointer locations
  Word *_sym, *_e, *_sp;  // initial pointer locations
  Word sources_read;

  //debug = 1;
  sources_read = 0;

  //printf("(c4plus) Argc: %lld\n", argc); i = 0; while(i < argc) { printf("(c4plus) Argv[%lld] = %s\n", i, argv[i]); ++i; }
  interrupts_waiting_index = 0;
  interrupts_waiting_max   = 16;
  if (!(interrupts_waiting = malloc(i = (sizeof(char) * interrupts_waiting_max)))) { printf("could not malloc(%d) interrupts list\n", i); return -1; }
  memset(interrupts_waiting, 0, i);

  poolsz = 256*1024; // arbitrary size
  printsyms = 0;
  --argc; ++argv;
  if (argc > 0 && **argv == '-' && (*argv)[1] == 's') { src = 1; --argc; ++argv; }
  if (argc > 0 && **argv == '-' && (*argv)[1] == 'd') { debug = 1; --argc; ++argv; }
  if (argc > 0 && **argv == '-' && (*argv)[1] == 'S') { printsyms = 1; --argc; ++argv; }
  // TODO: these options broken
  if (argc > 0 && **argv == '-' && (*argv)[1] == 'p') { i = 1; while((*argv)[1 + i++]) poolsz = poolsz / 2; --argc; ++argv; }
  if (argc > 0 && **argv == '-' && (*argv)[1] == 'P') { i = 1; while((*argv)[1 + i++]) poolsz = poolsz * 2; --argc; ++argv; }
  if (argc < 1) { printf("usage: c4_multiload [-s] [-d] [-p] [-P] file1 [files...] -- args ...\n"); return -1; }

  if (!(sym = _sym = malloc(poolsz))) { printf("could not malloc(%d) symbol area\n", poolsz); return -1; }
  if (!(le = e = _e = malloc(poolsz))) { printf("could not malloc(%d) text area\n", poolsz); return -1; }
  if (!(data = _data = malloc(poolsz))) { printf("could not malloc(%d) data area\n", poolsz); return -1; }
  if (!(sp = _sp = malloc(poolsz))) { printf("could not malloc(%d) stack area\n", poolsz); return -1; }

  memset(sym,  0, poolsz);
  memset(e,    0, poolsz);
  memset(data, 0, poolsz);

  p = "char else enum if int return sizeof while "
      "open read close printf malloc realloc free memset memcmp memcpy stacktrace exit void main";
  i = Char; while (i <= While) { next(); id[Tk] = i++; } // add keywords to symbol table
  i = OPEN; while (i <= EXIT) { next(); id[Class] = Sys; id[Type] = INT; id[Val] = i++; } // add library to symbol table
  next(); id[Tk] = Char; // handle void type
  next(); idmain = id; // keep track of main

  if (!(lp = p = _p = malloc(poolsz))) { printf("could not malloc(%d) source area\n", poolsz); return -1; }

  //i = 0; printf("// (c4plus) Argc: %lld\n", argc); while(i < argc) { printf("// (c4plus) argv[%lld] = %s\n", i, *(argv + i)); ++i; }

  // Read all specified source files
  r = poolsz - 1;        // track memory remaining
  //printf("// (c4plus) arg parsing starts...\n");
  if (argc > 1 && r > 0 && *argv && **argv == '-' && *(*argv + 1) == '-') {
	  ++argc; --argv;
  }
  while (argc > 1 && r > 0 && !(**argv == '-' && *(*argv + 1) == '-')) {
    //printf("// (c4plus) argv '%c' %lld, argv+1 '%c' %lld\n", **argv, **argv, *(*argv + 1), *(*argv + 1));
    if ((fd = open(*argv, 0)) < 0) { printf("could not open(%s)\n", *argv); return -1; }
    //else printf("// %s open success\n", *argv);
    if ((i = read(fd, p, r)) <= 0) { printf("read() returned %d\n", i); return -1; }
    p[i] = 0;
    // Advance p to the nul we just wrote, new content will go here
    p = p + i;
    r = r - i;
    close(fd);
    ++argv;
    --argc;
    sources_read++;
  }
  //++argv; --argc;
  //printf("// (c4plus) arg parsing ends, argv is now: '%s'\n", *argv);
  if (r == 0) { printf("could not read all source files: exceeded %d (0x%X) bytes\n", poolsz, poolsz); return -1; }
  // Reset pointer to start of code
  p = _p;

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
      //if (id[Class]) { printf("%d: duplicate global definition\n", line); return -1; }
      next();
      id[Type] = ty;
      if (tk == '(') { // function
        // don't overwrite builtins
        if (id >= idmain) {
            id[Class] = Fun;
            id[Val] = (Word)(e + 1);
        }
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
        *++e = ENT; *++e = i - loc;
        while (tk != '}') stmt();
        *++e = LEV;
        id = sym; // unwind symbol table locals
        while (id[Tk]) {
          if (id[Class] == Loc) {
            id[Class] = id[HClass];
            id[Type] = id[HType];
            id[Val] = id[HVal];
          }
          id = id + Idsz;
        }
        // Record length of emitted code
        id[Length] = (Word)e - id[Val];
      }
      else {
        id[Class] = Glo;
        id[Val] = (Word)data;
        data = data + sizeof(Word);
      }
      if (tk == ',') next();
    }
    next();
  }

  if (!(pc = (Word *)idmain[Val])) { printf("main() not defined\n"); return -1; }
  if (printsyms) {
    id = sym;
    a = 0;
    while (id[Tk]) {
        print_symbol(id); printf("\n");
        id = id + Idsz;
        a = a + 1;
    }
    printf("Symbol table: %d entries using %d (0x%X) bytes\n", a, a * Idsz * sizeof(Word));
  }
  if (src) return 0;

  // setup stack
  bp = sp = (Word *)((Word)sp + poolsz);
  *--sp = EXIT; // call exit if main returns
  *--sp = PSH; t = sp;
  *--sp = argc;
  *--sp = (Word)argv;
  *--sp = (Word)t;

  // run...
  run = 1;
  cycle = 0;
  status = 0;
  for ( ;; ) {
	if (!interrupt_enterred && interrupt_waiting) {
		interrupt_enterred = 1;
		// PSH interrupt signal
		*--sp = interrupt_waiting;
        interrupt_waiting = 0;
		// PSH PC
		*--sp = (Word)pc;
		pc = (Word *)interrupt_handler;
		if (debug) {
			printf("!Interrupt %lld, pc = %p\n", a, pc);
		}
	}
    i = *pc++; //++cycle;
    if (debug) {
      printf("%d> %.4s", cycle,
        &"LEA ,IMM ,JMP ,JSR ,JSRI,JSRS,BZ  ,BNZ ,ENT ,ADJ ,LEV ,LI  ,LC  ,SI  ,SC  ,PSH ,"
         "OR  ,XOR ,AND ,EQ  ,NE  ,LT  ,GT  ,LE  ,GE  ,SHL ,SHR ,ADD ,SUB ,MUL ,DIV ,MOD ,"
         "OPEN,READ,CLOS,PRTF,MALC,RALC,FREE,MSET,MCMP,MCPY,STRC,EXIT,"[i * 5]);
      if (i <= ADJ) printf(" %d\n", *pc); else printf("\n");
    }
	switch(i) {
	case LEA: a = (Word)(bp + *pc++); break;                      // load local address
	case IMM: a = *pc++; break;                                   // load global address or immediate 
    case JMP: pc = (Word *)*pc; break;                            // jump
    case JMPA: pc = (Word *)a; break;                             // jump using accumulator
    case JSR: *--sp = (Word)(pc + 1); pc = (Word *)*pc; break;    // jump to subroutine
    case JSRI: *--sp = (Word)(pc + 1); pc = (Word *)*pc; pc = (Word *)*pc; break;  // jump to subroutine indirect
    case JSRS: *--sp = (Word)(pc + 1); pc = (Word *)*(bp + *pc); break;  // jump to subroutine indirect on stack
    case BZ:  pc = a ? pc + 1 : (Word *)*pc; break;               // branch if zero
    case BNZ: pc = a ? (Word *)*pc : pc + 1; break;               // branch if not zero
    case ENT: *--sp = (Word)bp; bp = sp; sp = sp - *pc++; break;  // enter subroutine
    case ADJ: sp = sp + *pc++; break;                             // stack adjust
    case LEV: sp = bp; bp = (Word *)*sp++; pc = (Word *)*sp++; break;  // leave subroutine
    case LI:  a = *(Word *)a; break;                              // load Word
    case LC:  a = *(char *)a; break;                              // load char
    case LISP:sp = (Word *)*(Word *)a; break;                             // load int and store in SP
    case LIBP:bp = (Word *)*(Word *)a; break;                             // load int and store in BP
    case FESP:a = (Word)sp; break;                                // fetch SP and store in A
    case FEBP:a = (Word)bp; break;                                // fetch BP and store in A
    case SI:  *(Word *)*sp++ = a; break;                          // store Word
    case SC:  a = *(char *)*sp++ = a; break;                      // store char
    case PSH: *--sp = a; break;                                   // push
    case INTR: {
        if(!interrupt_waiting) interrupt_waiting = a;
        else {
            // record a new waiting interrupt
            i = 0;
            while(i < interrupts_waiting_max && interrupts_waiting[i]) ++i;
            if (i == interrupts_waiting_max) {
                printf("Unable to schedule interrupt %lld\n", a);
            } else {
                interrupts_waiting[i] = (char)a;
            }
        }
        break;
    }
    case OR:  a = *sp++ |  a; break;
    case XOR: a = *sp++ ^  a; break;
    case AND: a = *sp++ &  a; break;
    case EQ:  a = *sp++ == a; break;
    case NE:  a = *sp++ != a; break;
    case LT:  a = *sp++ <  a; break;
    case GT:  a = *sp++ >  a; break;
    case LE:  a = *sp++ <= a; break;
    case GE:  a = *sp++ >= a; break;
    case SHL: a = *sp++ << a; break;
    case SHR: a = *sp++ >> a; break;
    case ADD: a = *sp++ +  a; break;
    case SUB: a = *sp++ -  a; break;
    case MUL: a = *sp++ *  a; break;
    case DIV: a = *sp++ /  a; break;
    case MOD: a = *sp++ %  a; break;

    case OPEN: a = open((char *)sp[1], *sp); break;
    case READ: a = read(sp[2], (char *)sp[1], *sp); break;
    case CLOS: a = close(*sp); break;
    case PRTF: t = sp + pc[1]; a = printf((char *)t[-1], t[-2], t[-3], t[-4], t[-5], t[-6]); break;
    case MALC: a = (Word)malloc(*sp); break;
    case RALC: a = (Word)realloc((Word *)sp[1], *sp); break;
    case FREE: free((void *)*sp); break;
    case MSET: a = (Word)memset((char *)sp[2], sp[1], *sp); break;
    case MCMP: a = memcmp((char *)sp[2], (char *)sp[1], *sp); break;
    case MCPY: a = (Word)memcpy((void *)sp[2], (void *)sp[1], *sp); break;
    case STRC: print_stacktrace(pc, idmain, bp, sp); break;
    case EXIT:
		//printf("exit(%d) cycle = %d\n", *sp, cycle);
		status = *sp; run = 0;
		goto done;
    default:
		printf("unknown instruction = %d! cycle = %d\n", i, cycle);
		status = -1;
		goto done;
	}
  }

done:

  // free memory
  free(_p);
  free(_sym);
  free(_e);
  free(_data);
  free(_sp);

  return status;
}

