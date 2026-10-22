# libjs/guest — the display, from inside the machine

C code that runs as a guest and draws on libjs's display device. On any
machine without the display (native c4m, c4bb) the registers at 0x400 are
ordinary memory, so a program calls `gui_present()` first and stops with
"not fitted" if it returns 0. The device is announced, never probed.

| File | What it is |
|---|---|
| `gui.h` | The guest API. Register and command numbers, then the functions. |
| `gui-demo.c` | A bouncing box, a ring following the mouse, clicks and keys echoed. `gui-demo N` stops after N frames; `q` on the display quits. |
| `snd.h`, `snd.c` | The sound card's API: the AdLib, eight channels of 8-bit samples, the PC speaker. |
| `snd-demo.c` | A scale on the AdLib, a chord and a sample. `node libjs/cli.js --wav demo.wav src/c4bb/images/snd-demo.c4r` writes what it played. |
| `fb-demo.c` | The pixel framebuffer: every pixel of a 320x240 frame computed by the guest, with text on top. Takes events by interrupt (`gui_irq(1)` and a cycle handler) instead of polling. |

## Building

c4cc has no `#include`, so pass `gui.h` as a source ahead of the program.
`build-images.sh` (`make c4bb-images`) does this:

    ./c4cc32 -o gui-demo.c4r libjs/guest/gui.h libjs/guest/gui-demo.c
    ./c4cc32 -o fb-demo.c4r  libjs/guest/gui.h libjs/guest/fb-demo.c

c4lc can `#include` it; `src/c4ix/user/gui.c` and the C4IX desktop
(`src/c4ix/user/desktop/`) include `libjs/guest/gui.h`. `gui.h` is written
in the subset both compilers accept: locals at the top of each function,
literal enum values.

Run the demos at http://localhost:8472/libjs/web/?system=gui-demo or
`?system=fb-demo` after `make serve-libjs`.

## The contract

The guest hands the device one region of its own memory (`gui_attach`).
The first half is a ring of events to the guest, the second a ring of
command frames to the host, both in c4bb's mailbox format
(`include/c4bb_mbox.h`, `src/c4bb/sim/mbox.js`). Writing the bell register
tells the host to read.

| Group | Functions |
|---|---|
| Setup | `gui_present`, `gui_attach`, `gui_detach`, `gui_events` (event mask), `gui_size` |
| Drawing | `gui_clear`, `gui_rect`, `gui_recto`, `gui_line`, `gui_circle`, `gui_disc`, `gui_pixel`, `gui_text`, `gui_clip`, `gui_noclip` |
| Images | `gui_imgdef`, `gui_image` |
| Framing | `gui_show` (present), `gui_flush`, `gui_frame`/`gui_commit` (raw frames) |
| Framebuffer | `gui_fb`, `gui_fb_columns`, `gui_flip` |
| Input | `gui_poll`, `gui_irq`, `gui_mouse_x`, `gui_mouse_y`, `gui_buttons` |
| Time | `gui_ticks`, `gui_clock` |

A guest that never calls `gui_show` draws in immediate mode: each batch is
shown as it arrives. The host side is `../gui-device.js` and
`../display.js`; the design is in
[docs/libjs-design.md](../../docs/libjs-design.md), "The display device".
