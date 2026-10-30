// web-c4ix-swap.mjs - swap in the page (docs/c4ix-mmu.md 11): C4IX booted
// to its desktop in a 45 MB machine with the MMU on (?mem=45&mmu=1), and
// Doom's and Rise of the Triad's timedemos run from the console's shell,
// each to its end: more than the machine has room for at once, which swap
// makes. Each game's frames and checksum are what they are in a 64 MB
// machine (the same run without swap's help), and no task was killed for
// want of memory. Each game in a machine of its own. A screenshot of each game while it
// plays is written next to the log (SHOTS, default /tmp).
// Over CDP, in a tab of its own in one of the user's Chromes (cdp.mjs),
// against a page served from this tree:
//
//   node libjs/serve.mjs PORT &
//   node libjs/tests/web-c4ix-swap.mjs http://127.0.0.1:9222 https://PORT.code.home.stargazer.onl
//
// MEM (default 45) and REF (default 64): the machine to test, and the one
// whose frames and checksums it must match. PERSIST=1: the page's drive 1
// (its store), which the root archive's files can be read from rather
// than kept in RAM; otherwise ?persist=0, and nothing is left in the store.

import { writeFileSync } from 'node:fs';
import { Browser } from './cdp.mjs';

const [cdp = 'http://127.0.0.1:9222', base = 'http://127.0.0.1:8512'] = process.argv.slice(2);
const MEM = Number(process.env.MEM || 45), REF = Number(process.env.REF || 64);
const SHOTS = process.env.SHOTS || '/tmp';
const PERSIST = process.env.PERSIST === '1';
let fail = 0;
const ok = (what, cond, extra = '') => {
  console.log(`web-c4ix-swap: ${what} ${cond ? 'OK' : 'FAILED'}${!cond && extra ? '\n  ' + String(extra).slice(-1200) : ''}`);
  if (!cond) fail = 1;
  return cond;
};
const sleep = ms => new Promise(r => setTimeout(r, ms));

const games = [
  { name: 'doom', cmd: 'doom -timedemo demo3 -nosound', done: /timedemo: demo3: (\d+) frames, checksum ([0-9a-f]+)/ },
  { name: 'rott', cmd: 'rott --timedemo 4 NOSOUND', done: /timedemo: demo 4: (\d+) frames, checksum ([0-9a-f]+)/ },
];

const b = await Browser.connect(cdp);
console.log(`web-c4ix-swap: ${b.version}`);
const tab = await b.newTab(false);
try {
  // the page can be too busy to answer for a while (a game loading its
  // level): what it said last time, then
  let said = '';
  const out = async () => { try { said = await tab.eval('window.__out'); } catch (e) { console.log(`web-c4ix-swap: (the page is slow: ${e.message})`); } return said; };
  const boot = async mem => {
    const t0 = Date.now();
    await tab.goto(`${base}/libjs/web/?system=c4ix${PERSIST ? '' : '&persist=0'}&fast&mmu=1&mem=${mem}`);
    const loc = await tab.eval('location.href');
    if (!ok(`the page loads (${loc})`, loc.includes('/libjs/web/'), loc)) return false;
    await tab.eval(`window.__out = ''; const td = new TextDecoder();
      window.c4m.on('out', b => { window.__out += td.decode(new Uint8Array(b), { stream: true }); })`);
    let t = '';
    for (let i = 0; i < 1200 && !/desktop: running on the display/.test(t); i++) { await sleep(250); t = await out(); }
    ok(`${mem} MB: the desktop is up in ${((Date.now() - t0) / 1000).toFixed(1)} s`, /desktop: running on the display/.test(t), t);
    // the console's shell runs top once the desktop is up: Ctrl-C it
    await tab.eval('window.c4m.signal(2)');
    await sleep(1000);
    return true;
  };
  // a game's timedemo from the console's shell, to its end: frames, checksum, seconds
  const play = async (g, mem) => {
    await tab.eval(`window.__out = ''; window.c4m.write(${JSON.stringify(g.cmd + '\n')})`);
    said = '';
    const t0 = Date.now();
    let t = '', shot = false;
    for (let i = 0; i < 4 * 900 && !g.done.test(t) && !/killed|page fault|panic/.test(t); i++) {
      await sleep(250);
      t = await out();
      if (!shot && Date.now() - t0 > 20000) {
        shot = true;
        try { writeFileSync(`${SHOTS}/web-c4ix-swap-${g.name}-${mem}.png`, Buffer.from(await tab.screenshot(), 'base64')); }
        catch (e) { console.log(`web-c4ix-swap: no screenshot of ${g.name} (${e.message})`); }
      }
    }
    const m = g.done.exec(t);
    return m ? { frames: Number(m[1]), sum: m[2], s: (Date.now() - t0) / 1000, t } : { frames: 0, sum: '', s: 0, t };
  };
  const dmesg = async () => {
    await tab.eval("window.__out = ''; window.c4m.write('dmesg\\n')");
    await sleep(3000);
    return out();
  };

  // each game in a machine of its own: one after the other in one machine,
  // the first one's WAD stays in the RAM filesystem, which is the kernel's
  // memory, not swap's (docs/c4ix-mmu.md 13)
  for (const g of games) {
    const e = (await boot(REF)) ? await play(g, REF) : {};
    if (!(await boot(MEM))) continue;
    const r = await play(g, MEM);
    ok(`${MEM} MB: ${g.name} ran ${r.frames} frames in ${r.s.toFixed(1)} s, checksum ${r.sum}` +
      ` (${REF} MB: ${e.frames} frames in ${(e.s || 0).toFixed(1)} s, ${e.sum})`,
      r.frames > 0 && r.frames === e.frames && r.sum === e.sum, r.t);
    const t = await dmesg();
    const outs = (t.match(/swap: \d+ KB of task \d+ \([^)]*\) at 0x[0-9a-f]+ out/g) || []).length;
    ok(`${MEM} MB: no task was killed for want of memory (${outs} runs of pages put out)`,
      !/killed|no room|page fault/.test(t), t.split('\n').filter(l => /killed|no room|page fault/.test(l)).join('\n'));
  }
} finally {
  await tab.close();
  b.disconnect();
}
console.log(`web-c4ix-swap: ${fail ? 'FAILED' : 'OK'}`);
process.exit(fail);
