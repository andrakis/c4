/* ap.c -- the PC's other processors (M7), for the parallel engine (par.c).
 *
 * Every processor has a local APIC; the boot one sends the others INIT and
 * two Startup IPIs, to all but itself at once, and each starts in real
 * mode at the trampoline (aptramp.S, copied to 0x7000), comes up to the
 * boot processor's mode and paging, and waits in ap_entry, in hlt, for the
 * engine to give it a function (plat_cpu_start). How many there are is
 * how many arrive: QEMU's -smp N, a VM's processors, a PC's cores.
 *
 * A processor waits in hlt (plat_wait): kicked (plat_kick), it is sent an
 * IPI on vector F0h, which ends the hlt; a wait with an end gets the IPI
 * from the boot processor's 1 kHz tick when the end comes (ap_tick). The
 * boot processor itself is woken by its ticks and its own interrupts, and
 * by the same IPI. */
#include "../c4pc.h"
#include "../clib.h"

#define MAXCPU 32
#define WAKE_VEC 0xF0
#define SPURIOUS_VEC 0xFF
#define STACK_SIZE (256 * 1024)
#define TRAMP 0x7000

extern u8 ap_tramp_start[], ap_tramp_end[], ap_tramp_count[], ap_tramp_stacks[], ap_tramp_stack_size[];
extern u8 ap_tramp_cr0[], ap_tramp_cr3[], ap_tramp_cr4[], ap_tramp_entry[];
void pc_log(const char *s);
void pc_logn(i64 v);
void cpu_init_ap(void);

static volatile u32 *lapic;
static uintptr_t stk_lo, stk_hi;             /* the other processors' stacks */

/* this processor's number: 0 the boot one, k the one on stack k - 1
 * (aptramp.S: processor n, from 0, has the stack below stacks + (n+1) x size) */
int plat_self(void) {
  uintptr_t sp = (uintptr_t)__builtin_frame_address(0);
  return sp >= stk_lo && sp < stk_hi ? (int)((sp - stk_lo) / STACK_SIZE) + 1 : 0;
}

/* the device lock (engine.c eng_lock_fn, set by hv.c): one processor at a
 * time, taken again by the one that holds it as often as it likes.
 * No TSC on its way in or out: VirtualBox emulates rdtsc (its log's
 * "VirtTSCEmulated"), a VM exit each, and VM 0 under --hv, taking this for
 * every device access, ran at a third of its speed there (2026-11-26). */
static volatile int dl_flag, dl_owner = -1;
static int dl_depth[MAXCPU];
/* its account since the last report: the waits, in TSC ticks, timed only
 * when another processor had it; who held it, sampled by the boot
 * processor's 1 kHz tick (pc_lock_sample) */
static uint64_t dl_wait[MAXCPU], dl_since;
static volatile u32 dl_held[MAXCPU], dl_ticks;
void pc_lock(int on) {
  int me = plat_self();
  if (on > 0) {
    if (__atomic_load_n(&dl_owner, __ATOMIC_ACQUIRE) == me) { dl_depth[me]++; return; }
    if (__atomic_exchange_n(&dl_flag, 1, __ATOMIC_ACQUIRE)) {
      uint64_t t0 = __builtin_ia32_rdtsc();
      do while (__atomic_load_n(&dl_flag, __ATOMIC_RELAXED)) __builtin_ia32_pause();
      while (__atomic_exchange_n(&dl_flag, 1, __ATOMIC_ACQUIRE));
      dl_wait[me] += __builtin_ia32_rdtsc() - t0;
    }
    __atomic_store_n(&dl_owner, me, __ATOMIC_RELAXED);
    dl_depth[me] = 1;
  } else if (__atomic_load_n(&dl_owner, __ATOMIC_ACQUIRE) == me && dl_depth[me] > 0 && --dl_depth[me] == 0) {
    __atomic_store_n(&dl_owner, -1, __ATOMIC_RELAXED);
    __atomic_store_n(&dl_flag, 0, __ATOMIC_RELEASE);
  }
}
/* the boot processor's tick (pc.c's isr): who holds the lock now */
void pc_lock_sample(void) {
  int o = __atomic_load_n(&dl_owner, __ATOMIC_RELAXED);
  if (o >= 0 && o < MAXCPU) dl_held[o]++;
  dl_ticks++;
}
/* "processor 0 held it 23%, waited 1%; ..." since the last, for each that
 * took it; the account starts again */
int pc_lock_report(char *out, int max) {
  uint64_t now = __builtin_ia32_rdtsc(), span = dl_since ? now - dl_since : 0;
  u32 ticks = dl_ticks;
  int n = 0;
  dl_since = now; dl_ticks = 0;
  if (!span) return 0;
  for (int k = 0; k < MAXCPU; k++) {
    uint64_t h = dl_held[k], w = dl_wait[k];
    dl_held[k] = 0; dl_wait[k] = 0;
    if (!h && !w) continue;
    char t[96]; int j = 0;
    const char *a = n ? "; processor " : "processor ";
    while (*a) t[j++] = *a++;
    int v[3] = { k, ticks ? (int)(h * 1000 / ticks) : 0, (int)(w * 1000 / span) };
    for (int q = 0; q < 3; q++) {
      if (q == 1) { a = " held it "; while (*a) t[j++] = *a++; }
      if (q == 2) { a = ", waited "; while (*a) t[j++] = *a++; }
      int x = v[q], d = q ? 1 : 0;           /* tenths of a percent */
      char b[12]; int bn = 0;
      do b[bn++] = (char)('0' + x % 10); while ((x /= 10) || bn <= d);
      while (bn) { t[j++] = b[--bn]; if (d && bn == 1) t[j++] = '.'; }
      if (q) t[j++] = '%';
    }
    for (int q = 0; q < j && n < max - 1; q++) out[n++] = t[q];
  }
  out[n] = 0;
  return n;
}
int pc_lock_depth(void) { int me = plat_self(); return __atomic_load_n(&dl_owner, __ATOMIC_ACQUIRE) == me ? dl_depth[me] : 0; }
static int ncpu = 1;
static volatile int apic_id[MAXCPU];
static volatile int up[MAXCPU];
static void (*volatile fn_of[MAXCPU])(int);
static volatile int sleeping[MAXCPU];
static volatile i32 deadline[MAXCPU];        /* plat_now_ms's, 0: none */
static double idle_us[MAXCPU];               /* each one's time waiting (HOST's pidleN) */
/* processor k's time waiting, ms (0 is pc.c's own) */
i64 plat_idle_ms(int k) { return k > 0 && k < MAXCPU ? (i64)(idle_us[k] / 1000) : 0; }

static inline u32 lr(u32 off) { return lapic[off >> 2]; }
static inline void lw(u32 off, u32 v) { lapic[off >> 2] = v; }
static inline void cpuid(u32 l, u32 *a, u32 *b, u32 *c, u32 *d) { __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(l), "c"(0)); }
static inline uint64_t rdmsr(u32 r) { u32 lo, hi; __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(r)); return (uint64_t)hi << 32 | lo; }
static inline void wrmsr(u32 r, uint64_t v) { __asm__ volatile("wrmsr" : : "c"(r), "a"((u32)v), "d"((u32)(v >> 32))); }
/* The boot processor's PAT, which screen.c set to write-combine the
 * framebuffer (its entry 1) before the others start: each takes it as it
 * starts. The page tables are shared, the PAT is a processor's own, and
 * without it the others wrote the framebuffer through (the reset PAT's
 * entry 1, WT): a guest's frame put on the screen from its own processor
 * took 90 ms, and two processors saw one page as two memory types (Intel
 * SDM vol. 3, 11.12.4) */
static int has_pat; static uint64_t bsp_pat;

void lapic_eoi(void) { if (lapic) lw(0xB0, 0); }

/* an IPI: interrupts held while the ICR's two words are written, since
 * the tick sends them too */
static void send(u32 dest, u32 low) {
  uintptr_t fl;
  __asm__ volatile("pushf; pop %0; cli" : "=r"(fl) : : "memory");
  while (lr(0x300) & (1u << 12)) __builtin_ia32_pause();       /* the last one delivered */
  lw(0x310, dest << 24);
  lw(0x300, low);
  if (fl & 0x200) __asm__ volatile("sti");
}

static void spin_ms(i64 ms) { i64 t = plat_now_ms(); while (plat_now_ms() - t < ms) __builtin_ia32_pause(); }
static void spin_us(double us) { double t = plat_now_us(); while (plat_now_us() - t < us) __builtin_ia32_pause(); }

/* where each other processor lives: up, then waiting for a function */
static void ap_idle(int k) {
  for (;;) {
    void (*f)(int) = fn_of[k];
    if (f) { fn_of[k] = 0; f(k); continue; }
    sleeping[k] = 1;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    double t0 = plat_now_us();
    if (!fn_of[k]) __asm__ volatile("sti; hlt; cli");
    idle_us[k] += plat_now_us() - t0;
    sleeping[k] = 0;
  }
}
void ap_entry(int n) {
  cpu_init_ap();                       /* the kernel's GDT and IDT, the x87 (pc.c) */
  if (has_pat) { __asm__ volatile("wbinvd" : : : "memory"); wrmsr(0x277, bsp_pat); __asm__ volatile("wbinvd" : : : "memory"); }
  lw(0xF0, 0x100 | SPURIOUS_VEC);      /* the local APIC on */
  int k = n + 1;
  if (k >= MAXCPU) for (;;) __asm__ volatile("cli; hlt");
  apic_id[k] = (int)(lr(0x20) >> 24);
  __atomic_store_n(&up[k], 1, __ATOMIC_SEQ_CST);
  ap_idle(k);
}

/* the other processors started: how many processors, this one counted.
 * want: at most that many. why: a word for the console when there are none */
int ap_start(int want, const char **why) {
  u32 a, b, c, d;
  cpuid(1, &a, &b, &c, &d);
  if (!(d & (1u << 9))) { *why = "no local APIC"; return 1; }
  uint64_t base = rdmsr(0x1B);
  if (!(base & (1u << 11))) { *why = "the local APIC is off"; return 1; }
  lapic = (volatile u32 *)(uintptr_t)(base & 0xFFFFF000u);
  { extern void screen_uncached(u32 addr); screen_uncached((u32)(uintptr_t)lapic); }
  lw(0xF0, 0x100 | SPURIOUS_VEC);
  apic_id[0] = (int)(lr(0x20) >> 24);
  if (want <= 1) { *why = "one asked for"; return 1; }
  if (d & (1u << 16)) { has_pat = 1; bsp_pat = rdmsr(0x277); }
  /* the trampoline, and its words */
  u32 len = (u32)(ap_tramp_end - ap_tramp_start);
  memcpy((void *)(uintptr_t)TRAMP, ap_tramp_start, len);
#define TW(sym) (*(volatile u32 *)(uintptr_t)(TRAMP + (u32)((sym) - ap_tramp_start)))
  u8 *stacks = xmalloc((size_t)STACK_SIZE * MAXCPU + 16);
  TW(ap_tramp_count) = 0;
  TW(ap_tramp_stacks) = (u32)(((uintptr_t)stacks + 15) & ~(uintptr_t)15);
  stk_lo = ((uintptr_t)stacks + 15) & ~(uintptr_t)15; stk_hi = stk_lo + (uintptr_t)STACK_SIZE * MAXCPU;
  TW(ap_tramp_stack_size) = STACK_SIZE;
  uintptr_t cr0, cr3, cr4;
  __asm__ volatile("mov %%cr0, %0" : "=r"(cr0)); __asm__ volatile("mov %%cr3, %0" : "=r"(cr3)); __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
  TW(ap_tramp_cr0) = (u32)cr0; TW(ap_tramp_cr3) = (u32)cr3; TW(ap_tramp_cr4) = (u32)cr4;
  *(volatile uint64_t *)(uintptr_t)(TRAMP + (u32)(ap_tramp_entry - ap_tramp_start)) = (uint64_t)(uintptr_t)ap_entry;
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  /* INIT, then two Startup IPIs at vector 07h (0x7000), to all but this one */
  send(0, 0x000C4500);
  spin_ms(10);
  for (int i = 0; i < 2; i++) { send(0, 0x000C4600 | (TRAMP >> 12)); spin_us(200); }
  /* until they stop arriving: 100 ms after the last, at most a second */
  i64 t0 = plat_now_ms(), last = t0;
  u32 seen = 0;
  while (plat_now_ms() - last < 100 && plat_now_ms() - t0 < 1000) {
    u32 n = TW(ap_tramp_count);
    if (n != seen) { seen = n; last = plat_now_ms(); }
    __builtin_ia32_pause();
  }
  int got = 1;
  for (int k = 1; k < MAXCPU && k <= (int)seen; k++) if (__atomic_load_n(&up[k], __ATOMIC_SEQ_CST)) got = k + 1; else break;
  ncpu = got < want ? got : want;
  if (ncpu == 1) *why = "no other processor answered";
  return ncpu;
}

/* ---- the platform's side of the parallel engine (c4pc.h) ---- */
int plat_cpus(void) { return ncpu; }
int plat_cpu_start(int k, void (*fn)(int)) {
  if (k < 1 || k >= ncpu) return 0;
  fn_of[k] = fn;
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  send((u32)apic_id[k], 0x4000 | WAKE_VEC);
  return 1;
}
/* k: the waiting processor's own number (par.c knows it) */
void plat_wait(int k, volatile int *kick, int seen, int ms) {
  int me = k >= 0 && k < MAXCPU ? k : 0;
  i64 end = ms < 0 ? 0 : plat_now_ms() + ms;
  double t0 = me ? plat_now_us() : 0;
  uintptr_t fl;
  __asm__ volatile("pushf; pop %0" : "=r"(fl));
  for (;;) {
    __asm__ volatile("cli");
    sleeping[me] = 1;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (__atomic_load_n(kick, __ATOMIC_SEQ_CST) != seen || (ms >= 0 && plat_now_ms() >= end)) break;
    if (me && ms >= 0) deadline[me] = (i32)end;      /* the tick's IPI ends it (ap_tick) */
    __asm__ volatile("sti; hlt; cli");
    deadline[me] = 0;
  }
  sleeping[me] = 0;
  if (me) idle_us[me] += plat_now_us() - t0;
  if (fl & 0x200) __asm__ volatile("sti");
}
void plat_kick(int k, volatile int *kick) {
  __atomic_add_fetch(kick, 1, __ATOMIC_SEQ_CST);
  if (k < 0 || k >= ncpu || !lapic) return;
  /* only one that waits: an IPI to a running processor is a VM exit for nothing */
  if (__atomic_load_n(&sleeping[k], __ATOMIC_SEQ_CST)) send((u32)apic_id[k], 0x4000 | WAKE_VEC);
}
/* the boot processor's tick: a processor whose wait has come to its end, woken */
void ap_tick(void) {
  if (ncpu < 2) return;
  i64 now = -1;
  for (int k = 1; k < ncpu; k++) {
    i32 d = deadline[k];
    if (!d) continue;
    if (now < 0) now = plat_now_ms();
    if (now >= d) { deadline[k] = 0; send((u32)apic_id[k], 0x4000 | WAKE_VEC); }
  }
}
