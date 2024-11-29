#include <stdio.h>

int test (int a, int b, int c, int d) {
	printf("Numbers: %d %d %d %d", a, b, c, d);
	return 0;
}

int main (int argc, char **argv) {
	return test(1, 2, 3, 4);
}
