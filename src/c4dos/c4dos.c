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
//     src/tests/test_coop_switch.c: run_trans() saves its own frame
//     address; dos_exit() points its frame pair at it and returns
//     through the trampoline tail, landing exactly as if run_trans
//     had returned. LEV never touches A, so the status rides back as
//     the ordinary return value. No sp/bp registers were harmed.
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

// Build (both variants go through our own cpp - raw c4cc would skip
// the #if lines and compile BOTH clock branches in):
//   ./cpp src/c4dos/c4dos.c | ./c4cc -o c4dos.c4r -            (clockless)
//   ./cpp -DC4DOS_CLOCK=1 src/c4dos/c4dos.c | ./c4cc -o ... -  (with clock)
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
enum { RAMSIZE   = 1048576 };// default RAM disk budget, SIZE= overrides
enum { C4DOS_API_SLOTS = 32 }; // API table words (include/c4dos.h agrees)
enum { ARGVMAX  = 16 };
enum { BATDEPTH = 4 };

enum { VER_MAJOR = 0, VER_MINOR = 1 };

// ---- state (single-word globals only) ------------------------------------

int *c4l_stub_slot;      // operand slot of the rewritten invoke stub
int g_tramp_pc;          // the trampoline tail: ADJ/LEV, learned at boot
int g_run_bp;            // run_trans's frame address while a transient runs
int g_in_trans;          // a transient is running (dos_exit sanity check)

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
char *g_inbuf;           // console byte stream: one read() may carry
int g_inlen; int g_inpos; // many lines (a pipe) or one (a cooked tty),
                          // so lines are extracted here, never assumed.
                          // (c4sh assumed - test-c4bb.sh:104 documents
                          // the dropped-bytes bug that caused.)

int g_echo;              // batch ECHO state
int g_clock;             // CONFIG.SYS said DEVICE=CLOCK.SYS
int g_quit;              // EXIT was typed

// loaded-image registers (the plain-c4 "struct")
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
  if (!g_scratch) {
    if (!(g_scratch = malloc(BUF_MAX))) printf("c4dos: out of memory\n");
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

int invoke0 (int *code) { *c4l_stub_slot = (int)code; return stub(); }
int invoke1 (int *code, int a) { *c4l_stub_slot = (int)code; return stub(a); }
int invoke2 (int *code, int a, int b) { *c4l_stub_slot = (int)code; return stub(a, b); }

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

// The transient-facing API. Slot 0 of the table handed to programs
// via __c4dos_api. A transient's exit() calls this instead of the
// EXIT opcode (which would halt the machine).
int dos_exit (int status) {
  int *bp;
  if (!g_in_trans) {
    printf("c4dos: dos_exit outside a program?\n");
    exit(1);
  }
  g_in_trans = 0;
  bp = (int *)(&bp + 1);
  *bp = g_run_bp;          // after our LEV: bp = run_trans's frame...
  *(bp + 1) = g_tramp_pc;  // ...pc = the trampoline tail (ADJ/LEV)
  return status;           // LEV; A carries the status home
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

// c4dos.dir into g_dirbuf; returns its length, 0 if there is no listing
int dir_slurp () {
  int fd, n, total, going;
  // The raw open, deliberately: this is the resolver, and routing it
  // through dos_open would recurse straight back into itself.
  if ((fd = open("c4dos.dir", 0)) < 0) return 0;
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

// the real spelling of `name` according to the listing, or 0
char *dir_resolve (char *name) {
  char *p, *q;
  if (!dir_slurp()) return 0;
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

// open for reading, case-insensitively. Every file this system opens
// goes through here.
int dos_open (char *path) {
  int fd;
  char *real;
  // "./name" IS "name". c4ke.c says #include "./load-c4r.c", and a
  // name that came out of an archive is stored plainly -- c4bb's own
  // disk controller strips the prefix for the same reason.
  if (path[0] == '.' && path[1] == '/') path = path + 2;
  // RAM first, disk second -- the same precedence C4KE's loader uses,
  // and the reason a tool can rewrite a file that shipped read-only.
  if ((fd = ram_open(path)) >= 0) return fd;
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

int c4r_load (char *path, int *api) {
  char *p, *data;
  int *code, *cons, *des;
  int fd, n, total;
  int entry, codelen, datalen, patchlen, symlen, conslen, deslen, memsz;
  int i, ptype, paddr, pvalu;

  if (!scratch_need()) return 0;
  if ((fd = dos_open(path)) < 0) return 0;
  total = 0;
  while ((n = dos_read(fd, g_scratch + total, 65536)) > 0) total = total + n;
  dos_close(fd);
  if (total < 13) { printf("c4dos: %s is not a program\n", path); return 0; }

  p = g_scratch;
  if (!(p[0] == 'C' && p[1] == '4' && p[2] == 'R')) {
    printf("c4dos: bad signature in %s\n", path);
    return 0;
  }
  if (p[4] / 8 != sizeof(int)) {
    printf("c4dos: %d-bit image, this machine is %d-bit\n", p[4], sizeof(int) * 8);
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
  if (!(code = malloc(codelen * sizeof(int)))) { printf("c4dos: out of memory\n"); return 0; }
  i = 0;
  while (i < codelen) { code[i] = wordat(p + i * sizeof(int)); ++i; }
  p = p + codelen * sizeof(int);

  // data: allocate MEMSZ (BSS tail zeroed), copy DATALEN
  p = p + sizeof(int);   // 'D' marker
  if (!(data = malloc(memsz + sizeof(int)))) { free(code); printf("c4dos: out of memory\n"); return 0; }
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
  img_cons = cons;      // point into g_scratch: run them BEFORE the next load
  img_ncons = conslen;
  img_des = des;
  img_ndes = deslen;
  return 1;
}

// Save our own frame address, then hand control to the transient.
// Either main returns normally (its LEV walks back to us through the
// invoke stub) or dos_exit trampolines - both land here, status in A.
int run_trans (int entry, int argc, char **argv) {
  int *bp, r;
  bp = (int *)(&bp + 1);
  g_run_bp = (int)bp;
  g_in_trans = 1;
  r = invoke2((int *)entry, argc, (int)argv);
  g_in_trans = 0;
  return r;
}

// The API table: [0] magic 'C4D', [1] dos_exit's invoke address...
// which plain c4 cannot take (&function is a c4m feature). The table
// instead holds the STUB SLOT protocol: [1] = the address of a
// one-word cell the shim writes a target INDEX into, [2..] reserved.
// M0 keeps it minimal: transients built with include/c4dos.h call
// dos_exit through api[1] as a code address on machines that allow
// indirect calls, and everything else just returns from main.
int *g_api;

int run_program (char *path, int argc, char **argv) {
  int r, i;
  if (!c4r_load(path, g_api)) return -1;
  i = 0;
  while (i < img_ncons) { invoke1(img_code + img_cons[i], 0); ++i; }
  r = run_trans(img_entry, argc, argv);
  i = 0;
  while (i < img_ndes) { invoke0(img_code + img_des[i]); ++i; }
  free(img_code);
  free(img_data);
  return r;
}

// ---- files ---------------------------------------------------------------

// print a file to the console; returns 1 if it existed
int type_file (char *path) {
  int fd, n, i;
  if (!scratch_need()) return 0;
  if ((fd = dos_open(path)) < 0) return 0;
  while ((n = dos_read(fd, g_scratch, 4096)) > 0) {
    i = 0;
    while (i < n) { printf("%c", g_scratch[i]); ++i; }
  }
  dos_close(fd);
  return 1;
}

// Pull ONE line from the console into g_line. Returns 0 on EOF.
enum { INBUFMAX = 4096 };
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

// ---- builtins ------------------------------------------------------------

int cmd_ver () {
  printf("C4DOS version %d.%d\n", VER_MAJOR, VER_MINOR);
  printf("the operating system before operating systems\n");
  return 0;
}

// The raw disk has no directory service (READ is all the hardware
// gives us), so DIR reads the disk's manifest file - the same move
// the c4bb web app makes with manifest.json. An honest limitation:
// early machines listed what the label said.
int cmd_dir () {
  int i;
  if (!type_file("c4dos.dir"))
    printf("no c4dos.dir on this disk - the raw disk has no directory service\n");
  if (g_ramdisk) {
    i = 0;
    while (i < g_ram_n) {
      printf("%s  <ram> %d bytes\n", (char *)g_ram_name[i], g_ram_len[i]);
      ++i;
    }
    printf("ramdisk: %d of %d bytes used, %d file(s)\n",
           g_ram_used, g_ram_budget, g_ram_n);
  }
  return 0;
}

int cmd_type (char *path) {
  if (!path || !*path) { printf("usage: TYPE file\n"); return 1; }
  if (!type_file(path)) { printf("file not found: %s\n", path); return 1; }
  return 0;
}

int cmd_time () {
#if C4DOS_CLOCK
  int ms, s, m, h;
  if (!g_clock) {
    printf("no clock device - add DEVICE=CLOCK.SYS to CONFIG.SYS\n");
    return 1;
  }
  ms = __time();
  s = ms / 1000; ms = ms % 1000;
  m = s / 60; s = s % 60;
  h = m / 60; m = m % 60;
  printf("uptime %d:%.2d:%.2d.%.3d\n", h, m, s, ms);
  return 0;
#else
  printf("this build has no clock hardware support\n");
  return 1;
#endif
}

int cmd_mem () {
  // no meminfo primitive; report what DOS knows it holds
  printf("resident: C4DOS shell, %d byte scratch, %d byte line buffer\n",
         BUF_MAX, LINEMAX);
  return 0;
}

// ---- command dispatch ----------------------------------------------------

// split g_line into g_argv, in place; returns argc
int parse_line () {
  char *p; int argc;
  p = g_line;
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
int dos_api_create (char *name) {
  if (!g_ramdisk) return 0 - 1;
  return ram_create(name, 4096);
}
int dos_api_write (int slot, char *buf, int len) { return ram_write(slot, buf, len); }
int dos_api_close (int slot) { return 0; }
// The read half: DOS's own opener, which checks RAM before disk, made
// available to a transient whose own open() only ever sees the host.
int dos_api_open (char *name) { return dos_open(name); }
int dos_api_read (int fd, char *buf, int len) { return dos_read(fd, buf, len); }
int dos_api_rclose (int fd) { return dos_close(fd); }

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
// that the RAM disk works. There is no '>' redirection on this system
// by decision (docs/c4dos-design.md); a tool that wants to produce a
// file does it directly, and this is the smallest thing that does.
int cmd_copy (char *src, char *dst) {
  int fd, n, total, slot;
  if (!src || !dst || !*src || !*dst) { printf("usage: COPY src dst\n"); return 1; }
  if (!g_ramdisk) { printf("no ramdisk - add DEVICE=RAMDISK.SYS to CONFIG.SYS\n"); return 1; }
  if (!scratch_need()) return 1;
  if ((fd = dos_open(src)) < 0) { printf("file not found: %s\n", src); return 1; }
  total = 0;
  while ((n = dos_read(fd, g_scratch + total, 4096)) > 0) total = total + n;
  dos_close(fd);
  if ((slot = ram_create(dst, total)) < 0) return 1;
  ram_write(slot, g_scratch, total);
  printf("%s -> %s (%d bytes)\n", src, dst, total);
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
  char *cmd, *path; int r;
  if (!argc) return 0;
  cmd = g_argv[0];
  if (cieq(cmd, "REM")) return 0;
  if (cieq(cmd, "ECHO")) {
    if (argc > 1 && cieq(g_argv[1], "OFF")) { g_echo = 0; return 0; }
    if (argc > 1 && cieq(g_argv[1], "ON")) { g_echo = 1; return 0; }
    r = 1;
    while (r < argc) { printf(r > 1 ? " %s" : "%s", g_argv[r]); ++r; }
    printf("\n");
    return 0;
  }
  if (cieq(cmd, "VER")) return cmd_ver();
  if (cieq(cmd, "DIR")) return cmd_dir();
  if (cieq(cmd, "TYPE")) return cmd_type(argc > 1 ? g_argv[1] : 0);
  if (cieq(cmd, "COPY"))
    return cmd_copy(argc > 1 ? g_argv[1] : 0, argc > 2 ? g_argv[2] : 0);
  if (cieq(cmd, "TIME")) return cmd_time();
  if (cieq(cmd, "MEM")) return cmd_mem();
  if (cieq(cmd, "EXIT")) { g_quit = 1; return 0; }
  if (cieq(cmd, "RUN")) {
    if (argc < 2) { printf("usage: RUN program[.c4r] [args]\n"); return 1; }
    if (!(path = prog_path(g_argv[1]))) {
      printf("file not found: %s\n", g_argv[1]);
      return 1;
    }
    return run_program(path, argc - 1, g_argv + 1);
  }
  if (is_program_name(cmd)) return run_program(cmd, argc, g_argv);
  // Not a builtin and no extension typed: try it as a program name.
  if ((path = prog_ext(cmd))) return run_program(path, argc, g_argv);
  // Then as a batch file. dispatch cannot run one itself -- run_batch
  // is defined further down and this dialect is define-before-use, so
  // the batch is handed back and whoever called dispatch runs it.
  if ((path = bat_path(cmd))) { g_batreq = path; return 0; }
  printf("bad command or file name: %s\n", g_argv[0]);
  return 1;
}

// ---- CONFIG.SYS and batch ------------------------------------------------

// run a file of commands, one per line. '@' prefixes suppress echo.
// Returns 0 if the file existed.
int run_batch (char *path, int depth) {
  char *buf, *p, *e; int fd, n, total, show;
  if (depth > BATDEPTH) { printf("batch nested too deep\n"); return 1; }
  if (!scratch_need()) return 1;
  if ((fd = dos_open(path)) < 0) return 1;
  total = 0;
  while ((n = dos_read(fd, g_scratch + total, 4096)) > 0) total = total + n;
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
    if (show && g_line[0]) printf("A>%s\n", g_line);
    dispatch(parse_line());
    if (g_batreq) {
      g_batreq = 0;
      if (depth + 1 <= BATDEPTH) run_batch(g_batbuf, depth + 1);
      else printf("batch nested too deep\n");
    }
    p = *e ? e + 1 : e;
  }
  free(buf);
  return 0;
}

int read_config () {
  char *buf, *p, *e; int fd, n, total;
  if (!scratch_need()) return 0;
  if ((fd = dos_open("config.sys")) < 0) return 0;
  total = 0;
  while ((n = dos_read(fd, g_scratch + total, 4096)) > 0) total = total + n;
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
  g_batreq = 0;
  if (!(g_ram_name = malloc(RAMFILES * sizeof(int)))) { printf("c4dos: out of memory\n"); return 1; }
  if (!(g_ram_data = malloc(RAMFILES * sizeof(int)))) { printf("c4dos: out of memory\n"); return 1; }
  if (!(g_ram_len  = malloc(RAMFILES * sizeof(int)))) { printf("c4dos: out of memory\n"); return 1; }
  if (!(g_ram_cap  = malloc(RAMFILES * sizeof(int)))) { printf("c4dos: out of memory\n"); return 1; }
  if (!(g_rh_slot  = malloc(RAMHANDLE * sizeof(int)))) { printf("c4dos: out of memory\n"); return 1; }
  if (!(g_rh_pos   = malloc(RAMHANDLE * sizeof(int)))) { printf("c4dos: out of memory\n"); return 1; }
  n = 0;
  while (n < RAMHANDLE) { g_rh_slot[n] = 0 - 1; ++n; }
  if (!(g_inbuf = malloc(INBUFMAX))) { printf("c4dos: out of memory\n"); return 1; }
  g_inlen = 0;
  g_inpos = 0;
  if (!(g_argv = malloc(ARGVMAX * sizeof(char *)))) { printf("c4dos: out of memory\n"); return 1; }
  // 32 slots, ZEROED. The v1 table allocated 16 and filled 9, so slots
  // 9-15 were whatever the heap had in them -- a transient that read
  // one got a plausible-looking address and jumped to it. Room to grow
  // is cheap; uninitialised function pointers are not.
  if (!(g_api = malloc(C4DOS_API_SLOTS * sizeof(int)))) { printf("c4dos: out of memory\n"); return 1; }
  memset(g_api, 0, C4DOS_API_SLOTS * sizeof(int));
  g_api[0] = ('C' << 16) + ('4' << 8) + 'D';   // magic
  g_api[1] = 0;                                // dos_exit entry: see c4dos.h
  g_api[2] = 0;                                // filled after CONFIG.SYS
  g_api[3] = (int)&dos_api_write;
  g_api[4] = (int)&dos_api_close;
  g_api[5] = 2;                                // API version
  g_api[6] = (int)&dos_api_open;
  g_api[7] = (int)&dos_api_read;
  g_api[8] = (int)&dos_api_rclose;
  // v2 slots 13/14 are about DOS's own memory and do not need a disk.
  g_api[13] = (int)&dos_api_trim;
  g_api[14] = (int)&dos_api_release;
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
  if (g_ramdisk) {
    g_api[2]  = (int)&dos_api_create;
    // Enumeration follows the same rule: only offered when there is a
    // disk to enumerate, so a caller that checks the slot is told the
    // truth rather than being handed an always-empty listing.
    g_api[9]  = (int)&dos_api_count;
    g_api[10] = (int)&dos_api_entname;
    g_api[11] = (int)&dos_api_entsize;
    g_api[12] = (int)&dos_api_entdata;
  }
  run_batch("autoexec.bat", 0);

  while (!g_quit) {
    printf("A>");
    if (!get_line()) g_quit = 1;         // EOF: the console went away
    else {
      dispatch(parse_line());
      if (g_batreq) { g_batreq = 0; run_batch(g_batbuf, 1); }
    }
  }
  printf("C4DOS: system halted\n");
  return 0;
}
