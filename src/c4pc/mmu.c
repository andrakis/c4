/* mmu.c -- paged virtual memory with protection, for C4IX: libjs/mmu.js
 * in C (docs/c4ix-mmu.md), with arena.js's helpers for the kernel's map.
 *
 * A machine gets an MMU when the platform fits one (c4pc-host --mmu, or
 * --mmu on c4pc.elf's command line). It does nothing until the guest
 * turns it on through its registers, and then only in user mode: kernel
 * mode runs the engine's own loop, untranslated, and user mode runs
 * run_user below, which translates every fetch, load and store through
 * the guest's page tables -- two levels of 1,024 words, the i386's shape.
 * Translations are cached in three arrays indexed by virtual page (read,
 * write, fetch), and two windows sit in front of them: the run of stack
 * pages around sp and the code page around pc.
 *
 * A fault is precise: the registers the trap saves are the faulting
 * instruction's, before it changed any. mmu.js throws FAULT from deep in
 * sys(), the FPU or a device; here the same places longjmp to the
 * innermost catch (u->jb), which each of mmu.js's try blocks sets.
 */
#include "c4pc.h"
#include "clib.h"

enum {
  MMU_CTL = 0x500, MMU_PTBR = 0x504, MMU_KSP = 0x508, MMU_FADDR = 0x50c, MMU_FINFO = 0x510,
  MMU_FPC = 0x514, MMU_INVLPG = 0x518, MMU_FLUSH = 0x51c, MMU_IOPM = 0x520, MMU_MISSES = 0x524,
  MMU_FAULTS = 0x528, MMU_MEMSIZE = 0x52c, MMU_UWIN = 0x530, MMU_UDIR = 0x534,
};
enum { PTE_P = 1, PTE_W = 2, PTE_U = 4, PTE_X = 8, PTE_A = 0x20, PTE_D = 0x40 };
/* what a fault was (FINFO): the access, and whether the page was there.
 * DMA: a device's, at an address the task gave it (dma_check) */
enum { FK_READ = 1, FK_WRITE = 2, FK_FETCH = 4, FK_PRESENT = 8, FK_DMA = 32 };
/* page 0's windows a user task may use (IOPM). At reset all of them */
enum { IOPM_ROM = 1, IOPM_DEV = 2, IOPM_GUI = 4, IOPM_SND = 8, IOPM_LOCAL = 16, IOPM_LOCALW = 32, IOPM_ALL = 63 };

/* virtual pages the arrays cache: the user window and below -- 512 MB
 * for an arena of 256 MB or less, whose window is at 256 MB; past that,
 * C4IX puts the window at the next 256 MB past memory (src/c4ix/mmu.c
 * mmu_wbase), and the arrays reach its end. Set by the first mmu_new,
 * and every MMU after it has arrays as long: a page past it is not
 * cached, only slower */
static u32 mmu_limit;
#define LIMIT mmu_limit
/* pages the stack window grows by each way, and the code window */
#define STACK_GROW 16
#define CODE_GROW 256
/* code windows kept */
#define CODE_WINS 4

/* the double FPU sub-ops' address operands (fpu.js FPU_A, FPU_B, FPU_R) */
#define FPU_A 0x2af
#define FPU_B 0x02f
#define FPU_R 0x14f

/* ---------------------------------------------------------------- */
/* a fault: to the innermost catch. A device's use of the task's space
 * (dma_ctx) ends with it, as mmu.js's finally blocks end it */
__attribute__((noreturn, noinline)) static void fault(mmu *u) {
  u->m->dma_ctx = 0;
  if (!u->jb) plat_fatal("c4pc: an MMU fault with nothing to catch it");
  c4_longjmp(*(c4_jmp_buf *)u->jb, 1);
}

/* a word of the arena by index, 0 past its end (a typed array's read) */
static inline word rdw(machine *m, uword idx) { return idx < (m->size >> WSH) ? m->w[idx] : 0; }

/* The tables: a page of words each, 1,024 entries of 4 bytes on the 32-bit
 * machine (the i386's shape), 512 of 8 on the 64-bit, whose directory entry
 * covers 2 MB, and the two levels 1 GB: past that nothing is mapped
 * (docs/c4pc-64.md; src/c4ix/mmu.c MMU_PTES, MMU_DSHIFT) */
#define PT_N (4096 / WB)
#define PT_DSHIFT (22 - (WB >> 3))
#if WB == 8
#define VA_OK(va) (((uword)(va) >> 30) == 0)
#else
#define VA_OK(va) 1
#endif
#define PDE_AT(dir, va) (((uword)(dir) >> WSH) + (((uword)(va) >> PT_DSHIFT) & (PT_N - 1)))
#define PTE_AT(pt, va) (((uword)(pt) >> WSH) + (((uword)(va) >> 12) & (PT_N - 1)))

/* ---------------------------------------------------------------- */
/* the kernel's map (arena.js): the word of the entry for va in the space
 * whose directory is at dir, or -1 */
i64 pte_word(machine *m, u32 dir, u32 va) {
  if (!dir || !VA_OK(va)) return -1;
  word pde = rdw(m, PDE_AT(dir, va));
  if (!(pde & 1)) return -1;
  u32 pt = (uword)pde & ~4095u;
  if ((i64)pt + 4096 > m->size) return -1;
  return PTE_AT(pt, va);
}
word kernel_pte(machine *m, u32 udir, u32 x) {
  i64 w = pte_word(m, udir, x);
  return w < 0 ? 0 : m->w[w];
}
/* the memory at user address x by the kernel's map, the page pinned
 * unless PIN is 0; -1 if it maps nothing there */
i64 kernel_phys(machine *m, u32 udir, u32 x, int pin) {
  i64 w = pte_word(m, udir, x);
  if (w < 0) return -1;
  word pte = m->w[w];
  if (!(pte & 1) || (i64)((uword)pte & ~4095u) + 4096 > m->size) return -1;
  if (pin && !(pte & PTE_PIN)) m->w[w] = pte | PTE_PIN;
  return ((uword)pte & ~4095u) | (x & 4095);
}

/* the entry for va in the space at ptbr: 0 when there is none */
static word pte_of(machine *m, u32 ptbr, u32 va) {
  if (!ptbr || !VA_OK(va)) return 0;
  word pde = rdw(m, PDE_AT(ptbr, va));
  if (!(pde & 1)) return 0;
  u32 pt = (uword)pde & ~4095u;
  if ((i64)pt + 4096 > m->size) return 0;
  return m->w[PTE_AT(pt, va)];
}

/* Where [addr, addr + len) is, for a device, in the space at ptbr: every
 * page of it user mode's to read (and write), one run of memory, and the
 * physical address of addr returned. Else -1, and dma_bad says where it
 * went wrong and dma_bad_pte what the entry there was. UDIR: the kernel's
 * map, whose pages a device's use pins (0: not pinned) */
static u32 dma_bad; static word dma_bad_pte;
static i64 dma_phys(machine *m, u32 ptbr, u32 addr, word len, int write, u32 udir) {
  if (len < 1) len = 1;
  i64 end = (i64)addr + len;
  if (addr < 0x1000 || end > 0x100000000LL) { dma_bad = addr; dma_bad_pte = 0; return -1; }
  word need = PTE_P | PTE_U | (write ? PTE_W : 0);
  i64 pa = -1; u32 next = 0;
  /* from addr's page, as a signed word (mmu.js's addr & -4096) */
  for (i64 va = (word)(addr & ~4095u); va < end; va += 4096) {
    word pte = pte_of(m, ptbr, (uword)va); u32 f = (uword)pte & ~4095u;
    if ((pte & need) != need || (i64)f + 4096 > m->size || (pa >= 0 && f != next)) {
      dma_bad = va < addr ? addr : (uword)va; dma_bad_pte = pte; return -1;
    }
    if (pa < 0) pa = f | (addr & 4095);
    next = f + 4096;
  }
  /* a page a device has used stays in memory (C4IX keeps it out of swap) */
  if (udir) for (i64 va = (word)(addr & ~4095u); va < end; va += 4096) kernel_phys(m, udir, (uword)va, 1);
  return pa;
}
static word dma_kind(int write) { return (write ? FK_WRITE : FK_READ) | FK_DMA | (dma_bad_pte & PTE_P ? FK_PRESENT : 0); }

/* ---------------------------------------------------------------- */
static void flush(mmu *u);
static void reset(mmu *u) {
  u->on = 0;
  u->m->uwin = 0; u->m->udir = 0;
  u->general = 0;
  u->ptbr = 0; u->ksp = 0; u->iopm = IOPM_ALL;
  u->faddr = 0; u->finfo = 0; u->fpc = 0;
  u->ncwins = 0;
  flush(u);
}

/* a fault a device found for this task on CPU 0, raised here (par.c) */
void mmu_rethrow(mmu *u) { fault(u); }

/* off, as at power-on and when a CPU is started (smp.c) */
void mmu_reset(mmu *u) { reset(u); }

mmu *mmu_new(machine *m) {
  mmu *u = xmalloc(sizeof *u);
  u->m = m;
  if (!mmu_limit) {
    u32 win = 0x10000000u;
    if (m->size > win) win = (u32)(((uint64_t)m->size + 0x0FFFFFFFu) & ~(uint64_t)0x0FFFFFFFu);
    if (win > 0x70000000u) win = 0x70000000u;
    mmu_limit = (win + 0x10000000u) >> 12;
  }
  u->rd = xmalloc(LIMIT * 4); u->wr = xmalloc(LIMIT * 4); u->ex = xmalloc(LIMIT * 4);
  for (u32 v = 0; v < LIMIT; v++) u->rd[v] = u->wr[v] = u->ex[v] = -1;
  u->nx = xmalloc(LIMIT);
  u->log = xmalloc(LIMIT * 4);
  m->mmu = u;
  reset(u);
  return u;
}

static void flush(mmu *u) {
  for (u32 k = 0; k < u->nlog; k++) { i32 v = u->log[k]; u->rd[v] = -1; u->wr[v] = -1; u->ex[v] = -1; u->nx[v] = 0; }
  u->nlog = 0;
  u->ncwins = 0;
  u->s_lo = u->s_len = u->s_off = 0;
  u->c_lo = u->c_len = u->c_off = 0;
}

static void invlpg(mmu *u, word va) {
  uword v = (uword)va >> 12;
  if (v < LIMIT) { u->rd[v] = -1; u->wr[v] = -1; u->ex[v] = -1; u->nx[v] = 0; }
  u->ncwins = 0;
  u->s_lo = u->s_len = u->s_off = 0;
  u->c_lo = u->c_len = u->c_off = 0;
}

/* ---------------------------------------------------------------- */
/* the registers, at 0x500: kernel mode's (user mode's accesses below
 * 0x1000 come through low_ok and never reach them) */
word mmu_read32(mmu *u, word x) {
  machine *m = u->m;
  switch (x & ~3) {
    case MMU_CTL: return u->on ? 3 : 1;
    case MMU_PTBR: return u->ptbr;
    case MMU_KSP: return u->ksp;
    case MMU_FADDR: return u->faddr;
    case MMU_FINFO: return u->finfo;
    case MMU_FPC: return u->fpc;
    case MMU_IOPM: return u->iopm;
    case MMU_MISSES: { word n = u->misses; u->misses = 0; return n; }
    case MMU_FAULTS: return u->faults;
    case MMU_MEMSIZE: return (word)m->size;
    case MMU_UWIN: return (word)m->uwin;
    case MMU_UDIR: return (word)m->udir;
  }
  return 0;
}
void mmu_write32(mmu *u, word x, word v) {
  machine *m = u->m;
  switch (x & ~3) {
    case MMU_CTL: u->on = (v & 1) != 0; flush(u); return;
    case MMU_PTBR: u->ptbr = v & ~4095; flush(u); return;
    case MMU_KSP: u->ksp = v & ~3; return;
    case MMU_INVLPG: invlpg(u, v); return;
    case MMU_FLUSH: flush(u); return;
    case MMU_IOPM: u->iopm = v; return;
    /* the user window: where user addresses start (engine.c kpa) */
    case MMU_UWIN: m->uwin = (uword)v & ~4095u; return;
    case MMU_UDIR: m->udir = (uword)v & ~4095u; return;
  }
}

/* ---------------------------------------------------------------- */
/* translation: the entry for va, without side effects (0: none) */
static word probe(mmu *u, u32 va) {
  machine *m = u->m;
  u32 pd = (uword)u->ptbr;
  u->pte_at = -1;
  if (!pd || !VA_OK(va)) return 0;
  word pde = rdw(m, PDE_AT(pd, va));
  if (!(pde & 1)) return 0;
  u32 pt = (uword)pde & ~4095u;
  if ((i64)pt + 4096 > m->size) return 0;
  u->pte_at = (word)PTE_AT(pt, va);
  return m->w[u->pte_at];
}

/* v's entries are about to be set: logged, so that a flush clears them */
static void logv(mmu *u, u32 v) {
  if (u->nlog == LIMIT) {
    for (u32 k = 0; k < LIMIT; k++) { u->rd[k] = u->wr[k] = u->ex[k] = -1; u->nx[k] = 0; }
    u->nlog = 0;
  }
  u->log[u->nlog++] = (word)v;
}

/* the physical page for an access of this kind at va, filling the arrays;
 * or -1, with the fault recorded (FADDR, FINFO, FPC) for raise */
static word xlate(mmu *u, word va, int kind) {
  machine *m = u->m;
  word pte = probe(u, (uword)va);
  u32 frame = (uword)pte & ~4095u;
  if ((pte & 5) != 5 || (i64)frame + 4096 > m->size ||
      (kind == FK_WRITE && !(pte & PTE_W)) || (kind == FK_FETCH && !(pte & PTE_X))) {
    u->faddr = va;
    u->finfo = kind | ((pte & PTE_P) ? FK_PRESENT : 0);
    u->fpc = m->pc;
    u->faults++;
    return -1;
  }
  word npte = pte | PTE_A;
  if (kind == FK_WRITE) npte |= PTE_D;
  if (npte != pte) m->w[u->pte_at] = npte;
  u->misses++;
  uword v = (uword)va >> 12;
  if (v < LIMIT) {
    if (u->rd[v] < 0 && u->wr[v] < 0 && u->ex[v] < 0 && !u->nx[v]) logv(u, v);
    u->rd[v] = (word)frame;
    /* written to only once it is dirty, so the first store sets D */
    if ((npte & (PTE_W | PTE_D)) == (PTE_W | PTE_D)) u->wr[v] = (word)frame;
    if (npte & PTE_X) u->ex[v] = (word)frame;
  }
  return (word)frame;
}

/* the physical page at va if user mode may run it, filled; -1 if not,
 * which is remembered until the next flush. Raises nothing */
static word x_page(mmu *u, word va) {
  if ((uword)va < 0x1000) return -1;
  uword v = (uword)va >> 12;
  if (v < LIMIT) {
    if (u->ex[v] >= 0) return u->ex[v];
    if (u->nx[v]) return -1;
  }
  word pte = probe(u, (uword)va);
  if ((pte & (PTE_P | PTE_U | PTE_X)) != (PTE_P | PTE_U | PTE_X)) {
    if (v < LIMIT) {
      if (u->rd[v] < 0 && u->wr[v] < 0) logv(u, v);
      u->nx[v] = 1;
    }
    return -1;
  }
  return xlate(u, va, FK_FETCH);
}

/* the cached page, or a fill */
static word page(mmu *u, word va, int kind) {
  uword v = (uword)va >> 12;
  if (v < LIMIT) {
    word p = kind == FK_READ ? u->rd[v] : kind == FK_WRITE ? u->wr[v] : u->ex[v];
    if (p >= 0) return p;
  }
  return xlate(u, va, kind);
}

static char *hex8(char *buf, uword v) {
  static const char h[] = "0123456789ABCDEF";
  buf[0] = '0'; buf[1] = 'x';
  for (int i = 0; i < 2 * WB; i++) buf[2 + i] = h[(v >> (4 * (2 * WB - 1) - 4 * i)) & 15];
  buf[2 + 2 * WB] = 0;
  return buf;
}

/* raise the recorded fault as TRAP_SEGV(FADDR): the machine's registers
 * are the faulting instruction's. With no handler the machine stops */
static void raise_(mmu *u) {
  machine *m = u->m;
  if (!m->trap_handler) {
    char a[24], b[24];
    const char *what = u->finfo & FK_WRITE ? "write to" : u->finfo & FK_FETCH ? "fetch from" : "read of";
    machine_emits(m, "libjs: page fault: "); machine_emits(m, what); machine_emits(m, " ");
    machine_emits(m, hex8(a, (uword)u->faddr)); machine_emits(m, " at pc "); machine_emits(m, hex8(b, (uword)u->fpc));
    machine_emits(m, ", and no trap handler. Halting.\n");
    m->vm = 0;
    eng_halt(m, -104);
    return;
  }
  eng_trap(m, TRAP_SEGV, u->faddr, m->trap_handler);
}

/* a fault found outside xlate: page 0 the IOPM refuses, or a fetch there */
static void refuse(mmu *u, word va, int kind) {
  u->faddr = va;
  u->finfo = kind;
  u->fpc = u->m->pc;
  u->faults++;
}

/* A device, reached by this task's load or store, about to use [addr,
 * addr + len): the task's own memory, or the fault it would have taken */
i64 mmu_dma_check(mmu *u, u32 addr, i32 len, int write, int pin) {
  machine *m = u->m;
  i64 pa = dma_phys(m, (uword)u->ptbr, addr, len, write, pin ? m->udir : 0);
  if (pa >= 0) return pa;
  refuse(u, (word)dma_bad, dma_kind(write));
  fault(u);
}

/* the bus, for user mode: a device it reaches checks the addresses it is
 * given against this task's space while the access lasts */
static word low_rd32(mmu *u, word x) {
  machine *m = u->m;
  if (m->smp && x >= LOCAL_BASE && x < LOCAL_END && (x & (WB - 1)) == 0) return m->local[(x - LOCAL_BASE) >> WSH];
  return eng_bus_rd32(m, (uword)x);
}
static word bus_rd32(mmu *u, word x) {
  u->m->dma_ctx = u;
  word v = low_rd32(u, x);
  u->m->dma_ctx = 0;
  return v;
}
static void bus_wr32(mmu *u, word x, word v) {
  u->m->dma_ctx = u;
  eng_bus_wr32(u->m, (uword)x, v);
  u->m->dma_ctx = 0;
}
static void bus_wr8(mmu *u, word x, word v) {
  u->m->dma_ctx = u;
  eng_bus_wr8(u->m, (uword)x, v & 0xff);
  u->m->dma_ctx = 0;
}

/* page 0 in user mode, where the IOPM allows */
static int low_ok(mmu *u, word x0, int kind) {
  u32 x = (uword)x0; word io = u->iopm;
  if (x < 0x100) return (io & IOPM_ROM) != 0 && kind == FK_READ;
  if (x < 0x200) return (io & IOPM_DEV) != 0;
  if (x < 0x400) return (io & IOPM_ROM) != 0 && kind == FK_READ;
  if (x < 0x480) return (io & IOPM_GUI) != 0;
  if (x < 0x500) return (io & IOPM_SND) != 0;
  if (x >= LOCAL_BASE && x < LOCAL_END) return (io & (kind == FK_READ ? IOPM_LOCAL : IOPM_LOCALW)) != 0;
  return 0;
}

/* ---------------------------------------------------------------- */
/* loads and stores for everything but the loop's inline paths: sys(), the
 * FPU, OPCD. A fault goes to the catch */
word mmu_ld8(mmu *u, word x) {
  if ((uword)x < 0x1000) {
    if (!low_ok(u, x, FK_READ)) { refuse(u, x, FK_READ | FK_PRESENT); fault(u); }
    if (u->m->smp && x >= LOCAL_BASE && x < LOCAL_END) return (int8_t)((u8 *)u->m->local)[x - LOCAL_BASE];
    return eng_bus_rd8s(u->m, (uword)x);
  }
  word p = page(u, x, FK_READ);
  if (p < 0) fault(u);
  return (int8_t)u->m->u8[(uword)p | ((uword)x & 4095)];
}
word mmu_ld32(mmu *u, word x) {
  if ((uword)x < 0x1000) {
    if (!low_ok(u, x, FK_READ)) { refuse(u, x, FK_READ | FK_PRESENT); fault(u); }
    return bus_rd32(u, x);
  }
  if (x & (WB - 1)) {
    uword w = 0;
    for (int k = 0; k < WB; k++) w |= (uword)(mmu_ld8(u, (word)((uword)x + (uword)k)) & 255) << (8 * k);
    return (word)w;
  }
  word p = page(u, x, FK_READ);
  if (p < 0) fault(u);
  return u->m->w[((uword)p | ((uword)x & 4095)) >> WSH];
}
void mmu_st8(mmu *u, word x, word v) {
  machine *m = u->m;
  if ((uword)x < 0x1000) {
    if (!low_ok(u, x, FK_WRITE)) { refuse(u, x, FK_WRITE | FK_PRESENT); fault(u); }
    if (m->smp && x >= LOCAL_BASE && x < LOCAL_END) { ((u8 *)m->local)[x - LOCAL_BASE] = (u8)v; return; }
    if (is_rom(x, 1)) { eng_rom_store(m, x, v & 0xff); return; }
    bus_wr8(u, x, v);
    return;
  }
  word p = page(u, x, FK_WRITE);
  if (p < 0) fault(u);
  m->u8[(uword)p | ((uword)x & 4095)] = (u8)v;
}
void mmu_st32(mmu *u, word x, word v) {
  machine *m = u->m;
  if ((uword)x < 0x1000) {
    if (!low_ok(u, x, FK_WRITE)) { refuse(u, x, FK_WRITE | FK_PRESENT); fault(u); }
    if (m->smp && x >= LOCAL_BASE && x < LOCAL_END && (x & (WB - 1)) == 0) { m->local[(x - LOCAL_BASE) >> WSH] = v; return; }
    if (is_rom(x, WB)) { eng_rom_store(m, x, v); return; }
    bus_wr32(u, x, v);
    return;
  }
  if (x & (WB - 1)) { for (int k = 0; k < WB; k++) mmu_st8(u, (word)((uword)x + (uword)k), v >> (8 * k)); return; }
  word p = page(u, x, FK_WRITE);
  if (p < 0) fault(u);
  m->w[((uword)p | ((uword)x & 4095)) >> WSH] = v;
}

/* ---------------------------------------------------------------- */
/* The block opcodes in user mode, a page at a time. A page the task may
 * not use faults part way, as a store loop would; what was done before it
 * stays done, and the instruction, run again once the kernel has brought
 * the page in, goes on from there (part) rather than from its first byte */
__attribute__((noinline)) static void block_part(mmu *u, word op, word sp, word d, word s, word n, word k) {
  u->part.has = 1; u->part.op = op; u->part.pc = u->m->pc; u->part.sp = sp; u->part.ptbr = u->ptbr;
  u->part.d = d; u->part.s = s; u->part.n = n; u->part.k = k;
}
__attribute__((noinline)) static void block_fault(mmu *u, void *outer, word op, word sp, word d, word s, word n, word k) {
  u->jb = outer;
  block_part(u, op, sp, d, s, n, k);
  fault(u);
}
int mmu_block(mmu *u, word op) {
  machine *m = u->m;
  u8 *const u8m = m->u8;
  word sp = m->sp;
  word d = mmu_ld32(u, sp + 2 * WB), s = mmu_ld32(u, sp + WB), n = mmu_ld32(u, sp);
  /* the same instruction again: same space, pc, stack and operands */
  volatile word k = 0;
  if (u->part.has && u->part.op == op && u->part.pc == m->pc && u->part.sp == sp && u->part.ptbr == u->ptbr &&
      u->part.d == d && u->part.s == s && u->part.n == n) k = u->part.k;
  u->part.has = 0;
  void *outer = u->jb;
  c4_jmp_buf jb;
  if (c4_setjmp(jb)) block_fault(u, outer, op, sp, d, s, n, k);   /* a fault in a load or store below */
  u->jb = &jb;
  if (op == OP_MSET) {
    word v = s & 0xff;
    while (k < n) {
      word x = (word)((uword)d + (uword)k);
      if ((uword)x < 0x1000) { mmu_st8(u, x, v); k = k + 1; continue; }
      word run = n - k < 4096 - (x & 4095) ? n - k : 4096 - (x & 4095);
      word p = page(u, x, FK_WRITE);
      if (p < 0) block_fault(u, outer, op, sp, d, s, n, k);
      memset(u8m + ((uword)p | ((uword)x & 4095)), v, (uword)run);
      k = k + run;
    }
    u->jb = outer;
    m->a = d;
    return STOP_BUDGET;
  }
  if (op == OP_MCMP) {
    word r = 0;
    for (; k < n; k = k + 1) {
      word c = (mmu_ld8(u, (word)((uword)d + (uword)k)) & 255) - (mmu_ld8(u, (word)((uword)s + (uword)k)) & 255);
      if (c) { r = c; break; }
    }
    u->jb = outer;
    m->a = r;
    return STOP_BUDGET;
  }
  /* MCPY: forward, byte by byte when the ranges overlap that way, else a
   * run at a time within both pages */
  if (n > 0) {
    if (d > s && (i64)d < (i64)s + n) {
      for (; k < n; k = k + 1) mmu_st8(u, (word)((uword)d + (uword)k), mmu_ld8(u, (word)((uword)s + (uword)k)));
    } else {
      while (k < n) {
        word x = (word)((uword)d + (uword)k), y = (word)((uword)s + (uword)k);
        if ((uword)x < 0x1000 || (uword)y < 0x1000) { mmu_st8(u, x, mmu_ld8(u, y)); k = k + 1; continue; }
        word run = n - k;
        if (4096 - (x & 4095) < run) run = 4096 - (x & 4095);
        if (4096 - (y & 4095) < run) run = 4096 - (y & 4095);
        word ps = page(u, y, FK_READ);
        if (ps < 0) block_fault(u, outer, op, sp, d, s, n, k);
        word pd = page(u, x, FK_WRITE);
        if (pd < 0) block_fault(u, outer, op, sp, d, s, n, k);
        memmove(u8m + ((uword)pd | ((uword)x & 4095)), u8m + ((uword)ps | ((uword)y & 4095)), (uword)run);
        k = k + run;
      }
    }
  }
  u->jb = outer;
  m->a = d;
  return STOP_BUDGET;
}

/* OPCD of one of the loop's own opcodes without an operand, in user mode:
 * engine.c's opcd over the translated loads and stores */
static void opcd(mmu *u, word op) {
  machine *mm = u->m;
  word sp = mm->sp, a = mm->a, x = 0;
  switch (op) {
    case OP_LEV: {
      word bp = mm->bp;
      word nbp = mmu_ld32(u, bp), npc = mmu_ld32(u, bp + WB);
      mm->bp = nbp; mm->pc = npc; sp = bp + 2 * WB;
      break;
    }
    case OP_LI: a = mmu_ld32(u, a); break;
    case OP_LC: a = mmu_ld8(u, a); break;
    case OP_SI: x = mmu_ld32(u, sp); mmu_st32(u, x, a); sp += WB; break;
    case OP_SC: x = mmu_ld32(u, sp); mmu_st8(u, x, a); sp += WB; a = (int8_t)a; break;
    case OP_LH: { word lo = mmu_ld8(u, a) & 255; a = lo | (word)((uword)mmu_ld8(u, (word)((uword)a + 1)) << 8); break; }
    case OP_LHU: { word lo = mmu_ld8(u, a) & 255; a = lo | ((mmu_ld8(u, (word)((uword)a + 1)) & 255) << 8); break; }
    case OP_SH: x = mmu_ld32(u, sp); mmu_st8(u, x, a); mmu_st8(u, (word)((uword)x + 1), a >> 8); sp += WB; break;
    case OP_LSR: x = mmu_ld32(u, sp); a = (word)((uword)x >> (a & (WBITS - 1))); sp += WB; break;
    case OP_LCU: a = mmu_ld8(u, a) & 255; break;
    case OP_PSH: mmu_st32(u, sp - WB, a); sp -= WB; break;
    case OP_LIP: a = mmu_ld32(u, a); mmu_st32(u, sp - WB, a); sp -= WB; break;
    case OP_ADDL: x = (word)((uword)mmu_ld32(u, sp) + (uword)a); a = mmu_ld32(u, x); sp += WB; break;
    case OP_POPA: a = mmu_ld32(u, sp); sp += WB; break;
    case OP_JMPA: mm->pc = a; break;
    case OP__JMP: mm->pc = mmu_ld32(u, sp); sp += WB; break;
    case OP__ADJ: sp = (word)((uword)sp + ((uword)mmu_ld32(u, sp) << WSH)); break;
    case OP_C4CY: a = (word)mm->cycle; break;
    default: {
      word t = mmu_ld32(u, sp); sp += WB;
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
  mm->sp = sp; mm->a = a;
}

/* the opcode at pc in user mode, for boundary's "TLEV is never
 * interrupted": -1 if it cannot be read */
word mmu_peek(mmu *u, word pc) {
  if ((uword)pc < 0x1000) return (uword)pc == TLEV_ADDR ? OP_TLEV : -1;
  uword v = (uword)pc >> 12;
  word p = v < LIMIT ? u->ex[v] : -1;
  return p >= 0 ? u->m->w[((uword)p | ((uword)pc & 4095)) >> WSH] : -1;
}

/* ---------------------------------------------------------------- */
/* The loop's slow paths. Each is called with the machine's registers saved
 * as the instruction found them, and on a fault raises it: the caller
 * looks at m->mode, which the trap has made 0 */

/* the physical page at va if user mode may read and write it, filled (and
 * made dirty); -1, raising nothing, if not */
static word rw_page(mmu *u, word va) {
  if ((uword)va < 0x1000) return -1;
  uword v = (uword)va >> 12;
  if (v < LIMIT && u->wr[v] >= 0) return u->wr[v];
  word pte = probe(u, (uword)va);
  if ((pte & (PTE_P | PTE_U | PTE_W)) != (PTE_P | PTE_U | PTE_W)) return -1;
  return xlate(u, va, FK_WRITE);
}

/* The stack window: [s_lo, s_lo + s_len) virtual, at s_off from physical,
 * pages user mode may read and write, around sp: kept if it still has a
 * word either side of sp, else made there, joined to the old one when they
 * are one run, and grown by up to STACK_GROW pages each way while the next
 * page follows on in physical memory. 0 if a word either side of sp is
 * not the task's to write */
static int s_fix(mmu *u, word sp) {
  word lo = (sp - WB) & ~4095, hi = sp & ~4095;
  word p = rw_page(u, lo);
  if (p < 0) return 0;
  word off = p - lo;
  word wlo = lo, whi = lo + 4096;
  if (hi != lo) {
    word q = rw_page(u, hi);
    if (q < 0 || q - hi != off) return 0;
    whi = hi + 4096;
  }
  if (u->s_len && off == u->s_off && (i64)wlo <= (i64)u->s_lo + u->s_len && whi >= u->s_lo) {
    if (u->s_lo < wlo) wlo = u->s_lo;
    if (u->s_lo + u->s_len > whi) whi = u->s_lo + u->s_len;
  }
  for (int k = 0; k < STACK_GROW; k++) {
    word q = rw_page(u, wlo - 4096);
    if (q < 0 || q - (wlo - 4096) != off) break;
    wlo -= 4096;
  }
  for (int k = 0; k < STACK_GROW; k++) {
    word q = rw_page(u, whi);
    if (q < 0 || q - whi != off) break;
    whi += 4096;
  }
  u->s_lo = wlo; u->s_len = whi - wlo; u->s_off = off;
  return 1;
}

/* sp is outside the task's writable memory: the fault the next push or
 * pop would take, raised before the instruction runs */
static void stack_fault(mmu *u, word sp) {
  if (rw_page(u, sp - WB) < 0) refuse(u, sp - WB, FK_WRITE);
  else refuse(u, sp, FK_READ);
  raise_(u);
}

/* the catches: a fault in a load or store raises it (mmu.js sRd, pRd32 ...) */
#define CATCH_BEGIN(u) \
  void *outer_ = (u)->jb; c4_jmp_buf jb_; \
  if (c4_setjmp(jb_)) { (u)->jb = outer_; raise_(u); return 0; } \
  (u)->jb = &jb_;
#define CATCH_END(u) ((u)->jb = outer_)
__attribute__((noinline)) static word p_rd32(mmu *u, word x) { CATCH_BEGIN(u) word v = mmu_ld32(u, x); CATCH_END(u); return v; }
__attribute__((noinline)) static word p_rd8(mmu *u, word x) { CATCH_BEGIN(u) word v = mmu_ld8(u, x); CATCH_END(u); return v; }
__attribute__((noinline)) static int p_wr32(mmu *u, word x, word v) { CATCH_BEGIN(u) mmu_st32(u, x, v); CATCH_END(u); return 0; }
__attribute__((noinline)) static int p_wr8(mmu *u, word x, word v) { CATCH_BEGIN(u) mmu_st8(u, x, v); CATCH_END(u); return 0; }
/* a short, two bytes, low first: a fault at either raises it from the
 * instruction's start, which runs again from nothing (LH, SH) */
__attribute__((noinline)) static word p_rd16(mmu *u, word x) {
  CATCH_BEGIN(u)
  word lo = mmu_ld8(u, x) & 255;
  word v = lo | (word)((uword)mmu_ld8(u, (word)((uword)x + 1)) << 8);
  CATCH_END(u);
  return v;
}
__attribute__((noinline)) static int p_wr16(mmu *u, word x, word v) {
  CATCH_BEGIN(u)
  mmu_st8(u, x, v); mmu_st8(u, (word)((uword)x + 1), v >> 8);
  CATCH_END(u);
  return 0;
}
/* a frame word (bp-relative) or a stack word outside the window */
#define s_rd p_rd32
#define s_wr p_wr32

/* The code window: the run of pages around pc that may be run, mapped next
 * to each other in physical memory too, up to CODE_GROW pages each way.
 * c_len leaves out its last word, so that an instruction that starts
 * inside it has its operand inside too. The last CODE_WINS windows are
 * kept. Raises a fault (the caller looks at m->mode) if pc may not be run */
static void c_fill(mmu *u, word pc) {
  machine *m = u->m; i32 *w = u->cwins;
  if ((uword)pc < 0x1000) {
    /* the ROM's TLEV word, where trap frames return, and the CPUH word;
     * nothing else runs. The window is that word alone: c_len 0 */
    if ((uword)pc == TLEV_ADDR || (uword)pc == CPU_HALT_ADDR) {
      u->c_lo = pc; u->c_off = 0; u->c_len = 0;
      return;
    }
    refuse(u, pc, FK_FETCH); raise_(u); return;
  }
  for (int k = 0; k < u->ncwins; k++) {
    if ((uword)(pc - w[3 * k]) < (uword)w[3 * k + 1]) {
      u->c_lo = w[3 * k]; u->c_len = w[3 * k + 1]; u->c_off = w[3 * k + 2];
      return;
    }
  }
  word p = page(u, pc, FK_FETCH);
  if (p < 0) { raise_(u); return; }
  word vp = pc & ~4095, off = p - vp;
  word lo = vp, hi = vp + 4096;
  for (int k = 0; k < CODE_GROW; k++) {
    word q = x_page(u, lo - 4096);
    if (q < 0 || q - (lo - 4096) != off) break;
    lo -= 4096;
  }
  for (int k = 0; k < CODE_GROW; k++) {
    word q = x_page(u, hi);
    if (q < 0 || q - hi != off) break;
    hi += 4096;
  }
  u->c_lo = lo; u->c_off = off; u->c_len = hi - lo - WB;
  /* kept, the newest first */
  int n = u->ncwins < CODE_WINS ? u->ncwins++ : CODE_WINS - 1;
  memmove(w + 3, w, (size_t)(3 * n) * 4);
  w[0] = lo; w[1] = hi - lo - WB; w[2] = off;
  /* the last word of the window, whose operand would be past its end */
  if ((uword)(pc - u->c_lo) >= (uword)u->c_len && has_operand(m->w[((uword)p | ((uword)pc & 4095)) >> WSH])) {
    word q = page(u, pc + WB, FK_FETCH);
    if (q < 0) { raise_(u); return; }
    /* an operand across a page boundary into memory that is not next to
     * the opcode's: a fault */
    refuse(u, pc + WB, FK_FETCH | 16); raise_(u);
  }
}

/* ---------------------------------------------------------------- */
static int run_user(mmu *u, i64 max_cycles);

/* up to max_cycles, in whichever loop the mode calls for: kernel mode is
 * the engine's loop; a TLEV into user mode leaves it with STOP_SWITCH,
 * and a trap leaves this one the same way */
int mmu_run(mmu *u, i64 max_cycles) {
  machine *m = u->m;
  i64 end = m->cycle + max_cycles;
  for (;;) {
    i64 left = end - m->cycle;
    if (left <= 0) return STOP_BUDGET;
    int s = !(m->mode && u->on) ? eng_run_plain(m, left) : run_user(u, left);
    if (s != STOP_SWITCH) return s;
  }
}

/* calls into sys() from user mode: a fault anywhere in it comes back here,
 * raised from the instruction's own registers */
__attribute__((noinline)) static int sys_user(mmu *u, word op, word ip, word sp, word bp, word a) {
  machine *m = u->m;
  void *outer = u->jb; c4_jmp_buf jb;
  if (c4_setjmp(jb)) {
    u->jb = outer;
    m->pc = ip; m->sp = sp; m->bp = bp; m->a = a;
    u->fpc = ip;
    raise_(u);
    return STOP_BUDGET;
  }
  u->jb = &jb;
  int s = eng_sys(m, op);
  u->jb = outer;
  return s;
}
__attribute__((noinline)) static void opcd_user(mmu *u, word op, word ip, word sp, word bp, word a) {
  machine *m = u->m;
  void *outer = u->jb; c4_jmp_buf jb;
  if (c4_setjmp(jb)) {
    u->jb = outer;
    m->pc = ip; m->sp = sp; m->bp = bp; m->a = a;
    u->fpc = ip;
    raise_(u);
    return;
  }
  u->jb = &jb;
  opcd(u, op);
  u->jb = outer;
}

/* a double's FPU sub-op from the loop, or 0 for the long way: a page not
 * in the arrays, or a double across two. Nothing is done until all of it
 * is known, so the long way can start again from nothing */
static int fpu_user(mmu *u, word op, word pa, word pb, word pr) {
  if (op == 21) return 0;
  i32 *rd = u->rd, *wr = u->wr; int bit = 1 << (op - 17);
  word a = pa, ib = 0, ir = 0, p; uword v;
  if (FPU_A & bit) {
    if ((pa & (WB - 1)) != 0 || (pa & 4095) > 4088) return 0;
    v = (uword)pa >> 12; p = v < LIMIT ? rd[v] : -1;
    if (p < 0) return 0;
    a = (word)((uword)p | ((uword)pa & 4095));
  }
  if (FPU_B & bit) {
    if ((pb & (WB - 1)) != 0 || (pb & 4095) > 4088) return 0;
    v = (uword)pb >> 12; p = v < LIMIT ? rd[v] : -1;
    if (p < 0) return 0;
    ib = (word)((uword)p | ((uword)pb & 4095));
  }
  if (FPU_R & bit) {
    if ((pr & (WB - 1)) != 0 || (pr & 4095) > 4088) return 0;
    v = (uword)pr >> 12; p = v < LIMIT ? wr[v] : -1;
    if (p < 0) return 0;
    ir = (word)((uword)p | ((uword)pr & 4095));
  }
  word res;
  fpu_double_plain(u->m->u8, op, a, (uword)ib, (uword)ir, &res);
  u->fpu_v = res;
  return 1;
}

static i64 next_multiple(i64 cycle, word interval) { return (cycle / interval + 1) * interval; }

/* The user-mode loop: the engine's, with every access translated, its
 * cases one for one. What differs:
 *  - the fetch reads through the code window, and pushes and pops through
 *    the stack window, without looking: the top of the loop counts how far
 *    both are good for (c_fill, s_fix), and a jump, or an instruction that
 *    moves sp more than a word, ends the count (lim);
 *  - frame words are looked up in the stack window, and pointers in the
 *    arrays (rd, wr), with a slow path past either;
 *  - a slow path saves the registers as the instruction found them (pc its
 *    first word, sp before any pop) and leaves the loop when a fault has
 *    taken the machine into kernel mode.
 * mmu.js runs a copy of it without the windows' offsets while they are all
 * 0 (an identity map), which hands over to the general loop for good
 * (general) the first time a window is not: STOP_SWITCH, one cycle on, as
 * here when !general. */
static int run_user(mmu *u, i64 max_cycles) {
  machine *m = u->m;
  word *const mw = m->w;
  u8 *const mu = m->u8;
  const u32 wm = m->wmask;
#define MW(x) mw[((uword)(x) >> WSH) & wm]
  const i32 *const rd = u->rd, *const wr = u->wr;
  if (m->dv->halted) return STOP_HALT;
  const int ident = !u->general;
  word pc = m->pc, sp = m->sp, bp = m->bp, a = m->a;
  i64 cycle = m->cycle;
  const i64 end = cycle + max_cycles;
  word interval = m->cycle_interval;
  i64 next_irq = interval > 0 ? next_multiple(cycle, interval) : 0;
  int chk = eng_need_checks(m);
  i64 next_chk = 0, lim = -1, yy;
  int stop = STOP_BUDGET;
  word op = 0, x = 0, y = 0, p = 0; uword v = 0;
  word s_lo = u->s_lo, s_len = u->s_len, s_off = u->s_off;
  word c_lo = u->c_lo, c_len = u->c_len, c_off = u->c_off;
  m->vm = 1;
#define SAVEP(P) (m->pc = (P), m->sp = sp, m->bp = bp, m->a = a, m->cycle = cycle)
#define LOAD() (pc = m->pc, sp = m->sp, bp = m->bp, a = m->a)
#define SWITCH_OUT() do { stop = STOP_SWITCH; goto out; } while (0)
#define FAULTED() do { if (!m->mode) SWITCH_OUT(); } while (0)
#define FAULTED_H() do { if (!m->mode || m->dv->halted) { stop = m->dv->halted ? STOP_HALT : STOP_SWITCH; goto out; } } while (0)
#define URECHECK() do { interval = m->cycle_interval; \
    next_irq = interval > 0 ? next_multiple(cycle, interval) : 0; \
    chk = eng_need_checks(m); next_chk = 0; lim = -1; } while (0)
  /* how far the code window is good for after a jump (no -1: the jump's own) */
#define CODE_LIM() do { if ((uword)(pc - c_lo) < (uword)c_len) { \
      yy = cycle + ((word)((uword)c_lo + (uword)c_len + WB - (uword)pc) >> (WSH + 1)); if (yy < lim) lim = yy; } else lim = -1; } while (0)
#define STACK_LIM() do { x = sp - s_lo; if (x < WB || x > s_len - WB) lim = -1; \
    else { yy = cycle + ((x < s_len - x ? x : s_len - x) >> WSH); if (yy < lim) lim = yy; } } while (0)
#define CW(x) MW((uword)(x) + (uword)c_off)
#define SW(x) MW((uword)(x) + (uword)s_off)

  for (;;) {
    if (++cycle > lim) {
      if (cycle > end) { --cycle; break; }
      if (chk && cycle >= next_chk) {
        SAVEP(pc);
        next_chk = eng_boundary(m, cycle, next_irq);
        next_irq = m->b_next_irq;
        if (m->b_fired) {
          LOAD();
          interval = m->cycle_interval;
          chk = eng_need_checks(m);
          FAULTED_H();
        }
      }
      /* the code window: pc has left it, or may have */
      if ((uword)(pc - c_lo) >= (uword)c_len) {
        SAVEP(pc);
        c_fill(u, pc);
        FAULTED_H();
        c_lo = u->c_lo; c_len = u->c_len; c_off = u->c_off;
        if (ident && c_off != 0) { u->general = 1; SWITCH_OUT(); }
      }
      /* the stack window: a word either side of sp */
      x = sp - s_lo;
      if (x < WB || x > s_len - WB) {
        SAVEP(pc);
        if (!s_fix(u, sp)) {
          stack_fault(u, sp);
          stop = m->dv->halted ? STOP_HALT : STOP_SWITCH;
          goto out;
        }
        s_lo = u->s_lo; s_len = u->s_len; s_off = u->s_off;
        if (ident && s_off != 0) { u->general = 1; SWITCH_OUT(); }
        x = sp - s_lo;
      }
      /* how many instructions run before the next look */
      lim = chk && next_chk - 1 < end ? next_chk - 1 : end;
      yy = cycle + ((word)((uword)c_lo + (uword)c_len + WB - (uword)pc) >> (WSH + 1)) - 1;
      if (yy < lim) lim = yy;
      yy = cycle + ((x < s_len - x ? x : s_len - x) >> WSH) - 1;
      if (yy < lim) lim = yy;
      if (lim < cycle) lim = cycle;
    }

    op = CW(pc);
    pc += WB;

    switch (op) {
      case OP_LEA: a = (word)((uword)bp + ((uword)CW(pc) << WSH)); pc += WB; break;
      case OP_IMM: a = CW(pc); pc += WB; break;
      case OP_JMP: pc = CW(pc); CODE_LIM(); break;
      case OP_JSR: sp -= WB; SW(sp) = pc + WB; pc = CW(pc); CODE_LIM(); break;
      case OP_BZ: if (a) pc += WB; else { pc = CW(pc); CODE_LIM(); } break;
      case OP_BNZ: if (a) { pc = CW(pc); CODE_LIM(); } else pc += WB; break;
      case OP_ENT:
        sp -= WB; SW(sp) = bp; bp = sp;
        sp = (word)((uword)sp - ((uword)CW(pc) << WSH)); pc += WB;
        STACK_LIM();
        break;
      case OP_ADJ: sp = (word)((uword)sp + ((uword)CW(pc) << WSH)); pc += WB; STACK_LIM(); break;
      case OP_LEV:
        if ((i64)(uword)(bp - s_lo) < (i64)s_len - WB) { x = SW(bp); y = SW(bp + WB); }
        else {
          SAVEP(pc - WB);
          x = s_rd(u, bp);
          FAULTED();
          y = s_rd(u, bp + WB);
          FAULTED();
        }
        sp = bp + 2 * WB; bp = x; pc = y;
        STACK_LIM();
        CODE_LIM();
        break;
      case OP_LI:
        v = (uword)a >> 12; p = v < LIMIT ? rd[v] : -1;
        if (p >= 0 && (a & (WB - 1)) == 0) a = mw[((uword)p | ((uword)a & 4095)) >> WSH];
        else { SAVEP(pc - WB); a = p_rd32(u, a); FAULTED(); }
        break;
      case OP_LC:
        v = (uword)a >> 12; p = v < LIMIT ? rd[v] : -1;
        if (p >= 0) a = (int8_t)mu[(uword)p | ((uword)a & 4095)];
        else { SAVEP(pc - WB); a = p_rd8(u, a); FAULTED(); }
        break;
      case OP_SI:
        x = SW(sp);
        v = (uword)x >> 12; p = v < LIMIT ? wr[v] : -1;
        if (p >= 0 && (x & (WB - 1)) == 0) { mw[((uword)p | ((uword)x & 4095)) >> WSH] = a; sp += WB; }
        else {
          SAVEP(pc - WB);
          p_wr32(u, x, a);
          FAULTED_H();
          sp += WB;
          /* a store into the device window can change what is pending */
          URECHECK();
        }
        break;
      case OP_SC:
        x = SW(sp);
        v = (uword)x >> 12; p = v < LIMIT ? wr[v] : -1;
        if (p >= 0) { mu[(uword)p | ((uword)x & 4095)] = (u8)a; sp += WB; }
        else {
          SAVEP(pc - WB);
          p_wr8(u, x, a);
          FAULTED_H();
          sp += WB;
          URECHECK();
        }
        a = (int8_t)a;
        break;
      /* 16 bits: both bytes in one page, or the long way */
      case OP_LH:
      case OP_LHU:
        v = (uword)a >> 12; p = v < LIMIT ? rd[v] : -1;
        if (p >= 0 && (a & 4095) != 4095) { y = (word)((uword)p | ((uword)a & 4095)); a = mu[y] | mu[y + 1] << 8; }
        else { SAVEP(pc - WB); a = p_rd16(u, a); FAULTED(); }
        if (op == OP_LH) a = (int16_t)a; else a = a & 0xffff;
        break;
      case OP_SH:
        x = SW(sp);
        v = (uword)x >> 12; p = v < LIMIT ? wr[v] : -1;
        if (p >= 0 && (x & 4095) != 4095) { y = (word)((uword)p | ((uword)x & 4095)); mu[y] = (u8)a; mu[y + 1] = (u8)(a >> 8); sp += WB; }
        else {
          SAVEP(pc - WB);
          p_wr16(u, x, a);
          FAULTED_H();
          sp += WB;
          URECHECK();
        }
        break;
      case OP_LSR: a = (word)((uword)SW(sp) >> (a & (WBITS - 1))); sp += WB; break;
      case OP_LCU:
        v = (uword)a >> 12; p = v < LIMIT ? rd[v] : -1;
        if (p >= 0) a = mu[(uword)p | ((uword)a & 4095)];
        else { SAVEP(pc - WB); a = p_rd8(u, a) & 255; FAULTED(); }
        break;
      case OP_PSH: sp -= WB; SW(sp) = a; break;
      case OP_OR: a = SW(sp) | a; sp += WB; break;
      case OP_XOR: a = SW(sp) ^ a; sp += WB; break;
      case OP_AND: a = SW(sp) & a; sp += WB; break;
      case OP_EQ: a = SW(sp) == a; sp += WB; break;
      case OP_NE: a = SW(sp) != a; sp += WB; break;
      case OP_LT: a = SW(sp) < a; sp += WB; break;
      case OP_GT: a = SW(sp) > a; sp += WB; break;
      case OP_LE: a = SW(sp) <= a; sp += WB; break;
      case OP_GE: a = SW(sp) >= a; sp += WB; break;
      case OP_SHL: a = (word)((uword)SW(sp) << (a & (WBITS - 1))); sp += WB; break;
      case OP_SHR: a = SW(sp) >> (a & (WBITS - 1)); sp += WB; break;
      case OP_ADD: a = (word)((uword)SW(sp) + (uword)a); sp += WB; break;
      case OP_SUB: a = (word)((uword)SW(sp) - (uword)a); sp += WB; break;
      case OP_MUL: a = (word)((uword)SW(sp) * (uword)a); sp += WB; break;
      case OP_DIV: { word t = SW(sp); a = a == 0 ? 0 : a == -1 ? (word)((uword)0 - (uword)t) : t / a; sp += WB; break; }
      case OP_MOD: { word t = SW(sp); a = a == 0 ? 0 : a == -1 ? 0 : t % a; sp += WB; break; }

      case OP_LDL:
        y = (word)((uword)bp + ((uword)CW(pc) << WSH));
        if ((uword)(y - s_lo) < (uword)s_len) a = SW(y);
        else { SAVEP(pc - WB); a = s_rd(u, y); FAULTED(); }
        pc += WB;
        break;
      case OP_LDG:
        x = CW(pc);
        v = (uword)x >> 12; p = v < LIMIT ? rd[v] : -1;
        if (p >= 0 && (x & (WB - 1)) == 0) a = mw[((uword)p | ((uword)x & 4095)) >> WSH];
        else { SAVEP(pc - WB); a = p_rd32(u, x); FAULTED(); }
        pc += WB;
        break;
      case OP_PSHL:
        y = (word)((uword)bp + ((uword)CW(pc) << WSH));
        if ((uword)(y - s_lo) < (uword)s_len) a = SW(y);
        else { SAVEP(pc - WB); a = s_rd(u, y); FAULTED(); }
        sp -= WB; SW(sp) = a; pc += WB;
        break;
      case OP_PSHG:
        x = CW(pc);
        v = (uword)x >> 12; p = v < LIMIT ? rd[v] : -1;
        if (p >= 0 && (x & (WB - 1)) == 0) a = mw[((uword)p | ((uword)x & 4095)) >> WSH];
        else { SAVEP(pc - WB); a = p_rd32(u, x); FAULTED(); }
        sp -= WB; SW(sp) = a; pc += WB;
        break;
      case OP_LEAP: a = (word)((uword)bp + ((uword)CW(pc) << WSH)); pc += WB; sp -= WB; SW(sp) = a; break;
      case OP_IMMP: a = CW(pc); pc += WB; sp -= WB; SW(sp) = a; break;
      case OP_LIP:
        v = (uword)a >> 12; p = v < LIMIT ? rd[v] : -1;
        if (p >= 0 && (a & (WB - 1)) == 0) a = mw[((uword)p | ((uword)a & 4095)) >> WSH];
        else { SAVEP(pc - WB); a = p_rd32(u, a); FAULTED(); }
        sp -= WB; SW(sp) = a;
        break;
      case OP_ADDL:
        x = (word)((uword)SW(sp) + (uword)a);
        v = (uword)x >> 12; p = v < LIMIT ? rd[v] : -1;
        if (p >= 0 && (x & (WB - 1)) == 0) a = mw[((uword)p | ((uword)x & 4095)) >> WSH];
        else { SAVEP(pc - WB); a = p_rd32(u, x); FAULTED(); }
        sp += WB;
        break;
      case OP_STL:
        y = (word)((uword)bp + ((uword)CW(pc) << WSH));
        if ((uword)(y - s_lo) < (uword)s_len) SW(y) = a;
        else { SAVEP(pc - WB); s_wr(u, y, a); FAULTED(); }
        pc += WB;
        break;
      case OP_POPA: a = SW(sp); sp += WB; break;

      case OP_JSRI:
        x = CW(pc);
        v = (uword)x >> 12; p = v < LIMIT ? rd[v] : -1;
        if (p >= 0 && (x & (WB - 1)) == 0) x = mw[((uword)p | ((uword)x & 4095)) >> WSH];
        else { SAVEP(pc - WB); x = p_rd32(u, x); FAULTED(); }
        sp -= WB; SW(sp) = pc + WB; pc = x; CODE_LIM();
        break;
      case OP_JSRS:
        y = (word)((uword)bp + ((uword)CW(pc) << WSH));
        if ((uword)(y - s_lo) < (uword)s_len) x = SW(y);
        else { SAVEP(pc - WB); x = s_rd(u, y); FAULTED(); }
        sp -= WB; SW(sp) = pc + WB; pc = x; CODE_LIM();
        break;
      case OP_JMPA: pc = a; CODE_LIM(); break;
      case OP__JMP: pc = SW(sp); sp += WB; CODE_LIM(); break;
      case OP__ADJ: sp = (word)((uword)sp + ((uword)SW(sp) << WSH)); STACK_LIM(); break;
      case OP_C4CY: a = (word)cycle; break;

      case OP_OPCD:
        op = SW(sp);
        if (has_operand(op)) {
          /* as the engine does: a (usually missed) OPV, then C4CY */
          SAVEP(pc);
          eng_opv_trap(m, op);
          LOAD();
          FAULTED();
          a = (word)cycle;
          lim = -1;
          break;
        }
        if (is_plain_op(op)) {
          SAVEP(pc);
          opcd_user(u, op, pc - WB, sp, bp, a);
          FAULTED_H();
          LOAD();
          URECHECK();
          break;
        }
        goto fpu;

      case OP_FPU:
      fpu:
        /* a float's, on the stack, if all of it is in the window (which
         * promises a word either side of sp, no more) */
        if (op == OP_FPU) {
          y = SW(sp);
          if ((uword)(y - 1) < 8 && !m->no_fpu && sp - s_lo <= s_len - 3 * WB) {
            a = fpu_float(y, SW(sp + WB), SW(sp + 2 * WB));
            break;
          }
          /* ... and a double's, if its addresses are in the arrays */
          if ((uword)(y - 17) < 10 && !m->no_fpu && sp - s_lo <= s_len - 4 * WB) {
            if (fpu_user(u, y, SW(sp + WB), SW(sp + 2 * WB), SW(sp + 3 * WB))) { a = u->fpu_v; break; }
          }
        }
        /* fall through */
      default: {
        SAVEP(pc);
        int s = sys_user(u, op, pc - WB, sp, bp, a);
        LOAD();
        cycle = m->cycle;
        URECHECK();
        if (s != STOP_BUDGET) { stop = s; goto out; }
        if (m->dv->halted) { stop = STOP_HALT; goto out; }
        FAULTED();
      }
    }
  }
out:
  /* a fault or a trap left the registers in the machine already */
  if (m->mode && m->vm) { m->pc = pc; m->sp = sp; m->bp = bp; m->a = a; }
  m->cycle = cycle;
  u->s_lo = s_lo; u->s_len = s_len; u->s_off = s_off;
  u->c_lo = c_lo; u->c_len = c_len; u->c_off = c_off;
  m->vm = 0;
  if (m->dv->halted) stop = STOP_HALT;
  return stop;
#undef MW
#undef SAVEP
#undef LOAD
#undef SWITCH_OUT
#undef FAULTED
#undef FAULTED_H
#undef URECHECK
#undef CODE_LIM
#undef STACK_LIM
#undef CW
#undef SW
}

/* the physical page at va for an accelerator's read or write (accel.c,
 * accel.js's mmu.page and throw FAULT) */
word mmu_frame(mmu *u, word va, int write) {
  word p = page(u, va, write ? FK_WRITE : FK_READ);
  if (p < 0) fault(u);
  return p;
}

/* [addr, addr + len) in the space at ptbr, one run of memory, or -1: no
 * fault (the display's ring drained by the host, gui-device.js ringDma) */
i64 mmu_dma_space(machine *m, u32 ptbr, u32 addr, i32 len, int write) { return dma_phys(m, ptbr, addr, len, write, 0); }
word mmu_ptbr(mmu *u) { return u->ptbr; }
