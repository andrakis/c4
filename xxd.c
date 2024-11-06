// Very simple implementation of the xxd utility
//

#include "c4.h"

int is_printable (char c) {
	return c >= ' ' && c <= '~';
}

int dump_data (int fd) {
	int width, bytes, remain, i, off;
	char *d;
	char *buf;

	width = 16; // sizeof(int) * 2;
	if (!(buf = malloc(sizeof(char) * width))) {
		return 1;
	}
	off = 0;

	while(1) {
		bytes = read(fd, buf, width);
		if (bytes < 0) {
			free(buf);
			return 2; // read error
		} else if (bytes == 0) {
			free(buf); // end of file
			return 0;
		}

		// Print address
		// TODO: align properly
		printf("%08x: ", off);
		// Print hex repre
		remain = bytes;
		d = buf; i = 0; while(i++ < width && remain--) {
			// BUG: *d is getting larger than a char back?
			//      AND it with 0xFF to work around.
			printf("%02x", *d & 0xFF);
			if (i % 2 == 0) printf(" ");
			++d;
		}
		// print empty remainder spacing
		while(i <= width) { printf("  "); if (i % 2 == 0) printf(" "); ++i; }
		// Separator
		printf("  ");
		// Print ASCII repre
		remain = bytes;
		d = buf; i = 0; while(i++ < width && remain--) {
			if (is_printable(*d)) printf("%c", *d);
			else printf(".");
			++d;
		}
		printf("\n");
		off = off + width;
	}
}

static void show_help (char *spec) {
	printf("%s: make a hexdump of a file\n"
	       "%s: file1 [...fileN]\n", spec, spec);
}

static int my_strcmp (char *s1, char *s2) { while(*s1 && (*s1 == *s2)) { ++s1; ++s2; } return *s1 - *s2; }

int main (int argc, char **argv) {
	char *spec;
	int   fd, i;

	spec = *argv;

	// Skip argv[0]
	--argc; ++argv;
	if (!argc) {
		show_help(spec);
		return 1;
	}

	// Parse options and dump info about requested files
	while(argc) {
		if (!my_strcmp("--help", *argv)) {
			show_help(spec);
			return 1;
		} else {
			if ((fd = open(*argv, 0)) < 0) {
				printf("%s: could not open(%s)\n", spec, *argv);
			} else {
				if ((i = dump_data(fd))) {
					printf("%s: error %d, aborting\n", spec, i);
					return i;
				}
				close(fd);
			}
		}
		--argc; ++argv;
	}
	return 0;
}
