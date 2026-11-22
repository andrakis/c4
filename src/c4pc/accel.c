/* accel.c -- libjs/accel.js's loader and C4IX's own accelerators,
 * src/c4ix/accel.mjs: libarc's CRC-32 and its inflater's literal/match
 * loop, which are most of what a boot runs (unpacking the root archive,
 * docs/accel.md 7). c4pc has no game modules and loads none: only the
 * operating system's, "c4ix", the one module, so its operations are
 * ACC_BASE + ACC_SLOT and on. A game's ACC_FIND gets 0, and the game runs
 * its own loops, as with libjs's accelerators off.
 *
 * INFO's C4I_ACCEL says the machine has the loader (machine.accel, off
 * with --no-accel), and ACC_FIND gives "c4ix" only while the OS modules'
 * switch is on (machine.osaccel: --no-osaccel, and the guest's ACC_OS).
 *
 * The operations are accel.mjs's, line for line: the same bytes written,
 * the same state left behind, and where its C would meet an edge they
 * stop before that turn and the C takes it. Guest memory goes through
 * reader and writer (accel.js kit): plain memory as the loop's own loads
 * take it, a task's pages under the MMU (a page it may not use faults, to
 * sys()'s catch in mmu.c, as the program's own load would), or the
 * machine's loads for anything else. Each costs one cycle, as on libjs. */
#include "c4pc.h"
#include "clib.h"

#define ACC_FIND ACC_BASE
#define ACC_OS (ACC_BASE + 1)
#define ACC_SLOT 0x100

/* ---- the kit: readers and writers, each with a page of its own ---- */
typedef struct { machine *m; u32 vp; u32 base; i32 hi; } rw_t;

static void rw_init(rw_t *r, machine *m) {
  r->m = m; r->vp = 0xffffffffu; r->base = 0;
  i32 g = eng_guard_lo(m);
  r->hi = (u32)g < m->size ? g : (i32)m->size;
}
static inline i32 rd8(rw_t *r, i32 x) {
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
static inline void wr8(rw_t *r, i32 x, i32 b) {
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
static inline i32 rd32(rw_t *r, i32 x) {
  machine *m = r->m;
  if (!m->vm && !m->uwin && (x & 3) == 0 && x >= 0x1000 && x < r->hi - 3) return m->w[(u32)x >> 2];
  return m_ld32(m, x);
}
static inline void wr32(rw_t *r, i32 x, i32 v) {
  machine *m = r->m;
  if (!m->vm && !m->uwin && (x & 3) == 0 && x >= 0x1000 && x < r->hi - 3) { m->w[(u32)x >> 2] = v; return; }
  m_st32(m, x, v);
}
/* that the task may use [addr, addr + len), before anything is written */
static void probe(machine *m, i32 addr, i32 len, int write) {
  if (!m->vm || len <= 0) return;
  u32 last = (u32)addr + (u32)len - 1;
  for (u32 x = (u32)addr & ~4095u; ; x += 4096) {
    if ((x >> 12) != 0) mmu_frame(m->mmu, (i32)x, write);
    if (((x ^ last) & ~4095u) == 0) break;
  }
}
/* the n arguments, in the order the program wrote them */
static void args(machine *m, int n, i32 *a) {
  for (int i = 0; i < n; i++) a[i] = m_ld32(m, (i32)((u32)m->sp + 4u * (u32)(n - i)));
}

/* ---- CX_CRC32 (crc, buf, len): arc_crc32. Reads only ---- */
static u32 T[256];
static void crc_table(void) {
  if (T[1]) return;
  for (u32 n = 0; n < 256; n++) {
    u32 c = n;
    for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    T[n] = c;
  }
}
static i32 cx_crc32(machine *m) {
  i32 a[3]; args(m, 3, a);
  i32 crc = a[0], buf = a[1], len = a[2];
  if (len <= 0) return crc;
  probe(m, buf, len, 0);
  crc_table();
  rw_t rd; rw_init(&rd, m);
  u32 c = ~(u32)crc;
  for (i32 i = 0; i < len; i++) c = T[(c ^ (u32)rd8(&rd, (i32)((u32)buf + (u32)i))) & 255] ^ (c >> 8);
  return (i32)~c;
}

/* ---- CX_INFLATE (st): codes(), as far as it safely goes ---- */
static const i32 LBASE[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
  35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const i32 LEXT[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const i32 DBASE[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
  257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const i32 DEXT[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
#define MAXBITS 15
#define FASTMASK 511
#define FASTBITS 9
#define WINDOW 32768
#define MATCHMAX 258
#define INMARGIN 16
#define CHUNK 65536

typedef struct {
  rw_t rin, rout, wout, tab, syms;
  i32 bb, bc, ip, il, ib;
  int ok;
} inf_t;

static inline i32 inb(inf_t *s, i32 i) { return rd8(&s->rin, (i32)((u32)s->ib + (u32)i)); }
/* JavaScript's x << n: the count mod 32, the bits as an int32 */
static inline i32 shl(i32 x, i32 n) { return (i32)((u32)x << (n & 31)); }

/* decode()'s slow path: a long code a bit at a time */
static i32 bits(inf_t *s, i32 need) {
  while (s->bc < need) {
    if (s->ip >= s->il) { s->ok = 0; return 0; }
    s->bb |= shl(inb(s, s->ip++), s->bc);
    s->bc += 8;
  }
  i32 v = s->bb & ((1 << need) - 1);
  s->bb >>= need; s->bc -= need;
  return v;
}
static i32 decode(inf_t *s, i32 fast, i32 count, i32 sym) {
  while (s->bc < FASTBITS && s->ip < s->il) { s->bb |= shl(inb(s, s->ip++), s->bc); s->bc += 8; }
  i32 e = rd32(&s->tab, fast + 4 * (s->bb & FASTMASK));
  if (e && (e & 15) <= s->bc) { i32 l = e & 15; s->bb >>= l; s->bc -= l; return e >> 4; }
  i32 code = 0, first = 0, index = 0;
  for (i32 l = 1; l <= MAXBITS; l++) {
    code |= bits(s, 1);
    if (!s->ok) return -1;
    i32 n = rd32(&s->tab, count + 4 * l);
    if (code - n < first) return rd32(&s->syms, sym + 4 * (index + (code - first)));
    index += n; first += n; first <<= 1; code <<= 1;
  }
  return -10;
}

static i32 cx_inflate(machine *m) {
  i32 a[1]; args(m, 1, a);
  i32 st = a[0];
  inf_t S, *s = &S;
  rw_t r32; rw_init(&r32, m);
  s->bb = rd32(&r32, st); s->bc = rd32(&r32, st + 4); s->ip = rd32(&r32, st + 8);
  s->il = rd32(&r32, st + 12);
  i32 op = rd32(&r32, st + 16);
  i32 oc = rd32(&r32, st + 20); s->ib = rd32(&r32, st + 24); i32 ob = rd32(&r32, st + 28);
  i32 lfast = rd32(&r32, st + 32), dfast = rd32(&r32, st + 36);
  i32 lcount = rd32(&r32, st + 40), lsym = rd32(&r32, st + 44), dcount = rd32(&r32, st + 48), dsym = rd32(&r32, st + 52);
  if (op + MATCHMAX > oc || s->il - s->ip < INMARGIN) return 0;
  /* at most CHUNK of output and of input a time */
  i32 oend = oc < op + CHUNK ? oc : op + CHUNK, iend = s->il < s->ip + CHUNK ? s->il : s->ip + CHUNK;
  i32 wlo = op - WINDOW > 0 ? op - WINDOW : 0;
  probe(m, st, 56, 1);
  probe(m, s->ib + s->ip, iend - s->ip, 0);
  probe(m, ob + wlo, oend - wlo, 1);
  probe(m, lfast, 4 * (FASTMASK + 1), 0); probe(m, dfast, 4 * (FASTMASK + 1), 0);
  probe(m, lcount, 4 * (MAXBITS + 1), 0); probe(m, dcount, 4 * (MAXBITS + 1), 0);
  probe(m, lsym, 4 * 288, 0); probe(m, dsym, 4 * 30, 0);
  rw_init(&s->rin, m); rw_init(&s->rout, m); rw_init(&s->wout, m);
  rw_init(&s->tab, m); rw_init(&s->syms, m);
  s->ok = 1;

  i32 res = 0;
  for (;;) {
    if (op + MATCHMAX > oend || iend - s->ip < INMARGIN) break;
    i32 sbb = s->bb, sbc = s->bc, sip = s->ip;      /* the turn's start, to stop at */
    /* a literal/length symbol */
    if (s->bc < 16 && s->ip + 2 <= s->il) {
      s->bb = s->bb | shl(inb(s, s->ip), s->bc) | shl(inb(s, s->ip + 1), s->bc + 8); s->ip += 2; s->bc += 16;
    }
    i32 e = rd32(&s->tab, lfast + 4 * (s->bb & FASTMASK)), sym, l;
    if (e && (l = e & 15) <= s->bc) { s->bb >>= l; s->bc -= l; sym = e >> 4; }
    else sym = decode(s, lfast, lcount, lsym);
    if (!s->ok || sym < 0) { s->bb = sbb; s->bc = sbc; s->ip = sip; break; }
    if (sym < 256) { wr8(&s->wout, ob + op, sym); op++; continue; }
    if (sym == 256) { res = 1; break; }
    sym -= 257;
    if (sym >= 29) { s->bb = sbb; s->bc = sbc; s->ip = sip; break; }
    i32 len = LBASE[sym];
    if ((l = LEXT[sym])) len += bits(s, l);
    /* its distance */
    if (s->bc < 16 && s->ip + 2 <= s->il) {
      s->bb = s->bb | shl(inb(s, s->ip), s->bc) | shl(inb(s, s->ip + 1), s->bc + 8); s->ip += 2; s->bc += 16;
    }
    e = rd32(&s->tab, dfast + 4 * (s->bb & FASTMASK));
    if (e && (l = e & 15) <= s->bc) { s->bb >>= l; s->bc -= l; sym = e >> 4; }
    else sym = decode(s, dfast, dcount, dsym);
    if (!s->ok || sym < 0 || sym >= 30) { s->bb = sbb; s->bc = sbc; s->ip = sip; break; }
    i32 dist = DBASE[sym];
    if ((l = DEXT[sym])) dist += bits(s, l);
    if (!s->ok || dist > op) { s->bb = sbb; s->bc = sbc; s->ip = sip; break; }
    /* forward, byte by byte: the same bytes whether or not it overlaps */
    i32 from = ob + op - dist, to = ob + op;
    op += len;
    for (; len > 0; len--) wr8(&s->wout, to++, rd8(&s->rout, from++));
  }
  rw_t w; rw_init(&w, m);
  wr32(&w, st, s->bb); wr32(&w, st + 4, s->bc); wr32(&w, st + 8, s->ip); wr32(&w, st + 16, op);
  return res;
}

/* sys(): ACC_FIND, ACC_OS, or an operation; -1 for a number nobody has,
 * which is then a custom opcode like any other */
int accel_op(machine *m, i32 op) {
  if (op == ACC_FIND) {
    char name[34]; int n = 0;
    i32 p = m_ld32(m, (i32)((u32)m->sp + 4));
    while (n < 33) { i32 c = eng_ld8(m, (i32)((u32)p + (u32)n)) & 255; if (!c) break; name[n++] = (char)c; }
    name[n] = 0;
    m->a = m->osaccel && !strcmp(name, "c4ix") ? ACC_BASE + ACC_SLOT : 0;
    return STOP_BUDGET;
  }
  if (op == ACC_OS) {
    /* the OS modules' switch as it was, 1 or 0, and turned (v >= 0); 2
     * more: this machine has an OS module */
    i32 v = m_ld32(m, (i32)((u32)m->sp + 4)), was = m->osaccel ? 1 : 0;
    if (v >= 0) m->osaccel = v > 0;
    m->a = was | 2;
    return STOP_BUDGET;
  }
  switch (op - ACC_BASE - ACC_SLOT) {
    case 0: m->a = cx_crc32(m); return STOP_BUDGET;
    case 1: m->a = cx_inflate(m); return STOP_BUDGET;
  }
  return -1;
}
