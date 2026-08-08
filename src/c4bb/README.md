# c4bb — the c4m breadboard computer

A microcoded, 32-bit, hardware-style implementation of the c4m VM:
simulated in JavaScript, drawn as an interactive board in the browser,
verified instruction-for-instruction against native c4m, and capable
of booting C4KE and C4IX with preemption, protected mode and a
keyboard.

Full design: `docs/c4bb-design.md`. Quick start:

    make test-c4bb            # builds the 32-bit toolchain, firmware,
                              # images, both kernels; runs the suite

    # the board in a browser (from the repo root):
    python3 -m http.server 8471
    # -> http://localhost:8471/src/c4bb/web/index.html
    # pick c4ix32 or c4ke32, press Turbo, click the terminal, type.

    # headless / terminal:
    node src/c4bb/sim/cli.js -i -d src/c4bb/images/disk \
         src/c4bb/images/c4ix32.c4r

Layout: `hw/` microcode + board description (the source of truth),
`sim/` the engines and devices, `fw/` the boot firmware, `web/` the
board UI, `tools/` lockstep prover, `tests/` build + parity scripts.
