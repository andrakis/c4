// stdlib.h: C4IX's libc keeps its whole interface in libc.h -- but for
// the floating-point part, which is math.h's (docs/c4-float.md says why);
// C puts these three here too
#include "libc.h"
double strtod(char *s, char **end);
float  strtof(char *s, char **end);
double atof(char *s);
