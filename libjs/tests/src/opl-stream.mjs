// opl-stream.mjs RATE SAMPLES SEED OPL3 -- the register stream oplref.cpp
// renders, rendered by opl.js: the same samples to stdout, or it is wrong
import { OplChip } from "../../opl.js";
const [rate, total, seed, opl3] = process.argv.slice(2).map(Number);
let rs = seed >>> 0;
const rnd = () => { rs = (rs ^ (rs << 13)) >>> 0; rs = (rs ^ (rs >>> 17)) >>> 0; rs = (rs ^ (rs << 5)) >>> 0; return rs; };
const c = new OplChip(rate);
if (opl3) c.write(0x105, 1);
c.write(1, 0x20);
const buf = new Int32Array(1024), out = [];
let done = 0;
while (done < total) {
  const n = rnd() % 12;
  for (let i = 0; i < n; i++) {
    const r = rnd();
    let reg, val = (r >>> 8) & 0xff;
    switch (r % 8) {
      case 0: reg = 0x20 + rnd() % 0x16; break;
      case 1: reg = 0x40 + rnd() % 0x16; val &= 0x3f; break;
      case 2: reg = 0x60 + rnd() % 0x16; break;
      case 3: reg = 0x80 + rnd() % 0x16; break;
      case 4: reg = 0xa0 + rnd() % 9; break;
      case 5: reg = 0xb0 + rnd() % 9; break;
      case 6: reg = 0xc0 + rnd() % 9; break;
      default: reg = (rnd() & 1) ? 0xbd : 0xe0 + rnd() % 0x16; break;
    }
    if (opl3 && (rnd() & 1)) reg |= 0x100;
    c.write(reg, val);
  }
  let len = 1 + rnd() % 300;
  if (len > total - done) len = total - done;
  c.generate(len, buf);
  out.push(Buffer.from(buf.buffer.slice(0, (opl3 ? len * 2 : len) * 4)));
  done += len;
}
process.stdout.write(Buffer.concat(out));
