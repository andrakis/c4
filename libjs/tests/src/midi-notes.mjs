// midi-notes.mjs FILE MIN_NOTES MIN_CHANNELS - what a game played on the
// sound card's MIDI port, from `cli.js --midi FILE` (a line a message: its
// time in microseconds, then its bytes in hex), for the games' tests:
//
//   N notes on C channels, drums, in time, from S s to E s
//
// and status 0 when there are at least MIN_NOTES notes (note ons with a
// velocity) on at least MIN_CHANNELS channels, the percussion channel (10,
// 9 from 0) among them, and the times never go back.

import { readFileSync } from 'node:fs';

const [file, minNotes = '100', minChannels = '4'] = process.argv.slice(2);
let notes = 0, last = -1, inTime = true, first = null;
const chans = new Set();
for (const line of readFileSync(file, 'utf8').split('\n')) {
  if (!line) continue;
  const [t, ...hex] = line.split(' ');
  const time = Number(t), b = hex.map(h => parseInt(h, 16));
  if (time < last) inTime = false;
  last = time;
  if (first === null) first = time;
  if ((b[0] & 0xf0) === 0x90 && b[2] > 0) { notes++; chans.add(b[0] & 15); }
}
const drums = chans.has(9);
console.log(`${notes} notes on ${chans.size} channels, ${drums ? 'drums' : 'no drums'}, ${inTime ? 'in time' : 'out of time'}, ` +
  `from ${((first || 0) / 1e6).toFixed(1)} s to ${(Math.max(last, 0) / 1e6).toFixed(1)} s`);
process.exit(notes >= +minNotes && chans.size >= +minChannels && drums && inTime ? 0 : 1);
