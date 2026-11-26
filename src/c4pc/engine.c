/* engine.c -- the c4m interpreter: libjs/c4m.js's Machine (one CPU, no
 * display or sound) and boot.js, in C. The MMU's user loop is mmu.c's;
 * kernel mode under it runs here, reaching user addresses through the
 * user window (kpa).
 *
 * Words are int32 (or, built with C4PC_W64, int64: native c4m's word,
 * c4pc.h), addresses byte offsets into the arena. Arithmetic wraps as
 * JavaScript's | 0 does; a shift takes its count mod the word's bits; DIV
 * and MOD by 0 give 0, and INT_MIN / -1 gives INT_MIN, as they do in libjs.
 * One difference: libjs's typed arrays read 0 past the arena and drop the
 * write, where here a word index wraps inside it (wmask). Only a wild
 * program can tell.
 */
#include "c4pc.h"
#include "clib.h"
#include "snd.h"

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

#define W(x) m->w[((uword)(x) >> WSH) & m->wmask]
/* a word as an address for a device's u32 one: past 4 GB it is past the
 * arena, wherever its low bits point; and as a count, at most 2^31 - 1
 * (the 32-bit machine's own, unchanged) */
#define A32(x) ((uword)(x) > 0xffffffffu ? 0xffffffffu : (u32)(uword)(x))
#define N32(x) ((x) > 0x7fffffff ? (i32)0x7fffffff : (x) < -0x7fffffff - 1 ? (i32)(-0x7fffffff - 1) : (i32)(x))

_Static_assert(sizeof OPNAMES - 1 == INS_SIZE * 5, "is_rom (c4pc.h) counts the names as INS_SIZE of 5 bytes");

/* The device lock: pc/hv.c sets it while guests run on processors of
 * their own. Every machine's device registers, rare opcodes and console
 * are then one processor's at a time -- their scratch buffers, the font
 * cache, the drives' caches, the disks are the PC's, not a machine's --
 * while their instructions run side by side. A fault out of a device (a
 * DMA its task's space refuses) jumps past the unlock: the catch it lands
 * in gives back what was taken since its eng_lock_mark (mmu.c, par.c) */
void (*eng_lock_fn)(int on);
int (*eng_lock_depth_fn)(void);
int eng_lock_mark(void) { return eng_lock_depth_fn ? eng_lock_depth_fn() : 0; }
void eng_lock_back(int d) { if (eng_lock_fn) while (eng_lock_depth_fn() > d) eng_lock_fn(-1); }

void machine_emit(machine *m, const u8 *b, u32 n) {
  if (m->par_ap && !m->serving) { par_emit(m, b, n); return; }   /* the console is CPU 0's */
  if (eng_lock_fn) eng_lock_fn(1);
  m->dv->uart_activity += n;
  dev_con_bytes(m, b, n);
  if (eng_lock_fn) eng_lock_fn(-1);
}
void machine_emits(machine *m, const char *s) { machine_emit(m, (const u8 *)s, (u32)strlen(s)); }

/* a word in hex, as many digits as the word has (8, or 16) */
static char *hex8(char *buf, uword v) {
  static const char h[] = "0123456789ABCDEF";
  buf[0] = '0'; buf[1] = 'x';
  for (int i = 0; i < 2 * WB; i++) buf[2 + i] = h[(v >> (4 * (2 * WB - 1) - 4 * i)) & 15];
  buf[2 + 2 * WB] = 0;
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
static int in_guard(machine *m, word x, word n) {
  return m->dv->stack_guard ? (i64)x < m->dv->stack_top_reg && (i64)x + n > m->dv->stack_base : 0;
}
static word guard_lo(machine *m) {
  word g = m->dv->stack_guard ? (word)m->dv->stack_base : 0x7fffffff;
  /* the loop's own bound: below the arena's end, so its fast paths need no other */
  word e = (word)m->size - WB;
  if (e < g) g = e;
  /* the user window too: an address in it is a user task's, which kernel
   * mode reaches through kpa */
  return m->uwin && (i64)m->uwin < g ? (word)m->uwin : g;
}
static void guard_hit(machine *m, word x, word n, int write, word v, word op, word from);
static void rom_store(machine *m, word x, word v);

/* a device's registers: CPU 0's, which another CPU asks for (par.c) --
 * but the swap's, SWAP_ADDR and SWAP_CMD, which it runs itself under the
 * swap's lock: C4IX turns the heap's high end on and off through them
 * around every allocation a task makes, and as requests they were most of
 * what another CPU asked (5,863 commands, three requests each, in a boot
 * and a compile) */
#define SWAP_ADDR_REG 0x11c
#define SWAP_CMD_REG 0x158
/* The display's registers with the device lock (the hypervisor's: pc/hv.c)
 * are any CPU's to reach itself, under the lock: their state is the
 * display's alone, and it reaches into the asker's space as CPU 0 would on
 * its behalf (gui.c dm). A guest's game on its CPU 1 asked its CPU 0 for
 * each BELL, PRESENT and BLIT8, and drew 12% fewer frames than on CPU 0
 * (Quake in QEMU, 29-34 fps against 34-38, 2026-11-26); its frame drawn
 * on its own processor now. The sound card's stay CPU 0's: its posted
 * writes are applied there, in order */
#define AP_DEV(m, a) ((m)->par_ap && !(m)->serving && (((a) >= DEV_BASE && (a) < DEV_END) || ((a) >= 0x400 && (a) < 0x480 && !eng_lock_fn) || ((a) >= 0x480 && (a) < 0x500) || \
                      ((a) >= BLK_BASE && (a) < BLK_END && (m)->dv->nblk) || ((a) >= HV_BASE && (a) < HV_END && (m)->dv->hv_rd)) && \
                      ((a) & ~3u) != SWAP_ADDR_REG && ((a) & ~3u) != SWAP_CMD_REG && ((a) & ~3u) != TIME_MS_REG)
/* TIME read on another CPU: its own (machine_sim_ms: the real clock, or as
 * CPU 0 last published it), not asked of CPU 0 -- Doom asks for the time
 * between every frame, and on CPU 1, a round trip to CPU 0 each, it drew
 * a few frames a second at 100% (2026-11-23) */
/* A word at a device register (they are 32 bits wide and 4 bytes apart at
 * either word size): the register's value, sign-extended; a store keeps
 * its low 32 bits. Memory past the arena reads 0 and drops the store. */
/* a device's register read: 1, and *v, if there is one at r */
static int dev_rd(machine *m, u32 r, word *v) {
  if (r >= DEV_BASE && r < DEV_END) { *v = dev_read32(m, r); return 1; }
  if (r >= 0x400 && r < 0x480 && m->gui) { *v = gui_rd(m->gui, m, r & ~3u); return 1; }     /* bus.js: the display */
  if (r >= 0x480 && r < 0x500 && m->snd) { *v = snd_read32(m->snd, r & ~3u); return 1; }     /* ... and the sound card */
  if (r >= BLK_BASE && r < BLK_END && m->dv->nblk) { *v = blk_read32(m, r & ~3u); return 1; } /* ... and the disks */
  if (r >= HV_BASE && r < HV_END && m->dv->hv_rd) { *v = m->dv->hv_rd(m, r & ~3u); return 1; }   /* ... and the hypervisor */
  if (r >= NET_BASE && r < NET_END && m->dv->net) { *v = net_rd(m, r & ~3u); return 1; }        /* ... and the network card */
  return 0;
}
static int dev_wr(machine *m, u32 r, word v) {
  if (r >= DEV_BASE && r < DEV_END) { dev_write32(m, r, (i32)v); return 1; }
  if (r >= 0x400 && r < 0x480 && m->gui) { gui_wr(m->gui, m, r & ~3u, (i32)v); return 1; }
  if (r >= 0x480 && r < 0x500 && m->snd) { snd_write32(m->snd, r & ~3u, (i32)v); return 1; }
  if (r >= BLK_BASE && r < BLK_END && m->dv->nblk) { blk_write32(m, r & ~3u, (i32)v); return 1; }
  if (r >= HV_BASE && r < HV_END && m->dv->hv_wr) { m->dv->hv_wr(m, r & ~3u, (i32)v); return 1; }
  if (r >= NET_BASE && r < NET_END && m->dv->net) { net_wr(m, r & ~3u, (i32)v); return 1; }
  return 0;
}
static int dev_wr8(machine *m, uword a, i32 v) {
  if (a >= DEV_BASE && a < DEV_END) { dev_write32(m, (u32)a & ~3u, v & 0xff); return 1; }
  if (a >= 0x400 && a < 0x480 && m->gui) { gui_wr(m->gui, m, (u32)a & ~3u, v & 0xff); return 1; }
  if (a >= 0x480 && a < 0x500 && m->snd) { snd_write32(m->snd, (u32)a & ~3u, v & 0xff); return 1; }
  return 0;
}
static word arena_rd32(machine *m, uword a) {
  if (a < 0x1000) {
    u32 r = (u32)a;
    /* on a CPU but 0, the disks' count and the last command's result are
     * read here: the command itself was asked of CPU 0, and is done */
    if (m->par_ap && !m->serving && (r == BLK_BASE || r == BLK_BASE + 0x1c) && m->dv->nblk) return blk_read32(m, r);
    if (AP_DEV(m, r)) return par_rd32(m, r & ~3u);
    word v; int h;
    if (eng_lock_fn) { eng_lock_fn(1); h = dev_rd(m, r, &v); eng_lock_fn(-1); }
    else h = dev_rd(m, r, &v);
    if (h) return v;
  }
  if (!(a & (WB - 1))) return a <= m->size - WB ? m->w[a >> WSH] : 0;
  if (a > m->size - WB) return 0;
  return word_le(m->u8 + a);
}
static void arena_wr32(machine *m, uword a, word v) {
  if (a < 0x1000) {
    u32 r = (u32)a;
    if (AP_DEV(m, r)) { par_wr(m, r & ~3u, (i32)v, 0); return; }
    int h;
    if (eng_lock_fn) { eng_lock_fn(1); h = dev_wr(m, r, v); eng_lock_fn(-1); }
    else h = dev_wr(m, r, v);
    if (h) return;
  }
  if (!(a & (WB - 1))) { if (a <= m->size - WB) m->w[a >> WSH] = v; return; }
  if (a > m->size - WB) return;
  for (int k = 0; k < WB; k++) m->u8[a + k] = (u8)((uword)v >> (8 * k));
}
/* bus.js: below 0x10 reads 0; then the arena's (no display, no sound) */
static word bus_rd32(machine *m, uword a) { return a < 0x10 ? 0 : arena_rd32(m, a); }
static i32 bus_rd8s(machine *m, uword a) { return a < 0x10 || a >= m->size ? 0 : (int8_t)m->u8[a]; }
static void bus_wr8(machine *m, uword a, i32 v) {
  if (a < 0x1000 && AP_DEV(m, (u32)a)) { par_wr(m, (u32)a, v & 0xff, 1); return; }
  if (a < 0x1000) {
    int h;
    if (eng_lock_fn) { eng_lock_fn(1); h = dev_wr8(m, a, v); eng_lock_fn(-1); }
    else h = dev_wr8(m, a, v);
    if (h) return;
  }
  if (a < m->size) m->u8[a] = (u8)v;
}

word eng_bus_rd32(machine *m, u32 a) { return bus_rd32(m, a); }
i32 eng_bus_rd8s(machine *m, u32 a) { return bus_rd8s(m, a); }
void eng_bus_wr32(machine *m, u32 a, word v) { arena_wr32(m, a, v); }
void eng_bus_wr8(machine *m, u32 a, i32 v) { bus_wr8(m, a, v); }
void eng_rom_store(machine *m, word x, word v) { rom_store(m, x, v); }

static word kld32(machine *m, uword x);
static void kst32(machine *m, uword x, word v);
static i32 kld8(machine *m, uword x);
static void kst8(machine *m, uword x, i32 v);

word m_ldw(machine *m, word x) {
  if (C4PC_HAS_MMU && m->vm) return mmu_ld32(m->mmu, x);
  if (C4PC_HAS_MMU && m->uwin && (uword)x >= m->uwin) return kld32(m, (uword)x);
  if ((x & (WB - 1)) == 0 && x >= 0x1000) {
    if (in_guard(m, x, WB)) { guard_hit(m, x, WB, 0, 0, 0, 0); return 0; }
    return (uword)x <= m->size - WB ? m->w[(uword)x >> WSH] : 0;
  }
  if (m->smp && x >= LOCAL_BASE && x < LOCAL_END && (x & (WB - 1)) == 0) return m->local[(x - LOCAL_BASE) >> WSH];
  if (C4PC_HAS_MMU && m->mmu && x >= MMU_BASE && x < MMU_END) return mmu_read32(m->mmu, x);
  return bus_rd32(m, (uword)x);
}
void m_stw(machine *m, word x, word v) {
  if (C4PC_HAS_MMU && m->vm) { mmu_st32(m->mmu, x, v); return; }
  if (C4PC_HAS_MMU && m->uwin && (uword)x >= m->uwin) { kst32(m, (uword)x, v); return; }
  if ((x & (WB - 1)) == 0 && x >= 0x1000) {
    if (in_guard(m, x, WB)) { guard_hit(m, x, WB, 1, v, 0, 0); return; }
    if ((uword)x <= m->size - WB) m->w[(uword)x >> WSH] = v;
    return;
  }
  if (m->smp && x >= LOCAL_BASE && x < LOCAL_END && (x & (WB - 1)) == 0) { m->local[(x - LOCAL_BASE) >> WSH] = v; return; }
  if (C4PC_HAS_MMU && m->mmu && x >= MMU_BASE && x < MMU_END) { mmu_write32(m->mmu, x, v); return; }
  if (is_rom(x, WB)) { rom_store(m, x, v); return; }
  arena_wr32(m, (uword)x, v);
}
static i32 m_ld8(machine *m, word x) {
  if (C4PC_HAS_MMU && m->vm) return mmu_ld8(m->mmu, x);
  if (C4PC_HAS_MMU && m->uwin && (uword)x >= m->uwin) return kld8(m, (uword)x);
  if (x >= 0x1000) {
    if (in_guard(m, x, 1)) { guard_hit(m, x, 1, 0, 0, 0, 0); return 0; }
    return (uword)x < m->size ? (int8_t)m->u8[x] : 0;
  }
  if (m->smp && x >= LOCAL_BASE && x < LOCAL_END) return (int8_t)((u8 *)m->local)[x - LOCAL_BASE];
  return bus_rd8s(m, (uword)x);
}
static void m_st8(machine *m, word x, word v) {
  if (C4PC_HAS_MMU && m->vm) { mmu_st8(m->mmu, x, v); return; }
  if (C4PC_HAS_MMU && m->uwin && (uword)x >= m->uwin) { kst8(m, (uword)x, (i32)v); return; }
  if (x >= 0x1000) {
    if (in_guard(m, x, 1)) { guard_hit(m, x, 1, 1, v, 0, 0); return; }
    if ((uword)x < m->size) m->u8[x] = (u8)v;
    return;
  }
  if (m->smp && x >= LOCAL_BASE && x < LOCAL_END) { ((u8 *)m->local)[x - LOCAL_BASE] = (u8)v; return; }
  if (is_rom(x, 1)) { rom_store(m, x, v & 0xff); return; }
  bus_wr8(m, (uword)x, (i32)(v & 0xff));
}
static word m_ld16(machine *m, word x) {
  word lo = m_ld8(m, x) & 255;
  return lo | (word)((uword)(word)m_ld8(m, (word)((uword)x + 1)) << 8);
}
static void m_st16(machine *m, word x, word v) { m_st8(m, x, v); m_st8(m, (word)((uword)x + 1), v >> 8); }

/* ---------------------------------------------------------------- */
/* the user window (c4m.js kpa ...): under C4IX's MMU a user task's memory
 * is at user addresses from uwin, mapped in its page tables; kernel mode
 * runs untranslated and reaches a user address through the window --
 * where udir, the kernel's map of all of them, says it is, or with no
 * map, the page uwin below it. A page the map has put out to swap is -1
 * here, and its bytes are the host's copy (kswap). kpa: physical, or -1 */
static void kmiss(machine *m, uword x);
static int halt(machine *m, i32 status);
static i32 kswap(machine *m, uword x) {
  if (!m->udir) return -1;
  i32 pte = kernel_pte(m, m->udir, (u32)x);
  return (pte & 1) || !(pte & PTE_SWAP) ? -1 : (i32)((u32)pte >> 12);
}
static i64 kpa(machine *m, uword x) {
  if (m->udir) {
    i64 p = kernel_phys(m, m->udir, (u32)x, 0);
    if (p < 0 && kswap(m, x) < 0) kmiss(m, x);
    return p;
  }
  uword off = x - m->uwin;
  return off < m->size ? (i64)off : -1;
}
static void kmiss(machine *m, uword x) {
  if (m->dv->halted) return;
  char a[24], b[24];
  machine_emits(m, "libjs: the kernel used user address "); machine_emits(m, hex8(a, x));
  machine_emits(m, ", which nothing is at (pc "); machine_emits(m, hex8(b, (uword)m->pc));
  machine_emits(m, "). Halting.\n");
  halt(m, -105);
}
static i64 kaddr(machine *m, uword x) { return m->uwin && x >= m->uwin ? kpa(m, x) : (i64)x; }
/* the host's copy of a page put out (devices.js swapRd8, swapWr8) */
static int swap_rd8(machine *m, i32 slot, u32 off) {
  swap_lock(m);
  u8 *p = swap_page(m, (u32)slot, 0);
  int v = p ? p[off & 4095] : 0;
  swap_unlock(m);
  return v;
}
static void swap_wr8(machine *m, i32 slot, u32 off, int v) {
  swap_lock(m);
  u8 *p = swap_page(m, (u32)slot, 1);
  if (p) p[off & 4095] = (u8)v;
  swap_unlock(m);
}
static i32 kld8(machine *m, uword x) {
  i64 p = kpa(m, x);
  if (p >= 0) return (int8_t)m->u8[p];
  i32 s = kswap(m, x);
  return s < 0 ? 0 : (int8_t)swap_rd8(m, s, (u32)x & 4095);
}
static void kst8(machine *m, uword x, i32 v) {
  i64 p = kpa(m, x);
  if (p >= 0) { m->u8[p] = (u8)v; return; }
  i32 s = kswap(m, x);
  if (s >= 0) swap_wr8(m, s, (u32)x & 4095, v & 255);
}
static word kbytes32(machine *m, uword x) {
  uword v = 0;
  for (int k = 0; k < WB; k++) v |= (uword)(kld8(m, x + (uword)k) & 255) << (8 * k);
  return (word)v;
}
static word kld32(machine *m, uword x) {
  i64 p = kpa(m, x);
  if (p < 0) return kswap(m, x) < 0 ? 0 : kbytes32(m, x);
  if ((p & (WB - 1)) == 0) return m->w[p >> WSH];
  return kbytes32(m, x);
}
static void kst32(machine *m, uword x, word v) {
  i64 p = kpa(m, x);
  if (p >= 0 && (p & (WB - 1)) == 0) { m->w[p >> WSH] = v; return; }
  if (p < 0 && kswap(m, x) < 0) return;
  for (int k = 0; k < WB; k++) kst8(m, x + (uword)k, (i32)(v >> (8 * k)));
}
/* does the block opcode at the top of the stack touch the window? */
static int kwin(machine *m, word op) {
  word sp = m->sp; uword w = m->uwin; word n = m_ldw(m, sp);
  if (n <= 0) return 0;
  uword d = (uword)m_ldw(m, sp + 2 * WB);
  if (d >= w || d + (uword)n > w) return 1;
  if (op == OP_MSET) return 0;
  uword s = (uword)m_ldw(m, sp + WB);
  return s >= w || s + (uword)n > w;
}
/* MSET, MCMP and MCPY in kernel mode with a user address among them: a
 * run at a time that stays inside a page of each */
static u32 krun(uword x, uword y, word left) {
  i64 r = left;
  if ((i64)(4096 - (x & 4095)) < r) r = (i64)(4096 - (x & 4095));
  if ((i64)(4096 - (y & 4095)) < r) r = (i64)(4096 - (y & 4095));
  return (u32)r;
}
static int kblock(machine *m, word op) {
  word sp = m->sp; u8 *u = m->u8;
  if (op == OP_MSET) {
    word d = m_ldw(m, sp + 2 * WB), v = m_ldw(m, sp + WB) & 0xff, n = m_ldw(m, sp);
    for (word k = 0; k < n;) {
      uword x = (uword)d + (uword)k; u32 r = krun(x, x, n - k);
      i64 p = kaddr(m, x);
      if (p >= 0x1000 && p + r <= m->size) memset(u + p, (int)v, r);
      else for (u32 j = 0; j < r; j++) m_st8(m, (word)(x + j), v);   /* a page put out: the host's copy */
      k += (word)r;
    }
    m->a = d;
    return STOP_BUDGET;
  }
  if (op == OP_MCMP) {
    word p = m_ldw(m, sp + 2 * WB), q = m_ldw(m, sp + WB), n = m_ldw(m, sp);
    word r = 0;
    for (word k = 0; k < n; k++) {
      word x = (m_ld8(m, (word)((uword)p + (uword)k)) & 255) - (m_ld8(m, (word)((uword)q + (uword)k)) & 255);
      if (x) { r = x; break; }
    }
    m->a = r;
    return STOP_BUDGET;
  }
  word d = m_ldw(m, sp + 2 * WB), s = m_ldw(m, sp + WB), n = m_ldw(m, sp);
  if (d > s && (i64)d < (i64)s + n) {
    for (word k = 0; k < n; k++) m_st8(m, (word)((uword)d + (uword)k), m_ld8(m, (word)((uword)s + (uword)k)));
  } else {
    for (word k = 0; k < n;) {
      uword x = (uword)d + (uword)k, y = (uword)s + (uword)k; u32 r = krun(x, y, n - k);
      i64 pd = kaddr(m, x), ps = kaddr(m, y);
      if (pd >= 0x1000 && ps >= 0 && pd + r <= m->size && ps + r <= m->size) memmove(u + pd, u + ps, r);
      else for (u32 j = 0; j < r; j++) m_st8(m, (word)(x + j), m_ld8(m, (word)(y + j)));
      k += (word)r;
    }
  }
  m->a = d;
  return STOP_BUDGET;
}

static void rom_store(machine *m, word x, word v) {
  m->dv->rom_stores++;
  if (m->dv->rom_reported) return;
  m->dv->rom_reported = 1;
  char a[24], b[24], c[24], d[24], e[24], f[24];
  machine_emits(m, "libjs: store of "); machine_emits(m, hex8(a, (uword)v));
  machine_emits(m, " to ROM "); machine_emits(m, hex8(b, (uword)x));
  machine_emits(m, " at pc "); machine_emits(m, hex8(c, (uword)(m->pc - WB)));
  machine_emits(m, " dropped (bp "); machine_emits(m, hex8(d, (uword)m->bp));
  machine_emits(m, ", sp "); machine_emits(m, hex8(f, (uword)m->sp));
  machine_emits(m, ", mode "); machine_emits(m, dec(e, m->mode));
  machine_emits(m, ", cycle "); machine_emits(m, dec(e, m->cycle));
  machine_emits(m, "). Later ones are dropped silently.\n");
}

static void guard_hit(machine *m, word x, word n, int write, word v, word op, word from) {
  char a[24], b[24], c[24], d[24];
  u32 lo = m->dv->stack_base, hi = m->dv->stack_top_reg;
  m->dv->stack_guard = 0;
  machine_emits(m, "libjs: ");
  if (op == OP_TLEV) { machine_emits(m, "TLEV to a stack at "); machine_emits(m, hex8(a, (uword)x)); }
  else if (op) {
    char nm[5]; memcpy(nm, OPNAMES + op * 5, 4); nm[4] = 0;
    for (int k = 3; k >= 0 && nm[k] == ' '; k--) nm[k] = 0;
    machine_emits(m, nm); machine_emits(m, " of "); machine_emits(m, dec(d, n));
    machine_emits(m, " bytes at "); machine_emits(m, hex8(a, (uword)from));
    machine_emits(m, write ? " writes " : " reads "); machine_emits(m, hex8(b, (uword)x));
  } else if (write) { machine_emits(m, "write of "); machine_emits(m, hex8(a, (uword)v)); machine_emits(m, " to "); machine_emits(m, hex8(b, (uword)x)); }
  else { machine_emits(m, n == 1 ? "read of a byte at " : "read of a word at "); machine_emits(m, hex8(a, (uword)x)); }
  machine_emits(m, ", at pc "); machine_emits(m, hex8(c, (uword)(m->pc - WB)));
  machine_emits(m, ": that is the boot stack, guarded ("); machine_emits(m, hex8(a, lo));
  machine_emits(m, "-"); machine_emits(m, hex8(b, hi - 1)); machine_emits(m, ").\n");
  machine_emits(m, "libjs:   Something ran off the end of the heap -- a program writing past its block? Halting.\n");
  machine_emits(m, "libjs:   bp "); machine_emits(m, hex8(a, (uword)m->bp));
  machine_emits(m, ", sp "); machine_emits(m, hex8(b, (uword)m->sp));
  machine_emits(m, ", a "); machine_emits(m, hex8(c, (uword)m->a));
  machine_emits(m, ", mode "); machine_emits(m, dec(d, m->mode));
  machine_emits(m, ", after "); machine_emits(m, dec(d, m->cycle)); machine_emits(m, " cycles.\n");
  if (!m->dv->heap_base_reg || (uword)(m->pc - WB) < (u32)m->dv->heap_base_reg) machine_stacktrace(m);
  else machine_emits(m, "libjs:   pc is in the heap, a program's code: no symbols name it.\n");
  m->dv->halted = 1;
  m->dv->status = -103;
}
static int guard_range(machine *m, word op, word x, word n, int write) {
  guard_hit(m, x > (word)m->dv->stack_base ? x : (word)m->dv->stack_base, n, write, 0, op, x);
  return STOP_HALT;
}

/* the bytes of a C string at addr, in place: how many (to the arena's end at most) */
static u32 cstr(machine *m, uword addr, u32 max, const u8 **p) {
  if (C4PC_HAS_MMU && (m->vm || (m->uwin && addr >= m->uwin))) {
    /* a copy, a byte at a time: in user mode translated (a fault goes to
     * the user loop's catch), in the window through kpa (c4m.js cstring).
     * PRTF holds its format while it asks for its %s strings: a few
     * buffers, taken in turn */
    static obuf bufs[8]; static int next;
    obuf *o = &bufs[next++ & 7];
    o->n = 0;
    for (u32 k = 0; k < max; k++) {
      int b = (m->vm ? mmu_ld8(m->mmu, (word)(addr + k)) : kld8(m, addr + k)) & 255;
      if (!b) break;
      ob_put(o, b);
    }
    *p = o->b ? o->b : m->u8;
    return o->n;
  }
  if (addr >= m->size) { *p = m->u8; return 0; }
  u32 lim = m->size - (u32)addr < max ? m->size - (u32)addr : max, n = 0;
  while (n < lim && m->u8[addr + n]) n++;
  *p = m->u8 + addr;
  return n;
}
static u32 cstr_cb(void *ctx, uword addr, const u8 **p) { return cstr((machine *)ctx, addr, 1u << 20, p); }

/* ---------------------------------------------------------------- */
/* traps (c4m.js trap): false when there is no handler, and nothing moves */
static int trap(machine *m, word type, word param, word handler) {
  if (!handler) { m->missed_traps++; return 0; }
  word t = m->sp;
  word sp = t - TRAP_OFFSET * WB;
  /* With the MMU on, a trap is the way into kernel mode: from user mode
   * it switches to the kernel's stack (KSP), as an x86 does, and the
   * handler runs untranslated */
  mmu *u = m->mmu;
  int kern = C4PC_HAS_MMU && u && u->on;
  if (kern) {
    if (m->mode && u->ksp) sp = u->ksp - TRAP_OFFSET * WB;
    m->vm = 0;
  }
#define PUSHW(v) (sp -= WB, W(sp) = (v))
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
  sp -= WB;
  W(sp) = sp;                          /* bp+0 */
#undef PUSHW
  m->bp = sp;
  sp -= m_ldw(m, handler + WB) * WB;
  m->sp = sp;
  m->pc = handler + 2 * WB;
  m->trap_pc = m->pc;
  if (kern) m->mode = 0;
  m->irq_hold++;
  return 1;
}
int eng_trap(machine *m, word type, word param, word handler) { return trap(m, type, param, handler); }
static void hirq(machine *m, i32 param) {
  trap(m, TRAP_HARD_IRQ, param, m->cycle_handler);
  m->cycle_interval = 0;
  m->mode = 0;
}
static void deliver_signal(machine *m) {
  word sig = m->pending_signal;
  trap(m, TRAP_SIGNAL, sig, sig >= 0 && sig < 64 ? m->sigh[sig] : 0);
  m->cycle_interval = 0;
  m->mode = 0;
  m->pending_signal = 0;
}
static int pm_trap(machine *m, word op) {
  trap(m, TRAP_PM, op, m->trap_handler);
  m->cycle_interval = 0;
  m->mode = 0;
  return STOP_BUDGET;
}
/* EXIT. With CPUs, only CPU 0's ends the machine, as main returning ends
 * a program; another's halts that processor (c4mp's rule, c4m.js halt) */
static int halt(machine *m, i32 status) {
  if (m->smp && m->cpu_id != 0) {
    m->cpu_state = CPU_HALT;
    m->cpu_status = status;
    return STOP_CPUHALT;
  }
  m->dv->halted = 1;
  m->dv->status = status;
  return STOP_HALT;
}
int eng_halt(machine *m, i32 status) { return halt(m, status); }

i32 machine_info(machine *m) {
  /* --c4m (c4pc-host): what native c4m says (c4m.c c4_info), so that a
   * guest takes c4m's way, files by OPEN and no devices: for holding the
   * 64-bit machine against c4m on c4m's own tests */
  if (m->c4m_info) return C4I_C4M | C4I_HRT | C4I_SIG | C4I_FLT | C4I_PROT | (m->no_fpu ? 0 : C4I_FPU) | (m->trap_handler ? C4I_TRAPH : 0);
  return C4I_C4M | C4I_C4MJS | C4I_HRT | C4I_SIG | C4I_FLT | C4I_PROT | C4I_PIT | C4I_DRV |
         (m->no_fpu ? 0 : C4I_FPU) | (m->mmu ? C4I_MMU : 0) | (m->accel ? C4I_ACCEL : 0) | (m->gui ? C4I_GUI : 0) | (m->snd ? C4I_SND : 0) |
         (m->dv->nblk ? C4I_BLK : 0) | (m->dv->hv_rd ? (m->dv->hv_id ? C4I_HVG : C4I_HV) : 0) | (m->dv->net ? C4I_NET : 0) |
         (m->trap_handler ? C4I_TRAPH : 0) | (m->smp ? C4I_SMP : 0) | (m->smp && m->smp->par ? C4I_PAR : 0);
}

void machine_stacktrace(machine *m) {
  char a[24];
  word pc = m->pc - WB, bp = m->bp;
  machine_emits(m, "c4m: stacktrace:\n");
  for (int depth = 0; depth < 64; depth++) {
    struct sym *s = 0;
    int lo = 0, hi = m->dv->nsyms - 1;
    while (lo <= hi) { int mid = (lo + hi) >> 1; if (m->dv->syms[mid].addr <= (uword)pc) { s = &m->dv->syms[mid]; lo = mid + 1; } else hi = mid - 1; }
    for (int k = 0; k <= depth; k++) machine_emits(m, " ");
    machine_emits(m, s ? s->name : "?");
    machine_emits(m, "() [");
    { char *h = hex8(a, s ? (uword)s->addr : (uword)pc); /* c4m.js: no padding */ while (h[2] == '0' && h[3]) { memmove(h + 2, h + 3, strlen(h + 3) + 1); } machine_emits(m, h); }
    machine_emits(m, "]\n");
    if (!s || !strcmp(s->name, "main") || bp < 0x1000 || (uword)bp >= m->size) break;
    pc = m_ldw(m, bp + WB) - WB;
    bp = m_ldw(m, bp);
  }
}

static int null_jump(machine *m, word pc) {
  char a[24], b[24], c[24], d[24];
  word ret = m_ldw(m, m->sp);
  machine_emits(m, "libjs: jump to "); machine_emits(m, hex8(a, (uword)pc));
  machine_emits(m, ", the null guard: nothing runs there. Halting.\n");
  machine_emits(m, "libjs:   bp "); machine_emits(m, hex8(a, (uword)m->bp));
  machine_emits(m, ", sp "); machine_emits(m, hex8(b, (uword)m->sp));
  machine_emits(m, ", a "); machine_emits(m, hex8(c, (uword)m->a));
  machine_emits(m, ", mode "); machine_emits(m, dec(d, m->mode));
  machine_emits(m, ", after "); machine_emits(m, dec(d, m->cycle));
  machine_emits(m, " cycles; top of stack "); machine_emits(m, hex8(a, (uword)ret));
  machine_emits(m, ", the return address if it was a call.\n");
  if (ret >= 0x1000 && (uword)ret < m->size) { word at = m->pc; m->pc = ret; machine_stacktrace(m); m->pc = at; }
  return halt(m, -102);
}
static int tlev_wedge(machine *m, word pc) {
  char a[24], b[24], c[24], d[24];
  machine_emits(m, "libjs: TLEV at "); machine_emits(m, hex8(a, (uword)pc));
  machine_emits(m, " restores its own address -- this task can never advance.\n");
  machine_emits(m, "libjs:   bp "); machine_emits(m, hex8(a, (uword)m->bp));
  machine_emits(m, ", sp "); machine_emits(m, hex8(b, (uword)m->sp));
  machine_emits(m, ", a "); machine_emits(m, hex8(c, (uword)m->a));
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
    if (m->u8[x] != saved[x]) { rom_store(m, (word)x, m->u8[x]); m->u8[x] = saved[x]; }
  }
  xfree(saved);
  return r;
}

static int blocked_read(machine *m) {
  m->pc -= WB;
  word iv = m->cycle_interval;
  if (iv > 0) {
    i64 target = (m->cycle / iv + 1) * iv - 1;
    if (target > m->cycle) { m->skipped += target - m->cycle; m->cycle = target; }
    return STOP_BUDGET;
  }
  if (m->dv->pit_ms && plat_now_ms() >= m->dv->pit_next) {
    i64 target = m->cycle + (4095 - (m->cycle % 4096));
    if (target > m->cycle) { m->skipped += target - m->cycle; m->cycle = target; }
    return STOP_BUDGET;
  }
  return STOP_INPUT;
}

static void opv_note(machine *m, word op) {
  char a[5], b[6];
  memcpy(a, OPNAMES + OP_OPCD * 5, 4); a[4] = 0;
  if (op >= 0 && op < INS_SIZE) { memcpy(b, OPNAMES + op * 5, 5); b[4] = 0; } else { b[0] = '?'; b[1] = 0; }
  machine_emits(m, a); machine_emits(m, " does not support opcodes requiring arguments (");
  machine_emits(m, b); machine_emits(m, " given)\n");
}
/* OPCD handed an opcode that carries an operand: refused with a (usually
 * missed) OPV (c4m.js opvTrap, for the user loop) */
void eng_opv_trap(machine *m, word op) {
  opv_note(m, op);
  trap(m, TRAP_OPV, op, m->trap_handler);
  m->cycle_interval = 0; m->mode = 0;
}

/* the rare opcodes (c4m.js sys) */
static int sys_ops(machine *m, word op, word sp);
static int sys(machine *m, word op) {
  word sp = m->sp;
  /* Under the MMU, the opcodes that run the machine rather than the task
   * are the kernel's: starting, halting and waking CPUs, IPIs, and the
   * cycle interrupt's configuration (docs/c4ix-mmu.md 6) */
  if (m->vm && (op == OP_C4CF || (op >= OP_CPUS && op <= OP_CPUH) || (op >= OP_CWAI && op <= OP_IPI)))
    return pm_trap(m, op);
  if (m->par_ap && !m->serving) { int r = par_sys(m, op); if (r >= 0) return r; }
  if (op >= OP_CPUI && op <= OP_IPI && m->smp) return smp_op(m, op);
  /* the rest under the device lock, when there is one (above): never the
   * CPUs' own (their waits), nor another's asking (CPU 0 answers it) */
  if (!eng_lock_fn) return sys_ops(m, op, sp);
  eng_lock_fn(1);
  int r = sys_ops(m, op, sp);
  eng_lock_fn(-1);
  return r;
}
static int sys_ops(machine *m, word op, word sp) {
  switch (op) {
    case OP_OPEN:
      if (m->mode) return pm_trap(m, op);
      m->dv->disk_flags = (i32)m_ldw(m, sp);
      m->a = dev_disk_open(m, A32(m_ldw(m, sp + WB)));
      return STOP_BUDGET;
    case OP_READ: {
      if (m->mode) return pm_trap(m, op);
      word buf = m_ldw(m, sp + WB), len = m_ldw(m, sp);
      if (len > 0 && in_guard(m, buf, len)) return guard_range(m, OP_READ, buf, len, 1);
      word fd0 = m->dv->disk_fd, addr0 = m->dv->disk_addr;
      m->dv->disk_fd = (i32)m_ldw(m, sp + 2 * WB);
      m->dv->disk_addr = (i32)A32(buf);
      word r = is_rom(buf, len) ? rom_read(m, N32(len)) : dev_disk_read(m, N32(len));
      m->dv->disk_fd = fd0; m->dv->disk_addr = addr0;
      if (r == -2) return blocked_read(m);
      m->a = r;
      return STOP_BUDGET;
    }
    case OP_CLOS:
      if (m->mode) return pm_trap(m, op);
      dev_write32(m, DISK_CLOSE_REG, (i32)m_ldw(m, sp));
      m->a = dev_read32(m, DISK_CLOSE_REG);
      return STOP_BUDGET;
    case OP_PRTF: {
      if (m->mode) return pm_trap(m, op);
      word r = m_ldw(m, m->pc + WB);
      if (r > 7) { machine_emits(m, "Too many arguments to printf!\n"); return halt(m, -1); }
      word t = sp + r * WB;
      word args[6];
      for (int k = 2; k <= 7; k++) args[k - 2] = m_ldw(m, t - k * WB);
      const u8 *f; u32 fl = cstr(m, (uword)m_ldw(m, t - WB), 1u << 20, &f);
      static obuf out;
      out.n = 0;
      fmt_format(&out, f, fl, args, 6, cstr_cb, m);
      machine_emit(m, out.b, out.n);
      m->a = (word)out.n;
      return STOP_BUDGET;
    }
    case OP_MALC:
      if (m->mode) return pm_trap(m, op);
      m->a = (word)heap_alloc(m->heap, N32(m_ldw(m, sp)));
      return STOP_BUDGET;
    case OP_FREE:
      if (m->mode) return pm_trap(m, op);
      heap_free(m->heap, A32(m_ldw(m, sp)));
      return STOP_BUDGET;
    case OP_RALC:
      if (m->mode) return pm_trap(m, op);
      m->a = (word)heap_realloc(m->heap, m->u8, A32(m_ldw(m, sp + WB)), N32(m_ldw(m, sp)));
      return STOP_BUDGET;
    case OP_MSET: {
      if (m->vm) return mmu_block(m->mmu, op);
      if (m->uwin && kwin(m, op)) return kblock(m, op);
      word d = m_ldw(m, sp + 2 * WB), v = m_ldw(m, sp + WB), n = m_ldw(m, sp);
      if (n > 0) {
        if (in_guard(m, d, n)) return guard_range(m, OP_MSET, d, n, 1);
        if (d >= 0x1000) { if ((uword)d < m->size) memset(m->u8 + d, (int)(v & 0xff), (uword)n <= m->size - (uword)d ? (size_t)n : m->size - (size_t)d); }
        else for (word k = 0; k < n; k++) m_st8(m, d + k, v);
      }
      m->a = d;
      return STOP_BUDGET;
    }
    case OP_MCMP: {
      if (m->vm) return mmu_block(m->mmu, op);
      if (m->uwin && kwin(m, op)) return kblock(m, op);
      word p = m_ldw(m, sp + 2 * WB), q = m_ldw(m, sp + WB), n = m_ldw(m, sp);
      if (n > 0 && in_guard(m, p, n)) return guard_range(m, OP_MCMP, p, n, 0);
      if (n > 0 && in_guard(m, q, n)) return guard_range(m, OP_MCMP, q, n, 0);
      word r = 0;
      for (word k = 0; k < n; k++) {
        uword x = (uword)(p + k), y = (uword)(q + k);
        i32 c = (i32)(x < m->size ? m->u8[x] : 0) - (i32)(y < m->size ? m->u8[y] : 0);
        if (c) { r = c; break; }
      }
      m->a = r;
      return STOP_BUDGET;
    }
    case OP_MCPY: {
      if (m->vm) return mmu_block(m->mmu, op);
      if (m->uwin && kwin(m, op)) return kblock(m, op);
      word d = m_ldw(m, sp + 2 * WB), s = m_ldw(m, sp + WB), n = m_ldw(m, sp);
      if (n > 0) {
        if (in_guard(m, s, n)) return guard_range(m, OP_MCPY, s, n, 0);
        if (in_guard(m, d, n)) return guard_range(m, OP_MCPY, d, n, 1);
        uword ud = (uword)d, us = (uword)s, un = (uword)n;
        if (is_rom(d, n)) {
          for (word k = 0; k < n; k++) {
            word x = d + k; u8 b = (uword)(s + k) < m->size ? m->u8[s + k] : 0;
            if (is_rom(x, 1)) rom_store(m, x, b); else if ((uword)x < m->size) m->u8[x] = b;
          }
        } else if (ud > us && ud < us + un) {
          for (uword k = 0; k < un; k++) if (ud + k < m->size) m->u8[ud + k] = us + k < m->size ? m->u8[us + k] : 0;
        } else if (us < m->size && ud < m->size) {
          /* copyWithin: clamped to the arena at both ends */
          uword c = un;
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
      return halt(m, (i32)m_ldw(m, sp));
    case OP_PUTC: {
      if (m->mode) return pm_trap(m, op);
      u8 b = m->u8[(u32)sp & (m->size - 1)];
      machine_emit(m, &b, 1);
      m->a = b;
      return STOP_BUDGET;
    }
    case OP_PUTS: {
      if (m->mode) return pm_trap(m, op);
      const u8 *p; u32 n = cstr(m, (uword)m_ldw(m, sp), 1u << 20, &p);
      machine_emit(m, p, n);
      u8 nl = 10; machine_emit(m, &nl, 1);
      m->a = 10;
      return STOP_BUDGET;
    }
    case OP_STRC: machine_stacktrace(m); return STOP_BUDGET;
    case OP_ITH: {
      if (m->mode) return pm_trap(m, op);
      word old = m->trap_handler;
      m->trap_handler = m_ldw(m, sp);
      m->a = old;
      return STOP_BUDGET;
    }
    case OP__OPC: {
      char s[17]; const u8 *q; u32 n = cstr(m, (uword)m_ldw(m, sp), 16, &q);
      memcpy(s, q, n); s[n] = 0;
      m->a = dev_opcode_by_name(s);
      return STOP_BUDGET;
    }
    case OP__TRP:
      trap(m, m_ldw(m, sp + WB), m_ldw(m, sp), m->trap_handler);
      return STOP_BUDGET;
    case OP_C4CF: {
      word opt = m_ldw(m, sp + WB), val = m_ldw(m, sp);
      if (opt == 0) { m->a = m->cycle_interval; m->cycle_interval = val; }
      else if (opt == 1) { m->a = m->cycle_handler; m->cycle_handler = val; }
      else if (opt == 3) { m->a = m->trap_restores_interval; m->trap_restores_interval = val; }
      else { machine_emits(m, "c4m: C4CF issue\n"); return halt(m, -100); }
      return STOP_BUDGET;
    }
    case OP_TIME: m->a = dev_read32(m, TIME_MS_REG); return STOP_BUDGET;
    case OP_SIGH: {
      if (m->mode) return pm_trap(m, op);
      word sig = m_ldw(m, sp + WB), h = m_ldw(m, sp);
      if (sig >= 0 && sig < 64) { m->a = m->sigh[sig]; m->sigh[sig] = h; } else m->a = 0;
      return STOP_BUDGET;
    }
    case OP_SIGI: m->a = 2; return STOP_BUDGET;
    case OP_USLP:
      if (m->mode) return pm_trap(m, op);
      dev_write32(m, USLP_US_REG, (i32)m_ldw(m, sp));
      m->a = 0;
      return STOP_SLEEP;
    case OP_INFO:
      if (m->mode) return pm_trap(m, op);
      m->a = machine_info(m);
      return STOP_BUDGET;
    case OP_OPSL: m->a = OPNAMES_ROM; return STOP_BUDGET;
    case OP_C4IV: return STOP_BUDGET;
    case OP_FLT: m->a = flt_op(m, (uword)sp); return STOP_BUDGET;
    case OP_FPU: {
      word v;
      if (!m->no_fpu && fpu_op(m, (uword)sp, &v)) { m->a = v; return STOP_BUDGET; }
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
        word bp = m->bp;
        word pc = m_ldw(m, bp + 2 * WB);
        if (pc == m->pc - WB) return tlev_wedge(m, pc);
        word nsp = m_ldw(m, bp + 3 * WB), nbp = m_ldw(m, bp + 4 * WB), na = m_ldw(m, bp + 5 * WB);
        m->sp = nsp; m->bp = nbp; m->a = na; m->pc = pc;
        return STOP_BUDGET;
      }
      if (m->irq_hold > 0) m->irq_hold--;
      word bp = m->bp;
      word pc = m_ldw(m, bp + 2 * WB);
      if (pc == m->pc - WB) return tlev_wedge(m, pc);
      if (in_guard(m, m_ldw(m, bp + 3 * WB), WB)) { guard_hit(m, m_ldw(m, bp + 3 * WB), WB, 0, 0, OP_TLEV, 0); return STOP_HALT; }
      m->sp = m_ldw(m, bp + 3 * WB);
      m->bp = m_ldw(m, bp + 4 * WB);
      m->a = m_ldw(m, bp + 5 * WB);
      m->mode = m_ldw(m, bp + 6 * WB);
      if (m->trap_restores_interval) m->cycle_interval = m_ldw(m, bp + 9 * WB);
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
  if (op == (word)NULL_GUARD_OP) return null_jump(m, m->pc - WB);
  if (op >= 0 && op < INS_SIZE && !m->trap_handler) {
    char nm[6], a[24], buf[24];
    memcpy(nm, OPNAMES + op * 5, 4); nm[4] = 0;
    machine_emits(m, "c4m: "); machine_emits(m, nm); machine_emits(m, " (opcode ");
    machine_emits(m, dec(buf, op)); machine_emits(m, ") at ");
    { char *h = hex8(a, (uword)(m->pc - WB)); while (h[2] == '0' && h[3]) memmove(h + 2, h + 3, strlen(h + 3) + 1); machine_emits(m, h); }
    machine_emits(m, " is not an instruction this machine has,\n");
    machine_emits(m, "c4m: and no trap handler is installed to emulate it. Halting.\n");
    return halt(m, -1);
  }
  trap(m, TRAP_ILLOP, op, m->trap_handler);
  m->mode = 0;
  return STOP_BUDGET;
}

int eng_sys(machine *m, word op) { return sys(m, op); }

static int need_checks(machine *m) {
  if (m->par_ap) return m->cycle_interval || m->pending_signal || m->ipi_pending;   /* the devices' are CPU 0's */
  return m->cycle_interval || m->dv->pit_ms || m->pending_signal || m->dv->mbox_irq || m->ipi_pending ||
         (m->gui && gui_irq_pending(m->gui));
}
int eng_need_checks(machine *m) { return need_checks(m); }

static i64 next_multiple(i64 cycle, word interval) { return (cycle / interval + 1) * interval; }

/* the boundary checks, in c4bb's order (c4m.js boundary): inlined into the
 * loop (out of it, the loop ran 7% slower); mmu.c's calls eng_boundary */
static inline __attribute__((always_inline)) i64 boundary(machine *m, i64 cycle, i64 next_irq) {
  int tlev = (m->vm ? mmu_peek(m->mmu, m->pc) : W(m->pc)) == OP_TLEV;   /* TLEV is never interrupted */
  int held = tlev || m->irq_hold > 0;
  word interval = m->cycle_interval;
  int fired = 1;
  int dv = !m->par_ap;                 /* the devices' interrupts: CPU 0's alone on threads */
  if (blk_abusy && (cycle & 4095) == 0) blk_apump();   /* the disks' commands that do not wait (dev.c) */
  if (dv && m->dv->pit_ms && (cycle & 4095) == 0 && !held && plat_now_ms() >= m->dv->pit_next) {
    m->dv->pit_next = plat_now_ms() + m->dv->pit_ms;
    hirq(m, 1);
  } else if (dv && m->dv->mbox_irq && m->cycle_handler && !held) {
    m->dv->mbox_irq = 0;
    hirq(m, 2);
  } else if (dv && m->gui && m->cycle_handler && gui_irq_pending(m->gui) && !tlev) {
    /* the display's events: not held for a handler (c4m.js boundary) */
    gui_irq_taken(m->gui);
    hirq(m, 3);
  } else {
    int irq = 0;
    if (interval > 0 && cycle >= next_irq) { next_irq = next_multiple(cycle, interval); irq = !tlev; }
    if (irq) hirq(m, 0);
    else if (m->ipi_pending && !tlev && (!m->ipi_at_trap || m->pc == m->trap_pc)) {
      /* another CPU's interrupt (c4mp's order: after the tick), not held
       * back by the preemption mask: a peer asking */
      m->ipi_pending = 0;
      hirq(m, 1);                      /* c4mp's HIRQ_IPI */
    }
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
  if ((dv && m->dv->mbox_irq && m->cycle_handler && !m->irq_hold) || m->ipi_pending || m->pending_signal ||
      (dv && m->gui && m->cycle_handler && gui_irq_pending(m->gui))) return 0;
  i64 next = INT64_MAX;
  if (interval > 0) next = next_irq;
  if (dv && m->dv->pit_ms) { i64 pit = (cycle / 4096 + 1) * 4096; if (pit < next) next = pit; }
  return next;
}
i64 eng_boundary(machine *m, i64 cycle, i64 next_irq) { return boundary(m, cycle, next_irq); }

/* OPCD of one of the loop's own opcodes with no operand (c4m.js opcd) */
static void opcd(machine *m, word op) {
  word sp = m->sp, a = m->a, x = 0;
  switch (op) {
    case OP_LEV: { word bp = m->bp; m->bp = W(bp); m->pc = W(bp + WB); sp = bp + 2 * WB; break; }
    case OP_LI: a = m_ldw(m, a); break;
    case OP_LC: a = m_ld8(m, a); break;
    case OP_SI: x = W(sp); sp += WB; m_stw(m, x, a); break;
    case OP_SC: x = W(sp); sp += WB; m_st8(m, x, a); a = (int8_t)a; break;
    case OP_LH: a = m_ld16(m, a); break;
    case OP_LHU: a = m_ld16(m, a) & 0xffff; break;
    case OP_SH: x = W(sp); sp += WB; m_st16(m, x, a); break;
    case OP_LSR: x = W(sp); sp += WB; a = (word)((uword)x >> (a & (WBITS - 1))); break;
    case OP_LCU: a = m_ld8(m, a) & 255; break;
    case OP_PSH: sp -= WB; W(sp) = a; break;
    case OP_LIP: a = m_ldw(m, a); sp -= WB; W(sp) = a; break;
    case OP_ADDL: x = (word)((uword)W(sp) + (uword)a); sp += WB; a = m_ldw(m, x); break;
    case OP_POPA: a = W(sp); sp += WB; break;
    case OP_JMPA: m->pc = a; break;
    case OP__JMP: m->pc = W(sp); sp += WB; break;
    case OP__ADJ: sp = (word)((uword)sp + ((uword)W(sp) << WSH)); break;
    case OP_C4CY: a = (word)m->cycle; break;
    default: {
      word t = W(sp); sp += WB;
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
        case OP_SHL: a = (word)((uword)t << (a & (WBITS - 1))); break;
        case OP_SHR: a = t >> (a & (WBITS - 1)); break;
        case OP_ADD: a = (word)((uword)t + (uword)a); break;
        case OP_SUB: a = (word)((uword)t - (uword)a); break;
        case OP_MUL: a = (word)((uword)t * (uword)a); break;
        case OP_DIV: a = a == 0 ? 0 : a == -1 ? (word)((uword)0 - (uword)t) : t / a; break;
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
  /* with CPUs, the boot CPU's run is the round robin's (smp.c) */
  if (m->smp && m->cpu_id == 0 && !m->smp->running) return smp_run(m->smp, max_cycles);
  if (m->mmu && m->mmu->on) return mmu_run(m->mmu, max_cycles);
  return eng_run_plain(m, max_cycles);
}

/* the interpreter's loop: every guest's, and C4IX's kernel mode under the
 * MMU, which leaves it with STOP_SWITCH at a TLEV into user mode */
int eng_run_plain(machine *m, i64 max_cycles) {
  if (m->dv->halted) return STOP_HALT;
  word *const mw = m->w;
  u8 *const mu = m->u8;
  const uword wm = m->wmask;
#define MW(x) mw[((uword)(x) >> WSH) & wm]
  word pc = m->pc, sp = m->sp, bp = m->bp, a = m->a;
  i64 cycle = m->cycle;
  const i64 end = cycle + max_cycles;
  word interval = m->cycle_interval;
  i64 next_irq = interval > 0 ? next_multiple(cycle, interval) : 0;
  int chk = need_checks(m);
  i64 next_chk = 0, lim = -1;
  int stop = STOP_BUDGET;
  word op = 0, x = 0;
  word glo = guard_lo(m);
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
    pc += WB;
    switch (op) {
      case OP_LEA: a = (word)((uword)bp + ((uword)MW(pc) << WSH)); pc += WB; break;
      case OP_IMM: a = MW(pc); pc += WB; break;
      case OP_JMP: pc = MW(pc); break;
      case OP_JSR: sp -= WB; MW(sp) = pc + WB; pc = MW(pc); break;
      case OP_BZ: pc = a ? pc + WB : MW(pc); break;
      case OP_BNZ: pc = a ? MW(pc) : pc + WB; break;
      case OP_ENT: sp -= WB; MW(sp) = bp; bp = sp; sp = (word)((uword)sp - ((uword)MW(pc) << WSH)); pc += WB; break;
      case OP_ADJ: sp = (word)((uword)sp + ((uword)MW(pc) << WSH)); pc += WB; break;
      case OP_LEV: sp = bp; bp = MW(sp); pc = MW(sp + WB); sp += 2 * WB; break;
      case OP_LI:
        if ((a & (WB - 1)) == 0 && a >= 0x1000 && a < glo) a = mw[(uword)a >> WSH];
        else { SAVE(); a = m_ldw(m, a); if (m->dv->halted) { stop = STOP_HALT; goto out; } }
        break;
      case OP_LC:
        if (a >= 0x1000 && a < glo) a = (int8_t)mu[a];
        else { SAVE(); a = m_ld8(m, a); if (m->dv->halted) { stop = STOP_HALT; goto out; } }
        break;
      case OP_SI:
        x = MW(sp); sp += WB;
        if ((x & (WB - 1)) == 0 && x >= 0x1000 && x < glo) mw[(uword)x >> WSH] = a;
        else { SAVE(); m_stw(m, x, a); if (m->dv->halted) { stop = STOP_HALT; goto out; } RECHECK(); }
        break;
      case OP_SC:
        x = MW(sp); sp += WB;
        if (x >= 0x1000 && x < glo) mu[x] = (u8)a;
        else { SAVE(); m_st8(m, x, a); if (m->dv->halted) { stop = STOP_HALT; goto out; } RECHECK(); }
        a = (int8_t)a;
        break;
      case OP_LH:
        if (a >= 0x1000 && a < glo - 1) a = (int16_t)(mu[a] | mu[a + 1] << 8);
        else { SAVE(); a = m_ld16(m, a); if (m->dv->halted) { stop = STOP_HALT; goto out; } }
        break;
      case OP_LHU:
        if (a >= 0x1000 && a < glo - 1) a = mu[a] | mu[a + 1] << 8;
        else { SAVE(); a = m_ld16(m, a) & 0xffff; if (m->dv->halted) { stop = STOP_HALT; goto out; } }
        break;
      case OP_SH:
        x = MW(sp); sp += WB;
        if (x >= 0x1000 && x < glo - 1) { mu[x] = (u8)a; mu[x + 1] = (u8)(a >> 8); }
        else { SAVE(); m_st16(m, x, a); if (m->dv->halted) { stop = STOP_HALT; goto out; } RECHECK(); }
        break;
      case OP_LSR: a = (word)((uword)MW(sp) >> (a & (WBITS - 1))); sp += WB; break;
      case OP_LCU:
        if (a >= 0x1000 && a < glo) a = mu[a];
        else { SAVE(); a = m_ld8(m, a) & 255; if (m->dv->halted) { stop = STOP_HALT; goto out; } }
        break;
      case OP_PSH: sp -= WB; MW(sp) = a; break;
      case OP_OR: a = MW(sp) | a; sp += WB; break;
      case OP_XOR: a = MW(sp) ^ a; sp += WB; break;
      case OP_AND: a = MW(sp) & a; sp += WB; break;
      case OP_EQ: a = MW(sp) == a; sp += WB; break;
      case OP_NE: a = MW(sp) != a; sp += WB; break;
      case OP_LT: a = MW(sp) < a; sp += WB; break;
      case OP_GT: a = MW(sp) > a; sp += WB; break;
      case OP_LE: a = MW(sp) <= a; sp += WB; break;
      case OP_GE: a = MW(sp) >= a; sp += WB; break;
      case OP_SHL: a = (word)((uword)MW(sp) << (a & (WBITS - 1))); sp += WB; break;
      case OP_SHR: a = MW(sp) >> (a & (WBITS - 1)); sp += WB; break;
      case OP_ADD: a = (word)((uword)MW(sp) + (uword)a); sp += WB; break;
      case OP_SUB: a = (word)((uword)MW(sp) - (uword)a); sp += WB; break;
      case OP_MUL: a = (word)((uword)MW(sp) * (uword)a); sp += WB; break;
      case OP_DIV: { word t = MW(sp); a = a == 0 ? 0 : a == -1 ? (word)((uword)0 - (uword)t) : t / a; sp += WB; break; }
      case OP_MOD: { word t = MW(sp); a = a == 0 ? 0 : a == -1 ? 0 : t % a; sp += WB; break; }

      case OP_LDL: a = MW((uword)bp + ((uword)MW(pc) << WSH)); pc += WB; break;
      case OP_LDG: x = MW(pc); pc += WB; if ((x & (WB - 1)) == 0 && x >= 0x1000) a = mw[((uword)x >> WSH) & wm]; else { m->cycle = cycle; a = m_ldw(m, x); } break;
      case OP_PSHL: a = MW((uword)bp + ((uword)MW(pc) << WSH)); pc += WB; sp -= WB; MW(sp) = a; break;
      case OP_PSHG: x = MW(pc); pc += WB; if ((x & (WB - 1)) == 0 && x >= 0x1000) a = mw[((uword)x >> WSH) & wm]; else { m->cycle = cycle; a = m_ldw(m, x); } sp -= WB; MW(sp) = a; break;
      case OP_LEAP: a = (word)((uword)bp + ((uword)MW(pc) << WSH)); pc += WB; sp -= WB; MW(sp) = a; break;
      case OP_IMMP: a = MW(pc); pc += WB; sp -= WB; MW(sp) = a; break;
      case OP_LIP:
        if ((a & (WB - 1)) == 0 && a >= 0x1000 && a < glo) a = mw[(uword)a >> WSH];
        else { SAVE(); a = m_ldw(m, a); if (m->dv->halted) { stop = STOP_HALT; goto out; } }
        sp -= WB; MW(sp) = a;
        break;
      case OP_ADDL:
        x = (word)((uword)MW(sp) + (uword)a); sp += WB;
        if ((x & (WB - 1)) == 0 && x >= 0x1000 && x < glo) a = mw[(uword)x >> WSH];
        else { SAVE(); a = m_ldw(m, x); if (m->dv->halted) { stop = STOP_HALT; goto out; } }
        break;
      case OP_STL: MW((uword)bp + ((uword)MW(pc) << WSH)) = a; pc += WB; break;
      case OP_POPA: a = MW(sp); sp += WB; break;

      case OP_JSRI: x = MW(pc); sp -= WB; MW(sp) = pc + WB; SAVE(); pc = m_ldw(m, x); break;
      case OP_JSRS: x = MW(pc); sp -= WB; MW(sp) = pc + WB; pc = MW((uword)bp + ((uword)x << WSH)); break;
      case OP_JMPA: pc = a; break;
      case OP__JMP: pc = MW(sp); sp += WB; break;
      case OP__ADJ: sp = (word)((uword)sp + ((uword)MW(sp) << WSH)); break;
      case OP_C4CY: a = (word)cycle; break;

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
          a = (word)cycle;
          break;
        }
        if (is_plain_op(op)) {
          SAVE();
          opcd(m, op);
          LOAD();
          if (m->dv->halted) { stop = STOP_HALT; goto out; }
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
          if ((uword)(x - 1) < 8 && !m->no_fpu) { a = fpu_float(x, MW(sp + WB), MW(sp + 2 * WB)); break; }
          if ((uword)(x - 17) < 10 && x != 21 && !m->no_fpu) {
            /* a double's, if its addresses are all plain memory (c4m.js fpuPlain) */
            static const int FA = 0x2af, FB = 0x02f, FR = 0x14f;
            int bit = 1 << (x - 17);
            word pa = MW(sp + WB), pb = MW(sp + 2 * WB), pr = MW(sp + 3 * WB);
            int ok = 1;
            /* a double is 8 bytes: below glo with room for it */
            if ((FA & bit) && !((pa & (WB - 1)) == 0 && pa >= 0x1000 && pa < glo - (8 - WB))) ok = 0;
            if ((FB & bit) && !((pb & (WB - 1)) == 0 && pb >= 0x1000 && pb < glo - (8 - WB))) ok = 0;
            if ((FR & bit) && !((pr & (WB - 1)) == 0 && pr >= 0x1000 && pr < glo - (8 - WB))) ok = 0;
            if (ok) {
              word v;
              fpu_double_plain(mu, x, pa, (uword)pb, (uword)pr, &v);
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
        if (m->dv->halted) { stop = STOP_HALT; goto out; }
      }
    }
  }
out:
  SAVE();
  if (m->dv->halted) stop = STOP_HALT;
  return stop;
#undef MW
#undef SAVE
#undef LOAD
#undef RECHECK
}

/* ---------------------------------------------------------------- */
machine *machine_new(u32 arena_bytes, drive **drives, int ndrives, double mhz) {
  machine *m = xmalloc(sizeof *m);
  m->dv = m;
  u32 size = 1u << 20;
  while (size < arena_bytes && size < (1u << 31)) size <<= 1;
  m->size = size;
  m->u8 = xmalloc(size);
  m->w = (word *)m->u8;
  m->wmask = (size >> WSH) - 1;
  m->stack_top = size - ARGS_RESERVE;
  m->heap = heap_new();
  m->dv->drives = drives; m->dv->ndrives = ndrives;
  m->dv->fds = imap_new();
  m->dv->next_fd = 3;
  m->dv->swap_pages = imap_new();
  m->dv->swap_slots = 65536;
  i64 cpm = (i64)(mhz * 1000 + 0.5);
  m->dv->cycles_per_ms = mhz ? (u32)(cpm > 1 ? cpm : 1) : 20000;
  m->dv->t0 = plat_now_ms();
  /* as libjs: off with --no-accel, --no-osaccel (the 64-bit machine's are
   * C4IX's own, accel.c: the games' are the 32-bit machine's) */
  m->accel = m->osaccel = 1;
  return m;
}


/* the .c4r loader (src/c4bb/sim/loader.js) and the trampoline (boot.js).
 * An image is in words of the machine's size: its header says how many
 * bits (byte 4), and the lengths, code, patches, constructors and symbol
 * fields are words of that size (load-c4r.c, src/c4ix/loader.c) */
int machine_boot(machine *m, const u8 *img, u32 len, int argc, char **argv) {
  u8 *u = m->u8;
  for (u32 k = 0; k < 16 / WB; k++) m->w[k] = (word)NULL_GUARD_OP;
  m->w[TLEV_ADDR >> WSH] = OP_TLEV;
  m->w[CPU_HALT_ADDR >> WSH] = OP_CPUH;
  memcpy(u + OPNAMES_ROM, OPNAMES, OPNAMES_LEN);

  if (len < 13 || img[0] != 'C' || img[1] != '4' || img[2] != 'R') { machine_emits(m, "c4pc: not a .c4r image\n"); return -1; }
  int version = img[3];
  if (img[4] != WBITS) {
    machine_emits(m, WB == 4 ? "c4pc: a 32-bit machine; the image is not 32-bit\n"
                             : "c4pc: a 64-bit machine; the image is not 64-bit\n");
    return -1;
  }
  u32 p = 13;
#define RW() (p + WB <= len ? (p += WB, word_le(img + p - WB)) : 0)
  word entry = RW(), code_len = RW(), data_len = RW(), patch_len = RW(), syms_len = RW(), cons_len = RW(), des_len = RW();
  word memsz = version >= 3 ? word_le(img + 5) : data_len;
  if (memsz < data_len) memsz = data_len;
  if (code_len < 0 || data_len < 0 || patch_len < 0 || syms_len < 0 || cons_len < 0 || des_len < 0 ||
      (uword)code_len > len || (uword)memsz > m->size) { machine_emits(m, "c4pc: the image's header is not one\n"); return -1; }
  u32 code_base = MEM_BASE;
  u32 data_base = (code_base + (u32)code_len * WB + 7) & ~(u32)(WB - 1);
  u32 top = (data_base + (u32)memsz + 7) & ~(u32)(WB - 1);
  if ((i64)top + 65536 > m->stack_top) { machine_emits(m, "c4pc: the image does not fit the arena\n"); return -1; }
  RW();                                            /* 'C' */
  for (word i = 0; i < code_len; i++) m->w[(code_base >> WSH) + (u32)i] = RW();
  RW();                                            /* 'D' */
  if (p + (u32)data_len > len) { machine_emits(m, "c4pc: the image is cut short\n"); return -1; }
  memcpy(u + data_base, img + p, (u32)data_len); p += (u32)data_len;
  RW();                                            /* 'P' */
  for (word i = 0; i < patch_len; i++) {
    word type = RW(), addr = RW(), val = RW();
    if (type == -1) arena_wr32(m, code_base + (u32)addr * WB, (word)(code_base + (u32)val * WB));
    else if (type == -2) arena_wr32(m, code_base + (u32)addr * WB, (word)(data_base + (u32)val));
    else if (type == -3) arena_wr32(m, data_base + (u32)addr, (word)(code_base + (u32)val * WB));
    else if (type == -4) arena_wr32(m, data_base + (u32)addr, (word)(data_base + (u32)val));
  }
  RW();                                            /* 'c' */
  u32 *cons = xmalloc((size_t)(cons_len + 1) * 4), *des = xmalloc((size_t)(des_len + 1) * 4);
  for (word i = 0; i < cons_len; i++) cons[i] = code_base + (u32)RW() * WB;
  RW();                                            /* 'd' */
  for (word i = 0; i < des_len; i++) des[i] = code_base + (u32)RW() * WB;
  RW();                                            /* 'S' */
  /* the symbols, functions only (class 129), for STRC: id, type, class,
   * attrs (words), the name's length (a byte), the name, the value (a word) */
  m->dv->syms = xmalloc((size_t)(syms_len + 1) * sizeof *m->dv->syms);
  m->dv->nsyms = 0;
  for (word i = 0; i < syms_len && p + 4 * WB + 1 <= len; i++) {
    word cls = word_le(img + p + 2 * WB);
    u32 n = img[p + 4 * WB];
    if (p + 4 * WB + 1 + n + WB > len) break;
    word val = word_le(img + p + 4 * WB + 1 + n);
    if (cls == 129) {
      char *s = xmalloc(n + 1); memcpy(s, img + p + 4 * WB + 1, n); s[n] = 0;
      m->dv->syms[m->dv->nsyms].addr = code_base + (u32)val * WB; m->dv->syms[m->dv->nsyms].name = s; m->dv->nsyms++;
    }
    p += 4 * WB + 1 + n + WB;
  }
  for (int i = 1; i < m->dv->nsyms; i++) {             /* insertion sort: they come nearly in order */
    struct sym s = m->dv->syms[i]; int j = i - 1;
    while (j >= 0 && m->dv->syms[j].addr > s.addr) { m->dv->syms[j + 1] = m->dv->syms[j]; j--; }
    m->dv->syms[j + 1] = s;
  }
#undef RW
  m->dv->img_code = code_base; m->dv->img_data = data_base; m->dv->img_bss = data_base + (u32)data_len; m->dv->img_end = top;

  /* argv above the stack */
  u32 q = m->stack_top;
  u32 *ptrs = xmalloc((size_t)(argc + 1) * 4);
  for (int i = 0; i < argc; i++) {
    u32 n = (u32)strlen(argv[i]);
    if (q + n + 1 > m->size) { machine_emits(m, "c4pc: argv does not fit in the 4 KB above the stack\n"); return -1; }
    memcpy(u + q, argv[i], n); u[q + n] = 0;
    ptrs[i] = q; q += n + 1;
  }
  q = (q + WB - 1) & ~(u32)(WB - 1);
  u32 argv_base = q;
  for (int i = 0; i < argc; i++) { if (q + WB > m->size) return -1; m->w[q >> WSH] = (word)ptrs[i]; q += WB; }
  xfree(ptrs);

  /* the trampoline */
  u32 tramp = (top + WB - 1) & ~(u32)(WB - 1), w = tramp >> WSH;
  for (word k = 0; k < cons_len; k++) {
    m->w[w++] = OP_IMM; m->w[w++] = 0; m->w[w++] = OP_PSH;
    m->w[w++] = OP_JSR; m->w[w++] = (word)cons[k]; m->w[w++] = OP_ADJ; m->w[w++] = 1;
  }
  m->w[w++] = OP_IMM; m->w[w++] = argc; m->w[w++] = OP_PSH;
  m->w[w++] = OP_IMM; m->w[w++] = (word)argv_base; m->w[w++] = OP_PSH;
  m->w[w++] = OP_JSR; m->w[w++] = (word)(code_base + (u32)entry * WB); m->w[w++] = OP_ADJ; m->w[w++] = 2;
  m->w[w++] = OP_PSH;
  for (word k = des_len - 1; k >= 0; k--) { m->w[w++] = OP_JSR; m->w[w++] = (word)des[k]; }
  m->w[w++] = OP_EXIT;
  u32 tend = w << WSH;
  xfree(cons); xfree(des);

  u32 heap_base = (tend + 4096) & ~4095u;
  u32 heap_end = ((m->stack_top - 1024 * 1024) & ~4095u);
  if (heap_end <= heap_base) { machine_emits(m, "c4pc: the image does not leave room for a heap; use a bigger arena\n"); return -1; }
  heap_reset(m->heap, heap_base, heap_end);
  dev_write32(m, HEAP_BASE_REG, (i32)heap_base);
  dev_write32(m, HEAP_END_REG, (i32)heap_end);
  m->dv->stack_base = heap_end;
  m->dv->stack_top_reg = m->stack_top;
  m->dv->stack_guard = 0;
  m->sp = m->bp = (word)m->stack_top;
  m->pc = (word)tramp;
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
  word *w = c->w;
  u32 p = MEM_BASE >> WSH, loop, fn, call;
#define E(x) (w[p++] = (word)(x))
  E(OP_ENT); E(2);
  loop = p << WSH;
  E(OP_LEA); E(-1); E(OP_PSH); E(OP_LDL); E(-1); E(OP_PSH); E(OP_IMM); E(3); E(OP_ADD); E(OP_SI);
  E(OP_LEA); E(-2); E(OP_PSH); E(OP_LDL); E(-1); E(OP_PSH); E(OP_IMM); E(4099 * WB); E(OP_MUL);
  E(OP_PSH); E(OP_IMM); E(0xffffff & ~(WB - 1)); E(OP_AND); E(OP_LI); E(OP_PSH); E(OP_LDL); E(-2); E(OP_ADD); E(OP_SI);
  E(OP_JSR); call = p; E(0);
  E(OP_LDL); E(-1); E(OP_PSH); E(OP_IMM); E(0x7fffffff); E(OP_AND); E(OP_BNZ); E(loop);
  E(OP_JMP); E(loop);
  fn = p << WSH;
  E(OP_ENT); E(0); E(OP_IMM); E(1); E(OP_LEV);
  w[call] = (word)fn;
#undef E
  c->pc = MEM_BASE;
  c->sp = c->bp = (word)c->stack_top;
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
i32 eng_ld8(machine *m, word x) { return m_ld8(m, x); }
void eng_st8(machine *m, word x, word v) { m_st8(m, x, v); }
word eng_guard_lo(machine *m) { return guard_lo(m); }
