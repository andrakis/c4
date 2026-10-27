// C4DOS - a single-tasking, trap-free disk operating system for the
// C4 family, in the classic DOS shape: CONFIG.SYS, AUTOEXEC.BAT, a
// prompt, transient programs that load, run, and return.
//
//   node src/c4bb/sim/cli.js -d <disk> c4dos.c4r     (the real machine)
//   ./c4m load-c4r.c -- c4dos.c4r                    (native, for tests)
//   ./c4 c4l.c c4dos.c4r                             (clockless build only)
//
// Design: docs/c4dos-design.md. Everything here needs only plain-c4
// opcodes plus the basic c4m syscall set (OPEN READ CLOS PRTF MALC
// FREE MSET MCMP EXIT) - ordinary microcode on c4bb, no traps, no
// jsops. The ONE deliberate extension is the clock (TIME opcode),
// which C4 never had: it is compiled in only with -DC4DOS_CLOCK=1
// (preprocessed by our own cpp - the toolchain eating its own food)
// and gated at runtime behind CONFIG.SYS's DEVICE=CLOCK.SYS, so the
// clockless image stays runnable under `./c4 c4l.c`.
//
// Mechanisms, with provenance (copy, never link):
//   - the invoke stub (caller_address/stub/invokeN) is c4l.c's,
//     verbatim in spirit: function pointers on an unmodified VM.
//   - the loader is src/c4ix/loader.c's parser (v3 MEMSZ-aware),
//     de-structed into parallel globals for the plain-c4 dialect.
//   - exit() for transients is the double-LEV trampoline proven in
//     src/tests/test_coop_switch.c: frame_enter() saves its own frame
//     address; dos_exit() points its frame pair at it and returns
//     through the trampoline tail, landing exactly as if frame_enter
//     had returned. Frames nest (API v4 exec, start, catch), and the
//     status rides back in a global, not A. No sp/bp registers were
//     harmed.
//   - transients find dos_exit via the __c4dos_api symbol-injection
//     pattern (src/c4ix/loader.c loader_systable): the loader scans
//     the image's symbols and writes the API table address into it.
//
// EXIT opcode note: on every machine in this family, EXIT ends the
// WORLD (c4bb halts through the POWER latch). DOS's own EXIT command
// is the one legitimate use. A transient calling libc exit() gets the
// trampoline via include/c4dos.h instead; a transient that hard-EXITs
// halts the machine, which is honest.
//
// Dialect: strict c4 (no switch/break/continue/for, no structs, no
// local arrays, locals at top, single-word globals, literal enums,
// definition before use). '\t'/'\r' written as 9/13 (lexer quirk).

// Build (every variant goes through our own cpp first - a compiler that
// skips '#' lines would compile BOTH clock branches in), then c4lc -O.
// What the Makefile does:
//   ./cpp src/c4dos/c4dos.c > pp.c
//   ./c4sp -R src/c4sp/lisp/c4lc.lisp -O pp.c c4dos.c4r           (clockless)
//   ./cpp -DC4DOS_CLOCK=1 ... ; same c4lc line -> c4dos-clock.c4r (with clock)
//   ./cpp -DC4DOS_CLOCK=1 ... ; ./c4sp32 -c 64000000 ... c4lc.lisp -O
//                                                -> c4dos32.c4r  (c4bb)
// (src/c4dos/tests/test-c4dos.sh still builds its own copies with
// `./cpp | ./c4cc`, which works as well.)
// The guard below keeps cpp from chasing gcc's headers; plain c4 and
// raw c4cc skip '#' lines and never see any of it.
#ifdef __GNUC__
#include <stdio.h>
#include <stdlib.h>
#include <memory.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#define int long long
#endif

enum { LEA ,IMM ,JMP ,JSR ,BZ  ,BNZ ,ENT ,ADJ ,LEV ,LI  ,LC  ,SI  ,SC  ,PSH ,
       OR  ,XOR ,AND ,EQ  ,NE  ,LT  ,GT  ,LE  ,GE  ,SHL ,SHR ,ADD ,SUB ,MUL ,DIV ,MOD ,
       OPEN,READ,CLOS,PRTF,MALC,FREE,MSET,MCMP,EXIT };

enum { MAX_SEARCH = 512 };
enum { BUF_MAX  = 4194304 };  // 4MB image read buffer
enum { LINEMAX  = 256 };
enum { DIRMAX   = 16384 };   // c4dos.dir, slurped for name resolution
enum { RAMFILES  = 64 };     // RAM disk slots
enum { RAMHANDLE = 8 };      // RAM files open at once
enum { RAMFD     = 1000 };   // pseudo-fd base, above any host descriptor
enum { WHANDLE   = 16 };     // files open for writing at once
enum { WFD       = 2000 };   // write handles start here (create's answers)
enum { RAMSIZE   = 1048576 };// default RAM disk budget, SIZE= overrides
enum { C4DOS_API_SLOTS = 32 }; // API table words (include/c4dos.h agrees)
enum { ARGVMAX  = 16 };
enum { BATDEPTH = 4 };

enum { VER_MAJOR = 0, VER_MINOR = 1 };

// ---- state (single-word globals only) ------------------------------------

int *c4l_stub_slot;      // operand slot of the rewritten invoke stub
int g_tramp_pc;          // the trampoline tail: ADJ/LEV, learned at boot
int g_run_bp;            // the innermost run frame's address (frame_enter)
int g_in_trans;          // a run frame is open (dos_exit sanity check)
int g_exited;            // the frame being left was left through dos_exit...
int g_exit_status;       // ...with this status
int g_last_exited;       // what frame_call found, for its caller
int g_depth;             // run frames open, 0 at the prompt

char *g_line;            // the command line buffer
char **g_argv;           // parsed argument vector
char *g_scratch;         // shared file-read scratch (BUF_MAX)
char *g_dirbuf;          // c4dos.dir contents (NOT g_scratch: a name is
                         // resolved while a caller is filling scratch)
char *g_namebuf;         // the real spelling a resolve found
char *g_progbuf;         // a command with ".c4r" appended
char *g_batbuf;          // a command with ".bat" appended
char *g_batreq;          // dispatch found a batch file; the CALLER runs it
int   g_batdepth;

// ---- the RAM disk (DEVICE=RAMDISK.SYS) -----------------------------------
// c4 and c4m have no write primitive at all, and c4bb's disk controller
// is read-only, so "writing a file" cannot mean what it usually means.
// It is a DOS SERVICE instead: a name -> buffer table that lives as long
// as the machine is on. Opens check it BEFORE the disk, so a tool that
// rewrites a file shadows the read-only original rather than failing.
int  g_ramdisk;          // DEVICE=RAMDISK.SYS installed
int  g_ram_budget;       // total bytes allowed
int  g_ram_used;         // total bytes held
int  g_ram_n;            // slots in use
int *g_ram_name;         // char* per slot
int *g_ram_data;         // char* per slot
int *g_ram_len;          // bytes held per slot
int *g_ram_cap;          // bytes allocated per slot
int *g_rh_slot;          // open handle -> slot, -1 free
int *g_rh_pos;           // open handle -> read position
// Files open for WRITING: create's handle is WFD + an entry here, which
// says where the file is going -- a RAM-disk slot, or a drive's write
// head -- so the handle means the same file whatever happens to the
// RAM disk's slots meanwhile (an unlink moves them down).
enum { WH_FREE = 0, WH_RAM = 1, WH_DRV = 2 };
int *g_wh_kind;          // WH_*
int *g_wh_ref;           // the RAM slot (-1: deleted while open), or the drive's fd
int *g_wh_drv;           // the drive, for WH_DRV
int *g_wh_name;          // char*: its name there, for that drive's c4dos.dir
int *g_wh_img;           // the program that created it (closed when it goes)
char *g_inbuf;           // console byte stream: one read() may carry
int g_inlen; int g_inpos; // many lines (a pipe) or one (a cooked tty),
                          // so lines are extracted here, never assumed.
                          // (c4sh assumed - test-c4bb.sh:104 documents
                          // the dropped-bytes bug that caused.)

int g_echo;              // batch ECHO state
int g_clock;             // CONFIG.SYS said DEVICE=CLOCK.SYS
int g_conflush;          // CONFIG.SYS said DEVICE=CONSOLE.SYS FLUSH
int g_drives;            // CONFIG.SYS said DEVICE=DRIVES.SYS
int g_drive;             // the drive the prompt is on, 0 = A:
int g_home;              // DRIVES.SYS HOME=: where files are written, or -1
int g_quit;              // EXIT was typed
int g_linelen;           // the length of the line parse_line last split
int g_redir;             // CONFIG.SYS said DEVICE=REDIR.SYS: > >> < and |
int g_tap;               // ... with TAP: the machine has the console tap

// ---- drives ---------------------------------------------------------
//
// BOARD ONLY, which is why it is gated. These are c4bb's memory-mapped
// device registers (include/c4bb.h has the full set); on the board they
// are ordinary loads and stores and cost no opcode above EXIT, and
// under native c4m there is no device window there at all -- the read
// would be a wild access rather than a zero. So DOS does not probe: it
// is TOLD, by DEVICE=DRIVES.SYS, exactly as it is told about the clock
// and the console. Without that line C4DOS behaves as it always has,
// one drive called A: and no way to leave it.
enum { BB_DRIVE = 0x13c, BB_COUNT = 0x188 };
// ... and its write head (include/c4bb.h, docs/c4bb-storage.md): the
// same registers bbsave and install drive, and DISK_UNLINK beside them
enum { BB_FD = 0x124, BB_WNAME = 0x178, BB_WADDR = 0x17c, BB_WLEN = 0x180,
       BB_WCLOSE = 0x184, BB_RO = 0x18c, BB_UNLINK = 0x1cc };

int drv_count ()       { if (!g_drives) return 1; return *(int *)BB_COUNT; }
void drv_select (int n) { if (g_drives) *(int *)BB_DRIVE = n; }

// 'A' + n, and back. The machine is happy with either spelling -- its
// resolveDrive takes 0: and A: alike -- but a person should only ever
// meet one, and the prompt has said A> since the first boot.
int drv_letter (int n) { return 'A' + n; }
int drv_num (char *s) {
  int c;
  if (!s[0] || s[1] != ':' || s[2]) return 0 - 1;
  c = s[0];
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a';
  return 0 - 1;
}

// The drive a file name starts with ("B:NOTE.TXT"), or -1. A letter and
// a colon, nothing longer: "RAM:" is not a drive.
int drv_prefix (char *s) {
  int c;
  if (!s[0] || s[1] != ':') return 0 - 1;
  c = s[0];
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a';
  return 0 - 1;
}

// Is drive n there, and can it be written? DISK_RO speaks for the
// selected drive, so this selects n to ask and puts the selection back.
int drv_writable (int n) {
  int old, ro;
  if (!g_drives || n < 0 || n >= drv_count()) return 0;
  old = *(int *)BB_DRIVE;
  *(int *)BB_DRIVE = n;
  ro = *(int *)BB_RO;
  *(int *)BB_DRIVE = old;
  return !ro;
}

// "X:" and a name, which is how the registers are told the drive (the
// machine's resolveDrive), in g_wname; 0 when it does not fit
char *g_wname;
char *drv_path (int n, char *name) {
  int i;
  g_wname[0] = drv_letter(n);
  g_wname[1] = ':';
  i = 0;
  while (name[i]) {
    if (i + 3 >= LINEMAX) return 0;
    g_wname[i + 2] = name[i];
    ++i;
  }
  g_wname[i + 2] = 0;
  return g_wname;
}

// The write head itself. A file is on the medium only once it is closed,
// in one piece.
int bb_create (char *path) { *(int *)BB_WNAME = (int)path; return *(int *)BB_WNAME; }
int bb_write (int fd, char *buf, int n) {
  *(int *)BB_FD = fd;
  *(int *)BB_WADDR = (int)buf;
  *(int *)BB_WLEN = n;
  return *(int *)BB_WLEN;
}
int bb_close (int fd) { *(int *)BB_WCLOSE = fd; return *(int *)BB_WCLOSE; }
int bb_unlink (char *path) { *(int *)BB_UNLINK = (int)path; return *(int *)BB_UNLINK; }

// ---- heap generations (DEVICE=HEAP.SYS) ----------------------------
//
// Nothing frees what a program allocated and never gave back, so before
// this a game that took 15 MB left most of it behind every time it ran,
// and "play, quit, play another" ran out after a few rounds. The heap
// can now tag each block with a generation and free a whole generation
// with one store (libjs's heap.js; HEAP_GEN and HEAP_RELEASE, beside
// HEAP_USED in src/c4bb/sim/devices.js). DOS gives each program a
// generation of its own and releases it when the program is unloaded.
//
// The rule that makes it safe: DOS's own memory is generation 0, which
// the heap never releases, and every API entry that allocates on DOS's
// behalf -- a RAM-disk file a child creates or grows, the scratch, a
// loaded image's handle, an exec's command line -- switches to it for
// the allocation and back (gen_dos/gen_set). Otherwise a file written by
// a child would be freed with the child.
//
// ANNOUNCED, never probed, like the drives: these are device registers
// on libjs and ordinary memory under native c4m. And once announced,
// checked: a machine with the registers but no heap behind them (c4bb,
// whose heap is its firmware's) reads HEAP_GEN back as 0, and DOS then
// says so and carries on without.
enum { BB_HEAP_BASE = 0x144, BB_HEAP_END = 0x148, BB_HEAP_USED = 0x1c0,
       BB_HEAP_GEN = 0x1c4, BB_HEAP_RELEASE = 0x1c8 };

int g_heapgen;           // CONFIG.SYS said DEVICE=HEAP.SYS, and it answered
int g_gen_next;          // the last generation handed out

int gen_get () { if (!g_heapgen) return 0; return *(int *)BB_HEAP_GEN; }
int gen_set (int g) { if (g_heapgen) *(int *)BB_HEAP_GEN = g; return 0; }
// switch to DOS's own generation; returns the one to go back to
int gen_dos () {
  int old;
  if (!g_heapgen) return 0;
  old = *(int *)BB_HEAP_GEN;
  *(int *)BB_HEAP_GEN = 0;
  return old;
}
int gen_release (int g) { if (g_heapgen && g) *(int *)BB_HEAP_RELEASE = g; return 0; }

// loaded-image registers (the plain-c4 "struct"), filled by c4r_load and
// copied straight into a handle (img_load) before anything else can run
int *img_code; char *img_data; int img_entry; int *img_cons; int img_ncons;
int *img_des; int img_ndes;

// ---- tiny libc -----------------------------------------------------------

int xlen (char *s) { int n; n = 0; while (s[n]) ++n; return n; }

int upper (int c) { if (c >= 'a' && c <= 'z') return c - 32; return c; }

// case-insensitive equality, DOS style
int cieq (char *a, char *b) {
  while (*a && *b) {
    if (upper(*a) != upper(*b)) return 0;
    ++a; ++b;
  }
  return !*a && !*b;
}

int starts_ci (char *s, char *prefix) {
  while (*prefix) {
    if (upper(*s) != upper(*prefix)) return 0;
    ++s; ++prefix;
  }
  return 1;
}

// ---- the shared scratch --------------------------------------------------
// g_scratch is 4MB, and the biggest single thing DOS holds. A kernel
// loaded by dosload is about to want every byte of it, so the API can
// hand it back (slot TRIM). That is only safe if nobody caches the
// pointer: every user asks for it through here, and gets it back --
// re-allocated if it went away. Returns 0 only when memory is gone,
// which every caller already had to handle.
char *scratch_need () {
  int g;
  if (!g_scratch) {
    g = gen_dos();       // DOS's, whoever's call brought it back
    if (!(g_scratch = malloc(BUF_MAX))) printf("c4dos: out of memory\n");
    gen_set(g);
  }
  return g_scratch;
}

// ---- the invoke stub (c4l.c's mechanism) ---------------------------------

int caller_address (int dummy) {
  int *addr, *next, i;
  addr = (int *)(*(&addr + 2));
  i = 0;
  next = addr;
  while (++i <= MAX_SEARCH) {
    --next;
    if (*addr == ENT) {
      if (*next > ADJ) return (int)addr;
    }
    addr = next;
  }
  printf("c4dos: could not find caller entry\n");
  return 0;
}

int stub () {
  int *self;
  if (!(self = (int *)caller_address(0))) exit(1);
  *self = JMP;
  c4l_stub_slot = self + 1;
  return 0;
}

// invoke0 passes a dummy argument on purpose. c4lc -O turns a bare
// "JSR f; LEV" into a tail call, JMP f+2, which enters f PAST its first
// word -- and the stub's first word is the JMP it rewrote itself into.
// A zero-argument stub() call from here therefore re-ran the stub's
// original body, which rewrote whatever code sat near the caller: under
// C4DOS the first program with a destructor (c4th, anything built with
// stdarg.h) left the next one silent or crashing. With an argument the
// call is followed by ADJ, which is not a tail call. invoke1/2 always were.
int invoke0 (int *code) { *c4l_stub_slot = (int)code; return stub(0); }
int invoke1 (int *code, int a) { *c4l_stub_slot = (int)code; return stub(a); }
int invoke2 (int *code, int a, int b) { *c4l_stub_slot = (int)code; return stub(a, b); }
int invoke3 (int *code, int a, int b, int c) { *c4l_stub_slot = (int)code; return stub(a, b, c); }

// ---- the trampoline (test_coop_switch.c's mechanism) ---------------------
// tramp_mark records its raw return pc: the instruction after the JSR
// inside learn_trampoline, which is that function's ADJ/LEV tail. A
// frame whose return pc points there performs the second LEV that
// re-derives sp from the new bp. See the design doc.

int tramp_mark () {
  int *addr;
  g_tramp_pc = *(&addr + 2);
  return 0;
}

int learn_trampoline () {
  tramp_mark();
  return 0;
}

// The transient-facing API. Slot 1 of the table handed to programs
// via __c4dos_api. A transient's exit() calls this instead of the
// EXIT opcode (which would halt the machine), from however deep it is:
// the trampoline below returns from the innermost run frame
// (frame_enter), not from this call. libc's bare.c calls it directly;
// include/c4dos.h's dos_exit() reaches it through the invoke stub.
//
// The status rides home in g_exit_status, not in A. The trampoline tail
// is whatever follows the JSR in learn_trampoline, and learn_trampoline's
// `return 0` may well put an IMM 0 there. Nothing read the status while
// the prompt was the only caller; exec, start and catch do.
int dos_exit (int status) {
  int *bp;
  if (!g_in_trans) {
    printf("c4dos: dos_exit outside a program?\n");
    exit(1);
  }
  g_in_trans = 0;
  g_exited = 1;
  g_exit_status = status;
  bp = (int *)(&bp + 1);
  *bp = g_run_bp;          // after our LEV: bp = frame_enter's frame...
  *(bp + 1) = g_tramp_pc;  // ...pc = the trampoline tail (ADJ/LEV)
  return status;
}

// ---- run frames ----------------------------------------------------------
// Every call DOS makes into a program's code -- its constructors, main,
// its destructors, and whatever a caller hands to slot 23 (catch) -- goes
// through a run frame, so an exit() anywhere below lands back here.
// Frames nest: a program can exec another, which can exec a third, and
// each exit() returns to the innermost. That is all re-entrancy needed
// from the trampoline: frame_call keeps the outer frame's g_run_bp and
// g_in_trans in its own locals while the inner one is open.
//
// frame_enter records its own frame and makes the call. On a normal
// return it returns what the callee did; through dos_exit it returns
// as if it had, with A unspecified -- frame_call, its caller, reads
// g_exited to tell which happened.
int frame_enter (int code, int n, int a, int b) {
  int *bp;
  bp = (int *)(&bp + 1);
  g_run_bp = (int)bp;
  g_in_trans = 1;
  if (n == 2) return invoke2((int *)code, a, b);
  return invoke1((int *)code, a);
}

// Call code(a) (n == 1) or code(a, b) (n == 2) under a run frame. Returns
// its value, or the exit() status if it left that way; g_last_exited says
// which.
int frame_call (int code, int n, int a, int b) {
  int old_bp, old_in, r, ex;
  old_bp = g_run_bp;
  old_in = g_in_trans;
  ++g_depth;
  g_exited = 0;
  r = frame_enter(code, n, a, b);
  ex = g_exited;
  g_exited = 0;
  if (ex) r = g_exit_status;
  --g_depth;
  g_run_bp = old_bp;
  g_in_trans = old_in;
  g_last_exited = ex;
  return r;
}

// ---- the loader (src/c4ix/loader.c's parser, v3 MEMSZ-aware) -------------

int wordat (char *p) { return *(int *)p; }

int nameis (char *p, int len, char *want) {
  int i;
  i = 0;
  while (i < len) {
    if (!want[i]) return 0;
    if (p[i] != want[i]) return 0;
    ++i;
  }
  return !want[i];
}

// Walk the symbol section for __c4dos_api and write the API table's
// address into the image's global, so one binary runs under C4DOS
// (calls through the slot) or anywhere else (slot stays 0).
int inject_api (char *p, int nsyms, char *database, int *api) {
  int i, cls, namelen, value;
  i = 0;
  while (i < nsyms) {
    p = p + sizeof(int);                       // id
    p = p + sizeof(int);                       // type
    cls = wordat(p); p = p + sizeof(int);      // class
    p = p + sizeof(int);                       // attrs
    namelen = *p; p = p + 1;
    if (nameis(p, namelen, "__c4dos_api")) {
      p = p + namelen;
      value = wordat(p);
      if (cls == 131) *(int *)(database + value) = (int)api;  // Glo
      return 1;
    }
    p = p + namelen;
    p = p + sizeof(int);                       // value
    ++i;
  }
  return 0;
}

// Load path -> the img_* registers. Returns 1 on success.
// ---- the RAM disk --------------------------------------------------------

// "ram:name" is the RAM disk's copy and nothing else. cpp and c4cc
// report what they write as ram:<file>, so that spelling has to open --
// but the RAM disk is not a drive you can switch to: it sits over every
// drive, ahead of the disk, and the plain name reaches it too.
char *ram_name (char *path) {
  if (starts_ci(path, "RAM:")) return path + 4;
  return 0;
}

// slot holding `name`, or -1. Case-insensitive, like every other name
// on this system.
int ram_find (char *name) {
  int i;
  i = 0;
  while (i < g_ram_n) {
    if (cieq((char *)g_ram_name[i], name)) return i;
    ++i;
  }
  return 0 - 1;
}

// create or truncate `name` with room for `cap` bytes; returns the slot
int ram_create (char *name, int cap) {
  int i, slot;
  char *p, *q;
  if (!g_ramdisk) return 0 - 1;
  if (ram_name(name)) name = ram_name(name);
  slot = ram_find(name);
  if (slot >= 0) {
    g_ram_used = g_ram_used - g_ram_cap[slot];
    if (g_ram_data[slot]) free((char *)g_ram_data[slot]);
  } else {
    if (g_ram_n >= RAMFILES) { printf("ramdisk full (%d files)\n", RAMFILES); return 0 - 1; }
    slot = g_ram_n;
    if (!(p = malloc(LINEMAX))) return 0 - 1;
    q = p;
    while (*name) { *q = *name; ++q; ++name; }
    *q = 0;
    g_ram_name[slot] = (int)p;
    ++g_ram_n;
  }
  if (cap < 4096) cap = 4096;
  if (g_ram_used + cap + 1 > g_ram_budget) {
    printf("ramdisk full (%d bytes)\n", g_ram_budget);
    g_ram_data[slot] = 0; g_ram_len[slot] = 0; g_ram_cap[slot] = 0;
    return 0 - 1;
  }
  if (!(p = malloc(cap + 1))) {
    g_ram_data[slot] = 0; g_ram_len[slot] = 0; g_ram_cap[slot] = 0;
    return 0 - 1;
  }
  g_ram_data[slot] = (int)p;
  g_ram_len[slot] = 0;
  g_ram_cap[slot] = cap + 1;
  g_ram_used = g_ram_used + cap + 1;
  p[0] = 0;
  i = 0;
  return slot;
}

// Append, growing the buffer when it runs out. A tool streaming an
// image out has no idea how big it will be, and realloc is documented
// broken under c4m, so this doubles by hand.
int ram_write (int slot, char *buf, int n) {
  char *d, *nb;
  int i, need;
  if (slot < 0 || slot >= g_ram_n) return 0;
  d = (char *)g_ram_data[slot];
  if (!d) return 0;
  need = g_ram_len[slot] + n + 1;
  if (need > g_ram_cap[slot]) {
    i = g_ram_cap[slot] * 2 + n + 4096;
    if (g_ram_used - g_ram_cap[slot] + i > g_ram_budget) {
      printf("ramdisk full (%d bytes)\n", g_ram_budget);
      return 0;
    }
    if (!(nb = malloc(i))) { printf("c4dos: out of memory\n"); return 0; }
    need = 0;
    while (need < g_ram_len[slot]) { nb[need] = d[need]; ++need; }
    free(d);
    g_ram_used = g_ram_used - g_ram_cap[slot] + i;
    g_ram_cap[slot] = i;
    g_ram_data[slot] = (int)nb;
    d = nb;
  }
  i = 0;
  while (i < n) { d[g_ram_len[slot] + i] = buf[i]; ++i; }
  g_ram_len[slot] = g_ram_len[slot] + n;
  d[g_ram_len[slot]] = 0;
  return n;
}

// Delete `name` from the RAM disk; 0, or -1 if it is not there. Later
// files move down a slot, so the listing keeps no holes (count/entname
// enumerate by index). Open handles follow their files, read and write
// alike; a handle on the file deleted reads nothing more, and what is
// written to it goes nowhere.
int ram_unlink (char *name) {
  int slot, i;
  if (!g_ramdisk) return 0 - 1;
  if (ram_name(name)) name = ram_name(name);
  if ((slot = ram_find(name)) < 0) return 0 - 1;
  g_ram_used = g_ram_used - g_ram_cap[slot];
  if (g_ram_data[slot]) free((char *)g_ram_data[slot]);
  free((char *)g_ram_name[slot]);
  i = 0;
  while (i < RAMHANDLE) {
    if (g_rh_slot[i] == slot) g_rh_slot[i] = 0 - 1;
    else if (g_rh_slot[i] > slot) g_rh_slot[i] = g_rh_slot[i] - 1;
    ++i;
  }
  i = 0;
  while (i < WHANDLE) {
    if (g_wh_kind[i] == WH_RAM && g_wh_ref[i] == slot) g_wh_ref[i] = 0 - 1;
    else if (g_wh_kind[i] == WH_RAM && g_wh_ref[i] > slot) g_wh_ref[i] = g_wh_ref[i] - 1;
    ++i;
  }
  i = slot;
  while (i + 1 < g_ram_n) {
    g_ram_name[i] = g_ram_name[i + 1];
    g_ram_data[i] = g_ram_data[i + 1];
    g_ram_len[i] = g_ram_len[i + 1];
    g_ram_cap[i] = g_ram_cap[i + 1];
    ++i;
  }
  --g_ram_n;
  return 0;
}

// open a RAM file for reading; returns a pseudo-fd or -1
int ram_open (char *name) {
  int slot, h;
  if (!g_ramdisk) return 0 - 1;
  if ((slot = ram_find(name)) < 0) return 0 - 1;
  h = 0;
  while (h < RAMHANDLE) {
    if (g_rh_slot[h] < 0) {
      g_rh_slot[h] = slot;
      g_rh_pos[h] = 0;
      return RAMFD + h;
    }
    ++h;
  }
  return 0 - 1;
}

// ---- finding a file ------------------------------------------------------
//
// DOS never cared about case; a host filesystem does, and c4bb's disk
// does too. Rather than guess at spellings, ask the directory: DIR is
// already a file (c4dos.dir), and it is the only thing on this system
// that knows what is actually here. If a name does not open as typed,
// scan the listing for a case-insensitive match and open the spelling
// that exists. A disk with no c4dos.dir simply keeps the old
// behaviour, which is the same trade DIR already makes.

// compare s against the line at p, which ends at CR, LF or NUL
int line_eq_ci (char *p, char *s) {
  while (*s && *p && *p != 10 && *p != 13) {
    if (upper(*p) != upper(*s)) return 0;
    ++p; ++s;
  }
  if (*s) return 0;
  return !*p || *p == 10 || *p == 13;
}

// A listing into g_dirbuf ("c4dos.dir", the selected drive's, or
// "B:c4dos.dir"); returns its length, 0 if there is no listing
int dir_slurp_path (char *path) {
  int fd, n, total, going;
  // The raw open, deliberately: this is the resolver, and routing it
  // through dos_open would recurse straight back into itself.
  g_dirbuf[0] = 0;
  if ((fd = open(path, 0)) < 0) return 0;
  total = 0;
  going = 1;
  while (going) {
    if ((n = read(fd, g_dirbuf + total, 4096)) <= 0) going = 0;
    else {
      total = total + n;
      if (total >= DIRMAX - 4097) going = 0;
    }
  }
  close(fd);
  g_dirbuf[total] = 0;
  return total;
}

char *g_dname;           // "X:c4dos.dir"
char *dir_name (int n) {
  char *s, *d;
  g_dname[0] = drv_letter(n);
  g_dname[1] = ':';
  s = "c4dos.dir";
  d = g_dname + 2;
  while (*s) { *d = *s; ++d; ++s; }
  *d = 0;
  return g_dname;
}

// the real spelling of `name` according to the listing just slurped, or 0
char *dir_find (char *name) {
  char *p, *q;
  p = g_dirbuf;
  while (*p) {
    if (line_eq_ci(p, name)) {
      q = g_namebuf;
      while (*p && *p != 10 && *p != 13) { *q = *p; ++q; ++p; }
      *q = 0;
      return g_namebuf;
    }
    while (*p && *p != 10) ++p;
    if (*p) ++p;
  }
  return 0;
}

// ... in the selected drive's listing, and in drive n's
char *dir_resolve (char *name) {
  if (!dir_slurp_path("c4dos.dir")) return 0;
  return dir_find(name);
}
char *dir_resolve_on (int n, char *name) {
  if (!dir_slurp_path(dir_name(n))) return 0;
  return dir_find(name);
}

// ---- keeping a drive's listing ----------------------------------------
// DIR, and grin's File Manager, list a drive by its c4dos.dir, so a file
// DOS writes to a drive is added to the drive's listing when it is
// closed, and taken out when it is deleted; a drive that had no listing
// gets one. The listing is written as any file is, through the write
// head, whole.

// g_dirbuf's first len bytes as drive n's c4dos.dir: 0, or -1
int dir_put (int n, int len) {
  int fd, r;
  if ((fd = bb_create(dir_name(n))) < 0) return 0 - 1;
  r = 0;
  if (len > 0 && bb_write(fd, g_dirbuf, len) != len) r = 0 - 1;
  if (bb_close(fd) < 0) r = 0 - 1;
  return r;
}

int dir_add (int n, char *name) {
  int len, i;
  if (cieq(name, "c4dos.dir")) return 0;
  len = dir_slurp_path(dir_name(n));
  if (dir_find(name)) return 0;
  if (len + xlen(name) + 2 >= DIRMAX) return 0 - 1;
  if (len > 0 && g_dirbuf[len - 1] != 10) { g_dirbuf[len] = 10; ++len; }
  i = 0;
  while (name[i]) { g_dirbuf[len] = name[i]; ++len; ++i; }
  g_dirbuf[len] = 10; ++len;
  g_dirbuf[len] = 0;
  return dir_put(n, len);
}

int dir_drop (int n, char *name) {
  int len, out;
  char *p;
  if (!(len = dir_slurp_path(dir_name(n)))) return 0;
  p = g_dirbuf;
  out = 0;
  while (*p) {
    if (line_eq_ci(p, name)) {
      while (*p && *p != 10) ++p;
      if (*p) ++p;
    } else {
      while (*p && *p != 10) { g_dirbuf[out] = *p; ++out; ++p; }
      if (*p) { g_dirbuf[out] = 10; ++out; ++p; }
    }
  }
  g_dirbuf[out] = 0;
  return dir_put(n, out);
}

// Drive n's file, case-insensitively; -1 if it is not there
int drv_open (int n, char *path) {
  int fd;
  char *real;
  if (!drv_path(n, path)) return 0 - 1;
  if ((fd = open(g_wname, 0)) >= 0) return fd;
  if ((real = dir_resolve_on(n, path)) && drv_path(n, real)) return open(g_wname, 0);
  return 0 - 1;
}

// open for reading, case-insensitively. Every file this system opens
// goes through here. A name with a drive ("B:NOTE.TXT") is that drive's
// file. Any other is looked for in the RAM disk, then on the home drive
// (HOME=, where files are written), then on the drive the prompt is on.
int dos_open (char *path) {
  int fd, n;
  char *real;
  // "./name" IS "name". c4ke.c says #include "./load-c4r.c", and a
  // name that came out of an archive is stored plainly -- c4bb's own
  // disk controller strips the prefix for the same reason.
  if (path[0] == '.' && path[1] == '/') path = path + 2;
  if ((real = ram_name(path))) return ram_open(real);
  if (g_drives && (n = drv_prefix(path)) >= 0) return drv_open(n, path + 2);
  // RAM first, disk second -- the same precedence C4KE's loader uses,
  // and the reason a tool can rewrite a file that shipped read-only.
  if ((fd = ram_open(path)) >= 0) return fd;
  if (g_home >= 0 && g_home != g_drive && (fd = drv_open(g_home, path)) >= 0) return fd;
  if ((fd = open(path, 0)) >= 0) return fd;
  if ((real = dir_resolve(path))) {
    if ((fd = ram_open(real)) >= 0) return fd;
    return open(real, 0);
  }
  return 0 - 1;
}

// Reads and closes have to go through here too: a pseudo-fd is not
// something the host has ever heard of.
int dos_read (int fd, char *buf, int n) {
  int h, slot, left, i;
  char *d;
  if (fd < RAMFD) return read(fd, buf, n);
  h = fd - RAMFD;
  if (h < 0 || h >= RAMHANDLE) return 0;
  slot = g_rh_slot[h];
  if (slot < 0) return 0;
  d = (char *)g_ram_data[slot];
  left = g_ram_len[slot] - g_rh_pos[h];
  if (left <= 0) return 0;
  if (n < left) left = n;
  i = 0;
  while (i < left) { buf[i] = d[g_rh_pos[h] + i]; ++i; }
  g_rh_pos[h] = g_rh_pos[h] + left;
  return left;
}

int dos_close (int fd) {
  int h;
  if (fd < RAMFD) return close(fd);
  h = fd - RAMFD;
  if (h >= 0 && h < RAMHANDLE) g_rh_slot[h] = 0 - 1;
  return 0;
}

// ---- writing a file ------------------------------------------------------
// Where a new file goes (slot 2, create; COPY):
//   - "RAM:name": the RAM disk;
//   - "B:name": drive B:, if it can be written, else the RAM disk;
//   - a plain name: the home drive (DRIVES.SYS HOME=) if there is one and
//     it can be written, else the RAM disk.
// Without HOME= -- the build floppies -- a plain name goes to the RAM
// disk, as it always did. A file written to a drive is on it, and in its
// c4dos.dir, once it is closed; one written to the RAM disk is there at
// once. A plain name written to the home drive takes the RAM disk's copy
// of that name away, if there was one, so that the file read next is
// the one just written.

int g_cur_img;           // (the loader's, below) the program running, 0 = none

int wh_new (int kind, int ref, int drv, char *name) {
  int h, i;
  char *d;
  h = 0;
  while (h < WHANDLE && g_wh_kind[h]) ++h;
  if (h == WHANDLE) { printf("c4dos: %d files open for writing, no room for another\n", WHANDLE); return 0 - 1; }
  g_wh_kind[h] = kind;
  g_wh_ref[h] = ref;
  g_wh_drv[h] = drv;
  g_wh_img[h] = g_cur_img;
  d = (char *)g_wh_name[h];
  i = 0;
  while (name[i] && i < LINEMAX - 1) { d[i] = name[i]; ++i; }
  d[i] = 0;
  return WFD + h;
}

// the entry a handle names, or -1
int wh_get (int handle) {
  int h;
  h = handle - WFD;
  if (h < 0 || h >= WHANDLE || !g_wh_kind[h]) return 0 - 1;
  return h;
}

// create or truncate a file for writing; its handle, or -1
int file_create (char *name) {
  int n, fd, slot, plain;
  char *rest;
  if (name[0] == '.' && name[1] == '/') name = name + 2;
  rest = name;
  plain = 0;
  n = 0 - 1;
  if (ram_name(name)) rest = ram_name(name);
  else if (g_drives && (n = drv_prefix(name)) >= 0) rest = name + 2;
  else { n = g_home; plain = 1; }
  if (n >= 0 && *rest && drv_writable(n) && drv_path(n, rest)) {
    if ((fd = bb_create(g_wname)) < 0) return 0 - 1;
    if (plain && ram_find(rest) >= 0) ram_unlink(rest);
    if ((slot = wh_new(WH_DRV, fd, n, rest)) < 0) bb_close(fd);
    return slot;
  }
  if ((slot = ram_create(rest, 4096)) < 0) return 0 - 1;
  return wh_new(WH_RAM, slot, 0 - 1, rest);
}

int file_write (int handle, char *buf, int n) {
  int h;
  if ((h = wh_get(handle)) < 0) return 0;
  if (g_wh_kind[h] == WH_DRV) return bb_write(g_wh_ref[h], buf, n);
  if (g_wh_ref[h] < 0) return n;     // deleted while open: written nowhere
  return ram_write(g_wh_ref[h], buf, n);
}

// 0, or -1 when the drive would not take it
int file_close (int handle) {
  int h, r;
  if ((h = wh_get(handle)) < 0) return 0 - 1;
  r = 0;
  if (g_wh_kind[h] == WH_DRV) {
    if (bb_close(g_wh_ref[h]) < 0) r = 0 - 1;
    else dir_add(g_wh_drv[h], (char *)g_wh_name[h]);
  }
  g_wh_kind[h] = WH_FREE;
  return r;
}

// a program that ends with files still open for writing: closed for it,
// so what it wrote is kept and the entries come back
int file_close_img (int img) {
  int h;
  h = 0;
  while (h < WHANDLE) {
    if (g_wh_kind[h] && g_wh_img[h] == img) file_close(WFD + h);
    ++h;
  }
  return 0;
}

// Delete a file: "RAM:name" from the RAM disk, "B:name" from drive B:, a
// plain name from the RAM disk if it is there, else from the home drive.
// 0, or -1 if there was no such file or the drive cannot be written.
int drv_unlink (int n, char *name) {
  char *real;
  if (!drv_writable(n) || !drv_path(n, name)) return 0 - 1;
  if (bb_unlink(g_wname) < 0) {
    if (!(real = dir_resolve_on(n, name)) || !drv_path(n, real) || bb_unlink(g_wname) < 0) return 0 - 1;
    name = real;
  }
  dir_drop(n, name);
  return 0;
}

int file_unlink (char *name) {
  int n;
  if (name[0] == '.' && name[1] == '/') name = name + 2;
  if (ram_name(name)) return ram_unlink(name);
  if (g_drives && (n = drv_prefix(name)) >= 0) return drv_unlink(n, name + 2);
  if (ram_unlink(name) == 0) return 0;
  if (g_home >= 0) return drv_unlink(g_home, name);
  return 0 - 1;
}

// ---- the console (API v4 slots 16, 17 and 24) ----------------------------
// A program linked with libc's bare.c writes fd 1 and 2 and reads fd 0
// through these, so DOS decides where its console goes: the real one, a
// sink a shell installed (grin's DOS Prompt windows), or a redirect file
// (REDIR.SYS). DOS's own builtins print through here too (pf, below).
// c4cc programs print with PRTF and never come here; the console tap
// catches them where the machine has one (REDIR.SYS TAP).
int g_sink_w;            // write(ctx, buf, n), or 0 for the console
int g_sink_r;            // read(ctx, buf, n), or 0 for the console
int g_sink_ctx;
int g_sink_img;          // the program that installed the sink

// Redirection's state, for the command running now. Each level of
// redirection keeps the outer one's in its locals (run_seg), so a batch
// file redirected as a whole can redirect its own lines.
int g_out_h;             // output goes to this write handle, or -1
char *g_in_data;         // input comes from these bytes (a whole file), or 0
int g_in_len;
int g_in_pos;

// The console tap (src/c4bb/sim/devices.js; docs/c4dos-design.md "The
// console tap"): while CON_TAP is 1 the machine keeps what PRTF, PUTC and
// PUTS send in a buffer of its own instead of the UART, and DOS drains it
// (CON_ADDR, CON_LEN) to wherever the console goes; CON_IN feeds fd-0
// READs from a guest buffer, then end of input (-1: the keyboard again).
// ANNOUNCED, never probed, by DEVICE=REDIR.SYS TAP: under native c4m
// these addresses are nothing at all.
enum { BB_CON_TAP = 0x1d0, BB_CON_ADDR = 0x1d4, BB_CON_LEN = 0x1d8, BB_CON_IN = 0x1dc };
enum { TAPBUF = 1024 };
int g_tapping;           // CON_TAP is 1
int g_tap_said;          // "no tap here" has been said
char *g_tapbuf;          // a drained piece

enum { INBUFMAX = 4096 };

// n bytes to the real console. %.*s a run at a time, which every printf
// in the family takes (c4bb's firmware formatter does too), and a NUL,
// which would end a run, on its own. With the tap capturing, it is let
// go for these: they are meant for the console.
int con_put (char *buf, int n) {
  int i, j;
  if (g_tapping) *(int *)BB_CON_TAP = 0;
  i = 0;
  while (i < n) {
    j = i;
    while (j < n && buf[j]) ++j;
    if (j > i) printf("%.*s", j - i, buf + i);
    if (j < n) { printf("%c", 0); ++j; }
    i = j;
  }
  if (g_tapping) *(int *)BB_CON_TAP = 1;
  return n;
}

// Up to n bytes of the real console, a line at a time: what is buffered
// up to and including the next newline, waiting for one if need be; at
// end of input whatever is left, and then 0. The same buffer the prompt
// reads, so a line typed ahead of a program is the program's -- before,
// a program reading fd 0 itself missed whatever DOS had already read.
int con_get (char *buf, int n) {
  int i, k, got;
  if (n <= 0) return 0;
  got = 0 - 1;
  while (got < 0) {
    i = g_inpos;
    while (i < g_inlen && g_inbuf[i] != 10) ++i;
    if (i < g_inlen) got = i + 1 - g_inpos;
    else {
      k = g_inlen - g_inpos;
      i = 0;
      while (i < k) { g_inbuf[i] = g_inbuf[g_inpos + i]; ++i; }
      g_inlen = k;
      g_inpos = 0;
      if (g_inlen >= INBUFMAX) got = g_inlen;     // a line longer than the buffer
      else {
        k = read(0, g_inbuf + g_inlen, INBUFMAX - g_inlen);
        if (k <= 0) got = g_inlen;                // end of input
        else g_inlen = g_inlen + k;
      }
    }
  }
  if (got > n) got = n;
  i = 0;
  while (i < got) { buf[i] = g_inbuf[g_inpos + i]; ++i; }
  g_inpos = g_inpos + got;
  return got;
}

// A sink is called as the program that installed it: a program it loads
// is that program's child (grin starting Clock while a DOS Prompt's
// program waits for a line), not the child of whichever program DOS was
// running when it asked for input.
int sink_call (int f, char *buf, int n) {
  int oldimg, r;
  oldimg = g_cur_img;
  g_cur_img = g_sink_img;
  r = invoke3((int *)f, g_sink_ctx, (int)buf, n);
  g_cur_img = oldimg;
  return r;
}

// n bytes where the console goes now: a redirect file, a shell's sink,
// or the console itself. A file grows in DOS's generation: it is DOS's.
int out_put (char *buf, int n) {
  int g;
  if (g_out_h >= 0) {
    g = gen_dos();
    file_write(g_out_h, buf, n);
    gen_set(g);
    return n;
  }
  if (g_sink_w) return sink_call(g_sink_w, buf, n);
  return con_put(buf, n);
}

// What the tap holds, to where the console goes now. Called before
// anything else is written there, so that the order is kept, and when a
// level of redirection ends, so that what it caught goes into its file.
int tap_drain () {
  int n, going;
  if (!g_tapping) return 0;
  going = 1;
  while (going) {
    *(int *)BB_CON_ADDR = (int)g_tapbuf;
    *(int *)BB_CON_LEN = TAPBUF;
    n = *(int *)BB_CON_LEN;
    if (n <= 0) going = 0;
    else out_put(g_tapbuf, n);
  }
  return 0;
}

// The tap made to match the state: capturing while the console goes
// anywhere but the console; fd 0 fed from the input file, or nothing at
// all under a sink (a c4cc program in a DOS Prompt window reads end of
// input rather than the page's keyboard), or else the keyboard.
int tap_set () {
  if (!g_tap) return 0;
  g_tapping = (g_out_h >= 0 || g_sink_w) ? 1 : 0;   // (c4's || gives an operand, not 1)
  *(int *)BB_CON_TAP = g_tapping;
  if (g_in_data) {
    *(int *)BB_CON_ADDR = (int)(g_in_data + g_in_pos);
    *(int *)BB_CON_IN = g_in_len - g_in_pos;
  } else if (g_sink_w) {
    *(int *)BB_CON_ADDR = (int)g_tapbuf;
    *(int *)BB_CON_IN = 0;
  } else *(int *)BB_CON_IN = 0 - 1;
  return 0;
}

int dos_api_conwrite (char *buf, int n) {
  if (n <= 0) return 0;
  tap_drain();
  return out_put(buf, n);
}
// At most a line: from the input file (<), the sink, or the console. A
// prompt the program printed for itself goes out first.
int dos_api_conread (char *buf, int n) {
  int i, k;
  tap_drain();
  if (g_in_data) {
    i = 0;
    while (g_in_pos < g_in_len && i < n) {
      k = g_in_data[g_in_pos];
      buf[i] = k;
      ++i;
      ++g_in_pos;
      if (k == 10) return i;
    }
    return i;
  }
  if (g_sink_r) return sink_call(g_sink_r, buf, n);
  return con_get(buf, n);
}
// Point the console at a caller's functions; 0 for either gives that
// direction back to DOS. They are called as f(ctx, buf, n) and return
// what write() and read() would.
int dos_api_sink (int w, int r, int ctx) {
  tap_drain();
  g_sink_w = w;
  g_sink_r = r;
  g_sink_ctx = ctx;
  g_sink_img = g_cur_img;
  tap_set();
  return 0;
}

// ---- the formatter ---------------------------------------------------------
// printf goes straight to the console, so DOS's own words -- the
// builtins, their errors, a batch file's echo -- are formatted here and
// go through conwrite, where redirection and a shell's sink catch them.
// pf takes a format and exactly three arguments (plain c4 has no varargs
// of its own): %d, %s, %c, %% and %.Nd, a number of at least N digits.
enum { PFMAX = 512 };
char *g_pfbuf;
int g_pfn;
char *g_pfdig;

int pf_flush () {
  int n;
  n = g_pfn;
  g_pfn = 0;
  if (n > 0) dos_api_conwrite(g_pfbuf, n);
  return 0;
}
int pf_put (int c) {
  if (g_pfn >= PFMAX) pf_flush();
  g_pfbuf[g_pfn] = c;
  ++g_pfn;
  return 0;
}
int pf_num (int v, int digits) {
  int n;
  if (v < 0) { pf_put('-'); v = 0 - v; }
  n = 0;
  g_pfdig[n] = '0' + v % 10; v = v / 10; ++n;
  while (v > 0) { g_pfdig[n] = '0' + v % 10; v = v / 10; ++n; }
  while (n < digits && n < 20) { g_pfdig[n] = '0'; ++n; }
  while (n > 0) { --n; pf_put(g_pfdig[n]); }
  return 0;
}
int pf (char *fmt, int a, int b, int c) {
  int k, v, digits;
  char *s;
  k = 0;
  while (*fmt) {
    if (*fmt != '%') { pf_put(*fmt); ++fmt; }
    else {
      ++fmt;
      digits = 0;
      if (*fmt == '.') {
        ++fmt;
        while (*fmt >= '0' && *fmt <= '9') { digits = digits * 10 + *fmt - '0'; ++fmt; }
      }
      if (*fmt == '%') pf_put('%');
      else if (*fmt) {
        v = k == 0 ? a : (k == 1 ? b : c);
        ++k;
        if (*fmt == 'd') pf_num(v, digits);
        else if (*fmt == 'c') pf_put(v);
        else if (*fmt == 's') {
          s = (char *)v;
          if (!s) s = "(null)";
          while (*s) { pf_put(*s); ++s; }
        }
      }
      if (*fmt) ++fmt;
    }
  }
  pf_flush();
  return 0;
}
// n bytes as they are
int pf_n (char *s, int n) {
  if (n > 0) { tap_drain(); out_put(s, n); }
  return 0;
}

// The image's code and data are allocated in generation `gen`, the
// program's own, so that releasing it takes them too; the scratch, which
// is DOS's, is not.
int c4r_load (char *path, int *api, int gen) {
  char *p, *data;
  int *code, *cons, *des;
  int fd, n, total, g;
  int entry, codelen, datalen, patchlen, symlen, conslen, deslen, memsz;
  int i, ptype, paddr, pvalu;

  if (!scratch_need()) return 0;
  if ((fd = dos_open(path)) < 0) return 0;
  total = 0;
  // bounded by the scratch: a file of any size can be named
  while (total < BUF_MAX && (n = dos_read(fd, g_scratch + total, BUF_MAX - total < 65536 ? BUF_MAX - total : 65536)) > 0)
    total = total + n;
  dos_close(fd);
  if (total < 13) { pf("c4dos: %s is not a program\n", (int)path, 0, 0); return 0; }

  p = g_scratch;
  if (!(p[0] == 'C' && p[1] == '4' && p[2] == 'R')) {
    pf("c4dos: bad signature in %s\n", (int)path, 0, 0);
    return 0;
  }
  if (p[4] / 8 != sizeof(int)) {
    pf("c4dos: %d-bit image, this machine is %d-bit\n", p[4], sizeof(int) * 8, 0);
    return 0;
  }
  memsz = 0;
  if (p[3] >= 3) memsz = wordat(p + 5);   // v3: data MEMSZ rides the padding
  p = p + 13;

  entry    = wordat(p); p = p + sizeof(int);
  codelen  = wordat(p); p = p + sizeof(int);
  datalen  = wordat(p); p = p + sizeof(int);
  patchlen = wordat(p); p = p + sizeof(int);
  symlen   = wordat(p); p = p + sizeof(int);
  conslen  = wordat(p); p = p + sizeof(int);
  deslen   = wordat(p); p = p + sizeof(int);
  if (memsz < datalen) memsz = datalen;

  // code: copy out of the shared scratch into an exact allocation
  p = p + sizeof(int);   // 'C' marker
  g = gen_get();
  gen_set(gen);
  code = malloc(codelen * sizeof(int));
  data = 0;
  if (code) data = malloc(memsz + sizeof(int));
  gen_set(g);
  if (!code) { printf("c4dos: out of memory\n"); return 0; }
  if (!data) { free(code); printf("c4dos: out of memory\n"); return 0; }
  i = 0;
  while (i < codelen) { code[i] = wordat(p + i * sizeof(int)); ++i; }
  p = p + codelen * sizeof(int);

  // data: MEMSZ, allocated above (BSS tail zeroed), copy DATALEN
  p = p + sizeof(int);   // 'D' marker
  memset(data, 0, memsz + sizeof(int));
  i = 0;
  while (i < datalen) { data[i] = p[i]; ++i; }
  p = p + datalen;

  // patches
  p = p + sizeof(int);   // 'P' marker
  i = 0;
  while (i < patchlen) {
    ptype = wordat(p); paddr = wordat(p + sizeof(int)); pvalu = wordat(p + 2 * sizeof(int));
    p = p + 3 * sizeof(int);
    if (ptype == -1) code[paddr] = (int)(code + pvalu);
    else if (ptype == -2) code[paddr] = (int)(data + pvalu);
    else if (ptype == -3) *(int *)(data + paddr) = (int)(code + pvalu);
    else if (ptype == -4) *(int *)(data + paddr) = (int)(data + pvalu);
    ++i;
  }

  // constructors, destructors, symbols
  p = p + sizeof(int);   // 'c' marker
  cons = (int *)p;
  p = p + conslen * sizeof(int);
  p = p + sizeof(int);   // 'd' marker
  des = (int *)p;
  p = p + deslen * sizeof(int);
  p = p + sizeof(int);   // 'S' marker
  inject_api(p, symlen, data, api);

  img_code = code;
  img_data = data;
  img_entry = (int)(code + entry);
  img_cons = cons;      // point into g_scratch: img_load copies them out
  img_ncons = conslen;
  img_des = des;
  img_ndes = deslen;
  return 1;
}

// The API table: [0] magic 'C4D', [1] dos_exit, [2..] the rest (see
// include/c4dos.h for the slots). Each is a code address: a transient
// built with include/c4dos.h reaches it through the invoke stub, one
// on a machine with indirect calls calls it directly.
int *g_api;

enum { C4DOS_REFUSE = 1127037010 };   // 'C','4','D','R'

// A constructor returning EXACTLY C4DOS_REFUSE aborts the program.
// It was the only way for a transient to decline: exit() is the EXIT
// opcode, which halts the whole machine, and g_api[1] (dos_exit) had no
// frame to return through before main ran. u0.h uses this to say "requires C4KE"
// instead of trap-storming through 38 opcode requests at a kernel that
// is not here; libc4ix.c says the same for C4IX.
//
// It has to be a DISTINGUISHED value rather than "non-zero". A c4cc
// constructor with no return statement returns whatever happened to be
// in the accumulator, and c4cc.c4r's does: treating non-zero as a
// refusal silently stopped the compiler from running, which showed up
// as BUILD printing "Compiling the kernel" and then producing no
// kernel at all.
//
// 1127037010 is 'C','4','D','R' -- "DOS, refuse" -- written out
// because plain c4 cannot fold that in an enum. Kept in step with
// include/c4dos.h, include/u0.h and src/c4ix/lib/libc4ix.c.
//
// An exit() in a constructor is a refusal too. Constructors run under a
// run frame like everything else, so it returns here rather than to
// whichever program loaded this one.

// ---- loaded images (API v4 load, start, unload) --------------------------
// RUN used to keep what it loaded in the img_* globals and the scratch,
// so a program could not run another. Now each load is a HANDLE, a block
// of words (the plain-c4 struct) that owns its code, data, its own copies
// of the constructor and destructor tables, and its heap generation.
// RUN is load + start + unload; slots 20-22 offer the three apart, so a
// shell can keep an image loaded after its main returns (grin's
// applications, docs/grin-plan.md 3.4).
enum { H_CODE, H_DATA, H_ENTRY, H_CONS, H_NCONS, H_DES, H_NDES, H_GEN,
       H_PARENT, H_RUN, H_WORDS };
enum { IMGMAX = 32 };

int *g_imgs;             // the live handles, 0 = a free entry
                         // (g_cur_img, above: the image whose code DOS last called into)
int g_refused;           // why the last img_load returned 0: a refusal

int img_find (int h) {
  int i;
  i = 0;
  while (i < IMGMAX) {
    if (g_imgs[i] == h) return i;
    ++i;
  }
  return 0 - 1;
}

// n words of a table that points into the scratch, in a block of their
// own (one word more, so an empty table is a block too); 0 when memory
// is gone
int *words_copy (int *src, int n) {
  int *d; int i;
  if (!(d = malloc((n + 1) * sizeof(int)))) return 0;
  i = 0;
  while (i < n) { d[i] = src[i]; ++i; }
  return d;
}

// Give everything back, generation and all. DOS's generation must be
// current: the handle and the tables are DOS's.
int img_free (int *p) {
  int i;
  free((char *)p[H_CODE]);
  free((char *)p[H_DATA]);
  if (p[H_CONS]) free((char *)p[H_CONS]);
  if (p[H_DES]) free((char *)p[H_DES]);
  gen_release(p[H_GEN]);
  if ((i = img_find((int)p)) >= 0) g_imgs[i] = 0;
  free((char *)p);
  return 0;
}

// Call one of the image's own functions (a constructor or destructor,
// at word offset `off` in its code) with the image's generation current.
int img_call (int *p, int off) {
  int old, oldimg, r;
  old = gen_get();
  oldimg = g_cur_img;
  gen_set(p[H_GEN]);
  g_cur_img = (int)p;
  ++p[H_RUN];
  r = frame_call((int)((int *)p[H_CODE] + off), 1, 0, 0);
  --p[H_RUN];
  g_cur_img = oldimg;
  gen_set(old);
  return r;
}

// Load an image and run its constructors. Returns a handle, or 0 (and
// g_refused set when a constructor declined).
int img_load (char *path) {
  int *p; int g, slot, i, r;
  g_refused = 0;
  g = gen_dos();
  p = 0;
  if ((slot = img_find(0)) < 0) pf("c4dos: %d programs loaded, no room for another\n", IMGMAX, 0, 0);
  else if (!(p = malloc(H_WORDS * sizeof(int)))) printf("c4dos: out of memory\n");
  if (p) {
    memset(p, 0, H_WORDS * sizeof(int));
    if (g_heapgen) p[H_GEN] = ++g_gen_next;
    if (!c4r_load(path, g_api, p[H_GEN])) {
      gen_release(p[H_GEN]);
      free((char *)p);
      p = 0;
    }
  }
  if (p) {
    p[H_CODE] = (int)img_code;
    p[H_DATA] = (int)img_data;
    p[H_ENTRY] = img_entry;
    p[H_NCONS] = img_ncons;
    p[H_NDES] = img_ndes;
    p[H_CONS] = (int)words_copy(img_cons, img_ncons);
    p[H_DES] = (int)words_copy(img_des, img_ndes);
    p[H_PARENT] = g_cur_img;
    g_imgs[slot] = (int)p;
    if (!p[H_CONS] || !p[H_DES]) {
      printf("c4dos: out of memory\n");
      img_free(p);
      p = 0;
    }
  }
  gen_set(g);
  if (!p) return 0;
  i = 0;
  while (i < p[H_NCONS]) {
    r = img_call(p, ((int *)p[H_CONS])[i]);
    if (r == C4DOS_REFUSE || g_last_exited) {
      g = gen_dos();
      img_free(p);
      gen_set(g);
      g_refused = 1;
      return 0;
    }
    ++i;
  }
  return (int)p;
}

// Call main(argc, argv) under a run frame, in the image's generation.
// Returns its status; the image stays loaded.
int img_start (int h, int argc, char **argv) {
  int *p; int old, oldimg, r;
  if (!h || img_find(h) < 0) return 0 - 1;
  p = (int *)h;
  old = gen_get();
  oldimg = g_cur_img;
  gen_set(p[H_GEN]);
  g_cur_img = h;
  ++p[H_RUN];
  r = frame_call(p[H_ENTRY], 2, argc, (int)argv);
  --p[H_RUN];
  g_cur_img = oldimg;
  gen_set(old);
  return r;
}

// Run the destructors, then free the image and release its generation.
// Images it loaded and left loaded go first. An image whose code is
// still running -- below us on the stack -- is refused (-1).
int img_unload (int h) {
  int *p; int i, g;
  if (!h || img_find(h) < 0) return 0 - 1;
  p = (int *)h;
  if (p[H_RUN]) return 0 - 1;
  i = 0;
  while (i < IMGMAX) {
    if (g_imgs[i] && ((int *)g_imgs[i])[H_PARENT] == h) img_unload(g_imgs[i]);
    ++i;
  }
  i = 0;
  while (i < p[H_NDES]) { img_call(p, ((int *)p[H_DES])[i]); ++i; }
  file_close_img(h);
  g = gen_dos();
  img_free(p);
  gen_set(g);
  return 0;
}

int run_program (char *path, int argc, char **argv) {
  int h, r;
  // Redirected, with no tap: a program that prints for itself (c4cc's
  // printf) still prints on the console. Said once, on the console,
  // where that output is going to appear.
  if (g_redir && !g_tap && !g_tap_said && (g_out_h >= 0 || g_sink_w || g_in_data)) {
    g_tap_said = 1;
    printf("c4dos: this machine has no console tap (REDIR.SYS TAP), so what a program prints for itself stays on the console\n");
  }
  if (!(h = img_load(path))) {
    if (g_refused) return C4DOS_REFUSE;
    return 0 - 1;
  }
  r = img_start(h, argc, argv);
  img_unload(h);
  return r;
}

// ---- files ---------------------------------------------------------------

// Pull ONE line from the console into g_line. Returns 0 on EOF.
int get_line () {
  int i, n, got;
  memset(g_line, 0, LINEMAX);
  got = 0;
  while (!got) {
    // a complete line already buffered?
    i = g_inpos;
    while (i < g_inlen && g_inbuf[i] != 10) ++i;
    if (i < g_inlen) {
      n = i - g_inpos;
      if (n >= LINEMAX) n = LINEMAX - 1;
      i = 0;   // no memcpy on plain c4 (MCPY is c4m's)
      while (i < n) { g_line[i] = g_inbuf[g_inpos + i]; ++i; }
      if (n > 0 && g_line[n - 1] == 13) g_line[n - 1] = 0;   // CRLF consoles
      g_inpos = g_inpos + n + 1;
      got = 1;
    } else {
      // compact and refill
      n = g_inlen - g_inpos;
      i = 0;
      while (i < n) { g_inbuf[i] = g_inbuf[g_inpos + i]; ++i; }
      g_inlen = n;
      g_inpos = 0;
      n = read(0, g_inbuf + g_inlen, INBUFMAX - g_inlen);
      if (n <= 0) {
        // EOF: hand back a final partial line if one is buffered
        if (!g_inlen) return 0;
        n = g_inlen;
        if (n >= LINEMAX) n = LINEMAX - 1;
        i = 0;
        while (i < n) { g_line[i] = g_inbuf[i]; ++i; }
        g_inlen = 0;
        got = 1;
      }
      else g_inlen = g_inlen + n;
    }
  }
  return 1;
}

// Wait for the user before the next screenful: one line of input,
// through conread, so that a DOS Prompt window's Enter answers it and a
// piped script's next line does (con_get, like the prompt's reader,
// knows that one read(0, ...) may carry several lines, and takes ONE).
// EOF stops the paging rather than the file -- a script that piped no
// answer still wants the whole thing typed.
char *g_morebuf;
int type_more () {
  pf(g_conflush || g_sink_w ? "-- More --" : "-- More --\n", 0, 0, 0);
  if (dos_api_conread(g_morebuf, LINEMAX) <= 0) return 0;
  return 1;
}

// print a file to the console; returns 1 if it existed.
// page = lines per screenful, 0 for no paging (and none into a file).
// A buffer of its own rather than the scratch: a pause waits in a DOS
// Prompt window's sink, where grin may load a program meanwhile.
int type_file (char *path, int page) {
  int fd, n, i, start, lines, g;
  char *buf;
  if ((fd = dos_open(path)) < 0) return 0;
  g = gen_dos();
  buf = malloc(4096);
  gen_set(g);
  if (!buf) { dos_close(fd); printf("c4dos: out of memory\n"); return 1; }
  if (g_out_h >= 0) page = 0;
  lines = 0;
  while ((n = dos_read(fd, buf, 4096)) > 0) {
    i = 0;
    start = 0;
    while (i < n) {
      if (page && buf[i] == 10) {
        ++lines;
        if (lines >= page) {
          pf_n(buf + start, i + 1 - start);
          start = i + 1;
          lines = 0;
          if (!type_more()) page = 0;   // EOF: type the rest straight out
        }
      }
      ++i;
    }
    pf_n(buf + start, n - start);
  }
  dos_close(fd);
  free(buf);
  return 1;
}


// ---- builtins ------------------------------------------------------------

int cmd_ver () {
  pf("C4DOS version %d.%d\n", VER_MAJOR, VER_MINOR, 0);
  pf("the operating system before operating systems\n", 0, 0, 0);
  return 0;
}

// The raw disk has no directory service (READ is all the hardware
// gives us), so DIR reads the disk's manifest file - the same move
// the c4bb web app makes with manifest.json. An honest limitation:
// early machines listed what the label said.
// a pipe's temporary file (RAM:PIPEn.$$$), which DIR leaves out: it is
// the pipe's, not the person's, and DIR | x would list it while it is
// being written
int is_pipe_file (char *s) {
  int n;
  n = xlen(s);
  return n > 4 && s[n - 4] == '.' && s[n - 3] == '$' && s[n - 2] == '$' && s[n - 1] == '$';
}

int cmd_dir (char *drive, int page) {
  int i, n;
  // DIR B: lists B:'s c4dos.dir, and only that
  if (drive && g_drives && (n = drv_num(drive)) >= 0) {
    if (n >= drv_count()) { pf("no drive %c: - this machine has %d\n", drv_letter(n), drv_count(), 0); return 1; }
    if (!type_file(dir_name(n), page)) pf("no c4dos.dir on %c: - nothing has been written there\n", drv_letter(n), 0, 0);
    return 0;
  }
  // the prompt's drive's listing, by name: a plain "c4dos.dir" would be
  // the home drive's (dos_open looks there first)
  if (!type_file(g_drives ? dir_name(g_drive) : "c4dos.dir", page))
    pf("no c4dos.dir on this disk - the raw disk has no directory service\n", 0, 0, 0);
  if (g_ramdisk) {
    i = 0;
    n = 0;
    while (i < g_ram_n) {
      if (is_pipe_file((char *)g_ram_name[i])) ++n;
      else pf("%s  <ram> %d bytes\n", g_ram_name[i], g_ram_len[i], 0);
      ++i;
    }
    pf("ramdisk: %d of %d bytes used, %d file(s)\n",
           g_ram_used, g_ram_budget, g_ram_n - n);
  }
  return 0;
}

// How many lines to a screenful when /P is given. 23 leaves room for
// the "-- More --" and the command that follows on a 25-line console.
enum { PAGELINES = 23 };

// Is /P (or -P) among the arguments? Returns the page size, or 0.
// A switch, not a value: DOS spelled it /P and never asked how big the
// screen was.
int page_switch (int argc, char **argv) {
  int i;
  i = 1;
  while (i < argc) {
    if (cieq(argv[i], "/P") || cieq(argv[i], "-P")) return PAGELINES;
    ++i;
  }
  return 0;
}

// The first argument that is not a switch.
char *first_operand (int argc, char **argv) {
  int i;
  i = 1;
  while (i < argc) {
    if (*argv[i] != '/' && *argv[i] != '-') return argv[i];
    ++i;
  }
  return 0;
}

int cmd_type (int argc, char **argv) {
  char *path;
  path = first_operand(argc, argv);
  if (!path || !*path) { pf("usage: TYPE file [/P]\n", 0, 0, 0); return 1; }
  if (!type_file(path, page_switch(argc, argv))) {
    pf("file not found: %s\n", (int)path, 0, 0);
    return 1;
  }
  return 0;
}

// v3 slot 15: the clock, for a transient that cannot reach one itself.
//
// __time() is the TIME opcode, and that is the whole problem it solves:
// TIME is opcode 53, above EXIT, so a program that calls it directly
// leaves the base rung -- and c4m-dos32.c4r is pinned at base in
// RUNG_BASE precisely so the campaign can say "the VM needs no opcode
// the machine did not boot with". Reaching the clock THROUGH this table
// costs the caller nothing above EXIT (the invoke stub is base-c4), so
// a transient gets a working clock without moving up a rung.
//
// Only advertised when CLOCK.SYS was installed AND this build has the
// hardware compiled in -- the same honesty rule the RAMDISK slots keep.
#if C4DOS_CLOCK
int dos_api_time () { return __time(); }
#endif

int cmd_time () {
#if C4DOS_CLOCK
  int ms, s, m, h;
  if (!g_clock) {
    pf("no clock device - add DEVICE=CLOCK.SYS to CONFIG.SYS\n", 0, 0, 0);
    return 1;
  }
  ms = __time();
  s = ms / 1000; ms = ms % 1000;
  m = s / 60; s = s % 60;
  h = m / 60; m = m % 60;
  pf("uptime %d:%.2d:%.2d", h, m, s);
  pf(".%.3d\n", ms, 0, 0);
  return 0;
#else
  pf("this build has no clock hardware support\n", 0, 0, 0);
  return 1;
#endif
}

int cmd_mem () {
  int used, total, i, n;
  // no meminfo primitive; report what DOS knows it holds
  pf("resident: C4DOS shell, %d byte scratch, %d byte line buffer\n",
         BUF_MAX, LINEMAX, 0);
  // ...unless HEAP.SYS said where the heap's books are. The machine
  // that has the generation registers has these three beside them.
  if (g_heapgen) {
    used = *(int *)BB_HEAP_USED;
    total = *(int *)BB_HEAP_END - *(int *)BB_HEAP_BASE;
    n = 0;
    i = 0;
    while (i < IMGMAX) { if (g_imgs[i]) ++n; ++i; }
    pf("heap: %d bytes free of %d, %d in use\n", total - used, total, used);
    pf("programs loaded: %d\n", n, 0, 0);
  }
  return 0;
}

// ---- command dispatch ----------------------------------------------------

// split g_line into g_argv, in place; returns argc
int parse_line () {
  char *p; int argc;
  p = g_line;
  g_linelen = xlen(g_line);
  argc = 0;
  while (*p && argc < ARGVMAX - 1) {
    while (*p == ' ' || *p == 9) ++p;
    if (*p) {
      g_argv[argc++] = p;
      while (*p && *p != ' ' && *p != 9) ++p;
      if (*p) { *p = 0; ++p; }
    }
  }
  g_argv[argc] = 0;
  return argc;
}

// does the name look like a program? (ends .c4r, any case)
int is_program_name (char *s) {
  int n;
  n = xlen(s);
  if (n < 5) return 0;
  return s[n - 4] == '.' && upper(s[n - 3]) == 'C' && s[n - 2] == '4'
      && upper(s[n - 1]) == 'R';
}

// ---- the transient API (include/c4dos.h is the other half) ---------------
// A transient reaches these by address, out of the table the loader
// wrote into its __c4dos_api global. They are ordinary functions; the
// only unusual thing is who calls them.
// The two that allocate do it in DOS's generation (HEAP.SYS): the file
// is DOS's, and outlives the program that wrote it.
int dos_api_create (char *name) {
  int g, r;
  g = gen_dos();
  r = file_create(name);
  gen_set(g);
  return r;
}
int dos_api_write (int h, char *buf, int len) {
  int g, r;
  g = gen_dos();
  r = file_write(h, buf, len);
  gen_set(g);
  return r;
}
int dos_api_close (int h) { return file_close(h); }
// The read half: DOS's own opener, which checks RAM before disk, made
// available to a transient whose own open() only ever sees the host.
int dos_api_open (char *name) { return dos_open(name); }
int dos_api_read (int fd, char *buf, int len) { return dos_read(fd, buf, len); }
int dos_api_rclose (int fd) { return dos_close(fd); }

// ---- API v4: running programs, and deleting files -------------------------
// Slots 19-23 are what a shell needs to be one without a loader of its
// own (docs/grin-plan.md 3.2). exec is further down, after the batch
// runner it may need.
int dos_api_unlink (char *name) { return file_unlink(name); }
// v5: where files go, and how many drives there are (grin's File Manager)
int dos_api_home () { return g_home; }
// v6: the drive the prompt is on, 0 = A: (a DOS Prompt window's prompt)
int dos_api_drive () { return g_drive; }
int dos_api_drives () { return drv_count(); }
int dos_api_load (char *path) { return img_load(path); }
int dos_api_start (int h, int argc, char **argv) { return img_start(h, argc, argv); }
int dos_api_unload (int h) { return img_unload(h); }
// fn(arg) under a run frame: an exit() inside it comes back here, as
// its status. *exited (when exited is not 0) says whether it did. It
// runs in the caller's generation; a shell calling into an image it
// loaded switches to that image's first (26 and 27).
int dos_api_catch (int fn, int arg, int *exited) {
  int r;
  r = frame_call(fn, 1, arg, 0);
  if (exited) *exited = g_last_exited;
  return r;
}
// Generations for a shell, which runs code of several images and has to
// say whose memory a call allocates (docs/grin-plan.md 3.3). gen(h) is
// image h's generation, or with h = 0 that of the image DOS last
// started, which is the caller when it asks from its own main; 0 when
// HEAP.SYS is not in. setgen(g) makes g current and returns the
// previous one; -1 when HEAP.SYS is not in.
int dos_api_gen (int h) {
  if (!g_heapgen) return 0;
  if (!h) h = g_cur_img;
  if (!h || img_find(h) < 0) return 0;
  return ((int *)h)[H_GEN];
}
int dos_api_setgen (int g) {
  int old;
  if (!g_heapgen) return 0 - 1;
  old = gen_get();
  gen_set(g);
  return old;
}
// v7: the image DOS counts as running -- the parent of what it loads, the
// owner of the files it opens for writing, gen(0)'s answer -- for a
// shell whose code a child calls directly, not through DOS. grin's game
// window is one: the game calls grin's poll, which runs grin's loop, and
// an application started from there is grin's child, not the game's,
// as a sink's call makes it (sink_call). h = 0 only asks; a handle that
// is not loaded changes nothing. Returns the image that was running.
int dos_api_image (int h) {
  int old;
  old = g_cur_img;
  if (h && img_find(h) >= 0) g_cur_img = h;
  return old;
}

// ---- API v2: enumeration, and handing memory back ------------------------
// v1 could create, write, open and read a file BY NAME. That is enough
// for a tool, and not enough for a loader: a kernel taking over the
// machine wants everything the RAM disk holds without being told what
// is on it, the way a boot loader hands an initrd over whole. These
// four say what is there, and the last two say "you can have the
// memory now".
int dos_api_count () { if (!g_ramdisk) return 0; return g_ram_n; }
int dos_api_entname (int i) {
  if (!g_ramdisk || i < 0 || i >= g_ram_n) return 0;
  return g_ram_name[i];
}
int dos_api_entsize (int i) {
  if (!g_ramdisk || i < 0 || i >= g_ram_n) return 0 - 1;
  return g_ram_len[i];
}
int dos_api_entdata (int i) {
  if (!g_ramdisk || i < 0 || i >= g_ram_n) return 0;
  return g_ram_data[i];
}

// Give back the 4MB read scratch. Every DOS routine that wants it
// calls scratch_need(), so this is safe at any moment; the next TYPE
// or RUN simply pays for one malloc. Returns the bytes released.
int dos_api_trim () {
  if (!g_scratch) return 0;
  free(g_scratch);
  g_scratch = 0;
  return BUF_MAX;
}

// Give back the RAM disk contents. The caller has copied out whatever
// it wanted (ENTDATA pointers are dead after this). The slot table
// itself stays -- the disk is empty, not uninstalled -- so a transient
// that returns to the prompt finds a working, bare RAM disk rather
// than a broken one. Returns the bytes released.
int dos_api_release () {
  int i, freed;
  if (!g_ramdisk) return 0;
  freed = 0;
  i = 0;
  while (i < g_ram_n) {
    if (g_ram_data[i]) { freed = freed + g_ram_cap[i]; free((char *)g_ram_data[i]); }
    if (g_ram_name[i]) free((char *)g_ram_name[i]);
    g_ram_data[i] = 0; g_ram_name[i] = 0; g_ram_len[i] = 0; g_ram_cap[i] = 0;
    ++i;
  }
  g_ram_n = 0;
  g_ram_used = 0;
  i = 0;
  while (i < RAMHANDLE) { g_rh_slot[i] = 0 - 1; ++i; }
  return freed;
}

// COPY src dst -- the one builtin that WRITES, and the demonstration
// that the RAM disk works. '>' came later, and only with REDIR.SYS
// (docs/c4dos-design.md "Redirection"); without it a tool that wants to
// produce a file does it directly, and this is the smallest thing that does. dst
// goes where create puts it (file_create): B:NAME onto B:, a plain name
// onto the home drive or the RAM disk. A bare drive, "COPY X B:", keeps
// the name.
char *g_copybuf;         // a bare drive and the source's name
int cmd_copy (char *src, char *dst) {
  int fd, n, total, h, i, k;
  if (!src || !dst || !*src || !*dst) { pf("usage: COPY src dst\n", 0, 0, 0); return 1; }
  if (!g_ramdisk && g_home < 0 && drv_prefix(dst) < 0) { pf("no ramdisk - add DEVICE=RAMDISK.SYS to CONFIG.SYS\n", 0, 0, 0); return 1; }
  if (drv_num(dst) >= 0 || cieq(dst, "RAM:")) {
    k = 0;
    while (dst[k]) { g_copybuf[k] = dst[k]; ++k; }
    i = xlen(src);
    while (i > 0 && src[i - 1] != ':' && src[i - 1] != '/') --i;
    while (src[i] && k < LINEMAX - 1) { g_copybuf[k] = src[i]; ++k; ++i; }
    g_copybuf[k] = 0;
    dst = g_copybuf;
  }
  if (!scratch_need()) return 1;
  if ((fd = dos_open(src)) < 0) { pf("file not found: %s\n", (int)src, 0, 0); return 1; }
  total = 0;
  while (total < BUF_MAX && (n = dos_read(fd, g_scratch + total, BUF_MAX - total < 4096 ? BUF_MAX - total : 4096)) > 0)
    total = total + n;
  dos_close(fd);
  if ((h = file_create(dst)) < 0) { pf("cannot write %s\n", (int)dst, 0, 0); return 1; }
  if (total > 0 && file_write(h, g_scratch, total) != total) { file_close(h); pf("cannot write %s\n", (int)dst, 0, 0); return 1; }
  if (file_close(h) < 0) { pf("cannot write %s\n", (int)dst, 0, 0); return 1; }
  pf("%s -> %s (%d bytes)\n", (int)src, (int)dst, total);
  return 0;
}

// A bare command with ".c4r" appended, if that names something that
// opens. This is the courtesy COMMAND.COM extended with COM/EXE/BAT:
// you type the program, not the file. Returns 0 when there is no such
// program, so an unknown word still reaches "bad command or file name"
// rather than being reported as a broken executable.
char *prog_ext (char *name) {
  char *q, *s;
  int fd;
  if (xlen(name) + 5 > LINEMAX) return 0;
  q = g_progbuf;
  s = name;
  while (*s) { *q = *s; ++q; ++s; }
  *q = '.'; ++q; *q = 'c'; ++q; *q = '4'; ++q; *q = 'r'; ++q; *q = 0;
  if ((fd = dos_open(g_progbuf)) < 0) return 0;
  dos_close(fd);
  return g_progbuf;
}

// Does this name a batch file? As typed if it already ends .bat, else
// with .bat appended -- the same courtesy prog_ext extends to .c4r.
int is_bat_name (char *s) {
  int n;
  n = xlen(s);
  if (n < 5) return 0;
  return s[n - 4] == '.' && upper(s[n - 3]) == 'B' && upper(s[n - 2]) == 'A'
      && upper(s[n - 1]) == 'T';
}

char *bat_path (char *name) {
  char *q, *s;
  int fd;
  if (xlen(name) + 5 > LINEMAX) return 0;
  q = g_batbuf;
  s = name;
  while (*s) { *q = *s; ++q; ++s; }
  if (!is_bat_name(name)) { *q = '.'; ++q; *q = 'b'; ++q; *q = 'a'; ++q; *q = 't'; ++q; }
  *q = 0;
  if ((fd = dos_open(g_batbuf)) < 0) return 0;
  dos_close(fd);
  return g_batbuf;
}

// RUN's argument: as typed if it opens, else with ".c4r" appended.
char *prog_path (char *name) {
  int fd;
  if ((fd = dos_open(name)) >= 0) { dos_close(fd); return name; }
  return prog_ext(name);
}

int dispatch (int argc) {
  char *cmd, *path; int r, i;
  if (!argc) return 0;
  cmd = g_argv[0];
  if (cieq(cmd, "REM")) return 0;
  if (cieq(cmd, "ECHO")) {
    if (argc > 1 && cieq(g_argv[1], "OFF")) { g_echo = 0; return 0; }
    if (argc > 1 && cieq(g_argv[1], "ON")) { g_echo = 1; return 0; }
    // The rest of the line as it was typed, however many words (argv
    // stops at ARGVMAX - 1 of them): parse_line put a NUL after each
    // word it took, and those go back to being the spaces they were.
    if (argc < 2) { pf("\n", 0, 0, 0); return 0; }
    path = g_argv[1];
    r = 0;
    while (path + r < g_line + g_linelen) { if (!path[r]) path[r] = ' '; ++r; }
    pf("%s\n", (int)path, 0, 0);
    return 0;
  }
  if (cieq(cmd, "VER")) return cmd_ver();
  if (cieq(cmd, "DIR")) return cmd_dir(first_operand(argc, g_argv), page_switch(argc, g_argv));
  if (cieq(cmd, "TYPE")) return cmd_type(argc, g_argv);
  if (cieq(cmd, "COPY"))
    return cmd_copy(argc > 1 ? g_argv[1] : 0, argc > 2 ? g_argv[2] : 0);
  // DEL name: off the RAM disk if it is there, else off the home drive;
  // B:name off drive B: (file_unlink)
  if (cieq(cmd, "DEL") || cieq(cmd, "ERASE")) {
    if (argc < 2) { pf("usage: DEL file\n", 0, 0, 0); return 1; }
    if (file_unlink(g_argv[1]) < 0) { pf("cannot delete %s\n", (int)g_argv[1], 0, 0); return 1; }
    return 0;
  }
  if (cieq(cmd, "TIME")) return cmd_time();
  if (cieq(cmd, "MEM")) return cmd_mem();
  if (cieq(cmd, "EXIT")) { g_quit = 1; return 0; }
  if (cieq(cmd, "RUN")) {
    if (argc < 2) { pf("usage: RUN program[.c4r] [args]\n", 0, 0, 0); return 1; }
    if (!(path = prog_path(g_argv[1]))) {
      pf("file not found: %s\n", (int)g_argv[1], 0, 0);
      return 1;
    }
    return run_program(path, argc - 1, g_argv + 1);
  }
  if (cieq(cmd, "RAM:")) {
    pf("RAM: is not a drive - its files sit over every drive, ahead of the disk's\n", 0, 0, 0);
    pf("use the plain name (TYPE c4ke.i) or RAM:name for the RAM copy only\n", 0, 0, 0);
    return 1;
  }
  // A bare "B:" changes drive, the way it always has in a DOS. Checked
  // before the program-name fall-through so it can never be mistaken
  // for a file, and it is the only command whose whole spelling is its
  // argument.
  if ((i = drv_num(cmd)) >= 0) {
    if (!g_drives) {
      pf("no drive device - add DEVICE=DRIVES.SYS to CONFIG.SYS\n", 0, 0, 0);
      return 1;
    }
    if (i >= drv_count()) {
      pf("no drive %c: - this machine has %d\n", drv_letter(i), drv_count(), 0);
      return 1;
    }
    g_drive = i;
    drv_select(i);
    return 0;
  }
  if (is_program_name(cmd)) return run_program(cmd, argc, g_argv);
  // Not a builtin and no extension typed: try it as a program name.
  if ((path = prog_ext(cmd))) return run_program(path, argc, g_argv);
  // Then as a batch file. dispatch cannot run one itself -- run_batch
  // is defined further down and this dialect is define-before-use, so
  // the batch is handed back and whoever called dispatch runs it.
  if ((path = bat_path(cmd))) { g_batreq = path; return 0; }
  pf("bad command or file name: %s\n", (int)g_argv[0], 0, 0);
  return 1;
}

// ---- CONFIG.SYS and batch ------------------------------------------------

int g_linefn;            // &line_run, for run_batch

// run a file of commands, one per line. '@' prefixes suppress echo.
// Returns 0 if the file existed.
int run_batch (char *path, int depth) {
  char *buf, *p, *e; int fd, n, total, show;
  if (depth > BATDEPTH) { pf("batch nested too deep\n", 0, 0, 0); return 1; }
  if (!scratch_need()) return 1;
  if ((fd = dos_open(path)) < 0) return 1;
  total = 0;
  while (total < BUF_MAX && (n = dos_read(fd, g_scratch + total, BUF_MAX - total < 4096 ? BUF_MAX - total : 4096)) > 0)
    total = total + n;
  dos_close(fd);
  if (!(buf = malloc(total + 1))) { printf("c4dos: out of memory\n"); return 1; }
  n = 0;
  while (n < total) { buf[n] = g_scratch[n]; ++n; }
  buf[total] = 0;

  p = buf;
  while (*p && !g_quit) {
    e = p;
    while (*e && *e != 10) ++e;
    n = e - p;
    if (n > 0 && p[n - 1] == 13) --n;    // tolerate CRLF batches
    if (n >= LINEMAX) n = LINEMAX - 1;
    memset(g_line, 0, LINEMAX);
    show = g_echo;
    if (n > 0 && *p == '@') {
      show = 0;
      ++p; --n;
    }
    total = 0;
    while (total < n) { g_line[total] = p[total]; ++total; }
    if (show && g_line[0]) pf("%c>%s\n", drv_letter(g_drive), (int)g_line, 0);
    // line_run, defined below (it runs batch files, so it comes after
    // this), reached the way a transient reaches DOS: through the stub
    invoke1((int *)g_linefn, depth);
    p = *e ? e + 1 : e;
  }
  free(buf);
  return 0;
}

// ---- one command line, and redirection (DEVICE=REDIR.SYS) -----------------
//
// The prompt, a batch file's lines and exec (slot 19) all run a command
// line through line_run. Without REDIR.SYS that is dispatch and nothing
// else, exactly as before the line existed: the 2026-08-21 decision, kept
// as the default (docs/c4dos-design.md "Redirection"). With it:
//
//   cmd > file    cmd >> file    cmd < file    a | b | c
//
// A redirect target is any file create can make: "RAM:NAME", "B:NAME" on
// a drive that can be written, a plain name on the home drive or the RAM
// disk. A pipe runs a, then b, through a temporary RAM-disk file, as
// MS-DOS's did. What is redirected: DOS's own output (pf), a bare.c
// program's (slots 16 and 17), and with the tap a c4cc program's PRTF and
// fd-0 READ.

// a command, and a batch file it named; its status
int cmd_run (int depth) {
  int r;
  r = dispatch(parse_line());
  if (g_batreq) { g_batreq = 0; r = run_batch(g_batbuf, depth + 1); }
  return r;
}

int has_redir (char *s) {
  while (*s) { if (*s == '>' || *s == '<' || *s == '|') return 1; ++s; }
  return 0;
}

// A whole file, for <, in a block of DOS's own; g_rx_len its length.
// 0 if it does not open.
int g_rx_len;
char *redir_in (char *name) {
  int fd, n, total, g;
  char *d;
  if (!scratch_need()) return 0;
  if ((fd = dos_open(name)) < 0) return 0;
  total = 0;
  while (total < BUF_MAX && (n = dos_read(fd, g_scratch + total, BUF_MAX - total < 4096 ? BUF_MAX - total : 4096)) > 0)
    total = total + n;
  dos_close(fd);
  g = gen_dos();
  d = malloc(total + 1);
  gen_set(g);
  if (!d) { printf("c4dos: out of memory\n"); return 0; }
  n = 0;
  while (n < total) { d[n] = g_scratch[n]; ++n; }
  d[total] = 0;
  g_rx_len = total;
  return d;
}

// A file for > (made afresh) or >> (what it held, then what comes): a
// write handle, or -1.
int redir_out (char *name, int append) {
  int fd, n, total, h, g;
  total = 0;
  if (append) {
    if (!scratch_need()) return 0 - 1;
    if ((fd = dos_open(name)) >= 0) {
      while (total < BUF_MAX && (n = dos_read(fd, g_scratch + total, BUF_MAX - total < 4096 ? BUF_MAX - total : 4096)) > 0)
        total = total + n;
      dos_close(fd);
    }
  }
  g = gen_dos();
  h = file_create(name);
  if (h >= 0 && total > 0) file_write(h, g_scratch, total);
  gen_set(g);
  return h;
}

// Take the redirections out of segment s, in place: what is left is the
// command. A name goes into out (>, >>) or in (<), each LINEMAX long;
// g_rx_out, g_rx_app and g_rx_in say which were given. -1 when a > or <
// has no name after it.
int g_rx_out; int g_rx_app; int g_rx_in;
int seg_parse (char *s, char *out, char *in) {
  char *p, *q, *d;
  int kind, k;
  g_rx_out = 0; g_rx_app = 0; g_rx_in = 0;
  p = s;
  q = s;
  while (*p) {
    if (*p == '>' || *p == '<') {
      kind = *p;
      ++p;
      if (kind == '>') {
        g_rx_out = 1;
        g_rx_app = 0;
        if (*p == '>') { g_rx_app = 1; ++p; }
        d = out;
      } else { g_rx_in = 1; d = in; }
      while (*p == ' ' || *p == 9) ++p;
      k = 0;
      while (*p && *p != ' ' && *p != 9 && *p != '>' && *p != '<') {
        if (k < LINEMAX - 1) { d[k] = *p; ++k; }
        ++p;
      }
      d[k] = 0;
      if (!k) { pf("c4dos: no file name after %c\n", kind, 0, 0); return 0 - 1; }
    } else { *q = *p; ++q; ++p; }
  }
  // "ECHO hi > x" puts "hi" in x, not "hi " (MS-DOS kept the space)
  while (q > s && (*(q - 1) == ' ' || *(q - 1) == 9)) --q;
  *q = 0;
  return 0;
}

// Run cmd with its input from `in` (0: as it is) and its output to `out`
// (0: as it is). The outer level's state is kept in locals and put back.
int run_seg (char *cmd, char *in, char *out, int append, int depth) {
  int h, r, n, oh, olen, opos, g;
  char *data, *odata;
  h = 0 - 1;
  data = 0;
  n = 0;
  if (out && (h = redir_out(out, append)) < 0) { pf("cannot write %s\n", (int)out, 0, 0); return 1; }
  if (in && !(data = redir_in(in))) {
    if (h >= 0) file_close(h);
    pf("file not found: %s\n", (int)in, 0, 0);
    return 1;
  }
  if (data) n = g_rx_len;
  tap_drain();                       // what the outer level caught is the outer level's
  oh = g_out_h; odata = g_in_data; olen = g_in_len; opos = g_in_pos;
  if (h >= 0) g_out_h = h;
  if (data) { g_in_data = data; g_in_len = n; g_in_pos = 0; }
  tap_set();
  r = 0;
  while (cmd[r] && r < LINEMAX - 1) { g_line[r] = cmd[r]; ++r; }
  g_line[r] = 0;
  r = cmd_run(depth);
  tap_drain();                       // ... and this level's is this level's
  g_out_h = oh; g_in_data = odata; g_in_len = olen; g_in_pos = opos;
  tap_set();
  if (h >= 0 && file_close(h) < 0) pf("cannot write %s\n", (int)out, 0, 0);
  g = gen_dos();
  if (data) free(data);
  gen_set(g);
  return r;
}

// "PIPEn.$$$" on the RAM disk, n counting up, into d
int g_pipe_n;
char *pipe_name (char *d) {
  int n, k;
  char *s;
  s = "RAM:PIPE";
  k = 0;
  while (*s) { d[k] = *s; ++k; ++s; }
  n = ++g_pipe_n;
  if (n >= 1000) { g_pipe_n = 0; n = 0; }
  if (n >= 100) { d[k] = '0' + n / 100; ++k; }
  if (n >= 10) { d[k] = '0' + (n / 10) % 10; ++k; }
  d[k] = '0' + n % 10; ++k;
  s = ".$$$";
  while (*s) { d[k] = *s; ++k; ++s; }
  d[k] = 0;
  return d;
}

// A line with redirection in it, or any line under a shell's sink when
// the tap is in (so that a c4cc program's output reaches the sink): each
// segment between |s run in turn, the pipe files deleted afterwards.
int redir_run (int depth) {
  char *buf, *seg, *e, *oname, *iname, *pa, *pb, *t, *in, *out;
  int g, r, n, i, append, piped;
  g = gen_dos();
  buf = malloc(LINEMAX * 5);
  gen_set(g);
  if (!buf) { printf("c4dos: out of memory\n"); return 1; }
  oname = buf + LINEMAX; iname = buf + LINEMAX * 2;
  pa = buf + LINEMAX * 3; pb = buf + LINEMAX * 4;
  pa[0] = 0; pb[0] = 0;
  r = 0;
  while (g_line[r] && r < LINEMAX - 1) { buf[r] = g_line[r]; ++r; }
  buf[r] = 0;
  n = 1;
  r = 0;
  while (buf[r]) { if (buf[r] == '|') ++n; ++r; }
  if (n > 1 && !g_ramdisk) {
    pf("a pipe needs the RAM disk - add DEVICE=RAMDISK.SYS to CONFIG.SYS\n", 0, 0, 0);
    free(buf);
    return 1;
  }
  r = 0;
  i = 0;
  seg = buf;
  while (i < n && !g_quit) {
    e = seg;
    while (*e && *e != '|') ++e;
    if (*e) { *e = 0; ++e; }
    if (seg_parse(seg, oname, iname) < 0) { r = 1; i = n; }
    else {
      in = 0; out = 0; append = 0;
      piped = 0;
      if (g_rx_in) in = iname;
      else if (i > 0 && pa[0]) in = pa;   // the last segment's output
      if (g_rx_out) { out = oname; append = g_rx_app; }
      else if (i < n - 1) { out = pipe_name(pb); piped = 1; }
      r = run_seg(seg, in, out, append, depth);
      if (pa[0]) { file_unlink(pa); pa[0] = 0; }
      if (piped) { t = pa; pa = pb; pb = t; }
      seg = e;
      ++i;
    }
  }
  if (pa[0]) file_unlink(pa);
  free(buf);
  return r;
}

// One command line, in g_line: through redirection when REDIR.SYS is in
// and the line asks for it (or a sink is set and the tap is in), else
// dispatch as it always was. Its status.
int line_run (int depth) {
  if (g_redir && (has_redir(g_line) || (g_tap && g_sink_w))) return redir_run(depth);
  return cmd_run(depth);
}

// v4 slot 19: one command line, exactly as the prompt would run it --
// builtins, a .c4r, a batch file -- from inside a program. Returns its
// status. The line is parsed into a buffer of its own, so the caller's
// own command line (its argv points into it) is untouched, and the
// prompt's is put back afterwards. EXIT inside it ends the exec, not
// DOS: a caller that wants EXIT to mean something checks the line
// itself.
int dos_api_exec (char *cmd) {
  char *oline, *line;
  char **oargv, **argv;
  int g, r, n, oquit;
  g = gen_dos();
  line = malloc(LINEMAX);
  argv = (char **)malloc(ARGVMAX * sizeof(char *));
  if (!line || !argv) {
    printf("c4dos: out of memory\n");
    if (line) free(line);
    if (argv) free((char *)argv);
    gen_set(g);
    return 0 - 1;
  }
  n = 0;
  while (cmd[n] && n < LINEMAX - 1) { line[n] = cmd[n]; ++n; }
  line[n] = 0;
  oline = g_line;
  oargv = g_argv;
  oquit = g_quit;
  g_line = line;
  g_argv = argv;
  g_quit = 0;
  r = line_run(0);
  g_line = oline;
  g_argv = oargv;
  g_quit = oquit;
  free(line);
  free((char *)argv);
  gen_set(g);
  return r;
}

int read_config () {
  char *buf, *p, *e; int fd, n, total;
  if (!scratch_need()) return 0;
  if ((fd = dos_open("config.sys")) < 0) return 0;
  total = 0;
  while (total < BUF_MAX && (n = dos_read(fd, g_scratch + total, BUF_MAX - total < 4096 ? BUF_MAX - total : 4096)) > 0)
    total = total + n;
  dos_close(fd);
  if (!(buf = malloc(total + 1))) return 0;
  n = 0;
  while (n < total) { buf[n] = g_scratch[n]; ++n; }
  buf[total] = 0;

  p = buf;
  while (*p) {
    e = p;
    while (*e && *e != 10) ++e;
    if (starts_ci(p, "DEVICE=CLOCK.SYS")) {
#if C4DOS_CLOCK
      g_clock = 1;
      printf("clock device installed\n");
#else
      printf("DEVICE=CLOCK.SYS: this build has no clock hardware support\n");
#endif
    }
    if (starts_ci(p, "DEVICE=RAMDISK.SYS")) {
      g_ramdisk = 1;
      g_ram_budget = RAMSIZE;
      n = 0;
      while (p + n < e && !starts_ci(p + n, "SIZE=")) ++n;
      if (p + n < e) {
        total = 0;
        n = n + 5;
        while (p + n < e && p[n] >= '0' && p[n] <= '9') {
          total = total * 10 + (p[n] - '0');
          ++n;
        }
        if (total > 0) g_ram_budget = total;
      }
      printf("ramdisk device installed, %d bytes\n", g_ram_budget);
    }
    // Does this console flush a partial line? Natively PRTF bottoms out
    // in the host libc, whose stdout is line-buffered on a TTY, so an
    // "A>" with no newline sits in the buffer and the user cannot tell
    // DOS is ready. c4bb's UART emits each byte as it is written, so
    // there the classic DOS look works and is worth keeping.
    //
    // ANNOUNCED, NEVER PROBED. The only probe in the tree is
    // include/c4bb_info.h, and that header says why it is no use here:
    // it asks __c4_info(), INFO is opcode 57, and c4cc compiles every
    // function in a header whether it is called or not -- so including
    // it would put DOS above the base-c4 rung and break the purity pin.
    // A config line costs nothing above EXIT.
    if (starts_ci(p, "DEVICE=CONSOLE.SYS")) {
      n = 0;
      while (p + n < e && !starts_ci(p + n, "FLUSH")) ++n;
      if (p + n < e) {
        g_conflush = 1;
        printf("console device installed, prompt unbuffered\n");
      } else {
        printf("DEVICE=CONSOLE.SYS: no mode given, prompt stays on its own line\n");
      }
    }
    // HOME=B: names the drive files are written to (file_create): the
    // page keeps drive B: in the browser, so what is saved there lasts.
    // A home that is not there, or cannot be written, is no home, and
    // files go to the RAM disk as before.
    if (starts_ci(p, "DEVICE=DRIVES.SYS")) {
      g_drives = 1;
      n = 0;
      while (p + n < e && !starts_ci(p + n, "HOME=")) ++n;
      if (p + n + 6 < e && p[n + 6] == ':') {
        g_home = upper(p[n + 5]) - 'A';
        if (g_home < 0 || g_home > 25) g_home = 0 - 1;
      }
      if (g_home >= 0 && !drv_writable(g_home)) {
        printf("drive device installed, %d drive(s); %c: cannot be written, files go to the RAM disk\n",
               drv_count(), drv_letter(g_home));
        g_home = 0 - 1;
      } else if (g_home >= 0) {
        printf("drive device installed, %d drive(s), files are written to %c:\n", drv_count(), drv_letter(g_home));
      } else printf("drive device installed, %d drive(s)\n", drv_count());
    }
    // Heap generations (see gen_dos). The registers answer before DOS
    // relies on them: written 1, HEAP_GEN must read back 1.
    if (starts_ci(p, "DEVICE=HEAP.SYS")) {
      *(int *)BB_HEAP_GEN = 1;
      if (*(int *)BB_HEAP_GEN == 1) {
        *(int *)BB_HEAP_GEN = 0;
        g_heapgen = 1;
        printf("heap device installed, each program's memory is freed when it ends\n");
      } else {
        printf("DEVICE=HEAP.SYS: this machine's heap keeps no generations\n");
      }
    }
    // > >> < and | on the command line (line_run). TAP says the machine
    // has the console tap, so that c4cc programs are redirected too.
    if (starts_ci(p, "DEVICE=REDIR.SYS")) {
      g_redir = 1;
      n = 0;
      while (p + n < e && !starts_ci(p + n, "TAP")) ++n;
      if (p + n < e) {
        g_tap = 1;
        *(int *)BB_CON_TAP = 0;
        *(int *)BB_CON_IN = 0 - 1;
        printf("redirection installed (> >> < |), with the console tap\n");
      } else printf("redirection installed (> >> < |)\n");
    }
    // FILES=, SHELL=, other DEVICE= lines: reserved, ignored
    p = *e ? e + 1 : e;
  }
  free(buf);
  return 0;
}

// ---- main ----------------------------------------------------------------

int main (int argc, char **argv) {
  int n;

  stub();               // arm the invoke stub before anything else
  learn_trampoline();   // and learn the double-LEV tail address

  if (!scratch_need()) return 1;
  if (!(g_line = malloc(LINEMAX))) { printf("c4dos: out of memory\n"); return 1; }
  if (!(g_dirbuf = malloc(DIRMAX))) { printf("c4dos: out of memory\n"); return 1; }
  if (!(g_namebuf = malloc(LINEMAX))) { printf("c4dos: out of memory\n"); return 1; }
  if (!(g_progbuf = malloc(LINEMAX + 8))) { printf("c4dos: out of memory\n"); return 1; }
  if (!(g_batbuf  = malloc(LINEMAX + 8))) { printf("c4dos: out of memory\n"); return 1; }
  if (!(g_wname   = malloc(LINEMAX + 4))) { printf("c4dos: out of memory\n"); return 1; }
  if (!(g_dname   = malloc(16))) { printf("c4dos: out of memory\n"); return 1; }
  if (!(g_copybuf = malloc(LINEMAX + 4))) { printf("c4dos: out of memory\n"); return 1; }
  if (!(g_morebuf = malloc(LINEMAX + 4))) { printf("c4dos: out of memory\n"); return 1; }
  if (!(g_tapbuf  = malloc(TAPBUF + 4))) { printf("c4dos: out of memory\n"); return 1; }
  if (!(g_pfbuf   = malloc(PFMAX + 4))) { printf("c4dos: out of memory\n"); return 1; }
  if (!(g_pfdig   = malloc(32))) { printf("c4dos: out of memory\n"); return 1; }
  g_pfn = 0;
  g_out_h = 0 - 1;
  g_in_data = 0;
  g_linefn = (int)&line_run;
  g_batreq = 0;
  if (!(g_ram_name = malloc(RAMFILES * sizeof(int)))) { printf("c4dos: out of memory\n"); return 1; }
  if (!(g_ram_data = malloc(RAMFILES * sizeof(int)))) { printf("c4dos: out of memory\n"); return 1; }
  if (!(g_ram_len  = malloc(RAMFILES * sizeof(int)))) { printf("c4dos: out of memory\n"); return 1; }
  if (!(g_ram_cap  = malloc(RAMFILES * sizeof(int)))) { printf("c4dos: out of memory\n"); return 1; }
  if (!(g_rh_slot  = malloc(RAMHANDLE * sizeof(int)))) { printf("c4dos: out of memory\n"); return 1; }
  if (!(g_rh_pos   = malloc(RAMHANDLE * sizeof(int)))) { printf("c4dos: out of memory\n"); return 1; }
  n = 0;
  while (n < RAMHANDLE) { g_rh_slot[n] = 0 - 1; ++n; }
  if (!(g_wh_kind = malloc(WHANDLE * sizeof(int)))) { printf("c4dos: out of memory\n"); return 1; }
  if (!(g_wh_ref  = malloc(WHANDLE * sizeof(int)))) { printf("c4dos: out of memory\n"); return 1; }
  if (!(g_wh_drv  = malloc(WHANDLE * sizeof(int)))) { printf("c4dos: out of memory\n"); return 1; }
  if (!(g_wh_img  = malloc(WHANDLE * sizeof(int)))) { printf("c4dos: out of memory\n"); return 1; }
  if (!(g_wh_name = malloc(WHANDLE * sizeof(int)))) { printf("c4dos: out of memory\n"); return 1; }
  n = 0;
  while (n < WHANDLE) {
    g_wh_kind[n] = WH_FREE;
    if (!(g_wh_name[n] = (int)malloc(LINEMAX))) { printf("c4dos: out of memory\n"); return 1; }
    ++n;
  }
  if (!(g_inbuf = malloc(INBUFMAX))) { printf("c4dos: out of memory\n"); return 1; }
  g_inlen = 0;
  g_inpos = 0;
  if (!(g_argv = malloc(ARGVMAX * sizeof(char *)))) { printf("c4dos: out of memory\n"); return 1; }
  if (!(g_imgs = malloc(IMGMAX * sizeof(int)))) { printf("c4dos: out of memory\n"); return 1; }
  memset(g_imgs, 0, IMGMAX * sizeof(int));
  g_cur_img = 0;
  g_depth = 0;
  // 32 slots, ZEROED. The v1 table allocated 16 and filled 9, so slots
  // 9-15 were whatever the heap had in them -- a transient that read
  // one got a plausible-looking address and jumped to it. Room to grow
  // is cheap; uninitialised function pointers are not.
  if (!(g_api = malloc(C4DOS_API_SLOTS * sizeof(int)))) { printf("c4dos: out of memory\n"); return 1; }
  memset(g_api, 0, C4DOS_API_SLOTS * sizeof(int));
  g_api[0] = ('C' << 16) + ('4' << 8) + 'D';   // magic
  g_api[1] = (int)&dos_exit;                   // from main and below only
  g_api[2] = 0;                                // filled after CONFIG.SYS
  g_api[3] = (int)&dos_api_write;
  g_api[4] = (int)&dos_api_close;
  g_api[5] = 7;                                // API version
  g_api[6] = (int)&dos_api_open;
  g_api[7] = (int)&dos_api_read;
  g_api[8] = (int)&dos_api_rclose;
  // v2 slots 13/14 are about DOS's own memory and do not need a disk.
  g_api[13] = (int)&dos_api_trim;
  g_api[14] = (int)&dos_api_release;
  // v4: the console, and running programs. None needs a device; 18
  // (unlink) waits for the RAM disk, below, and 25 is the shell's to
  // fill (grin's window kit table), 0 until one does. 26 and 27 answer
  // 0 and -1 without HEAP.SYS rather than being absent, so a shell can
  // call them unconditionally.
  g_api[16] = (int)&dos_api_conwrite;
  g_api[17] = (int)&dos_api_conread;
  g_api[19] = (int)&dos_api_exec;
  g_api[20] = (int)&dos_api_load;
  g_api[21] = (int)&dos_api_start;
  g_api[22] = (int)&dos_api_unload;
  g_api[23] = (int)&dos_api_catch;
  g_api[24] = (int)&dos_api_sink;
  g_api[25] = 0;
  g_api[26] = (int)&dos_api_gen;
  g_api[27] = (int)&dos_api_setgen;
  g_sink_w = 0;
  g_sink_r = 0;
  g_sink_ctx = 0;
  g_heapgen = 0;
  g_gen_next = 0;
  g_home = 0 - 1;
  g_echo = 1;
  g_clock = 0;
  g_quit = 0;
  g_in_trans = 0;

  cmd_ver();
  read_config();
  // Only now is it known whether there is anywhere to write. Advertise
  // CREATE only if there is: a transient checks that slot before
  // believing any of the API, so a DOS booted without
  // DEVICE=RAMDISK.SYS honestly reports that it cannot save a file
  // rather than failing halfway through one.
  // The home drive (DRIVES.SYS HOME=) is somewhere to write as well.
  if (g_ramdisk || g_home >= 0) g_api[2] = (int)&dos_api_create;
  if (g_ramdisk) {
    // Enumeration follows the same rule: only offered when there is a
    // disk to enumerate, so a caller that checks the slot is told the
    // truth rather than being handed an always-empty listing.
    g_api[9]  = (int)&dos_api_count;
    g_api[10] = (int)&dos_api_entname;
    g_api[11] = (int)&dos_api_entsize;
    g_api[12] = (int)&dos_api_entdata;
  }
  if (g_ramdisk || g_drives) g_api[18] = (int)&dos_api_unlink;
  // v5: always there, like 26 and 27: home() says -1 and drives() 1 when
  // CONFIG.SYS gave no home and no drives
  g_api[28] = (int)&dos_api_home;
  g_api[29] = (int)&dos_api_drives;
  // v6: the prompt's drive, always there
  g_api[30] = (int)&dos_api_drive;
  // v7: the image counted as running, for a shell called into directly
  g_api[31] = (int)&dos_api_image;
#if C4DOS_CLOCK
  // Same rule for the clock: offered only when the hardware was
  // declared, so a caller that checks the slot is told the truth.
  if (g_clock) g_api[15] = (int)&dos_api_time;
#endif
  run_batch("autoexec.bat", 0);

  while (!g_quit) {
    // A console that cannot flush a partial line needs the newline, or
    // the prompt is invisible until the next one. DEVICE=CONSOLE.SYS
    // FLUSH says this one can, and keeps the classic look.
    printf(g_conflush ? "%c>" : "%c>\n", drv_letter(g_drive));
    if (!get_line()) g_quit = 1;         // EOF: the console went away
    else line_run(0);
  }
  printf("C4DOS: system halted\n");
  return 0;
}
