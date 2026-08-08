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
        "JSRI,JSRS,JMPA,TLEV,DBG ,";
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
    return (__c4_info() & (C4I_C4 | C4I_HRT | C4I_SIG | C4I_FLT)) | C4I_C4M | C4I_PROT;
#else
    return C4I_C4M | C4I_HRT | C4I_SIG | C4I_FLT | C4I_PROT;
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
    int i, r, run, reason;

    pc = c->pc; sp = c->sp; bp = c->bp;
    a = c->a; mode = c->mode; cycle = c->cycle;
    ival = c->ival; tri = c->tri;
    traph = c->traph; ihand = c->ihand;

    run = 1;
    reason = RUN_QUANTUM;

    while (run && quantum) {
        if (quantum > 0) --quantum;
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
            if (i <= ADJ) {
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
            if (i <= ADJ || i == JSRI || i == JSRS) printf(" %d\n", *pc);
            else printf("\n");
        }

        switch (i) {
        case LEA:  a = (int)(bp + *pc++); break;          // local address
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
#ifdef __c4cc__
            a = __c4_sigint();
#else
            a = __c4_sigint();
#endif
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
            // __c4_invoke exists to let plain-c4-hosted code call a
            // computed address through a self-modifying stub. c4mp is
            // never hosted by plain c4, so there is nothing to do --
            // and nothing to fault, since native c4m is also a no-op.
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
