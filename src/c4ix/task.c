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
    if (!(t = task_alloc(name))) return 0;
    t->entry = entry;
    t->state = TS_READY;
    if (!sched_forge(t, entry, argc, argv)) {
        sl4b_free(task_cache, (char *)t);
        return 0;
    }
    task_append(t);
    return t;
}

// Turn the currently running context (boot) into a task: no forged
// stack, nothing saved until the first switch away.
struct task *task_adopt(char *name) {
    struct task *t;
    if (!(t = task_alloc(name))) return 0;
    t->state = TS_RUNNING;
    t->sv = SV_NONE;
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

// Unlink and free everything a task owns. Never called on the
// running task: you cannot free the stack you stand on.
void task_release(struct task *t) {
    task_last_syscalls = t->nsyscalls;
    task_unlink(t);
    if (t->stack) free((int *)t->stack);
    if (t->img_code) free((int *)t->img_code);
    if (t->img_data) free((char *)t->img_data);
    sl4b_free(task_cache, (char *)t);
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
