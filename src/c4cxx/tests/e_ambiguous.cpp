// expect: an ambiguous call to
void f (int, long);
void f (long, int);
void g () { f (1, 2); }
