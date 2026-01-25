///
// c4.h - Manipulation of C4, including the reference opcodes and functions
//        to call function pointers.
//
// int *get_caller_address() : returns the calling functions address.
// These functions require calling c4_invoke_setup() prior to use.
// int  c4_invoke0(int *ptr) : calls a pointer as a C4 function. Function takes no parameters.
// int  c4_invoke1(int *ptr, int a) : calls a pointer as a C4 function. Function takes 1 parameter.
// int  c4_invoke2(int *ptr, int a, int b) : calls a pointer as a C4 function. Function takes 2 parameters.
///

#ifndef __C4_H
#define __C4_H

#include <stdio.h>

// C4 Opcodes
enum { LEA ,IMM ,JMP ,JSR ,BZ  ,BNZ ,ENT ,ADJ ,LEV ,LI  ,LC  ,SI  ,SC  ,PSH ,
       OR  ,XOR ,AND ,EQ  ,NE  ,LT  ,GT  ,LE  ,GE  ,SHL ,SHR ,ADD ,SUB ,MUL ,DIV ,MOD ,
       OPEN,READ,CLOS,PRTF,MALC,FREE,MSET,MCMP,EXIT };

char *c4_opcodes;
void setup_opcodes () {
	c4_opcodes =
		"LEA ,IMM ,JMP ,JSR ,BZ  ,BNZ ,ENT ,ADJ ,LEV ,LI  ,LC  ,SI  ,SC  ,PSH ,"
		"OR  ,XOR ,AND ,EQ  ,NE  ,LT  ,GT  ,LE  ,GE  ,SHL ,SHR ,ADD ,SUB ,MUL ,DIV ,MOD ,"
		"OPEN,READ,CLOS,PRTF,MALC,FREE,MSET,MCMP,EXIT,";
}

#define C4_OPCODE_STRING(opcode)  &c4_opcodes[opcode * 5]

enum { JB_MAX_SEARCH = 512 };

// This function uses local references to find the return PC on the stack.
// It then searches the code backwards to find the ENT opcode.
int *get_caller_address () {
	int *addr, *next, i;

	// Return pc is stored above local variables
	addr = (int *)(*(&addr + 2));

	// Find ENT x
	i = 0;
	next = addr;
	while (++i <= JB_MAX_SEARCH) {
		--next;
		if (*addr == ENT) { // Possibly found
			// Ensure it wasn't an argument to some other opcode
			if (*next > ADJ)
				return addr;
		}
		addr = next;
	}

	printf("c4/get_calling_address: couldn't find entry\n");
	return 0;
}

// invoke stub
int *__c4_invoke_stub_addr;
int  __c4_invoke_stub () {
	// First call, get our address then overwrite ourselves with the JMP opcode.
	if (!(__c4_invoke_stub_addr = get_caller_address())) {
		printf("c4_invoke: failed to get our function address\n");
		exit(99);
	}

	// Advance the pointer so that the target is already correct.
	*__c4_invoke_stub_addr++ = JMP;
	// Doesn't matter what we return here
	return 0;
}

static int __c4_invoke_setup_complete;
// Invoke setup. Must be called prior to c4_invokeX being called.
void c4_invoke_setup () {
	if (!__c4_invoke_setup_complete) {
		// Invoke the stub now to change it to a JMP x opcode
		__c4_invoke_stub();
		__c4_invoke_setup_complete = 1;
	}
}

C4R_CONSTRUCTOR(_c4_h_setup, _, __) {
	setup_opcodes();
	c4_invoke_setup();
}

// c4_invokeX methods

int c4_invoke0 (int *ptr) {
	*__c4_invoke_stub_addr = (int)ptr;
	return __c4_invoke_stub();
}

int c4_invoke1 (int *ptr, int arg) {
	*__c4_invoke_stub_addr = (int)ptr;
	return __c4_invoke_stub(arg);
}

int c4_invoke2 (int *ptr, int arg1, int arg2) {
	*__c4_invoke_stub_addr = (int)ptr;
	return __c4_invoke_stub(arg1, arg2);
}

#endif // #ifndef __C4_H
