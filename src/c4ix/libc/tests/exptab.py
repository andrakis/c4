#!/usr/bin/env python3
# math.c's exp table: 2^(j/32) for j = 0..31 as double pairs HI + LO (HI
# the nearest double, LO the nearest to the rest), and ln2/32 in two parts
# (HI with its low 20 bits zero, so k * HI is exact for |k| < 2^20).
from decimal import Decimal, getcontext
import struct
getcontext().prec = 80

def bits(x): return struct.unpack("<Q", struct.pack("<d", x))[0]
def dbl(d): return float(d)          # Decimal -> nearest double

ln2 = Decimal(2).ln()
words = []
for j in range(32):
    v = Decimal(2) ** (Decimal(j) / 32)
    hi = dbl(v)
    lo = dbl(v - Decimal(hi))
    words += [bits(hi), bits(lo)]
for i in range(0, 64, 4):
    print("    " + ", ".join("0x%016Xull" % w for w in words[i:i + 4]) + ",")
c = ln2 / 32
hi = bits(dbl(c)) & ~((1 << 20) - 1)
hid = struct.unpack("<d", struct.pack("<Q", hi))[0]
lo = dbl(c - Decimal(hid))
print("ln2/32 hi 0x%016Xull lo 0x%016Xull" % (hi, bits(lo)))
print("32/ln2 0x%016Xull" % bits(dbl(32 / ln2)))
