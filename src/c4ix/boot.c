//
// C4IX X2: boot.
//
// Ten .c4o objects (boot, con, va, host, sl4b, task, sched, sys,
// loader, init) compiled by c4lc -O -c and linked by c4rlink. Boots
// natively under c4m with preemptive scheduling and protected user
// tasks; degraded under plain c4 through the c4l loader with
// cooperative scheduling and no hardware boundary -- host_detect()
// tells the two apart with one INFO opcode.
//
// X2 scope: the syscall layer. User tasks run behind protected mode,
// so their IO reaches the kernel whether they ask through libc4ix or
// just call printf and get trapped.
//

#include "c4ix.h"

enum { PREEMPT_INTERVAL = 10000 };   // cycles between hard IRQs (c4m)

int main(int argc, char **argv) {
    struct task *t;
    int info;

    kprintf("C4IX booting, %d cycles spent loading the kernel image\n", __c4_cycles());
    info = host_detect();
    kprintf("c4ix: host %s, info 0x%x\n", host_name(), info);
    kprintf("c4ix: protected mode %s, preemption %s\n",
        host_has(C4IX_I_PROT) ? "on for user tasks" : "unavailable",
        host_type() == HOST_C4M ? "on" : "unavailable (cooperative)");

    // The VFS first: adopting the boot task opens its console fds.
    vfs_init();
    sched_init(host_type() == HOST_C4M ? PREEMPT_INTERVAL : 0);

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
