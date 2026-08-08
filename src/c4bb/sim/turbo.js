// turbo.js - the fast engine: microcode compiled to JavaScript.
//
// The step engine (machine.js) interprets one microstep at a time so
// the board can animate every control line. This module compiles the
// SAME assembled step tables into one JS function per opcode, so the
// fast path executes exactly the transfers the slow path would - same
// registers, same memory traffic, same cycle counts - just without
// the per-step interpretation. If the microcode is wrong, both
// engines are wrong identically, and the lockstep test would still
// match; that is why correctness is anchored by diffing against
// native c4m (test-c4bb.sh), while lockstep anchors step==turbo.
//
// Dispatch mirrors machine.decode: unknown opcodes miss their trap
// silently (oisc4 convention), OPCD re-dispatches with the <=ADJ
// guard, jsops go through machine.execJsop.

import { FETCH, DISPATCH, ALU_OPS } from './ucode.js';
import { R, OP, INS_SIZE, PM_GATED, Machine } from './machine.js';
import { OPNAMES } from './devices.js';

const REG_LOCAL = ['pc', 'sp', 'bp', 'a', 'ir', 'opr', 'mar', 'mdr', 'b', 't', 'u'];

function aluExpr(step) {
  const l = step.aluLA ? 'a' : 'b';
  const r = step.aluR === 'T' ? 't' : step.aluR === 'MDR' ? 'mdr' : 'a';
  switch (ALU_OPS[step.aluOp]) {
    case 'OR': return `(${l}|${r})|0`;
    case 'XOR': return `(${l}^${r})|0`;
    case 'AND': return `(${l}&${r})|0`;
    case 'EQ': return `(${l}===${r}?1:0)`;
    case 'NE': return `(${l}!==${r}?1:0)`;
    case 'LT': return `(${l}<${r}?1:0)`;
    case 'GT': return `(${l}>${r}?1:0)`;
    case 'LE': return `(${l}<=${r}?1:0)`;
    case 'GE': return `(${l}>=${r}?1:0)`;
    case 'SHL': return `(${l}<<(${r}&31))|0`;
    case 'SHR': return `(${l}>>(${r}&31))|0`;
    case 'ADD': return `(${l}+${r})|0`;
    case 'SUB': return `(${l}-${r})|0`;
    case 'MUL': return `Math.imul(${l},${r})`;
    case 'DIV': return `(${r}===0?0:(${l}/${r})|0)`;
    case 'MOD': return `(${r}===0?0:(${l}%${r})|0)`;
  }
}

function busExpr(step) {
  const d = step.drivers[0];
  if (d) {
    switch (d) {
      case 'PC_OUT': return 'pc';
      case 'SP_OUT': return 'sp';
      case 'BP_OUT': return 'bp';
      case 'A_OUT': return 'a';
      case 'OPR_OUT': return 'opr';
      case 'OPR_OUTX4': return '(opr<<2)|0';
      case 'MDR_OUT': return 'mdr';
      case 'B_OUT': return 'b';
      case 'T_OUT': return 't';
      case 'T_OUTX4': return '(t<<2)|0';
      case 'U_OUT': return 'u';
      case 'CYC_OUT': return 'm.cycle|0';
      case 'ALU_OUT': return aluExpr(step);
    }
  }
  if (step.cval !== null) return `${step.cval | 0}`;
  return '0';
}

// Compile the routine starting at 'start' into a JS function body.
// Returns source for: function(m, ar, r) -> 0 (fetch) | 1 (dispatch)
function compileRoutine(steps, start) {
  // collect reachable steps
  const seen = new Set();
  const work = [start];
  while (work.length) {
    const i = work.pop();
    if (i < 0 || seen.has(i)) continue;
    seen.add(i);
    const s = steps[i];
    if (s.next !== undefined && s.next >= 0) work.push(s.next);
    if (s.brTarget !== null && s.brTarget >= 0) work.push(s.brTarget);
  }

  let src = `let ${REG_LOCAL.map((n, i) => `${n}=r[${i}]|0`).join(',')};\n`;
  src += `let ip=${start},ret=0,bus=0;\n`;
  src += `loop: for(;;){ switch(ip){\n`;
  for (const i of [...seen].sort((x, y) => x - y)) {
    const s = steps[i];
    src += `case ${i}: {\n`;
    if (s.mem === 'MEM_RD') src += `mdr=ar.read32(mar)|0;\n`;
    else if (s.mem === 'MEM_RDB') src += `mdr=ar.read8s(mar)|0;\n`;
    else if (s.mem === 'MEM_RDBU') src += `mdr=ar.read8u(mar)|0;\n`;
    if (s.drivers.length || s.cval !== null) src += `bus=${busExpr(s)};\n`;
    for (const l of s.latches) {
      if (l === 'MAR_IN') src += `mar=${s.marOfs ? `(bus+${s.marOfs << 2})|0` : 'bus'};\n`;
      else src += `${REG_LOCAL[R[l.replace('_IN', '')]]}=bus;\n`;
    }
    if (s.memWrite === 'MEM_WR') src += `ar.write32(mar,mdr);\n`;
    else if (s.memWrite === 'MEM_WRB') src += `ar.write8(mar,mdr&0xff);\n`;
    for (const c of s.counters) {
      const map = { PC_INC: 'pc=(pc+4)|0', SP_INC: 'sp=(sp+4)|0', SP_DEC: 'sp=(sp-4)|0',
                    T_DEC1: 't=(t-1)|0', U_INC1: 'u=(u+1)|0', B_INC1: 'b=(b+1)|0' };
      src += map[c] + ';\n';
    }
    const gotoIp = tgt =>
      tgt === FETCH ? `{ret=0;break loop;}` :
      tgt === DISPATCH ? `{ret=1;break loop;}` : `{ip=${tgt};continue;}`;
    if (s.brFlag !== null) {
      const flagExpr = s.brFlag === 'az' ? 'a===0' : s.brFlag === 'tz' ? 't===0' : 'mdr===0';
      src += `if(${s.brNeg ? '!' : ''}(${flagExpr})) ${gotoIp(s.brTarget)}\n`;
    }
    src += gotoIp(s.next === undefined ? FETCH : s.next) + `\n`;
    src += `}\n`;
  }
  src += `}}\n`;
  src += REG_LOCAL.map((n, i) => `r[${i}]=${n}`).join(';') + ';\n';
  src += `return ret;\n`;
  return new Function('m', 'ar', 'r', src);
}

export class Turbo {
  constructor(machine) {
    this.m = machine;
    const { steps } = machine.ucode;
    this.fetchFn = compileRoutine(steps, machine.fetchStart);
    this.opFns = new Array(INS_SIZE).fill(null);
    for (const [name, entry] of machine.ucode.ops) {
      const num = OP[name];
      if (entry.jsop) {
        this.opFns[num] = null;       // handled via execJsop
      } else {
        this.opFns[num] = compileRoutine(steps, entry.start);
      }
    }
    this.jsopNames = new Set([...machine.ucode.ops].filter(([, e]) => e.jsop).map(([n]) => n));
    this.opcdNum = OP.OPCD;
    this.trapFn = compileRoutine(steps, machine.trapStart);
  }

  // Run up to maxCycles instructions; returns false when halted.
  // stopPc: pause (return true) when PC equals it at an instruction
  // boundary - the loader's main-return sentinel.
  run(maxCycles = Infinity, stopPc = -1) {
    const m = this.m, ar = m.arena, r = m.regs, dev = m.dev;
    const opFns = this.opFns, fetchFn = this.fetchFn;
    let budget = maxCycles;
    const trapFn = this.trapFn, trapStart = m.trapStart;
    while (budget-- > 0 && !dev.halted) {
      if (r[R.PC] === stopPc) return true;
      m.cycle++;
      // boundary interrupt checks; a boundary jam runs the trap
      // routine and lets the handler's first instruction reuse this
      // iteration's ++cycle (boundaryChecks gave the increment back)
      if (m.cycleInterval || m.pendingSignal) {
        const start = m.boundaryChecks();
        if (start === trapStart) { trapFn(m, ar, r); continue; }
      }
      fetchFn(m, ar, r);                       // always ends in dispatch
      let fromOpcd = false;
      for (;;) {
        let ir = r[R.IR] | 0;
        if (fromOpcd && ir <= OP.ADJ) {        // OPCD guard (c4m.c:1535)
          const name = i => OPNAMES.slice(i * 5, i * 5 + 4);
          m.printVm(`${name(this.opcdNum)} does not support opcodes requiring arguments (${name(ir)} given)\n`);
          const t = m.jamTrap(5, ir, m.trapHandler, { zeroInterval: true, unprot: true });
          if (t === trapStart) { trapFn(m, ar, r); break; }
          ir = OP.C4CY;
          r[R.IR] = ir;
        }
        if (ir >= 0 && ir < INS_SIZE) {
          if (m.mode === 1 && PM_GATED.has(ir)) {
            const t = m.jamTrap(6, ir, m.trapHandler, { zeroInterval: true, unprot: true });
            if (t === trapStart) trapFn(m, ar, r);
            break;
          }
          const fn = opFns[ir];
          if (fn) {
            const ret = fn(m, ar, r);
            if (ret === 1) { fromOpcd = (ir === this.opcdNum); continue; }
            break;
          }
          const name = OPNAMES.slice(ir * 5, ir * 5 + 4).trim();
          if (this.jsopNames.has(name)) {
            const jam = m.execJsop(name);       // _TRP/DBG may jam
            if (jam === trapStart) trapFn(m, ar, r);
            break;
          }
        }
        const t = m.jamTrap(0, ir, m.trapHandler, { unprot: true });
        if (t === trapStart) trapFn(m, ar, r);
        break;
      }
    }
    return !dev.halted;
  }
}
