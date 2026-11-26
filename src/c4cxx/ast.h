// ast.h -- c4cxx's syntax tree (M1b: C as c4lc takes it)
//
// A declaration keeps its specifiers (Spec, shared by its declarators) and
// each declarator as a chain of derived types (pointer, array, function)
// ending at the specifiers (TY_BASE), so that it prints as it was written:
// a struct's body where it was defined, a typedef name as the name.
#ifndef C4CXX_AST_H
#define C4CXX_AST_H
#include "c4cxx.h"

typedef struct Spec Spec;
typedef struct Type Type;
typedef struct Decl Decl;
typedef struct Node Node;
typedef struct Init Init;
typedef struct InitItem InitItem;
typedef struct Desig Desig;
typedef struct Attr Attr;
typedef struct Enumer Enumer;
struct Ty;

// a GNU __attribute__((...)) (or __declspec, asm label), kept as its tokens
struct Attr { Attr *next; Token *start; Token *end; };

enum { ST_TYPEDEF = 1, ST_EXTERN = 2, ST_STATIC = 4, ST_AUTO = 8, ST_REGISTER = 16, ST_INLINE = 32 };
enum { Q_CONST = 1, Q_VOLATILE = 2, Q_RESTRICT = 4 };
enum { SP_WORDS, SP_STRUCT, SP_UNION, SP_ENUM };

struct Enumer { Enumer *next; Token *name; Node *value; long long val; };

struct Spec {
	int storage;		// ST_ bits
	int qual;		// Q_ bits
	int kind;		// SP_
	Token **words;		// SP_WORDS: void, int, unsigned..., or a typedef name
	int nwords;
	int is_typedef_name;	// words[0] is a typedef name
	Token *tag;		// struct/union/enum
	int has_body;
	Decl *members;		// struct/union members (with their own Specs)
	Enumer *enumers;
	Attr *attrs;		// attributes among the specifiers
	Attr *tag_attrs;	// struct __attribute__((packed)) tag
	Token *tok;
	struct Ty *rty;		// sema: the type the specifiers name
};

enum { TY_BASE, TY_PTR, TY_ARRAY, TY_FUNC };

struct Type {
	int kind;
	Type *base;		// what it points to, holds or returns
	int qual;		// TY_PTR: qualifiers after the '*'
	Attr *attrs;		// TY_PTR: attributes after the '*'
	Node *len;		// TY_ARRAY: the size, or 0
	Decl *params;		// TY_FUNC: (each with its own Spec)
	int variadic;		// TY_FUNC: ...
	int oldstyle;		// TY_FUNC: () or an identifier list
	int paren;		// written inside its own parentheses
};

struct Decl {
	Decl *next;
	Spec *spec;
	Type *type;		// 0 or a chain ending at TY_BASE
	Token *name;		// 0 if abstract (or a member with a width only)
	Node *width;		// bit-field width
	Init *init;
	Attr *attrs;		// after the declarator
	Node *body;		// a function definition
	Decl *kr;		// K&R parameter declarations
	Token *tok;
	int is_pragma;		// a #pragma inside a struct body (tok)
	int first_of_spec;	// the first declarator of its Spec (prints the Spec)
	struct Ty *rty;		// sema: its type
};

struct Desig { Desig *next; Token *field; Node *index; };
struct InitItem { InitItem *next; Desig *desig; Init *init; };
struct Init { Node *expr; InitItem *items; int is_list; Token *tok; };

enum {
	// expressions
	ND_NUM, ND_CHAR, ND_STR, ND_IDENT,
	ND_BINARY, ND_ASSIGN, ND_PREFIX, ND_POSTFIX, ND_COND, ND_COMMA,
	ND_CALL, ND_INDEX, ND_MEMBER,		// MEMBER: op is "." or "->"
	ND_CAST, ND_SIZEOF_TYPE, ND_SIZEOF_EXPR, ND_COMPLIT,
	// statements
	ND_BLOCK, ND_EXPR_STMT, ND_DECL, ND_IF, ND_WHILE, ND_DO, ND_FOR,
	ND_SWITCH, ND_CASE, ND_DEFAULT, ND_BREAK, ND_CONTINUE, ND_RETURN,
	ND_GOTO, ND_LABEL, ND_NULL, ND_PRAGMA,
	// top level
	ND_FUNCDEF
};

struct Node {
	int kind;
	Token *tok;		// where it starts (and the literal / name / op)
	Token *op;		// the operator
	int ntoks;		// ND_STR: how many adjacent string tokens
	Node *lhs, *rhs, *cond, *then, *els, *inc, *body;
	Node *init_expr;	// ND_FOR: an expression init
	Decl *decl;		// ND_DECL / ND_FOR's declaration / ND_FUNCDEF / type names
	Node *args;		// ND_CALL
	Node *next;		// in a list (arguments, statements)
	Init *initz;		// ND_COMPLIT
	Token *name;		// ND_MEMBER, ND_GOTO, ND_LABEL
	// sema
	struct Ty *ty;		// the expression's type, before arrays decay
	int lvalue;
	int bitfield;		// a bit-field member
	int is_const;		// an integer constant expression, of value val
	long long val;
	int implicit;		// ND_CALL: of a function declared by the call
	int cvar;		// is_const through a const variable (C++'s rule, not C's)
	int unsure;		// typed by a fallback (a c4lc-ism gcc would refuse)
};

// parse.c
Node *parse (Token *t);		// the unit: ND_DECL, ND_FUNCDEF, ND_PRAGMA in a list
// sema.c
void sema (Node *unit);
// emit.c
extern int emit_check;		// -check-types: add assertions of the types for gcc
void emit_unit (FILE *out, Node *unit);

#endif
