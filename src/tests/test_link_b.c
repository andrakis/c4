// C4 Test: linking, module B (library)
// See test_link_a.c for usage and expected output.
// Exercises: cross-module calls, data strings in a non-first module,
// constructors and destructors in a non-first module, static privacy.

int b_state;

static int local_double (int x) { return x * 2; }

void __attribute__((constructor)) b_constructor () {
	printf("b_constructor\n");
	b_state = 7;
}

void __attribute__((destructor)) b_destructor () {
	printf("b_destructor\n");
}

int b_add (int a, int b) {
	// b_state proves the constructor ran; local_double is B's own static.
	return local_double(a + b) - b_state;
}

char *b_message () {
	return "hello from module b";
}
