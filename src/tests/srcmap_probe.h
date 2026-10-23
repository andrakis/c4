// srcmap_probe.h -- test-srcmap's header: code that comes from here
// maps here, not to the line that included it

#define TWICE(x) \
	((x) + (x))

// inlined under -minline: its code then has the caller's line
static int square(int n)
{
	return n * n;
}

// recursive, so never inlined: its rows are this file's
int fact(int n)
{
	if (n < 2)
		return 1;
	return n * fact(n - 1);
}
