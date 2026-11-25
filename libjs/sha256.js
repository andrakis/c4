// sha256.js - SHA-256 of bytes, synchronously, the same in a worker and
// under node: unpack.js checks what it unpacks with it while the machine
// waits for the file (crypto.subtle's digest is a promise). About 200 MB
// a second.
const K = new Uint32Array([
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
  0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
  0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
  0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
  0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
  0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
  0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
  0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
]);

// the digest of BYTES (a Uint8Array), as 64 hex digits
export function sha256(bytes) {
  const h = new Uint32Array([0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19]);
  const w = new Uint32Array(64);
  const n = bytes.length;
  // the last block or two: what is left, 0x80, zeros, the length in bits
  const tailAt = n - (n % 64);
  const tail = new Uint8Array(n % 64 < 56 ? 64 : 128);
  tail.set(bytes.subarray(tailAt));
  tail[n % 64] = 0x80;
  const bitsHi = Math.floor(n / 0x20000000), bitsLo = (n * 8) >>> 0;
  const e = tail.length;
  tail[e - 8] = bitsHi >>> 24; tail[e - 7] = bitsHi >>> 16; tail[e - 6] = bitsHi >>> 8; tail[e - 5] = bitsHi;
  tail[e - 4] = bitsLo >>> 24; tail[e - 3] = bitsLo >>> 16; tail[e - 2] = bitsLo >>> 8; tail[e - 1] = bitsLo;
  const block = (b, at) => {
    for (let i = 0; i < 16; i++, at += 4) w[i] = (b[at] << 24) | (b[at + 1] << 16) | (b[at + 2] << 8) | b[at + 3];
    for (let i = 16; i < 64; i++) {
      const x = w[i - 15], y = w[i - 2];
      const s0 = ((x >>> 7) | (x << 25)) ^ ((x >>> 18) | (x << 14)) ^ (x >>> 3);
      const s1 = ((y >>> 17) | (y << 15)) ^ ((y >>> 19) | (y << 13)) ^ (y >>> 10);
      w[i] = (w[i - 16] + s0 + w[i - 7] + s1) | 0;
    }
    let a = h[0], b2 = h[1], c = h[2], d = h[3], e2 = h[4], f = h[5], g = h[6], hh = h[7];
    for (let i = 0; i < 64; i++) {
      const S1 = ((e2 >>> 6) | (e2 << 26)) ^ ((e2 >>> 11) | (e2 << 21)) ^ ((e2 >>> 25) | (e2 << 7));
      const t1 = (hh + S1 + ((e2 & f) ^ (~e2 & g)) + K[i] + w[i]) | 0;
      const S0 = ((a >>> 2) | (a << 30)) ^ ((a >>> 13) | (a << 19)) ^ ((a >>> 22) | (a << 10));
      const t2 = (S0 + ((a & b2) ^ (a & c) ^ (b2 & c))) | 0;
      hh = g; g = f; f = e2; e2 = (d + t1) | 0; d = c; c = b2; b2 = a; a = (t1 + t2) | 0;
    }
    h[0] += a; h[1] += b2; h[2] += c; h[3] += d; h[4] += e2; h[5] += f; h[6] += g; h[7] += hh;
  };
  for (let at = 0; at < tailAt; at += 64) block(bytes, at);
  for (let at = 0; at < tail.length; at += 64) block(tail, at);
  let hex = '';
  for (const v of h) hex += v.toString(16).padStart(8, '0');
  return hex;
}
