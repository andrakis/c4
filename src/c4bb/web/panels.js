// panels.js - side panels: registers, microcode listing, terminal.

import { R, REG_NAMES } from '../sim/machine.js';
import { OPNAMES } from '../sim/devices.js';

const hex = v => '0x' + (v >>> 0).toString(16).toUpperCase().padStart(8, '0');
const opname = i => (i >= 0 && i < 66) ? OPNAMES.slice(i * 5, i * 5 + 4).trim() : '?' + i;

export class RegsPanel {
  constructor(table) {
    this.table = table;
    this.rows = new Map();
    this.prev = new Map();
    const names = [...REG_NAMES, 'cycle', 'µpc', 'mode'];
    for (const n of names) {
      const tr = document.createElement('tr');
      const td1 = document.createElement('td'); td1.className = 'name'; td1.textContent = n;
      const td2 = document.createElement('td'); td2.className = 'val';
      tr.append(td1, td2);
      table.appendChild(tr);
      this.rows.set(n, tr);
    }
  }
  set(name, val) {
    const tr = this.rows.get(name);
    tr.classList.toggle('changed', this.prev.get(name) !== undefined && this.prev.get(name) !== val);
    tr.lastChild.textContent = val;
    this.prev.set(name, val);
  }
  update(m) {
    for (let i = 0; i < REG_NAMES.length; i++) {
      const n = REG_NAMES[i];
      this.set(n, n === 'IR' ? `${opname(m.regs[R.IR])} (${m.regs[R.IR]})` : hex(m.regs[i]));
    }
    this.set('cycle', String(m.cycle));
    this.set('µpc', m.upc < 0 ? 'fetch' : String(m.upc));
    this.set('mode', (m.mode ? 'protected' : 'unprotected') + (m.trapHandler ? ', ITH set' : ''));
  }
}

export class UcodePanel {
  constructor(pre, title, ucode) {
    this.pre = pre;
    this.title = title;
    this.ucode = ucode;
    // scope -> ordered step indices
    this.byScope = new Map();
    ucode.steps.forEach((s, i) => {
      if (!this.byScope.has(s.scope)) this.byScope.set(s.scope, []);
      this.byScope.get(s.scope).push(i);
    });
    this.curScope = null;
  }
  update(m, lastEvent) {
    const upc = m.upc;
    const step = upc >= 0 ? this.ucode.steps[upc] : null;
    const scope = step ? step.scope : 'fetch';
    this.title.textContent = `microcode - ${scope}`;
    const idxs = this.byScope.get(scope) || [];
    let html = '';
    for (const i of idxs) {
      const line = this.ucode.steps[i].line || '(operand fetch)';
      html += (i === upc)
        ? `<span class="cur">&raquo; ${escapeHtml(line)}</span>\n`
        : `  ${escapeHtml(line)}\n`;
    }
    this.pre.innerHTML = html;
  }
}

export class Terminal {
  constructor(el) {
    this.el = el;
    this.buf = '';
    this.pending = '';
    this.flushTimer = null;
  }
  clear() { this.buf = ''; this.pending = ''; this.el.innerHTML = ''; }
  write(byte) {
    this.pending += String.fromCharCode(byte);
    if (!this.flushTimer) this.flushTimer = setTimeout(() => this.flush(), 16);
  }
  writeString(s) { for (const c of s) this.write(c.charCodeAt(0)); }
  backspace() {
    this.flush();
    if (this.buf.length && !this.buf.endsWith('\n')) {
      this.buf = this.buf.slice(0, -1);
      this.el.innerHTML = ansiToHtml(this.buf);
    }
  }
  flush() {
    this.flushTimer = null;
    if (!this.pending) return;
    this.buf += this.pending;
    this.pending = '';
    if (this.buf.length > 65536) this.buf = this.buf.slice(-49152);
    this.el.innerHTML = ansiToHtml(this.buf);
    this.el.scrollTop = this.el.scrollHeight;
  }
}

function escapeHtml(s) {
  return s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
}

// ---- ANSI SGR -> inline-styled HTML ----
//
// mandel.c (the only image that colors its output) emits exactly
// ESC[48;05;Nm (256-color background) and ESC[0m (reset) - see
// src/tests/mandel.c:106. Handled generally (fg/bg, 16 + 256-color,
// bold, underline, reverse) so any future colored program works too.
// Other CSI sequences (cursor movement etc.) are recognized and
// dropped rather than rendered as literal bytes.

const XTERM256 = (() => {
  const base = ['#000000', '#cd0000', '#00cd00', '#cdcd00', '#0000ee', '#cd00cd', '#00cdcd', '#e5e5e5',
                '#7f7f7f', '#ff0000', '#00ff00', '#ffff00', '#5c5cff', '#ff00ff', '#00ffff', '#ffffff'];
  const steps = [0, 95, 135, 175, 215, 255];
  const table = base.slice();
  for (let r = 0; r < 6; r++) for (let g = 0; g < 6; g++) for (let b = 0; b < 6; b++)
    table.push(`rgb(${steps[r]},${steps[g]},${steps[b]})`);
  for (let i = 0; i < 24; i++) { const v = 8 + i * 10; table.push(`rgb(${v},${v},${v})`); }
  return table;
})();

function applySgr(state, params) {
  for (let i = 0; i < params.length; i++) {
    const p = params[i];
    if (p === 0) { state.fg = state.bg = null; state.bold = state.underline = state.reverse = false; }
    else if (p === 1) state.bold = true;
    else if (p === 4) state.underline = true;
    else if (p === 7) state.reverse = true;
    else if (p === 22) state.bold = false;
    else if (p === 24) state.underline = false;
    else if (p === 27) state.reverse = false;
    else if (p >= 30 && p <= 37) state.fg = XTERM256[p - 30];
    else if (p === 38 && params[i + 1] === 5) { state.fg = XTERM256[params[i + 2]]; i += 2; }
    else if (p === 38 && params[i + 1] === 2) { state.fg = `rgb(${params[i + 2]},${params[i + 3]},${params[i + 4]})`; i += 4; }
    else if (p === 39) state.fg = null;
    else if (p >= 40 && p <= 47) state.bg = XTERM256[p - 40];
    else if (p === 48 && params[i + 1] === 5) { state.bg = XTERM256[params[i + 2]]; i += 2; }
    else if (p === 48 && params[i + 1] === 2) { state.bg = `rgb(${params[i + 2]},${params[i + 3]},${params[i + 4]})`; i += 4; }
    else if (p === 49) state.bg = null;
    else if (p >= 90 && p <= 97) state.fg = XTERM256[8 + p - 90];
    else if (p >= 100 && p <= 107) state.bg = XTERM256[8 + p - 100];
  }
}

function sgrStyle(state) {
  let fg = state.fg, bg = state.bg;
  if (state.reverse) { const t = fg; fg = bg || '#0d0b1e'; bg = t || '#c8c2e8'; }
  const parts = [];
  if (fg) parts.push(`color:${fg}`);
  if (bg) parts.push(`background:${bg}`);
  if (state.bold) parts.push('font-weight:bold');
  if (state.underline) parts.push('text-decoration:underline');
  return parts.join(';');
}

const CSI_RE = /\x1b\[([0-9;?]*)([a-zA-Z])/g;

function ansiToHtml(text) {
  const state = { fg: null, bg: null, bold: false, underline: false, reverse: false };
  let html = '', last = 0, m;
  const flushSpan = end => {
    if (end <= last) return;
    const chunk = escapeHtml(text.slice(last, end));
    const style = sgrStyle(state);
    html += style ? `<span style="${style}">${chunk}</span>` : chunk;
  };
  CSI_RE.lastIndex = 0;
  while ((m = CSI_RE.exec(text))) {
    flushSpan(m.index);
    if (m[2] === 'm') applySgr(state, m[1].length ? m[1].split(';').map(n => n === '' ? 0 : +n) : [0]);
    // any other final letter (cursor movement, clear, etc.): dropped
    last = CSI_RE.lastIndex;
  }
  flushSpan(text.length);
  return html;
}
