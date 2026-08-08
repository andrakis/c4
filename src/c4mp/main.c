//
// c4mp entry point: parse options, load the image, build a bootstrap
// trampoline, and run it.
//
// The trampoline is the part worth explaining. C4IX runs an image's
// constructors from a task shim, which works because the shim is guest
// code and can simply call them. c4mp is the machine, so it has no way
// to call guest code at all -- there is no host-side call that lands
// in the interpreter. Instead it assembles a handful of real
// instructions:
//
//      JSR ctor0 ... JSR ctorN        constructors, in order
//      IMM argc; PSH; IMM argv; PSH   main's arguments
//      JSR entry; ADJ 2
//      PSH                            main's result, waiting for EXIT
//      JSR dtorN ... JSR dtor0        destructors, in reverse
//      EXIT                           takes the result off the stack
//
// and points the CPU at it. Destructors run between the PSH and the
// EXIT so they cannot lose the exit code: each JSR/LEV pair leaves the
// stack where it found it, so the pushed result is still on top.
//
// This also means c4mp needs no fake return frame under main, which
// c4m does need -- there, main is entered directly and its LEV has to
// land somewhere.
//

#include "c4mp.h"

static void usage() {
    printf("c4mp: C4 MultiProcessor -- runs .c4r images\n");
    printf("usage: ./c4mp [options] image.c4r [args...]\n");
    printf("  -d       trace every instruction\n");
    printf("  -v       report load and exit details\n");
    printf("  -p KB    stack size per CPU in KB (default %d)\n", C4MP_STACK_SZ / 1024);
    printf("  -q N     instructions per slice (default %d with -cpus, otherwise unbroken)\n", C4MP_QUANTUM);
    printf("  -cpus N  number of processors (default 1)\n");
    printf("  --       end of options\n");
}

// c4 has no atoi, and neither compiler provides one.
static int c4_atoi(char *s) {
    int n, neg;
    n = 0; neg = 0;
    if (*s == '-') { neg = 1; ++s; }
    while (*s >= '0' && *s <= '9') { n = n * 10 + (*s - '0'); ++s; }
    return neg ? -n : n;
}

int main(int argc, char **argv) {
    struct c4r_image img;
    int *stack, *boot, *p, *sp;
    int stacksz, verbose, nboot, i, r, quantum, ncpu;
    struct c4_cpu *cpu0;
    char opt;

    c4_vm_init();
    stacksz = C4MP_STACK_SZ;
    quantum = 0;      // 0 = not given; resolved once ncpu is known
    ncpu = 1;
    verbose = 0;
    c4mp_debug = 0;

    --argc; ++argv;
    while (argc > 0 && **argv == '-') {
        opt = (*argv)[1];
        if (opt == '-') { --argc; ++argv; break; }
        else if (opt == 'd') c4mp_debug = 1;
        else if (opt == 'v') verbose = 1;
        else if (opt == 'q') {
            --argc; ++argv;
            if (argc <= 0) { printf("c4mp: -q needs a slice length\n"); return -1; }
            quantum = c4_atoi(*argv);
            if (quantum < 1) { printf("c4mp: -q must be at least 1\n"); return -1; }
        }
        else if (opt == 'c') {
            --argc; ++argv;
            if (argc <= 0) { printf("c4mp: -cpus needs a count\n"); return -1; }
            ncpu = c4_atoi(*argv);
            if (ncpu < 1) { printf("c4mp: -cpus must be at least 1\n"); return -1; }
        }
        else if (opt == 'p') {
            --argc; ++argv;
            if (argc <= 0) { printf("c4mp: -p needs a size in KB\n"); return -1; }
            stacksz = c4_atoi(*argv) * 1024;
            if (stacksz < 4096) { printf("c4mp: stack too small\n"); return -1; }
        }
        else { printf("c4mp: unrecognised option '%s'\n", *argv); usage(); return -1; }
        --argc; ++argv;
    }
    if (argc <= 0) { usage(); return -1; }

    if (!c4r_load(*argv, &img)) return -1;
    if (verbose)
        printf("c4mp: %s loaded, entry 0x%X, %d constructors, %d destructors\n",
               *argv, img.entry, img.ncons, img.ndes);

    if (!(stack = (int *)malloc(stacksz))) {
        printf("c4mp: could not allocate a %d byte stack\n", stacksz);
        c4r_free(&img);
        return -1;
    }
    sp = (int *)((int)stack + stacksz);

    nboot = img.ncons * 2 + img.ndes * 2 + 12;
    if (!(boot = (int *)malloc(nboot * sizeof(int)))) {
        printf("c4mp: could not allocate the bootstrap\n");
        free(stack);
        c4r_free(&img);
        return -1;
    }
    p = boot;
    for (i = 0; i < img.ncons; ++i) { *p++ = JSR; *p++ = img.cons[i]; }
    *p++ = IMM; *p++ = argc;
    *p++ = PSH;
    *p++ = IMM; *p++ = (int)argv;
    *p++ = PSH;
    *p++ = JSR; *p++ = (int)img.entry;
    *p++ = ADJ; *p++ = 2;
    *p++ = PSH;
    for (i = img.ndes - 1; i >= 0; --i) { *p++ = JSR; *p++ = img.des[i]; }
    *p++ = EXIT;

#ifndef __c4cc__
    __c4_signal_init();
#endif

    // With one processor there is nothing to interleave, so an
    // unbroken run is both fastest and exactly what c4m does. With
    // more than one an unbounded quantum would let CPU 0 finish before
    // CPU 1 ever started, which is not a multiprocessor.
    if (!quantum) quantum = (ncpu > 1) ? C4MP_QUANTUM : -1;

    if (!c4_smp_init(ncpu)) {
        printf("c4mp: could not allocate %d processors\n", ncpu);
        free(boot); free(stack); c4r_free(&img);
        return -1;
    }
    // CPU 0 starts on the bootstrap. Nothing reads bp before the
    // trampoline's first ENT, so the stack top will do.
    cpu0 = c4_cpus;
    cpu0->pc = boot;
    cpu0->sp = sp;
    cpu0->bp = sp;
    cpu0->mode = MODE_UNPROTECTED;
    cpu0->state = CPU_RUN;
    cpu0->stkbase = stack;
    // Every other processor stays CPU_OFF until the guest starts it
    // with __c4_cpu_start. There is no "boot all CPUs at main": which
    // processors exist is the machine's business, what runs on them is
    // the guest's.

    r = c4_smp_run(quantum);

    if (verbose) {
        printf("c4mp: exit %d after %d cycles (reason %d)\n",
               cpu0->status, cpu0->cycle, r);
        for (i = 1; i < c4_ncpu; ++i)
            printf("c4mp: cpu %d ran %d cycles\n", i, c4_cpus[i].cycle);
    }
    i = cpu0->status;
    // A machine that wedged must not exit like one that finished. The
    // deadlock diagnostic prints, but a caller comparing exit codes --
    // which is what the test suite does -- would otherwise see success.
    // __c4_configure already sets a status of its own, so only supply
    // one where nothing did.
    if (r == RUN_FAULT && !i) i = -1;

    c4_smp_free();
    free(boot);
    free(stack);
    c4r_free(&img);
#ifndef __c4cc__
    __c4_signal_shutdown();
#endif
    return i;
}
