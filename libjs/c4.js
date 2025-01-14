/**
 * C4 Virtual Machine for JavaScript
 *
 * Note: uses ArrayBuffer.resize, which may require a newer version of Node (12+).
 *
 * Implements the main C4 virtual machine loop.
 * - Designed to be included by files generated by asm-js.c. These files provide the initial VM setup, including
 *   loading code and data to memory, patching addresses, and passing arguments to the code the VM runs.
 * - Uses ArrayBuffers for memory, allowing full words or bytes to be read. Unaligned word addresses get truncated.
 * - Uses either Int32Array or BigInt64Array for memory. BigInt64Array is much slower than Int32Array, so compiling
 *   using a 32bit version of c4 is recommended.
 * - Has a very rudimentry filesystem and memory allocator which need to be replaced.
 * - Two interpreters are provided (see configure_instructions):
 *   - Overridden by default, an interpreter which just looks up each instruction in an instruction table, and
 *     calls the appropriate handler.
 *   - Enabled by default, an interpreter containing all the various instruction handlers, inlined into one big
 *     switch statement, generated at runtime. Requires all instruction handlers to avoid use of return keyword,
 *     for that would perform an early return from switch statement.
 */

'use strict';

var util = require('util');
var printj = require('printj');
const { Mallocator } = require('./simplest/mallocator');
var c4fs = require('./simplest/fs');

const MT_32 = 0, MT_64 = 1;
const POOLSZ = 256 * 1024;
const MAXPOOLSZ = 32 * 1024 * 1024; // 32mb

exports.TraceMemory = false;

exports.MT_32 = MT_32;
exports.MT_64 = MT_64;

function C4Memory32 (size) {
	this.memory  = new ArrayBuffer(size, { maxByteLength: MAXPOOLSZ });
	this.viewint = new Int32Array(this.memory);
	this.viewchar= new Uint8Array(this.memory);
	//console.log(`C4Memory32(${size}), viewint:`, this.viewint);
	this.word   = 4;
}
C4Memory32.prototype.readword  = function(loc) { return this.viewint[loc / this.word]; }
C4Memory32.prototype.writeword = function(loc, value) {
	//if (exports.TraceMemory) console.log("writeword.32(", loc, ", ", value, "), viewint addr = ", loc / this.word);
	return this.viewint[loc / this.word] = value;
}
C4Memory32.prototype.readchar  = function(loc) { return this.viewchar[loc]; }
C4Memory32.prototype.writechar = function(loc, value) {
	//if (exports.TraceMemory) console.log("writechar.32(", loc, ", ", value, ")");
	return this.viewchar[loc] = value;
}
C4Memory32.prototype.convert   = (c) => c;
C4Memory32.prototype.align     = (v) => v + (-v & (4 - 1));

function C4Memory64 (size) {
	this.memory  = new ArrayBuffer(size, { maxByteLength: MAXPOOLSZ });
	this.viewint = new BigInt64Array(this.memory);
	this.viewchar= new Uint8Array(this.memory);
	this.word    = 8n;
}
C4Memory64.prototype.readword = function(loc) { return this.viewint[loc / this.word]; }
C4Memory64.prototype.writeword = function(loc, value) {
	//if (exports.TraceMemory) console.log("writeword.64(", loc, ", ", value, "), viewint addr = ", loc / this.word);
	return this.viewint[loc / this.word] = BigInt(value || 0);
}
C4Memory64.prototype.readchar  = function(loc) { return this.viewchar[loc]; }
C4Memory64.prototype.writechar = function(loc, value) {
	//if (exports.TraceMemory) console.log("writechar.64(", loc, ", ", value, ")");
	return this.viewchar[loc] = Number(value);
}
C4Memory64.prototype.convert   = (c) => BigInt(c);
C4Memory64.prototype.align     = (v) => v + (-v & (8n - 1n));

const memtypes = [ (s) => new C4Memory32(s), (s) => new C4Memory64(s) ];

function C4VM (memoryType, poolsz) {
	//console.log("C4VM(", memoryType, ", ", poolsz, ")");
	this.memory = memtypes[memoryType](poolsz || POOLSZ);
	this.word = this.memory.word;
	this.instructions = {};
	this.PC = this.A = this.BP = this.SP = 0;
	this.exit_code = 0;
	this.flag_run  = 0;
	this.zero = this.convert(0);
	this.one  = this.convert(1);
	this.FALSE = this.zero;
	this.TRUE  = this.one;

	this.printj = printj;
	this.c4fs   = c4fs;
	this.Mallocator = Mallocator;

	this.TraceMemory = false;

	this._fix_types();
}

C4VM.prototype._fix_types = function() {
	var t = this;
	// Ensure registers are correct types
	['word', 'PC', 'A', 'BP', 'SP', 'exit_code'].forEach((name) => {
		var old = t[name];
		t[name] = t.memory.convert(t[name])
		//console.log("Convert ", name, " from ", old, " to ", t[name]);
	});
};

//
// Public interface
//

C4VM.prototype.readword = function(loc) { return this.memory.readword(loc); };
C4VM.prototype.writeword= function(loc, value) { return this.memory.writeword(loc, value); };
C4VM.prototype.readchar  = function(loc) { return this.memory.readchar(loc); };
C4VM.prototype.writechar = function(loc, value) {
	//console.log("C4VM.writechar(", loc, ", ", value, ")");
	return this.memory.writechar(loc, value); };
C4VM.prototype.convert   = function(value) { return this.memory.convert(value); }

C4VM.prototype.loadwords = function(loc, words) {
	var t = this;
	words.forEach((word, offset) => {
		var tloc = t.memory.convert((loc + offset - 1)) * t.memory.word;
		var tword = t.memory.convert(word);
		t.writeword(tloc, tword);
		var x;
		if ((x = t.memory.readword(tloc)) != tword) {
			console.log("Readback after write failed, read:", x, ", expected:", tword);
			process.exit(1);
		}
	});
	//words.forEach((word, offset) => console.log("offset", offset - 1, "word", word));
	//console.log(t.memory);
};
C4VM.prototype.loadchars = function(loc, chars) {
	var t = this;
	loc = t.memory.convert(loc);
	//console.log("loadchars(", loc, ")");
	chars.split("").forEach((ch, offset) => {
		//console.log("loadchar: ", loc + offset, " = ", word);
		t.writechar(loc + t.memory.convert(offset), ch.charCodeAt(0));
	});
	//var tmp = new Uint8Array(this.memory.memory, loc);
	//var tmp = this.memory.viewchar.slice(loc);
	//console.log("Memory loaded:", util.inspect(tmp));
};
C4VM.prototype.patch = function(loc, value) {
	loc = this.memory.convert(loc);// * this.word;
	//console.log("patch(", loc, ", ", value, ")");
	this.writeword(loc, this.memory.convert(value));
};
C4VM.prototype.ensureMemoryAvailable = function(newsize) {
	newsize = this.convert(newsize);
	if (this.memory.convert(this.memory.memory.byteLength) < newsize) {
		//console.log(`(MALC) MARK, memory resizing from 0x${this.memory.memory.byteLength.toString(16)} ` +
		//            ` to 0x${newsize.toString(16)} (${this.memory.memory.byteLength} to ${newsize})`);
		this.memory.memory.resize(Number(newsize));
	}
};

C4VM.prototype.init = function() {
	// Init malloc stuff
	this.mallocator = new Mallocator(this.MallocStart, this);
};
C4VM.prototype.run = function(debugFlag) {
	const t = this;
	//debugFlag = true;
	this.flag_run = 1;
	this._fix_types();
	//console.log(t.memory);
	while(this.flag_run) {
		var ins = this.readword(this.PC);
		//console.log("PC: ", this.PC, "ins: ", ins);
		if (debugFlag) {
			if (ins <= this.memory.convert(this.instructions_strs['ADJ']))
				console.log("PC: ", this.PC, this.instructions_strs[ins], " ", this.readword(this.PC + this.word), ", A = ", this.A);
			else
				console.log("PC: ", this.PC, this.instructions_strs[ins], ", A = ", this.A);
		}
		this.PC += this.word;
		this.instructions[ins](this);
	}
	this.mallocator.atexit();
};

//
// Core instructions
//
var instructions = {};

// LEA: a = (int)(bp + *pc++)
instructions.LEA = (vm) => { vm.A = vm.BP + vm.readword(vm.PC) * vm.word; vm.PC += vm.word; };
// IMM: a = *pc++
instructions.IMM = (vm) => {
	vm.A = vm.readword(vm.PC);
	//console.log("IMM @ ", vm.PC, " = ", vm.A);
	vm.PC += vm.word;
};
// JMP: pc = (int *)*pc
instructions.JMP = (vm) => vm.PC = vm.readword(vm.PC);
// JSR: *--sp = (int)(pc + 1); pc = (int *)*pc;
instructions.JSR = (vm) => {
	vm.SP -= vm.word;
	vm.writeword(vm.SP, vm.PC + vm.word);
	vm.PC = vm.readword(vm.PC);
};
// JSRI: *--sp = (int)(pc + 1); pc = (int *)*pc; pc = (int *)*pc;
instructions.JSRI = (vm) => {
	vm.SP -= vm.word;
	vm.writeword(vm.SP, vm.PC + vm.word);
	vm.PC = vm.readword(vm.PC);
	vm.PC = vm.readword(vm.PC);
};
// JSRS: *--sp = (int)(pc + 1); pc = (int *)*pc; pc = (int *)*(bp + *pc)
instructions.JSRS = (vm) => {
	vm.SP -= vm.word;
	vm.writeword(vm.SP, vm.PC + vm.word);
	vm.PC = vm.readword(vm.PC);
	vm.PC = vm.readword(vm.BP + vm.PC);
};
// BZ: pc = a ? pc + 1 : (int *)*pc
instructions.BZ  = (vm) => vm.PC = vm.A ? vm.PC + vm.word : vm.readword(vm.PC);
// BNZ: pc = a ? (int *)*pc : pc + 1
instructions.BNZ = (vm) => vm.PC = vm.A ? vm.readword(vm.PC) : vm.PC + vm.word;
// ENT: *--sp = (int)bp; bp = sp; sp = sp - *pc++
instructions.ENT = (vm) => {
	vm.SP -= vm.word; vm.writeword(vm.SP, vm.BP);
	vm.BP = vm.SP;
	vm.SP -= vm.readword(vm.PC) * vm.word; vm.PC += vm.word;
};
// ADJ: sp = sp + *pc++
instructions.ADJ = (vm) => { vm.SP += vm.readword(vm.PC) * vm.word; vm.PC += vm.word; };
// LEV: sp = bp; bp = (int *)*sp++; pc = (int *)*sp++
instructions.LEV = (vm) => {
	vm.SP = vm.BP;
	vm.BP = vm.readword(vm.SP); vm.SP += vm.word;
	vm.PC = vm.readword(vm.SP); vm.SP += vm.word;
};
// LI: a = *(int *)a
instructions.LI = (vm) => vm.A = vm.readword(vm.A);
// LC: a = *(char *)a
instructions.LC = (vm) => vm.A = vm.convert(vm.readchar(vm.A) || 0);
// SI: *(int *)*sp++ = a
instructions.SI = (vm) => { vm.writeword(vm.readword(vm.SP), vm.A); vm.SP += vm.word; };
// SC: *(char *)*sp++ = a
instructions.SC = (vm) => { vm.writechar(vm.readword(vm.SP), vm.A); vm.SP += vm.word; };
// PSH: *--sp = a
instructions.PSH = (vm) => { vm.SP -= vm.word; vm.writeword(vm.SP, vm.A); };

//
// Math operators
//
instructions.OR  = (vm) => { vm.A = vm.readword(vm.SP) |  vm.A; vm.SP += vm.word; };
instructions.XOR = (vm) => { vm.A = vm.readword(vm.SP) ^  vm.A; vm.SP += vm.word; };
instructions.AND = (vm) => { vm.A = vm.readword(vm.SP) &  vm.A; vm.SP += vm.word; };
instructions.EQ  = (vm) => { vm.A = vm.readword(vm.SP) == vm.A ? vm.TRUE : vm.FALSE; vm.SP += vm.word; };
instructions.NE  = (vm) => { vm.A = vm.readword(vm.SP) != vm.A ? vm.TRUE : vm.FALSE; vm.SP += vm.word; };
instructions.LT  = (vm) => { vm.A = vm.readword(vm.SP) <  vm.A ? vm.TRUE : vm.FALSE; vm.SP += vm.word; };
instructions.GT  = (vm) => { vm.A = vm.readword(vm.SP) >  vm.A ? vm.TRUE : vm.FALSE; vm.SP += vm.word; };
instructions.LE  = (vm) => { vm.A = vm.readword(vm.SP) <= vm.A ? vm.TRUE : vm.FALSE; vm.SP += vm.word; };
instructions.GE  = (vm) => { vm.A = vm.readword(vm.SP) >= vm.A ? vm.TRUE : vm.FALSE; vm.SP += vm.word; };
instructions.SHL = (vm) => { vm.A = vm.readword(vm.SP) << vm.A; vm.SP += vm.word; };
instructions.SHR = (vm) => { vm.A = vm.readword(vm.SP) >> vm.A; vm.SP += vm.word; };
instructions.ADD = (vm) => { vm.A = vm.readword(vm.SP) +  vm.A; vm.SP += vm.word; };
instructions.SUB = (vm) => { vm.A = vm.readword(vm.SP) -  vm.A; vm.SP += vm.word; };
instructions.MUL = (vm) => { vm.A = vm.readword(vm.SP) *  vm.A; vm.SP += vm.word; };
instructions.DIV = (vm) => { vm.A = vm.readword(vm.SP) /  vm.A; vm.SP += vm.word; };
instructions.MOD = (vm) => { vm.A = vm.readword(vm.SP) %  vm.A; vm.SP += vm.word; };

C4VM.prototype.readstring = function(loc) {
	var str = "";
	var ch;
	while (ch = this.readchar(loc++)) {
		str += String.fromCharCode(ch);
	};
	return str;
}

//
// System calls
//
instructions.OPEN = (vm) => {
	var pathword = vm.readword(vm.SP + vm.word);
	var pathname = vm.readstring(pathword);
	var mode     = vm.readword(vm.SP);
	vm.A = vm.memory.convert(vm.c4fs.open(pathname, Number(mode)));
};
instructions.READ = (vm) => {
	// TODO: move this to filesystem layer
	var fildes  = vm.readword(vm.SP + vm.word + vm.word); //console.log("READ, filwdes:", fildes);
	var buf     = vm.readword(vm.SP + vm.word); //console.log("READ, buf:", buf);
	var nbyte   = vm.readword(vm.SP); //console.log("READ, nbyte:", nbyte);
	var content = vm.c4fs.read(Number(fildes), Number(nbyte));
	// Write contents to buf
	var len     = vm.convert(content.length);
	for (var i = vm.zero; i < len; i++) {
		vm.writechar(buf + i, vm.convert(content[i].charCodeAt(0)));
		/*if (vm.readchar(buf + i) != vm.convert(content[i].charCodeAt(0))) {
			console.log(`READ, write verify failed and got: ${vm.readchar(buf + i)}`);
			console.log(`Resizable ArrayBuffer support doesn't seem to be present`);
			process.exit(1);
			return;
		}*/
	}
	if (len == 0) vm.A = vm.zero;
	else          vm.A = len;
};
instructions.CLOS = (vm) => {
	var fildes  = vm.readword(vm.SP); //console.log("CLOS, fildes :", fildes);
	vm.c4fs.close(Number(fildes));
};
instructions.PRTF = (vm) => {
	var s = "";
	// Like C4 does, look at the ADJ X instruction, it is the number of arguments
	// provided to printf.
	var argcount = vm.readword(vm.PC + vm.word);
	var t = vm.SP + (argcount * vm.word);
	var z = vm.zero;
	var tminus1 = vm.readword(t - vm.word);
	// Collect args
	var args = [];
	for (var i = vm.one; i < argcount; ++i) {
		var a = (vm.readword(t - (vm.word * (i + vm.one))));
		args.push(Number(a));
	}
	// Collect chars until we reach end of string
	do {
		var c = String.fromCharCode(vm.readchar(tminus1));
		s = s + c;
		++tminus1;
	} while(vm.readchar(tminus1) != z);
	// Does all of this need to be updated every call? Perhaps not
	vm.printj.stringFormatter.vm = vm;
	vm.printj.stringFormatter.tminus1 = tminus1;
	vm.printj.stringFormatter.z = z;
	var sprinted = vm.printj.sprintf(s, ...args);
	vm.A = vm.convert(sprinted.length);
	// Finally, output the formatted string
	process.stdout.write(sprinted);
};
// Implement our own string formatter that reads from a VM's memory.
printj.stringFormatter = (s) => {
	var vm = printj.stringFormatter.vm;
	var tminus1 = printj.stringFormatter.tminus1;
	var z = printj.stringFormatter.z;
	// Read a string from memory using this argument
	var loc = vm.memory.convert(s);
	var s1 = "";
	var ch;
	//console.log(`(stringFormatter reading from memory 0x${loc.toString(16)})`);
	while ((ch = vm.memory.readchar(loc++)) != z) {
		if (ch === undefined) // Well past done
			break;
		s1 = s1 + String.fromCharCode(ch);
	}
	//console.log(`(stringFormatter got string '${s1}')`);
	return s1;
};
instructions.MALC = (vm) => {
	vm.A = vm.mallocator.malloc(vm.readword(vm.SP)); //console.log("malloc() = 0x" + vm.A.toString(16));
};
// Not a required instruction
instructions.RALC = (vm) => { console.log("RALC not implemented"); };
instructions.FREE = (vm) => { vm.mallocator.free(vm.readword(vm.SP)); /*console.log("free(0x" + vm.readword(vm.SP).toString(16) + ")");*/ };
instructions.MSET = (vm) => {
	var s = vm.readword(vm.SP + vm.word + vm.word); //console.log("MSET: s=", s);
	var c = vm.readword(vm.SP + vm.word); //console.log("MSET: c=", c);
	var n = vm.readword(vm.SP); //console.log("MSET: n=", n);
	// Disable TraceMemory for actual writing
	var oldTrace = vm.TraceMemory;
	vm.TraceMemory = false;

	if (s % vm.word == 0 && n % vm.word == 0) {
		// optimized version
		for (var x = s, y = vm.zero; y < n; y += vm.word, x += vm.word)
			vm.writeword(x, c);
	} else {
		for (var x = s, y = vm.zero; y < n; y++, x++)
			vm.writechar(x, c);
	}
	vm.A = vm.zero; // it's a void function, but C4 turns it into an int?

	vm.TraceMemory = oldTrace;
};
instructions.MCMP = (vm) => {
	var s1 = vm.readword(vm.SP + vm.word + vm.word); //console.log("MCMP: s1=", s1);
	var s2 = vm.readword(vm.SP + vm.word); //console.log("MCMP: s2=", s2);
	var n  = vm.readword(vm.SP); //console.log("MCMP: n=", n);
	s1 = Number(s1);
	s2 = Number(s2);
	n  = Number(n);
	var ms1 = vm.memory.viewchar.slice(s1, n);
	var ms2 = vm.memory.viewchar.slice(s2, n);
	vm.A = ms1.toString() == ms2.toString() ? vm.FALSE : vm.TRUE;
};
instructions.MCMP/*old*/ = (vm) => {
	// benchmarked: this is faster than the above
	var s1 = vm.readword(vm.SP + vm.word + vm.word); //console.log("MCMP: s1=", s1);
	var s2 = vm.readword(vm.SP + vm.word); //console.log("MCMP: s2=", s2);
	var n  = vm.readword(vm.SP); //console.log("MCMP: n=", n);
	var r  = vm.zero;
	if ((s1 % vm.word == 0) && (s2 % vm.word == 0) && (n % vm.word == 0)) {
		// Fast mode
		do {
			var w1 = vm.readword(s1), w2 = vm.readword(s2);
			if (w1 != w2) {
				r = w1 - w2;
				break;
			}
			w1 += vm.word;
			w2 += vm.word;
			n  -= vm.word;
		} while (n != vm.zero);
	} else {
		// Slow mode
		do {
			var c1 = vm.readchar(s1++), c2 = vm.readchar(s2++);
			if (c1 != c2) {
				r = c1 - c2;
				break;
			}
		} while (--n != vm.zero);
	}
	vm.A = r;
};
// Not part of C4 spec, can be omitted
instructions.MCPY = (vm) => {
	// TODO: optimized version
	var dest = vm.readword(vm.SP + vm.word + vm.word); //console.log("MCPY: dest=", dest);
	var src = vm.readword(vm.SP + vm.word); //console.log("MCPY: src=", src);
	var n = vm.readword(vm.SP); //console.log("MCPY: n=", n);
};
instructions.STRC = (vm) => { console.log("STRC not implemented"); };
instructions.EXIT = (vm) => { vm.exit_code = vm.readword(vm.SP); vm.flag_run = 0; process.stdout.write("exit(" + vm.exit_code + ")\n"); };

instructions.UNKNOWN = (vm) => {
	console.log("UNKNOWN instruction");
	vm.instructions.EXIT();
};

// Return the body of a function, supporting the short function style.
Function.prototype.body = function() {
	if (this.toString().indexOf("{") == -1)
		return this.toString().substring(this.toString().indexOf("=>") + 3) + ";";
	return this.toString().substring(this.toString().indexOf("{") + 1, this.toString().lastIndexOf("}"));  
};

//
// Further public interface
//

instructions.lookup_instruction = (ins) => instructions[ins] || instructions.UNKNOWN;
// lins: Array(Array(Int InstructionIndex, String Handler), ...)
C4VM.prototype.configure_instructions = function(lins) {
	// Configure instruction table for normal run.
	var self = this;
	self.instructions_strs = {};
	lins.forEach((e) => {
		self.instructions_strs[e[0]] = e[1];
		self.instructions_strs[e[1]] = e[0];
		self.instructions[e[0]] = instructions.lookup_instruction(e[1])
	});
	// Build switch statement instruction interpreter that should be somewhat faster
	// than the normal run function, by virtue of inlining all the functions into one
	// big switch statement.
	// Probably massively overkill.
	var code = `  'use strict';` +
			   `  //debugFlag = true;\n` +
			   `  this.flag_run = 1;\n` +
			   `  this._fix_types();\n` +
			   `  //console.log(t.memory);\n` +
			   `  while(this.flag_run) {\n` +
			   `    var ins = this.readword(this.PC);\n` +
			   `    //console.log("PC: ", this.PC, "ins: ", ins);\n` +
			   `    if (debugFlag) {\n` +
			   `      if (ins <= this.memory.convert(this.instructions_strs['ADJ']))\n` +
			   `        console.log("PC: ", this.PC, this.instructions_strs[ins], " ", this.readword(this.PC + this.word), ", A = ", this.A);\n` +
			   `      else\n` +
			   `      console.log("PC: ", this.PC, this.instructions_strs[ins], ", A = ", this.A);\n` +
			   `    }\n` +
			   `    this.PC += this.word;\n` +
			   `    var vm = this;\n` +
			   `    switch(ins) {\n`;
	var suffix = "";
	var n = self.zero;
	if (typeof n == 'bigint') suffix = 'n';
	lins.forEach((e) => {
		code += `      case ${self.convert(e[0]).toString()}${suffix}: {       // ${e[1]}\n`;
		code += `      ` + instructions.lookup_instruction(e[1]).body();
		code += `      break;\n` +
		        `      }\n`;
	});
	code +=    `    }\n` +
	           `  }\n` +
			   `this.mallocator.atexit();\n`;
	//console.log("Interpreter:\n", code);
	this.run = new Function('debugFlag', code);
};

exports.C4VM = C4VM;
exports.instructions = instructions;
