// calc.c -- CALC.C4R, grin's Calculator (docs/grin-plan.md 4): the window
// kit's Calculator class (src/winkit/calc.c), the C4IX desktop's, linked
// with import.c. Every launch opens another Calculator on the same image.

#include <stdio.h>
#include "winkit.h"
#include "grin.h"

int main(int argc, char **argv) {
    if (!grin_present()) { printf("Calculator is a grin application: GRIN starts grin, and Calculator is in its Accessories.\n"); return 1; }
    if (calc_open() < 0) return 1;
    return GRIN_RESIDENT;
}
