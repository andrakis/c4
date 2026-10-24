#!/usr/bin/env python3
# c4r-body.py -- the CRC-32 and the length of a C4R image up to the end of
# its S segment, as "%08x %d", which is what C4IX's crc32 prints of a file.
# That is the image without its extension trailer (docs/c4-explorer.md
# 2.1): every byte a loader reads.
#
# The images build-images.sh makes carry source maps (Makefile, IMG_G).
# What C4IX compiles inside itself does not, until its cc has -g (track
# S, S3). So the tests that hold a build inside C4IX to the host's image
# compare it with this.
#
#   python3 -I libjs/tests/c4r-body.py FILE
import sys
import zlib

d = open(sys.argv[1], 'rb').read()
w = d[4] // 8


def word(at):
    return int.from_bytes(d[at:at + w], 'little', signed=True)


# signature, version, word size, MEMSZ; then entry and six lengths
code, data, patches, syms, cons, des = (word(13 + w * i) for i in range(1, 7))
# each segment after its marker word: C, D, P, c, d, then S
at = 13 + 7 * w + w + code * w + w + data + w + patches * 3 * w + w + cons * w + w + des * w + w
for _ in range(syms):
    # id, type, class, attributes; the name's length and the name; value
    at += 4 * w + 1 + d[at + 4 * w] + w
body = d[:at]
print('%08x %d' % (zlib.crc32(body) & 0xffffffff, len(body)))
