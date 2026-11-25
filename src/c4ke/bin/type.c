// C4 Utility: type.c
// Like DOS' TYPE command, just prints the contents of a file.

#include "u0.c"

void show_help (char *argv0) {
	printf("%s: display a file\n", argv0);
	printf("%s [-np] [-P n] [--] [file1] [...fileN]\n", argv0);
	printf("    -n      Display line numbers before contents\n"
	       "    -p      Paging mode (pause after 25 lines)\n"
	       "    -P n    Set page mode line count to n (implies -p)\n"
	       "    --      End options parsing\n");
}

char *file_buf;
enum { FILE_BUF_SZ = 1024 };
int  opt_numbers;   // -n show line numbers
int  opt_page;      // -p paging mode
int  opt_page_size; // -P n page size

// ---- waiting for the reader ----------------------------------------
// Never fd 0: a read there blocks the HOST, which stops the whole VM
// and every other task with it -- src/tests/raycast.c says so at
// length, and this is the same pattern one size down. /dev/tty first
// because c4bb answers that with a raw per-keystroke descriptor;
// /dev/stdin is the portable fallback. schedule() between polls is
// what keeps the rest of the system running while we sit here.
enum { O_RDONLY = 0, O_NONBLOCK = 0x800 };
int   page_fd;
char *page_buf;
enum { PAGE_BUF_SZ = 64 };

void page_init () {
	page_fd = 0 - 1;
	if (!(page_buf = malloc(PAGE_BUF_SZ))) return;
	page_fd = open("/dev/tty", O_RDONLY | O_NONBLOCK);
	if (page_fd < 0) page_fd = open("/dev/stdin", O_RDONLY | O_NONBLOCK);
}

// Wait for Enter (or any keystroke on a raw console). Returns 0 when
// the input is gone, which turns paging OFF rather than stopping the
// file: someone who piped us no answer still wants to see the rest.
int page_wait () {
	int n;
	if (page_fd < 0) return 0;
	printf("-- More --\n");
	while (1) {
		n = read(page_fd, page_buf, PAGE_BUF_SZ);
		if (n == 0) { close(page_fd); page_fd = 0 - 1; return 0; }
		if (n > 0) return 1;
		schedule();   // -1 is "nothing yet": let everyone else run
	}
}

void type_file (char *argv0, char *file) {
	int   fd, lineno, bytes, bytes_remain, shown;
	char *pos_start, *pos_end;

	// A negative fd is the failure; fd 0 is stdin and never a file we
	// just opened, but the old test rejected it and accepted -1.
	if ((fd = open(file, 0)) < 0) {
		printf("%s: failed to open '%s'\n", argv0, file);
		return;
	}

	lineno = 1;
	shown  = 0;
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
				if (opt_page) {
					++shown;
					if (shown >= opt_page_size) {
						shown = 0;
						if (!page_wait()) opt_page = 0;
					}
				}
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
	if (bytes < 0) printf("%s: unable to read '%s'\n", argv0, file);

	close(fd);
}

int main (int argc, char **argv) {
	char *argv0, *arg;
	char **files;
	int   endopts, endopt, nfiles, i;

	argv0 = *argv;
	// Globals start at 0, and 0 is STDIN -- so this has to be -1 before
	// anything can test it, or a run without paging closes stdin.
	page_fd = 0 - 1;

	if (!(file_buf = malloc(FILE_BUF_SZ))) {
		printf("%s: failed to allocate %ld bytes for file buffer\n", argv0, FILE_BUF_SZ);
		return 2;
	}

	--argc; ++argv; // skip invocation
	if (!argc) {
		show_help(argv0);
		return 1;
	}

	if (!(files = (char **)malloc(argc * sizeof(char *)))) {
		printf("%s: failed to allocate the file list\n", argv0);
		return 2;
	}
	nfiles = 0;

	// Set defaults
	opt_numbers   = 0;
	opt_page      = 0;
	opt_page_size = 25;

	// Parse the WHOLE command line before typing anything. The old
	// version typed each file as it reached it, so an option written
	// after a filename arrived too late to affect it.
	endopts = 0;
	while (argc) {
		arg = *argv;
		if (!endopts && *arg == '-') {
			++arg; // Move forward
			if (!strcmp(arg, "-help") || !strcmp(arg, "-h")) {
				show_help(argv0);
				return 1;
			}
			// A bare "--" ends option parsing for good. endopt below
			// only ends the current cluster, which is why the two are
			// separate -- and why endopts used to never get set.
			if (!strcmp(arg, "-")) { endopts = 1; --argc; ++argv; continue; }
			endopt = 0;
			while (!endopt && *arg) {
				if      (*arg == '-') endopt = 1;
				else if (*arg == 'n') opt_numbers = 1;
				else if (*arg == 'p') opt_page = 1;
				else if (*arg == 'P') {
					// -P takes the NEXT word as the page size, and
					// asking to size the page means wanting one.
					if (argc < 2) {
						printf("%s: -P needs a line count\n", argv0);
						return 1;
					}
					--argc; ++argv;
					if (atoi_check(*argv, &opt_page_size) != ATOI_OK || opt_page_size < 1) {
						printf("%s: -P wants a positive line count, got '%s'\n", argv0, *argv);
						return 1;
					}
					opt_page = 1;
				}
				else { printf("%s: unrecognised option -%s\n", argv0, arg); return 1; }
				++arg;
			}
		} else {         // Not an option, must be a file
			files[nfiles] = *argv;
			++nfiles;
		}
		--argc; ++argv;
	}

	if (opt_page) page_init();

	i = 0;
	while (i < nfiles) { type_file(argv0, files[i]); ++i; }

	if (page_fd >= 0) close(page_fd);
	free(files);
	free(file_buf);

	return 0;
}
