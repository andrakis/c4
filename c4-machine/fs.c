// fs.c - a simple filesystem manipulator
//
// Written in C4-Machine compliance.
//
// Usage: fs.c <image> <operation> [arguments...]
//
// Where:
//   image is an image file to read
//   operation is one of: create read
//
// Notes:
//  - image is not created correctly on windows
/*
 Filesystem name: QFS (Quick Filesystem)
 Design:
  - inode-based design: each file / directory occupies 1 or more inodes.
  - each inode has a "next inode" field, which if 0 means no more linked inodes.
  - directories are files, whose content maps names to inodes.
  - file have very limited attributes: type, permissions.
  - inodes may be marked as deleted, potentially allowing later data recovery.
  - inodes have a header followed by a data segment of (isize << 8)
  - the magic value allows detection of mismatched endianess, if the value is
    incorrect the bytes are flipped and attempted again. If they now match,
    the code continues flipping bytes for the duration of the read.

 Filesystem layout:
           c   1 byte char
           i   4 byte int
   Start   Length
   0x0     c  Filesystem type: 0xCC
   0x1     i  Magic value: 0x10203040
   0x5     c  Inode size (shifted left 8) (minimum: 256)
   0x6     i  Inode count
   0x10    i  Used Inode count
   0x14    i  Root inode
   ...        Inodes

 Inode layout:
   Start   Length
   0x0     i  Inode id (starts at 1)
   0x4     i  Next inode id
   0x8     c  Type (0 = nul, 1 = file, 2 = directory, 4 = deleted)
   0x9     c  Permissions (1 = read, 2 = write, 4 = executable)
   0x10    i  Used length
   0x14    i  Total length
   0x18..     Content (size: inodesize<<8)

 Directories are simply structured files, each line of the format:
   filename inode_id\n

   The filename may contain spaces, as the last number found before a newline is considered the node id.
*/

#include "../c4.h"

#ifdef __C4__
// C4 only: stub out the call to write()
int warned;
int write (int fd, void *buf, int length) {
	if (!warned) {
		warned = 1;
		printf("c4-stub: write() ");
	} else printf(".");
	return 1;
}
// also fake errno
int errno;
#endif

int g_flipbits;

enum { FS_REAL = 1024 }; // Real file ids are less than this
enum {
	DEF_FSTYPE = 204, // 0xCC
	DEF_FSVERSION = 0,
	DEF_ISIZE = 4, // shifted left 8
	DEF_ICOUNT = 1024
};

enum {
	MAGIC = 270544960 // 0x10203040
};

// Top level layout
enum {
	FS_TYPE,
	FS_VERSION,
	FS_ISIZE,
	FS_ICOUNT,
	FS_IUSED,
	FS_IROOT,
	FS_DATA,
	FS__SZ
};

// Inode layout
enum {
	IN_ID,
	IN_NEXT,
	IN_TYPE,
	IN_PERM,
	IN_LENGTH,
	IN_LENGTHTOTAL,
	IN__SZ
};

// file descriptor layout
enum {
	FD_INODE,
	FD_RNODE, // Root node
	FD_SIZE,
	FD_LOCATION,
	FD__SZ
};

// fs_open modes
enum { FS_RDONLY, FS_WRONLY, FS_RDWR, FS_CREAT = 100 };

// file types
enum { FT_NUL, FT_FILE, FT_DIRECTORY, FT_DELETED = 4 };

// file permissions
enum { FP_NUL, FP_READ, FP_WRITE, FP_EXECUTABLE = 4 };

// options
enum {
	OPT_CREATE,
	OPT_READ,
	OPT_ARCHIVE,
	OPT_FILES,
	OPT_FILESCOUNT,
	OPT_FSTYPE,
	OPT_FSVERSION,
	OPT_ISIZE,
	OPT_ICOUNT,
	OPT__SZ
};
int *options;

char *g_image;

int fs_strlen (char *s) { char *t; t = s; while (*t++) ; return t - s; }
int fs_strncmp (char *s1, char *s2, int n) { return memcmp(s1, s2, n); }
int fs_strcmp (char *s1, char *s2) { while(*s1 && (*s1 == *s2)) { ++s1; ++s2; } return *s1 - *s2; }

int fs_atoi (char *str, int radix) {
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
		v = v * radix + ((*str > '9') ? (*str & ~0x20) - 'A' + 10 : (*str - '0'));
		++str;
	}
	return v * sign;
}

int fs_itoa (int value, char *sp, int radix) {
	char *tmp, *tp;
	int i, v, sign, len;

	if (!(tp = tmp = malloc(16))) {
		printf("itoa: failed to create temporary space\n");
		return 0;
	}

	sign = (radix == 10 && value < 0);
	if (sign)
		v = -value;
	else
		v = value;
	
	while (v || tp == tmp) {
		i = v % radix;
		v = v / radix;
		if (i < 10)
			*tp++ = i + '0';
		else
			*tp++ = i + 'a' - 10;
	}

	len = tp - tmp;
	if (sign) {
		*sp++ = '-';
		len++;
	}

	// reverse copy
	while (tp > tmp) *sp++ = *--tp;

	free(tmp);

	return len;
}

void *fs_memcpy (void *source, void *dest, int length) {
	int   i;
	int  *is, *id;
	char *cs, *cd;

	i = 0;
	if((int)dest   % sizeof(int) == 0 &&
	   (int)source % sizeof(int) == 0 &&
	   length % sizeof(int) == 0) {
		is = source; id = dest;
		length = length / sizeof(int);
		while (i < length) { id[i] = is[i]; ++i; }
	} else {
		cs = source; cd = dest;
		while (i < length) { cd[i] = cs[i]; ++i; }
	}

	return dest;
}

void *fs_memmove (void *source, void *dest, int length) {
	int   i;
	int  *is, *id;
	char *cs, *cd;

	if ((int)dest < (int)source)
		return fs_memcpy(dest, source, length);

	i = length;
	if((int)dest   % sizeof(int) == 0 &&
	   (int)source % sizeof(int) == 0 &&
	   length % sizeof(int) == 0) {
		is = source; id = dest;
		length = length / sizeof(int);
		while (i > 0) { id[i - 1] = is[i - 1]; --i; }
	} else {
		cs = source; cd = dest;
		while (i > 0) { cd[i - 1] = cs[i - 1]; --i; }
	}

	return dest;
}

int fs_swap32 (int val) {
    return ((val & 0xFF) << 24) |
	       ((val & 0xFF00) << 8) |
	       ((val >> 8) & 0xFF00) |
	       ((val >> 24) & 0xFF);
}

int fs_open (char *path, int mode) {
	// TODO: check loaded image
	return open(path, mode);
}

int fs_read (int fd, void *buf, int length) {
	if (fd > FS_REAL) {
		// TODO
		return 0;
	} else {
		return read(fd, buf, length);
	}
}

int fs_write (int fd, void *buf, int length) {
	if (fd > FS_REAL) {
		// TODO
		return 0;
	} else {
		return write(fd, buf, length);
	}
}

int read8(int fd) {
  char b;
  int r;
 fs_read(fd, &b, 1);
 r = 0;
 r = (int)(b);
 if (r < 0) r = r + 256;
 return (int)r;
 }
int read32(int fd) { int i; i = 0;  fs_read(fd, &i, 4); if(g_flipbits) i = fs_swap32(i); return i; }
int readx  (int fd, void *buf, int length) { return fs_read(fd, buf, length); }

int write8(int fd, int value) { char b; b = (char)(value & 0xFF); return fs_write(fd, &b, 1); }
int write32 (int fd, int value) { return fs_write(fd, &value, 4); }
int writex  (int fd, void *buf, int length) { return fs_write(fd, buf, length); }

void dump_fs (int *fs) {
	printf(" FS [Type 0x%X] [Ver %d]    [ISize %d] [ICount %d] [IUsed %d]",
	       fs[FS_TYPE], fs[FS_VERSION], fs[FS_ISIZE] << 8, fs[FS_ICOUNT], fs[FS_IUSED]);
	printf(" [IRoot %d]", fs[FS_IROOT]);
	printf(" [Capacity %dk]\n", ((fs[FS_ISIZE] << 8) * fs[FS_ICOUNT]) / 1024);
}


char *inode_data (int *inode) { return (char*)inode + (sizeof(int) * IN__SZ); }

void dump_inode (int *inode, int mode) {
	if (mode == 0) {
		printf(" File  [Id 0x%X] [Next 0x%X] [Type", inode[IN_ID], inode[IN_NEXT]);
		if (!inode[IN_TYPE])
			printf(" NUL");
		if (inode[IN_TYPE] & FT_DELETED)
			printf(" DELETED");
		if (inode[IN_TYPE] & FT_FILE)
			printf(" file");
		if (inode[IN_TYPE] & FT_DIRECTORY)
			printf(" dir ");
		printf("]  [Permissions %c%c%c]",
			(inode[IN_PERM] & FP_READ) ? 'R' : '-',
			(inode[IN_PERM] & FP_WRITE) ? 'W' : '-',
			(inode[IN_PERM] & FP_EXECUTABLE) ? 'X' : '-');
		printf(" [Used %d]", inode[IN_LENGTH]);
		printf(" [Total %d]\n", inode[IN_LENGTHTOTAL]);
		printf("  -- Content: 0x%X\n", inode_data(inode));
	}

	printf("%.*s", inode[IN_LENGTH], inode_data(inode));
	if (inode[IN_NEXT] == 0)
		printf("\n  -- End\n");
}

int *fsnode_new (int type, int version, int isize, int icount, int iused, int iroot) {
	int sz, *node, *inodes;

	// allocate fsnode
	if (!(node = malloc(sizeof(int) * FS__SZ))) return 0;
	node[FS_TYPE] = type;
	node[FS_VERSION] = version;
	node[FS_ISIZE] = isize;
	node[FS_ICOUNT] = icount;
	node[FS_IUSED] = iused;
	node[FS_IROOT] = iroot;

	// allocate inodes
	sz = ((IN__SZ * sizeof(int)) + (isize << 8)) * icount;
	if (!(inodes = malloc(sz))) {
		free(node);
		return 0;
	}
	memset(inodes, 0, sz);
	node[FS_DATA] = (int)inodes;
	return node;
}

int *fsnode_inode (int inode, int *fsnode) {
	return (int*)((char*)fsnode[FS_DATA] + (((IN__SZ * sizeof(int)) + (fsnode[FS_ISIZE] << 8)) * (inode - 1)));
}
int *fsnode_iroot (int *fsnode) { return fsnode_inode(fsnode[FS_IROOT], fsnode); }

int *fsnode_find_free (int *fsnode) {
	int inode_id, *inode;

	inode_id = 1;
	while (inode_id < fsnode[FS_ICOUNT]) {
		inode = fsnode_inode(inode_id, fsnode);
		if (inode[IN_TYPE] == 0 || inode[IN_TYPE] & FT_DELETED) {
			// Ensure inode id is set
			inode[IN_ID] = inode_id;
			return inode;
		}
		++inode_id;
	}

	return 0; // no free inode
}

void dump_inode_complete (int *inode, int *fs) {
	int mode;
	mode = 0;
	while(inode) {
		dump_inode(inode, mode);
		//printf("next: %ld\n", inode[IN_NEXT]);
		//dump_fs(fs);
		inode = inode[IN_NEXT] ? fsnode_inode(inode[IN_NEXT], fs) : 0;
		mode = 1;
	}
}


int inode_write_str (int inode, char *data, int type, int permissions, int *fsnode) {
	int *node, length;
	if (inode > fsnode[FS_ICOUNT])
		return 0;
	length = fs_strlen(data);
	node = fsnode_inode(inode, fsnode);
	node[IN_ID] = inode;
	node[IN_NEXT] = 0;
	node[IN_TYPE] = type;
	node[IN_PERM] = permissions;
	node[IN_LENGTH] = length;
	node[IN_LENGTHTOTAL] = length;
	fs_memcpy(data, inode_data(node), length);
	return length;
}

char *inode_append (char ch, int *inode, int *fs) {
	char *data;
	int  *nnode, *onode;
	onode = inode;
	while(inode[IN_NEXT] != 0) {
		inode = fsnode_inode(inode[IN_NEXT], fs);
	}
	if(inode[IN_LENGTH] == fs[FS_ISIZE] << 8) {
		// extend inode
		if (!(nnode = fsnode_find_free(fs))) {
			printf("No free inode to extend to!\n");
			exit(2);
		}
		printf("inode %ld extended into inode %ld\n", inode[IN_ID], nnode[IN_ID]);
		inode[IN_NEXT] = nnode[IN_ID];
		nnode[IN_TYPE] = inode[IN_TYPE];
		nnode[IN_PERM] = inode[IN_PERM];
		inode = nnode;
	}
	data = inode_data(inode) + inode[IN_LENGTH];
	*data = ch;
	inode[IN_LENGTH]++;
	onode[IN_LENGTHTOTAL]++;
	if (inode != onode)
		inode[IN_LENGTHTOTAL]++;
	return data + 1;
}

char *inode_append_itoa (int x, int radix, int *inode, int *fs) {
	char *buf, *c, *last;
	int   length;

	// TODO: make buffer larger?
	if (!(buf = malloc(16))) {
		printf("Error allocated 16 bytes for buffer!\n");
		exit(-1);
	}
	memset(buf, 0, 16);

	length = fs_itoa(x, buf, radix);
	c = buf;
	last = inode_data(inode);
	while(length--) {
		last = inode_append(*c++, inode, fs);
	}
	free(buf);
	return last;
}

int *create_image (int type, int version, int isize, int icount, int iroot) {
	int *fs, *root, file, iused;

	if (!(fs = fsnode_new(type, version, isize, icount, 0, iroot)))
		return 0;
	dump_fs(fs);

	return fs;
}

void add_test_files(int *fs) {
	int *root, iroot, file;

	fs[FS_IROOT] = iroot = 1;
	root = fsnode_iroot(fs);
	inode_write_str(iroot, ". 1\n.. 1\nREADME 2\n", FT_DIRECTORY, FP_READ | FP_EXECUTABLE, fs);
	file = iroot + 1;
	inode_write_str(file, "Hello, world!\n", FT_FILE, FP_READ, fs);
	fs[FS_IUSED] = 2;

	dump_inode(root, 0);
	dump_inode(fsnode_inode(file, fs), 0);
}

int inode_writeall (int fd, int *inode, int inode_sz, int *fs) {
	int *inode2, *inode3, bytes;
	// reset length
	inode[IN_LENGTH] = inode[IN_LENGTHTOTAL] = 0;
	inode3 = inode;
	while ((bytes = read(fd, inode_data(inode3), inode_sz)) > 0) {
		inode2 = inode3;
		//printf("wrote %ld bytes to inode %ld\n", bytes, inode2[IN_ID]);
		inode2[IN_TYPE] = FT_FILE;
		inode2[IN_PERM] = FP_READ;
		fs[FS_IUSED] = fs[FS_IUSED] + 1;
		// local length
		inode2[IN_LENGTH] = bytes;
		// total length
		inode2[IN_LENGTHTOTAL] = inode[IN_LENGTHTOTAL] = inode[IN_LENGTHTOTAL] + bytes;
		if (!(inode3 = fsnode_find_free(fs))) {
			printf("No more inodes\n");
			return 0;
		}
		inode2[IN_NEXT] = inode3[IN_ID];
	}
	// cancel out next inode id
	inode2[IN_NEXT] = 0;
	printf("Length total: %ld\n", inode[IN_LENGTHTOTAL]);
	return inode[IN_LENGTHTOTAL];
}

void setup_directory (int *inode, int *fs) {
	char *data;
	inode[IN_TYPE] = FT_DIRECTORY;
	inode[IN_PERM] = FP_READ | FP_EXECUTABLE;
	data = inode_data(inode);
	// write default entries
	*data++ = '.'; *data++ = ' '; data = data + fs_itoa(inode[IN_ID], data, 10); *data++ = '\n';
	*data++ = '.'; *data++ = '.'; *data++ = ' '; data = data + fs_itoa(inode[IN_ID], data, 10); *data++ = '\n';
	inode[IN_LENGTH] = inode[IN_LENGTHTOTAL] = data - inode_data(inode);
}

int add_specified_files (int count, char **files, int *dnode, int *fs) {
	char *file, *buf, *rootdata, *x;
	int   fd, sz, bytes, tmp;
	int  *inode, *dest, result;

	// use inode size for buffer size
	sz = fs[FS_ISIZE] << 8;

	// destination directory
	dest = dnode;

	while(count > 0) {
		file = *files;
		printf("Adding file: '%s'\n", file);
		if ((fd = open(file, 0)) < 0) {
			printf("Unable to open '%s'\n", file);
			return 0;
		}
		if (!(inode = fsnode_find_free(fs))) {
			printf("No free inodes!\n");
			return 0;
		}
		printf("Got free inode: %ld\n", inode[IN_ID]);
		result = inode_writeall(fd, inode, sz, fs);
		inode[IN_LENGTHTOTAL] = result;
		inode[IN_PERM] = FP_READ;
		close(fd);
		if (!result) {
			printf("Error writing all inodes for file '%s'\n", file);
			return 0;
		}
		// TODO: update dest to directory specified in file
		// write out directory entry
		while(*file) inode_append(*file++, dest, fs);
		// write inode id
		inode_append(' ', dest, fs);
		inode_append_itoa(inode[IN_ID], 10, dest, fs);
		inode_append('\n', dest, fs);
		--count; ++files;
	}

	//printf("Dest node data: %s\n", inode_data(dest));
	dump_inode_complete(dest, fs);

	return 1; // success
}

int write_image_inode(int fd, int *fs, int node) {
	int *inode;
	inode = fsnode_inode(node, fs);
	//printf("Write inode %d\n", node);
	write32(fd, inode[IN_ID] = node);
	write32(fd, inode[IN_NEXT]);
	write8 (fd, inode[IN_TYPE]);
	write8 (fd, inode[IN_PERM]);
	write32(fd, inode[IN_LENGTH]);
	write32(fd, inode[IN_LENGTHTOTAL]);
	writex (fd, inode_data(inode), fs[FS_ISIZE] << 8);
	return 0;
}

int write_image_fs (int fd, int *fs) {
	int i;
	write8 (fd, fs[FS_TYPE]);
	write32(fd, MAGIC);
	printf("write_image_fs with magic(%lX)\n", MAGIC);
	write8 (fd, fs[FS_VERSION]);
	write8 (fd, fs[FS_ISIZE]);
	write32(fd, fs[FS_ICOUNT]);
	write32(fd, fs[FS_IUSED]);
	write32(fd, fs[FS_IROOT]);
	i = 1;
	while (i <= fs[FS_IUSED]) write_image_inode(fd, fs, i++);
	return 0;
}

int write_image (char *file, int *fs) {
	int fd, e, m;
	m = 578; //O_TRUNC | O_CREAT | O_RDWR; //164; // linux: FS_WRONLY | 256;
	if ((fd = open(file, m)) < 0) {
		e = errno;
		printf("write_image(%s) fd returned: %d, errno = %d, mode = %d\n", file, fd, e, m);
		m = 770; // _O_CREAT | _O_TRUNC | _O_RDWR;
		if ((fd = open(file, m)) < 0) {
			e = errno;
			printf("write_image(%s) fd returned: %d, errno = %d, mode = %d\n", file, fd, e, m);
			return 1;
		}
	}
	printf("write_image(%s) success fd returned: %d, mode = %d\n", file, fd, m);
	write_image_fs(fd, fs);
	close(fd);
	return 0;
}

int *read_image_fs (int fd) {
	int i_type, i_version, i_size, i_count, i_used, i_root;
	int *fs, i, *inode, idatasz, e, magic;
	i_type = read8(fd);
	magic = read32(fd);
	if(magic != MAGIC) {
		//printf("WARN: magic(%lX) vs %lX\n", magic, MAGIC);
		// Try swapping endianess
		magic = fs_swap32(magic);
		g_flipbits = 1;
		if(magic != MAGIC) {
			printf("ERROR: magic(%lX) vs %lX\n", magic, MAGIC);
			printf("Signature verification failed\n");
			return 0;
		}
	}
	i_version = read8(fd);
	i_size = read8(fd);
	i_count = read32(fd);
	i_used = read32(fd);
	i_root = read32(fd);
	printf("Read fs with type(%ld) version(%ld) isize(%ld) icount(%ld) iroot(%ld), magic(%lX)\n",
		   i_type, i_version, i_size, i_count, i_root, magic);
	if (!(fs = fsnode_new(i_type, i_version, i_size, i_count, i_used, i_root))) {
		printf("Unable to create fs with type(%ld) version(%ld) isize(%ld) icount(%ld) iroot(%ld)\n",
		       i_type, i_version, i_size, i_count, i_root);
		return 0;
	}
	i = 1;
	idatasz = i_size << 8;
	while(i <= i_used) {
		inode = fsnode_inode(i, fs);
		inode[IN_ID] = read32(fd);
		inode[IN_NEXT] = read32(fd);
		inode[IN_TYPE] = read8(fd);
		inode[IN_PERM] = read8(fd);
		inode[IN_LENGTH] = read32(fd);
		inode[IN_LENGTHTOTAL] = read32(fd);
		fs_read(fd, inode_data(inode), idatasz);
		++i;
	}
	// set other nodes
	while(i < i_count) {
		inode = fsnode_inode(i, fs);
		inode[IN_ID] = i;
		++i;
	}

	return fs;
}

int *read_image (char *file) {
	int fd, *fs;

	if ((fd = open(file, 0)) < 0) {
		printf("read_image(%s) fd returned: %d\n", file, fd);
		return 0;
	}
	fs = read_image_fs(fd);
	close(fd);
	return fs;
}

void free_image (int *fs) {
	free((char*)fs[FS_DATA]);
	free(fs);
}

int *g_image_fs;

enum { // read_options modes
	M_ACTION,  // action, one of: c (create) or r (read)
	M_ARCHIVE, // archive specification
	M_FILES,   // files to read/add
	M_ISIZE,   // Specifying isize
	M_ICOUNT   // Specifying icount
};

char *invocation;
void dump_usage () {
	printf("%s [options] <action> [archive] [--] [files...]\n", invocation);
	printf("\n");
	printf("<action> is required, and one of:\n");
	printf("  c                          Create archive\n");
	printf("  r                          Read archive\n");
	printf("[archive] (default: fs.img)  Archive to use\n");
	printf("[files...]                   File specification\n");
	printf("[options] are:\n");
	printf("  -isize n                   Set inode size (in bytes)\n");
	printf("                             Minimum: 256, trimmed to nearest 256 bytes\n");
	printf("  -icount n                  Set inode count\n");
}

int read_options (int argc, char **argv) {
	int mode, modeold;
	mode = M_ACTION;
	invocation = *argv++; --argc;
	while(argc) {
		if (mode != M_FILES && **argv == '-') {
			if (*(*argv + 1) == '-' && *(*argv + 2) == 0) {
				mode = M_FILES;
			} else if (!fs_strcmp("isize", *argv + 1)) {
				modeold = mode;
				mode = M_ISIZE;
			} else if (!fs_strcmp("icount", *argv + 1)) {
				modeold = mode;
				mode = M_ICOUNT;
			} else if (!fs_strcmp("help", *argv + 1) || !fs_strcmp("-help", *argv + 1)) {
				dump_usage();
				exit(0);
			} else {
				printf("Unknown option: '%s'\n", *argv);
				dump_usage();
				return 0;
			}
		} else if (mode == M_ISIZE || mode == M_ICOUNT) {
			if (mode == M_ISIZE) { // value must be shifted
				options[OPT_ISIZE] = fs_atoi(*argv, 10) >> 8;
				if (options[OPT_ISIZE] == 0) {
					options[OPT_ISIZE] = 1;
					printf("Invalid isize! Setting to minimum value %ld\n", 1 << 8);
				}
			} else {
				options[OPT_ICOUNT] = fs_atoi(*argv, 10);
			}
			mode = modeold;
		} else if (mode == M_ACTION) {
			if (**argv == 'c') options[OPT_CREATE] = 1;
			else if (**argv == 'r') options[OPT_READ] = 1;
			else { printf("Invalid action, must be one of: cr (create, read)\n"); return 0; }
			mode = M_ARCHIVE;
		} else if (mode == M_ARCHIVE) {
			options[OPT_ARCHIVE] = (int)*argv;
			mode = M_FILES;
		} else if (mode == M_FILES) {
			options[OPT_FILES] = (int)argv;
			options[OPT_FILESCOUNT] = argc;
			return 1;
		}
		--argc; ++argv;
	}
	return 1;
}

int main (int argc, char **argv) {
	int *iroot;

	g_flipbits = 0;
	g_image = "fs.img";
	if (!(options = malloc(sizeof(int) * OPT__SZ))) {
		printf("Failed to allocate %ld bytes for options\n", sizeof(int) * OPT__SZ);
		return 1;
	}
	memset(options, 0, sizeof(int) * OPT__SZ);
	// Set default options
	options[OPT_ARCHIVE] = (int)g_image;
	options[OPT_FSTYPE] = DEF_FSTYPE;
	options[OPT_FSVERSION] = DEF_FSVERSION;
	options[OPT_ISIZE] = DEF_ISIZE;
	options[OPT_ICOUNT] = DEF_ICOUNT;
	if (!read_options(argc, argv)) {
		printf("Options parsing failed!\n");
		return 1;
	}
	if (!options[OPT_CREATE] && !options[OPT_READ]) {
		printf("No action specific. Try specifying c (for create) or r (for read)\n");
		dump_usage();
		return 1;
	}

	if (options[OPT_CREATE]) {
		g_image_fs = create_image(options[OPT_FSTYPE], options[OPT_FSVERSION],
		                          options[OPT_ISIZE],  options[OPT_ICOUNT], 1);
		iroot = fsnode_iroot(g_image_fs);
		iroot[IN_ID] = 1;
		setup_directory(iroot, g_image_fs);
		if (options[OPT_FILESCOUNT] == 0)
			add_test_files(g_image_fs);
		else
			if (!add_specified_files(options[OPT_FILESCOUNT], (char**)options[OPT_FILES], iroot, g_image_fs)) {
				printf("Failed to add specified files\n");
				return 1;
			}
		write_image((char*)options[OPT_ARCHIVE], g_image_fs);
		free_image(g_image_fs);
	}
	if (options[OPT_READ]) {
		if (!(g_image_fs = read_image((char*)options[OPT_ARCHIVE]))) {
			printf("Unable to read image: %s\n", (char*)options[OPT_ARCHIVE]);
			return 1;
		}
		dump_fs(g_image_fs);
		dump_inode_complete(fsnode_inode(g_image_fs[FS_IROOT], g_image_fs), g_image_fs);
		dump_inode_complete(fsnode_inode(2, g_image_fs), g_image_fs);
		free_image(g_image_fs);
	}

	free(options);
	return 0;
}
