// c4m.c - C4 Multiload, originally C in four functions, supporting multiple
//         files and some bells and whistles whilst still running under
//         regular c4.
//
// Usage:
//  c4 c4m.c [-sSdpP] <first file.c> [file n.c, ...] [-- arguments...]
// Example:
//  c4 c4m.c classes.c classes_test.c -- test arguments
//
// Parameters:
//  -s           Print the source and exit (memory leaks)
//  -S           Print source symbol listing
//  -d           Enable debug output during execution
//  -p           (Currently nonfunctional) Decrease pool size
//  -P           (Currently nonfunctional)
// TODO: Fix the above two parameters.
//
// All given .c files are read into one big string, in the order given.
// Functions of the same name will silently overwrite any previous
// definition.
// Builtins can no longer be overridden, but can be dummied out so that
// code still works on C4.
// Trap and interrupt handlers can be installed. Signal handlers also supported.
// 'static' keyword supported but ignored. Used by c4cc.
//
// Notes:
//  2024 / 07 / 13 - C4 versions of malloc(), realloc() removed. Were causing crashes.

// Additional functions:
// - __c4_cycles()                 Request the cycle counter
// - __c4_configure(opt,val)       Configure system settings.
//     Currently two settings are supported, that control the cycle interrupt:
//       CONF_CYCLE_INTERRUPT_INTERVAL  How many cycles between generating interrupt.
//                                      Setting to 0 disables the cycle interrupt.
//       CONF_CYCLE_INTERRUPT_HANDLER   The function address of the handler.
//       The cycle handler signature is:
//       void handler(int type, int ins, int *a, int *bp, int *sp, int *returnpc) {}
//
// - void stacktrace();            Print a stacktrace
// - void install_trap_handler(void (*handler)(int trap, int ins, int *a, int *bp, int *sp, int *returnpc));
//     Install a trap handler. See test_customop.c for more information.
//     A trap occurs under the following conditions:
//       - TRAP_ILLOP: An unknown opcode is encountered. Allows custom opcodes to be
//         emulated in C4 code.
//     The function handler can modify the a, sp, bp, and return pc values.
//     The returnpc is the illegal instruction address + 1 word, but can be adjusted:
//       - If your opcode takes arguments you can adjust the returnpc to skip them.
// - void __c4_opcode (int opcode);Execute the given opcode, which can trap if
//                                 using a custom opcode. Opcode number must be last:
//        __c4_opcode(handler, sig, OP_USER_SIGNAL);
//        Arguments should be pushed in reverse order.
// - void __c4_jmp (int address);  Jump directly to a given function address.
// - void __c4_adjust (int offset);Adjust the stack. Negative offset grows stack.
// - int __opcode (char *name);    Request the integer value of an opcode.
// - int __builtin (char *name);   Request the opcode for a builtin.
// - Adds JSRI: Jump to SubRoutine Indirect
// - Adds JSRS: Jump to SubRoutine on Stack
// - Adds &function to get function address. Can be called if stored in an int*.
//   See classes.c and classes_test.c for usage.
// - Adds stacktrace() builtin
// - Adds realloc() builtin
// - Adds memcpy() builtin
// - Adds C4 versions of malloc(), realloc(), memcpy() (not used when compiled)


// char, int, and pointer types
// if, while, return, and expression statements
// ability to obtain and call function pointers
// just enough features to allow self-compilation and a bit more
//
// Originally written by Robert Swierczek

#include "c4.h"
#include "c4m_util.c"

// Allow testing stacktrace() by running in c4_multiload
#define stacktrace renamed_when_not_C4
//void stacktrace () { }
#undef stacktrace
#define stacktrace()

char *p, *lp, // current position in source code
     *data;   // data/bss pointer

int *e, *le,  // current position in emitted code
    *id,      // currently parsed identifier
    *sym,     // symbol table (simple list of identifiers)
    tk,       // current token
    ival,     // current token value
    ty,       // current expression type
    loc,      // local variable offset
    line,     // current line number
    src,      // print source and assembly flag
    debug,    // print executed instructions
    mode_priv;// Privilege mode
// A special stack for traps
// int *trap_stack, *trap_sp, *trap_bp;
int time_altmode; // use alternate mode for reading time

// tokens and classes (operators last and in precedence order)
enum {
  Num = 128, Fun, Sys, Glo, Loc, Id,
  Static, Extern, Attribute, Constructor, Destructor, // Ignored
  Char, Else, Enum, If, Int, Return, Sizeof, While,
  Assign, Cond, Lor, Lan, Or, Xor, And, Eq, Ne, Lt, Gt, Le, Ge, Shl, Shr, Add, Sub, Mul, Div, Mod, Inc, Dec, Brak
};

// Configure codes, for use with C4CF/__c4_configure
enum { CONF_CYCLE_INTERRUPT_INTERVAL, CONF_CYCLE_INTERRUPT_HANDLER, CONF_PRIVS };
enum { PRIV_KERNEL, PRIV_USER };

// C4INFO state
enum {
	C4I_NONE = 0x0,  // No C4 info
	C4I_C4   = 0x1,  // Ultimately running under C4
	C4I_C4M  = 0x2,  // Running under c4m (directly or C4)
	C4I_C4P  = 0x4,  // Running under c4plus
	C4I_HRT  = 0x10, // High resolution timer
	C4I_SIG  = 0x20, // Signals supported
};

// Trap codes
enum {
	// Illegal opcode, allows custom opcodes to be implemented using
	// install_trap_handler()
	TRAP_ILLOP,
	// Hard IRQ generated by c4m under some condition
	TRAP_HARD_IRQ,
	// Soft IRQ generated by user code
	TRAP_SOFT_IRQ,
	// A POSIX signal was received
	TRAP_SIGNAL,
	// Invalid memory read or write
	TRAP_SEGV,
	// Invalid opcode value (specifically with OPCD)
	TRAP_OPV,
};
// TRAP_HARD_IRQ codes
enum {
	HIRQ_CYCLE       // Cycle, runs every X cycles
};

// opcodes
enum { LEA ,IMM ,JMP ,JSR ,JSRI,JSRS,BZ  ,BNZ ,ENT ,ADJ ,LEV ,LI  ,LC  ,SI  ,SC  ,PSH ,
       JMPA,TLEV,
       OR  ,XOR ,AND ,EQ  ,NE  ,LT  ,GT  ,LE  ,GE  ,SHL ,SHR ,ADD ,SUB ,MUL ,DIV ,MOD ,
       OPEN,READ,CLOS,PRTF,MALC,RALC,FREE,MSET,MCMP,MCPY,STRC,ITH ,_OPC,_BLT,_TRP,
	   OPCD,_JMP,_ADJ,C4CF,C4CY,TIME,SIGH,SIGI,USLP,INFO,OPSL,
	   EXIT };
char *c4m_opcodes;
void c4m_setup_opcodes () {
	c4m_opcodes =
	   "LEA ,IMM ,JMP ,JSR ,JSRI,JSRS,BZ  ,BNZ ,ENT ,ADJ ,LEV ,LI  ,LC  ,SI  ,SC  ,PSH ,"
       "JMPA,TLEV,"
	   "OR  ,XOR ,AND ,EQ  ,NE  ,LT  ,GT  ,LE  ,GE  ,SHL ,SHR ,ADD ,SUB ,MUL ,DIV ,MOD ,"
	   "OPEN,READ,CLOS,PRTF,MALC,RALC,FREE,MSET,MCMP,MCPY,STRC,ITH ,_OPC,_BLT,_TRP,"
	   "OPCD,_JMP,_ADJ,C4CF,C4CY,TIME,SIGH,SIGI,USLP,INFO,OPSL,"
	   "EXIT,";
}
char *c4m_builtins;
void c4m_setup_builtins () {
	c4m_builtins = "static extern __attribute__ constructor destructor " // Ignored, used by c4cc
	               "char else enum if int return sizeof while "
	               "open read close printf malloc realloc free memset memcmp memcpy stacktrace "
	               "install_trap_handler __opcode __builtin __c4_trap __c4_opcode "
                   "__c4_jmp __c4_adjust __c4_configure __c4_cycles __time __c4_signal __c4_sigint "
				   "__c4_usleep __c4_info __c4_ops_list "
				   "exit void main";
}
char __toupper (char ch) {
    if (ch >= 'a' && ch >= 'z')
        ch = 'A' + (ch - 'a');
    return ch;
}
// Match up to 4 characters, stopping on space or end of string
// @return 0 on no match, 1 on match
int __opcode_match (char *op_a, char *op_b) {
    int m;
    m = 0;
    while(m < 4) {
        if (*op_a == 0 || *op_a == ' ')
            return 1;
        else if(*op_b == 0 || *op_b == ' ')
            return 1;

        // convert to uppercase for comparison
        if(__toupper(*op_a++) != __toupper(*op_b++))
            return 0;
        m++;
    }
    return 1;
}
int __opcode (char *name) {
    int r;
    char *ops;

	if (!name) {
		printf("c4m: invalid opcode given?\n");
		return -1;
	}
    if (!(ops = c4m_opcodes)) {
		printf("c4m: lost opcodes?\n");
		return -1;
	}
    r = 0;
    while(r <= EXIT) {
        if (__opcode_match(name, ops))
            return r;
        ++r;
        ops = ops + 5; // size of each member in the string
    }
    return -1;
}
int __builtin (char *name) {
    // TODO
    return -1;
}

// types
enum { CHAR, INT, PTR };

// identifier offsets (since we can't create an ident struct)
enum { Tk, Hash, Name, Class, Type, Val, HClass, HType, HVal, Idsz };

void dump_exit (int code) {
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
          printf("%8.4s", &c4m_opcodes[*++le * 5]);
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
  int t, *d;

  if (!tk) { printf("%d: unexpected eof in expression\n", line); exit(-1); }
  else if (tk == Num) { *++e = IMM; *++e = ival; next(); ty = INT; }
  else if (tk == '"') {
    *++e = IMM; *++e = ival; next();
    while (tk == '"') next();
    data = (char *)((int)data + sizeof(int) & -sizeof(int)); ty = PTR;
  }
  else if (tk == Sizeof) {
    next(); if (tk == '(') next(); else { printf("%d: open paren expected in sizeof\n", line); exit(-1); }
    ty = INT; if (tk == Int) next(); else if (tk == Char) { next(); ty = CHAR; }
    while (tk == Mul) { next(); ty = ty + PTR; }
    if (tk == ')') next(); else { printf("%d: close paren expected in sizeof\n", line); exit(-1); }
    *++e = IMM; *++e = (ty == CHAR) ? sizeof(char) : sizeof(int);
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
    *++e = IMM; *++e = (ty > PTR) ? sizeof(int) : sizeof(char);
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
      *d = (int)(e + 3); *++e = JMP; d = ++e;
      expr(Cond);
      *d = (int)(e + 1);
    }
    else if (tk == Lor) { next(); *++e = BNZ; d = ++e; expr(Lan); *d = (int)(e + 1); ty = INT; }
    else if (tk == Lan) { next(); *++e = BZ;  d = ++e; expr(Or);  *d = (int)(e + 1); ty = INT; }
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
      if ((ty = t) > PTR) { *++e = PSH; *++e = IMM; *++e = sizeof(int); *++e = MUL;  }
      *++e = ADD;
    }
    else if (tk == Sub) {
      next(); *++e = PSH; expr(Mul);
      if (t > PTR && t == ty) { *++e = SUB; *++e = PSH; *++e = IMM; *++e = sizeof(int); *++e = DIV; ty = INT; }
      else if ((ty = t) > PTR) { *++e = PSH; *++e = IMM; *++e = sizeof(int); *++e = MUL; *++e = SUB; }
      else *++e = SUB;
    }
    else if (tk == Mul) { next(); *++e = PSH; expr(Inc); *++e = MUL; ty = INT; }
    else if (tk == Div) { next(); *++e = PSH; expr(Inc); *++e = DIV; ty = INT; }
    else if (tk == Mod) { next(); *++e = PSH; expr(Inc); *++e = MOD; ty = INT; }
    else if (tk == Inc || tk == Dec) {
      if (*e == LC) { *e = PSH; *++e = LC; }
      else if (*e == LI) { *e = PSH; *++e = LI; }
      else { printf("%d: bad lvalue in post-increment\n", line); exit(-1); }
      *++e = PSH; *++e = IMM; *++e = (ty > PTR) ? sizeof(int) : sizeof(char);
      *++e = (tk == Inc) ? ADD : SUB;
      *++e = (ty == CHAR) ? SC : SI;
      *++e = PSH; *++e = IMM; *++e = (ty > PTR) ? sizeof(int) : sizeof(char);
      *++e = (tk == Inc) ? SUB : ADD;
      next();
    }
    else if (tk == Brak) {
      next(); *++e = PSH; expr(Assign);
      if (tk == ']') next(); else { printf("%d: close bracket expected\n", line); exit(-1); }
      if (t > PTR) { *++e = PSH; *++e = IMM; *++e = sizeof(int); *++e = MUL;  }
      else if (t < PTR) { printf("%d: pointer type expected\n", line); exit(-1); }
      *++e = ADD;
      *++e = ((ty = t - PTR) == CHAR) ? LC : LI;
    }
    else { printf("%d: compiler error tk=%d\n", line, tk); exit(-1); }
  }
}

void stmt()
{
  int *a, *b, t;

  if (tk == If) {
    next();
    if (tk == '(') next(); else { printf("%d: open paren expected\n", line); exit(-1); }
    expr(Assign);
    if (tk == ')') next(); else { printf("%d: close paren expected\n", line); exit(-1); }
    *++e = BZ; b = ++e;
    stmt();
    if (tk == Else) {
      *b = (int)(e + 3); *++e = JMP; b = ++e;
      next();
      stmt();
    }
    *b = (int)(e + 1);
  }
  else if (tk == While) {
    next();
    a = e + 1;
    if (tk == '(') next(); else { printf("%d: open paren expected\n", line); exit(-1); }
    expr(Assign);
	// TODO: if (tk == Num && ival) for while(1) ? 
    if (tk == ')') next(); else { printf("%d: close paren expected\n", line); exit(-1); }
    *++e = BZ; b = ++e;
    stmt();
    *++e = JMP; *++e = (int)a;
    *b = (int)(e + 1);
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
  while(t > PTR) { ptrcount++; t = t - PTR; }
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

void print_stacktrace (int *pc_orig, int *idmain, int *idmax, int *bp, int *sp) {
    int *pc, found, done, *idold, *t, range, depth;
    int comparisons; // debug info
	int cycles;
	int distance_max;

	distance_max = 0xFF; // distance from pc to search
    pc = pc_orig + 1; // Gets decremented in main loop
    idold = id;
    done = 0;
    t = 0;
    depth = 0;
	cycles = 0;

	//printf("idmain: 0x%lx  idmax: 0x%lx  difference = %ld\n",
	//        idmain,        idmax,        idmax - idmain);

    while (!done) {
		// TODO: disabled
	    if (++cycles > 0x1FFFF) {
		  printf("c4m: BUG ** print_stacktrace couldnt find function entry (1)\n");
		  return;
		}
        found = 0;
        comparisons = 0;
        while(!found && (pc_orig - pc) < distance_max) {
            id = idmain;
            --pc;
            while(!found && id[Tk] && id <= idmax) {
                ++comparisons;
                if (id[Tk] == Id && id[Class] == Fun && pc == (int *)id[Val]) {
                    t = id;
                    found = 1;
                }
                id = id + Idsz;
				// printf(" (%d comparisons, id(0x%lx)[Tk] == %d, id <= idmax = %d)\n", comparisons, id, id[Tk], id <= idmax);
				// printf("id(0x%ld) <= idmain(0x%ld): %d\n", id, idmax, id <= idmax);
            }
			// printf(" pc distance: %d\n", pc_orig - pc);
        }
		if (pc_orig - pc < distance_max) {
			if (depth++) printf("%*s", depth - 1, " ");
			print_symbol(t);
			// printf(" (%d cycles)\n", cycles);
			// Finish when we encounter main
			if (t == idmain)
				done = found = 1;
			// find stack return addresss: simulate LEV
			sp = bp;
			bp = (int*)*sp++;
			pc = (int*)*sp++;
		} else {
		  printf("c4m: BUG ** print_stacktrace couldnt find function entry (2)\n");
		  return;
		}
    }
}

#define C4_ONLY 0
#if C4_ONLY
int pending_signal; // if a signal is pending
int *signal_handlers;
int __c4_signal_init () { return 0; }
// Custom implementations for C4 (C library versions used when compiling c4_multiload)
//  - signal
int *__c4_signal (int sig, int *handler) {
	// printf("c4: no signal support\n");
	return 0;
}
void __c4_signal_shutdown () { }
int __c4_sigint () { return 0; }
void spin (int cycles) {
	while (cycles-- > 0)
		// random instructions that don't modify cycles value
		cycles = cycles | (cycles ^ cycles);
}
int c4_usleep (int useconds) {
	// cannot yield, just spin for a bit
	spin(1000);
}
//  - malloc
//    This version reserves an additional word of space for writing the size of the allocated block.
//    This is useful for a realloc implementation.
//  - memcpy
//  - realloc
//void *c4_malloc (int size) {
//  int *p;
//  if (!size) return 0;
//  if (!(p = malloc(size + sizeof(int))))
//    return p;
//  *p++ = size;
//  //printf("c4_malloc(%d) = real malloc(%d) = %p\n", size, size + sizeof(int), (void*)p);
//  //stacktrace();
//  return (void*)p;
//}
//void c4_free (void *ptr) {
//  int *p;
//  //stacktrace();
//  //free(ptr - sizeof(int));
//  p = (int *)ptr;
//  p--;
//  free(p);
//}
// Implement C4 version of memcpy
void *c4_memcpy(void *dst, void *src, int len) {
  int *di, *si, i, max;
  char *dc, *sc;

  //printf("c4_memcpy(%p, %p, %d)\n", dst, src, len);
  //stacktrace();
  i = 0;
  if ((int)dst % sizeof(int) == 0 &&
      (int)src % sizeof(int) == 0 &&
      len % sizeof(int) == 0) {
    // Word copy
    di = (int*)dst; si = (int*)src; i = 0;
    max = len / sizeof(int);
    while(i++ < max)
      di[i] = si[i];
  } else {
    // Byte copy
    dc = (char*)dst; sc = (char*)src;
    while(i++ < len)
      dc[i] = sc[i];
  }
  return dst;
}
// C4 version of realloc, respecting the various quirks for passing 0 in parameters.
//void *c4_realloc (void *ptr, int newsize) {
//  void *new_ptr;
//  int  *old_ptr;
//  int   len;
//
//  //printf("c4_realloc(%p, %d)\n", ptr, newsize);
//
//  // realloc(0, size) => malloc(size)
//  if (ptr == 0)
//    return c4_malloc(newsize);
//
//  // realloc(ptr, 0) => free(ptr)
//  if (newsize == 0) {
//    c4_free(ptr);
//    return 0;
//  }
//
//  old_ptr = (int*)ptr - 1;
//  //printf("c4_realloc: old length: %d\n", *old_ptr);
//  // C4 only: if newsize < old size, return unchanged
//  if (*old_ptr > newsize)
//    return ptr;
//
//  // If malloc fails, return old pointer and data unchanged
//  if (!(new_ptr = c4_malloc(newsize)))
//    return ptr;
//
//  // copy from original to new respecting newsize
//  len = *old_ptr;
//  if (len > newsize) len = newsize;
//  c4_memcpy(new_ptr, ptr, len);
//  c4_free(ptr);
//  return new_ptr;
//}
// allocated by main
char *c4_time_buf;
enum { C4_TIME_BUF_SZ = 32 };
// Attempt to read from a time source.
// This is hacky, it currently reads the file /proc/uptime for
// a time reference.
// This is meant to be fast, but it is not very pretty
// TODO: some systems have different formats for uptime.
//       On Ubuntu-x64, the first number only updates every second, and the
//       second number updates every 100ms.
//       This is reverse on Debian-pi.
int c4_time () {
	int fd, number, r;
	char *buf, ch;

	if ((fd = open("/proc/uptime", 0)) < 0) {
		printf("c4m: unable to open uptime file\n");
		return 0;
	}

	buf = c4_time_buf;
	if (!buf) {
		printf("c4m: buffer went away\n");
		return 0;
	}
	r = read(fd, buf, C4_TIME_BUF_SZ);
	close(fd);
	if (r < 0) {
		printf("c4m: read returned %d, buf: '%s' (0x%lx)\n", r, buf, buf);
		return 0;
	}

	r = C4_TIME_BUF_SZ;
	if (!time_altmode) {
		// Find first space
		while(*buf++ != ' ') {
			--r;
		}
	}

	number = 0;
	while(--r > 0 && *buf) {
		ch = *buf;
		if (ch == '.' || ch == ' ') {
			// convert to milliseconds
			// printf("c4: got number: %ld\n", number);
			if (time_altmode)
				return number * 1000;
			else
				return number * 100;
		}
		number = (number * 10) + (ch - '0');
		++buf;
	}
}
// Plain C4 (not compiled natively)
int c4_plain () { return 1; }
int c4_info  () {
	// No HRT or SIG support
	return C4I_C4 | C4I_C4M;
}
#else
// When compiled natively, calls real malloc and realloc
//#define c4_malloc(s)       malloc(s)
//#define c4_free(p)         free(p)
//#define c4_realloc(p,ns)   realloc(p, ns)
#define c4_memcpy(d,s,n)   memcpy(d, s, n)
#define c4_time()          c4m_time()
#define c4_plain()         0 /* Plain C4 or compiled c4m natively? */
#define c4_info()          (C4I_C4M | C4I_HRT | C4I_SIG)
#define c4_usleep(usec)    usleep(usec)
#endif

int  tlev_instruction;

// Cause a trap to occur and update stack and registers so that the given
// handler is executed.
// This is intended to be used by illegal opcode traps, irq handlers, and
// signal events.
// The arguments provided to the trap handler are also the temporary storage
// for sp, bp, a, and pc registers.  The TLEV instruction undoes all this,
// restoring registers from these stack values.
// The stack is setup by writing a pointer to a TLEV instruction,
// followed by the our trap handler's bp. This allows a LEV to point
// to a TLEV instruction for proper trap exit.
// The stack is further adjusted by inspecting the ENT x instruction,
// and adding that to sp. This allows local variables to work.
// Stack will be presented as such (offsets in words):
//   sp+0 = trap bp      Usually an sp and return pc are on the stack.
//   sp+1 = ptr to TLEV  Argument references (LEA) still expect these here.
//   sp+2 = trap
//   sp+3 = instruction
//   sp+4 = saved sp
//   sp+5 = saved bp
//   sp+6 = saved accumulator
//   sp+7 = return pc / instruction address
// After inspecting the trap handler's ENT x instruction, the stack is
// further adjusted to account for it.
void trap (int type, int parameter, int *handler, int **_sp, int **_bp, int **_pc, int a) {
	int *t, i;
	int *sp, *bp, *pc;


	sp = *_sp;
	bp = *_bp;
	pc = *_pc;

	if (debug || !handler) {
		printf("Trap type ");
		if (type == TRAP_ILLOP) printf("TRAP_ILLOP");
		else if (type == TRAP_HARD_IRQ) printf("TRAP_HARD_IRQ");
		else if (type == TRAP_SOFT_IRQ) printf("TRAP_SOFT_IRQ");
		else if (type == TRAP_SIGNAL) printf("TRAP_SIGNAL");
		else if (type == TRAP_SEGV) printf("TRAP_SEGV");
		else if (type == TRAP_OPV) printf("TRAP_OPV");
		else printf("(unknown %d)", type);
		printf(" start, offending instruction at 0x%X, sp=0x%X, bp=0x%X, handler=0x%X\n", pc - 1, *_sp, *_bp, handler);
	}
	// Sometimes trap() was being called with no handler, causing a segfault in this trap function.
	if (!handler) {
		printf("c4m: missed a trap, no handler installed\n");
		return;
	}

	// Push the details we'll use in TLEV
	t = sp;  // save old stack
	// Push instruction and trap number
	*--sp = type;             // printf("*sp(0x%X) = trap type %d\n", sp, *sp);
	*--sp = parameter;        // printf("*sp(0x%X) = parameter %d (at 0x%X)\n", sp, *sp, pc - 1);
	// Push the registers. These can be updated, they are restored by TLEV.
	*--sp = (int)a;           // printf("*sp(0x%X) = a %d\n", sp, a);
	*--sp = (int)bp;          // printf("*sp(0x%X) = bp 0x%X\n", sp, *sp);
	*--sp = (int)t;           // printf("*sp(0x%X) = sp 0x%X\n", sp, *sp);
	*--sp = (int)pc;          // printf("*sp(0x%X) = returnpc 0x%X\n", sp, *sp);
	*--sp = (int)&tlev_instruction; // set LEV returnpc to TLEV instruction address
	*--sp;                  // Next position is our handler bp
	// Update BP as if we entered a function, allowing arguments to be referenced.
	// We peek at the ENT x that was skipped to read the number of local variables.
	bp = sp;               // Update handler bp
	*sp = (int)bp;         // Set LEV bp to handler bp
	// Adjust stack to account for local variables
	sp = sp - *(handler + 1);    // printf("adjusted stack using ENT %d\n", *(handler - 1));
	// printf("Arguments pushed: %d, sp=0x%X\n", t - sp, sp);

	// Update passed in registers and set pc to trap handler after ENT x opcode.
	*_sp = sp;
	*_bp = bp;
	*_pc = handler + 2; // +2 to skip ENT

	// update trap values
	//trap_sp = sp;
	//trap_bp = bp;
}

int c4m_main(int argc, char **argv)
{
  int fd, bt, ty, poolsz, printsyms;
  int *pc, *sp, *bp, a; // vm registers
  int i, *t, r; // temps
  char*_p, *_data;       // initial pointer locations
  int *_sym, *_e, *_sp;  // initial pointer locations
  int  verb;
  int cycle, run;
  int cycle_interrupt_interval, *cycle_interrupt_handler;
  int *trap_handler, padding;
  int status, *idmain, *idmax;


  //debug = 1;
  debug = 0;
  verb = 0;
  trap_handler = (int *)0;
  a = 0;
  time_altmode =0 ;
  // Used to protect traps from just returning instead of using TLEV.
  tlev_instruction = TLEV;

  //printf("(C4M) Argc: %lld\n", argc); i = 0; while(i < argc) { printf("(C4M) Argv[%lld] = %s\n", i, argv[i]); ++i; }

  poolsz = 256*1024; // arbitrary size
  printsyms = 0;
  --argc; ++argv;
  if (argc > 0 && **argv == '-' && (*argv)[1] == 'v') { verb = 1; --argc; ++argv; }
  if (argc > 0 && **argv == '-' && (*argv)[1] == 's') { src = 1; --argc; ++argv; }
  if (argc > 0 && **argv == '-' && (*argv)[1] == 'd') { debug = 1; --argc; ++argv; }
  if (argc > 0 && **argv == '-' && (*argv)[1] == 'S') { printsyms = 1; --argc; ++argv; }
  if (argc > 0 && **argv == '-' && (*argv)[1] == 'a') { time_altmode = 1; --argc; ++argv; }
  // TODO: these options broken
  if (argc > 0 && **argv == '-' && (*argv)[1] == 'p') { i = 1; while((*argv)[1 + i++]) poolsz = poolsz / 2; --argc; ++argv; }
  if (argc > 0 && **argv == '-' && (*argv)[1] == 'P') { i = 1; while((*argv)[1 + i++]) poolsz = poolsz * 2; --argc; ++argv; }
  if (argc < 1) { printf("usage: c4_multiload [-v] [-s] [-d] [-p] [-P] file1 [files...] -- args ...\n"); return -1; }

  if (verb) printf("c4m: init...\n");
  if (!(sym = _sym = malloc(poolsz))) { printf("could not malloc(%d) symbol area\n", poolsz); return -1; }
  idmax = sym + (poolsz / sizeof(int)); // end of symbol table
  idmax = idmax - Idsz;                 // minus one element
  if (!(le = e = _e = malloc(poolsz))) { printf("could not malloc(%d) text area\n", poolsz); return -1; }
  if (!(data = _data = malloc(poolsz))) { printf("could not malloc(%d) data area\n", poolsz); return -1; }
  if (!(sp = _sp = malloc(poolsz))) { printf("could not malloc(%d) stack area\n", poolsz); return -1; }
  // if (!(trap_stack = trap_sp = malloc(poolsz))) { printf("could not malloc(%d) trap stack area\n", poolsz); return -1; }
  if ((i = __c4_signal_init())) { printf("c4m: signal init failed with reason %d\n", i); return -1; }

  memset(sym,  0, poolsz);
  memset(e,    0, poolsz);
  memset(data, 0, poolsz);

#if C4_ONLY
  if (!(c4_time_buf = malloc(C4_TIME_BUF_SZ))) { printf("could not malloc(%d) time buffer\n", C4_TIME_BUF_SZ); return -1; }
#endif

  c4m_setup_opcodes();
  c4m_setup_builtins();

  p = c4m_builtins;
  i = Static; while (i <= While) { next(); id[Tk] = i++; } // add keywords to symbol table
  i = OPEN; while (i <= EXIT) { next(); id[Class] = Sys; id[Type] = INT; id[Val] = i++; } // add library to symbol table
  next(); id[Tk] = Char; // handle void type
  next(); idmain = id; // keep track of main

  if (!(lp = p = _p = malloc(poolsz))) { printf("could not malloc(%d) source area\n", poolsz); return -1; }

  if (verb) {
	  i = 0; printf("// (C4M) Argc: %lld\n", argc); while(i < argc) { printf("// (C4M) argv[%lld] = %s\n", i, *(argv + i)); ++i; }
  }

  // Read all specified source files
  r = poolsz - 1;        // track memory remaining
  //printf("// (C4M) arg parsing starts...\n");
  if (argc > 1 && r > 0 && *argv && **argv == '-' && *(*argv + 1) == '-') {
	  ++argc; --argv;
  }
  if (verb) printf("c4m: read...\n");
  while (argc > 0 && r > 0 && !(**argv == '-' && *(*argv + 1) == '-')) {
    //printf("// (C4M) argv '%c' %lld, argv+1 '%c' %lld\n", **argv, **argv, *(*argv + 1), *(*argv + 1));
    if ((fd = open(*argv, 0)) < 0) { printf("could not open(%s)\n", *argv); return -1; }
    if ((i = read(fd, p, r)) <= 0) { printf("read() returned %d\n", i); return -1; }
    p[i] = 0;
    // Advance p to the nul we just wrote, new content will go here
    p = p + i;
    r = r - i;
    close(fd);
    ++argv;
    --argc;
  }
  //++argv; --argc;
  //printf("// (C4M) arg parsing ends. \n");
  if (r == 0) { printf("could not read all source files: exceeded %d (0x%X) bytes\n", poolsz, poolsz); return -1; }
  // Reset pointer to start of code
  p = _p;

  // parse declarations
  if (verb) printf("c4m: compile...\n");
  line = 1;
  next();
  while (tk) {
    bt = INT; // basetype
	if (tk == Static) next(); // Ignore static keyword
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
            if (tk == Sub) { next(); ival = -ival; } // Negative numbers
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
      if (tk == Attribute) {
          next();
          if (tk == '(') next();     // (format)
          if (tk == '(') next();     // ((format))
		  next();     // ((format))
		  // printf("attributes set2, tk now == %d '%c', %.*s\n", tk, tk, 5, p - 5);
		  if (tk == ')') next();     // ((format))
		  // printf("attributes set3, tk now == %d '%c', %.*s\n", tk, tk, 5, p - 5);
		  tk = Id;
		  next();
      }
      if (tk != Id) { printf("%d: bad global declaration\n", line); return -1; }
      //if (id[Class]) { printf("%d: duplicate global definition\n", line); return -1; }
      next();
      id[Type] = ty;
      if (tk == '(') { // function
        // don't overwrite builtins
        if (id >= idmain) {
            id[Class] = Fun;
            id[Val] = (int)(e + 1);
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

  if (printsyms) {
    id = sym;
    a = 0;
    while (id[Tk]) {
        print_symbol(id); printf("\n");
        id = id + Idsz;
        a = a + 1;
    }
    printf("Symbol table: %d entries using %d (0x%X) bytes\n", a, a * Idsz * sizeof(int));
  }
  if (src) return 0;
  if (!(pc = (int *)idmain[Val])) { printf("main() not defined\n"); return -1; }

  if (verb) printf("c4m: prepare...\n");
  // setup stack
  bp = sp = (int *)((int)sp + poolsz);
  // printf("//bp = 0x%X\n", bp);
  *--sp = EXIT; // printf("//sp(0x%X) = EXIT (0x%X)\n", sp, *sp); // call exit if main returns
  *--sp = PSH; t = sp; // printf("//sp(0x%X) = PSH (0x%X)\n", sp, *sp);
  *--sp = argc; // printf("//sp(0x%X) = argc (0x%X)\n", sp, *sp);
  *--sp = (int)argv; // printf("//sp(0x%X) = argv (0x%X)\n", sp, *sp);
  *--sp = (int)t; // printf("//sp(0x%X) = t (0x%X)\n", sp, t);
  // setup trap stack
  // trap_bp = trap_sp = (int *)((int)trap_sp + poolsz);

  // Used in debug output for value alignment
  padding = sizeof(int);

  // Free what we can now
  free(_p);
  //free(_sym);

  // run...
  if (verb) printf("c4m: run!\n");
  run = 1;
  cycle = 0;
  status = 0;

  cycle_interrupt_interval = 0;
  cycle_interrupt_handler = 0;

  while (run) {
	++cycle;

	// Allow a cycle interrupt. Since cycle is incremented above, this will
	// not interrupt the first cycle.
	if (cycle_interrupt_interval && !(cycle % cycle_interrupt_interval)) {
		//printf("!trap_hard_irq %d using handler 0x%X\n", cycle_interrupt_interval, cycle_interrupt_handler);
		trap(TRAP_HARD_IRQ, HIRQ_CYCLE, cycle_interrupt_handler, &sp, &bp, &pc, a);
	// Check for pending signals from the signal handler
	} else if (pending_signal) {
		// printf("c4m: trapping pending signal %d\n", pending_signal);
		trap(TRAP_SIGNAL, pending_signal, (int *)signal_handlers[pending_signal], &sp, &bp, &pc, a);
		pending_signal = 0;
	}

    i = *pc++;
    // This opcode is handled here so debug output can show the opcode
    if (i == OPCD) {
        i = *sp;
        if (i <= ADJ) {
            printf("%.4s does not support opcodes requiring arguments (%.4s given)\n",
                   &c4m_opcodes[OPCD * 5], &c4m_opcodes[i * 5]);
			// Raise an OPV trap
			trap(TRAP_OPV, i, trap_handler, &sp, &bp, &pc, a);
			// change opcode to a cycle count request (harmless)
			i = C4CY;
        }
    }

    if (debug) {
      // The output here is split into multiple calls because C4's printf only pushes
      // up to 6 arguments.
      printf("0x%-*X %-*d ", padding, pc - 1, padding, cycle);
      printf("A=0x%-*X> ", padding, a);
      if (i >= 0 && i <= EXIT) {
          printf("%.4s", &c4m_opcodes[i * 5]);
      } else {
          printf("unknown %-*d (0x%X)", padding, i, i);
      }
      if (i <= ADJ) printf(" %d\n", *pc); else printf("\n");
    }

    if      (i == LEA) a = (int)(bp + *pc++);                             // load local address
    else if (i == IMM) a = *pc++;                                         // load global address or immediate
    else if (i == JMP) pc = (int *)*pc;                                   // jump
    else if (i == JMPA) pc = (int *)a;                                    // jump using accumulator
    else if (i == _JMP) pc = (int *)*sp++;                                // jump using __c4_jmp
    else if (i == JSR) { *--sp = (int)(pc + 1); pc = (int *)*pc; }        // jump to subroutine
    else if (i == JSRI) { *--sp = (int)(pc + 1); pc = (int *)*pc; pc = (int *)*pc;}  // jump to subroutine indirect
    else if (i == JSRS) { *--sp = (int)(pc + 1); pc = (int*)*(bp + *pc); }  // jump to subroutine indirect on stack
    else if (i == BZ)  pc = a ? pc + 1 : (int *)*pc;                      // branch if zero
    else if (i == BNZ) pc = a ? (int *)*pc : pc + 1;                      // branch if not zero
    else if (i == ENT) { *--sp = (int)bp; bp = sp; sp = sp - *pc++; }     // enter subroutine
    else if (i == ADJ)  sp = sp + *pc++;                                  // stack adjust
    else if (i == _ADJ) sp = sp + *sp;                                    // stack adjust callable function
    else if (i == LEV) {                                                  // leave subroutine
		sp = bp; // printf("//LEV: sp = bp 0x%X\n", bp);
		bp = (int *)*sp++; // printf("//LEV: bp = 0x%X loaded from 0x%X\n", bp, sp - 1);
		pc = (int *)*sp++; // printf("//LEV: pc = 0x%X loaded from 0x%X\n", sp, sp - 1);
	}
    //else if (i == LI)  a = *(int *)a;                                     // load int
    else if (i == LI)  {
//#ifdef SEGFAULT_TRACING
//		// Enable segfault tracing for certain problematic values to find out where
//      // the issue is occurring.
//		if (a >= 0 && a <= 0xFFFF || a == 0xffffffffffffffff) {
//			print_stacktrace(pc, idmain, idmax, bp, sp);
//			cycle_interrupt_interval = 0;
//			trap(TRAP_SEGV, a, trap_handler, &sp, &bp, &pc, a);
//		} else
//#endif
			a = *(int *)a;                                     // load int
	}
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
    else if (i == PRTF) {
        r = pc[1];
        t = sp + r;
        // Fix potential access violation by not pushing arguments not given
        if (r == 1) a = printf((char*)t[-1]);
        else if (r == 2) a = printf((char*)t[-1], t[-2]);
        else if (r == 3) a = printf((char*)t[-1], t[-2], t[-3]);
        else if (r == 4) a = printf((char*)t[-1], t[-2], t[-3], t[-4]);
        else if (r == 5) a = printf((char*)t[-1], t[-2], t[-3], t[-4], t[-5]);
        else if (r == 6) a = printf((char*)t[-1], t[-2], t[-3], t[-4], t[-5], t[-6]);
        else if (r == 7) a = printf((char*)t[-1], t[-2], t[-3], t[-4], t[-5], t[-6], t[-7]);
        else { printf("Too many arguments to printf!\n"); exit(-1); }
    }
    else if (i == MALC) a = (int)malloc(*sp);
    //else if (i == RALC) a = (int)c4_realloc((int*)sp[1], *sp);
    else if (i == FREE) free((void *)*sp);
    else if (i == MSET) a = (int)memset((char *)sp[2], sp[1], *sp);
    else if (i == MCMP) a = memcmp((char *)sp[2], (char *)sp[1], *sp);
    else if (i == MCPY) a = (int)c4_memcpy((void*)sp[2], (void*)sp[1], *sp);
    else if (i == STRC) print_stacktrace(pc, idmain, idmax, bp, sp);
    else if (i == EXIT) {
        //printf("exit(%d) cycle = %d\n", *sp, cycle);
        status = *sp; run = 0;
    } else if (i == _OPC) { // return an opcode
      a = __opcode((char *)*sp);
      //printf("_OPCD: got %d (0x%X) from request %s\n", a, a, (char *)*sp);
    } else if (i == OPSL) { // return all opcodes
      a = (int)c4m_opcodes; // TODO: copy?
    } else if (i == TLEV) { // trap leave
        // Restore stack to TLEV parameters
        sp = bp;
        // sp will be overwritten below, so to be less convoluted use a temporary.
        t  = sp;
        //if (1) {
        //    // Print stack values for debugging
        //    r = 0;
        //    while (r < 8) {
        //        printf("  *sp+%d(0x%X) = %d 0x%X\n", r, t + r, *(t + r), *(t + r));
        //        ++r;
        //    }
        //}
        t = t + 2;
        // Grab register values from the stack, which may have been updated by
        // the trap handler.
        // Commenting the debug features out is tacky, but better than slowing down c4m under c4
        pc = (int *)*t++;    // printf("From 0x%X, loaded saved returnpc 0x%X\n", t - 1, pc);
        sp = (int *)*t++;    // printf("From 0x%X, loaded saved sp 0x%X\n", t - 1, sp);
        bp = (int *)*t++;    // printf("From 0x%X, loaded saved bp 0x%X\n", t - 1, bp);
        a  = (int  )*t++;    // printf("From 0x%X, loaded saved a %d\n", t - 1, a);
        //printf("Resume from pc 0x%X\n", pc);
	}
	else if (i == C4CY) a = cycle;
	else if (i == SIGI) a = __c4_sigint();
    else if (i == TIME) a = c4_time();
	else if (i == USLP) a = c4_usleep(*sp);
    else if (i == ITH) { // install trap handler
        if (!*sp) {
            // Remove trap handler
			a = (int)trap_handler;
            trap_handler = 0;
        } else {
            // Function address, skipping ENT x. Stack is adjusted in trap handler
            // below by inspecting the skipped ENT x.
			a = (int)trap_handler;
            trap_handler = (int *)*sp;
		}
	} else if (i == C4CF) { // Configure system: __c4_configure(CONF_*, value)
		if (sp[1] == CONF_CYCLE_INTERRUPT_INTERVAL) {
			a = cycle_interrupt_interval;
			cycle_interrupt_interval = sp[0];
#if 0
			// C4/C4CC only
			// printf("(c4m: cycle interval set to %d\n", sp[0]);
#endif
		} else if(sp[1] == CONF_CYCLE_INTERRUPT_HANDLER) {
			a = (int)cycle_interrupt_handler;
			cycle_interrupt_handler = (int *)sp[0];
			// printf("(c4m: cycle handler set to 0x%lx\n", sp[0]);
		} else {
			printf("c4m: C4CF issue\n");
			return -100;
		}
	}
	else if (i == SIGH) a = (int)__c4_signal(sp[1], (int *)sp[0]);
	else if (i == INFO) a = c4_info();
    else if (i == _TRP) {
      // __c4_trap(type, signal)
      // Trigger a trap
      trap(sp[1], sp[0], trap_handler, &sp, &bp, &pc, a);
    } else {
        if (trap_handler == 0) {
            printf("unknown instruction = %d! cycle = %d\n", i, cycle); status = -1; run = 0;
        } else {
			// printf("c4m: illegal opcode %d, invoking trap handler 0x%X\n", i, trap_handler);
			// Disable cycle interrupt
			cycle_interrupt_interval = 0;
			trap(TRAP_ILLOP, i, trap_handler, &sp, &bp, &pc, a);
        }
    }
  }

  // free memory
  //free(_p);
  free(_sym);
  free(_e);
  free(_data);
  free(_sp);
  // free(trap_stack);
#if C4_ONLY
  free(c4_time_buf);
#endif
  __c4_signal_shutdown();

  return status;
}

#ifndef NO_C4M_MAIN
int main(int argc, char **argv) {
	return c4m_main(argc, argv);
}
#endif
