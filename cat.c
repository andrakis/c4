// C4 Test: File read
//

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define int long long

int main (int argc, char **argv) {
	int bufsz, fd, i;
	char *buf, *spec;

	bufsz = 256 * 1024;
	if (!(buf = malloc(bufsz))) {
		printf("Failed to malloc %lld bytes\n", bufsz);
		return -1;
	}

	// Skip invocation
	--argc; ++argv;
	while (argc--) {
		if ((fd = open(*argv, 0)) < 0) {
			printf("Failed to open %s\n", *argv);
			return -2;
		}

		// TODO: read whole file
		if ((i = read(fd, buf, bufsz)) <= 0) {
			printf("read() returned %lld\n", i);
		} else {
			printf("%.*s", i, buf);
		}

		close(fd);
	}

	free(buf);
	return 0;
}

