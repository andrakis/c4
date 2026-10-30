// web-lazy-root.mjs - C4IX's root archive read from drive 1 in the page
// (docs/c4ix-design.md, "The root archive, read where it is kept"): the
// desktop booted in MB megabytes, with the MMU or without; what System
// Monitor says System holds; and, if asked, games started from the
// console once the desktop is up, each in a boot of its own.
// Over CDP, in a tab of its own in one of the user's Chromes (cdp.mjs):
//
//   node libjs/serve.mjs PORT &
//   node libjs/tests/web-lazy-root.mjs CDP BASE MB MMU [PERSIST [GAME...]]
//
// PERSIST 1 boots with the page's store as drive 1 (the first boot after
// a new root archive unpacks it there, the next reads the copy kept); 0,
// the default, is ?persist=0: no drive 1, and the tree unpacked into RAM.

import { Browser } from './cdp.mjs';

const [cdp = 'http://127.0.0.1:9222', base = 'http://127.0.0.1:8499', mb = '64', mmu = '1', persist = '0', ...games] = process.argv.slice(2);
let fail = 0;
const ok = (what, cond, extra = '') => {
  console.log(`web-lazy-root: ${what} ${cond ? 'OK' : 'FAILED'}${!cond && extra ? '\n  ' + String(extra).slice(-800) : ''}`);
  if (!cond) fail = 1;
  return cond;
};
const sleep = ms => new Promise(r => setTimeout(r, ms));

const b = await Browser.connect(cdp);
console.log(`web-lazy-root: ${b.version}, ${mb} MB, mmu=${mmu}, persist=${persist}`);
const tab = await b.newTab(false);
try {
  const out = () => tab.eval('window.__out');
  const shown = '(window.c4m.display.recordText = true, window.c4m.display.shownText)';
  const boot = async () => {
    await tab.goto(`${base}/libjs/web/?system=c4ix${persist === '1' ? '' : '&persist=0'}&fast&mem=${mb}&mmu=${mmu}`);
    if (await tab.eval('location.pathname') !== '/libjs/web/') throw new Error('not the page: ' + await tab.eval('location.href'));
    for (let i = 0; i < 40 && !await tab.eval('!!window.c4m'); i++) await sleep(250);
    await tab.eval(`window.__out = ''; const td = new TextDecoder();
      window.c4m.on('out', b => { window.__out += td.decode(new Uint8Array(b), { stream: true }); })`);
    let t = '';
    for (let i = 0; i < 1200 && !/desktop: running on the display|out of memory|halted/i.test(t + await tab.eval('document.getElementById("status").textContent')); i++) {
      await sleep(250); t = await out();
    }
    await sleep(3000);
    return { t: await out(), status: await tab.eval('document.getElementById("status").textContent') };
  };
  let { t, status } = await boot();
  const up = /desktop: running on the display/.test(t);
  ok(`the desktop is up in ${mb} MB (${status})`, up, t.slice(-600));
  for (const l of t.split('\n')) if (/persist|out of memory/.test(l) && !/^ +\d/.test(l)) console.log('  | ' + l.trim());
  if (up) {
    // System Monitor, from the taskbar's CPU meter: its System row
    const w = await tab.eval("document.getElementById('display').width"), h = await tab.eval("document.getElementById('display').height");
    const p = await tab.eval(`(() => { const r = document.getElementById('display').getBoundingClientRect();
      return { x: r.left + r.width * ${(w - 140) / w}, y: r.top + r.height * ${(h - 18) / h} }; })()`);
    await tab.mouse('mouseMoved', p.x, p.y); await tab.mouse('mousePressed', p.x, p.y, 'left', 1); await tab.mouse('mouseReleased', p.x, p.y, 'left', 0);
    await sleep(2500);
    // its Tasks tab lists System, with what it holds
    const tasks = await tab.waitFor(`${shown}.find(t => t.s.trim() === 'Tasks')`, 15000).catch(() => null);
    if (tasks) {
      const q = await tab.eval(`(() => { const r = document.getElementById('display').getBoundingClientRect();
        return { x: r.left + r.width * ${(tasks.x + 8) / w}, y: r.top + r.height * ${(tasks.y + 6) / h} }; })()`);
      await tab.mouse('mouseMoved', q.x, q.y); await tab.mouse('mousePressed', q.x, q.y, 'left', 1); await tab.mouse('mouseReleased', q.x, q.y, 'left', 0);
      await sleep(2500);
    }
    if (process.env.SHOT) (await import('node:fs')).writeFileSync(process.env.SHOT, await tab.screenshot());
    const row = await tab.waitFor(`(() => { const all = ${shown}; const s = all.find(t => t.s.trim() === 'System');
      return s ? all.filter(t => Math.abs(t.y - s.y) < 3).sort((p, q) => p.x - q.x).map(t => t.s.trim()).join(' ') : null; })()`, 15000).catch(() => null);
    console.log(`web-lazy-root: System Monitor: ${row}`);
    const heap = await tab.eval('window.c4m.status && JSON.stringify(window.c4m.status.heap || null)').catch(() => null);
    if (heap) console.log(`web-lazy-root: heap ${heap}`);
  }
  for (const g of games) {
    if (!up) break;
    ({ t } = await boot());
    // top runs on the console: Ctrl-C it, then the game
    await tab.eval('window.c4m.signal(2)');
    await sleep(1000);
    const n0 = (await out()).length;
    await tab.eval(`window.c4m.write('/usr/games/bin/${g}\\n')`);
    await sleep(25000);
    const said = (await out()).slice(n0);
    const size = await tab.eval("document.getElementById('display').width + 'x' + document.getElementById('display').height");
    // what a program that did not start says (the kernel's, the shell's, the game's own)
    const bad = said.match(/.{0,60}(out of memory|cannot run|killed|not found|exited|page fault|Couldn't malloc).{0,60}/i);
    ok(`${g} starts in ${mb} MB (display ${size})`, !bad && size !== '800x600', bad ? bad[0] : said.slice(-600));
  }
} finally {
  await tab.close();
  b.disconnect();
}
console.log(`web-lazy-root: ${fail ? 'FAILED' : 'OK'}`);
process.exit(fail);
