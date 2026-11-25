#!/usr/bin/env python3
# unpacked.py WHAT DISK -- the files DISK has as recipes (libjs/unpack.js):
# NAME.unpack beside the package NAME is made from (Quake's pak0.pak.unpack
# beside quake106.zip, with QUAKE_AS_ZIP=1: games/quake/c4-images.sh). They
# are not files on the disk, but the machine is to see them, so each of the
# disk's listings names them as the image build makes it:
#   sizes     prints "SIZE NAME" for each, as $OUT/.disk-sizes has a file
#             (for C4IX's c4ix.sizes and the disk's index.json)
#   listings  adds each to its directory's c4dos.dir, where that directory
#             has one, and takes the recipe out of it: C4DOS opens what its
#             listings name
#   index     puts each in DISK/index.json, the page's lazy disk (lazy.js),
#             as [NAME, SIZE, HASH]: the hash (the first 16 hex digits of
#             its SHA-256) is what the page keeps it under once made
# A file of the name the disk does have is left as it is. A listing is
# written anew, not through a hard link (the ladder's are links).
import json, os, sys

what, disk = sys.argv[1], sys.argv[2]
RECIPE = '.unpack'

def recipes():
    out = []
    for d, _, files in os.walk(disk):
        for f in files:
            if f.endswith(RECIPE) and len(f) > len(RECIPE):
                rel = os.path.relpath(os.path.join(d, f), disk).replace(os.sep, '/')
                name = rel[:-len(RECIPE)]
                if not os.path.exists(os.path.join(disk, name)):
                    out.append((name, json.load(open(os.path.join(d, f)))))
    return sorted(out, key=lambda r: r[0])

def write(path, data):
    tmp = path + '.tmp'
    with open(tmp, 'w', newline='\n') as f:
        f.write(data)
    os.replace(tmp, path)

if what == 'sizes':
    for name, r in recipes():
        print(f"{r['size']} {name}")
elif what == 'listings':
    for name, r in recipes():
        d, base = os.path.split(name)
        listing = os.path.join(disk, d, 'c4dos.dir')
        if not os.path.exists(listing):
            continue
        lines = [l for l in open(listing).read().split('\n') if l and l != base + RECIPE]
        if base.lower() not in (l.lower() for l in lines):
            lines.append(base)
        write(listing, ''.join(l + '\n' for l in lines))
elif what == 'index':
    path = os.path.join(disk, 'index.json')
    rs = recipes()
    if rs and os.path.exists(path):
        index = json.load(open(path))
        made = {name: [name, r['size'], r['sha256'][:16]] for name, r in rs}
        index = [e for e in index if e[0] not in made] + list(made.values())
        write(path, json.dumps(index, separators=(',', ':')) + '\n')
else:
    sys.exit(f'unpacked.py: {what}? (sizes, listings or index)')
