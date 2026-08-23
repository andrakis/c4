// c4th: the image, the two stacks, and the allocation pointer.
//
// One malloc holds the dictionary, the name pool and every compiled body.
// Addresses inside it are REAL machine addresses, not offsets -- that is
// what lets @ and ! map onto the VM's LI and SI, which is the whole reason
// for building a Forth on this machine (docs/c4th-design.md 1). The price
// is that a saved image is unrelocatable, and paying it is the job the
// metacompiler earns its place doing at B5.
//
// The data and return stacks are separate allocations. Forth wants two
// stacks and C4 has one; keeping them out of the VM's stack entirely is
// what makes the threaded core simple. The native backend at B5 is where
// that question gets interesting again.

// HERE is a BYTE address, because Forth's address unit is a byte: ALLOT
// reserves address units, C, lays down one, and the suite checks that two
// consecutive C, land one apart. Cell-granular allotment fails that, and
// it fails it quietly -- everything still runs, the addresses are just
// wrong. Cell writes align first; nothing else has to think about it.
int  *th_mem;       // base of the image
char *th_hp;        // next free byte
char *th_hlimit;    // one past the end
int *th_dstack;     // data stack base
int *th_sp;         // one past top of data stack
int *th_dstop;      // one past the end of the data stack
int *th_rstack;     // return stack base
int *th_rp;         // one past top of return stack
int *th_rstop;
int  th_err;        // set non-zero to abort the inner loop
int  th_quit;       // set by BYE: unwind everything and leave
int  th_base;       // numeric base, shared by the parser and by .

// Stack limits are checked on every push and pop. That costs a compare in
// the hottest code in the system, and it is worth it: an unchecked Forth
// answers a stack error with a wild store, and B3's job is to run a
// standards suite that deliberately provokes exactly that.
void th_push (int x) {
	if (th_sp >= th_dstop) { printf("c4th: data stack overflow\n"); th_err = 1; return; }
	*th_sp = x;
	th_sp = th_sp + 1;
}

int th_pop () {
	if (th_sp <= th_dstack) { printf("c4th: data stack underflow\n"); th_err = 1; return 0; }
	th_sp = th_sp - 1;
	return *th_sp;
}

void th_rpush (int x) {
	if (th_rp >= th_rstop) { printf("c4th: return stack overflow\n"); th_err = 1; return; }
	*th_rp = x;
	th_rp = th_rp + 1;
}

int th_rpop () {
	if (th_rp <= th_rstack) { printf("c4th: return stack underflow\n"); th_err = 1; return 0; }
	th_rp = th_rp - 1;
	return *th_rp;
}

// Round HERE up to a cell boundary and return it as a cell pointer.
int *th_align_here () {
	int a;

	a = (int)th_hp;
	a = (a + sizeof(int) - 1) / sizeof(int) * sizeof(int);
	th_hp = (char *)a;
	return (int *)th_hp;
}

void th_comma (int x) {
	int *p;

	p = th_align_here();
	if (th_hp + sizeof(int) > th_hlimit) { printf("c4th: image full\n"); th_err = 1; return; }
	*p = x;
	th_hp = th_hp + sizeof(int);
}

// ALLOT: exactly n address units, no rounding. A negative n releases.
char *th_alloc_bytes (int n) {
	char *r;

	if (th_hp + n > th_hlimit) { printf("c4th: image full\n"); th_err = 1; return 0; }
	r = th_hp;
	th_hp = th_hp + n;
	return r;
}

int th_mem_init (int image_cells, int dcells, int rcells) {
	th_err = 0;
	if (!(th_mem = malloc(image_cells * sizeof(int)))) return 0;
	th_hp     = (char *)th_mem;
	th_hlimit = (char *)(th_mem + image_cells);
	if (!(th_dstack = malloc(dcells * sizeof(int)))) return 0;
	th_sp    = th_dstack;
	th_dstop = th_dstack + dcells;
	if (!(th_rstack = malloc(rcells * sizeof(int)))) return 0;
	th_rp    = th_rstack;
	th_rstop = th_rstack + rcells;
	return 1;
}
