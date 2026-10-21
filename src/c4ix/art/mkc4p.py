#!/usr/bin/env python3
# mkc4p.py IN.png OUT.c4p.gz [W H] -- an image for C4IX's splash
# (src/c4ix/user/splash.c): scaled to W x H (800 x 600), reduced to 256
# colours with dithering, as the boot screens of the nineties were, and
# written as "C4P8", width, height, colour count (16-bit little-endian),
# the RGB palette, a byte per pixel, gzipped. Needs Pillow.
#
# The output is committed beside the PNG (build-images.sh copies it to
# the disk), so building the disk needs no Pillow; run this again when
# the PNG changes. gzip's mtime is 0, so the bytes are reproducible.
import gzip, struct, sys
from PIL import Image

src, dst = sys.argv[1], sys.argv[2]
w, h = (int(sys.argv[3]), int(sys.argv[4])) if len(sys.argv) > 4 else (800, 600)
im = Image.open(src).convert("RGB").resize((w, h), Image.LANCZOS)
q = im.quantize(colors=256, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.FLOYDSTEINBERG)
pal = q.getpalette()[:256 * 3]
n = len(pal) // 3
body = b"C4P8" + struct.pack("<HHH", w, h, n) + bytes(pal) + q.tobytes()
with open(dst, "wb") as f:
    f.write(gzip.compress(body, compresslevel=9, mtime=0))
print("%s: %dx%d, %d colours, %d bytes raw" % (dst, w, h, n, len(body)))
