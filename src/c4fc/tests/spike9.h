// F3: the header side of the preprocessor spike. Included by name, so
// it also checks that "quoted" resolves and that a guard works.
#ifndef SPIKE9_H
#define SPIKE9_H 1

#define S9_WIDTH   8
#define S9_HEIGHT  (S9_WIDTH / 2)
#define S9_AREA(w, h) ((w) * (h))
#define S9_JOIN(a, b) a ## b
#define S9_STR(x) #x
#define S9_XSTR(x) S9_STR(x)

// a continuation, so the splice has to happen before the directive is
// split off at its line
#define S9_LONG(a) \
    ((a) + \
     S9_WIDTH)

#endif
