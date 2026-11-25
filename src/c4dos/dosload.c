// dosload.c -- LOADLIN for C4DOS.
//
// You could boot Linux from a DOS prompt: LOADLIN read the kernel into
// memory, released what DOS was holding, and jumped. This is that, for
// this machine.
//
// BE CLEAR ABOUT WHAT THIS DOES NOT DO. `RUN c4ke.c4r` already works,
// and C4DOS already writes its API table into any image that exports
// __c4dos_api -- so a kernel started the ordinary way can already find
// the RAM disk (see src/c4ke/extensions/c4ke_dos.c, which is what
// actually seeds C4KE). dosload adds two things on top:
//
//   MEMORY.  DOS holds a 4MB read scratch and cannot free it while it
//            is running a program, because the loaded image's
//            constructor and destructor tables point INTO that scratch
//            (c4dos.c's c4r_load, and the comment there says so). This
//            loader copies those tables out first, so it can hand the
//            scratch back before the kernel starts. A kernel about to
//            compile C4IX wants every byte.
//
//   A COMMAND LINE.  RUN passes the transient's own arguments. dosload
//            passes the image's name as argv[0] and everything after
//            it as the kernel's own, which is how `-v 50`, `-c N` or an
//            alternate init reach c4ke's parse_commandline.
//
// Dialect and hardware: strict c4, nothing above the EXIT opcode. The
// player reaches this rung with a CPU that runs C4DOS; loading a
// kernel must not be the thing that demands a better one. `./c4 c4l.c
// dosload.c4r` is the pin -- c4l refuses an image that uses more, and
// names the instruction.
//
// Build:  ./c4cc -o dosload.c4r include/c4dos.h src/c4dos/dosload.c
// Usage:  RUN dosload.c4r <image.c4r> [kernel arguments...]

enum { DL_CHUNK = 65536 };
enum { DL_CAP0  = 1048576 };   // first guess at an image size; doubles

// The loaded image, the plain-c4 "struct". Same shape as c4dos.c's
// img_* registers, for the same reason: one image is loaded at a time
// and this dialect has no structs.
int *dl_code; char *dl_data; int dl_entry;
int *dl_cons; int dl_ncons;    // ABSOLUTE addresses, copied out of the
int *dl_des;  int dl_ndes;     // image buffer (see the header comment)

int dl_wordat (char *p) { return *(int *)p; }

int dl_nameis (char *p, int len, char *want) {
  int i;
  i = 0;
  while (i < len) {
    if (!want[i]) return 0;
    if (p[i] != want[i]) return 0;
    ++i;
  }
  return !want[i];
}

// Walk the symbol section for __c4dos_api and write the table's
// address into the image's global. c4dos.c's inject_api, verbatim in
// intent: what we forward is the table WE were handed, so a kernel
// loaded through dosload sees exactly what one started by RUN sees.
int dl_inject_api (char *p, int nsyms, char *database, int *api) {
  int i, cls, namelen, value;
  i = 0;
  while (i < nsyms) {
    p = p + sizeof(int);                       // id
    p = p + sizeof(int);                       // type
    cls = dl_wordat(p); p = p + sizeof(int);   // class
    p = p + sizeof(int);                       // attrs
    namelen = *p; p = p + 1;
    if (dl_nameis(p, namelen, "__c4dos_api")) {
      p = p + namelen;
      value = dl_wordat(p);
      if (cls == 131) *(int *)(database + value) = (int)api;  // Glo
      return 1;
    }
    p = p + namelen;
    p = p + sizeof(int);                       // value
    ++i;
  }
  return 0;
}

// Read a whole file through DOS -- which checks the RAM disk before
// the real one, so `dosload c4ke.c4r` finds the kernel BUILD.BAT just
// compiled rather than the prebuilt one on the floppy, and gets case
// resolution and "./" stripping for free. Returns bytes read, and
// hands the buffer back through dl_slurp_buf.
int *dl_slurp_buf;

int dl_slurp (char *name) {
  char *buf, *nb;
  int h, n, i, total, cap;

  if ((h = dos_fopen(name)) < 0) return 0 - 1;
  cap = DL_CAP0;
  if (!(buf = malloc(cap))) { dos_fclose(h); printf("dosload: out of memory\n"); return 0 - 1; }
  total = 0;
  n = 1;
  while (n > 0) {
    if (total + DL_CHUNK > cap) {
      // realloc is documented broken under c4m, so double by hand --
      // the same trade c4dos.c's ram_write makes.
      if (!(nb = malloc(cap * 2))) {
        free(buf); dos_fclose(h);
        printf("dosload: out of memory growing past %d bytes\n", cap);
        return 0 - 1;
      }
      i = 0;
      while (i < total) { nb[i] = buf[i]; ++i; }
      free(buf);
      buf = nb;
      cap = cap * 2;
    }
    n = dos_fread(h, buf + total, DL_CHUNK);
    if (n > 0) total = total + n;
  }
  dos_fclose(h);
  dl_slurp_buf = (int *)buf;
  return total;
}

// Parse a .c4r image out of `buf` into the dl_* registers. Returns 1
// on success. This is c4dos.c's c4r_load with three differences: it
// reads from our own buffer instead of DOS's scratch, it COPIES the
// constructor and destructor tables out and resolves them to absolute
// addresses (src/c4ix/loader.c's move), and it therefore leaves the
// buffer free to be released the moment we are done with it.
int dl_parse (char *buf, int total, char *path, int *api) {
  char *p, *data;
  int *code;
  int entry, codelen, datalen, patchlen, symlen, conslen, deslen, memsz;
  int i, ptype, paddr, pvalu;

  if (total < 13) { printf("dosload: %s is not a program\n", path); return 0; }
  p = buf;
  if (!(p[0] == 'C' && p[1] == '4' && p[2] == 'R')) {
    printf("dosload: bad signature in %s\n", path);
    return 0;
  }
  if (p[4] / 8 != sizeof(int)) {
    printf("dosload: %d-bit image, this machine is %d-bit\n", p[4], sizeof(int) * 8);
    return 0;
  }
  memsz = 0;
  if (p[3] >= 3) memsz = dl_wordat(p + 5);   // v3: data MEMSZ rides the padding
  p = p + 13;

  entry    = dl_wordat(p); p = p + sizeof(int);
  codelen  = dl_wordat(p); p = p + sizeof(int);
  datalen  = dl_wordat(p); p = p + sizeof(int);
  patchlen = dl_wordat(p); p = p + sizeof(int);
  symlen   = dl_wordat(p); p = p + sizeof(int);
  conslen  = dl_wordat(p); p = p + sizeof(int);
  deslen   = dl_wordat(p); p = p + sizeof(int);
  if (memsz < datalen) memsz = datalen;

  p = p + sizeof(int);   // 'C' marker
  if (!(code = malloc(codelen * sizeof(int)))) { printf("dosload: out of memory\n"); return 0; }
  i = 0;
  while (i < codelen) { code[i] = dl_wordat(p + i * sizeof(int)); ++i; }
  p = p + codelen * sizeof(int);

  p = p + sizeof(int);   // 'D' marker
  if (!(data = malloc(memsz + sizeof(int)))) { free(code); printf("dosload: out of memory\n"); return 0; }
  memset(data, 0, memsz + sizeof(int));
  i = 0;
  while (i < datalen) { data[i] = p[i]; ++i; }
  p = p + datalen;

  p = p + sizeof(int);   // 'P' marker
  i = 0;
  while (i < patchlen) {
    ptype = dl_wordat(p); paddr = dl_wordat(p + sizeof(int)); pvalu = dl_wordat(p + 2 * sizeof(int));
    p = p + 3 * sizeof(int);
    if (ptype == 0 - 1) code[paddr] = (int)(code + pvalu);
    else if (ptype == 0 - 2) code[paddr] = (int)(data + pvalu);
    else if (ptype == 0 - 3) *(int *)(data + paddr) = (int)(code + pvalu);
    else if (ptype == 0 - 4) *(int *)(data + paddr) = (int)(data + pvalu);
    ++i;
  }

  // Constructors and destructors. c4dos.c points straight into the
  // image buffer here and warns that they must be run before the next
  // load; we copy them out and resolve them, which is the whole reason
  // this loader can give DOS its memory back.
  p = p + sizeof(int);   // 'c' marker
  dl_ncons = conslen;
  dl_cons = 0;
  if (conslen) {
    if (!(dl_cons = malloc(conslen * sizeof(int)))) { printf("dosload: out of memory\n"); return 0; }
    i = 0;
    while (i < conslen) { dl_cons[i] = (int)(code + dl_wordat(p + i * sizeof(int))); ++i; }
  }
  p = p + conslen * sizeof(int);

  p = p + sizeof(int);   // 'd' marker
  dl_ndes = deslen;
  dl_des = 0;
  if (deslen) {
    if (!(dl_des = malloc(deslen * sizeof(int)))) { printf("dosload: out of memory\n"); return 0; }
    i = 0;
    while (i < deslen) { dl_des[i] = (int)(code + dl_wordat(p + i * sizeof(int))); ++i; }
  }
  p = p + deslen * sizeof(int);

  p = p + sizeof(int);   // 'S' marker
  dl_inject_api(p, symlen, data, api);

  dl_code = code;
  dl_data = data;
  dl_entry = (int)(code + entry);
  return 1;
}

int main (int argc, char **argv) {
  char *path;
  int total, i, r, freed;

  if (!dos_present()) {
    printf("dosload: this only runs under C4DOS\n");
    return 1;
  }
  if (argc < 2) {
    printf("dosload: LOADLIN for C4DOS\n");
    printf("usage: dosload <image.c4r> [arguments...]\n");
    printf("  Loads the image, hands DOS's memory back, and runs it,\n");
    printf("  passing everything after the name as its command line.\n");
    return 1;
  }
  if (!dos_readable()) {
    printf("dosload: this C4DOS cannot be asked to open files\n");
    return 1;
  }
  path = argv[1];

  if ((total = dl_slurp(path)) < 0) {
    printf("dosload: file not found: %s\n", path);
    return 1;
  }
  if (!dl_parse((char *)dl_slurp_buf, total, path, __c4dos_api)) {
    free((char *)dl_slurp_buf);
    return 1;
  }
  // The image buffer has done its job: everything is copied out.
  free((char *)dl_slurp_buf);
  dl_slurp_buf = 0;

  // ...and so has DOS's scratch. It re-allocates on demand, so this
  // costs a malloc if we ever come back to the prompt, and nothing at
  // all if the kernel keeps the machine -- which is the usual case.
  //
  // One thing this relies on: DOS's OWN img_des for us points into that
  // scratch, and DOS walks it after we return. dosload declares no
  // destructors, so that walk has zero iterations and never reads the
  // freed memory. Do not add one to this file.
  freed = dos_trim();
  if (freed) printf("dosload: %d bytes released, loading %s\n", freed, path);
  else printf("dosload: loading %s\n", path);

  // The stub is already armed (dos_fopen went through it), but say so
  // out loud: everything below is an indirect call, and an unarmed
  // stub writes its target through a null pointer.
  if (!__c4dos_arm()) { printf("dosload: could not arm the invoke stub\n"); return 1; }

  i = 0;
  while (i < dl_ncons) { __c4dos_call1((int *)dl_cons[i], 0); ++i; }
  // argv[1] becomes the image's argv[0]: a kernel skips its own name
  // when it parses options (c4ke.c's parse_commandline does exactly
  // that), so it has to be there.
  r = __c4dos_call2((int *)dl_entry, argc - 1, (int)(argv + 1));
  i = 0;
  while (i < dl_ndes) { __c4dos_call0((int *)dl_des[i]); ++i; }

  free((char *)dl_code);
  free(dl_data);
  if (dl_cons) free((char *)dl_cons);
  if (dl_des) free((char *)dl_des);
  return r;
}
