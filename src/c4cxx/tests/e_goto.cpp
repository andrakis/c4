// expect: a goto where objects with destructors are in scope
struct D { ~D () {} };
void f () { D d; goto out; out: ; }
