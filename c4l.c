// c4l.c - a .c4r loader for PLAIN c4.
//
//   ./c4 c4l.c program.c4r [args...]
//
// Loads a compiled .c4r image (c4cc or c4lc output) and executes it
// under the plain c4 VM, which knows nothing about images: c4 just
// interprets this file, and this file does the rest. Control transfer
// into loaded code uses c4lm's invoke-stub trick (src/c4lm/include/
// c4.h): a stub function finds its own address at runtime by scanning
// backward from its return pc for its ENT, then overwrites its own
// body with "JMP <slot>". Calling a code pointer is then "write the
// target into the slot, call the stub" -- the JSR pushes the return
// address, the stub jumps, the target's ENT builds a normal frame,
// and its LEV returns to us. Function pointers on an unmodified VM.
//
// WHAT THIS CAN AND CANNOT RUN. c4 implements LEA..EXIT and nothing
// else, so an image runs here only if its compiler emitted nothing
// above EXIT. That excludes any image with an indirect call (JSRS), a
// custom opcode (OPCD), putchar (PUTC), a cycle count (C4CY), or a
// jumptable switch (JMPA) -- so most real programs, and every kernel.
//
// Such an image is not broken and neither is this loader: it wants a
// machine with those instructions, which is c4m.
//
//   ./c4 c4m.c load-c4r.c -- program.c4r [args...]
//
// That is still plain c4 all the way down, c4m being itself a c4
// program -- one interpreter deeper, and correspondingly slower,
// which is exactly the cost this loader avoids for images that do not
// need it. scan_extended below refuses up front and names the
// instruction, instead of letting c4 die with a bare "unknown
// instruction" some thousands of cycles later.
//
// Patch types: -1 code->code, -2 code->data, -3 data->code,
// -4 data->data (byte-offset addresses); positive types are symbol
// references for the linker and are ignored here, as in load-c4r.c.
//
// Written in the strict c4 subset: no switch, no break, no function
// addresses, locals at function top, single-word globals.

enum { LEA ,IMM ,JMP ,JSR ,BZ  ,BNZ ,ENT ,ADJ ,LEV ,LI  ,LC  ,SI  ,SC  ,PSH ,
       OR  ,XOR ,AND ,EQ  ,NE  ,LT  ,GT  ,LE  ,GE  ,SHL ,SHR ,ADD ,SUB ,MUL ,DIV ,MOD ,
       OPEN,READ,CLOS,PRTF,MALC,FREE,MSET,MCMP,EXIT };
enum { MAX_SEARCH = 512 };
enum { BUF_MAX = 8388608 };  // 8MB image limit

int *c4l_stub_slot;   // operand slot of the rewritten stub
char *c4l_opnames;    // 5 bytes per opcode, as c4m lays them out

// Return pc of our caller, found on the stack relative to a local
int *caller_address (int dummy) {
  int *addr, *next, i;
  addr = (int *)(*(&addr + 2));
  i = 0;
  next = addr;
  while (++i <= MAX_SEARCH) {
    --next;
    if (*addr == ENT) {
      if (*next > ADJ) return addr;
    }
    addr = next;
  }
  printf("c4l: could not find caller entry\n");
  return 0;
}

// First call: locate self, become "JMP <slot>". Later calls: jump.
int stub () {
  int *self;
  if (!(self = caller_address(0))) exit(1);
  *self = JMP;
  c4l_stub_slot = self + 1;
  return 0;
}

int invoke0 (int *code) { *c4l_stub_slot = (int)code; return stub(); }
int invoke1 (int *code, int a) { *c4l_stub_slot = (int)code; return stub(a); }
int invoke2 (int *code, int a, int b) { *c4l_stub_slot = (int)code; return stub(a, b); }

int wordat (char *p) { return *(int *)p; }

// Offset of the first instruction c4 cannot execute, or -1 if the
// image is clean. Walking is exact rather than a search: every opcode
// up to ADJ carries an operand word and the rest do not, and anything
// above EXIT ends the walk, so the two-word forms above EXIT (JSRI,
// JSRS) never need a case of their own.
int scan_extended (int *code, int codelen) {
  int i;
  i = 0;
  while (i < codelen) {
    if (code[i] > EXIT) return i;
    if (code[i] <= ADJ) i = i + 2; else i = i + 1;
  }
  return -1;
}

int main (int argc, char **argv) {
  char *buf, *p, *data;
  int fd, n, total, wordbytes;
  int entry, codelen, datalen, patchlen, conslen, deslen;
  int *code, *cons, *des;
  int i, ptype, paddr, pvalu, r;

  --argc; ++argv;  // skip our own name
  if (argc < 1) { printf("usage: c4 c4l.c program.c4r [args...]\n"); return 1; }

  // arm the stub before anything else
  stub();

  c4l_opnames =
    "LEA ,IMM ,JMP ,JSR ,BZ  ,BNZ ,ENT ,ADJ ,LEV ,LI  ,LC  ,SI  ,SC  ,PSH ,"
    "OR  ,XOR ,AND ,EQ  ,NE  ,LT  ,GT  ,LE  ,GE  ,SHL ,SHR ,ADD ,SUB ,MUL ,DIV ,MOD ,"
    "OPEN,READ,CLOS,PRTF,MALC,FREE,MSET,MCMP,EXIT,"
    "PUTC,PUTS,RALC,MCPY,STRC,ITH ,_OPC,_BLT,_TRP,OPCD,"
    "_JMP,_ADJ,C4CF,C4CY,TIME,SIGH,SIGI,USLP,INFO,OPSL,"
    "C4IV,FLT ,JSRI,JSRS,JMPA,TLEV,DBG ,";

  if (!(buf = malloc(BUF_MAX))) { printf("c4l: out of memory\n"); return 1; }
  if ((fd = open(*argv, 0)) < 0) { printf("c4l: cannot open %s\n", *argv); return 1; }
  total = 0;
  while ((n = read(fd, buf + total, 65536)) > 0) total = total + n;
  close(fd);
  if (total < 13) { printf("c4l: %s is not a .c4r\n", *argv); return 1; }

  p = buf;
  if (!(p[0] == 'C' && p[1] == '4' && p[2] == 'R')) { printf("c4l: bad signature\n"); return 1; }
  wordbytes = p[4] / 8;
  if (wordbytes != sizeof(int)) { printf("c4l: %d-bit image, host is %d-bit\n", p[4], sizeof(int) * 8); return 1; }
  p = p + 13;  // signature, version, wordbits, 8 padding bytes

  entry    = wordat(p); p = p + 8;
  codelen  = wordat(p); p = p + 8;
  datalen  = wordat(p); p = p + 8;
  patchlen = wordat(p); p = p + 8;
  p = p + 8;  // symbol count, unused here
  conslen  = wordat(p); p = p + 8;
  deslen   = wordat(p); p = p + 8;

  // code segment: keep in place inside the read buffer (aligned: the
  // header prefix is 13 + 7*8 bytes, plus the marker word below)
  p = p + 8;  // 'C' marker word
  code = (int *)p;
  p = p + codelen * 8;

  // data segment: copy to a fresh zero-padded allocation so it is
  // word-aligned regardless of code length
  p = p + 8;  // 'D' marker
  if (!(data = malloc(datalen + 8))) { printf("c4l: out of memory\n"); return 1; }
  memset(data, 0, datalen + 8);
  i = 0;
  while (i < datalen) { data[i] = p[i]; ++i; }
  p = p + datalen;

  // patches
  p = p + 8;  // 'P' marker
  i = 0;
  while (i < patchlen) {
    ptype = wordat(p); paddr = wordat(p + 8); pvalu = wordat(p + 16);
    p = p + 24;
    if (ptype == -1) code[paddr] = (int)(code + pvalu);
    else if (ptype == -2) code[paddr] = (int)(data + pvalu);
    else if (ptype == -3) *(int *)(data + paddr) = (int)(code + pvalu);
    else if (ptype == -4) *(int *)(data + paddr) = (int)(data + pvalu);
    // positive types are symbol references: linker business, skipped
    ++i;
  }

  // Refuse an image this machine cannot execute, and say why. Done
  // after patching so the report is about the code that would have
  // run, and before any constructor, so nothing has happened yet.
  if ((i = scan_extended(code, codelen)) >= 0) {
    printf("c4l: %s needs %.4s (opcode %d) at code+%d, which plain c4 does not have.\n",
           *argv, c4l_opnames + code[i] * 5, code[i], i);
    printf("c4l: run it on a machine that does:\n");
    printf("c4l:   ./c4 c4m.c load-c4r.c -- %s [args...]\n", *argv);
    return 1;
  }

  // constructors and destructors (word offsets into code)
  p = p + 8;  // 'c' marker
  cons = (int *)p;
  p = p + conslen * 8;
  p = p + 8;  // 'd' marker
  des = (int *)p;
  // symbols follow; not needed to run

  i = 0;
  while (i < conslen) { invoke1(code + cons[i], 0); ++i; }

  r = invoke2(code + entry, argc, (int)argv);

  i = 0;
  while (i < deslen) { invoke0(code + des[i]); ++i; }

  return r;
}
