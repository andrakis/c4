//
// math.h -- C4IX's libc in floating point (docs/c4-float.md): math.c's
// functions, in double and float, and the conversions from text, which
// C puts in stdlib.h (libc.h says why they are here).
//
// math.c is fdlibm's (Sun Microsystems, 1993), measured against glibc by
// src/c4ix/libc/tests/mathtest.c: sqrt, floor, ceil, trunc, round, fmod,
// modf, frexp and ldexp exact; the rest within an ulp or two. The float
// versions are the double ones rounded. errno is EDOM (33) or ERANGE.
//

#ifndef __C4IX_MATH_H
#define __C4IX_MATH_H 1
#include "libc.h"

#define EDOM 33
#define HUGE_VAL (1.0 / 0.0)
#define HUGE_VALF ((float)(1.0 / 0.0))
#define INFINITY HUGE_VALF
#define NAN ((float)(0.0 / 0.0))
#define M_E        2.71828182845904523536
#define M_LOG2E    1.44269504088896340736
#define M_LOG10E   0.434294481903251827651
#define M_LN2      0.693147180559945309417
#define M_LN10     2.30258509299404568402
#define M_PI       3.14159265358979323846
#define M_PI_2     1.57079632679489661923
#define M_PI_4     0.785398163397448309616
#define M_1_PI     0.318309886183790671538
#define M_2_PI     0.636619772367581343076
#define M_2_SQRTPI 1.12837916709551257390
#define M_SQRT2    1.41421356237309504880
#define M_SQRT1_2  0.707106781186547524401

double strtod(char *s, char **end);
float  strtof(char *s, char **end);
double atof(char *s);

double fabs(double x);
double copysign(double x, double y);
double sqrt(double x);
double cbrt(double x);
double hypot(double x, double y);
double trunc(double x);
double floor(double x);
double ceil(double x);
double round(double x);
double modf(double x, double *ip);
double fmod(double x, double y);
double frexp(double x, int *e);
double ldexp(double x, int n);
double scalbn(double x, int n);
double fmin(double a, double b);
double fmax(double a, double b);
double exp(double x);
double expm1(double x);
double log(double x);
double log10(double x);
double log2(double x);
double log1p(double x);
double pow(double x, double y);
double sin(double x);
double cos(double x);
double tan(double x);
double asin(double x);
double acos(double x);
double atan(double x);
double atan2(double y, double x);
double sinh(double x);
double cosh(double x);
double tanh(double x);

float sqrtf(float x);
float fabsf(float x);
float floorf(float x);
float ceilf(float x);
float truncf(float x);
float roundf(float x);
float fmodf(float x, float y);
float expf(float x);
float logf(float x);
float log10f(float x);
float powf(float x, float y);
float sinf(float x);
float cosf(float x);
float tanf(float x);
float atanf(float x);
float atan2f(float y, float x);
float asinf(float x);
float acosf(float x);
float hypotf(float x, float y);

int __c4ix_isnan(double x);
int __c4ix_isinf(double x);
int __c4ix_isfinite(double x);
#define isnan(x) __c4ix_isnan(x)
#define isinf(x) __c4ix_isinf(x)
#define isfinite(x) __c4ix_isfinite(x)
#define signbit(x) (copysign(1.0, (x)) < 0)

#endif // __C4IX_MATH_H
