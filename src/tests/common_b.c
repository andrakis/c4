// C4 Test: unit B of common_a.c's link (src/c4sp/tests/common-link.sh)

int counter;
int table[8];
int limit = 42;
int hidden = 7;         // a global named as common_a.c's static

int bump_b (void) { counter = counter + 100; return counter; }

int sum_b (void) {
	int i, s;
	s = 0;
	for (i = 0; i < 8; i++) s = s + table[i];
	return s;
}

int hidden_b (void) { return hidden; }
