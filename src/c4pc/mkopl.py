#!/usr/bin/env python3
# mkopl.py OUT.c libjs/opl-tables.js -- the OPL's tables (opl.c) as C, from
# libjs's opl-tables.js, which dbopl's own C++ made (InitTables,
# WAVE_TABLEMUL): one source for both machines, so that no host's maths
# library changes a sample.
import re, sys

out, src = sys.argv[1], sys.argv[2]
text = open(src).read()
c = ['/* made by src/c4pc/mkopl.py from libjs/opl-tables.js: do not edit */',
     '#include <stdint.h>']
for name, ctype in (('WAVE_TABLE', 'int16_t'), ('MUL_TABLE', 'uint16_t')):
    m = re.search(r'export const %s = new \w+\(\[(.*?)\]\);' % name, text, re.S)
    if not m:
        sys.exit('mkopl.py: no %s in %s' % (name, src))
    vals = [v.strip() for v in m.group(1).split(',') if v.strip()]
    c.append('const %s OPL_%s[%d] = {' % (ctype, name, len(vals)))
    for i in range(0, len(vals), 16):
        c.append('  ' + ', '.join(vals[i:i + 16]) + ',')
    c.append('};')
open(out, 'w').write('\n'.join(c) + '\n')
