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
//   --build-jobs N  make -j for the first phase (default 2)
//   --no-build    skip the first phase
//   --list-prep   print what the first phase would build, and stop
//
// Two phases. Tests share what they are built from, and two makes building
// one file at once can corrupt it, so the first phase builds, in one make,
// everything the tests depend on: c4bb-images, each test's prerequisites,
// and what their recipes make again ($(MAKE) c4ix.c4r and the like). The
// second runs each test as `make -s -o c4bb-images TEST`, which then finds
// all it needs up to date and only tests. -o, because c4bb-images is no
// file: its recipe (build-images.sh) runs whenever a test names it, and a
// dozen tests do. Packed, several ran it at once, each deleting and
// copying the disks the others were copying.
//
// What a test needs is learned: each run keeps every test's time, its
// peak memory (all its processes together, sampled every second) and its
// processors (CPU time over wall time) in .suite-stats.json, and the next
// run packs by them. A test is started when, on top of what the
// running ones may still grow to, its peak fits in the memory available
// now (/proc/meminfo, so everything else on the machine counts too) less
// the reserve, and its processors fit; when nothing is running it starts
// whatever it needs. On Linux it reads /proc; on Windows (under MSYS2,
// docs/windows.md) MSYS2's /proc, through an MSYS2 shell.

import { spawn, execFileSync } from 'node:child_process';
import { readFileSync, writeFileSync, mkdirSync, openSync, closeSync, readdirSync } from 'node:fs';
import { cpus, freemem } from 'node:os';
import { join } from 'node:path';

const argv = process.argv.slice(2);
const opt = { out: '.suite', jobs: 0, cpus: 0, reserve: 300, buildJobs: 2, build: true };
const tests = [];
for (let i = 0; i < argv.length; i++) {
  const a = argv[i];
  if (a === '-o') opt.out = argv[++i];
  else if (a === '-j') opt.jobs = +argv[++i];
  else if (a === '--cpus') opt.cpus = +argv[++i];
  else if (a === '--reserve') opt.reserve = +argv[++i];
  else if (a === '--build-jobs') opt.buildJobs = +argv[++i];
  else if (a === '--no-build') opt.build = false;
  else if (a === '--list-prep') opt.listPrep = true;
  else if (a.startsWith('-')) { console.error(`suite: unknown option ${a}`); process.exit(2); }
  else tests.push(a);
}
const NCPU = cpus().length;
if (!opt.cpus) opt.cpus = Math.max(1, NCPU - 1);
if (!opt.jobs) opt.jobs = opt.cpus;
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

// ---- /proc: Linux's, or on Windows MSYS2's ------------------------------
const WIN = process.platform === 'win32';
const PAGE = 4096;
function memAvailableMb() {
  if (WIN) return freemem() / 1048576;  // the available physical memory
  const m = /MemAvailable:\s+(\d+)/.exec(readFileSync('/proc/meminfo', 'utf8'));
  return m ? +m[1] / 1024 : 0;
}
// On Windows (docs/windows.md) node is not an MSYS2 program and cannot
// read MSYS2's /proc, and Windows' own idea of who started whom is no
// use: an MSYS2 exec is a new Windows process whose parent has gone. So
// one MSYS2 shell, kept for the whole run, reads /proc for each request:
// every process's MSYS2 pid, parent, resident pages and Windows pid. The
// tree is MSYS2's, and a test's root is found by the Windows pid node has
// for it. MSYS2's pages are 64K, so it says their size first.
let sh;
function winProcesses() {
  if (!sh) {
    const script = `getconf PAGESIZE
      while read -r _; do
        for d in /proc/[0-9]*; do
          read -r w < $d/winpid && read -r st < $d/stat && read -r _ r _ < $d/statm || continue
          st=\${st##*) }; set -- $st
          echo "\${d#/proc/} $2 $r $w"
        done 2>/dev/null
        echo END
      done`;
    sh = spawn('sh', ['-c', script], { stdio: ['pipe', 'pipe', 'ignore'] });
    sh.buf = ''; sh.waiting = [];
    sh.stdout.setEncoding('utf8');
    sh.stdout.on('data', d => {
      sh.buf += d;
      if (!sh.page) {
        const i = sh.buf.indexOf('\n');
        if (i < 0) return;
        sh.page = +sh.buf.slice(0, i); sh.buf = sh.buf.slice(i + 1);
      }
      let i;
      while ((i = sh.buf.indexOf('END\n')) >= 0) {
        const block = sh.buf.slice(0, i);
        sh.buf = sh.buf.slice(i + 4);
        sh.waiting.shift()?.(block);
      }
    });
  }
  return new Promise(res => { sh.waiting.push(res); sh.stdin.write('\n'); }).then(block => {
    const kids = new Map(), rss = new Map(), root = new Map();
    for (const line of block.split('\n')) {
      const [pid, ppid, pages, winpid] = line.trim().split(' ').map(Number);
      if (!pid) continue;
      rss.set(pid, pages * sh.page / 1048576);
      root.set(winpid, pid);
      if (!kids.has(ppid)) kids.set(ppid, []);
      kids.get(ppid).push(pid);
    }
    return { kids, rss, root };
  });
}
// every process: its parent and resident size, in MB
async function processes() {
  if (WIN) return winProcesses();
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
  if (p.root) pid = p.root.get(pid) ?? pid;   // Windows: node's pid for it, to MSYS2's
  let mb = 0;
  const stack = [pid], seen = new Set();
  while (stack.length) {
    const q = stack.pop();
    if (seen.has(q)) continue;
    seen.add(q);
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
  // MSYS2's GNU time on Windows, found on the PATH: node is not an MSYS2
  // program, and has no /usr/bin
  const child = spawn(WIN ? 'time' : '/usr/bin/time', ['-f', '%e %U %S', '-o', timing, 'make', ...makeArgs, target],
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
  // A phony prerequisite with a recipe of its own does work, and as phony
  // would do it again for the test: left to the test, as is a test-* or
  // run-* one (test-c4l, a test of test's that is not declared phony).
  // One that only gathers files (libc-bare-objs) is built here.
  const want = new Set(['c4bb-images']);
  const add = p => {
    if (tests.includes(p) || want.has(p) || /^(test|run)-/.test(p)) return;
    if (phony.has(p) && (rules.get(p)?.recipe.length)) return;
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
  return [...want];
}

if (opt.listPrep) { console.log(prerequisites().join('\n')); process.exit(0); }
if (opt.build) {
  const prep = prerequisites();
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
    const job = run(t, join(opt.out, `${t}.log`), ['-s', '-o', 'c4bb-images']);
    running.add(job);
    job.done.then(j => {
      running.delete(j);
      j.wall = (Date.now() - j.start) / 1000;
      j.cpu = cpuOf(j);
      finished.push(j);
      stats[j.target] = { wall: Math.round(j.wall), mem: Math.round(j.peak), cpu: +j.cpu.toFixed(2) };
      writeFileSync(STATS, JSON.stringify(stats, null, 1) + '\n');
      note(`${j.target} exit=${j.code} ${Math.round(j.wall)}s ${Math.round(j.peak)}MB cpu ${j.cpu.toFixed(1)}`);
    });
    i = -1;                             // what fits has changed
  }
}

const s2 = Date.now();
await new Promise(res => {
  const tick = async () => {
    if (running.size) {
      const p = await processes();
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
