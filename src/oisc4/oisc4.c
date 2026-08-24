// oisc4.c - One Instruction Set Computer for C4
//
// Runs .c4r images (compiled by c4cc or c4lc) on a single-instruction VM:
//
//     add [addr:Source], [literal:Add], [addr:Dest]
//
// Every instruction reads a word from Source, adds the literal, writes the
// result to Dest, and sets three flag registers (EQ0/LT0/GT0). Control flow,
// arithmetic, calls, and IO all reduce to this one instruction; see
// docs/oisc4-design.md for the how.
//
// Architecture:
//   - One flat memory arena. Every OISC address is a byte offset into it.
//     There are NO host pointers anywhere in VM state, so images are
//     relocatable and the VM state is fully self-contained (a requirement
//     for the planned SIMD/GPU variant: N tasks = N arenas).
//   - Byte offsets 0..127 are the registers (word-sized slots).
//   - Byte offsets 256..511 are ports: writes/reads there hit devices
//     (console output, syscalls, and the math device).
//   - 4096 up: translated code, then data, argv, heap; stack at the top,
//     growing down. C4 pointers ARE arena offsets, so C4 pointer
//     arithmetic works unchanged and the syscall device translates
//     offsets to host pointers at the boundary only.
//   - PC = 0 halts the machine (register Z lives at 0 and is never a
//     valid instruction address).
//
// The translator loads a .c4r, walks the code segment twice (sizes then
// emission), and expands each C4/c4m opcode into 1..11 OISC instructions.
// The .c4r patch table says exactly which operands are code or data
// addresses, so no guessing is needed: code targets go through the
// address map, data targets are rebased onto the arena, everything else
// is a literal.
//
// Usage:
//   gcc -O2 -o oisc4 src/oisc4/oisc4.c
//   ./oisc4 [-d] [-s] [-v] [-m megs] [--] program.c4r [args...]
//     -d  trace every OISC instruction (very verbose)
//     -s  disassemble the translated code and exit
//     -v  verbose: translation stats, cycle count at exit
//     -m  arena size in MB (default 64)
//
// Written in the C4 dialect (single int type, no structs, char/int/ptr)
// so that c4cc/c4lc can also compile it to a .c4r for nested runs.

#include <stdio.h>
#include <stdlib.h>
#include <memory.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#pragma GCC diagnostic ignored "-Wformat"
// Native builds treat every int as a 64-bit word. c4cc/c4lc builds
// (which define C4CC, and remap 'long' to 'int' in their headers)
// already have word-sized int, and the define would expand to 'int int'.
#ifndef C4CC
#ifndef int
#define int long long
#endif
#endif

//////
// Constants
//////

// Registers: byte offsets of word-sized slots at the bottom of the arena.
enum {
	RZ    = 0,    // always zero (also: PC=0 means halt)
	RPC   = 8,    // program counter (byte offset of next instruction)
	REQ0  = 16,   // flag: last result == 0 ? 24 : 0
	RLT0  = 24,   // flag: last result <  0 ? 24 : 0
	RGT0  = 32,   // flag: last result >  0 ? 24 : 0
	RWR   = 40,   // read mode:  0 = word, 1 = signed byte
	RWW   = 48,   // write mode: 0 = word, 1 = byte
	RA    = 56,   // C4 accumulator
	RSP   = 64,   // C4 stack pointer (arena byte offset)
	RBP   = 72,   // C4 base pointer
	RT0   = 80,   // translator scratch
	RR0   = 88,   // syscall: stack snapshot
	RR1   = 96,   // syscall: printf argument count
	RR2   = 104,  // startup stub scratch (main return value)
	REGS_TOP = 128
};

// Ports: reads/writes in this range are device IO.
enum {
	PORT_BASE = 256,
	P_PUTC    = 256,  // write: emit low byte to stdout
	P_SYSCALL = 264,  // write: perform syscall (value = c4m opcode); read: last result
	P_MATHX   = 272,  // math device: X operand
	P_MATHY   = 280,  // math device: Y operand
	P_MATHOP  = 288,  // math device: write op (c4 opcode - OR) computes V = X op Y
	P_MATHV   = 296,  // math device: result
	PORT_TOP  = 512
};

enum {
	MEM_BASE = 4096,  // first general memory address
	INSTB    = 24,    // instruction size in bytes (3 words)
	FLAGV    = 24,    // value stored in a set flag register (= INSTB, see BZ)
	PH       = -99,   // placeholder: always overwritten at runtime before use
	STACK_RESERVE = 4194304, // 4MB stack at the top of the arena
	DEFAULT_MEGS  = 64
};

// C4/c4m opcodes, numbering must match c4m.c / load-c4r.c exactly.
enum {
	LEA ,IMM ,JMP ,JSR ,BZ  ,BNZ ,ENT ,ADJ ,LEV ,LI  ,LC  ,SI  ,SC  ,PSH ,
	OR  ,XOR ,AND ,EQ  ,NE  ,LT  ,GT  ,LE  ,GE  ,SHL ,SHR ,ADD ,SUB ,MUL ,DIV ,MOD ,
	OPEN,READ,CLOS,PRTF,MALC,FREE,MSET,MCMP,EXIT,
	PUTC,PUTS,RALC,MCPY,STRC,
	ITH ,_OPC,_BLT,_TRP,OPCD,
	_JMP,_ADJ,C4CF,C4CY,TIME,
	SIGH,SIGI,USLP,INFO,OPSL,
	C4IV,FLT ,JSRI,JSRS,JMPA,TLEV,DBG ,
	// c4mp's, which this translator names but does not implement
	CPUI,CPUN,CPUS,CPUH,
	CAS ,XCHG,FADD,CWAI,CWAK,IPI ,
	LXI ,SXI ,TRAW,
	// The fused opcodes -- docs/fused-opcodes.md. Appended, never
	// inserted; the same numbers as c4m.c, c4l.c, load-c4r.c, c4mp.h,
	// c4cc.c and c4r.lisp.
	LDL ,LDG ,PSHL,PSHG,LEAP,IMMP,LIP ,ADDL,STL ,POPA,
	INS_MAX
};

// .c4r patch types
enum {
	C4R_PTYPE_CODE  = -1,  // code word -> code address
	C4R_PTYPE_DATA  = -2,  // code word -> data address
	C4R_PTYPE_DCODE = -3,  // data word -> code address
	C4R_PTYPE_DDATA = -4   // data word -> data address
};

enum { FILE_OPEN_MODE = 0x8000 };
enum { RDBUF_MAX = 16777216 }; // 16MB max .c4r file size

//////
// VM globals
//////

char *vm_m;        // the arena
int   vm_size;     // arena size in bytes
int   vm_cycle;    // instructions executed
int   vm_exitcode;
int   vm_running;

// devices
int dev_mx, dev_my, dev_mop, dev_mv; // math device
int dev_sysres;                      // last syscall result

// allocator
int heap_brk, heap_top, heap_free;   // bump pointer, limit, freelist head

// options
int opt_trace, opt_disasm, opt_verbose, opt_maxcycles;

// layout (arena byte offsets)
int lay_codebase, lay_codeend, lay_database, lay_argvbase, lay_heapbase, lay_stacktop;

// mini-printf state
char *o4p_buf;     // digit conversion buffer
int   o4p_count;   // characters output by current vm_printf

// code-write gate (see vm_run): supports the repo's self-patching idiom
//   *f++ = __opcode("JMP"); *f = (int)&target;
int gate_pending;  // code address a JMP opcode was just written to
int gate_warned;

//////
// Word access
//////

int  wg (int addr)        { return *(int *)(vm_m + addr); }
void ws (int addr, int v) { *(int *)(vm_m + addr) = v; }

int align8 (int v) { return (v + 7) & -8; }

void fatal (char *msg) {
	printf("oisc4: %s\n", msg);
	exit(-1);
}

//////
// Time device: read /proc/uptime, return milliseconds.
// Same trick c4m uses; works native and nested.
//////
int vm_time () {
	char *buf, *p;
	int fd, n, ms;
	if ((fd = open("/proc/uptime", 0)) < 0) return 0;
	if (!(buf = malloc(64))) { close(fd); return 0; }
	memset(buf, 0, 64);
	n = read(fd, buf, 63);
	close(fd);
	if (n <= 0) { free(buf); return 0; }
	p = buf; ms = 0;
	while (*p >= '0' && *p <= '9') ms = ms * 10 + (*p++ - '0');
	ms = ms * 1000;
	if (*p == '.') {
		++p; n = 100;
		while (*p >= '0' && *p <= '9' && n) { ms = ms + (*p++ - '0') * n; n = n / 10; }
	}
	free(buf);
	return ms;
}

//////
// Arena allocator: 16-byte header [total size][freelist next], first fit.
//////

int vm_malloc (int n) {
	int total, b, bs, prev, rem;
	if (n < 0) return 0;
	total = align8(n) + 16;
	prev = 0; b = heap_free;
	while (b) {
		bs = wg(b);
		if (bs >= total) {
			if (bs - total >= 48) {
				// split, remainder stays free
				rem = b + total;
				ws(rem, bs - total);
				ws(rem + 8, wg(b + 8));
				ws(b, total);
				if (prev) ws(prev + 8, rem); else heap_free = rem;
			} else {
				if (prev) ws(prev + 8, wg(b + 8)); else heap_free = wg(b + 8);
			}
			return b + 16;
		}
		prev = b; b = wg(b + 8);
	}
	if (heap_brk + total > heap_top) {
		printf("oisc4: arena heap exhausted (%ld requested); rerun with -m <megs>\n", n);
		return 0;
	}
	b = heap_brk; heap_brk = heap_brk + total;
	ws(b, total);
	return b + 16;
}

void vm_free (int p) {
	if (!p) return;
	ws(p - 8, heap_free);
	heap_free = p - 16;
}

int vm_realloc (int p, int n) {
	int old, q;
	if (!p) return vm_malloc(n);
	old = wg(p - 16) - 16;
	if (align8(n) <= old) return p;
	if (!(q = vm_malloc(n))) return 0;
	memcpy(vm_m + q, vm_m + p, old);
	vm_free(p);
	return q;
}

//////
// Mini printf: PRTF must walk the format itself because %s arguments are
// arena offsets that need translation, and because forwarding to host
// printf is impossible when oisc4 itself runs nested under c4m.
// Behavior mirrors native c4m (glibc printf given word-sized args):
// without an 'l' modifier, integer conversions see their low 32 bits.
//////

void o4p_putc (char c) { putchar(c); ++o4p_count; }
void o4p_pad  (int n, char c) { while (n-- > 0) o4p_putc(c); }

// Unsigned decimal of a full 64-bit pattern into o4p_buf (reversed digits).
// Returns digit count.
int o4p_udec (int v) {
	int i, q, r;
	i = 0;
	if (!v) o4p_buf[i++] = '0';
	while (v) {
		q = ((v >> 1) & 0x7FFFFFFFFFFFFFFF) / 5;
		r = v - q * 10;
		o4p_buf[i++] = '0' + r;
		v = q;
	}
	return i;
}

int o4p_uhex (int v, int upper) {
	int i, d;
	i = 0;
	if (!v) o4p_buf[i++] = '0';
	while (v) {
		d = v & 15;
		if (d < 10) o4p_buf[i++] = '0' + d;
		else o4p_buf[i++] = (upper ? 'A' : 'a') + (d - 10);
		v = (v >> 4) & 0x0FFFFFFFFFFFFFFF;
	}
	return i;
}

int o4p_uoct (int v) {
	int i;
	i = 0;
	if (!v) o4p_buf[i++] = '0';
	while (v) {
		o4p_buf[i++] = '0' + (v & 7);
		v = (v >> 3) & 0x1FFFFFFFFFFFFFFF;
	}
	return i;
}

// Emit one integer conversion: digits already in o4p_buf (ndig, reversed),
// sign/prefix in front, honoring width/precision/flags.
void o4p_int (int ndig, char *sign, int width, int prec, int fminus, int fzero) {
	int len, zeros, i;
	zeros = 0;
	if (prec > ndig) zeros = prec - ndig;
	len = ndig + zeros;
	i = 0; while (sign[i]) ++i; // sign length
	len = len + i;
	// right-justified space pad (zero padding goes after the sign instead)
	if (!fminus && width > len && !(fzero && prec < 0)) o4p_pad(width - len, ' ');
	// sign/prefix
	i = 0; while (sign[i]) o4p_putc(sign[i++]);
	// zero pad from '0' flag
	if (!fminus && width > len && (fzero && prec < 0)) o4p_pad(width - len, '0');
	// precision zeros
	o4p_pad(zeros, '0');
	// digits (reversed in buffer)
	while (ndig) o4p_putc(o4p_buf[--ndig]);
	if (fminus && width > len) o4p_pad(width - len, ' ');
}

// The PRTF device. sp = stack snapshot (arena offset), argc from R1.
// Stack layout (args pushed left to right): fmt at sp+(argc-1)*8, then
// arg0 at sp+(argc-2)*8, ...
int vm_printf (int sp, int argc) {
	char *f, *s;
	int  t, ai, v, w, prec, fminus, fzero, fplus, fspace, falt, longmod;
	int  ndig, slen, c;
	char *sign;

	t = sp + argc * 8;
	f = vm_m + wg(t - 8);
	ai = t - 16;             // next argument address; move down by 8 each pull
	o4p_count = 0;
	if (!(sign = malloc(8))) return 0;

	while (*f) {
		if (*f != '%') { o4p_putc(*f++); }
		else {
			++f;
			if (*f == '%') { o4p_putc('%'); ++f; }
			else {
				// flags
				fminus = fzero = fplus = fspace = falt = 0;
				while (*f == '-' || *f == '0' || *f == '+' || *f == ' ' || *f == '#') {
					if (*f == '-') fminus = 1;
					else if (*f == '0') fzero = 1;
					else if (*f == '+') fplus = 1;
					else if (*f == ' ') fspace = 1;
					else falt = 1;
					++f;
				}
				// width
				w = 0;
				if (*f == '*') { w = wg(ai); ai = ai - 8; ++f; if (w < 0) { fminus = 1; w = -w; } }
				else while (*f >= '0' && *f <= '9') w = w * 10 + (*f++ - '0');
				// precision
				prec = -1;
				if (*f == '.') {
					++f; prec = 0;
					if (*f == '*') { prec = wg(ai); ai = ai - 8; ++f; }
					else while (*f >= '0' && *f <= '9') prec = prec * 10 + (*f++ - '0');
				}
				// length modifiers: l/ll widen to 64; h/hh/z/t/j parsed
				longmod = 0;
				while (*f == 'l' || *f == 'h' || *f == 'z' || *f == 't' || *f == 'j') {
					if (*f == 'l' || *f == 'z' || *f == 't' || *f == 'j') longmod = 1;
					++f;
				}
				c = *f++;
				if (c == 'd' || c == 'i') {
					v = wg(ai); ai = ai - 8;
					if (!longmod) v = (v << 32) >> 32;
					sign[0] = 0; sign[1] = 0;
					if (v < 0) { sign[0] = '-'; v = -v; } // INT_MIN wraps; udec treats as unsigned
					else if (fplus) sign[0] = '+';
					else if (fspace) sign[0] = ' ';
					ndig = o4p_udec(v);
					o4p_int(ndig, sign, w, prec, fminus, fzero);
				}
				else if (c == 'u') {
					v = wg(ai); ai = ai - 8;
					if (!longmod) v = v & 0xFFFFFFFF;
					sign[0] = 0;
					ndig = o4p_udec(v);
					o4p_int(ndig, sign, w, prec, fminus, fzero);
				}
				else if (c == 'x' || c == 'X' || c == 'o') {
					v = wg(ai); ai = ai - 8;
					if (!longmod) v = v & 0xFFFFFFFF;
					sign[0] = 0; sign[1] = 0; sign[2] = 0;
					if (falt && v) {
						if (c == 'o') sign[0] = '0';
						else { sign[0] = '0'; sign[1] = c == 'X' ? 'X' : 'x'; }
					}
					if (c == 'o') ndig = o4p_uoct(v);
					else ndig = o4p_uhex(v, c == 'X');
					o4p_int(ndig, sign, w, prec, fminus, fzero);
				}
				else if (c == 'p') {
					v = wg(ai); ai = ai - 8;
					sign[0] = '0'; sign[1] = 'x'; sign[2] = 0;
					ndig = o4p_uhex(v, 0);
					o4p_int(ndig, sign, w, -1, fminus, 0);
				}
				else if (c == 'c') {
					v = wg(ai); ai = ai - 8;
					if (!fminus && w > 1) o4p_pad(w - 1, ' ');
					o4p_putc(v);
					if (fminus && w > 1) o4p_pad(w - 1, ' ');
				}
				else if (c == 's') {
					v = wg(ai); ai = ai - 8;
					if (!v) s = "(null)"; else s = vm_m + v;
					slen = 0;
					while (s[slen] && (prec < 0 || slen < prec)) ++slen;
					if (!fminus && w > slen) o4p_pad(w - slen, ' ');
					v = 0; while (v < slen) o4p_putc(s[v++]);
					if (fminus && w > slen) o4p_pad(w - slen, ' ');
				}
				else if (!c) { --f; } // trailing lone '%'
				else { o4p_putc(c); } // unknown conversion: echo
			}
		}
	}
	free(sign);
	return o4p_count;
}

//////
// Syscall device. Value written to P_SYSCALL is a c4m opcode number;
// arguments are read from the VM stack via R0 (a stack snapshot the
// translated code stores just before the port write). Results go to
// dev_sysres, which the translated code reads back into A.
//////

int sarg (int sp, int k) { return wg(sp + k * 8); }

// __opcode("NAME") -> opcode number. Same table and 4-char
// case-insensitive prefix match as c4m's __opcode().
char o4_upper (char c) { return (c >= 'a' && c <= 'z') ? c - 'a' + 'A' : c; }
int vm_opcode_lookup (int nameoff) {
	char *name, *ops, *a, *b;
	int r, m, ok;
	if (!nameoff) return -1;
	name = vm_m + nameoff;
	ops =
	"LEA ,IMM ,JMP ,JSR ,BZ  ,BNZ ,ENT ,ADJ ,LEV ,LI  ,LC  ,SI  ,SC  ,PSH ,"
	"OR  ,XOR ,AND ,EQ  ,NE  ,LT  ,GT  ,LE  ,GE  ,SHL ,SHR ,ADD ,SUB ,MUL ,DIV ,MOD ,"
	"OPEN,READ,CLOS,PRTF,MALC,FREE,MSET,MCMP,EXIT,"
	"PUTC,PUTS,RALC,MCPY,STRC,"
	"ITH ,_OPC,_BLT,_TRP,OPCD,"
	"_JMP,_ADJ,C4CF,C4CY,TIME,"
	"SIGH,SIGI,USLP,INFO,OPSL,"
	"C4IV,FLT ,JSRI,JSRS,JMPA,TLEV,DBG ,"
	"CPUI,CPUN,CPUS,CPUH,"
	"CAS ,XCHG,FADD,CWAI,CWAK,IPI ,"
	"LXI ,SXI ,TRAW,"
	"LDL ,LDG ,PSHL,PSHG,LEAP,IMMP,LIP ,ADDL,STL ,POPA,";
	r = 0;
	while (r < INS_MAX) {
		a = name; b = ops + r * 5;
		m = 0; ok = 1;
		while (m < 4 && ok) {
			if (*a == 0 || *a == ' ') m = 4;
			else if (*b == 0 || *b == ' ') m = 4;
			else if (o4_upper(*a++) != o4_upper(*b++)) ok = 0;
			else ++m;
		}
		if (ok) return r;
		++r;
	}
	return -1;
}

void do_syscall (int op) {
	int sp;
	sp = wg(RR0);
	if (op == PRTF)      dev_sysres = vm_printf(sp, wg(RR1));
	else if (op == OPEN) dev_sysres = open(vm_m + sarg(sp, 1), sarg(sp, 0));
	else if (op == READ) dev_sysres = read(sarg(sp, 2), vm_m + sarg(sp, 1), sarg(sp, 0));
	else if (op == CLOS) dev_sysres = close(sarg(sp, 0));
	else if (op == MALC) dev_sysres = vm_malloc(sarg(sp, 0));
	else if (op == FREE) vm_free(sarg(sp, 0));
	else if (op == RALC) dev_sysres = vm_realloc(sarg(sp, 1), sarg(sp, 0));
	else if (op == MSET) { memset(vm_m + sarg(sp, 2), sarg(sp, 1), sarg(sp, 0)); dev_sysres = sarg(sp, 2); }
	else if (op == MCMP) dev_sysres = memcmp(vm_m + sarg(sp, 2), vm_m + sarg(sp, 1), sarg(sp, 0));
	else if (op == MCPY) { memcpy(vm_m + sarg(sp, 2), vm_m + sarg(sp, 1), sarg(sp, 0)); dev_sysres = sarg(sp, 2); }
	else if (op == PUTC) { o4p_count = 0; o4p_putc(sarg(sp, 0)); dev_sysres = sarg(sp, 0); }
	else if (op == PUTS) { // native c4m maps puts() to host puts: string + newline
		o4p_count = 0;
		printf("%s\n", vm_m + sarg(sp, 0));
		dev_sysres = 1;
	}
	else if (op == EXIT) { vm_exitcode = sarg(sp, 0); ws(RPC, 0); }
	else if (op == TIME) dev_sysres = vm_time();
	else if (op == C4CY) dev_sysres = vm_cycle;
	else if (op == OPCD) dev_sysres = sarg(sp, 0); // missed-probe convention
	else if (op == _OPC) dev_sysres = vm_opcode_lookup(sarg(sp, 0));
	else if (op == STRC) dev_sysres = printf("(oisc4: stacktrace unavailable)\n");
	else if (op == FLT)  { printf("oisc4: float instruction unsupported\n"); dev_sysres = 0; }
	else if (op == TLEV) { printf("oisc4: TLEV outside trap context, halting\n"); ws(RPC, 0); vm_exitcode = -1; }
	else if (op == DBG)  dev_sysres = 0;
	// ITH SIGH SIGI USLP INFO C4CF OPSL _OPC _BLT _TRP C4IV: no capabilities.
	// INFO=0 makes well-written images keep to the paths we support.
	else dev_sysres = 0;
}

//////
// Ports
//////

int port_read (int p) {
	if (p == P_MATHV)   return dev_mv;
	if (p == P_SYSCALL) return dev_sysres;
	if (p == P_MATHX)   return dev_mx;
	if (p == P_MATHY)   return dev_my;
	if (p == P_MATHOP)  return dev_mop;
	return 0;
}

void do_math (int op) {
	int x, y;
	x = dev_mx; y = dev_my;
	if (op == OR - OR)       dev_mv = x |  y;
	else if (op == XOR - OR) dev_mv = x ^  y;
	else if (op == AND - OR) dev_mv = x &  y;
	else if (op == EQ  - OR) dev_mv = x == y;
	else if (op == NE  - OR) dev_mv = x != y;
	else if (op == LT  - OR) dev_mv = x <  y;
	else if (op == GT  - OR) dev_mv = x >  y;
	else if (op == LE  - OR) dev_mv = x <= y;
	else if (op == GE  - OR) dev_mv = x >= y;
	else if (op == SHL - OR) dev_mv = x << y;
	else if (op == SHR - OR) dev_mv = x >> y;
	else if (op == ADD - OR) dev_mv = x +  y;
	else if (op == SUB - OR) dev_mv = x -  y;
	else if (op == MUL - OR) dev_mv = x *  y;
	else if (op == DIV - OR) dev_mv = x /  y;
	else if (op == MOD - OR) dev_mv = x %  y;
	else printf("oisc4: math device: bad op %ld\n", op);
}

void port_write (int p, int v) {
	if (p == P_PUTC)         putchar(v);
	else if (p == P_SYSCALL) do_syscall(v);
	else if (p == P_MATHX)   dev_mx = v;
	else if (p == P_MATHY)   dev_my = v;
	else if (p == P_MATHOP)  { dev_mop = v; do_math(v); }
	// P_MATHV: writes ignored
}

//////
// Debug: print an address symbolically
//////

void dbg_addr (int a) {
	if (a == RZ) printf("Z");
	else if (a == RPC)  printf("PC");
	else if (a == REQ0) printf("EQ0");
	else if (a == RLT0) printf("LT0");
	else if (a == RGT0) printf("GT0");
	else if (a == RWR)  printf("WR");
	else if (a == RWW)  printf("WW");
	else if (a == RA)   printf("A");
	else if (a == RSP)  printf("SP");
	else if (a == RBP)  printf("BP");
	else if (a == RT0)  printf("T0");
	else if (a == RR0)  printf("R0");
	else if (a == RR1)  printf("R1");
	else if (a == RR2)  printf("R2");
	else if (a == P_PUTC)    printf("IO.PUTC");
	else if (a == P_SYSCALL) printf("IO.SYS");
	else if (a == P_MATHX)   printf("IO.X");
	else if (a == P_MATHY)   printf("IO.Y");
	else if (a == P_MATHOP)  printf("IO.OP");
	else if (a == P_MATHV)   printf("IO.V");
	else if (a == PH) printf("PH");
	else printf("[%ld]", a);
}

void dbg_inst (int at, int src, int add, int dst) {
	printf("%8ld:  ", at);
	dbg_addr(src);
	printf(" + %ld -> ", add);
	dbg_addr(dst);
}

//////
// The VM
//////

int vm_run () {
	int pc, src, add, dst, v;
	while ((pc = wg(RPC))) {
		src = wg(pc); add = wg(pc + 8); dst = wg(pc + 16);
		ws(RPC, pc + INSTB);
		// read
		if (src >= PORT_BASE && src < PORT_TOP) v = port_read(src);
		else if (wg(RWR)) v = *(vm_m + src);       // signed byte
		else v = wg(src);
		v = v + add;
		// write
		if (dst >= PORT_BASE && dst < PORT_TOP) port_write(dst, v);
		else if (dst < 8) {                        // register Z: null-pointer write
			printf("oisc4: null pointer write at pc=%ld (cycle %ld), halting\n", pc, vm_cycle);
			vm_exitcode = -1;
			ws(RPC, 0);
		}
		else if (dst >= lay_codebase && dst < lay_codeend
		         && (dst - pc > 240 || dst - pc < -240)) {
			// A *guest* write into code. The translator's own self-patch
			// stores also land in the code region, but always within a few
			// instructions of the writer -- anything farther is the guest.
			// Translated code cannot be patched with C4 opcodes, but the
			// one idiom the repo uses --
			//   *f++ = __opcode("JMP"); *f = (int)&target;
			// -- is emulated: the pair becomes a real OISC jump at f.
			// (&f and &target arrive already OISC-mapped via the patch table.)
			if (v == JMP) gate_pending = dst;
			else if (gate_pending && dst == gate_pending + 8) {
				ws(gate_pending, RZ);
				ws(gate_pending + 8, v);
				ws(gate_pending + 16, RPC);
				gate_pending = 0;
			}
			else if (!gate_warned) {
				printf("oisc4: unsupported code write at pc=%ld (cycle %ld), ignored\n", pc, vm_cycle);
				gate_warned = 1;
			}
		}
		else if (wg(RWW)) *(vm_m + dst) = v;       // byte store
		else ws(dst, v);
		// flags
		ws(REQ0, v == 0 ? FLAGV : 0);
		ws(RLT0, v <  0 ? FLAGV : 0);
		ws(RGT0, v >  0 ? FLAGV : 0);
		++vm_cycle;
		// With -c, trace only the last 100 cycles before the limit.
		if (opt_trace && (!opt_maxcycles || vm_cycle > opt_maxcycles - 100)) {
			dbg_inst(pc, src, add, dst);
			printf("   v=%ld pc=%ld sp=%ld a=%ld\n", v, wg(RPC), wg(RSP), wg(RA));
		}
		if (opt_maxcycles && vm_cycle >= opt_maxcycles) {
			printf("oisc4: cycle limit %ld reached, pc=%ld\n", opt_maxcycles, wg(RPC));
			return -1;
		}
	}
	return vm_exitcode;
}

//////
// .c4r loader: whole file into memory, then parse.
//////

char *t_file;     // raw file bytes
int   t_flen;
int   t_entry, t_codelen, t_datalen, t_patchlen, t_conslen, t_deslen, t_memsz;
int  *t_code;     // code words (host copy)
char *t_data;     // data bytes (host copy)
int  *t_patches;  // patchlen * 3 words
int  *t_cons;     // constructor code offsets
int  *t_des;      // destructor code offsets

int   rd_pos;     // parse cursor

int rd_word () {
	int v;
	v = *(int *)(t_file + rd_pos);
	rd_pos = rd_pos + 8;
	return v;
}

int load_c4r (char *path) {
	int fd, n, i, seg;

	if (!(t_file = malloc(RDBUF_MAX))) fatal("cannot allocate read buffer");
	if ((fd = open(path, FILE_OPEN_MODE)) < 0) {
		printf("oisc4: cannot open '%s'\n", path);
		return 1;
	}
	t_flen = 0;
	while ((n = read(fd, t_file + t_flen, 262144)) > 0) {
		t_flen = t_flen + n;
		if (t_flen + 262144 > RDBUF_MAX) { close(fd); fatal("c4r file too large"); }
	}
	close(fd);
	if (t_flen < 70) { printf("oisc4: '%s' too small to be a c4r image\n", path); return 1; }
	if (!(t_file[0] == 'C' && t_file[1] == '4' && t_file[2] == 'R')) {
		printf("oisc4: '%s' has no C4R signature\n", path);
		return 1;
	}
	if (t_file[3] && t_file[3] != 2 && t_file[3] != 3) { printf("oisc4: unsupported c4r version %d\n", t_file[3]); return 1; }
	if (t_file[4] != 64) { printf("oisc4: c4r uses %d-bit words, need 64\n", t_file[4]); return 1; }

	// padding word (byte 5) = data MEMSZ in v3 (total in-memory size,
	// the excess over datalen being zero-filled BSS); v2 has none.
	t_memsz = (t_file[3] >= 3) ? *(int *)(t_file + 5) : 0;

	rd_pos = 13; // 3 sig + 1 version + 1 bits + 8 padding
	t_entry    = rd_word();
	t_codelen  = rd_word();
	t_datalen  = rd_word();
	t_patchlen = rd_word();
	rd_word(); // symbols (skipped)
	t_conslen  = rd_word();
	t_deslen   = rd_word();
	if (t_memsz < t_datalen) t_memsz = t_datalen;

	if (t_entry == -1) { printf("oisc4: image has no entry point\n"); return 1; }

	// Code segment
	seg = t_file[rd_pos]; rd_pos = rd_pos + 8;
	if (seg != 'C') { printf("oisc4: expected code segment, got 0x%x\n", seg); return 1; }
	if (!(t_code = malloc(8 * (t_codelen + 1)))) fatal("out of memory (code)");
	memcpy(t_code, t_file + rd_pos, 8 * t_codelen);
	rd_pos = rd_pos + 8 * t_codelen;

	// Data segment
	seg = t_file[rd_pos]; rd_pos = rd_pos + 8;
	if (seg != 'D') { printf("oisc4: expected data segment, got 0x%x\n", seg); return 1; }
	if (!(t_data = malloc(t_datalen + 8))) fatal("out of memory (data)");
	memcpy(t_data, t_file + rd_pos, t_datalen);
	rd_pos = rd_pos + t_datalen;

	// Patches
	seg = t_file[rd_pos]; rd_pos = rd_pos + 8;
	if (seg != 'P') { printf("oisc4: expected patch segment, got 0x%x\n", seg); return 1; }
	if (!(t_patches = malloc(8 * 3 * (t_patchlen + 1)))) fatal("out of memory (patches)");
	i = 0;
	while (i < t_patchlen * 3) { t_patches[i++] = rd_word(); }

	// Constructors
	seg = t_file[rd_pos]; rd_pos = rd_pos + 8;
	if (seg != 'c') { printf("oisc4: expected constructor segment, got 0x%x\n", seg); return 1; }
	if (!(t_cons = malloc(8 * (t_conslen + 1)))) fatal("out of memory (cons)");
	i = 0; while (i < t_conslen) t_cons[i++] = rd_word();

	// Destructors
	seg = t_file[rd_pos]; rd_pos = rd_pos + 8;
	if (seg != 'd') { printf("oisc4: expected destructor segment, got 0x%x\n", seg); return 1; }
	if (!(t_des = malloc(8 * (t_deslen + 1)))) fatal("out of memory (des)");
	i = 0; while (i < t_deslen) t_des[i++] = rd_word();

	// Symbols segment ignored.
	free(t_file); t_file = 0;
	return 0;
}

//////
// Translator
//////

int *tr_map;       // c4 code word index -> arena byte address of expansion
int *tr_ptype;     // per code word: 0 none, 1 CODE patch, 2 DATA patch
int *tr_pval;      // patch value for the above
int  oe;           // emission cursor (arena byte offset)

// Does this opcode carry an operand word? Same rule as c4m_has_operand
// in c4m.c -- LIP, ADDL and POPA take none.
int has_operand (int op) {
	return op <= ADJ || op == JSRI || op == JSRS
	    || (op >= LDL && op <= IMMP)
	    || op == STL;
}

// OISC instructions emitted for each c4 opcode (fixed per opcode).
int o4_size (int op) {
	if (op == LEA || op == IMM || op == JMP || op == ADJ || op == JMPA) return 1;
	if (op == LI) return 2;
	if (op == FREE || op == EXIT) return 2;
	if (op == PSH) return 3;
	if (op == JSR || op == BZ || op == BNZ || op == LC || op == JSRI || op == _JMP) return 4;
	if (op == ENT || op == SI || op == JSRS) return 5;
	if (op >= OR && op <= MOD) return 6;
	if (op == _ADJ) return 6;
	if (op == LEV) return 7;
	if (op == SC) return 11;
	if (op == PRTF) return 4;
	// The fused opcodes -- each is exactly the sequence it replaces,
	// so its expansion is the concatenation of theirs.
	if (op == LDL || op == LDG) return 3;
	if (op == PSHL || op == PSHG) return 6;
	if (op == LEAP || op == IMMP) return 4;
	if (op == LIP) return 5;
	if (op == ADDL) return 8;
	if (op == STL) return 2;
	if (op == POPA) return 3;
	// The syscall class ends at DBG. Bounding it by INS_MAX would make
	// every opcode appended after DBG look like a syscall.
	if (op >= OPEN && op <= DBG) return 3;  // syscall class
	printf("oisc4: unknown opcode %ld during sizing\n", op);
	exit(-1);
}

void emit (int s, int a, int d) {
	ws(oe, s); ws(oe + 8, a); ws(oe + 16, d);
	oe = oe + INSTB;
}

// Resolve the operand at code index i (the word after the opcode).
int operand (int i) {
	if (tr_ptype[i] == 1) return tr_map[tr_pval[i]];
	if (tr_ptype[i] == 2) return lay_database + tr_pval[i];
	return t_code[i];
}

// Emit the expansion of one c4 instruction. i = index of opcode word.
void translate_one (int i) {
	int op, n, b, v;
	op = t_code[i];
	b = oe;
	if (op == LEA) {
		emit(RBP, t_code[i + 1] * 8, RA);
	}
	else if (op == IMM) {
		emit(RZ, operand(i + 1), RA);
	}
	else if (op == JMP) {
		emit(RZ, operand(i + 1), RPC);
	}
	else if (op == JMPA) {
		emit(RA, 0, RPC);
	}
	else if (op == JSR) {
		emit(RSP, -8, RSP);
		emit(RSP, 0, b + 2 * INSTB + 16);   // patch dst of #3 = [SP]
		emit(RZ, b + 4 * INSTB, PH);        // push return address
		emit(RZ, operand(i + 1), RPC);
	}
	else if (op == JSRI) {
		// pc = *(global): operand resolves to an arena data address whose
		// word holds a code address (already OISC-mapped by the DCODE patch).
		emit(RSP, -8, RSP);
		emit(RSP, 0, b + 2 * INSTB + 16);
		emit(RZ, b + 4 * INSTB, PH);
		v = operand(i + 1);
		emit(v, 0, RPC);                    // src IS the global's address
	}
	else if (op == JSRS) {
		// pc = *(bp + n)
		emit(RSP, -8, RSP);
		emit(RSP, 0, b + 2 * INSTB + 16);
		emit(RZ, b + 5 * INSTB, PH);
		emit(RBP, t_code[i + 1] * 8, b + 4 * INSTB); // patch src of #5
		emit(PH, 0, RPC);
	}
	else if (op == BZ || op == BNZ) {
		v = operand(i + 1);
		emit(RA, 0, RA);                    // observe A -> flags
		emit(REQ0, b + 2 * INSTB, RPC);     // a==0: land on #4, else #3
		if (op == BZ) {
			emit(RZ, b + 4 * INSTB, RPC);   // a!=0: fall through
			emit(RZ, v, RPC);               // a==0: branch
		} else {
			emit(RZ, v, RPC);               // a!=0: branch
			emit(RZ, b + 4 * INSTB, RPC);   // a==0: fall through
		}
	}
	else if (op == ENT) {
		emit(RSP, -8, RSP);
		emit(RSP, 0, b + 2 * INSTB + 16);
		emit(RBP, 0, PH);                   // push bp
		emit(RSP, 0, RBP);                  // bp = sp
		emit(RSP, t_code[i + 1] * -8, RSP); // sp -= locals
	}
	else if (op == ADJ) {
		emit(RSP, t_code[i + 1] * 8, RSP);
	}
	else if (op == LEV) {
		emit(RBP, 0, RSP);                  // sp = bp
		emit(RSP, 0, b + 2 * INSTB);        // patch src of #3
		emit(PH, 0, RBP);                   // bp = [sp]
		emit(RSP, 8, b + 4 * INSTB);        // patch src of #5
		emit(PH, 0, RT0);                   // t0 = [sp+8] (return pc)
		emit(RSP, 16, RSP);
		emit(RT0, 0, RPC);
	}
	else if (op == LI) {
		emit(RA, 0, b + 1 * INSTB);         // patch src of #2
		emit(PH, 0, RA);                    // a = [a]
	}
	else if (op == LC) {
		emit(RA, 0, b + 2 * INSTB);         // patch src of #3
		emit(RZ, 1, RWR);
		emit(PH, 0, RA);                    // a = signed byte [a]
		emit(RZ, 0, RWR);
	}
	else if (op == SI) {
		emit(RSP, 0, b + 1 * INSTB);        // patch src of #2
		emit(PH, 0, RT0);                   // t0 = [sp] (target address)
		emit(RSP, 8, RSP);
		emit(RT0, 0, b + 4 * INSTB + 16);   // patch dst of #5
		emit(RA, 0, PH);                    // [t0] = a
	}
	else if (op == SC) {
		emit(RSP, 0, b + 1 * INSTB);
		emit(PH, 0, RT0);                   // t0 = [sp]
		emit(RSP, 8, RSP);
		emit(RT0, 0, b + 5 * INSTB + 16);   // patch dst of #6
		emit(RZ, 1, RWW);
		emit(RA, 0, PH);                    // byte [t0] = a
		emit(RZ, 0, RWW);
		emit(RT0, 0, b + 9 * INSTB);        // patch src of #10
		emit(RZ, 1, RWR);
		emit(PH, 0, RA);                    // a = signed byte [t0] (c4: a = *(char*)... = a)
		emit(RZ, 0, RWR);
	}
	else if (op == PSH) {
		emit(RSP, -8, RSP);
		emit(RSP, 0, b + 2 * INSTB + 16);   // patch dst of #3
		emit(RA, 0, PH);                    // [sp] = a
	}
	else if (op >= OR && op <= MOD) {
		emit(RSP, 0, b + 1 * INSTB);        // patch src of #2
		emit(PH, 0, P_MATHX);               // X = [sp]
		emit(RSP, 8, RSP);
		emit(RA, 0, P_MATHY);               // Y = a
		emit(RZ, op - OR, P_MATHOP);        // compute
		emit(P_MATHV, 0, RA);               // a = result
	}
	else if (op == _JMP) {
		emit(RSP, 0, b + 1 * INSTB);
		emit(PH, 0, RT0);                   // t0 = [sp]
		emit(RSP, 8, RSP);
		emit(RT0, 0, RPC);
	}
	else if (op == _ADJ) {
		// sp += *sp (word count -> bytes via math device)
		emit(RSP, 0, b + 1 * INSTB);
		emit(PH, 0, P_MATHX);               // X = [sp]
		emit(RZ, 8, P_MATHY);
		emit(RZ, MUL - OR, P_MATHOP);
		emit(P_MATHV, 0, b + 5 * INSTB + 8); // patch add of #6
		emit(RSP, PH, RSP);
	}
	else if (op == PRTF) {
		// c4m reads the argument count from the operand of the ADJ that
		// always follows PRTF. Same here, passed via R1.
		n = 0;
		if (i + 2 < t_codelen && t_code[i + 1] == ADJ) n = t_code[i + 2];
		emit(RZ, n, RR1);
		emit(RSP, 0, RR0);
		emit(RZ, PRTF, P_SYSCALL);
		emit(P_SYSCALL, 0, RA);
	}
	else if (op == FREE || op == EXIT) {
		emit(RSP, 0, RR0);
		emit(RZ, op, P_SYSCALL);
	}
	// -- the fused opcodes ------------------------------------------
	// Each is the concatenation of the expansions it replaces. The
	// self-patching offsets are counted from this block's start, which
	// is the only thing that changes when two blocks are run together.
	else if (op == LDL || op == LDG) {
		if (op == LDL) emit(RBP, t_code[i + 1] * 8, RA);
		else           emit(RZ, operand(i + 1), RA);
		emit(RA, 0, b + 2 * INSTB);         // patch src of #3
		emit(PH, 0, RA);                    // a = [a]
	}
	else if (op == PSHL || op == PSHG) {
		if (op == PSHL) emit(RBP, t_code[i + 1] * 8, RA);
		else            emit(RZ, operand(i + 1), RA);
		emit(RA, 0, b + 2 * INSTB);
		emit(PH, 0, RA);                    // a = [a]
		emit(RSP, -8, RSP);
		emit(RSP, 0, b + 5 * INSTB + 16);   // patch dst of #6
		emit(RA, 0, PH);                    // [sp] = a
	}
	else if (op == LEAP || op == IMMP) {
		if (op == LEAP) emit(RBP, t_code[i + 1] * 8, RA);
		else            emit(RZ, operand(i + 1), RA);
		emit(RSP, -8, RSP);
		emit(RSP, 0, b + 3 * INSTB + 16);   // patch dst of #4
		emit(RA, 0, PH);                    // [sp] = a
	}
	else if (op == LIP) {
		emit(RA, 0, b + 1 * INSTB);         // patch src of #2
		emit(PH, 0, RA);                    // a = [a]
		emit(RSP, -8, RSP);
		emit(RSP, 0, b + 4 * INSTB + 16);   // patch dst of #5
		emit(RA, 0, PH);                    // [sp] = a
	}
	else if (op == ADDL) {
		emit(RSP, 0, b + 1 * INSTB);        // patch src of #2
		emit(PH, 0, P_MATHX);               // X = [sp]
		emit(RSP, 8, RSP);
		emit(RA, 0, P_MATHY);               // Y = a
		emit(RZ, ADD - OR, P_MATHOP);
		emit(P_MATHV, 0, RA);               // a = [sp] + a
		emit(RA, 0, b + 7 * INSTB);         // patch src of #8
		emit(PH, 0, RA);                    // a = [a]
	}
	else if (op == STL) {
		emit(RBP, t_code[i + 1] * 8, b + 1 * INSTB + 16);  // patch dst of #2
		emit(RA, 0, PH);                    // [bp + n] = a
	}
	else if (op == POPA) {
		emit(RSP, 0, b + 1 * INSTB);        // patch src of #2
		emit(PH, 0, RA);                    // a = [sp]
		emit(RSP, 8, RSP);
	}
	else if (op >= OPEN && op <= DBG) { // remaining syscall class
		emit(RSP, 0, RR0);
		emit(RZ, op, P_SYSCALL);
		emit(P_SYSCALL, 0, RA);
	}
	else {
		printf("oisc4: unknown opcode %ld at code[%ld]\n", op, i);
		exit(-1);
	}
}

// Number of OISC instructions in the startup stub.
int stub_insts () {
	return 9 * t_conslen + 13 + 1 + 4 * t_deslen + 1 + 3 + 2;
}

// Emit the startup stub at lay_codebase: run constructors, call
// main(argc, argv), run destructors, push main's result, EXIT.
void emit_stub (int argc, int argvbase) {
	int i, b;
	oe = lay_codebase;
	i = 0;
	while (i < t_conslen) {
		// constructor(0)  (load-c4r passes the c4r struct; we pass 0)
		emit(RZ, 0, RA);                        // IMM 0
		b = oe; emit(RSP, -8, RSP);             // PSH
		emit(RSP, 0, b + 2 * INSTB + 16);
		emit(RA, 0, PH);
		b = oe; emit(RSP, -8, RSP);             // JSR cons
		emit(RSP, 0, b + 2 * INSTB + 16);
		emit(RZ, b + 4 * INSTB, PH);
		emit(RZ, tr_map[t_cons[i]], RPC);
		emit(RSP, 8, RSP);                      // ADJ 1
		++i;
	}
	// push argc
	emit(RZ, argc, RA);
	b = oe; emit(RSP, -8, RSP);
	emit(RSP, 0, b + 2 * INSTB + 16);
	emit(RA, 0, PH);
	// push argv
	emit(RZ, argvbase, RA);
	b = oe; emit(RSP, -8, RSP);
	emit(RSP, 0, b + 2 * INSTB + 16);
	emit(RA, 0, PH);
	// call main
	b = oe; emit(RSP, -8, RSP);
	emit(RSP, 0, b + 2 * INSTB + 16);
	emit(RZ, b + 4 * INSTB, PH);
	emit(RZ, tr_map[t_entry], RPC);
	// pop args
	emit(RSP, 16, RSP);
	// save return value
	emit(RA, 0, RR2);
	// destructors
	i = 0;
	while (i < t_deslen) {
		b = oe; emit(RSP, -8, RSP);
		emit(RSP, 0, b + 2 * INSTB + 16);
		emit(RZ, b + 4 * INSTB, PH);
		emit(RZ, tr_map[t_des[i]], RPC);
		++i;
	}
	// restore return value, push, exit
	emit(RR2, 0, RA);
	b = oe; emit(RSP, -8, RSP);
	emit(RSP, 0, b + 2 * INSTB + 16);
	emit(RA, 0, PH);
	emit(RSP, 0, RR0);
	emit(RZ, EXIT, P_SYSCALL);
}

// Full translation: sizes, layout, patches, emission.
// argc/argv: program arguments to copy into the arena.
int translate (int argc, char **argv) {
	int i, j, op, sz, p, ptype, paddr, pvalu, a, need;
	char *s;

	// Patch classification for code words
	if (!(tr_ptype = malloc(8 * (t_codelen + 2)))) fatal("out of memory (ptype)");
	if (!(tr_pval  = malloc(8 * (t_codelen + 2)))) fatal("out of memory (pval)");
	if (!(tr_map   = malloc(8 * (t_codelen + 2)))) fatal("out of memory (map)");
	i = 0; while (i <= t_codelen + 1) { tr_ptype[i] = 0; tr_map[i] = 0; ++i; }

	p = 0;
	while (p < t_patchlen) {
		ptype = t_patches[p * 3];
		paddr = t_patches[p * 3 + 1];
		pvalu = t_patches[p * 3 + 2];
		if (ptype == C4R_PTYPE_CODE) { tr_ptype[paddr] = 1; tr_pval[paddr] = pvalu; }
		else if (ptype == C4R_PTYPE_DATA) { tr_ptype[paddr] = 2; tr_pval[paddr] = pvalu; }
		// DCODE/DDATA applied after layout below
		++p;
	}

	// Pass 1: sizes -> map. Code word 0 is never an instruction: the c4
	// emitter writes via *++e, so streams are 1-based.
	lay_codebase = MEM_BASE;
	a = lay_codebase + stub_insts() * INSTB;
	i = 1;
	while (i < t_codelen) {
		op = t_code[i];
		tr_map[i] = a;
		a = a + o4_size(op) * INSTB;
		i = i + (has_operand(op) ? 2 : 1);
	}
	tr_map[i] = a; // one past the end
	lay_codeend = a;

	// Layout: data, argv, heap, stack
	lay_database = align8(lay_codeend) + 64;
	// reserve the full in-memory data size (t_memsz) so the BSS tail
	// beyond t_datalen -- zero in the zeroed arena -- is not overrun by
	// the argv/heap regions placed after it.
	lay_argvbase = align8(lay_database + t_memsz) + 64;
	// argv: argc words, then the strings
	need = argc * 8;
	i = 0; while (i < argc) { s = argv[i]; while (*s++) ++need; ++need; ++i; }
	lay_heapbase = align8(lay_argvbase + need) + 64;
	lay_stacktop = vm_size - 64;
	if (lay_heapbase + STACK_RESERVE >= vm_size)
		fatal("arena too small for image; rerun with -m <megs>");

	// Copy data segment into the arena
	memcpy(vm_m + lay_database, t_data, t_datalen);

	// Apply data-resident patches (need tr_map, have it now)
	p = 0;
	while (p < t_patchlen) {
		ptype = t_patches[p * 3];
		paddr = t_patches[p * 3 + 1];
		pvalu = t_patches[p * 3 + 2];
		if (ptype == C4R_PTYPE_DCODE) ws(lay_database + paddr, tr_map[pvalu]);
		else if (ptype == C4R_PTYPE_DDATA) ws(lay_database + paddr, lay_database + pvalu);
		++p;
	}

	// Copy argv into the arena
	a = lay_argvbase + argc * 8;
	i = 0;
	while (i < argc) {
		ws(lay_argvbase + i * 8, a);
		s = argv[i];
		while (*s) { vm_m[a++] = *s++; }
		vm_m[a++] = 0;
		++i;
	}

	// Pass 2: emit translated code
	oe = lay_codebase + stub_insts() * INSTB;
	i = 1;
	while (i < t_codelen) {
		op = t_code[i];
		if (oe != tr_map[i]) {
			printf("oisc4: BUG pass1/pass2 divergence at code[%ld]: %ld != %ld\n", i, oe, tr_map[i]);
			exit(-1);
		}
		translate_one(i);
		i = i + (has_operand(op) ? 2 : 1);
	}
	if (oe != lay_codeend) fatal("BUG: pass2 did not land on pass1 end");

	// Emit the startup stub
	emit_stub(argc, lay_argvbase);
	if (oe != lay_codebase + stub_insts() * INSTB)
		fatal("BUG: stub size mismatch");

	// Registers
	ws(RSP, lay_stacktop);
	ws(RBP, lay_stacktop);
	ws(RPC, lay_codebase);
	heap_brk  = lay_heapbase;
	heap_top  = vm_size - STACK_RESERVE;
	heap_free = 0;

	if (opt_verbose) {
		printf("oisc4: code %ld c4 words -> %ld oisc instructions (%ld bytes, x%ld expansion)\n",
			t_codelen, (lay_codeend - lay_codebase) / INSTB,
			lay_codeend - lay_codebase, (lay_codeend - lay_codebase) / (t_codelen * 8 + 1));
		printf("oisc4: data at %ld (%ld bytes), heap at %ld, stack top %ld\n",
			lay_database, t_datalen, lay_heapbase, lay_stacktop);
	}
	return 0;
}

void disasm () {
	int a;
	a = lay_codebase;
	printf("oisc4: translated code (%ld instructions):\n", (lay_codeend - lay_codebase) / INSTB);
	while (a < lay_codeend) {
		dbg_inst(a, wg(a), wg(a + 8), wg(a + 16));
		printf("\n");
		a = a + INSTB;
	}
}

//////
// Main
//////

int main (int argc, char **argv) {
	int megs, r;
	char *arg;

	megs = DEFAULT_MEGS;
	--argc; ++argv;
	while (argc > 0 && **argv == '-') {
		arg = *argv;
		if (arg[1] == '-') { --argc; ++argv; break; }
		else if (arg[1] == 'd') opt_trace = 1;
		else if (arg[1] == 's') opt_disasm = 1;
		else if (arg[1] == 'v') opt_verbose = 1;
		else if (arg[1] == 'm') {
			--argc; ++argv;
			if (argc <= 0) fatal("-m needs a size in MB");
			megs = 0; arg = *argv;
			while (*arg >= '0' && *arg <= '9') megs = megs * 10 + (*arg++ - '0');
			if (megs < 8) megs = 8;
		}
		else if (arg[1] == 'c') {
			--argc; ++argv;
			if (argc <= 0) fatal("-c needs a cycle count");
			opt_maxcycles = 0; arg = *argv;
			while (*arg >= '0' && *arg <= '9') opt_maxcycles = opt_maxcycles * 10 + (*arg++ - '0');
		}
		else {
			printf("oisc4: unknown option '%s'\n", arg);
			return -1;
		}
		--argc; ++argv;
	}
	if (argc < 1) {
		printf("usage: oisc4 [-d] [-s] [-v] [-m megs] [--] program.c4r [args...]\n");
		return -1;
	}

	vm_size = megs * 1024 * 1024;
	if (!(vm_m = malloc(vm_size))) fatal("cannot allocate arena");
	memset(vm_m, 0, vm_size);
	if (!(o4p_buf = malloc(80))) fatal("cannot allocate printf buffer");

	if (load_c4r(*argv)) return -1;
	if (translate(argc, argv)) return -1;

	if (opt_disasm) { disasm(); return 0; }

	r = vm_run();
	if (opt_verbose) printf("oisc4: exit(%ld) after %ld cycles\n", r, vm_cycle);
	return r;
}
