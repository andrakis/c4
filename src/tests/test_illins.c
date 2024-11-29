/**
 * OBSOLETE C4 Test: Illegal instruction handler
 * Was part of a test for adding new instructions, but uses old c4m constructs.
 *
 * Will not compile on gcc or c4, must use c4cc.
 *   ./c4 c4m.c c4cc.c asm-c4r.c -- -c test_illins.c > test_illins.c4r
 *   ./c4plus c4cc.c asm-c4r.c -- -c test_illins.c   # writes to test_illins.c4r
 *
 * Requires either c4m or c4plus to run.
 *   ./c4 c4m.c test_illins.c4r
 *   ./c4plus test_illins.c4r
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef __c4__
#define int long long
#pragma GCC diagnostic ignored "-Wformat"
#endif

enum { C4__ADJUST = 128 }; // Our own command that __c4_adjusts the stack pointer
enum { H_CB, H__Sz, H__BASE = 128, H__COUNT = 128 };

static int *illins_handlers;

#define _is_ins_handled(ins)     (ins >= H__BASE && ins <= (H__BASE + H__COUNT))
#define _get_illins_handler(ins) (illins_handlers + ((ins - H__BASE) * H__Sz))

#ifndef NO_INLINE
#define is_ins_handled(ins)      _is_ins_handled(ins)
#define get_illins_handler(ins)  _get_illins_handler(ins)
#else
static int  is_ins_handled (int ins) {
    return _is_ins_handled(ins);
}

// assumes you've already called is_ins_handled() to check if it's in range
static int *get_illins_handler (int ins) {
    return _get_illins_handler(ins);
}
#endif

void set_illins_handler (int ins, int *handler) {
    int *h;
    h = get_illins_handler(ins);
    h[H_CB] = (int)handler;
}

#ifndef __c4__
// Make other compilers happy by dummying out these c4cc constructs
#define __c4_ins(a,b)
#define __c4_adj(a)
#define install_illins_handler(a,b)
#endif

__c4_ins("__c4_adj", C4__ADJUST);

static void illins_unhandled (int ins, int *sp, int *bp, int *returnpc, int *a) {
    printf("Unknown opcode: %d\n", ins);
    printf("  Info: sp = 0x%x, bp = 0x%x\n", *sp, *bp);
    printf("        pc = 0x%x,  a = %ld (0x%x)\n", *returnpc, *a, *a);
    __asm("ILEV");
}

void illins_handler(int ins, int *sp, int *bp, int *returnpc, int *a) {
    int *h; // technically not allocated on the stack yet
    // TODO: does it matter if not allocated? just use as is?
    //       might trigger memory checkers?
    __asm("ADJ -1"); // allocate h
    if (is_ins_handled(ins)) {
        h = get_illins_handler(ins);
        h = (int *)h[H_CB];
        // load handler to register A
        __asm("LEA &h");
        __asm("LI");
    } else {
        __asm("IMM &illins_unhandled");
    }
    __asm("ADJ 1"); // unallocate h
    __asm("JMPA");  // jump
}

static void illins__c4_adj2 (int ins, int *sp, int *bp, int *returnpc, int *a) {
    *sp = *sp - (*returnpc * sizeof(int)); // increase stack by argument
    *returnpc = *returnpc + sizeof(int);  // __c4_adjust to skip argument
    __asm("ILEV");
}

int main (int argc, char **argv) {
   int t;
   if (!(illins_handlers = malloc(t = (H__COUNT * (sizeof(int) * H__Sz))))) {
       printf("Unable to allocate %ld bytes for handlers\n", t);
       return -1;
   }
   memset(illins_handlers, 0, t);
   install_illins_handler(C4__ADJUST, (int *)&illins_handler);
   __c4_adj(-5);
   __c4_adj( 5);
   free(illins_handlers);
   return 0;
}
