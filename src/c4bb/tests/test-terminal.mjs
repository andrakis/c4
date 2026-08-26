// test-terminal.mjs - the c4bb web terminal's screen model.
//
// panels.js's Terminal gained cursor addressing so a full-screen
// program (src/tests/raycast.c) can repaint in place instead of
// scrolling a frame off the top. These pin BOTH halves: a shell that
// never positions the cursor must still behave like the teletype it
// always was, and a program that does must land on the same rows every
// frame. Run: node src/c4bb/tests/test-terminal.mjs
import { Terminal } from '../web/panels.js';
import { readFileSync } from 'fs';

let fails = 0;
const eq = (name, got, want) => {
  if (got === want) { console.log(`  ok   ${name}`); return; }
  console.log(`  FAIL ${name}\n       got  ${JSON.stringify(got)}\n       want ${JSON.stringify(want)}`);
  fails++;
};
// drive it the way the machine does: one byte at a time, then flush
const feed = (t, s) => { for (const c of s) t.write(c.charCodeAt(0)); t.flush(); };
const text = t => t.lines.map(r => r.map(c => c.ch).join('').replace(/\s+$/, '')).join('\n');

// ---- teletype behaviour is unchanged --------------------------------
{
  const t = new Terminal(null, 25, 80);
  feed(t, 'A> ls\nhello.c4r\nA> ');
  eq('teletype appends lines', text(t), 'A> ls\nhello.c4r\nA>');  // text() trims trailing space
  eq('teletype cursor row', t.cy, 2);
  t.write(8); t.flush();          // backspace mid-line
  eq('backspace erases', text(t), 'A> ls\nhello.c4r\nA>');
}

// ---- cursor addressing ----------------------------------------------
{
  const t = new Terminal(null, 4, 80);
  feed(t, 'one\ntwo\nthree\nfour');
  feed(t, '\x1b[HXXX');
  eq('ESC[H homes to the top of the screen window', text(t), 'XXX\ntwo\nthree\nfour');
  feed(t, '\x1b[3;2Hz');
  eq('ESC[r;cH addresses a row and column', text(t), 'XXX\ntwo\ntzree\nfour');
}

// ---- a repainting program lands on the same rows every frame --------
{
  const t = new Terminal(null, 3, 80);
  const frame = n => `\x1b[H${n}aaa\n${n}bbb\n${n}ccc\x1b[H`;
  feed(t, '\x1b[2J');
  feed(t, frame(1));
  const after1 = text(t);
  feed(t, '\n');                  // the newline puts() appends
  feed(t, frame(2));
  eq('frame 1', after1, '1aaa\n1bbb\n1ccc');
  eq('frame 2 overwrites in place, no growth', text(t), '2aaa\n2bbb\n2ccc');
  eq('line count stays at the screen height', t.lines.length, 3);
}

// ---- a full-width row must not consume two lines --------------------
// Filling the last column arms a wrap that only the NEXT printable
// character takes. Wrapping eagerly costs one extra line per row, so a
// full-screen program grows the buffer by a whole screen every frame
// instead of repainting -- which is exactly what it looked like.
{
  const t = new Terminal(null, 4, 8);
  feed(t, '12345678\nabcdefgh\n');
  eq('full-width rows do not double-advance', t.lines.length, 3);
  eq('full-width rows land where written', text(t), '12345678\nabcdefgh\n');  // the final \n opens row 3
  feed(t, 'X');
  eq('the pending wrap is taken by the next char', text(t), '12345678\nabcdefgh\nX');
}

// ---- erase, and colour still works ----------------------------------
{
  const t = new Terminal(null, 4, 80);
  feed(t, 'abcdef\x1b[1;4H\x1b[K');
  eq('ESC[K erases to end of line', text(t), 'abc');
  const t2 = new Terminal(null, 4, 80);
  feed(t2, '\x1b[48;5;196mR\x1b[0m.');
  eq('SGR still colours a cell', t2.toHtml(),
     '<span style="background:rgb(255,0,0)">R</span>.');
}

// ---- an escape split across writes must not be printed literally ----
{
  const t = new Terminal(null, 4, 80);
  feed(t, 'ab\x1b');              // flush lands mid-sequence
  feed(t, '[1;1HZ');
  eq('escape split across flushes', text(t), 'Zb');
}

// ---- ?25 hide/show is consumed, not drawn ---------------------------
{
  const t = new Terminal(null, 4, 80);
  feed(t, '\x1b[?25lhi\x1b[?25h');
  eq('cursor hide/show consumed', text(t), 'hi');
  eq('hide tracked', t.cursorHidden, false);
}

// ---- the keyboard contract -----------------------------------------
// A full-screen program gets its arrows and function keys through
// app.js's KEYSEQ table, and decodes them with src/c4tui/c4tui.c. Those
// are two files that must agree about the same escape sequences and
// have no other reason to stay in step, so this reads both and checks.
{
  const here = new URL('.', import.meta.url).pathname;
  const app = readFileSync(here + '../web/app.js', 'utf8');
  const tui = readFileSync(here + '../../c4tui/c4tui.c', 'utf8');

  const m = app.match(/const KEYSEQ = \{([\s\S]*?)\n\};/);
  eq('KEYSEQ table present', !!m, true);
  const seqs = [...(m ? m[1] : '').matchAll(/(\w+):\s*'([^']*)'/g)]
                 .map(x => [x[1], x[2].replace(/\\x1b/g, '\x1b')
                                      .replace(/\\t/g, '\t').replace(/\\r/g, '\r')
                                      .replace(/\\x7f/g, '\x7f')]);
  eq('every special key is covered', seqs.length >= 22, true);

  // Every CSI/SS3 sequence the browser sends must be one c4tui's
  // decoder names. A sequence it drops would reach the program as a
  // stray Escape and a letter, which is exactly the bug this replaced.
  let unknown = [];
  for (const [name, seq] of seqs) {
    if (seq[0] !== '\x1b') continue;
    if (seq[1] === 'O') {                       // ESC O P..S
      if (!tui.includes(`c == '${seq[2]}') return TUI_F`)) unknown.push(name);
    } else if (seq[1] === '[') {
      const body = seq.slice(2);
      if (/^[A-Z]$/.test(body)) {
        if (!tui.includes(`c == '${body}') return TUI_`)) unknown.push(name);
      } else {                                  // ESC [ n ~
        const n = parseInt(body, 10);
        if (!new RegExp(`n1 == ${n}\\)\\s*return TUI_`).test(tui)) unknown.push(name);
      }
    }
  }
  eq('c4tui decodes every sequence app.js sends', unknown.join(',') || 'none', 'none');

  // And the raw/cooked switch itself: app.js must consult rawKbd, and
  // cli.js must too, or a full-screen program is echoed over.
  eq('app.js honours rawKbd', /dev\.rawKbd\s*>\s*0/.test(app), true);
  const cli = readFileSync(here + '../sim/cli.js', 'utf8');
  eq('cli.js honours rawKbd', /dev\.rawKbd\s*>\s*0/.test(cli), true);
}

console.log(fails ? `test-terminal: ${fails} FAILED` : 'test-terminal: OK');
process.exit(fails ? 1 : 0);
