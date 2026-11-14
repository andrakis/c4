// C4 Test: a global __c4lc_init sets, initialized twice in one unit, is an
// error, in c4lc ("redefinition of r") and c4fc alike (test-c4lc, test-c4fc)

int t[4];
int *r = &t[1];
int *r = &t[2];

int main () { return *r; }
