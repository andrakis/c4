//
// C4IX X1: boot.
//
// Nine .c4o objects (boot, con, va, host, sl4b, task, sched, loader,
// init) compiled by c4lc -O -c and linked by c4rlink. Boots natively
// under c4m with preemptive scheduling; degraded under plain c4
// through the c4l loader with cooperative scheduling -- host_detect()
// tells the two apart with one INFO opcode.
//
// X1 scope: the scheduler. Boot adopts itself as the idle task,
// starts init, and runs the round-robin until every other task is
// done. init demonstrates cooperative yields, preemption (c4m), and
// spawning a .c4r image from the host filesystem.
//

#include "c4ix.h"

enum { PREEMPT_INTERVAL = 10000 };   // cycles between hard IRQs (c4m)

int main(int argc, char **argv) {
    struct task *t;
    int info;

    kprintf("C4IX X1 booting\n");
    info = host_detect();
    kprintf("c4ix: host %s, info 0x%x\n", host_name(), info);
    kprintf("c4ix: protected mode %s, preemption %s\n",
        host_has(C4IX_I_PROT) ? "available" : "unavailable",
        host_type() == HOST_C4M ? "on" : "unavailable (cooperative)");

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
