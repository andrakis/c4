// web-grin.mjs - grin's browser check (docs/grin-plan.md G2): the page
// boots straight into grin; Control Panel's Display switches it to
// 800x600 and back; File > Exit grin comes back to A>, where VER answers.
// Screenshots at both sizes. Over CDP, in a tab of its own in one of the
// user's Chromes (cdp.mjs), against a page served from this tree:
//
//   node libjs/serve.mjs 8479 &
//   node libjs/tests/web-grin.mjs http://127.0.0.1:9222 http://HOST:8479 OUTDIR
//
// The canvas cannot be read as text, so the display records the strings
// of the last frame it showed (display.recordText), and things are
// clicked by the text drawn on them.

import { writeFileSync, mkdirSync } from 'node:fs';
import { Browser } from './cdp.mjs';

const [cdp = 'http://127.0.0.1:9222', base = 'http://127.0.0.1:8472', outdir = '/tmp/web-grin'] = process.argv.slice(2);
mkdirSync(outdir, { recursive: true });
let fail = 0;
const ok = (what, cond, extra = '') => {
  console.log(`web-grin: ${what} ${cond ? 'OK' : 'FAILED'}${!cond && extra ? '\n  ' + String(extra).slice(-800) : ''}`);
  if (!cond) fail = 1;
  return cond;
};
const sleep = ms => new Promise(r => setTimeout(r, ms));

const b = await Browser.connect(cdp);
console.log(`web-grin: ${b.version}`);
const tab = await b.newTab(false);
try {
  await tab.goto(`${base}/libjs/web/?system=c4dos`);
  const term = () => tab.eval('document.getElementById("terminal").innerText');
  const waitTerm = (s, t = 30000) => tab.waitFor(`document.getElementById("terminal").innerText.includes(${JSON.stringify(s)})`, t);
  const width = () => tab.eval("document.getElementById('display').width");
  const shown = 'window.c4m.display.shownText';
  const shownHas = (s, t = 20000) => tab.waitFor(`${shown}.some(t => t.s.trimEnd() === ${JSON.stringify(s)})`, t);
  const at = async (x, y) => {
    const w = await width(), h = await tab.eval("document.getElementById('display').height");
    return tab.eval(`(() => { const r = document.getElementById('display').getBoundingClientRect();
      return { x: r.left + r.width * ${x / w}, y: r.top + r.height * ${y / h} }; })()`);
  };
  const clickAt = async (x, y) => {
    const p = await at(x, y);
    await tab.mouse('mouseMoved', p.x, p.y); await tab.mouse('mousePressed', p.x, p.y, 'left', 1); await tab.mouse('mouseReleased', p.x, p.y, 'left', 0);
    await sleep(60);
  };
  const find = async s => tab.eval(`${shown}.filter(t => t.s.trimEnd() === ${JSON.stringify(s)})`);
  const clickText = async (s, dbl = false) => {
    const all = await find(s);
    const t = all[all.length - 1];
    if (!t) return false;
    await clickAt(t.x + 5, t.y + 5);
    if (dbl) await clickAt(t.x + 5, t.y + 5);
    await sleep(400);
    return true;
  };
  const shot = async name => { writeFileSync(`${outdir}/${name}.png`, await tab.screenshot()); console.log(`web-grin: screenshot ${outdir}/${name}.png`); };

  ok('the page boots C4DOS, and AUTOEXEC.BAT starts grin', await waitTerm('Starting grin', 90000), (await term()).slice(-300));
  ok('the display is 640x480', await tab.waitFor("document.getElementById('display').width === 640", 30000));
  await tab.eval('window.c4m.display.recordText = true');
  // a click on the desktop gives the display the keyboard; the focus
  // change makes grin draw the whole scene again
  await clickAt(620, 300);
  ok('grin is up: Program Manager and its Main group', await shownHas('Program Manager') && await shownHas('Main'));
  await clickText('Control Panel', true);
  ok('Control Panel opens', await shownHas('Display'));
  await shot('grin-640x480');

  await clickText('Display', true);
  ok('Display offers 800 x 600', await shownHas('800 x 600'));
  await clickText('800 x 600');
  await clickText('OK');
  ok('Display > 800 x 600: the canvas is 800x600', await tab.waitFor("document.getElementById('display').width === 800 && document.getElementById('display').height === 600", 15000));
  await sleep(500);
  // the Task List, with Ctrl+Esc, over the windows
  await tab.key('Escape', { ctrl: true });
  ok('Ctrl+Esc opens the Task List', await shownHas('Switch To'));
  await sleep(300);
  await shot('grin-800x600');
  await tab.key('Escape');
  await sleep(300);

  await clickText('Display', true);
  ok('Display again', await shownHas('640 x 480'));
  await clickText('640 x 480');
  await clickText('OK');
  ok('... 640 x 480: the canvas is 640x480 again', await tab.waitFor("document.getElementById('display').width === 640", 15000));
  await sleep(500);

  // File > Exit grin, in Program Manager (in front first: Ctrl+Esc, Switch To)
  await tab.key('Escape', { ctrl: true });
  await shownHas('Switch To');
  const rows = await find('Program Manager');
  const tl = (await find('Switch To'))[0];
  const row = rows.filter(t => tl && t.y < tl.y && t.y > tl.y - 180 && t.x > tl.x - 40).pop();
  if (row) await clickAt(row.x + 5, row.y + 5);
  await clickText('Switch To');
  await clickText('File');
  await clickText('Exit grin...');
  ok('Exit grin asks first', await shownHas('This will end your grin session.'));
  const before = (await term()).split('A>').length;
  await clickText('OK');
  ok('OK: back at A>', await tab.waitFor(`document.getElementById("terminal").innerText.split('A>').length > ${before}`, 15000), (await term()).slice(-300));
  await tab.eval('document.getElementById("terminal").focus()');
  await tab.type('VER\n');
  ok('VER answers', await tab.waitFor(`document.getElementById("terminal").innerText.split('C4DOS version').length > 2`, 15000), (await term()).slice(-300));
  await shot('grin-exited');
  ok('no page errors', tab.errors.length === 0, tab.errors.join('\n  '));
} finally {
  await tab.close();
  b.disconnect();
}
console.log(fail ? 'web-grin: FAILED' : 'web-grin: OK');
process.exit(fail);
