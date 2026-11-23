// C4IX userland: vm -- the hypervisor's machines (c4pc --hv: docs/c4pc-hypervisor.md).
//
//   vm                                 the machines: id, state, name, MB
//   vm new NAME KERNEL BOOT.tar MB [ARGS...]
//                                      a machine: its kernel and boot disk read
//                                      from these files (copied: they may go),
//                                      MB of memory, its kernel's arguments
//                                      (--desktop --no-top if none)
//   vm disk ID IMAGE                   its hd0: IMAGE, a file on a disk here
//                                      (vm mkdisk), read and written where it lies
//   vm cdrom ID ISO                    its CD drive: ISO, a file on a disk here
//   vm start ID | stop ID [-f] | delete ID | info ID
//   vm attach ID                       the screen, keyboard and mouse ID's;
//                                      Ctrl+Alt+Home, or `detach` in it, back
//   vm detach                          the same as `detach`
//   vm send ID TEXT...                 TEXT and a newline typed at ID's console
//   vm mkdisk FILE MB                  a disk image of MB, all of its blocks
//                                      written (a guest's disk must have them)
//   vm create NAME MB DISK_MB          /vm/NAME: a disk image and vm.cfg, set
//                                      for a first boot from the CD (install)
//   vm up NAME                         make and start the machine /vm/NAME/vm.cfg
//                                      says; `vmd` (or vm autostart) every one
//                                      whose vm.cfg says "autostart yes"
//
// vm.cfg, a line each: kernel FILE, boot FILE (the boot archive; - for none:
// a machine that boots from its disk, args --root hd0p1), mb N, disk FILE,
// cdrom FILE or pc (VM 0's own CD drive), args ..., autostart yes|no.
// A machine's ID may be its name.
//
// Only the control machine (VM 0) may make, start, stop and attach; any may
// detach. A guest's disk or CD is its file's blocks as this disk keeps them
// (SYS_FMAP): leave the file alone, and do not move it, while the guest runs.

#include "c4ix_user.h"

enum { R_ID = 0x560, R_A0 = 0x564, R_A1 = 0x568, R_A2 = 0x56c, R_A3 = 0x570, R_CMD = 0x574, R_RES = 0x578, R_COUNT = 0x57c };
enum { C_NAME = 1, C_MB = 2, C_ARGS = 3, C_NEW = 4, C_START = 5, C_STOP = 6, C_ATTACH = 7, C_DETACH = 8,
       C_STATE = 9, C_INFO = 10, C_DELETE = 11, C_DISK = 12, C_ATTACHED = 13, C_CDROM = 14, C_SEND = 15 };
enum { SYS_SYNC = 252, SYS_FMAP = 257, C4I_MMU = 0x80000, C4I_HV = 0x400000, CHUNK = 65536, ARGW = 256 };

// a system call by number (install.c's)
static int ksys(int num, int a, int b, int c) { return __c4_opcode(c, b, a, num); }

static char info_buf[512];
static char cfg[2048];
static char path_buf[256];
static char args_buf[ARGW];
static char zeros[CHUNK];

static int hv(int c, int a0, int a1, int a2, int a3) {
    *(int *)R_A0 = a0; *(int *)R_A1 = a1; *(int *)R_A2 = a2; *(int *)R_A3 = a3;
    *(int *)R_CMD = c;
    return *(int *)R_RES;
}
static int there() { return (__c4_info() & C4I_HV) != 0; }

static int file_size(char *path) {
    int st[4];
    if (ustat(path, st) < 0 || st[0] != 2) return 0 - 1;
    return st[1];
}
// PATH whole into memory: its address, its length in *N; 0
static char *read_whole(char *path, int *n) {
    int fd, size, k, got;
    char *p;
    if ((size = file_size(path)) <= 0) { ufprintf(STDERR, "vm: no file %s\n", path); return 0; }
    if (!(p = (char *)ualloc(size))) { ufprintf(STDERR, "vm: no memory for %s (%d KB)\n", path, size / 1024); return 0; }
    if ((fd = uopen(path, O_RD)) < 0) { ufprintf(STDERR, "vm: cannot read %s\n", path); return 0; }
    got = 0;
    while (got < size && (k = uread(fd, p + got, size - got > CHUNK ? CHUNK : size - got)) > 0) got = got + k;
    uclose(fd);
    if (got != size) { ufprintf(STDERR, "vm: %s: read %d of %d bytes\n", path, got, size); return 0; }
    *n = size;
    return p;
}

// IMAGE's blocks as extents for the hypervisor (4 words each: its first
// sector in the image, the disk, the first sector there, how many): the
// table's address, how many in *N, the image's sectors in *SECTORS; 0
static int *extents(char *image, int *n, int *sectors) {
    int fd, size, lbn, nb, r, ne, cap, *t, out[3], spb, img;
    if ((size = file_size(image)) <= 0) { ufprintf(STDERR, "vm: no file %s\n", image); return 0; }
    if ((fd = uopen(image, O_RD)) < 0) { ufprintf(STDERR, "vm: cannot open %s\n", image); return 0; }
    r = ksys(SYS_FMAP, fd, 0, (int)out);
    if (r < 0) { uclose(fd); ufprintf(STDERR, "vm: %s is not a file on a disk (an ext2 one: mount hdNpP)\n", image); return 0; }
    spb = out[2];
    nb = (size + spb * 512 - 1) / (spb * 512);
    cap = 64; ne = 0;
    if (!(t = (int *)ualloc(cap * 4 * sizeof(int)))) { uclose(fd); return 0; }
    lbn = 0;
    while (lbn < nb) {
        r = ksys(SYS_FMAP, fd, lbn, (int)out);
        if (r < 0) { uclose(fd); ufprintf(STDERR, "vm: %s: its block %d cannot be found\n", image, lbn); return 0; }
        if (r == 1) {
            img = lbn * spb;
            if (ne && t[4 * (ne - 1) + 1] == out[0] && t[4 * (ne - 1)] + t[4 * (ne - 1) + 3] == img &&
                t[4 * (ne - 1) + 2] + t[4 * (ne - 1) + 3] == out[1]) t[4 * (ne - 1) + 3] = t[4 * (ne - 1) + 3] + spb;
            else {
                if (ne == cap) {
                    cap = cap * 2;
                    r = (int)ualloc(cap * 4 * sizeof(int));
                    if (!r) { uclose(fd); ufprintf(STDERR, "vm: no memory for %s's map\n", image); return 0; }
                    memcpy((char *)r, (char *)t, ne * 4 * sizeof(int));
                    t = (int *)r;
                }
                t[4 * ne] = img; t[4 * ne + 1] = out[0]; t[4 * ne + 2] = out[1]; t[4 * ne + 3] = spb;
                ++ne;
            }
        }
        ++lbn;
    }
    uclose(fd);
    if (!ne) { ufprintf(STDERR, "vm: %s has no blocks written (vm mkdisk makes one that has)\n", image); return 0; }
    *n = ne;
    *sectors = nb * spb;
    return t;
}

static void show(int id) {
    int k;
    if (hv(C_INFO, id, (int)info_buf, sizeof info_buf, 0) < 0) return;
    uprintf("%s", info_buf);
}
static void list() {
    int id, n, st;
    char *p, *q;
    n = *(int *)R_COUNT;
    uprintf("ID  STATE    MB    NAME\n");
    id = 0;
    while (id < n) {
        st = hv(C_STATE, id, 0, 0, 0);
        if (st > 0 && hv(C_INFO, id, (int)info_buf, sizeof info_buf, 0) >= 0) {
            // "id N\nname X\nstate S\nmb M\n..." as a row
            p = info_buf;
            uprintf("%d   ", id);
            q = p; while (*q && ustrncmp(q, "state ", 6)) { while (*q && *q != 10) ++q; if (*q) ++q; }
            if (*q) { q = q + 6; while (*q && *q != 10) { uprintf("%c", *q); ++q; } }
            uprintf("  ");
            q = p; while (*q && ustrncmp(q, "mb ", 3)) { while (*q && *q != 10) ++q; if (*q) ++q; }
            if (*q) { q = q + 3; while (*q && *q != 10) { uprintf("%c", *q); ++q; } }
            uprintf("  ");
            q = p; while (*q && ustrncmp(q, "name ", 5)) { while (*q && *q != 10) ++q; if (*q) ++q; }
            if (*q) { q = q + 5; while (*q && *q != 10) { uprintf("%c", *q); ++q; } }
            if (hv(C_ATTACHED, 0, 0, 0, 0) == id) uprintf("  (on the screen)");
            uprintf("\n");
        }
        ++id;
    }
}

static int mkdisk(char *path, int mb) {
    int fd, k, n;
    if (mb < 1) { ufprintf(STDERR, "vm: mkdisk FILE MB\n"); return 1; }
    if ((fd = uopen(path, O_WR + O_CREATE + O_TRUNCATE)) < 0) { ufprintf(STDERR, "vm: cannot make %s\n", path); return 1; }
    n = mb * (1048576 / CHUNK);
    k = 0;
    while (k < n) {
        if (write(fd, zeros, CHUNK) != CHUNK) { uclose(fd); ufprintf(STDERR, "vm: %s: the disk is full at %d MB\n", path, k * CHUNK / 1048576); return 1; }
        ++k;
        if (k % (64 * 1048576 / CHUNK) == 0) uprintf("vm: %s: %d of %d MB\n", path, k * CHUNK / 1048576, mb);
    }
    uclose(fd);
    ksys(SYS_SYNC, 0, 0, 0);
    uprintf("vm: %s: %d MB\n", path, mb);
    return 0;
}

// KEY's value in the vm.cfg text at CFG, into OUT (W bytes): 1, or 0
static int cfg_get(char *key, char *out, int w) {
    char *p;
    int k, n;
    p = cfg;
    n = ustrlen(key);
    while (*p) {
        while (*p == ' ' || *p == 9) ++p;
        if (*p != '#' && !ustrncmp(p, key, n) && (p[n] == ' ' || p[n] == 9)) {
            p = p + n;
            while (*p == ' ' || *p == 9) ++p;
            k = 0;
            while (*p && *p != 10 && *p != 13 && k < w - 1) { out[k] = *p; ++k; ++p; }
            while (k > 0 && (out[k - 1] == ' ' || out[k - 1] == 9)) --k;
            out[k] = 0;
            return 1;
        }
        while (*p && *p != 10) ++p;
        if (*p) ++p;
    }
    return 0;
}

// a machine's id from its number or its name; -1
static int find(char *s) {
    int id, n;
    char *q;
    if (s[0] >= '0' && s[0] <= '9') return uatoi(s);
    n = *(int *)R_COUNT;
    id = 1;
    while (id < n) {
        if (hv(C_STATE, id, 0, 0, 0) > 0 && hv(C_INFO, id, (int)info_buf, sizeof info_buf, 0) >= 0) {
            q = info_buf;
            while (*q && ustrncmp(q, "name ", 5)) { while (*q && *q != 10) ++q; if (*q) ++q; }
            if (*q && !ustrncmp(q + 5, s, ustrlen(s)) && (q[5 + ustrlen(s)] == 10 || !q[5 + ustrlen(s)])) return id;
        }
        ++id;
    }
    return 0 - 1;
}

// the machine NAME's vm.cfg made and started: 0, or 1
static int up(char *name) {
    char kernel[128], boot[128], mb[16], disk[128], cd[128], *img, *tar;
    int ilen, tlen, id, k, n, sectors, *t;
    upathjoin(path_buf, 256, "/vm", name);
    upathjoin(path_buf, 256, path_buf, "vm.cfg");
    if ((k = uslurp(path_buf, cfg, sizeof cfg - 1)) <= 0) { ufprintf(STDERR, "vm: no %s\n", path_buf); return 1; }
    cfg[k] = 0;
    if (find(name) > 0) { ufprintf(STDERR, "vm: %s is made already (vm start, or vm delete)\n", name); return 1; }
    if (!cfg_get("kernel", kernel, 128)) ustrcpy(kernel, "/boot/c4ix32.c4r");
    if (!cfg_get("boot", boot, 128)) ustrcpy(boot, "-");
    if (!cfg_get("mb", mb, 16)) ustrcpy(mb, "256");
    if (!cfg_get("args", args_buf, ARGW)) ustrcpy(args_buf, "--desktop --no-top");
    if (!(img = read_whole(kernel, &ilen))) return 1;
    tar = 0; tlen = 0;
    if (ustrcmp(boot, "-") && !(tar = read_whole(boot, &tlen))) return 1;
    hv(C_NAME, (int)name, 0, 0, 0);
    hv(C_MB, uatoi(mb), 0, 0, 0);
    hv(C_ARGS, (int)args_buf, 0, 0, 0);
    if ((id = hv(C_NEW, (int)img, ilen, (int)tar, tlen)) < 0) { ufprintf(STDERR, "vm: %s was not made (%d)\n", name, id); return 1; }
    if (cfg_get("disk", disk, 128)) {
        if (!(t = extents(disk, &n, &sectors)) || hv(C_DISK, id, (int)t, n, sectors) < 0) { ufprintf(STDERR, "vm: %s: its disk %s was not given\n", name, disk); return 1; }
    }
    if (cfg_get("cdrom", cd, 128)) {
        if (!ustrcmp(cd, "pc")) k = hv(C_CDROM, id, 0, 0, 0);
        else k = (t = extents(cd, &n, &sectors)) ? hv(C_CDROM, id, (int)t, n, sectors) : 0 - 1;
        if (k < 0) { ufprintf(STDERR, "vm: %s: its CD %s was not given\n", name, cd); return 1; }
    }
    if (hv(C_START, id, 0, 0, 0) < 0) { ufprintf(STDERR, "vm: %s did not start\n", name); return 1; }
    uprintf("vm %d: %s up, %s MB%s%s\n", id, name, mb, cfg_get("disk", disk, 128) ? ", hd0 " : "", cfg_get("disk", disk, 128) ? disk : "");
    return 0;
}

// every /vm/NAME/vm.cfg that says "autostart yes", up
static int autostart() {
    char name[64], v[16];
    int i, k, bad;
    i = 0; bad = 0;
    while (ureaddir("/vm", i, name) >= 0) {
        upathjoin(path_buf, 256, "/vm", name);
        upathjoin(path_buf, 256, path_buf, "vm.cfg");
        if ((k = uslurp(path_buf, cfg, sizeof cfg - 1)) > 0) {
            cfg[k] = 0;
            if (cfg_get("autostart", v, 16) && !ustrcmp(v, "yes")) bad = bad + up(name);
        }
        ++i;
    }
    return bad ? 1 : 0;
}

// /vm/NAME: its disk image and a vm.cfg for a first boot from the CD
static int create(char *name, int mb, int dmb) {
    int fd;
    char disk[128];
    umkdir("/vm");
    upathjoin(path_buf, 256, "/vm", name);
    umkdir(path_buf);
    upathjoin(disk, 128, path_buf, "disk0.img");
    if (mkdisk(disk, dmb)) return 1;
    upathjoin(path_buf, 256, path_buf, "vm.cfg");
    if ((fd = uopen(path_buf, O_WR + O_CREATE + O_TRUNCATE)) < 0) { ufprintf(STDERR, "vm: cannot write %s\n", path_buf); return 1; }
    ufprintf(fd, "# %s: vm up %s starts it (docs/c4pc-hypervisor.md)\n", name, name);
    ufprintf(fd, "# First boot: from the CD (VM 0's drive, the C4PC disc), to run install\n");
    ufprintf(fd, "# onto hd0. Then: boot -, args --root hd0p1 --desktop --no-top.\n");
    ufprintf(fd, "kernel /mnt/cdrom/boot/c4ix32.c4r\nboot /mnt/cdrom/boot/disk.tar\nmb %d\ndisk %s\ncdrom pc\n", mb, disk);
    ufprintf(fd, "args --desktop --no-top\nautostart no\n");
    uclose(fd);
    uprintf("vm: %s made: mount cdrom (the C4PC disc at /mnt/cdrom), then vm up %s\n", path_buf, name);
    return 0;
}

static int usage() {
    ufprintf(STDERR, "usage: vm [new NAME KERNEL BOOT.tar MB [ARGS...] | disk ID IMAGE | cdrom ID ISO |\n");
    ufprintf(STDERR, "          start ID | stop ID [-f] | attach ID | detach | delete ID | info ID | send ID TEXT |\n");
    ufprintf(STDERR, "          mkdisk FILE MB | create NAME MB DISK_MB | up NAME | autostart]\n");
    return 2;
}

int main(int argc, char **argv) {
    char *img, *tar, *cmd;
    int ilen, tlen, id, k, n, sectors, *t;
    if (__c4_info() & C4I_MMU) uioperm(UIOPERM_DEV);
    if (argc > 1 && !ustrcmp(argv[1], "mkdisk")) return argc == 4 ? mkdisk(argv[2], uatoi(argv[3])) : usage();
    // vmd: vm autostart, at the control machine's boot (GRUB's hypervisor entry: -e vmd)
    k = ustrlen(argv[0]);
    if (k >= 3 && !ustrcmp(argv[0] + k - 3, "vmd")) return there() ? autostart() : 0;
    if (k >= 7 && !ustrcmp(argv[0] + k - 7, "vmd.c4r")) return there() ? autostart() : 0;
    if (argc > 1 && !ustrcmp(argv[1], "create")) return argc == 5 ? create(argv[2], uatoi(argv[3]), uatoi(argv[4])) : usage();
    if (!there()) { ufprintf(STDERR, "vm: no hypervisor (c4pc --hv: GRUB's \"C4PC hypervisor\")\n"); return 1; }
    if (argc < 2) { list(); return 0; }
    cmd = argv[1];
    if (!ustrcmp(cmd, "detach")) { hv(C_DETACH, 0, 0, 0, 0); return 0; }
    if (!ustrcmp(cmd, "autostart")) return autostart();
    if (!ustrcmp(cmd, "up")) return argc == 3 ? up(argv[2]) : usage();
    if (!ustrcmp(cmd, "new")) {
        if (argc < 6) return usage();
        if (!(img = read_whole(argv[3], &ilen)) || !(tar = read_whole(argv[4], &tlen))) return 1;
        args_buf[0] = 0;
        k = 6;
        while (k < argc) { if (k > 6) ustrcat(args_buf, " "); if (ustrlen(args_buf) + ustrlen(argv[k]) + 2 < ARGW) ustrcat(args_buf, argv[k]); ++k; }
        if (!args_buf[0]) ustrcpy(args_buf, "--desktop --no-top");
        hv(C_NAME, (int)argv[2], 0, 0, 0);
        hv(C_MB, uatoi(argv[5]), 0, 0, 0);
        hv(C_ARGS, (int)args_buf, 0, 0, 0);
        id = hv(C_NEW, (int)img, ilen, (int)tar, tlen);
        if (id < 0) { ufprintf(STDERR, "vm: the machine was not made (%d: no free slot, or no memory for its files)\n", id); return 1; }
        uprintf("vm %d: %s, %s MB (vm start %d)\n", id, argv[2], argv[5], id);
        return 0;
    }
    if (argc < 3) return usage();
    if ((id = find(argv[2])) < 0) { ufprintf(STDERR, "vm: no machine %s\n", argv[2]); return 1; }
    if (!ustrcmp(cmd, "disk") || !ustrcmp(cmd, "cdrom")) {
        if (argc < 4) return usage();
        if (cmd[0] == 'c' && !ustrcmp(argv[3], "pc")) { k = hv(C_CDROM, id, 0, 0, 0); if (k < 0) { ufprintf(STDERR, "vm: no CD drive here\n"); return 1; } return 0; }
        if (!(t = extents(argv[3], &n, &sectors))) return 1;
        k = hv(cmd[0] == 'd' ? C_DISK : C_CDROM, id, (int)t, n, sectors);
        if (k < 0) { ufprintf(STDERR, "vm: %d: the %s was not given (%d: running, no such machine, or a bad map)\n", id, cmd, k); return 1; }
        uprintf("vm %d: %s %s, %d MB in %d pieces\n", id, cmd[0] == 'd' ? "hd0" : "its CD", argv[3], sectors / 2048, n);
        return 0;
    }
    if (!ustrcmp(cmd, "send")) {
        args_buf[0] = 0;
        k = 3;
        while (k < argc) { if (k > 3) ustrcat(args_buf, " "); if (ustrlen(args_buf) + ustrlen(argv[k]) + 3 < ARGW) ustrcat(args_buf, argv[k]); ++k; }
        ustrcat(args_buf, "\n");
        k = hv(C_SEND, id, (int)args_buf, ustrlen(args_buf), 0);
        if (k < 0) { ufprintf(STDERR, "vm: send %d: not running\n", id); return 1; }
        return 0;
    }
    if (!ustrcmp(cmd, "start")) k = hv(C_START, id, 0, 0, 0);
    else if (!ustrcmp(cmd, "stop")) k = hv(C_STOP, id, argc > 3 && !ustrcmp(argv[3], "-f"), 0, 0);
    else if (!ustrcmp(cmd, "attach")) k = hv(C_ATTACH, id, 0, 0, 0);
    else if (!ustrcmp(cmd, "delete")) k = hv(C_DELETE, id, 0, 0, 0);
    else if (!ustrcmp(cmd, "info")) { show(id); return 0; }
    else return usage();
    if (k < 0) { ufprintf(STDERR, "vm: %s %d: no (%d)\n", cmd, id, k); return 1; }
    return 0;
}
