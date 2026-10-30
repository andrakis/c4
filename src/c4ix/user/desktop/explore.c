// explore.c -- the IDE's Code tab: the C4 code a C file compiles to, or a
// compiled file's own, a function at a time, with the C source it came
// from between the instructions. A cut-down Code Explorer
// (docs/c4-explorer.md, track E), after the web page's
// (libjs/web/explore.html): the functions down the left, found by typing
// part of a name, and the code of the one picked beside them, each line
// of source the map names shown above the instructions it made. It takes
// the editor's place in the window, as a tab beside the text, since at
// 800x600 and below there is no room for both side by side.
//
// Two ways in. For a .c file being edited, it compiles the saved file
// with cc -g -c (c4sc's source map, the SMAP trailer of
// docs/c4-explorer.md 2.2) into /tmp, on a pipe of its own, apart from
// the build; again after every save, and when its -O is turned on or
// off; the editor's cursor picks the function. A compiled file -- a
// program, an object or a library: anything with C4R's header, and every
// one C4IX ships carries a map -- opens in it as it is (edit.c, when it
// opens one), its sources found by the map's paths: as they are, under
// /usr (the root archive's /usr is the repository, so its src/ is
// /usr/src), or beside the file.
//
// It reads the object itself: the code, the patches that say what each
// operand refers to, the symbols, and the map's files, functions and
// rows. Each operand is named as libjs/c4r.js's insnText names it (a
// function, a global, a string, a label, [bp+n]). A click on a row lights
// every row of its line; a double click (or Enter) on a call goes to the
// callee, and Back (Alt+Left, or Backspace) comes back; a double click
// on any other row opens its line in the editor. Without a map the
// functions come from the symbols, with no source to show.
//
// The opcode names are the machine's (src/c4bb/sim/devices.js, OPNAMES);
// libjs/tests/test-desktop.mjs checks the copy here against them.

#include "desktop.h"

// the IDE's text metrics (edit.c), and the tab's parts: its headers (a
// field's height), its footer, how long a name is kept, the source lines
// with no code shown between two that have, and how far back Back goes
enum { XCW = 7, XCH = 15, XFONT = 12, XHDR = 22, XFOOT = XCH + 4, XGAP = 3, XHIST = 16, XFILT = 30 };
// the opcodes it treats apart (OPNAMES' order)
enum { O_LEA = 0, O_JMP = 2, O_JSR = 3, O_BZ = 4, O_BNZ = 5, O_ADJ = 7, O_JSRI = 61, O_JSRS = 62,
       O_LDL = 79, O_PSHL = 81, O_LEAP = 83, O_IMMP = 84, O_STL = 87, O_N = 90 };
// patch types (load-c4r.c), and symbol classes and attributes (c4r.js)
enum { P_CODE = -1, P_DATA = -2, S_FUN = 129, S_GLO = 131, S_EXTERN = 16 };
// the colours, from the IDE's scheme as edit.c passes it (HC_* then SC_*)
enum { XC_TEXT = 0, XC_KW = 1, XC_TYPE = 2, XC_STR = 3, XC_COM = 4, XC_NUM = 5, XC_PRE = 6,
       XC_BG = 7, XC_GUT = 8, XC_SEL = 10 };

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

// a line's colour: a small palette, cycled by line
static int bands[8] = { 0x5fd7ff, 0xffaf5f, 0x87ff87, 0xff87d7, 0xd7d75f, 0xaf87ff, 0x5fffd7, 0xff8787 };

struct xp {
    // what is shown: a compile of PATH (a .c file), or the compiled file PATH itself
    char path[128], dir[128], obj[48];
    int objmode;
    int pid, out_r, busy, opt, ok, failed;
    char err0[96], err1[96]; int errn;    // the compiler's last two lines, when it failed
    char part[96]; int partn;
    // the object
    char *buf; int bufn, W;
    int *code; int ncode; char *data; int ndata, memsz;
    int *ptype; int *pval;                // per code word: the patch on it, 0 none
    int nsym; int *sid; int *scls; int *sattr; int *sval; char **sname;
    int nfile; char **fpath; int *fsize; int here;   // the map's files, their sizes compiled; the file edited, or -1
    // each file's source, once looked for: its text, where its lines start and how many,
    // and 1 found, 2 found but not the size it was compiled at, -1 not found
    char **stext; int **sline; int *snl; int *sst; char **spath;
    int nfn; int *fstart; int *flen; int *ffile; int *fline; char **fname;
    int nrow; int *roff; int *rfile; int *rline;
    // the function list: every function in its order, and the ones the filter keeps
    int *lord; int *lvis; int nvis, ltop, lrows, byname, lfollow;
    char filt[XFILT + 2]; int filtc;
    // the function shown, decoded
    int cur; int nins; int *ioff; int *iop; int *iarg; int *ilab; int *irow;
    int lmin, lmax; int *lband;           // its lines in the file edited: a band's colour, or -1
    // the listing: row i is instruction dins[i], or (dins[i] < 0) line dline[i] of file dfile[i]
    int *dins; int *dfile; int *dline; int nd;
    int top, sel, rows, lastline, hfile, hline;   // the view; the row picked; the line lit
    int hist[XHIST]; int nhist;
    char gopath[128]; int goline;         // a line asked for in the editor
    char msg[96], note[160];
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
static int *xp_ints(int n) { return (int *)ualloc((n + 1) * sizeof(int)); }
static int *xp_zeros(int n) { int *p; p = xp_ints(n); memset((char *)p, 0, (n + 1) * sizeof(int)); return p; }

static void xp_unshow(struct xp *x) {
    if (x->ioff) { free((char *)x->ioff); free((char *)x->iop); free((char *)x->iarg); free((char *)x->ilab); free((char *)x->irow); }
    if (x->lband) free((char *)x->lband);
    if (x->dins) { free((char *)x->dins); free((char *)x->dfile); free((char *)x->dline); }
    x->ioff = 0; x->lband = 0; x->dins = 0; x->nins = 0; x->nd = 0;
}

static void xp_drop(struct xp *x) {
    int i;
    xp_unshow(x);
    if (x->buf) free(x->buf);
    if (x->code) { free((char *)x->code); free((char *)x->ptype); free((char *)x->pval); }
    i = 0; while (i < x->nsym) { free(x->sname[i]); ++i; }
    if (x->sid) { free((char *)x->sid); free((char *)x->scls); free((char *)x->sattr); free((char *)x->sval); free((char *)x->sname); }
    i = 0;
    while (i < x->nfile) {
        free(x->fpath[i]);
        if (x->stext && x->stext[i]) { free(x->stext[i]); free((char *)x->sline[i]); free(x->spath[i]); }
        ++i;
    }
    if (x->fpath) { free((char *)x->fpath); free((char *)x->fsize); }
    if (x->stext) { free((char *)x->stext); free((char *)x->sline); free((char *)x->snl); free((char *)x->sst); free((char *)x->spath); }
    i = 0; while (i < x->nfn) { free(x->fname[i]); ++i; }
    if (x->fstart) { free((char *)x->fstart); free((char *)x->flen); free((char *)x->ffile); free((char *)x->fline); free((char *)x->fname); }
    if (x->roff) { free((char *)x->roff); free((char *)x->rfile); free((char *)x->rline); }
    if (x->lord) { free((char *)x->lord); free((char *)x->lvis); }
    x->buf = 0; x->code = 0; x->sid = 0; x->fpath = 0; x->stext = 0; x->fstart = 0; x->roff = 0; x->lord = 0;
    x->nsym = 0; x->nfile = 0; x->nfn = 0; x->nrow = 0; x->ncode = 0; x->nvis = 0; x->nhist = 0;
    x->cur = 0 - 1; x->here = 0 - 1; x->ok = 0; x->sel = 0 - 1; x->hfile = 0 - 1; x->hline = 0;
}

// The SMAP chunk: files, functions and rows (docs/c4-explorer.md 2.2)
static int xp_smap(struct xp *x, char *b, int n) {
    int flags, i, k, start, off, file, line, d;
    up = 0;
    if ((b[0] & 255) != 1) return 0;
    flags = b[1] & 255;
    up = 2;
    x->nfile = xp_u(b);
    x->fpath = (char **)xp_ints(x->nfile);
    x->fsize = xp_ints(x->nfile);
    i = 0;
    while (i < x->nfile) {
        k = xp_u(b);
        x->fpath[i] = xp_str(b, k);
        x->fsize[i] = xp_u(b);                // its size, then its CRC
        if (flags & 1) up = up + 4;
        ++i;
    }
    x->nfn = xp_u(b);
    x->fstart = xp_ints(x->nfn); x->flen = xp_ints(x->nfn);
    x->ffile = xp_ints(x->nfn); x->fline = xp_ints(x->nfn);
    x->fname = (char **)xp_ints(x->nfn);
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
    x->roff = xp_ints(x->nrow); x->rfile = xp_ints(x->nrow); x->rline = xp_ints(x->nrow);
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

// With no map, the functions are the symbols' own, in address order, each
// up to the next
static void xp_symfuncs(struct xp *x) {
    int i, n, gap, j, t, *v;
    n = 0; i = 0;
    while (i < x->nsym) { if (x->scls[i] == S_FUN && !(x->sattr[i] & S_EXTERN) && x->sval[i] >= 0 && x->sval[i] < x->ncode) ++n; ++i; }
    if (!n) return;
    v = xp_ints(n);
    n = 0; i = 0;
    while (i < x->nsym) { if (x->scls[i] == S_FUN && !(x->sattr[i] & S_EXTERN) && x->sval[i] >= 0 && x->sval[i] < x->ncode) { v[n] = i; ++n; } ++i; }
    gap = n / 2;
    while (gap > 0) {
        i = gap;
        while (i < n) {
            t = v[i]; j = i;
            while (j >= gap && x->sval[v[j - gap]] > x->sval[t]) { v[j] = v[j - gap]; j = j - gap; }
            v[j] = t;
            ++i;
        }
        gap = gap / 2;
    }
    x->nfn = n;
    x->fstart = xp_ints(n); x->flen = xp_ints(n); x->ffile = xp_ints(n); x->fline = xp_ints(n);
    x->fname = (char **)xp_ints(n);
    i = 0;
    while (i < n) {
        x->fstart[i] = x->sval[v[i]];
        x->flen[i] = (i + 1 < n ? x->sval[v[i + 1]] : x->ncode) - x->fstart[i];
        x->ffile[i] = 0 - 1; x->fline[i] = 0;
        x->fname[i] = (char *)ualloc(ui_len(x->sname[v[i]]) + 1);
        ui_cpy(x->fname[i], x->sname[v[i]]);
        ++i;
    }
    free((char *)v);
}

// The whole object at PATH, as libjs/c4r.js's readC4r reads it; 0 and a
// message if it is not one
static int xp_read(struct xp *x, char *path) {
    int st[4], fd, n, p, W, clen, dlen, plen, slen, conl, desl, i, t, a, v, len, tag;
    char *b;
    if (ustat(path, st) < 0 || st[0] != 2) { ui_cpy(x->msg, x->objmode ? "Cannot find that file" : "No object was written"); return 0; }
    n = st[1];
    x->buf = b = (char *)ualloc(n + 8);
    fd = uopen(path, O_RD);
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
    x->code = xp_ints(clen); x->ptype = xp_ints(clen); x->pval = xp_ints(clen);
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
        x->sid = xp_ints(slen); x->scls = xp_ints(slen); x->sattr = xp_ints(slen); x->sval = xp_ints(slen);
        x->sname = (char **)xp_ints(slen);
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
    if (!x->nfn) xp_symfuncs(x);
    if (!x->nfn) { ui_cpy(x->msg, "No functions: the object has no map and no symbols"); return 0; }
    // the file edited, by its path, else by its name
    i = 0;
    while (!x->objmode && i < x->nfile && x->here < 0) { if (ui_eq(x->fpath[i], x->path)) x->here = i; ++i; }
    i = 0;
    while (!x->objmode && i < x->nfile && x->here < 0) { if (ui_eq(ui_base(x->fpath[i]), ui_base(x->path))) x->here = i; ++i; }
    return 1;
}

// ---- the sources ------------------------------------------------------------------------------

static int xp_isfile(char *p) { int st[4]; return ustat(p, st) == 0 && st[0] == 2; }

// Where map file I's source is on this disk, into OUT: the file edited;
// the map's path as it is, or under /usr; or beside what is shown. 0 if
// nowhere (or the compiler's own, as "<c4lc softfp>").
static int xp_where(struct xp *x, int i, char *out) {
    char *p;
    p = x->fpath[i];
    if (!p[0] || p[0] == '<') return 0;
    if (i == x->here) { ui_ncpy(out, x->path, 128); return 1; }
    if (p[0] == '/') { if (ui_ncpy(out, p, 128) && xp_isfile(out)) return 1; }
    else {
        ui_cpy(out, "/usr/");
        if (ui_ncat(out, p, 128) && xp_isfile(out)) return 1;
        if (x->dir[0] && ui_join(out, x->dir, p, 128) && xp_isfile(out)) return 1;
    }
    if (x->dir[0] && ui_join(out, x->dir, ui_base(p), 128) && xp_isfile(out)) return 1;
    return 0;
}

// map file I's source, read the first time it is asked for
static void xp_source(struct xp *x, int i) {
    char p[128];
    int st[4], fd, n, k, nl;
    char *t;
    int *ls;
    if (i < 0 || i >= x->nfile || !x->stext || x->sst[i]) return;
    x->sst[i] = 0 - 1;
    if (!xp_where(x, i, p) || ustat(p, st) < 0 || st[1] > 1048576) return;
    n = st[1];
    t = (char *)ualloc(n + 1);
    fd = uopen(p, O_RD);
    if (fd < 0) { free(t); return; }
    k = n > 0 ? uread(fd, t, n) : 0;
    uclose(fd);
    if (k != n) { free(t); return; }
    t[n] = 0;
    nl = 1; k = 0;
    while (k < n) { if (t[k] == 10) ++nl; ++k; }
    ls = xp_ints(nl + 1);
    ls[0] = 0; nl = 1; k = 0;
    while (k < n) { if (t[k] == 10) { ls[nl] = k + 1; ++nl; } ++k; }
    ls[nl] = n + 1;                           // so the last line ends at the text's end
    x->stext[i] = t; x->sline[i] = ls; x->snl[i] = nl;
    x->spath[i] = (char *)ualloc(ui_len(p) + 1);
    ui_cpy(x->spath[i], p);
    x->sst[i] = n == x->fsize[i] ? 1 : 2;
}

// line L (1-based) of map file I: its text, and its length in *PN; 0 if
// there is none
static char *xp_line(struct xp *x, int i, int l, int *pn) {
    int a, b;
    xp_source(x, i);
    if (i < 0 || i >= x->nfile || !x->stext || x->sst[i] <= 0 || l < 1 || l > x->snl[i]) return 0;
    a = x->sline[i][l - 1]; b = x->sline[i][l] - 1;
    if (b > a && x->stext[i][b - 1] == 13) --b;
    *pn = b - a;
    return x->stext[i] + a;
}
static int xp_blank(char *s, int n) {
    int i;
    i = 0;
    while (i < n) { if (s[i] != ' ' && s[i] != 9) return 0; ++i; }
    return 1;
}
// N characters of S into D (MAX at most), its tabs spaced out to eights
static int xp_untab(char *s, int n, char *d, int max) {
    int i, k;
    i = 0; k = 0;
    while (i < n && k < max - 1) {
        if (s[i] == 9) { d[k] = ' '; ++k; while ((k & 7) && k < max - 1) { d[k] = ' '; ++k; } }
        else { d[k] = s[i]; ++k; }
        ++i;
    }
    d[k] = 0;
    return k;
}

// ---- the compile ---------------------------------------------------------------------------

static int xp_seq;

int *xp_new() {
    struct xp *x;
    x = (struct xp *)ualloc(sizeof(struct xp));
    memset((char *)x, 0, sizeof(struct xp));
    x->cur = 0 - 1; x->here = 0 - 1; x->sel = 0 - 1; x->lastline = 0 - 1; x->hfile = 0 - 1; x->opt = 1;
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
    if (x->objmode) { xp_drop(x); x->objmode = 0; x->byname = 0; x->filt[0] = 0; x->filtc = 0; }
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

static void xp_ready(struct xp *x, char *keep);

// The compiler's output, and its end: 1 if the tab changed. Never blocks.
int xp_tick(int *xi) {
    struct xp *x;
    char buf[256], keep[64];
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
    // the function shown is shown again, in the new code, if it is still there
    keep[0] = 0;
    if (x->ok && x->cur >= 0) ui_ncpy(keep, x->fname[x->cur], 64);
    xp_drop(x);
    if (code != 0) { x->failed = 1; ui_cpy(x->msg, "It did not compile:"); return 1; }
    if (xp_read(x, x->obj)) { x->ok = 1; x->msg[0] = 0; xp_ready(x, keep); }
    return 1;
}

int  xp_busy(int *xi) { return X(xi)->busy; }
int  xp_opt(int *xi) { return X(xi)->opt; }

// ---- the function list ----------------------------------------------------------------------

static int xp_lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int xp_cmp(char *a, char *b) {
    while (*a && *a == *b) { ++a; ++b; }
    return (*a & 255) - (*b & 255);
}
static int xp_before(struct xp *x, int a, int b) {
    int c;
    c = xp_cmp(x->fname[a], x->fname[b]);
    return c < 0 || (c == 0 && x->fstart[a] < x->fstart[b]);
}
// P in S, in either case
static int xp_has(char *s, char *p) {
    int i, j;
    if (!p[0]) return 1;
    i = 0;
    while (s[i]) {
        j = 0;
        while (p[j] && s[i + j] && xp_lower(s[i + j]) == xp_lower(p[j])) ++j;
        if (!p[j]) return 1;
        ++i;
    }
    return 0;
}
static void xp_filter(struct xp *x) {
    int i, n;
    n = 0; i = 0;
    while (i < x->nfn) { if (xp_has(x->fname[x->lord[i]], x->filt)) { x->lvis[n] = x->lord[i]; ++n; } ++i; }
    x->nvis = n; x->ltop = 0; x->lfollow = 1;
}
// in address order (the map's, and xp_symfuncs'), or by name
static void xp_order(struct xp *x) {
    int i, j, t, gap;
    i = 0; while (i < x->nfn) { x->lord[i] = i; ++i; }
    gap = x->byname ? x->nfn / 2 : 0;
    while (gap > 0) {
        i = gap;
        while (i < x->nfn) {
            t = x->lord[i]; j = i;
            while (j >= gap && xp_before(x, t, x->lord[j - gap])) { x->lord[j] = x->lord[j - gap]; j = j - gap; }
            x->lord[j] = t;
            ++i;
        }
        gap = gap / 2;
    }
    xp_filter(x);
}
static int xp_named(struct xp *x, char *nm) {
    int i;
    i = 0;
    while (i < x->nfn) { if (ui_eq(x->fname[i], nm)) return i; ++i; }
    return 0 - 1;
}
static int xp_listed(struct xp *x, int f) {
    int k;
    k = 0;
    while (k < x->nvis) { if (x->lvis[k] == f) return k; ++k; }
    return 0 - 1;
}

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
// the function an offset is in, by binary search (they are in address
// order, and do not overlap); -1 if none
static int xp_fn_at(struct xp *x, int off) {
    int lo, hi, m;
    lo = 0; hi = x->nfn - 1;
    while (lo <= hi) {
        m = (lo + hi) >> 1;
        if (off < x->fstart[m]) hi = m - 1;
        else if (off >= x->fstart[m] + x->flen[m]) lo = m + 1;
        else return m;
    }
    return 0 - 1;
}

// decode function F, its labels, and its lines' bands
static void xp_decode(struct xp *x, int f) {
    int s, e, i, n, op, k, t, lo, hi, m, l, r;
    xp_unshow(x);
    x->cur = f; x->top = 0; x->sel = 0 - 1;
    s = x->fstart[f]; e = s + x->flen[f];
    if (e > x->ncode) e = x->ncode;
    n = e - s + 1;
    x->ioff = xp_ints(n); x->iop = xp_ints(n); x->iarg = xp_ints(n); x->ilab = xp_ints(n); x->irow = xp_ints(n);
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
    // which of the edited file's lines have code here, and their colours
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
        x->lband = xp_ints(x->lmax - x->lmin + 1);
        k = 0; while (k <= x->lmax - x->lmin) { x->lband[k] = 0 - 1; ++k; }
        k = 0;
        while (k < n) {
            r = x->irow[k];
            if (r >= 0 && x->rfile[r] == x->here && x->rline[r] > 0) x->lband[x->rline[r] - x->lmin] = bands[x->rline[r] % 8];
            ++k;
        }
    }
}

// The listing's rows: each instruction, after the source line it came
// from when that is a new one. The few lines between one line and the
// next with no code of their own -- a {, a declaration, a comment -- are
// shown too, as they read in the file; blank ones are not.
static void xp_layout(struct xp *x) {
    int f, k, r, l, lf, ll, a, n, len;
    char *t;
    f = x->cur;
    n = x->nins * (XGAP + 2) + 8;
    x->dins = xp_ints(n); x->dfile = xp_ints(n); x->dline = xp_ints(n);
    n = 0;
    lf = x->ffile[f]; ll = x->fline[f] - 1;   // so the function's own first line is shown
    k = 0;
    while (k < x->nins) {
        r = x->irow[k];
        if (r >= 0 && x->rline[r] > 0 && (x->rfile[r] != lf || x->rline[r] != ll)) {
            l = x->rline[r];
            a = l;
            if (x->rfile[r] == lf && l > ll && l - ll <= XGAP + 1) a = ll + 1;
            while (a <= l) {
                t = xp_line(x, x->rfile[r], a, &len);
                if (a == l || (t && !xp_blank(t, len))) { x->dins[n] = 0 - 1; x->dfile[n] = x->rfile[r]; x->dline[n] = a; ++n; }
                ++a;
            }
            lf = x->rfile[r]; ll = l;
        }
        x->dins[n] = k;
        x->dfile[n] = r >= 0 ? x->rfile[r] : 0 - 1;
        x->dline[n] = r >= 0 ? x->rline[r] : 0;
        ++n;
        ++k;
    }
    x->nd = n;
}

// Show function F; PUSH keeps the one shown before for Back
static void xp_show(struct xp *x, int f, int push) {
    int i;
    if (f < 0 || f >= x->nfn) return;
    if (push && x->cur >= 0 && x->cur != f) {
        if (x->nhist == XHIST) { i = 1; while (i < XHIST) { x->hist[i - 1] = x->hist[i]; ++i; } --x->nhist; }
        x->hist[x->nhist] = x->cur; ++x->nhist;
    }
    if (f != x->cur || !x->dins) { xp_decode(x, f); xp_layout(x); }
    x->lfollow = 1;
}
static void xp_back(struct xp *x) {
    if (!x->nhist) return;
    --x->nhist;
    xp_show(x, x->hist[x->nhist], 0);
}

// A new object read: its list, and the function KEEP (by name) if it is
// still there; a compiled file starts at main, or the first function of
// its own (not one of the compiler's, as "<udiv>")
static void xp_ready(struct xp *x, char *keep) {
    int f, k;
    x->lord = xp_ints(x->nfn); x->lvis = xp_ints(x->nfn);
    x->stext = (char **)xp_zeros(x->nfile); x->sline = (int **)xp_zeros(x->nfile);
    x->snl = xp_zeros(x->nfile); x->sst = xp_zeros(x->nfile); x->spath = (char **)xp_zeros(x->nfile);
    xp_order(x);
    f = 0 - 1;
    if (keep && keep[0]) f = xp_named(x, keep);
    if (f < 0 && x->objmode) f = xp_named(x, "main");
    k = 0;
    while (f < 0 && x->objmode && k < x->nvis) { if (x->fname[x->lvis[k]][0] != '<') f = x->lvis[k]; ++k; }
    if (f < 0 && x->objmode && x->nvis) f = x->lvis[0];
    if (f >= 0) xp_show(x, f, 0);
}

int xp_load(int *xi, char *path, char *dir) {
    struct xp *x;
    x = X(xi);
    xp_stop(x);
    xp_drop(x);
    x->objmode = 1; x->byname = 1; x->failed = 0; x->msg[0] = 0; x->note[0] = 0;
    x->filt[0] = 0; x->filtc = 0;
    ui_ncpy(x->path, path, 128);
    ui_ncpy(x->dir, dir, 128);
    if (!xp_read(x, path)) return 0;
    x->ok = 1;
    xp_ready(x, 0);
    return 1;
}

// The function for the cursor's LINE in the file edited: the one with
// code from that line, else the last one this file starts at or before it
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

// The editor's cursor on LINE (1-based), when it moves: its function, its
// line lit, and the view on it. edit.c calls this as it draws.
void xp_cursor(int *xi, int line) {
    struct xp *x;
    int f, k, hit, rows;
    x = X(xi);
    if (!x->ok || x->objmode || (line == x->lastline && x->cur >= 0)) return;
    rows = x->rows > 0 ? x->rows : 8;
    f = xp_fn_for(x, line);
    // before every function: this file's first, if nothing is shown yet
    k = 0;
    while (f < 0 && x->cur < 0 && k < x->nfn) { if (x->ffile[k] == x->here) f = k; ++k; }
    if (f < 0 && x->cur < 0 && x->nfn) f = 0;
    if (f >= 0) xp_show(x, f, 0);
    x->hfile = x->here; x->hline = line;
    k = 0; hit = 0 - 1;
    while (k < x->nd && hit < 0) { if (x->dfile[k] == x->here && x->dline[k] == line) hit = k; ++k; }
    if (hit >= 0 && (hit < x->top || hit >= x->top + rows)) x->top = hit - rows / 3;
    x->lastline = line;
}

// The line asked for in the editor (after xp_mouse or xp_key said 2): its
// file into PATH (128 bytes), and its number
int xp_goto(int *xi, char *path) {
    struct xp *x;
    x = X(xi);
    ui_ncpy(path, x->gopath, 128);
    return x->goline;
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
    i = xp_fn_at(x, off);
    if (i >= 0 && x->fstart[i] == off) return x->fname[i];
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

// the function list's width in a tab CW wide
static int xp_lw(int cw) { int w; w = cw / 4; if (w < 120) w = 120; if (w > 220) w = 220; return w; }
// how many hex digits the code's offsets take, four at least
static int xp_ow(struct xp *x) { int n, v; n = 1; v = x->ncode >> 4; while (v) { ++n; v = v >> 4; } return n < 4 ? 4 : n; }
// "file:line" of map file F
static void xp_where_s(struct xp *x, char *d, int f, int l) {
    ui_cpy(d, f >= 0 && f < x->nfile ? ui_base(x->fpath[f]) : "?");
    ui_cat(d, ":");
    ui_num(d + ui_len(d), l);
}

// the list: the filter, its order, the names
static void xp_draw_list(struct xp *x, int ox, int oy, int w, int h, int *cols, int focused) {
    int rows, i, k, f, y, c;
    d_rect(ox, oy, w, XHDR, C_FACE);
    ui_field(ox, oy, w - 34, x->filt, x->filtc, focused);
    if (!x->filt[0] && x->ok) {
        ui_num(xb, x->nvis); ui_cat(xb, x->nvis == 1 ? " function" : " functions");
        d_clip(ox + 2, oy + 2, w - 38, XHDR - 4);
        d_text(ox + 8, oy + 5, C_SHADOW, F_SANS, xb);
        d_clip_end();
    }
    ui_button(ox + w - 32, oy + 1, 30, XHDR - 2, "A-Z", x->byname, 0);
    rows = (h - XHDR - 4) / XCH;
    if (rows < 1) rows = 1;
    x->lrows = rows;
    if (x->lfollow) {
        k = xp_listed(x, x->cur);
        if (k >= 0 && k < x->ltop) x->ltop = k;
        if (k >= 0 && k >= x->ltop + rows) x->ltop = k - rows + 1;
        x->lfollow = 0;
    }
    if (x->ltop > x->nvis - rows) x->ltop = x->nvis - rows;
    if (x->ltop < 0) x->ltop = 0;
    y = oy + XHDR;
    d_rect(ox, y, w - 17, h - XHDR, cols[XC_BG]);
    d_clip(ox, y, w - 17, h - XHDR);
    i = 0;
    while (x->ok && i < rows && x->ltop + i < x->nvis) {
        f = x->lvis[x->ltop + i];
        // the one shown lit; the file edited's own functions, and the rest dimmer
        c = !x->objmode && x->ffile[f] != x->here ? cols[XC_COM] : cols[XC_TEXT];
        if (f == x->cur) { d_rect(ox, y + 2 + i * XCH, w - 17, XCH, cols[XC_SEL]); c = cols[XC_KW]; }
        d_textn(ox + 4, y + 2 + i * XCH, c, XFONT, F_MONO, XCW, x->fname[f], ui_len(x->fname[f]));
        ++i;
    }
    if (x->ok && !x->nvis) d_text(ox + 4, y + 4, cols[XC_COM], F_SANS, "None by that name");
    d_clip_end();
    ui_scrollbar(ox + w - 17, y, h - XHDR, x->nvis, rows, x->ltop);
}

// the code: its header, then the listing
static void xp_draw_code(struct xp *x, int ox, int oy, int cw, int ch, int *cols) {
    char m[8];
    int rows, y, k, j, c, i, sx, n, ow, c1, cm, ry, lw, bw, hot;
    char *t;
    // the header: the function, where it is, Back and -O
    bw = x->objmode ? 30 : 70;
    d_rect(ox, oy, cw, XHDR, C_FACE);
    d_bevel(ox, oy, cw, XHDR, B_RAISED);
    ui_cpy(xb, "Code");
    if (x->ok && x->cur >= 0) {
        ui_cpy(xb, x->fname[x->cur]);
        ui_cat(xb, " ("); ui_num(xb + ui_len(xb), x->flen[x->cur]); ui_cat(xb, " words");
        if (x->ffile[x->cur] >= 0 && x->fline[x->cur] > 0) { ui_cat(xb, ", "); xp_where_s(x, xb + ui_len(xb), x->ffile[x->cur], x->fline[x->cur]); }
        ui_cat(xb, ")");
    }
    d_clip(ox, oy, cw - bw - 4, XHDR);
    d_text(ox + 6, oy + 5, C_DARK, F_SANS, xb);
    d_clip_end();
    ui_button(ox + cw - bw, oy + 2, 26, XHDR - 4, "<", 0, 0);
    if (!x->objmode) ui_button(ox + cw - 40, oy + 2, 36, XHDR - 4, "-O", x->opt, 0);

    // the listing
    rows = (ch - XHDR - 4) / XCH;
    if (rows < 1) rows = 1;
    x->rows = rows;
    if (x->top > x->nd - rows) x->top = x->nd - rows;
    if (x->top < 0) x->top = 0;
    y = oy + XHDR;
    lw = cw - 17;
    d_rect(ox, y, lw, ch - XHDR, cols[XC_BG]);
    sx = ox + 6;
    ow = xp_ow(x);
    c1 = ow + 3;                              // labels, and the source's text
    d_clip(ox, y, lw, ch - XHDR);
    k = x->top; i = 0;
    while (x->ok && x->cur >= 0 && k < x->nd && i < rows) {
        ry = y + 2 + i * XCH;
        hot = x->dline[k] > 0 && x->dfile[k] == x->hfile && x->dline[k] == x->hline;
        if (x->dins[k] < 0) {
            // a line of source: its number where the offsets are, its text where the labels are
            d_rect(ox, ry, lw, XCH, hot ? cols[XC_SEL] : cols[XC_GUT]);
            d_rect(ox + 1, ry, 3, XCH, bands[x->dline[k] % 8]);
            ui_num(xb, x->dline[k]);
            n = ui_len(xb);
            d_textn(sx + (ow + 1 - n) * XCW, ry, cols[XC_COM], XFONT, F_MONO, XCW, xb, n);
            t = xp_line(x, x->dfile[k], x->dline[k], &n);
            if (t) { n = xp_untab(t, n, xo, 240); if (n) d_textn(sx + c1 * XCW, ry, cols[XC_TEXT], XFONT, F_MONO, XCW, xo, n); }
            else { xp_where_s(x, xb, x->dfile[k], x->dline[k]); d_textn(sx + c1 * XCW, ry, cols[XC_COM], XFONT, F_MONO, XCW, xb, ui_len(xb)); }
        } else {
            // an instruction: its offset (in hex), a label, the mnemonic and its operand
            j = x->dins[k];
            if (hot) d_rect(ox, ry, lw, XCH, cols[XC_SEL]);
            if (x->dline[k] > 0) d_rect(ox + 1, ry, 3, XCH, bands[x->dline[k] % 8]);
            xb[0] = 0;
            xp_hex(xb, x->ioff[j]);
            n = ui_len(xb + 2);
            d_textn(sx + (ow + 1 - n) * XCW, ry, cols[XC_COM], XFONT, F_MONO, XCW, xb + 2, n);
            cm = c1 + 5;
            if (x->ilab[j]) {
                ui_cpy(xb, "L"); ui_num(xb + 1, x->ilab[j]); ui_cat(xb, ":");
                d_textn(sx + c1 * XCW, ry, cols[XC_PRE], XFONT, F_MONO, XCW, xb, ui_len(xb));
                if (c1 + ui_len(xb) + 1 > cm) cm = c1 + ui_len(xb) + 1;
            }
            c = xp_text(x, j, m, xb);
            d_textn(sx + cm * XCW, ry, cols[XC_KW], XFONT, F_MONO, XCW, m, ui_len(m));
            if (xb[0]) d_textn(sx + (cm + 5) * XCW, ry, cols[c], XFONT, F_MONO, XCW, xb, ui_len(xb));
        }
        if (k == x->sel) d_recto(ox, ry, lw, XCH, cols[XC_KW]);
        ++k; ++i;
    }
    d_clip_end();
    if (x->ok && x->cur < 0) d_text(sx, y + 4, cols[XC_COM], F_SANS, "Pick a function from the list");
    ui_scrollbar(ox + cw - 17, y, ch - XHDR, x->nd, rows, x->top);
    // a failed compile's last words, over the listing
    if (x->failed && !x->busy) {
        if (x->errn > 0) d_textn(sx, y + 4, cols[XC_STR], XFONT, F_MONO, XCW, x->err0, ui_len(x->err0));
        if (x->errn > 1) d_textn(sx, y + 4 + XCH, cols[XC_STR], XFONT, F_MONO, XCW, x->err1, ui_len(x->err1));
    }
}

// The tab at OX, OY, CW by CH: the list, the code, and a footer across
// both; for a text MODIFIED since it was saved, COLS the IDE's scheme
void xp_draw(int *xi, int ox, int oy, int cw, int ch, int *cols, int modified, int focused) {
    struct xp *x;
    char m[8];
    int lw, h, y, k, j, f;
    x = X(xi);
    lw = xp_lw(cw);
    h = ch - XFOOT;
    xp_draw_list(x, ox, oy, lw, h, cols, focused);
    d_rect(ox + lw, oy, 2, h, C_SHADOW);
    xp_draw_code(x, ox + lw + 2, oy, cw - lw - 2, h, cols);

    // the footer: what is going on, the row picked, or what is shown
    y = oy + h;
    d_rect(ox, y, cw, XFOOT, C_FACE);
    d_bevel(ox, y, cw, XFOOT, B_SUNKEN);
    xb[0] = 0;
    f = x->ok && x->cur >= 0 ? x->ffile[x->cur] : 0 - 1;
    if (f >= 0) xp_source(x, f);
    if (x->busy || !x->ok) ui_cpy(xb, x->msg[0] ? x->msg : "Save the file, and the code it compiles to is shown here");
    else if (x->failed) ui_cpy(xb, x->msg);
    else if (x->note[0]) ui_cpy(xb, x->note);
    else if (x->sel >= 0 && x->sel < x->nd && x->dline[x->sel] > 0) {
        k = x->sel;
        xp_where_s(x, xb, x->dfile[k], x->dline[k]);
        if ((j = x->dins[k]) >= 0) { ui_cat(xb, "  "); xp_text(x, j, m, xo); ui_cat(xb, m); ui_cat(xb, " "); ui_ncat(xb, xo, 300); }
        else if (x->dfile[k] >= 0 && x->sst[x->dfile[k]] > 0) { ui_cat(xb, "  "); ui_ncat(xb, x->spath[x->dfile[k]], 300); }
    }
    else if (modified && !x->objmode) ui_cpy(xb, "Changed since it was compiled: save to see the new code");
    else if (f >= 0 && x->sst[f] < 0 && x->fpath[f][0] != '<') { ui_cpy(xb, "No source on this disk for "); ui_ncat(xb, x->fpath[f], 300); }
    else if (f >= 0 && x->sst[f] == 2) { ui_ncpy(xb, x->spath[f], 200); ui_cat(xb, " has changed since this was compiled"); }
    else if (x->objmode) {
        ui_cpy(xb, ui_base(x->path)); ui_cat(xb, ": ");
        ui_num(xb + ui_len(xb), x->nfn); ui_cat(xb, x->nfn == 1 ? " function, " : " functions, ");
        ui_num_commas(xb + ui_len(xb), x->ncode); ui_cat(xb, " words of code, ");
        if (x->nrow) { ui_cat(xb, "a map of "); ui_num(xb + ui_len(xb), x->nfile); ui_cat(xb, x->nfile == 1 ? " file" : " files"); }
        else ui_cat(xb, "no source map");
    }
    else if (x->here < 0) ui_cpy(xb, "The map does not name this file");
    else ui_cpy(xb, x->opt ? "Compiled with -g -O" : "Compiled with -g");
    d_clip(ox, y, cw, XFOOT);
    d_text(ox + 4, y + 3, C_DARK, F_SANS, xb);
    d_clip_end();
}

// ---- the mouse and the keys -------------------------------------------------------------------

// Row K followed: a call (or a function's address) goes to the callee, in
// this tab; anything else with a line, to the line, in the editor (2)
static int xp_follow(struct xp *x, int k) {
    int j, t, v, f, d;
    char *nm;
    j = x->dins[k];
    if (j >= 0 && x->iop[j] >= 0 && xp_operand(x->iop[j])) {
        t = x->ptype[x->ioff[j] + 1]; v = x->pval[x->ioff[j] + 1];
        f = 0 - 1;
        if (t == P_CODE) { f = xp_fn_at(x, v); if (f >= 0 && x->fstart[f] != v) f = 0 - 1; }
        else if (t > 0 && (nm = xp_symname(x, t))) {
            f = xp_named(x, nm);
            if (f < 0) { ui_cpy(x->note, nm); ui_cat(x->note, " is not in this file: it is linked in from elsewhere"); return 1; }
        }
        if (f >= 0 && f != x->cur) { xp_show(x, f, 1); return 1; }
    }
    d = x->dfile[k];
    if (x->dline[k] <= 0 || d < 0) return 1;
    xp_source(x, d);
    if (x->sst[d] <= 0) { ui_cpy(x->note, "No source on this disk for "); ui_ncat(x->note, x->fpath[d], 150); return 1; }
    ui_ncpy(x->gopath, x->spath[d], 128);
    x->goline = x->dline[k];
    if (d == x->here) x->lastline = x->goline;   // the editor's cursor is going there: nothing to follow
    return 2;
}

static int xp_mouse_list(struct xp *x, int ev, int px, int py, int w, int h) {
    int t, k;
    if (py < XHDR) {
        if (ev != E_DOWN) return 0;
        if (ui_hit(px, py, w - 32, 1, 30, XHDR - 2)) { x->byname = !x->byname; if (x->ok) xp_order(x); return 1; }
        if (px < w - 34) x->filtc = ui_field_hit(px, 0, w - 34, x->filt, x->filtc);
        return 1;
    }
    if (px >= w - 17) {
        if (ev != E_DOWN) return 0;
        t = ui_scroll_click(px, py, w - 17, XHDR, h - XHDR, x->nvis, x->lrows, x->ltop);
        if (t >= 0) x->ltop = t;
        return 1;
    }
    k = x->ltop + (py - XHDR - 2) / XCH;
    if (x->ok && k >= 0 && k < x->nvis) xp_show(x, x->lvis[k], 1);
    return 1;
}

static int xp_mouse_code(struct xp *x, int ev, int px, int py, int cw, int ch) {
    int t, k, bw;
    if (py < XHDR) {
        if (ev != E_DOWN) return 0;
        bw = x->objmode ? 30 : 70;
        if (ui_hit(px, py, cw - bw, 2, 26, XHDR - 4)) { xp_back(x); return 1; }
        if (!x->objmode && ui_hit(px, py, cw - 40, 2, 36, XHDR - 4)) {
            x->opt = !x->opt;
            if (x->path[0]) xp_compile((int *)x, x->path, x->dir);
            return 0 - 1;
        }
        return 0;
    }
    if (px >= cw - 17) {
        if (ev != E_DOWN) return 0;
        t = ui_scroll_click(px, py, cw - 17, XHDR, ch - XHDR, x->nd, x->rows, x->top);
        if (t >= 0) x->top = t;
        return 1;
    }
    if (!x->ok || x->cur < 0) return 0;
    k = x->top + (py - XHDR - 2) / XCH;
    if (k < 0 || k >= x->nd) return 0;
    x->sel = k;
    if (x->dline[k] > 0) { x->hfile = x->dfile[k]; x->hline = x->dline[k]; }
    if (ev == E_DBL) return xp_follow(x, k);
    return 1;
}

// A mouse event at PX, PY in the tab, CW by CH (a wheel's PY is how far,
// and PX where): 1 if it did something, 0 if not, 2 for a line to go to
// in the editor (xp_goto), -1 when -O was turned on or off (and it
// compiles again), -2 while a bar is dragged
int xp_mouse(int *xi, int ev, int px, int py, int cw, int ch) {
    struct xp *x;
    int lw, h, t;
    x = X(xi);
    lw = xp_lw(cw);
    h = ch - XFOOT;
    x->note[0] = 0;
    if (ev == E_WHEEL) { if (px < lw) x->ltop = x->ltop + py * 3; else x->top = x->top + py * 3; return 1; }
    if (ev == E_MOVE) {
        t = ui_scroll_drag(lw - 17, XHDR, py);
        if (t >= 0) { x->ltop = t; return 0 - 2; }
        t = ui_scroll_drag(cw - 17, XHDR, py);
        if (t >= 0) { x->top = t; return 0 - 2; }
        return 0;
    }
    if ((ev != E_DOWN && ev != E_DBL) || py >= h) return 0;
    if (px < lw) return xp_mouse_list(x, ev, px, py, lw, h);
    if (px < lw + 2) return 0;
    return xp_mouse_code(x, ev, px - lw - 2, py, cw - lw - 2, h);
}

// A key: typing finds a function by name (the first the filter keeps is
// shown), Up and Down go through the list, PgUp and PgDn scroll the
// code, Enter follows the row picked, Backspace (with nothing typed) or
// Alt+Left goes back. 1 if it was used, 2 for a line to go to in the
// editor; Esc with nothing typed is not used (edit.c goes back to the
// text).
int xp_key(int *xi, int code, int ch, int mods) {
    struct xp *x;
    int k;
    x = X(xi);
    x->note[0] = 0;
    if ((mods & M_ALT) && code == KEY_LEFT) { xp_back(x); return 1; }
    if (code == KEY_UP || code == KEY_DOWN) {
        k = xp_listed(x, x->cur);
        if (k < 0) k = code == KEY_DOWN ? 0 - 1 : x->nvis;
        k = code == KEY_DOWN ? k + 1 : k - 1;
        if (k >= 0 && k < x->nvis) xp_show(x, x->lvis[k], 0);
        return 1;
    }
    if (code == KEY_PGUP) { x->top = x->top - (x->rows - 1); return 1; }
    if (code == KEY_PGDN) { x->top = x->top + (x->rows - 1); return 1; }
    if ((mods & M_CTRL) && code == KEY_HOME) { x->top = 0; return 1; }
    if ((mods & M_CTRL) && code == KEY_END) { x->top = x->nd; return 1; }
    if (code == KEY_ENTER) { if (x->ok && x->sel >= 0 && x->sel < x->nd) return xp_follow(x, x->sel); return 1; }
    if (code == KEY_ESC) {
        if (!x->filt[0]) return 0;
        x->filt[0] = 0; x->filtc = 0;
        if (x->ok) xp_filter(x);
        return 1;
    }
    if (code == KEY_BACK && !x->filt[0]) { xp_back(x); return 1; }
    if (ui_field_key(x->filt, XFILT, &x->filtc, code, ch, mods) && x->ok) {
        xp_filter(x);
        if (x->nvis && xp_listed(x, x->cur) != 0) xp_show(x, x->lvis[0], 0);
    }
    return 1;
}
