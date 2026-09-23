// keys.js - what a keypress means to the machine.
//
// Copied from src/c4bb/web/app.js:560-617 (the terminal's keydown
// handler), which can't be imported: there it is wired to the DOM and to
// a machine on the same thread. Here the DOM is on the page and the
// machine is in a Worker, so the page sends the fields of the KeyboardEvent
// and this decides, next to the keyboard FIFO, what they turn into.
//
// Two modes, as on c4bb:
//   COOKED  (no program has /dev/tty open): the host is the line
//           discipline. It echoes, and Backspace erases a character that
//           has not been committed by Enter yet. Neither kernel's shell
//           echoes for itself.
//   RAW     (dev.rawKbd > 0): a full-screen program owns the screen. No
//           echo, and the keys a program cannot otherwise get (arrows,
//           function keys, Alt-letter, Ctrl-letter) go in as the escape
//           sequences a terminal sends.
//
// Returns the bytes to echo to the terminal (possibly none).

export const KEYSEQ = {
  ArrowUp: '\x1b[A', ArrowDown: '\x1b[B', ArrowRight: '\x1b[C', ArrowLeft: '\x1b[D',
  Home: '\x1b[H', End: '\x1b[F', PageUp: '\x1b[5~', PageDown: '\x1b[6~',
  Insert: '\x1b[2~', Delete: '\x1b[3~',
  F1: '\x1bOP', F2: '\x1bOQ', F3: '\x1bOR', F4: '\x1bOS',
  F5: '\x1b[15~', F6: '\x1b[17~', F7: '\x1b[18~', F8: '\x1b[19~',
  F9: '\x1b[20~', F10: '\x1b[21~', F11: '\x1b[23~', F12: '\x1b[24~',
  Tab: '\t', Escape: '\x1b', Enter: '\r', Backspace: '\x7f',
};

const push = (dev, s) => { for (let i = 0; i < s.length; i++) dev.rxFifo.push(s.charCodeAt(i) & 0xff); };

// k: { key, ctrlKey, altKey, metaKey } from a keydown.
// Returns { echo: number[], handled: bool }.
export function keyDown(machine, k) {
  const dev = machine.dev;
  const echo = [];
  if (k.ctrlKey && (k.key === 'c' || k.key === 'C')) {
    machine.pendingSignal = 2;                           // SIGINT
  } else if (k.ctrlKey && (k.key === 'd' || k.key === 'D')) {
    dev.rxEof = true;
  } else if (dev.rawKbd > 0) {
    if (k.altKey && k.key.length === 1) push(dev, '\x1b' + k.key);
    else if (KEYSEQ[k.key] !== undefined) push(dev, KEYSEQ[k.key]);
    else if (k.ctrlKey && k.key.length === 1) {
      const c = k.key.toLowerCase().charCodeAt(0);
      if (c >= 97 && c <= 122) dev.rxFifo.push(c - 96);   // Ctrl-A is 1
      else return { echo, handled: false };
    } else if (k.key.length === 1 && !k.metaKey) push(dev, k.key);
    else return { echo, handled: false };
  } else if (k.key === 'Enter') {
    dev.rxFifo.push(10);
    echo.push(10);
  } else if (k.key === 'Backspace') {
    const n = dev.rxFifo.length;
    if (n && dev.rxFifo[n - 1] !== 10) { dev.rxFifo.pop(); echo.push(8, 32, 8); }
  } else if (k.key.length === 1 && !k.ctrlKey && !k.metaKey) {
    const c = k.key.charCodeAt(0);
    if (c > 255) return { echo, handled: false };       // not a byte this terminal can carry
    dev.rxFifo.push(c);
    echo.push(c);
  } else return { echo, handled: false };
  return { echo, handled: true };
}

// Programmatic input (vm.write): bytes as if typed, echoed in cooked
// mode the way a terminal echoes a paste.
export function typeBytes(machine, bytes) {
  const dev = machine.dev, echo = [];
  for (const b of bytes) {
    const c = b === 13 ? 10 : b;
    dev.rxFifo.push(c);
    if (dev.rawKbd === 0) echo.push(c);
  }
  return echo;
}
