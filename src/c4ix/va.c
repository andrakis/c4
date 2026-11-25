//
// C4IX varargs: the one va area for the whole image.
//
// See c4ix.h for why this is a module of its own: the stock stdarg.h
// makes everything static, and statics never merge across objects,
// so caller and callee would account against different counters.
//

#include "c4ix.h"

enum { VA_STACK = 256 };

static int *va_stack;
static int  va_vptr;

// __c4cc_make_va runs while the outer call's arguments are already
// on the stack, so it keeps its working state in globals, exactly
// like the stock stdarg.h implementation it replaces.
static int *va_m_ptr;
static int *va_m_arg;
static int  va_m_n;

// The compiler inserts a call to this at every variadic call site;
// its result rides in the variadic fake slot that va_start reads.
//
// It takes sched_lock and does NOT release it: the matching release
// is in __c4ix_va_adj, which va_end calls. The lock therefore spans
// the whole variadic call, and it has to.
//
// The area is one shared LIFO, but a variadic call is not atomic:
// the compiler emits "push args; call make_va; call the function",
// and va_end runs inside the callee. Let preemption in anywhere
// between and two tasks interleave push/pop on one stack. Pops go by
// COUNT, not position, so a task popping its own arguments can
// rewind the cursor BELOW another task's still-live arguments -- the
// next push then overwrites them, and the victim reads a %s pointer
// that is now an integer. That fault looks like a wild load in the
// VM, nowhere near the kprintf that caused it.
//
// Holding the lock across the call makes every variadic call atomic
// against preemption, which also gives kprintf its line atomicity.
// The contract: a variadic callee MUST use va_start/va_end, or the
// lock leaks. In this kernel kprintf is the only one.
int *__c4cc_make_va(int count) {
    sched_lock();
    va_m_n   = count;
    va_m_arg = &count + count;
    va_m_ptr = &va_stack[va_vptr];

    // The count first (va_end pops it and the args in one adjust),
    // then the arguments in source order.
    va_stack[va_vptr] = count; ++va_vptr;
    while (va_m_n--) {
        va_stack[va_vptr] = *va_m_arg; ++va_vptr;
        --va_m_arg;
    }
    return va_m_ptr;   // lock stays held until __c4ix_va_adj
}

// va_end's release half, extern so every module pops the same
// counter -- and the release of the lock __c4cc_make_va took.
void __c4ix_va_adj(int n) {
    va_vptr = va_vptr - n;
    sched_unlock();
}

static void __attribute__((constructor)) va_ctor() {
    int bytes;
    if (!(va_stack = malloc(bytes = sizeof(int) * VA_STACK))) {
        // kprintf needs this module, so the panic path is putchar-only
        kputs("c4ix: panic: va area allocation failed\n");
        exit(-1);
    }
    memset(va_stack, 0, bytes);
    va_vptr = 0;
}

static void __attribute__((destructor)) va_dtor() {
    if (va_stack) free(va_stack);
}
