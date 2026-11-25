// test-opdoc.mjs - the Code Explorer's instruction tooltips (libjs/web/
// opdoc.js) against the machine's own opcode table (libjs/c4m.js).
//
// Every opcode the machine names has an explanation, headed by its name.
// It names an operand exactly when the instruction has one (hasOperand,
// the interpreter's own test). A fused opcode names the instructions it
// stands for. A number nothing names says it traps.
//
//   node libjs/tests/test-opdoc.mjs

import { OP, hasOperand } from '../c4m.js';
import { opDoc, documented } from '../web/opdoc.js';

let fail = 0;
const bad = why => { fail++; console.log(`test-opdoc: ${why}`); };
const names = { ...Object.fromEntries(Object.entries(OP).map(([k, v]) => [v, k])), 76: 'LXI', 77: 'SXI', 78: 'TRAW' };
for (const [op, name] of Object.entries(names).map(([k, v]) => [+k, v])) {
  if (!documented(op)) { bad(`${name} (${op}) has no explanation`); continue; }
  const h = opDoc(op, name);
  if (!h.startsWith(`<p><b>${name}</b>`)) bad(`${name}: the tooltip does not start with its name`);
  // LXI and SXI take none, though c4mp's own test is not hasOperand's
  const operand = hasOperand(op) && op !== 76 && op !== 77;
  if (operand !== /Operand <i>/.test(h)) bad(`${name}: says ${operand ? 'no operand' : 'an operand'}, the machine says otherwise`);
  if (!operand && !/No operand\./.test(h)) bad(`${name}: does not say it has no operand`);
}
for (const [f, parts] of [['LEAP', ['LEA', 'PSH']], ['PSHG', ['IMM', 'LI', 'PSH']], ['ADDL', ['ADD', 'LI']]]) {
  const h = opDoc(OP[f], f);
  for (const p of parts) if (!h.includes(`<li><b>${p}</b>:`)) bad(`${f} does not explain ${p}`);
}
if (!/TRAP_ILLOP/.test(opDoc(200, '200'))) bad('an unknown opcode does not say it traps');
if (!/dereference/i.test(opDoc(OP.LI, 'LI'))) bad('LI does not say it dereferences');
console.log(fail ? `test-opdoc: ${fail} FAILED` : `test-opdoc: ${Object.keys(names).length} opcodes explained OK`);
process.exit(fail ? 1 : 0);
