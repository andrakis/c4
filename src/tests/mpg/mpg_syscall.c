// mpg_syscall.c -- read() a lot more than the buffer holds.
//
// THE ONE THIS TOOL EXISTS FOR. docs/dos-rung-fixes.md F12: vfsload
// parsed past the end of its manifest into uninitialised heap and
// reported a hundred lines of C source as filesystem entries. Latent
// for months, and it only showed when the heap beneath it happened to
// hold something legible.
//
// No per-instruction check could ever catch this, and that is the
// point: the overrun happens inside the HOST's read(), so not one guest
// LI or SI executes for it. Only checking [buf, buf+len) BEFORE the
// pointer is handed over can see it.
//
// Expected: c4mpg halts before the read. Under plain c4m the read
// succeeds and stamps on whatever was after the buffer.
int main () {
	char *buf;
	int   fd, n;

	if (!(buf = malloc(64))) { printf("mpg_syscall: no memory\n"); return 1; }
	if ((fd = open("src/tests/mpg/mpg_syscall.c", 0)) < 0) {
		printf("mpg_syscall: cannot open my own source\n");
		return 1;
	}
	printf("mpg_syscall: 64-byte buffer, about to ask for 65536\n");
	n = read(fd, buf, 65536);          // <-- 1024x the buffer
	printf("mpg_syscall: STILL RUNNING -- read %d bytes into 64\n", n);
	close(fd);
	return 0;
}
