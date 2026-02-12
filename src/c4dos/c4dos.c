//
// C4 DOS  (C4 Diskless Operating System)
//
// Provides a single-tasking operating system, with the goal
// being to entirely self-host C4KE.
//
// Design:
//  o All syscalls are replaced with JSR to an equivalent function.
//  o These syscalls themselves can be overridden by client code.
//
//  o CONFIG.SYS:
//    - FILES=nn                   No effect
//    - BUFFERS=nn                 No effect
//    - DEVICE=file.c [arguments]  Load a device driver
//    - SHELL=file.c [arguments]   Specify shell (default: cmd.c)
//
// o AUTOEXEC.BAT should work as in a normal DOS.
//
// o TODO: fs.txt support, right now theres no directory listing support.
//

#include <c4.h>
#pragma GCC diagnostic ignored "-Wformat"
#pragma GCC diagnostic ignored "-Wformat-security"
#pragma GCC diagnostic ignored "-Wunused-result"

char *VERSION () { return "0.01"; }

// -----------------------
// Configurables
// -----------------------
enum {
	DEFAULT_SYSTABLE_SIZE = 32,
	DEFAULT_SHELL_ARGV    = 512,
	JAILBREAKMAX_SEARCH = 512
};

// -----------------------
// Globals
// -----------------------
int shell_argc;
char **shell_argv;
int option_systable_size;
int option_shell_argv;

// ------------------------
// C4 compiler globals
// ------------------------
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
    debug;    // print executed instructions

// tokens and classes (operators last and in precedence order)
enum {
  Num = 128, Fun, Sys, Glo, Loc, Id,
  Char, Else, Enum, If, Int, Return, Sizeof, While,
  Assign, Cond, Lor, Lan, Or, Xor, And, Eq, Ne, Lt, Gt, Le, Ge, Shl, Shr, Add, Sub, Mul, Div, Mod, Inc, Dec, Brak
};

// opcodes
enum { LEA ,IMM ,JMP ,JSR ,BZ  ,BNZ ,ENT ,ADJ ,LEV ,LI  ,LC  ,SI  ,SC  ,PSH ,
       OR  ,XOR ,AND ,EQ  ,NE  ,LT  ,GT  ,LE  ,GE  ,SHL ,SHR ,ADD ,SUB ,MUL ,DIV ,MOD ,
       OPEN,READ,CLOS,PRTF,MALC,FREE,MSET,MCMP,EXIT,
	   // These extended opcodes generate JSR like the above syscalls
	   WRIT,PUTS,PUTC
};

// types
enum { CHAR, INT, PTR };

// identifier offsets (since we can't create an ident struct)
enum { Tk, Hash, Name, Class, Type, Val, HClass, HType, HVal, Idsz };


// ---------------------
// Syscall table handling
// ---------------------

enum {
	SYSTBL_DATA,
	SYSTBL_USED,
	SYSTBL_SIZE,
	SYSTBL__Sz
};

int *syscall_table, *syscall_table_data;

// ------------------------------
// Jailbreak: offers a few useful functions:
//   - get_caller_address(): Find the start address of the calling function
//   - __c4_invoke_stub()  : Used by c4_invokeX
//   - c4_invokeX(...)     : Call arbritrary C4 function
// ------------------------------

// This function uses local references to find the return PC on the stack.
// It then searches the code backwards to find the ENT opcode.
int *get_caller_address () {
	int *addr, *next, i;

	// Return pc is stored above local variables
	addr = (int *)(*(&addr + 2));

	// Find ENT x
	i = 0;
	next = addr;
	while (++i <= JAILBREAKMAX_SEARCH) {
		--next;
		if (*addr == ENT) { // Possibly found
			// Ensure it wasn't an argument to some other opcode
			if (*next > ADJ)
				return addr;
		}
		addr = next;
	}

	printf("c4dos/get_calling_address: couldn't find entry\n");
	return 0;
}

// invoke stub
int *__c4_invoke_stub_addr;
int  __c4_invoke_stub () {
	// First call, get our address then overwrite ourselves with the JMP opcode.
	if (!(__c4_invoke_stub_addr = get_caller_address())) {
		printf("c4_invoke: failed to get our function address\n");
		exit(99);
	}

	// Advance the pointer so that the target is already correct.
	*__c4_invoke_stub_addr++ = JMP;
	// Doesn't matter what we return here
	return 0;
}

// c4_invokeX methods

int c4_invoke0 (int *ptr) {
	*__c4_invoke_stub_addr = (int)ptr;
	return __c4_invoke_stub();
}

int c4_invoke1 (int *ptr, int arg) {
	*__c4_invoke_stub_addr = (int)ptr;
	return __c4_invoke_stub(arg);
}

int c4_invoke2 (int *ptr, int arg1, int arg2) {
	*__c4_invoke_stub_addr = (int)ptr;
	return __c4_invoke_stub(arg1, arg2);
}

// ------------------------
// Default syscall handlers
// ------------------------
int *defsys_open_addr;
int defsys_open (char *file, int mode) {
	if (!defsys_open_addr) { defsys_open_addr = get_caller_address(); return 0; }
	return open(file, mode);
}
int *defsys_read_addr;
int defsys_read (int fd, char *buf, int count) {
	if (!defsys_read_addr) { defsys_read_addr = get_caller_address(); return 0; }
	return read(fd, buf, count);
}
int *defsys_write_addr;
int defsys_write (int fd, char *buf, int count) {
	if (!defsys_write_addr) { defsys_write_addr = get_caller_address(); return 0; }
	printf("C4DOS: write failed, volume is write-protected\n");
	return -1;
}
int *defsys_close_addr;
int defsys_close (int fd) {
	if (!defsys_close_addr) { defsys_close_addr = get_caller_address(); return 0; }
	return close(fd);
}
int *defsys_puts_addr;
int defsys_puts (char *buf) {
	if (!defsys_puts_addr) { defsys_puts_addr = get_caller_address(); return 0; }
	return printf("%s", buf);
}
int *defsys_putc_addr;
int defsys_putc (char ch) {
	if (!defsys_putc_addr) { defsys_putc_addr = get_caller_address(); return 0; }
	return printf("%c", ch);
}
int *defsys_malloc_addr;
int defsys_malloc (int size) {
	if (!defsys_malloc_addr) { defsys_malloc_addr = get_caller_address(); return 0; }
	return (int)malloc(size);
}
int *defsys_free_addr;
int defsys_free (void *ptr) {
	if (!defsys_free_addr) { defsys_free_addr = get_caller_address(); return 0; }
	free(ptr);
	return 0;
}
int *defsys_memset_addr;
int defsys_memset (void *s, char c, int n) {
	if (!defsys_memset_addr) { defsys_memset_addr = get_caller_address(); return 0; }
	memset(s, c, n);
	return 0;
}
int *defsys_memcmp_addr;
int defsys_memcmp (void *s1, void *s2, int n) {
	if (!defsys_memcmp_addr) { defsys_memcmp_addr = get_caller_address(); return 0; }
	return memcmp(s1, s2, n);
}
int *defsys_exit_addr;
int defsys_exit (int fd) {
	if (!defsys_exit_addr) { defsys_exit_addr = get_caller_address(); return 0; }
	printf("C4DOS/TODO: figure out how to perform exit\n");
	exit(-1);
}

int init_syscalls () {
	int *tbl, i;

	if (!(syscall_table = malloc((i = sizeof(int) * SYSTBL__Sz)))) {
		printf("Unable to allocate %ld bytes for syscall table\n", i);
		return -1;
	}
	if (!(tbl = malloc((i = sizeof(int *) * option_systable_size)))) {
		free(syscall_table);
		printf("Unable to allocate %ld bytes for syscall entries\n", i);
		return -1;
	}
	memset(tbl, 0, i);

	syscall_table_data = tbl;
	syscall_table[SYSTBL_DATA] = (int)tbl;
	syscall_table[SYSTBL_USED] = 0;
	syscall_table[SYSTBL_SIZE] = option_systable_size;

	// Setup default syscalls by first getting their addresses
	defsys_open("", 0); defsys_read(0, 0, 0); defsys_write(0, 0, 0); defsys_close(0);
	defsys_puts(""); defsys_putc(0);
	defsys_malloc(0); defsys_free(0);
	defsys_memset(0, 0, 0); defsys_memcmp(0, 0, 0);
	defsys_exit(0);
	// Now store the syscall addresses
	syscall_table_data[OPEN - OPEN] = (int)defsys_open_addr;
	syscall_table_data[READ - OPEN] = (int)defsys_read_addr;
	syscall_table_data[WRIT - OPEN] = (int)defsys_write_addr;
	syscall_table_data[CLOS - OPEN] = (int)defsys_close_addr;
	syscall_table_data[PUTS - OPEN] = (int)defsys_puts_addr;
	syscall_table_data[PUTC - OPEN] = (int)defsys_putc_addr;
	syscall_table_data[MALC - OPEN] = (int)defsys_malloc_addr;
	syscall_table_data[FREE - OPEN] = (int)defsys_free_addr;
	syscall_table_data[MSET - OPEN] = (int)defsys_memset_addr;
	syscall_table_data[MCMP - OPEN] = (int)defsys_memcmp_addr;
	syscall_table_data[EXIT - OPEN] = (int)defsys_exit_addr;

	return 0;
}

void dest_syscalls () {
	free((int *)syscall_table[SYSTBL_DATA]);
	free(syscall_table);
}


// ------------------------
// The entire C4 compiler
// ------------------------
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
          printf("%8.4s", &"LEA ,IMM ,JMP ,JSR ,BZ  ,BNZ ,ENT ,ADJ ,LEV ,LI  ,LC  ,SI  ,SC  ,PSH ,"
                           "OR  ,XOR ,AND ,EQ  ,NE  ,LT  ,GT  ,LE  ,GE  ,SHL ,SHR ,ADD ,SUB ,MUL ,DIV ,MOD ,"
                           "OPEN,READ,CLOS,PRTF,MALC,FREE,MSET,MCMP,EXIT,"[*++le * 5]);
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
      if (d[Class] == Sys) {
        if (d[Val] == PRTF) *++e = d[Val]; // Support PRTF for now
        else { *++e = JSR; *++e = syscall_table_data[d[Val] - OPEN]; }
      }
      else if (d[Class] == Fun) { *++e = JSR; *++e = d[Val]; }
      else { printf("%d: bad function call\n", line); exit(-1); }
      if (t) { *++e = ADJ; *++e = t; }
      ty = d[Type];
    }
    else if (d[Class] == Num) { *++e = IMM; *++e = d[Val]; ty = INT; }
    else {
      if (d[Class] == Loc) { *++e = LEA; *++e = loc - d[Val]; }
      else if (d[Class] == Glo) { *++e = IMM; *++e = d[Val]; }
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
  int *a, *b;

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
    if (tk == ';') next(); else { printf("%d: semicolon expected\n", line); exit(-1); }
  }
}

int compile_and_run(char *file, int argc, char **argv)
{
  int fd, bt, ty, poolsz, *idmain;
  int *bsym, *be;
  char *bdata;
  int *pc, *sp, *bp, a, cycle; // vm registers
  int i, *t; // temps

  if ((fd = open(file, 0)) < 0) { printf("could not open(%s)\n", file); return -1; }

  poolsz = 256*1024; // arbitrary size
  if (!(bsym = sym = malloc(poolsz))) { printf("could not malloc(%d) symbol area\n", poolsz); return -1; }
  if (!(be = le = e = malloc(poolsz))) { printf("could not malloc(%d) text area\n", poolsz); return -1; }
  if (!(bdata = data = malloc(poolsz))) { printf("could not malloc(%d) data area\n", poolsz); return -1; }

  memset(sym,  0, poolsz);
  memset(e,    0, poolsz);
  memset(data, 0, poolsz);

  p = "char else enum if int return sizeof while "
      "open read close printf malloc free memset memcmp exit "
      "write puts putchar "
      "void main";
  i = Char; while (i <= While) { next(); id[Tk] = i++; } // add keywords to symbol table
  i = OPEN; while (i <= EXIT) { next(); id[Class] = Sys; id[Type] = INT; id[Val] = i++; } // add library to symbol table
  while (i <= PUTC) { next(); id[Class] = Sys; id[Type] = INT; id[Val] = i++; }
  next(); id[Tk] = Char; // handle void type
  next(); idmain = id; // keep track of main

  if (!(lp = p = malloc(poolsz))) { printf("could not malloc(%d) source area\n", poolsz); return -1; }
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
      if (id[Class]) { printf("%d: duplicate global definition\n", line); return -1; }
      next();
      id[Type] = ty;
      if (tk == '(') { // function
        id[Class] = Fun;
        id[Val] = (int)(e + 1);
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

  if (!(pc = (int *)idmain[Val])) { printf("main() not defined\n"); i = -1; }
  else if (src) { i = 0; }
  else {
	  printf("C4DOS/compile success, about to invoke...\n");
    i = c4_invoke2(pc, argc, (int)argv);
  }

  free(bsym);
  free(be);
  free(bdata);

  return i;
}

int main (int argc, char **argv) {
	int i;

	printf("C4DOS v %s on C4 %ldbit\n", VERSION(), sizeof(int) * 8);
	option_systable_size = DEFAULT_SYSTABLE_SIZE;
	option_shell_argv = DEFAULT_SHELL_ARGV;

	if (!(shell_argv = malloc((i = sizeof(char) * option_shell_argv)))) {
		printf("Failed to allocate %ld bytes for shell argv\n", i);
		return -1;
	}
	shell_argc = 1; // TODO: SHELL= in CONFIG.SYS
	shell_argv[0] = "cmd.c";
	//shell_argv[1] = "";
	//shell_argc = 4;
	//shell_argv[0] = "c4m.c";
	//shell_argv[1] = "load-c4r.c";
	//shell_argv[2] = "--";
	//shell_argv[3] = "c4ke.c4r";

	printf("C4DOS: setting up syscalls...\n");
	if (i = init_syscalls())
		return i;
	__c4_invoke_stub();
	
	//src = 1; // debug
	printf("C4DOS: Starting SHELL\n");
	compile_and_run("cmd.c", shell_argc, shell_argv);
	//compile_and_run("c4m.c", shell_argc, shell_argv);
	
	free(shell_argv);
	dest_syscalls();
	printf("Thank you for using C4DOS\n");
	return 0;
}
