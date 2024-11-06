//
// Executable loader: .c4r format
//
// Invocation: c4m load-c4r.c -- hello.c4r
//
// See asm-c4r.c for layout
//

#include "c4.h"
#include "c4m.h"

enum { C4R__Supported_Version = 2 };

enum {
	C4ROPT_NONE,
	C4ROPT_SYMBOLS
};

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
enum {
	C4R_PTYPE_CODE = -1,
	C4R_PTYPE_DATA = -2
};

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
// Structure with references to all the above
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
	C4R__Sz
};

//
// C4R Interface
//

enum { C4R_BUFFER_SIZE = 255 };

int c4r_debug, c4r_verbose, c4r_fullmode;

// Convert up to 4 characters into a single word
static int c4r__charword (char a, char b, char c, char d) {
	return a + ((b) << 8) + ((c) << 16) + ((d) << 24);
}

static char *c4r_strcpy_alloc (char *source) {
	int len;
	char *result, *p, *x;

	// Count chars
	len = 0; p = source; while (*p++) ++len;
	if (!(result = p = malloc(len + 2)))
		return 0;
	// Copy
	p = source; x = result;
	while (*x++ = *p++)
		; // Empty
	// *x = 0;
	return result;
}

static int c4r_readoffset;
static int c4r_checked_read (int fd, char *buffer, int len) {
	int i;
	if ((i = read(fd, buffer, len)) <= 0) {
		if (c4r_debug) printf("lc4r: error, read() returned %d\n", i);
	} else {
		if (c4r_debug) printf("lc4r: successful read of %d bytes from file.0x%lx\n", len, c4r_readoffset);
		c4r_readoffset = c4r_readoffset + len;
	}
	return i;
}

// static
enum {
	C4R_BAD_NONE         = 0x0,
	C4R_BAD_CODE         = 0x1,
	C4R_BAD_DATA         = 0x2,
	C4R_BAD_PATCHES      = 0x4,
	C4R_BAD_SYMBOLS      = 0x8,
	C4R_BAD_CONSTRUCTORS = 0x10,
	C4R_BAD_DESTRUCTORS  = 0x20
};

int *c4r_create_empty () {
	int *c4r, *header;
	int  i;
	if (!(c4r = malloc((i = sizeof(int) * C4R__Sz))))
		return 0;
	memset(c4r, 0, i);
	if (!(header = malloc((i = sizeof(int) * C4R_HDR__Sz)))) {
		free(c4r);
		return 0;
	}
	header[C4R_HDR_SIGNATURE]    = c4r__charword('C', '4', 'R', 0);
	header[C4R_HDR_VERSION]      = C4R__Supported_Version;
	header[C4R_HDR_WORDBITS]     = sizeof(int) * 8;
	header[C4R_HDR_ENTRY]        = -1; // No entry (TODO: make enum/constant)
	header[C4R_HDR_CODELEN]      = 0;
	header[C4R_HDR_DATALEN]      = 0;
	header[C4R_HDR_PATCHLEN]     = 0;
	header[C4R_HDR_CONSTRUCTLEN] = 0;
	header[C4R_HDR_DESTRUCTLEN]  = 0;
	header[C4R_HDR_SYMBOLSLEN]   = 0;
	c4r[C4R_HEADER]       = (int)header;
	c4r[C4R_CODE]         = 0;
	c4r[C4R_DATA]         = 0;
	c4r[C4R_SYMBOLS]      = 0;
	c4r[C4R_LOADCOMPLETE] = 1;
	c4r[C4R_CONSTRUCTORS] = 0;
	c4r[C4R_DESTRUCTORS]  = 0;
	return c4r;
}

int *c4r_load_opt (char *file, int options) {
	int fd, i, x;
	char *buffer;
	int *c4r, *header, *code, *data, *patches, *symbols, *constructors, *destructors;
	int  wordbytes, bad;
	int *base, *target;
	int  loop_target;
	char *tmp;
	int  ptype, paddr, pvalu;
	int  percent, p, pc;

	if (!(buffer = malloc(C4R_BUFFER_SIZE))) {
		printf("Failed to allocate %d bytes for read buffer\n", C4R_BUFFER_SIZE);
		return 0;
	}
	memset(buffer, 0, C4R_BUFFER_SIZE);
	if (!(c4r = malloc((i = sizeof(int) * C4R__Sz)))) {
		free(buffer);
		printf("lc4r: Failed to allocate %d bytes for c4r structure\n", i);
		return 0;
	}
	memset(c4r, 0, i);
	if (!(header = malloc((i = sizeof(int) * C4R_HDR__Sz)))) {
		free(buffer);
		free(c4r);
		printf("lc4r: Failed to allocate %d bytes for header structure\n", i);
		return 0;
	}
	memset(header, 0, i);

	c4r[C4R_LOADCOMPLETE] = 0;

	// These will be set correctly later
	code = data = patches = symbols = constructors = destructors = 0;

	if (c4r_debug || c4r_verbose) printf("lc4r: loading '%s'...\n", file);
	if ((fd = open(file, 0)) < 0) return 0;

	//
	// Read header
	//
	if ((i = c4r_checked_read(fd, buffer, 3)) < 3) {
		// printf("lc4r: Shallow c4r_checked_read of first 3 bytes\n");
	} else {
		if (!(buffer[0] == 'C' && buffer[1] == '4' && buffer[2] == 'R')) {
			// Squelched so that running c4 from eshell doesn't complain
			// printf("lc4r: Signature mismatch: '%.3s' != correct 'C4R'\n", buffer);
		} else {
			//
			// Header section
			//
			if (c4r_verbose) printf("lc4r: load header...\n");
			header[C4R_HDR_SIGNATURE] = c4r__charword('C', '4', 'R', 0);
			read(fd, buffer, 1); header[C4R_HDR_VERSION] = *buffer;
			read(fd, buffer, 1); header[C4R_HDR_WORDBITS]= *buffer;
			wordbytes = header[C4R_HDR_WORDBITS] / 8;
			read(fd, buffer, wordbytes); header[C4R_HDR_ENTRY] = *(int *)buffer;
			read(fd, buffer, wordbytes); header[C4R_HDR_CODELEN] = *(int *)buffer;
			read(fd, buffer, wordbytes); header[C4R_HDR_DATALEN] = *(int *)buffer;
			read(fd, buffer, wordbytes); header[C4R_HDR_PATCHLEN] = *(int *)buffer;
			read(fd, buffer, wordbytes); header[C4R_HDR_SYMBOLSLEN] = *(int *)buffer;
			read(fd, buffer, wordbytes); header[C4R_HDR_CONSTRUCTLEN] = *(int *)buffer;
			read(fd, buffer, wordbytes); header[C4R_HDR_DESTRUCTLEN] = *(int *)buffer;
			c4r[C4R_HEADER] = (int)header;

			if (wordbytes != sizeof(int)) {
				// Partial success, but word bit mismatch.
				printf("lc4r: error, c4r file uses %dbit words and we use %dbit.\n",
				       header[C4R_HDR_WORDBITS], sizeof(int) * 8);
				return c4r;
			}
			// Allow older but not newer version
			else if (header[C4R_HDR_VERSION] && header[C4R_HDR_VERSION] < C4R__Supported_Version) {
				printf("lc4r: error, c4r file uses version %d and we support only %d\n",
				       header[C4R_HDR_VERSION], C4R__Supported_Version);
				return c4r;
			}

			//
			// Segment section
			//

			// Allocate and read segments
			if (c4r_verbose) printf("lc4r: allocate memory...\n");
			bad = C4R_BAD_NONE;
			if (!(code = malloc((i = sizeof(int) * header[C4R_HDR_CODELEN]))))
				bad = C4R_BAD_CODE;
			else memset(code, 0, i);
			if (c4r_verbose) printf("lc4r: code allocated\n");
			if (header[C4R_HDR_DATALEN]) {
				if (!(data = malloc((i = sizeof(char)* header[C4R_HDR_DATALEN]))))
					bad = bad | C4R_BAD_DATA;
				else memset(data, 0, i);
			}
			if (c4r_verbose) printf("lc4r: data allocated\n");
			if (header[C4R_HDR_PATCHLEN]) {
				if (!(patches = malloc((i = sizeof(int) * C4R_PAT__Sz * header[C4R_HDR_PATCHLEN]))))
					bad = bad | C4R_BAD_PATCHES;
				else memset(patches, 0, i);
			}
			if (c4r_verbose) printf("lc4r: patches allocated\n");
			if (header[C4R_HDR_SYMBOLSLEN]) {
				if (!(symbols = malloc((i = sizeof(int) * C4R_SYMB__Sz * header[C4R_HDR_SYMBOLSLEN]))))
					bad = bad | C4R_BAD_SYMBOLS;
				else memset(symbols, 0, i);
			}
			if (c4r_verbose) printf("lc4r: symbols allocated\n");
			if (header[C4R_HDR_CONSTRUCTLEN]) {
				if (!(constructors = malloc((i = sizeof(int) * C4R_CNDE__Sz * header[C4R_HDR_CONSTRUCTLEN]))))
					bad = bad | C4R_BAD_CONSTRUCTORS;
				else memset(constructors, 0, i);
			}
			if (c4r_verbose) printf("lc4r: constructors allocated\n");
			if (header[C4R_HDR_DESTRUCTLEN]) {
				if (!(destructors  = malloc((i = sizeof(int) * C4R_CNDE__Sz * header[C4R_HDR_DESTRUCTLEN]))))
					bad = bad | C4R_BAD_DESTRUCTORS;
				else memset(destructors, 0, i);
			}
			if (c4r_verbose) printf("lc4r: destructors allocated\n");

			if (bad) {
				printf("lc4r: memory allocation failure reason: 0x%x\n", bad);
			} else {
				// Load segments

				// Code
				if (c4r_verbose) printf("lc4r: load code...\n");
				read(fd, buffer, 1); if (*buffer != 'C') { printf("lc4r: expected code segment, found 0x%x\n", *buffer); return c4r; }
				read(fd, (char *)code, header[C4R_HDR_CODELEN] * wordbytes);
				if (c4r_debug) printf("lc4r: loaded code, now at position 0x%x\n", c4r_readoffset);
				// target = code; i = 0; while (i++ <= header[C4R_HDR_CODELEN]) printf("  Code @ 0x%lx: %d\n", target, *target++);
				c4r[C4R_CODE] = (int)code;
				// Data
				if (c4r_verbose) printf("lc4r: load data...\n");
				read(fd, buffer, 1); if (*buffer != 'D') { printf("lc4r: expected data segment, found '%c' 0x%x at file position 0x%lx %d\n", *buffer, *buffer, c4r_readoffset, c4r_readoffset); return c4r; }
				read(fd, (char *)data, header[C4R_HDR_DATALEN]);
				//i = 0; x = header[C4R_HDR_DATALEN] / wordbytes; target = data;
				//if (c4r_debug) printf("lc4r: data len %d = %d words\n", header[C4R_HDR_DATALEN], x);
				//while (i++ < x) {
				//	read(fd, buffer, wordbytes);
				//	*target++ = *(int *)buffer;
				//	*(int *)buffer = 0;
				//}
				if (c4r_debug) {
					printf("lc4r: loaded data @ 0x%X, now at position 0x%x\n", data, c4r_readoffset);
					printf("lc4r: Data: '%.*s'\n", header[C4R_HDR_DATALEN], (char *)data);
				}
				c4r[C4R_DATA] = (int)data;
				// Patches
				target = patches; i = 0; loop_target = header[C4R_HDR_PATCHLEN];
				if (c4r_verbose) printf("lc4r: loading and apply %d patches...\n", loop_target);
				read(fd, buffer, 1); if (*buffer != 'P') { printf("lc4r: expected patch segment, found 0x%x, position 0x%x (%d)\n", *buffer, c4r_readoffset - 1, c4r_readoffset - 1); return c4r; }
				percent = 0;
				p = __time();
				while (i < loop_target) {
					read(fd, buffer, wordbytes); target[C4R_PAT_TYPE]    = ptype = *(int *)buffer;
					read(fd, buffer, wordbytes); target[C4R_PAT_ADDRESS] = paddr = *(int *)buffer;
					read(fd, buffer, wordbytes); target[C4R_PAT_VALUE]   = pvalu = *(int *)buffer;

					// Perform the patching now
					// if (c4r_debug) printf("Patch %d ", i);
					if (ptype == C4R_PTYPE_CODE) {
						// if (c4r_debug) printf("updating code 0x%lX from 0x%lX to 0x%lX\n",
						//        (code + target[C4R_PAT_ADDRESS]),
						//       *(code + target[C4R_PAT_ADDRESS]),
						//	    (code + target[C4R_PAT_VALUE]));
						*(code + paddr) = (int)(code + pvalu);
					} else if(ptype == C4R_PTYPE_DATA) {
						//if (c4r_debug) printf("updating data 0x%lX from 0x%lX to 0x%lX\n",
						//        (code + target[C4R_PAT_ADDRESS]),
						 //      *(code + target[C4R_PAT_ADDRESS]),
						//	    (code + target[C4R_PAT_VALUE]));
						*(code + paddr) = (int)(((char *)data) + pvalu);
					} else {
						//printf("lc4r: unexpected patch type %d (not %d or %d)\n", target[C4R_PAT_TYPE], C4R_PTYPE_CODE, C4R_PTYPE_DATA);
						//return c4r;
					}

					++i;
					target = target + C4R_PAT__Sz;

					if (c4r_verbose && __time() - p >= 1000) {
						// Only print every 1s max
                        percent = (i * 100) / loop_target;
                        printf("lc4r: %d%% patched...\n", percent);
                        p = __time();
					}

				}
				if (c4r_verbose)
					printf("lc4r: 100%% patched\n");
				c4r[C4R_PATCHES] = (int)patches;
				if (c4r_debug) printf("lc4r: loaded patches, now at position 0x%x\n", c4r_readoffset);

				// Constructors
				if (c4r_verbose) printf("lc4r: load constructors and destructors...\n");
				read(fd, buffer, 1);
				if (*buffer != 'c') {
					printf("lc4r: expected constructors segment, found 0x%x\n", *buffer);
					return c4r;
				}
				target = constructors; i = 0; // Constructors
				while (i++ < header[C4R_HDR_CONSTRUCTLEN]) {
					// read(fd, buffer, wordbytes); target[C4R_CNDE_Priority] = *(int *)buffer;
					read(fd, buffer, wordbytes); target[C4R_CNDE_Value]    = *(int *)buffer;
					// printf("cnde_value=%d\n", target[C4R_CNDE_Value]);
					target = target + C4R_CNDE__Sz;
				}
				c4r[C4R_CONSTRUCTORS] = (int)constructors;
				if (c4r_debug) printf("lc4r: loaded constructors, now at position 0x%x\n", c4r_readoffset);
				// Destructors
				read(fd, buffer, 1);
				if (*buffer != 'd') {
					if (c4r_debug) printf("lc4r: expected constructors segment, found 0x%x\n", *buffer);
					return c4r;
				}
				target = destructors; i = 0;
				while (i++ < header[C4R_HDR_DESTRUCTLEN]) {
					// read(fd, buffer, wordbytes); target[C4R_CNDE_Priority] = *(int *)buffer;
					read(fd, buffer, wordbytes); target[C4R_CNDE_Value]    = *(int *)buffer;
					target = target + C4R_CNDE__Sz;
				}
				c4r[C4R_DESTRUCTORS] = (int)destructors;
				if (c4r_debug) printf("lc4r: loaded destructors, now at position 0x%x\n", c4r_readoffset);

				// Symbols
				if (options & C4ROPT_SYMBOLS) {
					read(fd, buffer, 1); if (*buffer != 'S') { printf("lc4r: expected symbols segment, found 0x%x\n", *buffer); return c4r; }
					target = symbols; i = 0; loop_target = header[C4R_HDR_SYMBOLSLEN];
					if (c4r_verbose) printf("lc4r: loading %d symbols...\n", loop_target);
					while (i < loop_target) {
						read(fd, buffer, wordbytes); target[C4R_SYMB_ID] = *(int *)buffer;
						read(fd, buffer, wordbytes); target[C4R_SYMB_TYPE] = *(int *)buffer;
						read(fd, buffer, wordbytes); target[C4R_SYMB_CLASS]= *(int *)buffer;
						read(fd, buffer, wordbytes); target[C4R_SYMB_ATTRS] = *(int *)buffer;
						read(fd, buffer, 1); target[C4R_SYMB_NAMELEN] = *buffer;
						memset(buffer, 0, C4R_BUFFER_SIZE); // TODO: write nul terminator
						read(fd, buffer, target[C4R_SYMB_NAMELEN]); target[C4R_SYMB_NAME] = (int)c4r_strcpy_alloc(buffer);
						read(fd, buffer, wordbytes); target[C4R_SYMB_VALUE] = *(int *)buffer;
						//read(fd, buffer, wordbytes); target[C4R_SYMB_LENGTH] = *(int *)buffer;
						if (c4r_debug) {
							printf("Symbol %d @ 0x%lX:\n  Type: %d\n", i, c4r_readoffset, target[C4R_SYMB_TYPE]);
							printf("  Class: %d", target[C4R_SYMB_CLASS]);
							printf("  Name len: %d", target[C4R_SYMB_NAMELEN]);
							printf("  Name: '%s'", (char *)target[C4R_SYMB_NAME]);
							printf("  Value: %d\n", target[C4R_SYMB_VALUE]);
							//printf("  Length: %d\n", target[C4R_SYMB_LENGTH]);
						}

						++i;
						target = target + C4R_SYMB__Sz;
					}
					if (c4r_debug) printf("lc4r: loaded %d symbols, now at position 0x%x\n", i, c4r_readoffset);
				}
				c4r[C4R_SYMBOLS] = (int)symbols;

				// Success, cleanup buffer and file, return
				if (c4r_debug) printf("lc4r: Successfully loaded, c4r = 0x%lx  header = 0x%lx\n", c4r, header);
				c4r[C4R_LOADCOMPLETE] = 1;

				free(buffer);
				close(fd);

				if (c4r_verbose) printf("lc4r: load complete.\n");
				return c4r;
			}
		}
	}
	
	// If we reach here, something went wrong, unallocate everything
	if (destructors) free(destructors);
	if (constructors) free(constructors);
	if (symbols) free(symbols);
	if (patches) free(patches);
	if (data) free(data);
	if (code) free(code);
	if (header) free(header);
	if (c4r) free(c4r);
	if (buffer) free(buffer);
	if (fd) close(fd);

	if (c4r_verbose) printf("lc4r: load failure.\n");

	return 0;
}

int *c4r_load (char *file) {
	return c4r_load_opt(file, C4ROPT_SYMBOLS);
}

// Free all elements of a C4R module
void c4r_free (int *c4r) {
	int i, *s, *h, *d;

	// First all the simple items
	if (c4r[C4R_CODE]) {
		if (c4r_debug) printf("lc4r: freeing code @ 0x%X\n", c4r[C4R_CODE]);
		free((int *)c4r[C4R_CODE]); c4r[C4R_CODE] = 0;
	}
	if (c4r[C4R_DATA]) {
		if (c4r_debug) printf("lc4r: freeing data @ 0x%X\n", c4r[C4R_DATA]);
		free((int *)c4r[C4R_DATA]); c4r[C4R_DATA] = 0;
	}
	if (c4r[C4R_PATCHES]) {
		if (c4r_debug) printf("lc4r: freeing patches @ 0x%X\n", c4r[C4R_PATCHES]);
		free((int *)c4r[C4R_PATCHES]); c4r[C4R_PATCHES] = 0;
	}
	if (c4r[C4R_CONSTRUCTORS]) { free((int *)c4r[C4R_CONSTRUCTORS]); c4r[C4R_CONSTRUCTORS] = 0; }
	if (c4r[C4R_DESTRUCTORS]) {
		if (c4r_debug) printf("lc4r: freeing destructors @ 0x%X\n", c4r[C4R_CONSTRUCTORS]);
		free((int *)c4r[C4R_DESTRUCTORS]); c4r[C4R_DESTRUCTORS] = 0;
	}
	// Symbols have a number of allocated strings
	if (c4r[C4R_SYMBOLS]) {
		if (c4r_debug) printf("lc4r: freeing symbols...\n");
		h = (int *)c4r[C4R_HEADER]; // Grab header
		i = 0;
		d = (int *)c4r[C4R_SYMBOLS];
		if (c4r_debug) printf("Freeing %d symbols\n", h[C4R_HDR_SYMBOLSLEN]);
		while (i++ < h[C4R_HDR_SYMBOLSLEN]) {
			if (d[C4R_SYMB_NAME]) {
				free((char *)d[C4R_SYMB_NAME]);
				d[C4R_SYMB_NAME] = 0;
			}
			d = d + C4R_SYMB__Sz;
		}
		free((int *)c4r[C4R_SYMBOLS]);
		c4r[C4R_SYMBOLS] = 0;
	}
	// The header and overall structure
	if (c4r[C4R_HEADER]) {
		if (c4r_debug) printf("lc4r: freeing header @ 0x%X\n", c4r[C4R_HEADER]);
		free((int *)c4r[C4R_HEADER]); c4r[C4R_HEADER] = 0;
	}
	if (c4r_debug) printf("lc4r: freeing structure @ 0x%X\n", c4r);
	free(c4r);
}

void c4r_dump_patches (int *c4r) {
	int *header, *patch, i, max;

	i = 0;
	header = (int *)c4r[C4R_HEADER];
	max = header[C4R_HDR_PATCHLEN];
	patch = (int *)c4r[C4R_PATCHES];
	while (patch && i++ < max) {
		printf("  patch type ");
		if (patch[C4R_PAT_TYPE] == C4R_PTYPE_CODE) printf("CODE");
		else if(patch[C4R_PAT_TYPE] == C4R_PTYPE_DATA) printf("DATA");
		else printf("symbols[%d]", patch[C4R_PAT_TYPE]);
		printf(" address 0x%x value 0x%x\n", patch[C4R_PAT_ADDRESS], patch[C4R_PAT_VALUE]);
		patch = patch + C4R_PAT__Sz;
	}
}

void c4r_dump_symbols (int *c4r) {
	int *header, *symbol, i, max;

	i = 0;
	header = (int *)c4r[C4R_HEADER];
	max = header[C4R_HDR_SYMBOLSLEN];
	symbol = (int *)c4r[C4R_SYMBOLS];
	while (symbol && i++ < max) {
		printf("  symbol %d '%.*s': ", symbol[C4R_SYMB_ID], symbol[C4R_SYMB_NAMELEN], (char *)symbol[C4R_SYMB_NAME]);
		printf("type %d class %d value %d (0x%x) ",
			   symbol[C4R_SYMB_TYPE], symbol[C4R_SYMB_CLASS], symbol[C4R_SYMB_VALUE], symbol[C4R_SYMB_VALUE]);
		printf("attrs 0x%x\n", symbol[C4R_SYMB_ATTRS]);
		symbol = symbol + C4R_SYMB__Sz;
	}
}

void c4r_dump_info (int *c4r) {
	int *header;
	char *c;

	header = (int *)c4r[C4R_HEADER];
	printf("lc4r: C4R ");
	if (c4r[C4R_LOADCOMPLETE]) printf("(loaded)");
	else                       printf("(**error**)");
	printf(" @ 0x%lx with header 0x%lx\n", c4r, header);
	c = (char *)&header[C4R_HDR_SIGNATURE];
	printf("lc4r: Signature %c%c%c  ", *c, *(c + 1), *(c + 2));
	printf("Version %d  ", header[C4R_HDR_VERSION]);
	printf("(%d bits):\n", header[C4R_HDR_WORDBITS]);
	printf("lc4r:");
	if (header[C4R_HDR_ENTRY] == -1)
		printf("   No Entry");
	else
		printf("  Entry = 0x%x", header[C4R_HDR_ENTRY]);
	printf("  Code = %d", header[C4R_HDR_CODELEN]);
	printf("  Data = %d", header[C4R_HDR_DATALEN]);
	printf("  Patch = %d", header[C4R_HDR_PATCHLEN]);
	printf("  Symbols = %d" ,header[C4R_HDR_SYMBOLSLEN]);
	printf("  Cons = %d", header[C4R_HDR_CONSTRUCTLEN]);
	printf("  Des  = %d\n", header[C4R_HDR_DESTRUCTLEN]);
	printf("lc4r:  0x%lx\n", c4r[C4R_CODE] + header[C4R_HDR_ENTRY]);

	//c4r_dump_symbols(c4r);
}

void c4r_print_stacktrace (int *c4r, int *alt, int *pc) {
	int *hdr;
	int *syms, syms_length;
	int *sym , *next_sym;
	int  i, code, cls;

	if (!c4r && alt) {
		c4r = alt;
		alt = 0;
	}

	printf("lc4r stacktrace for pc 0x%lx, value %d\n", pc, *pc);
	return;
	hdr = (int *) c4r[C4R_HEADER];
	code = c4r[C4R_CODE];
	syms = sym = (int *)c4r[C4R_SYMBOLS];
	syms_length = hdr[C4R_HDR_SYMBOLSLEN];
	next_sym = sym + C4R_SYMB__Sz;
	i = 0;
	while (i < syms_length) {
		cls = sym[C4R_SYMB_CLASS];
		if (cls == C4R_SCLASS_Fun) {
			//printf("  sym %d '%.*s': ", sym[C4R_SYMB_ID], sym[C4R_SYMB_NAMELEN], (char *)sym[C4R_SYMB_NAME]);
			//printf("type %d class %d value %d (0x%lx) ",
			//	   sym[C4R_SYMB_TYPE], sym[C4R_SYMB_CLASS], sym[C4R_SYMB_VALUE], code + sym[C4R_SYMB_VALUE]);
			//printf("attrs 0x%x\n", sym[C4R_SYMB_ATTRS]);
		}
		++i;
		sym = next_sym;
		next_sym = sym + C4R_SYMB__Sz;
	}
}

//
// Public interface
//

// This macro satisfies gcc to use c4 syntax of calling function pointers via int*
#ifndef __c4__
#define entry(a,b) ((int (*)(int, char**))entry)(a, b)
#define cons(c4r)  ((int (*)(int *))cons)(c4r)
#define des()      ((int (*)())des)()
#endif
// Execute a loaded c4r module. Must provide a fully loaded c4r structure.
// Also runs constructors and destructors.
int loadc4r_execute (int *c4r, int argc, char **argv) {
	int *header, *entry;
	int  i, *c, r, *cons, *des, len;

	// Must have a complete structure.
	if (!c4r[C4R_LOADCOMPLETE]) return -1;

	header = (int *)c4r[C4R_HEADER];
	// Must have an entry point
	if (header[C4R_HDR_ENTRY] == -1) {
		printf("lc4r: this module has no entry point.\n");
		return -2;
	}

	entry = (int *)c4r[C4R_CODE] + header[C4R_HDR_ENTRY];
	// printf("lc4r: header @ 0x%x  code @ 0x%x  entry @ 0x%x\n", header, (int *)c4r[C4R_CODE], entry);

    if ((len = header[C4R_HDR_CONSTRUCTLEN])) {
        // Run constructors
        if (c4r_verbose) printf("lc4r: running %d constructors...\n", len);
        c = (int *)c4r[C4R_CONSTRUCTORS];
        i = 0;
        while (i++ < len) { // C4R_HDR_CONSTRUCTLEN
            cons = (int *)c4r[C4R_CODE] + c[C4R_CNDE_Value];
            // printf("lc4r: calling constructor at offset 0x%x, real=0x%x\n", c[C4R_CNDE_Value], cons);
            cons(c4r);
            c = c + C4R_CNDE__Sz;
        }
    }

	// Call main entry point
	if (c4r_verbose) printf("lc4r: start program...\n");
	r = entry(argc, argv);

    if ((len = header[C4R_HDR_DESTRUCTLEN])) {
        // Run destructors
        if (c4r_verbose) printf("lc4r: running %d destructors...\n", len);
        c = (int *)c4r[C4R_DESTRUCTORS];
        i = 0;
        while (i++ < len) {
            des = (int *)c4r[C4R_CODE] + c[C4R_CNDE_Value];
            // printf("lc4r: calling destructor at offset 0x%x, real=0x%x\n", c[C4R_CNDE_Value], des);
            des();
            c = c + C4R_CNDE__Sz;
        }
    }

	return r;
}
#ifndef __c4__
#undef entry
#undef cons
#undef des
#endif

static char *strcpycat (char *source, char *append) {
	int length, slen, alen;
	char *buffer, *s, *d;

	// Count length
	length = slen = alen = 0;
	s = source; while(*s++) ++slen;
	s = append; while(*s++) ++alen;
	length = slen + alen + 1; // nul terminator
	if (!(buffer = malloc(sizeof(char) * length)))
		return 0;
	// Copy source
	s = source; d = buffer; while(*d++ = *s++) ; // empty body
	// rewind
	--d;
	// Copy append
	s = append;             while(*d++ = *s++) ; // empty body
	// terminating nul has already been copied

	return buffer;
}

int loadc4r_run (char *file, int argc, char **argv) {
	int *module, *modules, result;
	char *alt_file;

	alt_file = 0;

	if (!(module = c4r_load(file))) {
		// Attempt a version with .c4r appended
		alt_file = strcpycat(file, ".c4r");
		if (!(module = c4r_load(alt_file))) {
			printf("lc4r: unable to open '%s' or '%s'\n", file, alt_file);
			free(alt_file);
			return 1;
		}
	}
	// printf("lc4r: module %s loaded\n", alt_file ? alt_file : file);

    if (c4r_verbose)
        c4r_dump_info(module);
	result = -1;
	if (module[C4R_LOADCOMPLETE])
		result = loadc4r_execute(module, argc, argv);
	c4r_free(module);
	if (alt_file) free(alt_file);

	return result;
}

// Aliases to c4r functions
void loadc4r_dump_info (int *c4r) { c4r_dump_info(c4r); }
void loadc4r_free (int *c4r) { c4r_free(c4r); }

enum {
	LC4RF_NONE,
	LC4RF_NO_PARSE
};

void loadc4r_usage () {
    printf("Load C4R usage: ./c4m load-c4r.c [-v] [-d] [--] [file.c4r]\n");
}

#ifndef NO_LOADC4R_MAIN
int main (int argc, char **argv) {
	int result, flags;
	char n;
	result = -1;
	flags = LC4RF_NONE;
	if (argc < 1) {
        loadc4r_usage();
	} else {
		--argc; ++argv;
		while(argc && flags != LC4RF_NO_PARSE && **argv == '-') {
			// printf("option: '%s'\n", *argv);
			n = (*argv)[1];
			if (n == '-')
				flags = LC4RF_NO_PARSE;
			else if (n == 'd')
				c4r_debug = !c4r_debug;
			else if (n == 'v')
				c4r_verbose = !c4r_verbose;
			else {
				printf("load-c4r: unrecognised option: '%s'\n", *argv);
				exit(-1);
			}
			--argc; ++argv;
		}
        if (argc)
            result = loadc4r_run(*argv, argc, argv);
        else {
            loadc4r_usage();
            result = -1;
        }
	}
	return result;
}
#endif
