// #if, #elif, defined, and expressions (pp_cond.expected)
#define E 0
#define Q (!E)
#if Q
int q_true;
#else
int q_false;
#endif
#if defined(E) && !defined NOPE
int defined_ok;
#endif
#if 0
#error not here
#elif E + 1 == 1
int elif_taken;
#elif 1
int elif_not_taken;
#else
int else_not_taken;
#endif
#if UNDEFINED_NAME
int undefined_is_zero;
#endif
#if (1 << 4) == 16 && -1 < 0 && 7 / 2 == 3 && 7 % 3 == 1 && (2 ? 5 : 6) == 5
int arithmetic_ok;
#endif
#ifndef E
int ifndef_not_taken;
#endif
