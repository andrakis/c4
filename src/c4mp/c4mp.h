#ifndef __C4MP_H
#define __C4MP_H 1

//
// c4mp -- "C4 MultiProcessor". Shared declarations.
//
// A C4 virtual machine with room for more than one processor. Unlike
// c4m it carries no compiler: it loads .c4r images, which c4lc
// produces, and interprets them. Dropping the compiler drops about
// half the file, and takes the plain-c4 fallback machinery with it --
// the C4_ONLY shims, the invoke-stub jailbreak, the /proc/uptime
// clock. None of that has a job here.
//
// c4mp is itself compiled by c4lc, so it may use structs, for, ->,
// compound assignment and a preprocessor. c4m may use none of those,
// because c4m has to be parsable by c4.c. That is worth more than
// comfort: a restrict-qualified pointer is the only way to tell a
// real C compiler that the machine registers are registers and that
// guest stores cannot touch them.
//
// Three configurations from one source:
//
//   ./c4mp prog.c4r                         native, one CPU
//   ./c4mp -cpus 4 prog.c4r                 native, N host threads   (pass 2)
//   ./c4m load-c4r.c -- c4mp.c4r prog.c4r   hosted by c4m, simulated
//
// The last is a backwards-compatibility proof rather than the point:
// c4m has no multiprocessing to offer, so SMP there is simulated by
// interleaving contexts at a quantum boundary.
//

#ifdef __c4cc__
// Compiled by c4lc (or c4cc). int is already the machine word, and
// every host call below is a single opcode, so there is nothing to
// include. Neither qualifier exists in this dialect -- which is
// precisely the limitation c4mp was written to escape natively.
#define RESTRICT
#define CONST
#else
#define C4M_SIGNALS 1
#include "c4.h"        // also does #define int __INTPTR_TYPE__
#include <c4m_util.h>  // c4m_time()
#include "c4m_float.h" // c4_float_instruction()
#define RESTRICT restrict
#define CONST    const
#endif

// How many instructions a CPU gets before the next one runs, when
// there is more than one. Small enough that interleaving is fine
// grained, large enough that the register round trip stays noise: an
// order of magnitude below C4IX's 10000-cycle preemption interval.
enum { C4MP_QUANTUM = 1000 };

// Default per-CPU stack, in bytes. c4m allocates four pools this size
// (symbols, text, data, stack); c4mp needs only the stack, because the
// image brings its own code and data segments.
enum { C4MP_STACK_SZ = 262144 };

// Words of slack trap() leaves between the interrupted stack and the
// frame it builds, so a handler cannot scribble on the context it was
// called to save. c4m's TRAP_OFFSET, kept identical: guest kernels
// hand-build frames at this offset (C4IX's ck_signal_deliver does).
enum { C4MP_TRAP_OFFSET = 0x0F };

// ---- instruction set ----
//
// Numbering is c4m's, exactly, and must stay that way: it is repeated
// in c4m.c, load-c4r.c, oisc4.c, c4cc.c, c4r.lisp and five copies of
// the plain-c4 subset. Appending is free; inserting is not.
enum {
    LEA ,IMM ,JMP ,JSR ,BZ  ,BNZ ,ENT ,ADJ ,LEV ,LI  ,LC  ,SI  ,SC  ,PSH ,
    OR  ,XOR ,AND ,EQ  ,NE  ,LT  ,GT  ,LE  ,GE  ,SHL ,SHR ,ADD ,SUB ,MUL ,DIV ,MOD ,
    OPEN,READ,CLOS,PRTF,MALC,FREE,MSET,MCMP,EXIT,
    PUTC,PUTS,RALC,MCPY,STRC,
    ITH ,_OPC,_BLT,_TRP,OPCD,
    _JMP,_ADJ,C4CF,C4CY,TIME,
    SIGH,SIGI,USLP,INFO,OPSL,
    C4IV,
    FLT ,
    JSRI,JSRS,JMPA,TLEV,DBG ,
    // c4mp's own, starting at 66 -- the first numbers c4m does not
    // use. On c4m these arrive as TRAP_ILLOP, which is how a guest
    // kernel can emulate them; a guest should test C4I_SMP first
    // rather than find out by trapping.
    CPUI,CPUN,CPUS,CPUH,
    INS_SIZE
};

// Instruction privilege. Protected code traps on any opcode that
// reaches the host, so a kernel decides what it means.
enum { MODE_UNPROTECTED, MODE_PROTECTED };

// Trap codes, c4m's values.
enum {
    TRAP_ILLOP,          // unknown opcode: how custom opcodes are built
    TRAP_HARD_IRQ,       // raised by the machine
    TRAP_SOFT_IRQ,       // raised by guest code via __c4_trap
    TRAP_SIGNAL,         // a POSIX signal arrived
    TRAP_SEGV,
    TRAP_OPV,            // bad opcode value handed to OPCD
    TRAP_PM_VIOLATION,   // a guarded opcode in protected mode
    TRAP_DEBUG
};

// TRAP_HARD_IRQ sub-codes.
enum { HIRQ_CYCLE };

// __c4_configure options.
enum { CONF_CYCLE_INTERRUPT_INTERVAL, CONF_CYCLE_INTERRUPT_HANDLER, CONF_PRIVS,
       CONF_TRAP_RESTORES_INTERVAL };

// __c4_info() bits. c4m leaves 0x100 and 0x200 free; c4mp claims the
// first for SMP so a guest can ask before it dares execute an opcode
// c4m does not have.
enum {
    C4I_NONE  = 0x0,
    C4I_C4    = 0x1,
    C4I_C4M   = 0x2,
    C4I_C4P   = 0x4,
    C4I_C4MJS = 0x8,
    C4I_HRT   = 0x10,
    C4I_SIG   = 0x20,
    C4I_FLT   = 0x40,
    C4I_PROT  = 0x80,
    // c4mp's. SMP advertises the CAPABILITY, not the processor count:
    // a guest tests this bit, then calls __c4_cpu_count(). Without
    // that order an unguarded CPUN is an illegal opcode on c4m.
    C4I_SMP   = 0x100,
    C4I_CISC  = 0x200,   // reserved for the superinstruction milestone
    C4I_TRAPH = 0x400
};

// ---- one processor ----
//
// Everything the machine must remember to stop a CPU mid-instruction
// stream and resume it later. In c4m these are nine locals of
// c4m_main plus four file-scope globals; gathering them here is what
// makes a second processor expressible at all.
//
// The interrupt state is per-CPU for the same reason the registers
// are: an SMP kernel masks preemption on the CPU it is running on,
// not on the machine.
struct c4_cpu {
    int  id;                // index into the CPU table; CPUI returns it
    int *pc;
    int *sp;
    int *bp;
    int  a;
    int  mode;              // MODE_*
    int  cycle;             // instructions retired on this CPU
    int  state;             // CPU_*
    int  status;            // exit status once halted
    int *traph;             // trap handler, or 0
    int *ihand;             // cycle interrupt handler
    int  ival;              // cycle interrupt interval; 0 also means masked
    int  tri;               // CONF_TRAP_RESTORES_INTERVAL
    int *stkbase;           // stack allocation to free, 0 if borrowed
};

// CPU states. WAIT is unreachable until CWAI exists; the scheduler
// already distinguishes it from HALT, because "nothing runnable" means
// a finished machine in one case and a deadlocked one in the other.
enum { CPU_OFF, CPU_RUN, CPU_WAIT, CPU_HALT };

// Why c4_run returned.
enum { RUN_QUANTUM, RUN_EXIT, RUN_HALT, RUN_FAULT };

// ---- a loaded image ----
struct c4r_image {
    int *code;              // malloc'd code segment
    char *data;             // malloc'd data segment, zero-padded
    int *entry;             // absolute entry address
    int *cons;              // malloc'd, absolute constructor addresses
    int  ncons;
    int *des;               // malloc'd, absolute destructor addresses
    int  ndes;
};

// ---- loader.c ----
int  c4r_load(char *path, struct c4r_image *img);
void c4r_free(struct c4r_image *img);

// ---- smp.c ----
//
// The processor table. Not static, and in one module only: c4lc keeps
// statics per object file, so anything two modules share has to be a
// real extern -- the same reason src/c4ix/va.c exists.
extern struct c4_cpu *c4_cpus;
extern int c4_ncpu;

// Round-robin over every CPU in CPU_RUN, quantum instructions each.
// Returns the reason the machine stopped; CPU 0's status is the
// program's exit code.
int  c4_smp_init(int ncpu);
int  c4_smp_run(int quantum);
void c4_smp_free();
int  c4_cpu_start(int id, int entry, int stacktop);

// ---- vm.c ----
extern int c4mp_debug;      // -d: trace every instruction
void c4_vm_init();          // must run before the first c4_run
int  c4_run(struct c4_cpu * RESTRICT c, int quantum);
char *c4_opname(int op);    // five bytes wide, print with %.4s

#endif // __C4MP_H
