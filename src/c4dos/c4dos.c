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
int c4r_load (char *path, int *api) {
  char *p, *data;
  int *code, *cons, *des;
  int fd, n, total;
  int entry, codelen, datalen, patchlen, symlen, conslen, deslen, memsz;
  int i, ptype, paddr, pvalu;

  if ((fd = open(path, 0)) < 0) return 0;
  total = 0;
  while ((n = read(fd, g_scratch + total, 65536)) > 0) total = total + n;
  close(fd);
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
  if ((fd = open(path, 0)) < 0) return 0;
  while ((n = read(fd, g_scratch, 4096)) > 0) {
    i = 0;
    while (i < n) { printf("%c", g_scratch[i]); ++i; }
  }
  close(fd);
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
  if (type_file("c4dos.dir")) return 0;
  printf("no c4dos.dir on this disk - the raw disk has no directory service\n");
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

int dispatch (int argc) {
  char *cmd; int r;
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
  if (cieq(cmd, "TIME")) return cmd_time();
  if (cieq(cmd, "MEM")) return cmd_mem();
  if (cieq(cmd, "EXIT")) { g_quit = 1; return 0; }
  if (cieq(cmd, "RUN")) {
    if (argc < 2) { printf("usage: RUN program.c4r [args]\n"); return 1; }
    return run_program(g_argv[1], argc - 1, g_argv + 1);
  }
  if (is_program_name(cmd)) return run_program(cmd, argc, g_argv);
  printf("bad command or file name: %s\n", cmd);
  return 1;
}

// ---- CONFIG.SYS and batch ------------------------------------------------

// run a file of commands, one per line. '@' prefixes suppress echo.
// Returns 0 if the file existed.
int run_batch (char *path, int depth) {
  char *buf, *p, *e; int fd, n, total, show;
  if (depth > BATDEPTH) { printf("batch nested too deep\n"); return 1; }
  if ((fd = open(path, 0)) < 0) return 1;
  total = 0;
  while ((n = read(fd, g_scratch + total, 4096)) > 0) total = total + n;
  close(fd);
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
    p = *e ? e + 1 : e;
  }
  free(buf);
  return 0;
}

int read_config () {
  char *buf, *p, *e; int fd, n, total;
  if ((fd = open("config.sys", 0)) < 0) return 0;
  total = 0;
  while ((n = read(fd, g_scratch + total, 4096)) > 0) total = total + n;
  close(fd);
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

  if (!(g_scratch = malloc(BUF_MAX))) { printf("c4dos: out of memory\n"); return 1; }
  if (!(g_line = malloc(LINEMAX))) { printf("c4dos: out of memory\n"); return 1; }
  if (!(g_inbuf = malloc(INBUFMAX))) { printf("c4dos: out of memory\n"); return 1; }
  g_inlen = 0;
  g_inpos = 0;
  if (!(g_argv = malloc(ARGVMAX * sizeof(char *)))) { printf("c4dos: out of memory\n"); return 1; }
  if (!(g_api = malloc(8 * sizeof(int)))) { printf("c4dos: out of memory\n"); return 1; }
  g_api[0] = ('C' << 16) + ('4' << 8) + 'D';   // magic
  g_api[1] = 0;                                // dos_exit entry: see c4dos.h
  g_echo = 1;
  g_clock = 0;
  g_quit = 0;
  g_in_trans = 0;

  cmd_ver();
  read_config();
  run_batch("autoexec.bat", 0);

  while (!g_quit) {
    printf("A>");
    if (!get_line()) g_quit = 1;         // EOF: the console went away
    else dispatch(parse_line());
  }
  printf("C4DOS: system halted\n");
  return 0;
}
