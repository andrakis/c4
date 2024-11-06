// C4 Test: File read
//

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define int long long

int main (int argc, char **argv) {
	int bufsz, fd, i;
	char *buf;

	if ((fd = open("test_fread.c", 0)) < 0) {
		printf("Failed to open test_fread.c\n");
		return -1;
	}

	bufsz = 256 * 1024;
	if (!(buf = malloc(bufsz))) {
		printf("Failed to malloc %lld bytes\n", bufsz);
	} else {
		if ((i = read(fd, buf, bufsz)) <= 0) {
			printf("read() returned %lld\n", i);
		} else {
			printf("Read %lld bytes:\n%s\n", i, buf);
		}
	}

	close(fd);
	free(buf);

	return 0;
}
