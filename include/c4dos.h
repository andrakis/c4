// c4dos.h -- the transient-side C4DOS API.
//
// C4DOS loads a program into its own memory and jumps to it, so DOS's
// own routines are directly callable -- the only question is how a
// transient names one. The loader answers that: it scans the image's
// symbol section for __c4dos_api and writes the API table's address
// into it (the loader_systable pattern from src/c4ix/loader.c). A
// program that is NOT running under C4DOS is simply left with 0 there,
// which is how it knows.
//
// Why any of this exists: c4 and c4m have no write syscall, and c4bb's
// disk controller is read-only. A tool that wants to produce a file
// asks DOS to put it on the RAM disk (DEVICE=RAMDISK.SYS), or, since v5,
// on a drive that can be written: "B:NAME" names one, and a plain name
// goes to the home drive when CONFIG.SYS gives one (DEVICE=DRIVES.SYS
// HOME=B:). There is no '>' redirection on this system by decision --
// tools write directly.
//
// The calls go through the invoke stub, which is c4l.c's trick: a
// function rewrites its own first instruction to JMP <target>, so
// calling it lands wherever the slot points. That works on an
// unmodified c4, which matters -- a transient should not need a better
// CPU than DOS itself does just to save a file.
//
// Dialect: strict c4. Include it as a SOURCE FILE, the way u0.h is
// used, because c4cc has no preprocessor:
//   ./c4cc -o tool.c4r include/c4dos.h tool.c

// The opcodes the stub writes. Same numbering as every VM in the
// family (c4.c's enum); literal because plain c4's enum parser takes
// numbers only.
enum { C4DOS_JMP = 2, C4DOS_ENT = 6, C4DOS_ADJ = 7 };
enum { C4DOS_MAXSEARCH = 512 };

// ('C' << 16) + ('4' << 8) + 'D', written out because plain c4 cannot
// fold that in an enum.
enum { C4DOS_MAGIC = 4404292 };

// API table slots. Slots 0-8 are v1; 9-14 are v2 and MUST be gated on
// dos_version() >= 2, because a v1 DOS allocated a 16-word table and
// filled only the first nine -- the rest was uninitialised heap. 15 is
// v3 (the clock) and 16-25 are v4 (below); the table is 32 words.
enum {
  C4DOS_SLOT_MAGIC   = 0,
  C4DOS_SLOT_EXIT    = 1,   // dos_exit(status): return to the prompt
  C4DOS_SLOT_CREATE  = 2,   // create(name) -> handle, or -1 (on a drive: there once closed)
  C4DOS_SLOT_WRITE   = 3,   // write(handle, buf, len) -> len
  C4DOS_SLOT_CLOSE   = 4,   // close(handle)
  C4DOS_SLOT_VERSION = 5,
  C4DOS_SLOT_OPEN    = 6,   // open(name) -> handle, or -1
  C4DOS_SLOT_READ    = 7,   // read(handle, buf, len) -> len
  C4DOS_SLOT_RCLOSE  = 8,   // close(handle)
  C4DOS_SLOT_COUNT   = 9,   // count() -> RAM disk entries
  C4DOS_SLOT_ENTNAME = 10,  // entname(i) -> char *, or 0
  C4DOS_SLOT_ENTSIZE = 11,  // entsize(i) -> bytes, or -1
  C4DOS_SLOT_ENTDATA = 12,  // entdata(i) -> char *, or 0
  C4DOS_SLOT_TRIM    = 13,  // trim() -> bytes of scratch released
  C4DOS_SLOT_RELEASE = 14   // release() -> bytes of RAM disk released
};

// The first version that has the slots above. See dos_can_enum().
enum { C4DOS_API_V2 = 2 };
// v3 adds slot 15, the clock. See dos_can_time().
enum { C4DOS_API_V3 = 3 };
enum { C4DOS_SLOT_TIME = 15 };

int *__c4dos_api;        // patched by the loader; 0 when not under DOS
int *__c4dos_slot;       // the stub's operand cell

// The pc of our caller's ENT, found by walking back from the return
// address. c4l.c's, verbatim in spirit.
int __c4dos_caller (int dummy) {
  int *addr, *next, i;
  addr = (int *)(*(&addr + 2));
  i = 0;
  next = addr;
  while (++i <= C4DOS_MAXSEARCH) {
    --next;
    if (*addr == C4DOS_ENT) {
      if (*next > C4DOS_ADJ) return (int)addr;
    }
    addr = next;
  }
  return 0;
}

int __c4dos_stub () {
  int *self;
  if (!(self = (int *)__c4dos_caller(0))) return 0 - 1;
  *self = C4DOS_JMP;
  __c4dos_slot = self + 1;
  return 0;
}

// Arm the stub, once. The call that does it runs PAST the instruction
// it just rewrote and returns normally; every later call to
// __c4dos_stub lands on the JMP instead. Until this happens
// __c4dos_slot is 0, and writing through it would be a store to
// address 0 -- which is exactly what an unarmed stub does.
int __c4dos_arm () {
  if (!__c4dos_slot) __c4dos_stub();
  return __c4dos_slot != 0;
}

// call0 passes a dummy argument on purpose: c4lc -O turns a bare
// "JSR f; LEV" into a tail call that enters f past its first word, and
// the stub's first word is the JMP it rewrote itself into -- so a
// zero-argument call re-ran the stub's body and rewrote the caller.
// With an argument the call is followed by ADJ and stays a call.
// (src/c4dos/c4dos.c's invoke0 had the same bug, 2026-09-30.)
int __c4dos_call0 (int *code) {
  *__c4dos_slot = (int)code;
  return __c4dos_stub(0);
}
int __c4dos_call1 (int *code, int a) {
  *__c4dos_slot = (int)code;
  return __c4dos_stub(a);
}
int __c4dos_call2 (int *code, int a, int b) {
  *__c4dos_slot = (int)code;
  return __c4dos_stub(a, b);
}
int __c4dos_call3 (int *code, int a, int b, int c) {
  *__c4dos_slot = (int)code;
  return __c4dos_stub(a, b, c);
}

// 1 when a C4DOS with a working API table loaded us.
int dos_present () {
  if (!__c4dos_api) return 0;
  return __c4dos_api[C4DOS_SLOT_MAGIC] == C4DOS_MAGIC;
}

// 1 when that DOS also has somewhere to put a file.
int dos_can_write () {
  if (!dos_present()) return 0;
  return __c4dos_api[C4DOS_SLOT_CREATE] != 0;
}

int dos_create (char *name) {
  if (!dos_can_write()) return 0 - 1;
  if (!__c4dos_arm()) return 0 - 1;
  return __c4dos_call1((int *)__c4dos_api[C4DOS_SLOT_CREATE], (int)name);
}
int dos_write (int h, char *buf, int len) {
  if (!dos_can_write()) return 0;
  if (!__c4dos_arm()) return 0;
  return __c4dos_call3((int *)__c4dos_api[C4DOS_SLOT_WRITE], h, (int)buf, len);
}
int dos_close (int h) {
  if (!dos_can_write()) return 0;
  if (!__c4dos_arm()) return 0;
  return __c4dos_call1((int *)__c4dos_api[C4DOS_SLOT_CLOSE], h);
}

// Reading, the other half. A pipeline is only a pipeline if the next
// stage can see what the last one produced, and a transient's own
// open() goes straight to the host -- it has never heard of the RAM
// disk. These ask DOS instead, and DOS checks RAM before the disk, so
// one call finds a file wherever it actually lives.
int dos_readable () {
  if (!dos_present()) return 0;
  return __c4dos_api[C4DOS_SLOT_OPEN] != 0;
}
int dos_fopen (char *name) {
  if (!dos_readable()) return 0 - 1;
  if (!__c4dos_arm()) return 0 - 1;
  return __c4dos_call1((int *)__c4dos_api[C4DOS_SLOT_OPEN], (int)name);
}
int dos_fread (int h, char *buf, int len) {
  if (!dos_readable()) return 0;
  if (!__c4dos_arm()) return 0;
  return __c4dos_call3((int *)__c4dos_api[C4DOS_SLOT_READ], h, (int)buf, len);
}
int dos_fclose (int h) {
  if (!dos_readable()) return 0;
  if (!__c4dos_arm()) return 0;
  return __c4dos_call1((int *)__c4dos_api[C4DOS_SLOT_RCLOSE], h);
}

// Read a whole file into `buf`, RAM disk or real disk, DOS's rules.
// Returns bytes read, or -1 when there is no DOS to ask.
int dos_slurp (char *name, char *buf, int max) {
  int h, n, total;
  if ((h = dos_fopen(name)) < 0) return 0 - 1;
  total = 0;
  n = 1;
  while (n > 0 && total < max) {
    if ((n = dos_fread(h, buf + total, 4096)) > 0) total = total + n;
  }
  dos_fclose(h);
  return total;
}

// Save a whole buffer under `name`. The shape every tool here wants:
// render the image in memory, then hand it over in one go.
int dos_put (char *name, char *buf, int len) {
  int h;
  if ((h = dos_create(name)) < 0) return 0 - 1;
  dos_write(h, buf, len);
  dos_close(h);
  return len;
}

// ---- v2: enumeration, and handing memory back ----------------------------
//
// The loader's half of the API. A tool asks for a file by name; a
// program taking the machine over -- dosload handing an initrd to a
// kernel -- needs the whole RAM disk without being told what is on it.
//
// Every one of these is gated twice: the version word AND the slot
// itself, because slots 9-12 are only advertised when a RAM disk was
// actually installed by CONFIG.SYS.

int dos_version () {
  if (!dos_present()) return 0;
  return __c4dos_api[C4DOS_SLOT_VERSION];
}

// 1 when this DOS can be asked what it is holding.
int dos_can_enum () {
  if (dos_version() < C4DOS_API_V2) return 0;
  return __c4dos_api[C4DOS_SLOT_COUNT] != 0;
}

// ---- v3: the clock ------------------------------------------------------
//
// TIME is opcode 53, above EXIT, so a program that calls it directly
// leaves the base rung. Asking DOS costs nothing above EXIT (this
// header reaches the table through the invoke STUB, not an indirect
// call), which is what lets c4m keep its place in RUNG_BASE and still
// have a clock -- see src/c4dos/c4dos.c dos_api_time.
//
// Gated on BOTH the version word and the slot, the rule every slot at 9
// and above follows: a v1 DOS allocated sixteen words and filled nine,
// so slot 15 on one of those is uninitialised heap, not a zero.
int dos_can_time () {
  if (!dos_present()) return 0;
  if (dos_version() < C4DOS_API_V3) return 0;
  return __c4dos_api[C4DOS_SLOT_TIME] != 0;
}

// Milliseconds since the machine came up, or 0 when there is no clock
// to ask. 0 is also a legitimate reading for the first millisecond of
// uptime, so callers that need to distinguish "no clock" ask
// dos_can_time() first rather than testing the result.
int dos_time () {
  if (!dos_can_time()) return 0;
  if (!__c4dos_arm()) return 0;
  return __c4dos_call0((int *)__c4dos_api[C4DOS_SLOT_TIME]);
}

int dos_count () {
  if (!dos_can_enum()) return 0;
  if (!__c4dos_arm()) return 0;
  return __c4dos_call0((int *)__c4dos_api[C4DOS_SLOT_COUNT]);
}
char *dos_entname (int i) {
  if (!dos_can_enum()) return 0;
  if (!__c4dos_arm()) return 0;
  return (char *)__c4dos_call1((int *)__c4dos_api[C4DOS_SLOT_ENTNAME], i);
}
int dos_entsize (int i) {
  if (!dos_can_enum()) return 0 - 1;
  if (!__c4dos_arm()) return 0 - 1;
  return __c4dos_call1((int *)__c4dos_api[C4DOS_SLOT_ENTSIZE], i);
}
char *dos_entdata (int i) {
  if (!dos_can_enum()) return 0;
  if (!__c4dos_arm()) return 0;
  return (char *)__c4dos_call1((int *)__c4dos_api[C4DOS_SLOT_ENTDATA], i);
}

// Ask DOS for its 4MB read scratch back. Returns bytes released. DOS
// re-allocates it the next time it reads a file, so this costs a
// malloc later, not correctness.
int dos_trim () {
  if (dos_version() < C4DOS_API_V2) return 0;
  if (!__c4dos_api[C4DOS_SLOT_TRIM]) return 0;
  if (!__c4dos_arm()) return 0;
  return __c4dos_call0((int *)__c4dos_api[C4DOS_SLOT_TRIM]);
}

// Ask DOS to drop the RAM disk contents. Returns bytes released.
// CALL THIS ONLY AFTER COPYING OUT WHAT YOU WANTED: every pointer
// dos_entdata() returned is dead the moment this returns.
int dos_release () {
  if (dos_version() < C4DOS_API_V2) return 0;
  if (!__c4dos_api[C4DOS_SLOT_RELEASE]) return 0;
  if (!__c4dos_arm()) return 0;
  return __c4dos_call0((int *)__c4dos_api[C4DOS_SLOT_RELEASE]);
}

// ---- slot 1: exit ---------------------------------------------------------
//
// Back to whoever ran us, from any depth, with a status: DOS's prompt,
// or the program that exec'd or started us, or the caller of a catch.
// It does not return. Under no DOS it returns -1, and the caller had
// better return from main itself.
int dos_exit (int status) {
  if (!dos_present()) return 0 - 1;
  if (!__c4dos_api[C4DOS_SLOT_EXIT]) return 0 - 1;
  if (!__c4dos_arm()) return 0 - 1;
  return __c4dos_call1((int *)__c4dos_api[C4DOS_SLOT_EXIT], status);
}

// ---- v4: the console, running programs, deleting files ------------------
//
// What a shell needs to be one without a loader of its own (grin,
// docs/grin-plan.md 3.2), and what lets DOS decide where a program's
// console goes. Gated, like everything from slot 9 up, on the version
// word AND the slot; 18 is there only with a RAM disk.
enum { C4DOS_API_V4 = 4 };
enum {
  C4DOS_SLOT_CONWRITE = 16, // conwrite(buf, n) -> n: console output
  C4DOS_SLOT_CONREAD  = 17, // conread(buf, n) -> bytes, at most a line; 0 at end
  C4DOS_SLOT_UNLINK   = 18, // unlink(name) -> 0, or -1: the RAM disk's, or a drive's (v5)
  C4DOS_SLOT_EXEC     = 19, // exec(cmdline) -> its status, as the prompt runs it
  C4DOS_SLOT_LOAD     = 20, // load(path) -> handle, constructors run; 0 on failure
  C4DOS_SLOT_START    = 21, // start(h, argc, argv) -> main's status; h stays loaded
  C4DOS_SLOT_UNLOAD   = 22, // unload(h) -> 0: destructors, image and heap freed; -1
  C4DOS_SLOT_CATCH    = 23, // catch(fn, arg, &exited) -> fn(arg), or exit()'s status
  C4DOS_SLOT_SINK     = 24, // sink(write, read, ctx): console to f(ctx, buf, n); 0 = DOS's
  C4DOS_SLOT_SHELL    = 25, // data, not code: the shell's table (grin's), or 0
  C4DOS_SLOT_GEN      = 26, // gen(h) -> h's heap generation (0: the running image's); 0 without HEAP.SYS
  C4DOS_SLOT_SETGEN   = 27  // setgen(g) -> the previous generation; -1 without HEAP.SYS
};

int dos_v4 (int slot) {
  if (dos_version() < C4DOS_API_V4) return 0;
  return __c4dos_api[slot] != 0;
}

int dos_conwrite (char *buf, int n) {
  if (!dos_v4(C4DOS_SLOT_CONWRITE) || !__c4dos_arm()) return 0 - 1;
  return __c4dos_call2((int *)__c4dos_api[C4DOS_SLOT_CONWRITE], (int)buf, n);
}
int dos_conread (char *buf, int n) {
  if (!dos_v4(C4DOS_SLOT_CONREAD) || !__c4dos_arm()) return 0 - 1;
  return __c4dos_call2((int *)__c4dos_api[C4DOS_SLOT_CONREAD], (int)buf, n);
}
int dos_unlink (char *name) {
  if (!dos_v4(C4DOS_SLOT_UNLINK) || !__c4dos_arm()) return 0 - 1;
  return __c4dos_call1((int *)__c4dos_api[C4DOS_SLOT_UNLINK], (int)name);
}
int dos_exec (char *cmdline) {
  if (!dos_v4(C4DOS_SLOT_EXEC) || !__c4dos_arm()) return 0 - 1;
  return __c4dos_call1((int *)__c4dos_api[C4DOS_SLOT_EXEC], (int)cmdline);
}
int dos_load (char *path) {
  if (!dos_v4(C4DOS_SLOT_LOAD) || !__c4dos_arm()) return 0;
  return __c4dos_call1((int *)__c4dos_api[C4DOS_SLOT_LOAD], (int)path);
}
int dos_start (int h, int argc, char **argv) {
  if (!dos_v4(C4DOS_SLOT_START) || !__c4dos_arm()) return 0 - 1;
  return __c4dos_call3((int *)__c4dos_api[C4DOS_SLOT_START], h, argc, (int)argv);
}
int dos_unload (int h) {
  if (!dos_v4(C4DOS_SLOT_UNLOAD) || !__c4dos_arm()) return 0 - 1;
  return __c4dos_call1((int *)__c4dos_api[C4DOS_SLOT_UNLOAD], h);
}
// fn is a function's address (&f); exited may be 0
int dos_catch (int *fn, int arg, int *exited) {
  if (!dos_v4(C4DOS_SLOT_CATCH) || !__c4dos_arm()) return 0 - 1;
  return __c4dos_call3((int *)__c4dos_api[C4DOS_SLOT_CATCH], (int)fn, arg, (int)exited);
}
int dos_sink (int *w, int *r, int ctx) {
  if (!dos_v4(C4DOS_SLOT_SINK) || !__c4dos_arm()) return 0 - 1;
  return __c4dos_call3((int *)__c4dos_api[C4DOS_SLOT_SINK], (int)w, (int)r, ctx);
}
int *dos_shell () {
  if (dos_version() < C4DOS_API_V4) return 0;
  return (int *)__c4dos_api[C4DOS_SLOT_SHELL];
}
// Heap generations (DEVICE=HEAP.SYS), for a shell that runs code of
// several images and must say whose memory a call allocates.
int dos_gen (int h) {
  if (!dos_v4(C4DOS_SLOT_GEN) || !__c4dos_arm()) return 0;
  return __c4dos_call1((int *)__c4dos_api[C4DOS_SLOT_GEN], h);
}
int dos_setgen (int g) {
  if (!dos_v4(C4DOS_SLOT_SETGEN) || !__c4dos_arm()) return 0 - 1;
  return __c4dos_call1((int *)__c4dos_api[C4DOS_SLOT_SETGEN], g);
}

// ---- v5: drives that can be written ---------------------------------------
//
// create, write and close reach a writable drive (DEVICE=DRIVES.SYS):
// "B:NAME" there, and a plain name on the home drive (HOME=B:) when
// there is one. unlink takes a file off either. DOS keeps each drive's
// c4dos.dir, which DIR B: types. These two say where things are.
enum { C4DOS_API_V5 = 5 };
enum {
  C4DOS_SLOT_HOME   = 28,   // home() -> the home drive, 0 = A:; -1 for none
  C4DOS_SLOT_DRIVES = 29    // drives() -> how many drives (1 without DRIVES.SYS)
};
int dos_home () {
  if (dos_version() < C4DOS_API_V5 || !__c4dos_api[C4DOS_SLOT_HOME] || !__c4dos_arm()) return 0 - 1;
  return __c4dos_call1((int *)__c4dos_api[C4DOS_SLOT_HOME], 0);
}
int dos_drives () {
  if (dos_version() < C4DOS_API_V5 || !__c4dos_api[C4DOS_SLOT_DRIVES] || !__c4dos_arm()) return 1;
  return __c4dos_call1((int *)__c4dos_api[C4DOS_SLOT_DRIVES], 0);
}
