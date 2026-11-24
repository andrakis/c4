/* par.c -- several processors for real: one of the host's CPUs (a thread
 * in c4pc-host, a processor of the PC's own on a PC) for each of the
 * machine's (libjs/par.js and ap.js, docs/c4ix-smp.md S2). smp.c is the
 * simulated engine; the two run the same opcodes.
 *
 * Every CPU's machine runs on the one arena, flat out, while its state is
 * RUN. CAS, XCHG and FADD are the host's own atomic instructions on the
 * guest's word; CWAI parks until the word moves, a CWAK, an IPI or the
 * machine's end, looking again every 10 ms, as ap.js does.
 *
 * CPU 0 owns what is not memory: the devices and the console. Another CPU
 * that reaches them -- OPEN, READ, PRTF and the rest, a device register,
 * a page put out to swap, a message -- asks CPU 0 (ask), which runs the
 * same code on the asking CPU's own machine while it waits (serving): its
 * registers are the asker's, its device fields CPU 0's (machine.dv). CPU 0
 * answers between slices of 16K cycles, and while it waits, as soon as it
 * is woken. The heap every CPU uses itself, under a lock (heap.c).
 *
 * The platform gives four things (c4pc.h): how many CPUs, a CPU started
 * on a function, a wait for a word to move (a futex on Linux; on a PC,
 * hlt until an interrupt), and a kick that ends one. */
#include "c4pc.h"
#include "clib.h"

#define MAXCPU 32
enum { S_OFF, S_RUN, S_WAIT, S_HALT };
enum { R_IDLE, R_ASK, R_DONE };

/* Posted writes (par.js POSTED): the device registers whose writes need
 * no answer and read no guest memory -- the sound card's OPL, WAIT, its
 * PCM settings, STOP, SETVOL, the speaker and RESET (snd.c). Another CPU
 * queues them for CPU 0 and carries on, where every other device access
 * asks and waits; CPU 0 applies a CPU's queue, in order, before it
 * answers that CPU's next request and between its own slices. A game's
 * music is thousands of OPL writes a second, and in libjs a round trip
 * each made a game on CPU 1 slower than on CPU 0 (63.0 s against 53.5). */
#define PQLEN 1024
static int posted(u32 a) {
  switch (a) {
    case 0x484: case 0x488: case 0x490: case 0x494: case 0x498: case 0x49c:
    case 0x4a0: case 0x4a8: case 0x4ac: case 0x4b4: case 0x4b8: case 0x4cc: return 1;
    /* the block devices' select, LBA, count and address: the command after
     * them is asked, and CPU 0 applies these first (service). Each was a
     * round trip: install on CPU 1 took 5 minutes, on CPU 0 91 s (2026-11-24) */
    case 0x544: case 0x550: case 0x554: case 0x558: return 1;
  }
  return 0;
}

typedef struct pcpu {
  volatile int state, ipi, req, kick, woke;
  volatile word entry, stack, wait_addr, wait_val, pc; volatile i32 cycles;
  void (*fn)(machine *, void *); void *arg;
  /* its posted writes: CPU 0's head, the next to apply; its own tail */
  volatile u32 pq_head, pq_tail; int pq_new;
  u32 pq_addr[PQLEN]; i32 pq_val[PQLEN];
} pcpu;

struct par {
  smp *s;
  volatile int halt, door;
  int seen_door;
  i64 idle_cycles;
  i64 nap_from, nap_t0;
  i64 stuck;
  pcpu c[MAXCPU];
  /* -s's account (par_stats): requests and their round trips, parks and
   * how they ended, CPU 0's waits */
  volatile u32 n_ask, n_park, n_park_tmo, n_park_word, n_park_woke, n_park_ipi;
  volatile u32 ask_us, park_us, idle0_us;          /* microseconds, mod 2^32 */
};

#define LD(x) __atomic_load_n(&(x), __ATOMIC_SEQ_CST)
#define ST(x, v) __atomic_store_n(&(x), (v), __ATOMIC_SEQ_CST)

static void ring0(par *p) { __atomic_add_fetch(&p->door, 1, __ATOMIC_SEQ_CST); plat_kick(0, &p->c[0].kick); }
static void kick(par *p, int k) { plat_kick(k, &p->c[k].kick); }

/* ---- the machine's time ----
 * CPU 0's cycles and what passed while it was parked; the others read it
 * as CPU 0 last published it (machine_sim_ms). Napping, CPU 0's clock
 * takes the nap whole as it begins, and the others see its start and the
 * real time since (par.js timeMs) */
i64 par_clock(smp *s) { return s->cpu[0]->cycle + s->par->idle_cycles; }
static i32 time_ms(par *p) {
  machine *m0 = p->s->cpu[0];
  i64 t = machine_sim_ms(m0);
  if (p->nap_t0) { i64 u = p->nap_from + (plat_now_ms() - p->nap_t0); if (u < t) t = u; }
  return (i32)t;
}

/* ---- asking CPU 0 ---- */
static void ask(machine *m, void (*fn)(machine *, void *), void *arg) {
  par *p = m->smp->par; pcpu *me = &p->c[m->cpu_id];
  me->fn = fn; me->arg = arg;
  ST(me->req, R_ASK);
  ring0(p);
  for (;;) {
    int seen = LD(me->kick);
    if (LD(me->req) != R_ASK) break;
    if (LD(p->halt)) { m->dv->halted = 1; return; }
    plat_wait(m->cpu_id, &me->kick, seen, 100);
  }
  ST(me->req, R_IDLE);
}
static void count(volatile u32 *c) { __atomic_add_fetch(c, 1, __ATOMIC_RELAXED); }
static void addf(volatile u32 *d, double v) { __atomic_add_fetch(d, (u32)v, __ATOMIC_RELAXED); }
static void ask_counted(machine *m, void (*fn)(machine *, void *), void *arg) {
  par *p = m->smp->par;
  double t = plat_now_us();
  ask(m, fn, arg);
  count(&p->n_ask); addf(&p->ask_us, plat_now_us() - t);
}
#define ask ask_counted
/* -s: what was asked, by what: a register's address (its word in the
 * first 2 KB of page 0), an opcode (+ 512), or 1023 for anything else */
static volatile u32 asked_what[1024];
static void asked(u32 what) { __atomic_add_fetch(&asked_what[what < 1024 ? what : 1023], 1, __ATOMIC_RELAXED); }

/* CPU k's posted writes, in the order it made them */
static void apply_posted(par *p, int k) {
  pcpu *c = &p->c[k];
  u32 h = c->pq_head, t = LD(c->pq_tail);
  if (h == t) return;
  machine *mk = p->s->cpu[k];
  mk->serving = 1;
  for (; h != t; h++) {
    u32 a = c->pq_addr[h & (PQLEN - 1)]; i32 v = c->pq_val[h & (PQLEN - 1)];
    if (a & 0x10000) eng_bus_wr8(mk, a & 0xffff, v); else eng_bus_wr32(mk, a, v);
  }
  mk->serving = 0;
  ST(c->pq_head, h);
  kick(p, k);                           /* a CPU waiting for room */
}
/* a write queued for CPU 0; a full queue waits for room */
static void post(machine *m, u32 a, i32 v, int w8) {
  par *p = m->smp->par; pcpu *me = &p->c[m->cpu_id];
  u32 t = me->pq_tail;
  for (;;) {
    int seen = LD(me->kick);
    if (t - LD(me->pq_head) < PQLEN || LD(p->halt)) break;
    ring0(p);
    plat_wait(m->cpu_id, &me->kick, seen, 5);
  }
  me->pq_addr[t & (PQLEN - 1)] = a | (w8 ? 0x10000u : 0);
  me->pq_val[t & (PQLEN - 1)] = v;
  ST(me->pq_tail, t + 1);
  me->pq_new = 1;
}
/* CPU 0 told there are posted writes to apply, in case it sleeps */
static void flush_posted(par *p, machine *m) {
  pcpu *me = &p->c[m->cpu_id];
  if (me->pq_new) { me->pq_new = 0; ring0(p); }
}

/* every request waiting, answered on the asker's machine, after what it
 * posted before it */
static void service(par *p) {
  smp *s = p->s;
  p->seen_door = LD(p->door);
  ST(s->time_ms, time_ms(p));
  for (int k = 1; k < s->n; k++) {
    pcpu *c = &p->c[k];
    apply_posted(p, k);
    if (LD(c->req) != R_ASK) continue;
    machine *mk = s->cpu[k];
    mk->serving = 1;
    c->fn(mk, c->arg);
    mk->serving = 0;
    ST(c->req, R_DONE);
    kick(p, k);
  }
}

/* an opcode of CPU 0's: the devices', the console's */
struct sys_q { word op; int r; };
static void sys_fn(machine *m, void *a) { struct sys_q *q = a; q->r = eng_sys(m, q->op); }
/* a device register; a user task's load or store there has its space
 * checked by the device (mmu.c dma_ctx), and a fault comes back here to
 * be raised on the asker's own CPU */
struct bus_q { int kind; u32 addr; i32 val; word r; int fault; };
static void bus_fn(machine *m, void *a) {
  struct bus_q *q = a;
  mmu *u = m->dma_ctx;
  void *outer = u ? u->jb : 0;
  c4_jmp_buf jb;
  if (u) {
    int lk = eng_lock_mark();
    if (c4_setjmp(jb)) { eng_lock_back(lk); u->jb = outer; q->fault = 1; return; }
    u->jb = &jb;
  }
  if (q->kind == 0) q->r = eng_bus_rd32(m, q->addr);
  else if (q->kind == 1) eng_bus_wr32(m, q->addr, q->val);
  else eng_bus_wr8(m, q->addr, q->val);
  if (u) u->jb = outer;
}
struct emit_q { const u8 *b; u32 n; };
static void emit_fn(machine *m, void *a) { struct emit_q *q = a; machine_emit(m, q->b, q->n); }

/* the opcodes whose work is CPU 0's (ap.js ASK), and those guarded in
 * protected mode, where a protected task's takes the local trap */
static int is_ask(word op) {
  return op == OP_OPEN || op == OP_READ || op == OP_CLOS || op == OP_PRTF || op == OP_PUTC ||
         op == OP_PUTS || op == OP_STRC || op == OP__OPC;
}
static int guarded(word op) {
  return op == OP_OPEN || op == OP_READ || op == OP_CLOS || op == OP_PRTF || op == OP_MALC || op == OP_FREE ||
         op == OP_RALC || op == OP_PUTC || op == OP_PUTS || op == OP_EXIT || op == OP_USLP || op == OP_INFO;
}

/* sys() on a CPU other than 0: a STOP, or -1 for sys's own way */
int par_sys(machine *m, word op) {
  if (m->mode && guarded(op)) return -1;
  if (is_ask(op)) {
    struct sys_q q = { op, STOP_BUDGET };
    asked(512 + ((u32)op & 511));
    ask(m, sys_fn, &q);
    return m->dv->halted ? STOP_HALT : q.r;
  }
  if (op == OP_USLP) {
    /* this CPU sleeps; the others carry on. An IPI ends it early */
    par *p = m->smp->par; pcpu *me = &p->c[m->cpu_id];
    flush_posted(p, m);
    word us = m_ldw(m, m->sp);
    i64 ms = us / 1000 > 1 ? us / 1000 : 1, t0 = plat_now_ms();
    for (;;) {
      int seen = LD(me->kick);
      i64 left = ms - (plat_now_ms() - t0);
      if (left <= 0 || LD(me->ipi) || LD(p->halt)) break;
      plat_wait(m->cpu_id, &me->kick, seen, (int)left);
    }
    m->a = 0;
    return STOP_BUDGET;
  }
  return -1;
}
word par_rd32(machine *m, u32 a) {
  /* The sound card's AHEAD (how far the music is written past what is
   * playing), worked out here: the cursor, and this CPU's own OPL and WAIT
   * writes CPU 0 has not applied yet (snd_ahead) -- the queue's head read
   * before and after, so that a write CPU 0 applied meanwhile is not
   * counted twice. Doom's music asks it in a loop, and on CPU 1 every ask
   * was a round trip to CPU 0: three million in a minute, a few frames a
   * second at 100% (2026-11-23) */
  /* no sound card: its registers read 0 on CPU 0 (engine.c), and here */
  if (a >= 0x480 && a < 0x500 && !m->smp->par->s->cpu[0]->snd) return 0;
  if (a == 0x48c) {
    par *p = m->smp->par; pcpu *me = &p->c[m->cpu_id]; machine *m0 = p->s->cpu[0];
    if (m0->snd) {
      u32 h1, h2; i32 r;
      do {
        h1 = LD(me->pq_head);
        r = snd_ahead(m0->snd, me->pq_addr, me->pq_val, h1, me->pq_tail, PQLEN - 1);
        h2 = LD(me->pq_head);
      } while (h1 != h2);
      return r;
    }
  }
  struct bus_q q = { 0, a, 0, 0, 0 };
  mmu *u = m->dma_ctx;                  /* the asking task's: a fault clears dma_ctx (mmu.c fault) */
  asked(a < 2048 ? a >> 2 : 1023);
  ask(m, bus_fn, &q);
  if (q.fault) mmu_rethrow(u);
  return q.r;
}
void par_wr(machine *m, u32 a, i32 v, int w8) {
  if (posted(a & ~3u)) { post(m, a, v, w8); return; }
  struct bus_q q = { w8 ? 2 : 1, a, v, 0, 0 };
  /* the asking task's space, kept: the device's fault cleared dma_ctx on
   * its way out (mmu.c fault), and rethrown through 0 it was a crash --
   * the display's RING from splash on another CPU, its pages not yet in */
  mmu *u = m->dma_ctx;
  asked(a < 2048 ? a >> 2 : 1023);
  ask(m, bus_fn, &q);
  if (q.fault) mmu_rethrow(u);
}
void par_emit(machine *m, const u8 *b, u32 n) { struct emit_q q = { b, n }; ask(m, emit_fn, &q); }

/* ---- the processor opcodes ---- */
static int state(par *p, int k) { return LD(p->c[k].state); }
static i32 start(par *p, word id, word entry, word stacktop) {
  if (id < 0 || id >= p->s->n || !entry || !stacktop) return 0;
  pcpu *c = &p->c[id];
  int was = LD(c->state);
  if (was != S_OFF && was != S_HALT) return 0;
  ST(c->entry, entry); ST(c->stack, stacktop);
  /* a stale IPI dropped here, before the CPU counts as running (par.js start) */
  ST(c->ipi, 0);
  if (!__atomic_compare_exchange_n(&c->state, &was, S_RUN, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) return 0;
  kick(p, id);
  return 1;
}
static i32 ipi(par *p, word id) {
  if (id < 0 || id >= p->s->n) return 0;
  int s = state(p, id);
  if (s != S_RUN && s != S_WAIT) return 0;
  ST(p->c[id].ipi, 1);
  if (id == 0) ring0(p); else kick(p, id);
  return 1;
}
int par_op(machine *m, word op) {
  par *p = m->smp->par; pcpu *me = &p->c[m->cpu_id];
  word sp = m->sp;
  /* an arena word the host's atomics can take */
#define ALIGNED(t) (((t) & (WB - 1)) == 0 && (t) >= 0x1000 && (uword)(t) <= m->size - WB)
  switch (op) {
    case OP_CPUI: m->a = m->cpu_id; return STOP_BUDGET;
    case OP_CPUN: m->a = m->smp->n; return STOP_BUDGET;
    case OP_CPUS: m->a = start(p, m_ldw(m, sp + 2 * WB), m_ldw(m, sp + WB), m_ldw(m, sp)); return STOP_BUDGET;
    case OP_CPUH:
      m->cpu_state = CPU_HALT;
      ST(me->state, S_HALT);
      ring0(p);
      return STOP_CPUHALT;
    case OP_CAS: {
      word t = m_ldw(m, sp + 2 * WB), e = m_ldw(m, sp + WB), v = m_ldw(m, sp);
      if (ALIGNED(t)) { word x = e; __atomic_compare_exchange_n(&m->w[(uword)t >> WSH], &x, v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); m->a = x; }
      else { word o = m_ldw(m, t); if (o == e) m_stw(m, t, v); m->a = o; }
      return STOP_BUDGET;
    }
    case OP_XCHG: {
      word t = m_ldw(m, sp + WB), v = m_ldw(m, sp);
      if (ALIGNED(t)) m->a = __atomic_exchange_n(&m->w[(uword)t >> WSH], v, __ATOMIC_SEQ_CST);
      else { word o = m_ldw(m, t); m_stw(m, t, v); m->a = o; }
      return STOP_BUDGET;
    }
    case OP_FADD: {
      word t = m_ldw(m, sp + WB), d = m_ldw(m, sp);
      if (ALIGNED(t)) m->a = (word)__atomic_fetch_add((uword *)&m->w[(uword)t >> WSH], (uword)d, __ATOMIC_SEQ_CST);
      else { word o = m_ldw(m, t); m_stw(m, t, (word)((uword)o + (uword)d)); m->a = o; }
      return STOP_BUDGET;
    }
    case OP_CWAI: {
      word t = m_ldw(m, sp + WB), val = m_ldw(m, sp);
      if (!t || !ALIGNED(t) || LD(m->w[(uword)t >> WSH]) != val) { m->a = 1; return STOP_BUDGET; }
      m->a = 0;
      m->wait_addr = t; m->wait_val = val;
      m->cpu_state = CPU_WAIT;
      ST(me->woke, 0);
      ST(me->wait_val, val);
      me->pc = m->pc; me->cycles = (i32)m->cycle;
      ST(me->wait_addr, t);
      ST(me->state, S_WAIT);
      return STOP_PARK;
    }
    case OP_CWAK: {
      /* every CPU parked on the word, up to n of them, told and woken */
      word t = m_ldw(m, sp + WB), n = m_ldw(m, sp); i32 woken = 0;
      if (t && ALIGNED(t))
        for (int k = 0; k < m->smp->n; k++) {
          if (k == m->cpu_id || LD(p->c[k].wait_addr) != t) continue;
          if (n > 0 && woken >= n) break;
          ST(p->c[k].woke, 1);
          if (k == 0) ring0(p); else kick(p, k);
          woken++;
        }
      m->a = woken;
      return STOP_BUDGET;
    }
    case OP_IPI: m->a = ipi(p, m_ldw(m, sp)); return STOP_BUDGET;
  }
#undef ALIGNED
  return STOP_BUDGET;
}

/* an IPI for m, or the machine stopping */
static void poll(par *p, machine *m) {
  pcpu *me = &p->c[m->cpu_id];
  if (LD(p->halt)) m->dv->halted = 1;
  if (LD(me->ipi)) { ST(me->ipi, 0); m->ipi_pending = 1; }
}

/* ---- a CPU other than 0 (ap.js main) ---- */
static par *the_par;
static void ap_main(int k) {
  par *p = the_par; smp *s = p->s;
  machine *m = s->cpu[k];
  pcpu *me = &p->c[k];
  for (;;) {
    /* off or halted: wait to be started (or for the machine to stop) */
    for (;;) {
      int seen = LD(me->kick);
      if (LD(p->halt)) return;
      if (LD(me->state) == S_RUN) break;
      plat_wait(k, &me->kick, seen, 1000);
    }
    machine *m0 = s->cpu[0];
    m->accel = m0->accel; m->osaccel = m0->osaccel; m->no_fpu = m0->no_fpu;
    m->gui = m0->gui; m->snd = m0->snd; m->heap = m0->heap;
    smp_cpu_reset(m);
    m->sp = LD(me->stack) - WB;
    if ((uword)m->sp <= m->size - WB) m->w[(uword)m->sp >> WSH] = CPU_HALT_ADDR;
    m->bp = m->sp; m->pc = LD(me->entry); m->a = 0;
    m->ipi_pending = 0;
    m->cpu_state = CPU_RUN;
    for (;;) {
      /* a slice, then an IPI or the machine's end looked for: not inside
       * the instruction loop */
      int r = machine_run(m, 65536);
      flush_posted(p, m);
      if (r != STOP_CPUHALT) poll(p, m);
      ST(me->cycles, (i32)m->cycle);
      if (LD(m->dv->halted) || LD(p->halt)) return;
      if (r == STOP_CPUHALT) {
        /* EXIT or CPUH: this processor, never the machine */
        ST(me->state, S_HALT);
        ring0(p);
        break;
      }
      if (r == STOP_PARK) {
        /* the word, a CWAK, an IPI or the end; CWAI's caller looks again */
        i64 t0 = plat_now_ms();
        double tu = plat_now_us();
        uword wi = (uword)m->wait_addr >> WSH;
        int tmo = 0;
        count(&p->n_park);
        for (;;) {
          int seen = LD(me->kick);
          if (LD(me->ipi)) { count(&p->n_park_ipi); break; }
          if (LD(me->woke)) { count(&p->n_park_woke); break; }
          if (LD(m->w[wi]) != m->wait_val) { count(&p->n_park_word); break; }
          if (m->ipi_pending || LD(p->halt) || plat_now_ms() - t0 >= 1000) break;
          double w0 = plat_now_us();
          plat_wait(k, &me->kick, seen, 10);
          if (plat_now_us() - w0 >= 9000 && !tmo) { tmo = 1; count(&p->n_park_tmo); }
        }
        addf(&p->park_us, plat_now_us() - tu);
        if (LD(p->halt)) return;
        m->cpu_state = CPU_RUN;
        ST(me->wait_addr, 0);
        ST(me->state, S_RUN);
        if (LD(me->ipi)) { ST(me->ipi, 0); m->ipi_pending = 1; }
      } else if (r == STOP_INPUT) {
        int seen = LD(me->kick);
        plat_wait(k, &me->kick, seen, 5);   /* a key may come; ask again soon */
      }
    }
  }
}

/* the CPUs other than 0 on CPUs of the host's: how many the machine has */
int par_start(smp *s) {
  par *p = xmalloc(sizeof *p);
  int n = s->n > MAXCPU ? MAXCPU : s->n;
  p->s = s;
  the_par = p;
  s->par = p;
  machine *m0 = s->cpu[0];
  ST(p->c[0].state, S_RUN);
  heap_set_locked(m0->heap, 1);
  for (int k = 1; k < n; k++) {
    machine *m = s->cpu[k];
    m->par_ap = 1;                       /* no device interrupts here: CPU 0 takes them */
    if (!plat_cpu_start(k, ap_main)) { n = k; break; }
  }
  s->n = n;
  ST(s->time_ms, time_ms(p));
  return n;
}

/* CPU 0's run: slices of 16K cycles, and between them the others
 * answered and its own IPIs taken (par.js ParHost run) */
int par_run0(smp *s, i64 max_cycles) {
  par *p = s->par; machine *m0 = s->cpu[0];
  i64 left = max_cycles;
  int r = STOP_BUDGET;
  s->running = 1;
  p->nap_t0 = 0;
  while (left > 0) {
    i64 before = m0->cycle;
    r = machine_run(m0, left < 16384 ? left : 16384);
    i64 ran = m0->cycle - before;
    left -= ran > 1 ? ran : 1;
    if (r == STOP_SLEEP) { p->nap_from = machine_sim_ms(m0) - m0->nap_ms; p->nap_t0 = plat_now_ms(); }
    ST(s->time_ms, time_ms(p));
    if (LD(p->door) != p->seen_door) service(p);
    else for (int k = 1; k < s->n; k++) apply_posted(p, k);
    poll(p, m0);
    if (r != STOP_BUDGET || m0->halted) break;
  }
  s->running = 0;
  return m0->halted ? STOP_HALT : r;
}

/* ---- CPU 0 waiting ---- */
static int ipi_for0(par *p) { return LD(p->c[0].ipi) != 0; }
/* some other CPU running, or about to: an IPI waits for it, or the word
 * it parked on has moved on (par.js othersRunning) */
static int others_running(par *p) {
  machine *m0 = p->s->cpu[0];
  for (int k = 1; k < p->s->n; k++) {
    int st = state(p, k);
    if (st == S_RUN || LD(p->c[k].ipi)) return 1;
    if (st == S_WAIT) {
      word wa = LD(p->c[k].wait_addr);
      if (wa && LD(m0->w[(uword)wa >> WSH]) != LD(p->c[k].wait_val)) return 1;
    }
  }
  return 0;
}
static int others_done(par *p) {
  for (int k = 1; k < p->s->n; k++) { int st = state(p, k); if (st == S_RUN || st == S_WAIT) return 0; }
  return 1;
}
static int all_parked(par *p) {
  machine *m0 = p->s->cpu[0];
  if (m0->cpu_state != CPU_WAIT && m0->cpu_state != CPU_HALT) return 0;
  int waiting = m0->cpu_state == CPU_WAIT;
  for (int k = 1; k < p->s->n; k++) {
    int st = state(p, k);
    if (st == S_RUN) return 0;
    if (st == S_WAIT) waiting = 1;
  }
  for (int k = 1; k < p->s->n; k++) if (LD(p->c[k].req) == R_ASK) return 0;
  return waiting;
}
/* CPU 0 napping while others run: real time, ended early by an IPI, the
 * others answered meanwhile; at most 10 ms. The ms it lasted */
static i64 sleep_sync(par *p, i64 ms) {
  i64 t0 = plat_now_ms(), end = ms < 10 ? ms : 10;
  for (;;) {
    int seen = LD(p->c[0].kick);
    i64 left = end - (plat_now_ms() - t0);
    if (left <= 0 || ipi_for0(p) || LD(p->halt)) break;
    if (LD(p->door) == p->seen_door) plat_wait(0, &p->c[0].kick, seen, (int)left);
    service(p);
  }
  return plat_now_ms() - t0;
}
/* CPU 0 parked or halted: until it should look again, at most ms */
static void park_sync(par *p, int ms) {
  machine *m0 = p->s->cpu[0];
  pcpu *c = &p->c[0];
  i64 t0 = plat_now_ms();
  double tu = plat_now_us();
#define WOKEN() (LD(c->woke) || LD(c->ipi) || LD(p->halt) || \
                 (m0->cpu_state == CPU_WAIT && LD(m0->w[(uword)m0->wait_addr >> WSH]) != m0->wait_val))
  int w = 0;
  for (;;) {
    int seen = LD(c->kick);
    service(p);
    if ((w = WOKEN())) break;
    i64 left = ms - (plat_now_ms() - t0);
    if (left <= 0) break;
    if (LD(p->door) == p->seen_door) plat_wait(0, &c->kick, seen, (int)left);
  }
#undef WOKEN
  p->idle_cycles += (plat_now_ms() - t0) * m0->cycles_per_ms;
  addf(&p->idle0_us, plat_now_us() - tu);
  ST(c->woke, 0);
  poll(p, m0);
  if (m0->cpu_state == CPU_WAIT && w) {
    m0->cpu_state = CPU_RUN;
    ST(c->wait_addr, 0);
    ST(c->state, S_RUN);
  }
}

static void put_hex(machine *m, uword v) {
  char b[24], t[20]; int n = 0, k = 0;
  do { t[k++] = "0123456789ABCDEF"[v & 15]; v >>= 4; } while (v);
  b[n++] = '0'; b[n++] = 'x';
  while (k) b[n++] = t[--k];
  b[n] = 0;
  machine_emits(m, b);
}
static void put_dec(machine *m, i64 v) {
  char t[24], b[24]; int k = 0, n = 0;
  int neg = v < 0; uint64_t u = neg ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
  do { t[k++] = (char)('0' + u % 10); u /= 10; } while (u);
  if (neg) b[n++] = '-';
  while (k) b[n++] = t[--k];
  b[n] = 0;
  machine_emits(m, b);
}
/* smp.c's report, from what each CPU left in its words */
static void deadlock(par *p) {
  machine *m0 = p->s->cpu[0];
  machine_emits(m0, "libjs: deadlock: every processor is parked and none can be woken\n");
  for (int k = 0; k < p->s->n; k++) {
    int st = k == 0 ? (m0->cpu_state == CPU_RUN ? S_RUN : m0->cpu_state == CPU_WAIT ? S_WAIT : m0->cpu_state == CPU_HALT ? S_HALT : S_OFF)
                    : state(p, k);
    machine_emits(m0, "  cpu "); put_dec(m0, k); machine_emits(m0, ": ");
    if (st == S_OFF) machine_emits(m0, "off");
    else if (st == S_RUN) machine_emits(m0, "running");
    else if (st == S_WAIT) {
      machine_emits(m0, "waiting on "); put_hex(m0, (uword)LD(p->c[k].wait_addr));
      machine_emits(m0, " for it to stop being "); put_dec(m0, LD(p->c[k].wait_val));
    } else machine_emits(m0, "halted");
    machine_emits(m0, ", pc "); put_hex(m0, (uword)(k == 0 ? m0->pc : p->c[k].pc));
    machine_emits(m0, ", "); put_dec(m0, k == 0 ? m0->cycle : p->c[k].cycles); machine_emits(m0, " cycles\n");
  }
  m0->halted = 1;
  m0->status = -1;
}

/* After CPU 0's run stopped with r (cli.js's loop): 0, the host carries on
 * with r as it would; 1, CPU 0 napped or waited here, so run again; 2, the
 * machine is over, with *status */
int par_after(smp *s, int r, i32 *status) {
  par *p = s->par; machine *m0 = s->cpu[0];
  if (m0->halted) return 0;
  if (r == STOP_SLEEP && (others_running(p) || ipi_for0(p))) {
    i64 nap = m0->nap_ms, slept = 0;
    while (slept < nap && !ipi_for0(p) && !m0->halted) slept += sleep_sync(p, nap - slept);
    if (slept < nap) m0->sleep_ms -= nap - slept;
    return 1;
  }
  if (r == STOP_PARK || r == STOP_CPUHALT || m0->cpu_state != CPU_RUN) {
    if (m0->cpu_state == CPU_HALT && others_done(p)) { *status = m0->cpu_status; return 2; }
    /* parked with every other CPU parked or halted, three seconds running: nothing can wake them */
    i64 now = plat_now_ms();
    p->stuck = all_parked(p) ? (p->stuck ? p->stuck : now) : 0;
    if (p->stuck && now - p->stuck >= 3000) { deadlock(p); *status = m0->status; return 2; }
    park_sync(p, 10);
    return 1;
  }
  p->stuck = 0;
  return 0;
}

/* CPU 0 idle (pc.c, waiting for a key or the clock): until an interrupt,
 * or another CPU's request, at most 2 ms */
void par_idle0(smp *s) {
  par *p = s->par;
  int seen = LD(p->c[0].kick);
  if (LD(p->door) != p->seen_door) return;
  plat_wait(0, &p->c[0].kick, seen, 2);
}

/* -s: what the other CPUs asked and how they waited, a line */
void par_stats(smp *s, char *buf, u32 len) {
  par *p = s->par;
  char *o = buf, *e = buf + len - 1;
#define PUT(str) do { const char *q_ = (str); while (*q_ && o < e) *o++ = *q_++; } while (0)
#define NUM(v) do { char t_[24]; int k_ = 0; uint64_t x_ = (uint64_t)(v); do t_[k_++] = (char)('0' + x_ % 10); while (x_ /= 10); while (k_ && o < e) *o++ = t_[--k_]; } while (0)
  PUT("par: "); NUM(p->n_ask); PUT(" requests to CPU 0, "); NUM((uint64_t)p->ask_us / 1000); PUT(" ms waiting on them; ");
  NUM(p->n_park); PUT(" parks, "); NUM((uint64_t)p->park_us / 1000); PUT(" ms parked (ended by the word "); NUM(p->n_park_word);
  PUT(", a CWAK "); NUM(p->n_park_woke); PUT(", an IPI "); NUM(p->n_park_ipi); PUT("; a 10 ms look again in "); NUM(p->n_park_tmo);
  PUT("); CPU 0 parked "); NUM((uint64_t)p->idle0_us / 1000); PUT(" ms");
  /* the five asked most: "r0x40c 1234" a register, "op 77 56" an opcode */
  {
    static const char hx[] = "0123456789abcdef";
    int done[5] = { -1, -1, -1, -1, -1 };
    for (int n = 0; n < 5; n++) {
      int best = -1; u32 bv = 0;
      for (int i = 0; i < 1024; i++) {
        int taken = 0;
        for (int j = 0; j < n; j++) if (done[j] == i) taken = 1;
        if (!taken && asked_what[i] > bv) { bv = asked_what[i]; best = i; }
      }
      if (best < 0) break;
      done[n] = best;
      PUT(n ? ", " : "; most asked: ");
      if (best == 1023) PUT("other");
      else if (best >= 512) { PUT("op "); NUM(best - 512); }
      else { char a_[8]; u32 v_ = (u32)best << 2; a_[0] = 'r'; a_[1] = '0'; a_[2] = 'x'; a_[3] = hx[(v_ >> 8) & 15]; a_[4] = hx[(v_ >> 4) & 15]; a_[5] = hx[v_ & 15]; a_[6] = 0; PUT(a_); }
      PUT(" "); NUM(bv);
    }
  }
#undef PUT
#undef NUM
  *o = 0;
}

/* the machine stopped: every other CPU leaves its loop */
void par_shutdown(smp *s) {
  par *p = s->par;
  ST(p->halt, 1);
  for (int k = 1; k < s->n; k++) kick(p, k);
}
