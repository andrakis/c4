// OBSOLETE C4 Test: trapping
// Install a trap handler and test it.
// Was part of a test before traps were truly implemented.
//
// Invocation: ./c4 c4m.c test_trap.c
// Runs under c4: no
// Runs under c4 via c4m: yes
//
// Trap handlers can modify registers before returning.
// Not modifying the returnpc argument results in the instruction being
// retried, which currently serves no purpose.
// In the future, instructions may be able to cause traps that resolve the
// error that resulted in the trap. This would require more trapping conditions
// to be implemented in c4m.
//
// The only trap available at present is: 0, TRAP_ILLOP
//
// Traps are reentrant - a trap could occur in the trap handler, if the
// handler itself uses an illegal instruction (one implemented via a trap
// handler.)
// Careful use of this feature can allow custom instructions to be implemented
// and used within a trap handler.

// Stuff that makes GCC happy, but isn't required for c4(m)
#include <stdio.h>
#define int long long
#pragma GCC diagnostic ignored "-Wformat"
#define __c4_tlev()
#define __c4_trap(x)
#define install_trap_handler(x)
// End

// Handle a trap.
// @param trap      The trap signal, at present only legal value is:
//                  0 (TRAP_ILLOP)
// @param ins       The instruction code that caused the trap
// @param a         Register A at time of trap. Can be updated.
//                  Value restored to register A when a TLEV occurs.
// @param bp        Register BP at time of trap. Same as above.
// @param sp        Register SP at time of trap. Same as above that.
// @param returnpc  Register PC at time of trap. You get the idea.
void trap_handler (int trap, int ins, int *a, int *bp, int *sp, int *returnpc) {
	printf("Trap handler: T%d  I%d(0x%X)\n", trap, ins, ins);
	printf("  SP=0x%X  BP=0x%X  ReturnPC=0x%X\n", sp, bp, returnpc);
	// advanced return pc, otherwise instruction is retried
	returnpc = returnpc + sizeof(int);
	// must use "trap leave" instruction
	__c4_tlev();
}

int main (int argc, char **argv) {
	install_trap_handler((int *)&trap_handler);
	// Cause a trap. Currently not implemented in c4m, apart from its opcode.
	// Falls into illegal instruction handler. Argument ignored.
	__c4_trap(0);
	printf("Done\n");
	return 0;
}
