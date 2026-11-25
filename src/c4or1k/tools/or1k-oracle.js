#!/usr/bin/env node
// or1k-oracle.js -- runs a c4or1k test program (the same flat
// big-endian .bin tools/asm.py produces) through jor1k's own
// safecpu.js directly, and dumps state in the exact format main.c's
// cpu_dump()/ram_dump() use, so the two can be diffed line for line.
//
// Not a reimplementation: this requires the real
// jorconsole/jor1k/js/worker/or1k/safecpu.js as a library. That's the
// point -- it's the oracle M1's decoder is cross-checked against
// (see docs/c4or1k-design.md), so it needs to be jor1k's actual code,
// not a transcription of it that could share the same misreading.
//
// Usage: node or1k-oracle.js test.bin [ram-dump-base] [ram-dump-nwords]

const fs = require("fs");

// messagehandler.js (required by safecpu.js) is written for a real
// Worker global scope -- it assigns `onmessage = ...` and calls
// `postMessage(...)` at module scope, both undeclared identifiers
// under its own "use strict". jorconsole normally supplies these via
// the `webthreads` native worker-thread environment; a plain `node`
// process has neither, so they're stubbed here before the require.
// Debug()/Abort() (the only two calls this oracle should ever reach,
// and only on a real cpu.c-vs-jor1k divergence) route through Send(),
// which is why postMessage prints instead of silently doing nothing.
global.onmessage = undefined;
global.postMessage = (msg) => console.error("(jor1k)", msg);

const SafeCPU = require("../../../../jorconsole/jor1k/js/worker/or1k/safecpu.js");

const HEAP_SIZE = 0x200000; // 2MB: RAMOFFSET (see below) plus test RAM

// jor1k's SafeCPU aliases the register/SPR file onto heap bytes
// 0x0-0xE000 (`new Int32Array(ram.heap, 0, ...)` etc in safecpu.js's
// constructor) and, separately, system.js constructs its RAM object
// as `new RAM(heap, 0x100000)` -- guest address 0 is really heap byte
// 0x100000, not 0, precisely so real RAM never collides with that
// register aliasing. A first version of this stub read/wrote heap
// byte `addr` directly with no offset: guest address 0 landed on r0's
// own storage, and the test program (loaded at guest address 0)
// silently overwrote r1-r9 before a single instruction ran. Applying
// the same offset jor1k itself uses is not optional here.
const RAMOFFSET = 0x100000;

// ram.js's actual Read/WriteXBigTemplate formulas (not a
// reconstruction -- read verbatim from ram.js and matched exactly).
// The surprise: "Big" here does NOT mean "bytes physically stored
// big-endian". int32mem is a completely ordinary, host-native
// Int32Array, and Read32Big/Write32Big are *just* `int32mem[addr>>2]`
// -- no byte composition at all. Byte/halfword "Big" access instead
// XORs the address (^3 for a byte, ^2 for a halfword-within-a-word)
// to reach into that same native int32 storage from the opposite
// end, which is what makes narrower accesses agree with native word
// storage on a little-endian host without ever physically
// byte-swapping anything.
//
// A first version of this stub built Read/WriteXBig as manual
// big-endian byte composition instead (the "obvious" reading of the
// name) and got real, self-consistent-looking values back -- register
// results even matched, since GetInstruction happened to route
// through that same manual composition. It only broke once
// safecpu.js's int32mem *fast path* (taken by every l.sw/l.lwz here,
// since all this test's addresses are positive) started writing
// through the real, differently-addressed formula: two internally
// self-consistent but mutually incompatible endian conventions on the
// same memory, diagnosed by the byte-swapped values it produced
// (e.g. m56 read back as 0x2A000000 instead of 0x2A). Matching
// ram.js's real formulas exactly, not "big-endian" as a concept,
// is what makes every access path -- instruction fetch, the
// int32mem fast path, and the byte/halfword slow path -- agree.
function makeRam() {
    const heap = new ArrayBuffer(HEAP_SIZE);
    const int32mem = new Int32Array(heap, RAMOFFSET);
    const uint8mem = new Uint8Array(heap, RAMOFFSET);
    const ram = {
        heap: heap,
        int32mem: int32mem,
        Read8Big: (addr) => uint8mem[addr ^ 3],
        Write8Big: (addr, val) => { uint8mem[addr ^ 3] = val & 0xFF; },
        Read16Big: (addr) => (uint8mem[(addr ^ 2) + 1] << 8) | uint8mem[addr ^ 2],
        Write16Big: (addr, val) => {
            uint8mem[(addr ^ 2) + 1] = (val >> 8) & 0xFF;
            uint8mem[(addr ^ 2)] = val & 0xFF;
        },
        Read32Big: (addr) => int32mem[addr >> 2] | 0,
        Write32Big: (addr, val) => { int32mem[addr >> 2] = val | 0; },
    };
    return { ram, int32mem };
}

// Computes each word's intended big-endian VALUE from the file's byte
// stream, then stores it with a plain native assignment -- the moral
// equivalent of ram.js's own Little2Big pass (make native storage
// hold the right value), done once at load time instead of as a
// separate swap step.
function loadProgram(int32mem, filePath) {
    const bytes = fs.readFileSync(filePath);
    if (bytes.length % 4 !== 0) throw new Error(`${filePath} length not a multiple of 4`);
    for (let i = 0; i < bytes.length; i += 4) {
        const val = (bytes[i] << 24) | (bytes[i + 1] << 16) | (bytes[i + 2] << 8) | bytes[i + 3];
        int32mem[i >> 2] = val;
    }
    return bytes.length / 4;
}

function main() {
    const binPath = process.argv[2];
    const ramBase = process.argv[3] ? parseInt(process.argv[3], 16) : 0x1000;
    const ramWords = process.argv[4] ? parseInt(process.argv[4], 10) : 64;
    if (!binPath) {
        console.error("usage: node or1k-oracle.js test.bin [ram-dump-base-hex] [ram-dump-nwords]");
        process.exit(1);
    }

    const { ram, int32mem } = makeRam();
    const nwords = loadProgram(int32mem, binPath);

    const cpu = new SafeCPU(ram);
    // Reset() (called by the constructor) points pc at the RESET
    // vector (0x100), not 0 -- main.c starts a test program at guest
    // address 0 with no boot vector to honor, so the oracle must too.
    cpu.pc = 0;
    cpu.nextpc = 1;
    // Reset()'s Exception(EXCEPT_RESET, 0) call also sets ESR to
    // GetFlags() as it stood mid-construction (SR_SM/SR_FO already on
    // -> 0x8001), where cpu.c's simpler cpu_reset() (no vector jump,
    // see its comment) leaves ESR at 0. Every other Reset()-touched
    // field (EEAR, EPCR, SR_SM/IEE/TEE/DME/IME/OVE) already lands on
    // the same value cpu_reset() uses, checked by hand against
    // Exception()'s unconditional resets -- ESR is the one real
    // difference between "went through the vector" and "didn't".
    cpu.group0[64] = 0; // SPR_ESR_BASE

    let steps = 0;
    const maxSteps = 100000; // guard against a runaway test program
    while (cpu.pc !== nwords) {
        cpu.Step(1, 0); // 1 instruction/call, clockspeed unused (TTMR stays 0 throughout M1's tests)
        steps++;
        if (steps > maxSteps) {
            console.error(`FAULT: exceeded ${maxSteps} steps without reaching pc=${nwords} (stuck at pc=${cpu.pc})`);
            process.exit(1);
        }
    }

    const b = (v) => (v ? 1 : 0);
    console.log(`c4or1k-oracle: ran ${steps} instructions from ${binPath}`);
    // Matches cpu_dump()'s format exactly (cpu.c), field for field, so
    // the two can be diffed line for line -- see make c4or1k-m1-check
    // and (from M2) c4or1k-m2-check.
    console.log(`pc=${cpu.pc} nextpc=${cpu.nextpc} SR_F=${b(cpu.SR_F)} SR_CY=${b(cpu.SR_CY)} SR_OV=${b(cpu.SR_OV)}`);
    console.log(`SR_SM=${b(cpu.SR_SM)} SR_TEE=${b(cpu.SR_TEE)} SR_IEE=${b(cpu.SR_IEE)} SR_DME=${b(cpu.SR_DME)} SR_IME=${b(cpu.SR_IME)}`);
    console.log(`EPCR=${cpu.GetSPR(32) | 0} EEAR=${cpu.GetSPR(48) | 0} ESR=${cpu.GetSPR(64) | 0}`); // SPR_EPCR/EEAR/ESR_BASE
    console.log(`TTMR=${cpu.TTMR | 0} TTCR=${cpu.TTCR | 0} PICMR=${cpu.PICMR | 0} PICSR=${cpu.PICSR | 0}`);
    for (let i = 0; i < 32; i++) {
        console.log(`r${i}=${cpu.r[i] | 0}`);
    }
    for (let i = 0; i < ramWords; i++) {
        console.log(`m${i}=${ram.Read32Big(ramBase + i * 4)}`);
    }
}

main();
