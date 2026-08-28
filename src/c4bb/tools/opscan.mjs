// opscan -- which opcodes does this image actually use?
//
//   node src/c4bb/tools/opscan.mjs [-max N] image.c4r [image.c4r...]
//
// The rungs a HOMEWARD player reaches before they have extended the CPU
// -- the BIOS and C4DOS -- must run on base c4 and nothing more, or the
// milestone where they add opcodes to their own machine is a milestone
// they have already been given for free. `c4l.c` refuses such an image
// and names the instruction, but c4l is built for the host's word size
// and these images are 32-bit, so the check lives here instead.
//
// -max defaults to EXIT (38), the last base-c4 opcode.
import { readFileSync } from 'node:fs';
import { parseC4r } from '../sim/loader.js';
import { OPNAMES } from '../sim/devices.js';

const names = OPNAMES.split(',').map(s => s.trim());
const argv = process.argv.slice(2);
let max = 38;                                   // EXIT
if (argv[0] === '-max') { max = parseInt(argv[1], 10); argv.splice(0, 2); }
if (!argv.length) {
  console.error('usage: opscan.mjs [-max N] image.c4r [...]');
  process.exit(2);
}

let bad = 0;
for (const path of argv) {
  const { code } = parseC4r(new Uint8Array(readFileSync(path)));
  const used = new Set();
  // Walk instructions, not words: an operand is data and can be any
  // number at all. Same walk c4l.c's scan_extended does.
  let i = code[0] ? 0 : 1;
  const over = [];
  while (i < code.length) {
    const op = code[i];
    used.add(op);
    if (op > max) over.push([i, op]);
    i += (op <= 7) ? 2 : 1;                     // LEA..ADJ carry an operand
  }
  const list = [...used].sort((a, b) => a - b)
                        .map(o => names[o] ?? `?${o}`).join(' ');
  if (over.length) {
    bad = 1;
    const [at, op] = over[0];
    console.log(`${path}: USES ${names[op] ?? op} (${op}) at code+${at}` +
                (over.length > 1 ? ` and ${over.length - 1} more` : ''));
  } else {
    console.log(`${path}: ok, nothing above ${names[max] ?? max}`);
  }
  console.log(`  uses: ${list}`);
}
process.exit(bad);
