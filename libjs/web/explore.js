// explore.js - the Code Explorer page (docs/c4-explorer.md, E4).
//
// An image, object or library from the repository (or from this computer):
// its functions, globals and strings; each function's code with every
// operand named and linked; who calls what; and the C it came from.
//
// The source is matched a function at a time: each .c in the directories
// given is scanned for definitions. Line by line needs the image's source
// map, track S's SMAP chunk, which no compiler writes yet; when an image
// has one, its rows colour the code and the source together.

import { readC4r, analyse, opName, hex, PATCH, SCLASS } from '../c4r.js';
import { OP } from '../c4m.js';
import { opDoc } from './opdoc.js';

const ROOT = new URL('../../', location.href);
const $ = id => document.getElementById(id);
const esc = s => String(s).replace(/[&<>"]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' })[c]);
const store = {
  get(k, d) { try { const v = localStorage.getItem('c4x.' + k); return v === null ? d : v; } catch { return d; } },
  set(k, v) { try { localStorage.setItem('c4x.' + k, v); } catch { /* private window */ } },
};

const S = {
  path: '', a: null, sel: null, listTab: 'funcs', sideTab: 'source',
  hist: [], hpos: -1, src: null, srcDirs: '', srcFile: null, hlRow: null,
};

// ---------------------------------------------------------------- images

// The repository's images: the server's listing if it gives one
// (serve.mjs ?ls), else the disks' index.json files.
async function listDir(path) {
  try {
    const r = await fetch(new URL(path + '?ls', ROOT));
    if (!r.ok || !(r.headers.get('content-type') || '').includes('json')) return null;
    return await r.json();
  } catch { return null; }
}
async function findImages() {
  const base = 'src/c4bb/images/', out = [];
  const top = await listDir(base);
  if (top) {
    for (const e of top) {
      if (e.dir && e.name !== 'build') {
        const sub = await listDir(base + e.name + '/');
        for (const s of sub || []) if (/\.c4[rol]$/.test(s.name)) out.push({ path: `${base}${e.name}/${s.name}`, size: s.size });
      } else if (/\.c4[rol]$/.test(e.name)) out.push({ path: base + e.name, size: e.size });
    }
    return out;
  }
  for (const d of ['disk', 'games', 'c4ke-root']) {
    try {
      const idx = await (await fetch(new URL(`${base}${d}/index.json`, ROOT))).json();
      for (const [n, size] of idx) if (/\.c4[rol]$/.test(n)) out.push({ path: `${base}${d}/${n}`, size });
    } catch { /* not built */ }
  }
  for (const n of ['hello32.c4r', 'c4ix32.c4r', 'c4dos32.c4r', 'c4ke32.c4r']) out.push({ path: base + n });
  return out;
}
async function fillImages() {
  const imgs = await findImages();
  const sel = $('images');
  const groups = new Map();
  for (const im of imgs) {
    const dir = im.path.slice(0, im.path.lastIndexOf('/') + 1);
    if (!groups.has(dir)) groups.set(dir, []);
    groups.get(dir).push(im);
  }
  sel.innerHTML = '<option value="">choose&hellip;</option>' + [...groups].map(([dir, l]) =>
    `<optgroup label="${esc(dir.replace('src/c4bb/images/', 'images/'))}">` +
    l.sort((x, y) => x.path.localeCompare(y.path)).map(im =>
      `<option value="${esc(im.path)}">${esc(im.path.slice(dir.length))}${im.size ? ` (${kb(im.size)})` : ''}</option>`).join('') +
    '</optgroup>').join('');
  sel.value = S.path;
}
const kb = n => n >= 1048576 ? (n / 1048576).toFixed(1) + ' MB' : n >= 1024 ? Math.round(n / 1024) + ' KB' : n + ' B';

async function openPath(path, then) {
  status(`reading ${path}`);
  try {
    const r = await fetch(new URL(path, ROOT));
    if (!r.ok) throw new Error(`${r.status} ${r.statusText}`);
    openBytes(new Uint8Array(await r.arrayBuffer()), path, then);
  } catch (e) { status(`${path}: ${e.message}`, true); }
}
function openBytes(bytes, path, then) {
  const t0 = performance.now();
  let a;
  try { a = analyse(readC4r(bytes)); }
  catch (e) { status(`${path}: ${e.message}`, true); return; }
  S.a = a; S.path = path; S.sel = null; S.hist = []; S.hpos = -1;
  $('path').value = path; $('images').value = path;
  store.set('path', path);
  summary();
  S.srcDirs = store.get('src:' + path, guessSources(path, a));
  $('srcdirs').value = S.srcDirs;
  S.src = null; S.srcFile = null;
  renderList();
  status(`${path}: ${a.funcs.length} functions, ${a.globals.length} globals, read in ${Math.round(performance.now() - t0)} ms`);
  if (then) then();
  else {
    const main = a.byName.get('main');
    go(main ? { kind: 'func', obj: main[0] } : a.funcs.length ? { kind: 'func', obj: a.funcs.find(f => !f.made) || a.funcs[0] } : null);
  }
  // with a source map the rows say where the code came from; the scan
  // by name is for an image without one
  if (S.srcDirs && !(a.img.smap && a.img.smap.rows.length)) scanSources();
}

function summary() {
  const { img } = S.a;
  const n = t => S.a.img.patches.filter(p => p.type === t).length;
  const ext = img.patches.filter(p => p.type > 0).length;
  const bits = [
    `<b>v${img.version}</b>, ${img.wordBits}-bit`,
    `<b>${img.code.length.toLocaleString()}</b> code words`,
    `<b>${img.data.length.toLocaleString()}</b> data bytes` + (img.memsz > img.data.length ? ` + ${(img.memsz - img.data.length).toLocaleString()} BSS` : ''),
    `<b>${img.patches.length.toLocaleString()}</b> patches (${n(PATCH.CODE)} code, ${n(PATCH.DATA)} data, ${n(PATCH.DCODE) + n(PATCH.DDATA)} in data${ext ? `, <b>${ext}</b> unresolved` : ''})`,
    `<b>${img.symbols.length.toLocaleString()}</b> symbols`,
    `entry ${fnLink(S.a.funcOf(img.entry), img.entry)}`,
  ];
  if (img.cons.length) bits.push(`constructors ${img.cons.map(c => fnLink(S.a.funcOf(c), c)).join(' ')}`);
  if (img.des.length) bits.push(`destructors ${img.des.map(c => fnLink(S.a.funcOf(c), c)).join(' ')}`);
  bits.push(img.smap ? `source map: <b>${img.smap.rows.length}</b> rows over ${img.smap.files.length} files`
                     : img.chunks.length ? `chunks ${img.chunks.map(c => c.tag).join(' ')}` : 'no source map');
  $('summary').innerHTML = bits.join(' &middot; ');
}

// Where an image's C is, by what it is called; the field is the person's
// to correct, and is kept per image.
function guessSources(path, a) {
  const n = path.slice(path.lastIndexOf('/') + 1).replace(/\.c4[rol]$/, '').replace(/32$/, '');
  const libc = 'src/c4ix/libc/';
  const games = { rott: 'games/rott/', doom: 'games/doom/', wolf3d: 'games/wolf3d/', tyrian: 'games/tyrian/' };
  for (const [g, d] of Object.entries(games))
    if (n === g || n === 'c4ix-' + g) return n.startsWith('c4ix-') ? `${d} src/c4ix/lib/` : `${d} ${libc}`;
  if (n === 'c4ix') return 'src/c4ix/ src/c4ix/libc/';
  if (n === 'c4ix-desktop') return 'src/c4ix/user/desktop/ src/c4ix/lib/';
  if (n.startsWith('c4ix-')) return 'src/c4ix/user/ src/c4ix/lib/';
  if (n === 'c4dos') return 'src/c4dos/';
  if (n === 'c4ke') return 'src/c4ke/';
  if (a.byName.has('main') && a.funcs.length < 40) return 'src/tests/';
  return '';
}

// ---------------------------------------------------------------- lists

function renderList() {
  const a = S.a; if (!a) return;
  let re = null;
  try { re = $('filter').value ? new RegExp($('filter').value, 'i') : null; } catch { re = null; }
  const sort = $('sort').value;
  let rows;
  if (S.listTab === 'funcs') {
    rows = a.funcs.filter(f => !re || re.test(f.name) || f.aliases.some(x => re.test(x)));
    if (sort === 'name') rows = [...rows].sort((x, y) => x.name.localeCompare(y.name));
    if (sort === 'size') rows = [...rows].sort((x, y) => y.words - x.words);
    $('list').innerHTML = rows.map(f =>
      `<div class="row${S.sel && S.sel.obj === f ? ' sel' : ''}" data-f="${f.index}" title="${esc(f.name)}${f.aliases.length ? ' = ' + esc(f.aliases.join(', ')) : ''} at ${hex(f.start)}">` +
      `<span class="n${f.made ? ' made' : ''}">${esc(f.name)}${f.dups ? ` <span class="m">#${f.nth}</span>` : ''}</span>` +
      `<span class="m">${f.words}</span></div>`).join('');
    $('list-count').textContent = `${rows.length} of ${a.funcs.length} functions; sizes in words`;
  } else if (S.listTab === 'globals') {
    rows = a.globals.filter(g => !re || re.test(g.name));
    if (sort === 'name') rows = [...rows].sort((x, y) => x.name.localeCompare(y.name));
    if (sort === 'size') rows = [...rows].sort((x, y) => y.size - x.size);
    $('list').innerHTML = rows.map(g =>
      `<div class="row${S.sel && S.sel.obj === g ? ' sel' : ''}" data-g="${g.index}" title="${esc(g.name)} at data+${g.off}">` +
      `<span class="n">${esc(g.name)}</span><span class="m">${g.bss ? 'bss ' : ''}${g.size}</span></div>`).join('');
    $('list-count').textContent = `${rows.length} of ${a.globals.length} globals; sizes in bytes, to the next symbol`;
  } else {
    const strs = strings();
    rows = strs.filter(s => !re || re.test(s.text));
    if (sort === 'name') rows = [...rows].sort((x, y) => x.text.localeCompare(y.text));
    if (sort === 'size') rows = [...rows].sort((x, y) => y.uses.length - x.uses.length);
    $('list').innerHTML = rows.map(s =>
      `<div class="row" data-s="${s.off}" title="data+${s.off}, used by ${esc([...new Set(s.uses.map(u => u.fn.name))].join(', '))}">` +
      `<span class="n str">${esc(JSON.stringify(s.text))}</span><span class="m">${s.uses.length}</span></div>`).join('');
    $('list-count').textContent = `${rows.length} of ${strs.length} strings the code takes the address of`;
  }
}

let strCache = null;
function strings() {
  if (strCache && strCache.a === S.a) return strCache.list;
  const m = new Map();
  for (const ins of S.a.insns)
    if (ins.ref && ins.ref.kind === 'data' && ins.ref.str !== undefined) {
      const e = m.get(ins.ref.off) || { off: ins.ref.off, text: ins.ref.str, uses: [] };
      e.uses.push({ fn: S.a.funcOf(ins.off), off: ins.off });
      m.set(ins.ref.off, e);
    }
  const list = [...m.values()].sort((x, y) => x.off - y.off);
  strCache = { a: S.a, list };
  return list;
}

// ---------------------------------------------------------------- navigation

function go(target, push = true) {
  if (!target) return;
  S.sel = target;
  if (push) {
    S.hist = S.hist.slice(0, S.hpos + 1);
    S.hist.push(target); S.hpos = S.hist.length - 1;
  }
  $('back').disabled = S.hpos <= 0;
  $('fwd').disabled = S.hpos >= S.hist.length - 1;
  const key = target.kind === 'func' ? 'fn=' + encodeURIComponent(fnKey(target.obj)) : 'gl=' + encodeURIComponent(target.obj.name);
  history.replaceState(null, '', `#img=${encodeURIComponent(S.path)}&${key}`);
  if (target.kind === 'func') {
    renderCode(target.obj, target.at);
    if (S.sideTab === 'data') S.sideTab = 'source';
  } else {
    S.sideTab = 'data';
  }
  tabs('side-tabs', S.sideTab);
  renderSide();
  renderList();
  const row = $('list').querySelector('.row.sel');
  if (row) row.scrollIntoView({ block: 'nearest' });
}
const fnKey = f => f.dups || f.made ? '@' + hex(f.start) : f.name;
function findFn(key) {
  if (!key) return null;
  if (key[0] === '@') return S.a.funcOf(parseInt(key.slice(1)));
  const l = S.a.byName.get(key);
  return l ? l[0] : null;
}

function fnLink(f, off) {
  if (!f) return esc(hex(off));
  const d = off !== undefined && off !== f.start ? `+${off - f.start}` : '';
  return `<a class="ref fn" data-fn="${f.index}"${d ? ` data-at="${off}"` : ''}>${esc(f.name)}${d}</a>`;
}
function glLink(g, delta = 0) {
  return `<a class="ref gl" data-gl="${g.index}">${esc(g.name)}${delta ? '+' + delta : ''}</a>`;
}

// ---------------------------------------------------------------- code

const SYS_FIRST = OP.OPEN, SYS_LAST = OP.DBG, FUSED_FIRST = OP.LDL, FUSED_LAST = OP.POPA;

function renderCode(f, at) {
  const a = S.a;
  const rows = rowsOf(f);
  const src = S.src && S.src.defs.get(f.name);
  $('code-title').textContent = f.name + (f.aliases.length ? ' = ' + f.aliases.join(', ') : '');
  $('code-info').textContent = `${hex(f.start)}, ${f.words} words, ${f.callers.size} callers`;
  let h = '';
  let prevRow = null;
  for (let k = f.first; k >= 0 && k < a.insns.length && a.insns[k].off < f.end; k++) {
    const ins = a.insns[k];
    const lb = f.labels.get(ins.off);
    const row = rows ? rowAt(rows, ins.off) : null;
    const band = row ? ` style="border-left-color:${bandColour(row.line)}" data-line="${row.file}:${row.line}"` : '';
    if (row && row !== prevRow && row.line) {
      const fm = S.a.img.smap.files[row.file];
      h += `<div class="fnhead" data-key="${row.file}:${row.line}" title="${esc(fm ? fm.path : '?')}:${row.line}">` +
        `<span class="dim">${esc(fm ? fm.path.slice(fm.path.lastIndexOf('/') + 1) : '?')}:${row.line}</span></div>`;
    }
    prevRow = row;
    h += `<div class="ins band${ins.filler ? ' word' : ''}" data-off="${ins.off}"${band}>` +
      `<span class="o">${hex(ins.off)}</span><span class="lb">${lb ? lb + ':' : ''}</span>` +
      `<span class="op${opClass(ins.op)}"${ins.filler ? '' : ` data-op="${ins.op}"`}>${esc(ins.name)}</span><span class="a">${operand(ins, f)}</span></div>`;
  }
  $('code').innerHTML = h || '<p class="note">no instructions</p>';
  if (at !== undefined) flash(at); else $('code').scrollTop = 0;
}
function opClass(op) {
  if (op >= FUSED_FIRST && op <= FUSED_LAST) return ' fused';
  if (op >= SYS_FIRST && op <= SYS_LAST && op !== OP.JSRI && op !== OP.JSRS) return ' sys';
  return '';
}
function flash(off) {
  const el = $('code').querySelector(`.ins[data-off="${off}"]`);
  if (!el) return;
  el.scrollIntoView({ block: 'center' });
  el.classList.remove('flash'); void el.offsetWidth; el.classList.add('flash');
}

function operand(ins, f) {
  if (!('arg' in ins)) return '';
  if (ins.filler) return `<span class="c">${ins.arg} (not code)</span>`;
  const r = ins.ref, n = ins.arg;
  const c = t => ` <span class="c">; ${t}</span>`;
  if (!r) {
    if (ins.op === OP.ENT) return `${n}${c(`${n} local word${n === 1 ? '' : 's'}`)}`;
    if (ins.op === OP.ADJ) return `${n}${c(`drop ${n} argument word${n === 1 ? '' : 's'}`)}`;
    return `${n}${n > 9 || n < -9 ? c(hex(n)) : ''}`;
  }
  switch (r.kind) {
    case 'label': return `<a class="ref" data-at="${r.off}">${f.labels.get(r.off)}</a>`;
    case 'func':
      return (ins.op === OP.JSR ? '' : '&amp;') + fnLink(r.func) + (ins.op === OP.JSR ? '' : c('function pointer'));
    case 'code': return fnLink(r.func, r.off);
    case 'data':
      if (r.global) {
        const g = r.global, pre = ins.op === OP.JSRI ? '*' : '&amp;';
        const s = r.delta === 0 && g.size > 1 ? stringAt(r.off) : null;
        return pre + glLink(g, r.delta) + (ins.op === OP.JSRI ? c('call through it') : s ? c(`<span class="str">${esc(JSON.stringify(s))}</span>`) : '');
      }
      if (r.str !== undefined) return `<span class="str">${esc(JSON.stringify(r.str))}</span>`;
      return `data+${r.off}`;
    case 'extern': return `${esc(r.name)}${c('unresolved: linked or loaded later')}`;
    case 'bp': {
      // c4 pushes the arguments in order, so the last is at bp+2
      const ps = paramsOf(f);
      const p = ps && r.n >= 2 && r.n - 2 < ps.length ? ps[ps.length - 1 - (r.n - 2)] : null;
      const t = p ? `<b>${esc(p)}</b>` : r.n >= 2 ? `argument ${r.n - 1} from the last` : r.n < 0 ? `local ${-r.n}` : r.n === 1 ? 'return address' : 'saved bp';
      const how = ins.op === OP.JSRS ? 'call through ' : '';
      return `${r.n >= 0 ? '[bp+' + r.n + ']' : '[bp' + r.n + ']'}${c(how + t)}`;
    }
  }
  return String(n);
}
function paramsOf(f) {
  const l = S.src && S.src.defs.get(f.name);
  if (!l) return null;
  const d = f.dups && l.length === f.dups ? l[f.nth - 1] : l[0];
  return d.params;
}
function stringAt(off) {
  const d = S.a.img.data;
  if (off >= d.length) return null;
  let s = '';
  for (let i = off; i < d.length && i < off + 80; i++) {
    const ch = d[i];
    if (ch === 0) return s.length >= 2 ? s : null;
    if (ch === 10) s += '\n'; else if (ch < 32 || ch > 126) return null; else s += String.fromCharCode(ch);
  }
  return null;
}

// Source-map rows of a function, when the image has a map
function rowsOf(f) {
  const m = S.a.img.smap;
  if (!m || !m.rows.length) return null;
  return m.rows.filter((r, i) => {
    const next = i + 1 < m.rows.length ? m.rows[i + 1].off : S.a.img.code.length;
    return next > f.start && r.off < f.end;
  });
}
function rowAt(rows, off) {
  let r = null;
  for (const x of rows) { if (x.off <= off) r = x; else break; }
  return r;
}
const bandColour = line => `hsl(${(line * 47) % 360} 70% 60%)`;

// ---------------------------------------------------------------- side pane

function renderSide() {
  const s = S.sel;
  $('src-tools').style.display = S.sideTab === 'source' ? '' : 'none';
  if (!S.a) { $('side').innerHTML = ''; return; }
  if (S.sideTab === 'refs') return renderRefs();
  if (S.sideTab === 'data') return renderData();
  renderSource(s && s.kind === 'func' ? s.obj : null);
}

function renderRefs() {
  const s = S.sel;
  if (!s || s.kind !== 'func') { $('side').innerHTML = '<p class="note">choose a function</p>'; return; }
  const f = s.obj, a = S.a;
  const list = (title, items, fmt) => `<h3>${title} (${items.length})</h3>` +
    (items.length ? `<ul>${items.map(x => `<li>${fmt(x)}</li>`).join('')}</ul>` : '<p class="note">none</p>');
  // where each caller calls from
  const sites = [];
  for (const c of f.callers)
    for (let k = c.first; k >= 0 && k < a.insns.length && a.insns[k].off < c.end; k++) {
      const r = a.insns[k].ref;
      if (r && (r.kind === 'func' || r.kind === 'code') && r.func === f) sites.push({ c, off: a.insns[k].off, op: a.insns[k].op });
    }
  const callers = [...f.callers].sort((x, y) => x.name.localeCompare(y.name));
  let h = list('called or taken by', callers, c => {
    const at = sites.filter(x => x.c === c);
    return `${fnLink(c)} <span class="dim">${at.map(x => `<a class="ref" data-fn="${c.index}" data-at="${x.off}">${x.op === OP.JSR ? 'call' : 'address'} at ${hex(x.off)}</a>`).join(', ')}</span>`;
  });
  const dataUses = a.dataRefs.filter(d => d.target.kind === 'func' && d.target.func === f);
  if (dataUses.length) h += list('stored in data', dataUses, d => d.global ? glLink(d.global, d.addr - d.global.off) : `data+${d.addr}`);
  h += list('calls', [...f.callees].sort((x, y) => x.name.localeCompare(y.name)), c => fnLink(c));
  const ext = [];
  for (let k = f.first; k >= 0 && k < a.insns.length && a.insns[k].off < f.end; k++) {
    const r = a.insns[k].ref; if (r && r.kind === 'extern' && !ext.includes(r.name)) ext.push(r.name);
  }
  if (ext.length) h += list('unresolved', ext, esc);
  h += list('globals', [...f.globals].sort((x, y) => x.name.localeCompare(y.name)), g => glLink(g));
  // the opcode mix
  const mix = new Map();
  let total = 0;
  for (let k = f.first; k >= 0 && k < a.insns.length && a.insns[k].off < f.end; k++) {
    const ins = a.insns[k]; if (ins.filler) continue;
    mix.set(ins.name, (mix.get(ins.name) || 0) + 1); total++;
  }
  const top = [...mix].sort((x, y) => y[1] - x[1]);
  const max = top.length ? top[0][1] : 1;
  h += `<h3>instructions (${total})</h3><div class="hist">` + top.map(([n, k]) =>
    `<span>${esc(n)}</span><span><div class="bar" style="width:${(100 * k / max).toFixed(1)}%"></div></span><span class="dim">${k}</span>`).join('') + '</div>';
  $('side').innerHTML = h;
}

function renderData() {
  const s = S.sel, a = S.a;
  if (!s || s.kind !== 'global') {
    $('side').innerHTML = '<p class="note">choose a global (the list\'s globals, or a purple name in the code)</p>';
    return;
  }
  const g = s.obj;
  const sym = g.sym;
  let h = `<h3>${esc(g.name)}</h3>` +
    `<p class="kv">data+${g.off} (${hex(g.off)}), ${g.size} bytes to the next symbol${g.bss ? ', <b>BSS</b> (zero at start)' : ''}</p>` +
    `<p class="kv dim">type ${sym.type}, attributes ${hex(sym.attrs)}, symbol ${sym.id}</p>`;
  const users = [...g.refs].filter(Boolean).sort((x, y) => x.name.localeCompare(y.name));
  h += `<h3>used by (${users.length})</h3>` + (users.length ? `<ul>${users.map(u => `<li>${fnLink(u)}</li>`).join('')}</ul>` : '<p class="note">none in the code</p>');
  const ptrs = a.dataRefs.filter(d => d.addr >= g.off && d.addr < g.off + g.size).sort((x, y) => x.addr - y.addr);
  if (ptrs.length) {
    h += `<h3>pointers in it (${ptrs.length})</h3><ul>` + ptrs.slice(0, 400).map(d => {
      const t = d.target;
      const to = t.kind === 'func' ? '&amp;' + fnLink(t.func, t.off)
        : t.kind === 'data' ? (t.global ? '&amp;' + glLink(t.global, t.off - t.global.off) : (stringAt(t.off) !== null ? `<span class="str">${esc(JSON.stringify(stringAt(t.off)))}</span>` : `data+${t.off}`))
        : esc(t.name);
      return `<li><span class="dim">+${d.addr - g.off}</span> ${to}</li>`;
    }).join('') + (ptrs.length > 400 ? `<li class="dim">&hellip; ${ptrs.length - 400} more</li>` : '') + '</ul>';
  }
  if (!g.bss) {
    const d = a.img.data, n = Math.min(g.size, 512);
    const isPtr = new Set(ptrs.map(p => p.addr));
    let x = '';
    for (let i = 0; i < n; i += 16) {
      let hexs = '', asc = '';
      for (let j = 0; j < 16; j++) {
        const o = g.off + i + j;
        if (i + j >= n || o >= d.length) { hexs += '   '; continue; }
        const b = d[o].toString(16).padStart(2, '0');
        hexs += (isPtr.has(o) || isPtr.has(o - 1) || isPtr.has(o - 2) || isPtr.has(o - 3)) ? `<span class="hi">${b}</span> ` : b + ' ';
        asc += d[o] >= 32 && d[o] < 127 ? esc(String.fromCharCode(d[o])) : '.';
      }
      x += `${(i).toString(16).padStart(4, '0')}  ${hexs} ${asc}\n`;
    }
    h += `<h3>bytes${g.size > n ? ` (the first ${n})` : ''}</h3><div class="hex">${x}</div>` +
      (ptrs.length ? '<p class="note">yellow: a pointer, patched when the image loads (the bytes are the offset before it)</p>' : '');
  }
  $('side').innerHTML = h;
}

// ---------------------------------------------------------------- source

// Each .c in the given directories, scanned for function definitions.
async function scanSources() {
  const dirs = S.srcDirs.split(/\s+/).filter(Boolean).map(d => d.endsWith('/') ? d : d + '/');
  const want = S.srcDirs;
  const idx = { defs: new Map(), files: new Map(), dirs, missing: [] };
  S.src = idx;
  let n = 0;
  for (const d of dirs) {
    const l = await listDir(d);
    if (!l) { idx.missing.push(d); continue; }
    const cs = l.filter(e => !e.dir && /\.c$/.test(e.name));
    await Promise.all(cs.map(async e => {
      try {
        const text = await (await fetch(new URL(d + e.name, ROOT))).text();
        if (S.srcDirs !== want) return;
        const path = d + e.name;
        idx.files.set(path, { path, text, html: null });
        for (const def of findDefs(text)) {
          const l2 = idx.defs.get(def.name) || [];
          l2.push({ ...def, path });
          idx.defs.set(def.name, l2);
        }
        status(`scanning sources: ${++n} files`);
      } catch { /* unreadable */ }
    }));
  }
  if (S.srcDirs !== want) return;
  const named = S.a.funcs.filter(f => !f.made);
  const found = named.filter(f => idx.defs.has(f.name)).length;
  status(`sources: ${idx.files.size} files, ${found} of ${named.length} functions found${idx.missing.length ? `; no listing for ${idx.missing.join(' ')}` : ''}`);
  if (S.sel && S.sel.kind === 'func') renderCode(S.sel.obj);
  if (S.sideTab === 'source') renderSide();
}

// Function definitions in C: at file scope, a name, its parameters, and
// then a body (with K&R declarations between, perhaps). Comments, strings
// and preprocessor lines are blanked first, newlines kept.
export function findDefs(text) {
  const c = blank(text);
  const lineAt = [];
  let line = 1;
  for (let i = 0; i < c.length; i++) { lineAt.push(line); if (c[i] === '\n') line++; }
  lineAt.push(line);
  const out = [];
  let depth = 0;
  const match = (i, o, cl) => { let d = 0; for (; i < c.length; i++) { if (c[i] === o) d++; else if (c[i] === cl && --d === 0) return i; } return -1; };
  for (let i = 0; i < c.length; i++) {
    const ch = c[i];
    if (ch === '{') depth++;
    else if (ch === '}') depth = Math.max(0, depth - 1);
    else if (ch === '(' && depth === 0) {
      let j = i - 1;
      while (j >= 0 && /\s/.test(c[j])) j--;
      let k = j;
      while (k >= 0 && /\w/.test(c[k])) k--;
      const name = c.slice(k + 1, j + 1);
      const close = match(i, '(', ')');
      if (close < 0) break;
      if (!/^[A-Za-z_]\w*$/.test(name) || /^(if|while|for|switch|return|sizeof|__attribute__)$/.test(name)) { i = close; continue; }
      let m = close + 1;
      while (m < c.length && /\s/.test(c[m])) m++;
      if (c[m] !== '{' && !/[A-Za-z_]/.test(c[m] || '')) { i = close; continue; }
      let b = m;
      if (c[m] !== '{') {          // K&R: declarations up to the body
        b = c.indexOf('{', m);
        if (b < 0 || /[()=]/.test(c.slice(m, b)) || !/;\s*$/.test(c.slice(m, b))) { i = close; continue; }
      }
      const end = match(b, '{', '}');
      if (end < 0) break;
      out.push({ name, line: lineAt[k + 1], end: lineAt[end], params: params(c.slice(i + 1, close)) });
      i = end;
    }
  }
  return out;
}
// The parameters' names, in order: the last name in each declaration
// (int (*fp)(int) gives fp), or K&R's bare list. null for varargs.
function params(t) {
  if (/^\s*(void)?\s*$/.test(t)) return [];
  const out = [];
  let d = 0, cur = '';
  for (const ch of t + ',') {
    if (ch === '(' || ch === '[') d++;
    else if (ch === ')' || ch === ']') d--;
    if (ch === ',' && d === 0) {
      if (/\.\.\./.test(cur)) return null;
      const fp = cur.match(/\(\s*\*\s*(\w+)\s*\)/);
      const ids = cur.replace(/\[[^\]]*\]/g, '').match(/[A-Za-z_]\w*/g) || [];
      out.push(fp ? fp[1] : ids[ids.length - 1] || '?');
      cur = '';
    } else cur += ch;
  }
  return out;
}

function blank(t) {
  const o = t.split('');
  let i = 0, bol = true;
  while (i < t.length) {
    const ch = t[i];
    if (bol && /[ \t]/.test(ch)) { i++; continue; }
    if (bol && ch === '#') {          // a preprocessor line, with its continuations
      while (i < t.length && t[i] !== '\n') { if (t[i] === '\\' && t[i + 1] === '\n') { o[i] = ' '; i += 2; continue; } o[i] = ' '; i++; }
      continue;
    }
    bol = false;
    if (ch === '\n') { bol = true; i++; continue; }
    if (ch === '/' && t[i + 1] === '*') {
      const e = t.indexOf('*/', i + 2), stop = e < 0 ? t.length : e + 2;
      for (; i < stop; i++) if (o[i] !== '\n') o[i] = ' ';
      continue;
    }
    if (ch === '/' && t[i + 1] === '/') { while (i < t.length && t[i] !== '\n') o[i++] = ' '; continue; }
    if (ch === '"' || ch === "'") {
      o[i++] = ' ';
      while (i < t.length && t[i] !== ch && t[i] !== '\n') { if (t[i] === '\\') o[i++] = ' '; o[i++] = ' '; }
      if (i < t.length && t[i] === ch) o[i++] = ' ';
      continue;
    }
    i++;
  }
  return o.join('');
}

function renderSource(f) {
  const side = $('side');
  const m = S.a.img.smap;
  if (!S.srcDirs && !m) {
    side.innerHTML = '<p class="note">Name the directories this image\'s C came from, above, and each function\'s source is found by its definition.</p>' + noMap();
    return;
  }
  if (!S.src && !m) { side.innerHTML = '<p class="note">scanning&hellip;</p>'; return; }
  if (!f) { side.innerHTML = '<p class="note">choose a function</p>'; return; }
  if (m && m.rows.length) { showMapped(f); return; }
  byName(f);
}
function byName(f) {
  const side = $('side');
  const cands = (S.src && S.src.defs.get(f.name)) || [];
  if (!cands.length) {
    side.innerHTML = `<p class="note">${f.made ? 'This body has no symbol: the compiler made it (a helper, or a stub), or its symbol was stripped.'
      : `No definition of <b>${esc(f.name)}</b> in ${esc(S.srcDirs || 'the directories given')}.` +
        ' It may come from another directory (libc, say), or from a macro.'}</p>` + noMap();
    return;
  }
  // a static name in several units: the nth body of that name, if the
  // count matches, else the first
  const pick = f.dups && cands.length === f.dups ? cands[f.nth - 1] : cands[0];
  showFile(pick, f, cands);
}
function noMap() {
  return S.a.img.smap ? '' : '<p class="note">Line by line needs a source map in the image (track S, docs/c4-explorer.md 2); this one has none, so a function is matched to its source by name, as a whole.</p>';
}

function showFile(def, f, cands) {
  const file = S.src.files.get(def.path);
  if (!file.html) file.html = highlight(file.text);
  const starts = new Map();
  for (const [name, l] of S.src.defs) for (const d of l) if (d.path === def.path) starts.set(d.line, name);
  const lines = file.html.split('\n');
  const head = `<div class="srchead"><b>${esc(def.path)}</b>:${def.line}-${def.end}` +
    (cands.length > 1 ? ` &middot; also ${cands.filter(c => c !== def).map(c => `${esc(c.path.slice(c.path.lastIndexOf('/') + 1))}:${c.line}`).join(', ')}` : '') + '</div>';
  side_html(head + '<div class="src">' + lines.map((l, i) => {
    const n = i + 1, s = starts.get(n);
    const cls = n >= def.line && n <= def.end ? ' in' : '';
    return `<div class="ln${cls}" id="L${n}"><span class="no${s ? ' has' : ''}"${s ? ` data-def="${esc(s)}" title="${esc(s)}"` : ''}>${n}</span><span>${l || ' '}</span></div>`;
  }).join('') + '</div>');
  const el = document.getElementById('L' + def.line);
  if (el) el.scrollIntoView({ block: 'start' });
  linkNames();
}
function side_html(h) { $('side').innerHTML = h; }

// ---- line by line, from the image's source map ----
//
// The function's rows say which lines its code came from. The file is
// fetched from the server by the path the compiler was given (relative
// to the repository when the build ran there), and its size and CRC are
// checked against the map's, so a source changed since is said to be.
const CRC = (() => {
  const t = new Uint32Array(256);
  for (let n = 0; n < 256; n++) { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1; t[n] = c >>> 0; }
  return b => { let c = 0xffffffff; for (let i = 0; i < b.length; i++) c = t[(c ^ b[i]) & 255] ^ (c >>> 8); return (c ^ 0xffffffff) >>> 0; };
})();
const srcCache = new Map();
async function loadSrc(path) {
  if (srcCache.has(path)) return srcCache.get(path);
  let file = null;
  try {
    const r = await fetch(new URL(path, ROOT));
    const ct = r.headers.get('content-type') || '';
    if (r.ok && !ct.includes('html')) {
      const bytes = new Uint8Array(await r.arrayBuffer());
      file = { path, text: new TextDecoder('latin1').decode(bytes), html: null, size: bytes.length, crc: CRC(bytes) };
    }
  } catch { /* not served */ }
  srcCache.set(path, file);
  return file;
}

async function showMapped(f) {
  const m = S.a.img.smap;
  const rows = (rowsOf(f) || []).filter(r => r.line > 0 && r.off >= f.start && r.off < f.end);
  const fe = m.funcs.find(x => x.start === f.start);
  let fi = fe && fe.line > 0 ? fe.file : null;
  if (fi === null || !rows.some(r => r.file === fi)) {
    const count = new Map();
    for (const r of rows) count.set(r.file, (count.get(r.file) || 0) + 1);
    fi = count.size ? [...count].sort((a, b) => b[1] - a[1])[0][0] : null;
  }
  if (fi === null) {
    side_html(`<p class="note">${esc(f.name)} has no source lines in the map: the compiler made it${fe ? '' : ', or it came from an object without a map'}.</p>`);
    return;
  }
  const meta = m.files[fi];
  side_html(`<p class="note">reading ${esc(meta.path)}&hellip;</p>`);
  const file = await loadSrc(meta.path);
  if (!S.sel || S.sel.obj !== f || S.sideTab !== 'source') return;   // moved on meanwhile
  if (!file) {
    side_html(`<p class="note">The map says <b>${esc(meta.path)}</b>, which the server does not have` +
      ' (a path relative to where the compiler ran, or a file of the compiler\'s own).</p>');
    return;
  }
  if (!file.html) file.html = highlight(file.text);
  // every line of this file with a row anywhere in the image, and this
  // function's own
  const anyRow = new Map();
  m.rows.forEach((r, i) => { if (r.file === fi && r.line > 0 && !anyRow.has(r.line)) anyRow.set(r.line, r.off); });
  const mine = new Set(rows.filter(r => r.file === fi).map(r => r.line));
  const lo = Math.min(...mine), hi = Math.max(...mine);
  const stale = (meta.size && file.size !== meta.size) || (m.flags & 1 && meta.crc !== null && meta.crc !== file.crc);
  const lines = file.html.split('\n');
  side_html(`<div class="srchead"><b>${esc(meta.path)}</b>:${lo}-${hi}` +
    (stale ? ' &middot; <span style="color:var(--pink)">changed since it was compiled: the lines may not match</span>' : ' &middot; as compiled') +
    '</div><div class="src">' + lines.map((l, i) => {
      const n = i + 1, has = anyRow.has(n);
      const cls = mine.has(n) ? ' in' : '';
      const band = mine.has(n) ? ` style="box-shadow:3px 0 0 ${bandColour(n)} inset"` : '';
      return `<div class="ln${cls}" id="L${n}"${band}><span class="no${has ? ' has' : ''}"${has ? ` data-srcline="${fi}:${n}" title="the code from this line"` : ''}>${n}</span><span>${l || ' '}</span></div>`;
    }).join('') + '</div>');
  S.shown = { fi, path: meta.path };
  const el = document.getElementById('L' + (fe && fe.file === fi ? fe.line : lo));
  if (el) el.scrollIntoView({ block: 'start' });
  linkNames();
  // the code pane's row headings get the line itself, as objdump -S
  const src = file.text.split('\n');
  for (const h of $('code').querySelectorAll(`.fnhead[data-key^="${fi}:"]`)) {
    const n = +h.dataset.key.split(':')[1];
    h.innerHTML = `<span class="dim">${n}</span>  ${esc((src[n - 1] || '').trim())}`;
  }
  if (S.sel.line) markLine(S.sel.line, $('code').querySelectorAll(`.ins[data-line="${S.sel.line}"]`));
}

// a source line clicked: its instructions, in this function or the one
// they are in
function gotoLine(key) {
  const [fi, line] = key.split(':').map(Number);
  const m = S.a.img.smap;
  const here = $('code').querySelectorAll(`.ins[data-line="${key}"]`);
  if (here.length) { markLine(key, here); return; }
  const r = m.rows.find(x => x.file === fi && x.line === line);
  if (r) go({ kind: 'func', obj: S.a.funcOf(r.off), at: r.off, line: key });
}
function markLine(key, els, scroll = true) {
  for (const x of $('code').querySelectorAll('.ins.hl')) x.classList.remove('hl');
  for (const x of els) x.classList.add('hl');
  if (scroll && els.length) els[0].scrollIntoView({ block: 'center' });
  for (const x of $('side').querySelectorAll('.ln.cur')) x.classList.remove('cur');
  const [fi, line] = key.split(':').map(Number);
  if (S.shown && S.shown.fi === fi) {
    const el = document.getElementById('L' + line);
    if (el) { el.classList.add('cur'); el.scrollIntoView({ block: 'nearest' }); }
  }
}

// Names in the source that are functions in the image become links
function linkNames() {
  for (const el of $('side').querySelectorAll('.src .id')) {
    const l = S.a.byName.get(el.textContent);
    if (l) { el.classList.add("ref", "fn"); el.dataset.fn = l[0].index; }
  }
}

const KW = new Set(('auto break case char const continue default do double else enum extern float for goto if inline int long ' +
  'register restrict return short signed sizeof static struct switch typedef union unsigned void volatile while').split(' '));
function highlight(t) {
  let o = '', i = 0, bol = true;
  while (i < t.length) {
    const ch = t[i];
    if (ch === '\n') { o += '\n'; i++; bol = true; continue; }
    if (bol && ch === '#') { let j = t.indexOf('\n', i); if (j < 0) j = t.length; o += `<span class="pp">${esc(t.slice(i, j))}</span>`; i = j; continue; }
    if (ch !== ' ' && ch !== '\t') bol = false;
    if (ch === '/' && t[i + 1] === '*') {
      let j = t.indexOf('*/', i + 2); j = j < 0 ? t.length : j + 2;
      o += t.slice(i, j).split('\n').map(x => `<span class="cm">${esc(x)}</span>`).join('\n'); i = j; continue;
    }
    if (ch === '/' && t[i + 1] === '/') { let j = t.indexOf('\n', i); if (j < 0) j = t.length; o += `<span class="cm">${esc(t.slice(i, j))}</span>`; i = j; continue; }
    if (ch === '"' || ch === "'") {
      let j = i + 1;
      while (j < t.length && t[j] !== ch && t[j] !== '\n') j += t[j] === '\\' ? 2 : 1;
      j = Math.min(j + 1, t.length);
      o += `<span class="st">${esc(t.slice(i, j))}</span>`; i = j; continue;
    }
    if (/[A-Za-z_]/.test(ch)) {
      let j = i + 1; while (j < t.length && /\w/.test(t[j])) j++;
      const w = t.slice(i, j);
      o += KW.has(w) ? `<span class="kw">${w}</span>` : `<a class="id">${w}</a>`;
      i = j; continue;
    }
    o += esc(ch); i++;
  }
  return o;
}

// ---------------------------------------------------------------- the rest

function tabs(id, which) {
  for (const b of $(id).querySelectorAll('button')) b.classList.toggle('active', b.dataset.tab === which);
}
function status(t, err) { const s = $('status'); s.textContent = t; s.style.color = err ? 'var(--pink)' : ''; }

function gotoText(t) {
  t = t.trim(); if (!t || !S.a) return;
  const a = S.a;
  let off = null;
  if (t[0] === '@') {
    const addr = parseInt(t.slice(1)), base = parseInt($('base').value) || 0;
    if (!isNaN(addr)) off = Math.floor((addr - base) / 4);
  } else if (/^(0x[0-9a-f]+|\d+)$/i.test(t)) off = parseInt(t);
  if (off !== null) {
    const f = a.funcOf(off);
    if (!f) { status(`${t}: not in the code (${a.img.code.length} words)`, true); return; }
    const ins = a.insns[a.at[off] >= 0 ? a.at[off] : a.at[off - 1]];
    go({ kind: 'func', obj: f, at: ins ? ins.off : off });
    return;
  }
  const fl = a.byName.get(t);
  if (fl) { go({ kind: 'func', obj: fl[0] }); return; }
  const g = a.globals.find(x => x.name === t);
  if (g) { go({ kind: 'global', obj: g }); return; }
  const re = new RegExp(t.replace(/[.*+?^${}()|[\]\\]/g, '\\$&'), 'i');
  const f2 = a.funcs.find(x => re.test(x.name));
  if (f2) go({ kind: 'func', obj: f2 }); else status(`nothing called ${t}`, true);
}

document.addEventListener('click', e => {
  const sl = e.target.closest('[data-srcline]');
  if (sl && S.a) { gotoLine(sl.dataset.srcline); return; }
  const el = e.target.closest('[data-fn],[data-gl],[data-at],[data-def],.row,[data-tab]');
  if (!el || !S.a) return;
  if (el.dataset.tab) {
    const box = el.parentElement.id;
    if (box === 'list-tabs') { S.listTab = el.dataset.tab; tabs(box, S.listTab); renderList(); }
    else { S.sideTab = el.dataset.tab; tabs(box, S.sideTab); renderSide(); }
    return;
  }
  if (el.classList.contains('row')) {
    if (el.dataset.f) go({ kind: 'func', obj: S.a.funcs[+el.dataset.f] });
    else if (el.dataset.g) go({ kind: 'global', obj: S.a.globals[+el.dataset.g] });
    else if (el.dataset.s) {
      const s = strings().find(x => x.off === +el.dataset.s);
      if (s) go({ kind: 'func', obj: s.uses[0].fn, at: s.uses[0].off });
    }
    return;
  }
  if (el.dataset.def) { const f = findFn(el.dataset.def); if (f) go({ kind: 'func', obj: f }); return; }
  if (el.dataset.fn) {
    const f = S.a.funcs[+el.dataset.fn];
    go({ kind: 'func', obj: f, at: el.dataset.at !== undefined ? +el.dataset.at : undefined });
    return;
  }
  if (el.dataset.gl) { go({ kind: 'global', obj: S.a.globals[+el.dataset.gl] }); return; }
  if (el.dataset.at) flash(+el.dataset.at);
});
// clicking an instruction marks it, and its source row with a map
$('code').addEventListener('click', e => {
  const ins = e.target.closest('.ins');
  if (!ins || e.target.closest('a')) return;
  const key = ins.dataset.line;
  if (key) { markLine(key, $('code').querySelectorAll(`.ins[data-line="${key}"]`), false); return; }
  for (const x of $('code').querySelectorAll('.ins.hl')) x.classList.remove('hl');
  ins.classList.add('hl');
});

// hovering a mnemonic explains the instruction (opdoc.js), as Compiler
// Explorer explains an x86 one
const tip = document.createElement('div');
tip.id = 'optip';
tip.setAttribute('role', 'tooltip');
document.body.appendChild(tip);
$('code').addEventListener('mouseover', e => {
  const el = e.target.closest('.op[data-op]');
  if (!el) return;
  tip.innerHTML = opDoc(+el.dataset.op, el.textContent);
  tip.style.display = 'block';
  const r = el.getBoundingClientRect(), w = tip.offsetWidth, h = tip.offsetHeight;
  const x = Math.max(8, Math.min(r.left, innerWidth - w - 8));
  tip.style.left = x + 'px';
  tip.style.top = (r.bottom + 6 + h < innerHeight ? r.bottom + 6 : Math.max(8, r.top - h - 6)) + 'px';
});
$('code').addEventListener('mouseout', e => { if (e.target.closest('.op[data-op]')) tip.style.display = 'none'; });
$('code').addEventListener('scroll', () => { tip.style.display = 'none'; });

$('images').addEventListener('change', e => { if (e.target.value) openPath(e.target.value); });
$('path').addEventListener('keydown', e => { if (e.key === 'Enter') openPath($('path').value.trim()); });
$('goto').addEventListener('keydown', e => { if (e.key === 'Enter') gotoText(e.target.value); });
$('filter').addEventListener('input', renderList);
$('sort').addEventListener('change', renderList);
$('srcload').addEventListener('click', () => { S.srcDirs = $('srcdirs').value.trim(); store.set('src:' + S.path, S.srcDirs); scanSources(); });
$('srcdirs').addEventListener('keydown', e => { if (e.key === 'Enter') $('srcload').click(); });
$('back').addEventListener('click', () => { if (S.hpos > 0) go(S.hist[--S.hpos], false); });
$('fwd').addEventListener('click', () => { if (S.hpos < S.hist.length - 1) go(S.hist[++S.hpos], false); });
document.addEventListener('keydown', e => {
  if (e.altKey && e.key === 'ArrowLeft') { $('back').click(); e.preventDefault(); }
  if (e.altKey && e.key === 'ArrowRight') { $('fwd').click(); e.preventDefault(); }
  if (e.key === '/' && document.activeElement.tagName !== 'INPUT') { $('filter').focus(); e.preventDefault(); }
});
$('file').addEventListener('change', async e => {
  const f = e.target.files[0]; if (f) openBytes(new Uint8Array(await f.arrayBuffer()), f.name);
});
document.addEventListener('dragover', e => { e.preventDefault(); document.body.classList.add('drop'); });
document.addEventListener('dragleave', e => { if (!e.relatedTarget) document.body.classList.remove('drop'); });
document.addEventListener('drop', async e => {
  e.preventDefault(); document.body.classList.remove('drop');
  const f = e.dataTransfer.files[0]; if (f) openBytes(new Uint8Array(await f.arrayBuffer()), f.name);
});

// #img=PATH&fn=NAME|@0xOFF or &gl=NAME
function fromHash() {
  const h = new URLSearchParams(location.hash.slice(1));
  const img = h.get('img') || store.get('path', 'src/c4bb/images/hello32.c4r');
  const pick = () => {
    if (h.get('gl')) { const g = S.a.globals.find(x => x.name === h.get('gl')); if (g) return go({ kind: 'global', obj: g }); }
    const f = findFn(h.get('fn'));
    if (f) return go({ kind: 'func', obj: f });
    const main = S.a.byName.get('main');
    go({ kind: 'func', obj: main ? main[0] : S.a.funcs[0] });
  };
  if (img === S.path && S.a) pick(); else openPath(img, pick);
}
window.addEventListener('hashchange', () => {
  const h = new URLSearchParams(location.hash.slice(1));
  const key = S.sel ? (S.sel.kind === 'func' ? 'fn=' + encodeURIComponent(fnKey(S.sel.obj)) : 'gl=' + encodeURIComponent(S.sel.obj.name)) : '';
  if (h.get('img') !== S.path || !location.hash.endsWith(key)) fromHash();
});

fillImages();
fromHash();
