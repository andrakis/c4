//
// c4mp interpreter.
//
// Instruction semantics are c4m's, opcode for opcode, because guest
// images cannot tell the two apart and must not need to. What differs
// is the shape around them:
//
//   * Dispatch is a switch. c4m dispatches through an if-else chain,
//     and a switch was tried there and reverted (29d6f57) for a good
//     reason that does not apply here: plain c4's compiler cannot
//     parse switch, so `./c4 c4m.c` stopped working. c4mp is never
//     parsed by plain c4 -- c4lc compiles it, and c4lc emits c4cc's
//     data-segment jumptable. The revert also measured switch as no
//     faster natively, its only real gain being 1.79x for a c4m
//     running inside a c4m. That nested case is exactly what c4mp
//     under c4m is, so here the gain is the one that survives.
//
//   * The registers live in a struct. c4_run loads them into locals,
//     runs a quantum, and stores them back, so switching processors
//     is a call with a different pointer rather than a hand-written
//     save and restore.
//
// The trap frame layout is c4m's exactly, and must stay that way:
// guest kernels build frames by hand at these offsets. From the
// handler's bp:
//
//   bp+0 handler bp   bp+1 &TLEV        bp+2 return pc   bp+3 saved sp
//   bp+4 saved bp     bp+5 saved a      bp+6 saved mode  bp+7 instruction
//   bp+8 trap type    bp+9 saved cycle interrupt interval
//
// C4 numbers parameters DOWN from the top of the frame, so the word
// pushed first lands highest and stays invisible to a handler that
// declares fewer parameters. That is how bp+9 was added without
// breaking every existing handler.
//

#include "c4mp.h"

int c4mp_debug;

// ---- raw terminal mode (TRAW opcode, M18) ---------------------------
// c4or1k needs fd 0 in raw mode so a typed Ctrl+C is delivered to the
// guest as a byte (its tty line discipline then signals the guest's
// foreground process) instead of the host terminal turning it into a
// SIGINT that kills the emulator. The C4 VM otherwise has no termios
// facility (that was why run-c4or1k.sh existed), so this host syscall
// adds exactly that one capability. tty-aware: a no-op returning 0 when
// fd 0 is not a tty (piped input, tests), so those paths are unchanged.
// Restored on normal exit via atexit and on SIGTERM/SIGHUP.
//
// c4.h has already done `#define int __INTPTR_TYPE__` by this point, so
// `int` here would be `long` -- wrong for termios/signal, whose real
// prototypes take a genuine `int`. Undef it around this block (the same
// trick c4.c/native.h use) and restore it after.
#ifndef __c4cc__
#undef int
#include <termios.h>
#include <unistd.h>
#include <stdlib.h>
#include <signal.h>
static struct termios c4_tty_saved;
static int c4_tty_raw_on;
static void c4_tty_restore() { if (c4_tty_raw_on) { tcsetattr(0, TCSANOW, &c4_tty_saved); c4_tty_raw_on = 0; } }
static void c4_tty_sig(int s) { c4_tty_restore(); signal(s, SIG_DFL); raise(s); }
static int c4_termraw(int on) {
    struct termios raw;
    if (on) {
        if (!isatty(0)) return 0;
        if (c4_tty_raw_on) return 1;
        if (tcgetattr(0, &c4_tty_saved)) return 0;
        raw = c4_tty_saved;
        cfmakeraw(&raw);
        tcsetattr(0, TCSANOW, &raw);
        c4_tty_raw_on = 1;
        atexit(c4_tty_restore);
        signal(SIGTERM, c4_tty_sig);
        signal(SIGHUP, c4_tty_sig);
        return 1;
    }
    c4_tty_restore();
    return 0;
}
#define int __INTPTR_TYPE__
#else
static int c4_termraw(int on) { return 0; } // no termios under a hosted c4mp
#endif

// The address of this word is the return PC pushed into every trap
// frame, so a handler's ordinary LEV lands on a TLEV. It holds the
// TLEV opcode itself; c4m does the same with tlev_instruction.
static int c4_tlev_word;

// Opcode names, five bytes each, printed with %.4s. A flat string
// rather than an array of pointers because that is the one form both
// compilers agree on, and it is what c4m's OPSL hands to guests.
static char *c4_opcodes;

#ifdef __c4cc__
// Hosted by c4m. Host signals arrive as a TRAP_SIGNAL delivered to
// c4mp itself, not as a variable to poll, so there is nothing here to
// forward yet -- guest signal delivery arrives with the signal stage.
static int pending_signal;
static int *signal_handlers;
#endif

void c4_vm_init() {
    c4_tlev_word = TLEV;
    c4_opcodes =
        "LEA ,IMM ,JMP ,JSR ,BZ  ,BNZ ,ENT ,ADJ ,LEV ,LI  ,LC  ,SI  ,SC  ,PSH ,"
        "OR  ,XOR ,AND ,EQ  ,NE  ,LT  ,GT  ,LE  ,GE  ,SHL ,SHR ,ADD ,SUB ,MUL ,DIV ,MOD ,"
        "OPEN,READ,CLOS,PRTF,MALC,FREE,MSET,MCMP,EXIT,"
        "PUTC,PUTS,RALC,MCPY,STRC,"
        "ITH ,_OPC,_BLT,_TRP,OPCD,"
        "_JMP,_ADJ,C4CF,C4CY,TIME,"
        "SIGH,SIGI,USLP,INFO,OPSL,"
        "C4IV,"
        "FLT ,"
        "JSRI,JSRS,JMPA,TLEV,DBG ,"
        "CPUI,CPUN,CPUS,CPUH,"
        "CAS ,XCHG,FADD,CWAI,CWAK,IPI ,"
        "LXI ,SXI ,TRAW,"
        "LDL ,LDG ,PSHL,PSHG,LEAP,IMMP,LIP ,ADDL,STL ,POPA,";
}

// One place decides which opcodes carry an operand word, the same rule
// c4m_has_operand states in c4m.c. LIP, ADDL and POPA take none.
int c4_has_operand(int op) {
    return op <= ADJ || op == JSRI || op == JSRS
        || (op >= LDL && op <= IMMP)   // LIP, ADDL and POPA take none
        || op == STL;
}

char *c4_opname(int op) {
    if (op < 0 || op >= INS_SIZE) return "????";
    return c4_opcodes + op * 5;
}

// Case-insensitive match of up to four characters, stopping at a
// space or NUL on either side -- the same rule c4m's __opcode uses,
// so a guest asking for "jsri" gets the same answer from both.
static int c4_opmatch(char *a, char *b) {
    int i, ca, cb;
    for (i = 0; i < 4; ++i) {
        ca = a[i]; cb = b[i];
        if (ca >= 'a' && ca <= 'z') ca = ca - 32;
        if (cb >= 'a' && cb <= 'z') cb = cb - 32;
        if (ca == ' ') ca = 0;
        if (cb == ' ') cb = 0;
        if (!ca || !cb) return 1;
        if (ca != cb) return 0;
    }
    return 1;
}

static int c4_opnum(char *name) {
    int r;
    for (r = 0; r < INS_SIZE; ++r)
        if (c4_opmatch(name, c4_opcodes + r * 5)) return r;
    printf("c4mp: no such opcode '%s'\n", name);
    return -1;
}

static int c4mp_info() {
#ifdef __c4cc__
    // Hosted: every timer, signal and float call below goes through
    // the host, so report what the host can actually do rather than
    // what c4mp would like to claim. C4I_C4 propagates for the same
    // reason -- if the host is ultimately plain c4, so are we.
    return (__c4_info() & (C4I_C4 | C4I_HRT | C4I_SIG | C4I_FLT))
           | C4I_C4M | C4I_PROT | C4I_SMP;
#else
    return C4I_C4M | C4I_HRT | C4I_SIG | C4I_FLT | C4I_PROT | C4I_SMP;
#endif
}

//
// Build a trap frame on the interrupted stack and enter the handler.
//
// Registers come in by address the way c4m's trap() takes them, not
// through the CPU struct: sp, bp and pc are locals of c4_run for the
// whole quantum, and handing over their addresses is what lets the
// interpreter keep them in machine registers between traps.
//
static void c4_trap(int type, int parameter, int *handler,
                    int **_sp, int **_bp, int **_pc,
                    int a, int mode, int ival) {
    int *t, *sp, *bp, *pc;

    sp = *_sp; bp = *_bp; pc = *_pc;

    // Worded exactly as c4m words it, so the two can be diffed with
    // nothing masked but the addresses and the VM's own name.
    if (c4mp_debug || !handler) {
        printf("Trap type ");
        if (type == TRAP_ILLOP)             printf("TRAP_ILLOP");
        else if (type == TRAP_HARD_IRQ)     printf("TRAP_HARD_IRQ");
        else if (type == TRAP_SOFT_IRQ)     printf("TRAP_SOFT_IRQ");
        else if (type == TRAP_SIGNAL)       printf("TRAP_SIGNAL");
        else if (type == TRAP_SEGV)         printf("TRAP_SEGV");
        else if (type == TRAP_OPV)          printf("TRAP_OPV");
        else if (type == TRAP_PM_VIOLATION) printf("TRAP_PM_VIOLATION");
        else printf("(unknown %d)", type);
        printf(" start, offending instruction %d at 0x%X, sp=0x%X, bp=0x%X, handler=0x%X\n",
               *(pc - 1), pc - 1, sp, bp, handler);
    }
    // A trap with nowhere to go leaves the machine exactly as it was,
    // so execution simply continues past the offending instruction.
    // Silently corrupting the stack instead would be worse.
    if (!handler) {
        printf("c4mp: missed a trap, no handler installed\n");
        return;
    }

    t = sp;
    // Slack, so the frame cannot land on top of the context it saves.
    sp = sp - C4MP_TRAP_OFFSET;

    // The interrupt state belongs to the interrupted context, so it is
    // saved with it. Pushed first, so it lands at bp+9 -- past the
    // seven parameters an older handler can see.
    *--sp = ival;
    *--sp = type;
    *--sp = parameter;
    *--sp = mode;
    *--sp = a;
    *--sp = (int)bp;
    *--sp = (int)t;                 // the sp the handler must restore
    *--sp = (int)pc;
    *--sp = (int)&c4_tlev_word;     // where the handler's LEV lands
    --sp;                           // the handler's own saved-bp slot
    bp = sp;
    *sp = (int)bp;

    // Room for the handler's locals, read from the ENT we skip over.
    sp = sp - *(handler + 1);

    *_sp = sp;
    *_bp = bp;
    *_pc = handler + 2;             // past ENT n
}

// A protected task touching a guarded opcode: hand it to the kernel
// and let the kernel decide what it meant. Every such site in c4m
// also drops to unprotected mode and masks the interrupt, because the
// handler is kernel code and must not itself trap on its first
// syscall.
#define PM_TRAP(op) do { \
        c4_trap(TRAP_PM_VIOLATION, (op), traph, &sp, &bp, &pc, a, mode, ival); \
        ival = 0; \
        mode = MODE_UNPROTECTED; \
    } while (0)

//
// Run one CPU for up to `quantum` instructions. A negative quantum
// means "until it stops", which is how the single-CPU case and pass
// 2's per-thread loop both run.
//
// The registers are copied into locals here and written back at the
// end. That copy is the whole cost of switching processors, and it is
// paid once per quantum rather than once per instruction: at a
// quantum of 1000 it is under half a percent. Reading them through
// `c` instead would be tidier, but under c4lc every access becomes a
// real memory reference, and the interrupt check alone reads two of
// them on every single instruction.
//
// RESTRICT still earns its place: it promises the compiler that guest
// stores -- which write anywhere in the heap -- cannot alias this
// struct, which is what allows `c` to be read once rather than after
// every SI and SC.
//
int c4_run(struct c4_cpu * RESTRICT c, int quantum) {
    int *pc, *sp, *bp, *traph, *ihand, *t;
    int a, mode, cycle, ival, tri;
    int i, r, run, reason, deadline;

    pc = c->pc; sp = c->sp; bp = c->bp;
    a = c->a; mode = c->mode; cycle = c->cycle;
    ival = c->ival; tri = c->tri;
    traph = c->traph; ihand = c->ihand;

    // The quantum is a cycle deadline. -1 means "no deadline": cycle
    // only ever counts up from zero, so it can never reach it. The +1
    // is because the test sits after the increment, and is what makes
    // quantum=0 run nothing and quantum=n run exactly n instructions.
    //
    // How this is written matters more than it looks, all measured on
    // a 20M-iteration loop against c4m's 1.26s:
    //
    //   while (run && quantum) + conditional decrement    1.41s
    //   while (cycle != deadline)                         1.36s
    //   while (deadline--)                                1.75s
    //   while (run) + if (cycle == deadline) break        1.28s   <-- this
    //   no quantum check at all                           1.14s
    //
    // The obvious form costs 24% of runtime. Making the deadline the
    // loop condition is worse still, because it puts the freshly
    // incremented cycle on the critical path of the loop branch,
    // whereas `run` almost never changes and predicts perfectly.
    //
    // The 1.14s floor is real and reachable: an unbounded run needs no
    // deadline test at all, and pass 2 gives every CPU its own thread
    // and calls this with quantum = -1 exactly once. Specialising the
    // unbounded case is worth doing when that lands and both modes can
    // be measured; it is not worth duplicating the dispatch for now.
    deadline = quantum < 0 ? -1 : cycle + quantum;

    run = 1;
    reason = RUN_QUANTUM;

    while (run) {
        // Before the increment, not after. After, the aborting
        // iteration still bumps the counter, so every slice inflates
        // it by one -- hello.c4r reported 55 cycles under -q 1 against
        // 28 unbroken. cycle is not a statistic: C4CY hands it to
        // guests and the preemption tick is cycle % ival, so drift
        // would move when a kernel gets interrupted.
        if (cycle == deadline) break;
        ++cycle;

        // TLEV restores five registers in one step and there is no way
        // to express "half returned", so an interrupt landing on it
        // would hand the handler a context belonging to neither side.
        // Kernels that opt into tri never reach it armed; for the rest
        // stepping over it costs one tick.
        if (ival && !(cycle % ival) && *pc != TLEV) {
            c4_trap(TRAP_HARD_IRQ, HIRQ_CYCLE, ihand, &sp, &bp, &pc, a, mode, ival);
            ival = 0;
            mode = MODE_UNPROTECTED;
        } else if (c->ipipend) {
            // A directed interrupt from another processor. Delivered
            // through the same handler as the cycle tick, with
            // HIRQ_IPI to tell them apart, and unconditionally: an IPI
            // is a request from a peer, not a timer, so honouring the
            // preemption mask would let a CPU that has masked itself
            // ignore its peers indefinitely.
            //
            // Read through c rather than cached in a local, which is
            // what makes a CPU interrupting ITSELF work -- it sets the
            // flag and sees it on the next instruction. It is also the
            // one field here another processor writes, so PASS 2 MUST
            // make it volatile or atomic: with real threads nothing
            // stops the compiler hoisting this load out of the loop,
            // and the symptom would be an IPI that is simply never
            // noticed. Simulated SMP is safe because no other CPU runs
            // between this instruction and the next.
            c->ipipend = 0;
            c4_trap(TRAP_HARD_IRQ, HIRQ_IPI, ihand, &sp, &bp, &pc, a, mode, ival);
            ival = 0;
            mode = MODE_UNPROTECTED;
        } else if (pending_signal && !(tri && !ival)) {
            // Deferred, not dropped: pending_signal stays set, so a
            // signal masked here arrives as soon as the mask lifts.
            // Only kernels that opted into tri have told us the
            // interval IS a mask; without that, "masked" and "never
            // enabled preemption" are indistinguishable and deferring
            // would mean never delivering.
            c4_trap(TRAP_SIGNAL, pending_signal, (int *)signal_handlers[pending_signal],
                    &sp, &bp, &pc, a, mode, ival);
            ival = 0;
            mode = MODE_UNPROTECTED;
            pending_signal = 0;
        }

        i = *pc++;

        // OPCD executes the opcode named on the stack, which is how
        // custom syscalls reach a kernel. Resolved before dispatch so
        // the trace shows what actually ran. Opcodes 0..ADJ read an
        // inline operand that OPCD has no way to supply.
        if (i == OPCD) {
            i = *sp;
            if (c4_has_operand(i)) {
                printf("%.4s does not support opcodes requiring arguments (%.4s given)\n",
                       c4_opname(OPCD), c4_opname(i));
                c4_trap(TRAP_OPV, i, traph, &sp, &bp, &pc, a, mode, ival);
                ival = 0;
                mode = MODE_UNPROTECTED;
                i = C4CY;   // harmless substitute
            }
        }

        if (c4mp_debug) {
            printf("0x%-8X %-8d ", pc - 1, cycle);
            printf("A=0x%-8X> ", a);
            if (i >= 0 && i < INS_SIZE) printf("%.4s", c4_opname(i));
            else printf("unknown %-8d (0x%X)", i, i);
            if (c4_has_operand(i)) printf(" %d\n", *pc);
            else printf("\n");
        }

        switch (i) {
        case LEA:  a = (int)(bp + *pc++); break;          // local address
        // The fused opcodes. Each is exactly the sequence it replaces,
        // written out -- docs/fused-opcodes.md.
        case LDL:  a = *(int *)(bp + *pc++); break;
        case LDG:  a = *(int *)*pc++; break;
        case PSHL: a = *(int *)(bp + *pc++); *--sp = a; break;
        case PSHG: a = *(int *)*pc++; *--sp = a; break;
        case LEAP: a = (int)(bp + *pc++); *--sp = a; break;
        case IMMP: a = *pc++; *--sp = a; break;
        case LIP:  a = *(int *)a; *--sp = a; break;
        case ADDL: a = *(int *)(*sp++ + a); break;
        case STL:  *(int *)(bp + *pc++) = a; break;
        case POPA: a = *sp++; break;
        case IMM:  a = *pc++; break;                      // immediate / global address
        case JMP:  pc = (int *)*pc; break;
        case JMPA: pc = (int *)a; break;                  // jump through the accumulator
        case _JMP: pc = (int *)*sp++; break;              // __c4_jmp
        case JSR:  { *--sp = (int)(pc + 1); pc = (int *)*pc; } break;
        case JSRI: { *--sp = (int)(pc + 1); pc = (int *)*pc; pc = (int *)*pc; } break;
        case JSRS: { *--sp = (int)(pc + 1); pc = (int *)*(bp + *pc); } break;
        case BZ:   pc = a ? pc + 1 : (int *)*pc; break;
        case BNZ:  pc = a ? (int *)*pc : pc + 1; break;
        case ENT:  { *--sp = (int)bp; bp = sp; sp = sp - *pc++; } break;
        case ADJ:  sp = sp + *pc++; break;
        case _ADJ: sp = sp + *sp; break;                  // __c4_adjust
        case LEV:  { sp = bp; bp = (int *)*sp++; pc = (int *)*sp++; } break;
        case LI:   a = *(int *)a; break;
        case LC:   a = *(char *)a; break;
        case SI:   *(int *)*sp++ = a; break;
        case SC:   a = *(char *)*sp++ = a; break;
        case PSH:  *--sp = a; break;

        case OR:   a = *sp++ |  a; break;
        case XOR:  a = *sp++ ^  a; break;
        case AND:  a = *sp++ &  a; break;
        case EQ:   a = *sp++ == a; break;
        case NE:   a = *sp++ != a; break;
        case LT:   a = *sp++ <  a; break;
        case GT:   a = *sp++ >  a; break;
        case LE:   a = *sp++ <= a; break;
        case GE:   a = *sp++ >= a; break;
        case SHL:  a = *sp++ << a; break;
        case SHR:  a = *sp++ >> a; break;
        case ADD:  a = *sp++ +  a; break;
        case SUB:  a = *sp++ -  a; break;
        case MUL:  a = *sp++ *  a; break;
        case DIV:  a = *sp++ /  a; break;
        case MOD:  a = *sp++ %  a; break;

        // Opcodes that reach the host are the protected-mode boundary.
        case OPEN:
            if (mode == MODE_UNPROTECTED) a = open((char *)sp[1], *sp);
            else PM_TRAP(OPEN);
            break;
        case READ:
            if (mode == MODE_UNPROTECTED) a = read(sp[2], (char *)sp[1], *sp);
            else PM_TRAP(READ);
            break;
        case CLOS:
            if (mode == MODE_UNPROTECTED) a = close(*sp);
            else PM_TRAP(CLOS);
            break;
        case PUTC:
            if (mode == MODE_UNPROTECTED) a = putchar(*(char *)sp);
            else PM_TRAP(PUTC);
            break;
        case PUTS:
            if (mode == MODE_UNPROTECTED) a = puts((char *)*sp);
            else PM_TRAP(PUTS);
            break;
        case PRTF:
            if (mode == MODE_UNPROTECTED) {
                // The argument count is the operand of the ADJ that
                // follows this instruction -- the caller's own cleanup
                // tells us how many words it pushed.
                r = pc[1];
                t = sp + r;
                if (r > 7) { printf("Too many arguments to printf!\n"); exit(-1); }
                else a = printf((char *)t[-1], t[-2], t[-3], t[-4], t[-5], t[-6], t[-7]);
            } else PM_TRAP(PRTF);
            break;
        case MALC:
            if (mode == MODE_UNPROTECTED) a = (int)malloc(*sp);
            else PM_TRAP(MALC);
            break;
        case FREE:
            if (mode == MODE_UNPROTECTED) free((char *)*sp);
            else PM_TRAP(FREE);
            break;
        case EXIT:
            // Guarded like the rest: a protected task must not be able
            // to halt the machine, so the kernel decides what exit
            // means for it.
            if (mode == MODE_UNPROTECTED) {
                c->status = *sp;
                c->state = CPU_HALT;
                reason = RUN_EXIT;
                run = 0;
            } else PM_TRAP(EXIT);
            break;

        // Memory operations are unguarded in c4m, and stay so here:
        // they touch only memory the guest already addresses.
        case MSET: a = (int)memset((char *)sp[2], sp[1], *sp); break;
        case MCMP: a = memcmp((char *)sp[2], (char *)sp[1], *sp); break;
        case MCPY: a = (int)memcpy((char *)sp[2], (char *)sp[1], *sp); break;

        case STRC:
            // A frame walk without names. c4m resolves these against
            // its compiler's symbol table, which c4mp does not have --
            // the image carries a symbol section that could do the job,
            // but the loader discards it today.
            {
                int *fp;
                printf("c4mp: stacktrace (no symbols)\n");
                fp = bp;
                for (r = 0; r < 32 && fp; ++r) {
                    printf("  [%d] frame 0x%X return 0x%X\n", r, fp, *(fp + 1));
                    if ((int *)*fp <= fp) break;
                    fp = (int *)*fp;
                }
            }
            break;

        case ITH:
            // install_trap_handler(0) removes it; either way the old
            // one comes back, so a kernel can chain.
            a = (int)traph;
            traph = *sp ? (int *)*sp : 0;
            break;

        case _OPC: a = c4_opnum((char *)*sp); break;
        case OPSL: a = (int)c4_opcodes; break;
        case C4CY: a = cycle; break;
        case TIME:
#ifdef __c4cc__
            a = __time();
#else
            a = c4m_time();
#endif
            break;
        case USLP:
#ifdef __c4cc__
            a = __c4_usleep(*sp);
#else
            a = usleep(*sp);
#endif
            break;
        case SIGI:
            a = __c4_sigint();
            break;
        case SIGH:
#ifdef __c4cc__
            // Registering a host signal handler from inside a hosted
            // c4mp would install a c4mp-guest address in the host's
            // table, which is not an address the host can call.
            a = 0;
#else
            a = (int)__c4_signal(sp[1], (int *)sp[0]);
#endif
            break;

        case C4CF:
            // __c4_configure(option, value): option in sp[1], value in
            // sp[0], previous value returned. Deliberately unprivileged
            // -- C4KE calls it from protected mode.
            if (sp[1] == CONF_CYCLE_INTERRUPT_INTERVAL) { a = ival; ival = sp[0]; }
            else if (sp[1] == CONF_CYCLE_INTERRUPT_HANDLER) { a = (int)ihand; ihand = (int *)sp[0]; }
            else if (sp[1] == CONF_TRAP_RESTORES_INTERVAL) { a = tri; tri = sp[0]; }
            else {
                printf("c4mp: __c4_configure: no such option %d\n", sp[1]);
                c->status = -100;
                reason = RUN_FAULT;
                run = 0;
            }
            break;

        case INFO:
            if (mode == MODE_UNPROTECTED) a = c4mp_info() | (traph ? C4I_TRAPH : 0);
            else PM_TRAP(INFO);
            break;

        case _TRP:
            // __c4_trap(type, signal). Neither this nor DBG drops to
            // unprotected mode, matching c4m: sched_yield rides on it
            // and must not silently gain privilege.
            c4_trap(sp[1], sp[0], traph, &sp, &bp, &pc, a, mode, ival);
            break;
        case DBG:
            c4_trap(TRAP_DEBUG, 0, traph, &sp, &bp, &pc, a, mode, ival);
            break;

        case TLEV:
            // Trap return. The handler may have rewritten any of these
            // slots; that rewriting IS how a kernel switches tasks.
            sp = bp;
            t = sp + 2;
            pc   = (int *)*t++;
            sp   = (int *)*t++;
            bp   = (int *)*t++;
            a    = (int)*t++;
            mode = (int)*t++;
            // t now addresses bp+7; the interval sits two words past it.
            if (tri) ival = *(t + 2);
            break;

        case C4IV:
            // __c4_invoke(addr): call a computed address as if it were
            // a C4 function, via c4m's self-modifying stub.
#ifdef __c4cc__
            // Hosted, which is what the early c4mp targets: pass it
            // straight to the host. Guest, c4mp and host share one
            // address space, so the address on the stack is one the
            // host's stub can jump to.
            a = __c4_invoke(*sp);
#else
            // Native c4m compiles its own C4IV out entirely (the stub
            // is #if NOT_NATIVE), leaving the accumulator alone. Do
            // the same, so the two agree.
#endif
            break;

        case FLT:
#ifdef __c4cc__
            // No float opcodes when hosted: the host's FLT would need
            // a host address for its operand block.
            c4_trap(TRAP_ILLOP, FLT, traph, &sp, &bp, &pc, a, mode, ival);
#else
            a = c4_float_instruction(sp);
#endif
            break;

        case TRAW:
            // __c4_termraw(on): put fd 0 in (on!=0) or out of raw mode.
            // Returns 1 if raw mode is in effect, else 0.
            a = c4_termraw(*sp);
            break;

        // ---- processors ----
        case CPUI: a = c->id; break;
        case CPUN: a = c4_ncpu; break;
        case CPUS:
            // __c4_cpu_start(id, entry, stacktop): args are pushed
            // left to right, so the last one pushed is on top.
            a = c4_cpu_start(sp[2], sp[1], sp[0]);
            break;
        case CPUH:
            // Halt this processor, not the machine. Reached either by
            // the guest calling it or by a secondary's entry function
            // returning into the sentinel c4_cpu_start planted.
            c->state = CPU_HALT;
            reason = RUN_HALT;
            run = 0;
            break;

        // ---- atomics ----
        //
        // Unguarded, like MSET/MCMP/MCPY: they touch only memory the
        // guest already addresses, so protected mode has no interest
        // in them.
        //
        // Every one of these is a plain read-modify-write, and that is
        // CORRECT here rather than a shortcut: a CPU only ever changes
        // at a c4_run boundary, so an instruction cannot be split.
        // Pass 2 replaces the bodies with __atomic_* builtins and
        // nothing above this line changes.
        case CAS:
            // __c4_cas(addr, expect, new) -> the value found there
            t = (int *)sp[2];
            a = *t;
            if (a == sp[1]) *t = sp[0];
            break;
        case XCHG:
            // __c4_xchg(addr, val) -> the old value
            t = (int *)sp[1];
            a = *t;
            *t = sp[0];
            break;
        case FADD:
            // __c4_fadd(addr, delta) -> the old value
            t = (int *)sp[1];
            a = *t;
            *t = a + sp[0];
            break;

        // ---- park and wake ----
        case CWAI:
            // __c4_wait(addr, val): park while *addr is still val.
            // Returns 1 if it did not park because the value had
            // already changed, 0 when woken. Callers should re-test in
            // a loop -- a wake is a hint, not a guarantee, and IPI
            // releases a parked CPU too.
            t = (int *)sp[1];
            if (!t || *t != sp[0]) { a = 1; break; }
            c->waitaddr = t;
            c->waitval = sp[0];
            c->state = CPU_WAIT;
            a = 0;
            reason = RUN_WAIT;
            run = 0;
            break;
        case CWAK:
            // __c4_wake(addr, n) -> how many were woken; n <= 0 = all
            a = c4_cpu_wake(sp[1], sp[0]);
            break;
        case IPI:
            // __c4_ipi(cpu) -> 1 if it was raised
            a = c4_cpu_ipi(*sp);
            break;

        // ---- CISC: fused array-element load/store (M12, -mcisc) ----
        //
        // Not syscalls -- no ADJ follows these, unlike CPUS/CAS/etc.
        // above. c4lc emits them directly in place of the IMM/PSH/
        // SHL/ADD/LI (or .../ADD/PSH/../SI) sequence indexing a
        // known-8-byte-element array otherwise costs, only when
        // -mcisc is given. See c4lc-gen.lisp's g:indexload-cisc/
        // g:indexstore-cisc for the exact codegen shape each expects.
        case LXI:
            // a = *(int*)(base + index*8); base = *sp++ (popped),
            // index = a (already in the accumulator).
            a = *(int *)(*sp++ + a * 8);
            break;
        case SXI:
            // *(int*)(base + index*8) = a (value already in the
            // accumulator); base = sp[1], index = sp[0], both popped.
            *(int *)(sp[1] + sp[0] * 8) = a;
            sp += 2;
            break;

        default:
            // Everything unknown becomes a trap, which is the whole
            // custom-opcode mechanism: a kernel claims a number and
            // implements it here. RALC lands here too, matching c4m,
            // where realloc has never been implemented.
            c4_trap(TRAP_ILLOP, i, traph, &sp, &bp, &pc, a, mode, ival);
            // A handler is kernel code and must run privileged, or its
            // own first syscall raises a second trap on top of this one.
            mode = MODE_UNPROTECTED;
            break;
        }
    }

    c->pc = pc; c->sp = sp; c->bp = bp;
    c->a = a; c->mode = mode; c->cycle = cycle;
    c->ival = ival; c->tri = tri;
    c->traph = traph; c->ihand = ihand;
    return reason;
}
