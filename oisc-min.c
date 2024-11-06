#include <memory.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#pragma GCC diagnostic ignored "-Wformat"
#ifdef __GNUC__
#include <unistd.h>
#else
#if _WIN64
#define __INTPTR_TYPE__ long long
#elif _WIN32
#define __INTPTR_TYPE__ int
#endif // if _WIN64
#endif // ifdef __GNUC__

#ifndef int
// Please define this for your architecture if required.
#define int __INTPTR_TYPE__
#endif // ifndef int

int DEBUG;

//////
// Magic numbers demystified
//////

enum {
	RANGE_REGS_START   =     0,
	RANGE_REGS_END     =  1024,
	RANGE_MEM_START    =  1024,
};

enum {
	INST_SIZE = 3,    // Instruction size
	PH        = 99    // Placeholder mark: *?
};

//////
// VM globals
//////
int *devs;      // Devices
int  devcount;  // Device count
int *registers; // Register memory
int  regcount;  // Register count
int *memory;    // General  memory
int  wr;        // Word read size
int  ww;        // Word write size

// Additional registers
int  macro_observe_intregister;

//////
// Structures as enums, the C4 way
//////

// VM registers
enum {
	Z,           // Always contains 0
	PC,          // Code pointer
	R0,          // General purpose
	R1,          // General purpose
	R2,          // General purpose
	R3,          // General purpose
	// TODO: This register isn't used
	AC,          // Accumulator
	EQ0,         // Result == 0
	LT0,         // Result <  0
	GT0,         // Result >  0
    // Word mode flags
    WR,          // Word read
    WW,          // Word write
	// Temporary stores: (used by VM_Cycle())
	TV,          //
	TPC,         // 
	TSRC,        // Instruction Source register
	TADD,        // Instruction Add register
	TDST,        // Instruction Dest register
	REGS__Sz     // Number of registers
};

// Word mode flags
enum { WM_INT = 0, WM_CHAR = 1 };

// Device structure
enum {
	DEV_ID,        // int
	DEV_FLAGS,     // int, see DEVF_*
	DEV_RCALLBACK, // int* (really: int (*)())
	DEV_WCALLBACK, // int* (really: int (*)(int))
	DEV__Sz
};

// IO ports
enum {
	IO_PUTC = 100,
	IO_SYSCALL = 110,
	IO_MATH_X  = 200,   // X operand
	IO_MATH_Y  = 201,   // Y operand
	IO_MATH_OP = 202,   // operand (see MO_*)
	IO_MATH_V  = 203    // Value after calculation
};
enum { MAX_DEVICES = 32 };

// Syscalls
enum {
	SYS_OPEN, SYS_READ, SYS_CLOS, SYS_PRTF, SYS_MALC, SYS_FREE,
	SYS_MSET, SYS_MCMP, SYS_EXIT
};

// Math operations
enum {
	MATH_OR  ,MATH_XOR ,MATH_AND ,MATH_EQ  ,MATH_NE  ,
	MATH_LT  ,MATH_GT  ,MATH_LE  ,MATH_GE  ,MATH_SHL ,
	MATH_SHR ,MATH_ADD ,MATH_SUB ,MATH_MUL ,MATH_DIV , MATH_MOD
};

//////
// Utility
//////

int isflag(int reg) {
	return reg == EQ0 || reg == LT0 || reg == GT0;
}
int isflagC4(int *reg) {
	return reg == &registers[EQ0] ||
	       reg == &registers[LT0] ||
	       reg == &registers[GT0];
}

void Debug_PrintSymbol(int s) {
	char *m;
	if(s < RANGE_REGS_END) {
		if(s <= regcount) {
			if(s == Z)         m = "Z     ";
			else if(s == PC)   m = "PC    ";
			else if(s == R0)   m = "R0    ";
			else if(s == R1)   m = "R1    ";
			else if(s == R2)   m = "R2    ";
			else if(s == R3)   m = "R3    ";
			else if(s == AC)   m = "AC    ";
			else if(s == EQ0)  m = "EQ0   ";
			else if(s == LT0)  m = "LT0   ";
			else if(s == GT0)  m = "GT0   ";
			else if(s == macro_observe_intregister)
				               m = "observ";
			else               m = "<ERRR>";
			printf("%s", m);
		} else if(s == PH) {
			printf("*?    ");
		} else {
			printf("Port(%lld)", s);
		}
	} else {
		printf("[%lld]", s);
	}
}
void Debug_PrintSymbolC4(int *s) {
	char *m;
	if((int)s > 0 && (int)s < RANGE_REGS_END) {
		if((int)s == PH)              printf("PH          ");
		else if((int)s == IO_PUTC)    printf("IO_PUTC     ");
		else if((int)s == IO_SYSCALL) printf("IO_SYSCALL  ");
		else if((int)s == IO_MATH_X)  printf("IO_MATH_X   ");
		else if((int)s == IO_MATH_Y)  printf("IO_MATH_Y   ");
		else if((int)s == IO_MATH_OP) printf("IO_MATH_OP  ");
		else if((int)s == IO_MATH_V)  printf("IO_MATH_V   ");
        else                          printf("Prt.%-4lld    ", (int)s);
    } else {
        if(s == &registers[Z])         m = "Z           ";
        else if(s == &registers[PC])   m = "PC          ";
        else if(s == &registers[R0])   m = "R0          ";
        else if(s == &registers[R1])   m = "R1          ";
        else if(s == &registers[R2])   m = "R2          ";
        else if(s == &registers[R3])   m = "R3          ";
        else if(s == &registers[AC])   m = "AC          ";
		else if(s == &registers[WR])   m = "WR          ";
		else if(s == &registers[WW])   m = "WW          ";
        else if(s == &registers[EQ0])  m = "EQ0         ";
        else if(s == &registers[LT0])  m = "LT0         ";
        else if(s == &registers[GT0])  m = "GT0         ";
        else if(s == &macro_observe_intregister)
                           m = "observ";
        else               m = 0;

		if(m != 0) printf("%s", m);
		else       printf("%.12llx", (int)s);
	}
}

// Sourced from: https://gist.github.com/ccbrown/9722406
// Adjusted for C4 (replaced for loops with while loops, no arrays on stack)
void dump_hex (char *data, int size) {
	char *ascii;
	int i, j;
	// Inefficient hack because C4 can't do arrays on the stack
	ascii = malloc(sizeof(char) * 17);
	if(!ascii) return;

	ascii[16] = 0;
	//for (i = 0; i < size; ++i) {
	i = 0;
	while(i < size) {
		printf("%02X ", ((char*)data)[i]);
		if (((char*)data)[i] >= ' ' && ((char*)data)[i] <= '~') {
			ascii[i % 16] = ((char*)data)[i];
		} else {
			ascii[i % 16] = '.';
		}
		if ((i+1) % 8 == 0 || i+1 == size) {
			printf(" ");
			if ((i+1) % 16 == 0) {
				printf("|  %s \n", ascii);
			} else if (i+1 == size) {
				ascii[(i+1) % 16] = 0;
				if ((i+1) % 16 <= 8) {
					printf(" ");
				}
				//for (j = (i+1) % 16; j < 16; ++j) {
				j = (i+1) % 16;
				while(j < 16) {
					printf("   ");
					++j;
				}
				printf("|  %s \n", ascii);
			}
		}
		++i;
	}

	free(ascii);
}

int oisc_strlen(char *s) {
	int len; len = 0;
	while(*s++) ++len;
	return len;
}
void  oisc_swapchar(char *x, char *y) { char t; t = *x; *x = *y; *y = t; }
char* oisc_reverse(char *buffer, int i, int j) {
	while (i < j) {
		oisc_swapchar(&buffer[i++], &buffer[j--]);
	}
	return buffer;
}
int oisc_abs(int v) { return v >= 0 ? v : -v; }
// Iterative function to implement `itoa()` function in C
char* oisc_itoa(int value, char* buffer, int base)
{
	int n, i, r;
	// invalid input
	if (base < 2 || base > 32) {
		return buffer;
	}

	// consider the absolute value of the number
	n = oisc_abs(value);

	i = 0;
	while (n) {
		r = n % base;
		if (r >= 10) {
			buffer[i++] = 65 + (r - 10);
		} else {
			buffer[i++] = 48 + r;
		}
		n = n / base;
	}

	// if the number is 0
	if (i == 0) {
		buffer[i++] = '0';
	}

	// If the base is 10 and the value is negative, the resulting string
	// is preceded with a minus sign (-)
	// With any other base, value is always considered unsigned
	if (value < 0 && base == 10) {
		buffer[i++] = '-';
	}

	buffer[i] = 0; // null terminate string

	// reverse the string and return it
	return oisc_reverse(buffer, 0, i - 1);
}

//////
// Devices
//////


//
// IO_PUTC device
//
int dev_ioputc_read () { return 0; } // No effect
int dev_ioputc_write (int v) { printf("%c", (char)v); return v; }

//
// IO_SYSCALL device
//
int syscall_result;
int dev_iosyscall_read () { return syscall_result; }
// Expects R0 to be loaded with stack pointer.
// Printf expects R1 to be the number of arguments to printf.
int dev_iosyscall_write(int val) {
	int *sp, *t;
	sp = (int*)registers[R0];
	     if(val == SYS_OPEN) syscall_result = open((char*)sp[1], *sp);
	else if(val == SYS_READ) syscall_result = read(sp[2], (char*)sp[1], *sp);
	else if(val == SYS_CLOS) syscall_result = close(*sp);
	else if(val == SYS_PRTF) { t = sp + registers[R1];
	                           syscall_result = printf((char*)t[-1], t[-2], t[-3], t[-4], t[-5], t[-6]); }
	else if(val == SYS_MALC) syscall_result = (int)malloc(*sp);
	else if(val == SYS_FREE) free((void*)*sp);
	else if(val == SYS_MSET) syscall_result = (int)memset((char *)sp[2], sp[1], *sp);
	else if(val == SYS_MCMP) syscall_result = memcmp((char *)sp[2], (char *)sp[1], *sp);
	else if(val == SYS_EXIT) { printf("syscall exit(%lld)\n", *sp); registers[PC] = 0; }
	else { printf("syscall invalid: %lld\n", *sp); registers[PC] = 0; }
}

//
// Math device
//
int dev_iomath_x, dev_iomath_y, dev_iomath_op, dev_iomath_v;
int dev_iomath_x_read ()       { return dev_iomath_x; }
int dev_iomath_x_write (int v) { return dev_iomath_x = v; }
int dev_iomath_y_read ()       { return dev_iomath_y; }
int dev_iomath_y_write (int v) { return dev_iomath_y = v; }
int dev_iomath_op_read ()      { return dev_iomath_op; }
int dev_iomath_op_write (int v){
	// Performs the actual calculation and stores in _v
	dev_iomath_op = v;
	     if(v == MATH_OR)  dev_iomath_v = dev_iomath_x |  dev_iomath_y;
	else if(v == MATH_XOR) dev_iomath_v = dev_iomath_x ^  dev_iomath_y;
	else if(v == MATH_AND) dev_iomath_v = dev_iomath_x &  dev_iomath_y;
	else if(v == MATH_EQ)  dev_iomath_v = dev_iomath_x == dev_iomath_y;
	else if(v == MATH_NE)  dev_iomath_v = dev_iomath_x != dev_iomath_y;
	else if(v == MATH_LT)  dev_iomath_v = dev_iomath_x <  dev_iomath_y;
	else if(v == MATH_GT)  dev_iomath_v = dev_iomath_x >  dev_iomath_y;
	else if(v == MATH_LE)  dev_iomath_v = dev_iomath_x <= dev_iomath_y;
	else if(v == MATH_GE)  dev_iomath_v = dev_iomath_x >= dev_iomath_y;
	else if(v == MATH_SHL) dev_iomath_v = dev_iomath_x << dev_iomath_y;
	else if(v == MATH_SHR) dev_iomath_v = dev_iomath_x >> dev_iomath_y;
	else if(v == MATH_ADD) dev_iomath_v = dev_iomath_x +  dev_iomath_y;
	else if(v == MATH_SUB) dev_iomath_v = dev_iomath_x -  dev_iomath_y;
	else if(v == MATH_MUL) dev_iomath_v = dev_iomath_x *  dev_iomath_y;
	else if(v == MATH_DIV) dev_iomath_v = dev_iomath_x /  dev_iomath_y;
	else { printf("math device: invalid op %ld\n", v); }
}
int dev_iomath_v_read ()       { return dev_iomath_v; }
int dev_iomath_v_write (int v) { return dev_iomath_v; } // does nothing

//
// Device interface
//
int *dev_find (int id) {
	int *d, c;
	d = devs; c = 0;
	while(c++ < devcount)
		if(d[DEV_ID] == id) return d;
		else d = d + DEV__Sz;
	return 0;
}
int dev_handle_read (int id) {
	int *dev, *handler;
	if(!(dev = dev_find(id))) {
		printf("!!Port %lld: no device present for read\n", id);
		return 0;
	}
	handler = (int*)dev[DEV_RCALLBACK];
#define handler() ((int(*)())handler)()
	return handler();
#undef handler
}

int dev_handle_write (int val, int id) {
	int *dev, *handler;
	if(!(dev = dev_find(id))) {
		printf("!!Port %lld: no device present for write\n", id);
		return 0;
	}
	handler = (int*)dev[DEV_WCALLBACK];
#define handler(v) ((int(*)(int))handler)(v)
	return handler(val);
#undef handler
}

// Initialize the various port handlers
void oisc_init_devices () {
	int *d;
	d = devs;

	// IO_PUTC
	d[DEV_ID] = IO_PUTC;
	d[DEV_RCALLBACK] = (int)&dev_ioputc_read;
	d[DEV_WCALLBACK] = (int)&dev_ioputc_write;
	d = d + DEV__Sz; ++devcount;

	// IO_SYSCALL
	d[DEV_ID] = IO_SYSCALL;
	d[DEV_RCALLBACK] = (int)&dev_iosyscall_read;
	d[DEV_WCALLBACK] = (int)&dev_iosyscall_write;
	d = d + DEV__Sz; ++devcount;

	// IO_MATH and supporting ports
	// - IO_MATH_X
	d[DEV_ID] = IO_MATH_X;
	d[DEV_RCALLBACK] = (int)&dev_iomath_x_read;
	d[DEV_WCALLBACK] = (int)&dev_iomath_x_write;
	d = d + DEV__Sz; ++devcount;
	// - IO_MATH_Y
	d[DEV_ID] = IO_MATH_Y;
	d[DEV_RCALLBACK] = (int)&dev_iomath_y_read;
	d[DEV_WCALLBACK] = (int)&dev_iomath_y_write;
	d = d + DEV__Sz; ++devcount;
	// - IO_MATH_OP
	d[DEV_ID] = IO_MATH_OP;
	d[DEV_RCALLBACK] = (int)&dev_iomath_op_read;
	d[DEV_WCALLBACK] = (int)&dev_iomath_op_write;
	d = d + DEV__Sz; ++devcount;
	// - IO_MATH_V
	d[DEV_ID] = IO_MATH_V;
	d[DEV_RCALLBACK] = (int)&dev_iomath_v_read;
	d[DEV_WCALLBACK] = (int)&dev_iomath_v_write;
	d = d + DEV__Sz; ++devcount;
}

//////
// VM
//////

int VM_Cycle () {
	int pc, src, add, dst, v;
	pc = registers[PC];
	// C4 optimization: don't post increment index value.
	// In C4, memory[pc++] involves incrementing the index value,
	// then subtracting 1 for it to get the proper index value.
	src = memory[pc]; ++pc;
	add = memory[pc]; ++pc;
	dst = memory[pc]; ++pc;
	if(DEBUG) {
		printf("PC:%lld%c%c", registers[PC], 9, 9);
		Debug_PrintSymbol(src);
		printf(" + %lld%c-> ", add, 9);
		Debug_PrintSymbol(dst);
	}
	registers[PC] = pc;
	if(src < RANGE_REGS_END) {
		if(src <= regcount) {
			// Register access
			v = registers[src] + add;
		} else {
			// Ports access
			printf("TODO: port read %lld\n", src);
			v = 0;
		}
	} else {
		// Regular memory access
		v = memory[src] + add;
	}
	if(dst < RANGE_REGS_END) {
		if(dst <= regcount) {
			// Register access
			registers[dst] = v;
		} else {
			// Ports access
			if(dst == IO_PUTC) printf("%c", (char)v);
			else printf("TODO: port write '%lld (%c)'\n", v, (char)v);
		}
	} else {
		// Regular memory access
		memory[dst] = v;
	}
	registers[EQ0] = v == 0 ? INST_SIZE : 0;
	registers[LT0] = v <  0 ? INST_SIZE : 0;
	registers[GT0] = v >  0 ? INST_SIZE : 0;
	if(DEBUG) {
		printf("%c%c", (char)9, (char)9);
		printf("V=%lld", v);
		printf("%c", (char)9);
		printf("EQ0=%lld LT0=%lld GT0=%lld\n", registers[EQ0], registers[LT0], registers[GT0]);
	}
	return v;
}

int Debug_PrintHandlerC4 (int *pc, int *src, int add, int *dst) {
	printf("PC:%.12llx:%c%c", (int)pc, 9, 9);
	Debug_PrintSymbolC4(src);
	printf(" + ");
	if(add == 0) printf("           0");
	else if(add < 10000) printf("%12.lld", add);
	else printf("%12.llx", add);
	printf("%c-> ", 9);
	Debug_PrintSymbolC4(dst);
}

int INST_SIZE_C4;
int *debug_printhandler; // int (*)(Src,Add,Dst)
// This implementation uses absolute memory pointers.
int VM_CycleC4 () {
	int *pc, *src, add, *dst, v;
	pc = (int*)registers[PC];
	// C4 optimization: don't post increment index value.
	// In C4, memory[pc++] involves incrementing the index value,
	// then subtracting 1 for it to get the proper index value.
	src = (int*)*pc; ++pc;
	add = *pc; ++pc;
	dst = (int*)*pc; ++pc;
	if(DEBUG) {
#define debug_printhandler(a,b,c,d) ((int(*)(int*,int*,int,int*))debug_printhandler)(a,b,c,d)
		debug_printhandler(pc, src, add, dst);
#undef  debug_printhandler
	}
	registers[PC] = (int)pc;
	if((int)src < RANGE_REGS_END) {
        // Ports access
        //printf("port read %lld\n", (int)src);
        v = dev_handle_read((int)src);
	} else {
		// Regular memory access
        if(registers[WR] == WM_CHAR) v = (int)*((char*)src);
		else v = *src;
	}

	v = v + add;

	if((int)dst < RANGE_REGS_END) {
        // Ports access
        //printf("port write '%lld (%c)'\n", v, (char)v);
		v = dev_handle_write(v, (int)dst);
	} else {
		// Regular memory access
        if(registers[WW] == WM_CHAR) *((char*)dst) = (char)v;
        else *dst = v;
	}
	registers[EQ0] = v == 0 ? INST_SIZE_C4 : 0;
	registers[LT0] = v <  0 ? INST_SIZE_C4 : 0;
	registers[GT0] = v >  0 ? INST_SIZE_C4 : 0;
	if(DEBUG) {
		printf("%c%c", (char)9, (char)9);
		printf("V=%.12llx", v);
		printf("%c", (char)9);
		printf("EQ0=%.2lld LT0=%.2lld GT0=%.2lld\n", registers[EQ0], registers[LT0], registers[GT0]);
	}
	return v;
}

//////
// VM interface
//////

// Returns 0 on success, 1 on failure
int oisc_init() {
	int memsize, tmp;

    DEBUG = 1;
	INST_SIZE_C4 = sizeof(int) * 3;

	devcount = 0;
	if(!(devs = malloc(sizeof(int) * DEV__Sz * MAX_DEVICES))) {
		printf("Failed to allocate memory for devices\n");
		return 1;
	}
	oisc_init_devices();

	memsize = 64 * 1024;
	if (!(memory = malloc(tmp = sizeof(int) * memsize))) {
		printf("Failed to allocate %lld bytes for memory\n", tmp);
		free(devs);
		return 1;
	}
	memset(memory, 0, sizeof(int) * memsize);
	regcount = REGS__Sz; // Allow additional registers
	macro_observe_intregister = ++regcount;
	registers = memory;  // Really should be separate, but for ease of implementation
	memset(registers, 0, sizeof(int) * regcount);
	if(DEBUG) {
		printf("Register count: %lld\n", regcount);
	}

    // Set word sizes
    wr = ww = WM_INT;

	// Set debug handler
	debug_printhandler = (int *)&Debug_PrintHandlerC4;

	return 0;
}

// Run until exit, returns R0
int oisc_run () {
	while(registers[PC])
		VM_Cycle();
	return registers[R0];
}

int oisc4_run () {
	while(registers[PC])
		VM_CycleC4();
	return registers[R0];
}

void oisc_cleanup() {
	if(registers != memory) free(registers);
	free(memory);
	free(devs);
}

void oisc_dump(int *es, int *e, int offset) {
	int *i, x;
	i = es;
	x = (int)(e - es);
	printf("Code length = %lld words (%lld bytes):\n", x, sizeof(int) * x);
	while(i < e) {
		printf("%lld ", *i++);
	}
	printf("\n");
	//dump_hex((char*)es, sizeof(int) * (e - es));
	i = es + (registers[PC] - offset);
	while(i < e) {
		printf("%lld:%c%c", offset + (i - es), 9, 9);
		Debug_PrintSymbol(*i++);
		printf(" + %lld%c-> ", *i++, 9);
		Debug_PrintSymbol(*i++);
		printf("\n");
	}
	printf("\n");
}

void oisc_dumpC4(int *es, int *e, int *entry) {
	int *i, x;
	i = es;
	x = (int)(e - es);
	printf("Code length = %lld words (%lld bytes):\n", x, sizeof(int) * x);
	while(i < e) {
		printf("%lld ", *i++);
	}
	printf("\n");
	//dump_hex((char*)es, sizeof(int) * (e - es));
	i = (int*)entry;
	while((i - entry) < x) {
		printf("   %.12llx:%c%c", (int)i, 9, 9);
		Debug_PrintSymbolC4((int*)*i++);
		printf(" + ");
		if(*i == 0) printf("           0");
		else if(*i < 10000) printf("%12.lld", *i);
		else printf("%12.llx", *i);
		i++;
		printf("%c-> ", 9);
		Debug_PrintSymbolC4((int*)*i++);
		printf("\n");
	}
	printf("\n");
}
