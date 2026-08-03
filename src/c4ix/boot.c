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

    kprintf("C4IX X2 booting\n");
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
    kprintf("c4ix: task %d '%s' created, scheduling\n", t->id, t->name);

    sched_run();
    sched_stop();

    task_shutdown();
    sl4b_stats();
    kprintf("c4ix: shutdown complete, %d tasks remain\n", task_count());
    return 0;
}
