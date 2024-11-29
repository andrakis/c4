// C4 Test: multiple functions with same name
// Keep the last one

int test (int a) { return a + 1; }
int test (int a) { return a - 1; }

int main (int argc, char **argv) {
	printf("Should be 3: %d\n", test(4));
	return 0;
}
