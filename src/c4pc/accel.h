/* accel.h -- the accelerators' kit, libjs/accel.js's in C: what accel.c
 * and the games' modules (games/NAME.c) read guest memory with.
 *
 * An operation reads its arguments with acc_args (sp+4 upward, the last
 * one nearest), guest memory only through a reader or writer (rw_t: plain
 * memory as the loop's own loads take it, a task's pages under the MMU,
 * or the machine's loads for anything else), and returns what the program
 * gets back (a). Under the MMU a page the task may not use faults, to
 * sys()'s catch in mmu.c, as the program's own load would; the
 * instruction runs again from the start once the kernel has seen to it.
 * So an operation gives the same result run again after a partial run, or
 * first makes sure with acc_probe that nothing it writes can fault: a
 * hold_t keeps the writes of one that reads what it writes.
 *
 * The arithmetic is JavaScript's as the modules (games/NAME/c4/accel.mjs)
 * have it: int32 wrapping (the build's -fwrapv), shifts with their count
 * mod 32 (SHL, SAR, LSR), and a single's operations done in double and
 * rounded with fr(), which is Math.fround: on the PC the x87 rounds to
 * double (pc/pc.c), and a double's +, -, * and / rounded to single are
 * the single's own. */
#ifndef C4PC_ACCEL_H
#define C4PC_ACCEL_H
#include "c4pc.h"
#include "clib.h"

typedef uint64_t u64;

/* ---- readers and writers, each with a page of its own ----
 * The kit is the 64-bit machine's too (C4IX's own module, accel.c), with
 * addresses in words; the MMU paths are the 32-bit machine's alone, and
 * so are the games' modules (#if WB == 4 below) */
typedef struct { machine *m; u32 vp; u32 base; word hi; } rw_t;

static inline void rw_init(rw_t *r, machine *m) {
  r->m = m; r->vp = 0xffffffffu; r->base = 0;
  word g = eng_guard_lo(m);
  r->hi = (uword)g < m->size ? g : (word)m->size;
}
static inline i32 rd8(rw_t *r, word x) {
  machine *m = r->m;
  if (m->vm) {
    u32 v = (u32)x >> 12;
    if (v != r->vp) {
      if (v == 0) return eng_ld8(m, x) & 255;
      r->base = (u32)mmu_frame(m->mmu, x, 0);
      r->vp = v;
    }
    return m->u8[r->base | ((u32)x & 4095)];
  }
  if (!m->uwin && x >= 0x1000 && x < r->hi) return m->u8[x];
  return eng_ld8(m, x) & 255;
}
static inline void wr8(rw_t *r, word x, i32 b) {
  machine *m = r->m;
  if (m->vm) {
    u32 v = (u32)x >> 12;
    if (v != r->vp) {
      if (v == 0) { eng_st8(m, x, b); return; }
      r->base = (u32)mmu_frame(m->mmu, x, 1);
      r->vp = v;
    }
    m->u8[r->base | ((u32)x & 4095)] = (u8)b;
    return;
  }
  if (!m->uwin && x >= 0x1000 && x < r->hi) { m->u8[x] = (u8)b; return; }
  eng_st8(m, x, b);
}
/* a word: 4 bytes on the 32-bit machine (rd32 and wr32, the games'
 * name for it), 8 on the 64-bit */
static inline word rdw(rw_t *r, word x) {
  machine *m = r->m;
  if (!m->vm && !m->uwin && (x & (WB - 1)) == 0 && x >= 0x1000 && x < r->hi - (WB - 1)) return m->w[(uword)x >> WSH];
  return m_ldw(m, x);
}
static inline void wrw(rw_t *r, word x, word v) {
  machine *m = r->m;
  if (!m->vm && !m->uwin && (x & (WB - 1)) == 0 && x >= 0x1000 && x < r->hi - (WB - 1)) { m->w[(uword)x >> WSH] = v; return; }
  m_stw(m, x, v);
}
#define rd32 rdw
#define wr32 wrw
/* a reader or writer of its own, as accel.mjs's `const rs = reader(m)` */
#define RW(name) rw_t name; rw_init(&name, m)

/* that the task may use [addr, addr + len), before anything is written */
void acc_probe(machine *m, word addr, word len, int write);
/* the n arguments, in the order the program wrote them */
void acc_args(machine *m, int n, word *a);
/* [addr, addr + len) as the machine's own bytes when they are one run of
 * them (plain memory, or under the MMU a stretch within one page the task
 * may read, or write), else 0 and the operation goes byte by byte */
u8 *acc_span(machine *m, i32 addr, i32 len, int write);

/* ---- JavaScript's arithmetic ---- */
#define SHL(x, n) ((i32)((u32)(x) << ((n) & 31)))
#define SAR(x, n) ((i32)(x) >> ((n) & 31))
#define LSR(x, n) ((u32)(x) >> ((n) & 31))
#define IMUL(a, b) ((i32)((u32)(a) * (u32)(b)))

float f32_of(i32 bits);
i32 bits_of_f32(float f);
/* Math.fround */
static inline double fr(double d) { volatile float f = (float)d; return f; }
/* a float's bits as the float, and a value as a float's bits */
static inline double fval(i32 u) { return f32_of(u); }
static inline i32 fbits(double x) { return bits_of_f32((float)fr(x)); }
/* a conversion to int that does not fit gives the most negative int, as
 * c4's FPU and SSE do */
static inline i32 toint(double v) {
  if (v >= -2147483648.0 && v < 2147483648.0) return (i32)v;
  return (i32)0x80000000u;
}
/* sin and cos as the FPU opcode has them (fpu.c) */
double fpu_sin(double x);
double fpu_cos(double x);

/* an operation: what the program gets back */
typedef word (*acc_fn)(machine *m);

#if WB == 4                             /* the games' kit: the 32-bit machine's */

/* ---- writes held back ----
 * An operation that reads the screen it writes (a blend, a shadow) works
 * out every pixel first, then makes sure of every page it will write, and
 * only then writes: a fault comes before the first write. hold_put keeps
 * every write in order (accel.mjs's wa and wv) and hold_get finds the
 * last one to an address (its Map); hold_set keeps one write an address,
 * in the order they were first made (a Map's keys). One hold, since one
 * operation runs at a time on a CPU: a hold each (par.c runs them at once). */
typedef struct {
  i32 *a, *v; u32 n, cap;        /* the writes */
  u32 *hs, *hg; u32 hmask, g;    /* address -> index + 1, by generation */
} hold_t;
extern hold_t acc_holds[32];
#define acc_hold (acc_holds[m->acc_k])    /* the running processor's: m is the operation's machine */
void hold_begin(hold_t *h);
void hold_put(hold_t *h, i32 a, i32 v);
void hold_set(hold_t *h, i32 a, i32 v);
int hold_get(hold_t *h, i32 a, i32 *v);
/* every page written, a probe each where the page changes; len the bytes
 * of each write */
void hold_probe(machine *m, hold_t *h, i32 len);
void hold_write8(machine *m, hold_t *h);
void hold_write32(machine *m, hold_t *h);
/* a scratch of n words of the module's own (grown, never shrunk) */
i32 *acc_scratch_k(int cpu, int which, u32 n);
#define acc_scratch(which, n) acc_scratch_k(m->acc_k, (which), (n))    /* ... the running processor's */

#define ACC_MODULE(name) extern const acc_fn acc_##name[]; extern const int acc_##name##_n
ACC_MODULE(doom); ACC_MODULE(duke3d); ACC_MODULE(quake); ACC_MODULE(rott);
ACC_MODULE(sbk); ACC_MODULE(tyrian); ACC_MODULE(wolf3d);
#endif /* WB == 4 */
#endif
