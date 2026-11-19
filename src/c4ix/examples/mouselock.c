// mouselock.c -- the mouse locked to a window, as a game in a window
// steers with it (window.h, win_lock). docs/c4ix-desktop.md, part twenty-nine.
//
// In a Terminal on the desktop:
//
//   c4cc -o /tmp/mouselock.c4r /usr/include/window.h /usr/include/window.c /usr/src/c4ix/examples/mouselock.c
//   /tmp/mouselock.c4r
//
// Over the window the pointer says "click to capture"; a click locks the
// mouse to the display, and from then on its motion steers a dot (WIN_REL)
// and its buttons are counted, the left, the middle and the right. Esc lets
// the mouse go; a click takes it again. q ends it.
//
// Written for c4cc, as bounce.c is: no preprocessor, structs or arrays.

char *num;

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

// a label and a number after it
void line(int y, char *label, int v) {
    win_text(20, y, 0xc0c0c0, 13, label);
    win_text(200, y, 0xffff60, 13, itoa(num, v));
}

int main(int argc, char **argv) {
    int *ev, kind, running, dirty;
    int x, y, sx, sy, moves, downs, ups, last;

    if (!win_open("Mouse lock")) {
        printf("mouselock: no window\n");
        return 1;
    }
    win_lock(1);
    ev = (int *)malloc(3 * sizeof(int));
    num = (char *)malloc(32);
    x = WIN_W / 2; y = WIN_H / 2;
    sx = 0; sy = 0; moves = 0; downs = 0; ups = 0; last = 0 - 1;
    running = 1; dirty = 1;
    while (running) {
        kind = win_poll(ev);
        while (kind != WIN_NONE) {
            if (kind < 0) running = 0;                     // the window went away
            else if (kind == WIN_REL) {
                x = x + ev[0]; y = y + ev[1];
                if (x < 0) x = 0;
                if (x >= WIN_W) x = WIN_W - 1;
                if (y < 40) y = 40;
                if (y >= WIN_H) y = WIN_H - 1;
                sx = sx + ev[0]; sy = sy + ev[1];
                moves = moves + 1;
            } else if (kind == WIN_DOWN) { downs = downs + 1; last = ev[2]; }
            else if (kind == WIN_UP) ups = ups + 1;
            else if (kind == WIN_KEY && ev[1] == 'q') running = 0;
            dirty = 1;
            if (running) kind = win_poll(ev); else kind = WIN_NONE;
        }
        if (!running) break;
        if (dirty) {
            win_clear(0x102030);
            win_rect(0, 0, WIN_W, 34, 0x000080);
            win_text(10, 11, 0xffffff, 13, "Click to capture the mouse; Esc lets it go; q quits");
            line(60, "Locked moves:", moves);
            line(80, "Moved across:", sx);
            line(100, "Moved down:", sy);
            line(120, "Presses:", downs);
            line(140, "Releases:", ups);
            line(160, "Last button:", last);
            win_disc(x, y, 8, 0xff4020);
            win_present();
            dirty = 0;
        }
        win_sleep(20);
    }
    win_close();
    printf("mouselock: %d locked moves, %d across, %d down, %d presses, last button %d\n", moves, sx, sy, downs, last);
    return 0;
}
