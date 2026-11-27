// c4lc_linka.c, with c4lc_linkb.c: a function's linkage as C has it.
// Each unit has a helper declared static and defined without the word
// (Blood's MoveToCeil, in aibat.cpp and aiboneel.cpp): each stays its
// unit's own, and the link keeps both. And `extern` on a definition
// (chocolate-blood's a_asm.c) is a definition the other unit calls.
// Compiled -c, linked by c4rlink: "a 2 b 3".
static int helper (void);

int helper (void)
{
	return 2;
}

extern int from_a (void)
{
	return helper ();
}
