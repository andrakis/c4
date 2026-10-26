// web-c4dos-ladder.mjs - the Build Ladder's browser check
// (docs/c4dos-ladder.md): ?system=c4dos-ladder in the user's Chrome, over
// CDP, in a tab of its own (cdp.mjs), the climb typed into the terminal as
// a person would, with a screenshot at each rung:
//
//   A:\>     DIR; CD \GAMES\DOOM, DIR, CD \ (the disk's directories)
//   A:\>     DOOM (quit with F10, Y); BUILD; RUN dosload.c4r c4ke.c4r
//   c4sh>    doom; tar x c4ix-src.tar; b4ke -f c4ix.b4k; kboot c4ix.c4r
//   c4ix:/$  doom; exit
//   A:\>     MEM
//
//   node libjs/serve.mjs PORT --isolate &
//   node libjs/tests/web-c4dos-ladder.mjs http://127.0.0.1:9222 https://PORT.example OUTDIR
//
// Says what each step took on the page: wall time, and the machine's own
// clock from the status line (the page paces it at 400 MHz).

import { writeFileSync, mkdirSync } from 'node:fs';
import { Browser } from './cdp.mjs';

const [cdp = 'http://127.0.0.1:9222', base = 'http://127.0.0.1:8472', outdir = '/tmp/web-c4dos-ladder'] = process.argv.slice(2);
mkdirSync(outdir, { recursive: true });
const W = 'web-c4dos-ladder';
let fail = 0;
const ok = (what, cond, extra = '') => {
  console.log(`${W}: ${what} ${cond ? 'OK' : 'FAILED'}${!cond && extra ? '\n  ' + String(extra).slice(-1200) : ''}`);
  if (!cond) fail = 1;
  return cond;
};
const sleep = ms => new Promise(r => setTimeout(r, ms));

const b = await Browser.connect(cdp);
console.log(`${W}: ${b.version}`);
const tab = await b.newTab(false);
const costs = [];
try {
  await tab.goto(`${base}/libjs/web/?system=c4dos-ladder`);
  const term = () => tab.eval('document.getElementById("terminal").innerText');
  const tail = async () => (await term()).slice(-1500);
  // the terminal's text after `from` characters matches re
  const waitRe = (re, from, t) => tab.waitFor(
    `new RegExp(${JSON.stringify(re.source)}).test(document.getElementById("terminal").innerText.slice(${from}))`, t, 500);
  const status = () => tab.eval('document.getElementById("status").textContent');
  const simSecs = async () => { const m = /t ([\d.]+)s/.exec(await status()); return m ? +m[1] : 0; };
  const shot = async name => { writeFileSync(`${outdir}/${name}.png`, await tab.screenshot()); console.log(`${W}: screenshot ${outdir}/${name}.png`); };
  const focusTerm = () => tab.eval('document.getElementById("terminal").focus()');
  const focusDisplay = async () => {
    const p = await tab.eval(`(() => { const r = document.getElementById('display').getBoundingClientRect();
      return { x: r.left + r.width / 2, y: r.top + r.height / 2 }; })()`);
    await tab.mouse('mouseMoved', p.x, p.y); await tab.mouse('mousePressed', p.x, p.y, 'left', 1);
    await tab.mouse('mouseReleased', p.x, p.y, 'left', 0);
    await tab.eval('document.getElementById("display").focus()');
  };
  const F10 = async () => {
    const k = { key: 'F10', code: 'F10', windowsVirtualKeyCode: 121 };
    await tab.send('Input.dispatchKeyEvent', { type: 'rawKeyDown', ...k });
    await sleep(150);
    await tab.send('Input.dispatchKeyEvent', { type: 'keyUp', ...k });
  };
  // one line at a prompt, and the prompt again; what it cost
  const step = async (what, line, prompt, t = 120000) => {
    await focusTerm();
    const from = (await term()).length, w0 = Date.now(), s0 = await simSecs();
    await tab.type(line + '\n');
    const r = await waitRe(prompt, from, t);
    costs.push(`${what}: ${((Date.now() - w0) / 1000).toFixed(1)} s on the page, ${((await simSecs()) - s0).toFixed(1)} s of machine time`);
    return r && (await term()).slice(from);
  };
  // a game: start it, let it draw, shoot it, quit with F10 then Y
  const game = async (what, line, prompt, name) => {
    await focusTerm();
    const from = (await term()).length;
    await tab.type(line + '\n');
    const drawing = await tab.waitFor("document.getElementById('display-info').textContent.includes('frames')", 60000, 500);
    await sleep(4000);
    await shot(name);
    await focusDisplay();
    await F10(); await sleep(600);
    await tab.key('y'); await sleep(600);
    return ok(what, drawing && await waitRe(prompt, from, 60000), await tail());
  };
  const A = /A:\\[^>\n]*>\s*$/, KE = /c4sh>\s*$/, IX = /c4ix:\/\$\s*$/;

  ok('the page boots C4DOS on the ladder disk', await waitRe(/TYPE LADDER\.TXT[^]*A:\\>\s*$/, 0, 90000), await tail());
  ok("drive 1 is the ladder's own store", (await tab.eval('window.c4m.persistName')) === 'ladder');
  await step('TYPE LADDER.TXT', 'TYPE LADDER.TXT', A);
  await shot('1-c4dos');
  let d = await step('DIR at the root', 'DIR', A, 60000);
  ok('DIR at A:\\: the directories', d && /^GAMES  <DIR>$/m.test(d) && /^BUILD  <DIR>$/m.test(d) && !/doom1\.wad/.test(d), d);
  d = await step('CD \\GAMES\\DOOM', 'CD \\GAMES\\DOOM', /A:\\GAMES\\DOOM>\s*$/, 60000);
  d = d && await step('DIR there', 'DIR', /A:\\GAMES\\DOOM>\s*$/, 60000);
  ok('CD \\GAMES\\DOOM and DIR there: Doom and its WAD', d && /doom1\.wad/.test(d), await tail());
  await shot('1b-dirs');
  ok('CD \\ back to the root', await step('CD \\', 'CD \\', A, 60000));
  await game('Doom at A:\\, played and quit (found along APPEND)', 'DOOM', A, '2-doom-c4dos');

  let o = await step('BUILD', 'BUILD', A, 600000);
  ok('BUILD compiles C4KE on the RAM disk', o && o.includes('ram:c4ke.c4r') && o.includes('ram:init.c4r'), await tail());
  o = await step('RUN dosload.c4r c4ke.c4r', 'RUN dosload.c4r c4ke.c4r', KE, 120000);
  ok('dosload boots the C4KE it built', o && o.includes('C4SH - The C4 SHell'), await tail());
  await shot('3-c4ke');
  await game('Doom under C4KE, played and quit', 'doom', KE, '4-doom-c4ke');

  o = await step('tar x c4ix-src.tar', 'tar x c4ix-src.tar', KE, 120000);
  ok("tar unpacks C4IX's sources", o && o.includes('tar: extracted'), await tail());
  o = await step('b4ke -f c4ix.b4k', 'b4ke -f c4ix.b4k', KE, 1200000);
  ok('b4ke builds C4IX', o && o.includes('b4ke: 13 ran, 0 skipped, 0 failed'), await tail());
  await shot('5-b4ke');
  o = await step('kboot c4ix.c4r', 'kboot c4ix.c4r', IX, 120000);
  ok('kboot: C4KE stops and the C4IX it built boots', o && o.includes('c4ke: booting c4ix.c4r') && o.includes('C4IX booting'), await tail());
  await step('ls', 'ls', IX);
  await shot('6-c4ix');
  await game('Doom under C4IX, played and quit', 'doom', IX, '7-doom-c4ix');
  o = await step('exit', 'exit', A, 120000);
  ok('C4IX exits, through C4KE, to A>', o && o.includes('c4ke: c4ix.c4r returned 0'), await tail());
  o = await step('MEM', 'MEM', A);
  ok('MEM at the end', o && /heap: \d+ bytes free/.test(o), await tail());
  await shot('8-back-at-a');
  console.log(`${W}: status: ${await status()}`);
  ok('no page errors', tab.errors.length === 0, tab.errors.join('\n'));
} finally {
  for (const c of costs) console.log(`${W}: ${c}`);
  await tab.close();
  b.disconnect();
}
console.log(`${W}: ${fail ? 'FAILED' : 'OK'}`);
process.exit(fail);
