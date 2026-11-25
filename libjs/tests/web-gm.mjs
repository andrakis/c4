// web-gm.mjs - General MIDI music in the browser (docs/libjs-design.md,
// "The sound card"): each game booted with ?music=gm and a SoundFont in
// the Uploads must load it into the worklet's synth, send it MIDI, and
// sound; and with the default (the AdLib) the synth must not be there at
// all. Over CDP, in a tab of its own in one of the user's Chromes
// (cdp.mjs), against a page served from this tree:
//
//   node libjs/serve.mjs PORT --isolate &
//   node libjs/tests/web-gm.mjs http://127.0.0.1:9224 https://PORT.code.home.stargazer.onl SF2URL [SECONDS [SYSTEMS]]
//
// SF2URL is a SoundFont the page can fetch (a path on the same server, as
// /.gm/GeneralUser-GS.sf2 from a copy in the tree, never committed). It is
// put in the page's Uploads as soundfonts/web-gm-check.sf2 for the check
// and taken out again after it. SYSTEMS: comma-separated, of doom, rott,
// duke3d (all three by default).
//
// A key sent over CDP is the page being used, so the AudioContext runs.

import { Browser } from './cdp.mjs';

const [cdp = 'http://127.0.0.1:9224', base = 'http://127.0.0.1:8472', sf = '', secs = '20', systems = 'doom,rott,duke3d'] = process.argv.slice(2);
const SECS = +secs, NAME = 'soundfonts/web-gm-check.sf2';
let fail = 0;
const ok = (what, cond, extra = '') => {
  console.log(`web-gm: ${what} ${cond ? 'OK' : 'FAILED'}${!cond && extra ? '\n  ' + String(extra).slice(-800) : ''}`);
  if (!cond) fail = 1;
  return cond;
};
const sleep = ms => new Promise(r => setTimeout(r, ms));

const b = await Browser.connect(cdp);
console.log(`web-gm: ${b.version}`);
const tab = await b.newTab(false);
const page = async q => {
  await tab.goto(`${base}/libjs/web/?${q}`);
  const up = await tab.waitFor("location.pathname.startsWith('/libjs/web') && !!window.c4m && window.c4m.status.state !== 'loading'", 60000);
  if (!up) throw new Error(`no page at ${base} (not signed in there?): ${await tab.eval('location.href')}`);
};
// SECONDS of a game, a key now and then; then the mixer's stats and how the page says the music plays
const play = async q => {
  await page(q);
  for (let t = 0; t < SECS; t += 2) { await tab.key(t ? 'Escape' : 'Enter'); await sleep(2000); }
  return tab.eval('(async () => ({ stats: await window.c4m.sound.stats(), music: window.c4m.music, shown: document.getElementById("music").textContent }))()');
};
try {
  await page('system=snd-demo&snd=0&persist=0');
  const put = await tab.eval(`(async () => {
    const r = await fetch(${JSON.stringify(sf)});
    if (!r.ok) return 'fetch ' + r.status;
    const bytes = new Uint8Array(await r.arrayBuffer());
    const { MediaStore, indexedDbBackend } = await import('/src/c4bb/web/store.js');
    await new MediaStore(indexedDbBackend()).backend.putFile('uploads', ${JSON.stringify(NAME)}, bytes);
    return bytes.length;
  })()`);
  ok(`the SoundFont is in the Uploads (${put} bytes)`, typeof put === 'number' && put > 0, put);

  for (const s of systems.split(',')) {
    const r = await play(`system=${s}&music=gm&persist=0`);
    const g = r.stats && r.stats.gm;
    console.log(`web-gm: ${s}: ${JSON.stringify({ music: r.music, shown: r.shown, state: r.stats && r.stats.state, how: r.stats && r.stats.how, gm: g })}`);
    ok(`${s}: General MIDI from the SoundFont, playing`,
      r.music.music === 'gm' && r.stats.state === 'running' && g && g.ready && g.presets > 100 && g.messages > 50 && g.peak > 0.01 &&
      r.shown.startsWith('music: General MIDI'), JSON.stringify(r));
  }
  const r = await play('system=doom&persist=0');
  ok('doom with the default: the AdLib, and no synth', r.music.music === 'opl' && r.stats.gm === null && r.shown === '', JSON.stringify(r));
} catch (e) {
  ok('the check ran', false, e.stack || e);
} finally {
  try {
    await tab.eval(`(async () => { const { MediaStore, indexedDbBackend } = await import('/src/c4bb/web/store.js');
      await new MediaStore(indexedDbBackend()).backend.deleteFile('uploads', ${JSON.stringify(NAME)}); })()`);
  } catch { /* the page is gone */ }
  await tab.close();
  b.disconnect();
}
console.log(fail ? 'web-gm: FAILED' : 'web-gm: all OK');
process.exit(fail);
