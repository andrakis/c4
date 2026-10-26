// C4 Test: linking, a module for --start-lib and --gc (make test-link).
// Nothing names it: given after --start-lib it is left out, and linked
// whole, --gc drops it. Either way the image is the same bytes as
// test_link.c4r (modules A and B alone).

int c_unused (int x) { return x + 1; }
