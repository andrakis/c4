// mpg_freed.c -- write through a pointer after freeing it.
//
// Included at M1 because it costs nothing: c4_free drops the region, so
// the very next access has nothing owning it. The design lists this
// under M3 with the four opcodes; it falls out of the allocator hook.
int main () {
	int *p;

	if (!(p = malloc(sizeof(int) * 4))) { printf("mpg_freed: no memory\n"); return 1; }
	p[0] = 1;
	free(p);
	printf("mpg_freed: freed, and about to write through it anyway\n");
	p[0] = 2;                       // <-- use after free
	printf("mpg_freed: STILL RUNNING -- the guard did not catch it\n");
	return 0;
}
