// devices.js - memory-mapped peripherals of the c4bb machine.
//
// The device bus occupies 0x100-0x1FF, one 32-bit register per slot.
// All host interaction (console, clock, halting) happens here and only
// here; the CPU microcode just reads and writes these addresses.
//
// Determinism: there is no wall clock. TIME_MS is derived from the
// cycle counter (CYCLES_PER_MS cycles = 1 simulated millisecond) plus
// an offset that USLP advances, so a run is a pure function of its
// inputs and every run of the same image produces identical timing.

export const UART_TX   = 0x100;
export const UART_RX   = 0x104;
export const UART_RXAVL= 0x108;
export const TIME_MS   = 0x10c;
export const CYCLE_LO  = 0x110;
export const CYCLE_HI  = 0x114;
export const USLP_US   = 0x118;
export const DISK_NAME = 0x120;
export const DISK_FD   = 0x124;
export const DISK_ADDR = 0x128;
export const DISK_LEN  = 0x12c;
export const DISK_CLOSE= 0x130;
export const OPNAME    = 0x134;
export const DISK_FLAGS= 0x138;
export const POWER     = 0x140;
export const HEAP_BASE = 0x144;
export const HEAP_END  = 0x148;
export const INFO_REG  = 0x14c;
// CPU control latches, visible to the trap/TLEV microcode (proxied to
// machine state so both engines and the renderer agree):
export const TRAPH_REG    = 0x150;  // ITH latch (trap handler address)
export const INTERVAL_REG = 0x154;  // cycle-interrupt interval (0 = masked)
export const TRESTORE_REG = 0x15c;  // CONF_TRAP_RESTORES_INTERVAL
export const MODE_REG     = 0x160;  // 0 unprotected / 1 protected
export const TT_REG       = 0x164;  // jam latch: trap type
export const TP_REG       = 0x168;  // jam latch: trap parameter
export const HND_REG      = 0x16c;  // jam latch: handler address
// state snapshot taken by the jam BEFORE call-site effects: c4m's
// trap() pushes the interval and mode as they were when the trap was
// raised (c4m.c:1121,1132); the sites zero/drop them afterwards
export const JMODE_REG    = 0x170;  // mode at jam time
export const JINTERVAL_REG= 0x174;  // interval at jam time

// c4_info() capability bits (c4m.c:206)
export const C4I_C4M = 0x2, C4I_HRT = 0x10, C4I_SIG = 0x20,
             C4I_FLT = 0x40, C4I_PROT = 0x80, C4I_TRAPH = 0x400;

export const CYCLES_PER_MS = 1000;   // a 1 MHz machine

// Opcode-name ROM contents, 5 bytes per opcode, exactly as
// c4m_setup_opcodes lays them out (c4m.c:278, c4l.c:110).
export const OPNAMES =
  'LEA ,IMM ,JMP ,JSR ,BZ  ,BNZ ,ENT ,ADJ ,LEV ,LI  ,LC  ,SI  ,SC  ,PSH ,' +
  'OR  ,XOR ,AND ,EQ  ,NE  ,LT  ,GT  ,LE  ,GE  ,SHL ,SHR ,ADD ,SUB ,MUL ,DIV ,MOD ,' +
  'OPEN,READ,CLOS,PRTF,MALC,FREE,MSET,MCMP,EXIT,' +
  'PUTC,PUTS,RALC,MCPY,STRC,ITH ,_OPC,_BLT,_TRP,OPCD,' +
  '_JMP,_ADJ,C4CF,C4CY,TIME,SIGH,SIGI,USLP,INFO,OPSL,' +
  'C4IV,FLT ,JSRI,JSRS,JMPA,TLEV,DBG ,' +
  // c4mp's (66-78), named so a disassembler names them; this board does
  // not execute them, exactly as c4m names them without executing them.
  'CPUI,CPUN,CPUS,CPUH,' +
  'CAS ,XCHG,FADD,CWAI,CWAK,IPI ,' +
  'LXI ,SXI ,TRAW,' +
  // fused (79-88), docs/fused-opcodes.md. LDL, STL and POPA are real
  // here (hw/microcode.uc); the other seven are c4mp's and named only.
  'LDL ,LDG ,PSHL,PSHG,LEAP,IMMP,LIP ,ADDL,STL ,POPA,';

// Disk controller fd space: 0 is the blocking, line-buffered keyboard
// (a terminal in cooked mode); opening "/dev/stdin" with O_NONBLOCK
// (0x800) yields a nonblocking byte fd whose empty read returns -1,
// the Linux EAGAIN convention C4IX's console expects (con.c:53).
// Opening "/dev/tty" instead yields a RAW keyboard fd: same EAGAIN
// convention, but no line discipline, so a keystroke is readable the
// instant it arrives instead of when Enter is pressed. That is the
// difference between a shell and a game; /dev/stdin's behaviour is
// deliberately untouched so c4sh, C4IX's console and every existing
// golden keep the cooked semantics they were written against.
// The name is chosen because it already means this on a real host:
// /dev/tty is the controlling terminal, so the identical guest code
// works under native c4m, where rawness comes from stty outside.
// Real files come from a caller-supplied name -> Uint8Array map.
// READ returns -2 for "would block"; the microcode rewinds PC by one
// word and retries, so the machine keeps taking interrupts while a
// task waits for input (the C4IX blocking-syscall convention).
const O_NONBLOCK = 0x800;
const FD_KEYBOARD = 0, FD_FIRST = 3;

export class Devices {
  constructor(arena, opts = {}) {
    this.arena = arena;
    this.machine = null;              // set by machine.js on attach
    this.onByte = opts.onByte || null;// UART TX sink: fn(byteValue)
    this.rxFifo = [];                 // keyboard bytes
    this.rxEof = false;               // Ctrl-D / end of input stream
    this.rawKbd = 0;                  // open raw keyboards; a host UI
                                      // suppresses local echo while > 0
    this.sleepMs = 0;                 // accumulated USLP time
    this.halted = false;
    this.status = 0;
    this.heapBase = 0;
    this.heapEnd = 0;
    this.uartActivity = 0;            // counters for the renderer
    this.diskActivity = 0;
    this.files = opts.files || new Map();   // name -> Uint8Array
    this.fds = new Map();             // fd -> {data, pos} | {kbd, nonblock}
    this.nextFd = FD_FIRST;
    this.diskFlags = 0;
    this.diskFd = 0;
    this.diskAddr = 0;
    this.diskResult = 0;
  }

  diskOpen(nameAddr) {
    const name = this.arena.cstring(nameAddr >>> 0, 256);
    if (name === '/dev/stdin' || name === '/dev/tty') {
      const fd = this.nextFd++;
      const raw = name === '/dev/tty';
      this.fds.set(fd, { kbd: true, raw, nonblock: !!(this.diskFlags & O_NONBLOCK) });
      if (raw) this.rawKbd++;
      return fd;
    }
    const clean = name.startsWith('./') ? name.slice(2) : name;
    let data = this.files.get(clean) ?? this.files.get(name);
    // The disk is flat, but ls/cat show the vfs.txt tree's hierarchical
    // paths (e.g. "/home/user/hello.c") since that's what ramfs holds -
    // real disk-I/O tools like c4cc never learned that vocabulary and
    // ask for the path verbatim. Falling back to the basename here,
    // once, transparently to every guest program, means "cat" showing
    // you a path is also a path you can hand to any other tool.
    if (!data && name.includes('/')) {
      const base = name.slice(name.lastIndexOf('/') + 1);
      data = this.files.get(base);
    }
    if (!data) return -1;
    const fd = this.nextFd++;
    this.fds.set(fd, { data, pos: 0 });
    return fd;
  }

  // Canonical-mode line buffering, unconditionally: a real terminal's
  // line discipline holds a line in its own edit buffer (where local
  // echo and backspace happen) and does not release ANY of it to a
  // reader - blocking or non-blocking - until Enter or EOF, no matter
  // which fd is doing the reading. Skipping this for the non-blocking
  // path (as an earlier version did) let a fast reader in turbo mode
  // drain keystrokes out of rxFifo faster than a human could press
  // Backspace, breaking line editing; waiting for \n here is what
  // keeps not-yet-committed characters available to erase.
  kbdRead(addr, len, nonblock, raw) {
    if (this.rxFifo.length === 0) return this.rxEof ? 0 : (nonblock ? -1 : -2);
    // A raw fd is the one caller that has opted out of line buffering,
    // so it takes whatever has arrived. Everyone else still waits for
    // the newline that commits the line.
    if (!raw) {
      const nl = this.rxFifo.indexOf(10);
      if (nl < 0 && !this.rxEof) return nonblock ? -1 : -2;
    }
    let n = 0;
    while (n < len && this.rxFifo.length) {
      const b = this.rxFifo.shift();
      this.arena.write8(addr + n, b);
      n++;
      if (!nonblock && b === 10) break;    // one line per read
    }
    return n;
  }

  diskRead(len) {
    this.diskActivity++;
    const f = this.fds.get(this.diskFd);
    if (this.diskFd === FD_KEYBOARD) return this.kbdRead(this.diskAddr, len, false);
    if (!f) return -1;
    if (f.kbd) return this.kbdRead(this.diskAddr, len, f.nonblock, f.raw);
    const n = Math.min(len, f.data.length - f.pos);
    for (let i = 0; i < n; i++) this.arena.write8(this.diskAddr + i, f.data[f.pos + i]);
    f.pos += n;
    return n;
  }

  simMs() {
    const cyc = this.machine ? this.machine.cycle : 0;
    return (Math.floor(cyc / CYCLES_PER_MS) + this.sleepMs) | 0;
  }

  read32(addr) {
    switch (addr) {
      case UART_RX:    return this.rxFifo.length ? this.rxFifo.shift() : -1;
      case UART_RXAVL: return this.rxFifo.length;
      case TIME_MS:    return this.simMs();
      case CYCLE_LO:   return this.machine ? (this.machine.cycle | 0) : 0;
      case CYCLE_HI:   return this.machine ? (this.machine.cycle / 0x100000000) | 0 : 0;
      case OPNAME:     return this.opnameResult | 0;
      case HEAP_BASE:  return this.heapBase | 0;
      case HEAP_END:   return this.heapEnd | 0;
      case INFO_REG: {
        // mirrors native c4_info() | TRAPH (c4m.c:1033, 1814)
        const m = this.machine;
        return C4I_C4M | C4I_HRT | C4I_SIG | C4I_FLT | C4I_PROT |
               (m && m.trapHandler ? C4I_TRAPH : 0);
      }
      case TRAPH_REG:    return this.machine.trapHandler | 0;
      case INTERVAL_REG: return this.machine.cycleInterval | 0;
      case TRESTORE_REG: return this.machine.trapRestoresInterval | 0;
      case MODE_REG:     return this.machine.mode | 0;
      case TT_REG:       return this.machine.tt | 0;
      case TP_REG:       return this.machine.tp | 0;
      case HND_REG:      return this.machine.hnd | 0;
      case JMODE_REG:    return this.machine.jmode | 0;
      case JINTERVAL_REG:return this.machine.jinterval | 0;
      case DISK_NAME:    return this.diskResult | 0;
      case DISK_LEN:     return this.diskResult | 0;
      case DISK_CLOSE:   return this.diskResult | 0;
      default:         return 0;
    }
  }

  write32(addr, val) {
    switch (addr) {
      case UART_TX:
        this.uartActivity++;
        if (this.onByte) this.onByte(val & 0xff);
        return;
      case USLP_US:
        // usleep(val): advance simulated time; also count sub-ms sleeps
        this.sleepMs += Math.max(1, Math.floor(val / 1000));
        return;
      case OPNAME: {
        // Written a pointer to a NUL/space-terminated name; look it up
        // the way c4m's __opcode does: case-insensitive 4-char match.
        this.opnameResult = this.opcodeByName(this.arena.cstring(val >>> 0, 16));
        return;
      }
      case DISK_FLAGS: this.diskFlags = val | 0; return;
      case DISK_NAME:  this.diskResult = this.diskOpen(val); return;
      case DISK_FD:    this.diskFd = val | 0; return;
      case DISK_ADDR:  this.diskAddr = val | 0; return;
      case DISK_LEN:   this.diskResult = this.diskRead(val | 0); return;
      case DISK_CLOSE: {
        const f = this.fds.get(val | 0);
        if (f) { if (f.raw) this.rawKbd--; this.fds.delete(val | 0); this.diskResult = 0; }
        else this.diskResult = -1;
        return;
      }
      case POWER:
        this.halted = true;
        this.status = val | 0;
        return;
      case HEAP_BASE: this.heapBase = val | 0; return;
      case HEAP_END:  this.heapEnd = val | 0;  return;
      case TRAPH_REG:    this.machine.trapHandler = val | 0; return;
      case INTERVAL_REG: this.machine.cycleInterval = val | 0; return;
      case TRESTORE_REG: this.machine.trapRestoresInterval = val | 0; return;
      case MODE_REG:     this.machine.mode = val | 0; return;
      case TT_REG:       this.machine.tt = val | 0; return;
      case TP_REG:       this.machine.tp = val | 0; return;
      case HND_REG:      this.machine.hnd = val | 0; return;
      default: return;
    }
  }

  opcodeByName(name) {
    const want = (name + '    ').slice(0, 4).toUpperCase();
    for (let i = 0; i * 5 + 4 <= OPNAMES.length; i++) {
      if (OPNAMES.slice(i * 5, i * 5 + 4).toUpperCase() === want) return i;
    }
    return -1;
  }

  pushInput(str) {
    for (const ch of new TextEncoder().encode(str)) this.rxFifo.push(ch);
  }
}
