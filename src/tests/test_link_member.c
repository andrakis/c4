// C4 Test: linking, a module nothing names (make test-link). Given after
// --start-lib it is left out; linked plainly, c4rlink drops its function
// (--no-gc keeps it). Either way the image is the same bytes as
// test_link.c4r (modules A and B alone).

int c_unused (int x) { return x + 1; }
