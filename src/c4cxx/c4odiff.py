#!/usr/bin/env python3
# c4odiff.py A.c4o B.c4o: which functions' code, and what data, differ
# between two objects (the .c4r format, src/c4cc/README.md). For finding
# where c4cxx's output and its input compile apart.
import struct, sys

def read(path):
    d = open(path, 'rb').read()
    assert d[:3] == b'C4R', path
    bits = d[4]
    W = bits // 8
    fmt = '<i' if W == 4 else '<q'
    def word(o):
        return struct.unpack_from(fmt, d, o)[0]
    o = 5 + 8
    entry, codelen, datalen, patchlen, symlen, clen, dlen = [word(o + i * W) for i in range(7)]
    o += 7 * W
    o += W  # 'C'
    code = [word(o + i * W) for i in range(codelen)]
    o += codelen * W
    o += W  # 'D'
    data = d[o:o + datalen]
    o += datalen
    o += W  # 'P'
    patches = [tuple(word(o + (i * 3 + k) * W) for k in range(3)) for i in range(patchlen)]
    o += patchlen * 3 * W
    o += W  # 'c'
    o += clen * W
    o += W  # 'd'
    o += dlen * W
    o += W  # 'S'
    syms = []
    for i in range(symlen):
        sid, typ, cls, attr = [word(o + k * W) for k in range(4)]
        o += 4 * W
        n = d[o]; o += 1
        name = d[o:o + n].decode('latin-1'); o += n
        val = word(o); o += W
        syms.append((sid, typ, cls, attr, name, val))
    return dict(code=code, data=data, patches=patches, syms=syms)

def functions(img):
    # symbols whose value is a code offset: those that patches of type -1
    # point at are not known here, so take every symbol with a value inside
    # the code and a class the compiler gives functions (the most common
    # class among them), sorted by offset
    fs = sorted((s[5], s[4], s[2]) for s in img['syms'] if 0 <= s[5] < len(img['code']))
    return fs

a, b = read(sys.argv[1]), read(sys.argv[2])
print('code words %d %d, data bytes %d %d, patches %d %d, symbols %d %d' % (
    len(a['code']), len(b['code']), len(a['data']), len(b['data']),
    len(a['patches']), len(b['patches']), len(a['syms']), len(b['syms'])))
sa = {s[4]: s for s in a['syms']}
sb = {s[4]: s for s in b['syms']}
for n in sorted(set(sa) ^ set(sb)):
    print('only in', 'A' if n in sa else 'B', n)
for n in sorted(set(sa) & set(sb)):
    if sa[n][1:4] != sb[n][1:4]:
        print('symbol %s: type/class/attr %s vs %s' % (n, sa[n][1:4], sb[n][1:4]))
# per function: code between its offset and the next symbol's
def slices(img):
    offs = sorted(set(s[5] for s in img['syms'] if s[2] != 0 and 0 <= s[5] <= len(img['code'])))
    out = {}
    for s in img['syms']:
        v = s[5]
        if v in offs:
            i = offs.index(v)
            end = offs[i + 1] if i + 1 < len(offs) else len(img['code'])
            out[s[4]] = img['code'][v:end]
    return out
ca, cb = slices(a), slices(b)
for n in sorted(set(ca) & set(cb)):
    if ca[n] != cb[n] and len(ca[n]) < 100000:
        print('code differs: %s (%d words vs %d)' % (n, len(ca[n]), len(cb[n])))
if a['data'] != b['data']:
    for i in range(min(len(a['data']), len(b['data']))):
        if a['data'][i] != b['data'][i]:
            print('data differs first at byte %d' % i)
            break
