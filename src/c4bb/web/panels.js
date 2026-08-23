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

// A terminal with a SCREEN, not just a scrollback.
//
// It began as a pure teletype: bytes appended to one string, the whole
// string re-rendered, and every CSI sequence that was not a colour
// dropped on the floor. That is right for a shell and wrong for
// anything that repaints -- src/tests/raycast.c draws a full frame
// many times a second, and without cursor addressing each frame has to
// scroll the previous one off, which reads as a picture that will not
// hold still.
//
// So: cells, a cursor, and the sequences a full-screen program
// actually uses (H/f, J, K, A/B/C/D, and ?25 hide/show). Scrollback is
// preserved -- lines simply accumulate, and "the screen" is the last
// `rows` of them, which is what ESC[H homes to. A program that never
// positions the cursor behaves exactly as it did before.
export class Terminal {
  constructor(el, rows = 25, cols = 80) {
    this.el = el;
    this.rows = rows;
    this.cols = cols;
    this.maxLines = 1000;
    this.clear();
  }

  clear() {
    this.lines = [[]];          // each row: array of { ch, style }
    this.cy = 0;
    this.cx = 0;
    this.state = { fg: null, bg: null, bold: false, underline: false, reverse: false };
    this.pending = '';
    this.carry = '';            // an escape sequence split across writes
    this.cursorHidden = false;
    this.wrap = false;           // deferred-wrap armed at the last column
    this.flushTimer = null;
    if (this.el) this.el.innerHTML = '';
  }

  write(byte) {
    this.pending += String.fromCharCode(byte);
    if (!this.flushTimer) this.flushTimer = setTimeout(() => this.flush(), 16);
  }
  writeString(s) { for (const c of s) this.write(c.charCodeAt(0)); }

  backspace() {
    this.flush();
    if (this.cx > 0) {
      this.cx--;
      const row = this.lines[this.cy];
      if (row && this.cx < row.length) row[this.cx] = { ch: ' ', style: '' };
      this.render();
    }
  }

  flush() {
    this.flushTimer = null;
    if (!this.pending) return;
    const s = this.pending;
    this.pending = '';
    this.feed(s);
    this.render();
  }

  // ---- the byte stream ----

  feed(s) {
    s = this.carry + s;
    this.carry = '';
    let i = 0;
    while (i < s.length) {
      if (s[i] === '\x1b') {
        const m = /^\x1b\[([0-9;?]*)([a-zA-Z])/.exec(s.slice(i));
        if (m) { this.csi(m[1], m[2]); i += m[0].length; continue; }
        // Nothing matched yet. A sequence can be split across writes --
        // bytes arrive one at a time -- so hold a short tail back and
        // retry when more arrives; anything longer than a plausible
        // sequence is malformed and gets skipped.
        if (s.length - i < 16) { this.carry = s.slice(i); return; }
        i++;
        continue;
      }
      this.putc(s[i]);
      i++;
    }
  }

  ensure() {
    while (this.lines.length <= this.cy) this.lines.push([]);
    if (this.lines.length > this.maxLines) {
      const drop = this.lines.length - this.maxLines;
      this.lines.splice(0, drop);
      this.cy -= drop;
      if (this.cy < 0) this.cy = 0;
    }
  }

  putc(c) {
    const code = c.charCodeAt(0);
    if (c === '\n') { this.wrap = false; this.cy++; this.cx = 0; this.ensure(); return; }
    if (c === '\r') { this.wrap = false; this.cx = 0; return; }
    if (code === 8) { this.wrap = false; if (this.cx > 0) this.cx--; return; }
    if (code < 32) return;
    // Deferred wrap, as a real terminal does it: filling the last
    // column does NOT move the cursor, it arms a pending wrap that the
    // NEXT printable character takes. Wrapping eagerly costs a line
    // every time a program writes a full-width row -- which a
    // full-screen program does on every row of every frame.
    if (this.wrap) { this.wrap = false; this.cy++; this.cx = 0; }
    this.ensure();
    const row = this.lines[this.cy];
    while (row.length < this.cx) row.push({ ch: ' ', style: '' });
    row[this.cx] = { ch: c, style: sgrStyle(this.state) };
    this.cx++;
    if (this.cx >= this.cols) { this.cx = this.cols - 1; this.wrap = true; }
  }

  // The screen is the last `rows` lines; everything above is scrollback.
  // ESC[H homes to the top of that window, so a program drawing exactly
  // `rows` lines per frame lands on the same rows every time.
  home() { return Math.max(0, this.lines.length - this.rows); }

  blank(row, from, to) {
    const r = this.lines[row];
    if (!r) return;
    for (let i = from; i < to && i < r.length; i++) r[i] = { ch: ' ', style: '' };
    if (to >= r.length) r.length = Math.min(r.length, from);
  }

  csi(param, final) {
    if (final === 'm') {
      applySgr(this.state, param.length ? param.split(';').map(n => n === '' ? 0 : +n) : [0]);
      return;
    }
    if (final === 'h' || final === 'l') {
      if (param === '?25') this.cursorHidden = (final === 'l');
      return;                                   // other modes: ignored
    }
    const a = param.replace('?', '').split(';').map(n => n === '' ? 0 : +n);
    const n = a[0] || 1;
    this.wrap = false;                          // any movement cancels a pending wrap
    const top = this.home();
    if (final === 'H' || final === 'f') {
      this.cy = top + Math.max(1, a[0] || 1) - 1;
      this.cx = Math.max(1, a[1] || 1) - 1;
      this.ensure();
    } else if (final === 'A') { this.cy = Math.max(top, this.cy - n); }
    else if (final === 'B') { this.cy += n; this.ensure(); }
    else if (final === 'C') { this.cx = Math.min(this.cols - 1, this.cx + n); }
    else if (final === 'D') { this.cx = Math.max(0, this.cx - n); }
    else if (final === 'G') { this.cx = Math.max(0, (a[0] || 1) - 1); }
    else if (final === 'J') {
      const mode = a[0] || 0;
      if (mode === 2 || mode === 3) {
        while (this.lines.length < this.rows) this.lines.push([]);
        const t = this.lines.length - this.rows;
        for (let r = t; r < this.lines.length; r++) this.lines[r] = [];
        this.cy = t; this.cx = 0;
      } else if (mode === 0) {
        this.blank(this.cy, this.cx, this.cols);
        for (let r = this.cy + 1; r < this.lines.length; r++) this.lines[r] = [];
      } else {
        for (let r = top; r < this.cy; r++) this.lines[r] = [];
        this.blank(this.cy, 0, this.cx + 1);
      }
    }
    else if (final === 'K') {
      const mode = a[0] || 0;
      if (mode === 0) this.blank(this.cy, this.cx, this.cols);
      else if (mode === 1) this.blank(this.cy, 0, this.cx + 1);
      else this.blank(this.cy, 0, this.cols);
    }
    // anything else: recognised and dropped, as before
  }

  // ---- rendering ----

  toHtml() {
    let html = '';
    for (let r = 0; r < this.lines.length; r++) {
      const row = this.lines[r];
      let end = row.length;
      while (end > 0 && row[end - 1].ch === ' ' && !row[end - 1].style) end--;
      let i = 0;
      while (i < end) {
        const style = row[i].style;
        let j = i;
        let chunk = '';
        while (j < end && row[j].style === style) { chunk += row[j].ch; j++; }
        html += style ? `<span style="${style}">${escapeHtml(chunk)}</span>` : escapeHtml(chunk);
        i = j;
      }
      if (r < this.lines.length - 1) html += '\n';
    }
    return html;
  }

  render() {
    if (!this.el) return;
    this.el.innerHTML = this.toHtml();
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
// Cursor movement, erase and mode sequences are handled by Terminal
// above, against its cell grid; anything still unrecognised is
// consumed rather than rendered as literal bytes.

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


