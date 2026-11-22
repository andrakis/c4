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
#define LOCAL_BASE 0x800
#define LOCAL_END  0x900

/* virtual pages the arrays cache: 512 MB, the user window and below */
#define LIMIT 131072u
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
static inline i32 rdw(machine *m, u32 idx) { return idx < (m->size >> 2) ? m->w[idx] : 0; }

/* ---------------------------------------------------------------- */
/* the kernel's map (arena.js): the word of the entry for va in the space
 * whose directory is at dir, or -1 */
i64 pte_word(machine *m, u32 dir, u32 va) {
  if (!dir) return -1;
  i32 pde = rdw(m, (dir >> 2) + (va >> 22));
  if (!(pde & 1)) return -1;
  u32 pt = (u32)pde & ~4095u;
  if ((i64)pt + 4096 > m->size) return -1;
  return (pt >> 2) + ((va >> 12) & 1023);
}
i32 kernel_pte(machine *m, u32 udir, u32 x) {
  i64 w = pte_word(m, udir, x);
  return w < 0 ? 0 : m->w[w];
}
/* the memory at user address x by the kernel's map, the page pinned
 * unless PIN is 0; -1 if it maps nothing there */
i64 kernel_phys(machine *m, u32 udir, u32 x, int pin) {
  i64 w = pte_word(m, udir, x);
  if (w < 0) return -1;
  i32 pte = m->w[w];
  if (!(pte & 1) || (i64)((u32)pte & ~4095u) + 4096 > m->size) return -1;
  if (pin && !(pte & PTE_PIN)) m->w[w] = pte | PTE_PIN;
  return ((u32)pte & ~4095u) | (x & 4095);
}

/* the entry for va in the space at ptbr: 0 when there is none */
static i32 pte_of(machine *m, u32 ptbr, u32 va) {
  if (!ptbr) return 0;
  i32 pde = rdw(m, (ptbr >> 2) + (va >> 22));
  if (!(pde & 1)) return 0;
  u32 pt = (u32)pde & ~4095u;
  if ((i64)pt + 4096 > m->size) return 0;
  return m->w[(pt >> 2) + ((va >> 12) & 1023)];
}

/* Where [addr, addr + len) is, for a device, in the space at ptbr: every
 * page of it user mode's to read (and write), one run of memory, and the
 * physical address of addr returned. Else -1, and dma_bad says where it
 * went wrong and dma_bad_pte what the entry there was. UDIR: the kernel's
 * map, whose pages a device's use pins (0: not pinned) */
static u32 dma_bad; static i32 dma_bad_pte;
static i64 dma_phys(machine *m, u32 ptbr, u32 addr, i32 len, int write, u32 udir) {
  if (len < 1) len = 1;
  i64 end = (i64)addr + len;
  if (addr < 0x1000 || end > 0x100000000LL) { dma_bad = addr; dma_bad_pte = 0; return -1; }
  i32 need = PTE_P | PTE_U | (write ? PTE_W : 0);
  i64 pa = -1; u32 next = 0;
  /* from addr's page, as a signed word (mmu.js's addr & -4096) */
  for (i64 va = (i32)(addr & ~4095u); va < end; va += 4096) {
    i32 pte = pte_of(m, ptbr, (u32)va); u32 f = (u32)pte & ~4095u;
    if ((pte & need) != need || (i64)f + 4096 > m->size || (pa >= 0 && f != next)) {
      dma_bad = va < addr ? addr : (u32)va; dma_bad_pte = pte; return -1;
    }
    if (pa < 0) pa = f | (addr & 4095);
    next = f + 4096;
  }
  /* a page a device has used stays in memory (C4IX keeps it out of swap) */
  if (udir) for (i64 va = (i32)(addr & ~4095u); va < end; va += 4096) kernel_phys(m, udir, (u32)va, 1);
  return pa;
}
static i32 dma_kind(int write) { return (write ? FK_WRITE : FK_READ) | FK_DMA | (dma_bad_pte & PTE_P ? FK_PRESENT : 0); }

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

mmu *mmu_new(machine *m) {
  mmu *u = xmalloc(sizeof *u);
  u->m = m;
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

static void invlpg(mmu *u, i32 va) {
  u32 v = (u32)va >> 12;
  if (v < LIMIT) { u->rd[v] = -1; u->wr[v] = -1; u->ex[v] = -1; u->nx[v] = 0; }
  u->ncwins = 0;
  u->s_lo = u->s_len = u->s_off = 0;
  u->c_lo = u->c_len = u->c_off = 0;
}

/* ---------------------------------------------------------------- */
/* the registers, at 0x500: kernel mode's (user mode's accesses below
 * 0x1000 come through low_ok and never reach them) */
i32 mmu_read32(mmu *u, i32 x) {
  machine *m = u->m;
  switch (x & ~3) {
    case MMU_CTL: return u->on ? 3 : 1;
    case MMU_PTBR: return u->ptbr;
    case MMU_KSP: return u->ksp;
    case MMU_FADDR: return u->faddr;
    case MMU_FINFO: return u->finfo;
    case MMU_FPC: return u->fpc;
    case MMU_IOPM: return u->iopm;
    case MMU_MISSES: { i32 n = u->misses; u->misses = 0; return n; }
    case MMU_FAULTS: return u->faults;
    case MMU_MEMSIZE: return (i32)m->size;
    case MMU_UWIN: return (i32)m->uwin;
    case MMU_UDIR: return (i32)m->udir;
  }
  return 0;
}
void mmu_write32(mmu *u, i32 x, i32 v) {
  machine *m = u->m;
  switch (x & ~3) {
    case MMU_CTL: u->on = (v & 1) != 0; flush(u); return;
    case MMU_PTBR: u->ptbr = v & ~4095; flush(u); return;
    case MMU_KSP: u->ksp = v & ~3; return;
    case MMU_INVLPG: invlpg(u, v); return;
    case MMU_FLUSH: flush(u); return;
    case MMU_IOPM: u->iopm = v; return;
    /* the user window: where user addresses start (engine.c kpa) */
    case MMU_UWIN: m->uwin = (u32)v & ~4095u; return;
    case MMU_UDIR: m->udir = (u32)v & ~4095u; return;
  }
}

/* ---------------------------------------------------------------- */
/* translation: the entry for va, without side effects (0: none) */
static i32 probe(mmu *u, u32 va) {
  machine *m = u->m;
  u32 pd = (u32)u->ptbr;
  u->pte_at = -1;
  if (!pd) return 0;
  i32 pde = rdw(m, (pd >> 2) + (va >> 22));
  if (!(pde & 1)) return 0;
  u32 pt = (u32)pde & ~4095u;
  if ((i64)pt + 4096 > m->size) return 0;
  u->pte_at = (i32)((pt >> 2) + ((va >> 12) & 1023));
  return m->w[u->pte_at];
}

/* v's entries are about to be set: logged, so that a flush clears them */
static void logv(mmu *u, u32 v) {
  if (u->nlog == LIMIT) {
    for (u32 k = 0; k < LIMIT; k++) { u->rd[k] = u->wr[k] = u->ex[k] = -1; u->nx[k] = 0; }
    u->nlog = 0;
  }
  u->log[u->nlog++] = (i32)v;
}

/* the physical page for an access of this kind at va, filling the arrays;
 * or -1, with the fault recorded (FADDR, FINFO, FPC) for raise */
static i32 xlate(mmu *u, i32 va, int kind) {
  machine *m = u->m;
  i32 pte = probe(u, (u32)va);
  u32 frame = (u32)pte & ~4095u;
  if ((pte & 5) != 5 || (i64)frame + 4096 > m->size ||
      (kind == FK_WRITE && !(pte & PTE_W)) || (kind == FK_FETCH && !(pte & PTE_X))) {
    u->faddr = va;
    u->finfo = kind | ((pte & PTE_P) ? FK_PRESENT : 0);
    u->fpc = m->pc;
    u->faults++;
    return -1;
  }
  i32 npte = pte | PTE_A;
  if (kind == FK_WRITE) npte |= PTE_D;
  if (npte != pte) m->w[u->pte_at] = npte;
  u->misses++;
  u32 v = (u32)va >> 12;
  if (v < LIMIT) {
    if (u->rd[v] < 0 && u->wr[v] < 0 && u->ex[v] < 0 && !u->nx[v]) logv(u, v);
    u->rd[v] = (i32)frame;
    /* written to only once it is dirty, so the first store sets D */
    if ((npte & (PTE_W | PTE_D)) == (PTE_W | PTE_D)) u->wr[v] = (i32)frame;
    if (npte & PTE_X) u->ex[v] = (i32)frame;
  }
  return (i32)frame;
}

/* the physical page at va if user mode may run it, filled; -1 if not,
 * which is remembered until the next flush. Raises nothing */
static i32 x_page(mmu *u, i32 va) {
  if ((u32)va < 0x1000) return -1;
  u32 v = (u32)va >> 12;
  if (v < LIMIT) {
    if (u->ex[v] >= 0) return u->ex[v];
    if (u->nx[v]) return -1;
  }
  i32 pte = probe(u, (u32)va);
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
static i32 page(mmu *u, i32 va, int kind) {
  u32 v = (u32)va >> 12;
  if (v < LIMIT) {
    i32 p = kind == FK_READ ? u->rd[v] : kind == FK_WRITE ? u->wr[v] : u->ex[v];
    if (p >= 0) return p;
  }
  return xlate(u, va, kind);
}

static char *hex8(char *buf, u32 v) {
  static const char h[] = "0123456789ABCDEF";
  buf[0] = '0'; buf[1] = 'x';
  for (int i = 0; i < 8; i++) buf[2 + i] = h[(v >> (28 - 4 * i)) & 15];
  buf[10] = 0;
  return buf;
}

/* raise the recorded fault as TRAP_SEGV(FADDR): the machine's registers
 * are the faulting instruction's. With no handler the machine stops */
static void raise_(mmu *u) {
  machine *m = u->m;
  if (!m->trap_handler) {
    char a[12], b[12];
    const char *what = u->finfo & FK_WRITE ? "write to" : u->finfo & FK_FETCH ? "fetch from" : "read of";
    machine_emits(m, "libjs: page fault: "); machine_emits(m, what); machine_emits(m, " ");
    machine_emits(m, hex8(a, (u32)u->faddr)); machine_emits(m, " at pc "); machine_emits(m, hex8(b, (u32)u->fpc));
    machine_emits(m, ", and no trap handler. Halting.\n");
    m->vm = 0;
    eng_halt(m, -104);
    return;
  }
  eng_trap(m, TRAP_SEGV, u->faddr, m->trap_handler);
}

/* a fault found outside xlate: page 0 the IOPM refuses, or a fetch there */
static void refuse(mmu *u, i32 va, int kind) {
  u->faddr = va;
  u->finfo = kind;
  u->fpc = u->m->pc;
  u->faults++;
}

/* A device, reached by this task's load or store, about to use [addr,
 * addr + len): the task's own memory, or the fault it would have taken */
i64 mmu_dma_check(mmu *u, u32 addr, i32 len, int write, int pin) {
  machine *m = u->m;
  i64 pa = dma_phys(m, (u32)u->ptbr, addr, len, write, pin ? m->udir : 0);
  if (pa >= 0) return pa;
  refuse(u, (i32)dma_bad, dma_kind(write));
  fault(u);
}

/* the bus, for user mode: a device it reaches checks the addresses it is
 * given against this task's space while the access lasts */
static i32 low_rd32(mmu *u, i32 x) { return eng_bus_rd32(u->m, (u32)x); }
static i32 bus_rd32(mmu *u, i32 x) {
  u->m->dma_ctx = u;
  i32 v = low_rd32(u, x);
  u->m->dma_ctx = 0;
  return v;
}
static void bus_wr32(mmu *u, i32 x, i32 v) {
  u->m->dma_ctx = u;
  eng_bus_wr32(u->m, (u32)x, v);
  u->m->dma_ctx = 0;
}
static void bus_wr8(mmu *u, i32 x, i32 v) {
  u->m->dma_ctx = u;
  eng_bus_wr8(u->m, (u32)x, v & 0xff);
  u->m->dma_ctx = 0;
}

/* page 0 in user mode, where the IOPM allows */
static int low_ok(mmu *u, i32 x0, int kind) {
  u32 x = (u32)x0; i32 io = u->iopm;
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
i32 mmu_ld8(mmu *u, i32 x) {
  if ((u32)x < 0x1000) {
    if (!low_ok(u, x, FK_READ)) { refuse(u, x, FK_READ | FK_PRESENT); fault(u); }
    return eng_bus_rd8s(u->m, (u32)x);
  }
  i32 p = page(u, x, FK_READ);
  if (p < 0) fault(u);
  return (int8_t)u->m->u8[(u32)p | ((u32)x & 4095)];
}
i32 mmu_ld32(mmu *u, i32 x) {
  if ((u32)x < 0x1000) {
    if (!low_ok(u, x, FK_READ)) { refuse(u, x, FK_READ | FK_PRESENT); fault(u); }
    return bus_rd32(u, x);
  }
  if (x & 3) {
    i32 b0 = mmu_ld8(u, x) & 255, b1 = mmu_ld8(u, (i32)((u32)x + 1)) & 255;
    i32 b2 = mmu_ld8(u, (i32)((u32)x + 2)) & 255, b3 = mmu_ld8(u, (i32)((u32)x + 3));
    return (i32)((u32)b0 | (u32)b1 << 8 | (u32)b2 << 16 | (u32)b3 << 24);
  }
  i32 p = page(u, x, FK_READ);
  if (p < 0) fault(u);
  return u->m->w[((u32)p | ((u32)x & 4095)) >> 2];
}
void mmu_st8(mmu *u, i32 x, i32 v) {
  machine *m = u->m;
  if ((u32)x < 0x1000) {
    if (!low_ok(u, x, FK_WRITE)) { refuse(u, x, FK_WRITE | FK_PRESENT); fault(u); }
    if (is_rom(x, 1)) { eng_rom_store(m, x, v & 0xff); return; }
    bus_wr8(u, x, v);
    return;
  }
  i32 p = page(u, x, FK_WRITE);
  if (p < 0) fault(u);
  m->u8[(u32)p | ((u32)x & 4095)] = (u8)v;
}
void mmu_st32(mmu *u, i32 x, i32 v) {
  machine *m = u->m;
  if ((u32)x < 0x1000) {
    if (!low_ok(u, x, FK_WRITE)) { refuse(u, x, FK_WRITE | FK_PRESENT); fault(u); }
    if (is_rom(x, 4)) { eng_rom_store(m, x, v); return; }
    bus_wr32(u, x, v);
    return;
  }
  if (x & 3) { for (int k = 0; k < 4; k++) mmu_st8(u, (i32)((u32)x + (u32)k), v >> (8 * k)); return; }
  i32 p = page(u, x, FK_WRITE);
  if (p < 0) fault(u);
  m->w[((u32)p | ((u32)x & 4095)) >> 2] = v;
}

/* ---------------------------------------------------------------- */
/* The block opcodes in user mode, a page at a time. A page the task may
 * not use faults part way, as a store loop would; what was done before it
 * stays done, and the instruction, run again once the kernel has brought
 * the page in, goes on from there (part) rather than from its first byte */
__attribute__((noinline)) static void block_part(mmu *u, i32 op, i32 sp, i32 d, i32 s, i32 n, i32 k) {
  u->part.has = 1; u->part.op = op; u->part.pc = u->m->pc; u->part.sp = sp; u->part.ptbr = u->ptbr;
  u->part.d = d; u->part.s = s; u->part.n = n; u->part.k = k;
}
__attribute__((noinline)) static void block_fault(mmu *u, void *outer, i32 op, i32 sp, i32 d, i32 s, i32 n, i32 k) {
  u->jb = outer;
  block_part(u, op, sp, d, s, n, k);
  fault(u);
}
int mmu_block(mmu *u, i32 op) {
  machine *m = u->m;
  u8 *const u8m = m->u8;
  i32 sp = m->sp;
  i32 d = mmu_ld32(u, sp + 8), s = mmu_ld32(u, sp + 4), n = mmu_ld32(u, sp);
  /* the same instruction again: same space, pc, stack and operands */
  volatile i32 k = 0;
  if (u->part.has && u->part.op == op && u->part.pc == m->pc && u->part.sp == sp && u->part.ptbr == u->ptbr &&
      u->part.d == d && u->part.s == s && u->part.n == n) k = u->part.k;
  u->part.has = 0;
  void *outer = u->jb;
  c4_jmp_buf jb;
  if (c4_setjmp(jb)) block_fault(u, outer, op, sp, d, s, n, k);   /* a fault in a load or store below */
  u->jb = &jb;
  if (op == OP_MSET) {
    i32 v = s & 0xff;
    while (k < n) {
      i32 x = (i32)((u32)d + (u32)k);
      if ((u32)x < 0x1000) { mmu_st8(u, x, v); k = k + 1; continue; }
      i32 run = n - k < 4096 - (x & 4095) ? n - k : 4096 - (x & 4095);
      i32 p = page(u, x, FK_WRITE);
      if (p < 0) block_fault(u, outer, op, sp, d, s, n, k);
      memset(u8m + ((u32)p | ((u32)x & 4095)), v, (u32)run);
      k = k + run;
    }
    u->jb = outer;
    m->a = d;
    return STOP_BUDGET;
  }
  if (op == OP_MCMP) {
    i32 r = 0;
    for (; k < n; k = k + 1) {
      i32 c = (mmu_ld8(u, (i32)((u32)d + (u32)k)) & 255) - (mmu_ld8(u, (i32)((u32)s + (u32)k)) & 255);
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
      for (; k < n; k = k + 1) mmu_st8(u, (i32)((u32)d + (u32)k), mmu_ld8(u, (i32)((u32)s + (u32)k)));
    } else {
      while (k < n) {
        i32 x = (i32)((u32)d + (u32)k), y = (i32)((u32)s + (u32)k);
        if ((u32)x < 0x1000 || (u32)y < 0x1000) { mmu_st8(u, x, mmu_ld8(u, y)); k = k + 1; continue; }
        i32 run = n - k;
        if (4096 - (x & 4095) < run) run = 4096 - (x & 4095);
        if (4096 - (y & 4095) < run) run = 4096 - (y & 4095);
        i32 ps = page(u, y, FK_READ);
        if (ps < 0) block_fault(u, outer, op, sp, d, s, n, k);
        i32 pd = page(u, x, FK_WRITE);
        if (pd < 0) block_fault(u, outer, op, sp, d, s, n, k);
        memmove(u8m + ((u32)pd | ((u32)x & 4095)), u8m + ((u32)ps | ((u32)y & 4095)), (u32)run);
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
static void opcd(mmu *u, i32 op) {
  machine *mm = u->m;
  i32 sp = mm->sp, a = mm->a, x = 0;
  switch (op) {
    case OP_LEV: {
      i32 bp = mm->bp;
      i32 nbp = mmu_ld32(u, bp), npc = mmu_ld32(u, bp + 4);
      mm->bp = nbp; mm->pc = npc; sp = bp + 8;
      break;
    }
    case OP_LI: a = mmu_ld32(u, a); break;
    case OP_LC: a = mmu_ld8(u, a); break;
    case OP_SI: x = mmu_ld32(u, sp); mmu_st32(u, x, a); sp += 4; break;
    case OP_SC: x = mmu_ld32(u, sp); mmu_st8(u, x, a); sp += 4; a = (int8_t)a; break;
    case OP_LH: { i32 lo = mmu_ld8(u, a) & 255; a = lo | (i32)((u32)mmu_ld8(u, (i32)((u32)a + 1)) << 8); break; }
    case OP_LHU: { i32 lo = mmu_ld8(u, a) & 255; a = lo | ((mmu_ld8(u, (i32)((u32)a + 1)) & 255) << 8); break; }
    case OP_SH: x = mmu_ld32(u, sp); mmu_st8(u, x, a); mmu_st8(u, (i32)((u32)x + 1), a >> 8); sp += 4; break;
    case OP_LSR: x = mmu_ld32(u, sp); a = (i32)((u32)x >> (a & 31)); sp += 4; break;
    case OP_LCU: a = mmu_ld8(u, a) & 255; break;
    case OP_PSH: mmu_st32(u, sp - 4, a); sp -= 4; break;
    case OP_LIP: a = mmu_ld32(u, a); mmu_st32(u, sp - 4, a); sp -= 4; break;
    case OP_ADDL: x = (i32)((u32)mmu_ld32(u, sp) + (u32)a); a = mmu_ld32(u, x); sp += 4; break;
    case OP_POPA: a = mmu_ld32(u, sp); sp += 4; break;
    case OP_JMPA: mm->pc = a; break;
    case OP__JMP: mm->pc = mmu_ld32(u, sp); sp += 4; break;
    case OP__ADJ: sp = (i32)((u32)sp + ((u32)mmu_ld32(u, sp) << 2)); break;
    case OP_C4CY: a = (i32)mm->cycle; break;
    default: {
      i32 t = mmu_ld32(u, sp); sp += 4;
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
  mm->sp = sp; mm->a = a;
}

/* the opcode at pc in user mode, for boundary's "TLEV is never
 * interrupted": -1 if it cannot be read */
i32 mmu_peek(mmu *u, i32 pc) {
  if ((u32)pc < 0x1000) return (u32)pc == TLEV_ADDR ? OP_TLEV : -1;
  u32 v = (u32)pc >> 12;
  i32 p = v < LIMIT ? u->ex[v] : -1;
  return p >= 0 ? u->m->w[((u32)p | ((u32)pc & 4095)) >> 2] : -1;
}

/* ---------------------------------------------------------------- */
/* The loop's slow paths. Each is called with the machine's registers saved
 * as the instruction found them, and on a fault raises it: the caller
 * looks at m->mode, which the trap has made 0 */

/* the physical page at va if user mode may read and write it, filled (and
 * made dirty); -1, raising nothing, if not */
static i32 rw_page(mmu *u, i32 va) {
  if ((u32)va < 0x1000) return -1;
  u32 v = (u32)va >> 12;
  if (v < LIMIT && u->wr[v] >= 0) return u->wr[v];
  i32 pte = probe(u, (u32)va);
  if ((pte & (PTE_P | PTE_U | PTE_W)) != (PTE_P | PTE_U | PTE_W)) return -1;
  return xlate(u, va, FK_WRITE);
}

/* The stack window: [s_lo, s_lo + s_len) virtual, at s_off from physical,
 * pages user mode may read and write, around sp: kept if it still has a
 * word either side of sp, else made there, joined to the old one when they
 * are one run, and grown by up to STACK_GROW pages each way while the next
 * page follows on in physical memory. 0 if a word either side of sp is
 * not the task's to write */
static int s_fix(mmu *u, i32 sp) {
  i32 lo = (sp - 4) & ~4095, hi = sp & ~4095;
  i32 p = rw_page(u, lo);
  if (p < 0) return 0;
  i32 off = p - lo;
  i32 wlo = lo, whi = lo + 4096;
  if (hi != lo) {
    i32 q = rw_page(u, hi);
    if (q < 0 || q - hi != off) return 0;
    whi = hi + 4096;
  }
  if (u->s_len && off == u->s_off && (i64)wlo <= (i64)u->s_lo + u->s_len && whi >= u->s_lo) {
    if (u->s_lo < wlo) wlo = u->s_lo;
    if (u->s_lo + u->s_len > whi) whi = u->s_lo + u->s_len;
  }
  for (int k = 0; k < STACK_GROW; k++) {
    i32 q = rw_page(u, wlo - 4096);
    if (q < 0 || q - (wlo - 4096) != off) break;
    wlo -= 4096;
  }
  for (int k = 0; k < STACK_GROW; k++) {
    i32 q = rw_page(u, whi);
    if (q < 0 || q - whi != off) break;
    whi += 4096;
  }
  u->s_lo = wlo; u->s_len = whi - wlo; u->s_off = off;
  return 1;
}

/* sp is outside the task's writable memory: the fault the next push or
 * pop would take, raised before the instruction runs */
static void stack_fault(mmu *u, i32 sp) {
  if (rw_page(u, sp - 4) < 0) refuse(u, sp - 4, FK_WRITE);
  else refuse(u, sp, FK_READ);
  raise_(u);
}

/* the catches: a fault in a load or store raises it (mmu.js sRd, pRd32 ...) */
#define CATCH_BEGIN(u) \
  void *outer_ = (u)->jb; c4_jmp_buf jb_; \
  if (c4_setjmp(jb_)) { (u)->jb = outer_; raise_(u); return 0; } \
  (u)->jb = &jb_;
#define CATCH_END(u) ((u)->jb = outer_)
__attribute__((noinline)) static i32 p_rd32(mmu *u, i32 x) { CATCH_BEGIN(u) i32 v = mmu_ld32(u, x); CATCH_END(u); return v; }
__attribute__((noinline)) static i32 p_rd8(mmu *u, i32 x) { CATCH_BEGIN(u) i32 v = mmu_ld8(u, x); CATCH_END(u); return v; }
__attribute__((noinline)) static int p_wr32(mmu *u, i32 x, i32 v) { CATCH_BEGIN(u) mmu_st32(u, x, v); CATCH_END(u); return 0; }
__attribute__((noinline)) static int p_wr8(mmu *u, i32 x, i32 v) { CATCH_BEGIN(u) mmu_st8(u, x, v); CATCH_END(u); return 0; }
/* a short, two bytes, low first: a fault at either raises it from the
 * instruction's start, which runs again from nothing (LH, SH) */
__attribute__((noinline)) static i32 p_rd16(mmu *u, i32 x) {
  CATCH_BEGIN(u)
  i32 lo = mmu_ld8(u, x) & 255;
  i32 v = lo | (i32)((u32)mmu_ld8(u, (i32)((u32)x + 1)) << 8);
  CATCH_END(u);
  return v;
}
__attribute__((noinline)) static int p_wr16(mmu *u, i32 x, i32 v) {
  CATCH_BEGIN(u)
  mmu_st8(u, x, v); mmu_st8(u, (i32)((u32)x + 1), v >> 8);
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
static void c_fill(mmu *u, i32 pc) {
  machine *m = u->m; i32 *w = u->cwins;
  if ((u32)pc < 0x1000) {
    /* the ROM's TLEV word, where trap frames return, and the CPUH word;
     * nothing else runs. The window is that word alone: c_len 0 */
    if ((u32)pc == TLEV_ADDR || (u32)pc == CPU_HALT_ADDR) {
      u->c_lo = pc; u->c_off = 0; u->c_len = 0;
      return;
    }
    refuse(u, pc, FK_FETCH); raise_(u); return;
  }
  for (int k = 0; k < u->ncwins; k++) {
    if ((u32)(pc - w[3 * k]) < (u32)w[3 * k + 1]) {
      u->c_lo = w[3 * k]; u->c_len = w[3 * k + 1]; u->c_off = w[3 * k + 2];
      return;
    }
  }
  i32 p = page(u, pc, FK_FETCH);
  if (p < 0) { raise_(u); return; }
  i32 vp = pc & ~4095, off = p - vp;
  i32 lo = vp, hi = vp + 4096;
  for (int k = 0; k < CODE_GROW; k++) {
    i32 q = x_page(u, lo - 4096);
    if (q < 0 || q - (lo - 4096) != off) break;
    lo -= 4096;
  }
  for (int k = 0; k < CODE_GROW; k++) {
    i32 q = x_page(u, hi);
    if (q < 0 || q - hi != off) break;
    hi += 4096;
  }
  u->c_lo = lo; u->c_off = off; u->c_len = hi - lo - 4;
  /* kept, the newest first */
  int n = u->ncwins < CODE_WINS ? u->ncwins++ : CODE_WINS - 1;
  memmove(w + 3, w, (size_t)(3 * n) * 4);
  w[0] = lo; w[1] = hi - lo - 4; w[2] = off;
  /* the last word of the window, whose operand would be past its end */
  if ((u32)(pc - u->c_lo) >= (u32)u->c_len && has_operand(m->w[((u32)p | ((u32)pc & 4095)) >> 2])) {
    i32 q = page(u, pc + 4, FK_FETCH);
    if (q < 0) { raise_(u); return; }
    /* an operand across a page boundary into memory that is not next to
     * the opcode's: a fault */
    refuse(u, pc + 4, FK_FETCH | 16); raise_(u);
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
__attribute__((noinline)) static int sys_user(mmu *u, i32 op, i32 ip, i32 sp, i32 bp, i32 a) {
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
__attribute__((noinline)) static void opcd_user(mmu *u, i32 op, i32 ip, i32 sp, i32 bp, i32 a) {
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
static int fpu_user(mmu *u, i32 op, i32 pa, i32 pb, i32 pr) {
  if (op == 21) return 0;
  i32 *rd = u->rd, *wr = u->wr; int bit = 1 << (op - 17);
  i32 a = pa, ib = 0, ir = 0, p; u32 v;
  if (FPU_A & bit) {
    if ((pa & 3) != 0 || (pa & 4095) > 4088) return 0;
    v = (u32)pa >> 12; p = v < LIMIT ? rd[v] : -1;
    if (p < 0) return 0;
    a = (i32)(((u32)p | ((u32)pa & 4095)) >> 2);
  }
  if (FPU_B & bit) {
    if ((pb & 3) != 0 || (pb & 4095) > 4088) return 0;
    v = (u32)pb >> 12; p = v < LIMIT ? rd[v] : -1;
    if (p < 0) return 0;
    ib = (i32)(((u32)p | ((u32)pb & 4095)) >> 2);
  }
  if (FPU_R & bit) {
    if ((pr & 3) != 0 || (pr & 4095) > 4088) return 0;
    v = (u32)pr >> 12; p = v < LIMIT ? wr[v] : -1;
    if (p < 0) return 0;
    ir = (i32)(((u32)p | ((u32)pr & 4095)) >> 2);
  }
  i32 res;
  fpu_double_plain(u->m->w, op, a, ib, ir, &res);
  u->fpu_v = res;
  return 1;
}

static i64 next_multiple(i64 cycle, i32 interval) { return (cycle / interval + 1) * interval; }

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
  i32 *const mw = m->w;
  u8 *const mu = m->u8;
  const u32 wm = m->wmask;
#define MW(x) mw[((u32)(x) >> 2) & wm]
  const i32 *const rd = u->rd, *const wr = u->wr;
  if (m->halted) return STOP_HALT;
  const int ident = !u->general;
  i32 pc = m->pc, sp = m->sp, bp = m->bp, a = m->a;
  i64 cycle = m->cycle;
  const i64 end = cycle + max_cycles;
  i32 interval = m->cycle_interval;
  i64 next_irq = interval > 0 ? next_multiple(cycle, interval) : 0;
  int chk = eng_need_checks(m);
  i64 next_chk = 0, lim = -1, yy;
  int stop = STOP_BUDGET;
  i32 op = 0, x = 0, y = 0, p = 0; u32 v = 0;
  i32 s_lo = u->s_lo, s_len = u->s_len, s_off = u->s_off;
  i32 c_lo = u->c_lo, c_len = u->c_len, c_off = u->c_off;
  m->vm = 1;
#define SAVEP(P) (m->pc = (P), m->sp = sp, m->bp = bp, m->a = a, m->cycle = cycle)
#define LOAD() (pc = m->pc, sp = m->sp, bp = m->bp, a = m->a)
#define SWITCH_OUT() do { stop = STOP_SWITCH; goto out; } while (0)
#define FAULTED() do { if (!m->mode) SWITCH_OUT(); } while (0)
#define FAULTED_H() do { if (!m->mode || m->halted) { stop = m->halted ? STOP_HALT : STOP_SWITCH; goto out; } } while (0)
#define URECHECK() do { interval = m->cycle_interval; \
    next_irq = interval > 0 ? next_multiple(cycle, interval) : 0; \
    chk = eng_need_checks(m); next_chk = 0; lim = -1; } while (0)
  /* how far the code window is good for after a jump (no -1: the jump's own) */
#define CODE_LIM() do { if ((u32)(pc - c_lo) < (u32)c_len) { \
      yy = cycle + ((i32)((u32)c_lo + (u32)c_len + 4 - (u32)pc) >> 3); if (yy < lim) lim = yy; } else lim = -1; } while (0)
#define STACK_LIM() do { x = sp - s_lo; if (x < 4 || x > s_len - 4) lim = -1; \
    else { yy = cycle + ((x < s_len - x ? x : s_len - x) >> 2); if (yy < lim) lim = yy; } } while (0)
#define CW(x) MW((u32)(x) + (u32)c_off)
#define SW(x) MW((u32)(x) + (u32)s_off)

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
      if ((u32)(pc - c_lo) >= (u32)c_len) {
        SAVEP(pc);
        c_fill(u, pc);
        FAULTED_H();
        c_lo = u->c_lo; c_len = u->c_len; c_off = u->c_off;
        if (ident && c_off != 0) { u->general = 1; SWITCH_OUT(); }
      }
      /* the stack window: a word either side of sp */
      x = sp - s_lo;
      if (x < 4 || x > s_len - 4) {
        SAVEP(pc);
        if (!s_fix(u, sp)) {
          stack_fault(u, sp);
          stop = m->halted ? STOP_HALT : STOP_SWITCH;
          goto out;
        }
        s_lo = u->s_lo; s_len = u->s_len; s_off = u->s_off;
        if (ident && s_off != 0) { u->general = 1; SWITCH_OUT(); }
        x = sp - s_lo;
      }
      /* how many instructions run before the next look */
      lim = chk && next_chk - 1 < end ? next_chk - 1 : end;
      yy = cycle + ((i32)((u32)c_lo + (u32)c_len + 4 - (u32)pc) >> 3) - 1;
      if (yy < lim) lim = yy;
      yy = cycle + ((x < s_len - x ? x : s_len - x) >> 2) - 1;
      if (yy < lim) lim = yy;
      if (lim < cycle) lim = cycle;
    }

    op = CW(pc);
    pc += 4;

    switch (op) {
      case OP_LEA: a = (i32)((u32)bp + ((u32)CW(pc) << 2)); pc += 4; break;
      case OP_IMM: a = CW(pc); pc += 4; break;
      case OP_JMP: pc = CW(pc); CODE_LIM(); break;
      case OP_JSR: sp -= 4; SW(sp) = pc + 4; pc = CW(pc); CODE_LIM(); break;
      case OP_BZ: if (a) pc += 4; else { pc = CW(pc); CODE_LIM(); } break;
      case OP_BNZ: if (a) { pc = CW(pc); CODE_LIM(); } else pc += 4; break;
      case OP_ENT:
        sp -= 4; SW(sp) = bp; bp = sp;
        sp = (i32)((u32)sp - ((u32)CW(pc) << 2)); pc += 4;
        STACK_LIM();
        break;
      case OP_ADJ: sp = (i32)((u32)sp + ((u32)CW(pc) << 2)); pc += 4; STACK_LIM(); break;
      case OP_LEV:
        if ((i64)(u32)(bp - s_lo) < (i64)s_len - 4) { x = SW(bp); y = SW(bp + 4); }
        else {
          SAVEP(pc - 4);
          x = s_rd(u, bp);
          FAULTED();
          y = s_rd(u, bp + 4);
          FAULTED();
        }
        sp = bp + 8; bp = x; pc = y;
        STACK_LIM();
        CODE_LIM();
        break;
      case OP_LI:
        v = (u32)a >> 12; p = v < LIMIT ? rd[v] : -1;
        if (p >= 0 && (a & 3) == 0) a = mw[((u32)p | ((u32)a & 4095)) >> 2];
        else { SAVEP(pc - 4); a = p_rd32(u, a); FAULTED(); }
        break;
      case OP_LC:
        v = (u32)a >> 12; p = v < LIMIT ? rd[v] : -1;
        if (p >= 0) a = (int8_t)mu[(u32)p | ((u32)a & 4095)];
        else { SAVEP(pc - 4); a = p_rd8(u, a); FAULTED(); }
        break;
      case OP_SI:
        x = SW(sp);
        v = (u32)x >> 12; p = v < LIMIT ? wr[v] : -1;
        if (p >= 0 && (x & 3) == 0) { mw[((u32)p | ((u32)x & 4095)) >> 2] = a; sp += 4; }
        else {
          SAVEP(pc - 4);
          p_wr32(u, x, a);
          FAULTED_H();
          sp += 4;
          /* a store into the device window can change what is pending */
          URECHECK();
        }
        break;
      case OP_SC:
        x = SW(sp);
        v = (u32)x >> 12; p = v < LIMIT ? wr[v] : -1;
        if (p >= 0) { mu[(u32)p | ((u32)x & 4095)] = (u8)a; sp += 4; }
        else {
          SAVEP(pc - 4);
          p_wr8(u, x, a);
          FAULTED_H();
          sp += 4;
          URECHECK();
        }
        a = (int8_t)a;
        break;
      /* 16 bits: both bytes in one page, or the long way */
      case OP_LH:
      case OP_LHU:
        v = (u32)a >> 12; p = v < LIMIT ? rd[v] : -1;
        if (p >= 0 && (a & 4095) != 4095) { y = (i32)((u32)p | ((u32)a & 4095)); a = mu[y] | mu[y + 1] << 8; }
        else { SAVEP(pc - 4); a = p_rd16(u, a); FAULTED(); }
        if (op == OP_LH) a = (int16_t)a; else a = a & 0xffff;
        break;
      case OP_SH:
        x = SW(sp);
        v = (u32)x >> 12; p = v < LIMIT ? wr[v] : -1;
        if (p >= 0 && (x & 4095) != 4095) { y = (i32)((u32)p | ((u32)x & 4095)); mu[y] = (u8)a; mu[y + 1] = (u8)(a >> 8); sp += 4; }
        else {
          SAVEP(pc - 4);
          p_wr16(u, x, a);
          FAULTED_H();
          sp += 4;
          URECHECK();
        }
        break;
      case OP_LSR: a = (i32)((u32)SW(sp) >> (a & 31)); sp += 4; break;
      case OP_LCU:
        v = (u32)a >> 12; p = v < LIMIT ? rd[v] : -1;
        if (p >= 0) a = mu[(u32)p | ((u32)a & 4095)];
        else { SAVEP(pc - 4); a = p_rd8(u, a) & 255; FAULTED(); }
        break;
      case OP_PSH: sp -= 4; SW(sp) = a; break;
      case OP_OR: a = SW(sp) | a; sp += 4; break;
      case OP_XOR: a = SW(sp) ^ a; sp += 4; break;
      case OP_AND: a = SW(sp) & a; sp += 4; break;
      case OP_EQ: a = SW(sp) == a; sp += 4; break;
      case OP_NE: a = SW(sp) != a; sp += 4; break;
      case OP_LT: a = SW(sp) < a; sp += 4; break;
      case OP_GT: a = SW(sp) > a; sp += 4; break;
      case OP_LE: a = SW(sp) <= a; sp += 4; break;
      case OP_GE: a = SW(sp) >= a; sp += 4; break;
      case OP_SHL: a = (i32)((u32)SW(sp) << (a & 31)); sp += 4; break;
      case OP_SHR: a = SW(sp) >> (a & 31); sp += 4; break;
      case OP_ADD: a = (i32)((u32)SW(sp) + (u32)a); sp += 4; break;
      case OP_SUB: a = (i32)((u32)SW(sp) - (u32)a); sp += 4; break;
      case OP_MUL: a = (i32)((u32)SW(sp) * (u32)a); sp += 4; break;
      case OP_DIV: { i32 t = SW(sp); a = a == 0 ? 0 : a == -1 ? (i32)(0u - (u32)t) : t / a; sp += 4; break; }
      case OP_MOD: { i32 t = SW(sp); a = a == 0 ? 0 : a == -1 ? 0 : t % a; sp += 4; break; }

      case OP_LDL:
        y = (i32)((u32)bp + ((u32)CW(pc) << 2));
        if ((u32)(y - s_lo) < (u32)s_len) a = SW(y);
        else { SAVEP(pc - 4); a = s_rd(u, y); FAULTED(); }
        pc += 4;
        break;
      case OP_LDG:
        x = CW(pc);
        v = (u32)x >> 12; p = v < LIMIT ? rd[v] : -1;
        if (p >= 0 && (x & 3) == 0) a = mw[((u32)p | ((u32)x & 4095)) >> 2];
        else { SAVEP(pc - 4); a = p_rd32(u, x); FAULTED(); }
        pc += 4;
        break;
      case OP_PSHL:
        y = (i32)((u32)bp + ((u32)CW(pc) << 2));
        if ((u32)(y - s_lo) < (u32)s_len) a = SW(y);
        else { SAVEP(pc - 4); a = s_rd(u, y); FAULTED(); }
        sp -= 4; SW(sp) = a; pc += 4;
        break;
      case OP_PSHG:
        x = CW(pc);
        v = (u32)x >> 12; p = v < LIMIT ? rd[v] : -1;
        if (p >= 0 && (x & 3) == 0) a = mw[((u32)p | ((u32)x & 4095)) >> 2];
        else { SAVEP(pc - 4); a = p_rd32(u, x); FAULTED(); }
        sp -= 4; SW(sp) = a; pc += 4;
        break;
      case OP_LEAP: a = (i32)((u32)bp + ((u32)CW(pc) << 2)); pc += 4; sp -= 4; SW(sp) = a; break;
      case OP_IMMP: a = CW(pc); pc += 4; sp -= 4; SW(sp) = a; break;
      case OP_LIP:
        v = (u32)a >> 12; p = v < LIMIT ? rd[v] : -1;
        if (p >= 0 && (a & 3) == 0) a = mw[((u32)p | ((u32)a & 4095)) >> 2];
        else { SAVEP(pc - 4); a = p_rd32(u, a); FAULTED(); }
        sp -= 4; SW(sp) = a;
        break;
      case OP_ADDL:
        x = (i32)((u32)SW(sp) + (u32)a);
        v = (u32)x >> 12; p = v < LIMIT ? rd[v] : -1;
        if (p >= 0 && (x & 3) == 0) a = mw[((u32)p | ((u32)x & 4095)) >> 2];
        else { SAVEP(pc - 4); a = p_rd32(u, x); FAULTED(); }
        sp += 4;
        break;
      case OP_STL:
        y = (i32)((u32)bp + ((u32)CW(pc) << 2));
        if ((u32)(y - s_lo) < (u32)s_len) SW(y) = a;
        else { SAVEP(pc - 4); s_wr(u, y, a); FAULTED(); }
        pc += 4;
        break;
      case OP_POPA: a = SW(sp); sp += 4; break;

      case OP_JSRI:
        x = CW(pc);
        v = (u32)x >> 12; p = v < LIMIT ? rd[v] : -1;
        if (p >= 0 && (x & 3) == 0) x = mw[((u32)p | ((u32)x & 4095)) >> 2];
        else { SAVEP(pc - 4); x = p_rd32(u, x); FAULTED(); }
        sp -= 4; SW(sp) = pc + 4; pc = x; CODE_LIM();
        break;
      case OP_JSRS:
        y = (i32)((u32)bp + ((u32)CW(pc) << 2));
        if ((u32)(y - s_lo) < (u32)s_len) x = SW(y);
        else { SAVEP(pc - 4); x = s_rd(u, y); FAULTED(); }
        sp -= 4; SW(sp) = pc + 4; pc = x; CODE_LIM();
        break;
      case OP_JMPA: pc = a; CODE_LIM(); break;
      case OP__JMP: pc = SW(sp); sp += 4; CODE_LIM(); break;
      case OP__ADJ: sp = (i32)((u32)sp + ((u32)SW(sp) << 2)); STACK_LIM(); break;
      case OP_C4CY: a = (i32)cycle; break;

      case OP_OPCD:
        op = SW(sp);
        if (has_operand(op)) {
          /* as the engine does: a (usually missed) OPV, then C4CY */
          SAVEP(pc);
          eng_opv_trap(m, op);
          LOAD();
          FAULTED();
          a = (i32)cycle;
          lim = -1;
          break;
        }
        if (is_plain_op(op)) {
          SAVEP(pc);
          opcd_user(u, op, pc - 4, sp, bp, a);
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
          if ((u32)(y - 1) < 8 && !m->no_fpu && sp - s_lo <= s_len - 12) {
            a = fpu_float(y, SW(sp + 4), SW(sp + 8));
            break;
          }
          /* ... and a double's, if its addresses are in the arrays */
          if ((u32)(y - 17) < 10 && !m->no_fpu && sp - s_lo <= s_len - 16) {
            if (fpu_user(u, y, SW(sp + 4), SW(sp + 8), SW(sp + 12))) { a = u->fpu_v; break; }
          }
        }
        /* fall through */
      default: {
        SAVEP(pc);
        int s = sys_user(u, op, pc - 4, sp, bp, a);
        LOAD();
        cycle = m->cycle;
        URECHECK();
        if (s != STOP_BUDGET) { stop = s; goto out; }
        if (m->halted) { stop = STOP_HALT; goto out; }
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
  if (m->halted) stop = STOP_HALT;
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
