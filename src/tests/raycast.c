// raycast -- a console raycaster for the C4 family.
//
//   ./c4m load-c4r.c -- raycast.c4r            (bare c4m)
//   make run  ->  raycast                      (C4KE)
//   ./c4m load-c4r.c -- c4ix.c4r raycast.c4r   (C4IX, same image)
//   node src/c4bb/sim/cli.js -i -d <disk> c4ke32.c4r  ->  raycast
//
// One ray per terminal column, integer DDA against a 1-D map, wall
// height from the perpendicular distance, painted as 256-colour
// background blocks. No floating point anywhere: soft float exists
// (include/c4_float.h) and is far too slow to move a camera with.
//
// Built by c4lc with -conforming, so escape sequences are real: this
// is the first program in the tree that can write "\033[" instead of
// poking 27 in as an integer the way src/tests/mandel.c has to.
//
// THE 32-BIT RULE. c4bb is a 32-bit machine and native c4m is 64-bit,
// and this same source runs on both. No intermediate expression may
// exceed 2^30. Every multiply below is budgeted against that in a
// comment; the worst is 4095 * 131072 = 5.4e8. An overflow here does
// not crash, it draws a plausible-looking wrong picture, which is why
// test-raycast diffs 32-bit output against 64-bit rather than trusting
// inspection.
//
// Dialect: no switch and no function pointers, so the image stays free
// of JMPA/JSRI and a C4DOS build stays reachable (docs/c4dos-design.md).

// ---- platform port -------------------------------------------------
// Two builds from one source. -D RC_KE=1 is the normal one (C4KE,
// C4IX, c4m, c4bb); -D RC_DOS=1 restricts the program to stock-c4
// opcodes plus TIME so it can run as a C4DOS transient: no PUTS, no
// C4CY, no USLP. TIME stays, because C4DOS has a clock when
// CONFIG.SYS says DEVICE=CLOCK.SYS -- so the frame counter works
// there, and only the cycle figure goes dark.
//
// ALWAYS pass one of them. c4lc only honours '#' lines when the
// preprocessor is on, and -D is what turns it on -- compile with
// neither and both branches below are compiled, silently.
//
// puts is worth the split: on c4bb PUTS is real microcode
// (hw/microcode.uc) and the cycle counter only advances at instruction
// boundaries, so emitting a whole frame costs ONE cycle, where printf
// vectors into the firmware formatter (fw/fw.c) at ~15-20 instructions
// per character -- about 40,000 cycles for the same frame.
#ifdef RC_DOS
// puts appends a newline of its own, so the printf form must too:
// plat_emit means "the frame, then one newline" in both builds, and
// the frame relies on that newline landing at the home position.
#define plat_emit(b)     printf("%s\n", (b))
#define plat_cyc()       0
#define plat_usleep(us)  0
#else
int plat_emit   (char *b)  { puts(b); return 0; }
int plat_cyc    ()         { return __c4_cycles(); }
int plat_usleep (int us)   { __c4_usleep(us); return 0; }
#endif

// ---- fixed point ---------------------------------------------------
// Q12. Angles are BAM -- 256 units to a turn, held as a Q6 sub-index
// so the whole circle is 16384 -- which makes wrapping an AND and
// removes the negative-angle case entirely.
enum { FP_BITS = 12, FP_ONE = 1 << FP_BITS, FP_HALF = FP_ONE / 2,
       FP_MASK = FP_ONE - 1 };
enum { ANG_FULL = 16384, ANG_MASK = ANG_FULL - 1, ANG_QUARTER = ANG_FULL / 4 };
enum { DELTA_MAX = 131072,   // 32 cells in Q12: the deltaDist clamp
       DIST_MAX  = 131072,
       DDA_STEPS = 96,
       PLANE_066 = 2703 };   // 0.66 * 4096, the classic 66-degree FOV

// ---- state ---------------------------------------------------------
int *sintab;              // 257 entries, Q12; [256] duplicates [0]
int *map;                 // mw*mh, 0 = open, 1..5 = wall class
int mw, mh;
int px, py, ang;          // player position (Q12) and heading (BAM)
int vw, vh, hz;           // view geometry and horizon row
int *col_top, *col_bot, *col_bg;   // one wall span per column
int min_top, max_bot;              // the wall band, for the fast path
int *ov_ch, *ov_bg;                // HUD overlay, HUD_ROWS deep only
char *frame;              // the emitted byte stream
int g_dist, g_side;       // cast() results
int *pal;                 // pal[class*6 + shade] -> xterm-256 index

// wall class -> a colour cube triple, brightest shade
int cls_r[6] = { 0, 5, 1, 1, 5, 4 };
int cls_g[6] = { 0, 1, 4, 2, 4, 4 };
int cls_b[6] = { 0, 1, 1, 5, 1, 4 };

int *visits;              // demo-mode coverage, one counter per cell
int rng;                  // xorshift state, 1..65535
int in_fd;                // non-blocking input, -1 = none
char *in_buf;
int *kq, kq_head, kq_tail;
int demo, facing, idle, hud, covered;
int dem_state, dem_t, dem_n, dem_a0, dem_da, dem_x0, dem_y0, dem_x1, dem_y1;
int fps_t0, fps_c0, fps_n, fps_val, fps_kc;
int frame_ms, running;

int dx4[4] = {  1,  0, -1,  0 };     // 0=E 1=S 2=W 3=N
int dy4[4] = {  0,  1,  0, -1 };
int pref[4] = { 0, 3, 1, 2 };        // front, left, right, back

enum { O_RDONLY = 0, O_NONBLOCK = 0x800, IN_BUF = 64, KQ_SZ = 64 };
enum { DS_IDLE = 0, DS_TURN = 1, DS_WALK = 2, TURN_FR = 10, WALK_FR = 14 };
enum { IDLE_FR = 250, MM_W = 23, MM_H = 9, HUD_BG = 236, HUD_ROWS = 11 };
enum { STEP = FP_ONE / 6, TURN = ANG_FULL / 48 };

int iabs (int v) { if (v < 0) return 0 - v; return v; }

// ---- trig ----------------------------------------------------------
// Bhaskara I's approximation, with pi-squared cancelled out. Writing
// sin(x) = 16x(pi-x) / (5pi^2 - 4x(pi-x)) and substituting x = pi*i/128
// makes every pi^2 divide away, leaving a purely rational expression in
// i -- so a table that would otherwise need a float generator is ten
// lines of integer code that gives bit-identical results at 32 and 64
// bits. Worst-case error 0.17%, exact at 0, pi/2 and pi, and perfectly
// symmetric, so it cannot drift.
void sin_init () {
  int i, p;
  sintab = malloc(257 * sizeof(int));
  for (i = 0; i <= 128; ++i) {
    p = i * (128 - i);                                // <= 4096
    sintab[i] = (16 * p * FP_ONE) / (81920 - 4 * p);  // 2.7e8, ok
  }
  for (i = 129; i < 256; ++i) sintab[i] = 0 - sintab[i - 128];
  sintab[256] = 0;
}

int sin_q (int a) {
  int i, f;
  a = a & ANG_MASK;
  i = a >> 6;
  f = a & 63;
  return sintab[i] + (((sintab[i + 1] - sintab[i]) * f) >> 6);
}
int cos_q (int a) { return sin_q(a + ANG_QUARTER); }

// ---- palette -------------------------------------------------------
// The xterm 6x6x6 colour cube: 16 + 36r + 6g + b. Shade scales all
// three components together, so a wall keeps its hue as it recedes.
void pal_init () {
  int c, s, r, g, b;
  pal = malloc(6 * 6 * sizeof(int));
  for (c = 1; c <= 5; ++c) {
    for (s = 0; s < 6; ++s) {
      r = (cls_r[c] * (6 - s)) / 6;
      g = (cls_g[c] * (6 - s)) / 6;
      b = (cls_b[c] * (6 - s)) / 6;
      pal[c * 6 + s] = 16 + 36 * r + 6 * g + b;
    }
  }
}

int shade_of (int dist, int side) {
  int s;
  s = dist >> 13;            // 8 buckets across 0..32 cells
  if (side) ++s;             // the free N/S vs E/W lighting cue
  if (s > 5) s = 5;
  return s;
}

// ---- the DDA -------------------------------------------------------
// Grid-crossing, not ray-marching: one step per gridline crossed,
// typically 8-20 rather than the ~192 a 1/8-cell march would take, and
// with no stair-stepping on the wall face.
// The camera basis is the same for every column, so it is computed
// once per frame rather than once per ray. Doing it inside cast() cost
// four table lookups and two multiplies per column -- 320 sin_q calls
// a frame -- and was most of the frame time before this was hoisted.
int c_dirx, c_diry, c_planx, c_plany;

void camera () {
  c_dirx = cos_q(ang);
  c_diry = sin_q(ang);
  c_planx = (0 - c_diry) * PLANE_066 >> FP_BITS;   // 1.1e7, ok
  c_plany =      c_dirx  * PLANE_066 >> FP_BITS;
}

int cast (int col) {
  int camx, rdx, rdy, ddx, ddy, sdx, sdy, sx, sy, mx, my, side, n, c;
  int dirx, diry, planx, plany;

  dirx = c_dirx; diry = c_diry; planx = c_planx; plany = c_plany;

  camx = (2 * col * FP_ONE) / vw - FP_ONE;      // -4096 .. +4096
  rdx  = dirx + ((planx * camx) >> FP_BITS);
  rdy  = diry + ((plany * camx) >> FP_BITS);

  if (rdx) ddx = (FP_ONE << FP_BITS) / iabs(rdx); else ddx = DELTA_MAX;
  if (rdy) ddy = (FP_ONE << FP_BITS) / iabs(rdy); else ddy = DELTA_MAX;
  // The clamp is what keeps the next two multiplies inside the budget:
  // 4095 * 131072 = 5.4e8. A ray whose delta is clamped simply never
  // wins the sideDist comparison, which is the right answer anyway.
  if (ddx > DELTA_MAX) ddx = DELTA_MAX;
  if (ddy > DELTA_MAX) ddy = DELTA_MAX;

  mx = px >> FP_BITS;  my = py >> FP_BITS;

  if (rdx < 0) { sx = 0 - 1; sdx = ((px & FP_MASK) * ddx) >> FP_BITS; }
  else         { sx = 1;     sdx = ((FP_ONE - (px & FP_MASK)) * ddx) >> FP_BITS; }
  if (rdy < 0) { sy = 0 - 1; sdy = ((py & FP_MASK) * ddy) >> FP_BITS; }
  else         { sy = 1;     sdy = ((FP_ONE - (py & FP_MASK)) * ddy) >> FP_BITS; }

  c = 0; n = 0; side = 0;
  while (!c && n < DDA_STEPS) {
    if (sdx < sdy) { sdx = sdx + ddx; mx = mx + sx; side = 0; }
    else           { sdy = sdy + ddy; my = my + sy; side = 1; }
    ++n;
    if (mx < 0 || my < 0 || mx >= mw || my >= mh) { c = 0; n = DDA_STEPS; }
    else c = map[my * mw + mx];
  }

  // Perpendicular distance is a SUBTRACTION, not a second division:
  // back off the one whole deltaDist we just stepped past. This is
  // what makes DDA cheap, and using the perpendicular rather than the
  // euclidean distance is what removes the fish-eye.
  if (side) g_dist = sdy - ddy; else g_dist = sdx - ddx;
  if (g_dist < 256) g_dist = 256;         // 1/16 cell: caps column height
  if (g_dist > DIST_MAX) { g_dist = DIST_MAX; c = 0; }
  g_side = side;
  return c;
}

// ---- the character grid --------------------------------------------
void render () {
  int col, c, h, top, bot;

  camera();
  min_top = vh; max_bot = 0;
  for (col = 0; col < vw; ++col) {
    c = cast(col);
    if (!c) { col_top[col] = vh; col_bot[col] = vh; col_bg[col] = 0; continue; }
    h = (vh * FP_ONE) / g_dist;           // 25*4096 = 102400, tiny
    top = (vh - h) / 2;
    bot = top + h;
    if (top < 0) top = 0;
    if (bot > vh) bot = vh;
    col_top[col] = top;
    col_bot[col] = bot;
    col_bg[col] = pal[c * 6 + shade_of(g_dist, g_side)];
    if (top < min_top) min_top = top;
    if (bot > max_bot) max_bot = bot;
  }
}

// ---- prng ----------------------------------------------------------
// 16-bit xorshift, and the width is the whole point. The obvious LCG
// (state * 1103515245 + 12345, which u0.h's rand uses) overflows
// DIFFERENTLY at 32 and 64 bits, so the same seed would grow a
// different maze on c4bb than on native c4m and no golden test could
// ever hold. Every intermediate here is at most 2^23.
int rnd16 () {
  rng = rng ^ ((rng << 7) & 65535);
  rng = rng ^ (rng >> 9);
  rng = rng ^ ((rng << 8) & 65535);
  return rng;
}
int rnd_below (int n) { return (rnd16() >> 3) % n; }   // low bits are weak

// ---- map -----------------------------------------------------------
// Iterative recursive-backtracker. Binary-tree and sidewinder need no
// stack, but both leave a tell-tale fully open edge and a diagonal
// bias that reads as obviously wrong in first person; the backtracker
// gives the long winding corridors that are the Windows 3 maze look.
// The stack is the only thing recursion would have bought and it is
// one malloc.
void gen_map () {
  int *stk, sp, cur, cx, cy, i, n, d, k, pick, nx, ny;
  int *cand;

  for (i = 0; i < mw * mh; ++i) { map[i] = 5; visits[i] = 0; }

  stk = malloc(mw * mh * sizeof(int));
  cand = malloc(4 * sizeof(int));
  cx = 1; cy = 1;
  map[cy * mw + cx] = 0;
  sp = 0; stk[sp] = cy * mw + cx; ++sp;

  while (sp) {
    cur = stk[sp - 1];
    cx = cur % mw; cy = cur / mw;
    n = 0;
    for (d = 0; d < 4; ++d) {
      nx = cx + dx4[d] * 2; ny = cy + dy4[d] * 2;
      if (nx > 0 && ny > 0 && nx < mw - 1 && ny < mh - 1)
        if (map[ny * mw + nx]) { cand[n] = d; ++n; }
    }
    if (!n) { --sp; continue; }
    pick = cand[rnd_below(n)];
    map[(cy + dy4[pick]) * mw + (cx + dx4[pick])] = 0;         // the wall
    map[(cy + dy4[pick] * 2) * mw + (cx + dx4[pick] * 2)] = 0;  // the cell
    stk[sp] = (cy + dy4[pick] * 2) * mw + (cx + dx4[pick] * 2); ++sp;
  }
  free(stk); free(cand);

  // Braid: knock out roughly one dead end in eight. A perfect maze
  // makes the demo explorer ping-pong down every stub; a few loops
  // keep it moving and read as architecture rather than as a puzzle.
  for (cy = 1; cy < mh - 1; ++cy) {
    for (cx = 1; cx < mw - 1; ++cx) {
      if (map[cy * mw + cx]) continue;
      n = 0;
      for (d = 0; d < 4; ++d)
        if (!map[(cy + dy4[d]) * mw + cx + dx4[d]]) ++n;
      if (n == 1 && rnd_below(8) == 0) {
        for (k = 0; k < 4; ++k) {
          d = rnd_below(4);
          nx = cx + dx4[d]; ny = cy + dy4[d];
          if (nx > 0 && ny > 0 && nx < mw - 1 && ny < mh - 1)
            if (map[ny * mw + nx]) { map[ny * mw + nx] = 0; k = 4; }
        }
      }
    }
  }

  // Colour by zone, not per cell: blocky regions give the player
  // something to navigate by, where per-cell random is just noise.
  for (cy = 0; cy < mh; ++cy) {
    for (cx = 0; cx < mw; ++cx) {
      if (!map[cy * mw + cx]) continue;
      if (cx == 0 || cy == 0 || cx == mw - 1 || cy == mh - 1)
        map[cy * mw + cx] = 5;
      else
        map[cy * mw + cx] = 1 + (((cx >> 2) + (cy >> 2)) % 4);
    }
  }
}

// ---- input ---------------------------------------------------------
// Never fd 0: a read there blocks the HOST, which stops the whole VM
// and every other task with it (src/c4ix/console.c says so at length).
// /dev/tty first because c4bb answers that with a raw, per-keystroke
// descriptor; /dev/stdin is the portable fallback, and on hosts whose
// line discipline holds bytes until Enter the key queue below turns
// that into batched moves rather than a broken game.
int in_pipe;     // -K: take keys from stdin rather than the terminal

void in_init () {
  in_buf = malloc(IN_BUF);
  kq = malloc(KQ_SZ * sizeof(int));
  kq_head = 0; kq_tail = 0;
  in_fd = 0 - 1;
  // /dev/tty first: c4bb answers that with a RAW keyboard, one that
  // hands over a keystroke without waiting for Enter. -K skips it,
  // because a scripted run wants the pipe on stdin and not whatever
  // terminal happens to be attached -- which is also the difference
  // between a test that passes everywhere and one that passes only on
  // a machine with a controlling tty.
  if (!in_pipe) in_fd = open("/dev/tty", O_RDONLY | O_NONBLOCK);
  if (in_fd < 0) in_fd = open("/dev/stdin", O_RDONLY | O_NONBLOCK);
}

int in_eof;

void in_poll () {
  int n, i, nx;
  if (in_fd < 0) return;
  n = read(in_fd, in_buf, IN_BUF);
  // -1 is EAGAIN, "nothing yet". 0 is real end of input: on a terminal
  // that never happens, but under a pipe it happens forever, and
  // polling a dead descriptor every frame would spin with no way left
  // to quit. Retire the descriptor and fall back to the screensaver.
  if (n == 0) { close(in_fd); in_fd = 0 - 1; in_eof = 1; return; }
  if (n < 0) return;
  for (i = 0; i < n; ++i) {
    nx = (kq_tail + 1) % KQ_SZ;
    if (nx != kq_head) { kq[kq_tail] = in_buf[i] & 255; kq_tail = nx; }
  }
}

int in_next () {
  int c;
  if (kq_head == kq_tail) return 0 - 1;
  c = kq[kq_head];
  kq_head = (kq_head + 1) % KQ_SZ;
  return c;
}

// ---- demo mode -----------------------------------------------------
// A min-visit walk, not a wall follower. A left-hand follower covers a
// PERFECT maze, but this one is braided, and the moment there is a
// loop the follower can orbit it forever -- which is the one thing a
// screensaver must never do. Preferring the least-visited neighbour
// covers a connected graph however it is shaped, and taking front
// before left/right/back keeps it running down corridors instead of
// jittering, which is what makes it look like the Windows 3 saver.
int pick_dir (int cx, int cy) {
  int k, d, nx, ny, best, bestv, v;
  best = 0 - 1; bestv = 0;
  for (k = 0; k < 4; ++k) {
    d = (facing + pref[k]) & 3;
    nx = cx + dx4[d]; ny = cy + dy4[d];
    if (nx < 0 || ny < 0 || nx >= mw || ny >= mh) continue;
    if (map[ny * mw + nx]) continue;
    v = visits[ny * mw + nx];
    if (best < 0 || v < bestv) { best = d; bestv = v; }
  }
  return best;
}

void demo_step () {
  int cx, cy, d, want;

  if (dem_state == DS_IDLE) {
    cx = px >> FP_BITS; cy = py >> FP_BITS;
    if (!visits[cy * mw + cx]) ++covered;
    ++visits[cy * mw + cx];
    d = pick_dir(cx, cy);
    if (d < 0) { facing = (facing + 2) & 3; return; }
    if (d != facing) {
      want = d - facing;                        // shortest signed turn
      if (want > 2) want = want - 4;
      if (want < 0 - 2) want = want + 4;
      dem_a0 = ang; dem_da = want * ANG_QUARTER;
      dem_state = DS_TURN; dem_t = 0; dem_n = TURN_FR;
      facing = d;
    } else {
      dem_x0 = px; dem_y0 = py;
      dem_x1 = ((cx + dx4[d]) << FP_BITS) + FP_HALF;
      dem_y1 = ((cy + dy4[d]) << FP_BITS) + FP_HALF;
      dem_state = DS_WALK; dem_t = 0; dem_n = WALK_FR;
    }
    return;
  }

  ++dem_t;
  if (dem_state == DS_TURN) {
    ang = (dem_a0 + (dem_da * dem_t) / dem_n) & ANG_MASK;  // 4096*10, ok
  } else {
    px = dem_x0 + ((dem_x1 - dem_x0) * dem_t) / dem_n;     // 4096*14, ok
    py = dem_y0 + ((dem_y1 - dem_y0) * dem_t) / dem_n;
  }
  if (dem_t >= dem_n) dem_state = DS_IDLE;
}

// ---- movement ------------------------------------------------------
// Collision uses a margin so the camera never gets close enough to a
// wall for the near-distance clamp in cast() to matter.
void try_move (int nx, int ny) {
  int mx, my, cx, cy;
  cx = px >> FP_BITS; cy = py >> FP_BITS;
  mx = nx >> FP_BITS; my = ny >> FP_BITS;
  // Axis-separated, so walking into a wall at an angle slides along it
  // instead of stopping dead.
  if (mx >= 0 && mx < mw && !map[cy * mw + mx]) px = nx;
  cx = px >> FP_BITS;
  if (my >= 0 && my < mh && !map[my * mw + cx]) py = ny;
}

void walk (int sign) {
  int nx, ny;
  nx = px + (cos_q(ang) * sign * STEP >> FP_BITS);   // 4096*682, ok
  ny = py + (sin_q(ang) * sign * STEP >> FP_BITS);
  try_move(nx, ny);
}

// ---- emission ------------------------------------------------------
char *put_num (char *p, int v) {
  if (v >= 100) { *p++ = '0' + v / 100; v = v % 100;
                  *p++ = '0' + v / 10;  *p++ = '0' + v % 10; }
  else if (v >= 10) { *p++ = '0' + v / 10; *p++ = '0' + v % 10; }
  else *p++ = '0' + v;
  return p;
}

char *put_str (char *p, char *s) { while (*s) *p++ = *s++; return p; }

// Every row opens with its own SGR and closes with a reset. That is
// not tidiness: HOMEWARD renders a sliding window of the last N lines
// (ansiToHtml(term.tail(22))), so its SGR parser starts fresh at the
// top of the visible area -- colour carried across a line boundary is
// lost the moment that line scrolls off the top.
void frame_build () {
  char *p;
  int row, col, cur, bg, ch, bgrow, band, o, oi;

  // Home the cursor and overwrite the previous frame in place. Printing
  // vh fresh lines instead makes the terminal scroll a whole screen
  // every frame, which is what makes the picture visibly jump. The last
  // row deliberately gets NO newline, and the frame ends back at home,
  // so the newline puts() appends lands at the top-left where it cannot
  // scroll anything. A terminal that drops CSI (an embedded teletype)
  // just sees the rows and a separator, exactly as it did before.
  p = frame;
  p = put_str(p, "\033[H");
  for (row = 0; row < vh; ++row) {
    cur = 0 - 2;                        // impossible: forces an SGR at col 0
    // Ceiling and floor are constant across a row, so a row with no
    // wall in it is a SINGLE colour run however wide the terminal is.
    if (row < hz) bgrow = 233 + (row * 3) / hz;
    else          bgrow = 240 - ((row - hz) * 3) / (vh - hz);
    band = 0;
    if (hud && row < HUD_ROWS) band = 1;
    oi = row * vw;

    // A row entirely above every wall, or entirely below every wall,
    // is one colour from end to end. memset is a single VM opcode, so
    // filling such a row costs one instruction instead of vw stores --
    // and in an open corridor that is most of the screen.
    if (!band && (row < min_top || row >= max_bot)) {
      p = put_str(p, "\033[48;5;");
      p = put_num(p, bgrow);
      *p++ = 'm';
      memset(p, ' ', vw);
      p = p + vw;
      p = put_str(p, "\033[0m");
      if (row < vh - 1) *p++ = '\n';
      continue;
    }

    for (col = 0; col < vw; ++col) {
      ch = ' ';
      if (row < col_top[col] || row >= col_bot[col]) bg = bgrow;
      else bg = col_bg[col];
      // The overlay is consulted only inside the HUD band, so the
      // other three quarters of the screen never pay for it.
      if (band) {
        o = ov_bg[oi + col];
        if (o >= 0) { bg = o; ch = ov_ch[oi + col]; }
      }
      if (bg != cur) {
        cur = bg;
        if (cur < 0) p = put_str(p, "\033[49m");
        else {
          p = put_str(p, "\033[48;5;");
          p = put_num(p, cur);
          *p++ = 'm';
        }
      }
      *p++ = ch;
    }
    p = put_str(p, "\033[0m");
    if (row < vh - 1) *p++ = '\n';
  }
  p = put_str(p, "\033[H");
  *p = 0;
}

// ---- HUD -----------------------------------------------------------
// Composited into the character grid, never positioned with escapes.
// HOMEWARD's terminal and c4bb's web terminal implement SGR and drop
// every other CSI sequence, so ESC[H and ESC[2J do nothing at all
// there: the only way to put something in a corner is to draw it into
// the corner before the frame is built.
void put_cell (int col, int row, int ch, int bg) {
  if (col < 0 || row < 0 || col >= vw || row >= HUD_ROWS) return;
  ov_ch[row * vw + col] = ch;
  ov_bg[row * vw + col] = bg;
}

void hud_text (int col, int row, char *sv) {
  while (*sv) { put_cell(col, row, *sv, HUD_BG); ++col; ++sv; }
}

char *numbuf;    // allocated once: malloc is FIRMWARE on c4bb, so a
                 // malloc/free pair per HUD field would be six round
                 // trips through guest code every single frame

// Right-aligned in a FIXED-WIDTH field, blanks included. A field that
// grew and shrank with its value would shuffle every cell after it
// along the row, which looks like jitter to a player and, worse, makes
// two runs with different frame rates differ in their colour-run
// layout -- so the cross-host frame comparison would fail on a number
// that is legitimately host-dependent.
void hud_num (int col, int row, int v, int w) {
  int n;
  n = 0;
  if (!v) { numbuf[n] = '0'; ++n; }
  while (v > 0) { numbuf[n] = '0' + v % 10; ++n; v = v / 10; }
  if (n > w) n = w;                      // too big to show: keep the field
  while (w > n) { put_cell(col, row, ' ', HUD_BG); ++col; --w; }
  while (n) { --n; put_cell(col, row, numbuf[n], HUD_BG); ++col; }
}

void hud_draw () {
  int x, y, ox, oy, mx, my, c, ch, bg, pcol, prow;

  if (!hud) return;
  for (x = 0; x < vw * HUD_ROWS; ++x) ov_bg[x] = 0 - 1;

  // a window on the map, centred on the player and clamped to it
  ox = (px >> FP_BITS) - MM_W / 2;
  oy = (py >> FP_BITS) - MM_H / 2;
  if (ox < 0) ox = 0;
  if (oy < 0) oy = 0;
  if (ox > mw - MM_W) ox = mw - MM_W;
  if (oy > mh - MM_H) oy = mh - MM_H;
  if (ox < 0) ox = 0;
  if (oy < 0) oy = 0;

  for (y = 0; y < MM_H; ++y) {
    for (x = 0; x < MM_W; ++x) {
      mx = ox + x; my = oy + y;
      if (mx >= mw || my >= mh) continue;
      c = map[my * mw + mx];
      if (c) { ch = ' '; bg = pal[c * 6 + 3]; }
      else if (visits[my * mw + mx]) { ch = '.'; bg = 236; }
      else { ch = ' '; bg = 234; }
      put_cell(x + 1, y + 1, ch, bg);
    }
  }
  // the player: a bright dot, drawn last so nothing covers it
  pcol = (px >> FP_BITS) - ox + 1;
  prow = (py >> FP_BITS) - oy + 1;
  put_cell(pcol, prow, '@', 226);

  // frame rate top right. Both numbers on purpose: on c4bb __time() is
  // derived from the cycle counter (a defined 1MHz machine, no wall
  // clock), so f/s is a simulated figure there while kc -- thousands
  // of VM instructions per frame -- is the honest one, and it is the
  // number that matters when tuning against HOMEWARD's cycle budget.
  hud_text(vw - 28, 0, "seen ");
  hud_num (vw - 23, 0, covered, 4);
  hud_text(vw - 18, 0, "f/s ");
  hud_num (vw - 14, 0, fps_val, 5);
  hud_text(vw - 8,  0, "kc ");
  hud_num (vw - 5,  0, fps_kc,  5);
}

void fps_tick () {
  int t, c;
  ++fps_n;
  if (fps_n < 8) return;
  t = __time(); c = plat_cyc();
  if (t != fps_t0) fps_val = (fps_n * 1000) / (t - fps_t0);
  if (c != fps_c0) fps_kc = (c - fps_c0) / (fps_n * 1000);
  fps_t0 = t; fps_c0 = c; fps_n = 0;
}

// ---- argv ----------------------------------------------------------
int isnum (int c) { if (c >= '0') return c <= '9'; return 0; }

int atoi_at (char **pp) {
  char *a;
  int v;
  a = *pp; v = 0;
  while (*a && isnum(*a)) { v = v * 10 + (*a - '0'); ++a; }
  *pp = a;
  return v;
}

void usage (char *a0) {
  printf("raycast -- console raycaster for the C4 family\n");
  printf("usage: %s [WxH] [-g COLSxROWS] [-s SEED] [-f MS] [-n N] [-d] [-i]\n", a0);
  printf("  WxH   map size in cells (default 21x21, forced odd, 9..63)\n");
  printf("  -g    terminal geometry (default 80x25)\n");
  printf("  -s N  maze seed        -n N  render N frames then exit\n");
  printf("  -f N  frame cap in ms  -d/-i  force demo / interactive\n");
  printf("  -K    read keys from stdin instead of the terminal\n");
  printf("keys: w a s d move, m minimap, p demo, q quit\n");
  printf("  q is the quit key, not ESC: hosts that embed this terminal\n");
  printf("  intercept ESC for their own console toggle.\n");
}

int main (int argc, char **argv) {
  int i, nframes, seed, c, want_demo, want_inter, gw, gh, ok, dump_map;
  char **av, *arg;
  int ac;

  vw = 80; vh = 25;
  mw = 21; mh = 21;
  nframes = 0; seed = 0; frame_ms = 0; dump_map = 0; in_pipe = 0;
  want_demo = 0; want_inter = 0;
  hud = 1; running = 1; demo = 1; facing = 0;

  // mandel.c's argument walk: the house idiom in this tree
  ac = argc; av = argv;
  --ac; ++av;
  while (ac) {
    arg = *av;
    if (*arg == '-') {
      ++arg;
      if (*arg == 'h') { usage(argv[0]); return 0; }
      else if (*arg == 'M') dump_map = 1;
      else if (*arg == 'K') in_pipe = 1;
      else if (*arg == 'd') want_demo = 1;
      else if (*arg == 'i') want_inter = 1;
      else if (*arg == 'g' || *arg == 's' || *arg == 'n' || *arg == 'f') {
        c = *arg;
        ++arg;
        if (!*arg) { --ac; ++av; if (!ac) { usage(argv[0]); return 1; } arg = *av; }
        if (c == 'g') {
          gw = atoi_at(&arg);
          if (*arg == 'x') { ++arg; gh = atoi_at(&arg); } else gh = 25;
          if (gw > 8 && gh > 6) { vw = gw; vh = gh; }
        }
        else if (c == 's') seed = atoi_at(&arg);
        else if (c == 'n') nframes = atoi_at(&arg);
        else if (c == 'f') frame_ms = atoi_at(&arg);
      }
      else { printf("raycast: unknown option -%c\n", *arg); usage(argv[0]); return 1; }
    } else if (isnum(*arg)) {
      mw = atoi_at(&arg);
      if (*arg == 'x') { ++arg; mh = atoi_at(&arg); } else mh = mw;
    } else {
      printf("raycast: unrecognised argument '%s'\n", *av);
      usage(argv[0]);
      return 1;
    }
    --ac; ++av;
  }

  // odd sizes only (the backtracker carves on the odd lattice), and an
  // upper bound that keeps px = mw << 12 far inside the 32-bit budget
  if (mw < 9) mw = 9;
  if (mh < 9) mh = 9;
  if (mw > 63) mw = 63;
  if (mh > 63) mh = 63;
  if (!(mw & 1)) ++mw;
  if (!(mh & 1)) ++mh;
  hz = vh / 2;

  sin_init();
  pal_init();

  map = malloc(mw * mh * sizeof(int));
  visits = malloc(mw * mh * sizeof(int));
  numbuf  = malloc(24);
  col_top = malloc(vw * sizeof(int));
  col_bot = malloc(vw * sizeof(int));
  col_bg  = malloc(vw * sizeof(int));
  ov_ch   = malloc(vw * HUD_ROWS * sizeof(int));
  ov_bg   = malloc(vw * HUD_ROWS * sizeof(int));
  frame = malloc(vh * (vw * 12 + 16) + 8);
  if (!map || !visits || !col_top || !col_bot || !col_bg || !ov_bg || !frame) {
    printf("raycast: out of memory\n");
    return 1;
  }

  if (!seed) seed = __time() ^ plat_cyc();
  rng = seed & 65535;
  if (!rng) rng = 1;
  gen_map();

  // -M dumps the generated maze as text and exits: the fastest way to
  // tell a bad generator from a bad renderer, and it makes the maze
  // itself testable without going through the raycaster at all.
  if (dump_map) {
    // One printf per row rather than putchar per cell: putchar is PUTC,
    // which is a c4m opcode, and the RC_DOS build is restricted to the
    // stock-c4 set plus TIME.
    for (c = 0; c < mh; ++c) {
      for (i = 0; i < mw; ++i) frame[i] = map[c * mw + i] ? '#' : ' ';
      frame[mw] = 0;
      printf("%s\n", frame);
    }
    return 0;
  }

  px = FP_ONE + FP_HALF;
  py = FP_ONE + FP_HALF;
  ang = 0;
  covered = 0;

  // -d never reads the keyboard, so it never opens one: that keeps the
  // whole run a pure function of (seed, frame count, geometry), which
  // is what makes the golden test possible. Opening it anyway would
  // make the output depend on whether the host happens to have a
  // controlling terminal, which is exactly the bug this comment
  // replaces.
  in_fd = 0 - 1;
  if (!want_demo) {
    in_init();
    // Degrade to a screensaver rather than block: with no input there
    // is no way to steer and no way to quit, so a frame count is the
    // only thing that can end the run.
    if (in_fd < 0) {
      printf("raycast: no input device, demo mode\n");
      if (!nframes) nframes = 600;
    }
  }
  if (in_fd < 0) demo = 1;
  if (want_demo) demo = 1;
  if (want_inter && in_fd >= 0) demo = 0;

  // Clear once, and hide the cursor: with the frame repainting in place
  // a visible cursor would flicker across the picture on every row.
  printf("\033[2J\033[?25l");

  fps_t0 = __time(); fps_c0 = plat_cyc(); fps_n = 0;

  i = 0;
  while (running) {
    if (!want_demo) {
      // Poll only while the descriptor is live, but ALWAYS drain the
      // queue: in_poll retires the fd the moment it sees end of input,
      // and gating the drain on the fd too would throw away every key
      // still queued behind the one that triggered it. That is the
      // batched case the queue exists for -- a host whose line
      // discipline hands over "wwwdq" in a single read.
      if (in_fd >= 0) in_poll();
      c = in_next();
      if (c >= 0) {
        idle = 0;
        ok = 1;
        if (c == 'w' || c == 'W') walk(1);
        else if (c == 's' || c == 'S') walk(0 - 1);
        else if (c == 'a' || c == 'A') ang = (ang - TURN) & ANG_MASK;
        else if (c == 'd' || c == 'D') ang = (ang + TURN) & ANG_MASK;
        else if (c == 'q' || c == 'Q') running = 0;
        else if (c == 'm' || c == 'M') hud = !hud;
        else if (c == 'p' || c == 'P') demo = !demo;
        else if (c == 27) {
          // A lone ESC quits; ESC [ A/B/C/D is an arrow key. Hosts that
          // embed this terminal eat ESC for their own console toggle,
          // which is why q is documented as THE quit key.
          if (kq_head == kq_tail) running = 0;
          else if (kq[kq_head] == '[') {
            in_next();
            c = in_next();
            if (c == 'A') walk(1);
            else if (c == 'B') walk(0 - 1);
            else if (c == 'C') ang = (ang + TURN) & ANG_MASK;
            else if (c == 'D') ang = (ang - TURN) & ANG_MASK;
          } else running = 0;
        }
        else ok = 0;
        if (ok && demo && c != 'p' && c != 'P') demo = 0;
      } else {
        ++idle;
        if (idle > IDLE_FR) demo = 1;
        // End of input, and nothing left to replay: there is no way
        // left to steer or to quit, so fall back to the screensaver
        // and give the run a bound.
        if (in_eof) { demo = 1; if (!nframes) nframes = 600; }
      }
    }

    if (demo) demo_step();

    render();
    hud_draw();
    frame_build();
    plat_emit(frame);
    fps_tick();

    ++i;
    if (nframes && i >= nframes) running = 0;
    if (frame_ms) plat_usleep(frame_ms * 1000);
  }

  // Put the terminal back as it was found: cursor visible, colour
  // reset, and the prompt on a line of its own below the frame.
  printf("\033[?25h\033[0m\n");
  return 0;
}
