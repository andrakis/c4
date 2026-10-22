// smp.js - several processors on one machine, simulated: c4mp's
// instruction set (src/c4mp/README.md) on libjs, docs/c4ix-smp.md S1.
//
// Every CPU is a Machine (c4m.js) of its own -- registers, cycle count,
// trap handler, cycle interval, protected mode -- over the same arena,
// devices, bus and heap. CPU 0 is the one boot() starts and the hosts
// drive; its run() hands over to this round robin, which runs each
// runnable CPU for a quantum (c4mp's -q, 1000 instructions by default)
// in CPU order, as c4mp's c4_smp_run does. So:
//
//   - a run is DETERMINISTIC: the same quantum and order give the same
//     interleaving, and tests can pin transcripts;
//   - every instruction is atomic, so CAS, XCHG and FADD are plain
//     read-modify-writes, and CWAI's test and park cannot lose a wake;
//   - deadlock is diagnosable: when nothing can run and some CPU is
//     parked, the machine says so, names each CPU and where it waits,
//     and stops.
//
// Time is the machine's, not a CPU's: `cycle` here advances by the most
// any CPU ran in each round, which is what TIME and the pacing go by
// (devices.js timeSource). Each CPU's own count is what C4CY gives it.
//
// USLP sleeps the CPU that executes it, not the machine, while another
// CPU can run: it is 'sleep' until the machine's time reaches the end of
// its nap, or an IPI comes. Only when no CPU can run does the machine
// nap, to the earliest of those ends, and every sleeper wakes with it.
//
// The parallel engine (S2) runs the same opcodes on one worker per CPU.

import { OP, STOP, CPU_HALT_ADDR } from './c4m.js';
export { CPU_HALT_ADDR };

export class Smp {
  // m0: the boot CPU; make(): another Machine on the same arena and devices
  constructor(m0, n, quantum, make) {
    this.n = n;
    this.quantum = quantum > 0 ? quantum : 1000;
    this.dev = m0.dev;
    this.machines = [];
    for (let i = 0; i < n; i++) {
      const m = i === 0 ? m0 : make();
      m.smp = this;
      m.cpuId = i;
      m.cpuState = i === 0 ? 'run' : 'off';
      m.local = new Int32Array(64);
      m.localU8 = new Uint8Array(m.local.buffer);
      m.ipiPending = 0;
      m.waitAddr = 0; m.waitVal = 0; m.cpuStatus = 0;
      this.machines.push(m);
    }
    this.dev.machine = m0;
    this.dev.timeSource = this;
    this.cycle = 0;
    this.running = false;
  }

  // __c4_cpu_start(id, entry, stacktop): the guest's function on the
  // guest's stack; 1, or 0 if the id is out of range or that CPU runs
  start(id, entry, stacktop) {
    if (id < 0 || id >= this.n) return 0;
    const m = this.machines[id];
    if (m.cpuState === 'run' || m.cpuState === 'wait' || m.cpuState === 'sleep') return 0;
    if (!entry || !stacktop) return 0;
    const symbols = m.symbols;
    m.reset();
    m.symbols = this.machines[0].symbols || symbols;
    m.sp = (stacktop - 4) | 0;
    m.i32[m.sp >> 2] = CPU_HALT_ADDR;   // where the entry function's LEV lands
    m.bp = m.sp;
    m.pc = entry | 0;
    m.a = 0;
    m.ipiPending = 0;
    m.cpuState = 'run';
    m.cpuStatus = 0;
    return 1;
  }

  // __c4_wake(addr, n): release up to n CPUs parked on addr (n <= 0: all)
  wake(addr, n) {
    if (!addr) return 0;
    let woken = 0;
    for (const m of this.machines) {
      if (n > 0 && woken >= n) break;
      if (m.cpuState !== 'wait' || m.waitAddr !== addr) continue;
      m.waitAddr = 0; m.waitVal = 0;
      m.cpuState = 'run';
      woken++;
    }
    return woken;
  }

  // __c4_ipi(id): HIRQ_IPI on that CPU; a parked or sleeping one is
  // woken to take it
  ipi(id) {
    if (id < 0 || id >= this.n) return 0;
    const m = this.machines[id];
    if (m.cpuState === 'halt' || m.cpuState === 'off') return 0;
    m.ipiPending = 1;
    if (m.cpuState === 'wait') { m.waitAddr = 0; m.waitVal = 0; m.cpuState = 'run'; }
    if (m.cpuState === 'sleep') m.cpuState = 'run';
    return 1;
  }

  // Up to maxCycles of machine time, a quantum per CPU per round
  run(maxCycles) {
    const dev = this.dev, m0 = this.machines[0];
    const end = this.cycle + maxCycles;
    let stop = STOP.BUDGET;
    this.running = true;
    try {
      while (this.cycle < end) {
        let ran = 0, waiting = 0, inputs = 0, most = 0, sleep = false, sleepers = 0;
        for (const m of this.machines) {
          if (m.cpuState === 'sleep') {
            if (dev.simMs() - m.wakeAt < 0) { sleepers++; continue; }
            m.cpuState = 'run';
          }
          if (m.cpuState === 'wait') { waiting++; continue; }
          if (m.cpuState !== 'run') continue;
          ran++;
          dev.machine = m;
          const before = m.cycle;
          const r = m.run(Math.min(this.quantum, end - this.cycle));
          if (m.cycle - before > most) most = m.cycle - before;
          if (dev.halted) { this.cycle += Math.max(most, 1); return STOP.HALT; }
          if (r === STOP.INPUT) inputs++;
          else if (r === STOP.SLEEP) {
            if (this.machines.some(o => o !== m && o.cpuState === 'run')) {
              // another CPU can use the time: this one sleeps alone, and
              // the machine's clock keeps going by the others' cycles
              const nap = dev.napMs;
              dev.sleepMs -= nap;
              dev.napMs = 0;
              m.wakeAt = dev.simMs() + nap;
              m.cpuState = 'sleep';
              sleepers++;
            } else sleep = true;
          }
        }
        this.cycle += Math.max(most, ran ? 1 : 0);
        if (!ran) {
          if (sleepers) {
            // no CPU can run: the machine naps to the first sleeper's end
            const now = dev.simMs();
            let first = Infinity;
            for (const m of this.machines) if (m.cpuState === 'sleep') { first = Math.min(first, m.wakeAt); m.cpuState = 'run'; }
            const nap = Math.max(1, first - now);
            dev.sleepMs += nap;
            dev.napMs = nap;
            stop = STOP.SLEEP;
            break;
          }
          if (waiting) { this.deadlock(); return STOP.HALT; }
          // every CPU halted without CPU 0 exiting: its status stands
          dev.halted = true;
          dev.status = m0.cpuStatus | 0;
          return STOP.HALT;
        }
        if (inputs === ran) { stop = STOP.INPUT; break; }
        if (sleep) { stop = STOP.SLEEP; break; }
      }
    } finally {
      this.running = false;
      dev.machine = m0;
    }
    return stop;
  }

  // Nothing can run, and some CPU waits for a word no one will change
  deadlock() {
    const m0 = this.machines[0], hex = v => '0x' + (v >>> 0).toString(16).toUpperCase();
    let s = 'libjs: deadlock: every processor is parked and none can be woken\n';
    this.machines.forEach((m, i) => {
      s += `  cpu ${i}: `;
      if (m.cpuState === 'off') s += 'off';
      else if (m.cpuState === 'run') s += 'running';
      else if (m.cpuState === 'wait') s += `waiting on ${hex(m.waitAddr)} for it to stop being ${m.waitVal}`;
      else if (m.cpuState === 'sleep') s += 'asleep';
      else s += `halted, status ${m.cpuStatus}`;
      s += `, pc ${hex(m.pc)}, ${m.cycle} cycles\n`;
    });
    m0.emitString(s);
    this.dev.halted = true;
    this.dev.status = -1;           // exits 255, as c4mp does
  }
}
