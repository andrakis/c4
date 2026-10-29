//
// C4IX: boot.
//
// Thirteen .c4o objects (boot, console, va, host, sl4b, task, sched,
// vfs, sys, c4ke, loader, mmu, init) compiled by c4lc -O -c and linked by
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

// ---- the boot report ----
//
// What the kernel finds as it boots, into its log (console.c) the way a
// Linux kernel's goes into dmesg: the image, the processors and their
// clock, the devices, the syscalls. Every number is read from the
// machine or measured here; where the host does not say, the line says
// that instead. On the console too with -v.
int main(int argc, char **argv);
static int boot_cycles0;       // __c4_cycles() as main began (boot_done)
static int boot_ms0;           // ... and __time()

// "N.NN": a hundredth-precise quotient, for the MHz and milliseconds
static void boot_frac(char *buf, int num, int den) {
    int w, f, i, n;
    char d[12];
    if (den <= 0) { buf[0] = '?'; buf[1] = 0; return; }
    w = num / den;
    f = (num % den) * 100 / den;
    n = 0;
    d[n] = '0' + w % 10; ++n; w = w / 10;
    while (w) { d[n] = '0' + w % 10; ++n; w = w / 10; }
    i = 0;
    while (n) { --n; buf[i] = d[n]; ++i; }
    buf[i] = '.'; buf[i + 1] = '0' + f / 10; buf[i + 2] = '0' + f % 10; buf[i + 3] = 0;
}

// The host's capability bits, by name: what the VM does (the devices
// fitted to it have lines of their own, below)
static void boot_caps(int info) {
    klog("host: c4m%s%s%s%s%s%s%s%s\n",
        (info & C4IX_I_C4MJS) ? " (libjs)" : "",
        (info & C4IX_I_HRT) ? ", high-resolution timer" : "",
        (info & C4IX_I_SIG) ? ", signals" : "",
        (info & C4IX_I_FLT) ? ", float" : "",
        (info & C4IX_I_FPU) ? ", FPU" : "",
        (info & C4IX_I_PROT) ? ", protected mode" : "",
        (info & C4IX_I_SMP) ? ((info & C4IX_I_PAR) ? ", processors on threads" : ", processors") : "",
        (info & C4IX_I_MMU) ? ", MMU" : "");
}

void boot_report() {
    int info, code, data, bss, end, n, k, ro, cur;
    char mhz[16], ms[16];

    info = host_info();
    boot_caps(info);
    // the clock: how many cycles a millisecond of it is
    if (klog_calibrate()) {
        boot_frac(mhz, klog_cpms, 1000);
        klog("cpu: clock calibrated: %d cycles a millisecond, %s MHz\n", klog_cpms, mhz);
    } else klog("cpu: clock not calibrated: the machine's time did not move\n");
    if (!host_has(C4IX_I_C4MJS)) {
        klog("kernel: the host says nothing of where it loaded the image\n");
        return;
    }
    code = *(int *)C4IX_IMG_CODE;
    data = *(int *)C4IX_IMG_DATA;
    bss = *(int *)C4IX_IMG_BSS;
    end = *(int *)C4IX_IMG_END;
    if (code && end > code) {
        klog("kernel: image [mem 0x%08x-0x%08x], %d KB\n", code, end - 1, (end - code + 1023) / 1024);
        klog("kernel:   code 0x%08x-0x%08x (%d KB)\n", code, data - 1, (data - code + 1023) / 1024);
        klog("kernel:   data 0x%08x-0x%08x (%d KB)\n", data, bss - 1, (bss - data + 1023) / 1024);
        klog("kernel:   bss  0x%08x-0x%08x (%d KB)\n", bss, end - 1, (end - bss + 1023) / 1024);
        klog("kernel:   entry main 0x%08x\n", (int)&main);
    }
    // the drives (c4bb's DISK_*), the first selected again afterwards
    if (info & C4IX_I_DRV) {
        n = *(int *)C4IX_DISK_COUNT;
        cur = *(int *)C4IX_DISK_DRIVE;
        k = 0;
        while (k < n) {
            *(int *)C4IX_DISK_DRIVE = k;
            ro = *(int *)C4IX_DISK_RO;
            klog("drive %d: %s%s\n", k, ro ? "read-only" : "writable",
                k == 0 ? ", the boot disk" : (!ro ? ", where persist keeps changes" : ""));
            ++k;
        }
        *(int *)C4IX_DISK_DRIVE = cur;
    }
    if (info & C4IX_I_MBOX)
        klog("mailbox: %d KB at 0x%08x\n", *(int *)C4IX_MBOX_LEN / 1024, *(int *)C4IX_MBOX_BASE);
    if (info & C4IX_I_GUI)
        klog("display: %dx%d, capabilities 0x%x\n", *(int *)C4IX_GUI_W, *(int *)C4IX_GUI_H, *(int *)C4IX_GUI_CAPS);
    if (info & C4IX_I_SND) klog("sound: an OPL3 card, sample playback\n");
    if (info & C4IX_I_PIT) klog("timer: the PIT is fitted; C4IX schedules by the cycle interrupt instead\n");
    if (klog_cpms) {
        boot_frac(ms, PREEMPT_INTERVAL, klog_cpms);
        klog("timer: cycle interrupt every %d cycles, %s ms\n", PREEMPT_INTERVAL, ms);
    }
    klog("syscalls: %d (%d-%d), and %d C4KE services (%d-%d)\n",
        SYS_TOP - SYS_BASE, SYS_BASE, SYS_TOP - 1, CK_TOP - CK_BASE, CK_BASE, CK_TOP - 1);
}

// The boot is over (init has started the shell): what it cost
int boot_lastid;

void boot_done() {
    struct task *t;
    char ms[16];
    // what the boot started may still be running -- the root archive's
    // unpack, in the background -- and its end goes in the log too
    t = task_first();
    while (t) { if (t->id > boot_lastid) boot_lastid = t->id; t = task_next(t); if (t == task_first()) break; }
    boot_frac(ms, __time() - boot_ms0, 1000);
    klog("boot: done after %s s of machine time, %d cycles\n", ms, __c4_cycles() - boot_cycles0);
    if (vfs_nlazy)
        klog("vfs: %d files (%d KB) left on the host until first use\n", vfs_nlazy, (vfs_lazybytes + 1023) / 1024);
}

// The kernel task: everything from the scheduler's start to shutdown, on
// a stack of its own in the kernel arena (sched.c, "Off the boot stack").
static int kernel_main(int argc, int argv) {
    struct task *t;

    sched_init(PREEMPT_INTERVAL);

    if (!(t = task_create("init", (int)&init_main, argc, argv))) {
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

int main(int argc, char **argv) {
    int info, k, quiet;

    boot_cycles0 = __c4_cycles();
    boot_ms0 = __time();
    sched_early();
    // -v: the kernel log on the console too, from the first line to the
    // last; -q: not even the boot's
    quiet = 0;
    k = 1;
    while (k < argc) {
        if (argv[k][0] == '-' && argv[k][1] == 'v' && !argv[k][2]) klog_verbose = 1;
        if (argv[k][0] == '-' && argv[k][1] == 'q' && !argv[k][2]) quiet = 1;
        ++k;
    }
    kprintf("C4IX booting, %d cycles spent loading the kernel image\n", __c4_cycles());
    info = host_detect();
    kprintf("c4ix: host %s, info 0x%x\n", host_name(), info);
    // What the boot finds on the console as it goes, as Linux's says it,
    // where someone is watching: libjs (the page, cli.js). On native c4m
    // the log keeps it (dmesg): there the console is a test's transcript,
    // and the lines' times and clocks are never twice the same.
    klog_boot = !quiet && host_has(C4IX_I_C4MJS);
    if (!host_has(C4IX_I_C4M)) {
        kputs("c4ix: panic: this host is not c4m: C4IX needs its traps and cycle interrupt\n");
        return 1;
    }
    kprintf("c4ix: protected mode %s, preemption on\n",
        host_has(C4IX_I_PROT) ? "on for user tasks" : "unavailable");
    boot_report();

    // The kernel arena before anything else is allocated, so that it is
    // the heap's lowest block after the va area (sl4b.c)
    kmem_init(sched_ncpus());
    // The MMU, when the host has one, before anything runs in user mode
    mmu_init();
    // The VFS first: adopting the kernel task opens its console fds.
    vfs_init();
    return sched_offboot((int)&kernel_main, argc, (int)argv);
}
