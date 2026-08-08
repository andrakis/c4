// board.js - Canvas renderer for the c4bb board.
//
// Draws the layout described by hw/board.hwd and lights it from the
// machine's actual state: the current microstep's asserted signals
// decide which chips, bus segments and control stubs glow. In turbo
// mode there is no per-step event stream, so modules glow when their
// visible value changed since the last frame ("heat").

import { R } from '../sim/machine.js';
import { OPNAMES } from '../sim/devices.js';
import { ALU_OPS } from '../sim/ucode.js';

const C = {
  bg: '#0d0b1e', chip: '#1a1633', edge: '#3a3160', text: '#c8c2e8',
  dim: '#6a6396', pink: '#ff2d95', cyan: '#00e5ff', yellow: '#ffe66d',
  green: '#3dff9e', busIdle: '#2a2347',
};

const hex = v => '0x' + (v >>> 0).toString(16).toUpperCase().padStart(8, '0');
const opname = i => (i >= 0 && i < 66) ? OPNAMES.slice(i * 5, i * 5 + 4).trim() : '?' + i;

export class BoardRenderer {
  constructor(canvas, boardDef) {
    this.canvas = canvas;
    this.ctx = canvas.getContext('2d');
    this.def = boardDef;
    this.lastVals = new Map();
    this.heat = new Map();
    // extent for scaling
    let mx = 0, my = 0;
    for (const m of boardDef.modules) {
      mx = Math.max(mx, m.at[0] + m.size[0]);
      my = Math.max(my, m.at[1] + m.size[1]);
    }
    for (const n of boardDef.nets)
      for (const [x, y] of n.route) { mx = Math.max(mx, x); my = Math.max(my, y); }
    this.extent = [mx + 20, my + 20];
    this.netByName = new Map(boardDef.nets.map(n => [n.name, n]));
  }

  // Value shown on each module's LED bank.
  moduleValue(name, machine) {
    const r = machine.regs;
    switch (name) {
      case 'PC': return hex(r[R.PC]);
      case 'IR': return opname(r[R.IR]);
      case 'OPR': return hex(r[R.OPR]);
      case 'A': return hex(r[R.A]);
      case 'SP': return hex(r[R.SP]);
      case 'BP': return hex(r[R.BP]);
      case 'B': return hex(r[R.B]);
      case 'T': return hex(r[R.T]);
      case 'U': return hex(r[R.U]);
      case 'MAR': return hex(r[R.MAR]);
      case 'MDR': return hex(r[R.MDR]);
      case 'CYC': return String(machine.cycle);
      case 'UC': return machine.upc < 0 ? 'fetch' : 'µ' + machine.upc;
      case 'MODE': return (machine.mode ? 'PROT' : 'unprot') + (machine.trapHandler ? ' ITH' : '');
      case 'ALU': {
        const s = this.curStep;
        return s && s.aluOp >= 0 ? ALU_OPS[s.aluOp] : '-';
      }
      case 'UART': return machine.dev.uartActivity ? String(machine.dev.uartActivity) : '-';
      case 'TIMER': return machine.dev.simMs() + 'ms';
      case 'POWER': return machine.dev.halted ? 'HALT ' + machine.dev.status : 'RUN';
      case 'CONST': {
        const s = this.curStep;
        return s && s.cval !== null ? hex(s.cval) : '-';
      }
      default: return '';
    }
  }

  assertedSignals(step) {
    if (!step) return new Set();
    const s = new Set(step.drivers);
    for (const l of step.latches) s.add(l === 'MAR_IN' ? 'MAR_IN' : l);
    if (step.mem) s.add(step.mem);
    if (step.memWrite) s.add(step.memWrite);
    for (const c of step.counters) s.add(c);
    if (step.aluOp >= 0) s.add('ALU_OUT');
    if (step.aluLA) s.add('ALU_LA');
    if (step.aluR === 'T') s.add('ALU_RT');
    if (step.aluR === 'MDR') s.add('ALU_RM');
    if (step.cval !== null) s.add('C_OUT');
    return s;
  }

  // mode: 'step' (use curStep signals) or 'heat' (value-change glow)
  render(machine, { step = null, mode = 'step', busValue = 0 } = {}) {
    this.curStep = step;
    const ctx = this.ctx, cv = this.canvas;
    const sx = cv.width / this.extent[0], sy = cv.height / this.extent[1];
    ctx.setTransform(1, 0, 0, 1, 0, 0);
    ctx.fillStyle = C.bg;
    ctx.fillRect(0, 0, cv.width, cv.height);
    ctx.scale(sx, sy);

    const sigs = mode === 'step' ? this.assertedSignals(step) : new Set();
    const busActive = mode === 'step' &&
      (step && (step.drivers.length > 0 || step.cval !== null));

    // ---- nets ----
    for (const net of this.def.nets) {
      const active = net.name === 'BUS' ? busActive
        : net.name === 'MEMB' ? (sigs.has('MEM_RD') || sigs.has('MEM_RDB') || sigs.has('MEM_RDBU') || sigs.has('MEM_WR') || sigs.has('MEM_WRB'))
        : net.name === 'CTRL' ? (mode === 'step' && step != null)
        : false;
      ctx.strokeStyle = active ? (net.name === 'CTRL' ? C.pink : C.cyan) : C.busIdle;
      ctx.lineWidth = net.width >= 30 ? 5 : 2;
      ctx.shadowColor = ctx.strokeStyle;
      ctx.shadowBlur = active ? 12 : 0;
      ctx.beginPath();
      net.route.forEach(([x, y], i) => i ? ctx.lineTo(x, y) : ctx.moveTo(x, y));
      ctx.stroke();
      ctx.shadowBlur = 0;
      if (net.name === 'BUS' && busActive) {
        ctx.fillStyle = C.cyan;
        ctx.font = '13px monospace';
        const [x0, y0] = net.route[0];
        ctx.fillText(hex(busValue), x0 + 8, y0 - 8);
      }
    }

    // ---- connect stubs ----
    for (const cn of this.def.connects) {
      const mod = this.def.modules.find(m => m.name === cn.from);
      const net = this.netByName.get(cn.to);
      if (!mod || !net || !net.route.length) continue;
      const netY = net.route[0][1];
      const cx = mod.at[0] + mod.size[0] / 2 +
        (cn.to === 'MEMB' ? 10 : cn.to === 'CTRL' ? 5 : 0);
      const fromY = netY > mod.at[1] ? mod.at[1] + mod.size[1] : mod.at[1];
      const active = mode === 'step' && mod.sigs.some(s => sigs.has(s));
      ctx.strokeStyle = active ? C.yellow : C.busIdle;
      ctx.lineWidth = 1.5;
      ctx.shadowColor = C.yellow;
      ctx.shadowBlur = active ? 8 : 0;
      ctx.beginPath();
      ctx.moveTo(cx, fromY);
      ctx.lineTo(cx, netY);
      ctx.stroke();
      ctx.shadowBlur = 0;
    }

    // ---- modules ----
    for (const mod of this.def.modules) {
      const [x, y] = mod.at, [w, h] = mod.size;
      const val = this.moduleValue(mod.name, machine);

      let active = false;
      if (mode === 'step') {
        active = mod.sigs.some(s => sigs.has(s)) ||
          (mod.name === 'CONST' && sigs.has('C_OUT')) ||
          (mod.name === 'RAM' && (sigs.has('MEM_RD') || sigs.has('MEM_WR')));
      } else {
        const prev = this.lastVals.get(mod.name);
        if (prev !== val) { this.heat.set(mod.name, 1); active = true; }
        else {
          const heat = (this.heat.get(mod.name) || 0) * 0.85;
          this.heat.set(mod.name, heat);
          active = heat > 0.1;
        }
      }
      this.lastVals.set(mod.name, val);

      ctx.fillStyle = C.chip;
      ctx.strokeStyle = active ? C.cyan : C.edge;
      ctx.lineWidth = active ? 2 : 1;
      ctx.shadowColor = C.cyan;
      ctx.shadowBlur = active ? 14 : 0;
      roundRect(ctx, x, y, w, h, 6);
      ctx.fill();
      ctx.stroke();
      ctx.shadowBlur = 0;
      // chip notch
      ctx.fillStyle = C.edge;
      ctx.beginPath();
      ctx.arc(x + w / 2, y, 5, 0, Math.PI);
      ctx.fill();

      ctx.fillStyle = C.dim;
      ctx.font = '10px monospace';
      ctx.fillText(mod.label, x + 8, y + 16, w - 16);
      ctx.fillStyle = active ? C.yellow : C.cyan;
      ctx.font = 'bold 15px monospace';
      ctx.fillText(val, x + 8, y + h - 14, w - 16);
    }
  }
}

function roundRect(ctx, x, y, w, h, r) {
  ctx.beginPath();
  ctx.moveTo(x + r, y);
  ctx.arcTo(x + w, y, x + w, y + h, r);
  ctx.arcTo(x + w, y + h, x, y + h, r);
  ctx.arcTo(x, y + h, x, y, r);
  ctx.arcTo(x, y, x + w, y, r);
  ctx.closePath();
}
