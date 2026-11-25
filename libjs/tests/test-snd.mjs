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
import { SoundMixer } from '../snd-mix.js';

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

// The OPL's mode is the card's. Doom turns the OPL3's on (0x105 = 1) and,
// before, never off: then an OPL2 program's voice -- 0xC0 without the
// left/right bits, as ROTT's and Duke3D's al_midi.c write it -- is
// silent, and the music of every such game after it was, until a reset.
// The mixer's oplState() says so: writes, but a level of 0, in OPL3 mode.
const voice = (c0, pre = []) => {
  const mix = new SoundMixer(44100);
  const ev = pre.map(([r, v]) => [EVT.OPL, 0, r, v]);
  for (const [r, v] of [[0x20, 0x01], [0x23, 0x01], [0x40, 0x10], [0x43, 0x00], [0x60, 0xf4], [0x63, 0xf4],
    [0x80, 0x55], [0x83, 0x56], [0xc0, c0], [0xa0, 0x6b], [0xb0, 0x31]]) ev.push([EVT.OPL, 1000, r, v]);
  mix.push(ev);
  const L = new Float32Array(4410), R = new Float32Array(4410);
  mix.render(L, R, 4410);
  return mix.oplState();
};
let st = voice(0x08);
check(st.writes === 11 && st.rms > 500 && !st.opl3, 'an OPL2 voice plays, and oplState() says how loud', JSON.stringify(st));
st = voice(0x08, [[0x105, 1]]);
check(st.writes === 12 && st.rms === 0 && st.peak === 0 && st.opl3 && st.c0[0] === 0x08,
  'the same voice on a card left in its OPL3 mode is silent, and oplState() shows why (opl3, c0 without 0x30)', JSON.stringify(st));
st = voice(0x08, [[0x105, 1], [0x104, 0], [0x105, 0]]);
check(st.rms > 500 && !st.opl3, '... and plays once the program turns the mode off first (ROTT, Duke3D)', JSON.stringify(st));
st = voice(0x38, [[0x105, 1]]);
check(st.rms > 500 && st.opl3, '... or sets both sides in 0xC0, which an OPL3 reads and an OPL2 ignores (grin\'s chime)', JSON.stringify(st));
check(voice(0x38).rms > 500, '... and an OPL2 plays that voice too');
st = new SoundMixer(44100); st.push([[EVT.OPL, 0, 0x105, 1], [EVT.RESET, 10]]); st.render(new Float32Array(64), new Float32Array(64), 64);
check(!st.oplState().opl3, 'RESET (snd_reset, a machine reset) makes the OPL an OPL2 again');

console.log(`test-snd: ${fail ? 'FAILED' : 'OK'}`);
process.exit(fail);
