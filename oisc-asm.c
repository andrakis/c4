// One Instruction Set Computer
// Assembler
//
// Invocation: ./c4 c4_multiload.c oisc-min.c oisc-asm.c

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "oisc-min.c"

#define FunPtr(Return,ArgList,Ptr) ((Return(*)ArgList)Ptr)
#define Parent(arg1,arg2) FunPtr(int, (int, int), Parent)(arg1, arg2)
int callmember1 (int *Parent, int Member, int Arg1) { return Parent(Member, Arg1); }
#undef Parent
#define Parent(arg1,arg2,arg3) FunPtr(int, (int, int, int), Parent)(arg1, arg2, arg3)
int callmember2 (int *Parent, int Member, int Arg1, int Arg2) { return Parent(Member, Arg1, Arg2); }
#undef Parent

// Emitted code type
enum {
	ET_INTEGER,
	ET_REGISTER,
	ET_LABEL,
	ET_LOOKUP,
	ET_PLACEHOLDER,
	ET_GLOBAL,     // Global word
	ET_CUSTOMREG,  // Custom register
};

// Emitted code structure
enum {
	E_TYPE,     // int, see ET_*
	E_VAL,      // depends on E_TYPE
	E_ADDR,     // int, address of this item
	E__sz
};

// Label structure
enum {
	L_NAME,     // char*, unique
	L_ADDR,     // int*, Address, not unique
	L_PEND,     // int, whether we know the address yet or not
	L__sz
};

// Macro structure
// Macros are builtins only at this point.
enum {
	M_NAME,     // char*, unique
	M_ARGC,     // int, expected argument count
	M_ARGV,     // given arguments (as E_* structure)
	M_FUNC,     // int* (or int (*)(int *Macro)), callback function
	M__sz
};

// Exit status codes
enum {
	ES_OK,
	ES_MEMORY
};

// Globals
int *ec,        // Emitted code entries. See E_*
    *es,        // Emitted code start
     addr;      // Address counter
int *labels,    // Label entries. See L_*
     labcount;
int *oc, *os;   // Outputted code and start
int *macros;    // Macro entries

// Like strcmp
int  label_cmp (char *s1, int *label) {
	char *s2;
	s2 = (char*)label[L_NAME];
	while(*s1 && (*s1 == *s2)) {
		s1++;
		s2++;
	}
	return *s1 - *s2;
}
int  _labelname_uniqid;
int  _labelname_uniqidlen() {
	int len, n;
	len = 0; n = _labelname_uniqid;
	while(n) {
		++len;
		n = n / 10; // or whatever radix
	}
}
char*labelname_unique(char *prefix, char *suffix) {
	int name_max, idlen;
	char *name, *n;
	idlen = _labelname_uniqidlen();
	name_max = 1 + oisc_strlen(prefix) + oisc_strlen(suffix) + idlen;
	if(!(name = n = malloc(name_max))) return 0;
	// copy prefix into name
	while(*prefix)
		*n++ = *prefix++;
	// copy uniqueid into name
	n = oisc_itoa(_labelname_uniqid, n, 10); ++_labelname_uniqid;
	n = n + idlen;
	if(*n) printf("BUG: idlen miscount\n");
	// copy suffix into name
	while(*suffix)
		*n++ = *suffix++;
	// and trailing nul
	*n++ = 0;
	return name;
}
int *label_declare (char *name) {
	int *label;

	label = labels;
	while(label[L_NAME]) {
		if(label_cmp(name, label))
			return label;
		label = label + L__sz;
	}

	// Not found, create pending entry
	label[L_NAME] = (int)name;
	label[L_ADDR] = 0;
	label[L_PEND] = 1;
	++labcount;
	return label;
}
int *label_unique (char *prefix, char *suffix) {
	int *label;
	label = labels + (L__sz * labcount);
	label[L_NAME] = (int)labelname_unique(prefix, suffix);
	if(!label[L_NAME]) {
		printf("BUG: label_unique returned nul\n");
		return 0;
	}
	label[L_ADDR] = 0;
	label[L_PEND] = 1;
	++labcount;
	return label;
}
int *label_unique_here (char *prefix, char *suffix) {
	int *label;
	label = label_unique(prefix, suffix);
	if(!label) return 0;
	label[L_ADDR] = addr;
	label[L_PEND] = 0;
	return label;
}

// 
int ec_addrsize (int *ec) {
	// Labels have no size
	if(ec[E_TYPE] == ET_LABEL) return 0;
	// Everything else does
	return 1;
}
void emit_register (int reg) {
	ec[E_TYPE] = ET_REGISTER;
	ec[E_VAL ] = reg;
	ec[E_ADDR] = addr;
	addr = addr + ec_addrsize(ec);
	ec = ec + E__sz;
}
void emit_integer  (int val) {
	ec[E_TYPE] = ET_INTEGER;
	ec[E_VAL ] = val;
	ec[E_ADDR] = addr;
	addr = addr + ec_addrsize(ec);
	ec = ec + E__sz;
}
void emit_label (char *name) {
	int *label;
	label = label_declare(name);
	label[L_ADDR] = addr;
	label[L_PEND] = 0;

	ec[E_TYPE] = ET_LABEL;
	ec[E_VAL ] = (int)label;
	ec[E_ADDR] = addr;
	addr = addr + ec_addrsize(ec);
	ec = ec + E__sz;
}
void emit_lookup (char *name) {
	int *label;
	label = label_declare(name);
	
	ec[E_TYPE] = ET_LOOKUP;
	ec[E_VAL ] = (int)label;
	ec[E_ADDR] = addr;
	addr = addr + ec_addrsize(ec);
	ec = ec + E__sz;
}
void emit_lookup_label (int *label) {
	ec[E_TYPE] = ET_LOOKUP;
	ec[E_VAL ] = (int)label;
	ec[E_ADDR] = addr;
	addr = addr + ec_addrsize(ec);
	ec = ec + E__sz;
}
void emit_placeholder() {
	ec[E_TYPE] = ET_PLACEHOLDER;
	ec[E_ADDR] = addr;
	addr = addr + ec_addrsize(ec);
	ec = ec + E__sz;
}
// Copies given arg (type E_*) to output
void emit_arg (int *arg) {
	ec[E_TYPE] = arg[E_TYPE];
	ec[E_VAL ] = arg[E_VAL];
	ec[E_ADDR] = addr;
	addr = addr + ec_addrsize(ec);
	ec = ec + E__sz;
}
void ec_puts (int *e) {
	int type; type = e[E_TYPE];
	if(type == ET_INTEGER)       printf("%lld", e[E_VAL]);
	else if(type == ET_REGISTER) Debug_PrintSymbol(e[E_VAL]);
	else if(type == ET_LABEL)    printf("%s:", (char*)e[E_VAL]);
	else if(type == ET_LOOKUP)   printf(":%s", (char*)e[E_VAL]);
	else if(type == ET_PLACEHOLDER)
		                         printf("<PH>");
	else                         printf("<unknown ET_TYPE %lld>", e[E_TYPE]);
}

//////
// Macros: builtins
//////
int MACRO_OBSERVE(int argc, int *arg) {
	if(argc != 1) {
		return 1; // err
	}
	if(arg[E_TYPE] == ET_REGISTER) {
		// Move into itself
		// :0, 0, :0
		emit_arg(arg);
		emit_integer(0);
		emit_arg(arg);
	} else if(arg[E_TYPE] == ET_INTEGER) {
		// Add from zero register to a local word
		// Z, :0, macro_observe_intregister
		emit_register(Z);
		emit_arg(arg);
		emit_register(macro_observe_intregister);
	}
	return 0;
}
int MACRO_JUMP(int argc, int *arg) {
	if(argc != 1) return 1;
	if(arg[E_TYPE] == ET_INTEGER || arg[E_TYPE] == ET_LOOKUP) {
		// Z, :0, PC
		emit_register(Z);
		emit_arg(arg);
		emit_register(PC);
	} else if(arg[E_TYPE] == ET_REGISTER) {
		// :0, 0, PC
		emit_arg(arg);
		emit_integer(0);
		emit_register(PC);
	}
	return 0;
}
int MACRO_IF(int argc, int *args) {
	int *regarg, *truearg, *falsearg;
	int *label, reg;
	if(argc != 3) return 1;
	regarg   = args;
	truearg  = args + (E__sz * 1);
	falsearg = args + (E__sz * 2);
	reg = regarg[E_VAL];
	if(!isflag(reg)) {
		MACRO_OBSERVE(1, regarg);
		reg = EQ0;
		// reuse label temporarily to swap
		label = truearg;
		truearg = falsearg;
		falsearg = label;
	}

	// reg, :0, PC
	emit_register(reg);
	label = label_unique("IF_", "");
	emit_lookup((char*)label[L_NAME]);
	emit_register(PC);

	// 0:
	label[L_ADDR] = addr;
	label[L_PEND] = 0;
	// JUMP(truelabel)
	MACRO_JUMP(1, truearg);
	// JUMP(falselabel)
	MACRO_JUMP(1, falsearg);
	return 0;
}
int MACRO_DEREFERENCE(int argc, int *args) {
	int *from, *to;
	int *label;
	if(argc != 2) return 1;
	from = args;
	to   = args + E__sz;
	// from, 0, :0
	emit_arg(from);
	emit_integer(0);
	label = label_unique("DEREF_", "");
	emit_lookup_label(label);
	// 0:   PH   value read in from
	//      0
	//      to
	label[L_ADDR] = addr;
	label[L_PEND] = 0;
	emit_placeholder();
	emit_integer(0);
	emit_arg(to);
	return 0;
}

int attempted_size;
int init_memory () {
	// Allow for 256K emitted structures
	attempted_size = sizeof(int) * E__sz * 256 * 1024;
	if(!(ec = es = malloc(attempted_size))) {
		return 1;
	}

	// Allow for 1024 labels
	attempted_size = sizeof(int) * L__sz * 1024;
	if(!(labels = malloc(attempted_size))) {
		free(ec);
		return 1;
	}
	memset(labels, 0, attempted_size);

	// Allow for 256K words output code
	attempted_size = sizeof(int) * 256 * 1024;
	if(!(oc = os = malloc(attempted_size))) {
		free(ec);
		free(labels);
		return 1;
	}
	memset(oc, 0, attempted_size);
	return 0;
}

int main(int argc, char **argv) {
	if(init_memory()) {
		printf("Failed to allocate %lld bytes\n", attempted_size);
		return ES_MEMORY;
	}

	// Cleanup
	free(es);
	free(labels);
}
