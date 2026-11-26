// types.h -- c4cxx's C types (M2), laid out as c4lc's 32-bit target lays
// them out, which is gcc -m32's System V i386 layout: int, long and pointers
// 4 bytes, long long and double 8 with 4-byte alignment in a struct,
// #pragma pack and bit-fields as gcc places them.
#ifndef C4CXX_TYPES_H
#define C4CXX_TYPES_H
#include "ast.h"

typedef struct Ty Ty;
typedef struct Field Field;

enum {
	T_VOID, T_BOOL, T_CHAR, T_SCHAR, T_UCHAR, T_SHORT, T_USHORT, T_INT, T_UINT,
	T_LONG, T_ULONG, T_LLONG, T_ULLONG, T_FLOAT, T_DOUBLE, T_LDOUBLE,
	T_ENUM, T_PTR, T_ARRAY, T_FUNC, T_STRUCT, T_UNION
};

struct Field {
	Field *next;
	Token *name;		// 0: an anonymous struct/union member, or a padding bit-field
	Ty *ty;
	int offset;		// bytes
	int bit_offset;		// within the storage unit at offset, for a bit-field
	int bit_width;		// -1 if not a bit-field
};

struct Ty {
	int kind;
	int size;		// -1 if incomplete
	int align;		// as a struct member
	int qual;		// Q_CONST, Q_VOLATILE
	Ty *base;		// T_PTR, T_ARRAY: the target; T_FUNC: the return
	int len;		// T_ARRAY: elements, -1 if not given
	Ty **params;		// T_FUNC
	int nparams;
	int variadic;
	int oldstyle;		// T_FUNC: no prototype
	Ty *def;		// T_STRUCT, T_UNION, T_ENUM: the one record of the tag
	// on the record (def == self):
	Token *tag;
	char *name;		// a typedef name that names it, when it has no tag
	Field *fields;
	int complete;
	int is_unsigned_enum;	// T_ENUM: all values >= 0 (gcc then makes it unsigned int)
};

extern Ty *ty_void, *ty_bool, *ty_char, *ty_schar, *ty_uchar, *ty_short, *ty_ushort,
	*ty_int, *ty_uint, *ty_long, *ty_ulong, *ty_llong, *ty_ullong,
	*ty_float, *ty_double, *ty_ldouble;

void types_init (void);
Ty *new_ty (int kind, int size, int align);
Ty *pointer_to (Ty *t);
Ty *array_of (Ty *t, int len);
Ty *func_type (Ty *ret);
Ty *qualified (Ty *t, int qual);	// a copy with these qualifiers added
Ty *unqualified (Ty *t);
int is_integer (Ty *t);
int is_arith (Ty *t);
int is_scalar (Ty *t);
int is_ptr (Ty *t);			// pointer (not array)
int is_unsigned (Ty *t);
int same_type (Ty *a, Ty *b);		// compatible, ignoring top-level qualifiers
Ty *decay (Ty *t);			// arrays and functions as operands
Ty *promote (Ty *t);			// the integer promotions
Ty *usual_arith (Ty *a, Ty *b);
void layout (Ty *rec, int pack);	// fields set: offsets, size, align
char *ty_str (Ty *t, const char *inner);	// as C; 0 if it cannot be named
Field *find_field (Ty *rec, Token *name, int *offset);

#endif
