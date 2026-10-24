// srcmap_link.c -- test-srcmap's second mapped object (docs/c4-explorer.md,
// S2). It is linked after srcmap_probe.c and an object with no map
// (src/c4sp/tests/srcmap-link.sh). It includes srcmap_probe.h too, with
// fact renamed so that the two do not collide, so the linked map's file
// table must name that header once.
#define fact link_fact
#include "srcmap_probe.h"

int link_sum(int n)
{
	int s;
	s = 0;
	while (n > 0) {
		s = s + square(n);
		n = n - 1;
	}
	return s + link_fact(3);
}
