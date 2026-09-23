// test-web-gui.mjs - the display legs of test-web.mjs (run with --gui).
//
// M3: the standalone demo on a bare machine. The canvas has to change
// between presents, follow a real mouse, and take a click and keys typed
// with the display focused.
// M4: the same demo as a C4IX program started from the shell, with the
// terminal still live beside it.

const canvasHash = `(() => { const c = document.getElementById('display');
  const d = c.getContext('2d').getImageData(0, 0, c.width, c.height).data;
  let h = 0; for (let i = 0; i < d.length; i += 97) h = (h * 31 + d[i]) | 0; return h; })()`;
const canvasPoint = (fx, fy) => `(() => { const r = document.getElementById('display').getBoundingClientRect();
  return { x: r.left + r.width * ${fx}, y: r.top + r.height * ${fy} }; })()`;

async function mouseAndKeys(t, { tab, ok, waitTerm, sleep }) {
  const h1 = await tab.eval(canvasHash);
  await sleep(500);
  const h2 = await tab.eval(canvasHash);
  ok(`${t}: the picture moves between presents`, h1 !== h2);
  const p = await tab.eval(canvasPoint(0.5, 0.5));
  await tab.mouse('mouseMoved', p.x, p.y);
  await tab.mouse('mousePressed', p.x, p.y, 'left', 1);
  await tab.mouse('mouseReleased', p.x, p.y, 'left', 0);
  ok(`${t}: a click on the display reaches the program`, await waitTerm('gui: click at 3', 15000));
  await tab.key('k');
  ok(`${t}: a key typed on the display reaches the program`, await waitTerm("gui: key 75 'k'", 15000));
}

export async function run(ctx) {
  const { tab, open, ok, waitTerm, focusTerm, sleep, shot, term } = ctx;

  // ---- M3: bare machine ----------------------------------------------
  console.log('test-web: the GUI demo on a bare machine');
  tab.errors.length = 0;
  const base = await open('system=gui-demo');
  if (!ok('the page loads', !!base)) return;
  ok('the demo sees the display', await waitTerm('gui: fitted, 640x480', 30000), (await term()).slice(-300));
  ok('frames are drawn', await tab.waitFor('window.c4m.status.gui && window.c4m.status.gui.presents > 10', 15000));
  await mouseAndKeys('bare', ctx);
  await shot('gui-demo');
  await tab.key('q');
  ok('q on the display ends it', await waitTerm('gui: done after', 15000));
  ok('the machine halts cleanly', await tab.waitFor('window.c4m.exited && window.c4m.exited.status === 0', 15000));
  ok('no page errors', tab.errors.length === 0, tab.errors.join('\n       '));

  // ---- the framebuffer, on the shared path and the message path ----------
  // Paced first, as a person sees it; then unpaced (?fast), which pushes
  // the display path as hard as the machine can, once each way.
  console.log('test-web: the framebuffer demo');
  tab.errors.length = 0;
  await open('system=fb-demo');
  ok('the page is cross-origin isolated', await tab.eval('crossOriginIsolated === true'));
  ok('the framebuffer demo starts', await waitTerm('fb: 320x240 framebuffer', 30000), (await term()).slice(-300));
  ok('framebuffer frames arrive', await tab.waitFor('window.c4m.status.gui && window.c4m.status.gui.flips > 10', 20000));
  ok('on the shared path', await tab.eval('window.c4m.status.gui.shared === true'));
  {
    const h1 = await tab.eval(canvasHash);
    await sleep(500);
    ok('the framebuffer animates', h1 !== await tab.eval(canvasHash));
    const p = await tab.eval(canvasPoint(0.25, 0.25));
    await tab.mouse('mouseMoved', p.x, p.y);
    await sleep(300);
    await shot('fb-demo');
    await tab.eval("document.getElementById('display').focus()");
    await tab.key('q');
    const done = await waitTerm('fb: done after', 15000);
    ok('q ends it, and the events came by interrupt', done && /[1-9]\d* event interrupts, 1 keys/.test(await term()),
       (await term()).slice(-200));
  }
  ok('no page errors', tab.errors.length === 0, tab.errors.join('\n       '));

  const measure = async (label, query) => {
    await open(query);
    await tab.waitFor('window.c4m.status.gui && window.c4m.status.gui.flips > 20', 30000);
    await sleep(1000);
    const probe = `(() => { const s = window.c4m.status, d = window.c4m.display;
      return { flips: s.gui.flips, drawn: d.framesDrawn, busy: s.busy, shared: s.gui.shared, t: performance.now() }; })()`;
    // the page's main thread: how late animation frames run while this goes on
    const jank = `new Promise(res => { let n = 0, worst = 0, last = performance.now(); const f = t => {
      worst = Math.max(worst, t - last); last = t; if (++n < 120) requestAnimationFrame(f); else res(worst); };
      requestAnimationFrame(f); })`;
    const a = await tab.eval(probe);
    const worst = await tab.eval(jank);
    const b = await tab.eval(probe);
    const dt = (b.t - a.t) / 1000;
    const r = { label, shared: b.shared, flips: Math.round((b.flips - a.flips) / dt),
                drawn: Math.round((b.drawn - a.drawn) / dt), busy: Math.round(b.busy * 100), worst: Math.round(worst) };
    console.log(`  (${label}: guest flips ${r.flips}/s, page draws ${r.drawn}/s, worker ${r.busy}% busy, slowest page frame ${r.worst} ms)`);
    return r;
  };
  const sh = await measure('fb-demo, shared, unpaced', 'system=fb-demo&fast');
  const msg = await measure('fb-demo, messages, unpaced', 'system=fb-demo&fast&shared=0');
  ok('unpaced, the shared path is taken and ?shared=0 turns it off', sh.shared === true && msg.shared === false);
  ok('unpaced, both paths keep drawing', sh.drawn > 10 && msg.drawn > 10);
  ok('no page errors', tab.errors.length === 0, tab.errors.join('\n       '));

  // ---- raycast at 640x480, flat out: the display path is the load -------
  console.log('test-web: raycast -G 640x480');
  tab.errors.length = 0;
  const rs = await measure('raycast, shared', 'system=raycast');
  ok('raycast draws on the display', rs.drawn > 10 && rs.flips > 10);
  {
    const h1 = await tab.eval(canvasHash);
    await sleep(300);
    ok('the view moves', h1 !== await tab.eval(canvasHash));
    await shot('raycast');
    await tab.eval("document.getElementById('display').focus()");
    await tab.key('q');
    ok('q on the display quits it', await waitTerm('raycast: ', 15000) && await tab.waitFor('window.c4m.exited && window.c4m.exited.status === 0', 15000),
       (await term()).slice(-200));
  }
  const rm = await measure('raycast, messages', 'system=raycast&shared=0');
  ok('raycast on the message path still draws', rm.drawn > 1);
  console.log(`  (raycast: shared ${rs.flips} guest f/s vs messages ${rm.flips}; slowest page frame ${rs.worst} vs ${rm.worst} ms)`);
  ok('no page errors', tab.errors.length === 0, tab.errors.join('\n       '));

  // ---- the C4IX desktop -------------------------------------------------------
  // The canvas cannot be read as text, so the display records the strings
  // of the last frame it showed (display.recordText) and this reads those.
  console.log('test-web: the C4IX desktop');
  tab.errors.length = 0;
  await open('system=c4ix');
  if (!ok('C4IX boots to its shell', await waitTerm('c4ix:/$', 90000))) return;
  await tab.eval('window.c4m.display.recordText = true');
  await focusTerm();
  await tab.type('desktop\n');
  ok('desktop starts from the shell', await waitTerm('desktop: running on the display', 30000), (await term()).slice(-300));
  const shown = `window.c4m.display.shownText.map(t => t.s).join('\\n')`;
  const shownHas = (re, t = 30000) => tab.waitFor(`${re}.test(${shown})`, t);
  ok('the display becomes 800x600', await tab.waitFor("document.getElementById('display').width === 800", 15000));
  ok('a terminal window shows the shell prompt', await shownHas('/c4ix:\\/\\$/', 60000));
  // desktop coordinates to the page's
  const at = async (x, y) => tab.eval(canvasPoint(x / 800, y / 600));
  const clickAt = async (x, y) => {
    const p = await at(x, y);
    await tab.mouse('mouseMoved', p.x, p.y); await tab.mouse('mousePressed', p.x, p.y, 'left', 1); await tab.mouse('mouseReleased', p.x, p.y, 'left', 0);
    await sleep(150);
  };
  await clickAt(300, 200);                      // into the terminal: focus the canvas and the window
  await tab.type('ps\n');
  ok('ps typed on the display runs in that window', await shownHas('/c4ix-desktop/') && await shownHas('/tasks,/'));
  await clickAt(20, 582);                       // Start
  ok('Start opens its menu', await shownHas('/Shut Down/'));
  await clickAt(80, 600 - 30 - 90 + 6 + 12);    // Terminal
  ok('a second terminal opens from the Start menu', await tab.waitFor(`(${shown}.match(/Terminal \\(task/g) || []).length >= 4`, 30000));
  // drag the second window by its title bar
  {
    const a = await at(128 + 250, 54 + 10), b = await at(128 + 270, 54 + 170);   // mostly down: 800 wide leaves little room to the right
    await tab.mouse('mouseMoved', a.x, a.y); await tab.mouse('mousePressed', a.x, a.y, 'left', 1);
    for (let k = 1; k <= 8; k++) await tab.mouse('mouseMoved', a.x + (b.x - a.x) * k / 8, a.y + (b.y - a.y) * k / 8, 'left', 1);
    await tab.mouse('mouseReleased', b.x, b.y, 'left', 0);
    ok('a window drags by its title bar', await tab.waitFor(`window.c4m.display.shownText.some(t => t.s.startsWith('Terminal (task') && t.x > 160 && t.y > 200 && t.y < 560)`, 15000));
  }
  await sleep(500);
  await shot('desktop');
  // close the dragged window: its close button is at (x+630, y+6)
  await clickAt(148 + 630 + 8, 214 + 6 + 6);
  ok('its close button closes it', await tab.waitFor(`(${shown}.match(/Terminal \\(task/g) || []).length === 2`, 30000));
  await clickAt(20, 582);
  await clickAt(80, 600 - 30 - 90 + 6 + 2 * 26 + 12);   // Shut Down...
  ok('Shut Down hands the console back', await waitTerm('desktop: shut down', 30000));
  ok('no page errors', tab.errors.length === 0, tab.errors.join('\n       '));

  // ---- M4: a C4IX program ----------------------------------------------
  if (process.argv.includes('--no-c4ix')) return;
  console.log('test-web: gui under C4IX');
  tab.errors.length = 0;
  await open('system=c4ix');
  if (!ok('C4IX boots to its shell', await waitTerm('c4ix:/$', 90000))) return;
  await focusTerm();
  await tab.type('gui &\n');
  ok('gui starts from the shell', await waitTerm('gui: fitted, 640x480', 30000), (await term()).slice(-300));
  await sleep(1000);
  await focusTerm();
  await tab.type('ps\n');
  ok('the terminal still answers while it draws, and ps lists it',
     await tab.waitFor(`/c4ix-gui/.test(document.getElementById('terminal').innerText.split('\\nps\\n').pop())`, 15000),
     (await term()).slice(-500));
  await mouseAndKeys('c4ix', ctx);
  const st = await tab.eval('window.c4m.status');
  console.log(`  (while gui runs under C4IX: ${st.state}, cpu ${Math.round(st.busy * 100)}%, ${st.gui.presents} frames)`);
  await shot('gui-c4ix');
  await tab.key('q');
  ok('q on the display ends it', await waitTerm('gui: done after', 15000));
  await focusTerm();
  await tab.type('echo back-at-the-prompt\n');
  ok('the shell carries on', await waitTerm('back-at-the-prompt\n', 15000) || await waitTerm('back-at-the-prompt', 1));
  ok('no page errors', tab.errors.length === 0, tab.errors.join('\n       '));
}
