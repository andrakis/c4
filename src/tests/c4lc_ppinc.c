// Regression. #include splices the included file's tokens in front of the
// rest, and they carry THEIR line numbers, so the numbers in the stream
// stop increasing. A directive used to be "every token sharing the Hash's
// line", so a header ending in a directive on line N, included from line N
// of a file whose line N+... see docs. Both headers must arrive.
#include "c4lc_ppinc.h"
#include "c4lc_ppinc2.h"
int main () { return PPINC_OK + PPINC2_OK; }
