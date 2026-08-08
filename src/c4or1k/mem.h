// c4or1k guest RAM.
//
// A flat byte array with explicit big-endian composition, not an
// int*-cast-over-bytes trick (c4 has no such trick to exploit anyway
// -- everything is explicit array indexing). Every load zero-extends
// by construction (each byte is masked &0xFF before being shifted
// in); sign extension for l.lbs/l.lhs happens in cpu.c via sext(),
// not here.
//
// M1 has no MMU and no MMIO dispatch table yet (that's M3): addr is
// masked into RAM_SIZE (wrapping, not faulting) rather than routed
// through a device map. Real physical-address bounds and MMIO come
// later; this is only ever fed hand-assembled test-program addresses
// for now.
//
// ram is malloc'd by mem_init(), not a static array: c4lc's data
// segment is capped at 256KB (g:DMAX in c4lc-gen.lisp, shared across
// every global in the whole linked program) -- a `char ram[RAM_SIZE]`
// static array fails to compile ("data segment full") at any size
// that matters, and M4 needs room for a 5.7MB vmlinux.bin regardless.
// Call mem_init() once before any ram_* access.

// c4lc's enum initializers must be a plain numeric literal, not an
// expression -- 0x100000, not "1 << 20".
//
// 32MB, matching jor1k's own default `memorysize` (index.js) -- not
// just "plenty for hand-written test programs" from M4 on: this is
// what a real vmlinux.bin actually needs room to boot into.
enum { RAM_SIZE = 0x2000000 };

extern char *ram;

void mem_init();
int ram_lw(int addr);
int ram_lh(int addr);
int ram_lb(int addr);
void ram_sw(int addr, int val);
void ram_sh(int addr, int val);
void ram_sb(int addr, int val);
void ram_dump(int base, int nwords);
