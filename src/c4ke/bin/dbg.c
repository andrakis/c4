// C4KE Debugger
//
// - When the debugger is attached:
//   - The code section is copied so it can be referenced.
//   - The first statement to be executed in the entry is replaced with a DBG opcode.
//   - This triggers a trap that forwards to the debug_handler function.
//   - The debug_handler:
//     - Moves the returnpc back 1
//     - Updates the instruction with the correct instruction in the copied code.
//     - Analyzes the saved instruction and:
//       - If it branches, updates the branch target with the DBG instruction.
//       - If it calls a function, updates the function entry with the DBG instruction.
//       - If it's a multi-word opcode, skips a word (++returnpc).
//       - Changes the next instruction to DBG.
//     - Signals to the kernel that the task should resume at given returnpc.
//
// In this way, the debugger keeps getting invoked for every instruction.
