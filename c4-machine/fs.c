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
   0x14..     Content (max size: inodesize<<8 - 14)


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

int test;
enum { FS_REAL = 1024 }; // Real file ids are less than this
enum {
	DEF_FSTYPE = 204, // 0xCC
	DEF_FSVERSION = 0,
	DEF_ISIZE = 1, // shifted left 8
	DEF_ICOUNT = 1024
};

enum {
	SZ_INODE_HDR = 14,
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

char *g_image;

int fs_strlen (char *s) { char *t; t = s; while (*t++) ; return t - s; }
int fs_strncmp (char *s1, char *s2, int n) { return memcmp(s1, s2, n); }
int fs_strcmp (char *s1, char *s2) { while(*s1 && (*s1 == *s2)) { ++s1; ++s2; } return *s1 == *s2; }

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
	sz = icount * (isize << 8);
	if (!(inodes = malloc(sz))) {
		free(node);
		return 0;
	}
	memset(inodes, 0, sz);
	node[FS_DATA] = (int)inodes;
	return node;
}

int *fsnode_inode (int inode, int *fsnode) {
	return (int*)((char*)fsnode[FS_DATA] + ((inode - 1) * (fsnode[FS_ISIZE] << 8)));
}

int *fsnode_iroot (int *fsnode) { return fsnode_inode(fsnode[FS_IROOT], fsnode); }
char *inode_data (int *inode) { return (char*)inode + (sizeof(int) * IN__SZ); }

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
	fs_memcpy(data, inode_data(node), length);
	return length;
}

void dump_fs (int *fs) {
	printf(" FS [Type 0x%X] [Ver %d]    [ISize %d]      [ICount %d]  [IUsed %d]",
	       fs[FS_TYPE], fs[FS_VERSION], fs[FS_ISIZE] << 8, fs[FS_ICOUNT], fs[FS_IUSED]);
	printf(" [IRoot %d]", fs[FS_IROOT]);
	printf(" [Capacity %dk]\n", ((fs[FS_ISIZE] << 8) * fs[FS_ICOUNT]) / 1024);
}

void dump_inode (int *inode) {
	printf(" File  [Id 0x%X] [Next 0x%X] [Type", inode[IN_ID], inode[IN_NEXT]);
	if (!inode[IN_TYPE])
		printf(" NUL");
	if (inode[IN_TYPE] & FT_DELETED)
		printf(" DELETED");
	if (inode[IN_TYPE] & FT_FILE)
		printf(" file");
	if (inode[IN_TYPE] & FT_DIRECTORY)
		printf(" dir ");
	printf("] [Permissions %c%c%c]",
		(inode[IN_PERM] & FP_READ) ? 'R' : '-',
		(inode[IN_PERM] & FP_WRITE) ? 'W' : '-',
		(inode[IN_PERM] & FP_EXECUTABLE) ? 'X' : '-');
	printf(" [Length %d]\n", inode[IN_LENGTH]);
	printf("  -- Content: 0x%X\n%.*s\n  -- End\n", inode_data(inode), inode[IN_LENGTH], inode_data(inode));
}

int *create_image (int type, int version, int isize, int icount, int iroot) {
	int *fs, *root, file, iused;

	iused = 2;

	if (!(fs = fsnode_new(type, version, isize, icount, iused, iroot)))
		return 0;

	root = fsnode_iroot(fs);
	inode_write_str(iroot, ". 1\n.. 1\nREADME 2\n", FT_DIRECTORY, FP_READ | FP_EXECUTABLE, fs);
	file = iroot + 1;
	inode_write_str(file, "Hello, world!\n", FT_FILE, FP_READ, fs);

	dump_fs(fs);
	dump_inode(root);
	dump_inode(fsnode_inode(file, fs));

	return fs;
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
	writex (fd, inode_data(inode), (fs[FS_ISIZE] << 8) - SZ_INODE_HDR);
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
	while (i < fs[FS_ICOUNT]) write_image_inode(fd, fs, i++);
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
	idatasz = (i_size << 8) - SZ_INODE_HDR;
	while(i <= i_used) {
		inode = fsnode_inode(i, fs);
		inode[IN_ID] = read32(fd);
		inode[IN_NEXT] = read32(fd);
		inode[IN_TYPE] = read8(fd);
		inode[IN_PERM] = read8(fd);
		inode[IN_LENGTH] = read32(fd);
		if(1)
			fs_read(fd, inode_data(inode), idatasz);
		else
			if ((e = fs_read(fd, inode_data(inode), idatasz)) != idatasz) {
				printf("Error reading inode %d, result: %d\n", i, e);
				free(fs);
				return 0;
			}
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
	//free((char*)fs[FS_DATA]);
	//free(fs);
}

int *g_image_fs;
enum { // options
	OPT_CREATE,
	OPT_READ,
	OPT_ARCHIVE,
	OPT_FILES,
	OPT__SZ
};

enum { // read_options modes
	M_ACTION,  // action, one of: c (create) or r (read)
	M_ARCHIVE, // archive specification
	M_FILES    // files to read/add
};
int *options;

int read_options (int argc, char **argv) {
	int mode;
	mode = M_ACTION;
	--argc; ++argv; // skip invocation name
	while(argc) {
		if (mode == M_ACTION) {
			if (**argv == 'c') options[OPT_CREATE] = 1;
			else if (**argv == 'r') options[OPT_READ] = 1;
			else { printf("Invalid action, must be one of: cr (create, read)\n"); return 0; }
			mode = M_ARCHIVE;
		} else if (mode == M_ARCHIVE) {
			options[OPT_ARCHIVE] = (int)*argv;
			mode = M_FILES;
		} else if (mode == M_FILES) {
			printf("Warn: filespec not implemented for argument '%s'\n", argv);
		}
		--argc; ++argv;
	}
	return 1;
}

int main (int argc, char **argv) {
	int create_fstype, create_fsversion, create_isize, create_icount;

	g_flipbits = 0;
	g_image = "fs.img";
	create_fstype = DEF_FSTYPE;
	create_fsversion = DEF_FSVERSION;
	create_isize = DEF_ISIZE;
	create_icount = DEF_ICOUNT;
	if (!(options = malloc(sizeof(int) * OPT__SZ))) {
		printf("Failed to allocate %ld bytes for options\n", sizeof(int) * OPT__SZ);
		return 1;
	}
	memset(options, 0, sizeof(int) * OPT__SZ);
	options[OPT_ARCHIVE] = (int)g_image;
	if (!read_options(argc, argv)) {
		printf("Options parsing failed!\n");
		return 1;
	}
	if (!options[OPT_CREATE] && !options[OPT_READ]) {
		printf("No action specific. Try specifying c (for create) or r (for read)\n");
		return 1;
	}

	if (options[OPT_CREATE]) {
		g_image_fs = create_image(create_fstype, create_fsversion, create_isize, create_icount, 1);
		write_image((char*)options[OPT_ARCHIVE], g_image_fs);
	}
	free_image(g_image_fs);
	if (options[OPT_READ]) {
		if (!(g_image_fs = read_image((char*)options[OPT_ARCHIVE]))) {
			printf("Unable to read image: %s\n", (char*)options[OPT_ARCHIVE]);
			return 1;
		}
		dump_fs(g_image_fs);
		dump_inode(fsnode_inode(1, g_image_fs));
		dump_inode(fsnode_inode(2, g_image_fs));
		free_image(g_image_fs);
	}
	return 0;
}
