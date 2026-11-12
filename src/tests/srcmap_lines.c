// srcmap_lines.c -- test-srcmap: the rows c4lc -g writes for code that
// is no statement's own (src/c4sp/tests/expected/srcmap-lines.txt). A
// function's row is its name's line, though its ( is on the next; an
// initialised local's code is its declaration's; and a switch's
// dispatch, after its cases, is the switch's line, not the last case's.

int kr
   (int a)
{
	return a + 1;
}

int pick(int a)
{
	int n = 3;
	char w[] = "hi";

	switch (a) {
	case 1:
		n = 4;
		break;
	default:
		n = w[1];
	}
	return n;
}

int main()
{
	printf("%d %d\n", kr(1), pick(2));
	return 0;
}
