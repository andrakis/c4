// C4 tests
#include <stdio.h>

int add (int a, int b) { return a + b; }

int main (int argc, char **argv) {
	char *_charptr, _char;
	int  *_intptr, _int;

	_charptr = "Hello, world";
	++_charptr;
	_char = *_charptr;

	if(1) _int = add(1, 2);
	else  _int = add(2, 1);
	_int = _int ? add(_int, _int) : _int;

	printf("tests succeeded.\n");

	return 0;
}
