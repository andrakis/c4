/* c4pc.h -- the c4pc machine: libjs's engine (libjs/c4m.js, boot.js,
 * heap.js, printf.js and c4bb's sim/devices.js, arena.js, loader.js,
 * fpu.js) in C, for a PC with no operating system under it, and for
 * Linux, where the same engine runs as c4pc-host so that it can be held
 * against libjs on the same images (docs/c4pc.md).
 *
 * One CPU, no display or sound yet; an MMU when the platform fits one
 * (mmu.c), as libjs's --mmu does. Every address the guest sees is a byte
 * offset into one arena, as in libjs, or with the MMU on, user mode's
 * goes through the guest's page tables to one.
 *
 * The platform (pc/pc.c, or host.c on Linux) gives the engine a clock,
 * a console and somewhere to wait; the engine gives the platform a
 * machine to run (plat_*, below).
 */
#ifndef C4PC_H
#define C4PC_H

#include <stdint.h>
#include <stddef.h>

typedef int64_t i64;
typedef uint32_t u32;
typedef int32_t i32;
typedef uint8_t u8;

/* ---- opcodes (c4m.js OP) ---- */
enum {
  OP_LEA, OP_IMM, OP_JMP, OP_JSR, OP_BZ, OP_BNZ, OP_ENT, OP_ADJ,
  OP_LEV, OP_LI, OP_LC, OP_SI, OP_SC, OP_PSH,
  OP_OR, OP_XOR, OP_AND, OP_EQ, OP_NE, OP_LT, OP_GT, OP_LE, OP_GE,
  OP_SHL, OP_SHR, OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD,
  OP_OPEN, OP_READ, OP_CLOS, OP_PRTF, OP_MALC, OP_FREE, OP_MSET, OP_MCMP, OP_EXIT,
  OP_PUTC, OP_PUTS, OP_RALC, OP_MCPY, OP_STRC, OP_ITH, OP__OPC, OP__BLT, OP__TRP, OP_OPCD,
  OP__JMP, OP__ADJ, OP_C4CF, OP_C4CY, OP_TIME, OP_SIGH, OP_SIGI, OP_USLP, OP_INFO, OP_OPSL,
  OP_C4IV, OP_FLT, OP_JSRI, OP_JSRS, OP_JMPA, OP_TLEV, OP_DBG,
  OP_CPUI, OP_CPUN, OP_CPUS, OP_CPUH, OP_CAS, OP_XCHG, OP_FADD, OP_CWAI, OP_CWAK, OP_IPI,
  OP_LXI, OP_SXI, OP_TRAW,
  OP_LDL, OP_LDG, OP_PSHL, OP_PSHG, OP_LEAP, OP_IMMP, OP_LIP, OP_ADDL, OP_STL, OP_POPA, OP_FPU,
  OP_LH, OP_LHU, OP_SH, OP_LSR, OP_LCU,
  INS_SIZE
};
extern const char OPNAMES[];   /* 5 bytes an opcode, the ROM at 0x200 */

/* ---- the memory map (arena.js) ---- */
#define TLEV_ADDR     0x10
#define CPU_HALT_ADDR 0x18
#define DEV_BASE      0x100
#define DEV_END       0x200
#define OPNAMES_ROM   0x200
#define MEM_BASE      0x1000
#define ARGS_RESERVE  4096
#define NULL_GUARD_OP 0x4c554e00

/* ---- why run() stopped (c4m.js STOP). PARK and CPUHALT are SMP's,
 * which c4pc has not; SWITCH is the MMU's: the mode changed, and with it
 * the loop to run (mmu.c run). A host that sees it runs on. ---- */
enum { STOP_BUDGET, STOP_HALT, STOP_INPUT, STOP_SLEEP, STOP_PARK, STOP_CPUHALT, STOP_SWITCH };

enum { TRAP_ILLOP, TRAP_HARD_IRQ, TRAP_SOFT_IRQ, TRAP_SIGNAL, TRAP_SEGV, TRAP_OPV, TRAP_PM, TRAP_DEBUG };

/* ---- capability bits (c4m.js, devices.js) ---- */
#define C4I_C4M   0x2
#define C4I_C4MJS 0x8
#define C4I_HRT   0x10
#define C4I_SIG   0x20
#define C4I_FLT   0x40
#define C4I_PROT  0x80
#define C4I_TRAPH 0x400
#define C4I_PIT   0x800
#define C4I_MBOX  0x2000
#define C4I_FPU   0x10000
#define C4I_DRV   0x40000
#define C4I_MMU   0x80000

/* ---- the MMU's registers, a window at 0x500 (mmu.c, libjs/mmu.js) ---- */
#define MMU_BASE  0x500
#define MMU_END   0x540
/* a C4IX page table entry's bits the host looks at (src/c4bb/sim/arena.js) */
#define PTE_PIN   0x200
#define PTE_SWAP  0x400

/* ---- a file's bytes: shared by the drive that holds it and every fd
 * open on it, freed when the last lets go (a file the ramdisk brought
 * is never freed: refs < 0) ---- */
typedef struct blob { int refs; u32 len; u8 *bytes; } blob;
blob *blob_new(u32 len);           /* bytes zeroed */
blob *blob_static(u8 *bytes, u32 len);
void blob_ref(blob *b);
void blob_unref(blob *b);

/* ---- a drive: files by '/'-joined path from its root ---- */
typedef struct drive {
  struct smap *files;              /* name -> blob */
  int writable;
  char **dirs; int ndirs;          /* the search list (DISK_DIRS) */
  void (*sink)(struct drive *, const char *name, blob *);   /* a file written, or 0 */
  void (*remove)(struct drive *, const char *name);
  void *ctx;                       /* the platform's */
} drive;

/* string -> pointer, in insertion order of nothing (lookups only) */
typedef struct smap smap;
smap *smap_new(void);
void *smap_get(smap *m, const char *k);
void smap_set(smap *m, const char *k, void *v);   /* v replaces; the old one is the caller's */
void *smap_del(smap *m, const char *k);           /* the old value, or 0 */

/* u32 -> pointer */
typedef struct imap imap;
imap *imap_new(void);
void *imap_get(imap *m, u32 k);
void imap_set(imap *m, u32 k, void *v);
void *imap_del(imap *m, u32 k);
void imap_clear(imap *m, void (*f)(void *));

/* ---- the heap (heap.js) ---- */
typedef struct heap heap;
heap *heap_new(void);
void heap_reset(heap *h, u32 base, u32 end);
u32 heap_alloc(heap *h, i32 n);
void heap_free(heap *h, u32 p);
u32 heap_realloc(heap *h, u8 *mem, u32 p, i32 n);
void heap_set_high(heap *h, int on);
u32 heap_largest(heap *h);
i32 heap_release(heap *h, i32 g);
i32 heap_used(heap *h), heap_peak(heap *h), heap_gen(heap *h), heap_released(heap *h);
void heap_set_gen(heap *h, i32 g);
u32 heap_base(heap *h), heap_end(heap *h);

/* ---- the machine ---- */
typedef struct fdent fdent;
typedef struct machine machine;

/* ---- the MMU (mmu.c: libjs/mmu.js's Mmu) ---- */
typedef struct mmu {
  machine *m;
  /* translations by virtual page: read, write, fetch -- the physical
   * page or -1 -- and nx, known not runnable; log, the pages filled */
  i32 *rd, *wr, *ex; u8 *nx; i32 *log; u32 nlog;
  i32 cwins[12]; int ncwins;           /* lo, len, off of each code window kept */
  i32 misses, faults;
  int on, general;
  i32 ptbr, ksp, iopm, faddr, finfo, fpc;
  i32 s_lo, s_len, s_off, c_lo, c_len, c_off;   /* the stack and code windows */
  i32 pte_at;                          /* probe's: the word the entry was read from */
  i32 fpu_v;
  /* a block opcode a fault stopped part way (block) */
  struct { int has; i32 op, pc, sp, ptbr, d, s, n, k; } part;
  void *jb;                            /* where a fault goes (a c4_jmp_buf), or 0 */
} mmu;

struct machine {
  /* the arena: size is a power of two, and wmask makes every word index
   * one inside it (libjs's typed arrays read 0 past the end instead) */
  u8 *u8; i32 *w; u32 size, wmask, stack_top;
  /* registers */
  i32 pc, sp, bp, a, mode;
  i64 cycle, skipped;
  i32 trap_handler, cycle_interval, cycle_handler, trap_pc, trap_restores_interval;
  int b_fired; i64 b_next_irq;
  i32 sigh[64];                    /* signal handlers, 0..63 */
  i32 pending_signal;
  i64 missed_traps;
  i32 tt, tp, hnd, jmode, jinterval;
  i32 irq_hold;
  int no_fpu;
  i32 fpu_v;
  i32 rom_stores;
  heap *heap;
  /* symbols for STRC: code address and name, sorted */
  struct sym { u32 addr; char *name; } *syms; int nsyms;

  /* ---- the devices (devices.js Devices) ---- */
  int halted; i32 status;
  int rom_reported;
  i32 heap_base_reg, heap_end_reg;
  u32 img_code, img_data, img_bss, img_end;
  u32 stack_base, stack_top_reg; int stack_guard;
  /* the keyboard: bytes waiting, and end of input */
  u8 *rx; u32 rx_head, rx_tail, rx_cap;
  int rx_eof, rx_starved, raw_kbd;
  i64 sleep_ms; i64 nap_ms;
  u32 cycles_per_ms;
  i64 t0;
  i32 pit_ms, pit_armed; i64 pit_next;
  i32 mbox_irq;
  /* drives */
  drive **drives; int ndrives; int drive;
  /* fds */
  imap *fds; i32 next_fd;
  i32 disk_flags, disk_fd, disk_addr, disk_result, opname_result;
  i32 uart_tx_addr, uart_tx_result;
  i64 uart_activity, disk_activity;
  /* the console tap */
  int tap_on; u8 *tap_buf; u32 tap_len, tap_cap; i64 tap_dropped; i32 tap_addr, tap_result;
  u8 *tap_in; u32 tap_in_len, tap_in_pos; int tap_in_on;
  /* swap */
  imap *swap_pages; u32 swap_slots; u32 swap_addr; i32 swap_result;
  int reset_requested;
  /* the console sink */
  void (*on_byte)(machine *, int b);
  /* the MMU when one is fitted (mmu_new); vm while the user loop runs,
   * which makes the loads and stores translate. The user window (c4m.js
   * kpa): where user addresses start, 0 for none, and udir the kernel's
   * map of them. dma_ctx: the user task's MMU while its load or store is
   * at a device, which checks the addresses it is given (arena.js dmaCtx) */
  mmu *mmu; int vm; u32 uwin, udir; mmu *dma_ctx;
};

machine *machine_new(u32 arena_bytes, drive **drives, int ndrives, double mhz);
/* the image, its arguments: the trampoline set up, the heap laid out (boot.js) */
int machine_boot(machine *m, const u8 *image, u32 len, int argc, char **argv);
int machine_run(machine *m, i64 max_cycles);
/* how fast the interpreter runs here: c4 instructions a millisecond,
 * timed for ms milliseconds on a loop of a scratch machine with an arena
 * of bytes (c4pc -hz auto) */
u32 machine_kips(int ms, u32 bytes);
void machine_emit(machine *m, const u8 *b, u32 n);
void machine_emits(machine *m, const char *s);
void machine_stacktrace(machine *m);
i64 machine_sim_ms(machine *m);
void kbd_push(machine *m, int b);
int kbd_pending(machine *m);

/* devices (dev.c) */
i32 dev_read32(machine *m, u32 addr);
void dev_write32(machine *m, u32 addr, i32 val);
i32 dev_disk_open(machine *m, u32 name_addr);
i32 dev_disk_read(machine *m, i32 len);
int dev_opcode_by_name(const char *name);
void dev_con_bytes(machine *m, const u8 *b, u32 n);
/* the bytes at addr up to a NUL, at most max, as a C string (static buffer
 * of the caller's) */
u32 dma_cstring(machine *m, u32 addr, char *out, u32 max);

/* the MMU (mmu.c) */
mmu *mmu_new(machine *m);              /* fitted, off until the guest turns it on */
int mmu_run(mmu *u, i64 max_cycles);
i32 mmu_read32(mmu *u, i32 x);         /* its registers, kernel mode's */
void mmu_write32(mmu *u, i32 x, i32 v);
/* user mode's loads and stores for sys(), the FPU, OPCD: a fault goes to
 * the innermost catch (u->jb) */
i32 mmu_ld32(mmu *u, i32 x);
i32 mmu_ld8(mmu *u, i32 x);
void mmu_st32(mmu *u, i32 x, i32 v);
void mmu_st8(mmu *u, i32 x, i32 v);
int mmu_block(mmu *u, i32 op);         /* MSET, MCMP, MCPY in user mode */
i32 mmu_peek(mmu *u, i32 pc);
i64 mmu_dma_check(mmu *u, u32 addr, i32 len, int write, int pin);
/* the kernel's map of user addresses (arena.js pteWord, kernelPte, kernelPhys) */
i64 pte_word(machine *m, u32 dir, u32 va);
i32 kernel_pte(machine *m, u32 udir, u32 x);
i64 kernel_phys(machine *m, u32 udir, u32 x, int pin);

/* the engine's, for mmu.c */
i32 m_ld32(machine *m, i32 x);
void m_st32(machine *m, i32 x, i32 v);
int eng_trap(machine *m, i32 type, i32 param, i32 handler);
int eng_halt(machine *m, i32 status);
int eng_sys(machine *m, i32 op);
i64 eng_boundary(machine *m, i64 cycle, i64 next_irq);
int eng_need_checks(machine *m);
void eng_opv_trap(machine *m, i32 op);
int eng_run_plain(machine *m, i64 max_cycles);
void eng_rom_store(machine *m, i32 x, i32 v);
i32 eng_bus_rd32(machine *m, u32 a);
i32 eng_bus_rd8s(machine *m, u32 a);
void eng_bus_wr32(machine *m, u32 a, i32 v);
void eng_bus_wr8(machine *m, u32 a, i32 v);
static inline int has_operand(i32 i) {
  return i <= OP_ADJ || i == OP_JSRI || i == OP_JSRS || (i >= OP_LDL && i <= OP_IMMP) || i == OP_STL;
}
static inline int is_plain_op(i32 i) {
  return (i >= OP_LEV && i <= OP_MOD) || i == OP__JMP || i == OP__ADJ || i == OP_C4CY ||
         i == OP_JMPA || i == OP_LIP || i == OP_ADDL || i == OP_POPA || (i >= OP_LH && i <= OP_LCU);
}
/* the ROM a guest never stores to: below 0x20, and the opcode names */
static inline int is_rom(i32 x, i32 n) {
  i64 lo = x, hi = lo + n;
  return (lo < 0x20 && hi > 0) || (lo < OPNAMES_ROM + (i64)(INS_SIZE * 5) && hi > OPNAMES_ROM);
}

/* the formatter (fmt.c): PRTF's bytes for fmt and its words */
typedef struct obuf { u8 *b; u32 n, cap; } obuf;
void ob_put(obuf *o, int c);
void ob_puts(obuf *o, const char *s);
void fmt_format(obuf *out, const u8 *fmt, u32 fmtlen, const i32 *args, int nargs,
                u32 (*str)(void *ctx, u32 addr, const u8 **p), void *ctx);

/* the FPU (fpu.c) */
i32 fpu_float(i32 op, i32 a, i32 b);
int fpu_double_plain(i32 *w, i32 op, i32 a, i32 ib, i32 ir, i32 *res);
int fpu_op(machine *m, u32 sp, i32 *res);   /* 0: no such sub-op */
i32 flt_op(machine *m, u32 sp);
float f32_of(i32 bits);
i32 bits_of_f32(float f);

/* the drives (drive.c) */
drive *drive_new(int writable);
void drive_put(drive *d, const char *name, blob *b);       /* takes the reference */
/* a ustar archive's files onto a drive, by their paths (the PC's ramdisk) */
int drive_add_tar(drive *d, u8 *tar, u32 len);

/* ---- what the platform gives ---- */
i64 plat_now_ms(void);                 /* a real millisecond clock */
void plat_fatal(const char *msg);      /* never returns */

/* the little of libc the engine uses: the PC build has its own (pc/libc.c) */
void *xmalloc(size_t n);               /* zeroed; fatal when there is none */
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
void xfree(void *p);
#endif
