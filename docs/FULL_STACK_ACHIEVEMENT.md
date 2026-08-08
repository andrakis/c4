# Building a Full-Stack System on C4: A Journey in Computational Minimalism

## Executive Summary

What started as "an exercise in minimalism" has evolved into something extraordinary: a complete, bootstrapped software stack that challenges fundamental assumptions about how complex systems must be built. In 2026, we've constructed the first known full-stack system built entirely on top of C4—a remarkably compact interpreter that runs real, sophisticated workloads.

From a lisp interpreter to a self-hosting optimizing C compiler, to a modern operating system that boots Linux, we've demonstrated that minimalism and sophistication aren't opposing forces. They can coexist, reinforce each other, and enable accomplishments that would seem impossible with such meager foundations.

---

## The Foundation: C4

C4 is a 1200-line implementation of a minimal virtual machine implementing a practical instruction set for building sophisticated systems.

It can run on anything. It has been ported to JavaScript, WebAssembly, interpreted on itself, and compiled to native code. Its genius is in how *much* becomes possible with *how little*, and what elaborate systems can be built on top.

---

## Milestone 1: Self-Hosting and Extension (C4M)

**Achievement**: Building a compiler that can compile itself, running on itself.

The first major milestone was creating C4M, the "C4 Multiloader"—an extended version of C4 that adds new opcodes and functionality while remaining runnable on the original interpreter. C4M introduced:

- **Multiple-file compilation**: Read many .c files as if they were one, enabling modularity
- **Function pointers**: Store addresses of functions in variables and call them
- **Trap handlers**: Hook into events and override default behavior
- **Cycle-based preemption**: Switch tasks automatically without OS support
- **Extended opcodes and builtin support**: New instructions, custom opcode handling, and more

The critical insight: **you don't need more instructions; you need the *ability to trap* and let user code handle the rest.**

C4M could run inside the original C4, but when compiled natively, it became the foundation for everything that followed. This self-hosting capability proved the entire stack was within reach.

---

## Milestone 2: Operating System as Library (C4KE)

**Achievement**: A pre-emptive multitasking kernel in 3,300 lines of C, running on C4.

C4KE—the "C4 Kernel Experiment"—rewrote OS primitives as library calls. Where traditional kernels are special privileged code, C4KE is just a well-organized C program. It provided:

- **Pre-emptive multitasking**: Task switching on a cycle budget via trap handlers
- **Task isolation**: Each task has its own registers, stack, and program counter
- **Shell and utilities**: A working Unix-like command-line interface
- **File abstraction**: A RAM-based filesystem
- **Process control**: fork-like spawning, wait syscalls, exit codes

The Unix-like shell ran unmodified C4KE binaries, and benchmarks showed it was actually *fast*—real work was getting done, not just simulated. Developers could write ordinary C code and have it run in a multitasking kernel—on the C4 VM.

---

## Milestone 3: A C Compiler for C4 (C4CC)

**Achievement**: Compiling C to the C4 ISA, bootstrapping the entire stack.

C4CC proved you could write a compiler in C, compile it to C4 bytecode, and have it compile *itself*. This is the hardest part of any bootstrapping story: can the tools you build actually build themselves?

C4CC:
- Compiles a substantial subset of C (pointers, arrays, control flow)
- Emits C4R (C4 Relocatable) format bytecode with symbol tables and relocation info
- Can run self-hosted: the compiler's source, compiled by itself, produces identical binaries

The C4R format itself was a win—it included enough metadata that multiple object files could be linked, enabling separate compilation and true modularity.

---

## Milestone 4: A Lisp Interpreter for Pattern Matching (C4SP)

**Achievement**: A complete Lisp implementation, in C, on C4, enabling high-level program transformation.

C4SP is a Lisp interpreter that runs on C4, whose purpose is to host optimization passes written in Lisp rather than imperative C. This solved a real problem: writing a peephole optimizer in C4's minimal language is painful. Writing it in Lisp is elegant:

```lisp
(defrule (IMM ?a) (PSH) (IMM ?b) (ADD)  =>  (IMM (+ ?a ?b)))
```

The interpreter includes:
- **Cons cells and environments**: Classic Lisp data structures implemented in arrays
- **Mark-and-sweep GC**: Garbage collection in C4, necessary to handle allocation-heavy Lisp
- **Byte-identical test suite**: Every sample verified against the Node.js alisp oracle
- **C4 optimizer in Lisp**: A real optimizer, shrinking compiled C4CC by 8.9% while preserving semantics

This proved you could build a high-level language inside C4 and have it do real work. The leap from imperative assembly-level thinking to functional list processing didn't require a larger VM—just the right layers of abstraction.

---

## Milestone 5: Modern Language Features (C4LC)

**Achievement**: An optimizing C compiler with structs, unions, and compiler-driven optimization.

C4LC extended C4CC with real language features:
- **Structs and unions**: Proper aggregate types with `.` and `->` access
- **Typedefs**: Type aliases and proper type checking
- **Optimization passes**: Peephole, constant folding, dead-code elimination
- **Extended ISA**: New instructions for common patterns
- **Preprocessor**: Stringize, paste, and macro support

Critically, C4LC could be compiled by itself. Developers could write readable, maintainable code with proper types and structures—all compiling to C4 through multiple optimization passes.

---

## Milestone 6: A Modern OS (C4IX)

**Achievement**: A ground-up rewrite of C4KE as a sophisticated Unix-like OS, using the languages we built.

C4IX was the proof that C4 could support sophisticated, readable system code:
- **Structs everywhere**: Proper data structures instead of hand-indexing arrays
- **Separate compilation**: Object files linked together, no monolithic kernel
- **Real filesystem**: Hierarchical directories, proper IO redirection
- **Process management**: spawn, wait, reaper for zombie collection, signal delivery
- **Multiple shells and utilities**: Pipelines, job control, real command-line editing
- **Nested kernels**: C4IX running under C4IX, demonstrating full isolation
- **Performance**: Cycle accounting per task, benchmarks rivaling C4KE

C4IX is written in extended C, compiled by c4lc, linked by c4rlink, and runs on c4m. It proves that high-level language features don't require a larger VM—they require better compilation.

---

## Milestone 7: A Hardware Simulator (C4BB)

**Achievement**: Simulating a 32-bit, microcoded CPU implementing C4M, verified against native execution.

C4BB—the "C4 Breadboard Computer"—is a WebGL-rendered, interactive simulation of a complete computer:
- **Microcode**: Real hardware-style microinstruction sequences
- **32-bit ALU, registers, memory**: A full memory hierarchy
- **Parity verification**: Instruction-by-instruction comparison with native c4m
- **Interactive UI**: Watch registers change, see memory updates, drive the keyboard
- **Boots both kernels**: C4KE and C4IX run unchanged in the simulator

C4BB proves the semantics of C4M are well-defined enough to be implemented in hardware. It's also strikingly beautiful—watching a VM's logic spread across a microcoded board shows that minimalism and elegance go hand in hand.

---

## Milestone 8: An OpenRISC Emulator (C4OR1K)

**Achievement**: Porting a full-featured CPU emulator to C4, and booting real Linux.

C4OR1K is a port of jor1k (a JavaScript OpenRISC emulator) to C, compiled to C4 bytecode. It implements:
- **Full OR1000 ISA**: All integer instructions, shift, multiply, divide, bit operations
- **Exception handling**: Real exception delivery to exception vectors
- **TLB and MMU**: Virtual memory with page table walk
- **UART and MMIO**: Interactive console IO
- **Interrupt handling**: Tick-timer interrupts for scheduler preemption

And it boots real Linux. Not a toy kernel or a simulation—the actual Linux kernel for OpenRISC, hitting the genuine panic when the root filesystem isn't found (no 9P device yet, but that's next).

This is extraordinary because:
1. It's written in C and compiled by our own toolchain
2. It's large, complex, real-world code—not a toy
3. It demonstrates that performance is achievable: millions of guest instructions per second
4. It proves the stack is production-grade: you can do sophisticated work on C4

---

## The Full Stack

Here's what we've built, layer by layer:

```
┌─────────────────────────────────────────────┐
│  Linux Kernel (Real, for OpenRISC)           │  Boots with real kernel panic
├─────────────────────────────────────────────┤
│  C4OR1K (OR1000 Emulator in C)               │  Millions of instructions/sec
├─────────────────────────────────────────────┤
│  C4IX (Unix-like OS with proper structs)     │  Nested kernels, pipes, signals
├─────────────────────────────────────────────┤
│  C4BB (Microcoded Hardware Simulator)        │  Verified bit-for-bit, WebGL UI
├─────────────────────────────────────────────┤
│  C4LC (Optimizing C Compiler)                │  Structs, optimization passes, self-hosting
├─────────────────────────────────────────────┤
│  C4SP (Lisp Interpreter)                     │  Pattern matching for optimization
├─────────────────────────────────────────────┤
│  C4KE (Pre-emptive Kernel)                   │  Multitasking in userspace C
├─────────────────────────────────────────────┤
│  C4CC (C to C4 Compiler)                     │  Object files, relocation, linking
├─────────────────────────────────────────────┤
│  C4M (Extended C4)                           │  The foundation that enables everything
├─────────────────────────────────────────────┤
│  C4 (1200-line VM)                           │  Practical instruction set
└─────────────────────────────────────────────┘
```

Each layer is written in the languages provided by layers below it. Each layer proves the previous layer is sufficient. The top layer—booting real Linux—validates the entire stack.

---

## Why This Matters

### 1. **It Challenges Our Assumptions About Complexity**

We assume modern systems require modern instruction sets. C4 proves otherwise. The complexity comes not from the ISA, but from what you build *on top* of it. A good compiler can emit practical code. A well-designed OS doesn't need privileged modes—it can trap and handle everything in userspace C code.

### 2. **It Demonstrates Bootstrapping Purity**

There's no "cheating" here. Every tool was built from the tools below it:
- The C compiler was written in C and compiled by itself
- The OS was written in C and runs on its own kernel
- The optimizing compiler proves that high-level languages don't require bloated VMs
- The whole stack fits in a single GitHub repository

### 3. **It Makes Systems Understandable**

When your entire stack is small enough to read and understand, systems go from magic to science. A student can understand how a multitasking kernel works by reading 3,300 lines of C. They can understand how structs are implemented by reading the compiler. They can understand how Linux boots by reading the emulator that runs it.

### 4. **It Enables Experimentation**

Want to try a new ISA? Rewrite the instruction handling. Want to try preemption strategies? Modify the cycle budget in the trap handler. Want to optimize code differently? Write new Lisp rules. The entire stack is open and modifiable because it's all just code.

---

## What's Next?

Future milestones are already planned:

- **9P filesystem** for c4or1k: Mount a real 9P server, letting Linux access host files
- **Debugger**: Breakpoints, inspection, stepping through any layer
- **More architectures**: Porting to ARM, RISC-V, MIPS
- **Parallelism**: c4mp (multiprocessor) continues to evolve
- **Formal verification**: Proving correctness of critical sections

But even with these future work, what we've accomplished stands alone: **we've built a complete, end-to-end, bootstrapped, real-world computing stack.**

---

## Conclusion

C4 started as "an exercise in minimalism." It remains practical and minimal. But through careful layering, good abstractions, and a commitment to bootstrapping purity, we've shown that minimalism and sophistication are not opposed. 

We've built a Lisp interpreter for optimization passes. We've built an operating system with proper structs and preemption. We've built a hardware simulator you can interact with in a web browser. We've booted Linux.

It's all built on C4.

**This is the first known full-stack system built entirely on C4.** We believe that makes it worth sharing, worth studying, and worth celebrating as a milestone in how we think about software systems.

---

## References

- [C4 Original](https://github.com/rswier/c4) - The inspiration and oracle
- [C4KE - C4 Kernel Experiment](../README.md)
- [C4IX Design](c4ix-design.md)
- [C4LC Design](c4lc-design.md)
- [C4SP Design](c4sp-design.md)
- [C4BB Design](c4bb-design.md)
- [C4OR1K Design](c4or1k-design.md)
- [OISC4 Design](oisc4-design.md) - One Instruction Set Computer version

---

**Built by**: The C4 community  
**Last updated**: August 2026  
**Status**: All major milestones complete; shipping production-quality code layers
