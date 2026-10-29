//
// C4IX: the MMU (docs/c4ix-mmu.md).
//
// libjs fits an MMU when its host asks for one (cli.js --mmu, the page's
// ?mmu=1), and announces it as C4I_MMU. It translates user mode only:
// the kernel runs untranslated, on physical addresses, as it always has.
// This module owns the page tables, two levels of 1,024 words in the
// i386's shape, built from pages of the heap; turns the MMU on, on
// every CPU, with a stack of its own for the traps that come from user
// mode; and says what a page fault was.
//
// Stage 1 of docs/c4ix-mmu.md: one address space, in which every page
// above page 0 is a user task's to use, as it was without the MMU.
//

#include "c4ix.h"

int mmu_on;                    // user mode is translated
static int *mmu_dir;           // the page directory every user task runs in
static int mmu_memsize;        // the arena, bytes
static int mmu_ntables;        // page tables made
int mmu_kstack[CPU_MAX];       // each CPU's trap stack (its base)

// ---- pages for the tables ----
//
// A table is one page, page-aligned. The heap's blocks are 8-byte
// aligned, so pages come from blocks of 16 pages at a time, of which
// the aligned 15 or 16 inside are used; freed ones go on a list threaded
// through their first word.
enum { MMU_CHUNK = 16 };
static int mmu_freepage;
static int mmu_chunkp, mmu_chunkn;

static int mmu_page() {
    int p, raw;
    if (mmu_freepage) {
        p = mmu_freepage;
        mmu_freepage = *(int *)p;
    } else {
        if (!mmu_chunkn) {
            if (!(raw = (int)malloc(MMU_CHUNK * MMU_PAGE))) return 0;
            mmu_chunkp = (raw + MMU_PAGE - 1) & ~(MMU_PAGE - 1);
            mmu_chunkn = (raw + MMU_CHUNK * MMU_PAGE - mmu_chunkp) / MMU_PAGE;
        }
        p = mmu_chunkp;
        mmu_chunkp = mmu_chunkp + MMU_PAGE;
        --mmu_chunkn;
    }
    memset((char *)p, 0, MMU_PAGE);
    return p;
}

// VA's entry in directory DIR, its table made if MAKE; 0 if there is none
static int *mmu_pte(int *dir, int va, int make) {
    int *pt;
    int pde;
    pde = dir[(va >> 22) & 1023];
    if (!(pde & PTE_P)) {
        if (!make || !(pt = (int *)mmu_page())) return 0;
        ++mmu_ntables;
        dir[(va >> 22) & 1023] = (int)pt | PTE_P;
    } else pt = (int *)(pde & ~(MMU_PAGE - 1));
    return pt + ((va >> 12) & 1023);
}

// Map N bytes at VA, page by page, to the same addresses, with FLAGS
// (0 unmaps). This CPU forgets what it had of them.
int mmu_map(int *dir, int va, int n, int flags) {
    int *e;
    int end;
    end = va + n;
    va = va & ~(MMU_PAGE - 1);
    while (va < end) {
        if (!(e = mmu_pte(dir, va, flags != 0))) { if (flags) return 0; }
        else {
            *e = flags ? (va | flags) : 0;
            *(int *)MMU_INVLPG = va;
        }
        va = va + MMU_PAGE;
    }
    return 1;
}

// This CPU: a stack for traps from user mode, the directory, and on.
// CPU 0 from mmu_init; the others as they start (sched.c smp_ap).
void mmu_cpu(int k) {
    int s;
    if (!mmu_on) return;
    if (!(s = (int)malloc(MMU_KSTACK))) {
        kputs("c4ix: panic: no memory for a trap stack\n");
        exit(-1);
    }
    mmu_kstack[k] = s;
    *(int *)MMU_KSP = s + MMU_KSTACK;
    *(int *)MMU_PTBR = (int)mmu_dir;
    *(int *)MMU_CTL = 1;
    klog("mmu: CPU %d translating user mode, its traps on a %d KB stack at 0x%08x\n",
        k, MMU_KSTACK / 1024, s);
}

// At boot, before anything runs in user mode
void mmu_init() {
    if (!host_has(C4IX_I_MMU)) return;
    mmu_memsize = *(int *)MMU_MEMSIZE;
    if (!(mmu_dir = (int *)mmu_page()) ||
        !mmu_map(mmu_dir, MMU_PAGE, mmu_memsize - MMU_PAGE, PTE_P | PTE_W | PTE_U | PTE_X)) {
        klog("mmu: no memory for the page tables; left off\n");
        return;
    }
    mmu_on = 1;
    klog("mmu: 4 KB pages, one space for every task: all %d MB above page 0, in %d page tables\n",
        mmu_memsize / 1048576, mmu_ntables);
    mmu_cpu(0);
}

// ---- faults ----

// A user task took a page fault (TRAP_SEGV): say what it was, and kill it
void mmu_fault(struct task *t, int va) {
    int info, pc;
    char *what;
    info = *(int *)MMU_FINFO;
    pc = *(int *)MMU_FPC;
    what = (info & MMU_FK_WRITE) ? "write to" : (info & MMU_FK_FETCH) ? "jump to" : "read of";
    kprintf("c4ix: task %d (%s): page fault: %s 0x%08x (%s) at pc 0x%08x; killed\n",
        t ? t->id : -1, t ? t->name : "?", what, va,
        (info & MMU_FK_PRESENT) ? "not allowed" : "not mapped", pc);
    if (t) t->killsig = C4IX_SIGSEGV;
    task_exit(-1);
}
