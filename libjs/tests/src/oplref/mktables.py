#!/usr/bin/env python3
# mktables.py TABLES.TXT OUT.JS -- libjs/opl-tables.js from `oplref -t`
# (dbopl's InitTables), so the tables are its C++'s to the bit
import sys
lines = open(sys.argv[1]).read().split("\n")
w = lines[0].split()[1:]
m = lines[1].split()[1:]
assert len(w) == 4096 and len(m) == 384

def rows(v, n=24):
    return "\n".join("  " + ", ".join(v[i:i + n]) + "," for i in range(0, len(v), n))

s = """// opl-tables.js - the tables dbopl makes with sin() and pow() (opl.js), as
// its own C++ made them (InitTables, DBOPL_WAVE == WAVE_TABLEMUL), so that
// no browser's Math library changes a sample. From the DOSBox Team's
// dbopl.cpp, GNU GPL v2 or later; libjs/tests/src/oplref/oplref.cpp -t
// prints them, and mktables.py beside it makes this file.

// the waveforms, 8 x 512 overlapping entries (WaveBaseTable says where)
export const WAVE_TABLE = new Int16Array([
""" + rows(w) + """
]);

// volume as a multiplier, 1.16 fixed point
export const MUL_TABLE = new Uint16Array([
""" + rows(m) + """
]);
"""
open(sys.argv[2], "w", newline="\n").write(s)
