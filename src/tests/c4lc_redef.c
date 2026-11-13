// C4 Test: a global initialized twice in one unit is an error, in c4lc
// ("redefinition of r") and c4fc alike (test-c4lc, test-c4fc)

int r = 1;
int r = 2;

int main () { return r; }
