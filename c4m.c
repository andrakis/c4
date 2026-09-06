// c4m.c - C4 Multiload, an extension of C4, supporting multiple source
//         files and some bells and whistles whilst still running under
//         regular c4. Better results if compiled natively.
//   (Originally) Written by Robert Swierczek
//   (Over-) Extended by Julian Thatcher
//
// Original comments from c4:
//   char, int, and pointer types
//   if, while, return, and expression statements
//   just enough features to allow self-compilation and a bit more
//
//   Written by Robert Swierczek
//
// C4m provides some additional features, namely:
//  o Compatible with C4: C4m is designed to work under plain C4 without
//    any modifications. Some features, such as signal handling and time
//    management, are unavailble when running under C4.
//    - C4KE, the C4 Kernel Experiment, is also designed to run under
//      unmodified C4 (via C4m), with certain features disabled.
//    - For best results, compile natively instead of running under C4.
//  o Traps: certain instructions, as well as a cycle-based counter, can
//           interrupt running code, enabling task switching, signal
//           handling, illegal opcode intervention, protected mode
//           intervention, and more.
//  o Protected mode: IO instructions can trap when in a special 'protected' mode,
//    set by trap handlers before resuming a task, allowing an OS to handle them.
//    - Used by C4KE to implement its own memory management, filesystem, and IO
//      streams.
//  o Calling functions from pointers: any 'int *' can be called as if
//    it were a function pointer. Unlike most compilers, you do not need
//    to declare the function signature, 'int *' is enough:
//      int some_func (int a, int b) { return a + b; }
//      int main () { int *f; f = (int *)&some_func; printf("%d", f(1, 2)); return 0; }
//      - Prints 3
//      - Passing an incorrect number of arguments to a function will result in
//        arguments not referencing the correct stack variable, and thus have the
//        wrong values.
//        + This feature is used by c4cc to implement variable arguments (...), which are
//          otherwise unsupported by C4 or C4m.
//  o New opcodes to support above functionality.
//  o Some additional keywords are supported, but do nothing:
//    - static: has no effect
//    - __attribute__((whatever)): does nothing
//    * c4cc does make use of these keywords. C4m is designed to not care about them.
//
// Usage:
//  gcc -O2 -o c4m c4m.c
//  ./c4m [-sSdpP] <first file.c> [file n.c, ...] [-- arguments...]
// With plain c4:
//  ./c4 c4m.c [-sSdpP] <first file.c> [file n.c, ...] [-- arguments...]
// Example:
//  c4 c4m.c classes.c classes_test.c -- test arguments
//
// Parameters:
//  -s           Print the source and exit (memory leaks)
//  -S           Print source symbol listing
//  -d           Enable debug output during execution
//  -U           Disable protected mode
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
//  2025 / 04 / 27 - Implemented basic protected mode support.
//                 - Added puts(str), putchar(c)
//  2024 / 07 / 13 - C4 versions of malloc(), realloc() removed. Were causing crashes.

// Additional functions:
// - __c4_cycles()                 Request the cycle counter
// - __c4_configure(opt,val)       Configure system settings.
//     Currently two settings are supported, that control the cycle interrupt:
//       CONF_CYCLE_INTERRUPT_INTERVAL  How many cycles between generating interrupt.
//                                      Setting to 0 disables the cycle interrupt.
//       CONF_CYCLE_INTERRUPT_HANDLER   The function address of the handler.
//       The cycle handler signature is:
//       void handler(int type, int ins, int mode, int a, int *bp, int *sp, int *returnpc) {}
//       CONF_TRAP_RESTORES_INTERVAL    1 = TLEV restores the interval that was
//                                      in effect when the trap was taken, so
//                                      the interrupt stays masked across the
//                                      whole handler including its return. A
//                                      handler that opts in and wants to choose
//                                      the interval for the context it is about
//                                      to resume declares an eighth parameter
//                                      ahead of the other seven:
//       void handler(int interval, int type, int ins, int mode, int a, int *bp, int *sp, int *returnpc) {}
//
// - void stacktrace();            Print a stacktrace
// - void install_trap_handler(void (*handler)(int trap, int ins, int mode, int a, int *bp, int *sp, int *returnpc));
//     Install a trap handler. See test_customop.c for more information.
//     A trap occurs under the following conditions:
//       - TRAP_ILLOP:    An unknown opcode is encountered. Allows custom opcodes to be
//                        emulated in C4 code.
//       - TRAP_HARD_IRQ: Currently only generated by the cycle interrupt.
//       - TRAP_SOFT_IRQ: User code requesting an interrupt.
//       - TRAP_SIGNAL:   A signal was received (SIGINT, etc).
//       - TRAP_SEGV:     A process accessed memory it was not entitled to.
//       - TRAP_OPV:      Invalid opcode requested in __opcode().
//       - TRAP_PM_VIOLATION:
//                        Protected mode violation, currently when a syscall like OPEN, MALC, etc
//                        is executed in user mode.
//     The function handler can modify the mode, a, sp, bp, and return pc values.
//     The returnpc is the illegal instruction address + 1 word, but can be adjusted:
//       - If your opcode takes arguments you can adjust the returnpc to skip them.
// - void __c4_opcode (int opcode); Execute the given opcode, which can trap if
//                                  using a custom opcode. Opcode number must be last due to stack
//                                  layout:
//        __c4_opcode(handler, sig, OP_USER_SIGNAL);
//        - Therefore, arguments should be pushed in reverse order.
// - void __c4_jmp (int address);  Jump directly to a given function address.
//   + Used by C4KE in trap handlers.
// - void __c4_adjust (int offset); Adjust the stack. Negative offset grows stack.
// - int __opcode (char *name);     Request the integer value of an opcode.
// - int __builtin (char *name);    Request the opcode for a builtin.
// - Adds JSRI: Jump to SubRoutine Indirect: 'int *f; int call_it(int a) { return f(a); }'
// - Adds JSRS: Jump to SubRoutine on Stack: 'int call_it (int a, int *f){ return f(a); }' or
//                                           'int call_it (int a) { int *f; f = (int *)&func; return f(a); }'
// - Adds &function to get function address. Can be called if stored in an int*.
//   See classes.c and classes_test.c for usage. // TODO: these files moved. Update locationj.
// - Adds stacktrace() builtin
// - Adds realloc() builtin
// - Adds memcpy() builtin
// - ~~Strike -- Adds C4 versions of malloc(), realloc(), memcpy() (not used when compiled) --~~ These
//   implementations were broken. Someone should have coded them better.

#ifndef __C4M_C__
#define __C4M_C__ 1

#define C4M_SIGNALS 1
// FREESTANDING: no headers at all, and everything c4m needs it defines
// itself -- the C4_ONLY arm further down is exactly that set.
//
// This is what a build through raw c4cc produced by accident, because
// c4cc skips every '#' line and so never opened a header. The C4DOS
// rung depends on it and nobody knew: the tree's own <string.h> defines
// memmove() in terms of memcpy(), and memcpy is MCPY, opcode 42, ABOVE
// the base rung c4m-dos32.c4r is pinned to (test-c4bb-rungs). Give that
// build a real preprocessor without this switch and the census refuses
// it, correctly -- the image would need a CPU the player has not
// extended yet.
//
// So the flag exists to ASK for the headerless build. Its companions
// are -DC4_ONLY=1 (the C4 implementations, since there is no host libc
// to call) and -DNOT_NATIVE=1 (which c4.h would otherwise have derived
// from __c4cc__), and c4dos.h/u0.h come in as SOURCE FILES the way c4cc
// has always taken them.
#ifndef C4M_FREESTANDING
#include "c4.h"
#include "c4m_float.h"
#include <c4m_util.h>
#endif

#ifdef __c4cc__
// u0 is the C4KE runtime, and it REFUSES to run under C4DOS by design
// (docs/dos-rung-fixes.md F5: its constructor sees __c4dos_api and
// declines rather than asking a kernel that is not there for 38
// opcodes). So an image meant for a disk C4DOS also boots has to be
// built without it -- ./cpp -DC4M_NO_U0=1 -- and that is a thing to
// ASK FOR, not a thing to get because the compiler happened to have no
// preprocessor and skipped this line.
#ifndef C4M_NO_U0
#include <u0.h>
#endif
#endif

// Configurables
enum {
	// Pool size, in bytes
	// Default memory amount to allocate for stack, code, etc
	POOL_SZ = 262144,
	// Trap offset, in words
	// When a trap occurs, it uses a stack pointer offset from
	// the true stack pointer, such that trap code can alter
	// the stack of the trapping code.
	// If this is too big, fun things happen to memory in C4KE.
	// 0 works too, but doesn't allow stack to grow in a trap handler.
	TRAP_OFFSET = 0x0F
};

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
    pm_avail; // protected mode available? (disable with -U)
// A special stack for traps
// int *trap_stack, *trap_sp, *trap_bp;
int time_altmode; // use alternate mode for reading time
int cycle_interrupt_interval, *cycle_interrupt_handler;

// tokens and classes (operators last and in precedence order)
enum {
  Num = 128, Fun, Sys, Glo, Loc, Id,
  Static, Extern, Attribute, Constructor, Destructor, // Ignored
  Char, Else, Enum, If, Int, Return, Sizeof, While,
  Switch, Case, Default, Break,
  Assign, Cond, Lor, Lan, Or, Xor, And, Eq, Ne, Lt, Gt, Le, Ge, Shl, Shr, Add, Sub, Mul, Div, Mod, Inc, Dec, Brak
};

// switch/break state (see stmt): compiled to an in-memory jumptable
// of absolute addresses, dispatched with JMPA
enum { BRK_MAX = 256, SWITCH_MAX_CASES = 256, SWITCH_MAX_RANGE = 4096 };
int *brk_slots;    // pending break JMP operand slots
int  brk_top, brk_depth;

// Configure codes, for use with C4CF/__c4_configure
enum { CONF_CYCLE_INTERRUPT_INTERVAL, CONF_CYCLE_INTERRUPT_HANDLER, CONF_PRIVS,
       // 1 = TLEV restores the cycle interrupt interval saved at trap
       //     entry, instead of leaving whatever the handler last set.
       CONF_TRAP_RESTORES_INTERVAL };
enum { PRIV_KERNEL, PRIV_USER };

// C4INFO state
enum {
	C4I_NONE = 0x0,  // No C4 info
	C4I_C4   = 0x1,  // Ultimately running under C4
	C4I_C4M  = 0x2,  // Running under c4m (directly or C4)
	C4I_C4P  = 0x4,  // Running under c4plus
	C4I_C4MJS= 0x8,  // Running under C4M.JS host
	C4I_HRT  = 0x10, // High resolution timer
	C4I_SIG  = 0x20, // Signals supported
	C4I_FLT  = 0x40, // Floating point instruction support
	C4I_PROT = 0x80, // Protected mode support
	C4I_TRAPH = 0x400, // A custom trap handler is installed (safe to probe opcodes)
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
	// SYSCALL in protected mode
	TRAP_PM_VIOLATION,
	// Debug trap, used by DBG opcode
	TRAP_DEBUG,
};
// TRAP_HARD_IRQ codes
enum {
	HIRQ_CYCLE       // Cycle, runs every X cycles
};

// Control registers
int cr_timekeeping; // CRTK_*
enum {
	CRTK_DETECT,    // Detect available modes
	CRTK_BUILTIN,   // A builtin time call
	CRTK_UPTIME,    // Slow: read /proc/uptime
	CRTK_CYCLES,    // Cycle-based timekeeping
};

// Part of a test that has been abandoned.
// Instruction mode  : unprotected (default) and protected.
// o Unprotected mode: SYSCALLs operate as usual
// o Protected mode  : SYSCALLs generate a trap, exiting protected mode
enum { MODE_UNPROTECTED, MODE_PROTECTED };

// opcodes
enum {
	// Opcodes
	LEA ,IMM ,JMP ,JSR ,BZ  ,BNZ ,ENT ,ADJ ,LEV ,LI  ,LC  ,SI  ,SC  ,PSH ,
	OR  ,XOR ,AND ,EQ  ,NE  ,LT  ,GT  ,LE  ,GE  ,SHL ,SHR ,ADD ,SUB ,MUL ,DIV ,MOD ,
	// Syscalls
	OPEN,READ,CLOS,PRTF,MALC,FREE,MSET,MCMP,EXIT,
	// C4M Extended opcodes
	PUTC,PUTS,RALC,MCPY,STRC,
	ITH ,_OPC,_BLT,_TRP,OPCD,
	_JMP,_ADJ,C4CF,C4CY,TIME,
	SIGH,SIGI,USLP,INFO,OPSL,
	// C4 Invoke: call a section of code as if it were a C4 function
	C4IV,
	// Unsupported float instruction
	FLT ,
	// Instructions
	JSRI,JSRS,JMPA,TLEV,DBG ,
	// 66-78 are c4mp's (see src/c4mp/c4mp.h). c4m does not implement
	// them and still traps them as TRAP_ILLOP; they are named here so
	// that every mirrored table holds the same names at the same
	// numbers, and so the debug disassembler names them rather than
	// printing "unknown". A guest must keep feature-testing with
	// C4I_SMP rather than by asking for one of these by name -- and
	// note that __opcode() will now answer with a number for them.
	CPUI,CPUN,CPUS,CPUH,
	CAS ,XCHG,FADD,CWAI,CWAK,IPI ,
	LXI ,SXI ,TRAW,
	// The fused opcodes -- see docs/fused-opcodes.md. THE SPLIT IS
	// DELIBERATE. c4m implements exactly three of them:
	//
	//   LDL  a = *(bp+n)      STL  *(bp+n) = a      POPA  a = *sp++
	//
	// which are what a stack-machine code generator needs to treat the
	// frame as registers -- c4th's native backend uses these three and
	// nothing else, and without them >R, DO/LOOP and every spill cost
	// three instructions instead of one.
	//
	// The other seven are compiler-shaped -- they pay for c4cc's and
	// c4lc's output, not for a Forth's -- and they live in c4mp only.
	// c4m is the workhorse that boots C4KE and the machine c4bb models
	// in hardware, where each opcode is microcode and ROM depth on a
	// breadboard. Three is about twelve microsteps; ten is forty-odd.
	// Keeping the seven in c4mp is what lets a c4mp-capable c4bb be an
	// EXTENSION of the base board rather than a second one.
	//
	// All ten are NAMED here so every mirrored table holds the same
	// names at the same numbers, so the disassembler names one rather
	// than printing "unknown", and so c4l can say what an image needs.
	LDL ,LDG ,PSHL,PSHG,LEAP,IMMP,LIP ,ADDL,STL ,POPA,
	// End of instructions
	INS_SIZE,
};
char *c4m_opcodes;
void c4m_setup_opcodes () {
	c4m_opcodes =
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
	// c4mp's, which c4m names but does not implement
	"CPUI,CPUN,CPUS,CPUH,"
	"CAS ,XCHG,FADD,CWAI,CWAK,IPI ,"
	"LXI ,SXI ,TRAW,"
	// fused -- docs/fused-opcodes.md. LDL, STL and POPA are implemented
	// here; the other seven are c4mp's, and named only.
	"LDL ,LDG ,PSHL,PSHG,LEAP,IMMP,LIP ,ADDL,STL ,POPA,";
}
// One place decides which opcodes carry an operand word. Everything that
// walks code -- the debug disassembler, OPCD's refusal -- asks here.
int c4m_has_operand (int i) {
	return i <= ADJ || i == JSRI || i == JSRS
	    || (i >= LDL && i <= IMMP)          // LIP, ADDL and POPA take none
	    || i == STL;
}
char *c4m_builtins;
void c4m_setup_builtins () {
	c4m_builtins =
		"static extern __attribute__ constructor destructor "    // Ignored by c4m
		"char else enum if int return sizeof while "             // Keywords
		"switch case default break "                             // ... with a jumptable switch
		"open read close printf malloc free memset memcmp exit " // Syscalls
		"putchar puts realloc memcpy stacktrace "                // C4M extended opcodes...
		"install_trap_handler __opcode __builtin __c4_trap __c4_opcode "
		"__c4_jmp __c4_adjust __c4_configure __c4_cycles __time __c4_signal __c4_sigint "
		"__c4_usleep __c4_info __c4_ops_list "
		// C4 Invoke
		"__c4_invoke "
		// Future use: floating point support
		"__c4_float "
		"void main";                                             // void type and main entry
}
// Don't turn this function into a macro, it's used in __opcode_match
// using a complex expression (*op_a++), and we don't want that getting
// inlined into a macro.
char __toupper (char ch) {
    if (ch >= 'a' && ch <= 'z')
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
    while(r < INS_SIZE) {
        if (__opcode_match(name, ops))
            return r;
        ++r;
        ops = ops + 5; // size of each member in the string
    }
	printf("c4m: opcode not found: '%s'\n", name);
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
  int *swv, *swa, *tbl, *b3, *b4, *bend, *bdef;
  int  swn, swdef, swmin, swmax, swrange, i2, v2, neg2, brk_base;

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
    brk_base = brk_top; ++brk_depth;
    stmt();
    *++e = JMP; *++e = (int)a;
    *b = (int)(e + 1);
    --brk_depth;
    while (brk_top > brk_base) { --brk_top; *(int *)brk_slots[brk_top] = (int)(e + 1); }
  }
  else if (tk == Break) {
    next();
    if (tk != ';') { printf("%d: semicolon expected after 'break'\n", line); exit(-1); }
    if (!brk_depth) { printf("%d: 'break' outside of loop or switch\n", line); exit(-1); }
    if (brk_top >= BRK_MAX) { printf("%d: too many pending breaks\n", line); exit(-1); }
    *++e = JMP; b = ++e;
    brk_slots[brk_top] = (int)b;
    ++brk_top;
  }
  else if (tk == Switch) {
    // switch (expr) { case C: ... default: ... }, with fallthrough and
    // break, compiled to an in-memory jumptable of absolute addresses,
    // dispatched via JMPA. Layout matches c4cc's (see c4cc.c), minus
    // relocation: the table is plain malloc'd memory.
    next();
    if (tk == '(') next(); else { printf("%d: in 'switch': open paren expected\n", line); exit(-1); }
    expr(Assign);
    if (tk == ')') next(); else { printf("%d: in 'switch': close paren expected\n", line); exit(-1); }
    if (!(swv = malloc(SWITCH_MAX_CASES * sizeof(int))) ||
        !(swa = malloc(SWITCH_MAX_CASES * sizeof(int)))) { printf("%d: switch: out of memory\n", line); exit(-1); }
    swn = 0; swdef = 0; bdef = 0;
    *++e = JMP; b = ++e;                    // entry -> dispatch
    brk_base = brk_top; ++brk_depth;
    if (tk == '{') next(); else { printf("%d: in 'switch': open brace expected\n", line); exit(-1); }
    while (tk != '}') {
      if (tk == Case) {
        next();
        neg2 = 0;
        if (tk == Sub) { next(); neg2 = 1; }
        if (tk == Num) v2 = ival;
        else if (tk == Id && id[Class] == Num) v2 = id[Val];
        else { printf("%d: 'case' needs an integer constant\n", line); exit(-1); }
        if (neg2) v2 = -v2;
        next();
        if (tk == ':') next(); else { printf("%d: colon expected after 'case'\n", line); exit(-1); }
        i2 = 0;
        while (i2 < swn) {
          if (swv[i2] == v2) { printf("%d: duplicate case value %d\n", line, v2); exit(-1); }
          ++i2;
        }
        if (swn >= SWITCH_MAX_CASES) { printf("%d: too many cases\n", line); exit(-1); }
        swv[swn] = v2;
        swa[swn] = (int)(e + 1);
        ++swn;
      }
      else if (tk == Default) {
        next();
        if (tk == ':') next(); else { printf("%d: colon expected after 'default'\n", line); exit(-1); }
        if (swdef) { printf("%d: duplicate 'default'\n", line); exit(-1); }
        swdef = 1;
        bdef = e + 1;
      }
      else stmt();
    }
    next();
    *++e = JMP; bend = ++e;                 // running off the body -> end
    *b = (int)(e + 1);                      // dispatch:
    if (!swn) {
      if (swdef) { *++e = JMP; *++e = (int)bdef; }
    } else {
      swmin = swmax = swv[0];
      i2 = 1;
      while (i2 < swn) {
        if (swv[i2] < swmin) swmin = swv[i2];
        if (swv[i2] > swmax) swmax = swv[i2];
        ++i2;
      }
      swrange = swmax - swmin;
      if (swrange >= SWITCH_MAX_RANGE) { printf("%d: switch range %d too sparse for a jump table\n", line, swrange); exit(-1); }
      if (!(tbl = malloc((swrange + 1) * sizeof(int)))) { printf("%d: switch: out of memory\n", line); exit(-1); }
      // bounds check, three copies of the index on the stack
      if (swmin) { *++e = PSH; *++e = IMM; *++e = swmin; *++e = SUB; }
      *++e = PSH; *++e = PSH; *++e = PSH;
      *++e = IMM; *++e = swrange; *++e = GT; *++e = BNZ; b3 = ++e;
      *++e = IMM; *++e = 0; *++e = LT; *++e = BNZ; b4 = ++e;
      *++e = IMM; *++e = sizeof(int); *++e = MUL;
      *++e = PSH; *++e = IMM; *++e = (int)tbl; *++e = ADD; *++e = LI; *++e = JMPA;
      // out of range: drop the leftover index copies, then default or end
      *b3 = (int)(e + 1);
      *++e = ADJ; *++e = 2;
      if (swdef) { *++e = JMP; *++e = (int)bdef; }
      else { *++e = JMP; b = ++e; brk_slots[brk_top] = (int)b; ++brk_top; }
      *b4 = (int)(e + 1);
      *++e = ADJ; *++e = 1;
      if (swdef) { *++e = JMP; *++e = (int)bdef; }
      else { *++e = JMP; b = ++e; brk_slots[brk_top] = (int)b; ++brk_top; }
      // fill the table: cases where present, else default, else end
      // (nothing else is emitted below, so end == e + 1)
      i2 = 0;
      while (i2 <= swrange) {
        neg2 = 0;
        v2 = 0;
        while (v2 < swn) {
          if (swv[v2] == swmin + i2) { tbl[i2] = swa[v2]; neg2 = 1; v2 = swn; }
          ++v2;
        }
        if (!neg2) tbl[i2] = swdef ? (int)bdef : (int)(e + 1);
        ++i2;
      }
    }
    // end: resolve the fallthrough jump and every pending break
    *bend = (int)(e + 1);
    --brk_depth;
    while (brk_top > brk_base) { --brk_top; *(int *)brk_slots[brk_top] = (int)(e + 1); }
    free(swv); free(swa);
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

// This section is only compiled when run under C4 directly.
// Here we put stubs or alternate implementations for functions that must be emulated under C4.
// Overridable, so a build can ASK for the C4 implementations rather
// than only ever getting them by being compiled with no preprocessor.
#ifndef C4_ONLY
#define C4_ONLY 0
#endif
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
// Implement C4 version of memcpy
void *c4_memcpy(void *dst, void *src, int len) {
  int *di, *si, i, max;
  char *dc, *sc;

  //printf("c4_memcpy(%p, %p, %d)\n", dst, src, len);
  //stacktrace();
  // The loops below were `while (i++ < max) di[i] = si[i];`, which is
  // off by one at BOTH ends: the post-increment means the body first
  // runs with i == 1, so element 0 is never copied, and the last
  // iteration touches element max -- one past the end of both
  // buffers. Element 0 staying whatever the destination already held
  // is the subtler half: a freshly malloc'd block reads as zero, so a
  // copied .c4r image lost the first word of its code segment, and
  // opcode 0 is LEA. An image whose entry is at offset 0 -- which is
  // most of them -- had the ENT of its entry point silently replaced,
  // so calling it skipped the ENT, the callee ran on its CALLER's
  // frame, and its LEV returned through the caller's saved bp and pc.
  // The read and write past the end is the louder half, and showed up
  // as glibc aborting on a corrupted heap top chunk.
  //
  // Only reachable when c4m is interpreted by plain c4: compiled
  // natively, c4_memcpy is #defined to the host memcpy below.
  i = 0;
  if ((int)dst % sizeof(int) == 0 &&
      (int)src % sizeof(int) == 0 &&
      len % sizeof(int) == 0) {
    // Word copy
    di = (int*)dst; si = (int*)src;
    max = len / sizeof(int);
    while (i < max) { di[i] = si[i]; ++i; }
  } else {
    // Byte copy
    dc = (char*)dst; sc = (char*)src;
    while (i < len) { dc[i] = sc[i]; ++i; }
  }
  return dst;
}
// allocated by main
char *c4_time_buf;
int   c4_time_unavailable;
int   c4_time_approx, c4_time_last;
enum {
	// Buffer size for reading timefile
	C4_TIME_BUF_SZ = 32,
	// How often to read the timefile
	C4_TIME_APPROX = 1
};
// Attempt to read from a time source.
// This is hacky, it currently reads the file /proc/uptime for
// a time reference.
// This is meant to be fast, but it is not very pretty
// TODO: some systems have different formats for uptime.
//       On Ubuntu-x64, the first number only updates every second, and the
//       second number updates every 100ms.
//       This is reverse on Debian-pi.
//       Use -a flag to use alternate format.
// The host clock for a c4m with no host: /proc/uptime, which exists on
// Linux and on no breadboard. c4_time() below is what callers use; this
// is only its last resort.
int c4_time_host () {
	int fd, number, r;
	char *buf, ch;

	// Don't complain endlessly
	if (c4_time_unavailable)
		return 0;

	if ((fd = open("/proc/uptime", 0)) < 0) {
		printf("c4m: unable to open uptime file\n");
		c4_time_unavailable = 1;
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
				return c4_time_last = number * 1000;
			else
				return c4_time_last = number * 100;
		}
		number = (number * 10) + (ch - '0');
		++buf;
	}
}
// Plain C4 (not compiled natively)
int c4_plain () { return 1; }
int c4_info  () {
	// No HRT, SIG, or floating point support.
	// PROT only given if -U not specified.
	return C4I_C4 | C4I_C4M | (pm_avail ? C4I_PROT : 0);
}
int has_float () { return 0; }
int do_float  (int *sp) { return 0; }
int do_putchar(char *c) { return printf("%c", c); }
int do_puts   (char *str) { return printf("%s", str); }
#else
// When compiled natively, use whatever native functions we can.
// TODO: native c4_malloc, etc disabled due to bad implementation.
//#define c4_malloc(s)       malloc(s)
//#define c4_free(p)         free(p)
//#define c4_realloc(p,ns)   realloc(p, ns)
#define c4_memcpy(d,s,n)   memcpy(d, s, n)
#define c4_time_host()     c4m_time()
#define c4_plain()         0 /* Plain C4 or compiled c4m natively? */
#define c4_info()          (C4I_C4M | C4I_HRT | C4I_SIG | C4I_FLT | (pm_avail ? C4I_PROT : 0))
#define c4_usleep(usec)    usleep(usec)
#define has_float()        1
#define do_float(sp)       c4_float_instruction(sp)
#define do_putchar(c)      putchar(c)
#define do_puts(s)         puts(s)
#endif

// The clock, for every build there is.
//
// This sits OUTSIDE the C4_ONLY split on purpose, and the purpose is a
// bug that cost a session. The DOS check used to live INSIDE the
// C4_ONLY arm, which meant it existed only in builds where C4_ONLY was
// true -- and C4_ONLY is only ever true by accident, when a build goes
// through raw c4cc and every '#' line is skipped. Every build that has
// a preprocessor evaluated `#define C4_ONLY 0` honestly, took the other
// arm, and quietly had no DOS clock. Whether c4m can ask DOS the time
// has nothing to do with whether it was compiled against a host libc;
// nesting the one inside the other is what made "which compiler built
// this" decide a feature.
//
// Additive by construction, so it survives a compiler with no
// preprocessor at all: raw c4cc skips the '#' lines and compiles the
// DOS check in, where it costs one runtime test on a machine that has
// no DOS to answer it. That is the rule this file was written to and
// had drifted from -- an '#ifdef' may only ADD something harmless,
// never pick between two spellings of the same thing.
int c4_time () {
#if C4M_DOS
	// ASK C4DOS FIRST. c4_time_host() reads /proc/uptime, which is a
	// Linux fallback and exists on no breadboard -- so on c4bb this
	// latched c4_time_unavailable and returned 0 FOREVER. A guest that
	// waits for the clock to move then waits forever: the C4KE boot's
	// "measuring instructions per second" step never finished, at any
	// cycle budget, which looked like slowness and was actually a
	// stopped clock.
	//
	// DOS has the clock (TIME opcode, serviced by the board's microcode
	// or by a native c4m) and lends it through API slot 15. Going that
	// way rather than calling TIME here is deliberate: TIME is opcode
	// 53, above EXIT, and c4m-dos32.c4r is pinned in RUNG_BASE so the
	// campaign can say the VM needs no opcode the machine did not boot
	// with. The API table costs nothing above EXIT.
	if (dos_can_time()) return dos_time();
#endif
	return c4_time_host();
}

// Guest memory: malloc(), free() and realloc() for the MALC, FREE and RALC
// opcodes.
//
// The VM has no way to ask the host how large an allocation was, which is
// why the C4 versions of malloc()/realloc() were removed in 2024 (see the
// note at the top of this file) and why RALC has been dead ever since --
// its case was commented out, so guest code compiled fine and then fell
// through to the unknown-instruction path, leaving the SIZE argument in
// the accumulator for the guest to use as a pointer. That is a segfault,
// not a diagnostic.
//
// The obvious fix is a size header: over-allocate, stash the length in
// front, hand back the word after it. DO NOT DO THAT. Guest free() is
// reached by pointers that guest malloc() never produced -- measured, on
// ./c4 c4m.c load-c4r.c -- prog.c4r, where the loader issues 8 MALCs and
// 16 FREEs while the same run compiled natively is a balanced 17/17. A
// header makes free() do free(q - 1), and on those pointers that is heap
// corruption. (Why the counts differ between hosts is a separate open
// question, recorded in docs/compiler-speed.md; it is not this code's to
// answer, and this design does not care either way.)
//
// So sizes live in a side table keyed by pointer, and guest pointers are
// handed out and taken back EXACTLY as before. free() of something we
// never allocated falls through to a plain free(), which is precisely
// what it did before this change -- the compatibility risk is zero by
// construction rather than by audit. Only realloc() needs the table, and
// realloc() is only ever legal on your own allocation.
//
// Open addressing, linear probing, power-of-two capacity, tombstones on
// delete. Growth rehashes into a fresh pair of arrays, so it needs only
// malloc/free/memset -- all of which plain c4 has. That matters: ONE
// implementation serves both hosts and they cannot drift apart.
enum { C4_MT_EMPTY = 0, C4_MT_DEAD = 1, C4_MT_MIN = 4096 };

int *c4_mt_key;   // pointer keys; 0 = never used, 1 = tombstone
int *c4_mt_val;   // sizes, parallel to c4_mt_key
int  c4_mt_cap;   // capacity, always a power of two
int  c4_mt_fill;  // live entries + tombstones (what probing must bound)
int  c4_mt_live;  // live entries only

// Heap pointers are at least word aligned, so the low bits carry no
// information; fold a high slice down before masking or every allocation
// in one arena lands in the same cluster.
int c4_mt_slot (int p) {
  int h;

  h = (p >> 4) ^ (p >> 20);
  if (h < 0) h = 0 - h;
  return h & (c4_mt_cap - 1);
}

void c4_mt_setup (int cap) {
  c4_mt_cap  = cap;
  c4_mt_key  = malloc(cap * sizeof(int));
  c4_mt_val  = malloc(cap * sizeof(int));
  if (!c4_mt_key || !c4_mt_val) { c4_mt_cap = 0; return; }
  memset((char *)c4_mt_key, 0, cap * sizeof(int));
  memset((char *)c4_mt_val, 0, cap * sizeof(int));
  c4_mt_fill = 0;
  c4_mt_live = 0;
}

// Insert without growing or checking for duplicates. Used by both the
// public insert and by rehashing, which knows its keys are distinct.
void c4_mt_put_raw (int p, int n) {
  int i;

  i = c4_mt_slot(p);
  while (c4_mt_key[i] != C4_MT_EMPTY && c4_mt_key[i] != C4_MT_DEAD) {
    if (c4_mt_key[i] == p) { c4_mt_val[i] = n; return; }
    i = (i + 1) & (c4_mt_cap - 1);
  }
  if (c4_mt_key[i] == C4_MT_EMPTY) ++c4_mt_fill;
  c4_mt_key[i] = p;
  c4_mt_val[i] = n;
  ++c4_mt_live;
}

// Rehash into a table sized for the LIVE entries, which also sweeps the
// tombstones away. Called when probing distance would otherwise grow.
void c4_mt_grow () {
  int *oldk;
  int *oldv;
  int  oldcap, i, want;

  oldk = c4_mt_key; oldv = c4_mt_val; oldcap = c4_mt_cap;
  want = C4_MT_MIN;
  while (want < (c4_mt_live + c4_mt_live + c4_mt_live)) want = want + want;
  c4_mt_setup(want);
  if (!c4_mt_cap) { c4_mt_key = oldk; c4_mt_val = oldv; c4_mt_cap = oldcap; return; }
  i = 0;
  while (i < oldcap) {
    if (oldk[i] != C4_MT_EMPTY && oldk[i] != C4_MT_DEAD)
      c4_mt_put_raw(oldk[i], oldv[i]);
    ++i;
  }
  free(oldk);
  free(oldv);
}

void c4_mt_put (int p, int n) {
  if (!c4_mt_cap) c4_mt_setup(C4_MT_MIN);
  if (!c4_mt_cap) return;                       // out of memory: sizes are
                                                // lost, realloc will refuse
  // Keep the table at most half full counting tombstones, so a probe
  // always terminates quickly.
  if ((c4_mt_fill + c4_mt_fill) >= c4_mt_cap) c4_mt_grow();
  c4_mt_put_raw(p, n);
}

// Returns the recorded size, or -1 if we did not allocate this pointer.
int c4_mt_get (int p) {
  int i, n;

  if (!c4_mt_cap) return -1;
  i = c4_mt_slot(p);
  n = 0;
  while (c4_mt_key[i] != C4_MT_EMPTY && n < c4_mt_cap) {
    if (c4_mt_key[i] == p) return c4_mt_val[i];
    i = (i + 1) & (c4_mt_cap - 1);
    ++n;
  }
  return -1;
}

// Drop a pointer, leaving a tombstone so probe chains through it survive.
void c4_mt_del (int p) {
  int i, n;

  if (!c4_mt_cap) return;
  i = c4_mt_slot(p);
  n = 0;
  while (c4_mt_key[i] != C4_MT_EMPTY && n < c4_mt_cap) {
    if (c4_mt_key[i] == p) {
      c4_mt_key[i] = C4_MT_DEAD;
      c4_mt_val[i] = 0;
      --c4_mt_live;
      return;
    }
    i = (i + 1) & (c4_mt_cap - 1);
    ++n;
  }
}

int c4_malloc (int n) {
  int p;

  if (n < 0) return 0;
  if (!(p = (int)malloc(n))) return 0;
  c4_mt_put(p, n);
  return p;
}

void c4_free (int p) {
  if (!p) return;
  c4_mt_del(p);          // a no-op for pointers that were never ours
  free((void *)p);
}

int c4_realloc (int p, int n) {
  int r, old;

  // realloc(0, n) is malloc(n); realloc(p, 0) frees and yields null,
  // which is what glibc does and therefore what the gcc oracle in
  // src/tests/test_realloc.c pins.
  if (!p) return c4_malloc(n);
  if (n <= 0) { c4_free(p); return 0; }
  old = c4_mt_get(p);
  // A pointer we never handed out has no recorded length, so there is no
  // safe number of bytes to copy. Refusing is the only honest answer.
  if (old < 0) return 0;
  if (!(r = c4_malloc(n))) return 0;
  if (old > n) old = n;
  c4_memcpy((char *)r, (char *)p, old);
  c4_free(p);
  return r;
}

int  tlev_instruction;

// When set, TLEV restores the cycle interrupt interval that was in
// effect when the trap was taken (see the bp+9 slot below), the way a
// real machine's interrupt return restores the interrupt flag.
//
// It is off by default because it changes what a handler's own
// __c4_configure(CONF_CYCLE_INTERRUPT_INTERVAL, x) means: with this
// on, the frame decides, not the last write before returning. C4KE
// re-arms from inside its handler and predates all of this, so it
// keeps the old behaviour untouched; C4IX opts in.
int  trap_restores_interval;

// Cause a trap to occur and update stack and registers so that the given
// handler is executed.
// This is intended to be used by illegal opcode traps, irq handlers, and
// signal events.
// The arguments provided to the trap handler are also the temporary storage
// for sp, bp, a, mode, and pc registers.  The TLEV instruction undoes all this,
// restoring registers from these stack values.
// The stack is setup by writing a pointer to a TLEV instruction,
// followed by the our trap handler's bp. This allows a LEV to point
// to a TLEV instruction for proper trap exit.
// The stack is further adjusted by inspecting the ENT x instruction,
// and adding that to sp. This allows local variables to work.
// The frame is presented as such (offsets in words from the handler's bp):
//   bp+0 = trap bp      Usually an sp and return pc are on the stack.
//   bp+1 = ptr to TLEV  Argument references (LEA) still expect these here.
//   bp+2 = return pc / instruction address
//   bp+3 = saved sp
//   bp+4 = saved bp
//   bp+5 = saved accumulator
//   bp+6 = saved mode
//   bp+7 = instruction
//   bp+8 = trap
//   bp+9 = saved cycle interrupt interval
// A C4 handler's parameters are numbered from the top of the frame
// down, so a handler declared with the usual seven parameters reads
// bp+8..bp+2 as (trap, instruction, mode, a, bp, sp, returnpc). The
// interval at bp+9 is pushed FIRST for exactly that reason: it is
// invisible to those handlers, and a handler that wants it declares
// an eighth parameter ahead of the other seven. That is what makes
// this slot free to add without disturbing C4KE.
// After inspecting the trap handler's ENT x instruction, the stack is
// further adjusted to account for it.
void trap (int type, int parameter, int *handler, int **_sp, int **_bp, int **_pc, int a, int mode) {
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
		else if (type == TRAP_PM_VIOLATION) printf("TRAP_PM_VIOLATION");
		else printf("(unknown %d)", type);
		printf(" start, offending instruction %d at 0x%X, sp=0x%X, bp=0x%X, handler=0x%X\n", *(pc - 1), pc - 1, *_sp, *_bp, handler);
	}
	// Sometimes trap() was being called with no handler, causing a segfault in this trap function.
	if (!handler) {
		printf("c4m: missed a trap, no handler installed\n");
		return;
	}

	// Push the details we'll use in TLEV
	t = sp;  // save old stack

	// Offset so that stack doesn't get overwritten
	sp = sp - TRAP_OFFSET;

	// The interrupt state belongs to the interrupted context, so it is
	// saved with it. Pushed first, so it lands at bp+9, past the seven
	// parameters an existing handler can see.
	*--sp = cycle_interrupt_interval;
	// Handlers run with the cycle interrupt off. Every caller of trap()
	// already did this on the way back; doing it here as well means the
	// mask covers the handler's own epilogue and its TLEV, which is the
	// window a handler cannot close from the inside.
	cycle_interrupt_interval = 0;

	// Push instruction and trap number
	*--sp = type;             // printf("*sp(0x%X) = trap type %d\n", sp, *sp);
	*--sp = parameter;        // printf("*sp(0x%X) = parameter %d (at 0x%X)\n", sp, *sp, pc - 1);
	// Push the registers. These can be updated, they are restored by TLEV.
	*--sp = (int)mode;        // printf("*sp(0x%X) = mode %d\n", sp, mode);
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
    // Disable cycle interrupt
    // cycle_interrupt_interval = 0;
}

///
// C4 Jailbreak
// Enables code to be called that runs in C4
///

enum { MAX_SEARCH = 512 };

// This function uses local references to find the return PC on the stack.
// It then searches up the code stack to find the ENT opcode.
int *get_calling_address () {
#if NOT_NATIVE
	int *addr, *next, i;

	// Return pc is stored above local variables
	addr = (int *)(*(&addr + 2));

	// Find ENT x
	i = 0;
	next = addr;
	while (++i <= MAX_SEARCH) {
		--next;
		if (*addr == ENT) { // Possibly found
			// Ensure it wasn't an argument to some other opcode
			if (*next > ADJ)
				return addr;
		}
		addr = next;
	}

	printf("c4m/get_calling_address: couldn't find entry\n");
#endif
	return 0;
}

// Location of where to change JMP target for c4_invoke_stub.
int *c4_invoke_stub_addr;
// This function overwrites itself with a JMP. Before that it has to grab its own address
// so that it can do so.
int c4_invoke_stub () {
#if NOT_NATIVE
	// Setup
	if (!(c4_invoke_stub_addr = get_calling_address())) {
		printf("c4_invoke_stub: unable to get calling address!\n");
		exit(999);
	}

	*c4_invoke_stub_addr = JMP; // Set JMP opcode
	++c4_invoke_stub_addr;      // Update address
#endif

	// Return for now
	return 0;
}

///
// The VM's file syscalls, and the one place they are not just the host's.
//
// A program running INSIDE c4m reaches the outside world through these
// opcodes, so whatever they can see, everything c4m interprets can see.
// Under C4DOS that matters: the RAM disk lives in DOS's own memory and
// a transient's open() has never heard of it, so `c4m load-c4r.c -- c4ke`
// could not find a kernel the machine had just compiled -- load-c4r.c
// was doing the open, one level down, and no amount of patching it
// would help because it is compiled from source at runtime and has no
// symbol table to inject into. Routing the VM's own OPEN/READ/CLOS
// through the DOS API fixes it for load-c4r.c and for every other
// program c4m will ever run, without any of them knowing.
//
// DOS checks the RAM disk BEFORE the real disk, so host files still
// resolve; when there is no DOS, __c4dos_api is 0, dos_readable() is
// false, and these are exactly the host calls they always were.
//
// C4M_DOS is defined only for the C4DOS-rung builds, which are the
// ones compiled by raw c4cc -- it skips # lines, so the branch is
// always present there and include/c4dos.h is prepended to supply it.
// Every other build goes through gcc or gcc -E, where C4M_DOS is 0 and
// the branch disappears: native ./c4m and the C4KE c4m.c4r are byte
// for byte the programs they were.
///

int c4m_open (char *path, int flags) {
#if C4M_DOS
	if (dos_readable()) return dos_fopen(path);
#endif
	return open(path, flags);
}

int c4m_read (int fd, char *buf, int len) {
#if C4M_DOS
	if (dos_readable()) return dos_fread(fd, buf, len);
#endif
	return read(fd, buf, len);
}

int c4m_close (int fd) {
#if C4M_DOS
	if (dos_readable()) return dos_fclose(fd);
#endif
	return close(fd);
}

///
// Callable main entry point, TODO: c4cc (should) includes this file for the opcode definitions.
///

int c4m_main(int argc, char **argv)
{
  int fd, bt, ty, poolsz, printsyms;
  int *pc, *sp, *bp, a; // vm registers
  int i, *t, r; // temps
  char*_p, *_data;       // initial pointer locations
  int *_sym, *_e, *_sp;  // initial pointer locations
  int  verb;
  int cycle, run;
  int *trap_handler, mode, padding;
  int status, *idmain, *idmax;
  int len;
  char *x;



  //debug = 1;
  debug = 0;
  verb = 0;
  trap_handler = (int *)0;
  a = 0;
  time_altmode = 0;
  // Used to protect traps from just returning instead of using TLEV.
  tlev_instruction = TLEV;
  // Off by default: kernels written before this existed re-arm the
  // cycle interrupt from inside their handler, and turning this on
  // would overrule them.
  trap_restores_interval = 0;
  // Start in unprotected mode with pm available
  mode = MODE_UNPROTECTED;
  pm_avail = 1;
  // Setup invoke stub
  c4_invoke_stub();

  //printf("(C4M) Argc: %lld\n", argc); i = 0; while(i < argc) { printf("(C4M) Argv[%lld] = %s\n", i, argv[i]); ++i; }

  poolsz = POOL_SZ;
  printsyms = 0;
  --argc; ++argv;
  if (argc > 0 && **argv == '-' && (*argv)[1] == 'v') { verb = 1; --argc; ++argv; }
  if (argc > 0 && **argv == '-' && (*argv)[1] == 's') { src = 1; --argc; ++argv; }
  if (argc > 0 && **argv == '-' && (*argv)[1] == 'd') { debug = 1; --argc; ++argv; }
  if (argc > 0 && **argv == '-' && (*argv)[1] == 'S') { printsyms = 1; --argc; ++argv; }
  if (argc > 0 && **argv == '-' && (*argv)[1] == 'a') { time_altmode = 1; --argc; ++argv; }
  if (argc > 0 && **argv == '-' && (*argv)[1] == 'U') { pm_avail = 0; --argc; ++argv; }
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
  if (!(brk_slots = malloc(BRK_MAX * sizeof(int)))) { printf("could not malloc break stack\n"); return -1; }
  brk_top = 0; brk_depth = 0;
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
  i = Static; while (i <= Break) { next(); id[Tk] = i++; } // add keywords to symbol table
  i = OPEN; while (i <= FLT) {
	next(); id[Class] = Sys; id[Type] = INT; id[Val] = i++;
	if (verb) { // don't be so verbose
		len = 0;
		x = (char *)id[Name];
		while (*x++ != ' ') ++len;
		printf("  builtin '%.*s' = %d\n", len, id[Name], id[Val]);
	}
  } // add library to symbol table
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
  if (r <= 0) { printf("could not read all source files: exceeded %d (0x%X) bytes\n", poolsz, poolsz); return -1; }
  // Reset pointer to start of code
  p = _p;

  // parse declarations
  if (verb) printf("c4m: compile...\n");
  line = 1;
  next();
  while (tk) {
    bt = INT; // basetype
    if (tk == Static) next(); // Ignore static keyword, used by c4cc
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
      if (tk == Attribute) { // Attributes, used by c4cc, ignored by c4m
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
      // Allow overwriting existing definitions. This allows for multiple main() definitions,
      // using the last one seen. c4cc has a similar section that allows any defined symbol
      // to be overwritten.
      //if (id[Class]) { printf("%d: duplicate global definition\n", line); return -1; }
      next();
      id[Type] = ty;
      if (tk == '(') { // function
        // Don't overwrite builtins
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
	// TLEV is a trap return: it restores five registers from a frame in
	// one step, and there is no way to express "half returned". Landing
	// an interrupt on it hands the handler a context that belongs to
	// neither the outgoing nor the incoming side. Kernels that opt into
	// trap_restores_interval never reach this instruction with the
	// interrupt armed; for the others, stepping over it costs one tick.
	if (cycle_interrupt_interval && !(cycle % cycle_interrupt_interval) && *pc != TLEV) {
		//printf("!trap_hard_irq %d using handler 0x%X\n", cycle_interrupt_interval, cycle_interrupt_handler);
		trap(TRAP_HARD_IRQ, HIRQ_CYCLE, cycle_interrupt_handler, &sp, &bp, &pc, a, mode);
		// Disable cycle interrupt and set unprotected mode
		cycle_interrupt_interval = 0;
		mode = MODE_UNPROTECTED;
	// Check for pending signals from the signal handler.
	//
	// A signal is an asynchronous interrupt like the cycle one, so it
	// honours the same mask -- but only for kernels that have told us
	// the interval IS a mask, by opting into trap_restores_interval.
	// Without that, a masked interval and "this kernel simply never
	// enabled preemption" are indistinguishable, and deferring would
	// mean never delivering.
	//
	// Deferred, not dropped: pending_signal stays set, so the signal
	// arrives as soon as the mask lifts. Delivering it regardless is
	// how Ctrl-C used to vanish -- the trap landed while the kernel
	// was inside its own handler, the handler's re-entry guard turned
	// delivery into a no-op, and the signal was gone.
	} else if (pending_signal
	           && !(trap_restores_interval && !cycle_interrupt_interval)) {
		// TODO: sometimes crashes here, as if signal_handlers[pending_signal] points to code that has been
		//       deallocated.
		// printf("c4m: trapping pending signal %d\n", pending_signal);
		trap(TRAP_SIGNAL, pending_signal, (int *)signal_handlers[pending_signal], &sp, &bp, &pc, a, mode);
		// Disable cycle interrupt and set unprotected mode
		cycle_interrupt_interval = 0;
		mode = MODE_UNPROTECTED;
		pending_signal = 0;
	}

    i = *pc++;
    // This opcode is handled here so debug output can show the opcode
    if (i == OPCD) {
        i = *sp;
        // The fused opcodes take an operand too, and it would be read
        // out of the CALLER's instruction stream -- so OPCD has to
        // refuse them for the same reason it refuses LEA..ADJ.
        if (c4m_has_operand(i)) {
            printf("%.4s does not support opcodes requiring arguments (%.4s given)\n",
                   &c4m_opcodes[OPCD * 5], &c4m_opcodes[i * 5]);
			// Raise an OPV trap
			trap(TRAP_OPV, i, trap_handler, &sp, &bp, &pc, a, mode);
			// Disable cycle interrupt and set unprotected mode
			cycle_interrupt_interval = 0;
			mode = MODE_UNPROTECTED;
			// change opcode to a cycle count request (harmless)
			i = C4CY;
        }
    }

    if (debug) {
      // The output here is split into multiple calls because C4's printf only pushes
      // up to 6 arguments.
      printf("0x%-*X %-*d ", padding, pc - 1, padding, cycle);
      printf("A=0x%-*X> ", padding, a);
      if (i >= 0 && i < INS_SIZE) {
          printf("%.4s", &c4m_opcodes[i * 5]);
      } else {
          printf("unknown %-*d (0x%X)", padding, i, i);
      }
      if (c4m_has_operand(i)) printf(" %d\n", *pc); else printf("\n");
    }

    if      (i == LEA) a = (int)(bp + *pc++);                             // load local address
    // The three fused opcodes c4m has, in the hot part of the chain
    // because that is the whole point of them.
    else if (i == LDL)  a = *(int *)(bp + *pc++);                         // load local
    else if (i == STL)  *(int *)(bp + *pc++) = a;                         // store local
    else if (i == POPA) a = *sp++;                                        // pop into the accumulator
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
	// SYSCALL functions
    else if (i == OPEN) {
		if (mode == MODE_UNPROTECTED)
            a = c4m_open((char *)sp[1], *sp);
		else {
			trap(TRAP_PM_VIOLATION, OPEN, trap_handler, &sp, &bp, &pc, a, mode);
			cycle_interrupt_interval = 0;
			mode = MODE_UNPROTECTED;
		}
	}
    else if (i == READ) {
		if (mode == MODE_UNPROTECTED)
        a = c4m_read(sp[2], (char *)sp[1], *sp);
		else {
			trap(TRAP_PM_VIOLATION, READ, trap_handler, &sp, &bp, &pc, a, mode);
			cycle_interrupt_interval = 0;
			mode = MODE_UNPROTECTED;
		}
	}
    else if (i == CLOS) {
		if (mode == MODE_UNPROTECTED)
            a = c4m_close(*sp);
		else {
			trap(TRAP_PM_VIOLATION, CLOS, trap_handler, &sp, &bp, &pc, a, mode);
			cycle_interrupt_interval = 0;
			mode = MODE_UNPROTECTED;
		}
	}
	else if (i == PUTC) {
		if (mode == MODE_UNPROTECTED)
            a = do_putchar(*((char *)sp));
		else {
			trap(TRAP_PM_VIOLATION, PUTC, trap_handler, &sp, &bp, &pc, a, mode);
			cycle_interrupt_interval = 0;
			mode = MODE_UNPROTECTED;
		}
	}
	else if (i == PUTS) {
		if (mode == MODE_UNPROTECTED)
            a = do_puts((char *)*sp);
		else {
			trap(TRAP_PM_VIOLATION, PUTS, trap_handler, &sp, &bp, &pc, a, mode);
			cycle_interrupt_interval = 0;
			mode = MODE_UNPROTECTED;
		}
	}
    else if (i == PRTF) {
		if (mode == MODE_UNPROTECTED) {
			r = pc[1];
			t = sp + r;
			// Fix potential access violation by not pushing arguments not given
			//if (r == 1) a = printf((char*)t[-1]);
			//else if (r == 2) a = printf((char*)t[-1], t[-2]);
			//else if (r == 3) a = printf((char*)t[-1], t[-2], t[-3]);
			//else if (r == 4) a = printf((char*)t[-1], t[-2], t[-3], t[-4]);
			//else if (r == 5) a = printf((char*)t[-1], t[-2], t[-3], t[-4], t[-5]);
			//else if (r == 6) a = printf((char*)t[-1], t[-2], t[-3], t[-4], t[-5], t[-6]);
			//else if (r == 7) a = printf((char*)t[-1], t[-2], t[-3], t[-4], t[-5], t[-6], t[-7]);
			if (r > 7) { printf("Too many arguments to printf!\n"); exit(-1); }
			else a = printf((char*)t[-1], t[-2], t[-3], t[-4], t[-5], t[-6], t[-7]);
		} else {
			trap(TRAP_PM_VIOLATION, PRTF, trap_handler, &sp, &bp, &pc, a, mode);
			cycle_interrupt_interval = 0;
			mode = MODE_UNPROTECTED;
		}
    }
    else if (i == MALC) {
		if (mode == MODE_UNPROTECTED)
            a = c4_malloc(*sp);
		else {
			trap(TRAP_PM_VIOLATION, MALC, trap_handler, &sp, &bp, &pc, a, mode);
			cycle_interrupt_interval = 0;
			mode = MODE_UNPROTECTED;
		}
	}
    else if (i == RALC) {
		if (mode == MODE_UNPROTECTED)
            a = c4_realloc(sp[1], *sp);
		else {
			trap(TRAP_PM_VIOLATION, RALC, trap_handler, &sp, &bp, &pc, a, mode);
			cycle_interrupt_interval = 0;
			mode = MODE_UNPROTECTED;
		}
	}
    else if (i == FREE) {
		if (mode == MODE_UNPROTECTED)
            c4_free(*sp);
		else {
			trap(TRAP_PM_VIOLATION, FREE, trap_handler, &sp, &bp, &pc, a, mode);
			cycle_interrupt_interval = 0;
			mode = MODE_UNPROTECTED;
		}
	}
    else if (i == MSET) a = (int)memset((char *)sp[2], sp[1], *sp);
    else if (i == MCMP) a = memcmp((char *)sp[2], (char *)sp[1], *sp);
    else if (i == MCPY) a = (int)c4_memcpy((void*)sp[2], (void*)sp[1], *sp);
    else if (i == STRC) {
//		if (mode == MODE_UNPROTECTED) {
			print_stacktrace(pc, idmain, idmax, bp, sp);
//		} else {
//			trap(TRAP_PM_VIOLATION, STRC, trap_handler, &sp, &bp, &pc, a, mode);
//			// Disable cycle interrupt and set unprotected mode
//			cycle_interrupt_interval = 0;
//			mode = MODE_UNPROTECTED;
//		}
	}
    else if (i == EXIT) {
		// Guarded like the other syscalls: a protected task calling
		// exit() must not be able to halt the whole VM, so it traps
		// and the kernel decides what "exit" means for that task.
		// Unprotected code (including every kernel) is unaffected.
		if (mode == MODE_UNPROTECTED) {
			//printf("exit(%d) cycle = %d\n", *sp, cycle);
			status = *sp; run = 0;
		} else {
			trap(TRAP_PM_VIOLATION, EXIT, trap_handler, &sp, &bp, &pc, a, mode);
			// Disable cycle interrupt and set unprotected mode
			cycle_interrupt_interval = 0;
			mode = MODE_UNPROTECTED;
		}
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
		mode = (int)*t++;    // printf("From 0x%X, loaded saved mode %d\n", t - 1, mode);
        //printf("Resume from pc 0x%X\n", pc);
        // The interrupt state is part of the context too. t has walked
        // past the instruction and trap number, so it now addresses the
        // interval saved at bp+9.
        if (trap_restores_interval) cycle_interrupt_interval = *(t + 2);
	}
	else if (i == C4CY) a = cycle;
	else if (i == SIGI) a = __c4_sigint();
    else if (i == TIME) a = c4_time();
	else if (i == USLP) a = c4_usleep(*sp);
    else if (i == ITH) { // install trap handler
//		if (mode == MODE_UNPROTECTED) {
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
//		} else {
//			trap(TRAP_PM_VIOLATION, ITH, trap_handler, &sp, &bp, &pc, a, mode);
//			// Disable cycle interrupt and set unprotected mode
//			cycle_interrupt_interval = 0;
//			mode = MODE_UNPROTECTED;
//		}
	} else if (i == C4CF) { // Configure system: __c4_configure(CONF_*, value)
		// TODO: this should be priveleged, but C4KE is using it in mode 1 (protected)
		//printf("(c4: C4CF %d in mode %d\n", sp[1], mode);
		//if (mode == MODE_UNPROTECTED) {
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
			} else if(sp[1] == CONF_TRAP_RESTORES_INTERVAL) {
				a = trap_restores_interval;
				trap_restores_interval = sp[0];
			} else {
				printf("c4m: C4CF issue\n");
				return -100;
			}
		//} else {
		//	printf("C4CF in mode %d??\n", mode);
		//	trap(TRAP_PM_VIOLATION, C4CF, trap_handler, &sp, &bp, &pc, a, mode);
		//	mode = MODE_UNPROTECTED;
		//}
	}
	else if (i == SIGH) a = (int)__c4_signal(sp[1], (int *)sp[0]);
	else if (i == INFO) {
		// Trap this under protected mode
		if (mode == MODE_UNPROTECTED)
            a = c4_info() | (trap_handler ? C4I_TRAPH : 0);
		else {
			trap(TRAP_PM_VIOLATION, INFO, trap_handler, &sp, &bp, &pc, a, mode);
			cycle_interrupt_interval = 0;
			mode = MODE_UNPROTECTED;
		}
	} else if (i == _TRP) {
      // __c4_trap(type, signal)
      // Trigger a trap
      trap(sp[1], sp[0], trap_handler, &sp, &bp, &pc, a, mode);
	  // Disable cycle interrupt and set unprotected mode
//	  cycle_interrupt_interval = 0;
//	  mode = MODE_UNPROTECTED;
	} else if (i == DBG) {
		// DBG - signal the debugger
		trap(TRAP_DEBUG, 0, trap_handler, &sp, &bp, &pc, a, mode);
		// Disable cycle interrupt and set unprotected mode
//		mode = MODE_UNPROTECTED;
	} else if (i == C4IV) {
		// C4 Invoke
		// TODO: only allow in unprotected mode?
		// Update stub function to JMP to given address in *sp
#if C4M_DOS
		// THE INVOKE MUST NOT ESCAPE THIS INTERPRETER WHEN WE ARE THE
		// ONE HOLDING THE FILESYSTEM.
		//
		// The stub below rewrites our OWN code to jump at the target,
		// so the invoked function stops being something we interpret
		// and becomes something the machine runs directly. Its
		// syscalls then go straight to the disk controller and never
		// reach the C4DOS RAM disk -- and load-c4r.c takes exactly
		// that route for every load it does (c4r_load_opt_pure, which
		// c4r_load picks whenever __c4_info() says C4I_C4). That is
		// why `c4m load-c4r.c -- c4ke` could not find a kernel BUILD
		// had just compiled: the open happened one level above the
		// only place that knows what a RAM disk is.
		//
		// A JSR is the same call without the escape -- push the return
		// address, jump, let the callee's LEV come back -- and it is
		// what the #else branch below has always had a TODO asking
		// for. Only taken when DOS is actually there, so a c4m running
		// on bare hardware still gets the stub it always did.
		if (dos_readable()) {
			t = (int *)*sp;
			*--sp = (int)pc;
			pc = t;
		} else {
#endif
#if NOT_NATIVE
		*c4_invoke_stub_addr = *sp;
		a = c4_invoke_stub();
#else
		// Under C4, don't run this code
		if (0) {
			// TODO: emulate it by invoking a JSR
			printf("c4m: invoke only supported under c4.\n");
			a = 0;
		}
#endif
#if C4M_DOS
		}
#endif
	} else if (i == FLT) {
		if (has_float()) a = do_float(sp);
		else {
			// Maybe the running code can emulate this instruction
			trap(TRAP_ILLOP, FLT, trap_handler, &sp, &bp, &pc, a, mode);
		}
    } else if (!trap_handler && i >= 0 && i < INS_SIZE) {
			// An opcode this machine KNOWS THE NAME OF but does not
			// implement -- c4mp's, or one of the seven fused opcodes
			// that live there. That is a machine mismatch, not a custom
			// opcode, so halt and say so.
			//
			// Trapping into nowhere and resuming is not an option here:
			// pc is past the opcode but NOT past its operand, so
			// carrying on executes a data word and walks off into
			// memory. That used to be a segfault with no explanation,
			// which is a poor way to find out an image was built for
			// c4mp.
			//
			// An opcode OUTSIDE the table keeps the old behaviour and
			// falls through below: that is the custom-opcode
			// convention, where a program raises an opcode a kernel
			// emulates, and running the same program with no kernel is
			// expected to print the missed trap and carry on (see
			// src/oisc4/test-oisc4.sh, which filters exactly that).
			printf("c4m: %.4s (opcode %d) at 0x%X is not an instruction this machine has,\n",
			       &c4m_opcodes[i * 5], i, pc - 1);
			printf("c4m: and no trap handler is installed to emulate it. Halting.\n");
			status = -1;
			run = 0;
    } else {
			// printf("c4m: illegal opcode %d, invoking trap handler 0x%X\n", i, trap_handler);
			trap(TRAP_ILLOP, i, trap_handler, &sp, &bp, &pc, a, mode);
			// A trap handler must run privileged, as it does at every
			// other trap site. Without this an illegal opcode raised BY
			// A PROTECTED TASK -- which is how custom-opcode syscalls
			// work -- runs the handler still protected, so the first
			// syscall opcode inside the kernel raises a SECOND trap on
			// top of the first. (C4KE is unaffected: it compiles its
			// protected mode out, so mode is already unprotected.)
			mode = MODE_UNPROTECTED;
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

#endif // ifndef __C4M_C__

#ifndef NO_C4M_MAIN
int main(int argc, char **argv) {
	return c4m_main(argc, argv);
}
#endif

