//
// c4r.h - Main interface for C4 Relocatable
// Also provides C4R_CONSTRUCTOR and C4R_DESTRUCTOR macros.
//
#ifndef __C4R_H
#define __C4R_H 1

#define C4R_CONSTRUCTOR(name,c4r,syscall) static void __attribute__((constructor)) name (int *c4r, int *syscall)
#define C4R_DESTRUCTOR(name)              static void __attribute__((destructor))  name ()

#ifndef __c4cc__
// GCC doesn't like our attributes
#pragma GCC diagnostic ignored "-Wattributes"
#endif

enum { C4R__Supported_Version = 2 };

enum {
	C4ROPT_NONE,
	C4ROPT_SYMBOLS
};

#ifndef FILE_OPEN_MODE
enum { FILE_OPEN_MODE = 0x8000 };
#endif

//
// C4R header
//
enum {
	C4R_HDR_SIGNATURE,  // char[3] "C4R" (no null terminator)
	C4R_HDR_VERSION,    // char
	C4R_HDR_WORDBITS,   // char
	C4R_HDR_ENTRY,      // int
	C4R_HDR_CODELEN,    // int
	C4R_HDR_DATALEN,    // int
	C4R_HDR_PATCHLEN,   // int
	C4R_HDR_SYMBOLSLEN, // int
	C4R_HDR_CONSTRUCTLEN, // int
	C4R_HDR_DESTRUCTLEN, // int
	C4R_HDR__Sz
};

//
// Patch segment
//

// Patch type. Apart from these, the patch type refers
// to an id in the symbols table.
#if PURE_C4
int C4R_PTYPE_CODE, C4R_PTYPE_DATA; // set in c4r_load_opt
#else
enum {
	C4R_PTYPE_CODE = -1,
	C4R_PTYPE_DATA = -2
};
#endif

// Patch Structure
enum {
	C4R_PAT_TYPE,       // int, see C4R_PTYPE_
	C4R_PAT_ADDRESS,    // int
	C4R_PAT_VALUE,      // int
	C4R_PAT__Sz
};

//
// Symbols segment
//

// Symbol types
enum { C4R_STYPE_CHAR, C4R_STYPE_INT, C4R_STYPE_PTR };

// Symbol classes
enum { C4R_SCLASS_Num = 128, C4R_SCLASS_Fun, C4R_SCLASS_Sys, C4R_SCLASS_Glo, C4R_SCLASS_Loc, C4R_SCLASS_Id };

// Symbol structure
enum {
	C4R_SYMB_ID,         // int
	C4R_SYMB_TYPE,       // char, CHAR or INT, optionally plus any number of PTR
	C4R_SYMB_CLASS,      // char, see Symbol classes
	C4R_SYMB_ATTRS,      // int, attributes
	C4R_SYMB_NAMELEN,    // char
	C4R_SYMB_NAME,       // char *
	C4R_SYMB_VALUE,      // int
	//C4R_SYMB_LENGTH,     // int, used for functions only currently
	C4R_SYMB__Sz
};

//
// Construct / Destruct segment
//

// Construct / Destruct structure
enum {
	C4R_CNDE_Priority,   // int, lowest runs first
	C4R_CNDE_Value,      // int, address of function (not adjusted for code load addr)
	C4R_CNDE__Sz
};

//
// Structure with references to all the above, plus interface
//
enum {
	C4R_HEADER,
	C4R_CODE,
	C4R_DATA,
	C4R_PATCHES,
	C4R_SYMBOLS,
	C4R_CONSTRUCTORS,
	C4R_DESTRUCTORS,
	C4R_LOADCOMPLETE,
	C4R_SYSCALL,        // Syscall interface
	C4R__Sz
};

enum {
	C4R_BAD_NONE         = 0x0,
	C4R_BAD_CODE         = 0x1,
	C4R_BAD_DATA         = 0x2,
	C4R_BAD_PATCHES      = 0x4,
	C4R_BAD_SYMBOLS      = 0x8,
	C4R_BAD_CONSTRUCTORS = 0x10,
	C4R_BAD_DESTRUCTORS  = 0x20
};

#endif // #ifndef __C4R_H
