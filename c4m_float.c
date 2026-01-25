// C4 Multiloader optional component: floating point support
//

#ifndef __C4M_FLOAT_C
#define __C4M_FLOAT_C 1

#ifndef NO_LIBMATH
#include <math.h>
#endif

#include "c4.h"
#include "c4m_float.h"

#ifndef C4CC // __c4cc__ ?

union f2i {
	float f;
	int i;
};

float c4_reinterpret_as_float (int v) {
	union f2i f = { .i = v };
	return f.f;
}

int c4_reinterpret_as_int (float v) {
	union f2i i = { .f = v };
	return i.i;
}

int c4_float_instruction (int *sp) {
	float a, b, c;

	a = c4_reinterpret_as_float(sp[1]);
	b = c4_reinterpret_as_float(sp[2]);

	switch(sp[0]) {
		case FLTI_ABSF:
			if ((c = a) < 0) c = a * -1.0f;
			break;
		case FLTI_ADD: c = a + b; break;
		case FLTI_SUB: c = a - b; break;
		case FLTI_MUL: c = a * b; break;
		case FLTI_DIV: c = a / b; break;
		case FLTI_NEG: c = -a; break;
#ifndef NO_LIBMATH
		case FLTI_SIN: c = sinf(a); break;
		case FLTI_COS: c = cosf(a); break;
		case FLTI_TAN: c = tanf(a); break;
#endif
		case FLTI_ITOF: c = (float)(sp[1]); break;
		case FLTI_FTOI: c = (int)a; break;
		default:
			printf("c4m fatal error: unsupported floating point instruction %d!\n", sp[0]);
			break;
	}

	return c4_reinterpret_as_int(c);
}

#else
int c4_float_instruction (int *sp) {
	// TODO: raise trap
	return 0;
}

#endif // ifndef C4CC

#endif // __C4M_FLOAT_C
