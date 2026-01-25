// C4 Multiloader component: floating point support
//
// Defines the interface programs should use.

#ifndef __C4M_FLOAT_H
#define __C4M_FLOAT_H 1

#ifndef C4M_FLOAT
#define C4M_FLOAT 1
// Forward declaration
int c4_float_instruction (int *sp);
#endif

enum {
	FLTI_ABSF,
	FLTI_ADD,
	FLTI_SUB,
	FLTI_MUL,
	FLTI_DIV,
	FLTI_NEG,  // Negate
	FLTI_SIN,
	FLTI_COS,
	FLTI_TAN,
	FLTI_ITOF, // Integer to float
	FLTI_FTOI, // Float to integer
};

// Constants, done the C4M way with a constructor
static int fltv_0,   // 0.0f
           fltv_1,   // 1.0f
           fltv_2,   // 2.0f
           fltv_1eMinus1;
static void __attribute__((constructor)) c4m_float_constructor () {
	if (sizeof(int) == 4) { // 32bit
		fltv_0 = 0x0; // Float(0.000000) == Hex(0x0)
		fltv_1 = 0x3f800000; // Float(1.000000) == Hex(0x3f800000)
		fltv_2 = 0x40000000; // Float(2.000000) == Hex(0x40000000)
		// Float(3.000000) == Hex(0x40400000)
		// Float(4.000000) == Hex(0x40800000)
		// Float(5.000000) == Hex(0x40a00000)
		// Float(6.000000) == Hex(0x40c00000)
		// Float(7.000000) == Hex(0x40e00000)
		// Float(8.000000) == Hex(0x41000000)
		fltv_1eMinus1 = 0x3dcccccd; // Float(0.100000) == Hex(0x3dcccccd)
	} else if (sizeof(int) == 8) { // 64bit
		fltv_0 = 0xcf8ece2000000000; // Float(0.000000) == Hex(0xcf8ece2000000000)
		fltv_1 = 0xcf8ece203f800000; // Float(1.000000) == Hex(0xcf8ece203f800000)
		fltv_2 = 0xcf8ece2040000000; // Float(2.000000) == Hex(0xcf8ece2040000000)
		// Float(3.000000) == Hex(0xcf8ece2040400000)
		// Float(4.000000) == Hex(0xcf8ece2040800000)
		// Float(5.000000) == Hex(0xcf8ece2040a00000)
		// Float(6.000000) == Hex(0xcf8ece2040c00000)
		// Float(7.000000) == Hex(0xcf8ece2040e00000)
		// Float(8.000000) == Hex(0xcf8ece2041000000)
		fltv_1eMinus1 = 0xcf8ece203dcccccd; // Float(0.100000) == Hex(0xcf8ece203dcccccd)
	} else {
		printf("c4m: floating point unavailable, architecture size %dbit not supported\n", sizeof(int) * 8);
	}
}

#endif
