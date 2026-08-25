// c4th: primitives.
//
// Every primitive takes its own xt, which most ignore; the uniform
// signature is what lets one indirect call site in th_run drive all of
// them. Control-flow primitives read their operand from the cell after
// them in the body and step th_ip past it themselves.
//
// Registration uses (int)&fn, never (int)fn: c4cc mis-emits a bare
// function name used as a value, and the & is what makes it emit the
// address.

// -- unsigned arithmetic on a signed cell ------------------------------
//
// C4 has no unsigned type, so a cell whose top bit is set reads as
// negative and every ordinary comparison and division is wrong for it.
// These do the unsigned thing anyway. They matter more than they look:
// U., U<, UM/MOD and the whole pictured-output path are defined on
// unsigned values, and the Forth-2012 suite checks all of them at the
// extremes where the top bit is set.

int th_uless (int a, int b) {
	if ((a < 0) == (b < 0)) return a < b;   // same half: signed order agrees
	return b < 0;                            // the one with the top bit set
}                                            // is the LARGER unsigned

// q = a / d and r = a % d, both unsigned. The shift-by-one dance is the
// standard way to do it without a wider type: halving a first makes it
// non-negative, and the correction loop puts back what the halving lost.
void th_udivmod (int a, int d, int *q, int *r) {
	int qq, rr, half, mask;

	if (d == 0) { *q = 0; *r = 0; return; }
	if (a >= 0 && d > 0) { *q = a / d; *r = a % d; return; }
	if (d < 0) {                             // divisor has the top bit set
		if (th_uless(a, d)) { *q = 0; *r = a; }
		else                { *q = 1; *r = a - d; }
		return;
	}
	// Half the top-bit mask, doubled -- written this way because a
	// literal would be wrong at 32 bits, and 1 << (bits-1) would shift
	// into the sign bit. c4th runs at both widths: c4sp32 builds the
	// 32-bit toolchain c4bb boots.
	half = 1;
	half = half << (sizeof(int) * 8 - 2);
	mask = half - 1 + half;
	qq = ((a >> 1) & mask) / d * 2;
	rr = a - qq * d;
	while (!th_uless(rr, d)) { rr = rr - d; qq = qq + 1; }
	*q = qq;
	*r = rr;
}

// RSHIFT is a LOGICAL shift in Forth-2012, but C's >> on a signed value
// is arithmetic, so -1 RSHIFT 1 would come back as -1 instead of MAX-INT.
// The suite builds MAX-INT and MIN-INT out of exactly that expression, so
// getting it wrong makes every comparison test at the extremes fail --
// which is how it was found.
int th_urshift (int a, int b) {
	int half, mask;

	if (b <= 0) return a;
	if (b >= sizeof(int) * 8) return 0;
	half = 1;
	half = half << (sizeof(int) * 8 - 2);
	mask = half - 1 + half;
	return ((a >> 1) & mask) >> (b - 1);
}

void th_print_unum (int v, int base) {
	char buf[72];
	int  i, q, r;

	if (base < 2) base = 10;
	i = 0;
	if (!v) { buf[i] = '0'; ++i; }
	while (v) {
		th_udivmod(v, base, &q, &r);
		if (r < 10) buf[i] = '0' + r; else buf[i] = 'A' + r - 10;
		++i;
		v = q;
	}
	while (i > 0) { --i; th_emit(buf[i]); }
}

// -- stack ------------------------------------------------------------
void th_p_dup (int *w)  { int x; x = th_pop(); th_push(x); th_push(x); }
void th_p_drop (int *w) { th_pop(); }
void th_p_swap (int *w) { int a,b; b=th_pop(); a=th_pop(); th_push(b); th_push(a); }
void th_p_over (int *w) { int a,b; b=th_pop(); a=th_pop(); th_push(a); th_push(b); th_push(a); }
void th_p_nip (int *w)  { int a,b; b=th_pop(); a=th_pop(); th_push(b); }
void th_p_tuck (int *w) { int a,b; b=th_pop(); a=th_pop(); th_push(b); th_push(a); th_push(b); }
void th_p_rot (int *w)  { int a,b,c; c=th_pop(); b=th_pop(); a=th_pop(); th_push(b); th_push(c); th_push(a); }
void th_p_nrot (int *w) { int a,b,c; c=th_pop(); b=th_pop(); a=th_pop(); th_push(c); th_push(a); th_push(b); }
void th_p_qdup (int *w) { int x; x = th_pop(); th_push(x); if (x) th_push(x); }
void th_p_depth (int *w){ th_push(th_sp - th_dstack); }
// PICK is CORE EXT rather than CORE, but the peephole's pattern matcher
// reads several instructions ahead and wants it. 0 PICK is DUP.
void th_p_pick (int *w) {
	int n;
	n = th_pop();
	if (n < 0 || th_sp - th_dstack <= n) { printf("c4th: PICK out of range\n"); th_err = 1; return; }
	th_push(th_sp[-1 - n]);
}
void th_p_2dup (int *w) { int a,b; b=th_pop(); a=th_pop(); th_push(a); th_push(b); th_push(a); th_push(b); }
void th_p_2drop (int *w){ th_pop(); th_pop(); }
void th_p_2swap (int *w){ int a,b,c,d; d=th_pop(); c=th_pop(); b=th_pop(); a=th_pop(); th_push(c); th_push(d); th_push(a); th_push(b); }
void th_p_2over (int *w){ int a,b,c,d; d=th_pop(); c=th_pop(); b=th_pop(); a=th_pop(); th_push(a); th_push(b); th_push(c); th_push(d); th_push(a); th_push(b); }

// -- return stack -----------------------------------------------------
void th_p_tor (int *w)  { th_rpush(th_pop()); }
void th_p_rfrom (int *w){ th_push(th_rpop()); }
void th_p_rfetch (int *w){ if (th_rp <= th_rstack) { printf("c4th: return stack underflow\n"); th_err = 1; return; } th_push(th_rp[-1]); }

// -- arithmetic -------------------------------------------------------
void th_p_add (int *w)  { int a,b; b=th_pop(); a=th_pop(); th_push(a+b); }
void th_p_sub (int *w)  { int a,b; b=th_pop(); a=th_pop(); th_push(a-b); }
void th_p_mul (int *w)  { int a,b; b=th_pop(); a=th_pop(); th_push(a*b); }
void th_p_div (int *w)  { int a,b; b=th_pop(); a=th_pop(); if (!b) { printf("c4th: division by zero\n"); th_err=1; return; } th_push(a/b); }
void th_p_mod (int *w)  { int a,b; b=th_pop(); a=th_pop(); if (!b) { printf("c4th: division by zero\n"); th_err=1; return; } th_push(a%b); }
void th_p_divmod (int *w){ int a,b; b=th_pop(); a=th_pop(); if (!b) { printf("c4th: division by zero\n"); th_err=1; return; } th_push(a%b); th_push(a/b); }
void th_p_1plus (int *w){ th_push(th_pop()+1); }
void th_p_1minus (int *w){ th_push(th_pop()-1); }
void th_p_2times (int *w){ th_push(th_pop() << 1); }
void th_p_2div (int *w) { th_push(th_pop() >> 1); }   // arithmetic: 2/ propagates the sign
void th_p_negate (int *w){ th_push(0-th_pop()); }
void th_p_abs (int *w)  { int x; x=th_pop(); if (x<0) x=0-x; th_push(x); }
void th_p_min (int *w)  { int a,b; b=th_pop(); a=th_pop(); if (a<b) th_push(a); else th_push(b); }
void th_p_max (int *w)  { int a,b; b=th_pop(); a=th_pop(); if (a>b) th_push(a); else th_push(b); }
void th_p_lshift (int *w){ int a,b; b=th_pop(); a=th_pop(); th_push(a<<b); }
void th_p_rshift (int *w){ int a,b; b=th_pop(); a=th_pop(); th_push(th_urshift(a,b)); }

// -- logic and comparison ---------------------------------------------
// Forth-2012 flags are all-bits-set for true, so comparisons yield 0 or -1
// rather than 0 or 1. Getting this wrong makes AND-of-flags silently wrong.
void th_p_and (int *w)  { int a,b; b=th_pop(); a=th_pop(); th_push(a&b); }
void th_p_or (int *w)   { int a,b; b=th_pop(); a=th_pop(); th_push(a|b); }
void th_p_xor (int *w)  { int a,b; b=th_pop(); a=th_pop(); th_push(a^b); }
void th_p_invert (int *w){ th_push(~th_pop()); }
void th_p_eq (int *w)   { int a,b; b=th_pop(); a=th_pop(); if (a==b) th_push(-1); else th_push(0); }
void th_p_ne (int *w)   { int a,b; b=th_pop(); a=th_pop(); if (a!=b) th_push(-1); else th_push(0); }
void th_p_lt (int *w)   { int a,b; b=th_pop(); a=th_pop(); if (a<b) th_push(-1); else th_push(0); }
void th_p_ult (int *w)  { int a,b; b=th_pop(); a=th_pop(); if (th_uless(a,b)) th_push(-1); else th_push(0); }
void th_p_ugt (int *w)  { int a,b; b=th_pop(); a=th_pop(); if (th_uless(b,a)) th_push(-1); else th_push(0); }
void th_p_gt (int *w)   { int a,b; b=th_pop(); a=th_pop(); if (a>b) th_push(-1); else th_push(0); }
void th_p_le (int *w)   { int a,b; b=th_pop(); a=th_pop(); if (a<=b) th_push(-1); else th_push(0); }
void th_p_ge (int *w)   { int a,b; b=th_pop(); a=th_pop(); if (a>=b) th_push(-1); else th_push(0); }
void th_p_zeq (int *w)  { if (th_pop()==0) th_push(-1); else th_push(0); }
void th_p_zne (int *w)  { if (th_pop()!=0) th_push(-1); else th_push(0); }
void th_p_zlt (int *w)  { if (th_pop()<0) th_push(-1); else th_push(0); }
void th_p_zgt (int *w)  { if (th_pop()>0) th_push(-1); else th_push(0); }

// -- memory -----------------------------------------------------------
void th_p_fetch (int *w) { th_push(*(int *)th_pop()); }
void th_p_store (int *w) { int a,x; a=th_pop(); x=th_pop(); *(int *)a = x; }
void th_p_cfetch (int *w){ th_push(*(char *)th_pop()); }
void th_p_cstore (int *w){ int a,x; a=th_pop(); x=th_pop(); *(char *)a = x; }
void th_p_plusstore (int *w){ int a,x; a=th_pop(); x=th_pop(); *(int *)a = *(int *)a + x; }
// Forth-2012 requires MOVE to work when the regions overlap; memcpy does
// not promise that, and the native backend's code motion slides blocks
// over themselves. Copy in whichever direction is safe.
void th_p_move (int *w) {
	int   n, d, s, i;
	char *dp;
	char *sp2;

	n = th_pop(); d = th_pop(); s = th_pop();
	if (n <= 0 || d == s) return;
	dp = (char *)d; sp2 = (char *)s;
	if (d < s) { i = 0; while (i < n) { dp[i] = sp2[i]; ++i; } }
	else       { i = n; while (i > 0) { --i; dp[i] = sp2[i]; } }
}
void th_p_fill (int *w) { int c,n,a; c=th_pop(); n=th_pop(); a=th_pop(); if (n>0) memset((char *)a,c,n); }

// -- dictionary space -------------------------------------------------
void th_p_here (int *w) { th_push((int)th_hp); }
void th_p_comma (int *w){ th_comma(th_pop()); }
void th_p_ccomma (int *w){ char *p; p = th_alloc_bytes(1); if (p) *p = th_pop(); }
void th_p_allot (int *w){ int n; n = th_pop(); if (n > 0) th_alloc_bytes(n); else if (n < 0) th_hp = th_hp + n; }
void th_p_cells (int *w){ th_push(th_pop() * sizeof(int)); }
void th_p_cellplus (int *w){ th_push(th_pop() + sizeof(int)); }

// -- control ----------------------------------------------------------
void th_p_lit (int *w)  { th_push(*th_ip); th_ip = th_ip + 1; }
void th_p_branch (int *w){ th_ip = (int *)*th_ip; }
void th_p_zbranch (int *w){ int f; f = th_pop(); if (f) th_ip = th_ip + 1; else th_ip = (int *)*th_ip; }
void th_p_exit (int *w) { th_ip = (int *)th_rpop(); }
void th_p_execute (int *w){ th_execute((int *)th_pop()); }

// -- output -----------------------------------------------------------
void th_p_emit (int *w) { th_emit(th_pop()); }
void th_p_cr (int *w)   { printf("\n"); }
void th_p_space (int *w){ th_emit(' '); }
// . prints in BASE, not in decimal. Forth-2012 defines it in terms of the
// pictured-output words (<# # #S #>), which arrive with num.h at B3; until
// then this is the same conversion written directly, so that BASE means
// what it says from the start rather than only after B3.
void th_print_num (int v, int base) {
	// The magnitude is taken as an UNSIGNED value, because 0 - MIN-INT is
	// still MIN-INT: negating it and then dividing produced negative
	// digits and printed punctuation. Reading it unsigned is exactly
	// right -- the magnitude of MIN-INT really is 2^(bits-1).
	if (v < 0) { th_emit('-'); th_print_unum(0 - v, base); }
	else th_print_unum(v, base);
}

void th_p_dot (int *w)  { th_print_num(th_pop(), th_base); th_emit(' '); }
void th_p_decimal (int *w) { th_base = 10; }
void th_p_hex (int *w)     { th_base = 16; }
void th_p_type (int *w) { int n,a; n=th_pop(); a=th_pop(); th_type((char *)a, n); }
void th_p_dots (int *w) {           // .S -- non-standard but indispensable
	int *p;
	printf("<%d> ", th_sp - th_dstack);
	p = th_dstack;
	while (p < th_sp) { printf("%d ", *p); p = p + 1; }
}
// CYCLES and INVOKE are how c4th measures and runs native C4 code. Both
// are c4m opcodes; include/c4m.h stubs them to 0 for the gcc build, so
// natively CYCLES reads zero and INVOKE does nothing. That is the honest
// behaviour -- there is no cycle counter and no VM to invoke into.
void th_p_cycles (int *w) { th_push(__c4_cycles()); }
// Call generated code. Not __c4_invoke: c4m implements C4IV only when it
// is itself running under plain c4 (c4m.c's NOT_NATIVE branch), so
// natively it does nothing and returns the accumulator unchanged. An
// indirect call through a local is a JSRS, which works on every host --
// the same mechanism the inner interpreter already runs on.
#ifndef __c4cc__
#define th_nf()        ((int (*)(void))th_nf)()
#define th_nf1(a)      ((int (*)(int))th_nf1)(a)
#define th_nf2(a,b)    ((int (*)(int,int))th_nf2)(a,b)
#define th_nf3(a,b,c)  ((int (*)(int,int,int))th_nf3)(a,b,c)
#endif

void th_p_invoke (int *w) {
	int *th_nf;

	th_nf = (int *)th_pop();
	th_push(th_nf());
}

// Generated code for a word that takes arguments follows C4's own calling
// convention -- the caller pushes them and the callee reads them off bp --
// so calling it IS an ordinary call with that many arguments. Forth pushes
// the first argument first, and so does C4, so the orders already agree.
void th_p_invoke1 (int *w) {
	int *th_nf1; int a;

	th_nf1 = (int *)th_pop(); a = th_pop();
	th_push(th_nf1(a));
}

void th_p_invoke2 (int *w) {
	int *th_nf2; int a, b;

	th_nf2 = (int *)th_pop(); b = th_pop(); a = th_pop();
	th_push(th_nf2(a, b));
}

void th_p_invoke3 (int *w) {
	int *th_nf3; int a, b, c;

	th_nf3 = (int *)th_pop(); c = th_pop(); b = th_pop(); a = th_pop();
	th_push(th_nf3(a, b, c));
}

// SAVE-FILE ( addr len c-addr u -- flag )
//
// The C4 VM has no write syscall, so this works on the gcc build only --
// the same limit c4sp's file:write lives with (src/c4sp/include/stdlib.h),
// and the same 577/384 (O_WRONLY|O_CREAT|O_TRUNC, 0600) it uses. Under
// c4m the metacompiler can still build an image and compare it in
// memory; it just cannot put it on disk.
void th_p_savefile (int *w) {
	int a, n, na, nu;
#if NATIVE
	int fd, wr;
	char *path;
#endif

	nu = th_pop(); na = th_pop(); n = th_pop(); a = th_pop();
#if NATIVE
	if (nu >= TH_PATH_MAX) { printf("c4th: SAVE-FILE: name too long\n"); th_push(0); return; }
	path = th_pathbuf;
	memcpy(path, (char *)na, nu);
	path[nu] = 0;
	if ((fd = open(path, 577, 384)) < 0) { th_push(0); return; }
	wr = write(fd, (char *)a, n);
	close(fd);
	th_push(wr == n ? -1 : 0);
#else
	printf("c4th: SAVE-FILE needs a native build -- the C4 VM has no write syscall\n");
	th_push(0);
#endif
}

// The four the self-hosting compiler needs, and c4th did not have --
// each is one C4 syscall, which is the point: self.f emits MALC, OPEN,
// READ and CLOS for them directly, so the compiled compiler reaches
// memory and files by the same route the hosted one does. OPENF takes a
// nul-terminated path because open() does, and because a Forth string
// with an explicit length would need a scratch buffer in the target for
// no gain -- self.f builds the nul itself.
//
// ALLOCATE is here rather than as an ordinary Forth word for the reason
// SAVE-FILE is: the image's data segment is written out byte for byte,
// so a compiler's buffers must NOT live there. Half a megabyte of zeros
// in the file is half a megabyte the image has to print.
void th_p_allocate (int *w) { th_push((int)malloc(th_pop())); }
void th_p_openf (int *w)    { th_push(open((char *)th_pop(), 0)); }
void th_p_readf (int *w)    { int fd, a, n; n = th_pop(); a = th_pop(); fd = th_pop(); th_push(read(fd, (char *)a, n)); }
void th_p_closef (int *w)   { close(th_pop()); }
// HALT ( n -- ) is exit(n). c4th has BYE, but BYE takes no status and a
// compiler that has just refused a program must say so in its exit code.
void th_p_halt (int *w)     { exit(th_pop()); }

void th_p_bye (int *w)  { th_ip = 0; th_quit = 1; }


// -- counted loops ----------------------------------------------------
//
// DO ... LOOP keeps its limit and index on the return stack, so a loop
// body can still use >R and R> as long as it balances them. I reaches past
// nothing; J reaches past one loop's pair.

void th_p_pdo (int *w) {             // (DO) ( limit index -- )
	int i, l;
	i = th_pop(); l = th_pop();
	th_rpush(l); th_rpush(i);
}

void th_p_ploop (int *w) {           // (LOOP): ++index, branch back unless done
	int i, l;
	if (th_rp - th_rstack < 2) { printf("c4th: (LOOP) outside a loop\n"); th_err = 1; return; }
	i = th_rp[-1] + 1;
	l = th_rp[-2];
	if (i == l) { th_rp = th_rp - 2; th_ip = th_ip + 1; }
	else        { th_rp[-1] = i; th_ip = (int *)*th_ip; }
}

void th_p_pploop (int *w) {          // (+LOOP) ( n -- )
	int i, l, n, nu;

	n = th_pop();
	if (th_rp - th_rstack < 2) { printf("c4th: (+LOOP) outside a loop\n"); th_err = 1; return; }
	i = th_rp[-1];
	l = th_rp[-2];
	nu = i + n;
	// Terminate when the index crosses the boundary between limit-1 and
	// limit, in EITHER direction -- which is what the standard says, and
	// why a plain i >= l test is wrong for a negative step. Biasing both
	// the old and new index by the limit turns "crossed" into "the sign
	// of the difference changed", which is one xor.
	if (((i - l) ^ (nu - l)) < 0) { th_rp = th_rp - 2; th_ip = th_ip + 1; }
	else                          { th_rp[-1] = nu;   th_ip = (int *)*th_ip; }
}

void th_p_i (int *w) {
	if (th_rp - th_rstack < 1) { printf("c4th: I outside a loop\n"); th_err = 1; return; }
	th_push(th_rp[-1]);
}

void th_p_j (int *w) {
	if (th_rp - th_rstack < 3) { printf("c4th: J outside two loops\n"); th_err = 1; return; }
	th_push(th_rp[-3]);
}

void th_p_unloop (int *w) {
	if (th_rp - th_rstack < 2) { printf("c4th: UNLOOP outside a loop\n"); th_err = 1; return; }
	th_rp = th_rp - 2;
}

// -- inline strings ---------------------------------------------------
//
// (S") is followed in the body by a length cell and then the bytes,
// padded out to a whole number of cells. It pushes address and length and
// steps th_ip past the lot, so a string costs no runtime allocation.

void th_p_psquote (int *w) {
	int   n, cells;
	char *p;

	n = *th_ip;
	p = (char *)(th_ip + 1);
	cells = (n + sizeof(int) - 1) / sizeof(int);
	th_push((int)p);
	th_push(n);
	th_ip = th_ip + 1 + cells;
}

// -- odds and ends core.f needs ---------------------------------------
void th_p_count (int *w) { int a; a = th_pop(); th_push(a + 1); th_push(*(char *)a); }
void th_p_chars (int *w) { }                      // CHARS is the identity here
void th_p_charplus (int *w) { th_push(th_pop() + 1); }
void th_p_aligned (int *w) {
	int a;
	a = th_pop();
	th_push((a + sizeof(int) - 1) / sizeof(int) * sizeof(int));
}
void th_p_align (int *w) { th_align_here(); }
void th_p_bl (int *w)    { th_push(32); }
void th_p_udot (int *w)  { th_print_unum(th_pop(), th_base); th_emit(' '); }
// ACCEPT reads a line from the terminal, not from the source file -- it
// is the one word here that genuinely wants fd 0. A byte at a time is a
// syscall per character, which is slow under the VM and irrelevant: the
// suite calls it once.
void th_p_accept (int *w) {
	int   n, a, i;
	char  ch;

	n = th_pop();
	a = th_pop();
	i = 0;
	while (i < n) {
		if (read(0, &ch, 1) != 1) break;
		if (ch == '\n') break;
		((char *)a)[i] = ch;
		++i;
	}
	th_push(i);
}

void th_p_source (int *w) {
	int *s;
	s = th_src();
	th_push(s[SRC_LINE]);
	th_push(s[SRC_LLEN]);
}
void th_p_toin (int *w) { th_push((int)(th_src() + SRC_IN)); }
void th_p_refill (int *w) { if (th_refill()) th_push(-1); else th_push(0); }
void th_p_key (int *w) {
	int  *s;
	char *line;
	// KEY from the current line; enough for the suite, which never uses it
	// interactively.
	s = th_src();
	line = (char *)s[SRC_LINE];
	if (s[SRC_IN] < s[SRC_LLEN]) { th_push(line[s[SRC_IN]]); s[SRC_IN] = s[SRC_IN] + 1; }
	else th_push(10);
}

void th_prims_init () {
	th_defword("DUP",0,(int)&th_p_dup);        th_defword("DROP",0,(int)&th_p_drop);
	th_defword("SWAP",0,(int)&th_p_swap);      th_defword("OVER",0,(int)&th_p_over);
	th_defword("NIP",0,(int)&th_p_nip);        th_defword("TUCK",0,(int)&th_p_tuck);
	th_defword("ROT",0,(int)&th_p_rot);        th_defword("-ROT",0,(int)&th_p_nrot);
	th_defword("?DUP",0,(int)&th_p_qdup);      th_defword("DEPTH",0,(int)&th_p_depth);
	th_defword("PICK",0,(int)&th_p_pick);
	th_defword("2DUP",0,(int)&th_p_2dup);      th_defword("2DROP",0,(int)&th_p_2drop);
	th_defword("2SWAP",0,(int)&th_p_2swap);    th_defword("2OVER",0,(int)&th_p_2over);
	th_defword(">R",FL_COMPONLY,(int)&th_p_tor);
	th_defword("R>",FL_COMPONLY,(int)&th_p_rfrom);
	th_defword("R@",FL_COMPONLY,(int)&th_p_rfetch);
	th_defword("+",0,(int)&th_p_add);          th_defword("-",0,(int)&th_p_sub);
	th_defword("*",0,(int)&th_p_mul);          th_defword("/",0,(int)&th_p_div);
	th_defword("MOD",0,(int)&th_p_mod);        th_defword("/MOD",0,(int)&th_p_divmod);
	th_defword("1+",0,(int)&th_p_1plus);       th_defword("1-",0,(int)&th_p_1minus);
	th_defword("2*",0,(int)&th_p_2times);      th_defword("2/",0,(int)&th_p_2div);
	th_defword("NEGATE",0,(int)&th_p_negate);  th_defword("ABS",0,(int)&th_p_abs);
	th_defword("MIN",0,(int)&th_p_min);        th_defword("MAX",0,(int)&th_p_max);
	th_defword("LSHIFT",0,(int)&th_p_lshift);  th_defword("RSHIFT",0,(int)&th_p_rshift);
	th_defword("AND",0,(int)&th_p_and);        th_defword("OR",0,(int)&th_p_or);
	th_defword("XOR",0,(int)&th_p_xor);        th_defword("INVERT",0,(int)&th_p_invert);
	th_defword("=",0,(int)&th_p_eq);           th_defword("<>",0,(int)&th_p_ne);
	th_defword("<",0,(int)&th_p_lt);           th_defword(">",0,(int)&th_p_gt);
	th_defword("<=",0,(int)&th_p_le);          th_defword(">=",0,(int)&th_p_ge);
	th_defword("0=",0,(int)&th_p_zeq);         th_defword("0<>",0,(int)&th_p_zne);
	th_defword("0<",0,(int)&th_p_zlt);         th_defword("0>",0,(int)&th_p_zgt);
	th_defword("@",0,(int)&th_p_fetch);        th_defword("!",0,(int)&th_p_store);
	th_defword("C@",0,(int)&th_p_cfetch);      th_defword("C!",0,(int)&th_p_cstore);
	th_defword("+!",0,(int)&th_p_plusstore);   th_defword("MOVE",0,(int)&th_p_move);
	th_defword("FILL",0,(int)&th_p_fill);
	th_defword("HERE",0,(int)&th_p_here);      th_defword(",",0,(int)&th_p_comma);
	th_defword("C,",0,(int)&th_p_ccomma);      th_defword("ALLOT",0,(int)&th_p_allot);
	th_defword("CELLS",0,(int)&th_p_cells);    th_defword("CELL+",0,(int)&th_p_cellplus);
	th_defword("LIT",FL_COMPONLY,(int)&th_p_lit);
	th_defword("BRANCH",FL_COMPONLY,(int)&th_p_branch);
	th_defword("0BRANCH",FL_COMPONLY,(int)&th_p_zbranch);
	th_defword("EXIT",FL_COMPONLY,(int)&th_p_exit);
	th_defword("EXECUTE",0,(int)&th_p_execute);
	th_defword("EMIT",0,(int)&th_p_emit);      th_defword("CR",0,(int)&th_p_cr);
	th_defword("SPACE",0,(int)&th_p_space);    th_defword(".",0,(int)&th_p_dot);
	th_defword("TYPE",0,(int)&th_p_type);      th_defword(".S",0,(int)&th_p_dots);
	th_defword("(DO)",FL_COMPONLY,(int)&th_p_pdo);
	th_defword("(LOOP)",FL_COMPONLY,(int)&th_p_ploop);
	th_defword("(+LOOP)",FL_COMPONLY,(int)&th_p_pploop);
	th_defword("I",FL_COMPONLY,(int)&th_p_i);
	th_defword("J",FL_COMPONLY,(int)&th_p_j);
	th_defword("UNLOOP",FL_COMPONLY,(int)&th_p_unloop);
	th_defword("(S\")",FL_COMPONLY,(int)&th_p_psquote);
	th_defword("COUNT",0,(int)&th_p_count);
	th_defword("CHARS",0,(int)&th_p_chars);
	th_defword("CHAR+",0,(int)&th_p_charplus);
	th_defword("ALIGNED",0,(int)&th_p_aligned);
	th_defword("ALIGN",0,(int)&th_p_align);
	th_defword("BL",0,(int)&th_p_bl);
	th_defword("U.",0,(int)&th_p_udot);
	th_defword("U<",0,(int)&th_p_ult);
	th_defword("U>",0,(int)&th_p_ugt);
	th_defword("SOURCE",0,(int)&th_p_source);
	th_defword(">IN",0,(int)&th_p_toin);
	th_defword("REFILL",0,(int)&th_p_refill);
	th_defword("KEY",0,(int)&th_p_key);
	th_defword("ACCEPT",0,(int)&th_p_accept);
	th_defword("DECIMAL",0,(int)&th_p_decimal);
	th_defword("HEX",0,(int)&th_p_hex);
	th_defword("CYCLES",0,(int)&th_p_cycles);
	th_defword("INVOKE",0,(int)&th_p_invoke);
	th_defword("INVOKE1",0,(int)&th_p_invoke1);
	th_defword("INVOKE2",0,(int)&th_p_invoke2);
	th_defword("INVOKE3",0,(int)&th_p_invoke3);
	th_defword("SAVE-FILE",0,(int)&th_p_savefile);
	th_defword("ALLOCATE",0,(int)&th_p_allocate);
	th_defword("OPENF",0,(int)&th_p_openf);
	th_defword("READF",0,(int)&th_p_readf);
	th_defword("CLOSEF",0,(int)&th_p_closef);
	th_defword("HALT",0,(int)&th_p_halt);
	th_defword("BYE",0,(int)&th_p_bye);
}
