#!/usr/bin/env python3
# lazy-index.py DISK SIZES PREFIX... -- DISK/index.json, the web page's table
# of a lazy disk (libjs/lazy.js), from SIZES (a "SIZE NAME" line per file,
# as c4ix.sizes is, but every file): [NAME, SIZE] for each file, c4ix.sizes
# itself last. A file under one of the PREFIXes is
# [NAME, SIZE, HASH] instead, the first 16 hex digits of its SHA-256: the
# page keeps such a file once it has read it, and the hash is what it is
# kept under, so a file that changes is read again (docs/c4ix-desktop.md,
# part twenty-one). Not the directories' .c4ix-dir listings, which are
# small and read once a boot, when C4IX first looks in the directory.
import hashlib, json, os, sys

disk, sizes, prefixes = sys.argv[1], sys.argv[2], tuple(sys.argv[3:])
index = []
for line in open(sizes).read().split('\n'):
    if not line:
        continue
    size, name = line.split(' ', 1)
    e = [name, int(size)]
    if prefixes and name.startswith(prefixes) and os.path.basename(name) != '.c4ix-dir':
        e.append(hashlib.sha256(open(os.path.join(disk, name), 'rb').read()).hexdigest()[:16])
    index.append(e)
index.append(['c4ix.sizes', os.path.getsize(os.path.join(disk, 'c4ix.sizes'))])
open(os.path.join(disk, 'index.json'), 'w').write(json.dumps(index, separators=(',', ':')) + '\n')
