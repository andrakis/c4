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
    }
    else fd_init_console(t);
    task_append(t);
    sched_unlock();
    return t;
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

static struct task *task_reaplist;   // released, waiting to be freed

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
enum { TASK_GHOSTS = 32 };
static int ghost_id[TASK_GHOSTS];
static int ghost_code[TASK_GHOSTS];
static int ghost_next;

static void task_remember(struct task *t) {
    ghost_id[ghost_next] = t->id;
    ghost_code[ghost_next] = t->exitcode;
    ghost_next = (ghost_next + 1) % TASK_GHOSTS;
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

// Free what a task owns. Only ever called for a task nobody is
// standing on -- see task_release.
static void task_destroy(struct task *t) {
    ck_task_free(t);
    if (t->stack) free((int *)t->stack);
    if (t->img_code) free((int *)t->img_code);
    if (t->img_data) free((char *)t->img_data);
    if (t->img_cons) free((int *)t->img_cons);
    if (t->img_des) free((int *)t->img_des);
    sl4b_free(task_cache, (char *)t);
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
    if (t == sched_current()) {
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
        if (t == sched_current()) { t->next = keep; keep = t; }
        else task_destroy(t);
    }
    task_reaplist = keep;
    sched_unlock();
}

// Release zombies nobody is waiting on. Called from the idle loop,
// where the kernel is not standing on any task's stack. A task with a
// waiter is left alone: that waiter is entitled to reap it and read
// its exit code the ordinary way.
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
static int task_parent_is_user(struct task *t) {
    struct task *p;
    if (!(p = task_get(t->parent))) return 0;
    return p->privs == PRIV_USER;
}

int task_reap_orphans() {
    struct task *t, *w;
    int freed, waited;

    freed = 0;
    sched_lock();
    t = task_head;
    while (t) {
        if (t->state == TS_ZOMBIE && t != sched_current()
            && task_parent_is_user(t)) {
            waited = 0;
            w = task_head;
            while (w) {
                if (w->state == TS_WAITING && w->wait_for == t->id) { waited = 1; w = 0; }
                else w = w->next;
            }
            if (!waited) {
                w = t->next;
                task_release(t);
                ++freed;
                t = w;
                continue;
            }
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
