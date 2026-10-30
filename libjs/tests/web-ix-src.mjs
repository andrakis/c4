// web-ix-src.mjs - the games' sources in the IDE's Code tab, in a browser
// (docs/c4ix-desktop.md, part twenty): :e /usr/games/bin/doom (the name
// the shell's PATH finds) and File > Open of /usr/games/wolf3d/wolf3d.c4r
// show main with its C source between the instructions; the
// boot fetched none of the sources, opening one fetched it, and the page
// kept it in IndexedDB, so after a reload it comes from there. Over CDP,
// in a tab of its own in one of the user's Chromes (cdp.mjs), against a
// page served from this tree, run from this tree's root:
//
//   node libjs/serve.mjs PORT --isolate &
//   node libjs/tests/web-ix-src.mjs http://127.0.0.1:9222 https://PORT.code.home.stargazer.onl OUTDIR
//
// It boots with ?persist=reset, and empties the lazy disk's cache first.

import { readFileSync, writeFileSync, mkdirSync, existsSync } from 'node:fs';
import { Browser } from './cdp.mjs';
import { readC4r } from '../c4r.js';

const [cdp = 'http://127.0.0.1:9222', base = 'http://127.0.0.1:8472', outdir = '/tmp/web-ix-src'] = process.argv.slice(2);
mkdirSync(outdir, { recursive: true });
let fail = 0;
const ok = (what, cond, extra = '') => {
  console.log(`web-ix-src: ${what} ${cond ? 'OK' : 'FAILED'}${!cond && extra ? '\n  ' + String(extra).slice(-800) : ''}`);
  if (!cond) fail = 1;
  return cond;
};
const sleep = ms => new Promise(r => setTimeout(r, ms));

// where each game's main is, by its image's map, and the lines of C the
// Code tab should show there: a few of main's first, as they are in the file
const DISK = 'src/c4bb/images/disk';
const games = ['doom', 'wolf3d'].filter(g => existsSync(`${DISK}/c4ix-${g}.c4r`) && existsSync(`${DISK}/games/${g}`)).map((g, i) => {
  const m = readC4r(new Uint8Array(readFileSync(`${DISK}/c4ix-${g}.c4r`))).smap;
  const fn = m.funcs.find(f => f.name === 'main');
  const file = m.files[fn.file].path;
  const lines = readFileSync(file, 'latin1').split('\n').slice(fn.line - 1, fn.line + 14)
    .map(l => l.trim()).filter(l => l.length >= 12 && !l.includes('\t'));
  // the first by its name in /usr/games/bin, the second in its own directory
  return { g, prog: i === 0 ? `/usr/games/bin/${g}` : `/usr/games/${g}/${g}.c4r`, file, line: fn.line, lines, re: new RegExp(`^main \\(\\d+ words, ${file.split('/').pop().replace('.', '\\.')}:${fn.line}\\)$`) };
});
if (!games.length) { console.log('web-ix-src: neither doom nor wolf3d is on the disk, skipped'); process.exit(0); }

const b = await Browser.connect(cdp);
console.log(`web-ix-src: ${b.version}`);
const tab = await b.newTab(false);
try {
  const shown = 'window.c4m.display.shownText';
  const shownHas = (s, t = 20000) => tab.waitFor(`${shown}.some(t => ${s instanceof RegExp ? `${s}.test(t.s)` : `t.s.trimEnd() === ${JSON.stringify(s)}`})`, t);
  const find = async s => tab.eval(`${shown}.filter(t => ${s instanceof RegExp ? `${s}.test(t.s)` : `t.s.trimEnd() === ${JSON.stringify(s)}`})`);
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
  const clickT = async t => { if (!t) return false; await clickAt(t.x + 5, t.y + 5); await sleep(400); return true; };
  const clickText = async s => { const all = await find(s); return clickT(all[all.length - 1]); };
  const escape = async () => { await tab.key('Escape'); await sleep(200); };
  const shot = async name => { writeFileSync(`${outdir}/${name}.png`, await tab.screenshot()); console.log(`web-ix-src: screenshot ${outdir}/${name}.png`); };
  const go = async item => { const g = await find('Go'); await clickT(g[g.length - 1]); if (!await shownHas('Power Off...', 5000)) return false; return clickText(item); };
  const disk = () => tab.eval('window.c4m.status.disk');
  // the page's cache of the lazy disk: the names in it
  const cached = () => tab.eval(`(async () => { const { indexedDbBackend } = await import('/src/c4bb/web/store.js');
    return (await indexedDbBackend().listFiles(window.c4m.cacheName)).map(f => f.name); })()`);
  // the C lines of G's main, on the display
  const source = g => tab.waitFor(`${shown}.filter(t => ${JSON.stringify(g.lines)}.some(l => t.s.includes(l))).length`, 20000);
  const close = async name => {
    const fs = await find(/^File$/);
    await clickT(fs[fs.length - 1]);
    await shownHas('Close', 3000);
    await clickText('Close');
    return tab.waitFor(`!${shown}.some(t => t.s.trimEnd() === '${name} - IDE')`, 5000);
  };
  const boot = async what => {
    await tab.goto(`${base}/libjs/web/?persist=reset&mmu=1`);
    const up = await tab.waitFor('window.c4m && window.c4m.display', 90000);
    await tab.eval('window.c4m.display.recordText = true');
    ok(what, up && await shownHas(/^Terminal - task/, 120000));
    await sleep(1500);
  };

  // ---- an empty cache, and a boot that fetches no source ----
  await boot('?persist=reset: the page boots the C4IX desktop');
  await tab.eval(`(async () => { const { indexedDbBackend } = await import('/src/c4bb/web/store.js');
    await indexedDbBackend().deleteFiles(window.c4m.cacheName); })()`);
  ok('the page knows its lazy disk\'s cache', await tab.eval('window.c4m.cacheName'), await tab.eval('window.c4m.cacheName'));
  await boot('again, with nothing in the cache');
  const d0 = await disk();
  ok('the boot fetched files, none from the cache', d0 && d0.lazy && d0.fetched > 0 && d0.cached === 0, JSON.stringify(d0));
  ok('and kept none: no source was read', (await cached()).length === 0, (await cached()).join(' '));

  // ---- :e /usr/games/bin/doom, and File > Open of /usr/games/wolf3d/wolf3d.c4r ----
  for (const [i, g] of games.entries()) {
    await go('IDE');
    await shownHas('Untitled - IDE');
    const t0 = Date.now();
    if (i === 0) {
      await escape();
      await tab.type(`e ${g.prog}\n`);
    } else {
      const fs = await find(/^File$/);
      await clickT(fs[fs.length - 1]);
      await shownHas('Open...', 3000);
      await clickText('Open...');
      await sleep(800);
      await tab.type(`${g.prog}\n`);
    }
    const how = i === 0 ? `:e ${g.prog}` : `File > Open of ${g.prog}`;
    ok(`${how} opens it in the Code tab, at main (${g.file}:${g.line})`, await shownHas(g.re, 90000));
    ok(`and main's C source is between its instructions`, await source(g), g.lines.join(' | '));
    console.log(`web-ix-src: ${g.prog} shown with its source in ${((Date.now() - t0) / 1000).toFixed(1)} s`);
    await sleep(800);
    await shot(`ix-src-${i + 1}-${g.g}`);
    const kept = await tab.waitFor(`(async () => { const { indexedDbBackend } = await import('/src/c4bb/web/store.js');
      const n = (await indexedDbBackend().listFiles(window.c4m.cacheName)).map(f => f.name);
      return n.some(x => x.endsWith(' ${g.file}')) && n; })()`, 10000);
    ok(`the page keeps ${g.file} in its cache`, kept, (await cached()).join(' '));
    ok('File > Close closes it', await close(g.prog.split('/').pop()));
  }
  const n1 = (await cached()).length;
  console.log(`web-ix-src: the cache holds ${n1} sources: ${(await cached()).join(' ')}`);

  // ---- a reload: the sources come from the cache ----
  await boot('a reload boots the desktop');
  const g = games[0];
  await tab.waitFor('window.c4m.status.disk', 10000);
  await go('IDE');
  await shownHas('Untitled - IDE');
  await escape();
  await tab.type(`e ${g.prog}\n`);
  ok(`:e ${g.prog} again`, await shownHas(g.re, 90000) && await source(g));
  await sleep(1500);
  const d2 = await disk();
  ok('its sources came from the cache', d2.cached > 0, JSON.stringify(d2));
  ok('and the cache has no more in it than before', (await cached()).length === n1, (await cached()).join(' '));
  await shot(`ix-src-3-${g.g}-cached`);
  await close(g.prog.split('/').pop());
  ok('no page errors', tab.errors.length === 0, tab.errors.join('\n  '));
} finally {
  await tab.close();
  b.disconnect();
}
console.log(fail ? 'web-ix-src: FAILED' : 'web-ix-src: OK');
process.exit(fail);
