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

// `defined(X)` must not expand X first. With VERSION defined as 7, c4lc
// asked whether `7` was defined, so this was always 0 (M7b; Busybox's
// headers use the form 107 times).
#if defined(VERSION) && defined (TWO) && !defined(NOT_DEFINED)
int defined_paren() { return 1; }
#else
int defined_paren() { return 0; }
#endif

// Once an arm is taken, no later #elif or #else runs. c4lc's #else
// used to run after `#if 1 / #elif 1`, which made one_arm defined twice.
#if VERSION
int one_arm() { return 1; }
#elif VERSION
int one_arm() { return 2; }
#else
int one_arm() { return 3; }
#endif

// Variadic macros (M7b): `...` and __VA_ARGS__, which Busybox's generated
// autoconf.h uses for every IF_FEATURE_X(...), GNU `name...`, and GNU
// `, ## __VA_ARGS__`, whose comma goes when the variable part is empty.
#define IF_ON(...) __VA_ARGS__
#define IF_OFF(...)
#define SUM3(z, ...) sum3(__VA_ARGS__)
#define GNU(fmt, args...) printf(fmt, ##args)
#define GNU2(fmt, ...) printf(fmt, ## __VA_ARGS__)
#define STRV(...) #__VA_ARGS__
int sum3(int a, int b, int c) { return a + b + c; }
IF_ON(int va_on() { return 1; })
IF_OFF(int va_on() { return 2; })

// Blue paint: a macro is not expanded again inside its own expansion,
// so these stand for the functions of the same name instead of looping.
// hush.c defines msg_and_die_if_script(lineno, ...) exactly this way.
int twice(int x) { return 2 * x; }
#define twice(x) (twice(x) + 0)
int note(int line, int v) { return line * 100 + v; }
#define note(line, ...) note(line, __VA_ARGS__)

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
    printf("defined-paren %d\n", defined_paren());
    printf("one-arm %d\n", one_arm());
    printf("va-on %d\n", va_on());
    printf("va-sum %d\n", SUM3(0, 1, 2, 3));
    GNU("gnu-none\n");
    GNU("gnu %d %d\n", 4, 5);
    GNU2("gnu2-none\n");
    GNU2("gnu2 %d\n", 6);
    printf("strv %s\n", STRV(x));
    printf("self %d\n", twice(twice(3)));
    printf("self-va %d\n", note(7, 8));
    return 0;
}
