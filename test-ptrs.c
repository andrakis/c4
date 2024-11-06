#include <stdio.h>

int main (int argc, char **argv) {
	int a, b, *x, *y;
	char c, d, *z;

//	a = 1;
//	b = a + 1;
//	x = &a;
//	y = &b;
//	*x = 4;
//	*y = *x + 1;
//	c = 2;
//	d = c + 1;
//	z = &c;
//	*z = 2;
	a = 1; b = 2; c = 3; d = 4;
	x = &a;
	y = &b;
	z = &c;
	*x++;
	++x;
	*z++;
	++z;

	return 0;
}
