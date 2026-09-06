// drive.mjs -- paced interactive driver for c4bb's cli.js.
//
//   node src/c4bb/tests/drive.mjs <logfile> <secondsTotal> \
//        -- <cli.js args...> :: cmd@sec cmd@sec ...
//
//   node src/c4bb/tests/drive.mjs /tmp/run.log 90 \
//        -- src/c4bb/sim/cli.js -i --fast -m 64 -d src/c4bb/images/disk \
//           src/c4bb/images/c4ke32.c4r :: ls@8 echo_hello@20 ps@32
//
// Underscores in a command stand for spaces, so one shell word is one
// typed line.
//
// Why this exists rather than a pipe. Prefeeding stdin hands the whole
// script to the machine at once, and a guest reads it one line at a
// time -- so a session driven that way tests the queue, not the system,
// and for a long time silently lost every line after the first
// (docs/dos-rung-fixes.md F18). `-i` with paced writes types the way a
// person does. It also streams the machine's output to <logfile> as it
// arrives: buffering and writing at the end loses everything when the
// run is killed, which is exactly when you most want the log.
//
// This is an instrument. Point it at a session you already know works
// before you believe what it says about one that does not.

import { spawn } from 'node:child_process';
import { createWriteStream } from 'node:fs';

const argv = process.argv.slice(2);
const log = argv.shift();
const total = parseInt(argv.shift(), 10);
if (argv.shift() !== '--') throw new Error('expected --');
const sep = argv.indexOf('::');
const cliArgs = argv.slice(0, sep);
const script = argv.slice(sep + 1).map(s => {
  const at = s.lastIndexOf('@');
  return { cmd: s.slice(0, at).replace(/_/g, ' '), at: parseFloat(s.slice(at + 1)) };
});

const out = createWriteStream(log, { flags: 'w' });
const p = spawn('node', cliArgs, { stdio: ['pipe', 'pipe', 'pipe'] });
p.stdout.on('data', d => out.write(d));
p.stderr.on('data', d => out.write(d));

for (const s of script)
  setTimeout(() => { out.write(`\n[drive: typing ${JSON.stringify(s.cmd)} at t=${s.at}s]\n`); p.stdin.write(s.cmd + '\n'); }, s.at * 1000);
setTimeout(() => { out.write(`\n[drive: giving up at t=${total}s]\n`); p.kill('SIGKILL'); process.exit(0); }, total * 1000);
p.on('exit', c => { out.write(`\n[drive: machine exited ${c}]\n`); process.exit(0); });
