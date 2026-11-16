// heapdump.mjs -- heapDump(vm): where libjs's heap (heap.js) has its memory:
// its bounds, the free blocks and ranges, a map of each 256 KB, and the
// biggest live blocks. try-inner (HEAP=1), try-heapwho and try50 print it.
export function heapDump(vm) {
  const h = vm.heap;
  const lines = [];
  const K = n => Math.round(n / 1024) + 'K';
  if (h.sizes) {
    let smallFree = 0, smallN = 0;
    for (const [c, s] of h.small) { smallFree += c * s.length; smallN += s.length; }
    let lf = 0, lmax = 0;
    for (const r of h.large) { lf += r.len; if (r.len > lmax) lmax = r.len; }
    lines.push(`heap ${K(h.base)}..${K(h.end)} brk ${K(h.brk)} top ${K(h.top)} gap ${K(h.top - h.brk)} used ${K(h.used)}`);
    lines.push(`small free ${K(smallFree)} in ${smallN} blocks; ranges ${h.large.length} total ${K(lf)} largest ${K(lmax)}`);
    // a map: each 256K, what is live (L large, s small), free in ranges (.), small-free (f), bump gap ( )
    const B = 262144, nb = Math.ceil((h.end - h.base) / B);
    const live = new Float64Array(nb), sf = new Float64Array(nb), rf = new Float64Array(nb);
    const add = (arr, a, n) => { while (n > 0) { const b = Math.floor((a - h.base) / B), e = h.base + (b + 1) * B, t = Math.min(n, e - a); if (b >= 0 && b < nb) arr[b] += t; a += t; n -= t; } };
    for (const [a, s] of h.spans) add(live, a, s);
    for (const [c, s] of h.small) for (const a of s) add(sf, a, c);
    for (const r of h.large) add(rf, r.addr, r.len);
    let row = '';
    for (let b = 0; b < nb; b++) {
      const u = live[b] / B, f = sf[b] / B, r = rf[b] / B;
      row += u > 0.9 ? '#' : u > 0.5 ? '+' : u > 0.1 ? '-' : f > 0.3 ? 'f' : r > 0.5 ? '.' : u > 0 ? ',' : ' ';
      if (b % 64 == 63) { lines.push(row); row = ''; }
    }
    if (row) lines.push(row);
    // the largest live blocks, by address
    const big = [...h.spans].filter(([a, s]) => s >= 65536).sort((x, y) => x[0] - y[0]);
    lines.push('live blocks >= 64K: ' + big.map(([a, s]) => `${K(a - h.base)}:${K(s)}`).join(' '));
    // small live, by 1M bucket
    const sl = new Map();
    for (const [a, s] of h.spans) if (s <= 4096) { const b = Math.floor((a - h.base) / 1048576); sl.set(b, (sl.get(b) || 0) + s); }
    lines.push('small live by MB: ' + [...sl].sort((x, y) => x[0] - y[0]).map(([b, n]) => `${b}:${K(n)}`).join(' '));
  } else lines.push('shared heap: largest ' + K(h.largest()));
  return lines.join('\n');
}
