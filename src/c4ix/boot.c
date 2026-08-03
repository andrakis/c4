//
// C4IX X0: boot.
//
// The kernel is six .c4o objects (boot, con, va, host, task, init)
// compiled by c4lc -O -c and linked by c4rlink. It boots natively
// under c4m and, degraded, under plain c4 through the c4l loader --
// host_detect() tells the two apart with one INFO opcode.
//
// X0 scope: banner, host report, one init task, clean shutdown.
//

#include "c4ix.h"

int main(int argc, char **argv) {
    struct task *t;
    int info;

    kprintf("C4IX X0 booting\n");
    info = host_detect();
    kprintf("c4ix: host %s, info 0x%x\n", host_name(), info);
    kprintf("c4ix: protected mode %s, preemption %s\n",
        host_has(C4IX_I_PROT) ? "available" : "unavailable",
        host_type() == HOST_C4M ? "possible" : "cooperative only");

    if (!(t = task_create("init", (int)&init_main))) {
        kputs("c4ix: panic: cannot allocate init task\n");
        return 1;
    }
    kprintf("c4ix: task %d '%s' created\n", t->id, t->name);
    task_run(t);
    kprintf("c4ix: task %d '%s' exited with status %d\n",
        t->id, t->name, t->exitcode);

    task_shutdown();
    kprintf("c4ix: shutdown complete, %d tasks remain\n", task_count());
    return 0;
}
