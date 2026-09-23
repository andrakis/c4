// test-printf.mjs - printf.js against glibc.
//
// The expected column was produced by glibc's own printf in a 32-bit
// build (gcc -m32), which is what native c4m32 hands PRTF to. Strings
// live in a small fake memory; a %s argument is an address into it.

import { format } from '../printf.js';

const mem = new Uint8Array(256);
let top = 16;
const S = s => { const at = top; for (const c of s) mem[top++] = c.charCodeAt(0); mem[top++] = 0; return at; };
const str = p => { let e = p; while (mem[e]) e++; return mem.subarray(p, e); };

const cases = [
  ['%d|%i', [42, -7], '42|-7'],
  ['%5d|%-5d|%05d', [42, 42, 42], '   42|42   |00042'],
  ['%+d|% d|%+d', [5, 5, -5], '+5| 5|-5'],
  ['%x|%X|%#x|%#X|%#x', [255, 255, 255, 255, 0], 'ff|FF|0xff|0XFF|0'],
  ['%o|%#o|%#o', [8, 8, 0], '10|010|0'],
  ['%u', [-1], '4294967295'],
  ['%x', [-1], 'ffffffff'],
  ['%.3d|%.0d|%5.3d|%-6.2d|', [7, 0, 7, 3], '007||  007|03    |'],
  ['%c%c%c', [97, 98, 99], 'abc'],
  ['[%s]|[%10s]|[%-10s]|[%.2s]', [S('hi'), S('hi'), S('hi'), S('hello')], '[hi]|[        hi]|[hi        ]|[he]'],
  ['%*d|%-*d|%.*d', [6, 1, 6, 2, 4, 3], '     1|2     |0003'],
  ['%*d', [-6, 9], '9     '],
  ['%hd|%hhd|%hu|%hhx', [70000, 300, -1, 511], '4464|44|65535|ff'],
  ['%ld|%lx|%lu|%zd', [-3, 255, 3, 4], '-3|ff|3|4'],
  ['100%%|%d%%', [5], '100%|5%'],
  ['%05x|%-05d|', [26, 7], '0001a|7    |'],
  ['%d', [-2147483648], '-2147483648'],
  ['%08.3d|', [5], '     005|'],
  ['% 05d|%+05d', [3, 3], ' 0003|+0003'],
  ['%s', [S('')], ''],
  ['%q%d', [5], '%d'],
  ['%s', [0], '(null)'],
];

let fail = 0;
for (const [fmt, args, want] of cases) {
  const got = String.fromCharCode(...format(new TextEncoder().encode(fmt), args, str));
  if (got !== want) { fail = 1; console.log(`test-printf: ${JSON.stringify(fmt)} gave ${JSON.stringify(got)}, want ${JSON.stringify(want)}`); }
}
console.log(fail ? 'test-printf: FAILED' : `test-printf: ${cases.length} formats OK`);
process.exit(fail);
