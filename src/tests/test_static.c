/*
 * C4CC Test: static keyword
 *
 * ./c4plus c4cc.c asm-c4r.c -- -c test_static.c
 */

#include <stdio.h>

static int private_variable;
       int public_variable;

static int private_function () { return 0; }
int        public_function  () { return 1; }

int main (int argc, char **argv) {
	private_variable = 0;
	public_variable  = 1;
	private_function();
	public_function();

	return 0;
}
