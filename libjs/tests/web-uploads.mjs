// web-uploads.mjs - the page's Uploads folders, Saved Files and C4IX's
// download, in the browser (libjs/README.md, "Uploads" and "Downloads").
// Over CDP, in a tab of its own in one of the user's Chromes (cdp.mjs),
// against a page served from this tree:
//
//   node libjs/serve.mjs PORT --isolate &
//   node libjs/tests/web-uploads.mjs http://127.0.0.1:9222 https://PORT.code.home.stargazer.onl OUTDIR
//
// Its own files only: uploads named zz-webtest-*, put in the page's store
// and taken out again; the C4IX desktop boots with ?persist=reset. What
// the page would save as a download is caught before the browser saves
// it (the anchor's click), so nothing lands in the browser's downloads.

import { writeFileSync, mkdirSync } from 'node:fs';
import { Browser } from './cdp.mjs';

const [cdp = 'http://127.0.0.1:9222', base = 'http://127.0.0.1:8493', outdir = '/tmp/web-uploads'] = process.argv.slice(2);
mkdirSync(outdir, { recursive: true });
let fail = 0;
const ok = (what, cond, extra = '') => {
  console.log(`web-uploads: ${what} ${cond ? 'OK' : 'FAILED'}${!cond && extra ? '\n  ' + String(extra).slice(-800) : ''}`);
  if (!cond) fail = 1;
  return cond;
};
const store = 'window.c4m.store';
const MINE = [['zz-webtest-a.txt', 'in /home\n'], ['wolf3d/zz-webtest-b.txt', 'in /home/wolf3d\n']];

// the page's downloads, caught: [{ name, bytes: [..] }] in window.__dl
const CATCH = `(() => { if (window.__dl) return true; window.__dl = [];
  HTMLAnchorElement.prototype.click = function () {
    if (this.download && this.href.startsWith('blob:')) {
      const name = this.download;
      fetch(this.href).then(r => r.arrayBuffer()).then(b => window.__dl.push({ name, bytes: [...new Uint8Array(b)] }));
      return;
    }
    return HTMLElement.prototype.click.call(this);
  }; return true; })()`;
const dl = tab => tab.eval('JSON.stringify((window.__dl || []).map(d => d.name))').then(JSON.parse, () => []);
const shot = async (tab, name) => { try { writeFileSync(`${outdir}/${name}.png`, Buffer.from(await tab.screenshot(), 'base64')); } catch { /* none */ } };

const b = await Browser.connect(cdp);
console.log(`web-uploads: ${b.version}`);
const tab = await b.newTab(false);
try {
  // ---- C4IX at its console: the uploads in /home and /home/wolf3d, and download ----
  const consolePage = `${base}/libjs/web/?image=../../src/c4bb/images/c4ix32.c4r&disk=../../src/c4bb/images/disk&mmu=1`;
  await tab.goto(consolePage);
  if (await tab.eval('location.pathname') !== '/libjs/web/') {
    ok('the page loads (this browser must be signed in to the proxy on that port)', false, await tab.eval('location.href'));
    throw new Error('not the page');
  }
  ok('the test uploads go into the page\'s store, one in a folder', !!await tab.waitFor(`(async () => { const s = ${store};
    for (const [n, t] of ${JSON.stringify(MINE)}) await s.backend.putFile('uploads', n, new TextEncoder().encode(t));
    return true; })()`, 60000));
  await tab.goto(consolePage);
  await tab.waitFor(CATCH, 30000);
  const termText = "document.getElementById('terminal').textContent";
  ok('C4IX boots to its shell', !!await tab.waitFor(`${termText}.includes('c4ix:/$')`, 180000), await tab.eval(termText).catch(() => ''));
  const sh = async (cmd, want, t = 60000) => {
    const n = (await tab.eval(`${termText}.length`)) | 0;
    await tab.eval(`window.c4m.write(${JSON.stringify(cmd + '\n')})`);
    await tab.waitFor(`${termText}.slice(${n}).includes(${JSON.stringify(want)})`, t);
    return tab.eval(`${termText}.slice(${n})`);
  };
  let said = await sh('ls /home/wolf3d', 'zz-webtest-b.txt');
  ok('the folder\'s upload is in /home/wolf3d', said.includes('zz-webtest-b.txt'), said);
  said = await sh('cat /home/wolf3d/zz-webtest-b.txt', 'in /home/wolf3d');
  ok('and reads as uploaded', said.includes('in /home/wolf3d'), said);
  said = await sh('download /home/zz-webtest-a.txt', 'download:');
  ok('download hands a file to the page, which saves it',
     !!await tab.waitFor(`(window.__dl || []).some(d => d.name === 'zz-webtest-a.txt' && new TextDecoder().decode(new Uint8Array(d.bytes)) === 'in /home\\n')`, 20000),
     said + ' ' + JSON.stringify(await dl(tab)));
  said = await sh('download /home/wolf3d', 'download:');
  ok('and a folder, as a tar with what is in it', !!await tab.waitFor(`(async () => { const { untar } = await import('/libjs/kept.js');
      const d = (window.__dl || []).find(d => d.name === 'wolf3d.tar'); if (!d) return false;
      return untar(new Uint8Array(d.bytes)).files.some(([n, b]) => n === 'wolf3d/zz-webtest-b.txt' && new TextDecoder().decode(b) === 'in /home/wolf3d\\n'); })()`, 20000),
     said + ' ' + JSON.stringify(await dl(tab)));

  // ---- the Uploads dialog: folders, a drop into one, Forget ----
  await tab.eval("document.getElementById('uploadsbtn').click()");
  ok('the dialog lists each upload with its folder', !!await tab.waitFor(`(() => { const t = document.getElementById('uploads-list').innerText;
      return t.includes('/home/wolf3d/zz-webtest-b.txt') && t.includes('/home/zz-webtest-a.txt'); })()`, 10000),
     await tab.eval("document.getElementById('uploads-list').innerText"));
  const opts = await tab.eval("[...document.getElementById('uploads-folder').options].map(o => o.textContent).join(' ')");
  ok('the folder list: /home and a folder each for the five games', opts === '/home /home/wolf3d /home/duke3d /home/quake /home/doom /home/rott', opts);
  // pick /home/doom, and drop a file on the page
  ok('a file dropped with /home/doom picked goes in doom/, lower case', !!await tab.waitFor(`(async () => {
      const sel = document.getElementById('uploads-folder'); sel.value = 'doom'; sel.dispatchEvent(new Event('change'));
      const dt = new DataTransfer(); dt.items.add(new File(['dropped\\n'], 'ZZ-WEBTEST-C.TXT'));
      window.dispatchEvent(new DragEvent('drop', { dataTransfer: dt, cancelable: true }));
      for (let i = 0; i < 50; i++) { await new Promise(r => setTimeout(r, 100));
        const m = await ${store}.load('uploads'); if (m.has('doom/zz-webtest-c.txt')) return true; }
      return false; })()`, 20000));
  ok('and the list shows it in /home/doom/', !!await tab.waitFor("document.getElementById('uploads-list').innerText.includes('/home/doom/zz-webtest-c.txt')", 10000));
  await shot(tab, 'uploads-dialog');
  ok('Forget takes out that one file', !!await tab.waitFor(`(async () => {
      const li = [...document.querySelectorAll('#uploads-list li')].find(l => l.innerText.includes('/home/doom/zz-webtest-c.txt'));
      if (li) li.querySelector('button').click();
      await new Promise(r => setTimeout(r, 500));
      const m = await ${store}.load('uploads');
      return !m.has('doom/zz-webtest-c.txt') && m.has('wolf3d/zz-webtest-b.txt') && !document.getElementById('uploads-list').innerText.includes('zz-webtest-c'); })()`, 20000));
  // the folder picked is remembered for this browser; put it back
  await tab.eval(`(() => { const sel = document.getElementById('uploads-folder'); sel.value = ''; sel.dispatchEvent(new Event('change')); document.getElementById('uploads').close(); })()`);

  // ---- the desktop: what persist keeps, in Saved Files ----
  const desk = `${base}/libjs/web/?system=c4ix&persist=reset&mmu=1`;
  await tab.goto(desk);
  await tab.waitFor(CATCH, 30000);
  const shown = '(window.c4m.display.recordText = true, window.c4m.display.shownText)';
  ok('the desktop comes up, with a Terminal', !!await tab.waitFor(`${shown}.some(t => /c4ix:\\/\\$/.test(t.s))`, 300000));
  ok('Saved Files is offered for C4IX', await tab.eval("getComputedStyle(document.getElementById('savedbtn')).display !== 'none'"));
  // type into the desktop's Terminal: a file, then sync (persist save)
  const r = await tab.eval("(() => { const r = document.getElementById('display').getBoundingClientRect(); return { x: r.left + r.width / 2, y: r.top + r.height / 2 }; })()");
  await tab.mouse('mouseMoved', r.x, r.y); await tab.mouse('mousePressed', r.x, r.y, 'left', 1); await tab.mouse('mouseReleased', r.x, r.y, 'left', 0);
  await tab.type('mkdir /home/zzkept\necho kept by persist > /home/zzkept/zz-webtest-d.txt\nsync\n');
  ok('the Terminal\'s sync keeps it', !!await tab.waitFor(`${shown}.some(t => /persist: [0-9]+ changes/.test(t.s))`, 120000));
  await tab.eval("document.getElementById('savedbtn').click()");
  ok('Saved Files lists it by its path, from the store', !!await tab.waitFor(`(() => { const t = document.getElementById('saved-list').innerText;
      return t.includes('/home/zzkept') && t.includes('zz-webtest-d.txt'); })()`, 20000),
     await tab.eval("document.getElementById('saved-list').innerText").catch(() => ''));
  await shot(tab, 'saved-files');
  await tab.eval(`[...document.querySelectorAll('#saved-list li')].find(l => l.innerText.includes('zz-webtest-d.txt')).querySelector('button').click()`);
  ok('its Download saves the file', !!await tab.waitFor(`(window.__dl || []).some(d => d.name === 'zz-webtest-d.txt' && new TextDecoder().decode(new Uint8Array(d.bytes)) === 'kept by persist\\n')`, 10000),
     JSON.stringify(await dl(tab)));
  await tab.eval(`[...document.querySelectorAll('#saved-list li.folder')].find(l => l.innerText.startsWith('/home/zzkept')).querySelector('button').click()`);
  ok('its folder\'s .tar saves the folder', !!await tab.waitFor(`(async () => { const { untar } = await import('/libjs/kept.js');
      const d = (window.__dl || []).find(d => d.name === 'zzkept.tar'); if (!d) return false;
      return untar(new Uint8Array(d.bytes)).files.some(([n]) => n === 'zzkept/zz-webtest-d.txt'); })()`, 10000), JSON.stringify(await dl(tab)));
  await tab.eval("document.getElementById('saved-all').click()");
  ok('and Download all saves the store as c4ix.tar', !!await tab.waitFor(`(async () => { const { untar } = await import('/libjs/kept.js');
      const d = (window.__dl || []).find(d => d.name === 'c4ix.tar'); if (!d) return false;
      return untar(new Uint8Array(d.bytes)).files.some(([n]) => n === 'c4ix/home/zzkept/zz-webtest-d.txt'); })()`, 10000), JSON.stringify(await dl(tab)));
} catch (e) {
  ok(`the check ran (${e.message})`, false);
} finally {
  // the test's uploads out of the store again, and the C4IX store emptied
  try {
    await tab.waitFor(`(async () => { const s = ${store};
      for (const n of ['zz-webtest-a.txt', 'wolf3d/zz-webtest-b.txt', 'doom/zz-webtest-c.txt']) await s.backend.deleteFile('uploads', n).catch(() => {});
      await s.erase('c4ix'); return true; })()`, 30000);
  } catch (e) { console.log(`web-uploads: could not take the test's files out again: ${e.message}`); }
  await tab.close();
  b.disconnect();
}
console.log(fail ? 'web-uploads: FAILED' : 'web-uploads: OK');
process.exit(fail);
