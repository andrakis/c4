// L9 preprocessor exercise: every construct c4lc's own preprocessor
// claims to support. Compiled with -P (no gcc -E), its behaviour must
// match the gcc-preprocessed build of the same file exactly.

#define ONE 1
#define TWO (ONE + ONE)
#define ADD(a, b) ((a) + (b))
#define TWICE(x) ADD(x, x)
#define LONG_MACRO(a) \
    ((a) * 10)

#ifndef GUARD
#define GUARD
int guarded() { return TWO; }
#endif

#ifdef NOT_DEFINED
int should_not_exist() { return 1 / 0; }
#endif

#if ONE
int taken_if() { return ADD(TWO, 3); }
#else
int not_taken() { return 1 / 0; }
#endif

#if defined(NOT_DEFINED)
int nope() { return 1 / 0; }
#elif ONE > 0 && !defined(NOT_DEFINED)
int taken_elif() { return TWICE(4); }
#else
int nope2() { return 1 / 0; }
#endif

#undef ONE
#ifdef ONE
int undef_failed() { return 1 / 0; }
#endif

int nested() { return LONG_MACRO(ADD(1, 2)); }

// stringize and paste, including the two-level form: an argument is
// macro-expanded before substitution EXCEPT as an operand of # or ##,
// which is why STR(V) gives "V" while XSTR(V) gives V's value.
#define STR(x) #x
#define XSTR(x) STR(x)
#define CAT(a, b) a ## b
#define VERSION 7
#define PREFIXED(n) c4lc_ ## n

int CAT(fo, ur)() { return 4; }
int PREFIXED(probe)() { return 11; }

int main() {
    printf("guarded %d\n", guarded());
    printf("taken_if %d\n", taken_if());
    printf("taken_elif %d\n", taken_elif());
    printf("nested %d\n", nested());
    printf("str %s\n", STR(hello world));
    printf("xstr %s\n", XSTR(VERSION));
    printf("str-noexpand %s\n", STR(VERSION));
    printf("cat %d\n", CAT(fo, ur)());
    printf("paste %d\n", c4lc_probe());
    return 0;
}
