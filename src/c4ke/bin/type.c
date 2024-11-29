// C4 Utility: type.c
// Like DOS' TYPE command, just prints the contents of a file.

#include "u0.c"

void show_help (char *argv0) {
	printf("%s: display a file\n");
	printf("%s [-npP n] [--] [file1] [...fileN]\n", argv0);
	printf("    -n      Display line numbers before contents\n"
	       "    -p      Paging mode (pause after 25 lines)\n"
	       "    -P n    Set page mode line count to n\n"
	       "    --      End options parsing\n");
}

char *file_buf;
enum { FILE_BUF_SZ = 1024 };
int  opt_numbers;   // -n show line numbers
int  opt_page;      // -p paging mode
int  opt_page_size; // -P n page size
void type_file (char *argv0, char *file) {
	int   fd, lineno, bytes, bytes_remain;
	char *pos_start, *pos_end;

	if (!(fd = open(file, 0))) {
		printf("%s: failed to open '%s'\n", argv0, file);
		return;
	}

	lineno = 1;
	memset(file_buf, 0, FILE_BUF_SZ);
	while((bytes = read(fd, file_buf, FILE_BUF_SZ)) > 0) {
		bytes_remain = bytes;
		// Print line by line
		pos_start = pos_end = file_buf;
		while (*pos_end && bytes_remain > 0) {
			if (*pos_end == '\n') {
				if (opt_numbers) printf("%ld: ", lineno);
				printf("%.*s\n", pos_end - pos_start, pos_start);
				bytes_remain = bytes_remain - (pos_end - pos_start);
				pos_start = pos_end + 1;
				++lineno;
			}
			++pos_end;
		}
		// Print remaining content
		if (bytes_remain) {
			if (opt_numbers) printf("%ld: ", lineno);
			printf("%.*s\n", pos_end - pos_start, pos_start);
		}
		// clear for next read
		memset(file_buf, 0, FILE_BUF_SZ);
	}
	if (bytes < 0) printf("%s: unable to open '%s'\n", argv0, file);

	close(fd);
}

int main (int argc, char **argv) {
	char *argv0, *arg;
	int   endopts, endopt;

	if (!(file_buf = malloc(FILE_BUF_SZ))) {
		printf("%s: failed to allocate %ld bytes for file buffer\n", argv0, FILE_BUF_SZ);
		return 2;
	}

	argv0 = *argv;
	--argc; ++argv; // skip invocation
	if (!argc) {
		show_help(argv0);
		return 1;
	}

	// Set defaults
	opt_numbers   = 0;
	opt_page      = 0;
	opt_page_size = 25;

	// Parse command line
	endopts = 0;
	while (argc) {
		arg = *argv;
		if (!endopts && *arg == '-') {
			++arg; // Move forward
			if (!strcmp(arg, "-help") || !strcmp(arg, "-h")) {
				show_help(argv0);
				return 1;
			}
			endopt = 0;
			while (!endopt && *arg) {
				if      (*arg == '-') endopt = 1;
				else if (*arg == 'n') opt_numbers = 1;
				else if (*arg == 'p') opt_page = 1;
				else if (*arg == 'P') { printf("%s: error, -P not implemented\n", argv0); return 1; }
				else { printf("%s: unrecognised option -%s\n", argv0, arg); return 1; }
				++arg;
			}
		} else {         // Not an option, must be a file
			type_file(argv0, *argv);
		}
		--argc; ++argv;
	}

	free(file_buf);

	return 0;
}
