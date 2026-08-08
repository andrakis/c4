#include "mmio.h"
#include "uart.h"

int mmio_read8(int addr) {
    int top;
    top = (addr >> 24) & 0xFF;
    if (top == ((UART_MMIO_BASE >> 24) & 0xFF)) return uart_read8(addr & 0xFFFFFF);
    printf("mmio_read8: no device at top byte 0x%x (addr=0x%x)\n", top, addr);
    return 0;
}

void mmio_write8(int addr, int val) {
    int top;
    top = (addr >> 24) & 0xFF;
    if (top == ((UART_MMIO_BASE >> 24) & 0xFF)) { uart_write8(addr & 0xFFFFFF, val); return; }
    printf("mmio_write8: no device at top byte 0x%x (addr=0x%x)\n", top, addr);
}
