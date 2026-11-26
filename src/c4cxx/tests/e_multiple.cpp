// expect: multiple inheritance is not supported
struct A { int a; };
struct B { int b; };
struct C : A, B { int c; };
