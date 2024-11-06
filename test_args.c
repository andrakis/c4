#include <stdio.h>
#include <stdlib.h>

#define int long long

int main (int argc, char **argv) {
	int i;
	i = 0;
	printf("Arg count: %lld\n", argc);
	while (argc) {
		printf("At %lld: %s\n", i, *argv);
		--argc;
		++argv;
		++i;
	}

	return 2;
}
