// explore.c -- the IDE's Code pane: the C4 code the file being edited
// compiles to, a function at a time, beside its source. A cut-down Code
// Explorer (docs/c4-explorer.md, track E): the web page's three panes
// (libjs/web/explore.html) become one, since the IDE's text is the
// source pane and the cursor picks the function, and it fits beside the
// text from 800x600 up and below it at 640x480.
//
// The pane compiles the saved file with cc -g -c (c4sc's source map, the
// SMAP trailer of docs/c4-explorer.md 2.2) into /tmp, on a pipe of its
// own, apart from the build; again after every save, and when its -O is
// turned on or off. It reads the object itself: the code, the patches
// that say what each operand refers to, the symbols, and the map's
// files, functions and rows. It shows the function the cursor is in:
// each instruction with its operand named as libjs/c4r.js's insnText
// names it (a function, a global, a string, a label, [bp+n]), the ones
// the cursor's line made highlighted, and a band of each row's colour
// down the left; edit.c draws the same bands beside the source lines.
// A click on an instruction moves the cursor to its line; a double click
// on a call goes to the callee, when it is in this file.
//
// The opcode names are the machine's (src/c4bb/sim/devices.js, OPNAMES);
// libjs/tests/test-desktop.mjs checks the copy here against them.

#include "desktop.h"

// the IDE's text metrics (edit.c), and the pane's parts
enum { XCW = 7, XCH = 15, XFONT = 12, XHDR = XCH + 2, XFOOT = XCH + 4, XNAME = 64 };
// the opcodes it treats apart (OPNAMES' order)
enum { O_LEA = 0, O_JMP = 2, O_JSR = 3, O_BZ = 4, O_BNZ = 5, O_ADJ = 7, O_JSRI = 61, O_JSRS = 62,
       O_LDL = 79, O_PSHL = 81, O_LEAP = 83, O_IMMP = 84, O_STL = 87, O_N = 90 };
// patch types (load-c4r.c), and symbol classes and attributes (c4r.js)
enum { P_CODE = -1, P_DATA = -2, S_FUN = 129, S_GLO = 131, S_EXTERN = 16 };
// the colours, from the IDE's scheme as edit.c passes it (HC_* then SC_*)
enum { XC_TEXT = 0, XC_KW = 1, XC_TYPE = 2, XC_STR = 3, XC_COM = 4, XC_NUM = 5, XC_PRE = 6,
       XC_BG = 7, XC_SEL = 10 };

static char *opnames =
    "LEA ,IMM ,JMP ,JSR ,BZ  ,BNZ ,ENT ,ADJ ,LEV ,LI  ,LC  ,SI  ,SC  ,PSH ,"
    "OR  ,XOR ,AND ,EQ  ,NE  ,LT  ,GT  ,LE  ,GE  ,SHL ,SHR ,ADD ,SUB ,MUL ,DIV ,MOD ,"
    "OPEN,READ,CLOS,PRTF,MALC,FREE,MSET,MCMP,EXIT,"
    "PUTC,PUTS,RALC,MCPY,STRC,ITH ,_OPC,_BLT,_TRP,OPCD,"
    "_JMP,_ADJ,C4CF,C4CY,TIME,SIGH,SIGI,USLP,INFO,OPSL,"
    "C4IV,FLT ,JSRI,JSRS,JMPA,TLEV,DBG ,"
    "CPUI,CPUN,CPUS,CPUH,"
    "CAS ,XCHG,FADD,CWAI,CWAK,IPI ,"
    "LXI ,SXI ,TRAW,"
    "LDL ,LDG ,PSHL,PSHG,LEAP,IMMP,LIP ,ADDL,STL ,POPA,"
    "FPU ,";

// a row's colour: a small palette, cycled by line
static int bands[8] = { 0x5fd7ff, 0xffaf5f, 0x87ff87, 0xff87d7, 0xd7d75f, 0xaf87ff, 0x5fffd7, 0xff8787 };

struct xp {
    // the compile
    char path[128], dir[128], obj[48];
    int pid, out_r, busy, opt, ok, failed;
    char err0[96], err1[96]; int errn;    // the compiler's last two lines, when it failed
    char part[96]; int partn;
    // the object
    char *buf; int bufn, W;
    int *code; int ncode; char *data; int ndata, memsz;
    int *ptype; int *pval;                // per code word: the patch on it, 0 none
    int nsym; int *sid; int *scls; int *sattr; int *sval; char **sname;
    int nfile; char **fpath; int here;    // the map's files; this file's, or -1
    int nfn; int *fstart; int *flen; int *ffile; int *fline; char **fname;
    int nrow; int *roff; int *rfile; int *rline;
    // the function shown, decoded
    int cur; int nins; int *ioff; int *iop; int *iarg; int *ilab; int *irow;
    int lmin, lmax; int *lband;           // its lines in this file: a band's colour, or -1
    int top, sel, lastline, rows;
    char msg[96];
};

static struct xp *X(int *x) { return (struct xp *)x; }
static char xb[320], xo[256];
static char *hexd = "0123456789abcdef";

// ---- reading the object ----------------------------------------------------------------------

static int xp_word(struct xp *x, int p) {
    char *b;
    int v;
    b = x->buf + p;
    v = (b[0] & 255) | ((b[1] & 255) << 8) | ((b[2] & 255) << 16) | ((b[3] & 255) << 24);
    if (b[3] & 128) v = v | (-1 << 31);   // a negative word, at either size
    return v;
}
static int up;                            // the map's reading position
static int xp_u(char *b) {
    int v, s, c;
    v = 0; s = 0;
    c = 128;
    while (c & 128) { c = b[up] & 255; ++up; if (s < 31) v = v + ((c & 127) << s); s = s + 7; }
    return v;
}
static int xp_s(char *b) {
    int v, s, c;
    v = 0; s = 0;
    c = 128;
    while (c & 128) { c = b[up] & 255; ++up; if (s < 31) v = v + ((c & 127) << s); s = s + 7; }
    if ((c & 64) && s < 31) v = v - (1 << s);
    return v;
}
static char *xp_str(char *b, int n) {
    char *s;
    s = (char *)ualloc(n + 1);
    memcpy(s, b + up, n);
    s[n] = 0;
    up = up + n;
    return s;
}

static void xp_drop(struct xp *x) {
    int i;
    if (x->buf) free(x->buf);
    if (x->code) { free((char *)x->code); free((char *)x->ptype); free((char *)x->pval); }
    i = 0; while (i < x->nsym) { free(x->sname[i]); ++i; }
    if (x->sid) { free((char *)x->sid); free((char *)x->scls); free((char *)x->sattr); free((char *)x->sval); free((char *)x->sname); }
    i = 0; while (i < x->nfile) { free(x->fpath[i]); ++i; }
    if (x->fpath) free((char *)x->fpath);
    i = 0; while (i < x->nfn) { free(x->fname[i]); ++i; }
    if (x->fstart) { free((char *)x->fstart); free((char *)x->flen); free((char *)x->ffile); free((char *)x->fline); free((char *)x->fname); }
    if (x->roff) { free((char *)x->roff); free((char *)x->rfile); free((char *)x->rline); }
    if (x->ioff) { free((char *)x->ioff); free((char *)x->iop); free((char *)x->iarg); free((char *)x->ilab); free((char *)x->irow); }
    if (x->lband) free((char *)x->lband);
    x->buf = 0; x->code = 0; x->sid = 0; x->fpath = 0; x->fstart = 0; x->roff = 0; x->ioff = 0; x->lband = 0;
    x->nsym = 0; x->nfile = 0; x->nfn = 0; x->nrow = 0; x->nins = 0; x->ncode = 0;
    x->cur = 0 - 1; x->here = 0 - 1; x->ok = 0;
}

// The SMAP chunk: files, functions and rows (docs/c4-explorer.md 2.2)
static int xp_smap(struct xp *x, char *b, int n) {
    int flags, i, k, start, off, file, line, d;
    up = 0;
    if ((b[0] & 255) != 1) return 0;
    flags = b[1] & 255;
    up = 2;
    x->nfile = xp_u(b);
    x->fpath = (char **)ualloc((x->nfile + 1) * sizeof(int));
    i = 0;
    while (i < x->nfile) {
        k = xp_u(b);
        x->fpath[i] = xp_str(b, k);
        xp_u(b);                              // its size, then its CRC
        if (flags & 1) up = up + 4;
        ++i;
    }
    x->nfn = xp_u(b);
    x->fstart = (int *)ualloc((x->nfn + 1) * sizeof(int)); x->flen = (int *)ualloc((x->nfn + 1) * sizeof(int));
    x->ffile = (int *)ualloc((x->nfn + 1) * sizeof(int)); x->fline = (int *)ualloc((x->nfn + 1) * sizeof(int));
    x->fname = (char **)ualloc((x->nfn + 1) * sizeof(int));
    start = 0; i = 0;
    while (i < x->nfn) {
        start = start + xp_u(b);
        x->fstart[i] = start;
        x->flen[i] = xp_u(b);
        k = xp_u(b);
        x->fname[i] = xp_str(b, k);
        x->ffile[i] = xp_u(b);
        x->fline[i] = xp_u(b);
        ++i;
    }
    x->nrow = xp_u(b);
    x->roff = (int *)ualloc((x->nrow + 1) * sizeof(int)); x->rfile = (int *)ualloc((x->nrow + 1) * sizeof(int));
    x->rline = (int *)ualloc((x->nrow + 1) * sizeof(int));
    off = 0; file = 0; line = 0; i = 0;
    while (i < x->nrow && up < n) {
        d = xp_u(b);
        off = off + (d >> 1);
        if (d & 1) file = xp_u(b);
        line = line + xp_s(b);
        x->roff[i] = off; x->rfile[i] = file; x->rline[i] = line;
        ++i;
    }
    x->nrow = i;
    return 1;
}

// The whole object, as libjs/c4r.js's readC4r reads it; 0 and a message
// if it is not one
static int xp_read(struct xp *x) {
    int st[4], fd, n, p, W, clen, dlen, plen, slen, conl, desl, i, t, a, v, len, tag;
    char *b;
    if (ustat(x->obj, st) < 0 || st[0] != 2) { ui_cpy(x->msg, "No object was written"); return 0; }
    n = st[1];
    x->buf = b = (char *)ualloc(n + 8);
    fd = uopen(x->obj, O_RD);
    if (fd < 0 || uread(fd, b, n) != n) { if (fd >= 0) uclose(fd); ui_cpy(x->msg, "Cannot read the object"); return 0; }
    uclose(fd);
    x->bufn = n;
    if (n < 13 || b[0] != 'C' || b[1] != '4' || b[2] != 'R') { ui_cpy(x->msg, "Not a C4R object"); return 0; }
    W = (b[4] & 255) / 8;
    if (W != 4 && W != 8) { ui_cpy(x->msg, "A word size this cannot read"); return 0; }
    x->W = W;
    p = 5;
    x->memsz = xp_word(x, p); p = p + W;
    if (W == 4) p = p + W;
    p = p + W;                                // the entry
    clen = xp_word(x, p); p = p + W;
    dlen = xp_word(x, p); p = p + W;
    plen = xp_word(x, p); p = p + W;
    slen = xp_word(x, p); p = p + W;
    conl = xp_word(x, p); p = p + W;
    desl = xp_word(x, p); p = p + W;
    if (x->memsz < dlen) x->memsz = dlen;
    if (p + W * (clen + 3 * plen + conl + desl + 5) + dlen > n || b[p] != 'C') { ui_cpy(x->msg, "The object is cut short"); return 0; }
    p = p + W;
    x->ncode = clen;
    x->code = (int *)ualloc((clen + 1) * sizeof(int));
    x->ptype = (int *)ualloc((clen + 1) * sizeof(int));
    x->pval = (int *)ualloc((clen + 1) * sizeof(int));
    i = 0;
    while (i < clen) { x->code[i] = xp_word(x, p); x->ptype[i] = 0; p = p + W; ++i; }
    p = p + W;                                // 'D'
    x->data = b + p; x->ndata = dlen;
    p = p + dlen + W;                         // the data, 'P'
    i = 0;
    while (i < plen) {
        t = xp_word(x, p); a = xp_word(x, p + W); v = xp_word(x, p + 2 * W);
        // only what is patched into the code: a code or data address, or a symbol
        if ((t == P_CODE || t == P_DATA || t > 0) && a >= 0 && a < clen) { x->ptype[a] = t; x->pval[a] = v; }
        p = p + 3 * W;
        ++i;
    }
    p = p + W + conl * W + W + desl * W;      // 'c', 'd'
    if (p + W <= n && b[p] == 'S') {
        p = p + W;
        x->sid = (int *)ualloc((slen + 1) * sizeof(int)); x->scls = (int *)ualloc((slen + 1) * sizeof(int));
        x->sattr = (int *)ualloc((slen + 1) * sizeof(int)); x->sval = (int *)ualloc((slen + 1) * sizeof(int));
        x->sname = (char **)ualloc((slen + 1) * sizeof(int));
        i = 0;
        while (i < slen && p + 4 * W + 1 <= n) {
            x->sid[i] = xp_word(x, p); x->scls[i] = xp_word(x, p + 2 * W); x->sattr[i] = xp_word(x, p + 3 * W);
            p = p + 4 * W;
            len = b[p] & 255; ++p;
            if (p + len + W > n) break;
            up = p; x->sname[i] = xp_str(b, len); p = p + len;
            x->sval[i] = xp_word(x, p); p = p + W;
            ++i;
        }
        x->nsym = i;
    }
    // the extension trailer: 'X', then tag, length and payload, padded, to a 0 tag
    if (p + W <= n && b[p] == 'X') {
        p = p + W;
        while (p + W <= n && (tag = xp_word(x, p)) != 0) {
            len = xp_word(x, p + W); p = p + 2 * W;
            if (len < 0 || p + len > n) break;
            if (b[p - 2 * W] == 'S' && b[p - 2 * W + 1] == 'M' && b[p - 2 * W + 2] == 'A' && b[p - 2 * W + 3] == 'P')
                xp_smap(x, b + p, len);
            p = p + ((len + W - 1) & (0 - W));
        }
    }
    if (!x->nrow) { ui_cpy(x->msg, "The object has no source map"); return 0; }
    // this file, by its path, else by its name
    i = 0;
    while (i < x->nfile && x->here < 0) { if (ui_eq(x->fpath[i], x->path)) x->here = i; ++i; }
    i = 0;
    while (i < x->nfile && x->here < 0) { if (ui_eq(ui_base(x->fpath[i]), ui_base(x->path))) x->here = i; ++i; }
    return 1;
}

// ---- the compile ---------------------------------------------------------------------------

static int xp_seq;

int *xp_new() {
    struct xp *x;
    x = (struct xp *)ualloc(sizeof(struct xp));
    memset((char *)x, 0, sizeof(struct xp));
    x->cur = 0 - 1; x->here = 0 - 1; x->sel = 0 - 1; x->lastline = 0 - 1; x->opt = 1;
    ++xp_seq;
    ui_cpy(x->obj, "/tmp/.ide-");
    ui_num(x->obj + ui_len(x->obj), getpid());
    ui_cat(x->obj, "-");
    ui_num(x->obj + ui_len(x->obj), xp_seq);
    ui_cat(x->obj, ".c4o");
    return (int *)x;
}

static void xp_stop(struct xp *x) {
    if (!x->busy) return;
    ukill(x->pid, SIGTERM);
    uclose(x->out_r);
    uwait(x->pid);
    x->busy = 0;
}

void xp_free(int *xi) {
    struct xp *x;
    x = X(xi);
    xp_stop(x);
    xp_drop(x);
    uunlink(x->obj);
    free((char *)x);
}

// cc -g -c [-O] -I DIR -o OBJ PATH on pipes, as edit.c's :make runs
// b4ke; 0 and a message if it cannot start
int xp_compile(int *xi, char *path, char *dir) {
    struct xp *x;
    char **argv;
    int pin[2], pout[2], s0, s1, s2, k;
    x = X(xi);
    xp_stop(x);
    ui_ncpy(x->path, path, 128);
    ui_ncpy(x->dir, dir, 128);
    if (!ui_ends(path, ".c")) { ui_cpy(x->msg, "Only a C file (.c) has code to show"); return 0; }
    argv = (char **)ualloc(12 * sizeof(int));
    k = 0;
    argv[k] = "cc"; ++k; argv[k] = "-g"; ++k; argv[k] = "-c"; ++k;
    if (x->opt) { argv[k] = "-O"; ++k; }
    argv[k] = "-I"; ++k; argv[k] = x->dir; ++k;
    argv[k] = "-o"; ++k; argv[k] = x->obj; ++k;
    argv[k] = x->path; ++k; argv[k] = 0;
    if (upipe(pin) < 0) { free((char *)argv); ui_cpy(x->msg, "No pipe for the compiler"); return 0; }
    if (upipe(pout) < 0) { uclose(pin[0]); uclose(pin[1]); free((char *)argv); ui_cpy(x->msg, "No pipe for the compiler"); return 0; }
    s0 = udup(STDIN); s1 = udup(STDOUT); s2 = udup(STDERR);
    x->pid = 0 - 1;
    if (s0 >= 0 && s1 >= 0 && s2 >= 0) {
        ucloexec(pin[1], 1); ucloexec(pout[0], 1);
        ucloexec(s0, 1); ucloexec(s1, 1); ucloexec(s2, 1);
        if (udup2(pin[0], STDIN) >= 0 && udup2(pout[1], STDOUT) >= 0 && udup2(pout[1], STDERR) >= 0) {
            x->pid = spawn("/bin/cc", k, argv);
            if (x->pid < 0) x->pid = spawn("c4ix-cc.c4r", k, argv);   // as cc finds its tools
        }
        udup2(s0, STDIN); udup2(s1, STDOUT); udup2(s2, STDERR);
    }
    uclose(pin[0]); uclose(pout[1]); uclose(pin[1]);
    uclose(s0); uclose(s1); uclose(s2);
    free((char *)argv);
    if (x->pid < 0) { uclose(pout[0]); ui_cpy(x->msg, "Cannot start cc"); return 0; }
    x->out_r = pout[0];
    x->busy = 1; x->failed = 0; x->errn = 0; x->partn = 0;
    ui_cpy(x->msg, x->opt ? "Compiling with -g -O..." : "Compiling with -g...");
    return 1;
}

static void xp_errline(struct xp *x) {
    x->part[x->partn] = 0;
    if (x->partn == 0 || (x->part[0] == ';' && x->part[1] == ';')) return;   // c4sc's size line
    if (x->errn == 2) { ui_cpy(x->err0, x->err1); x->errn = 1; }
    ui_cpy(x->errn ? x->err1 : x->err0, x->part);
    ++x->errn;
}

// The compiler's output, and its end: 1 if the pane changed. Never blocks.
int xp_tick(int *xi) {
    struct xp *x;
    char buf[256];
    int n, i, code;
    x = X(xi);
    if (!x->busy) return 0;
    while ((n = uavail(x->out_r)) > 0) {
        if (n > 256) n = 256;
        n = uread(x->out_r, buf, n);
        if (n <= 0) break;
        i = 0;
        while (i < n) {
            if (buf[i] == 10 || x->partn >= 94) { xp_errline(x); x->partn = 0; }
            if (buf[i] != 10 && buf[i] != 13) { x->part[x->partn] = buf[i]; ++x->partn; }
            ++i;
        }
    }
    if (n >= 0) return 0;
    if (x->partn) { xp_errline(x); x->partn = 0; }
    uclose(x->out_r);
    code = uwait(x->pid);
    x->busy = 0;
    xp_drop(x);
    if (code != 0) { x->failed = 1; ui_cpy(x->msg, "It did not compile:"); return 1; }
    if (xp_read(x)) { x->ok = 1; x->msg[0] = 0; }
    x->lastline = 0 - 1;
    return 1;
}

int  xp_busy(int *xi) { return X(xi)->busy; }
int  xp_opt(int *xi) { return X(xi)->opt; }

// ---- the function shown ------------------------------------------------------------------------

static int xp_operand(int op) {
    return op <= O_ADJ || op == O_JSRI || op == O_JSRS || (op >= O_LDL && op <= O_IMMP) || op == O_STL;
}

// the row an offset is in, by binary search; -1 before the first
static int xp_row(struct xp *x, int off) {
    int lo, hi, m, r;
    lo = 0; hi = x->nrow - 1; r = 0 - 1;
    while (lo <= hi) { m = (lo + hi) >> 1; if (x->roff[m] <= off) { r = m; lo = m + 1; } else hi = m - 1; }
    return r;
}
// the map's function an offset is in, or -1
static int xp_fn_at(struct xp *x, int off) {
    int i;
    i = 0;
    while (i < x->nfn) { if (off >= x->fstart[i] && off < x->fstart[i] + x->flen[i]) return i; ++i; }
    return 0 - 1;
}

// decode function F, its labels, and its lines' bands
static void xp_decode(struct xp *x, int f) {
    int s, e, i, n, op, k, t, lo, hi, m, l, r;
    if (x->ioff) { free((char *)x->ioff); free((char *)x->iop); free((char *)x->iarg); free((char *)x->ilab); free((char *)x->irow); }
    if (x->lband) free((char *)x->lband);
    x->ioff = 0; x->lband = 0;
    x->cur = f; x->nins = 0; x->top = 0; x->sel = 0 - 1;
    s = x->fstart[f]; e = s + x->flen[f];
    if (e > x->ncode) e = x->ncode;
    n = e - s + 1;
    x->ioff = (int *)ualloc(n * sizeof(int)); x->iop = (int *)ualloc(n * sizeof(int));
    x->iarg = (int *)ualloc(n * sizeof(int)); x->ilab = (int *)ualloc(n * sizeof(int));
    x->irow = (int *)ualloc(n * sizeof(int));
    i = s; n = 0;
    while (i < e) {
        op = x->code[i];
        x->ioff[n] = i; x->iop[n] = op; x->ilab[n] = 0; x->irow[n] = xp_row(x, i);
        if (xp_operand(op) && i + 1 < e) { x->iarg[n] = x->code[i + 1]; i = i + 2; }
        else { x->iarg[n] = 0; if (xp_operand(op)) x->iop[n] = 0 - 1; i = i + 1; }   // an operand cut off: a word
        ++n;
    }
    x->nins = n;
    // branch targets inside the function are labels, L1 up in order
    k = 0;
    while (k < n) {
        op = x->iop[k];
        if ((op == O_JMP || op == O_BZ || op == O_BNZ) && x->ptype[x->ioff[k] + 1] == P_CODE) {
            t = x->pval[x->ioff[k] + 1];
            lo = 0; hi = n - 1;
            while (lo <= hi) { m = (lo + hi) >> 1; if (x->ioff[m] == t) { x->ilab[m] = 1; lo = hi + 1; } else if (x->ioff[m] < t) lo = m + 1; else hi = m - 1; }
        }
        ++k;
    }
    l = 0; k = 0;
    while (k < n) { if (x->ilab[k]) { ++l; x->ilab[k] = l; } ++k; }
    // which of this file's lines have code here, and their colours
    x->lmin = 0; x->lmax = 0 - 1;
    k = 0;
    while (k < n) {
        r = x->irow[k];
        if (r >= 0 && x->rfile[r] == x->here && x->rline[r] > 0) {
            if (x->lmax < x->lmin) { x->lmin = x->lmax = x->rline[r]; }
            if (x->rline[r] < x->lmin) x->lmin = x->rline[r];
            if (x->rline[r] > x->lmax) x->lmax = x->rline[r];
        }
        ++k;
    }
    if (x->lmax >= x->lmin) {
        x->lband = (int *)ualloc((x->lmax - x->lmin + 1) * sizeof(int));
        k = 0; while (k <= x->lmax - x->lmin) { x->lband[k] = 0 - 1; ++k; }
        k = 0;
        while (k < n) {
            r = x->irow[k];
            if (r >= 0 && x->rfile[r] == x->here && x->rline[r] > 0) x->lband[x->rline[r] - x->lmin] = bands[x->rline[r] % 8];
            ++k;
        }
    }
}

// The function for the cursor's LINE: the one with code from that line,
// else the last one this file starts at or before it
static int xp_fn_for(struct xp *x, int line) {
    int i, f, best;
    i = 0;
    while (i < x->nrow) {
        if (x->rfile[i] == x->here && x->rline[i] == line && (f = xp_fn_at(x, x->roff[i])) >= 0) return f;
        ++i;
    }
    best = 0 - 1;
    i = 0;
    while (i < x->nfn) {
        if (x->ffile[i] == x->here && x->fline[i] <= line && (best < 0 || x->fline[i] >= x->fline[best])) best = i;
        ++i;
    }
    return best;
}

// The band beside source LINE (1-based) of the function shown, or -1
int xp_band(int *xi, int line) {
    struct xp *x;
    x = X(xi);
    if (!x->ok || !x->lband || line < x->lmin || line > x->lmax) return 0 - 1;
    return x->lband[line - x->lmin];
}

// ---- an operand, as libjs/c4r.js's insnText says it --------------------------------------------

static char *xp_symname(struct xp *x, int id) {
    int i;
    i = 0;
    while (i < x->nsym) { if (x->sid[i] == id) return x->sname[i]; ++i; }
    return 0;
}
// the function starting at OFF: the symbols', then the map's; 0 if none
static char *xp_fn_named(struct xp *x, int off) {
    int i;
    i = 0;
    while (i < x->nsym) { if (x->scls[i] == S_FUN && !(x->sattr[i] & S_EXTERN) && x->sval[i] == off) return x->sname[i]; ++i; }
    i = 0;
    while (i < x->nfn) { if (x->fstart[i] == off) return x->fname[i]; ++i; }
    return 0;
}
// the global a data offset is in, or -1
static int xp_global(struct xp *x, int off) {
    int i, g, next;
    g = 0 - 1;
    i = 0;
    while (i < x->nsym) {
        if (x->scls[i] == S_GLO && !(x->sattr[i] & S_EXTERN) && x->sval[i] <= off && (g < 0 || x->sval[i] > x->sval[g])) g = i;
        ++i;
    }
    if (g < 0) return g;
    next = x->memsz;
    i = 0;
    while (i < x->nsym) {
        if (x->scls[i] == S_GLO && !(x->sattr[i] & S_EXTERN) && x->sval[i] > x->sval[g] && x->sval[i] < next) next = x->sval[i];
        ++i;
    }
    if (next <= x->sval[g]) next = x->sval[g] + 1;
    return off < next ? g : 0 - 1;
}
// the printable C string at a data offset: its length (200 at most), or
// -1. The data's end ends one too: c4lc leaves the last literal's NUL to
// the zeroed memory after it.
static int xp_cstr(struct xp *x, int off) {
    int i, c;
    if (off < 0 || off >= x->ndata) return 0 - 1;
    i = off;
    while (i < x->ndata) {
        c = x->data[i] & 255;
        if (c == 0) return i - off;
        if ((c < 32 && c != 9 && c != 10 && c != 13 && c != 27) || c > 126) return 0 - 1;
        ++i;
        if (i - off == 200) return 200;
    }
    return i - off;
}
static void xp_hex(char *d, int v) {
    char t[12];
    int n;
    ui_cat(d, "0x");
    n = 0;
    if (v == 0) { t[0] = '0'; n = 1; }
    while (v != 0 && n < 8) { t[n] = hexd[v & 15]; v = (v >> 4) & 0xfffffff; ++n; }
    d = d + ui_len(d);
    while (n > 0) { --n; *d = t[n]; ++d; }
    *d = 0;
}
// a string literal, quoted and escaped as JSON.stringify does it
static void xp_quote(struct xp *x, char *d, int off, int n, int max) {
    int i, c, k;
    k = ui_len(d);
    d[k] = '"'; ++k;
    i = 0;
    while (i < n && k < max - 8) {
        c = x->data[off + i] & 255;
        if (c == '"' || c == 92) { d[k] = 92; d[k + 1] = c; k = k + 2; }
        else if (c == 10) { d[k] = 92; d[k + 1] = 'n'; k = k + 2; }
        else if (c == 13) { d[k] = 92; d[k + 1] = 'r'; k = k + 2; }
        else if (c == 9) { d[k] = 92; d[k + 1] = 't'; k = k + 2; }
        else if (c == 27) { d[k] = 0; ui_cat(d, "\\u001b"); k = k + 6; }
        else { d[k] = c; ++k; }
        ++i;
    }
    d[k] = '"'; d[k + 1] = 0;
}

// instruction K's mnemonic into M and its operand into D; the operand's
// colour
static int xp_text(struct xp *x, int k, char *m, char *d) {
    int op, a, w, t, v, f, g, n, s, j;
    char *nm;
    op = x->iop[k]; a = x->iarg[k]; w = x->ioff[k] + 1;
    d[0] = 0;
    if (op < 0) { ui_cpy(m, ".word"); ui_num(d, x->code[x->ioff[k]]); return XC_NUM; }
    if (op < O_N) { memcpy(m, opnames + op * 5, 4); m[4] = 0; j = 3; while (j > 0 && m[j] == ' ') { m[j] = 0; --j; } }
    else { ui_cpy(m, "?"); ui_num(m + 1, op); }
    if (!xp_operand(op)) return XC_TEXT;
    t = x->ptype[w]; v = x->pval[w];
    if (t == P_CODE) {
        f = xp_fn_at(x, v);
        if ((op == O_JMP || op == O_BZ || op == O_BNZ) && f == x->cur) {
            j = 0;
            while (j < x->nins) { if (x->ioff[j] == v && x->ilab[j]) { ui_cpy(d, "L"); ui_num(d + 1, x->ilab[j]); return XC_PRE; } ++j; }
            xp_hex(d, v); return XC_PRE;
        }
        if ((nm = xp_fn_named(x, v))) { if (op != O_JSR) ui_cpy(d, "&"); ui_cat(d, nm); return XC_TYPE; }
        if (f >= 0) { ui_cpy(d, x->fname[f]); ui_cat(d, "+"); ui_num(d + ui_len(d), v - x->fstart[f]); return XC_TYPE; }
        xp_hex(d, v); return XC_NUM;
    }
    if (t == P_DATA) {
        g = xp_global(x, v);
        n = xp_cstr(x, v);
        // a unit's literals follow its globals with no symbol of their own
        if (g >= 0 && n >= 2 && v - x->sval[g] >= x->W && v > 0 && x->data[v - 1] == 0) g = 0 - 1;
        if (g >= 0) {
            ui_cpy(d, op == O_JSRI ? "*" : "&");
            ui_cat(d, x->sname[g]);
            if (v != x->sval[g]) { ui_cat(d, "+"); ui_num(d + ui_len(d), v - x->sval[g]); }
            return XC_TYPE;
        }
        if (n >= 0) {
            if (n > 200) n = 200;
            xp_quote(x, d, v, n, 240);
            if (n == 200) ui_cat(d, "...");
            return XC_STR;
        }
        ui_cpy(d, "data+"); ui_num(d + 5, v); return XC_NUM;
    }
    if (t > 0) {
        nm = xp_symname(x, t);
        if (nm) ui_cpy(d, nm); else { ui_cpy(d, "symbol "); ui_num(d + 7, t); }
        return XC_TYPE;
    }
    if (op == O_LEA || op == O_LDL || op == O_STL || op == O_PSHL || op == O_LEAP || op == O_JSRS) {
        ui_cpy(d, "[bp");
        if (a >= 0) ui_cat(d, "+");
        ui_num(d + ui_len(d), a);
        ui_cat(d, "]");
        return XC_NUM;
    }
    s = 0;
    if (a < 0) { d[0] = '-'; s = 1; a = 0 - a; }
    ui_num(d + s, a);
    return XC_NUM;
}

// ---- drawing ---------------------------------------------------------------------------------

// The cursor on LINE (1-based) picks the function, and the view finds
// the line's instructions. edit.c calls this before it draws the text,
// whose gutter shows the function's bands.
void xp_cursor(int *xi, int line) {
    struct xp *x;
    int f, k, r, hit, rows;
    x = X(xi);
    if (!x->ok || line == x->lastline) return;
    rows = x->rows > 0 ? x->rows : 8;
    f = xp_fn_for(x, line);
    if (f >= 0 && f != x->cur) xp_decode(x, f);
    if (f < 0) { x->cur = 0 - 1; x->nins = 0; }
    k = 0; hit = 0 - 1;
    while (k < x->nins && hit < 0) { r = x->irow[k]; if (r >= 0 && x->rfile[r] == x->here && x->rline[r] == line) hit = k; ++k; }
    if (hit >= 0 && (hit < x->top || hit >= x->top + rows)) x->top = hit - rows / 3;
    x->lastline = line;
}

static int xp_list_rows(int ch) { int r; r = (ch - XHDR - XFOOT - 4) / XCH; return r < 1 ? 1 : r; }

// The pane at OX, OY, CW by CH, for the cursor on LINE (1-based) of a
// text MODIFIED since it was saved; COLS the IDE's scheme
void xp_draw(int *xi, int ox, int oy, int cw, int ch, int *cols, int line, int modified) {
    struct xp *x;
    char m[8];
    int rows, y, k, r, c, i, sx, n;
    x = X(xi);
    rows = xp_list_rows(ch);
    x->rows = rows;
    xp_cursor(xi, line);
    if (x->top > x->nins - rows) x->top = x->nins - rows;
    if (x->top < 0) x->top = 0;

    // the header: the function, and -O
    d_rect(ox, oy, cw, XHDR, C_FACE);
    d_bevel(ox, oy, cw, XHDR, B_RAISED);
    ui_cpy(xb, "Code");
    if (x->ok && x->cur >= 0) {
        ui_cat(xb, ": "); ui_cat(xb, x->fname[x->cur]);
        ui_cat(xb, " ("); ui_num(xb + ui_len(xb), x->flen[x->cur]); ui_cat(xb, " words)");
    }
    d_clip(ox, oy, cw - 44, XHDR);
    d_text(ox + 6, oy + 2, C_DARK, F_SANS, xb);
    d_clip_end();
    ui_button(ox + cw - 40, oy + 1, 36, XHDR - 2, "-O", x->opt, 0);

    // the listing
    y = oy + XHDR;
    d_rect(ox, y, cw - 17, rows * XCH + 4, cols[XC_BG]);
    sx = ox + 6;
    k = x->top; i = 0;
    while (x->ok && k < x->nins && i < rows) {
        r = x->irow[k];
        if (r >= 0 && x->rfile[r] == x->here && x->rline[r] == line) d_rect(ox, y + 2 + i * XCH, cw - 17, XCH, cols[XC_SEL]);
        if (k == x->sel) d_recto(ox, y + 2 + i * XCH, cw - 17, XCH, cols[XC_KW]);
        if (r >= 0 && x->rline[r] > 0) d_rect(ox + 1, y + 2 + i * XCH, 3, XCH, bands[x->rline[r] % 8]);
        // the offset (in hex, right in four), a label, the mnemonic and its operand
        xb[0] = 0;
        xp_hex(xb, x->ioff[k]);
        n = ui_len(xb + 2);
        d_textn(sx + (n < 4 ? 4 - n : 0) * XCW, y + 2 + i * XCH, cols[XC_COM], XFONT, F_MONO, XCW, xb + 2, n);
        n = 10;
        if (x->ilab[k]) {
            ui_cpy(xb, "L"); ui_num(xb + 1, x->ilab[k]); ui_cat(xb, ":");
            d_textn(sx + 5 * XCW, y + 2 + i * XCH, cols[XC_PRE], XFONT, F_MONO, XCW, xb, ui_len(xb));
            if (ui_len(xb) + 6 > n) n = ui_len(xb) + 6;
        }
        c = xp_text(x, k, m, xb);
        d_textn(sx + n * XCW, y + 2 + i * XCH, cols[XC_KW], XFONT, F_MONO, XCW, m, ui_len(m));
        if (xb[0]) {
            d_clip(ox, y, cw - 17, rows * XCH + 4);
            d_textn(sx + (n + 5) * XCW, y + 2 + i * XCH, cols[c], XFONT, F_MONO, XCW, xb, ui_len(xb));
            d_clip_end();
        }
        ++k; ++i;
    }
    if (x->ok && x->cur < 0) d_text(sx, y + 4, cols[XC_COM], F_SANS, "No code from here: the cursor is before every function");
    ui_scrollbar(ox + cw - 17, y, rows * XCH + 4, x->nins, rows, x->top);

    // the footer: what is going on, or where the instruction picked came from
    y = oy + ch - XFOOT;
    d_rect(ox, y, cw, XFOOT, C_FACE);
    d_bevel(ox, y, cw, XFOOT, B_SUNKEN);
    xb[0] = 0;
    if (x->busy || !x->ok) ui_cpy(xb, x->msg[0] ? x->msg : "View > Code compiles the file when it is saved");
    else if (x->failed) ui_cpy(xb, x->msg);
    else if (x->sel >= 0 && x->sel < x->nins && (r = x->irow[x->sel]) >= 0) {
        ui_cpy(xb, ui_base(x->fpath[x->rfile[r]]));
        ui_cat(xb, ":"); ui_num(xb + ui_len(xb), x->rline[r]);
        ui_cat(xb, "  ");
        c = xp_text(x, x->sel, m, xo);
        ui_cat(xb, m); ui_cat(xb, " "); ui_ncat(xb, xo, 300);
    }
    else if (modified) ui_cpy(xb, "Changed since it was compiled: save to see the new code");
    else if (x->here < 0) ui_cpy(xb, "The map does not name this file");
    else ui_cpy(xb, x->opt ? "Compiled with -g -O" : "Compiled with -g");
    d_clip(ox, y, cw, XFOOT);
    d_text(ox + 4, y + 3, C_DARK, F_SANS, xb);
    d_clip_end();
    // a failed compile's last words, over the listing
    if (x->failed && !x->busy) {
        i = 0;
        if (x->errn > 0) d_textn(sx, oy + XHDR + 4, cols[XC_STR], XFONT, F_MONO, XCW, x->err0, ui_len(x->err0));
        if (x->errn > 1) d_textn(sx, oy + XHDR + 4 + XCH, cols[XC_STR], XFONT, F_MONO, XCW, x->err1, ui_len(x->err1));
    }
}

// A mouse event at X, Y in the pane: the source line to go to (1-based),
// 0 for none, -1 when -O was turned on or off (and it recompiles), -2
// when its bar was dragged
int xp_mouse(int *xi, int ev, int px, int py, int cw, int ch) {
    struct xp *x;
    int rows, k, t, r, f, v;
    char *nm;
    x = X(xi);
    rows = x->rows;
    if (ev == E_WHEEL) { x->top = x->top + py * 3; return 0; }
    if (ev == E_MOVE) { t = ui_scroll_drag(cw - 17, XHDR, py); if (t < 0) return 0; x->top = t; return 0 - 2; }
    if (ev != E_DOWN && ev != E_DBL) return 0;
    if (py < XHDR) {
        if (ev == E_DOWN && ui_hit(px, py, cw - 40, 1, 36, XHDR - 2)) {
            x->opt = !x->opt;
            if (x->path[0]) xp_compile(xi, x->path, x->dir);
            return 0 - 1;
        }
        return 0;
    }
    if (px >= cw - 17) {
        t = ui_scroll_click(px, py, cw - 17, XHDR, rows * XCH + 4, x->nins, rows, x->top);
        if (t >= 0) x->top = t;
        return 0;
    }
    if (py >= ch - XFOOT || !x->ok) return 0;
    k = x->top + (py - XHDR - 2) / XCH;
    if (k < 0 || k >= x->nins) return 0;
    x->sel = k;
    // a double click on a call, or on a function's address: its source
    if (ev == E_DBL && x->iop[k] >= 0 && xp_operand(x->iop[k]) && x->ptype[x->ioff[k] + 1] == P_CODE) {
        v = x->pval[x->ioff[k] + 1];
        f = xp_fn_at(x, v);
        if (f >= 0 && f != x->cur && x->fstart[f] == v) {
            if (x->ffile[f] == x->here && x->fline[f] > 0) { x->lastline = 0 - 1; return x->fline[f]; }
            nm = x->fpath[x->ffile[f]];
            ui_cpy(x->msg, x->fname[f]); ui_cat(x->msg, " is in "); ui_cat(x->msg, nm);
            return 0;
        }
    }
    r = x->irow[k];
    if (r >= 0 && x->rfile[r] == x->here && x->rline[r] > 0) { x->lastline = x->rline[r]; return x->rline[r]; }
    return 0;
}
