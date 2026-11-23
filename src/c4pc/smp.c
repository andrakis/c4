/* smp.c -- several processors on one machine (libjs/smp.js): c4mp's
 * instruction set (src/c4mp/README.md), docs/c4ix-smp.md S1.
 *
 * Every CPU is a machine of its own -- registers, cycle count, trap
 * handler, cycle interval, protected mode, MMU -- over CPU 0's arena,
 * heap and devices (machine.dv). CPU 0 is the one the hosts boot and
 * drive; its machine_run hands over to this round robin, which runs each
 * runnable CPU for a quantum (c4mp's -q, 1000 instructions by default) in
 * CPU order, as smp.js does, so a run is the same as libjs's --cpus N to
 * the byte and the cycle:
 *
 *   - every instruction is atomic, so CAS, XCHG and FADD are plain
 *     read-modify-writes, and CWAI's test and park cannot lose a wake;
 *   - when nothing can run and some CPU is parked, the machine says so,
 *     names each CPU and where it waits, and stops.
 *
 * Time is the machine's, not a CPU's: cycle here advances by the most any
 * CPU ran in each round, which is what TIME and the pacing go by
 * (machine_clock). USLP sleeps the CPU that runs it while another can
 * run; only when none can does the machine nap, to the earliest end.
 *
 * The parallel engine (par.c) runs the same opcodes on a CPU of the PC's
 * own for each. */
#include "c4pc.h"
#include "clib.h"

void smp_cpu_reset(machine *m) {
  m->pc = m->sp = m->bp = m->a = 0;
  m->mode = 0;
  m->cycle = 0; m->skipped = 0;
  m->trap_handler = 0; m->cycle_interval = 0; m->cycle_handler = 0;
  m->trap_pc = 0;
  m->b_fired = 0; m->b_next_irq = 0;
  m->trap_restores_interval = 0;
  memset(m->sigh, 0, sizeof m->sigh);
  m->pending_signal = 0;
  m->missed_traps = 0;
  m->tt = m->tp = m->hnd = m->jmode = m->jinterval = 0;
  m->irq_hold = 0;
  m->vm = 0;
  if (m->mmu) mmu_reset(m->mmu);
}

/* CPU k > 0: a machine on m0's memory and devices */
static machine *cpu_new(machine *m0) {
  machine *m = xmalloc(sizeof *m);
  *m = *m0;                              /* the shared pointers; the device fields go unused */
  m->dv = m0;
  m->mmu = 0;
  if (m0->mmu) mmu_new(m);
  memset(m->local, 0, sizeof m->local);
  smp_cpu_reset(m);
  return m;
}

smp *smp_new(machine *m0, int n, i32 quantum) {
  smp *s = xmalloc(sizeof *s);
  if (n > 32) n = 32;                    /* the accelerators keep 32 CPUs' state (accel.h), par.c's MAXCPU */
  s->n = n;
  s->quantum = quantum > 0 ? quantum : 1000;
  s->cpu = xmalloc((size_t)n * sizeof *s->cpu);
  for (int i = 0; i < n; i++) {
    machine *m = i == 0 ? m0 : cpu_new(m0);
    m->smp = s;
    m->cpu_id = i;
    m->cpu_state = i == 0 ? CPU_RUN : CPU_OFF;
    m->ipi_pending = 0;
    m->wait_addr = m->wait_val = m->cpu_status = 0;
    s->cpu[i] = m;
  }
  return s;
}

i64 machine_clock(machine *m) {
  if (!m->smp) return m->cycle;
  return m->smp->par ? par_clock(m->smp) : m->smp->cycle;
}

/* __c4_cpu_start(id, entry, stacktop): the guest's function on the
 * guest's stack; 1, or 0 if the id is out of range or that CPU runs */
static i32 start(smp *s, word id, word entry, word stacktop) {
  if (id < 0 || id >= s->n) return 0;
  machine *m = s->cpu[id], *m0 = s->cpu[0];
  if (m->cpu_state == CPU_RUN || m->cpu_state == CPU_WAIT || m->cpu_state == CPU_SLEEP) return 0;
  if (!entry || !stacktop) return 0;
  /* what the hosts set on CPU 0 after the CPUs were made */
  m->accel = m0->accel; m->osaccel = m0->osaccel; m->no_fpu = m0->no_fpu;
  m->gui = m0->gui; m->snd = m0->snd; m->heap = m0->heap;
  smp_cpu_reset(m);
  m->sp = stacktop - WB;
  if ((uword)m->sp <= m->size - WB) m->w[(uword)m->sp >> WSH] = CPU_HALT_ADDR;   /* where the entry function's LEV lands */
  m->bp = m->sp;
  m->pc = entry;
  m->a = 0;
  m->ipi_pending = 0;
  m->cpu_state = CPU_RUN;
  m->cpu_status = 0;
  return 1;
}

/* __c4_wake(addr, n): release up to n CPUs parked on addr (n <= 0: all) */
static i32 wake(smp *s, word addr, word n) {
  if (!addr) return 0;
  i32 woken = 0;
  for (int k = 0; k < s->n; k++) {
    machine *m = s->cpu[k];
    if (n > 0 && woken >= n) break;
    if (m->cpu_state != CPU_WAIT || m->wait_addr != addr) continue;
    m->wait_addr = 0; m->wait_val = 0;
    m->cpu_state = CPU_RUN;
    woken++;
  }
  return woken;
}

/* __c4_ipi(id): HIRQ_IPI on that CPU; a parked or sleeping one is woken
 * to take it */
static i32 ipi(smp *s, word id) {
  if (id < 0 || id >= s->n) return 0;
  machine *m = s->cpu[id];
  if (m->cpu_state == CPU_HALT || m->cpu_state == CPU_OFF) return 0;
  m->ipi_pending = 1;
  if (m->cpu_state == CPU_WAIT) { m->wait_addr = 0; m->wait_val = 0; m->cpu_state = CPU_RUN; }
  if (m->cpu_state == CPU_SLEEP) m->cpu_state = CPU_RUN;
  return 1;
}

/* c4mp's processor opcodes (66-75): their arguments on the stack as a
 * syscall's are, the last pushed on top (c4m.js smpOp) */
int smp_op(machine *m, word op) {
  smp *s = m->smp;
  word sp = m->sp;
  /* the parallel engine: real CPUs, real atomics */
  if (s->par) return par_op(m, op);
  switch (op) {
    case OP_CPUI: m->a = m->cpu_id; return STOP_BUDGET;
    case OP_CPUN: m->a = s->n; return STOP_BUDGET;
    case OP_CPUS: m->a = start(s, m_ldw(m, sp + 2 * WB), m_ldw(m, sp + WB), m_ldw(m, sp)); return STOP_BUDGET;
    case OP_CPUH: m->cpu_state = CPU_HALT; return STOP_CPUHALT;
    case OP_CAS: {
      word t = m_ldw(m, sp + 2 * WB), v = m_ldw(m, t);
      if (v == m_ldw(m, sp + WB)) m_stw(m, t, m_ldw(m, sp));
      m->a = v;
      return STOP_BUDGET;
    }
    case OP_XCHG: {
      word t = m_ldw(m, sp + WB), v = m_ldw(m, t);
      m_stw(m, t, m_ldw(m, sp));
      m->a = v;
      return STOP_BUDGET;
    }
    case OP_FADD: {
      word t = m_ldw(m, sp + WB), v = m_ldw(m, t);
      m_stw(m, t, (word)((uword)v + (uword)m_ldw(m, sp)));
      m->a = v;
      return STOP_BUDGET;
    }
    case OP_CWAI: {
      /* park while *addr is still val: 1 if it already is not, 0 once woken */
      word t = m_ldw(m, sp + WB), val = m_ldw(m, sp);
      if (!t || m_ldw(m, t) != val) { m->a = 1; return STOP_BUDGET; }
      m->a = 0;
      m->wait_addr = t; m->wait_val = val;
      m->cpu_state = CPU_WAIT;
      return STOP_PARK;
    }
    case OP_CWAK: m->a = wake(s, m_ldw(m, sp + WB), m_ldw(m, sp)); return STOP_BUDGET;
    case OP_IPI: m->a = ipi(s, m_ldw(m, sp)); return STOP_BUDGET;
  }
  return STOP_BUDGET;
}

/* ---- the deadlock report ---- */
static void put_hex(machine *m, uword v) {
  char b[24]; int n = 0; char t[20]; int k = 0;
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
/* nothing can run, and some CPU waits for a word no one will change */
static void deadlock(smp *s) {
  machine *m0 = s->cpu[0];
  machine_emits(m0, "libjs: deadlock: every processor is parked and none can be woken\n");
  for (int i = 0; i < s->n; i++) {
    machine *m = s->cpu[i];
    machine_emits(m0, "  cpu "); put_dec(m0, i); machine_emits(m0, ": ");
    switch (m->cpu_state) {
      case CPU_OFF: machine_emits(m0, "off"); break;
      case CPU_RUN: machine_emits(m0, "running"); break;
      case CPU_WAIT:
        machine_emits(m0, "waiting on "); put_hex(m0, (uword)m->wait_addr);
        machine_emits(m0, " for it to stop being "); put_dec(m0, m->wait_val);
        break;
      case CPU_SLEEP: machine_emits(m0, "asleep"); break;
      default: machine_emits(m0, "halted, status "); put_dec(m0, m->cpu_status);
    }
    machine_emits(m0, ", pc "); put_hex(m0, (uword)m->pc);
    machine_emits(m0, ", "); put_dec(m0, m->cycle); machine_emits(m0, " cycles\n");
  }
  m0->halted = 1;
  m0->status = -1;                       /* exits 255, as c4mp does */
}

/* up to max_cycles of machine time, a quantum per CPU per round */
int smp_run(smp *s, i64 max_cycles) {
  if (s->par) return par_run0(s, max_cycles);
  machine *m0 = s->cpu[0];
  i64 end = s->cycle + max_cycles;
  int stop = STOP_BUDGET;
  s->running = 1;
  while (s->cycle < end) {
    int ran = 0, waiting = 0, inputs = 0, sleepers = 0, sleep = 0;
    i64 most = 0;
    for (int k = 0; k < s->n; k++) {
      machine *m = s->cpu[k];
      if (m->cpu_state == CPU_SLEEP) {
        if (machine_sim_ms(m0) - m->wake_at < 0) { sleepers++; continue; }
        m->cpu_state = CPU_RUN;
      }
      if (m->cpu_state == CPU_WAIT) { waiting++; continue; }
      if (m->cpu_state != CPU_RUN) continue;
      ran++;
      i64 before = m->cycle, left = end - s->cycle;
      int r = machine_run(m, s->quantum < left ? s->quantum : left);
      if (m->cycle - before > most) most = m->cycle - before;
      if (m0->halted) { s->cycle += most > 1 ? most : 1; s->running = 0; return STOP_HALT; }
      if (r == STOP_INPUT) inputs++;
      else if (r == STOP_SLEEP) {
        int other = 0;
        for (int j = 0; j < s->n; j++) if (j != k && s->cpu[j]->cpu_state == CPU_RUN) { other = 1; break; }
        if (other) {
          /* another CPU can use the time: this one sleeps alone, and the
           * machine's clock keeps going by the others' cycles */
          i64 nap = m0->nap_ms;
          m0->sleep_ms -= nap;
          m0->nap_ms = 0;
          m->wake_at = machine_sim_ms(m0) + nap;
          m->cpu_state = CPU_SLEEP;
          sleepers++;
        } else sleep = 1;
      }
    }
    s->cycle += most > (ran ? 1 : 0) ? most : (ran ? 1 : 0);
    if (!ran) {
      if (sleepers) {
        /* no CPU can run: the machine naps to the first sleeper's end */
        i64 now = machine_sim_ms(m0), first = INT64_MAX;
        for (int k = 0; k < s->n; k++) {
          machine *m = s->cpu[k];
          if (m->cpu_state == CPU_SLEEP) { if (m->wake_at < first) first = m->wake_at; m->cpu_state = CPU_RUN; }
        }
        i64 nap = first - now > 1 ? first - now : 1;
        m0->sleep_ms += nap;
        m0->nap_ms = nap;
        m0->nap_until = now + nap;
        stop = STOP_SLEEP;
        break;
      }
      if (waiting) { deadlock(s); s->running = 0; return STOP_HALT; }
      /* every CPU halted without CPU 0 exiting: its status stands */
      m0->halted = 1;
      m0->status = m0->cpu_status;
      s->running = 0;
      return STOP_HALT;
    }
    if (inputs == ran) { stop = STOP_INPUT; break; }
    if (sleep) { stop = STOP_SLEEP; break; }
  }
  s->running = 0;
  return stop;
}
