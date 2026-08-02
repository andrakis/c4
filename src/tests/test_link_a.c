// C4 Test: linking, module A (main)
// Compile separately from test_link_b.c, then link:
//   ./c4cc -o tla.c4o src/tests/test_link_a.c
//   ./c4cc -o tlb.c4o src/tests/test_link_b.c
//   ./c4rlink tla.c4o tlb.c4o -o tl.c4r
//   ./c4m load-c4r.c -- tl.c4r
// Expected output:
//   b_constructor
//   local: 8
//   b_add(3, 4) = 7
//   b_message: hello from module b
//   b_destructor

int b_add (int a, int b);
char *b_message ();

static int local_double (int x) { return x + x; }

int main (int argc, char **argv) {
	printf("local: %d\n", local_double(4));
	printf("b_add(3, 4) = %d\n", b_add(3, 4));
	printf("b_message: %s\n", b_message());
	return 0;
}
