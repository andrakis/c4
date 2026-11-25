#include "boot.h"
#include "mem.h"

enum { LOAD_CHUNK = 65536 };

int load_kernel(char *path) {
    int fd, n, total;

    fd = open(path, 0);
    if (fd < 0) {
        printf("load_kernel: could not open %s\n", path);
        return -1;
    }
    total = 0;
    while (1) {
        if (total + LOAD_CHUNK > RAM_SIZE) {
            printf("load_kernel: %s too large for RAM_SIZE\n", path);
            close(fd);
            return -1;
        }
        n = read(fd, ram + total, LOAD_CHUNK);
        if (n <= 0) break;
        total = total + n;
    }
    close(fd);
    return total;
}

void patch_kernel(int length, int memorysize_mb) {
    int i, sz, patched;

    patched = 0;
    for (i = 0; i < length; ++i) {
        if (ram[i] == 0x6d) // 'm'
        if (ram[i + 1] == 0x65) // 'e'
        if (ram[i + 2] == 0x6d) // 'm'
        if (ram[i + 3] == 0x6f) // 'o'
        if (ram[i + 4] == 0x72) // 'r'
        if (ram[i + 5] == 0x79) // 'y'
        if (ram[i + 6] == 0x00)
        if ((ram[i + 24] & 0xFF) == 0x01)
        if ((ram[i + 25] & 0xFF) == 0xF0)
        if ((ram[i + 26] & 0xFF) == 0x00)
        if ((ram[i + 27] & 0xFF) == 0x00) {
            sz = memorysize_mb * 0x100000;
            ram[i + 24] = (sz >> 24) & 0xFF;
            ram[i + 25] = (sz >> 16) & 0xFF;
            ram[i + 26] = 0x00;
            ram[i + 27] = 0x00;
            patched = patched + 1;
        }
    }
    printf("patch_kernel: patched %d memory-size cell(s) to %d MB\n", patched, memorysize_mb);
}
