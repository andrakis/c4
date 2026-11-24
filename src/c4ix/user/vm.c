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
//   vm start ID | stop ID [-f] | info ID
//   vm pause ID | resume ID            a running machine stopped where it is
//                                      (its memory and screen kept, its clock
//                                      stopped), and on again
//   vm delete NAME [-y]                the machine gone: out of the hypervisor,
//                                      and /vm/NAME (vm.cfg, its disk images)
//                                      removed, once yes is typed (-y: not
//                                      asked). Not while it runs or is paused
//   vm attach ID                       the screen, keyboard and mouse ID's;
//                                      Ctrl+Alt+Home, or `detach` in it, back
//   vm detach                          the same as `detach`
//   vm send ID TEXT...                 TEXT and a newline typed at ID's console
//   vm mkdisk FILE MB                  a disk image of MB, all of its blocks
//                                      written (a guest's disk must have them)
//   vm create NAME MB DISK_MB [SYSTEM|ISO] [cpus=N] [swap=MB] [autostart]
//                                      /vm/NAME: a disk image (none for 0) and
//                                      vm.cfg, for SYSTEM (c4ix by default):
//                                        c4ix       C4IX's desktop
//                                        c4ix-text  C4IX at its text console
//                                        c4ke       C4KE, at its shell
//                                        c4dos      C4DOS, grin and the games
//                                      or C4IX's desktop with ISO as its CD
//   vm images [DIR]                    the systems into /vm/images (DIR) from the
//                                      C4PC disc (vm create does, the first
//                                      time; install does onto a disk)
//   vm up NAME                         make and start the machine /vm/NAME/vm.cfg
//                                      says; `vmd` (or vm autostart) every one
//                                      whose vm.cfg says "autostart yes"
//   vm log                             c4pc's log: what it and the guests said
//
//   vm mkswap NAME MB                  /vm/NAME/swap.img of MB, its swap from
//                                      its next start (vm.cfg's swap line)
//
// vm.cfg, a line each: kernel FILE, boot FILE (the boot archive; - for none:
// a machine that boots from its disk, args --root hd0p1), mb N, mmu yes|no
// (yes when it does not say), disk FILE, cdrom FILE or pc (VM 0's own CD
// drive), args ... (none for the console), autostart yes|no, system NAME.
// A machine's ID may be its name.
//
// /vm/images is where the systems' kernels and boot archives are, copied
// once from the C4PC disc, so that a machine starts with no CD in the drive:
// c4ix32.c4r and c4ix.tar (the disc's /boot/disk.tar, which C4KE boots
// from too), c4ke32.c4r, c4dos32.c4r and c4dos.tar (its /boot/images').
//
// Only the control machine (VM 0) may make, start, stop and attach; any may
// detach. A guest's disk or CD is its file's blocks as this disk keeps them
// (SYS_FMAP): leave the file alone, and do not move it, while the guest runs.

#include "c4ix_user.h"

enum { R_ID = 0x560, R_A0 = 0x564, R_A1 = 0x568, R_A2 = 0x56c, R_A3 = 0x570, R_CMD = 0x574, R_RES = 0x578, R_COUNT = 0x57c };
enum { C_NAME = 1, C_MB = 2, C_ARGS = 3, C_NEW = 4, C_START = 5, C_STOP = 6, C_ATTACH = 7, C_DETACH = 8,
       C_STATE = 9, C_INFO = 10, C_DELETE = 11, C_DISK = 12, C_ATTACHED = 13, C_CDROM = 14, C_SEND = 15,
       C_LOG = 17, C_PAUSE = 18, C_RESUME = 19, C_SWAP = 20 };
enum { S_RUN = 2, S_PAUSE = 4 };
enum { DISK_COUNT = 0x188, N_SYS = 4, N_IMG = 5, LOGW = 32768 };
enum { SYS_SYNC = 252, SYS_FMAP = 257, C4I_MMU = 0x80000, C4I_HV = 0x400000, CHUNK = 1048576, ARGW = 256 };

// a system call by number (install.c's)
static int ksys(int num, int a, int b, int c) { return __c4_opcode(c, b, a, num); }

static char info_buf[512];
static char cfg[2048];
static char path_buf[256];
static char args_buf[ARGW];
static char zeros[CHUNK];
static char text[4096];
static char from_buf[256];

// the systems vm create knows: their names, what they are, their kernels,
// boot archives, arguments and memory
static char *sys_name[N_SYS] = { "c4ix", "c4ix-text", "c4ke", "c4dos" };
static char *sys_label[N_SYS] = { "C4IX, its desktop", "C4IX, its text console", "C4KE, at its shell", "C4DOS, grin and the games" };
static char *sys_kernel[N_SYS] = { "c4ix32.c4r", "c4ix32.c4r", "c4ke32.c4r", "c4dos32.c4r" };
static char *sys_boot[N_SYS] = { "c4ix.tar", "c4ix.tar", "c4ix.tar", "c4dos.tar" };
static char *sys_args[N_SYS] = { "--desktop --no-top", "", "", "" };
static int sys_mb[N_SYS] = { 256, 128, 128, 256 };
// /vm/images' files, and where each is on the C4PC disc
static char *img_name[N_IMG] = { "c4ix32.c4r", "c4ix.tar", "c4ke32.c4r", "c4dos32.c4r", "c4dos.tar" };
static char *img_disc[N_IMG] = { "boot/c4ix32.c4r", "boot/disk.tar", "boot/images/c4ke32.c4r", "boot/images/c4dos32.c4r", "boot/images/c4dos.tar" };
static int images();
static int is_image(char *name);

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
    // a file's size and place are an int at the 32-bit word (C4IX_LFS's
    // 64-bit ones not in vm yet): an image of 2 GB or more is later work
    if (sizeof(int) == 4 && mb > 2047) { ufprintf(STDERR, "vm: a disk image is 2047 MB at most for now\n"); return 1; }
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
        if (*p != '#' && !ustrncmp(p, key, n) && (p[n] == ' ' || p[n] == 9 || p[n] == 10 || p[n] == 13 || !p[n])) {
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
    char kernel[128], boot[128], mb[16], disk[128], cd[128], v[16], *img, *tar, *av[3];
    int ilen, tlen, id, k, n, sectors, *t, mmu, cpus;
    upathjoin(path_buf, 256, "/vm", name);
    upathjoin(path_buf, 256, path_buf, "vm.cfg");
    if ((k = uslurp(path_buf, cfg, sizeof cfg - 1)) <= 0) { ufprintf(STDERR, "vm: no %s\n", path_buf); return 1; }
    cfg[k] = 0;
    if (find(name) > 0) { ufprintf(STDERR, "vm: %s is made already (vm start, or vm delete)\n", name); return 1; }
    if (!cfg_get("kernel", kernel, 128)) ustrcpy(kernel, "/boot/c4ix32.c4r");
    // its kernel on the C4PC disc, not mounted yet: mount cdrom
    if (!ustrncmp(kernel, "/mnt/cdrom/", 11) && file_size(kernel) <= 0) {
        av[0] = "mount"; av[1] = "cdrom"; av[2] = 0;
        if ((k = spawn("/bin/mount", 2, av)) >= 0) uwait(k);
    }
    if (!cfg_get("boot", boot, 128)) ustrcpy(boot, "-");
    if (!cfg_get("mb", mb, 16)) ustrcpy(mb, "256");
    mmu = !cfg_get("mmu", v, 16) || ustrcmp(v, "no") ? 1 : 2;
    cpus = cfg_get("cpus", v, 16) ? uatoi(v) : 1;
    if (!cfg_get("args", args_buf, ARGW)) ustrcpy(args_buf, "--desktop --no-top");
    // a system's file in /vm/images not there yet: copied from the disc now
    if (!ustrncmp(kernel, "/vm/images/", 11) && file_size(kernel) <= 0 && is_image(kernel + 11) && images()) return 1;
    if (!(img = read_whole(kernel, &ilen))) return 1;
    tar = 0; tlen = 0;
    if (ustrcmp(boot, "-") && !(tar = read_whole(boot, &tlen))) return 1;
    hv(C_NAME, (int)name, 0, 0, 0);
    hv(C_MB, uatoi(mb), mmu, cpus, 0);
    hv(C_ARGS, (int)args_buf, 0, 0, 0);
    if ((id = hv(C_NEW, (int)img, ilen, (int)tar, tlen)) < 0) { ufprintf(STDERR, "vm: %s was not made (%d: no free slot, or no memory for its files)\n", name, id); return 1; }
    if (cfg_get("disk", disk, 128)) {
        if (!(t = extents(disk, &n, &sectors)) || hv(C_DISK, id, (int)t, n, sectors) < 0) { ufprintf(STDERR, "vm: %s: its disk %s was not given\n", name, disk); hv(C_DELETE, id, 0, 0, 0); return 1; }
    }
    // its swap: an image of its own (the guest's MMU puts pages out there)
    if (cfg_get("swap", disk, 128) && disk[0] && ustrcmp(disk, "none")) {
        if (!(t = extents(disk, &n, &sectors)) || hv(C_SWAP, id, (int)t, n, sectors) < 0) { ufprintf(STDERR, "vm: %s: its swap %s was not given\n", name, disk); hv(C_DELETE, id, 0, 0, 0); return 1; }
    }
    if (cfg_get("cdrom", cd, 128)) {
        if (!ustrcmp(cd, "pc")) {
            // VM 0's CD drive: none, and the machine starts without a CD
            if (hv(C_CDROM, id, 0, 0, 0) < 0) uprintf("vm: %s: no CD drive here, so no CD\n", name);
        } else {
            k = (t = extents(cd, &n, &sectors)) ? hv(C_CDROM, id, (int)t, n, sectors) : 0 - 1;
            if (k < 0) { ufprintf(STDERR, "vm: %s: its CD %s was not given\n", name, cd); hv(C_DELETE, id, 0, 0, 0); return 1; }
        }
    }
    if (hv(C_START, id, 0, 0, 0) < 0) { ufprintf(STDERR, "vm: %s did not start: not memory enough for its %s MB (vm log says)\n", name, mb); hv(C_DELETE, id, 0, 0, 0); return 1; }
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

// c4pc's boot disc: where it is mounted, mounted at /mnt/c4pc now if it is
// in a drive (the CD whose label is C4PC: N:.c4pc-drive); 0 if it is not
static char *disc() {
    char name[16], *av[4], *p;
    int n, k, id;
    if (file_size("/mnt/c4pc/boot/c4ix32.c4r") > 0) return "/mnt/c4pc";
    if (file_size("/mnt/cdrom/boot/c4ix32.c4r") > 0) return "/mnt/cdrom";
    n = *(int *)DISK_COUNT;
    k = 2;
    while (k < n && k < 10) {
        name[0] = '0' + k; name[1] = ':'; ustrcpy(name + 2, ".c4pc-drive");
        if (uslurp(name, text, sizeof text - 1) >= 0) {
            p = text;
            while (*p) {
                if (!ustrncmp(p, "label C4PC", 10) && (p[10] == 10 || p[10] == 13 || !p[10])) {
                    name[0] = '0' + k; name[1] = 0;
                    umkdir("/mnt");
                    av[0] = "mount"; av[1] = name; av[2] = "/mnt/c4pc"; av[3] = 0;
                    if ((id = spawn("/bin/mount", 3, av)) < 0 || uwait(id) != 0) return 0;
                    return file_size("/mnt/c4pc/boot/c4ix32.c4r") > 0 ? "/mnt/c4pc" : 0;
                }
                while (*p && *p != 10) ++p; if (*p) ++p;
            }
        }
        ++k;
    }
    return 0;
}

// FROM copied to TO, a MB at a time: 0, or 1
static int copy_file(char *from, char *to) {
    int in, out, k, done, size;
    size = file_size(from);
    if ((in = uopen(from, O_RD)) < 0) { ufprintf(STDERR, "vm: cannot read %s\n", from); return 1; }
    if ((out = uopen(to, O_WR + O_CREATE + O_TRUNCATE)) < 0) { uclose(in); ufprintf(STDERR, "vm: cannot make %s\n", to); return 1; }
    done = 0;
    while ((k = uread(in, zeros, CHUNK)) > 0) {
        if (write(out, zeros, k) != k) { uclose(in); uclose(out); ufprintf(STDERR, "vm: %s: a write failed (the disk full?)\n", to); return 1; }
        done = done + k;
        if (size > 8 * 1048576 && done % (8 * 1048576) == 0) uprintf("vm:   %s: %d of %d MB\n", to, done / 1048576, size / 1048576);
    }
    uclose(in);
    if (uclose(out) < 0 || k < 0) { ufprintf(STDERR, "vm: %s was not copied whole\n", to); return 1; }
    return 0;
}

// the systems' files into DIR (/vm/images) from the C4PC disc, each that
// is not there with its size already: 0, or 1. Also install's, onto a disk
int vm_images(char *dir) {
    char *src, to[160];
    int k, bad;
    if (!(src = disc())) {
        ufprintf(STDERR, "vm: the C4PC disc is not in a drive: its systems are copied from it to %s once\n", dir);
        ufprintf(STDERR, "vm: (put the ISO in the PC's CD drive, then vm images)\n");
        return 1;
    }
    // DIR and the directories it is in
    k = 1;
    while (dir[k]) { if (dir[k] == '/') { ustrcpy(to, dir); to[k] = 0; umkdir(to); } ++k; }
    umkdir(dir);
    bad = 0; k = 0;
    while (k < N_IMG) {
        upathjoin(from_buf, 256, src, img_disc[k]);
        upathjoin(to, 160, dir, img_name[k]);
        if (file_size(from_buf) <= 0) { ufprintf(STDERR, "vm: %s is not on the disc (an older C4PC disc?)\n", from_buf); bad = 1; }
        else if (file_size(to) != file_size(from_buf)) {
            uprintf("vm: %s (%d MB)\n", to, (file_size(from_buf) + 1048575) / 1048576);
            bad = bad + copy_file(from_buf, to);
        }
        ++k;
    }
    ksys(SYS_SYNC, 0, 0, 0);
    if (!bad) uprintf("vm: the systems are in %s\n", dir);
    return bad ? 1 : 0;
}
static int images() { return vm_images("/vm/images"); }
// is NAME one of /vm/images' files (one the disc has)
static int is_image(char *name) {
    int k;
    k = 0;
    while (k < N_IMG) { if (!ustrcmp(name, img_name[k])) return 1; ++k; }
    return 0;
}

// /vm/NAME: its disk image and a vm.cfg for SYSTEM (c4ix if 0), or for
// C4IX's desktop with the ISO file SYSTEM as its CD
static int create(char *name, int mb, int dmb, char *system, int cpus, int autostart) {
    int fd, s;
    char disk[128], *iso;
    s = 0; iso = 0;
    if (system) {
        while (s < N_SYS && ustrcmp(system, sys_name[s])) ++s;
        if (s == N_SYS) {
            if (file_size(system) <= 0) {
                ufprintf(STDERR, "vm: %s is not a system (c4ix, c4ix-text, c4ke, c4dos) or an ISO file\n", system);
                return 1;
            }
            iso = system; s = 0;
        }
    }
    if (mb <= 0) mb = sys_mb[s];
    // its system's files, from the disc if they are not here yet
    upathjoin(from_buf, 256, "/vm/images", sys_kernel[s]);
    if (file_size(from_buf) <= 0 && images()) return 1;
    upathjoin(from_buf, 256, "/vm/images", sys_boot[s]);
    if (file_size(from_buf) <= 0 && images()) return 1;
    umkdir("/vm");
    upathjoin(path_buf, 256, "/vm", name);
    umkdir(path_buf);
    upathjoin(disk, 128, path_buf, "disk0.img");
    if (dmb > 0 && mkdisk(disk, dmb)) return 1;
    upathjoin(path_buf, 256, path_buf, "vm.cfg");
    if ((fd = uopen(path_buf, O_WR + O_CREATE + O_TRUNCATE)) < 0) { ufprintf(STDERR, "vm: cannot write %s\n", path_buf); return 1; }
    ufprintf(fd, "# %s: %s. vm up %s starts it (docs/c4pc-hypervisor.md)\n", name, sys_label[s], name);
    if (dmb > 0 && s < 2)
        ufprintf(fd, "# install, in it, puts C4IX on hd0; then boot -, args --root hd0p1 --desktop --no-top\n");
    ufprintf(fd, "system %s\nkernel /vm/images/%s\nboot /vm/images/%s\nmb %d\nmmu yes\n", sys_name[s], sys_kernel[s], sys_boot[s], mb);
    if (dmb > 0) ufprintf(fd, "disk %s\n", disk);
    if (iso) ufprintf(fd, "cdrom %s\n", iso);
    ufprintf(fd, "args %s\nautostart %s\n", sys_args[s], autostart ? "yes" : "no");
    if (cpus > 1) ufprintf(fd, "cpus %d\n", cpus);
    uclose(fd);
    uprintf("vm: %s made: %s. vm up %s starts it\n", path_buf, sys_label[s], name);
    return 0;
}

// c4pc's log, as much of it as it keeps
static int show_log() {
    char *b;
    int n;
    if (!(b = (char *)ualloc(LOGW + 1))) return 1;
    n = hv(C_LOG, (int)b, LOGW + 1, 0, 0);
    if (n < 0) { ufprintf(STDERR, "vm: no log (an older c4pc?)\n"); return 1; }
    write(STDOUT, b, n);
    return 0;
}

// NAME gone: out of the hypervisor if it was made (not while it runs or is
// paused), and /vm/NAME removed, its files and itself, once YES or asked
static int delete(char *name, int yes) {
    char file[64], dir[128], ans[16];
    int id, st, n, k;
    id = find(name);
    if (id == 0) { ufprintf(STDERR, "vm: the control machine is not deleted\n"); return 1; }
    if (id > 0) {
        st = hv(C_STATE, id, 0, 0, 0);
        if (st == S_RUN || st == S_PAUSE) { ufprintf(STDERR, "vm: %s is %s: stop it first (vm stop %s)\n", name, st == S_RUN ? "running" : "paused", name); return 1; }
    }
    k = 0;
    while (name[k]) { if (name[k] == '/') break; ++k; }
    if (name[k] || name[0] == '.' || !ustrcmp(name, "images")) { ufprintf(STDERR, "vm: %s is not a machine's name\n", name); return 1; }
    upathjoin(dir, 128, "/vm", name);
    if (id < 0 && ureaddir(dir, 0, file) < 0) {
        if (uunlink(dir) < 0) { ufprintf(STDERR, "vm: no machine %s\n", name); return 1; }
        uprintf("vm: %s deleted\n", name);
        return 0;
    }
    if (!yes && ureaddir(dir, 0, file) >= 0) {
        uprintf("vm: delete %s: %s and everything in it (its vm.cfg and disk images), for good?\n", name, dir);
        uprintf("    Type yes to delete it: ");
        n = uread(0, ans, 15);
        if (n < 0) n = 0;
        ans[n] = 0;
        if (ustrncmp(ans, "yes", 3)) { uprintf("vm: %s kept\n", name); return 1; }
    }
    if (id > 0 && hv(C_DELETE, id, 0, 0, 0) < 0) { ufprintf(STDERR, "vm: %s could not be taken out of the hypervisor\n", name); return 1; }
    // its files (flat: vm create makes no directories in it), then it
    n = 0;
    while (ureaddir(dir, 0, file) >= 0 && n < 1000) {
        upathjoin(path_buf, 256, dir, file);
        if (uunlink(path_buf) < 0) { ufprintf(STDERR, "vm: %s could not be removed\n", path_buf); return 1; }
        ++n;
    }
    uunlink(dir);
    ksys(SYS_SYNC, 0, 0, 0);
    uprintf("vm: %s deleted (%d files)\n", name, n);
    return 0;
}

// /vm/NAME/swap.img of MB, and vm.cfg's swap line set to it: 0, or 1
static int mkswap(char *name, int mb) {
    char img[128], *out, *p, *q;
    int fd, k, done;
    upathjoin(path_buf, 256, "/vm", name);
    upathjoin(path_buf, 256, path_buf, "vm.cfg");
    if ((k = uslurp(path_buf, cfg, sizeof cfg - 1)) < 0) { ufprintf(STDERR, "vm: no %s\n", path_buf); return 1; }
    cfg[k] = 0;
    upathjoin(img, 128, "/vm", name);
    upathjoin(img, 128, img, "swap.img");
    if (mkdisk(img, mb)) return 1;
    // vm.cfg again, its swap line this image
    if (!(out = (char *)ualloc(sizeof cfg + 160))) return 1;
    p = cfg; q = out; done = 0;
    while (*p) {
        if (!ustrncmp(p, "swap", 4) && (p[4] == ' ' || p[4] == 9 || p[4] == 10)) { while (*p && *p != 10) ++p; if (*p) ++p; continue; }
        while (*p && *p != 10) { *q = *p; ++q; ++p; }
        if (*p) { *q = *p; ++q; ++p; }
    }
    *q = 0;
    if (q > out && q[-1] != 10) ustrcat(out, "\n");
    ustrcat(out, "swap "); ustrcat(out, img); ustrcat(out, "\n");
    if ((fd = uopen(path_buf, O_WR + O_CREATE + O_TRUNCATE)) < 0) { ufprintf(STDERR, "vm: cannot write %s\n", path_buf); return 1; }
    write(fd, out, ustrlen(out));
    uclose(fd);
    uprintf("vm: %s: swap %s, %d MB, from its next start\n", name, img, mb);
    return 0;
}

static int usage() {
    ufprintf(STDERR, "usage: vm [new NAME KERNEL BOOT.tar MB [ARGS...] | disk ID IMAGE | cdrom ID ISO |\n");
    ufprintf(STDERR, "          start ID | stop ID [-f] | pause ID | resume ID | attach ID | detach | delete NAME [-y] |\n");
    ufprintf(STDERR, "          info ID | send ID TEXT |\n");
    ufprintf(STDERR, "          mkdisk FILE MB | create NAME MB DISK_MB [c4ix|c4ix-text|c4ke|c4dos|ISO] [cpus=N] [swap=MB] [autostart] |\n");
    ufprintf(STDERR, "          images | mkswap NAME MB | up NAME | autostart | log]\n");
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
    if (argc > 1 && !ustrcmp(argv[1], "create")) {
        // NAME MB DISK_MB [SYSTEM|ISO], then cpus=N, swap=MB, autostart
        char *sys;
        int cpus, swapmb, autos, a;
        if (argc < 5) return usage();
        sys = 0; cpus = 1; swapmb = 0; autos = 0;
        a = 5;
        if (a < argc && ustrncmp(argv[a], "cpus=", 5) && ustrncmp(argv[a], "swap=", 5) && ustrcmp(argv[a], "autostart")) { sys = argv[a]; ++a; }
        while (a < argc) {
            if (!ustrncmp(argv[a], "cpus=", 5)) cpus = uatoi(argv[a] + 5);
            else if (!ustrncmp(argv[a], "swap=", 5)) swapmb = uatoi(argv[a] + 5);
            else if (!ustrcmp(argv[a], "autostart")) autos = 1;
            else return usage();
            ++a;
        }
        if (cpus < 1 || cpus > 8) { ufprintf(STDERR, "vm: cpus=1 to 8\n"); return 1; }
        if (create(argv[2], uatoi(argv[3]), uatoi(argv[4]), sys, cpus, autos)) return 1;
        return swapmb > 0 ? mkswap(argv[2], swapmb) : 0;
    }
    if (argc > 1 && !ustrcmp(argv[1], "images")) return argc == 3 ? vm_images(argv[2]) : images();
    if (argc > 1 && !ustrcmp(argv[1], "mkswap")) return argc == 4 ? mkswap(argv[2], uatoi(argv[3])) : usage();
    // a guest's own: detach (c4pc's C4I_HVG)
    if (argc > 1 && !ustrcmp(argv[1], "detach") && (__c4_info() & 0x800000)) { hv(C_DETACH, 0, 0, 0, 0); return 0; }
    if (!there()) { ufprintf(STDERR, "vm: no hypervisor (c4pc --hv: GRUB's \"C4PC hypervisor\")\n"); return 1; }
    if (argc < 2) { list(); return 0; }
    cmd = argv[1];
    if (!ustrcmp(cmd, "detach")) { hv(C_DETACH, 0, 0, 0, 0); return 0; }
    if (!ustrcmp(cmd, "autostart")) return autostart();
    if (!ustrcmp(cmd, "log")) return show_log();
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
    if (!ustrcmp(cmd, "delete")) return delete(argv[2], argc > 3 && !ustrcmp(argv[3], "-y"));
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
    else if (!ustrcmp(cmd, "pause")) k = hv(C_PAUSE, id, 0, 0, 0);
    else if (!ustrcmp(cmd, "resume")) k = hv(C_RESUME, id, 0, 0, 0);
    else if (!ustrcmp(cmd, "info")) { show(id); return 0; }
    else return usage();
    if (k < 0) { ufprintf(STDERR, "vm: %s %d: no (%d)\n", cmd, id, k); return 1; }
    return 0;
}
