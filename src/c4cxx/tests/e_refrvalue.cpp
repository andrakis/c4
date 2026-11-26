// expect: a reference (not const) to what is not an lvalue
void g (int &);
void h () { g (3); }
