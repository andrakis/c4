# c4tui — a character-cell UI library for C4DOS

`src/c4tui/c4tui.c`, about 700 lines, included as a source file. It
exists so that a full-screen program on this family of machines can be
written without first writing a terminal layer, a keyboard decoder and a
string library — because C4DOS supplies none of the three.

It is a **library, not a framework**. It owns a grid of cells and the
bytes that reach the terminal. It does not own your main loop, your
data, or your idea of what a window is.

## What you are building on, and what you are not

C4DOS gives a program the nine stock-c4 builtins and nothing else:
`open read close printf malloc free memset memcmp exit`. There is no
`strlen`, no `memcpy`, no `realloc`, no termios, no `ioctl`, no way to
ask how big the terminal is, and the C4DOS API table (`include/c4dos.h`)
is fifteen slots of filesystem and memory with **no console call at
all**. `puts` exists on c4m, c4mp and c4bb but not on plain c4.

So the library supplies: the grid, the flush, the key decoder, and the
four string helpers it needs. It does not supply a text buffer, an undo
stack, or a syntax highlighter — those are the editor's.

## The two decisions everything follows from

**One write per frame.** raycast established this: on c4bb `PUTS` is one
microcoded instruction for a whole frame, where `printf` vectors into
the firmware formatter at 15-20 instructions per character — about
40,000 cycles for the same screen. Everything is assembled into one
buffer and emitted once. `-D TUI_PLAIN_C4` swaps `puts` for
`printf("%s\n", …)` for a machine that has no `PUTS`.

**Only the damage.** raycast repaints all two thousand cells every frame
because in a raycaster every cell really does change. An editor is the
opposite: a keystroke changes one line, often one cell. `tui_flush`
walks the grid against what the terminal is already showing and emits
the runs that differ, positioning the cursor once per run.

Measured, from the demo, on c4m and on c4bb alike:

    first frame (whole screen)      2521 bytes
    a dialog opens                   417
    the dialog closes                311
    an incidental change              17

A repaint-everything design would have paid 2521 for each of those. This
is the difference between an editor that feels like an editor on a
breadboard and one that does not.

## The cursor is drawn, not placed

The hardware cursor is hidden and `tui_cursor(x, y)` marks a cell, which
`tui_flush` renders by swapping that cell's foreground and background.
Two reasons: `PUTS` appends a newline that would move a *placed* cursor
off the cell it was just put on, and a block cursor is what a DOS editor
looks like anyway. `tui_hwcursor(1)` turns the real one back on if you
would rather have it.

Because the swap happens at flush time rather than in the grid, moving
the cursor damages exactly two cells and the caller never has to undo
it.

## The API

    tui_init(w, h)              80x25 is what C4DOS assumes
    tui_end()
    tui_flush()                 the only thing that writes to the terminal
    tui_damage()                forget the screen; next flush repaints all

    tui_attr(fg, bg)            fg | bg<<4, the DOS layout
    tui_cls(a)  tui_fill(x,y,w,h,ch,a)  tui_put(x,y,ch,a)
    tui_text(x,y,s,a)           returns the column past the end
    tui_textf(x,y,s,a,w)        padded or truncated to exactly w
    tui_num(x,y,n,a)
    tui_box / tui_window(x,y,w,h,title,a) / tui_shadow
    tui_cursor(x,y) / tui_nocursor() / tui_hwcursor(on)
    tui_save(x,y,w,h) -> blk    tui_restore(blk)

    tui_key()                   waits; returns a key code
    tui_key_nb()                TUI_NONE if nothing is waiting

    tui_menubar(y, names, n, sel, a, asel)
    tui_menux(names, n, which)  where the nth name starts
    tui_popup(x, y, items, n, start, a, asel)   -> index, or -1
    tui_dialog(title, msg, buttons, nb, a, asel) -> index, or -1
    tui_input(x, y, w, buf, max, a)             -> 1 = Enter, 0 = Esc
    tui_list(x, y, w, h, title, items, n, start, a, asel) -> index, or -1

The modal four run their own key loop, save what they cover and put it
back, so nothing has to be redrawn after one returns. An item beginning
`-` in a popup is a separator.

Colours are the sixteen DOS ones (`TUI_BLUE`, `TUI_LGREY`, `TUI_YELLOW`,
…). The DOS palette and the ANSI one order their colours differently;
the library holds the permutation so you can think in the DOS names.

## Keys

Printable keys are themselves and control keys are 1..31 as they arrive.
Everything else is above 255: `TUI_UP`, `TUI_HOME`, `TUI_PGDN`,
`TUI_F1`..`TUI_F12`, `TUI_INS`, `TUI_DEL`, and **`TUI_ALT + 'f'`** for
Alt-F, which is how a DOS menu bar is opened and the reason the decoder
bothers with it.

Input never touches fd 0 — a blocking read there stops the whole VM.
The rule in this family is a second descriptor opened `O_NONBLOCK`, and
`/dev/tty` rather than `/dev/stdin` because only the former is raw;
c4bb's device model draws exactly that distinction.

**Esc is ambiguous and always will be.** An Esc key and the start of an
arrow sequence are the same byte; the only way to tell them apart is to
wait and give up. `TUI_ESCWAIT` is that number of polls.

## Building

    ./c4sp src/c4sp/lisp/c4lc.lisp -O -conforming -P -I src/c4tui \
           yourprog.c yourprog.c4r          # yourprog.c does #include "c4tui.c"

`-conforming` is needed because the library writes `\033`. c4fc compiles
it identically (`-mfuse`/`-mcisc` too). With c4cc, which has no
preprocessor, drop the `#include` and pass both files.

The dialect is strict c4 — no structs, no `unsigned`, literals in enums
— so it builds with c4cc as well as with c4lc and c4fc.

## Known limits, honestly

- **Boxes are ASCII** (`+`, `-`, `|`). c4bb's web terminal stores one
  byte per cell, so a UTF-8 line-drawing character would arrive as three
  cells of noise. In a native UTF-8 terminal you could do better, and
  the library would need a byte-vs-cell distinction it does not have.
- **The screen size is declared, not discovered.** There is no
  `TIOCGWINSZ` anywhere in this family. raycast has the same problem and
  the same answer: a flag.
- **In a browser, keys do not arrive yet.** `src/c4bb/web/app.js:227`
  forwards only single-character `e.key` values, so arrows, function
  keys, Home/End and Tab are dropped, and it echoes unconditionally even
  though `dev.rawKbd` is maintained for exactly this. Under
  `cli.js -i` on a real tty everything works. That is c4bb's to fix and
  it is small: encode the special keys, and honour `rawKbd`.
- `tui_input` is a single line. A multi-line editor is the editor's job
  — the library gives it the grid, the cursor and the keys.
