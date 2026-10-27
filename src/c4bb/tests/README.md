# c4bb/tests — image build and test scripts

Everything runs from the repo root; the make targets do that for you. None
of these are part of `make test`.

## Building the corpus

`build-images.sh` (`make c4bb-images`) builds everything under
`src/c4bb/images/` plus the four firmware stages in `../fw/`:

- the firmware, with c4lc under `c4sp32`;
- the test corpus (`src/tests/*.c`, the `bb_*` tests below, the libjs display
  demos) with `c4cc32`, so the parity suite covers both compilers;
- `factorial-fused.c4r` and the `fused/` disk for the fused-opcode
  differential;
- the kernels `c4ke32.c4r` and `c4ix32.c4r` and their userlands;
- four disks: `disk` (shared, three systems), `dos-recovery`, `c4ke-root`,
  and `climb`, which is **copied** from `c4dos-c4ix32/` at the repo root and
  never built here (`climb-disks.sh`, which also makes the page's ladder
  disk). `make c4bb-images` remakes that floppy after this script, since
  it carries the C4IX userland built here, and runs `climb-disks.sh` again;
  this script run by hand warns when the copy is older than its sources.

It needs `./cpp`, `c4cc32`, `c4sp32` and `c4th32.c4r`, which the make target
lists as prerequisites.

## Scripts

| File | Run by | What it checks |
|---|---|---|
| `test-c4bb.sh` | `make test-c4bb` | Every corpus image against native `c4m32` (or 64-bit c4m where `c4m32` cannot be trusted); C4KE and C4IX boot and their filesystems load; the trap tests; the PIT; all ten fused opcodes; step-vs-turbo lockstep; exit status; C4DOS on the shared disk. Also runs the `.mjs` tests marked below and `check-fork-arena.sh`. |
| `test-storage.sh` | `make test-c4bb-storage` | Drives and media: what one session writes, a later one reads back. |
| `test-firmware.sh` | `make test-c4bb-firmware` | Each firmware stage does its job and is denied the next one's. |
| `test-climb.sh` | `make test-c4bb-climb` (and `test-c4bb-install`) | Three power-ons, three systems, each booted from a medium the previous one wrote. |
| `test-web.mjs` | `make test-c4bb-web` | The page in a real browser over CDP (`C4BB_CDP` names the endpoint): the drives panel, a firmware stage refusing to boot, a written medium surviving a reload. Skips and passes when there is no browser. |
| `test-ladder.sh` | `make test-ladder` | C4DOS builds C4KE from source with no `init.c4r` on the floppy, hands over with `dosload`, and C4KE compiles and links in its RAM filesystem. Runs on native 64-bit c4m, not c4bb. Minutes. |
| `check-fork-arena.sh` | `test-c4bb.sh` | `sim/arena-whowrote.js` minus its marked blocks is byte-identical to `sim/arena.js`. |
| `test-terminal.mjs` | `test-c4bb.sh` | The web terminal's screen model, including cursor addressing. |
| `test-drives.mjs` | `test-c4bb.sh` | `web/drives.js` and `web/store.js`: a soft reset must not put back an ejected medium, writes persist. |
| `test-tlev-wedge.mjs` | `test-c4bb.sh` | A `TLEV` that restores its own address cannot wedge the machine. |
| `test-mbox.mjs` | `test-c4bb.sh` | The mailbox, host side, against `src/bb_mbox.c`. |
| `test-c4ke-mbox.mjs` | `test-c4bb.sh` | The mailbox under C4KE: `mbecho`, then `mbpair`. Fails about one run in four; see [../README.md](../README.md#limits-and-known-issues). |
| `test-uart-block.mjs` | by hand | The UART block write is indistinguishable downstream from byte-at-a-time writes. |
| `drive.mjs` | by hand | Types commands into `cli.js` at set times and logs the session: `node src/c4bb/tests/drive.mjs <log> <seconds> -- <cli.js args> :: cmd@sec ...`. |

## src/

Programs the board's own tests are built from (by `build-images.sh`, with
`c4cc32`). They exist because `src/tests/test_customop.c` predates `TLEV` and
`DBG` and its opcode numbers collide with them.

| File | What it checks |
|---|---|
| `bb_customop.c` | Custom opcodes and re-entrant traps, against native c4m. |
| `bb_preempt.c` | The cycle interrupt, with both ways of re-arming it. |
| `bb_pm.c` | Protected mode: violations and emulation. |
| `bb_pit.c` | The programmable interrupt timer. |
| `bb_mbox.c` | The mailbox: parity when absent, echoes when fitted. Built with `include/c4bb_mbox.h` passed as a source. |
| `bb_whowrote.c` | The `whowrote` provenance port. |
