//
// C4IX syscalls: the kernel side of the userland boundary.
//
// Two doors lead here, and both end in sys_dispatch:
//
//   1. The syscall gateway. Userland executes custom opcode SYS_*
//      (>= 200), which c4m does not implement, so it raises
//      TRAP_ILLOP into sched.c's handler. On plain c4 there is no
//      trap machinery at all, so libc4ix calls sys_dispatch through
//      the systable the loader injected -- same function, same
//      numbers, different door.
//
//   2. A protected task executing a host syscall opcode directly.
//      c4m raises TRAP_PM_VIOLATION instead of running it, and
//      sys_pmviolation emulates the opcode ON TOP OF the syscall
//      layer. That is what makes redirection universal: a program
//      that never heard of C4IX and just calls printf still has its
//      output go through the fd its parent gave it.
//
// Everything below fd-level goes through vfs.c. A syscall that must
// block (reading an empty pipe) parks the task and asks the trap
// handler to rewind the pc by one word, so the task re-executes the
// syscall opcode when it wakes -- its arguments are untouched on its
// own stack, so the call simply happens again.
//

#include "c4ix.h"

int sys_restart;    // set when the current syscall must be re-issued

// Park the running task on a vnode. In trap context the handler
// rewinds the pc; in kernel context the caller spins on yield, which
// costs one context switch rather than one syscall.
static int sys_block(struct vnode *vn, int pos) {
    struct task *t;
    t = sched_current();
    t->block_vn = vn;
    t->block_pos = pos;
    task_state(t, TS_BLOCKED);
    if (sched_in_trap()) { sys_restart = 1; return 1; }
    while (t->state == TS_BLOCKED) sched_yield();
    return 0;
}

// Sleep MS ms and run the syscall again (TCP's waits for room to write
// and for a handshake, which nothing wakes a blocked task for)
static int sys_nap(int ms) {
    struct task *t;
    int until;
    t = sched_current();
    if (sched_in_trap()) {
        t->ev_wait = 0;
        t->ck_wake = __time() + ms;
        task_state(t, TS_SLEEPING);
        sys_restart = 1;
        return 1;
    }
    until = __time() + ms;
    while (__time() - until < 0) sched_yield();
    return 0;
}

int sys_write(int fd, char *buf, int len) {
    struct file *f;
    struct task *t;
    int r;
    t = sched_current();
    if (!(f = fd_get(t, fd))) return -1;
    if (f->vn->type != VN_SOCK) return vfs_write(f, buf, len);
    // a TCP connection's send ring full: the writer waits for room, a ms at a time
    while ((r = net_write(f->vn->host, buf, len)) == NET_AGAIN) {
        if (f->flags & C4IX_O_NONBLOCK) return -1;
        if (sys_nap(1)) return 0;                 // trap path: restarting
        if (!(f = fd_get(t, fd)) || f->vn->type != VN_SOCK) return -1;
    }
    return r;
}

int sys_read(int fd, char *buf, int len) {
    struct file *f;
    struct task *t;

    t = sched_current();
    if (!(f = fd_get(t, fd))) return -1;
    while (!vfs_readable(f->vn, f->pos)) {
        // non-blocking: nothing yet is -1 (EAGAIN), and the caller polls
        if (f->flags & C4IX_O_NONBLOCK) return -1;
        if (sys_block(f->vn, f->pos)) return 0;   // trap path: restarting
        if (!(f = fd_get(t, fd))) return -1;      // table may have moved on
    }
    return vfs_read(f, buf, len);
}

// A read or write of a disk's file, a step of SYS_STEP bytes a trap. The
// kernel serves a trap start to finish, holding the kernel lock, and
// nothing else runs on its CPU meanwhile -- nor anything that needs the
// lock, on any: vm mkdisk's writes of a MB were 2.6M cycles of ext2 and the
// disk's time each, against a slice of 50K, so it had nearly all of the
// machine while it wrote 1.5 GB, and the desktop and the shells froze. So
// a call for more is the same syscall again for what is left (sys_restart,
// its buffer and length moved on in the task's own arguments), and the
// turns go round between its steps. The program sees one call for the
// whole: the steps' bytes are counted (t->stepdone) and given at the last,
// or where a step falls short (a read at the end of its file). A write's
// metadata is committed once, at the last (vfs_nocommit: write-through
// files). Writes step on an ext2 file (vfs_on_disk); reads on one still on
// a disk or a drive (vfs_from_device), whose bytes the device brings.
enum { SYS_STEP = 131072 };
static int sys_steps(struct task *t, int *args, int wr) {
    struct file *f;
    int n, r, d;
    n = args[2];
    // a step left over from a call that did not come back for the rest
    // (killed, or a signal's handler in between): not this one's
    if (t && t->stepdone && t->stepnext != args[1]) t->stepdone = 0;
    if (n > SYS_STEP && t && sched_in_trap() && (f = fd_get(t, args[0])) && (wr ? vfs_on_disk(f) : vfs_from_device(f))) {
        if (wr) {
            vfs_nocommit = 1;
            r = vfs_write(f, (char *)args[1], SYS_STEP);
            vfs_nocommit = 0;
        } else r = vfs_read(f, (char *)args[1], SYS_STEP);
        if (r == SYS_STEP) {
            t->stepdone = t->stepdone + r;
            args[1] = args[1] + r;
            args[2] = n - r;
            t->stepnext = args[1];
            sys_restart = 1;
            return 0;
        }
        if (wr) vfs_commit_file(f);
        d = t->stepdone;
        t->stepdone = 0;
        if (r < 0) return d ? d : 0 - 1;
        return d + r;
    }
    // served whole: the rest of the buffer checked too (sys_args_ok checked a step's)
    if (n > SYS_STEP && !uaccess(args[1] + SYS_STEP, n - SYS_STEP, !wr)) r = 0 - 1;
    else r = wr ? sys_write(args[0], (char *)args[1], n) : sys_read(args[0], (char *)args[1], n);
    if (t && t->stepdone) {
        d = t->stepdone;
        t->stepdone = 0;
        r = r < 0 ? d : d + r;
    }
    return r;
}

static int sys_same(char *a, char *b) {
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}

// RAM files first, host files second -- the same precedence C4KE's
// loader uses, and the reason a program can be handed a "file" that
// only ever existed in memory.
//
// /dev/stdin and /dev/tty are how a C4KE program reads its keyboard
// without stopping the machine: c4sh, raycast and type open one
// non-blocking and poll it. The host's answer is the machine's console,
// which is right for a task whose stdin is the console. A task whose
// stdin is a pipe -- anything run in a Terminal, c4m booting C4KE
// there among them -- means that pipe: nobody types at the console for
// it, and C4KE's shell would wait for ever. So for such a task they name
// its own stdin, opened again read-only with the caller's O_NONBLOCK.
int sys_open(char *path, int flags) {
    struct vnode *vn;
    struct task *t;
    struct file *in;
    int h, fd;

    t = sched_current();
    if ((sys_same(path, "/dev/stdin") || sys_same(path, "/dev/tty"))
        && (in = fd_get(t, FD_STDIN)) && in->vn->type != VN_CONSOLE)
        return fd_open_vnode(t, in->vn, C4IX_O_RDONLY | (flags & C4IX_O_NONBLOCK));
    // O_CREAT with O_EXCL: only a file made now. One trap, so nothing can
    // make it between the look and the create.
    if ((flags & C4IX_O_EXCL) && (flags & C4IX_O_CREAT)) {
        if (vfs_lookup(path)) return -1;
        if ((h = open(path, 0)) >= 0) { close(h); return -1; }
    }
    if ((vn = vfs_lookup(path))) {
        if (vn->type == VN_PROC && (flags & 3)) return -1;   // /proc/uptime is read-only
        if (flags & C4IX_O_TRUNC) vfs_truncate(vn, 0);   // the host bytes are not wanted
        return fd_open_vnode(t, vn, flags);
    }
    if (flags & C4IX_O_CREAT) {
        if (!(vn = vfs_ramfile(path))) return -1;
        return fd_open_vnode(t, vn, flags);
    }
    // Mask to the flags the host may see, but O_NONBLOCK must survive:
    // dropping it silently turns a caller's non-blocking descriptor
    // into a blocking one, and vfs_read on a VN_HOSTFILE goes straight
    // to the host read(), which halts every task in the system rather
    // than parking one. vfs_readable already answers 1 unconditionally
    // for host files, so nothing parks on this path either way, and a
    // -1/EAGAIN return is what a non-blocking reader already expects.
    if ((h = open(path, (flags & 3) | (flags & C4IX_O_NONBLOCK))) < 0) return -1;
    if (!(vn = vn_hostfile(h))) { close(h); return -1; }
    if ((fd = fd_open_vnode(t, vn, flags)) < 0) { close(h); return -1; }
    return fd;
}

int sys_close(int fd) {
    return fd_close(sched_current(), fd);
}

int sys_dup(int fd) {
    struct task *t;
    int n;
    t = sched_current();
    if (!fd_get(t, fd)) return -1;
    n = 0;
    while (n < FD_MAX) {
        if (!fd_get(t, n)) return fd_dup2(t, fd, n);
        ++n;
    }
    return -1;
}

int sys_dup2(int oldfd, int newfd) {
    return fd_dup2(sched_current(), oldfd, newfd);
}

// SYS_FDGIVE: give task PID -- one T started, or one its tasks did, as
// for SYS_MAPFROM -- T's descriptor FD as its descriptor TO, as dup2 would
// there: what TO was is closed. The desktop's Terminal gives a program
// that ran in the background from it the pipes of a window of its own
// (term.c, term_adopt; window.c, win_open). Every syscall holds the
// kernel lock, so the table is not in use; a task parked reading what TO
// was is woken, and its read, which starts again, takes what TO is now.
// TO, or -1.
static int sys_fdgive(struct task *t, int pid, int fd, int to) {
    struct task *c, *p;
    int n;
    if (!t || !(c = task_get(pid)) || c == t || c->state == TS_ZOMBIE || c->privs != PRIV_USER) return -1;
    p = c; n = 0;
    while (p && p != t && n < 64) { p = p->parent ? task_get(p->parent) : 0; ++n; }
    if (p != t) return -1;
    if (fd_give(t, fd, c, to) < 0) return -1;
    if (c->state == TS_BLOCKED) { task_state(c, TS_READY); sched_wake = 1; }
    return to;
}

int sys_pipe(int *fds) {
    struct vnode *vn;
    struct task *t;
    int r, w;

    t = sched_current();
    if (!(vn = vfs_pipe())) return -1;
    if ((r = fd_open_vnode(t, vn, C4IX_O_RDONLY)) < 0) return -1;
    if ((w = fd_open_vnode(t, vn, C4IX_O_WRONLY)) < 0) { fd_close(t, r); return -1; }
    fds[0] = r;
    fds[1] = w;
    return 0;
}

// Snapshot of the index'th task, for a userland `ps`. Walking the
// list from userland is not an option -- that is kernel memory --
// so the kernel copies out a fixed-shape record instead.
int sys_taskinfo(int index, int *out) {
    struct task *t;
    char *dst;
    int i;
    long long c;

    t = task_first();
    i = 0;
    while (t && i < index) { t = t->next; ++i; }
    if (!t) return 0;

    out[0] = t->id;
    out[1] = t->parent;
    out[2] = t->state;
    out[3] = t->privs;
    out[4] = t->nsyscalls;
    out[5] = t->ntraps;
    // Cycles, in the two words this record has always had: billions,
    // and the rest. (The caller's own run up to this syscall is in it:
    // the trap charged that on the way in.)
    c = task_cycles(t);
    out[7] = (int)(c / 1000000000);
    out[6] = (int)(c - (long long)out[7] * 1000000000);
    dst = (char *)(out + 8);
    i = 0;
    while (i < TASK_NAME_MAX) { dst[i] = t->name[i]; ++i; }
    out[TI_CPU] = task_cpu(t);
    out[TI_NCPU] = smp_ncpu > 1 ? smp_ncpu : 1;
    return 1;
}

int sys_chdir(char *path) { return vfs_chdir(path); }

int sys_mkdir(char *path) {
    if (!vfs_mkdir(path)) return -1;
    return 0;
}

// Build the absolute path of the working directory by walking
// parents to the root and then emitting the components in reverse.
int sys_getcwd(char *buf, int len) {
    struct vnode *chain[16];
    struct vnode *vn;
    int n, i, j, k;

    vn = vfs_cwd();
    n = 0;
    while (vn != vfs_root() && n < 16) { chain[n] = vn; ++n; vn = vn->parent; }
    if (!n) {
        if (len < 2) return -1;
        buf[0] = '/'; buf[1] = 0;
        return 1;
    }
    k = 0;
    i = n;
    while (i) {
        --i;
        if (k < len - 1) { buf[k] = '/'; ++k; }
        j = 0;
        while (chain[i]->name[j]) {
            if (k < len - 1) { buf[k] = chain[i]->name[j]; ++k; }
            ++j;
        }
    }
    buf[k] = 0;
    return k;
}

// One directory entry per call: returns 1 for a directory, 0 for a
// file, -1 past the end -- so userland never sees a kernel pointer.
int sys_readdir(char *path, int index, char *name) {
    struct vnode *dir;
    int isdir;
    if (!(dir = vfs_lookup(path))) return -1;
    if (!vfs_direntry(dir, index, name, &isdir)) return -1;
    return isdir;
}

// ---- execve and waitpid ----

// Replace the calling task's program: PATH's image, with ARGV and ENVP
// copied into the kernel, a fresh stack, the same id, descriptors and
// directory (less the close-on-exec ones), and the default signal
// handling. The trap then returns into the new image's task_shim, not
// to the caller (sched_reload). A vfork child that execs gives its
// parent its memory back. -1, with nothing changed, if the image will
// not load.
//
// Everything the call names is the old image's memory, or the vfork
// parent's, so it is all copied, and the task renamed, before any of
// that is freed. The old stack is the one this handler runs on, so it
// goes at the next trap.
static int sys_execve(char *path, char **argv, char **envp) {
    struct c4r_image img;
    struct task *t;
    char *name;
    int argc, av, ad, ev, ed, ostack, ok, i;

    t = sched_current();
    if (!t || !sched_in_trap() || t->privs != PRIV_USER) return -1;
    loader_uva = mmu_on;
    ok = c4r_load(path, &img);
    loader_uva = 0;
    if (!ok) return -1;
    if ((argc = task_strvec(argv, -1, &av, &ad)) < 0) { c4r_unload(&img); return -1; }
    if (task_strvec(envp, -1, &ev, &ed) < 0) {
        if (av) { ufree(av); ufree(ad); }
        c4r_unload(&img);
        return -1;
    }
    // A vfork child's stack is its parent's choice for its children; any
    // other task keeps the size it has
    ostack = t->stack;
    if (t->vfparent) sched_next_stack_words = t->spawn_stack ? t->spawn_stack / sizeof(int) : 0;
    else sched_next_stack_words = t->stack_words;
    ok = sched_forge(t, img.entry, argc, av);
    sched_next_stack_words = 0;
    if (!ok) {
        if (av) { ufree(av); ufree(ad); }
        if (ev) { ufree(ev); ufree(ed); }
        c4r_unload(&img);
        return -1;
    }
    name = path;
    i = 0;
    while (path[i]) { if (path[i] == '/') name = path + i + 1; ++i; }
    i = 0;
    while (name[i] && i < TASK_NAME_MAX - 1) { t->name[i] = name[i]; ++i; }
    t->name[i] = 0;

    task_drop_image(t);
    ck_exec_reset(t);
    if (t->argv_vec) ufree(t->argv_vec);
    if (t->argv_data) ufree(t->argv_data);
    if (t->env_vec) ufree(t->env_vec);
    if (t->env_data) ufree(t->env_data);
    t->argv_vec = av; t->argv_data = ad;
    t->env_vec = ev; t->env_data = ed;
    fd_close_on_exec(t);
    t->entry = img.entry;
    t->img_code = img.code;
    t->img_data = img.data;
    t->img_cons = img.cons;
    t->img_ncons = img.ncons;
    t->img_des = img.des;
    t->img_ndes = img.ndes;
    t->img_libs = img.libs;
    t->img_nlibs = img.nlibs;
    t->img_bytes = img.bytes;
    if (ostack) sched_stale_stack = ostack;
    // the new image, stack and arguments are its own, and it starts in
    // a page of its own (mmu.c)
    mmu_adopt(t);
    // no memory for the page it starts in: the old image is gone, so it ends
    if (!mmu_user_entry(t)) {
        kprintf("c4ix: out of memory starting %s\n", t->name);
        task_exit(-1);
        return 0;
    }
    sched_reload = 1;
    sched_new_slice();
    return 0;
}

// waitpid(PID, ST, OPTIONS): a child that has exited, PID or (-1) any,
// is reaped, and its id returned with ST[0] its exit code and ST[1] the
// signal that killed it (0 if none). With C4IX_WNOHANG, 0 while none
// has; -1 if the caller has no such child. Otherwise the caller parks
// until one exits, and runs the call again then (task_zombify wakes it
// without reaping, and sys_restart re-executes the syscall). There are
// no process groups, so 0 and below -1 mean any child.
static int sys_waitpid(int pid, int *st, int opts) {
    struct task *t, *c;
    int any, id, code, sig;

    if (!(t = sched_current())) return -1;
    t->posix = 1;
    if (pid < -1 || pid == 0) pid = -1;
    any = 0;
    c = task_head;
    while (c) {
        if (c->parent == t->id && c != t && (pid == -1 || c->id == pid)) {
            if (c->state == TS_ZOMBIE) {
                if (st) { st[0] = c->exitcode; st[1] = c->killsig; }
                id = c->id;
                task_release(c);
                return id;
            }
            any = 1;
        }
        c = c->next;
    }
    // reaped by the idle sweep before this parent asked
    if ((id = task_ghost_child(t->id, pid, &code, &sig))) {
        if (st) { st[0] = code; st[1] = sig; }
        return id;
    }
    if (!any) return -1;
    if ((opts & C4IX_WNOHANG) || !sched_in_trap()) return 0;
    t->wait_for = pid;
    t->wait_posix = 1;
    task_state(t, TS_WAITING);
    sys_restart = 1;
    return 0;
}

// ---- the dispatcher ----
//
// args[0] is the first argument, args[1] the second, and so on.
// Both doors normalize into that shape before calling.
//
// ---- The display, taken and given back ------------------------------------
//
// libjs's display (libjs/guest/gui.h) serves one ring at a time: the
// program that writes ATTACH last has it, and nothing in the machine stops
// a second one doing so while the first still draws. So a program that
// wants the whole display asks here first (include/display.h), and the
// kernel keeps who has it -- the way Windows 3 to 98 let a DOS game have
// the whole screen, the desktop waiting until it was done.
//
// The holder may agree to give way (DISP_YIELDS): the desktop does. A
// program that asks then waits, asking again, while the holder is told
// (DISP_STATE answers 1) and woken; at its next turn it yields, and runs
// on without drawing. It has the display back when the new holder gives
// it up or dies (task_zombify), and DISP_STATE answers 0 again. A holder
// that has not given way after DISP_GRACE is passed over, so a stuck
// desktop cannot keep a game off the screen; one that did not agree to
// give way is never asked, and a second program is told no. One level
// deep: the desktop under a game, never a game under a game.
//
// Alt+Tab, where the display keeps screens (its CAPS' GUI_CAP_SCREENS:
// libjs/gui-device.js, c4pc's gui.c). Each program that has the whole
// display draws on a screen of its own: its rings, its canvas, its
// pictures and pointer, which the display puts away whole when another is
// shown and brings back as they were (GUI_SCREEN). The kernel picks the
// screens: the first holder's is the one shown, and a program the holder
// gives way to gets one nobody has. While a game has the display over the
// desktop, the display watches for Alt+Tab (GUI_SWITCH armed) and does not
// give it to the game; each ms (sched_tick, sys_display_tick) the kernel
// looks, and when it was pressed, parks the game: it is held (TF_PARKED,
// not run, as a DOS game Windows 3 put in the background was suspended),
// its screen put away, and the desktop has the display back as if the game
// had given it up. The desktop lists the parked (DISP_PARKED) beside its
// windows, and brings one back (DISP_RESUME), giving way to it as to a
// game that asks: the game's screen is shown again as it left it, and the
// game runs on from where it stopped, never having known. A parked game
// that dies, or is killed, has its screen forgotten (GUI_SCRDROP).
// DISP_NPARK at once; a fourth game's Alt+Tab does nothing. While a game
// is parked the sound card is muted (its master's bit 8): a game stopped
// mid-tune leaves the AdLib's notes held, a drone under the desktop (Doom
// measured so); resumed, it is unmuted, unless the desktop's volume
// control set the master meanwhile.
enum { DISP_GRACE = 1000, DISP_NPARK = 3, DISP_NSCR = 4 };
static int disp_holder, disp_hyields, disp_hscr;    // who has it; gives way?; its screen
static int disp_prev, disp_pyields, disp_pscr;      // who gave way to it
static int disp_want, disp_wyields, disp_want_at;   // who asks, and since when
static int disp_park[DISP_NPARK], disp_kyields[DISP_NPARK], disp_kscr[DISP_NPARK];   // parked,
static int disp_npark;                              // the last parked first
static int disp_shown;                              // the screen the display shows
static int disp_caps;                               // 1 it keeps screens, -1 not, 0 not yet asked
int disp_armed;                                     // GUI_SWITCH armed: sched_tick looks
static int disp_snd = 0 - 1;                        // the master before a park muted it, or -1

static int disp_screens() {
    if (!disp_caps) disp_caps = host_has(C4IX_I_GUI) && (*(int *)C4IX_GUI_CAPS & C4IX_GUI_CAP_SCREENS) ? 1 : 0 - 1;
    return disp_caps > 0;
}
// screen S on the display, and S forgotten (not the one shown)
static void disp_show(int s) {
    if (!disp_screens() || s == disp_shown) return;
    *(int *)C4IX_GUI_SCREEN = s;
    disp_shown = s;
}
static void disp_forget(int s) {
    if (disp_screens() && s != disp_shown) *(int *)C4IX_GUI_SCRDROP = s;
}
// The sound muted while a game is parked; back as it was when none is,
// unless something else set the master since
static void disp_quiet() {
    int v;
    if (!host_has(C4IX_I_SND)) return;
    if (disp_npark && disp_snd < 0 && disp_holder && !disp_prev) {
        disp_snd = *(int *)C4IX_SND_MASTER;
        *(int *)C4IX_SND_MASTER = disp_snd | C4IX_SND_MUTE;
    } else if (disp_snd >= 0 && !(disp_npark && !disp_prev)) {
        v = *(int *)C4IX_SND_MASTER;
        if (v == (disp_snd | C4IX_SND_MUTE)) *(int *)C4IX_SND_MASTER = disp_snd;
        disp_snd = 0 - 1;
    }
}

// Alt+Tab watched for while a game has the display over one who gave way
static void disp_arm() {
    int on;
    if (!disp_screens()) return;
    on = disp_holder && disp_prev ? 1 : 0;
    if (on != disp_armed) { disp_armed = on; *(int *)C4IX_GUI_SWITCH = on; }
    disp_quiet();
}
// a screen nobody has: not the holder's, nor who gave way's, nor a parked game's
static int disp_free_screen() {
    int s, k, used;
    s = 0;
    while (s < DISP_NSCR) {
        used = (disp_holder && s == disp_hscr) || (disp_prev && s == disp_pscr);
        k = 0;
        while (k < disp_npark) { if (disp_kscr[k] == s) used = 1; ++k; }
        if (!used) return s;
        ++s;
    }
    return 0;
}

// the K'th parked off the list
static void disp_unpark(int k) {
    --disp_npark;
    while (k < disp_npark) {
        disp_park[k] = disp_park[k + 1]; disp_kyields[k] = disp_kyields[k + 1]; disp_kscr[k] = disp_kscr[k + 1];
        ++k;
    }
}

// A holder asleep on the clock or its event ring looks now
static void disp_nudge(int id) {
    struct task *w;
    // (again on its queue: the clock's is in order of deadline)
    if ((w = task_get(id)) && w->state == TS_SLEEPING) { w->ck_wake = __time(); task_state(w, TS_SLEEPING); }
}

static void disp_pass() {
    int s;
    s = disp_free_screen();
    disp_prev = disp_holder; disp_pyields = disp_hyields; disp_pscr = disp_hscr;
    disp_holder = disp_want; disp_hyields = disp_wyields; disp_hscr = s;
    disp_want = 0;
    disp_show(s);
    disp_arm();
}

// A task gives the display up, or dies having it, waiting under a game
// for it, parked, or asking for it. The holder's screen is forgotten once
// another is shown; with nobody under it, a parked game has the display.
void sys_display_drop(int id) {
    struct task *t;
    int k, s;
    if (!id) return;
    if (disp_want == id) disp_want = 0;
    if (disp_prev == id) { disp_prev = 0; disp_forget(disp_pscr); }
    k = 0;
    while (k < disp_npark) { if (disp_park[k] == id) { disp_forget(disp_kscr[k]); disp_unpark(k); } else ++k; }
    if (disp_holder == id) {
        s = disp_hscr;
        if (disp_prev) { disp_holder = disp_prev; disp_hyields = disp_pyields; disp_hscr = disp_pscr; disp_prev = 0; }
        else {
            disp_holder = 0;
            if (disp_npark && (t = task_get(disp_park[0]))) {
                disp_holder = disp_park[0]; disp_hyields = disp_kyields[0]; disp_hscr = disp_kscr[0];
                disp_unpark(0);
                TF_CLR(t, TF_PARKED);
                sched_wake = 1;
            }
        }
        if (disp_holder) { disp_show(disp_hscr); disp_forget(s); disp_nudge(disp_holder); }
    }
    disp_arm();
}

// Each ms, while Alt+Tab is watched for (sched_tick): pressed, the game
// parked and the display the desktop's again
void sys_display_tick() {
    struct task *t;
    int k;
    if (!disp_armed || !*(int *)C4IX_GUI_SWITCH) return;
    *(int *)C4IX_GUI_SWITCH = 1;                    // taken: still watched for
    if (disp_npark == DISP_NPARK || !disp_prev || !(t = task_get(disp_holder))) return;
    k = disp_npark;
    while (k > 0) {
        disp_park[k] = disp_park[k - 1]; disp_kyields[k] = disp_kyields[k - 1]; disp_kscr[k] = disp_kscr[k - 1];
        --k;
    }
    ++disp_npark;
    disp_park[0] = disp_holder; disp_kyields[0] = disp_hyields; disp_kscr[0] = disp_hscr;
    TF_SET(t, TF_PARKED);
    disp_holder = disp_prev; disp_hyields = disp_pyields; disp_hscr = disp_pscr;
    disp_prev = 0;
    disp_show(disp_hscr);
    disp_arm();
    disp_nudge(disp_holder);
    sched_wake = 1;
}

// RESUME: the holder, who gives way, gives way to parked game ID: 1 if it did
static int disp_resume(int me, int id) {
    struct task *t;
    int k;
    if (!id || disp_holder != me || disp_prev || !disp_hyields) return 0;
    k = 0;
    while (k < disp_npark && disp_park[k] != id) ++k;
    if (k == disp_npark || !(t = task_get(id))) return 0;
    disp_prev = disp_holder; disp_pyields = disp_hyields; disp_pscr = disp_hscr;
    disp_holder = id; disp_hyields = disp_kyields[k]; disp_hscr = disp_kscr[k];
    disp_unpark(k);
    TF_CLR(t, TF_PARKED);
    sched_wake = 1;
    disp_show(disp_hscr);
    disp_arm();
    return 1;
}

// TAKE: 1 it is the caller's, 0 ask again soon, -1 no. STATE: 0 the
// caller has it, 1 it has it and another asks, 2 it has not. YIELD: 1 it
// passed to the one asking. GIVE: it goes back to who gave way, if anyone.
// PARKED: the id of the FLAGS'th parked game (the last parked first), 0
// past the last. RESUME:
// 1 the display passed to parked game FLAGS.
static int sys_display(struct task *t, int op, int flags) {
    int me;
    if (!t) return -1;
    me = t->id;
    if (op == DISP_PIN) { sched_softpin(t); return 0; }
    if (op == DISP_TAKE) {
        sched_softpin(t);
        if (!disp_holder || disp_holder == me) {
            if (!disp_holder) disp_hscr = disp_shown;
            disp_holder = me; disp_hyields = flags & DISP_YIELDS;
            if (disp_want == me) disp_want = 0;
            return 1;
        }
        if (!disp_hyields || disp_prev) return -1;
        if (disp_want && disp_want != me) return -1;
        if (disp_want != me) { disp_want = me; disp_wyields = flags & DISP_YIELDS; disp_want_at = __time(); }
        if (__time() - disp_want_at >= DISP_GRACE) { disp_pass(); return 1; }
        disp_nudge(disp_holder);
        return 0;
    }
    if (op == DISP_STATE) {
        if (disp_holder != me) return 2;
        return disp_want ? 1 : 0;
    }
    if (op == DISP_YIELD) {
        if (disp_holder != me || !disp_want) return 0;
        disp_pass();
        return 1;
    }
    if (op == DISP_GIVE) { sys_display_drop(me); return 0; }
    if (op == DISP_PARKED) return flags >= 0 && flags < disp_npark ? disp_park[flags] : 0;
    if (op == DISP_RESUME) return disp_resume(me, flags);
    return -1;
}

// A switch: c4lc compiles one to a jump table, a fixed cost, where a
// ladder of ifs cost six instructions a rung -- SYS_SLEEP, the desktop's
// most frequent call, was the twenty-fourth.
// Under the MMU, whatever a syscall is given to read or write must be
// the caller's (mmu.c uaccess): a bad pointer is -1, Linux's EFAULT,
// never a read or a write of someone else's memory by the kernel. (The
// kernel's own calls to sys_write and the rest pass its own buffers, so
// the checks are here and in sys_pmviolation, not in them.)
static int sys_args_ok(int num, int *args) {
    int k;
    if (!mmu_on) return 1;
    k = (int)args;
    if (!UARGS_OK(k)) return 0;
    switch (num) {
    // a read or write's first step only: each step is checked as it comes
    // (sys_steps), and one served whole has the rest checked there. Checked
    // whole at each step, a read of 53 MB walked its 13,000 pages 416 times
    case SYS_WRITE:    return uaccess(args[1], args[2] > SYS_STEP ? SYS_STEP : args[2], 0);
    case SYS_READ:     return uaccess(args[1], args[2] > SYS_STEP ? SYS_STEP : args[2], 1);
    case SYS_OPEN:     return ustrlen((char *)args[0]) >= 0;
    case SYS_PIPE:     return uaccess(args[0], 2 * sizeof(int), 1);
    case SYS_TASKINFO: return uaccess(args[1], TASKINFO_WORDS * sizeof(int), 1);
    case SYS_CPUSTAT:  return uaccess(args[0], sizeof(struct cpustat), 1) &&
                              uaccess(args[1], args[2] * sizeof(struct cpustat_task), 1);
    case SYS_CHDIR: case SYS_MKDIR: case SYS_UNLINK:
                       return ustrlen((char *)args[0]) >= 0;
    case SYS_GETCWD:   return uaccess(args[0], args[1], 1);
    case SYS_READDIR:  return ustrlen((char *)args[0]) >= 0 && uaccess(args[2], VN_NAME_MAX, 1);
    case SYS_STAT:     return ustrlen((char *)args[0]) >= 0 && uaccess(args[1], (args[2] ? args[2] : 4) * sizeof(int), 1);
    case SYS_FSTAT:    return uaccess(args[1], (args[2] ? args[2] : 4) * sizeof(int), 1);
    case SYS_LINK: case SYS_RENAME:
                       return ustrlen((char *)args[0]) >= 0 && ustrlen((char *)args[1]) >= 0;
    case SYS_LAZYFILE: case SYS_RELAZY:
                       return ustrlen((char *)args[0]) >= 0 && ustrlen((char *)args[1]) >= 0;
    case SYS_WAITPID:  return !args[1] || uaccess(args[1], 2 * sizeof(int), 1);
    case SYS_SPAWN:    return ustrlen((char *)args[0]) >= 0 && uvecok((char **)args[2], args[1]);
    case SYS_EXECVE:   return ustrlen((char *)args[0]) >= 0 && uvecok((char **)args[1], -1) &&
                              uvecok((char **)args[2], -1);
    case SYS_EVWAIT:   return !args[1] || uaccess(args[1], 3 * sizeof(int), 0);
    case SYS_MOUNT:    return ustrlen((char *)args[1]) >= 0;
    case SYS_UMOUNT: case SYS_CHMOD:
                       return ustrlen((char *)args[0]) >= 0;
    case SYS_MKFS:     return !args[2] || ustrlen((char *)args[2]) >= 0;
    case SYS_BLKIO:    return uaccess(args[2], (((args[0] >> 16) & 255) + 1) * 512, (args[0] & 256) ? 0 : 1);
    case SYS_STATFS:   return ustrlen((char *)args[0]) >= 0 && uaccess(args[1], 7 * sizeof(int), 1);
    case SYS_LSEEK64:  return uaccess(args[1], 2 * sizeof(int), 1);
    case SYS_FMAP:     return uaccess(args[2], 3 * sizeof(int), 1);
    case SYS_SENDTO:   return uaccess(args[1], 4 * sizeof(int), 0) && uaccess(((int *)args[1])[0], ((int *)args[1])[1], 0);
    case SYS_RECVFROM: return uaccess(args[1], 5 * sizeof(int), 1) && uaccess(((int *)args[1])[0], ((int *)args[1])[1], 1);
    case SYS_NETCTL:   return args[0] == 0 || args[0] == 3 ? uaccess(args[1], args[2] * sizeof(int), 1) : (args[0] != 2 || ustrlen((char *)args[1]) >= 0);
    case SYS_ACCEPT:   return !args[1] || uaccess(args[1], 2 * sizeof(int), 1);
    }
    return 1;
}

int sys_dispatch(int num, int *args) {
    struct task *t, *s;
    struct file *f;
    c4off off;
    int *w;
    int fm_spb, fm_b, fm_dev, fm_d;     // SYS_FMAP's
    int ns, nr;                         // the network's
    struct vnode *nv;

    if ((t = sched_current())) ++t->nsyscalls;
    if (!sys_args_ok(num, args)) return -1;

    switch (num) {
    case SYS_WRITE:    return sys_steps(t, args, 1);
    case SYS_READ:     return sys_steps(t, args, 0);
    // disks (docs/c4ix-ext2.md): block device args[0]'s ext2 at a path,
    // read-only where args[2]; unmounted; made (block size, label); the
    // counts of the disk a path is on; every change written; a mode
    case SYS_MOUNT:    return vfs_mount(args[0], (char *)args[1], args[2]);
    case SYS_UMOUNT:   return vfs_umount((char *)args[0]);
    case SYS_MKFS:     return ext2_mkfs(args[0], args[1], (char *)args[2]);
    case SYS_STATFS:   return vfs_statfs((char *)args[0], (int *)args[1]);
    case SYS_SYNC:     return vfs_sync();
    case SYS_CHMOD:    return vfs_chmod((char *)args[0], args[1]);
    // the machine off (0) or restarted (1), once every task has gone
    // (sched.c, sched_halt): every disk's changes written first
    case SYS_SHUTDOWN: vfs_sync(); return sched_halt(args[0] ? 1 : 0, "shutdown");
    // sectors of a device to (args[0] + 256) or from a buffer: install.c's
    // MBR, boot loader and system partition
    case SYS_BLKIO:    return blk_user(args[0] & 255, args[1], ((args[0] >> 16) & 255) + 1, (char *)args[2], (args[0] & 256) ? 1 : 0);
    case SYS_OPEN:     return sys_open((char *)args[0], args[1]);
    case SYS_CLOSE:    return sys_close(args[0]);
    case SYS_DUP:      return sys_dup(args[0]);
    case SYS_DUP2:     return sys_dup2(args[0], args[1]);
    case SYS_PIPE:     return sys_pipe((int *)args[0]);
    case SYS_CYCLES:   return __c4_cycles();
    case SYS_TASKINFO: return sys_taskinfo(args[0], (int *)args[1]);
    case SYS_CPUSTAT:
        return sched_cpustat((struct cpustat *)args[0], (struct cpustat_task *)args[1], args[2]);
    case SYS_CHDIR:    return sys_chdir((char *)args[0]);
    case SYS_MKDIR:    return sys_mkdir((char *)args[0]);
    case SYS_GETCWD:   return sys_getcwd((char *)args[0], args[1]);
    case SYS_READDIR:  return sys_readdir((char *)args[0], args[1], (char *)args[2]);
    // Signals are the compat layer's machinery, but they are useful to
    // C4IX's own shell too: `jobs` could list a background task and
    // nothing could stop it. A C4IX program installs no handlers, so
    // for one of those this is simply "cancel that job".
    case SYS_KILL:     return ck_kill(args[0], args[1]);
    case SYS_YIELD:    sched_yield(); return 0;
    // Bytes waiting on fd, without ever blocking: the desktop reads every
    // terminal's pipe from one loop. 0 means empty with a writer still
    // there, -1 end of file (or a bad fd).
    case SYS_AVAIL:
        if (!t || !(f = fd_get(t, args[0]))) return -1;
        if (f->vn->type == VN_PIPE) {
            if (f->vn->size > f->vn->rpos) return f->vn->size - f->vn->rpos;
            return f->vn->writers ? 0 : -1;
        }
        if (f->vn->type == VN_CONSOLE) return con_poll() ? 1 : 0;
        if (f->vn->type == VN_SOCK) return net_avail(f->vn->host);
        if (f->vn->type == VN_RAMFILE || f->vn->type == VN_PROC) return f->vn->size > f->pos ? f->vn->size - f->pos : -1;
        return 1;
    case SYS_INTR:     return sched_intr_below(task_get(args[0]));
    // Drop what a pipe holds unread, from either end: what a tty does with
    // its input queue on Ctrl-C. The desktop's terminals do it, so lines
    // typed at a program that never read them are not run by the shell
    // once it is interrupted. The bytes dropped, or -1 for no pipe.
    case SYS_DISCARD:
        if (!t || !(f = fd_get(t, args[0])) || f->vn->type != VN_PIPE) return -1;
        sched_lock();
        num = f->vn->size - f->vn->rpos;
        f->vn->size = 0; f->vn->rpos = 0;
        sched_unlock();
        return num;
    // the first args[2] words of the ST_* record; 0 asks for the four
    // there were before M7c
    case SYS_STAT:     return vfs_stat((char *)args[0], (int *)args[1], args[2]);
    case SYS_FSTAT:
        if (!t || !(f = fd_get(t, args[0]))) return -1;
        vn_stat(f->vn, (int *)args[1], args[2]);
        return 0;
    // Only a RAM file (or /proc/uptime) has a position to move: a pipe, the console and a
    // host file (the host has no seek) are -1, libc's ESPIPE. Past the
    // end is allowed, and a write there leaves zeros in the gap.
    case SYS_LSEEK:
        if (!t || !(f = fd_get(t, args[0])) || (f->vn->type != VN_RAMFILE && f->vn->type != VN_PROC)) return -1;
        if (args[2] == 0) off = args[1];
        else if (args[2] == 1) off = f->pos + args[1];
        else if (args[2] == 2) off = VN_END(f->vn) + args[1];
        else return -1;
        // past what a word holds (a 32-bit build's 2 GB): lseek64's to say
        if (off < 0 || (sizeof(int) == 4 && off > 0x7FFFFFFF)) return -1;
        f->pos = off;
        return (int)off;
    // the network (net.c): sockets as fds
    case SYS_SOCKET:
        if (!t || (ns = net_socket(args[0])) < 0) return -1;
        if (!(nv = vfs_socket(ns))) { net_close(ns); return -1; }
        if ((nr = fd_open_vnode(t, nv, C4IX_O_RDWR)) < 0) net_close(ns);
        return nr;
    case SYS_BIND: case SYS_SENDTO: case SYS_RECVFROM:
        if (!t || !(f = fd_get(t, args[0])) || f->vn->type != VN_SOCK) return -1;
        if (num == SYS_BIND) return net_bind(f->vn->host, args[1]);
        if (num == SYS_SENDTO) return net_sendto(f->vn->host, (int *)args[1]);
        // a datagram, or a wait for one (as a pipe's reader waits) until the deadline
        w = (int *)args[1];
        while ((nr = net_recvfrom(f->vn->host, w)) == NET_AGAIN) {
            if (w[4] < 0 || (w[4] && __time() - w[4] >= 0)) return -2;
            if (sys_block(f->vn, w[4])) return 0;      // trap path: restarting
            if (!(f = fd_get(t, args[0])) || f->vn->type != VN_SOCK) return -1;
        }
        return nr;
    case SYS_NETCTL:   return net_ctl(args[0], args[1], args[2]);
    // TCP (net.c)
    case SYS_CONNECT: case SYS_LISTEN: case SYS_ACCEPT: case SYS_SHUTSOCK:
        if (!t || !(f = fd_get(t, args[0])) || f->vn->type != VN_SOCK) return -1;
        if (num == SYS_LISTEN) return net_listen(f->vn->host, args[1]);
        if (num == SYS_SHUTSOCK) return net_shutdown(f->vn->host);
        if (num == SYS_CONNECT) {
            // the handshake, waited for a ms at a time
            while ((nr = net_connect(f->vn->host, args[1], args[2])) == NET_AGAIN) {
                if (sys_nap(1)) return 0;         // trap path: restarting
                if (!(f = fd_get(t, args[0])) || f->vn->type != VN_SOCK) return -1;
            }
            return nr;
        }
        // the next connection made, waited for as a pipe's reader waits
        while ((ns = net_accept(f->vn->host, (int *)args[1])) == NET_AGAIN) {
            if (f->flags & C4IX_O_NONBLOCK) return -1;
            if (sys_block(f->vn, 0)) return 0;    // trap path: restarting
            if (!(f = fd_get(t, args[0])) || f->vn->type != VN_SOCK) return -1;
        }
        if (ns < 0) return -1;
        if (!(nv = vfs_socket(ns))) { net_close(ns); return -1; }
        if ((nr = fd_open_vnode(t, nv, C4IX_O_RDWR)) < 0) net_close(ns);
        return nr;
    // where a disk's file keeps a block (c4ix.h SYS_FMAP)
    case SYS_FMAP:
        if (!t || !(f = fd_get(t, args[0])) || !f->vn->e2) return -1;
        fm_b = ext2_fmap(f->vn->e2, f->vn->e2ino, args[1], &fm_spb);
        if (fm_b < 0) return -1;
        fm_dev = ext2_dev(f->vn->e2);
        if ((fm_d = blk_disk(fm_dev)) < 0) return -1;
        w = (int *)args[2];
        w[0] = fm_d; w[1] = fm_b ? blk_start(fm_dev) + fm_b * fm_spb : 0; w[2] = fm_spb;
        return fm_b ? 1 : 0;
    // the same, its offset in two words (docs/c4ix-lfs.md): a disk's file
    // past 2 GB, with C4IX_LFS; without, an offset a word holds
    case SYS_LSEEK64:
        if (!t || !(f = fd_get(t, args[0])) || (f->vn->type != VN_RAMFILE && f->vn->type != VN_PROC)) return -1;
        w = (int *)args[1];
#ifdef C4IX_LFS
        off = ((c4off)w[1] << 32) | ((c4off)w[0] & 0xFFFFFFFFll);
#else
        if (w[1] != (w[0] < 0 ? 0 - 1 : 0)) return -1;
        off = w[0];
#endif
        if (args[2] == 1) off = off + f->pos;
        else if (args[2] == 2) off = off + VN_END(f->vn);
        else if (args[2] != 0) return -1;
        if (off < 0) return -1;
        f->pos = off;
#ifdef C4IX_LFS
        w[0] = (int)(off & 0xFFFFFFFFll); w[1] = (int)(off >> 32);
#else
        w[0] = off; w[1] = 0;
#endif
        return 0;
    case SYS_FTRUNCATE:
        if (!t || !(f = fd_get(t, args[0])) || (f->flags & 3) == C4IX_O_RDONLY) return -1;
        return vfs_truncate(f->vn, args[1]);
    case SYS_LINK:     return vfs_link((char *)args[0], (char *)args[1]);
    // The environment execve gave the caller, NAME=VALUE strings ending
    // in a null pointer, or 0 for none: the kernel's copy, which libc
    // reads its own from
    case SYS_ENVIRON:  return t ? mmu_uva(t->env_vec) : 0;
    case SYS_EXECVE:   return sys_execve((char *)args[0], (char **)args[1], (char **)args[2]);
    case SYS_WAITPID:  return sys_waitpid(args[0], (int *)args[1], args[2]);
    // sched_trap services it: it needs the trap's registers. Outside a
    // trap there are none.
    case SYS_VFORK:    return -1;
    case SYS_UNLINK:   return vfs_unlink((char *)args[0]);
    case SYS_RENAME:   return vfs_rename((char *)args[0], (char *)args[1]);
    case SYS_LAZYFILE: return vfs_lazyfile((char *)args[0], (char *)args[1], args[2]);
    case SYS_RELAZY:   return vfs_relazy((char *)args[0], (char *)args[1], args[2]);
    // The stack the caller's later spawns get, in bytes; 0 for the
    // default. A compiler on a big source wants more than the default
    // and every task paying for that would be most of the memory.
    case SYS_STACK:
        if (!t || args[0] < 0 || args[0] > C4IX_STACK_MAX) return -1;
        t->spawn_stack = args[0];
        return 0;
    case SYS_CLOEXEC:
        if (!t || !fd_get(t, args[0])) return -1;
        t->fdcloexec[args[0]] = args[1] ? 1 : 0;
        return 0;
    // Sleep for args[0] milliseconds. In a trap it is a state change, the
    // same one C4KE's OP_USER_SLEEP makes (c4ke.c): park on the clock and
    // let the scheduler run everyone else, and nap when nobody is ready.
    // It completes on waking, so nothing re-executes.
    case SYS_SLEEP:
        if (!t || args[0] <= 0) return 0;
        t->ev_wait = 0;
        if (!sched_in_trap()) {
            num = __time() + args[0];
            while (__time() - num < 0) sched_yield();
            return 0;
        }
        t->ck_wake = __time() + args[0];
        task_state(t, TS_SLEEPING);
        return 0;
    // Sleep args[0] ms, or less: until an event lands in the GUI ring at
    // args[1] (0: none), or with args[2] set, until a pipe the caller
    // reads has data or reaches end of file. The desktop's loop, which
    // used to wake every 15 ms to look (docs/c4ix-cpu.md, step 11).
    case SYS_EVWAIT:
        if (!t || !sched_in_trap()) return 0;
        t->ev_ring = args[1];
        TF_PUT(t, TF_GUI, args[1]);
        t->ev_wait = (args[1] ? 1 : 0) | (args[2] ? 2 : 0);
        if (sched_events(t)) { t->ev_wait = 0; return 1; }
        t->ck_wake = __time() + (args[0] > 0 ? args[0] : 0);
        task_state(t, TS_SLEEPING);
        return 0;
    case SYS_DISPLAY:  return sys_display(t, args[0], args[1]);
    case SYS_AFFINITY: return sched_affinity(args[0] ? args[0] : (t ? t->id : 0), args[1]);
    case SYS_NICE:     return sched_nice(args[0] ? args[0] : (t ? t->id : 0), args[1]);
    // c4bb's device registers, which a user task does not get unless it
    // asks (docs/c4ix-mmu.md 7). Nothing to do without the MMU.
    case SYS_IOPERM:   return mmu_ioperm(t, args[0]);
    case SYS_MAPFROM:  return mmu_mapfrom(t, args[0], args[1]);
    case SYS_FDGIVE:   return sys_fdgive(t, args[0], args[1], args[2]);
    case SYS_GETPID:   return t ? t->id : -1;
    // Freed with the task if it does not free it itself (task.c)
    case SYS_SBRK:     return task_ualloc(t, args[0]);
    case SYS_EXIT:     task_exit(args[0]); return 0;
    case SYS_SPAWN:
        sched_next_stack_words = (t && t->spawn_stack) ? t->spawn_stack / sizeof(int) : 0;
        s = task_spawn((char *)args[0], args[1], args[2]);
        sched_next_stack_words = 0;
        // not there, out of memory, or it will not load (loader.c)
        if (!s) return loader_err < 0 ? loader_err : SPAWN_BAD;
        // the child gets its own argv: the parent's may be gone, or
        // rewritten, by the time the child reads it
        // (under the MMU task_spawn has, into the child's own memory)
        if (!mmu_on && ck_argv_copy(s, args[1], (char **)args[2])) sched_forge_argv(s, s->argv_vec);
        // Loading the image is a long stay in the kernel, which the slice
        // would count against the parent: a shell that spawns and then
        // waits would reach its wait or not by where the slice happened
        // to end, and a ps its child runs would see it either way. A
        // fresh slice, and it always gets there.
        sched_new_slice();
        return s->id;
    case SYS_WAIT:
        // The target may have been swept by the idle reaper before the
        // parent got round to waiting; its exit code outlives it.
        if (!(s = task_get(args[0]))) {
            if (task_ghost(args[0], &num)) return num;
            return -1;
        }
        if (!sched_in_trap()) return task_wait(s);
        // In trap context blocking is a state change, not a spin: park
        // the caller, and task_zombify completes the wait when the target
        // dies -- it writes the exit code into the saved accumulator,
        // which is this syscall's return value.
        if (s->state == TS_ZOMBIE) {
            num = s->exitcode;
            task_release(s);
            return num;
        }
        t->wait_for = s->id;
        t->wait_posix = 0;
        task_state(t, TS_WAITING);
        return 0;
    }

    kprintf("c4ix: unknown syscall %d from task %d\n", num, t ? t->id : -1);
    return -1;
}

// ---- emulating guarded host opcodes for protected tasks ----

// printf, formatted onto the fd layer. c4m's own PRTF reads the
// argument count from the word after the opcode -- the compiler
// always emits "PRTF; ADJ n" -- so the trapped returnpc points at
// that ADJ and returnpc[1] is n. Arguments sit above sp in the same
// order c4m would have read them.
//
// The conversion set has to be REAL, not kprintf's five. c4m's PRTF
// hands the format string to the host's printf, so a program written
// for c4m expects everything the host supports; the moment such a
// program runs behind protected mode, this function is what it gets
// instead. C4KE's ps alone uses %3ld, %4ld.%03d, %2d, %3d%% and %*s,
// and %*s is the one that proves the point -- an unrecognised spec
// that does not CONSUME its width argument shifts every conversion
// after it, so the output is not merely misaligned, it is wrong.
//
// Supported: flags '-' and '0' (and '+', ' ', '#' accepted and
// ignored), a width or '*', a precision or '.*', the length
// modifiers l/ll/h/z (every integer here is one word), and the
// conversions d i u x X c s p %. %p is glibc's, which is what c4m's
// PRTF prints: %#x of the pointer, or (nil). libc4ix's uformat is the
// same parser for userland -- keep the two in step.
//
// Output is buffered into one write: through the fd layer this may
// be a pipe or a RAM file, and byte-at-a-time would be absurd.

enum { PF_BUF = 512, PF_HIGH = 480 };

static char pf_buf[PF_BUF];
static int  pf_n;
static int  pf_fd;
static int  pf_out;      // characters emitted, printf's return value

static void pf_flush() {
    if (pf_n) { sys_write(pf_fd, pf_buf, pf_n); pf_n = 0; }
}

static void pf_ch(int c) {
    pf_buf[pf_n] = c;
    ++pf_n;
    ++pf_out;
    if (pf_n >= PF_HIGH) pf_flush();
}

static void pf_pad(int c, int n) {
    while (n > 0) { pf_ch(c); --n; }
}

// Digits of V in BASE, most significant first, into BUF; returns the
// length. Base 16 is treated as UNSIGNED: C4 has no unsigned type, so
// the shift's top nibble is masked off rather than sign-extending --
// 28 bits of a word on a 32-bit machine, 60 on a 64-bit one. (%p of a
// pointer above 0x80000000 is the case that needs it.)
static int pf_digits(char *buf, int v, int base) {
    char tmp[72];
    char *ds;
    int n, i, mask;

    ds = "0123456789abcdef";
    mask = ~(15 << (sizeof(int) * 8 - 4));
    n = 0;
    if (!v) { tmp[0] = '0'; n = 1; }
    while (v) {
        if (base == 16) { tmp[n] = ds[v & 15]; v = (v >> 4) & mask; }
        else { tmp[n] = ds[v - (v / base) * base]; v = v / base; }
        ++n;
    }
    i = 0;
    while (n) { --n; buf[i] = tmp[n]; ++i; }
    return i;
}

static int sys_vprintf(int fd, char *fmt, int *argv, int argc) {
    char num[80];
    char *s;
    int c, v, used, width, prec, left, zero, len, neg, pre, i, base, upper;

    pf_fd = fd;
    pf_n = 0;
    pf_out = 0;
    used = 0;

    while (*fmt) {
        c = *fmt; ++fmt;
        if (c != '%') { pf_ch(c); continue; }
        if (!*fmt) { pf_ch('%'); break; }

        // flags
        left = 0; zero = 0;
        while (1) {
            c = *fmt;
            if (c == '-') { left = 1; ++fmt; }
            else if (c == '0') { zero = 1; ++fmt; }
            else if (c == '+' || c == ' ' || c == '#') { ++fmt; }
            else break;
        }

        // width, possibly from an argument -- a negative one means
        // left-justify, which is how ps asks for it (ps.c:312 computes
        // a width of -2)
        width = 0;
        if (*fmt == '*') {
            ++fmt;
            width = (used < argc) ? argv[used] : 0; ++used;
            if (width < 0) { left = 1; width = -width; }
        } else {
            while (*fmt >= '0' && *fmt <= '9') {
                width = width * 10 + (*fmt - '0'); ++fmt;
            }
        }

        // precision: minimum digits for integers, maximum for strings
        prec = -1;
        if (*fmt == '.') {
            ++fmt; prec = 0;
            if (*fmt == '*') {
                ++fmt;
                prec = (used < argc) ? argv[used] : 0; ++used;
                if (prec < 0) prec = -1;
            } else {
                while (*fmt >= '0' && *fmt <= '9') {
                    prec = prec * 10 + (*fmt - '0'); ++fmt;
                }
            }
        }

        // length modifiers: every integer here is one machine word
        while (*fmt == 'l' || *fmt == 'h' || *fmt == 'z') ++fmt;

        c = *fmt;
        if (!c) { pf_ch('%'); break; }
        ++fmt;

        if (c == '%') { pf_pad(' ', left ? 0 : width - 1); pf_ch('%');
                        pf_pad(' ', left ? width - 1 : 0); continue; }

        // Anything unrecognised prints verbatim AND CONSUMES NOTHING,
        // which keeps the remaining arguments aligned.
        if (c != 'd' && c != 'i' && c != 'u' && c != 'x' && c != 'X'
            && c != 'c' && c != 's' && c != 'p') {
            pf_ch('%'); pf_ch(c);
            continue;
        }

        v = (used < argc) ? argv[used] : 0; ++used;

        if (c == 'c') {
            if (!left) pf_pad(' ', width - 1);
            pf_ch(v);
            if (left) pf_pad(' ', width - 1);
            continue;
        }

        // a null %p is a string, and its precision means nothing
        if (c == 's' || (c == 'p' && !v)) {
            s = (c == 's') ? (char *)v : "(nil)";
            if (!s) s = "(null)";
            if (c == 'p') prec = -1;
            len = 0;
            while (s[len] && (prec < 0 || len < prec)) ++len;
            if (!left) pf_pad(' ', width - len);
            i = 0;
            while (i < len) { pf_ch(s[i]); ++i; }
            if (left) pf_pad(' ', width - len);
            continue;
        }

        // integers, and %p as %#x
        upper = (c == 'X');
        base = (c == 'x' || c == 'X' || c == 'p') ? 16 : 10;
        neg = 0;
        if (base == 10 && v < 0) { neg = 1; v = -v; }
        pre = neg ? 1 : (c == 'p') ? 2 : 0;     // the '-' or the 0x
        len = pf_digits(num, v, base);
        if (upper) {
            i = 0;
            while (i < len) {
                if (num[i] >= 'a' && num[i] <= 'f') num[i] = num[i] - 32;
                ++i;
            }
        }
        // an explicit precision is a minimum digit count, and it
        // overrides zero padding (C's rule)
        if (prec >= 0) { zero = 0; while (len < prec) { i = len; while (i) { num[i] = num[i - 1]; --i; } num[0] = '0'; ++len; } }
        i = len + pre;
        if (!left && !zero) pf_pad(' ', width - i);
        if (neg) pf_ch('-');
        if (c == 'p') { pf_ch('0'); pf_ch('x'); }
        if (!left && zero) pf_pad('0', width - i);
        i = 0;
        while (i < len) { pf_ch(num[i]); ++i; }
        if (left) pf_pad(' ', width - len - pre);
    }

    pf_flush();
    return pf_out;
}

// A protected task executed a host syscall opcode. Service it on the
// kernel's terms and leave the result in the trapped accumulator.
// Anything unrecognized kills the task rather than silently letting
// it run on with a bogus result.
void sys_pmviolation(int op, int *sp, int *returnpc, int *a) {
    struct task *t;
    int argc, i;
    int args[8];

    if ((t = sched_current())) ++t->nsyscalls;

    switch (op) {
    case C4IX_OP_PUTC:
        if (!uaccess((int)sp, 1, 0)) { *a = -1; return; }
        *a = sys_write(FD_STDOUT, (char *)sp, 1);
        return;
    case C4IX_OP_PUTS:
        if ((i = ustrlen((char *)sp[0])) < 0) { *a = -1; return; }
        *a = sys_write(FD_STDOUT, (char *)sp[0], i);
        return;
    case C4IX_OP_PRTF:
        // returnpc points at the ADJ that follows PRTF; its operand
        // is the pushed-argument count, format string included.
        argc = returnpc[1];
        if (argc > 8) argc = 8;
        // c4m reads them as t[-1], t[-2], ... from sp + argc
        i = 0;
        while (i < argc - 1) { args[i] = sp[argc - 2 - i]; ++i; }
        if (ustrlen((char *)sp[argc - 1]) < 0) { *a = -1; return; }
        *a = sys_vprintf(FD_STDOUT, (char *)sp[argc - 1], args, argc - 1);
        return;
    case C4IX_OP_OPEN:
        if (ustrlen((char *)sp[1]) < 0) { *a = -1; return; }
        *a = sys_open((char *)sp[1], sp[0]);
        return;
    case C4IX_OP_READ:
        if (!uaccess(sp[1], sp[0], 1)) { *a = -1; return; }
        *a = sys_read(sp[2], (char *)sp[1], sp[0]);
        return;
    case C4IX_OP_CLOS: *a = sys_close(sp[0]); return;
    // Verbatim, not synthesized, but for C4I_TRAPH (host.c task_info).
    // Programs branch on these bits -- C4KE's ps patches its own code
    // differently when C4I_C4 is set -- so the answer is what the machine
    // actually is, as the task sees it.
    case C4IX_OP_INFO: *a = task_info(); return;
    // __c4_usleep(us): SYS_SLEEP's state change, in whole milliseconds
    // rounded up. On the host it would stop every task, not this one.
    case C4IX_OP_USLP:
        *a = 0;
        if (t && sp[0] > 0) {
            t->ck_wake = __time() + (sp[0] + 999) / 1000;
            task_state(t, TS_SLEEPING);
        }
        return;
    // __c4_signal(sig, handler): the task's own handler, as a C4KE
    // program's signal() sets it -- entered the same way, through a trap
    // frame. On the machine it would replace the kernel's.
    case C4IX_OP_SIGH: *a = ck_signal(t, sp[1], sp[0]); return;
    // The same blocks as SYS_SBRK: programs free ualloc'd memory with
    // free(), and what they do not free goes with the task (task.c).
    case C4IX_OP_MALC: *a = task_ualloc(t, sp[0]); return;
    case C4IX_OP_FREE: task_ufree(sp[0]); *a = 0; return;
    // install_trap_handler: every trap of every task would go to this
    // one. Only a kernel does that -- C4KE's c4ke.c4r started as a
    // program, say, which then took the machine and wrote through its
    // own failed allocations. It goes as any forbidden opcode does,
    // saying why.
    case C4IX_OP_ITH:
        kprintf("c4ix: task %d tried to install the machine's trap handler"
            " (a kernel run as a program? boot it with c4m), killing it\n",
            t ? t->id : -1);
        task_exit(-1);
        return;
    case C4IX_OP_EXIT:
        kprintf("c4ix: task %d exited via EXIT opcode, status %d\n",
            t ? t->id : -1, sp[0]);
        task_exit(sp[0]);
        return;
    }

    kprintf("c4ix: task %d used forbidden opcode %d, killing it\n",
        t ? t->id : -1, op);
    task_exit(-1);
}
