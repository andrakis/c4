# c4bb/web — the board in a browser

A static page, no build step. It runs the modules in [../sim/](../sim/README.md)
on the main thread and draws the board from [../hw/board.hwd](../hw/board.hwd).

    make c4bb-web      # everything the page loads, including the climb disk
    make serve-c4bb    # python3 -m http.server 8471 from the repo root

Open http://localhost:8471/src/c4bb/web/index.html. `app.js` fetches
`../hw/`, `../fw/` and `../images/` (including each disk's `manifest.json`)
by relative path, so the server's root has to be at or above `src/c4bb`; a
server rooted in `web/` comes up with an empty drives panel. `serve-c4bb`
serves the repo root.

| File | What it is |
|---|---|
| `index.html` | The layout: toolbar, board canvas, terminal, and the drives, registers and microcode panels. |
| `style.css` | Styles. |
| `app.js` | Wires the simulator to the page. Program and firmware pickers, and the four execution modes: µStep (one microstep), Step (one instruction), Run (animated), Turbo (the compiled engine, board shown as "heat"). |
| `board.js` | Canvas renderer. Lights chips, bus segments and control stubs from the current microstep's signals; in Turbo, lights whatever changed since the last frame. |
| `panels.js` | The registers panel, the microcode listing, and the terminal (with cursor addressing for full-screen programs). libjs imports its `Terminal`. |
| `drives.js` | `DriveSet`, the drive logic with no DOM (which medium is in which drive, eject, soft reset), and `DrivePanel`, its view. Split so the logic can be tested in Node. |
| `store.js` | Media that survive a reload: a medium is a named set of files in IndexedDB. The backend is injectable; tests use a memory one. |

## Using it

Pick `c4ix32` or `c4ke32`, press Turbo, click the terminal and type. Or pick
`(BIOS)`: the firmware looks at the drives and boots the first medium it can.
The drives panel shows what each drive holds; New makes a blank writable
medium kept in this browser, Erase empties one, Delete removes it. The
firmware picker chooses how much of the machine is built (the stages in
[../fw/README.md](../fw/README.md)).

## Tests

- `node src/c4bb/tests/test-drives.mjs` and `test-terminal.mjs` test
  `drives.js`, `store.js` (memory backend) and the terminal in Node; both run
  inside `make test-c4bb`.
- `make test-c4bb-web` drives the real page over CDP in a browser that is
  already running, and skips (passing) when none answers. See
  [../tests/README.md](../tests/README.md).
