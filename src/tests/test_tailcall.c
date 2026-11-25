// C4 Test: zero-arg tail calls, transformable by c4opt's tail pass.
// Unoptimized, a million mutual recursions overflow the VM stack; the
// optimizer's ADJ (m-k); JMP f+2 rewrite reuses the caller's frame.
// even() has two locals and odd() none, so the rewrite must adjust sp
// across differing frame sizes in both directions.
int counter, limit;

int odd ();

int even () {
	int a, b;
	a = counter;
	b = a + 1;
	if (a >= limit) return 0;
	counter = b;
	return odd();
}

int odd () {
	if (counter >= limit) return 1;
	counter = counter + 1;
	return even();
}

int main () {
	counter = 0;
	limit = 1000000;
	printf("parity %d counter %d\n", even(), counter);
	return 0;
}
