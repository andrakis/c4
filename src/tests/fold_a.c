// C4 Test: c4rlink folds c4lc's helpers (src/c4sp/tests/fold-link.sh)
// Module A, with main; fold_b.c is module B. Each uses the same helpers
// -- unsigned division and remainder, 16-bit loads and stores, a logical
// shift, long long and double arithmetic -- so c4lc splices a copy of
// each into both, and the link keeps one.
// Expected output: "fold 2863 3 -1234 5000 268435200 1024 -72 63"

int fold_b (unsigned int u, short *s, long long l, double d);

static short sa[2];

int main () {
	unsigned int u;
	long long l;
	double d;
	int r;
	u = 0xFFFFF000;
	sa[0] = -1234; sa[1] = -1234;
	l = 1;
	l = l << 40;
	d = 2.25;
	r = fold_b(u, sa, l, d);
	printf("fold %d %d %d %d ", (int)(u / 1500000), (int)(u % 9), sa[0], (unsigned short)sa[1]);
	printf("%d %d %d %d\n", (int)(u >> (u & 1 ? 3 : 4)), (int)(l >> 30), (int)(d * -32.0), r);
	return 0;
}
