#!/usr/bin/env python3
# packdisk.py IN.tar OUT.tar -- the boot disk with its files compressed, for
# the ISO (Makefile, c4pc.iso): each member gzipped alone (level 9) and
# named NAME.c4pcz, which c4pc's drive (drive.c) takes as NAME, inflated
# when first read (inflate.c). Left as they are: what is compressed
# already (.tgz, .gz, .zip, .png, .jpg, .ogg, .mp3), and what gzip does not
# shrink by a tenth. The sizes c4ix.sizes gives stay the files' own.
import sys, tarfile, zlib, io

KEEP = (".tgz", ".gz", ".zip", ".png", ".jpg", ".jpeg", ".ogg", ".mp3", ".c4pcz")
src = tarfile.open(sys.argv[1])
out = tarfile.open(sys.argv[2], "w", format=tarfile.USTAR_FORMAT)
raw_total = out_total = packed = 0
# a name the archive has twice (c4pc's own files laid over the images':
# Makefile, .c4pc/disk.tar) is the later one's, as on the drive: the
# earlier left out
members = src.getmembers()
last = {m.name.lstrip("./"): i for i, m in enumerate(members)}
for i, m in enumerate(members):
    if m.isfile() and last[m.name.lstrip("./")] != i:
        continue
    if not m.isfile():
        out.addfile(m)
        continue
    data = src.extractfile(m).read()
    raw_total += len(data)
    name = m.name
    if not name.lower().endswith(KEEP) and len(data) >= 256:
        c = zlib.compressobj(9, zlib.DEFLATED, 31)      # 31: a gzip member
        z = c.compress(data) + c.flush()
        if len(z) * 10 < len(data) * 9:
            data, name = z, name + ".c4pcz"
            packed += 1
    n = tarfile.TarInfo(name)
    n.size, n.mode, n.mtime, n.type = len(data), m.mode, m.mtime, tarfile.REGTYPE
    out.addfile(n, io.BytesIO(data))
    out_total += len(data)
out.close()
print("packdisk: %d of the files compressed: %.1f MB to %.1f MB" % (packed, raw_total / 1048576.0, out_total / 1048576.0))
