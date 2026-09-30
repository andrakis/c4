# c4bb/tools — checks and small programs

| File | What it is |
|---|---|
| `lockstep.js` | Boots one image on both engines (the microstep interpreter and the compiled turbo engine) and compares every register, including MAR/MDR/B/T/U, and the cycle counter at each instruction boundary, then the output. `test-c4bb` runs it on several images. |
| `opscan.mjs` | Which opcodes an image uses, and whether that fits a rung of the ladder. The rungs (`base`, `dos`, `c4m`, `fused`) are defined in its `RUNGS` table and nowhere else. `-machine` checks that `hw/microcode.uc` implements exactly the top rung. Used by `make test-c4bb-rungs`. |
| `memcensus.mjs` | How much memory an image needs, from a real run under the full firmware: the highest address written (image, globals, heap) and the deepest machine stack. For choosing `-m` values. |
| `reboot.c` | A guest program that writes c4bb's reset register: the arena is cleared and the BIOS boots from the drives again, media intact. With a drive argument it ejects that drive first, so the BIOS does not boot the system you are leaving. It uses no OS API, so one image (`reboot32.c4r`, built by the Makefile) runs under C4DOS, C4KE and C4IX. |

## Usage

    node src/c4bb/tools/lockstep.js image.c4r [maxInstructions]

    node src/c4bb/tools/opscan.mjs [-q] -rung NAME  image.c4r [...]   # fits the rung
    node src/c4bb/tools/opscan.mjs [-q] -needs NAME image.c4r [...]   # does NOT fit the rung
    node src/c4bb/tools/opscan.mjs [-q] [-max N|NAME] image.c4r [...]
    node src/c4bb/tools/opscan.mjs -rungs                             # print the table
    node src/c4bb/tools/opscan.mjs -machine

    node src/c4bb/tools/memcensus.mjs [-m MB] [-c cycles] [-d dir] [-json out] image.c4r [...]

    reboot [drive]            (inside the guest: eject that drive if given, then restart;
                               a drive is "0", "1:" or a letter)

`-needs` is the half that keeps the ladder honest: an image that fits the
rung below its own did not need the CPU extended to run.
