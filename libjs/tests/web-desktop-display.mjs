// web-desktop-display.mjs - the C4IX desktop's screen sizes and volume in
// the browser (docs/c4ix-desktop.md, part fifteen): the tray's speaker
// opens the volume, its slider and Mute reach the page's sound card, and
// Settings > Screen size gives 640x480, 1024x768 and 800x600 again, the
// canvas taking each and the taskbar, clock and windows laid out on it.
// Over CDP, in a tab of its own in one of the user's Chromes (cdp.mjs),
// against a page served from this tree:
//
//   node libjs/serve.mjs PORT --isolate &
//   node libjs/tests/web-desktop-display.mjs http://127.0.0.1:9222 https://PORT.code.home.stargazer.onl OUTDIR
//
// It boots with ?persist=reset, and leaves the desktop at 800x600 with the
// volume full, as a new page has them.

import { writeFileSync, mkdirSync } from 'node:fs';
import { Browser } from './cdp.mjs';

const [cdp = 'http://127.0.0.1:9224', base = 'http://127.0.0.1:8472', outdir = '/tmp/web-desktop-display'] = process.argv.slice(2);
mkdirSync(outdir, { recursive: true });
let fail = 0;
const ok = (what, cond, extra = '') => {
  console.log(`web-desktop-display: ${what} ${cond ? 'OK' : 'FAILED'}${!cond && extra ? '\n  ' + String(extra).slice(-800) : ''}`);
  if (!cond) fail = 1;
  return cond;
};
const sleep = ms => new Promise(r => setTimeout(r, ms));

const b = await Browser.connect(cdp);
console.log(`web-desktop-display: ${b.version}`);
const tab = await b.newTab(false);
try {
  // the sound card's master volume, as the page is handed it (page.js's SoundOut.play)
  await tab.send('Page.addScriptToEvaluateOnNewDocument', { source: `
    window.__master = -1;
    const hook = setInterval(() => {
      const s = window.c4m && window.c4m.sound;
      if (!s || s.__hooked) return;
      const play = s.play.bind(s);
      s.play = (events, now) => {
        for (const e of events || []) if (e[0] === 7) window.__master = e[2];
        return play(events, now);
      };
      s.__hooked = true;
      clearInterval(hook);
    }, 5);` });
  const shown = 'window.c4m.display.shownText';
  const shownHas = (s, t = 20000) => tab.waitFor(`${shown}.some(t => t.s.trimEnd() === ${JSON.stringify(s)})`, t);
  const find = async s => tab.eval(`${shown}.filter(t => t.s.trimEnd() === ${JSON.stringify(s)})`);
  const size = () => tab.eval("(() => { const c = document.getElementById('display'); return [c.width, c.height]; })()");
  const at = async (x, y) => {
    const [w, h] = await size();
    return tab.eval(`(() => { const r = document.getElementById('display').getBoundingClientRect();
      return { x: r.left + r.width * ${x / w}, y: r.top + r.height * ${y / h} }; })()`);
  };
  const press = async (x, y, type, buttons) => { const p = await at(x, y); await tab.mouse(type, p.x, p.y, type === 'mouseMoved' ? 'none' : 'left', buttons); };
  const clickAt = async (x, y) => {
    await press(x, y, 'mouseMoved', 0); await press(x, y, 'mousePressed', 1); await press(x, y, 'mouseReleased', 0);
    await sleep(60);
  };
  const clickT = async (t, dx = 5) => { if (!t) return false; await clickAt(t.x + dx, t.y + 5); await sleep(400); return true; };
  const clickText = async (s, dx) => { const all = await find(s); return clickT(all[all.length - 1], dx); };
  const key = async (k, vk) => {
    await tab.send('Input.dispatchKeyEvent', { type: 'rawKeyDown', key: k, code: k, windowsVirtualKeyCode: vk });
    await sleep(60);
    await tab.send('Input.dispatchKeyEvent', { type: 'keyUp', key: k, code: k, windowsVirtualKeyCode: vk });
    await sleep(300);
  };
  const clock = async () => (await tab.eval(`${shown}.filter(t => /^\\d+:\\d\\d [AP]M$/.test(t.s))`)).pop();
  const shot = async name => { writeFileSync(`${outdir}/${name}.png`, await tab.screenshot()); console.log(`web-desktop-display: screenshot ${outdir}/${name}.png`); };
  const go = async item => {
    const [, h] = await size();
    await clickAt(20, h - 18);
    if (!await shownHas('Power Off...', 5000)) return false;
    return clickText(item);
  };
  // the screen laid out at w x h: the canvas that size, the taskbar's Go
  // and the clock along its bottom, every string drawn inside it
  const laidOut = async (w, h) => {
    const [cw, ch] = await size();
    const c = await clock();
    const gos = (await find('Go')).filter(t => t.y > h - 30);
    const out = await tab.eval(`${shown}.filter(t => t.x < 0 || t.y < 0 || t.x >= ${w} || t.y >= ${h}).length`);
    return { good: cw === w && ch === h && gos.length > 0 && !!c && c.x > w - 100 && c.y > h - 30 && out === 0, why: JSON.stringify({ cw, ch, c, gos: gos.length, out }) };
  };

  await tab.goto(`${base}/libjs/web/?persist=reset`);
  await tab.waitFor('window.c4m && window.c4m.display', 60000);
  await tab.eval('window.c4m.display.recordText = true');
  const up = await shownHas('Go', 120000);
  await sleep(1500);
  {
    const l = await laidOut(800, 600);
    ok('the page boots the desktop at 800x600', up && l.good, l.why);
  }
  ok('which sets the sound card\'s volume full as it starts', await tab.waitFor('window.__master === 255', 10000), await tab.eval('window.__master'));
  await shot('ix-display-1-800x600');

  // ---- the volume ----
  {
    const c = await clock();
    await clickAt(c.x - 20, c.y + 4);
    ok('the speaker beside the clock opens the volume', await shownHas('Mute', 5000) && await shownHas('100%', 1000));
    const v = (await find('Volume')).pop(), px = v.x - 16, py = v.y - 10, sx = px + 38;
    // a drag on the slider's thumb, from the top to halfway down
    await press(sx, py + 34, 'mouseMoved', 0); await press(sx, py + 34, 'mousePressed', 1);
    for (let k = 1; k <= 5; k++) { await press(sx, py + 34 + k * 10, 'mouseMoved', 1); await sleep(40); }
    await press(sx, py + 84, 'mouseReleased', 0);
    // the canvas is scaled to its pane, so the middle is within a pixel or two: 48-52%
    const mid = await tab.waitFor('window.__master >= 58 && window.__master <= 69', 5000), m = await tab.eval('window.__master');
    ok(`dragging the slider to the middle: about 50%, the card at ${m} of 255`, mid && (await tab.eval(`${shown}.some(t => /^(4[89]|5[012])%$/.test(t.s))`)), m);
    await clickText('Mute', -12);
    ok('Mute: the card muted, keeping the volume', await tab.waitFor(`window.__master === (${m} | 256)`, 5000), await tab.eval('window.__master'));
    await shot('ix-display-2-volume-muted');
    await clickText('Mute', -12);
    await press(sx, py + 34, 'mouseMoved', 0); await press(sx, py + 34, 'mousePressed', 1); await press(sx, py + 34, 'mouseReleased', 0);
    ok('unmuted and full again', await tab.waitFor('window.__master === 255', 5000), await tab.eval('window.__master'));
    await key('Escape', 27);
    ok('Esc closes it', !(await shownHas('Mute', 1500)));
  }

  // ---- the screen's size ----
  ok('Go > Settings shows Screen size', await go('Settings') && await shownHas('Screen size', 5000) && await shownHas('1024x768', 1000));
  for (const [w, h] of [[640, 480], [1024, 768], [800, 600]]) {
    await clickText(`${w}x${h}`);
    await tab.waitFor(`document.getElementById('display').width === ${w}`, 10000);
    await sleep(1500);
    const l = await laidOut(w, h);
    ok(`Settings > Screen size > ${w}x${h}: the canvas, the taskbar, the clock and the windows`, l.good, l.why);
    await shot(`ix-display-3-${w}x${h}`);
  }
  await key('Escape', 27);
} finally {
  await tab.close();
  b.disconnect();
}
process.exit(fail);
