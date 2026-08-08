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
    printf("  -q N     run in slices of N instructions (default: one unbroken run)\n");
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
    struct c4_cpu cpu;
    int *stack, *boot, *p, *sp;
    int stacksz, verbose, nboot, i, r, quantum;
    char opt;

    c4_vm_init();
    stacksz = C4MP_STACK_SZ;
    quantum = -1;
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

    // Nothing reads bp before the trampoline's first ENT, so pointing
    // it at the stack top is enough.
    cpu.pc = boot;
    cpu.sp = sp;
    cpu.bp = sp;
    cpu.a = 0;
    cpu.mode = MODE_UNPROTECTED;
    cpu.cycle = 0;
    cpu.state = CPU_RUN;
    cpu.status = 0;
    cpu.traph = 0;
    cpu.ihand = 0;
    cpu.ival = 0;
    cpu.tri = 0;
    cpu.stkbase = stack;

    // Slicing must not change behaviour: the only difference between
    // -q 1 and one unbroken run is how many times the registers make
    // the round trip through struct c4_cpu. That is the whole
    // mechanism stage 2's second processor rests on, so it is worth
    // being able to exercise it against a single CPU first, where any
    // difference in output is unambiguously this code's fault.
    do {
        r = c4_run(&cpu, quantum);
    } while (r == RUN_QUANTUM && cpu.state == CPU_RUN);

    if (verbose)
        printf("c4mp: exit %d after %d cycles (reason %d)\n", cpu.status, cpu.cycle, r);

    free(boot);
    free(stack);
    c4r_free(&img);
#ifndef __c4cc__
    __c4_signal_shutdown();
#endif
    return cpu.status;
}
