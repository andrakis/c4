// C4KE boot-time filesystem loader.
//
// Reads a small declarative manifest (c4ke.vfs.txt, format documented
// in that file) from disk and populates the kernel's RAM filesystem
// (OP_VFS_*, backed by ramfs_* in c4ke.c) so ls/cat have something
// real to show. Runs once at boot (see init.c) as an ordinary task:
// vfs_put() writes into the KERNEL's shared ramfs regardless of which
// process calls it, so nothing further needs to happen afterward.
//
// Implements:
//   NAME = word word word \        variable definition, continued
//                                  across lines by a trailing '\'
//   path/:                        enter a directory - contributes
//                                  "path/" to every descendant's full
//                                  vfs name (a trailing ':' and any
//                                  text after it are both optional
//                                  and, when present, ignored)
//   key: value                    a file: value is a host disk
//                                  filename, unless it names file(...)
//                                  or ilink(...)
//   key                           shorthand for "key: key"
//   each(WORDS):                  block form - repeats the indented
//                                  body once per word, substituting
//                                  $1 (or $2, $3... at deeper nesting)
//   each(WORDS): pattern          single-line form - one entry per
//                                  word, key == value == pattern with
//                                  $N substituted (pattern may itself
//                                  contain "key: value" to split them)
//   ilink(path)                   alias: same content as whatever
//                                  "path" resolves to elsewhere in
//                                  the tree - order-independent, since
//                                  every ilink is resolved only after
//                                  every real file is loaded
//   # comment                     to end of line, stripped first
//
// file(a[,b]) is accepted (b, or a if b is absent, is the host name)
// but is not exercised by the real manifest. WORDS in each(...) is
// either a $VARIABLE reference or literal whitespace-separated words.
//
// Not a general-purpose config language: capacities below are fixed
// and generous for a file describing this repository, not unbounded
// input. This is boot-time, one-shot code - clarity over economy.

#include <u0.h>

// u0.h's strlen/strcmp are #define-renamed to __c4_*, which the c4lc
// preprocessor does not resolve across this file's own calls to them
// (c4ke.c avoids the same trap with its own ramfs_strlen); side-step
// it entirely with plain local helpers.
int vlen (char *s) { char *t; t = s; while (*t) ++t; return t - s; }
int vstrcmp (char *a, char *b) { while (*a && *a == *b) { ++a; ++b; } return *a - *b; }

// c4lc/c4cc share a lexer quirk where '\t' and '\r' char literals
// come out as 8 and 10 (not 9 and 13) - use numeric constants for
// the real bytes instead of fighting it.
enum { TAB = 9, CR = 13 };

enum {
	MAX_LINES   = 4000,
	MAX_VARS    = 32,
	MAX_WORDS   = 64,     // per variable
	MAX_ENTRIES = 300,    // must comfortably clear RAMFS_MAX (256)
	MAX_SUBST   = 8,
	READ_CAP    = 262144, // scratch buffer for any single host file
	SUBST_SLOP  = 512,    // extra room substitute() reserves per call
};

char *g_buf;              // whole manifest, line-split in place

char **g_line_text;
int   *g_line_indent;
int    g_nlines;
int    g_cursor;

char **g_var_name;
char **g_var_words;       // flat MAX_VARS*MAX_WORDS, index v*MAX_WORDS+w
int   *g_var_wordcount;
int    g_nvars;

char **g_subst;
int    g_subst_depth;

char **g_entry_path;
char **g_entry_value;     // host filename, or (for ilinks) the target path
int   *g_entry_is_ilink;
int    g_nentries;

///
/// string helpers
///

char *str_dup_n (char *s, int n) {
	char *r;
	if (!(r = malloc(n + 1))) return 0;
	memcpy(r, s, n);
	r[n] = 0;
	return r;
}
char *str_dup (char *s) { return str_dup_n(s, vlen(s)); }

char *str_cat (char *a, char *b) {
	int la, lb;
	char *r;
	la = vlen(a); lb = vlen(b);
	if (!(r = malloc(la + lb + 1))) return 0;
	memcpy(r, a, la);
	memcpy(r + la, b, lb);
	r[la + lb] = 0;
	return r;
}

// Trims leading/trailing spaces, tabs, and CR in place; returns the
// (possibly advanced) start pointer. Safe to call on any line slice.
char *trim (char *s) {
	char *end;
	while (*s == ' ' || *s == TAB) ++s;
	if (!*s) return s;
	end = s + vlen(s) - 1;
	while (end > s && (*end == ' ' || *end == TAB || *end == CR)) { *end = 0; --end; }
	return s;
}

int starts_with (char *s, char *pfx) {
	while (*pfx) { if (*s != *pfx) return 0; ++s; ++pfx; }
	return 1;
}
int ends_with (char *s, char *sfx) {
	int ls, lf;
	ls = vlen(s); lf = vlen(sfx);
	if (lf > ls) return 0;
	return !memcmp(s + ls - lf, sfx, lf);
}

// index of the first ':' at paren-depth 0, or -1
int find_top_colon (char *s) {
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

// Everything between the outermost parens of "name(...)", trimmed.
// Returns 0 if s does not look like a call.
char *paren_inner (char *s, char *name) {
	int nlen, slen;
	nlen = vlen(name); slen = vlen(s);
	if (!starts_with(s, name) || s[nlen] != '(' || s[slen - 1] != ')') return 0;
	return trim(str_dup_n(s + nlen + 1, slen - nlen - 2));
}

// Replaces $1..$9 with the current substitution stack, deepest slot
// first (slot N = g_subst[N-1]). Anything beyond the reserved slop is
// silently truncated rather than overflowing - the manifest we ship
// never comes close.
char *substitute (char *s) {
	char *out;
	int cap, len, i, n, wl;
	cap = vlen(s) + SUBST_SLOP;
	if (!(out = malloc(cap))) return str_dup(s);
	len = 0; i = 0;
	while (s[i] && len < cap - 1) {
		if (s[i] == '$' && s[i + 1] >= '1' && s[i + 1] <= '9') {
			n = s[i + 1] - '0';
			if (n <= g_subst_depth && g_subst[n - 1]) {
				wl = vlen(g_subst[n - 1]);
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
/// file I/O against the c4bb disk controller (real OPEN/READ/CLOS -
/// the same device C4IX uses; not the ramfs we are populating)
///

char *read_whole_file (char *name, int *plen) {
	int fd, n;
	char *buf;
	if ((fd = open(name, 0)) < 0) { printf("vfsload: cannot open %s\n", name); return 0; }
	// +1 and a NUL, because the only consumer -- split_lines() -- walks
	// this with `while (*p)` and never looks at *plen. Without the
	// terminator the parser runs off the end of the file into whatever
	// the heap happened to hold, and treats it as more manifest. That
	// is not theoretical: booting C4KE nested under c4m on c4bb, right
	// after BUILD had unpacked c4ke-src.tar and compiled a kernel, the
	// bytes past the manifest were C SOURCE, and vfsload dutifully
	// reported `too many entries, dropping free(custom_opcodes);` about
	// a hundred times. Natively the next byte happened to be zero, so
	// the same bug read as `55/55 entries loaded` -- it was always
	// there, it just had a tidier heap to run into.
	if (!(buf = malloc(READ_CAP + 1))) { close(fd); return 0; }
	n = read(fd, buf, READ_CAP);
	close(fd);
	if (n < 0) { printf("vfsload: read failed on %s\n", name); free(buf); return 0; }
	buf[n] = 0;
	*plen = n;
	return buf;
}

///
/// manifest loading: split into (indent, text) lines, comments and
/// blank lines dropped, tabs counted as one indent column each
///

void split_lines (char *raw) {
	char *p, *lineStart;
	int indent;

	g_line_text = malloc(MAX_LINES * sizeof(int));
	g_line_indent = malloc(MAX_LINES * sizeof(int));
	g_nlines = 0;

	p = raw;
	while (*p) {
		lineStart = p;
		while (*p && *p != '\n') ++p;
		if (*p == '\n') { *p = 0; ++p; }

		// strip a comment (# to end of line); simple, no quoting needed
		{
			char *h;
			h = lineStart;
			while (*h) { if (*h == '#') { *h = 0; break; } ++h; }
		}

		indent = 0;
		{
			char *q;
			q = lineStart;
			while (*q == ' ' || *q == TAB) { ++indent; ++q; }
		}

		{
			char *text;
			text = trim(lineStart);
			if (!*text) continue;               // blank (or comment-only) line
			if (g_nlines >= MAX_LINES) { printf("vfsload: manifest too long, truncating\n"); return; }
			g_line_text[g_nlines] = text;
			g_line_indent[g_nlines] = indent;
			++g_nlines;
		}
	}
}

///
/// variables: NAME = word word \<newline>continued...
/// Runs AFTER split_lines(), over its already comment-stripped,
/// trimmed, indent-tagged line array - not the raw buffer - so there
/// is exactly one notion of "the line list" and no manual re-splicing
/// of the character buffer.
///

int line_is_var_def (char *l, int *outEq) {
	int i;
	i = 0;
	while (l[i] && l[i] != '=' && l[i] != ':') ++i;
	if (l[i] != '=') return 0;
	*outEq = i;
	return 1;
}

int var_lookup (char *name);

void add_word (int v, char *word) {
	if (g_var_wordcount[v] < MAX_WORDS) {
		g_var_words[v * MAX_WORDS + g_var_wordcount[v]] = word;
		++g_var_wordcount[v];
	}
}

// Splits s on whitespace into variable `name`'s word list. A token of
// the form $(OTHER) - the make-style reference BIN_ALL = $(BIN_CORE)
// $(BIN_UTIL) ... uses to combine variables, distinct from each($X)'s
// bare $X - splices in OTHER's own words in place of the token;
// OTHER must already be defined, which every real use satisfies since
// variables are expanded in file order.
void split_words_into (char *s, char *name) {
	int v, j, found, ov;
	char *p, *w, *tok;

	if (g_nvars >= MAX_VARS) { printf("vfsload: too many variables\n"); return; }
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
			found = var_lookup(str_dup_n(tok + 2, vlen(tok) - 3));
			if (found) {
				ov = found - 1;
				j = 0;
				while (j < g_var_wordcount[ov]) { add_word(v, g_var_words[ov * MAX_WORDS + j]); ++j; }
			} else {
				printf("vfsload: %s references undefined variable %s\n", name, tok);
			}
		} else {
			add_word(v, tok);
		}
		if (*p) ++p;
	}
}

// Scans g_line_text for "NAME = ..." definitions at indent 0,
// consuming continuation lines (marked by a trailing '\'). Consumed
// lines are compacted out of the line array in a single src/dst pass
// so the tree parser (which runs after this) never sees them.
void extract_vars () {
	int src, dst, eq;

	g_var_name = malloc(MAX_VARS * sizeof(int));
	g_var_words = malloc(MAX_VARS * MAX_WORDS * sizeof(int));
	g_var_wordcount = malloc(MAX_VARS * sizeof(int));
	g_nvars = 0;

	src = 0; dst = 0;
	while (src < g_nlines) {
		if (g_line_indent[src] == 0 && line_is_var_def(g_line_text[src], &eq)) {
			char *name, *value, *valBuf;
			int valCap, valLen, wl, hasCont;

			name = trim(str_dup_n(g_line_text[src], eq));
			value = trim(str_dup(g_line_text[src] + eq + 1));

			valCap = vlen(value) + 256;
			valBuf = malloc(valCap);
			valLen = 0;
			for (;;) {
				hasCont = ends_with(value, "\\");
				wl = vlen(value) - (hasCont ? 1 : 0);
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
			free(valBuf);
			free(name);
		} else {
			g_line_text[dst] = g_line_text[src];
			g_line_indent[dst] = g_line_indent[src];
			++dst; ++src;
		}
	}
	g_nlines = dst;
}

// Returns index+1 so that 0 unambiguously means "not found" - a match
// at index 0 would otherwise be indistinguishable from failure once
// the index is smuggled through as a pointer-sized value.
int var_lookup (char *name) {
	int i;
	i = 0;
	while (i < g_nvars) { if (!vstrcmp(g_var_name[i], name)) return i + 1; ++i; }
	return 0;
}

///
/// the tree parser
///

void add_entry (char *path, char *value, int isIlink) {
	if (g_nentries >= MAX_ENTRIES) { printf("vfsload: too many entries, dropping %s\n", path); return; }
	g_entry_path[g_nentries] = path;
	g_entry_value[g_nentries] = value;
	g_entry_is_ilink[g_nentries] = isIlink;
	++g_nentries;
}

void emit_value (char *path, char *value) {
	char *inner;
	if ((inner = paren_inner(value, "ilink"))) { add_entry(path, inner, 1); return; }
	if ((inner = paren_inner(value, "file"))) {
		int c;
		if ((c = find_top_colon(inner)) >= 0 || 1) { /* comma-split below */ }
		{
			int comma;
			comma = -1;
			{ int i; i = 0; while (inner[i]) { if (inner[i] == ',') { comma = i; break; } ++i; } }
			if (comma >= 0) add_entry(path, trim(str_dup(trim(str_dup_n(inner + comma + 1, vlen(inner) - comma - 1)))), 0);
			else add_entry(path, inner, 0);
		}
		return;
	}
	add_entry(path, value, 0);
}

void parse_block (int parentIndent, char *pathPrefix);

void handle_each (char *keyExpr, char *valuePattern, int indent, char *pathPrefix) {
	char *argText;
	int nwords, i, blockStart, v, found;

	argText = paren_inner(keyExpr, "each");
	if (!argText) { printf("vfsload: malformed each() near '%s'\n", keyExpr); ++g_cursor; return; }

	if (*argText == '$') {
		found = var_lookup(argText + 1);
		if (!found) { printf("vfsload: unknown variable %s\n", argText); ++g_cursor; return; }
		v = found - 1;
		nwords = g_var_wordcount[v];
	} else {
		// literal word list: reuse split_words_into's tokenizer via a
		// throwaway variable slot
		split_words_into(argText, "$each$");
		v = g_nvars - 1;
		nwords = g_var_wordcount[v];
	}

	if (*valuePattern) {
		// single-line form: consumes exactly this one line
		++g_cursor;
		i = 0;
		while (i < nwords) {
			char *word, *pattern, *key, *val;
			int c;
			word = g_var_words[v * MAX_WORDS + i];
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
		// block form: repeat the indented body once per word
		++g_cursor;
		blockStart = g_cursor;
		i = 0;
		while (i < nwords) {
			char *word;
			word = g_var_words[v * MAX_WORDS + i];
			if (g_subst_depth < MAX_SUBST) g_subst[g_subst_depth++] = word;
			g_cursor = blockStart;
			parse_block(indent, pathPrefix);
			--g_subst_depth;
			++i;
		}
		if (nwords == 0) {
			// still need to skip past the (unrun) body once
			g_cursor = blockStart;
			while (g_cursor < g_nlines && g_line_indent[g_cursor] > indent) ++g_cursor;
		}
	}
}

void process_line (char *pathPrefix) {
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
		++g_cursor;
		parse_block(indent, str_cat(pathPrefix, key));
		return;
	}

	++g_cursor;
	value = *rawVal ? substitute(rawVal) : key;
	emit_value(str_cat(pathPrefix, key), value);
}

void parse_block (int parentIndent, char *pathPrefix) {
	while (g_cursor < g_nlines && g_line_indent[g_cursor] > parentIndent) {
		process_line(pathPrefix);
	}
}

///
/// resolution: real files first (so their content lands in ramfs),
/// then ilinks (which just copy whatever their target resolved to)
///

int load_entries () {
	int i, ok, len;
	char *buf;

	ok = 0;
	i = 0;
	while (i < g_nentries) {
		if (!g_entry_is_ilink[i]) {
			if ((buf = read_whole_file(g_entry_value[i], &len))) {
				if (!vfs_put(g_entry_path[i], buf, len)) ++ok;
				else printf("vfsload: vfs_put failed for %s\n", g_entry_path[i]);
				free(buf);
			}
		}
		++i;
	}
	i = 0;
	while (i < g_nentries) {
		if (g_entry_is_ilink[i]) {
			char *content;
			int len;
			if ((content = vfs_get(g_entry_value[i], &len))) {
				if (!vfs_put(g_entry_path[i], content, len)) ++ok;
			} else {
				printf("vfsload: ilink %s -> %s: target not found\n", g_entry_path[i], g_entry_value[i]);
			}
		}
		++i;
	}
	return ok;
}

int main (int argc, char **argv) {
	char *manifest;
	int len;
	char *manifestName;

	manifestName = argc > 1 ? argv[1] : "c4ke.vfs.txt";
	if (!(manifest = read_whole_file(manifestName, &len))) {
		printf("vfsload: no manifest (%s); ramfs left empty\n", manifestName);
		return 1;
	}

	g_entry_path = malloc(MAX_ENTRIES * sizeof(int));
	g_entry_value = malloc(MAX_ENTRIES * sizeof(int));
	g_entry_is_ilink = malloc(MAX_ENTRIES * sizeof(int));
	g_nentries = 0;
	g_subst = malloc(MAX_SUBST * sizeof(int));
	g_subst_depth = 0;

	split_lines(manifest);
	extract_vars();

	g_cursor = 0;
	parse_block(-1, "");

	{
		int ok;
		ok = load_entries();
		printf("vfsload: %d/%d entries loaded from %s\n", ok, g_nentries, manifestName);
	}
	return 0;
}
