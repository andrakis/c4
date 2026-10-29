//
// C4IX tasks: a singly linked task list with no fixed capacity.
// Task structs come from an SL4B cache (the user's requirement:
// no malloc-per-task); 64KB stacks and loaded images stay plain
// malloc -- variable-size things are not slab material.
//
// task_head is shared extern data: sched.c walks the list directly
// through the L8 extern-data link support. task_next() wraps from
// the tail back to the head -- the round-robin walk the scheduler
// context-switches along.
//

#include "c4ix.h"

struct task *task_head;
int task_last_syscalls;      // syscalls made by the most recently reaped task
static struct task *task_tail;
static struct sl4b_cache *task_cache;
static int ntasks;
static int nextid;

static void namecpy(char *dst, char *src) {
    int i;
    i = 0;
    while (src[i] && i < TASK_NAME_MAX - 1) { dst[i] = src[i]; ++i; }
    dst[i] = 0;
}

static struct task *task_alloc(char *name) {
    struct task *t;
    if (!task_cache)
        if (!(task_cache = sl4b_cache_create("task", sizeof(struct task), 16)))
            return 0;
    if (!(t = (struct task *)sl4b_alloc(task_cache))) return 0;
    t->id = nextid;
    namecpy(t->name, name);
    return t;
}

static void task_append(struct task *t) {
    sched_lock();
    ++nextid;
    ++sched_nlive;
    if (task_tail) task_tail->next = t; else task_head = t;
    task_tail = t;
    ++ntasks;
    sched_unlock();
}

struct task *task_create(char *name, int entry, int argc, int argv) {
    struct task *t;
    struct task *parent;
    if (!(t = task_alloc(name))) return 0;
    t->entry = entry;
    t->state = TS_READY;
    if (!sched_forge(t, entry, argc, argv)) {
        sl4b_free(task_cache, (char *)t);
        return 0;
    }
    // Inherit the creator's descriptors, sharing the open file
    // descriptions -- a redirected fd 1 stays redirected in the
    // child, which is redirection without fork.
    sched_lock();
    if ((parent = sched_current())) {
        fd_clone(t, parent);
        t->cwd = parent->cwd;      // children start where the parent is
        t->parent = parent->id;
        // and with its environment, as execve last gave it
        if (parent->env_vec) task_strvec((char **)parent->env_vec, -1, &t->env_vec, &t->env_data);
    }
    else fd_init_console(t);
    task_append(t);
    sched_unlock();
    return t;
}

// The vfork child (sched_vfork). Its stack is its own: the parent's frames
// below the one that called vfork must survive the child's calls, which
// is the one thing a stack machine's vfork cannot share (sched.c).
struct task *task_vchild(struct task *p) {
    struct task *t;
    if (!(t = task_alloc(p->name))) return 0;
    t->state = TS_READY;
    if (!sched_forge(t, 0, 0, 0)) {
        sl4b_free(task_cache, (char *)t);
        return 0;
    }
    sched_lock();
    fd_clone_all(t, p);
    t->cwd = p->cwd;
    t->parent = p->id;
    t->privs = p->privs;
    t->cpumask = p->cpumask;
    t->nice = p->nice;
    t->spawn_stack = p->spawn_stack;
    t->img_libs = p->img_libs;
    t->img_nlibs = p->img_nlibs;
    t->vfparent = p;
    if (p->env_vec) task_strvec((char **)p->env_vec, -1, &t->env_vec, &t->env_data);
    task_append(t);
    sched_unlock();
    return t;
}

// Its execve or its exit: the parent's memory is the parent's again,
// and the parent goes on from its vfork, which returns the child's id
// (already in its saved accumulator).
void task_unborrow(struct task *t) {
    struct task *p;
    if (!(p = t->vfparent)) return;
    t->vfparent = 0;
    t->img_libs = 0;
    t->img_nlibs = 0;
    if (p->state == TS_VFORK && p->wait_for == t->id) p->state = TS_READY;
}

int task_strvec(char **v, int n, int *pvec, int *pdata) {
    char **vec;
    char *blob, *p;
    int i, k, bytes;

    *pvec = 0;
    *pdata = 0;
    if (!v) return 0;
    if (n < 0) { n = 0; while (v[n]) ++n; }
    bytes = 1;
    i = 0;
    while (i < n) {
        if (v[i]) { k = 0; while (v[i][k]) ++k; bytes = bytes + k + 1; }
        else bytes = bytes + 1;
        ++i;
    }
    if (!(vec = (char **)malloc((n + 1) * sizeof(int)))) return -1;
    if (!(blob = (char *)malloc(bytes))) { free((int *)vec); return -1; }
    p = blob;
    i = 0;
    while (i < n) {
        vec[i] = p;
        if (v[i]) { k = 0; while (v[i][k]) { *p = v[i][k]; ++p; ++k; } }
        *p = 0; ++p;
        ++i;
    }
    vec[n] = 0;
    *pvec = (int)vec;
    *pdata = (int)blob;
    return n;
}

// Turn the currently running context (boot) into a task: no forged
// stack, nothing saved until the first switch away.
struct task *task_adopt(char *name) {
    struct task *t;
    if (!(t = task_alloc(name))) return 0;
    t->state = TS_RUNNING;
    t->sv = SV_NONE;
    fd_init_console(t);
    task_append(t);
    return t;
}

void task_unlink(struct task *t) {
    struct task *p;
    sched_lock();
    if (task_head == t) task_head = t->next;
    else {
        p = task_head;
        while (p) {
            if (p->next == t) { p->next = t->next; p = 0; }
            else p = p->next;
        }
    }
    if (task_tail == t) {
        task_tail = task_head;
        while (task_tail && task_tail->next) task_tail = task_tail->next;
    }
    --ntasks;
    sched_unlock();
}

struct task *task_reaplist;   // released, waiting to be freed (sched_trap checks it)

// ---- reaping, and remembering what was reaped ----
//
// A zombie used to be released only by whoever waited on it, or by
// the shutdown drain. Nothing else reaped, so a parent that never
// waits leaves its children on the list for the whole run -- and one
// exists: C4KE's innerbench spawns several benches, can only wait on
// one (C4KE has no group wait), and polls the rest instead. Under
// C4IX they simply accumulated.
//
// So the idle loop sweeps them, which is what C4KE's idle task does
// and what task_release's own comment already pointed at. The race
// that makes it delicate is a parent that waits AFTER its child has
// been swept: task_get returns nothing and the wait yields -1 instead
// of the exit code. This table closes it -- the last few exit statuses
// outlive their tasks, so a late wait still gets the right answer.
//
// A ghost the sweep made also keeps its parent, for a waitpid(-1) that
// comes later; one collected by a wait does not (-1), so it is not
// handed out twice.
enum { TASK_GHOSTS = 32 };
static int ghost_id[TASK_GHOSTS];
static int ghost_code[TASK_GHOSTS];
static int ghost_parent[TASK_GHOSTS];
static int ghost_sig[TASK_GHOSTS];
static int ghost_next;

static void task_remember(struct task *t) {
    ghost_id[ghost_next] = t->id;
    ghost_code[ghost_next] = t->exitcode;
    ghost_parent[ghost_next] = -1;
    ghost_sig[ghost_next] = t->killsig;
    ghost_next = (ghost_next + 1) % TASK_GHOSTS;
}

int task_ghost_child(int parent, int pid, int *pcode, int *psig) {
    int i;
    i = 0;
    while (i < TASK_GHOSTS) {
        if (ghost_parent[i] == parent && ghost_id[i] && (pid == -1 || ghost_id[i] == pid)) {
            ghost_parent[i] = -1;
            *pcode = ghost_code[i];
            *psig = ghost_sig[i];
            return ghost_id[i];
        }
        ++i;
    }
    return 0;
}

// 1 if ID was a task we reaped, with its exit code out through PCODE.
int task_ghost(int id, int *pcode) {
    int i;
    i = 0;
    while (i < TASK_GHOSTS) {
        if (ghost_id[i] == id) { *pcode = ghost_code[i]; return 1; }
        ++i;
    }
    return 0;
}

// ---- a task's own memory ----
//
// SYS_SBRK (libc4ix's ualloc) and malloc behind protected mode (sys.c's
// C4IX_OP_MALC) are kernel malloc. They used to be nothing more, so what
// a program did not free outlived it: every run of crc32 or cc left its
// tables behind, and a b4ke build inside C4IX climbed towards the top of
// the arena. Each block now carries a header that puts it on its task's
// list, and task_destroy frees the list.
//
//   h[0] next block's header        h[2] bytes asked for
//   h[1] address of the word that   h[3] UBLOCK_MAGIC, so a free can
//        points here (&t->ublocks        tell its blocks from anything
//        or the previous h[0])           else (0 once freed)
//
// The back link is to a WORD, not a block, so a block unlinks without
// knowing whose it is. Four words keep the caller's pointer as aligned as
// malloc's.
//
// Only a protected task's blocks are listed. An unprotected one executes
// FREE on the host itself, which would be handed h + 4 rather than h.
//
// A program cannot pass its memory on: a child's argv is the kernel's
// copy (ck_argv_copy), and pipes, files and the console copy what is
// written. The one thing that keeps a pointer is libjs's display, which
// writes events into a ring the task attached and should have detached
// on its way out -- task_ufree_all detaches it instead of letting the
// host write into freed memory.
//
// The lists have a lock of their own (docs/c4ix-smp.md, finer locks): a
// program's malloc and free come here from sched_trap's light path,
// without the kernel lock, on whichever CPU runs it. Everything that
// walks or changes a list holds umem_lock while it does.
enum { UBLOCK_WORDS = 4, UBLOCK_MAGIC = 0x55424c4b };
static int umem_lock[2];

// What a task holds in blocks, headers included (sched.c's task_mem)
int task_umem(struct task *t) {
    int b, *u;
    b = 0;
    klock_take(umem_lock);
    u = (int *)t->ublocks;
    while (u) { b = b + u[2] + UBLOCK_WORDS * sizeof(int); u = (int *)u[0]; }
    klock_drop(umem_lock);
    return b;
}

// A vfork child allocates for its parent: the heap is part of the memory
// it borrows, and libc's allocator, in the shared library data, carves
// up what it gets for both of them.
int task_ualloc(struct task *t, int bytes) {
    int *h;
    if (!t || t->privs != PRIV_USER) return (int)malloc(bytes);
    while (t->vfparent) t = t->vfparent;
    if (bytes < 0 || !(h = (int *)malloc(bytes + UBLOCK_WORDS * sizeof(int)))) return 0;
    sched_lock();
    klock_take(umem_lock);
    h[0] = t->ublocks;
    h[1] = (int)&t->ublocks;
    h[2] = bytes;
    h[3] = UBLOCK_MAGIC;
    if (h[0]) ((int *)h[0])[1] = (int)h;
    t->ublocks = (int)h;
    klock_drop(umem_lock);
    sched_unlock();
    return (int)(h + UBLOCK_WORDS);
}

void task_ufree(int p) {
    int *h;
    if (!p) return;
    h = (int *)p - UBLOCK_WORDS;
    if (h[3] != UBLOCK_MAGIC) { free((int *)p); return; }
    sched_lock();
    klock_take(umem_lock);
    // looked at again under the lock: another CPU may have freed it
    if (h[3] != UBLOCK_MAGIC) { klock_drop(umem_lock); sched_unlock(); return; }
    *(int *)h[1] = h[0];
    if (h[0]) ((int *)h[0])[1] = h[1];
    h[3] = 0;
    klock_drop(umem_lock);
    sched_unlock();
    free(h);
}

static void task_ufree_all(struct task *t) {
    int *h, *n;
    int ring;
    ring = host_has(C4IX_I_GUI) ? *(int *)C4IX_GUI_RING : 0;
    klock_take(umem_lock);
    h = (int *)t->ublocks;
    t->ublocks = 0;
    while (h) {
        n = (int *)h[0];
        if (ring && ring >= (int)(h + UBLOCK_WORDS) && ring < (int)(h + UBLOCK_WORDS) + h[2]) {
            *(int *)C4IX_GUI_RING = 0;
            ring = 0;
        }
        h[3] = 0;
        free(h);
        h = n;
    }
    klock_drop(umem_lock);
}

// Free what a task owns. Only ever called for a task nobody is
// standing on -- see task_release.
static void task_destroy(struct task *t) {
    ck_task_free(t);
    task_drop_image(t);
    if (t->stack) kmem_free((char *)t->stack);   // an idle task's is the kernel arena's
    if (t->env_vec) free((int *)t->env_vec);
    if (t->env_data) free((char *)t->env_data);
    sl4b_free(task_cache, (char *)t);
}

// Its image, its libraries' code and data and its heap: what a task
// gives up to execve, and with everything else when it is reaped. A
// vfork child that has neither exec'd nor exited is borrowing all of it
// and owns none of it.
void task_drop_image(struct task *t) {
    int i;
    task_unborrow(t);
    task_ufree_all(t);
    if (t->img_code) free((int *)t->img_code);
    if (t->img_data) free((char *)t->img_data);
    if (t->img_cons) free((int *)t->img_cons);
    if (t->img_des) free((int *)t->img_des);
    if (t->img_libs) {
        i = 0;
        while (i < t->img_nlibs) {
            // code is 0 for a shared (-fpic) library: the loader's cache keeps it
            if (((int *)t->img_libs)[3 * i]) free((int *)((int *)t->img_libs)[3 * i]);
            free((char *)((int *)t->img_libs)[3 * i + 1]);
            ++i;
        }
        free((int *)t->img_libs);
    }
    t->img_code = 0; t->img_data = 0;
    t->img_cons = 0; t->img_ncons = 0;
    t->img_des = 0; t->img_ndes = 0;
    t->img_libs = 0; t->img_nlibs = 0;
    t->img_bytes = 0;
}

// Detach a task and free it -- unless it is the one currently
// running, which happens on the ordinary exit path: a task calls
// exit(), the trap handler services it, and the handler's own frame
// is ON THAT TASK'S STACK. Freeing it there means executing the rest
// of the handler, and the context switch at its end, on returned
// memory. So the corpse goes on a list and task_reap frees it once
// the kernel is standing somewhere else. (C4KE learned the same
// lesson: its idle task reaps zombies for exactly this reason.)
void task_release(struct task *t) {
    sched_lock();
    task_last_syscalls = t->nsyscalls;
    task_remember(t);
    fd_closeall(t);
    task_unlink(t);
    sched_retire(t);
    if (task_busy(t)) {
        t->next = task_reaplist;
        task_reaplist = t;
        sched_unlock();
        return;
    }
    task_destroy(t);
    sched_unlock();
}

// Called from safe points -- the top of a trap, a cooperative
// switch, the idle loop -- where the kernel is no longer running on
// a dead task's stack.
void task_reap() {
    struct task *t, *keep;
    sched_lock();
    keep = 0;
    while (task_reaplist) {
        t = task_reaplist;
        task_reaplist = t->next;
        if (task_busy(t)) { t->next = keep; keep = t; }
        else task_destroy(t);
    }
    task_reaplist = keep;
    sched_unlock();
}

// Release zombies nobody is waiting on. Called from the idle loop,
// where the kernel is not standing on any task's stack. No zombie has a
// waiter any more: task_zombify hands a dying task straight to the one
// waiting on it, and a wait that comes later reaps the zombie at once.
//
// Only children of USER tasks are swept, and that restriction is not
// caution -- it is the difference between working and a double free.
// Kernel code waits by POINTER (task_wait takes a struct task *), and
// holds those pointers across waits: init creates ping and pong, then
// waits on each in turn, so sweeping pong while it waits on ping
// hands the second task_wait a dangling pointer into freed slab
// memory. Userland only ever names a task by id, and an id that has
// been reaped is answered from the ghost table above. So: if the
// parent is userland, nobody is holding a pointer, and it is safe.
//
// A parent that is GONE holds nothing either -- and a zombie whose parent
// exited first (c4ix-tar, which vfsload starts and does not wait for) was
// otherwise never collected. Kernel code only holds its own children, and
// a kernel parent is still there while it holds them.
//
// A parent that waits the POSIX way (vfork, waitpid) is left its zombies:
// its waitpid(-1) has to find them, as a Unix parent's does, and one that
// never waits leaves them until it exits, as there too. A zombie swept
// before its parent first asked leaves a ghost with its parent's id.
static int task_parent_is_user(struct task *t) {
    struct task *p;
    if (!(p = task_get(t->parent))) return 1;
    return p->privs == PRIV_USER && !p->posix;
}

int task_reap_orphans() {
    struct task *t, *w;
    int freed, parent;

    freed = 0;
    sched_lock();
    t = task_head;
    while (t) {
        if (t->state == TS_ZOMBIE && !task_busy(t)
            && task_parent_is_user(t)) {
            w = t->next;
            parent = t->parent;
            task_release(t);
            ghost_parent[(ghost_next + TASK_GHOSTS - 1) % TASK_GHOSTS] = parent;
            ++freed;
            t = w;
            continue;
        }
        t = t->next;
    }
    sched_unlock();
    return freed;
}

struct task *task_get(int id) {
    struct task *t;
    t = task_head;
    while (t) {
        if (t->id == id) return t;
        t = t->next;
    }
    return 0;
}

struct task *task_first() {
    return task_head;
}

// Round-robin successor: after the tail comes the head again.
struct task *task_next(struct task *t) {
    if (t && t->next) return t->next;
    return task_head;
}

int task_count() {
    return ntasks;
}

// Free the whole list -- the tail end of a clean shutdown, after
// sched_run() has drained every other task.
void task_shutdown() {
    while (task_head) task_release(task_head);
}
