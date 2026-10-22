// wav-levels.mjs FILE T... -- how loud a 16-bit stereo WAV is in the 50 ms
// from each time T (seconds): loud, some or quiet, one word for each
import { readFileSync } from 'node:fs';
const [file, ...times] = process.argv.slice(2);
const b = readFileSync(file);
const rate = b.readUInt32LE(24), data = 44, frames = (b.length - data) / 4;
const out = [];
for (const t of times.map(Number)) {
  const f0 = Math.floor(t * rate), n = Math.floor(0.05 * rate);
  let sum = 0, k = 0;
  for (let f = f0; f < f0 + n && f < frames; f++, k++) {
    const l = b.readInt16LE(data + f * 4) / 32768, r = b.readInt16LE(data + f * 4 + 2) / 32768;
    sum += l * l + r * r;
  }
  const rms = k ? Math.sqrt(sum / (2 * k)) : 0;
  out.push(process.env.WAV_RMS ? rms.toFixed(5) : rms > 0.01 ? 'loud' : rms < 0.001 ? 'quiet' : 'some');
}
console.log(out.join(' '));
