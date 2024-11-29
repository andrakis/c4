// C4 Test: custom opcodes
// Install a trap handler that captures our custom opcodes and executes them.
//
// Invocation: ./c4 c4m.c test_customop.c
//           : ./c4 c4m.c -d test_customop.c
// Runs under c4: no
// Runs under c4 via c4m: yes
//
// Traps are reentrant - a trap could occur in the trap handler, if the
// handler itself uses an illegal instruction (one implemented via a trap
// handler.)
// Careful use of this feature can allow custom instructions to be implemented
// and used within a trap handler.
//
// Trap handlers may use the return keyword or allow execution to fall out of
// a function without issue.
//
// Trap handlers do not need to increment returnpc, unless they take arguments.
// When the trap handler is entered, returnpc is already the instruction after
// the trapping instruction.

// Stuff that makes GCC happy, but isn't required for c4(m)
// Mainly so syntax checking works in editors
#ifndef __c4__
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define int long long
#pragma GCC diagnostic ignored "-Wformat"
#define __c4_tlev()
#define __c4_trap(x)
#define __c4_opcode(x)
#define __c4_jmp(x)
#define __c4_adjust(x)
#define install_trap_handler(x) 0
#endif /* __c4__ */

// At some stage this will be a symbol available under c4m
enum { TRAP_ILLOP };

// Our custom opcodes, avoiding ones used by c4ke
enum { OP_PRINTA = 64, OP_PRINTBP, OP_PRINTSP, OP_ADJ1 };

// Details for managing the opcode to function vector
enum { CO_BASE = 64, CO_MAX = 64 };
int *custom_opcodes;

// Handle a trap.
//
// This handler allows a number of custom opcodes. It jumps directly into
// the custom handler.
//
// @param trap      The trap signal, at present only legal value is:
//                  0 (TRAP_ILLOP)
// @param ins       The instruction code that caused the trap.
// @param a         Register A at time of trap. Can be updated.
//                  Value restored to register A when a TLEV occurs.
// @param bp        Register BP at time of trap. Restored by TLEV.
// @param sp        Register SP at time of trap. Restored by TLEV.
// @param returnpc  Register PC at time of trap. You get the idea.
void trap_handler (int trap, int ins, int *a, int *bp, int *sp, int *returnpc) {
	int *handler;
	printf("Trap handler: T%d  I%d(0x%X)\n", trap, ins, ins);
	printf("  SP=0x%X  BP=0x%X  ReturnPC=0x%X\n", sp, bp, returnpc);
	printf("  opcode handler address: 0x%X\n", (custom_opcodes + (ins - CO_BASE)));

	// No other traps supported
	if (trap != TRAP_ILLOP) {
		printf("Unexpected trap %d\n", trap);
		exit(-1);
	}

	handler = (int *)custom_opcodes[ins - CO_BASE];

	// Ensure within range
	if (ins >= CO_BASE && ins <= CO_BASE + CO_MAX && *handler) {
		// Adjust stack based on bp for handler we're about to jump into.
		// Reads x from ENT x instruction.
		__c4_adjust(1 + (*(handler - 1) * -1));
		// Jump to the handler, does not return
		__c4_jmp(handler);
	}

	// No way to recover from this
	printf("Custom opcode not found: %d\n", ins);
	exit(-2);
}

// Trap handlers can call other functions as normal.
int some_function (int x) {
	int y;
	y = x;
	x = x * x;
	y = x ^ y;
	return y;
}

void handler_op_printa (int trap, int ins, int *a, int *bp, int *sp, int *returnpc) {
	int something;
	printf("OP_PRINTA: A is %ld\n", a);
	printf("  some_function call: %d\n", some_function(ins));
}
void handler_op_printbp (int trap, int ins, int *a, int *bp, int *sp, int *returnpc) {
	int something, too;
	printf("OP_PRINTBP: BP is 0x%X\n", bp);
	printf("  some_function call: %d\n", some_function(ins));
	// Invoke nested trap
	__c4_opcode(OP_PRINTA);
	// leave bp in a register
	a = bp;
}
void handler_op_printsp (int trap, int ins, int *a, int *bp, int *sp, int *returnpc) {
	int much, wow, omg;
	printf("OP_PRINTSP: SP is 0x%X\n", sp);
	printf("  some_function call: %d\n", some_function(ins));
	// Invoke nested trap
	__c4_opcode(OP_PRINTBP);
	// Leave sp in register a
	a = sp;
}

void handler_op_adj1 (int trap, int ins, int *a, int *bp, int *sp, int *returnpc) {
	printf("adj is updating sp from 0x%X to 0x%X\n", sp, sp - 1);
	--sp;
	a = sp;
}

// Install a custom opcode handler.
//
// By convention, custom opcodes start at 128.
//
// The handler address recorded skips the ENT x instruction, but the main
// trap handler inspects this instruction to adjust the stack accordingly.
//
// If a custom opcode handler already exists, the program exits. This could
// likely signify an opcode getting trashed because the wrong value is being
// used.
//
// @param opcode    Opcode to install handler for
// @param handler   The handler, obtained using (int *)&function_name
// @return          0 on success, 1 on error (out of range).
int install_custom_opcode (int opcode, int *handler) {
	int addr;

	if (opcode >= CO_BASE && opcode <= CO_BASE + CO_MAX) {
		addr = opcode - CO_BASE;
		if (custom_opcodes[addr]) {
			// Signal an error since we may be clobbering other custom opcodes
			exit(-3);
		}
		// Skip the ENT x instruction
		custom_opcodes[addr] = (int)(handler + 2);
		return 0;
	}

	return 1;
}

int main (int argc, char **argv) {
	int t, old_trap;

	if (!(custom_opcodes = malloc((t = sizeof(int *) * CO_MAX)))) {
		printf("Unable to allocate %d bytes for custom opcode vector\n", t);
		return -1;
	}
	memset(custom_opcodes, 0, t);

	install_custom_opcode(OP_PRINTA, (int *)&handler_op_printa);
	install_custom_opcode(OP_PRINTBP, (int *)&handler_op_printbp);
	install_custom_opcode(OP_PRINTSP, (int *)&handler_op_printsp);
	install_custom_opcode(OP_ADJ1, (int *)&handler_op_adj1);

	// Install the main trap handler
	old_trap = install_trap_handler((int *)&trap_handler);
	printf("test_customop: installed our trap handler, previous = 0x%lx\n", old_trap);

	// Invoke the custom opcodes. A compiler might be able to implement these as
	// opcodes, but until that is implemented use this bruteforce opcode call.
	__c4_opcode(OP_PRINTA);
	__c4_opcode(OP_PRINTBP);
	__c4_opcode(OP_PRINTSP);
	__c4_opcode(OP_ADJ1);

	printf("test_customop: restoring old handler = 0x%lx\n", old_trap);
	install_trap_handler(old_trap);
	printf("Done\n");
	free(custom_opcodes);
	return 0;
}

