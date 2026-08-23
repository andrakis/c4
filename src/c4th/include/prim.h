// c4th: primitives.
//
// B1 carries the dozen the selftest needs. Every primitive takes its own xt,
// which most ignore; the uniform signature is what lets one indirect call
// site drive all of them.
//
// Control-flow primitives read their operand from the cell after them in
// the body and step th_ip past it, exactly as a threaded Forth does.

void th_p_lit (int *w) {              // LIT: next cell is a literal
	th_push(*th_ip);
	th_ip = th_ip + 1;
}

void th_p_dup (int *w) {
	int x;
	x = th_pop(); th_push(x); th_push(x);
}

void th_p_drop (int *w) { th_pop(); }

void th_p_swap (int *w) {
	int a, b;
	b = th_pop(); a = th_pop(); th_push(b); th_push(a);
}

void th_p_over (int *w) {
	int a, b;
	b = th_pop(); a = th_pop(); th_push(a); th_push(b); th_push(a);
}

void th_p_mul (int *w) {
	int a, b;
	b = th_pop(); a = th_pop(); th_push(a * b);
}

void th_p_sub (int *w) {
	int a, b;
	b = th_pop(); a = th_pop(); th_push(a - b);
}

void th_p_add (int *w) {
	int a, b;
	b = th_pop(); a = th_pop(); th_push(a + b);
}

void th_p_branch (int *w) {           // BRANCH: next cell is the target
	th_ip = (int *)*th_ip;
}

void th_p_zbranch (int *w) {          // 0BRANCH: branch when TOS is zero
	int f;
	f = th_pop();
	if (f) th_ip = th_ip + 1;
	else   th_ip = (int *)*th_ip;
}

void th_p_dot (int *w) {              // . -- print TOS and a space
	printf("%d ", th_pop());
}

void th_p_cr (int *w) { printf("\n"); }

void th_p_exit (int *w) {             // EXIT: return to the caller
	th_ip = (int *)th_rpop();
}

void th_p_bye (int *w) {              // stop the inner loop
	th_ip = 0;
}

// Register the B1 set. Each name gets a header whose code field is the
// primitive's address; &name is required because c4cc mis-emits a bare
// function name used as a value.
void th_prims_init () {
	th_create("LIT",     3, FL_COMPONLY, (int)&th_p_lit);
	th_create("DUP",     3, 0,           (int)&th_p_dup);
	th_create("DROP",    4, 0,           (int)&th_p_drop);
	th_create("SWAP",    4, 0,           (int)&th_p_swap);
	th_create("OVER",    4, 0,           (int)&th_p_over);
	th_create("*",       1, 0,           (int)&th_p_mul);
	th_create("-",       1, 0,           (int)&th_p_sub);
	th_create("+",       1, 0,           (int)&th_p_add);
	th_create("BRANCH",  6, FL_COMPONLY, (int)&th_p_branch);
	th_create("0BRANCH", 7, FL_COMPONLY, (int)&th_p_zbranch);
	th_create(".",       1, 0,           (int)&th_p_dot);
	th_create("CR",      2, 0,           (int)&th_p_cr);
	th_create("EXIT",    4, FL_COMPONLY, (int)&th_p_exit);
	th_create("BYE",     3, 0,           (int)&th_p_bye);
}
