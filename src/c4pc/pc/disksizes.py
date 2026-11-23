#!/usr/bin/env python3
# disksizes.py SIZES DIR -- the boot disk's c4ix.sizes ("SIZE NAME" a line,
# what C4IX's VFS believes of a host file until it reads it: vfsload.c)
# with every file in DIR's top level given its size there, and those it
# did not list added. c4pc's .c4pc/disk.tar lays .c4pc/add's files over the
# images' (Makefile): a program or library of another size with the old
# line was read short or cut off by the kernel ("a patch names symbol 95,
# which is not in its table"). Written to standard output.
import os, sys

sizes, d = sys.argv[1], sys.argv[2]
have = {}
for f in os.listdir(d):
    p = os.path.join(d, f)
    if os.path.isfile(p) and f != "c4ix.sizes":
        have[f] = os.path.getsize(p)
out = []
for line in open(sizes, encoding="latin-1"):
    line = line.rstrip("\n")
    n, _, name = line.partition(" ")
    if name in have:
        out.append("%d %s" % (have.pop(name), name))
    else:
        out.append(line)
for name in sorted(have):
    out.append("%d %s" % (have[name], name))
sys.stdout.write("\n".join(out) + "\n")
