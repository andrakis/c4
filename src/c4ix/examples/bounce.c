// bounce.c -- a first windowed program, to compile inside C4IX.
// docs/c4ix-desktop.md, part three.
//
// In a Command Prompt on the desktop:
//
//   c4cc -o /ram/bounce.c4r /usr/include/window.h /usr/src/examples/bounce.c
//   /ram/bounce.c4r
//
// The window becomes the program's: a ball bounces, the button counts
// clicks, a click anywhere else leaves a dot, and the last key pressed
// is shown. q or Esc ends it.
//
// Written for c4cc, which has no preprocessor, structs or arrays of its
// own: window.h is given to the compiler ahead of this file, and the
// dots live in memory from malloc.

enum { MAXDOTS = 64, BX = 470, BY = 330, BW = 140, BH = 40 };

int *dotx;
int *doty;
int *dotc;
int ndots;

int inside(int x, int y, int rx, int ry, int rw, int rh) {
    return x >= rx && y >= ry && x < rx + rw && y < ry + rh;
}

// A number as text, into buf.
char *itoa(char *buf, int v) {
    int n, k, d;
    char c;
    n = 0;
    if (v < 0) { buf[n] = '-'; n = n + 1; v = 0 - v; }
    k = n;
    d = 0;
    while (d == 0 || v > 0) { buf[n] = '0' + v % 10; v = v / 10; n = n + 1; d = 1; }
    buf[n] = 0;
    n = n - 1;
    while (k < n) { c = buf[k]; buf[k] = buf[n]; buf[n] = c; k = k + 1; n = n - 1; }
    return buf;
}

void button(int x, int y, int w, int h, char *label, int down) {
    win_rect(x, y, w, h, 0xc0c0c0);
    if (down) {
        win_frame(x, y, w, h, 0x000000);
    } else {
        win_line(x, y, x + w - 1, y, 0xffffff);
        win_line(x, y, x, y + h - 1, 0xffffff);
        win_line(x, y + h - 1, x + w - 1, y + h - 1, 0x404040);
        win_line(x + w - 1, y, x + w - 1, y + h - 1, 0x404040);
    }
    win_text(x + 18 + down, y + 13 + down, 0x000000, 13, label);
}

int main(int argc, char **argv) {
    int *ev, kind, running, frame;
    int x, y, dx, dy, r, clicks, pressed, key, mx, my;
    char *num;
    char *keytext;

    if (!win_open("Bounce - compiled inside C4IX")) {
        printf("bounce: no memory\n");
        return 1;
    }
    ev = (int *)malloc(3 * sizeof(int));
    dotx = (int *)malloc(MAXDOTS * sizeof(int));
    doty = (int *)malloc(MAXDOTS * sizeof(int));
    dotc = (int *)malloc(MAXDOTS * sizeof(int));
    num = (char *)malloc(32);
    keytext = (char *)malloc(64);
    ndots = 0;
    x = 100; y = 80; dx = 5; dy = 3; r = 18;
    clicks = 0; pressed = 0; key = 0; mx = 0; my = 0;
    frame = 0;
    running = 1;
    while (running) {
        // everything that has happened since the last frame
        kind = win_poll(ev);
        while (kind != WIN_NONE) {
            if (kind < 0) running = 0;                     // the window went away
            else if (kind == WIN_DOWN) {
                if (inside(ev[0], ev[1], BX, BY, BW, BH)) pressed = 1;
                else if (ndots < MAXDOTS) {
                    dotx[ndots] = ev[0]; doty[ndots] = ev[1];
                    dotc[ndots] = 0x2060ff + ((ndots * 0x3a1f) & 0xff00);
                    ndots = ndots + 1;
                }
            } else if (kind == WIN_UP) {
                if (pressed && inside(ev[0], ev[1], BX, BY, BW, BH)) clicks = clicks + 1;
                pressed = 0;
            } else if (kind == WIN_MOVE) {
                mx = ev[0]; my = ev[1];
            } else if (kind == WIN_KEY) {
                key = ev[1];
                if (key == 'q' || ev[0] == WIN_K_ESC) running = 0;
            }
            if (running) kind = win_poll(ev); else kind = WIN_NONE;
        }
        if (!running) break;

        // the ball
        x = x + dx; y = y + dy;
        if (x < r) { x = r; dx = 0 - dx; }
        if (x > WIN_W - r) { x = WIN_W - r; dx = 0 - dx; }
        if (y < 40 + r) { y = 40 + r; dy = 0 - dy; }
        if (y > WIN_H - r) { y = WIN_H - r; dy = 0 - dy; }

        // the frame
        win_clear(0x102030);
        win_rect(0, 0, WIN_W, 34, 0x000080);
        win_text(10, 11, 0xffffff, 13, "Compiled inside C4IX by c4cc, drawing through its Command Prompt");
        kind = 0;
        while (kind < ndots) { win_disc(dotx[kind], doty[kind], 5, dotc[kind]); kind = kind + 1; }
        win_disc(x + 4, y + 5, r, 0x000000);
        win_disc(x, y, r, 0xff4020);
        win_disc(x - 6, y - 6, 5, 0xffc0a0);
        button(BX, BY, BW, BH, "Click me", pressed);
        win_text(20, 330, 0xc0c0c0, 13, "Button clicks:");
        win_text(130, 330, 0xffff60, 13, itoa(num, clicks));
        win_text(20, 350, 0xc0c0c0, 13, "Last key:");
        if (key >= 32 && key < 127) { keytext[0] = key; keytext[1] = 0; }
        else if (key) itoa(keytext, key);
        else { keytext[0] = '-'; keytext[1] = 0; }
        win_text(130, 350, 0xffff60, 13, keytext);
        win_text(20, 370, 0xc0c0c0, 13, "Mouse:");
        win_text(130, 370, 0xffff60, 13, itoa(num, mx));
        win_text(170, 370, 0xffff60, 13, itoa(num, my));
        win_text(20, 50, 0x80a0c0, 11, "Click to drop a dot. q or Esc quits.");
        win_line(mx - 6, my, mx + 6, my, 0xffffff);
        win_line(mx, my - 6, mx, my + 6, 0xffffff);
        win_present();
        frame = frame + 1;
        win_sleep(33);
    }
    win_close();
    printf("bounce: %d frames, %d clicks, %d dots\n", frame, clicks, ndots);
    return 0;
}
