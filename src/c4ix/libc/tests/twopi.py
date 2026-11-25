#!/usr/bin/env python3
# The bits of 2/pi as 32-bit words, for math.c's Payne-Hanek reduction.
def atan_inv(x, bits):
    one = 1 << bits
    total = term = one // x
    x2 = x * x
    n, sign = 1, -1
    while term:
        term //= x2
        total += sign * (term // (2 * n + 1))
        sign, n = -sign, n + 1
    return total

B = 1600
pi = 4 * (4 * atan_inv(5, B) - atan_inv(239, B))      # pi * 2^B, give or take a few units
v = (2 << (2 * B)) // pi                               # (2/pi) * 2^B
words = [(v >> (B - 32 * (k + 1))) & 0xFFFFFFFF for k in range(40)]
assert words[0] == 0xA2F9836E and words[1] == 0x4E441529
for i in range(0, 40, 6):
    print("    " + ", ".join("0x%08X" % w for w in words[i:i + 6]) + ",")
