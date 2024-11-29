#include <stdio.h>

// In test_extern_fun.c
int some_external_function (int n);

int main (int argc, char **argv) {
	printf("%d\n", some_external_function(10));
	return 0;
}
