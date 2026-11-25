#ifndef FPU_H
#define FPU_H

// OR1000 single-precision floating point (the lf.* opcode-0x32 group).
// The guest keeps binary32 values directly in its GPRs, so every helper
// here takes and returns a RAW IEEE-754 float32 bit pattern in the low
// 32 bits of an int cell; the two integer converters take/return a
// 32-bit signed integer instead.
//
// Two implementations live in fpu.c, chosen at compile time:
//
//   * NATIVE (native.h defines C4OR1K_NATIVE_FLOAT): real host floats,
//     via a float/unsigned union bit-reinterpret. Exact and complete.
//   * HOSTED (the c4lc dialect, which has no float type -- and the FLT
//     opcode is stubbed on several loaders): a native-only stub. Never
//     reached on the tested hosted paths (basefs's busybox shell and
//     the m-checks use no FP); a hosted soft-float / verified __c4_float
//     wiring is future work. See docs/c4or1k-design.md's M16 section.
//
// This is why the extended filesystem is native-only for now: its real
// userland (perl, awk, ...) is the first thing to exercise the FPU.

int fpu_add(int a, int b);
int fpu_sub(int a, int b);
int fpu_mul(int a, int b);
int fpu_div(int a, int b);
int fpu_madd(int d, int a, int b); // lf.madd.s: d + a*b, accumulating into rD
int fpu_itof(int i);               // lf.itof.s: signed int32 -> float bits
int fpu_ftoi(int a);               // lf.ftoi.s: float bits -> int32 (floor, as jor1k)

// Ordered compares on float bits, each returning 0/1 for SR_F.
int fpu_eq(int a, int b);
int fpu_ne(int a, int b);
int fpu_gt(int a, int b);
int fpu_ge(int a, int b);
int fpu_lt(int a, int b);
int fpu_le(int a, int b);

#endif
