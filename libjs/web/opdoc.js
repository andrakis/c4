// opdoc.js - what each C4 instruction does, for the Code Explorer's
// tooltips (explore.js), the way Compiler Explorer explains an x86
// mnemonic on hover.
//
// The machine has one register, A, beside pc, sp and bp. sp and bp count
// in bytes, but a frame offset (LEA's, ENT's, LDL's n) and a stack
// adjustment count in words. A binary operator's left operand is the
// word on top of the stack, which it pops, and its right operand is A.
// Taken from the interpreters: libjs/c4m.js, c4m.c, src/c4mp/vm.c, and
// docs/internals.md's and docs/fused-opcodes.md's tables.
//
// Markup: **bold**, *italic*, `code`; a blank line starts a paragraph.

import { OP } from '../c4m.js';

// A library call's note: its operands are the C call's, on the stack
const CALL = 'The C call\'s arguments are on the stack, the last one on top. The result is in `A`.';
const PROT = 'In a protected task (C4IX\'s programs) it traps to the kernel instead, which does it for the task.';

// [operand, what it does]: operand '' is none. A fused opcode's entry is
// its parts instead (FUSED, below).
const DOC = {
  LEA:  ['n', 'Load effective address: `A` = `bp` + *n* words. The address of a local (*n* < 0) or of an argument (*n* ≥ 2), as `&x` gives it. Nothing is read from memory.'],
  IMM:  ['v', 'Immediate: `A` = *v*. *v* is a number, or an address the loader relocates: a global, a string or a function. Nothing is read from memory.'],
  JMP:  ['addr', 'Jump to *addr*.'],
  JSR:  ['addr', 'Jump to subroutine: push the return address (the next instruction) onto the stack and jump to *addr*. The callee\'s **ENT** makes its frame, and its **LEV** returns. The caller then drops the arguments with **ADJ**.'],
  BZ:   ['addr', 'Branch if zero: jump to *addr* if `A` is 0, else go on to the next instruction. `A` is the condition an **if**, **while** or **&&** tested.'],
  BNZ:  ['addr', 'Branch if not zero: jump to *addr* if `A` is not 0, else go on to the next instruction.'],
  ENT:  ['n', 'Enter a function: push `bp`, set `bp` = `sp`, then make room for *n* words of locals below it. Arguments are then at `bp`+2 words and up, the last one first; locals are below `bp`.'],
  ADJ:  ['n', 'Adjust the stack: `sp` += *n* words. After a call, it drops the *n* arguments that were pushed for it.'],
  LEV:  ['', 'Leave a function: `sp` = `bp`, pop `bp`, then pop the return address and jump to it. The return value is in `A`.'],
  LI:   ['', 'Load int: `A` = the word at the address in `A`. A dereference, `A = *(int *)A`.'],
  LC:   ['', 'Load char: `A` = the byte at the address in `A`, sign-extended. A dereference, `A = *(char *)A`.'],
  SI:   ['', 'Store int: pop an address off the stack, and store `A` in the word there. A dereference, `*(int *)pop = A`. `A` keeps its value, as an assignment\'s.'],
  SC:   ['', 'Store char: pop an address off the stack, and store `A`\'s low byte there. A dereference, `*(char *)pop = A`. `A` becomes that byte, sign-extended.'],
  PSH:  ['', 'Push the contents of the `A` register onto the stack.'],
  OR:   ['', 'Pop the first item off the stack and OR it with the `A` register, bit by bit: `A` = pop | `A`.'],
  XOR:  ['', 'Pop the first item off the stack and exclusive-OR it with the `A` register: `A` = pop ^ `A`.'],
  AND:  ['', 'Pop the first item off the stack and AND it with the `A` register, bit by bit: `A` = pop & `A`.'],
  EQ:   ['', 'Pop the first item off the stack and compare it with the `A` register: `A` = 1 if they are equal, else 0.'],
  NE:   ['', 'Pop the first item off the stack and compare it with the `A` register: `A` = 1 if they differ, else 0.'],
  LT:   ['', 'Pop the first item off the stack: `A` = 1 if it is less than `A` (signed), else 0.'],
  GT:   ['', 'Pop the first item off the stack: `A` = 1 if it is greater than `A` (signed), else 0.'],
  LE:   ['', 'Pop the first item off the stack: `A` = 1 if it is less than or equal to `A` (signed), else 0.'],
  GE:   ['', 'Pop the first item off the stack: `A` = 1 if it is greater than or equal to `A` (signed), else 0.'],
  SHL:  ['', 'Pop the first item off the stack and shift it left by `A` bits: `A` = pop << `A`.'],
  SHR:  ['', 'Pop the first item off the stack and shift it right by `A` bits, keeping the sign: `A` = pop >> `A`.'],
  ADD:  ['', 'Pop the first item off the stack and add it to the `A` register: `A` = pop + `A`.'],
  SUB:  ['', 'Pop the first item off the stack and subtract the `A` register from it: `A` = pop − `A`.'],
  MUL:  ['', 'Pop the first item off the stack and multiply the `A` register by it: `A` = pop × `A`.'],
  DIV:  ['', 'Pop the first item off the stack and divide it by the `A` register, signed and rounding toward zero: `A` = pop / `A`.'],
  MOD:  ['', 'Pop the first item off the stack: `A` = the remainder of dividing it by `A` (signed): `A` = pop % `A`.'],

  OPEN: ['', '**open**(path, flags): open a file. `A` = the descriptor, or −1. ' + CALL + '\n\n' + PROT],
  READ: ['', '**read**(fd, buf, n): read up to *n* bytes into *buf* (memory is written through the pointer). `A` = the bytes read. ' + CALL + '\n\n' + PROT],
  CLOS: ['', '**close**(fd). ' + CALL + '\n\n' + PROT],
  PRTF: ['', '**printf**(format, ...): print. How many arguments there are is the operand of the **ADJ** that follows it (at most 7). `A` = the characters printed. ' + CALL + '\n\n' + PROT],
  MALC: ['', '**malloc**(n): `A` = a block of *n* bytes, or 0. ' + CALL + '\n\n' + PROT],
  FREE: ['', '**free**(p). ' + CALL + '\n\n' + PROT],
  MSET: ['', '**memset**(dst, c, n): fill *n* bytes at *dst* with *c* (memory is written through the pointer). `A` = *dst*. ' + CALL],
  MCMP: ['', '**memcmp**(p, q, n): compare *n* bytes, through both pointers. `A` = the difference of the first bytes that differ (unsigned), or 0. ' + CALL],
  EXIT: ['', '**exit**(code): end the program. ' + CALL + '\n\n' + PROT],
  PUTC: ['', '**putchar**(c): write one character. `A` = *c*. ' + CALL + '\n\n' + PROT],
  PUTS: ['', '**puts**(s): write the string at *s* and a newline. ' + CALL + '\n\n' + PROT],
  RALC: ['', '**realloc**(p, n): `A` = a block of *n* bytes holding what *p* held, or 0. ' + CALL + '\n\n' + PROT],
  MCPY: ['', '**memcpy**(dst, src, n): copy *n* bytes from *src* to *dst*, through both pointers. `A` = *dst*. ' + CALL],
  STRC: ['', '**stacktrace**(): print the call stack, named from the image\'s symbols.'],
  ITH:  ['', '**install_trap_handler**(fn): `A` = the old trap handler. ' + CALL + '\n\n' + PROT],
  _OPC: ['', '**__opcode**(name): `A` = the number of the opcode named by the string (`"LEV"` gives 8). ' + CALL],
  _BLT: ['', '**__builtin**: named only. No machine runs it: it halts, or traps as an unknown opcode.'],
  _TRP: ['', '**__c4_trap**(type, param): raise a trap of *type* from code. ' + CALL],
  OPCD: ['', '**__c4_opcode**(args..., op): run the opcode on top of the stack, with the arguments under it. C4KE\'s and C4IX\'s system calls are custom opcodes run this way: an opcode the machine does not have traps to the kernel, which does the call. One that takes an operand cannot be run this way.'],
  _JMP: ['', 'Pop an address off the stack and jump to it: `pc` = pop. A trap handler uses it to pass the trap on, leaving the frame as it is.'],
  _ADJ: ['', 'Adjust the stack by the word on top of it: `sp` += `*sp` words.'],
  C4CF: ['', '**__c4_configure**(what, value): set the cycle interrupt\'s interval (0) or handler (1), or whether a trap return restores the interval (3). `A` = the old value. ' + CALL],
  C4CY: ['', 'Cycles: `A` = the machine\'s instruction counter.'],
  TIME: ['', '**__time**(): `A` = milliseconds on the machine\'s clock.'],
  SIGH: ['', '**__c4_signal**(sig, handler): install the machine\'s handler for a signal. `A` = the old one. ' + CALL + '\n\n' + PROT],
  SIGI: ['', '`A` = the number of SIGINT.'],
  USLP: ['', '**__c4_usleep**(us): sleep for *us* microseconds. ' + CALL + '\n\n' + PROT],
  INFO: ['', '**__c4_info**(): `A` = the machine\'s capability bits (`C4I_*`): float, the FPU, protected mode, CPUs and so on.\n\n' + PROT],
  OPSL: ['', '`A` = the address of the opcode names, 5 bytes each (`"LEA ,IMM ,..."`).'],
  C4IV: ['', '**__c4_invoke**: a stub that rewrites itself into a **JMP**. Nothing happens on c4m.'],
  FLT:  ['', '**__c4_float**: a floating-point operation. The sub-op is on top of the stack and its operands under it. `A` = the result, a float\'s bits.'],
  JSRI: ['addr', 'Call through a pointer held in a global: push the return address, then jump to the function whose address is in the word at *addr*. A dereference, `pc = *(int *)addr`.'],
  JSRS: ['n', 'Call through a pointer held in a local or an argument: push the return address, then jump to the function whose address is in the word at `bp` + *n* words.'],
  JMPA: ['', 'Jump to the address in `A`: `pc` = `A`. A **switch** jumps through its table this way.'],
  TLEV: ['', 'Return from a trap: reload `pc`, `sp`, `bp`, `A` and the protection mode from the handler\'s frame, as they were when the trap was taken.'],
  DBG:  ['', 'Raise the debug trap (`TRAP_DEBUG`).'],

  CPUI: ['', '`A` = this CPU\'s number. (c4mp\'s and libjs\'s processors.)'],
  CPUN: ['', '`A` = how many CPUs the machine has.'],
  CPUS: ['', '**__c4_cpu_start**(id, entry, stacktop): start CPU *id* running *entry* on the stack at *stacktop*. ' + CALL],
  CPUH: ['', 'Halt this CPU, not the machine.'],
  CAS:  ['', '**__c4_cas**(addr, expect, new): compare and swap, atomically. If the word at *addr* is *expect*, store *new* there. `A` = the word as it was. ' + CALL],
  XCHG: ['', '**__c4_xchg**(addr, val): swap atomically. Store *val* in the word at *addr*, and `A` = the word as it was. ' + CALL],
  FADD: ['', '**__c4_fadd**(addr, delta): fetch and add, atomically. Add *delta* to the word at *addr*, and `A` = the word as it was. ' + CALL],
  CWAI: ['', '**__c4_wait**(addr, val): park this CPU while the word at *addr* is *val*. `A` = 1 if it already was not, 0 once woken. ' + CALL],
  CWAK: ['', '**__c4_wake**(addr, n): wake up to *n* CPUs parked on *addr*. ' + CALL],
  IPI:  ['', '**__c4_ipi**(cpu): interrupt another CPU. ' + CALL],
  LXI:  ['', 'Load an element of an array of 8-byte elements: pop the base off the stack, and `A` = the word at base + `A`×8. A dereference. c4mp only (c4lc -mcisc).'],
  SXI:  ['', 'Store an element of an array of 8-byte elements: pop the index, then the base, and store `A` in the word at base + index×8. A dereference. c4mp only (c4lc -mcisc).'],
  TRAW: ['', 'Put the terminal in raw mode, or take it out (the word on top of the stack). c4mp only.'],
  FPU:  ['', '**__c4_fpu**(r, b, a, op): IEEE arithmetic, single or double. The sub-op is on top of the stack, then *a*, *b* and *r*. A float is its bits; a double is the address of its two words, and a double result is written where *r* points. `A` = the result.'],
};

// A fused opcode is two or three instructions in one (docs/fused-opcodes.md)
const FUSED = {
  LDL:  ['n', ['LEA', 'LI'], 'Load a local or an argument: `A` = the word at `bp` + *n* words.'],
  STL:  ['n', ['LEA', 'PSH', 'SI'], 'Store into a local or an argument: the word at `bp` + *n* words = `A`. The address is pushed first and the value computed in between, as an assignment does.'],
  POPA: ['', null, 'Pop the word on top of the stack into `A`: what **IMM** 0 and **ADD** do.'],
  LDG:  ['addr', ['IMM', 'LI'], 'Load a global: `A` = the word at *addr*.'],
  PSHL: ['n', ['LEA', 'LI', 'PSH'], 'Push a local or an argument: `A` = the word at `bp` + *n* words, pushed.'],
  PSHG: ['addr', ['IMM', 'LI', 'PSH'], 'Push a global: `A` = the word at *addr*, pushed.'],
  LEAP: ['n', ['LEA', 'PSH'], 'Push the address of a local or an argument.'],
  IMMP: ['v', ['IMM', 'PSH'], 'Push a constant or an address.'],
  LIP:  ['', ['LI', 'PSH'], 'Dereference `A` and push what it pointed to.'],
  ADDL: ['', ['ADD', 'LI'], 'Index and load: `A` = the word at pop + `A`, as `p[i]` does once `i` is scaled.'],
};

// code spans first, set aside, since C in them has stars of its own
function md(s) {
  return s.split('\n\n').map(p => {
    const code = [];
    p = p.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;')
         .replace(/`(.+?)`/g, (_, c) => `\u0000${code.push(c) - 1}\u0000`)
         .replace(/\*\*(.+?)\*\*/g, '<b>$1</b>')
         .replace(/\*([^*\s][^*]*?)\*/g, '<i>$1</i>')
         .replace(/\u0000(\d+)\u0000/g, (_, k) => `<code>${code[k]}</code>`);
    return `<p>${p}</p>`;
  }).join('');
}
function head(name, arg) { return `<b>${name}</b>${arg ? ` <i>${arg}</i>` : ''}`; }
function first(text) { return text.split('\n\n')[0]; }
function operand(arg) {
  if (!arg) return '<p class="dim">No operand.</p>';
  const what = {
    n: 'a word count from <code>bp</code>, in the next word of code',
    v: 'a value, in the next word of code',
    addr: 'an address, in the next word of code',
  }[arg];
  return `<p class="dim">Operand <i>${arg}</i>: ${what}.</p>`;
}

const NAMES = Object.fromEntries(Object.entries(OP).map(([k, v]) => [v, k]));
// OP has c4m's names; c4mp's three that c4m does not have are 76-78
NAMES[76] = 'LXI'; NAMES[77] = 'SXI'; NAMES[78] = 'TRAW';

// The tooltip for opcode OP, as HTML; NAME is what the listing shows.
export function opDoc(op, name) {
  const nm = NAMES[op];
  if (nm && FUSED[nm]) {
    const [arg, parts, text] = FUSED[nm];
    let h = `<p>${head(nm, arg)} - ` +
      (parts ? parts.map(p => `<b>${p}</b>`).join(' with ').replace(/ with (?=<b>[^<]*<\/b>$)/, parts.length > 2 ? ' and ' : ' with ') + '. ' : '') + md(text).slice(3);
    if (parts) h += '<ul>' + [...new Set(parts)].map(p => `<li><b>${p}</b>: ${md(first(DOC[p][1])).slice(3, -4)}</li>`).join('') + '</ul>';
    return h + operand(arg) + '<p class="dim">A fused opcode (c4lc -mfuse).</p>';
  }
  if (nm && DOC[nm]) {
    const [arg, text] = DOC[nm];
    return `<p>${head(nm, arg)} - ${md(text).slice(3)}` + operand(arg);
  }
  return `<p><b>${name || op}</b> - opcode ${op}, which no machine here has. It traps as an unknown opcode ` +
    '(<code>TRAP_ILLOP</code>): a kernel that claims the number does it, which is how C4KE\'s and C4IX\'s system calls work.</p>';
}

// for tests: every opcode the machine names has an entry
export const documented = op => !!(NAMES[op] && (DOC[NAMES[op]] || FUSED[NAMES[op]]));
