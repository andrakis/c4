// web-c4ix-mmu.mjs - the MMU in the page (docs/c4ix-mmu.md): C4IX booted to
// its desktop with ?mmu=1 and without, each timed from the page's load to
// the desktop running; the kernel's log in a Terminal (dmesg) says the
// MMU is on; and the Configuration dialog's Machine tab has the option.
// Over CDP, in a tab of its own in one of the user's Chromes (cdp.mjs),
// against a page served from this tree:
//
//   node libjs/serve.mjs PORT &
//   node libjs/tests/web-c4ix-mmu.mjs http://127.0.0.1:9224 http://127.0.0.1:PORT
//
// Every boot is ?persist=0, so it leaves nothing in the page's store, and
// ?fast, unpaced, so that the times are the machine's work, not its clock.
// The console's shell runs top once the desktop is started, which clears
// the Terminal, so what is looked for is in everything the machine has
// written to it (the page's 'out' events), not on its screen.

import { Browser } from './cdp.mjs';

const [cdp = 'http://127.0.0.1:9224', base = 'http://127.0.0.1:8512'] = process.argv.slice(2);
let fail = 0;
const ok = (what, cond, extra = '') => {
  console.log(`web-c4ix-mmu: ${what} ${cond ? 'OK' : 'FAILED'}${!cond && extra ? '\n  ' + String(extra).slice(-800) : ''}`);
  if (!cond) fail = 1;
  return cond;
};
const sleep = ms => new Promise(r => setTimeout(r, ms));

const b = await Browser.connect(cdp);
console.log(`web-c4ix-mmu: ${b.version}`);
const tab = await b.newTab(false);
try {
  // everything written to the Terminal since the page loaded
  const out = () => tab.eval('window.__out');
  const status = () => tab.eval('document.getElementById("status").textContent');
  const boot = async mmu => {
    const t0 = Date.now();
    await tab.goto(`${base}/libjs/web/?system=c4ix&persist=0&fast&mmu=${mmu}`);
    await tab.eval(`window.__out = ''; const td = new TextDecoder();
      window.c4m.on('out', b => { window.__out += td.decode(new Uint8Array(b), { stream: true }); })`);
    let t = '';
    for (let i = 0; i < 1200 && !/desktop: running on the display/.test(t); i++) { await sleep(250); t = await out(); }
    const s = (Date.now() - t0) / 1000;
    ok(`?mmu=${mmu}: the desktop is up in ${s.toFixed(1)} s (${await status()})`, /desktop: running on the display/.test(t), t);
    return s;
  };
  // twice each, alternately, and the better of the two: the first boot
  // also fills the browser's cache with the disk
  let off = Infinity, on = Infinity;
  for (let k = 0; k < 2; k++) {
    off = Math.min(off, await boot(0));
    on = Math.min(on, await boot(1));
  }
  console.log(`web-c4ix-mmu: boot to the desktop: ${off.toFixed(1)} s without the MMU, ${on.toFixed(1)} s with it`);
  // the kernel's log, on the console's shell (top runs there: Ctrl-C it)
  await tab.eval('window.c4m.signal(2)');
  await sleep(1000);
  await tab.eval("window.__out = ''; window.c4m.write('dmesg\\n')");
  let t = '';
  for (let i = 0; i < 80 && !/\] mmu: CPU 0/.test(t); i++) { await sleep(250); t = await out(); }
  ok('dmesg says the MMU is on', /\] mmu: CPU 0 translating user mode/.test(t) && /host: c4m \(libjs\)[^\n]*MMU/.test(t), t);
  // the Configuration dialog's Machine tab
  await tab.eval('document.getElementById("configure").click()');
  await sleep(300);
  const sel = await tab.eval(`(() => { const s = document.getElementById('cfg-mmu');
    return s ? s.value + ' ' + [...s.options].map(o => o.value).join(',') + ' ' + !s.closest('.cfg-panel').hidden : 'none'; })()`);
  ok(`the Configuration dialog's Machine tab has the MMU (${sel})`, /^1 0,1 true$/.test(sel), sel);
} finally {
  await tab.close();
  b.disconnect();
}
console.log(`web-c4ix-mmu: ${fail ? 'FAILED' : 'OK'}`);
process.exit(fail);
