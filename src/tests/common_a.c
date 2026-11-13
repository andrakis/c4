// C4 Test: a global several units define is one object
// (src/c4sp/tests/common-link.sh; c4rlink.c, common_plan)
// Unit A, with main; common_b.c and common_c.c are the others, and
// common_d.c, which initializes limit too, must not link with them.
// Expected output: "common 111 4 42 5 7 7111"
// Before c4rlink merged them it printed "common 11 0 0 5 7 5011": each
// unit had its own counter, table and limit, and common_c.c's extern
// hidden found A's static.

int counter;            // tentative here and in common_b.c: one object
int table[4];           // 4 here, 8 in common_b.c: the biggest is kept
int limit;              // tentative here, initialized in common_b.c
static int hidden = 5;  // A's own: common_b.c's global of the name is not it
int *tend = &table[4];  // just past the end of A's table: moves with it

int bump_b (void);
int sum_b (void);
int hidden_b (void);
int read_c (void);

int bump_a (void) { counter = counter + 10; return counter; }

int main () {
	int *p;
	counter = 1;
	bump_a();
	bump_b();
	for (p = table; p < tend; p++) *p = 1;
	printf("common %d %d %d %d %d %d\n", counter, sum_b(), limit, hidden, hidden_b(), read_c());
	return 0;
}
