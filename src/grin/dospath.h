// dospath.h -- the window kit's paths and C4DOS's names, both ways
// (docs/grin-plan.md 3.8). Included by grin (disk.c) and by every
// application (import.c), each of which opens files itself.
//
// The kit's file box thinks in folders, "/" and names under it; C4DOS
// has drives, each with its directories (API v7). So in grin a drive is
// a folder of the root: "/B:/NOTE.TXT" is DOS's "B:/NOTE.TXT", B:'s
// NOTE.TXT from its root, "/A:/GAMES/DOOM/DOOM.C4R" is A:\GAMES\DOOM\
// DOOM.C4R, and "/RAM:/X" is "RAM:/X" (the RAM disk, which DOS reads
// ahead of every drive). A name with no folder is DOS's as it stands,
// and DOS looks for it where it looks for any.

#ifndef GRIN_DOSPATH_H
#define GRIN_DOSPATH_H 1

// "/B:/NOTE.TXT" -> "B:/NOTE.TXT" in out (n bytes); any other path as it is
static char *grin_dospath(char *path, char *out, int n) {
    int i, k;
    if (path[0] != '/') return path;
    i = 1; k = 0;
    while (path[i] && k < n - 1) { out[k] = path[i]; ++k; ++i; }
    out[k] = 0;
    return out;
}

// ... and back: "B:NOTE.TXT", "B:\SAVES\X" or "RAM:X" -> "/B:/NOTE.TXT",
// "/B:/SAVES/X", "/RAM:/X"; anything else as it is
static char *grin_kitpath(char *name, char *out, int n) {
    int i, k, c;
    if (name[0] && name[1] == ':') i = 1;
    else if ((name[0] | 32) == 'r' && (name[1] | 32) == 'a' && (name[2] | 32) == 'm' && name[3] == ':') i = 3;
    else return name;
    out[0] = '/'; k = 1;
    c = 0;
    while (c <= i) { out[k] = name[c]; ++k; ++c; }
    out[k] = '/'; ++k;
    while (name[c] == '/' || name[c] == '\\') ++c;
    while (name[c] && k < n - 1) { out[k] = name[c] == '\\' ? '/' : name[c]; ++k; ++c; }
    out[k] = 0;
    return out;
}

#endif
