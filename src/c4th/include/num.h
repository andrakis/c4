// c4th: double-cell arithmetic and pictured numeric output.
//
// A double is two cells, low first, so the high cell is on top. C4 has no
// wider type and no unsigned type, so the two-cell product and the
// two-cell division are built by hand out of half-width pieces and a
// restoring shift-subtract. This is where small Forths die, and the
// Forth-2012 CORE suite tests all of it at the extremes.

int th_half_bits () { return sizeof(int) * 8 / 2; }

int th_half_mask () {
	int h, m;
	h = 1;
	h = h << (th_half_bits() - 1);
	m = h - 1 + h;                    // 2^(bits/2) - 1, without shifting
	return m;                         // into the sign bit
}

// UM* -- unsigned 2-cell product of two cells.
void th_ummul (int a, int b, int *lo, int *hi) {
	int H, M, al, ah, bl, bh, p0, p1, p2, p3, t;

	H = th_half_bits();
	M = th_half_mask();
	al = a & M; ah = th_urshift(a, H);
	bl = b & M; bh = th_urshift(b, H);
	p0 = al * bl;
	p1 = ah * bl;
	p2 = al * bh;
	p3 = ah * bh;
	// Fold the two middle products in half at a time so nothing overflows
	// before its carry has been taken out.
	t  = th_urshift(p0, H) + (p1 & M) + (p2 & M);
	*lo = (p0 & M) | ((t & M) << H);
	*hi = p3 + th_urshift(p1, H) + th_urshift(p2, H) + th_urshift(t, H);
}

// UM/MOD -- unsigned (2-cell / 1-cell), restoring division a bit at a
// time. Slow and obviously correct, which is the right trade here: it is
// not on any hot path, and the alternatives are easy to get subtly wrong.
void th_umdivmod (int hi, int lo, int d, int *rem, int *quot) {
	int r, q, i, bits, rtop, carry;

	if (d == 0) { *rem = 0; *quot = 0; return; }
	bits = sizeof(int) * 8;
	r = hi; q = 0; i = 0;
	while (i < bits) {
		rtop  = r < 0;                // bit about to be shifted out of r
		carry = lo < 0;               // top bit of lo shifts into r
		r = (r << 1) | carry;
		lo = lo << 1;
		q = q << 1;
		// rtop means the true remainder is 2^bits + r, which always
		// exceeds a single-cell divisor, so it must be reduced.
		if (rtop || !th_uless(r, d)) { r = r - d; q = q | 1; }
		++i;
	}
	*rem = r; *quot = q;
}

void th_dnegate (int lo, int hi, int *rlo, int *rhi) {
	int l;
	l = ~lo + 1;
	*rlo = l;
	if (l == 0) *rhi = ~hi + 1; else *rhi = ~hi;
}

// -- the words --------------------------------------------------------

void th_p_stod (int *w) {             // S>D ( n -- d )
	int n;
	n = th_pop();
	th_push(n);
	if (n < 0) th_push(-1); else th_push(0);
}

void th_p_ummul (int *w) {            // UM* ( u1 u2 -- ud )
	int a, b, lo, hi;
	b = th_pop(); a = th_pop();
	th_ummul(a, b, &lo, &hi);
	th_push(lo); th_push(hi);
}

void th_p_mmul (int *w) {             // M* ( n1 n2 -- d )
	int a, b, lo, hi, neg;
	b = th_pop(); a = th_pop();
	neg = 0;
	if (a < 0) { a = 0 - a; neg = !neg; }
	if (b < 0) { b = 0 - b; neg = !neg; }
	th_ummul(a, b, &lo, &hi);
	if (neg) th_dnegate(lo, hi, &lo, &hi);
	th_push(lo); th_push(hi);
}

void th_p_umdivmod (int *w) {         // UM/MOD ( ud u -- rem quot )
	int d, hi, lo, r, q;
	d = th_pop(); hi = th_pop(); lo = th_pop();
	th_umdivmod(hi, lo, d, &r, &q);
	th_push(r); th_push(q);
}

// SM/REM truncates toward zero: the remainder takes the dividend's sign.
void th_p_smrem (int *w) {
	int d, hi, lo, r, q, dneg, nneg;

	d = th_pop(); hi = th_pop(); lo = th_pop();
	nneg = hi < 0;
	if (nneg) th_dnegate(lo, hi, &lo, &hi);
	dneg = d < 0;
	if (dneg) d = 0 - d;
	th_umdivmod(hi, lo, d, &r, &q);
	if (nneg != dneg) q = 0 - q;
	if (nneg) r = 0 - r;
	th_push(r); th_push(q);
}

// FM/MOD floors: the remainder takes the divisor's sign. Built on SM/REM's
// result by nudging when the signs disagree and the remainder is non-zero.
void th_p_fmmod (int *w) {
	int d, hi, lo, r, q, dneg, nneg;

	d = th_pop(); hi = th_pop(); lo = th_pop();
	nneg = hi < 0;
	if (nneg) th_dnegate(lo, hi, &lo, &hi);
	dneg = d < 0;
	if (dneg) d = 0 - d;
	th_umdivmod(hi, lo, d, &r, &q);
	if (nneg != dneg) q = 0 - q;
	if (nneg) r = 0 - r;
	if (dneg) d = 0 - d;
	if (r != 0 && ((r < 0) != (d < 0))) { q = q - 1; r = r + d; }
	th_push(r); th_push(q);
}

void th_p_dplus (int *w) {            // D+ ( d1 d2 -- d3 )
	int alo, ahi, blo, bhi, lo, hi;
	bhi = th_pop(); blo = th_pop(); ahi = th_pop(); alo = th_pop();
	lo = alo + blo;
	hi = ahi + bhi;
	if (th_uless(lo, alo)) hi = hi + 1;    // carry out of the low cell
	th_push(lo); th_push(hi);
}

void th_p_dnegate (int *w) {
	int lo, hi;
	hi = th_pop(); lo = th_pop();
	th_dnegate(lo, hi, &lo, &hi);
	th_push(lo); th_push(hi);
}

// -- pictured numeric output ------------------------------------------
//
// Digits are produced least significant first and held in a buffer filled
// from its end, which is exactly what <# # #S HOLD SIGN #> describe.

enum { TH_PIC_MAX = 128 };
char *th_pic_buf;
int   th_pic_len;

void th_p_lessnum (int *w) { th_pic_len = 0; }    // <#

void th_pic_hold (int c) {
	if (th_pic_len >= TH_PIC_MAX) { printf("c4th: pictured output overflow\n"); th_err = 1; return; }
	th_pic_buf[TH_PIC_MAX - 1 - th_pic_len] = c;
	++th_pic_len;
}

void th_p_hold (int *w) { th_pic_hold(th_pop()); }

void th_p_sign (int *w) { if (th_pop() < 0) th_pic_hold('-'); }

void th_p_num (int *w) {              // # ( ud -- ud' )
	int hi, lo, r, q1, q2, d;

	hi = th_pop(); lo = th_pop();
	// Divide the double by BASE: the high cell first, then the low cell
	// carrying the high remainder in.
	th_umdivmod(0,  hi, th_base, &r,  &q1);
	th_umdivmod(r,  lo, th_base, &d,  &q2);
	if (d < 10) th_pic_hold('0' + d); else th_pic_hold('A' + d - 10);
	th_push(q2); th_push(q1);
}

void th_p_nums (int *w) {             // #S
	int hi, lo;
	while (1) {
		th_p_num(w);
		hi = th_sp[-1]; lo = th_sp[-2];
		if (th_err) return;
		if (!hi && !lo) return;
	}
}

void th_p_numgreater (int *w) {       // #> ( ud -- addr u )
	th_pop(); th_pop();
	th_push((int)(th_pic_buf + TH_PIC_MAX - th_pic_len));
	th_push(th_pic_len);
}

void th_num_init () {
	th_pic_buf = malloc(TH_PIC_MAX);
	th_pic_len = 0;
	th_defword("S>D", 0, (int)&th_p_stod);
	th_defword("UM*", 0, (int)&th_p_ummul);
	th_defword("M*", 0, (int)&th_p_mmul);
	th_defword("UM/MOD", 0, (int)&th_p_umdivmod);
	th_defword("SM/REM", 0, (int)&th_p_smrem);
	th_defword("FM/MOD", 0, (int)&th_p_fmmod);
	th_defword("D+", 0, (int)&th_p_dplus);
	th_defword("DNEGATE", 0, (int)&th_p_dnegate);
	th_defword("<#", 0, (int)&th_p_lessnum);
	th_defword("HOLD", 0, (int)&th_p_hold);
	th_defword("SIGN", 0, (int)&th_p_sign);
	th_defword("#", 0, (int)&th_p_num);
	th_defword("#S", 0, (int)&th_p_nums);
	th_defword("#>", 0, (int)&th_p_numgreater);
}
