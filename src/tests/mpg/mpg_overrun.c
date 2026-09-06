// mpg_overrun.c -- write one word past the end of a malloc'd buffer.
//
// The commonest shape there is, and the one a tool that only knows
// mapped-from-unmapped never sees: the word past this buffer is inside
// the process heap and very probably inside somebody else's live
// allocation. c4mpg knows the LENGTH, because MALC recorded it, so it
// can say "8 bytes past the end of 'malloc'" instead of nothing at all.
//
// Expected: c4mpg halts here. Under plain c4m it "works".
int main () {
	int *p, i;

	if (!(p = malloc(sizeof(int) * 4))) { printf("mpg_overrun: no memory\n"); return 1; }
	i = 0;
	while (i < 4) { p[i] = i; ++i; }
	printf("mpg_overrun: four words written, all legal so far\n");
	p[4] = 99;                      // <-- one past the end
	printf("mpg_overrun: STILL RUNNING -- the guard did not catch it\n");
	return 0;
}
