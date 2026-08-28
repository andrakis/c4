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
// Drives (docs/c4bb-storage.md). The read side above is unchanged and
// still talks to whichever drive is selected; these say WHICH, how many
// there are, and whether the one selected can be written to.
export const DISK_DRIVE= 0x13c;  // r/w: selected drive, 0-based
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

// The write head. Deliberately the same shape as the read side, so a
// guest that can read a file can write one by learning four addresses
// and no new opcode -- fw.c already drives DEV_INTERVAL this way.
export const DISK_WNAME  = 0x178;  // w: create/truncate -> fd, or -1
export const DISK_WADDR  = 0x17c;  // w: buffer address
export const DISK_WLEN   = 0x180;  // w: write N bytes -> N written
export const DISK_WCLOSE = 0x184;  // w: close and flush -> 0, or -1
export const DISK_COUNT  = 0x188;  // r: how many drives are attached
export const DISK_RO     = 0x18c;  // r: 1 if the selected drive is read-only
export const DISK_EJECT  = 0x190;  // w: empty the drive whose number is written
export const DISK_RESCAN = 0x194;  // w: re-read the drive whose number is written
export const RESET       = 0x198;  // w: soft reset -- back to the BIOS
// Clocks. TIME_MS above is the machine's own, derived from the cycle
// counter and therefore repeatable; these two are about the world
// outside it, which is not. Registers rather than opcodes on purpose:
// a program at the base-c4 rung can read a clock with an ordinary load
// and does not need the CPU extended to do it.
export const RTC_MS      = 0x19c;  // r: host milliseconds since power-on
export const PIT_MS      = 0x1a0;  // r/w: tick every N real ms (0 = off)

// c4_info() capability bits (c4m.c:206)
export const C4I_C4M = 0x2, C4I_HRT = 0x10, C4I_SIG = 0x20,
             C4I_FLT = 0x40, C4I_PROT = 0x80, C4I_TRAPH = 0x400,
// This machine has the clock and timer registers (RTC_MS, PIT_MS). A
// kernel cannot just poke 0x19c to find out, because native c4m has no
// device window there and the poke would be a wild access -- so the
// capability is announced the way every other one is, and a kernel that
// does not see the bit keeps counting cycles.
             C4I_PIT = 0x800;

// How fast the machine thinks it is. This was 1000 -- a 1 MHz machine --
// while the simulator actually executes fifteen to twenty million
// instructions a second, so a simulated second went past in a
// twentieth of a real one and `top`, which refreshes once a second,
// redrew twenty times. 20 MHz is roughly what this simulator really
// manages, so a simulated second is about a real one; cli.js -hz
// changes it, and the interactive loop paces execution to it so the
// match is exact rather than approximate.
export const CYCLES_PER_MS = 20000;

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
    // Drives (docs/c4bb-storage.md). One machine, several media: drive
    // 0 is what `files` used to be and every existing caller still gets
    // exactly that. A drive is { files, writable, sink } -- sink is
    // whatever the host does with a written file (a directory on the
    // CLI, an image in the browser); a drive with no sink is read-only
    // no matter what it says.
    this.drives = opts.drives ||
                  [{ files: opts.files || new Map(), writable: false, sink: null }];
    this.drive = 0;                   // the selected drive
    this.onRescan = opts.onRescan || null;
    this.resetRequested = false;
    this.cyclesPerMs = opts.cyclesPerMs || CYCLES_PER_MS;
    // The real clock. hostNow is injectable so a test can pin it; by
    // default it is the wall clock, which is the whole point.
    this.hostNow = opts.hostNow || (() => Date.now());
    this.t0 = this.hostNow();
    this.pitMs = 0;                   // 0 = the PIT is off (or masked)
    this.pitArmed = 0;                // the interval last armed, masked or not
    this.pitNext = 0;                 // when the next tick is due, host ms
    this.fds = new Map();             // fd -> {data, pos} | {kbd, nonblock} | {w}
    this.nextFd = FD_FIRST;
    this.diskFlags = 0;
    this.diskFd = 0;
    this.diskAddr = 0;
    this.diskResult = 0;
  }

  // What `files` meant before there was more than one drive. Kept as a
  // property so nothing that reaches for dev.files has to know.
  get files () { return this.drives[this.drive] ? this.drives[this.drive].files : new Map(); }

  // "1:name" says which drive; anything else means the selected one.
  // C4DOS already thinks in drive letters, and a prefix costs the
  // machine nothing -- no register to set, no state to get wrong.
  resolveDrive (name) {
    if (name.length > 1 && name[1] === ':') {
      const d = name.charCodeAt(0);
      let n = -1;
      if (d >= 48 && d <= 57) n = d - 48;               // "0:".."9:"
      else if (d >= 65 && d <= 90) n = d - 65;          // "A:".."Z:"
      else if (d >= 97 && d <= 122) n = d - 97;         // "a:".."z:"
      if (n >= 0) return { drv: n, rest: name.slice(2) };
    }
    return { drv: this.drive, rest: name };
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
    const { drv, rest } = this.resolveDrive(name);
    const media = this.drives[drv];
    if (!media) return -1;
    const files = media.files;
    const clean = rest.startsWith('./') ? rest.slice(2) : rest;
    let data = files.get(clean) ?? files.get(rest);
    // The disk is flat, but ls/cat show the vfs.txt tree's hierarchical
    // paths (e.g. "/home/user/hello.c") since that's what ramfs holds -
    // real disk-I/O tools like c4cc never learned that vocabulary and
    // ask for the path verbatim. Falling back to the basename here,
    // once, transparently to every guest program, means "cat" showing
    // you a path is also a path you can hand to any other tool.
    if (!data && rest.includes('/')) {
      const base = rest.slice(rest.lastIndexOf('/') + 1);
      data = files.get(base);
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

  // Create or truncate a file on a drive. A drive with no sink is a
  // pressed disc: it can be read forever and never written.
  diskCreate (nameAddr) {
    const name = this.arena.cstring(nameAddr >>> 0, 256);
    const { drv, rest } = this.resolveDrive(name);
    const media = this.drives[drv];
    if (!media || !media.writable || !media.sink) return -1;
    const fd = this.nextFd++;
    this.fds.set(fd, { w: { media, name: rest, chunks: [], len: 0 } });
    return fd;
  }

  diskWrite (len) {
    this.diskActivity++;
    const f = this.fds.get(this.diskFd);
    if (!f || !f.w) return -1;
    const n = Math.max(0, len | 0);
    const buf = this.arena.u8.slice(this.diskAddr >>> 0, (this.diskAddr >>> 0) + n);
    f.w.chunks.push(buf);
    f.w.len += n;
    return n;
  }

  // Close is where a written file becomes real, in one piece: a half
  // written medium after a crash is a bug report nobody can read.
  diskWClose (fd) {
    const f = this.fds.get(fd | 0);
    if (!f || !f.w) return -1;
    const all = new Uint8Array(f.w.len);
    let at = 0;
    for (const c of f.w.chunks) { all.set(c, at); at += c.length; }
    f.w.media.files.set(f.w.name, all);
    this.fds.delete(fd | 0);
    try { f.w.media.sink(f.w.name, all); } catch { return -1; }
    return 0;
  }

  simMs() {
    const cyc = this.machine ? this.machine.cycle : 0;
    return (Math.floor(cyc / this.cyclesPerMs) + this.sleepMs) | 0;
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
        return C4I_C4M | C4I_HRT | C4I_SIG | C4I_FLT | C4I_PROT | C4I_PIT |
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
      case RTC_MS:       return (this.hostNow() - this.t0) | 0;
      case PIT_MS:       return this.pitMs | 0;
      case DISK_DRIVE:   return this.drive | 0;
      case DISK_COUNT:   return this.drives.length | 0;
      case DISK_RO: {
        const m = this.drives[this.drive];
        return (m && m.writable && m.sink) ? 0 : 1;
      }
      case DISK_WNAME:   return this.diskResult | 0;
      case DISK_WLEN:    return this.diskResult | 0;
      case DISK_WCLOSE:  return this.diskResult | 0;
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
      // Ask for a tick every N real milliseconds. The machine raises the
      // same trap the cycle interrupt does, so a kernel that already
      // has a handler needs no new one -- it just stops having to guess
      // how many cycles a second is on this host.
      //
      // Arming, re-arming and MASKING are three different writes, and
      // what tells them apart is the phase. A kernel hides from its own
      // timer in every critical path -- 0 on the way in, the interval
      // on the way out -- so if writing the interval always restarted
      // the countdown, a kernel that entered a critical path more often
      // than once an interval would never tick again. It would mask
      // itself to death.
      //
      // So: 0 stops the timer and LEAVES THE DEADLINE WHERE IT IS, and
      // re-arming the same interval resumes toward that deadline. Only
      // a genuinely new interval starts a new countdown. The cycle
      // interrupt gets this for free, because its counter is
      // free-running and `cycle % interval` does not care when the
      // interval was written; the PIT has to be told.
      case PIT_MS: {
        const ms = Math.max(0, val | 0);
        if (!ms) { this.pitMs = 0; return; }          // masked, phase kept
        if (ms !== this.pitArmed || !this.pitNext)
          this.pitNext = this.hostNow() + ms;         // a new countdown
        this.pitMs = this.pitArmed = ms;
        return;
      }
      case DISK_DRIVE:
        // Out of range selects nothing rather than crashing: a program
        // asking for drive 3 on a two-drive machine should see empty
        // media, which is what an empty drive looks like anyway.
        this.drive = (val | 0) >= 0 ? (val | 0) : 0;
        return;
      case DISK_WNAME:  this.diskResult = this.diskCreate(val); return;
      case DISK_WADDR:  this.diskAddr = val | 0; return;
      case DISK_WLEN:   this.diskResult = this.diskWrite(val | 0); return;
      case DISK_WCLOSE: this.diskResult = this.diskWClose(val | 0); return;
      case DISK_EJECT: {
        const m = this.drives[val | 0];
        if (m) { m.files = new Map(); m.ejected = true; }
        return;
      }
      // The BIOS's retry loop is only useful if a disk put in while it
      // waits is actually seen, and the host read the medium once at
      // start. This is the machine asking to look again.
      case DISK_RESCAN:
        if (this.onRescan) this.onRescan(val | 0);
        return;
      // A soft reset. An operating system that has just written a boot
      // disk has no other way to say "start again and find it" -- the
      // alternative is asking the player to stop the machine and start
      // it, which on a homebrew computer is a different thing entirely.
      // Halts like POWER, and the host boots the firmware again.
      case RESET:
        this.pitMs = 0;
        this.pitArmed = 0;
        this.pitNext = 0;
        this.resetRequested = true;
        this.halted = true;
        this.status = val | 0;
        return;
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
