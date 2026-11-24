/* vterm.h -- an 80 x 25 text screen as the VGA keeps it (a 16-bit cell
 * each: the character, then its colours), written as a terminal: CR, LF,
 * backspace, tab, and the ANSI escapes C4IX's programs send (cursor moves,
 * erases, colours). The PC's own console is one (pc.c, shown on the VGA's
 * text screen, or drawn on the framebuffer by screen.c's screen_console),
 * and so is each hypervisor guest's (hv.c: a guest with no display ring
 * shows it when attached, and in Machines). Static, for both to include. */
#ifndef C4PC_VTERM_H
#define C4PC_VTERM_H

typedef struct vterm {
  uint16_t cells[2000];
  int x, y;
  uint16_t attr;
  int esc, args[8], n;
  volatile uint16_t *mirror;           /* also written here (the VGA's text memory), or 0 */
} vterm;

static inline void vt_init(vterm *v, volatile uint16_t *mirror) {
  v->x = v->y = 0; v->attr = 0x0700; v->esc = v->n = 0; v->mirror = mirror;
  for (int i = 0; i < 2000; i++) v->cells[i] = 0x0700 | ' ';
}
static inline void vt_set(vterm *v, int i, uint16_t c) { v->cells[i] = c; if (v->mirror) v->mirror[i] = c; }
static inline void vt_scroll(vterm *v) {
  memmove(v->cells, v->cells + 80, 80 * 24 * 2);
  for (int i = 80 * 24; i < 80 * 25; i++) v->cells[i] = v->attr | ' ';
  if (v->mirror) for (int i = 0; i < 80 * 25; i++) v->mirror[i] = v->cells[i];
  v->y = 24;
}
static inline void vt_clear(vterm *v, int from, int to) { for (int i = from; i < to; i++) vt_set(v, i, v->attr | ' '); }
static inline void vt_csi(vterm *v, int c) {
  static const uint8_t ansi_vga[8] = { 0, 4, 2, 6, 1, 5, 3, 7 };
  int a0 = v->n > 0 ? v->args[0] : 0, a1 = v->n > 1 ? v->args[1] : 0;
  switch (c) {
    case 'A': v->y -= a0 ? a0 : 1; if (v->y < 0) v->y = 0; break;
    case 'B': v->y += a0 ? a0 : 1; if (v->y > 24) v->y = 24; break;
    case 'C': v->x += a0 ? a0 : 1; if (v->x > 79) v->x = 79; break;
    case 'D': v->x -= a0 ? a0 : 1; if (v->x < 0) v->x = 0; break;
    case 'H': case 'f': v->y = (a0 ? a0 : 1) - 1; v->x = (a1 ? a1 : 1) - 1;
      if (v->y > 24) v->y = 24;
      if (v->x > 79) v->x = 79;
      break;
    case 'J':
      if (a0 == 2) vt_clear(v, 0, 2000);
      else if (a0 == 1) vt_clear(v, 0, v->y * 80 + v->x + 1);
      else vt_clear(v, v->y * 80 + v->x, 2000);
      break;
    case 'K':
      if (a0 == 2) vt_clear(v, v->y * 80, v->y * 80 + 80);
      else if (a0 == 1) vt_clear(v, v->y * 80, v->y * 80 + v->x + 1);
      else vt_clear(v, v->y * 80 + v->x, v->y * 80 + 80);
      break;
    case 'm':
      if (!v->n) { v->attr = 0x0700; break; }
      for (int i = 0; i < v->n; i++) {
        int a = v->args[i];
        if (a == 0) v->attr = 0x0700;
        else if (a == 1) v->attr |= 0x0800;
        else if (a == 7) v->attr = (uint16_t)(((v->attr & 0x0f00) << 4) | ((v->attr & 0xf000) >> 4));
        else if (a >= 30 && a <= 37) v->attr = (uint16_t)((v->attr & 0xf800) | ansi_vga[a - 30] << 8);
        else if (a >= 40 && a <= 47) v->attr = (uint16_t)((v->attr & 0x0fff) | ansi_vga[a - 40] << 12);
        else if (a == 39) v->attr = (uint16_t)((v->attr & 0xf800) | 0x0700);
        else if (a == 49) v->attr &= 0x0fff;
      }
      break;
  }
}
/* C written: 1 when it ended an escape sequence (the cursor may have moved) */
static inline int vt_put(vterm *v, int c) {
  if (v->esc == 1) {
    if (c == '[') { v->esc = 2; v->n = 0; v->args[0] = 0; return 0; }
    v->esc = 0; return 0;
  }
  if (v->esc == 2) {
    if (c >= '0' && c <= '9') { if (!v->n) v->n = 1; v->args[v->n - 1] = v->args[v->n - 1] * 10 + (c - '0'); return 0; }
    if (c == ';') { if (!v->n) v->n = 1; if (v->n < 8) v->args[v->n++] = 0; return 0; }
    if (c == '?') return 0;
    v->esc = 0;
    vt_csi(v, c);
    return 1;
  }
  if (c == 27) { v->esc = 1; return 0; }
  if (c == '\n') { v->x = 0; if (++v->y > 24) vt_scroll(v); }
  else if (c == '\r') v->x = 0;
  else if (c == '\b') { if (v->x) v->x--; }
  else if (c == '\t') { v->x = (v->x + 8) & ~7; if (v->x > 79) { v->x = 0; if (++v->y > 24) vt_scroll(v); } }
  else if (c >= 32 || c < 0) {
    vt_set(v, v->y * 80 + v->x, v->attr | (uint8_t)c);
    if (++v->x > 79) { v->x = 0; if (++v->y > 24) vt_scroll(v); }
  }
  return 0;
}
static inline int vt_cursor(vterm *v) { return v->y * 80 + v->x; }

#endif
