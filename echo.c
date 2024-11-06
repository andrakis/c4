// C4 test: echo
//
//

#include <stdio.h>

int main (int argc, char **argv) {
	int first;
	// TODO: flags
	--argc; ++argv;
	first = 1;
	while(argc) {
		if (!first) printf(" ");
		else        first = 0;
		printf("%s", *argv);
        --argc; ++argv;
	}
    printf("\n");
	return 0;
}
