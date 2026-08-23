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
void th_p_2times (int *w){ th_push(th_pop()*2); }
void th_p_2div (int *w) { th_push(th_pop()/2); }
void th_p_negate (int *w){ th_push(0-th_pop()); }
void th_p_abs (int *w)  { int x; x=th_pop(); if (x<0) x=0-x; th_push(x); }
void th_p_min (int *w)  { int a,b; b=th_pop(); a=th_pop(); if (a<b) th_push(a); else th_push(b); }
void th_p_max (int *w)  { int a,b; b=th_pop(); a=th_pop(); if (a>b) th_push(a); else th_push(b); }
void th_p_lshift (int *w){ int a,b; b=th_pop(); a=th_pop(); th_push(a<<b); }
void th_p_rshift (int *w){ int a,b; b=th_pop(); a=th_pop(); th_push(a>>b); }

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
void th_p_move (int *w) { int n,d,s; n=th_pop(); d=th_pop(); s=th_pop(); if (n>0) memcpy((char *)d,(char *)s,n); }
void th_p_fill (int *w) { int c,n,a; c=th_pop(); n=th_pop(); a=th_pop(); if (n>0) memset((char *)a,c,n); }

// -- dictionary space -------------------------------------------------
void th_p_here (int *w) { th_push((int)th_here); }
void th_p_comma (int *w){ th_comma(th_pop()); }
void th_p_ccomma (int *w){ char *p; p = th_alloc_bytes(1); if (p) *p = th_pop(); }
void th_p_allot (int *w){ int n; n = th_pop(); if (n > 0) th_alloc_bytes(n); }
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
	char buf[72];
	int  i, neg, d;

	if (base < 2) base = 10;
	neg = 0;
	if (v < 0) { neg = 1; v = 0 - v; }
	i = 0;
	if (!v) { buf[i] = '0'; ++i; }
	while (v) {
		d = v % base;
		if (d < 10) buf[i] = '0' + d; else buf[i] = 'A' + d - 10;
		++i;
		v = v / base;
	}
	if (neg) { buf[i] = '-'; ++i; }
	while (i > 0) { --i; th_emit(buf[i]); }
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
void th_p_bye (int *w)  { th_ip = 0; th_quit = 1; }

void th_prims_init () {
	th_defword("DUP",0,(int)&th_p_dup);        th_defword("DROP",0,(int)&th_p_drop);
	th_defword("SWAP",0,(int)&th_p_swap);      th_defword("OVER",0,(int)&th_p_over);
	th_defword("NIP",0,(int)&th_p_nip);        th_defword("TUCK",0,(int)&th_p_tuck);
	th_defword("ROT",0,(int)&th_p_rot);        th_defword("-ROT",0,(int)&th_p_nrot);
	th_defword("?DUP",0,(int)&th_p_qdup);      th_defword("DEPTH",0,(int)&th_p_depth);
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
	th_defword("DECIMAL",0,(int)&th_p_decimal);
	th_defword("HEX",0,(int)&th_p_hex);
	th_defword("BYE",0,(int)&th_p_bye);
}
