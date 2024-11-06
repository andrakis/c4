#include <stdio.h>
#include <stdlib.h>
#include <memory.h>

#define int long long

int **_allocate_many (int size, int count) {
	int i, *p, **dest, **r;

	if (!(dest = malloc(i = sizeof(int *) * count))) {
		printf("_allocate_many(%lld, %lld, %p) failed on buffer allocation of %lld bytes\n", size, count, r, i);
		exit(1);
	}
	r = dest;
	i = 0;
	while (i++ < count) {
		if (!(*dest = malloc(size))) {
			printf("_allocate_many(%lld, %lld, %p) failed on iteration %lld\n", size, count, r, i - 1);
			exit(1);
		}
		++dest;
	}

	return r;
}

void _free_many (int count, int **ptrs) {
	int i;
	i = 0;
	while (i++ < count) {
		free(*ptrs);
		*ptrs = 0;
		++ptrs;
	}
}

void test_malloc_many () {
	int **bigbuffers, **smallbuffers1, **smallbuffers2;
	int i, bufcount;

	bufcount = 4;

	bigbuffers = _allocate_many(64, bufcount);
	smallbuffers1 = _allocate_many(8, bufcount);


}

void test_malloc_consolidate () {
	int *buf1, *buf2, *buf3, *buf4;
	int *buf5, *buf6;
	int  sz;

	buf1 = malloc(8);
	buf2 = malloc(8);
	buf3 = malloc(8);
	buf4 = malloc(8);
	printf("buf1 = %p, buf2 = %p, buf3 = %p, buf4 = %p\n", buf1, buf2, buf3, buf4);

	free(buf1); free(buf2);
	buf5 = malloc(16);
	printf("buf5 = %p\n", buf5);
	free(buf5);
	free(buf4);
	free(buf3);
	
	buf6 = malloc(64);
	printf("buf6 = %p\n", buf6);
	free(buf6);
}

int main (int argc, char **argv) {
	char *buf, *x;
	int   buflen, i;

	buflen = 32 + 1;
	if (!(buf = malloc(buflen))) {
		printf("Unable to allocate %lld bytes for buf\n", buflen);
		return -1;
	}
	// memset
	memset(buf, 0, buflen);
	printf("Empty: '%s'\n", buf);

	// memcmp
	printf("memcmp equal: %lld\n", (int)memcmp("test", "test", 5));
	return 0;

	// initialize buffer contents
	i = 0;
	while (i < buflen) { buf[i] = 'A' + i; ++i; }
	buf[buflen - 1] = 0;

	printf("In the buffer: %s\n", buf);

	test_malloc_consolidate();

	free(buf);
	return 0;
}
