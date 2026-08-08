// ucode.js - assembler for the c4bb microcode file (hw/microcode.uc).
//
// The .uc file is the single source of truth for what the CPU does:
// both the visual step engine (machine.js) and the fast engine
// (turbo.js) execute the tables this assembler produces, so the board
// animation can never disagree with what actually ran.
//
// File format, line oriented, '#' comments:
//
//   const NAME 0x123          named constant for C= references
//   routine fetch:            a control routine (fetch is required)
//   op LEA operand:           an opcode routine; 'operand' auto-prefixes
//                             the two operand-fetch steps into OPR
//   op C4CF: jsop             opcode handled by the system controller
//                             (a single black-box microstep)
//   .label:                   local label
//       SIG SIG ...           one microstep: signals asserted together
//       br az .label          conditional next-step (same step may carry
//       br !tz .label         signals); flags: az (A==0), tz, mz (MDR==0)
//       goto .label           unconditional next
//       dispatch              next step chosen by IR (fetch/OPCD only)
//
// Signals:
//   drivers:  PC_OUT SP_OUT BP_OUT A_OUT OPR_OUT OPR_OUTX4 MDR_OUT
//             B_OUT T_OUT T_OUTX4 U_OUT CYC_OUT ALU_OUT C=<const>
//   latches:  PC_IN SP_IN BP_IN A_IN IR_IN OPR_IN MAR_IN[+words]
//             MDR_IN B_IN T_IN U_IN
//   memory:   MEM_RD MEM_RDB MEM_RDBU MEM_WR MEM_WRB
//   counters: PC_INC SP_INC SP_DEC T_DEC1 U_INC1 B_INC1
//   ALU:      ALU=<OR..MOD>  ALU_LA (left=A, else B)
//             ALU_RT (right=T) ALU_RM (right=MDR) (else right=A)
//
// A routine falling off its end returns to fetch (instruction done).

export const FETCH = -1;      // sentinel: next instruction
export const DISPATCH = -2;   // sentinel: decode IR

export const ALU_OPS = ['OR','XOR','AND','EQ','NE','LT','GT','LE','GE',
                        'SHL','SHR','ADD','SUB','MUL','DIV','MOD'];

const DRIVERS = new Set(['PC_OUT','SP_OUT','BP_OUT','A_OUT','OPR_OUT',
  'OPR_OUTX4','MDR_OUT','B_OUT','T_OUT','T_OUTX4','U_OUT','CYC_OUT','ALU_OUT']);
const LATCHES = new Set(['PC_IN','SP_IN','BP_IN','A_IN','IR_IN','OPR_IN',
  'MDR_IN','B_IN','T_IN','U_IN']);
const MEMSIGS = new Set(['MEM_RD','MEM_RDB','MEM_RDBU','MEM_WR','MEM_WRB']);
const COUNTERS = new Set(['PC_INC','SP_INC','SP_DEC','T_DEC1','U_INC1','B_INC1']);
const ALUMODS = new Set(['ALU_LA','ALU_RT','ALU_RM']);
const FLAGS = new Set(['az','tz','mz']);

export function assemble(source) {
  const consts = new Map();
  const steps = [];              // flat array of step objects
  const routines = new Map();    // name -> start index
  const ops = new Map();         // opcode name -> {start, jsop}
  const fixups = [];             // {step, field, label, scope}

  let scope = null;              // current routine/op name
  let scopeLabels = new Map();   // label -> step index (per scope)
  const allScopeLabels = new Map(); // scope -> labels map
  let pendingLabels = [];

  function endScope() {
    if (scope !== null) {
      // a label at the very end of a routine means "instruction done"
      for (const l of pendingLabels) scopeLabels.set(l, FETCH);
      pendingLabels = [];
      allScopeLabels.set(scope, scopeLabels);
    }
    scopeLabels = new Map();
  }

  function beginStep(step) {
    steps.push(step);
    for (const l of pendingLabels) scopeLabels.set(l, steps.length - 1);
    pendingLabels = [];
    return step;
  }

  const lines = source.split('\n');
  for (let ln = 0; ln < lines.length; ln++) {
    let line = lines[ln].replace(/#.*/, '').trim();
    if (!line) continue;

    let m;
    if ((m = line.match(/^const\s+(\w+)\s+(\S+)$/))) {
      consts.set(m[1], parseNum(m[2], consts));
      continue;
    }
    if ((m = line.match(/^routine\s+(\w+):$/))) {
      endScope();
      scope = m[1];
      routines.set(scope, steps.length);
      continue;
    }
    if ((m = line.match(/^op\s+(\w+)(\s+operand)?:(\s*jsop)?$/))) {
      endScope();
      scope = m[1];
      const entry = { start: steps.length, jsop: !!m[3] };
      ops.set(scope, entry);
      if (m[3]) {
        beginStep(mkStep({ jsop: scope, scope, line: `jsop ${scope}` }));
        continue;
      }
      if (m[2]) {
        // operand fetch prologue: word at PC -> OPR, PC += 4
        beginStep(parseStep('PC_OUT MAR_IN', consts, scope));
        beginStep(parseStep('MEM_RD MDR_OUT OPR_IN PC_INC', consts, scope));
      }
      continue;
    }
    if ((m = line.match(/^(\.\w+):$/))) {
      pendingLabels.push(m[1]);
      continue;
    }
    if (scope === null) throw new Error(`ucode line ${ln + 1}: step outside routine`);
    const step = beginStep(parseStep(line, consts, scope));
    if (step.brLabel) fixups.push({ idx: steps.length - 1, field: 'brTarget', label: step.brLabel, scope });
    if (step.gotoLabel) fixups.push({ idx: steps.length - 1, field: 'next', label: step.gotoLabel, scope });
  }
  endScope();

  // Resolve label fixups (labels are scope-local; routine names global)
  for (const f of fixups) {
    const labels = allScopeLabels.get(f.scope);
    let target;
    if (labels && labels.has(f.label)) target = labels.get(f.label);
    else if (routines.has(f.label)) target = routines.get(f.label);
    else if (f.label === 'fetch') target = FETCH;
    else throw new Error(`ucode: unresolved label ${f.label} in ${f.scope}`);
    steps[f.idx][f.field] = target;
  }

  // Default next: sequential within the file, FETCH at scope boundaries.
  // A step already given a next (goto/dispatch) keeps it.
  const boundaries = new Set();
  for (const r of routines.values()) boundaries.add(r);
  for (const o of ops.values()) boundaries.add(o.start);
  for (let i = 0; i < steps.length; i++) {
    if (steps[i].next === undefined) {
      const j = i + 1;
      steps[i].next = (j < steps.length && !boundaries.has(j)) ? j : FETCH;
    }
  }

  if (!routines.has('fetch')) throw new Error('ucode: no fetch routine');
  return { steps, routines, ops, consts };
}

function parseNum(tok, consts) {
  if (consts.has(tok)) return consts.get(tok);
  const v = tok.startsWith('0x') ? parseInt(tok, 16) : parseInt(tok, 10);
  if (Number.isNaN(v)) throw new Error(`ucode: bad number ${tok}`);
  return v;
}

function mkStep(extra = {}) {
  return {
    drivers: [], latches: [], mem: null, counters: [],
    aluOp: -1, aluLA: false, aluR: 'A',
    cval: null, marOfs: 0,
    brFlag: null, brNeg: false, brTarget: null, brLabel: null,
    next: undefined, gotoLabel: null, jsop: null,
    scope: null, line: '',
    ...extra,
  };
}

function parseStep(line, consts, scope) {
  const step = mkStep({ scope, line });
  const toks = line.split(/\s+/);
  for (let i = 0; i < toks.length; i++) {
    const t = toks[i];
    let m;
    if (t === 'br') {
      let flag = toks[++i];
      step.brNeg = flag.startsWith('!');
      if (step.brNeg) flag = flag.slice(1);
      if (!FLAGS.has(flag)) throw new Error(`ucode: bad flag ${flag} in ${scope}`);
      step.brFlag = flag;
      step.brLabel = toks[++i];
    } else if (t === 'goto') {
      step.gotoLabel = toks[++i];
    } else if (t === 'dispatch') {
      step.next = DISPATCH;
    } else if ((m = t.match(/^C=(\S+)$/))) {
      step.cval = parseNum(m[1], consts) | 0;
    } else if ((m = t.match(/^MAR_IN(\+(\d+))?$/))) {
      step.latches.push('MAR_IN');
      step.marOfs = m[2] ? parseInt(m[2], 10) : 0;
    } else if ((m = t.match(/^ALU=(\w+)$/))) {
      step.aluOp = ALU_OPS.indexOf(m[1]);
      if (step.aluOp < 0) throw new Error(`ucode: bad ALU op ${m[1]}`);
    } else if (ALUMODS.has(t)) {
      if (t === 'ALU_LA') step.aluLA = true;
      else if (t === 'ALU_RT') step.aluR = 'T';
      else step.aluR = 'MDR';
    } else if (DRIVERS.has(t)) {
      step.drivers.push(t);
    } else if (LATCHES.has(t)) {
      step.latches.push(t);
    } else if (MEMSIGS.has(t)) {
      if (t === 'MEM_RD' || t === 'MEM_RDB' || t === 'MEM_RDBU') {
        if (step.mem && step.mem.startsWith('MEM_W'))
          throw new Error(`ucode: read+write in one step: ${line}`);
        step.mem = step.mem || t;
        if (step.mem !== t) throw new Error(`ucode: two reads in one step: ${line}`);
      } else {
        step.memWrite = t;   // write happens after latches
      }
    } else if (COUNTERS.has(t)) {
      step.counters.push(t);
    } else {
      throw new Error(`ucode: unknown token '${t}' in ${scope}: ${line}`);
    }
  }
  const driverCount = step.drivers.length + (step.cval !== null ? 1 : 0);
  if (driverCount > 1)
    throw new Error(`ucode: ${driverCount} bus drivers in one step: ${line}`);
  return step;
}
