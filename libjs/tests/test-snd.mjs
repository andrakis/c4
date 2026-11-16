// test-snd.mjs - the sound card's samples, one write each (snd-device.js
// PCM_START, PCM_CHVOL), on a card with nothing but a memory behind it.
//
// PCM_ADDR, LEN, RATE and VOL are the card's, not a program's: two
// programs that play at once can each set them before the other's
// PCM_PLAY, which then plays the other's sample (three windowed Quakes at
// 32 MB under C4IX: one killed for "a device's read of" another's
// memory). PCM_START takes the address of a block of five words and is
// one write, so whatever was written between, its sample is its own.
//
//   node libjs/tests/test-snd.mjs

import { SoundDevice, SND, EVT, HAS_START } from '../snd-device.js';

let fail = 0;
const check = (cond, what, extra = '') => {
  console.log(`test-snd: ${what} ${cond ? 'OK' : 'FAILED'}${!cond && extra ? '\n  ' + extra : ''}`);
  if (!cond) fail = 1;
};

// 64 KB of memory: dmaRead as the arena has it, the bytes at [addr, addr + len)
const mem = new Uint8Array(65536);
const arena = { dmaRead: (a, n) => mem.slice(a, a + n) };
let t = 0;
const snd = new SoundDevice({ now: () => t });
snd.attach({ arena });
const w32 = (a, v) => { mem[a] = v & 255; mem[a + 1] = (v >>> 8) & 255; mem[a + 2] = (v >>> 16) & 255; mem[a + 3] = (v >>> 24) & 255; };
const block = (at, ch, data, len, rate, vol) => { [ch, data, len, rate, vol].forEach((v, k) => w32(at + k * 4, v)); return at; };

// two programs' samples: A's 100 bytes of 0xAA at 0x1000, B's 50 of 0xBB at 0x2000
mem.fill(0xaa, 0x1000, 0x1000 + 100);
mem.fill(0xbb, 0x2000, 0x2000 + 50);

check((snd.read32(SND.CAPS) & HAS_START) !== 0, 'CAPS bit 6 says PCM_START and PCM_CHVOL are there');

// The latches, interleaved: A sets its sample, B sets its own, then A plays
snd.write32(SND.PCM_ADDR, 0x1000); snd.write32(SND.PCM_LEN, 100); snd.write32(SND.PCM_RATE, 11025); snd.write32(SND.PCM_VOL, 0xffff);
snd.write32(SND.PCM_ADDR, 0x2000); snd.write32(SND.PCM_LEN, 50); snd.write32(SND.PCM_RATE, 22050); snd.write32(SND.PCM_VOL, 0x8080);
snd.write32(SND.PCM_PLAY, 3);
let ev = (snd.drain() || []).filter(e => e[0] === EVT.PCM);
check(ev.length === 1 && ev[0][5].length === 50 && ev[0][5][0] === 0xbb,
  "the latches, written by two players in turn: A's PLAY plays B's sample (what PCM_START is for)", JSON.stringify(ev.map(e => [e[2], e[3], e[5].length])));

// PCM_START: A's block, with B's latches written in the middle of A's play
const ba = block(0x3000, 3, 0x1000, 100, 11025, 0xffff);
const bb = block(0x3100, 5, 0x2000, 50, 22050, 0x8080);
snd.write32(SND.PCM_ADDR, 0x2000); snd.write32(SND.PCM_LEN, 50);
snd.write32(SND.PCM_START, ba);
snd.write32(SND.PCM_START, bb);
ev = (snd.drain() || []).filter(e => e[0] === EVT.PCM);
check(ev.length === 2, 'two PCM_STARTs, two samples', String(ev.length));
check(ev[0] && ev[0][2] === 3 && ev[0][3] === 11025 && ev[0][4] === 0xffff && ev[0][5].length === 100 && ev[0][5].every(b => b === 0xaa),
  "PCM_START plays its block's own sample on its channel, at its rate and volume, whatever the latches say",
  ev[0] && JSON.stringify([ev[0][2], ev[0][3], ev[0][4], ev[0][5].length, ev[0][5][0]]));
check(ev[1] && ev[1][2] === 5 && ev[1][3] === 22050 && ev[1][4] === 0x8080 && ev[1][5].length === 50 && ev[1][5][0] === 0xbb,
  "... and the other block's, its own", ev[1] && JSON.stringify([ev[1][2], ev[1][3], ev[1][4], ev[1][5].length]));
check(snd.read32(SND.PCM_ADDR) === 0x2000 && snd.read32(SND.PCM_LEN) === 50, 'PCM_START leaves the latches as they were');
check((snd.read32(SND.PCM_BUSY) & ((1 << 3) | (1 << 5))) === ((1 << 3) | (1 << 5)), 'both channels are busy');

// a block that says nothing to play plays nothing
snd.write32(SND.PCM_START, block(0x3200, 1, 0x1000, 0, 11025, 0xffff));
check((snd.drain() || []).filter(e => e[0] === EVT.PCM).length === 0, 'a block of length 0 plays nothing');

// PCM_CHVOL: one channel's volume in one write
snd.write32(SND.PCM_VOL, 0x1111);
snd.write32(SND.PCM_CHVOL, 5 | (0x4020 << 8));
ev = (snd.drain() || []).filter(e => e[0] === EVT.VOL);
check(ev.length === 1 && ev[0][2] === 5 && ev[0][3] === 0x4020, 'PCM_CHVOL sets the volume it carries, not PCM_VOL\'s', JSON.stringify(ev));
check(snd.read32(SND.PCM_VOL) === 0x1111, '... and leaves PCM_VOL as it was');

console.log(`test-snd: ${fail ? 'FAILED' : 'OK'}`);
process.exit(fail);
