// C4 Machine
//
// Emulates a simple system.
// Instead of compiling and running C, compiles and runs
// assembly.

#include <memory.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>

#ifdef __GNUC__
#include <unistd.h>
#else
#if _WIN64
#define __INTPTR_TYPE__ long long
#elif _WIN32
#define __INTPTR_TYPE__ int
#endif // if _WIN64
#endif // ifdef __GNUC__
#define int __INTPTR_TYPE__

enum { LEA ,IMM ,JMP ,JSR ,BZ  ,BNZ ,ENT ,ADJ ,LEV ,LI  ,LC  ,SI  ,SC  ,PSH ,
       OR  ,XOR ,AND ,EQ  ,NE  ,LT  ,GT  ,LE  ,GE  ,SHL ,SHR ,ADD ,SUB ,MUL ,DIV ,MOD ,
       OPEN,READ,CLOS,PRTF,MALC,FREE,MSET,MCMP,EXIT };

char *opcodes;
void setup_opcodes () {
	opcodes =
		"LEA ,IMM ,JMP ,JSR ,BZ  ,BNZ ,ENT ,ADJ ,LEV ,LI  ,LC  ,SI  ,SC  ,PSH ,"
		"OR  ,XOR ,AND ,EQ  ,NE  ,LT  ,GT  ,LE  ,GE  ,SHL ,SHR ,ADD ,SUB ,MUL ,DIV ,MOD ,"
		"OPEN,READ,CLOS,PRTF,MALC,FREE,MSET,MCMP,EXIT,";
}

// Application flags
char *init;
int flags;
enum {
	FLG_NONE =  0x0,
	FLG_DEBUG = 0x1,
};

// struct label {
enum {
	LBL_MAGIC,         // (int)   Magic value
	LBL_NAME,          // (char*) Label name
	LBL_OFFSET,        // (int)   Label offset
	LBL__SZ            // Size of label struct
};
int *label_base, label_idx, label_max;
int  label_magic;

int poolsz;
// struct process {
enum {
	// Creation state
	P_ProcID,
	P_INUSE,
	P_EXIT,
	// Allocation information
	P_MM_SYM,
	P_MM_E,
	P_MM_SP,
	P_MM_DATA,
	P_MM_P,
	// Compilation information
	P_E,             // Emitted code pointer
	P_Data,          // Emitted data pointer
	// Registers
	P_PC,            // Code pointer
	P_SP,            // Stack pointer
	P_BP,            // Base pointer
	P_A,             // Accumulator
	P_CYCLE,         // Cycle counter
	P__SZ            // Size of process struct
};
int *processes;      // Pointer to processes struct
int  proc_max;       // Maximum number of concurrent processes

int mach_strlen (char *s) {
	int l;
	l = 0;
	while(*s++) ++l;
	return l;
}

int mach_isprint (char c) { return c >= 0x21 && c <= 0x7e; }

int mach_strcmp (char *s1, char *s2) {
	while(*s1++ == *s2++) ;
	return (*s1 == *s2) ? 0 : 1;
}

int mach_strncmp (char *s1, char *s2, int n) {
	return memcmp(s1, s2, n);
}

void mach_strcpy (char *dest, char *source) {
	char *curr;
	curr = source;
	while(*curr) *dest++ = *curr++;
	*dest = 0;
}

void mach_strncpy (char *dest, char *source, int length) {
	char *curr;
	curr = source;
	if(length) {
		while(*curr && --length)
			*dest++ = *curr++;
	}
	*dest = 0;
}

char *mach_atoi_move (char *str, int radix, int *dest) {
	int v, sign;

	v = 0;
	sign = 1;
	if(*str == '-') {
		sign = -1;
		++str;
	}
	while (
		(*str >= 'A' && *str <= 'Z') ||
		(*str >= 'a' && *str <= 'z') ||
		(*str >= '0' && *str <= '9')) {
		//v = v * 10 + ('0' - *str++);
		v = v * radix + ((*str > '9') ? (*str & ~0x20) - 'A' + 10 : (*str - '0'));
		++str;
	}
	*dest = v * sign;
	return str;
}

char *mach_atoin_move(char *str, int radix, int *dest, int len) {
	int v, sign;

	v = 0;
	sign = 1;
	if(*str == '-') {
		sign = -1;
		++str;
	}
	while (len-- && ((*str >= 'A' && *str <= 'z') || (*str >= '0' && *str <= '9'))) {
		//v = v * 10 + ('0' - *str++);
		v = v * radix + ((*str > '9') ? (*str & ~0x20) - 'A' + 10 : (*str - '0'));
		++str;
	}
	*dest = v * sign;
	return str;
}

int mach_atoi (char *str, int radix) {
	int dest;
	mach_atoi_move(str, radix, &dest);
	return dest;
}

int mach_atoin (char *str, int radix, int length) {
	int dest;
	mach_atoin_move(str, radix, &dest, length);
	return dest;
}

int *label_new (char *name, int offset) {
	int *label;

	if (label_idx >= label_max) {
		printf("Label count exceeded!\n");
		return 0;
	}
	label = label_base + (LBL__SZ * label_idx++);
	if(!(label[LBL_NAME] = (int)malloc(sizeof(char) * mach_strlen(name) + 1))) {
		free(label);
		return 0;
	}
	mach_strcpy((char*)label[LBL_NAME], name);
	label[LBL_OFFSET] = offset;
	label[LBL_MAGIC] = label_magic ^ (offset-1);
	return label;
}

int label_valid (int *label) {
	return label[LBL_MAGIC] == (label_magic ^ (label[LBL_OFFSET] - 1));
}

void label_free (int *label) {
	if(label_valid(label)) {
		free((char*)label[LBL_NAME]);
		memset(label, 0, sizeof(int) * LBL__SZ);
	}
}

int *label_find_strn (char *str, int len) {
	int *label, *end;

	label = label_base;
	end = label_base + (LBL__SZ * label_max);
	while(label < end) {
		if(label_valid(label)) {
			if(!mach_strncmp((char*)label[LBL_NAME], str, len))
				return label;
		}
		label = label + LBL__SZ;
	}

	return 0;
}

int argc;
char **argv;
int setup () {
	char *symbol, *ptr;
	int   op, tmp;

	poolsz = 256 * 1024; // arbritrary size
	init = "asm/init.asm";
	flags = FLG_NONE;
	label_idx = 0;
	label_max = 512;
	proc_max = 32;

	// Read arguments
	--argc; ++argv;
	if (argc > 0 && **argv == '-' && (*argv)[1] == 'd') { flags = flags | FLG_DEBUG; --argc; ++argv; }
	if (argc > 0 && **argv == '-' && (*argv)[1] == 'p') { proc_max = mach_atoi(*argv + 3, 10); --argc; ++argv; }
	if (argc >= 1) { init = *argv; --argc; ++argv; }

	if (!(processes = malloc(tmp = sizeof(int) * P__SZ * proc_max))) {
		printf("Failed allocate %ld bytes for process storage\n", tmp);
		return 5;
	}

	memset(processes, 0, tmp);

	tmp = sizeof(int) * LBL__SZ * label_max;
	if (!(label_base = malloc(tmp))) {
		printf("Failed to allocate %ld bytes for %ld labels\n", tmp, label_max);
		return 1;
	}
	memset(label_base, 0, tmp);

	setup_opcodes();
	label_magic = 0xBEEF;

	// Setup symbols
	if (!(symbol = malloc(sizeof(char) * 5))) {
		printf("Failed to allocate 5 bytes!\n");
		return 2;
	}
	symbol[4] = 0;

	ptr = opcodes;
	op = 0;
	while (*ptr) {
		// Copy current word to symbol
		symbol[0] = ptr[0];
		symbol[1] = ptr[1];
		symbol[2] = ptr[2];
		symbol[3] = ptr[3];
		if (ptr[3] == ' ') symbol[3] = 0;
		if (ptr[2] == ' ') symbol[2] = 0;
		if (flags & FLG_DEBUG)
			printf("Opcode %s = %ld\n", symbol, op);
		if(!label_new(symbol, op)) {
			printf("Failed to create symbols!\n");
			return 3;
		}
		ptr = ptr + 5;
		++op;
	}

	free(symbol);
	return 0;
}

void free_labels() {
	int label;

	label = 0;
	while (label < label_idx)
		label_free(label_base + (LBL__SZ * label++));
}

void cleanup() {
	free_labels();
	free(label_base);
}

char *readline() {
	char *buf;
	int   bufsz;
	bufsz = 128;
	if (!(buf = malloc(bufsz)))
		return buf;
	memset(buf, 0, bufsz);
	bufsz = read(0, buf, bufsz - 1);
	buf[bufsz] = 0;
	return buf;
}

void process_exit(int code, int *process) {
	// TODO: emit to listeners
	printf("exit(%d) cycle = %d\n", code, process[P_CYCLE]);
	// Cleanup
	free((int*)process[P_MM_SYM]);
	free((int*)process[P_MM_E]);
	free((int*)process[P_MM_SP]);
	free((char*)process[P_MM_DATA]);
	free((char*)process[P_MM_P]);
	process[P_EXIT] = code;
	process[P_INUSE] = 0;
}

void process_unknown_op(int op, int *process) {
	// TODO: Unknown op handler?
	printf("unknown instruction = %d! cycle = %d\n", op, process[P_CYCLE]);
	process_exit(255, process);
}

int run_procs() {
	int cycle, *pc, *sp, *bp, *process, i, a, *t;
	int pid, debug, cycle_max, pid_cycle, procs_run;

	pid = 0;
	debug = 1;// flags & FLG_DEBUG;
	cycle_max = 100;
	process = processes;
	procs_run = 0;

	while (pid < proc_max) {
		if (process[P_INUSE]) {
			pid_cycle = 0;
			pc = (int*)process[P_PC];
			sp = (int*)process[P_SP];
			bp = (int*)process[P_BP];
			cycle = process[P_CYCLE];
			a = process[P_A];
			while (pid_cycle++ < cycle_max) {
				i = *pc++; ++cycle;
				if (debug) {
					printf("%d> %.4s", cycle, &opcodes[i * 5]);
					if (i <= ADJ) printf(" %lld\n", *pc); else printf("\n");
				}
				if (i == LEA) a = (int)(bp + *pc++);                                  // load local address
				else if (i == IMM) a = *pc++;                                         // load global address or immediate
				else if (i == JMP) pc = (int *)*pc;                                   // jump
				else if (i == JSR) { *--sp = (int)(pc + 1); pc = (int *)*pc; }        // jump to subroutine
				else if (i == BZ)  pc = a ? pc + 1 : (int *)*pc;                      // branch if zero
				else if (i == BNZ) pc = a ? (int *)*pc : pc + 1;                      // branch if not zero
				else if (i == ENT) { *--sp = (int)bp; bp = sp; sp = sp - *pc++; }     // enter subroutine
				else if (i == ADJ) sp = sp + *pc++;                                   // stack adjust
				else if (i == LEV) { sp = bp; bp = (int *)*sp++; pc = (int *)*sp++; } // leave subroutine
				else if (i == LI)  a = *(int *)a;                                     // load int
				else if (i == LC)  a = *(char *)a;                                    // load char
				else if (i == SI)  *(int *)*sp++ = a;                                 // store int
				else if (i == SC)  a = *(char *)*sp++ = a;                            // store char
				else if (i == PSH) *--sp = a;                                         // push
				else if (i == OR)  a = *sp++ | a;
				else if (i == XOR) a = *sp++ ^  a;
				else if (i == AND) a = *sp++ &  a;
				else if (i == EQ)  a = *sp++ == a;
				else if (i == NE)  a = *sp++ != a;
				else if (i == LT)  a = *sp++ < a;
				else if (i == GT)  a = *sp++ > a;
				else if (i == LE)  a = *sp++ <= a;
				else if (i == GE)  a = *sp++ >= a;
				else if (i == SHL) a = *sp++ << a;
				else if (i == SHR) a = *sp++ >> a;
				else if (i == ADD) a = *sp++ + a;
				else if (i == SUB) a = *sp++ - a;
				else if (i == MUL) a = *sp++ *  a;
				else if (i == DIV) a = *sp++ / a;
				else if (i == MOD) a = *sp++ %  a;
				else if (i == OPEN) a = open((char *)sp[1], *sp);
				else if (i == READ) a = read(sp[2], (char *)sp[1], *sp);
				else if (i == CLOS) a = close(*sp);
				else if (i == PRTF) { t = sp + pc[1]; a = printf((char *)t[-1], t[-2], t[-3], t[-4], t[-5], t[-6]); }
				else if (i == MALC) a = (int)malloc(*sp);
				else if (i == FREE) free((void *)*sp);
				else if (i == MSET) a = (int)memset((char *)sp[2], sp[1], *sp);
				else if (i == MCMP) a = memcmp((char *)sp[2], (char *)sp[1], *sp);
				else if (i == EXIT) { process_exit(*sp, process); pid_cycle = cycle_max; } // force stop process
				else { process_unknown_op(i, process); pid_cycle = cycle_max; } // force stop process
			}
			// Save process state
			process[P_PC] = (int)pc;
			process[P_SP] = (int)sp;
			process[P_BP] = (int)bp;
			process[P_CYCLE] = cycle;
			process[P_A] = a;
			procs_run++;
		}
		// Advance
		process = process + P__SZ;
		++pid;
	}

	return procs_run;
}

int *process_next () {
	int *proc, i;

	i = 0;
	while(i < proc_max) {
		proc = processes + (i * P__SZ);
		if(!proc[P_INUSE])
			return proc;
	}

	return 0;
}

// Find next token
char *next (char *c) {
	char prev, quot;

	prev = 0;
	quot = 0;

	if(*c == ';' && prev != '\'') {
		// comment, read until end of line
		while(*c != '\n') ++c;
	}
	// skip unprintables
	while(*c && !mach_isprint(*c)) ++c;
	return c;
}

// Find next whitespace
char *next_whitespace (char *c) {
	while(*c && mach_isprint(*c)) ++c;
	return c;
}

char *asm_read_expression (char *expr, int *dest, int *proc) {
	int    offset, result;
	int   *base_code, result_code;
	char  *base_data, offset_data;
	char  operator;
	char *name, *name_end;
	int   name_len;

	if(*expr++ == '[') {
		if((name_end = next_whitespace(expr))) {
			name = expr;
			if((expr = next(name_end))) {
				name_len = name_end - name;
				operator = *expr++;
				if((expr = next(expr))) {
					expr = mach_atoi_move(expr, 10, &offset);
					if (operator != '+' && operator != '-') {
						printf("[expression:] expected operator to be + or -\n");
						return 0;
					}
					while (*expr++ != ']'); // skip to end of directive
					if (!mach_strncmp("code", name, name_len)) {
						base_code = (int*)proc[P_MM_E];
						offset /= sizeof(int);
						result = (int)(operator == '+' ?
							(base_code + offset) :
							(base_code - offset));
					} else if (!mach_strncmp("data", name, name_len)) {
						base_data = (char*)proc[P_MM_DATA];
						result = (int)(operator == '+' ?
							(base_data + offset) :
							(base_data - offset));
					} else {
						printf("Don't know what base this is: '%.*s'\n", name_len, name);
						return 0;
					}
					*dest = result;
					return expr;
				}
			}
		}
	}

	return 0;
}

char *asm_pass_scan_directive (char *directive, int len, char *start, int *proc) {
	int tmp, *label;
	char *n;

	start = next(start);
	if(!mach_strncmp(directive, "TARGET", len)) {
		tmp = mach_atoin(start, 10, 2);
		printf("TARGET %dbit\n", tmp);
		if(tmp / 8 != sizeof(int)) {
			printf("This assembly file is for another architecture. Recompile for your system.\n");
			return 0;
		}
		return next_whitespace(start);
	} else if(!mach_strncmp(directive, "ENTRY", len)) {
		if(*start == '[') {
			if(!(n = asm_read_expression(start, &tmp, proc))) {
				printf("Failed to parse [directive + expression]\n");
				return 0;
			}
		} else {
			// Lookup label
			if(!(n = next_whitespace(start))) {
				printf("Expected a label\n");
				return 0;
			}
			--n;
			if(!(label = label_find_strn(start, n - start))) {
				printf("Label not found: '%.*s'\n", n - start, start);
				return 0;
			}
			tmp = label[LBL_OFFSET];
		}
		proc[P_PC] = tmp;
		return n;
	} else {
		printf("Unexpected directive\n");
		return 0;
	}
}

char *asm_pass_label_directive (char *directive, int len, int location, int *proc) {
	char *name;
	int  *result;

	if(!(name = malloc(len * sizeof(char)))) {
		printf("Failed to allocate %ld bytes for label name\n", len * sizeof(char));
		return 0;
	}
	// copy name over
	mach_strncpy(name, directive, len);

	result = label_new(name, location);
	free(name);
	if(!result) {
		printf("Failed to create label '%s'\n", name);
		return 0;
	}
	return directive + len + 1;
}

// Scan pass:
//  1) Keep track of code position for labels
//  2) Find any labels and set position
int line, column;
int asm_pass_scan (int *proc, char *content) {
	char *c, *t, *data;
	int   e, tmp, v;

	c = next(content);
	e = 0;
	data = (char*)proc[P_Data];

	while(*c) {
		while (*c == '\n' || *c == '\r' || *c == ' ' || *c == 8) ++c;
		if (*c == ';') {
			// comment (to end of line)
			while (*c++ != '\n') ;
		} else if(*c == '.') {
			// .DIRECTIVE [some value] (skip for now)
			while(*c++ != '\n') ;
		} else if(*c == '"') {
			// Skip strings
			while(*c++ != '\n') ;
			// Counts as 1
			++e;
		} else if (*c == '\'') {
			// Skip small quotes
			while (*++c != '\'');
			++e;
		} else if (*c == '-' || (*c >= '0') && *c <= '9') {
			// A number
			c = mach_atoi_move(c, 10, &tmp);
			++e;
		} else if (*c == '[') {
			// Expansion, skip for now
			while (*c++ != ']') ;
			++e;
		} else {
			// scan end of word for label (:)
			if(!(t = next_whitespace(c)))
				return 0;
			if(*(t - 1) == ':') {
				if(!(c = asm_pass_label_directive(c, t - c, e, proc)))
					return 0;
			} else {
				// is it a DATA segment?
				if(!memcmp("DATA", c, 4)) {
					c = c + 5;
					// DATA contains direct text or \00 escape codes.
					while(*c && *c != '\n') {
						if(*c == '\\') {
							++c;
							c = mach_atoin_move(c, 16, &v, 2);
							*data++ = v;
						} else {
							*data++ = *c++;
						}
					}
				} else {
					// Any standard token
					c = t;
					++e;
				}
			}
		}
	}

	proc[P_Data] = (int)data;

	return 1;
}

// Emit pass:
//  1) Find any .DIRECTIVE entries and set appropriate flags
//  2) Emit opcodes to proc[P_E]
//  3) Emit data to proc[P_DATA]
//  4) Emit labels when found
int asm_pass_emit (int *proc, char *content) {
	char *c, *t, prev;
	int  *e, *l, tmp;
	char *data;

	c = next(content);
	e = (int*)proc[P_E];
	data = (char*)proc[P_Data];
	++e; // empty item seems to be emitted

	while(*c) {
		while (*c == '\n' || *c == '\r' || *c == ' ' || *c == 8) ++c;
		if (*c == ';') {
			// comment (to end of line)
			while (*c++ != '\n');
		}  else if (*c == '.') {
			++c;
			// .DIRECTIVE [some value]
			t = next_whitespace(c);
			if(!(c = asm_pass_scan_directive(c, t - c, t + 1, proc)))
				return 0;
		} else if(*c == '"') {
			// emit current data location
			*e++ = (int)data;
			// Emit strings to Data
			++c;
			t = c;
			prev = 0;
			while(*t && !(*t == '"' && prev != '\\')) { *data++ = *t; prev = *t; ++t; }
			*data++ = 0; // end of string marker
			c = ++t;
			++e;
		} else if(*c == '\'') {
			// emit a character
			printf("STUB: character emit not present\n");
			while(*++c != '\'');
			++e;
		} else if (*c == '-' || (*c >= '0') && *c <= '9') {
			// A number
			c = mach_atoi_move(c, 10, &tmp);
			*e++ = tmp;
		} else if (*c == '[') {
			// [expr...]
			if(!(c = asm_read_expression(c, &tmp, proc)))
				return 0;
			*e++ = tmp;
		} else {
			// scan end of word for label (:)
			if(!(t = next_whitespace(c)))
				return 0;
			if(*(t - 1) == ':') {
				if(!(c = asm_pass_label_directive(c, t - c, (int)e, proc)))
					return 0;
			} else {
				// is it a DATA segment?
				if(!memcmp("DATA", c, 4)) {
					// skip to end of line
					while(*c++ != '\n') ;
				} else {
					// normal emittion of code
					// lookup value
					if(!(l = label_find_strn(c, t - c))) {
						printf("Unknown label: %.*s\n", t - c, c);
						return 0;
					}
					*e++ = l[LBL_OFFSET];
					c = t + 1;
				}
			}
		}
	}

	proc[P_E] = (int)e;
	proc[P_Data] = (int)data;

	return 1;
}

int *compile_proc (char *content) {
	int failure, *proc, label_pos, *label, *sp, *t;

	if(!(proc = process_next())) return 0;

	failure = 0;
	memset(proc, 0, P__SZ * sizeof(int));
	proc[P_INUSE] = 1;
	if(!(proc[P_MM_SP] = (int)malloc(poolsz))) { failure = 1; }
	else if(!(proc[P_MM_E] = (int)malloc(poolsz))) { failure = 1; }
	else if(!(proc[P_MM_DATA] = (int)malloc(poolsz))) { failure = 1; }

	if (!failure) {
		proc[P_SP] = proc[P_BP] = (int)(int *)(proc[P_MM_SP] + poolsz);
		proc[P_E] = proc[P_MM_E];
		proc[P_Data] = proc[P_MM_DATA];
		memset((int*)proc[P_MM_SP], 0, poolsz);
		memset((int*)proc[P_MM_E], 0, poolsz);
		memset((char*)proc[P_MM_DATA], 0, poolsz);

		label_pos = label_idx; // remember for restoring afterwards
		if (asm_pass_scan(proc, content)) {
			if (asm_pass_emit(proc, content)) {
				// setup stack
				sp = (int*)proc[P_SP];
				*--sp = EXIT; // call exit if main returns
				*--sp = PSH; t = sp;
				*--sp = argc;
				*--sp = (int)argv;
				*--sp = (int)t;
				proc[P_SP] = sp;
			} else {
				printf("failed to emit assembly\n");
				failure = 1;
			}
		} else {
			printf("failed to scan assembly file\n");
			failure = 1;
		}
		label_idx = label_pos; // restore label position (erasing any created labels)
	}

	if(failure) {
		if(proc[P_MM_SP])   free((int*)proc[P_MM_SP]);
		if(proc[P_MM_E])    free((int*)proc[P_MM_E]);
		if(proc[P_MM_DATA]) free((int*)proc[P_MM_DATA]);
		proc[P_INUSE] = 0;
		return 0;
	}

	return proc;
}

int *start_asm (char *file) {
	char *buf;
	int  fd, bytes, *result;

	if (!(buf = malloc(poolsz))) return 0;
	if ( (fd = open(file, 0)) < 0) { free(buf); return 0; }

	if ((bytes = read(fd, buf, poolsz - 1)) <= 0) { close(fd); free(buf); return 0; }
	close(fd);
	buf[bytes] = 0;

	result = compile_proc(buf);
	free(buf);
	return result;
}

int *start_proc (char *file) {
	return start_asm(file);
}

int main(int _argc, char **_argv) {
	int tmp;

	argc = _argc;
	argv = _argv;
	if ((tmp = setup())) {
		printf("setup() failed: code %ld\n", tmp);
		return tmp;
	}

	start_proc(init);

	// main loop
	while (run_procs());

	cleanup();

	return 0;
}
