int main () {
	int s, i, c0, c1;
	c0 = __c4_cycles();
	s = 0; i = 0;
	while (i < 100000) { s = s + i; i = i + 1; }
	c1 = __c4_cycles();
	printf("native  cycles: %d  (sum %d)\n", c1 - c0, s);
	return 0;
}
