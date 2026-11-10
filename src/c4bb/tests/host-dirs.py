#!/usr/bin/env python3
# host-dirs.py DISK PREFIX... -- a listing, .c4ix-dir, in DISK/PREFIX and in
# every directory under it: what C4IX's kernel reads to make a host
# directory's entries the first time something looks in it (src/c4ix/vfs.c,
# vfs_lazydir; vfsload makes one of each hostdir(PREFIX) in the manifest).
# A line each, sorted: "SIZE NAME" a file, "d NAME" a directory. A PREFIX
# not on the disk is passed over.
import os, sys

LIST = '.c4ix-dir'
disk, prefixes = sys.argv[1], sys.argv[2:]
n = 0
for p in prefixes:
    top = os.path.join(disk, p)
    if not os.path.isdir(top):
        continue
    for d, dirs, files in os.walk(top):
        dirs.sort()
        lines = [f'd {x}' for x in dirs]
        lines += [f'{os.path.getsize(os.path.join(d, f))} {f}' for f in sorted(files) if f != LIST]
        open(os.path.join(d, LIST), 'w').write(''.join(l + '\n' for l in lines))
        n += 1
print(n)
