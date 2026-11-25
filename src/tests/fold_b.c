// C4 Test: c4rlink folds c4lc's helpers, module B (see fold_a.c)

int fold_b (unsigned int u, short *s, long long l, double d) {
	unsigned short h;
	s[1] = 5000;
	h = (unsigned short)s[1];
	return (int)(u / 1000000000) + (int)(u % 7) + h / 1000 + (int)((l >> 38) * 6) + (int)(d * 12.0);
}
