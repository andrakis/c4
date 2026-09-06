// mpg_underrun.c -- write one word BEFORE a malloc'd buffer.
//
// The other half of the same mistake, and the nastier one in practice:
// the word before an allocation is usually the allocator's own
// bookkeeping, so the damage surfaces as a corrupted heap somewhere
// entirely else, thousands of allocations later. docs/task-memory.md is
// that story at length.
//
// Expected: c4mpg halts here. Under plain c4m it "works", and then
// something unrelated falls over.
int main () {
	int *p;

	if (!(p = malloc(sizeof(int) * 4))) { printf("mpg_underrun: no memory\n"); return 1; }
	p[0] = 1;
	printf("mpg_underrun: the buffer itself is fine\n");
	p[0 - 1] = 99;                  // <-- one before the start
	printf("mpg_underrun: STILL RUNNING -- the guard did not catch it\n");
	return 0;
}
