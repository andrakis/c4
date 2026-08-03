//
// C4IX init: the first task. At X0 it only has to exist -- prove
// that a task runs with kernel services (console, host queries)
// alive and returns a status the kernel can report.
//

#include "c4ix.h"

int init_main() {
    int i, acc;

    kprintf("init: c4ix init v0 on %s\n", host_name());

    acc = 0;
    i = 1;
    while (i <= 10) { acc = acc + i; ++i; }
    kprintf("init: sum 1..10 = %d, tasks alive %d\n", acc, task_count());

    return 0;
}
