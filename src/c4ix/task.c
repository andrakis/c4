//
// C4IX tasks, X0 edition: a malloc'd singly linked task list and
// just enough machinery to create the init task and run it to
// completion on the caller's stack. The scheduler arrives in X1;
// what X0 pins is the shape -- tasks are ordinary structs on a list
// with no fixed capacity, entry points are function addresses called
// through a variable (JSRI/JSRS, which both hosts execute), and
// task_next() already walks the list round-robin, which is the loop
// a context switch slots into.
//

#include "c4ix.h"

static struct task *head;
static struct task *tail;
static int ntasks;
static int nextid;

static void namecpy(char *dst, char *src) {
    int i;
    i = 0;
    while (src[i] && i < TASK_NAME_MAX - 1) { dst[i] = src[i]; ++i; }
    dst[i] = 0;
}

struct task *task_create(char *name, int entry) {
    struct task *t;
    if (!(t = (struct task *)malloc(sizeof(struct task)))) return 0;
    t->id = nextid; ++nextid;
    t->state = TS_READY;
    t->entry = entry;
    t->exitcode = 0;
    t->next = 0;
    namecpy(t->name, name);
    if (tail) tail->next = t; else head = t;
    tail = t;
    ++ntasks;
    return t;
}

struct task *task_get(int id) {
    struct task *t;
    t = head;
    while (t) {
        if (t->id == id) return t;
        t = t->next;
    }
    return 0;
}

struct task *task_first() {
    return head;
}

// Round-robin successor: after the tail comes the head again.
struct task *task_next(struct task *t) {
    if (t && t->next) return t->next;
    return head;
}

int task_count() {
    return ntasks;
}

int task_run(struct task *t) {
    int e;
    if (!t) return -1;
    if (t->state != TS_READY) return -1;
    e = t->entry;
    t->exitcode = e();
    t->state = TS_DONE;
    return t->exitcode;
}

// Free the whole list -- part of a clean shutdown.
void task_shutdown() {
    struct task *t, *n;
    t = head;
    while (t) {
        n = t->next;
        free(t);
        t = n;
    }
    head = 0;
    tail = 0;
    ntasks = 0;
}
