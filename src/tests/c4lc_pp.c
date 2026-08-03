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

int main() {
    printf("guarded %d\n", guarded());
    printf("taken_if %d\n", taken_if());
    printf("taken_elif %d\n", taken_elif());
    printf("nested %d\n", nested());
    return 0;
}
