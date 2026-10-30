// explore.c -- the IDE's Code tab: the C4 code a C file compiles to, or a
// compiled file's own, a function at a time, with the C source it came
// from between the instructions. A cut-down Code Explorer
// (docs/c4-explorer.md, track E), after the web page's
// (libjs/web/explore.html): the functions down the left, found by typing
// part of a name, and the code of the one picked beside them, each line
// of source the map names shown above the instructions it made, long
// lines folded (or, with Wrap off, scrolled across). It takes the
// editor's place in the window, as a tab beside the text, since at
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
// /usr/src), or beside the file. A program linked against shared
// libraries brings them too, as the loader would (loader.c: a static
// global __c4_needed names each one): their functions follow the
// program's in the list, in a colour of their own, and a call into one
// goes there.
//
// It reads each object itself (a module): the code, the patches that
// say what each operand refers to, the symbols, and the map's files,
// functions and rows. Each operand is named as libjs/c4r.js's insnText
// names it (a function, a global, a string, a label, [bp+n]). A click on
// a row lights every row of its line; a double click (or Enter) on a
// call goes to the callee, and Back (Alt+Left, or Backspace) comes back;
// a double click on any other row opens its line in the editor. Without
// a map the functions come from the symbols, with no source to show.
//
// c4lc splices the helpers it calls (<udiv>, <ld16s>, the soft float
// __c4d_ and long long __c4ll_ ones) into each unit that uses them, so a
// program has as many copies as units that needed one; the list names
// each copy's unit beside it.
//
// The opcode names are the machine's (src/c4bb/sim/devices.js, OPNAMES);
// libjs/tests/test-desktop.mjs checks the copy here against them.

#include "desktop.h"

// the IDE's text metrics (edit.c), and the tab's parts: its headers (a
// field's height), its footer, how long a name is kept, the source lines
// with no code shown between two that have, how far back Back goes, and
// the modules one tab holds (a program and the libraries it loads:
// loader.c's LIB_MAX and one)
enum { XCW = 7, XCH = 15, XFONT = 12, XHDR = 22, XFOOT = XCH + 4, XGAP = 3, XHIST = 16, XFILT = 30,
       XMODS = 9, XSRC = 480 };
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

// One object read: the program shown, or a library it loads
struct xmod {
    char path[128];
    char *buf; int bufn, W;
    int *code; int ncode; char *data; int ndata, memsz;
    int *ptype; int *pval;                // per code word: the patch on it, 0 none
    int nsym; int *sid; int *scls; int *sattr; int *sval; char **sname;
    int nfile; char **fpath; int *fsize;  // the map's files, their sizes compiled
    // each file's source, once looked for: its text, where its lines start and how many,
    // and 1 found, 2 found but not the size it was compiled at, -1 not found
    // -- and for the compiler's own, the line of the file holding them before their first
    char **stext; int **sline; int *snl; int *sst; char **spath; int *soff;
    int nfn; int *fstart; int *flen; int *ffile; int *fline; char **fname;
    int *funit;                           // the unit each function came with (a helper: the one before it), or -1
    int nrow; int *roff; int *rfile; int *rline;
    int fn0;                              // its first function's number among all the tab's
};

struct xp {
    // what is shown: a compile of PATH (a .c file), or the compiled file PATH itself
    char path[128], dir[128], obj[48];
    int objmode;
    int pid, out_r, busy, opt, ok, failed;
    char err0[96], err1[96]; int errn;    // the compiler's last two lines, when it failed
    char part[96]; int partn;
    struct xmod *mods[XMODS]; int nmod;   // the program (0), then the libraries it loads
    int here;                             // module 0's file being edited, or -1
    // every function, numbered: its module and its number there; one of a name shared in its module
    int nall; int *gmod; int *gidx; int *gdup;
    // the function list: all of them in order, and the ones the filter keeps
    int *lord; int *lvis; int nvis, ltop, lrows, byname, lfollow;
    char filt[XFILT + 2]; int filtc;
    // the function shown (a number among all), its module, decoded
    int cur; struct xmod *cm; int nins; int *ioff; int *iop; int *iarg; int *ilab; int *irow;
    int lmin, lmax; int *lband;           // its lines in the file edited: a band's colour, or -1
    // the listing: row i is instruction dins[i], or (dins[i] < 0) piece dseg[i] of line dline[i] of file dfile[i]
    int *dins; int *dfile; int *dline; int *dseg; int nd, laycols, maxw;
    int top, left, wrap, sel, rows, cols, lastline, hmod, hfile, hline;   // the view; the row picked; the line lit
    int hist[XHIST]; int nhist;
    char gopath[128]; int goline;         // a line asked for in the editor
    char msg[96], note[160];
};

static struct xp *X(int *x) { return (struct xp *)x; }
static char xb[320], xo[XSRC + 8];
static char *hexd = "0123456789abcdef";

// ---- reading an object -----------------------------------------------------------------------

static int xm_word(struct xmod *m, int p) {
    char *b;
    int v;
    b = m->buf + p;
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

static void xm_free(struct xmod *m) {
    int i;
    if (m->buf) free(m->buf);
    if (m->code) { free((char *)m->code); free((char *)m->ptype); free((char *)m->pval); }
    i = 0; while (i < m->nsym) { free(m->sname[i]); ++i; }
    if (m->sid) { free((char *)m->sid); free((char *)m->scls); free((char *)m->sattr); free((char *)m->sval); free((char *)m->sname); }
    i = 0;
    while (i < m->nfile) {
        free(m->fpath[i]);
        if (m->stext && m->stext[i]) { free(m->stext[i]); free((char *)m->sline[i]); free(m->spath[i]); }
        ++i;
    }
    if (m->fpath) { free((char *)m->fpath); free((char *)m->fsize); }
    if (m->stext) { free((char *)m->stext); free((char *)m->sline); free((char *)m->snl); free((char *)m->sst); free((char *)m->spath); free((char *)m->soff); }
    i = 0; while (i < m->nfn) { free(m->fname[i]); ++i; }
    if (m->fstart) { free((char *)m->fstart); free((char *)m->flen); free((char *)m->ffile); free((char *)m->fline); free((char *)m->fname); }
    if (m->funit) free((char *)m->funit);
    if (m->roff) { free((char *)m->roff); free((char *)m->rfile); free((char *)m->rline); }
    free((char *)m);
}

// The SMAP chunk: files, functions and rows (docs/c4-explorer.md 2.2)
static int xm_smap(struct xmod *m, char *b, int n) {
    int flags, i, k, start, off, file, line, d;
    up = 0;
    if ((b[0] & 255) != 1) return 0;
    flags = b[1] & 255;
    up = 2;
    m->nfile = xp_u(b);
    m->fpath = (char **)xp_ints(m->nfile);
    m->fsize = xp_ints(m->nfile);
    i = 0;
    while (i < m->nfile) {
        k = xp_u(b);
        m->fpath[i] = xp_str(b, k);
        m->fsize[i] = xp_u(b);                // its size, then its CRC
        if (flags & 1) up = up + 4;
        ++i;
    }
    m->nfn = xp_u(b);
    m->fstart = xp_ints(m->nfn); m->flen = xp_ints(m->nfn);
    m->ffile = xp_ints(m->nfn); m->fline = xp_ints(m->nfn);
    m->fname = (char **)xp_ints(m->nfn);
    start = 0; i = 0;
    while (i < m->nfn) {
        start = start + xp_u(b);
        m->fstart[i] = start;
        m->flen[i] = xp_u(b);
        k = xp_u(b);
        m->fname[i] = xp_str(b, k);
        m->ffile[i] = xp_u(b);
        m->fline[i] = xp_u(b);
        ++i;
    }
    m->nrow = xp_u(b);
    m->roff = xp_ints(m->nrow); m->rfile = xp_ints(m->nrow); m->rline = xp_ints(m->nrow);
    off = 0; file = 0; line = 0; i = 0;
    while (i < m->nrow && up < n) {
        d = xp_u(b);
        off = off + (d >> 1);
        if (d & 1) file = xp_u(b);
        line = line + xp_s(b);
        m->roff[i] = off; m->rfile[i] = file; m->rline[i] = line;
        ++i;
    }
    m->nrow = i;
    return 1;
}

// With no map, the functions are the symbols' own, in address order, each
// up to the next
static void xm_symfuncs(struct xmod *m) {
    int i, n, gap, j, t, *v;
    n = 0; i = 0;
    while (i < m->nsym) { if (m->scls[i] == S_FUN && !(m->sattr[i] & S_EXTERN) && m->sval[i] >= 0 && m->sval[i] < m->ncode) ++n; ++i; }
    if (!n) return;
    v = xp_ints(n);
    n = 0; i = 0;
    while (i < m->nsym) { if (m->scls[i] == S_FUN && !(m->sattr[i] & S_EXTERN) && m->sval[i] >= 0 && m->sval[i] < m->ncode) { v[n] = i; ++n; } ++i; }
    gap = n / 2;
    while (gap > 0) {
        i = gap;
        while (i < n) {
            t = v[i]; j = i;
            while (j >= gap && m->sval[v[j - gap]] > m->sval[t]) { v[j] = v[j - gap]; j = j - gap; }
            v[j] = t;
            ++i;
        }
        gap = gap / 2;
    }
    m->nfn = n;
    m->fstart = xp_ints(n); m->flen = xp_ints(n); m->ffile = xp_ints(n); m->fline = xp_ints(n);
    m->fname = (char **)xp_ints(n);
    i = 0;
    while (i < n) {
        m->fstart[i] = m->sval[v[i]];
        m->flen[i] = (i + 1 < n ? m->sval[v[i + 1]] : m->ncode) - m->fstart[i];
        m->ffile[i] = 0 - 1; m->fline[i] = 0;
        m->fname[i] = (char *)ualloc(ui_len(m->sname[v[i]]) + 1);
        ui_cpy(m->fname[i], m->sname[v[i]]);
        ++i;
    }
    free((char *)v);
}

// A map file of the compiler's own, as "<c4lc softfp>"
static int xm_own(struct xmod *m, int f) { return f < 0 || f >= m->nfile || m->fpath[f][0] == '<' || !m->fpath[f][0]; }

// The object at PATH, as libjs/c4r.js's readC4r reads it, into a new
// module; 0 and a message in MSG if it is not one
static struct xmod *xm_read(char *path, char *msg) {
    int st[4], fd, n, p, W, clen, dlen, plen, slen, conl, desl, i, t, a, v, len, tag, u;
    char *b;
    struct xmod *m;
    if (ustat(path, st) < 0 || st[0] != 2) { ui_cpy(msg, "Cannot find "); ui_ncat(msg, path, 96); return 0; }
    n = st[1];
    m = (struct xmod *)ualloc(sizeof(struct xmod));
    memset((char *)m, 0, sizeof(struct xmod));
    ui_ncpy(m->path, path, 128);
    m->buf = b = (char *)ualloc(n + 8);
    fd = uopen(path, O_RD);
    if (fd < 0 || uread(fd, b, n) != n) { if (fd >= 0) uclose(fd); ui_cpy(msg, "Cannot read the object"); xm_free(m); return 0; }
    uclose(fd);
    m->bufn = n;
    if (n < 13 || b[0] != 'C' || b[1] != '4' || b[2] != 'R') { ui_cpy(msg, "Not a C4R object"); xm_free(m); return 0; }
    W = (b[4] & 255) / 8;
    if (W != 4 && W != 8) { ui_cpy(msg, "A word size this cannot read"); xm_free(m); return 0; }
    m->W = W;
    p = 5;
    m->memsz = xm_word(m, p); p = p + W;
    if (W == 4) p = p + W;
    p = p + W;                                // the entry
    clen = xm_word(m, p); p = p + W;
    dlen = xm_word(m, p); p = p + W;
    plen = xm_word(m, p); p = p + W;
    slen = xm_word(m, p); p = p + W;
    conl = xm_word(m, p); p = p + W;
    desl = xm_word(m, p); p = p + W;
    if (m->memsz < dlen) m->memsz = dlen;
    if (p + W * (clen + 3 * plen + conl + desl + 5) + dlen > n || b[p] != 'C') { ui_cpy(msg, "The object is cut short"); xm_free(m); return 0; }
    p = p + W;
    m->ncode = clen;
    m->code = xp_ints(clen); m->ptype = xp_ints(clen); m->pval = xp_ints(clen);
    i = 0;
    while (i < clen) { m->code[i] = xm_word(m, p); m->ptype[i] = 0; p = p + W; ++i; }
    p = p + W;                                // 'D'
    m->data = b + p; m->ndata = dlen;
    p = p + dlen + W;                         // the data, 'P'
    i = 0;
    while (i < plen) {
        t = xm_word(m, p); a = xm_word(m, p + W); v = xm_word(m, p + 2 * W);
        // only what is patched into the code: a code or data address, or a symbol
        if ((t == P_CODE || t == P_DATA || t > 0) && a >= 0 && a < clen) { m->ptype[a] = t; m->pval[a] = v; }
        p = p + 3 * W;
        ++i;
    }
    p = p + W + conl * W + W + desl * W;      // 'c', 'd'
    if (p + W <= n && b[p] == 'S') {
        p = p + W;
        m->sid = xp_ints(slen); m->scls = xp_ints(slen); m->sattr = xp_ints(slen); m->sval = xp_ints(slen);
        m->sname = (char **)xp_ints(slen);
        i = 0;
        while (i < slen && p + 4 * W + 1 <= n) {
            m->sid[i] = xm_word(m, p); m->scls[i] = xm_word(m, p + 2 * W); m->sattr[i] = xm_word(m, p + 3 * W);
            p = p + 4 * W;
            len = b[p] & 255; ++p;
            if (p + len + W > n) break;
            up = p; m->sname[i] = xp_str(b, len); p = p + len;
            m->sval[i] = xm_word(m, p); p = p + W;
            ++i;
        }
        m->nsym = i;
    }
    // the extension trailer: 'X', then tag, length and payload, padded, to a 0 tag
    if (p + W <= n && b[p] == 'X') {
        p = p + W;
        while (p + W <= n && (tag = xm_word(m, p)) != 0) {
            len = xm_word(m, p + W); p = p + 2 * W;
            if (len < 0 || p + len > n) break;
            if (b[p - 2 * W] == 'S' && b[p - 2 * W + 1] == 'M' && b[p - 2 * W + 2] == 'A' && b[p - 2 * W + 3] == 'P')
                xm_smap(m, b + p, len);
            p = p + ((len + W - 1) & (0 - W));
        }
    }
    if (!m->nfn) xm_symfuncs(m);
    if (!m->nfn) { ui_cpy(msg, "No functions: the object has no map and no symbols"); xm_free(m); return 0; }
    // the unit each function came with: its own file, or for a helper
    // spliced in by the compiler, the file of the function before it
    m->funit = xp_ints(m->nfn);
    u = 0 - 1; i = 0;
    while (i < m->nfn) { if (!xm_own(m, m->ffile[i]) && m->fline[i] > 0) u = m->ffile[i]; m->funit[i] = u; ++i; }
    m->stext = (char **)xp_zeros(m->nfile); m->sline = (int **)xp_zeros(m->nfile);
    m->snl = xp_zeros(m->nfile); m->sst = xp_zeros(m->nfile); m->spath = (char **)xp_zeros(m->nfile);
    m->soff = xp_zeros(m->nfile);
    return m;
}

// ---- the sources ------------------------------------------------------------------------------

static int xp_isfile(char *p) { int st[4]; return ustat(p, st) == 0 && st[0] == 2; }
static void xp_dirof(char *path, char *out) {
    int k;
    ui_ncpy(out, path, 128);
    k = ui_len(out);
    while (k > 0 && out[k - 1] != '/') --k;
    if (k > 1) --k;
    out[k] = 0;
    if (!out[0]) ui_cpy(out, "/");
}

// The helpers c4lc brings with it are C kept in strings of its parser:
// "<c4lc softfp>" is p:fpsrc and "<c4lc long long>" p:llsrc, each from
// the line after its define's opening quote; the marker that starts it,
// or 0 if map file I is not one of them
static char *xp_helper(struct xmod *m, int i) {
    if (ui_eq(m->fpath[i], "<c4lc softfp>")) return "(define p:fpsrc \"";
    if (ui_eq(m->fpath[i], "<c4lc long long>")) return "(define p:llsrc \"";
    return 0;
}

// Where module M's map file I is on this disk, into OUT: the file edited;
// the map's path as it is, or under /usr; or beside the module; the
// compiler's helpers, in its parser's source. 0 if nowhere.
static int xp_where(struct xp *x, struct xmod *m, int i, char *out) {
    char *p, dir[128];
    p = m->fpath[i];
    if (xp_helper(m, i)) { ui_cpy(out, "/usr/src/c4sp/lisp/c4lc-parse.lisp"); return xp_isfile(out); }
    if (xm_own(m, i)) return 0;
    if (m == x->mods[0] && i == x->here) { ui_ncpy(out, x->path, 128); return 1; }
    if (m == x->mods[0]) ui_ncpy(dir, x->dir, 128); else xp_dirof(m->path, dir);
    if (p[0] == '/') { if (ui_ncpy(out, p, 128) && xp_isfile(out)) return 1; }
    else {
        ui_cpy(out, "/usr/");
        if (ui_ncat(out, p, 128) && xp_isfile(out)) return 1;
        if (dir[0] && ui_join(out, dir, p, 128) && xp_isfile(out)) return 1;
    }
    if (dir[0] && ui_join(out, dir, ui_base(p), 128) && xp_isfile(out)) return 1;
    return 0;
}

// module M's map file I's source, read the first time it is asked for
static void xp_source(struct xp *x, struct xmod *m, int i) {
    char p[128];
    int st[4], fd, n, k, nl, j;
    char *t, *mk;
    int *ls;
    if (!m || i < 0 || i >= m->nfile || m->sst[i]) return;
    m->sst[i] = 0 - 1;
    if (!xp_where(x, m, i, p) || ustat(p, st) < 0 || st[1] > 1048576) return;
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
    m->sst[i] = n == m->fsize[i] ? 1 : 2;
    // the compiler's helpers: their first line is their define's, after the quote
    if ((mk = xp_helper(m, i))) {
        k = ui_len(mk); j = 0;
        while (j < nl && (ls[j] + k > n || memcmp(t + ls[j], mk, k))) ++j;
        if (j == nl) { free(t); free((char *)ls); return; }
        m->soff[i] = j;
        m->sst[i] = 1;
    }
    m->stext[i] = t; m->sline[i] = ls; m->snl[i] = nl;
    m->spath[i] = (char *)ualloc(ui_len(p) + 1);
    ui_cpy(m->spath[i], p);
}

// line L (1-based) of module M's map file I, its tabs spaced out to eights,
// into xo: its length, or -1 if there is no such line to read
static int xp_line(struct xp *x, struct xmod *m, int i, int l) {
    int a, b, k;
    char *s;
    xp_source(x, m, i);
    if (!m || i < 0 || i >= m->nfile || m->sst[i] <= 0) return 0 - 1;
    l = l + m->soff[i];
    if (l < 1 || l > m->snl[i]) return 0 - 1;
    a = m->sline[i][l - 1]; b = m->sline[i][l] - 1;
    if (b > a && m->stext[i][b - 1] == 13) --b;
    s = m->stext[i];
    k = 0;
    while (a < b && k < XSRC) {
        if (s[a] == 9) { xo[k] = ' '; ++k; while ((k & 7) && k < XSRC) { xo[k] = ' '; ++k; } }
        else { xo[k] = s[a]; ++k; }
        ++a;
    }
    xo[k] = 0;
    return k;
}
static int xp_blank(char *s, int n) {
    int i;
    i = 0;
    while (i < n) { if (s[i] != ' ') return 0; ++i; }
    return 1;
}

// ---- the compile ---------------------------------------------------------------------------

static int xp_seq;

int *xp_new() {
    struct xp *x;
    x = (struct xp *)ualloc(sizeof(struct xp));
    memset((char *)x, 0, sizeof(struct xp));
    x->cur = 0 - 1; x->here = 0 - 1; x->sel = 0 - 1; x->lastline = 0 - 1; x->hfile = 0 - 1; x->opt = 1; x->wrap = 1;
    ++xp_seq;
    ui_cpy(x->obj, "/tmp/.ide-");
    ui_num(x->obj + ui_len(x->obj), getpid());
    ui_cat(x->obj, "-");
    ui_num(x->obj + ui_len(x->obj), xp_seq);
    ui_cat(x->obj, ".c4o");
    return (int *)x;
}

static void xp_unshow(struct xp *x) {
    if (x->ioff) { free((char *)x->ioff); free((char *)x->iop); free((char *)x->iarg); free((char *)x->ilab); free((char *)x->irow); }
    if (x->lband) free((char *)x->lband);
    if (x->dins) { free((char *)x->dins); free((char *)x->dfile); free((char *)x->dline); free((char *)x->dseg); }
    x->ioff = 0; x->lband = 0; x->dins = 0; x->nins = 0; x->nd = 0; x->laycols = 0;
}

static void xp_drop(struct xp *x) {
    int i;
    xp_unshow(x);
    i = 0; while (i < x->nmod) { xm_free(x->mods[i]); x->mods[i] = 0; ++i; }
    if (x->gmod) { free((char *)x->gmod); free((char *)x->gidx); free((char *)x->gdup); }
    if (x->lord) { free((char *)x->lord); free((char *)x->lvis); }
    x->gmod = 0; x->lord = 0; x->nmod = 0; x->nall = 0; x->nvis = 0; x->nhist = 0;
    x->cur = 0 - 1; x->cm = 0; x->here = 0 - 1; x->ok = 0; x->sel = 0 - 1; x->hfile = 0 - 1; x->hline = 0;
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

static int xp_open(struct xp *x, char *path, char *keep);

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
    if (x->ok && x->cur >= 0) ui_ncpy(keep, x->cm->fname[x->gidx[x->cur]], 64);
    xp_drop(x);
    if (code != 0) { x->failed = 1; ui_cpy(x->msg, "It did not compile:"); return 1; }
    if (xp_open(x, x->obj, keep)) x->msg[0] = 0;
    return 1;
}

int  xp_busy(int *xi) { return X(xi)->busy; }
int  xp_opt(int *xi) { return X(xi)->opt; }

// ---- every function, and the list ----------------------------------------------------------

static struct xmod *xp_mod(struct xp *x, int g) { return x->mods[x->gmod[g]]; }
static char *xp_name(struct xp *x, int g) { return x->mods[x->gmod[g]]->fname[x->gidx[g]]; }
static int xp_lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int xp_cmp(char *a, char *b) {
    while (*a && *a == *b) { ++a; ++b; }
    return (*a & 255) - (*b & 255);
}
// the list's order: the program's functions, then each library's; in each,
// by name (then address) or by address
static int xp_before(struct xp *x, int a, int b, int byname) {
    int c;
    if (x->gmod[a] != x->gmod[b]) return x->gmod[a] < x->gmod[b];
    c = byname ? xp_cmp(xp_name(x, a), xp_name(x, b)) : 0;
    return c < 0 || (c == 0 && x->gidx[a] < x->gidx[b]);
}
static void xp_sort(struct xp *x, int *v, int n, int byname) {
    int i, j, t, gap;
    gap = n / 2;
    while (gap > 0) {
        i = gap;
        while (i < n) {
            t = v[i]; j = i;
            while (j >= gap && xp_before(x, t, v[j - gap], byname)) { v[j] = v[j - gap]; j = j - gap; }
            v[j] = t;
            ++i;
        }
        gap = gap / 2;
    }
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
    while (i < x->nall) { if (xp_has(xp_name(x, x->lord[i]), x->filt)) { x->lvis[n] = x->lord[i]; ++n; } ++i; }
    x->nvis = n; x->ltop = 0; x->lfollow = 1;
}
static void xp_order(struct xp *x) {
    int i;
    i = 0; while (i < x->nall) { x->lord[i] = i; ++i; }
    if (x->byname) xp_sort(x, x->lord, x->nall, 1);
    xp_filter(x);
}
// the function named NM, in the modules' order: outside module AVOID
// first, then in it; -1 if none
static int xp_named(struct xp *x, char *nm, int avoid) {
    int g, pass;
    pass = 0;
    while (pass < 2) {
        g = 0;
        while (g < x->nall) {
            if ((pass == 0) == (x->gmod[g] != avoid) && ui_eq(xp_name(x, g), nm)) return g;
            ++g;
        }
        ++pass;
    }
    return 0 - 1;
}
static int xp_listed(struct xp *x, int g) {
    int k;
    k = 0;
    while (k < x->nvis) { if (x->lvis[k] == g) return k; ++k; }
    return 0 - 1;
}

// ---- the function shown ------------------------------------------------------------------------

static int xp_operand(int op) {
    return op <= O_ADJ || op == O_JSRI || op == O_JSRS || (op >= O_LDL && op <= O_IMMP) || op == O_STL;
}

// the row an offset is in, by binary search; -1 before the first
static int xm_row(struct xmod *m, int off) {
    int lo, hi, k, r;
    lo = 0; hi = m->nrow - 1; r = 0 - 1;
    while (lo <= hi) { k = (lo + hi) >> 1; if (m->roff[k] <= off) { r = k; lo = k + 1; } else hi = k - 1; }
    return r;
}
// the function an offset is in, by binary search (they are in address
// order, and do not overlap); -1 if none
static int xm_fn_at(struct xmod *m, int off) {
    int lo, hi, k;
    lo = 0; hi = m->nfn - 1;
    while (lo <= hi) {
        k = (lo + hi) >> 1;
        if (off < m->fstart[k]) hi = k - 1;
        else if (off >= m->fstart[k] + m->flen[k]) lo = k + 1;
        else return k;
    }
    return 0 - 1;
}

// decode function G, its labels, and its lines' bands
static void xp_decode(struct xp *x, int g) {
    int s, e, i, n, op, k, t, lo, hi, md, l, r, f, here;
    struct xmod *m;
    xp_unshow(x);
    x->cur = g; x->cm = m = xp_mod(x, g); f = x->gidx[g];
    x->top = 0; x->left = 0; x->sel = 0 - 1;
    s = m->fstart[f]; e = s + m->flen[f];
    if (e > m->ncode) e = m->ncode;
    n = e - s + 1;
    x->ioff = xp_ints(n); x->iop = xp_ints(n); x->iarg = xp_ints(n); x->ilab = xp_ints(n); x->irow = xp_ints(n);
    i = s; n = 0;
    while (i < e) {
        op = m->code[i];
        x->ioff[n] = i; x->iop[n] = op; x->ilab[n] = 0; x->irow[n] = xm_row(m, i);
        if (xp_operand(op) && i + 1 < e) { x->iarg[n] = m->code[i + 1]; i = i + 2; }
        else { x->iarg[n] = 0; if (xp_operand(op)) x->iop[n] = 0 - 1; i = i + 1; }   // an operand cut off: a word
        ++n;
    }
    x->nins = n;
    // branch targets inside the function are labels, L1 up in order
    k = 0;
    while (k < n) {
        op = x->iop[k];
        if ((op == O_JMP || op == O_BZ || op == O_BNZ) && m->ptype[x->ioff[k] + 1] == P_CODE) {
            t = m->pval[x->ioff[k] + 1];
            lo = 0; hi = n - 1;
            while (lo <= hi) { md = (lo + hi) >> 1; if (x->ioff[md] == t) { x->ilab[md] = 1; lo = hi + 1; } else if (x->ioff[md] < t) lo = md + 1; else hi = md - 1; }
        }
        ++k;
    }
    l = 0; k = 0;
    while (k < n) { if (x->ilab[k]) { ++l; x->ilab[k] = l; } ++k; }
    // which of the edited file's lines have code here, and their colours
    here = m == x->mods[0] ? x->here : 0 - 1;
    x->lmin = 0; x->lmax = 0 - 1;
    k = 0;
    while (here >= 0 && k < n) {
        r = x->irow[k];
        if (r >= 0 && m->rfile[r] == here && m->rline[r] > 0) {
            if (x->lmax < x->lmin) { x->lmin = x->lmax = m->rline[r]; }
            if (m->rline[r] < x->lmin) x->lmin = m->rline[r];
            if (m->rline[r] > x->lmax) x->lmax = m->rline[r];
        }
        ++k;
    }
    if (x->lmax >= x->lmin) {
        x->lband = xp_ints(x->lmax - x->lmin + 1);
        k = 0; while (k <= x->lmax - x->lmin) { x->lband[k] = 0 - 1; ++k; }
        k = 0;
        while (k < n) {
            r = x->irow[k];
            if (r >= 0 && m->rfile[r] == here && m->rline[r] > 0) x->lband[m->rline[r] - x->lmin] = bands[m->rline[r] % 8];
            ++k;
        }
    }
}

// one row of the listing
static int xp_row_add(struct xp *x, int n, int ins, int file, int line, int seg) {
    x->dins[n] = ins; x->dfile[n] = file; x->dline[n] = line; x->dseg[n] = seg;
    return n + 1;
}
// ... and the rows of a source line, COLS wide (0: one row, scrolled across)
static int xp_src_rows(struct xp *x, int n, int file, int line, int len, int cols) {
    int k;
    if (cols <= 0 || len <= cols) return xp_row_add(x, n, 0 - 1, file, line, 0);
    k = 0;
    while (k * cols < len) { n = xp_row_add(x, n, 0 - 1, file, line, k); ++k; }
    return n;
}

// The listing's rows, for source COLS wide (0 when it is not wrapped):
// each instruction, after the source line it came from when that is a
// new one. The few lines between one line and the next with no code of
// their own -- a {, a declaration, a comment -- are shown too, as they
// read in the file; blank ones are not.
static int xp_text(struct xp *x, int k, char *mn, char *d);
static void xp_layout(struct xp *x, int cols) {
    int f, k, r, l, lf, ll, a, n, len, cap, w;
    char mn[8];
    struct xmod *m;
    if (x->dins) { free((char *)x->dins); free((char *)x->dfile); free((char *)x->dline); free((char *)x->dseg); }
    m = x->cm; f = x->gidx[x->cur];
    // a row for each piece of each line: count them first
    cap = 8; k = 0; lf = m->ffile[f]; ll = m->fline[f] - 1;
    while (k < x->nins) {
        r = x->irow[k];
        if (r >= 0 && m->rline[r] > 0 && (m->rfile[r] != lf || m->rline[r] != ll)) {
            l = m->rline[r];
            a = l;
            if (m->rfile[r] == lf && l > ll && l - ll <= XGAP + 1) a = ll + 1;
            while (a <= l) { len = xp_line(x, m, m->rfile[r], a); cap = cap + (cols > 0 && len > cols ? (len + cols - 1) / cols : 1); ++a; }
            lf = m->rfile[r]; ll = l;
        }
        ++cap; ++k;
    }
    x->dins = xp_ints(cap); x->dfile = xp_ints(cap); x->dline = xp_ints(cap); x->dseg = xp_ints(cap);
    n = 0; x->maxw = 0;
    lf = m->ffile[f]; ll = m->fline[f] - 1;   // so the function's own first line is shown
    k = 0;
    while (k < x->nins) {
        r = x->irow[k];
        if (r >= 0 && m->rline[r] > 0 && (m->rfile[r] != lf || m->rline[r] != ll)) {
            l = m->rline[r];
            a = l;
            if (m->rfile[r] == lf && l > ll && l - ll <= XGAP + 1) a = ll + 1;
            while (a <= l) {
                len = xp_line(x, m, m->rfile[r], a);
                if (a == l || (len > 0 && !xp_blank(xo, len))) {
                    n = xp_src_rows(x, n, m->rfile[r], a, len, cols);
                    if (len > x->maxw) x->maxw = len;
                }
                ++a;
            }
            lf = m->rfile[r]; ll = l;
        }
        n = xp_row_add(x, n, k, r >= 0 ? m->rfile[r] : 0 - 1, r >= 0 ? m->rline[r] : 0, 0);
        ++k;
    }
    x->nd = n;
    // the widest instruction, for the bar across: a label's room, the
    // mnemonic's, and its operand, which can be a long string
    k = 0;
    while (k < x->nins) {
        xp_text(x, k, mn, xb);
        w = 10 + ui_len(xb) + (x->ilab[k] ? 4 : 0);
        if (w > x->maxw) x->maxw = w;
        ++k;
    }
    x->laycols = cols > 0 ? cols : 0 - 1;
}

// Show function G; PUSH keeps the one shown before for Back
static void xp_show(struct xp *x, int g, int push) {
    int i;
    if (g < 0 || g >= x->nall) return;
    if (push && x->cur >= 0 && x->cur != g) {
        if (x->nhist == XHIST) { i = 1; while (i < XHIST) { x->hist[i - 1] = x->hist[i]; ++i; } --x->nhist; }
        x->hist[x->nhist] = x->cur; ++x->nhist;
    }
    if (g != x->cur || !x->ioff) xp_decode(x, g);
    x->lfollow = 1;
}
static void xp_back(struct xp *x) {
    if (!x->nhist) return;
    --x->nhist;
    xp_show(x, x->hist[x->nhist], 0);
}

// the libraries module M loads, as the loader finds them: a static global
// __c4_needed for each, whose value is the data offset of its path
static void xp_needs(struct xp *x, struct xmod *m) {
    int i, k, j, n;
    char path[128];
    struct xmod *l;
    i = 0;
    while (i < m->nsym && x->nmod < XMODS) {
        if (m->scls[i] == S_GLO && ui_eq(m->sname[i], "__c4_needed") && m->sval[i] >= 0 && m->sval[i] < m->ndata) {
            k = m->sval[i]; n = 0;
            while (k + n < m->ndata && m->data[k + n] && n < 127) { path[n] = m->data[k + n]; ++n; }
            path[n] = 0;
            j = 0;
            while (j < x->nmod && !ui_eq(x->mods[j]->path, path)) ++j;
            if (j == x->nmod && (l = xm_read(path, x->note))) { x->mods[x->nmod] = l; ++x->nmod; }
        }
        ++i;
    }
}

// The object at PATH as module 0, the libraries it loads after it, every
// function numbered, and the list; shows KEEP (by name) if it is there,
// else a compiled file's main, or the first function of its own (not
// one of the compiler's, as "<udiv>"). 0 if it cannot be read.
static int xp_open(struct xp *x, char *path, char *keep) {
    struct xmod *m;
    int i, j, k, g, *v;
    x->note[0] = 0;
    if (!(m = xm_read(path, x->msg))) return 0;
    x->mods[0] = m; x->nmod = 1;
    // the file edited, by its path, else by its name
    i = 0;
    while (!x->objmode && i < m->nfile && x->here < 0) { if (ui_eq(m->fpath[i], x->path)) x->here = i; ++i; }
    i = 0;
    while (!x->objmode && i < m->nfile && x->here < 0) { if (ui_eq(ui_base(m->fpath[i]), ui_base(x->path))) x->here = i; ++i; }
    i = 0;
    while (i < x->nmod) { xp_needs(x, x->mods[i]); ++i; }
    x->nall = 0; i = 0;
    while (i < x->nmod) { x->mods[i]->fn0 = x->nall; x->nall = x->nall + x->mods[i]->nfn; ++i; }
    x->gmod = xp_ints(x->nall); x->gidx = xp_ints(x->nall); x->gdup = xp_zeros(x->nall);
    g = 0; i = 0;
    while (i < x->nmod) { j = 0; while (j < x->mods[i]->nfn) { x->gmod[g] = i; x->gidx[g] = j; ++g; ++j; } ++i; }
    // names a module has more than once: by name, side by side
    v = xp_ints(x->nall);
    k = 0; while (k < x->nall) { v[k] = k; ++k; }
    xp_sort(x, v, x->nall, 1);
    k = 1;
    while (k < x->nall) {
        if (x->gmod[v[k]] == x->gmod[v[k - 1]] && ui_eq(xp_name(x, v[k]), xp_name(x, v[k - 1]))) { x->gdup[v[k]] = 1; x->gdup[v[k - 1]] = 1; }
        ++k;
    }
    free((char *)v);
    x->lord = xp_ints(x->nall); x->lvis = xp_ints(x->nall);
    x->ok = 1;
    xp_order(x);
    g = 0 - 1;
    if (keep && keep[0]) g = xp_named(x, keep, 0 - 1);
    if (g < 0 && x->objmode) g = xp_named(x, "main", 0 - 1);
    k = 0;
    while (g < 0 && x->objmode && k < x->nvis) { if (xp_name(x, x->lvis[k])[0] != '<') g = x->lvis[k]; ++k; }
    if (g < 0 && x->objmode && x->nvis) g = x->lvis[0];
    if (g >= 0) xp_show(x, g, 0);
    return 1;
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
    return xp_open(x, path, 0);
}

// The function for the cursor's LINE in the file edited: the one with
// code from that line, else the last one this file starts at or before it
static int xp_fn_for(struct xp *x, int line) {
    int i, f, best;
    struct xmod *m;
    m = x->mods[0];
    i = 0;
    while (i < m->nrow) {
        if (m->rfile[i] == x->here && m->rline[i] == line && (f = xm_fn_at(m, m->roff[i])) >= 0) return f;
        ++i;
    }
    best = 0 - 1;
    i = 0;
    while (i < m->nfn) {
        if (m->ffile[i] == x->here && m->fline[i] <= line && (best < 0 || m->fline[i] >= m->fline[best])) best = i;
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

// the first row of line LINE of the file edited, or -1
static int xp_row_of(struct xp *x, int line) {
    int k;
    k = 0;
    while (k < x->nd) { if (x->cm == x->mods[0] && x->dfile[k] == x->here && x->dline[k] == line) return k; ++k; }
    return 0 - 1;
}

// The editor's cursor on LINE (1-based), when it moves: its function, its
// line lit, and the view on it. edit.c calls this as it draws.
void xp_cursor(int *xi, int line) {
    struct xp *x;
    int f, k;
    x = X(xi);
    if (!x->ok || x->objmode || (line == x->lastline && x->cur >= 0)) return;
    f = xp_fn_for(x, line);
    // before every function: this file's first, if nothing is shown yet
    k = 0;
    while (f < 0 && x->cur < 0 && k < x->mods[0]->nfn) { if (x->mods[0]->ffile[k] == x->here) f = k; ++k; }
    if (f < 0 && x->cur < 0 && x->mods[0]->nfn) f = 0;
    if (f >= 0) xp_show(x, f, 0);
    x->hmod = 0; x->hfile = x->here; x->hline = line;
    x->lastline = line;
    x->sel = 0 - 2;                           // the view goes to the line when it is laid out
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

static char *xm_symname(struct xmod *m, int id) {
    int i;
    i = 0;
    while (i < m->nsym) { if (m->sid[i] == id) return m->sname[i]; ++i; }
    return 0;
}
// the function starting at OFF: the symbols', then the map's; 0 if none
static char *xm_fn_named(struct xmod *m, int off) {
    int i;
    i = 0;
    while (i < m->nsym) { if (m->scls[i] == S_FUN && !(m->sattr[i] & S_EXTERN) && m->sval[i] == off) return m->sname[i]; ++i; }
    i = xm_fn_at(m, off);
    if (i >= 0 && m->fstart[i] == off) return m->fname[i];
    return 0;
}
// the global a data offset is in, or -1
static int xm_global(struct xmod *m, int off) {
    int i, g, next;
    g = 0 - 1;
    i = 0;
    while (i < m->nsym) {
        if (m->scls[i] == S_GLO && !(m->sattr[i] & S_EXTERN) && m->sval[i] <= off && (g < 0 || m->sval[i] > m->sval[g])) g = i;
        ++i;
    }
    if (g < 0) return g;
    next = m->memsz;
    i = 0;
    while (i < m->nsym) {
        if (m->scls[i] == S_GLO && !(m->sattr[i] & S_EXTERN) && m->sval[i] > m->sval[g] && m->sval[i] < next) next = m->sval[i];
        ++i;
    }
    if (next <= m->sval[g]) next = m->sval[g] + 1;
    return off < next ? g : 0 - 1;
}
// the printable C string at a data offset: its length (200 at most), or
// -1. The data's end ends one too: c4lc leaves the last literal's NUL to
// the zeroed memory after it.
static int xm_cstr(struct xmod *m, int off) {
    int i, c;
    if (off < 0 || off >= m->ndata) return 0 - 1;
    i = off;
    while (i < m->ndata) {
        c = m->data[i] & 255;
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
static void xm_quote(struct xmod *m, char *d, int off, int n, int max) {
    int i, c, k;
    k = ui_len(d);
    d[k] = '"'; ++k;
    i = 0;
    while (i < n && k < max - 8) {
        c = m->data[off + i] & 255;
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
static int xp_text(struct xp *x, int k, char *mn, char *d) {
    int op, a, w, t, v, f, g, n, s, j;
    char *nm;
    struct xmod *m;
    m = x->cm;
    op = x->iop[k]; a = x->iarg[k]; w = x->ioff[k] + 1;
    d[0] = 0;
    if (op < 0) { ui_cpy(mn, ".word"); ui_num(d, m->code[x->ioff[k]]); return XC_NUM; }
    if (op < O_N) { memcpy(mn, opnames + op * 5, 4); mn[4] = 0; j = 3; while (j > 0 && mn[j] == ' ') { mn[j] = 0; --j; } }
    else { ui_cpy(mn, "?"); ui_num(mn + 1, op); }
    if (!xp_operand(op)) return XC_TEXT;
    t = m->ptype[w]; v = m->pval[w];
    if (t == P_CODE) {
        f = xm_fn_at(m, v);
        if ((op == O_JMP || op == O_BZ || op == O_BNZ) && f == x->gidx[x->cur]) {
            j = 0;
            while (j < x->nins) { if (x->ioff[j] == v && x->ilab[j]) { ui_cpy(d, "L"); ui_num(d + 1, x->ilab[j]); return XC_PRE; } ++j; }
            xp_hex(d, v); return XC_PRE;
        }
        if ((nm = xm_fn_named(m, v))) { if (op != O_JSR) ui_cpy(d, "&"); ui_cat(d, nm); return XC_TYPE; }
        if (f >= 0) { ui_cpy(d, m->fname[f]); ui_cat(d, "+"); ui_num(d + ui_len(d), v - m->fstart[f]); return XC_TYPE; }
        xp_hex(d, v); return XC_NUM;
    }
    if (t == P_DATA) {
        g = xm_global(m, v);
        n = xm_cstr(m, v);
        // a unit's literals follow its globals with no symbol of their own
        if (g >= 0 && n >= 2 && v - m->sval[g] >= m->W && v > 0 && m->data[v - 1] == 0) g = 0 - 1;
        if (g >= 0) {
            ui_cpy(d, op == O_JSRI ? "*" : "&");
            ui_cat(d, m->sname[g]);
            if (v != m->sval[g]) { ui_cat(d, "+"); ui_num(d + ui_len(d), v - m->sval[g]); }
            return XC_TYPE;
        }
        if (n >= 0) {
            if (n > 200) n = 200;
            xm_quote(m, d, v, n, 240);
            if (n == 200) ui_cat(d, "...");
            return XC_STR;
        }
        ui_cpy(d, "data+"); ui_num(d + 5, v); return XC_NUM;
    }
    if (t > 0) {
        nm = xm_symname(m, t);
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
static int xp_ow(struct xp *x) { int n, v; n = 1; v = x->cm ? x->cm->ncode >> 4 : 0; while (v) { ++n; v = v >> 4; } return n < 4 ? 4 : n; }
// "file:line" of map file F of module M
static void xp_where_s(struct xmod *m, char *d, int f, int l) {
    ui_cpy(d, m && f >= 0 && f < m->nfile ? ui_base(m->fpath[f]) : "?");
    ui_cat(d, ":");
    ui_num(d + ui_len(d), l);
}
// the code area's buttons: Wrap, Back, and -O for a compile
static int xp_bw(struct xp *x) { return x->objmode ? 76 : 116; }

// the list: the filter, its order, the names
static void xp_draw_list(struct xp *x, int ox, int oy, int w, int h, int *cols, int focused) {
    int rows, i, k, g, y, c, n;
    struct xmod *m;
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
        g = x->lvis[x->ltop + i];
        m = xp_mod(x, g);
        // a library's in a colour of its own; the file edited's own, and the rest dimmer
        c = x->gmod[g] > 0 ? cols[XC_TYPE] : !x->objmode && m->ffile[x->gidx[g]] != x->here ? cols[XC_COM] : cols[XC_TEXT];
        if (g == x->cur) { d_rect(ox, y + 2 + i * XCH, w - 17, XCH, cols[XC_SEL]); c = cols[XC_KW]; }
        n = ui_len(xp_name(x, g));
        d_textn(ox + 4, y + 2 + i * XCH, c, XFONT, F_MONO, XCW, xp_name(x, g), n);
        // a name the module has more than once: the unit each copy came with
        k = m->funit[x->gidx[g]];
        if (x->gdup[g] && k >= 0) d_text(ox + 4 + (n + 1) * XCW, y + 2 + i * XCH, cols[XC_COM], F_SANS, ui_base(m->fpath[k]));
        ++i;
    }
    if (x->ok && !x->nvis) d_text(ox + 4, y + 4, cols[XC_COM], F_SANS, "None by that name");
    d_clip_end();
    ui_scrollbar(ox + w - 17, y, h - XHDR, x->nvis, rows, x->ltop);
}

// the code: its header, then the listing
static void xp_draw_code(struct xp *x, int ox, int oy, int cw, int ch, int *cols) {
    char mn[8];
    int rows, y, k, j, c, i, sx, n, ow, c1, cm, ry, lw, bw, hot, hb, lh, cols0, f, a;
    struct xmod *m;
    m = x->cm;
    // the header: the function, where it is, Wrap, Back and -O
    bw = xp_bw(x);
    d_rect(ox, oy, cw, XHDR, C_FACE);
    d_bevel(ox, oy, cw, XHDR, B_RAISED);
    ui_cpy(xb, "Code");
    if (x->ok && x->cur >= 0) {
        f = x->gidx[x->cur];
        ui_cpy(xb, m->fname[f]);
        ui_cat(xb, " ("); ui_num(xb + ui_len(xb), m->flen[f]); ui_cat(xb, " words");
        if (m->ffile[f] >= 0 && m->fline[f] > 0) { ui_cat(xb, ", "); xp_where_s(m, xb + ui_len(xb), m->ffile[f], m->fline[f]); }
        else if (m->funit[f] >= 0) { ui_cat(xb, ", with "); ui_cat(xb, ui_base(m->fpath[m->funit[f]])); }
        if (x->gmod[x->cur] > 0) { ui_cat(xb, ", in "); ui_cat(xb, ui_base(m->path)); }
        ui_cat(xb, ")");
    }
    d_clip(ox, oy, cw - bw - 4, XHDR);
    d_text(ox + 6, oy + 5, C_DARK, F_SANS, xb);
    d_clip_end();
    ui_button(ox + cw - bw, oy + 2, 44, XHDR - 4, "Wrap", x->wrap, 0);
    ui_button(ox + cw - bw + 48, oy + 2, 26, XHDR - 4, "<", 0, 0);
    if (!x->objmode) ui_button(ox + cw - 40, oy + 2, 36, XHDR - 4, "-O", x->opt, 0);

    // the listing, laid out for its width
    lw = cw - 17;
    ow = xp_ow(x);
    c1 = ow + 3;                              // labels, and the source's text
    cols0 = (lw - 6) / XCW - c1;
    if (cols0 < 8) cols0 = 8;
    x->cols = (lw - 6) / XCW;
    if (x->ok && x->cur >= 0 && x->laycols != (x->wrap ? cols0 : 0 - 1)) {
        // laid out again: the view stays on the instruction at its top
        k = x->top; a = 0 - 1;
        while (x->dins && k < x->nd && a < 0) { if (x->dins[k] >= 0) a = x->dins[k]; ++k; }
        xp_layout(x, x->wrap ? cols0 : 0);
        if (a >= 0) { k = 0; while (k < x->nd && x->dins[k] != a) ++k; x->top = k; }
    }
    hb = !x->wrap;
    lh = ch - XHDR - (hb ? 16 : 0);
    rows = (lh - 4) / XCH;
    if (rows < 1) rows = 1;
    x->rows = rows;
    if (x->sel == 0 - 2) {
        // the cursor's line, from xp_cursor: into view
        x->sel = 0 - 1;
        k = xp_row_of(x, x->hline);
        if (k >= 0 && (k < x->top || k >= x->top + rows)) x->top = k - rows / 3;
    }
    if (x->top > x->nd - rows) x->top = x->nd - rows;
    if (x->top < 0) x->top = 0;
    if (x->left > x->maxw + c1 + 2 - x->cols) x->left = x->maxw + c1 + 2 - x->cols;
    if (x->left < 0 || x->wrap) x->left = 0;
    y = oy + XHDR;
    d_rect(ox, y, lw, lh, cols[XC_BG]);
    sx = ox + 6 - x->left * XCW;
    d_clip(ox, y, lw, lh);
    k = x->top; i = 0;
    while (x->ok && x->cur >= 0 && k < x->nd && i < rows) {
        ry = y + 2 + i * XCH;
        hot = x->dline[k] > 0 && x->gmod[x->cur] == x->hmod && x->dfile[k] == x->hfile && x->dline[k] == x->hline;
        if (x->dins[k] < 0) {
            // a line of source: its number where the offsets are, its text where the labels are
            d_rect(ox, ry, lw, XCH, hot ? cols[XC_SEL] : cols[XC_GUT]);
            d_rect(ox + 1, ry, 3, XCH, bands[x->dline[k] % 8]);
            if (x->dseg[k] == 0) {
                ui_num(xb, x->dline[k]);
                n = ui_len(xb);
                d_textn(sx + (ow + 1 - n) * XCW, ry, cols[XC_COM], XFONT, F_MONO, XCW, xb, n);
            }
            n = xp_line(x, m, x->dfile[k], x->dline[k]);
            if (n >= 0) {
                // a wrapped line's piece, or the whole line scrolled across
                a = x->wrap && x->laycols > 0 ? x->dseg[k] * x->laycols : 0;
                n = n - a;
                if (x->wrap && x->laycols > 0 && n > x->laycols) n = x->laycols;
                if (n > 0) d_textn(sx + c1 * XCW, ry, cols[XC_TEXT], XFONT, F_MONO, XCW, xo + a, n);
            }
            else { xp_where_s(m, xb, x->dfile[k], x->dline[k]); d_textn(sx + c1 * XCW, ry, cols[XC_COM], XFONT, F_MONO, XCW, xb, ui_len(xb)); }
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
            c = xp_text(x, j, mn, xb);
            d_textn(sx + cm * XCW, ry, cols[XC_KW], XFONT, F_MONO, XCW, mn, ui_len(mn));
            if (xb[0]) d_textn(sx + (cm + 5) * XCW, ry, cols[c], XFONT, F_MONO, XCW, xb, ui_len(xb));
        }
        if (k == x->sel) d_recto(ox, ry, lw, XCH, cols[XC_KW]);
        ++k; ++i;
    }
    d_clip_end();
    if (x->ok && x->cur < 0) d_text(ox + 6, y + 4, cols[XC_COM], F_SANS, "Pick a function from the list");
    ui_scrollbar(ox + cw - 17, y, lh, x->nd, rows, x->top);
    if (hb) {
        ui_hscrollbar(ox, y + lh, cw - 17, x->maxw + c1 + 2, x->cols, x->left);
        d_rect(ox + cw - 17, y + lh, 17, 16, C_FACE);
    }
    // a failed compile's last words, over the listing
    if (x->failed && !x->busy) {
        if (x->errn > 0) d_textn(ox + 6, y + 4, cols[XC_STR], XFONT, F_MONO, XCW, x->err0, ui_len(x->err0));
        if (x->errn > 1) d_textn(ox + 6, y + 4 + XCH, cols[XC_STR], XFONT, F_MONO, XCW, x->err1, ui_len(x->err1));
    }
}

// The tab at OX, OY, CW by CH: the list, the code, and a footer across
// both; for a text MODIFIED since it was saved, COLS the IDE's scheme
void xp_draw(int *xi, int ox, int oy, int cw, int ch, int *cols, int modified, int focused) {
    struct xp *x;
    char mn[8];
    int lw, h, y, k, j, f, i;
    struct xmod *m;
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
    m = x->cm;
    f = x->ok && x->cur >= 0 ? m->ffile[x->gidx[x->cur]] : 0 - 1;
    if (f >= 0) xp_source(x, m, f);
    if (x->busy || !x->ok) ui_cpy(xb, x->msg[0] ? x->msg : "Save the file, and the code it compiles to is shown here");
    else if (x->failed) ui_cpy(xb, x->msg);
    else if (x->note[0]) ui_cpy(xb, x->note);
    else if (x->sel >= 0 && x->sel < x->nd && x->dline[x->sel] > 0) {
        k = x->sel;
        xp_where_s(m, xb, x->dfile[k], x->dline[k]);
        if ((j = x->dins[k]) >= 0) { ui_cat(xb, "  "); xp_text(x, j, mn, xo); ui_cat(xb, mn); ui_cat(xb, " "); ui_ncat(xb, xo, 300); }
        else if (x->dfile[k] >= 0 && m->sst[x->dfile[k]] > 0) { ui_cat(xb, "  "); ui_ncat(xb, m->spath[x->dfile[k]], 300); }
    }
    else if (modified && !x->objmode) ui_cpy(xb, "Changed since it was compiled: save to see the new code");
    else if (f >= 0 && m->sst[f] < 0 && !xm_own(m, f)) { ui_cpy(xb, "No source on this disk for "); ui_ncat(xb, m->fpath[f], 300); }
    else if (f >= 0 && m->sst[f] == 2) { ui_ncpy(xb, m->spath[f], 200); ui_cat(xb, " has changed since this was compiled"); }
    else if (x->objmode) {
        m = x->mods[0];
        ui_cpy(xb, ui_base(x->path)); ui_cat(xb, ": ");
        ui_num(xb + ui_len(xb), m->nfn); ui_cat(xb, m->nfn == 1 ? " function, " : " functions, ");
        ui_num_commas(xb + ui_len(xb), m->ncode); ui_cat(xb, " words of code, ");
        if (m->nrow) { ui_cat(xb, "a map of "); ui_num(xb + ui_len(xb), m->nfile); ui_cat(xb, m->nfile == 1 ? " file" : " files"); }
        else ui_cat(xb, "no source map");
        i = 1;
        while (i < x->nmod) { ui_cat(xb, i == 1 ? "; loads " : ", "); ui_ncat(xb, ui_base(x->mods[i]->path), 300); ++i; }
    }
    else if (x->here < 0) ui_cpy(xb, "The map does not name this file");
    else ui_cpy(xb, x->opt ? "Compiled with -g -O" : "Compiled with -g");
    d_clip(ox, y, cw, XFOOT);
    d_text(ox + 4, y + 3, C_DARK, F_SANS, xb);
    d_clip_end();
}

// ---- the mouse and the keys -------------------------------------------------------------------

// Row K followed: a call (or a function's address) goes to the callee, in
// this tab, in a library if that is where it is; anything else with a
// line, to the line, in the editor (2)
static int xp_follow(struct xp *x, int k) {
    int j, t, v, f, d, g;
    char *nm;
    struct xmod *m;
    m = x->cm;
    j = x->dins[k];
    if (j >= 0 && x->iop[j] >= 0 && xp_operand(x->iop[j])) {
        t = m->ptype[x->ioff[j] + 1]; v = m->pval[x->ioff[j] + 1];
        g = 0 - 1;
        if (t == P_CODE) { f = xm_fn_at(m, v); if (f >= 0 && m->fstart[f] == v) g = m->fn0 + f; }
        else if (t > 0 && (nm = xm_symname(m, t))) {
            // an extern: another module's, the libraries' first
            g = xp_named(x, nm, x->gmod[x->cur]);
            if (g < 0) { ui_cpy(x->note, nm); ui_cat(x->note, " is not in this file or the libraries it loads: it is linked in from elsewhere"); return 1; }
        }
        if (g >= 0 && g != x->cur) { xp_show(x, g, 1); return 1; }
    }
    d = x->dfile[k];
    if (x->dline[k] <= 0 || d < 0) return 1;
    xp_source(x, m, d);
    if (m->sst[d] <= 0) { ui_cpy(x->note, "No source on this disk for "); ui_ncat(x->note, m->fpath[d], 150); return 1; }
    ui_ncpy(x->gopath, m->spath[d], 128);
    x->goline = x->dline[k] + m->soff[d];
    if (m == x->mods[0] && d == x->here) x->lastline = x->goline;   // the editor's cursor is going there: nothing to follow
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
        t = ui_scroll_click(px, py, w - 17, XHDR, h - XHDR, x->nvis, x->lrows, x->ltop);
        if (t >= 0) x->ltop = t;
        return 1;
    }
    k = x->ltop + (py - XHDR - 2) / XCH;
    if (x->ok && k >= 0 && k < x->nvis) xp_show(x, x->lvis[k], 1);
    return 1;
}

static int xp_mouse_code(struct xp *x, int ev, int px, int py, int cw, int ch) {
    int t, k, bw, lh;
    lh = ch - XHDR - (x->wrap ? 0 : 16);
    if (py < XHDR) {
        if (ev != E_DOWN) return 0;
        bw = xp_bw(x);
        if (ui_hit(px, py, cw - bw, 2, 44, XHDR - 4)) { x->wrap = !x->wrap; x->left = 0; return 1; }
        if (ui_hit(px, py, cw - bw + 48, 2, 26, XHDR - 4)) { xp_back(x); return 1; }
        if (!x->objmode && ui_hit(px, py, cw - 40, 2, 36, XHDR - 4)) {
            x->opt = !x->opt;
            if (x->path[0]) xp_compile((int *)x, x->path, x->dir);
            return 0 - 1;
        }
        return 0;
    }
    if (px >= cw - 17) {
        t = ui_scroll_click(px, py, cw - 17, XHDR, lh, x->nd, x->rows, x->top);
        if (t >= 0) x->top = t;
        return 1;
    }
    if (py >= XHDR + lh) {
        t = ui_hscroll_click(px, py, 0, XHDR + lh, cw - 17, x->maxw + xp_ow(x) + 5, x->cols, x->left);
        if (t >= 0) x->left = t;
        return 1;
    }
    if (!x->ok || x->cur < 0) return 0;
    k = x->top + (py - XHDR - 2) / XCH;
    if (k < 0 || k >= x->nd) return 0;
    x->sel = k;
    if (x->dline[k] > 0) { x->hmod = x->gmod[x->cur]; x->hfile = x->dfile[k]; x->hline = x->dline[k]; }
    if (ev == E_DBL) return xp_follow(x, k);
    return 1;
}

// A mouse event at PX, PY in the tab, CW by CH (a wheel's PY is how far,
// and PX where): 1 if it did something, 0 if not, 2 for a line to go to
// in the editor (xp_goto), -1 when -O was turned on or off (and it
// compiles again), -2 while a bar is dragged
int xp_mouse(int *xi, int ev, int px, int py, int cw, int ch) {
    struct xp *x;
    int lw, h, t, ccw, lh;
    x = X(xi);
    lw = xp_lw(cw);
    h = ch - XFOOT;
    ccw = cw - lw - 2;                        // the code's width, right of the list
    lh = h - XHDR - (x->wrap ? 0 : 16);
    x->note[0] = 0;
    if (ev == E_WHEEL) { if (px < lw) x->ltop = x->ltop + py * 3; else x->top = x->top + py * 3; return 1; }
    if (ev == E_MOVE) {
        // a thumb being dragged, named by where its bar is, as the click named it
        t = ui_scroll_drag(lw - 17, XHDR, py);
        if (t >= 0) { x->ltop = t; return 0 - 2; }
        t = ui_scroll_drag(ccw - 17, XHDR, py);
        if (t >= 0) { x->top = t; return 0 - 2; }
        t = ui_scroll_drag(0, XHDR + lh, px - lw - 2);
        if (t >= 0) { x->left = t; return 0 - 2; }
        return 0;
    }
    if ((ev != E_DOWN && ev != E_DBL) || py >= h) return 0;
    if (px < lw) return xp_mouse_list(x, ev, px, py, lw, h);
    if (px < lw + 2) return 0;
    return xp_mouse_code(x, ev, px - lw - 2, py, ccw, h);
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
