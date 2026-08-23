// c4th: the inner interpreter.
//
// Indirect-threaded: a body is a list of execution tokens, and each token's
// W_CODE field holds the address of the C function that runs it. The call
// goes through a LOCAL, which c4cc compiles to JSRS -- one VM instruction,
// and reentrant because the pointer lives in a frame slot rather than a
// global (c4m.c:494-495). c4cc emits a function's address for &name
// (c4cc.c:808-810); a bare name as a value emits a bogus LI, so every code
// field below is written with &.
//
// gcc will not call through an int *, so the macro shadows the call site
// and casts -- the same trick load-c4r.c:997-1001 already uses. The local
// is named th_w_code rather than something short so the macro cannot
// collide with anything else in the translation unit.
//
// Rejected alternatives, for the record: a C switch on a token number costs
// about fifteen VM instructions under c4cc (the jumptable is built with
// PSH/bounds/MUL/ADD/LI/JMPA, c4cc.c:1088-1183) and is capped at 256 cases;
// a JSRI through one global table has no per-word code field, so VARIABLE,
// CONSTANT, VALUE and especially CREATE/DOES> would each need a bespoke
// token; direct threading leaves >BODY nowhere natural to live.

#ifndef __c4cc__
#define th_w_code(w) ((void (*)(int *))th_w_code)(w)
#endif

int *th_ip;         // instruction pointer into a threaded body

// The whole engine. EXECUTE and do_colon set th_ip and RETURN rather than
// recursing, so the C stack stays one frame deep however deep the Forth
// recursion goes. c4sp needed a 270-line CEK conversion (cek.h) to buy the
// same property for its evaluator; here it falls out of the shape.
void th_run () {
	int *w;
	int *th_w_code;

	while (th_ip && !th_err) {
		w    = (int *)*th_ip;
		th_ip = th_ip + 1;
		th_w_code = (int *)w[W_CODE];
		th_w_code(w);
	}
}

// -- the code fields --------------------------------------------------

// A colon definition: push the return address, jump into the body.
void th_do_colon (int *w) {
	th_rpush((int)th_ip);
	th_ip = w + W__Sz;
}

// CREATE'd word: push the address of its body.
void th_do_var (int *w) {
	th_push((int)(w + W__Sz));
}

// A word made by CREATE ... DOES>: push the body address, then run the
// code the defining word left behind. That is the whole of DOES>, and it
// is why W_DOES earns a header slot rather than a corner of another one --
// a DSL built on c4th (the stated further goal) lives or dies on CREATE
// and DOES> being real rather than approximated.
void th_do_does (int *w) {
	th_push((int)(w + W__Sz));
	th_rpush((int)th_ip);
	th_ip = (int *)w[W_DOES];
}

// CONSTANT: push the value stored in the body.
void th_do_const (int *w) {
	th_push(*(w + W__Sz));
}

// EXECUTE, as a C entry point: set th_ip and let th_run continue. Used by
// the outer interpreter at B2 as well as by the EXECUTE primitive.
void th_execute (int *xt) {
	int *th_w_code;

	th_w_code = (int *)xt[W_CODE];
	th_w_code(xt);
}
