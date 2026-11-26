// test-net.mjs -- the network card (net.js, docs/c4pc-network.md N2): three
// NetCards on one BroadcastChannel hub, as a page's tabs are. A broadcast
// reaches the others, a unicast only its MAC's card; CMD 1 and 2 move
// frames through memory; the addresses are the switch's; 64 kept, the rest
// dropped and counted
import { NetCard, ChannelPort } from '../net.js';
const mem = n => ({ u8: new Uint8Array(n) });
const a = new NetCard(new ChannelPort('t'), 1), b = new NetCard(new ChannelPort('t'), 2), c = new NetCard(new ChannelPort('t'), 3);
a.arena = mem(4096); b.arena = mem(4096); c.arena = mem(4096);
let woke = 0; b.onwake = () => woke++;
const frame = (dst, n) => { const f = new Uint8Array(n); f.set(dst, 0); f.set(a.mac, 6); f[12] = 8; f[13] = 0; for (let k = 14; k < n; k++) f[k] = k & 255; return f; };
const send = (card, f) => { card.arena.u8.set(f, 100); card.write32(0x590, 100); card.write32(0x594, f.length); card.write32(0x598, 1); return card.read32(0x598); };
const recv = card => { card.write32(0x590, 2000); card.write32(0x594, 1536); card.write32(0x598, 2); return card.read32(0x598); };
const ok = (c, m) => { console.log(`${c ? 'ok  ' : 'FAIL'}  ${m}`); if (!c) process.exitCode = 1; };
ok(send(a, frame([255, 255, 255, 255, 255, 255], 60)) === 60, 'a sends a broadcast');
ok(send(a, frame(b.mac, 98), 98) === 98, "a sends to b's MAC");
await new Promise(r => setTimeout(r, 50));
ok(b.read32(0x59c) === 2 && b.read32(0x580) === 3, 'b has both (RXQ 2, STATUS link+waiting)');
ok(c.read32(0x59c) === 1, 'c has the broadcast only');
ok(a.read32(0x59c) === 0, 'a has nothing of its own');
ok(recv(b) === 60 && recv(b) === 98 && b.arena.u8[2000 + 50] === 50 && recv(b) === 0, 'b takes them in order, its bytes in memory, then none');
ok(woke === 2, 'b woke for each');
ok(a.read32(0x58c) === 0x0a040002 && b.read32(0x584) === (0x52 | 0x54 << 8 | 0x00 << 16 | 0xc4 << 24) && b.read32(0x588) === 2 << 8, "addresses and MACs as c4pc's switch gives");
b.arena.u8.set(new TextEncoder().encode('vm7\0'), 300); b.write32(0x590, 300); b.write32(0x598, 3);
ok(b.read32(0x598) === 0x0a040008, 'vm7 resolves to 10.4.0.8');
for (let k = 0; k < 70; k++) send(a, frame(c.mac, 64));
await new Promise(r => setTimeout(r, 50));
ok(c.read32(0x59c) === 64 && c.drops === 7, 'c keeps 64, drops the rest (counted)');
for (const x of [a, b, c]) x.port.close();
