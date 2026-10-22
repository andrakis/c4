// oplref.cpp -- DOSBox's dbopl (dosbox/, GNU GPL v2 or later, as Wolf4SDL
// carries it), the reference for libjs/opl.js: dump its tables (-t), or
// render a deterministic random register stream (RATE SAMPLES SEED OPL3)
// to stdout as 32-bit samples. libjs/tests/src/opl-stream.mjs makes the
// same stream with opl.js, and test-libjs requires the same bytes.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dosbox/dbopl.cpp"
using namespace DBOPL;
static uint32_t rs;
static uint32_t rnd() { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }
int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "-t")) {
        InitTables();
        printf("WAVE"); for (int i = 0; i < 8 * 512; i++) printf(" %d", WaveTable[i]); printf("\n");
        printf("MUL"); for (int i = 0; i < 384; i++) printf(" %d", MulTable[i]); printf("\n");
        return 0;
    }
    int rate = atoi(argv[1]), total = atoi(argv[2]); rs = atoi(argv[3]);
    int opl3 = argc > 4 && atoi(argv[4]);
    Chip *c = new Chip(); c->Setup(rate);
    if (opl3) c->WriteReg(0x105, 1);
    c->WriteReg(1, 0x20);
    static Bit32s buf[512 * 2];
    int done = 0;
    while (done < total) {
        // a burst of writes: mostly sensible, some anything
        int n = rnd() % 12;
        for (int i = 0; i < n; i++) {
            uint32_t r = rnd(), reg, val = (r >> 8) & 0xff;
            switch (r % 8) {
                case 0: reg = 0x20 + rnd() % 0x16; break;
                case 1: reg = 0x40 + rnd() % 0x16; val &= 0x3f; break;
                case 2: reg = 0x60 + rnd() % 0x16; break;
                case 3: reg = 0x80 + rnd() % 0x16; break;
                case 4: reg = 0xa0 + rnd() % 9; break;
                case 5: reg = 0xb0 + rnd() % 9; break;
                case 6: reg = 0xc0 + rnd() % 9; break;
                default: reg = (rnd() & 1) ? 0xbd : 0xe0 + rnd() % 0x16; break;
            }
            if (opl3 && (rnd() & 1)) reg |= 0x100;
            c->WriteReg(reg, val);
        }
        int len = 1 + rnd() % 300;
        if (len > total - done) len = total - done;
        if (opl3) c->GenerateBlock3(len, buf); else c->GenerateBlock2(len, buf);
        fwrite(buf, 4, opl3 ? len * 2 : len, stdout);
        done += len;
    }
    return 0;
}
