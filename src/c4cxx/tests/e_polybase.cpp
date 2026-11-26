// expect: a class with virtual functions whose base has none is not supported
struct A { int a; };
struct B : A { virtual void f (); };
