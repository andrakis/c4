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
