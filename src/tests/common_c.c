// C4 Test: unit C of common_a.c's link (src/c4sp/tests/common-link.sh)

extern int hidden;      // common_b.c's global, never common_a.c's static
extern int counter;

int read_c (void) { return hidden * 1000 + counter; }
