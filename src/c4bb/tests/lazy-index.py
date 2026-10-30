#!/usr/bin/env python3
# lazy-index.py DISK PREFIX... -- DISK/index.json, the web page's table of
# a lazy disk (libjs/lazy.js), from DISK/c4ix.sizes: [NAME, SIZE] for each
# file, c4ix.sizes itself last. A file under one of the PREFIXes is
# [NAME, SIZE, HASH] instead, the first 16 hex digits of its SHA-256: the
# page keeps such a file once it has read it, and the hash is what it is
# kept under, so a file that changes is read again (docs/c4ix-desktop.md,
# part twenty).
import hashlib, json, os, sys

disk, prefixes = sys.argv[1], tuple(sys.argv[2:])
index = []
for line in open(os.path.join(disk, 'c4ix.sizes')).read().split('\n'):
    if not line:
        continue
    size, name = line.split(' ', 1)
    e = [name, int(size)]
    if prefixes and name.startswith(prefixes):
        e.append(hashlib.sha256(open(os.path.join(disk, name), 'rb').read()).hexdigest()[:16])
    index.append(e)
index.append(['c4ix.sizes', os.path.getsize(os.path.join(disk, 'c4ix.sizes'))])
open(os.path.join(disk, 'index.json'), 'w').write(json.dumps(index, separators=(',', ':')) + '\n')
