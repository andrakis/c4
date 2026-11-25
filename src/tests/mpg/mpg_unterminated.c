// mpg_unterminated.c -- hand printf a string with no terminator.
//
// The other half of F12, and its actual root cause: "an unterminated
// buffer, in the one function that had not been read". A string is a
// range whose length nobody knows until they have already read it, so
// the host's formatter walks off the end of the allocation and prints
// whatever came next -- which is how a manifest parser came to report C
// source as filesystem entries.
//
// Expected: c4mpg halts. Under plain c4m this prints garbage, or does
// not, depending entirely on what the heap happened to hold.
int main () {
	char *buf;
	int   i;

	if (!(buf = malloc(8))) { printf("mpg_unterminated: no memory\n"); return 1; }
	i = 0;
	while (i < 8) { buf[i] = 65 + i; ++i; }   // eight letters, no NUL
	printf("mpg_unterminated: eight bytes, no terminator, about to print it\n");
	printf("mpg_unterminated: [%s]\n", buf);
	printf("mpg_unterminated: STILL RUNNING -- the guard did not catch it\n");
	return 0;
}
