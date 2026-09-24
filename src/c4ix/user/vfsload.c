// C4IX userland: boot-time filesystem loader.
//
// Same manifest format and job as C4KE's vfsload.c (src/c4ke/bin/
// vfsload.c has the full grammar reference), but targets C4IX's real
// hierarchical VFS instead of C4KE's flat ramfs: directories are
// created with umkdir() as the manifest tree is walked (parents
// always exist by the time a child directory line is reached, since
// the parser is a top-down recursive descent over the same
// structure), and each file is a real RAM file - uopen(path,
// O_CREATE) then a copy loop from the real host file, opened the
// same way C4KE's loader does (uopen falls back to a host open()
// when the path isn't already in the VFS tree - see sys_open's
// comment in sys.c). ilink(path) entries are resolved the same way:
// after every real file has loaded, read the target back through the
// VFS (now populated) and write a second copy under the alias name.
//
// Run once at boot by init.c before the shell starts.

#include "c4ix_user.h"

enum {
    MAX_LINES   = 4000,
    MAX_VARS    = 32,
    MAX_WORDS   = 64,
    MAX_ENTRIES = 300,
    MAX_SUBST   = 8,
    READ_CAP    = 262144,
    SUBST_SLOP  = 512,
};

// c4lc/c4cc share a lexer quirk where '\t' and '\r' char literals
// come out as 8 and 10, not 9 and 13 - use numeric constants.
enum { TAB = 9, CR = 13 };

// KNOWN ISSUE: roughly 1 run in a few, exactly one entry's value
// comes back empty or garbled by the time load_entries() reads it
// back, even though it was stored correctly (confirmed by tracing).
// It heals when unrelated debug prints are added, which only shift
// timing - the signature of a race, not a logic bug in this file.
// Masking the cycle-interrupt-interval device register (0x154, the
// same one hw/microcode.uc's trap routine and fw.c's allocator use;
// ordinary memory access is never protected-mode gated, so userland
// can poke it directly) around this whole program's run was tried as
// a fix and made things reliably WORSE (5/5 failures instead of an
// intermittent one), so something else depends on preemption staying
// live even during this boot-time task - not yet understood. Left
// unmasked and undiagnosed rather than guessed at further; see
// [[c4bb-breadboard-computer]] memory / docs/c4bb-design.md for the
// related, likely-connected C4IX `top` crash this shares a root cause
// with.

char **g_line_text;
int   *g_line_indent;
int    g_nlines;
int    g_cursor;

char **g_var_name;
char **g_var_words;
int   *g_var_wordcount;
int    g_nvars;

char **g_subst;
int    g_subst_depth;

char **g_entry_path;
char **g_entry_value;
int   *g_entry_is_ilink;
int    g_nentries;

///
/// string helpers
///

char *str_dup_n(char *s, int n) {
    char *r;
    r = (char *)ualloc(n + 1);
    if (!r) return 0;
    memcpy(r, s, n);
    r[n] = 0;
    return r;
}
char *str_dup(char *s) { return str_dup_n(s, ustrlen(s)); }

char *str_cat(char *a, char *b) {
    int la, lb;
    char *r;
    la = ustrlen(a); lb = ustrlen(b);
    r = (char *)ualloc(la + lb + 1);
    if (!r) return 0;
    memcpy(r, a, la);
    memcpy(r + la, b, lb);
    r[la + lb] = 0;
    return r;
}

char *trim(char *s) {
    char *end;
    while (*s == ' ' || *s == TAB) ++s;
    if (!*s) return s;
    end = s + ustrlen(s) - 1;
    while (end > s && (*end == ' ' || *end == TAB || *end == CR)) { *end = 0; --end; }
    return s;
}

int starts_with(char *s, char *pfx) {
    while (*pfx) { if (*s != *pfx) return 0; ++s; ++pfx; }
    return 1;
}
int ends_with(char *s, char *sfx) {
    int ls, lf;
    ls = ustrlen(s); lf = ustrlen(sfx);
    if (lf > ls) return 0;
    return !memcmp(s + ls - lf, sfx, lf);
}

int find_top_colon(char *s) {
    int i, depth;
    i = 0; depth = 0;
    while (s[i]) {
        if (s[i] == '(') ++depth;
        else if (s[i] == ')') --depth;
        else if (s[i] == ':' && depth == 0) return i;
        ++i;
    }
    return -1;
}

char *paren_inner(char *s, char *name) {
    int nlen, slen;
    nlen = ustrlen(name); slen = ustrlen(s);
    if (!starts_with(s, name) || s[nlen] != '(' || s[slen - 1] != ')') return 0;
    return trim(str_dup_n(s + nlen + 1, slen - nlen - 2));
}

char *substitute(char *s) {
    char *out;
    int cap, len, i, n, wl;
    cap = ustrlen(s) + SUBST_SLOP;
    out = (char *)ualloc(cap);
    if (!out) return str_dup(s);
    len = 0; i = 0;
    while (s[i] && len < cap - 1) {
        if (s[i] == '$' && s[i + 1] >= '1' && s[i + 1] <= '9') {
            n = s[i + 1] - '0';
            if (n <= g_subst_depth && g_subst[n - 1]) {
                wl = ustrlen(g_subst[n - 1]);
                if (wl > cap - 1 - len) wl = cap - 1 - len;
                memcpy(out + len, g_subst[n - 1], wl);
                len = len + wl;
                i = i + 2;
                continue;
            }
        }
        out[len++] = s[i++];
    }
    out[len] = 0;
    return out;
}

///
/// I/O: uopen/uread walk the SAME open() sys_open falls back to a
/// real host file for, so a bare host filename here reads the actual
/// disk (never the VFS tree we are about to populate)
///

char *read_whole_file(char *name, int *plen) {
    int fd, n;
    char *buf;
    fd = uopen(name, O_RD);
    if (fd < 0) { uprintf("vfsload: cannot open %s\n", name); return 0; }
    buf = (char *)ualloc(READ_CAP);
    if (!buf) { uclose(fd); return 0; }
    n = uread(fd, buf, READ_CAP);
    uclose(fd);
    if (n < 0) { uprintf("vfsload: read failed on %s\n", name); return 0; }
    *plen = n;
    return buf;
}

int write_vfs_file(char *path, char *buf, int len) {
    int fd, n;
    fd = uopen(path, O_WR + O_CREATE + O_TRUNCATE);
    if (fd < 0) return -1;
    n = write(fd, buf, len);
    uclose(fd);
    return (n == len) ? 0 : -1;
}

///
/// manifest splitting (identical to C4KE's vfsload.c)
///

void split_lines(char *raw) {
    char *p, *lineStart;
    int indent;

    g_line_text = (char **)ualloc(MAX_LINES * sizeof(int));
    g_line_indent = (int *)ualloc(MAX_LINES * sizeof(int));
    g_nlines = 0;

    p = raw;
    while (*p) {
        char *h, *text, *q;
        lineStart = p;
        while (*p && *p != '\n') ++p;
        if (*p == '\n') { *p = 0; ++p; }

        h = lineStart;
        while (*h) { if (*h == '#') { *h = 0; break; } ++h; }

        indent = 0;
        q = lineStart;
        while (*q == ' ' || *q == TAB) { ++indent; ++q; }

        text = trim(lineStart);
        if (!*text) continue;
        if (g_nlines >= MAX_LINES) { uprintf("vfsload: manifest too long, truncating\n"); return; }
        g_line_text[g_nlines] = text;
        g_line_indent[g_nlines] = indent;
        ++g_nlines;
    }
}

int line_is_var_def(char *l, int *outEq) {
    int i;
    i = 0;
    while (l[i] && l[i] != '=' && l[i] != ':') ++i;
    if (l[i] != '=') return 0;
    *outEq = i;
    return 1;
}

void split_words_into(char *s, char *name);   // forward: mutually used below
int var_lookup(char *name);

void add_word(int v, char *word) {
    if (g_var_wordcount[v] < MAX_WORDS) {
        g_var_words[v * MAX_WORDS + g_var_wordcount[v]] = word;
        ++g_var_wordcount[v];
    }
}

void split_words_into(char *s, char *name) {
    int v, j, found, ov;
    char *p, *w, *tok;

    if (g_nvars >= MAX_VARS) { uprintf("vfsload: too many variables\n"); return; }
    v = g_nvars++;
    g_var_name[v] = str_dup(name);
    g_var_wordcount[v] = 0;

    p = s;
    while (*p) {
        while (*p == ' ' || *p == TAB) ++p;
        if (!*p) break;
        w = p;
        while (*p && *p != ' ' && *p != TAB) ++p;
        tok = str_dup_n(w, p - w);
        if (starts_with(tok, "$(") && ends_with(tok, ")")) {
            found = var_lookup(str_dup_n(tok + 2, ustrlen(tok) - 3));
            if (found) {
                ov = found - 1;
                j = 0;
                while (j < g_var_wordcount[ov]) { add_word(v, g_var_words[ov * MAX_WORDS + j]); ++j; }
            } else {
                uprintf("vfsload: %s references undefined variable %s\n", name, tok);
            }
        } else {
            add_word(v, tok);
        }
        if (*p) ++p;
    }
}

void extract_vars() {
    int src, dst, eq;

    g_var_name = (char **)ualloc(MAX_VARS * sizeof(int));
    g_var_words = (char **)ualloc(MAX_VARS * MAX_WORDS * sizeof(int));
    g_var_wordcount = (int *)ualloc(MAX_VARS * sizeof(int));
    g_nvars = 0;

    src = 0; dst = 0;
    while (src < g_nlines) {
        if (g_line_indent[src] == 0 && line_is_var_def(g_line_text[src], &eq)) {
            char *name, *value, *valBuf;
            int valCap, valLen, wl, hasCont;

            name = trim(str_dup_n(g_line_text[src], eq));
            value = trim(str_dup(g_line_text[src] + eq + 1));

            valCap = ustrlen(value) + 256;
            valBuf = (char *)ualloc(valCap);
            valLen = 0;
            for (;;) {
                hasCont = ends_with(value, "\\");
                wl = ustrlen(value) - (hasCont ? 1 : 0);
                if (wl > 0) {
                    if (valLen + wl + 1 > valCap) wl = valCap - valLen - 1;
                    if (wl > 0) { memcpy(valBuf + valLen, value, wl); valLen = valLen + wl; valBuf[valLen++] = ' '; }
                }
                ++src;
                if (!hasCont || src >= g_nlines) break;
                value = trim(str_dup(g_line_text[src]));
            }
            valBuf[valLen] = 0;
            split_words_into(trim(valBuf), name);
        } else {
            g_line_text[dst] = g_line_text[src];
            g_line_indent[dst] = g_line_indent[src];
            ++dst; ++src;
        }
    }
    g_nlines = dst;
}

int var_lookup(char *name) {
    int i;
    i = 0;
    while (i < g_nvars) { if (!ustrcmp(g_var_name[i], name)) return i + 1; ++i; }
    return 0;
}

///
/// the tree parser: directories are created HERE, as encountered, so
/// a child directory's umkdir() always runs after its parent's
///

void add_entry(char *path, char *value, int isIlink) {
    if (g_nentries >= MAX_ENTRIES) { uprintf("vfsload: too many entries, dropping %s\n", path); return; }
    g_entry_path[g_nentries] = path;
    g_entry_value[g_nentries] = value;
    g_entry_is_ilink[g_nentries] = isIlink;
    ++g_nentries;
}

void emit_value(char *path, char *value) {
    char *inner;
    inner = paren_inner(value, "ilink");
    if (inner) { add_entry(path, inner, 1); return; }
    inner = paren_inner(value, "file");
    if (inner) {
        int comma, i;
        comma = -1;
        i = 0;
        while (inner[i]) { if (inner[i] == ',') { comma = i; break; } ++i; }
        if (comma >= 0) add_entry(path, trim(str_dup(trim(str_dup_n(inner + comma + 1, ustrlen(inner) - comma - 1)))), 0);
        else add_entry(path, inner, 0);
        return;
    }
    add_entry(path, value, 0);
}

void parse_block(int parentIndent, char *pathPrefix);

void handle_each(char *keyExpr, char *valuePattern, int indent, char *pathPrefix) {
    char *argText, *v;
    int nwords, i, blockStart, found, vi;

    argText = paren_inner(keyExpr, "each");
    if (!argText) { uprintf("vfsload: malformed each() near '%s'\n", keyExpr); ++g_cursor; return; }

    if (*argText == '$') {
        found = var_lookup(argText + 1);
        if (!found) { uprintf("vfsload: unknown variable %s\n", argText); ++g_cursor; return; }
        vi = found - 1;
        nwords = g_var_wordcount[vi];
    } else {
        split_words_into(argText, "$each$");
        vi = g_nvars - 1;
        nwords = g_var_wordcount[vi];
    }

    if (*valuePattern) {
        ++g_cursor;
        i = 0;
        while (i < nwords) {
            char *word, *pattern, *key, *val;
            int c;
            word = g_var_words[vi * MAX_WORDS + i];
            if (g_subst_depth < MAX_SUBST) g_subst[g_subst_depth++] = word;
            pattern = substitute(valuePattern);
            c = find_top_colon(pattern);
            if (c >= 0) { key = trim(str_dup_n(pattern, c)); val = trim(str_dup(pattern + c + 1)); }
            else { key = pattern; val = pattern; }
            emit_value(str_cat(pathPrefix, key), val);
            --g_subst_depth;
            ++i;
        }
    } else {
        ++g_cursor;
        blockStart = g_cursor;
        i = 0;
        while (i < nwords) {
            char *word;
            word = g_var_words[vi * MAX_WORDS + i];
            if (g_subst_depth < MAX_SUBST) g_subst[g_subst_depth++] = word;
            g_cursor = blockStart;
            parse_block(indent, pathPrefix);
            --g_subst_depth;
            ++i;
        }
        if (nwords == 0) {
            g_cursor = blockStart;
            while (g_cursor < g_nlines && g_line_indent[g_cursor] > indent) ++g_cursor;
        }
    }
}

void process_line(char *pathPrefix) {
    char *line, *rawKey, *rawVal, *key, *value;
    int indent, c;

    indent = g_line_indent[g_cursor];
    line = g_line_text[g_cursor];
    c = find_top_colon(line);
    if (c >= 0) { rawKey = trim(str_dup_n(line, c)); rawVal = trim(str_dup(line + c + 1)); }
    else { rawKey = trim(str_dup(line)); rawVal = ""; }

    if (starts_with(rawKey, "each(") && ends_with(rawKey, ")")) {
        handle_each(rawKey, rawVal, indent, pathPrefix);
        return;
    }

    key = substitute(rawKey);
    if (ends_with(key, "/")) {
        char *path;
        ++g_cursor;
        path = str_cat(pathPrefix, key);
        // root always exists; every other directory is created right
        // as it's encountered, so its parent is always already there
        if (ustrcmp(path, "/")) umkdir(path);
        parse_block(indent, path);
        return;
    }

    ++g_cursor;
    value = *rawVal ? substitute(rawVal) : key;
    emit_value(str_cat(pathPrefix, key), value);
}

void parse_block(int parentIndent, char *pathPrefix) {
    while (g_cursor < g_nlines && g_line_indent[g_cursor] > parentIndent) {
        process_line(pathPrefix);
    }
}

///
/// resolution: real files first (their vfs paths must exist before
/// any ilink can read them back), then ilinks
///

// ---- lazy entries ----
//
// With c4ix.sizes on the disk (a line "SIZE NAME" per host file, written
// by the image build), an entry is not copied at all: it becomes a lazy
// RAM file (ulazyfile) that the kernel reads in from the host the first
// time something uses it. A boot then reads nothing it does not run. An
// entry the table does not list is copied as before, and with no table
// at all everything is.

char *g_sizes;
int g_sizes_len;

// the size the table gives for host file NAME, or -1
int size_of(char *name) {
    int i, n, v, k;
    if (!g_sizes) return 0 - 1;
    n = ustrlen(name);
    i = 0;
    while (i < g_sizes_len) {
        v = 0;
        while (i < g_sizes_len && g_sizes[i] >= '0' && g_sizes[i] <= '9') { v = v * 10 + (g_sizes[i] - '0'); ++i; }
        if (i < g_sizes_len && g_sizes[i] == ' ') ++i;
        k = 0;
        while (k < n && i + k < g_sizes_len && g_sizes[i + k] == name[k]) ++k;
        if (k == n && (i + k >= g_sizes_len || g_sizes[i + k] == 10)) return v;
        while (i < g_sizes_len && g_sizes[i] != 10) ++i;
        ++i;
    }
    return 0 - 1;
}

void load_sizes() {
    int fd, n;
    g_sizes = 0;
    if ((fd = uopen("c4ix.sizes", O_RD)) < 0) return;
    g_sizes = (char *)ualloc(65536);
    n = uread(fd, g_sizes, 65535);
    uclose(fd);
    if (n <= 0) { g_sizes = 0; return; }
    g_sizes_len = n;
}

// the host name behind a vfs path an earlier entry made, or 0
char *host_of(char *path) {
    int i;
    i = 0;
    while (i < g_nentries) {
        if (g_entry_is_ilink[i] != 1 && !ustrcmp(g_entry_path[i], path)) return g_entry_value[i];
        ++i;
    }
    return 0;
}

int load_entries() {
    int i, ok, rlen, ilFd, ilLen, sz, lazy;
    char *rbuf, *ilBuf, *host;

    ok = 0; lazy = 0;
    i = 0;
    while (i < g_nentries) {
        if (!g_entry_is_ilink[i] && (sz = size_of(g_entry_value[i])) >= 0 &&
            ulazyfile(g_entry_path[i], g_entry_value[i], sz) == 0) { ++ok; ++lazy; g_entry_is_ilink[i] = 2; }
        ++i;
    }
    i = 0;
    while (i < g_nentries) {
        if (!g_entry_is_ilink[i]) {
            rbuf = read_whole_file(g_entry_value[i], &rlen);
            if (rbuf) {
                if (!write_vfs_file(g_entry_path[i], rbuf, rlen)) ++ok;
                else uprintf("vfsload: write failed for %s\n", g_entry_path[i]);
            }
        }
        ++i;
    }
    i = 0;
    while (i < g_nentries) {
        // an alias of a lazy file is another lazy file on the same host bytes
        if (g_entry_is_ilink[i] == 1 && (host = host_of(g_entry_value[i])) && (sz = size_of(host)) >= 0 &&
            ulazyfile(g_entry_path[i], host, sz) == 0) { ++ok; ++lazy; }
        else if (g_entry_is_ilink[i] == 1) {
            ilFd = uopen(g_entry_value[i], O_RD);
            if (ilFd < 0) {
                uprintf("vfsload: ilink %s -> %s: target not found\n", g_entry_path[i], g_entry_value[i]);
            } else {
                ilBuf = (char *)ualloc(READ_CAP);
                ilLen = uread(ilFd, ilBuf, READ_CAP);
                uclose(ilFd);
                if (ilLen >= 0 && !write_vfs_file(g_entry_path[i], ilBuf, ilLen)) ++ok;
            }
        }
        ++i;
    }
    if (lazy) uprintf("vfsload: %d entries left on the host until first use\n", lazy);
    return ok;
}

int main(int argc, char **argv) {
    char *manifest, *manifestName;
    int len, ok;

    manifestName = (argc > 1) ? argv[1] : "c4ix.vfs.txt";
    manifest = read_whole_file(manifestName, &len);
    if (!manifest) {
        uprintf("vfsload: no manifest (%s); vfs left at defaults\n", manifestName);
        return 1;
    }

    g_entry_path = (char **)ualloc(MAX_ENTRIES * sizeof(int));
    g_entry_value = (char **)ualloc(MAX_ENTRIES * sizeof(int));
    g_entry_is_ilink = (int *)ualloc(MAX_ENTRIES * sizeof(int));
    g_nentries = 0;
    g_subst = (char **)ualloc(MAX_SUBST * sizeof(int));
    g_subst_depth = 0;

    load_sizes();
    split_lines(manifest);
    extract_vars();

    g_cursor = 0;
    parse_block(-1, "");

    ok = load_entries();
    uprintf("vfsload: %d/%d entries loaded from %s\n", ok, g_nentries, manifestName);
    return 0;
}
