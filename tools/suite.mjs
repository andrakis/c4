#!/usr/bin/env node
// tools/suite.mjs -- the full test suite, packed: as many tests at once as
// the machine has memory and processors for, longest first.
//
//   node tools/suite.mjs [options] [test-X ...]     (make suite)
//
//   -o DIR        logs and the result go here (default .suite/)
//   -j N          at most N tests at once (default: processors - 1)
//   --cpus N      processors the tests may use between them (default: processors - 1)
//   --reserve MB  memory left free for everything else (default 300)
//   --mem MB      memory the machine can grow to (default: $SUITE_MEM, else
//                 what it has now). A Hyper-V guest with Dynamic Memory
//                 shows only what it has been given so far; the rest is
//                 counted as available, and the host adds it as it is used.
//   --build-jobs N  make -j for the first phase (default: a job per 1.5 GB
//                 of memory, from 2 up to the processors the tests may use)
//   --no-build    skip the first phase
//   --list-prep   print what the first phase would build, and stop
//
// Two phases. Tests share what they are built from, and two makes building
// one file at once can corrupt it, so the first phase builds, in one make,
// everything the tests depend on: c4bb-images, each test's prerequisites,
// and what their recipes make again ($(MAKE) c4ix.c4r and the like). The
// second runs each test as `make -s TEST`, which then finds all it needs
// up to date and only tests. What the first built that is not a file
// (c4bb-images, phony) the second is told is made (make -o c4bb-images):
// else every test runs build-images.sh again, several at once over the
// same src/c4bb/images, and they trip over each other's files.
//
// What a test needs is learned: each run keeps every test's time, its
// peak memory (all its processes together, sampled every second) and its
// processors (CPU time over wall time) in .suite-stats.json, and the next
// run packs by them. A test is started when, on top of what the
// running ones may still grow to, its peak fits in the memory available
// now (/proc/meminfo, so everything else on the machine counts too) less
// the reserve, and its processors fit; when nothing is running it starts
// whatever it needs. Linux only: it reads /proc.

import { spawn, execFileSync } from 'node:child_process';
import { readFileSync, writeFileSync, mkdirSync, openSync, closeSync, readdirSync, existsSync } from 'node:fs';
import { cpus } from 'node:os';
import { join } from 'node:path';

const argv = process.argv.slice(2);
const opt = { out: '.suite', jobs: 0, cpus: 0, reserve: 300, mem: +(process.env.SUITE_MEM || 0), buildJobs: 0, build: true };
const tests = [];
for (let i = 0; i < argv.length; i++) {
  const a = argv[i];
  if (a === '-o') opt.out = argv[++i];
  else if (a === '-j') opt.jobs = +argv[++i];
  else if (a === '--cpus') opt.cpus = +argv[++i];
  else if (a === '--reserve') opt.reserve = +argv[++i];
  else if (a === '--mem') opt.mem = +argv[++i];
  else if (a === '--build-jobs') opt.buildJobs = +argv[++i];
  else if (a === '--no-build') opt.build = false;
  else if (a === '--list-prep') opt.listPrep = true;
  else if (a.startsWith('-')) { console.error(`suite: unknown option ${a}`); process.exit(2); }
  else tests.push(a);
}
const NCPU = cpus().length;
if (!opt.cpus) opt.cpus = Math.max(1, NCPU - 1);
if (!opt.jobs) opt.jobs = opt.cpus;
const memTotalMb = () => +/MemTotal:\s+(\d+)/.exec(readFileSync('/proc/meminfo', 'utf8'))[1] / 1024;
if (!opt.buildJobs)
  opt.buildJobs = Math.max(2, Math.min(opt.cpus, Math.floor(Math.max(opt.mem, memTotalMb()) / 1536)));
if (!tests.length) tests.push(...execFileSync('make', ['-s', 'print-suite'], { encoding: 'utf8' }).trim().split(/\s+/));
mkdirSync(opt.out, { recursive: true });

const STATS = '.suite-stats.json';
let stats = {};
try { stats = JSON.parse(readFileSync(STATS, 'utf8')); } catch {}
// unseen: long, so that it starts early, and middling in size -- the
// memory check is made against what is available when it starts, and an
// underestimate costs swap, not a failure
const UNSEEN = { wall: 1800, mem: 600, cpu: 1 };
const est = t => {
  const s = stats[t] || {};
  return {
    wall: s.wall ?? UNSEEN.wall,
    mem: s.mem !== undefined ? s.mem * 1.15 + 50 : UNSEEN.mem,
    cpu: s.cpu !== undefined ? Math.min(opt.cpus, Math.max(1, Math.round(s.cpu))) : UNSEEN.cpu,
  };
};

const t0 = Date.now();
const secs = ms => Math.round(ms / 1000);
const result = join(opt.out, 'result.txt');
writeFileSync(result, '');
const note = line => { writeFileSync(result, line + '\n', { flag: 'a' }); console.log(line); };

// ---- /proc ----------------------------------------------------------
const PAGE = 4096;
// with --mem, what the machine has yet to be given counts as available
function memAvailableMb() {
  const info = readFileSync('/proc/meminfo', 'utf8');
  const kb = k => +(new RegExp(`${k}:\\s+(\\d+)`).exec(info) || [0, 0])[1] / 1024;
  return kb('MemAvailable') + Math.max(0, opt.mem - kb('MemTotal'));
}
// every process: its parent and resident size, in MB
function processes() {
  const kids = new Map(), rss = new Map();
  for (const d of readdirSync('/proc')) {
    if (!/^\d+$/.test(d)) continue;
    try {
      const st = readFileSync(`/proc/${d}/stat`, 'utf8');
      const ppid = +st.slice(st.lastIndexOf(')') + 2).split(' ')[1];
      const pages = +readFileSync(`/proc/${d}/statm`, 'utf8').split(' ')[1];
      rss.set(+d, pages * PAGE / 1048576);
      if (!kids.has(ppid)) kids.set(ppid, []);
      kids.get(ppid).push(+d);
    } catch {}                          // gone meanwhile
  }
  return { kids, rss };
}
function treeMb(pid, p) {
  let mb = 0;
  const stack = [pid];
  while (stack.length) {
    const q = stack.pop();
    mb += p.rss.get(q) || 0;
    for (const k of p.kids.get(q) || []) stack.push(k);
  }
  return mb;
}

// ---- running one make, timed ----------------------------------------
// /usr/bin/time gives the CPU time of everything make waited for
function run(target, log, makeArgs) {
  const timing = log + '.time';
  const fd = openSync(log, 'w');
  const child = spawn('/usr/bin/time', ['-f', '%e %U %S', '-o', timing, 'make', ...makeArgs, target],
    { stdio: ['ignore', fd, fd] });
  closeSync(fd);
  const job = { target, child, start: Date.now(), peak: 0, now: 0, timing };
  job.done = new Promise(res => child.on('exit', code => { job.code = code; res(job); }));
  return job;
}
function cpuOf(job) {
  try {
    const [e, u, s] = readFileSync(job.timing, 'utf8').trim().split('\n').pop().split(' ').map(Number);
    return e > 0 ? (u + s) / e : 1;
  } catch { return 1; }
}

// ---- phase one: everything the tests are built from ------------------
// `make -p` prints the rules as make sees them: each test's
// prerequisites, and its recipe, unexpanded, where $(MAKE) X shows what
// it builds again. Asked for .DEFAULT, which has no recipe, it runs
// nothing: asked for the tests, even -n runs every recipe line with
// $(MAKE) in it, and with it some of the tests themselves.
function prerequisites() {
  let db = '';
  try { db = execFileSync('make', ['-npq', '.DEFAULT'], { encoding: 'utf8', maxBuffer: 1 << 28, stdio: ['ignore', 'pipe', 'ignore'] }); }
  catch (e) { db = e.stdout || ''; }
  // every rule: its prerequisites and its recipe
  const rules = new Map();
  const lines = db.split('\n');
  for (let i = 0; i < lines.length; i++) {
    const m = /^([^\s:#=]+):(?!=)\s*(.*)$/.exec(lines[i]);
    if (!m) continue;
    const recipe = [];
    for (let j = i + 1; j < lines.length && /^(\t|#|$)/.test(lines[j]); j++)
      if (lines[j][0] === '\t') recipe.push(lines[j]);
    rules.set(m[1], { prereqs: m[2].split('|')[0].split(/\s+/).filter(Boolean), recipe });
  }
  const phony = new Set((rules.get('.PHONY') || { prereqs: [] }).prereqs);
  // A test that is another's prerequisite (test-libc-bare, of test-doom,
  // test-quake and test-duke3d) runs once, as a job of its own, and the
  // others are told it is made: else each runs it again, at once, in the
  // same scratch directory (.libc_t), and they delete each other's files.
  // A run-* prerequisite is left to its test. Everything else is built
  // here, phony steps too (pre copies the images into the top directory,
  // which tests running at once would do over each other).
  const want = new Set(['c4bb-images']);
  const sub = new Set();
  const add = p => {
    if (/^test-/.test(p)) { sub.add(p); if (!tests.includes(p)) tests.push(p); return; }
    if (tests.includes(p) || want.has(p) || /^run-/.test(p)) return;
    want.add(p);
  };
  for (const t of tests) {
    const r = rules.get(t);
    if (!r) continue;
    r.prereqs.forEach(add);
    for (const line of r.recipe) {
      const m = /\$\(MAKE\)((?:\s+-\S+)*)\s+([^\s;&|)]+)/.exec(line);
      if (m && !m[2].startsWith('$')) add(m[2]);
    }
  }
  // What this phase builds that is no file -- c4bb-images, phony, whose
  // recipe (build-images.sh) every test would otherwise run again, all of
  // them at once over the same src/c4bb/images -- the tests are told is
  // made already (make -o).
  const held = [...want].filter(p => rules.get(p)?.recipe.length && (phony.has(p) || !existsSync(p)));
  return { prep: [...want], held: [...held, ...sub] };
}

const { prep, held } = prerequisites();
if (opt.listPrep) { console.log(prep.join('\n') + `\n(held: ${held.join(' ')})`); process.exit(0); }
if (opt.build) {
  const log = join(opt.out, 'build.log');
  const fd = openSync(log, 'w');
  const s = Date.now();
  const code = await new Promise(res =>
    spawn('make', ['-s', '-k', `-j${opt.buildJobs}`, ...prep], { stdio: ['ignore', fd, fd] }).on('exit', res));
  closeSync(fd);
  note(`build exit=${code} ${secs(Date.now() - s)}s (${prep.length} targets, -j${opt.buildJobs})`);
}

// ---- phase two: the tests, packed -------------------------------------
const queue = tests.slice().sort((a, b) => est(b).wall - est(a).wall);
const running = new Set();
const finished = [];
let peakTotal = 0;

function fits(t) {
  if (!running.size) return true;
  if (running.size >= opt.jobs) return false;
  const e = est(t);
  let cpu = e.cpu, grow = 0;
  for (const j of running) {
    const ej = est(j.target);
    cpu += ej.cpu;
    grow += Math.max(0, ej.mem - j.now);
  }
  return cpu <= opt.cpus && memAvailableMb() - grow - e.mem >= opt.reserve;
}

function startFitting() {
  // the longest first; when it does not fit, a shorter one that does
  for (let i = 0; i < queue.length; i++) {
    if (!fits(queue[i])) continue;
    const t = queue.splice(i, 1)[0];
    const job = run(t, join(opt.out, `${t}.log`), ['-s', ...held.filter(h => h !== t).flatMap(h => ['-o', h])]);
    running.add(job);
    job.done.then(j => {
      running.delete(j);
      j.wall = (Date.now() - j.start) / 1000;
      j.cpu = cpuOf(j);
      finished.push(j);
      // a failure's numbers are of what it did before failing: not kept
      if (j.code === 0) {
        stats[j.target] = { wall: Math.round(j.wall), mem: Math.round(j.peak), cpu: +j.cpu.toFixed(2) };
        writeFileSync(STATS, JSON.stringify(stats, null, 1) + '\n');
      }
      note(`${j.target} exit=${j.code} ${Math.round(j.wall)}s ${Math.round(j.peak)}MB cpu ${j.cpu.toFixed(1)}`);
    });
    i = -1;                             // what fits has changed
  }
}

const s2 = Date.now();
await new Promise(res => {
  const tick = () => {
    if (running.size) {
      const p = processes();
      let total = 0;
      for (const j of running) { j.now = treeMb(j.child.pid, p); j.peak = Math.max(j.peak, j.now); total += j.now; }
      peakTotal = Math.max(peakTotal, total);
    }
    startFitting();
    if (!running.size && !queue.length) return res();
    setTimeout(tick, 1000);
  };
  tick();
});

const failed = finished.filter(j => j.code !== 0).map(j => j.target);
note(`tests ${secs(Date.now() - s2)}s, at most ${Math.round(peakTotal)}MB at once; total ${secs(Date.now() - t0)}s`);
note(failed.length ? `FAILED: ${failed.join(' ')}` : `all ${finished.length} passed`);
note('DONE');
process.exit(failed.length ? 1 : 0);
