// C4 Multiloader optional component: floating point support
//

#ifndef __C4M_FLOAT_C
#define __C4M_FLOAT_C 1

// The FPU (F3) is softfp.c itself, compiled natively: the same source as
// the guest's helpers, so its answers are theirs to the bit, at native
// speed. Included before c4.h, whose `#define int` would make its int
// the machine word.
#ifndef C4CC
#include "src/c4fp/softfp.c"
#endif

#ifndef NO_LIBMATH
#include <math.h>
#endif

#include "c4.h"
#include "c4m_float.h"

#ifndef C4CC // __c4cc__ ?

// int is the machine word here (c4.h): a float's bits are 32 of them, and
// the rest must be zero, not whatever the union's other bytes held
union f2i {
	float f;
	__UINT32_TYPE__ i;
};

float c4_reinterpret_as_float (int v) {
	union f2i f = { .i = v };
	return f.f;
}

int c4_reinterpret_as_int (float v) {
	union f2i i = { .f = v };
	return (int)i.i;
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
		case FLTI_FTOI: return (int)a;   // an integer, not a float's bits
		default:
			printf("c4m fatal error: unsupported floating point instruction %d!\n", sp[0]);
			break;
	}

	return c4_reinterpret_as_int(c);
}

// The FPU opcode: sp[0] the sub-op, sp[1] a, sp[2] b, sp[3] r -- floats
// as their bits, doubles by address (src/c4fp/softfp.c lists the sub-ops)
static long long c4_fpu_int (int v) {
	return sizeof(void *) == 8 ? (long long)v : (long long)(__INT32_TYPE__)v;
}
static int c4_fpu_toint (long long v) {
	// a 32-bit machine's int: what does not fit is the most negative
	if (sizeof(void *) == 4 && (v < -2147483647LL - 1 || v > 2147483647LL)) return (int)(__INT32_TYPE__)0x80000000u;
	return (int)v;
}
int c4_fpu_instruction (int *sp) {
	int op, a, b, r;
	unsigned long long *pa, *pb, *pr;
	__UINT32_TYPE__ fa, fb;
	op = sp[0]; a = sp[1]; b = sp[2]; r = sp[3];
	fa = (__UINT32_TYPE__)a; fb = (__UINT32_TYPE__)b;
	pa = (unsigned long long *)a; pb = (unsigned long long *)b; pr = (unsigned long long *)r;
	switch (op) {
		case 1:  return (int)__c4f_add_s(fa, fb);
		case 2:  return (int)__c4f_sub_s(fa, fb);
		case 3:  return (int)__c4f_mul_s(fa, fb);
		case 4:  return (int)__c4f_div_s(fa, fb);
		case 5:  return (int)__c4f_sqrt_s(fa);
		case 6:  return __c4f_cmp_s(fa, fb);
		case 7:  return (int)__c4f_from_ll(c4_fpu_int(a));
		case 8:  return c4_fpu_toint(__c4f_to_ll(fa));
		case 17: *pr = __c4d_add_s(*pa, *pb); return 0;
		case 18: *pr = __c4d_sub_s(*pa, *pb); return 0;
		case 19: *pr = __c4d_mul_s(*pa, *pb); return 0;
		case 20: *pr = __c4d_div_s(*pa, *pb); return 0;
		case 21: *pr = __c4d_sqrt_s(*pa); return 0;
		case 22: return __c4d_cmp_s(*pa, *pb);
		case 23: *pr = __c4d_from_ll(c4_fpu_int(a)); return 0;
		case 24: return c4_fpu_toint(__c4d_to_ll(*pa));
		case 25: *pr = __c4f_to_d_s(fa); return 0;
		case 26: return (int)__c4d_to_f_s(*pa);
		default:
			printf("c4m fatal error: unsupported FPU operation %d!\n", (__INT32_TYPE__)op);
			return 0;
	}
}

#else
int c4_float_instruction (int *sp) {
	// TODO: raise trap
	return 0;
}
int c4_fpu_instruction (int *sp) { return 0; }

#endif // ifndef C4CC

#endif // __C4M_FLOAT_C
