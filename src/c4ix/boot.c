//
// C4IX: boot.
//
// Twelve .c4o objects (boot, console, va, host, sl4b, task, sched,
// vfs, sys, c4ke, loader, init) compiled by c4lc -O -c and linked by
// c4rlink. Boots under c4m with preemptive scheduling and protected
// user tasks. C4IX images use C4CY, PUTC, JSRI and JSRS, which plain
// c4 lacks, so plain c4 runs C4IX through c4m itself
// (c4 -> c4m.c -> C4IX, `make test-c4ix-c4`) and C4IX still sees a c4m
// host. A host that is not c4m-compatible, with its trap machinery, is
// refused at boot: the cooperative, no-boundary path there once was for
// one was never reached, and went in docs/c4ix-cpu.md step 10.
//
// User tasks run behind protected mode, so their IO reaches the kernel
// whether they ask through libc4ix or just call printf and get trapped.
//

#include "c4ix.h"

// Cycles in a time slice: between hard IRQs (c4m), and the software slice
// sched_trap keeps on the syscall path. Measured on C4IX (docs/c4ix-cpu.md,
// step 5): against 10000, C4KE's, a syscall-bound task makes 6% more calls
// and 9% more under the desktop, and 100000 adds only 1% more for twice
// the wait. 0.5 ms at the browser's 100 MHz.
enum { PREEMPT_INTERVAL = 50000 };

int main(int argc, char **argv) {
    struct task *t;
    int info;

    sched_early();
    kprintf("C4IX booting, %d cycles spent loading the kernel image\n", __c4_cycles());
    info = host_detect();
    kprintf("c4ix: host %s, info 0x%x\n", host_name(), info);
    if (!host_has(C4IX_I_C4M)) {
        kputs("c4ix: panic: this host is not c4m: C4IX needs its traps and cycle interrupt\n");
        return 1;
    }
    kprintf("c4ix: protected mode %s, preemption on\n",
        host_has(C4IX_I_PROT) ? "on for user tasks" : "unavailable");

    // The VFS first: adopting the kernel task opens its console fds.
    vfs_init();
    sched_init(PREEMPT_INTERVAL);

    if (!(t = task_create("init", (int)&init_main, argc, (int)argv))) {
        kputs("c4ix: panic: cannot create init task\n");
        return 1;
    }
    // Cycles from VM start to the moment userland first runs. This
    // is the boot cost, and it is the one number that can be
    // compared with C4KE on equal terms: same VM, same counter, same
    // meaning (see docs/c4ix-design.md 7).
    kprintf("c4ix: task %d '%s' created, scheduling after %d cycles\n",
        t->id, t->name, __c4_cycles());

    sched_run();
    sched_stop();

    task_shutdown();
    sl4b_stats();
    kprintf("c4ix: shutdown complete, %d tasks remain\n", task_count());
    return 0;
}
