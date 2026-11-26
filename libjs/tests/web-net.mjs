// web-net.mjs - the page's network (docs/c4pc-network.md, N2): two tabs of
// the page, ?net=1 and ?net=2, are machines 1 and 2 (10.4.0.2 and .3) on
// a hub of the page's tabs (a BroadcastChannel, libjs/net.js). C4IX boots
// to its desktop in each; on the console's shell (top runs there: Ctrl-C
// it) b says its address, a pings b by its name, b serves its / once
// (httpd -d -n 1) and a fetches /bin/ from it. Over CDP, in tabs of its own
// in one of the user's Chromes (cdp.mjs), against a page served from this
// tree:
//
//   node libjs/serve.mjs PORT &
//   node libjs/tests/web-net.mjs http://127.0.0.1:9224 https://PORT.code.home.stargazer.onl
//
// Every boot is ?persist=0, so it leaves nothing in the page's store.

import { Browser } from './cdp.mjs';

const [cdp = 'http://127.0.0.1:9224', base = 'http://127.0.0.1:8512'] = process.argv.slice(2);
let fail = 0;
const ok = (what, cond, extra = '') => {
  console.log(`web-net: ${what} ${cond ? 'OK' : 'FAILED'}${!cond && extra ? '\n  ' + String(extra).slice(-800) : ''}`);
  if (!cond) fail = 1;
  return cond;
};
const sleep = ms => new Promise(r => setTimeout(r, ms));

const b = await Browser.connect(cdp);
console.log(`web-net: ${b.version}`);
const tabs = [];
try {
  // machine ID booted in a tab of its own, what it writes kept in window.__out
  const boot = async id => {
    const tab = await b.newTab(false);
    tabs.push(tab);
    await tab.goto(`${base}/libjs/web/?system=c4ix&persist=0&net=${id}`);
    // the page's module sets window.c4m once it has loaded (a sign-in page never does)
    if (!ok(`machine ${id}'s page loaded`, await tab.waitFor('!!window.c4m', 30000), await tab.eval('location.href'))) throw new Error('no page');
    await tab.eval(`window.__out = ''; const td = new TextDecoder();
      window.c4m.on('out', b => { window.__out += td.decode(new Uint8Array(b), { stream: true }); })`);
    return tab;
  };
  const out = tab => tab.eval('window.__out');
  const until = async (tab, re, n = 240) => {
    let t = '';
    for (let i = 0; i < n && !re.test(t); i++) { await sleep(250); t = await out(tab); }
    return t;
  };
  const run = async (tab, line, re, n) => {
    await tab.eval(`window.__out = ''; window.c4m.write(${JSON.stringify(line + '\n')})`);
    return until(tab, re, n);
  };
  const a = await boot(1), c = await boot(2);
  for (const [tab, name] of [[a, 'a'], [c, 'b']]) {
    await tab.send('Page.bringToFront');
    const t = await until(tab, /desktop: running on the display/, 1200);
    ok(`${name}'s desktop is up`, /desktop: running on the display/.test(t), t);
    ok(`${name}'s kernel found the card`, /net0: c4net, MAC 52:54:00:c4:00:0[12]/.test(t), t);
    await tab.eval('window.c4m.signal(2)');
  }
  await sleep(1000);
  let t = await run(c, 'ifconfig', /RX packets/);
  ok("b's card: 10.4.0.3", /inet 10\.4\.0\.3/.test(t), t);
  await a.send('Page.bringToFront');
  t = await run(a, 'ping -c 3 vm2', /packet loss/, 120);
  ok('a pings b (vm2): 3 replies', /3 packets transmitted, 3 received, 0% packet loss/.test(t), t);
  ok('from 10.4.0.3', /64 bytes from 10\.4\.0\.3/.test(t), t);
  const rtt = /rtt min\/avg\/max = [\d/]+ ms/.exec(t);
  if (rtt) console.log(`web-net: ${rtt[0]}`);
  t = await run(c, 'httpd -d -n 1 /', /httpd: serving/);
  ok('b serves its /', /httpd: serving \/ on TCP port 80/.test(t), t);
  t = await run(a, 'echo GET /bin/ HTTP/1.0 | nc vm2 80', /<\/ul>/, 120);
  ok("a fetched b's /bin/ over HTTP", /HTTP\/1\.0 200 OK/.test(t) && /<li><a href="sh">sh<\/a><\/li>/.test(t), t);
  t = await until(c, /GET \/bin\/ 200/, 40);
  ok('b says it served a', /10\.4\.0\.2 GET \/bin\/ 200/.test(t), t);
  for (const tab of tabs) if (tab.errors.length) ok('no exceptions in the page', false, tab.errors.join('\n'));
} finally {
  for (const tab of tabs) await tab.close();
  b.disconnect();
}
console.log(fail ? 'web-net: FAILED' : 'web-net: passed');
process.exit(fail);
