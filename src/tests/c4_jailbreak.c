// C4 Test: Jailbreak
//
// This test serves to outline how one may execute arbitrary function pointers.
// C4 doesn't normally let you get the address of a function. However, the
// return PC is saved onto the stack. If one were to look for this value, and
// find the ENT instruction, they would have the function address.
// We can then overwrite the code at that function to JMP to whatever code we
// desire.
//
// Uses:
//  - Obtain a function pointer by calling get_calling_address(), from the
//    function you need the address of.
//  - Callable function pointers in C4 without requiring C4M features.
//  - While running C4M under C4, allow execution of code directly in C4 for
//    a speed boost. Code must not use any extended C4M keywords or features,
//    ie be "pure c4" code.
//  - Self-modifying code under plain C4.
//  - Implementing features C4 doesn't natively support via custom assembly.
//
//  - C4M exposes __c4_invoke(function_pointer) which allows calling compiled
//    code from C4 instead of under the C4M interpreter.
//    - This is only useful when running C4M under C4, but provides a speed
//      boost to code that doesn't require the advanced features of C4M, such as:
//      * C4KE's kernel_task_find() function. A large speedup is (EXPECTED|OBTAINED)
//        by calling this function via the invoke jailbreak.
//      * load-c4r's load_c4r() function, until the VFS takes over.

#include <stdio.h>
#include <stdlib.h>

// For editor warnings
#ifndef int
#define int long
#endif

// Opcodes as defined in an unmodified c4.c
enum { LEA ,IMM ,JMP ,JSR ,BZ  ,BNZ ,ENT ,ADJ ,LEV ,LI  ,LC  ,SI  ,SC  ,PSH ,
       OR  ,XOR ,AND ,EQ  ,NE  ,LT  ,GT  ,LE  ,GE  ,SHL ,SHR ,ADD ,SUB ,MUL ,DIV ,MOD ,
       OPEN,READ,CLOS,PRTF,MALC,FREE,MSET,MCMP,EXIT };

enum { MAX_SEARCH = 512 };

// This function uses local references to find the return PC on the stack.
// It then searches up the code stack to find the ENT opcode.
int *get_calling_address () {
	int *addr, *next, i;

	// Return pc is stored above local variables
	addr = (int *)(*(&addr + 2));

	// Find ENT x
	i = 0;
	next = addr;
	while (++i <= MAX_SEARCH) {
		--next;
		if (*addr == ENT) { // Possibly found
			// Ensure it wasn't an argument to some other opcode
			if (*next > ADJ)
				return addr;
		}
		addr = next;
	}

	printf("get_calling_address: couldn't find entry\n");
	return 0;
}

int *jailbreak_function_addr;
int jailbreak_function () {
	// First time this gets called, we want to self-modify our own code
	if (!(jailbreak_function_addr = get_calling_address())) {
		printf("jailbreak_function: address not found.\n");
		exit(-1);
	}

	printf("jailbreak_function setup\n");
	// Now lets modify our code to be a JMP
	*jailbreak_function_addr = JMP;
	// Now return
	return 0;
}

// Simply updates the JMP target before calling the jailbreak_function.
int jailbreak_invoker (int *addr) {
	*(jailbreak_function_addr + 1) = (int)addr; // Update JMP address
	return jailbreak_function();
}

int *redirected_function_addr;
// This is the function that gets called when we call jailbreak_function.
// We need to call it once to get the redirected_function_addr, but after
// that we can use jailbreak_invoker() with any known function address.
int redirected_function () {
	// If addr not known, grab it and return.
	if (!redirected_function_addr) {
		redirected_function_addr = get_calling_address();
		return 0;
	}

	// Otherwise, assume we're being called under jailbreak conditions
	printf("Jailbroken!\n");
	return 100;
}

int main (int argc, char **argv) {
	// Call jailbreak and redirected functions to get their addresses
	jailbreak_function();
	redirected_function();

	// Now attempt to redirect jailbreak_function to redirected_function
	printf("result of call: %ld\n", jailbreak_invoker(redirected_function_addr));
	return 0;
}
