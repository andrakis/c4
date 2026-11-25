#!/usr/bin/env python3
# Measure a host-gcc Busybox build for docs/busybox-port.md, section 2.1.
#
#   python3 src/tests/bbport/bb-measure.py BUILD_DIR SRC_DIR
#
# BUILD_DIR is a `make O=...` output with busybox_unstripped.map in it, and
# SRC_DIR the unpacked busybox-1.37.0. It prints:
#   - the units the link pulled in, their text size and their source lines;
#   - every symbol those units import that none of them defines, which is what
#     /lib/libc.c4s must provide, because c4rlink links whole units;
#   - how often each construct the probes cover appears in those units.
import os, re, subprocess, sys
B, SRC = sys.argv[1], sys.argv[2]
LIBC_H = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'c4ix', 'libc', 'include', 'libc.h')
mp = open(os.path.join(B, 'busybox_unstripped.map')).read()
objs = sorted(set(d + '/' + o for d, o in re.findall(r'([a-z_0-9/]+)/lib\.a\(([^)]+\.o)\)', mp)))

text = lines = 0
rows = []
for o in objs:
    t = int(subprocess.run(['size', os.path.join(B, o)], capture_output=True, text=True).stdout.split('\n')[1].split()[0])
    c = os.path.join(SRC, o[:-2] + '.c')
    n = sum(1 for _ in open(c, errors='replace')) if os.path.exists(c) else 0
    text += t; lines += n; rows.append((t, n, o))
print('units linked: %d, text %d bytes (whole units), %d source lines' % (len(objs), text, lines))
for t, n, o in sorted(rows, reverse=True)[:10]:
    print('  %6d %6d %s' % (t, n, o))

defined, undef = set(), set()
for o in objs:
    for l in subprocess.run(['nm', os.path.join(B, o)], capture_output=True, text=True).stdout.splitlines():
        f = l.split()
        if len(f) == 2 and f[0] == 'U': undef.add(f[1])
        elif len(f) == 3 and f[1] in 'TtDdBbRrCVWw': defined.add(f[2])
ext = sorted(undef - defined)
h = open(LIBC_H).read()
have = set(re.findall(r'\b([a-z_][a-z_0-9]*)\s*[(;,]', h))
print('\nimported: %d symbols' % len(ext))
print('not in libc.h: ' + ' '.join(s for s in ext if s not in have))

def strip(s):
    s = re.sub(r'/\*.*?\*/', ' ', s, flags=re.S)
    s = re.sub(r'//[^\n]*', ' ', s)
    return re.sub(r'"(\\.|[^"\\\n])*"', '""', s)
C = {}
for o in objs:
    c = os.path.join(SRC, o[:-2] + '.c')
    if os.path.exists(c): C[os.path.relpath(c, SRC)] = strip(open(c, errors='replace').read())
pats = [
  ('goto', r'\bgoto\s+\w+\s*;'),
  ('(void) parameters', r'\w\s*\(\s*void\s*\)\s*(\{|;|FAST_FUNC|$)'),
  ('defined(X)', r'defined\s*\('),
  ('__VA_ARGS__', r'__VA_ARGS__|\w+\.\.\.\s*\)'),
  ('static local', r'\n[ \t]+static\s+(const\s+)?(unsigned\s+)?\w+[\s*]+\w+\s*(\[|=|;)'),
  ('function-pointer declarator', r'\(\s*\*\s*\w*\s*\)\s*\('),
  ('sizeof(*p), sizeof x', r'sizeof\s*\(\s*\*|sizeof\s+[a-zA-Z_]'),
  ('struct assignment', r'\*\s*\w+\s*=\s*\*\s*\w+\s*;'),
  ('2-D array', r'\w+\s*\[[^\]\n]*\]\s*\[[^\]\n]*\]\s*(=|;)'),
  ('[i] = designator', r'[{,]\s*\[\s*\w+\s*\]\s*='),
  ('.x = designator', r'[{,]\s*\.\w+\s*='),
  ('bitfield', r'\b(unsigned|int|smallint|uint8_t)\s+\w+\s*:\s*\d+\s*;'),
  ('statement expression', r'\(\s*\{'),
  ('case range', r'case\s+[^:]*\.\.\.'),
  ('typeof', r'\b(__)?typeof(__)?\s*\('),
  ('compound literal', r'\(\s*(struct\s+\w+|\w+_t)\s*\)\s*\{'),
  ('for (int i', r'for\s*\(\s*(int|unsigned|char|size_t|smallint)\b'),
  ('inline', r'\binline\b|ALWAYS_INLINE'),
  ('typedef enum', r'typedef\s+enum'),
  ('alloca', r'\balloca\b'),
  ('setjmp/longjmp', r'\b(sig)?(setjmp|longjmp)\b'),
  ('long long', r'long\s+long'),
]
print('\n%-28s %5s  %s' % ('construct', 'uses', 'most in'))
for name, p in pats:
    r = re.compile(p)
    cu = {f: len(r.findall(s)) for f, s in C.items()}
    top = sorted(((n, f) for f, n in cu.items() if n), reverse=True)[:3]
    print('%-28s %5d  %s' % (name, sum(cu.values()), ', '.join('%s %d' % (f, n) for n, f in top)))
