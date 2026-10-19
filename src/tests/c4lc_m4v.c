// c4lc_m4v.c -- M4: every access to a volatile object survives -O and
// -minline, exactly once and in order.
//
// The test (test-c4lc) counts, in c4rdump's patch listing, the DATA
// references to `vcount` and to `ncount`. The two are used identically;
// only vcount is volatile. Seven accesses plus one &vcount must stay
// eight references however the image is optimised (drop() never reads
// its parameter, peek() reads it twice) -- and ncount, five references
// in a plain build, is the control: under -minline peek(ncount) is
// spliced with the argument's TEXT and reads it twice, making six. If
// the control ever stops changing, the inliner stopped doing what this
// test is here to catch, and the test says so.

volatile int vcount;
int ncount;
volatile int *vp;

int peek(int x) { return x + x; }
int drop(int x) { return 7; }

int main() {
	int i, s, t;
	volatile int local;
	vp = &vcount;
	vcount = 0;
	ncount = 0;
	for (i = 0; i < 10; i++) {
		vcount = vcount + 1;
		ncount = ncount + 1;
	}
	s = peek(vcount);
	t = peek(ncount);
	s = s + drop(vcount);
	s = s + peek(*vp);
	local = 5;
	local = local + vcount;
	printf("%d %d %d %d %d\n", vcount, ncount, s, t, local);
	return 0;
}
