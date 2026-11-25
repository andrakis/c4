// test-uart-block.mjs -- the UART's block write (docs/c4bb-uart-block.md).
//
// The rule worth pinning is that nothing downstream can tell a block
// write from N stores to UART_TX: same sink, same order, same activity
// counter. If that ever stops being true, the CLI's line buffering and
// the web terminal's parser start disagreeing about what the machine
// said, and the firmware formatter is the thing that would be blamed.
//
// Run: node src/c4bb/tests/test-uart-block.mjs
import { Arena } from '../sim/arena.js';
import { Devices, UART_TX, UART_TXADDR, UART_TXLEN } from '../sim/devices.js';

let fails = 0;
const eq = (name, got, want) => {
  if (JSON.stringify(got) === JSON.stringify(want)) { console.log(`  ok   ${name}`); return; }
  console.log(`  FAIL ${name}\n       got  ${JSON.stringify(got)}\n       want ${JSON.stringify(want)}`);
  fails++;
};

// A machine just big enough to have an end worth falling off.
const make = () => {
  const arena = new Arena(64 * 1024);
  const out = [];
  const dev = new Devices(arena, { onByte: b => out.push(b) });
  return { arena, dev, out, text: () => String.fromCharCode(...out) };
};
const put = (arena, addr, s) => {
  for (let i = 0; i < s.length; i++) arena.write8(addr + i, s.charCodeAt(i));
};

// ---- the ordinary case ----------------------------------------------
{
  const { arena, dev, out, text } = make();
  put(arena, 0x2000, 'hello, world');
  dev.write32(UART_TXADDR, 0x2000);
  dev.write32(UART_TXLEN, 5);
  eq('emits exactly N bytes', text(), 'hello');
  eq('reports what it took', dev.read32(UART_TXLEN), 5);
  eq('counts every byte for the lamp', dev.uartActivity, 5);
  eq('the address latch holds', dev.read32(UART_TXADDR), 0x2000);

  // The latch does not advance, matching DISK_WADDR: a second write of
  // the same length says the same thing again.
  dev.write32(UART_TXLEN, 5);
  eq('TXADDR does not auto-advance', text(), 'hellohello');
  eq('activity keeps counting', dev.uartActivity, 10);
  out.length = 0;
}

// ---- indistinguishable from N stores to UART_TX ----------------------
{
  const a = make();
  put(a.arena, 0x1000, 'c4bb\n\x1b[0m');
  a.dev.write32(UART_TXADDR, 0x1000);
  a.dev.write32(UART_TXLEN, 9);

  const b = make();
  for (const ch of 'c4bb\n\x1b[0m') b.dev.write32(UART_TX, ch.charCodeAt(0));

  eq('same bytes as byte-at-a-time', a.out, b.out);
  eq('same activity count', a.dev.uartActivity, b.dev.uartActivity);
}

// ---- the degenerate lengths -----------------------------------------
{
  const { arena, dev, out, text } = make();
  put(arena, 0x3000, 'xyz');
  dev.write32(UART_TXADDR, 0x3000);

  dev.write32(UART_TXLEN, 0);
  eq('zero length emits nothing', text(), '');
  eq('zero length reports 0', dev.read32(UART_TXLEN), 0);

  dev.write32(UART_TXLEN, -4);
  eq('negative length emits nothing', text(), '');
  eq('negative length reports 0', dev.read32(UART_TXLEN), 0);
  eq('nothing reached the lamp either', dev.uartActivity, 0);
  out.length = 0;
}

// ---- falling off the end of the arena --------------------------------
// Clamped, not wrapped and not faulted: read32 already answers 0 past
// the end rather than trapping, and the returned count is how a caller
// finds out it was short.
{
  const { arena, dev, text } = make();
  const near = arena.size - 3;
  put(arena, near, 'abc');
  dev.write32(UART_TXADDR, near);
  dev.write32(UART_TXLEN, 100);
  eq('clamps at the end of the arena', text(), 'abc');
  eq('reports the short count', dev.read32(UART_TXLEN), 3);
}
{
  const { dev, text } = make();
  dev.write32(UART_TXADDR, 0x7fffffff);
  dev.write32(UART_TXLEN, 8);
  eq('an address past the end emits nothing', text(), '');
  eq('and reports 0', dev.read32(UART_TXLEN), 0);
}

console.log(fails ? `\n${fails} failure(s)` : '\nall ok');
process.exit(fails ? 1 : 0);
