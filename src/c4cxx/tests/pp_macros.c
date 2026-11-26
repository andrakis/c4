// c4cxx preprocessor cases, compared with pp_macros.expected (c4cxx -E).
// Each line is one case; the expected output was checked against gcc -E.
#define NULL 0
#define S(x) #x
#define XS(x) S(x)
#define CAT(a, b) a##b
#define A(e) ((e) ? 0 : fail(#e))
#define V(fmt, ...) printf(fmt, __VA_ARGS__)
#define G(fmt, args...) printf(fmt, ## args)
#define E(fmt, ...) printf(fmt, ## __VA_ARGS__)
#define SELF SELF + 1
#define F(x) F(x)
#define UINT64_C(c) c##ULL
#define TWO 2
// an argument used and then stringized: the stringized one is unexpanded
A(p != NULL);
// stringizing a string literal keeps its quotes, escaped
A(strcmp(s, "*.MAP") == 0);
// two-level stringize
char *a = S(TWO), *b = XS(TWO);
// paste, and paste giving a number with a suffix
int CAT(x, 1) = CAT(1, 2); unsigned long long u = UINT64_C(0xfffffffff);
// variadic, named variadic, GNU comma
V("%d %d", 1, 2); G("%d", 3); G("none"); E("none");
// a macro naming itself, and a function-like one
int s = SELF; int f = F(1);
// a function-like name without '(' is just the name
int (*g)(int) = F;
// a line splice inside a string is removed
char *j = "one\n\
two";
