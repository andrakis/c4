// F3: everything c4fc's preprocessor claims to do, in a program whose
// image must be byte-identical to c4lc -P's. Only constructs the two
// agree on: the deliberate divergences (an #elif after a taken #if
// followed by #else, and defined(X) where X is itself a macro) are
// exercised in src/tests/c4lc_pp.c, whose bar is gcc.
#include "spike9.h"

#define ONE 1
#define TWO (ONE + ONE)

#ifndef S9_GUARDED
#define S9_GUARDED
int guarded () { return TWO; }
#endif

#ifdef S9_NOT_DEFINED
int should_not_exist () { return 1 / 0; }
#endif

#if S9_WIDTH > 4 && !defined(S9_NOT_DEFINED)
int taken_if () { return S9_AREA(S9_WIDTH, S9_HEIGHT); }
#else
int not_taken () { return 1 / 0; }
#endif

#if defined(S9_NOT_DEFINED)
int nope () { return 1 / 0; }
#elif S9_WIDTH * 2 == 16
int taken_elif () { return S9_LONG(S9_HEIGHT); }
#else
int nope2 () { return 1 / 0; }
#endif

#if 0
#if 1
int dead_nested () { return 1 / 0; }
#endif
#endif

#undef ONE
#ifdef ONE
int undef_failed () { return 1 / 0; }
#endif

#define S9_VERSION 9
int S9_JOIN(part, one) () { return 41; }

int main () {
  printf("guarded %d\n", guarded());
  printf("taken_if %d\n", taken_if());
  printf("taken_elif %d\n", taken_elif());
  printf("str %s\n", S9_STR(hello world));
  printf("xstr %s\n", S9_XSTR(S9_VERSION));
  printf("noexpand %s\n", S9_STR(S9_VERSION));
  printf("paste %d\n", partone());
  printf("width %d height %d\n", S9_WIDTH, S9_HEIGHT);
  return 0;
}
