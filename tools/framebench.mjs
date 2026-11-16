#!/usr/bin/env node
// framebench.mjs -- how a game's frames go: the frame rate's low, average
// and high, its 1% and 0.1% lows, the worst frame's time, and the VM
// instructions a frame (docs/framebench.md).
//
//   node tools/framebench.mjs [options] RUN [-- GAME ARGS...]
//
// RUN is one of the runs below (--list prints them). Two places to run:
//
//   node (the default)   the bare machine under node, on the game's virtual
//                        clock (-sum 0 and the like), so every run of an
//                        image counts the same instructions. A frame ends
//                        where the game calls its frame function (each
//                        game's *-sum.c), caught at the call, so a frame's
//                        count is exact. Frame rates are given at --mips
//                        (a host that runs that many million instructions
//                        a second) and by node's own clock.
//   --page CDP           the page, in a tab of its own in the Chrome whose
//                        DevTools port is CDP (http://127.0.0.1:9222), C4IX
//                        at its shell: the game is typed there with -show,
//                        and each frame is timed as the page shows it.
//
// Options:
//   --data DIR       the person's own files, laid over the disk (duke3d's
//                    Atomic Edition: DUKE3D.GRP and the demos). Read from DIR
//                    as they are; nothing is copied anywhere
//   --mips LIST      frame rates at these speeds, in M instructions a
//                    second (default 100,250)
//   --skip N         frames left out at the start, beyond the first (the boot)
//   --worst N        the worst frames listed (default 5)
//   --json FILE      every frame's numbers, for a later look
//   --profile        node: where the heaviest 1% of frames' instructions go,
//                    against the median frames', by function (a second run,
//                    its pc sampled every --every instructions, default 97,
//                    and every call counted by its target)
//   --leave A-B,...  frames the profile leaves out (Duke's demo1 dies at
//                    1949-2782, a desync of the game's versions, not play)
//   --image FILE     another build of the game's image
//   --lines RE       node: the game's output lines that match RE, printed
//   --no-accel       no drawing accelerators (libjs/accel.js): the game draws
//                    with its own loops; the page: &accel=0
//   --out DIR        node: the files the game writes go to DIR (duke3d's
//                    -shot N frames, as shotN.ppm)
//   page only:
//   --base URL       where the page is served (https://8497.code.home.stargazer.onl)
//   --query Q        the page's query (default mhz=400&mmu=1)
//   --lag N          instead of a timedemo: the game played, a turn key held
//                    N times, and the time from each key's press to the
//                    first frame shown that differs from the one before it
//   --like FILE      the frames a node run's --json counted (duke3d's demos)
//   --again FILE     a page run's --json read again, not run (with --like)
//   --timeout S      give up after S seconds (default 1800)
import { registerHooks } from 'node:module';
import { readFileSync, writeFileSync, existsSync, statSync, readdirSync } from 'node:fs';
import { join, basename, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..');
const GAMES = join(ROOT, 'src/c4bb/images/games');

// ---- the runs ----
// image, its arguments, the function each frame calls (where a frame ends),
// and for duke3d the function that opens a demo (where a demo starts).
// What each timedemo counts besides drawing is said in docs/framebench.md:
// Doom, Wolf3D and ROTT checksum every frame in a timedemo, which costs
// 0.2 to 0.4 M instructions a frame; Duke and Quake do not with -sum 0.
const RUNS = {
  'duke3d': { image: 'duke3d.c4r', frame: 'duke_sum_frame', mark: 'opendemoread', marks: 3, mem: 256, data: true,
              args: ['-home', '-sum', '0', '/ns', '/nm'], page: 'duke3d -home -sum 0 -show /ns /nm',
              about: "the Atomic Edition's three demos, one after another, with --data (the person's GRP)" },
  'duke3d-sw': { image: 'duke3d.c4r', frame: 'duke_sum_frame', mem: 64, skip: 1,
                 args: ['/v1', '/l1', '/s2', '/ns', '/nm', '-sum', '0', '-frames', '600', '-script'],
                 page: 'duke3d /v1 /l1 /s2 /ns /nm -sum 0 -frames 600 -script -show',
                 about: "test-duke3d's E1L1 script on the shareware GRP, 600 frames" },
  ...Object.fromEntries([1, 2, 3].map(n => [`quake-demo${n}`, {
    image: 'quake.c4r', frame: 'qg_sum_frame', mem: 128, skip: 8,
    args: ['-nosound', '-nocdaudio', '-sum', '0', '+timedemo', `demo${n}`],
    page: `quake -nosound -nocdaudio -sum 0 -show +timedemo demo${n}`,
    about: `Quake's timedemo demo${n} (shareware)` }])),
  ...Object.fromEntries([1, 2, 3].map(n => [`doom-demo${n}`, {
    image: 'doom.c4r', frame: 'dg_frame', mem: 64, args: ['-timedemo', `demo${n}`, '-nosound'],
    page: `doom -timedemo demo${n} -nosound -show`, about: `Doom's -timedemo demo${n} (shareware)` }])),
  'wolf3d': { image: 'wolf3d.c4r', frame: 'c4sdl_present', mem: 64, skip: 1, args: ['--timedemo', '1'],
              page: 'wolf3d --timedemo 1 --show', about: "Wolfenstein 3D's --timedemo 1 (shareware)" },
  'rott': { image: 'rott.c4r', frame: 'rott_frame', mem: 64, skip: 2, args: ['--timedemo', '4', 'NOSOUND'],
            page: 'rott --timedemo 4 NOSOUND --show', about: "Rise of the Triad's --timedemo 4 (shareware)" },
  ...Object.fromEntries([1, 2, 3].map(n => [`rott-demo${n}`, {
    image: 'rott.c4r', frame: 'rott_frame', mem: 64, skip: 2, args: ['--timedemo', `${n}`, 'NOSOUND'],
    page: `rott --timedemo ${n} NOSOUND --show`, about: `Rise of the Triad's --timedemo ${n} (shareware)` }])),
};
// the games played for --lag: started at the shell, then a turn key held
const LAG = {
  'duke3d-sw': { page: 'duke3d /v1 /l1 /s2', settle: 25 },
  'duke3d': { page: 'duke3d -home /v1 /l1 /s2', settle: 25 },
  'quake-demo1': { page: 'quake +map e1m1', settle: 40 },
  'doom-demo1': { page: 'doom -warp 1 1', settle: 25 },
};

// ---- the command line ----
const argv = process.argv.slice(2);
const o = { mips: [100, 250], skip: null, worst: 5, every: 97, query: 'mhz=400&mmu=1',
            base: 'https://8497.code.home.stargazer.onl', timeout: 1800, extra: [] };
for (let i = 0; i < argv.length; i++) {
  const a = argv[i], v = () => argv[++i];
  if (a === '--') { o.extra = argv.slice(i + 1); break; }
  else if (a === '--data') o.data = v();
  else if (a === '--mips') o.mips = v().split(',').map(Number);
  else if (a === '--skip') o.skip = +v();
  else if (a === '--worst') o.worst = +v();
  else if (a === '--json') o.json = v();
  else if (a === '--profile') o.profile = true;
  else if (a === '--leave') o.leave = v().split(',').map(r => r.split('-').map(Number));
  else if (a === '--every') o.every = +v();
  else if (a === '--image') o.image = v();
  else if (a === '--no-accel') o.accel = false;
  else if (a === '--lines') o.lines = new RegExp(v());
  else if (a === '--out') o.out = v();
  else if (a === '--page') o.page = v();
  else if (a === '--base') o.base = v().replace(/\/$/, '');
  else if (a === '--query') o.query = v();
  else if (a === '--lag') o.lag = +v();
  else if (a === '--timeout') o.timeout = +v();
  else if (a === '--like') o.like = v();
  else if (a === '--again') { o.again = v(); o.page = o.page || 'again'; }
  else if (a === '--list') { for (const [k, r] of Object.entries(RUNS)) console.log(`${k.padEnd(12)} ${r.about}`); process.exit(0); }
  else if (a.startsWith('-')) { console.error(`framebench: unknown option ${a}`); process.exit(2); }
  else o.run = a;
}
const R = RUNS[o.run];
if (!R) { console.error(`usage: framebench.mjs [options] RUN [-- ARGS...]; RUN one of ${Object.keys(RUNS).join(', ')}`); process.exit(2); }
if (R.data && !o.data && !o.again) { console.error(`framebench: ${o.run} needs --data DIR, the person's files`); process.exit(2); }

// ---- the numbers ----
const fmt = (x, d = 1) => Number.isFinite(x) ? x.toFixed(d) : '-';
const sorted = a => Float64Array.from(a).sort();
const pct = (s, p) => s[Math.min(s.length - 1, Math.max(0, Math.ceil(p / 100 * s.length) - 1))];
// frame rates from frame times in ms: the low is the slowest frame's, the
// high the fastest's, the average the frames over their time; a 1% low is
// the rate of the slowest 1% of frames taken together (at least one frame)
function rates(ms) {
  const s = sorted(ms), n = s.length, sum = s.reduce((x, y) => x + y, 0);
  const low = f => { const k = Math.max(1, Math.ceil(n * f)); let t = 0; for (let i = n - k; i < n; i++) t += s[i]; return 1000 * k / t; };
  return { frames: n, low: 1000 / s[n - 1], avg: 1000 * n / sum, high: 1000 / s[0], low1: low(0.01), low01: low(0.001), worst: s[n - 1] };
}
function rateRow(name, r) {
  return `${name.padEnd(26)}${fmt(r.low).padStart(7)}${fmt(r.low01).padStart(9)}${fmt(r.low1).padStart(8)}` +
         `${fmt(r.avg).padStart(8)}${fmt(r.high).padStart(8)}${(fmt(r.worst, 0) + ' ms').padStart(11)}`;
}
const rateHead = `${'frame rate'.padEnd(26)}${'low'.padStart(7)}${'0.1% low'.padStart(9)}${'1% low'.padStart(8)}${'avg'.padStart(8)}${'high'.padStart(8)}${'worst'.padStart(11)}`;
function instrRows(ins, first) {
  const s = sorted(ins);
  const worst = ins.map((v, i) => [v, i + first]).sort((a, b) => b[0] - a[0]).slice(0, o.worst);
  return [`M instructions a frame: min ${fmt(s[0] / 1e6, 3)}, median ${fmt(pct(s, 50) / 1e6, 3)}, ` +
          `p99 ${fmt(pct(s, 99) / 1e6, 3)}, max ${fmt(s[s.length - 1] / 1e6, 3)}, mean ${fmt(s.reduce((x, y) => x + y, 0) / s.length / 1e6, 3)}`,
          `worst frames: ${worst.map(([v, f]) => `#${f} ${fmt(v / 1e6, 2)} M`).join(', ')}`];
}

// ---- node: the bare machine, every instruction counted ----
// A copy of c4m.js as it loads, with a look at each call's target (JSR,
// JSRI, JSRS): FB.on marks the functions whose calls are timed, FB.calls
// (when profiling) counts every call by its target. Nothing is written to
// the tree. If c4m.js's call lines change, this says so rather than
// measuring nothing.
const FB = globalThis.__framebench = { base: 0, n: 0, on: new Uint8Array(1), starts: new Uint8Array(1), calls: null, hit: null };
function hookC4m(src) {
  const look = ' if ((x = (pc - FB.base) >>> 2) < FB.n) { if (FB.calls !== null) FB.calls[x]++; if (FB.on[x] !== 0) FB.hit(x, cycle, this); }';
  for (const line of ['case 3:  /* JSR */ sp -= 4; i32[sp >> 2] = pc + 4; pc = i32[pc >> 2];',
                      'case 61: /* JSRI */ x = i32[pc >> 2]; sp -= 4; i32[sp >> 2] = pc + 4; pc = this.ld32(x);',
                      'case 62: /* JSRS */ x = i32[pc >> 2]; sp -= 4; i32[sp >> 2] = pc + 4; pc = i32[(bp >> 2) + x];']) {
    if (src.split(line).length !== 2) throw new Error(`framebench: libjs/c4m.js no longer has "${line}"; its hook needs updating`);
    src = src.replace(line, line + look);
  }
  // c4lc calls through an expression with JMPA, the return address pushed
  // first (c4lc-gen.lisp g:callexpr); a switch's jump table jumps with it
  // too. A JMPA counts as a call where it lands on a function's start
  // (FB.starts)
  const jmpa = 'case 63: /* JMPA */ pc = a;';
  if (src.split(jmpa).length !== 2) throw new Error(`framebench: libjs/c4m.js no longer has "${jmpa}"; its hook needs updating`);
  src = src.replace(jmpa, jmpa + ' if (FB.calls !== null && (x = (pc - FB.base) >>> 2) < FB.n && FB.starts[x] !== 0) FB.calls[x]++;');
  return 'const FB = globalThis.__framebench;\n' + src;
}

// the disk: the games disk, with --data's files over it, any case
class Disk extends Map {
  constructor(dirs) { super(); this.dirs = dirs; this.idx = new Map();
    for (const d of dirs.slice().reverse()) for (const n of readdirSync(d)) if (statSync(join(d, n)).isFile()) this.idx.set(n.toLowerCase(), join(d, n)); }
  path(n) { return this.idx.get(n.replace(/^\.?\//, '').toLowerCase()); }
  has(n) { return super.has(n) || !!this.path(n); }
  get(n) { if (super.has(n)) return super.get(n); const p = this.path(n); if (!p) return undefined;
    const b = new Uint8Array(readFileSync(p)); super.set(n, b); return b; }
}

async function nodeRun(profile) {
  registerHooks({
    load(url, ctx, next) {
      const r = next(url, ctx);
      if (url.endsWith('/libjs/c4m.js')) return { ...r, source: hookC4m(String(r.source)), shortCircuit: true };
      return r;
    },
  });
  const { createMachine, boot } = await import('../libjs/boot.js');
  if (o.accel !== false) {
    const { loadAccel, accelManifest } = await import('../libjs/accel.js');
    await loadAccel(await accelManifest());
  }
  const { STOP } = await import('../libjs/c4m.js');
  const { readC4r } = await import('../libjs/c4r.js');
  const runOnce = async (wantProfile, groups) => {
    const imgPath = o.image || join(GAMES, R.image);
    const bytes = new Uint8Array(readFileSync(imgPath));
    const out = [];
    const files = new Disk(o.data ? [o.data, GAMES] : [GAMES]);
    // --out: what the game writes (Duke's -shot N frames) lands there. The
    // bare libc writes to drive 1, the game's store (bare.c)
    const sink = o.out && !wantProfile ? (name, b) => writeFileSync(join(o.out, basename(name)), b) : null;
    const drives = [{ files, writable: false, sink: null }];
    if (sink) drives.push({ files: new Map(), writable: true, sink });
    const vm = createMachine({ arenaMb: R.mem, drives, onByte: b => out.push(b), mhz: 20, accel: o.accel });
    boot(vm, bytes, [basename(imgPath), ...R.args, ...o.extra]);
    const img = readC4r(bytes), S = img.smap;
    if (!S || !S.funcs) throw new Error('framebench: the image has no source map (build it with -g)');
    const funcs = S.funcs.slice().sort((a, b) => a.start - b.start);
    const anchor = vm.symbols.find(s => funcs.some(f => f.name === s.name));
    FB.base = anchor.addr - funcs.find(f => f.name === anchor.name).start * 4;
    FB.n = img.code.length + 8;
    FB.on = new Uint8Array(FB.n);
    const at = name => { const f = funcs.filter(f => f.name === name); if (f.length !== 1) throw new Error(`framebench: ${f.length} functions named ${name}`); return f[0].start; };
    const fAt = at(R.frame), mAt = R.mark ? at(R.mark) : -1;
    FB.on[fAt] = 1; if (mAt >= 0) FB.on[mAt] = 2;
    FB.calls = wantProfile ? new Uint32Array(FB.n) : null;
    FB.starts = new Uint8Array(FB.n);
    for (const f of funcs) if (f.start < FB.n) FB.starts[f.start] = 1;
    const cyc = [], wall = [], marks = [];
    let stop = false, frame = 0;
    // profile: samples and calls charged to the group of the frame being made
    const starts = funcs.map(f => f.start);
    const fidx = w => { let lo = 0, hi = starts.length - 1, b = -1; while (lo <= hi) { const m = (lo + hi) >> 1; if (starts[m] <= w) { b = m; lo = m + 1; } else hi = m - 1; } return b; };
    const G = wantProfile ? groups.map(() => ({ samp: new Float64Array(funcs.length), calls: new Float64Array(funcs.length), frames: 0 })) : null;
    let gNow = -1;
    const closeCalls = () => {     // the calls since the last frame, into the group of the frame they made
      if (!G) return;
      if (gNow >= 0) { const g = G[gNow]; for (let i = 0; i < FB.n; i++) if (FB.calls[i]) g.calls[fidx(i)] += FB.calls[i]; }
      FB.calls.fill(0);
    };
    FB.hit = (x, cycle, m) => {
      if (FB.on[x] === 1) {
        cyc.push(cycle - m.skipped); wall.push(performance.now()); frame++;
        if (G) { closeCalls(); gNow = groups.findIndex(s => s.has(frame + 1)); }
        if (o.stopAt && frame >= o.stopAt) stop = true;
      } else {
        marks.push(frame);
        if (R.marks && marks.length > R.marks) stop = true;     // the demo after the last: back to the first
      }
    };
    const t0 = performance.now(), lim = t0 + o.timeout * 1000;
    const CH = wantProfile ? o.every : 2e6;
    let r, n = 0;
    for (;;) {
      r = vm.run(CH);
      if (G && gNow >= 0) { const w = (vm.pc - FB.base) >> 2; if (w >= 0 && w < FB.n) G[gNow].samp[fidx(w)]++; }
      if (r === STOP.HALT || vm.dev.halted || stop) break;
      if (r === STOP.INPUT) { console.error('framebench: the game waits for input'); break; }
      if ((++n & 1023) === 0 && performance.now() > lim) { console.error('framebench: timed out'); break; }
    }
    if (G) { closeCalls(); for (const g of G) g.frames = 0; }
    const text = Buffer.from(out).toString('latin1');
    return { cyc, wall, marks, text, funcs, G, secs: (performance.now() - t0) / 1000 };
  };

  const skip = o.skip ?? R.skip ?? 0;
  const res = await runOnce(false);
  const { cyc, wall, marks, text } = res;
  // frame k (1-based) is what was done between the calls for frames k-1 and k
  const ins = [], ms = [];
  for (let k = 2; k <= cyc.length; k++) { ins[k] = cyc[k - 1] - cyc[k - 2]; ms[k] = wall[k - 1] - wall[k - 2]; }
  const end = (text.match(/^timedemo: .*$/m) || [''])[0];
  if (o.lines) for (const l of text.split('\n')) if (o.lines.test(l)) console.log(l);
  const segs = [];
  if (R.mark) {
    // a demo starts at its open: its first frame loads the level, and is
    // left out; it ends where the next one opens
    for (let i = 0; i < marks.length - 1; i++) segs.push({ name: `demo${i + 1}`, from: marks[i] + 3, to: marks[i + 1], load: [marks[i] + 1, marks[i] + 2] });
    if (R.marks && marks.length <= R.marks && marks.length) segs.push({ name: `demo${marks.length} (cut short)`, from: marks[marks.length - 1] + 3, to: cyc.length });
  } else segs.push({ name: 'run', from: 2 + skip, to: cyc.length, load: skip ? [2, 1 + skip] : null });
  console.log(`framebench: ${o.run} on node's bare machine: ${basename(o.image || R.image)} ${[...R.args, ...o.extra].join(' ')}`);
  console.log(`  ${cyc.length} frames in ${fmt(res.secs, 1)} s; ${end || 'no timedemo line'}`);
  const dump = { run: o.run, where: 'node', args: [...R.args, ...o.extra], frames: cyc.length, end, segs: [] };
  for (const s of segs) {
    const fi = [], fm = [];
    for (let k = s.from; k <= s.to; k++) { fi.push(ins[k]); fm.push(ms[k]); }
    if (!fi.length) continue;
    const left = s.load ? ins.slice(s.load[0], s.load[1] + 1).map(v => fmt(v / 1e6, 2)) : [];
    console.log(`\n-- ${s.name}: frames ${s.from}-${s.to} (${fi.length})${left.length ? `; left out before them, the start, frames ${s.load[0]}-${s.load[1]}: ${left.join(', ')} M` : ''}`);
    console.log(rateHead);
    for (const m of o.mips) console.log(rateRow(`at ${m} M instructions/s`, rates(fi.map(v => v / (m * 1e3)))));
    console.log(rateRow("node's clock", rates(fm)));
    for (const l of instrRows(fi, s.from)) console.log(l);
    dump.segs.push({ ...s, ins: fi, ms: fm });
  }
  if (o.json) writeFileSync(o.json, JSON.stringify(dump));
  if (!profile) return;

  // ---- the heaviest 1% of frames against the median ones ----
  const all = [];
  const left = k => (o.leave || []).some(([a, b]) => k >= a && k <= (b ?? a));
  for (const s of segs) for (let k = s.from; k <= s.to; k++) if (!left(k)) all.push([ins[k], k]);
  all.sort((a, b) => a[0] - b[0]);
  const nH = Math.max(1, Math.ceil(all.length * 0.01));
  const heavy = new Set(all.slice(all.length - nH).map(x => x[1]));
  const mid = all.length >> 1, half = Math.max(1, Math.ceil(all.length * 0.05));
  const median = new Set(all.slice(mid - half, mid + half).map(x => x[1]));
  const p = await runOnce(true, [heavy, median]);
  // the same frames, as far as both went: a run stops at the end of the slice in which
  // the demo after the last began, and that is 97 instructions here but 2 M in the plain
  // run, which may finish the frame it began in (whose count has the next demo's start
  // in it) and another
  const upto = Math.min(Math.max(...segs.map(s => s.to)) + 1, cyc.length, p.cyc.length);
  const same = upto >= Math.max(...segs.map(s => s.to)) && cyc.slice(0, upto).every((v, i) => v === p.cyc[i]);
  const insOf = set => [...set].reduce((t, k) => t + ins[k], 0) / set.size;
  const H = p.G[0], M = p.G[1], F = p.funcs;
  const sH = H.samp.reduce((a, b) => a + b, 0), sM = M.samp.reduce((a, b) => a + b, 0);
  const iH = insOf(heavy), iM = insOf(median);
  // a function's instructions a frame: its share of the group's samples times the group's mean
  const rows = F.map((f, i) => ({ name: f.name, h: H.samp[i] / sH * iH, m: M.samp[i] / sM * iM,
                                  ch: H.calls[i] / heavy.size, cm: M.calls[i] / median.size }));
  console.log(`\n-- where the heaviest 1% of frames go (${heavy.size} frames, mean ${fmt(iH / 1e6, 3)} M), against the median 10% (${median.size} frames, mean ${fmt(iM / 1e6, 3)} M)`);
  console.log(`   pc sampled every ${o.every} instructions; the second run ${same ? 'made the same frames' : 'DIFFERED from the first'}`);
  if (!same) {
    const k = cyc.findIndex((v, i) => i < upto && v !== p.cyc[i]);
    if (k >= 0) console.log(`   first at frame ${k}: ${cyc[k] - (cyc[k - 1] || 0)} instructions, then ${p.cyc[k] - (p.cyc[k - 1] || 0)}`);
  }
  console.log(`${'function'.padEnd(28)}${'median M'.padStart(9)}${'heavy M'.padStart(9)}${'growth'.padStart(9)}${'of it'.padStart(7)}${'calls med'.padStart(11)}${'calls hvy'.padStart(11)}`);
  const grow = iH - iM;
  for (const r of rows.sort((a, b) => (b.h - b.m) - (a.h - a.m)).slice(0, 30))
    console.log(`${r.name.slice(0, 27).padEnd(28)}${fmt(r.m / 1e6, 3).padStart(9)}${fmt(r.h / 1e6, 3).padStart(9)}${fmt((r.h - r.m) / 1e6, 3).padStart(9)}` +
                `${(fmt(100 * (r.h - r.m) / grow, 1) + '%').padStart(7)}${fmt(r.cm, 0).padStart(11)}${fmt(r.ch, 0).padStart(11)}`);
  console.log(`heaviest frames: ${[...heavy].sort((a, b) => a - b).map(k => `#${k}`).join(' ')}`);
  console.log(`\n-- where a median frame goes (${fmt(iM / 1e6, 3)} M), and the same functions in the heaviest`);
  console.log(`${'function'.padEnd(28)}${'median M'.padStart(9)}${'share'.padStart(7)}${'heavy M'.padStart(9)}${'share'.padStart(7)}`);
  for (const r of rows.sort((a, b) => b.m - a.m).slice(0, 20))
    console.log(`${r.name.slice(0, 27).padEnd(28)}${fmt(r.m / 1e6, 3).padStart(9)}${(fmt(100 * r.m / iM, 1) + '%').padStart(7)}` +
                `${fmt(r.h / 1e6, 3).padStart(9)}${(fmt(100 * r.h / iH, 1) + '%').padStart(7)}`);
  if (o.json) writeFileSync(o.json.replace(/\.json$/, '') + '-profile.json', JSON.stringify({ heavy: [...heavy], median: [...median], rows }));
}

// ---- the page ----
// C4IX in a tab of its own, the desktop stopped for its shell, the game
// typed there. Each batch of display commands the page draws is timed, and
// the frames presented in it counted (CMD.PRESENT, gui-device.js): a batch
// of k frames after an interval of t stands for k frames of t/k each, and
// only its last is seen. The page's display runs on animation frames,
// which only a tab in front gets, so the tab is brought to the front.
async function pageRun() {
  if (o.again) return pageReport(JSON.parse(readFileSync(o.again, 'utf8')));
  const { Browser } = await import('../libjs/tests/cdp.mjs');
  const sleep = t => new Promise(r => setTimeout(r, t));
  const b = await Browser.connect(o.page);
  const mine = new Set();
  const listTabs = async () => (await b.send('Target.getTargets')).targetInfos.filter(t => t.type === 'page').length;
  const before = await listTabs();
  const tab = await b.newTab(false);
  mine.add(tab.targetId);
  let added = [];
  const url = `${o.base}/libjs/web/?${o.query}${o.accel === false ? '&accel=0' : ''}`;
  const lag = o.lag ? LAG[o.run] : null;
  if (o.lag && !lag) throw new Error(`framebench: no --lag game for ${o.run}; one of ${Object.keys(LAG).join(', ')}`);
  const cmd = (lag ? lag.page : R.page) + (o.extra.length ? ' ' + o.extra.join(' ') : '');
  let result = null;
  try {
    const boot = async () => {
      await tab.goto(url);
      if (!await tab.waitFor('!!window.c4m', 40000)) throw new Error('no machine on the page: ' + await tab.eval('location.href'));
      // the game's output, and when its timedemo line came: what the page
      // draws after that (the shell taking the display back) is not its
      await tab.eval(`window.__out = ''; window.__end = 0; { const td = new TextDecoder(); window.c4m.on('out', b => {
        window.__out += td.decode(b); if (!window.__end && /timedemo: /.test(window.__out.slice(-400))) window.__end = performance.now(); }); }`);
      if (!await tab.waitFor('/desktop: running on the display/.test(window.__out)', 400000, 250)) throw new Error('the desktop did not start');
    };
    await boot();
    // the person's files, laid over the disk at boot: put in the page's
    // Uploads unless they are there already (by name and size), and taken
    // out again at the end
    if (o.data && R.data) {
      const have = new Map(await tab.eval(`(async () => [...(await window.c4m.store.load('uploads'))].map(([n, b]) => [n, b.length]))()`));
      for (const f of readdirSync(o.data)) {
        const name = 'duke3d/' + f.toLowerCase(), bytes = readFileSync(join(o.data, f));
        if (have.get(name) === bytes.length || have.get(f.toLowerCase()) === bytes.length) continue;
        await tab.eval('window.__up = []');
        for (let i = 0; i < bytes.length; i += 3 << 20)
          await tab.eval(`window.__up.push(Uint8Array.from(atob(${JSON.stringify(bytes.subarray(i, i + (3 << 20)).toString('base64'))}), c => c.charCodeAt(0)))`);
        await tab.eval(`(async () => { const n = window.__up.reduce((t, c) => t + c.length, 0), all = new Uint8Array(n); let at = 0;
          for (const c of window.__up) { all.set(c, at); at += c.length; } window.__up = null;
          await window.c4m.store.backend.putFile('uploads', ${JSON.stringify(name)}, all); return n; })()`);
        added.push(name);
      }
      if (added.length) { console.error(`framebench: put ${added.join(', ')} in the page's Uploads for this run`); await boot(); }
    }
    await tab.eval('window.c4m.signal(2)');
    await sleep(1500);
    // the frames as the page draws them, and (for --lag) the keys
    await tab.eval(`(() => {
      const d = window.c4m.display, draw = d.drawCoalesced.bind(d), PRESENT = 8;
      const fb = window.__fb = { t: [], k: [], key: [], seen: [], armed: false, prev: null, shared: !!window.c4m.shared };
      const look = () => {
        const c = d.front.canvas, w = c.width, h = c.height, px = new Uint32Array(d.front.getImageData(0, 0, w, h).data.buffer);
        const s = new Uint32Array(64 * 40);
        for (let y = 0; y < 40; y++) for (let x = 0; x < 64; x++) s[y * 64 + x] = px[Math.floor((y + 0.5) * h / 40) * w + Math.floor((x + 0.5) * w / 64)];
        return s;
      };
      d.drawCoalesced = words => {
        let k = 0;
        for (let i = 0; i < words.length && words[i] >= 2; i += words[i]) if (words[i + 1] === PRESENT) k++;
        draw(words);
        if (!k) return;
        const t = performance.now();
        fb.t.push(t); fb.k.push(k);
        if (fb.armed) {
          const s = look(), p = fb.prev; fb.prev = s;
          if (p && fb.key.length > fb.seen.length && fb.key[fb.key.length - 1] < t) {
            let n = 0; for (let i = 0; i < s.length; i++) if (s[i] !== p[i]) n++;
            if (n > s.length * 0.25) fb.seen.push([t, fb.t.length]);
          }
        }
      };
      addEventListener('keydown', e => { if (fb.armed && !e.repeat) fb.key.push(performance.now()); }, true);
    })()`);
    const shared = await tab.eval('window.__fb.shared');
    if (!shared) console.error('framebench: the page has no shared display path (not cross-origin isolated?): its timings are the message path\'s');
    await tab.eval(`window.c4m.write(${JSON.stringify(cmd + '\n')})`);
    const t0 = Date.now(), lim = o.timeout * 1000;
    if (lag) {
      await sleep(lag.settle * 1000);
      await tab.eval(`document.getElementById('display').focus()`);
      await tab.eval('window.__fb.armed = true');
      const turn = async (key, code, vk) => {
        await tab.send('Input.dispatchKeyEvent', { type: 'rawKeyDown', key, code, windowsVirtualKeyCode: vk });
        await sleep(300);
        await tab.send('Input.dispatchKeyEvent', { type: 'keyUp', key, code, windowsVirtualKeyCode: vk });
        await sleep(1700);
      };
      for (let i = 0; i < o.lag; i++)
        await (i & 1 ? turn('ArrowRight', 'ArrowRight', 39) : turn('ArrowLeft', 'ArrowLeft', 37));
      const fb = await tab.eval('({ t: window.__fb.t, k: window.__fb.k, key: window.__fb.key, seen: window.__fb.seen })');
      result = { fb, out: await tab.eval('window.__out') };
    } else {
      let out = '';
      while (Date.now() - t0 < lim) {
        out = await tab.eval('window.__out');
        if (/timedemo: .*frames/.test(out) || /cannot load|over the|libjs: /.test(out)) break;
        await sleep(2000);
      }
      const fb = await tab.eval('({ t: window.__fb.t, k: window.__fb.k, end: window.__end })');
      result = { fb, out };
    }
  } finally {
    if (added.length) {
      try { for (const n of added) await tab.eval(`window.c4m.store.backend.deleteFile('uploads', ${JSON.stringify(n)})`); }
      catch (e) { console.error(`framebench: could not take ${added.join(', ')} out of the page's Uploads: ${e.message}`); }
    }
    await tab.close();
    const after = await listTabs();
    if (after !== before) console.error(`framebench: ${before} tabs before, ${after} after`);
    b.disconnect();
  }
  result = { run: o.run, where: 'page', url, chrome: o.page, query: o.query, cmd, lag: !!lag, ...result };
  if (o.json) writeFileSync(o.json, JSON.stringify(result));
  pageReport(result);
}

function pageReport(result) {
  const { out, url, chrome, cmd } = result, lag = result.lag;
  let fb = result.fb;
  // only what was drawn before the game's timedemo line came
  if (fb.end) { const n = fb.t.findIndex(t => t > fb.end); if (n >= 0) fb = { ...fb, t: fb.t.slice(0, n), k: fb.k.slice(0, n) }; }
  const gameLines = out.split('\n').filter(l => /^speed:|^timedemo:|^duke3d:|cannot load|libjs:/.test(l));
  console.log(`framebench: ${result.run} in the page, ${url}, Chrome at ${chrome}: ${cmd}`);
  console.log(`  ${gameLines.join(' | ') || 'no result: ' + out.slice(-300)}`);
  // each frame's time: a batch's interval shared among its frames
  const per = [], shownMs = [];
  for (let j = 1; j < fb.t.length; j++) { const dt = fb.t[j] - fb.t[j - 1]; shownMs.push(dt); for (let i = 0; i < fb.k[j]; i++) per.push(dt / fb.k[j]); }
  const total = fb.k.reduce((a, b) => a + b, 0);
  if (lag) {
    const lags = [], counts = [];
    for (let i = 0; i < fb.seen.length; i++) {
      const [t, idx] = fb.seen[i], k = fb.key[i];
      lags.push(t - k);
      // frames the page showed after the key, up to the one that showed it
      let n = 0; for (let j = 0; j < idx; j++) if (fb.t[j] > k) n++;
      counts.push(n);
    }
    // the frame rate around the presses
    const rate = rates(shownMs.slice(Math.floor(shownMs.length / 4)));
    const s = sorted(lags);
    console.log(`  ${fb.key.length} presses, ${lags.length} seen; shown frames ${fmt(rate.avg)} a second (frame ${fmt(1000 / rate.avg, 0)} ms)`);
    console.log(`  key to the frame that shows it: median ${fmt(pct(s, 50), 0)} ms, min ${fmt(s[0], 0)}, max ${fmt(s[s.length - 1], 0)} (${lags.map(x => fmt(x, 0)).join(' ')})`);
    console.log(`  ... in frames shown: ${counts.join(' ')}; in frame times: median ${fmt(pct(s, 50) * rate.avg / 1000, 2)}`);
    return;
  }
  // the game's frames, by its timedemo line; off, the presents beyond them
  const told = /^timedemo: .*?(\d+) frames/m.exec(out), N = told ? +told[1] : total, off = Math.max(0, total - N);
  console.log(`  ${total} presents for the game's ${N} frames, ${fb.t.length} batches drawn`);
  // The page presents more often than the game makes frames (a palette
  // shown alone presents too: Duke's flashes when hit), so its presents are
  // not the game's frames. A run (or with --like, each of a node run's
  // demos) starts after its level's load, found as the last long wait for a
  // batch where the start should be, and ends where the next one's load
  // starts, or at the game's timedemo line. Its average is the game's
  // frames (node's count) over that time; the rest is the frames the page
  // showed, a batch each, timed from the one before.
  const segs = o.like ? JSON.parse(readFileSync(o.like, 'utf8')).segs.map(({ name, from, to }) => ({ name, from, to }))
                      : [{ name: 'run', from: 2 + (o.skip ?? R.skip ?? 0), to: N }];
  const cum = [], gap = [];
  for (let j = 0, c = 0; j < fb.t.length; j++) { c += fb.k[j]; cum[j] = c; gap[j] = j ? fb.t[j] - fb.t[j - 1] : 0; }
  const medGap = sorted(gap.slice(1))[gap.length >> 1] || 1;
  for (const s of segs) {
    // where the load should be, by the page's count of presents: a run's
    // start, past the presents before the game's frames, within 12; a demo's
    // up to a quarter further on than the game's count, palettes and all
    const lo = o.like ? s.from - 3 : s.from + off - 12, hi = o.like ? Math.ceil(s.from * 1.25) + 60 : s.from + off + 12;
    // a load can be several waits (Wolf3D's are three): up to the last of them
    let at = -1;
    for (let j = 1; j < fb.t.length; j++) if (cum[j] >= lo && cum[j] <= hi && gap[j] > Math.max(4 * medGap, 300)) at = j;
    s.at = at;
    s.first = at >= 0 ? at : Math.max(0, cum.findIndex(c => c >= s.from + (o.like ? 0 : off)) - 1);
  }
  for (let i = 0; i < segs.length; i++) {
    const s = segs[i], last = i + 1 < segs.length ? segs[i + 1].first - 1 : fb.t.length - 1;
    if (last <= s.first) continue;
    const seen = gap.slice(s.first + 1, last + 1), secs = (fb.t[last] - fb.t[s.first]) / 1000, frames = s.to - s.from + 1;
    const r = rates(seen);
    r.avg = frames / secs;
    console.log(`\n-- ${s.name}: the game's frames ${s.from}-${s.to} (${frames}) in ${fmt(secs, 1)} s, ${seen.length} seen` +
                (s.at >= 0 ? `; its load left out (${fmt(gap[s.at], 0)} ms)` : ''));
    console.log(rateHead);
    console.log(rateRow('the page', r));
  }
}

const main = o.page ? pageRun() : nodeRun(o.profile);
// node 26 can hang in its exit (cli.js says why): exit at once
main.then(() => process.exit(0), e => { console.error(e.stack || e); process.exit(1); });
