// test_c4l_bss.c -- does c4l.c give an image its BSS?
//
// c4lc writes format v3: uninitialized globals occupy no image bytes,
// and the header's MEMSZ word (where v2 had 8 padding bytes) says how big
// the data segment is in memory. c4l.c used to allocate only the stored
// DATALEN, so these arrays lay past the end of its allocation, in memory
// the next malloc handed out again. Plain c4's opcodes only: c4l runs it.
int stored = 42;                     // in the image: DATALEN is not zero
int big[50000];                      // 400 KB of BSS at 64 bits
char tail[1000];

int main () {
	int i, bad;
	char *m;

	bad = 0;
	i = 0;
	while (i < 50000) { if (big[i]) bad = bad + 1; i = i + 1; }
	i = 0;
	while (i < 1000) { if (tail[i]) bad = bad + 1; i = i + 1; }
	if (bad) { printf("c4l_bss: %d words of BSS not zero\n", bad); return 1; }
	i = 0;
	while (i < 50000) { big[i] = i * 3 + 1; i = i + 1; }
	memset(tail, 'x', 1000);
	// memory after the data segment, scribbled on: none of it is BSS's
	m = malloc(500000);
	memset(m, 0, 500000);
	i = 0;
	while (i < 50000) { if (big[i] != i * 3 + 1) bad = bad + 1; i = i + 1; }
	i = 0;
	while (i < 1000) { if (tail[i] != 'x') bad = bad + 1; i = i + 1; }
	if (bad || stored != 42) { printf("c4l_bss: %d words of BSS lost\n", bad); return 1; }
	printf("c4l_bss: ok\n");
	return 0;
}
