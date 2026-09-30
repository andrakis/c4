# c4tui — a character-cell UI library for C4DOS

c4tui is about 700 lines of strict-c4 C that give a full-screen program a
grid of cells (character plus attribute), a damage-tracked flush, a
keyboard decoder, and DOS-style widgets: boxes, windows with shadows, a
menu bar, pop-up menus, dialogs, a one-line input field and a list. It was
written for C4DOS, which offers a program the nine stock c4 builtins and
no console calls, so the library supplies what it needs itself.

It is a library, not a framework: it owns the screen and the bytes sent to
the terminal, not the program's main loop. It is used by including it as a
source file; there is no separate object.

Two decisions shape it. Each flush emits only the cells that changed since
the last one, and emits them in one write (`PUTS` on c4m, c4mp and c4bb,
where a whole frame is one instruction). The cursor is drawn into a cell
rather than placed, since `PUTS` appends a newline.

## Build

| target | produces |
|---|---|
| `make c4tui-demo.c4r` | the demo, 64-bit: `c4lc -O -conforming -P -I src/c4tui src/c4tui/demo.c` |
| `make c4tui-demo32.c4r` | the same built by `c4sp32`, for c4bb |

To use it in your own program:

    # c4lc (has a preprocessor): put #include "c4tui.c" in yourprog.c
    ./c4sp src/c4sp/lisp/c4lc.lisp -O -conforming -P -I src/c4tui yourprog.c yourprog.c4r
    # c4cc (no preprocessor): pass the library first
    ./c4cc -o yourprog.c4r src/c4tui/c4tui.c yourprog.c

`-conforming` is needed because the library writes `"\033"`. Defining
`TUI_PLAIN_C4` (through a preprocessor, e.g. c4lc's `-D`) makes the flush
use `printf` instead of `puts`, which plain c4 does not have. c4cc skips
`#` lines, so a c4cc build always takes the `puts` branch.

## Run

    ./c4m load-c4r.c -- c4tui-demo.c4r
    node src/c4bb/sim/cli.js -i -d <disk> c4tui-demo32.c4r

Alt-F / Alt-E / Alt-H open the menus, F2 a dialog, F3 a list, F4 an input
field, Esc quits.

## Test

`make test-c4tui` (not in `make test`) feeds a fixed key sequence to the
demo on native c4m and on c4bb, compares both outputs byte for byte with
`tests/expected/demo.txt`, and checks that the second frame is short,
which fails if the flush stops being incremental.

## Layout

| file | what |
|---|---|
| `c4tui.c` | the library: grid, attributes (DOS layout, fg low nibble / bg high), flush, key decoder, widgets |
| `demo.c` | every widget, driven from a menu bar; also the manual test |
| `tests/expected/demo.txt` | the golden output for `test-c4tui` |

## Hosts

- **c4m, c4mp, c4bb**: `puts` exists; the default build.
- **Plain c4**: needs a build with `TUI_PLAIN_C4` defined; no make target
  builds one.
- **Keys**: input comes from a second descriptor opened non-blocking on
  `/dev/tty`, never a blocking read of fd 0. On c4bb, a program that opens
  `/dev/tty` gets raw keys (arrows, function keys, Alt-letters) from both
  `cli.js` and the web terminal.

## Limits and known issues

- Boxes are ASCII (`+ - |`); c4bb's terminal stores one byte per cell.
- The screen size is declared (`tui_init(w, h)`, 80x25 by default in the
  demo), not discovered.
- Esc and the start of an arrow-key sequence are the same byte; the
  decoder waits `TUI_ESCWAIT` polls to tell them apart.
- `tui_input` is a single line.

## Docs

[docs/c4tui-design.md](../../docs/c4tui-design.md) has the API and the
design. It has drifted in one place: its limit "in a browser, keys do not
arrive yet" was fixed by commit d9f919e, which made c4bb's web terminal
send raw key sequences when a program has `/dev/tty` open.
