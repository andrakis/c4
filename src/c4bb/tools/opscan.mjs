// opscan -- which opcodes does this image actually use, and is that
// more than the machine the player has been given?
//
//   node src/c4bb/tools/opscan.mjs [-q] -rung NAME  image.c4r [...]
//   node src/c4bb/tools/opscan.mjs [-q] -needs NAME image.c4r [...]
//   node src/c4bb/tools/opscan.mjs [-q] [-max N|NAME] image.c4r [...]
//   node src/c4bb/tools/opscan.mjs -rungs
//   node src/c4bb/tools/opscan.mjs -machine
//
// HOMEWARD's ladder is a ladder of opcodes: the player extends their
// own CPU to climb it, and a rung they can already run is a rung they
// have been given for free. `c4l.c` refuses an over-budget image and
// names the instruction, but c4l is built for the host's word size and
// these images are 32-bit, so the check lives here instead.
//
// -rung asserts an image fits a rung. -needs asserts the opposite, and
// is the half that makes the ladder real: an image that FITS the rung
// below did not need the machine extended, whatever the design
// document says. See docs/c4bb-storage.md M15.
import { readFileSync } from 'node:fs';
import { parseC4r } from '../sim/loader.js';
import { OPNAMES } from '../sim/devices.js';

const names = OPNAMES.split(',').map(s => s.trim());
const OP = Object.fromEntries(names.map((n, i) => [n, i]));

// Which opcodes carry an operand word. This is c4m.c's
// c4m_has_operand() and has to stay that: get it wrong and the walk
// reads an operand as an instruction and is out of phase for the whole
// rest of the image, which is not a wrong answer so much as a random
// one.
//
// It WAS wrong here until M15 -- `op <= ADJ`, which is c4l.c's rule.
// Correct for c4l, because c4l stops at the first opcode above EXIT and
// so never meets one that takes an operand; wrong for opscan, which
// keeps walking, met JSRS in every C4KE and C4IX image, and came out
// the far side reporting opcodes like `?-3` -- pieces of somebody's
// operand, read as instructions.
// the machine's own answer (a set, so a permuted ROM keeps it right)
import { hasOperand } from '../sim/machine.js';

// Opcodes this machine does not have at all, at any rung.
//
// 66-78 are c4mp's -- a different machine, with cores and atomics --
// and c4bb's microcode has never had them; `_BLT` is a hole in c4m's
// own table. Excluded from every rung by construction, so that no rung
// can quietly permit something nothing could execute.
const ABSENT = [OP._BLT, ...Array.from({ length: 13 }, (_, k) => OP.CPUI + k)];

// The rungs, and this table is the whole definition of them.
//
// Written from what the images MEASURABLY use, not from what the design
// says they should: `c4m` covers C4KE and C4IX alike because C4IX's
// image genuinely reaches no further than C4KE's, and saying otherwise
// in a test would be pinning a story rather than a program.
const RUNGS = {
  base: {
    max: 'EXIT', extra: [],
    what: 'stock c4, nothing added',
    who: 'the BIOS, C4DOS transients, cpp, c4, c4m, rps',
  },
  dos: {
    max: 'EXIT', extra: ['TIME'],
    what: 'stock c4 plus a clock',
    who: 'C4DOS itself, mandel, raycast -- anything that times itself',
  },
  c4m: {
    max: 'DBG', extra: [],
    what: "c4m's own opcodes: JSRS, traps, signals, INFO and the rest",
    who: 'c4cc and c4rlink, C4KE and C4IX, and both userlands',
  },
  fused: {
    max: 'DBG',
    extra: ['LDL', 'LDG', 'PSHL', 'PSHG', 'LEAP', 'IMMP', 'LIP', 'ADDL',
            'STL', 'POPA'],
    what: 'c4m plus the ten fused opcodes (docs/fused-opcodes.md)',
    who: 'images built through the fused backend',
  },
};

function permitted (rung) {
  const r = RUNGS[rung];
  const set = new Set();
  for (let i = 0; i <= OP[r.max]; ++i) set.add(i);
  for (const n of r.extra) set.add(OP[n]);
  for (const o of ABSENT) set.delete(o);
  return set;
}

function opname (op) { return names[op] ?? `?${op}`; }

// Every opcode an image uses, and where it was first seen.
function scan (path) {
  const { code } = parseC4r(new Uint8Array(readFileSync(path)));
  const used = new Map();                       // opcode -> first index
  // Where the stream STARTS is not fixed: c4cc emits through *++e, so
  // word 0 is never an instruction and its images are 1-based, while
  // c4lc puts its first instruction at word 0. A zero first word
  // settles it -- LEA is opcode 0, and no function begins with LEA.
  // Every one begins with ENT. (Same reasoning as c4l.c's.)
  let i = code[0] ? 0 : 1;
  while (i < code.length) {
    const op = code[i];
    if (!used.has(op)) used.set(op, i);
    i += hasOperand(op) ? 2 : 1;
  }
  return used;
}

// The top rung against the machine itself.
//
// Both directions matter. An opcode in the microcode and in no rung is
// something the player can execute that no test ever notices; an opcode
// in a rung and not in the microcode is a budget permitting an
// instruction the board would trap on. Reading hw/microcode.uc rather
// than keeping a second copy of the list is the point -- a copy is a
// thing that goes stale.
function machineCheck () {
  const uc = readFileSync(new URL('../hw/microcode.uc', import.meta.url), 'utf8');
  const have = new Set();
  for (const line of uc.split('\n')) {
    const m = /^op\s+([A-Za-z_0-9]+)/.exec(line);
    if (m) have.add(m[1]);
  }
  const top = permitted('fused');
  const missing = [...top].filter(o => !have.has(names[o])).map(opname);
  const extra  = [...have].filter(n => !top.has(OP[n])).sort();
  if (missing.length)
    console.log(`opscan: rung 'fused' permits ${missing.join(' ')}, which the microcode does not have`);
  if (extra.length)
    console.log(`opscan: the microcode has ${extra.join(' ')}, which no rung permits`);
  if (!missing.length && !extra.length)
    console.log(`opscan: the top rung is exactly the machine -- ${top.size} opcodes, microcode and budget agree`);
  return (missing.length || extra.length) ? 1 : 0;
}

const argv = process.argv.slice(2);
let mode = 'max', arg = OP.EXIT;
// -q drops the per-image `uses:` line, which is what a caller wants
// when it has many images and only cares that they passed. It exists so
// a Makefile need not reach for `| grep -v`: a pipe hands the recipe
// GREP's exit status, and a budget test that cannot fail is worse than
// no budget test at all.
let quiet = false;
if (argv[0] === '-q') { quiet = true; argv.shift(); }
if (argv[0] === '-machine') process.exit(machineCheck());
if (argv[0] === '-rungs') {
  const w = Math.max(...Object.keys(RUNGS).map(k => k.length));
  for (const [name, r] of Object.entries(RUNGS)) {
    const n = permitted(name).size;
    console.log(`${name.padEnd(w)}  ${String(n).padStart(2)} opcodes  ${r.what}`);
    console.log(`${' '.repeat(w)}              ${r.who}`);
  }
  process.exit(0);
}
if (argv[0] === '-rung' || argv[0] === '-needs') {
  mode = argv[0].slice(1);
  arg = argv[1];
  if (!RUNGS[arg]) {
    console.error(`opscan: no such rung '${arg}' (have: ${Object.keys(RUNGS).join(', ')})`);
    process.exit(2);
  }
  argv.splice(0, 2);
} else if (argv[0] === '-max') {
  arg = /^\d+$/.test(argv[1]) ? parseInt(argv[1], 10) : OP[argv[1]];
  if (arg === undefined) { console.error(`opscan: no such opcode '${argv[1]}'`); process.exit(2); }
  argv.splice(0, 2);
}
if (!argv.length) {
  console.error('usage: opscan.mjs [-q] [-rung NAME | -needs NAME | -max N] image.c4r [...]');
  console.error('       opscan.mjs -rungs | -machine');
  process.exit(2);
}

let bad = 0;
for (const path of argv) {
  const used = scan(path);
  const list = [...used.keys()].sort((a, b) => a - b).map(opname).join(' ');

  if (mode === 'max') {
    const over = [...used].filter(([op]) => op > arg).sort((a, b) => a[0] - b[0]);
    if (over.length) {
      bad = 1;
      const [op, at] = over[0];
      console.log(`${path}: USES ${opname(op)} (${op}) at code+${at}` +
                  (over.length > 1 ? ` and ${over.length - 1} more` : ''));
    } else {
      console.log(`${path}: ok, nothing above ${opname(arg)}`);
    }
  } else {
    const ok = permitted(arg);
    // Sorted, so the opcode named is the same one on every run and on
    // every machine. A test that names a different instruction each
    // time is a test nobody trusts.
    const outside = [...used].filter(([op]) => !ok.has(op)).sort((a, b) => a[0] - b[0]);
    if (mode === 'rung') {
      if (outside.length) {
        bad = 1;
        const [op, at] = outside[0];
        console.log(`${path}: ABOVE RUNG ${arg} -- uses ${opname(op)} (${op}) at code+${at}` +
                    (outside.length > 1 ? ` and ${outside.length - 1} more` : ''));
      } else {
        console.log(`${path}: ok at rung ${arg} (${RUNGS[arg].what})`);
      }
    } else {                                    // -needs
      if (outside.length) {
        const shown = outside.slice(0, 4).map(([op]) => opname(op)).join(' ');
        console.log(`${path}: needs more than rung ${arg} -- ${shown}` +
                    (outside.length > 4 ? ` +${outside.length - 4} more` : ''));
      } else {
        bad = 1;
        console.log(`${path}: FITS rung ${arg}, so it does not require the machine ` +
                    `to be extended -- the milestone that says it does is a story, not a fact`);
      }
    }
  }
  if (!quiet) console.log(`  uses: ${list}`);
}
process.exit(bad);
