/* accel.c -- libjs/accel.js's loader and kit, and C4IX's own accelerators,
 * src/c4ix/accel.mjs: libarc's CRC-32 and its inflater's literal/match
 * loop, which are most of what a boot runs (unpacking the root archive,
 * docs/accel.md 7).
 *
 * The modules are built in, where libjs loads the ones its build lists
 * (src/c4bb/images/accel.json): the games' (games/NAME.c, each its
 * games/NAME/c4/accel.mjs in C) and the operating system's, "c4ix", in the
 * order of the full build's list, so that the k-th module's operation i
 * is ACC_BASE + ACC_SLOT * (k + 1) + i as libjs numbers it there.
 *
 * INFO's C4I_ACCEL says the machine has the loader (machine.accel, off
 * with --no-accel), and ACC_FIND gives "c4ix" only while the OS modules'
 * switch is on (machine.osaccel: --no-osaccel, and the guest's ACC_OS).
 *
 * The operations are the modules' line for line: the same bytes written,
 * the same state left behind, and where the C would meet an edge they
 * stop before that turn and the C takes it. Guest memory goes through
 * accel.h's readers and writers; each operation costs one cycle, as on
 * libjs. */
#include "accel.h"

#define ACC_FIND ACC_BASE
#define ACC_OS (ACC_BASE + 1)
#define ACC_SLOT 0x100

/* ---- the kit ---- */
void acc_probe(machine *m, i32 addr, i32 len, int write) {
  if (!m->vm || len <= 0) return;
  u32 last = (u32)addr + (u32)len - 1;
  for (u32 x = (u32)addr & ~4095u; ; x += 4096) {
    if ((x >> 12) != 0) mmu_frame(m->mmu, (i32)x, write);
    if (((x ^ last) & ~4095u) == 0) break;
  }
}
void acc_args(machine *m, int n, i32 *a) {
  for (int i = 0; i < n; i++) a[i] = m_ld32(m, (i32)((u32)m->sp + 4u * (u32)(n - i)));
}
u8 *acc_span(machine *m, i32 addr, i32 len, int write) {
  if (len <= 0) return 0;
  if (m->vm) {
    if (((u32)addr >> 12) == 0 || (((u32)addr ^ ((u32)addr + (u32)len - 1)) & ~4095u) != 0) return 0;
    u32 p = (u32)mmu_frame(m->mmu, addr, write);
    return m->u8 + (p | ((u32)addr & 4095));
  }
  if (m->uwin) return 0;
  i32 g = eng_guard_lo(m);
  i64 hi = (u32)g < m->size ? g : (i64)m->size;
  if (addr >= 0x1000 && (i64)addr + len <= hi) return m->u8 + addr;
  return 0;
}

hold_t acc_holds[32];
static u32 hash32(i32 a, u32 mask) { u32 x = (u32)a; x ^= x >> 16; x *= 0x45d9f3bu; x ^= x >> 16; return x & mask; }
static void hold_index(hold_t *h, u32 i) {
  u32 k = hash32(h->a[i], h->hmask);
  for (;; k = (k + 1) & h->hmask) {
    if (h->hg[k] != h->g) { h->hg[k] = h->g; h->hs[k] = i + 1; return; }
    if (h->a[h->hs[k] - 1] == h->a[i]) { h->hs[k] = i + 1; return; }
  }
}
static void *grow(void *p, u32 n, u32 sz) { return xrealloc(p, (size_t)n * sz); }
void hold_begin(hold_t *h) {
  h->n = 0;
  if (!h->hs) {
    h->cap = 1024; h->a = grow(0, h->cap, 4); h->v = grow(0, h->cap, 4);
    h->hmask = 2047; h->hs = grow(0, 2048, 4); h->hg = xmalloc(2048 * 4);
  }
  if (++h->g == 0) { memset(h->hg, 0, (size_t)(h->hmask + 1) * 4); h->g = 1; }
}
static void hold_room(hold_t *h) {
  if (h->n < h->cap) return;
  h->cap *= 2;
  h->a = grow(h->a, h->cap, 4); h->v = grow(h->v, h->cap, 4);
  /* the index at twice the writes' room, rebuilt */
  u32 size = h->cap * 2;
  free(h->hs); free(h->hg);
  h->hs = grow(0, size, 4); h->hg = xmalloc((size_t)size * 4); h->hmask = size - 1; h->g = 1;
  for (u32 i = 0; i < h->n; i++) hold_index(h, i);
}
void hold_put(hold_t *h, i32 a, i32 v) {
  hold_room(h);
  h->a[h->n] = a; h->v[h->n] = v;
  hold_index(h, h->n);
  h->n++;
}
static i32 hold_find(hold_t *h, i32 a) {
  u32 k = hash32(a, h->hmask);
  for (;; k = (k + 1) & h->hmask) {
    if (h->hg[k] != h->g) return -1;
    if (h->a[h->hs[k] - 1] == a) return (i32)h->hs[k] - 1;
  }
}
void hold_set(hold_t *h, i32 a, i32 v) {
  i32 i = hold_find(h, a);
  if (i >= 0) { h->v[i] = v; return; }
  hold_put(h, a, v);
}
int hold_get(hold_t *h, i32 a, i32 *v) {
  i32 i = hold_find(h, a);
  if (i < 0) return 0;
  *v = h->v[i];
  return 1;
}
void hold_probe(machine *m, hold_t *h, i32 len) {
  u32 page = 0xffffffffu;
  for (u32 i = 0; i < h->n; i++)
    if (((u32)h->a[i] >> 12) != page) { acc_probe(m, h->a[i], len, 1); page = (u32)h->a[i] >> 12; }
}
void hold_write8(machine *m, hold_t *h) {
  RW(w);
  for (u32 i = 0; i < h->n; i++) wr8(&w, h->a[i], h->v[i]);
}
void hold_write32(machine *m, hold_t *h) {
  RW(w);
  for (u32 i = 0; i < h->n; i++) wr32(&w, h->a[i], h->v[i]);
}
static i32 *scr[32][4]; static u32 scr_n[32][4];
i32 *acc_scratch_k(int cpu, int which, u32 n) {
  if (n > scr_n[cpu][which]) {
    u32 k = scr_n[cpu][which] ? scr_n[cpu][which] : 256;
    while (k < n) k *= 2;
    scr[cpu][which] = grow(scr[cpu][which], k, 4); scr_n[cpu][which] = k;
  }
  return scr[cpu][which];
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
  i32 a[3]; acc_args(m, 3, a);
  i32 crc = a[0], buf = a[1], len = a[2];
  if (len <= 0) return crc;
  acc_probe(m, buf, len, 0);
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
  i32 a[1]; acc_args(m, 1, a);
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
  acc_probe(m, st, 56, 1);
  acc_probe(m, s->ib + s->ip, iend - s->ip, 0);
  acc_probe(m, ob + wlo, oend - wlo, 1);
  acc_probe(m, lfast, 4 * (FASTMASK + 1), 0); acc_probe(m, dfast, 4 * (FASTMASK + 1), 0);
  acc_probe(m, lcount, 4 * (MAXBITS + 1), 0); acc_probe(m, dcount, 4 * (MAXBITS + 1), 0);
  acc_probe(m, lsym, 4 * 288, 0); acc_probe(m, dsym, 4 * 30, 0);
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
static const acc_fn acc_c4ix[] = {cx_crc32, cx_inflate};

/* ---- the modules, in the order of the full build's accel.json ---- */
typedef struct { const char *name; int os; const acc_fn *ops; const int *n; } module;
static const module MODS[] = {
  {"doom", 0, acc_doom, &acc_doom_n},
  {"duke3d", 0, acc_duke3d, &acc_duke3d_n},
  {"quake", 0, acc_quake, &acc_quake_n},
  {"rott", 0, acc_rott, &acc_rott_n},
  {"sbk", 0, acc_sbk, &acc_sbk_n},
  {"tyrian", 0, acc_tyrian, &acc_tyrian_n},
  {"wolf3d", 0, acc_wolf3d, &acc_wolf3d_n},
  {"c4ix", 1, acc_c4ix, 0},
};
#define NMODS (int)(sizeof MODS / sizeof MODS[0])

/* sys(): ACC_FIND, ACC_OS, or an operation; -1 for a number nobody has,
 * which is then a custom opcode like any other */
int accel_op(machine *m, i32 op) {
  if (op == ACC_FIND) {
    char name[34]; int n = 0;
    i32 p = m_ld32(m, (i32)((u32)m->sp + 4));
    while (n < 33) { i32 c = eng_ld8(m, (i32)((u32)p + (u32)n)) & 255; if (!c) break; name[n++] = (char)c; }
    name[n] = 0;
    m->a = 0;
    for (int k = 0; k < NMODS; k++)
      if (!strcmp(name, MODS[k].name)) { if (!MODS[k].os || m->osaccel) m->a = ACC_BASE + ACC_SLOT * (k + 1); break; }
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
  i32 r = op - ACC_BASE - ACC_SLOT;
  if (r < 0 || (r >> 8) >= NMODS) return -1;
  const module *mod = &MODS[r >> 8];
  int nops = mod->n ? *mod->n : (int)(sizeof acc_c4ix / sizeof acc_c4ix[0]);
  if ((r & 255) >= nops) return -1;
  m->a = mod->ops[r & 255](m);
  return STOP_BUDGET;
}
