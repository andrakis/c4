// web-grin-g4.mjs - grin's G4 browser check (docs/grin-plan.md G4): a note
// saved in Notepad is still there when the page is loaded again, and
// ?persist=reset forgets it. Program Manager, File Manager and Notepad are
// photographed on the way. Over CDP, in a tab of its own in one of the
// user's Chromes (cdp.mjs), against a page served from this tree:
//
//   node libjs/serve.mjs 8486 &
//   node libjs/tests/web-grin-g4.mjs http://127.0.0.1:9222 http://HOST:8486 OUTDIR
//
// It starts with ?persist=reset, so the c4dos store is empty, and ends with
// it, so it leaves nothing behind. It also reads the page's IndexedDB to
// see that the note is in the c4dos store and not in another system's.

import { writeFileSync, mkdirSync } from 'node:fs';
import { Browser } from './cdp.mjs';

const [cdp = 'http://127.0.0.1:9222', base = 'http://127.0.0.1:8472', outdir = '/tmp/web-grin-g4'] = process.argv.slice(2);
mkdirSync(outdir, { recursive: true });
let fail = 0;
const ok = (what, cond, extra = '') => {
  console.log(`web-grin-g4: ${what} ${cond ? 'OK' : 'FAILED'}${!cond && extra ? '\n  ' + String(extra).slice(-800) : ''}`);
  if (!cond) fail = 1;
  return cond;
};
const sleep = ms => new Promise(r => setTimeout(r, ms));

const b = await Browser.connect(cdp);
console.log(`web-grin-g4: ${b.version}`);
const tab = await b.newTab(false);
const errors = [];
try {
  const shown = 'window.c4m.display.shownText';
  const shownHas = (s, t = 20000) => tab.waitFor(`${shown}.some(t => t.s.trimEnd() === ${JSON.stringify(s)})`, t);
  const shownNot = (s, t = 20000) => tab.waitFor(`!${shown}.some(t => t.s.trimEnd() === ${JSON.stringify(s)})`, t);
  const find = async s => tab.eval(`${shown}.filter(t => t.s.trimEnd() === ${JSON.stringify(s)})`);
  const at = async (x, y) => {
    const w = await tab.eval("document.getElementById('display').width"), h = await tab.eval("document.getElementById('display').height");
    return tab.eval(`(() => { const r = document.getElementById('display').getBoundingClientRect();
      return { x: r.left + r.width * ${x / w}, y: r.top + r.height * ${y / h} }; })()`);
  };
  const clickAt = async (x, y) => {
    const p = await at(x, y);
    await tab.mouse('mouseMoved', p.x, p.y); await tab.mouse('mousePressed', p.x, p.y, 'left', 1); await tab.mouse('mouseReleased', p.x, p.y, 'left', 0);
    await sleep(60);
  };
  const clickT = async (t, dbl = false) => { if (!t) return false; await clickAt(t.x + 5, t.y + 5); if (dbl) await clickAt(t.x + 5, t.y + 5); await sleep(400); return true; };
  const clickText = async (s, dbl = false) => { const all = await find(s); return clickT(all[all.length - 1], dbl); };
  // a window's menu: the texts 20 px under its title (Windows 3.1's chrome)
  const menu = async (title, label, item) => {
    const t = (await find(title)).pop();
    if (!t) return false;
    const m = (await find(label)).filter(x => x.y === t.y + 20 && Math.abs(x.x - t.x) < 320)[0];
    if (!(await clickT(m))) return false;
    if (!(await shownHas(item, 5000))) return false;
    return clickText(item);
  };
  // Program Manager to the front by its title, a group from its list, an item
  const item = async (group, s) => {
    await clickText('Program Manager');
    await clickT((await find(group)).sort((a, c) => a.x - c.x)[0]);
    const all = await find(s);
    return clickT(all.filter(t => t.x > 140)[0] || all[all.length - 1], true);
  };
  const shot = async name => { writeFileSync(`${outdir}/${name}.png`, await tab.screenshot()); console.log(`web-grin-g4: screenshot ${outdir}/${name}.png`); };
  // the c4dos store's file names, and another system's, from IndexedDB
  const stored = medium => tab.eval(`new Promise(res => { const o = indexedDB.open('c4bb');
    o.onsuccess = () => { const r = o.result.transaction(['files']).objectStore('files').index('medium').getAll(${JSON.stringify(medium)});
      r.onsuccess = () => res(r.result.map(f => f.name)); r.onerror = () => res(null); }; o.onerror = () => res(null); })`);
  const boot = async query => {
    await tab.goto(`${base}/libjs/web/?system=c4dos${query}`);
    const up = await tab.waitFor('window.c4m && document.getElementById("terminal").innerText.includes("Starting grin")', 90000);
    await tab.eval('window.c4m.display.recordText = true');
    await sleep(500);
    await clickAt(620, 300);               // the display gets the keyboard, and grin draws everything again
    return up && await shownHas('Program Manager');
  };

  ok('?persist=reset: the page boots into grin with the c4dos store emptied', await boot('&persist=reset'));
  ok('Program Manager shows Main: File Manager, DOS Prompt, Control Panel, Notepad',
    await shownHas('File Manager') && await shownHas('DOS Prompt') && await shownHas('Notepad'));
  await clickText('Accessories'); await sleep(300); await clickT((await find('Main')).sort((a, c) => a.x - c.x)[0]);
  await shot('grin-g4-progman');

  await item('Main', 'Notepad');
  ok('Main > Notepad opens Notepad', await shownHas('Untitled - Notepad', 30000));
  await tab.type('Saved in the browser by grin.');
  ok('Notepad takes typing', await shownHas('Saved in the browser by grin.'));
  ok('File > Save As... is the file box, on B:', await menu('Untitled * - Notepad', 'File', 'Save As...') && await shownHas('/B:'));
  await shot('grin-g4-saveas');
  for (let i = 0; i < 16; i++) await tab.key('Backspace');
  await tab.type('NOTE.TXT\n');
  ok('it saves as NOTE.TXT', await shownHas('NOTE.TXT - Notepad'));
  await shot('grin-g4-notepad');

  await item('Main', 'File Manager');
  ok('File Manager lists NOTE.TXT on B:', await shownHas('File Manager') && await shownHas('NOTE.TXT'));
  await clickText('NOTE.TXT');
  ok('... with its size when selected', await tab.waitFor(`${shown}.some(t => /^B:NOTE.TXT +29 bytes$/.test(t.s.trimEnd()))`, 10000));
  await shot('grin-g4-filemgr');
  await sleep(1500);                       // the store's write, from the worker's message
  const c4dos = await stored('c4dos'), doom = await stored('doom');
  ok("NOTE.TXT is in IndexedDB's c4dos store", c4dos && c4dos.includes('NOTE.TXT'), JSON.stringify(c4dos));
  ok('... and not in the doom store', doom && !doom.includes('NOTE.TXT'), JSON.stringify(doom));

  ok('the page loaded again (no reset): grin comes up', await boot(''));
  await item('Main', 'File Manager');
  ok('... and File Manager still lists NOTE.TXT on B:', await shownHas('File Manager') && await shownHas('NOTE.TXT'));
  await clickText('NOTE.TXT', true);
  ok('... which opens in Notepad with its text', await shownHas('NOTE.TXT - Notepad', 30000) && await shownHas('Saved in the browser by grin.'));
  await shot('grin-g4-reloaded');

  ok('?persist=reset: grin again', await boot('&persist=reset'));
  await item('Main', 'File Manager');
  ok('... and NOTE.TXT is forgotten', await shownHas('File Manager') && await shownNot('NOTE.TXT', 3000));
  const after = await stored('c4dos');
  ok('... the c4dos store has no NOTE.TXT', after && !after.includes('NOTE.TXT'), JSON.stringify(after));
  ok('no page errors', tab.errors.length === 0, tab.errors.join('\n  '));
} finally {
  await tab.close();
  b.disconnect();
}
console.log(fail ? 'web-grin-g4: FAILED' : 'web-grin-g4: OK');
process.exit(fail);
