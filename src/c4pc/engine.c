/* engine.c -- the c4m interpreter: libjs/c4m.js's Machine (one CPU, no
 * display or sound) and boot.js, in C. The MMU's user loop is mmu.c's;
 * kernel mode under it runs here, reaching user addresses through the
 * user window (kpa).
 *
 * Words are int32, addresses byte offsets into the arena. Arithmetic
 * wraps as JavaScript's | 0 does; a shift takes its count mod 32; DIV and
 * MOD by 0 give 0, and INT_MIN / -1 gives INT_MIN, as they do in libjs.
 * One difference: libjs's typed arrays read 0 past the arena and drop the
 * write, where here a word index wraps inside it (wmask). Only a wild
 * program can tell.
 */
#include "c4pc.h"
#include "clib.h"

const char OPNAMES[] =
  "LEA ,IMM ,JMP ,JSR ,BZ  ,BNZ ,ENT ,ADJ ,LEV ,LI  ,LC  ,SI  ,SC  ,PSH ,"
  "OR  ,XOR ,AND ,EQ  ,NE  ,LT  ,GT  ,LE  ,GE  ,SHL ,SHR ,ADD ,SUB ,MUL ,DIV ,MOD ,"
  "OPEN,READ,CLOS,PRTF,MALC,FREE,MSET,MCMP,EXIT,"
  "PUTC,PUTS,RALC,MCPY,STRC,ITH ,_OPC,_BLT,_TRP,OPCD,"
  "_JMP,_ADJ,C4CF,C4CY,TIME,SIGH,SIGI,USLP,INFO,OPSL,"
  "C4IV,FLT ,JSRI,JSRS,JMPA,TLEV,DBG ,"
  "CPUI,CPUN,CPUS,CPUH,"
  "CAS ,XCHG,FADD,CWAI,CWAK,IPI ,"
  "LXI ,SXI ,TRAW,"
  "LDL ,LDG ,PSHL,PSHG,LEAP,IMMP,LIP ,ADDL,STL ,POPA,"
  "FPU ,"
  "LH  ,LHU ,SH  ,LSR ,LCU ,";
#define OPNAMES_LEN ((u32)sizeof OPNAMES - 1)
#define TRAP_OFFSET 15
#define ROM_LOW_END 0x20
#define TIME_MS_REG 0x10c
#define USLP_US_REG 0x118
#define DISK_CLOSE_REG 0x130
#define HEAP_BASE_REG 0x144
#define HEAP_END_REG 0x148

#define W(x) m->w[((u32)(x) >> 2) & m->wmask]
_Static_assert(sizeof OPNAMES - 1 == INS_SIZE * 5, "is_rom (c4pc.h) counts the names as INS_SIZE of 5 bytes");

void machine_emit(machine *m, const u8 *b, u32 n) {
  m->uart_activity += n;
  dev_con_bytes(m, b, n);
}
void machine_emits(machine *m, const char *s) { machine_emit(m, (const u8 *)s, (u32)strlen(s)); }

static char *hex8(char *buf, u32 v) {
  static const char h[] = "0123456789ABCDEF";
  buf[0] = '0'; buf[1] = 'x';
  for (int i = 0; i < 8; i++) buf[2 + i] = h[(v >> (28 - 4 * i)) & 15];
  buf[10] = 0;
  return buf;
}
static char *dec(char *buf, i64 v) {
  char t[24]; int n = 0, k = 0;
  int neg = v < 0; uint64_t u = neg ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
  do { t[n++] = (char)('0' + u % 10); u /= 10; } while (u);
  if (neg) buf[k++] = '-';
  while (n) buf[k++] = t[--n];
  buf[k] = 0;
  return buf;
}

/* ---------------------------------------------------------------- */
/* memory, the slow paths (c4m.js ld32 ...): the device window, the ROM,
 * unaligned words, the guarded boot stack */
static int in_guard(machine *m, i32 x, i32 n) {
  return m->stack_guard ? (i64)x < m->stack_top_reg && (i64)x + n > m->stack_base : 0;
}
static i32 guard_lo(machine *m) {
  i32 g = m->stack_guard ? (i32)m->stack_base : 0x7fffffff;
  /* the loop's own bound: below the arena's end, so its fast paths need no other */
  i32 e = (i32)m->size - 4;
  if (e < g) g = e;
  /* the user window too: an address in it is a user task's, which kernel
   * mode reaches through kpa */
  return m->uwin && (i64)m->uwin < g ? (i32)m->uwin : g;
}
static void guard_hit(machine *m, i32 x, i32 n, int write, i32 v, i32 op, i32 from);
static void rom_store(machine *m, i32 x, i32 v);

static i32 arena_rd32(machine *m, u32 a) {
  if (a >= DEV_BASE && a < DEV_END) return dev_read32(m, a);
  if (!(a & 3)) return a + 4 <= m->size ? m->w[a >> 2] : 0;
  if ((i64)a + 4 > m->size) return 0;
  return (i32)(m->u8[a] | m->u8[a + 1] << 8 | m->u8[a + 2] << 16 | (u32)m->u8[a + 3] << 24);
}
static void arena_wr32(machine *m, u32 a, i32 v) {
  if (a >= DEV_BASE && a < DEV_END) { dev_write32(m, a, v); return; }
  if (!(a & 3)) { if (a + 4 <= m->size) m->w[a >> 2] = v; return; }
  if ((i64)a + 4 > m->size) return;
  m->u8[a] = (u8)v; m->u8[a + 1] = (u8)(v >> 8); m->u8[a + 2] = (u8)(v >> 16); m->u8[a + 3] = (u8)((u32)v >> 24);
}
/* bus.js: below 0x10 reads 0; then the arena's (no display, no sound) */
static i32 bus_rd32(machine *m, u32 a) { return a < 0x10 ? 0 : arena_rd32(m, a); }
static i32 bus_rd8s(machine *m, u32 a) { return a < 0x10 || a >= m->size ? 0 : (int8_t)m->u8[a]; }
static void bus_wr8(machine *m, u32 a, i32 v) {
  if (a >= DEV_BASE && a < DEV_END) { dev_write32(m, a & ~3u, v & 0xff); return; }
  if (a < m->size) m->u8[a] = (u8)v;
}

i32 eng_bus_rd32(machine *m, u32 a) { return bus_rd32(m, a); }
i32 eng_bus_rd8s(machine *m, u32 a) { return bus_rd8s(m, a); }
void eng_bus_wr32(machine *m, u32 a, i32 v) { arena_wr32(m, a, v); }
void eng_bus_wr8(machine *m, u32 a, i32 v) { bus_wr8(m, a, v); }
void eng_rom_store(machine *m, i32 x, i32 v) { rom_store(m, x, v); }

static i32 kld32(machine *m, u32 x);
static void kst32(machine *m, u32 x, i32 v);
static i32 kld8(machine *m, u32 x);
static void kst8(machine *m, u32 x, i32 v);

i32 m_ld32(machine *m, i32 x) {
  if (m->vm) return mmu_ld32(m->mmu, x);
  if (m->uwin && (u32)x >= m->uwin) return kld32(m, (u32)x);
  if ((x & 3) == 0 && x >= 0x1000) {
    if (in_guard(m, x, 4)) { guard_hit(m, x, 4, 0, 0, 0, 0); return 0; }
    return (u32)x + 4 <= m->size ? m->w[(u32)x >> 2] : 0;
  }
  if (m->mmu && x >= MMU_BASE && x < MMU_END) return mmu_read32(m->mmu, x);
  return bus_rd32(m, (u32)x);
}
void m_st32(machine *m, i32 x, i32 v) {
  if (m->vm) { mmu_st32(m->mmu, x, v); return; }
  if (m->uwin && (u32)x >= m->uwin) { kst32(m, (u32)x, v); return; }
  if ((x & 3) == 0 && x >= 0x1000) {
    if (in_guard(m, x, 4)) { guard_hit(m, x, 4, 1, v, 0, 0); return; }
    if ((u32)x + 4 <= m->size) m->w[(u32)x >> 2] = v;
    return;
  }
  if (m->mmu && x >= MMU_BASE && x < MMU_END) { mmu_write32(m->mmu, x, v); return; }
  if (is_rom(x, 4)) { rom_store(m, x, v); return; }
  arena_wr32(m, (u32)x, v);
}
static i32 m_ld8(machine *m, i32 x) {
  if (m->vm) return mmu_ld8(m->mmu, x);
  if (m->uwin && (u32)x >= m->uwin) return kld8(m, (u32)x);
  if (x >= 0x1000) {
    if (in_guard(m, x, 1)) { guard_hit(m, x, 1, 0, 0, 0, 0); return 0; }
    return (u32)x < m->size ? (int8_t)m->u8[x] : 0;
  }
  return bus_rd8s(m, (u32)x);
}
static void m_st8(machine *m, i32 x, i32 v) {
  if (m->vm) { mmu_st8(m->mmu, x, v); return; }
  if (m->uwin && (u32)x >= m->uwin) { kst8(m, (u32)x, v); return; }
  if (x >= 0x1000) {
    if (in_guard(m, x, 1)) { guard_hit(m, x, 1, 1, v, 0, 0); return; }
    if ((u32)x < m->size) m->u8[x] = (u8)v;
    return;
  }
  if (is_rom(x, 1)) { rom_store(m, x, v & 0xff); return; }
  bus_wr8(m, (u32)x, v & 0xff);
}
static i32 m_ld16(machine *m, i32 x) {
  i32 lo = m_ld8(m, x) & 255;
  return lo | (i32)((u32)m_ld8(m, (i32)((u32)x + 1)) << 8);
}
static void m_st16(machine *m, i32 x, i32 v) { m_st8(m, x, v); m_st8(m, (i32)((u32)x + 1), v >> 8); }

/* ---------------------------------------------------------------- */
/* the user window (c4m.js kpa ...): under C4IX's MMU a user task's memory
 * is at user addresses from uwin, mapped in its page tables; kernel mode
 * runs untranslated and reaches a user address through the window --
 * where udir, the kernel's map of all of them, says it is, or with no
 * map, the page uwin below it. A page the map has put out to swap is -1
 * here, and its bytes are the host's copy (kswap). kpa: physical, or -1 */
static void kmiss(machine *m, u32 x);
static int halt(machine *m, i32 status);
static i32 kswap(machine *m, u32 x) {
  if (!m->udir) return -1;
  i32 pte = kernel_pte(m, m->udir, x);
  return (pte & 1) || !(pte & PTE_SWAP) ? -1 : (i32)((u32)pte >> 12);
}
static i64 kpa(machine *m, u32 x) {
  if (m->udir) {
    i64 p = kernel_phys(m, m->udir, x, 0);
    if (p < 0 && kswap(m, x) < 0) kmiss(m, x);
    return p;
  }
  u32 off = x - m->uwin;
  return off < m->size ? (i64)off : -1;
}
static void kmiss(machine *m, u32 x) {
  if (m->halted) return;
  char a[12], b[12];
  machine_emits(m, "libjs: the kernel used user address "); machine_emits(m, hex8(a, x));
  machine_emits(m, ", which nothing is at (pc "); machine_emits(m, hex8(b, (u32)m->pc));
  machine_emits(m, "). Halting.\n");
  halt(m, -105);
}
static i64 kaddr(machine *m, u32 x) { return m->uwin && x >= m->uwin ? kpa(m, x) : (i64)x; }
/* the host's copy of a page put out (devices.js swapRd8, swapWr8) */
static int swap_rd8(machine *m, i32 slot, u32 off) {
  u8 *p = imap_get(m->swap_pages, (u32)slot);
  return p ? p[off & 4095] : 0;
}
static void swap_wr8(machine *m, i32 slot, u32 off, int v) {
  u8 *p = imap_get(m->swap_pages, (u32)slot);
  if (!p) { if ((u32)slot >= m->swap_slots) return; p = xmalloc(4096); imap_set(m->swap_pages, (u32)slot, p); }
  p[off & 4095] = (u8)v;
}
static i32 kld8(machine *m, u32 x) {
  i64 p = kpa(m, x);
  if (p >= 0) return (int8_t)m->u8[p];
  i32 s = kswap(m, x);
  return s < 0 ? 0 : (int8_t)swap_rd8(m, s, x & 4095);
}
static void kst8(machine *m, u32 x, i32 v) {
  i64 p = kpa(m, x);
  if (p >= 0) { m->u8[p] = (u8)v; return; }
  i32 s = kswap(m, x);
  if (s >= 0) swap_wr8(m, s, x & 4095, v & 255);
}
static i32 kbytes32(machine *m, u32 x) {
  i32 b0 = kld8(m, x) & 255, b1 = kld8(m, x + 1) & 255, b2 = kld8(m, x + 2) & 255, b3 = kld8(m, x + 3);
  return (i32)((u32)b0 | (u32)b1 << 8 | (u32)b2 << 16 | (u32)b3 << 24);
}
static i32 kld32(machine *m, u32 x) {
  i64 p = kpa(m, x);
  if (p < 0) return kswap(m, x) < 0 ? 0 : kbytes32(m, x);
  if ((p & 3) == 0) return m->w[p >> 2];
  return kbytes32(m, x);
}
static void kst32(machine *m, u32 x, i32 v) {
  i64 p = kpa(m, x);
  if (p >= 0 && (p & 3) == 0) { m->w[p >> 2] = v; return; }
  if (p < 0 && kswap(m, x) < 0) return;
  for (int k = 0; k < 4; k++) kst8(m, x + (u32)k, v >> (8 * k));
}
/* does the block opcode at the top of the stack touch the window? */
static int kwin(machine *m, i32 op) {
  i32 sp = m->sp; u32 w = m->uwin; i32 n = m_ld32(m, sp);
  if (n <= 0) return 0;
  u32 d = (u32)m_ld32(m, sp + 8);
  if (d >= w || d + (u32)n > w) return 1;
  if (op == OP_MSET) return 0;
  u32 s = (u32)m_ld32(m, sp + 4);
  return s >= w || s + (u32)n > w;
}
/* MSET, MCMP and MCPY in kernel mode with a user address among them: a
 * run at a time that stays inside a page of each */
static u32 krun(u32 x, u32 y, i32 left) {
  i64 r = left;
  if (4096 - (x & 4095) < r) r = 4096 - (x & 4095);
  if (4096 - (y & 4095) < r) r = 4096 - (y & 4095);
  return (u32)r;
}
static int kblock(machine *m, i32 op) {
  i32 sp = m->sp; u8 *u = m->u8;
  if (op == OP_MSET) {
    i32 d = m_ld32(m, sp + 8), v = m_ld32(m, sp + 4) & 0xff, n = m_ld32(m, sp);
    for (i32 k = 0; k < n;) {
      u32 x = (u32)d + (u32)k, r = krun(x, x, n - k);
      i64 p = kaddr(m, x);
      if (p >= 0x1000 && p + r <= m->size) memset(u + p, v, r);
      else for (u32 j = 0; j < r; j++) m_st8(m, (i32)(x + j), v);   /* a page put out: the host's copy */
      k += (i32)r;
    }
    m->a = d;
    return STOP_BUDGET;
  }
  if (op == OP_MCMP) {
    i32 p = m_ld32(m, sp + 8), q = m_ld32(m, sp + 4), n = m_ld32(m, sp);
    i32 r = 0;
    for (i32 k = 0; k < n; k++) {
      i32 x = (m_ld8(m, (i32)((u32)p + (u32)k)) & 255) - (m_ld8(m, (i32)((u32)q + (u32)k)) & 255);
      if (x) { r = x; break; }
    }
    m->a = r;
    return STOP_BUDGET;
  }
  i32 d = m_ld32(m, sp + 8), s = m_ld32(m, sp + 4), n = m_ld32(m, sp);
  if (d > s && (i64)d < (i64)s + n) {
    for (i32 k = 0; k < n; k++) m_st8(m, (i32)((u32)d + (u32)k), m_ld8(m, (i32)((u32)s + (u32)k)));
  } else {
    for (i32 k = 0; k < n;) {
      u32 x = (u32)d + (u32)k, y = (u32)s + (u32)k, r = krun(x, y, n - k);
      i64 pd = kaddr(m, x), ps = kaddr(m, y);
      if (pd >= 0x1000 && ps >= 0 && pd + r <= m->size && ps + r <= m->size) memmove(u + pd, u + ps, r);
      else for (u32 j = 0; j < r; j++) m_st8(m, (i32)(x + j), m_ld8(m, (i32)(y + j)));
      k += (i32)r;
    }
  }
  m->a = d;
  return STOP_BUDGET;
}

static void rom_store(machine *m, i32 x, i32 v) {
  m->rom_stores++;
  if (m->rom_reported) return;
  m->rom_reported = 1;
  char a[12], b[12], c[12], d[12], e[24], f[12];
  machine_emits(m, "libjs: store of "); machine_emits(m, hex8(a, (u32)v));
  machine_emits(m, " to ROM "); machine_emits(m, hex8(b, (u32)x));
  machine_emits(m, " at pc "); machine_emits(m, hex8(c, (u32)(m->pc - 4)));
  machine_emits(m, " dropped (bp "); machine_emits(m, hex8(d, (u32)m->bp));
  machine_emits(m, ", sp "); machine_emits(m, hex8(f, (u32)m->sp));
  machine_emits(m, ", mode "); machine_emits(m, dec(e, m->mode));
  machine_emits(m, ", cycle "); machine_emits(m, dec(e, m->cycle));
  machine_emits(m, "). Later ones are dropped silently.\n");
}

static void guard_hit(machine *m, i32 x, i32 n, int write, i32 v, i32 op, i32 from) {
  char a[12], b[12], c[12], d[24];
  u32 lo = m->stack_base, hi = m->stack_top_reg;
  m->stack_guard = 0;
  machine_emits(m, "libjs: ");
  if (op == OP_TLEV) { machine_emits(m, "TLEV to a stack at "); machine_emits(m, hex8(a, (u32)x)); }
  else if (op) {
    char nm[5]; memcpy(nm, OPNAMES + op * 5, 4); nm[4] = 0;
    for (int k = 3; k >= 0 && nm[k] == ' '; k--) nm[k] = 0;
    machine_emits(m, nm); machine_emits(m, " of "); machine_emits(m, dec(d, n));
    machine_emits(m, " bytes at "); machine_emits(m, hex8(a, (u32)from));
    machine_emits(m, write ? " writes " : " reads "); machine_emits(m, hex8(b, (u32)x));
  } else if (write) { machine_emits(m, "write of "); machine_emits(m, hex8(a, (u32)v)); machine_emits(m, " to "); machine_emits(m, hex8(b, (u32)x)); }
  else { machine_emits(m, n == 1 ? "read of a byte at " : "read of a word at "); machine_emits(m, hex8(a, (u32)x)); }
  machine_emits(m, ", at pc "); machine_emits(m, hex8(c, (u32)(m->pc - 4)));
  machine_emits(m, ": that is the boot stack, guarded ("); machine_emits(m, hex8(a, lo));
  machine_emits(m, "-"); machine_emits(m, hex8(b, hi - 1)); machine_emits(m, ").\n");
  machine_emits(m, "libjs:   Something ran off the end of the heap -- a program writing past its block? Halting.\n");
  machine_emits(m, "libjs:   bp "); machine_emits(m, hex8(a, (u32)m->bp));
  machine_emits(m, ", sp "); machine_emits(m, hex8(b, (u32)m->sp));
  machine_emits(m, ", a "); machine_emits(m, hex8(c, (u32)m->a));
  machine_emits(m, ", mode "); machine_emits(m, dec(d, m->mode));
  machine_emits(m, ", after "); machine_emits(m, dec(d, m->cycle)); machine_emits(m, " cycles.\n");
  if (!m->heap_base_reg || (u32)(m->pc - 4) < (u32)m->heap_base_reg) machine_stacktrace(m);
  else machine_emits(m, "libjs:   pc is in the heap, a program's code: no symbols name it.\n");
  m->halted = 1;
  m->status = -103;
}
static int guard_range(machine *m, i32 op, i32 x, i32 n, int write) {
  guard_hit(m, x > (i32)m->stack_base ? x : (i32)m->stack_base, n, write, 0, op, x);
  return STOP_HALT;
}

/* the bytes of a C string at addr, in place: how many (to the arena's end at most) */
static u32 cstr(machine *m, u32 addr, u32 max, const u8 **p) {
  if (m->vm || (m->uwin && addr >= m->uwin)) {
    /* a copy, a byte at a time: in user mode translated (a fault goes to
     * the user loop's catch), in the window through kpa (c4m.js cstring).
     * PRTF holds its format while it asks for its %s strings: a few
     * buffers, taken in turn */
    static obuf bufs[8]; static int next;
    obuf *o = &bufs[next++ & 7];
    o->n = 0;
    for (u32 k = 0; k < max; k++) {
      int b = (m->vm ? mmu_ld8(m->mmu, (i32)(addr + k)) : kld8(m, addr + k)) & 255;
      if (!b) break;
      ob_put(o, b);
    }
    *p = o->b ? o->b : m->u8;
    return o->n;
  }
  if (addr >= m->size) { *p = m->u8; return 0; }
  u32 lim = m->size - addr < max ? m->size - addr : max, n = 0;
  while (n < lim && m->u8[addr + n]) n++;
  *p = m->u8 + addr;
  return n;
}
static u32 cstr_cb(void *ctx, u32 addr, const u8 **p) { return cstr((machine *)ctx, addr, 1u << 20, p); }

/* ---------------------------------------------------------------- */
/* traps (c4m.js trap): false when there is no handler, and nothing moves */
static int trap(machine *m, i32 type, i32 param, i32 handler) {
  if (!handler) { m->missed_traps++; return 0; }
  i32 t = m->sp;
  i32 sp = t - TRAP_OFFSET * 4;
  /* With the MMU on, a trap is the way into kernel mode: from user mode
   * it switches to the kernel's stack (KSP), as an x86 does, and the
   * handler runs untranslated */
  mmu *u = m->mmu;
  int kern = u && u->on;
  if (kern) {
    if (m->mode && u->ksp) sp = u->ksp - TRAP_OFFSET * 4;
    m->vm = 0;
  }
#define PUSHW(v) (sp -= 4, W(sp) = (v))
  PUSHW(m->cycle_interval);            /* bp+9 */
  m->cycle_interval = 0;
  PUSHW(type);                         /* bp+8 */
  PUSHW(param);                        /* bp+7 */
  PUSHW(m->mode);                      /* bp+6 */
  PUSHW(m->a);                         /* bp+5 */
  PUSHW(m->bp);                        /* bp+4 */
  PUSHW(t);                            /* bp+3 */
  PUSHW(m->pc);                        /* bp+2 */
  PUSHW(TLEV_ADDR);                    /* bp+1 */
  sp -= 4;
  W(sp) = sp;                          /* bp+0 */
#undef PUSHW
  m->bp = sp;
  sp -= m_ld32(m, handler + 4) * 4;
  m->sp = sp;
  m->pc = handler + 8;
  m->trap_pc = m->pc;
  if (kern) m->mode = 0;
  m->irq_hold++;
  return 1;
}
int eng_trap(machine *m, i32 type, i32 param, i32 handler) { return trap(m, type, param, handler); }
static void hirq(machine *m, i32 param) {
  trap(m, TRAP_HARD_IRQ, param, m->cycle_handler);
  m->cycle_interval = 0;
  m->mode = 0;
}
static void deliver_signal(machine *m) {
  i32 sig = m->pending_signal;
  trap(m, TRAP_SIGNAL, sig, sig >= 0 && sig < 64 ? m->sigh[sig] : 0);
  m->cycle_interval = 0;
  m->mode = 0;
  m->pending_signal = 0;
}
static int pm_trap(machine *m, i32 op) {
  trap(m, TRAP_PM, op, m->trap_handler);
  m->cycle_interval = 0;
  m->mode = 0;
  return STOP_BUDGET;
}
static int halt(machine *m, i32 status) {
  m->halted = 1;
  m->status = status;
  return STOP_HALT;
}
int eng_halt(machine *m, i32 status) { return halt(m, status); }

i32 machine_info(machine *m) {
  return C4I_C4M | C4I_C4MJS | C4I_HRT | C4I_SIG | C4I_FLT | C4I_PROT | C4I_PIT | C4I_DRV |
         (m->no_fpu ? 0 : C4I_FPU) | (m->mmu ? C4I_MMU : 0) | (m->accel ? C4I_ACCEL : 0) | (m->trap_handler ? C4I_TRAPH : 0);
}

void machine_stacktrace(machine *m) {
  char a[12];
  i32 pc = m->pc - 4, bp = m->bp;
  machine_emits(m, "c4m: stacktrace:\n");
  for (int depth = 0; depth < 64; depth++) {
    struct sym *s = 0;
    int lo = 0, hi = m->nsyms - 1;
    while (lo <= hi) { int mid = (lo + hi) >> 1; if (m->syms[mid].addr <= (u32)pc) { s = &m->syms[mid]; lo = mid + 1; } else hi = mid - 1; }
    for (int k = 0; k <= depth; k++) machine_emits(m, " ");
    machine_emits(m, s ? s->name : "?");
    machine_emits(m, "() [");
    { char *h = hex8(a, s ? s->addr : (u32)pc); /* c4m.js: no padding */ while (h[2] == '0' && h[3]) { memmove(h + 2, h + 3, strlen(h + 3) + 1); } machine_emits(m, h); }
    machine_emits(m, "]\n");
    if (!s || !strcmp(s->name, "main") || bp < 0x1000 || (u32)bp >= m->size) break;
    pc = m_ld32(m, bp + 4) - 4;
    bp = m_ld32(m, bp);
  }
}

static int null_jump(machine *m, i32 pc) {
  char a[12], b[12], c[12], d[24];
  i32 ret = m_ld32(m, m->sp);
  machine_emits(m, "libjs: jump to "); machine_emits(m, hex8(a, (u32)pc));
  machine_emits(m, ", the null guard: nothing runs there. Halting.\n");
  machine_emits(m, "libjs:   bp "); machine_emits(m, hex8(a, (u32)m->bp));
  machine_emits(m, ", sp "); machine_emits(m, hex8(b, (u32)m->sp));
  machine_emits(m, ", a "); machine_emits(m, hex8(c, (u32)m->a));
  machine_emits(m, ", mode "); machine_emits(m, dec(d, m->mode));
  machine_emits(m, ", after "); machine_emits(m, dec(d, m->cycle));
  machine_emits(m, " cycles; top of stack "); machine_emits(m, hex8(a, (u32)ret));
  machine_emits(m, ", the return address if it was a call.\n");
  if (ret >= 0x1000 && (u32)ret < m->size) { i32 at = m->pc; m->pc = ret; machine_stacktrace(m); m->pc = at; }
  return halt(m, -102);
}
static int tlev_wedge(machine *m, i32 pc) {
  char a[12], b[12], c[12], d[24];
  machine_emits(m, "libjs: TLEV at "); machine_emits(m, hex8(a, (u32)pc));
  machine_emits(m, " restores its own address -- this task can never advance.\n");
  machine_emits(m, "libjs:   bp "); machine_emits(m, hex8(a, (u32)m->bp));
  machine_emits(m, ", sp "); machine_emits(m, hex8(b, (u32)m->sp));
  machine_emits(m, ", a "); machine_emits(m, hex8(c, (u32)m->a));
  machine_emits(m, ", after "); machine_emits(m, dec(d, m->cycle)); machine_emits(m, " cycles.\n");
  machine_emits(m, "libjs:   A trap frame was built wrong or written over. Halting.\n");
  return halt(m, -101);
}

/* READ into a buffer over the ROM: put the ROM back afterwards */
static i32 rom_read(machine *m, i32 len) {
  u32 end = OPNAMES_ROM + OPNAMES_LEN;
  u8 *saved = xmalloc(end);
  memcpy(saved, m->u8, end);
  i32 r = dev_disk_read(m, len);
  for (u32 x = 0; x < end; x++) {
    if (x == ROM_LOW_END) x = OPNAMES_ROM;
    if (m->u8[x] != saved[x]) { rom_store(m, (i32)x, m->u8[x]); m->u8[x] = saved[x]; }
  }
  xfree(saved);
  return r;
}

static int blocked_read(machine *m) {
  m->pc -= 4;
  i32 iv = m->cycle_interval;
  if (iv > 0) {
    i64 target = (m->cycle / iv + 1) * iv - 1;
    if (target > m->cycle) { m->skipped += target - m->cycle; m->cycle = target; }
    return STOP_BUDGET;
  }
  if (m->pit_ms && plat_now_ms() >= m->pit_next) {
    i64 target = m->cycle + (4095 - (m->cycle % 4096));
    if (target > m->cycle) { m->skipped += target - m->cycle; m->cycle = target; }
    return STOP_BUDGET;
  }
  return STOP_INPUT;
}

static void opv_note(machine *m, i32 op) {
  char a[5], b[6];
  memcpy(a, OPNAMES + OP_OPCD * 5, 4); a[4] = 0;
  if (op >= 0 && op < INS_SIZE) { memcpy(b, OPNAMES + op * 5, 5); b[4] = 0; } else { b[0] = '?'; b[1] = 0; }
  machine_emits(m, a); machine_emits(m, " does not support opcodes requiring arguments (");
  machine_emits(m, b); machine_emits(m, " given)\n");
}
/* OPCD handed an opcode that carries an operand: refused with a (usually
 * missed) OPV (c4m.js opvTrap, for the user loop) */
void eng_opv_trap(machine *m, i32 op) {
  opv_note(m, op);
  trap(m, TRAP_OPV, op, m->trap_handler);
  m->cycle_interval = 0; m->mode = 0;
}

/* the rare opcodes (c4m.js sys) */
static int sys(machine *m, i32 op) {
  i32 sp = m->sp;
  /* Under the MMU, the opcodes that run the machine rather than the task
   * are the kernel's: starting, halting and waking CPUs, IPIs, and the
   * cycle interrupt's configuration (docs/c4ix-mmu.md 6) */
  if (m->vm && (op == OP_C4CF || (op >= OP_CPUS && op <= OP_CPUH) || (op >= OP_CWAI && op <= OP_IPI)))
    return pm_trap(m, op);
  switch (op) {
    case OP_OPEN:
      if (m->mode) return pm_trap(m, op);
      m->disk_flags = m_ld32(m, sp);
      m->a = dev_disk_open(m, (u32)m_ld32(m, sp + 4));
      return STOP_BUDGET;
    case OP_READ: {
      if (m->mode) return pm_trap(m, op);
      i32 buf = m_ld32(m, sp + 4), len = m_ld32(m, sp);
      if (len > 0 && in_guard(m, buf, len)) return guard_range(m, OP_READ, buf, len, 1);
      i32 fd0 = m->disk_fd, addr0 = m->disk_addr;
      m->disk_fd = m_ld32(m, sp + 8);
      m->disk_addr = buf;
      i32 r = is_rom(buf, len) ? rom_read(m, len) : dev_disk_read(m, len);
      m->disk_fd = fd0; m->disk_addr = addr0;
      if (r == -2) return blocked_read(m);
      m->a = r;
      return STOP_BUDGET;
    }
    case OP_CLOS:
      if (m->mode) return pm_trap(m, op);
      dev_write32(m, DISK_CLOSE_REG, m_ld32(m, sp));
      m->a = dev_read32(m, DISK_CLOSE_REG);
      return STOP_BUDGET;
    case OP_PRTF: {
      if (m->mode) return pm_trap(m, op);
      i32 r = m_ld32(m, m->pc + 4);
      if (r > 7) { machine_emits(m, "Too many arguments to printf!\n"); return halt(m, -1); }
      i32 t = sp + r * 4;
      i32 args[6];
      for (int k = 2; k <= 7; k++) args[k - 2] = m_ld32(m, t - k * 4);
      const u8 *f; u32 fl = cstr(m, (u32)m_ld32(m, t - 4), 1u << 20, &f);
      static obuf out;
      out.n = 0;
      fmt_format(&out, f, fl, args, 6, cstr_cb, m);
      machine_emit(m, out.b, out.n);
      m->a = (i32)out.n;
      return STOP_BUDGET;
    }
    case OP_MALC:
      if (m->mode) return pm_trap(m, op);
      m->a = (i32)heap_alloc(m->heap, m_ld32(m, sp));
      return STOP_BUDGET;
    case OP_FREE:
      if (m->mode) return pm_trap(m, op);
      heap_free(m->heap, (u32)m_ld32(m, sp));
      return STOP_BUDGET;
    case OP_RALC:
      if (m->mode) return pm_trap(m, op);
      m->a = (i32)heap_realloc(m->heap, m->u8, (u32)m_ld32(m, sp + 4), m_ld32(m, sp));
      return STOP_BUDGET;
    case OP_MSET: {
      if (m->vm) return mmu_block(m->mmu, op);
      if (m->uwin && kwin(m, op)) return kblock(m, op);
      i32 d = m_ld32(m, sp + 8), v = m_ld32(m, sp + 4), n = m_ld32(m, sp);
      if (n > 0) {
        if (in_guard(m, d, n)) return guard_range(m, OP_MSET, d, n, 1);
        if (d >= 0x1000) { if ((u32)d < m->size) memset(m->u8 + d, v & 0xff, (u32)n <= m->size - (u32)d ? (u32)n : m->size - (u32)d); }
        else for (i32 k = 0; k < n; k++) m_st8(m, d + k, v);
      }
      m->a = d;
      return STOP_BUDGET;
    }
    case OP_MCMP: {
      if (m->vm) return mmu_block(m->mmu, op);
      if (m->uwin && kwin(m, op)) return kblock(m, op);
      i32 p = m_ld32(m, sp + 8), q = m_ld32(m, sp + 4), n = m_ld32(m, sp);
      if (n > 0 && in_guard(m, p, n)) return guard_range(m, OP_MCMP, p, n, 0);
      if (n > 0 && in_guard(m, q, n)) return guard_range(m, OP_MCMP, q, n, 0);
      i32 r = 0;
      for (i32 k = 0; k < n; k++) {
        u32 x = (u32)(p + k), y = (u32)(q + k);
        i32 c = (i32)(x < m->size ? m->u8[x] : 0) - (i32)(y < m->size ? m->u8[y] : 0);
        if (c) { r = c; break; }
      }
      m->a = r;
      return STOP_BUDGET;
    }
    case OP_MCPY: {
      if (m->vm) return mmu_block(m->mmu, op);
      if (m->uwin && kwin(m, op)) return kblock(m, op);
      i32 d = m_ld32(m, sp + 8), s = m_ld32(m, sp + 4), n = m_ld32(m, sp);
      if (n > 0) {
        if (in_guard(m, s, n)) return guard_range(m, OP_MCPY, s, n, 0);
        if (in_guard(m, d, n)) return guard_range(m, OP_MCPY, d, n, 1);
        u32 ud = (u32)d, us = (u32)s, un = (u32)n;
        if (is_rom(d, n)) {
          for (i32 k = 0; k < n; k++) {
            i32 x = d + k; u8 b = (u32)(s + k) < m->size ? m->u8[s + k] : 0;
            if (is_rom(x, 1)) rom_store(m, x, b); else if ((u32)x < m->size) m->u8[x] = b;
          }
        } else if (ud > us && ud < us + un) {
          for (u32 k = 0; k < un; k++) if (ud + k < m->size) m->u8[ud + k] = us + k < m->size ? m->u8[us + k] : 0;
        } else if (us < m->size && ud < m->size) {
          /* copyWithin: clamped to the arena at both ends */
          u32 c = un;
          if (c > m->size - us) c = m->size - us;
          if (c > m->size - ud) c = m->size - ud;
          memmove(m->u8 + ud, m->u8 + us, c);
        }
      }
      m->a = d;
      return STOP_BUDGET;
    }
    case OP_EXIT:
      if (m->mode) return pm_trap(m, op);
      return halt(m, m_ld32(m, sp));
    case OP_PUTC: {
      if (m->mode) return pm_trap(m, op);
      u8 b = m->u8[(u32)sp & (m->size - 1)];
      machine_emit(m, &b, 1);
      m->a = b;
      return STOP_BUDGET;
    }
    case OP_PUTS: {
      if (m->mode) return pm_trap(m, op);
      const u8 *p; u32 n = cstr(m, (u32)m_ld32(m, sp), 1u << 20, &p);
      machine_emit(m, p, n);
      u8 nl = 10; machine_emit(m, &nl, 1);
      m->a = 10;
      return STOP_BUDGET;
    }
    case OP_STRC: machine_stacktrace(m); return STOP_BUDGET;
    case OP_ITH: {
      if (m->mode) return pm_trap(m, op);
      i32 old = m->trap_handler;
      m->trap_handler = m_ld32(m, sp);
      m->a = old;
      return STOP_BUDGET;
    }
    case OP__OPC: {
      char s[17]; const u8 *q; u32 n = cstr(m, (u32)m_ld32(m, sp), 16, &q);
      memcpy(s, q, n); s[n] = 0;
      m->a = dev_opcode_by_name(s);
      return STOP_BUDGET;
    }
    case OP__TRP:
      trap(m, m_ld32(m, sp + 4), m_ld32(m, sp), m->trap_handler);
      return STOP_BUDGET;
    case OP_C4CF: {
      i32 opt = m_ld32(m, sp + 4), val = m_ld32(m, sp);
      if (opt == 0) { m->a = m->cycle_interval; m->cycle_interval = val; }
      else if (opt == 1) { m->a = m->cycle_handler; m->cycle_handler = val; }
      else if (opt == 3) { m->a = m->trap_restores_interval; m->trap_restores_interval = val; }
      else { machine_emits(m, "c4m: C4CF issue\n"); return halt(m, -100); }
      return STOP_BUDGET;
    }
    case OP_TIME: m->a = dev_read32(m, TIME_MS_REG); return STOP_BUDGET;
    case OP_SIGH: {
      if (m->mode) return pm_trap(m, op);
      i32 sig = m_ld32(m, sp + 4), h = m_ld32(m, sp);
      if (sig >= 0 && sig < 64) { m->a = m->sigh[sig]; m->sigh[sig] = h; } else m->a = 0;
      return STOP_BUDGET;
    }
    case OP_SIGI: m->a = 2; return STOP_BUDGET;
    case OP_USLP:
      if (m->mode) return pm_trap(m, op);
      dev_write32(m, USLP_US_REG, m_ld32(m, sp));
      m->a = 0;
      return STOP_SLEEP;
    case OP_INFO:
      if (m->mode) return pm_trap(m, op);
      m->a = machine_info(m);
      return STOP_BUDGET;
    case OP_OPSL: m->a = OPNAMES_ROM; return STOP_BUDGET;
    case OP_C4IV: return STOP_BUDGET;
    case OP_FLT: m->a = flt_op(m, (u32)sp); return STOP_BUDGET;
    case OP_FPU: {
      i32 v;
      if (!m->no_fpu && fpu_op(m, (u32)sp, &v)) { m->a = v; return STOP_BUDGET; }
      if (m->no_fpu && !m->trap_handler) {
        machine_emits(m, "c4m: this program needs an FPU (it was built with -mfpu), and this machine has none. Halting.\n");
        return halt(m, -1);
      }
      break;
    }
    case OP_TLEV: {
      if (m->vm) {
        /* a user task's own TLEV, a C4KE signal handler returning: the
         * registers from its frame, but never the mode or the preemption mask */
        i32 bp = m->bp;
        i32 pc = m_ld32(m, bp + 8);
        if (pc == m->pc - 4) return tlev_wedge(m, pc);
        i32 nsp = m_ld32(m, bp + 12), nbp = m_ld32(m, bp + 16), na = m_ld32(m, bp + 20);
        m->sp = nsp; m->bp = nbp; m->a = na; m->pc = pc;
        return STOP_BUDGET;
      }
      if (m->irq_hold > 0) m->irq_hold--;
      i32 bp = m->bp;
      i32 pc = m_ld32(m, bp + 8);
      if (pc == m->pc - 4) return tlev_wedge(m, pc);
      if (in_guard(m, m_ld32(m, bp + 12), 4)) { guard_hit(m, m_ld32(m, bp + 12), 4, 0, 0, OP_TLEV, 0); return STOP_HALT; }
      m->sp = m_ld32(m, bp + 12);
      m->bp = m_ld32(m, bp + 16);
      m->a = m_ld32(m, bp + 20);
      m->mode = m_ld32(m, bp + 24);
      if (m->trap_restores_interval) m->cycle_interval = m_ld32(m, bp + 36);
      m->pc = pc;
      if (m->mode && m->mmu && m->mmu->on) return STOP_SWITCH;
      return STOP_BUDGET;
    }
    case OP_DBG: trap(m, TRAP_DEBUG, 0, m->trap_handler); return STOP_BUDGET;
  }
  if (op >= ACC_BASE && op < ACC_END && m->accel) {
    /* an accelerator (accel.c); a number nobody has is a custom opcode
     * like any other */
    int s = accel_op(m, op);
    if (s >= 0) return s;
  }
  if (op == (i32)NULL_GUARD_OP) return null_jump(m, m->pc - 4);
  if (op >= 0 && op < INS_SIZE && !m->trap_handler) {
    char nm[6], a[12], buf[24];
    memcpy(nm, OPNAMES + op * 5, 4); nm[4] = 0;
    machine_emits(m, "c4m: "); machine_emits(m, nm); machine_emits(m, " (opcode ");
    machine_emits(m, dec(buf, op)); machine_emits(m, ") at ");
    { char *h = hex8(a, (u32)(m->pc - 4)); while (h[2] == '0' && h[3]) memmove(h + 2, h + 3, strlen(h + 3) + 1); machine_emits(m, h); }
    machine_emits(m, " is not an instruction this machine has,\n");
    machine_emits(m, "c4m: and no trap handler is installed to emulate it. Halting.\n");
    return halt(m, -1);
  }
  trap(m, TRAP_ILLOP, op, m->trap_handler);
  m->mode = 0;
  return STOP_BUDGET;
}

int eng_sys(machine *m, i32 op) { return sys(m, op); }

static int need_checks(machine *m) {
  return m->cycle_interval || m->pit_ms || m->pending_signal || m->mbox_irq;
}
int eng_need_checks(machine *m) { return need_checks(m); }

static i64 next_multiple(i64 cycle, i32 interval) { return (cycle / interval + 1) * interval; }

/* the boundary checks, in c4bb's order (c4m.js boundary): inlined into the
 * loop (out of it, the loop ran 7% slower); mmu.c's calls eng_boundary */
static inline __attribute__((always_inline)) i64 boundary(machine *m, i64 cycle, i64 next_irq) {
  int tlev = (m->vm ? mmu_peek(m->mmu, m->pc) : W(m->pc)) == OP_TLEV;   /* TLEV is never interrupted */
  int held = tlev || m->irq_hold > 0;
  i32 interval = m->cycle_interval;
  int fired = 1;
  if (m->pit_ms && (cycle & 4095) == 0 && !held && plat_now_ms() >= m->pit_next) {
    m->pit_next = plat_now_ms() + m->pit_ms;
    hirq(m, 1);
  } else if (m->mbox_irq && m->cycle_handler && !held) {
    m->mbox_irq = 0;
    hirq(m, 2);
  } else {
    int irq = 0;
    if (interval > 0 && cycle >= next_irq) { next_irq = next_multiple(cycle, interval); irq = !tlev; }
    if (irq) hirq(m, 0);
    else if (m->pending_signal && !(m->trap_restores_interval && !interval)) deliver_signal(m);
    else fired = 0;
  }
  m->b_fired = fired;
  if (fired) {
    interval = m->cycle_interval;
    m->b_next_irq = interval > 0 ? next_multiple(cycle, interval) : 0;
    return 0;
  }
  m->b_next_irq = next_irq;
  if ((m->mbox_irq && m->cycle_handler && !m->irq_hold) || m->pending_signal) return 0;
  i64 next = INT64_MAX;
  if (interval > 0) next = next_irq;
  if (m->pit_ms) { i64 pit = (cycle / 4096 + 1) * 4096; if (pit < next) next = pit; }
  return next;
}
i64 eng_boundary(machine *m, i64 cycle, i64 next_irq) { return boundary(m, cycle, next_irq); }

/* OPCD of one of the loop's own opcodes with no operand (c4m.js opcd) */
static void opcd(machine *m, i32 op) {
  i32 sp = m->sp, a = m->a, x = 0;
  switch (op) {
    case OP_LEV: { i32 bp = m->bp; m->bp = W(bp); m->pc = W(bp + 4); sp = bp + 8; break; }
    case OP_LI: a = m_ld32(m, a); break;
    case OP_LC: a = m_ld8(m, a); break;
    case OP_SI: x = W(sp); sp += 4; m_st32(m, x, a); break;
    case OP_SC: x = W(sp); sp += 4; m_st8(m, x, a); a = (int8_t)a; break;
    case OP_LH: a = m_ld16(m, a); break;
    case OP_LHU: a = m_ld16(m, a) & 0xffff; break;
    case OP_SH: x = W(sp); sp += 4; m_st16(m, x, a); break;
    case OP_LSR: x = W(sp); sp += 4; a = (i32)((u32)x >> (a & 31)); break;
    case OP_LCU: a = m_ld8(m, a) & 255; break;
    case OP_PSH: sp -= 4; W(sp) = a; break;
    case OP_LIP: a = m_ld32(m, a); sp -= 4; W(sp) = a; break;
    case OP_ADDL: x = (i32)((u32)W(sp) + (u32)a); sp += 4; a = m_ld32(m, x); break;
    case OP_POPA: a = W(sp); sp += 4; break;
    case OP_JMPA: m->pc = a; break;
    case OP__JMP: m->pc = W(sp); sp += 4; break;
    case OP__ADJ: sp = (i32)((u32)sp + ((u32)W(sp) << 2)); break;
    case OP_C4CY: a = (i32)m->cycle; break;
    default: {
      i32 t = W(sp); sp += 4;
      switch (op) {
        case OP_OR: a = t | a; break;
        case OP_XOR: a = t ^ a; break;
        case OP_AND: a = t & a; break;
        case OP_EQ: a = t == a; break;
        case OP_NE: a = t != a; break;
        case OP_LT: a = t < a; break;
        case OP_GT: a = t > a; break;
        case OP_LE: a = t <= a; break;
        case OP_GE: a = t >= a; break;
        case OP_SHL: a = (i32)((u32)t << (a & 31)); break;
        case OP_SHR: a = t >> (a & 31); break;
        case OP_ADD: a = (i32)((u32)t + (u32)a); break;
        case OP_SUB: a = (i32)((u32)t - (u32)a); break;
        case OP_MUL: a = (i32)((u32)t * (u32)a); break;
        case OP_DIV: a = a == 0 ? 0 : a == -1 ? (i32)(0u - (u32)t) : t / a; break;
        case OP_MOD: a = a == 0 ? 0 : a == -1 ? 0 : t % a; break;
      }
    }
  }
  m->sp = sp; m->a = a;
}

/* ---------------------------------------------------------------- */
/* Up to max_cycles. With the MMU on, user mode runs a loop of its own
 * (mmu.c), and the MMU says which loop runs when (c4m.js run) */
int machine_run(machine *m, i64 max_cycles) {
  if (m->mmu && m->mmu->on) return mmu_run(m->mmu, max_cycles);
  return eng_run_plain(m, max_cycles);
}

/* the interpreter's loop: every guest's, and C4IX's kernel mode under the
 * MMU, which leaves it with STOP_SWITCH at a TLEV into user mode */
int eng_run_plain(machine *m, i64 max_cycles) {
  if (m->halted) return STOP_HALT;
  i32 *const mw = m->w;
  u8 *const mu = m->u8;
  const u32 wm = m->wmask;
#define MW(x) mw[((u32)(x) >> 2) & wm]
  i32 pc = m->pc, sp = m->sp, bp = m->bp, a = m->a;
  i64 cycle = m->cycle;
  const i64 end = cycle + max_cycles;
  i32 interval = m->cycle_interval;
  i64 next_irq = interval > 0 ? next_multiple(cycle, interval) : 0;
  int chk = need_checks(m);
  i64 next_chk = 0, lim = -1;
  int stop = STOP_BUDGET;
  i32 op = 0, x = 0;
  i32 glo = guard_lo(m);
#define SAVE() (m->pc = pc, m->sp = sp, m->bp = bp, m->a = a, m->cycle = cycle)
#define LOAD() (pc = m->pc, sp = m->sp, bp = m->bp, a = m->a)
#define RECHECK() do { glo = guard_lo(m); interval = m->cycle_interval; \
    next_irq = interval > 0 ? next_multiple(cycle, interval) : 0; \
    chk = need_checks(m); next_chk = 0; lim = -1; } while (0)

  for (;;) {
    if (++cycle > lim) {
      if (cycle > end) { --cycle; break; }
      if (chk && cycle >= next_chk) {
        SAVE();
        next_chk = boundary(m, cycle, next_irq);
        next_irq = m->b_next_irq;
        if (m->b_fired) {
          LOAD();
          interval = m->cycle_interval;
          chk = need_checks(m);
        }
      }
      lim = chk && next_chk - 1 < end ? next_chk - 1 : end;
    }

    op = MW(pc);
    pc += 4;
    switch (op) {
      case OP_LEA: a = (i32)((u32)bp + ((u32)MW(pc) << 2)); pc += 4; break;
      case OP_IMM: a = MW(pc); pc += 4; break;
      case OP_JMP: pc = MW(pc); break;
      case OP_JSR: sp -= 4; MW(sp) = pc + 4; pc = MW(pc); break;
      case OP_BZ: pc = a ? pc + 4 : MW(pc); break;
      case OP_BNZ: pc = a ? MW(pc) : pc + 4; break;
      case OP_ENT: sp -= 4; MW(sp) = bp; bp = sp; sp = (i32)((u32)sp - ((u32)MW(pc) << 2)); pc += 4; break;
      case OP_ADJ: sp = (i32)((u32)sp + ((u32)MW(pc) << 2)); pc += 4; break;
      case OP_LEV: sp = bp; bp = MW(sp); pc = MW(sp + 4); sp += 8; break;
      case OP_LI:
        if ((a & 3) == 0 && a >= 0x1000 && a < glo) a = mw[(u32)a >> 2];
        else { SAVE(); a = m_ld32(m, a); if (m->halted) { stop = STOP_HALT; goto out; } }
        break;
      case OP_LC:
        if (a >= 0x1000 && a < glo) a = (int8_t)mu[a];
        else { SAVE(); a = m_ld8(m, a); if (m->halted) { stop = STOP_HALT; goto out; } }
        break;
      case OP_SI:
        x = MW(sp); sp += 4;
        if ((x & 3) == 0 && x >= 0x1000 && x < glo) mw[(u32)x >> 2] = a;
        else { SAVE(); m_st32(m, x, a); if (m->halted) { stop = STOP_HALT; goto out; } RECHECK(); }
        break;
      case OP_SC:
        x = MW(sp); sp += 4;
        if (x >= 0x1000 && x < glo) mu[x] = (u8)a;
        else { SAVE(); m_st8(m, x, a); if (m->halted) { stop = STOP_HALT; goto out; } RECHECK(); }
        a = (int8_t)a;
        break;
      case OP_LH:
        if (a >= 0x1000 && a < glo - 1) a = (int16_t)(mu[a] | mu[a + 1] << 8);
        else { SAVE(); a = m_ld16(m, a); if (m->halted) { stop = STOP_HALT; goto out; } }
        break;
      case OP_LHU:
        if (a >= 0x1000 && a < glo - 1) a = mu[a] | mu[a + 1] << 8;
        else { SAVE(); a = m_ld16(m, a) & 0xffff; if (m->halted) { stop = STOP_HALT; goto out; } }
        break;
      case OP_SH:
        x = MW(sp); sp += 4;
        if (x >= 0x1000 && x < glo - 1) { mu[x] = (u8)a; mu[x + 1] = (u8)(a >> 8); }
        else { SAVE(); m_st16(m, x, a); if (m->halted) { stop = STOP_HALT; goto out; } RECHECK(); }
        break;
      case OP_LSR: a = (i32)((u32)MW(sp) >> (a & 31)); sp += 4; break;
      case OP_LCU:
        if (a >= 0x1000 && a < glo) a = mu[a];
        else { SAVE(); a = m_ld8(m, a) & 255; if (m->halted) { stop = STOP_HALT; goto out; } }
        break;
      case OP_PSH: sp -= 4; MW(sp) = a; break;
      case OP_OR: a = MW(sp) | a; sp += 4; break;
      case OP_XOR: a = MW(sp) ^ a; sp += 4; break;
      case OP_AND: a = MW(sp) & a; sp += 4; break;
      case OP_EQ: a = MW(sp) == a; sp += 4; break;
      case OP_NE: a = MW(sp) != a; sp += 4; break;
      case OP_LT: a = MW(sp) < a; sp += 4; break;
      case OP_GT: a = MW(sp) > a; sp += 4; break;
      case OP_LE: a = MW(sp) <= a; sp += 4; break;
      case OP_GE: a = MW(sp) >= a; sp += 4; break;
      case OP_SHL: a = (i32)((u32)MW(sp) << (a & 31)); sp += 4; break;
      case OP_SHR: a = MW(sp) >> (a & 31); sp += 4; break;
      case OP_ADD: a = (i32)((u32)MW(sp) + (u32)a); sp += 4; break;
      case OP_SUB: a = (i32)((u32)MW(sp) - (u32)a); sp += 4; break;
      case OP_MUL: a = (i32)((u32)MW(sp) * (u32)a); sp += 4; break;
      case OP_DIV: { i32 t = MW(sp); a = a == 0 ? 0 : a == -1 ? (i32)(0u - (u32)t) : t / a; sp += 4; break; }
      case OP_MOD: { i32 t = MW(sp); a = a == 0 ? 0 : a == -1 ? 0 : t % a; sp += 4; break; }

      case OP_LDL: a = MW((u32)bp + ((u32)MW(pc) << 2)); pc += 4; break;
      case OP_LDG: x = MW(pc); pc += 4; if ((x & 3) == 0 && x >= 0x1000) a = mw[((u32)x >> 2) & wm]; else { m->cycle = cycle; a = m_ld32(m, x); } break;
      case OP_PSHL: a = MW((u32)bp + ((u32)MW(pc) << 2)); pc += 4; sp -= 4; MW(sp) = a; break;
      case OP_PSHG: x = MW(pc); pc += 4; if ((x & 3) == 0 && x >= 0x1000) a = mw[((u32)x >> 2) & wm]; else { m->cycle = cycle; a = m_ld32(m, x); } sp -= 4; MW(sp) = a; break;
      case OP_LEAP: a = (i32)((u32)bp + ((u32)MW(pc) << 2)); pc += 4; sp -= 4; MW(sp) = a; break;
      case OP_IMMP: a = MW(pc); pc += 4; sp -= 4; MW(sp) = a; break;
      case OP_LIP:
        if ((a & 3) == 0 && a >= 0x1000 && a < glo) a = mw[(u32)a >> 2];
        else { SAVE(); a = m_ld32(m, a); if (m->halted) { stop = STOP_HALT; goto out; } }
        sp -= 4; MW(sp) = a;
        break;
      case OP_ADDL:
        x = (i32)((u32)MW(sp) + (u32)a); sp += 4;
        if ((x & 3) == 0 && x >= 0x1000 && x < glo) a = mw[(u32)x >> 2];
        else { SAVE(); a = m_ld32(m, x); if (m->halted) { stop = STOP_HALT; goto out; } }
        break;
      case OP_STL: MW((u32)bp + ((u32)MW(pc) << 2)) = a; pc += 4; break;
      case OP_POPA: a = MW(sp); sp += 4; break;

      case OP_JSRI: x = MW(pc); sp -= 4; MW(sp) = pc + 4; SAVE(); pc = m_ld32(m, x); break;
      case OP_JSRS: x = MW(pc); sp -= 4; MW(sp) = pc + 4; pc = MW((u32)bp + ((u32)x << 2)); break;
      case OP_JMPA: pc = a; break;
      case OP__JMP: pc = MW(sp); sp += 4; break;
      case OP__ADJ: sp = (i32)((u32)sp + ((u32)MW(sp) << 2)); break;
      case OP_C4CY: a = (i32)cycle; break;

      case OP_OPCD:
        op = MW(sp);
        if (has_operand(op)) {
          SAVE();
          opv_note(m, op);
          trap(m, TRAP_OPV, op, m->trap_handler);
          m->cycle_interval = 0; m->mode = 0;
          LOAD();
          interval = 0; next_irq = 0;
          chk = need_checks(m); next_chk = 0; lim = -1;
          a = (i32)cycle;
          break;
        }
        if (is_plain_op(op)) {
          SAVE();
          opcd(m, op);
          LOAD();
          if (m->halted) { stop = STOP_HALT; goto out; }
          interval = m->cycle_interval;
          next_irq = interval > 0 ? next_multiple(cycle, interval) : 0;
          chk = need_checks(m); next_chk = 0; lim = -1;
          break;
        }
        goto fpu;

      case OP_FPU:
      fpu:
        if (op == OP_FPU) {
          x = MW(sp);
          if ((u32)(x - 1) < 8 && !m->no_fpu) { a = fpu_float(x, MW(sp + 4), MW(sp + 8)); break; }
          if ((u32)(x - 17) < 10 && x != 21 && !m->no_fpu) {
            /* a double's, if its addresses are all plain memory (c4m.js fpuPlain) */
            static const int FA = 0x2af, FB = 0x02f, FR = 0x14f;
            int bit = 1 << (x - 17);
            i32 pa = MW(sp + 4), pb = MW(sp + 8), pr = MW(sp + 12);
            int ok = 1;
            if ((FA & bit) && !((pa & 3) == 0 && pa >= 0x1000 && pa < glo - 4)) ok = 0;
            if ((FB & bit) && !((pb & 3) == 0 && pb >= 0x1000 && pb < glo - 4)) ok = 0;
            if ((FR & bit) && !((pr & 3) == 0 && pr >= 0x1000 && pr < glo - 4)) ok = 0;
            if (ok) {
              i32 v;
              fpu_double_plain(mw, x, (FA & bit) ? (i32)((u32)pa >> 2) : pa, (i32)((u32)pb >> 2), (i32)((u32)pr >> 2), &v);
              a = v;
              break;
            }
          }
        }
        /* fall through */
      default: {
        SAVE();
        int s = sys(m, op);
        LOAD();
        cycle = m->cycle;
        RECHECK();
        if (s != STOP_BUDGET) { stop = s; goto out; }
        if (m->halted) { stop = STOP_HALT; goto out; }
      }
    }
  }
out:
  SAVE();
  if (m->halted) stop = STOP_HALT;
  return stop;
#undef MW
#undef SAVE
#undef LOAD
#undef RECHECK
}

/* ---------------------------------------------------------------- */
machine *machine_new(u32 arena_bytes, drive **drives, int ndrives, double mhz) {
  machine *m = xmalloc(sizeof *m);
  u32 size = 1u << 20;
  while (size < arena_bytes && size < (1u << 31)) size <<= 1;
  m->size = size;
  m->u8 = xmalloc(size);
  m->w = (i32 *)m->u8;
  m->wmask = (size >> 2) - 1;
  m->stack_top = size - ARGS_RESERVE;
  m->heap = heap_new();
  m->drives = drives; m->ndrives = ndrives;
  m->fds = imap_new();
  m->next_fd = 3;
  m->swap_pages = imap_new();
  m->swap_slots = 65536;
  i64 cpm = (i64)(mhz * 1000 + 0.5);
  m->cycles_per_ms = mhz ? (u32)(cpm > 1 ? cpm : 1) : 20000;
  m->t0 = plat_now_ms();
  m->accel = m->osaccel = 1;           /* as libjs: off with --no-accel, --no-osaccel */
  return m;
}

static i32 rd32le(const u8 *p) { return (i32)(p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24); }

/* the .c4r loader (src/c4bb/sim/loader.js) and the trampoline (boot.js) */
int machine_boot(machine *m, const u8 *img, u32 len, int argc, char **argv) {
  u8 *u = m->u8;
  for (int k = 0; k < 4; k++) m->w[k] = (i32)NULL_GUARD_OP;
  m->w[TLEV_ADDR >> 2] = OP_TLEV;
  m->w[CPU_HALT_ADDR >> 2] = OP_CPUH;
  memcpy(u + OPNAMES_ROM, OPNAMES, OPNAMES_LEN);

  if (len < 13 || img[0] != 'C' || img[1] != '4' || img[2] != 'R') { machine_emits(m, "c4pc: not a .c4r image\n"); return -1; }
  int version = img[3];
  if (img[4] != 32) { machine_emits(m, "c4pc: a 32-bit machine; the image is not 32-bit\n"); return -1; }
  u32 p = 13;
#define RW() (p + 4 <= len ? (p += 4, rd32le(img + p - 4)) : 0)
  i32 entry = RW(), code_len = RW(), data_len = RW(), patch_len = RW(), syms_len = RW(), cons_len = RW(), des_len = RW();
  i32 memsz = version >= 3 ? rd32le(img + 5) : data_len;
  if (memsz < data_len) memsz = data_len;
  u32 code_base = MEM_BASE;
  u32 data_base = (code_base + (u32)code_len * 4 + 7) & ~3u;
  u32 top = (data_base + (u32)memsz + 7) & ~3u;
  if ((i64)top + 65536 > m->stack_top) { machine_emits(m, "c4pc: the image does not fit the arena\n"); return -1; }
  RW();                                            /* 'C' */
  for (i32 i = 0; i < code_len; i++) m->w[(code_base >> 2) + (u32)i] = RW();
  RW();                                            /* 'D' */
  if (p + (u32)data_len > len) { machine_emits(m, "c4pc: the image is cut short\n"); return -1; }
  memcpy(u + data_base, img + p, (u32)data_len); p += (u32)data_len;
  RW();                                            /* 'P' */
  for (i32 i = 0; i < patch_len; i++) {
    i32 type = RW(), addr = RW(), val = RW();
    if (type == -1) arena_wr32(m, code_base + (u32)addr * 4, (i32)(code_base + (u32)val * 4));
    else if (type == -2) arena_wr32(m, code_base + (u32)addr * 4, (i32)(data_base + (u32)val));
    else if (type == -3) arena_wr32(m, data_base + (u32)addr, (i32)(code_base + (u32)val * 4));
    else if (type == -4) arena_wr32(m, data_base + (u32)addr, (i32)(data_base + (u32)val));
  }
  RW();                                            /* 'c' */
  u32 *cons = xmalloc((size_t)(cons_len + 1) * 4), *des = xmalloc((size_t)(des_len + 1) * 4);
  for (i32 i = 0; i < cons_len; i++) cons[i] = code_base + (u32)RW() * 4;
  RW();                                            /* 'd' */
  for (i32 i = 0; i < des_len; i++) des[i] = code_base + (u32)RW() * 4;
  RW();                                            /* 'S' */
  /* the symbols, functions only (class 129), for STRC */
  m->syms = xmalloc((size_t)(syms_len + 1) * sizeof *m->syms);
  m->nsyms = 0;
  for (i32 i = 0; i < syms_len && p + 17 <= len; i++) {
    i32 cls = rd32le(img + p + 8);
    u32 n = img[p + 16];
    if (p + 17 + n + 4 > len) break;
    i32 val = rd32le(img + p + 17 + n);
    if (cls == 129) {
      char *s = xmalloc(n + 1); memcpy(s, img + p + 17, n); s[n] = 0;
      m->syms[m->nsyms].addr = code_base + (u32)val * 4; m->syms[m->nsyms].name = s; m->nsyms++;
    }
    p += 17 + n + 4;
  }
  for (int i = 1; i < m->nsyms; i++) {             /* insertion sort: they come nearly in order */
    struct sym s = m->syms[i]; int j = i - 1;
    while (j >= 0 && m->syms[j].addr > s.addr) { m->syms[j + 1] = m->syms[j]; j--; }
    m->syms[j + 1] = s;
  }
#undef RW
  m->img_code = code_base; m->img_data = data_base; m->img_bss = data_base + (u32)data_len; m->img_end = top;

  /* argv above the stack */
  u32 q = m->stack_top;
  u32 *ptrs = xmalloc((size_t)(argc + 1) * 4);
  for (int i = 0; i < argc; i++) {
    u32 n = (u32)strlen(argv[i]);
    if (q + n + 1 > m->size) { machine_emits(m, "c4pc: argv does not fit in the 4 KB above the stack\n"); return -1; }
    memcpy(u + q, argv[i], n); u[q + n] = 0;
    ptrs[i] = q; q += n + 1;
  }
  q = (q + 3) & ~3u;
  u32 argv_base = q;
  for (int i = 0; i < argc; i++) { if (q + 4 > m->size) return -1; m->w[q >> 2] = (i32)ptrs[i]; q += 4; }
  xfree(ptrs);

  /* the trampoline */
  u32 tramp = (top + 3) & ~3u, w = tramp >> 2;
  for (i32 k = 0; k < cons_len; k++) {
    m->w[w++] = OP_IMM; m->w[w++] = 0; m->w[w++] = OP_PSH;
    m->w[w++] = OP_JSR; m->w[w++] = (i32)cons[k]; m->w[w++] = OP_ADJ; m->w[w++] = 1;
  }
  m->w[w++] = OP_IMM; m->w[w++] = argc; m->w[w++] = OP_PSH;
  m->w[w++] = OP_IMM; m->w[w++] = (i32)argv_base; m->w[w++] = OP_PSH;
  m->w[w++] = OP_JSR; m->w[w++] = (i32)(code_base + (u32)entry * 4); m->w[w++] = OP_ADJ; m->w[w++] = 2;
  m->w[w++] = OP_PSH;
  for (i32 k = des_len - 1; k >= 0; k--) { m->w[w++] = OP_JSR; m->w[w++] = (i32)des[k]; }
  m->w[w++] = OP_EXIT;
  u32 tend = w << 2;
  xfree(cons); xfree(des);

  u32 heap_base = (tend + 4096) & ~4095u;
  u32 heap_end = ((m->stack_top - 1024 * 1024) & ~4095u);
  if (heap_end <= heap_base) { machine_emits(m, "c4pc: the image does not leave room for a heap; use a bigger arena\n"); return -1; }
  heap_reset(m->heap, heap_base, heap_end);
  dev_write32(m, HEAP_BASE_REG, (i32)heap_base);
  dev_write32(m, HEAP_END_REG, (i32)heap_end);
  m->stack_base = heap_end;
  m->stack_top_reg = m->stack_top;
  m->stack_guard = 0;
  m->sp = m->bp = (i32)m->stack_top;
  m->pc = (i32)tramp;
  m->a = 0;
  return 0;
}

/* ---- the clock, measured (c4pc's -hz auto) ----
 * c4pc at the keyboard paces the machine to its clock, as cli.js -i
 * does, so the clock must be one this PC keeps up with: 20 MHz, libjs's,
 * holds a VM with hardware virtualization at a seventh of what it can
 * do, and is more than an emulator gives. So before the boot, the
 * interpreter runs a loop on a scratch machine of its own for a moment,
 * and the count says how fast it goes here. The loop is the shape of
 * compiled c4: locals loaded, pushed, combined, stored, a call and a
 * branch, and a load from somewhere else in its arena each time round
 * (bytes of it, 16 MB where there is room), as
 * C4IX's own work is spread over its arena (which a VM's nested paging
 * makes dear, where a loop in a page or two is not). */
u32 machine_kips(int ms, u32 bytes) {
  machine *c = machine_new(bytes, 0, 0, 20);
  i32 *w = c->w;
  u32 p = MEM_BASE >> 2, loop, fn, call;
#define E(x) (w[p++] = (i32)(x))
  E(OP_ENT); E(2);
  loop = p << 2;
  E(OP_LEA); E(-1); E(OP_PSH); E(OP_LDL); E(-1); E(OP_PSH); E(OP_IMM); E(3); E(OP_ADD); E(OP_SI);
  E(OP_LEA); E(-2); E(OP_PSH); E(OP_LDL); E(-1); E(OP_PSH); E(OP_IMM); E(4099 * 4); E(OP_MUL);
  E(OP_PSH); E(OP_IMM); E(0xfffffc); E(OP_AND); E(OP_LI); E(OP_PSH); E(OP_LDL); E(-2); E(OP_ADD); E(OP_SI);
  E(OP_JSR); call = p; E(0);
  E(OP_LDL); E(-1); E(OP_PSH); E(OP_IMM); E(0x7fffffff); E(OP_AND); E(OP_BNZ); E(loop);
  E(OP_JMP); E(loop);
  fn = p << 2;
  E(OP_ENT); E(0); E(OP_IMM); E(1); E(OP_LEV);
  w[call] = (i32)fn;
#undef E
  c->pc = MEM_BASE;
  c->sp = c->bp = (i32)c->stack_top;
  /* start on a tick, so that ms of them are ms */
  i64 t = plat_now_ms();
  while (plat_now_ms() == t) machine_run(c, 1000);
  i64 c0 = c->cycle, t0 = plat_now_ms();
  while (plat_now_ms() - t0 < ms) machine_run(c, 100000);
  i64 dt = plat_now_ms() - t0;
  u32 k = (u32)((c->cycle - c0) / (dt > 0 ? dt : 1));
  /* the heap and maps are a few hundred bytes, kept */
  xfree(c->u8); xfree(c);
  return k;
}

/* for accel.c */
i32 eng_ld8(machine *m, i32 x) { return m_ld8(m, x); }
void eng_st8(machine *m, i32 x, i32 v) { m_st8(m, x, v); }
i32 eng_guard_lo(machine *m) { return guard_lo(m); }
